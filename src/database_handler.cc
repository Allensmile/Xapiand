/*
 * Copyright (C) 2015-2018 Dubalu LLC. All rights reserved.
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

#include "database_handler.h"

#include <algorithm>                        // for min, move
#include <cctype>                           // for isupper, tolower
#include <exception>                        // for exception
#include <stdexcept>                        // for out_of_range
#include <utility>

#include "cast.h"                           // for Cast
#include "database.h"                       // for DatabasePool, Database
#include "exception.h"                      // for CheckoutError, ClientError
#include "length.h"                         // for unserialise_length, seria...
#include "log.h"                            // for L_CALL
#include "msgpack.h"                        // for MsgPack, object::object, ...
#include "msgpack_patcher.h"                // for apply_patch
#include "multivalue/aggregation.h"         // for AggregationMatchSpy
#include "multivalue/keymaker.h"            // for Multi_MultiValueKeyMaker
#include "query_dsl.h"                      // for QUERYDSL_QUERY, QueryDSL
#include "rapidjson/document.h"             // for Document
#include "schema.h"                         // for Schema, required_spc_t
#include "schemas_lru.h"                    // for SchemasLRU
#include "script.h"                         // for Script
#include "serialise.h"                      // for cast, serialise, type
#include "utils.h"                          // for repr

#if defined(XAPIAND_V8)
#include "v8pp/v8pp.h"                      // for v8pp namespace
#endif
#if defined(XAPIAND_CHAISCRIPT)
#include "chaipp/chaipp.h"                  // for chaipp namespace
#endif


// Reserved words only used in the responses to the user.
constexpr const char RESPONSE_AV_LENGTH[]           = "#av_length";
constexpr const char RESPONSE_CONTENT_TYPE[]        = "#content_type";
constexpr const char RESPONSE_DOC_COUNT[]           = "#doc_count";
constexpr const char RESPONSE_DOC_DEL[]             = "#doc_del";
constexpr const char RESPONSE_DOC_LEN_LOWER[]       = "#doc_len_lower";
constexpr const char RESPONSE_DOC_LEN_UPPER[]       = "#doc_len_upper";
constexpr const char RESPONSE_HAS_POSITIONS[]       = "#has_positions";
constexpr const char RESPONSE_LAST_ID[]             = "#last_id";
constexpr const char RESPONSE_OBJECT[]              = "#object";
constexpr const char RESPONSE_OFFSET[]              = "#offset";
constexpr const char RESPONSE_POS[]                 = "#pos";
constexpr const char RESPONSE_SIZE[]                = "#size";
constexpr const char RESPONSE_TERM_FREQ[]           = "#term_freq";
constexpr const char RESPONSE_TYPE[]                = "#type";
constexpr const char RESPONSE_UUID[]                = "#uuid";
constexpr const char RESPONSE_VOLUME[]              = "#volume";
constexpr const char RESPONSE_WDF[]                 = "#wdf";
constexpr const char RESPONSE_DOCID[]               = "#docid";
constexpr const char RESPONSE_DATA[]                = "#data";
constexpr const char RESPONSE_TERMS[]               = "#terms";
constexpr const char RESPONSE_VALUES[]              = "#values";

constexpr size_t NON_STORED_SIZE_LIMIT = 1024 * 1024;

const std::string dump_metadata_header ("xapiand-dump-meta");
const std::string dump_schema_header("xapiand-dump-schm");
const std::string dump_documents_header("xapiand-dump-docs");


Xapian::docid
to_docid(std::string_view document_id)
{
	size_t sz = document_id.size();
	if (sz > 2 && document_id[0] == ':' && document_id[1] == ':') {
		std::string_view did_str(document_id.data() + 2, document_id.size() - 2);
		try {
			return static_cast<Xapian::docid>(strict_stol(did_str));
		} catch (const InvalidArgument& er) {
			THROW(ClientError, "Value %s cannot be cast to integer [%s]", repr(did_str), er.what());
		} catch (const OutOfRange& er) {
			THROW(ClientError, "Value %s cannot be cast to integer [%s]", repr(did_str), er.what());
		}
	}
	return static_cast<Xapian::docid>(0);
}


class FilterPrefixesExpandDecider : public Xapian::ExpandDecider {
	std::vector<std::string> prefixes;

public:
	FilterPrefixesExpandDecider(std::vector<std::string>  prefixes_)
		: prefixes(std::move(prefixes_)) { }

	bool operator() (const std::string& term) const override {
		for (const auto& prefix : prefixes) {
			if (string::startswith(term, prefix)) {
				return true;
			}
		}

		return prefixes.empty();
	}
};


template<typename F, typename... Args>
lock_database::lock_database(DatabaseHandler* db_handler_, F&& f, Args&&... args)
	: db_handler(db_handler_)
{
	lock(std::forward<F>(f), std::forward<Args>(args)...);
}


lock_database::lock_database(DatabaseHandler* db_handler_)
	: db_handler(db_handler_)
{
	lock();
}


lock_database::~lock_database()
{
	if (db_handler != nullptr) {
		if (db_handler->database) {
			unlock();
		}
	}
}


template<typename F, typename... Args>
void
lock_database::lock(F&& f, Args&&... args)
{
	L_CALL("lock_database::lock(...)");

	if (db_handler) {
		if (db_handler->database) {
			THROW(Error, "lock_database is already locked: %s", repr(db_handler->database->endpoints.to_string()));
		} else {
			XapiandManager::manager->database_pool.checkout(db_handler->database, db_handler->endpoints, db_handler->flags, std::forward<F>(f), std::forward<Args>(args)...);
		}
	}
}


void
lock_database::lock()
{
	L_CALL("lock_database::lock()");

	if (db_handler != nullptr) {
		if (db_handler->database) {
			THROW(Error, "lock_database is already locked: %s", repr(db_handler->database->endpoints.to_string()));
		} else {
			XapiandManager::manager->database_pool.checkout(db_handler->database, db_handler->endpoints, db_handler->flags);
		}
	}
}


void
lock_database::unlock()
{
	L_CALL("lock_database::unlock(...)");

	if (db_handler != nullptr) {
		if (db_handler->database) {
			XapiandManager::manager->database_pool.checkin(db_handler->database);
		} else {
			THROW(Error, "lock_database is not locked: %s", repr(db_handler->database->endpoints.to_string()));
		}
	}
}


DatabaseHandler::DatabaseHandler()
	: flags(0),
	  method(HTTP_GET) { }


DatabaseHandler::DatabaseHandler(Endpoints  endpoints_, int flags_, enum http_method method_, std::shared_ptr<std::unordered_set<size_t>>  context_)
	: endpoints(std::move(endpoints_)),
	  flags(flags_),
	  method(method_),
	  context(std::move(context_)) { }


std::shared_ptr<Database>
DatabaseHandler::get_database() const noexcept
{
	return database;
}


std::shared_ptr<Schema>
DatabaseHandler::get_schema(const MsgPack* obj)
{
	L_CALL("DatabaseHandler::get_schema(<obj>)");
	auto s = XapiandManager::manager->schemas.get(this, obj, (obj != nullptr) && ((flags & DB_WRITABLE) != 0));
	return std::make_shared<Schema>(std::move(std::get<0>(s)), std::move(std::get<1>(s)), std::move(std::get<2>(s)));
}


void
DatabaseHandler::recover_index()
{
	L_CALL("DatabaseHandler::recover_index()");

	XapiandManager::manager->database_pool.recover_database(endpoints, RECOVER_REMOVE_WRITABLE);
	reset(endpoints, flags, HTTP_PUT, context);
}


void
DatabaseHandler::reset(const Endpoints& endpoints_, int flags_, enum http_method method_, const std::shared_ptr<std::unordered_set<size_t>>& context_)
{
	L_CALL("DatabaseHandler::reset(%s, %x, <method>)", repr(endpoints_.to_string()), flags_);

	if (endpoints_.empty()) {
		THROW(ClientError, "It is expected at least one endpoint");
	}

	method = method_;

	if (endpoints != endpoints_ || flags != flags_) {
		endpoints = endpoints_;
		flags = flags_;
	}

	context = context_;
}


#if XAPIAND_DATABASE_WAL
MsgPack
DatabaseHandler::repr_wal(uint32_t start_revision, uint32_t end_revision)
{
	L_CALL("DatabaseHandler::repr_wal(%u, %u)", start_revision, end_revision);

	if (endpoints.size() != 1) {
		THROW(ClientError, "It is expected one single endpoint");
	}

	// WAL required on a local writable database, open it.
	lock_database lk_db(this);
	auto wal = std::make_unique<DatabaseWAL>(endpoints[0].path, database.get());
	return wal->repr(start_revision, end_revision);
}
#endif


Document
DatabaseHandler::get_document_term(const std::string& term_id)
{
	L_CALL("DatabaseHandler::get_document_term(%s)", repr(term_id));

	lock_database lk_db(this);
	auto did = database->find_document(term_id);
	return Document(this, database->get_document(did, (database->flags & DB_WRITABLE) != 0));
}


Document
DatabaseHandler::get_document_term(std::string_view term_id)
{
	return get_document_term(std::string(term_id));
}


#if defined(XAPIAND_V8) || defined(XAPIAND_CHAISCRIPT)
std::mutex DatabaseHandler::documents_mtx;
std::unordered_map<size_t, std::shared_ptr<std::pair<size_t, const MsgPack>>> DatabaseHandler::documents;


template<typename Processor>
MsgPack&
DatabaseHandler::call_script(MsgPack& data, std::string_view term_id, size_t script_hash, size_t body_hash, std::string_view script_body, std::shared_ptr<std::pair<size_t, const MsgPack>>& old_document_pair)
{
	try {
		auto processor = Processor::compile(script_hash, body_hash, std::string(script_body));
		switch (method) {
			case HTTP_PUT:
				old_document_pair = get_document_change_seq(term_id);
				if (old_document_pair) {
					L_INDEX("Script: on_put(%s, %s)", data.to_string(4), old_document_pair->second.to_string(4));
					data = (*processor)["on_put"](data, old_document_pair->second);
				} else {
					L_INDEX("Script: on_put(%s)", data.to_string(4));
					data = (*processor)["on_put"](data, MsgPack(MsgPack::Type::MAP));
				}
				break;

			case HTTP_PATCH:
			case HTTP_MERGE:
				old_document_pair = get_document_change_seq(term_id);
				if (old_document_pair) {
					L_INDEX("Script: on_patch(%s, %s)", data.to_string(4), old_document_pair->second.to_string(4));
					data = (*processor)["on_patch"](data, old_document_pair->second);
				} else {
					L_INDEX("Script: on_patch(%s)", data.to_string(4));
					data = (*processor)["on_patch"](data, MsgPack(MsgPack::Type::MAP));
				}
				break;

			case HTTP_DELETE:
				old_document_pair = get_document_change_seq(term_id);
				if (old_document_pair) {
					L_INDEX("Script: on_delete(%s, %s)", data.to_string(4), old_document_pair->second.to_string(4));
					data = (*processor)["on_delete"](data, old_document_pair->second);
				} else {
					L_INDEX("Script: on_delete(%s)", data.to_string(4));
					data = (*processor)["on_delete"](data, MsgPack(MsgPack::Type::MAP));
				}
				break;

			case HTTP_GET:
				L_INDEX("Script: on_get(%s)", data.to_string(4));
				data = (*processor)["on_get"](data);
				break;

			case HTTP_POST:
				L_INDEX("Script: on_post(%s)", data.to_string(4));
				data = (*processor)["on_post"](data);
				break;

			default:
				break;
		}
		return data;
#if defined(XAPIAND_V8)
	} catch (const v8pp::ReferenceError&) {
		return data;
	} catch (const v8pp::Error& e) {
		THROW(ClientError, e.what());
#endif
#if defined(XAPIAND_CHAISCRIPT)
	} catch (const chaipp::ReferenceError&) {
		return data;
	} catch (const chaipp::Error& e) {
		THROW(ClientError, e.what());
#endif
	}
}


MsgPack&
DatabaseHandler::run_script(MsgPack& data, std::string_view term_id, std::shared_ptr<std::pair<size_t, const MsgPack>>& old_document_pair, const MsgPack& data_script)
{
	L_CALL("DatabaseHandler::run_script(...)");

	if (data_script.is_map()) {
		const auto& type = data_script.at(RESERVED_TYPE);
		const auto& sep_type = required_spc_t::get_types(type.str_view());
		if (sep_type[SPC_FOREIGN_TYPE] == FieldType::FOREIGN) {
			THROW(ClientError, "Missing Implementation for Foreign scripts");
		} else {
			auto it_s = data_script.find(RESERVED_CHAI);
			if (it_s == data_script.end()) {
#if defined(XAPIAND_V8)
				const auto& ecma = data_script.at(RESERVED_ECMA);
				return call_script<v8pp::Processor>(data, term_id, ecma.at(RESERVED_HASH).u64(), ecma.at(RESERVED_BODY_HASH).u64(), ecma.at(RESERVED_BODY).str_view(), old_document_pair);
#else
				THROW(ClientError, "Script type 'ecma' (ECMAScript or JavaScript) not available.");
#endif
			} else {
#if defined(XAPIAND_CHAISCRIPT)
				const auto& chai = it_s.value();
				return call_script<chaipp::Processor>(data, term_id, chai.at(RESERVED_HASH).u64(), chai.at(RESERVED_BODY_HASH).u64(), chai.at(RESERVED_BODY).str_view(), old_document_pair);
#else
				THROW(ClientError, "Script type 'chai' (ChaiScript) not available.");
#endif
			}
		}
	}

	return data;
}
#endif


DataType
DatabaseHandler::index(std::string_view document_id, MsgPack& obj, Data& data, bool commit_)
{
	L_CALL("DatabaseHandler::index(%s, %s, <data>, %s)", repr(document_id), repr(obj.to_string()), commit_ ? "true" : "false");

	static UUIDGenerator generator;

	Xapian::Document doc;
	required_spc_t spc_id;
	std::string term_id;
	std::string prefixed_term_id;

	Xapian::docid did = 0;
	std::string doc_uuid;
	std::string doc_id;
	std::string doc_xid;
	if (document_id.empty()) {
		doc_uuid = Unserialise::uuid(generator(opts.uuid_compact).serialise(), static_cast<UUIDRepr>(opts.uuid_repr));
		// Add a new empty document to get its document ID:
		lock_database lk_db(this);
		try {
			did = database->add_document(Xapian::Document(), false, false);
		} catch (const Xapian::DatabaseError&) {
			// Try to recover from DatabaseError (i.e when the index is manually deleted)
			lk_db.unlock();
			recover_index();
			lk_db.lock();
			did = database->add_document(Xapian::Document(), false, false);
		}
		doc_id = std::to_string(did);
	} else {
		doc_xid = document_id;
	}

#if defined(XAPIAND_V8) || defined(XAPIAND_CHAISCRIPT)
	try {
		std::shared_ptr<std::pair<size_t, const MsgPack>> old_document_pair;
		do {
#endif
			auto schema_begins = std::chrono::system_clock::now();
			do {
				schema = get_schema(&obj);
				L_INDEX("Schema: %s", repr(schema->to_string()));

				// Get term ID.
				spc_id = schema->get_data_id();
				auto id_type = spc_id.get_type();
				if (did != 0) {
					if (id_type == FieldType::UUID || id_type == FieldType::EMPTY) {
						doc_xid = doc_uuid;
					} else {
						doc_xid = doc_id;
					}
				}
				if (id_type == FieldType::EMPTY) {
					auto f_it = obj.find(ID_FIELD_NAME);
					if (f_it != obj.end()) {
						const auto& field = f_it.value();
						if (field.is_map()) {
							auto f_it_end = field.end();
							auto ft_it = field.find(RESERVED_TYPE);
							if (ft_it != f_it_end) {
								const auto& type = ft_it.value();
								if (!type.is_string()) {
									THROW(ClientError, "Data inconsistency, %s must be string", RESERVED_TYPE);
								}
								spc_id.set_types(type.str_view());
								id_type = spc_id.get_type();
								if (did != 0) {
									if (id_type == FieldType::UUID || id_type == FieldType::EMPTY) {
										doc_xid = doc_uuid;
									} else {
										doc_xid = doc_id;
									}
								}
							}
						}
					}
				} else {
					term_id = Serialise::serialise(spc_id, doc_xid);
					prefixed_term_id = prefixed(term_id, spc_id.prefix(), spc_id.get_ctype());
				}

				// Add ID.
				auto id_value = Cast::cast(id_type, doc_xid);
				auto& id_field = obj[ID_FIELD_NAME];
				if (id_field.is_map()) {
					id_field[RESERVED_VALUE] = id_value;
				} else {
					id_field = id_value;
				}

				// Index object.
#if defined(XAPIAND_CHAISCRIPT) || defined(XAPIAND_V8)
				obj = schema->index(obj, doc, prefixed_term_id, &old_document_pair, this);
#else
				obj = schema->index(obj, doc);
#endif

				// Ensure term ID.
				if (prefixed_term_id.empty()) {
					// Now the schema is full, get specification id.
					spc_id = schema->get_data_id();
					id_type = spc_id.get_type();
					if (did != 0) {
						if (id_type == FieldType::UUID || id_type == FieldType::EMPTY) {
							doc_xid = doc_uuid;
						} else {
							doc_xid = doc_id;
						}
					}
					if (id_type == FieldType::EMPTY) {
						// Index like a namespace.
						const auto type_ser = Serialise::guess_serialise(doc_xid);
						spc_id.set_type(type_ser.first);
						Schema::set_namespace_spc_id(spc_id);
						term_id = type_ser.second;
						prefixed_term_id = prefixed(term_id, spc_id.prefix(), spc_id.get_ctype());
					} else {
						term_id = Serialise::serialise(spc_id, doc_xid);
						prefixed_term_id = prefixed(term_id, spc_id.prefix(), spc_id.get_ctype());
					}
				}
			} while (!update_schema(schema_begins));

			// Finish document: add data, ID term and ID value.
			data.update("", obj.serialise());
			data.flush();
			doc.set_data(data.serialise());

			doc.add_boolean_term(prefixed_term_id);
			doc.add_value(spc_id.slot, term_id);

			// Index document.
#if defined(XAPIAND_V8) || defined(XAPIAND_CHAISCRIPT)
			if (set_document_change_seq(prefixed_term_id, std::make_shared<std::pair<size_t, const MsgPack>>(std::make_pair(Document(doc).hash(), obj)), old_document_pair)) {
#endif
				lock_database lk_db(this);
				try {
					try {
						if (did != 0u) { database->replace_document(did, doc, commit_);
						} else { did = database->replace_document_term(prefixed_term_id, doc, commit_); }
						return std::make_pair(std::move(did), std::move(obj));
					} catch (const Xapian::DatabaseError& exc) {
						// Try to recover from DatabaseError (i.e when the index is manually deleted)
						L_WARNING("ERROR: %s (try recovery)", exc.get_description());
						lk_db.unlock();
						recover_index();
						lk_db.lock();
						if (did != 0u) { database->replace_document(did, doc, commit_);
						} else { did = database->replace_document_term(prefixed_term_id, doc, commit_); }
						return std::make_pair(std::move(did), std::move(obj));
					}
				} catch (...) {
					if (did != 0) {
						try {
							database->delete_document(did, false, false);
						} catch (...) { }
					}
					throw;
				}
#if defined(XAPIAND_V8) || defined(XAPIAND_CHAISCRIPT)
			}
		} while (true);
	} catch (const MissingTypeError&) { //FIXME: perhaps others erros must be handlend in here
		unsigned doccount;
		{
			lock_database lk_db(this);
			doccount = database->db->get_doccount();
		}
		if (doccount == 0 && schema) {
			auto old_schema = schema->get_const_schema();
			XapiandManager::manager->schemas.drop(this, old_schema);
		}
		if (!prefixed_term_id.empty()) {
			dec_document_change_cnt(prefixed_term_id);
		}
		throw;
	} catch (...) {
		if (!prefixed_term_id.empty()) {
			dec_document_change_cnt(prefixed_term_id);
		}
		throw;
	}
#endif
}


DataType
DatabaseHandler::index(std::string_view document_id, bool stored, const MsgPack& body, bool commit_, const ct_type_t& ct_type)
{
	L_CALL("DatabaseHandler::index(%s, %s, %s, %s, %s/%s)", repr(document_id), stored ? "true" : "false", repr(body.to_string()), commit_ ? "true" : "false", ct_type.first, ct_type.second);

	if ((flags & DB_WRITABLE) == 0) {
		THROW(Error, "Database is read-only");
	}

	Data data;
	MsgPack obj(MsgPack::Type::MAP);
	switch (body.getType()) {
		case MsgPack::Type::STR:
			if (stored) {
				data.update(ct_type, -1, 0, 0, serialise_strings({ ct_type.to_string(), body.str_view() }));
			} else {
				data.update(ct_type, serialise_strings({ ct_type.to_string(), body.str_view() }));
			}
			break;
		case MsgPack::Type::UNDEFINED:
			data.erase(ct_type);
			break;
		case MsgPack::Type::MAP:
			obj = body.clone();
			break;
		default:
			THROW(ClientError, "Indexed object must be a JSON, a MsgPack or a blob, is %s", body.getStrType());
	}

	return index(document_id, obj, data, commit_);
}


DataType
DatabaseHandler::patch(std::string_view document_id, const MsgPack& patches, bool commit_, const ct_type_t& /*ct_type*/)
{
	L_CALL("DatabaseHandler::patch(%s, <patches>, %s)", repr(document_id), commit_ ? "true" : "false");

	if ((flags & DB_WRITABLE) == 0) {
		THROW(Error, "database is read-only");
	}

	if (document_id.empty()) {
		THROW(ClientError, "Document must have an 'id'");
	}

	if (!patches.is_map() && !patches.is_array()) {
		THROW(ClientError, "Patches must be a JSON or MsgPack");
	}

	auto document = get_document(document_id);
	auto data = Data(document.get_data());
	auto main_locator = data.get("");
	auto obj = main_locator != nullptr ? MsgPack::unserialise(main_locator->data()) : MsgPack(MsgPack::Type::MAP);

	apply_patch(patches, obj);

	return index(document_id, obj, data, commit_);
}


