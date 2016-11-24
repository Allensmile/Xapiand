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

#pragma once

#include "xapiand.h"

#include <sys/types.h>          // for uint32_t, uint8_t, ssize_t
#include <xapian.h>             // for docid, termcount, Document, ExpandDecider
#include <atomic>               // for atomic_bool
#include <chrono>               // for system_clock, system_clock::time_point
#include <cstring>              // for size_t
#include <list>                 // for __list_iterator, operator!=
#include <memory>               // for shared_ptr, enable_shared_from_this, mak...
#include <mutex>                // for mutex, condition_variable, unique_lock
#include <stdexcept>            // for range_error
#include <string>               // for string, operator!=
#include <type_traits>          // for forward
#include <unordered_map>        // for unordered_map
#include <unordered_set>        // for unordered_set
#include <utility>              // for pair, make_pair
#include <vector>               // for vector

#include "atomic_shared_ptr.h"  // for atomic_shared_ptr
#include "database_utils.h"     // for DB_WRITABLE
#include "endpoint.h"           // for Endpoints, Endpoint
#include "queue.h"              // for Queue, QueueSet
#include "storage.h"            // for STORAGE_BLOCK_SIZE, MSG_StorageCorruptVo...
#include "threadpool.h"         // for TaskQueue
#include "lru.h"                // for LRU, DropAction, LRU<>::iterator, DropAc...

class Database;
class DatabasePool;
class DatabaseQueue;
class DatabasesLRU;
class MsgPack;
struct WalHeader;


#define WAL_SLOTS ((STORAGE_BLOCK_SIZE - sizeof(WalHeader::StorageHeaderHead)) / sizeof(uint32_t))


#if XAPIAND_DATABASE_WAL
struct WalHeader {
	struct StorageHeaderHead {
		uint32_t magic;
		uint32_t offset;
		char uuid[36];
		uint32_t revision;
	} head;

	uint32_t slot[WAL_SLOTS];

	void init(void* param, void* args);
	void validate(void* param, void* args);
};


#pragma pack(push, 1)
struct WalBinHeader {
	uint8_t magic;
	uint8_t flags;  // required
	uint32_t size;  // required

	inline void init(void*, void*, uint32_t size_, uint8_t flags_) {
		magic = STORAGE_BIN_HEADER_MAGIC;
		size = size_;
		flags = (0 & ~STORAGE_FLAG_MASK) | flags_;
	}

	inline void validate(void*, void*) {
		if (magic != STORAGE_BIN_HEADER_MAGIC) {
			THROW(StorageCorruptVolume, "Bad line header magic number");
		}
		if (flags & STORAGE_FLAG_DELETED) {
			THROW(StorageNotFound, "Line deleted");
		}
	}
};


struct WalBinFooter {
	uint32_t checksum;
	uint8_t magic;

	inline void init(void*, void*, uint32_t checksum_) {
		magic = STORAGE_BIN_FOOTER_MAGIC;
		checksum = checksum_;
	}

	inline void validate(void*, void*, uint32_t checksum_) {
		if (magic != STORAGE_BIN_FOOTER_MAGIC) {
			THROW(StorageCorruptVolume, "Bad line footer magic number");
		}
		if (checksum != checksum_) {
			THROW(StorageCorruptVolume, "Bad line checksum");
		}
	}
};
#pragma pack(pop)


class DatabaseWAL : Storage<WalHeader, WalBinHeader, WalBinFooter> {
	friend WalHeader;

	static constexpr const char* const names[] = {
		"ADD_DOCUMENT",
		"CANCEL",
		"DELETE_DOCUMENT_TERM",
		"COMMIT",
		"REPLACE_DOCUMENT",
		"REPLACE_DOCUMENT_TERM",
		"DELETE_DOCUMENT",
		"SET_METADATA",
		"ADD_SPELLING",
		"REMOVE_SPELLING",
	};
	bool modified;
	bool validate_uuid;

	bool execute(const std::string& line);
	uint32_t highest_valid_slot();

	inline void open(const std::string& path, int flags, bool commit_eof=false) {
		Storage<WalHeader, WalBinHeader, WalBinFooter>::open(path, flags, reinterpret_cast<void*>(commit_eof));
	}

public:
	enum class Type {
		ADD_DOCUMENT,
		CANCEL,
		DELETE_DOCUMENT_TERM,
		COMMIT,
		REPLACE_DOCUMENT,
		REPLACE_DOCUMENT_TERM,
		DELETE_DOCUMENT,
		SET_METADATA,
		ADD_SPELLING,
		REMOVE_SPELLING,
		MAX
	};

	Database* database;

	DatabaseWAL(Database* database_);
	~DatabaseWAL();

	bool open_current(const std::string& path, bool current);

	bool init_database(const std::string& dir);
	void write_line(Type type, const std::string& data, bool commit=false);
	void write_add_document(const Xapian::Document& doc);
	void write_cancel();
	void write_delete_document_term(const std::string& term);
	void write_commit();
	void write_replace_document(Xapian::docid did, const Xapian::Document& doc);
	void write_replace_document_term(const std::string& term, const Xapian::Document& doc);
	void write_delete_document(Xapian::docid did);
	void write_set_metadata(const std::string& key, const std::string& val);
	void write_add_spelling(const std::string& word, Xapian::termcount freqinc);
	void write_remove_spelling(const std::string& word, Xapian::termcount freqdec);
};
#endif


class Database {
public:
	std::weak_ptr<DatabaseQueue> weak_queue;
	Endpoints endpoints;
	int flags;
	size_t hash;
	std::chrono::system_clock::time_point access_time;
	bool modified;
	long long mastery_level;
	uint32_t checkout_revision;

