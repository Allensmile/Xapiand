/*
* Copyright (c) 2015-2019 Dubalu LLC
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

#include <memory>                                 // for std::shared_ptr
#include <string>                                 // for std::string
#include "string_view.hh"                         // for std::string_view
#include <unordered_map>                          // for std::unordered_map
#include <unordered_set>                          // for std::unordered_set

#include "msgpack.h"                              // for MsgPack
#include "multivalue/keymaker.h"                  // for Multi_MultiValueKeyMaker"
#include "reserved.h"                             // for RESERVED__
#include "schema.h"                               // for Schema, FieldType, required_spc_t
#include "xapian.h"                               // for Query, Query::op, termcount


constexpr const char RESERVED_QUERYDSL_FROM[]               = RESERVED__ "from";
constexpr const char RESERVED_QUERYDSL_IN[]                 = RESERVED__ "in";
constexpr const char RESERVED_QUERYDSL_QUERY[]              = RESERVED__ "query";
constexpr const char RESERVED_QUERYDSL_RANGE[]              = RESERVED__ "range";
constexpr const char RESERVED_QUERYDSL_RAW[]                = RESERVED__ "raw";
constexpr const char RESERVED_QUERYDSL_TO[]                 = RESERVED__ "to";
constexpr const char RESERVED_QUERYDSL_LIMIT[]              = RESERVED__ "limit";
constexpr const char RESERVED_QUERYDSL_CHECK_AT_LEAST[]     = RESERVED__ "check_at_least";
constexpr const char RESERVED_QUERYDSL_OFFSET[]             = RESERVED__ "offset";
constexpr const char RESERVED_QUERYDSL_SORT[]               = RESERVED__ "sort";
constexpr const char RESERVED_QUERYDSL_SELECTOR[]           = RESERVED__ "selector";
constexpr const char RESERVED_QUERYDSL_ORDER[]              = RESERVED__ "order";
constexpr const char RESERVED_QUERYDSL_METRIC[]             = RESERVED__ "metric";

constexpr const char QUERYDSL_ASC[]     = "asc";
constexpr const char QUERYDSL_DESC[]    = "desc";

/* A domain-specific language (DSL) for query */


class QueryDSL {
	std::shared_ptr<Schema> schema;

	FieldType get_in_type(const MsgPack& obj);

	std::pair<FieldType, MsgPack> parse_guess_range(const required_spc_t& field_spc, std::string_view range);
	MsgPack parse_range(const required_spc_t& field_spc, std::string_view range);

	Xapian::Query process(Xapian::Query::op op, std::string_view path, const MsgPack& obj, Xapian::termcount wqf, unsigned flags);
	Xapian::Query get_in_query(std::string_view path, const MsgPack& obj, Xapian::termcount wqf, unsigned flags);
	Xapian::Query get_raw_query(std::string_view path, const MsgPack& obj, Xapian::termcount wqf, unsigned flags);
	Xapian::Query get_value_query(std::string_view path, const MsgPack& obj, Xapian::termcount wqf, unsigned flags);

	Xapian::Query get_acc_date_query(const required_spc_t& field_spc, std::string_view field_accuracy, const MsgPack& obj, Xapian::termcount wqf);
	Xapian::Query get_acc_time_query(const required_spc_t& field_spc, std::string_view field_accuracy, const MsgPack& obj, Xapian::termcount wqf);
	Xapian::Query get_acc_timedelta_query(const required_spc_t& field_spc, std::string_view field_accuracy, const MsgPack& obj, Xapian::termcount wqf);
	Xapian::Query get_acc_num_query(const required_spc_t& field_spc, std::string_view field_accuracy, const MsgPack& obj, Xapian::termcount wqf);
	Xapian::Query get_acc_geo_query(const required_spc_t& field_spc, std::string_view field_accuracy, const MsgPack& obj, Xapian::termcount wqf);
	Xapian::Query get_accuracy_query(const required_spc_t& field_spc, std::string_view field_accuracy, const MsgPack& obj, Xapian::termcount wqf);
	Xapian::Query get_namespace_query(const required_spc_t& field_spc, const MsgPack& obj, Xapian::termcount wqf, unsigned flags);
	Xapian::Query get_regular_query(const required_spc_t& field_spc, const MsgPack& obj, Xapian::termcount wqf, unsigned flags);
	Xapian::Query get_term_query(const required_spc_t& field_spc, std::string_view serialised_term, Xapian::termcount wqf, unsigned flags);
	Xapian::Query get_namespace_in_query(const required_spc_t& field_spc, const MsgPack& obj, Xapian::termcount wqf, unsigned flags);
	Xapian::Query get_regular_in_query(const required_spc_t& field_spc, const MsgPack& obj, Xapian::termcount wqf, unsigned flags);
	Xapian::Query get_in_query(const required_spc_t& field_spc, const MsgPack& obj);

	void create_2exp_op_dsl(std::vector<MsgPack>& stack_msgpack, const std::string& operator_dsl);
	void create_exp_op_dsl(std::vector<MsgPack>& stack_msgpack, const std::string& operator_dsl);

public:
	QueryDSL(std::shared_ptr<Schema>  schema_);

	MsgPack make_dsl_query(std::string_view query);
	MsgPack make_dsl_query(const query_field_t& e);

	Xapian::Query get_query(const MsgPack& obj);
	void get_sorter(std::unique_ptr<Multi_MultiValueKeyMaker>& sorter, const MsgPack& obj);
};
