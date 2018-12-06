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

#include "discovery.h"

#ifdef XAPIAND_CLUSTERING

#include <errno.h>                          // for errno
#include <sysexits.h>                       // for EX_SOFTWARE

#include "cassert.h"                        // for ASSERT
#include "color_tools.hh"                   // for color
#include "cuuid/uuid.h"                     // for UUID
#include "epoch.hh"                         // for epoch::now
#include "error.hh"                         // for error:name, error::description
#include "ignore_unused.h"                  // for ignore_unused
#include "manager.h"                        // for XapiandManager::manager, XapiandManager::StateNames, XapiandManager::State
#include "namegen.h"                        // for name_generator
#include "node.h"                           // for Node, local_node
#include "opts.h"                           // for opts::*
#include "random.hh"                        // for random_int
#include "readable_revents.hh"              // for readable_revents
#include "repr.hh"                          // for repr
#include "utype.hh"                         // for toUType


// #undef L_DEBUG
// #define L_DEBUG L_GREY
// #undef L_CALL
// #define L_CALL L_STACKED_DIM_GREY
// #undef L_DISCOVERY
// #define L_DISCOVERY L_SALMON
// #undef L_EV_BEGIN
// #define L_EV_BEGIN L_DELAYED_200
// #undef L_EV_END
// #define L_EV_END L_DELAYED_N_UNLOG


Discovery::Discovery(const std::shared_ptr<Worker>& parent_, ev::loop_ref* ev_loop_, unsigned int ev_flags_, int port, const std::string& group)
	: UDP(port, "Discovery", XAPIAND_DISCOVERY_PROTOCOL_MAJOR_VERSION, XAPIAND_DISCOVERY_PROTOCOL_MINOR_VERSION, UDP_SO_REUSEPORT),
	  Worker(parent_, ev_loop_, ev_flags_),
	  io(*ev_loop),
	  discovery(*ev_loop),
	  db_update_send_async(*ev_loop)
{
	bind(1, group);
	io.set<Discovery, &Discovery::io_accept_cb>(this);
	discovery.set<Discovery, &Discovery::discovery_cb>(this);

	db_update_send_async.set<Discovery, &Discovery::db_update_send_async_cb>(this);
	db_update_send_async.start();
	L_EV("Start discovery's async db_update_send signal event");
}


Discovery::~Discovery()
{
	UDP::close();

	Worker::deinit();
}


void
Discovery::shutdown_impl(long long asap, long long now)
{
	L_CALL("Discovery::shutdown_impl(%lld, %lld)", asap, now);

	Worker::shutdown_impl(asap, now);

	if (asap) {
		stop(false);
		destroy(false);

		detach();
		if (is_runner()) {
			break_loop();
		}
	}
}


void
Discovery::destroy_impl()
{
	L_CALL("Discovery::destroy_impl()");

	Worker::destroy_impl();

	UDP::close();
}


void
Discovery::start_impl()
{
	L_CALL("Discovery::start_impl()");

	Worker::start_impl();

	discovery.start(0, WAITING_FAST);
	L_EV("Start discovery's discovery exploring event (%f)", discovery.repeat);

	io.start(sock, ev::READ);
	L_EV("Start discovery's server accept event {sock:%d}", sock);

	L_DISCOVERY("Discovery was started! (exploring)");
}


void
Discovery::stop_impl()
{
	L_CALL("Discovery::stop_impl()");

	Worker::stop_impl();

	auto local_node = Node::local_node();
	send_message(Message::BYE, local_node->serialise());
	L_INFO("Waving goodbye to cluster %s!", opts.cluster_name);

	discovery.stop();
	L_EV("Stop discovery's discovery event");

	io.stop();
	L_EV("Stop discovery's server accept event");

	L_DISCOVERY("Discovery was stopped!");
}


void
Discovery::operator()()
{
	L_CALL("Discovery::operator()()");

	L_EV("Starting discovery server loop...");
	run_loop();
	L_EV("Discovery server loop ended!");

	detach();
}


void
Discovery::send_message(Message type, const std::string& message)
{
	L_CALL("Discovery::send_message(%s, <message>)", MessageNames(type));

	L_DISCOVERY_PROTO("<< send_message (%s): %s", MessageNames(type), repr(message));
	UDP::send_message(toUType(type), message);
}


