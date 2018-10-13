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

#include "binary_server.h"

#ifdef XAPIAND_CLUSTERING

#include "binary.h"
#include "binary_client.h"
#include "endpoint.h"
#include "ignore_unused.h"


BinaryServer::BinaryServer(const std::shared_ptr<XapiandServer>& server_, ev::loop_ref* ev_loop_, unsigned int ev_flags_, const std::shared_ptr<Binary>& binary_)
	: BaseServer(server_, ev_loop_, ev_flags_),
	  binary(binary_),
	  signal_async(*ev_loop)
{
	io.start(binary->sock, ev::READ);
	L_EV("Start binary's server accept event (sock=%d)", binary->sock);

	signal_async.set<BinaryServer, &BinaryServer::signal_async_cb>(this);
	signal_async.start();
	L_EV("Start binary's async signal event");

	L_OBJ("CREATED BINARY SERVER!");
}


BinaryServer::~BinaryServer()
{
	L_OBJ("DELETED BINARY SERVER!");
}


void
BinaryServer::signal_async_cb(ev::async&, int revents)
{
	L_CALL("BinaryServer::signal_async_cb(<watcher>, 0x%x (%s))", revents, readable_revents(revents));

	ignore_unused(revents);

	L_EV_BEGIN("BinaryServer::signal_async_cb:BEGIN");

	while (binary->tasks.call(share_this<BinaryServer>())) {};

	L_EV_END("BinaryServer::signal_async_cb:END");
}


void
BinaryServer::io_accept_cb(ev::io& watcher, int revents)
{
	L_CALL("BinaryServer::io_accept_cb(<watcher>, 0x%x (%s)) {sock:%d, fd:%d}", revents, readable_revents(revents), binary->sock, watcher.fd);

	int fd = binary->sock;
	if (fd == -1) {
		return;
	}
	ignore_unused(watcher);
	assert(fd == watcher.fd || fd == -1);

	L_DEBUG_HOOK("BinaryServer::io_accept_cb", "BinaryServer::io_accept_cb(<watcher>, 0x%x (%s)) {fd:%d}", revents, readable_revents(revents), fd);

	if (EV_ERROR & revents) {
		L_EV("ERROR: got invalid binary event {fd:%d}: %s", fd, strerror(errno));
		return;
	}

	L_EV_BEGIN("BinaryServer::io_accept_cb:BEGIN");

	int client_sock = binary->accept();
	if (client_sock == -1) {
		if (!ignored_errorno(errno, true, false)) {
			L_ERR("ERROR: accept binary error {fd:%d}: %s", fd, strerror(errno));
		}
		L_EV_END("BinaryServer::io_accept_cb:END");
		return;
	}

	auto client = Worker::make_shared<BinaryClient>(share_this<BinaryServer>(), ev_loop, ev_flags, client_sock, active_timeout, idle_timeout);

	if (!client->init_remote()) {
		client->destroy();
		L_EV_END("BinaryServer::io_accept_cb:END");
		return;
	}

	L_INFO("Accepted new client! (client_sock=%d)", client_sock);

	L_EV_END("BinaryServer::io_accept_cb:END");
}


bool
BinaryServer::trigger_replication(const Endpoint& src_endpoint, const Endpoint& dst_endpoint)
{
	int client_sock = binary->connection_socket();
	if (client_sock < 0) {
		return false;
	}

	auto client = Worker::make_shared<BinaryClient>(share_this<BinaryServer>(), ev_loop, ev_flags, client_sock, active_timeout, idle_timeout);

	if (!client->init_replication(src_endpoint, dst_endpoint)) {
		client->destroy();
		return false;
	}

	L_INFO("Database being synchronized from %s...", repr(src_endpoint.to_string()));

	return true;
}

#endif /* XAPIAND_CLUSTERING */
