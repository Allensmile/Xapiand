/*
 * Copyright (C) 2015,2016,2017 deipi.com LLC and contributors. All rights reserved.
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

#include <list>
#include <mutex>
#include <regex>
#include <unordered_map>

#include "base_x.hh"
#include "database.h"
#include "endpoint_resolver.h"
#include "ev/ev++.h"
#include "length.h"
#include "schemas_lru.h"
#include "stats.h"
#include "threadpool.h"
#include "worker.h"
#include "serialise.h"


#define UNKNOWN_REGION -1


struct opts_t {
	int verbosity;
	bool detach;
	bool chert;
	bool solo;
	bool strict;
	bool optimal;
	bool colors;
	bool no_colors;
	std::string database;
	std::string cluster_name;
	std::string node_name;
	unsigned int http_port;
	unsigned int binary_port;
	unsigned int discovery_port;
	unsigned int raft_port;
	std::string pidfile;
	std::string logfile;
	std::string uid;
	std::string gid;
	std::string discovery_group;
	std::string raft_group;
	ssize_t num_servers;
	ssize_t dbpool_size;
	ssize_t num_replicators;
	ssize_t threadpool_size;
	ssize_t endpoints_list_size;
	ssize_t num_committers;
	ssize_t num_fsynchers;
	ssize_t max_clients;
	ssize_t max_databases;
	ssize_t max_files;
	unsigned int ev_flags;
	bool uuid_compact;
	UUIDRepr uuid_repr;
	bool uuid_partition;
	std::string dump;
	std::string restore;
	std::string filename;
};


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


inline uint64_t unserialise_node_id(const std::string& node_id_str) {
	auto serialized = Base62::inverted().decode(node_id_str);
	const char *p = serialized.data();
	const char *p_end = p + serialized.size();
	return unserialise_length(&p, p_end);
}


class XapiandManager : public Worker  {
	friend Worker;

	using nodes_map_t = std::unordered_map<std::string, std::shared_ptr<const Node>>;

	std::mutex qmtx;

	XapiandManager(const opts_t& o);
	XapiandManager(ev::loop_ref* ev_loop_, unsigned int ev_flags_, const opts_t& o);

	struct sockaddr_in host_address();

	void destroyer();

	void destroy_impl() override;
	void shutdown_impl(time_t asap, time_t now) override;

	void finish();
	void join();

	void _get_stats_time(MsgPack& stats, int start, int end, int increment);

protected:
	std::mutex nodes_mtx;
	nodes_map_t nodes;

	size_t nodes_size();
	std::string load_node_name();
	void save_node_name(const std::string& node_name);
	std::string set_node_name(const std::string& node_name_);

	uint64_t load_node_id();
	void save_node_id(uint64_t node_id);
	uint64_t get_node_id();

	void make_servers(const opts_t& o);
	void make_replicators(const opts_t& o);

public:
	std::string __repr__() const override {
		return Worker::__repr__("XapiandManager");
	}

	enum class State {
		BAD,
		READY,
		SETUP,
		WAITING_,
		WAITING,
		RESET,
	};

	static constexpr const char* const StateNames[] = {
		"BAD", "READY", "SETUP", "WAITING_", "WAITING", "RESET",
	};

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

	ThreadPool<> thread_pool;
	ThreadPool<> server_pool;
#ifdef XAPIAND_CLUSTERING
	ThreadPool<> replicator_pool;
#endif
#ifdef XAPIAND_CLUSTERING
	EndpointResolver endp_r;
#endif

	std::atomic<time_t> shutdown_asap;
	std::atomic<time_t> shutdown_now;

	std::atomic<State> state;
	const opts_t& opts;
	std::string node_name;

	std::atomic_int atom_sig;
	ev::async signal_sig_async;
	ev::async shutdown_sig_async;
	void signal_sig(int sig);
	void signal_sig_async_cb(ev::async&, int);

	void shutdown_sig(int sig);

	~XapiandManager();

	void setup_node();

	void setup_node(std::shared_ptr<XapiandServer>&& server);

	void run(const opts_t& o);

	bool is_single_node();

#ifdef XAPIAND_CLUSTERING
	void reset_state();

	bool put_node(std::shared_ptr<const Node> node);
	std::shared_ptr<const Node> get_node(const std::string& node_name);
	std::shared_ptr<const Node> touch_node(const std::string& node_name, int32_t region);
	void drop_node(const std::string& node_name);

	size_t get_nodes_by_region(int32_t region);

	// Return the region to which db name belongs
	int32_t get_region(const std::string& db_name);
	// Return the region to which local_node belongs
	int32_t get_region();

	std::shared_future<bool> trigger_replication(const Endpoint& src_endpoint, const Endpoint& dst_endpoint);
#endif

	bool resolve_index_endpoint(const std::string &path, std::vector<Endpoint> &endpv, size_t n_endps=1, std::chrono::duration<double, std::milli> timeout=1s);

	void server_status(MsgPack& stats);
	void get_stats_time(MsgPack& stats, const std::string& time_req, const std::string& gran_req);

	inline decltype(auto) get_lock() noexcept {
		return std::unique_lock<std::mutex>(qmtx);
	}
};
