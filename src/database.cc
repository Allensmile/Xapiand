/*
 * Copyright (C) 2015, 2016 deipi.com LLC and contributors. All rights reserved.
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
#include "generate_terms.h"
#include "length.h"
#include "multivaluerange.h"

#include <bitset>
#include <fcntl.h>
#include <limits>
#include <sysexits.h>

#define XAPIAN_LOCAL_DB_FALLBACK 1

#define DATABASE_UPDATE_TIME 10

#define DATA_STORAGE_PATH "docdata."

#define WAL_STORAGE_PATH "wal."

#define MAGIC 0xC0DE

#define SIZE_UUID 36

#define WAL_SYNC_MODE STORAGE_ASYNC_SYNC
#define XAPIAN_SYNC_MODE 0  // This could also be Xapian::DB_FULL_SYNC for xapian to ensure full sync
#define STORAGE_SYNC_MODE STORAGE_FULL_SYNC


static const std::regex find_field_re("(([_a-z][_a-z0-9]*):)?(\"[^\"]+\"|[^\": ]+)[ ]*", std::regex::icase | std::regex::optimize);


#if XAPIAND_DATABASE_WAL

constexpr const char* const DatabaseWAL::names[];


void
WalHeader::init(void* param, void* args)
{
	const DatabaseWAL* wal = static_cast<const DatabaseWAL*>(param);
	bool commit_eof = static_cast<bool>(args);

	head.magic = MAGIC;
	head.offset = STORAGE_START_BLOCK_OFFSET;
	strncpy(head.uuid, wal->database->get_uuid().c_str(), sizeof(head.uuid));

	std::string rev_serialised = wal->database->get_revision_info();
	const char *p = rev_serialised.data();
	const char *p_end = p + rev_serialised.size();

	size_t length = unserialise_length(&p, p_end);
	const char *r_end = p + length;
	uint32_t revision;
	unserialise_unsigned(&p, r_end, &revision);

	if (commit_eof) {
		++revision;
	}

	head.revision = revision;
}


void
WalHeader::validate(void* param, void*)
{
	if (head.magic != MAGIC) {
		throw MSG_StorageCorruptVolume("Bad WAL header magic number");
	}

	const DatabaseWAL* wal = static_cast<const DatabaseWAL*>(param);
	if (strncasecmp(head.uuid, wal->database->get_uuid().c_str(), sizeof(head.uuid))) {
		throw MSG_StorageCorruptVolume("WAL UUID mismatch");
	}
}


bool
DatabaseWAL::open_current(const std::string& path, bool commited)
{
	L_CALL(this, "DatabaseWAL::open_current()");

	const char *p = database->checkout_revision.data();
	const char *p_end = p + database->checkout_revision.size();
	size_t length = unserialise_length(&p, p_end);
	const char *r_end = p + length;

	uint32_t revision;
	unserialise_unsigned(&p, r_end, &revision);

	DIR *dir = opendir(path.c_str(), true);
	if (!dir) {
		throw MSG_Error("Could not open the dir (%s)", strerror(errno));
	}

	uint32_t highest_revision = 0;
	uint32_t lowest_revision = std::numeric_limits<uint32_t>::max();

	File_ptr fptr;
	find_file_dir(dir, fptr, WAL_STORAGE_PATH, true);

	while (fptr.ent) {
		try {
			uint32_t file_revision = get_volume(std::string(fptr.ent->d_name));
			if (static_cast<long>(file_revision) >= static_cast<long>(revision - WAL_SLOTS)) {
				if (file_revision < lowest_revision) {
					lowest_revision = file_revision;
				}

				if (file_revision > highest_revision) {
					highest_revision = file_revision;
				}
			}
		} catch (const std::invalid_argument&) {
			throw MSG_Error("In wal file %s (%s)", std::string(fptr.ent->d_name).c_str(), strerror(errno));
		} catch (const std::out_of_range&) {
			throw MSG_Error("In wal file %s (%s)", std::string(fptr.ent->d_name).c_str(), strerror(errno));
		}

		find_file_dir(dir, fptr, WAL_STORAGE_PATH, true);
	}

	closedir(dir);
	if (lowest_revision > revision) {
		open(path + "/" + WAL_STORAGE_PATH + std::to_string(revision), STORAGE_OPEN | STORAGE_WRITABLE | STORAGE_CREATE | STORAGE_COMPRESS | WAL_SYNC_MODE);
	} else {
		modified = false;

		bool reach_end = false;
		uint32_t start_off, end_off;
		uint32_t file_rev, begin_rev, end_rev;
		for (auto slot = lowest_revision; slot <= highest_revision && not reach_end; ++slot) {
			file_rev = begin_rev = slot;
			open(path + "/" + WAL_STORAGE_PATH + std::to_string(slot), STORAGE_OPEN);

			uint32_t high_slot = highest_valid_slot();
			if (high_slot == static_cast<uint32_t>(-1)) {
				continue;
			}

			if (slot == highest_revision) {
				reach_end = true; /* Avoid reenter to the loop with the high valid slot of the highest revision */
				if (!commited) {
					/* last slot contain offset at the end of file */
					/* In case not "commited" not execute the high slot avaible because are operations without commit */
					--high_slot;
				}
			}

			if (slot == lowest_revision) {
				slot = revision - header.head.revision - 1;
				if (slot == static_cast<uint32_t>(-1)) {
					/* The offset saved in slot 0 is the beginning of the revision 1 to reach 2
					 * for that reason the revision 0 to reach 1 start in STORAGE_START_BLOCK_OFFSET
					 */
					start_off = STORAGE_START_BLOCK_OFFSET;
					begin_rev = 0;
				} else {
					start_off = header.slot[slot];
					if (start_off == 0) {
						throw MSG_StorageCorruptVolume("Bad offset");
					}
					begin_rev = slot;
				}
			} else {
				start_off = STORAGE_START_BLOCK_OFFSET;
			}

			seek(start_off);

			end_off =  header.slot[high_slot];

			if (start_off < end_off) {
				end_rev =  header.head.revision + high_slot;
				L_INFO(nullptr, "Read and execute operations WAL file [wal.%u] from (%u..%u) revision", file_rev, begin_rev, end_rev);
			}

			try {
				while (true) {
					std::string line = read(end_off);
					if (!execute(line)) {
						throw MSG_Error("WAL revision mismatch!");
					}
				}
			} catch (const StorageEOF& exc) { }

			slot = high_slot;
		}

		open(path + "/" + WAL_STORAGE_PATH + std::to_string(highest_revision), STORAGE_OPEN | STORAGE_WRITABLE | STORAGE_CREATE | STORAGE_COMPRESS | WAL_SYNC_MODE);
	}
	return modified;
}


uint32_t
DatabaseWAL::highest_valid_slot()
{
	L_CALL(this, "DatabaseWAL::highest_valid_slot()");

	uint32_t slot = -1;
	for (uint32_t i = 0; i < WAL_SLOTS; ++i) {
		if (header.slot[i] == 0) {
			break;
		}
		slot = i;
	}
	return slot;
}


bool
DatabaseWAL::execute(const std::string& line)
{
	L_CALL(this, "DatabaseWAL::execute()");

	const char *p = line.data();
	const char *p_end = p + line.size();

	if (!(database->flags & DB_WRITABLE)) {
		throw MSG_Error("Database is read-only");
	}

	if (!database->endpoints[0].is_local()) {
		throw MSG_Error("Can not execute WAL on a remote database!");
	}

	size_t size = unserialise_length(&p, p_end, true);
	std::string revision(p, size);
	p += size;

	std::string encoded_db_rev = database->get_revision_info();
	const char* r = encoded_db_rev.data();
	const char* r_end = r + encoded_db_rev.size();
	std::string db_revision = unserialise_string(&r, r_end);

	if (revision != db_revision) {
		return false;
	}

	Type type = static_cast<Type>(unserialise_length(&p, p_end));

	std::string data(p, p_end);

	Xapian::docid did;
	Xapian::Document doc;
	Xapian::termcount freq;
	std::string term;

	p = data.data();
	p_end = p + data.size();

	modified = true;

	switch (type) {
		case Type::ADD_DOCUMENT:
			doc = Xapian::Document::unserialise(data);
			database->add_document(doc, false, false);
			break;
		case Type::CANCEL:
			database->cancel(false);
			break;
		case Type::DELETE_DOCUMENT_TERM:
			size = unserialise_length(&p, p_end, true);
			term = std::string(p, size);
			database->delete_document_term(term, false, false);
			break;
		case Type::COMMIT:
			database->commit(false);
			modified = false;
			break;
		case Type::REPLACE_DOCUMENT:
			did = static_cast<Xapian::docid>(unserialise_length(&p, p_end));
			doc = Xapian::Document::unserialise(std::string(p, p_end - p));
			database->replace_document(did, doc, false, false);
			break;
		case Type::REPLACE_DOCUMENT_TERM:
			size = unserialise_length(&p, p_end, true);
			term = std::string(p, size);
			doc = Xapian::Document::unserialise(std::string(p + size, p_end - p - size));
			database->replace_document_term(term, doc, false, false);
			break;
		case Type::DELETE_DOCUMENT:
			did = static_cast<Xapian::docid>(unserialise_length(&p, p_end));
			database->delete_document(did, false, false);
			break;
		case Type::SET_METADATA:
			size = unserialise_length(&p, p_end, true);
			database->set_metadata(std::string(p, size), std::string(p + size, p_end - p - size), false, false);
			break;
		case Type::ADD_SPELLING:
			freq = static_cast<Xapian::termcount>(unserialise_length(&p, p_end));
			database->add_spelling(std::string(p, p_end - p), freq, false, false);
			break;
		case Type::REMOVE_SPELLING:
			freq = static_cast<Xapian::termcount>(unserialise_length(&p, p_end));
			database->remove_spelling(std::string(p, p_end - p), freq, false, false);
			break;
		default:
			throw MSG_Error("Invalid WAL message!");
	}

	return true;
}


