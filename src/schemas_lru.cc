/*
 * Copyright (C) 2015-2018 Dubalu LLC. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "schemas_lru.h"

#include "database_handler.h"
#include "log.h"
#include "opts.h"


static const std::string reserved_schema(RESERVED_SCHEMA);
static const std::hash<std::string_view> hasher;


template <typename ErrorType>
inline std::pair<const MsgPack*, const MsgPack*>
SchemasLRU::validate_schema(const MsgPack& object, const char* prefix, std::string_view& foreign, std::string_view& foreign_path, std::string_view& foreign_id)
{
	L_CALL("SchemasLRU::validate_schema(%s)", repr(object.to_string()));

	auto checked = Schema::check<ErrorType>(object, prefix, true, true, true);
	if (checked.first) {
		foreign = checked.first->str_view();
		split_path_id(foreign, foreign_path, foreign_id);
		if (foreign_path.empty() || foreign_id.empty()) {
			THROW(ErrorType, "%s'%s' must contain index and docid [%s]", prefix, RESERVED_ENDPOINT, repr(foreign));
		}
	}
	return checked;
}


MsgPack
SchemasLRU::get_shared(const Endpoint& endpoint, std::string_view id, std::shared_ptr<std::unordered_set<size_t>> context)
{
	L_CALL("SchemasLRU::get_shared(%s, %s, %s)", repr(endpoint.to_string()), repr(id), context ? std::to_string(context->size()) : "nullptr");

	auto hash = endpoint.hash();
	if (!context) {
		context = std::make_shared<std::unordered_set<size_t>>();
	}

	try {
		if (context->size() > MAX_SCHEMA_RECURSION) {
			THROW(Error, "Maximum recursion reached: %s", endpoint.to_string());
		}
		if (!context->insert(hash).second) {
			THROW(Error, "Cyclic schema reference detected: %s", endpoint.to_string());
		}
		DatabaseHandler _db_handler(Endpoints(endpoint), DB_OPEN | DB_NOWAL, HTTP_GET, context);
		// FIXME: Process the subfields instead of ignoring.
		auto needle = id.find_first_of("|{", 1);  // to get selector, find first of either | or {
		auto doc = _db_handler.get_document(id.substr(0, needle));
		context->erase(hash);
		return doc.get_obj();
	} catch (...) {
		context->erase(hash);
		throw;
	}
}


std::tuple<std::shared_ptr<const MsgPack>, std::unique_ptr<MsgPack>, std::string>
SchemasLRU::get(DatabaseHandler* db_handler, const MsgPack* obj, bool write)
{
	L_CALL("SchemasLRU::get(<db_handler>, %s)", obj ? repr(obj->to_string()) : "nullptr");

	std::string_view foreign, foreign_path, foreign_id;
	std::shared_ptr<const MsgPack> schema_ptr;

	const MsgPack* schema_obj = nullptr;

	const auto local_schema_hash = db_handler->endpoints.hash();
	atomic_shared_ptr<const MsgPack>* atom_local_schema;
	{
		std::lock_guard<std::mutex> lk(smtx);
		atom_local_schema = &(*this)[local_schema_hash];
	}
	auto local_schema_ptr = atom_local_schema->load();

	if (obj && obj->is_map()) {
		const auto it = obj->find(reserved_schema);
		if (it != obj->end()) {
			schema_obj = &it.value();
			validate_schema<Error>(*schema_obj, "Schema metadata is corrupt: ", foreign, foreign_path, foreign_id);
		}
	}

	if (foreign_path.empty()) {
		// Foreign schema not passed by the user in '_schema', load schema instead.
		if (local_schema_ptr) {
			// Schema found in cache.
			schema_ptr = local_schema_ptr;
		} else {
			// Schema not found in cache, try loading from metadata.
			bool new_metadata = false;
			auto str_schema = db_handler->get_metadata(reserved_schema);
			if (str_schema.empty()) {
				new_metadata = true;
				schema_ptr = Schema::get_initial_schema();
			} else {
				schema_ptr = std::make_shared<const MsgPack>(MsgPack::unserialise(str_schema));
				schema_ptr->lock();
			}

			if (!atom_local_schema->compare_exchange_strong(local_schema_ptr, schema_ptr)) {
				schema_ptr = local_schema_ptr;
			}

			if (new_metadata && write) {
				// New LOCAL schema:
				if (opts.foreign) {
					THROW(ForeignSchemaError, "Schema of %s must use a foreign schema", repr(db_handler->endpoints.to_string()));
				}
				try {
					// Try writing (only if there's no metadata there alrady)
					if (!db_handler->set_metadata(reserved_schema, schema_ptr->serialise(), false)) {
						// or fallback to load from metadata (again).
						local_schema_ptr = schema_ptr;
						str_schema = db_handler->get_metadata(reserved_schema);
						if (str_schema.empty()) {
							schema_ptr = Schema::get_initial_schema();
						} else {
							schema_ptr = std::make_shared<const MsgPack>(MsgPack::unserialise(str_schema));
							schema_ptr->lock();
						}
						if (!atom_local_schema->compare_exchange_strong(local_schema_ptr, schema_ptr)) {
							schema_ptr = local_schema_ptr;
						}
					}
				} catch(...) {
					if (local_schema_ptr != schema_ptr) {
						// On error, try reverting
						atom_local_schema->compare_exchange_strong(schema_ptr, local_schema_ptr);
					}
					throw;
				}
			}
		}
	} else {
		// New FOREIGN schema, write the foreign link to metadata:
		schema_ptr = std::make_shared<MsgPack>(MsgPack({
			{ RESERVED_TYPE, "foreign/object" },
			{ RESERVED_ENDPOINT, foreign },
		}));
		schema_ptr->lock();
		if (atom_local_schema->compare_exchange_strong(local_schema_ptr, schema_ptr)) {
			if (write) {
				try {
					if (!db_handler->set_metadata(reserved_schema, schema_ptr->serialise(), false)) {
						// It doesn't matter if new metadata cannot be set
						// it should continue with newly created foreign
						// schema, as requested by user.
					}
				} catch(...) {
					// On error, try reverting
					atom_local_schema->compare_exchange_strong(schema_ptr, local_schema_ptr);
					throw;
				}
			}
		}
	}

	// Try validating loaded/created schema as LOCAL or FOREIGN
	validate_schema<Error>(*schema_ptr, "Schema metadata is corrupt: ", foreign, foreign_path, foreign_id);
	if (!foreign_path.empty()) {
		// FOREIGN Schema, get from the cache or use `get_shared()`
		// to load from `foreign_path/foreign_id` endpoint:
		const auto foreign_schema_hash = hasher(foreign);
		atomic_shared_ptr<const MsgPack>* atom_shared_schema;
		{
			std::lock_guard<std::mutex> lk(smtx);
			atom_shared_schema = &(*this)[foreign_schema_hash];
		}
		auto foreign_schema_ptr = atom_shared_schema->load();
		if (foreign_schema_ptr) {
			// found in cache
			schema_ptr = foreign_schema_ptr;
		} else {
			try {
				schema_ptr = std::make_shared<const MsgPack>(get_shared(foreign_path, foreign_id, db_handler->context));
				if (schema_ptr->empty()) {
					schema_ptr = Schema::get_initial_schema();
				} else {
					schema_ptr->lock();
				}
				if (!schema_ptr->is_map()) {
					THROW(Error, "Schema of %s must be map [%s]", repr(db_handler->endpoints.to_string()), repr(schema_ptr->to_string()));
				}
			} catch (const ForeignSchemaError&) {
				schema_ptr = Schema::get_initial_schema();
			} catch (const CheckoutError&) {
				schema_ptr = Schema::get_initial_schema();
			} catch (const DocNotFoundError&) {
				schema_ptr = Schema::get_initial_schema();
			}
			if (!atom_shared_schema->compare_exchange_strong(foreign_schema_ptr, schema_ptr)) {
				schema_ptr = foreign_schema_ptr;
			}
		}
	}

	if (schema_obj && schema_obj->is_map()) {
		MsgPack o = *schema_obj;
		o.erase(RESERVED_ENDPOINT);
		auto sep_types = required_spc_t::get_types(o[RESERVED_TYPE].str_view());
		sep_types[SPC_FOREIGN_TYPE] = FieldType::EMPTY;
		o[RESERVED_TYPE] = required_spc_t::get_str_type(sep_types);
		o[RESERVED_RECURSE] = false;
		if (o.find(ID_FIELD_NAME) == o.end()) {
			o[VERSION_FIELD_NAME] = DB_VERSION_SCHEMA;
		}
		if (opts.strict && o.find(ID_FIELD_NAME) == o.end()) {
			THROW(MissingTypeError, "Type of field '%s' for the foreign schema is missing", ID_FIELD_NAME);
		}
		if (o.find(SCHEMA_FIELD_NAME) == o.end()) {
			o[SCHEMA_FIELD_NAME] = MsgPack(MsgPack::Type::MAP);
		}
		Schema schema(schema_ptr, nullptr, "");
		schema.update(o);
		std::unique_ptr<MsgPack> mut_schema;
		schema.swap(mut_schema);
		if (mut_schema) {
			return std::make_tuple(std::move(schema_ptr), std::move(mut_schema), std::string(foreign));
		}
	}

	return std::make_tuple(std::move(schema_ptr), nullptr, std::string(foreign));
}


bool
SchemasLRU::set(DatabaseHandler* db_handler, std::shared_ptr<const MsgPack>& old_schema, const std::shared_ptr<const MsgPack>& new_schema)
{
	L_CALL("SchemasLRU::set(<db_handler>, <old_schema>, %s)", new_schema ? repr(new_schema->to_string()) : "nullptr");

	bool failure = false;
	std::string_view foreign, foreign_path, foreign_id;
	std::shared_ptr<const MsgPack> schema_ptr;
	bool new_metadata = false;

	const auto local_schema_hash = db_handler->endpoints.hash();
	atomic_shared_ptr<const MsgPack>* atom_local_schema;
	{
		std::lock_guard<std::mutex> lk(smtx);
		atom_local_schema = &(*this)[local_schema_hash];
	}
	auto local_schema_ptr = atom_local_schema->load();

	validate_schema<Error>(*new_schema, "Schema metadata is corrupt: ", foreign, foreign_path, foreign_id);
	if (foreign_path.empty()) {
		// LOCAL new schema.
		if (local_schema_ptr) {
			// found in cache
			schema_ptr = local_schema_ptr;
		} else {
			auto str_schema = db_handler->get_metadata(reserved_schema);
			if (str_schema.empty()) {
				new_metadata = true;
				schema_ptr = new_schema;
			} else {
				schema_ptr = std::make_shared<const MsgPack>(MsgPack::unserialise(str_schema));
				schema_ptr->lock();
			}

			if (!atom_local_schema->compare_exchange_strong(local_schema_ptr, schema_ptr)) {
				schema_ptr = local_schema_ptr;
			}

			old_schema = schema_ptr;  // renew old_schema since lru didn't already have it

			if (new_metadata) {
				try {
					if (!db_handler->set_metadata(reserved_schema, schema_ptr->serialise(), false)) {
						str_schema = db_handler->get_metadata(reserved_schema);
						if (str_schema.empty()) {
							THROW(Error, "Cannot set metadata: %s", repr(reserved_schema));
						}
						new_metadata = false;
						local_schema_ptr = schema_ptr;
						schema_ptr = std::make_shared<const MsgPack>(MsgPack::unserialise(str_schema));
						schema_ptr->lock();
						if (!atom_local_schema->compare_exchange_strong(local_schema_ptr, schema_ptr)) {
							schema_ptr = local_schema_ptr;
						}
					}
				} catch(...) {
					if (local_schema_ptr != schema_ptr) {
						// On error, try reverting
						atom_local_schema->compare_exchange_strong(schema_ptr, local_schema_ptr);
					}
					throw;
				}
			}
		}

		validate_schema<Error>(*schema_ptr, "Schema metadata is corrupt: ", foreign, foreign_path, foreign_id);
		if (foreign_path.empty()) {
			// LOCAL new schema *and* LOCAL metadata schema.
			if (old_schema != schema_ptr) {
				old_schema = schema_ptr;
				return false;
			}

			if (schema_ptr == new_schema) {
				return true;
			} else if (atom_local_schema->compare_exchange_strong(schema_ptr, new_schema)) {
				if (*schema_ptr != *new_schema) {
					try {
						db_handler->set_metadata(reserved_schema, new_schema->serialise());
					} catch(...) {
						// On error, try reverting
						std::shared_ptr<const MsgPack> aux_new_schema(new_schema);
						atom_local_schema->compare_exchange_strong(aux_new_schema, schema_ptr);
						throw;
					}
				}
				return true;
			}

			validate_schema<Error>(*schema_ptr, "Schema metadata is corrupt: ", foreign, foreign_path, foreign_id);
			if (foreign_path.empty()) {
				// it faield, but metadata continues to be local
				old_schema = schema_ptr;
				return false;
			}

			failure = true;
		}
	} else {
		// FOREIGN new schema, write the foreign link to metadata:
		if (old_schema != local_schema_ptr) {
			const auto foreign_schema_hash = hasher(foreign);
			atomic_shared_ptr<const MsgPack>* atom_shared_schema;
			{
				std::lock_guard<std::mutex> lk(smtx);
				atom_shared_schema = &(*this)[foreign_schema_hash];
			}
			auto foreign_schema_ptr = atom_shared_schema->load();
			if (old_schema != foreign_schema_ptr) {
				old_schema = foreign_schema_ptr;
				return false;
			}
		}

		if (atom_local_schema->compare_exchange_strong(local_schema_ptr, new_schema)) {
			if (*local_schema_ptr != *new_schema) {
				try {
					db_handler->set_metadata(reserved_schema, new_schema->serialise());
				} catch(...) {
					// On error, try reverting
					std::shared_ptr<const MsgPack> aux_new_schema(new_schema);
					atom_local_schema->compare_exchange_strong(aux_new_schema, local_schema_ptr);
					throw;
				}
			}
			return true;
		}

		validate_schema<Error>(*local_schema_ptr, "Schema metadata is corrupt: ", foreign, foreign_path, foreign_id);
		if (foreign_path.empty()) {
			// it faield, but metadata continues to be local
			old_schema = local_schema_ptr;
			return false;
		}

		failure = true;
	}

	// FOREIGN Schema, get from the cache or use `get_shared()`
	// to load from `foreign_path/foreign_id` endpoint:
	const auto foreign_schema_hash = hasher(foreign);
	atomic_shared_ptr<const MsgPack>* atom_shared_schema;
	{
		std::lock_guard<std::mutex> lk(smtx);
		atom_shared_schema = &(*this)[foreign_schema_hash];
	}
	auto foreign_schema_ptr = atom_shared_schema->load();
	if (old_schema != foreign_schema_ptr) {
		old_schema = foreign_schema_ptr;
		return false;
	}

	if (!failure) {
		if (foreign_schema_ptr == new_schema) {
			return true;
		} else if (atom_shared_schema->compare_exchange_strong(foreign_schema_ptr, new_schema)) {
			if (*foreign_schema_ptr != *new_schema) {
				try {
					DatabaseHandler _db_handler(Endpoints(Endpoint(foreign_path)), DB_WRITABLE | DB_SPAWN | DB_NOWAL, HTTP_PUT, db_handler->context);
					if (_db_handler.get_metadata(reserved_schema).empty()) {
						_db_handler.set_metadata(reserved_schema, Schema::get_initial_schema()->serialise());
					}
					// FIXME: Process the foreign_path's subfields instead of ignoring.
					auto needle = foreign_id.find_first_of("|{", 1);  // to get selector, find first of either | or {
					_db_handler.index(foreign_id.substr(0, needle), true, *new_schema, false, msgpack_type);
				} catch(...) {
					// On error, try reverting
					std::shared_ptr<const MsgPack> aux_new_schema(new_schema);
					atom_shared_schema->compare_exchange_strong(aux_new_schema, foreign_schema_ptr);
					throw;
				}
			}
			return true;
		}
	}

	old_schema = foreign_schema_ptr;
	return false;
}


bool
SchemasLRU::drop(DatabaseHandler* db_handler, std::shared_ptr<const MsgPack>& old_schema)
{
	L_CALL("SchemasLRU::delete(<db_handler>, <old_schema>)");

	std::string_view foreign, foreign_path, foreign_id;
	std::shared_ptr<const MsgPack> schema_ptr;

	const auto local_schema_hash = db_handler->endpoints.hash();
	atomic_shared_ptr<const MsgPack>* atom_local_schema;
	{
		std::lock_guard<std::mutex> lk(smtx);
		atom_local_schema = &(*this)[local_schema_hash];
	}
	auto local_schema_ptr = atom_local_schema->load();
	if (old_schema != local_schema_ptr) {
		validate_schema<Error>(*local_schema_ptr, "Schema metadata is corrupt: ", foreign, foreign_path, foreign_id);
		if (foreign_path.empty()) {
			// it faield, but metadata continues to be local
			old_schema = local_schema_ptr;
			return false;
		}
		const auto foreign_schema_hash = hasher(foreign);
		atomic_shared_ptr<const MsgPack>* atom_shared_schema;
		{
			std::lock_guard<std::mutex> lk(smtx);
			atom_shared_schema = &(*this)[foreign_schema_hash];
		}
		auto foreign_schema_ptr = atom_shared_schema->load();
		if (old_schema != foreign_schema_ptr) {
			old_schema = foreign_schema_ptr;
			return false;
		}
	}

	std::shared_ptr<const MsgPack> new_schema = nullptr;
	if (local_schema_ptr == new_schema) {
		return true;
	} else if (atom_local_schema->compare_exchange_strong(local_schema_ptr, new_schema)) {
		try {
			db_handler->set_metadata(reserved_schema, "");
		} catch(...) {
			// On error, try reverting
			atom_local_schema->compare_exchange_strong(new_schema, local_schema_ptr);
			throw;
		}
		return true;
	}

	validate_schema<Error>(*local_schema_ptr, "Schema metadata is corrupt: ", foreign, foreign_path, foreign_id);
	if (foreign_path.empty()) {
		// it faield, but metadata continues to be local
		old_schema = local_schema_ptr;
		return false;
	}

	const auto foreign_schema_hash = hasher(foreign);
	atomic_shared_ptr<const MsgPack>* atom_shared_schema;
	{
		std::lock_guard<std::mutex> lk(smtx);
		atom_shared_schema = &(*this)[foreign_schema_hash];
	}
	auto foreign_schema_ptr = atom_shared_schema->load();

	old_schema = foreign_schema_ptr;
	return false;
}
