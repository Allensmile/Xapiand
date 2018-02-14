/*
 * Copyright (C) 2015-2018 dubalu.com LLC. All rights reserved.
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

#include <stdlib.h>         // for atoi
#include <xapian.h>         // for SerialisationError

#include "length.h"         // for serialise_length, unserialise_length, ser...
#include "opts.h"           // for opts
#include "serialise.h"      // for Serialise


atomic_shared_ptr<const Node> local_node(std::make_shared<const Node>());


static inline std::string
normalize(const void *p, size_t size)
{
	UUIDRepr repr = static_cast<UUIDRepr>(opts.uuid_repr);
	std::string serialised_uuid;
	std::string normalized;
	std::string unserialised(static_cast<const char*>(p), size);
	if (Serialise::possiblyUUID(unserialised)) {
		try {
			serialised_uuid = Serialise::uuid(unserialised);
			normalized = Unserialise::uuid(serialised_uuid, repr);
		} catch (const SerialisationError&) { }
	}
	return normalized;
}


static inline std::string
normalize_and_partition(const void *p, size_t size)
{
	UUIDRepr repr = static_cast<UUIDRepr>(opts.uuid_repr);
	std::string serialised_uuid;
	std::string normalized;
	std::string unserialised(static_cast<const char*>(p), size);
	if (Serialise::possiblyUUID(unserialised)) {
		try {
			serialised_uuid = Serialise::uuid(unserialised);
			normalized = Unserialise::uuid(serialised_uuid, repr);
		} catch (const SerialisationError& exc) {
			return normalized;
		}
	}

	std::string result;
	switch (repr) {
#ifdef XAPIAND_UUID_GUID
		case UUIDRepr::guid:
			// {00000000-0000-1000-8000-010000000000}
			result.reserve(2 + 8 + normalized.size());
			result.append(&normalized[1 + 14], &normalized[1 + 18]);
			result.push_back('/');
			result.append(&normalized[1 + 9], &normalized[1 + 13]);
			result.push_back('/');
			result.append(normalized);
			break;
#endif
#ifdef XAPIAND_UUID_URN
		case UUIDRepr::urn:
			// urn:uuid:00000000-0000-1000-8000-010000000000
			result.reserve(2 + 8 + normalized.size());
			result.append(&normalized[9 + 14], &normalized[9 + 18]);
			result.push_back('/');
			result.append(&normalized[9 + 9], &normalized[9 + 13]);
			result.push_back('/');
			result.append(normalized);
			break;
#endif
#ifdef XAPIAND_UUID_ENCODED
		case UUIDRepr::encoded:
			if (serialised_uuid.front() != 1 && ((serialised_uuid.back() & 1) || (serialised_uuid.size() > 5 && *(serialised_uuid.rbegin() + 5) & 2))) {
				auto cit = normalized.cbegin();
				auto cit_e = normalized.cend();
				result.reserve(2 + normalized.size());
				if (cit == cit_e) return result;
				result.push_back(*cit++);
				if (cit == cit_e) return result;
				result.push_back(*cit++);
				if (cit == cit_e) return result;
				result.push_back(*cit++);
				if (cit == cit_e) return result;
				result.push_back(*cit++);
				if (cit == cit_e) return result;
				result.push_back('/');
				result.push_back(*cit++);
				if (cit == cit_e) return result;
				result.push_back(*cit++);
				if (cit == cit_e) return result;
				result.push_back('/');
				result.append(cit, cit_e);
				break;
			}
#endif
		default:
		case UUIDRepr::simple:
			// 00000000-0000-1000-8000-010000000000
			result.reserve(2 + 8 + normalized.size());
			result.append(&normalized[14], &normalized[18]);
			result.push_back('/');
			result.append(&normalized[9], &normalized[13]);
			result.push_back('/');
			result.append(normalized);
			break;
	}
	return result;
}


typedef std::string(*normalizer_t)(const void *p, size_t size);
template<normalizer_t normalize>
static inline std::string
normalizer(const void *p, size_t size)
{
	std::string buf;
	buf.reserve(size);
	auto q = static_cast<const char *>(p);
	auto p_end = q + size;
	auto oq = q;
	while (q != p_end) {
		char c = *q++;
		switch (c) {
			case '.':
			case '/': {
				auto sz = q - oq - 1;
				if (sz) {
					auto normalized = normalize(oq, sz);
					if (!normalized.empty()) {
						buf.resize(buf.size() - sz);
						buf.append(normalized);
					}
				}
				buf.push_back(c);
				oq = q;
				break;
			}
			default:
				buf.push_back(c);
		}
	}
	auto sz = q - oq;
	if (sz) {
		auto normalized = normalize(oq, sz);
		if (!normalized.empty()) {
			buf.resize(buf.size() - sz);
			buf.append(normalized);
		}
	}
	return buf;
}


std::string
Node::serialise() const
{
	std::string node_str;
	if (!name.empty()) {
		node_str.append(serialise_length(addr.sin_addr.s_addr));
		node_str.append(serialise_length(http_port));
		node_str.append(serialise_length(binary_port));
		node_str.append(serialise_length(region));
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
	node.region = static_cast<int>(unserialise_length(&ptr, end, false));

	node.name = unserialise_string(&ptr, end);
	if (node.name.empty()) {
		throw Xapian::SerialisationError("Bad Node: No name");
	}

	*p = ptr;

	return node;
}


std::string Endpoint::cwd("/");


Endpoint::Endpoint()
	: port(-1),
	  mastery_level(-1) { }


Endpoint::Endpoint(const std::string& uri_, const Node* node_, long long mastery_level_, const std::string& node_name_)
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
	if (protocol == "file") {
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

	if (!startswith(path, "/")) {
		path = Endpoint::cwd + path;
	}
	path = normalize_path(path, buffer);
	if (path.substr(0, Endpoint::cwd.size()) == Endpoint::cwd) {
		path.erase(0, Endpoint::cwd.size());
	}

	if (path.size() != 1 && endswith(path, '/')) {
		path = path.substr(0, path.size() - 1);
	}

	if (opts.uuid_partition) {
		path = normalizer<normalize_and_partition>(path.data(), path.size());
	} else {
		path = normalizer<normalize>(path.data(), path.size());
	}

	if (protocol == "file") {
		auto local_node_ = local_node.load();
		if (!node_) {
			node_ = local_node_.get();
		}
		host = node_->host();
		port = node_->binary_port;
		if (!port) port = XAPIAND_BINARY_SERVERPORT;
	}
}


inline std::string
Endpoint::slice_after(std::string& subject, const std::string& delimiter) const
{
	size_t delimiter_location = subject.find(delimiter);
	std::string output;
	if (delimiter_location != std::string::npos) {
		size_t start = delimiter_location + delimiter.length();
		output = subject.substr(start, subject.length() - start);
		if (!output.empty()) {
			subject = subject.substr(0, delimiter_location);
		}
	}
	return output;
}


inline std::string
Endpoint::slice_before(std::string& subject, const std::string& delimiter) const
{
	size_t delimiter_location = subject.find(delimiter);
	std::string output;
	if (delimiter_location != std::string::npos) {
		size_t start = delimiter_location + delimiter.length();
		output = subject.substr(0, delimiter_location);
		subject = subject.substr(start, subject.length() - start);
	}
	return output;
}


std::string
Endpoint::to_string() const
{
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


bool
Endpoint::operator<(const Endpoint& other) const
{
	return hash() < other.hash();
}


size_t
Endpoint::hash() const
{
	static std::hash<std::string> hash_fn_string;
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


std::string
Endpoints::to_string() const
{
	std::string ret;
	auto j = endpoints.cbegin();
	for (int i = 0; j != endpoints.cend(); ++j, ++i) {
		if (i) ret += ";";
		ret += (*j).to_string();
	}
	return ret;
}


size_t
Endpoints::hash() const
{
	size_t hash = 0;
	std::hash<Endpoint> hash_fn;
	auto j = endpoints.cbegin();
	for (int i = 0; j != endpoints.cend(); ++j, ++i) {
		hash ^= hash_fn(*j);
	}
	return hash;
}


bool
operator==(const Endpoint& le, const Endpoint& re)
{
	std::hash<Endpoint> hash_fn;
	return hash_fn(le) == hash_fn(re);
}


bool
operator==(const Endpoints& le, const Endpoints& re)
{
	std::hash<Endpoints> hash_fn;
	return hash_fn(le) == hash_fn(re);
}


bool
operator!=(const Endpoint& le, const Endpoint& re)
{
	return !(le == re);
}


bool
operator!=(const Endpoints& le, const Endpoints& re)
{
	return !(le == re);
}


size_t
std::hash<Endpoint>::operator()(const Endpoint& e) const
{
	return e.hash();
}


size_t
std::hash<Endpoints>::operator()(const Endpoints& e) const
{
	return e.hash();
}
