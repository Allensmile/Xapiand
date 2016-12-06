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

#include "test_query.h"

#include "utils.h"


const std::string path_test_query = std::string(PACKAGE_PATH_TEST) + "/examples/";


// TEST query
const test_query_t test_query[] {
	//Testing string field terms.
	{
		{ "description:\"American teenager\"" }, { }, { "Back to the Future", "Planet Apes" }, "movie"
	},
	{
		{ "\"American teenager\"" }, { }, { "Back to the Future" }, "movie"
	},
	{
		{ "name.es:'hola mundo'" }, { }, { "3", "8" }, "number"
	},
	{
		{ "name.en:bookstore" }, { }, { "2" }, "number"
	},
	// autor.male is a bool_term. Therefore it is case sensitive.
	{
		{ "actors.male:'Michael J. Fox'" }, { }, { "Back to the Future" }, "movie"
	},
	{
		{ "actors.male:'Michael j. Fox'" }, { }, { }, "movie"
	},
	{
		{ "actors.male:'Roddy McDowall'" }, { }, { "Planet Apes" }, "movie"
	},
	{
		{ "actors.male:'roddy mcdowall'" }, { }, { }, "movie"
	},
	// autor.female is not a bool_term. Therefore it is not case sensitive.
	{
		{ "actors.female:LINDA" }, { }, { "Planet Apes" }, "movie"
	},
	{
		{ "actors.female:linda" }, { }, { "Planet Apes" }, "movie"
	},
	// OR
	{
		{ "actors.female:linda OR actors.male:'Michael J. Fox'" }, { }, { "Back to the Future", "Planet Apes" }, "movie"
	},
	// AND
	{
		{ "actors.female:linda AND actors.male:'Michael J. Fox'" }, { }, { }, "movie"
	},
	// Testing date terms
	{
		{ "released:1985-07-03" }, { }, { "Back to the Future" }, "movie"
	},
	{
		{ "date:'2011-01-01||+1y-1y+3M-3M'" }, { }, { "1", "10" }, "number"
	},
	{
		{ "date:'2011-01-01||+4y'" }, { }, { "5", "6" }, "number"
	},
	// OR
	{
		{ "date:'2011-01-01||+1y-1y+3M-3M' OR date:'2011-01-01||+4y'" }, { }, { "1", "5", "6", "10" }, "number"
	},
	// AND
	{
		{ "date:'2011-01-01||+1y-1y+3M-3M' AND date:'2011-01-01||+4y'" }, { }, { }, "number"
	},
	// Testing numeric terms
	{
		{ "year:2001" }, { }, { "2", "9" }, "number"
	},
	{
		{ "year:0" }, { }, { "3", "8" }, "number"
	},
	// OR
	{
		{ "year:2001 OR year:0" }, { }, { "2", "3", "8", "9" }, "number"
	},
	// AND
	{
		{ "year:2001 AND year:0" }, { }, { }, "number"
	},
	// Testing boolean terms
	{
		{ "there:true" }, { }, { "1", "3", "4", "7", "8", "10" }, "number"
	},
	{
		{ "there:false" }, { }, { "1", "2", "5", "6", "9", "10" }, "number"
	},
	// OR
	{
		{ "there:true OR there:false" }, { }, { "1", "2", "3", "4", "5", "6", "7", "8", "9", "10" }, "number"
	},
	// AND
	{
		{ "there:true AND there:false" }, { }, { "1", "10" }, "number"
	}
	// Testing geospatials is in test_geo.cc.
};


// TEST partials.
const test_query_t test_partials[] {
	// Only applying for strings types.
	{
		{ }, { "directed_by:Rob" }, { "Back to the Future" }, "movie"
	},
	{
		{ }, { "directed_by:Zem" }, { "Back to the Future" }, "movie"
	},
	{
		{ }, { "description:t" }, {"Back to the Future", "Planet Apes" }, "movie"
	}
};