DataType
DatabaseHandler::merge(std::string_view document_id, bool stored, const MsgPack& body, bool commit_, const ct_type_t& ct_type)
{
	L_CALL("DatabaseHandler::merge(%s, %s, <body>, %s, %s/%s)", repr(document_id), stored ? "true" : "false", commit_ ? "true" : "false", ct_type.first, ct_type.second);

	if ((flags & DB_WRITABLE) == 0) {
		THROW(Error, "database is read-only");
	}

	if (document_id.empty()) {
		THROW(ClientError, "Document must have an 'id'");
	}

	Data data;
	try {
		auto document = get_document(document_id);
		data = Data(document.get_data());
	} catch (const DocNotFoundError&) { }
	auto main_locator = data.get("");
	auto obj = main_locator != nullptr ? MsgPack::unserialise(main_locator->data()) : MsgPack(MsgPack::Type::MAP);

	switch (body.getType()) {
		case MsgPack::Type::STR:
			if (stored) {
				data.update(ct_type, -1, 0, 0, serialise_strings({ ct_type.to_string(), body.str_view() }));
			} else {
				if (body.size() > NON_STORED_SIZE_LIMIT) {
					THROW(ClientError, "Non-stored object has a size limit of %s", string::from_bytes(NON_STORED_SIZE_LIMIT));
				}
				data.update(ct_type, serialise_strings({ ct_type.to_string(), body.str_view() }));
			}
			break;
		case MsgPack::Type::UNDEFINED:
			data.erase(ct_type);
			break;
		case MsgPack::Type::MAP:
			if (stored) {
				THROW(ClientError, "Objects of this type cannot be put in storage");
			}
			obj.update(body);
			break;
		default:
			THROW(ClientError, "Indexed object must be a JSON, a MsgPack or a blob, is %s", body.getStrType());
	}

	return index(document_id, obj, data, commit_);
}


