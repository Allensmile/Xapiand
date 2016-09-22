/*
 * Copyright (C) 2015,2016 deipi.com LLC and contributors. All rights reserved.
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


#include "../geo/wkt_parser.h"
#include "../phonetic.h"
#include "../schema.h"
#include "../serialise.h"
#include "../string_metric.h"

#include <cfloat>
#include <cmath>
#include <cstdlib>


const std::string MAX_CMPVALUE(Serialise::_float(DBL_MAX));
const std::string STR_FOR_EMPTY("\xff");


class Multi_MultiValueKeyMaker;


using dispatch_str_metric = void (Multi_MultiValueKeyMaker::*)(const required_spc_t&, bool, const std::string&, const query_field_t&);


extern const dispatch_str_metric def_str_metric;
extern const dispatch_str_metric def_soundex_metric;
extern const std::unordered_map<std::string, dispatch_str_metric> map_dispatch_str_metric;
extern const std::unordered_map<std::string, dispatch_str_metric> map_dispatch_soundex_metric;


// Base class for create keys.
class BaseKey {
protected:
	Xapian::valueno _slot;
	bool _reverse;

public:
	BaseKey(Xapian::valueno slot, bool reverse)
		: _slot(slot),
		  _reverse(reverse) { }

	virtual std::string findSmallest(const Xapian::Document& doc) const;
	virtual std::string findLargest(const Xapian::Document& doc) const;
	virtual std::string get_cmpvalue(const std::string& serialise_val) const = 0;

	bool get_reverse() const {
		return _reverse;
	}
};


// Class for create the key from the serialized value.
class SerialiseKey : public BaseKey {
	std::string get_cmpvalue(const std::string&) const override {
		return STR_FOR_EMPTY;
	}

public:
	std::string findSmallest(const Xapian::Document& doc) const override;
	std::string findLargest(const Xapian::Document& doc) const override;

	SerialiseKey(Xapian::valueno slot, bool reverse)
		: BaseKey(slot, reverse) { }
};


// Class for create the key using as a reference a float value.
class FloatKey : public BaseKey {
	double _ref_val;

	std::string get_cmpvalue(const std::string& serialise_val) const override {
		return Serialise::_float(std::fabs(Unserialise::_float(serialise_val) - _ref_val));
	}

public:
	FloatKey(Xapian::valueno slot, bool reverse, const std::string& value)
		: BaseKey(slot, reverse),
		  _ref_val(strict(std::stod, value)) { }
};


// Class for create the key using as a reference a integer value.
class IntegerKey : public BaseKey {
	int64_t _ref_val;

	std::string get_cmpvalue(const std::string& serialise_val) const override {
		return Serialise::integer(std::llabs(Unserialise::integer(serialise_val) - _ref_val));
	}

public:
	IntegerKey(Xapian::valueno slot, bool reverse, const std::string& value)
		: BaseKey(slot, reverse),
		  _ref_val(strict(std::stoll, value)) { }
};


// Class for create the key using as a reference a positive value.
class PositiveKey : public BaseKey {
	uint64_t _ref_val;

	std::string get_cmpvalue(const std::string& serialise_val) const override {
		int64_t val = Unserialise::positive(serialise_val) - _ref_val;
		return Serialise::positive(std::llabs(val));
	}

public:
	PositiveKey(Xapian::valueno slot, bool reverse, const std::string& value)
		: BaseKey(slot, reverse),
		  _ref_val(strict(std::stoull, value)) { }
};


// Class for create the key using as a reference a date value.
class DateKey : public BaseKey {
	double _ref_val;

	std::string get_cmpvalue(const std::string& serialise_val) const override {
		return Serialise::timestamp(std::fabs(Unserialise::timestamp(serialise_val) - _ref_val));
	}

public:
	DateKey(Xapian::valueno slot, bool reverse, const std::string& value)
		: BaseKey(slot, reverse),
		  _ref_val(Datetime::timestamp(value)) { }
};


// Class for create the key using as a reference a boolean value.
class BoolKey : public BaseKey {
	std::string _ref_val;

	std::string get_cmpvalue(const std::string& serialise_val) const override {
		return Serialise::positive(serialise_val[0] != _ref_val[0]);
	}

public:
	BoolKey(Xapian::valueno slot, bool reverse, const std::string& value)
		: BaseKey(slot, reverse),
		  _ref_val(Serialise::boolean(value)) { }
};


// Class for create the key using as a reference a string value.
template <typename StringMetric>
class StringKey : public BaseKey {
	StringMetric _metric;

	std::string get_cmpvalue(const std::string& serialise_val) const override {
		return Serialise::_float(_metric.distance(serialise_val));
	}

public:
	StringKey(Xapian::valueno slot, bool reverse, const std::string& value, bool icase)
		: BaseKey(slot, reverse),
		  _metric(value, icase) { }
};


// Class for create the key using as a reference a geospatial value.
class GeoKey : public BaseKey {
	CartesianUSet _centroids;

	std::string get_cmpvalue(const std::string& serialise_val) const override;

public:
	GeoKey(const required_spc_t& field_spc, bool reverse, const std::string& value)
		: BaseKey(field_spc.slot, reverse),
		  _centroids(EWKT_Parser::getCentroids(value, field_spc.partials, field_spc.error)) { }
};


/*
 * KeyMaker subclass which combines several Multivalues.
 * This class only is used for sorting, there are two cases:
 * Case 1: Ascending (include in the query -> sort:+field_name or sort:field_name), of all values is selected the smallest.
 * Case 2: Descending (include in the query -> sort:-field_name), of all values is selected the largest.

 * For collapsing, it is used Xapian::MultiValueKeyMaker.
 */
