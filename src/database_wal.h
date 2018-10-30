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

#include "config.h"             // for XAPIAND_BINARY_SERVERPORT, XAPIAND_BINARY_PROXY

#if XAPIAND_DATABASE_WAL

#include <array>                // for std::array
#include <string>               // for std::string
#include <sys/types.h>          // for uint32_t, uint8_t, ssize_t
#include <utility>              // for pair, make_pair
#include <xapian.h>             // for Xapian::docid, Xapian::termcount, Xapian::Document

#include "cuuid/uuid.h"         // for UUID
#include "storage.h"            // for Storage, STORAGE_BLOCK_SIZE, StorageCorruptVolume...


class MsgPack;
class Database;
struct WalHeader;


#define WAL_SLOTS ((STORAGE_BLOCK_SIZE - sizeof(WalHeader::StorageHeaderHead)) / sizeof(uint32_t))


struct WalHeader {
	struct StorageHeaderHead {
		uint32_t offset;
		Xapian::rev revision;
		std::array<unsigned char, 16> uuid;
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

	void init(void*, void*, uint32_t size_, uint8_t flags_) {
		magic = STORAGE_BIN_HEADER_MAGIC;
		size = size_;
		flags = (0 & ~STORAGE_FLAG_MASK) | flags_;
	}

	void validate(void*, void*) {
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

	void init(void*, void*, uint32_t checksum_) {
		magic = STORAGE_BIN_FOOTER_MAGIC;
		checksum = checksum_;
	}

	void validate(void*, void*, uint32_t checksum_) {
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
	class iterator;

	friend WalHeader;

	static constexpr const char* const names[] = {
		"ADD_DOCUMENT",
		"DELETE_DOCUMENT_TERM",
		"COMMIT",
		"REPLACE_DOCUMENT",
		"REPLACE_DOCUMENT_TERM",
		"DELETE_DOCUMENT",
		"SET_METADATA",
		"ADD_SPELLING",
		"REMOVE_SPELLING",
		"MAX",
	};

	bool validate_uuid;

	MsgPack repr_document(std::string_view document, bool unserialised);
	MsgPack repr_metadata(std::string_view document, bool unserialised);
	MsgPack repr_line(std::string_view line, bool unserialised);
	uint32_t highest_valid_slot();

	bool open(std::string_view path, int flags, bool commit_eof = false) {
		return Storage<WalHeader, WalBinHeader, WalBinFooter>::open(path, flags, reinterpret_cast<void*>(commit_eof));
	}

public:
	enum class Type : uint8_t {
		ADD_DOCUMENT,
		DELETE_DOCUMENT_TERM,
		COMMIT,
		REPLACE_DOCUMENT,
		REPLACE_DOCUMENT_TERM,
		DELETE_DOCUMENT,
		SET_METADATA,
		ADD_SPELLING,
		REMOVE_SPELLING,
		MAX,
	};

	mutable UUID _uuid;
	mutable UUID _uuid_le;
	Database* database;

	DatabaseWAL(std::string_view base_path_, Database* database_);
	~DatabaseWAL();

	iterator begin();
	iterator end();

	bool open_current(bool only_committed, bool unsafe = false);
	MsgPack repr(Xapian::rev start_revision, Xapian::rev end_revision, bool unserialised);

	const UUID& uuid() const;
	const UUID& uuid_le() const;

	bool init_database();
	bool execute(std::string_view line, bool wal_, bool send_update, bool unsafe);
	void write_line(Type type, std::string_view data, bool send_update);
	void write_add_document(const Xapian::Document& doc);
	void write_delete_document_term(std::string_view term);
	void write_commit(bool send_update);
	void write_replace_document(Xapian::docid did, const Xapian::Document& doc);
	void write_replace_document_term(std::string_view term, const Xapian::Document& doc);
	void write_delete_document(Xapian::docid did);
	void write_set_metadata(std::string_view key, std::string_view val);
	void write_add_spelling(std::string_view word, Xapian::termcount freqinc);
	void write_remove_spelling(std::string_view word, Xapian::termcount freqdec);
	std::pair<bool, unsigned long long> has_revision(Xapian::rev revision);
	iterator find(Xapian::rev revision);
	std::pair<Xapian::rev, std::string> get_current_line(uint32_t end_off);
};


class DatabaseWAL::iterator {
	friend DatabaseWAL;

	DatabaseWAL* wal;
	std::pair<Xapian::rev, std::string> item;
	uint32_t end_off;

public:
	using iterator_category = std::forward_iterator_tag;
	using value_type = std::pair<Xapian::rev, std::string>;
	using difference_type = std::pair<Xapian::rev, std::string>;
	using pointer = std::pair<Xapian::rev, std::string>*;
	using reference = std::pair<Xapian::rev, std::string>&;

	iterator(DatabaseWAL* wal_, std::pair<Xapian::rev, std::string>&& item_, uint32_t end_off_)
		: wal(wal_),
		  item(item_),
		  end_off(end_off_) { }

	iterator& operator++() {
		item = wal->get_current_line(end_off);
		return *this;
	}

	iterator operator=(const iterator& other) {
		wal = other.wal;
		item = other.item;
		return *this;
	}

	std::pair<Xapian::rev, std::string>& operator*() {
		return item;
	}

	std::pair<Xapian::rev, std::string>* operator->() {
		return &operator*();
	}

	std::pair<Xapian::rev, std::string>& value() {
		return item;
	}

	bool operator==(const iterator& other) const {
		return this == &other || item == other.item;
	}

	bool operator!=(const iterator& other) const {
		return !operator==(other);
	}

};


inline DatabaseWAL::iterator DatabaseWAL::begin() {
	return find(0);
}


inline DatabaseWAL::iterator DatabaseWAL::end() {
	return iterator(this, std::make_pair(std::numeric_limits<Xapian::rev>::max() - 1, ""), 0);
}

#endif /* XAPIAND_DATABASE_WAL */
