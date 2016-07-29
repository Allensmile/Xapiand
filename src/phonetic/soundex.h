/*
 * Copyright (C) 2016 deipi.com LLC and contributors. All rights reserved.
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

#include "../utils.h"


/*
 * Interface for implement soundex with diferent languages.
 */
template <typename Impl>
class Soundex {
protected:
	std::string _code_str;

public:
	Soundex() = default;

	template <typename T>
	Soundex(T&& code_str)
		: _code_str(std::forward<T>(code_str)) { }

	template <typename T>
	inline std::string encode(T&& str) const {
		return static_cast<const Impl*>(this)->_encode(str);
	}

	inline std::string encode() const {
		return _code_str;
	}

	inline std::string description() const noexcept {
		return static_cast<const Impl*>(this)->_description();
	}
};


/*
 * Auxiliary functions.
 */

template <typename Container>
inline static void replace(std::string& str, size_t pos, const Container& patterns) {
	for (const auto& pattern : patterns) {
		auto _pos = str.find(pattern.first, pos);
		while (_pos != std::string::npos) {
			str.replace(_pos, pattern.first.length(), pattern.second);
			_pos = str.find(pattern.first, _pos + pattern.second.length());
		}
	}
}


template <typename Iterator>
inline static void replace(std::string& str, size_t pos, Iterator begin, Iterator end) {
	while (begin != end) {
		auto _pos = str.find(begin->first, pos);
		while (_pos != std::string::npos) {
			str.replace(_pos, begin->first.length(), begin->second);
			_pos = str.find(begin->first, _pos + begin->second.length());
		}
		++begin;
	}
}


template <typename Container>
inline static void replace_prefix(std::string& str, const Container& prefixes) {
	for (const auto& prefix : prefixes) {
		if (std::mismatch(prefix.first.begin(), prefix.first.end(), str.begin()).first == prefix.first.end()) {
			str.replace(0, prefix.first.length(), prefix.second);
			return;
		}
	}
}