void
DatabaseWAL::write_line(Type type, const std::string& data, bool commit_)
{
	L_CALL(this, "DatabaseWAL::write_line()");

	assert(database->flags & DB_WRITABLE);
	assert(!(database->flags & DB_NOWAL));

	auto endpoint = database->endpoints[0];
	assert(endpoint.is_local());

	std::string revision_encode = database->get_revision_info();
	std::string uuid = database->get_uuid();
	std::string line(revision_encode + serialise_length(toUType(type)) + data);

	L_DATABASE_WAL(this, "%s on %s: '%s'", names[toUType(type)], endpoint.path.c_str(), repr(line).c_str());

	const char* p = revision_encode.data();
	const char* p_end = p + revision_encode.size();
	std::string revision = unserialise_string(&p, p_end);

	uint32_t rev;
	const char* r = revision.data();
	const char* r_end = r + revision.size();
	unserialise_unsigned(&r, r_end, &rev);

	uint32_t slot = rev - header.head.revision;

	if (slot >= WAL_SLOTS) {
		close();
		open(endpoint.path + "/" + WAL_STORAGE_PATH + std::to_string(rev), STORAGE_OPEN | STORAGE_WRITABLE | STORAGE_CREATE | STORAGE_COMPRESS | WAL_SYNC_MODE);
		slot = rev - header.head.revision;
	}

	write(line.data(), line.size());
	header.slot[slot] = header.head.offset; /* Beginning of the next revision */

	if (commit_) {
		if (slot + 1 >= WAL_SLOTS) {
			close();
			open(endpoint.path + "/" + WAL_STORAGE_PATH + std::to_string(rev + 1), STORAGE_OPEN | STORAGE_WRITABLE | STORAGE_CREATE | STORAGE_COMPRESS | WAL_SYNC_MODE, true);
		} else {
			header.slot[slot + 1] = header.slot[slot];
		}
	}

	commit();
}


void
DatabaseWAL::write_add_document(const Xapian::Document& doc)
{
	L_CALL(this, "DatabaseWAL::write_add_document()");

	write_line(Type::ADD_DOCUMENT, doc.serialise());
}


void
DatabaseWAL::write_cancel()
{
	L_CALL(this, "DatabaseWAL::write_cancel()");

	write_line(Type::CANCEL, "");
}


void
DatabaseWAL::write_delete_document_term(const std::string& term)
{
	L_CALL(this, "DatabaseWAL::write_delete_document_term()");

	write_line(Type::DELETE_DOCUMENT_TERM, serialise_length(term.size()) + term);
}


void
DatabaseWAL::write_commit()
{
	L_CALL(this, "DatabaseWAL::write_commit()");

	write_line(Type::COMMIT, "", true);
}


void
DatabaseWAL::write_replace_document(Xapian::docid did, const Xapian::Document& doc)
{
	L_CALL(this, "DatabaseWAL::write_replace_document()");

	write_line(Type::REPLACE_DOCUMENT, serialise_length(did) + doc.serialise());
}


void
DatabaseWAL::write_replace_document_term(const std::string& term, const Xapian::Document& doc)
{
	L_CALL(this, "DatabaseWAL::write_replace_document_term()");

	write_line(Type::REPLACE_DOCUMENT_TERM, serialise_length(term.size()) + term + doc.serialise());
}


void
DatabaseWAL::write_delete_document(Xapian::docid did)
{
	L_CALL(this, "DatabaseWAL::write_delete_document()");

	write_line(Type::DELETE_DOCUMENT, serialise_length(did));
}


void
DatabaseWAL::write_set_metadata(const std::string& key, const std::string& val)
{
	L_CALL(this, "DatabaseWAL::write_set_metadata()");

	write_line(Type::SET_METADATA, serialise_length(key.size()) + key + val);
}


void
DatabaseWAL::write_add_spelling(const std::string& word, Xapian::termcount freqinc)
{
	L_CALL(this, "DatabaseWAL::write_add_spelling()");

	write_line(Type::ADD_SPELLING, serialise_length(freqinc) + word);
}


void
DatabaseWAL::write_remove_spelling(const std::string& word, Xapian::termcount freqdec)
{
	L_CALL(this, "DatabaseWAL::write_remove_spelling()");

	write_line(Type::REMOVE_SPELLING, serialise_length(freqdec) + word);
}

#endif


//  ____        _        _                  __        ___    _
// |  _ \  __ _| |_ __ _| |__   __ _ ___  __\ \      / / \  | |
// | | | |/ _` | __/ _` | '_ \ / _` / __|/ _ \ \ /\ / / _ \ | |
// | |_| | (_| | || (_| | |_) | (_| \__ \  __/\ V  V / ___ \| |___
// |____/ \__,_|\__\__,_|_.__/ \__,_|___/\___| \_/\_/_/   \_\_____|
//
////////////////////////////////////////////////////////////////////////////////


Database::Database(std::shared_ptr<DatabaseQueue>& queue_, const Endpoints& endpoints_, int flags_) :
	weak_queue(queue_),
	endpoints(endpoints_),
	flags(flags_),
	hash(endpoints.hash()),
	access_time(system_clock::now()),
	modified(false),
	mastery_level(-1)
{
	reopen();

	if (auto queue = weak_queue.lock()) {
		queue->inc_count();
	}

	L_OBJ(this, "CREATED DATABASE!");
}


Database::~Database()
{
	if (auto queue = weak_queue.lock()) {
		queue->dec_count();
	}

	L_OBJ(this, "DELETED DATABASE!");
}


long long
Database::read_mastery(const Endpoint& endpoint)
{
	L_CALL(this, "Database::read_mastery()");

	if (mastery_level != -1) return mastery_level;
	if (!endpoint.is_local()) return -1;

	mastery_level = ::read_mastery(endpoint.path, true);

	return mastery_level;
}


void
Database::get_stats_database(MsgPack&& stats)
{
	L_CALL(this, "Database::get_stats_database()");

	unsigned doccount = db->get_doccount();
	unsigned lastdocid = db->get_lastdocid();
	stats["uuid"] = db->get_uuid();
	stats["doc_count"] = doccount;
	stats["last_id"] = lastdocid;
	stats["doc_del"] = lastdocid - doccount;
	stats["av_length"] = db->get_avlength();
	stats["doc_len_lower"] =  db->get_doclength_lower_bound();
	stats["doc_len_upper"] = db->get_doclength_upper_bound();
	stats["has_positions"] = db->has_positions();
}


void
Database::get_stats_doc(MsgPack&& stats, const std::string& doc_id)
{
	L_CALL(this, "Database::get_stats_doc()");

	Xapian::Document doc = get_document(doc_id);

	stats[RESERVED_ID] = get_value(doc, RESERVED_ID);

	MsgPack obj_data = get_MsgPack(doc);
	try {
		obj_data = obj_data.at(RESERVED_DATA);
	} catch (const std::out_of_range&) {
		clean_reserved(obj_data);
	}

	stats[RESERVED_DATA] = std::move(obj_data);

	std::string ct_type = doc.get_value(DB_SLOT_TYPE);
	stats["blob"] = ct_type != JSON_TYPE && ct_type != MSGPACK_TYPE;

	stats["number_terms"] = doc.termlist_count();

	std::string terms;
	const auto it_e = doc.termlist_end();
	for (auto it = doc.termlist_begin(); it != it_e; ++it) {
		terms += repr(*it) + " ";
	}
	stats[RESERVED_TERMS] = terms;

	stats["number_values"] = doc.values_count();

	std::string values;
	const auto iv_e = doc.values_end();
	for (auto iv = doc.values_begin(); iv != iv_e; ++iv) {
		values += std::to_string(iv.get_valueno()) + ":" + repr(*iv) + " ";
	}
	stats[RESERVED_VALUES] = values;
}


