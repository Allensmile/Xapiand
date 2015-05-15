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

#ifndef XAPIAND_INCLUDED_ENDPOINT_H
#define XAPIAND_INCLUDED_ENDPOINT_H

#include "xapiand.h"
#include "utils.h"

#include <string>
#include <netinet/in.h>


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


class Endpoint;
class Endpoints;


#ifdef HAVE_CXX11
#  include <unordered_set>
   typedef std::unordered_set<Endpoint> endpoints_set_t;
#else
#  include <set>
   typedef std::set<Endpoint> endpoints_set_t;
#endif


inline char *normalize_path(const char * src, char * dst);

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

bool operator == (Endpoint const& le, Endpoint const& re);
bool operator == (Endpoints const& le, Endpoints const& re);


class Endpoint {
	inline std::string slice_after(std::string &subject, std::string delimiter);
	inline std::string slice_before(std::string &subject, std::string delimiter);

public:
	int port;
	std::string protocol, user, password, host, path, search;

	Endpoint(const std::string &path_, const Node &node_);
	Endpoint(const std::string &uri, const std::string &base_=std::string(), int port_=XAPIAND_BINARY_SERVERPORT);
	std::string as_string() const;
	bool operator< (const Endpoint & other) const;
};


class Endpoints : public endpoints_set_t {
public:
	size_t hash() const;
	std::string as_string() const;
};

#endif /* XAPIAND_INCLUDED_ENDPOINT_H */

