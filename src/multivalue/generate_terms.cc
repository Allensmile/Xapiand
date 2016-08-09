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

#include "generate_terms.h"

#include "../database.h"
#include "../schema.h"

#include <algorithm>
#include <bitset>
#include <map>
#include <cstdint>


inline static bool isnotSubtrixel(std::string& last_valid, uint64_t id_trixel) {
	auto res = std::bitset<SIZE_BITS_ID>(id_trixel).to_string();
	res.assign(res.substr(res.find('1')));
	if (res.find(last_valid) == 0) {
		return false;
	} else {
		last_valid.assign(res);
		return true;
	}
}


inline static std::string transform_to_query_string(Datetime::tm_t& tm) {
	return to_query_string(Datetime::timegm(tm));
}


std::pair<std::string, std::vector<std::string>>
GenerateTerms::date(double start_, double end_, const std::vector<double>& accuracy, const std::vector<std::string>& acc_prefix)
{
	if (accuracy.empty() || end_ < start_) {
		return std::make_pair(std::string(), std::vector<std::string>());
	}

	std::string result_terms;
	std::vector<std::string> used_prefixes;
	used_prefixes.reserve(2);

	auto tm_s = Datetime::to_tm_t(start_);
	auto tm_e = Datetime::to_tm_t(end_);

	int diff = tm_e.year - tm_s.year, acc = -1;
	// Find the accuracy needed.
	if (diff) {
		if (diff >= 1000) {
			acc = toUType(unitTime::MILLENNIUM);
		} else if (diff >= 100) {
			acc = toUType(unitTime::CENTURY);
		} else if (diff >= 10) {
			acc = toUType(unitTime::DECADE);
		} else {
			acc = toUType(unitTime::YEAR);
		}
	} else if (tm_e.mon - tm_s.mon) {
		acc = toUType(unitTime::MONTH);
	} else if (tm_e.day - tm_s.day) {
		acc = toUType(unitTime::DAY);
	} else if (tm_e.hour - tm_s.hour) {
		acc = toUType(unitTime::HOUR);
	} else if (tm_e.min - tm_s.min) {
		acc = toUType(unitTime::MINUTE);
	} else {
		acc = toUType(unitTime::SECOND);
	}

	// Find the upper or equal accuracy.
	size_t pos = 0, len = accuracy.size();
	while (pos < len && accuracy[pos] <= acc) {
		++pos;
	}

	// If there is an upper accuracy.
	if (pos < len) {
		auto c_tm_s = tm_s;
		auto c_tm_e = tm_e;
		switch ((unitTime)accuracy[pos]) {
			case unitTime::MILLENNIUM:
				result_terms.assign(millennium(c_tm_s, c_tm_e, acc_prefix[pos]));
				break;
			case unitTime::CENTURY:
				result_terms.assign(century(c_tm_s, c_tm_e, acc_prefix[pos]));
				break;
			case unitTime::DECADE:
				result_terms.assign(decade(c_tm_s, c_tm_e, acc_prefix[pos]));
				break;
			case unitTime::YEAR:
				result_terms.assign(year(c_tm_s, c_tm_e, acc_prefix[pos]));
				break;
			case unitTime::MONTH:
				result_terms.assign(month(c_tm_s, c_tm_e, acc_prefix[pos]));
				break;
			case unitTime::DAY:
				result_terms.assign(day(c_tm_s, c_tm_e, acc_prefix[pos]));
				break;
			case unitTime::HOUR:
				result_terms.assign(hour(c_tm_s, c_tm_e, acc_prefix[pos]));
				break;
			case unitTime::MINUTE:
				result_terms.assign(minute(c_tm_s, c_tm_e, acc_prefix[pos]));
				break;
			case unitTime::SECOND:
				result_terms.assign(second(c_tm_s, c_tm_e, acc_prefix[pos]));
				break;
		}
		if (!result_terms.empty()) {
			used_prefixes.push_back(acc_prefix[pos]);
		}
	}

	// If there is the needed accuracy.
	if (pos > 0 && acc == accuracy[--pos]) {
		std::string lower_terms;
		switch ((unitTime)accuracy[pos]) {
			case unitTime::MILLENNIUM:
				lower_terms.assign(millennium(tm_s, tm_e, acc_prefix[pos]));
				break;
			case unitTime::CENTURY:
				lower_terms.assign(century(tm_s, tm_e, acc_prefix[pos]));
				break;
			case unitTime::DECADE:
				lower_terms.assign(decade(tm_s, tm_e, acc_prefix[pos]));
				break;
			case unitTime::YEAR:
				lower_terms.assign(year(tm_s, tm_e, acc_prefix[pos]));
				break;
			case unitTime::MONTH:
				lower_terms.assign(month(tm_s, tm_e, acc_prefix[pos]));
				break;
			case unitTime::DAY:
				lower_terms.assign(day(tm_s, tm_e, acc_prefix[pos]));
				break;
			case unitTime::HOUR:
				lower_terms.assign(hour(tm_s, tm_e, acc_prefix[pos]));
				break;
			case unitTime::MINUTE:
				lower_terms.assign(minute(tm_s, tm_e, acc_prefix[pos]));
				break;
			case unitTime::SECOND:
				lower_terms.assign(second(tm_s, tm_e, acc_prefix[pos]));
				break;
		}

		if (!lower_terms.empty()) {
			used_prefixes.push_back(acc_prefix[pos]);
			if (result_terms.empty()) {
				result_terms.assign(lower_terms);
			} else {
				result_terms.reserve(result_terms.length() + lower_terms.length() + 9);
				result_terms.insert(result_terms.begin(), '(');
				result_terms.append(") AND (").append(lower_terms).push_back(')');
			}
		}
	}

	return std::make_pair(std::move(result_terms), std::move(used_prefixes));
}


