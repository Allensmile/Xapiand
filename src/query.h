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

#include "xapiand.h"

#include <xapian.h>  // for Query
#include <memory>    // for shared_ptr
#include <string>    // for string
#include <vector>    // for vector

#include "database.h"
#include "schema.h"


class Database;
class Schema;
struct query_field_t;


class Query {
	std::shared_ptr<Schema> schema;
	std::shared_ptr<Database> database;

	Xapian::Query build_query(const std::string& token, std::vector<std::string>& suggestions, int q_flags);
	Xapian::Query make_query(const std::string& str_query, std::vector<std::string>& suggestions, int q_flags);

public:
	Query(const std::shared_ptr<Schema>& schema_, const std::shared_ptr<Database>& database_);

	Xapian::Query get_query(const query_field_t& e, std::vector<std::string>& suggestions);
};
