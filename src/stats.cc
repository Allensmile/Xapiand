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

#include "stats.h"

Stats::Pos::Pos()
	: minute(0),
	  second(0)
{
}


void
Stats::Counter::Element::clear()
{
	cnt = 0;
	total = 0;
}


void
Stats::Counter::Element::merge(const Element& other)
{
	cnt += other.cnt;
	total += other.total;
}


void
Stats::Counter::Element::add(uint64_t duration)
{
	++cnt;
	total += duration;
}


Stats::Counter::Counter()
{
	clear_stats_min(0, SLOT_TIME_MINUTE - 1);
	clear_stats_sec(0, SLOT_TIME_SECOND - 1);
}


void
Stats::Counter::clear_stats_min(uint16_t start, uint16_t end)
{
	for (auto i = start; i <= end; ++i) {
		min[i].clear();
	}
}


void
Stats::Counter::clear_stats_sec(uint8_t start, uint8_t end)
{
	for (auto i = start; i <= end; ++i) {
		sec[i].clear();
	}
}


void
Stats::Counter::merge_stats_min(uint16_t start, uint16_t end, Stats::Counter::Element& element)
{
	for (auto i = start; i <= end; ++i) {
		element.merge(min[i]);
	}
}


void
Stats::Counter::merge_stats_sec(uint8_t start, uint8_t end, Element& element)
{
	for (auto i = start; i <= end; ++i) {
		element.merge(sec[i]);
	}
}


Stats::Pos::Pos(std::chrono::time_point<std::chrono::system_clock> current)
{
	time_t epoch = std::chrono::system_clock::to_time_t(current);
	struct tm *timeinfo = localtime(&epoch);
	timeinfo->tm_hour   = 0;
	timeinfo->tm_min    = 0;
	timeinfo->tm_sec    = 0;
	auto delta = epoch - mktime(timeinfo);
	minute = delta / SLOT_TIME_SECOND;
	second =  delta % SLOT_TIME_SECOND;
}


Stats&
Stats::cnt()
{
	static Stats stats_cnt;
	return stats_cnt;
}


Stats::Stats()
	: current(std::chrono::system_clock::now()),
	  current_pos(current)
{
}


Stats::Stats(Stats& other)
	: current(std::chrono::system_clock::now())
{
	std::lock_guard<std::mutex> lk(other.mtx);
	current = other.current;
	current_pos = other.current_pos;
	counters = other.counters;
}


void
Stats::update_pos_time()
{
	auto b_time_second = current_pos.second;
	auto b_time_minute = current_pos.minute;

	auto now = std::chrono::system_clock::now();
	auto t_elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - current).count();

	if (t_elapsed >= SLOT_TIME_SECOND) {
		clear_stats_sec(0, SLOT_TIME_SECOND - 1);
		current_pos.minute += t_elapsed / SLOT_TIME_SECOND;
		current_pos.second = t_elapsed % SLOT_TIME_SECOND;
	} else {
		current_pos.second += t_elapsed;
		if (current_pos.second >= SLOT_TIME_SECOND) {
			clear_stats_sec(b_time_second + 1, SLOT_TIME_SECOND - 1);
			clear_stats_sec(0, current_pos.second % SLOT_TIME_SECOND);
			current_pos.minute += current_pos.second / SLOT_TIME_SECOND;
			current_pos.second = t_elapsed % SLOT_TIME_SECOND;
		} else {
			clear_stats_sec(b_time_second + 1, current_pos.second);
		}
	}

	current = now;

	if (current_pos.minute >= SLOT_TIME_MINUTE) {
		clear_stats_min(b_time_minute + 1, SLOT_TIME_MINUTE - 1);
		clear_stats_min(0, current_pos.minute % SLOT_TIME_MINUTE);
		current_pos.minute = current_pos.minute % SLOT_TIME_MINUTE;
	} else {
		clear_stats_min(b_time_minute + 1, current_pos.minute);
	}

	ASSERT(current_pos.second < SLOT_TIME_SECOND);
	ASSERT(current_pos.minute < SLOT_TIME_MINUTE);
}


void
Stats::clear_stats_min(uint16_t start, uint16_t end)
{
	for (auto& counter : counters) {
		counter.second.clear_stats_min(start, end);
	}
}


void
Stats::clear_stats_sec(uint8_t start, uint8_t end)
{
	for (auto& counter : counters) {
		counter.second.clear_stats_sec(start, end);
	}
}


void
Stats::merge_stats_min(uint16_t start, uint16_t end, std::unordered_map<std::string, Stats::Counter::Element>& cnt)
{
	for (auto& counter : counters) {
		auto& element = cnt[counter.first];
		counter.second.merge_stats_min(start, end, element);
	}
}


void
Stats::merge_stats_sec(uint8_t start, uint8_t end, std::unordered_map<std::string, Stats::Counter::Element>& cnt)
{
	for (auto& counter : counters) {
		auto& element = cnt[counter.first];
		counter.second.merge_stats_sec(start, end, element);
	}
}


void
Stats::add(Counter& counter, uint64_t duration)
{
	std::lock_guard<std::mutex> lk(mtx);
	update_pos_time();
	counter.min[current_pos.minute].add(duration);
	counter.sec[current_pos.second].add(duration);
}


void
Stats::add(const std::string& counter, uint64_t duration)
{
	auto& stats_cnt = cnt();
	stats_cnt.add(stats_cnt.counters[counter], duration);
}
