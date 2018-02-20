/*
 * Copyright (C) 2015-2018 dubalu.com LLC. All rights reserved.
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

#include "datetime.h"

#include <cctype>           // for std::isdigit
#include <cmath>            // for ceil
#include <exception>        // for exception
#include <stdexcept>        // for invalid_argument, out_of_range
#include <stdio.h>          // for snprintf

#include "log.h"            // for L_ERR
#include "msgpack.h"        // for MsgPack
#include "utils.h"          // for stox
#include "string_view.h"    // for string_view

constexpr const char RESERVED_YEAR[]                = "_year";
constexpr const char RESERVED_MONTH[]               = "_month";
constexpr const char RESERVED_DAY[]                 = "_day";
constexpr const char RESERVED_TIME[]                = "_time";


const std::regex Datetime::date_re("([0-9]{4})([-/ ]?)(0[1-9]|1[0-2])\\2(0[0-9]|[12][0-9]|3[01])([T ]?([01]?[0-9]|2[0-3]):([0-5][0-9])(:([0-5][0-9])([.,]([0-9]+))?)?([ ]*[+-]([01]?[0-9]|2[0-3]):([0-5][0-9])|Z)?)?([ ]*\\|\\|[ ]*([+-/\\dyMwdhms]+))?", std::regex::optimize);
const std::regex Datetime::date_math_re("([+-]\\d+|\\/{1,2})([dyMwhms])", std::regex::optimize);


static constexpr int days[2][12] = {
	{ 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 },
	{ 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }
};


static constexpr int cumdays[2][12] = {
	{ 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 },
	{ 0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335 }
};


static void process_date_year(Datetime::tm_t& tm, const MsgPack& year) {
	switch (year.getType()) {
		case MsgPack::Type::POSITIVE_INTEGER:
			tm.year = year.u64();
			return;
		case MsgPack::Type::NEGATIVE_INTEGER:
			tm.year = year.i64();
			return;
		default:
			THROW(DatetimeError, "'%s' must be a positive integer value", RESERVED_YEAR);
	}
}


static void process_date_month(Datetime::tm_t& tm, const MsgPack& month) {
	switch (month.getType()) {
		case MsgPack::Type::POSITIVE_INTEGER:
			tm.mon = month.u64();
			return;
		case MsgPack::Type::NEGATIVE_INTEGER:
			tm.mon = month.i64();
			return;
		default:
			THROW(DatetimeError, "'%s' must be a positive integer value", RESERVED_MONTH);
	}
}


static void process_date_day(Datetime::tm_t& tm, const MsgPack& day) {
	switch (day.getType()) {
		case MsgPack::Type::POSITIVE_INTEGER:
			tm.day = day.u64();
			return;
		case MsgPack::Type::NEGATIVE_INTEGER:
			tm.day = day.i64();
			return;
		default:
			THROW(DatetimeError, "'%s' must be a positive integer value", RESERVED_DAY);
	}
}


static void process_date_time(Datetime::tm_t& tm, string_view str_time) {
	auto size = str_time.size();
	try {
		switch (size) {
			case 5: // 00:00
				if (str_time[2] == ':') {
					tm.hour = strict_stoul(str_time.substr(0, 2));
					if (tm.hour < 24) {
						tm.min = strict_stoul(str_time.substr(3, 2));
						if (tm.min < 60) {
							tm.sec = 0;
							tm.fsec = 0.0;
							return;
						}
					}
					THROW(DatetimeError, "Time: %s is out of range", std::string(str_time).c_str());
				}
				break;
			case 8: // 00:00:00
				if (str_time[2] == ':' && str_time[5] == ':') {
					tm.hour = strict_stoul(str_time.substr(0, 2));
					if (tm.hour < 24) {
						tm.min = strict_stoul(str_time.substr(3, 2));
						if (tm.min < 60) {
							tm.sec = strict_stoul(str_time.substr(6, 2));
							if (tm.sec < 60) {
								tm.fsec = 0.0;
								return;
							}
						}
					}
					THROW(DatetimeError, "Time: %s is out of range", std::string(str_time).c_str());
				}
				break;
			default: //  00:00:00[+-]00:00  00:00:00.000...  00:00:00.000...[+-]00:00
				if (size > 9 && (str_time[2] == ':' && str_time[5] == ':')) {
					tm.hour = strict_stoul(str_time.substr(0, 2));
					if (tm.hour < 24) {
						tm.min = strict_stoul(str_time.substr(3, 2));
						if (tm.min < 60) {
							tm.sec = strict_stoul(str_time.substr(6, 2));
							if (tm.sec < 60) {
								switch (str_time[8]) {
									case '+':
									case '-':
										if (size == 14 && str_time[11] == ':') {
											tm.fsec = 0.0;
											auto tz_h = str_time.substr(9, 2);
											auto tz_m = str_time.substr(12, 2);
											if (strict_stoul(tz_h) < 24 && strict_stoul(tz_m) < 60) {
												computeTimeZone(tm, str_time[8], tz_h, tz_m);
												return;
											}
											THROW(DatetimeError, "Time: %s is out of range", std::string(str_time).c_str());
										}
										THROW(DatetimeError, "Error format in: %s, the format must be '00:00:00[+-]00:00'", std::string(str_time).c_str());
									case '.': {
										auto it = str_time.begin() + 8;
										const auto it_e = str_time.end();
										for (auto aux = it + 1; aux != it_e; ++aux) {
											const auto& c = *aux;
											if (c < '0' || c > '9') {
												if (c == '+' || c == '-') {
													if ((it_e - aux) == 6) {
														auto aux_end = aux + 3;
														if (*aux_end == ':') {
															auto tz_h = string_view(aux + 1, aux_end - aux + 1);
															auto tz_m = string_view(aux_end + 1, it_e - aux_end + 1);
															if (strict_stoul(tz_h) < 24 && strict_stoul(tz_m) < 60) {
																computeTimeZone(tm, c, tz_h, tz_m);
																tm.fsec = Datetime::normalize_fsec(strict_stod(string_view(it, aux - it)));
																return;
															}
															THROW(DatetimeError, "Time: %s is out of range", std::string(str_time).c_str());
														}
													}
												}
												THROW(DatetimeError, "Error format in _time: %s, the format must be '00:00(:00(.0...)([+-]00:00))'", std::string(str_time).c_str());
											}
										}
										tm.fsec = Datetime::normalize_fsec(strict_stod(string_view(it, it_e - it)));
										return;
									}
									default:
										break;
								}
							}
						}
					}
				}
				break;
		}
		THROW(DatetimeError, "Error format in _time: %s, the format must be '00:00(:00(.0...)[+-]00:00))'", std::string(str_time).c_str());
	} catch (const OutOfRange& er) {
		THROW(DatetimeError, "Error format in _time: %s, the format must be '00:00(:00(.0...)[+-]00:00))' %s", std::string(str_time).c_str(), er.what());
	} catch (const InvalidArgument& er) {
		THROW(DatetimeError, "Error format in _time: %s, the format must be '00:00(:00(.0...)[+-]00:00))' %s", std::string(str_time).c_str(), er.what());
	}
}


static const std::unordered_map<string_view, void (*)(Datetime::tm_t&, const MsgPack&)> map_dispatch_date({
	{ RESERVED_YEAR,    &process_date_year   },
	{ RESERVED_MONTH,   &process_date_month  },
	{ RESERVED_DAY,     &process_date_day    },
});


/*
 * Returnd struct tm according to the date specified by date.
 */