void
DatabaseHandler::write_schema(const MsgPack& obj, bool replace)
{
	L_CALL("DatabaseHandler::write_schema(%s)", repr(obj.to_string()));

	auto schema_begins = std::chrono::system_clock::now();
	bool was_foreign_obj;
	do {
		schema = get_schema();
		was_foreign_obj = schema->write(obj, replace);
		if (!was_foreign_obj && opts.foreign) {
			THROW(ForeignSchemaError, "Schema of %s must use a foreign schema", repr(endpoints.to_string()));
		}
		L_INDEX("Schema to write: %s %s", repr(schema->to_string()), was_foreign_obj ? "(foreign)" : "(local)");
	} while (!update_schema(schema_begins));

	if (was_foreign_obj) {
		MsgPack o = obj;
		o[RESERVED_TYPE] = "object";
		o.erase(RESERVED_ENDPOINT);
		do {
			schema = get_schema();
			was_foreign_obj = schema->write(o, replace);
			L_INDEX("Schema to write: %s (local)", repr(schema->to_string()));
		} while (!update_schema(schema_begins));
	}
}


void
DatabaseHandler::delete_schema()
{
	L_CALL("DatabaseHandler::delete_schema()");

	auto schema_begins = std::chrono::system_clock::now();
	bool done;
	do {
		schema = get_schema();
		auto old_schema = schema->get_const_schema();
		done = XapiandManager::manager->schemas.drop(this, old_schema);
		L_INDEX("Schema to delete: %s", repr(schema->to_string()));
	} while (!done);
	auto schema_ends = std::chrono::system_clock::now();
	Stats::cnt().add("schema_updates", std::chrono::duration_cast<std::chrono::nanoseconds>(schema_ends - schema_begins).count());
}


Xapian::RSet
DatabaseHandler::get_rset(const Xapian::Query& query, Xapian::doccount maxitems)
{
	L_CALL("DatabaseHandler::get_rset(...)");

	lock_database lk_db(this);

	// Xapian::RSet only keeps a set of Xapian::docid internally,
	// so it's thread safe across database checkouts.

	Xapian::RSet rset;

	for (int t = DB_RETRIES; t >= 0; --t) {
		try {
			Xapian::Enquire enquire(*database->db);
			enquire.set_query(query);
			auto mset = enquire.get_mset(0, maxitems);
			for (const auto& did : mset) {
				rset.add_document(did);
			}
			break;
		} catch (const Xapian::DatabaseModifiedError& exc) {
			if (t == 0) { THROW(TimeOutError, "Database was modified, try again: %s", exc.get_description()); }
		} catch (const Xapian::NetworkError& exc) {
			if (t == 0) { THROW(Error, "Problem communicating with the remote database: %s", exc.get_description()); }
		} catch (const Xapian::Error& exc) {
			THROW(Error, exc.get_description());
		}
		database->reopen();
	}

	return rset;
}


