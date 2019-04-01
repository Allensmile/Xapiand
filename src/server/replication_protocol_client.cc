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

#include "replication_protocol_client.h"

#ifdef XAPIAND_CLUSTERING

#include <errno.h>                            // for errno
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sysexits.h>
#include <unistd.h>

#include "cassert.h"                          // for ASSERT
#include "database/lock.h"                    // for lock_shard
#include "database/shard.h"                   // for Shard
#include "database/wal.h"                     // for DatabaseWAL
#include "error.hh"                           // for error:name, error::description
#include "fs.hh"                              // for delete_files, build_path_index
#include "io.hh"                              // for io::*
#include "length.h"
#include "manager.h"                          // for XapiandManager
#include "metrics.h"                          // for Metrics::metrics
#include "tcp.h"                              // for TCP::connect
#include "random.hh"                          // for random_int
#include "repr.hh"                            // for repr
#include "utype.hh"                           // for toUType
#include "xapian/net/serialise-error.h"       // for serialise_error, unserialise_error


// #undef L_DEBUG
// #define L_DEBUG L_GREY
// #undef L_CALL
// #define L_CALL L_STACKED_DIM_GREY
// #undef L_REPLICATION
// #define L_REPLICATION L_ROSY_BROWN
// #undef L_CONN
// #define L_CONN L_GREEN
// #undef L_REPLICA_WIRE
// #define L_REPLICA_WIRE L_ORANGE
// #undef L_REPLICA
// #define L_REPLICA L_TEAL
// #undef L_REPLICA_PROTO
// #define L_REPLICA_PROTO L_TEAL
// #undef L_TIMED_VAR
// #define L_TIMED_VAR _L_TIMED_VAR


/*  ____            _ _           _   _
 * |  _ \ ___ _ __ | (_) ___ __ _| |_(_) ___  _ __
 * | |_) / _ \ '_ \| | |/ __/ _` | __| |/ _ \| '_ \
 * |  _ <  __/ |_) | | | (_| (_| | |_| | (_) | | | |
 * |_| \_\___| .__/|_|_|\___\__,_|\__|_|\___/|_| |_|
 *           |_|
 */


ReplicationProtocolClient::ReplicationProtocolClient(const std::shared_ptr<Worker>& parent_, ev::loop_ref* ev_loop_, unsigned int ev_flags_, int sock_, double /*active_timeout_*/, double /*idle_timeout_*/, bool cluster_database_)
	: MetaBaseClient<ReplicationProtocolClient>(std::move(parent_), ev_loop_, ev_flags_, sock_),
	  state(ReplicaState::INIT_REPLICATION_CLIENT),
#ifdef SAVE_LAST_MESSAGES
	  last_message_received('\xff'),
	  last_message_sent('\xff'),
#endif
	  file_descriptor(-1),
	  file_message_type('\xff'),
	  temp_file_template("xapiand.XXXXXX"),
	  cluster_database(cluster_database_),
	  changesets(0)
{
	++XapiandManager::replication_clients();

	Metrics::metrics()
		.xapiand_replication_connections
		.Increment();

	L_CONN("New Replication Protocol Client in socket {}, {} client(s) of a total of {} connected.", sock_, XapiandManager::replication_clients().load(), XapiandManager::total_clients().load());
}


ReplicationProtocolClient::~ReplicationProtocolClient() noexcept
{
	try {
		reset();

		if (XapiandManager::replication_clients().fetch_sub(1) == 0) {
			L_CRIT("Inconsistency in number of replication clients");
			sig_exit(-EX_SOFTWARE);
		}

		if (file_descriptor != -1) {
			io::close(file_descriptor);
			file_descriptor = -1;
		}

		for (const auto& filename : temp_files) {
			io::unlink(filename.c_str());
		}

		if (!temp_directory.empty()) {
			delete_files(temp_directory.c_str());
		}

		if (is_shutting_down() && !is_idle()) {
			L_INFO("Replication Protocol client killed!");
		}

		if (cluster_database) {
			L_CRIT("Cannot synchronize cluster database!");
			sig_exit(-EX_CANTCREAT);
		}
	} catch (...) {
		L_EXC("Unhandled exception in destructor");
	}
}


