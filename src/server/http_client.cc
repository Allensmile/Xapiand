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

#include "http_client.h"

#include <algorithm>                        // for move
#include <exception>                        // for exception
#include <functional>                       // for __base, function
#include <regex>                            // for regex_iterator, match_res...
#include <stdexcept>                        // for invalid_argument, range_e...
#include <cstdlib>                          // for mkstemp
#include <cstring>                          // for strerror, strcpy
#include <errno.h>                          // for __error, errno
#include <sysexits.h>                       // for EX_SOFTWARE
#include <syslog.h>                         // for LOG_WARNING, LOG_ERR, LOG...
#include <type_traits>                      // for enable_if<>::type
#include <xapian.h>                         // for version_string, MSetIterator

#if defined(XAPIAND_V8)
#include <v8-version.h>                       // for V8_MAJOR_VERSION, V8_MINOR_VERSION
#endif

#if defined(XAPIAND_CHAISCRIPT)
#include <chaiscript/chaiscript_defines.hpp>  // for chaiscript::Build_Info
#endif

#include "cppcodec/base64_rfc4648.hpp"      // for cppcodec::base64_rfc4648
#include "endpoint.h"                       // for Endpoints, Node, Endpoint
#include "ev/ev++.h"                        // for async, io, loop_ref (ptr ...
#include "exception.h"                      // for Exception, SerialisationE...
#include "cuuid/uuid.h"                     // for UUIDGenerator, UUID
#include "io_utils.h"                       // for close, write, unlink
#include "log.h"                            // for L_CALL, L_ERR, LOG_D...
#include "manager.h"                        // for XapiandManager, XapiandMa...
#include "msgpack.h"                        // for MsgPack, object::object, ...
#include "multivalue/aggregation.h"         // for AggregationMatchSpy
#include "multivalue/aggregation_metric.h"  // for AGGREGATION_AGGS
#include "opts.h"                           // for opts
#include "queue.h"                          // for Queue
#include "rapidjson/document.h"             // for Document
#include "schema.h"                         // for Schema
#include "serialise.h"                      // for boolean
#include "server.h"                         // for XapiandServer, XapiandSer...
#include "http_server.h"                    // for HttpServer
#include "threadpool.h"                     // for ThreadPool
#include "string.hh"                        // for string::from_delta
#include "package.h"                        // for Package
#include "hashes.hh"                        // for fnv1ah32


#define QUERY_FIELD_COMMIT     (1 << 0)
#define QUERY_FIELD_SEARCH     (1 << 1)
#define QUERY_FIELD_ID         (1 << 2)
#define QUERY_FIELD_TIME       (1 << 3)
#define QUERY_FIELD_PERIOD     (1 << 4)
#define QUERY_FIELD_VOLATILE   (1 << 5)


// Reserved words only used in the responses to the user.
constexpr const char RESPONSE_ENDPOINT[]            = "#endpoint";
constexpr const char RESPONSE_RANK[]                = "#rank";
constexpr const char RESPONSE_WEIGHT[]              = "#weight";
constexpr const char RESPONSE_PERCENT[]             = "#percent";
constexpr const char RESPONSE_TOTAL_COUNT[]         = "#total_count";
constexpr const char RESPONSE_MATCHES_ESTIMATED[]   = "#matches_estimated";
constexpr const char RESPONSE_HITS[]                = "#hits";
constexpr const char RESPONSE_AGGREGATIONS[]        = "#aggregations";
constexpr const char RESPONSE_QUERY[]               = "#query";
constexpr const char RESPONSE_MESSAGE[]             = "#message";
constexpr const char RESPONSE_STATUS[]              = "#status";
constexpr const char RESPONSE_TOOK[]                = "#took";
constexpr const char RESPONSE_NODES[]               = "#nodes";
constexpr const char RESPONSE_CLUSTER_NAME[]        = "#cluster_name";
constexpr const char RESPONSE_COMMIT[]              = "#commit";
constexpr const char RESPONSE_SERVER[]              = "#server";
constexpr const char RESPONSE_URL[]                 = "#url";
constexpr const char RESPONSE_VERSIONS[]            = "#versions";
constexpr const char RESPONSE_DELETE[]              = "#delete";
constexpr const char RESPONSE_DOCID[]               = "#docid";
constexpr const char RESPONSE_DOCUMENT_INFO[]       = "#document_info";
constexpr const char RESPONSE_DATABASE_INFO[]       = "#database_info";


static const std::regex header_params_re(R"(\s*;\s*([a-z]+)=(\d+(?:\.\d+)?))", std::regex::optimize);
static const std::regex header_accept_re(R"(([-a-z+]+|\*)/([-a-z+]+|\*)((?:\s*;\s*[a-z]+=\d+(?:\.\d+)?)*))", std::regex::optimize);
static const std::regex header_accept_encoding_re(R"(([-a-z+]+|\*)((?:\s*;\s*[a-z]+=\d+(?:\.\d+)?)*))", std::regex::optimize);

static const std::string eol("\r\n");


bool can_preview(const ct_type_t& ct_type) {
	#define CONTENT_TYPE_OPTIONS() \
		OPTION("application/eps") \
		OPTION("application/pdf") \
		OPTION("application/postscript") \
		OPTION("application/x-bzpdf") \
		OPTION("application/x-eps") \
		OPTION("application/x-gzpdf") \
		OPTION("application/x-pdf") \
		OPTION("application/x-photoshop") \
		OPTION("application/photoshop") \
		OPTION("application/psd")

	constexpr static auto _ = phf::make_phf({
		#define OPTION(ct) hhl(ct),
		CONTENT_TYPE_OPTIONS()
		#undef OPTION
	});
	switch (_.fhhl(ct_type.to_string())) {
		#define OPTION(ct) case _.fhhl(ct):
		CONTENT_TYPE_OPTIONS()
		#undef OPTION
			return true;
		default:
			return ct_type.first == "image";
	}
}


std::string
HttpClient::http_response(Request& request, Response& response, enum http_status status, int mode, int total_count, int matches_estimated, const std::string& body, const std::string& ct_type, const std::string& ct_encoding, size_t content_length) {
	L_CALL("HttpClient::http_response()");

	std::string head;
	std::string headers;
	std::string head_sep;
	std::string headers_sep;
	std::string response_text;

	if ((mode & HTTP_STATUS_RESPONSE) != 0) {
		response.status = status;
		auto http_major = request.parser.http_major;
		auto http_minor = request.parser.http_minor;
		if (http_major == 0 && http_minor == 0) {
			http_major = 1;
		}
		head += string::format("HTTP/%d.%d %d ", http_major, http_minor, status);
		head += http_status_str(status);
		head_sep += eol;
		if ((mode & HTTP_HEADER_RESPONSE) == 0) {
			headers_sep += eol;
		}
	}

	if ((mode & HTTP_HEADER_RESPONSE) != 0) {
		headers += "Server: " + Package::STRING + eol;

		if (!endpoints.empty()) {
			headers += "Database: " + endpoints.to_string() + eol;
		}

		request.ends = std::chrono::system_clock::now();

		if ((mode & HTTP_CHUNKED_RESPONSE) != 0) {
			headers += string::format("Response-Time: %lu", std::chrono::duration_cast<std::chrono::nanoseconds>(request.ends - request.begins).count()) + eol;
			if (request.ready >= request.processing) {
				headers += string::format("Operation-Time: %lu", std::chrono::duration_cast<std::chrono::nanoseconds>(request.ready - request.processing).count()) + eol;
			}
		}

		if ((mode & HTTP_OPTIONS_RESPONSE) != 0) {
			headers += "Allow: GET, POST, PUT, PATCH, MERGE, STORE, DELETE, HEAD, OPTIONS" + eol;
		}

		if ((mode & HTTP_TOTAL_COUNT_RESPONSE) != 0) {
			headers += string::format("Total-Count: %lu", total_count) + eol;
		}

		if ((mode & HTTP_MATCHES_ESTIMATED_RESPONSE) != 0) {
			headers += string::format("Matches-Estimated: %lu", matches_estimated) + eol;
		}

		if ((mode & HTTP_CONTENT_TYPE_RESPONSE) != 0 && !ct_type.empty()) {
			headers += "Content-Type: " + ct_type + eol;
		}

		if ((mode & HTTP_CONTENT_ENCODING_RESPONSE) != 0 && !ct_encoding.empty()) {
			headers += "Content-Encoding: " + ct_encoding + eol;
		}

		if ((mode & HTTP_CHUNKED_RESPONSE) != 0) {
			headers += "Transfer-Encoding: chunked" + eol;
		} else if ((mode & HTTP_CONTENT_LENGTH_RESPONSE) != 0) {
			headers += string::format("Content-Length: %lu", content_length) + eol;
		} else {
			headers += string::format("Content-Length: %lu", body.size()) + eol;
		}
		headers_sep += eol;
	}

	if ((mode & HTTP_BODY_RESPONSE) != 0) {
		if ((mode & HTTP_CHUNKED_RESPONSE) != 0) {
			response_text += string::format("%lx", body.size()) + eol;
			response_text += body + eol;
		} else {
			response_text += body;
		}
	}

	auto this_response_size = response_text.size();
	response.size += this_response_size;

	if (Logging::log_level > LOG_DEBUG) {
		response.head += head;
		response.headers += headers;
	}

	return head + head_sep + headers + headers_sep + response_text;
}


HttpClient::HttpClient(std::shared_ptr<HttpServer> server_, ev::loop_ref* ev_loop_, unsigned int ev_flags_, int sock_)
	: BaseClient(std::move(server_), ev_loop_, ev_flags_, sock_),
	  new_request(this)
{
	int http_clients = ++XapiandServer::http_clients;
	if (http_clients > XapiandServer::max_http_clients) {
		XapiandServer::max_http_clients = http_clients;
	}
	int total_clients = XapiandServer::total_clients;
	if (http_clients > total_clients) {
		L_CRIT("Inconsistency in number of http clients");
		sig_exit(-EX_SOFTWARE);
	}

	L_CONN("New Http Client in socket %d, %d client(s) of a total of %d connected.", sock_, http_clients, total_clients);

	idle = true;

	L_OBJ("CREATED HTTP CLIENT! (%d clients)", http_clients);
}


HttpClient::~HttpClient()
{
	int http_clients = --XapiandServer::http_clients;
	int total_clients = XapiandServer::total_clients;
	if (http_clients < 0 || http_clients > total_clients) {
		L_CRIT("Inconsistency in number of http clients");
		sig_exit(-EX_SOFTWARE);
	}

	if (XapiandManager::manager->shutdown_asap.load() != 0) {
		if (http_clients <= 0) {
			XapiandManager::manager->shutdown_sig(0);
		}
	}

	if (shutting_down || !(idle && write_queue.empty())) {
		L_WARNING("Client killed!");
	}

	L_OBJ("DELETED HTTP CLIENT! (%d clients left)", http_clients);
}


void
HttpClient::on_read(const char* buf, ssize_t received)
{
	L_CALL("HttpClient::on_read(<buf>, %zd)", received);

	unsigned init_state = new_request.parser.state;

	if (received <= 0) {
		if (received < 0) {
			L_WARNING("Connection unexpectedly closed after %s: %d - %s", string::from_delta(new_request.begins, std::chrono::system_clock::now()), errno, strerror(errno));
		} else if (init_state != 18) {
			L_WARNING("Client unexpectedly closed the other end after %s: Not in final HTTP state (%d)", string::from_delta(new_request.begins, std::chrono::system_clock::now()), init_state);
		} else if (!write_queue.empty()) {
			L_WARNING("Client unexpectedly closed the other end after %s: There was still pending data", string::from_delta(new_request.begins, std::chrono::system_clock::now()));
		}
		return;
	}

	L_HTTP_WIRE("HttpClient::on_read: %zd bytes", received);
	ssize_t parsed = http_parser_execute(&new_request.parser, &settings, buf, received);
	if (parsed != received) {
		enum http_status error_code = HTTP_STATUS_BAD_REQUEST;
		http_errno err = HTTP_PARSER_ERRNO(&new_request.parser);
		if (err == HPE_INVALID_METHOD) {
			Response response;
			write_http_response(new_request, response, HTTP_STATUS_NOT_IMPLEMENTED);
		} else {
			std::string message(http_errno_description(err));
			MsgPack err_response = {
				{ RESPONSE_STATUS, (int)error_code },
				{ RESPONSE_MESSAGE, string::split(message, '\n') }
			};
			Response response;
			write_http_response(new_request, response, error_code, err_response);
			L_WARNING(HTTP_PARSER_ERRNO(&new_request.parser) != HPE_OK ? message : "incomplete request");
		}
		destroy();  // Handle error. Just close the connection.
		detach();
	}
}


void
HttpClient::on_read_file(const char* /*buf*/, ssize_t received)
{
	L_CALL("HttpClient::on_read_file(<buf>, %zd)", received);

	L_ERR("Not Implemented: HttpClient::on_read_file: %zd bytes", received);
}


void
HttpClient::on_read_file_done()
{
	L_CALL("HttpClient::on_read_file_done()");

	L_ERR("Not Implemented: HttpClient::on_read_file_done");
}


// HTTP parser callbacks.
const http_parser_settings HttpClient::settings = {
	HttpClient::_on_info,  // on_message_begin
	HttpClient::_on_data,  // on_url
	HttpClient::_on_data,  // on_status
	HttpClient::_on_data,  // on_header_field
	HttpClient::_on_data,  // on_header_value
	HttpClient::_on_info,  // on_headers_complete
	HttpClient::_on_data,  // on_body
	HttpClient::_on_info,  // on_message_complete
	HttpClient::_on_info,  // on_chunk_header
	HttpClient::_on_info   // on_chunk_complete
};


int
HttpClient::_on_info(http_parser* parser)
{
	return static_cast<HttpClient *>(parser->data)->on_info(parser);
}


int
HttpClient::_on_data(http_parser* parser, const char* at, size_t length)
{
	return static_cast<HttpClient *>(parser->data)->on_data(parser, at, length);
}


