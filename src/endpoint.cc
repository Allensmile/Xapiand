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

#include "endpoint.h"

#include <cstdlib>          // for atoi
#include <xapian.h>         // for SerialisationError

#include "length.h"         // for serialise_length, unserialise_length, ser...
#include "opts.h"           // for opts
#include "serialise.h"      // for Serialise
#include "string.hh"        // for string::Number


static inline std::string
normalize(const void *p, size_t size)
{
	auto repr = static_cast<UUIDRepr>(opts.uuid_repr);
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
	auto repr = static_cast<UUIDRepr>(opts.uuid_repr);
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
	} else {
		return normalized;
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
			if (serialised_uuid.front() != 1 && (((serialised_uuid.back() & 1) != 0) || (serialised_uuid.size() > 5 && ((*(serialised_uuid.rbegin() + 5) & 2) != 0)))) {
				auto cit = normalized.cbegin();
				auto cit_e = normalized.cend();
				result.reserve(4 + normalized.size());
				if (cit == cit_e) { return result; }
				result.push_back(*cit++);
				if (cit == cit_e) { return result; }
				result.push_back('/');
				result.push_back(*cit++);
				if (cit == cit_e) { return result; }
				result.push_back(*cit++);
				if (cit == cit_e) { return result; }
				result.push_back('/');
				result.push_back(*cit++);
				if (cit == cit_e) { return result; }
				result.push_back(*cit++);
				if (cit == cit_e) { return result; }
				result.push_back('/');
				result.push_back(*cit++);
				if (cit == cit_e) { return result; }
				result.push_back(*cit++);
				if (cit == cit_e) { return result; }
				result.push_back('/');
				result.append(cit, cit_e);
				break;
			}
			/* FALLTHROUGH */
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


using normalizer_t = std::string (*)(const void *, size_t);
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
	return _name.empty()
		? ""
		: serialise_length(_addr.sin_addr.s_addr) +
			serialise_length(http_port) +
			serialise_length(binary_port) +
			serialise_string(_name);
}


Node
Node::unserialise(const char **p, const char *end)
{
	const char *ptr = *p;

	Node node;

	node._addr.sin_addr.s_addr = unserialise_length(&ptr, end);
	node.http_port = unserialise_length(&ptr, end);
	node.binary_port = unserialise_length(&ptr, end);

	node._name = unserialise_string(&ptr, end);
	if (node._name.empty()) {
		throw Xapian::SerialisationError("Bad Node: No name");
	}

	node._lower_name = string::lower(node._name);
	node._host = fast_inet_ntop4(node._addr.sin_addr);

	*p = ptr;

	return node;
}


atomic_shared_ptr<const Node> Node::_local_node{std::make_shared<const Node>()};
atomic_shared_ptr<const Node> Node::_master_node{std::make_shared<const Node>()};


#ifndef XAPIAND_CLUSTERING

static std::shared_ptr<const Node>
Node::local_node(std::shared_ptr<const Node> node)
{
	atomic_shared_ptr<const Node> _local_node{std::make_shared<const Node>()};
	if (node) {
		_local_node.store(node);
	}
	return _local_node.load();
}


static std::shared_ptr<const Node>
Node::master_node(std::shared_ptr<const Node> node)
{
	atomic_shared_ptr<const Node> _master_node{std::make_shared<const Node>()};
	if (node) {
		_master_node.store(node);
	}
	return _master_node.load();
}

#else

std::mutex Node::_nodes_mtx;
std::unordered_map<std::string, std::shared_ptr<const Node>> Node::_nodes;

std::atomic_size_t Node::total_nodes;
std::atomic_size_t Node::active_nodes;


inline void
Node::_update_nodes(const std::shared_ptr<const Node>& node)
{
	auto local_node_ = _local_node.load();
	if (node->lower_name() == local_node_->lower_name()) {
		_local_node.store(node);
	}

	auto master_node_ = _master_node.load();
	if (node->lower_name() == master_node_->lower_name()) {
		_master_node.store(node);
	}
}


