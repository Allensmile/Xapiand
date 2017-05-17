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

#include "config.h"

#define XAPIAND_TAGLINE              "You Know, Also for Search"

#define XAPIAND_CLUSTER_NAME         "Xapiand"
#define XAPIAND_DISCOVERY_GROUP      "224.2.2.88"   /* Gossip group */
#define XAPIAND_DISCOVERY_SERVERPORT 58870          /* Gossip port */
#define XAPIAND_RAFT_GROUP           "224.2.2.89"   /* Gossip group */
#define XAPIAND_RAFT_SERVERPORT      58880          /* Gossip port */
#define XAPIAND_HTTP_SERVERPORT      8880           /* HTTP TCP port */
#define XAPIAND_BINARY_SERVERPORT    8890           /* Binary TCP port */
#ifndef XAPIAND_BINARY_PROXY
#define XAPIAND_BINARY_PROXY         XAPIAND_BINARY_SERVERPORT
#endif

#define XAPIAND_PID_FILE             "xapiand.pid"
#define XAPIAND_LOG_FILE             "xapiand.log"

#define DBPOOL_SIZE          250     /* Maximum number of database endpoints in database pool. */
#define NUM_REPLICATORS      10      /* Number of replicators. */
#define NUM_COMMITTERS       10      /* Number of threads handling the commits. */
#define NUM_FSYNCHERS        10      /* Number of threads handling the fsyncs. */
#define THEADPOOL_SIZE       100     /* Threadpool's size. */
#define SERVERS_MULTIPLIER   4       /* Server workers multiplier (by number of CPUs) */
#define ENDPOINT_LIST_SIZE   10      /* Endpoints List's size. */
#define SCRIPTS_CACHE_SIZE   100     /* Size of each script processor LRU. */

#define CONFIG_DEFAULT_MAX_CLIENTS 1000 /* Max number of clients. */

#if !defined(_WIN32) && \
	!defined(__linux__) && \
	(defined(__unix__) || (defined(__APPLE__) && defined(__MACH__)))
#define XAPIAND_TCP_BACKLOG       -1      /* TCP listen backlog */
#else
#define XAPIAND_TCP_BACKLOG       511     /* TCP listen backlog */
#endif


#ifdef HAVE___BUILTIN_EXPECT
#define likely(x) (__builtin_expect(!!(x), 1))
#define unlikely(x) (__builtin_expect(!!(x), 0))
#else
#define likely(x) (x)
#define unlikely(x) (x)
#endif


#include <assert.h>
#ifdef NDEBUG
#define ASSERT(args...)
#else
#define ASSERT(args...) assert(args)
#endif
