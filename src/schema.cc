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

#include "schema.h"

#include "geo/wkt_parser.h"
#include "multivalue/generate_terms.h"
#include "log.h"
#include "manager.h"
#include "utils.h"


const std::unordered_set<std::string> reserved_field_names({
	RESERVED_ID_FIELD, RESERVED_UUID_FIELD, RESERVED_GEO_FIELD, RESERVED_DATE_FIELD
});


/*
 * Unordered Maps used for reading user data specification.
 */

static const std::unordered_map<std::string, UnitTime> map_acc_date({
	{ "second",     UnitTime::SECOND     }, { "minute",  UnitTime::MINUTE  },
	{ "hour",       UnitTime::HOUR       }, { "day",     UnitTime::DAY     },
	{ "month",      UnitTime::MONTH      }, { "year",    UnitTime::YEAR    },
	{ "decade",     UnitTime::DECADE     }, { "century", UnitTime::CENTURY },
	{ "millennium", UnitTime::MILLENNIUM }
});


static const std::unordered_map<std::string, StemStrategy> map_stem_strategy({
	{ "stem_none",  StemStrategy::STEM_NONE   }, { "none",  StemStrategy::STEM_NONE   },
	{ "stem_some",  StemStrategy::STEM_SOME   }, { "some",  StemStrategy::STEM_SOME   },
	{ "stem_all",   StemStrategy::STEM_ALL    }, { "all",   StemStrategy::STEM_ALL    },
	{ "stem_all_z", StemStrategy::STEM_ALL_Z  }, { "all_z", StemStrategy::STEM_ALL_Z  }
});


static const std::unordered_map<std::string, TypeIndex> map_index({
	{ "none",          TypeIndex::NONE          }, { "terms",        TypeIndex::TERMS        },
	{ "values",        TypeIndex::VALUES        }, { "all",          TypeIndex::ALL          },
	{ "field_terms",   TypeIndex::FIELD_TERMS   }, { "field_values", TypeIndex::FIELD_VALUES },
	{ "field_all",     TypeIndex::FIELD_ALL     }, { "global_terms", TypeIndex::GLOBAL_TERMS },
	{ "global_values", TypeIndex::GLOBAL_VALUES }, { "global_all",   TypeIndex::GLOBAL_ALL   }
});


static const std::unordered_map<std::string, FieldType> map_type({
	{ FLOAT_STR,       FieldType::FLOAT        }, { INTEGER_STR,     FieldType::INTEGER     },
	{ POSITIVE_STR,    FieldType::POSITIVE     }, { STRING_STR,      FieldType::STRING      },
	{ TEXT_STR,        FieldType::TEXT         }, { DATE_STR,        FieldType::DATE        },
	{ GEO_STR,         FieldType::GEO          }, { BOOLEAN_STR,     FieldType::BOOLEAN     },
	{ UUID_STR,        FieldType::UUID         }
});


/*
 * Default accuracies.
 */

static const std::vector<uint64_t> def_accuracy_geo({ 0, 5, 10, 15, 20, 25 });
static const std::vector<uint64_t> def_accuracy_num({ 100, 1000, 10000, 100000, 1000000, 10000000 });
static const std::vector<uint64_t> def_accuracy_date({ toUType(UnitTime::HOUR), toUType(UnitTime::DAY), toUType(UnitTime::MONTH), toUType(UnitTime::YEAR), toUType(UnitTime::DECADE), toUType(UnitTime::CENTURY) });


/*
 * Default acc_prefixes for global values.
 */

static const std::vector<std::string> global_acc_prefix_geo({
	get_prefix("_geo_0",  DOCUMENT_CUSTOM_TERM_PREFIX, toUType(FieldType::GEO)),
	get_prefix("_geo_5",  DOCUMENT_CUSTOM_TERM_PREFIX, toUType(FieldType::GEO)),
	get_prefix("_geo_10", DOCUMENT_CUSTOM_TERM_PREFIX, toUType(FieldType::GEO)),
	get_prefix("_geo_15", DOCUMENT_CUSTOM_TERM_PREFIX, toUType(FieldType::GEO)),
	get_prefix("_geo_20", DOCUMENT_CUSTOM_TERM_PREFIX, toUType(FieldType::GEO)),
	get_prefix("_geo_25", DOCUMENT_CUSTOM_TERM_PREFIX, toUType(FieldType::GEO))
});

static const std::vector<std::string> global_acc_prefix_date({
	get_prefix("_date_hour",    DOCUMENT_CUSTOM_TERM_PREFIX, toUType(FieldType::DATE)),
	get_prefix("_date_day",     DOCUMENT_CUSTOM_TERM_PREFIX, toUType(FieldType::DATE)),
	get_prefix("_date_month",   DOCUMENT_CUSTOM_TERM_PREFIX, toUType(FieldType::DATE)),
	get_prefix("_date_year",    DOCUMENT_CUSTOM_TERM_PREFIX, toUType(FieldType::DATE)),
	get_prefix("_date_decade",  DOCUMENT_CUSTOM_TERM_PREFIX, toUType(FieldType::DATE)),
	get_prefix("_date_century", DOCUMENT_CUSTOM_TERM_PREFIX, toUType(FieldType::DATE))
});

static const std::vector<std::string> global_acc_prefix_num({
	get_prefix("_num_100",      DOCUMENT_CUSTOM_TERM_PREFIX, toUType(FieldType::INTEGER)),
	get_prefix("_num_1000",     DOCUMENT_CUSTOM_TERM_PREFIX, toUType(FieldType::INTEGER)),
	get_prefix("_num_10000",    DOCUMENT_CUSTOM_TERM_PREFIX, toUType(FieldType::INTEGER)),
	get_prefix("_num_100000",   DOCUMENT_CUSTOM_TERM_PREFIX, toUType(FieldType::INTEGER)),
	get_prefix("_num_1000000",  DOCUMENT_CUSTOM_TERM_PREFIX, toUType(FieldType::INTEGER)),
	get_prefix("_num_10000000", DOCUMENT_CUSTOM_TERM_PREFIX, toUType(FieldType::INTEGER))
});


/*
 * Acceptable values string used when there is a data inconsistency.
 */

static const std::string str_set_acc_date = []() {
	std::string res("{ ");
	for (const auto& p : map_acc_date) {
		res.append(p.first).append(", ");
	}
	res.push_back('}');
	return res;
}();

static const std::string str_set_stem_strategy = []() {
	std::string res("{ ");
	for (const auto& p : map_stem_strategy) {
		res.append(p.first).append(", ");
	}
	res.push_back('}');
	return res;
}();

static const std::string str_set_index = []() {
	std::string res("{ ");
	for (const auto& p : map_index) {
		res.append(p.first).append(", ");
	}
	res.push_back('}');
	return res;
}();

static const std::string str_set_type = []() {
	std::string res("{ ");
	for (const auto& p : map_type) {
		res.append(p.first).append(", ");
	}
	res.push_back('}');
	return res;
}();


const specification_t default_spc;


const std::unordered_map<std::string, dispatch_reserved> map_dispatch_document({
	{ RESERVED_WEIGHT,          &Schema::process_weight          },
	{ RESERVED_POSITION,        &Schema::process_position        },
	{ RESERVED_SPELLING,        &Schema::process_spelling        },
	{ RESERVED_POSITIONS,       &Schema::process_positions       },
	{ RESERVED_TYPE,            &Schema::process_type            },
	{ RESERVED_PREFIX,          &Schema::process_prefix          },
	{ RESERVED_SLOT,            &Schema::process_slot            },
	{ RESERVED_INDEX,           &Schema::process_index           },
	{ RESERVED_STORE,           &Schema::process_store           },
	{ RESERVED_DYNAMIC,         &Schema::process_dynamic         },
	{ RESERVED_D_DETECTION,     &Schema::process_d_detection     },
	{ RESERVED_N_DETECTION,     &Schema::process_n_detection     },
	{ RESERVED_G_DETECTION,     &Schema::process_g_detection     },
	{ RESERVED_B_DETECTION,     &Schema::process_b_detection     },
	{ RESERVED_S_DETECTION,     &Schema::process_s_detection     },
	{ RESERVED_T_DETECTION,     &Schema::process_t_detection     },
	{ RESERVED_U_DETECTION,     &Schema::process_u_detection     },
	{ RESERVED_BOOL_TERM,       &Schema::process_bool_term       },
	{ RESERVED_VALUE,           &Schema::process_value           },
	{ RESERVED_NAME,            &Schema::process_name            },
	{ RESERVED_ACCURACY,        &Schema::process_accuracy        },
	{ RESERVED_ACC_PREFIX,      &Schema::process_acc_prefix      },
	{ RESERVED_STEM_STRATEGY,   &Schema::process_stem_strategy   },
	{ RESERVED_STEM_LANGUAGE,   &Schema::process_stem_language   },
	{ RESERVED_LANGUAGE,        &Schema::process_language        },
	{ RESERVED_PARTIALS,        &Schema::process_partials        },
	{ RESERVED_ERROR,           &Schema::process_error           },
	{ RESERVED_LATITUDE,        &Schema::process_latitude        },
	{ RESERVED_LONGITUDE,       &Schema::process_longitude       },
	{ RESERVED_RADIUS,          &Schema::process_radius          },
	{ RESERVED_DATE,            &Schema::process_date            },
	{ RESERVED_TIME,            &Schema::process_time            },
	{ RESERVED_YEAR,            &Schema::process_year            },
	{ RESERVED_MONTH,           &Schema::process_month           },
	{ RESERVED_DAY,             &Schema::process_day             },
	{ RESERVED_SCRIPT,          &Schema::process_script          }
});


const std::unordered_map<std::string, dispatch_reserved> map_dispatch_properties({
	{ RESERVED_WEIGHT,          &Schema::update_weight           },
	{ RESERVED_POSITION,        &Schema::update_position         },
	{ RESERVED_SPELLING,        &Schema::update_spelling         },
	{ RESERVED_POSITIONS,       &Schema::update_positions        },
	{ RESERVED_TYPE,            &Schema::update_type             },
	{ RESERVED_PREFIX,          &Schema::update_prefix           },
	{ RESERVED_SLOT,            &Schema::update_slot             },
	{ RESERVED_INDEX,           &Schema::update_index            },
	{ RESERVED_STORE,           &Schema::update_store            },
	{ RESERVED_DYNAMIC,         &Schema::update_dynamic          },
	{ RESERVED_D_DETECTION,     &Schema::update_d_detection      },
	{ RESERVED_N_DETECTION,     &Schema::update_n_detection      },
	{ RESERVED_G_DETECTION,     &Schema::update_g_detection      },
	{ RESERVED_B_DETECTION,     &Schema::update_b_detection      },
	{ RESERVED_S_DETECTION,     &Schema::update_s_detection      },
	{ RESERVED_T_DETECTION,     &Schema::update_t_detection      },
	{ RESERVED_U_DETECTION,     &Schema::update_u_detection      },
	{ RESERVED_BOOL_TERM,       &Schema::update_bool_term        },
	{ RESERVED_ACCURACY,        &Schema::update_accuracy         },
	{ RESERVED_ACC_PREFIX,      &Schema::update_acc_prefix       },
	{ RESERVED_STEM_STRATEGY,   &Schema::update_stem_strategy    },
	{ RESERVED_STEM_LANGUAGE,   &Schema::update_stem_language    },
	{ RESERVED_LANGUAGE,        &Schema::update_language         },
	{ RESERVED_PARTIALS,        &Schema::update_partials         },
	{ RESERVED_ERROR,           &Schema::update_error            }
});


const std::unordered_map<std::string, dispatch_root> map_dispatch_root({
	{ RESERVED_DATA,           &Schema::process_data             },
	{ RESERVED_VALUES,         &Schema::process_values           },
	{ RESERVED_FIELD_VALUES,   &Schema::process_field_values     },
	{ RESERVED_GLOBAL_VALUES,  &Schema::process_global_values    },
	{ RESERVED_TERMS,          &Schema::process_terms            },
	{ RESERVED_FIELD_TERMS,    &Schema::process_field_terms      },
	{ RESERVED_GLOBAL_TERMS,   &Schema::process_global_terms     },
	{ RESERVED_FIELD_ALL,      &Schema::process_field_all        },
	{ RESERVED_GLOBAL_ALL,     &Schema::process_global_all       },
	{ RESERVED_NONE,           &Schema::process_none             }
});


const std::unordered_map<std::string, dispatch_readable> map_dispatch_readable({
	{ RESERVED_TYPE,            &Schema::readable_type           },
	{ RESERVED_PREFIX,          &Schema::readable_prefix         },
	{ RESERVED_STEM_STRATEGY,   &Schema::readable_stem_strategy  },
	{ RESERVED_INDEX,           &Schema::readable_index          },
	{ RESERVED_ACC_PREFIX,      &Schema::readable_acc_prefix     }
});


// LRU of scripts.
ScriptLRU script_lru;


const std::unordered_map<std::string, std::pair<bool, std::string>> map_stem_language({
	{ "armenian",    { true,  "hy" } },  { "hy",               { true,  "hy" } },  { "basque",          { true,  "ue" } },
	{ "eu",          { true,  "eu" } },  { "catalan",          { true,  "ca" } },  { "ca",              { true,  "ca" } },
	{ "danish",      { true,  "da" } },  { "da",               { true,  "da" } },  { "dutch",           { true,  "nl" } },
	{ "nl",          { true,  "nl" } },  { "kraaij_pohlmann",  { false, "nl" } },  { "english",         { true,  "en" } },
	{ "en",          { true,  "en" } },  { "earlyenglish",     { false, "en" } },  { "english_lovins",  { false, "en" } },
	{ "lovins",      { false, "en" } },  { "english_porter",   { false, "en" } },  { "porter",          { false, "en" } },
	{ "finnish",     { true,  "fi" } },  { "fi",               { true,  "fi" } },  { "french",          { true,  "fr" } },
	{ "fr",          { true,  "fr" } },  { "german",           { true,  "de" } },  { "de",              { true,  "de" } },
	{ "german2",     { false, "de" } },  { "hungarian",        { true,  "hu" } },  { "hu",              { true,  "hu" } },
	{ "italian",     { true,  "it" } },  { "it",               { true,  "it" } },  { "norwegian",       { true,  "no" } },
	{ "nb",          { false, "no" } },  { "nn",               { false, "no" } },  { "no",              { true,  "no" } },
	{ "portuguese",  { true,  "pt" } },  { "pt",               { true,  "pt" } },  { "romanian",        { true,  "ro" } },
	{ "ro",          { true,  "ro" } },  { "russian",          { true,  "ru" } },  { "ru",              { true,  "ru" } },
	{ "spanish",     { true,  "es" } },  { "es",               { true,  "es" } },  { "swedish",         { true,  "sv" } },
	{ "sv",          { true,  "sv" } },  { "turkish",          { true,  "tr" } },  { "tr",              { true,  "tr" } },
	{ "none",        { false, DEFAULT_LANGUAGE } }
});


required_spc_t::required_spc_t()
	: sep_types({{ FieldType::EMPTY, FieldType::EMPTY, FieldType::EMPTY }}),
	  slot(Xapian::BAD_VALUENO),
	  bool_term(false),
	  stem_strategy(StemStrategy::STEM_SOME),
	  stem_language(DEFAULT_LANGUAGE),
	  language(DEFAULT_LANGUAGE),
	  partials(DEFAULT_GEO_PARTIALS),
	  error(DEFAULT_GEO_ERROR) { }


