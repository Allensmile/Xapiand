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

#include "wkt_parser.h"

#include <algorithm>               // for move

#include "geo/cartesian.h"         // for Cartesian, CartesianUnits, Cartesi...
#include "geo/htm.h"               // for HTM, HTM_MAX_LEVEL
#include "split.h"                 // for Split
#include "stl_serialise.h"         // for CartesianUSet, RangeList
#include "utils.h"                 // for stox


const std::regex find_geometry_re("(SRID[\\s]*=[\\s]*([0-9]{4})[\\s]*\\;[\\s]*)?(POLYGON|MULTIPOLYGON|CIRCLE|MULTICIRCLE|POINT|MULTIPOINT|CHULL|MULTICHULL)[\\s]*\\(([()0-9.\\s,-]*)\\)|(GEOMETRYCOLLECTION|GEOMETRYINTERSECTION)[\\s]*\\(([()0-9.\\s,A-Z-]*)\\)", std::regex::optimize);
const std::regex find_circle_re("(\\-?\\d*\\.\\d+|\\-?\\d+)\\s(\\-?\\d*\\.\\d+|\\-?\\d+)(\\s(\\-?\\d*\\.\\d+|\\-?\\d+))?[\\s]*\\,[\\s]*(\\d*\\.\\d+|\\d+)", std::regex::optimize);
const std::regex find_subpolygon_re("[\\s]*(\\(([\\-?\\d*\\.\\d+\\s,]*|[\\-?\\d+\\s,]*)\\))[\\s]*(\\,)?", std::regex::optimize);
const std::regex find_multi_poly_re("[\\s]*[\\s]*\\((.*?\\))\\)[\\s]*(,)?", std::regex::optimize);
const std::regex find_multi_circle_re("[\\s]*[\\s]*\\((.*?)\\)[\\s]*(,)?", std::regex::optimize);
const std::regex find_collection_re("[\\s]*(POLYGON|MULTIPOLYGON|CIRCLE|MULTICIRCLE|POINT|MULTIPOINT|CHULL|MULTICHULL)[\\s]*\\(([()0-9.\\s,-]*)\\)([\\s]*\\,[\\s]*)?", std::regex::optimize);


/*
 * Parser for EWKT (A PostGIS-specific format that includes the spatial reference system identifier (SRID))
 * Geometric objects WKT supported:
 *  POINT
 *  MULTIPOINT
 *  POLYGON       // Polygon should be convex. Otherwise it should be used CHULL.
 *  MULTIPOLYGON
 *  GEOMETRYCOLLECTION
 *
 * Geometric objects not defined in wkt supported, but defined here by their relevance:
 *  GEOMETRYINTERSECTION
 *  CIRCLE
 *  MULTICIRCLE
 *  CHULL  			// Convex Hull from a points' set.
 *  MULTICHULL
 *
 * Coordinates for geometries may be:
 * (lat lon) or (lat lon height)
 *
 * This parser do not accept EMPTY geometries and
 * polygons are not required to be repeated first coordinate to end like EWKT.
*/
EWKT_Parser::EWKT_Parser(const std::string& EWKT, bool _partials, double _error)
	: error(_error),
	  partials(_partials)
{
	std::smatch m;
	if (std::regex_match(EWKT, m, find_geometry_re) && static_cast<size_t>(m.length(0)) == EWKT.size()) {
		if (m.length(2) != 0) {
			SRID = std::stoi(m.str(2));
			if (!Cartesian::is_SRID_supported(SRID)) {
				THROW(EWKTError, "SRID = %d is not supported", SRID);
			}
		} else {
			SRID = WGS84; // WGS84 default.
		}
		if (m.length(5) != 0) {
			std::string geometry(m.str(5));
			if (geometry == "GEOMETRYCOLLECTION") {
				trixels = parse_geometry_collection(m.str(6));
			} else if (geometry == "GEOMETRYINTERSECTION") {
				trixels = parse_geometry_intersection(m.str(6));
			}
		} else {
			std::string geometry(m.str(3));
			if (geometry == "CIRCLE") {
				trixels = parse_circle(m.str(4));
			} else if (geometry == "MULTICIRCLE") {
				trixels = parse_multicircle(m.str(4));
			} else if (geometry == "POLYGON") {
				trixels = parse_polygon(m.str(4), GeometryType::CONVEX_POLYGON);
			} else if (geometry == "MULTIPOLYGON") {
				trixels = parse_multipolygon(m.str(4), GeometryType::CONVEX_POLYGON);
			} else if (geometry == "CHULL") {
				trixels = parse_polygon(m.str(4), GeometryType::CONVEX_HULL);
			} else if (geometry == "MULTICHULL") {
				trixels = parse_multipolygon(m.str(4), GeometryType::CONVEX_HULL);
			} else if (geometry == "POINT") {
				trixels = parse_point(m.str(4));
			} else if (geometry == "MULTIPOINT") {
				trixels = parse_multipoint(m.str(4));
			}
		}
	} else {
		THROW(EWKTError, "Syntax error in %s, format or geometry object not supported", EWKT.c_str());
	}
}