void
Discovery::io_accept_cb(ev::io &watcher, int revents)
{
	L_CALL("Discovery::io_accept_cb(<watcher>, 0x%x (%s)) {sock:%d}", revents, readable_revents(revents), watcher.fd);

	L_EV_BEGIN("Discovery::io_accept_cb:BEGIN {state:%s}", XapiandManager::StateNames(XapiandManager::manager->state));
	L_EV_END("Discovery::io_accept_cb:END {state:%s}", XapiandManager::StateNames(XapiandManager::manager->state));

	ignore_unused(watcher);
	ASSERT(sock == -1 || sock == watcher.fd);

	if (closed) {
		return;
	}

	L_DEBUG_HOOK("Discovery::io_accept_cb", "Discovery::io_accept_cb(<watcher>, 0x%x (%s)) {sock:%d}", revents, readable_revents(revents), watcher.fd);

	if (EV_ERROR & revents) {
		L_EV("ERROR: got invalid discovery event {sock:%d}: %s (%d): %s", watcher.fd, error::name(errno), errno, error::description(errno));
		return;
	}

	if (revents & EV_READ) {
		while (true) {
			try {
				std::string message;
				auto raw_type = get_message(message, static_cast<char>(Message::MAX));
				if (raw_type == '\xff') {
					break;  // no message
				}
				Message type = static_cast<Message>(raw_type);
				L_DISCOVERY_PROTO(">> get_message (%s): %s", MessageNames(type), repr(message));
				discovery_server(type, message);
			} catch (const BaseException& exc) {
				L_WARNING("WARNING: %s", *exc.get_context() ? exc.get_context() : "Unkown Exception!");
				break;
			}
		}
	}
}


void
Discovery::discovery_server(Message type, const std::string& message)
{
	L_CALL("Discovery::discovery_server(%s, <message>)", MessageNames(type));

	L_EV_BEGIN("Discovery::discovery_server:BEGIN {state:%s, type:%s}", XapiandManager::StateNames(XapiandManager::manager->state), MessageNames(type));
	L_EV_END("Discovery::discovery_server:END {state:%s, type:%s}", XapiandManager::StateNames(XapiandManager::manager->state), MessageNames(type));

	switch (type) {
		case Message::HELLO:
			hello(type, message);
			return;
		case Message::WAVE:
			wave(type, message);
			return;
		case Message::SNEER:
			sneer(type, message);
			return;
		case Message::ENTER:
			enter(type, message);
			return;
		case Message::BYE:
			bye(type, message);
			return;
		case Message::DB_UPDATED:
			db_updated(type, message);
			return;
		default: {
			std::string errmsg("Unexpected message type ");
			errmsg += std::to_string(toUType(type));
			THROW(InvalidArgumentError, errmsg);
		}
	}
}


void
Discovery::hello(Message type, const std::string& message)
{
	L_CALL("Discovery::hello(%s, <message>) {state:%s}", MessageNames(type), XapiandManager::StateNames(XapiandManager::manager->state.load()));
	ignore_unused(type);

	const char *p = message.data();
	const char *p_end = p + message.size();

	auto remote_node = std::make_shared<const Node>(Node::unserialise(&p, p_end));
	L_DISCOVERY(">> %s [from %s]", MessageNames(type), remote_node->name());

	auto local_node = Node::local_node();
	if (!Node::is_equal(remote_node, local_node)) {
		auto node = Node::touch_node(remote_node);
		if (node) {
			if (Node::is_equal(remote_node, node)) {
				send_message(Message::WAVE, local_node->serialise());
			} else {
				send_message(Message::SNEER, remote_node->serialise());
			}
		} else {
			send_message(Message::WAVE, local_node->serialise());
		}
	}
}


void
Discovery::wave(Message type, const std::string& message)
{
	L_CALL("Discovery::wave(%s, <message>) {state:%s}", MessageNames(type), XapiandManager::StateNames(XapiandManager::manager->state.load()));
	ignore_unused(type);

	const char *p = message.data();
	const char *p_end = p + message.size();

	auto remote_node = std::make_shared<const Node>(Node::unserialise(&p, p_end));
	L_DISCOVERY(">> %s [from %s]", MessageNames(type), remote_node->name());

	auto put = Node::put_node(remote_node);
	remote_node = put.first;
	if (put.second) {
		L_INFO("Node %s is at the party on ip:%s, tcp:%d (http), tcp:%d (xapian)!", remote_node->name(), remote_node->host(), remote_node->http_port, remote_node->binary_port);
	}

	// After receiving WAVE, flag as WAITING_MORE so it waits just a little longer
	// (prevent it from switching to slow waiting)
	auto waiting = XapiandManager::State::WAITING;
	if (XapiandManager::manager->state.compare_exchange_strong(waiting, XapiandManager::State::WAITING_MORE)) {
		// L_DEBUG("State changed: %s -> %s", XapiandManager::StateNames(state), XapiandManager::StateNames(XapiandManager::manager->state.load()));
	}
}