void
ReplicationProtocolClient::reset()
{
	wal.reset();

	if (switch_shard) {
		switch_shard->close();
		XapiandManager::database_pool()->checkin(switch_shard);
	}

	if (!switch_shard_path.empty()) {
		delete_files(switch_shard_path.c_str());
		switch_shard_path.clear();
	}

	if (log) {
		log->clear();
	}
	changesets = 0;
}


bool
ReplicationProtocolClient::init_replication_protocol(const Endpoint &src_endpoint, const Endpoint &dst_endpoint) noexcept
{
	L_CALL("ReplicationProtocolClient::init_replication_protocol({}, {})", repr(src_endpoint.to_string()), repr(dst_endpoint.to_string()));

	try {
		lk_shard_ptr = std::make_unique<lock_shard>(dst_endpoint, DB_WRITABLE | DB_CREATE_OR_OPEN, false);
		lk_shard_ptr->lock(0, [=] {
			// If it cannot checkout because database is busy, retry when ready...
			trigger_replication()->delayed_debounce(std::chrono::milliseconds{random_int(0, 3000)}, dst_endpoint.path, src_endpoint, dst_endpoint);
		});

		temp_directory_template = dst_endpoint.path + "/.tmp.XXXXXX";

		L_REPLICATION("Replication initialized: {} -->  {}", repr(src_endpoint.to_string()), repr(dst_endpoint.to_string()));
	} catch (const Xapian::DatabaseNotAvailableError&) {
		L_REPLICATION("Replication deferred: {} -->  {}", repr(src_endpoint.to_string()), repr(dst_endpoint.to_string()));
		return false;
	} catch (...) {
		L_EXC("ERROR: Replication initialization ended with an unhandled exception");
		return false;
	}
	return true;
}


void
ReplicationProtocolClient::send_message(ReplicationReplyType type, const std::string& message)
{
	L_CALL("ReplicationProtocolClient::send_message({}, <message>)", ReplicationReplyTypeNames(type));

	L_REPLICA_PROTO("<< send_message ({}): {}", ReplicationReplyTypeNames(type), repr(message));

	send_message(toUType(type), message);
}


void
ReplicationProtocolClient::send_file(ReplicationReplyType type, int fd)
{
	L_CALL("ReplicationProtocolClient::send_file({}, <fd>)", ReplicationReplyTypeNames(type));

	L_REPLICA_PROTO("<< send_file ({}): {}", ReplicationReplyTypeNames(type), fd);

	send_file(toUType(type), fd);
}


void
ReplicationProtocolClient::replication_server(ReplicationMessageType type, const std::string& message)
{
	L_CALL("ReplicationProtocolClient::replication_server({}, <message>)", ReplicationMessageTypeNames(type));

	L_OBJ_BEGIN("ReplicationProtocolClient::replication_server:BEGIN {{type:{}}}", ReplicationMessageTypeNames(type));
	L_OBJ_END("ReplicationProtocolClient::replication_server:END {{type:{}}}", ReplicationMessageTypeNames(type));

	try {
		switch (type) {
			case ReplicationMessageType::MSG_GET_CHANGESETS:
				msg_get_changesets(message);
				return;
			default: {
				std::string errmsg("Unexpected message type ");
				errmsg += std::to_string(toUType(type));
				THROW(InvalidArgumentError, errmsg);
			}
		}
	} catch (const Xapian::NetworkTimeoutError& exc) {
		try {
			// We've had a timeout, so the client may not be listening, if we can't
			// send the message right away, just exit and the client will cope.
			send_message(ReplicationReplyType::REPLY_EXCEPTION, serialise_error(exc));
		} catch (...) {}
		destroy();
		detach();
	} catch (const Xapian::NetworkError&) {
		// All other network errors mean we are fatally confused and are unlikely
		// to be able to communicate further across this connection. So we don't
		// try to propagate the error to the client, but instead just log the
		// exception and close the connection.
		L_EXC("ERROR: Dispatching remote protocol message");
		destroy();
		detach();
	} catch (const Xapian::Error& exc) {
		// Propagate the exception to the client, then close the connection.
		send_message(ReplicationReplyType::REPLY_EXCEPTION, serialise_error(exc));
		destroy();
		detach();
	} catch (...) {
		L_EXC("ERROR: Dispatching remote protocol message");
		send_message(ReplicationReplyType::REPLY_EXCEPTION, std::string());
		destroy();
		detach();
	}
}


