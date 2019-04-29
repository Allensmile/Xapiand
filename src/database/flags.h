/*
 * Copyright (c) 2015-2019 Dubalu LLC
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

#pragma once

#include "strings.hh"         // for strings::join

constexpr int DB_OPEN              = 0x0000;  // Opens a database
constexpr int DB_CREATE_OR_OPEN    = 0x0001;  // Automatically creates the database if it doesn't exist
constexpr int DB_WRITABLE          = 0x0002;  // Opens as writable
constexpr int DB_DISABLE_WAL       = 0x0010;  // Disable open wal file
constexpr int DB_SYNCHRONOUS_WAL   = 0x0020;  // Use sync wal

constexpr int DB_RETRIES           = 10;      // Number of tries to do an operation on a Xapian::Database or Document


inline std::string readable_flags(int flags) {
	std::vector<std::string> values;
	if ((flags & DB_OPEN) == DB_OPEN) values.push_back("DB_OPEN");
	if ((flags & DB_CREATE_OR_OPEN) == DB_CREATE_OR_OPEN) values.push_back("DB_CREATE_OR_OPEN");
	if ((flags & DB_WRITABLE) == DB_WRITABLE) values.push_back("DB_WRITABLE");
	if ((flags & DB_DISABLE_WAL) == DB_DISABLE_WAL) values.push_back("DB_DISABLE_WAL");
	if ((flags & DB_SYNCHRONOUS_WAL) == DB_SYNCHRONOUS_WAL) values.push_back("DB_SYNCHRONOUS_WAL");
	return strings::join(values, "|");
}
