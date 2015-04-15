/*
 * Copyright deipi.com LLC and contributors. All rights reserved.
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

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include "pthread.h"
#include "utils.h"
#include <xapian.h>

#define DATE_RE "(([1-9][0-9]{3})-(0[1-9]|1[0-2])-(0[1-9]|[12][0-9]|3[01])(T([01][0-9]|2[0-3]):([0-5][0-9])(:([0-5][0-9])(\\.([0-9]{3}))?)?(([+-])([01][0-9]|2[0-3])(:([0-5][0-9]))?)?)?)"
#define COORDS_RE "(\\d*\\.\\d+|\\d+)\\s?,\\s?(\\d*\\.\\d+|\\d+)"
#define COORDS_DISTANCE_RE "(\\d*\\.\\d+|\\d+)\\s?,\\s?(\\d*\\.\\d*|\\d+)\\s?;\\s?(\\d*\\.\\d*|\\d+)"
#define NUMERIC_RE "(\\d*\\.\\d+|\\d+)"
#define FIND_RANGE_RE "([^ ]*\\.\\.)"
#define FIND_ORDER_RE "([_a-zA-Z][_a-zA-Z0-9]+,[_a-zA-Z][_a-zA-Z0-9]*)"


pthread_mutex_t qmtx = PTHREAD_MUTEX_INITIALIZER;
pcre *compiled_date_re = NULL;
pcre *compiled_coords_re = NULL;
pcre *compiled_coords_dist_re = NULL;
pcre *compiled_numeric_re = NULL;
pcre *compiled_find_range_re = NULL;


std::string repr(const std::string &string)
{
	return repr(string.c_str(), string.size());
}


std::string repr(const char * p, size_t size)
{
	char *buff = new char[size * 4 + 1];
	char *d = buff;
	const char *p_end = p + size;
	while (p != p_end) {
		char c = *p++;
		if (c == 9) {
			*d++ = '\\';
			*d++ = 't';
		} else if (c == 10) {
			*d++ = '\\';
			*d++ = 'n';
		} else if (c == 13) {
			*d++ = '\\';
			*d++ = 'r';
		} else if (c == '\'') {
			*d++ = '\\';
			*d++ = '\'';
		} else if (c >= ' ' && c <= '~') {
			*d++ = c;
		} else {
			*d++ = '\\';
			*d++ = 'x';
			sprintf(d, "%02x", (unsigned char)c);
			d += 2;
		}
		//printf("%02x: %ld < %ld\n", (unsigned char)c, (unsigned long)(d - buff), (unsigned long)(size * 4 + 1));
	}
	*d = '\0';
	std::string ret(buff);
	delete [] buff;
	return ret;
}


void log(void *obj, const char *format, ...)
{
	pthread_mutex_lock(&qmtx);

	FILE * file = stderr;
	pthread_t thread = pthread_self();
	char name[100];
	pthread_getname_np(thread, name, sizeof(name));
	fprintf(file, "tid(0x%lx:%s): 0x%.12lx - ", (unsigned long)thread, name, (unsigned long)obj);
	va_list argptr;
	va_start(argptr, format);
	vfprintf(file, format, argptr);
	va_end(argptr);

	pthread_mutex_unlock(&qmtx);
}

const char HEX2DEC[256] =
{
	/*       0  1  2  3   4  5  6  7   8  9  A  B   C  D  E  F */
	/* 0 */ -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
	/* 1 */ -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
	/* 2 */ -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
	/* 3 */  0, 1, 2, 3,  4, 5, 6, 7,  8, 9,-1,-1, -1,-1,-1,-1,
	
	/* 4 */ -1,10,11,12, 13,14,15,-1, -1,-1,-1,-1, -1,-1,-1,-1,
	/* 5 */ -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
	/* 6 */ -1,10,11,12, 13,14,15,-1, -1,-1,-1,-1, -1,-1,-1,-1,
	/* 7 */ -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
	
	/* 8 */ -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
	/* 9 */ -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
	/* A */ -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
	/* B */ -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
	
	/* C */ -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
	/* D */ -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
	/* E */ -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
	/* F */ -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1
};


