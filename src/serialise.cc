/*
 * Copyright (C) 2015-2018 Dubalu LLC. All rights reserved.
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

#include "serialise.h"

#include <algorithm>                                  // for move
#include <ctype.h>                                    // for toupper
#include <functional>                                 // for cref
#include <math.h>                                     // for round
#include <stdexcept>                                  // for out_of_range, invalid_argument
#include <stdio.h>                                    // for sprintf
#include <string_view>                                // for std::string_view
#include <strings.h>                                  // for strcasecmp
#include <time.h>                                     // for tm, gmtime, time_t

#include "base_x.hh"                                  // for base62
#include "cast.h"                                     // for Cast
#include "endian.h"                                   // for htobe32, htobe56
#include "exception.h"                                // for SerialisationError, ...
#include "geospatial/geospatial.h"                    // for GeoSpatial, EWKT
#include "geospatial/htm.h"                           // for Cartesian, HTM_MAX_LENGTH_NAME, HTM_BYTES_ID, range_t
#include "cuuid/uuid.h"                               // for Uuid
#include "msgpack.h"                                  // for MsgPack, object::object, type_error
#include "query_dsl.h"                                // for QUERYDSL_FROM, QUERYDSL_TO
#include "schema.h"                                   // for FieldType, FieldType::TERM, Fiel...
#include "serialise_list.h"                           // for StringList, CartesianList and RangeList
#include "split.h"                                    // for Split
#include "utils.h"                                    // for toUType, stox, repr


constexpr char UUID_SEPARATOR_LIST = ';';


bool
Serialise::possiblyUUID(std::string_view field_value) noexcept
{
	auto field_value_sz = field_value.size();
	if (field_value_sz > 2) {
		Split<> split(field_value, UUID_SEPARATOR_LIST);
		if (field_value.front() == '{' && field_value.back() == '}') {
			split = Split<>(field_value.substr(1, field_value_sz - 2), UUID_SEPARATOR_LIST);
		} else
		if (field_value.compare(0, 9, "urn:uuid:") == 0) {
			split = Split<>(field_value.substr(9), UUID_SEPARATOR_LIST);
		}
		for (const auto& uuid : split) {
			auto uuid_sz = uuid.size();
			if (uuid_sz) {
				if (uuid_sz == UUID_LENGTH) {
					if (UUID::is_valid(uuid)) {
						continue;
					}
				}
#ifdef XAPIAND_UUID_ENCODED
				if (uuid_sz >= 7 && uuid.front() == '~') {  // floor((4 * 8) / log2(59)) + 2
					if (UUID_ENCODER.is_valid(uuid)) {
						continue;
					}
				}
#endif
				return false;
			}
		}
		return true;
	}
	return false;
}


bool
Serialise::isUUID(std::string_view field_value) noexcept
{
	auto field_value_sz = field_value.size();
	if (field_value_sz > 2) {
		Split<> split(field_value, UUID_SEPARATOR_LIST);
		if (field_value.front() == '{' && field_value.back() == '}') {
			split = Split<>(field_value.substr(1, field_value_sz - 2), UUID_SEPARATOR_LIST);
		} else
		if (field_value.compare(0, 9, "urn:uuid:") == 0) {
			split = Split<>(field_value.substr(9), UUID_SEPARATOR_LIST);
		}
		for (const auto& uuid : split) {
			auto uuid_sz = uuid.size();
			if (uuid_sz) {
				if (uuid_sz == UUID_LENGTH) {
					if (UUID::is_valid(uuid)) {
						continue;
					}
				}
#ifdef XAPIAND_UUID_ENCODED
				if (uuid_sz >= 7 && uuid.front() == '~') {  // floor((4 * 8) / log2(59)) + 2
					try {
						auto decoded = UUID_ENCODER.decode(uuid);
						if (UUID::is_serialised(decoded)) {
							continue;
						}
					} catch (const std::invalid_argument&) { }
				}
#endif
				return false;
			}
		}
		return true;
	}
	return false;
}


std::string
Serialise::MsgPack(const required_spc_t& field_spc, const class MsgPack& field_value)
{
	switch (field_value.getType()) {
		case MsgPack::Type::BOOLEAN:
			return boolean(field_spc.get_type(), field_value.boolean());
		case MsgPack::Type::POSITIVE_INTEGER:
			return positive(field_spc.get_type(), field_value.u64());
		case MsgPack::Type::NEGATIVE_INTEGER:
			return integer(field_spc.get_type(), field_value.i64());
		case MsgPack::Type::FLOAT:
			return _float(field_spc.get_type(), field_value.f64());
		case MsgPack::Type::STR:
			return string(field_spc, field_value.str_view());
		case MsgPack::Type::MAP:
			return object(field_spc, field_value);
		default:
			THROW(SerialisationError, "msgpack::type %s is not supported", field_value.getStrType().c_str());
	}
}


std::string
Serialise::object(const required_spc_t& field_spc, const class MsgPack& o)
{
	if (o.size() == 1) {
		auto str_key = o.begin()->str_view();
		switch ((Cast::Hash)fnv1ah32::hash(str_key)) {
			case Cast::Hash::INTEGER:
				return integer(field_spc.get_type(), Cast::integer(o.at(str_key)));
			case Cast::Hash::POSITIVE:
				return positive(field_spc.get_type(), Cast::positive(o.at(str_key)));
			case Cast::Hash::FLOAT:
				return _float(field_spc.get_type(), Cast::_float(o.at(str_key)));
			case Cast::Hash::BOOLEAN:
				return boolean(field_spc.get_type(), Cast::boolean(o.at(str_key)));
			case Cast::Hash::TERM:
			case Cast::Hash::TEXT:
			case Cast::Hash::STRING:
				return string(field_spc, Cast::string(o.at(str_key)));
			case Cast::Hash::UUID:
				return string(field_spc, Cast::uuid(o.at(str_key)));
			case Cast::Hash::DATE:
				return date(field_spc, Cast::date(o.at(str_key)));
			case Cast::Hash::TIME:
				return time(field_spc, Cast::time(o.at(str_key)));
			case Cast::Hash::TIMEDELTA:
				return timedelta(field_spc, Cast::timedelta(o.at(str_key)));
			case Cast::Hash::EWKT:
				return string(field_spc, Cast::ewkt(o.at(str_key)));
			case Cast::Hash::POINT:
			case Cast::Hash::CIRCLE:
			case Cast::Hash::CONVEX:
			case Cast::Hash::POLYGON:
			case Cast::Hash::CHULL:
			case Cast::Hash::MULTIPOINT:
			case Cast::Hash::MULTICIRCLE:
			case Cast::Hash::MULTIPOLYGON:
			case Cast::Hash::MULTICHULL:
			case Cast::Hash::GEO_COLLECTION:
			case Cast::Hash::GEO_INTERSECTION:
				return geospatial(field_spc.get_type(), o);
			default:
				THROW(SerialisationError, "Unknown cast type %s", repr(str_key).c_str());
		}
	}

	THROW(SerialisationError, "Expected map with one element");
}


std::string
Serialise::serialise(const required_spc_t& field_spc, const class MsgPack& field_value)
{
	auto field_type = field_spc.get_type();

	switch (field_type) {
		case FieldType::INTEGER:
			return integer(field_value.i64());
		case FieldType::POSITIVE:
			return positive(field_value.u64());
		case FieldType::FLOAT:
			return _float(field_value.f64());
		case FieldType::DATE:
			return date(field_value);
		case FieldType::TIME:
			return time(field_value);
		case FieldType::TIMEDELTA:
			return timedelta(field_value);
		case FieldType::BOOLEAN:
			return boolean(field_value.boolean());
		case FieldType::TERM:
		case FieldType::TEXT:
		case FieldType::STRING:
			return field_value.str();
		case FieldType::GEO:
			return geospatial(field_value);
		case FieldType::UUID:
			return uuid(field_value.str_view());
		default:
			THROW(SerialisationError, "Type: 0x%02x is an unknown type", field_type);
	}
}


std::string
Serialise::serialise(const required_spc_t& field_spc, std::string_view field_value)
{
	auto field_type = field_spc.get_type();

	switch (field_type) {
		case FieldType::INTEGER:
			return integer(field_value);
		case FieldType::POSITIVE:
			return positive(field_value);
		case FieldType::FLOAT:
			return _float(field_value);
		case FieldType::DATE:
			return date(field_value);
		case FieldType::TIME:
			return time(field_value);
		case FieldType::TIMEDELTA:
			return timedelta(field_value);
		case FieldType::BOOLEAN:
			return boolean(field_value);
		case FieldType::TERM:
		case FieldType::TEXT:
		case FieldType::STRING:
			return std::string(field_value);
		case FieldType::GEO:
			return geospatial(field_value);
		case FieldType::UUID:
			return uuid(field_value);
		default:
			THROW(SerialisationError, "Type: 0x%02x is an unknown type", field_type);
	}
}


std::string
Serialise::string(const required_spc_t& field_spc, std::string_view field_value)
{
	switch (field_spc.get_type()) {
		case FieldType::DATE:
			return date(field_value);
		case FieldType::TIME:
			return time(field_value);
		case FieldType::TIMEDELTA:
			return timedelta(field_value);
		case FieldType::BOOLEAN:
			return boolean(field_value);
		case FieldType::TERM:
		case FieldType::TEXT:
		case FieldType::STRING:
			return std::string(field_value);
		case FieldType::GEO:
			return geospatial(field_value);
		case FieldType::UUID:
			return uuid(field_value);
		default:
			THROW(SerialisationError, "Type: %s is not string", type(field_spc.get_type()).c_str());
	}
}


std::string
Serialise::date(const required_spc_t& field_spc, const class MsgPack& field_value)
{
	switch (field_value.getType()) {
		case MsgPack::Type::POSITIVE_INTEGER:
			return positive(field_spc.get_type(), field_value.u64());
		case MsgPack::Type::NEGATIVE_INTEGER:
			return integer(field_spc.get_type(), field_value.i64());
		case MsgPack::Type::FLOAT:
			return _float(field_spc.get_type(), field_value.f64());
		case MsgPack::Type::STR:
			return string(field_spc, field_value.str_view());
		case MsgPack::Type::MAP:
			switch (field_spc.get_type()) {
				case FieldType::FLOAT:
					return _float(Datetime::timestamp(Datetime::DateParser(field_value)));
				case FieldType::DATE:
					return date(field_value);
				case FieldType::TIME:
					return time(Datetime::timestamp(Datetime::DateParser(field_value)));
				case FieldType::TIMEDELTA:
					return timedelta(Datetime::timestamp(Datetime::DateParser(field_value)));
				case FieldType::STRING:
					return Datetime::iso8601(Datetime::DateParser(field_value));
				default:
					THROW(SerialisationError, "Type: %s is not a date", field_value.getStrType().c_str());
			}
		default:
			THROW(SerialisationError, "Type: %s is not a date", field_value.getStrType().c_str());
	}
}


std::string
Serialise::time(const required_spc_t& field_spc, const class MsgPack& field_value)
{
	switch (field_value.getType()) {
		case MsgPack::Type::POSITIVE_INTEGER:
			return positive(field_spc.get_type(), field_value.u64());
		case MsgPack::Type::NEGATIVE_INTEGER:
			return integer(field_spc.get_type(), field_value.i64());
		case MsgPack::Type::FLOAT:
			return _float(field_spc.get_type(), field_value.f64());
		case MsgPack::Type::STR:
			return string(field_spc, field_value.str_view());
		default:
			THROW(SerialisationError, "Type: %s is not a time", field_value.getStrType().c_str());
	}
}


std::string
Serialise::timedelta(const required_spc_t& field_spc, const class MsgPack& field_value)
{
	switch (field_value.getType()) {
		case MsgPack::Type::POSITIVE_INTEGER:
			return positive(field_spc.get_type(), field_value.u64());
		case MsgPack::Type::NEGATIVE_INTEGER:
			return integer(field_spc.get_type(), field_value.i64());
		case MsgPack::Type::FLOAT:
			return _float(field_spc.get_type(), field_value.f64());
		case MsgPack::Type::STR:
			return string(field_spc, field_value.str_view());
		default:
			THROW(SerialisationError, "Type: %s is not a timedelta", field_value.getStrType().c_str());
	}
}


std::string
Serialise::_float(FieldType field_type, double field_value)
{
	switch (field_type) {
		case FieldType::DATE:
			return timestamp(field_value);
		case FieldType::TIME:
			return time(field_value);
		case FieldType::TIMEDELTA:
			return timedelta(field_value);
		case FieldType::FLOAT:
			return _float(field_value);
		default:
			THROW(SerialisationError, "Type: %s is not a float", type(field_type).c_str());
	}
}


std::string
Serialise::integer(FieldType field_type, int64_t field_value)
{
	switch (field_type) {
		case FieldType::POSITIVE:
			if (field_value < 0) {
				THROW(SerialisationError, "Type: %s must be a positive number [%lld]", type(field_type).c_str(), field_value);
			}
			return positive(field_value);
		case FieldType::DATE:
			return timestamp(field_value);
		case FieldType::TIME:
			return time(field_value);
		case FieldType::TIMEDELTA:
			return timedelta(field_value);
		case FieldType::FLOAT:
			return _float(field_value);
		case FieldType::INTEGER:
			return integer(field_value);
		default:
			THROW(SerialisationError, "Type: %s is not a integer [%lld]", type(field_type).c_str(), field_value);
	}
}


std::string
Serialise::positive(FieldType field_type, uint64_t field_value)
{
	switch (field_type) {
		case FieldType::DATE:
			return timestamp(field_value);
		case FieldType::FLOAT:
			return _float(field_value);
		case FieldType::TIME:
			return time(field_value);
		case FieldType::TIMEDELTA:
			return timedelta(field_value);
		case FieldType::INTEGER:
			return integer(field_value);
		case FieldType::POSITIVE:
			return positive(field_value);
		default:
			THROW(SerialisationError, "Type: %s is not a positive integer [%llu]", type(field_type).c_str(), field_value);
	}
}


std::string
Serialise::boolean(FieldType field_type, bool field_value)
{
	if (field_type == FieldType::BOOLEAN) {
		return boolean(field_value);
	}

	THROW(SerialisationError, "Type: %s is not boolean", type(field_type).c_str());
}


std::string
Serialise::geospatial(FieldType field_type, const class MsgPack& field_value)
{
	if (field_type == FieldType::GEO) {
		return geospatial(field_value);
	}

	THROW(SerialisationError, "Type: %s is not geospatial", type(field_type).c_str());
}


std::string
Serialise::date(std::string_view field_value)
{
	return date(Datetime::DateParser(field_value));
}


std::string
Serialise::date(const class MsgPack& field_value)
{
	return date(Datetime::DateParser(field_value));
}


std::string
Serialise::date(const class MsgPack& value, Datetime::tm_t& tm)
{
	tm = Datetime::DateParser(value);
	return date(tm);
}


std::string
Serialise::time(std::string_view field_value)
{
	return timestamp(Datetime::time_to_double(Datetime::TimeParser(field_value)));
}


std::string
Serialise::time(const class MsgPack& field_value)
{
	return timestamp(Datetime::time_to_double(field_value));
}


std::string
Serialise::time(const class MsgPack& field_value, double& t_val)
{
	switch (field_value.getType()) {
		case MsgPack::Type::POSITIVE_INTEGER:
			t_val = field_value.u64();
			return time(t_val);
		case MsgPack::Type::NEGATIVE_INTEGER:
			t_val = field_value.i64();
			return time(t_val);
		case MsgPack::Type::FLOAT:
			t_val = field_value.f64();
			return time(t_val);
		case MsgPack::Type::STR:
			t_val = Datetime::time_to_double(Datetime::TimeParser(field_value.str_view()));
			return timestamp(t_val);
		default:
			THROW(SerialisationError, "Type: %s is not time", field_value.getStrType().c_str());
	}
}


std::string
Serialise::time(double field_value)
{
	if (Datetime::isvalidTime(field_value)) {
		return timestamp(field_value);
	}

	THROW(SerialisationError, "Time: %f is out of range", field_value);
}


std::string
Serialise::timedelta(std::string_view field_value)
{
	return timestamp(Datetime::timedelta_to_double(Datetime::TimedeltaParser(field_value)));
}


std::string
Serialise::timedelta(const class MsgPack& field_value)
{
	return timestamp(Datetime::timedelta_to_double(field_value));
}


std::string
Serialise::timedelta(const class MsgPack& field_value, double& t_val)
{
	switch (field_value.getType()) {
		case MsgPack::Type::POSITIVE_INTEGER:
			t_val = field_value.u64();
			return timedelta(t_val);
		case MsgPack::Type::NEGATIVE_INTEGER:
			t_val = field_value.i64();
			return timedelta(t_val);
		case MsgPack::Type::FLOAT:
			t_val = field_value.f64();
			return timedelta(t_val);
		case MsgPack::Type::STR:
			t_val = Datetime::timedelta_to_double(Datetime::TimedeltaParser(field_value.str_view()));
			return timestamp(t_val);
		default:
			THROW(SerialisationError, "Type: %s is not timedelta", field_value.getStrType().c_str());
	}
}


std::string
Serialise::timedelta(double field_value)
{
	if (Datetime::isvalidTimedelta(field_value)) {
		return timestamp(field_value);
	}

	THROW(SerialisationError, "Timedelta: %f is out of range", field_value);
}


std::string
Serialise::_float(std::string_view field_value)
{
	try {
		return _float(strict_stod(field_value));
	} catch (const std::invalid_argument& exc) {
		RETHROW(SerialisationError, "Invalid float format: %s", repr(field_value).c_str());
	} catch (const std::out_of_range& exc) {
		RETHROW(SerialisationError, "Out of range float format: %s", repr(field_value).c_str());
	}
}


std::string
Serialise::integer(std::string_view field_value)
{
	try {
		return integer(strict_stoll(field_value));
	} catch (const std::invalid_argument& exc) {
		RETHROW(SerialisationError, "Invalid integer format: %s", repr(field_value).c_str());
	} catch (const std::out_of_range& exc) {
		RETHROW(SerialisationError, "Out of range integer format: %s", repr(field_value).c_str());
	}
}


std::string
Serialise::positive(std::string_view field_value)
{
	try {
		return positive(strict_stoull(field_value));
	} catch (const std::invalid_argument& exc) {
		RETHROW(SerialisationError, "Invalid positive integer format: %s", repr(field_value).c_str());
	} catch (const std::out_of_range& exc) {
		RETHROW(SerialisationError, "Out of range positive integer format: %s", repr(field_value).c_str());
	}
}


std::string
Serialise::uuid(std::string_view field_value)
{
	auto field_value_sz = field_value.size();
	if (field_value_sz > 2) {
		Split<> split(field_value, UUID_SEPARATOR_LIST);
		if (field_value.front() == '{' && field_value.back() == '}') {
			split = Split<>(field_value.substr(1, field_value_sz - 2), UUID_SEPARATOR_LIST);
		} else
		if (field_value.compare(0, 9, "urn:uuid:") == 0) {
			split = Split<>(field_value.substr(9), UUID_SEPARATOR_LIST);
		}
		std::string serialised;
		for (const auto& uuid : split) {
			auto uuid_sz = uuid.size();
			if (uuid_sz) {
				if (uuid_sz == UUID_LENGTH) {
					try {
						serialised.append(UUID(uuid).serialise());
						continue;
					} catch (const std::invalid_argument&) { }
				}
			#ifdef XAPIAND_UUID_ENCODED
				auto uuid_front = uuid.front();
				if (uuid_sz >= 7 && uuid_front == '~') {  // floor((4 * 8) / log2(59)) + 2
					try {
						auto decoded = UUID_ENCODER.decode(uuid);
						if (UUID::is_serialised(decoded)) {
							serialised.append(decoded);
							continue;
						}
					} catch (const std::invalid_argument&) { }
				}
			#endif
				THROW(SerialisationError, "Invalid encoded UUID format in: %s", uuid.c_str());
			}
		}
		return serialised;
	}
	THROW(SerialisationError, "Invalid UUID format in: %s", repr(field_value).c_str());
}


std::string
Serialise::boolean(std::string_view field_value)
{
	switch (field_value.size()) {
		case 0:
			return std::string(1, SERIALISED_FALSE);
		case 1:
			switch (field_value[0]) {
				case '0':
				case 'f':
				case 'F':
					return std::string(1, SERIALISED_FALSE);
				case '1':
				case 't':
				case 'T':
					return std::string(1, SERIALISED_TRUE);
			}
			break;
		case 4:
			switch (field_value[0]) {
				case 't':
				case 'T': {
					auto lower_value = lower_string(field_value);
					if (lower_value == "true") {
						return std::string(1, SERIALISED_TRUE);
					}
				}
			}
			break;
		case 5:
			switch (field_value[0]) {
				case 'f':
				case 'F': {
					auto lower_value = lower_string(field_value);
					if (lower_value == "false") {
						return std::string(1, SERIALISED_FALSE);
					}
				}
			}
			break;
	}

	THROW(SerialisationError, "Boolean format is not valid: %s", repr(field_value).c_str());
}


std::string
Serialise::geospatial(std::string_view field_value)
{
	EWKT ewkt(field_value);
	return Serialise::ranges(ewkt.getGeometry()->getRanges(DEFAULT_GEO_PARTIALS, DEFAULT_GEO_ERROR));
}


std::string
Serialise::geospatial(const class MsgPack& field_value)
{
	GeoSpatial geo(field_value);
	return Serialise::ranges(geo.getGeometry()->getRanges(DEFAULT_GEO_PARTIALS, DEFAULT_GEO_ERROR));
}


std::string
Serialise::ranges_centroids(const std::vector<range_t>& ranges, const std::vector<Cartesian>& centroids)
{
	std::vector<std::string> data = { RangeList::serialise(ranges.begin(), ranges.end()), CartesianList::serialise(centroids.begin(), centroids.end()) };
	return StringList::serialise(data.begin(), data.end());
}


std::string
Serialise::ranges(const std::vector<range_t>& ranges)
{
	if (ranges.empty()) {
		return "";
	}

	size_t hash = 0;
	for (const auto& range : ranges) {
		hash ^= std::hash<range_t>{}(range);
	}

	return sortable_serialise(hash);
}


std::string
Serialise::cartesian(const Cartesian& norm_cartesian)
{
	uint32_t x = htobe32(((uint32_t)(norm_cartesian.x * DOUBLE2INT) + MAXDOU2INT));
	uint32_t y = htobe32(((uint32_t)(norm_cartesian.y * DOUBLE2INT) + MAXDOU2INT));
	uint32_t z = htobe32(((uint32_t)(norm_cartesian.z * DOUBLE2INT) + MAXDOU2INT));
	const char serialised[] = { (char)(x & 0xFF), (char)((x >> 8) & 0xFF), (char)((x >> 16) & 0xFF), (char)((x >> 24) & 0xFF),
								(char)(y & 0xFF), (char)((y >> 8) & 0xFF), (char)((y >> 16) & 0xFF), (char)((y >> 24) & 0xFF),
								(char)(z & 0xFF), (char)((z >> 8) & 0xFF), (char)((z >> 16) & 0xFF), (char)((z >> 24) & 0xFF) };
	return std::string(serialised, SERIALISED_LENGTH_CARTESIAN);
}


std::string
Serialise::trixel_id(uint64_t id)
{
	id = htobe56(id);
	const char serialised[] = { (char)(id & 0xFF), (char)((id >> 8) & 0xFF), (char)((id >> 16) & 0xFF), (char)((id >> 24) & 0xFF),
								(char)((id >> 32) & 0xFF), (char)((id >> 40) & 0xFF), (char)((id >> 48) & 0xFF) };
	return std::string(serialised, HTM_BYTES_ID);
}


std::string
Serialise::range(const range_t& range)
{
	uint64_t start = htobe56(range.start);
	uint64_t end = htobe56(range.end);
	const char serialised[] = { (char)(start & 0xFF), (char)((start >> 8) & 0xFF), (char)((start >> 16) & 0xFF), (char)((start >> 24) & 0xFF),
								(char)((start >> 32) & 0xFF), (char)((start >> 40) & 0xFF), (char)((start >> 48) & 0xFF),
								(char)(end & 0xFF), (char)((end >> 8) & 0xFF), (char)((end >> 16) & 0xFF), (char)((end >> 24) & 0xFF),
								(char)((end >> 32) & 0xFF), (char)((end >> 40) & 0xFF), (char)((end >> 48) & 0xFF) };
	return std::string(serialised, SERIALISED_LENGTH_RANGE);
}


const std::string&
Serialise::type(FieldType field_type)
{
	switch (field_type) {
		case FieldType::TERM: {
			static const std::string term_str(TERM_STR);
			return term_str;
		}
		case FieldType::TEXT: {
			static const std::string text_str(TEXT_STR);
			return text_str;
		}
		case FieldType::STRING: {
			static const std::string string_str(STRING_STR);
			return string_str;
		}
		case FieldType::FLOAT: {
			static const std::string float_str(FLOAT_STR);
			return float_str;
		}
		case FieldType::INTEGER: {
			static const std::string integer_str(INTEGER_STR);
			return integer_str;
		}
		case FieldType::POSITIVE: {
			static const std::string positive_str(POSITIVE_STR);
			return positive_str;
		}
		case FieldType::BOOLEAN: {
			static const std::string boolean_str(BOOLEAN_STR);
			return boolean_str;
		}
		case FieldType::GEO: {
			static const std::string geo_str(GEO_STR);
			return geo_str;
		}
		case FieldType::DATE: {
			static const std::string date_str(DATE_STR);
			return date_str;
		}
		case FieldType::TIME: {
			static const std::string time_str(TIME_STR);
			return time_str;
		}
		case FieldType::TIMEDELTA: {
			static const std::string timedelta_str(TIMEDELTA_STR);
			return timedelta_str;
		}
		case FieldType::UUID: {
			static const std::string uuid_str(UUID_STR);
			return uuid_str;
		}
		case FieldType::SCRIPT: {
			static const std::string script_str(SCRIPT_STR);
			return script_str;
		}
		case FieldType::OBJECT: {
			static const std::string object_str(OBJECT_STR);
			return object_str;
		}
		case FieldType::ARRAY: {
			static const std::string array_str(ARRAY_STR);
			return array_str;
		}
		case FieldType::FOREIGN: {
			static const std::string foreign_str(FOREIGN_STR);
			return foreign_str;
		}
		case FieldType::EMPTY: {
			static const std::string empty_str(EMPTY_STR);
			return empty_str;
		}
		default: {
			static const std::string unknown("unknown");
			return unknown;
		}
	}
}


FieldType
Serialise::guess_type(const class MsgPack& field_value, bool bool_term)
{
	switch (field_value.getType()) {
		case MsgPack::Type::NEGATIVE_INTEGER:
			return FieldType::INTEGER;

		case MsgPack::Type::POSITIVE_INTEGER:
			return FieldType::POSITIVE;

		case MsgPack::Type::FLOAT:
			return FieldType::FLOAT;

		case MsgPack::Type::BOOLEAN:
			return FieldType::BOOLEAN;

		case MsgPack::Type::STR: {
			const auto str_value = field_value.str_view();

			if (isUUID(str_value)) {
				return FieldType::UUID;
			}

			if (Datetime::isDate(str_value)) {
				return FieldType::DATE;
			}

			if (Datetime::isTime(str_value)) {
				return FieldType::TIME;
			}

			if (Datetime::isTimedelta(str_value)) {
				return FieldType::TIMEDELTA;
			}

			if (EWKT::isEWKT(str_value)) {
				return FieldType::GEO;
			}

			if (bool_term) {
				return FieldType::TERM;
			}

			if (isText(str_value, bool_term)) {
				return FieldType::TEXT;
			}

			return FieldType::STRING;
		}

		case MsgPack::Type::MAP: {
			if (field_value.size() == 1) {
				const auto str_key = field_value.begin()->str_view();
				switch ((Cast::Hash)fnv1ah32::hash(str_key)) {
					case Cast::Hash::INTEGER:
						return FieldType::INTEGER;
					case Cast::Hash::POSITIVE:
						return FieldType::POSITIVE;
					case Cast::Hash::FLOAT:
						return FieldType::FLOAT;
					case Cast::Hash::BOOLEAN:
						return FieldType::BOOLEAN;
					case Cast::Hash::TERM:
						return FieldType::TERM;
					case Cast::Hash::TEXT:
						return FieldType::TEXT;
					case Cast::Hash::STRING:
						return FieldType::STRING;
					case Cast::Hash::UUID:
						return FieldType::UUID;
					case Cast::Hash::DATE:
						return FieldType::DATE;
					case Cast::Hash::TIME:
						return FieldType::TIME;
					case Cast::Hash::TIMEDELTA:
						return FieldType::TIMEDELTA;
					case Cast::Hash::EWKT:
					case Cast::Hash::POINT:
					case Cast::Hash::CIRCLE:
					case Cast::Hash::CONVEX:
					case Cast::Hash::POLYGON:
					case Cast::Hash::CHULL:
					case Cast::Hash::MULTIPOINT:
					case Cast::Hash::MULTICIRCLE:
					case Cast::Hash::MULTIPOLYGON:
					case Cast::Hash::MULTICHULL:
					case Cast::Hash::GEO_COLLECTION:
					case Cast::Hash::GEO_INTERSECTION:
						return FieldType::GEO;
					default:
						THROW(SerialisationError, "Unknown cast type: %s", repr(str_key).c_str());
				}
			} else {
				THROW(SerialisationError, "Expected map with one element");
			}
		}

		case MsgPack::Type::UNDEFINED:
		case MsgPack::Type::NIL:
			if (bool_term) {
				return FieldType::TERM;
			}

			// Default type STRING.
			return FieldType::STRING;

		default:
			THROW(SerialisationError, "Unexpected type %s", field_value.getStrType().c_str());
	}
}


std::pair<FieldType, std::string>
Serialise::guess_serialise(const class MsgPack& field_value, bool bool_term)
{
	switch (field_value.getType()) {
		case MsgPack::Type::NEGATIVE_INTEGER:
			return std::make_pair(FieldType::INTEGER, integer(field_value.i64()));

		case MsgPack::Type::POSITIVE_INTEGER:
			return std::make_pair(FieldType::POSITIVE, positive(field_value.u64()));

		case MsgPack::Type::FLOAT:
			return std::make_pair(FieldType::FLOAT, _float(field_value.f64()));

		case MsgPack::Type::BOOLEAN:
			return std::make_pair(FieldType::BOOLEAN, boolean(field_value.boolean()));

		case MsgPack::Type::STR: {
			auto str_obj = field_value.str_view();

			// Try like UUID
			try {
				return std::make_pair(FieldType::UUID, uuid(str_obj));
			} catch (const SerialisationError&) { }

			// Try like DATE
			try {
				return std::make_pair(FieldType::DATE, date(str_obj));
			} catch (const DatetimeError&) { }

			// Try like TIME
			try {
				return std::make_pair(FieldType::TIME, time(str_obj));
			} catch (const TimeError&) { }

			// Try like TIMEDELTA
			try {
				return std::make_pair(FieldType::TIMEDELTA, timedelta(str_obj));
			} catch (const TimedeltaError&) { }

			// Try like GEO
			try {
				return std::make_pair(FieldType::GEO, geospatial(str_obj));
			} catch (const EWKTError&) { }

			if (bool_term) {
				return std::make_pair(FieldType::TERM, std::string(str_obj));
			}

			// Like TEXT
			if (isText(str_obj, bool_term)) {
				return std::make_pair(FieldType::TEXT, std::string(str_obj));
			}

			// Default type STRING.
			return std::make_pair(FieldType::STRING, std::string(str_obj));
		}

		case MsgPack::Type::MAP: {
			if (field_value.size() == 1) {
				const auto it = field_value.begin();
				const auto str_key = it->str_view();
				switch ((Cast::Hash)fnv1ah32::hash(str_key)) {
					case Cast::Hash::INTEGER:
						return std::make_pair(FieldType::INTEGER, integer(Cast::integer(it.value())));
					case Cast::Hash::POSITIVE:
						return std::make_pair(FieldType::POSITIVE, positive(Cast::positive(it.value())));
					case Cast::Hash::FLOAT:
						return std::make_pair(FieldType::FLOAT, _float(Cast::_float(it.value())));
					case Cast::Hash::BOOLEAN:
						return std::make_pair(FieldType::BOOLEAN, boolean(Cast::boolean(it.value())));
					case Cast::Hash::TERM:
						return std::make_pair(FieldType::TERM, Cast::string(it.value()));
					case Cast::Hash::TEXT:
						return std::make_pair(FieldType::TEXT, Cast::string(it.value()));
					case Cast::Hash::STRING:
						return std::make_pair(FieldType::STRING, Cast::string(it.value()));
					case Cast::Hash::UUID:
						return std::make_pair(FieldType::UUID, uuid(Cast::uuid(it.value())));
					case Cast::Hash::DATE:
						return std::make_pair(FieldType::DATE, date(Cast::date(it.value())));
					case Cast::Hash::TIME:
						return std::make_pair(FieldType::TIME, date(Cast::time(it.value())));
					case Cast::Hash::TIMEDELTA:
						return std::make_pair(FieldType::TIMEDELTA, date(Cast::timedelta(it.value())));
					case Cast::Hash::EWKT:
					case Cast::Hash::POINT:
					case Cast::Hash::CIRCLE:
					case Cast::Hash::CONVEX:
					case Cast::Hash::POLYGON:
					case Cast::Hash::CHULL:
					case Cast::Hash::MULTIPOINT:
					case Cast::Hash::MULTICIRCLE:
					case Cast::Hash::MULTIPOLYGON:
					case Cast::Hash::MULTICHULL:
					case Cast::Hash::GEO_COLLECTION:
					case Cast::Hash::GEO_INTERSECTION:
						return std::make_pair(FieldType::GEO, geospatial(field_value));
					default:
						THROW(SerialisationError, "Unknown cast type: %s", repr(str_key).c_str());
				}
			} else {
				THROW(SerialisationError, "Expected map with one element");
			}
		}

		case MsgPack::Type::UNDEFINED:
		case MsgPack::Type::NIL:
			if (bool_term) {
				return std::make_pair(FieldType::TERM, "");
			}

			// Default type STRING.
			return std::make_pair(FieldType::STRING, "");

		default:
			THROW(SerialisationError, "Unexpected type %s", field_value.getStrType().c_str());
	}
}


MsgPack
Unserialise::MsgPack(FieldType field_type, std::string_view serialised_val)
{
	class MsgPack result;
	switch (field_type) {
		case FieldType::FLOAT:
			result = _float(serialised_val);
			break;
		case FieldType::INTEGER:
			result = integer(serialised_val);
			break;
		case FieldType::POSITIVE:
			result = positive(serialised_val);
			break;
		case FieldType::DATE:
			result = date(serialised_val);
			break;
		case FieldType::TIME:
			result = time(serialised_val);
			break;
		case FieldType::TIMEDELTA:
			result = timedelta(serialised_val);
			break;
		case FieldType::BOOLEAN:
			result = boolean(serialised_val);
			break;
		case FieldType::TERM:
		case FieldType::TEXT:
		case FieldType::STRING:
			result = serialised_val;
			break;
		case FieldType::GEO: {
			const auto unser_geo = ranges_centroids(serialised_val);
			auto& ranges = result["Ranges"];
			int i = 0;
			for (const auto& range : unser_geo.first) {
				ranges[i++] = { range.start, range.end };
			}
			auto& centroids = result["Centroids"];
			i = 0;
			for (const auto& centroid : unser_geo.second) {
				centroids[i++] = { centroid.x, centroid.y, centroid.z };
			}
			break;
		}
		case FieldType::UUID:
			result = uuid(serialised_val);
			break;
		default:
			THROW(SerialisationError, "Type: 0x%02x is an unknown type", field_type);
	}

	return result;
}


std::string
Unserialise::date(std::string_view serialised_date)
{
	return Datetime::iso8601(timestamp(serialised_date));
}


std::string
Unserialise::time(std::string_view serialised_time)
{
	return Datetime::time_to_string(sortable_unserialise(serialised_time));
}


double
Unserialise::time_d(std::string_view serialised_time)
{
	auto t = sortable_unserialise(serialised_time);
	if (Datetime::isvalidTime(t)) {
		return t;
	}

	THROW(SerialisationError, "Unserialised time: %f is out of range", t);
}


std::string
Unserialise::timedelta(std::string_view serialised_timedelta)
{
	return Datetime::timedelta_to_string(sortable_unserialise(serialised_timedelta));
}


double
Unserialise::timedelta_d(std::string_view serialised_time)
{
	auto t = sortable_unserialise(serialised_time);
	if (Datetime::isvalidTimedelta(t)) {
		return t;
	}

	THROW(SerialisationError, "Unserialised timedelta: %f is out of range", t);
}


std::string
Unserialise::uuid(std::string_view serialised_uuid, UUIDRepr repr)
{
	std::string result;
	std::vector<UUID> uuids;
	switch (repr) {
#ifdef XAPIAND_UUID_GUID
		case UUIDRepr::guid:
			// {00000000-0000-1000-8000-010000000000}
			UUID::unserialise(serialised_uuid, std::back_inserter(uuids));
			result.push_back('{');
			result.append(join_string(uuids, std::string(1, UUID_SEPARATOR_LIST)));
			result.push_back('}');
			break;
#endif
#ifdef XAPIAND_UUID_URN
		case UUIDRepr::urn:
			// urn:uuid:00000000-0000-1000-8000-010000000000
			UUID::unserialise(serialised_uuid, std::back_inserter(uuids));
			result.append("urn:uuid:");
			result.append(join_string(uuids, std::string(1, UUID_SEPARATOR_LIST)));
			break;
#endif
#ifdef XAPIAND_UUID_ENCODED
		case UUIDRepr::encoded:
			if (serialised_uuid.front() != 1 && ((serialised_uuid.back() & 1) || (serialised_uuid.size() > 5 && *(serialised_uuid.rbegin() + 5) & 2))) {
				result.append("~" + UUID_ENCODER.encode(serialised_uuid));
				break;
			}
#endif
		default:
		case UUIDRepr::simple:
			// 00000000-0000-1000-8000-010000000000
			UUID::unserialise(serialised_uuid, std::back_inserter(uuids));
			result.append(join_string(uuids, std::string(1, UUID_SEPARATOR_LIST)));
			break;
	}
	return result;
}


std::pair<RangeList, CartesianList>
Unserialise::ranges_centroids(std::string_view serialised_geo)
{
	StringList data(serialised_geo);
	switch (data.size()) {
		case 0:
			return std::make_pair(RangeList(std::string_view("")), CartesianList(std::string_view("")));
		case 1:
			return std::make_pair(RangeList(data.front()), CartesianList(std::string_view("")));
		case 2:
			return std::make_pair(RangeList(data.front()), CartesianList(data.back()));
		default:
			THROW(SerialisationError, "Serialised geospatial must contain at most two elements");
	}
}


RangeList
Unserialise::ranges(std::string_view serialised_geo)
{
	StringList data(serialised_geo);
	switch (data.size()) {
		case 0:
			return RangeList(std::string_view(""));
		case 1:
		case 2:
			return RangeList(data.front());
		default:
			THROW(SerialisationError, "Serialised geospatial must contain at most two elements");
	}
}


CartesianList
Unserialise::centroids(std::string_view serialised_geo)
{
	StringList data(serialised_geo);
	switch (data.size()) {
		case 0:
		case 1:
			return CartesianList(std::string_view(""));
		case 2:
			return CartesianList(data.back());
		default:
			THROW(SerialisationError, "Serialised geospatial must contain at most two elements");
	}
}


Cartesian
Unserialise::cartesian(std::string_view serialised_val)
{
	if (serialised_val.size() != SERIALISED_LENGTH_CARTESIAN) {
		THROW(SerialisationError, "Cannot unserialise cartesian: %s [%zu]", repr(serialised_val).c_str(), serialised_val.size());
	}

	double x = (((unsigned)serialised_val[0] << 24) & 0xFF000000) | (((unsigned)serialised_val[1] << 16) & 0xFF0000) | (((unsigned)serialised_val[2] << 8) & 0xFF00)  | (((unsigned)serialised_val[3]) & 0xFF);
	double y = (((unsigned)serialised_val[4] << 24) & 0xFF000000) | (((unsigned)serialised_val[5] << 16) & 0xFF0000) | (((unsigned)serialised_val[6] << 8) & 0xFF00)  | (((unsigned)serialised_val[7]) & 0xFF);
	double z = (((unsigned)serialised_val[8] << 24) & 0xFF000000) | (((unsigned)serialised_val[9] << 16) & 0xFF0000) | (((unsigned)serialised_val[10] << 8) & 0xFF00) | (((unsigned)serialised_val[11]) & 0xFF);
	return Cartesian((x - MAXDOU2INT) / DOUBLE2INT, (y - MAXDOU2INT) / DOUBLE2INT, (z - MAXDOU2INT) / DOUBLE2INT);
}


uint64_t
Unserialise::trixel_id(std::string_view serialised_id)
{
	if (serialised_id.size() != HTM_BYTES_ID) {
		THROW(SerialisationError, "Cannot unserialise trixel_id: %s [%zu]", repr(serialised_id).c_str(), serialised_id.size());
	}

	uint64_t id = (((uint64_t)serialised_id[0] << 48) & 0xFF000000000000) | (((uint64_t)serialised_id[1] << 40) & 0xFF0000000000) | \
				  (((uint64_t)serialised_id[2] << 32) & 0xFF00000000)     | (((uint64_t)serialised_id[3] << 24) & 0xFF000000)     | \
				  (((uint64_t)serialised_id[4] << 16) & 0xFF0000)         | (((uint64_t)serialised_id[5] <<  8) & 0xFF00)         | \
				  (serialised_id[6] & 0xFF);
	return id;
}


range_t
Unserialise::range(std::string_view serialised_range)
{
	if (serialised_range.size() != SERIALISED_LENGTH_RANGE) {
		THROW(SerialisationError, "Cannot unserialise range_t: %s [%zu]", repr(serialised_range).c_str(), serialised_range.size());
	}

	uint64_t start = (((uint64_t)serialised_range[0] << 48) & 0xFF000000000000) | (((uint64_t)serialised_range[1] << 40) & 0xFF0000000000) | \
					 (((uint64_t)serialised_range[2] << 32) & 0xFF00000000)     | (((uint64_t)serialised_range[3] << 24) & 0xFF000000)     | \
					 (((uint64_t)serialised_range[4] << 16) & 0xFF0000)         | (((uint64_t)serialised_range[5] <<  8) & 0xFF00)         | \
					 (serialised_range[6] & 0xFF);

	uint64_t end = (((uint64_t)serialised_range[7] << 48) & 0xFF000000000000) | (((uint64_t)serialised_range[8] << 40) & 0xFF0000000000) | \
				   (((uint64_t)serialised_range[9] << 32) & 0xFF00000000)     | (((uint64_t)serialised_range[10] << 24) & 0xFF000000)    | \
				   (((uint64_t)serialised_range[11] << 16) & 0xFF0000)        | (((uint64_t)serialised_range[12] <<  8) & 0xFF00)        | \
				   (serialised_range[13] & 0xFF);

	return range_t(start, end);
}


FieldType
Unserialise::type(std::string_view str_type)
{
	switch (str_type.size()) {
		case 1:
			switch (str_type[0]) {
				case ' ':
				case 'e':
				case 'E':
					return FieldType::EMPTY;
				case 'a':
				case 'A':
					return FieldType::ARRAY;
				case 'b':
				case 'B':
					return FieldType::BOOLEAN;
				case 'd':
				case 'D':
					return FieldType::DATE;
				case 'f':
				case 'F':
					return FieldType::FLOAT;
				case 'g':
				case 'G':
					return FieldType::GEO;
				case 'i':
				case 'I':
					return FieldType::INTEGER;
				case 'o':
				case 'O':
					return FieldType::OBJECT;
				case 'p':
				case 'P':
					return FieldType::POSITIVE;
				case 's':
				case 'S':
					return FieldType::STRING;
				case 't':
				case 'T':
					return FieldType::TERM;
				case 'u':
				case 'U':
					return FieldType::UUID;
				case 'x':
				case 'X':
					return FieldType::SCRIPT;
			}
			break;

		case 4:
			switch (str_type[0]) {
				case 'd':
				case 'D': {
					static_assert(fnv1ah32::hash(DATE_STR) == fnv1ah32::hash("date"), "DATE_STR is not 'date', reorder switch.");
					auto lower_str_type = lower_string(str_type);
					if (lower_str_type == DATE_STR) {
						return FieldType::DATE;
					}
					break;
				}
				case 't':
				case 'T': {
					static_assert(fnv1ah32::hash(TERM_STR) == fnv1ah32::hash("term"), "TERM_STR is not 'term', reorder switch.");
					static_assert(fnv1ah32::hash(TEXT_STR) == fnv1ah32::hash("text"), "TEXT_STR is not 'text', reorder switch.");
					static_assert(fnv1ah32::hash(TIME_STR) == fnv1ah32::hash("time"), "TIME_STR is not 'time', reorder switch.");
					auto lower_str_type = lower_string(str_type);
					if (lower_str_type == TERM_STR) {
						return FieldType::TERM;
					} else if (lower_str_type == TEXT_STR) {
						return FieldType::TEXT;
					} else if (lower_str_type == TIME_STR) {
						return FieldType::TIME;
					}
					break;
				}
			}
			break;

		case 5:
			switch (str_type[0]) {
				case 'a':
				case 'A': {
					static_assert(fnv1ah32::hash(ARRAY_STR) == fnv1ah32::hash("array"), "ARRAY_STR is not 'array', reorder switch.");
					auto lower_str_type = lower_string(str_type);
					if (lower_str_type == ARRAY_STR) {
						return FieldType::ARRAY;
					}
					break;
				}
				case 'e':
				case 'E': {
					static_assert(fnv1ah32::hash(EMPTY_STR) == fnv1ah32::hash("empty"), "EMPTY_STR is not 'empty', reorder switch.");
					auto lower_str_type = lower_string(str_type);
					if (lower_str_type == EMPTY_STR) {
						return FieldType::EMPTY;
					}
					break;
				}
				case 'f':
				case 'F': {
					static_assert(fnv1ah32::hash(FLOAT_STR) == fnv1ah32::hash("float"), "FLOAT_STR is not 'float', reorder switch.");
					auto lower_str_type = lower_string(str_type);
					if (lower_str_type == FLOAT_STR) {
						return FieldType::FLOAT;
					}
					break;
				}
			}
			break;

		case 6:
			switch (str_type[0]) {
				case 'o':
				case 'O': {
					static_assert(fnv1ah32::hash(OBJECT_STR) == fnv1ah32::hash("object"), "OBJECT_STR is not 'object', reorder switch.");
					auto lower_str_type = lower_string(str_type);
					if (lower_str_type == OBJECT_STR) {
						return FieldType::OBJECT;
					}
					break;
				}
				case 's':
				case 'S': {
					static_assert(fnv1ah32::hash(SCRIPT_STR) == fnv1ah32::hash("script"), "SCRIPT_STR is not 'script', reorder switch.");
					static_assert(fnv1ah32::hash(STRING_STR) == fnv1ah32::hash("string"), "STRING_STR is not 'string', reorder switch.");
					auto lower_str_type = lower_string(str_type);
					if (lower_str_type == SCRIPT_STR) {
						return FieldType::SCRIPT;
					} else if (lower_str_type == STRING_STR) {
						return FieldType::STRING;
					}
					break;
				}
			}
			break;

		case 7:
			switch (str_type[0]) {
				case 'b':
				case 'B': {
					static_assert(fnv1ah32::hash(BOOLEAN_STR) == fnv1ah32::hash("boolean"), "BOOLEAN_STR is not 'boolean', reorder switch.");
					auto lower_str_type = lower_string(str_type);
					if (lower_str_type == BOOLEAN_STR) {
						return FieldType::BOOLEAN;
					}
					break;
				}
				case 'f':
				case 'F': {
					static_assert(fnv1ah32::hash(FOREIGN_STR) == fnv1ah32::hash("foreign"), "FOREIGN_STR is not 'foreign', reorder switch.");
					auto lower_str_type = lower_string(str_type);
					if (lower_str_type == FOREIGN_STR) {
						return FieldType::FOREIGN;
					}
					break;
				}
				case 'i':
				case 'I': {
					static_assert(fnv1ah32::hash(INTEGER_STR) == fnv1ah32::hash("integer"), "INTEGER_STR is not 'integer', reorder switch.");
					auto lower_str_type = lower_string(str_type);
					if (lower_str_type == INTEGER_STR) {
						return FieldType::INTEGER;
					}
					break;
				}
			}
			break;

		case 8:
			switch (str_type[0]) {
				case 'p':
				case 'P': {
					static_assert(fnv1ah32::hash(POSITIVE_STR) == fnv1ah32::hash("positive"), "POSITIVE_STR is not 'positive', reorder switch.");
					auto lower_str_type = lower_string(str_type);
					if (lower_str_type == POSITIVE_STR) {
						return FieldType::POSITIVE;
					}
					break;
				}
			}
			break;

		case 9:
			switch (str_type[0]) {
				case 't':
				case 'T': {
					static_assert(fnv1ah32::hash(TIMEDELTA_STR) == fnv1ah32::hash("timedelta"), "TIMEDELTA_STR is not 'timedelta', reorder switch.");
					auto lower_str_type = lower_string(str_type);
					if (lower_str_type == TIMEDELTA_STR) {
						return FieldType::TIMEDELTA;
					}
					break;
				}
			}
			break;

		case 10:
			switch (str_type[0]) {
				case 'g':
				case 'G': {
					static_assert(fnv1ah32::hash(GEO_STR) == fnv1ah32::hash("geospatial"), "GEO_STR is not 'geospatial', reorder switch.");
					auto lower_str_type = lower_string(str_type);
					if (lower_str_type == GEO_STR) {
						return FieldType::GEO;
					}
					break;
				}
			}
			break;
	}

	THROW(SerialisationError, "Type: %s is an unsupported type", repr(str_type).c_str());
}