required_spc_t::required_spc_t(Xapian::valueno _slot, FieldType type, const std::vector<uint64_t>& acc,
	const std::vector<std::string>& _acc_prefix)
	: sep_types({{ FieldType::EMPTY, FieldType::EMPTY, type }}),
	  slot(_slot),
	  bool_term(false),
	  accuracy(acc),
	  acc_prefix(_acc_prefix),
	  stem_strategy(StemStrategy::STEM_SOME),
	  stem_language(DEFAULT_LANGUAGE),
	  language(DEFAULT_LANGUAGE),
	  partials(DEFAULT_GEO_PARTIALS),
	  error(DEFAULT_GEO_ERROR) { }


required_spc_t::required_spc_t(const required_spc_t& o)
	: sep_types(o.sep_types),
	  prefix(o.prefix),
	  slot(o.slot),
	  bool_term(o.bool_term),
	  accuracy(o.accuracy),
	  acc_prefix(o.acc_prefix),
	  stem_strategy(o.stem_strategy),
	  stem_language(o.stem_language),
	  language(o.language),
	  partials(o.partials),
	  error(o.error) { }


required_spc_t::required_spc_t(required_spc_t&& o) noexcept
	: sep_types(std::move(o.sep_types)),
	  prefix(std::move(o.prefix)),
	  slot(std::move(o.slot)),
	  bool_term(std::move(o.bool_term)),
	  accuracy(std::move(o.accuracy)),
	  acc_prefix(std::move(o.acc_prefix)),
	  stem_strategy(std::move(o.stem_strategy)),
	  stem_language(std::move(o.stem_language)),
	  language(std::move(o.language)),
	  partials(std::move(o.partials)),
	  error(std::move(o.error)) { }


specification_t::specification_t()
	: position({ 0 }),
	  weight({ 1 }),
	  spelling({ false }),
	  positions({ false }),
	  index(TypeIndex::ALL),
	  store(true),
	  parent_store(true),
	  dynamic(true),
	  date_detection(true),
	  numeric_detection(true),
	  geo_detection(true),
	  bool_detection(true),
	  string_detection(true),
	  text_detection(true),
	  uuid_detection(true),
	  found_field(true),
	  set_type(false),
	  set_bool_term(false),
	  dynamic_type(DynamicFieldType::NONE) { }


specification_t::specification_t(Xapian::valueno _slot, FieldType type, const std::vector<uint64_t>& acc,
	const std::vector<std::string>& _acc_prefix)
	: required_spc_t(_slot, type, acc, _acc_prefix),
	  position({ 0 }),
	  weight({ 1 }),
	  spelling({ false }),
	  positions({ false }),
	  index(TypeIndex::ALL),
	  store(true),
	  parent_store(true),
	  dynamic(true),
	  date_detection(true),
	  numeric_detection(true),
	  geo_detection(true),
	  bool_detection(true),
	  string_detection(true),
	  text_detection(true),
	  uuid_detection(true),
	  found_field(true),
	  set_type(false),
	  set_bool_term(false),
	  dynamic_type(DynamicFieldType::NONE) { }


specification_t::specification_t(const specification_t& o)
	: position(o.position),
	  weight(o.weight),
	  spelling(o.spelling),
	  positions(o.positions),
	  index(o.index),
	  store(o.store),
	  parent_store(o.parent_store),
	  dynamic(o.dynamic),
	  date_detection(o.date_detection),
	  numeric_detection(o.numeric_detection),
	  geo_detection(o.geo_detection),
	  bool_detection(o.bool_detection),
	  string_detection(o.string_detection),
	  text_detection(o.text_detection),
	  uuid_detection(o.uuid_detection),
	  script(o.script),
	  name(o.name),
	  full_name(o.full_name),
	  found_field(o.found_field),
	  set_type(o.set_type),
	  set_bool_term(o.set_bool_term),
	  aux_stem_lan(o.aux_stem_lan),
	  aux_lan(o.aux_lan),
	  dynamic_type(o.dynamic_type),
	  dynamic_prefix(o.dynamic_prefix),
	  dynamic_name(o.dynamic_name),
	  dynamic_full_name(o.dynamic_full_name) { }


specification_t::specification_t(specification_t&& o) noexcept
	: position(std::move(o.position)),
	  weight(std::move(o.weight)),
	  spelling(std::move(o.spelling)),
	  positions(std::move(o.positions)),
	  index(std::move(o.index)),
	  store(std::move(o.store)),
	  parent_store(std::move(o.parent_store)),
	  dynamic(std::move(o.dynamic)),
	  date_detection(std::move(o.date_detection)),
	  numeric_detection(std::move(o.numeric_detection)),
	  geo_detection(std::move(o.geo_detection)),
	  bool_detection(std::move(o.bool_detection)),
	  string_detection(std::move(o.string_detection)),
	  text_detection(std::move(o.text_detection)),
	  uuid_detection(std::move(o.uuid_detection)),
	  script(std::move(o.script)),
	  name(std::move(o.name)),
	  full_name(std::move(o.full_name)),
	  found_field(std::move(o.found_field)),
	  set_type(std::move(o.set_type)),
	  set_bool_term(std::move(o.set_bool_term)),
	  aux_stem_lan(std::move(o.aux_stem_lan)),
	  aux_lan(std::move(o.aux_lan)),
	  dynamic_type(std::move(o.dynamic_type)),
	  dynamic_prefix(std::move(o.dynamic_prefix)),
	  dynamic_name(std::move(o.dynamic_name)),
	  dynamic_full_name(std::move(o.dynamic_full_name)) { }


specification_t&
specification_t::operator=(const specification_t& o)
{
	position = o.position;
	weight = o.weight;
	spelling = o.spelling;
	positions = o.positions;
	sep_types = o.sep_types;
	prefix = o.prefix;
	slot = o.slot;
	index = o.index;
	store = o.store;
	parent_store = o.parent_store;
	dynamic = o.dynamic;
	date_detection = o.date_detection;
	numeric_detection = o.numeric_detection;
	geo_detection = o.geo_detection;
	bool_detection = o.bool_detection;
	string_detection = o.string_detection;
	text_detection = o.text_detection;
	uuid_detection = o.uuid_detection;
	bool_term = o.bool_term;
	value.reset();
	value_rec.reset();
	doc_acc.reset();
	script = o.script;
	name = o.name;
	full_name = o.full_name;
	accuracy = o.accuracy;
	acc_prefix = o.acc_prefix;
	stem_strategy = o.stem_strategy;
	stem_language = o.stem_language;
	language = o.language;
	partials = o.partials;
	error = o.error;
	found_field = o.found_field;
	set_type = o.set_type;
	set_bool_term = o.set_bool_term;
	aux_stem_lan = o.aux_stem_lan;
	aux_lan = o.aux_lan;
	dynamic_type = o.dynamic_type;
	dynamic_prefix = o.dynamic_prefix;
	dynamic_name = o.dynamic_name;
	dynamic_full_name = o.dynamic_full_name;
	return *this;
}


specification_t&
specification_t::operator=(specification_t&& o) noexcept
{
	position = std::move(o.position);
	weight = std::move(o.weight);
	spelling = std::move(o.spelling);
	positions = std::move(o.positions);
	sep_types = std::move(o.sep_types);
	prefix = std::move(o.prefix);
	slot = std::move(o.slot);
	index = std::move(o.index);
	store = std::move(o.store);
	parent_store = std::move(o.parent_store);
	dynamic = std::move(o.dynamic);
	date_detection = std::move(o.date_detection);
	numeric_detection = std::move(o.numeric_detection);
	geo_detection = std::move(o.geo_detection);
	bool_detection = std::move(o.bool_detection);
	string_detection = std::move(o.string_detection);
	text_detection = std::move(o.text_detection);
	uuid_detection = std::move(o.uuid_detection);
	bool_term = std::move(o.bool_term);
	value.reset();
	value_rec.reset();
	doc_acc.reset();
	script = std::move(o.script);
	name = std::move(o.name);
	full_name = std::move(o.full_name);
	accuracy = std::move(o.accuracy);
	acc_prefix = std::move(o.acc_prefix);
	stem_strategy = std::move(o.stem_strategy);
	stem_language = std::move(o.stem_language);
	language = std::move(o.language);
	partials = std::move(o.partials);
	error = std::move(o.error);
	found_field = std::move(o.found_field);
	set_type = std::move(o.set_type);
	set_bool_term = std::move(o.set_bool_term);
	aux_stem_lan = std::move(o.aux_stem_lan);
	aux_lan = std::move(o.aux_lan);
	dynamic_type = std::move(o.dynamic_type);
	dynamic_prefix = std::move(o.dynamic_prefix);
	dynamic_name = std::move(o.dynamic_name);
	dynamic_full_name = std::move(o.dynamic_full_name);
	return *this;
}


const specification_t&
specification_t::get_global(FieldType field_type)
{
	switch (field_type) {
		case FieldType::FLOAT: {
			static const specification_t spc(DB_SLOT_NUMERIC, FieldType::FLOAT, def_accuracy_num, global_acc_prefix_num);
			return spc;
		}
		case FieldType::INTEGER: {
			static const specification_t spc(DB_SLOT_NUMERIC, FieldType::INTEGER, def_accuracy_num, global_acc_prefix_num);
			return spc;
		}
		case FieldType::POSITIVE: {
			static const specification_t spc(DB_SLOT_NUMERIC, FieldType::POSITIVE, def_accuracy_num, global_acc_prefix_num);
			return spc;
		}
		case FieldType::STRING: {
			static const specification_t spc(DB_SLOT_STRING, FieldType::TEXT, default_spc.accuracy, default_spc.acc_prefix);
			return spc;
		}
		case FieldType::TEXT: {
			static const specification_t spc(DB_SLOT_STRING, FieldType::TEXT, default_spc.accuracy, default_spc.acc_prefix);
			return spc;
		}
		case FieldType::BOOLEAN: {
			static const specification_t spc(DB_SLOT_BOOLEAN, FieldType::BOOLEAN, default_spc.accuracy, default_spc.acc_prefix);
			return spc;
		}
		case FieldType::DATE: {
			static const specification_t spc(DB_SLOT_DATE, FieldType::DATE, def_accuracy_date, global_acc_prefix_date);
			return spc;
		}
		case FieldType::GEO: {
			static const specification_t spc(DB_SLOT_GEO, FieldType::GEO, def_accuracy_geo, global_acc_prefix_geo);
			return spc;
		}
		case FieldType::UUID: {
			static const specification_t spc(DB_SLOT_UUID, FieldType::UUID, default_spc.accuracy, default_spc.acc_prefix);
			return spc;
		}
		default:
			throw MSG_ClientError("Type: '%u' is an unknown type", field_type);
	}
}


std::string
specification_t::to_string() const
{
	std::stringstream str;
	str << "\n{\n";
	str << "\t" << RESERVED_NAME << ": " << full_name << "\n";
	str << "\t" << RESERVED_POSITION << ": [ ";
	for (const auto& _position : position) {
		str << _position << " ";
	}
	str << "]\n";

	str << "\t" << RESERVED_WEIGHT   << ": [ ";
	for (const auto& _w : weight) {
		str << _w << " ";
	}
	str << "]\n";

	str << "\t" << RESERVED_SPELLING << ": [ ";
	for (const auto& spel : spelling) {
		str << (spel ? "true" : "false") << " ";
	}
	str << "]\n";

	str << "\t" << RESERVED_POSITIONS<< ": [ ";
	for (const auto& _positions : positions) {
		str << (_positions ? "true" : "false") << " ";
	}
	str << "]\n";

	str << "\t" << RESERVED_STEM_STRATEGY    << ": " << readable_stem_strategy(stem_strategy) << "\n";
	str << "\t" << RESERVED_STEM_LANGUAGE    << ": " << stem_language     << "\n";
	str << "\t" << RESERVED_LANGUAGE         << ": " << language          << "\n";

	str << "\t" << RESERVED_ACCURACY << ": [ ";
	for (const auto& acc : accuracy) {
		str << acc << " ";
	}
	str << "]\n";

	str << "\t" << RESERVED_ACC_PREFIX  << ": [ ";
	for (const auto& acc_p : acc_prefix) {
		str << acc_p << " ";
	}
	str << "]\n";

	str << "\t" << RESERVED_VALUE     << ": " << (value ? value->to_string() : std::string())                 << "\n";
	str << "\t" << "Recovery value"   << ": " << (value_rec ? value_rec->to_string().c_str() : std::string())  << "\n";

	str << "\t" << RESERVED_SLOT        << ": " << slot                           << "\n";
	str << "\t" << RESERVED_TYPE        << ": " << readable_type(sep_types)       << "\n";
	str << "\t" << RESERVED_PREFIX      << ": " << prefix                         << "\n";
	str << "\t" << RESERVED_INDEX       << ": " << readable_index(index)          << "\n";
	str << "\t" << RESERVED_STORE       << ": " << (store             ? "true" : "false") << "\n";
	str << "\t" << RESERVED_DYNAMIC     << ": " << (dynamic           ? "true" : "false") << "\n";
	str << "\t" << RESERVED_D_DETECTION << ": " << (date_detection    ? "true" : "false") << "\n";
	str << "\t" << RESERVED_N_DETECTION << ": " << (numeric_detection ? "true" : "false") << "\n";
	str << "\t" << RESERVED_G_DETECTION << ": " << (geo_detection     ? "true" : "false") << "\n";
	str << "\t" << RESERVED_B_DETECTION << ": " << (bool_detection    ? "true" : "false") << "\n";
	str << "\t" << RESERVED_S_DETECTION << ": " << (string_detection  ? "true" : "false") << "\n";
	str << "\t" << RESERVED_T_DETECTION << ": " << (text_detection    ? "true" : "false") << "\n";
	str << "\t" << RESERVED_U_DETECTION << ": " << (uuid_detection    ? "true" : "false") << "\n";
	str << "\t" << RESERVED_BOOL_TERM   << ": " << (bool_term         ? "true" : "false") << "\n}\n";

	return str.str();
}


Schema::Schema(const std::shared_ptr<const MsgPack>& other)
	: schema(other)
{
	if (schema->is_undefined()) {
		MsgPack new_schema = {
			{ RESERVED_VERSION, DB_VERSION_SCHEMA },
			{ RESERVED_SCHEMA, MsgPack() },
		};
		new_schema.lock();
		schema = std::make_shared<const MsgPack>(std::move(new_schema));
	} else {
		try {
			const auto& version = schema->at(RESERVED_VERSION);
			if (version.as_f64() != DB_VERSION_SCHEMA) {
				throw MSG_Error("Different database's version schemas, the current version is %1.1f", DB_VERSION_SCHEMA);
			}
		} catch (const std::out_of_range&) {
			throw MSG_Error("Schema is corrupt, you need provide a new one");
		} catch (const msgpack::type_error&) {
			throw MSG_Error("Schema is corrupt, you need provide a new one");
		}
	}
}


MsgPack&
Schema::get_mutable(const std::string& full_name)
{
	L_CALL(this, "Schema::get_mutable()");

	if (!mut_schema) {
		mut_schema = std::make_unique<MsgPack>(*schema);
	}

	MsgPack* prop = &mut_schema->at(RESERVED_SCHEMA);
	std::vector<std::string> field_names;
	stringTokenizer(full_name, DB_OFFSPRING_UNION, field_names);
	for (const auto& field_name : field_names) {
		prop = &(*prop)[field_name];
	}
	return *prop;
}


