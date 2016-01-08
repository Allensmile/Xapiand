/*
 * Copyright (C) 2015 deipi.com LLC and contributors. All rights reserved.
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

#include "database.h"
#include "database_autocommit.h"
#include "multivaluerange.h"
#include "length.h"
#include "cJSON_Utils.h"
#include "log.h"
#include "generate_terms.h"

#include <assert.h>
#include <bitset>

#define XAPIAN_LOCAL_DB_FALLBACK 1

#define DATABASE_UPDATE_TIME 10

#define DEFAULT_OFFSET "0" /* Replace for the real offset */


static const std::regex find_field_re("(([_a-z][_a-z0-9]*):)?(\"[^\"]+\"|[^\": ]+)[ ]*", std::regex::icase | std::regex::optimize);


static auto getPos = [](size_t pos, size_t size) noexcept {
	return pos < size ? pos : size;
};


Database::Database(std::shared_ptr<DatabaseQueue> &queue_, const Endpoints &endpoints_, int flags_)
	: weak_queue(queue_),
	  endpoints(endpoints_),
	  flags(flags_),
	  hash(endpoints.hash()),
	  access_time(system_clock::now()),
	  mastery_level(-1)
{
	reopen();

	if (auto queue = weak_queue.lock()) {
		queue->inc_count();
	}
}


Database::~Database()
{
	if (auto queue = weak_queue.lock()) {
		queue->dec_count();
	}
}


long long
Database::read_mastery(const std::string &dir)
{
	if (!local) return -1;
	if (mastery_level != -1) return mastery_level;

	mastery_level = ::read_mastery(dir, true);

	return mastery_level;
}


void
Database::reopen()
{
	access_time = system_clock::now();

	if (db) {
		// Try to reopen
		try {
			db->reopen();
			schema.setDatabase(this);
			return;
		} catch (const Xapian::Error &err) {
			L_ERR(this, "ERROR: %s", err.get_msg().c_str());
			db->close();
			db.reset();
		}
	}

	Xapian::Database rdb;
	Xapian::WritableDatabase wdb;
	Xapian::Database ldb;

	auto endpoints_size = endpoints.size();

	const Endpoint *e;
	auto i = endpoints.begin();
	if (flags & DB_WRITABLE) {
		db = std::make_unique<Xapian::WritableDatabase>();
		if (endpoints_size != 1) {
			L_ERR(this, "ERROR: Expecting exactly one database, %d requested: %s", endpoints_size, endpoints.as_string().c_str());
		} else {
			e = &*i;
			if (e->is_local()) {
				local = true;
				wdb = Xapian::WritableDatabase(e->path, (flags & DB_SPAWN) ? Xapian::DB_CREATE_OR_OPEN : Xapian::DB_OPEN);
				if (endpoints_size == 1) read_mastery(e->path);
			}
#ifdef HAVE_REMOTE_PROTOCOL
			else {
				local = false;
				// Writable remote databases do not have a local fallback
				int port = (e->port == XAPIAND_BINARY_SERVERPORT) ? XAPIAND_BINARY_PROXY : e->port;
				wdb = Xapian::Remote::open_writable(e->host, port, 0, 10000, e->path);
			}
#endif
			db->add_database(wdb);
		}
	} else {
		for (db = std::make_unique<Xapian::Database>(); i != endpoints.end(); ++i) {
			e = &*i;
			if (e->is_local()) {
				local = true;
				try {
					rdb = Xapian::Database(e->path, Xapian::DB_OPEN);
					if (endpoints_size == 1) read_mastery(e->path);
				} catch (const Xapian::DatabaseOpeningError &err) {
					if (!(flags & DB_SPAWN))  {
						db.reset();
						throw;
					}
					wdb = Xapian::WritableDatabase(e->path, Xapian::DB_CREATE_OR_OPEN);
					rdb = Xapian::Database(e->path, Xapian::DB_OPEN);
					if (endpoints_size == 1) read_mastery(e->path);
				}
			}
#ifdef HAVE_REMOTE_PROTOCOL
			else {
				local = false;
# ifdef XAPIAN_LOCAL_DB_FALLBACK
				int port = (e->port == XAPIAND_BINARY_SERVERPORT) ? XAPIAND_BINARY_PROXY : e->port;
				rdb = Xapian::Remote::open(e->host, port, 0, 10000, e->path);
				try {
					ldb = Xapian::Database(e->path, Xapian::DB_OPEN);
					if (ldb.get_uuid() == rdb.get_uuid()) {
						L_DATABASE(this, "Endpoint %s fallback to local database!", e->as_string().c_str());
						// Handle remote endpoints and figure out if the endpoint is a local database
						rdb = Xapian::Database(e->path, Xapian::DB_OPEN);
						local = true;
						if (endpoints_size == 1) read_mastery(e->path);
					}
				} catch (const Xapian::DatabaseOpeningError &err) { }
# else
				rdb = Xapian::Remote::open(e->host, port, 0, 10000, e->path);
# endif
			}
#endif
			db->add_database(rdb);
		}
	}

	schema.setDatabase(this);
}


bool
Database::drop(const std::string &doc_id, bool _commit)
{
	if (!(flags & DB_WRITABLE)) {
		L_ERR(this, "ERROR: database is read-only");
		return false;
	}

	std::string document_id = prefixed(doc_id, DOCUMENT_ID_TERM_PREFIX);

	for (int t = 3; t >= 0; --t) {
		L_DATABASE_WRAP(this, "Deleting: -%s- t:%d", document_id.c_str(), t);
		Xapian::WritableDatabase *wdb = static_cast<Xapian::WritableDatabase *>(db.get());
		try {
			wdb->delete_document(document_id);
		} catch (const Xapian::DatabaseCorruptError &e) {
			L_ERR(this, "ERROR: %s", e.get_msg().c_str());
			return false;
		} catch (const Xapian::DatabaseError &e) {
			L_ERR(this, "ERROR: %s", e.get_msg().c_str());
			return false;
		} catch (const Xapian::Error &e) {
			L_DEBUG(this, "Inside catch drop");
			L_ERR(this, "ERROR: %s", e.get_msg().c_str());
			if (t) reopen();
			continue;
		}
		L_DATABASE_WRAP(this, "Document deleted");
		if (_commit) return commit();
		else {
			modified = true;
			return true;
		}
	}

	L_ERR(this, "ERROR: Cannot delete document: %s!", document_id.c_str());
	return false;
}


bool
Database::commit()
{
	schema.store();

	for (int t = 3; t >= 0; --t) {
		L_DATABASE_WRAP(this, "Commit: t%d", t);
		Xapian::WritableDatabase *wdb = static_cast<Xapian::WritableDatabase *>(db.get());
		try {
			wdb->commit();
		} catch (const Xapian::Error &e) {
			L_ERR(this, "ERROR: %s", e.get_msg().c_str());
			if (t) reopen();
			continue;
		}
		L_DATABASE_WRAP(this, "Commit made");
		return true;
	}

	L_ERR(this, "ERROR: Cannot do commit!");
	return false;
}


Xapian::docid
Database::patch(cJSON *patches, const std::string &_document_id, bool _commit, const std::string &ct_type, const std::string &ct_length)
{
	if (!(flags & DB_WRITABLE)) {
		L_ERR(this, "ERROR: database is read-only");
		return 0;
	}

	Xapian::Document document;
	Xapian::QueryParser queryparser;

	std::string prefix(DOCUMENT_ID_TERM_PREFIX);
	if (isupper(_document_id.at(0))) prefix += ":";
	queryparser.add_boolean_prefix(RESERVED_ID, prefix);
	auto query = queryparser.parse_query(std::string(RESERVED_ID) + ":" + _document_id);

	Xapian::Enquire enquire(*db);
	enquire.set_query(query);
	Xapian::MSet mset = enquire.get_mset(0, 1);
	Xapian::MSetIterator m = mset.begin();

	for (int t = 3; t >= 0; --t) {
		try {
			document = db->get_document(*m);
			break;
		} catch (const Xapian::InvalidArgumentError &err) {
			return 0;
		} catch (const Xapian::DocNotFoundError &err) {
			return 0;
		} catch (const Xapian::Error &err) {
			reopen();
			m = mset.begin();
		}
	}

	unique_cJSON data_json(cJSON_Parse(document.get_data().c_str()));
	if (!data_json) {
		L_ERR(this, "ERROR: JSON Before: [%s]", cJSON_GetErrorPtr());
		return 0;
	}

	if (cJSONUtils_ApplyPatches(data_json.get(), patches) == 0) {
		// Object patched
		unique_char_ptr _cprint(cJSON_PrintUnformatted(data_json.get()));
		return index(_cprint.get(), _document_id, _commit, ct_type, ct_length);
	}

	// Object no patched
	return 0;
}


