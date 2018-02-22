/*
 * Copyright (C) 2015-2018 dubalu.com LLC. All rights reserved.
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

#include "aggregation.h"

#include <stdexcept>                        // for out_of_range

#include "aggregation_bucket.h"             // for FilterAggregation, Histog...
#include "aggregation_metric.h"             // for AGGREGATION_AVG, AGGREGAT...
#include "database_utils.h"                 // for is_valid
#include "exception.h"                      // for AggregationError, MSG_Agg...
#include "msgpack.h"                        // for MsgPack, MsgPack::const_i...
#include "schema.h"                         // for Schema
#include "hashes.hh"                        // for fnv1a32


Aggregation::Aggregation(MsgPack& result)
	: _result(result),
	  _doc_count(0)
{
	_result[AGGREGATION_DOC_COUNT] = _doc_count;  // Initialize here so it's at the start
}


Aggregation::Aggregation(MsgPack& result, const MsgPack& conf, const std::shared_ptr<Schema>& schema)
	: _result(result),
	  _doc_count(0)
{
	_result[AGGREGATION_DOC_COUNT] = _doc_count;  // Initialize here so it's at the start

	try {
		const auto& aggs = conf.at(AGGREGATION_AGGS);
		for (const auto& agg : aggs) {
			auto sub_agg_name = agg.str_view();
			if (is_valid(sub_agg_name)) {
				const auto& sub_agg = aggs.at(sub_agg_name);
				auto sub_agg_type = sub_agg.begin()->str_view();
				switch (fnv1a32::hash(sub_agg_type)) {
					case fnv1a32::hash(AGGREGATION_COUNT):
						add_metric<AGGREGATION_COUNT, MetricCount>(_result[sub_agg_name], sub_agg, schema);
						break;
					// case fnv1a32::hash(AGGREGATION_CARDINALITY):
					// 	add_metric<AGGREGATION_CARDINALITY, MetricCardinality>(_result[sub_agg_name], sub_agg, schema);
					// 	break;
					case fnv1a32::hash(AGGREGATION_SUM):
						add_metric<AGGREGATION_SUM, MetricSum>(_result[sub_agg_name], sub_agg, schema);
						break;
					case fnv1a32::hash(AGGREGATION_AVG):
						add_metric<AGGREGATION_AVG, MetricAvg>(_result[sub_agg_name], sub_agg, schema);
						break;
					case fnv1a32::hash(AGGREGATION_MIN):
						add_metric<AGGREGATION_MIN, MetricMin>(_result[sub_agg_name], sub_agg, schema);
						break;
					case fnv1a32::hash(AGGREGATION_MAX):
						add_metric<AGGREGATION_MAX, MetricMax>(_result[sub_agg_name], sub_agg, schema);
						break;
					case fnv1a32::hash(AGGREGATION_VARIANCE):
						add_metric<AGGREGATION_VARIANCE, MetricVariance>(_result[sub_agg_name], sub_agg, schema);
						break;
					case fnv1a32::hash(AGGREGATION_STD):
						add_metric<AGGREGATION_STD, MetricSTD>(_result[sub_agg_name], sub_agg, schema);
						break;
					case fnv1a32::hash(AGGREGATION_MEDIAN):
						add_metric<AGGREGATION_MEDIAN, MetricMedian>(_result[sub_agg_name], sub_agg, schema);
						break;
					case fnv1a32::hash(AGGREGATION_MODE):
						add_metric<AGGREGATION_MODE, MetricMode>(_result[sub_agg_name], sub_agg, schema);
						break;
					case fnv1a32::hash(AGGREGATION_STATS):
						add_metric<AGGREGATION_STATS, MetricStats>(_result[sub_agg_name], sub_agg, schema);
						break;
					case fnv1a32::hash(AGGREGATION_EXT_STATS):
						add_metric<AGGREGATION_EXT_STATS, MetricExtendedStats>(_result[sub_agg_name], sub_agg, schema);
						break;
					// case fnv1a32::hash(AGGREGATION_GEO_BOUNDS):
					// 	add_metric<AGGREGATION_GEO_BOUNDS, MetricGeoBounds>(_result[sub_agg_name], sub_agg, schema);
					// 	break;
					// case fnv1a32::hash(AGGREGATION_GEO_CENTROID):
					// 	add_metric<AGGREGATION_GEO_CENTROID, MetricGeoCentroid>(_result[sub_agg_name], sub_agg, schema);
					// 	break;
					// case fnv1a32::hash(AGGREGATION_PERCENTILES):
					// 	add_metric<AGGREGATION_PERCENTILES, MetricPercentiles>(_result[sub_agg_name], sub_agg, schema);
					// 	break;
					// case fnv1a32::hash(AGGREGATION_PERCENTILES_RANK):
					// 	add_metric<AGGREGATION_PERCENTILES_RANK, MetricPercentilesRank>(_result[sub_agg_name], sub_agg, schema);
					// 	break;
					// case fnv1a32::hash(AGGREGATION_SCRIPTED_METRIC):
					// 	add_metric<AGGREGATION_SCRIPTED_METRIC, MetricScripted>(_result[sub_agg_name], sub_agg, schema);
					// 	break;
					case fnv1a32::hash(AGGREGATION_FILTER):
						add_bucket<FilterAggregation>(_result[sub_agg_name], sub_agg, schema);
						break;
					case fnv1a32::hash(AGGREGATION_VALUE):
						add_bucket<ValueAggregation>(_result[sub_agg_name], sub_agg, schema);
						break;
					// case fnv1a32::hash(AGGREGATION_DATE_HISTOGRAM):
					// 	add_bucket<DateHistogramAggregation>(_result[sub_agg_name], sub_agg, schema);
					// 	break;
					// case fnv1a32::hash(AGGREGATION_DATE_RANGE):
					// 	add_bucket<DateRangeAggregation>(_result[sub_agg_name], sub_agg, schema);
					// 	break;
					// case fnv1a32::hash(AGGREGATION_GEO_DISTANCE):
					// 	add_bucket<GeoDistanceAggregation>(_result[sub_agg_name], sub_agg, schema);
					// 	break;
					// case fnv1a32::hash(AGGREGATION_GEO_TRIXELS):
					// 	add_bucket<GeoTrixelsAggregation>(_result[sub_agg_name], sub_agg, schema);
					// 	break;
					case fnv1a32::hash(AGGREGATION_HISTOGRAM):
						add_bucket<HistogramAggregation>(_result[sub_agg_name], sub_agg, schema);
						break;
					// case fnv1a32::hash(AGGREGATION_MISSING):
					// 	add_bucket<MissingAggregation>(_result[sub_agg_name], sub_agg, schema);
					// 	break;
					case fnv1a32::hash(AGGREGATION_RANGE):
						add_bucket<RangeAggregation>(_result[sub_agg_name], sub_agg, schema);
						break;
					// case fnv1a32::hash(AGGREGATION_IP_RANGE):
					// 	add_bucket<IPRangeAggregation>(_result[sub_agg_name], sub_agg, schema);
					// 	break;
					// case fnv1a32::hash(AGGREGATION_GEO_IP):
					// 	add_bucket<GeoIPAggregation>(_result[sub_agg_name], sub_agg, schema);
					// 	break;
					default:
						THROW(AggregationError, "Aggregation type: %s is not valid", repr(sub_agg_name).c_str());
				}
			} else {
				THROW(AggregationError, "Aggregation sub_agg_name: %s is not valid", repr(sub_agg_name).c_str());
			}
		}
	} catch (const msgpack::type_error) {
		THROW(AggregationError, "Aggregations must be an object");
	} catch (const std::out_of_range&) { }
}


void
Aggregation::operator()(const Xapian::Document& doc)
{
	++_doc_count;
	for (auto& sub_agg : _sub_aggregations) {
		(*sub_agg)(doc);
	}
};


void
Aggregation::update()
{
	for (auto& sub_agg : _sub_aggregations) {
		sub_agg->update();
	}
	_result[AGGREGATION_DOC_COUNT] = _doc_count;
}


void
AggregationMatchSpy::operator()(const Xapian::Document& doc, double)
{
	++_total;
	_aggregation(doc);
}


Xapian::MatchSpy*
AggregationMatchSpy::clone() const
{
	return new AggregationMatchSpy(_aggs, _schema);
}


std::string
AggregationMatchSpy::name() const
{
	return "AggregationMatchSpy";
}


std::string
AggregationMatchSpy::serialise() const
{
	std::vector<std::string> data = { _aggs.serialise(), _schema->get_const_schema()->serialise() };
	return StringList::serialise(data.begin(), data.end());
}


Xapian::MatchSpy*
AggregationMatchSpy::unserialise(const std::string& s, const Xapian::Registry&) const
{
	try {
		StringList data(s);

		if (data.size() != 2) {
			throw Xapian::NetworkError("Bad serialised AggregationMatchSpy");
		}

		auto it = data.begin();
		return new AggregationMatchSpy(MsgPack::unserialise(*it), std::make_shared<Schema>(std::make_shared<const MsgPack>(MsgPack::unserialise(*++it)), nullptr, ""));
	} catch (const SerialisationError&) {
		throw Xapian::NetworkError("Bad serialised AggregationMatchSpy");
	}
}


std::string
AggregationMatchSpy::get_description() const
{
	std::string desc("AggregationMatchSpy(");
	desc.append(_aggs.to_string()).push_back(')');
	return desc;
}
