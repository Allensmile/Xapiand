/*
 * Copyright (C) 2015,2016,2017 deipi.com LLC and contributors. All rights reserved.
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

#include <cctype>                // for isxdigit
#include <stddef.h>              // for size_t
#include <string>                // for string
#include <sys/types.h>           // for uint64_t, int64_t, uint32_t, uint8_t
#include <tuple>                 // for tuple
#include <unordered_map>         // for unordered_map
#include <utility>               // for pair
#include <vector>                // for vector

#include "database_utils.h"      // for get_hashed, RESERVED_BOOLEAN, RESERV...
#include "datetime.h"            // for tm_t (ptr only), timestamp
#include "geo/cartesian.h"       // for Cartesian
#include "geo/htm.h"             // for range_t
#include "hash/endian.h"         // for __BYTE_ORDER, __BIG_ENDIAN, __LITTLE...
#include "length.h"              // for serialise_length, unserialise_length
#include "msgpack.h"             // for MsgPack
#include "sortable_serialise.h"  // for sortable_serialise, sortable_unseria...
#include "xxh64.hpp"             // for xxh64


#ifndef __has_builtin         // Optional of course
  #define __has_builtin(x) 0  // Compatibility with non-clang compilers
#endif
#if (defined(__clang__) && __has_builtin(__builtin_bswap32) && __has_builtin(__builtin_bswap64)) || (defined(__GNUC__ ) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 3)))
//  GCC and Clang recent versions provide intrinsic byte swaps via builtins
//  prior to 4.8, gcc did not provide __builtin_bswap16 on some platforms so we emulate it
//  see http://gcc.gnu.org/bugzilla/show_bug.cgi?id=52624
//  Clang has a similar problem, but their feature test macros make it easier to detect
#  if (defined(__clang__) && __has_builtin(__builtin_bswap16)) || (defined(__GNUC__) &&(__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 8)))
#    define BYTE_SWAP_2(x) (__builtin_bswap16(x))
#  else
#    define BYTE_SWAP_2(x) (__builtin_bswap32((x) << 16))
#  endif
#  define BYTE_SWAP_4(x) (__builtin_bswap32(x))
#  define BYTE_SWAP_8(x) (__builtin_bswap64(x))
#elif defined(__linux__)
// Linux systems provide the byteswap.h header, with
// don't check for obsolete forms defined(linux) and defined(__linux) on the theory that
// compilers that predefine only these are so old that byteswap.h probably isn't present.
#  include <byteswap.h>

#  define BYTE_SWAP_2(x) (bswap_16(x))
#  define BYTE_SWAP_4(x) (bswap_32(x))
#  define BYTE_SWAP_8(x) (bswap_64(x))
#elif defined(_MSC_VER)
// Microsoft documents these as being compatible since Windows 95 and specificly
// lists runtime library support since Visual Studio 2003 (aka 7.1).
#  include <cstdlib>

#  define BYTE_SWAP_2(x) (_byteswap_ushort(x))
#  define BYTE_SWAP_4(x) (_byteswap_ulong(x))
#  define BYTE_SWAP_8(x) (_byteswap_uint64(x))
#else
#  define BYTE_SWAP_2(x) ((((x) & 0xFF00) >> 8) | (((x) & 0x00FF) << 8))
#  define BYTE_SWAP_4(x) ((BYTE_SWAP_2(((x) & 0xFFFF0000) >> 16)) | ((BYTE_SWAP_2((x) & 0x0000FFFF)) << 16))
#  define BYTE_SWAP_8(x) ((BYTE_SWAP_8(((x) & 0xFFFFFFFF00000000) >> 32)) | ((BYTE_SWAP_8((x) & 0x00000000FFFFFFFF)) << 32))
#endif


#if __BYTE_ORDER == __BIG_ENDIAN
// No translation needed for big endian system.
#  define Swap7Bytes(val) (val) // HTM's trixel's ids are represent in 7 bytes.
#  define Swap2Bytes(val) (val) // uint16_t, short in 2 bytes
#  define Swap4Bytes(val) (val) // Unsigned int is represent in 4 bytes
#  define Swap8Bytes(val) (val) // uint64_t is represent in 8 bytes
#elif __BYTE_ORDER == __LITTLE_ENDIAN
// Swap 7 byte, 56 bit values. (If it is not big endian, It is considered little endian)
#  define Swap2Bytes(val) BYTE_SWAP_2(val)
#  define Swap4Bytes(val) BYTE_SWAP_4(val)
#  define Swap7Bytes(val) (BYTE_SWAP_8((val) << 8))
#  define Swap8Bytes(val) BYTE_SWAP_8(val)
#endif


#define FLOAT_STR    "float"
#define INTEGER_STR  "integer"
#define POSITIVE_STR "positive"
#define TERM_STR     "term"
#define TEXT_STR     "text"
#define STRING_STR   "string"
#define DATE_STR     "date"
#define GEO_STR      "geospatial"
#define BOOLEAN_STR  "boolean"
#define UUID_STR     "uuid"
#define ARRAY_STR    "array"
#define OBJECT_STR   "object"
#define EMPTY_STR    "empty"


constexpr char SERIALISED_FALSE      = 'f';
constexpr char SERIALISED_TRUE       = 't';
constexpr char SERIALISED_STL_MAGIC  = '\0';


constexpr uint8_t SERIALISED_LENGTH_CARTESIAN = 12;
constexpr uint8_t SERIALISED_LENGTH_RANGE     = 2 * HTM_BYTES_ID;


constexpr uint32_t DOUBLE2INT = 1000000000;
constexpr uint32_t MAXDOU2INT =  999999999;


constexpr uint8_t SIZE_UUID                 = 36;
constexpr uint8_t SIZE_CURLY_BRACES_UUID    = 38;
constexpr uint8_t MAX_SIZE_BASE64_UUID      = 24;


struct required_spc_t;
enum class FieldType : uint8_t;


namespace Cast {
	enum class Hash : uint64_t {
		INTEGER           = xxh64::hash(RESERVED_INTEGER),
		POSITIVE          = xxh64::hash(RESERVED_POSITIVE),
		FLOAT             = xxh64::hash(RESERVED_FLOAT),
		BOOLEAN           = xxh64::hash(RESERVED_BOOLEAN),
		TERM              = xxh64::hash(RESERVED_TERM),
		TEXT              = xxh64::hash(RESERVED_TEXT),
		STRING            = xxh64::hash(RESERVED_STRING),
		UUID              = xxh64::hash(RESERVED_UUID),
		DATE              = xxh64::hash(RESERVED_DATE),
		EWKT              = xxh64::hash(RESERVED_EWKT),
		POINT             = xxh64::hash(RESERVED_POINT),
		POLYGON           = xxh64::hash(RESERVED_POLYGON),
		CIRCLE            = xxh64::hash(RESERVED_CIRCLE),
		CHULL             = xxh64::hash(RESERVED_CHULL),
		MULTIPOINT        = xxh64::hash(RESERVED_MULTIPOINT),
		MULTIPOLYGON      = xxh64::hash(RESERVED_MULTIPOLYGON),
		MULTICIRCLE       = xxh64::hash(RESERVED_MULTICIRCLE),
		MULTICHULL        = xxh64::hash(RESERVED_MULTICHULL),
		GEO_COLLECTION    = xxh64::hash(RESERVED_GEO_COLLECTION),
		GEO_INTERSECTION  = xxh64::hash(RESERVED_GEO_INTERSECTION),
	};

	/*
	 * Functions for doing cast between types.
	 */

	MsgPack cast(const MsgPack& obj);
	MsgPack cast(FieldType type, const std::string& field_value);
	int64_t integer(const MsgPack& obj);
	uint64_t positive(const MsgPack& obj);
	double _float(const MsgPack& obj);
	std::string string(const MsgPack& obj);
	bool boolean(const MsgPack& obj);
	MsgPack date(const MsgPack& obj);

	FieldType getType(const std::string& cast_word);
};