void
ReplicationProtocolClient::msg_get_changesets(const std::string& message)
{
	L_CALL("ReplicationProtocolClient::msg_get_changesets(<message>)");

	L_REPLICATION("GET_CHANGESETS");

	size_t _total_sent_bytes = total_sent_bytes;
	auto begins = std::chrono::system_clock::now();

	const char *p = message.c_str();
	const char *p_end = p + message.size();

	auto remote_uuid = unserialise_string(&p, p_end);
	auto remote_revision = unserialise_length(&p, p_end);
	auto endpoint_path = unserialise_string(&p, p_end);

	if (endpoint_path.empty()) {
		send_message(ReplicationReplyType::REPLY_FAIL, "Database must have a valid path");
	}

	Xapian::Database* db;

	lock_shard lk_shard(Endpoint{endpoint_path}, DB_WRITABLE, false);

	db = lk_shard.lock()->db();
	auto uuid = db->get_uuid();
	auto revision = db->get_revision();
	lk_shard.unlock();

	auto from_revision = remote_revision;
	if (from_revision && uuid != remote_uuid) {
		from_revision = 0;
	}

	wal = std::make_unique<DatabaseWAL>(endpoint_path);
	if (from_revision && wal->locate_revision(from_revision).first == DatabaseWAL::max_rev) {
		from_revision = 0;
	}

	auto to_revision = from_revision;

	if (from_revision < revision) {
		if (from_revision == 0) {
			int whole_db_copies_left = 5;

			while (true) {
				// Send the current revision number in the header.
				send_message(ReplicationReplyType::REPLY_DB_HEADER,
					serialise_string(uuid) +
					serialise_length(revision));

				static std::array<const std::string, 7> filenames = {
					"termlist.glass",
					"synonym.glass",
					"spelling.glass",
					"docdata.glass",
					"position.glass",
					"postlist.glass",
					"iamglass"
				};

				for (const auto& filename : filenames) {
					auto path = string::format("{}/{}", endpoint_path, filename);
					int fd = io::open(path.c_str());
					if (fd != -1) {
						send_message(ReplicationReplyType::REPLY_DB_FILENAME, filename);
						send_file(ReplicationReplyType::REPLY_DB_FILEDATA, fd);
					}
				}

				for (size_t volume = 0; true; ++volume) {
					auto filename = string::format("docdata.{}", volume);
					auto path = string::format("{}/{}", endpoint_path, filename);
					int fd = io::open(path.c_str());
					if (fd != -1) {
						send_message(ReplicationReplyType::REPLY_DB_FILENAME, filename);
						send_file(ReplicationReplyType::REPLY_DB_FILEDATA, fd);
						continue;
					}
					break;
				}

				db = lk_shard.lock()->db();
				auto final_revision = db->get_revision();
				lk_shard.unlock();

				send_message(ReplicationReplyType::REPLY_DB_FOOTER, serialise_length(final_revision));

				if (revision == final_revision) {
					to_revision = revision;
					break;
				}

				if (whole_db_copies_left == 0) {
					send_message(ReplicationReplyType::REPLY_FAIL, "Database changing too fast");

					auto ends = std::chrono::system_clock::now();
					_total_sent_bytes = total_sent_bytes - _total_sent_bytes;
					L(LOG_NOTICE, RED, "\"GET_CHANGESETS {{{}}} {} {}\" ERROR {} {}", remote_uuid, remote_revision, repr(endpoint_path), string::from_bytes(_total_sent_bytes), string::from_delta(begins, ends));
					return;
				} else if (--whole_db_copies_left == 0) {
					db = lk_shard.lock()->db();
					uuid = db->get_uuid();
					revision = db->get_revision();
				} else {
					db = lk_shard.lock()->db();
					uuid = db->get_uuid();
					revision = db->get_revision();
					lk_shard.unlock();
				}
			}
			lk_shard.unlock();
		}

		int wal_iterations = 5;
		do {
			// Send WAL operations.
			auto wal_it = wal->find(to_revision);
			for (; wal_it != wal->end(); ++wal_it) {
				to_revision = wal_it->first + 1;
				if (to_revision > revision) {
					break;
				}
				send_message(ReplicationReplyType::REPLY_CHANGESET, wal_it->second);
			}
			db = lk_shard.lock()->db();
			revision = db->get_revision();
			lk_shard.unlock();
		} while (to_revision < revision && --wal_iterations != 0);
	}

	send_message(ReplicationReplyType::REPLY_END_OF_CHANGES, "");

	auto ends = std::chrono::system_clock::now();
	total_sent_bytes = total_sent_bytes - total_sent_bytes;
	if (from_revision == to_revision) {
		L(LOG_DEBUG, WHITE, "\"GET_CHANGESETS {{{}}} {} {}\" OK EMPTY {} {}", remote_uuid, remote_revision, repr(endpoint_path), string::from_bytes(total_sent_bytes), string::from_delta(begins, ends));
	} else {
		L(LOG_DEBUG, WHITE, "\"GET_CHANGESETS {{{}}} {} {}\" OK [{}..{}] {} {}", remote_uuid, remote_revision, repr(endpoint_path), from_revision + 1, to_revision, string::from_bytes(total_sent_bytes), string::from_delta(begins, ends));
	}

	destroy();
	detach();
}


