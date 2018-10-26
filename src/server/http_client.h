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

#pragma once

#include "xapiand.h"

#include <atomic>               // for atomic_bool
#include <chrono>               // for system_clock, time_point, duration
#include <memory>               // for shared_ptr, unique_ptr
#include <mutex>                // for mutex, lock_guard
#include <set>                  // for set
#include <stdio.h>              // for size_t, snprintf
#include <string>               // for string, operator==
#include <sys/types.h>          // for ssize_t
#include <tuple>                // for get, tuple
#include <utility>              // for pair
#include <vector>               // for vector

#include "atomic_shared_ptr.h"  // for atomic_shared_ptr
#include "base_client.h"        // for BaseClient
#include "cuuid/uuid.h"         // for UUIDGenerator
#include "database_handler.h"   // for DatabaseHandler
#include "database_utils.h"     // for query_field_t (ptr only)
#include "deflate_compressor.h" // for DeflateCompressData
#include "http_parser.h"        // for http_parser, http_parser_settings
#include "lru.h"                // for LRU
#include "phf.hh"               // for phf::make_phf
#include "msgpack.h"            // for MsgPack
#include "threadpool.h"         // for Task
#include "url_parser.h"         // for PathParser, QueryParser
#include "hashes.hh"            // for fnv1ah32

// #define L_CONN L_DEBUG

class UUIDGenerator;
class HttpServer;
class Logging;
class Worker;


#define HTTP_STATUS_RESPONSE            (1 << 0)
#define HTTP_HEADER_RESPONSE            (1 << 1)
#define HTTP_ACCEPT_RESPONSE            (1 << 2)
#define HTTP_BODY_RESPONSE              (1 << 3)
#define HTTP_CONTENT_TYPE_RESPONSE      (1 << 4)
#define HTTP_CONTENT_ENCODING_RESPONSE  (1 << 5)
#define HTTP_CONTENT_LENGTH_RESPONSE    (1 << 6)
#define HTTP_CHUNKED_RESPONSE           (1 << 7)
#define HTTP_OPTIONS_RESPONSE           (1 << 8)
#define HTTP_TOTAL_COUNT_RESPONSE       (1 << 9)
#define HTTP_MATCHES_ESTIMATED_RESPONSE (1 << 10)


class AcceptLRU : private lru::LRU<std::string, accept_set_t> {
	std::mutex qmtx;

public:
	AcceptLRU()
		: LRU<std::string, accept_set_t>(100) { }

	auto at(std::string key) {
		std::lock_guard<std::mutex> lk(qmtx);
		return LRU::at(key);
	}

	auto emplace(std::string key, accept_set_t set) {
		std::lock_guard<std::mutex> lk(qmtx);
		return LRU::emplace(key, set);
	}
};

struct AcceptEncoding {
	int position;
	double priority;

	std::string encoding;

	AcceptEncoding(int position, double priority, std::string encoding) : position(position), priority(priority), encoding(encoding) { }
};
using accept_encoding_t = std::set<AcceptEncoding, accept_preference_comp<AcceptEncoding>>;


class AcceptEncodingLRU : private lru::LRU<std::string, accept_encoding_t> {
	std::mutex qmtx;

public:
	AcceptEncodingLRU()
	: LRU<std::string, accept_encoding_t>(100) { }

	auto at(std::string key) {
		std::lock_guard<std::mutex> lk(qmtx);
		return LRU::at(key);
	}

	auto emplace(std::string key, accept_encoding_t set) {
		std::lock_guard<std::mutex> lk(qmtx);
		return LRU::emplace(key, set);
	}
};


enum class Encoding {
	none,
	gzip,
	deflate,
	identity,
	unknown,
};


// Available commands

