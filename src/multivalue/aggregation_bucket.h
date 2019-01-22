/*
 * Copyright (C) 2015-2019 Dubalu LLC. All rights reserved.
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

#include <algorithm>                        // for std::sort, std::sort_heap
#include <cstdint>                          // for int64_t, uint64_t
#include <limits>                           // for std::numeric_limits
#include <map>                              // for std::map
#include <math.h>                           // for fmodl
#include <memory>                           // for std::shared_ptr
#include <string>                           // for std::string
#include <sys/types.h>                      // for int64_t, uint64_t
#include <tuple>                            // for std::forward_as_tuple
#include <utility>                          // for std::pair, std::make_pair
#include <vector>                           // for std::vector
#include <xapian.h>                         // for Document, valueno

#include "aggregation.h"                    // for Aggregation
#include "msgpack.h"                        // for MsgPack, object::object, ...
#include "aggregation_metric.h"             // for AGGREGATION_INTERVAL, AGG...
#include "exception.h"                      // for AggregationError, MSG_Agg...
#include "schema.h"                         // for FieldType
#include "serialise.h"                      // for _float
#include "string.hh"                        // for string::format, string::Number
#include "hashes.hh"                        // for xxh64

class Schema;


template <typename Handler>
class BucketAggregation : public HandledSubAggregation<Handler> {
protected:
	std::map<std::string, Aggregation> _aggs;
	const std::shared_ptr<Schema> _schema;
	const MsgPack& _context;
	Split<std::string_view> _field;
	enum class Sort {
		by_key_asc,
		by_key_desc,
		by_count_asc,
		by_count_desc,
		by_field_asc,
		by_field_desc,
	} _sort;
	size_t _limit;
	size_t _min_doc_count;

private:
	friend struct CmpByKeyAsc;
	friend struct CmpByKeyDesc;
	friend struct CmpByCountAsc;
	friend struct CmpByCountDesc;
	friend struct CmpByFieldAsc;
	friend struct CmpByFieldDesc;

	struct CmpByKeyAsc {
		BucketAggregation<Handler>& _agg;

		CmpByKeyAsc(BucketAggregation<Handler>& agg)
			: _agg(agg) { }

		bool operator()(const std::map<std::string, Aggregation>::iterator& a, const std::map<std::string, Aggregation>::iterator& b) const {
			if (a->first > b->first) return false;
			return true;
		}
	};

	struct CmpByKeyDesc {
		BucketAggregation<Handler>& _agg;

		CmpByKeyDesc(BucketAggregation<Handler>& agg)
			: _agg(agg) { }

		bool operator()(const std::map<std::string, Aggregation>::iterator& a, const std::map<std::string, Aggregation>::iterator& b) const {
			if (a->first < b->first) return false;
			return true;
		}
	};

	struct CmpByCountAsc {
		BucketAggregation<Handler>& _agg;

		CmpByCountAsc(BucketAggregation<Handler>& agg)
			: _agg(agg) { }

		bool operator()(const std::map<std::string, Aggregation>::iterator& a, const std::map<std::string, Aggregation>::iterator& b) const {
			if (a->second.doc_count() < b->second.doc_count()) return true;
			if (a->second.doc_count() > b->second.doc_count()) return false;
			if (a->first > b->first) return false;
			return true;
		}
	};

	struct CmpByCountDesc {
		BucketAggregation<Handler>& _agg;

		CmpByCountDesc(BucketAggregation<Handler>& agg)
			: _agg(agg) { }

		bool operator()(const std::map<std::string, Aggregation>::iterator& a, const std::map<std::string, Aggregation>::iterator& b) const {
			if (a->second.doc_count() > b->second.doc_count()) return true;
			if (a->second.doc_count() < b->second.doc_count()) return false;
			if (a->first < b->first) return false;
			return true;
		}
	};

	struct CmpByFieldAsc {
		BucketAggregation<Handler>& _agg;

		CmpByFieldAsc(BucketAggregation<Handler>& agg)
			: _agg(agg) { }

		bool operator()(const std::map<std::string, Aggregation>::iterator& a, const std::map<std::string, Aggregation>::iterator& b) const {
			for (const auto& f : _agg._field) {
				L_BLUE("  %s", repr(f));
			}

			if (a->second.doc_count() < b->second.doc_count()) return true;
			if (a->second.doc_count() > b->second.doc_count()) return false;
			if (a->first > b->first) return false;
			return true;
		}
	};

	struct CmpByFieldDesc {
		BucketAggregation<Handler>& _agg;

		CmpByFieldDesc(BucketAggregation<Handler>& agg)
			: _agg(agg) { }

		bool operator()(const std::map<std::string, Aggregation>::iterator& a, const std::map<std::string, Aggregation>::iterator& b) const {
			for (const auto& f : _agg._field) {
				L_BLUE("  %s", repr(f));
			}

			if (a->second.doc_count() > b->second.doc_count()) return true;
			if (a->second.doc_count() < b->second.doc_count()) return false;
			if (a->first < b->first) return false;
			return true;
		}
	};


	template <typename Cmp>
	MsgPack _get_aggregation() {
		Cmp cmp(*this);
		bool is_heap = false;
		std::vector<std::map<std::string, Aggregation>::iterator> ordered;
		for (auto it = _aggs.begin(); it != _aggs.end(); ++it) {
			if (it->second.doc_count() >= _min_doc_count) {
				it->second.update();
				ordered.push_back(it);
				if (ordered.size() > _limit) {
					if (is_heap) {
						std::push_heap(ordered.begin(), ordered.end(), cmp);
					} else {
						std::make_heap(ordered.begin(), ordered.end(), cmp);
						is_heap = true;
					}
					std::pop_heap(ordered.begin(), ordered.end(), cmp);
					ordered.pop_back();
				}
			}
		}
		if (is_heap) {
			std::sort_heap(ordered.begin(), ordered.end(), cmp);
		} else {
			std::sort(ordered.begin(), ordered.end(), cmp);
		}

		MsgPack result(MsgPack::Type::MAP);
		for (auto& agg : ordered) {
			result[agg->first] = agg->second.get_aggregation();
		}
		return result;
	}

	Sort _conf_sort() {
		const auto& conf = this->_conf;
		auto it = conf.find(AGGREGATION_SORT);
		if (it != conf.end()) {
			const auto& value = it.value();
			switch (value.getType()) {
				case MsgPack::Type::STR: {
					auto str = value.str_view();
					if (str == AGGREGATION_DOC_COUNT) {
						return Sort::by_count_asc;
					}
					if (str == AGGREGATION_KEY) {
						return Sort::by_key_asc;
					}
					return Sort::by_field_asc;
				}

				case MsgPack::Type::MAP: {
					it = value.find(AGGREGATION_DOC_COUNT);
					if (it != value.end()) {
						const auto& sorter = it.value();
						switch (sorter.getType()) {
							case MsgPack::Type::STR: {
								auto order_str = sorter.str_view();
								if (order_str == "desc") {
									return Sort::by_count_desc;
								}
								if (order_str == "asc") {
									return Sort::by_count_asc;
								}
								THROW(AggregationError, "'%s.%s' must use either 'desc' or 'asc'", AGGREGATION_SORT, AGGREGATION_DOC_COUNT);
							}
							case MsgPack::Type::MAP: {
								it = sorter.find(AGGREGATION_ORDER);
								if (it != sorter.end()) {
									const auto& order = it.value();
									switch (order.getType()) {
										case MsgPack::Type::STR: {
											auto order_str = order.str_view();
											if (order_str == "desc") {
												return Sort::by_count_desc;
											}
											if (order_str == "asc") {
												return Sort::by_count_asc;
											}
											THROW(AggregationError, "'%s.%s.%s' must be either 'desc' or 'asc'", AGGREGATION_SORT, AGGREGATION_DOC_COUNT, AGGREGATION_ORDER);
										}
										default:
											THROW(AggregationError, "'%s.%s.%s' must be a string", AGGREGATION_SORT, AGGREGATION_DOC_COUNT, AGGREGATION_ORDER);
									}
									break;
								}
								THROW(AggregationError, "'%s.%s' must contain '%s'", AGGREGATION_SORT, AGGREGATION_DOC_COUNT, AGGREGATION_ORDER);
							}
							default:
								THROW(AggregationError, "'%s.%s' must be a string or an object", AGGREGATION_SORT, AGGREGATION_DOC_COUNT);
						}
					}

					it = value.find(AGGREGATION_KEY);
					if (it != value.end()) {
						const auto& sorter = it.value();
						switch (sorter.getType()) {
							case MsgPack::Type::STR: {
								auto order_str = sorter.str_view();
								if (order_str == "desc") {
									return Sort::by_key_desc;
								}
								if (order_str == "asc") {
									return Sort::by_key_asc;
								}
								THROW(AggregationError, "'%s.%s' must use either 'desc' or 'asc'", AGGREGATION_SORT, AGGREGATION_KEY);
							}
							case MsgPack::Type::MAP: {
								it = sorter.find(AGGREGATION_ORDER);
								if (it != sorter.end()) {
									const auto& order = it.value();
									switch (order.getType()) {
										case MsgPack::Type::STR: {
											auto order_str = order.str_view();
											if (order_str == "desc") {
												return Sort::by_key_desc;
											}
											if (order_str == "asc") {
												return Sort::by_key_asc;
											}
											THROW(AggregationError, "'%s.%s.%s' must be either 'desc' or 'asc'", AGGREGATION_SORT, AGGREGATION_KEY, AGGREGATION_ORDER);
										}
										default:
											THROW(AggregationError, "'%s.%s.%s' must be a string", AGGREGATION_SORT, AGGREGATION_KEY, AGGREGATION_ORDER);
									}
									break;
								}
								THROW(AggregationError, "'%s.%s' must contain '%s'", AGGREGATION_SORT, AGGREGATION_KEY, AGGREGATION_ORDER);
							}
							default:
								THROW(AggregationError, "'%s.%s' must be a string or an object", AGGREGATION_SORT, AGGREGATION_KEY);
						}
					}

					it = value.begin();
					if (it != value.end()) {
						const auto& field = it->str_view();
						const auto& sorter = it.value();
						switch (sorter.getType()) {
							case MsgPack::Type::STR: {
								auto order_str = sorter.str_view();
								if (order_str == "desc") {
									_field = Split<std::string_view>(field, '.');
									return Sort::by_field_desc;
								}
								if (order_str == "asc") {
									_field = Split<std::string_view>(field, '.');
									return Sort::by_field_asc;
								}
								THROW(AggregationError, "'%s.%s' must use either 'desc' or 'asc'", AGGREGATION_SORT, field);
							}
							case MsgPack::Type::MAP: {
								it = sorter.find(AGGREGATION_ORDER);
								if (it != sorter.end()) {
									const auto& order = it.value();
									switch (order.getType()) {
										case MsgPack::Type::STR: {
											auto order_str = order.str_view();
											if (order_str == "desc") {
												_field = Split<std::string_view>(field, '.');
												return Sort::by_field_desc;
											}
											if (order_str == "asc") {
												_field = Split<std::string_view>(field, '.');
												return Sort::by_field_asc;
											}
											THROW(AggregationError, "'%s.%s.%s' must be either 'desc' or 'asc'", AGGREGATION_SORT, field, AGGREGATION_ORDER);
										}
										default:
											THROW(AggregationError, "'%s.%s.%s' must be a string", AGGREGATION_SORT, field, AGGREGATION_ORDER);
									}
									break;
								}
								THROW(AggregationError, "'%s.%s' must contain '%s'", AGGREGATION_SORT, field, AGGREGATION_ORDER);
							}
							default:
								THROW(AggregationError, "'%s.%s' must be a string or an object", AGGREGATION_SORT, field);
						}
					}

					THROW(AggregationError, "'%s' must contain a field name", AGGREGATION_SORT);
				}

				default:
					THROW(AggregationError, "'%s' must be a string or an object", AGGREGATION_SORT);
			}
		}

		return Sort::by_count_desc;
	}

	size_t _conf_limit() {
		const auto& conf = this->_conf;
		auto it = conf.find(AGGREGATION_LIMIT);
		if (it != conf.end()) {
			const auto& value = it.value();
			switch (value.getType()) {
				case MsgPack::Type::POSITIVE_INTEGER:
				case MsgPack::Type::NEGATIVE_INTEGER: {
					auto val = value.as_i64();
					if (val >= 0) {
						return val;
					}
				}
				default:
					THROW(AggregationError, "'%s' must be a positive integer", AGGREGATION_LIMIT);
			}
		}

		return 10;
	}

	size_t _conf_min_doc_count() {
		const auto& conf = this->_conf;
		auto it = conf.find(AGGREGATION_MIN_DOC_COUNT);
		if (it != conf.end()) {
			const auto& value = it.value();
			switch (value.getType()) {
				case MsgPack::Type::POSITIVE_INTEGER:
				case MsgPack::Type::NEGATIVE_INTEGER: {
					auto val = value.as_i64();
					if (val >= 0) {
						return val;
					}
				}
				default:
					THROW(AggregationError, "'%s' must be a positive number", AGGREGATION_MIN_DOC_COUNT);
			}
		}

		return 1;
	}

public:
	BucketAggregation(const MsgPack& context, std::string_view name, const std::shared_ptr<Schema>& schema)
		: HandledSubAggregation<Handler>(context, name, schema),
		  _schema(schema),
		  _context(context),
		  _sort(_conf_sort()),
		  _limit(_conf_limit()),
		  _min_doc_count(_conf_min_doc_count()) { }

	MsgPack get_aggregation() override {
		switch (_sort) {
			case Sort::by_key_asc:
				return _get_aggregation<CmpByKeyAsc>();
			case Sort::by_key_desc:
				return _get_aggregation<CmpByKeyDesc>();
			case Sort::by_count_asc:
				return _get_aggregation<CmpByCountAsc>();
			case Sort::by_count_desc:
				return _get_aggregation<CmpByCountDesc>();
			case Sort::by_field_asc:
				return _get_aggregation<CmpByFieldAsc>();
			case Sort::by_field_desc:
				return _get_aggregation<CmpByFieldDesc>();
		}
	}

	auto& add(std::string_view bucket) {
		auto it = _aggs.find(std::string(bucket));  // FIXME: This copies bucket as std::map cannot find std::string_view directly!
		if (it != _aggs.end()) {
			return it->second;
		}
		auto emplaced = _aggs.emplace(std::piecewise_construct,
			std::forward_as_tuple(bucket),
			std::forward_as_tuple(_context, _schema));
		return emplaced.first->second;
	}

	void aggregate(std::string_view bucket, const Xapian::Document& doc) {
		add(bucket)(doc);
	}
};


class ValuesAggregation : public BucketAggregation<ValuesHandler> {
public:
	ValuesAggregation(const MsgPack& context, std::string_view name, const std::shared_ptr<Schema>& schema)
		: BucketAggregation<ValuesHandler>(context, name, schema) { }

	void aggregate_float(long double value, const Xapian::Document& doc) override {
		aggregate(string::Number(value), doc);
	}

	void aggregate_integer(int64_t value, const Xapian::Document& doc) override {
		aggregate(string::Number(value), doc);
	}

	void aggregate_positive(uint64_t value, const Xapian::Document& doc) override {
		aggregate(string::Number(value), doc);
	}

	void aggregate_date(double value, const Xapian::Document& doc) override {
		aggregate(string::Number(value), doc);
	}

	void aggregate_time(double value, const Xapian::Document& doc) override {
		aggregate(string::Number(value), doc);
	}

	void aggregate_timedelta(double value, const Xapian::Document& doc) override {
		aggregate(string::Number(value), doc);
	}

	void aggregate_boolean(bool value, const Xapian::Document& doc) override {
		aggregate(std::string_view(value ? "true" : "false"), doc);
	}

	void aggregate_string(std::string_view value, const Xapian::Document& doc) override {
		aggregate(value, doc);
	}

	void aggregate_geo(const range_t& value, const Xapian::Document& doc) override {
		aggregate(value.to_string(), doc);
	}

	void aggregate_uuid(std::string_view value, const Xapian::Document& doc) override {
		aggregate(value, doc);
	}
};


class TermsAggregation : public BucketAggregation<TermsHandler> {
public:
	TermsAggregation(const MsgPack& context, std::string_view name, const std::shared_ptr<Schema>& schema)
		: BucketAggregation<TermsHandler>(context, name, schema) { }

	void aggregate_float(long double value, const Xapian::Document& doc) override {
		aggregate(string::Number(value), doc);
	}

	void aggregate_integer(int64_t value, const Xapian::Document& doc) override {
		aggregate(string::Number(value), doc);
	}

	void aggregate_positive(uint64_t value, const Xapian::Document& doc) override {
		aggregate(string::Number(value), doc);
	}

	void aggregate_date(double value, const Xapian::Document& doc) override {
		aggregate(string::Number(value), doc);
	}

	void aggregate_time(double value, const Xapian::Document& doc) override {
		aggregate(string::Number(value), doc);
	}

	void aggregate_timedelta(double value, const Xapian::Document& doc) override {
		aggregate(string::Number(value), doc);
	}

	void aggregate_boolean(bool value, const Xapian::Document& doc) override {
		aggregate(std::string_view(value ? "true" : "false"), doc);
	}

	void aggregate_string(std::string_view value, const Xapian::Document& doc) override {
		aggregate(value, doc);
	}

	void aggregate_geo(const range_t& value, const Xapian::Document& doc) override {
		aggregate(value.to_string(), doc);
	}

	void aggregate_uuid(std::string_view value, const Xapian::Document& doc) override {
		aggregate(value, doc);
	}
};


class HistogramAggregation : public BucketAggregation<ValuesHandler> {
	uint64_t interval_u64;
	int64_t interval_i64;
	long double interval_f64;

	auto get_bucket(uint64_t value) {
		if (!interval_u64) {
			THROW(AggregationError, "'%s' must be a non-zero number", AGGREGATION_INTERVAL);
		}
		auto rem = value % interval_u64;
		return value - rem;
	}

	auto get_bucket(int64_t value) {
		if (!interval_i64) {
			THROW(AggregationError, "'%s' must be a non-zero number", AGGREGATION_INTERVAL);
		}
		auto rem = value % interval_i64;
		if (rem < 0) {
			rem += interval_i64;
		}
		return value - rem;
	}

	auto get_bucket(long double value) {
		if (!interval_f64) {
			THROW(AggregationError, "'%s' must be a non-zero number", AGGREGATION_INTERVAL);
		}
		auto rem = fmodl(value, interval_f64);
		if (rem < 0) {
			rem += interval_f64;
		}
		return value - rem;
	}

	void configure_u64(const MsgPack& interval_value) {
		switch (interval_value.getType()) {
			case MsgPack::Type::POSITIVE_INTEGER:
			case MsgPack::Type::NEGATIVE_INTEGER:
			case MsgPack::Type::FLOAT:
				interval_u64 = interval_value.as_u64();
				break;
			default:
				THROW(AggregationError, "'%s' must be a number", AGGREGATION_INTERVAL);
		}
	}

	void configure_i64(const MsgPack& interval_value) {
		switch (interval_value.getType()) {
			case MsgPack::Type::POSITIVE_INTEGER:
			case MsgPack::Type::NEGATIVE_INTEGER:
			case MsgPack::Type::FLOAT:
				interval_i64 = interval_value.as_i64();
				break;
			default:
				THROW(AggregationError, "'%s' must be a number", AGGREGATION_INTERVAL);
		}
	}

	void configure_f64(const MsgPack& interval_value) {
		switch (interval_value.getType()) {
			case MsgPack::Type::POSITIVE_INTEGER:
			case MsgPack::Type::NEGATIVE_INTEGER:
			case MsgPack::Type::FLOAT:
				interval_f64 = interval_value.as_f64();
				break;
			default:
				THROW(AggregationError, "'%s' must be a number", AGGREGATION_INTERVAL);
		}
	}

public:
	HistogramAggregation(const MsgPack& context, std::string_view name, const std::shared_ptr<Schema>& schema)
		: BucketAggregation<ValuesHandler>(context, name, schema),
		  interval_u64(0),
		  interval_i64(0),
		  interval_f64(0.0) {
		const auto it = _conf.find(AGGREGATION_INTERVAL);
		if (it == _conf.end()) {
			THROW(AggregationError, "'%s' must be object with '%s'", name, AGGREGATION_INTERVAL);
		}
		const auto& interval_value = it.value();
		switch (_handler.get_type()) {
			case FieldType::POSITIVE:
				configure_u64(interval_value);
				break;
			case FieldType::INTEGER:
				configure_i64(interval_value);
				break;
			case FieldType::FLOAT:
			case FieldType::DATE:
			case FieldType::TIME:
			case FieldType::TIMEDELTA:
				configure_f64(interval_value);
				break;
			default:
				THROW(AggregationError, "Histogram aggregation can work only on numeric fields");
		}
	}

	void aggregate_float(long double value, const Xapian::Document& doc) override {
		auto bucket = get_bucket(value);
		aggregate(string::Number(bucket), doc);
	}

	void aggregate_integer(int64_t value, const Xapian::Document& doc) override {
		auto bucket = get_bucket(value);
		aggregate(string::Number(bucket), doc);
	}

	void aggregate_positive(uint64_t value, const Xapian::Document& doc) override {
		auto bucket = get_bucket(value);
		aggregate(string::Number(bucket), doc);
	}

	void aggregate_date(double value, const Xapian::Document& doc) override {
		auto bucket = get_bucket(static_cast<long double>(value));
		aggregate(string::Number(bucket), doc);
	}

	void aggregate_time(double value, const Xapian::Document& doc) override {
		auto bucket = get_bucket(static_cast<long double>(value));
		aggregate(string::Number(bucket), doc);
	}

	void aggregate_timedelta(double value, const Xapian::Document& doc) override {
		auto bucket = get_bucket(static_cast<long double>(value));
		aggregate(string::Number(bucket), doc);
	}
};


class RangeAggregation : public BucketAggregation<ValuesHandler> {
	std::vector<std::pair<std::string, std::pair<uint64_t, uint64_t>>> ranges_u64;
	std::vector<std::pair<std::string, std::pair<int64_t, int64_t>>> ranges_i64;
	std::vector<std::pair<std::string, std::pair<long double, long double>>> ranges_f64;

	template <typename T>
	std::string _as_bucket(T start, T end) const {
		if (end == std::numeric_limits<T>::max()) {
			if (start == std::numeric_limits<T>::min()) {
				return "..";
			}
			return string::format("%s..", string::Number(start));
		}
		if (start == std::numeric_limits<T>::min()) {
			if (end == std::numeric_limits<T>::max()) {
				return "..";
			}
			return string::format("..%s", string::Number(end));
		}
		return string::format("%s..%s", string::Number(start), string::Number(end));
	}

	void configure_u64(const MsgPack& ranges) {
		for (const auto& range : ranges) {
			std::string default_key;
			std::string_view key;
			auto key_it = range.find(AGGREGATION_KEY);
			if (key_it != range.end()) {
				const auto& key_value = key_it.value();
				if (!key_value.is_string()) {
					THROW(AggregationError, "'%s' must be a string", AGGREGATION_KEY);
				}
				key = key_value.str_view();
			}

			long double from_u64 = std::numeric_limits<long double>::min();
			auto from_it = range.find(AGGREGATION_FROM);
			if (from_it != range.end()) {
				const auto& from_value = from_it.value();
				switch (from_value.getType()) {
					case MsgPack::Type::POSITIVE_INTEGER:
					case MsgPack::Type::NEGATIVE_INTEGER:
					case MsgPack::Type::FLOAT:
						from_u64 = from_value.as_u64();
						break;
					default:
						THROW(AggregationError, "'%s' must be a number", AGGREGATION_FROM);
				}
			}

			long double to_u64 = std::numeric_limits<long double>::max();
			auto to_it = range.find(AGGREGATION_TO);
			if (to_it != range.end()) {
				const auto& to_value = to_it.value();
				switch (to_value.getType()) {
					case MsgPack::Type::POSITIVE_INTEGER:
					case MsgPack::Type::NEGATIVE_INTEGER:
					case MsgPack::Type::FLOAT:
						to_u64 = to_value.as_u64();
						break;
					default:
						THROW(AggregationError, "'%s' must be a number", AGGREGATION_TO);
				}
			}

			if (key.empty()) {
				default_key = _as_bucket(from_u64, to_u64);
				key = default_key;
			}
			ranges_u64.emplace_back(key, std::make_pair(from_u64, to_u64));
		}
	}

	void configure_i64(const MsgPack& ranges) {
		for (const auto& range : ranges) {
			std::string default_key;
			std::string_view key;
			auto key_it = range.find(AGGREGATION_KEY);
			if (key_it != range.end()) {
				const auto& key_value = key_it.value();
				if (!key_value.is_string()) {
					THROW(AggregationError, "'%s' must be a string", AGGREGATION_KEY);
				}
				key = key_value.str_view();
			}

			long double from_i64 = std::numeric_limits<long double>::min();
			auto from_it = range.find(AGGREGATION_FROM);
			if (from_it != range.end()) {
				const auto& from_value = from_it.value();
				switch (from_value.getType()) {
					case MsgPack::Type::POSITIVE_INTEGER:
					case MsgPack::Type::NEGATIVE_INTEGER:
					case MsgPack::Type::FLOAT:
						from_i64 = from_value.as_i64();
						break;
					default:
						THROW(AggregationError, "'%s' must be a number", AGGREGATION_FROM);
				}
			}

			long double to_i64 = std::numeric_limits<long double>::max();
			auto to_it = range.find(AGGREGATION_TO);
			if (to_it != range.end()) {
				const auto& to_value = to_it.value();
				switch (to_value.getType()) {
					case MsgPack::Type::POSITIVE_INTEGER:
					case MsgPack::Type::NEGATIVE_INTEGER:
					case MsgPack::Type::FLOAT:
						to_i64 = to_value.as_i64();
						break;
					default:
						THROW(AggregationError, "'%s' must be a number", AGGREGATION_TO);
				}
			}

			if (key.empty()) {
				default_key = _as_bucket(from_i64, to_i64);
				key = default_key;
			}
			ranges_i64.emplace_back(key, std::make_pair(from_i64, to_i64));
		}
	}

	void configure_f64(const MsgPack& ranges) {
		for (const auto& range : ranges) {
			std::string default_key;
			std::string_view key;
			auto key_it = range.find(AGGREGATION_KEY);
			if (key_it != range.end()) {
				const auto& key_value = key_it.value();
				if (!key_value.is_string()) {
					THROW(AggregationError, "'%s' must be a string", AGGREGATION_KEY);
				}
				key = key_value.str_view();
			}

			long double from_f64 = std::numeric_limits<long double>::min();
			auto from_it = range.find(AGGREGATION_FROM);
			if (from_it != range.end()) {
				const auto& from_value = from_it.value();
				switch (from_value.getType()) {
					case MsgPack::Type::POSITIVE_INTEGER:
					case MsgPack::Type::NEGATIVE_INTEGER:
					case MsgPack::Type::FLOAT:
						from_f64 = from_value.as_f64();
						break;
					default:
						THROW(AggregationError, "'%s' must be a number", AGGREGATION_FROM);
				}
			}

			long double to_f64 = std::numeric_limits<long double>::max();
			auto to_it = range.find(AGGREGATION_TO);
			if (to_it != range.end()) {
				const auto& to_value = to_it.value();
				switch (to_value.getType()) {
					case MsgPack::Type::POSITIVE_INTEGER:
					case MsgPack::Type::NEGATIVE_INTEGER:
					case MsgPack::Type::FLOAT:
						to_f64 = to_value.as_f64();
						break;
					default:
						THROW(AggregationError, "'%s' must be a number", AGGREGATION_TO);
				}
			}

			if (key.empty()) {
				default_key = _as_bucket(from_f64, to_f64);
				key = default_key;
			}
			ranges_f64.emplace_back(key, std::make_pair(from_f64, to_f64));
		}
	}

public:
	RangeAggregation(const MsgPack& context, std::string_view name, const std::shared_ptr<Schema>& schema)
		: BucketAggregation<ValuesHandler>(context, name, schema)
	{
		const auto it = _conf.find(AGGREGATION_RANGES);
		if (it == _conf.end()) {
			THROW(AggregationError, "'%s' must be object with '%s'", name, AGGREGATION_RANGES);
		}
		const auto& ranges = it.value();
		if (!ranges.is_array()) {
			THROW(AggregationError, "'%s.%s' must be an array", name, AGGREGATION_RANGES);
		}

		switch (_handler.get_type()) {
			case FieldType::POSITIVE:
				configure_u64(ranges);
				break;
			case FieldType::INTEGER:
				configure_i64(ranges);
				break;
			case FieldType::FLOAT:
			case FieldType::DATE:
			case FieldType::TIME:
			case FieldType::TIMEDELTA:
				configure_f64(ranges);
				break;
			default:
				THROW(AggregationError, "Range aggregation can work only on numeric fields");
		}
	}

	void aggregate_float(long double value, const Xapian::Document& doc) override {
		for (const auto& range : ranges_f64) {
			if (value >= range.second.first && value < range.second.second) {
				aggregate(range.first, doc);
			}
		}
	}

	void aggregate_integer(int64_t value, const Xapian::Document& doc) override {
		for (const auto& range : ranges_i64) {
			if (value >= range.second.first && value < range.second.second) {
				aggregate(range.first, doc);
			}
		}
	}

	void aggregate_positive(uint64_t value, const Xapian::Document& doc) override {
		for (const auto& range : ranges_u64) {
			if (value >= range.second.first && value < range.second.second) {
				aggregate(range.first, doc);
			}
		}
	}

	void aggregate_date(double value, const Xapian::Document& doc) override {
		for (const auto& range : ranges_f64) {
			if (value >= range.second.first && value < range.second.second) {
				aggregate(range.first, doc);
			}
		}
	}

	void aggregate_time(double value, const Xapian::Document& doc) override {
		for (const auto& range : ranges_f64) {
			if (value >= range.second.first && value < range.second.second) {
				aggregate(range.first, doc);
			}
		}
	}

	void aggregate_timedelta(double value, const Xapian::Document& doc) override {
		for (const auto& range : ranges_f64) {
			if (value >= range.second.first && value < range.second.second) {
				aggregate(range.first, doc);
			}
		}
	}
};


class FilterAggregation : public SubAggregation {
	using func_filter = void (FilterAggregation::*)(const Xapian::Document&);

	std::vector<std::pair<Xapian::valueno, std::set<std::string>>> _filters;
	Aggregation _agg;
	func_filter func;

public:
	FilterAggregation(const MsgPack& context, std::string_view name, const std::shared_ptr<Schema>& schema);

	void operator()(const Xapian::Document& doc) override {
		(this->*func)(doc);
	}

	MsgPack get_aggregation() override;

	void check_single(const Xapian::Document& doc);
	void check_multiple(const Xapian::Document& doc);
};
