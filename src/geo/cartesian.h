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

#pragma once

#include "../exception.h"

#include <cstdio>
#include <functional>
#include <string>


/*
 * This SRIDs were obtained of http://www.epsg.org/. However we can use differents datums.
 * The datums used were obtained from:
 *   http://earth-info.nga.mil/GandG/coordsys/datums/NATO_DT.pdf
 *      CRS     SRID
 */
#define WGS84   4326 // Cartesian uses this Coordinate Reference System (CRS).
#define WGS72   4322
#define NAD83   4269
#define NAD27   4267
#define OSGB36  4277
#define TM75    4300
#define TM65    4299
#define ED79    4668
#define ED50    4230
#define TOYA    4301
#define DHDN    4314
#define OEG     4229
#define AGD84   4203
#define SAD69   4618
#define PUL42   4178
#define MGI1901 3906
#define GGRS87  4121


// Double tolerance.
constexpr double DBL_TOLERANCE = 1e-15;


// Constant used for converting degrees to radians and back
constexpr double RAD_PER_DEG = 0.01745329251994329576923691;
constexpr double DEG_PER_RAD = 57.2957795130823208767981548;


// Constant used to verify the range of latitude.
constexpr double PI_HALF = 1.57079632679489661923132169;


/*
 * The simple geometric shape which most closely approximates the shape of the Earth is a
 * biaxial ellipsoid.
 *
 * Name of ellipsoids were obtained from:
 *   http://earth-info.nga.mil/GandG/coordsys/datums/ellips.txt
 */
struct ellipsoid_t {
	std::string name;
	double major_axis;
	double minor_axis;
	double e2;              // First eccentricity squared = 2f - f^2
};


struct datum_t {
	std::string name;       // Datum name
	ellipsoid_t ellipsoid;  // Ellipsoid used
	double tx;              // meters
	double ty;              // meters
	double tz;              // meters
	double rx;              // radians
	double ry;              // radians
	double rz;              // radians
	double s;               // scale factor s / 1E6
};


enum class CartesianUnits {
	RADIANS,
	DEGREES
};


class CartesianError : public ClientError {
public:
	template<typename... Args>
	CartesianError(Args&&... args) : ClientError(std::forward<Args>(args)...) { }
};

#define MSG_CartesianError(...) CartesianError(__FILE__, __LINE__, __VA_ARGS__)


/*
 * The formulas used for the conversions were obtained from:
 *    "A guide to coordinate systems in Great Britain"
 */
class Cartesian {
	int SRID;
	datum_t datum;

	void transform2WGS84() noexcept;
	void toCartesian(double lat, double lon, double height, CartesianUnits units);

public:
	double x;
	double y;
	double z;

	Cartesian();
	Cartesian(double lat, double lon, double height, CartesianUnits units, int SRID=WGS84);
	Cartesian(double x, double y, double z);
	// Move constructor
	Cartesian(Cartesian&& p) = default;
	// Copy constructor
	Cartesian(const Cartesian& p) = default;

	// Move assignment
	Cartesian& operator=(Cartesian&& p) = default;
	// Copy assignment
	Cartesian& operator=(const Cartesian& p) = default;
	bool operator==(const Cartesian& p) const noexcept;
	bool operator!=(const Cartesian& p) const noexcept;
	// Dot product
	double operator*(const Cartesian& p) const noexcept;
	// Vector product
	Cartesian operator^(const Cartesian& p) const noexcept;
	Cartesian& operator^=(const Cartesian& p) noexcept;
	Cartesian operator+(const Cartesian& p) const noexcept;
	Cartesian& operator+=(const Cartesian& p) noexcept;
	Cartesian operator-(const Cartesian& p) const noexcept;
	Cartesian& operator-=(const Cartesian& p) noexcept;

	std::string Decimal2Degrees() const;
	void toGeodetic(double& lat, double& lon, double& height) const;
	void normalize();
	void inverse() noexcept;
	double norm() const;
	std::string as_string() const;

	static bool is_SRID_supported(int _SRID);

	inline int getSRID() const noexcept {
		return SRID;
	}

	inline datum_t getDatum() const noexcept {
		return datum;
	}
};


namespace std {
	template<>
	struct hash<Cartesian> {
		size_t operator()(const Cartesian& p) const;
	};
}
