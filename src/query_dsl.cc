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

#include "xxh64.hpp"

#include "query_dsl.h"

#include "database_utils.h"
#include "exception.h"
#include "multivalue/range.h"


constexpr const char QUERYDSL_VALUE[]     = "_value";
constexpr const char QUERYDSL_BOOST[]     = "_boost";
constexpr const char QUERYDSL_RANGE[]     = "_range";
constexpr const char QUERYDSL_FROM[]      = "_from";
constexpr const char QUERYDSL_IN[]        = "_in";
constexpr const char QUERYDSL_TO[]        = "_to";
constexpr const char QUERYDSL_OR[]        = "_or";
constexpr const char QUERYDSL_AND[]       = "_and";
constexpr const char QUERYDSL_XOR[]       = "_xor";
constexpr const char QUERYDSL_NOT[]       = "_not";
constexpr const char QUERYDSL_MATCH_ALL[] = "_all";
//constexpr const char QUERYDSL_GEO_POLIGON[] = "_polygon";
// constexpr const char QUERYDSL_EWKT[]       = "_ewkt";


static constexpr auto HASH_ALL   = xxh64::hash(QUERYDSL_MATCH_ALL);


static const std::unordered_map<std::string, FieldType> map_type({
	{ FLOAT_STR,       FieldType::FLOAT        }, { INTEGER_STR,     FieldType::INTEGER     },
	{ POSITIVE_STR,    FieldType::POSITIVE     }, { STRING_STR,      FieldType::STRING      },
	{ TEXT_STR,        FieldType::TEXT         }, { DATE_STR,        FieldType::DATE        },
	{ GEO_STR,         FieldType::GEO          }, { BOOLEAN_STR,     FieldType::BOOLEAN     },
	{ UUID_STR,        FieldType::UUID         },
});


static const std::unordered_map<std::string, Xapian::Query::op> map_xapian_operator({
	{ QUERYDSL_OR,        Xapian::Query::OP_OR         },
	{ QUERYDSL_AND,       Xapian::Query::OP_AND        },
	{ QUERYDSL_XOR,       Xapian::Query::OP_XOR  	   },
	{ QUERYDSL_NOT,       Xapian::Query::OP_AND_NOT    },
});


const std::unordered_map<std::string, dispatch_op_dsl> map_op_dispatch_dsl({
	{ QUERYDSL_OR,        &QueryDSL::join_queries         },
	{ QUERYDSL_AND,       &QueryDSL::join_queries         },
	{ QUERYDSL_XOR,       &QueryDSL::join_queries  		  },
	{ QUERYDSL_NOT,       &QueryDSL::join_queries         },
});


const std::unordered_map<std::string, dispatch_dsl> map_dispatch_dsl({
	{ QUERYDSL_IN,        &QueryDSL::in_range_query   },
	{ QUERYDSL_VALUE,     &QueryDSL::query            },
	{ TYPE_INTEGER,       &QueryDSL::query            },
	{ TYPE_POSITIVE,      &QueryDSL::query            },
	{ TYPE_FLOAT,         &QueryDSL::query            },
	{ TYPE_BOOLEAN,       &QueryDSL::query            },
	{ TYPE_STRING,        &QueryDSL::query            },
	{ TYPE_TEXT,          &QueryDSL::query            },
	{ TYPE_UUID,          &QueryDSL::query            },
	{ TYPE_EWKT,          &QueryDSL::query            },
});


const std::unordered_map<std::string, dispatch_dsl> map_range_dispatch_dsl({
	{ QUERYDSL_RANGE,     &QueryDSL::range_query   },
	/* Add more types in range */
});


/* A domain-specific language (DSL) for query */
QueryDSL::QueryDSL(std::shared_ptr<Schema> schema_)
	: schema(schema_),
      state(QUERY::INIT),
      _wqf(1)
{
	q_flags = Xapian::QueryParser::FLAG_DEFAULT | Xapian::QueryParser::FLAG_WILDCARD;
}


