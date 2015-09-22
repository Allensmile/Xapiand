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

#include "server_base.h"


BaseServer::BaseServer(XapiandServer *server_, ev::loop_ref *loop_, int sock_, DatabasePool *database_pool_, ThreadPool *thread_pool_)
	: Worker(server_, loop_),
	  server(server_),
	  io(*loop),
	  sock(sock_),
	  database_pool(database_pool_),
	  thread_pool(thread_pool_)
{
	io.set<BaseServer, &BaseServer::io_accept>(this);
	io.start(sock, ev::READ);
}


BaseServer::~BaseServer()
{
	destroy();
}


void BaseServer::destroy()
{
	if (sock == -1) return;

	sock = -1;
	io.stop();
}