void
Database::index_fields(cJSON *item, const std::string &item_name, Xapian::Document &doc, cJSON *properties, bool is_value)
{
	specification_t spc_bef = schema.specification;
	if (item->type == cJSON_Object) {
		bool offsprings = false;
		int elements = cJSON_GetArraySize(item);
		for (int i = 0; i < elements; ++i) {
			cJSON *subitem = cJSON_GetArrayItem(item, i);
			if (!is_reserved(subitem->string)) {
				std::string subitem_name = !item_name.empty() ? item_name + DB_OFFSPRING_UNION + subitem->string : subitem->string;
				cJSON *subproperties = schema.get_subproperties(properties, subitem->string, subitem);
				index_fields(subitem, subitem_name, doc, subproperties, is_value);
				offsprings = true;
			} else if (strcmp(subitem->string, RESERVED_VALUE) == 0) {
				if (schema.specification.sep_types[2] == NO_TYPE) {
					schema.set_type(subitem, item_name, properties);
				}
				if (is_value || schema.specification.index == Index::VALUE) {
					index_values(doc, subitem, item_name, properties);
				} else if (schema.specification.index == Index::TERM) {
					index_terms(doc, subitem, item_name, properties);
				} else {
					index_terms(doc, subitem, item_name, properties);
					index_values(doc, subitem, item_name, properties);
				}
			}
			schema.specification = spc_bef;
		}
		if (offsprings) {
			schema.set_type_to_object(properties);
		}
	} else {
		if (schema.specification.sep_types[2] == NO_TYPE) {
			schema.set_type(item, item_name, properties);
		}
		if (is_value || schema.specification.index == Index::VALUE) {
			index_values(doc, item, item_name, properties);
		} else if (schema.specification.index == Index::TERM) {
			index_terms(doc, item, item_name, properties);
		} else {
			index_terms(doc, item, item_name, properties);
			index_values(doc, item, item_name, properties);
		}
		schema.specification = spc_bef;
	}
}


void
Database::index_texts(Xapian::Document &doc, cJSON *texts, const std::string &name, cJSON *properties)
{
	// L_DATABASE_WRAP(this, "Texts => Field: %s\nSpecifications: %s", name.c_str(), schema.specification.to_string().c_str());
	if (!(schema.found_field || schema.specification.dynamic)) throw MSG_Error("This object is not dynamic");

	if (!schema.specification.store) return;

	if (schema.specification.bool_term) throw MSG_Error("A boolean term can not be indexed as text");

	size_t elements = 1;
	if (texts->type == cJSON_Array) {
		elements = cJSON_GetArraySize(texts);
		cJSON *value = cJSON_GetArrayItem(texts, 0);
		if (value->type == cJSON_Array) {
			throw MSG_Error("It can not be indexed array of arrays");
		}
		schema.set_type_to_array(properties);
	}

	const Xapian::WritableDatabase *wdb = static_cast<Xapian::WritableDatabase *>(db.get());
	for (size_t j = 0; j < elements; ++j) {
		cJSON *text = (texts->type == cJSON_Array) ? cJSON_GetArrayItem(texts, (int)j) : texts;
		if (text->type != cJSON_String) throw MSG_Error("Text should be string or array of strings");
		Xapian::TermGenerator term_generator;
		term_generator.set_document(doc);
		term_generator.set_stemmer(Xapian::Stem(schema.specification.language[getPos(j, schema.specification.language.size())]));
		if (schema.specification.spelling[getPos(j, schema.specification.spelling.size())]) {
			term_generator.set_database(*wdb);
			term_generator.set_flags(Xapian::TermGenerator::FLAG_SPELLING);
			term_generator.set_stemming_strategy((Xapian::TermGenerator::stem_strategy)schema.specification.analyzer[getPos(j, schema.specification.analyzer.size())]);
		}

		if (schema.specification.positions[getPos(j, schema.specification.positions.size())] > 0) {
			if (schema.specification.prefix.empty()) {
				term_generator.index_text_without_positions(text->valuestring, schema.specification.weight[getPos(j, schema.specification.weight.size())]);
			} else {
				term_generator.index_text_without_positions(text->valuestring, schema.specification.weight[getPos(j, schema.specification.weight.size())], schema.specification.prefix);
			}
			L_DATABASE_WRAP(this, "Text to Index with positions = %s: %s", name.c_str(), text->valuestring);
		} else {
			if (schema.specification.prefix.empty()) {
				term_generator.index_text(text->valuestring, schema.specification.weight[getPos(j, schema.specification.weight.size())]);
			} else {
				term_generator.index_text(text->valuestring, schema.specification.weight[getPos(j, schema.specification.weight.size())], schema.specification.prefix);
			}
			L_DATABASE_WRAP(this, "Text to Index = %s: %s", name.c_str(), text->valuestring);
		}
	}
}


void
Database::index_terms(Xapian::Document &doc, cJSON *terms, const std::string &name, cJSON *properties)
{
	// L_DATABASE_WRAP(this, "Terms => Field: %s\nSpecifications: %s", name.c_str(), schema.specification.to_string().c_str());
	if (!(schema.found_field || schema.specification.dynamic)) throw MSG_Error("This object is not dynamic");

	if (!schema.specification.store) return;

	size_t elements = 1;
	if (terms->type == cJSON_Array) {
		elements = cJSON_GetArraySize(terms);
		cJSON *value = cJSON_GetArrayItem(terms, 0);
		if (value->type == cJSON_Array) {
			throw MSG_Error("It can not be indexed array of arrays");
		}
		schema.set_type_to_array(properties);
	}

	for (size_t j = 0; j < elements; ++j) {
		cJSON *term = (terms->type == cJSON_Array) ? cJSON_GetArrayItem(terms, (int)j) : terms;
		unique_char_ptr _cprint(cJSON_Print(term));
		std::string term_v(_cprint.get());
		if (term->type == cJSON_String) {
			term_v.assign(term_v, 1, term_v.size() - 2);
			if(!schema.specification.bool_term && term_v.find(" ") != std::string::npos && schema.specification.sep_types[2] == STRING_TYPE) {
				index_texts(doc, term, name, properties);
				continue;
			}
		} else if (term->type == cJSON_Number) {
			term_v = std::to_string(term->valuedouble);
		}

		L_DATABASE_WRAP(this, "%d Term -> %s: %s", j, schema.specification.prefix.c_str(), term_v.c_str());

		term_v = Serialise::serialise(schema.specification.sep_types[2], term_v);
		if (term_v.empty()) throw MSG_Error("%s: %s can not be serialized", name.c_str(), term_v.c_str());
		if (schema.specification.sep_types[2] == STRING_TYPE && !schema.specification.bool_term) term_v = stringtolower(term_v);

		if (schema.specification.position[getPos(j, schema.specification.position.size())] >= 0) {
			std::string nameterm(prefixed(term_v, schema.specification.prefix));
			if (schema.specification.bool_term) {
				doc.add_posting(nameterm, schema.specification.position[getPos(j, schema.specification.position.size())], 0);
			} else {
				doc.add_posting(nameterm, schema.specification.position[getPos(j, schema.specification.position.size())], schema.specification.weight[getPos(j, schema.specification.weight.size())]);
			}
			L_DATABASE_WRAP(this, "Bool: %d  Posting: %s", schema.specification.bool_term, repr(nameterm).c_str());
		} else {
			std::string nameterm(prefixed(term_v, schema.specification.prefix));
			if (schema.specification.bool_term) {
				doc.add_boolean_term(nameterm);
			} else {
				doc.add_term(nameterm, schema.specification.weight[getPos(j, schema.specification.weight.size())]);
			}
			L_DATABASE_WRAP(this, "Bool: %d  Term: %s", schema.specification.bool_term, repr(nameterm).c_str());
		}
	}
}