bool
Database::reopen()
{
	L_CALL(this, "Database::reopen()");

	access_time = system_clock::now();

	if (db) {
		// Try to reopen
		try {
			bool ret = db->reopen();
			return ret;
		} catch (const Xapian::Error& exc) {
			L_EXC(this, "ERROR: %s", exc.get_msg().c_str());
			db->close();
			db.reset();
		}
	}

	auto endpoints_size = endpoints.size();
	auto i = endpoints.cbegin();
	if (flags & DB_WRITABLE) {
		assert(endpoints_size == 1);
		db = std::make_unique<Xapian::WritableDatabase>();
		auto& e = *i;
		Xapian::WritableDatabase wdb;
		bool local = false;
		int _flags = (flags & DB_SPAWN) ? Xapian::DB_CREATE_OR_OPEN | XAPIAN_SYNC_MODE : Xapian::DB_OPEN | XAPIAN_SYNC_MODE;
#ifdef XAPIAND_CLUSTERING
		if (!e.is_local()) {
			// Writable remote databases do not have a local fallback
			int port = (e.port == XAPIAND_BINARY_SERVERPORT) ? XAPIAND_BINARY_PROXY : e.port;
			wdb = Xapian::Remote::open_writable(e.host, port, 0, 10000, _flags, e.path);
#ifdef XAPIAN_LOCAL_DB_FALLBACK
			try {
				Xapian::Database tmp = Xapian::Database(e.path, Xapian::DB_OPEN);
				if (tmp.get_uuid() == wdb.get_uuid()) {
					L_DATABASE(this, "Endpoint %s fallback to local database!", e.as_string().c_str());
					// Handle remote endpoints and figure out if the endpoint is a local database
					wdb = Xapian::WritableDatabase(e.path, _flags);
					local = true;
					if (endpoints_size == 1) read_mastery(e);
				}
			} catch (const Xapian::DatabaseOpeningError& exc) { }
#endif

		}
		else
#endif
		{
			wdb = Xapian::WritableDatabase(e.path, _flags);
			local = true;
			if (endpoints_size == 1) read_mastery(e);
		}

		db->add_database(wdb);

		if (local) {
			checkout_revision = get_revision_info();
		}

#ifdef XAPIAND_DATABASE_WAL
		if (local && !(flags & DB_NOWAL)) {
			// WAL required on a local writable database, open it.
			wal = std::make_unique<DatabaseWAL>(this);
			if (wal->open_current(e.path, true)) {
				modified = true;
			}
		}
#endif
	} else {
		for (db = std::make_unique<Xapian::Database>(); i != endpoints.cend(); ++i) {
			auto& e = *i;
			Xapian::Database rdb;
			int _flags = (flags & DB_SPAWN) ? Xapian::DB_CREATE_OR_OPEN : Xapian::DB_OPEN;
#ifdef XAPIAND_CLUSTERING
			if (!e.is_local()) {
				int port = (e.port == XAPIAND_BINARY_SERVERPORT) ? XAPIAND_BINARY_PROXY : e.port;
				rdb = Xapian::Remote::open(e.host, port, 10000, 10000, _flags, e.path);
#ifdef XAPIAN_LOCAL_DB_FALLBACK
				try {
					Xapian::Database tmp = Xapian::Database(e.path, Xapian::DB_OPEN);
					if (tmp.get_uuid() == rdb.get_uuid()) {
						L_DATABASE(this, "Endpoint %s fallback to local database!", e.as_string().c_str());
						// Handle remote endpoints and figure out if the endpoint is a local database
						rdb = Xapian::Database(e.path, _flags);
						if (endpoints_size == 1) read_mastery(e);
					}
				} catch (const Xapian::DatabaseOpeningError& exc) { }
#endif
			}
			else
#endif
			{
				try {
					rdb = Xapian::Database(e.path, Xapian::DB_OPEN);
					if (endpoints_size == 1) read_mastery(e);
				} catch (const Xapian::DatabaseOpeningError& exc) {
					if (!(flags & DB_SPAWN))  {
						db.reset();
						throw;
					}
					Xapian::WritableDatabase tmp = Xapian::WritableDatabase(e.path, Xapian::DB_CREATE_OR_OPEN);
					rdb = Xapian::Database(e.path, Xapian::DB_OPEN);
					if (endpoints_size == 1) read_mastery(e);
				}
			}

			db->add_database(rdb);
		}
	}

	return true;
}