Datetime::tm_t
Datetime::DateParser(string_view date)
{
	tm_t tm;
	// Check if date is ISO 8601.
	auto pos = date.find("||");
	if (pos == string_view::npos) {
		auto format = Iso8601Parser(date, tm);
		switch (format) {
			case Format::VALID:
				return tm;
			case Format::INVALID:
				break;
			case Format::OUT_OF_RANGE:
				THROW(DatetimeError, "Date: %s is out of range", std::string(date).c_str());
			default:
				THROW(DatetimeError, "In DatetimeParser, format %s is incorrect", std::string(date).c_str());
		}
	} else {
		auto format = Iso8601Parser(date.substr(0, pos), tm);
		switch (format) {
			case Format::VALID:
				processDateMath(date.substr(pos + 2), tm);
				return tm;
			case Format::INVALID:
				break;
			case Format::OUT_OF_RANGE:
				THROW(DatetimeError, "Date: %s is out of range", std::string(date).c_str());
			default:
				THROW(DatetimeError, "In DatetimeParser, format %s is incorrect", std::string(date).c_str());
		}
	}

	std::cmatch m;
	if (std::regex_match(date.begin(), date.end(), m, date_re) && static_cast<std::size_t>(m.length(0)) == date.size()) {
		tm.year = strict_stoi(m.str(1));
		tm.mon = strict_stoi(m.str(3));
		tm.day = strict_stoi(m.str(4));
		if (!isvalidDate(tm.year, tm.mon, tm.day)) {
			THROW(DatetimeError, "Date: %s is out of range", std::string(date).c_str());
		}

		// Process time
		if (m.length(5) == 0) {
			tm.hour = tm.min = tm.sec = 0;
			tm.fsec = 0.0;
		} else {
			tm.hour = strict_stoi(m.str(6));
			tm.min = strict_stoi(m.str(7));
			if (m.length(8) == 0) {
				tm.sec = 0;
				tm.fsec = 0.0;
			} else {
				tm.sec = strict_stoi(m.str(9));
				if (m.length(10) == 0) {
					tm.fsec = 0.0;
				} else {
					auto fs = m.str(11);
					fs.insert(0, 1, '.');
					tm.fsec = normalize_fsec(strict_stod(fs));
				}
			}
			if (m.length(12) != 0) {
				computeTimeZone(tm, date[m.position(13) - 1], m.str(13), m.str(14));
			}
		}

		// Process Date Math
		if (m.length(16) != 0) {
			processDateMath(m.str(16), tm);
		}

		return tm;
	}

	THROW(DatetimeError, "In DatetimeParser, format %s is incorrect", std::string(date).c_str());
}


/*
 * Returnd struct tm according to the date specified by value.
 */
Datetime::tm_t
Datetime::DateParser(const MsgPack& value)
{
	double _timestamp;
	switch (value.getType()) {
		case MsgPack::Type::POSITIVE_INTEGER:
			_timestamp = value.u64();
			return Datetime::to_tm_t(_timestamp);
		case MsgPack::Type::NEGATIVE_INTEGER:
			_timestamp = value.i64();
			return Datetime::to_tm_t(_timestamp);
		case MsgPack::Type::FLOAT:
			_timestamp = value.f64();
			return Datetime::to_tm_t(_timestamp);
		case MsgPack::Type::STR:
			return Datetime::DateParser(value.str_view());
		case MsgPack::Type::MAP: {
			Datetime::tm_t tm;
			string_view str_time;
			const auto it_e = value.end();
			for (auto it = value.begin(); it != it_e; ++it) {
				auto str_key = it->str_view();
				try {
					auto func = map_dispatch_date.at(str_key);
					(*func)(tm, it.value());
				} catch (const std::out_of_range&) {
					if (str_key == RESERVED_TIME) {
						try {
							str_time = it.value().str_view();
						} catch (const msgpack::type_error&) {
							THROW(DatetimeError, "'%s' must be string", RESERVED_TIME);
						}
					} else {
						THROW(DatetimeError, "Unsupported Key: %s in date", repr(str_key).c_str());
					}
				}
			}
			if (Datetime::isvalidDate(tm.year, tm.mon, tm.day)) {
				if (!str_time.empty()) {
					process_date_time(tm, str_time);
				}
				return tm;
			}
			THROW(DatetimeError, "Date is out of range");
		}
		default:
			THROW(DatetimeError, "Date value must be numeric or string");
	}
}


/*
 * Full struct tm according to the date in ISO 8601 format.
 */
