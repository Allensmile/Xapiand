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

#include "geospatial.h"

#include "../cast.h"
#include "../utils.h"


const std::unordered_map<string_view, GeoSpatial::dispatch_func> GeoSpatial::map_dispatch({
	{ GEO_LATITUDE,          &GeoSpatial::process_latitude    },
	{ GEO_LONGITUDE,         &GeoSpatial::process_longitude   },
	{ GEO_HEIGHT,            &GeoSpatial::process_height      },
	{ GEO_RADIUS,            &GeoSpatial::process_radius      },
	{ GEO_UNITS,             &GeoSpatial::process_units       },
	{ GEO_SRID,              &GeoSpatial::process_srid        },
});


GeoSpatial::GeoSpatial(const MsgPack& obj)
{
	switch (obj.getType()) {
		case MsgPack::Type::STR: {
			EWKT ewkt(obj.str_view());
			geometry = ewkt.getGeometry();
			return;
		}
		case MsgPack::Type::MAP: {
			auto it = obj.begin();
			const auto str_key = it->str();
			switch ((Cast::Hash)xxh64::hash(str_key)) {
				case Cast::Hash::EWKT: {
					try {
						EWKT ewkt(it.value().str_view());
						geometry = ewkt.getGeometry();
						return;
					} catch (const msgpack::type_error&) {
						THROW(GeoSpatialError, "%s must be string", RESERVED_EWKT);
					}
				}
				case Cast::Hash::POINT:
					geometry = std::make_unique<Point>(make_point(it.value()));
					return;
				case Cast::Hash::CIRCLE:
					geometry = std::make_unique<Circle>(make_circle(it.value()));
					return;
				case Cast::Hash::CONVEX:
					geometry = std::make_unique<Convex>(make_convex(it.value()));
					return;
				case Cast::Hash::POLYGON:
					geometry = std::make_unique<Polygon>(make_polygon(it.value(), Geometry::Type::POLYGON));
					return;
				case Cast::Hash::CHULL:
					geometry = std::make_unique<Polygon>(make_polygon(it.value(), Geometry::Type::CHULL));
					return;
				case Cast::Hash::MULTIPOINT:
					geometry = std::make_unique<MultiPoint>(make_multipoint(it.value()));
					return;
				case Cast::Hash::MULTICIRCLE:
					geometry = std::make_unique<MultiCircle>(make_multicircle(it.value()));
					return;
				case Cast::Hash::MULTIPOLYGON:
					geometry = std::make_unique<MultiPolygon>(make_multipolygon(it.value()));
					return;
				case Cast::Hash::GEO_COLLECTION:
					geometry = std::make_unique<Collection>(make_collection(it.value()));
					return;
				case Cast::Hash::GEO_INTERSECTION:
					geometry = std::make_unique<Intersection>(make_intersection(it.value()));
					return;
				default:
					THROW(GeoSpatialError, "Unknown geometry %s", str_key.c_str());
			}
		}
		default:
			THROW(GeoSpatialError, "Object must be string or map");
	}
}


inline void
GeoSpatial::process_latitude(data_t& data, const MsgPack& latitude) {
	data.lat = &latitude;
}


inline void
GeoSpatial::process_longitude(data_t& data, const MsgPack& longitude) {
	data.lon = &longitude;
}


inline void
GeoSpatial::process_height(data_t& data, const MsgPack& height) {
	data.height = &height;
}


inline void
GeoSpatial::process_radius(data_t& data, const MsgPack& radius) {
	if (!data.has_radius) {
		THROW(GeoSpatialError, "%s applies only to %s or %s", GEO_RADIUS, RESERVED_CIRCLE, RESERVED_MULTICIRCLE);
	}
	data.radius = &radius;
}


inline void
GeoSpatial::process_units(data_t& data, const MsgPack& units)
{
	try {
		const auto str = units.str_view();
		if (str == "degrees") {
			data.units = Cartesian::Units::DEGREES;
		} else if (str == "radians") {
			data.units = Cartesian::Units::RADIANS;
		} else {
			THROW(GeoSpatialError, "%s must be \"degrees\" or \"radians\"", GEO_UNITS);
		}
	} catch (const msgpack::type_error&) {
		THROW(GeoSpatialError, "%s must be string (\"degrees\" or \"radians\")", GEO_UNITS);
	}
}


inline void
GeoSpatial::process_srid(data_t& data, const MsgPack& srid) {
	try {
		data.srid = srid.i64();
		if (!Cartesian::is_SRID_supported(data.srid)) {
			THROW(GeoSpatialError, "SRID = %d is not supported", data.srid);
		}
	} catch (const msgpack::type_error&) {
		THROW(GeoSpatialError, "%s must be integer", GEO_SRID);
	}
}


