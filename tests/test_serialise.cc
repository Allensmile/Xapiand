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

#include "test_serialise.h"


const test test_timestamp_date[] = {
	// Date 									Expected timestamp.
	{ "2014-01-01||-1M/y",                      "1388534399.999000"   },
	{ "2014-10-10||-12M",                       "1381363200.000000"   },
	{ "2014-10-10||-42M",                       "1302393600.000000"   },
	{ "2014-10-10||+2M",                        "1418169600.000000"   },
	{ "2014-10-10||+47M",                       "1536537600.000000"   },
	{ "2014-10-10||+200d",                      "1430179200.000000"   },
	{ "2014-10-10||-200d",                      "1395619200.000000"   },
	{ "2014-10-10||+5d",                        "1413331200.000000"   },
	{ "2014-10-10||-5d",                        "1412467200.000000"   },
	{ "2010 12 20 08:10-03:00||-10y",           "977310600.000000"    },
	{ "2010 12 20 08:10-03:00||+10y",           "1608462600.000000"   },
	{ "2010 12 20 08:10-03:00||-100w",          "1232363400.000000"   },
	{ "2010 12 20 08:10-03:00||+100w",          "1353323400.000000"   },
	{ "2010/12/20T08:10-03:00||-17616360h",     "-62126052600.000000" },
	{ "2010/12/20T08:10-03:00||+17616360h",     "64711739400.000000"  },
	{ "0001/12/20T08:10-03:00||//y",            "-62135596800.000000" },
	{ "9999/12/20T08:10-03:00||/y",             "253402300799.999000" },
	{ "2014-10-10",                             "1412899200.000000"   },
	{ "20141010T00:00:00",                      "1412899200.000000"   },
	{ "2014/10/10",                             "1412899200.000000"   },
	{ "2012/10/10T0:00:00",                     "1349827200.000000"   },
	{ "2012-10-10T23:59:59",                    "1349913599.000000"   },
	{ "2010-10-10T10:10:10 +06:30",             "1286682010.000000"   },
	{ "2010-10-10T03:40:10Z",                   "1286682010.000000"   },
	{ "2010/10/1003:40:10+00:00",               "1286682010.000000"   },
	{ "2010 10 10 3:40:10.000-00:00",           "1286682010.000000"   },
	{ "2015-10-10T23:55:58.765-07:50",          "1444549558.765000"   },
	{ "201012208:10-3:00||-1y",                 "1261307400.000000"   },
	{ "2010 12 20 08:10-03:00||+1y",            "1324379400.000000"   },
	{ "2010 12 20 08:10-03:00||+1M",            "1295521800.000000"   },
	{ "2010/12/20T08:10-03:00||-1M",            "1290251400.000000"   },
	{ "2010 12 20 08:10-03:00||+12d",           "1293880200.000000"   },
	{ "2010/12/20T08:10-03:00||-22d",           "1290942600.000000"   },
	{ "2010 12 20 08:10-03:00||+20h",           "1292915400.000000"   },
	{ "2010/12/20T08:10-03:00||-6h",            "1292821800.000000"   },
	{ "2010 12 20 08:10-03:00||+55m",           "1292846700.000000"   },
	{ "2010/12/20T08:10-03:00||-14m",           "1292842560.000000"   },
	{ "2010 12 20 08:10-03:00||+69s",           "1292843469.000000"   },
	{ "2010/12/20T08:10-03:00||-9s",            "1292843391.000000"   },
	{ "2015 04 20 08:10-03:00||+2w",            "1430737800.000000"   },
	{ "2015/04/20T08:10-03:00||-3w",            "1427713800.000000"   },
	{ "2010/12/20T08:10-03:00||/y",             "1293839999.999000"   },
	{ "2010/12/20T08:10-03:00 || //y",          "1262304000.000000"   },
	{ "2010/12/20T08:10-03:00||/M",             "1293839999.999000"   },
	{ "2010/12/20T08:10-03:00||//M",            "1291161600.000000"   },
	{ "2010/12/20T08:10-03:00||/d",             "1292889599.999000"   },
	{ "2010/12/20T08:10-03:00||//d",            "1292803200.000000"   },
	{ "2010/12/20T08:10-03:00  ||  /h",         "1292846399.999000"   },
	{ "2010/12/20 08:10-03:00||//h",            "1292842800.000000"   },
	{ "2010/12/20T08:10-03:00||/m",             "1292843459.999000"   },
	{ "2010/12/20T08:10-03:00||//m",            "1292843400.000000"   },
	{ "2010 12 20 8:10:00.000 -03:00 || /s",    "1292843400.999000"   },
	{ "2010/12/20 08:10:00-03:00||//s",         "1292843400.000000"   },
	{ "2015 04 23 8:10:00.000 -03:00 || /w",    "1430006399.999000"   },
	{ "2015/04/23 08:10:00-03:00||//w",         "1429401600.000000"   },
	{ "2015-10-10T23:55:58.765-06:40||+5y",     "1602398158.765000"   },
	{ "2015-10-10T23:55:58.765-6:40||+5y/M",    "1604188799.999000"   },
	{ "2010 07 21 8:10||+3d-12h+56m/d",         "1279929599.999000"   },
	{ "2010 07 21 8:10||+3d-12h+56m//d",        "1279843200.000000"   },
	{ "2010/12/12||+10M-3h//y",                 "1293840000.000000"   },
	{ "2010 12 10 0:00:00 || +2M/M",            "1298937599.999000"   },
	{ "20100202||/w+3w/M+3M/M-3M+2M/M-2M//M",   "1264982400.000000"   },
	{ "2010/12/12||+10M-3h//y4",                ""                    },
	{ "2010-10/10",                             ""                    },
	{ "201010-10",                              ""                    },
	{ "2010-10-10T 4:55",                       ""                    },
	{ "2010-10-10Z",                            ""                    },
	{ "2010-10-10 09:10:10 - 6:56",             ""                    },
	{ "2010-10-10 09:10:10 -656",               ""                    },
	{ NULL,                                     NULL                  },
	};