	std::unique_ptr<Xapian::Database> db;

#if XAPIAND_DATABASE_WAL
	std::unique_ptr<DatabaseWAL> wal;
#endif

	Database(std::shared_ptr<DatabaseQueue>& queue_, const Endpoints& endpoints, int flags);
	~Database();

	long long read_mastery(const Endpoint& endpoint);

	bool reopen();

	std::string get_uuid() const;
	uint32_t get_revision() const;
	std::string get_revision_str() const;

	bool commit(bool wal_=true);
	void cancel(bool wal_=true);

	void delete_document(Xapian::docid did, bool commit_=false, bool wal_=true);
	void delete_document_term(const std::string& term, bool commit_=false, bool wal_=true);
	Xapian::docid add_document(const Xapian::Document& doc, bool commit_=false, bool wal_=true);
	Xapian::docid replace_document(Xapian::docid did, const Xapian::Document& doc, bool commit_=false, bool wal_=true);
	Xapian::docid replace_document(const std::string& doc_id, const Xapian::Document& doc, bool commit_=false, bool wal_=true);
	Xapian::docid replace_document_term(const std::string& term, const Xapian::Document& doc, bool commit_=false, bool wal_=true);

	void add_spelling(const std::string & word, Xapian::termcount freqinc, bool commit_=false, bool wal_=true);
	void remove_spelling(const std::string & word, Xapian::termcount freqdec, bool commit_=false, bool wal_=true);

	Xapian::docid find_document(const Xapian::Query& query);
	Xapian::Document get_document(const Xapian::docid& did);
	std::string get_metadata(const std::string& key);
	void set_metadata(const std::string& key, const std::string& value, bool commit_=false, bool wal_=true);

	std::string to_string() const {
		return endpoints.to_string();
	}
};


class DatabaseQueue : public queue::Queue<std::shared_ptr<Database>>,
			  public std::enable_shared_from_this<DatabaseQueue> {
	// FIXME: Add queue creation time and delete databases when deleted queue

	friend class Database;
	friend class DatabasePool;
	friend class DatabasesLRU;

private:
	enum class replica_state {
		REPLICA_FREE,
		REPLICA_LOCK,
		REPLICA_SWITCH,
	};

	replica_state state;
	bool persistent;

	size_t count;

	std::condition_variable switch_cond;

	std::weak_ptr<DatabasePool> weak_database_pool;
	Endpoints endpoints;

	TaskQueue<> checkin_callbacks;

public:
	DatabaseQueue();
	DatabaseQueue(DatabaseQueue&&);
	~DatabaseQueue();

	bool inc_count(int max=-1);
	bool dec_count();
};


class DatabasesLRU : public lru::LRU<size_t, std::shared_ptr<DatabaseQueue>> {
public:
	DatabasesLRU(ssize_t max_size);

	std::shared_ptr<DatabaseQueue>& operator[] (size_t key);

	void finish();
};


class SchemaLRU: public lru::LRU<size_t, atomic_shared_ptr<const MsgPack>> {
public:
	SchemaLRU(ssize_t max_size=-1);
};


class DatabasePool : public std::enable_shared_from_this<DatabasePool> {
	// FIXME: Add maximum number of databases available for the queue
	// FIXME: Add cleanup for removing old database queues
	friend class DatabaseQueue;

private:
	std::mutex qmtx;
	std::mutex smtx;
	std::atomic_bool finished;

	std::unordered_map<size_t, std::unordered_set<std::shared_ptr<DatabaseQueue>>> queues;

	DatabasesLRU databases;
	DatabasesLRU writable_databases;
	SchemaLRU schemas;

	std::condition_variable checkin_cond;

	void init_ref(const Endpoint& endpoint);
	void inc_ref(const Endpoint& endpoint);
	void dec_ref(const Endpoint& endpoint);
	int get_master_count();

	void add_endpoint_queue(const Endpoint& endpoint, const std::shared_ptr<DatabaseQueue>& queue);
	void drop_endpoint_queue(const Endpoint& endpoint, const std::shared_ptr<DatabaseQueue>& queue);
	bool _switch_db(const Endpoint& endpoint);

public:
	DatabasePool(size_t max_size);
	~DatabasePool();

	long long get_mastery_level(const std::string& dir);

	void finish();

	std::shared_ptr<const MsgPack> get_schema(const Endpoint& endpoint, int flags=-1);
	void set_schema(const Endpoint& endpoint, int flags, std::shared_ptr<const MsgPack> new_schema);

	template<typename F, typename... Args>
	bool checkout(std::shared_ptr<Database>& database, const Endpoints& endpoints, int flags, F&& f, Args&&... args) {
		bool ret = checkout(database, endpoints, flags);
		if (!ret) {
			std::unique_lock<std::mutex> lk(qmtx);

			size_t hash = endpoints.hash();

			std::shared_ptr<DatabaseQueue> queue;
			if (flags & DB_WRITABLE) {
				queue = writable_databases[hash];
			} else {
				queue = databases[hash];
			}

			queue->checkin_callbacks.clear();
			queue->checkin_callbacks.enqueue(std::forward<F>(f), std::forward<Args>(args)...);
		}
		return ret;
	}

	bool checkout(std::shared_ptr<Database>& database, const Endpoints &endpoints, int flags);
	void checkin(std::shared_ptr<Database>& database);
	bool switch_db(const Endpoint &endpoint);

	queue::QueueSet<Endpoint> updated_databases;
};


class ExpandDeciderFilterPrefixes : public Xapian::ExpandDecider {
	std::vector<std::string> prefixes;

public:
	ExpandDeciderFilterPrefixes(const std::vector<std::string>& prefixes_)
		: prefixes(prefixes_) { }

	virtual bool operator() (const std::string& term) const override;
};
