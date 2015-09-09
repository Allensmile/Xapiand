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


#include "test_query.h"
#include "../src/cJSON.h"
#include "../src/utils.h"
#include "../src/serialise.h"
#include "../src/database.h"
#include "../src/endpoint.h"

#include <sstream>
#include <fstream>


static DatabaseQueue *queue = NULL;
static Database *database = NULL;
static std::string name_database(".db_testsearch.db");


// TEST query
const test_query_t test_query[] {
	{
		{ "description:American teenager" }, { }, { }, { }, { "Back to the Future", "Planet Apes" }, { }
	},
	{
		{ "American teenager" }, { }, { }, { }, { "Back to the Future" }, { }
	},
	{
		{ "description:Dakota" }, { }, { }, { }, { "North Dakota", "Bismarck", "Minot", "Rapid City", "North Dakota and South Dakota" }, { }
	},
	{
		{ "description:dakotA" }, { }, { }, { }, { "North Dakota", "Bismarck", "Minot", "Rapid City", "North Dakota and South Dakota" }, { }
	},
	{
		{ "name:hola mundo" }, { }, { }, { }, { "3", "8" }, { }
	},
	{
		{ "name:\"book store\"" }, { }, { }, { }, { "2" }, { }
	}
};

// TEST terms
const test_query_t test_terms[] {
	// it get different results if we use terms instead of query.
	{
		{ }, { "name:hola mundo" }, { }, { }, { "3", "4", "7", "8" }, { }
	},
	// EMPTY because we do not have the stemmer like query.
	{
		{ }, { "name:\"book store\"" }, { }, { }, { }, { }
	},
	// Testing string term.
	// autor__male is a bool_term. Therefore it is case sensitive.
	{
		{ }, { "actors__male:\"Michael J. Fox\"" }, { }, { }, { "Back to the Future" }, { }
	},
	{
		{ }, { "actors__male:\"Michael j. Fox\"" }, { }, { }, { }, { }
	},
	{
		{ }, { "actors__male:\"Roddy McDowall\"" }, { }, { }, { "Planet Apes" }, { }
	},
	{
		{ }, { "actors__male:\"roddy mcdowall\"" }, { }, { }, { }, { }
	},
	// autor__female is not a bool_term. Therefore it is not case sensitive.
	{
		{ }, { "actors__female:LINDA" }, { }, { }, { "Planet Apes" }, { }
	},
	{
		{ }, { "actors__female:linda" }, { }, { }, { "Planet Apes" }, { }
	},
	// OR
	{
		{ }, { "actors__female:linda actors__male:\"Michael J. Fox\"" }, { }, { }, { "Back to the Future", "Planet Apes" }, { }
	},
	// AND
	{
		{ }, { "actors__female:linda", "actors__male:\"Michael J. Fox\"" }, { }, { }, { }, { }
	},
	// Testing date terms
	{
		{ }, { "released:1985-07-03" }, { }, { }, { "Back to the Future" }, { }
	},
	{
		{ }, { "date:2011-01-01||+1y-1y+3M-3M" }, { }, { }, { "10", "1" }, { }
	},
	{
		{ }, { "date:2011-01-01||+4y" }, { }, { }, { "5", "6" }, { }
	},
	// OR
	{
		{ }, { "date:2011-01-01||+1y-1y+3M-3M date:2011-01-01||+4y" }, { }, { }, { "5", "6", "10", "1" }, { }
	},
	// AND
	{
		{ }, { "date:2011-01-01||+1y-1y+3M-3M", "date:2011-01-01||+4y" }, { }, { }, { }, { }
	},
	// Testing numeric terms
	{
		{ }, { "year:2001" }, { }, { }, { "2", "9" }, { }
	},
	{
		{ }, { "year:0" }, { }, { }, { "3", "8" }, { }
	},
	// OR
	{
		{ }, { "year:2001 year:0" }, { }, { }, { "2", "3", "8", "9" }, { }
	},
	// AND
	{
		{ }, { "year:2001", "year:0" }, { }, { }, { }, { }
	},
	// Testing boolean terms
	{
		{ }, { "there:true" }, { }, { }, { "3", "4", "7", "8", "10", "1" }, { }
	},
	{
		{ }, { "there:false" }, { }, { }, { "2", "5", "6", "9", "10", "1" }, { }
	},
	// OR
	{
		{ }, { "there:true there:false" }, { }, { }, { "2", "3", "4", "5", "6", "7", "8", "9", "10", "1" }, { }
	},
	// AND
	{
		{ }, { "there:true", "there:false" }, { }, { }, { "10", "1" }, { }
	}
	// Testing geospatials is in test_geo.cc.
};