void
ReplicationProtocolClient::replication_client(ReplicationReplyType type, const std::string& message)
{
	L_CALL("ReplicationProtocolClient::replication_client({}, <message>)", ReplicationReplyTypeNames(type));

	L_OBJ_BEGIN("ReplicationProtocolClient::replication_client:BEGIN {{type:{}}}", ReplicationReplyTypeNames(type));
	L_OBJ_END("ReplicationProtocolClient::replication_client:END {{type:{}}}", ReplicationReplyTypeNames(type));

	try {
		switch (type) {
			case ReplicationReplyType::REPLY_WELCOME:
				reply_welcome(message);
				return;
			case ReplicationReplyType::REPLY_EXCEPTION:
				reply_exception(message);
				return;
			case ReplicationReplyType::REPLY_END_OF_CHANGES:
				reply_end_of_changes(message);
				return;
			case ReplicationReplyType::REPLY_FAIL:
				reply_fail(message);
				return;
			case ReplicationReplyType::REPLY_DB_HEADER:
				reply_db_header(message);
				return;
			case ReplicationReplyType::REPLY_DB_FILENAME:
				reply_db_filename(message);
				return;
			case ReplicationReplyType::REPLY_DB_FILEDATA:
				reply_db_filedata(message);
				return;
			case ReplicationReplyType::REPLY_DB_FOOTER:
				reply_db_footer(message);
				return;
			case ReplicationReplyType::REPLY_CHANGESET:
				reply_changeset(message);
				return;
			default: {
				std::string errmsg("Unexpected message type ");
				errmsg += std::to_string(toUType(type));
				THROW(InvalidArgumentError, errmsg);
			}
		}
	} catch (const BaseException& exc) {
		ASSERT(lk_shard_ptr);
		L_EXC("ERROR: Replicating database: {}", (*lk_shard_ptr)->endpoint.path);
	} catch (const Xapian::DatabaseNotFoundError& exc) {
		ASSERT(lk_shard_ptr);
		L_WARNING("WARNING: Replicating non-existent database: {}", (*lk_shard_ptr)->endpoint.path);
	} catch (const Xapian::Error& exc) {
		ASSERT(lk_shard_ptr);
		L_EXC("ERROR: Replicating database: {}", (*lk_shard_ptr)->endpoint.path);
	} catch (const std::exception& exc) {
		ASSERT(lk_shard_ptr);
		L_EXC("ERROR: Replicating database: {}", (*lk_shard_ptr)->endpoint.path);
	} catch (...) {
		ASSERT(lk_shard_ptr);
		L_EXC("ERROR: Replicating database: {}", (*lk_shard_ptr)->endpoint.path);
	}

	destroy();
	detach();
}


void
ReplicationProtocolClient::reply_welcome(const std::string&)
{
	std::string message;

	ASSERT(lk_shard_ptr);
	auto shard = lk_shard_ptr->locked();
	auto db = shard->db();

	message.append(serialise_string(db->get_uuid()));
	message.append(serialise_length(db->get_revision()));
	message.append(serialise_string(shard->endpoint.path));

	send_message(static_cast<ReplicationReplyType>(ReplicationMessageType::MSG_GET_CHANGESETS), message);
}


void
ReplicationProtocolClient::reply_exception(const std::string& message)
{
	unserialise_error(message, "REPLICATION:", "");
}