Datetime::Format
Datetime::Iso8601Parser(string_view date, tm_t& tm)
{
	auto size = date.size();
	try {
		switch (size) {
			case 10: // 0000-00-00
				if (date[4] == '-' && date[7] == '-') {
					tm.year  = strict_stoul(date.substr(0, 4));
					tm.mon   = strict_stoul(date.substr(5, 2));
					tm.day   = strict_stoul(date.substr(8, 2));
					if (isvalidDate(tm.year, tm.mon, tm.day)) {
						tm.hour = 0;
						tm.min  = 0;
						tm.sec  = 0;
						tm.fsec = 0.0;
						return Format::VALID;
					}
					return Format::OUT_OF_RANGE;
				}
				return Format::INVALID;
			case 19: // 0000-00-00[T ]00:00:00
				if (date[4] == '-' && date[7] == '-' && (date[10] == 'T' || date[10] == ' ') && date[13] == ':' && date[16] == ':') {
					tm.year  = strict_stoul(date.substr(0, 4));
					tm.mon   = strict_stoul(date.substr(5, 2));
					tm.day   = strict_stoul(date.substr(8, 2));
					if (isvalidDate(tm.year, tm.mon, tm.day)) {
						tm.hour = strict_stoul(date.substr(11, 2));
						if (tm.hour < 24) {
							tm.min = strict_stoul(date.substr(14, 2));
							if (tm.min < 60) {
								tm.sec = strict_stoul(date.substr(17, 2));
								if (tm.sec < 60) {
									tm.fsec = 0.0;
									return Format::VALID;
								}
							}
						}
					}
					return Format::OUT_OF_RANGE;
				}
				return Format::INVALID;
			case 20: // 0000-00-00[T ]00:00:00Z
				if (date[4] == '-' && date[7] == '-' && (date[10] == 'T' || date[10] == ' ') && date[13] == ':' &&
					date[16] == ':' && date[19] == 'Z') {
					tm.year  = strict_stoul(date.substr(0, 4));
					tm.mon   = strict_stoul(date.substr(5, 2));
					tm.day   = strict_stoul(date.substr(8, 2));
					if (isvalidDate(tm.year, tm.mon, tm.day)) {
						tm.hour = strict_stoul(date.substr(11, 2));
						if (tm.hour < 24) {
							tm.min = strict_stoul(date.substr(14, 2));
							if (tm.min < 60) {
								tm.sec = strict_stoul(date.substr(17, 2));
								if (tm.sec < 60) {
									tm.fsec = 0.0;
									return Format::VALID;
								}
							}
						}
					}
					return Format::OUT_OF_RANGE;
				}
				return Format::INVALID;
			default: // 0000-00-00[T ]00:00:00[+-]00:00  0000-00-00[T ]00:00:00.0...  0000-00-00[T ]00:00:00.0...[+-]00:00
				if (size > 20 && date[4] == '-' && date[7] == '-' && (date[10] == 'T' || date[10] == ' ') &&
					date[13] == ':' && date[16] == ':') {
					tm.year  = strict_stoul(date.substr(0, 4));
					tm.mon   = strict_stoul(date.substr(5, 2));
					tm.day   = strict_stoul(date.substr(8, 2));
					if (isvalidDate(tm.year, tm.mon, tm.day)) {
						tm.hour = strict_stoul(date.substr(11, 2));
						if (tm.hour < 24) {
							tm.min = strict_stoul(date.substr(14, 2));
							if (tm.min < 60) {
								tm.sec = strict_stoul(date.substr(17, 2));
								if (tm.sec < 60) {
									switch (date[19]) {
										case '+':
										case '-':
											if (size == 25 && date[22] == ':') {
												tm.fsec = 0.0;
												auto tz_h = date.substr(20, 2);
												auto tz_m = date.substr(23, 2);
												if (strict_stoul(tz_h) < 24 && strict_stoul(tz_m) < 60) {
													computeTimeZone(tm, date[19], tz_h, tz_m);
													return Format::VALID;
												}
												return Format::OUT_OF_RANGE;
											}
											return Format::INVALID;
										case '.': {
											auto it = date.begin() + 19;
											const auto it_e = date.end();
											for (auto aux = it + 1; aux != it_e; ++aux) {
												const auto& c = *aux;
												if (c < '0' || c > '9') {
													switch (c) {
														case 'Z':
															if ((aux + 1) == it_e) {
																tm.fsec = normalize_fsec(strict_stod(string_view(it, aux - it)));
																return Format::VALID;
															}
															return Format::ERROR;
														case '+':
														case '-':
															if ((it_e - aux) == 6) {
																auto aux_end = aux + 3;
																if (*aux_end == ':') {
																	string_view tz_h(aux + 1, aux_end - aux + 1);
																	string_view tz_m(aux_end + 1, it_e - aux_end + 1);
																	if (strict_stoul(tz_h) < 24 && strict_stoul(tz_m) < 60) {
																		computeTimeZone(tm, c, tz_h, tz_m);
																		tm.fsec = normalize_fsec(strict_stod(string_view(it, aux - it)));
																		return Format::VALID;
																	}
																	return Format::OUT_OF_RANGE;
																}
															}
															return Format::INVALID;
														default:
															return Format::INVALID;
													}
												}
											}
											tm.fsec = normalize_fsec(strict_stod(string_view(it, it_e - it)));
											return Format::VALID;
										}
										default:
											return Format::INVALID;
									}
								}
							}
						}
					}
					return Format::OUT_OF_RANGE;
				}
				return Format::INVALID;
		}
	} catch (const OutOfRange&) {
		return Format::ERROR;
	} catch (const InvalidArgument&) {
		return Format::ERROR;
	}
}


Datetime::Format
Datetime::Iso8601Parser(string_view date)
{
	auto size = date.size();
	try {
		switch (size) {
			case 10: // 0000-00-00
				if (date[4] == '-' && date[7] == '-') {
					auto year  = strict_stoul(date.substr(0, 4));
					auto mon   = strict_stoul(date.substr(5, 2));
					auto day   = strict_stoul(date.substr(8, 2));
					if (isvalidDate(year, mon, day)) {
						return Format::VALID;
					}
					return Format::OUT_OF_RANGE;
				}
				return Format::INVALID;
			case 19: // 0000-00-00[T ]00:00:00
				if (date[4] == '-' && date[7] == '-' && (date[10] == 'T' || date[10] == ' ') && date[13] == ':' && date[16] == ':') {
					auto year  = strict_stoul(date.substr(0, 4));
					auto mon   = strict_stoul(date.substr(5, 2));
					auto day   = strict_stoul(date.substr(8, 2));
					if (isvalidDate(year, mon, day)) {
						auto hour = strict_stoul(date.substr(11, 2));
						if (hour < 24) {
							auto min = strict_stoul(date.substr(14, 2));
							if (min < 60) {
								auto sec = strict_stoul(date.substr(17, 2));
								if (sec < 60) {
									return Format::VALID;
								}
							}
						}
					}
					return Format::OUT_OF_RANGE;
				}
				return Format::INVALID;
			case 20: // 0000-00-00[T ]00:00:00Z
				if (date[4] == '-' && date[7] == '-' && (date[10] == 'T' || date[10] == ' ') && date[13] == ':' &&
					date[16] == ':' && date[19] == 'Z') {
					auto year  = strict_stoul(date.substr(0, 4));
					auto mon   = strict_stoul(date.substr(5, 2));
					auto day   = strict_stoul(date.substr(8, 2));
					if (isvalidDate(year, mon, day)) {
						auto hour = strict_stoul(date.substr(11, 2));
						if (hour < 24) {
							auto min = strict_stoul(date.substr(14, 2));
							if (min < 60) {
								auto sec = strict_stoul(date.substr(17, 2));
								if (sec < 60) {
									return Format::VALID;
								}
							}
						}
					}
					return Format::OUT_OF_RANGE;
				}
				return Format::INVALID;
			default: // 0000-00-00[T ]00:00:00[+-]00:00  0000-00-00[T ]00:00:00.0...  0000-00-00[T ]00:00:00.0...[+-]00:00
				if (size > 20 && date[4] == '-' && date[7] == '-' && (date[10] == 'T' || date[10] == ' ') &&
					date[13] == ':' && date[16] == ':') {
					auto year  = strict_stoul(date.substr(0, 4));
					auto mon   = strict_stoul(date.substr(5, 2));
					auto day   = strict_stoul(date.substr(8, 2));
					if (isvalidDate(year, mon, day)) {
						auto hour = strict_stoul(date.substr(11, 2));
						if (hour < 24) {
							auto min = strict_stoul(date.substr(14, 2));
							if (min < 60) {
								auto sec = strict_stoul(date.substr(17, 2));
								if (sec < 60) {
									switch (date[19]) {
										case '+':
										case '-':
											if (size == 25 && date[22] == ':') {
												auto tz_h = date.substr(20, 2);
												auto tz_m = date.substr(23, 2);
												if (strict_stoul(tz_h) < 24 && strict_stoul(tz_m) < 60) {
													return Format::VALID;
												}
												return Format::OUT_OF_RANGE;
											}
											return Format::INVALID;
										case '.': {
											auto it = date.begin() + 19;
											const auto it_e = date.end();
											for (auto aux = it + 1; aux != it_e; ++aux) {
												const auto& c = *aux;
												if (c < '0' || c > '9') {
													switch (c) {
														case 'Z':
															if ((aux + 1) == it_e) {
																return Format::VALID;
															}
															return Format::ERROR;
														case '+':
														case '-':
															if ((it_e - aux) == 6) {
																auto aux_end = aux + 3;
																if (*aux_end == ':') {
																	auto tz_h = std::string(aux + 1, aux_end);
																	auto tz_m = std::string(aux_end + 1, it_e);
																	if (strict_stoul(tz_h) < 24 && strict_stoul(tz_m) < 60) {
																		return Format::VALID;
																	}
																	return Format::OUT_OF_RANGE;
																}
															}
															return Format::INVALID;
														default:
															return Format::INVALID;
													}
												}
											}
											return Format::VALID;
										}
										default:
											return Format::INVALID;
									}
								}
							}
						}
					}
					return Format::OUT_OF_RANGE;
				}
				return Format::INVALID;
		}
	} catch (const OutOfRange&) {
		return Format::ERROR;
	} catch (const InvalidArgument&) {
		return Format::ERROR;
	}
}