std::string
Schema::serialise_id(const MsgPack& properties, const std::string& value_id)
{
	L_CALL(this, "Schema::serialise_id()");

	specification.set_type = true;
	try {
		auto& prop_id = properties.at(RESERVED_ID_FIELD);
		update_specification(prop_id);
		return Serialise::serialise(specification, value_id);
	} catch (const std::out_of_range&) {
		auto& prop_id = get_mutable(RESERVED_ID_FIELD);
		specification.found_field = false;
		auto res_serialise = Serialise::get_type(value_id, specification.bool_term);
		prop_id[RESERVED_TYPE] = std::array<FieldType, 3>({{ FieldType::EMPTY, FieldType::EMPTY, res_serialise.first }});
		prop_id[RESERVED_PREFIX] = DOCUMENT_ID_TERM_PREFIX;
		prop_id[RESERVED_SLOT] = DB_SLOT_ID;
		prop_id[RESERVED_INDEX] = TypeIndex::ALL;
		if (res_serialise.first == FieldType::STRING) {
			prop_id[RESERVED_BOOL_TERM] = true;
			prop_id[RESERVED_LANGUAGE] = DEFAULT_LANGUAGE;
		}
		return res_serialise.second;
	}
}


void
Schema::update_specification(const MsgPack& properties)
{
	L_CALL(this, "Schema::update_specification()");

	for (const auto& property : properties) {
		auto str_prop = property.as_string();
		try {
			auto func = map_dispatch_properties.at(str_prop);
			(this->*func)(properties.at(str_prop));
		} catch (const std::out_of_range&) { }
	}
}


void
Schema::restart_specification()
{
	L_CALL(this, "Schema::restart_specification()");

	specification.sep_types          = default_spc.sep_types;
	specification.prefix             = default_spc.prefix;
	specification.slot               = default_spc.slot;
	specification.accuracy           = default_spc.accuracy;
	specification.acc_prefix         = default_spc.acc_prefix;
	specification.bool_term          = default_spc.bool_term;
	specification.name               = default_spc.name;
	specification.stem_strategy      = default_spc.stem_strategy;
	specification.stem_language      = default_spc.stem_language;
	specification.language           = default_spc.language;
	specification.partials           = default_spc.partials;
	specification.error              = default_spc.error;
	specification.set_type           = default_spc.set_type;
	specification.aux_stem_lan       = default_spc.aux_stem_lan;
	specification.aux_lan            = default_spc.aux_lan;
	specification.dynamic_type       = default_spc.dynamic_type;
}


void
Schema::normalize_field(const std::string& field_name)
{
	L_CALL(this, "Schema::normalize_field()");

	if (Serialise::isUUID(field_name)) {
		specification.dynamic_prefix.append(lower_string(field_name));
		specification.dynamic_name.assign(RESERVED_UUID_FIELD);
		specification.dynamic_type = DynamicFieldType::UUID;
		specification.index = TypeIndex::TERMS;
		return;
	}

	try {
		specification.dynamic_prefix.assign(Datetime::normalizeISO8601(field_name));
		specification.dynamic_name.assign(RESERVED_DATE_FIELD);
		specification.dynamic_type = DynamicFieldType::DATE;
		specification.index = TypeIndex::TERMS;
		return;
	} catch (const DatetimeError&) { }

	try {
		specification.dynamic_prefix.assign(Serialise::ewkt(field_name, DEFAULT_GEO_PARTIALS, DEFAULT_GEO_ERROR));
		specification.dynamic_name.assign(RESERVED_GEO_FIELD);
		specification.dynamic_type = DynamicFieldType::GEO;
		specification.index = TypeIndex::TERMS;
		return;
	} catch (const EWKTError&) { }

	specification.dynamic_prefix.assign(field_name);
	specification.dynamic_name.assign(field_name);
	specification.dynamic_type = DynamicFieldType::NONE;
	return;
}


void
Schema::add_field(MsgPack* properties, const std::string& field_name)
{
	L_CALL(this, "Schema::add_field()");

	properties = &(*properties)[field_name];
	if (specification.dynamic_type == DynamicFieldType::NONE) {
		try {
			auto data_lan = map_stem_language.at(field_name);
			if (data_lan.first) {
				specification.language = data_lan.second;
			}
		} catch (const std::out_of_range) { }
		if (specification.full_name.empty()) {
			specification.full_name.assign(field_name);
			specification.dynamic_full_name.assign(field_name);
		} else {
			specification.full_name.append(DB_OFFSPRING_UNION).append(field_name);
			specification.dynamic_full_name.append(DB_OFFSPRING_UNION).append(field_name);
		}
	} else {
		if (specification.full_name.empty()) {
			specification.full_name.assign(field_name);
			specification.dynamic_full_name.assign(specification.dynamic_prefix);
		} else {
			specification.full_name.append(DB_OFFSPRING_UNION).append(field_name);
			specification.dynamic_full_name.append(DB_OFFSPRING_UNION).append(specification.dynamic_prefix);
		}
	}
}


void
Schema::get_subproperties(const MsgPack* properties, const std::string& field_name)
{
	L_CALL(this, "Schema::get_subproperties(1)");

	properties = &properties->at(field_name);
	specification.found_field = true;
	try {
		auto data_lan = map_stem_language.at(field_name);
		if (data_lan.first) {
			specification.language.assign(data_lan.second);
		}
	} catch (const std::out_of_range) { }
	update_specification(*properties);
	if (specification.full_name.empty()) {
		specification.full_name.assign(field_name);
		specification.dynamic_full_name.assign(field_name);
	} else {
		specification.full_name.append(DB_OFFSPRING_UNION).append(field_name);
		specification.dynamic_full_name.append(DB_OFFSPRING_UNION).append(field_name);
	}
}


const MsgPack&
Schema::get_subproperties(const MsgPack& properties)
{
	L_CALL(this, "Schema::get_subproperties(2)");

	std::vector<std::string> field_names;
	stringTokenizer(specification.name, DB_OFFSPRING_UNION, field_names);

	const MsgPack* subproperties = &properties;
	const auto it_e = field_names.end();
	for (auto it = field_names.begin(); it != it_e; ++it) {
		const auto& field_name = *it;
		if (!is_valid(field_name)) {
			throw MSG_ClientError("The field name: %s (%s) is not valid", specification.name.c_str(), field_name.c_str());
		}
		restart_specification();
		try {
			get_subproperties(subproperties, field_name);
		} catch (const std::out_of_range&) {
			normalize_field(field_name);
			if (specification.dynamic_type != DynamicFieldType::NONE) {
				try {
					get_subproperties(subproperties, specification.dynamic_name);
					continue;
				} catch (const std::out_of_range&) { }
			}

			MsgPack* mut_subprop = &get_mutable(specification.full_name);
			specification.found_field = false;
			add_field(mut_subprop, specification.dynamic_name);
			for (++it; it != it_e; ++it) {
				normalize_field(*it);
				add_field(mut_subprop, specification.dynamic_name);
			}
			return *mut_subprop;
		}
	}

	return *subproperties;
}


std::tuple<std::string, DynamicFieldType, const MsgPack&>
Schema::get_subproperties(const MsgPack& properties, const std::string& full_name) const
{
	L_CALL(this, "Schema::get_subproperties(3)");

	std::vector<std::string> field_names;
	stringTokenizer(full_name, DB_OFFSPRING_UNION, field_names);

	const MsgPack* subproperties = &properties;
	std::string dynamic_full_name;
	dynamic_full_name.reserve(full_name.length());
	DynamicFieldType type;
	bool root = true;
	for (const auto& field_name : field_names) {
		if (!is_valid(field_name) && !root && field_name != RESERVED_ID_FIELD) {
			throw MSG_ClientError("The field name: %s (%s) is not valid", specification.name.c_str(), field_name.c_str());
		}
		root = false;
		try {
			subproperties = &subproperties->at(field_name);
			type = DynamicFieldType::NONE;
			if (dynamic_full_name.empty()) {
				dynamic_full_name.assign(field_name);
			} else {
				dynamic_full_name.append(DB_OFFSPRING_UNION).append(field_name);
			}
		} catch (const std::out_of_range&) {
			if (Serialise::isUUID(field_name)) {
				subproperties = &subproperties->at(RESERVED_UUID_FIELD);
				type = DynamicFieldType::UUID;
				if (dynamic_full_name.empty()) {
					dynamic_full_name.assign(lower_string(field_name));
				} else {
					dynamic_full_name.append(DB_OFFSPRING_UNION).append(lower_string(field_name));
				}
				continue;
			}

			try {
				auto dynamic_name = Datetime::normalizeISO8601(field_name);
				subproperties = &subproperties->at(RESERVED_DATE_FIELD);
				type = DynamicFieldType::DATE;
				if (dynamic_full_name.empty()) {
					dynamic_full_name.assign(dynamic_name);
				} else {
					dynamic_full_name.append(DB_OFFSPRING_UNION).append(dynamic_name);
				}
				continue;
			} catch (const DatetimeError&) { }

			try {
				auto dynamic_name = Serialise::ewkt(field_name, DEFAULT_GEO_PARTIALS, DEFAULT_GEO_ERROR);
				subproperties = &subproperties->at(RESERVED_GEO_FIELD);
				type = DynamicFieldType::GEO;
				if (dynamic_full_name.empty()) {
					dynamic_full_name.assign(dynamic_name);
				} else {
					dynamic_full_name.append(DB_OFFSPRING_UNION).append(dynamic_name);
				}
				continue;
			} catch (const EWKTError&) { }

			throw MSG_ClientError("%s does not exist in schema", field_name.c_str());
		}
	}

	return std::forward_as_tuple(std::move(dynamic_full_name), type, *subproperties);
}


void
Schema::set_type(const MsgPack& item_doc)
{
	L_CALL(this, "Schema::set_type()");

	const auto& field = item_doc.is_array() ? item_doc.at(0) : item_doc;
	switch (field.getType()) {
		case MsgPack::Type::POSITIVE_INTEGER:
			if (specification.numeric_detection) {
				specification.sep_types[2] = FieldType::POSITIVE;
				return;
			}
			break;
		case MsgPack::Type::NEGATIVE_INTEGER:
			if (specification.numeric_detection) {
				specification.sep_types[2] = FieldType::INTEGER;
				return;
			}
			break;
		case MsgPack::Type::FLOAT:
			if (specification.numeric_detection) {
				specification.sep_types[2] = FieldType::FLOAT;
				return;
			}
			break;
		case MsgPack::Type::BOOLEAN:
			if (specification.bool_detection) {
				specification.sep_types[2] = FieldType::BOOLEAN;
				return;
			}
			break;
		case MsgPack::Type::STR: {
			auto str_value = field.as_string();
			if (specification.date_detection && Datetime::isDate(str_value)) {
				specification.sep_types[2] = FieldType::DATE;
				return;
			}
			if (specification.geo_detection && EWKT_Parser::isEWKT(str_value)) {
				specification.sep_types[2] = FieldType::GEO;
				return;
			}
			if (specification.uuid_detection && Serialise::isUUID(str_value)) {
				specification.sep_types[2] = FieldType::UUID;
				return;
			}
			if (specification.text_detection && Serialise::isText(str_value, specification.bool_term)) {
				specification.sep_types[2] = FieldType::TEXT;
				return;
			}
			if (specification.string_detection) {
				specification.sep_types[2] = FieldType::STRING;
				return;
			}
			if (specification.bool_detection) {
				try {
					Serialise::boolean(str_value);
					specification.sep_types[2] = FieldType::BOOLEAN;
					return;
				} catch (const SerialisationError&) { }
			}
			break;
		}
		case MsgPack::Type::ARRAY:
			throw MSG_ClientError("%s can not be array of arrays", RESERVED_VALUE);
		case MsgPack::Type::MAP:
			throw MSG_ClientError("%s can not be object", RESERVED_VALUE);
		case MsgPack::Type::NIL:
			// Do not process this field.
			throw MSG_DummyException();
		default:
			break;
	}

	throw MSG_ClientError("%s: %s is ambiguous", RESERVED_VALUE, item_doc.to_string().c_str());
}


void
Schema::set_type_to_array()
{
	L_CALL(this, "Schema::set_type_to_array()");

	auto& _types = get_mutable(specification.full_name)[RESERVED_TYPE];
	if (_types.is_undefined()) {
		_types = MsgPack({ FieldType::EMPTY, FieldType::ARRAY, FieldType::EMPTY });
		specification.sep_types[1] = FieldType::ARRAY;
	} else {
		_types[1] = FieldType::ARRAY;
		specification.sep_types[1] = FieldType::ARRAY;
	}
}


void
Schema::set_type_to_object()
{
	L_CALL(this, "Schema::set_type_to_object()");

	auto& _types = get_mutable(specification.full_name)[RESERVED_TYPE];
	if (_types.is_undefined()) {
		_types = MsgPack({ FieldType::OBJECT, FieldType::EMPTY, FieldType::EMPTY });
		specification.sep_types[0] = FieldType::OBJECT;
	} else {
		_types[0] = FieldType::OBJECT;
		specification.sep_types[0] = FieldType::OBJECT;
	}
}


std::string
Schema::to_string(bool prettify) const
{
	L_CALL(this, "Schema::to_string()");

	return get_readable().to_string(prettify);
}


const MsgPack
Schema::get_readable() const
{
	L_CALL(this, "Schema::get_readable()");

	auto schema_readable = mut_schema ? *mut_schema : *schema;
	auto& properties = schema_readable.at(RESERVED_SCHEMA);
	if unlikely(properties.is_undefined()) {
		schema_readable.erase(RESERVED_SCHEMA);
	} else {
		readable(properties);
	}

	return schema_readable;
}


void
Schema::readable(MsgPack& item_schema)
{
	// Change this item of schema in readable form.
	for (const auto& item_key : item_schema) {
		auto str_key = item_key.as_string();
		try {
			auto func = map_dispatch_readable.at(str_key);
			(*func)(item_schema.at(str_key), item_schema);
		} catch (const std::out_of_range&) {
			if (is_valid(str_key) || reserved_field_names.find(str_key) != reserved_field_names.end()) {
				auto& sub_item = item_schema.at(str_key);
				if unlikely(sub_item.is_undefined()) {
					item_schema.erase(str_key);
				} else {
					readable(sub_item);
				}
			}
		}
	}
}


void
Schema::readable_type(MsgPack& prop_type, MsgPack& properties)
{
	std::array<FieldType, 3> sep_types({{
		(FieldType)prop_type.at(0).as_u64(),
		(FieldType)prop_type.at(1).as_u64(),
		(FieldType)prop_type.at(2).as_u64()
	}});
	prop_type = ::readable_type(sep_types);

	// Readable accuracy.
	if (sep_types[2] == FieldType::DATE) {
		for (auto& _accuracy : properties.at(RESERVED_ACCURACY)) {
			_accuracy = readable_acc_date((UnitTime)_accuracy.as_u64());
		}
	}
}


void
Schema::readable_prefix(MsgPack& prop_prefix, MsgPack&)
{
	prop_prefix = prop_prefix.as_string();
}


void
Schema::readable_stem_strategy(MsgPack& prop_stem_strategy, MsgPack&)
{
	prop_stem_strategy = ::readable_stem_strategy((StemStrategy)prop_stem_strategy.as_u64());
}


void
Schema::readable_index(MsgPack& prop_index, MsgPack&)
{
	prop_index = ::readable_index((TypeIndex)prop_index.as_u64());
}


void
Schema::readable_acc_prefix(MsgPack& prop_acc_prefix, MsgPack& properties)
{
	for (auto& prop_prefix : prop_acc_prefix) {
		readable_prefix(prop_prefix, properties);
	}
}


void
Schema::process_position(const MsgPack& doc_position)
{
	// RESERVED_POSITION is heritable and can change between documents.
	try {
		specification.position.clear();
		if (doc_position.is_array()) {
			if (doc_position.empty()) {
				throw MSG_ClientError("Data inconsistency, %s must be a positive integer or a not-empty array of positive integers", RESERVED_POSITION);
			}
			for (const auto& _position : doc_position) {
				specification.position.push_back(static_cast<unsigned>(_position.as_u64()));
			}
		} else {
			specification.position.push_back(static_cast<unsigned>(doc_position.as_u64()));
		}

		if unlikely(!specification.found_field) {
			get_mutable(specification.full_name)[RESERVED_POSITION] = specification.position;
		}
	} catch (const msgpack::type_error&) {
		throw MSG_ClientError("Data inconsistency, %s must be a positive integer or a not-empty array of positive integers", RESERVED_POSITION);
	}
}


