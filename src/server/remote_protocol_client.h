/*
 * Copyright (C) 2015-2019 Dubalu LLC
 * Copyright (C) 2006,2007,2008,2009,2010,2014,2017 Olly Betts
 * Copyright (C) 2007,2009,2010 Lemur Consulting Ltd
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#pragma once

#include "config.h"             // for XAPIAND_CLUSTERING

#ifdef XAPIAND_CLUSTERING

#include <deque>                              // for std::deque
#include <memory>                             // for shared_ptr
#include <mutex>                              // for std::mutex
#include <string>                             // for std::string
#include <vector>                             // for std::vector
#include <xapian.h>

#include "base_client.h"                      // for MetaBaseClient
#include "lock_database.h"
#include "threadpool.hh"                      // for Task

// #define SAVE_LAST_MESSAGES
#if !defined(NDEBUG) || defined(XAPIAND_TRACEBACKS)
#ifndef SAVE_LAST_MESSAGES
#define SAVE_LAST_MESSAGES 1
#endif
#endif


// Versions:
// 21: Overhauled remote backend supporting WritableDatabase
// 22: Lossless double serialisation
// 23: Support get_lastdocid() on remote databases
// 24: Support for OP_VALUE_RANGE in query serialisation
// 25: Support for delete_document and replace_document with unique term
// 26: Tweak delete_document with unique term; delta encode rset and termpos
// 27: Support for postlists (always passes the whole list across)
// 28: Pass document length in reply to MSG_TERMLIST
// 29: Serialisation of Xapian::Error includes error_string
// 30: Add minor protocol version numbers, to reduce need for client upgrades
// 30.1: Pass the prefix parameter for MSG_ALLTERMS, and use it.
// 30.2: New REPLY_DELETEDOCUMENT returns MSG_DONE to allow exceptions.
// 30.3: New MSG_GETMSET which passes check_at_least parameter.
// 30.4: New query operator OP_SCALE_WEIGHT.
// 30.5: New MSG_GETMSET which expects MSet's percent_factor to be returned.
// 30.6: Support for OP_VALUE_GE and OP_VALUE_LE in query serialisation
// 31: 1.1.0 Clean up for Xapian 1.1.0
// 32: 1.1.1 Serialise termfreq and reltermfreqs together in serialise_stats.
// 33: 1.1.3 Support for passing matchspies over the remote connection.
// 34: 1.1.4 Support for metadata over with remote databases.
// 35: 1.1.5 Support for add_spelling() and remove_spelling().
// 35.1: 1.2.4 Support for metadata_keys_begin().
// 36: 1.3.0 REPLY_UPDATE and REPLY_GREETING merged, and more...
// 37: 1.3.1 Prefix-compress termlists.
// 38: 1.3.2 Stats serialisation now includes collection freq, and more...
// 39: 1.3.3 New query operator OP_WILDCARD; sort keys in serialised MSet.
// 39.1: pre-1.5.0 MSG_POSITIONLISTCOUNT added.
// 40: pre-1.5.0 REPLY_REMOVESPELLING added.
// 41: 1.5.0 Changed REPLY_ALLTERMS, REPLY_METADATAKEYLIST, REPLY_TERMLIST.
#if XAPIAN_AT_LEAST(1, 5, 0)
#define XAPIAN_REMOTE_PROTOCOL_MAJOR_VERSION 41
#define XAPIAN_REMOTE_PROTOCOL_MINOR_VERSION 0
#else
#define XAPIAN_REMOTE_PROTOCOL_MAJOR_VERSION 39
#define XAPIAN_REMOTE_PROTOCOL_MINOR_VERSION 1
#endif

#define FILE_FOLLOWS '\xfd'


enum class State {
	INIT_REMOTE,
	REMOTE_SERVER,
};


inline const std::string& StateNames(State type) {
	static const std::string _[] = {
		"INIT_REMOTE",
		"REMOTE_SERVER",
	};
	auto idx = static_cast<size_t>(type);
	if (idx >= 0 && idx < sizeof(_) / sizeof(_[0])) {
		return _[idx];
	}
	static const std::string UNKNOWN = "UNKNOWN";
	return UNKNOWN;
}


enum class RemoteMessageType {
	MSG_ALLTERMS,               // All Terms
	MSG_COLLFREQ,               // Get Collection Frequency
	MSG_DOCUMENT,               // Get Document
	MSG_TERMEXISTS,             // Term Exists?
	MSG_TERMFREQ,               // Get Term Frequency
	MSG_VALUESTATS,             // Get value statistics
	MSG_KEEPALIVE,              // Keep-alive
	MSG_DOCLENGTH,              // Get Doc Length
	MSG_QUERY,                  // Run Query
	MSG_TERMLIST,               // Get TermList
	MSG_POSITIONLIST,           // Get PositionList
	MSG_POSTLIST,               // Get PostList
	MSG_REOPEN,                 // Reopen
	MSG_UPDATE,                 // Get Updated DocCount and AvLength
	MSG_ADDDOCUMENT,            // Add Document
	MSG_CANCEL,                 // Cancel
	MSG_DELETEDOCUMENTTERM,     // Delete Document by term
	MSG_COMMIT,                 // Commit
	MSG_REPLACEDOCUMENT,        // Replace Document
	MSG_REPLACEDOCUMENTTERM,    // Replace Document by term
	MSG_DELETEDOCUMENT,         // Delete Document
	MSG_WRITEACCESS,            // Upgrade to WritableDatabase
	MSG_GETMETADATA,            // Get metadata
	MSG_SETMETADATA,            // Set metadata
	MSG_ADDSPELLING,            // Add a spelling
	MSG_REMOVESPELLING,         // Remove a spelling
	MSG_GETMSET,                // Get MSet
	MSG_SHUTDOWN,               // Shutdown
	MSG_METADATAKEYLIST,        // Iterator for metadata keys
	MSG_FREQS,                  // Get termfreq and collfreq
	MSG_UNIQUETERMS,            // Get number of unique terms in doc
	MSG_POSITIONLISTCOUNT,      // Get PositionList length
	MSG_READACCESS,             // Select current database
	MSG_MAX
};


inline const std::string& RemoteMessageTypeNames(RemoteMessageType type) {
	static const std::string _[] = {
		"MSG_ALLTERMS", "MSG_COLLFREQ", "MSG_DOCUMENT", "MSG_TERMEXISTS",
		"MSG_TERMFREQ", "MSG_VALUESTATS", "MSG_KEEPALIVE", "MSG_DOCLENGTH",
		"MSG_QUERY", "MSG_TERMLIST", "MSG_POSITIONLIST", "MSG_POSTLIST",
		"MSG_REOPEN", "MSG_UPDATE", "MSG_ADDDOCUMENT", "MSG_CANCEL",
		"MSG_DELETEDOCUMENTTERM", "MSG_COMMIT", "MSG_REPLACEDOCUMENT",
		"MSG_REPLACEDOCUMENTTERM", "MSG_DELETEDOCUMENT", "MSG_WRITEACCESS",
		"MSG_GETMETADATA", "MSG_SETMETADATA", "MSG_ADDSPELLING",
		"MSG_REMOVESPELLING", "MSG_GETMSET", "MSG_SHUTDOWN",
		"MSG_METADATAKEYLIST", "MSG_FREQS", "MSG_UNIQUETERMS",
		"MSG_POSITIONLISTCOUNT", "MSG_READACCESS",
	};
	auto idx = static_cast<size_t>(type);
	if (idx >= 0 && idx < sizeof(_) / sizeof(_[0])) {
		return _[idx];
	}
	static const std::string UNKNOWN = "UNKNOWN";
	return UNKNOWN;
}


enum class RemoteReplyType {
	REPLY_UPDATE,               // Updated database stats
	REPLY_EXCEPTION,            // Exception
	REPLY_DONE,                 // Done sending list
	REPLY_ALLTERMS,             // All Terms
	REPLY_COLLFREQ,             // Get Collection Frequency
	REPLY_DOCDATA,              // Get Document
	REPLY_TERMDOESNTEXIST,      // Term Doesn't Exist
	REPLY_TERMEXISTS,           // Term Exists
	REPLY_TERMFREQ,             // Get Term Frequency
	REPLY_VALUESTATS,           // Value statistics
	REPLY_DOCLENGTH,            // Get Doc Length
	REPLY_STATS,                // Stats
	REPLY_TERMLIST,             // Get Termlist
	REPLY_POSITIONLIST,         // Get PositionList
	REPLY_POSTLISTSTART,        // Start of a postlist
	REPLY_POSTLISTITEM,         // Item in body of a postlist
	REPLY_VALUE,                // Document Value
	REPLY_ADDDOCUMENT,          // Add Document
	REPLY_RESULTS,              // Results (MSet)
	REPLY_METADATA,             // Metadata
	REPLY_METADATAKEYLIST,      // Iterator for metadata keys
	REPLY_FREQS,                // Get termfreq and collfreq
	REPLY_UNIQUETERMS,          // Get number of unique terms in doc
	REPLY_POSITIONLISTCOUNT,    // Get PositionList length
	REPLY_REMOVESPELLING,       // Remove a spelling
	REPLY_TERMLIST0,            // Header for get Termlist
	REPLY_MAX
};


inline const std::string& RemoteReplyTypeNames(RemoteReplyType type) {
	static const std::string _[] = {
		"REPLY_UPDATE", "REPLY_EXCEPTION", "REPLY_DONE", "REPLY_ALLTERMS",
		"REPLY_COLLFREQ", "REPLY_DOCDATA", "REPLY_TERMDOESNTEXIST",
		"REPLY_TERMEXISTS", "REPLY_TERMFREQ", "REPLY_VALUESTATS", "REPLY_DOCLENGTH",
		"REPLY_STATS", "REPLY_TERMLIST", "REPLY_POSITIONLIST", "REPLY_POSTLISTSTART",
		"REPLY_POSTLISTITEM", "REPLY_VALUE", "REPLY_ADDDOCUMENT", "REPLY_RESULTS",
		"REPLY_METADATA", "REPLY_METADATAKEYLIST", "REPLY_FREQS", "REPLY_UNIQUETERMS",
		"REPLY_POSITIONLISTCOUNT", "REPLY_REMOVESPELLING",
	};
	auto idx = static_cast<size_t>(type);
	if (idx >= 0 && idx < sizeof(_) / sizeof(_[0])) {
		return _[idx];
	}
	static const std::string UNKNOWN = "UNKNOWN";
	return UNKNOWN;
}



// A single instance of a non-blocking Xapiand binary protocol handler
class RemoteProtocolClient : public MetaBaseClient<RemoteProtocolClient>, public LockableDatabase {
	friend MetaBaseClient<RemoteProtocolClient>;

	mutable std::mutex runner_mutex;

	std::atomic<State> state;

#ifdef SAVE_LAST_MESSAGES
	std::atomic_char last_message_received;
	std::atomic_char last_message_sent;
#endif

	int file_descriptor;
	char file_message_type;
	std::string temp_directory;
	std::string temp_directory_template;
	std::string temp_file_template;
	std::vector<std::string> temp_files;

	// Buffers that are pending write
	std::string buffer;
	std::deque<Buffer> messages;
	bool cluster_database;

	// For msg_query and msg_mset:
	lock_database _msg_query_database_lock;
	Xapian::Registry _msg_query_reg;
	std::unique_ptr<Xapian::Enquire> _msg_query_enquire;
	std::vector<Xapian::MatchSpy*> _msg_query_matchspies;
	void init_msg_query();
	void reset();

	RemoteProtocolClient(const std::shared_ptr<Worker>& parent_, ev::loop_ref* ev_loop_, unsigned int ev_flags_, int sock_, double active_timeout_, double idle_timeout_, bool cluster_database_ = false);

	bool is_idle() const;

	ssize_t on_read(const char *buf, ssize_t received);
	void on_read_file(const char *buf, ssize_t received);
	void on_read_file_done();

	friend Worker;

public:
	~RemoteProtocolClient() noexcept;

	explicit RemoteProtocolClient(RemoteProtocolClient& client_);

	void send_message(RemoteReplyType type, const std::string& message);

	void remote_server(RemoteMessageType type, const std::string& message);
	void msg_allterms(const std::string& message);
	void msg_termlist(const std::string& message);
	void msg_positionlist(const std::string& message);
	void msg_postlist(const std::string& message);
	void msg_positionlistcount(const std::string& message);
	void msg_readaccess(const std::string& message);
	void msg_writeaccess(const std::string& message);
	void msg_reopen(const std::string& message);
	void msg_update(const std::string& message);
	void msg_query(const std::string& message);
	void msg_getmset(const std::string& message);
	void msg_document(const std::string& message);
	void msg_keepalive(const std::string& message);
	void msg_termexists(const std::string& message);
	void msg_collfreq(const std::string& message);
	void msg_termfreq(const std::string& message);
	void msg_freqs(const std::string& message);
	void msg_valuestats(const std::string& message);
	void msg_doclength(const std::string& message);
	void msg_uniqueterms(const std::string& message);
	void msg_commit(const std::string& message);
	void msg_cancel(const std::string& message);
	void msg_adddocument(const std::string& message);
	void msg_deletedocument(const std::string& message);
	void msg_deletedocumentterm(const std::string& message);
	void msg_replacedocument(const std::string& message);
	void msg_replacedocumentterm(const std::string& message);
	void msg_getmetadata(const std::string& message);
	void msg_metadatakeylist(const std::string& message);
	void msg_setmetadata(const std::string& message);
	void msg_addspelling(const std::string& message);
	void msg_removespelling(const std::string& message);
	void msg_shutdown(const std::string& message);

	char get_message(std::string &result, char max_type);
	void send_message(char type_as_char, const std::string& message);
	void send_file(char type_as_char, int fd);

	bool init_remote() noexcept;
	bool init_replication(const Endpoint &src_endpoint, const Endpoint &dst_endpoint) noexcept;

	void operator()();

	std::string __repr__() const override;
};


#endif  /* XAPIAND_CLUSTERING */
