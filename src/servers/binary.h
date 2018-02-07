/*
 * Copyright (C) 2015-2018 dubalu.com LLC and contributors
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


#ifdef XAPIAND_CLUSTERING

#include <memory>            // for shared_ptr, weak_ptr
#include <string>            // for string
#include <vector>            // for vector

#include "tcp_base.h"        // for BaseTCP
#include "threadpool.h"      // for TaskQueue


class Endpoint;
class BinaryServer;


// Configuration data for Binary
class Binary : public BaseTCP {
	friend BinaryServer;

	std::mutex bsmtx;
	void signal_send_async();

	std::vector<std::weak_ptr<BinaryServer>> servers_weak;
	TaskQueue<const std::shared_ptr<BinaryServer>&> tasks;

public:
	std::string __repr__() const override {
		return Worker::__repr__("Binary");
	}

	Binary(const std::shared_ptr<XapiandManager>& manager_, ev::loop_ref* ev_loop_, unsigned int ev_flags_, int port_);
	~Binary();

	std::string getDescription() const noexcept override;

	int connection_socket();

	void add_server(const std::shared_ptr<BinaryServer>& server);

	std::shared_future<bool> trigger_replication(const Endpoint& src_endpoint, const Endpoint& dst_endpoint);
};


#endif  /* XAPIAND_CLUSTERING */