std::unique_ptr<Xapian::ExpandDecider>
DatabaseHandler::get_edecider(const similar_field_t& similar)
{
	L_CALL("DatabaseHandler::get_edecider(...)");

	// Expand Decider filter.
	std::vector<std::string> prefixes;
	prefixes.reserve(similar.type.size() + similar.field.size());
	for (const auto& sim_type : similar.type) {
		char type = toUType(Unserialise::type(sim_type));
		prefixes.emplace_back(1, type);
		prefixes.emplace_back(1, tolower(type));
	}
	for (const auto& sim_field : similar.field) {
		auto field_spc = schema->get_data_field(sim_field).first;
		if (field_spc.get_type() != FieldType::EMPTY) {
			prefixes.push_back(field_spc.prefix());
		}
	}
	return std::make_unique<FilterPrefixesExpandDecider>(prefixes);
}


void
DatabaseHandler::dump_metadata(int fd)
{
	L_CALL("DatabaseHandler::dump_metadata()");

	lock_database lk_db(this);

	XXH32_state_t* xxh_state = XXH32_createState();
	XXH32_reset(xxh_state, 0);

	auto db_endpoints = endpoints.to_string();
	serialise_string(fd, dump_metadata_header);
	XXH32_update(xxh_state, dump_metadata_header.data(), dump_metadata_header.size());

	serialise_string(fd, db_endpoints);
	XXH32_update(xxh_state, db_endpoints.data(), db_endpoints.size());

	database->dump_metadata(fd, xxh_state);

	uint32_t current_hash = XXH32_digest(xxh_state);
	XXH32_freeState(xxh_state);

	serialise_length(fd, current_hash);
	L_INFO("Dump hash is 0x%08x", current_hash);
}


void
DatabaseHandler::dump_schema(int fd)
{
	L_CALL("DatabaseHandler::dump_schema()");

	schema = get_schema();
	auto saved_schema_ser = schema->get_full().serialise();

	lock_database lk_db(this);

	XXH32_state_t* xxh_state = XXH32_createState();
	XXH32_reset(xxh_state, 0);

	auto db_endpoints = endpoints.to_string();
	serialise_string(fd, dump_schema_header);
	XXH32_update(xxh_state, dump_schema_header.data(), dump_schema_header.size());

	serialise_string(fd, db_endpoints);
	XXH32_update(xxh_state, db_endpoints.data(), db_endpoints.size());

	serialise_string(fd, saved_schema_ser);
	XXH32_update(xxh_state, saved_schema_ser.data(), saved_schema_ser.size());

	uint32_t current_hash = XXH32_digest(xxh_state);
	XXH32_freeState(xxh_state);

	serialise_length(fd, current_hash);
	L_INFO("Dump hash is 0x%08x", current_hash);
}


void
DatabaseHandler::dump_documents(int fd)
{
	L_CALL("DatabaseHandler::dump_documents()");

	lock_database lk_db(this);

	XXH32_state_t* xxh_state = XXH32_createState();
	XXH32_reset(xxh_state, 0);

	auto db_endpoints = endpoints.to_string();
	serialise_string(fd, dump_documents_header);
	XXH32_update(xxh_state, dump_documents_header.data(), dump_documents_header.size());

	serialise_string(fd, db_endpoints);
	XXH32_update(xxh_state, db_endpoints.data(), db_endpoints.size());

	database->dump_documents(fd, xxh_state);

	uint32_t current_hash = XXH32_digest(xxh_state);
	XXH32_freeState(xxh_state);

	serialise_length(fd, current_hash);
	L_INFO("Dump hash is 0x%08x", current_hash);
}


void
DatabaseHandler::restore(int fd)
{
	L_CALL("DatabaseHandler::restore()");

	std::string buffer;
	std::size_t off = 0;

	lock_database lk_db(this);

	XXH32_state_t* xxh_state = XXH32_createState();
	XXH32_reset(xxh_state, 0);

	auto header = unserialise_string(fd, buffer, off);
	XXH32_update(xxh_state, header.data(), header.size());
	if (header != dump_documents_header && header != dump_schema_header && header != dump_metadata_header) {
		THROW(ClientError, "Invalid dump", RESERVED_TYPE);
	}

	auto db_endpoints = unserialise_string(fd, buffer, off);
	XXH32_update(xxh_state, db_endpoints.data(), db_endpoints.size());

	// restore metadata (key, value)
	if (header == dump_metadata_header) {
		size_t i = 0;
		do {
			++i;
			auto key = unserialise_string(fd, buffer, off);
			XXH32_update(xxh_state, key.data(), key.size());
			auto value = unserialise_string(fd, buffer, off);
			XXH32_update(xxh_state, value.data(), value.size());
			if (key.empty() && value.empty()) {
				break;
			}
			if (key.empty()) {
				L_WARNING("Metadata with no key ignored [%zu]", ID_FIELD_NAME, i);
				continue;
			}
			L_INFO_HOOK("DatabaseHandler::restore", "Restoring metadata %s = %s", key, value);
			database->set_metadata(key, value, false, false);
		} while (true);
	}

	// restore schema
	if (header == dump_schema_header) {
		auto saved_schema_ser = unserialise_string(fd, buffer, off);
		XXH32_update(xxh_state, saved_schema_ser.data(), saved_schema_ser.size());

		lk_db.unlock();
		if (!saved_schema_ser.empty()) {
			auto saved_schema = MsgPack::unserialise(saved_schema_ser);
			L_INFO_HOOK("DatabaseHandler::restore", "Restoring schema: %s", saved_schema.to_string(4));
			write_schema(saved_schema, true);
		}
		schema = get_schema();
		lk_db.lock();
	}

	// restore documents (document_id, object, blob)
	if (header == dump_documents_header) {
		lk_db.unlock();
		schema = get_schema();
		lk_db.lock();

		size_t i = 0;
		do {
			++i;
			auto obj_ser = unserialise_string(fd, buffer, off);
			XXH32_update(xxh_state, obj_ser.data(), obj_ser.size());
			auto blob = unserialise_string(fd, buffer, off);
			XXH32_update(xxh_state, blob.data(), blob.size());
			if (obj_ser.empty() && blob.empty()) { break; }

			Xapian::Document doc;
			required_spc_t spc_id;
			std::string term_id;
			std::string prefixed_term_id;

			std::string ct_type_str;
			if (!blob.empty()) {
				ct_type_str = unserialise_string_at(STORED_BLOB_CONTENT_TYPE, blob);
			}
			auto ct_type = ct_type_t(ct_type_str);

			MsgPack document_id;
			auto obj = MsgPack::unserialise(obj_ser);

			// Get term ID.
			spc_id = schema->get_data_id();
			auto f_it = obj.find(ID_FIELD_NAME);
			if (f_it != obj.end()) {
				const auto& field = f_it.value();
				if (field.is_map()) {
					auto f_it_end = field.end();
					if (spc_id.get_type() == FieldType::EMPTY) {
						auto ft_it = field.find(RESERVED_TYPE);
						if (ft_it != f_it_end) {
							const auto& type = ft_it.value();
							if (!type.is_string()) {
								THROW(ClientError, "Data inconsistency, %s must be string", RESERVED_TYPE);
							}
							spc_id.set_types(type.str_view());
						}
					}
					auto fv_it = field.find(RESERVED_VALUE);
					if (fv_it != f_it_end) {
						document_id = fv_it.value();
					}
				} else {
					document_id = field;
				}
			}

			if (document_id.is_undefined()) {
				L_WARNING("Document with no '%s' ignored [%zu]", ID_FIELD_NAME, i);
				continue;
			}

			obj = schema->index(obj, doc);

			// Ensure term ID.
			if (prefixed_term_id.empty()) {
				// Now the schema is full, get specification id.
				spc_id = schema->get_data_id();
				if (spc_id.get_type() == FieldType::EMPTY) {
					// Index like a namespace.
					const auto type_ser = Serialise::guess_serialise(document_id);
					spc_id.set_type(type_ser.first);
					Schema::set_namespace_spc_id(spc_id);
					term_id = type_ser.second;
					prefixed_term_id = prefixed(term_id, spc_id.prefix(), spc_id.get_ctype());
				} else {
					term_id = Serialise::serialise(spc_id, document_id);
					prefixed_term_id = prefixed(term_id, spc_id.prefix(), spc_id.get_ctype());
				}
			}

			// Finish document: add data, ID term and ID value.
			Data data;
			data.update("", obj.serialise());
			if (!blob.empty()) {
				data.update(ct_type, -1, 0, 0, std::move(blob));
			}
			data.flush();
			doc.set_data(data.serialise());
			doc.add_boolean_term(prefixed_term_id);
			doc.add_value(spc_id.slot, term_id);

			// Index document.
			L_INFO_HOOK("DatabaseHandler::restore", "Restoring document (%zu): %s", i, document_id.to_string());
			database->replace_document_term(prefixed_term_id, doc, false, false);
		} while (true);

		lk_db.unlock();
		auto schema_begins = std::chrono::system_clock::now();
		while (!update_schema(schema_begins)) { }
		lk_db.lock();
	}

	uint32_t saved_hash = unserialise_length(fd, buffer, off);
	uint32_t current_hash = XXH32_digest(xxh_state);
	XXH32_freeState(xxh_state);

	if (saved_hash != current_hash) {
		L_WARNING("Invalid dump hash (0x%08x != 0x%08x)", saved_hash, current_hash);
	}

	database->commit(false);
}