int
HttpClient::on_info(http_parser* parser)
{
	L_CALL("HttpClient::on_info(...)");

	int state = parser->state;

	L_HTTP_PROTO_PARSER("%4d - (INFO)", state);

	switch (state) {
		case 18:  // message_complete
			if (!closed) {
				if (new_request.accept_set.empty()) {
					if (!new_request.ct_type.empty()) {
						new_request.accept_set.emplace(0, 1.0, new_request.ct_type, 0);
					}
					new_request.accept_set.emplace(1, 1.0, any_type, 0);
				}
				std::lock_guard<std::mutex> lk(requests_mutex);
				if (requests.empty()) {
					// Enqueue request...
					requests.push_back(std::move(new_request));
					// And start a runner.
					XapiandManager::manager->client_pool.enqueue([task = share_this<HttpClient>()]{
						task->run();
					});
				} else {
					// There should be a runner, just enqueue request.
					requests.push_back(std::move(new_request));
				}
			}
			new_request = Request(this);
			break;
		case 19:  // message_begin
			idle = false;
			new_request.begins = std::chrono::system_clock::now();
			new_request.log->clear();
			new_request.log = L_DELAYED(true, 10s, LOG_WARNING, PURPLE, "Request taking too long...").release();
			break;
		case 50:  // headers done
			new_request.head = string::format("%s %s HTTP/%d.%d", http_method_str(HTTP_PARSER_METHOD(parser)), new_request.path, parser->http_major, parser->http_minor);
			if (new_request.expect_100) {
				// Return 100 if client is expecting it
				Response response;
				write(http_response(new_request, response, HTTP_STATUS_CONTINUE, HTTP_STATUS_RESPONSE));
			}
			break;
		case 57: //s_chunk_data begin chunk
			break;
	}

	return 0;
}


int
HttpClient::on_data(http_parser* parser, const char* at, size_t length)
{
	L_CALL("HttpClient::on_data(...)");

	int state = parser->state;

	L_HTTP_PROTO_PARSER("%4d - %s", state, repr(at, length));

	if (state > 26 && state <= 32) {
		// s_req_path  ->  s_req_http_start
		new_request.path.append(at, length);
	} else if (state >= 43 && state <= 44) {
		// s_header_field  ->  s_header_value_discard_ws
		new_request._header_name.append(at, length);
	} else if (state >= 45 && state <= 50) {
		// s_header_value_discard_ws_almost_done  ->  s_header_almost_done
		new_request._header_value.append(at, length);
		if (Logging::log_level > LOG_DEBUG) {
			new_request.headers += new_request._header_name + ": " + new_request._header_value + eol;
		}
		if (state == 50) {
			constexpr static auto _ = phf::make_phf({
				hhl("host"),
				hhl("expect"),
				hhl("100-continue"),
				hhl("content-type"),
				hhl("content-length"),
				hhl("accept"),
				hhl("accept-encoding"),
				hhl("http-method-override"),
				hhl("x-http-method-override"),
			});

			switch (_.fhhl(new_request._header_name)) {
				case _.fhhl("host"):
					new_request.host = new_request._header_value;
					break;
				case _.fhhl("expect"):
				case _.fhhl("100-continue"):
					// Respond with HTTP/1.1 100 Continue
					new_request.expect_100 = true;
					break;

				case _.fhhl("content-type"):
					new_request.ct_type = ct_type_t(new_request._header_value);
					break;
				case _.fhhl("content-length"):
					new_request.content_length = new_request._header_value;
					break;
				case _.fhhl("accept"): {
					static AcceptLRU accept_sets;
					auto value = string::lower(new_request._header_value);
					try {
						new_request.accept_set = accept_sets.at(value);
					} catch (const std::out_of_range&) {
						std::sregex_iterator next(value.begin(), value.end(), header_accept_re, std::regex_constants::match_any);
						std::sregex_iterator end;
						int i = 0;
						while (next != end) {
							int indent = -1;
							double q = 1.0;
							if (next->length(3) != 0) {
								auto param = next->str(3);
								std::sregex_iterator next_param(param.begin(), param.end(), header_params_re, std::regex_constants::match_any);
								while (next_param != end) {
									if (next_param->str(1) == "q") {
										q = strict_stod(next_param->str(2));
									} else if (next_param->str(1) == "indent") {
										indent = strict_stoi(next_param->str(2));
										if (indent < 0) { indent = 0;
										} else if (indent > 16) { indent = 16; }
									}
									++next_param;
								}
							}
							new_request.accept_set.emplace(i, q, ct_type_t(next->str(1), next->str(2)), indent);
							++next;
							++i;
						}
						accept_sets.emplace(value, new_request.accept_set);
					}
					break;
				}

				case _.fhhl("accept-encoding"): {
					static AcceptEncodingLRU accept_encoding_sets;
					auto value = string::lower(new_request._header_value);
					try {
						new_request.accept_encoding_set = accept_encoding_sets.at(value);
					} catch (const std::out_of_range&) {
						std::sregex_iterator next(value.begin(), value.end(), header_accept_encoding_re, std::regex_constants::match_any);
						std::sregex_iterator end;
						int i = 0;
						while (next != end) {
							double q = 1.0;
							if (next->length(2) != 0) {
								auto param = next->str(2);
								std::sregex_iterator next_param(param.begin(), param.end(), header_params_re, std::regex_constants::match_any);
								while (next_param != end) {
									if (next_param->str(1) == "q") {
										q = strict_stod(next_param->str(2));
									}
									++next_param;
								}
							} else {
							}
							new_request.accept_encoding_set.emplace(i, q, next->str(1));
							++next;
							++i;
						}
						accept_encoding_sets.emplace(value, new_request.accept_encoding_set);
					}
					break;
				}

				case _.fhhl("x-http-method-override"):
				case _.fhhl("http-method-override"): {
					if (parser->method != HTTP_POST) {
						THROW(ClientError, "%s header must use the POST method", repr(new_request._header_name));
					}

					constexpr static auto __ = phf::make_phf({
						hhl("PUT"),
						hhl("PATCH"),
						hhl("MERGE"),
						hhl("STORE"),
						hhl("DELETE"),
						hhl("GET"),
						hhl("POST"),
					});

					switch (__.fhhl(new_request._header_value)) {
						case __.fhhl("PUT"):
							parser->method = HTTP_PUT;
							break;
						case __.fhhl("PATCH"):
							parser->method = HTTP_PATCH;
							break;
						case __.fhhl("MERGE"):
							parser->method = HTTP_MERGE;
							break;
						case __.fhhl("STORE"):
							parser->method = HTTP_STORE;
							break;
						case __.fhhl("DELETE"):
							parser->method = HTTP_DELETE;
							break;
						case __.fhhl("GET"):
							parser->method = HTTP_GET;
							break;
						case __.fhhl("POST"):
							parser->method = HTTP_POST;
							break;
						default:
							parser->http_errno = HPE_INVALID_METHOD;
							break;
					}
					break;
				}
			}

			// header used, expect next header
			new_request._header_name.clear();
			new_request._header_value.clear();
		}
	} else if (state >= 59 && state <= 62) { // s_chunk_data_done, s_body_identity  ->  s_message_done
		new_request.raw.append(at, length);
	}

	return 0;
}


void
HttpClient::run_one(Request& request, Response& response)
{
	written = 0;
	L_OBJ_BEGIN("HttpClient::run:BEGIN");

	request.log->clear();
	request.log = L_DELAYED(true, 1s, LOG_WARNING, PURPLE, "Response taking too long...").release();
	request.received = std::chrono::system_clock::now();

	std::string error;
	enum http_status error_code = HTTP_STATUS_OK;

	try {
		if (Logging::log_level > LOG_DEBUG) {
			log_request(request);
		}

		auto method = HTTP_PARSER_METHOD(&request.parser);
		switch (method) {
			case HTTP_DELETE:
				_delete(request, response, method);
				break;
			case HTTP_GET:
				_get(request, response, method);
				break;
			case HTTP_POST:
				_post(request, response, method);
				break;
			case HTTP_HEAD:
				_head(request, response, method);
				break;
			case HTTP_MERGE:
				_merge(request, response, method);
				break;
			case HTTP_STORE:
				_store(request, response, method);
				break;
			case HTTP_PUT:
				_put(request, response, method);
				break;
			case HTTP_OPTIONS:
				_options(request, response, method);
				break;
			case HTTP_PATCH:
				_patch(request, response, method);
				break;
			default:
				error_code = HTTP_STATUS_NOT_IMPLEMENTED;
				request.parser.http_errno = HPE_INVALID_METHOD;
				break;
		}
	} catch (const NotFoundError& exc) {
		error_code = HTTP_STATUS_NOT_FOUND;
		error.assign(http_status_str(error_code));
		// L_EXC("ERROR: %s", error);
	} catch (const MissingTypeError& exc) {
		error_code = HTTP_STATUS_PRECONDITION_FAILED;
		error.assign(exc.what());
		// L_EXC("ERROR: %s", error);
	} catch (const ClientError& exc) {
		error_code = HTTP_STATUS_BAD_REQUEST;
		error.assign(exc.what());
		// L_EXC("ERROR: %s", error);
	} catch (const TimeOutError& exc) {
		error_code = HTTP_STATUS_REQUEST_TIMEOUT;
		error.assign(std::string(http_status_str(error_code)) + ": " + exc.what());
		// L_EXC("ERROR: %s", error);
	} catch (const BaseException& exc) {
		error_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
		error.assign(*exc.get_message() != 0 ? exc.get_message() : "Unkown BaseException!");
		L_EXC("ERROR: %s", *exc.get_context() ? exc.get_context() : "Unkown BaseException!");
	} catch (const Xapian::Error& exc) {
		error_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
		error.assign(exc.get_description());
		L_EXC("ERROR: %s", error);
	} catch (const std::exception& exc) {
		error_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
		error.assign(*exc.what() != 0 ? exc.what() : "Unkown std::exception!");
		L_EXC("ERROR: %s", error);
	} catch (...) {
		error_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
		error.assign("Unknown exception!");
		std::exception exc;
		L_EXC("ERROR: %s", error);
	}

	if (error_code != HTTP_STATUS_OK) {
		if (written != 0) {
			destroy();
			detach();
		} else {
			MsgPack err_response = {
				{ RESPONSE_STATUS, (int)error_code },
				{ RESPONSE_MESSAGE, string::split(error, '\n') }
			};

			write_http_response(request, response, error_code, err_response);
		}
	}

	clean_http_request(request, response);

	L_OBJ_END("HttpClient::run:END");
}


void
HttpClient::run()
{
	L_CALL("HttpClient::run()");

	L_CONN("Start running in worker...");

	idle = false;
	try {

		{
			std::unique_lock<std::mutex> lk(requests_mutex);
			while (!requests.empty() && !closed) {
				auto& request = requests.front();
				Response response;
				lk.unlock();

				run_one(request, response);

				lk.lock();
				requests.pop_front();
			}
		}

		if (shutting_down && write_queue.empty()) {
			L_WARNING("Programmed shut down!");
			destroy();
			detach();
		}

	} catch (...) {
		idle = true;
		L_CONN("Running in worker ended with an exception.");
		detach();  // try re-detaching if already flagged as detaching
		throw;
	}
	idle = true;
	L_CONN("Running in worker ended.");
	redetach();  // try re-detaching if already flagged as detaching
}


void
HttpClient::_options(Request& request, Response& response, enum http_method /*unused*/)
{
	L_CALL("HttpClient::_options()");

	write(http_response(request, response, HTTP_STATUS_OK, HTTP_STATUS_RESPONSE | HTTP_HEADER_RESPONSE | HTTP_OPTIONS_RESPONSE | HTTP_BODY_RESPONSE));
}


void
HttpClient::_head(Request& request, Response& response, enum http_method method)
{
	L_CALL("HttpClient::_head()");

	auto cmd = url_resolve(request);
	switch (cmd) {
		case Command::NO_CMD_NO_ID:
			write_http_response(request, response, HTTP_STATUS_OK);
			break;
		case Command::NO_CMD_ID:
			document_info_view(request, response, method, cmd);
			break;
		default:
			write_status_response(request, response, HTTP_STATUS_METHOD_NOT_ALLOWED);
			break;
	}
}


void
HttpClient::_get(Request& request, Response& response, enum http_method method)
{
	L_CALL("HttpClient::_get()");

	auto cmd = url_resolve(request);
	switch (cmd) {
		case Command::NO_CMD_NO_ID:
			home_view(request, response, method, cmd);
			break;
		case Command::NO_CMD_ID:
			search_view(request, response, method, cmd);
			break;
		case Command::CMD_SEARCH:
			request.path_parser.skip_id();  // Command has no ID
			search_view(request, response, method, cmd);
			break;
		case Command::CMD_SCHEMA:
			request.path_parser.skip_id();  // Command has no ID
			schema_view(request, response, method, cmd);
			break;
#if XAPIAND_DATABASE_WAL
		case Command::CMD_WAL:
			request.path_parser.skip_id();  // Command has no ID
			wal_view(request, response, method, cmd);
			break;
#endif
		case Command::CMD_CHECK:
			request.path_parser.skip_id();  // Command has no ID
			check_view(request, response, method, cmd);
			break;
		case Command::CMD_INFO:
			request.path_parser.skip_id();  // Command has no ID
			info_view(request, response, method, cmd);
			break;
		case Command::CMD_METRICS:
			request.path_parser.skip_id();  // Command has no ID
			metrics_view(request, response, method, cmd);
			break;
		case Command::CMD_NODES:
			request.path_parser.skip_id();  // Command has no ID
			nodes_view(request, response, method, cmd);
			break;
		case Command::CMD_METADATA:
			request.path_parser.skip_id();  // Command has no ID
			metadata_view(request, response, method, cmd);
			break;
		default:
			write_status_response(request, response, HTTP_STATUS_METHOD_NOT_ALLOWED);
			break;
	}
}


void
HttpClient::_merge(Request& request, Response& response, enum http_method method)
{
	L_CALL("HttpClient::_merge()");


	auto cmd = url_resolve(request);
	switch (cmd) {
		case Command::NO_CMD_ID:
			update_document_view(request, response, method, cmd);
			break;
		case Command::CMD_METADATA:
			request.path_parser.skip_id();  // Command has no ID
			update_metadata_view(request, response, method, cmd);
			break;
		default:
			write_status_response(request, response, HTTP_STATUS_METHOD_NOT_ALLOWED);
			break;
	}
}