Xapian::Query
QueryDSL::get_query(const MsgPack& obj)
{
	L_CALL(this, "QueryDSL::get_query()");

	if (obj.is_map() && obj.is_map() == 1) {
		for (auto const& elem : obj) {
			state =	QUERY::GLOBALQUERY;
			auto str_key = elem.as_string();
			try {
				auto func = map_op_dispatch_dsl.at(str_key);
				return (this->*func)(obj.at(str_key), map_xapian_operator.at(str_key));
			} catch (const std::out_of_range&) {
				try {
					auto func = map_dispatch_dsl.at(str_key);
					return (this->*func)(obj.at(str_key));
				} catch (const std::out_of_range&) {
					auto const& o = obj.at(str_key);
					switch (o.getType()) {
						case MsgPack::Type::ARRAY:
							throw MSG_QueryDslError("Unexpected type %s in %s", MsgPackTypes[static_cast<int>(MsgPack::Type::ARRAY)], str_key.c_str());
						case MsgPack::Type::MAP:
							return process_query(o, str_key);
						default: {
							_fieldname = str_key;
							return query(o);
						}
					}
				}
			}
		}
	} else if (obj.is_string() && xxh64::hash(lower_string(obj.as_string())) == HASH_ALL) {
		return Xapian::Query::MatchAll;
	} else {
		throw MSG_QueryDslError("Type error expected map of size one at root level in query dsl");
	}
	return Xapian::Query();
}


Xapian::Query
QueryDSL::join_queries(const MsgPack& obj, Xapian::Query::op op)
{
	L_CALL(this, "QueryDSL::join_queries()");

	Xapian::Query final_query;
	if (op == Xapian::Query::OP_AND_NOT) {
		final_query = Xapian::Query::MatchAll;
	}
	if (obj.is_array()) {
		for (const auto& elem : obj) {
			if (elem.is_map() && elem.size() == 1) {
				for (const auto& field : elem) {
					state = QUERY::GLOBALQUERY;
					auto str_key = field.as_string();
					try {
						auto func = map_op_dispatch_dsl.at(str_key);
						final_query.empty() ?  final_query = (this->*func)(elem.at(str_key), map_xapian_operator.at(str_key)) : final_query = Xapian::Query(op, final_query, (this->*func)(elem.at(str_key), map_xapian_operator.at(str_key)));
					} catch (const std::out_of_range&) {
						try{
							auto func = map_dispatch_dsl.at(str_key);
							final_query.empty() ? final_query = (this->*func)(elem.at(str_key)) : final_query = Xapian::Query(op, (this->*func)(elem.at(str_key)));
						} catch (const std::out_of_range&) {
							if (!startswith(str_key, "_")) {
								const auto& o = elem.at(str_key);
								switch (o.getType()) {
									case MsgPack::Type::ARRAY:
										throw MSG_QueryDslError("Unexpected type array in %s", str_key.c_str());
									case MsgPack::Type::MAP:
										final_query.empty() ? final_query = process_query(o, str_key) : final_query = Xapian::Query(op, final_query, process_query(o, str_key));
										break;
									default:
										_fieldname = str_key;
										final_query.empty() ? final_query = query(o) : final_query = Xapian::Query(op, final_query, query(o));
								}
							} else {
								throw MSG_QueryDslError("Unexpected reserved word %s", str_key.c_str());
							}
						}
					}
				}

			} else {
				throw MSG_QueryDslError("Expected array of objects with one element");
			}
		}

	} else {
		throw MSG_QueryDslError("Type error expected map in boolean operator");
	}
	return final_query;
}


Xapian::Query
QueryDSL::process_query(const MsgPack& obj, const std::string& field_name)
{
	L_CALL(this, "QueryDSL::process_query()");

	_fieldname = field_name;
	state = QUERY::QUERY;
	if (obj.is_map()) {
		set_parameters(obj);
		for (const auto& elem : obj) {
			auto str_key = elem.as_string();
			try {
				auto func = map_dispatch_dsl.at(str_key);
				return (this->*func)(obj.at(str_key));
			} catch (const std::out_of_range&) { }
		}
	}

	return Xapian::Query();
}


Xapian::Query
QueryDSL::in_range_query(const MsgPack& obj)
{
	L_CALL(this, "QueryDSL::in_range_query()");

	if (obj.is_map() && obj.size() == 1) {
		for (const auto& elem : obj) {
			std::string str_key;
			try {
				str_key = elem.as_string();
				auto func = map_range_dispatch_dsl.at(str_key);
				return (this->*func)(obj.at(str_key));
			} catch (const std::out_of_range&) {
				throw MSG_QueryDslError("Unexpected range type %s", str_key.c_str());
			}
		}
	} else {
		throw MSG_QueryDslError("Expected object type with one element");
	}

	return Xapian::Query();
}