constexpr const char COMMAND_COMMIT[]      = COMMAND_PREFIX "commit";
constexpr const char COMMAND_DUMP[]        = COMMAND_PREFIX "dump";
constexpr const char COMMAND_INFO[]        = COMMAND_PREFIX "info";
constexpr const char COMMAND_METADATA[]    = COMMAND_PREFIX "metadata";
constexpr const char COMMAND_METRICS[]     = COMMAND_PREFIX "metrics";
constexpr const char COMMAND_NODES[]       = COMMAND_PREFIX "nodes";
constexpr const char COMMAND_QUIT[]        = COMMAND_PREFIX "quit";
constexpr const char COMMAND_RESTORE[]     = COMMAND_PREFIX "restore";
constexpr const char COMMAND_SCHEMA[]      = COMMAND_PREFIX "schema";
constexpr const char COMMAND_SEARCH[]      = COMMAND_PREFIX "search";
constexpr const char COMMAND_STATS[]       = COMMAND_PREFIX "stats";
constexpr const char COMMAND_TOUCH[]       = COMMAND_PREFIX "touch";
constexpr const char COMMAND_WAL[]         = COMMAND_PREFIX "wal";
constexpr const char COMMAND_CHECK[]       = COMMAND_PREFIX "check";

#define COMMAND_OPTIONS() \
	OPTION(COMMIT) \
	OPTION(DUMP) \
	OPTION(INFO) \
	OPTION(METADATA) \
	OPTION(METRICS) \
	OPTION(NODES) \
	OPTION(QUIT) \
	OPTION(RESTORE) \
	OPTION(SCHEMA) \
	OPTION(SEARCH) \
	OPTION(STATS) \
	OPTION(TOUCH) \
	OPTION(WAL) \
	OPTION(CHECK)

constexpr static auto http_commands = phf::make_phf({
	#define OPTION(name) hhl(COMMAND_##name),
	COMMAND_OPTIONS()
	#undef OPTION
});


class Response {
public:
	std::string head;
	std::string headers;
	std::string body;

	ct_type_t ct_type;
	std::string blob;

	enum http_status status;
	size_t size;

	DeflateCompressData encoding_compressor;
	DeflateCompressData::iterator it_compressor;

	Response();
};


class Request {
	MsgPack _decoded_body;

	void _decode();

public:
	std::string _header_name;
	std::string _header_value;

	accept_set_t accept_set;
	accept_encoding_t accept_encoding_set;

	std::string path;
	struct http_parser parser;

	std::string head;
	std::string headers;
	std::string body;

	std::string raw;

	ct_type_t ct_type;
	std::string content_length;

	int indented;
	bool expect_100;

	std::string host;

	PathParser path_parser;
	QueryParser query_parser;

	std::shared_ptr<Logging> log;

	std::chrono::time_point<std::chrono::system_clock> begins;
	std::chrono::time_point<std::chrono::system_clock> received;
	std::chrono::time_point<std::chrono::system_clock> processing;
	std::chrono::time_point<std::chrono::system_clock> ready;
	std::chrono::time_point<std::chrono::system_clock> ends;

	~Request();
	Request(class HttpClient* client);

	const MsgPack& decoded_body() {
		_decode();
		return _decoded_body;
	}
};


// A single instance of a non-blocking Xapiand HTTP protocol handler.
class HttpClient : public BaseClient {
	enum class Command : uint32_t {
		#define OPTION(name) CMD_##name = http_commands.fhhl(COMMAND_##name),
		COMMAND_OPTIONS()
		#undef OPTION
		NO_CMD_NO_ID,
		NO_CMD_ID,
		BAD_QUERY,
	};

	Command getCommand(std::string_view command_name);

	ssize_t on_read(const char* buf, ssize_t received) override;
	void on_read_file(const char* buf, ssize_t received) override;
	void on_read_file_done() override;

	static const http_parser_settings settings;

	Request new_request;
	std::mutex requests_mutex;
	std::deque<Request> requests;
	Endpoints endpoints;

	static int _on_info(http_parser* parser);
	int on_info(http_parser* parser);
	static int _on_data(http_parser* parser, const char* at, size_t length);
	int on_data(http_parser* parser, const char* at, size_t length);