std::string urldecode(const char *src, size_t size)
{
	// Note from RFC1630:  "Sequences which start with a percent sign
	// but are not followed by two hexadecimal characters (0-9, A-F) are reserved
	// for future extension"
	
	const char * SRC_END = src + size;
	const char * SRC_LAST_DEC = SRC_END - 2;   // last decodable '%'
	
	char * const pStart = new char[size];
	char * pEnd = pStart;
	
	while (src < SRC_LAST_DEC)
	{
		if (*src == '%')
		{
			char dec1, dec2;
			if (-1 != (dec1 = HEX2DEC[*(src + 1)])
				&& -1 != (dec2 = HEX2DEC[*(src + 2)]))
			{
				*pEnd++ = (dec1 << 4) + dec2;
				src += 3;
				continue;
			}
		}
		
		*pEnd++ = *src++;
	}
	
	// the last 2- chars
	while (src < SRC_END)
	*pEnd++ = *src++;
	
	std::string sResult(pStart, pEnd);
	delete [] pStart;
	//std::replace( sResult.begin(), sResult.end(), '+', ' ');
	return sResult;
}


int url_qs(const char *name, const char *qs, size_t size, parser_query *par)
{
	const char *nf = qs + size;
	const char *n1, *n0;
	const char *v0 = NULL;
	
	if(par->offset == NULL) {
		n0 = n1 = qs;
	} else {
		n0 = n1 = par->offset + par -> length + 1;
	}
	
	while (1) {
		char cn = *n1;
		if (n1 == nf) {
			cn = '\0';
		}
		switch (cn) {
			case '=' :
			v0 = n1;
			case '\0':
			case '&' :
			case ';' :
			if(strlen(name) == n1 - n0 && strncmp(n0, name, n1 - n0) == 0) {
				if (v0) {
					const char *v1 = v0 + 1;
					while (1) {
						char cv = *v1;
						if (v1 == nf) {
							cv = '\0';
						}
						switch(cv) {
							case '\0':
							case '&' :
							case ';' :
							par->offset = v0 + 1;
							par->length = v1 - v0 - 1;
							return 0;
						}
						v1++;
					}
				} else {
					par->offset = n1 + 1;
					par->length = 0;
					return 0;
				}
			} else if (!cn) {
				return -1;
			} else if (cn != '=') {
				n0 = n1 + 1;
				v0 = NULL;
			}
		}
		n1++;
	}
	return -1;
}