void
Schema::process_weight(const MsgPack& doc_weight)
{
	// RESERVED_WEIGHT is heritable and can change between documents.
	try {
		specification.weight.clear();
		if (doc_weight.is_array()) {
			if (doc_weight.empty()) {
				throw MSG_ClientError("Data inconsistency, %s must be a positive integer or a not-empty array of positive integers", RESERVED_WEIGHT);
			}
			for (const auto& _weight : doc_weight) {
				specification.weight.push_back(static_cast<unsigned>(_weight.as_u64()));
			}
		} else {
			specification.weight.push_back(static_cast<unsigned>(doc_weight.as_u64()));
		}

		if unlikely(!specification.found_field) {
			get_mutable(specification.full_name)[RESERVED_WEIGHT] = specification.weight;
		}
	} catch (const msgpack::type_error&) {
		throw MSG_ClientError("Data inconsistency, %s must be a positive integer or a not-empty array of positive integers", RESERVED_WEIGHT);
	}
}


void
Schema::process_spelling(const MsgPack& doc_spelling)
{
	// RESERVED_SPELLING is heritable and can change between documents.
	try {
		specification.spelling.clear();
		if (doc_spelling.is_array()) {
			if (doc_spelling.empty()) {
				throw MSG_ClientError("Data inconsistency, %s must be a boolean or a not-empty array of booleans", RESERVED_SPELLING);
			}
			for (const auto& _spelling : doc_spelling) {
				specification.spelling.push_back(_spelling.as_bool());
			}
		} else {
			specification.spelling.push_back(doc_spelling.as_bool());
		}

		if unlikely(!specification.found_field) {
			get_mutable(specification.full_name)[RESERVED_SPELLING] = specification.spelling;
		}
	} catch (const msgpack::type_error&) {
		throw MSG_ClientError("Data inconsistency, %s must be a boolean or a not-empty array of booleans", RESERVED_SPELLING);
	}
}


void
Schema::process_positions(const MsgPack& doc_positions)
{
	// RESERVED_POSITIONS is heritable and can change between documents.
	try {
		specification.positions.clear();
		if (doc_positions.is_array()) {
			if (doc_positions.empty()) {
				throw MSG_ClientError("Data inconsistency, %s must be a boolean or a not-empty array of booleans", RESERVED_POSITIONS);
			}
			for (const auto& _positions : doc_positions) {
				specification.positions.push_back(_positions.as_bool());
			}
		} else {
			specification.positions.push_back(doc_positions.as_bool());
		}

		if unlikely(!specification.found_field) {
			get_mutable(specification.full_name)[RESERVED_POSITIONS] = specification.positions;
		}
	} catch (const msgpack::type_error&) {
		throw MSG_ClientError("Data inconsistency, %s must be a boolean or a not-empty array of booleans", RESERVED_POSITIONS);
	}
}


void
Schema::process_stem_strategy(const MsgPack& doc_stem_strategy)
{
	// RESERVED_STEM_STRATEGY isn't heritable and can't change once fixed.
	if likely(specification.set_type) {
		return;
	}

	try {
		auto _stem_strategy = lower_string(doc_stem_strategy.as_string());
		try {
			specification.stem_strategy = map_stem_strategy.at(_stem_strategy);
		} catch (const std::out_of_range&) {
			throw MSG_ClientError("%s can be in %s (%s not supported)", RESERVED_STEM_STRATEGY, str_set_stem_strategy.c_str(), _stem_strategy.c_str());
		}
	} catch (const msgpack::type_error&) {
		throw MSG_ClientError("Data inconsistency, %s must be string", RESERVED_STEM_STRATEGY);
	}
}


void
Schema::process_stem_language(const MsgPack& doc_stem_language)
{
	// RESERVED_STEM_LANGUAGE isn't heritable and can't change once fixed.
	if likely(specification.set_type) {
		return;
	}

	try {
		auto _stem_language = lower_string(doc_stem_language.as_string());
		try {
			auto data_lan = map_stem_language.at(_stem_language);
			specification.stem_language = _stem_language;
			specification.aux_stem_lan =  data_lan.second;
		} catch (const std::out_of_range&) {
			throw MSG_ClientError("%s: %s is not supported", RESERVED_STEM_LANGUAGE, _stem_language.c_str());
		}
	} catch (const msgpack::type_error&) {
		throw MSG_ClientError("Data inconsistency, %s must be string", RESERVED_STEM_LANGUAGE);
	}
}


void
Schema::process_language(const MsgPack& doc_language)
{
	// RESERVED_LANGUAGE isn't heritable and can't change once fixed.
	if likely(specification.set_type) {
		return;
	}

	try {
		auto _str_language = lower_string(doc_language.as_string());
		try {
			auto data_lan = map_stem_language.at(_str_language);
			if (data_lan.first) {
				specification.language = data_lan.second;
				specification.aux_lan = data_lan.second;
			} else {
				throw MSG_ClientError("%s: %s is not supported", RESERVED_LANGUAGE, _str_language.c_str());
			}
		} catch (const std::out_of_range&) {
			throw MSG_ClientError("%s: %s is not supported", RESERVED_LANGUAGE, _str_language.c_str());
		}
	} catch (const msgpack::type_error&) {
		throw MSG_ClientError("Data inconsistency, %s must be string", RESERVED_LANGUAGE);
	}
}


void
Schema::process_type(const MsgPack& doc_type)
{
	// RESERVED_TYPE isn't heritable and can't change once fixed.
	if likely(specification.set_type) {
		return;
	}

	try {
		auto str_type = lower_string(doc_type.as_string());
		if (str_type.empty()) {
			throw MSG_ClientError("%s must be in { object, array, [object/][array/]< %s > }", RESERVED_TYPE, str_set_type.c_str());
		}

		try {
			specification.sep_types[2] = map_type.at(str_type);
		} catch (const std::out_of_range&) {
			std::vector<std::string> tokens;
			stringTokenizer(str_type, "/", tokens);
			try {
				switch (tokens.size()) {
					case 1:
						if (tokens[0] == OBJECT_STR) {
							specification.sep_types[0] = FieldType::OBJECT;
							return;
						} else if (tokens[0] == ARRAY_STR) {
							specification.sep_types[1] = FieldType::ARRAY;
							return;
						}
						break;
					case 2:
						if (tokens[0] == OBJECT_STR) {
							specification.sep_types[0] = FieldType::OBJECT;
							if (tokens[1] == ARRAY_STR) {
								specification.sep_types[1] = FieldType::ARRAY;
								return;
							} else {
								specification.sep_types[2] = map_type.at(tokens[1]);
								return;
							}
						} else if (tokens[0] == ARRAY_STR) {
							specification.sep_types[1] = FieldType::ARRAY;
							specification.sep_types[2] = map_type.at(tokens[1]);
							return;
						}
						break;
					case 3:
						if (tokens[0] == OBJECT_STR && tokens[1] == ARRAY_STR) {
							specification.sep_types[0] = FieldType::OBJECT;
							specification.sep_types[1] = FieldType::ARRAY;
							specification.sep_types[2] = map_type.at(tokens[2]);
						}
						break;
				}
				throw MSG_ClientError("%s must be in { object, array, [object/][array/]< %s > }", RESERVED_TYPE, str_set_type.c_str());
			} catch (const std::out_of_range&) {
				throw MSG_ClientError("%s must be in { object, array, [object/][array/]< %s > }", RESERVED_TYPE, str_set_type.c_str());
			}
		}
	} catch (const msgpack::type_error&) {
		throw MSG_ClientError("Data inconsistency, %s must be string", RESERVED_TYPE);
	}
}


void
Schema::process_accuracy(const MsgPack& doc_accuracy)
{
	// RESERVED_ACCURACY isn't heritable and can't change once fixed.
	if likely(specification.set_type) {
		return;
	}

	if (doc_accuracy.is_array()) {
		try {
			specification.doc_acc = std::make_unique<const MsgPack>(doc_accuracy);
		} catch (const msgpack::type_error&) {
			throw MSG_ClientError("Data inconsistency, %s must be array", RESERVED_ACCURACY);
		}
	} else {
		throw MSG_ClientError("Data inconsistency, %s must be array", RESERVED_ACCURACY);
	}
}


void
Schema::process_acc_prefix(const MsgPack& doc_acc_prefix)
{
	// RESERVED_ACC_PREFIX isn't heritable and can't change once fixed.
	// It is taken into account only if RESERVED_ACCURACY is defined.
	if likely(specification.set_type) {
		return;
	}

	if (doc_acc_prefix.is_array()) {
		std::unordered_set<std::string> uset_acc_prefix;
		uset_acc_prefix.reserve(doc_acc_prefix.size());
		specification.acc_prefix.reserve(doc_acc_prefix.size());
		try {
			for (const auto& _acc_prefix : doc_acc_prefix) {
				auto prefix = _acc_prefix.as_string();
				if (uset_acc_prefix.insert(prefix).second) {
					specification.acc_prefix.push_back(std::move(prefix));
				}
			}
		} catch (const msgpack::type_error&) {
			throw MSG_ClientError("Data inconsistency, %s must be an array of strings", RESERVED_ACC_PREFIX);
		}
	} else {
		throw MSG_ClientError("Data inconsistency, %s must be an array of strings", RESERVED_ACC_PREFIX);
	}
}


void
Schema::process_prefix(const MsgPack& doc_prefix)
{
	// RESERVED_PREFIX isn't heritable and can't change once fixed.
	if likely(specification.set_type) {
		return;
	}

	try {
		specification.prefix = doc_prefix.as_string();
	} catch (const msgpack::type_error&) {
		throw MSG_ClientError("Data inconsistency, %s must be string", RESERVED_PREFIX);
	}
}


void
Schema::process_slot(const MsgPack& doc_slot)
{
	// RESERVED_SLOT isn't heritable and can't change once fixed.
	if likely(specification.set_type) {
		return;
	}

	try {
		specification.slot = static_cast<unsigned>(doc_slot.as_u64());
		if (specification.slot < DB_SLOT_RESERVED) {
			specification.slot += DB_SLOT_RESERVED;
		} else if (specification.slot == Xapian::BAD_VALUENO) {
			specification.slot = 0xfffffffe;
		}
	} catch (const msgpack::type_error&) {
		throw MSG_ClientError("Data inconsistency, %s must be a positive integer", RESERVED_SLOT);
	}
}


void
Schema::process_index(const MsgPack& doc_index)
{
	// RESERVED_INDEX is heritable and can change.
	try {
		auto str_index = lower_string(doc_index.as_string());
		try {
			specification.index = map_index.at(str_index);

			if unlikely(!specification.found_field) {
				get_mutable(specification.full_name)[RESERVED_INDEX] = specification.index;
			}
		} catch (const std::out_of_range&) {
			throw MSG_ClientError("%s must be in %s (%s not supported)", RESERVED_INDEX, str_set_index.c_str(), str_index.c_str());
		}
	} catch (const msgpack::type_error&) {
		throw MSG_ClientError("Data inconsistency, %s must be string", RESERVED_INDEX);
	}
}


void
Schema::process_store(const MsgPack& doc_store)
{
	/*
	 * RESERVED_STORE is heritable and can change, but once fixed in false
	 * it cannot change in its offsprings.
	 */
	try {
		auto val_store = doc_store.as_bool();
		specification.store = val_store && specification.parent_store;
		specification.parent_store = specification.store;

		if unlikely(!specification.found_field) {
			get_mutable(specification.full_name)[RESERVED_STORE] = val_store;
		}
	} catch (const msgpack::type_error&) {
		throw MSG_ClientError("Data inconsistency, %s must be boolean", RESERVED_STORE);
	} catch (const std::out_of_range&) { }
}


void
Schema::process_dynamic(const MsgPack& doc_dynamic)
{
	// RESERVED_DYNAMIC is heritable but can't change.
	if likely(specification.found_field) {
		return;
	}

	try {
		specification.dynamic = doc_dynamic.as_bool();
		get_mutable(specification.full_name)[RESERVED_DYNAMIC] = specification.dynamic;
	} catch (const msgpack::type_error&) {
		throw MSG_ClientError("Data inconsistency, %s must be boolean", RESERVED_DYNAMIC);
	} catch (const std::out_of_range&) { }
}


void
Schema::process_d_detection(const MsgPack& doc_d_detection)
{
	// RESERVED_D_DETECTION is heritable and can't change.
	if likely(specification.found_field) {
		return;
	}

	try {
		specification.date_detection = doc_d_detection.as_bool();
		get_mutable(specification.full_name)[RESERVED_D_DETECTION] = specification.date_detection;
	} catch (const msgpack::type_error&) {
		throw MSG_ClientError("Data inconsistency, %s must be boolean", RESERVED_D_DETECTION);
	}
}


void
Schema::process_n_detection(const MsgPack& doc_n_detection)
{
	// RESERVED_N_DETECTION is heritable and can't change.
	if likely(specification.found_field) {
		return;
	}

	try {
		specification.numeric_detection = doc_n_detection.as_bool();
		get_mutable(specification.full_name)[RESERVED_N_DETECTION] = specification.numeric_detection;
	} catch (const msgpack::type_error&) {
		throw MSG_ClientError("Data inconsistency, %s must be boolean", RESERVED_N_DETECTION);
	}
}


void
Schema::process_g_detection(const MsgPack& doc_g_detection)
{
	// RESERVED_G_DETECTION is heritable and can't change.
	if likely(specification.found_field) {
		return;
	}

	try {
		specification.geo_detection = doc_g_detection.as_bool();
		get_mutable(specification.full_name)[RESERVED_G_DETECTION] = specification.geo_detection;
	} catch (const msgpack::type_error&) {
		throw MSG_ClientError("Data inconsistency, %s must be boolean", RESERVED_G_DETECTION);
	}
}


void
Schema::process_b_detection(const MsgPack& doc_b_detection)
{
	// RESERVED_B_DETECTION is heritable and can't change.
	if likely(specification.found_field) {
		return;
	}

	try {
		specification.bool_detection = doc_b_detection.as_bool();
		get_mutable(specification.full_name)[RESERVED_B_DETECTION] = specification.bool_detection;
	} catch (const msgpack::type_error&) {
		throw MSG_ClientError("Data inconsistency, %s must be boolean", RESERVED_B_DETECTION);
	}
}


void
Schema::process_s_detection(const MsgPack& doc_s_detection)
{
	// RESERVED_S_DETECTION is heritable and can't change.
	if likely(specification.found_field) {
		return;
	}

	try {
		specification.string_detection = doc_s_detection.as_bool();
		get_mutable(specification.full_name)[RESERVED_S_DETECTION] = specification.string_detection;
	} catch (const msgpack::type_error&) {
		throw MSG_ClientError("Data inconsistency, %s must be boolean", RESERVED_S_DETECTION);
	}
}


void
Schema::process_t_detection(const MsgPack& doc_t_detection)
{
	// RESERVED_T_DETECTION is heritable and can't change.
	if likely(specification.found_field) {
		return;
	}

	try {
		specification.text_detection = doc_t_detection.as_bool();
		get_mutable(specification.full_name)[RESERVED_T_DETECTION] = specification.text_detection;
	} catch (const msgpack::type_error&) {
		throw MSG_ClientError("Data inconsistency, %s must be boolean", RESERVED_T_DETECTION);
	}
}