/*
 * The specification is: lat lon [height], radius(double positive)
 * lat and lon in degrees.
 * height and radius in meters.
 *
 * Returns the trixels that cover the region.
 */
std::vector<std::string>
EWKT_Parser::parse_circle(const std::string& specification)
{
	std::smatch m;
	if (std::regex_match(specification, m, find_circle_re) && static_cast<size_t>(m.length(0)) == specification.size()) {
		double lat = stox(std::stod, m.str(1));
		double lon = stox(std::stod, m.str(2));
		double h = m.length(4) > 0 ? stox(std::stod, m.str(4)) : 0;
		HTM _htm(partials, error, Geometry(Constraint(Cartesian(lat, lon, h, CartesianUnits::DEGREES, SRID), stox(std::stod, m.str(5)))));
		_htm.run();

		centroids.insert(_htm.region.centroid);

		gv.push_back(std::move(_htm.region));

		return _htm.names;
	} else {
		THROW(EWKTError, "The specification for CIRCLE is lat lon [height], radius in meters(double positive)");
	}
}


/*
 * The specification is: (lat lon [height], radius), ... (lat lon [height], radius)
 * lat and lon in degrees.
 * height and radius in meters.
 *
 * Returns the trixels that cover the region.
 */
std::vector<std::string>
EWKT_Parser::parse_multicircle(const std::string& specification)
{
	// Checking if the format is correct and circles are procesed.
	std::vector<std::string> names_f;
	bool first = true;
	size_t match_size = 0;

	std::sregex_iterator next(specification.begin(), specification.end(), find_multi_circle_re, std::regex_constants::match_continuous);
	std::sregex_iterator end;
	while (next != end) {
		match_size += next->length(0);
		if (first) {
			names_f = parse_circle(next->str(1));
			first = false;
		} else {
			std::vector<std::string> txs = parse_circle(next->str(1));
			or_trixels(names_f, std::move(txs));
		}
		++next;
	}

	if (match_size != specification.size()) {
		THROW(EWKTError, "Syntax error in EWKT format (MULTICIRCLE)");
	}

	return names_f;
}


/*
 * The specification is (lat lon [height], ..., lat lon [height]), (lat lon [height], ..., lat lon [height]), ...
 * lat and lon in degrees.
 * height in meters.
 *
 * Returns the trixels that cover the region.
 */