int url_path(const char* n1, size_t size, parser_url_path *par)
{
	const char *nf = n1 + size + 1;
	const char *n0, *n2 ,*r, *p = NULL;
	size_t cmd_size = 0;
	int state = 0;
	n0 = n1;
	
	bool other_slash = false;
	par->off_host = NULL;
	par->len_host = 0;
	par->off_command = NULL;
	par->len_command = 0;
	
	
	if(par->offset == NULL) {
		n0 = n2 = n1;
	} else {
		n0 = n2 = n1 = par->offset + par -> length + 1;
	}
	
	par->length = 0;
	par->offset = 0;
	
	while (1) {
		char cn = *n1;
		if (n1 == nf) {
			cn = '\0';
		}
		switch(cn) {
			case '\0':
				if (n0 == n1) return -1;
				if (p) {
					r = p + 1;
					while(1) {
						char cr = *r;
						if (r == nf) {
							cr = '\0';
						}
						if (!cr) break;
						switch (cr) {
							case '/':
								r++;
								continue;
								
							default:
								cmd_size++;
								r++;
								break;
						}
					}
					par->off_command = p + 1;
					par->len_command = cmd_size;
					par->offset = n2;
					par->length = r - n2;
				}
			case ',':
				if (!p) p = n1;
				switch (state) {
					case 0:
					case 1:
						par->off_path = n0;
						par->len_path = p - n0;
						if (cn) n1++;
						if(!par->length) {
							par->offset = n2;
							par->length = p - n2;
						}
						
						return 0;
					case 2:
						par->off_host = n0;
						par->len_host = p - n0;
						if (cn) n1++;
						if(!par->length) {
							par->offset = n2;
							par->length = p - n2;
						}
						return 0;
				}
				p = NULL;
				other_slash = false;
				break;
				
			case ':':
				switch (state) {
					case 0:
						par->off_namespace = n0;
						par->len_namespace = n1 - n0;
						state = 1;
						n0 = n1 + 1;
						break;
					default:
						state = -1;
				}
				p = NULL;
				other_slash = false;
				break;
				
			case '@':
				switch (state) {
					case 0:
						par->off_path = n0;
						par->len_path = n1 - n0;
						state = 2;
						n0 = n1 + 1;
						break;
					case 1:
						par->off_path = n0;
						par->len_path = n1 - n0;
						state = 2;
						n0 = n1 + 1;
						break;
					default:
						state = -1;
				}
				p = NULL;
				other_slash = false;
				break;
				
				
			case '/':
				if (*(n1 + 1) && !p && !other_slash) {
					p = n1;
					other_slash = true;
				} else if(*(n1 + 1) && *(n1 + 1) != '/') {
					p = n1;
					other_slash = true;
				}
				
		}
		n1++;
	}
	return -1;
}


int pcre_search(const char *subject, int length, int startoffset, int options, const char *pattern, pcre **code, group **groups)
{
	int erroffset;
	const char *error;

	// First, the regex string must be compiled.
	if (*code == NULL) {
		//pcre_free is not use because we use a struct pcre static and gets free at the end of the program
		LOG(NULL, "pcre compiled is NULL.\n");
		*code = pcre_compile(pattern, 0, &error, &erroffset, 0);
		if (*code == NULL) {
			LOG_ERR(NULL, "pcre_compile of %s failed (offset: %d), %s\n", pattern, erroffset, error);
			return -1;
		}
	}

	if (*code != NULL) {
		int n;
		if (pcre_fullinfo(*code, NULL, PCRE_INFO_CAPTURECOUNT, &n) != 0) {
			return -1;
		}

		if (*groups == NULL) {
			*groups = (group *)malloc((n + 1) * 3 * sizeof(int));
		}

		int *ocvector = (int *)*groups;
		if (pcre_exec(*code, 0, subject, length, startoffset, options, ocvector, (n + 1) * 3) >= 0) {
			return 0;
		} else return -1;
	}

	return -1;
}


int field_type(const std::string &field_name)
{
	if (field_name.size() < 2 || field_name.at(1) != '_') {
		return 1; //default: str.
	}

	switch (field_name.at(0)) {
		case NUMERIC_PREFIX: return NUMERIC_TYPE;
		case STRING_PREFIX: return STRING_TYPE;
		case DATE_PREFIX: return DATE_TYPE;
		case GEO_PREFIX: return GEO_TYPE;
		case BOOLEAN_PREFIX: return BOOLEAN_TYPE;
		default: return STRING_TYPE;
	}
}


std::string serialise_numeric(const std::string &field_value)
{
	double val;
	if (isNumeric(field_value)) {
		val = strtodouble(field_value);
		return Xapian::sortable_serialise(val);
	}
	return std::string("");
}


std::string serialise_date(const std::string &field_value)
{
	std::string str_timestamp = timestamp_date(field_value);
	if (str_timestamp.size() == 0) {
		LOG_ERR(NULL, "ERROR: Format date (%s) must be ISO 8601: (eg 1997-07-16T19:20:30.451+05:00) or a epoch (double)\n", field_value.c_str());
		return std::string("");
	}

	double timestamp = strtodouble(str_timestamp);
	LOG(NULL, "timestamp %s %f\n", str_timestamp.c_str(), timestamp);
	return Xapian::sortable_serialise(timestamp);
}


