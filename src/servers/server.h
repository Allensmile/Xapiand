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

#pragma once

#include "xapiand.h"

#include <stdio.h>              // for snprintf
#include <time.h>               // for time_t
#include <atomic>               // for atomic_int
#include <memory>               // for shared_ptr
#include <mutex>                // for mutex
#include <string>               // for string

#include "ev/ev++.h"            // for async, loop_ref (ptr only)
#include "worker.h"             // for Worker
#include "threadpool.h"         // for Task

class BaseServer;
class BinaryServer;
class DiscoveryServer;
class HttpServer;
class RaftServer;
class XapiandManager;


class XapiandServer : public Task<>, public Worker {
	friend Worker;
	friend XapiandManager;

	std::mutex qmtx;

	ev::async async_setup_node;

	XapiandServer(const std::shared_ptr<XapiandManager>& manager_, ev::loop_ref* ev_loop_, unsigned int ev_flags_);

	void destroyer();

	void destroy_impl() override;
	void shutdown_impl(time_t asap, time_t now) override;

	void async_setup_node_cb(ev::async& watcher, int revents);

public:
	std::string __repr__() const override {
		return Worker::__repr__("XapiandServer");
	}

	static std::mutex static_mutex;
	static std::atomic_int total_clients;
	static std::atomic_int http_clients;
	static std::atomic_int binary_clients;
	static std::atomic_int max_total_clients;
	static std::atomic_int max_http_clients;
	static std::atomic_int max_binary_clients;

	~XapiandServer();

	void run() override;
};