void
Database::index_values(Xapian::Document &doc, cJSON *values, const std::string &name, cJSON *properties)
{
	// L_DATABASE_WRAP(this, "Values => Field: %s\nSpecifications: %s", name.c_str(), schema.specification.to_string().c_str());
	if (!(schema.found_field || schema.specification.dynamic)) throw MSG_Error("This object is not dynamic");

	if (!schema.specification.store) return;

	size_t elements = 1;
	if (values->type == cJSON_Array) {
		elements = cJSON_GetArraySize(values);
		cJSON *value = cJSON_GetArrayItem(values, 0);
		if (value->type == cJSON_Array) {
			throw MSG_Error("It can not be indexed array of arrays");
		}
		schema.set_type_to_array(properties);
	}

	StringList s;
	for (size_t j = 0; j < elements; ++j) {
		cJSON *value = (values->type == cJSON_Array) ? cJSON_GetArrayItem(values, (int)j) : values;
		unique_char_ptr _cprint(cJSON_Print(value));
		std::string value_v(_cprint.get());
		if (value->type == cJSON_String) {
			value_v.assign(value_v, 1, value_v.size() - 2);
		} else if (value->type == cJSON_Number) {
			value_v = std::to_string(value->valuedouble);
		}

		L_DATABASE_WRAP(this, "Name: (%s) Value: (%s)", name.c_str(), value_v.c_str());

		if (schema.specification.sep_types[2] == GEO_TYPE) {
			std::vector<range_t> ranges;
			CartesianList centroids;
			uInt64List start_end;
			EWKT_Parser::getRanges(value_v, schema.specification.accuracy[0], schema.specification.accuracy[1], ranges, centroids);
			// Index Values and looking for terms generated by accuracy.
			std::set<std::string> set_terms;
			for (auto it = ranges.begin(); it != ranges.end(); ++it) {
				start_end.push_back(it->start);
				start_end.push_back(it->end);
				int idx = -1;
				uint64_t val;
				if (it->start != it->end) {
					std::bitset<SIZE_BITS_ID> b1(it->start), b2(it->end), res;
					for (idx = SIZE_BITS_ID - 1; b1.test(idx) == b2.test(idx); --idx) {
						res.set(idx, b1.test(idx));
					}
					val = res.to_ullong();
				} else val = it->start;
				for (size_t i = 2; i < schema.specification.accuracy.size(); ++i) {
					int pos = START_POS - schema.specification.accuracy[i] * 2;
					if (idx < pos) {
						uint64_t vterm = val >> pos;
						set_terms.insert(prefixed(Serialise::trixel_id(vterm), schema.specification.acc_prefix[i - 2]));
					} else {
						break;
					}
				}
			}
			// Insert terms generated by accuracy.
			for (auto it = set_terms.begin(); it != set_terms.end(); ++it)
				doc.add_term(*it);
			s.push_back(start_end.serialise());
			s.push_back(centroids.serialise());
		} else {
			std::string value_s = Serialise::serialise(schema.specification.sep_types[2], value_v);
			if (value_s.empty()) {
				throw MSG_Error("%s: %s can not serialized", name.c_str(), value_v.c_str());
			}
			s.push_back(value_s);

			// Index terms generated by accuracy.
			switch (schema.specification.sep_types[2]) {
				case NUMERIC_TYPE: {
					for (size_t len = schema.specification.accuracy.size(), i = 0; i < len; ++i) {
						long long _v = std::stoll(value_v);
						std::string term_v = Serialise::numeric(std::to_string(_v - _v % (long long)schema.specification.accuracy[i]));
						std::string nameterm(prefixed(term_v, schema.specification.acc_prefix[i]));
						doc.add_term(nameterm);
					}
					break;
				}
				case DATE_TYPE: {
					bool findMath = value_v.find("||") != std::string::npos;
					for (size_t len = schema.specification.accuracy.size(), i = 0; i < len; ++i) {
						std::string acc(value_v);
						switch ((unitTime)schema.specification.accuracy[i]) {
							case unitTime::YEAR:
								acc += findMath ? "//y" : "||//y";
								break;
							case unitTime::MONTH:
								acc += findMath ? "//M" : "||//M";
								break;
							case unitTime::DAY:
								acc += findMath ? "//d" : "||//d";
								break;
							case unitTime::HOUR:
								acc += findMath ? "//h" : "||//h";
								break;
							case unitTime::MINUTE:
								acc += findMath ? "//m" : "||//m";
								break;
							case unitTime::SECOND:
								acc += findMath ? "//s" : "||//s";
								break;
						}
						acc.assign(Serialise::date(acc));
						std::string nameterm(prefixed(acc, schema.specification.acc_prefix[i]));
						doc.add_term(nameterm);
					}
					break;
				}
			}
		}
	}

	doc.add_value(schema.specification.slot, s.serialise());
	L_DATABASE_WRAP(this, "Slot: %u serialized: %s", schema.specification.slot, repr(s.serialise()).c_str());
}


MsgPack
Database::getMsgPack(Xapian::Document& doc, const std::string &body, const std::string& ct_type, bool& blob, msgpack::sbuffer &buffer)
{
	rapidjson::Document rdoc;

	MIMEType t = get_mimetype(ct_type);

	switch (t) {
		case MIMEType::APPLICATION_JSON:
			if (json_load(rdoc, body)) {
				blob = false;
				return MsgPack(rdoc);
			}
			throw MSG_Error("MsgPack::json_load can not do the parser");
		case MIMEType::APPLICATION_XWWW_FORM_URLENCODED:
			if (json_load(rdoc, body)) {
				doc.add_value(DB_SLOT_TYPE, JSON_TYPE);
				blob = false;
				return MsgPack(rdoc);
			} else {
				return MsgPack(std::string());
			}
		case MIMEType::APPLICATION_X_MSGPACK:
			return MsgPack(body);
		case MIMEType::UNKNOW:
			return MsgPack(std::string());
	}
}


void
Database::index_required_data(Xapian::Document& doc, std::string& term_id, const std::string& _document_id, const std::string& ct_type, const std::string& ct_length)
{
	std::size_t found = ct_type.find_last_of("/");
	std::string type(ct_type.c_str(), found);
	std::string subtype(ct_type.c_str(), found + 1, ct_type.size());

	// Saves document's id in DB_SLOT_ID
	doc.add_value(DB_SLOT_ID, _document_id);

	// Document's id is also a boolean term (otherwise it doesn't replace an existing document)
	term_id = prefixed(_document_id, DOCUMENT_ID_TERM_PREFIX);
	doc.add_boolean_term(term_id);
	L_DATABASE_WRAP(this, "Slot: 0 _id: %s  term: %s", _document_id.c_str(), document_id.c_str());

	// Indexing the content values of data.
	doc.add_value(DB_SLOT_OFFSET, DEFAULT_OFFSET);
	doc.add_value(DB_SLOT_TYPE, ct_type);
	doc.add_value(DB_SLOT_LENGTH, ct_length);

	// Index terms for content-type
	std::string term_prefix = get_prefix("content_type", DOCUMENT_CUSTOM_TERM_PREFIX, STRING_TYPE);
	doc.add_term(prefixed(ct_type, term_prefix));
	doc.add_term(prefixed(type + "/*", term_prefix));
	doc.add_term(prefixed("*/" + subtype, term_prefix));
}


Xapian::docid
Database::index(const std::string &body, const std::string &_document_id, bool _commit, const std::string &ct_type, const std::string &ct_length)
{
	if (!(flags & DB_WRITABLE)) {
		L_ERR(this, "ERROR: database is read-only");
		return 0;
	}

	if (_document_id.empty()) {
		L_ERR(this, "ERROR: Document must have an 'id'");
		return 0;
	}

	// Index required data for the document
	Xapian::Document doc;
	std::string term_id;
	index_required_data(doc, term_id, _document_id, ct_type, ct_length);

	// Create MsgPack object
	bool blob = true;
	msgpack::sbuffer buffer;
	MsgPack obj = getMsgPack(doc, body, ct_type, blob, buffer);

	L_DATABASE_WRAP(this, "Document to index: %s", buffer.data());
	doc.set_data(encode_length(buffer.size()) + std::string(buffer.data(), buffer.size()) + (blob ? body : ""));

	cJSON *json;
	unique_cJSON document(json);

	// Save a copy of schema for undo changes.
	auto _schema = schema.getSchema();
	auto _to_store = schema.getStore();

	try {
		cJSON *properties = schema.get_properties_schema();
		cJSON *subproperties = nullptr;

		schema.update_root(properties, document.get());

		specification_t spc_bef = schema.specification;

		int elements = cJSON_GetArraySize(document.get());
		for (int i = 0; i < elements; ++i) {
			cJSON *item = cJSON_GetArrayItem(document.get(), i);
			if (!is_reserved(item->string)) {
				subproperties = schema.get_subproperties(properties, item->string, item);
				index_fields(item, item->string, doc, subproperties, false);
			} else if (strcmp(item->string, RESERVED_VALUES) == 0) {
				if (item->type != cJSON_Object) {
					throw MSG_Error("%s must be an object", RESERVED_VALUES);
				}
				index_fields(item, "", doc, properties);
			} else if (strcmp(item->string, RESERVED_TEXTS) == 0) {
				for (int _size = cJSON_GetArraySize(item), i = 0; i < _size; ++i) {
					cJSON *subitem = cJSON_GetArrayItem(item, i);
					cJSON *name = cJSON_GetObjectItem(subitem, RESERVED_NAME);
					cJSON *value = cJSON_GetObjectItem(subitem, RESERVED_VALUE);
					if (value) {
						std::string name_s = name ? name->type == cJSON_String ? name->valuestring : throw MSG_Error("%s should be string", RESERVED_NAME) : std::string();
						if (name_s.empty()) {
							// Only need to update the specifications.
							schema.update_specification(subitem);
							subproperties = cJSON_CreateObject();
						} else {
							subproperties = schema.get_subproperties(properties, name_s.c_str(), subitem);
						}
						if (schema.specification.sep_types[2] == NO_TYPE) {
							schema.set_type(value, name_s, subproperties);
						}
						index_texts(doc, value, name_s, subproperties);
						schema.specification = spc_bef;
					} else {
						throw MSG_Error("Value must be defined");
					}
				}
			} else if (strcmp(item->string, RESERVED_TERMS) == 0) {
				for (int _size = cJSON_GetArraySize(item), i = 0; i < _size; ++i) {
					cJSON *subitem = cJSON_GetArrayItem(item, i);
					cJSON *name = cJSON_GetObjectItem(subitem, RESERVED_NAME);
					cJSON *value = cJSON_GetObjectItem(subitem, RESERVED_VALUE);
					if (value) {
						std::string name_s = name ? name->type == cJSON_String ? name->valuestring : throw MSG_Error("%s should be string", RESERVED_NAME) : std::string();
						if (name_s.empty()) {
							// Only need to update the specifications.
							schema.update_specification(subitem);
							subproperties = cJSON_CreateObject();
						} else {
							subproperties = schema.get_subproperties(properties, name_s.c_str(), subitem);
						}
						if (schema.specification.sep_types[2] == NO_TYPE) {
							schema.set_type(value, name_s, subproperties);
						}
						index_terms(doc, value, name_s, subproperties);
						schema.specification = spc_bef;
					} else {
						throw MSG_Error("Value must be defined");
					}
				}
			}
		}
	} catch (const std::exception &err) {
		// Back to the initial schema if there are changes.
		if (schema.getStore()) {
			schema.setSchema(std::move(_schema));
			schema.setStore(_to_store);
		}
		L_ERR(this, "ERROR: %s", err.what());
		return 0;
	}

	return replace(term_id, doc, _commit);
}


