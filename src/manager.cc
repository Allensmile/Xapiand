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

#include "manager.h"

#include "utils.h"
#include "replicator.h"
#include "database_autocommit.h"
#include "endpoint.h"
#include "servers/server.h"
#include "client_binary.h"
#include "servers/http.h"
#include "servers/binary.h"
#include "servers/discovery.h"
#include "servers/raft.h"

#include <list>
#include <stdlib.h>
#include <assert.h>
#include <sys/sysctl.h>
#include <fcntl.h>
#include <net/if.h> /* for IFF_LOOPBACK */
#include <ifaddrs.h>
#include <unistd.h>

#define NANOSEC 1e-9


static const std::regex time_re("((([01]?[0-9]|2[0-3])h)?(([0-5]?[0-9])m)?(([0-5]?[0-9])s)?)(\\.\\.(([01]?[0-9]|2[0-3])h)?(([0-5]?[0-9])m)?(([0-5]?[0-9])s)?)?", std::regex::icase | std::regex::optimize);


std::atomic<time_t> XapiandManager::shutdown_asap(0);
std::atomic<time_t> XapiandManager::shutdown_now(0);


XapiandManager::XapiandManager(ev::loop_ref *loop_, const opts_t &o)
	: Worker(nullptr, loop_),
	  database_pool(o.dbpool_size),
	  thread_pool("W%02zu", o.threadpool_size),
	  async_shutdown(*loop),
	  endp_r(o.endpoints_list_size),
	  state(State::RESET),
	  cluster_name(o.cluster_name),
	  node_name(o.node_name)
{
	// Setup node from node database directory
	std::string node_name_(get_node_name());
	if (!node_name_.empty()) {
		if (!node_name.empty() && stringtolower(node_name) != stringtolower(node_name_)) {
			L_ERR(this, "Node name %s doesn't match with the one in the cluster's database: %s!", node_name.c_str(), node_name_.c_str());
			assert(false);
		}
		node_name = node_name_;
	}

	// Set the id in local node.
	local_node.id = get_node_id();

	// Set addr in local node
	local_node.addr = host_address();

	async_shutdown.set<XapiandManager, &XapiandManager::async_shutdown_cb>(this);
	async_shutdown.start();
	L_EV(this, "Start manager's async shutdown event");

	L_OBJ(this, "CREATED MANAGER! [%llx]", this);
}


XapiandManager::~XapiandManager()
{
	discovery->send_message(Discovery::Message::BYE, local_node.serialise());

	destroy();

	async_shutdown.stop();
	L_EV(this, "Stop async shutdown event");

	L_OBJ(this, "DELETED MANAGER! [%llx]", this);
}


std::string
XapiandManager::get_node_name()
{
	size_t length = 0;
	unsigned char buf[512];
	int fd = open("nodename", O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		length = read(fd, (char *)buf, sizeof(buf) - 1);
		if (length > 0) {
			buf[length] = '\0';
			for (size_t i = 0, j = 0; (buf[j] = buf[i]); j += !isspace(buf[i++]));
		}
		close(fd);
	}
	return std::string((const char *)buf, length);
}


bool
XapiandManager::set_node_name(const std::string &node_name_, std::unique_lock<std::mutex> &lk)
{
	if (node_name_.empty()) {
		lk.unlock();
		return false;
	}

	node_name = get_node_name();
	if (!node_name.empty() && stringtolower(node_name) != stringtolower(node_name_)) {
		lk.unlock();
		return false;
	}

	if (stringtolower(node_name) != stringtolower(node_name_)) {
		node_name = node_name_;

		int fd = open("nodename", O_WRONLY | O_CREAT, 0644);
		if (fd >= 0) {
			if (write(fd, node_name.c_str(), node_name.size()) != static_cast<ssize_t>(node_name.size())) {
				assert(false);
			}
			close(fd);
		} else {
			assert(false);
		}
	}

	L_NOTICE(this, "Node %s accepted to the party!", node_name.c_str());
	return true;
}