MsgPack
DatabaseHandler::dump_documents()
{
	L_CALL("DatabaseHandler::dump_documents()");

	lock_database lk_db(this);

	return database->dump_documents();
}


void
DatabaseHandler::restore_documents(const MsgPack& docs)
{
	L_CALL("DatabaseHandler::restore_documents()");

	static UUIDGenerator generator;

	lock_database lk_db(this);

	lk_db.unlock();
	schema = get_schema();
	lk_db.lock();

	for (auto obj : docs) {
		std::string_view blob;
		std::string_view ct_type_str;
		auto blob_it = obj.find("_blobs");
		if (blob_it != obj.end()) {
			auto _blob = blob_it.value();
			blob = _blob.at("_data").str_view();
			ct_type_str = _blob.at("_content_type").str_view();
		}

		Xapian::Document doc;
		Xapian::docid did = 0;
		required_spc_t spc_id;
		std::string term_id;
		std::string prefixed_term_id;

		auto ct_type = ct_type_t(ct_type_str);

		MsgPack document_id;

		// Get term ID.
		spc_id = schema->get_data_id();
		auto f_it = obj.find(ID_FIELD_NAME);
		if (f_it != obj.end()) {
			const auto& field = f_it.value();
			if (field.is_map()) {
				auto f_it_end = field.end();
				if (spc_id.get_type() == FieldType::EMPTY) {
					auto ft_it = field.find(RESERVED_TYPE);
					if (ft_it != f_it_end) {
						const auto& type = ft_it.value();
						if (!type.is_string()) {
							THROW(ClientError, "Data inconsistency, %s must be string", RESERVED_TYPE);
						}
						spc_id.set_types(type.str_view());
					}
				}
				auto fv_it = field.find(RESERVED_VALUE);
				if (fv_it != f_it_end) {
					document_id = fv_it.value();
				}
			} else {
				document_id = field;
			}
		}

		obj = schema->index(obj, doc);

		// Ensure term ID.
		if (prefixed_term_id.empty()) {
			// Now the schema is full, get specification id.
			spc_id = schema->get_data_id();
			if (spc_id.get_type() == FieldType::EMPTY) {
				// Index like a namespace.
				if (document_id.is_undefined()) {
					document_id = Unserialise::uuid(generator(opts.uuid_compact).serialise(), static_cast<UUIDRepr>(opts.uuid_repr));
				}
				const auto type_ser = Serialise::guess_serialise(document_id);
				spc_id.set_type(type_ser.first);
				Schema::set_namespace_spc_id(spc_id);
				term_id = type_ser.second;
				prefixed_term_id = prefixed(term_id, spc_id.prefix(), spc_id.get_ctype());
			} else {
				if (document_id.is_undefined()) {
					try {
						// Add a new empty document to get its document ID:
						did = database->add_document(Xapian::Document(), false, false);
					} catch (const Xapian::DatabaseError&) {
						// Try to recover from DatabaseError (i.e when the index is manually deleted)
						lk_db.unlock();
						recover_index();
						lk_db.lock();
						did = database->add_document(Xapian::Document(), false, false);
					}
					document_id = Cast::cast(spc_id.get_type(), std::to_string(did));
				}
				term_id = Serialise::serialise(spc_id, document_id);
				prefixed_term_id = prefixed(term_id, spc_id.prefix(), spc_id.get_ctype());
			}
		}

		// Finish document: add data, ID term and ID value.
		Data data;
		data.update("", obj.serialise());
		if (!blob.empty()) {
			data.update(ct_type, -1, 0, 0, blob);
		}
		data.flush();
		doc.set_data(data.serialise());
		doc.add_boolean_term(prefixed_term_id);
		doc.add_value(spc_id.slot, term_id);

		// Index document.
		if (did != 0u) { database->replace_document(did, doc, false, false);
		} else { did = database->replace_document_term(prefixed_term_id, doc, false, false); }
	};

	lk_db.unlock();
	auto schema_begins = std::chrono::system_clock::now();
	while (!update_schema(schema_begins)) { }
	lk_db.lock();
}


MSet
DatabaseHandler::get_mset(const query_field_t& e, const MsgPack* qdsl, AggregationMatchSpy* aggs, std::vector<std::string>& /*suggestions*/)
{
	L_CALL("DatabaseHandler::get_mset(%s, %s)", repr(string::join(e.query, " & ")), qdsl ? repr(qdsl->to_string()) : "null");

	schema = get_schema();

	auto limit = -1;
	auto offset = -1;
	Xapian::Query query;
	std::unique_ptr<Multi_MultiValueKeyMaker> sorter;
	switch (method) {
		case HTTP_GET:
		case HTTP_POST: {
			if ((qdsl != nullptr) && qdsl->find(QUERYDSL_QUERY) != qdsl->end()) {
				QueryDSL query_object(schema);
				query = query_object.get_query(qdsl->at(QUERYDSL_QUERY));

				if (qdsl->find(QUERYDSL_LIMIT) != qdsl->end()) {
					auto lm = qdsl->at(QUERYDSL_LIMIT);
					if (lm.is_integer()) {
						limit = lm.as_u64();
					} else {
						THROW(ClientError, "The %s must be a unsigned int", QUERYDSL_LIMIT);
					}
				}

				if (qdsl->find(QUERYDSL_OFFSET) != qdsl->end()) {
					auto off = qdsl->at(QUERYDSL_OFFSET);
					if (off.is_integer()) {
						offset = off.as_u64();
					} else {
						THROW(ClientError, "The %s must be a unsigned int", QUERYDSL_OFFSET);
					}
				}

				if (qdsl->find(QUERYDSL_SORT) != qdsl->end()) {
					auto sort = qdsl->at(QUERYDSL_SORT);
					query_object.get_sorter(sorter, sort);
				}
			} else {
				QueryDSL query_object(schema);
				query = query_object.get_query(query_object.make_dsl_query(e));
			}
			break;
		}

		default:
			break;
	}

	if (offset < 0) {
		offset = e.offset;
	}

	if (limit < 0) {
		limit = e.limit;
	}

	// L_DEBUG("query: %s", query.get_description());

	// Configure sorter.
	if (!sorter && !e.sort.empty()) {
		sorter = std::make_unique<Multi_MultiValueKeyMaker>();
		std::string field, value;
		for (const auto& sort : e.sort) {
			size_t pos = sort.find(":");
			if (pos == std::string::npos) {
				field.assign(sort);
				value.clear();
			} else {
				field.assign(sort.substr(0, pos));
				value.assign(sort.substr(pos + 1));
			}
			bool descending = false;
			switch (field.at(0)) {
				case '-':
					descending = true;
					/* FALLTHROUGH */
				case '+':
					field.erase(field.begin());
					break;
			}
			const auto field_spc = schema->get_slot_field(field);
			if (field_spc.get_type() != FieldType::EMPTY) {
				sorter->add_value(field_spc, descending, value, e);
			}
		}
	}

	// Get the collapse key to use for queries.
	Xapian::valueno collapse_key = Xapian::BAD_VALUENO;
	if (!e.collapse.empty()) {
		const auto field_spc = schema->get_slot_field(e.collapse);
		collapse_key = field_spc.slot;
	}

	// Configure nearest and fuzzy search:
	std::unique_ptr<Xapian::ExpandDecider> nearest_edecider;
	Xapian::RSet nearest_rset;
	if (e.is_nearest) {
		nearest_edecider = get_edecider(e.nearest);
		nearest_rset = get_rset(query, e.nearest.n_rset);
	}

	Xapian::RSet fuzzy_rset;
	std::unique_ptr<Xapian::ExpandDecider> fuzzy_edecider;
	if (e.is_fuzzy) {
		fuzzy_edecider = get_edecider(e.fuzzy);
		fuzzy_rset = get_rset(query, e.fuzzy.n_rset);
	}

	MSet mset{};

	lock_database lk_db(this);
	for (int t = DB_RETRIES; t >= 0; --t) {
		try {
			auto final_query = query;
			Xapian::Enquire enquire(*database->db);
			if (collapse_key != Xapian::BAD_VALUENO) {
				enquire.set_collapse_key(collapse_key, e.collapse_max);
			}
			if (aggs != nullptr) {
				enquire.add_matchspy(aggs);
			}
			if (sorter) {
				enquire.set_sort_by_key_then_relevance(sorter.get(), false);
			}
			if (e.is_nearest) {
				auto eset = enquire.get_eset(e.nearest.n_eset, nearest_rset, nearest_edecider.get());
				final_query = Xapian::Query(Xapian::Query::OP_ELITE_SET, eset.begin(), eset.end(), e.nearest.n_term);
			}
			if (e.is_fuzzy) {
				auto eset = enquire.get_eset(e.fuzzy.n_eset, fuzzy_rset, fuzzy_edecider.get());
				final_query = Xapian::Query(Xapian::Query::OP_OR, final_query, Xapian::Query(Xapian::Query::OP_ELITE_SET, eset.begin(), eset.end(), e.fuzzy.n_term));
			}
			enquire.set_query(final_query);
			mset = enquire.get_mset(offset, limit, e.check_at_least);
			break;
		} catch (const Xapian::DatabaseModifiedError& exc) {
			if (t == 0) { THROW(TimeOutError, "Database was modified, try again: %s", exc.get_description()); }
		} catch (const Xapian::NetworkError& exc) {
			if (t == 0) { THROW(Error, "Problem communicating with the remote database: %s", exc.get_description()); }
		} catch (const QueryParserError& exc) {
			THROW(ClientError, exc.what());
		} catch (const SerialisationError& exc) {
			THROW(ClientError, exc.what());
		} catch (const QueryDslError& exc) {
			THROW(ClientError, exc.what());
		} catch (const Xapian::QueryParserError& exc) {
			THROW(ClientError, exc.get_description());
		} catch (const Xapian::Error& exc) {
			THROW(Error, exc.get_description());
		} catch (const std::exception& exc) {
			THROW(ClientError, "The search was not performed: %s", exc.what());
		}
		database->reopen();
	}

	return mset;
}