Xapian::docid
Database::replace(const std::string &document_id, const Xapian::Document &doc, bool _commit)
{
	Xapian::docid did;
	for (int t = 3; t >= 0; --t) {
		L_DATABASE_WRAP(this, "Inserting: -%s- t:%d", document_id.c_str(), t);
		Xapian::WritableDatabase *wdb = static_cast<Xapian::WritableDatabase *>(db.get());
		try {
			L_DATABASE_WRAP(this, "Doing replace_document.");
			did = wdb->replace_document(document_id, doc);
			L_DATABASE_WRAP(this, "Replace_document was done.");
		} catch (const Xapian::Error &e) {
			L_ERR(this, "ERROR: %s", e.get_msg().c_str());
			if (t) reopen();
			continue;
		}
		L_DATABASE_WRAP(this, "Document inserted");
		if (_commit) commit();
		else modified = true;
		return did;
	}

	return 0;
}


Xapian::docid
Database::replace(const Xapian::docid &did, const Xapian::Document &doc, bool _commit)
{
	for (int t = 3; t >= 0; --t) {
		L_DATABASE_WRAP(this, "Inserting: -did:%u- t:%d", did, t);
		Xapian::WritableDatabase *wdb = static_cast<Xapian::WritableDatabase *>(db.get());
		try {
			L_DATABASE_WRAP(this, "Doing replace_document.");
			wdb->replace_document(did, doc);
			L_DATABASE_WRAP(this, "Replace_document was done.");
		} catch (const Xapian::Error &e) {
			L_ERR(this, "ERROR: %s", e.get_msg().c_str());
			if (t) reopen();
			continue;
		}
		L_DATABASE_WRAP(this, "Document inserted");
		if (_commit) commit();
		else modified = true;
		return did;
	}

	return 0;
}


data_field_t
Database::get_data_field(const std::string &field_name)
{
	data_field_t res = { Xapian::BAD_VALUENO, "", NO_TYPE, std::vector<double>(), std::vector<std::string>(), false };

	if (field_name.empty()) {
		return res;
	}

	cJSON *properties = schema.get_properties_schema();

	auto fields = split_fields(field_name);
	for (auto it = fields.begin(); it != fields.end(); ++it) {
		properties = cJSON_GetObjectItem(properties, (*it).c_str());
		if (!properties) break;
	}

	// If properties exits then the specifications too.
	if (properties) {
		cJSON *_aux = cJSON_GetObjectItem(properties, RESERVED_TYPE);
		res.type = cJSON_GetArrayItem(_aux, 2)->valueint;
		if (res.type == NO_TYPE) return res;

		_aux = cJSON_GetObjectItem(properties, RESERVED_SLOT);
		unique_char_ptr _cprint(cJSON_Print(_aux));
		res.slot = static_cast<unsigned int>(std::stoul(_cprint.get()));

		_aux = cJSON_GetObjectItem(properties, RESERVED_PREFIX);
		res.prefix = _aux->valuestring;

		_aux = cJSON_GetObjectItem(properties, RESERVED_BOOL_TERM);
		res.bool_term = _aux->type == cJSON_False ? false : true;

		// Strings and booleans do not have accuracy.
		if (res.type != STRING_TYPE && res.type != BOOLEAN_TYPE) {
			_aux = cJSON_GetObjectItem(properties, RESERVED_ACCURACY);
			int elements = cJSON_GetArraySize(_aux);
			for (int i = 0; i < elements; ++i) {
				cJSON *acc = cJSON_GetArrayItem(_aux, i);
				res.accuracy.push_back(acc->valuedouble);
			}

			_aux = cJSON_GetObjectItem(properties, RESERVED_ACC_PREFIX);
			elements = cJSON_GetArraySize(_aux);
			for (int i = 0; i < elements; ++i) {
				cJSON *acc = cJSON_GetArrayItem(_aux, i);
				res.acc_prefix.push_back(acc->valuestring);
			}
		}
	}

	return res;
}


data_field_t
Database::get_slot_field(const std::string &field_name)
{
	data_field_t res = { Xapian::BAD_VALUENO, "", NO_TYPE, std::vector<double>(), std::vector<std::string>(), false };

	if (field_name.empty()) {
		return res;
	}

	cJSON *properties = schema.get_properties_schema();

	auto fields = split_fields(field_name);
	for (auto it = fields.begin(); it != fields.end(); ++it) {
		properties = cJSON_GetObjectItem(properties, (*it).c_str());
		if (!properties) break;
	}

	if (properties) {
		cJSON *_aux = cJSON_GetObjectItem(properties, RESERVED_SLOT);
		unique_char_ptr _cprint(cJSON_Print(_aux));
		res.slot = static_cast<unsigned int>(std::stoul(_cprint.get()));

		_aux = cJSON_GetObjectItem(properties, RESERVED_TYPE);
		res.type = cJSON_GetArrayItem(_aux, 2)->valueint;
	}

	return res;
}


Database::search_t
Database::search(const query_field_t &e)
{
	search_t srch_resul;

	Xapian::Query queryQ;
	Xapian::Query queryP;
	Xapian::Query queryT;
	Xapian::Query queryF;
	std::vector<std::string> sug_query;
	search_t srch;
	bool first = true;

	L_DEBUG(this, "e.query size: %d  Spelling: %d Synonyms: %d", e.query.size(), e.spelling, e.synonyms);
	auto lit = e.language.begin();
	std::string lan;
	unsigned int flags = Xapian::QueryParser::FLAG_DEFAULT | Xapian::QueryParser::FLAG_WILDCARD | Xapian::QueryParser::FLAG_PURE_NOT;
	if (e.spelling) flags |= Xapian::QueryParser::FLAG_SPELLING_CORRECTION;
	if (e.synonyms) flags |= Xapian::QueryParser::FLAG_SYNONYM;
	for (auto qit = e.query.begin(); qit != e.query.end(); ++qit) {
		if (lit != e.language.end()) {
			lan = *lit++;
		}
		srch = _search(*qit, flags, true, lan);
		if (first) {
			queryQ = srch.query;
			first = false;
		} else {
			queryQ =  Xapian::Query(Xapian::Query::OP_AND, queryQ, srch.query);
		}
		sug_query.push_back(srch.suggested_query.back());
		srch_resul.nfps.insert(srch_resul.nfps.end(), std::make_move_iterator(srch.nfps.begin()), std::make_move_iterator(srch.nfps.end()));
		srch_resul.dfps.insert(srch_resul.dfps.end(), std::make_move_iterator(srch.dfps.begin()), std::make_move_iterator(srch.dfps.end()));
		srch_resul.gfps.insert(srch_resul.gfps.end(), std::make_move_iterator(srch.gfps.begin()), std::make_move_iterator(srch.gfps.end()));
		srch_resul.bfps.insert(srch_resul.bfps.end(), std::make_move_iterator(srch.bfps.begin()), std::make_move_iterator(srch.bfps.end()));
	}
	L_DEBUG(this, "e.query: %s", queryQ.get_description().c_str());

	L_DEBUG(this, "e.partial size: %d", e.partial.size());
	flags = Xapian::QueryParser::FLAG_PARTIAL;
	if (e.spelling) flags |= Xapian::QueryParser::FLAG_SPELLING_CORRECTION;
	if (e.synonyms) flags |= Xapian::QueryParser::FLAG_SYNONYM;
	first = true;
	for (auto pit = e.partial.begin(); pit != e.partial.end(); ++pit) {
		srch = _search(*pit, flags, false, "");
		if (first) {
			queryP = srch.query;
			first = false;
		} else {
			queryP = Xapian::Query(Xapian::Query::OP_AND_MAYBE , queryP, srch.query);
		}
		sug_query.push_back(srch.suggested_query.back());
		srch_resul.nfps.insert(srch_resul.nfps.end(), std::make_move_iterator(srch.nfps.begin()), std::make_move_iterator(srch.nfps.end()));
		srch_resul.dfps.insert(srch_resul.dfps.end(), std::make_move_iterator(srch.dfps.begin()), std::make_move_iterator(srch.dfps.end()));
		srch_resul.gfps.insert(srch_resul.gfps.end(), std::make_move_iterator(srch.gfps.begin()), std::make_move_iterator(srch.gfps.end()));
		srch_resul.bfps.insert(srch_resul.bfps.end(), std::make_move_iterator(srch.bfps.begin()), std::make_move_iterator(srch.bfps.end()));
	}
	L_DEBUG(this, "e.partial: %s", queryP.get_description().c_str());


	L_DEBUG(this, "e.terms size: %d", e.terms.size());
	flags = Xapian::QueryParser::FLAG_BOOLEAN | Xapian::QueryParser::FLAG_PURE_NOT;
	if (e.spelling) flags |= Xapian::QueryParser::FLAG_SPELLING_CORRECTION;
	if (e.synonyms) flags |= Xapian::QueryParser::FLAG_SYNONYM;
	first = true;
	for (auto tit = e.terms.begin(); tit != e.terms.end(); ++tit) {
		srch = _search(*tit, flags, false, "");
		if (first) {
			queryT = srch.query;
			first = false;
		} else {
			queryT = Xapian::Query(Xapian::Query::OP_AND, queryT, srch.query);
		}
		sug_query.push_back(srch.suggested_query.back());
		srch_resul.nfps.insert(srch_resul.nfps.end(), std::make_move_iterator(srch.nfps.begin()), std::make_move_iterator(srch.nfps.end()));
		srch_resul.dfps.insert(srch_resul.dfps.end(), std::make_move_iterator(srch.dfps.begin()), std::make_move_iterator(srch.dfps.end()));
		srch_resul.gfps.insert(srch_resul.gfps.end(), std::make_move_iterator(srch.gfps.begin()), std::make_move_iterator(srch.gfps.end()));
		srch_resul.bfps.insert(srch_resul.bfps.end(), std::make_move_iterator(srch.bfps.begin()), std::make_move_iterator(srch.bfps.end()));
	}
	L_DEBUG(this, "e.terms: %s", repr(queryT.get_description()).c_str());

	first = true;
	if (!e.query.empty()) {
		queryF = queryQ;
		first = false;
	}
	if (!e.partial.empty()) {
		if (first) {
			queryF = queryP;
			first = false;
		} else {
			queryF = Xapian::Query(Xapian::Query::OP_AND, queryF, queryP);
		}
	}
	if (!e.terms.empty()) {
		if (first) {
			queryF = queryT;
		} else {
			queryF = Xapian::Query(Xapian::Query::OP_AND, queryF, queryT);
		}
	}
	srch_resul.query = queryF;
	srch_resul.suggested_query = sug_query;

	return srch_resul;
}


