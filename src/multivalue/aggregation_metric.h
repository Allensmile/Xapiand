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

#include <xapian.h>            // for valueno
#include <algorithm>           // for nth_element, max_element
#include <cfloat>              // for DBL_MAX, DBL_MIN
#include <cmath>               // for sqrt
#include <cstdio>
#include <cstring>             // for size_t
#include <memory>              // for shared_ptr, allocator
#include <stdexcept>           // for out_of_range
#include <string>              // for string
#include <unordered_map>       // for __hash_map_iterator, unordered_map
#include <utility>             // for pair
#include <vector>              // for vector

#include "exception.h"         // for AggregationError, MSG_AggregationError
#include "msgpack.h"           // for MsgPack, object::object
#include "serialise.h"         // for _float, boolean, geo, integer, positive
#include "stl_serialise.h"     // for StringList

class Schema;

constexpr const char AGGREGATION_AGGS[]             = "_aggregations";
constexpr const char AGGREGATION_DOC_COUNT[]        = "_doc_count";
constexpr const char AGGREGATION_FIELD[]            = "_field";
constexpr const char AGGREGATION_SUM_OF_SQ[]        = "_sum_of_squares";
constexpr const char AGGREGATION_INTERVAL[]         = "_interval";
constexpr const char AGGREGATION_RANGES[]           = "_ranges";

constexpr const char AGGREGATION_COUNT[]            = "_count";
constexpr const char AGGREGATION_CARDINALITY[]      = "_cardinality";
constexpr const char AGGREGATION_SUM[]              = "_sum";
constexpr const char AGGREGATION_AVG[]              = "_avg";
constexpr const char AGGREGATION_MIN[]              = "_min";
constexpr const char AGGREGATION_MAX[]              = "_max";
constexpr const char AGGREGATION_VARIANCE[]         = "_variance";
constexpr const char AGGREGATION_STD[]              = "_std";
constexpr const char AGGREGATION_MEDIAN[]           = "_median";
constexpr const char AGGREGATION_MODE[]             = "_mode";
constexpr const char AGGREGATION_STATS[]            = "_stats";
constexpr const char AGGREGATION_EXT_STATS[]        = "_extended_stats";
constexpr const char AGGREGATION_GEO_BOUNDS[]       = "_geo_bounds";
constexpr const char AGGREGATION_GEO_CENTROID[]     = "_geo_centroid";
constexpr const char AGGREGATION_PERCENTILES[]      = "_percentiles";
constexpr const char AGGREGATION_PERCENTILES_RANK[] = "_percentiles_rank";
constexpr const char AGGREGATION_SCRIPTED_METRIC[]  = "_scripted_metric";

constexpr const char AGGREGATION_TERM[]             = "_term";
constexpr const char AGGREGATION_FILTER[]           = "_filter";
constexpr const char AGGREGATION_VALUE[]            = "_value";
constexpr const char AGGREGATION_DATE_HISTOGRAM[]   = "_date_histogram";
constexpr const char AGGREGATION_DATE_RANGE[]       = "_date_range";
constexpr const char AGGREGATION_GEO_DISTANCE[]     = "_geo_distance";
constexpr const char AGGREGATION_GEO_TRIXELS[]      = "_geo_trixels";
constexpr const char AGGREGATION_HISTOGRAM[]        = "_histogram";
constexpr const char AGGREGATION_MISSING[]          = "_missing";
constexpr const char AGGREGATION_RANGE[]            = "_range";
constexpr const char AGGREGATION_IP_RANGE[]         = "_ip_range";
constexpr const char AGGREGATION_GEO_IP[]           = "_geo_ip";

class SubAggregation;


using func_value_handle = void (SubAggregation::*)(const std::string&, const Xapian::Document&);


class ValueHandle {
protected:
	Xapian::valueno _slot;
	func_value_handle _func;

public:
	ValueHandle() = default;

	inline void set(Xapian::valueno slot, func_value_handle func) {
		_slot = slot;
		_func = func;
	}

	void operator()(SubAggregation* agg, const Xapian::Document& doc) const;
};



class SubAggregation {
protected:
	MsgPack& _result;

public:
	SubAggregation(MsgPack& result)
		: _result(result) { }

	virtual void aggregate_float(double, const Xapian::Document&) {
		throw MSG_AggregationError("float type is not supported");
	}

	virtual void aggregate_integer(long, const Xapian::Document&) {
		throw MSG_AggregationError("integer type is not supported");
	}