bool
DatabaseHandler::update_schema(std::chrono::time_point<std::chrono::system_clock> schema_begins)
{
	L_CALL("DatabaseHandler::update_schema()");
	bool done = true;
	bool updated = false;

	auto mod_schema = schema->get_modified_schema();
	if (mod_schema) {
		updated = true;
		auto old_schema = schema->get_const_schema();
		done = XapiandManager::manager->schemas.set(this, old_schema, mod_schema);
	}

	if (done) {
		auto schema_ends = std::chrono::system_clock::now();
		if (updated) {
			Stats::cnt().add("schema_updates", std::chrono::duration_cast<std::chrono::nanoseconds>(schema_ends - schema_begins).count());
		} else {
			Stats::cnt().add("schema_reads", std::chrono::duration_cast<std::chrono::nanoseconds>(schema_ends - schema_begins).count());
		}
	}

	return done;
}


std::string
DatabaseHandler::get_prefixed_term_id(std::string_view document_id)
{
	L_CALL("DatabaseHandler::get_prefixed_term_id(%s)", repr(document_id));

	schema = get_schema();

	auto field_spc = schema->get_data_id();
	if (field_spc.get_type() == FieldType::EMPTY) {
		// Search like namespace.
		const auto type_ser = Serialise::guess_serialise(document_id);
		field_spc.set_type(type_ser.first);
		Schema::set_namespace_spc_id(field_spc);
		return prefixed(type_ser.second, field_spc.prefix(), field_spc.get_ctype());
	}

	return prefixed(Serialise::serialise(field_spc, document_id), field_spc.prefix(), field_spc.get_ctype());
}


std::vector<std::string>
DatabaseHandler::get_metadata_keys()
{
	L_CALL("DatabaseHandler::get_metadata_keys()");

	lock_database lk_db(this);
	return database->get_metadata_keys();
}


std::string
DatabaseHandler::get_metadata(const std::string& key)
{
	L_CALL("DatabaseHandler::get_metadata(%s)", repr(key));

	lock_database lk_db(this);
	return database->get_metadata(key);
}


std::string
DatabaseHandler::get_metadata(std::string_view key)
{
	return get_metadata(std::string(key));
}


bool
DatabaseHandler::set_metadata(const std::string& key, const std::string& value, bool overwrite)
{
	L_CALL("DatabaseHandler::set_metadata(%s, %s, %s)", repr(key), repr(value), overwrite ? "true" : "false");

	lock_database lk_db(this);
	if (!overwrite) {
		auto old_value = database->get_metadata(key);
		if (!old_value.empty()) {
			return (old_value == value);
		}
	}
	database->set_metadata(key, value);
	return true;
}


bool
DatabaseHandler::set_metadata(std::string_view key, std::string_view value, bool overwrite)
{
	return set_metadata(std::string(key), std::string(value), overwrite);
}


Document
DatabaseHandler::get_document(const Xapian::docid& did)
{
	L_CALL("DatabaseHandler::get_document((Xapian::docid)%d)", did);

	lock_database lk_db(this);
	return Document(this, database->get_document(did));
}


Document
DatabaseHandler::get_document(std::string_view document_id)
{
	L_CALL("DatabaseHandler::get_document((std::string)%s)", repr(document_id));

	auto did = to_docid(document_id);
	if (did != 0u) {
		return get_document(did);
	}

	const auto term_id = get_prefixed_term_id(document_id);

	lock_database lk_db(this);
	did = database->find_document(term_id);
	return Document(this, database->get_document(did, (database->flags & DB_WRITABLE) != 0));
}


Xapian::docid
DatabaseHandler::get_docid(std::string_view document_id)
{
	L_CALL("DatabaseHandler::get_docid(%s)", repr(document_id));

	auto did = to_docid(document_id);
	if (did != 0u) {
		return did;
	}

	const auto term_id = get_prefixed_term_id(document_id);

	lock_database lk_db(this);
	return database->find_document(term_id);
}


void
DatabaseHandler::delete_document(std::string_view document_id, bool commit_, bool wal_)
{
	L_CALL("DatabaseHandler::delete_document(%s)", repr(document_id));

	auto did = to_docid(document_id);
	if (did != 0u) {
		database->delete_document(did, commit_, wal_);
		return;
	}

	const auto term_id = get_prefixed_term_id(document_id);

	lock_database lk_db(this);
	database->delete_document(database->find_document(term_id), commit_, wal_);
}


MsgPack
DatabaseHandler::get_document_info(std::string_view document_id)
{
	L_CALL("DatabaseHandler::get_document_info(%s)", repr(document_id));

	auto document = get_document(document_id);
	const auto data = Data(document.get_data());

	MsgPack info;

	info[RESPONSE_DOCID] = document.get_docid();

	if (data.empty()) {
		info[RESPONSE_DATA] = data.serialise();
	} else {
		auto& info_data = info[RESPONSE_DATA];
		for (auto& locator : data) {
			switch (locator.type) {
				case Data::Type::inplace:
					if (locator.ct_type.empty()) {
						info_data.push_back(MsgPack({
							{ RESPONSE_CONTENT_TYPE, MSGPACK_CONTENT_TYPE },
							{ RESPONSE_TYPE, "inplace" },
							{ RESPONSE_SIZE, locator.data().size() },
							{ RESPONSE_OBJECT, MsgPack::unserialise(locator.data()) }
						}));
					} else {
						info_data.push_back(MsgPack({
							{ RESPONSE_CONTENT_TYPE, locator.ct_type.to_string() },
							{ RESPONSE_TYPE, "inplace" },
							{ RESPONSE_SIZE, locator.data().size() },
						}));
					}
					break;
				case Data::Type::stored:
					info_data.push_back(MsgPack({
						{ RESPONSE_CONTENT_TYPE, locator.ct_type.to_string() },
						{ RESPONSE_TYPE, "stored" },
						{ RESPONSE_VOLUME, locator.volume },
						{ RESPONSE_OFFSET, locator.offset },
						{ RESPONSE_SIZE, locator.size },
					}));
					break;
			}
		}
	}

	info[RESPONSE_TERMS] = document.get_terms();
	info[RESPONSE_VALUES] = document.get_values();

	return info;
}


MsgPack
DatabaseHandler::get_database_info()
{
	L_CALL("DatabaseHandler::get_database_info()");

	lock_database lk_db(this);
	unsigned doccount = database->db->get_doccount();
	unsigned lastdocid = database->db->get_lastdocid();
	MsgPack info;
	info[RESPONSE_UUID] = database->db->get_uuid();
	info[RESPONSE_DOC_COUNT] = doccount;
	info[RESPONSE_LAST_ID] = lastdocid;
	info[RESPONSE_DOC_DEL] = lastdocid - doccount;
	info[RESPONSE_AV_LENGTH] = database->db->get_avlength();
	info[RESPONSE_DOC_LEN_LOWER] =  database->db->get_doclength_lower_bound();
	info[RESPONSE_DOC_LEN_UPPER] = database->db->get_doclength_upper_bound();
	info[RESPONSE_HAS_POSITIONS] = database->db->has_positions();
	return info;
}


bool
DatabaseHandler::commit(bool _wal)
{
	L_CALL("DatabaseHandler::commit(%s)", _wal ? "true" : "false");

	lock_database lk_db(this);
	return database->commit(_wal);
}


bool
DatabaseHandler::reopen()
{
	L_CALL("DatabaseHandler::reopen()");

	lock_database lk_db(this);
	return database->reopen();
}