void
Datetime::processDateMath(string_view date_math, tm_t& tm)
{
	size_t size_match = 0;

	std::cregex_iterator next(date_math.begin(), date_math.end(), date_math_re, std::regex_constants::match_continuous);
	std::cregex_iterator end;
	while (next != end) {
		size_match += next->length(0);
		computeDateMath(tm, next->str(1), next->str(2)[0]);
		++next;
	}

	if (date_math.size() != size_match) {
		THROW(DatetimeError, "Date Math (%s) is used incorrectly", std::string(date_math).c_str());
	}
}


void
Datetime::computeTimeZone(tm_t& tm, char op, string_view hour, string_view min)
{
	std::string oph, opm;
	oph.reserve(3);
	opm.reserve(3);
	if (op == '+') {
		oph.push_back('-');
		oph.append(hour.data(), hour.size());
		opm.push_back('-');
		opm.append(min.data(), min.size());
	} else {
		oph.push_back('+');
		oph.append(hour.data(), hour.size());
		opm.push_back('+');
		opm.append(min.data(), min.size());
	}
	computeDateMath(tm, oph, 'h');
	computeDateMath(tm, opm, 'm');
}


/*
 * Compute a Date Math former by op + units.
 * op can be +#, -#, /, //
 * unit can be y, M, w, d, h, m, s
 */
void
Datetime::computeDateMath(tm_t& tm, string_view op, char unit)
{
	switch (op[0]) {
		case '+': {
			auto num = strict_stoi(op.substr(1));
			switch (unit) {
				case 'y':
					tm.year += num; break;
				case 'M': {
					tm.mon += num;
					normalizeMonths(tm.year, tm.mon);
					auto max_days = getDays_month(tm.year, tm.mon);
					if (tm.day > max_days) tm.day = max_days;
					break;
				}
				case 'w':
					tm.day += 7 * num; break;
				case 'd':
					tm.day += num; break;
				case 'h':
					tm.hour += num; break;
				case 'm':
					tm.min += num; break;
				case 's':
					tm.sec += num; break;
				default:
					THROW(DatetimeError, "Invalid format in Date Math unit: '%c'. Unit must be in { y, M, w, d, h, m, s }", unit);
			}
			break;
		}
		case '-': {
			auto num = strict_stoi(op.substr(1));
			switch (unit) {
				case 'y':
					tm.year -= num;
					break;
				case 'M': {
					tm.mon -= num;
					normalizeMonths(tm.year, tm.mon);
					auto max_days = getDays_month(tm.year, tm.mon);
					if (tm.day > max_days) tm.day = max_days;
					break;
				}
				case 'w':
					tm.day -= 7 * num; break;
				case 'd':
					tm.day -= num; break;
				case 'h':
					tm.hour -= num; break;
				case 'm':
					tm.min -= num; break;
				case 's':
					tm.sec -= num; break;
				default:
					THROW(DatetimeError, "Invalid format in Date Math unit: '%c'. Unit must be in { y, M, w, d, h, m, s }", unit);
			}
			break;
		}
		case '/':
			switch (unit) {
				case 'y':
					if (op.size() == 1) {
						tm.mon = 12;
						tm.day = getDays_month(tm.year, 12);
						tm.hour = 23;
						tm.min = tm.sec = 59;
						tm.fsec = DATETIME_MAX_FSEC;
					} else if (op.size() == 2 && op[1] == '/') {
						tm.mon = tm.day = 1;
						tm.hour = tm.min = tm.sec = 0;
						tm.fsec = 0.0;
					} else {
						THROW(DatetimeError, "Invalid format in Date Math operator: %s. Operator must be in { +#, -#, /, // }", std::string(op).c_str());
					}
					break;
				case 'M':
					if (op.size() == 1) {
						tm.day = getDays_month(tm.year, tm.mon);
						tm.hour = 23;
						tm.min = tm.sec = 59;
						tm.fsec = DATETIME_MAX_FSEC;
					} else if (op.size() == 2 && op[1] == '/') {
						tm.day = 1;
						tm.hour = tm.min = tm.sec = 0;
						tm.fsec = 0.0;
					} else {
						THROW(DatetimeError, "Invalid format in Date Math operator: %s. Operator must be in { +#, -#, /, // }", std::string(op).c_str());
					}
					break;
				case 'w': {
					auto dateGMT = timegm(tm);
					struct tm timeinfo;
					gmtime_r(&dateGMT, &timeinfo);
					if (op.size() == 1) {
						tm.day += 6 - timeinfo.tm_wday;
						tm.hour = 23;
						tm.min = tm.sec = 59;
						tm.fsec = DATETIME_MAX_FSEC;
					} else if (op.size() == 2 && op[1] == '/') {
						tm.day -= timeinfo.tm_wday;
						tm.hour = tm.min = tm.sec = 0;
						tm.fsec = 0.0;
					} else {
						THROW(DatetimeError, "Invalid format in Date Math operator: %s. Operator must be in { +#, -#, /, // }", std::string(op).c_str());
					}
					break;
				}
				case 'd':
					if (op.size() == 1) {
						tm.hour = 23;
						tm.min = tm.sec = 59;
						tm.fsec = DATETIME_MAX_FSEC;
					} else if (op.size() == 2 && op[1] == '/') {
						tm.hour = tm.min = tm.sec = 0;
						tm.fsec = 0.0;
					} else {
						THROW(DatetimeError, "Invalid format in Date Math operator: %s. Operator must be in { +#, -#, /, // }", std::string(op).c_str());
					}
					break;
				case 'h':
					if (op.size() == 1) {
						tm.min = tm.sec = 59;
						tm.fsec = DATETIME_MAX_FSEC;
					} else if (op.size() == 2 && op[1] == '/') {
						tm.min = tm.sec = 0;
						tm.fsec = 0.0;
					} else {
						THROW(DatetimeError, "Invalid format in Date Math operator: %s. Operator must be in { +#, -#, /, // }", std::string(op).c_str());
					}
					break;
				case 'm':
					if (op.size() == 1) {
						tm.sec = 59;
						tm.fsec = DATETIME_MAX_FSEC;
					} else if (op.size() == 2 && op[1] == '/') {
						tm.sec = 0;
						tm.fsec = 0.0;
					} else {
						THROW(DatetimeError, "Invalid format in Date Math operator: %s. Operator must be in { +#, -#, /, // }", std::string(op).c_str());
					}
					break;
				case 's':
					if (op.size() == 1) {
						tm.fsec = DATETIME_MAX_FSEC;
					} else if (op.size() == 2 && op[1] == '/') {
						tm.fsec = 0.0;
					} else {
						THROW(DatetimeError, "Invalid format in Date Math operator: %s. Operator must be in { +#, -#, /, // }", std::string(op).c_str());
					}
				break;
			}
			break;
		default:
			THROW(DatetimeError, "Invalid format in Date Math operator: %s. Operator must be in { +#, -#, /, // }", std::string(op).c_str());
	}

	// Update date.
	auto dateGMT = timegm(tm);
	struct tm timeinfo;
	gmtime_r(&dateGMT, &timeinfo);
	tm.year = timeinfo.tm_year + DATETIME_START_YEAR;
	tm.mon = timeinfo.tm_mon + 1;
	tm.day = timeinfo.tm_mday;
	tm.hour = timeinfo.tm_hour;
	tm.min = timeinfo.tm_min;
	tm.sec = timeinfo.tm_sec;
}