namespace Serialise {
	inline static bool isText(const std::string& field_value, bool bool_term) noexcept {
		return !bool_term && field_value.find(' ') != std::string::npos;
	}

	// Returns if field_value is UUID.
	bool isUUID(const std::string& field_value) noexcept;


	/*
	 * Serialise field_value according to field_spc.
	 */

	std::string MsgPack(const required_spc_t& field_spc, const class MsgPack& field_value);
	std::string cast_object(const required_spc_t& field_spc, const class MsgPack& o);
	std::string serialise(const required_spc_t& field_spc, const std::string& field_value);
	std::string string(const required_spc_t& field_spc, const std::string& field_value);
	std::string date(const required_spc_t& field_spc, const class MsgPack& field_value);


	/*
	 * Serialise field_value according to field_type.
	 */

	std::string _float(FieldType field_type, double field_value);
	std::string integer(FieldType field_type, int64_t field_value);
	std::string positive(FieldType field_type, uint64_t field_value);
	std::string boolean(FieldType field_type, bool field_value);


	/*
	 * Given a field_value, it gets the type.
	 *
	 * If bool_term can not return FieldType::TEXT.
	 *
	 * Returns the type and the serialised values according to type.
	 */

	std::pair<FieldType, std::string> get_type(const std::string& field_value, bool bool_term=false);
	std::pair<FieldType, std::string> get_type(const class MsgPack& field_value, bool bool_term=false);
	std::tuple<FieldType, std::string, std::string> get_range_type(const std::string& start, const std::string& end, bool bool_term=false);
	std::tuple<FieldType, std::string, std::string> get_range_type(const class MsgPack& obj, bool bool_term=false);