std::vector<std::string>
EWKT_Parser::parse_polygon(const std::string& specification, GeometryType type)
{
	// Checking if the format is correct and subpolygons are procesed.
	std::vector<std::string> names_f;
	bool first = true;
	size_t match_size = 0;

	std::sregex_iterator next(specification.begin(), specification.end(), find_subpolygon_re, std::regex_constants::match_continuous);
	std::sregex_iterator end;
	while (next != end) {
		match_size += next->length(0);
		// split points
		std::vector<Cartesian> pts;
		auto points = Split::split_first_of(next->str(2), ",");
		if (points.size() == 0) THROW(EWKTError, "Syntax error in EWKT format (POLYGON)");

		for (auto it_p = points.begin(); it_p != points.end(); ++it_p) {
			// Get lat, lon and height.
			std::vector<std::string> coords = Split::split_first_of(*it_p, " ");
			if (coords.size() == 3) {
				pts.push_back(Cartesian(stox(std::stod, coords.at(0)), stox(std::stod, coords.at(1)), stox(std::stod, coords.at(2)), CartesianUnits::DEGREES, SRID));
			} else if (coords.size() == 2) {
				pts.push_back(Cartesian(stox(std::stod, coords.at(0)), stox(std::stod, coords.at(1)), 0, CartesianUnits::DEGREES, SRID));
			} else {
				THROW(EWKTError, "The specification for POLYGON is (lat lon [height], ..., lat lon [height]), (lat lon [height], ..., lat lon [height]), ...");
			}
		}

		HTM _htm(partials, error, Geometry(std::move(pts), type));
		_htm.run();

		gv.push_back(std::move(_htm.region));

		if (first) {
			names_f = std::move(_htm.names);
			first = false;
		} else {
			xor_trixels(names_f, std::move(_htm.names));
		}

		++next;
	}

	centroids.insert(HTM::getCentroid(names_f));

	if (match_size != specification.size()) {
		THROW(EWKTError, "Syntax error in EWKT format");
	}

	return names_f;
}


/*
 * The specification is ((lat lon [height], ..., lat lon [height]), (lat lon [height], ..., lat lon [height]), ...)), ((...))
 * lat and lon in degrees.
 * height in meters.
 *
 * Returns the trixels that cover the region.
 */
std::vector<std::string>
EWKT_Parser::parse_multipolygon(const std::string& specification, GeometryType type)
{
	std::vector<std::string> names_f;
	bool first = true;
	size_t match_size = 0;

	std::sregex_iterator next(specification.begin(), specification.end(), find_multi_poly_re, std::regex_constants::match_continuous);
	std::sregex_iterator end;
	while (next != end) {
		match_size += next->length(0);
		if (first) {
			names_f = parse_polygon(next->str(1), type);
			first = false;
		} else {
			std::vector<std::string> txs = parse_polygon(next->str(1), type);
			or_trixels(names_f, std::move(txs));
		}
		++next;
	}

	if (match_size != specification.size()) {
		THROW(EWKTError, "Syntax error in EWKT format (MULTIPOLYGON)");
	}

	return names_f;
}


/*
 * The specification is (lat lon [height], ..., lat lon [height]) or (lat lon [height]), ..., (lat lon [height]), ...
 * lat and lon in degrees.
 * height in meters.
 *
 * Returns the points' trixels.
 */
std::vector<std::string>
EWKT_Parser::parse_point(const std::string& specification)
{
	std::vector<std::string> res;

	std::vector<std::string> coords = Split::split_first_of(specification, " (,");
	if (coords.size() == 3) {
		Cartesian c(stox(std::stod, coords.at(0)), stox(std::stod, coords.at(1)), stox(std::stod, coords.at(2)), CartesianUnits::DEGREES, SRID);
		c.normalize();
		res.push_back(HTM::cartesian2name(c));
		centroids.insert(std::move(c));
	} else if (coords.size() == 2) {
		Cartesian c(stox(std::stod, coords.at(0)), stox(std::stod, coords.at(1)), 0, CartesianUnits::DEGREES, SRID);
		c.normalize();
		res.push_back(HTM::cartesian2name(c));
		centroids.insert(std::move(c));
	} else {
		THROW(EWKTError, "The specification for MULTIPOINT is (lat lon [height], ..., lat lon [height]) or (lat lon [height]), ..., (lat lon [height]), ...");
	}

	return res;
}


/*
 * The specification is lat lon [height]
 * lat and lon in degrees.
 * height in meters.
 *
 * Returns the point's trixels.
 */
