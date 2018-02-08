/*
 * Copyright (C) 2015-2018 dubalu.com LLC. All rights reserved.
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


template <typename ErrorType>
inline std::pair<const MsgPack*, const MsgPack*>
SchemasLRU::validate_schema(const MsgPack& object, const char* prefix, std::string& foreign_path, std::string& foreign_id)
{
	L_CALL("SchemasLRU::validate_schema(%s)", repr(object.to_string()).c_str());

	auto checked = Schema::check<ErrorType>(object, prefix, true, true, true);
	if (checked.first) {
		const auto aux_schema_str = checked.first->str();
		split_path_id(aux_schema_str, foreign_path, foreign_id);
		if (foreign_path.empty() || foreign_id.empty()) {
			THROW(ErrorType, "%s'%s' must contain index and docid [%s]", prefix, RESERVED_ENDPOINT, aux_schema_str.c_str());
		}
	}
	return checked;
}


MsgPack
SchemasLRU::get_shared(const Endpoint& endpoint, const std::string& id, std::shared_ptr<std::unordered_set<size_t>> context)
{
	L_CALL("SchemasLRU::get_shared(%s, %s, %s)", repr(endpoint.to_string()).c_str(), id.c_str(), context ? std::to_string(context->size()).c_str() : "nullptr");

	auto hash = endpoint.hash();
	if (!context) {
		context = std::make_shared<std::unordered_set<size_t>>();
	}

	try {
		if (context->size() > MAX_SCHEMA_RECURSION) {
			THROW(Error, "Maximum recursion reached: %s", endpoint.to_string().c_str());
		}
		if (!context->insert(hash).second) {
			THROW(Error, "Cyclic schema reference detected: %s", endpoint.to_string().c_str());
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
	L_CALL("SchemasLRU::get(<db_handler>, %s)", obj ? repr(obj->to_string()).c_str() : "nullptr");

	std::string foreign_path, foreign_id;
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
		const auto it = obj->find(RESERVED_SCHEMA);
		if (it != obj->end()) {
			schema_obj = &it.value();
			validate_schema<Error>(*schema_obj, "Schema metadata is corrupt: ", foreign_path, foreign_id);
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
			auto str_schema = db_handler->get_metadata(RESERVED_SCHEMA);
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
					THROW(ForeignSchemaError, "Schema of %s must use a foreign schema", repr(db_handler->endpoints.to_string()).c_str());
				}
				try {
					// Try writing (only if there's no metadata there alrady)
					if (!db_handler->set_metadata(RESERVED_SCHEMA, schema_ptr->serialise(), false)) {
						// or fallback to load from metadata (again).
						local_schema_ptr = schema_ptr;
						str_schema = db_handler->get_metadata(RESERVED_SCHEMA);
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
			{ RESERVED_ENDPOINT, foreign_path + "/" + foreign_id },
		}));
		schema_ptr->lock();
		if (atom_local_schema->compare_exchange_strong(local_schema_ptr, schema_ptr)) {
			if (write) {
				try {
					if (!db_handler->set_metadata(RESERVED_SCHEMA, schema_ptr->serialise(), false)) {
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

	std::string foreign;

	// Try validating loaded/created schema as LOCAL or FOREIGN
	validate_schema<Error>(*schema_ptr, "Schema metadata is corrupt: ", foreign_path, foreign_id);
	if (!foreign_path.empty()) {
		// FOREIGN Schema, get from the cache or use `get_shared()`
		// to load from `foreign_path/foreign_id` endpoint:
		foreign = foreign_path + "/" + foreign_id;
		const auto foreign_schema_hash = std::hash<std::string>{}(foreign);
		atomic_shared_ptr<const MsgPack>* atom_shared_schema;
		{
			std::lock_guard<std::mutex> lk(smtx);
			atom_shared_schema = &(*this)[foreign_schema_hash];
		}
		auto shared_schema_ptr = atom_shared_schema->load();
		if (shared_schema_ptr) {
			// found in cache
			schema_ptr = shared_schema_ptr;
		} else {
			try {
				schema_ptr = std::make_shared<const MsgPack>(get_shared(foreign_path, foreign_id, db_handler->context));
				if (schema_ptr->empty()) {
					schema_ptr = Schema::get_initial_schema();
				} else {
					schema_ptr->lock();
				}
				if (!schema_ptr->is_map()) {
					THROW(Error, "Schema of %s must be map [%s]", repr(db_handler->endpoints.to_string()).c_str(), repr(schema_ptr->to_string()).c_str());
				}
			} catch (const ForeignSchemaError&) {
				schema_ptr = Schema::get_initial_schema();
			} catch (const CheckoutError&) {
				schema_ptr = Schema::get_initial_schema();
			} catch (const DocNotFoundError&) {
				schema_ptr = Schema::get_initial_schema();
			}
			if (!atom_shared_schema->compare_exchange_strong(shared_schema_ptr, schema_ptr)) {
				schema_ptr = shared_schema_ptr;
			}
		}
	}

	if (schema_obj && schema_obj->is_map()) {
		MsgPack o = *schema_obj;
		o[RESERVED_TYPE] = "object";
		o.erase(RESERVED_ENDPOINT);
		if (o.find(SCHEMA_FIELD_NAME) == o.end()) {
			o[SCHEMA_FIELD_NAME] = MsgPack(MsgPack::Type::MAP);
		}
		Schema schema(schema_ptr, nullptr, "");
		schema.update(o);
		std::unique_ptr<MsgPack> mut_schema;
		schema.swap(mut_schema);
		if (mut_schema) {
			return std::make_tuple(std::move(schema_ptr), std::move(mut_schema), foreign);
		}
	}

	return std::make_tuple(std::move(schema_ptr), nullptr, foreign);
}


bool
SchemasLRU::set(DatabaseHandler* db_handler, std::shared_ptr<const MsgPack>& old_schema, const std::shared_ptr<const MsgPack>& new_schema)
{
	L_CALL("SchemasLRU::set(<db_handler>, <old_schema>, %s)", new_schema ? repr(new_schema->to_string()).c_str() : "nullptr");

	bool failure = false;
	std::string foreign_path, foreign_id;
	std::shared_ptr<const MsgPack> schema_ptr;
	bool new_metadata = false;

	const auto local_schema_hash = db_handler->endpoints.hash();

	atomic_shared_ptr<const MsgPack>* atom_local_schema;
	{
		std::lock_guard<std::mutex> lk(smtx);
		atom_local_schema = &(*this)[local_schema_hash];
	}
	auto local_schema_ptr = atom_local_schema->load();

	validate_schema<Error>(*new_schema, "Schema metadata is corrupt: ", foreign_path, foreign_id);
	if (foreign_path.empty()) {
		// LOCAL new schema.
		if (local_schema_ptr) {
			// found in cache
			schema_ptr = local_schema_ptr;
		} else {
			auto str_schema = db_handler->get_metadata(RESERVED_SCHEMA);
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

			if (new_metadata) {
				try {
					if (!db_handler->set_metadata(RESERVED_SCHEMA, schema_ptr->serialise(), false)) {
						str_schema = db_handler->get_metadata(RESERVED_SCHEMA);
						if (str_schema.empty()) {
							THROW(Error, "Cannot set metadata: '%s'", RESERVED_SCHEMA);
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

		validate_schema<Error>(*schema_ptr, "Schema metadata is corrupt: ", foreign_path, foreign_id);
		if (foreign_path.empty()) {
			// LOCAL new schema *and* LOCAL metadata schema.
			if (schema_ptr == new_schema || atom_local_schema->compare_exchange_strong(schema_ptr, new_schema)) {
				if (*schema_ptr != *new_schema) {
					try {
						db_handler->set_metadata(RESERVED_SCHEMA, new_schema->serialise());
					} catch(...) {
						// On error, try reverting
						std::shared_ptr<const MsgPack> aux_new_schema(new_schema);
						atom_local_schema->compare_exchange_strong(aux_new_schema, schema_ptr);
						throw;
					}
				}
				return true;
			}

			validate_schema<Error>(*schema_ptr, "Schema metadata is corrupt: ", foreign_path, foreign_id);
			if (foreign_path.empty()) {
				// it faield, but metadata continues to be local
				old_schema = schema_ptr;
				return false;
			}
			failure = true;
		}
	} else {
		// FOREIGN new schema, write the foreign link to metadata:
		if (atom_local_schema->compare_exchange_strong(local_schema_ptr, new_schema)) {
			if (*local_schema_ptr != *new_schema) {
				try {
					db_handler->set_metadata(RESERVED_SCHEMA, new_schema->serialise());
				} catch(...) {
					// On error, try reverting
					std::shared_ptr<const MsgPack> aux_new_schema(new_schema);
					atom_local_schema->compare_exchange_strong(aux_new_schema, local_schema_ptr);
					throw;
				}
			}
			return true;
		}

		validate_schema<Error>(*local_schema_ptr, "Schema metadata is corrupt: ", foreign_path, foreign_id);
		if (foreign_path.empty()) {
			// it faield, but metadata continues to be local
			old_schema = local_schema_ptr;
			return false;
		}
		failure = true;
	}

	// FOREIGN Schema, get from the cache or use `get_shared()`
	// to load from `foreign_path/foreign_id` endpoint:
	const auto foreign_schema_hash = std::hash<std::string>{}(foreign_path + "/" + foreign_id);
	atomic_shared_ptr<const MsgPack>* atom_shared_schema;
	{
		std::lock_guard<std::mutex> lk(smtx);
		atom_shared_schema = &(*this)[foreign_schema_hash];
	}
	auto shared_schema_ptr = atom_shared_schema->load();

	if (!failure && atom_shared_schema->compare_exchange_strong(shared_schema_ptr, new_schema)) {
		if (*shared_schema_ptr != *new_schema) {
			try {
				DatabaseHandler _db_handler(Endpoints(Endpoint(foreign_path)), DB_WRITABLE | DB_SPAWN | DB_NOWAL, HTTP_PUT, db_handler->context);
				if (_db_handler.get_metadata(RESERVED_SCHEMA).empty()) {
					_db_handler.set_metadata(RESERVED_SCHEMA, Schema::get_initial_schema()->serialise());
				}
				// FIXME: Process the foreign_path's subfields instead of ignoring.
				auto needle = foreign_id.find_first_of("|{", 1);  // to get selector, find first of either | or {
				_db_handler.index(foreign_id.substr(0, needle), true, *new_schema, false, msgpack_type);
			} catch(...) {
				// On error, try reverting
				std::shared_ptr<const MsgPack> aux_new_schema(new_schema);
				atom_shared_schema->compare_exchange_strong(aux_new_schema, shared_schema_ptr);
				throw;
			}
		}
		return true;
	}

	old_schema = shared_schema_ptr;
	return false;
}