/*
 * Returns if a year is leap.
 */
bool
Datetime::isleapYear(int year)
{
	return year % 400 == 0 || (year % 4 == 0 && year % 100);
}


/*
 * Returns if a tm_year is leap.
 */
bool
Datetime::isleapRef_year(int tm_year)
{
	tm_year += DATETIME_START_YEAR;
	return isleapYear(tm_year);
}


/*
 * Returns number of days in month, given the year.
 */
int
Datetime::getDays_month(int year, int month)
{
	if (month < 1 || month > 12) THROW(DatetimeError, "Month must be in 1..12");

	int leap = isleapYear(year);
	return days[leap][month - 1];
}


/*
 * Normalize months between -11 and 11
 */
void
Datetime::normalizeMonths(int& year, int& mon)
{
	if (mon > 12) {
		year += mon / 12;
		mon %= 12;
	} else while (mon < 1) {
		mon += 12;
		year--;
	}
}


/*
 * Returns the proleptic Gregorian ordinal of the date,
 * where January 1 of year 1 has ordinal 1 (reference date).
 * year -> Any positive number except zero.
 * month -> Between 1 and 12 inclusive.
 * day -> Between 1 and the number of days in the given month of the given year.
 */
std::time_t
Datetime::toordinal(int year, int month, int day)
{
	if (year < 1) THROW(DatetimeError, "Year is out of range");
	if (day < 1 || day > getDays_month(year, month)) THROW(DatetimeError, "Day is out of range for month");

	int leap = isleapYear(year);
	std::time_t result = 365 * (year - 1) + (year - 1) / 4 - (year - 1) / 100 + (year - 1) / 400 + cumdays[leap][month - 1] + day;
	return result;
}


/*
 * Function to calculate Unix timestamp from Coordinated Universal Time (UTC).
 * Only for year greater than 0.
 */
std::time_t
Datetime::timegm(const std::tm& tm)
{
	int year = tm.tm_year + DATETIME_START_YEAR, mon = tm.tm_mon + 1;
	normalizeMonths(year, mon);
	auto result = toordinal(year, mon, 1) - DATETIME_EPOCH_ORD + tm.tm_mday - 1;
	result *= 24;
	result += tm.tm_hour;
	result *= 60;
	result += tm.tm_min;
	result *= 60;
	result += tm.tm_sec;
	return result;
}


/*
 * Function to calculate Unix timestamp from Coordinated Universal Time (UTC).
 * Only for year greater than 0.
 */
std::time_t
Datetime::timegm(tm_t& tm)
{
	normalizeMonths(tm.year, tm.mon);
	auto result = toordinal(tm.year, tm.mon, 1) - DATETIME_EPOCH_ORD + tm.day - 1;
	result *= 24;
	result += tm.hour;
	result *= 60;
	result += tm.min;
	result *= 60;
	result += tm.sec;
	return result;
}


/*
 * Transforms timestamp to a struct tm_t.
 */
Datetime::tm_t
Datetime::to_tm_t(std::time_t timestamp)
{
	struct tm timeinfo;
	gmtime_r(&timestamp, &timeinfo);
	return tm_t(
		timeinfo.tm_year + DATETIME_START_YEAR, timeinfo.tm_mon + 1,
		timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min,
		timeinfo.tm_sec
	);
}


/*
 * Transforms timestamp to a struct tm_t.
 */
Datetime::tm_t
Datetime::to_tm_t(double timestamp)
{
	auto _time = static_cast<std::time_t>(timestamp);
	struct tm timeinfo;
	gmtime_r(&_time, &timeinfo);
	return tm_t(
		timeinfo.tm_year + DATETIME_START_YEAR, timeinfo.tm_mon + 1,
		timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min,
		timeinfo.tm_sec, normalize_fsec(timestamp - _time)
	);
}


/*
 * Function to calculate Unix timestamp from Coordinated Universal Time (UTC).
 * Only for year greater than 0.
 * Returns Timestamp with milliseconds as the decimal part.
 */
double
Datetime::timestamp(const tm_t& tm)
{
	int year = tm.year, mon = tm.mon;
	normalizeMonths(year, mon);
	auto result = static_cast<double>(toordinal(year, mon, 1) - DATETIME_EPOCH_ORD + tm.day - 1);
	result *= 24;
	result += tm.hour;
	result *= 60;
	result += tm.min;
	result *= 60;
	result += tm.sec;
	result < 0 ? result -= tm.fsec : result += tm.fsec;
	return result;
}


/*
 * Validate Date.
 */
bool
Datetime::isvalidDate(int year, int month, int day)
{
	if (year < 1) {
		L_ERR("ERROR: Year is out of range.");
		return false;
	}

	try {
		if (day < 1 || day > getDays_month(year, month)) {
			L_ERR("ERROR: Day is out of range for month.");
			return false;
		}
	} catch (const std::exception& ex) {
		L_ERR("ERROR: %s.", *ex.what() ? ex.what() : "Unkown exception!");
		return false;
	}

	return true;
}


/*
 * Return a string with the date in ISO 8601 Format.
 */