// TEST partials.
const test_query_t test_partials[] {
	// Only applying for strings types.
	{
		{ }, { }, { "directed_by:Rob" }, { }, { "Back to the Future" }, { }
	},
	{
		{ }, { }, { "directed_by:Zem" }, { }, { "Back to the Future" }, { }
	},
	{
		{ }, { }, { "description:Dak" }, { }, { "North Dakota", "Bismarck", "Minot", "Rapid City", "North Dakota and South Dakota" }, { }
	},
	{
		{ }, { }, { "description:t" }, { }, { "North Dakota", "Back to the Future", "Planet Apes", "Utah", "Wyoming", "Mountain View, Wyoming" }, { }
	},
	{
		{ }, { }, { "description:south dak" }, { }, { "Rapid City", "Utah", "North Dakota and South Dakota" }, { }
	}
};


// TEST facets.
const test_query_t test_facets[] {
	// Test string value
	{
		{ "description:American" }, { }, { }, { "actors__male" }, { "Back to the Future", "Planet Apes" },
		{ "Charlton Heston", "Christopher Lloyd", "Michael J. Fox", "Roddy McDowall", "Thomas F. Wilson"  }
	},
	{
		{ "description:American" }, { }, { }, { "actors__female" }, { "Back to the Future", "Planet Apes" },
		{ "Jennifer Parker", "Kim Hunter", "Lea Thompson", "Linda Harrison"  }
	},
	// Test numerical value
	{
		{ "there:true" }, { }, { }, { "year" }, { "3", "4", "7", "8", "10", "1" },
		{ "-10000.000000", "0.000000", "100.000000", "2010.000000", "2015.000000", "2020.000000"  }
	},
	// Test date value
	{
		{ "there:false" }, { }, { }, { "date" }, { "2", "5", "6", "9", "10", "1" },
		{ "1810-01-01T00:00:00.000", "1910-01-01T00:00:00.000", "2010-10-21T00:00:00.000", "2011-01-01T00:00:00.000", "2015-01-01T00:00:00.000" }
	},
	// Test bool value
	{
		{ "year:2001" }, { }, { }, { "there" }, { "2", "9" },
		{ "false" }
	},
	// Test geo value.
	{
		{ "year:2001" }, { }, { }, { "location" }, { "2", "9" },
		{
			"Ranges: { [17849634882335139, 17849634882335139] }  Centroids: { (0.720409, 0.604495, 0.339996) }",
			"Ranges: { [17904729175652709, 17904729175652709] }  Centroids: { (0.322660, 0.558863, 0.763913) }"
		}
	},
	{
		{ "year:100" }, { }, { }, { "location" }, { "4", "7" },
		{
			"Ranges: { [17455794073108480, 17455794077302783] [17455794131828736, 17455794136023039] [17455794156994560, 17455794182160383] [17455794186354688, 17455794198937599] [17455794203131904, 17455794207326207] [17455794597396480, 17455794601590783] [17455794609979392, 17455794626756607] [17455794651922432, 17455794656116735] [17455794660311040, 17455794677088255] [17455794693865472, 17455794744197119] [17455794794528768, 17455794861637631] [17455794865831936, 17455794928746495] [17455794949718016, 17455794953912319] [17455794962300928, 17455794966495231] [17455794970689536, 17455794983272447] [17455794995855360, 17455795062964223] }  Centroids: { (0.998790, 0.034879, 0.034666) }"
		}
	},
	{
		{ "description:US" }, { }, { }, { "location" }, { "North Dakota and South Dakota" },
		{
			"Ranges: { [15061110277275648, 15061247716229119] [15061316435705856, 15061385155182591] [15622960719069184, 15623510474883071] [15623785352790016, 15624060230696959] [15624609986510848, 15625022303371263] [15625091022848000, 15625434620231679] [15627633643487232, 15627702362963967] [15628458277208064, 15628526996684799] [15628595716161536, 15628733155115007] [15629008033021952, 15629420349882367] [15629489069359104, 15629626508312575] [15635330224881664, 15635673822265343] [15635742541742080, 15635948700172287] [15636017419649024, 15636154858602495] [15636429736509440, 15636704614416383] [15636842053369856, 15636910772846591] [15636979492323328, 15637048211800063] [15637116931276800, 15639453393485823] }  Centroids: { (-0.127065, -0.665547, 0.735460) (-0.128320, -0.703822, 0.698691) }"
		}
	}
};