void
ReplicationProtocolClient::reply_end_of_changes(const std::string&)
{
	L_CALL("ReplicationProtocolClient::reply_end_of_changes(<message>)");

	ASSERT(lk_shard_ptr);
	auto shard = lk_shard_ptr->locked();

	bool switching = !switch_shard_path.empty();

	if (switching) {
		// Close internal databases
		shard->do_close(false, false, shard->transaction);

		if (switch_shard) {
			switch_shard->close();
			XapiandManager::database_pool()->checkin(switch_shard);
		}

		// get exclusive lock
		XapiandManager::database_pool()->lock(shard);

		// Now we are sure no readers are using the database before moving the files
		delete_files(shard->endpoint.path, {"*glass", "wal.*"});
		move_files(switch_shard_path, shard->endpoint.path);

		// release exclusive lock
		XapiandManager::database_pool()->unlock(shard);
	}

	L_REPLICATION("END_OF_CHANGES: {} ({} a set of {} changesets){}", repr(shard->endpoint.path), switching ? "from a full copy and" : "from", changesets, switch_shard ? " (to switch database)" : "");

	if (cluster_database) {
		cluster_database = false;
		XapiandManager::set_cluster_database_ready();
	}

	destroy();
	detach();
}


void
ReplicationProtocolClient::reply_fail(const std::string&)
{
	L_CALL("ReplicationProtocolClient::reply_fail(<message>)");

	ASSERT(lk_shard_ptr);
	L_REPLICATION("FAIL: {}", repr((*lk_shard_ptr)->endpoint.path));

	reset();

	L_ERR("ReplicationProtocolClient failure!");
	destroy();
	detach();
}


void
ReplicationProtocolClient::reply_db_header(const std::string& message)
{
	L_CALL("ReplicationProtocolClient::reply_db_header(<message>)");

	ASSERT(lk_shard_ptr);
	auto shard = lk_shard_ptr->locked();

	const char *p = message.data();
	const char *p_end = p + message.size();

	current_uuid = unserialise_string(&p, p_end);
	current_revision = unserialise_length(&p, p_end);

	reset();

	char path[PATH_MAX];
	strncpy(path, temp_directory_template.c_str(), PATH_MAX);
	build_path_index(temp_directory_template);
	if (io::mkdtemp(path) == nullptr) {
		L_ERR("Directory {} not created: {} ({}): {}", path, error::name(errno), errno, error::description(errno));
		detach();
		return;
	}
	switch_shard_path = path;

	L_REPLICATION("DB_HEADER: {} in {} ({}@{})", repr(shard->endpoint.path), repr(switch_shard_path), current_uuid, current_revision);
	L_TIMED_VAR(log, 1s,
		"Replication of whole database taking too long: {}",
		"Replication of whole database took too long: {}",
		repr(shard->endpoint.path));
}


void
ReplicationProtocolClient::reply_db_filename(const std::string& filename)
{
	L_CALL("ReplicationProtocolClient::reply_db_filename(<filename>)");

	ASSERT(lk_shard_ptr);

	ASSERT(!switch_shard_path.empty());

	file_path = switch_shard_path + "/" + filename;

	L_REPLICATION("DB_FILENAME({}): {}", repr(filename), repr((*lk_shard_ptr)->endpoint.path));
}


void
ReplicationProtocolClient::reply_db_filedata(const std::string& tmp_file)
{
	L_CALL("ReplicationProtocolClient::reply_db_filedata(<tmp_file>)");

	ASSERT(lk_shard_ptr);

	ASSERT(!switch_shard_path.empty());

	if (::rename(tmp_file.c_str(), file_path.c_str()) == -1) {
		L_ERR("Cannot rename temporary file {} to {}: {} ({}): {}", tmp_file, file_path, error::name(errno), errno, error::description(errno));
		detach();
		return;
	}

	L_REPLICATION("DB_FILEDATA({} -> {}): {}", repr(tmp_file), repr(file_path), repr((*lk_shard_ptr)->endpoint.path));
}


void
ReplicationProtocolClient::reply_db_footer(const std::string& message)
{
	L_CALL("ReplicationProtocolClient::reply_db_footer(<message>)");

	ASSERT(lk_shard_ptr);

	const char *p = message.data();
	const char *p_end = p + message.size();
	size_t revision = unserialise_length(&p, p_end);

	ASSERT(!switch_shard_path.empty());

	if (revision != current_revision) {
		delete_files(switch_shard_path.c_str());
		switch_shard_path.clear();
	}

	L_REPLICATION("DB_FOOTER{}: {}", revision != current_revision ? " (ignored files)" : "", repr((*lk_shard_ptr)->endpoint.path));
}