	void home_view(Request& request, Response& response, enum http_method method, Command cmd);
	void metrics_view(Request& request, Response& response, enum http_method method, Command cmd);
	void info_view(Request& request, Response& response, enum http_method method, Command cmd);
	void metadata_view(Request& request, Response& response, enum http_method method, Command cmd);
	void write_metadata_view(Request& request, Response& response, enum http_method method, Command cmd);
	void update_metadata_view(Request& request, Response& response, enum http_method method, Command cmd);
	void delete_metadata_view(Request& request, Response& response, enum http_method method, Command cmd);
	void delete_document_view(Request& request, Response& response, enum http_method method, Command cmd);
	void delete_schema_view(Request& request, Response& response, enum http_method method, Command cmd);
	void index_document_view(Request& request, Response& response, enum http_method method, Command cmd);
	void write_schema_view(Request& request, Response& response, enum http_method method, Command cmd);
	void document_info_view(Request& request, Response& response, enum http_method method, Command cmd);
	void update_document_view(Request& request, Response& response, enum http_method method, Command cmd);
	void search_view(Request& request, Response& response, enum http_method method, Command cmd);
	void touch_view(Request& request, Response& response, enum http_method method, Command cmd);
	void commit_view(Request& request, Response& response, enum http_method method, Command cmd);
	void dump_view(Request& request, Response& response, enum http_method method, Command cmd);
	void restore_view(Request& request, Response& response, enum http_method method, Command cmd);
	void schema_view(Request& request, Response& response, enum http_method method, Command cmd);
#if XAPIAND_DATABASE_WAL
	void wal_view(Request& request, Response& response, enum http_method method, Command cmd);
#endif
	void check_view(Request& request, Response& response, enum http_method method, Command cmd);
	void nodes_view(Request& request, Response& response, enum http_method method, Command cmd);

	void _options(Request& request, Response& response, enum http_method method);
	void _head(Request& request, Response& response, enum http_method method);
	void _get(Request& request, Response& response, enum http_method method);
	void _merge(Request& request, Response& response, enum http_method method);
	void _store(Request& request, Response& response, enum http_method method);
	void _put(Request& request, Response& response, enum http_method method);
	void _post(Request& request, Response& response, enum http_method method);
	void _patch(Request& request, Response& response, enum http_method method);
	void _delete(Request& request, Response& response, enum http_method method);

	Command url_resolve(Request& request);
	void _endpoint_maker(Request& request, bool master);
	void endpoints_maker(Request& request, bool master);
	query_field_t query_field_maker(Request& request, int flags);

	void log_request(Request& request);
	void log_response(Response& response);

	std::string http_response(Request& request, Response& response, enum http_status status, int mode, int total_count = 0, int matches_estimated = 0, const std::string& body = "", const std::string& ct_type = "application/json; charset=UTF-8", const std::string& ct_encoding = "", size_t content_length = 0);
	void clean_http_request(Request& request, Response& response);
	void set_idle();
	std::pair<std::string, std::string> serialize_response(const MsgPack& obj, const ct_type_t& ct_type, int indent, bool serialize_error=false);

	ct_type_t resolve_ct_type(Request& request, ct_type_t ct_type_str);
	template <typename T>
	const ct_type_t& get_acceptable_type(Request& request, const T& ct);
	const ct_type_t* is_acceptable_type(const ct_type_t& ct_type_pattern, const ct_type_t& ct_type);
	const ct_type_t* is_acceptable_type(const ct_type_t& ct_type_pattern, const std::vector<ct_type_t>& ct_types);
	void write_status_response(Request& request, Response& response, enum http_status status, const std::string& message="");
	void write_http_response(Request& request, Response& response, enum http_status status, const MsgPack& obj=MsgPack());
	Encoding resolve_encoding(Request& request);
	std::string readable_encoding(Encoding e);
	std::string encoding_http_response(Response& response, Encoding e, const std::string& response_obj, bool chunk, bool start, bool end);

	friend Worker;

public:
	std::string __repr__() const override {
		return Worker::__repr__("HttpClient");
	}

	HttpClient(std::shared_ptr<HttpServer> server_, ev::loop_ref* ev_loop_, unsigned int ev_flags_, int sock_);

	~HttpClient();

	void run_one(Request& request, Response& response);
	void run();
};