void
HttpClient::_store(Request& request, Response& response, enum http_method method)
{
	L_CALL("HttpClient::_store()");


	auto cmd = url_resolve(request);
	switch (cmd) {
		case Command::NO_CMD_ID:
			update_document_view(request, response, method, cmd);
			break;
		default:
			write_status_response(request, response, HTTP_STATUS_METHOD_NOT_ALLOWED);
			break;
	}
}


void
HttpClient::_put(Request& request, Response& response, enum http_method method)
{
	L_CALL("HttpClient::_put()");

	auto cmd = url_resolve(request);
	switch (cmd) {
		case Command::NO_CMD_ID:
			index_document_view(request, response, method, cmd);
			break;
		case Command::CMD_METADATA:
			request.path_parser.skip_id();  // Command has no ID
			write_metadata_view(request, response, method, cmd);
			break;
		case Command::CMD_SCHEMA:
			request.path_parser.skip_id();  // Command has no ID
			write_schema_view(request, response, method, cmd);
			break;
		default:
			write_status_response(request, response, HTTP_STATUS_METHOD_NOT_ALLOWED);
			break;
	}
}


void
HttpClient::_post(Request& request, Response& response, enum http_method method)
{
	L_CALL("HttpClient::_post()");

	auto cmd = url_resolve(request);
	switch (cmd) {
		case Command::NO_CMD_ID:
			request.path_parser.skip_id();  // Command has no ID
			index_document_view(request, response, method, cmd);
			break;
		case Command::CMD_SCHEMA:
			request.path_parser.skip_id();  // Command has no ID
			write_schema_view(request, response, method, cmd);
			break;
		case Command::CMD_SEARCH:
			request.path_parser.skip_id();  // Command has no ID
			search_view(request, response, method, cmd);
			break;
		case Command::CMD_TOUCH:
			request.path_parser.skip_id();  // Command has no ID
			touch_view(request, response, method, cmd);
			break;
		case Command::CMD_COMMIT:
			request.path_parser.skip_id();  // Command has no ID
			commit_view(request, response, method, cmd);
			break;
		case Command::CMD_DUMP:
			request.path_parser.skip_id();  // Command has no ID
			dump_view(request, response, method, cmd);
			break;
		case Command::CMD_RESTORE:
			request.path_parser.skip_id();  // Command has no ID
			restore_view(request, response, method, cmd);
			break;
#ifndef NDEBUG
		case Command::CMD_QUIT:
			XapiandManager::manager->shutdown_asap.store(epoch::now<>());
			XapiandManager::manager->shutdown_sig(0);
			break;
#endif
		default:
			write_status_response(request, response, HTTP_STATUS_METHOD_NOT_ALLOWED);
			break;
	}
}


void
HttpClient::_patch(Request& request, Response& response, enum http_method method)
{
	L_CALL("HttpClient::_patch()");

	auto cmd = url_resolve(request);
	switch (cmd) {
		case Command::NO_CMD_ID:
			update_document_view(request, response, method, cmd);
			break;
		default:
			write_status_response(request, response, HTTP_STATUS_METHOD_NOT_ALLOWED);
			break;
	}
}


void
HttpClient::_delete(Request& request, Response& response, enum http_method method)
{
	L_CALL("HttpClient::_delete()");

	auto cmd = url_resolve(request);
	switch (cmd) {
		case Command::NO_CMD_ID:
			delete_document_view(request, response, method, cmd);
			break;
		case Command::CMD_METADATA:
			request.path_parser.skip_id();  // Command has no ID
			delete_metadata_view(request, response, method, cmd);
			break;
		case Command::CMD_SCHEMA:
			request.path_parser.skip_id();  // Command has no ID
			delete_schema_view(request, response, method, cmd);
			break;
		default:
			write_status_response(request, response, HTTP_STATUS_METHOD_NOT_ALLOWED);
			break;
	}
}


void
HttpClient::home_view(Request& request, Response& response, enum http_method method, Command /*unused*/)
{
	L_CALL("HttpClient::home_view()");

	endpoints.clear();
	auto master_node_ = master_node.load();
	endpoints.add(Endpoint(".", master_node_.get()));

	request.processing = std::chrono::system_clock::now();

	DatabaseHandler db_handler(endpoints, DB_SPAWN, method);

	auto local_node_ = local_node.load();
	auto document = db_handler.get_document(local_node_->name());

	auto obj = document.get_obj();
	if (obj.find(ID_FIELD_NAME) == obj.end()) {
		obj[ID_FIELD_NAME] = document.get_field(ID_FIELD_NAME) || document.get_value(ID_FIELD_NAME);
	}

	request.ready = std::chrono::system_clock::now();

#ifdef XAPIAND_CLUSTERING
	obj[RESPONSE_CLUSTER_NAME] = opts.cluster_name;
#endif
	obj[RESPONSE_SERVER] = Package::STRING;
	obj[RESPONSE_URL] = Package::BUGREPORT;
	obj[RESPONSE_VERSIONS] = {
		{ "Xapiand", Package::REVISION.empty() ? Package::VERSION : string::format("%s_%s", Package::VERSION, Package::REVISION) },
		{ "Xapian", string::format("%d.%d.%d", Xapian::major_version(), Xapian::minor_version(), Xapian::revision()) },
#if defined(XAPIAND_V8)
		{ "V8", string::format("%u.%u", V8_MAJOR_VERSION, V8_MINOR_VERSION) },
#endif
#if defined(XAPIAND_CHAISCRIPT)
		{ "ChaiScript", string::format("%d.%d", chaiscript::Build_Info::version_major(), chaiscript::Build_Info::version_minor()) },
#endif
	};

	write_http_response(request, response, HTTP_STATUS_OK, obj);
}


void
HttpClient::metrics_view(Request& request, Response& response, enum http_method /*unused*/, Command /*unused*/)
{
	L_CALL("HttpClient::metrics_view()");

	endpoints_maker(request);

	request.processing = std::chrono::system_clock::now();

	auto server_info =  XapiandManager::manager->server_metrics();
	write(http_response(request, response, HTTP_STATUS_OK, HTTP_STATUS_RESPONSE | HTTP_HEADER_RESPONSE | HTTP_CONTENT_TYPE_RESPONSE | HTTP_CONTENT_LENGTH_RESPONSE | HTTP_BODY_RESPONSE, 0, 0, server_info, "text/plain", "", server_info.size()));
}


void
HttpClient::document_info_view(Request& request, Response& response, enum http_method method, Command /*unused*/)
{
	L_CALL("HttpClient::document_info_view()");

	endpoints_maker(request);

	request.processing = std::chrono::system_clock::now();

	DatabaseHandler db_handler(endpoints, DB_SPAWN, method);

	MsgPack response_obj;
	response_obj[RESPONSE_DOCID] = db_handler.get_docid(request.path_parser.get_id());

	request.ready = std::chrono::system_clock::now();

	write_http_response(request, response, HTTP_STATUS_OK, response_obj);
}


void
HttpClient::delete_document_view(Request& request, Response& response, enum http_method method, Command /*unused*/)
{
	L_CALL("HttpClient::delete_document_view()");

	endpoints_maker(request);
	auto query_field = query_field_maker(request, QUERY_FIELD_COMMIT);

	std::string doc_id(request.path_parser.get_id());

	request.processing = std::chrono::system_clock::now();

	enum http_status status_code;
	MsgPack response_obj;
	DatabaseHandler db_handler(endpoints, DB_WRITABLE | DB_SPAWN, method);

	db_handler.delete_document(doc_id, query_field.commit);
	request.ready = std::chrono::system_clock::now();
	status_code = HTTP_STATUS_OK;

	response_obj[RESPONSE_DELETE] = {
		{ ID_FIELD_NAME, doc_id },
		{ RESPONSE_COMMIT,  query_field.commit }
	};

	write_http_response(request, response, status_code, response_obj);

	auto took = std::chrono::duration_cast<std::chrono::nanoseconds>(request.ready - request.processing).count();
	L_TIME("Deletion took %s", string::from_delta(took));

	Metrics::metrics()
		.xapiand_operations_summary
		.Add({
			{"operation", "delete"},
		})
		.Observe(took / 1e9);
}


void
HttpClient::delete_schema_view(Request& request, Response& response, enum http_method method, Command /*unused*/)
{
	L_CALL("HttpClient::delete_schema_view()");

	endpoints_maker(request);

	request.processing = std::chrono::system_clock::now();

	DatabaseHandler db_handler(endpoints, DB_WRITABLE | DB_SPAWN, method);
	db_handler.delete_schema();

	request.ready = std::chrono::system_clock::now();

	write_http_response(request, response, HTTP_STATUS_NO_CONTENT);

	auto took = std::chrono::duration_cast<std::chrono::nanoseconds>(request.ready - request.processing).count();
	L_TIME("Schema deletion took %s", string::from_delta(took));

	Metrics::metrics()
		.xapiand_operations_summary
		.Add({
			{"operation", "delete_schema"},
		})
		.Observe(took / 1e9);
}


void
HttpClient::index_document_view(Request& request, Response& response, enum http_method method, Command /*unused*/)
{
	L_CALL("HttpClient::index_document_view()");

	enum http_status status_code = HTTP_STATUS_BAD_REQUEST;

	std::string doc_id;
	if (method != HTTP_POST) {
		doc_id = request.path_parser.get_id();
	}

	endpoints_maker(request);
	auto query_field = query_field_maker(request, QUERY_FIELD_COMMIT);

	request.processing = std::chrono::system_clock::now();

	MsgPack response_obj;
	DatabaseHandler db_handler(endpoints, DB_WRITABLE | DB_SPAWN, method);
	auto& decoded_body = request.decoded_body();
	response_obj = db_handler.index(doc_id, false, decoded_body, query_field.commit, request.ct_type).second;

	request.ready = std::chrono::system_clock::now();

	status_code = HTTP_STATUS_OK;
	response_obj[RESPONSE_COMMIT] = query_field.commit;

	write_http_response(request, response, status_code, response_obj);

	auto took = std::chrono::duration_cast<std::chrono::nanoseconds>(request.ready - request.processing).count();
	L_TIME("Indexing took %s", string::from_delta(took));

	Metrics::metrics()
		.xapiand_operations_summary
		.Add({
			{"operation", "index"},
		})
		.Observe(took / 1e9);
}


void
HttpClient::write_schema_view(Request& request, Response& response, enum http_method method, Command /*unused*/)
{
	L_CALL("HttpClient::write_schema_view()");

	enum http_status status_code = HTTP_STATUS_BAD_REQUEST;

	endpoints_maker(request);

	request.processing = std::chrono::system_clock::now();

	DatabaseHandler db_handler(endpoints, DB_WRITABLE | DB_SPAWN, method);
	db_handler.write_schema(request.decoded_body(), method == HTTP_PUT);

	request.ready = std::chrono::system_clock::now();

	MsgPack response_obj;
	status_code = HTTP_STATUS_OK;
	response_obj = db_handler.get_schema()->get_full(true);

	write_http_response(request, response, status_code, response_obj);

	auto took = std::chrono::duration_cast<std::chrono::nanoseconds>(request.ready - request.processing).count();
	L_TIME("Schema write took %s", string::from_delta(took));

	Metrics::metrics()
		.xapiand_operations_summary
		.Add({
			{"operation", "write_schema"},
		})
		.Observe(took / 1e9);
}


void
HttpClient::update_document_view(Request& request, Response& response, enum http_method method, Command /*unused*/)
{
	L_CALL("HttpClient::update_document_view()");

	endpoints_maker(request);
	auto query_field = query_field_maker(request, QUERY_FIELD_COMMIT);

	std::string doc_id(request.path_parser.get_id());
	enum http_status status_code = HTTP_STATUS_BAD_REQUEST;

	request.processing = std::chrono::system_clock::now();

	MsgPack response_obj;
	DatabaseHandler db_handler(endpoints, DB_WRITABLE | DB_SPAWN, method);
	auto& decoded_body = request.decoded_body();
	if (method == HTTP_PATCH) {
		response_obj = db_handler.patch(doc_id, decoded_body, query_field.commit, request.ct_type).second;
	} else if (method == HTTP_STORE) {
		response_obj = db_handler.merge(doc_id, true, decoded_body, query_field.commit, request.ct_type).second;
	} else {
		response_obj = db_handler.merge(doc_id, false, decoded_body, query_field.commit, request.ct_type).second;
	}

	request.ready = std::chrono::system_clock::now();

	status_code = HTTP_STATUS_OK;
	if (response_obj.find(ID_FIELD_NAME) == response_obj.end()) {
		response_obj[ID_FIELD_NAME] = doc_id;
	}
	response_obj[RESPONSE_COMMIT] = query_field.commit;

	write_http_response(request, response, status_code, response_obj);

	auto took = std::chrono::duration_cast<std::chrono::nanoseconds>(request.ready - request.processing).count();
	L_TIME("Updating took %s", string::from_delta(took));

	if (method == HTTP_PATCH) {
		Metrics::metrics()
			.xapiand_operations_summary
			.Add({
				{"operation", "patch"},
			})
			.Observe(took / 1e9);
	} else if (method == HTTP_STORE) {
		Metrics::metrics()
			.xapiand_operations_summary
			.Add({
				{"operation", "store"},
			})
			.Observe(took / 1e9);
	} else {
		Metrics::metrics()
			.xapiand_operations_summary
			.Add({
				{"operation", "merge"},
			})
			.Observe(took / 1e9);
	}
}