void
ReplicationProtocolClient::reply_changeset(const std::string& line)
{
	L_CALL("ReplicationProtocolClient::reply_changeset(<line>)");

	ASSERT(lk_shard_ptr);
	auto shard = lk_shard_ptr->locked();

	bool switching = !switch_shard_path.empty();

	if (!wal) {
		if (switching) {
			if (!switch_shard) {
				switch_shard = XapiandManager::database_pool()->checkout(Endpoint{switch_shard_path}, DB_WRITABLE | DB_SYNCHRONOUS_WAL);
			}
			switch_shard->begin_transaction(false);
			wal = std::make_unique<DatabaseWAL>(switch_shard.get());
		} else {
			shard->begin_transaction(false);
			wal = std::make_unique<DatabaseWAL>(shard.get());
		}
		L_TIMED_VAR(log, 1s,
			"Replication of {}changesets taking too long: {}",
			"Replication of {}changesets took too long: {}",
			switching ? "whole database with " : "",
			repr(shard->endpoint.path));
	}

	wal->execute_line(line, true, false, false);

	++changesets;
	L_REPLICATION("CHANGESET ({} changesets{}): {}", changesets, switch_shard ? " to a new database" : "", repr(shard->endpoint.path));
}


bool
ReplicationProtocolClient::is_idle() const
{
	if (!is_waiting() && !is_running() && write_queue.empty()) {
		std::lock_guard<std::mutex> lk(runner_mutex);
		return messages.empty();
	}
	return false;
}


bool
ReplicationProtocolClient::init_replication() noexcept
{
	L_CALL("ReplicationProtocolClient::init_replication()");

	std::lock_guard<std::mutex> lk(runner_mutex);

	ASSERT(!running);

	// Setup state...
	state = ReplicaState::INIT_REPLICATION_SERVER;

	// And start a runner.
	running = true;
	XapiandManager::replication_client_pool()->enqueue(share_this<ReplicationProtocolClient>());
	return true;
}


bool
ReplicationProtocolClient::init_replication(const Endpoint &src_endpoint, const Endpoint &dst_endpoint) noexcept
{
	L_CALL("ReplicationProtocolClient::init_replication({}, {})", repr(src_endpoint.to_string()), repr(dst_endpoint.to_string()));

	std::lock_guard<std::mutex> lk(runner_mutex);

	ASSERT(!running);

	// Setup state...
	state = ReplicaState::INIT_REPLICATION_CLIENT;

	if (init_replication_protocol(src_endpoint, dst_endpoint)) {
		// And start a runner.
		running = true;
		XapiandManager::replication_client_pool()->enqueue(share_this<ReplicationProtocolClient>());
		return true;
	}
	return false;
}


ssize_t
ReplicationProtocolClient::on_read(const char *buf, ssize_t received)
{
	L_CALL("ReplicationProtocolClient::on_read(<buf>, {})", received);

	if (received <= 0) {
		return received;
	}

	L_REPLICA_WIRE("ReplicationProtocolClient::on_read: {} bytes", received);
	ssize_t processed = -buffer.size();
	buffer.append(buf, received);
	while (buffer.size() >= 2) {
		const char *o = buffer.data();
		const char *p = o;
		const char *p_end = p + buffer.size();

		char type = *p++;
		L_REPLICA_WIRE("on_read message: {} {{state:{}}}", repr(std::string(1, type)), StateNames(state));
		switch (type) {
			case FILE_FOLLOWS: {
				char path[PATH_MAX];
				if (temp_directory.empty()) {
					if (temp_directory_template.empty()) {
						temp_directory = "/tmp";
					} else {
						strncpy(path, temp_directory_template.c_str(), PATH_MAX);
						build_path_index(temp_directory_template);
						if (io::mkdtemp(path) == nullptr) {
							L_ERR("Directory {} not created: {} ({}): {}", temp_directory_template, error::name(errno), errno, error::description(errno));
							detach();
							return processed;
						}
						temp_directory = path;
					}
				}
				strncpy(path, (temp_directory + "/" + temp_file_template).c_str(), PATH_MAX);
				file_descriptor = io::mkstemp(path);
				temp_files.push_back(path);
				file_message_type = *p++;
				if (file_descriptor == -1) {
					L_ERR("Cannot create temporary file: {} ({}): {}", error::name(errno), errno, error::description(errno));
					detach();
					return processed;
				} else {
					L_REPLICA("Start reading file: {} ({})", path, file_descriptor);
				}
				read_file();
				processed += p - o;
				buffer.clear();
				return processed;
			}
		}

		ssize_t len;
		try {
			len = unserialise_length(&p, p_end, true);
		} catch (const Xapian::SerialisationError) {
			return received;
		}

		if (!closed) {
			std::lock_guard<std::mutex> lk(runner_mutex);
			if (!running) {
				// Enqueue message...
				messages.push_back(Buffer(type, p, len));
				// And start a runner.
				running = true;
				XapiandManager::replication_client_pool()->enqueue(share_this<ReplicationProtocolClient>());
			} else {
				// There should be a runner, just enqueue message.
				messages.push_back(Buffer(type, p, len));
			}
		}

		buffer.erase(0, p - o + len);
		processed += p - o + len;
	}

	return received;
}