	virtual void aggregate_positive(unsigned long, const Xapian::Document&) {
		throw MSG_AggregationError("positive type is not supported");
	}

	virtual void aggregate_date(double, const Xapian::Document&) {
		throw MSG_AggregationError("date type is not supported");
	}

	virtual void aggregate_boolean(bool, const Xapian::Document&) {
		throw MSG_AggregationError("boolean type is not supported");
	}

	virtual void aggregate_string(const std::string&, const Xapian::Document&) {
		throw MSG_AggregationError("string type is not supported");
	}

	virtual void aggregate_geo(const std::pair<std::string, std::string>&, const Xapian::Document&) {
		throw MSG_AggregationError("geo type is not supported");
	}

	virtual void aggregate_uuid(const std::string&, const Xapian::Document&) {
		throw MSG_AggregationError("uuid type is not supported");
	}

	void _aggregate_float(const std::string& s, const Xapian::Document& doc) {
		StringList l;
		l.unserialise(s);
		for (const auto& value : l) {
			aggregate_float(Unserialise::_float(value), doc);
		}
	}

	void _aggregate_integer(const std::string& s, const Xapian::Document& doc) {
		StringList l;
		l.unserialise(s);
		for (const auto& value : l) {
			aggregate_integer(Unserialise::integer(value), doc);
		}
	}

	void _aggregate_positive(const std::string& s, const Xapian::Document& doc) {
		StringList l;
		l.unserialise(s);
		for (const auto& value : l) {
			aggregate_positive(Unserialise::positive(value), doc);
		}
	}

	void _aggregate_date(const std::string& s, const Xapian::Document& doc) {
		StringList l;
		l.unserialise(s);
		for (const auto& value : l) {
			aggregate_date(Unserialise::timestamp(value), doc);
		}
	}

	void _aggregate_boolean(const std::string& s, const Xapian::Document& doc)  {
		StringList l;
		l.unserialise(s);
		for (const auto& value : l) {
			aggregate_boolean(Unserialise::boolean(value), doc);
		}
	}

	void _aggregate_string(const std::string& s, const Xapian::Document& doc) {
		StringList l;
		l.unserialise(s);
		for (const auto& value : l) {
			aggregate_string(value, doc);
		}
	}

	void _aggregate_geo(const std::string& s, const Xapian::Document& doc) {
		StringList l;
		l.unserialise(s);
		for (const auto& value : l) {
			aggregate_geo(Unserialise::geo(value), doc);
		}
	}

	void _aggregate_uuid(const std::string& s, const Xapian::Document& doc) {
		StringList l;
		l.unserialise(s);
		for (const auto& value : l) {
			aggregate_uuid(Unserialise::uuid(value), doc);
		}
	}

	virtual void operator()(const Xapian::Document&) = 0;
	virtual void update() = 0;
};


class HandledSubAggregation : public SubAggregation {
protected:
	ValueHandle _handle;

	HandledSubAggregation(MsgPack& result)
		: SubAggregation(result) { }

public:
	HandledSubAggregation(MsgPack& result, const MsgPack& conf, const std::shared_ptr<Schema>& schema);

	void operator()(const Xapian::Document& doc) override {
		_handle(this, doc);
	}
};


class MetricCount : public HandledSubAggregation {
protected:
	size_t _count;

public:
	MetricCount(MsgPack& result, const MsgPack& conf, const std::shared_ptr<Schema>& schema)
		: HandledSubAggregation(result, conf, schema),
		  _count(0) { }

	void update() override {
		_result[AGGREGATION_COUNT] = _count;
	}

	void _aggregate() {
		++_count;
	}

	void aggregate_float(double, const Xapian::Document&) override {
		_aggregate();
	}

	void aggregate_integer(long, const Xapian::Document&) override {
		_aggregate();
	}

	void aggregate_positive(unsigned long, const Xapian::Document&) override {
		_aggregate();
	}

	void aggregate_date(double, const Xapian::Document&) override {
		_aggregate();
	}

	void aggregate_boolean(bool, const Xapian::Document&) override {
		_aggregate();
	}

	void aggregate_string(const std::string&, const Xapian::Document&) override {
		_aggregate();
	}

	void aggregate_geo(const std::pair<std::string, std::string>&, const Xapian::Document&) override {
		_aggregate();
	}

	void aggregate_uuid(const std::string&, const Xapian::Document&) override {
		_aggregate();
	}
};