Xapian::Query
QueryDSL::range_query(const MsgPack& obj)
{
	L_CALL(this, "QueryDSL::range_query()");

	switch (state) {
		case QUERY::GLOBALQUERY:
		{
			MsgPack to;
			MsgPack from;
			try {
				to = obj.at(QUERYDSL_TO);
			} catch (const std::out_of_range&) { }
			try {
				from = obj.at(QUERYDSL_FROM);
			} catch (const std::out_of_range&) { }
			if (to || from) {
				std::tuple<FieldType, std::string, std::string> ser_type = Serialise::get_range_type(from, to);
				const auto& global_spc = Schema::get_data_global(std::get<0>(ser_type));
				return MultipleValueRange::getQuery(global_spc, "", from, to);
			} else {
				throw MSG_QueryDslError("Expected %s and/or %s in %s", QUERYDSL_FROM, QUERYDSL_TO, QUERYDSL_RANGE);
			}

		}
		break;

		case QUERY::QUERY:
		{
			MsgPack to;
			MsgPack from;
			try {
				to = obj.at(QUERYDSL_TO);
			} catch (const std::out_of_range&) { }
			try {
				from = obj.at(QUERYDSL_FROM);
			} catch (const std::out_of_range&) { }
			if (to || from) {
				return MultipleValueRange::getQuery(schema->get_data_field(_fieldname), _fieldname, from, to);
			} else {
				throw MSG_QueryDslError("Expected %s and/or %s in %s", QUERYDSL_FROM, QUERYDSL_TO, QUERYDSL_RANGE);
			}
		}
		break;

		default:
			break;
	}
	return Xapian::Query();
}


Xapian::Query
QueryDSL::query(const MsgPack& obj)
{
	L_CALL(this, "QueryDSL::query()");

	switch (state) {
		case QUERY::GLOBALQUERY:
		{
			auto ser_type = Serialise::get_type(obj);
			const auto& global_spc = Schema::get_data_global(ser_type.first);
			switch (ser_type.first) {
				case FieldType::STRING:
				case FieldType::TEXT: {
					Xapian::QueryParser queryTexts;
					queryTexts.set_stemming_strategy(getQueryParserStrategy(global_spc.stem_strategy));
					queryTexts.set_stemmer(Xapian::Stem(global_spc.stem_language));
					return queryTexts.parse_query(obj.as_string(), q_flags);
				}
				default:
					return Xapian::Query(ser_type.second);
			}
		}
		break;

		case QUERY::QUERY:
		{
			auto field_spc = schema->get_data_field(_fieldname);
			try {
				switch (field_spc.get_type()) {
					case FieldType::INTEGER:
					case FieldType::POSITIVE:
					case FieldType::FLOAT:
					case FieldType::DATE:
					case FieldType::UUID:
					case FieldType::BOOLEAN:
						return Xapian::Query(prefixed(Serialise::serialise(field_spc, obj), field_spc.prefix), _wqf);

					case FieldType::STRING:
					{
						auto field_value = Serialise::serialise(field_spc, obj);
						return Xapian::Query(prefixed(field_spc.bool_term ? field_value : lower_string(field_value), field_spc.prefix), _wqf);
					}
					case FieldType::TEXT:
					{
						auto field_value = Serialise::serialise(field_spc, obj);
						Xapian::QueryParser queryTexts;
						field_spc.bool_term ? queryTexts.add_boolean_prefix(_fieldname, field_spc.prefix) : queryTexts.add_prefix(_fieldname, field_spc.prefix);
						queryTexts.set_stemming_strategy(getQueryParserStrategy(field_spc.stem_strategy));
						queryTexts.set_stemmer(Xapian::Stem(field_spc.stem_language));
						std::string str_texts;
						str_texts.reserve(field_value.length() + field_value.length() + 1);
						str_texts.assign(field_value).append(":").append(field_value);
						return queryTexts.parse_query(str_texts, q_flags);
					}
					case FieldType::GEO:
					{
						std::string field_value(Serialise::serialise(field_spc, obj));
						// If the region for search is empty, not process this query.
						if (field_value.empty()) {
							return Xapian::Query::MatchNothing;
						}
						return Xapian::Query(prefixed(field_value, field_spc.prefix), _wqf);
					}
					default:
						throw MSG_QueryDslError("Type error unexpected %s");
				}
			} catch (const msgpack::type_error&) {
				throw MSG_QueryDslError("Type error expected %s in %s", Serialise::type(field_spc.get_type()).c_str(), _fieldname.c_str());
			}

		}
		break;

		default:
			break;
	}
	return Xapian::Query();
}


void
QueryDSL::set_parameters(const MsgPack& obj)
{
	try {
		auto const& boost = obj.at(QUERYDSL_BOOST);
		if (boost.is_number() && boost.getType() != MsgPack::Type::NEGATIVE_INTEGER) {
			_wqf = boost.as_u64();
		} else {
			throw MSG_QueryDslError("Type error expected unsigned int in %s", QUERYDSL_BOOST);
		}
	} catch(const std::out_of_range&) { }

	/* Add here more options for the fields */
}