GeoSpatial::data_t
GeoSpatial::get_data(const MsgPack& o, bool hradius)
{
	data_t data(hradius);
	static const auto dit_e = map_dispatch.end();
	const auto it_e = o.end();
	for (auto it = o.begin(); it != it_e; ++it) {
		const auto str_key = it->str_view();
		const auto dit = map_dispatch.find(str_key);
		if (dit == dit_e) {
			THROW(GeoSpatialError, "%s is a invalid word", repr(str_key).c_str());
		} else {
			(this->*dit->second)(data, it.value());
		}
	}
	return data;
}


std::vector<Cartesian>
GeoSpatial::getPoints(const data_t& data, const MsgPack& latitude, const MsgPack& longitude, const MsgPack* height)
{
	try {
		if (data.lat->size() == data.lon->size()) {
			std::vector<Cartesian> points;
			points.reserve(latitude.size());
			if (height) {
				auto it = latitude.begin();
				auto hit = height->begin();
				for (const auto& lon : longitude) {
					points.emplace_back(it->f64(), lon.f64(), hit->f64(), data.units, data.srid);
					++it;
					++hit;
				}
			} else {
				auto it = latitude.begin();
				for (const auto& lon : longitude) {
					points.emplace_back(it->f64(), lon.f64(), 0, data.units, data.srid);
					++it;
				}
			}
			return points;
		} else {
			THROW(GeoSpatialError, "%s, %s and %s must have the same size", GEO_LATITUDE, GEO_LONGITUDE, GEO_HEIGHT);
		}
	} catch (const msgpack::type_error&) {
		THROW(GeoSpatialError, "%s, %s and %s must be array of numbers or nested array of numbers", GEO_LATITUDE, GEO_LONGITUDE, GEO_HEIGHT);
	}
}


Point
GeoSpatial::make_point(const MsgPack& o)
{
	if (o.is_map()) {
		const auto data = get_data(o);
		if (data.lat && data.lon) {
			try {
				return Point(Cartesian(data.lat->f64(), data.lon->f64(), data.height ? data.height->f64() : 0, data.units, data.srid));
			} catch (const msgpack::type_error&) {
				THROW(GeoSpatialError, "%s, %s and %s must be numeric", GEO_LATITUDE, GEO_LONGITUDE, GEO_HEIGHT);
			}
		} else {
			THROW(GeoSpatialError, "%s must contain %s and %s", RESERVED_POINT, GEO_LATITUDE, GEO_LONGITUDE);
		}
	} else {
		THROW(GeoSpatialError, "%s must be map", RESERVED_POINT);
	}
}


Circle
GeoSpatial::make_circle(const MsgPack& o)
{
	if (o.is_map()) {
		const auto data = get_data(o, true);
		if (data.lat && data.lon && data.radius) {
			try {
				return Circle(Cartesian(data.lat->f64(), data.lon->f64(), data.height ? data.height->f64() : 0, data.units, data.srid), data.radius->f64());
			} catch (const msgpack::type_error&) {
				THROW(GeoSpatialError, "%s, %s, %s and %s must be numeric", GEO_LATITUDE, GEO_LONGITUDE, GEO_HEIGHT, GEO_RADIUS);
			}
		} else {
			THROW(GeoSpatialError, "%s must contain %s, %s and %s", RESERVED_CIRCLE, GEO_LATITUDE, GEO_LONGITUDE, GEO_RADIUS);
		}
	} else {
		THROW(GeoSpatialError, "%s must be map", RESERVED_CIRCLE);
	}
}