class Multi_MultiValueKeyMaker : public Xapian::KeyMaker {
	// Vector of slots
	std::vector<std::unique_ptr<BaseKey>> slots;

public:
	Multi_MultiValueKeyMaker() = default;

	template <class Iterator>
	Multi_MultiValueKeyMaker(Iterator begin, Iterator end)
		: slots(begin, end) { }

	virtual std::string operator()(const Xapian::Document& doc) const override;
	void add_value(const required_spc_t& field_spc, bool reverse, const std::string& value, const query_field_t& qf);

	void levenshtein(const required_spc_t& field_spc, bool reverse, const std::string& value, const query_field_t& qf) {
		slots.push_back(std::make_unique<StringKey<Levenshtein>>(field_spc.slot, reverse, value, qf.icase));
	}

	void jaro(const required_spc_t& field_spc, bool reverse, const std::string& value, const query_field_t& qf) {
		slots.push_back(std::make_unique<StringKey<Jaro>>(field_spc.slot, reverse, value, qf.icase));
	}

	void jaro_winkler(const required_spc_t& field_spc, bool reverse, const std::string& value, const query_field_t& qf) {
		slots.push_back(std::make_unique<StringKey<Jaro_Winkler>>(field_spc.slot, reverse, value, qf.icase));
	}

	void sorensen_dice(const required_spc_t& field_spc, bool reverse, const std::string& value, const query_field_t& qf) {
		slots.push_back(std::make_unique<StringKey<Sorensen_Dice>>(field_spc.slot, reverse, value, qf.icase));
	}

	void jaccard(const required_spc_t& field_spc, bool reverse, const std::string& value, const query_field_t& qf) {
		slots.push_back(std::make_unique<StringKey<Jaccard>>(field_spc.slot, reverse, value, qf.icase));
	}

	void lcs(const required_spc_t& field_spc, bool reverse, const std::string& value, const query_field_t& qf) {
		slots.push_back(std::make_unique<StringKey<LCSubstr>>(field_spc.slot, reverse, value, qf.icase));
	}

	void lcsq(const required_spc_t& field_spc, bool reverse, const std::string& value, const query_field_t& qf) {
		slots.push_back(std::make_unique<StringKey<LCSubsequence>>(field_spc.slot, reverse, value, qf.icase));
	}

	void soundex_en(const required_spc_t& field_spc, bool reverse, const std::string& value, const query_field_t& qf) {
		slots.push_back(std::make_unique<StringKey<SoundexMetric<SoundexEnglish, LCSubsequence>>>(field_spc.slot, reverse, value, qf.icase));
	}

	void soundex_fr(const required_spc_t& field_spc, bool reverse, const std::string& value, const query_field_t& qf) {
		slots.push_back(std::make_unique<StringKey<SoundexMetric<SoundexFrench, LCSubsequence>>>(field_spc.slot, reverse, value, qf.icase));
	}

	void soundex_de(const required_spc_t& field_spc, bool reverse, const std::string& value, const query_field_t& qf) {
		slots.push_back(std::make_unique<StringKey<SoundexMetric<SoundexGerman, LCSubsequence>>>(field_spc.slot, reverse, value, qf.icase));
	}

	void soundex_es(const required_spc_t& field_spc, bool reverse, const std::string& value, const query_field_t& qf) {
		slots.push_back(std::make_unique<StringKey<SoundexMetric<SoundexSpanish, LCSubsequence>>>(field_spc.slot, reverse, value, qf.icase));
	}

	void soundex(const required_spc_t& field_spc, bool reverse, const std::string& value, const query_field_t& qf) {
		try {
			auto func = map_dispatch_soundex_metric.at(field_spc.language);
			(this->*func)(field_spc, reverse, value, qf);
		} catch (const std::out_of_range&) {
			(this->*def_soundex_metric)(field_spc, reverse, value, qf);
		}
	}
};
