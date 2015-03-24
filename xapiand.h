/*
 * Copyright deipi.com LLC and contributors. All rights reserved.
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

#include "config.h"

#ifndef XAPIAND_INCLUDED_XAPIAND_H
#define XAPIAND_INCLUDED_XAPIAND_H

#define XAPIAND_HTTP_SERVERPORT   8880    /* HTTP TCP port */
#define XAPIAND_BINARY_SERVERPORT 8890    /* Binary TCP port */

#if !defined(_WIN32) && \
	!defined(__linux__) && \
	(defined(__unix__) || (defined(__APPLE__) && defined(__MACH__)))
#define XAPIAND_TCP_BACKLOG       -1      /* TCP listen backlog */
#else
#define XAPIAND_TCP_BACKLOG       511     /* TCP listen backlog */
#endif

#if defined(__APPLE__)
#define HAVE_PTHREAD_SETNAME_NP_1 1
#else
#define HAVE_PTHREAD_SETNAME_NP_2 1
#endif

#endif /* XAPIAND_INCLUDED_XAPIAND_H */
