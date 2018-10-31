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

#include "server.h"

#include <algorithm>             // for move
#include <chrono>                // for operator""ms
#include <type_traits>           // for remove_reference<>::type

#include "ignore_unused.h"       // for ignore_unused
#include "log.h"                 // for L_EV, L_OBJ, L_CALL, L_EV_BEGIN
#include "manager.h"             // for XapiandManager, XapiandManager::manager
#include "readable_revents.hh"   // for readable_revents


std::atomic_int XapiandServer::total_clients(0);
std::atomic_int XapiandServer::http_clients(0);
std::atomic_int XapiandServer::binary_clients(0);
std::atomic_int XapiandServer::max_total_clients(0);
std::atomic_int XapiandServer::max_http_clients(0);
std::atomic_int XapiandServer::max_binary_clients(0);


XapiandServer::XapiandServer(const std::shared_ptr<Worker>& parent_, ev::loop_ref* ev_loop_, unsigned int ev_flags_)
	: Worker(std::move(parent_), ev_loop_, ev_flags_)
{
	L_OBJ("CREATED XAPIAN SERVER!");
}


XapiandServer::~XapiandServer()
{
	L_OBJ("DELETED XAPIAN SERVER!");
}


void
XapiandServer::run()
{
	L_CALL("XapiandServer::run()");

	L_EV("Starting server loop...");
	run_loop();
	L_EV("Server loop ended!");

	detach();
}


void
XapiandServer::shutdown_impl(time_t asap, time_t now)
{
	L_CALL("XapiandServer::shutdown_impl(%d, %d)", (int)asap, (int)now);

	Worker::shutdown_impl(asap, now);

	destroy();

	if (now != 0) {
		detach();
		break_loop();
	}
}
