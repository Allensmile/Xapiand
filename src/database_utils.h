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

#include <string>                  // for string
#include <vector>                  // for vector
#include <xapian.h>                // for valueno

#include "msgpack.h"               // for object
#include "rapidjson/document.h"    // for Document
#include "sortable_serialise.h"    // for sortable_serialise
#include "xxh64.hpp"               // for xxh64


// Reserved words only used in the responses to the user.
#define RESERVED_ENDPOINT              "_endpoint"
#define RESERVED_RANK                  "_rank"
#define RESERVED_PERCENT               "_percent"

// Reserved words used in schema.
#define ID_FIELD_NAME                  "_id"
#define CT_FIELD_NAME                  "_ct"
#define UUID_FIELD_NAME                "<uuid_field>"

#define RESERVED_WEIGHT                "_weight"
#define RESERVED_POSITION              "_position"
#define RESERVED_SPELLING              "_spelling"
#define RESERVED_POSITIONS             "_positions"
#define RESERVED_LANGUAGE              "_language"
#define RESERVED_ACCURACY              "_accuracy"
#define RESERVED_ACC_PREFIX            "_accuracy_prefix"
#define RESERVED_ACC_GPREFIX           "_accuracy_gprefix"
#define RESERVED_STORE                 "_store"
#define RESERVED_TYPE                  "_type"
#define RESERVED_DYNAMIC               "_dynamic"
#define RESERVED_STRICT                "_strict"
#define RESERVED_BOOL_TERM             "_bool_term"
#define RESERVED_VALUE                 "_value"
#define RESERVED_SLOT                  "_slot"
#define RESERVED_INDEX                 "_index"
#define RESERVED_PREFIX                "_prefix"
#define RESERVED_VERSION               "_version"
#define RESERVED_SCRIPT                "_script"
#define RESERVED_BODY                  "_body"
#define RESERVED_RECURSE               "_recurse"
#define RESERVED_NAMESPACE             "_namespace"
#define RESERVED_PARTIAL_PATHS         "_partial_paths"
#define RESERVED_INDEX_UUID_FIELD      "_index_uuid_field"
#define RESERVED_SCHEMA                "_schema"
// Reserved words for detecting types.
#define RESERVED_DATE_DETECTION        "_date_detection"
#define RESERVED_TIME_DETECTION        "_time_detection"
#define RESERVED_TIMEDELTA_DETECTION   "_timedelta_detection"
#define RESERVED_NUMERIC_DETECTION     "_numeric_detection"
#define RESERVED_GEO_DETECTION         "_geo_detection"
#define RESERVED_BOOL_DETECTION        "_bool_detection"
#define RESERVED_STRING_DETECTION      "_string_detection"
#define RESERVED_TEXT_DETECTION        "_text_detection"
#define RESERVED_TERM_DETECTION        "_term_detection"
#define RESERVED_UUID_DETECTION        "_uuid_detection"
// Reserved words used only in the root of the  document.
#define RESERVED_VALUES                "_values"
#define RESERVED_TERMS                 "_terms"
#define RESERVED_DATA                  "_data"
// Reserved words used in schema only for TEXT fields.
#define RESERVED_STOP_STRATEGY         "_stop_strategy"
#define RESERVED_STEM_STRATEGY         "_stem_strategy"
#define RESERVED_STEM_LANGUAGE         "_stem_language"
// Reserved words used in schema only for GEO fields.
#define RESERVED_PARTIALS              "_partials"
#define RESERVED_ERROR                 "_error"
// Reserved words used for doing explicit cast convertions
#define RESERVED_FLOAT                 "_float"
#define RESERVED_POSITIVE              "_positive"
#define RESERVED_INTEGER               "_integer"
#define RESERVED_BOOLEAN               "_boolean"
#define RESERVED_TERM                  "_term"
#define RESERVED_TEXT                  "_text"
#define RESERVED_STRING                "_string"
#define RESERVED_DATE                  "_date"
#define RESERVED_TIME                  "_time"
#define RESERVED_TIMEDELTA             "_timedelta"
#define RESERVED_UUID                  "_uuid"
#define RESERVED_EWKT                  "_ewkt"
#define RESERVED_POINT                 "_point"
#define RESERVED_CIRCLE                "_circle"
#define RESERVED_CONVEX                "_convex"
#define RESERVED_POLYGON               "_polygon"
#define RESERVED_CHULL                 "_chull"
#define RESERVED_MULTIPOINT            "_multipoint"
#define RESERVED_MULTICIRCLE           "_multicircle"
#define RESERVED_MULTICONVEX           "_multiconvex"
#define RESERVED_MULTIPOLYGON          "_multipolygon"
#define RESERVED_MULTICHULL            "_multichull"
#define RESERVED_GEO_COLLECTION        "_geometrycollection"
#define RESERVED_GEO_INTERSECTION      "_geometryintersection"


