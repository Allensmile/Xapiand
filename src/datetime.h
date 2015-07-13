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

#ifndef XAPIAND_INCLUDED_DATETIME_H
#define XAPIAND_INCLUDED_DATETIME_H

#include <sys/time.h>
#include <iostream>
#include "utils.h"
#include <pcre.h>

#define DATE_RE "([0-9]{4})([-/ ]?)(0[1-9]|1[0-2])\\2(0[0-9]|[12][0-9]|3[01])([T ]?([01]?[0-9]|2[0-3]):([0-5][0-9])(:([0-5][0-9])([.,]([0-9]{1,3}))?)?([ ]*[+-]([01]?[0-9]|2[0-3]):([0-5][0-9])|Z)?)?([ ]*\\|\\|[ ]*([+-/\\dyMwdhms]+))?"
#define DATE_MATH_RE "([+-]\\d+|\\/{1,2})([dyMwhms])"

#define _EPOCH      1970
#define _START_YEAR 1900
#define _EPOCH_ORD  719163  /* toordinal(_EPOCH, 1, 1) */


namespace Datetime {
	typedef struct tm_s {
		int year;
		int mon;
		int day;
		int hour;
		int min;
		int sec;
		int msec;
	} tm_t;

	extern pcre *compiled_date_re;
	extern pcre *compiled_date_math_re;

	extern void dateTimeParser(const std::string &date, tm_t &tm);
	extern void computeDateMath(tm_t &tm, const std::string &op, const std::string &units);
	extern bool isleapYear(int year);
	extern bool isleapRef_year(int tm_year);
	extern int getDays_month(int year, int month);
	extern time_t toordinal(int year, int month, int day);
	extern time_t timegm(struct tm *tm);
	extern time_t timegm(tm_t &tm);
	extern long double mtimegm(tm_t &tm);
	extern long double timestamp(const std::string &date);
	extern bool isvalidDate(int year, int month, int day);
	extern char* isotime(const struct tm *timep);
	extern ::std::string ctime(const ::std::string &epoch);
	extern void normalizeMonths(int &year, int &mon);
};


#endif /* XAPIAND_INCLUDED_DATETIME_H */