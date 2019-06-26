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

#pragma once

#include "config.h"

#include <cassert>                            // for assert
#include <chrono>                             // for std::chrono
#include <atomic>                             // for std::atomic, std::atomic_int
#include <mutex>                              // for std::mutex
#include <string>                             // for std::string
#include <string_view>                        // for std::string_view
#include <vector>                             // for std::vector

#include "base_x.hh"                          // for Base62
#include "database/utils.h"                   // for UNKNOWN_REVISION
#include "debouncer.h"                        // for Debouncer
#include "endpoint.h"                         // for Endpoint
#include "enum.h"                             // for ENUM_CLASS
#include "ev/ev++.h"                          // for ev::loop_ref
#include "length.h"                           // for serialise_length
#include "opts.h"                             // for opts::*
#include "node.h"                             // for Node, local_node
#include "thread.hh"                          // for ThreadPolicyType::*
#include "threadpool.hh"                      // for ThreadPool
#include "worker.h"                           // for Worker
#include "xapian.h"                           // for Xapian::*


class Http;
#ifdef XAPIAND_CLUSTERING
class Discovery;
class RemoteProtocol;
class RemoteProtocolClient;
class RemoteProtocolServer;
class ReplicationProtocol;
class ReplicationProtocolClient;
class ReplicationProtocolServer;
#endif

class DocMatcher;
class DocPreparer;
class DocIndexer;

class MsgPack;
class HttpClient;
class HttpServer;
class DatabasePool;
class DatabaseWALWriter;
class DatabaseCleanup;
class SchemasLRU;

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


ENUM_CLASS(XapiandManagerState, int,
	BAD,
	READY,
	SETUP,
	JOINING,
	WAITING_MORE,
	WAITING,
	RESET
)


struct IndexSettingsShard {
	Xapian::rev version;
	bool modified;

	std::vector<std::string> nodes;

	IndexSettingsShard();
};


struct IndexSettings {
	Xapian::rev version;
	bool loaded;
	bool saved;
	bool modified;

	std::chrono::time_point<std::chrono::steady_clock> stalled;

	size_t num_shards;
	size_t num_replicas_plus_master;
	std::vector<IndexSettingsShard> shards;

	IndexSettings();

	IndexSettings(Xapian::rev version, bool loaded, bool saved, bool modified, const std::chrono::time_point<std::chrono::steady_clock>& stalled, size_t num_shards, size_t num_replicas_plus_master, const std::vector<IndexSettingsShard>& shards);

	std::string __repr__() const;
};


class XapiandManager : public Worker  {
	friend Worker;

	XapiandManager();
	XapiandManager(ev::loop_ref* ev_loop_, unsigned int ev_flags_, std::chrono::time_point<std::chrono::steady_clock> process_start_ = std::chrono::steady_clock::now());
	~XapiandManager() noexcept;

	std::pair<struct sockaddr_in, std::string> host_address();

	void shutdown_impl(long long asap, long long now) override;

	void _get_stats_time(MsgPack& stats, int start, int end, int increment);

	std::string load_node_name();
	void save_node_name(std::string_view node_name);
	std::string set_node_name(std::string_view node_name);

	void start_discovery();
	void make_servers();

public:
	using State = XapiandManagerState;

private:
	static std::shared_ptr<XapiandManager> _manager;

	std::shared_ptr<Logging> log;

	std::atomic_int _total_clients;
	std::atomic_int _http_clients;
	std::atomic_int _remote_clients;
	std::atomic_int _replication_clients;

	std::shared_ptr<Http> _http;
#ifdef XAPIAND_CLUSTERING
	std::shared_ptr<RemoteProtocol> _remote;
	std::shared_ptr<ReplicationProtocol> _replication;
	std::shared_ptr<Discovery> _discovery;
#endif

	std::shared_ptr<DatabaseCleanup> _database_cleanup;

	std::unique_ptr<SchemasLRU> _schemas;
	std::unique_ptr<DatabasePool> _database_pool;
	std::unique_ptr<DatabaseWALWriter> _wal_writer;

	std::unique_ptr<ThreadPool<std::shared_ptr<HttpClient>, ThreadPolicyType::http_clients>> _http_client_pool;
	std::unique_ptr<ThreadPool<std::shared_ptr<HttpServer>, ThreadPolicyType::http_servers>> _http_server_pool;
#ifdef XAPIAND_CLUSTERING
	std::unique_ptr<ThreadPool<std::shared_ptr<RemoteProtocolClient>, ThreadPolicyType::binary_clients>> _remote_client_pool;
	std::unique_ptr<ThreadPool<std::shared_ptr<RemoteProtocolServer>, ThreadPolicyType::binary_servers>> _remote_server_pool;

