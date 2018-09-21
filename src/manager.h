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
#include "opts.h"
#include "prometheus/registry.h"
#include "schemas_lru.h"
#include "stats.h"
#include "threadpool.h"
#include "worker.h"
#include "serialise.h"


#define UNKNOWN_REGION -1


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

enum class RequestType {
	INDEX,
	SEARCH,
	DELETE,
	PATCH,
	MERGE,
	AGGREGATIONS,
	COMMIT
};

class Requestinfo {
public:
	Requestinfo(const std::string& nodename);
	~Requestinfo() = default;

	std::shared_ptr<prometheus::Registry> registry;

	prometheus::Family<prometheus::Summary>& index_summary;
	prometheus::Summary& xapiand_index_summary;

	prometheus::Family<prometheus::Summary>& search_summary;
	prometheus::Summary& xapiand_search_summary;

	prometheus::Family<prometheus::Summary>& delete_summary;
	prometheus::Summary& xapiand_delete_summary;

	prometheus::Family<prometheus::Summary>& patch_summary;
	prometheus::Summary& xapiand_patch_summary;

	prometheus::Family<prometheus::Summary>& merge_summary;
	prometheus::Summary& xapiand_merge_summary;

	prometheus::Family<prometheus::Summary>& aggregation_summary;
	prometheus::Summary& xapiand_aggregation_summary;

	// clients_tasks:
	prometheus::Family<prometheus::Gauge>& xapiand_http_clients_run;
	prometheus::Gauge& xapiand_http_clients_run_met;

	prometheus::Family<prometheus::Gauge>& xapiand_http_clients_queue;
	prometheus::Gauge& xapiand_http_clients_queue_met;

	prometheus::Family<prometheus::Gauge>& xapiand_http_clients_capacity;
	prometheus::Gauge& xapiand_http_clients_capacity_met;

	prometheus::Family<prometheus::Gauge>& xapiand_http_clients_pool_size;
	prometheus::Gauge& xapiand_http_clients_pool_size_met;

	// server_tasks:
	prometheus::Family<prometheus::Gauge>& xapiand_servers_run;
	prometheus::Gauge& xapiand_servers_run_met;

	prometheus::Family<prometheus::Gauge>& xapiand_servers_pool_size;
	prometheus::Gauge& xapiand_servers_pool_size_met;

	prometheus::Family<prometheus::Gauge>& xapiand_servers_queue;
	prometheus::Gauge& xapiand_servers_queue_met;

	prometheus::Family<prometheus::Gauge>& xapiand_servers_capacity;
	prometheus::Gauge& 	xapiand_servers_capacity_met;

	// committers_threads:
	prometheus::Family<prometheus::Gauge>& xapiand_committers_running;
	prometheus::Gauge& xapiand_committers_running_met;

	prometheus::Family<prometheus::Gauge>& xapiand_committers_queue;
	prometheus::Gauge& xapiand_committers_queue_met;

	prometheus::Family<prometheus::Gauge>& xapiand_committers_capacity;
	prometheus::Gauge& xapiand_committers_capacity_met;

	prometheus::Family<prometheus::Gauge>& xapiand_committers_pool_size;
	prometheus::Gauge& xapiand_committers_pool_size_met;

	// fsync_threads:
	prometheus::Family<prometheus::Gauge>& xapiand_fsync_running;
	prometheus::Gauge& xapiand_fsync_running_met;

	prometheus::Family<prometheus::Gauge>& xapiand_fsync_queue;
	prometheus::Gauge& xapiand_fsync_queue_met;

	prometheus::Family<prometheus::Gauge>& xapiand_fsync_capacity;
	prometheus::Gauge& xapiand_fsync_capacity_met;

	prometheus::Family<prometheus::Gauge>& xapiand_fsync_pool_size;
	prometheus::Gauge& xapiand_fsync_pool_size_met;

	// connections:
	prometheus::Family<prometheus::Gauge>& xapiand_http_current_connections;
	prometheus::Gauge& xapiand_http_current_connections_met;

	prometheus::Family<prometheus::Gauge>& xapiand_http_peak_connections;
	prometheus::Gauge& xapiand_http_peak_connections_met;

	prometheus::Family<prometheus::Gauge>& xapiand_file_descriptors;
	prometheus::Gauge& xapiand_file_descriptors_met;

	prometheus::Family<prometheus::Gauge>& xapiand_max_file_descriptors;
	prometheus::Gauge& xapiand_max_file_descriptors_met;

	prometheus::Family<prometheus::Gauge>& xapiand_resident_memory_bytes;
	prometheus::Gauge& xapiand_resident_memory_bytes_met;

	prometheus::Family<prometheus::Gauge>& xapiand_virtual_memory_bytes;
	prometheus::Gauge& xapiand_virtual_memory_bytes_met;

	prometheus::Family<prometheus::Gauge>& xapiand_used_memory_bytes;
	prometheus::Gauge& xapiand_used_memory_bytes_met;

	prometheus::Family<prometheus::Gauge>& xapiand_total_memory_system_bytes;
	prometheus::Gauge& xapiand_total_memory_system_bytes_met;