void
Discovery::sneer(Message type, const std::string& message)
{
	L_CALL("Discovery::sneer(%s, <message>) {state:%s}", MessageNames(type), XapiandManager::StateNames(XapiandManager::manager->state.load()));
	ignore_unused(type);

	if (XapiandManager::manager->state != XapiandManager::State::RESET &&
		XapiandManager::manager->state != XapiandManager::State::WAITING &&
		XapiandManager::manager->state != XapiandManager::State::WAITING_MORE &&
		XapiandManager::manager->state != XapiandManager::State::JOINING) {
		return;
	}

	const char *p = message.data();
	const char *p_end = p + message.size();

	Node remote_node = Node::unserialise(&p, p_end);
	L_DISCOVERY(">> %s [from %s]", MessageNames(type), remote_node.name());

	auto local_node = Node::local_node();
	if (remote_node == *local_node) {
		if (XapiandManager::manager->node_name.empty()) {
			L_DISCOVERY("Node name %s already taken. Retrying other name...", local_node->name());
			XapiandManager::manager->reset_state();
		} else {
			XapiandManager::manager->state.store(XapiandManager::State::BAD);
			Node::local_node(std::make_shared<const Node>());
			L_CRIT("Cannot join the party. Node name %s already taken!", local_node->name());
			sig_exit(-EX_SOFTWARE);
		}
	}
}


void
Discovery::enter(Message type, const std::string& message)
{
	L_CALL("Discovery::enter(%s, <message>) {state:%s}", MessageNames(type), XapiandManager::StateNames(XapiandManager::manager->state.load()));
	ignore_unused(type);

	const char *p = message.data();
	const char *p_end = p + message.size();

	auto remote_node = std::make_shared<const Node>(Node::unserialise(&p, p_end));
	L_DISCOVERY(">> %s [from %s]", MessageNames(type), remote_node->name());

	auto put = Node::put_node(remote_node);
	remote_node = put.first;
	if (put.second) {
		L_INFO("Node %s%s" + INFO_COL + " joined the party on ip:%s, tcp:%d (http), tcp:%d (xapian)!", remote_node->col().ansi(), remote_node->name(), remote_node->host(), remote_node->http_port, remote_node->binary_port);
	}
}


void
Discovery::bye(Message type, const std::string& message)
{
	L_CALL("Discovery::bye(%s, <message>) {state:%s}", MessageNames(type), XapiandManager::StateNames(XapiandManager::manager->state.load()));
	ignore_unused(type);

	if (XapiandManager::manager->state != XapiandManager::State::JOINING &&
		XapiandManager::manager->state != XapiandManager::State::SETUP &&
		XapiandManager::manager->state != XapiandManager::State::READY) {
		return;
	}

	const char *p = message.data();
	const char *p_end = p + message.size();

	Node remote_node = Node::unserialise(&p, p_end);
	L_DISCOVERY(">> %s [from %s]", MessageNames(type), remote_node.name());

	Node::drop_node(remote_node.name());

	auto leader_node = Node::leader_node();
	if (*leader_node == remote_node) {
		L_INFO("Leader node %s%s" + INFO_COL + " left the party!", remote_node.col().ansi(), remote_node.name());

		Node::leader_node(std::make_shared<const Node>());
		XapiandManager::manager->renew_leader();
	} else {
		L_INFO("Node %s%s" + INFO_COL + " left the party!", remote_node.col().ansi(), remote_node.name());
	}

	L_DEBUG("Nodes still active after %s left: %zu", remote_node.name(), Node::active_nodes());
}


void
Discovery::db_updated(Message type, const std::string& message)
{
	L_CALL("Discovery::db_updated(%s, <message>) {state:%s}", MessageNames(type), XapiandManager::StateNames(XapiandManager::manager->state.load()));
	ignore_unused(type);

	if (XapiandManager::manager->state != XapiandManager::State::READY) {
		return;
	}

	const char *p = message.data();
	const char *p_end = p + message.size();

	auto remote_node = std::make_shared<const Node>(Node::unserialise(&p, p_end));

	auto local_node = Node::local_node();
	if (Node::is_equal(remote_node, local_node)) {
		// It's just me, do nothing!
		return;
	}

	auto path = std::string(p, p_end);
	L_DISCOVERY(">> %s [from %s]: %s", MessageNames(type), remote_node->name(), repr(path));

	auto node = Node::touch_node(remote_node);
	if (node) {
		Endpoint local_endpoint(path);
		if (local_endpoint.empty()) {
			L_WARNING("Ignoring update for empty database path: %s!", repr(path));
		} else {
			// Replicate database from the other node
			Endpoint remote_endpoint(path, node.get());
			trigger_replication().delayed_debounce(std::chrono::milliseconds{random_int(0, 3000)}, local_endpoint.path, remote_endpoint, local_endpoint);
		}
	}
}


