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

#include "server_discovery.h"

#ifdef XAPIAND_CLUSTERING

#include "binary.h"
#include "discovery.h"
#include "server.h"

#include <assert.h>
#include <arpa/inet.h>


using dispatch_func = void (DiscoveryServer::*)(const std::string&);

DiscoveryServer::DiscoveryServer(const std::shared_ptr<XapiandServer>& server_, ev::loop_ref *loop_, const std::shared_ptr<Discovery> &discovery_)
	: BaseServer(server_, loop_, discovery_->sock),
	discovery(discovery_)
{
	// accept event actually started in BaseServer::BaseServer
	L_EV(this, "Start discovery's server accept event (sock=%d)", discovery->sock);

	L_OBJ(this, "CREATED DISCOVERY SERVER!");
}


DiscoveryServer::~DiscoveryServer()
{
	L_OBJ(this, "DELETED DISCOVERY SERVER!");
}


void
DiscoveryServer::discovery_server(Discovery::Message type, const std::string &message)
{
	static const dispatch_func dispatch[] = {
		&DiscoveryServer::heartbeat,
		&DiscoveryServer::hello,
		&DiscoveryServer::wave,
		&DiscoveryServer::sneer,
		&DiscoveryServer::bye,
		&DiscoveryServer::db,
		&DiscoveryServer::db_wave,
		&DiscoveryServer::bossy_db_wave,
		&DiscoveryServer::db_updated,
	};
	if (static_cast<size_t>(type) >= sizeof(dispatch) / sizeof(dispatch[0])) {
		std::string errmsg("Unexpected message type ");
		errmsg += std::to_string(toUType(type));
		throw Xapian::InvalidArgumentError(errmsg);
	}
	(this->*(dispatch[static_cast<int>(type)]))(message);
}


void
DiscoveryServer::hello(const std::string& message)
{
	const char *p = message.data();
	const char *p_end = p + message.size();

	Node remote_node;
	if (remote_node.unserialise(&p, p_end) == -1) {
		L_DISCOVERY(this, "Badly formed message: No proper node!");
		return;
	}
	if (remote_node == local_node) {
		// It's me! ...wave hello!
		discovery->send_message(Discovery::Message::WAVE, local_node.serialise());
	} else {
		const Node *node = nullptr;
		if (manager()->touch_node(remote_node.name, remote_node.region.load(), &node)) {
			if (remote_node == *node) {
				discovery->send_message(Discovery::Message::WAVE, local_node.serialise());
			} else {
				discovery->send_message(Discovery::Message::SNEER, remote_node.serialise());
			}
		} else {
			discovery->send_message(Discovery::Message::WAVE, local_node.serialise());
		}
	}
}

void
DiscoveryServer::_wave(bool heartbeat, const std::string& message)
{
	const char *p = message.data();
	const char *p_end = p + message.size();

	Node remote_node;
	if (remote_node.unserialise(&p, p_end) == -1) {
		L_DISCOVERY(this, "Badly formed message: No proper node!");
		return;
	}

	int region;
	if (remote_node == local_node) {
		region = local_node.region.load();
	} else {
		region = remote_node.region.load();
	}

	auto m = manager();
	char inet_addr[INET_ADDRSTRLEN];

	const Node *node = nullptr;
	if (m->touch_node(remote_node.name, region, &node)) {
		if (remote_node != *node && remote_node.name != local_node.name) {
			if (heartbeat || node->touched < epoch::now<>() - HEARTBEAT_MAX) {
				m->drop_node(remote_node.name);
				L_INFO(this, "Stalled node %s left the party!", remote_node.name.c_str());
				if (m->put_node(remote_node)) {
					L_INFO(this, "Node %s joined the party on ip:%s, tcp:%d (http), tcp:%d (xapian) (2)!", remote_node.name.c_str(), inet_ntop(AF_INET, &remote_node.addr.sin_addr, inet_addr, sizeof(inet_addr)), remote_node.http_port, remote_node.binary_port);
					local_node.regions.store(-1);
					m->get_region();
				} else {
					L_ERR(this, "ERROR: Cannot register remote node (1): %s", remote_node.name.c_str());
				}
			}
		}
	} else {
		if (m->put_node(remote_node)) {
			L_INFO(this, "Node %s joined the party on ip:%s, tcp:%d (http), tcp:%d (xapian) (1)!", remote_node.name.c_str(), inet_ntop(AF_INET, &remote_node.addr.sin_addr, inet_addr, sizeof(inet_addr)), remote_node.http_port, remote_node.binary_port);
			local_node.regions.store(-1);
			m->get_region();
		} else {
			L_ERR(this, "ERROR: Cannot register remote node (2): %s", remote_node.name.c_str());
		}
	}
}