std::string serialise_geo(const std::string &field_value)
{
	Xapian::LatLongCoords coords;
	double latitude, longitude;
	int len = (int) field_value.size(), Ncoord = 0, offset = 0;
	bool end = false;
	group *g = NULL;
	while (pcre_search(field_value.c_str(), len, offset, 0, COORDS_RE, &compiled_coords_re, &g) != -1) {
		std::string parse(field_value, g[1].start, g[1].end - g[1].start);
		latitude = strtodouble(parse);
		parse = std::string(field_value, g[2].start, g[2].end - g[2].start);
		longitude = strtodouble(parse);
		Ncoord++;
		try {
			coords.append(Xapian::LatLongCoord(latitude, longitude));
		} catch (Xapian::Error &e) {
			LOG_ERR(NULL, "Latitude or longitude out-of-range\n");
			return std::string("");
		}
		LOG(NULL, "Coord %d: %f, %f\n", Ncoord, latitude, longitude);
		if (g[2].end == len) {
			end = true;
			break;
		}
		offset = g[2].end;
	}
	
	if (g) {
		free(g);
		g = NULL;
	}

	if (Ncoord == 0 || !end) {
		LOG_ERR(NULL, "ERROR: %s must be an array of doubles [lat, lon, lat, lon, ...]\n", field_value.c_str());
		return std::string("");
	}
	return coords.serialise();
}


std::string serialise_bool(const std::string &field_value)
{
	if (!field_value.c_str()) {
		return std::string("f");
	} else if(field_value.size() > 1) {
		if (strcasecmp(field_value.c_str(), "TRUE") == 0) {
			return std::string("t");
		} else if (strcasecmp(field_value.c_str(), "FALSE") == 0) {
			return std::string("f");
		} else {
			return std::string("t");
		}
	} else {
		switch (tolower(field_value.at(0))) {
			case '1':
				return std::string("t");
			case '0':
				return std::string("f");
			case 't':
				return std::string("t");
			case 'f':
				return std::string("f");
			default:
				return std::string("t");
		}
	}
}


std::string stringtoupper(const std::string &str)
{
	std::string tmp = str;

	struct TRANSFORM {
		char operator() (char c) { return  toupper(c);}
	};

	std::transform(tmp.begin(), tmp.end(), tmp.begin(), TRANSFORM());

	return tmp;
}


std::string stringtolower(const std::string &str)
{
	std::string tmp = str;

	struct TRANSFORM {
		char operator() (char c) { return  tolower(c);}
	};

	std::transform(tmp.begin(), tmp.end(), tmp.begin(), TRANSFORM());

	return tmp;
}


std::string prefixed(const std::string &term, const std::string &prefix)
{
	if (isupper(term.at(0))) {
		return prefix + ":" + term;
	}

	return prefix + term;
}


unsigned int get_slot(const std::string &name)
{
	if (stringtolower(name).compare("id") == 0) return 0;

	std::string standard_name;
	if (strhasupper(name)) {
		standard_name = stringtoupper(name);
	} else {
		standard_name = name;
	}
	std::string _md5 = std::string(md5(standard_name), 24, 8);
	unsigned int slot = hex2int(_md5);
	if (slot == 0x00000000) {
		slot = 0x00000001;
	} else if (slot == 0xffffffff) {
		slot = 0xfffffffe;
	}
	return slot;
}


unsigned int hex2int(const std::string &input)
{
	unsigned int n;
	std::stringstream ss;
	ss << std::hex << input;
	ss >> n;
	ss.flush();
	return n;
}


int strtoint(const std::string &str)
{
	int number;
	std::stringstream ss;
	ss << std::dec << str;
	ss >> number;
	ss.flush();
	return number;
}


double strtodouble(const std::string &str)
{
	double number;
	std::stringstream ss;
	ss << std::dec << str;
	ss >> number;
	ss.flush();
	return number;
}


