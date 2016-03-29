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

#include "test_utils.h"

#include "log.h"


//TESTS CASES
const struct test_url_path urls[] {
	{
		"db_new.db,db_new.db/_search", {"db_new.db", "db_new.db"}, {""}, "", "_search", "", 0, true
	},
	{
		"/AQjN/BVf/78w/QjNBVfWKH78w/clients/clients.client.cd7ec34a-5d4a-11e5-b0b2-34363bc9ddd6/", {"/AQjN/BVf/78w/QjNBVfWKH78w/clients"}, {""}, "", "clients.client.cd7ec34a-5d4a-11e5-b0b2-34363bc9ddd6", "", 0, true
	},
	{
		"/favicon.ico", {""}, {""}, "", "favicon.ico", "", -2, true
	},
	{
		"//patt/to:namespace1/index1@host1,//namespace2/index2@host2,namespace3/index3@host3/type/search////", {"namespace1/index1", "//namespace2/index2", "namespace3/index3"}, {"host1", "host2", "host3/type"}, "", "search", "//patt/to", 0, true
	},
	{
		"/patt/to:namespace1/index1@host1,@host2,namespace3/index3/search", {"namespace1/index1"}, {"host1"}, "", "search", "/patt/to", 0, true
	},
	{
		"/database/", {""}, {""}, "", "database", "", -2, true
	},
	{
		"path/1", {"path"}, {""}, "", "1", "", 0, true
	},
	{
		"/db_titles/localhost/_upload/", {"/db_titles/localhost"}, {""}, "", "_upload", "", 0, true
	},
	{
		"delete", {""}, {""}, "", "delete", "", -1, true
	},
	{
		"//patt/to:namespace1/index1@host1,//namespace2/index2@host2:8890,namespace3/index3@host3/type1,type2/search////", {"namespace1/index1", "//namespace2/index2", "namespace3/index3", "type2"}, {"host1", "host2:8890", "host3/type1", "host3/type1"}, "", "search", "//patt/to", 0, true
	},
	{
		"/patt/to:namespace1/index1@host1,/namespace2/index2@host2,namespace3/index3@host3/t1/_upload/search/", {"namespace1/index1", "/namespace2/index2", "namespace3/index3"}, {"host1", "host2", "host3/t1"}, "_upload", "search", "/patt/to", 0, true
	},
	{
		"/database.db/subdir/_upload/3/", {"/database.db/subdir"}, {""}, "_upload", "3", "", 0, true
	},
	{
		"usr/dir:subdir,_upload/1", {"subdir"}, {""}, "_upload", "1", "usr/dir", 0, true
	},
	{
		"/database.db/_upload/_search/", {"/database.db"}, {""}, "_upload", "_search", "", 0, true
	},
	{
		"/_stats/", {""}, {""}, "", "_stats", "", 10, true
	},
	{
		"/index/_stats", {"/index"}, {""}, "", "_stats", "", 0, true
	},
	{
		"/index/_stats/1", {"/index"}, {""}, "_stats", "1", "", 0, true
	},
	{
		"/index/_stats/1", {"/index/_stats/1"}, {""}, "", "", "", 0, false
	}
};


int run_url_path(const struct test_url_path& u) {
	 struct parser_url_path_t p;
	 memset(&p, 0, sizeof(p));

	 int rval = url_path(u.url.c_str(), u.url.size(), &p, u.find_id);
	 if (strncmp(p.off_namespace, u.nspace.c_str(), p.len_namespace) != 0) {
		return 1;
	}

	if (rval != u.reval) {
		return 1;
	}

	int pos = 0;
	if (rval == 0) {
		while (rval == 0) {
			if (p.len_path == u.path[pos].size()) {
				if (strncmp(p.off_path, u.path[pos].c_str(), p.len_path) != 0) {
					print_error_url(u.path[pos], std::string(p.off_path, p.len_path));
					return 1;
				}
			} else {
				print_error_url(u.path[pos], std::string(p.off_path, p.len_path));
				return 1;
			}

			if (p.len_host == u.host[pos].size()) {
				if (strncmp(p.off_host, u.host[pos].c_str(), p.len_host) != 0) {
					print_error_url(u.host[pos], std::string(p.off_host, p.len_host));
					return 1;
				}
			} else {
				print_error_url(u.host[pos], std::string(p.off_host, p.len_host));
				return 1;
			}

			if (p.len_command == u.command.size()) {
				if (strncmp(p.off_command, u.command.c_str(), p.len_command) != 0) {
					print_error_url(u.command, std::string(p.off_command, p.len_command));
					return 1;
				}
			} else {
				print_error_url(u.command, std::string(p.off_command, p.len_command));
				return 1;
			}

			if (p.len_parameter == u.parameter.size()) {
				if (strncmp(p.off_parameter, u.parameter.c_str(), p.len_parameter) != 0) {
					print_error_url(u.parameter, std::string(p.off_parameter, p.len_parameter));
					return 1;
				}
			} else {
				print_error_url(u.parameter, std::string(p.off_parameter, p.len_parameter));
				return 1;
			}
			rval = url_path(u.url.c_str(), u.url.size(), &p);
			++rval;
		}
	} else {
		if (strncmp(p.off_command, u.command.c_str(), p.len_command) != 0) {
			return 1;
		}
	}
	return 0;
}



int test_url_path() {

	int count = 0;
	size_t array_size = arraySize(urls);
	for (int i = 0; i < static_cast<int>(array_size); i++) {
		if (run_url_path(urls[i]) != 0) {
			++count;
		}
	}
	return count;
}


void print_error_url(std::string str, std::string str_err) {
	L_ERR(nullptr, "Error: the value obtained from the url path should be [%s] but it is [%s]", str.c_str(), str_err.c_str());
}