uint64_t
XapiandManager::get_node_id()
{
	return random_int(0, UINT64_MAX);
}


bool
XapiandManager::set_node_id()
{
	if (random_real(0, 1.0) < 0.5) return false;

	// std::lock_guard<std::mutex> lk(qmtx);
	local_node.id = get_node_id();

	return true;
}


void
XapiandManager::setup_node()
{
	std::shared_ptr<XapiandServer> server = std::static_pointer_cast<XapiandServer>(*_children.begin());
	server->async_setup_node.send();
}


void
XapiandManager::setup_node(std::shared_ptr<XapiandServer>&& server)
{
	int new_cluster = 0;

	std::unique_lock<std::mutex> lk(qmtx);

	// Open cluster database
	cluster_endpoints.clear();
	Endpoint cluster_endpoint(".");
	cluster_endpoints.insert(cluster_endpoint);

	std::shared_ptr<Database> cluster_database;
	if (!database_pool.checkout(cluster_database, cluster_endpoints, DB_WRITABLE | DB_PERSISTENT)) {
		new_cluster = 1;
		L_INFO(this, "Cluster database doesn't exist. Generating database...");
		if (!database_pool.checkout(cluster_database, cluster_endpoints, DB_WRITABLE | DB_SPAWN | DB_PERSISTENT)) {
			assert(false);
		}
	}
	database_pool.checkin(cluster_database);

	// Get a node (any node)
	std::unique_lock<std::mutex> lk_n(nodes_mtx);

	for (auto it = nodes.cbegin(); it != nodes.cend(); ++it) {
		const Node &node = it->second;
		Endpoint remote_endpoint(".", &node);
		// Replicate database from the other node
#ifdef HAVE_REMOTE_PROTOCOL
		L_INFO(this, "Syncing cluster data from %s...", node.name.c_str());

		auto ret = trigger_replication(remote_endpoint, *cluster_endpoints.begin());
		if (ret.get()) {
			L_INFO(this, "Cluster data being synchronized from %s...", node.name.c_str());
			new_cluster = 2;
			break;
		}
#endif
	}

	lk_n.unlock();

	// Set node as ready!
	set_node_name(local_node.name, lk);
	state = State::READY;

	switch (new_cluster) {
		case 0:
			L_NOTICE(this, "Joined cluster %s: It is now online!", cluster_name.c_str());
			break;
		case 1:
			L_NOTICE(this, "Joined new cluster %s: It is now online!", cluster_name.c_str());
			break;
		case 2:
			L_NOTICE(this, "Joined cluster %s: It was already online!", cluster_name.c_str());
			break;
	}
}


void
XapiandManager::reset_state()
{
	if (state != State::RESET) {
		state = State::RESET;
		discovery->start();
	}
}


bool
XapiandManager::is_single_node()
{
	return nodes.size() == 0;
}


bool
XapiandManager::put_node(const Node &node)
{
	std::lock_guard<std::mutex> lk(nodes_mtx);
	std::string node_name_lower(stringtolower(node.name));
	if (node_name_lower == stringtolower(local_node.name)) {
		local_node.touched = epoch::now<>();
		return false;
	} else {
		try {
			Node &node_ref = nodes.at(node_name_lower);
			if (node == node_ref) {
				node_ref.touched = epoch::now<>();
			}
		} catch (const std::out_of_range &err) {
			Node &node_ref = nodes[node_name_lower];
			node_ref = node;
			node_ref.touched = epoch::now<>();
			return true;
		} catch (...) {
			throw;
		}
	}
	return false;
}


bool
XapiandManager::get_node(const std::string &node_name, const Node **node)
{
	try {
		std::string node_name_lower(stringtolower(node_name));
		const Node &node_ref = nodes.at(node_name_lower);
		*node = &node_ref;
		return true;
	} catch (const std::out_of_range &err) {
		return false;
	}
}