void
HttpClient::metadata_view(Request& request, Response& response, enum http_method method, Command /*unused*/)
{
	L_CALL("HttpClient::metadata_view()");

	enum http_status status_code = HTTP_STATUS_OK;

	endpoints_maker(request);

	request.processing = std::chrono::system_clock::now();

	MsgPack response_obj;

	DatabaseHandler db_handler;
	auto query_field = query_field_maker(request, QUERY_FIELD_VOLATILE);
	if (query_field.as_volatile) {
		if (endpoints.size() != 1) {
			THROW(ClientError, "Expecting exactly one index with volatile");
		}
		db_handler.reset(endpoints, DB_OPEN | DB_WRITABLE, method);
	} else {
		db_handler.reset(endpoints, DB_OPEN, method);
	}

	auto selector = request.path_parser.get_slc();
	auto key = request.path_parser.get_pmt();

	if (key.empty()) {
		response_obj = MsgPack(MsgPack::Type::MAP);
		for (auto& _key : db_handler.get_metadata_keys()) {
			auto metadata = db_handler.get_metadata(_key);
			if (!metadata.empty()) {
				response_obj[_key] = MsgPack::unserialise(metadata);
			}
		}
	} else {
		auto metadata = db_handler.get_metadata(key);
		if (metadata.empty()) {
			status_code = HTTP_STATUS_NOT_FOUND;
		} else {
			response_obj = MsgPack::unserialise(metadata);
		}
	}

	request.ready = std::chrono::system_clock::now();

	if (!selector.empty()) {
		response_obj = response_obj.select(selector);
	}

	write_http_response(request, response, status_code, response_obj);

	auto took = std::chrono::duration_cast<std::chrono::nanoseconds>(request.ready - request.processing).count();
	L_TIME("Get metadata took %s", string::from_delta(took));

	Metrics::metrics()
		.xapiand_operations_summary
		.Add({
			{"operation", "get_metadata"},
		})
		.Observe(took / 1e9);
}


void
HttpClient::write_metadata_view(Request& request, Response& response, enum http_method /*unused*/, Command /*unused*/)
{
	L_CALL("HttpClient::write_metadata_view()");

	write_http_response(request, response, HTTP_STATUS_NOT_IMPLEMENTED);
}


void
HttpClient::update_metadata_view(Request& request, Response& response, enum http_method /*unused*/, Command /*unused*/)
{
	L_CALL("HttpClient::update_metadata_view()");

	write_http_response(request, response, HTTP_STATUS_NOT_IMPLEMENTED);
}


void
HttpClient::delete_metadata_view(Request& request, Response& response, enum http_method /*unused*/, Command /*unused*/)
{
	L_CALL("HttpClient::delete_metadata_view()");

	write_http_response(request, response, HTTP_STATUS_NOT_IMPLEMENTED);
}


void
HttpClient::info_view(Request& request, Response& response, enum http_method method, Command /*unused*/)
{
	L_CALL("HttpClient::info_view()");

	MsgPack response_obj;
	auto selector = request.path_parser.get_slc();

	endpoints_maker(request);

	request.processing = std::chrono::system_clock::now();

	DatabaseHandler db_handler;
	auto query_field = query_field_maker(request, QUERY_FIELD_VOLATILE);
	if (query_field.as_volatile) {
		if (endpoints.size() != 1) {
			THROW(ClientError, "Expecting exactly one index with volatile");
		}
		db_handler.reset(endpoints, DB_OPEN | DB_WRITABLE, method);
	} else {
		db_handler.reset(endpoints, DB_OPEN, method);
	}

	response_obj[RESPONSE_DATABASE_INFO] = db_handler.get_database_info();

	// Info about a specific document was requested
	if (request.path_parser.off_pmt != nullptr) {
		auto id = request.path_parser.get_pmt();
		response_obj[RESPONSE_DOCUMENT_INFO] = db_handler.get_document_info(id, false);
	}

	request.ready = std::chrono::system_clock::now();

	if (!selector.empty()) {
		response_obj = response_obj.select(selector);
	}

	write_http_response(request, response, HTTP_STATUS_OK, response_obj);

	auto took = std::chrono::duration_cast<std::chrono::nanoseconds>(request.ready - request.processing).count();
	L_TIME("Info took %s", string::from_delta(took));

	Metrics::metrics()
		.xapiand_operations_summary
		.Add({
			{"operation", "info"}
		})
		.Observe(took / 1e9);
}


void
HttpClient::nodes_view(Request& request, Response& response, enum http_method method, Command /*unused*/)
{
	L_CALL("HttpClient::nodes_view()");

	request.path_parser.next();
	if (request.path_parser.next() != PathParser::State::END) {
		write_status_response(request, response, HTTP_STATUS_NOT_FOUND);
		return;
	}

	if ((request.path_parser.len_pth != 0u) || (request.path_parser.len_pmt != 0u) || (request.path_parser.len_ppmt != 0u)) {
		write_status_response(request, response, HTTP_STATUS_NOT_FOUND);
		return;
	}

	endpoints.clear();
	auto master_node_ = master_node.load();
	endpoints.add(Endpoint(".", master_node_.get()));

	DatabaseHandler db_handler(endpoints, DB_SPAWN, method);

	MsgPack nodes(MsgPack::Type::ARRAY);

	auto mset = db_handler.get_all_mset();
	const auto m_e = mset.end();
	for (auto m = mset.begin(); m != m_e; ++m) {
		auto document = db_handler.get_document(*m);
		auto obj = document.get_obj();
		obj.erase(ID_FIELD_NAME);
		auto node = XapiandManager::manager->get_node(obj["name"].as_str());
		if (node) {
			obj["host"] = node->host();
			obj["http_port"] = node->http_port;
			obj["binary_port"] = node->binary_port;
			obj["active"] = true;
		} else {
			obj["active"] = false;
		}
		obj["id"] = *m;
		nodes.push_back(obj);
	}

	write_http_response(request, response, HTTP_STATUS_OK, {
		{ RESPONSE_CLUSTER_NAME, opts.cluster_name },
		{ RESPONSE_NODES, nodes },
	});
}


void
HttpClient::touch_view(Request& request, Response& response, enum http_method method, Command /*unused*/)
{
	L_CALL("HttpClient::touch_view()");

	endpoints_maker(request);

	request.processing = std::chrono::system_clock::now();

	DatabaseHandler db_handler(endpoints, DB_WRITABLE | DB_SPAWN, method);

	db_handler.reopen();  // Ensure touch.

	request.ready = std::chrono::system_clock::now();

	MsgPack response_obj;
	response_obj[RESPONSE_ENDPOINT] = endpoints.to_string();

	write_http_response(request, response, HTTP_STATUS_CREATED, response_obj);

	auto took = std::chrono::duration_cast<std::chrono::nanoseconds>(request.ready - request.processing).count();
	L_TIME("Touch took %s", string::from_delta(took));

	Metrics::metrics()
		.xapiand_operations_summary
		.Add({
			{"operation", "touch"},
		})
		.Observe(took / 1e9);
}


void
HttpClient::commit_view(Request& request, Response& response, enum http_method method, Command /*unused*/)
{
	L_CALL("HttpClient::commit_view()");

	endpoints_maker(request);

	request.processing = std::chrono::system_clock::now();

	DatabaseHandler db_handler(endpoints, DB_WRITABLE | DB_SPAWN, method);

	db_handler.commit();  // Ensure touch.

	request.ready = std::chrono::system_clock::now();

	MsgPack response_obj;
	response_obj[RESPONSE_ENDPOINT] = endpoints.to_string();

	write_http_response(request, response, HTTP_STATUS_OK, response_obj);

	auto took = std::chrono::duration_cast<std::chrono::nanoseconds>(request.ready - request.processing).count();
	L_TIME("Commit took %s", string::from_delta(took));

	Metrics::metrics()
		.xapiand_operations_summary
		.Add({
			{"operation", "commit"},
		})
		.Observe(took / 1e9);
}


void
HttpClient::dump_view(Request& request, Response& response, enum http_method /*unused*/, Command /*unused*/)
{
	L_CALL("HttpClient::dump_view()");

	endpoints_maker(request);

	request.processing = std::chrono::system_clock::now();

	DatabaseHandler db_handler(endpoints, DB_OPEN | DB_NOWAL);

	auto ct_type = resolve_ct_type(request, MSGPACK_CONTENT_TYPE);

	if (ct_type.empty()) {
		auto dump_ct_type = resolve_ct_type(request, ct_type_t("application/octet-stream"));
		if (dump_ct_type.empty()) {
			// No content type could be resolved, return NOT ACCEPTABLE.
			enum http_status error_code = HTTP_STATUS_NOT_ACCEPTABLE;
			MsgPack err_response = {
				{ RESPONSE_STATUS, (int)error_code },
				{ RESPONSE_MESSAGE, { "Response type application/octet-stream not provided in the Accept header" } }
			};
			write_http_response(request, response, error_code, err_response);
			L_SEARCH("ABORTED SEARCH");
			return;
		}

		char path[] = "/tmp/xapian_dump.XXXXXX";
		int file_descriptor = mkstemp(path);
		try {
			db_handler.dump_documents(file_descriptor);
		} catch (...) {
			io::close(file_descriptor);
			io::unlink(path);
			throw;
		}

		request.ready = std::chrono::system_clock::now();

		size_t content_length = io::lseek(file_descriptor, 0, SEEK_CUR);
		io::close(file_descriptor);
		write(http_response(request, response, HTTP_STATUS_OK, HTTP_STATUS_RESPONSE | HTTP_HEADER_RESPONSE | HTTP_CONTENT_TYPE_RESPONSE | HTTP_CONTENT_LENGTH_RESPONSE, 0, 0, "", dump_ct_type.to_string(), "", content_length));
		write_file(path, true);
		return;
	}

	auto docs = db_handler.dump_documents();

	request.ready = std::chrono::system_clock::now();

	write_http_response(request, response, HTTP_STATUS_OK, docs);

	auto took = std::chrono::duration_cast<std::chrono::nanoseconds>(request.ready - request.processing).count();
	L_TIME("Dump took %s", string::from_delta(took));

	Metrics::metrics()
		.xapiand_operations_summary
		.Add({
			{"operation", "dump"},
		})
		.Observe(took / 1e9);
}


void
HttpClient::restore_view(Request& request, Response& response, enum http_method method, Command /*unused*/)
{
	L_CALL("HttpClient::restore_view()");

	endpoints_maker(request);

	request.processing = std::chrono::system_clock::now();

	DatabaseHandler db_handler(endpoints, DB_WRITABLE | DB_SPAWN | DB_NOWAL, method);

	auto& decoded_body = request.decoded_body();
	if (decoded_body.is_string()) {
		char path[] = "/tmp/xapian_dump.XXXXXX";
		int file_descriptor = mkstemp(path);
		try {
			auto body = decoded_body.str_view();
			io::write(file_descriptor, body.data(), body.size());
			io::lseek(file_descriptor, 0, SEEK_SET);
			db_handler.restore(file_descriptor);
		} catch (...) {
			io::close(file_descriptor);
			io::unlink(path);
			throw;
		}

		io::close(file_descriptor);
		io::unlink(path);
	} else if (decoded_body.is_array()) {
		db_handler.restore_documents(decoded_body);
	} else {
		THROW(ClientError, "Expected a binary or list dump");
	}

	request.ready = std::chrono::system_clock::now();
	auto took = std::chrono::duration_cast<std::chrono::nanoseconds>(request.ready - request.processing).count();
	auto took_milliseconds = took / 1e6;

	MsgPack response_obj = {
		{ RESPONSE_ENDPOINT, endpoints.to_string() },
		{ RESPONSE_TOOK, took_milliseconds },
	};

	write_http_response(request, response, HTTP_STATUS_OK, response_obj);

	L_TIME("Restore took %s", string::from_delta(took));

	Metrics::metrics()
		.xapiand_operations_summary
		.Add({
			{"operation", "restore"},
		})
		.Observe(took / 1e9);
}


void
HttpClient::schema_view(Request& request, Response& response, enum http_method method, Command /*unused*/)
{
	L_CALL("HttpClient::schema_view()");

	auto selector = request.path_parser.get_slc();

	endpoints_maker(request);

	request.processing = std::chrono::system_clock::now();

	DatabaseHandler db_handler;
	auto query_field = query_field_maker(request, QUERY_FIELD_VOLATILE);
	if (query_field.as_volatile) {
		if (endpoints.size() != 1) {
			THROW(ClientError, "Expecting exactly one index with volatile");
		}
		db_handler.reset(endpoints, DB_OPEN | DB_WRITABLE, method);
	} else {
		db_handler.reset(endpoints, DB_OPEN, method);
	}

	auto schema = db_handler.get_schema()->get_full(true);
	if (!selector.empty()) {
		schema = schema.select(selector);
	}

	request.ready = std::chrono::system_clock::now();

	write_http_response(request, response, HTTP_STATUS_OK, schema);

	auto took = std::chrono::duration_cast<std::chrono::nanoseconds>(request.ready - request.processing).count();
	L_TIME("Schema took %s", string::from_delta(took));

	Metrics::metrics()
		.xapiand_operations_summary
		.Add({
			{"operation", "schema"},
		})
		.Observe(took / 1e9);
}


#if XAPIAND_DATABASE_WAL
void
HttpClient::wal_view(Request& request, Response& response, enum http_method /*unused*/, Command /*unused*/)
{
	L_CALL("HttpClient::wal_view()");

	endpoints_maker(request);

	request.processing = std::chrono::system_clock::now();

	DatabaseHandler db_handler{endpoints};

	bool unserialised = request.query_parser.next("raw") == -1;
	auto repr = db_handler.repr_wal(0, std::numeric_limits<uint32_t>::max(), unserialised);

	request.ready = std::chrono::system_clock::now();

	write_http_response(request, response, HTTP_STATUS_OK, repr);

	auto took = std::chrono::duration_cast<std::chrono::nanoseconds>(request.ready - request.processing).count();
	L_TIME("WAL took %s", string::from_delta(took));

	Metrics::metrics()
		.xapiand_operations_summary
		.Add({
			{"operation", "wal"},
		})
		.Observe(took / 1e9);
}
#endif