std::string
Datetime::iso8601(const std::tm& tm, bool trim, char sep)
{
	std::string res;
	if (trim) {
		res.resize(20);
		res.resize(snprintf(&res[0], 20, "%04d-%02d-%02d%c%02d:%02d:%02d",
			tm.tm_year + DATETIME_START_YEAR, tm.tm_mon + 1, tm.tm_mday,
			sep, tm.tm_hour, tm.tm_min, tm.tm_sec));
	} else {
		res.resize(27);
		res.resize(snprintf(&res[0], 27, "%04d-%02d-%02d%c%02d:%02d:%02d.000000",
			tm.tm_year + DATETIME_START_YEAR, tm.tm_mon + 1, tm.tm_mday,
			sep, tm.tm_hour, tm.tm_min, tm.tm_sec));
	}
	return res;
}


/*
 * Return a string with the date in ISO 8601 Format.
 */
std::string
Datetime::iso8601(const tm_t& tm, bool trim, char sep)
{
	std::string res;
	if (trim) {
		if (tm.fsec > 0.0) {
			res.resize(27);
			res.resize(snprintf(&res[0], 27, "%04d-%02d-%02d%c%02d:%02d:%02d.%06d",
				tm.year, tm.mon, tm.day, sep,
				tm.hour, tm.min, tm.sec, static_cast<int>(tm.fsec / DATETIME_MICROSECONDS)));
			auto it_e = res.end();
			auto it = it_e - 1;
			for (; *it == '0'; --it);
			if (*it != '.') ++it;
			res.erase(it, it_e);
		} else {
			res.resize(20);
			res.resize(snprintf(&res[0], 20, "%04d-%02d-%02d%c%02d:%02d:%02d",
				tm.year, tm.mon, tm.day, sep,
				tm.hour, tm.min, tm.sec));
		}
	} else {
		if (tm.fsec > 0.0) {
			res.resize(27);
			res.resize(snprintf(&res[0], 27, "%04d-%02d-%02d%c%02d:%02d:%02d.%06d",
				tm.year, tm.mon, tm.day, sep,
				tm.hour, tm.min, tm.sec, static_cast<int>(tm.fsec / DATETIME_MICROSECONDS)));
		} else {
			res.resize(27);
			res.resize(snprintf(&res[0], 27, "%04d-%02d-%02d%c%02d:%02d:%02d.000000",
				tm.year, tm.mon, tm.day, sep,
				tm.hour, tm.min, tm.sec));
		}
	}
	return res;
}


/*
 * Transforms a timestamp in seconds with decimal fraction to ISO 8601 format.
 */
std::string
Datetime::iso8601(double timestamp, bool trim, char sep)
{
	auto tm = to_tm_t(timestamp);
	return iso8601(tm, trim, sep);
}


/*
 * Transforms a time_point in seconds with decimal fraction to ISO 8601 format.
 */
std::string
Datetime::iso8601(const std::chrono::time_point<std::chrono::system_clock>& tp, bool trim, char sep)
{
	return iso8601(std::chrono::duration_cast<std::chrono::microseconds>(tp.time_since_epoch()).count() * DATETIME_MICROSECONDS, trim, sep);
}


bool
Datetime::isDate(string_view date)
{
	auto format = Iso8601Parser(date);
	switch (format) {
		case Format::VALID:
			return true;
		case Format::INVALID: {
			std::cmatch m;
			return std::regex_match(date.begin(), date.end(), m, date_re) && static_cast<std::size_t>(m.length(0)) == date.size();
		}
		default:
			return false;
	}
}


Datetime::clk_t
Datetime::TimeParser(string_view _time)
{
	clk_t clk;
	auto length = _time.length();
	try {
		switch (length) {
			case 5: // 00:00
				if (_time[2] == ':') {
					clk.hour = strict_stoul(_time.substr(0, 2));
					clk.min = strict_stoul(_time.substr(3, 2));
					return clk;
				}
				break;
			case 8: // 00:00:00
				if (_time[2] == ':' && _time[5] == ':') {
					clk.hour = strict_stoul(_time.substr(0, 2));
					clk.min = strict_stoul(_time.substr(3, 2));
					clk.sec = strict_stoul(_time.substr(6, 2));
					return clk;
				}
				break;
			default: //  00:00:00[+-]00:00  00:00:00.000...  00:00:00.000...[+-]00:00
				if (length > 9 && (_time[2] == ':' && _time[5] == ':')) {
					clk.hour = strict_stoul(_time.substr(0, 2));
					clk.min = strict_stoul(_time.substr(3, 2));
					clk.sec = strict_stoul(_time.substr(6, 2));
					switch (_time[8]) {
						case '-':
							clk.tz_s = '-';
						case '+':
							if (length == 14 && _time[11] == ':') {
								clk.tz_h = strict_stoul(_time.substr(9, 2));
								clk.tz_m = strict_stoul(_time.substr(12, 2));
								return clk;
							}
							break;
						case '.': {
							auto it = _time.begin() + 8;
							const auto it_e = _time.end();
							for (auto aux = it + 1; aux != it_e; ++aux) {
								const auto& c = *aux;
								if (c < '0' || c > '9') {
									switch (c) {
										case '-':
											clk.tz_s = '-';
										case '+':
											if ((it_e - aux) == 6) {
												auto aux_end = aux + 3;
												if (*aux_end == ':') {
													clk.tz_h = strict_stoul(string_view(aux + 1, aux_end - aux + 1));
													clk.tz_m = strict_stoul(string_view(aux_end + 1, it_e - aux_end + 1));
													clk.fsec = Datetime::normalize_fsec(strict_stod(string_view(it, aux - it)));
													return clk;
												}
											}
											break;
										default:
											break;
									}
									THROW(TimeError, "Error format in time: %s, the format must be '00:00(:00(.0...)([+-]00:00))'", std::string(_time).c_str());
								}
							}
							clk.fsec = Datetime::normalize_fsec(strict_stod(string_view(it, it_e - it)));
							return clk;
						}
						default:
							break;
					}
				}
				break;
		}
		THROW(TimeError, "Error format in time: %s, the format must be '00:00(:00(.0...)([+-]00:00))'", std::string(_time).c_str());
	} catch (const OutOfRange& er) {
		THROW(TimeError, "Error format in time: %s, the format must be '00:00(:00(.0...)([+-]00:00))' [%s]", std::string(_time).c_str(), er.what());
	} catch (const InvalidArgument& er) {
		THROW(TimeError, "Error format in time: %s, the format must be '00:00(:00(.0...)([+-]00:00))' [%s]", std::string(_time).c_str(), er.what());
	}
}


/*
 * Transforms double time to a struct clk_t.
 */
