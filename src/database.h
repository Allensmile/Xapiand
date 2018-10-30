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

#pragma once

#include "config.h"               // for XAPIAND_BINARY_SERVERPORT, XAPIAND_BINARY_PROXY

#include <chrono>                 // for system_clock, system_clock::time_point
#include <cstring>                // for size_t
#include <memory>                 // for std::shared_ptr
#include <string>                 // for std::string
#include <utility>                // for std::pair
#include <vector>                 // for std::vector
#include <xapian.h>               // for Xapian::docid, Xapian::termcount, Xapian::Document

#include "cuuid/uuid.h"           // for UUID, UUID_LENGTH
#include "database_flags.h"       // for Data::Locator
#include "database_data.h"        // for Data::Locator
#include "endpoint.h"             // for Endpoints, Endpoint


class DatabaseWAL;
class DatabaseQueue;
class MsgPack;
class DataStorage;


//  ____        _        _
// |  _ \  __ _| |_ __ _| |__   __ _ ___  ___
// | | | |/ _` | __/ _` | '_ \ / _` / __|/ _ \
// | |_| | (_| | || (_| | |_) | (_| \__ \  __/
// |____/ \__,_|\__\__,_|_.__/ \__,_|___/\___|
//
class Database {
#ifdef XAPIAND_DATA_STORAGE
	void storage_pull_blobs(Xapian::Document& doc, const Xapian::docid& did) const;
	void storage_push_blobs(Xapian::Document& doc, const Xapian::docid& did) const;
	void storage_commit();
#endif /* XAPIAND_DATA_STORAGE */

	void reopen_writable();
	void reopen_readable();
public:
	std::weak_ptr<DatabaseQueue> weak_queue;
	std::weak_ptr<DatabaseQueue> weak_readable_queue;

	Endpoints endpoints;
	int flags;
	size_t hash;
	bool modified;
	bool transaction;
	std::chrono::system_clock::time_point reopen_time;
	Xapian::rev reopen_revision;
	bool incomplete;
	bool closed;

	std::unique_ptr<Xapian::Database> db;
	std::vector<std::pair<Xapian::Database, bool>> dbs;

#if XAPIAND_DATABASE_WAL
	std::unique_ptr<DatabaseWAL> wal;
#endif /* XAPIAND_DATABASE_WAL */

#ifdef XAPIAND_DATA_STORAGE
	std::vector<std::unique_ptr<DataStorage>> writable_storages;
	std::vector<std::unique_ptr<DataStorage>> storages;
#endif /* XAPIAND_DATA_STORAGE */

	Database(std::shared_ptr<DatabaseQueue>& queue_, Endpoints  endpoints_, int flags_);
	~Database();

	bool reopen();

#ifdef XAPIAND_DATA_STORAGE
	std::string storage_get_stored(const Xapian::docid& did, const Data::Locator& locator) const;
#endif /* XAPIAND_DATA_STORAGE */

	UUID get_uuid() const;
	Xapian::rev get_revision() const;

	void close();

	bool commit(bool wal_ = true, bool send_update = true);

	void begin_transaction(bool flushed = true);
	void commit_transaction();
	void cancel_transaction();

	void delete_document(Xapian::docid did, bool commit_ = false, bool wal_ = true);
	void delete_document_term(const std::string& term, bool commit_ = false, bool wal_ = true);
	Xapian::docid add_document(const Xapian::Document& doc, bool commit_ = false, bool wal_ = true);
	Xapian::docid replace_document(Xapian::docid did, const Xapian::Document& doc, bool commit_ = false, bool wal_ = true);
	Xapian::docid replace_document_term(const std::string& term, const Xapian::Document& doc, bool commit_ = false, bool wal_ = true);

	void add_spelling(const std::string& word, Xapian::termcount freqinc, bool commit_ = false, bool wal_ = true);
	void remove_spelling(const std::string& word, Xapian::termcount freqdec, bool commit_ = false, bool wal_ = true);

	Xapian::docid find_document(const std::string& term_id);
	Xapian::Document get_document(const Xapian::docid& did, bool assume_valid_ = false, bool pull_ = false);

	std::vector<std::string> get_metadata_keys();
	std::string get_metadata(const std::string& key);
	void set_metadata(const std::string& key, const std::string& value, bool commit_ = false, bool wal_ = true);

	void dump_metadata(int fd, XXH32_state_t* xxh_state);
	void dump_documents(int fd, XXH32_state_t* xxh_state);
	MsgPack dump_documents();

	std::string to_string() const {
		return endpoints.to_string();
	}
};