void
Schema::process_u_detection(const MsgPack& doc_u_detection)
{
	// RESERVED_U_DETECTION is heritable and can't change.
	if likely(specification.found_field) {
		return;
	}

	try {
		specification.uuid_detection = doc_u_detection.as_bool();
		get_mutable(specification.full_name)[RESERVED_U_DETECTION] = specification.uuid_detection;
	} catch (const msgpack::type_error&) {
		throw MSG_ClientError("Data inconsistency, %s must be boolean", RESERVED_U_DETECTION);
	}
}


void
Schema::process_bool_term(const MsgPack& doc_bool_term)
{
	// RESERVED_BOOL_TERM isn't heritable and can't change.
	if likely(specification.set_type) {
		return;
	}

	try {
		specification.bool_term = doc_bool_term.as_bool();
		specification.set_bool_term = true;
	} catch (const msgpack::type_error&) {
		throw MSG_ClientError("Data inconsistency, %s must be a boolean", RESERVED_BOOL_TERM);
	}
}


void
Schema::process_partials(const MsgPack& doc_partials)
{
	// RESERVED_PARTIALS isn't heritable and can't change once fixed.
	if likely(specification.set_type) {
		return;
	}

	try {
		specification.partials = doc_partials.as_bool();
	} catch (const msgpack::type_error&) {
		throw MSG_ClientError("Data inconsistency, %s must be boolean", RESERVED_PARTIALS);
	}
}


void
Schema::process_error(const MsgPack& doc_error)
{
	// RESERVED_PARTIALS isn't heritable and can't change once fixed.
	if likely(specification.set_type) {
		return;
	}

	try {
		specification.error = doc_error.as_f64();
	} catch (const msgpack::type_error&) {
		throw MSG_ClientError("Data inconsistency, %s must be a double", RESERVED_ERROR);
	}
}


void
Schema::process_latitude(const MsgPack& doc_latitude)
{
	// RESERVED_LATITUDE isn't heritable and is not saved in schema.
	if (!specification.value_rec) {
		specification.value_rec = std::make_unique<MsgPack>();
	}
	(*specification.value_rec)[RESERVED_LATITUDE] = doc_latitude;
}


void
Schema::process_longitude(const MsgPack& doc_longitude)
{
	// RESERVED_LONGITUDE isn't heritable and is not saved in schema.
	if (!specification.value_rec) {
		specification.value_rec = std::make_unique<MsgPack>();
	}
	(*specification.value_rec)[RESERVED_LONGITUDE] = doc_longitude;
}


void
Schema::process_radius(const MsgPack& doc_radius)
{
	// RESERVED_RADIUS isn't heritable and is not saved in schema.
	if (!specification.value_rec) {
		specification.value_rec = std::make_unique<MsgPack>();
	}
	(*specification.value_rec)[RESERVED_RADIUS] = doc_radius;
}


void
Schema::process_date(const MsgPack& doc_date)
{
	// RESERVED_DATE isn't heritable and is not saved in schema.
	if (!specification.value_rec) {
		specification.value_rec = std::make_unique<MsgPack>();
	}
	(*specification.value_rec)[RESERVED_DATE] = doc_date;
}


void
Schema::process_time(const MsgPack& doc_time)
{
	// RESERVED_TIME isn't heritable and is not saved in schema.
	if (!specification.value_rec) {
		specification.value_rec = std::make_unique<MsgPack>();
	}
	(*specification.value_rec)[RESERVED_TIME] = doc_time;
}


void
Schema::process_year(const MsgPack& doc_year)
{
	// RESERVED_YEAR isn't heritable and is not saved in schema.
	if (!specification.value_rec) {
		specification.value_rec = std::make_unique<MsgPack>();
	}
	(*specification.value_rec)[RESERVED_YEAR] = doc_year;
}


void
Schema::process_month(const MsgPack& doc_month)
{
	// RESERVED_MONTH isn't heritable and is not saved in schema.
	if (!specification.value_rec) {
		specification.value_rec = std::make_unique<MsgPack>();
	}
	(*specification.value_rec)[RESERVED_MONTH] = doc_month;
}


void
Schema::process_day(const MsgPack& doc_day)
{
	// RESERVED_DAY isn't heritable and is not saved in schema.
	if (!specification.value_rec) {
		specification.value_rec = std::make_unique<MsgPack>();
	}
	(*specification.value_rec)[RESERVED_DAY] = doc_day;
}


void
Schema::process_value(const MsgPack& doc_value)
{
	// RESERVED_VALUE isn't heritable and is not saved in schema.
	specification.value = std::make_unique<const MsgPack>(doc_value);
}


void
Schema::process_name(const MsgPack& doc_name)
{
	// RESERVED_NAME isn't heritable and is not saved in schema.
	try {
		specification.name.assign(doc_name.as_string());
	} catch (const msgpack::type_error&) {
		throw MSG_ClientError("Data inconsistency, %s must be string", RESERVED_NAME);
	}
}


void
Schema::process_script(const MsgPack& doc_script)
{
	// RESERVED_SCRIPT isn't heritable and is not saved in schema.
	specification.script = std::make_unique<const MsgPack>(doc_script);
}


MsgPack
Schema::index(const MsgPack& properties, const MsgPack& object, Xapian::Document& doc)
{
	L_CALL(this, "Schema::index()");

	try {
		MsgPack data;
		TaskVector tasks;
		tasks.reserve(object.size());
		auto prop_ptr = &properties;
		auto data_ptr = &data;
		for (const auto& item_key : object) {
			const auto str_key = item_key.as_string();
			try {
				auto func = map_dispatch_document.at(str_key);
				(this->*func)(object.at(str_key));
			} catch (const std::out_of_range&) {
				if (is_valid(str_key)) {
					tasks.push_back(std::async(std::launch::deferred, &Schema::index_object, this, std::ref(prop_ptr), std::ref(object.at(str_key)), std::ref(data_ptr), std::ref(doc), std::move(str_key)));
				} else {
					try {
						auto func = map_dispatch_root.at(str_key);
						tasks.push_back(std::async(std::launch::deferred, func, this, std::ref(properties), std::ref(object.at(str_key)), std::ref(data), std::ref(doc)));
					} catch (const std::out_of_range&) { }
				}
			}
		}

		restart_specification();
		const specification_t spc_start = std::move(specification);
		for (auto& task : tasks) {
			specification = spc_start;
			task.get();
		}

		for (const auto& elem : map_values) {
			auto val_ser = elem.second.serialise();
			doc.add_value(elem.first, val_ser);
			L_INDEX(this, "Slot: %zu  Values: %s", elem.first, repr(val_ser).c_str());
		}

		return data;
	} catch (...) {
		mut_schema.reset();
		throw;
	}
}


void
Schema::process_data(const MsgPack&, const MsgPack& doc_data, MsgPack& data, Xapian::Document&)
{
	L_CALL(this, "Schema::process_data()");

	data[RESERVED_DATA] = doc_data;
}


void
Schema::process_values(const MsgPack& properties, const MsgPack& doc_values, MsgPack& data, Xapian::Document& doc)
{
	L_CALL(this, "Schema::process_values()");

	specification.index = TypeIndex::VALUES;
	fixed_index(properties, doc_values, specification.store ? data[RESERVED_VALUES] : data, doc, RESERVED_VALUES);
}


void
Schema::process_field_values(const MsgPack& properties, const MsgPack& doc_values, MsgPack& data, Xapian::Document& doc)
{
	L_CALL(this, "Schema::process_field_values()");

	specification.index = TypeIndex::FIELD_VALUES;
	fixed_index(properties, doc_values, specification.store ? data[RESERVED_FIELD_VALUES] : data, doc, RESERVED_FIELD_VALUES);
}


void
Schema::process_global_values(const MsgPack& properties, const MsgPack& doc_values, MsgPack& data, Xapian::Document& doc)
{
	L_CALL(this, "Schema::process_global_values()");

	specification.index = TypeIndex::GLOBAL_VALUES;
	fixed_index(properties, doc_values, specification.store ? data[RESERVED_GLOBAL_VALUES] : data, doc, RESERVED_GLOBAL_VALUES);
}


void
Schema::process_terms(const MsgPack& properties, const MsgPack& doc_terms, MsgPack& data, Xapian::Document& doc)
{
	L_CALL(this, "Schema::process_terms()");

	specification.index = TypeIndex::TERMS;
	fixed_index(properties, doc_terms, specification.store ? data[RESERVED_TERMS] : data, doc, RESERVED_TERMS);
}


void
Schema::process_field_terms(const MsgPack& properties, const MsgPack& doc_terms, MsgPack& data, Xapian::Document& doc)
{
	L_CALL(this, "Schema::process_field_terms()");

	specification.index = TypeIndex::FIELD_TERMS;
	fixed_index(properties, doc_terms, specification.store ? data[RESERVED_FIELD_TERMS] : data, doc, RESERVED_FIELD_TERMS);
}


void
Schema::process_global_terms(const MsgPack& properties, const MsgPack& doc_terms, MsgPack& data, Xapian::Document& doc)
{
	L_CALL(this, "Schema::process_global_terms()");

	specification.index = TypeIndex::GLOBAL_TERMS;
	fixed_index(properties, doc_terms, specification.store ? data[RESERVED_GLOBAL_TERMS] : data, doc, RESERVED_GLOBAL_TERMS);
}


void
Schema::process_field_all(const MsgPack& properties, const MsgPack& doc_values, MsgPack& data, Xapian::Document& doc)
{
	L_CALL(this, "Schema::process_field_all()");

	specification.index = TypeIndex::FIELD_ALL;
	fixed_index(properties, doc_values, specification.store ? data[RESERVED_FIELD_ALL] : data, doc, RESERVED_FIELD_ALL);
}


void
Schema::process_global_all(const MsgPack& properties, const MsgPack& doc_values, MsgPack& data, Xapian::Document& doc)
{
	L_CALL(this, "Schema::process_global_all()");

	specification.index = TypeIndex::GLOBAL_ALL;
	fixed_index(properties, doc_values, specification.store ? data[RESERVED_GLOBAL_ALL] : data, doc, RESERVED_GLOBAL_ALL);
}


void
Schema::process_none(const MsgPack& properties, const MsgPack& doc_values, MsgPack& data, Xapian::Document& doc)
{
	L_CALL(this, "Schema::process_none()");

	specification.index = TypeIndex::NONE;
	fixed_index(properties, doc_values, specification.store ? data[RESERVED_NONE] : data, doc, RESERVED_NONE);
}


void
Schema::fixed_index(const MsgPack& properties, const MsgPack& object, MsgPack& data, Xapian::Document& doc, const char* reserved_word)
{
	specification.fixed_index = true;
	switch (object.getType()) {
		case MsgPack::Type::MAP: {
			auto prop_ptr = &properties;
			auto data_ptr = &data;
			return index_object(std::ref(prop_ptr), object, std::ref(data_ptr), doc);
		}
		case MsgPack::Type::ARRAY:
			return index_array(properties, object, data, doc);
		default:
			throw MSG_ClientError("%s must be an object or an array of objects", reserved_word);
	}
}


void
Schema::index_object(const MsgPack*& parent_properties, const MsgPack& object, MsgPack*& parent_data, Xapian::Document& doc, const std::string& name)
{
	L_CALL(this, "Schema::index_object()");

	const auto spc_start = specification;
	const MsgPack* properties = nullptr;
	MsgPack* data = nullptr;
	if (name.empty()) {
		properties = parent_properties;
		data = parent_data;
		specification.found_field = true;
	} else {
		data = specification.store ? &(*parent_data)[name] : parent_data;
		specification.name.assign(name);
		properties = &get_subproperties(*parent_properties);
	}

	switch (object.getType()) {
		case MsgPack::Type::MAP: {
			bool offsprings = false;
			TaskVector tasks;
			tasks.reserve(object.size());
			for (const auto& item_key : object) {
				const auto str_key = item_key.as_string();
				try {
					auto func = map_dispatch_document.at(str_key);
					(this->*func)(object.at(str_key));
				} catch (const std::out_of_range&) {
					if (is_valid(str_key)) {
						tasks.push_back(std::async(std::launch::deferred, &Schema::index_object, this, std::ref(properties), std::ref(object.at(str_key)), std::ref(data), std::ref(doc), std::move(str_key)));
						offsprings = true;
					}
				}
			}

			if (specification.name.empty()) {
				if (data != parent_data && !specification.store) {
					parent_data->erase(name);
					data = parent_data;
				}
				if (specification.value) {
					index_item(doc, *specification.value, *data);
					if (specification.store && !offsprings) {
						*data = (*data)[RESERVED_VALUE];
					}
				}
				if (specification.value_rec) {
					index_item(doc, *specification.value_rec, *data, 0);
					if (specification.store && !offsprings) {
						*data = (*data)[RESERVED_VALUE];
					}
				}
			} else {
				properties = &get_subproperties(*properties);
				if (specification.store) {
					data = &(*data)[specification.name];
				}
				if (specification.value) {
					index_item(doc, *specification.value, *data);
					if (specification.store && !offsprings) {
						*data = (*data)[RESERVED_VALUE];
					}
				}
				if (specification.value_rec) {
					index_item(doc, *specification.value_rec, *data, 0);
					if (specification.store && !offsprings) {
						*data = (*data)[RESERVED_VALUE];
					}
				}
			}

			const auto spc_object = std::move(specification);
			for (auto& task : tasks) {
				specification = spc_object;
				task.get();
			}

			if unlikely(offsprings && specification.sep_types[0] == FieldType::EMPTY) {
				set_type_to_object();
			}
			break;
		}
		case MsgPack::Type::ARRAY:
			if unlikely(specification.sep_types[1] == FieldType::EMPTY) {
				set_type_to_array();
			}
			index_array(*properties, object, *data, doc);
			break;
		default:
			index_item(doc, object, *data);
			if (specification.store && data->size() == 1) {
				*data = (*data)[RESERVED_VALUE];
			}
			break;
	}

	if (data->is_undefined()) {
		parent_data->erase(name);
	}

	specification = std::move(spc_start);
}


void
Schema::index_array(const MsgPack& properties, const MsgPack& array, MsgPack& data, Xapian::Document& doc)
{
	L_CALL(this, "Schema::index_array()");

	const auto spc_start = specification;
	size_t pos = 0;
	for (const auto& item : array) {
		switch (item.getType()) {
			case MsgPack::Type::MAP: {
				TaskVector tasks;
				tasks.reserve(item.size());
				specification.value = nullptr;
				specification.value_rec = nullptr;
				auto sub_properties = &properties;
				MsgPack* data_pos = nullptr;
				bool offsprings = false;

				for (const auto& property : item) {
					auto str_prop = property.as_string();
					try {
						auto func = map_dispatch_document.at(str_prop);
						(this->*func)(item.at(str_prop));
					} catch (const std::out_of_range&) {
						if (is_valid(str_prop)) {
							tasks.push_back(std::async(std::launch::deferred, &Schema::index_object, this, std::ref(sub_properties), std::ref(item.at(str_prop)), std::ref(data_pos), std::ref(doc), std::move(str_prop)));
							offsprings = true;
						}
					}
				}
				data_pos = specification.store ? &data[pos] : &data;

				if (specification.name.empty()) {
					if (specification.value) {
						index_item(doc, *specification.value, *data_pos);
						if (specification.store && !offsprings) {
							*data_pos = (*data_pos)[RESERVED_VALUE];
						}
					}
					if (specification.value_rec) {
						index_item(doc, *specification.value_rec, *data_pos);
						if (specification.store && !offsprings) {
							*data_pos = (*data_pos)[RESERVED_VALUE];
						}
					}
				} else {
					sub_properties = &get_subproperties(*sub_properties);
					if (specification.store) {
						data_pos = &(*data_pos)[specification.name];
					}
					if (specification.value) {
						index_item(doc, *specification.value, *data_pos);
						if (specification.store && !offsprings) {
							*data_pos = (*data_pos)[RESERVED_VALUE];
						}
					}
					if (specification.value_rec) {
						index_item(doc, *specification.value_rec, *data_pos);
						if (specification.store && !offsprings) {
							*data_pos = (*data_pos)[RESERVED_VALUE];
						}
					}
				}

				const auto spc_item = std::move(specification);
				for (auto& task : tasks) {
					specification = spc_item;
					task.get();
				}

				if unlikely(offsprings && specification.sep_types[0] == FieldType::EMPTY) {
					set_type_to_object();
				}

				specification = spc_start;
				break;
			}
			case MsgPack::Type::ARRAY: {
				MsgPack& data_pos = specification.store ? data[pos] : data;
				index_item(doc, item, data_pos);
				if (specification.store) {
					data_pos = data_pos[RESERVED_VALUE];
				}
				break;
			}
			default: {
				MsgPack& data_pos = specification.store ? data[pos] : data;
				index_item(doc, item, data_pos, pos);
				if (specification.store) {
					data_pos = data_pos[RESERVED_VALUE];
				}
				break;
			}
		}
		++pos;
	}
}