std::shared_ptr<const Node>
Node::local_node(std::shared_ptr<const Node> node)
{
	if (node) {
		auto now = epoch::now<>();
		auto node_copy = std::make_unique<Node>(*node);
		node_copy->touched = now;
		node = std::shared_ptr<const Node>(node_copy.release());
		_local_node.store(node);
		auto master_node_ = _master_node.load();
		if (node->lower_name() == master_node_->lower_name()) {
			_master_node.store(node);
		}
		std::lock_guard<std::mutex> lk(_nodes_mtx);
		auto it = _nodes.find(node->lower_name());
		if (it != _nodes.end()) {
			auto& node_ref = it->second;
			node_ref = node;
		}
	}
	return _local_node.load();
}


std::shared_ptr<const Node>
Node::master_node(std::shared_ptr<const Node> node)
{
	if (node) {
		auto now = epoch::now<>();
		auto node_copy = std::make_unique<Node>(*node);
		node_copy->touched = now;
		node = std::shared_ptr<const Node>(node_copy.release());
		_master_node.store(node);
		auto local_node_ = _local_node.load();
		if (node->lower_name() == local_node_->lower_name()) {
			_local_node.store(node);
		}
		std::lock_guard<std::mutex> lk(_nodes_mtx);
		auto it = _nodes.find(node->lower_name());
		if (it != _nodes.end()) {
			auto& node_ref = it->second;
			node_ref = node;
		}
	}
	return _master_node.load();
}


std::shared_ptr<const Node>
Node::get_node(std::string_view _node_name)
{
	auto lower_node_name = string::lower(_node_name);

	std::lock_guard<std::mutex> lk(_nodes_mtx);
	auto it = _nodes.find(lower_node_name);
	if (it != _nodes.end()) {
		return it->second;
	}
	return nullptr;
}


std::pair<std::shared_ptr<const Node>, bool>
Node::put_node(std::shared_ptr<const Node> node)
{
	auto now = epoch::now<>();

	std::lock_guard<std::mutex> lk(_nodes_mtx);
	auto it = _nodes.find(node->lower_name());
	if (it != _nodes.end()) {
		auto& node_ref = it->second;
		if (*node == *node_ref) {
			auto node_copy = std::make_unique<Node>(*node_ref);
			node_copy->touched = now;
			node_ref = std::shared_ptr<const Node>(node_copy.release());
			_update_nodes(node_ref);
		}
		return std::make_pair(node_ref, false);
	}

	auto node_copy = std::make_unique<Node>(*node);
	node_copy->touched = now;
	auto final_node = std::shared_ptr<const Node>(node_copy.release());
	_nodes[node->lower_name()] = final_node;
	_update_nodes(final_node);

	size_t cnt = 0;
	for (const auto& node_pair : _nodes) {
		if (node_pair.second->touched >= now - NODE_LIFESPAN) {
			++cnt;
		}
	}
	active_nodes = cnt;
	total_nodes = _nodes.size();

	return std::make_pair(final_node, true);
}


std::shared_ptr<const Node>
Node::touch_node(std::string_view _node_name)
{
	auto now = epoch::now<>();
	auto lower_node_name = string::lower(_node_name);

	std::lock_guard<std::mutex> lk(_nodes_mtx);
	auto it = _nodes.find(lower_node_name);
	if (it != _nodes.end()) {
		auto& node_ref = it->second;
		if (node_ref->touched < now - NODE_LIFESPAN) {
			return nullptr;
		}
		auto node_ref_copy = std::make_unique<Node>(*node_ref);
		node_ref_copy->touched = now;
		node_ref = std::shared_ptr<const Node>(node_ref_copy.release());
		_update_nodes(node_ref);
		return node_ref;
	}

	return nullptr;
}