	// Serialise field_value like date.
	std::string date(const std::string& field_value);
	std::string date(const class MsgPack& field_value);

	inline std::string timestamp(double field_value) {
		return sortable_serialise(field_value);
	}

	// Serialise value like date and fill tm.
	std::string date(const class MsgPack& value, Datetime::tm_t& tm);

	inline std::string date(Datetime::tm_t& tm) {
		return sortable_serialise(Datetime::timestamp(tm));
	}

	// Serialise field_value like float.
	std::string _float(const std::string& field_value);

	inline std::string _float(double field_value) {
		return sortable_serialise(field_value);
	}

	// Serialise field_value like integer.
	std::string integer(const std::string& field_value);

	inline std::string integer(int64_t field_value) {
		return sortable_serialise(field_value);
	}

	// Serialise field_value like positive integer.
	std::string positive(const std::string& field_value);

	inline std::string positive(uint64_t field_value) {
		return sortable_serialise(field_value);
	}

	// Serialise field_value like UUID.
	std::string uuid(const std::string& field_value);

	// Serialise field_value like EWKT.
	std::string ewkt(const std::string& field_value, bool partials, double error);

	// Serialise a vector of ranges (HTM).
	std::string ranges(const std::vector<range_t>& ranges);

	// Serialise a geo data.
	std::string geo(const std::vector<range_t>& ranges, const std::vector<Cartesian>& centroids);

	// Serialise field_value like boolean.
	std::string boolean(const std::string& field_value);

	inline std::string boolean(bool field_value) {
		return std::string(1, field_value ? SERIALISED_TRUE : SERIALISED_FALSE);
	}

	// Serialise a normalize cartesian coordinate in SERIALISED_LENGTH_CARTESIAN bytes.
	std::string cartesian(const Cartesian& norm_cartesian);

	// Serialise a trixel's id (HTM).
	std::string trixel_id(uint64_t id);

	// Serialise a range_t.
	std::string range(const range_t& range);

	// Serialise type to its string representation.
	std::string type(FieldType type);

	inline std::string serialise(const std::string& val) {
		return val;
	}

	inline std::string serialise(int64_t val) {
		return integer(val);
	}

	inline std::string serialise(uint64_t val) {
		return positive(val);
	}

	inline std::string serialise(bool val) {
		return boolean(val);
	}

	inline std::string serialise(double val) {
		return _float(val);
	}

	inline std::string serialise(Datetime::tm_t& tm) {
		return date(tm);
	}

	inline std::string serialise(const std::vector<range_t>& val) {
		return ranges(val);
	}


	/*
	 * Serialise functions to serialise a range of values.
	 */

	template <typename InputIt>
	std::string STLString(InputIt first, InputIt last) {
		const auto size = std::distance(first, last);
		if (size == 1) {
			return *first;
		} else if (size > 1) {
			std::string serialised(1, SERIALISED_STL_MAGIC);
			for ( ; first != last; ++first) {
				serialised.append(serialise_length(first->length())).append(*first);
			}
			return serialised;
		}

		return std::string();
	}

	template <typename InputIt>
	std::string STLCartesian(InputIt first, InputIt last) {
		const auto size = std::distance(first, last);
		if (size == 1) {
			return Serialise::cartesian(*first);
		} else if (size > 1) {
			std::string serialised;
			serialised.reserve(SERIALISED_LENGTH_CARTESIAN * size + 1);
			serialised.push_back(SERIALISED_STL_MAGIC);
			for ( ; first != last; ++first) {
				serialised.append(Serialise::cartesian(*first));
			}
			return serialised;
		}

		return std::string();
	}

	template <typename InputIt>
	std::string STLRange(InputIt first, InputIt last) {
		const auto size = std::distance(first, last);
		if (size == 1) {
			return Serialise::range(*first);
		} else if (size > 1) {
			std::string serialised;
			serialised.reserve(SERIALISED_LENGTH_RANGE * size + 1);
			serialised.push_back(SERIALISED_STL_MAGIC);
			for ( ; first != last; ++first) {
				serialised.append(Serialise::range(*first));
			}
			return serialised;
		}

		return std::string();
	}
};


namespace Unserialise {
	// Unserialise serialised_val according to field_type and returns a MsgPack.
	MsgPack MsgPack(FieldType field_type, const std::string& serialised_val);