class MetricSum : public HandledSubAggregation {
protected:
	long double _sum;

public:
	MetricSum(MsgPack& result, const MsgPack& conf, const std::shared_ptr<Schema>& schema)
		: HandledSubAggregation(result, conf, schema),
		  _sum(0) { }

	void update() override {
		_result[AGGREGATION_SUM] = static_cast<double>(_sum);
	}

	void _aggregate(double value) {
		_sum += value;
	}

	void aggregate_float(double value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_integer(long value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_positive(unsigned long value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_date(double value, const Xapian::Document&) override {
		_aggregate(value);
	}
};


class MetricAvg : public MetricSum {
protected:
	size_t _count;

public:
	MetricAvg(MsgPack& result, const MsgPack& conf, const std::shared_ptr<Schema>& schema)
		: MetricSum(result, conf, schema),
		  _count(0) { }

	void update() override {
		_result[AGGREGATION_AVG] = static_cast<double>(avg());
	}

	void _aggregate(double value) {
		++_count;
		_sum += value;
	}

	void aggregate_float(double value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_integer(long value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_positive(unsigned long value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_date(double value, const Xapian::Document&) override {
		_aggregate(value);
	}

	inline long double avg() const {
		return _sum ? _sum / _count : _sum;
	}
};


class MetricExtendedStats;
class MetricStats;


class MetricMin : public HandledSubAggregation {
	friend class MetricStats;
	friend class MetricExtendedStats;

protected:
	double _min;

	MetricMin(MsgPack& result)
		: HandledSubAggregation(result),
		  _min(DBL_MAX) { }

public:
	MetricMin(MsgPack& result, const MsgPack& conf, const std::shared_ptr<Schema>& schema)
		: HandledSubAggregation(result, conf, schema),
		  _min(DBL_MAX) { }

	void update() override {
		_result[AGGREGATION_MIN] = _min;
	}

	void _aggregate(double value) {
		if (value < _min) {
			_min = value;
		}
	}

	void aggregate_float(double value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_integer(long value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_positive(unsigned long value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_date(double value, const Xapian::Document&) override {
		_aggregate(value);
	}
};


class MetricMax : public HandledSubAggregation {
	friend class MetricStats;
	friend class MetricExtendedStats;

protected:
	double _max;

	MetricMax(MsgPack& result)
		: HandledSubAggregation(result),
		  _max(DBL_MIN) { }

public:
	MetricMax(MsgPack& result, const MsgPack& conf, const std::shared_ptr<Schema>& schema)
		: HandledSubAggregation(result, conf, schema),
		  _max(DBL_MIN) { }

	void update() override {
		_result[AGGREGATION_MAX] = _max;
	}

	void _aggregate(double value) {
		if (value > _max) {
			_max = value;
		}
	}

	void aggregate_float(double value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_integer(long value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_positive(unsigned long value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_date(double value, const Xapian::Document&) override {
		_aggregate(value);
	}
};


class MetricVariance : public MetricAvg {
protected:
	long double _sq_sum;

public:
	MetricVariance(MsgPack& result, const MsgPack& conf, const std::shared_ptr<Schema>& schema)
		: MetricAvg(result, conf, schema),
		  _sq_sum(0) { }

	void update() override {
		_result[AGGREGATION_VARIANCE] = static_cast<double>(variance());
	}

	void _aggregate(double value) {
		++_count;
		_sum += value;
		_sq_sum += value * value;
	}

	void aggregate_float(double value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_integer(long value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_positive(unsigned long value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_date(double value, const Xapian::Document&) override {
		_aggregate(value);
	}

	inline long double variance() const {
		auto _avg = avg();
		return (_sq_sum - _count * _avg * _avg) / (_count - 1);
	}
};


// Standard deviation.
class MetricSTD : public MetricVariance {
public:
	MetricSTD(MsgPack& result, const MsgPack& conf, const std::shared_ptr<Schema>& schema)
		: MetricVariance(result, conf, schema) { }

	void update() override {
		_result[AGGREGATION_STD] = static_cast<double>(std());
	}

	inline long double std() const {
		return std::sqrt(variance());
	}
};


class MetricMedian : public HandledSubAggregation {
	std::vector<double> values;

public:
	MetricMedian(MsgPack& result, const MsgPack& conf, const std::shared_ptr<Schema>& schema)
		: HandledSubAggregation(result, conf, schema) { }

	void update() override {
		if (values.empty()) {
			_result[AGGREGATION_MEDIAN] = 0;
			return;
		}
		size_t median_pos = values.size();
		if (median_pos % 2 == 0) {
			median_pos /= 2;
			std::nth_element(values.begin(), values.begin() + median_pos, values.end());
			auto val1 = values[median_pos];
			std::nth_element(values.begin(), values.begin() + median_pos - 1, values.end());
			_result[AGGREGATION_MEDIAN] = (val1 + values[median_pos - 1]) / 2;
		} else {
			median_pos /= 2;
			std::nth_element(values.begin(), values.begin() + median_pos, values.end());
			_result[AGGREGATION_MEDIAN] = values[median_pos];
		}
	}

	void _aggregate(double value) {
		values.push_back(value);
	}

	void aggregate_float(double value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_integer(long value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_positive(unsigned long value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_date(double value, const Xapian::Document&) override {
		_aggregate(value);
	}
};


class MetricMode : public HandledSubAggregation {
	std::unordered_map<double, size_t> _histogram;

public:
	MetricMode(MsgPack& result, const MsgPack& conf, const std::shared_ptr<Schema>& schema)
		: HandledSubAggregation(result, conf, schema) { }

	void update() override {
		if (_histogram.empty()) {
			_result[AGGREGATION_MODE] = 0;
			return;
		}
		auto it = std::max_element(_histogram.begin(), _histogram.end(), [](const std::pair<double, size_t>& a, const std::pair<double, size_t>& b) { return a.second < b.second; });
		_result[AGGREGATION_MODE] = it->first;
	}

	void _aggregate(double value) {
		try {
			++_histogram.at(value);
		} catch (const std::out_of_range&) {
			_histogram[value] = 1;
		}
	}

	void aggregate_float(double value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_integer(long value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_positive(unsigned long value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_date(double value, const Xapian::Document&) override {
		_aggregate(value);
	}
};


class MetricStats : public MetricAvg {
protected:
	MetricMin _min_metric;
	MetricMax _max_metric;

public:
	MetricStats(MsgPack& result, const MsgPack& conf, const std::shared_ptr<Schema>& schema)
		: MetricAvg(result, conf, schema),
		  _min_metric(_result, conf, schema),
		  _max_metric(_result, conf, schema) { }

	void update() override {
		_result[AGGREGATION_COUNT] = _count;
		_result[AGGREGATION_MIN]   = _min_metric._min;
		_result[AGGREGATION_MAX]   = _max_metric._max;
		_result[AGGREGATION_AVG]   = static_cast<double>(avg());
		_result[AGGREGATION_SUM]   = static_cast<double>(_sum);
	}

	void _aggregate(double value) {
		_min_metric._aggregate(value);
		_max_metric._aggregate(value);
		MetricAvg::_aggregate(value);
	}

	void aggregate_float(double value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_integer(long value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_positive(unsigned long value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_date(double value, const Xapian::Document&) override {
		_aggregate(value);
	}
};


class MetricExtendedStats : public MetricSTD {
protected:
	MetricMin _min_metric;
	MetricMax _max_metric;

public:
	MetricExtendedStats(MsgPack& result, const MsgPack& conf, const std::shared_ptr<Schema>& schema)
		: MetricSTD(result, conf, schema),
		  _min_metric(_result, conf, schema),
		  _max_metric(_result, conf, schema) { }

	void update() override {
		_result[AGGREGATION_COUNT]      = _count;
		_result[AGGREGATION_MIN]        = _min_metric._min;
		_result[AGGREGATION_MAX]        = _max_metric._max;
		_result[AGGREGATION_AVG]        = static_cast<double>(avg());
		_result[AGGREGATION_SUM]        = static_cast<double>(_sum);
		_result[AGGREGATION_SUM_OF_SQ]  = static_cast<double>(_sq_sum);
		_result[AGGREGATION_VARIANCE]   = static_cast<double>(variance());
		_result[AGGREGATION_STD]        = static_cast<double>(std());
	}

	void _aggregate(double value) {
		_min_metric._aggregate(value);
		_max_metric._aggregate(value);
		MetricSTD::_aggregate(value);
	}

	void aggregate_float(double value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_integer(long value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_positive(unsigned long value, const Xapian::Document&) override {
		_aggregate(value);
	}

	void aggregate_date(double value, const Xapian::Document&) override {
		_aggregate(value);
	}
};