std::string
GenerateTerms::millennium(Datetime::tm_t& tm_s, Datetime::tm_t& tm_e, const std::string& prefix)
{
	std::string res, prefix_dot;
	prefix_dot.reserve(prefix.length() + 1);
	prefix_dot.assign(prefix).push_back(':');

	tm_s.sec = tm_s.min = tm_s.hour = tm_e.sec = tm_e.min = tm_e.hour = 0;
	tm_s.day = tm_s.mon = tm_e.day = tm_e.mon = 1;
	tm_s.year = year(tm_s.year, 1000);
	tm_e.year = year(tm_e.year, 1000);
	int num_unions = (tm_e.year - tm_s.year) / 1000;
	if (num_unions < MAX_TERMS) {
		// Reserve upper bound.
		res.reserve(get_upper_bound(prefix_dot.length(), num_unions, 4));
		while (tm_s.year != tm_e.year) {
			res.append(prefix_dot).append(transform_to_query_string(tm_s)).append(" OR ");
			tm_s.year += 1000;
		}
		res.append(prefix_dot).append(transform_to_query_string(tm_e));
	}

	return res;
}


std::string
GenerateTerms::century(Datetime::tm_t& tm_s, Datetime::tm_t& tm_e, const std::string& prefix)
{
	std::string res, prefix_dot;
	prefix_dot.reserve(prefix.length() + 1);
	prefix_dot.assign(prefix).push_back(':');

	tm_s.sec = tm_s.min = tm_s.hour = tm_e.sec = tm_e.min = tm_e.hour = 0;
	tm_s.day = tm_s.mon = tm_e.day = tm_e.mon = 1;
	tm_s.year = year(tm_s.year, 100);
	tm_e.year = year(tm_e.year, 100);
	int num_unions = (tm_e.year - tm_s.year) / 100;
	if (num_unions < MAX_TERMS) {
		// Reserve upper bound.
		res.reserve(get_upper_bound(prefix_dot.length(), num_unions, 4));
		while (tm_s.year != tm_e.year) {
			res.append(prefix_dot).append(transform_to_query_string(tm_s)).append(" OR ");
			tm_s.year += 100;
		}
		res.append(prefix_dot).append(transform_to_query_string(tm_e));
	}

	return res;
}


