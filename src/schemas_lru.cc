/*
 * Copyright (c) 2015-2019 Dubalu LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "schemas_lru.h"

#include "cassert.h"                              // for ASSERT
#include "database/handler.h"                     // for DatabaseHandler
#include "database/utils.h"                       // for unsharded_path
#include "log.h"                                  // for L_CALL
#include "manager.h"                              // for XapiandManager::resolve_index_endpoints
#include "opts.h"                                 // for opts.strict
#include "reserved/schema.h"                      // for RESERVED_RECURSE, RESERVED_ENDPOINT, ...
#include "serialise.h"                            // for KEYWORD_STR
#include "string.hh"                              // for string::format, string::replace
#include "url_parser.h"                           // for urldecode

#define L_SCHEMA L_NOTHING


// #undef L_DEBUG
// #define L_DEBUG L_GREY
// #undef L_CALL
// #define L_CALL L_STACKED_DIM_GREY
// #undef L_SCHEMA
// #define L_SCHEMA L_YELLOW_GREEN


static const std::string reserved_schema(RESERVED_SCHEMA);


template <typename ErrorType>
static inline std::pair<const MsgPack*, const MsgPack*>
validate_schema(const MsgPack& object, const char* prefix, std::string& foreign_uri, std::string& foreign_path, std::string& foreign_id)
{
	L_CALL("validate_schema({})", repr(object.to_string()));

	auto checked = Schema::check<ErrorType>(object, prefix, true, true);
	if (checked.first) {
		foreign_uri = checked.first->str();
		std::string_view foreign_path_view, foreign_id_view;
		split_path_id(foreign_uri, foreign_path_view, foreign_id_view);
		if (foreign_path_view.empty() || foreign_id_view.empty()) {
			THROW(ErrorType, "{}'{}' must contain index and docid [{}]", prefix, RESERVED_ENDPOINT, repr(foreign_uri));
		}
		foreign_path = urldecode(foreign_path_view);
		foreign_id = urldecode(foreign_id_view);
	}
	return checked;
}


static inline MsgPack
get_shared(const Endpoint& endpoint, std::string_view id, std::shared_ptr<std::unordered_set<std::string>> context)
{
	L_CALL("get_shared({}, {}, {})", repr(endpoint.to_string()), repr(id), context ? std::to_string(context->size()) : "nullptr");

	auto path = endpoint.path;
	if (!context) {
		context = std::make_shared<std::unordered_set<std::string>>();
	}
	if (context->size() > MAX_SCHEMA_RECURSION) {
		THROW(ClientError, "Maximum recursion reached: {}", endpoint.to_string());
	}
	if (!context->insert(path).second) {
		if (path == ".xapiand/index") {
			// Return default .xapiand/index (chicken and egg problem)
			return {
				{ RESERVED_RECURSE, false },
				{ SCHEMA_FIELD_NAME, {
					{ ID_FIELD_NAME, {
						{ RESERVED_STORE, false },
						{ RESERVED_TYPE,  KEYWORD_STR },
					} },
				} },
			};
		}
		THROW(ClientError, "Cyclic schema reference detected: {}", endpoint.to_string());
	}
	try {
		auto endpoints = XapiandManager::resolve_index_endpoints(endpoint, true);
		if (endpoints.empty()) {
			THROW(ClientError, "Cannot resolve endpoint: {}", endpoint.to_string());
		}
		DatabaseHandler _db_handler(endpoints, DB_OPEN, HTTP_GET, context);
		std::string_view selector;
		auto needle = id.find_first_of(".{", 1);  // Find first of either '.' (Drill Selector) or '{' (Field selector)
		if (needle != std::string_view::npos) {
			selector = id.substr(id[needle] == '.' ? needle + 1 : needle);
			id = id.substr(0, needle);
		}
		auto doc = _db_handler.get_document(id);
		auto o = doc.get_obj();
		if (!selector.empty()) {
			o = o.select(selector);
		}
		auto it = o.find(SCHEMA_FIELD_NAME);  // If there's a "schema" field inside, extract it
		if (it != o.end()) {
			o = it.value();
		}
		o = MsgPack({
			{ RESERVED_RECURSE, false },
			{ SCHEMA_FIELD_NAME, o },
		});
		Schema::check<Error>(o, "Foreign schema is invalid: ", false, false);
		context->erase(path);
		return o;
	} catch (...) {
		context->erase(path);
		throw;
	}
}


static inline void
save_shared(const Endpoint& endpoint, std::string_view id, MsgPack schema, std::shared_ptr<std::unordered_set<std::string>> context)
{
	L_CALL("save_shared({}, {}, <schema>, {})", repr(endpoint.to_string()), repr(id), context ? std::to_string(context->size()) : "nullptr");

	auto path = endpoint.path;
	if (!context) {
		context = std::make_shared<std::unordered_set<std::string>>();
	}
	if (context->size() > MAX_SCHEMA_RECURSION) {
		THROW(ClientError, "Maximum recursion reached: {}", endpoint.to_string());
	}
	if (!context->insert(path).second) {
		if (path == ".xapiand/index") {
			// Ignore .xapiand/index (chicken and egg problem)
			return;
		}
		THROW(ClientError, "Cyclic schema reference detected: {}", endpoint.to_string());
	}
	try {
		auto endpoints = XapiandManager::resolve_index_endpoints(endpoint, true);
		if (endpoints.empty()) {
			THROW(ClientError, "Cannot resolve endpoint: {}", endpoint.to_string());
		}
		DatabaseHandler _db_handler(endpoints, DB_WRITABLE | DB_CREATE_OR_OPEN, HTTP_PUT, context);
		auto needle = id.find_first_of(".{", 1);  // Find first of either '.' (Drill Selector) or '{' (Field selector)
		// FIXME: Process the subfields instead of ignoring.
		_db_handler.update(id.substr(0, needle), 0, false, schema, true, false, msgpack_type);
		context->erase(path);
	} catch (...) {
		context->erase(path);
		throw;
	}
}


SchemasLRU::SchemasLRU(ssize_t max_size) :
	local_schemas(max_size),
	foreign_schemas(max_size)
{
}


std::tuple<bool, std::shared_ptr<const MsgPack>, std::string>
SchemasLRU::_update(const char* prefix, DatabaseHandler* db_handler, const std::shared_ptr<const MsgPack>& new_schema, const MsgPack* schema_obj)
{
	L_CALL("SchemasLRU::_update(<db_handler>, {})", new_schema ? repr(new_schema->to_string()) : "nullptr", schema_obj ? repr(schema_obj->to_string()) : "nullptr");

	ASSERT(db_handler);
	ASSERT(!db_handler->endpoints.empty());

	std::string foreign_uri, foreign_path, foreign_id;
	std::shared_ptr<const MsgPack> schema_ptr;

	bool exchanged;
	bool failure = false;

	// We first try to load schema from the LRU cache
	std::shared_ptr<const MsgPack> local_schema_ptr;
	const auto local_schema_path = std::string(unsharded_path(db_handler->endpoints[0].path));  // FIXME: This should remain a string_view, but LRU's std::unordered_map cannot find std::string_view directly!
	{
		std::lock_guard<std::mutex> lk(smtx);
		local_schema_ptr = local_schemas[local_schema_path].load();
	}

	if (new_schema) {
		// Now we check if the schema points to a foreign schema
		validate_schema<Error>(*new_schema, "Schema metadata is corrupt: ", foreign_uri, foreign_path, foreign_id);
	} else if (schema_obj) {
		// Check if passed object specifies a foreign schema
		validate_schema<Error>(*schema_obj, "Schema metadata is corrupt: ", foreign_uri, foreign_path, foreign_id);
	}

	// Whatever was passed by the user doesn't specify a foreign schema,
	// or there it wasn't passed anything.
	if (local_schema_ptr) {
		// Schema was in the cache
		L_SCHEMA("{}Schema {} found in cache", prefix, repr(local_schema_path));
		schema_ptr = local_schema_ptr;
		if (!foreign_uri.empty()) {
			auto tmp_schema_ptr = std::make_shared<MsgPack>(MsgPack({
				{ RESERVED_TYPE, "foreign/object" },
				{ RESERVED_ENDPOINT, foreign_uri },
			}));
			if (*tmp_schema_ptr != *schema_ptr) {
				tmp_schema_ptr->lock();
				{
					std::lock_guard<std::mutex> lk(smtx);
					exchanged = local_schemas[local_schema_path].compare_exchange_strong(schema_ptr, tmp_schema_ptr);
				}
				if (exchanged) {
					L_SCHEMA("{}Foreign Schema Link {} added to LRU", prefix, repr(local_schema_path));
				} else {
					failure = true;
				}
			}
		}
	} else {
		// Schema needs to be read
		L_SCHEMA("{}Schema {} not found in cache, try loading from metadata", prefix, repr(local_schema_path));
		std::string schema_ser;
		try {
			schema_ser = db_handler->get_metadata(reserved_schema);
		} catch (const Xapian::DocNotFoundError&) {
		} catch (const Xapian::DatabaseNotFoundError&) {
		} catch (...) {
			L_EXC("Exception");
		}
		if (schema_ser.empty()) {
			if (!foreign_uri.empty()) {
				schema_ptr = std::make_shared<MsgPack>(MsgPack({
					{ RESERVED_TYPE, "foreign/object" },
					{ RESERVED_ENDPOINT, foreign_uri },
				}));
				schema_ptr->lock();
			} else if (local_schema_path != ".xapiand") {
				// Implement foreign schemas in .xapiand/index by default:
				schema_ptr = std::make_shared<MsgPack>(MsgPack({
					{ RESERVED_TYPE, "foreign/object" },
					{ RESERVED_ENDPOINT, string::format(".xapiand/index/{}", string::replace(local_schema_path, "/", "%2F")) },
				}));
				schema_ptr->lock();
			} else if (new_schema) {
				schema_ptr = new_schema;
			} else {
				schema_ptr = Schema::get_initial_schema();
			}
		} else {
			schema_ptr = std::make_shared<const MsgPack>(MsgPack::unserialise(schema_ser));
			schema_ptr->lock();
			schema_ptr->set_flags(1);
		}
		{
			std::lock_guard<std::mutex> lk(smtx);
			exchanged = local_schemas[local_schema_path].compare_exchange_strong(local_schema_ptr, schema_ptr);
		}
		if (exchanged) {
			L_SCHEMA("{}Local Schema {} added to LRU", prefix, repr(local_schema_path));
		} else {
			// Read object couldn't be stored in cache,
			// so we use the schema now currently in cache
			schema_ptr = local_schema_ptr;
			failure = true;
		}
	}

	// If we still need to save the metadata, we save it:
	if (schema_ptr->get_flags() == 0 && (db_handler->flags & DB_WRITABLE) == DB_WRITABLE) {
		L_SCHEMA("{}Cached Local Schema {}, write schema metadata", prefix, repr(local_schema_path));
		try {
			// Try writing (only if there's no metadata there alrady)
			if (db_handler->set_metadata(reserved_schema, schema_ptr->serialise(), false)) {
				schema_ptr->set_flags(1);
			} else {
				L_SCHEMA("{}Metadata for Cached Schema {} wasn't overwriten, try reloading from metadata", prefix, repr(local_schema_path));
				std::string schema_ser;
				try {
					schema_ser = db_handler->get_metadata(reserved_schema);
				} catch (const Xapian::DocNotFoundError&) {
				} catch (const Xapian::DatabaseNotFoundError&) {
				} catch (...) {
					L_EXC("Exception");
				}
				if (schema_ser.empty()) {
					THROW(Error, "Cannot set metadata: {}", repr(reserved_schema));
				}
				schema_ptr = std::make_shared<const MsgPack>(MsgPack::unserialise(schema_ser));
				schema_ptr->lock();
				schema_ptr->set_flags(1);
				{
					std::lock_guard<std::mutex> lk(smtx);
					exchanged = local_schemas[local_schema_path].compare_exchange_strong(local_schema_ptr, schema_ptr);
				}
				if (exchanged) {
					L_SCHEMA("{}Cached Schema {} re-added to LRU", prefix, repr(local_schema_path));
				} else {
					schema_ptr = local_schema_ptr;
				}
				failure = true;
			}
		} catch (...) {
			if (local_schema_ptr != schema_ptr) {
				L_SCHEMA("{}Metadata for Schema {} wasn't set, try reverting LRU", prefix, repr(local_schema_path));
				// On error, try reverting
				std::lock_guard<std::mutex> lk(smtx);
				local_schemas[local_schema_path].compare_exchange_strong(schema_ptr, local_schema_ptr);
			}
			throw;
		}
	}

	if (new_schema && !foreign_uri.empty()) {
		return std::make_tuple(failure, std::move(schema_ptr), std::move(foreign_uri));
	}

	// Now we check if the schema points to a foreign schema
	validate_schema<Error>(*schema_ptr, "Schema metadata is corrupt: ", foreign_uri, foreign_path, foreign_id);
	if (!foreign_uri.empty()) {
		// FOREIGN Schema, get from the cache or load from `foreign_path/foreign_id` endpoint:
		std::shared_ptr<const MsgPack> foreign_schema_ptr;
		{
			std::lock_guard<std::mutex> lk(smtx);
			foreign_schema_ptr = foreign_schemas[foreign_uri].load();
		}
		if (foreign_schema_ptr && (!new_schema || *foreign_schema_ptr == *new_schema)) {
			// Same Foreign Schema was in the cache
			L_SCHEMA("{}Foreign Schema {} found in cache", prefix, repr(foreign_uri));
			schema_ptr = foreign_schema_ptr;
		} else if (new_schema) {
			schema_ptr = new_schema;
			{
				std::lock_guard<std::mutex> lk(smtx);
				exchanged = foreign_schemas[foreign_uri].compare_exchange_strong(foreign_schema_ptr, schema_ptr);
			}
			if (exchanged) {
				L_SCHEMA("{}New Foreign Schema {} added to LRU", prefix, repr(foreign_uri));
			} else {
				schema_ptr = foreign_schema_ptr;
				failure = true;
			}
		} else {
			// Foreign Schema needs to be read
			L_SCHEMA("{}Foreign Schema {} not found in cache, try loading from {} {}", prefix, repr(foreign_uri), repr(foreign_path), repr(foreign_id));
			try {
				schema_ptr = std::make_shared<const MsgPack>(get_shared(Endpoint{foreign_path}, foreign_id, db_handler->context));
				schema_ptr->lock();
				schema_ptr->set_flags(1);
			} catch (const ClientError&) {
				throw;
			} catch (const Error&) {
				schema_ptr = Schema::get_initial_schema();
			} catch (const Xapian::DocNotFoundError&) {
				schema_ptr = Schema::get_initial_schema();
			} catch (const Xapian::DatabaseNotFoundError&) {
				schema_ptr = Schema::get_initial_schema();
			} catch (...) {
				L_EXC("Exception");
				schema_ptr = Schema::get_initial_schema();
			}
			{
				std::lock_guard<std::mutex> lk(smtx);
				exchanged = foreign_schemas[foreign_uri].compare_exchange_strong(foreign_schema_ptr, schema_ptr);
			}
			if (exchanged) {
				L_SCHEMA("{}Foreign Schema {} added to LRU", prefix, repr(foreign_uri));
			} else {
				schema_ptr = foreign_schema_ptr;
				failure = true;
			}
		}
		// If we still need to save the schema document, we save it:
		if (schema_ptr->get_flags() == 0 && (db_handler->flags & DB_WRITABLE) == DB_WRITABLE) {
			L_SCHEMA("{}Cached Foreign Schema {}, write schema", prefix, repr(foreign_uri));
			try {
				save_shared(Endpoint{foreign_path}, foreign_id, *schema_ptr, db_handler->context);
				schema_ptr->set_flags(1);
			} catch (const Xapian::DocVersionConflictError&) {
				// Foreign Schema needs to be read
				L_SCHEMA("{}Foreign Schema {} not found in cache, try loading from {} {}", prefix, repr(foreign_uri), repr(foreign_path), repr(foreign_id));
				try {
					schema_ptr = std::make_shared<const MsgPack>(get_shared(Endpoint{foreign_path}, foreign_id, db_handler->context));
					schema_ptr->lock();
					schema_ptr->set_flags(1);
				} catch (const ClientError&) {
					throw;
				} catch (const Error&) {
					schema_ptr = Schema::get_initial_schema();
				} catch (const Xapian::DocNotFoundError&) {
					schema_ptr = Schema::get_initial_schema();
				} catch (const Xapian::DatabaseNotFoundError&) {
					schema_ptr = Schema::get_initial_schema();
				} catch (...) {
					L_EXC("Exception");
					schema_ptr = Schema::get_initial_schema();
				}
				{
					std::lock_guard<std::mutex> lk(smtx);
					exchanged = foreign_schemas[foreign_uri].compare_exchange_strong(foreign_schema_ptr, schema_ptr);
				}
				if (exchanged) {
					L_SCHEMA("{}Foreign Schema {} added to LRU", prefix, repr(foreign_uri));
				} else {
					schema_ptr = foreign_schema_ptr;
				}
				failure = true;
			} catch (...) {
				if (foreign_schema_ptr != schema_ptr) {
					L_SCHEMA("{}Foreign Schema {} wasn't saved, try reverting LRU", prefix, repr(foreign_uri));
					// On error, try reverting
					std::lock_guard<std::mutex> lk(smtx);
					foreign_schemas[foreign_uri].compare_exchange_strong(schema_ptr, foreign_schema_ptr);
				}
				throw;
			}
		}
	}

	return std::make_tuple(failure, std::move(schema_ptr), std::move(foreign_uri));
}


std::tuple<std::shared_ptr<const MsgPack>, std::unique_ptr<MsgPack>, std::string>
SchemasLRU::get(DatabaseHandler* db_handler, const MsgPack* obj)
{
	/**
	 * Returns schema, mut_schema and foreign_uri
	 */
	L_CALL("SchemasLRU::get(<db_handler>, {})", obj ? repr(obj->to_string()) : "nullptr");

	const MsgPack* schema_obj = nullptr;
	if (obj && obj->is_map()) {
		const auto it = obj->find(reserved_schema);
		if (it != obj->end()) {
			schema_obj = &it.value();
		}
	}

	auto up = _update("GET: ", db_handler, nullptr, schema_obj);
	auto schema_ptr = std::get<1>(up);
	auto foreign_uri = std::get<2>(up);

	if (schema_obj && schema_obj->is_map()) {
		MsgPack o = *schema_obj;
		// Initialize schema (non-foreign, non-recursive, ensure there's "schema"):
		o.erase(RESERVED_ENDPOINT);
		auto it = o.find(RESERVED_TYPE);
		if (it != o.end()) {
			auto &type = it.value();
			auto sep_types = required_spc_t::get_types(type.str_view());
			sep_types[SPC_FOREIGN_TYPE] = FieldType::EMPTY;
			type = required_spc_t::get_str_type(sep_types);
		}
		o[RESERVED_RECURSE] = false;
		if (opts.strict && o.find(ID_FIELD_NAME) == o.end()) {
			THROW(MissingTypeError, "Type of field '{}' for the foreign schema is missing", ID_FIELD_NAME);
		}
		if (o.find(SCHEMA_FIELD_NAME) == o.end()) {
			o[SCHEMA_FIELD_NAME] = MsgPack::MAP();
		}
		Schema schema(schema_ptr, nullptr, "");
		schema.update(o);
		std::unique_ptr<MsgPack> mut_schema;
		schema.swap(mut_schema);
		if (mut_schema) {
			return std::make_tuple(std::move(schema_ptr), std::move(mut_schema), std::move(foreign_uri));
		}
	}

	return std::make_tuple(std::move(schema_ptr), nullptr, std::move(foreign_uri));
}


