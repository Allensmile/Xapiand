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

#pragma once

#include "msgpack.h"
#include "schema.h"
#include "utils.h"

#include "xapian.h"

class QueryDSL;

constexpr const char QUERYDSL_QUERY[] = "_query";

using dispatch_op_dsl = Xapian::Query (QueryDSL::*)(const MsgPack&, Xapian::Query::op);
using dispatch_dsl = Xapian::Query (QueryDSL::*)(const MsgPack&);


/* A domain-specific language (DSL) for query */

class QueryDSL {
	enum class QUERY {
		INIT,
		GLOBALQUERY,
		QUERY
	};

	std::shared_ptr<Schema> schema;

	int q_flags;
	QUERY state;
	std::string _fieldname;
	Xapian::termcount _wqf;

	Xapian::Query process_query(const MsgPack& obj, const std::string& field_name);
	void find_parameters(const MsgPack& obj);

	bool find_operators(const std::string& key, const MsgPack& obj, Xapian::Query& q);
	bool find_casts(const std::string& key, const MsgPack& obj, Xapian::Query& q);
	bool find_values(const std::string& key, const MsgPack& obj, Xapian::Query& q);
	bool find_ranges(const std::string& key, const MsgPack& obj, Xapian::Query& q);

public:
	QueryDSL(std::shared_ptr<Schema> schema_);

	Xapian::Query get_query(const MsgPack& obj);

	Xapian::Query join_queries(const MsgPack& obj, Xapian::Query::op op);
	Xapian::Query in_range_query(const MsgPack& obj);
	Xapian::Query query(const MsgPack& o);
	Xapian::Query range_query(const MsgPack& o);
};

extern const std::unordered_map<std::string, dispatch_op_dsl> map_op_dispatch_dsl;
extern const std::unordered_map<std::string, dispatch_dsl> map_dispatch_dsl;