std::vector<std::string>
EWKT_Parser::parse_multipoint(const std::string& specification)
{
	// Checking if the format is (lat lon [height]), (lat lon [height]), ... and save the points.
	std::vector<std::string> res;
	size_t match_size = 0;

	std::sregex_iterator next(specification.begin(), specification.end(), find_subpolygon_re, std::regex_constants::match_continuous);
	std::sregex_iterator end;
	while (next != end) {
		match_size += next->length(0);
		std::vector<std::string> coords = Split::split_first_of(next->str(2), " ");
		if (coords.size() == 3) {
			Cartesian c(stox(std::stod, coords.at(0)), stox(std::stod, coords.at(1)), stox(std::stod, coords.at(2)), CartesianUnits::DEGREES, SRID);
			c.normalize();
			res.push_back(HTM::cartesian2name(c));
			centroids.insert(std::move(c));
		} else if (coords.size() == 2) {
			Cartesian c(stox(std::stod, coords.at(0)), stox(std::stod, coords.at(1)), 0, CartesianUnits::DEGREES, SRID);
			c.normalize();
			res.push_back(HTM::cartesian2name(c));
			centroids.insert(std::move(c));
		} else {
			THROW(EWKTError, "The specification for MULTIPOINT is (lat lon [height], ..., lat lon [height]), (lat lon [height], ..., lat lon [height]), ...");
		}
		++next;
	}

	if (match_size == 0) {
		std::vector<std::string> points = Split::split_first_of(specification, ",");
		for (auto it = points.begin(); it != points.end(); ++it) {
			std::vector<std::string> coords = Split::split_first_of(*it, " ");
			if (coords.size() == 3) {
				Cartesian c(stox(std::stod, coords.at(0)), stox(std::stod, coords.at(1)), stox(std::stod, coords.at(2)), CartesianUnits::DEGREES, SRID);
				c.normalize();
				res.push_back(HTM::cartesian2name(c));
				centroids.insert(std::move(c));
			} else if (coords.size() == 2) {
				Cartesian c(stox(std::stod, coords.at(0)), stox(std::stod, coords.at(1)), 0, CartesianUnits::DEGREES, SRID);
				c.normalize();
				res.push_back(HTM::cartesian2name(c));
				centroids.insert(std::move(c));
			} else {
				THROW(EWKTError, "The specification for MULTIPOINT is (lat lon [height], ..., lat lon [height]) or (lat lon [height]), ..., (lat lon [height]), ...");
			}
		}
	} else if (match_size != specification.size()) {
		THROW(EWKTError, "Syntax error in EWKT format (MULTIPOINT)");
	}

	return res;
}


// Parse a collection of geometries (join by OR operation).
std::vector<std::string>
EWKT_Parser::parse_geometry_collection(const std::string& data)
{
	// Checking if the format is correct and processing the geometries.
	std::vector<std::string> names_f;
	bool first = true;
	size_t match_size = 0;

	std::sregex_iterator next(data.begin(), data.end(), find_collection_re, std::regex_constants::match_continuous);
	std::sregex_iterator end;
	while (next != end) {
		match_size += next->length(0);
		std::string geometry(next->str(1));
		std::string specification(next->str(2));
		std::vector<std::string> txs;
		if (geometry == "CIRCLE") txs = parse_circle(specification);
		else if (geometry == "MULTICIRCLE") txs = parse_multicircle(specification);
		else if (geometry == "POLYGON") txs = parse_polygon(specification, GeometryType::CONVEX_POLYGON);
		else if (geometry == "MULTIPOLYGON") txs = parse_multipolygon(specification, GeometryType::CONVEX_POLYGON);
		else if (geometry == "POINT") txs = parse_point(specification);
		else if (geometry == "MULTIPOINT") txs = parse_multipoint(specification);
		else if (geometry == "CHULL") txs = parse_polygon(specification, GeometryType::CONVEX_HULL);
		else if (geometry == "MULTICHULL") txs = parse_polygon(specification, GeometryType::CONVEX_HULL);

		if (first) {
			names_f = std::move(txs);
			first = false;
		} else {
			or_trixels(names_f, std::move(txs));
		}

		++next;
	}

	if (match_size != data.size()) {
		THROW(EWKTError, "Syntax error in EWKT format");
	}

	return names_f;
}