Database::search_t
Database::_search(const std::string &query, unsigned int flags, bool text, const std::string &lan)
{
	search_t srch;

	if (query.compare("*") == 0) {
		srch.query = Xapian::Query::MatchAll;
		srch.suggested_query.push_back("");
		return srch;
	}

	size_t size_match = 0;
	bool first_time = true, first_timeR = true;
	std::string querystring;
	Xapian::Query queryRange;
	Xapian::QueryParser queryparser;
	queryparser.set_database(*db);

	if (text) {
		queryparser.set_stemming_strategy(queryparser.STEM_SOME);
		lan.empty() ? queryparser.set_stemmer(Xapian::Stem(default_spc.language[0])) : queryparser.set_stemmer(Xapian::Stem(lan));
	}

	std::unordered_set<std::string> added_prefixes;
	std::unique_ptr<NumericFieldProcessor> nfp;
	std::unique_ptr<DateFieldProcessor> dfp;
	std::unique_ptr<GeoFieldProcessor> gfp;
	std::unique_ptr<BooleanFieldProcessor> bfp;

	std::sregex_iterator next(query.begin(), query.end(), find_field_re, std::regex_constants::match_continuous);
	std::sregex_iterator end;
	while (next != end) {
		std::string field(next->str(0));
		size_match += next->length(0);
		std::string field_name_dot(next->str(1));
		std::string field_name(next->str(2));
		std::string field_value(next->str(3));
		data_field_t field_t = get_data_field(field_name);

		// Auxiliary variables.
		std::vector<std::string> prefixes;
		std::vector<std::string>::const_iterator it;
		std::vector<range_t> ranges;
		std::vector<range_t>::const_iterator rit;
		CartesianList centroids;
		std::string filter_term, start, end, prefix;

		std::smatch m;
		if (std::regex_match(field_value, m, find_range_re)) {
			// If this field is not indexed as value, not process this query.
			if (field_t.slot == Xapian::BAD_VALUENO) {
				++next;
				continue;
			}

			switch (field_t.type) {
				case NUMERIC_TYPE:
					start = m.str(1);
					end = m.str(2);
					GenerateTerms::numeric(filter_term, start, end, field_t.accuracy, field_t.acc_prefix, prefixes);
					queryRange = MultipleValueRange::getQuery(field_t.slot, NUMERIC_TYPE, start, end, field_name);
					if (!filter_term.empty()) {
						for (it = prefixes.begin(); it != prefixes.end(); ++it) {
							// Xapian does not allow repeat prefixes.
							if (added_prefixes.insert(*it).second) {
								nfp = std::make_unique<NumericFieldProcessor>(*it);
								queryparser.add_prefix(*it, nfp.get());
								srch.nfps.push_back(std::move(nfp));
							}
						}
						queryRange = Xapian::Query(Xapian::Query::OP_AND, queryparser.parse_query(filter_term, flags), queryRange);
					}
					break;
				case STRING_TYPE:
					start = m.str(1);
					end = m.str(2);
					queryRange = MultipleValueRange::getQuery(field_t.slot, STRING_TYPE, start, end, field_name);
					break;
				case DATE_TYPE:
					start = m.str(1);
					end = m.str(2);
					GenerateTerms::date(filter_term, start, end, field_t.accuracy, field_t.acc_prefix, prefixes);
					queryRange = MultipleValueRange::getQuery(field_t.slot, DATE_TYPE, start, end, field_name);
					if (!filter_term.empty()) {
						for (it = prefixes.begin(); it != prefixes.end(); ++it) {
							// Xapian does not allow repeat prefixes.
							if (added_prefixes.insert(*it).second) {
								dfp = std::make_unique<DateFieldProcessor>(*it);
								queryparser.add_prefix(*it, dfp.get());
								srch.dfps.push_back(std::move(dfp));
							}
						}
						queryRange = Xapian::Query(Xapian::Query::OP_AND, queryparser.parse_query(filter_term, flags), queryRange);
					}
					break;
				case GEO_TYPE:
					// Validate special case.
					if (field_value.compare("..") == 0) {
						queryRange = Xapian::Query::MatchAll;
						break;
					}

					// The format is: "..EWKT". We always delete double quotes and .. -> EWKT
					field_value.assign(field_value.c_str(), 3, field_value.size() - 4);
					EWKT_Parser::getRanges(field_value, field_t.accuracy[0], field_t.accuracy[1], ranges, centroids);

					queryRange = GeoSpatialRange::getQuery(field_t.slot, ranges, centroids);
					GenerateTerms::geo(filter_term, ranges, field_t.accuracy, field_t.acc_prefix, prefixes);
					if (!filter_term.empty()) {
						// Xapian does not allow repeat prefixes.
						for (it = prefixes.begin(); it != prefixes.end(); ++it) {
							// Xapian does not allow repeat prefixes.
							if (added_prefixes.insert(*it).second) {
								gfp = std::make_unique<GeoFieldProcessor>(*it);
								queryparser.add_prefix(*it, gfp.get());
								srch.gfps.push_back(std::move(gfp));
							}
						}
						queryRange = Xapian::Query(Xapian::Query::OP_AND, queryparser.parse_query(filter_term, flags), queryRange);
					}
					break;
			}

			// Concatenate with OR all the ranges queries.
			if (first_timeR) {
				srch.query = queryRange;
				first_timeR = false;
			} else srch.query = Xapian::Query(Xapian::Query::OP_OR, srch.query, queryRange);
		} else {
			// If the field has not been indexed as a term, not process this query.
			if (!field_name.empty() && field_t.prefix.empty()) {
				++next;
				continue;
			}

			switch (field_t.type) {
				case NUMERIC_TYPE:
					// Xapian does not allow repeat prefixes.
					if (added_prefixes.insert(field_t.prefix).second) {
						nfp = std::make_unique<NumericFieldProcessor>(field_t.prefix);
						field_t.bool_term ? queryparser.add_boolean_prefix(field_name, nfp.get()) : queryparser.add_prefix(field_name, nfp.get());
						srch.nfps.push_back(std::move(nfp));
					}
					if (field_value.at(0) == '-') {
						field_value.at(0) = '_';
						field = field_name_dot + field_value;
					}
					break;
				case STRING_TYPE:
					// Xapian does not allow repeat prefixes.
					if (added_prefixes.insert(field_t.prefix).second) {
						field_t.bool_term ? queryparser.add_boolean_prefix(field_name, field_t.prefix) : queryparser.add_prefix(field_name, field_t.prefix);
					}
					break;
				case DATE_TYPE:
					// If there are double quotes, they are deleted: "date" -> date
					if (field_value.at(0) == '"') field_value.assign(field_value, 1, field_value.size() - 2);
					try {
						// Always pass to timestamp.
						field_value = std::to_string(Datetime::timestamp(field_value));
						// Xapian does not allow repeat prefixes.
						if (added_prefixes.insert(field_t.prefix).second) {
							dfp = std::make_unique<DateFieldProcessor>(field_t.prefix);
							field_t.bool_term ? queryparser.add_boolean_prefix(field_name, dfp.get()) : queryparser.add_prefix(field_name, dfp.get());
							srch.dfps.push_back(std::move(dfp));
						}
						if (field_value.at(0) == '-') field_value.at(0) = '_';
						field = field_name_dot + field_value;
					} catch (const std::exception &ex) {
						throw Xapian::QueryParserError("Didn't understand date field name's specification: '" + field_name + "'");
					}
					break;
				case GEO_TYPE:
					// Delete double quotes (always): "EWKT" -> EWKT
					field_value.assign(field_value, 1, field_value.size() - 2);
					field_value.assign(Serialise::ewkt(field_value));

					// If the region for search is empty, not process this query.
					if (field_value.empty()) {
						++next;
						continue;
					}

					field = field_name_dot + field_value;

					// Xapian does not allow repeat prefixes.
					if (added_prefixes.insert(field_t.prefix).second) {
						field_t.bool_term ? queryparser.add_boolean_prefix(field_name, field_t.prefix) : queryparser.add_prefix(field_name, field_t.prefix);
					}
					break;
				case BOOLEAN_TYPE:
					// Xapian does not allow repeat prefixes.
					if (added_prefixes.insert(field_t.prefix).second) {
						bfp = std::make_unique<BooleanFieldProcessor>(field_t.prefix);
						field_t.bool_term ? queryparser.add_boolean_prefix(field_name, bfp.get()) : queryparser.add_prefix(field_name, bfp.get());
						srch.bfps.push_back(std::move(bfp));
					}
					break;
			}

			// Concatenate with OR all the queries.
			if (first_time) {
				querystring = field;
				first_time = false;
			} else querystring += " OR " + field;
		}

		++next;
	}

	if (size_match != query.size()) {
		throw Xapian::QueryParserError("Query '" + query + "' contains errors.\n" );
	}

	switch (first_time << 1 | first_timeR) {
		case 0:
			try {
				srch.query = Xapian::Query(Xapian::Query::OP_OR, queryparser.parse_query(querystring, flags), srch.query);
				srch.suggested_query.push_back(queryparser.get_corrected_query_string());
			} catch (const Xapian::QueryParserError &er) {
				L_ERR(this, "ERROR: %s", er.get_msg().c_str());
				reopen();
				srch.query = Xapian::Query(Xapian::Query::OP_OR, queryparser.parse_query(querystring, flags), srch.query);
				srch.suggested_query.push_back(queryparser.get_corrected_query_string());
			}
			break;
		case 1:
			try {
				srch.query = queryparser.parse_query(querystring, flags);
				srch.suggested_query.push_back(queryparser.get_corrected_query_string());
			} catch (const Xapian::QueryParserError &er) {
				L_ERR(this, "ERROR: %s", er.get_msg().c_str());
				reopen();
				queryparser.set_database(*db);
				srch.query = queryparser.parse_query(querystring, flags);
				srch.suggested_query.push_back(queryparser.get_corrected_query_string());
			}
			break;
		case 2:
			srch.suggested_query.push_back("");
			break;
		case 3:
			srch.query = Xapian::Query::MatchNothing;
			srch.suggested_query.push_back("");
			break;
	}

	return srch;
}