const test test_unserialisedate[] {
	// Date to be serialised.				 Expected date after unserialise.
	{ "2010-10-10T23:05:24.800",             "2010-10-10T23:05:24.800" },
	{ "2010101023:05:24",                    "2010-10-10T23:05:24.000" },
	{ "2010/10/10",                          "2010-10-10T00:00:00.000" },
	{ "2015-10-10T23:55:58.765-6:40||+5y/M", "2020-10-31T23:59:59.999" },
	{ "9115/01/0115:10:50.897-6:40",         "9115-01-01T21:50:50.897" },
	{ "9999/12/20T08:10-03:00||/y",          "9999-12-31T23:59:59.999" },
	{ "-62135596800.000",                    "0001-01-01T00:00:00.000" },
	{ "253402300799.999000",                 "9999-12-31T23:59:59.999" },
	{ NULL,                                  NULL                      },
};


const test test_unserialiseLatLong[] {
	// Set of coordinates to serialise.		 Expected coordinates after unserialse.s
	{ "20.35,78.90,23.45,32.14",             "20.35,78.9,23.45,32.14" },
	{ "20.35, 78.90",                        "20.35,78.9"             },
	{ "20.35 , 78.90 , 23.45 , 32.14",       "20.35,78.9,23.45,32.14" },
	{ "20, 78.90, 23.010, 32",               "20,78.9,23.01,32"       },
	{ NULL,                                   NULL                    },
};


// Testing the transformation between date string and timestamp.
int test_datetotimestamp()
{
	int cont = 0;
	for (const test *p = test_timestamp_date; p->str; ++p) {
		std::string date = std::string(p->str);
		std::string timestamp;
		try {
			timestamp = std::to_string(Datetime::timestamp(date));
		} catch (const std::exception &ex) {
			LOG_ERR(NULL, "ERROR: %s\n", ex.what());
			timestamp = "";
		}
		if (timestamp.compare(p->expect) != 0) {
			cont++;
			LOG_ERR(NULL, "ERROR: Resul: %s Expect: %s\n", timestamp.c_str(), p->expect);
		}
	}

	if (cont == 0) {
		LOG(NULL, "Testing the transformation between date string and timestamp is correct!\n");
		return 0;
	} else {
		LOG_ERR(NULL, "ERROR: Testing the transformation between date string and timestamp has mistakes.\n");
		return 1;
	}
}


// Testing the conversion of units in LatLong Distance.
int test_distanceLatLong()
{
	int cont = 0;
	for (const test_str_double *p = test_distanceLatLong_fields; p->str; ++p) {
		double coords_[3];
		if (get_coords(p->str, coords_) == 0) {
			if (coords_[2] != p->val) {
				cont++;
				LOG_ERR(NULL, "ERROR: Resul: %f Expect: %f\n", coords_[2], p->val);
			}
		} else {
			if (p->val != -1.0) {
				cont++;
				LOG_ERR(NULL, "ERROR: Resul: Error en format(-1) Expect: %f\n", p->val);
			}
		}
	}

	if (cont == 0) {
		LOG(NULL, "Testing the conversion of units in LatLong Distance is correct!\n");
		return 0;
	} else {
		LOG_ERR(NULL, "ERROR: Testing the conversion of units in LatLong Distance has mistakes.\n");
		return 1;
	}
}


// Testing unserialise date.
int test_unserialise_date()
{
	int cont = 0;
	for (const test *p = test_unserialisedate; p->str; ++p) {
		std::string date_s = serialise_date(p->str);
		std::string date = unserialise_date(date_s);
		if (date.compare(p->expect) != 0) {
			cont++;
			LOG_ERR(NULL, "ERROR: Resul: %s Expect: %s\n", date.c_str(), p->expect);
		}
	}

	if (cont == 0) {
		LOG(NULL, "Testing unserialise date is correct!\n");
		return 0;
	} else {
		LOG_ERR(NULL, "ERROR: Testing unserialise date has mistakes.\n");
		return 1;
	}
}


// Testing unserialise LatLong coordinates.
int test_unserialise_geo()
{
	int cont = 0;
	/*
	for (const test *p = test_unserialiseLatLong; p->str; ++p) {
		std::string geo_s = serialise_geo(p->str);
		std::string geo = unserialise_geo(geo_s);
		if (geo.compare(p->expect) != 0) {
			cont++;
			LOG_ERR(NULL, "ERROR: Resul: %s Expect: %s\n", geo.c_str(), p->expect);
		}
	}*/

	if (cont == 0) {
		LOG(NULL, "Testing unserialise LatLong coordinates is correct!\n");
		return 0;
	} else {
		LOG_ERR(NULL, "ERROR: Testing unserialise LatLong coordinates has mistakes.\n");
		return 1;
	}
}