bool
XapiandManager::touch_node(const std::string &node_name, int region, const Node **node)
{
	std::lock_guard<std::mutex> lk(nodes_mtx);
	std::string node_name_lower(stringtolower(node_name));
	if (node_name_lower == stringtolower(local_node.name)) {
		local_node.touched = epoch::now<>();
		if (region != UNKNOWN_REGION) {
			local_node.region.store(region);
		}
		if (node) *node = &local_node;
		return true;
	} else {
		try {
			Node &node_ref = nodes.at(node_name_lower);
			node_ref.touched = epoch::now<>();
			if (region != UNKNOWN_REGION) {
				node_ref.region.store(region);
			}
			if (node) *node = &node_ref;
			return true;
		} catch (const std::out_of_range &err) {
		} catch (...) {
			throw;
		}
	}
	return false;
}


void
XapiandManager::drop_node(const std::string &node_name)
{
	std::lock_guard<std::mutex> lk(nodes_mtx);
	nodes.erase(stringtolower(node_name));
}


size_t
XapiandManager::get_nodes_by_region(int region)
{
	size_t cont = 0;
	std::lock_guard<std::mutex> lk(nodes_mtx);
	for (auto it(nodes.begin()); it != nodes.end(); ++it) {
		if (it->second.region.load() == region) ++cont;
	}
	return cont;
}


struct sockaddr_in
XapiandManager::host_address()
{
	struct sockaddr_in addr;
	struct ifaddrs *if_addr_struct;
	if (getifaddrs(&if_addr_struct) < 0) {
		L_ERR(this, "ERROR: getifaddrs: %s", strerror(errno));
	} else {
		for (struct ifaddrs *ifa = if_addr_struct; ifa != NULL; ifa = ifa->ifa_next) {
			if (ifa->ifa_addr != NULL && ifa->ifa_addr->sa_family == AF_INET && !(ifa->ifa_flags & IFF_LOOPBACK)) { // check it is IP4
				char ip[INET_ADDRSTRLEN];
				addr = *(struct sockaddr_in *)ifa->ifa_addr;
				inet_ntop(AF_INET, &addr.sin_addr, ip, INET_ADDRSTRLEN);
				L_NOTICE(this, "Node IP address is %s on interface %s", ip, ifa->ifa_name);
				break;
			}
		}
		freeifaddrs(if_addr_struct);
	}
	return addr;
}


void
XapiandManager::sig_shutdown_handler(int sig)
{
	/* SIGINT is often delivered via Ctrl+C in an interactive session.
	 * If we receive the signal the second time, we interpret this as
	 * the user really wanting to quit ASAP without waiting to persist
	 * on disk. */
	auto now = epoch::now<>();

	if (XapiandManager::shutdown_now && sig != SIGTERM) {
		if (sig && now > XapiandManager::shutdown_asap + 1 && now < XapiandManager::shutdown_asap + 4) {
			L_INFO(this, "You insist... exiting now.");
			exit(1); /* Exit with an error since this was not a clean shutdown. */
		}
	} else if (XapiandManager::shutdown_asap && sig != SIGTERM) {
		if (sig && now > XapiandManager::shutdown_asap + 1 && now < XapiandManager::shutdown_asap + 4) {
			XapiandManager::shutdown_now = now;
			L_INFO(this, "Trying immediate shutdown.");
		} else if (sig == 0) {
			XapiandManager::shutdown_now = now;
		}
	} else {
		switch (sig) {
			case SIGINT:
				L_INFO(this, "Received SIGINT scheduling shutdown...");
				break;
			case SIGTERM:
				L_INFO(this, "Received SIGTERM scheduling shutdown...");
				break;
			default:
				L_INFO(this, "Received shutdown signal, scheduling shutdown...");
		};
	}

	if (now > XapiandManager::shutdown_asap + 1) {
		XapiandManager::shutdown_asap = now;
	}
	shutdown();
}