void
Database::get_similar(bool is_fuzzy, Xapian::Enquire &enquire, Xapian::Query &query, const similar_field_t *similar)
{
	Xapian::RSet rset;

	for (int t = 3; t >= 0; --t) {
		try {
			Xapian::Enquire renquire = get_enquire(query, Xapian::BAD_VALUENO, 1, nullptr, nullptr, nullptr, nullptr, nullptr);
			Xapian::MSet mset = renquire.get_mset(0, similar->n_rset);
			for (auto m = mset.begin(); m != mset.end(); ++m) {
				rset.add_document(*m);
			}
		} catch (const Xapian::Error &er) {
			L_ERR(this, "ERROR: %s", er.get_msg().c_str());
			if (t) reopen();
			continue;
		}

		std::vector<std::string> prefixes;
		for (auto it = similar->type.begin(); it != similar->type.end(); ++it) {
			prefixes.push_back(DOCUMENT_CUSTOM_TERM_PREFIX + Unserialise::type(*it));
		}

		for(auto it = similar->field.begin(); it != similar->field.end(); ++it) {
			data_field_t field_t = get_data_field(*it);
			prefixes.push_back(field_t.prefix);
		}

		ExpandDeciderFilterPrefixes efp(prefixes);
		Xapian::ESet eset = enquire.get_eset(similar->n_eset, rset, &efp);

		if (is_fuzzy) {
			query = Xapian::Query(Xapian::Query::OP_OR, query, Xapian::Query(Xapian::Query::OP_ELITE_SET, eset.begin(), eset.end(), similar->n_term));
		} else {
			query = Xapian::Query(Xapian::Query::OP_ELITE_SET, eset.begin(), eset.end(), similar->n_term);
		}
		return;
	}
}


Xapian::Enquire
Database::get_enquire(Xapian::Query &query, const Xapian::valueno &collapse_key, const Xapian::valueno &collapse_max,
		Multi_MultiValueKeyMaker *sorter, std::vector<std::pair<std::string, std::unique_ptr<MultiValueCountMatchSpy>>> *spies,
		const similar_field_t *nearest, const similar_field_t *fuzzy, const std::vector<std::string> *facets)
{
	std::string field;
	std::unique_ptr<MultiValueCountMatchSpy> spy;
	Xapian::Enquire enquire(*db);

	if (nearest) get_similar(false, enquire, query, nearest);

	if (fuzzy) get_similar(true, enquire, query, fuzzy);

	enquire.set_query(query);

	if (sorter) enquire.set_sort_by_key_then_relevance(sorter, false);

	if (spies) {
		if (!facets->empty()) {
			for (auto fit = facets->begin(); fit != facets->end(); ++fit) {
				data_field_t field_t = get_slot_field(*fit);
				if (field_t.type != NO_TYPE) {
					spy = std::make_unique<MultiValueCountMatchSpy>(get_slot(*fit), field_t.type == GEO_TYPE);
					enquire.add_matchspy(spy.get());
					L_ERR(this, "added spy de -%s-", (*fit).c_str());
					spies->push_back(std::make_pair(*fit, std::move(spy)));
				}
			}
		}
	}

	if (collapse_key != Xapian::BAD_VALUENO) {
		enquire.set_collapse_key(collapse_key, collapse_max);
	}

	return enquire;
}


int
Database::get_mset(const query_field_t &e, Xapian::MSet &mset, std::vector<std::pair<std::string, std::unique_ptr<MultiValueCountMatchSpy>>> &spies,
		std::vector<std::string> &suggestions, int offset)
{
	unsigned int doccount = db->get_doccount();
	unsigned int check_at_least = std::max(std::min(doccount, e.check_at_least), 0u);
	Xapian::valueno collapse_key;

	// Get the collapse key to use for queries.
	if (!e.collapse.empty()) {
		data_field_t field_t = get_slot_field(e.collapse);
		collapse_key = field_t.slot;
	} else {
		collapse_key = Xapian::BAD_VALUENO;
	}

	Multi_MultiValueKeyMaker sorter_obj;
	Multi_MultiValueKeyMaker *sorter = nullptr;
	if (!e.sort.empty()) {
		sorter = &sorter_obj;
		for (auto oit = e.sort.begin(); oit != e.sort.end(); ++oit) {
			std::string field, value;
			size_t pos = oit->find(":");
			if (pos != std::string::npos) {
				field = oit->substr(0, pos);
				value = oit->substr(pos + 1);
			} else field = *oit;

			if (startswith(field, "-")) {
				field.assign(field, 1, field.size() - 1);
				data_field_t field_t = get_slot_field(field);
				if (field_t.type != NO_TYPE) sorter->add_value(field_t.slot, field_t.type, value, true);
			} else if (startswith(field, "+")) {
				field.assign(field, 1, field.size() - 1);
				data_field_t field_t = get_slot_field(field);
				if (field_t.type != NO_TYPE) sorter->add_value(field_t.slot, field_t.type, value);
			} else {
				data_field_t field_t = get_slot_field(field);
				if (field_t.type != NO_TYPE) sorter->add_value(field_t.slot, field_t.type, value);
			}
		}
	}

	for (int t = 3; t >= 0; --t) {
		try {
			search_t srch = search(e);
			Xapian::Enquire enquire = get_enquire(srch.query, collapse_key, e.collapse_max, sorter, &spies, e.is_nearest ? &e.nearest : nullptr, e.is_fuzzy ? &e.fuzzy : nullptr, &e.facets);
			suggestions = srch.suggested_query;
			mset = enquire.get_mset(e.offset + offset, e.limit - offset, check_at_least);
		} catch (const Xapian::DatabaseModifiedError &er) {
			L_ERR(this, "ERROR: %s", er.get_msg().c_str());
			if (t) reopen();
			continue;
		} catch (const Xapian::NetworkError &er) {
			L_ERR(this, "ERROR: %s", er.get_msg().c_str());
			if (t) reopen();
			continue;
		} catch (const Xapian::Error &er) {
			L_ERR(this, "ERROR: %s", er.get_msg().c_str());
			return 2;
		} catch (const std::exception &er) {
			L_DATABASE_WRAP(this, "ERROR: %s", er.what());
			return 1;
		}

		return 0;
	}

	L_ERR(this, "ERROR: The search was not performed!");
	return 2;
}


bool
Database::get_metadata(const std::string &key, std::string &value)
{
	for (int t = 3; t >= 0; --t) {
		try {
			value = db->get_metadata(key);
		} catch (const Xapian::Error &e) {
			L_ERR(this, "ERROR: %s", e.get_msg().c_str());
			if (t) reopen();
			continue;
		}
		L_DATABASE_WRAP(this, "get_metadata was done");
		return value.empty() ? false : true;
	}

	L_ERR(this, "ERROR: get_metadata can not be done!");
	return false;
}