void
Schema::index_item(Xapian::Document& doc, const MsgPack& value, MsgPack& data, size_t pos)
{
	L_CALL(this, "Schema::index_item(1)");

	try {
		if unlikely(!specification.found_field && !specification.set_type) {
			if (!specification.dynamic) {
				throw MSG_ClientError("%s is not dynamic", specification.dynamic_full_name.c_str());
			}
			validate_required_data(value);
		} else if (specification.dynamic_type != DynamicFieldType::NONE) {
			update_dynamic_specification();
		}

		if (specification.prefix.empty()) {
			switch (specification.index) {
				case TypeIndex::NONE:
					return;
				case TypeIndex::TERMS:
				case TypeIndex::FIELD_TERMS:
				case TypeIndex::GLOBAL_TERMS:
					index_global_term(doc, Serialise::MsgPack(specification, value), specification_t::get_global(specification.sep_types[2]), pos);
					break;
				case TypeIndex::VALUES:
				case TypeIndex::FIELD_VALUES:
				case TypeIndex::GLOBAL_VALUES: {
					const auto& global_spc = specification_t::get_global(specification.sep_types[2]);
					StringSet& s_g = map_values[global_spc.slot];
					index_value(doc, value, s_g, global_spc, pos);
					break;
				}
				case TypeIndex::ALL:
				case TypeIndex::FIELD_ALL:
				case TypeIndex::GLOBAL_ALL: {
					const auto& global_spc = specification_t::get_global(specification.sep_types[2]);
					StringSet& s_g = map_values[global_spc.slot];
					index_value(doc, value, s_g, global_spc, pos, &Schema::index_global_term);
					break;
				}
			}
		} else {
			switch (specification.index) {
				case TypeIndex::NONE:
					return;
				case TypeIndex::TERMS:
					index_all_term(doc, Serialise::MsgPack(specification, value), specification, specification_t::get_global(specification.sep_types[2]), pos);
					break;
				case TypeIndex::FIELD_TERMS:
					index_field_term(doc, Serialise::MsgPack(specification, value), specification, pos);
					break;
				case TypeIndex::GLOBAL_TERMS:
					index_global_term(doc, Serialise::MsgPack(specification, value), specification_t::get_global(specification.sep_types[2]), pos);
					break;
				case TypeIndex::VALUES: {
					const auto& global_spc = specification_t::get_global(specification.sep_types[2]);
					StringSet& s_f = map_values[specification.slot];
					StringSet& s_g = map_values[global_spc.slot];
					index_all_value(doc, value, s_f, s_g, specification, global_spc, pos);
					break;
				}
				case TypeIndex::FIELD_VALUES: {
					StringSet& s_f = map_values[specification.slot];
					index_value(doc, value, s_f, specification, pos);
					break;
				}
				case TypeIndex::GLOBAL_VALUES: {
					const auto& global_spc = specification_t::get_global(specification.sep_types[2]);
					StringSet& s_g = map_values[global_spc.slot];
					index_value(doc, value, s_g, global_spc, pos);
					break;
				}
				case TypeIndex::ALL: {
					const auto& global_spc = specification_t::get_global(specification.sep_types[2]);
					StringSet& s_f = map_values[specification.slot];
					StringSet& s_g = map_values[global_spc.slot];
					index_all_value(doc, value, s_f, s_g, specification, global_spc, pos, true);
					break;
				}
				case TypeIndex::FIELD_ALL: {
					StringSet& s_f = map_values[specification.slot];
					index_value(doc, value, s_f, specification, pos, &Schema::index_field_term);
					break;
				}
				case TypeIndex::GLOBAL_ALL: {
					const auto& global_spc = specification_t::get_global(specification.sep_types[2]);
					StringSet& s_g = map_values[global_spc.slot];
					index_value(doc, value, s_g, global_spc, pos, &Schema::index_global_term);
					break;
				}
			}
		}

		if (specification.store) {
			// Add value to data.
			auto& data_value = data[RESERVED_VALUE];
			switch (data_value.getType()) {
				case MsgPack::Type::UNDEFINED:
					data_value = value;
					break;
				case MsgPack::Type::ARRAY:
					data_value.push_back(value);
					break;
				default:
					data_value = MsgPack({ data_value, value });
			}
		}
	} catch (const DummyException&) { }
}


void
Schema::index_item(Xapian::Document& doc, const MsgPack& values, MsgPack& data)
{
	L_CALL(this, "Schema::index_item()");

	try {
		if unlikely(!specification.found_field && !specification.set_type) {
			if (!specification.dynamic) {
				throw MSG_ClientError("%s is not dynamic", specification.dynamic_full_name.c_str());
			}
			validate_required_data(values);
		} else if (specification.dynamic_type != DynamicFieldType::NONE) {
			update_dynamic_specification();
		}

		if (specification.prefix.empty()) {
			switch (specification.index) {
				case TypeIndex::NONE:
					return;
				case TypeIndex::TERMS:
				case TypeIndex::FIELD_TERMS:
				case TypeIndex::GLOBAL_TERMS: {
					if (values.is_array()) {
						if unlikely(specification.sep_types[1] == FieldType::EMPTY) {
							set_type_to_array();
						}
						size_t pos = 0;
						const auto& global_spc = specification_t::get_global(specification.sep_types[2]);
						for (const auto& value : values) {
							index_global_term(doc, Serialise::MsgPack(specification, value), global_spc, pos++);
						}
					} else {
						index_global_term(doc, Serialise::MsgPack(specification, values), specification_t::get_global(specification.sep_types[2]), 0);
					}
					break;
				}
				case TypeIndex::VALUES:
				case TypeIndex::FIELD_VALUES:
				case TypeIndex::GLOBAL_VALUES: {
					const auto& global_spc = specification_t::get_global(specification.sep_types[2]);
					StringSet& s_g = map_values[global_spc.slot];
					if (values.is_array()) {
						if unlikely(specification.sep_types[1] == FieldType::EMPTY) {
							set_type_to_array();
						}
						size_t pos = 0;
						for (const auto& value : values) {
							index_value(doc, value, s_g, global_spc, pos++);
						}
					} else {
						index_value(doc, values, s_g, global_spc, 0);
					}
					break;
				}
				case TypeIndex::ALL:
				case TypeIndex::FIELD_ALL:
				case TypeIndex::GLOBAL_ALL: {
					const auto& global_spc = specification_t::get_global(specification.sep_types[2]);
					StringSet& s_g = map_values[global_spc.slot];
					if (values.is_array()) {
						if unlikely(specification.sep_types[1] == FieldType::EMPTY) {
							set_type_to_array();
						}
						size_t pos = 0;
						for (const auto& value : values) {
							index_value(doc, value, s_g, global_spc, pos++, &Schema::index_global_term);
						}
					} else {
						index_value(doc, values, s_g, global_spc, 0, &Schema::index_global_term);
					}
					break;
				}
			}
		} else {
			switch (specification.index) {
				case TypeIndex::NONE:
					return;
				case TypeIndex::TERMS: {
					if (values.is_array()) {
						if unlikely(specification.sep_types[1] == FieldType::EMPTY) {
							set_type_to_array();
						}
						size_t pos = 0;
						const auto& global_spc = specification_t::get_global(specification.sep_types[2]);
						for (const auto& value : values) {
							index_all_term(doc, Serialise::MsgPack(specification, value), specification, global_spc, pos++);
						}
					} else {
						index_all_term(doc, Serialise::MsgPack(specification, values), specification, specification_t::get_global(specification.sep_types[2]), 0);
					}
					break;
				}
				case TypeIndex::FIELD_TERMS: {
					if (values.is_array()) {
						if unlikely(specification.sep_types[1] == FieldType::EMPTY) {
							set_type_to_array();
						}
						size_t pos = 0;
						for (const auto& value : values) {
							index_field_term(doc, Serialise::MsgPack(specification, value), specification, pos++);
						}
					} else {
						index_field_term(doc, Serialise::MsgPack(specification, values), specification, 0);
					}
					break;
				}
				case TypeIndex::GLOBAL_TERMS: {
					if (values.is_array()) {
						if unlikely(specification.sep_types[1] == FieldType::EMPTY) {
							set_type_to_array();
						}
						size_t pos = 0;
						const auto& global_spc = specification_t::get_global(specification.sep_types[2]);
						for (const auto& value : values) {
							index_global_term(doc, Serialise::MsgPack(specification, value), global_spc, pos++);
						}
					} else {
						index_global_term(doc, Serialise::MsgPack(specification, values), specification_t::get_global(specification.sep_types[2]), 0);
					}
					break;
				}
				case TypeIndex::VALUES: {
					const auto& global_spc = specification_t::get_global(specification.sep_types[2]);
					StringSet& s_f = map_values[specification.slot];
					StringSet& s_g = map_values[global_spc.slot];
					if (values.is_array()) {
						if unlikely(specification.sep_types[1] == FieldType::EMPTY) {
							set_type_to_array();
						}
						size_t pos = 0;
						for (const auto& value : values) {
							index_all_value(doc, value, s_f, s_g, specification, global_spc, pos++);
						}
					} else {
						index_all_value(doc, values, s_f, s_g, specification, global_spc, 0);
					}
					break;
				}
				case TypeIndex::FIELD_VALUES: {
					StringSet& s_f = map_values[specification.slot];
					if (values.is_array()) {
						if unlikely(specification.sep_types[1] == FieldType::EMPTY) {
							set_type_to_array();
						}
						size_t pos = 0;
						for (const auto& value : values) {
							index_value(doc, value, s_f, specification, pos++);
						}
					} else {
						index_value(doc, values, s_f, specification, 0);
					}
					break;
				}
				case TypeIndex::GLOBAL_VALUES: {
					const auto& global_spc = specification_t::get_global(specification.sep_types[2]);
					StringSet& s_g = map_values[global_spc.slot];
					if (values.is_array()) {
						if unlikely(specification.sep_types[1] == FieldType::EMPTY) {
							set_type_to_array();
						}
						size_t pos = 0;
						for (const auto& value : values) {
							index_value(doc, value, s_g, global_spc, pos++);
						}
					} else {
						index_value(doc, values, s_g, global_spc, 0);
					}
					break;
				}
				case TypeIndex::ALL: {
					const auto& global_spc = specification_t::get_global(specification.sep_types[2]);
					StringSet& s_f = map_values[specification.slot];
					StringSet& s_g = map_values[global_spc.slot];
					if (values.is_array()) {
						if unlikely(specification.sep_types[1] == FieldType::EMPTY) {
							set_type_to_array();
						}
						size_t pos = 0;
						for (const auto& value : values) {
							index_all_value(doc, value, s_f, s_g, specification, global_spc, pos++, true);
						}
					} else {
						index_all_value(doc, values, s_f, s_g, specification, global_spc, 0, true);
					}
					break;
				}
				case TypeIndex::FIELD_ALL: {
					StringSet& s_f = map_values[specification.slot];
					if (values.is_array()) {
						if unlikely(specification.sep_types[1] == FieldType::EMPTY) {
							set_type_to_array();
						}
						size_t pos = 0;
						for (const auto& value : values) {
							index_value(doc, value, s_f, specification, pos++, &Schema::index_field_term);
						}
					} else {
						index_value(doc, values, s_f, specification, 0, &Schema::index_field_term);
					}
					break;
				}
				case TypeIndex::GLOBAL_ALL: {
					const auto& global_spc = specification_t::get_global(specification.sep_types[2]);
					StringSet& s_g = map_values[global_spc.slot];
					if (values.is_array()) {
						if unlikely(specification.sep_types[1] == FieldType::EMPTY) {
							set_type_to_array();
						}
						size_t pos = 0;
						for (const auto& value : values) {
							index_value(doc, value, s_g, global_spc, pos++, &Schema::index_global_term);
						}
					} else {
						index_value(doc, values, s_g, global_spc, 0, &Schema::index_global_term);
					}
					break;
				}
			}
		}

		if (specification.store) {
			// Add value to data.
			auto& data_value = data[RESERVED_VALUE];
			switch (data_value.getType()) {
				case MsgPack::Type::UNDEFINED:
					data_value = values;
					break;
				case MsgPack::Type::ARRAY:
					if (values.is_array()) {
						for (const auto& value : values) {
							data_value.push_back(value);
						}
					} else {
						data_value.push_back(values);
					}
					break;
				default:
					if (values.is_array()) {
						data_value = MsgPack({ data_value });
						for (const auto& value : values) {
							data_value.push_back(value);
						}
					} else {
						data_value = MsgPack({ data_value, values });
					}
					break;
			}
		}
	} catch (const DummyException&) { }
}