int create_test_db()
{
	int cont = 0;
	local_node.name.assign("node_test");
	local_node.binary_port = XAPIAND_BINARY_SERVERPORT;

	Endpoints endpoints;
	Endpoint e;
	e.node_name.assign("node_test");
	e.port = XAPIAND_BINARY_SERVERPORT;
	e.path.assign(name_database);
	e.host.assign("0.0.0.0");
	endpoints.insert(e);

	// There are delete in the make_search.
	queue = new DatabaseQueue();
	database = new Database(queue, endpoints, DB_WRITABLE | DB_SPAWN);

	std::vector<std::string> _docs({
		// Examples used in test geo.
		"examples/geo_search/Json_geo_1.txt",
		"examples/geo_search/Json_geo_2.txt",
		"examples/geo_search/Json_geo_3.txt",
		"examples/geo_search/Json_geo_4.txt",
		"examples/geo_search/Json_geo_5.txt",
		"examples/geo_search/Json_geo_6.txt",
		"examples/geo_search/Json_geo_7.txt",
		"examples/geo_search/Json_geo_8.txt",
		// Examples used in test sort.
		"examples/sort/doc1.txt",
		"examples/sort/doc2.txt",
		"examples/sort/doc3.txt",
		"examples/sort/doc4.txt",
		"examples/sort/doc5.txt",
		"examples/sort/doc6.txt",
		"examples/sort/doc7.txt",
		"examples/sort/doc8.txt",
		"examples/sort/doc9.txt",
		"examples/sort/doc10.txt",
		// Search examples.
		"examples/search_examples/Json_example_1.txt",
		"examples/search_examples/Json_example_2.txt"
	});

	// Index documents in the database.
	size_t i = 1;
	for (std::vector<std::string>::iterator it(_docs.begin()); it != _docs.end(); it++) {
		std::ifstream fstream(*it);
		std::stringstream buffer;
		buffer << fstream.rdbuf();
		unique_cJSON document(cJSON_Parse(buffer.str().c_str()), cJSON_Delete);
		if (not database->index(document.get(), std::to_string(i), true)) {
			cont++;
			LOG_ERR(NULL, "ERROR: File %s can not index\n", it->c_str());
		}
		fstream.close();
		++i;
	}

	return cont;
}


int make_search(const test_query_t _tests[], int len)
{
	int cont = 0;
	query_t query;
	query.offset = 0;
	query.limit = 20;
	query.check_at_least = 0;
	query.spelling = true;
	query.synonyms = false;
	query.is_fuzzy = false;
	query.is_nearest = false;
	query.sort.push_back(RESERVED_ID); // All the result are sort by its id.

	for (size_t i = 0; i < len; ++i) {
		test_query_t p = _tests[i];
		query.query.clear();
		query.terms.clear();
		query.partial.clear();
		query.facets.clear();

		// Insert query
		std::vector<std::string>::const_iterator it(p.query.begin());
		for ( ; it != p.query.end(); it++) {
			query.query.push_back(*it);
		}

		// Insert terms
		it = p.terms.begin();
		for ( ; it != p.terms.end(); it++) {
			query.terms.push_back(*it);
		}

		// Insert partials
		it = p.partial.begin();
		for ( ; it != p.partial.end(); it++) {
			query.partial.push_back(*it);
		}

		// Insert facets
		it = p.facets.begin();
		for ( ; it != p.facets.end(); it++) {
			query.facets.push_back(*it);
		}


		Xapian::MSet mset;
		std::vector<std::string> suggestions;
		std::vector<std::pair<std::string, std::unique_ptr<MultiValueCountMatchSpy>>> spies;

		int rmset = database->get_mset(query, mset, spies, suggestions);
		if (rmset != 0) {
			cont++;
			LOG_ERR(NULL, "ERROR: Failed in get_mset\n");
		} else {
			// Check by documents
			if (mset.size() != p.expect_datas.size()) {
				cont++;
				LOG_ERR(NULL, "ERROR: Different number of documents obtained, get: %zu expected: %zu\n", mset.size(), p.expect_datas.size());
			} else {
				it = p.expect_datas.begin();
				Xapian::MSetIterator m = mset.begin();
				for ( ; m != mset.end(); ++it, ++m) {
					std::string data(m.get_document().get_data());
					unique_cJSON object(cJSON_Parse(data.c_str()), cJSON_Delete);
					cJSON* object_data = cJSON_GetObjectItem(object.get(), RESERVED_DATA);
					if (object_data && it->compare(object_data->valuestring) != 0) {
						cont++;
						LOG_ERR(NULL, "ERROR: Result = %s:%s   Expected = %s:%s\n", RESERVED_DATA, data.c_str(), RESERVED_DATA, it->c_str());
					}
				}
			}
			// Check by facets
			if (!p.facets.empty()) {
				std::vector<std::pair<std::string, std::unique_ptr<MultiValueCountMatchSpy>>>::const_iterator spy(spies.begin());
				Xapian::TermIterator facet = (*spy).second->values_begin();
				it = p.expect_facets.begin();
				for ( ; facet != (*spy).second->values_end() && it !=  p.expect_facets.end(); ++facet, ++it) {
					data_field_t field_t = database->get_data_field((*spy).first);
					std::string str_facet(Unserialise::unserialise(field_t.type, *facet));
					if (str_facet.compare(*it) != 0) {
						cont++;
						LOG_ERR(NULL, "ERROR: Facet result = %s   Facet expected = %s\n", str_facet.c_str(), it->c_str());
					}
				}
				if (it !=  p.expect_facets.end() || facet != (*spy).second->values_end()) {
					cont++;
					LOG_ERR(NULL, "ERROR: Different number of terms generated by facets obtained\n");
				}
			}
		}
	}

	// Delete de database and release memory.
	delete_files(name_database);
	delete database;
	delete queue;

	return cont;
}