void
HttpClient::check_view(Request& request, Response& response, enum http_method /*unused*/, Command /*unused*/)
{
	L_CALL("HttpClient::wal_view()");

	endpoints_maker(request);

	request.processing = std::chrono::system_clock::now();

	DatabaseHandler db_handler{endpoints};

	auto status = db_handler.check();

	request.ready = std::chrono::system_clock::now();

	write_http_response(request, response, HTTP_STATUS_OK, status);

	auto took = std::chrono::duration_cast<std::chrono::nanoseconds>(request.ready - request.processing).count();
	L_TIME("Database check took %s", string::from_delta(took));

	Metrics::metrics()
		.xapiand_operations_summary
		.Add({
			{"operation", "db_check"},
		})
		.Observe(took / 1e9);
}


void
HttpClient::search_view(Request& request, Response& response, enum http_method method, Command /*unused*/)
{
	L_CALL("HttpClient::search_view()");

	auto selector = request.path_parser.get_slc();
	auto id = request.path_parser.get_id();

	endpoints_maker(request);

	bool single = !id.empty() && !isRange(id);

	MSet mset{};
	MsgPack aggregations;
	std::vector<std::string> suggestions;

	request.processing = std::chrono::system_clock::now();

	DatabaseHandler db_handler;
	auto query_field = query_field_maker(request, QUERY_FIELD_VOLATILE | (id.empty() ? QUERY_FIELD_SEARCH : QUERY_FIELD_ID));
	try {
		if (query_field.as_volatile) {
			if (endpoints.size() != 1) {
				THROW(ClientError, "Expecting exactly one index with volatile");
			}
			db_handler.reset(endpoints, DB_OPEN | DB_WRITABLE, method);
		} else {
			db_handler.reset(endpoints, DB_OPEN, method);
		}

		if (single) {
			try {
				mset = db_handler.get_docid(id);
			} catch (const NotFoundError&) { }
		} else {
			if (request.raw.empty()) {
				mset = db_handler.get_mset(query_field, nullptr, nullptr, suggestions);
			} else {
				auto& decoded_body = request.decoded_body();
				AggregationMatchSpy aggs(decoded_body, db_handler.get_schema());
				mset = db_handler.get_mset(query_field, &decoded_body, &aggs, suggestions);
				aggregations = aggs.get_aggregation().at(AGGREGATION_AGGS);
			}
		}
	} catch (const NotFoundError&) {
		/* At the moment when the endpoint does not exist and it is chunck it will return 200 response
		 * with zero matches this behavior may change in the future for instance ( return 404 ) */
		if (single) {
			throw;
		}
	}

	L_SEARCH("Suggested queries: %s", [&suggestions]() {
		MsgPack res(MsgPack::Type::ARRAY);
		for (const auto& suggestion : suggestions) {
			res.push_back(suggestion);
		}
		return res;
	}().to_string());

	int rc = 0;
	auto total_count = mset.size();

	if (single && total_count == 0u) {
		enum http_status error_code = HTTP_STATUS_NOT_FOUND;
		MsgPack err_response = {
			{ RESPONSE_STATUS, (int)error_code },
			{ RESPONSE_MESSAGE, http_status_str(error_code) }
		};
		write_http_response(request, response, error_code, err_response);
		return;
	}

	auto type_encoding = resolve_encoding(request);
	if (type_encoding == Encoding::unknown) {
		enum http_status error_code = HTTP_STATUS_NOT_ACCEPTABLE;
		MsgPack err_response = {
			{ RESPONSE_STATUS, (int)error_code },
			{ RESPONSE_MESSAGE, { "Response encoding gzip, deflate or identity not provided in the Accept-Encoding header" } }
		};
		write_http_response(request, response, error_code, err_response);
		L_SEARCH("ABORTED SEARCH");
		return;
	}

	bool indent_chunk = false;
	std::string first_chunk;
	std::string last_chunk;
	std::string sep_chunk;
	std::string eol_chunk;

	std::string l_first_chunk;
	std::string l_last_chunk;
	std::string l_eol_chunk;
	std::string l_sep_chunk;

	// Get default content type to return.
	auto ct_type = resolve_ct_type(request, MSGPACK_CONTENT_TYPE);

	if (!single) {
		MsgPack basic_query({
			{ RESPONSE_TOTAL_COUNT, total_count},
			{ RESPONSE_MATCHES_ESTIMATED, mset.get_matches_estimated()},
			{ RESPONSE_HITS, MsgPack(MsgPack::Type::ARRAY) },
		});
		MsgPack basic_response;
		if (aggregations) {
			basic_response[RESPONSE_AGGREGATIONS] = aggregations;
		}
		basic_response[RESPONSE_QUERY] = basic_query;
		basic_response[""] = nullptr;

		if ((is_acceptable_type(msgpack_type, ct_type) != nullptr) || (is_acceptable_type(x_msgpack_type, ct_type) != nullptr)) {
			first_chunk = basic_response.serialise();
			// Remove zero size array and manually add the msgpack array header
			first_chunk.erase(first_chunk.size() - 3);
			if (total_count < 16) {
				first_chunk.push_back(static_cast<char>(0x90u | total_count));
			} else if (total_count < 65536) {
				char buf[3];
				buf[0] = static_cast<char>(0xdcu); _msgpack_store16(&buf[1], static_cast<uint16_t>(total_count));
				first_chunk.append(buf, 3);
			} else {
				char buf[5];
				buf[0] = static_cast<char>(0xddu); _msgpack_store32(&buf[1], static_cast<uint32_t>(total_count));
				first_chunk.append(buf, 5);
			}
			basic_response.erase("");
		} else if (is_acceptable_type(json_type, ct_type) != nullptr) {
			basic_response.erase("");
			first_chunk = basic_response.to_string(request.indented);
			if (request.indented != -1) {
				first_chunk.erase(first_chunk.size() - ((request.indented * 2) + 1));
				first_chunk += "\n";
				last_chunk = std::string(request.indented * 2, ' ') + "]\n" + std::string(request.indented, ' ') + "},\n" + std::string(request.indented, ' ') + "\"" + std::string(RESPONSE_TOOK) + "\": %s\n}";
				eol_chunk = "\n";
				sep_chunk = ",";
				indent_chunk = true;
			} else {
				first_chunk.erase(first_chunk.size() - 3);
				last_chunk = "]},\"" + std::string(RESPONSE_TOOK) + "\":%s}";
				sep_chunk = ",";
			}
		} else {
			enum http_status error_code = HTTP_STATUS_NOT_ACCEPTABLE;
			MsgPack err_response = {
				{ RESPONSE_STATUS, (int)error_code },
				{ RESPONSE_MESSAGE, { "Response type application/msgpack or application/json not provided in the Accept header" } }
			};
			write_http_response(request, response, error_code, err_response);
			L_SEARCH("ABORTED SEARCH");
			return;
		}

		if (Logging::log_level > LOG_DEBUG && response.size <= 1024 * 10) {
			l_first_chunk = basic_response.to_string(4);
			l_first_chunk.erase(l_first_chunk.size() - 9);
			l_first_chunk += "\n";
			l_last_chunk = "        ]\n    },\n    \"" + std::string(RESPONSE_TOOK) + "\": %s\n}";
			l_eol_chunk = "\n";
			l_sep_chunk = ",";
		}
	}

	std::string buffer;
	std::string l_buffer;
	const auto m_e = mset.end();
	for (auto m = mset.begin(); m != m_e; ++rc, ++m) {
		auto document = db_handler.get_document(*m);

		const auto data = Data(document.get_data());
		if (data.empty()) {
			continue;
		}

		MsgPack obj;
		if (single) {
			auto accepted = data.get_accepted(request.accept_set);
			if (accepted.first != nullptr) {
				auto& locator = *accepted.first;
				if (locator.ct_type.empty()) {
					obj = MsgPack::unserialise(locator.data());
				} else {
					ct_type = locator.ct_type;
					response.ct_type = ct_type;
					response.blob = document.get_blob(response.ct_type);
					if (type_encoding != Encoding::none) {
						auto encoded = encoding_http_response(response, type_encoding, response.blob, false, true, true);
						if (!encoded.empty() && encoded.size() <= response.blob.size()) {
							write(http_response(request, response, HTTP_STATUS_OK, HTTP_STATUS_RESPONSE | HTTP_HEADER_RESPONSE | HTTP_CONTENT_TYPE_RESPONSE | HTTP_CONTENT_ENCODING_RESPONSE | HTTP_BODY_RESPONSE, 0, 0, encoded, ct_type.to_string(), readable_encoding(type_encoding)));
						} else {
							write(http_response(request, response, HTTP_STATUS_OK, HTTP_STATUS_RESPONSE | HTTP_HEADER_RESPONSE | HTTP_CONTENT_TYPE_RESPONSE | HTTP_CONTENT_ENCODING_RESPONSE | HTTP_BODY_RESPONSE, 0, 0, response.blob, ct_type.to_string(), readable_encoding(Encoding::identity)));
						}
					} else {
						write(http_response(request, response, HTTP_STATUS_OK, HTTP_STATUS_RESPONSE | HTTP_HEADER_RESPONSE | HTTP_CONTENT_TYPE_RESPONSE | HTTP_BODY_RESPONSE, 0, 0, response.blob, ct_type.to_string()));
					}
					return;
				}
			} else {
				// No content type could be resolved, return NOT ACCEPTABLE.
				enum http_status error_code = HTTP_STATUS_NOT_ACCEPTABLE;
				MsgPack err_response = {
					{ RESPONSE_STATUS, (int)error_code },
					{ RESPONSE_MESSAGE, { "Response type not accepted by the Accept header" } }
				};
				write_http_response(request, response, error_code, err_response);
				L_SEARCH("ABORTED SEARCH");
				return;
			}
		} else {
			auto main_locator = data.get("");
			if (main_locator != nullptr) {
				obj = MsgPack::unserialise(main_locator->data());
			}
		}

		if (obj.find(ID_FIELD_NAME) == obj.end()) {
			obj[ID_FIELD_NAME] = document.get_value(ID_FIELD_NAME);
		}

		// Detailed info about the document:
		obj[RESPONSE_DOCID] = document.get_docid();
		if (!single) {
			obj[RESPONSE_RANK] = m.get_rank();
			obj[RESPONSE_WEIGHT] = m.get_weight();
			obj[RESPONSE_PERCENT] = m.get_percent();
			// int subdatabase = (document.get_docid() - 1) % endpoints.size();
			// auto endpoint = endpoints[subdatabase];
			// obj[RESPONSE_ENDPOINT] = endpoint.to_string();
		}

		if (!selector.empty()) {
			obj = obj.select(selector);
		}

		if (Logging::log_level > LOG_DEBUG && response.size <= 1024 * 10) {
			if (single) {
				response.body += obj.to_string(4);
			} else {
				if (rc == 0) {
					response.body += l_first_chunk;
				}
				if (!l_buffer.empty()) {
					response.body += string::indent(l_buffer, ' ', 3 * 4) + l_sep_chunk + l_eol_chunk;
				}
				l_buffer = obj.to_string(4);
			}
		}

		auto result = serialize_response(obj, ct_type, request.indented);
		if (single) {
			if (type_encoding != Encoding::none) {
				auto encoded = encoding_http_response(response, type_encoding, result.first, false, true, true);
				if (!encoded.empty() && encoded.size() <= result.first.size()) {
					write(http_response(request, response, HTTP_STATUS_OK, HTTP_STATUS_RESPONSE | HTTP_HEADER_RESPONSE | HTTP_BODY_RESPONSE | HTTP_CONTENT_TYPE_RESPONSE | HTTP_CONTENT_ENCODING_RESPONSE, 0, 0, encoded, result.second, readable_encoding(type_encoding)));
				} else {
					write(http_response(request, response, HTTP_STATUS_OK, HTTP_STATUS_RESPONSE | HTTP_HEADER_RESPONSE | HTTP_BODY_RESPONSE | HTTP_CONTENT_TYPE_RESPONSE | HTTP_CONTENT_ENCODING_RESPONSE, 0, 0, result.first, result.second, readable_encoding(Encoding::identity)));
				}
			} else {
				write(http_response(request, response, HTTP_STATUS_OK, HTTP_STATUS_RESPONSE | HTTP_HEADER_RESPONSE | HTTP_BODY_RESPONSE | HTTP_CONTENT_TYPE_RESPONSE, 0, 0, result.first, result.second));
			}
		} else {
			if (rc == 0) {
				if (type_encoding != Encoding::none) {
					auto encoded = encoding_http_response(response, type_encoding, first_chunk, true, true, false);
					write(http_response(request, response, HTTP_STATUS_OK, HTTP_STATUS_RESPONSE | HTTP_HEADER_RESPONSE | HTTP_CONTENT_TYPE_RESPONSE | HTTP_CONTENT_ENCODING_RESPONSE | HTTP_CHUNKED_RESPONSE | HTTP_TOTAL_COUNT_RESPONSE | HTTP_MATCHES_ESTIMATED_RESPONSE, mset.size(), mset.get_matches_estimated(), "", ct_type.to_string(), readable_encoding(type_encoding)));
					if (!encoded.empty()) {
						write(http_response(request, response, HTTP_STATUS_OK, HTTP_CHUNKED_RESPONSE | HTTP_BODY_RESPONSE, 0, 0, encoded));
					}
				} else {
					write(http_response(request, response, HTTP_STATUS_OK, HTTP_STATUS_RESPONSE | HTTP_HEADER_RESPONSE | HTTP_CONTENT_TYPE_RESPONSE | HTTP_CHUNKED_RESPONSE | HTTP_TOTAL_COUNT_RESPONSE | HTTP_MATCHES_ESTIMATED_RESPONSE, mset.size(), mset.get_matches_estimated(), "", ct_type.to_string()));
					if (!first_chunk.empty()) {
						write(http_response(request, response, HTTP_STATUS_OK, HTTP_CHUNKED_RESPONSE | HTTP_BODY_RESPONSE, 0, 0, first_chunk));
					}
				}
			}

			if (!buffer.empty()) {
				auto indented_buffer = (indent_chunk ? string::indent(buffer, ' ', 3 * request.indented) : buffer) + sep_chunk + eol_chunk;
				if (type_encoding != Encoding::none) {
					auto encoded = encoding_http_response(response, type_encoding, indented_buffer, true, false, false);
					if (!encoded.empty()) {
						write(http_response(request, response, HTTP_STATUS_OK, HTTP_CHUNKED_RESPONSE | HTTP_BODY_RESPONSE, 0, 0, encoded));
					}
				} else {
					if (!indented_buffer.empty()) {
						write(http_response(request, response, HTTP_STATUS_OK, HTTP_CHUNKED_RESPONSE | HTTP_BODY_RESPONSE, 0, 0, indented_buffer));
					}
				}
			}
			buffer = result.first;
		}

		if (single) { break; }
	}

	request.ready = std::chrono::system_clock::now();
	auto took = std::chrono::duration_cast<std::chrono::nanoseconds>(request.ready - request.processing).count();
	auto took_milliseconds = took / 1e6;
	auto took_delta = string::Number(took_milliseconds).str();
	L_TIME("Searching took %s", string::from_delta(took));

	if (Logging::log_level > LOG_DEBUG && response.size <= 1024 * 10) {
		if (!single) {
			if (rc == 0) {
				response.body += l_first_chunk;
			}

			if (!l_buffer.empty()) {
				response.body += string::indent(l_buffer, ' ', 3 * 4) + l_eol_chunk;
			}

			response.body += string::format(l_last_chunk, took_delta);
		}
	}

	if (!single) {
		if (rc == 0) {
			if (type_encoding != Encoding::none) {
				auto encoded = encoding_http_response(response, type_encoding, first_chunk, true, true, false);
				write(http_response(request, response, HTTP_STATUS_OK, HTTP_STATUS_RESPONSE | HTTP_HEADER_RESPONSE | HTTP_CONTENT_TYPE_RESPONSE | HTTP_CONTENT_ENCODING_RESPONSE | HTTP_CHUNKED_RESPONSE | HTTP_TOTAL_COUNT_RESPONSE | HTTP_MATCHES_ESTIMATED_RESPONSE, mset.size(), mset.get_matches_estimated(), "", ct_type.to_string(), readable_encoding(type_encoding)));
				if (!encoded.empty()) {
					write(http_response(request, response, HTTP_STATUS_OK, HTTP_CHUNKED_RESPONSE | HTTP_BODY_RESPONSE, 0, 0, encoded));
				}
			} else {
				write(http_response(request, response, HTTP_STATUS_OK, HTTP_STATUS_RESPONSE | HTTP_HEADER_RESPONSE | HTTP_CONTENT_TYPE_RESPONSE | HTTP_CHUNKED_RESPONSE | HTTP_TOTAL_COUNT_RESPONSE | HTTP_MATCHES_ESTIMATED_RESPONSE, mset.size(), mset.get_matches_estimated(), "", ct_type.to_string()));
				if (!first_chunk.empty()) {
					write(http_response(request, response, HTTP_STATUS_OK, HTTP_CHUNKED_RESPONSE | HTTP_BODY_RESPONSE, 0, 0, first_chunk));
				}
			}
		}

		if (!buffer.empty()) {
			auto indented_buffer = (indent_chunk ? string::indent(buffer, ' ', 3 * request.indented) : buffer) + eol_chunk;
			if (type_encoding != Encoding::none) {
				auto encoded = encoding_http_response(response, type_encoding, indented_buffer, true, false, false);
				if (!encoded.empty()) {
					write(http_response(request, response, HTTP_STATUS_OK, HTTP_CHUNKED_RESPONSE | HTTP_BODY_RESPONSE, 0, 0, encoded));
				}
			} else {
				if (!indented_buffer.empty()) {
					write(http_response(request, response, HTTP_STATUS_OK, HTTP_CHUNKED_RESPONSE | HTTP_BODY_RESPONSE, 0, 0, indented_buffer));
				}
			}
		}

		if (last_chunk.empty()) {
			last_chunk = MsgPack({
				{ RESPONSE_TOOK, took_milliseconds },
			}).serialise().substr(1);
		} else {
			last_chunk = string::format(last_chunk, took_delta);
		}

		if (type_encoding != Encoding::none) {
			auto encoded = encoding_http_response(response, type_encoding, last_chunk, true, false, true);
			if (!encoded.empty()) {
				write(http_response(request, response, HTTP_STATUS_OK, HTTP_CHUNKED_RESPONSE | HTTP_BODY_RESPONSE, 0, 0, encoded));
			}
		} else {
			write(http_response(request, response, HTTP_STATUS_OK, HTTP_CHUNKED_RESPONSE | HTTP_BODY_RESPONSE, 0, 0, last_chunk));
		}

		write(http_response(request, response, HTTP_STATUS_OK, HTTP_CHUNKED_RESPONSE | HTTP_BODY_RESPONSE));
	}

	if (aggregations) {
		Metrics::metrics()
			.xapiand_operations_summary
			.Add({
				{"operation", "aggregation"},
			})
			.Observe(took / 1e9);
	} else {
		Metrics::metrics()
			.xapiand_operations_summary
			.Add({
				{"operation", "search"},
			})
			.Observe(took / 1e9);
	}

	L_SEARCH("FINISH SEARCH");
}