Datetime::clk_t
Datetime::time_to_clk_t(double t)
{
	if (isvalidTime(t)) {
		clk_t clk;
		if (t < 0) {
			auto _time = static_cast<int>(-t);
			clk.fsec = t + _time;
			if (clk.fsec < 0.0) {
				++_time;
			}
			clk.tz_h = _time / 3600;
			int aux;
			if (clk.tz_h < 100) {
				aux = clk.tz_h * 3600;
			} else {
				clk.tz_h = 99;
				aux = clk.tz_h * 3600;
			}
			clk.tz_m = (_time - aux) / 60;
			clk.sec = _time - aux - clk.tz_m * 60;
			if (clk.sec) {
				clk.sec = 60 - clk.sec;
				++clk.tz_m;
			}
			clk.tz_s = '+';
			if (clk.fsec < 0) {
				clk.fsec += 1.0;
			}
		} else {
			auto _time = static_cast<int>(t);
			clk.hour = _time / 3600;
			if (clk.hour < 100) {
				auto aux = _time - clk.hour * 3600;
				clk.min = aux / 60;
				clk.sec = aux - clk.min * 60;
			} else {
				if (clk.hour < 199) {
					auto aux = _time - clk.hour * 3600;
					clk.tz_h = clk.hour - 99;
					clk.hour = 99;
					clk.min = aux / 60;
					clk.sec = aux - clk.min * 60;
				} else {
					clk.tz_h = 99;
					clk.hour = 99;
					auto aux = _time - 712800; // 198 * 3600
					clk.min = aux / 60;
					if (clk.min < 100) {
						clk.sec = aux - clk.min * 60;
					} else {
						if (clk.min < 199) {
							clk.sec = aux - clk.min * 60;
							clk.tz_m = 198 - clk.min;
							clk.min = 99;
						} else {
							clk.tz_m = 99;
							clk.min = 99;
							clk.sec = aux - 11880; // 198 * 60
						}
					}
				}
			}
			clk.tz_s = '-';
			clk.fsec = t - _time;
		}

		return clk;
	}

	THROW(TimeError, "Bad serialised time value");
}


double
Datetime::time_to_double(const MsgPack& _time)
{
	switch (_time.getType()) {
		case MsgPack::Type::POSITIVE_INTEGER: {
			double t_val = _time.u64();
			if (isvalidTime(t_val)) {
				return t_val;
			}
			THROW(TimeError, "Time: %f is out of range", t_val);
		}
		case MsgPack::Type::NEGATIVE_INTEGER: {
			double t_val = _time.i64();
			if (isvalidTime(t_val)) {
				return t_val;
			}
			THROW(TimeError, "Time: %f is out of range", t_val);
		}
		case MsgPack::Type::FLOAT: {
			double t_val = _time.f64();
			if (isvalidTime(t_val)) {
				return t_val;
			}
			THROW(TimeError, "Time: %f is out of range", t_val);
		}
		case MsgPack::Type::STR:
			return time_to_double(TimeParser(_time.str_view()));
		default:
			THROW(TimeError, "Time must be numeric or string");
	}
}


double
Datetime::time_to_double(const clk_t& clk)
{
	int hour, min;
	if (clk.tz_s == '-') {
		hour = clk.hour + clk.tz_h;
		min = clk.min + clk.tz_m;
	} else {
		hour = clk.hour - clk.tz_h;
		min = clk.min - clk.tz_m;
	}
	return clk.sec + clk.fsec + (hour * 60 + min) * 60;
}


std::string
Datetime::time_to_string(const clk_t& clk, bool trim)
{
	std::string res;
	if (clk.fsec > 0 || !trim) {
		if (trim && clk.tz_h == 0 && clk.tz_m == 0) {
			res.resize(16);
			res.resize(snprintf(&res[0], 16, "%02d:%02d:%02d.%06d",
				clk.hour, clk.min, clk.sec, static_cast<int>(clk.fsec / DATETIME_MICROSECONDS)));
			auto it_e = res.end();
			auto it = it_e - 1;
			for (; *it == '0'; --it);
			if (*it != '.') ++it;
			res.erase(it, it_e);
		} else {
			res.resize(23);
			res.resize(snprintf(&res[0], 23, "%02d:%02d:%02d.%06d%c%02d:%02d",
				clk.hour, clk.min, clk.sec, static_cast<int>(clk.fsec / DATETIME_MICROSECONDS),
				clk.tz_s, clk.tz_h, clk.tz_m));
			auto it_e = res.begin() + 15;
			auto it = it_e - 1;
			for (; *it == '0'; --it);
			if (*it != '.') ++it;
			res.erase(it, it_e);
		}
	} else if (clk.tz_h == 0 && clk.tz_m == 0) {
		res.resize(9);
		res.resize(snprintf(&res[0], 9, "%02d:%02d:%02d",
			clk.hour, clk.min, clk.sec));
	} else {
		res.resize(15);
		res.resize(snprintf(&res[0], 15, "%02d:%02d:%02d%c%02d:%02d",
			clk.hour, clk.min, clk.sec,
			clk.tz_s, clk.tz_h, clk.tz_m));
	}
	return res;
}


std::string
Datetime::time_to_string(double t, bool trim)
{
	return time_to_string(time_to_clk_t(t), trim);
}


bool
Datetime::isTime(string_view _time) {
	auto length = _time.length();
	switch (length) {
		case 5: // 00:00
			return std::isdigit(_time[0]) && std::isdigit(_time[1]) && _time[2] == ':' &&
				std::isdigit(_time[3]) && std::isdigit(_time[4]);
		case 8: // 00:00:00
			return std::isdigit(_time[0]) && std::isdigit(_time[1]) && _time[2] == ':' &&
				std::isdigit(_time[3]) && std::isdigit(_time[4]) && _time[5] == ':' &&
				std::isdigit(_time[6]) && std::isdigit(_time[7]);
		default: //  00:00:00[+-]00:00  00:00:00.000...  00:00:00.000...[+-]00:00
			if (length > 9 && std::isdigit(_time[0]) && std::isdigit(_time[1]) && _time[2] == ':' &&
				std::isdigit(_time[3]) && std::isdigit(_time[4]) && _time[5] == ':' &&
				std::isdigit(_time[6]) && std::isdigit(_time[7])) {
				switch (_time[8]) {
					case '+':
					case '-':
						return length == 14 && std::isdigit(_time[9]) && std::isdigit(_time[10]) && _time[11] == ':' &&
							std::isdigit(_time[12]) && std::isdigit(_time[13]);
					case '.':
						for (size_t i = 9; i < length; ++i) {
							const auto c = _time[i];
							if (!std::isdigit(c)) {
								return (c == '+' || c == '-') && length == (i + 6) && std::isdigit(_time[i + 1]) &&
									std::isdigit(_time[i + 2]) && _time[i + 3] == ':' && std::isdigit(_time[i + 4]) &&
									std::isdigit(_time[i + 5]);
							}
						}
						return true;
				}
			}
			return false;
	}
}