Convex
GeoSpatial::make_convex(const MsgPack& o)
{
	if (o.is_map()) {
		const auto data = get_data(o, true);
		if (data.lat && data.lon && data.radius) {
			if (data.lat->size() == data.lon->size()) {
				try {
					Convex convex;
					if (data.height) {
						if (data.lat->size() == data.height->size()) {
							auto it = data.lon->begin();
							auto hit = data.height->begin();
							convex.reserve(data.lat->size());
							for (const auto& latitude : *data.lat) {
								convex.add(Circle(Cartesian(latitude.f64(), it->f64(), hit->f64(), data.units, data.srid), data.radius->f64()));
								++it;
								++hit;
							}
						} else {
							THROW(GeoSpatialError, "%s, %s and %s must have the same size", GEO_LATITUDE, GEO_LONGITUDE, GEO_HEIGHT);
						}
					} else {
						auto it = data.lon->begin();
						convex.reserve(data.lat->size());
						for (const auto& latitude : *data.lat) {
							convex.add(Circle(Cartesian(latitude.f64(), it->f64(), 0, data.units, data.srid), data.radius->f64()));
							++it;
						}
					}
					return convex;
				} catch (const msgpack::type_error&) {
					THROW(GeoSpatialError, "%s, %s, %s and %s must be array of numbers", GEO_LATITUDE, GEO_LONGITUDE, GEO_HEIGHT, GEO_RADIUS);
				}
			} else {
				THROW(GeoSpatialError, "%s and %s must have the same size", GEO_LATITUDE, GEO_LONGITUDE);
			}
		} else {
			THROW(GeoSpatialError, "%s must contain %s, %s and %s", RESERVED_CONVEX, GEO_LATITUDE, GEO_LONGITUDE, GEO_RADIUS);
		}
	} else {
		THROW(GeoSpatialError, "%s must be map", RESERVED_CONVEX);
	}
}


Polygon
GeoSpatial::make_polygon(const MsgPack& o, Geometry::Type type)
{
	if (o.is_map()) {
		const auto data = get_data(o);
		if (data.lat && data.lon) {
			if (data.lat->size() == data.lon->size()) {
				auto it = data.lon->begin();
				if (it->is_array()) {
					Polygon polygon(type);
					if (data.height) {
						if (data.lat->size() == data.height->size()) {
							auto hit = data.height->begin();
							polygon.reserve(data.lat->size());
							for (const auto& lat : *data.lat) {
								polygon.add(getPoints(data, lat, *it, &*hit));
								++it;
								++hit;
							}
						} else {
							THROW(GeoSpatialError, "%s, %s and %s must have the same size", GEO_LATITUDE, GEO_LONGITUDE, GEO_HEIGHT);
						}
					} else {
						polygon.reserve(data.lat->size());
						for (const auto& lat : *data.lat) {
							polygon.add(getPoints(data, lat, *it));
							++it;
						}
					}
					return polygon;
				} else {
					return Polygon(type, getPoints(data, *data.lat, *data.lon, data.height));
				}
			} else {
				THROW(GeoSpatialError, "%s and %s must have the same size", GEO_LATITUDE, GEO_LONGITUDE);
			}
		} else {
			THROW(GeoSpatialError, "%s must contain %s and %s", RESERVED_POLYGON, GEO_LATITUDE, GEO_LONGITUDE);
		}
	} else {
		THROW(GeoSpatialError, "%s must be map", RESERVED_POLYGON);
	}
}


MultiPoint
GeoSpatial::make_multipoint(const MsgPack& o)
{
	if (o.is_map()) {
		const auto data = get_data(o);
		if (data.lat && data.lon) {
			if (data.lat->size() == data.lon->size()) {
				try {
					MultiPoint multipoint;
					if (data.height) {
						if (data.lat->size() == data.height->size()) {
							auto it = data.lon->begin();
							auto hit = data.height->begin();
							multipoint.reserve(data.lat->size());
							for (const auto& latitude : *data.lat) {
								multipoint.add(Point(Cartesian(latitude.f64(), it->f64(), hit->f64(), data.units, data.srid)));
								++it;
								++hit;
							}
						} else {
							THROW(GeoSpatialError, "%s, %s and %s must have the same size", GEO_LATITUDE, GEO_LONGITUDE, GEO_HEIGHT);
						}
					} else {
						auto it = data.lon->begin();
						multipoint.reserve(data.lat->size());
						for (const auto& latitude : *data.lat) {
							multipoint.add(Point(Cartesian(latitude.f64(), it->f64(), 0, data.units, data.srid)));
							++it;
						}
					}
					return multipoint;
				} catch (const msgpack::type_error&) {
					THROW(GeoSpatialError, "%s, %s and %s must be array of numbers", GEO_LATITUDE, GEO_LONGITUDE, GEO_HEIGHT);
				}
			} else {
				THROW(GeoSpatialError, "%s and %s must have the same size", GEO_LATITUDE, GEO_LONGITUDE);
			}
		} else {
			THROW(GeoSpatialError, "%s must contain %s and %s", RESERVED_MULTIPOINT, GEO_LATITUDE, GEO_LONGITUDE);
		}
	} else {
		THROW(GeoSpatialError, "%s must be map", RESERVED_MULTIPOINT);
	}
}