	std::unique_ptr<ThreadPool<std::shared_ptr<ReplicationProtocolClient>, ThreadPolicyType::binary_clients>> _replication_client_pool;
	std::unique_ptr<ThreadPool<std::shared_ptr<ReplicationProtocolServer>, ThreadPolicyType::binary_servers>> _replication_server_pool;
#endif
	std::unique_ptr<ThreadPool<std::shared_ptr<DocMatcher>, ThreadPolicyType::doc_matchers>> _doc_matcher_pool;
	std::unique_ptr<ThreadPool<std::unique_ptr<DocPreparer>, ThreadPolicyType::doc_preparers>> _doc_preparer_pool;
	std::unique_ptr<ThreadPool<std::shared_ptr<DocIndexer>, ThreadPolicyType::doc_indexers>> _doc_indexer_pool;

	std::atomic_llong _shutdown_asap;
	std::atomic_llong _shutdown_now;

	std::atomic<State> _state;
	std::string _node_name;
	int _new_cluster;
	std::chrono::time_point<std::chrono::steady_clock> _process_start;

	ev::timer try_shutdown_timer;
	ev::async signal_sig_async;
	ev::async setup_node_async;
	ev::async set_cluster_database_ready_async;

#ifdef XAPIAND_CLUSTERING
	ev::async new_leader_async;
	ev::async raft_apply_command_async;

	ConcurrentQueue<std::string> raft_apply_command_args;
#endif

	void try_shutdown_timer_cb(ev::timer& watcher, int revents);
	void signal_sig_async_cb(ev::async&, int);
	void signal_sig_impl();

	void shutdown_sig(int sig, bool async);

	void setup_node_async_cb(ev::async&, int);
	void setup_node_impl();

	void set_cluster_database_ready_async_cb(ev::async&, int);
	void set_cluster_database_ready_impl();

#ifdef XAPIAND_CLUSTERING
	void load_nodes();
	void new_leader_async_cb(ev::async&, int);
	void new_leader_impl();
	void renew_leader_impl();
	void reset_state_impl();
	void join_cluster_impl();

	void sweep_primary_impl(size_t shards, const std::string& normalized_path);

	void raft_apply_command_async_cb(ev::async& watcher, int revents);
	void raft_apply_command_impl(const std::string& command);
	void _raft_apply_command(const std::string& command);

	void add_node(std::string_view name);
	void node_added(std::string_view name);
#endif

	IndexSettings resolve_index_settings_impl(const std::string& normalized_slashed_path, bool writable, bool primary, const MsgPack* settings, bool reload, bool rebuild, bool clear);
	Endpoints resolve_index_endpoints_impl(const Endpoint& endpoint, bool writable, bool primary, const MsgPack* settings);

	std::string server_metrics_impl();

	void try_shutdown_impl(bool always) {
		if (always || _shutdown_asap) {
			shutdown_sig(0, true);
		}
	}

	void init();

public:
	std::atomic_int atom_sig;

	void signal_sig(int sig);

	void run();
	void join();

	std::string __repr__() const override;

	template<typename... Args>
	static auto& make(Args&&... args) {
		_manager = Worker::make_shared<XapiandManager>(std::forward<Args>(args)...);
		_manager->init();
		return _manager;
	}

	static const auto& manager([[maybe_unused]] bool check = true) {
		assert(!check || _manager);
		return _manager;
	}

	static void reset() {
		assert(_manager);
		_manager.reset();
	}

	static IndexSettings resolve_index_settings(const std::string& normalized_path, bool writable = false, bool primary = false, const MsgPack* settings = nullptr) {
		assert(_manager);
		return _manager->resolve_index_settings_impl(normalized_path, writable, primary, settings, false, false, false);
	}

	static std::vector<std::vector<std::shared_ptr<const Node>>> resolve_nodes(const IndexSettings& index_settings);

	static Endpoints resolve_index_endpoints(const Endpoint& endpoint, bool writable = false, bool primary = false, const MsgPack* settings = nullptr) {
		assert(_manager);
		return _manager->resolve_index_endpoints_impl(endpoint, writable, primary, settings);
	}

	static void setup_node() {
		assert(_manager);
		_manager->setup_node_impl();
	}

	static void new_leader() {
		assert(_manager);
#ifdef XAPIAND_CLUSTERING
		_manager->new_leader_impl();
#endif
	}

	static void renew_leader() {
#ifdef XAPIAND_CLUSTERING
		assert(_manager);
		_manager->renew_leader_impl();
#endif
	}

	static void raft_apply_command(const std::string& command) {
#ifdef XAPIAND_CLUSTERING
		assert(_manager);
		_manager->raft_apply_command_impl(command);
#endif
	}