std::string timestamp_date(const std::string &str)
{
	int len = (int) str.size();
	char sign;
	int ret, n[9];
	double  timestamp;
	group *g = NULL;
	
	ret = pcre_search(str.c_str(), len, 0, 0, DATE_RE, &compiled_date_re, &g);

	if (ret != -1 && len == (g[0].end - g[0].start)) {
		std::string parse = std::string(str, g[2].start, g[2].end - g[2].start);
		n[0] = strtoint(parse);
		parse = std::string(str, g[3].start, g[3].end - g[3].start);
		n[1] = strtoint(parse);
		parse = std::string(str, g[4].start, g[4].end - g[4].start);
		n[2] = strtoint(parse);

		if (g[5].end - g[5].start > 0) {
			parse = std::string(str, g[6].start, g[6].end - g[6].start);
			n[3] = strtoint(parse);
			parse = std::string(str, g[7].start, g[7].end - g[7].start);
			n[4] = strtoint(parse);
			if (g[8].end - g[8].start > 0) {
				parse = std::string(str, g[9].start, g[9].end - g[9].start);
				n[5] = strtoint(parse);
				if (g[10].end - g[10].start > 0) {
					parse = std::string(str, g[11].start, g[11].end - g[11].start);
					n[6] = strtoint(parse);
				} else {
					n[6] = 0;
				}
			} else {
				n[5] =  n[6] = 0;
			}
			if (g[12].end - g[12].start > 0) {
				sign = std::string(str, g[13].start, g[13].end - g[13].start).at(0);
				parse = std::string(str, g[14].start, g[14].end - g[14].start);
				n[7] = strtoint(parse);
				if (g[15].end - g[15].start > 0) {   
					parse = std::string(str, g[16].start, g[16].end - g[16].start);
					n[8] = strtoint(parse); 
				} else {
					n[8] = 0;
				}
			} else {
				n[7] = 0;
				n[8] = 0;
				sign = '+';
			}
		} else {
			n[3] = n[4] = n[5] = n[6] = n[7] = n[8] = 0;
		}
		LOG(NULL, "Fecha Reconstruida: %04d-%02d-%02dT%02d:%02d:%02d.%03d%c%02d:%02d\n", n[0], n[1], n[2], n[3], n[4], n[5], n[6], sign, n[7], n[8]);
		if (n[1] == 2 && !((n[0] % 4 == 0 && n[0] % 100 != 0) || n[0] % 400 == 0) && n[2] > 28) {
			LOG_ERR(NULL, "ERROR: Incorrect Date, This month only has 28 days\n");
			return std::string("");
		} else if(n[1] == 2 && ((n[0] % 4 == 0 && n[0] % 100 != 0) || n[0] % 400 == 0) && n[2] > 29) {
			LOG_ERR(NULL, "ERROR: Incorrect Date, This month only has 29 days\n");
			return std::string("");
		} else if((n[1] == 4 || n[1] == 6 || n[1] == 9 || n[1] == 11) && n[2] > 30) {
			LOG_ERR(NULL, "ERROR: Incorrect Date, This month only has 30 days\n");
			return std::string("");
		}
		time_t tt = 0;
		struct tm *timeinfo = gmtime(&tt);
		timeinfo->tm_year   = n[0] - 1900;
		timeinfo->tm_mon    = n[1] - 1;
		timeinfo->tm_mday   = n[2]; 
		if (sign == '-') {
			timeinfo->tm_hour  = n[3] + n[7];
			timeinfo->tm_min   = n[4] + n[8];
		} else {
			timeinfo->tm_hour  = n[3] - n[7];
			timeinfo->tm_min   = n[4] - n[8];
		}
		timeinfo->tm_sec    = n[5];
		const time_t dateGMT = timegm(timeinfo);
		timestamp = (double) dateGMT;
		timestamp += n[6]/1000.0;
		
		if (g) {
			free(g);
			g = NULL;
		}
		return std::to_string(timestamp);
	}

	if (g) {
		free(g);
		g = NULL;
	}

	if (isNumeric(str)) {
		return str;
	}

	return std::string("");
}