Datetime::clk_t
Datetime::TimedeltaParser(string_view timedelta)
{
	clk_t clk;
	auto size = timedelta.size();
	try {
		switch (size) {
			case 6: // [+-]00:00
				switch (timedelta[0]) {
					case '-':
						clk.tz_s = '-';
					case '+':
						if (timedelta[3] == ':') {
							clk.hour = strict_stoul(timedelta.substr(1, 2));
							clk.min = strict_stoul(timedelta.substr(4, 2));
							return clk;
						}
					default:
						break;
				}
				break;

			case 9: // [+-]00:00:00
				switch (timedelta[0]) {
					case '-':
						clk.tz_s = '-';
					case '+':
						if (timedelta[3] == ':' && timedelta[6] == ':') {
							clk.hour = strict_stoul(timedelta.substr(1, 2));
							clk.min = strict_stoul(timedelta.substr(4, 2));
							clk.sec = strict_stoul(timedelta.substr(7, 2));
							return clk;
						}
					default:
						break;
				}
				break;

			default: //  [+-]00:00:00.000...
				switch (timedelta[0]) {
					case '-':
						clk.tz_s = '-';
					case '+':
						if (size > 10 && (timedelta[3] == ':' && timedelta[6] == ':' && timedelta[9] == '.')) {
							clk.hour = strict_stoul(timedelta.substr(1, 2));
							clk.min = strict_stoul(timedelta.substr(4, 2));
							clk.sec = strict_stoul(timedelta.substr(7, 2));
							auto it = timedelta.begin() + 9;
							const auto it_e = timedelta.end();
							for (auto aux = it + 1; aux != it_e; ++aux) {
								const auto& c = *aux;
								if (c < '0' || c > '9') {
									THROW(TimedeltaError, "Error format in timedelta: %s, the format must be '[+-]00:00(:00(.0...))'", std::string(timedelta).c_str());
								}
							}
							clk.fsec = Datetime::normalize_fsec(strict_stod(string_view(it, it_e - it)));
							return clk;
						}
					default:
						break;
				}
				break;
		}
		THROW(TimedeltaError, "Error format in timedelta: %s, the format must be '[+-]00:00(:00(.0...))'", std::string(timedelta).c_str());
	} catch (const OutOfRange& er) {
		THROW(TimedeltaError, "Error format in timedelta: %s, the format must be '[+-]00:00(:00(.0...))' %s", std::string(timedelta).c_str(), er.what());
	} catch (const InvalidArgument& er) {
		THROW(TimedeltaError, "Error format in timedelta: %s, the format must be '[+-]00:00(:00(.0...))' %s", std::string(timedelta).c_str(), er.what());
	}
}


/*
 * Transforms double timedelta to a struct clk_t.
 */
Datetime::clk_t
Datetime::timedelta_to_clk_t(double t)
{
	if (isvalidTimedelta(t)) {
		clk_t clk;
		if (t < 0) {
			t *= -1.0;
			clk.tz_s = '-';
		}

		auto _time = static_cast<int>(t);
		clk.hour = _time / 3600;
		if (clk.hour < 100) {
			auto aux = _time - clk.hour * 3600;
			clk.min = aux / 60;
			clk.sec = aux - clk.min * 60;
		} else {
			clk.hour = 99;
			auto aux = _time - clk.hour * 3600;
			clk.min = aux / 60;
			if (clk.min < 100) {
				clk.sec =  aux - clk.min * 60;
			} else {
				clk.min = 99;
				clk.sec =  aux - clk.min * 60;
			}
		}
		clk.fsec = t - _time;

		return clk;
	}

	THROW(TimedeltaError, "Bad serialised timedelta value");
}


double
Datetime::timedelta_to_double(const MsgPack& timedelta)
{
	switch (timedelta.getType()) {
		case MsgPack::Type::POSITIVE_INTEGER: {
			double t_val = timedelta.u64();
			if (isvalidTimedelta(t_val)) {
				return t_val;
			}
			THROW(TimedeltaError, "Timedelta: %f is out of range", t_val);
		}
		case MsgPack::Type::NEGATIVE_INTEGER: {
			double t_val = timedelta.i64();
			if (isvalidTimedelta(t_val)) {
				return t_val;
			}
			THROW(TimedeltaError, "Timedelta: %f is out of range", t_val);
		}
		case MsgPack::Type::FLOAT: {
			double t_val = timedelta.f64();
			if (isvalidTimedelta(t_val)) {
				return t_val;
			}
			THROW(TimedeltaError, "Timedelta: %f is out of range", t_val);
		}
		case MsgPack::Type::STR:
			return timedelta_to_double(TimedeltaParser(timedelta.str_view()));
		default:
			THROW(TimedeltaError, "Timedelta must be numeric or string");
	}
}


double
Datetime::timedelta_to_double(const clk_t& clk)
{
	double t = (clk.hour * 60 + clk.min) * 60 + clk.sec + clk.fsec;
	if (clk.tz_s == '-') {
		return -t;
	} else {
		return t;
	}
}


std::string
Datetime::timedelta_to_string(const clk_t& clk, bool trim)
{
	std::string res;
	if (clk.fsec > 0 || !trim) {
		res.resize(17);
		res.resize(snprintf(&res[0], 17, "%c%02d:%02d:%02d.%06d",
			clk.tz_s, clk.hour, clk.min, clk.sec, static_cast<int>(clk.fsec / DATETIME_MICROSECONDS)));
		if (trim) {
			auto it_e = res.end();
			auto it = it_e - 1;
			for (; *it == '0'; --it);
			if (*it != '.') ++it;
			res.erase(it, it_e);
		}
	} else {
		res.resize(10);
		res.resize(snprintf(&res[0], 10, "%c%02d:%02d:%02d",
			clk.tz_s, clk.hour, clk.min, clk.sec));
	}
	return res;
}


std::string
Datetime::timedelta_to_string(double t, bool trim)
{
	return timedelta_to_string(timedelta_to_clk_t(t), trim);
}


bool
Datetime::isTimedelta(string_view timedelta)
{
	auto size = timedelta.size();
	switch (size) {
		case 6: // [+-]00:00
			return (timedelta[0] == '+' || timedelta[0] == '-') && std::isdigit(timedelta[1]) && std::isdigit(timedelta[2]) &&
				timedelta[3] == ':' && std::isdigit(timedelta[4]) && std::isdigit(timedelta[5]);
		case 9: // [+-]00:00:00
			return (timedelta[0] == '+' || timedelta[0] == '-') && std::isdigit(timedelta[1]) && std::isdigit(timedelta[2]) &&
				timedelta[3] == ':' && std::isdigit(timedelta[4]) && std::isdigit(timedelta[5]) &&
				timedelta[6] == ':' && std::isdigit(timedelta[7]) && std::isdigit(timedelta[8]);
		default: // [+-]00:00:00  [+-]00:00:00.000...
			if (size > 10 && (timedelta[0] == '+' || timedelta[0] == '-') && std::isdigit(timedelta[1]) &&
				std::isdigit(timedelta[2]) && timedelta[3] == ':' && std::isdigit(timedelta[4]) &&
				std::isdigit(timedelta[5]) && timedelta[6] == ':' && std::isdigit(timedelta[7]) &&
				std::isdigit(timedelta[8]) && timedelta[9] == '.') {
				for (size_t i = 10; i < size; ++i) {
					if (!std::isdigit(timedelta[i])) {
						return false;
					}
				}
				return true;
			}
			return false;
	}
}