void
ReplicationProtocolClient::on_read_file(const char *buf, ssize_t received)
{
	L_CALL("ReplicationProtocolClient::on_read_file(<buf>, {})", received);

	L_REPLICA_WIRE("ReplicationProtocolClient::on_read_file: {} bytes", received);

	io::write(file_descriptor, buf, received);
}


void
ReplicationProtocolClient::on_read_file_done()
{
	L_CALL("ReplicationProtocolClient::on_read_file_done()");

	L_REPLICA_WIRE("ReplicationProtocolClient::on_read_file_done");

	io::close(file_descriptor);
	file_descriptor = -1;

	const auto& temp_file = temp_files.back();

	if (!closed) {
		std::lock_guard<std::mutex> lk(runner_mutex);
		if (!running) {
			// Enqueue message...
			messages.push_back(Buffer(file_message_type, temp_file.data(), temp_file.size()));
			// And start a runner.
			running = true;
			XapiandManager::replication_client_pool()->enqueue(share_this<ReplicationProtocolClient>());
		} else {
			// There should be a runner, just enqueue message.
			messages.push_back(Buffer(file_message_type, temp_file.data(), temp_file.size()));
		}
	}
}


char
ReplicationProtocolClient::get_message(std::string &result, char max_type)
{
	L_CALL("ReplicationProtocolClient::get_message(<result>, <max_type>)");

	auto& msg = messages.front();

	char type = msg.type;

#ifdef SAVE_LAST_MESSAGES
	last_message_received.store(type, std::memory_order_relaxed);
#endif

	if (type >= max_type) {
		std::string errmsg("Invalid message type ");
		errmsg += std::to_string(int(type));
		THROW(InvalidArgumentError, errmsg);
	}

	const char *msg_str = msg.dpos();
	size_t msg_size = msg.nbytes();
	result.assign(msg_str, msg_size);

	messages.pop_front();

	return type;
}


void
ReplicationProtocolClient::send_message(char type_as_char, const std::string &message)
{
	L_CALL("ReplicationProtocolClient::send_message(<type_as_char>, <message>)");

#ifdef SAVE_LAST_MESSAGES
	last_message_sent.store(type_as_char, std::memory_order_relaxed);
#endif

	std::string buf;
	buf += type_as_char;
	buf += serialise_length(message.size());
	buf += message;
	write(buf);
}


void
ReplicationProtocolClient::send_file(char type_as_char, int fd)
{
	L_CALL("ReplicationProtocolClient::send_file(<type_as_char>, <fd>)");

	std::string buf;
	buf += FILE_FOLLOWS;
	buf += type_as_char;
	write(buf);

	MetaBaseClient<ReplicationProtocolClient>::send_file(fd);
}