Database::search_t
Database::_search(const std::string& query, unsigned flags, bool text, const std::string& lan)
{
	L_CALL(this, "Database::_search()");

	search_t srch;

	if (query.compare("*") == 0) {
		srch.query = Xapian::Query::MatchAll;
		srch.suggested_query.push_back("");
		return srch;
	}

	size_t size_match = 0;
	bool first_time = true, first_timeR = true;
	std::string querystring;
	Xapian::QueryParser queryparser;
	queryparser.set_database(*db);

	// Set for save the prefix added in queryparser.
	std::unordered_set<std::string> added_prefixes;

	if (text) {
		queryparser.set_stemming_strategy(queryparser.STEM_SOME);
		lan.empty() ? queryparser.set_stemmer(Xapian::Stem(default_spc.language[0])) : queryparser.set_stemmer(Xapian::Stem(lan));
	}

	std::sregex_iterator next(query.begin(), query.end(), find_field_re, std::regex_constants::match_continuous);
	std::sregex_iterator end;
	while (next != end) {
		std::string field(next->str(0));
		size_match += next->length(0);
		std::string field_name_dot(next->str(1));
		std::string field_name(next->str(2));
		std::string field_value(next->str(3));
		
		std::shared_ptr<const Schema> schema = XapiandManager::manager->database_pool.get_schema(endpoints[0]);
		data_field_t field_t = schema->get_data_field(field_name);

		std::smatch m;
		if (std::regex_match(field_value, m, find_range_re)) {
			// If this field is not indexed as value, not process this query.
			if (field_t.slot == Xapian::BAD_VALUENO) {
				++next;
				continue;
			}

			Xapian::Query queryRange;

			switch (field_t.type) {
				case NUMERIC_TYPE: {
					auto start(m.str(1)), end(m.str(2));

					queryRange = MultipleValueRange::getQuery(field_t.slot, NUMERIC_TYPE, start, end, field_name);

					auto filter_term = GenerateTerms::numeric(start, end, field_t.accuracy, field_t.acc_prefix, added_prefixes, srch.nfps, queryparser);
					if (!filter_term.empty()) {
						queryRange = Xapian::Query(Xapian::Query::OP_AND, queryparser.parse_query(filter_term, flags), queryRange);
					}
					break;
				}
				case STRING_TYPE: {
					std::string start(m.str(1)), end(m.str(2));
					queryRange = MultipleValueRange::getQuery(field_t.slot, STRING_TYPE, start, end, field_name);
					break;
				}
				case DATE_TYPE: {
					auto start(m.str(1)), end(m.str(2));

					queryRange = MultipleValueRange::getQuery(field_t.slot, DATE_TYPE, start, end, field_name);

					auto filter_term = GenerateTerms::date(start, end, field_t.accuracy, field_t.acc_prefix, added_prefixes, srch.dfps, queryparser);
					if (!filter_term.empty()) {
						queryRange = Xapian::Query(Xapian::Query::OP_AND, queryparser.parse_query(filter_term, flags), queryRange);
					}
					break;
				}
				case GEO_TYPE: {
					// Validate special case.
					if (field_value.compare("..") == 0) {
						queryRange = Xapian::Query::MatchAll;
						break;
					}

					// The format is: "..EWKT". We always delete double quotes and .. -> EWKT
					field_value.assign(field_value, 3, field_value.size() - 4);

					RangeList ranges;
					CartesianUSet centroids;
					EWKT_Parser::getRanges(field_value, field_t.accuracy[0], field_t.accuracy[1], ranges, centroids);

					queryRange = GeoSpatialRange::getQuery(field_t.slot, ranges, centroids);

					auto filter_term = GenerateTerms::geo(ranges, field_t.accuracy, field_t.acc_prefix, added_prefixes, srch.gfps, queryparser);
					if (!filter_term.empty()) {
						queryRange = Xapian::Query(Xapian::Query::OP_AND, queryparser.parse_query(filter_term, flags), queryRange);
					}
					break;
				}
			}

			// Concatenate with OR all the ranges queries.
			if (first_timeR) {
				srch.query = queryRange;
				first_timeR = false;
			} else {
				srch.query = Xapian::Query(Xapian::Query::OP_OR, srch.query, queryRange);
			}
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
						auto nfp = std::make_unique<NumericFieldProcessor>(field_t.prefix);
						field_t.bool_term ? queryparser.add_boolean_prefix(field_name, nfp.get()) : queryparser.add_prefix(field_name, nfp.get());
						srch.nfps.push_back(std::move(nfp));
					}
					field.assign(field_name_dot).append(to_query_string(field_value));
					break;
				case STRING_TYPE:
					// Xapian does not allow repeat prefixes.
					if (added_prefixes.insert(field_t.prefix).second) {
						field_t.bool_term ? queryparser.add_boolean_prefix(field_name, field_t.prefix) : queryparser.add_prefix(field_name, field_t.prefix);
					}
					break;
				case DATE_TYPE:
					// If there are double quotes, they are deleted: "date" -> date
					if (field_value.at(0) == '"') {
						field_value.assign(field_value, 1, field_value.size() - 2);
					}

					// Xapian does not allow repeat prefixes.
					if (added_prefixes.insert(field_t.prefix).second) {
						auto dfp = std::make_unique<DateFieldProcessor>(field_t.prefix);
						field_t.bool_term ? queryparser.add_boolean_prefix(field_name, dfp.get()) : queryparser.add_prefix(field_name, dfp.get());
						srch.dfps.push_back(std::move(dfp));
					}
					field.assign(field_name_dot).append(to_query_string(std::to_string(Datetime::timestamp(field_value))));
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

					// Xapian does not allow repeat prefixes.
					if (added_prefixes.insert(field_t.prefix).second) {
						field_t.bool_term ? queryparser.add_boolean_prefix(field_name, field_t.prefix) : queryparser.add_prefix(field_name, field_t.prefix);
					}
					field.assign(field_name_dot).append(field_value);
					break;
				case BOOLEAN_TYPE:
					// Xapian does not allow repeat prefixes.
					if (added_prefixes.insert(field_t.prefix).second) {
						auto bfp = std::make_unique<BooleanFieldProcessor>(field_t.prefix);
						field_t.bool_term ? queryparser.add_boolean_prefix(field_name, bfp.get()) : queryparser.add_prefix(field_name, bfp.get());
						srch.bfps.push_back(std::move(bfp));
					}
					break;
			}

			// Concatenate with OR all the queries.
			if (first_time) {
				querystring = field;
				first_time = false;
			} else {
				querystring += " OR " + field;
			}
		}

		++next;
	}

	if (size_match != query.size()) {
		throw MSG_QueryParserError("Query '" + query + "' contains errors");
	}

	switch (first_time << 1 | first_timeR) {
		case 0:
			try {
				srch.query = Xapian::Query(Xapian::Query::OP_OR, queryparser.parse_query(querystring, flags), srch.query);
				srch.suggested_query.push_back(queryparser.get_corrected_query_string());
			} catch (const Xapian::QueryParserError& exc) {
				L_EXC(this, "ERROR: %s", exc.get_msg().c_str());
				reopen();
				srch.query = Xapian::Query(Xapian::Query::OP_OR, queryparser.parse_query(querystring, flags), srch.query);
				srch.suggested_query.push_back(queryparser.get_corrected_query_string());
			}
			break;
		case 1:
			try {
				srch.query = queryparser.parse_query(querystring, flags);
				srch.suggested_query.push_back(queryparser.get_corrected_query_string());
			} catch (const Xapian::QueryParserError& exc) {
				L_EXC(this, "ERROR: %s", exc.get_msg().c_str());
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


Database::search_t
Database::search(const query_field_t& e)
{
	L_CALL(this, "Database::search()");

	search_t srch_resul;
	std::vector<std::string> sug_query;
	bool first = true;

	L_DEBUG(this, "e.query size: %d  Spelling: %d Synonyms: %d", e.query.size(), e.spelling, e.synonyms);
	auto lit = e.language.begin();
	std::string lan;
	unsigned flags = Xapian::QueryParser::FLAG_DEFAULT | Xapian::QueryParser::FLAG_WILDCARD | Xapian::QueryParser::FLAG_PURE_NOT;
	if (e.spelling) flags |= Xapian::QueryParser::FLAG_SPELLING_CORRECTION;
	if (e.synonyms) flags |= Xapian::QueryParser::FLAG_SYNONYM;
	Xapian::Query queryQ;
	for (const auto& query : e.query) {
		if (lit != e.language.end()) {
			lan = *lit++;
		}
		search_t srch = _search(query, flags, true, lan);
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
	Xapian::Query queryP;
	for (const auto& partial : e.partial) {
		search_t srch = _search(partial, flags, false, "");
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
	Xapian::Query queryT;
	for (const auto& terms : e.terms) {
		search_t srch = _search(terms, flags, false, "");
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
	Xapian::Query queryF;
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


void
Database::get_similar(bool is_fuzzy, Xapian::Enquire& enquire, Xapian::Query& query, const similar_field_t& similar)
{
	L_CALL(this, "Database::get_similar()");

	Xapian::RSet rset;

	for (int t = DB_RETRIES; t >= 0; --t) {
		try {
			Xapian::Enquire renquire = get_enquire(query, Xapian::BAD_VALUENO, nullptr, nullptr, nullptr);
			Xapian::MSet mset = renquire.get_mset(0, similar.n_rset);
			for (const auto& doc : mset) {
				rset.add_document(doc);
			}
			break;
		} catch (const Xapian::DatabaseModifiedError& exc) {
			if (!t) throw MSG_Error("Database was modified, try again (%s)", exc.get_msg().c_str());
		} catch (const Xapian::NetworkError& exc) {
			if (!t) throw MSG_Error("Problem communicating with the remote database (%s)", exc.get_msg().c_str());
		} catch (const Xapian::Error& exc) {
			throw MSG_Error(exc.get_msg().c_str());
		}
		reopen();
	}

	std::vector<std::string> prefixes;
	prefixes.reserve(similar.type.size() + similar.field.size());
	for (const auto& sim_type : similar.type) {
		prefixes.push_back(DOCUMENT_CUSTOM_TERM_PREFIX + Unserialise::type(sim_type));
	}

	for (const auto& sim_field : similar.field) {
		std::shared_ptr<const Schema> schema = XapiandManager::manager->database_pool.get_schema(endpoints[0]);
		data_field_t field_t = schema->get_data_field(sim_field);
		if (field_t.type != NO_TYPE) {
			prefixes.push_back(field_t.prefix);
		}
	}

	ExpandDeciderFilterPrefixes efp(prefixes);
	Xapian::ESet eset = enquire.get_eset(similar.n_eset, rset, &efp);

	if (is_fuzzy) {
		query = Xapian::Query(Xapian::Query::OP_OR, query, Xapian::Query(Xapian::Query::OP_ELITE_SET, eset.begin(), eset.end(), similar.n_term));
	} else {
		query = Xapian::Query(Xapian::Query::OP_ELITE_SET, eset.begin(), eset.end(), similar.n_term);
	}
}


Xapian::Enquire
Database::get_enquire(Xapian::Query& query, const Xapian::valueno& collapse_key, const query_field_t *e, Multi_MultiValueKeyMaker *sorter,
		std::vector<std::pair<std::string, std::unique_ptr<MultiValueCountMatchSpy>>> *spies)
{
	L_CALL(this, "Database::get_enquire()");

	Xapian::Enquire enquire(*db);

	enquire.set_query(query);

	if (sorter) {
		enquire.set_sort_by_key_then_relevance(sorter, false);
	}

	int collapse_max = 1;
	if (e) {
		if (e->is_nearest) {
			get_similar(false, enquire, query, e->nearest);
		}

		if (e->is_fuzzy) {
			get_similar(true, enquire, query, e->fuzzy);
		}

		for (const auto& facet : e->facets) {
			std::shared_ptr<const Schema> schema = XapiandManager::manager->database_pool.get_schema(endpoints[0]);
			data_field_t field_t = schema->get_slot_field(facet);
			if (field_t.type != NO_TYPE) {
				std::unique_ptr<MultiValueCountMatchSpy> spy = std::make_unique<MultiValueCountMatchSpy>(get_slot(facet), field_t.type == GEO_TYPE);
				enquire.add_matchspy(spy.get());
				L_DATABASE_WRAP(this, "added spy -%s-", (facet).c_str());
				spies->push_back(std::make_pair(facet, std::move(spy)));
			}
		}

		collapse_max = e->collapse_max;
	}

	enquire.set_collapse_key(collapse_key, collapse_max);

	return enquire;
}


void
Database::get_mset(const query_field_t& e, Xapian::MSet& mset, std::vector<std::pair<std::string, std::unique_ptr<MultiValueCountMatchSpy>>>& spies,
		std::vector<std::string>& suggestions, int offset)
{
	L_CALL(this, "Database::get_mset()");

	auto doccount = db->get_doccount();
	auto check_at_least = std::max(std::min(doccount, e.check_at_least), 0u);
	Xapian::valueno collapse_key;

	std::shared_ptr<const Schema> schema = XapiandManager::manager->database_pool.get_schema(endpoints[0]);

	// Get the collapse key to use for queries.
	if (!e.collapse.empty()) {
		data_field_t field_t = schema->get_slot_field(e.collapse);
		collapse_key = field_t.slot;
	} else {
		collapse_key = Xapian::BAD_VALUENO;
	}

	Multi_MultiValueKeyMaker sorter_obj;
	Multi_MultiValueKeyMaker *sorter = nullptr;
	if (!e.sort.empty()) {
		sorter = &sorter_obj;
		for (const auto& sort : e.sort) {
			std::string field, value;
			size_t pos = sort.find(":");
			if (pos != std::string::npos) {
				field = sort.substr(0, pos);
				value = sort.substr(pos + 1);
			} else {
				field = sort;
			}

			if (field.at(0) == '-') {
				field.assign(field, 1, field.size() - 1);
				data_field_t field_t = schema->get_slot_field(field);
				if (field_t.type != NO_TYPE) {
					sorter->add_value(field_t.slot, field_t.type, value, true);
				}
			} else if (field.at(0) == '+') {
				field.assign(field, 1, field.size() - 1);
				data_field_t field_t = schema->get_slot_field(field);
				if (field_t.type != NO_TYPE) {
					sorter->add_value(field_t.slot, field_t.type, value);
				}
			} else {
				data_field_t field_t = schema->get_slot_field(field);
				if (field_t.type != NO_TYPE) {
					sorter->add_value(field_t.slot, field_t.type, value);
				}
			}
		}
	}

	for (int t = DB_RETRIES; t >= 0; --t) {
		try {
			search_t srch = search(e);
			Xapian::Enquire enquire = get_enquire(srch.query, collapse_key, &e, sorter, &spies);
			suggestions = srch.suggested_query;
			mset = enquire.get_mset(e.offset + offset, e.limit - offset, check_at_least);
			break;
		} catch (const Xapian::DatabaseModifiedError& exc) {
			if (!t) throw MSG_Error("Database was modified, try again (%s)", exc.get_msg().c_str());
		} catch (const Xapian::NetworkError& exc) {
			if (!t) throw MSG_Error("Problem communicating with the remote database (%s)", exc.get_msg().c_str());
		} catch (const Xapian::QueryParserError& exc) {
			throw MSG_ClientError("%s", exc.get_msg().c_str());
		} catch (const Xapian::Error& exc) {
			throw MSG_Error(exc.get_msg().c_str());
		} catch (const std::exception& exc) {
			throw MSG_ClientError("The search was not performed (%s)", exc.what());
		}
		reopen();
	}
}


std::string
Database::get_uuid() const
{
	L_CALL(this, "Database::get_uuid");

	return db->get_uuid();
}


std::string
Database::get_revision_info() const
{
	L_CALL(this, "Database::get_revision_info()");

#if HAVE_DATABASE_REVISION_INFO
	return db->get_revision_info();
#else
	return std::string();
#endif
}


bool
Database::commit(bool wal_)
{
	L_CALL(this, "Database::commit()");

	// FIXME: When it should be the schema update
	// schema->store();

	if (!modified) {
		L_DATABASE_WRAP(this, "Do not commit, because there are not changes");
		return false;
	}

#if XAPIAND_DATABASE_WAL
	if (wal_ && wal) wal->write_commit();
#else
	(void)wal_;
#endif

	for (int t = DB_RETRIES; t >= 0; --t) {
		L_DATABASE_WRAP(this, "Commit: t: %d", t);
		Xapian::WritableDatabase *wdb = static_cast<Xapian::WritableDatabase *>(db.get());
		try {
			wdb->commit();
			modified = false;
			break;
		} catch (const Xapian::DatabaseModifiedError& exc) {
			if (!t) throw MSG_Error("Database was modified, try again (%s)", exc.get_msg().c_str());
		} catch (const Xapian::NetworkError& exc) {
			if (!t) throw MSG_Error("Problem communicating with the remote database (%s)", exc.get_msg().c_str());
		} catch (const Xapian::Error& exc) {
			throw MSG_Error(exc.get_msg().c_str());
		}
		reopen();
	}

	L_DATABASE_WRAP(this, "Commit made");
	return true;
}


void
Database::cancel(bool wal_)
{
	L_CALL(this, "Database::cancel()");

	if (!(flags & DB_WRITABLE)) {
		throw MSG_Error("database is read-only");
	}

#if XAPIAND_DATABASE_WAL
	if (wal_ && wal) wal->write_cancel();
#else
	(void)wal_;
#endif

	for (int t = DB_RETRIES; t >= 0; --t) {
		L_DATABASE_WRAP(this, "Cancel: t: %d", t);
		Xapian::WritableDatabase *wdb = static_cast<Xapian::WritableDatabase *>(db.get());
		try {
			wdb->begin_transaction(false);
			wdb->cancel_transaction();
			break;
		} catch (const Xapian::DatabaseModifiedError& exc) {
			if (!t) throw MSG_Error("Database was modified, try again (%s)", exc.get_msg().c_str());
		} catch (const Xapian::NetworkError& exc) {
			if (!t) throw MSG_Error("Problem communicating with the remote database (%s)", exc.get_msg().c_str());
		} catch (const Xapian::Error& exc) {
			throw MSG_Error(exc.get_msg().c_str());
		}
		reopen();
	}

	L_DATABASE_WRAP(this, "Cancel made");
}


void
Database::delete_document(Xapian::docid did, bool commit_, bool wal_)
{
	L_CALL(this, "Database::delete_document()");

	if (!(flags & DB_WRITABLE)) {
		throw MSG_Error("database is read-only");
	}

#if XAPIAND_DATABASE_WAL
	if (wal_ && wal) wal->write_delete_document(did);
#else
	(void)wal_;
#endif

	for (int t = DB_RETRIES; t >= 0; --t) {
		L_DATABASE_WRAP(this, "Deleting document: %d  t: %d", did, t);
		Xapian::WritableDatabase *wdb = static_cast<Xapian::WritableDatabase *>(db.get());
		try {
			wdb->delete_document(did);
			modified = true;
			break;
		} catch (const Xapian::DatabaseModifiedError& exc) {
			if (!t) throw MSG_Error("Database was modified, try again (%s)", exc.get_msg().c_str());
		} catch (const Xapian::NetworkError& exc) {
			if (!t) throw MSG_Error("Problem communicating with the remote database (%s)", exc.get_msg().c_str());
		} catch (const Xapian::Error& exc) {
			throw MSG_Error(exc.get_msg().c_str());
		}
		reopen();
	}

	L_DATABASE_WRAP(this, "Document deleted");
	if (commit_) commit(wal_);
}


void
Database::delete_document(const std::string& doc_id, bool commit_, bool wal_)
{
	L_CALL(this, "Database::delete_document()");
	delete_document_term(prefixed(doc_id, DOCUMENT_ID_TERM_PREFIX), commit_, wal_);
}


void
Database::delete_document_term(const std::string& term, bool commit_, bool wal_)
{
	L_CALL(this, "Database::delete_document_term()");

	if (!(flags & DB_WRITABLE)) {
		throw MSG_Error("database is read-only");
	}

#if XAPIAND_DATABASE_WAL
	if (wal_ && wal) wal->write_delete_document_term(term);
#else
	(void)wal_;
#endif

	for (int t = DB_RETRIES; t >= 0; --t) {
		L_DATABASE_WRAP(this, "Deleting document: '%s'  t: %d", term.c_str(), t);
		Xapian::WritableDatabase *wdb = static_cast<Xapian::WritableDatabase *>(db.get());
		try {
			wdb->delete_document(term);
			modified = true;
			break;
		} catch (const Xapian::DatabaseModifiedError& exc) {
			if (!t) throw MSG_Error("Database was modified, try again (%s)", exc.get_msg().c_str());
		} catch (const Xapian::NetworkError& exc) {
			if (!t) throw MSG_Error("Problem communicating with the remote database (%s)", exc.get_msg().c_str());
		} catch (const Xapian::Error& exc) {
			throw MSG_Error(exc.get_msg().c_str());
		}
		reopen();
	}

	L_DATABASE_WRAP(this, "Document deleted");
	if (commit_) commit(wal_);
}


Xapian::docid
Database::add_document(const Xapian::Document& doc, bool commit_, bool wal_)
{
	L_CALL(this, "Database::add_document()");

	Xapian::docid did = 0;

#if XAPIAND_DATABASE_WAL
	if (wal_ && wal) wal->write_add_document(doc);
#else
	(void)wal_;
#endif

	Xapian::Document doc_ = doc;

	for (int t = DB_RETRIES; t >= 0; --t) {
		L_DATABASE_WRAP(this, "Adding new document.  t: %d", t);
		Xapian::WritableDatabase *wdb = static_cast<Xapian::WritableDatabase *>(db.get());
		try {
			did = wdb->add_document(doc_);
			modified = true;
			break;
		} catch (const Xapian::DatabaseModifiedError& exc) {
			if (!t) throw MSG_Error("Database was modified, try again (%s)", exc.get_msg().c_str());
		} catch (const Xapian::NetworkError& exc) {
			if (!t) throw MSG_Error("Problem communicating with the remote database (%s)", exc.get_msg().c_str());
		} catch (const Xapian::Error& exc) {
			throw MSG_Error(exc.get_msg().c_str());
		}
		reopen();
	}

	L_DATABASE_WRAP(this, "Document replaced");
	if (commit_) commit(wal_);
	return did;
}


Xapian::docid
Database::replace_document(Xapian::docid did, const Xapian::Document& doc, bool commit_, bool wal_)
{
	L_CALL(this, "Database::replace_document()");

#if XAPIAND_DATABASE_WAL
	if (wal_ && wal) wal->write_replace_document(did, doc);
#else
	(void)wal_;
#endif

	Xapian::Document doc_ = doc;

	for (int t = DB_RETRIES; t >= 0; --t) {
		L_DATABASE_WRAP(this, "Replacing: %d  t: %d", did, t);
		Xapian::WritableDatabase *wdb = static_cast<Xapian::WritableDatabase *>(db.get());
		try {
			wdb->replace_document(did, doc_);
			modified = true;
			break;
		} catch (const Xapian::DatabaseModifiedError& exc) {
			if (!t) throw MSG_Error("Database was modified, try again (%s)", exc.get_msg().c_str());
		} catch (const Xapian::NetworkError& exc) {
			if (!t) throw MSG_Error("Problem communicating with the remote database (%s)", exc.get_msg().c_str());
		} catch (const Xapian::Error& exc) {
			throw MSG_Error(exc.get_msg().c_str());
		}
		reopen();
	}

	L_DATABASE_WRAP(this, "Document replaced");
	if (commit_) commit(wal_);
	return did;
}


Xapian::docid
Database::replace_document(const std::string& doc_id, const Xapian::Document& doc, bool commit_, bool wal_)
{
	L_CALL(this, "Database::replace_document()");
	return replace_document_term(prefixed(doc_id, DOCUMENT_ID_TERM_PREFIX), doc, commit_, wal_);
}


Xapian::docid
Database::replace_document_term(const std::string& term, const Xapian::Document& doc, bool commit_, bool wal_)
{
	L_CALL(this, "Database::replace_document_term()");

	Xapian::docid did = 0;

#if XAPIAND_DATABASE_WAL
	if (wal_ && wal) wal->write_replace_document_term(term, doc);
#else
	(void)wal_;
#endif

	Xapian::Document doc_ = doc;

	for (int t = DB_RETRIES; t >= 0; --t) {
		L_DATABASE_WRAP(this, "Replacing: '%s'  t: %d", term.c_str(), t);
		Xapian::WritableDatabase *wdb = static_cast<Xapian::WritableDatabase *>(db.get());
		try {
			did = wdb->replace_document(term, doc_);
			modified = true;
			break;
		} catch (const Xapian::DatabaseModifiedError& exc) {
			if (!t) throw MSG_Error("Database was modified, try again (%s)", exc.get_msg().c_str());
		} catch (const Xapian::NetworkError& exc) {
			if (!t) throw MSG_Error("Problem communicating with the remote database (%s)", exc.get_msg().c_str());
		} catch (const Xapian::Error& exc) {
			throw MSG_Error(exc.get_msg().c_str());
		}
		reopen();
	}

	L_DATABASE_WRAP(this, "Document replaced");
	if (commit_) commit(wal_);
	return did;
}


void
Database::add_spelling(const std::string & word, Xapian::termcount freqinc, bool commit_, bool wal_)
{
	L_CALL(this, "Database::add_spelling()");

#if XAPIAND_DATABASE_WAL
	if (wal_ && wal) wal->write_add_spelling(word, freqinc);
#else
	(void)wal_;
#endif

	for (int t = DB_RETRIES; t >= 0; --t) {
		Xapian::WritableDatabase *wdb = static_cast<Xapian::WritableDatabase *>(db.get());
		try {
			wdb->add_spelling(word, freqinc);
			modified = true;
			break;
		} catch (const Xapian::DatabaseModifiedError& exc) {
			if (!t) throw MSG_Error("Database was modified, try again (%s)", exc.get_msg().c_str());
		} catch (const Xapian::NetworkError& exc) {
			if (!t) throw MSG_Error("Problem communicating with the remote database (%s)", exc.get_msg().c_str());
		} catch (const Xapian::Error& exc) {
			throw MSG_Error(exc.get_msg().c_str());
		}
		reopen();
	}

	L_DATABASE_WRAP(this, "add_spelling was done");
	if (commit_) commit(wal_);
}


void
Database::remove_spelling(const std::string & word, Xapian::termcount freqdec, bool commit_, bool wal_)
{
	L_CALL(this, "Database::remove_spelling()");

#if XAPIAND_DATABASE_WAL
	if (wal_ && wal) wal->write_remove_spelling(word, freqdec);
#else
	(void)wal_;
#endif

	for (int t = DB_RETRIES; t >= 0; --t) {
		Xapian::WritableDatabase *wdb = static_cast<Xapian::WritableDatabase *>(db.get());
		try {
			wdb->remove_spelling(word, freqdec);
			modified = true;
			break;
		} catch (const Xapian::DatabaseModifiedError& exc) {
			if (!t) throw MSG_Error("Database was modified, try again (%s)", exc.get_msg().c_str());
		} catch (const Xapian::NetworkError& exc) {
			if (!t) throw MSG_Error("Problem communicating with the remote database (%s)", exc.get_msg().c_str());
		} catch (const Xapian::Error& exc) {
			throw MSG_Error(exc.get_msg().c_str());
		}
		reopen();
	}

	L_DATABASE_WRAP(this, "remove_spelling was done");
	if (commit_) commit(wal_);
}


std::string
Database::get_metadata(const std::string& key)
{
	L_CALL(this, "Database::get_metadata()");

	std::string value;

	for (int t = DB_RETRIES; t >= 0; --t) {
		try {
			value = db->get_metadata(key);
			break;
		} catch (const Xapian::DatabaseModifiedError& exc) {
			if (!t) throw MSG_Error("Database was modified, try again (%s)", exc.get_msg().c_str());
		} catch (const Xapian::NetworkError& exc) {
			if (!t) throw MSG_Error("Problem communicating with the remote database (%s)", exc.get_msg().c_str());
		} catch (const Xapian::InvalidArgumentError&) {
			break;
		} catch (const Xapian::Error& exc) {
			throw MSG_Error(exc.get_msg().c_str());
		}
		reopen();
	}

	L_DATABASE_WRAP(this, "get_metadata was done");
	return value;
}


void
Database::set_metadata(const std::string& key, const std::string& value, bool commit_, bool wal_)
{
	L_CALL(this, "Database::set_metadata()");

#if XAPIAND_DATABASE_WAL
	if (wal_ && wal) wal->write_set_metadata(key, value);
#else
	(void)wal_;
#endif

	for (int t = DB_RETRIES; t >= 0; --t) {
		Xapian::WritableDatabase *wdb = static_cast<Xapian::WritableDatabase *>(db.get());
		try {
			wdb->set_metadata(key, value);
			modified = true;
			break;
		} catch (const Xapian::DatabaseModifiedError& exc) {
			if (!t) throw MSG_Error("Database was modified, try again (%s)", exc.get_msg().c_str());
		} catch (const Xapian::NetworkError& exc) {
			if (!t) throw MSG_Error("Problem communicating with the remote database (%s)", exc.get_msg().c_str());
		} catch (const Xapian::Error& exc) {
			throw MSG_Error(exc.get_msg().c_str());
		}
		reopen();
	}

	L_DATABASE_WRAP(this, "set_metadata was done");
	if (commit_) commit(wal_);
}


Xapian::Document
Database::get_document(const Xapian::docid& did)
{
	L_CALL(this, "Database::get_document()");

	Xapian::Document doc;

	for (int t = DB_RETRIES; t >= 0; --t) {
		try {
			doc = db->get_document(did);
			break;
		} catch (const Xapian::DatabaseModifiedError& exc) {
			if (!t) throw MSG_Error("Database was modified, try again (%s)", exc.get_msg().c_str());
		} catch (const Xapian::NetworkError& exc) {
			if (!t) throw MSG_Error("Problem communicating with the remote database (%s)", exc.get_msg().c_str());
		} catch (const Xapian::InvalidArgumentError&) {
			throw MSG_DocNotFoundError("Document not found");
		} catch (const Xapian::DocNotFoundError&) {
			throw MSG_DocNotFoundError("Document not found");
		} catch (const Xapian::Error& exc) {
			throw MSG_Error(exc.get_msg().c_str());
		}
		reopen();
	}

	L_DATABASE_WRAP(this, "get_document was done");
	return doc;
}


Xapian::Document
Database::get_document(const std::string& doc_id)
{
	L_CALL(this, "Database::get_document()");
	Xapian::Query query(prefixed(doc_id, DOCUMENT_ID_TERM_PREFIX));

	Xapian::Document doc;

	for (int t = DB_RETRIES; t >= 0; --t) {
		try {
			Xapian::Enquire enquire(*db);
			enquire.set_query(query);
			auto mset = enquire.get_mset(0, 1);
			if (mset.empty()) {
				throw MSG_DocNotFoundError("Document not found");
			}
			doc = get_document(*mset.begin());
			break;
		} catch (const Xapian::DatabaseModifiedError& exc) {
			if (!t) throw MSG_Error("Database was modified, try again (%s)", exc.get_msg().c_str());
		} catch (const Xapian::NetworkError& exc) {
			if (!t) throw MSG_Error("Problem communicating with the remote database (%s)", exc.get_msg().c_str());
		} catch (const Xapian::InvalidArgumentError&) {
			throw MSG_DocNotFoundError("Document not found");
		} catch (const Xapian::DocNotFoundError&) {
			throw MSG_DocNotFoundError("Document not found");
		} catch (const Xapian::Error& exc) {
			throw MSG_Error(exc.get_msg().c_str());
		}
		reopen();
	}

	L_DATABASE_WRAP(this, "get_document was done");
	return doc;
}


std::string
Database::get_value(const Xapian::Document& document, Xapian::valueno slot)
{
	L_CALL(this, "Database::get_value()");

	Xapian::Document doc = document;
	std::string value;

	for (int t = DB_RETRIES; t >= 0; --t) {
		try {
			value = doc.get_value(slot);
			break;
		} catch (const Xapian::DatabaseModifiedError& exc) {
			if (!t) throw MSG_Error("Database was modified, try again (%s)", exc.get_msg().c_str());
		} catch (const Xapian::NetworkError& exc) {
			if (!t) throw MSG_Error("Problem communicating with the remote database (%s)", exc.get_msg().c_str());
		} catch (const Xapian::Error& exc) {
			throw MSG_Error(exc.get_msg().c_str());
		}
		reopen();
		doc = get_document(document.get_docid());
	}

	L_DATABASE_WRAP(this, "get_value was done");
	return value;
}


MsgPack
Database::get_value(const Xapian::Document& document, const std::string& slot_name)
{
	std::shared_ptr<const Schema> schema = XapiandManager::manager->database_pool.get_schema(endpoints[0]);
	auto slot_field = schema->get_slot_field(slot_name);

	std::string value = get_value(document, slot_field.slot);

	MsgPack result;
	try {
		Unserialise::unserialise(slot_field.type, value, result);
	} catch (const SerialisationError& exc) {
		throw MSG_Error("Problem unserializing value (%s)", exc.get_msg().c_str());
	}
	return result;
}


//  ____        _        _                     ___
// |  _ \  __ _| |_ __ _| |__   __ _ ___  ___ / _ \ _   _  ___ _   _  ___
// | | | |/ _` | __/ _` | '_ \ / _` / __|/ _ \ | | | | | |/ _ \ | | |/ _ \
// | |_| | (_| | || (_| | |_) | (_| \__ \  __/ |_| | |_| |  __/ |_| |  __/
// |____/ \__,_|\__\__,_|_.__/ \__,_|___/\___|\__\_\\__,_|\___|\__,_|\___|
//
////////////////////////////////////////////////////////////////////////////////


DatabaseQueue::DatabaseQueue()
	: state(replica_state::REPLICA_FREE),
	  persistent(false),
	  count(0) {
	L_OBJ(this, "CREATED DATABASE QUEUE!");
}


DatabaseQueue::DatabaseQueue(DatabaseQueue&& q)
{
	std::lock_guard<std::mutex> lk(q._mutex);
	_items_queue = std::move(q._items_queue);
	_limit = std::move(q._limit);
	state = std::move(q.state);
	persistent = std::move(q.persistent);
	count = std::move(q.count);
	weak_database_pool = std::move(q.weak_database_pool);

	L_OBJ(this, "CREATED DATABASE QUEUE!");
}


DatabaseQueue::~DatabaseQueue()
{
	if (size() != count) {
		L_CRIT(this, "Inconsistency in the number of databases in queue");
		sig_exit(-EX_SOFTWARE);
	}

	L_OBJ(this, "DELETED DATABASE QUEUE!");
}


bool
DatabaseQueue::inc_count(int max)
{
	L_CALL(this, "DatabaseQueue::inc_count()");

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
	L_CALL(this, "DatabaseQueue::dec_count()");

	std::unique_lock<std::mutex> lk(_mutex);

	if (count <= 0) {
		L_CRIT(this, "Inconsistency in the number of databases in queue");
		sig_exit(-EX_SOFTWARE);
	}

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


//  ____        _        _                    ____             _
// |  _ \  __ _| |_ __ _| |__   __ _ ___  ___|  _ \ ___   ___ | |
// | | | |/ _` | __/ _` | '_ \ / _` / __|/ _ \ |_) / _ \ / _ \| |
// | |_| | (_| | || (_| | |_) | (_| \__ \  __/  __/ (_) | (_) | |
// |____/ \__,_|\__\__,_|_.__/ \__,_|___/\___|_|   \___/ \___/|_|
//
////////////////////////////////////////////////////////////////////////////////


DatabasePool::DatabasePool(size_t max_size)
	: finished(false),
	  databases(max_size),
	  writable_databases(max_size),
	  schemas(max_size) {
	L_OBJ(this, "CREATED DATABASE POLL!");
}


DatabasePool::~DatabasePool()
{
	finish();

	L_OBJ(this, "DELETED DATABASE POOL!");
}


void
DatabasePool::add_endpoint_queue(const Endpoint& endpoint, const std::shared_ptr<DatabaseQueue>& queue)
{
	L_CALL(this, "DatabasePool::add_endpoint_queue()");

	size_t hash = endpoint.hash();
	auto& queues_set = queues[hash];
	queues_set.insert(queue);
}


void
DatabasePool::drop_endpoint_queue(const Endpoint& endpoint, const std::shared_ptr<DatabaseQueue>& queue)
{
	L_CALL(this, "DatabasePool::drop_endpoint_queue()");

	size_t hash = endpoint.hash();
	auto& queues_set = queues[hash];
	queues_set.erase(queue);

	if (queues_set.empty()) {
		queues.erase(hash);
	}
}


long long
DatabasePool::get_mastery_level(const std::string& dir)
{
	L_CALL(this, "DatabasePool::get_mastery_level()");

	Endpoints endpoints;
	endpoints.add(Endpoint(dir));

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

	writable_databases.finish();
	databases.finish();

	L_OBJ(this, "FINISH DATABASE!");
}


bool
DatabasePool::checkout(std::shared_ptr<Database>& database, const Endpoints& endpoints, int flags)
{
	L_CALL(this, "DatabasePool::checkout()");

	bool writable = flags & DB_WRITABLE;
	bool persistent = flags & DB_PERSISTENT;
	bool initref = flags & DB_INIT_REF;
	bool replication = flags & DB_REPLICATION;

	L_DATABASE_BEGIN(this, "++ CHECKING OUT DB [%s]: %s ...", writable ? "WR" : "RO", endpoints.as_string().c_str());

	if (database) {
		L_ERR(this, "Trying to checkout a database with a not null pointer");
		return false;
	}
	if (writable && endpoints.size() != 1) {
		L_ERR(this, "ERROR: Expecting exactly one database, %d requested: %s", endpoints.size(), endpoints.as_string().c_str());
		return false;
	}

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
					L_DATABASE_END(this, "!! ABORTED CHECKOUT DB [%s]: %s", writable ? "WR" : "RO", endpoints.as_string().c_str());
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
				bool count = queue->count;
				lk.unlock();
				try {
					database = std::make_shared<Database>(queue, endpoints, flags);

					if (writable && initref) {
						init_ref(endpoints[0]);
					}

#ifdef XAPIAND_DATABASE_WAL
					if (!writable && count == 1) {
						bool reopen = false;
						for (auto& endpoint : database->endpoints) {
							if (endpoint.is_local()) {
								Endpoints e;
								e.add(endpoint);
								std::shared_ptr<Database> d;
								checkout(d, e, DB_WRITABLE | DB_VOLATILE);
								// Checkout executes any commands from the WAL
								reopen = true;
								checkin(d);
							}
						}
						if (reopen) {
							database->reopen();
						}
					}
#endif
				} catch (const Xapian::DatabaseOpeningError& exc) {
					L_DEBUG(this, "DEBUG: %s", exc.get_msg().c_str());
				} catch (const Xapian::Error& exc) {
					L_EXC(this, "ERROR: %s", exc.get_msg().c_str());
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

	if (!database) {
		L_DATABASE_END(this, "!! FAILED CHECKOUT DB [%s]: %s", writable ? "WR" : "WR", endpoints.as_string().c_str());
		return false;
	}

	if (!writable && duration_cast<seconds>(system_clock::now() -  database->access_time).count() >= DATABASE_UPDATE_TIME) {
		database->reopen();
		L_DATABASE(this, "== REOPEN DB [%s]: %s", (database->flags & DB_WRITABLE) ? "WR" : "RO", database->endpoints.as_string().c_str());
	}

	L_DATABASE_END(this, "++ CHECKED OUT DB [%s]: %s (rev:%s)", writable ? "WR" : "WR", endpoints.as_string().c_str(), repr(database->checkout_revision, false).c_str());
	return true;
}


void
DatabasePool::checkin(std::shared_ptr<Database>& database)
{
	L_CALL(this, "DatabasePool::checkin()");

	L_DATABASE_BEGIN(this, "-- CHECKING IN DB [%s]: %s ...", (database->flags & DB_WRITABLE) ? "WR" : "RO", database->endpoints.as_string().c_str());

	assert(database);

	std::unique_lock<std::mutex> lk(qmtx);

	std::shared_ptr<DatabaseQueue> queue;

	if (database->flags & DB_WRITABLE) {
		Endpoint& endpoint = database->endpoints[0];
		if (endpoint.is_local()) {
			std::string new_revision = database->get_revision_info();
			if (database->checkout_revision != new_revision) {
				database->checkout_revision = new_revision;
				if (database->mastery_level != -1) {
					endpoint.mastery_level = database->mastery_level;
					updated_databases.push(endpoint);
				}
			}
		}
		queue = writable_databases[database->hash];
	} else {
		queue = databases[database->hash];
	}

	assert(database->weak_queue.lock() == queue);

	int flags = database->flags;

	if (database->modified) {
		DatabaseAutocommit::commit(database);
	}

	if (!(flags & DB_VOLATILE)) {
		queue->push(database);
	}

	Endpoints& endpoints = database->endpoints;
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

	if (queue->count < queue->size()) {
		L_CRIT(this, "Inconsistency in the number of databases in queue");
		sig_exit(-EX_SOFTWARE);
	}

	L_DATABASE_END(this, "-- CHECKED IN DB [%s]: %s", (flags & DB_WRITABLE) ? "WR" : "RO", endpoints.as_string().c_str());

	database.reset();

	lk.unlock();

	if (signal_checkins) {
		while (queue->checkin_callbacks.call());
	}
}


bool
DatabasePool::_switch_db(const Endpoint& endpoint)
{
	L_CALL(this, "DatabasePool::_switch_db()");

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
DatabasePool::switch_db(const Endpoint& endpoint)
{
	L_CALL(this, "DatabasePool::switch_db()");

	std::lock_guard<std::mutex> lk(qmtx);
	return _switch_db(endpoint);
}


void
DatabasePool::init_ref(const Endpoint& endpoint)
{
	L_CALL(this, "DatabasePool::init_ref()");

	Endpoints ref_endpoints;
	ref_endpoints.add(Endpoint(".refs"));
	std::shared_ptr<Database> ref_database;
	if (!checkout(ref_database, ref_endpoints, DB_WRITABLE | DB_SPAWN | DB_PERSISTENT | DB_NOWAL)) {
		L_CRIT(this, "Cannot open %s database.", ref_endpoints.as_string().c_str());
		return;
	}

	std::string unique_id(prefixed(get_slot_hex(endpoint.path), DOCUMENT_ID_TERM_PREFIX));
	Xapian::PostingIterator p(ref_database->db->postlist_begin(unique_id));
	if (p == ref_database->db->postlist_end(unique_id)) {
		Xapian::Document doc;
		// Boolean term for the node.
		doc.add_boolean_term(unique_id);
		// Start values for the DB.
		doc.add_boolean_term(prefixed(DB_MASTER, get_prefix("master", DOCUMENT_CUSTOM_TERM_PREFIX, STRING_TYPE)));
		doc.add_value(DB_SLOT_CREF, "0");
		try {
			ref_database->replace_document_term(unique_id, doc, true);
		} catch (const Exception& exc) {
			L_EXC(this, "ERROR: %s", *exc.get_context() ? exc.get_context() : "Unkown exception!");
		}
	}

	checkin(ref_database);
}


void
DatabasePool::inc_ref(const Endpoint& endpoint)
{
	L_CALL(this, "DatabasePool::inc_ref()");

	Endpoints ref_endpoints;
	ref_endpoints.add(Endpoint(".refs"));
	std::shared_ptr<Database> ref_database;
	if (!checkout(ref_database, ref_endpoints, DB_WRITABLE | DB_SPAWN | DB_PERSISTENT | DB_NOWAL)) {
		L_CRIT(this, "Cannot open %s database.", ref_endpoints.as_string().c_str());
		return;
	}

	Xapian::Document doc;

	std::string unique_id(prefixed(get_slot_hex(endpoint.path), DOCUMENT_ID_TERM_PREFIX));
	Xapian::PostingIterator p = ref_database->db->postlist_begin(unique_id);
	if (p == ref_database->db->postlist_end(unique_id)) {
		// QUESTION: Document not found - should add?
		// QUESTION: This case could happen?
		doc.add_boolean_term(unique_id);
		doc.add_value(0, "0");
		try {
			ref_database->replace_document_term(unique_id, doc, true);
		} catch (const Exception& exc) {
			L_EXC(this, "ERROR: %s", *exc.get_context() ? exc.get_context() : "Unkown exception!");
		}
	} else {
		// Document found - reference increased
		doc = ref_database->db->get_document(*p);
		doc.add_boolean_term(unique_id);
		int nref = std::stoi(doc.get_value(0));
		doc.add_value(0, std::to_string(nref + 1));
		try {
			ref_database->replace_document_term(unique_id, doc, true);
		} catch (const Exception& exc) {
			L_EXC(this, "ERROR: %s", *exc.get_context() ? exc.get_context() : "Unkown exception!");
		}
	}

	checkin(ref_database);
}


void
DatabasePool::dec_ref(const Endpoint& endpoint)
{
	L_CALL(this, "DatabasePool::dec_ref()");

	Endpoints ref_endpoints;
	ref_endpoints.add(Endpoint(".refs"));
	std::shared_ptr<Database> ref_database;
	if (!checkout(ref_database, ref_endpoints, DB_WRITABLE | DB_SPAWN | DB_PERSISTENT | DB_NOWAL)) {
		L_CRIT(this, "Cannot open %s database.", ref_endpoints.as_string().c_str());
		return;
	}

	Xapian::Document doc;

	std::string unique_id(prefixed(get_slot_hex(endpoint.path), DOCUMENT_ID_TERM_PREFIX));
	Xapian::PostingIterator p = ref_database->db->postlist_begin(unique_id);
	if (p != ref_database->db->postlist_end(unique_id)) {
		doc = ref_database->db->get_document(*p);
		doc.add_boolean_term(unique_id);
		int nref = std::stoi(doc.get_value(0)) - 1;
		doc.add_value(0, std::to_string(nref));
		try {
			ref_database->replace_document_term(unique_id, doc, true);
		} catch (const Exception& exc) {
			L_EXC(this, "ERROR: %s", *exc.get_context() ? exc.get_context() : "Unkown exception!");
		}
		if (nref == 0) {
			// qmtx need a lock
			delete_files(endpoint.path);
		}
	}

	checkin(ref_database);
}


int
DatabasePool::get_master_count()
{
	L_CALL(this, "DatabasePool::get_master_count()");

	Endpoints ref_endpoints;
	ref_endpoints.add(Endpoint(".refs"));
	std::shared_ptr<Database> ref_database;
	if (!checkout(ref_database, ref_endpoints, DB_WRITABLE | DB_SPAWN | DB_PERSISTENT | DB_NOWAL)) {
		L_CRIT(this, "Cannot open %s database.", ref_endpoints.as_string().c_str());
		return -1;
	}

	int count = 0;

	if (ref_database) {
		Xapian::PostingIterator p(ref_database->db->postlist_begin(DB_MASTER));
		count = std::distance(ref_database->db->postlist_begin(DB_MASTER), ref_database->db->postlist_end(DB_MASTER));
	}

	checkin(ref_database);

	return count;
}


std::shared_ptr<const Schema>
DatabasePool::get_schema(const Endpoint& endpoint, int flags)
{
	L_CALL(this, "DatabasePool::get_schema()");

	if (finished) return nullptr;

	std::shared_ptr<const Schema>* schema;
	{
		std::lock_guard<std::mutex> lk(smtx);
		schema = &schemas[endpoint.hash()];
	}

	if (!*schema) {
		std::string schema_str;
		std::shared_ptr<Database> database;
		if (checkout(database, Endpoints(endpoint), flags != -1 ? flags : DB_WRITABLE)) {
			schema_str = database->get_metadata(RESERVED_SCHEMA);
			checkin(database);
		} else {
			throw MSG_CheckoutError("Cannot checkout database: %s", endpoint.as_string().c_str());
		}
		auto schema_ptr = new Schema();
		schema_ptr->build_schema(schema_str);

		std::atomic_exchange(schema, std::shared_ptr<const Schema>(schema_ptr));
	}
	return *schema;
}


void
DatabasePool::set_schema(const Endpoint& endpoint, int flags, std::shared_ptr<const Schema> new_schema)
{
	L_CALL(this, "DatabasePool::set_schema()");

	std::shared_ptr<const Schema>* schema;
	{
		std::lock_guard<std::mutex> lk(smtx);
		schema = &schemas[endpoint.hash()];
	}

	std::atomic_exchange(schema, new_schema);

	std::shared_ptr<Database> database;
	if (checkout(database, Endpoints(endpoint), flags != -1 ? flags : DB_WRITABLE)) {
		database->set_metadata(RESERVED_SCHEMA, (*schema)->to_string());
		checkin(database);
	} else {
		throw MSG_CheckoutError("Cannot checkout database: %s", endpoint.as_string().c_str());
	}
}


//  ____       _                          _     ____  _   _
// / ___|  ___| |__   ___ _ __ ___   __ _| |   |  _ \| | | |
// \___ \ / __| '_ \ / _ \ '_ ` _ \ / _` | |   | |_) | | | |
//  ___) | (__| | | |  __/ | | | | | (_| | |___|  _ <| |_| |
// |____/ \___|_| |_|\___|_| |_| |_|\__,_|_____|_| \_\\___/
//
///////////////////////////////////////////////////////////////////////////////


SchemaLRU::SchemaLRU(ssize_t max_size)
	: LRU(max_size) { }


bool
ExpandDeciderFilterPrefixes::operator()(const std::string& term) const
{
	for (const auto& prefix : prefixes) {
		if (startswith(term, prefix)) {
			return true;
		}
	}

	return prefixes.empty();
}
