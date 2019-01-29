/*
 * Copyright (c) 2015-2018 Dubalu LLC
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

#include "replication.h"

#ifdef XAPIAND_CLUSTERING

#include <netinet/tcp.h>                      // for TCP_NODELAY

#include "endpoint.h"                         // for Endpoint
#include "io.hh"                              // for io::*
#include "node.h"                             // for Node, local_node
#include "remote_protocol_client.h"           // for XAPIAN_REMOTE_PROTOCOL_MAJOR_VERSION, XAPIAN_REMOTE_PROTOCOL_MAINOR_VERSION
#include "replication_protocol_server.h"      // For ReplicationProtocolServer


// #undef L_DEBUG
// #define L_DEBUG L_GREY
// #undef L_CALL
// #define L_CALL L_STACKED_DIM_GREY


Replication::Replication(const std::shared_ptr<Worker>& parent_, ev::loop_ref* ev_loop_, unsigned int ev_flags_, const char* hostname, unsigned int serv, int tries)
	: BaseTCP(parent_, ev_loop_, ev_flags_, "Replication", TCP_TCP_NODELAY)
{
	bind(hostname, serv, tries);
}


void
Replication::start()
{
	L_CALL("Replication::start()");

	auto weak_children = gather_children();
	for (auto& weak_child : weak_children) {
		if (auto child = weak_child.lock()) {
			std::static_pointer_cast<ReplicationProtocolServer>(child)->start();
		}
	}
}


void
Replication::trigger_replication(const TriggerReplicationArgs& args)
{
	L_CALL("Replication::trigger_replication(%s, %s, %s)", repr(src_endpoint.to_string()), repr(dst_endpoint.to_string()), cluster_database ? "true" : "false");

	trigger_replication_args.enqueue(args);

	auto weak_children = gather_children();
	for (auto& weak_child : weak_children) {
		if (auto child = weak_child.lock()) {
			std::static_pointer_cast<ReplicationProtocolServer>(child)->trigger_replication();
		}
	}
}


std::string
Replication::__repr__() const
{
	return string::format("<Replication {cnt:%ld}%s%s%s>",
		use_count(),
		is_runner() ? " (runner)" : " (worker)",
		is_running_loop() ? " (running loop)" : " (stopped loop)",
		is_detaching() ? " (deteaching)" : "");
}


std::string
Replication::getDescription() const
{
	return string::format("TCP %s:%d (%s v%d.%d)", addr.sin_addr.s_addr ? inet_ntop(addr) : "", ntohs(addr.sin_port), description, XAPIAND_REPLICATION_PROTOCOL_MAJOR_VERSION, XAPIAND_REPLICATION_PROTOCOL_MINOR_VERSION);
}

#endif  /* XAPIAND_CLUSTERING */