std::string
GenerateTerms::decade(Datetime::tm_t& tm_s, Datetime::tm_t& tm_e, const std::string& prefix)
{
	std::string res, prefix_dot;
	prefix_dot.reserve(prefix.length() + 1);
	prefix_dot.assign(prefix).push_back(':');

	tm_s.sec = tm_s.min = tm_s.hour = tm_e.sec = tm_e.min = tm_e.hour = 0;
	tm_s.day = tm_s.mon = tm_e.day = tm_e.mon = 1;
	tm_s.year = year(tm_s.year, 10);
	tm_e.year = year(tm_e.year, 10);
	int num_unions = (tm_e.year - tm_s.year) / 10;
	if (num_unions < MAX_TERMS) {
		// Reserve upper bound.
		res.reserve(get_upper_bound(prefix_dot.length(), num_unions, 4));
		while (tm_s.year != tm_e.year) {
			res.append(prefix_dot).append(transform_to_query_string(tm_s)).append(" OR ");
			tm_s.year += 10;
		}
		res.append(prefix_dot).append(transform_to_query_string(tm_e));
	}

	return res;
}


std::string
GenerateTerms::year(Datetime::tm_t& tm_s, Datetime::tm_t& tm_e, const std::string& prefix)
{
	std::string res, prefix_dot;
	prefix_dot.reserve(prefix.length() + 1);
	prefix_dot.assign(prefix).push_back(':');

	tm_s.sec = tm_s.min = tm_s.hour = tm_e.sec = tm_e.min = tm_e.hour = 0;
	tm_s.day = tm_s.mon = tm_e.day = tm_e.mon = 1;
	int num_unions = tm_e.year - tm_s.year;
	if (num_unions < MAX_TERMS) {
		// Reserve upper bound.
		res.reserve(get_upper_bound(prefix_dot.length(), num_unions, 4));
		while (tm_s.year != tm_e.year) {
			res.append(prefix_dot).append(transform_to_query_string(tm_s)).append(" OR ");
			++tm_s.year;
		}
		res.append(prefix_dot).append(transform_to_query_string(tm_e));
	}

	return res;
}


std::string
GenerateTerms::month(Datetime::tm_t& tm_s, Datetime::tm_t& tm_e, const std::string& prefix)
{
	std::string res, prefix_dot;
	prefix_dot.reserve(prefix.length() + 1);
	prefix_dot.assign(prefix).push_back(':');

	tm_s.sec = tm_s.min = tm_s.hour = tm_e.sec = tm_e.min = tm_e.hour = 0;
	tm_s.day = tm_e.day = 1;
	int num_unions = tm_e.mon - tm_s.mon;
	if (num_unions < MAX_TERMS) {
		// Reserve upper bound.
		res.reserve(get_upper_bound(prefix_dot.length(), num_unions, 4));
		while (tm_s.mon != tm_e.mon) {
			res.append(prefix_dot).append(transform_to_query_string(tm_s)).append(" OR ");
			++tm_s.mon;
		}
		res.append(prefix_dot).append(transform_to_query_string(tm_e));
	}

	return res;
}


std::string
GenerateTerms::day(Datetime::tm_t& tm_s, Datetime::tm_t& tm_e, const std::string& prefix)
{
	std::string res, prefix_dot;
	prefix_dot.reserve(prefix.length() + 1);
	prefix_dot.assign(prefix).push_back(':');

	tm_s.sec = tm_s.min = tm_s.hour = tm_e.sec = tm_e.min = tm_e.hour = 0;
	int num_unions = tm_e.day - tm_s.day;
	if (num_unions < MAX_TERMS) {
		// Reserve upper bound.
		res.reserve(get_upper_bound(prefix_dot.length(), num_unions, 4));
		while (tm_s.day != tm_e.day) {
			res.append(prefix_dot).append(transform_to_query_string(tm_s)).append(" OR ");
			++tm_s.day;
		}
		res.append(prefix_dot).append(transform_to_query_string(tm_e));
	}

	return res;
}


std::string
GenerateTerms::hour(Datetime::tm_t& tm_s, Datetime::tm_t& tm_e, const std::string& prefix)
{
	std::string res, prefix_dot;
	prefix_dot.reserve(prefix.length() + 1);
	prefix_dot.assign(prefix).push_back(':');

	tm_s.sec = tm_s.min = tm_e.sec = tm_e.min = 0;
	int num_unions = tm_e.hour - tm_s.hour;
	if (num_unions < MAX_TERMS) {
		// Reserve upper bound.
		res.reserve(get_upper_bound(prefix_dot.length(), num_unions, 4));
		while (tm_s.hour != tm_e.hour) {
			res.append(prefix_dot).append(transform_to_query_string(tm_s)).append(" OR ");
			++tm_s.hour;
		}
		res.append(prefix_dot).append(transform_to_query_string(tm_e));
	}

	return res;
}


