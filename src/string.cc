/*
 * Copyright (C) 2015-2018 Dubalu LLC. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "string.hh"

#include "colors.h"

#include <cmath>              // for std::log, std::pow
#include <vector>             // for std::vector


class Humanize {
	long double base;
	long double div;
	std::vector<long double> scaling;
	std::vector<std::string> units;
	std::vector<std::string> colors;
	int needle;

public:
	Humanize(
		long double base_,
		std::vector<long double>&& scaling_,
		std::vector<std::string>&& units_,
		std::vector<std::string>&& colors_
	) :
		base(base_),
		div(std::log(base)),
		scaling(std::move(scaling_)),
		units(std::move(units_)),
		colors(std::move(colors_)),
		needle(std::distance(scaling.begin(), std::find(scaling.begin(), scaling.end(), 1)))
	{}

	std::string operator()(long double delta, const char* prefix, bool colored, long double rounding) const {
		long double num = delta;
		auto n = units.size();

		if (delta < 0) {
			delta = -delta;
		}
		size_t order = (delta == 0) ? n : -std::floor(std::log(delta) / div);
		order += needle;
		if (order < 0) {
			order = 0;
		} else if (order > n) {
			order = n;
		}

		num = std::round(rounding * num / scaling[order]) / rounding;
		auto& unit = units[order];

		if (colored) {
			auto& color = colors[order];
			auto& reset = colors[n + 1];
			return string::format("%s%s%s%s%s", color, prefix, string::Number(static_cast<double>(num)), unit, reset);
		}

		return string::format("%s%s%s", prefix, string::Number(static_cast<double>(num)), unit);
	}

};

// MEDIUM_SEA_GREEN  -> rgb(60, 179, 113)
// MEDIUM_SEA_GREEN  -> rgb(60, 179, 113)
// SEA_GREEN         -> rgb(46, 139, 87);
// OLIVE_DRAB        -> rgb(107, 142, 35)
// OLIVE             -> rgb(128, 128, 0)
// DARK_GOLDEN_ROD   -> rgb(184, 134, 11);
// PERU              -> rgb(205, 133, 63);
// SADDLE_BROWN      -> rgb(139, 69, 19);
// BROWN             -> rgb(165, 42, 42);

static inline std::string _from_bytes(size_t bytes, const char* prefix, bool colored) {
	static const Humanize humanize(
		1024,
		{ std::pow(1024, 8), std::pow(1024, 7), std::pow(1024, 6), std::pow(1024, 5), std::pow(1024, 4), std::pow(1024, 3), std::pow(1024, 2), std::pow(1024, 1), 1 },
		{ "YiB", "ZiB", "EiB", "PiB", "TiB", "GiB", "MiB", "KiB", "B" },
		{ BROWN, BROWN, BROWN, BROWN, BROWN, PERU, OLIVE, SEA_GREEN, MEDIUM_SEA_GREEN, CLEAR_COLOR }
	);
	return humanize(bytes, prefix, colored, 10.0L);
}

std::string string::from_bytes(size_t bytes, const char* prefix, bool colored) {
	return _from_bytes(bytes, prefix, colored);
}


static inline std::string _from_small_time(long double seconds, const char* prefix, bool colored) {
	static const Humanize humanize(
		1000,
		{ 1, std::pow(1000, -1), std::pow(1000, -2), std::pow(1000, -3), std::pow(1000, -4) },
		{ "s", "ms", R"(µs)", "ns", "ps" },
		{ OLIVE, OLIVE_DRAB, SEA_GREEN, MEDIUM_SEA_GREEN, MEDIUM_SEA_GREEN, CLEAR_COLOR }
	);
	return humanize(seconds, prefix, colored, 1000.0L);
}

std::string string::from_small_time(long double seconds, const char* prefix, bool colored) {
	return _from_small_time(seconds, prefix, colored);
}

static inline std::string _from_time(long double seconds, const char* prefix, bool colored) {
	static const Humanize humanize(
		60,
		{ std::pow(60, 2), std::pow(60, 1), 1 },
		{ "hrs", "min", "s" },
		{ SADDLE_BROWN, PERU, DARK_GOLDEN_ROD, CLEAR_COLOR }
	);
	return humanize(seconds, prefix, colored, 100.0L);
}

std::string string::from_time(long double seconds, const char* prefix, bool colored) {
	return _from_time(seconds, prefix, colored);
}


static inline std::string _from_delta(long double nanoseconds, const char* prefix, bool colored) {
	long double seconds = nanoseconds / 1e9;  // convert nanoseconds to seconds (as a double)
	return (seconds < 1) ? _from_small_time(seconds, prefix, colored) : _from_time(seconds, prefix, colored);
}

std::string string::from_delta(long double nanoseconds, const char* prefix, bool colored) {
	return _from_delta(nanoseconds, prefix, colored);
}


std::string string::from_delta(const std::chrono::time_point<std::chrono::system_clock>& start, const std::chrono::time_point<std::chrono::system_clock>& end, const char* prefix, bool colored) {
	return _from_delta(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count(), prefix, colored);
}