	prometheus::Family<prometheus::Gauge>& xapiand_total_virtual_memory_used;
	prometheus::Gauge& xapiand_total_virtual_memory_used_met;

	// databases:
	prometheus::Family<prometheus::Gauge>& xapiand_readable_db;
	prometheus::Gauge& xapiand_readable_db_met;

	prometheus::Family<prometheus::Gauge>& xapiand_total_readable_db;
	prometheus::Gauge& xapiand_total_readable_db_met;

	prometheus::Family<prometheus::Gauge>& xapiand_writable_db;
	prometheus::Gauge& xapiand_writable_db_met;

	prometheus::Family<prometheus::Gauge>& xapiand_total_writable_db;
	prometheus::Gauge& xapiand_total_writable_db_met;

	prometheus::Family<prometheus::Gauge>& xapiand_total_db;
	prometheus::Gauge& xapiand_total_db_met;

	prometheus::Family<prometheus::Gauge>& xapiand_total_peak_db;
	prometheus::Gauge& xapiand_total_peak_db_met;

	// request:
	prometheus::Family<prometheus::Gauge>& xapiand_max_time_index;
	prometheus::Gauge& xapiand_max_time_index_met;

	prometheus::Family<prometheus::Gauge>& xapiand_max_time_search;
	prometheus::Gauge& xapiand_max_time_search_met;

	prometheus::Family<prometheus::Gauge>& xapiand_max_time_delete;
	prometheus::Gauge& xapiand_max_time_delete_met;

	prometheus::Family<prometheus::Gauge>& xapiand_max_time_patch;
	prometheus::Gauge& xapiand_max_time_patch_met;

	prometheus::Family<prometheus::Gauge>& xapiand_max_time_merge;
	prometheus::Gauge& xapiand_max_time_merge_met;

	prometheus::Family<prometheus::Gauge>& xapiand_max_time_aggregation;
	prometheus::Gauge& xapiand_max_time_aggregation_met;

	prometheus::Family<prometheus::Gauge>& xapiand_index_total;
	prometheus::Gauge& xapiand_index_total_met;

	prometheus::Family<prometheus::Gauge>& xapiand_search_total;
	prometheus::Gauge& xapiand_search_total_met;

	prometheus::Family<prometheus::Gauge>& xapiand_delete_total;
	prometheus::Gauge& xapiand_delete_total_met;

	prometheus::Family<prometheus::Gauge>& xapiand_patch_total;
	prometheus::Gauge& xapiand_patch_total_met;

	prometheus::Family<prometheus::Gauge>& xapiand_merge_total;
	prometheus::Gauge& xapiand_merge_total_met;

	prometheus::Family<prometheus::Gauge>& xapiand_aggregation_total;
	prometheus::Gauge& xapiand_aggregation_total_met;

	prometheus::Family<prometheus::Gauge>& xapiand_commit_total;
	prometheus::Gauge& xapiand_commit_total_met;
};


class XapiandManager : public Worker  {
	friend Worker;

	using nodes_map_t = std::unordered_map<std::string, std::shared_ptr<const Node>>;

	std::mutex qmtx;

	XapiandManager();
	XapiandManager(ev::loop_ref* ev_loop_, unsigned int ev_flags_);

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
	void save_node_name(std::string_view _node_name);
	std::string set_node_name(std::string_view node_name_);

	uint64_t load_node_id();
	void save_node_id(uint64_t node_id);
	uint64_t get_node_id();

	void make_servers();
	void make_replicators();

	std::unique_ptr<Requestinfo> req_info;

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

	ThreadPool thread_pool;
	ThreadPool client_pool;
	ThreadPool server_pool;
#ifdef XAPIAND_CLUSTERING
	ThreadPool replicator_pool;
#endif
#ifdef XAPIAND_CLUSTERING
	EndpointResolver endp_r;
#endif

	std::atomic<time_t> shutdown_asap;
	std::atomic<time_t> shutdown_now;

	std::atomic<State> state;
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

	void run();

	bool is_single_node();

	void update_req_info(std::uint64_t duration, RequestType typ);

	void set_request_info();

#ifdef XAPIAND_CLUSTERING
	void reset_state();

	bool put_node(std::shared_ptr<const Node> node);
	std::shared_ptr<const Node> get_node(std::string_view node_name);
	std::shared_ptr<const Node> touch_node(std::string_view node_name, int32_t region);
	void drop_node(std::string_view node_name);

	size_t get_nodes_by_region(int32_t region);

	// Return the region to which db name belongs
	int32_t get_region(std::string_view db_name);
	// Return the region to which local_node belongs
	int32_t get_region();

	std::shared_future<bool> trigger_replication(const Endpoint& src_endpoint, const Endpoint& dst_endpoint);
#endif

	bool resolve_index_endpoint(const std::string &path, std::vector<Endpoint> &endpv, size_t n_endps=1, std::chrono::duration<double, std::milli> timeout=1s);

	void server_status(MsgPack& stats);
	std::string server_metrics();
	void get_stats_time(MsgPack& stats, const std::string& time_req, const std::string& gran_req);

	inline decltype(auto) get_lock() noexcept {
		return std::unique_lock<std::mutex>(qmtx);
	}
};