// Parse a intersection of geometries (join by AND operation).
std::vector<std::string>
EWKT_Parser::parse_geometry_intersection(const std::string& data)
{
	// Checking if the format is correct and processing the geometries.
	std::vector<std::string> names_f;
	bool first = true;
	size_t match_size = 0;

	std::sregex_iterator next(data.begin(), data.end(), find_collection_re, std::regex_constants::match_continuous);
	std::sregex_iterator end;
	while (next != end) {
		match_size += next->length(0);
		std::string geometry(next->str(1));
		std::string specification(next->str(2));
		std::vector<std::string> txs;
		if (geometry == "CIRCLE") txs = parse_circle(specification);
		else if (geometry == "MULTICIRCLE") txs = parse_multicircle(specification);
		else if (geometry == "POLYGON") txs = parse_polygon(specification, GeometryType::CONVEX_POLYGON);
		else if (geometry == "MULTIPOLYGON") txs = parse_multipolygon(specification, GeometryType::CONVEX_POLYGON);
		else if (geometry == "POINT") txs = parse_point(specification);
		else if (geometry == "MULTIPOINT") txs = parse_multipoint(specification);
		else if (geometry == "CHULL") txs = parse_polygon(specification, GeometryType::CONVEX_HULL);
		else if (geometry == "MULTICHULL") txs = parse_polygon(specification, GeometryType::CONVEX_HULL);

		if (first) {
			names_f = std::move(txs);
			first = false;
		} else {
			and_trixels(names_f, std::move(txs));
			if (names_f.empty()) return names_f;
		}

		++next;
	}

	if (match_size != data.size()) {
		THROW(EWKTError, "Syntax error in EWKT format");
	}

	centroids.clear();
	centroids.insert(HTM::getCentroid(names_f));

	return names_f;
}


RangeList
EWKT_Parser::getRanges()
{
	RangeList ranges;
	ranges.reserve(trixels.size());

	for (const auto& trixel : trixels) {
		HTM::insertRange(trixel, ranges, HTM_MAX_LEVEL);
	}

	HTM::mergeRanges(ranges);

	return ranges;
}


// Exclusive or of two sets of trixels.
void
EWKT_Parser::xor_trixels(std::vector<std::string>& txs1, std::vector<std::string>&& txs2)
{
	for (auto it1 = txs1.begin(); it1 != txs1.end(); ) {
		bool deleted = false;
		for (auto it2 = txs2.begin(); it2 != txs2.end(); ) {
			size_t s1 = it1->size(), s2 = it2->size();
			if (s1 >= s2 && (*it1).find(*it2) == 0) {
				if (s1 == s2) {
					it1 = txs1.erase(it1);
					it2 = txs2.erase(it2);
				} else {
					auto txs_aux = get_trixels(*it2, s1 - s2, *it1);
					it1 = txs1.erase(it1);
					it2 = txs2.insert(txs2.erase(it2), txs_aux.begin(), txs_aux.end());
				}
				deleted = true;
				break;
			} else if (s2 > s1 && (*it2).find(*it1) == 0) {
				auto txs_aux = get_trixels(*it1, s2 - s1, *it2);
				it2 = txs2.erase(it2);
				it1 = txs1.insert(txs1.erase(it1), txs_aux.begin(), txs_aux.end());
				deleted = true;
				break;
			}
			++it2;
		}
		if (!deleted) ++it1;
	}

	txs1.reserve(txs1.size() + txs2.size());
	txs1.insert(txs1.end(), txs2.begin(), txs2.end());
}