void
DiscoveryServer::wave(const std::string& message)
{
	_wave(false, message);
}

void
DiscoveryServer::sneer(const std::string& message)
{
	auto m = manager();

	if (m->state != XapiandManager::State::READY) {
		return;
	}

	const char *p = message.data();
	const char *p_end = p + message.size();

	Node remote_node;
	if (remote_node.unserialise(&p, p_end) == -1) {
		L_DISCOVERY(this, "Badly formed message: No proper node!");
		return;
	}
	if (remote_node == local_node) {
		if (m->node_name.empty()) {
			L_DISCOVERY(this, "Node name %s already taken. Retrying other name...", local_node.name.c_str());
			m->reset_state();
		} else {
			L_ERR(this, "Cannot join the party. Node name %s already taken!", local_node.name.c_str());
			m->state = XapiandManager::State::BAD;
			local_node.name.clear();
			m->shutdown_asap = epoch::now<>();
			m->async_shutdown.send();
		}
	}
}

void
DiscoveryServer::heartbeat(const std::string& message)
{
	_wave(true, message);
}

void
DiscoveryServer::bye(const std::string& message)
{
	auto m = manager();

	if (m->state != XapiandManager::State::READY) {
		return;
	}

	const char *p = message.data();
	const char *p_end = p + message.size();

	Node remote_node;
	if (remote_node.unserialise(&p, p_end) == -1) {
		L_DISCOVERY(this, "Badly formed message: No proper node!");
		return;
	}
	m->drop_node(remote_node.name);
	L_INFO(this, "Node %s left the party!", remote_node.name.c_str());
	local_node.regions.store(-1);
	m->get_region();
}

void
DiscoveryServer::db(const std::string& message)
{
	auto m = manager();

	if (m->state != XapiandManager::State::READY) {
		return;
	}

	const char *p = message.data();
	const char *p_end = p + message.size();

	std::string index_path;
	if (unserialise_string(index_path, &p, p_end) == -1) {
		L_DISCOVERY(this, "Badly formed message: No index path!");
		return;
	}

	long long mastery_level = m->database_pool.get_mastery_level(index_path);

	if (m->get_region() == m->get_region(index_path) /* FIXME: missing leader check */) {
		const Node *node = nullptr;
		if (m->endp_r.get_master_node(index_path, &node, m)) {
			discovery->send_message(
				Discovery::Message::BOSSY_DB_WAVE,
				serialise_string(std::to_string(mastery_level)) +  // The mastery level of the database
				serialise_string(index_path) +  // The path of the index
				node->serialise()				// The node where the index master is at
			);
			return;
		}
	}

	if (mastery_level != -1) {
			L_DISCOVERY(this, "Found local database '%s' with m:%llx!", index_path.c_str(), mastery_level);
			discovery->send_message(
				Discovery::Message::DB_WAVE,
				serialise_string(std::to_string(mastery_level)) +  // The mastery level of the database
				serialise_string(index_path) +  // The path of the index
				local_node.serialise()  // The node where the index is at
			);
	}
}

void
DiscoveryServer::_db_wave(bool bossy, const std::string& message)
{
	auto m = manager();

	if (m->state != XapiandManager::State::READY) {
		return;
	}

	char inet_addr[INET_ADDRSTRLEN];

	const char *p = message.data();
	const char *p_end = p + message.size();

	std::string mastery_str;
	if (unserialise_string(mastery_str, &p, p_end) == -1) {
		L_DISCOVERY(this, "Badly formed message: No proper mastery!");
		return;
	}
	long long remote_mastery_level = std::stoll(mastery_str);

	std::string index_path;
	if (unserialise_string(index_path, &p, p_end) == -1) {
		L_DISCOVERY(this, "Badly formed message: No index path!");
		return;
	}

	Node remote_node;
	if (remote_node.unserialise(&p, p_end) == -1) {
		L_DISCOVERY(this, "Badly formed message: No proper node!");
		return;
	}
	if (m->put_node(remote_node)) {
		L_INFO(this, "Node %s joined the party on ip:%s, tcp:%d (http), tcp:%d (xapian)! (3)", remote_node.name.c_str(), inet_ntop(AF_INET, &remote_node.addr.sin_addr, inet_addr, sizeof(inet_addr)), remote_node.http_port, remote_node.binary_port);
	}

	L_DISCOVERY(this, "Node %s has '%s' with a mastery of %llx!", remote_node.name.c_str(), index_path.c_str(), remote_mastery_level);

	if (m->get_region() == m->get_region(index_path)) {
		L_DEBUG(this, "The DB is in the same region that this cluster!");
		Endpoint index(index_path, &remote_node, remote_mastery_level, remote_node.name);
		m->endp_r.add_index_endpoint(index, true, bossy);
	} else if (m->endp_r.exists(index_path)) {
		L_DEBUG(this, "The DB is in the LRU of this node!");
		Endpoint index(index_path, &remote_node, remote_mastery_level, remote_node.name);
		m->endp_r.add_index_endpoint(index, false, bossy);
	}
}