void
Discovery::discovery_cb(ev::timer&, int revents)
{
	auto state = XapiandManager::manager->state.load();

	L_CALL("Discovery::discovery_cb(<watcher>, 0x%x (%s)) {state:%s}", revents, readable_revents(revents), XapiandManager::StateNames(state));

	L_EV_BEGIN("Discovery::discovery_cb:BEGIN {state:%s}", XapiandManager::StateNames(state));
	L_EV_END("Discovery::discovery_cb:END {state:%s}", XapiandManager::StateNames(state));

	ignore_unused(revents);

	switch (state) {
		case XapiandManager::State::RESET: {
			auto local_node = Node::local_node();
			auto node_copy = std::make_unique<Node>(*local_node);
			std::string drop = node_copy->name();

			if (XapiandManager::manager->node_name.empty()) {
				node_copy->name(name_generator());
			} else {
				node_copy->name(XapiandManager::manager->node_name);
			}
			Node::local_node(std::shared_ptr<const Node>(node_copy.release()));

			if (!drop.empty()) {
				Node::drop_node(drop);
			}

			local_node = Node::local_node();
			auto reset = XapiandManager::State::RESET;
			if (XapiandManager::manager->state.compare_exchange_strong(reset, XapiandManager::State::WAITING)) {
				// L_DEBUG("State changed: %s -> %s", XapiandManager::StateNames(state), XapiandManager::StateNames(XapiandManager::manager->state.load()));
			}
			L_INFO("Advertising as %s%s" + INFO_COL + "...", local_node->col().ansi(), local_node->name());
			send_message(Message::HELLO, local_node->serialise());
			break;
		}
		case XapiandManager::State::WAITING: {
			// We're here because no one sneered nor entered during
			// WAITING_FAST, wait longer then...

			discovery.repeat = WAITING_SLOW;
			discovery.again();
			L_EV("Reset discovery's discovery event (%f)", discovery.repeat);

			auto waiting = XapiandManager::State::WAITING;
			if (XapiandManager::manager->state.compare_exchange_strong(waiting, XapiandManager::State::WAITING_MORE)) {
				// L_DEBUG("State changed: %s -> %s", XapiandManager::StateNames(state), XapiandManager::StateNames(XapiandManager::manager->state.load()));
			}
			break;
		}
		case XapiandManager::State::WAITING_MORE: {
			discovery.stop();
			L_EV("Stop discovery's discovery event");

			auto waiting_more = XapiandManager::State::WAITING_MORE;
			if (XapiandManager::manager->state.compare_exchange_strong(waiting_more, XapiandManager::State::JOINING)) {
				// L_DEBUG("State changed: %s -> %s", XapiandManager::StateNames(state), XapiandManager::StateNames(XapiandManager::manager->state.load()));
			}

			auto local_node = Node::local_node();
			send_message(Message::ENTER, local_node->serialise());

			XapiandManager::manager->join_cluster();
			break;
		}
		default: {
			break;
		}
	}
}


void
Discovery::db_update_send_async_cb(ev::async&, int revents)
{
	L_CALL("Discovery::db_update_send_async_cb(<watcher>, 0x%x (%s))", revents, readable_revents(revents));

	L_EV_BEGIN("Discovery::db_update_send_async_cb:BEGIN {state:%s}", XapiandManager::StateNames(XapiandManager::manager->state));
	L_EV_END("Discovery::db_update_send_async_cb:END {state:%s}", XapiandManager::StateNames(XapiandManager::manager->state));

	ignore_unused(revents);

	std::string path;
	while (db_update_send_args.try_dequeue(path)) {
		auto local_node = Node::local_node();
		send_message(Message::DB_UPDATED,
			local_node->serialise() +   // The node where the index is at
			path);  // The path of the index

		L_DEBUG("Sending database updated signal for %s", repr(path));
	}
}


void
Discovery::db_update_send(const std::string& path)
{
	L_CALL("Discovery::db_update_send(%s)", repr(path));

	db_update_send_args.enqueue(path);

	db_update_send_async.send();
}


std::string
Discovery::__repr__() const
{
	return string::format("<Discovery {cnt:%ld, sock:%d}%s%s%s>",
		use_count(),
		sock,
		is_runner() ? " (runner)" : " (worker)",
		is_running_loop() ? " (running loop)" : " (stopped loop)",
		is_detaching() ? " (deteaching)" : "");
}


std::string
Discovery::getDescription() const
{
	L_CALL("Discovery::getDescription()");

	return "UDP:" + std::to_string(port) + " (" + description + " v" + std::to_string(XAPIAND_DISCOVERY_PROTOCOL_MAJOR_VERSION) + "." + std::to_string(XAPIAND_DISCOVERY_PROTOCOL_MINOR_VERSION) + ")";
}


void
db_updater_send(std::string path)
{
	if (auto discovery = XapiandManager::manager->weak_discovery.lock()) {
		discovery->db_update_send(path);
	}
}

#endif
