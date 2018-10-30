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

#include "config.h"    // for XAPIAND_CLUSTERING


#ifdef XAPIAND_CLUSTERING

#include <string>                           // for std::string
#include <unordered_map>                    // for std::unordered_map
#include <vector>                           // for std::vector

#include "base_udp.h"                       // for UDP
#include "node.h"                           // for Node
#include "worker.h"                         // for Worker


// Values in seconds
constexpr double HEARTBEAT_LEADER_MIN = 0.150;
constexpr double HEARTBEAT_LEADER_MAX = 0.300;

constexpr double LEADER_ELECTION_MIN = 2.5 * HEARTBEAT_LEADER_MAX;
constexpr double LEADER_ELECTION_MAX = 5.0 * HEARTBEAT_LEADER_MAX;

constexpr uint16_t XAPIAND_RAFT_PROTOCOL_MAJOR_VERSION = 1;
constexpr uint16_t XAPIAND_RAFT_PROTOCOL_MINOR_VERSION = 0;

constexpr uint16_t XAPIAND_RAFT_PROTOCOL_VERSION = XAPIAND_RAFT_PROTOCOL_MAJOR_VERSION | XAPIAND_RAFT_PROTOCOL_MINOR_VERSION << 8;


struct RaftLogEntry {
	uint64_t term;
	std::string command;
};

// The Raft consensus algorithm
class Raft : public UDP, public Worker {
public:
	enum class State {
		FOLLOWER,
		CANDIDATE,
		LEADER,
		MAX,
	};

	static const std::string& StateNames(State type) {
		static const std::string StateNames[] = {
			"LEADER", "FOLLOWER", "CANDIDATE",
		};

		auto type_int = static_cast<int>(type);
		if (type_int >= 0 || type_int < static_cast<int>(State::MAX)) {
			return StateNames[type_int];
		}
		static const std::string UNKNOWN = "UNKNOWN";
		return UNKNOWN;
	}

	enum class Message {
		HEARTBEAT,               // same as APPEND_ENTRIES
		HEARTBEAT_RESPONSE,      // same as APPEND_ENTRIES_RESPONSE
		APPEND_ENTRIES,          // Node saying hello when it become leader
		APPEND_ENTRIES_RESPONSE, // Request information from leader
		REQUEST_VOTE,            // Invoked by candidates to gather votes
		REQUEST_VOTE_RESPONSE,   // Gather votes
		ADD_COMMAND,
		MAX,
	};

	static const std::string& MessageNames(Message type) {
		static const std::string MessageNames[] = {
			"HEARTBEAT", "HEARTBEAT_RESPONSE",
			"APPEND_ENTRIES", "APPEND_ENTRIES_RESPONSE",
			"REQUEST_VOTE", "REQUEST_VOTE_RESPONSE",
			"ADD_COMMAND",
		};

		auto type_int = static_cast<int>(type);
		if (type_int >= 0 || type_int < static_cast<int>(Message::MAX)) {
			return MessageNames[type_int];
		}
		static const std::string UNKNOWN = "UNKNOWN";
		return UNKNOWN;
	}

private:
	ev::io io;

	ev::timer leader_election_timeout;
	ev::timer leader_heartbeat;

	State state;
	size_t votes_granted;
	size_t votes_denied;

	uint64_t current_term;
	Node voted_for;
	std::vector<RaftLogEntry> log;

	size_t commit_index;
	size_t last_applied;

	std::unordered_map<std::string, size_t> next_indexes;
	std::unordered_map<std::string, size_t> match_indexes;

	void send_message(Message type, const std::string& message);
	void io_accept_cb(ev::io& watcher, int revents);
	void raft_server(Raft::Message type, const std::string& message);

	void request_vote(Message type, const std::string& message);
	void request_vote_response(Message type, const std::string& message);
	void append_entries(Message type, const std::string& message);
	void append_entries_response(Message type, const std::string& message);
	void add_command(Message type, const std::string& message);

	void leader_election_timeout_cb(ev::timer& watcher, int revents);
	void leader_heartbeat_cb(ev::timer& watcher, int revents);

	void _start_leader_heartbeat(double min = HEARTBEAT_LEADER_MIN, double max = HEARTBEAT_LEADER_MAX);
	void _reset_leader_election_timeout(double min = LEADER_ELECTION_MIN, double max = LEADER_ELECTION_MAX);
	void _set_leader_node(const std::shared_ptr<const Node>& node);

	void _apply(const std::string& command);
	void _commit_log();

	void destroyer();

	void destroy_impl() override;
	void shutdown_impl(time_t asap, time_t now) override;

	// No copy constructor
	Raft(const Raft&) = delete;
	Raft& operator=(const Raft&) = delete;

public:
	Raft(const std::shared_ptr<Worker>& parent_, ev::loop_ref* ev_loop_, unsigned int ev_flags_, int port_, const std::string& group_);
	~Raft();

	void add_command(const std::string& command);
	void request_vote();
	void start();
	void stop();

	std::string __repr__() const override {
		return Worker::__repr__("Raft");
	}

	std::string getDescription() const noexcept override;
};

#endif