bool
Database::set_metadata(const std::string &key, const std::string &value, bool _commit)
{
	for (int t = 3; t >= 0; --t) {
		Xapian::WritableDatabase *wdb = static_cast<Xapian::WritableDatabase *>(db.get());
		try {
			wdb->set_metadata(key, value);
		} catch (const Xapian::Error &e) {
			L_ERR(this, "ERROR: %s", e.get_msg().c_str());
			if (t) reopen();
			continue;
		}
		L_DATABASE_WRAP(this, "set_metadata was done");
		return (_commit) ? commit() : true;
	}

	L_ERR(this, "ERROR: set_metadata can not be done!");
	return false;
}


bool
Database::get_document(const Xapian::docid &did, Xapian::Document &doc)
{
	for (int t = 3; t >= 0; --t) {
		try {
			doc = db->get_document(did);
		} catch (const Xapian::Error &e) {
			L_ERR(this, "ERROR: %s", e.get_msg().c_str());
			if (t) reopen();
			continue;
		}
		return true;
	}

	return false;
}


unique_cJSON
Database::get_stats_database()
{
	unique_cJSON database(cJSON_CreateObject());
	unsigned int doccount = db->get_doccount();
	unsigned int lastdocid = db->get_lastdocid();
	cJSON_AddStringToObject(database.get(), "uuid", db->get_uuid().c_str());
	cJSON_AddNumberToObject(database.get(), "doc_count", doccount);
	cJSON_AddNumberToObject(database.get(), "last_id", lastdocid);
	cJSON_AddNumberToObject(database.get(), "doc_del", lastdocid - doccount);
	cJSON_AddNumberToObject(database.get(), "av_length", db->get_avlength());
	cJSON_AddNumberToObject(database.get(), "doc_len_lower", db->get_doclength_lower_bound());
	cJSON_AddNumberToObject(database.get(), "doc_len_upper", db->get_doclength_upper_bound());
	db->has_positions() ? cJSON_AddTrueToObject(database.get(), "has_positions") : cJSON_AddFalseToObject(database.get(), "has_positions");
	return std::move(database);
}


unique_cJSON
Database::get_stats_docs(const std::string &document_id)
{
	unique_cJSON document(cJSON_CreateObject());

	Xapian::Document doc;
	Xapian::QueryParser queryparser;

	std::string prefix(DOCUMENT_ID_TERM_PREFIX);
	if (isupper(document_id.at(0))) prefix += ":";
	queryparser.add_boolean_prefix(RESERVED_ID, prefix);
	Xapian::Query query = queryparser.parse_query(std::string(RESERVED_ID) + ":" + document_id);

	Xapian::Enquire enquire(*db);
	enquire.set_query(query);
	Xapian::MSet mset = enquire.get_mset(0, 1);
	Xapian::MSetIterator m = mset.begin();

	for (int t = 3; t >= 0; --t) {
		try {
			doc = db->get_document(*m);
			break;
		} catch (Xapian::InvalidArgumentError &err) {
			cJSON_AddStringToObject(document.get(), RESERVED_ID, document_id.c_str());
			cJSON_AddStringToObject(document.get(), "_error",  "Document not found");
			return std::move(document);
		} catch (Xapian::DocNotFoundError &err) {
			cJSON_AddStringToObject(document.get(), RESERVED_ID, document_id.c_str());
			cJSON_AddStringToObject(document.get(), "_error",  "Document not found");
			return std::move(document);
		} catch (const Xapian::Error &err) {
			reopen();
			m = mset.begin();
		}
	}

	cJSON_AddStringToObject(document.get(), RESERVED_ID, document_id.c_str());
	cJSON_AddItemToObject(document.get(), RESERVED_DATA, cJSON_Parse(doc.get_data().c_str()));
	cJSON_AddNumberToObject(document.get(), "number_terms", doc.termlist_count());
	std::string terms;
	for (auto it = doc.termlist_begin(); it != doc.termlist_end(); ++it) {
		terms = terms + repr(*it) + " ";
	}
	cJSON_AddStringToObject(document.get(), RESERVED_TERMS, terms.c_str());
	cJSON_AddNumberToObject(document.get(), "number_values", doc.values_count());
	std::string values;
	for (auto iv = doc.values_begin(); iv != doc.values_end(); ++iv) {
		values = values + std::to_string(iv.get_valueno()) + ":" + repr(*iv) + " ";
	}
	cJSON_AddStringToObject(document.get(), RESERVED_VALUES, values.c_str());

	return std::move(document);
}


DatabaseQueue::DatabaseQueue()
	: state(replica_state::REPLICA_FREE),
	  persistent(false),
	  count(0) { }


DatabaseQueue::DatabaseQueue(DatabaseQueue&& q)
{
	std::lock_guard<std::mutex> lk(q._mutex);
	_items_queue = std::move(q._items_queue);
	_limit = std::move(q._limit);
	state = std::move(q.state);
	persistent = std::move(q.persistent);
	count = std::move(q.count);
	weak_database_pool = std::move(q.weak_database_pool);
}


DatabaseQueue::~DatabaseQueue()
{
	assert(size() == count);
}


bool
DatabaseQueue::inc_count(int max)
{
	std::unique_lock<std::mutex> lk(_mutex);

	if (count == 0) {
		if (auto database_pool = weak_database_pool.lock()) {
			for (auto& endpoint : endpoints) {
				database_pool->add_endpoint_queue(endpoint, shared_from_this());
			}
		}
	}

	if (max == -1 || count < static_cast<size_t>(max)) {
		++count;
		return true;
	}

	return false;
}


bool
DatabaseQueue::dec_count()
{
	std::unique_lock<std::mutex> lk(_mutex);

	assert(count > 0);

	if (count > 0) {
		--count;
		return true;
	}

	if (auto database_pool = weak_database_pool.lock()) {
		for (auto& endpoint : endpoints) {
			database_pool->drop_endpoint_queue(endpoint, shared_from_this());
		}
	}

	return false;
}


DatabasePool::DatabasePool(size_t max_size)
	: finished(false),
	  databases(max_size),
	  writable_databases(max_size) { }


DatabasePool::~DatabasePool()
{
	finish();
}


void
DatabasePool::add_endpoint_queue(const Endpoint& endpoint, const std::shared_ptr<DatabaseQueue>& queue)
{
	size_t hash = endpoint.hash();
	auto& queues_set = queues[hash];
	queues_set.insert(queue);
}


void
DatabasePool::drop_endpoint_queue(const Endpoint& endpoint, const std::shared_ptr<DatabaseQueue>& queue)
{
	size_t hash = endpoint.hash();
	auto& queues_set = queues[hash];
	queues_set.erase(queue);

	if (queues_set.empty()) {
		queues.erase(hash);
	}
}


long long
DatabasePool::get_mastery_level(const std::string &dir)
{
	Endpoints endpoints;
	endpoints.insert(Endpoint(dir));

	std::shared_ptr<Database> database;
	if (checkout(database, endpoints, 0)) {
		long long mastery_level = database->mastery_level;
		checkin(database);
		return mastery_level;
	}

	return read_mastery(dir, false);
}


void
DatabasePool::finish()
{
	finished = true;
}


bool DatabasePool::checkout(std::shared_ptr<Database>& database, const Endpoints& endpoints, int flags)
{
	bool writable = flags & DB_WRITABLE;
	bool persistent = flags & DB_PERSISTENT;
	bool initref = flags & DB_INIT_REF;
	bool replication = flags & DB_REPLICATION;

	L_DATABASE_BEGIN(this, "++ CHECKING OUT DB %s(%s) [%lx]...", writable ? "w" : "r", endpoints.as_string().c_str(), (unsigned long)database.get());

	assert(!database);

	std::unique_lock<std::mutex> lk(qmtx);

	if (!finished) {
		size_t hash = endpoints.hash();

		std::shared_ptr<DatabaseQueue> queue;
		if (writable) {
			queue = writable_databases[hash];
		} else {
			queue = databases[hash];
		}

		auto old_state = queue->state;

		if (replication) {
			switch (queue->state) {
				case DatabaseQueue::replica_state::REPLICA_FREE:
					queue->state = DatabaseQueue::replica_state::REPLICA_LOCK;
					break;
				case DatabaseQueue::replica_state::REPLICA_LOCK:
				case DatabaseQueue::replica_state::REPLICA_SWITCH:
					L_REPLICATION(this, "A replication task is already waiting");
					L_DATABASE_END(this, "!! ABORTED CHECKOUT DB (%s)!", endpoints.as_string().c_str());
					return false;
			}
		} else {
			if (queue->state == DatabaseQueue::replica_state::REPLICA_SWITCH) {
				queue->switch_cond.wait(lk);
			}
		}

		bool old_persistent = queue->persistent;
		queue->persistent = persistent;

		if (!queue->pop(database, 0)) {
			// Increment so other threads don't delete the queue
			if (queue->inc_count(writable ? 1 : -1)) {
				lk.unlock();
				try {
					database = std::make_shared<Database>(queue, endpoints, flags);

					if (writable && initref && endpoints.size() == 1) {
						init_ref(endpoints);
					}

				} catch (const Xapian::DatabaseOpeningError &err) {
				} catch (const Xapian::Error &err) {
					L_ERR(this, "ERROR: %s", err.get_msg().c_str());
				}
				lk.lock();
				queue->dec_count();  // Decrement, count should have been already incremented if Database was created
			} else {
				// Lock until a database is available if it can't get one.
				lk.unlock();
				int s = queue->pop(database);
				lk.lock();
				if (!s) {
					L_ERR(this, "ERROR: Database is not available. Writable: %d", writable);
				}
			}
		}
		if (!database) {
			queue->state = old_state;
			queue->persistent = old_persistent;
			if (queue->count == 0) {
				// There was an error and the queue ended up being empty, remove it!
				databases.erase(hash);
			}
		}
	}

	lk.unlock();

	if (database) {
		database->modified = false;
	} else {
		L_DATABASE_END(this, "!! FAILED CHECKOUT DB (%s)!", endpoints.as_string().c_str());
		return false;
	}

	if (!writable && duration_cast<seconds>(system_clock::now() -  database->access_time).count() >= DATABASE_UPDATE_TIME) {
		database->reopen();
		L_DATABASE(this, "== REOPEN DB %s(%s) [%lx]", (database->flags & DB_WRITABLE) ? "w" : "r", database->endpoints.as_string().c_str(), (unsigned long)database.get());
	}

#ifdef HAVE_REMOTE_PROTOCOL
	if (database->local) {
		database->checkout_revision = database->db->get_revision_info();
	}
#endif
	L_DATABASE_END(this, "++ CHECKED OUT DB %s(%s), %s at rev:%s %lx", writable ? "w" : "r", endpoints.as_string().c_str(), database->local ? "local" : "remote", repr(database->checkout_revision, false).c_str(), (unsigned long)database.get());
	return true;
}