void
XapiandManager::destroy()
{
	L_OBJ(this, "DESTROYED MANAGER! [%llx]", this);
}


void
XapiandManager::async_shutdown_cb(ev::async &, int)
{
	L_EV_BEGIN(this, "XapiandManager::async_shutdown_cb:BEGIN");
	L_EV(this, "Async shutdown event received!");

	sig_shutdown_handler(0);
	L_EV_END(this, "XapiandManager::async_shutdown_cb:END");
}


void
XapiandManager::shutdown()
{
	L_CALL(this, "XapiandManager::shutdown()");

	Worker::shutdown();

	if (XapiandManager::shutdown_asap) {
		discovery->send_message(Discovery::Message::BYE, local_node.serialise());
		destroy();
		L_OBJ(this, "Finishing thread pool!");
		thread_pool.finish();
	}

	if (XapiandManager::shutdown_now) {
		L_EV(this, "Breaking Manager loop!");
		break_loop();
	}
}


void
XapiandManager::run(const opts_t &o)
{
	std::string msg("Listening on ");

	auto manager = share_this<XapiandManager>();

	auto http = std::make_shared<Http>(manager, o.http_port);
	msg += http->getDescription() + ", ";

#ifdef HAVE_REMOTE_PROTOCOL
	binary = std::make_shared<Binary>(manager, o.binary_port);
	msg += binary->getDescription() + ", ";
#endif

	discovery = std::make_shared<Discovery>(manager, loop, o.discovery_port, o.discovery_group);
	msg += discovery->getDescription() + ", ";

	raft = std::make_shared<Raft>(manager, loop, o.raft_port, o.raft_group);
	msg += raft->getDescription() + ", ";

	msg += "at pid:" + std::to_string(getpid()) + "...";

	L_NOTICE(this, msg.c_str());

	ThreadPool<> server_pool("S%02zu", o.num_servers);
	for (size_t i = 0; i < o.num_servers; ++i) {
		std::shared_ptr<XapiandServer> server = Worker::create<XapiandServer>(manager, nullptr);
		Worker::create<HttpServer>(server, server->loop, http);
#ifdef HAVE_REMOTE_PROTOCOL
		binary->add_server(Worker::create<BinaryServer>(server, server->loop, binary));
#endif
		Worker::create<DiscoveryServer>(server, server->loop, discovery);
		Worker::create<RaftServer>(server, server->loop, raft);
		server_pool.enqueue(std::move(server));
	}

	ThreadPool<> replicator_pool("R%02zu", o.num_replicators);
	for (size_t i = 0; i < o.num_replicators; ++i) {
		replicator_pool.enqueue(Worker::create<XapiandReplicator>(manager, nullptr));
	}

	ThreadPool<> autocommit_pool("C%02zu", o.num_committers);
	std::vector<std::shared_ptr<DatabaseAutocommit>> committers;
	for (size_t i = 0; i < o.num_committers; ++i) {
		auto dbcommit = std::make_shared<DatabaseAutocommit>(manager);
		autocommit_pool.enqueue(dbcommit);
		committers.push_back(dbcommit);
	}

	L_NOTICE(this, "Started %d server%s"
		     ", %d worker thread%s"
		     ", %d autocommitter%s"
		     ", %d replicator%s.",
		o.num_servers, (o.num_servers == 1) ? "" : "s",
		o.threadpool_size, (o.threadpool_size == 1) ? "" : "s",
		o.num_committers, (o.num_committers == 1) ? "" : "s",
		o.num_replicators, (o.num_replicators == 1) ? "" : "s"
	);

	L_INFO(this, "Joining cluster %s...", cluster_name.c_str());

	discovery->start();

	L_EV(this, "Starting manager loop...");
	loop->run();
	L_EV(this, "Manager loop ended!");

	L_DEBUG(this, "Waiting for servers...");
	server_pool.finish();
	server_pool.join();

	L_DEBUG(this, "Waiting for replicators...");
	replicator_pool.finish();
	replicator_pool.join();

	L_DEBUG(this, "Waiting for committers...");
	for (auto& commiter: committers) {
		commiter->shutdown();
	}
	autocommit_pool.finish();
	autocommit_pool.join();

	L_DEBUG(this, "Server ended!");
}


