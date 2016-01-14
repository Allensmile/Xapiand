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

#pragma once

#include "msgpack_wrapper.h"
#include "rapidjson/document.h"
#include "msgpack_wrapper.h"

#include <regex>
#include <vector>
#include <xapian.h>

#define RESERVED_WEIGHT "_weight"
#define RESERVED_POSITION "_position"
#define RESERVED_LANGUAGE "_language"
#define RESERVED_SPELLING "_spelling"
#define RESERVED_POSITIONS "_positions"
#define RESERVED_TEXTS "_texts"
#define RESERVED_VALUES "_values"
#define RESERVED_TERMS "_terms"
#define RESERVED_DATA "_data"
#define RESERVED_ACCURACY "_accuracy"
#define RESERVED_ACC_PREFIX "_acc_prefix"
#define RESERVED_STORE "_store"
#define RESERVED_TYPE "_type"
#define RESERVED_ANALYZER "_analyzer"
#define RESERVED_DYNAMIC "_dynamic"
#define RESERVED_D_DETECTION "_date_detection"
#define RESERVED_N_DETECTION "_numeric_detection"
#define RESERVED_G_DETECTION "_geo_detection"
#define RESERVED_B_DETECTION "_bool_detection"
#define RESERVED_S_DETECTION "_string_detection"
#define RESERVED_BOOL_TERM "_bool_term"
#define RESERVED_VALUE "_value"
#define RESERVED_NAME "_name"
#define RESERVED_SLOT "_slot"
#define RESERVED_INDEX "_index"
#define RESERVED_PREFIX "_prefix"
#define RESERVED_ID "_id"
#define RESERVED_SCHEMA "_schema"
#define RESERVED_VERSION "_version"

#define DB_OFFSPRING_UNION "__"
#define DB_LANGUAGES "da nl en lovins porter fi fr de hu it nb nn no pt ro ru es sv tr"
#define DB_VERSION_SCHEMA 2.0

// Default prefixes
#define DOCUMENT_ID_TERM_PREFIX "Q"
#define DOCUMENT_CUSTOM_TERM_PREFIX "X"

#define JSON_TYPE "application/json"
#define FORM_URLENCODED_TYPE "application/x-www-form-urlencoded"
#define MSGPACK_TYPE "application/x-msgpack"


extern const std::regex find_types_re;


struct data_field_t {
	unsigned int slot;
	std::string prefix;
	char type;
	std::vector<double> accuracy;
	std::vector<std::string> acc_prefix;
	bool bool_term;
};


struct similar_field_t {
	unsigned int n_rset;
	unsigned int n_eset;
	unsigned int n_term; //If the number of subqueries is less than this threshold, OP_ELITE_SET behaves identically to OP_OR
	std::vector <std::string> field;
	std::vector <std::string> type;

	similar_field_t()
		: n_rset(5), n_eset(32), n_term(10) { }
};


struct query_field_t {
	unsigned int offset;
	unsigned int limit;
	unsigned int check_at_least;
	bool spelling;
	bool synonyms;
	bool pretty;
	bool commit;
	bool server;
	bool database;
	std::string document;
	bool unique_doc;
	bool is_fuzzy;
	bool is_nearest;
	std::string stats;
	std::string collapse;
	unsigned int collapse_max;
	std::vector <std::string> language;
	std::vector <std::string> query;
	std::vector <std::string> partial;
	std::vector <std::string> terms;
	std::vector <std::string> sort;
	std::vector <std::string> facets;
	similar_field_t fuzzy;
	similar_field_t nearest;

	query_field_t()
		: offset(0), limit(10), check_at_least(0), spelling(true), synonyms(false), pretty(false),
		  commit(false), server(false), database(false), document(""), unique_doc(false), is_fuzzy(false),
		  is_nearest(false), stats(""), collapse(""), collapse_max(1), fuzzy(), nearest() { }

};


enum class MIMEType {
	APPLICATION_JSON,
	APPLICATION_XWWW_FORM_URLENCODED,
	APPLICATION_X_MSGPACK,
	UNKNOW
};


long long read_mastery(const std::string& dir, bool force);
// All the field that start with '_' are considered reserved word.
bool is_reserved(const std::string& word);
bool is_language(const std::string& language);
bool set_types(const std::string& type, std::vector<char>& sep_types);
std::string str_type(const std::vector<char>& sep_types);
std::vector<std::string> split_fields(const std::string& field_name);
void clean_reserved(MsgPack& document);
MIMEType get_mimetype(const std::string& type);
void json_load(rapidjson::Document& doc, const std::string& str);
void apply_patch(const MsgPack& patch, MsgPack& object);
bool patch_add(const MsgPack& obj_patch, MsgPack& object);
bool patch_remove(const MsgPack& obj_patch, MsgPack& object);
bool patch_replace(const MsgPack& obj_patch, MsgPack& object);
bool patch_move(const MsgPack& obj_patch, MsgPack& object);
bool patch_copy(const MsgPack& obj_patch, MsgPack& object);
bool patch_test(const MsgPack& obj_patch, MsgPack& object);
MsgPack get_patch_path(const MsgPack& obj_patch, MsgPack& object, const char* path, std::string& target, bool verify_exist=false);
MsgPack get_patch_value(const MsgPack& obj_patch);