MultiCircle
GeoSpatial::make_multicircle(const MsgPack& o)
{
	if (o.is_map()) {
		const auto data = get_data(o, true);
		if (data.lat && data.lon && data.radius) {
			if (data.lat->size() == data.lon->size()) {
				try {
					MultiCircle multicircle;
					if (data.height) {
						if (data.lat->size() == data.height->size()) {
							auto it = data.lon->begin();
							auto hit = data.height->begin();
							multicircle.reserve(data.lat->size());
							for (const auto& latitude : *data.lat) {
								multicircle.add(Circle(Cartesian(latitude.f64(), it->f64(), hit->f64(), data.units, data.srid), data.radius->f64()));
								++it;
								++hit;
							}
						} else {
							THROW(GeoSpatialError, "%s, %s and %s must have the same size", GEO_LATITUDE, GEO_LONGITUDE, GEO_HEIGHT);
						}
					} else {
						auto it = data.lon->begin();
						multicircle.reserve(data.lat->size());
						for (const auto& latitude : *data.lat) {
							multicircle.add(Circle(Cartesian(latitude.f64(), it->f64(), 0, data.units, data.srid), data.radius->f64()));
							++it;
						}
					}
					return multicircle;
				} catch (const msgpack::type_error&) {
					THROW(GeoSpatialError, "%s, %s, %s and %s must be array of numbers", GEO_LATITUDE, GEO_LONGITUDE, GEO_HEIGHT, GEO_RADIUS);
				}
			} else {
				THROW(GeoSpatialError, "%s and %s must have the same size", GEO_LATITUDE, GEO_LONGITUDE);
			}
		} else {
			THROW(GeoSpatialError, "%s must contain %s, %s and %s", RESERVED_MULTICIRCLE, GEO_LATITUDE, GEO_LONGITUDE, GEO_RADIUS);
		}
	} else {
		THROW(GeoSpatialError, "%s must be map", RESERVED_MULTICIRCLE);
	}
}


MultiPolygon
GeoSpatial::make_multipolygon(const MsgPack& o)
{
	switch (o.getType()) {
		case MsgPack::Type::MAP: {
			MultiPolygon multipolygon;
			multipolygon.reserve(o.size());
			const auto it_e = o.end();
			for (auto it = o.begin(); it != it_e; ++it) {
				const auto str_key = it->str();
				switch ((Cast::Hash)xxh64::hash(str_key)) {
					case Cast::Hash::POLYGON:
						multipolygon.add(make_polygon(it.value(), Geometry::Type::POLYGON));
						break;
					case Cast::Hash::CHULL:
						multipolygon.add(make_polygon(it.value(), Geometry::Type::CHULL));
						break;
					default:
						THROW(GeoSpatialError, "%s must be a map only with %s and %s", RESERVED_MULTIPOLYGON, RESERVED_POLYGON, RESERVED_CHULL);
				}
			}
			return multipolygon;
		}
		case MsgPack::Type::ARRAY: {
			const auto data = get_data(o);
			if (data.lat && data.lon) {
				if (data.lat->size() == data.lon->size()) {
					MultiPolygon multipolygon;
					multipolygon.reserve(data.lat->size());
					if (data.height) {
						if (data.lat->size() == data.height->size()) {
							auto m_it = data.lon->begin();
							auto m_hit = data.height->begin();
							for (const auto& m_lat : *data.lat) {
								if (m_lat.is_array()) {
									Polygon polygon(Geometry::Type::POLYGON);
									polygon.reserve(m_lat.size());
									auto it = m_it->begin();
									auto hit = m_hit->begin();
									for (const auto& lat : m_lat) {
										polygon.add(getPoints(data, lat, *it, &*hit));
										++it;
										++hit;
									}
									multipolygon.add(std::move(polygon));
								} else {
									multipolygon.add(Polygon(Geometry::Type::POLYGON, getPoints(data, m_lat, *m_it, &*m_hit)));
								}
								++m_it;
								++m_hit;
							}
						} else {
							THROW(GeoSpatialError, "%s, %s and %s must have the same size", GEO_LATITUDE, GEO_LONGITUDE, GEO_HEIGHT);
						}
					} else {
						auto m_it = data.lon->begin();
						for (const auto& m_lat : *data.lat) {
							if (m_lat.is_array()) {
								Polygon polygon(Geometry::Type::POLYGON);
								polygon.reserve(m_lat.size());
								auto it = m_it->begin();
								for (const auto& lat : m_lat) {
									polygon.add(getPoints(data, lat, *it));
									++it;
								}
								multipolygon.add(std::move(polygon));
							} else {
								multipolygon.add(Polygon(Geometry::Type::POLYGON, getPoints(data, m_lat, *m_it)));
							}
							++m_it;
						}
					}
					return multipolygon;
				} else {
					THROW(GeoSpatialError, "%s and %s must have the same size", GEO_LATITUDE, GEO_LONGITUDE);
				}
			} else {
				THROW(GeoSpatialError, "%s must contain %s and %s", RESERVED_MULTIPOLYGON, GEO_LATITUDE, GEO_LONGITUDE);
			}
		}
		default: {
			THROW(GeoSpatialError, "%s must be map or nested array of numbers", RESERVED_MULTIPOLYGON);
		}
	}
}