void
HttpClient::write_status_response(Request& request, Response& response, enum http_status status, const std::string& message)
{
	L_CALL("HttpClient::write_status_response()");

	write_http_response(request, response, status, {
		{ RESPONSE_STATUS, (int)status },
		{ RESPONSE_MESSAGE, message.empty() ? MsgPack({ http_status_str(status) }) : string::split(message, '\n') }
	});
}


HttpClient::Command
HttpClient::getCommand(std::string_view command_name)
{
	static const auto _ = http_commands;

	return static_cast<Command>(_.fhhl(command_name));
}


HttpClient::Command
HttpClient::url_resolve(Request& request)
{
	L_CALL("HttpClient::url_resolve(request)");

	struct http_parser_url u;
	std::string b = repr(request.path, true, 0);

	L_HTTP("URL: %s", b);

	if (http_parser_parse_url(request.path.data(), request.path.size(), 0, &u) == 0) {
		L_HTTP_PROTO_PARSER("HTTP parsing done!");

		if ((u.field_set & (1 << UF_PATH )) != 0) {
			size_t path_size = u.field_data[3].len;
			std::unique_ptr<char[]> path_buf_ptr(new char[path_size + 1]);
			auto path_buf_str = path_buf_ptr.get();
			const char* path_str = request.path.data() + u.field_data[3].off;
			normalize_path(path_str, path_str + path_size, path_buf_str);
			if (*path_buf_str != '/' || *(path_buf_str + 1) != '\0') {
				if (request.path_parser.init(path_buf_str) >= PathParser::State::END) {
					return Command::BAD_QUERY;
				}
			}
		}

		if ((u.field_set & (1 <<  UF_QUERY)) != 0) {
			if (request.query_parser.init(std::string_view(b.data() + u.field_data[4].off, u.field_data[4].len)) < 0) {
				return Command::BAD_QUERY;
			}
		}

		if (request.query_parser.next("pretty") != -1) {
			if (request.query_parser.len != 0u) {
				try {
					request.indented = Serialise::boolean(request.query_parser.get()) == "t" ? 4 : -1;
				} catch (const Exception&) { }
			} else if (request.indented == -1) {
				request.indented = 4;
			}
		}
		request.query_parser.rewind();

		if (request.path_parser.off_cmd != nullptr) {
			return getCommand(request.path_parser.get_cmd());
		}

		if (request.path_parser.off_id != nullptr) {
			return Command::NO_CMD_ID;
		}

		return Command::NO_CMD_NO_ID;
	}

	L_HTTP_PROTO_PARSER("Parsing not done");
	// Bad query
	return Command::BAD_QUERY;
}


void
HttpClient::endpoints_maker(Request& request)
{
	endpoints.clear();

	PathParser::State state;
	while ((state = request.path_parser.next()) < PathParser::State::END) {
		_endpoint_maker(request);
	}
}


void
HttpClient::_endpoint_maker(Request& request)
{
	auto ns = request.path_parser.get_nsp();
	if (string::startswith(ns, "/")) { /* ns without slash */
		ns = ns.substr(1);
	}

	auto _path = request.path_parser.get_pth();
	if (string::startswith(_path, "/")) { /* path without slash */
		_path = _path.substr(1);
	}

	std::string index_path;
	if (ns.empty() && _path.empty()) {
		index_path = ".";
	} else {
		if (!ns.empty()) {
			index_path.append(ns);
			if (!string::endswith(index_path, "/")) {
				index_path.push_back('/');
			}
		}
		if (!_path.empty()) {
			index_path.append(_path);
		}
	}

	if (request.path_parser.off_hst != nullptr) {
		auto node_name = request.path_parser.get_hst();
#ifdef XAPIAND_CLUSTERING
		Endpoint index("xapian://" + std::string(node_name) + "/" + index_path);
		int node_port = (index.port == XAPIAND_BINARY_SERVERPORT) ? 0 : index.port;
		node_name = index.host.empty() ? node_name : index.host;

		// Convert node to endpoint:
		auto node = XapiandManager::manager->touch_node(node_name);
		if (!node) {
			THROW(Error, "Node %s not found", node_name);
		}
		if (!node_port) {
			node_port = node->binary_port;
		}
		Endpoint endpoint(string::format("xapian://%s:%d/%s", node->host(), node_port, index_path), nullptr, -1, node_name);
#else
		Endpoint endpoint(index_path);
#endif
		endpoints.add(endpoint);
	} else {
		endpoints.add(XapiandManager::manager->resolve_index_endpoint(index_path));
	}
	L_HTTP("Endpoint: -> %s", endpoints.to_string());
}


query_field_t
HttpClient::query_field_maker(Request& request, int flags)
{
	query_field_t query_field;

	if ((flags & QUERY_FIELD_COMMIT) != 0) {
		if (request.query_parser.next("commit") != -1) {
			query_field.commit = true;
			if (request.query_parser.len != 0u) {
				try {
					query_field.commit = Serialise::boolean(request.query_parser.get()) == "t";
				} catch (const Exception&) { }
			}
		}
		request.query_parser.rewind();
	}

	if ((flags & QUERY_FIELD_VOLATILE) != 0) {
		if (request.query_parser.next("volatile") != -1) {
			query_field.as_volatile = true;
			if (request.query_parser.len != 0u) {
				try {
					query_field.as_volatile = Serialise::boolean(request.query_parser.get()) == "t";
				} catch (const Exception&) { }
			}
		}
		request.query_parser.rewind();
	}

	if (((flags & QUERY_FIELD_ID) != 0) || ((flags & QUERY_FIELD_SEARCH) != 0)) {
		if (request.query_parser.next("offset") != -1) {
			query_field.offset = strict_stou(nullptr, request.query_parser.get());
		}
		request.query_parser.rewind();

		if (request.query_parser.next("check_at_least") != -1) {
			query_field.check_at_least = strict_stou(nullptr, request.query_parser.get());
		}
		request.query_parser.rewind();

		if (request.query_parser.next("limit") != -1) {
			query_field.limit = strict_stou(nullptr, request.query_parser.get());
		}
		request.query_parser.rewind();
	}

	if ((flags & QUERY_FIELD_SEARCH) != 0) {
		if (request.query_parser.next("spelling") != -1) {
			query_field.spelling = true;
			if (request.query_parser.len != 0u) {
				try {
					query_field.spelling = Serialise::boolean(request.query_parser.get()) == "t";
				} catch (const Exception&) { }
			}
		}
		request.query_parser.rewind();

		if (request.query_parser.next("synonyms") != -1) {
			query_field.synonyms = true;
			if (request.query_parser.len != 0u) {
				try {
					query_field.synonyms = Serialise::boolean(request.query_parser.get()) == "t";
				} catch (const Exception&) { }
			}
		}
		request.query_parser.rewind();

		while (request.query_parser.next("query") != -1) {
			L_SEARCH("query=%s", request.query_parser.get());
			query_field.query.emplace_back(request.query_parser.get());
		}
		request.query_parser.rewind();

		while (request.query_parser.next("q") != -1) {
			L_SEARCH("query=%s", request.query_parser.get());
			query_field.query.emplace_back(request.query_parser.get());
		}
		request.query_parser.rewind();

		while (request.query_parser.next("sort") != -1) {
			query_field.sort.emplace_back(request.query_parser.get());
		}
		request.query_parser.rewind();

		if (request.query_parser.next("metric") != -1) {
			query_field.metric = request.query_parser.get();
		}
		request.query_parser.rewind();

		if (request.query_parser.next("icase") != -1) {
			query_field.icase = Serialise::boolean(request.query_parser.get()) == "t";
		}
		request.query_parser.rewind();

		if (request.query_parser.next("collapse_max") != -1) {
			query_field.collapse_max = strict_stou(nullptr, request.query_parser.get());
		}
		request.query_parser.rewind();

		if (request.query_parser.next("collapse") != -1) {
			query_field.collapse = request.query_parser.get();
		}
		request.query_parser.rewind();

		if (request.query_parser.next("fuzzy") != -1) {
			query_field.is_fuzzy = true;
			if (request.query_parser.len != 0u) {
				try {
					query_field.is_fuzzy = Serialise::boolean(request.query_parser.get()) == "t";
				} catch (const Exception&) { }
			}
		}
		request.query_parser.rewind();

		if (query_field.is_fuzzy) {
			if (request.query_parser.next("fuzzy.n_rset") != -1) {
				query_field.fuzzy.n_rset = strict_stou(nullptr, request.query_parser.get());
			}
			request.query_parser.rewind();

			if (request.query_parser.next("fuzzy.n_eset") != -1) {
				query_field.fuzzy.n_eset = strict_stou(nullptr, request.query_parser.get());
			}
			request.query_parser.rewind();

			if (request.query_parser.next("fuzzy.n_term") != -1) {
				query_field.fuzzy.n_term = strict_stou(nullptr, request.query_parser.get());
			}
			request.query_parser.rewind();

			while (request.query_parser.next("fuzzy.field") != -1) {
				query_field.fuzzy.field.emplace_back(request.query_parser.get());
			}
			request.query_parser.rewind();

			while (request.query_parser.next("fuzzy.type") != -1) {
				query_field.fuzzy.type.emplace_back(request.query_parser.get());
			}
			request.query_parser.rewind();
		}

		if (request.query_parser.next("nearest") != -1) {
			query_field.is_nearest = true;
			if (request.query_parser.len != 0u) {
				try {
					query_field.is_nearest = Serialise::boolean(request.query_parser.get()) == "t";
				} catch (const Exception&) { }
			}
		}
		request.query_parser.rewind();

		if (query_field.is_nearest) {
			query_field.nearest.n_rset = 5;
			if (request.query_parser.next("nearest.n_rset") != -1) {
				query_field.nearest.n_rset = strict_stoul(nullptr, request.query_parser.get());
			}
			request.query_parser.rewind();

			if (request.query_parser.next("nearest.n_eset") != -1) {
				query_field.nearest.n_eset = strict_stoul(nullptr, request.query_parser.get());
			}
			request.query_parser.rewind();

			if (request.query_parser.next("nearest.n_term") != -1) {
				query_field.nearest.n_term = strict_stoul(nullptr, request.query_parser.get());
			}
			request.query_parser.rewind();

			while (request.query_parser.next("nearest.field") != -1) {
				query_field.nearest.field.emplace_back(request.query_parser.get());
			}
			request.query_parser.rewind();

			while (request.query_parser.next("nearest.type") != -1) {
				query_field.nearest.type.emplace_back(request.query_parser.get());
			}
			request.query_parser.rewind();
		}
	}

	if ((flags & QUERY_FIELD_TIME) != 0) {
		if (request.query_parser.next("time") != -1) {
			query_field.time = request.query_parser.get();
		} else {
			query_field.time = "1h";
		}
		request.query_parser.rewind();
	}

	if ((flags & QUERY_FIELD_PERIOD) != 0) {
		if (request.query_parser.next("period") != -1) {
			query_field.period = request.query_parser.get();
		} else {
			query_field.period = "1m";
		}
		request.query_parser.rewind();
	}

	return query_field;
}