// Join of two sets of trixels.
void
EWKT_Parser::or_trixels(std::vector<std::string>& txs1, std::vector<std::string>&& txs2)
{
	for (auto it1 = txs1.begin(); it1 != txs1.end(); ) {
		bool deleted = false;
		for (auto it2 = txs2.begin(); it2 != txs2.end(); ) {
			size_t s1 = it1->size(), s2 = it2->size();
			if (s1 >= s2 && (*it1).find(*it2) == 0) {
				it1 = txs1.erase(it1);
				deleted = true;
				break;
			} else if (s2 > s1 && (*it2).find(*it1) == 0) {
				it2 = txs2.erase(it2);
				continue;
			}
			++it2;
		}
		if (!deleted) ++it1;
	}

	txs1.reserve(txs1.size() + txs2.size());
	txs1.insert(txs1.end(), txs2.begin(), txs2.end());
}


// Intersection of two sets of trixels.
void
EWKT_Parser::and_trixels(std::vector<std::string>& txs1, std::vector<std::string>&& txs2)
{
	std::vector<std::string> res;
	res.reserve(txs1.size() + txs2.size());
	for (auto it1 = txs1.begin(); it1 != txs1.end(); ++it1) {
		for (auto it2 = txs2.begin(); it2 != txs2.end(); ) {
			size_t s1 = it1->size(), s2 = it2->size();
			if (s1 >= s2 && (*it1).find(*it2) == 0) {
				res.push_back(*it1);
				break;
			} else if (s2 > s1 && (*it2).find(*it1) == 0) {
				res.push_back(*it2);
				it2 = txs2.erase(it2);
				continue;
			}
			++it2;
		}
	}

	txs1 = std::move(res);
}


/*
 * Returns the trixels that conform to the father except trixel's son.
 *   Father      Son			 Trixels back:
 *     /\	     /\
 *    /__\      /__\	   =>	     __
 *   /\  /\					       /\  /\
 *  /__\/__\					  /__\/__\
 */
std::vector<std::string>
EWKT_Parser::get_trixels(const std::string& father, size_t depth, const std::string& son)
{
	std::vector<std::string> sonsF;
	std::string p_son(father);
	size_t m_size = father.size() + depth;

	for (size_t i = father.size(); i < m_size; ++i) {
		switch (son.at(i)) {
			case '0':
				sonsF.push_back(p_son + "1");
				sonsF.push_back(p_son + "2");
				sonsF.push_back(p_son + "3");
				p_son += "0";
				break;
			case '1':
				sonsF.push_back(p_son + "0");
				sonsF.push_back(p_son + "2");
				sonsF.push_back(p_son + "3");
				p_son += "1";
				break;
			case '2':
				sonsF.push_back(p_son + "0");
				sonsF.push_back(p_son + "1");
				sonsF.push_back(p_son + "3");
				p_son += "2";
				break;
			case '3':
				sonsF.push_back(p_son + "0");
				sonsF.push_back(p_son + "1");
				sonsF.push_back(p_son + "2");
				p_son += "3";
				break;
		}
	}

	return sonsF;
}


bool
EWKT_Parser::isEWKT(const std::string& str)
{
	std::smatch m;
	return std::regex_match(str, m, find_geometry_re) && static_cast<size_t>(m.length(0)) == str.size();
}


GeoSpatial
EWKT_Parser::getGeoSpatial(const std::string& field_value, bool partials, double error)
{
	EWKT_Parser ewkt(field_value, partials, error);

	RangeList ranges;
	ranges.reserve(ewkt.trixels.size());

	for (const auto& trixel : ewkt.trixels) {
		HTM::insertRange(trixel, ranges, HTM_MAX_LEVEL);
	}

	HTM::mergeRanges(ranges);

	return GeoSpatial({ std::move(ranges), std::move(ewkt.centroids) });
}


CartesianUSet
EWKT_Parser::getCentroids(const std::string& field_value, bool partials, double error)
{
	EWKT_Parser ewkt(field_value, partials, error);
	return std::move(ewkt.centroids);
}
