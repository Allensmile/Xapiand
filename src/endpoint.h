/*
 * Copyright (C) 2015,2016 deipi.com LLC and contributors. All rights reserved.
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

#include "atomic_shared_ptr.h"
#include "utils.h"
#include "xapiand.h"

#include <string>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <atomic>


struct Node {
	uint64_t id;
	std::string name;
	struct sockaddr_in addr;
	int http_port;
	int binary_port;

	int32_t regions;
	int32_t region;
	time_t touched;

	Node() : id(0), http_port(0), binary_port(0), regions(1), region(0), touched(0) {
		memset(&addr, 0, sizeof(addr));
	}

	// Move constructor
	Node(Node&& other)
		: id(std::move(other.id)),
		  name(std::move(other.name)),
		  addr(std::move(other.addr)),
		  http_port(std::move(other.http_port)),
		  binary_port(std::move(other.binary_port)),
		  regions(std::move(other.regions)),   /* should be exist move a copy constructor? */
		  region(std::move(other.region)),
		  touched(std::move(other.touched)) { }

	// Copy Constructor
	Node(const Node& other)
		: id(other.id),
		  name(other.name),
		  addr(other.addr),
		  http_port(other.http_port),
		  binary_port(other.binary_port),
		  regions(other.regions),
		  region(other.region),
		  touched(other.touched) { }

	// Move assignment
	Node& operator=(Node&& other) {
		id = std::move(other.id);
		name = std::move(other.name);
		addr = std::move(other.addr);
		http_port = std::move(other.http_port);
		binary_port = std::move(other.binary_port);
		regions = std::move(other.regions);
		region = std::move(other.region);
		touched = std::move(other.touched);
		return *this;
	}

	// Copy assignment
	Node& operator=(const Node& other) {
		id = other.id;
		name = other.name;
		addr = other.addr;
		http_port = other.http_port;
		binary_port = other.binary_port;
		regions = other.regions;
		region = other.region;
		touched = other.touched;
		return *this;
	}

	void clear() {
		name.clear();
		id = 0;
		regions = 1;
		region = 0;
		memset(&addr, 0, sizeof(addr));
		http_port = 0;
		binary_port = 0;
		touched = 0;
	}

	bool empty() const {
		return name.empty();
	}

	std::string serialise() const;
	static Node unserialise(const char **p, const char *end);

	std::string host() const {
		char ip[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &addr.sin_addr, ip, INET_ADDRSTRLEN);
		return std::string(ip);
	}

	bool operator==(const Node& other) const {
		return
			lower_string(name) == lower_string(other.name) &&
			addr.sin_addr.s_addr == other.addr.sin_addr.s_addr &&
			http_port == other.http_port &&
			binary_port == other.binary_port;
	}

	bool operator!=(const Node& other) const {
		return !operator==(other);
	}

	std::string to_string() const {
		return name + " (" + std::to_string(id) + ")";
	}
};


extern atomic_shared_ptr<const Node> local_node;


class Endpoint;
class Endpoints;


#include <unordered_set>
#include <vector>


namespace std {
	template<>
	struct hash<Endpoint> {
		size_t operator()(const Endpoint &e) const;
	};


	template<>
	struct hash<Endpoints> {
		size_t operator()(const Endpoints &e) const;
	};
}

bool operator==(Endpoint const& le, Endpoint const& re);
bool operator==(Endpoints const& le, Endpoints const& re);


class Endpoint {
	inline std::string slice_after(std::string &subject, std::string delimiter);
	inline std::string slice_before(std::string &subject, std::string delimiter);

public:
	static std::string cwd;

	int port;
	std::string user, password, host, path, search;

	std::string node_name;
	long long mastery_level;

	Endpoint();
	Endpoint(const std::string &path_, const Node* node_=nullptr, long long mastery_level_=-1, const std::string& node_name="");

	bool is_local() const {
		auto local_node_ = local_node.load();
		int binary_port = local_node_->binary_port;
		if (!binary_port) binary_port = XAPIAND_BINARY_SERVERPORT;
		return (host == local_node_->host() || host == "127.0.0.1" || host == "localhost") && port == binary_port;
	}

	size_t hash() const;
	std::string to_string() const;

	bool operator<(const Endpoint & other) const;
	bool operator==(const Node &other) const;

	struct compare {
		constexpr bool operator() (const Endpoint &a, const Endpoint &b) const noexcept {
			return b.mastery_level > a.mastery_level;
		}
	};
};


class Endpoints : private std::vector<Endpoint> {
	std::unordered_set<Endpoint> endpoints;

public:
	using std::vector<Endpoint>::empty;
	using std::vector<Endpoint>::size;
	using std::vector<Endpoint>::operator[];
	using std::vector<Endpoint>::begin;
	using std::vector<Endpoint>::end;
	using std::vector<Endpoint>::cbegin;
	using std::vector<Endpoint>::cend;

	Endpoints() { }

	Endpoints(const Endpoint &endpoint) {
		add(endpoint);
	}

	size_t hash() const;
	std::string to_string() const;

	void clear() {
		endpoints.clear();
		std::vector<Endpoint>::clear();
	}

	void add(const Endpoint& endpoint) {
		auto p = endpoints.insert(endpoint);
		if (p.second) {
			push_back(endpoint);
		}
	}
};
