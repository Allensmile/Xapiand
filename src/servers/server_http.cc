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

#include "server_http.h"

#include "../client_http.h"
#include "http.h"


HttpServer::HttpServer(const std::shared_ptr<XapiandServer>& server_, ev::loop_ref* ev_loop_, unsigned int ev_flags_, const std::shared_ptr<Http>& http_)
	: BaseServer(server_, ev_loop_, ev_flags_),
	  http(http_)
{
	io.start(http->sock, ev::READ);
	L_EV(this, "Start http's server accept event (sock=%d)", http->sock);

	L_OBJ(this, "CREATED HTTP SERVER!");
}


HttpServer::~HttpServer()
{
	L_OBJ(this, "DELETED HTTP SERVER!");
}


void
HttpServer::io_accept_cb(ev::io& watcher, int revents)
{
	int fd = watcher.fd;
	int sock = http->sock;

	L_EV_BEGIN(this, "HttpServer::io_accept_cb:BEGIN");
	if (EV_ERROR & revents) {
		L_EV(this, "ERROR: got invalid http event (sock=%d, fd=%d): %s", sock, fd, strerror(errno));
		L_EV_END(this, "HttpServer::io_accept_cb:END");
		return;
	}

	assert(sock == fd || sock == -1);

	int client_sock;
	if ((client_sock = http->accept()) < 0) {
		if (!ignored_errorno(errno, true, false)) {
			L_ERR(this, "ERROR: accept http error (sock=%d, fd=%d): %s", sock, fd, strerror(errno));
		}
	} else {
		Worker::make_shared<HttpClient>(share_this<HttpServer>(), ev_loop, ev_flags, client_sock);
	}
	L_EV_END(this, "HttpServer::io_accept_cb:END");
}