void
DatabasePool::checkin(std::shared_ptr<Database>& database)
{
	L_DATABASE_BEGIN(this, "-- CHECKING IN DB %s(%s) [%lx]...", (database->flags & DB_WRITABLE) ? "w" : "r", database->endpoints.as_string().c_str(), (unsigned long)database.get());

	assert(database);

	std::unique_lock<std::mutex> lk(qmtx);

	std::shared_ptr<DatabaseQueue> queue;

	if (database->flags & DB_WRITABLE) {
		queue = writable_databases[database->hash];
#ifdef HAVE_REMOTE_PROTOCOL
		if (database->local && database->mastery_level != -1) {
			std::string new_revision = database->db->get_revision_info();
			if (new_revision != database->checkout_revision) {
				Endpoint endpoint = *database->endpoints.begin();
				endpoint.mastery_level = database->mastery_level;
				updated_databases.push(endpoint);
			}
		}
#endif
	} else {
		queue = databases[database->hash];
	}

	assert(database->weak_queue.lock() == queue);

	int flags = database->flags;
	Endpoints &endpoints = database->endpoints;

	if (database->modified) {
		DatabaseAutocommit::signal_changed(database);
	}

	if (!(flags & DB_VOLATILE)) {
		queue->push(database);
	}

	bool signal_checkins = false;
	switch (queue->state) {
		case DatabaseQueue::replica_state::REPLICA_SWITCH:
			for (auto& endpoint : endpoints) {
				_switch_db(endpoint);
			}
			if (queue->state == DatabaseQueue::replica_state::REPLICA_FREE) {
				signal_checkins = true;
			}
			break;
		case DatabaseQueue::replica_state::REPLICA_LOCK:
			queue->state = DatabaseQueue::replica_state::REPLICA_FREE;
			signal_checkins = true;
			break;
		case DatabaseQueue::replica_state::REPLICA_FREE:
			break;
	}

	assert(queue->count >= queue->size());

	L_DATABASE_END(this, "-- CHECKED IN DB %s(%s) [%lx]", (flags & DB_WRITABLE) ? "w" : "r", endpoints.as_string().c_str(), (unsigned long)database.get());

	database.reset();

	lk.unlock();

	if (signal_checkins) {
		while (queue->checkin_callbacks.call());
	}
}


bool
DatabasePool::_switch_db(const Endpoint &endpoint)
{
	auto queues_set = queues[endpoint.hash()];

	bool switched = true;
	for (auto& queue : queues_set) {
		queue->state = DatabaseQueue::replica_state::REPLICA_SWITCH;
		if (queue->count != queue->size()) {
			switched = false;
			break;
		}
	}

	if (switched) {
		move_files(endpoint.path + "/.tmp", endpoint.path);

		for (auto& queue : queues_set) {
			queue->state = DatabaseQueue::replica_state::REPLICA_FREE;
			queue->switch_cond.notify_all();
		}
	} else {
		L_DEBUG(this, "Inside switch_db not queue->count == queue->size()");
	}

	return switched;
}


bool
DatabasePool::switch_db(const Endpoint &endpoint)
{
	std::lock_guard<std::mutex> lk(qmtx);
	return _switch_db(endpoint);
}


void
DatabasePool::init_ref(const Endpoints &endpoints)
{
	Endpoints ref_endpoints;
	ref_endpoints.insert(Endpoint(".refs"));
	std::shared_ptr<Database> ref_database;
	if (!checkout(ref_database, ref_endpoints, DB_WRITABLE | DB_SPAWN | DB_PERSISTENT)) {
		L_ERR(this, "Database refs it could not be checkout.");
		assert(false);
	}

	for (auto endp_it = endpoints.begin(); endp_it != endpoints.end(); ++endp_it) {
		std::string unique_id(prefixed(get_slot_hex(endp_it->path), DOCUMENT_ID_TERM_PREFIX));
		Xapian::PostingIterator p(ref_database->db->postlist_begin(unique_id));
		if (p == ref_database->db->postlist_end(unique_id)) {
			Xapian::Document doc;
			// Boolean term for the node.
			doc.add_boolean_term(unique_id);
			// Start values for the DB.
			doc.add_boolean_term(prefixed(DB_MASTER, get_prefix("master", DOCUMENT_CUSTOM_TERM_PREFIX, STRING_TYPE)));
			doc.add_value(DB_SLOT_CREF, "0");
			ref_database->replace(unique_id, doc, true);
		}
	}

	checkin(ref_database);
}


void
DatabasePool::inc_ref(const Endpoints &endpoints)
{
	Endpoints ref_endpoints;
	ref_endpoints.insert(Endpoint(".refs"));
	std::shared_ptr<Database> ref_database;
	if (!checkout(ref_database, ref_endpoints, DB_WRITABLE | DB_SPAWN | DB_PERSISTENT)) {
		L_ERR(this, "Database refs it could not be checkout.");
		assert(false);
	}

	Xapian::Document doc;

	for (auto endp_it = endpoints.begin(); endp_it != endpoints.end(); ++endp_it) {
		std::string unique_id(prefixed(get_slot_hex(endp_it->path), DOCUMENT_ID_TERM_PREFIX));
		Xapian::PostingIterator p = ref_database->db->postlist_begin(unique_id);
		if (p == ref_database->db->postlist_end(unique_id)) {
			// QUESTION: Document not found - should add?
			// QUESTION: This case could happen?
			doc.add_boolean_term(unique_id);
			doc.add_value(0, "0");
			ref_database->replace(unique_id, doc, true);
		} else {
			// Document found - reference increased
			doc = ref_database->db->get_document(*p);
			doc.add_boolean_term(unique_id);
			int nref = std::stoi(doc.get_value(0));
			doc.add_value(0, std::to_string(nref + 1));
			ref_database->replace(unique_id, doc, true);
		}
	}

	checkin(ref_database);
}


void
DatabasePool::dec_ref(const Endpoints &endpoints)
{
	Endpoints ref_endpoints;
	ref_endpoints.insert(Endpoint(".refs"));
	std::shared_ptr<Database> ref_database;
	if (!checkout(ref_database, ref_endpoints, DB_WRITABLE | DB_SPAWN | DB_PERSISTENT)) {
		L_ERR(this, "Database refs it could not be checkout.");
		assert(false);
	}

	Xapian::Document doc;

	for (auto endp_it = endpoints.begin(); endp_it != endpoints.end(); ++endp_it) {
		std::string unique_id(prefixed(get_slot_hex(endp_it->path), DOCUMENT_ID_TERM_PREFIX));
		Xapian::PostingIterator p = ref_database->db->postlist_begin(unique_id);
		if (p != ref_database->db->postlist_end(unique_id)) {
			doc = ref_database->db->get_document(*p);
			doc.add_boolean_term(unique_id);
			int nref = std::stoi(doc.get_value(0)) - 1;
			doc.add_value(0, std::to_string(nref));
			ref_database->replace(unique_id, doc, true);
			if (nref == 0) {
				// qmtx need a lock
				delete_files(endp_it->path);
			}
		}
	}

	checkin(ref_database);
}


int
DatabasePool::get_master_count()
{
	Endpoints ref_endpoints;
	ref_endpoints.insert(Endpoint(".refs"));
	std::shared_ptr<Database> ref_database;
	if (!checkout(ref_database, ref_endpoints, DB_WRITABLE | DB_SPAWN | DB_PERSISTENT)) {
		L_ERR(this, "Database refs it could not be checkout.");
		assert(false);
	}

	int count = 0;

	if (ref_database) {
		Xapian::PostingIterator p(ref_database->db->postlist_begin(DB_MASTER));
		count = std::distance(ref_database->db->postlist_begin(DB_MASTER), ref_database->db->postlist_end(DB_MASTER));
	}

	checkin(ref_database);

	return count;
}


bool
ExpandDeciderFilterPrefixes::operator()(const std::string &term) const
{
	for (auto i = prefixes.cbegin(); i != prefixes.cend(); ++i) {
		if (startswith(term, *i)) return true;
	}

	return prefixes.empty();
}