void
HttpClient::log_request(Request& request)
{
	std::string request_prefix = " 🌎  ";

	static constexpr auto no_col = NO_COLOR;
	auto request_headers_color = no_col.c_str();
	auto request_head_color = no_col.c_str();
	auto request_body_color = no_col.c_str();
	int priority = -LOG_DEBUG;

	switch (HTTP_PARSER_METHOD(&request.parser)) {
		case HTTP_OPTIONS: {
			// rgb(13, 90, 167)
			static constexpr auto _request_headers_color = rgba(30, 77, 124, 0.6);
			request_headers_color = _request_headers_color.c_str();
			static constexpr auto _request_head_color = brgb(30, 77, 124);
			request_head_color = _request_head_color.c_str();
			static constexpr auto _request_body_color = rgb(30, 77, 124);
			request_body_color = _request_body_color.c_str();
			break;
		}
		case HTTP_HEAD: {
			// rgb(144, 18, 254)
			static constexpr auto _request_headers_color = rgba(100, 64, 131, 0.6);
			request_headers_color = _request_headers_color.c_str();
			static constexpr auto _request_head_color = brgb(100, 64, 131);
			request_head_color = _request_head_color.c_str();
			static constexpr auto _request_body_color = rgb(100, 64, 131);
			request_body_color = _request_body_color.c_str();
			break;
		}
		case HTTP_GET: {
			// rgb(101, 177, 251)
			static constexpr auto _request_headers_color = rgba(34, 113, 191, 0.6);
			request_headers_color = _request_headers_color.c_str();
			static constexpr auto _request_head_color = brgb(34, 113, 191);
			request_head_color = _request_head_color.c_str();
			static constexpr auto _request_body_color = rgb(34, 113, 191);
			request_body_color = _request_body_color.c_str();
			break;
		}
		case HTTP_POST: {
			// rgb(80, 203, 146)
			static constexpr auto _request_headers_color = rgba(55, 100, 79, 0.6);
			request_headers_color = _request_headers_color.c_str();
			static constexpr auto _request_head_color = brgb(55, 100, 79);
			request_head_color = _request_head_color.c_str();
			static constexpr auto _request_body_color = rgb(55, 100, 79);
			request_body_color = _request_body_color.c_str();
			break;
		}
		case HTTP_PATCH: {
			// rgb(88, 226, 194)
			static constexpr auto _request_headers_color = rgba(51, 136, 116, 0.6);
			request_headers_color = _request_headers_color.c_str();
			static constexpr auto _request_head_color = brgb(51, 136, 116);
			request_head_color = _request_head_color.c_str();
			static constexpr auto _request_body_color = rgb(51, 136, 116);
			request_body_color = _request_body_color.c_str();
			break;
		}
		case HTTP_MERGE: {
			// rgb(88, 226, 194)
			static constexpr auto _request_headers_color = rgba(51, 136, 116, 0.6);
			request_headers_color = _request_headers_color.c_str();
			static constexpr auto _request_head_color = brgb(51, 136, 116);
			request_head_color = _request_head_color.c_str();
			static constexpr auto _request_body_color = rgb(51, 136, 116);
			request_body_color = _request_body_color.c_str();
			break;
		}
		case HTTP_STORE: {
			// rgb(88, 226, 194)
			static constexpr auto _request_headers_color = rgba(51, 136, 116, 0.6);
			request_headers_color = _request_headers_color.c_str();
			static constexpr auto _request_head_color = brgb(51, 136, 116);
			request_head_color = _request_head_color.c_str();
			static constexpr auto _request_body_color = rgb(51, 136, 116);
			request_body_color = _request_body_color.c_str();
			break;
		}
		case HTTP_PUT: {
			// rgb(250, 160, 63)
			static constexpr auto _request_headers_color = rgba(158, 95, 28, 0.6);
			request_headers_color = _request_headers_color.c_str();
			static constexpr auto _request_head_color = brgb(158, 95, 28);
			request_head_color = _request_head_color.c_str();
			static constexpr auto _request_body_color = rgb(158, 95, 28);
			request_body_color = _request_body_color.c_str();
			break;
		}
		case HTTP_DELETE: {
			// rgb(246, 64, 68)
			static constexpr auto _request_headers_color = rgba(151, 31, 34, 0.6);
			request_headers_color = _request_headers_color.c_str();
			static constexpr auto _request_head_color = brgb(151, 31, 34);
			request_head_color = _request_head_color.c_str();
			static constexpr auto _request_body_color = rgb(151, 31, 34);
			request_body_color = _request_body_color.c_str();
			break;
		}
		default:
			break;
	};

	auto request_text = request_head_color + request.head + "\n" + request_headers_color + request.headers + request_body_color;
	if (!request.raw.empty()) {
		if (Logging::log_level > LOG_DEBUG + 1 && can_preview(request.ct_type)) {
			// From [https://www.iterm2.com/documentation-images.html]
			std::string b64_name = cppcodec::base64_rfc4648::encode("");
			std::string b64_data = cppcodec::base64_rfc4648::encode(request.raw);
			request_text += string::format("\033]1337;File=name=%s;inline=1;size=%d;width=20%%:",
				b64_name,
				b64_data.size());
			request_text += b64_data;
			request_text += '\a';
		} else {
			if (request.raw.size() > 1024 * 10) {
				request_text += "<body " + string::from_bytes(request.raw.size()) + ">";
			} else {
				auto& decoded_body = request.decoded_body();
				if (request.ct_type == json_type || request.ct_type == msgpack_type) {
					request_text += decoded_body.to_string(4);
				} else {
					request_text += "<body " + string::from_bytes(request.raw.size()) + ">";
				}
			}
		}
	} else if (!request.body.empty()) {
		if (request.body.size() > 1024 * 10) {
			request_text += "<body " + string::from_bytes(request.body.size()) + ">";
		} else {
			request_text += request.body;
		}
	}
	L(priority, NO_COLOR, "%s%s", request_prefix, string::indent(request_text, ' ', 4, false));
}


void
HttpClient::log_response(Response& response)
{
	std::string response_prefix = " 💊  ";

	static constexpr auto no_col = NO_COLOR;
	auto response_headers_color = no_col.c_str();
	auto response_head_color = no_col.c_str();
	auto response_body_color = no_col.c_str();
	int priority = -LOG_DEBUG;

	if ((int)response.status >= 200 && (int)response.status <= 299) {
		static constexpr auto _response_headers_color = rgba(68, 136, 68, 0.6);
		response_headers_color = _response_headers_color.c_str();
		static constexpr auto _response_head_color = brgb(68, 136, 68);
		response_head_color = _response_head_color.c_str();
		static constexpr auto _response_body_color = rgb(68, 136, 68);
		response_body_color = _response_body_color.c_str();
	} else if ((int)response.status >= 300 && (int)response.status <= 399) {
		response_prefix = " 💫  ";
		static constexpr auto _response_headers_color = rgba(68, 136, 120, 0.6);
		response_headers_color = _response_headers_color.c_str();
		static constexpr auto _response_head_color = brgb(68, 136, 120);
		response_head_color = _response_head_color.c_str();
		static constexpr auto _response_body_color = rgb(68, 136, 120);
		response_body_color = _response_body_color.c_str();
	} else if ((int)response.status == 404) {
		response_prefix = " 🕸  ";
		static constexpr auto _response_headers_color = rgba(116, 100, 77, 0.6);
		response_headers_color = _response_headers_color.c_str();
		static constexpr auto _response_head_color = brgb(116, 100, 77);
		response_head_color = _response_head_color.c_str();
		static constexpr auto _response_body_color = rgb(116, 100, 77);
		response_body_color = _response_body_color.c_str();
		priority = -LOG_INFO;
	} else if ((int)response.status >= 400 && (int)response.status <= 499) {
		response_prefix = " 💥  ";
		static constexpr auto _response_headers_color = rgba(183, 70, 17, 0.6);
		response_headers_color = _response_headers_color.c_str();
		static constexpr auto _response_head_color = brgb(183, 70, 17);
		response_head_color = _response_head_color.c_str();
		static constexpr auto _response_body_color = rgb(183, 70, 17);
		response_body_color = _response_body_color.c_str();
	} else if ((int)response.status >= 500 && (int)response.status <= 599) {
		response_prefix = " 🔥  ";
		static constexpr auto _response_headers_color = rgba(190, 30, 10, 0.6);
		response_headers_color = _response_headers_color.c_str();
		static constexpr auto _response_head_color = brgb(190, 30, 10);
		response_head_color = _response_head_color.c_str();
		static constexpr auto _response_body_color = rgb(190, 30, 10);
		response_body_color = _response_body_color.c_str();
		priority = -LOG_ERR;
	}

	auto response_text = response_head_color + response.head + "\n" + response_headers_color + response.headers + response_body_color;
	if (!response.blob.empty()) {
		if (Logging::log_level > LOG_DEBUG + 1 && can_preview(response.ct_type)) {
			// From [https://www.iterm2.com/documentation-images.html]
			std::string b64_name = cppcodec::base64_rfc4648::encode("");
			std::string b64_data = cppcodec::base64_rfc4648::encode(response.blob);
			response_text += string::format("\033]1337;File=name=%s;inline=1;size=%d;width=20%%:",
				b64_name,
				b64_data.size());
			response_text += b64_data;
			response_text += '\a';
		} else {
			if (response.blob.size() > 1024 * 10) {
				response_text += "<blob " + string::from_bytes(response.blob.size()) + ">";
			} else {
				response_text += "<blob " + string::from_bytes(response.blob.size()) + ">";
			}
		}
	} else if (!response.body.empty()) {
		if (response.size > 1024 * 10) {
			response_text += "<body " + string::from_bytes(response.size) + ">";
		} else {
			response_text += response.body;
		}
	}
	L(priority, NO_COLOR, "%s%s", response_prefix, string::indent(response_text, ' ', 4, false));
}


void
HttpClient::clean_http_request(Request& request, Response& response)
{
	L_CALL("HttpClient::clean_http_request()");

	request.ends = std::chrono::system_clock::now();

	request.log->clear();
	if (request.parser.http_errno != 0u) {
		L(LOG_ERR, LIGHT_RED, "HTTP parsing error (%s): %s", http_errno_name(HTTP_PARSER_ERRNO(&request.parser)), http_errno_description(HTTP_PARSER_ERRNO(&request.parser)));
	} else {
		static constexpr auto fmt_defaut = RED + "\"%s\" %d %s %s";
		auto fmt = fmt_defaut.c_str();
		int priority = LOG_DEBUG;

		if ((int)response.status >= 200 && (int)response.status <= 299) {
			static constexpr auto fmt_2xx = WHITE + "\"%s\" %d %s %s";
			fmt = fmt_2xx.c_str();
		} else if ((int)response.status >= 300 && (int)response.status <= 399) {
			static constexpr auto fmt_3xx = STEEL_BLUE + "\"%s\" %d %s %s";
			fmt = fmt_3xx.c_str();
		} else if ((int)response.status >= 400 && (int)response.status <= 499) {
			static constexpr auto fmt_4xx = SADDLE_BROWN + "\"%s\" %d %s %s";
			fmt = fmt_4xx.c_str();
			priority = LOG_INFO;
		} else if ((int)response.status >= 500 && (int)response.status <= 599) {
			static constexpr auto fmt_5xx = LIGHT_PURPLE + "\"%s\" %d %s %s";
			fmt = fmt_5xx.c_str();
			priority = LOG_ERR;
		}
		if (Logging::log_level > LOG_DEBUG) {
			log_response(response);
		}

		auto took = std::chrono::duration_cast<std::chrono::nanoseconds>(request.ends - request.begins).count();
		Metrics::metrics()
			.xapiand_http_requests_summary
			.Add({
				{"method", http_method_str(HTTP_PARSER_METHOD(&request.parser))},
				{"status", string::Number(response.status).str()},
			})
			.Observe(took / 1e9);

		L(priority, NO_COLOR, fmt, request.head, (int)response.status, string::from_bytes(response.size), string::from_delta(request.begins, request.ends));
	}

	L_TIME("Full request took %s, response took %s", string::from_delta(request.begins, request.ends), string::from_delta(request.received, request.ends));
}