int
XapiandManager::get_region(const std::string &db_name)
{
	if (local_node.regions.load() == -1) {
		local_node.regions.store(sqrt(nodes.size()));
	}
	std::hash<std::string> hash_fn;
	return jump_consistent_hash(hash_fn(db_name), local_node.regions.load());
}


int
XapiandManager::get_region()
{
	if (local_node.regions.load() == -1) {
		if (is_single_node()) {
			local_node.regions.store(1);
			local_node.region.store(0);
			raft->stop();
		} else {
			raft->start();
			local_node.regions.store(sqrt(nodes.size() + 1));
			int region = jump_consistent_hash(local_node.id, local_node.regions.load());
			if (local_node.region.load() != region) {
				local_node.region.store(region);
				raft->reset();
			}
		}
		L_RAFT(this, "Regions: %d Region: %d", local_node.regions.load(), local_node.region.load());
	}
	return local_node.region.load();
}


std::future<bool>
XapiandManager::trigger_replication(const Endpoint &src_endpoint, const Endpoint &dst_endpoint)
{
	return binary->trigger_replication(src_endpoint, dst_endpoint);
}

std::future<bool>
XapiandManager::store(const Endpoints &endpoints, const Xapian::docid &did, const std::string &filename)
{
	return binary->store(endpoints, did, filename);
}


void
XapiandManager::server_status(MsgPack& stats)
{
//	unique_cJSON root_status(cJSON_CreateObject());
//	std::lock_guard<std::mutex> lk(XapiandServer::static_mutex);
//	cJSON_AddNumberToObject(root_status.get(), "Connections", XapiandServer::total_clients);
//	cJSON_AddNumberToObject(root_status.get(), "Http connections", XapiandServer::http_clients);
//	cJSON_AddNumberToObject(root_status.get(), "Xapian remote connections", XapiandServer::binary_clients);
//	cJSON_AddNumberToObject(root_status.get(), "Size thread pool", thread_pool.size());
//	return root_status;

	std::lock_guard<std::mutex> lk(XapiandServer::static_mutex);
	stats["Connections"] = XapiandServer::total_clients.load();
	stats["Http connections"] = XapiandServer::http_clients.load();
	stats["Xapian remote connections"] = XapiandServer::binary_clients.load();
	stats["Size thread pool"] = thread_pool.size();
}


void
XapiandManager::get_stats_time(MsgPack& stats, const std::string& time_req)
{
	std::smatch m;
	if (std::regex_match(time_req, m, time_re) && static_cast<size_t>(m.length()) == time_req.size() && m.length(1) != 0) {
		pos_time_t first_time, second_time;
		first_time.minute = SLOT_TIME_SECOND * (m.length(3) != 0 ? std::stoi(m.str(3)) : 0);
		first_time.minute += m.length(5) != 0 ? std::stoi(m.str(5)) : 0;
		first_time.second = m.length(7) != 0 ? std::stoi(m.str(7)) : 0;
		if (m.length(8) != 0) {
			second_time.minute = SLOT_TIME_SECOND * (m.length(10) != 0 ? std::stoi(m.str(10)) : 0);
			second_time.minute += m.length(12) != 0 ? std::stoi(m.str(12)) : 0;
			second_time.second = m.length(14) != 0 ? std::stoi(m.str(14)) : 0;
		} else {
			second_time.minute = 0;
			second_time.second = 0;
		}
		return _get_stats_time(stats, first_time, second_time);
	}

	stats["Error in time argument"] = "Incorrect input";
}


