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

#include "aggregation_bucket.h"

#include <algorithm>                      // for move, set_intersection
#include <iterator>                       // for back_insert_iterator, back_...
#include <map>                            // for __tree_const_iterator, oper...

#include "metrics/basic_string_metric.h"  // for Counter
#include "multivalue/aggregation.h"       // for Aggregation
#include "schema.h"                       // for Schema, required_spc_t


FilterAggregation::FilterAggregation(MsgPack& result, const MsgPack& conf, const std::shared_ptr<Schema>& schema)
	: SubAggregation(result),
	  _agg(result, conf, schema)
{
	if (!conf.is_map()) {
		THROW(AggregationError, "%s must be object", repr(conf.to_string()));
	}

	const auto filter_it = conf.find(AGGREGATION_FILTER);
	if (filter_it == conf.end()) {
		THROW(AggregationError, "'%s' must be specified in %s", AGGREGATION_FILTER, repr(conf.to_string()));
	}
	const auto& filter_conf = filter_it.value();
	if (!filter_conf.is_map()) {
		THROW(AggregationError, "%s must be object", repr(filter_conf.to_string()));
	}

	const auto term_filter_it = filter_conf.find(AGGREGATION_TERM);
	if (term_filter_it == filter_conf.end()) {
		THROW(AggregationError, "'%s' must be specified in %s", AGGREGATION_TERM, repr(filter_conf.to_string()));
	}
	const auto& term_filter_conf = term_filter_it.value();
	if (!term_filter_conf.is_map()) {
		THROW(AggregationError, "%s must be object", repr(term_filter_conf.to_string()));
	}

	const auto it = term_filter_conf.begin();
	const auto it_end = term_filter_conf.end();
	for (; it != it_end; ++it) {
		auto field_name = it->str_view();
		auto field_spc = schema->get_slot_field(field_name);
		const auto& values = it.value();
		std::set<std::string> s_values;
		if (values.is_array()) {
			for (const auto& value : values) {
				s_values.insert(Serialise::MsgPack(field_spc, value));
			}
			func = &FilterAggregation::check_multiple;
		} else {
			s_values.insert(Serialise::MsgPack(field_spc, values));
			func = &FilterAggregation::check_single;
		}
		_filters.emplace_back(field_spc.slot, std::move(s_values));
	}
}


void
FilterAggregation::update()
{
	_agg.update();
}


void
FilterAggregation::check_single(const Xapian::Document& doc)
{
	for (const auto& filter : _filters) {
		std::unordered_set<std::string> values;
		StringList::unserialise(doc.get_value(filter.first), std::inserter(values, values.begin()));
		if (values.find(*filter.second.begin()) != values.end()) {
			return _agg(doc);
		}
	}
}


void
FilterAggregation::check_multiple(const Xapian::Document& doc)
{
	for (const auto& filter : _filters) {
		std::set<std::string> values;
		StringList::unserialise(doc.get_value(filter.first), std::inserter(values, values.begin()));
		Counter c;
		std::set_intersection(values.begin(), values.end(), filter.second.begin(), filter.second.end(), std::back_inserter(c));
		if (c.count) {
			return _agg(doc);
		}
	}
}