void
DiscoveryServer::db_wave(const std::string& message)
{
	_db_wave(false, message);
}

void
DiscoveryServer::bossy_db_wave(const std::string& message)
{
	_db_wave(true, message);
}

void
DiscoveryServer::db_updated(const std::string& message)
{
	auto m = manager();

	if (m->state != XapiandManager::State::READY) {
		return;
	}

	char inet_addr[INET_ADDRSTRLEN];

	const char *p = message.data();
	const char *p_end = p + message.size();

	std::string mastery_str;
	if (unserialise_string(mastery_str, &p, p_end) == -1) {
		L_DISCOVERY(this, "Badly formed message: No proper mastery!");
		return;
	}
	long long remote_mastery_level = std::stoll(mastery_str);

	std::string index_path;
	if (unserialise_string(index_path, &p, p_end) == -1) {
		L_DISCOVERY(this, "Badly formed message: No index path!");
		return;
	}

	long long mastery_level = m->database_pool.get_mastery_level(index_path);
	if (mastery_level == -1) {
		return;
	}

	if (mastery_level > remote_mastery_level) {
		L_DISCOVERY(this, "Mastery of remote's %s wins! (local:%llx > remote:%llx) - Updating!", index_path.c_str(), mastery_level, remote_mastery_level);
		Node remote_node;
		if (remote_node.unserialise(&p, p_end) == -1) {
			L_DISCOVERY(this, "Badly formed message: No proper node!");
			return;
		}
		if (m->put_node(remote_node)) {
			L_INFO(this, "Node %s joined the party on ip:%s, tcp:%d (http), tcp:%d (xapian)! (4)", remote_node.name.c_str(), inet_ntop(AF_INET, &remote_node.addr.sin_addr, inet_addr, sizeof(inet_addr)), remote_node.http_port, remote_node.binary_port);
		}

		Endpoint local_endpoint(index_path);
		Endpoint remote_endpoint(index_path, &remote_node);
#ifdef XAPIAND_CLUSTERING
		// Replicate database from the other node
		L_INFO(this, "Request syncing database from %s...", remote_node.name.c_str());
		auto ret = m->trigger_replication(remote_endpoint, local_endpoint);
		if (ret.get()) {
			L_INFO(this, "Replication triggered!");
		}
#endif
	} else if (mastery_level != remote_mastery_level) {
		L_DISCOVERY(this, "Mastery of local's %s wins! (local:%llx <= remote:%llx) - Ignoring update!", index_path.c_str(), mastery_level, remote_mastery_level);
	}
}


void
DiscoveryServer::io_accept_cb(ev::io &watcher, int revents)
{
	L_EV_BEGIN(this, "DiscoveryServer::io_accept_cb:BEGIN");
	if (EV_ERROR & revents) {
		L_EV(this, "ERROR: got invalid discovery event (sock=%d): %s", discovery->sock, strerror(errno));
		return;
	}

	assert(discovery->sock == watcher.fd || discovery->sock == -1);

	if (revents & EV_READ) {
		try {
			std::string message;
			Discovery::Message type = static_cast<Discovery::Message>(discovery->get_message(message, static_cast<char>(Discovery::Message::MAX)));
			if (type != Discovery::Message::HEARTBEAT) {
				L_DISCOVERY(this, ">> get_message(%s)", Discovery::MessageNames[static_cast<int>(type)]);
			}
			L_DISCOVERY_PROTO(this, "message: '%s'", repr(message).c_str());
			discovery_server(type, message);
		} catch (...) {
			L_EV_END(this, "DiscoveryServer::io_accept_cb:END %lld", now);
			throw;
		}
	}

	L_EV_END(this, "DiscoveryServer::io_accept_cb:END %lld", now);
}

#endif