void
XapiandManager::_get_stats_time(MsgPack& stats, pos_time_t& first_time, pos_time_t& second_time)
{
	MsgPack time_period;

	std::unique_lock<std::mutex> lk(XapiandServer::static_mutex);
	update_pos_time();
	auto now_time = std::chrono::system_clock::to_time_t(init_time);
	auto b_time_cpy = b_time;
	auto stats_cnt_cpy = stats_cnt;
	lk.unlock();

	auto seconds = first_time.minute == 0 ? true : false;
	uint16_t start, end;
	if (second_time.minute == 0 && second_time.second == 0) {
		start = first_time.minute * SLOT_TIME_SECOND + first_time.second;
		end = 0;
		first_time.minute = (first_time.minute > b_time_cpy.minute ? SLOT_TIME_MINUTE + b_time_cpy.minute : b_time_cpy.minute) - first_time.minute;
		first_time.second = (first_time.second > b_time_cpy.second ? SLOT_TIME_SECOND + b_time_cpy.second : b_time_cpy.second) - first_time.second;
	} else {
		start = second_time.minute * SLOT_TIME_SECOND + second_time.second;
		end = first_time.minute * SLOT_TIME_SECOND + first_time.second;
		first_time.minute = (second_time.minute > b_time_cpy.minute ? SLOT_TIME_MINUTE + b_time_cpy.minute : b_time_cpy.minute) - second_time.minute;
		first_time.second = (second_time.second > b_time_cpy.second ? SLOT_TIME_SECOND + b_time_cpy.second : b_time_cpy.second) - second_time.second;
	}

	if (end > start) {
		stats["Error in time argument"] = "First argument must be less or equal than the second";
	} else {
		std::vector<uint64_t> cnt{0, 0, 0};
		std::vector<double> tm_cnt{0.0, 0.0, 0.0};
		stats["System time"] = ctime(&now_time);
		if (seconds) {
			auto aux = first_time.second + start - end;
			if (aux < SLOT_TIME_SECOND) {
				add_stats_sec(first_time.second, aux, cnt, tm_cnt, stats_cnt_cpy);
			} else {
				add_stats_sec(first_time.second, SLOT_TIME_SECOND - 1, cnt, tm_cnt, stats_cnt_cpy);
				add_stats_sec(0, aux % SLOT_TIME_SECOND, cnt, tm_cnt, stats_cnt_cpy);
			}
		} else {
			auto aux = first_time.minute + (start - end) / SLOT_TIME_SECOND;
			if (aux < SLOT_TIME_MINUTE) {
				add_stats_min(first_time.minute, aux, cnt, tm_cnt, stats_cnt_cpy);
			} else {
				add_stats_min(first_time.second, SLOT_TIME_MINUTE - 1, cnt, tm_cnt, stats_cnt_cpy);
				add_stats_min(0, aux % SLOT_TIME_MINUTE, cnt, tm_cnt, stats_cnt_cpy);
			}
		}
		auto p_time = now_time - start;
		time_period["Period start"] = ctime(&p_time);
		p_time = now_time - end;
		time_period["Period end"] = ctime(&p_time);
		stats["Time"] = time_period;
		stats["Docs index"] = cnt[0];
		stats["Number search"] = cnt[1];
		stats["Docs deleted"] = cnt[2];
		cnt[0] == 0 ? stats["Average time indexing (secs)"] = 0 : stats["Average time indexing (secs)"] = (tm_cnt[0] / cnt[0]) * NANOSEC;
		cnt[1] == 0 ? stats["Average search time (secs)"] = 0 : stats["Average search time (secs))"] = (tm_cnt[1] / cnt[1]) * NANOSEC;
		cnt[2] == 0 ? stats["Average deletion time (secs)"] = 0 : stats["Average deletion time (secs)"] = (tm_cnt[2] / cnt[2]) * NANOSEC;
	}
}
