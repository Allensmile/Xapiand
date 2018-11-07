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

#include "config.h"

#include <atomic>                             // for std::atomic, std::atomic_int
#include <mutex>                              // for std::mutex
#include <string>                             // for std::string
#include "string_view.hh"                     // for std::string_view
#include <vector>                             // for std::vector

#include "base_x.hh"                          // for Base62
#include "database_pool.h"                    // for DatabasePool
#include "endpoint.h"                         // for Endpoint
#include "ev/ev++.h"                          // for ev::loop_ref
#include "length.h"                           // for serialise_length
#include "node.h"                             // for Node, local_node
#include "schemas_lru.h"                      // for SchemasLRU
#include "threadpool.h"                       // for ThreadPool
#include "worker.h"                           // for Worker


class Http;
#ifdef XAPIAND_CLUSTERING
class Binary;
class Discovery;
class Raft;
#endif

class XapiandServer;

extern void sig_exit(int sig);


inline std::string serialise_node_id(uint64_t node_id) {
	return Base62::inverted().encode(serialise_length(node_id));
}


inline uint64_t unserialise_node_id(std::string_view node_id_str) {
	auto serialized = Base62::inverted().decode(node_id_str);
	const char *p = serialized.data();
	const char *p_end = p + serialized.size();
	return unserialise_length(&p, p_end);
}


class XapiandManager : public Worker  {
	friend Worker;

	XapiandManager();
	XapiandManager(ev::loop_ref* ev_loop_, unsigned int ev_flags_, std::chrono::time_point<std::chrono::system_clock> process_start_ = std::chrono::system_clock::now());
	~XapiandManager();

	struct sockaddr_in host_address();

	void shutdown_impl(time_t asap, time_t now) override;
	void stop_impl() override;

	void finish();

	void _get_stats_time(MsgPack& stats, int start, int end, int increment);

protected:
	std::string load_node_name();
	void save_node_name(std::string_view _node_name);
	std::string set_node_name(std::string_view node_name_);

	void make_servers();

public:
	std::string __repr__() const override {
		return Worker::__repr__("XapiandManager");
	}

	enum class State {
		BAD,
		READY,
		SETUP,
		JOINING,
		WAITING_MORE,
		WAITING,
		RESET,
		MAX,
	};

	static const std::string& StateNames(State type) {
		static const std::string StateNames[] = {
			"BAD", "READY", "SETUP", "JOINING", "WAITING_MORE", "WAITING", "RESET",
		};

		auto type_int = static_cast<int>(type);
		if (type_int >= 0 || type_int < static_cast<int>(State::MAX)) {
			return StateNames[type_int];
		}
		static const std::string UNKNOWN = "UNKNOWN";
		return UNKNOWN;
	}

	static std::shared_ptr<XapiandManager> manager;

	std::vector<std::weak_ptr<XapiandServer>> servers_weak;
	std::weak_ptr<Http> weak_http;
#ifdef XAPIAND_CLUSTERING
	std::weak_ptr<Binary> weak_binary;
	std::weak_ptr<Discovery> weak_discovery;
	std::weak_ptr<Raft> weak_raft;
#endif

	DatabasePool database_pool;
	SchemasLRU schemas;

	ThreadPool thread_pool;
	ThreadPool client_pool;
	ThreadPool server_pool;

	std::atomic<time_t> shutdown_asap;
	std::atomic<time_t> shutdown_now;

	std::atomic<State> state;
	std::string node_name;
	int new_cluster;

	std::atomic_int atom_sig;
	ev::async signal_sig_async;
	ev::async setup_node_async;
	ev::async cluster_database_ready_async;
	ev::async shutdown_sig_async;
	std::chrono::time_point<std::chrono::system_clock> process_start;

	ev::timer cleanup;
	void cleanup_cb(ev::timer& watcher, int revents);

	void signal_sig(int sig);
	void signal_sig_async_cb(ev::async&, int);

	void shutdown_sig(int sig);

	void setup_node();
	void setup_node_async_cb(ev::async&, int);

	void cluster_database_ready();
	void cluster_database_ready_async_cb(ev::async&, int);

	void run();
	void join();

#ifdef XAPIAND_CLUSTERING
	void reset_state();
	void join_cluster();

	void renew_leader();
	void new_leader(std::shared_ptr<const Node>&& leader_node);

	void trigger_replication(const Endpoint& src_endpoint, const Endpoint& dst_endpoint, bool cluster_database = false);
#endif

	std::vector<std::shared_ptr<const Node>> resolve_index_nodes(std::string_view path);
	Endpoint resolve_index_endpoint(std::string_view path, bool master);

	std::string server_metrics();
};
