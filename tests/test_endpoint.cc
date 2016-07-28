/*
 * Copyright (C) 2015,2016 deipi.com LLC and contributors. All rights reserved.
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

#include "test_endpoint.h"


int test_endpoint() {

	std::string uri_test[][3] = {
		{"/var/db/xapiand/", "/", "/"},
		{"/var/db/xapiand/", "/home/user/something/", "/home/user/something"},
		{"/var/db/xapiand/", "home/////user///something/" , "home/user/something"},
		{"/", "/////home/user/something/", "home/user/something"},
		{"/var/db/xapiand/", "/////home/user/something/", "/home/user/something"},
		{"/var/db/xapiand/", "/home/user/something////////", "/home/user/something"},
		{"/var/db/xapiand/", "xapiand://home/user/something/", "/user/something"},
		{"/var/db/xapiand/", "xapiand://home////////user/something/", "/user/something"},
		{"/var/db/xapiand/", "://home/user/something/", "home/user/something"},
		{"/var/db/xapiand/", ":///home/user/something/", "/home/user/something"},
		{"/var/db/xapiand/", "file://home/user/something/", "home/user/something"}
	};

	int count = 0;
	for (size_t i = 0; i < arraySize(uri_test); ++i) {
		Endpoint::cwd = uri_test[i][0];
		Endpoint e(uri_test[i][1]);
		if (e.path != uri_test[i][2]) {
			++count;
			L_ERR(nullptr, "ERROR: Endpoint missmatch. Result: %s  Expected: %s\n", e.path.c_str(), uri_test[i][2].c_str());
		}
	}

	RETURN(count);
}