Collection
GeoSpatial::make_collection(const MsgPack& o)
{
	if (o.is_map()) {
		Collection collection;
		const auto it_e = o.end();
		for (auto it = o.begin(); it != it_e; ++it) {
			const auto str_key = it->str();
			switch ((Cast::Hash)xxh64::hash(str_key)) {
				case Cast::Hash::POINT:
					collection.add_point(make_point(it.value()));
					break;
				case Cast::Hash::CIRCLE:
					collection.add_circle(make_circle(it.value()));
					break;
				case Cast::Hash::CONVEX:
					collection.add_convex(make_convex(it.value()));
					break;
				case Cast::Hash::POLYGON:
					collection.add_polygon(make_polygon(it.value(), Geometry::Type::POLYGON));
					break;
				case Cast::Hash::CHULL:
					collection.add_polygon(make_polygon(it.value(), Geometry::Type::CHULL));
					break;
				case Cast::Hash::MULTIPOINT:
					collection.add_multipoint(make_multipoint(it.value()));
					break;
				case Cast::Hash::MULTICIRCLE:
					collection.add_multicircle(make_multicircle(it.value()));
					break;
				case Cast::Hash::MULTIPOLYGON:
					collection.add_multipolygon(make_multipolygon(it.value()));
					break;
				case Cast::Hash::GEO_COLLECTION:
					collection.add(make_collection(it.value()));
					break;
				case Cast::Hash::GEO_INTERSECTION:
					collection.add_intersection(make_intersection(it.value()));
					break;
				default:
					THROW(GeoSpatialError, "Unknown geometry %s", str_key.c_str());
			}
		}
		return collection;
	} else {
		THROW(GeoSpatialError, "%s must be map", RESERVED_GEO_COLLECTION);
	}
}


Intersection
GeoSpatial::make_intersection(const MsgPack& o)
{
	if (o.is_map()) {
		Intersection intersection;
		intersection.reserve(o.size());
		const auto it_e = o.end();
		for (auto it = o.begin(); it != it_e; ++it) {
			const auto str_key = it->str();
			switch ((Cast::Hash)xxh64::hash(str_key)) {
				case Cast::Hash::POINT:
					intersection.add(std::make_shared<Point>(make_point(it.value())));
					break;
				case Cast::Hash::CIRCLE:
					intersection.add(std::make_shared<Circle>(make_circle(it.value())));
					break;
				case Cast::Hash::CONVEX:
					intersection.add(std::make_shared<Convex>(make_convex(it.value())));
					break;
				case Cast::Hash::POLYGON:
					intersection.add(std::make_shared<Polygon>(make_polygon(it.value(), Geometry::Type::POLYGON)));
					break;
				case Cast::Hash::CHULL:
					intersection.add(std::make_shared<Polygon>(make_polygon(it.value(), Geometry::Type::CHULL)));
					break;
				case Cast::Hash::MULTIPOINT:
					intersection.add(std::make_shared<MultiPoint>(make_multipoint(it.value())));
					break;
				case Cast::Hash::MULTICIRCLE:
					intersection.add(std::make_shared<MultiCircle>(make_multicircle(it.value())));
					break;
				case Cast::Hash::MULTIPOLYGON:
					intersection.add(std::make_shared<MultiPolygon>(make_multipolygon(it.value())));
					break;
				case Cast::Hash::GEO_COLLECTION:
					intersection.add(std::make_shared<Collection>(make_collection(it.value())));
					break;
				case Cast::Hash::GEO_INTERSECTION:
					intersection.add(std::make_shared<Intersection>(make_intersection(it.value())));
					break;
				default:
					THROW(GeoSpatialError, "Unknown geometry %s", str_key.c_str());
			}
		}
		return intersection;
	} else {
		THROW(GeoSpatialError, "%s must be map", RESERVED_GEO_INTERSECTION);
	}
}