void
ReplicationProtocolClient::operator()()
{
	L_CALL("ReplicationProtocolClient::operator()()");

	L_CONN("Start running in replication worker...");

	std::unique_lock<std::mutex> lk(runner_mutex);

	switch (state) {
		case ReplicaState::INIT_REPLICATION_SERVER:
			state = ReplicaState::REPLICATION_SERVER;
			lk.unlock();
			try {
				send_message(static_cast<char>(ReplicationReplyType::REPLY_WELCOME), "");
			} catch (...) {
				lk.lock();
				running = false;
				lk.unlock();
				L_CONN("Running in worker ended with an exception.");
				detach();
				throw;
			}
			lk.lock();
			break;
		case ReplicaState::INIT_REPLICATION_CLIENT:
			state = ReplicaState::REPLICATION_CLIENT;
			[[fallthrough]];
		default:
			break;
	}

	while (!messages.empty() && !closed) {
		switch (state) {
			case ReplicaState::REPLICATION_SERVER: {
				std::string message;
				ReplicationMessageType type = static_cast<ReplicationMessageType>(get_message(message, static_cast<char>(ReplicationMessageType::MSG_MAX)));
				lk.unlock();
				try {

					L_REPLICA_PROTO(">> get_message[REPLICATION_SERVER] ({}): {}", ReplicationMessageTypeNames(type), repr(message));
					replication_server(type, message);

					auto sent = total_sent_bytes.exchange(0);
					Metrics::metrics()
						.xapiand_replication_sent_bytes
						.Increment(sent);

					auto received = total_received_bytes.exchange(0);
					Metrics::metrics()
						.xapiand_replication_received_bytes
						.Increment(received);

				} catch (...) {
					lk.lock();
					running = false;
					lk.unlock();
					L_CONN("Running in worker ended with an exception.");
					detach();
					throw;
				}
				lk.lock();
				break;
			}

			case ReplicaState::REPLICATION_CLIENT: {
				std::string message;
				ReplicationReplyType type = static_cast<ReplicationReplyType>(get_message(message, static_cast<char>(ReplicationReplyType::REPLY_MAX)));
				lk.unlock();
				try {

					L_REPLICA_PROTO(">> get_message[REPLICATION_CLIENT] ({}): {}", ReplicationReplyTypeNames(type), repr(message));
					replication_client(type, message);

					auto sent = total_sent_bytes.exchange(0);
					Metrics::metrics()
						.xapiand_replication_sent_bytes
						.Increment(sent);

					auto received = total_received_bytes.exchange(0);
					Metrics::metrics()
						.xapiand_replication_received_bytes
						.Increment(received);

				} catch (...) {
					lk.lock();
					running = false;
					lk.unlock();
					L_CONN("Running in worker ended with an exception.");
					detach();
					throw;
				}
				lk.lock();
				break;
			}

			default:
				running = false;
				lk.unlock();
				L_ERR("Unexpected ReplicationProtocolClient State!");
				stop();
				destroy();
				detach();
				return;
		}
	}

	running = false;
	lk.unlock();

	if (is_shutting_down() && is_idle()) {
		L_CONN("Running in worker ended due shutdown.");
		detach();
		return;
	}

	L_CONN("Running in replication worker ended.");
	redetach();  // try re-detaching if already flagged as detaching
}


std::string
ReplicationProtocolClient::__repr__() const
{
#ifdef SAVE_LAST_MESSAGES
	auto state_repr = ([this]() -> std::string {
		auto received = last_message_received.load(std::memory_order_relaxed);
		auto sent = last_message_sent.load(std::memory_order_relaxed);
		auto st = state.load(std::memory_order_relaxed);
		switch (st) {
			case ReplicaState::INIT_REPLICATION_CLIENT:
			case ReplicaState::REPLICATION_CLIENT:
				return string::format("{}) ({}<->{}",
					StateNames(st),
					ReplicationReplyTypeNames(static_cast<ReplicationReplyType>(received)),
					ReplicationMessageTypeNames(static_cast<ReplicationMessageType>(sent)));
			case ReplicaState::INIT_REPLICATION_SERVER:
			case ReplicaState::REPLICATION_SERVER:
				return string::format("{}) ({}<->{}",
					StateNames(st),
					ReplicationMessageTypeNames(static_cast<ReplicationMessageType>(received)),
					ReplicationReplyTypeNames(static_cast<ReplicationReplyType>(sent)));
			default:
				return "";
		}
	})();
#else
	auto& state_repr = StateNames(state.load(std::memory_order_relaxed));
#endif
	return string::format("<ReplicationProtocolClient ({}) {{cnt:{}, sock:{}}}{}{}{}{}{}{}{}{}>",
		state_repr,
		use_count(),
		sock,
		is_runner() ? " (runner)" : " (worker)",
		is_running_loop() ? " (running loop)" : " (stopped loop)",
		is_detaching() ? " (deteaching)" : "",
		is_idle() ? " (idle)" : "",
		is_waiting() ? " (waiting)" : "",
		is_running() ? " (running)" : "",
		is_shutting_down() ? " (shutting down)" : "",
		is_closed() ? " (closed)" : "");
}

#endif  /* XAPIAND_CLUSTERING */