long long
DatabaseHandler::get_mastery_level()
{
	L_CALL("DatabaseHandler::get_mastery_level()");

	try {
		lock_database lk_db(this);
		return database->mastery_level;
	} catch (const CheckoutError&) {
		return ::read_mastery(endpoints[0].path, false);
	}
}


void
DatabaseHandler::init_ref(const Endpoint& endpoint)
{
	L_CALL("DatabaseHandler::init_ref(%s)", repr(endpoint.to_string()));

	DatabaseHandler db_handler(Endpoints(Endpoint(".refs")), DB_WRITABLE | DB_SPAWN | DB_PERSISTENT | DB_NOWAL);

	const auto document_id = get_hashed(endpoint.path);

	try {
		static const std::string reserved_schema(RESERVED_SCHEMA);
		if (db_handler.get_metadata(reserved_schema).empty()) {
			db_handler.set_metadata(reserved_schema, Schema::get_initial_schema()->serialise());
		}
		try {
			db_handler.get_document(document_id);
		} catch (const DocNotFoundError&) {
			static const MsgPack obj = {
				{ ID_FIELD_NAME, { { RESERVED_TYPE,  "term"             }, { RESERVED_INDEX, "field"   } } },
				{ "master",      { { RESERVED_VALUE, DOCUMENT_DB_MASTER }, { RESERVED_TYPE,  "term"    }, { RESERVED_INDEX, "field_terms"  } } },
				{ "reference",   { { RESERVED_VALUE, 1                  }, { RESERVED_TYPE,  "integer" }, { RESERVED_INDEX, "field_values" } } },
			};
			db_handler.index(document_id, false, obj, true, msgpack_type);
		}
	} catch (const CheckoutError&) {
		L_CRIT("Cannot open %s database", repr(db_handler.endpoints.to_string()));
		return;
	}
}


void
DatabaseHandler::inc_ref(const Endpoint& endpoint)
{
	L_CALL("DatabaseHandler::inc_ref(%s)", repr(endpoint.to_string()));

	DatabaseHandler db_handler(Endpoints(Endpoint(".refs")), DB_WRITABLE | DB_SPAWN | DB_PERSISTENT | DB_NOWAL);

	const auto document_id = get_hashed(endpoint.path);

	try {
		try {
			auto document = db_handler.get_document(document_id);
			auto nref = document.get_value("reference").i64() + 1;
			const MsgPack obj = {
				{ ID_FIELD_NAME, { { RESERVED_TYPE,  "term"             }, { RESERVED_INDEX, "field"   } } },
				{ "master",      { { RESERVED_VALUE, DOCUMENT_DB_MASTER }, { RESERVED_TYPE,  "term"    }, { RESERVED_INDEX, "field_terms"  } } },
				{ "reference",   { { RESERVED_VALUE, nref               }, { RESERVED_TYPE,  "integer" }, { RESERVED_INDEX, "field_values" } } },
			};
			db_handler.index(document_id, false, obj, true, msgpack_type);
		} catch (const DocNotFoundError&) {
			// QUESTION: Document not found - should add?
			// QUESTION: This case could happen?
			static const MsgPack obj = {
				{ ID_FIELD_NAME, { { RESERVED_TYPE,  "term"             }, { RESERVED_INDEX, "field"   } } },
				{ "master",      { { RESERVED_VALUE, DOCUMENT_DB_MASTER }, { RESERVED_TYPE,  "term"    }, { RESERVED_INDEX, "field_terms"  } } },
				{ "reference",   { { RESERVED_VALUE, 1                  }, { RESERVED_TYPE,  "integer" }, { RESERVED_INDEX, "field_values" } } },
			};
			db_handler.index(document_id, false, obj, true, msgpack_type);
		}
	} catch (const CheckoutError&) {
		L_CRIT("Cannot open %s database", repr(db_handler.endpoints.to_string()));
		return;
	}
}


void
DatabaseHandler::dec_ref(const Endpoint& endpoint)
{
	L_CALL("DatabaseHandler::dec_ref(%s)", repr(endpoint.to_string()));

	DatabaseHandler db_handler(Endpoints(Endpoint(".refs")), DB_WRITABLE | DB_SPAWN | DB_PERSISTENT | DB_NOWAL);

	const auto document_id = get_hashed(endpoint.path);

	try {
		try {
			auto document = db_handler.get_document(document_id);
			auto nref = document.get_value("reference").i64() - 1;
			const MsgPack obj = {
				{ ID_FIELD_NAME, { { RESERVED_TYPE,  "term"             }, { RESERVED_INDEX, "field"   } } },
				{ "master",      { { RESERVED_VALUE, DOCUMENT_DB_MASTER }, { RESERVED_TYPE,  "term"    }, { RESERVED_INDEX, "field_terms"  } } },
				{ "reference",   { { RESERVED_VALUE, nref               }, { RESERVED_TYPE,  "integer" }, { RESERVED_INDEX, "field_values" } } },
			};
			db_handler.index(document_id, false, obj, true, msgpack_type);
			if (nref == 0) {
				// qmtx need a lock
				delete_files(endpoint.path);
			}
		} catch (const DocNotFoundError&) { }
	} catch (const CheckoutError&) {
		L_CRIT("Cannot open %s database", repr(db_handler.endpoints.to_string()));
		return;
	}
}


int
DatabaseHandler::get_master_count()
{
	L_CALL("DatabaseHandler::get_master_count()");

	DatabaseHandler db_handler(Endpoints(Endpoint(".refs")), DB_WRITABLE | DB_SPAWN | DB_PERSISTENT | DB_NOWAL);

	try {
		std::vector<std::string> suggestions;
		query_field_t q_t;
		q_t.limit = 0;
		q_t.query.emplace_back("master:M");
		auto mset = db_handler.get_mset(q_t, nullptr, nullptr, suggestions);
		return mset.get_matches_estimated();
	} catch (const CheckoutError&) {
		L_CRIT("Cannot open %s database", repr(db_handler.endpoints.to_string()));
		return - 1;
	}
}


#if defined(XAPIAND_V8) || defined(XAPIAND_CHAISCRIPT)
const std::shared_ptr<std::pair<size_t, const MsgPack>>
DatabaseHandler::get_document_change_seq(std::string_view term_id)
{
	L_CALL("DatabaseHandler::get_document_change_seq(%s, %s)", endpoints.to_string(), repr(term_id));

	static std::hash<std::string_view> hash_fn_string;
	auto key = endpoints.hash() ^ hash_fn_string(term_id);

	bool is_local = endpoints[0].is_local();

	std::unique_lock<std::mutex> lk(DatabaseHandler::documents_mtx);

	auto it = DatabaseHandler::documents.end();
	if (is_local) {
		it = DatabaseHandler::documents.find(key);
	}

	std::shared_ptr<std::pair<size_t, const MsgPack>> current_document_pair;
	if (it == DatabaseHandler::documents.end()) {
		lk.unlock();

		// Get document from database
		try {
			auto current_document = get_document_term(term_id);
			current_document_pair = std::make_shared<std::pair<size_t, const MsgPack>>(std::make_pair(current_document.hash(), current_document.get_obj()));
		} catch (const DocNotFoundError&) { }

		lk.lock();

		if (is_local) {
			it = DatabaseHandler::documents.emplace(key, current_document_pair).first;
			current_document_pair = it->second;
		}
	} else {
		current_document_pair = it->second;
	}

	return current_document_pair;
}


bool
DatabaseHandler::set_document_change_seq(std::string_view term_id, const std::shared_ptr<std::pair<size_t, const MsgPack>>& new_document_pair, std::shared_ptr<std::pair<size_t, const MsgPack>>& old_document_pair)
{
	L_CALL("DatabaseHandler::set_document_change_seq(%s, %s, %s, %s)", endpoints.to_string(), repr(term_id), std::to_string(new_document_pair->first), old_document_pair ? std::to_string(old_document_pair->first) : "nullptr");

	static std::hash<std::string_view> hash_fn_string;
	auto key = endpoints.hash() ^ hash_fn_string(term_id);

	bool is_local = endpoints[0].is_local();

	std::unique_lock<std::mutex> lk(DatabaseHandler::documents_mtx);

	auto it = DatabaseHandler::documents.end();
	if (is_local) {
		it = DatabaseHandler::documents.find(key);
	}

	std::shared_ptr<std::pair<size_t, const MsgPack>> current_document_pair;
	if (it == DatabaseHandler::documents.end()) {
		if (old_document_pair) {
			lk.unlock();

			// Get document from database
			try {
				auto current_document = get_document_term(term_id);
				current_document_pair = std::make_shared<std::pair<size_t, const MsgPack>>(std::make_pair(current_document.hash(), current_document.get_obj()));
			} catch (const DocNotFoundError&) { }

			lk.lock();

			if (is_local) {
				it = DatabaseHandler::documents.emplace(key, current_document_pair).first;
				current_document_pair = it->second;
			}
		}
	} else {
		current_document_pair = it->second;
	}

	bool accepted = (!old_document_pair || (current_document_pair && old_document_pair->first == current_document_pair->first));

	current_document_pair.reset();
	old_document_pair.reset();

	if (it != DatabaseHandler::documents.end()) {
		if (it->second.use_count() == 1) {
			DatabaseHandler::documents.erase(it);
		} else if (accepted) {
			it->second = new_document_pair;
		}
	}

	return accepted;
}