bool
SchemasLRU::set(DatabaseHandler* db_handler, std::shared_ptr<const MsgPack>& old_schema, const std::shared_ptr<const MsgPack>& new_schema)
{
	L_CALL("SchemasLRU::set(<db_handler>, <old_schema>, {})", new_schema ? repr(new_schema->to_string()) : "nullptr");

	auto up = _update("SET: ", db_handler, new_schema, nullptr);
	auto failure = std::get<0>(up);
	auto schema_ptr = std::get<1>(up);

	if (failure) {
		old_schema = schema_ptr;
		return false;
	}
	return true;
}


bool
SchemasLRU::drop(DatabaseHandler* db_handler, std::shared_ptr<const MsgPack>& old_schema)
{
	L_CALL("SchemasLRU::delete(<db_handler>, <old_schema>)");

	ASSERT(db_handler);
	ASSERT(!db_handler->endpoints.empty());

	bool exchanged;
	std::string foreign_uri, foreign_path, foreign_id;
	std::shared_ptr<const MsgPack> schema_ptr;

	const auto local_schema_path = std::string(unsharded_path(db_handler->endpoints[0].path));  // FIXME: This should remain a string_view, but LRU's std::unordered_map cannot find std::string_view directly!
	std::shared_ptr<const MsgPack> local_schema_ptr;
	{
		std::lock_guard<std::mutex> lk(smtx);
		local_schema_ptr = local_schemas[local_schema_path].load();
	}
	if (old_schema != local_schema_ptr) {
		validate_schema<Error>(*local_schema_ptr, "Schema metadata is corrupt: ", foreign_uri, foreign_path, foreign_id);
		if (foreign_uri.empty()) {
			// it faield, but metadata continues to be local
			old_schema = local_schema_ptr;
			return false;
		}
		std::shared_ptr<const MsgPack> foreign_schema_ptr;
		{
			std::lock_guard<std::mutex> lk(smtx);
			foreign_schema_ptr = foreign_schemas[foreign_uri].load();
		}
		if (old_schema != foreign_schema_ptr) {
			old_schema = foreign_schema_ptr;
			return false;
		}
	}

	std::shared_ptr<const MsgPack> new_schema = nullptr;
	if (local_schema_ptr == new_schema) {
		return true;
	}
	{
		std::lock_guard<std::mutex> lk(smtx);
		exchanged = local_schemas[local_schema_path].compare_exchange_strong(local_schema_ptr, new_schema);
	}
	if (exchanged) {
		try {
			db_handler->set_metadata(reserved_schema, "");
		} catch (...) {
			// On error, try reverting
			std::lock_guard<std::mutex> lk(smtx);
			local_schemas[local_schema_path].compare_exchange_strong(new_schema, local_schema_ptr);
			throw;
		}
		return true;
	}

	validate_schema<Error>(*local_schema_ptr, "Schema metadata is corrupt: ", foreign_uri, foreign_path, foreign_id);
	if (foreign_uri.empty()) {
		// it faield, but metadata continues to be local
		old_schema = local_schema_ptr;
		return false;
	}

	std::shared_ptr<const MsgPack> foreign_schema_ptr;
	{
		std::lock_guard<std::mutex> lk(smtx);
		foreign_schema_ptr = foreign_schemas[foreign_uri].load();
	}

	old_schema = foreign_schema_ptr;
	return false;
}
