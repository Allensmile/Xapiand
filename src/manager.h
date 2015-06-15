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

#ifndef XAPIAND_INCLUDED_MANAGER_H
#define XAPIAND_INCLUDED_MANAGER_H

#include "xapiand.h"

#include "database.h"
#include "threadpool.h"

#include <list>
#include <ev++.h>
#include <pthread.h>
#include <netinet/in.h>

#ifdef HAVE_CXX11
#include <unordered_map>
#else
#include <map>
#endif

#define XAPIAND_DISCOVERY_PROTOCOL_MAJOR_VERSION 1
#define XAPIAND_DISCOVERY_PROTOCOL_MINOR_VERSION 0

#define STATE_BAD        -1
#define STATE_READY       0
#define STATE_WAITING___  1
#define STATE_WAITING__   2
#define STATE_WAITING_    3
#define STATE_WAITING     4
#define STATE_RESET       5


class XapiandServer;
class BaseClient;

struct Node {
	std::string name;
	struct sockaddr_in addr;
	int http_port;
	int binary_port;
	time_t touched;

	std::string serialise();
	size_t unserialise(const char **p, const char *end);
	size_t unserialise(const std::string &s);

	inline bool operator==(const Node& other) const {
		return (
			stringtolower(name) == stringtolower(other.name) &&
			addr.sin_addr.s_addr == other.addr.sin_addr.s_addr &&
			http_port == other.http_port &&
			binary_port == other.binary_port
		);
	}
};

enum discovery_type {
	DISCOVERY_HELLO,    // New node saying hello
	DISCOVERY_WAVE,     // Nodes waving hello to the new node
	DISCOVERY_SNEER,    // Nodes telling the client they don't agree on the new node's name
	DISCOVERY_PING,     // Ping
	DISCOVERY_PONG,     // Pong
	DISCOVERY_BYE,      // Node says goodbye
	DISCOVERY_DB,
	DISCOVERY_DB_WAVE,
	DISCOVERY_MAX
};


class XapiandManager {
#ifdef HAVE_CXX11
	typedef std::unordered_map<std::string, Node> nodes_map_t;
#else
	typedef std::map<std::string, Node> nodes_map_t;
#endif

private:
	ev::dynamic_loop dynamic_loop;
	ev::loop_ref *loop;

	pthread_mutex_t qmtx;
	pthread_mutexattr_t qmtx_attr;

	ev::timer discovery_heartbeat;
	struct sockaddr_in discovery_addr;
	int discovery_port;

	int discovery_sock;
	int http_sock;
	int binary_sock;

	static pcre *compiled_time_re;

	DatabasePool database_pool;
	ThreadPool thread_pool;

	Database *cluster_database;

	ev::async break_loop;
	void break_loop_cb(ev::async &watcher, int revents);

	void discovery_heartbeat_cb(ev::timer &watcher, int revents);
	void discovery(const char *message, size_t size);

	void check_tcp_backlog(int tcp_backlog);
	void shutdown_cb(ev::async &watcher, int revents);
	struct sockaddr_in host_address();
	void destroy();

protected:
	pthread_mutex_t nodes_mtx;
	pthread_mutexattr_t nodes_mtx_attr;
	nodes_map_t nodes;

	pthread_mutex_t servers_mutex;
	pthread_mutexattr_t servers_mutex_attr;
	std::list<XapiandServer *>servers;

public:
	time_t shutdown_asap;
	time_t shutdown_now;
	ev::async async_shutdown;

	unsigned char state;
	std::string cluster_name;
	std::string node_name;
	Node this_node;

	XapiandManager(ev::loop_ref *loop_, const std::string &cluster_name_, const std::string &node_name_, const std::string &discovery_group_, int discovery_port_, int http_port_, int binary_port_, size_t dbpool_size);
	~XapiandManager();

	void run(int num_servers);
	void sig_shutdown_handler(int sig);
	void shutdown();

	void reset_state();
	std::list<XapiandServer *>::const_iterator attach_server(XapiandServer *server);
	void detach_server(XapiandServer *server);

	bool put_node(Node &node);
	bool touch_node(std::string &node_name, Node *node=NULL);
	void drop_node(std::string &node_name);

	void discovery(discovery_type type, const std::string &content);

	cJSON* server_status();
	cJSON* get_stats_time(const std::string &time_req);
	cJSON* get_stats_json(pos_time_t first_time, pos_time_t second_time);
};


#endif /* XAPIAND_INCLUDED_MANAGER_H */