void
DatabaseHandler::dec_document_change_cnt(std::string_view term_id)
{
	L_CALL("DatabaseHandler::dec_document_change_cnt(%s, %s)", endpoints.to_string(), repr(term_id));

	static std::hash<std::string_view> hash_fn_string;
	auto key = endpoints.hash() ^ hash_fn_string(term_id);

	bool is_local = endpoints[0].is_local();

	std::lock_guard<std::mutex> lk(DatabaseHandler::documents_mtx);

	auto it = DatabaseHandler::documents.end();
	if (is_local) {
		it = DatabaseHandler::documents.find(key);
	}

	if (it != DatabaseHandler::documents.end()) {
		if (it->second.use_count() == 1) {
			DatabaseHandler::documents.erase(it);
		}
	}
}
#endif



/*  ____                                        _
 * |  _ \  ___   ___ _   _ _ __ ___   ___ _ __ | |_
 * | | | |/ _ \ / __| | | | '_ ` _ \ / _ \ '_ \| __|
 * | |_| | (_) | (__| |_| | | | | | |  __/ | | | |_
 * |____/ \___/ \___|\__,_|_| |_| |_|\___|_| |_|\__|
 *
 */

Document::Document()
	: did(0),
	  db_handler(nullptr) { }


Document::Document(const Xapian::Document& doc_)
	: did(doc_.get_docid()),
	  db_handler(nullptr) { }


Document::Document(DatabaseHandler* db_handler_, const Xapian::Document& doc_)
	: did(doc_.get_docid()),
	  db_handler(db_handler_) { }


Document::Document(const Document& doc_)
	
	  = default;


Document&
Document::operator=(const Document& doc_)
= default;


Xapian::Document
Document::get_document()
{
	L_CALL("Document::get_document()");

	Xapian::Document doc;
	if ((db_handler != nullptr) && db_handler->database) {
		doc = db_handler->database->get_document(did, true);
	}
	return doc;
}


Xapian::docid
Document::get_docid()
{
	return did;
}


std::string
Document::serialise(size_t retries)
{
	L_CALL("Document::serialise(%zu)", retries);

	try {
		lock_database lk_db(db_handler);
		auto doc = get_document();
		return doc.serialise();
	} catch (const Xapian::DatabaseModifiedError& exc) {
		if (retries != 0u) {
			return serialise(--retries);
		}
		THROW(TimeOutError, "Database was modified, try again: %s", exc.get_description());
	}
}


std::string
Document::get_value(Xapian::valueno slot, size_t retries)
{
	L_CALL("Document::get_value(%u, %zu)", slot, retries);

	try {
		lock_database lk_db(db_handler);
		auto doc = get_document();
		return doc.get_value(slot);
	} catch (const Xapian::DatabaseModifiedError& exc) {
		if (retries != 0u) {
			return get_value(slot, --retries);
		}
		THROW(TimeOutError, "Database was modified, try again: %s", exc.get_description());
	}
}


std::string
Document::get_data(size_t retries)
{
	L_CALL("Document::get_data(%zu)", retries);

	try {
		lock_database lk_db(db_handler);
		auto doc = get_document();
		return doc.get_data();
	} catch (const Xapian::DatabaseModifiedError& exc) {
		if (retries != 0u) {
			return get_data(--retries);
		}
		THROW(TimeOutError, "Database was modified, try again: %s", exc.get_description());
	}
}


std::string
Document::get_blob(const ct_type_t& ct_type, size_t retries)
{
	L_CALL("Document::get_blob(%zu)", retries);

	try {
		lock_database lk_db(db_handler);
		auto doc = get_document();
		auto data = Data(doc.get_data());
		auto locator = data.get(ct_type);
		if (locator != nullptr) {
			if (!locator->data().empty()) {
				return std::string(locator->data());
			}
#ifdef XAPIAND_DATA_STORAGE
			if (locator->type == Data::Type::stored) {
 				return db_handler->database->storage_get_blob(doc, *locator);
			}
#endif
		}
		return "";
	} catch (const Xapian::DatabaseModifiedError& exc) {
		if (retries != 0u) {
			return get_blob(ct_type, --retries);
		}
		THROW(TimeOutError, "Database was modified, try again: %s", exc.get_description());
	}
}


MsgPack
Document::get_terms(size_t retries)
{
	L_CALL("get_terms(%zu)", retries);

	try {
		MsgPack terms;

		lock_database lk_db(db_handler);
		auto doc = get_document();

		// doc.termlist_count() disassociates the database in doc.

		const auto it_e = doc.termlist_end();
		for (auto it = doc.termlist_begin(); it != it_e; ++it) {
			auto& term = terms[*it];
			term[RESPONSE_WDF] = it.get_wdf();  // The within-document-frequency of the current term in the current document.
			try {
				auto _term_freq = it.get_termfreq();  // The number of documents which this term indexes.
				term[RESPONSE_TERM_FREQ] = _term_freq;
			} catch (const Xapian::InvalidOperationError&) { }  // Iterator has moved, and does not support random access or doc is not associated with a database.
			if (it.positionlist_count() != 0u) {
				auto& term_pos = term[RESPONSE_POS];
				term_pos.reserve(it.positionlist_count());
				const auto pit_e = it.positionlist_end();
				for (auto pit = it.positionlist_begin(); pit != pit_e; ++pit) {
					term_pos.push_back(*pit);
				}
			}
		}
		return terms;
	} catch (const Xapian::DatabaseModifiedError& exc) {
		if (retries != 0u) {
			return get_terms(--retries);
		}
		THROW(TimeOutError, "Database was modified, try again: %s", exc.get_description());
	}
}


MsgPack
Document::get_values(size_t retries)
{
	L_CALL("get_values(%zu)", retries);

	try {
		MsgPack values;

		lock_database lk_db(db_handler);
		auto doc = get_document();

		values.reserve(doc.values_count());
		const auto iv_e = doc.values_end();
		for (auto iv = doc.values_begin(); iv != iv_e; ++iv) {
			values[std::to_string(iv.get_valueno())] = *iv;
		}
		return values;
	} catch (const Xapian::DatabaseModifiedError& exc) {
		if (retries != 0u) {
			return get_values(--retries);
		}
		THROW(TimeOutError, "Database was modified, try again: %s", exc.get_description());
	}
}


MsgPack
Document::get_value(std::string_view slot_name)
{
	L_CALL("Document::get_value(%s)", repr(slot_name));

	if (db_handler != nullptr) {
		auto slot_field = db_handler->get_schema()->get_slot_field(slot_name);
		return Unserialise::MsgPack(slot_field.get_type(), get_value(slot_field.slot));
	}
	return MsgPack(MsgPack::Type::NIL);
}


MsgPack
Document::get_obj()
{
	L_CALL("Document::get_obj()");

	auto data = Data(get_data());
	auto main_locator = data.get("");
	auto obj = main_locator != nullptr ? MsgPack::unserialise(main_locator->data()) : MsgPack();
	return obj;
}


MsgPack
Document::get_field(std::string_view slot_name)
{
	L_CALL("Document::get_field(%s)", repr(slot_name));

	return get_field(slot_name, get_obj());
}


MsgPack
Document::get_field(std::string_view slot_name, const MsgPack& obj)
{
	L_CALL("Document::get_field(%s, <obj>)", repr(slot_name));

	auto itf = obj.find(slot_name);
	if (itf != obj.end()) {
		const auto& value = itf.value();
		if (value.is_map()) {
			auto itv = value.find(RESERVED_VALUE);
			if (itv != value.end()) {
				return itv.value();
			}
		}
		return value;
	}

	return MsgPack(MsgPack::Type::NIL);
}


uint64_t
Document::hash(size_t retries)
{
	try {
		lock_database lk_db(db_handler);

		auto doc = get_document();

		uint64_t hash = 0;

		// Add hash of values
		const auto iv_e = doc.values_end();
		for (auto iv = doc.values_begin(); iv != iv_e; ++iv) {
			hash ^= xxh64::hash(*iv) * iv.get_valueno();
		}

		// Add hash of terms
		const auto it_e = doc.termlist_end();
		for (auto it = doc.termlist_begin(); it != it_e; ++it) {
			hash ^= xxh64::hash(*it) * it.get_wdf();
			const auto pit_e = it.positionlist_end();
			for (auto pit = it.positionlist_begin(); pit != pit_e; ++pit) {
				hash ^= *pit;
			}
		}

		// Add hash of data
		hash ^= xxh64::hash(doc.get_data());

		return hash;
	} catch (const Xapian::DatabaseModifiedError& exc) {
		if (retries != 0u) {
			return hash(--retries);
		}
		THROW(TimeOutError, "Database was modified, try again: %s", exc.get_description());
	}
}
