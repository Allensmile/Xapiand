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

#include "test_lru.h"

#include "../src/lru.h"
#include "../src/log.h"
#include "utils.h"

#include <iostream>


using namespace lru;


int test_lru() {
	LRU<std::string, int> lru(3);
	lru.insert(std::make_pair("test1", 111));
	lru.insert(std::make_pair("test2", 222));
	lru.insert(std::make_pair("test3", 333));
	lru.insert(std::make_pair("test4", 444));  // this pushes 'test1' out of the lru

	try {
		if (lru.at("test1")) {
			L_ERR(nullptr, "ERROR: LRU::insert with limit is not working");
			RETURN (1);
		}
	} catch (const std::range_error&) { }

	if (lru.at("test4") != 444 || lru.at("test3") != 333 || lru.at("test2") != 222) {
		L_ERR(nullptr, "ERROR: LRU::at is not working");
		RETURN (1);
	}

	lru.insert(std::make_pair("test5", 555));  // this pushes 'test4' out of the lru

	try {
		if (lru.at("test4")) {
			L_ERR(nullptr, "ERROR: LRU::insert with limit is not working");
			RETURN (1);
		}
	} catch (const std::range_error&) { }

	if (lru.at("test2") != 222 || lru.at("test3") != 333 || lru.at("test5") != 555) {
		L_ERR(nullptr, "ERROR: LRU::at is not working");
		RETURN (1);
	}

	L(nullptr, "Test LRU is correct!");

	RETURN (0);
}


int test_lru_emplace() {
	LRU<std::string, int> lru(1);
	lru.emplace("test1", 111);
	lru.emplace_and([](int&){ return DropAction::leave; }, "test2", 222);

	if (lru.at("test1") != 111 || lru.at("test2") != 222) {
		L_ERR(nullptr, "ERROR: LRU emplace is not working");
		RETURN(1);
	}

	L(nullptr, "Test LRU emplace is correct!");

	RETURN(0);
}


int test_lru_actions() {
	try {
		LRU<std::string, int> lru(3);
		lru.insert(std::make_pair("test1", 111));
		lru.insert(std::make_pair("test2", 222));
		lru.insert(std::make_pair("test3", 333));
		lru.insert_and([](int&){ return DropAction::leave; }, std::make_pair("test4", 444));  // this DOES NOT push 'test1' out of the lru

		if (lru.size() != 4) {
			L_ERR(nullptr, "ERROR: LRU::insert_and is not working");
			return 1;
		}

		// this gets, but doesn't renew 'test1'
		if (lru.at_and([](int&){ return GetAction::leave; }, "test1") != 111) {
			L_ERR(nullptr, "ERROR: LRU::at_and is not working");
			return 1;
		}

		lru.insert(std::make_pair("test5", 555));  // this pushes 'test1' *and* 'test2' out of the lru

		try {
			if (lru.at("test1")) {
				L_ERR(nullptr, "ERROR: LRU::insert with limit is not working");
				RETURN (1);
			}
		} catch (std::range_error) { }

		if (lru.size() != 3) {
			L_ERR(nullptr, "ERROR: LRU::insert with limit is not working");
			RETURN (1);
		}

		lru.insert_and([](int&){ return DropAction::renew; }, std::make_pair("test6", 666));  // this renews 'test3'

		if (lru.size() != 4) {
			L_ERR(nullptr, "ERROR: LRU::insert_and  is not working");
			RETURN(1);
		}

		if (lru.at("test3") != 333 || lru.at("test4") != 444 || lru.at("test5") != 555 || lru.at("test6") != 666) {
			L_ERR(nullptr, "ERROR: LRU insert is not working");
			RETURN(1);
		}
    } catch (const std::exception& exc) {
    	L_EXC(nullptr, "%s\n", exc.what());
    }

	L(nullptr, "Test LRU with actions is correct!");

	RETURN(0);
}


int test_lru_mutate() {
	LRU<std::string, int> lru(3);
	lru.insert(std::make_pair("test1", 111));
	if (lru.at_and([](int& o){ o = 123; return GetAction::leave; }, "test1") != 123 ||
		lru.get_and([](int& o){ o = 456; return GetAction::leave; }, "test1") != 456 ||
		lru.at("test1") != 456) {
		L_ERR(nullptr, "ERROR: LRU mutate is not working");
		RETURN(1);
	}

	L(nullptr, "Test LRU mutate is correct!");

	RETURN(0);
}