	static void reset_state() {
#ifdef XAPIAND_CLUSTERING
		assert(_manager);
		_manager->reset_state_impl();
#endif
	}

	static void join_cluster() {
#ifdef XAPIAND_CLUSTERING
		assert(_manager);
		_manager->join_cluster_impl();
#endif
	}

	static std::string server_metrics() {
		assert(_manager);
		return _manager->server_metrics_impl();
	}

	static void try_shutdown(bool always = false) {
		assert(_manager);
		_manager->try_shutdown_impl(always);
	}

	static void set_cluster_database_ready() {
		assert(_manager);
		_manager->set_cluster_database_ready_impl();
	}

	static auto& node_name() {
		assert(_manager);
		return _manager->_node_name;
	}

	static auto& state() {
		assert(_manager);
		return _manager->_state;
	}

	static bool exchange_state(State from, State to, std::chrono::milliseconds timeout = 0s, std::string_view format_timeout = "", std::string_view format_done = "");

	static auto& total_clients() {
		assert(_manager);
		return _manager->_total_clients;
	}
	static auto& http_clients() {
		assert(_manager);
		return _manager->_http_clients;
	}
	static auto& remote_clients() {
		assert(_manager);
		return _manager->_remote_clients;
	}
	static auto& replication_clients() {
		assert(_manager);
		return _manager->_replication_clients;
	}

	static auto& database_pool() {
		assert(_manager);
		assert(_manager->_database_pool);
		return _manager->_database_pool;
	}
	static auto& http_client_pool() {
		assert(_manager);
		assert(_manager->_http_client_pool);
		return _manager->_http_client_pool;
	}
	static auto& http_server_pool() {
		assert(_manager);
		assert(_manager->_http_server_pool);
		return _manager->_http_server_pool;
	}
#ifdef XAPIAND_CLUSTERING
	static auto& remote_client_pool() {
		assert(_manager);
		assert(_manager->_remote_client_pool);
		return _manager->_remote_client_pool;
	}
	static auto& remote_server_pool() {
		assert(_manager);
		assert(_manager->_remote_server_pool);
		return _manager->_remote_server_pool;
	}
	static auto& replication_client_pool() {
		assert(_manager);
		assert(_manager->_replication_client_pool);
		return _manager->_replication_client_pool;
	}
	static auto& replication_server_pool() {
		assert(_manager);
		assert(_manager->_replication_server_pool);
		return _manager->_replication_server_pool;
	}
	static auto& discovery() {
		assert(_manager);
		assert(_manager->_discovery);
		return _manager->_discovery;
	}
	static auto& remote() {
		assert(_manager);
		assert(_manager->_remote);
		return _manager->_remote;
	}
	static auto& replication() {
		assert(_manager);
		assert(_manager->_replication);
		return _manager->_replication;
	}

	static void sweep_primary(size_t shards, const std::string& normalized_path) {
		assert(_manager);
		_manager->sweep_primary_impl(shards, normalized_path);
	}
#endif

	static auto& doc_matcher_pool() {
		assert(_manager);
		assert(_manager->_doc_matcher_pool);
		return _manager->_doc_matcher_pool;
	}

	static auto& doc_preparer_pool() {
		assert(_manager);
		assert(_manager->_doc_preparer_pool);
		return _manager->_doc_preparer_pool;
	}

	static auto& doc_indexer_pool() {
		assert(_manager);
		assert(_manager->_doc_indexer_pool);
		return _manager->_doc_indexer_pool;
	}

	static auto& http() {
		assert(_manager);
		assert(_manager->_http);
		return _manager->_http;
	}

	static auto& wal_writer() {
		assert(_manager);
		assert(_manager->_wal_writer);
		return _manager->_wal_writer;
	}

	static auto& schemas() {
		assert(_manager);
		assert(_manager->_schemas);
		return _manager->_schemas;
	}
};


#ifdef XAPIAND_CLUSTERING

void trigger_replication_trigger(Endpoint src_endpoint, Endpoint dst_endpoint);

inline auto& trigger_replication(bool create = true) {
	static auto trigger_replication = create ? make_unique_debouncer<std::string, ThreadPolicyType::replication>("TR--", "TR{:02}", opts.num_replicators, trigger_replication_trigger, std::chrono::milliseconds(opts.trigger_replication_throttle_time), std::chrono::milliseconds(opts.trigger_replication_debounce_timeout), std::chrono::milliseconds(opts.trigger_replication_debounce_busy_timeout), std::chrono::milliseconds(opts.trigger_replication_debounce_min_force_timeout), std::chrono::milliseconds(opts.trigger_replication_debounce_max_force_timeout)) : nullptr;
	assert(!create || trigger_replication);
	return trigger_replication;
}

#endif
