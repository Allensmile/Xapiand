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

#ifndef INCLUDED_TEST_SERIALISE_H
#define INCLUDED_TEST_SERIALISE_H

#include "../src/config.h"
#include "../src/htm.h"


typedef struct test {
	const char *str;
	const char *expect;
} test;

typedef struct test_str_double {
	const char *str;
	const double val;
} test_str_double;

typedef struct test_cartesian {
	const Cartesian cartesian;
	const char *expect_serialise;
	const char *expect_unserialise;
} test_cartesian;

typedef struct test_trixel_id {
	const uInt64 trixel_id;
	const char *expect_serialise;
	const uInt64 expect_unserialise;
} test_trixel_id;


// Testing the transformation between date string and timestamp.
int test_datetotimestamp();
// Testing unserialise date.
int test_unserialise_date();
// Testing serialise Cartesian.
int test_serialise_cartesian();
// Testing unserialise Cartesian.
int test_unserialise_cartesian();
// Testing serialise HTM trixel's id.
int test_serialise_trixel_id();
// Testing unserialise HTM trixel's id.
int test_unserialise_trixel_id();


#endif /* INCLUDED_TEST_SERIALISE_H */