std::string
GenerateTerms::minute(Datetime::tm_t& tm_s, Datetime::tm_t& tm_e, const std::string& prefix)
{
	std::string res, prefix_dot;
	prefix_dot.reserve(prefix.length() + 1);
	prefix_dot.assign(prefix).push_back(':');

	tm_s.sec = tm_e.sec = 0;
	int num_unions = tm_e.min - tm_s.min;
	if (num_unions < MAX_TERMS) {
		// Reserve upper bound.
		res.reserve(get_upper_bound(prefix_dot.length(), num_unions, 4));
		while (tm_s.min != tm_e.min) {
			res.append(prefix_dot).append(transform_to_query_string(tm_s)).append(" OR ");
			++tm_s.min;
		}
		res.append(prefix_dot).append(transform_to_query_string(tm_e));
	}

	return res;
}


std::string
GenerateTerms::second(Datetime::tm_t& tm_s, Datetime::tm_t& tm_e, const std::string& prefix)
{
	std::string res, prefix_dot;
	prefix_dot.reserve(prefix.length() + 1);
	prefix_dot.assign(prefix).push_back(':');

	int num_unions = tm_e.sec - tm_s.sec;
	if (num_unions < MAX_TERMS) {
		// Reserve upper bound.
		res.reserve(get_upper_bound(prefix_dot.length(), num_unions, 4));
		while (tm_s.sec != tm_e.sec) {
			res.append(prefix_dot).append(transform_to_query_string(tm_s)).append(" OR ");
			++tm_s.sec;
		}
		res.append(prefix_dot).append(transform_to_query_string(tm_e));
	}

	return res;
}


std::pair<std::string, std::unordered_set<std::string>>
GenerateTerms::geo(const std::vector<range_t>& ranges, const std::vector<double>& accuracy, const std::vector<std::string>& acc_prefix)
{
	// The user does not specify the accuracy.
	if (acc_prefix.empty() || ranges.empty()) {
		return std::make_pair(std::string(), std::unordered_set<std::string>());
	}

	std::vector<int> pos_accuracy;
	pos_accuracy.reserve(accuracy.size());
	for (const auto& acc : accuracy) {
		pos_accuracy.push_back(START_POS - acc * 2);
	}

	std::map<uint64_t, std::string> results;
	for (const auto& range : ranges) {
		std::bitset<SIZE_BITS_ID> b1(range.start), b2(range.end), res;
		auto idx = -1;
		uint64_t val;
		if (range.start != range.end) {
			for (idx = SIZE_BITS_ID - 1; idx > 0 && b1.test(idx) == b2.test(idx); --idx) {
				res.set(idx, b1.test(idx));
			}
			val = res.to_ullong();
		} else {
			val = range.start;
		}

		for (auto i = accuracy.size() - 1; i > 1; --i) {
			if (pos_accuracy[i] > idx) {
				results.insert(std::make_pair(val >> pos_accuracy[i], acc_prefix[i - 2]));
				break;
			}
		}
	}

	// The search have trixels more big that the biggest trixel in accuracy.
	if (results.empty()) {
		return std::make_pair(std::string(), std::unordered_set<std::string>());
	}

	// Delete duplicates terms.
	auto it = results.begin();
	auto last_valid(std::bitset<SIZE_BITS_ID>(it->first).to_string());
	last_valid.assign(last_valid.substr(last_valid.find("1")));
	auto result_terms(it->second);
	result_terms.append(":").append(std::to_string(it->first));
	std::unordered_set<std::string> used_prefixes({ it->second });
	used_prefixes.reserve(acc_prefix.size());
	const auto it_e = results.end();
	for (++it; it != it_e; ++it) {
		if (isnotSubtrixel(last_valid, it->first)) {
			used_prefixes.insert(it->second);
			result_terms.append(" OR ").append(it->second).append(":").append(std::to_string(it->first));
		}
	}

	return std::make_pair(std::move(result_terms), std::move(used_prefixes));
}