int test_query_search()
{
	try {
		int cont = create_test_db();
		if (cont == 0 && make_search(test_query, sizeof(test_query) / sizeof(test_query[0])) == 0) {
			LOG(NULL, "Testing search using query is correct!\n");
			return 0;
		} else {
			LOG_ERR(NULL, "ERROR: Testing search using query has mistakes.\n");
			return 1;
		}
	} catch (const Xapian::Error &err) {
		LOG_ERR(NULL, "ERROR: %s\n", err.get_msg().c_str());
		return 1;
	} catch (const std::exception &err) {
		LOG_ERR(NULL, "ERROR: %s\n", err.what());
		return 1;
	}
}


int test_terms_search()
{
	try {
		int cont = create_test_db();
		if (cont == 0 && make_search(test_terms, sizeof(test_terms) / sizeof(test_terms[0])) == 0) {
			LOG(NULL, "Testing search using terms is correct!\n");
			return 0;
		} else {
			LOG_ERR(NULL, "ERROR: Testing search using terms has mistakes.\n");
			return 1;
		}
	} catch (const Xapian::Error &err) {
		LOG_ERR(NULL, "ERROR: %s\n", err.get_msg().c_str());
		return 1;
	} catch (const std::exception &err) {
		LOG_ERR(NULL, "ERROR: %s\n", err.what());
		return 1;
	}
}


int test_partials_search()
{
	try {
		int cont = create_test_db();
		if (cont == 0 && make_search(test_partials, sizeof(test_partials) / sizeof(test_partials[0])) == 0) {
			LOG(NULL, "Testing search using partials is correct!\n");
			return 0;
		} else {
			LOG_ERR(NULL, "ERROR: Testing search using partials has mistakes.\n");
			return 1;
		}
	} catch (const Xapian::Error &err) {
		LOG_ERR(NULL, "ERROR: %s\n", err.get_msg().c_str());
		return 1;
	} catch (const std::exception &err) {
		LOG_ERR(NULL, "ERROR: %s\n", err.what());
		return 1;
	}
}


int test_facets_search()
{
	try {
		int cont = create_test_db();
		if (cont == 0 && make_search(test_facets, sizeof(test_facets) / sizeof(test_facets[0])) == 0) {
			LOG(NULL, "Testing facets is correct!\n");
			return 0;
		} else {
			LOG_ERR(NULL, "ERROR: Testing facets has mistakes.\n");
			return 1;
		}
	} catch (const Xapian::Error &err) {
		LOG_ERR(NULL, "ERROR: %s\n", err.get_msg().c_str());
		return 1;
	} catch (const std::exception &err) {
		LOG_ERR(NULL, "ERROR: %s\n", err.what());
		return 1;
	}
}