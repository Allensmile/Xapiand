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

#include "replicator.h"

#ifdef XAPIAND_CLUSTERING

#include "servers/discovery.h"


XapiandReplicator::XapiandReplicator(const std::shared_ptr<XapiandManager>& manager_, ev::loop_ref *loop_)
	: Worker(std::move(manager_), loop_) {

	L_OBJ(this, "CREATED XAPIAN REPLICATOR!");
}

XapiandReplicator::~XapiandReplicator()
{
	L_OBJ(this, "DESTROYING XAPIAN REPLICATOR!");
	L_OBJ(this, "DESTROYED XAPIAN REPLICATOR!");
}

void
XapiandReplicator::on_commit(const Endpoint &endpoint)
{
	std::string endpoint_mastery(std::to_string(endpoint.mastery_level));
	if (auto discovery = manager()->proto_discovery.lock()) {
		discovery->send_message(
        	Discovery::Message::DB_UPDATED,
			serialise_string(endpoint_mastery) +  // The mastery level of the database
			serialise_string(endpoint.path) +  // The path of the index
			local_node.serialise()   // The node where the index is at
		);
	}
}


void
XapiandReplicator::run()
{
	// Function that retrieves a task from a queue, runs it and deletes it
	Endpoint endpoint;
	while (manager()->database_pool.updated_databases.pop(endpoint)) {
		L_DEBUG(this, "Replicator was informed database was updated: %s", endpoint.as_string().c_str());
		on_commit(endpoint);
	}
}


void
XapiandReplicator::shutdown(bool asap, bool now)
{
	L_OBJ(this , "SHUTDOWN XAPIAN REPLICATOR! (%d %d)", asap, now);

	Worker::shutdown(asap, now);

	manager()->database_pool.updated_databases.finish();
}

#endif
