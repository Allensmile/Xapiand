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

#ifndef XAPIAND_INCLUDED_TEST_HTM_H
#define XAPIAND_INCLUDED_TEST_HTM_H

#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <unistd.h>
#include "../src/utils.h"
#include "../src/htm.h"


typedef struct test_transform_s {
	// Source CRS.
	int SRID;
	double lat_src;
	double lon_src;
	double h_src;
	// Target CRS.
	std::string res;
} test_transform_t;

typedef std::vector<test_transform_t> Vector_Transforms;

int test_cartesian_transforms();
int test_hullConvex();
int test_HTM_chull();
int test_HTM_circle();


#endif /* XAPIAND_INCLUDED_TEST_HTM_H */