void
Schema::validate_required_data(const MsgPack& value)
{
	L_CALL(this, "Schema::validate_required_data()");

	if (specification.sep_types[2] == FieldType::EMPTY) {
		if (XapiandManager::manager->type_required) {
			throw MSG_MissingTypeError("Type of field [%s] is missing", specification.dynamic_full_name.c_str());
		}
		set_type(value);
	}

	if (!specification.full_name.empty()) {
		auto& properties = get_mutable(specification.full_name);

		// Process RESERVED_ACCURACY, RESERVED_ACC_PREFIX.
		std::set<uint64_t> set_acc;
		switch (specification.sep_types[2]) {
			case FieldType::GEO: {
				// Set partials and error.
				properties[RESERVED_PARTIALS] = specification.partials;
				properties[RESERVED_ERROR] = specification.error;

				if (specification.doc_acc) {
					try {
						for (const auto& _accuracy : *specification.doc_acc) {
							auto val_acc = _accuracy.as_u64();
							if (val_acc <= HTM_MAX_LEVEL) {
								set_acc.insert(val_acc);
							} else {
								throw MSG_ClientError("Data inconsistency, level value in %s: %s must be a positive number between 0 and %d (%llu not supported)", RESERVED_ACCURACY, GEO_STR, HTM_MAX_LEVEL, val_acc);
							}
						}
					} catch (const msgpack::type_error&) {
						throw MSG_ClientError("Data inconsistency, level value in %s: %s must be a positive number between 0 and %d", RESERVED_ACCURACY, GEO_STR, HTM_MAX_LEVEL);
					}
				} else {
					set_acc.insert(def_accuracy_geo.begin(), def_accuracy_geo.end());
				}
				break;
			}
			case FieldType::DATE: {
				if (specification.doc_acc) {
					try {
						for (const auto& _accuracy : *specification.doc_acc) {
							auto str_accuracy(lower_string(_accuracy.as_string()));
							try {
								set_acc.insert(toUType(map_acc_date.at(str_accuracy)));
							} catch (const std::out_of_range&) {
								throw MSG_ClientError("Data inconsistency, %s: %s must be a subset of %s (%s not supported)", RESERVED_ACCURACY, DATE_STR, str_set_acc_date.c_str(), str_accuracy.c_str());
							}
						}
					} catch (const msgpack::type_error&) {
						throw MSG_ClientError("Data inconsistency, %s in %s must be a subset of %s", RESERVED_ACCURACY, DATE_STR, str_set_acc_date.c_str());
					}
				} else {
					set_acc.insert(def_accuracy_date.begin(), def_accuracy_date.end());
				}
				break;
			}
			case FieldType::INTEGER:
			case FieldType::POSITIVE:
			case FieldType::FLOAT: {
				if (specification.doc_acc) {
					try {
						for (const auto& _accuracy : *specification.doc_acc) {
							set_acc.insert(_accuracy.as_u64());
						}
					} catch (const msgpack::type_error&) {
						throw MSG_ClientError("Data inconsistency, %s in %s must be an array of positive numbers", RESERVED_ACCURACY, Serialise::type(specification.sep_types[2]).c_str());
					}
				} else {
					set_acc.insert(def_accuracy_num.begin(), def_accuracy_num.end());
				}
				break;
			}
			case FieldType::TEXT:
				properties[RESERVED_STEM_STRATEGY] = specification.stem_strategy;
				if (specification.aux_stem_lan.empty() && !specification.aux_lan.empty()) {
					specification.stem_language = specification.aux_lan;
				}
				properties[RESERVED_STEM_LANGUAGE] = specification.stem_language;

				if (specification.aux_lan.empty() && !specification.aux_stem_lan.empty()) {
					specification.language = specification.aux_stem_lan;
				}
				properties[RESERVED_LANGUAGE] = specification.language;
				break;
			case FieldType::STRING:
				if (specification.aux_lan.empty() && !specification.aux_stem_lan.empty()) {
					specification.language = specification.aux_stem_lan;
				}
				properties[RESERVED_LANGUAGE] = specification.language;

				// Process RESERVED_BOOL_TERM
				if (!specification.set_bool_term) {
					// By default, if field name has upper characters then it is consider bool term.
					specification.bool_term = strhasupper(specification.dynamic_name);
				}
				properties[RESERVED_BOOL_TERM] = specification.bool_term;
				break;
			case FieldType::BOOLEAN:
			case FieldType::UUID:
				break;
			default:
				throw MSG_ClientError("%s '%c' is not supported", RESERVED_TYPE, toUType(specification.sep_types[2]));
		}

		if (specification.dynamic_type == DynamicFieldType::NONE) {
			if (set_acc.size()) {
				if (specification.acc_prefix.empty()) {
					for (const auto& acc : set_acc) {
						specification.acc_prefix.push_back(get_prefix(specification.dynamic_full_name + std::to_string(acc), DOCUMENT_CUSTOM_TERM_PREFIX, toUType(specification.sep_types[2])));
					}
				} else if (specification.acc_prefix.size() != set_acc.size()) {
					throw MSG_ClientError("Data inconsistency, there must be a prefix for each unique value in %s", RESERVED_ACCURACY);
				}

				specification.accuracy.insert(specification.accuracy.end(), set_acc.begin(), set_acc.end());
				properties[RESERVED_ACCURACY] = specification.accuracy;
				properties[RESERVED_ACC_PREFIX] = specification.acc_prefix;
			}

			// Process RESERVED_PREFIX
			if (specification.prefix.empty()) {
				specification.prefix = get_prefix(specification.dynamic_full_name, DOCUMENT_CUSTOM_TERM_PREFIX, toUType(specification.sep_types[2]));
			}
			properties[RESERVED_PREFIX] = specification.prefix;

			// Process RESERVED_SLOT
			if (specification.slot == Xapian::BAD_VALUENO) {
				specification.slot = get_slot(specification.dynamic_full_name);
			}
			properties[RESERVED_SLOT] = specification.slot;

		} else {
			if (set_acc.size()) {
				specification.accuracy.insert(specification.accuracy.end(), set_acc.begin(), set_acc.end());
				properties[RESERVED_ACCURACY] = specification.accuracy;
			}
			properties[RESERVED_INDEX] = specification.index;
			update_dynamic_specification();
		}

		// Process RESERVED_TYPE
		properties[RESERVED_TYPE] = specification.sep_types;

		specification.set_type = true;
	}
}


void
Schema::update_dynamic_specification() {
	switch (specification.index) {
		case TypeIndex::ALL:
		case TypeIndex::FIELD_ALL:
		case TypeIndex::GLOBAL_ALL:
			specification.prefix.assign(get_dynamic_prefix(specification.dynamic_full_name, DOCUMENT_CUSTOM_TERM_PREFIX, toUType(specification.sep_types[2])));
			specification.slot = get_slot(specification.dynamic_full_name);
			for (const auto& acc : specification.accuracy) {
				specification.acc_prefix.push_back(get_dynamic_prefix(specification.dynamic_full_name + std::to_string(acc), DOCUMENT_CUSTOM_TERM_PREFIX, toUType(specification.sep_types[2])));
			}
			break;
		case TypeIndex::VALUES:
		case TypeIndex::FIELD_VALUES:
		case TypeIndex::GLOBAL_VALUES:
			specification.slot = get_slot(specification.dynamic_full_name);
			for (const auto& acc : specification.accuracy) {
				specification.acc_prefix.push_back(get_dynamic_prefix(specification.dynamic_full_name + std::to_string(acc), DOCUMENT_CUSTOM_TERM_PREFIX, toUType(specification.sep_types[2])));
			}
			break;
		case TypeIndex::TERMS:
		case TypeIndex::FIELD_TERMS:
		case TypeIndex::GLOBAL_TERMS:
			specification.prefix.assign(get_dynamic_prefix(specification.dynamic_full_name, DOCUMENT_CUSTOM_TERM_PREFIX, toUType(specification.sep_types[2])));
			break;
		default:
			break;
	}
}


void
Schema::index_field_term(Xapian::Document& doc, std::string&& serialise_val, const specification_t& field_spc, size_t pos)
{
	L_CALL(nullptr, "Schema::index_field_term()");

	if (serialise_val.empty()) {
		return;
	}

	if (field_spc.sep_types[2] == FieldType::TEXT) {
		Xapian::TermGenerator term_generator;
		term_generator.set_document(doc);
		term_generator.set_stemmer(Xapian::Stem(field_spc.stem_language));
		term_generator.set_stemming_strategy(getGeneratorStrategy(field_spc.stem_strategy));
		// Xapian::WritableDatabase *wdb = nullptr;
		// bool spelling = field_spc.spelling[getPos(pos, field_spc.spelling.size())];
		// if (spelling) {
		// 	wdb = static_cast<Xapian::WritableDatabase *>(database->db.get());
		// 	term_generator.set_database(*wdb);
		// 	term_generator.set_flags(Xapian::TermGenerator::FLAG_SPELLING);
		// }
		bool positions = field_spc.positions[getPos(pos, field_spc.positions.size())];
		if (positions) {
			term_generator.index_text(serialise_val, field_spc.weight[getPos(pos, field_spc.weight.size())], field_spc.prefix);
		} else {
			term_generator.index_text_without_positions(serialise_val, field_spc.weight[getPos(pos, field_spc.weight.size())], field_spc.prefix);
		}
		L_INDEX(nullptr, "Field Text to Index [%d] => %s:%s [Positions: %d]", pos, field_spc.prefix.c_str(), serialise_val.c_str(), positions);
	} else {
		if (!field_spc.bool_term && field_spc.sep_types[2] == FieldType::STRING) {
			to_lower(serialise_val);
		}
		serialise_val = prefixed(serialise_val, field_spc.prefix);
		auto position = field_spc.position[getPos(pos, field_spc.position.size())];
		if (position) {
			if (field_spc.bool_term) {
				doc.add_posting(serialise_val, position, 0);
			} else {
				doc.add_posting(serialise_val, position, field_spc.weight[getPos(pos, field_spc.weight.size())]);
			}
		} else {
			if (field_spc.bool_term) {
				doc.add_boolean_term(serialise_val);
			} else {
				doc.add_term(serialise_val, field_spc.weight[getPos(pos, field_spc.weight.size())]);
			}
		}
		L_INDEX(nullptr, "Field Term [%d] -> %s  Bool: %d  Posting: %d", pos, repr(serialise_val).c_str(), field_spc.bool_term, position);
	}
}


void
Schema::index_global_term(Xapian::Document& doc, std::string&& serialise_val, const specification_t& global_spc, size_t pos)
{
	L_CALL(nullptr, "Schema::index_global_term()");

	if (serialise_val.empty()) {
		return;
	}

	if (global_spc.sep_types[2] == FieldType::TEXT) {
		Xapian::TermGenerator term_generator;
		term_generator.set_document(doc);
		term_generator.set_stemmer(Xapian::Stem(global_spc.stem_language));
		term_generator.set_stemming_strategy(getGeneratorStrategy(global_spc.stem_strategy));
		bool positions = global_spc.positions[getPos(pos, global_spc.positions.size())];
		if (positions) {
			term_generator.index_text(serialise_val, global_spc.weight[getPos(pos, global_spc.weight.size())]);
		} else {
			term_generator.index_text_without_positions(serialise_val, global_spc.weight[getPos(pos, global_spc.weight.size())]);
		}
		L_INDEX(nullptr, "Global Text to Index [%d] => %s [with positions: %d]", pos, serialise_val.c_str(), positions);
	} else {
		auto position = global_spc.position[getPos(pos, global_spc.position.size())];
		if (position) {
			if (global_spc.bool_term) {
				doc.add_posting(serialise_val, position, 0);
			} else {
				doc.add_posting(serialise_val, position, global_spc.weight[getPos(pos, global_spc.weight.size())]);
			}
		} else {
			if (global_spc.bool_term) {
				doc.add_boolean_term(serialise_val);
			} else {
				doc.add_term(serialise_val, global_spc.weight[getPos(pos, global_spc.weight.size())]);
			}
		}
		L_INDEX(nullptr, "Global Term [%d] -> %s  Bool: %d  Posting: %d", pos, repr(serialise_val).c_str(), global_spc.bool_term, position);
	}
}


void
Schema::index_all_term(Xapian::Document& doc, std::string&& serialise_val, const specification_t& field_spc, const specification_t& global_spc, size_t pos)
{
	L_CALL(nullptr, "Schema::index_all_term()");

	if (serialise_val.empty()) {
		return;
	}

	auto serialise_val_cp = serialise_val;
	index_field_term(doc, std::move(serialise_val_cp), field_spc, pos);
	index_global_term(doc, std::move(serialise_val), global_spc, pos);
}


void
Schema::index_value(Xapian::Document& doc, const MsgPack& value, StringSet& s, const specification_t& spc, size_t pos, dispatch_index fun)
{
	L_CALL(nullptr, "Schema::index_value()");

	switch (spc.sep_types[2]) {
		case FieldType::FLOAT: {
			try {
				auto f_val = value.as_f64();
				if (fun) {
					auto ser_value = Serialise::_float(f_val);
					s.insert(ser_value);
					(*fun)(doc, std::move(ser_value), spc, pos);
				} else {
					s.insert(Serialise::_float(f_val));
				}
				GenerateTerms::integer(doc, spc.accuracy, spc.acc_prefix, static_cast<int64_t>(f_val));
				return;
			} catch (const msgpack::type_error&) {
				throw MSG_ClientError("Format invalid for float type: %s", value.to_string().c_str());
			}
		}
		case FieldType::INTEGER: {
			try {
				auto i_val = value.as_i64();
				if (fun) {
					auto ser_value = Serialise::integer(i_val);
					s.insert(ser_value);
					(*fun)(doc, std::move(ser_value), spc, pos);
				} else {
					s.insert(Serialise::integer(i_val));
				}
				GenerateTerms::integer(doc, spc.accuracy, spc.acc_prefix, i_val);
				return;
			} catch (const msgpack::type_error&) {
				throw MSG_ClientError("Format invalid for integer type: %s", value.to_string().c_str());
			}
		}
		case FieldType::POSITIVE: {
			try {
				auto u_val = value.as_u64();
				if (fun) {
					auto ser_value = Serialise::positive(u_val);
					s.insert(ser_value);
					(*fun)(doc, std::move(ser_value), spc, pos);
				} else {
					s.insert(Serialise::positive(u_val));
				}
				GenerateTerms::positive(doc, spc.accuracy, spc.acc_prefix, u_val);
				return;
			} catch (const msgpack::type_error&) {
				throw MSG_ClientError("Format invalid for positive type: %s", value.to_string().c_str());
			}
		}
		case FieldType::DATE: {
			try {
				Datetime::tm_t tm;
				if (fun) {
					auto ser_value = Serialise::date(value, tm);
					s.insert(ser_value);
					(*fun)(doc, std::move(ser_value), spc, pos);
				} else {
					s.insert(Serialise::date(value, tm));
				}
				GenerateTerms::date(doc, spc.accuracy, spc.acc_prefix, tm);
				return;
			} catch (const msgpack::type_error&) {
				throw MSG_ClientError("Format invalid for date type: %s", value.to_string().c_str());
			}
		}
		case FieldType::GEO: {
			try {
				EWKT_Parser ewkt(value.as_string(), spc.partials, spc.error);
				if (fun) {
					(*fun)(doc, Serialise::trixels(ewkt.trixels), spc, pos);
				}
				auto ranges = ewkt.getRanges();
				s.insert(Serialise::geo(ranges, ewkt.centroids));
				GenerateTerms::geo(doc, spc.accuracy, spc.acc_prefix, ranges);
				return;
			} catch (const msgpack::type_error&) {
				throw MSG_ClientError("Format invalid for geo type: %s", value.to_string().c_str());
			}
		}
		case FieldType::STRING:
		case FieldType::TEXT: {
			try {
				if (fun) {
					auto ser_value = value.as_string();
					s.insert(ser_value);
					(*fun)(doc, std::move(ser_value), spc, pos);
				} else {
					s.insert(value.as_string());
				}
				return;
			} catch (const msgpack::type_error&) {
				throw MSG_ClientError("Format invalid for %s type: %s", Serialise::type(spc.sep_types[2]).c_str(), value.to_string().c_str());
			}
		}
		case FieldType::BOOLEAN: {
			try {
				if (fun) {
					auto ser_value = Serialise::MsgPack(spc, value);
					s.insert(ser_value);
					(*fun)(doc, std::move(ser_value), spc, pos);
				} else {
					s.insert(Serialise::MsgPack(spc, value));
				}
				return;
			} catch (const SerialisationError&) {
				throw MSG_ClientError("Format invalid for boolean type: %s", value.to_string().c_str());
			}
		}
		case FieldType::UUID: {
			try {
				if (fun) {
					auto ser_value = Serialise::uuid(value.as_string());
					s.insert(ser_value);
					(*fun)(doc, std::move(ser_value), spc, pos);
				} else {
					s.insert(Serialise::uuid(value.as_string()));
				}
				return;
			} catch (const msgpack::type_error&) {
				throw MSG_ClientError("Format invalid for uuid type: %s", value.to_string().c_str());
			} catch (const SerialisationError&) {
				throw MSG_ClientError("Format invalid for uuid type: %s", value.to_string().c_str());
			}
		}
		default:
			throw MSG_ClientError("Type: '%c' is an unknown type", spc.sep_types[2]);
	}
}


