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

#include "endpoint.h"

#include "length.h"

#include <iostream>
#include <limits.h>
#include <unistd.h>


Node local_node;


std::string
Node::serialise() const
{
	std::string node_str;
	if (!name.empty()) {
		node_str.append(serialise_length(addr.sin_addr.s_addr));
		node_str.append(serialise_length(http_port));
		node_str.append(serialise_length(binary_port));
		node_str.append(serialise_length(region.load()));
		node_str.append(serialise_string(name));
	}
	return node_str;
}


Node
Node::unserialise(const char **p, const char *end)
{
	const char *ptr = *p;

	Node node;

	node.addr.sin_addr.s_addr = static_cast<int>(unserialise_length(&ptr, end, false));
	node.http_port = static_cast<int>(unserialise_length(&ptr, end, false));
	node.binary_port = static_cast<int>(unserialise_length(&ptr, end, false));
	node.region.store(static_cast<int>(unserialise_length(&ptr, end, false)));

	node.name = unserialise_string(&ptr, end);
	if (node.name.empty()) {
		throw Xapian::SerialisationError("Bad Node: No name");
	}

	*p = ptr;

	return node;
}


std::string Endpoint::cwd("/");


Endpoint::Endpoint()
	: mastery_level(-1) { }


Endpoint::Endpoint(const std::string &uri_, const Node *node_, long long mastery_level_, std::string node_name_)
	: node_name(node_name_),
	  mastery_level(mastery_level_)
{
	std::string uri(uri_);
	char buffer[PATH_MAX + 1];
	std::string protocol = slice_before(uri, "://");
	if (protocol.empty()) {
		protocol = "file";
	}
	search = slice_after(uri, "?");
	path = slice_after(uri, "/");
	std::string userpass = slice_before(uri, "@");
	password = slice_after(userpass, ":");
	user = userpass;
	std::string portstring = slice_after(uri, ":");
	port = atoi(portstring.c_str());
	if (protocol.empty() || protocol == "file") {
		if (path.empty()) {
			path = uri;
		} else {
			path = uri + "/" + path;
		}
		port = 0;
		search = "";
		password = "";
		user = "";
	} else {
		host = uri;
		if (!port) port = XAPIAND_BINARY_SERVERPORT;
	}
	path = Endpoint::cwd + path;
	normalize_path(path.c_str(), buffer);
	path = buffer;
	if (path.substr(0, Endpoint::cwd.size()) == Endpoint::cwd) {
		path.erase(0, Endpoint::cwd.size());
	} else {
		path = "";
	}
	if (protocol == "file") {
		if (!node_) {
			node_ = &local_node;
		}
		protocol = "xapian";
		host = node_->host();
		port = node_->binary_port;
		if (!port) port = XAPIAND_BINARY_SERVERPORT;
	}
}


std::string Endpoint::slice_after(std::string &subject, std::string delimiter) {
	size_t delimiter_location = subject.find(delimiter);
	size_t delimiter_length = delimiter.length();
	std::string output = "";
	if (delimiter_location < std::string::npos) {
		size_t start = delimiter_location + delimiter_length;
		output = subject.substr(start, subject.length() - start);
		subject = subject.substr(0, delimiter_location);
	}
	return output;
}


std::string Endpoint::slice_before(std::string &subject, std::string delimiter) {
	size_t delimiter_location = subject.find(delimiter);
	size_t delimiter_length = delimiter.length();
	std::string output = "";
	if (delimiter_location < std::string::npos) {
		size_t start = delimiter_location + delimiter_length;
		output = subject.substr(0, delimiter_location);
		subject = subject.substr(start, subject.length() - start);
	}
	return output;
}


std::string Endpoint::as_string() const {
	std::string ret;
	if (path.empty()) {
		return ret;
	}
	ret += "xapian://";
	if (!user.empty() || !password.empty()) {
		ret += user;
		if (!password.empty()) {
			ret += ":" + password;
		}
		ret += "@";
	}
	ret += host;
	if (port > 0) {
		ret += ":";
		ret += std::to_string(port);
	}
	if (!host.empty() || port > 0) {
		ret += "/";
	}
	ret += path;
	if (!search.empty()) {
		ret += "?" + search;
	}
	return ret;
}


bool Endpoint::operator<(const Endpoint & other) const
{
	return hash() < other.hash();
}


size_t Endpoint::hash() const {
	std::hash<std::string> hash_fn_string;
	std::hash<int> hash_fn_int;
	return (
		hash_fn_string(path) ^
		hash_fn_string(user) ^
		hash_fn_string(password) ^
		hash_fn_string(host) ^
		hash_fn_int(port) ^
		hash_fn_string(search)
	);
}


std::string Endpoints::as_string() const {
	std::string ret;
	auto j = endpoints.cbegin();
	for (int i=0; j != endpoints.cend(); j++, i++) {
		if (i) ret += ";";
		ret += (*j).as_string();
	}
	return ret;
}


size_t Endpoints::hash() const {
	size_t hash = 0;
	std::hash<Endpoint> hash_fn;
	auto j = endpoints.cbegin();
	for (int i = 0; j != endpoints.cend(); j++, i++) {
		hash ^= hash_fn(*j);
	}
	return hash;
}


bool operator == (Endpoint const& le, Endpoint const& re)
{
	std::hash<Endpoint> hash_fn;
	return hash_fn(le) == hash_fn(re);
}


bool operator == (Endpoints const& le, Endpoints const& re)
{
	std::hash<Endpoints> hash_fn;
	return hash_fn(le) == hash_fn(re);
}


size_t std::hash<Endpoint>::operator()(const Endpoint &e) const
{
	return e.hash();
}


size_t std::hash<Endpoints>::operator()(const Endpoints &e) const
{
	return e.hash();
}