#define DB_META_SCHEMA         "_schema"
#define DB_SCHEMA              "schema"
#define DB_OFFSPRING_UNION     '.'
#define DB_VERSION_SCHEMA      1.0

#define DB_SLOT_RESERVED       20    // Reserved slots by special data
#define DB_RETRIES             3     // Number of tries to do an operation on a Xapian::Database or Document

#define DB_SLOT_ID             0     // Slot ID document
#define DB_SLOT_CONTENT_TYPE   1     // Slot content type data

#define DB_SLOT_NUMERIC        10    // Slot for saving global float/integer/positive values
#define DB_SLOT_DATE           11    // Slot for saving global date values
#define DB_SLOT_GEO            12    // Slot for saving global geo values
#define DB_SLOT_STRING         13    // Slot for saving global string/text values.
#define DB_SLOT_BOOLEAN        14    // Slot for saving global boolean values.
#define DB_SLOT_UUID           15    // Slot for saving global uuid values.
#define DB_SLOT_TIME           16    // Slot for saving global time values.
#define DB_SLOT_TIMEDELTA      17    // Slot for saving global timedelta values.


// Default prefixes
#define DOCUMENT_ID_TERM_PREFIX            "Q"
#define DOCUMENT_NAMESPACE_TERM_PREFIX     "N"
#define DOCUMENT_ACCURACY_TERM_PREFIX      "A"
#define DOCUMENT_CONTENT_TYPE_TERM_PREFIX  "C"
#define DOCUMENT_USER_DEFINED_TERM_PREFIX  "X"

#define DOCUMENT_DB_MASTER                 "M"
#define DOCUMENT_DB_SLAVE                  "S"


#define ANY_CONTENT_TYPE                "*/*"
#define HTML_CONTENT_TYPE               "text/html"
#define TEXT_CONTENT_TYPE               "text/plain"
#define JSON_CONTENT_TYPE               "application/json"
#define MSGPACK_CONTENT_TYPE            "application/msgpack"
#define X_MSGPACK_CONTENT_TYPE          "application/x-msgpack"
#define FORM_URLENCODED_CONTENT_TYPE    "application/www-form-urlencoded"
#define X_FORM_URLENCODED_CONTENT_TYPE  "application/x-www-form-urlencoded"


#define DATABASE_DATA_HEADER_MAGIC        0x11
#define DATABASE_DATA_HEADER_MAGIC_STORED 0x12
#define DATABASE_DATA_FOOTER_MAGIC        0x15


struct type_t {
	std::string first;
	std::string second;

	type_t() {
	}

	type_t(const std::pair<std::string, std::string>& pair) {
		first = pair.first;
		second = pair.second;
	}

	type_t(const std::string& first_, const std::string& second_) {
		first = first_;
		second = second_;
	}

	type_t(const std::string& ct_type_str) {
		const std::size_t found = ct_type_str.find_last_of('/');
		if (found != std::string::npos) {
			first = std::string(ct_type_str, 0, found);
			second = std::string(ct_type_str, found + 1);
		}
	}

	bool operator==(const type_t &other) const {
		return first == other.first && second == other.second;
	}
	bool operator!=(const type_t &other) const {
		return !operator==(other);
	}

	void clear() {
		first.clear();
		second.clear();
	}

	bool empty() const {
		return first.empty() && second.empty();
	}

	std::string to_string() const {
		return first + "/" + second;
	}
};