static DB_Test db_query(".db_query.db", std::vector<std::string>({
		// FIXME:
		// Examples used in test geo.
		// path_test_query + "json/geo_1.txt",
		// path_test_query + "json/geo_2.txt",
		// path_test_query + "json/geo_3.txt",
		// path_test_query + "json/geo_4.txt",
		// path_test_query + "json/geo_5.txt",
		// path_test_query + "json/geo_6.txt",
		// path_test_query + "json/geo_7.txt",
		// path_test_query + "json/geo_8.txt",
		// Examples used in test sort.
		path_test_query + "sort/doc1.txt",
		path_test_query + "sort/doc2.txt",
		path_test_query + "sort/doc3.txt",
		path_test_query + "sort/doc4.txt",
		path_test_query + "sort/doc5.txt",
		path_test_query + "sort/doc6.txt",
		path_test_query + "sort/doc7.txt",
		path_test_query + "sort/doc8.txt",
		path_test_query + "sort/doc9.txt",
		path_test_query + "sort/doc10.txt",
		// Search examples.
		path_test_query + "json/example_1.txt",
		path_test_query + "json/example_2.txt"
	}), DB_WRITABLE | DB_SPAWN | DB_NOWAL);


static int make_search(const test_query_t _tests[], int len) {
	int cont = 0;
	query_field_t query;
	query.offset = 0;
	query.limit = 20;
	query.check_at_least = 0;
	query.spelling = true;
	query.synonyms = false;
	query.is_fuzzy = false;
	query.is_nearest = false;
	query.sort.push_back(ID_FIELD_NAME); // All the result are sort by its id.

	for (int i = 0; i < len; ++i) {
		test_query_t p = _tests[i];
		query.query.clear();

		// Insert query
		for (const auto& _query : p.query) {
			query.query.push_back(_query);
		}

		MSet mset;
		std::vector<std::string> suggestions;

		try {
			mset = db_query.db_handler.get_mset(query, nullptr, nullptr, suggestions);
			// Check by documents
			if (mset.size() != p.expect_datas.size()) {
				++cont;
				L_ERR(nullptr, "ERROR: Different number of documents. Obtained %d. Expected: %zu.", mset.size(), p.expect_datas.size());
			} else {
				Xapian::MSetIterator m = mset.begin();
				for (auto it = p.expect_datas.begin(); m != mset.end(); ++it, ++m) {
					auto document = db_query.db_handler.get_document(*m);
					auto obj_data = document.get_obj();
					try {
						auto data = obj_data.at(p.field);
						auto str_data = data.as_string();
						if (it->compare(str_data) != 0) {
							++cont;
							L_ERR(nullptr, "ERROR: Result = %s:%s   Expected = %s:%s", p.field.c_str(), str_data.c_str(), p.field.c_str(), it->c_str());
						}
					} catch (const msgpack::type_error& exc) {
						++cont;
						L_EXC(nullptr, "ERROR: %s", exc.what());
					}
				}
			}
		} catch (const std::exception& exc) {
			L_EXC(nullptr, "ERROR: %s\n", exc.what());
			++cont;
		}
	}

	return cont;
}


int test_query_search() {
	INIT_LOG
	try {
		int cont = make_search(test_query, arraySize(test_query));
		if (cont == 0) {
			L_DEBUG(nullptr, "Testing search using query is correct!");
		} else {
			L_ERR(nullptr, "ERROR: Testing search using query has mistakes.");
		}
		RETURN(cont);
	} catch (const Xapian::Error& exc) {
		L_EXC(nullptr, "ERROR: %s", exc.get_msg().c_str());
		RETURN(1);
	} catch (const std::exception& exc) {
		L_EXC(nullptr, "ERROR: %s", exc.what());
		RETURN(1);
	}
}


int test_partials_search() {
	INIT_LOG
	try {
		int cont = make_search(test_partials, arraySize(test_partials));
		if (cont == 0) {
			L_DEBUG(nullptr, "Testing search using partials is correct!");
		} else {
			L_ERR(nullptr, "ERROR: Testing search using partials has mistakes.");
		}
		RETURN(cont);
	} catch (const Xapian::Error& exc) {
		L_EXC(nullptr, "ERROR: %s", exc.get_msg().c_str());
		RETURN(1);
	} catch (const std::exception& exc) {
		L_EXC(nullptr, "ERROR: %s", exc.what());
		RETURN(1);
	}
}
