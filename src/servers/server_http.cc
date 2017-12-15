/*
 * Copyright (C) 2015-2017 deipi.com LLC and contributors. All rights reserved.
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

#include "server_http.h"

#include <string.h>               // for strerror
#include <sys/errno.h>            // for __error, errno
#include <chrono>                 // for operator""ms
#include <ratio>                  // for ratio

#include "./server.h"             // for XapiandServer
#include "./server_base.h"        // for BaseServer
#include "client_http.h"          // for HttpClient
#include "ev/ev++.h"              // for io, ::READ, loop_ref (ptr only)
#include "http.h"                 // for Http
#include "log.h"                  // for L_EV, L_OBJ, L_CALL, L_ERR
#include "utils.h"                // for ignored_errorno, readable_revents
#include "worker.h"               // for Worker


HttpServer::HttpServer(const std::shared_ptr<XapiandServer>& server_, ev::loop_ref* ev_loop_, unsigned int ev_flags_, const std::shared_ptr<Http>& http_)
	: BaseServer(server_, ev_loop_, ev_flags_),
	  http(http_)
{
	io.start(http->sock, ev::READ);
	L_EV("Start http's server accept event (sock=%d)", http->sock);

	L_OBJ("CREATED HTTP SERVER!");
}


HttpServer::~HttpServer()
{
	L_OBJ("DELETED HTTP SERVER!");
}


void
HttpServer::io_accept_cb(ev::io& watcher, int revents)
{
	int fd = watcher.fd;

	L_CALL("HttpServer::io_accept_cb(<watcher>, 0x%x (%s)) {fd:%d}", revents, readable_revents(revents).c_str(), fd);
	L_INFO_HOOK_LOG("HttpServer::io_accept_cb", this, "HttpServer::io_accept_cb(<watcher>, 0x%x (%s)) {fd:%d}", revents, readable_revents(revents).c_str(), fd);

	if (EV_ERROR & revents) {
		L_EV("ERROR: got invalid http event {fd:%d}: %s", fd, strerror(errno));
		return;
	}

	assert(http->sock == fd || http->sock == -1);

	L_EV_BEGIN("HttpServer::io_accept_cb:BEGIN");

	int client_sock;
	if ((client_sock = http->accept()) < 0) {
		if (!ignored_errorno(errno, true, false)) {
			L_ERR("ERROR: accept http error {fd:%d}: %s", fd, strerror(errno));
		}
	} else {
		Worker::make_shared<HttpClient>(share_this<HttpServer>(), ev_loop, ev_flags, client_sock);
	}
	L_EV_END("HttpServer::io_accept_cb:END");
}