static const type_t no_type;
static const type_t any_type(ANY_CONTENT_TYPE);
static const type_t html_type(HTML_CONTENT_TYPE);
static const type_t text_type(TEXT_CONTENT_TYPE);
static const type_t json_type(JSON_CONTENT_TYPE);
static const type_t msgpack_type(MSGPACK_CONTENT_TYPE);
static const type_t x_msgpack_type(X_MSGPACK_CONTENT_TYPE);
static const auto msgpack_serializers = std::vector<type_t>({ json_type, msgpack_type, x_msgpack_type, html_type, text_type });

constexpr int DB_OPEN         = 0x0000; // Opens a database
constexpr int DB_WRITABLE     = 0x0001; // Opens as writable
constexpr int DB_SPAWN        = 0x0002; // Automatically creates the database if it doesn't exist
constexpr int DB_PERSISTENT   = 0x0004; // Always try keeping the database in the database pool
constexpr int DB_INIT_REF     = 0x0008; // Initializes the writable index in the database .refs
constexpr int DB_VOLATILE     = 0x0010; // Always drop the database from the database pool as soon as possible
constexpr int DB_REPLICATION  = 0x0020; // Use conditional pop in the queue, only pop when replication is done
constexpr int DB_NOWAL        = 0x0040; // Disable open wal file
constexpr int DB_NOSTORAGE    = 0x0080; // Disable separate data storage file for the database
constexpr int DB_COMMIT       = 0x0101; // Commits database when needed


enum class FieldType : uint8_t;


struct similar_field_t {
	unsigned n_rset;
	unsigned n_eset;
	unsigned n_term; // If the number of subqueries is less than this threshold, OP_ELITE_SET behaves identically to OP_OR
	std::vector <std::string> field;
	std::vector <std::string> type;

	similar_field_t()
		: n_rset(5), n_eset(32), n_term(10) { }
};


struct query_field_t {
	unsigned offset;
	unsigned limit;
	unsigned check_at_least;
	bool volatile_;
	bool spelling;
	bool synonyms;
	bool commit;
	bool unique_doc;
	bool is_fuzzy;
	bool is_nearest;
	std::string collapse;
	unsigned collapse_max;
	std::vector<std::string> query;
	std::vector<std::string> sort;
	similar_field_t fuzzy;
	similar_field_t nearest;
	std::string time;
	std::string period;

	// Only used when the sort type is string.
	std::string metric;
	bool icase;

	query_field_t()
		: offset(0), limit(10), check_at_least(0), volatile_(false), spelling(true), synonyms(false), commit(false),
		  unique_doc(false), is_fuzzy(false), is_nearest(false), collapse_max(1), icase(false) { }
};


// All non-empty field names not starting with underscore are valid.
inline bool is_valid(const std::string& field_name) {
	return !field_name.empty() && field_name.at(0) != '_';
}


inline std::string get_hashed(const std::string& name) {
	return sortable_serialise(xxh64::hash(name));
}


std::string prefixed(const std::string& term, const std::string& field_prefix, char field_type);
Xapian::valueno get_slot(const std::string& field_prefix, char field_type);
std::string get_prefix(unsigned long long field_number);
std::string get_prefix(const std::string& field_name);
std::string normalize_uuid(const std::string& uuid);
MsgPack normalize_uuid(const MsgPack& uuid);
long long read_mastery(const std::string& dir, bool force);
void json_load(rapidjson::Document& doc, const std::string& str);
rapidjson::Document to_json(const std::string& str);
std::string msgpack_to_html(const msgpack::object& o);
std::string msgpack_map_value_to_html(const msgpack::object& o);
std::string msgpack_to_html_error(const msgpack::object& o);


std::string join_data(bool stored, const std::string& stored_locator, const std::string& obj, const std::string& blob);
std::pair<bool, std::string> split_data_store(const std::string& data);
std::string split_data_obj(const std::string& data);
std::string split_data_blob(const std::string& data);
void split_path_id(const std::string& path_id, std::string& path, std::string& id);
#ifdef XAPIAND_DATA_STORAGE
std::tuple<ssize_t, size_t, size_t> storage_unserialise_locator(const std::string& store);
std::string storage_serialise_locator(ssize_t volume, size_t offset, size_t size);
#endif /* XAPIAND_DATA_STORAGE */