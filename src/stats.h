/*
 * Copyright (C) 2017 deipi.com LLC and contributors. All rights reserved.
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

#include <chrono>        // for system_clock, time_point, duration_cast, seconds
#include <mutex>         // for mutex
#include <string>        // for string
#include <unordered_map> // for unordered_map
#include <vector>        // for vector


constexpr unsigned SLOT_TIME_MINUTE = 1440;
constexpr unsigned SLOT_TIME_SECOND = 60;


struct Stats {
	struct Counter {
		struct Element {
			uint32_t cnt;
			uint64_t total;
			uint64_t max;
			uint64_t min;

			Element();
			Element(uint64_t duration);
			void clear();
			void add(const Element& other);
		};
		Element min[SLOT_TIME_MINUTE];
		Element sec[SLOT_TIME_SECOND];

		Counter();
		void clear_stats_min(unsigned start, unsigned end);
		void clear_stats_sec(unsigned start, unsigned end);
		void add_stats_min(unsigned start, unsigned end, Element& element);
		void add_stats_sec(unsigned start, unsigned end, Element& element);
	};

	struct Pos {
		unsigned minute;
		unsigned second;

		Pos();
		Pos(const std::chrono::time_point<std::chrono::system_clock>& current);
	};

	std::chrono::time_point<std::chrono::system_clock> current;
	Pos current_pos;

	std::mutex mtx;

	std::unordered_map<std::string, Counter> counters;

	static Stats& cnt();

	Stats();
	Stats(Stats& other);

	void update_pos_time();

	void clear_stats_min(unsigned start, unsigned end);
	void clear_stats_sec(unsigned start, unsigned end);
	void add_stats_min(unsigned start, unsigned end, std::unordered_map<std::string, Counter::Element>& cnt);
	void add_stats_sec(unsigned start, unsigned end, std::unordered_map<std::string, Counter::Element>& cnt);

	void add(Counter& counter, uint64_t duration);
	static void add(const std::string& counter, uint64_t duration);
};