std::string get_prefix(const std::string &name, const std::string &prefix)
{
	std::string slot = get_slot_hex(name);

	struct TRANSFORM {
		char operator() (char c) { return  c + 17;}
	};

	std::transform(slot.begin(), slot.end(), slot.begin(), TRANSFORM());

	return prefix + slot;
}


std::string get_slot_hex(const std::string &name)
{
	std::string standard_name;
	if (strhasupper(name)) {
		standard_name = stringtoupper(name);
	} else {
		standard_name = name;
	}
	std::string _md5 = std::string(md5(standard_name), 24, 8);
	return stringtoupper(_md5);
}


void print_hexstr(const std::string &str)
{
	std::string::const_iterator it(str.begin());
	printf("0x");
	for ( ; it != str.end(); it++) {
		printf("%.2x", (unsigned char)*it);
	}
	printf("\n");
}


bool strhasupper(const std::string &str)
{
	std::string::const_iterator it(str.begin());
	for ( ; it != str.end(); it++) {
		if (isupper(*it)) return true;
	}

	return false;
}


int get_coords(const std::string &str, double *coords)
{
	std::stringstream ss;
	group *g = NULL;
	int offset = 0;
	
	while ((pcre_search(str.c_str(), (int)str.size(), offset, 0, COORDS_DISTANCE_RE, &compiled_coords_dist_re, &g)) != -1) {
		offset = g[0].end;
		/*LOG(NULL,"group[1] %s\n" , std::string(str.c_str() + g[1].start, g[1].end - g[1].start).c_str());
		 LOG(NULL,"group[2] %s\n" , std::string(str.c_str() + g[2].start, g[2].end - g[2].start).c_str());
		 LOG(NULL,"group[3] %s\n" , std::string(str.c_str() + g[3].start, g[3].end - g[3].start).c_str());*/
		ss.clear();
		ss << std::string(str.c_str() + g[1].start, g[1].end - g[1].start);
		ss >> coords[0];
		ss.clear();
		ss << std::string(str.c_str() + g[2].start, g[2].end - g[2].start);
		ss >> coords[1];
		ss.clear();
		ss << std::string(str.c_str() + g[3].start, g[3].end - g[3].start);
		ss >> coords[2];
		return 0;
	}
	return -1;
}


bool isRange(const std::string &str)
{
	group *gr = NULL;
	int len = (int)str.size();
	int ret = pcre_search(str.c_str(), (int)str.size(), 0, 0, FIND_RANGE_RE, &compiled_find_range_re , &gr);
	if (ret != -1) {
		if (gr) {
			free(gr);
			gr = NULL;
		}
		return true;
	}
	return false;
}


bool isLatLongDistance(const std::string &str)
{
	group *gr = NULL;
	int len = (int)str.size();
	int ret = pcre_search(str.c_str(), len, 0, 0, COORDS_DISTANCE_RE, &compiled_coords_dist_re, &gr);
	if (ret != -1 && (gr[0].end - gr[0].start) == len) {
		if (gr) {
			free(gr);
			gr = NULL;
		}
		return true;
	}
	return false;
}


bool isNumeric(const std::string &str)
{
	group *g = NULL;
	int len = (int)str.size();
	int ret = pcre_search(str.c_str(), len, 0, 0, NUMERIC_RE, &compiled_numeric_re, &g);
	if (ret != -1 && (g[0].end - g[0].start) == len) {
		if (g) {
			free(g);
			g = NULL;
		}
		return true;
	}
	return false;
}

bool StartsWith(const std::string &text, const std::string &token)
{
	if (text.length() < token.length())
		return false;
	return (text.compare(0, token.length(), token) == 0);
}