void
Schema::index_all_value(Xapian::Document& doc, const MsgPack& value, StringSet& s_f, StringSet& s_g, const specification_t& field_spc, const specification_t& global_spc, size_t pos, bool is_term)
{
	L_CALL(nullptr, "Schema::index_all_value()");

	switch (field_spc.sep_types[2]) {
		case FieldType::FLOAT: {
			try {
				auto f_val = value.as_f64();
				auto ser_value = Serialise::_float(f_val);
				if (is_term) {
					s_f.insert(ser_value);
					s_g.insert(ser_value);
					index_all_term(doc, std::move(ser_value), field_spc, global_spc, pos);
				} else {
					s_f.insert(ser_value);
					s_g.insert(std::move(ser_value));
				}
				if (field_spc.accuracy == global_spc.accuracy) {
					GenerateTerms::integer(doc, field_spc.accuracy, field_spc.acc_prefix, global_spc.acc_prefix, static_cast<int64_t>(f_val));
				} else {
					GenerateTerms::integer(doc, field_spc.accuracy, field_spc.acc_prefix, static_cast<int64_t>(f_val));
					GenerateTerms::integer(doc, global_spc.accuracy, global_spc.acc_prefix, static_cast<int64_t>(f_val));
				}
				return;
			} catch (const msgpack::type_error&) {
				throw MSG_ClientError("Format invalid for float type: %s", value.to_string().c_str());
			}
		}
		case FieldType::INTEGER: {
			try {
				auto i_val = value.as_i64();
				auto ser_value = Serialise::integer(i_val);
				if (is_term) {
					s_f.insert(ser_value);
					s_g.insert(ser_value);
					index_all_term(doc, std::move(ser_value), field_spc, global_spc, pos);
				} else {
					s_f.insert(ser_value);
					s_g.insert(std::move(ser_value));
				}
				if (field_spc.accuracy == global_spc.accuracy) {
					GenerateTerms::integer(doc, field_spc.accuracy, field_spc.acc_prefix, global_spc.acc_prefix, i_val);
				} else {
					GenerateTerms::integer(doc, field_spc.accuracy, field_spc.acc_prefix, i_val);
					GenerateTerms::integer(doc, global_spc.accuracy, global_spc.acc_prefix, i_val);
				}
				return;
			} catch (const msgpack::type_error&) {
				throw MSG_ClientError("Format invalid for integer type: %s", value.to_string().c_str());
			}
		}
		case FieldType::POSITIVE: {
			try {
				auto u_val = value.as_u64();
				auto ser_value = Serialise::positive(u_val);
				if (is_term) {
					s_f.insert(ser_value);
					s_g.insert(ser_value);
					index_all_term(doc, std::move(ser_value), field_spc, global_spc, pos);
				} else {
					s_f.insert(ser_value);
					s_g.insert(std::move(ser_value));
				}
				if (field_spc.accuracy == global_spc.accuracy) {
					GenerateTerms::positive(doc, field_spc.accuracy, field_spc.acc_prefix, global_spc.acc_prefix, u_val);
				} else {
					GenerateTerms::positive(doc, field_spc.accuracy, field_spc.acc_prefix, u_val);
					GenerateTerms::positive(doc, global_spc.accuracy, global_spc.acc_prefix, u_val);
				}
				return;
			} catch (const msgpack::type_error&) {
				throw MSG_ClientError("Format invalid for positive type: %s", value.to_string().c_str());
			}
		}
		case FieldType::DATE: {
			try {
				Datetime::tm_t tm;
				auto ser_value = Serialise::date(value, tm);
				if (is_term) {
					s_f.insert(ser_value);
					s_g.insert(ser_value);
					index_all_term(doc, std::move(ser_value), field_spc, global_spc, pos);
				} else {
					s_f.insert(ser_value);
					s_g.insert(std::move(ser_value));
				}
				if (field_spc.accuracy == global_spc.accuracy) {
					GenerateTerms::date(doc, field_spc.accuracy, field_spc.acc_prefix, global_spc.acc_prefix, tm);
				} else {
					GenerateTerms::date(doc, field_spc.accuracy, field_spc.acc_prefix, tm);
					GenerateTerms::date(doc, global_spc.accuracy, global_spc.acc_prefix, tm);
				}
				return;
			} catch (const msgpack::type_error&) {
				throw MSG_ClientError("Format invalid for date type: %s", value.to_string().c_str());
			}
		}
		case FieldType::GEO: {
			try {
				auto str_ewkt = value.as_string();
				if (field_spc.partials == global_spc.partials &&
					field_spc.error    == global_spc.error    &&
					field_spc.accuracy == global_spc.accuracy) {
					EWKT_Parser ewkt(str_ewkt, field_spc.partials, field_spc.error);
					if (is_term) {
						index_all_term(doc, Serialise::trixels(ewkt.trixels), field_spc, global_spc, pos);
					}
					auto ranges = ewkt.getRanges();
					auto val_ser = Serialise::geo(ranges, ewkt.centroids);
					s_f.insert(val_ser);
					s_g.insert(std::move(val_ser));
					GenerateTerms::geo(doc, field_spc.accuracy, field_spc.acc_prefix, global_spc.acc_prefix, ranges);
				} else {
					EWKT_Parser ewkt(str_ewkt, field_spc.partials, field_spc.error);
					EWKT_Parser g_ewkt(str_ewkt, global_spc.partials, global_spc.error);
					if (is_term) {
						index_field_term(doc, Serialise::trixels(ewkt.trixels), field_spc, pos);
						index_global_term(doc, Serialise::trixels(g_ewkt.trixels), global_spc, pos);
					}
					auto ranges = ewkt.getRanges();
					auto g_ranges = g_ewkt.getRanges();
					s_f.insert(Serialise::geo(ranges, ewkt.centroids));
					s_g.insert(Serialise::geo(g_ranges, g_ewkt.centroids));
					GenerateTerms::geo(doc, field_spc.accuracy, field_spc.acc_prefix, ranges);
					GenerateTerms::geo(doc, global_spc.accuracy, global_spc.acc_prefix, g_ranges);
				}
				return;
			} catch (const msgpack::type_error&) {
				throw MSG_ClientError("Format invalid for geo type: %s", value.to_string().c_str());
			}
		}
		case FieldType::STRING:
		case FieldType::TEXT: {
			try {
				auto ser_value = value.as_string();
				if (is_term) {
					s_f.insert(ser_value);
					s_g.insert(ser_value);
					index_all_term(doc, std::move(ser_value), field_spc, global_spc, pos);
				} else {
					s_f.insert(ser_value);
					s_g.insert(std::move(ser_value));
				}
				return;
			} catch (const msgpack::type_error&) {
				throw MSG_ClientError("Format invalid for %s type: %s", Serialise::type(field_spc.sep_types[2]).c_str(), value.to_string().c_str());
			}
		}
		case FieldType::BOOLEAN: {
			try {
				auto ser_value = Serialise::MsgPack(field_spc, value);
				if (is_term) {
					s_f.insert(ser_value);
					s_g.insert(ser_value);
					index_all_term(doc, std::move(ser_value), field_spc, global_spc, pos);
				} else {
					s_f.insert(ser_value);
					s_g.insert(std::move(ser_value));
				}
				return;
			} catch (const SerialisationError&) {
				throw MSG_ClientError("Format invalid for boolean type: %s", value.to_string().c_str());
			}
		}
		case FieldType::UUID: {
			try {
				auto ser_value = Serialise::uuid(value.as_string());
				if (is_term) {
					s_f.insert(ser_value);
					s_g.insert(ser_value);
					index_all_term(doc, std::move(ser_value), field_spc, global_spc, pos);
				} else {
					s_f.insert(ser_value);
					s_g.insert(std::move(ser_value));
				}
				return;
			} catch (const msgpack::type_error&) {
				throw MSG_ClientError("Format invalid for uuid type: %s", value.to_string().c_str());
			}
		}
		default:
			throw MSG_ClientError("Type: '%c' is an unknown type", field_spc.sep_types[2]);
	}
}


required_spc_t
Schema::get_data_field(const std::string& field_name) const
{
	L_CALL(this, "Schema::get_data_field()");

	required_spc_t res;

	if (field_name.empty()) {
		return res;
	}

	try {
		auto info = get_subproperties(schema->at(RESERVED_SCHEMA), field_name);
		const auto& properties = std::get<2>(info);

		const auto& sep_types = properties.at(RESERVED_TYPE);
		res.sep_types[0] = (FieldType)sep_types.at(0).as_u64();
		res.sep_types[1] = (FieldType)sep_types.at(1).as_u64();
		res.sep_types[2] = (FieldType)sep_types.at(2).as_u64();

		if (res.sep_types[2] == FieldType::EMPTY) {
			return res;
		}

		if (std::get<1>(info) == DynamicFieldType::NONE) {
			res.slot = static_cast<Xapian::valueno>(properties.at(RESERVED_SLOT).as_u64());
			res.prefix = properties.at(RESERVED_PREFIX).as_string();

			// Get accuracy, acc_prefix and reserved word.
			switch (res.sep_types[2]) {
				case FieldType::GEO:
					res.partials = properties.at(RESERVED_PARTIALS).as_bool();
					res.error = properties.at(RESERVED_ERROR).as_f64();
				case FieldType::FLOAT:
				case FieldType::INTEGER:
				case FieldType::POSITIVE:
				case FieldType::DATE: {
					try {
						for (const auto& acc : properties.at(RESERVED_ACCURACY)) {
							res.accuracy.push_back(acc.as_u64());
						}
						for (const auto& acc_p : properties.at(RESERVED_ACC_PREFIX)) {
							res.acc_prefix.push_back(acc_p.as_string());
						}
					} catch (const std::out_of_range&) { }
					break;
				}
				case FieldType::TEXT:
					res.stem_strategy = (StemStrategy)properties.at(RESERVED_STEM_STRATEGY).as_u64();
					res.stem_language = properties.at(RESERVED_STEM_LANGUAGE).as_string();
					res.language = properties.at(RESERVED_LANGUAGE).as_string();
					break;
				case FieldType::STRING:
					res.language = properties.at(RESERVED_LANGUAGE).as_string();
					res.bool_term = properties.at(RESERVED_BOOL_TERM).as_bool();
					break;
				default:
					break;
			}
		} else {
			res.slot = get_slot(std::get<0>(info));
			res.prefix = get_dynamic_prefix(std::get<0>(info), DOCUMENT_CUSTOM_TERM_PREFIX, (char)res.sep_types[2]);

			// Get accuracy, acc_prefix and reserved word.
			switch (res.sep_types[2]) {
				case FieldType::GEO:
					res.partials = properties.at(RESERVED_PARTIALS).as_bool();
					res.error = properties.at(RESERVED_ERROR).as_f64();
				case FieldType::FLOAT:
				case FieldType::INTEGER:
				case FieldType::POSITIVE:
				case FieldType::DATE: {
					try {
						for (const auto& acc : properties.at(RESERVED_ACCURACY)) {
							res.accuracy.push_back(acc.as_u64());
							res.acc_prefix.push_back(get_dynamic_prefix(std::get<0>(info) + std::to_string(res.accuracy.back()), DOCUMENT_CUSTOM_TERM_PREFIX, (char)res.sep_types[2]));
						}
					} catch (const std::out_of_range&) { }
					break;
				}
				case FieldType::TEXT:
					res.stem_strategy = (StemStrategy)properties.at(RESERVED_STEM_STRATEGY).as_u64();
					res.stem_language = properties.at(RESERVED_STEM_LANGUAGE).as_string();
					res.language = properties.at(RESERVED_LANGUAGE).as_string();
					break;
				case FieldType::STRING:
					res.language = properties.at(RESERVED_LANGUAGE).as_string();
					res.bool_term = properties.at(RESERVED_BOOL_TERM).as_bool();
					break;
				default:
					break;
			}
		}
	} catch (const std::exception& er) {
		L_ERR(this, "ERROR: %s", er.what());
	}

	return res;
}


required_spc_t
Schema::get_slot_field(const std::string& field_name) const
{
	L_CALL(this, "Schema::get_slot_field()");

	required_spc_t res;

	if (field_name.empty()) {
		return res;
	}

	try {
		auto info = get_subproperties(schema->at(RESERVED_SCHEMA), field_name);
		const auto& properties = std::get<2>(info);

		const auto& sep_types = properties.at(RESERVED_TYPE);
		res.sep_types[0] = (FieldType)sep_types.at(0).as_u64();
		res.sep_types[1] = (FieldType)sep_types.at(1).as_u64();
		res.sep_types[2] = (FieldType)sep_types.at(2).as_u64();

		// Get partials and error if type is GEO.
		switch (res.sep_types[2]) {
			case FieldType::GEO:
				res.partials = properties.at(RESERVED_PARTIALS).as_bool();
				res.error = properties.at(RESERVED_ERROR).as_f64();
				break;
			case FieldType::TEXT:
				res.stem_strategy = (StemStrategy)properties.at(RESERVED_STEM_STRATEGY).as_u64();
				res.stem_language = properties.at(RESERVED_STEM_LANGUAGE).as_string();
				res.language = properties.at(RESERVED_LANGUAGE).as_string();
				break;
			case FieldType::STRING:
				res.language = properties.at(RESERVED_LANGUAGE).as_string();
				res.bool_term = properties.at(RESERVED_BOOL_TERM).as_bool();
				break;
			default:
				break;
		}

		if (std::get<1>(info) == DynamicFieldType::NONE) {
			res.slot = static_cast<Xapian::valueno>(properties.at(RESERVED_SLOT).as_u64());
		} else {
			res.slot = get_slot(std::get<0>(info));
		}
	} catch (const std::exception& er) {
		L_ERR(this, "ERROR: %s", er.what());
	}

	return res;
}


const required_spc_t&
Schema::get_data_global(FieldType field_type)
{
	L_CALL(nullptr, "Schema::get_data_global()");

	switch (field_type) {
		case FieldType::FLOAT: {
			static const required_spc_t prop(DB_SLOT_NUMERIC, FieldType::FLOAT, def_accuracy_num, global_acc_prefix_num);
			return prop;
		}
		case FieldType::INTEGER: {
			static const required_spc_t prop(DB_SLOT_NUMERIC, FieldType::INTEGER, def_accuracy_num, global_acc_prefix_num);
			return prop;
		}
		case FieldType::POSITIVE: {
			static const required_spc_t prop(DB_SLOT_NUMERIC, FieldType::POSITIVE, def_accuracy_num, global_acc_prefix_num);
			return prop;
		}
		case FieldType::STRING: {
			static const required_spc_t prop(DB_SLOT_STRING, FieldType::TEXT, default_spc.accuracy, default_spc.acc_prefix);
			return prop;
		}
		case FieldType::TEXT: {
			static const required_spc_t prop(DB_SLOT_STRING, FieldType::TEXT, default_spc.accuracy, default_spc.acc_prefix);
			return prop;
		}
		case FieldType::BOOLEAN: {
			static const required_spc_t prop(DB_SLOT_BOOLEAN, FieldType::BOOLEAN, default_spc.accuracy, default_spc.acc_prefix);
			return prop;
		}
		case FieldType::DATE: {
			static const required_spc_t prop(DB_SLOT_DATE, FieldType::DATE, def_accuracy_date, global_acc_prefix_date);
			return prop;
		}
		case FieldType::GEO: {
			static const required_spc_t prop(DB_SLOT_GEO, FieldType::GEO, def_accuracy_geo, global_acc_prefix_geo);
			return prop;
		}
		case FieldType::UUID: {
			static const required_spc_t prop(DB_SLOT_UUID, FieldType::UUID, default_spc.accuracy, default_spc.acc_prefix);
			return prop;
		}
		default:
			throw MSG_ClientError("Type: '%u' is an unknown type", toUType(field_type));
	}
}