ct_type_t
HttpClient::resolve_ct_type(Request& request, ct_type_t ct_type)
{
	L_CALL("HttpClient::resolve_ct_type(%s)", repr(ct_type.to_string()));

	if (ct_type == json_type || ct_type == msgpack_type || ct_type == x_msgpack_type) {
		if (is_acceptable_type(get_acceptable_type(request, json_type), json_type) != nullptr) {
			ct_type = json_type;
		} else if (is_acceptable_type(get_acceptable_type(request, msgpack_type), msgpack_type) != nullptr) {
			ct_type = msgpack_type;
		} else if (is_acceptable_type(get_acceptable_type(request, x_msgpack_type), x_msgpack_type) != nullptr) {
			ct_type = x_msgpack_type;
		}
	}

	std::vector<ct_type_t> ct_types;
	if (ct_type == json_type || ct_type == msgpack_type || ct_type == x_msgpack_type) {
		ct_types = msgpack_serializers;
	} else {
		ct_types.push_back(std::move(ct_type));
	}

	const auto& accepted_type = get_acceptable_type(request, ct_types);
	const auto accepted_ct_type = is_acceptable_type(accepted_type, ct_types);
	if (accepted_ct_type == nullptr) {
		return no_type;
	}

	return *accepted_ct_type;
}


const ct_type_t*
HttpClient::is_acceptable_type(const ct_type_t& ct_type_pattern, const ct_type_t& ct_type)
{
	L_CALL("HttpClient::is_acceptable_type(%s, %s)", repr(ct_type_pattern.to_string()), repr(ct_type.to_string()));

	bool type_ok = false, subtype_ok = false;
	if (ct_type_pattern.first == "*") {
		type_ok = true;
	} else {
		type_ok = ct_type_pattern.first == ct_type.first;
	}
	if (ct_type_pattern.second == "*") {
		subtype_ok = true;
	} else {
		subtype_ok = ct_type_pattern.second == ct_type.second;
	}
	if (type_ok && subtype_ok) {
		return &ct_type;
	}
	return nullptr;
}


const ct_type_t*
HttpClient::is_acceptable_type(const ct_type_t& ct_type_pattern, const std::vector<ct_type_t>& ct_types)
{
	L_CALL("HttpClient::is_acceptable_type((%s, <ct_types>)", repr(ct_type_pattern.to_string()));

	for (auto& ct_type : ct_types) {
		if (is_acceptable_type(ct_type_pattern, ct_type) != nullptr) {
			return &ct_type;
		}
	}
	return nullptr;
}


template <typename T>
const ct_type_t&
HttpClient::get_acceptable_type(Request& request, const T& ct)
{
	L_CALL("HttpClient::get_acceptable_type()");

	if (request.accept_set.empty()) {
		return no_type;
	}
	for (const auto& accept : request.accept_set) {
		if (is_acceptable_type(accept.ct_type, ct)) {
			return accept.ct_type;
		}
	}
	const auto& accept = *request.accept_set.begin();
	auto indent = accept.indent;
	if (indent != -1) {
		request.indented = indent;
	}
	return accept.ct_type;
}


std::pair<std::string, std::string>
HttpClient::serialize_response(const MsgPack& obj, const ct_type_t& ct_type, int indent, bool serialize_error)
{
	L_CALL("HttpClient::serialize_response(%s, %s, %u, %s)", repr(obj.to_string()), repr(ct_type.to_string()), indent, serialize_error ? "true" : "false");

	if (ct_type == no_type) {
		return std::make_pair("", "");
	}
	if (is_acceptable_type(ct_type, json_type) != nullptr) {
		return std::make_pair(obj.to_string(indent), json_type.to_string() + "; charset=utf-8");
	}
	if (is_acceptable_type(ct_type, msgpack_type) != nullptr) {
		return std::make_pair(obj.serialise(), msgpack_type.to_string() + "; charset=utf-8");
	}
	if (is_acceptable_type(ct_type, x_msgpack_type) != nullptr) {
		return std::make_pair(obj.serialise(), x_msgpack_type.to_string() + "; charset=utf-8");
	}
	if (is_acceptable_type(ct_type, html_type) != nullptr) {
		std::function<std::string(const msgpack::object&)> html_serialize = serialize_error ? msgpack_to_html_error : msgpack_to_html;
		return std::make_pair(obj.external(html_serialize), html_type.to_string() + "; charset=utf-8");
	}
	/*if (is_acceptable_type(ct_type, text_type)) {
		error:
			{{ RESPONSE_STATUS }} - {{ RESPONSE_MESSAGE }}

		obj:
			{{ key1 }}: {{ val1 }}
			{{ key2 }}: {{ val2 }}
			...

		array:
			{{ val1 }}, {{ val2 }}, ...
	}*/
	THROW(SerialisationError, "Type is not serializable");
}


void
HttpClient::write_http_response(Request& request, Response& response, enum http_status status, const MsgPack& obj)
{
	L_CALL("HttpClient::write_http_response()");

	auto type_encoding = resolve_encoding(request);
	if (type_encoding == Encoding::unknown && status != HTTP_STATUS_NOT_ACCEPTABLE) {
		enum http_status error_code = HTTP_STATUS_NOT_ACCEPTABLE;
		MsgPack err_response = {
			{ RESPONSE_STATUS, (int)error_code },
			{ RESPONSE_MESSAGE, { "Response encoding gzip, deflate or identity not provided in the Accept-Encoding header" } }
		};
		write_http_response(request, response, error_code, err_response);
		return;
	}

	if (obj.is_undefined()) {
		write(http_response(request, response, status, HTTP_STATUS_RESPONSE | HTTP_HEADER_RESPONSE | HTTP_BODY_RESPONSE));
		return;
	}

	std::vector<ct_type_t> ct_types;
	if (request.ct_type == json_type || request.ct_type == msgpack_type || request.ct_type.empty()) {
		ct_types = msgpack_serializers;
	} else {
		ct_types.push_back(request.ct_type);
	}
	const auto& accepted_type = get_acceptable_type(request, ct_types);

	try {
		auto result = serialize_response(obj, accepted_type, request.indented, (int)status >= 400);
		if (Logging::log_level > LOG_DEBUG && response.size <= 1024 * 10) {
			if (is_acceptable_type(accepted_type, json_type) != nullptr) {
				response.body.append(obj.to_string(4));
			} else if (is_acceptable_type(accepted_type, msgpack_type) != nullptr) {
				response.body.append(obj.to_string(4));
			} else if (is_acceptable_type(accepted_type, x_msgpack_type) != nullptr) {
				response.body.append(obj.to_string(4));
			} else if (is_acceptable_type(accepted_type, html_type) != nullptr) {
				response.body.append(obj.to_string(4));
			} else if (is_acceptable_type(accepted_type, text_type) != nullptr) {
				response.body.append(obj.to_string(4));
			} else if (!obj.empty()) {
				response.body.append("...");
			}
		}
		if (type_encoding != Encoding::none) {
			auto encoded = encoding_http_response(response, type_encoding, result.first, false, true, true);
			if (!encoded.empty() && encoded.size() <= result.first.size()) {
				write(http_response(request, response, status, HTTP_STATUS_RESPONSE | HTTP_HEADER_RESPONSE | HTTP_BODY_RESPONSE | HTTP_CONTENT_TYPE_RESPONSE | HTTP_CONTENT_ENCODING_RESPONSE, 0, 0, encoded, result.second, readable_encoding(type_encoding)));
			} else {
				write(http_response(request, response, status, HTTP_STATUS_RESPONSE | HTTP_HEADER_RESPONSE | HTTP_BODY_RESPONSE | HTTP_CONTENT_TYPE_RESPONSE | HTTP_CONTENT_ENCODING_RESPONSE, 0, 0, result.first, result.second, readable_encoding(Encoding::identity)));
			}
		} else {
			write(http_response(request, response, status, HTTP_STATUS_RESPONSE | HTTP_HEADER_RESPONSE | HTTP_BODY_RESPONSE | HTTP_CONTENT_TYPE_RESPONSE, 0, 0, result.first, result.second));
		}
	} catch (const SerialisationError& exc) {
		status = HTTP_STATUS_NOT_ACCEPTABLE;
		MsgPack response_err = {
			{ RESPONSE_STATUS, (int)status },
			{ RESPONSE_MESSAGE, { "Response type " + accepted_type.to_string() + " " + exc.what() } }
		};
		auto response_str = response_err.to_string();
		if (type_encoding != Encoding::none) {
			auto encoded = encoding_http_response(response, type_encoding, response_str, false, true, true);
			if (!encoded.empty() && encoded.size() <= response_str.size()) {
				write(http_response(request, response, status, HTTP_STATUS_RESPONSE | HTTP_HEADER_RESPONSE | HTTP_BODY_RESPONSE | HTTP_CONTENT_TYPE_RESPONSE | HTTP_CONTENT_ENCODING_RESPONSE, 0, 0, encoded, accepted_type.to_string(), readable_encoding(type_encoding)));
			} else {
				write(http_response(request, response, status, HTTP_STATUS_RESPONSE | HTTP_HEADER_RESPONSE | HTTP_BODY_RESPONSE | HTTP_CONTENT_TYPE_RESPONSE | HTTP_CONTENT_ENCODING_RESPONSE, 0, 0, response_str, accepted_type.to_string(), readable_encoding(Encoding::identity)));
			}
		} else {
			write(http_response(request, response, status, HTTP_STATUS_RESPONSE | HTTP_HEADER_RESPONSE | HTTP_BODY_RESPONSE | HTTP_CONTENT_TYPE_RESPONSE, 0, 0, response_str, accepted_type.to_string()));
		}
		return;
	}
}


Encoding
HttpClient::resolve_encoding(Request& request)
{
	L_CALL("HttpClient::resolve_encoding()");

	if (request.accept_encoding_set.empty()) {
		return Encoding::none;
	}

	constexpr static auto _ = phf::make_phf({
		hhl("gzip"),
		hhl("deflate"),
		hhl("identity"),
		hhl("*"),
	});
	for (const auto& encoding : request.accept_encoding_set) {
		switch (_.fhhl(encoding.encoding)) {
			case _.fhhl("gzip"):
				return Encoding::gzip;
			case _.fhhl("deflate"):
				return Encoding::deflate;
			case _.fhhl("identity"):
				return Encoding::identity;
			case _.fhhl("*"):
				return Encoding::identity;
			default:
				continue;
		}
	}
	return Encoding::unknown;
}


std::string
HttpClient::readable_encoding(Encoding e)
{
	switch (e) {
		case Encoding::none:
			return "none";
		case Encoding::gzip:
			return "gzip";
		case Encoding::deflate:
			return "deflate";
		case Encoding::identity:
			return "identity";
		default:
			return "Encoding:UNKNOWN";
	}
}


std::string
HttpClient::encoding_http_response(Response& response, Encoding e, const std::string& response_obj, bool chunk, bool start, bool end)
{
	L_CALL("HttpClient::encoding_http_response(%s)", repr(response_obj));

	bool gzip = false;
	switch (e) {
		case Encoding::gzip:
			gzip = true;
			/* FALLTHROUGH */
		case Encoding::deflate: {
			if (chunk) {
				if (start) {
					response.encoding_compressor.reset(nullptr, 0, gzip);
					response.encoding_compressor.begin();
				}
				if (end) {
					auto ret = response.encoding_compressor.next(response_obj.data(), response_obj.size(), DeflateCompressData::FINISH_COMPRESS);
					return ret;
				}
				auto ret = response.encoding_compressor.next(response_obj.data(), response_obj.size());
				return ret;
			}

			response.encoding_compressor.reset(response_obj.data(), response_obj.size(), gzip);
			response.it_compressor = response.encoding_compressor.begin();
			std::string encoding_respose;
			while (response.it_compressor) {
				encoding_respose.append(*response.it_compressor);
				++response.it_compressor;
			}
			return encoding_respose;
		}

		case Encoding::identity:
			return response_obj;

		default:
			return std::string();
	}
}


Request::Request(HttpClient* client)
	: indented{-1},
	  expect_100{false},
	  log{L_DELAYED(true, 300s, LOG_WARNING, PURPLE, "Client idle for too long...").release()},
	  begins{std::chrono::system_clock::now()}
{
	parser.data = client;
	http_parser_init(&parser, HTTP_REQUEST);
}


Request::~Request()
{
	log->clear();
}


void
Request::_decode()
{
	L_CALL("Request::decode()");

	if (!raw.empty() && _decoded_body.is_undefined()) {
		// Create a decoded MsgPack object from the raw body

		std::string ct_type_str = ct_type.to_string();
		if (ct_type_str.empty()) {
			ct_type_str = JSON_CONTENT_TYPE;
		}

		rapidjson::Document rdoc;

		constexpr static auto _ = phf::make_phf({
			hhl(FORM_URLENCODED_CONTENT_TYPE),
			hhl(X_FORM_URLENCODED_CONTENT_TYPE),
			hhl(JSON_CONTENT_TYPE),
			hhl(MSGPACK_CONTENT_TYPE),
			hhl(X_MSGPACK_CONTENT_TYPE),
		});
		switch (_.fhhl(ct_type_str)) {
			case _.fhhl(FORM_URLENCODED_CONTENT_TYPE):
			case _.fhhl(X_FORM_URLENCODED_CONTENT_TYPE):
				try {
					json_load(rdoc, raw);
					_decoded_body = MsgPack(rdoc);
					ct_type = json_type;
				} catch (const std::exception&) {
					_decoded_body = MsgPack(raw);
					ct_type = msgpack_type;
				}
				break;
			case _.fhhl(JSON_CONTENT_TYPE):
				json_load(rdoc, raw);
				_decoded_body = MsgPack(rdoc);
				ct_type = json_type;
				break;
			case _.fhhl(MSGPACK_CONTENT_TYPE):
			case _.fhhl(X_MSGPACK_CONTENT_TYPE):
				_decoded_body = MsgPack::unserialise(raw);
				ct_type = msgpack_type;
				break;
			default:
				_decoded_body = MsgPack(raw);
				break;
		}
	}
}


Response::Response()
	: status{HTTP_STATUS_OK},
	  size{0}
{
}