	// Unserialise serialised_val according to field_type.
	std::string unserialise(FieldType field_type, const std::string& serialised_val);

	// Unserialise a serialised float.
	inline double _float(const std::string& serialised_float) {
		return sortable_unserialise(serialised_float);
	}

	// Unserialise a serialised integer.
	inline int64_t integer(const std::string& serialised_integer) {
		return sortable_unserialise(serialised_integer);
	}

	// Unserialise a serialised positive.
	inline uint64_t positive(const std::string& serialised_positive) {
		return sortable_unserialise(serialised_positive);
	}

	// Unserialise a serialised date.
	std::string date(const std::string& serialised_date);

	// Unserialise a serialised date and returns the timestamp.
	inline double timestamp(const std::string& serialised_timestamp) {
		return sortable_unserialise(serialised_timestamp);
	}

	// Unserialise a serialised boolean.
	inline bool boolean(const std::string& serialised_boolean) {
		return serialised_boolean.at(0) == SERIALISED_TRUE;
	}

	// Unserialise a serialised cartesian coordinate.
	Cartesian cartesian(const std::string& serialised_cartesian);

	// Unserialise a serialised trixel's id (HTM).
	uint64_t trixel_id(const std::string& serialised_id);

	// Unserialise a serialised range_t
	range_t range(const std::string& serialised_range);

	// Unserialise a serialised UUID.
	std::string uuid(const std::string& serialised_uuid);

	// Unserialise a serialised GEO (save as value), in serialised ranges and serialises centroids.
	std::string ewkt(const std::string& serialised_geo);

	// Unserialise a serialised GEO (Save as value).
	std::vector<range_t> ranges(const std::string& serialised_geo);

	// Unserialise a serialised GEO (Save as a value), returns unserialised ranges and centroids.
	std::pair<std::string, std::string> geo(const std::string& serialised_geo);

	// Unserialise str_type to its FieldType.
	FieldType type(const std::string& str_type);


	/*
	 * Unserialise functions for add the unserialised range of values  in d_first.
	 */

	template <typename OutputIt>
	void STLString(const char** ptr, const char* end, OutputIt d_first) {
		const char* pos = *ptr;
		if (pos != end) {
			if (*pos == SERIALISED_STL_MAGIC) {
				++pos;
				for ( ; pos != end; ++d_first) {
					const auto length = unserialise_length(&pos, end, true);
					*d_first = std::string(pos, length);
					pos += length;
				}
			} else {
				*d_first = std::string(pos, end - pos);
			}
		}
	}

	template <typename OutputIt>
	inline void STLString(const std::string& serialised, OutputIt d_first) {
		const char* ptr = serialised.data();
		const char* end = serialised.data() + serialised.length();
		STLString(&ptr, end, d_first);
	}

	template <typename OutputIt>
	void STLCartesian(const char** ptr, const char* end, OutputIt d_first) {
		const char* pos = *ptr;
		if (pos != end) {
			if (*pos == SERIALISED_STL_MAGIC) {
				++pos;
				for ( ; end - pos >= SERIALISED_LENGTH_CARTESIAN; ++d_first) {
					*d_first = Unserialise::cartesian(std::string(pos, SERIALISED_LENGTH_CARTESIAN));
					pos += SERIALISED_LENGTH_CARTESIAN;
				}
			} else if (end - pos == SERIALISED_LENGTH_CARTESIAN) {
				*d_first = Unserialise::cartesian(std::string(pos, SERIALISED_LENGTH_CARTESIAN));
			}
		}
	}

	template <typename OutputIt>
	inline void STLCartesian(const std::string& serialised, OutputIt d_first) {
		const char* ptr = serialised.data();
		const char* end = serialised.data() + serialised.length();
		STLCartesian(&ptr, end, d_first);
	}

	template <typename OutputIt>
	void STLRange(const char** ptr, const char* end, OutputIt d_first) {
		const char* pos = *ptr;
		if (pos != end) {
			if (*pos == SERIALISED_STL_MAGIC) {
				++pos;
				for ( ; end - pos >= SERIALISED_LENGTH_RANGE; ++d_first) {
					*d_first = Unserialise::range(std::string(pos, SERIALISED_LENGTH_RANGE));
					pos += SERIALISED_LENGTH_RANGE;
				}
			} else if (end - pos == SERIALISED_LENGTH_RANGE) {
				*d_first = Unserialise::range(std::string(pos, SERIALISED_LENGTH_RANGE));
			}
		}
	}

	template <typename OutputIt>
	inline void STLRange(const std::string& serialised, OutputIt d_first) {
		const char* ptr = serialised.data();
		const char* end = serialised.data() + serialised.length();
		STLRange(&ptr, end, d_first);
	}
};