void
Node::drop_node(std::string_view _node_name)
{
	auto now = epoch::now<>();
	auto lower_node_name = string::lower(_node_name);

	std::lock_guard<std::mutex> lk(_nodes_mtx);
	auto it = _nodes.find(lower_node_name);
	if (it != _nodes.end()) {
		auto& node_ref = it->second;
		auto node_ref_copy = std::make_unique<Node>(*node_ref);
		node_ref_copy->touched = 0;
		node_ref = std::shared_ptr<const Node>(node_ref_copy.release());
		_update_nodes(node_ref);
	}

	size_t cnt = 0;
	for (const auto& node_pair : _nodes) {
		if (node_pair.second->touched >= now - NODE_LIFESPAN) {
			++cnt;
		}
	}
	active_nodes = cnt;
	total_nodes = _nodes.size();
}


void
Node::reset()
{
	std::lock_guard<std::mutex> lk(_nodes_mtx);
	_nodes.clear();
}


std::vector<std::shared_ptr<const Node>>
Node::nodes()
{
	std::vector<std::shared_ptr<const Node>> nodes;
	for (const auto& node_pair : _nodes) {
		nodes.push_back(node_pair.second);
	}
	return nodes;
}

#endif


std::string Endpoint::cwd("/");


Endpoint::Endpoint()
	: port(-1) { }


Endpoint::Endpoint(std::string_view uri, const Node* node_, std::string_view node_name_)
	: node_name(node_name_)
{
	auto protocol = slice_before(uri, "://");
	if (protocol.empty()) {
		protocol = "file";
	}
	auto _search = slice_after(uri, "?");
	auto _path = slice_after(uri, "/");
	auto _user = slice_before(uri, "@");
	auto _password = slice_after(_user, ":");
	auto _port = slice_after(uri, ":");

	if (protocol == "file") {
		if (_path.empty()) {
			_path = uri;
		} else {
			_path = path = std::string(uri) + "/" + std::string(_path);
		}
		port = 0;
	} else {
		host = uri;
		port = strict_stoi(nullptr, _port);
		if (port == 0) {
			port = XAPIAND_BINARY_SERVERPORT;
		}
		search = _search;
		password = _password;
		user = _user;
	}

	if (!string::startswith(_path, '/')) {
		_path = path = Endpoint::cwd + std::string(_path);
	}
	path = normalize_path(_path);
	_path = path;
	if (string::startswith(_path, Endpoint::cwd)) {
		_path.remove_prefix(Endpoint::cwd.size());
	}

	if (_path.size() != 1 && string::endswith(_path, '/')) {
		_path.remove_suffix(1);
	}

	if (opts.uuid_partition) {
		path = normalizer<normalize_and_partition>(_path.data(), _path.size());
	} else {
		path = normalizer<normalize>(_path.data(), _path.size());
	}

	if (protocol == "file") {
		auto local_node_ = Node::local_node();
		if (node_ == nullptr) {
			node_ = local_node_.get();
		}
		host = node_->host();
		port = node_->binary_port;
		if (port == 0) {
			port = XAPIAND_BINARY_SERVERPORT;
		}
	}
}


inline std::string_view
Endpoint::slice_after(std::string_view& subject, std::string_view delimiter) const
{
	size_t delimiter_location = subject.find(delimiter);
	std::string_view output;
	if (delimiter_location != std::string_view::npos) {
		size_t start = delimiter_location + delimiter.size();
		output = subject.substr(start, subject.size() - start);
		if (!output.empty()) {
			subject = subject.substr(0, delimiter_location);
		}
	}
	return output;
}


inline std::string_view
Endpoint::slice_before(std::string_view& subject, std::string_view delimiter) const
{
	size_t delimiter_location = subject.find(delimiter);
	std::string_view output;
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
		ret += string::Number(port);
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


std::string
Endpoints::to_string() const
{
	std::string ret;
	auto j = endpoints.cbegin();
	for (int i = 0; j != endpoints.cend(); ++j, ++i) {
		if (i != 0) {
			ret += ";";
		}
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
