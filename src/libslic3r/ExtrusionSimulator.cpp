// Optimize the extrusion simulator to the bones.
//#pragma GCC optimize ("O3")
//#undef SLIC3R_DEBUG
//#define NDEBUG

#include <cmath>
#include <cassert>

#include <boost/geometry.hpp>
#include <boost/geometry/geometries/box.hpp>
#include <boost/geometry/geometries/point.hpp>
#include <boost/geometry/geometries/point_xy.hpp>

#include <boost/multi_array.hpp>

#include "libslic3r.h"
#include "ExtrusionSimulator.hpp"

#ifndef M_PI
#define M_PI 3.1415926535897932384626433832795
#endif

namespace Slic3r {

// Replacement for a template alias.
// Shorthand for the point_xy.
template<typename T>
struct V2
{
	typedef boost::geometry::model::d2::point_xy<T> Type;
};

// Replacement for a template alias.
// Shorthand for the point with a cartesian coordinate system.
template<typename T>
struct V3
{
	typedef boost::geometry::model::point<T, 3, boost::geometry::cs::cartesian> Type;
};

// Replacement for a template alias.
// Shorthand for the point with a cartesian coordinate system.
template<typename T>
struct V4
{
	typedef boost::geometry::model::point<T, 4, boost::geometry::cs::cartesian> Type;
};

typedef V2<int   >::Type V2i;
typedef V2<float >::Type V2f;
typedef V2<double>::Type V2d;

// Used for an RGB color.
typedef V3<unsigned char>::Type V3uc;
// Used for an RGBA color.
typedef V4<unsigned char>::Type V4uc;

typedef boost::geometry::model::box<V2i> B2i;
typedef boost::geometry::model::box<V2f> B2f;
typedef boost::geometry::model::box<V2d> B2d;

typedef boost::multi_array<unsigned char, 2> 	 A2uc;
typedef boost::multi_array<int   		, 2> 	 A2i;
typedef boost::multi_array<float 		, 2> 	 A2f;
typedef boost::multi_array<double		, 2> 	 A2d;

template<typename T>
inline void operator+=(
	      boost::geometry::model::d2::point_xy<T> &v1, 
	const boost::geometry::model::d2::point_xy<T> &v2)
{
	boost::geometry::add_point(v1, v2);
}

template<typename T>
inline void operator-=(
	      boost::geometry::model::d2::point_xy<T> &v1, 
	const boost::geometry::model::d2::point_xy<T> &v2)
{
	boost::geometry::subtract_point(v1, v2);
}

template<typename T>
inline void operator*=(boost::geometry::model::d2::point_xy<T> &v, const T c)
{
	boost::geometry::multiply_value(v, c);
}

template<typename T>
inline void operator/=(boost::geometry::model::d2::point_xy<T> &v, const T c)
{
	boost::geometry::divide_value(v, c);
}

template<typename T>
inline typename boost::geometry::model::d2::point_xy<T> operator+(
	const boost::geometry::model::d2::point_xy<T> &v1,
	const boost::geometry::model::d2::point_xy<T> &v2)
{
	boost::geometry::model::d2::point_xy<T> out(v1);
	out += v2;
	return out;
}

template<typename T>
inline boost::geometry::model::d2::point_xy<T> operator-(
	const boost::geometry::model::d2::point_xy<T> &v1,
	const boost::geometry::model::d2::point_xy<T> &v2)
{
	boost::geometry::model::d2::point_xy<T> out(v1);
	out -= v2;
	return out;
}

template<typename T>
inline boost::geometry::model::d2::point_xy<T> operator*(
	const boost::geometry::model::d2::point_xy<T> &v, const T c)
{
	boost::geometry::model::d2::point_xy<T> out(v);
	out *= c;
	return out;
}

template<typename T>
inline typename boost::geometry::model::d2::point_xy<T> operator*(
	const T c, const boost::geometry::model::d2::point_xy<T> &v)
{
	boost::geometry::model::d2::point_xy<T> out(v);
	out *= c;
	return out;
}

template<typename T>
inline typename boost::geometry::model::d2::point_xy<T> operator/(
	const boost::geometry::model::d2::point_xy<T> &v, const T c)
{
	boost::geometry::model::d2::point_xy<T> out(v);
	out /= c;
	return out;
}

template<typename T>
inline T dot(
	const boost::geometry::model::d2::point_xy<T> &v1,
	const boost::geometry::model::d2::point_xy<T> &v2)
{
	return boost::geometry::dot_product(v1, v2);
}

template<typename T>
inline T dot(const boost::geometry::model::d2::point_xy<T> &v)
{
	return boost::geometry::dot_product(v, v);
}

template <typename T>
inline T cross(
	const boost::geometry::model::d2::point_xy<T> &v1, 
	const boost::geometry::model::d2::point_xy<T> &v2)
{
	return v1.x() * v2.y() - v2.x() * v1.y();
}

// Euclidian measure
template<typename T>
inline T l2(const boost::geometry::model::d2::point_xy<T> &v)
{
	return std::sqrt(dot(v));
}

// Euclidian measure
template<typename T>
inline T mag(const boost::geometry::model::d2::point_xy<T> &v)
{
	return l2(v);
}

template<typename T>
inline T dist2_to_line(
	const boost::geometry::model::d2::point_xy<T> &p0,
	const boost::geometry::model::d2::point_xy<T> &p1,
	const boost::geometry::model::d2::point_xy<T> &px)
{
	boost::geometry::model::d2::point_xy<T> v  = p1 - p0;
	boost::geometry::model::d2::point_xy<T> vx = px - p0;
	T 									    l  = dot(v);
	T 									    t  = dot(v, vx);
	if (l != T(0) && t > T(0.)) {
		t /= l;
		vx = px - ((t > T(1.)) ? p1 : (p0 + t * v));
	}
	return dot(vx);
}

// Intersect a circle with a line segment.
// Returns number of intersection points.
template<typename T>
int line_circle_intersection(
	const boost::geometry::model::d2::point_xy<T>	&p0,
	const boost::geometry::model::d2::point_xy<T>	&p1,
	const boost::geometry::model::d2::point_xy<T>	&center,
	const T 										 radius,
	boost::geometry::model::d2::point_xy<T>			 intersection[2])
{
	typedef typename V2<T>::Type V2T;
	V2T v  = p1 - p0;
	V2T vc = p0 - center;
	T   a = dot(v);
	T   b = T(2.) * dot(vc, v);
	T   c = dot(vc) - radius * radius;
	T   d = b * b - T(4.) * a * c;

	if (d < T(0))
		// The circle misses the ray.
		return 0;

	int n = 0;
	if (d == T(0)) {
		// The circle touches the ray at a single tangent point.
		T t = - b / (T(2.) * a);
		if (t >= T(0.) && t <= T(1.))
			intersection[n ++] = p0 + t * v;
	} else {
		// The circle intersects the ray in two points.
		d = sqrt(d);
		T t = (- b - d) / (T(2.) * a);
		if (t >= T(0.) && t <= T(1.))
			intersection[n ++] = p0 + t * v;
		t = (- b + d) / (T(2.) * a);
		if (t >= T(0.) && t <= T(1.))
			intersection[n ++] = p0 + t * v;
	}
	return n;
}

// Sutherland–Hodgman clipping of a rectangle against an AABB.
// Expects the first 4 points of rect to be filled at the beginning.
// The clipping may produce up to 8 points.
// Returns the number of resulting points.
template<typename T>
int clip_rect_by_AABB(
	boost::geometry::model::d2::point_xy<T>			   					         rect[8], 
	const boost::geometry::model::box<boost::geometry::model::d2::point_xy<T> > &aabb)
{
	typedef typename V2<T>::Type V2T;
	V2T  result[8];
	int  nin  = 4;
	int  nout = 0;
	V2T *in   = rect;
	V2T *out  = result;
	// Clip left
	{
		const V2T *S    = in + nin - 1;
		T          left = aabb.min_corner().x();
		for (int i = 0; i < nin; ++i) {
			const V2T &E = in[i];
			if (E.x() == left) {
				out[nout++] = E;
			}
			else if (E.x() > left) {
				// E is inside the AABB.
				if (S->x() < left) {
					// S is outside the AABB. Calculate an intersection point.
					T t = (left - S->x()) / (E.x() - S->x());
					out[nout++] = V2T(left, S->y() + t * (E.y() - S->y()));
				}
				out[nout++] = E;
			}
			else if (S->x() > left) {
				// S is inside the AABB, E is outside the AABB.
				T t = (left - S->x()) / (E.x() - S->x());
				out[nout++] = V2T(left, S->y() + t * (E.y() - S->y()));
			}
			S = &E;
		}
		assert(nout <= 8);
	}
	// Clip bottom
	{
		std::swap(in, out);
		nin = nout;
		nout = 0;
		const V2T *S      = in + nin - 1;
		T          bottom = aabb.min_corner().y();
		for (int i = 0; i < nin; ++i) {
			const V2T &E = in[i];
			if (E.y() == bottom) {
				out[nout++] = E;
			}
			else if (E.y() > bottom) {
				// E is inside the AABB.
				if (S->y() < bottom) {
					// S is outside the AABB. Calculate an intersection point.
					T t = (bottom - S->y()) / (E.y() - S->y());
					out[nout++] = V2T(S->x() + t * (E.x() - S->x()), bottom);
				}
				out[nout++] = E;
			}
			else if (S->y() > bottom) {
				// S is inside the AABB, E is outside the AABB.
				T t = (bottom - S->y()) / (E.y() - S->y());
				out[nout++] = V2T(S->x() + t * (E.x() - S->x()), bottom);
			}
			S = &E;
		}
		assert(nout <= 8);
	}
	// Clip right
	{
		std::swap(in, out);
		nin = nout;
		nout = 0;
		const V2T *S = in + nin - 1;
		T right = aabb.max_corner().x();
		for (int i = 0; i < nin; ++i) {
			const V2T &E = in[i];
			if (E.x() == right) {
				out[nout++] = E;
			}
			else if (E.x() < right) {
				// E is inside the AABB.
				if (S->x() > right) {
					// S is outside the AABB. Calculate an intersection point.
					T t = (right - S->x()) / (E.x() - S->x());
					out[nout++] = V2T(right, S->y() + t * (E.y() - S->y()));
				}
				out[nout++] = E;
			}
			else if (S->x() < right) {
				// S is inside the AABB, E is outside the AABB.
				T t = (right - S->x()) / (E.x() - S->x());
				out[nout++] = V2T(right, S->y() + t * (E.y() - S->y()));
			}
			S = &E;
		}
		assert(nout <= 8);
	}
	// Clip top
	{
		std::swap(in, out);
		nin = nout;
		nout = 0;
		const V2T *S = in + nin - 1;
		T top = aabb.max_corner().y();
		for (int i = 0; i < nin; ++i) {
			const V2T &E = in[i];
			if (E.y() == top) {
				out[nout++] = E;
			}
			else if (E.y() < top) {
				// E is inside the AABB.
				if (S->y() > top) {
					// S is outside the AABB. Calculate an intersection point.
					T t = (top - S->y()) / (E.y() - S->y());
					out[nout++] = V2T(S->x() + t * (E.x() - S->x()), top);
				}
				out[nout++] = E;
			}
			else if (S->y() < top) {
				// S is inside the AABB, E is outside the AABB.
				T t = (top - S->y()) / (E.y() - S->y());
				out[nout++] = V2T(S->x() + t * (E.x() - S->x()), top);
			}
			S = &E;
		}
		assert(nout <= 8);
	}

	assert(nout <= 8);
	return nout;
}

// Calculate area of the circle x AABB intersection.
// The calculation is approximate in a way, that the circular segment
// intersecting the cell is approximated by its chord (a linear segment).
template<typename T>
int clip_circle_by_AABB(
	const boost::geometry::model::d2::point_xy<T>							    &center,
	const T 																	 radius,
	const boost::geometry::model::box<boost::geometry::model::d2::point_xy<T> > &aabb,
	boost::geometry::model::d2::point_xy<T>			   					         result[8],
	bool											   					         result_arc[8])
{
	typedef typename V2<T>::Type V2T;

	V2T rect[4] = {
		aabb.min_corner(),
		V2T(aabb.max_corner().x(), aabb.min_corner().y()),
		aabb.max_corner(),
		V2T(aabb.min_corner().x(), aabb.max_corner().y())
	};

	int  bits_corners = 0;
	T    r2 = sqr(radius);
	for (int i = 0; i < 4; ++ i, bits_corners <<= 1)
		bits_corners |= dot(rect[i] - center) >= r2;
	bits_corners >>= 1;

	if (bits_corners == 0) {
		// all inside
		memcpy(result, rect, sizeof(rect));
		memset(result_arc, true, 4);
		return 4;
	}

	if (bits_corners == 0x0f)
		// all outside
		return 0;

	// Some corners are outside, some are inside. Trim the rectangle.
	int n = 0;
	for (int i = 0; i < 4; ++ i) {
		bool inside = (bits_corners & 0x08) == 0;
		bits_corners <<= 1;
		V2T chordal_points[2];
		int n_chordal_points = line_circle_intersection(rect[i], rect[(i + 1)%4], center, radius, chordal_points);
		if (n_chordal_points == 2) {
			result_arc[n] = true;
			result[n ++] = chordal_points[0];
			result_arc[n] = true;
			result[n ++] = chordal_points[1];
		} else {
			if (inside) {
				result_arc[n] = false;
				result[n ++] = rect[i];
			}
			if (n_chordal_points == 1) {
				result_arc[n] = false;
				result[n ++] = chordal_points[0];
			}
		}
	}
	return n;
}
/*
// Calculate area of the circle x AABB intersection.
// The calculation is approximate in a way, that the circular segment
// intersecting the cell is approximated by its chord (a linear segment).
template<typename T>
T circle_AABB_intersection_area(
	const boost::geometry::model::d2::point_xy<T>							    &center,
	const T 																	 radius,
	const boost::geometry::model::box<boost::geometry::model::d2::point_xy<T> > &aabb)
{
	typedef typename V2<T>::Type V2T;
	typedef typename boost::geometry::model::box<V2T> B2T;
	T radius2 = radius * radius;

	bool intersectionLeft   = sqr(aabb.min_corner().x() - center.x()) < radius2;
	bool intersectionRight  = sqr(aabb.max_corner().x() - center.x()) < radius2;
	bool intersectionBottom = sqr(aabb.min_corner().y() - center.y()) < radius2;
	bool intersectionTop    = sqr(aabb.max_corner().y() - center.y()) < radius2;

	if (! (intersectionLeft || intersectionRight || intersectionTop || intersectionBottom))
		// No intersection between the aabb and the center.
		return boost::geometry::point_in_box<V2T, B2T>()::apply(center, aabb) ? 1.f : 0.f;



	V2T rect[4] = {
		aabb.min_corner(),
		V2T(aabb.max_corner().x(), aabb.min_corner().y()),
		aabb.max_corner(),
		V2T(aabb.min_corner().x(), aabb.max_corner().y())
	};

	int  bits_corners = 0;
	T    r2 = sqr(radius);
	for (int i = 0; i < 4; ++ i, bits_corners <<= 1)
		bits_corners |= dot(rect[i] - center) >= r2;
	bits_corners >>= 1;

	if (bits_corners == 0) {
		// all inside
		memcpy(result, rect, sizeof(rect));
		memset(result_arc, true, 4);
		return 4;
	}

	if (bits_corners == 0x0f)
		// all outside
		return 0;

	// Some corners are outside, some are inside. Trim the rectangle.
	int n = 0;
	for (int i = 0; i < 4; ++ i) {
		bool inside = (bits_corners & 0x08) == 0;
		bits_corners <<= 1;
		V2T chordal_points[2];
		int n_chordal_points = line_circle_intersection(rect[i], rect[(i + 1)%4], center, radius, chordal_points);
		if (n_chordal_points == 2) {
			result_arc[n] = true;
			result[n ++] = chordal_points[0];
			result_arc[n] = true;
			result[n ++] = chordal_points[1];
		} else {
			if (inside) {
				result_arc[n] = false;
				result[n ++] = rect[i];
			}
			if (n_chordal_points == 1) {
				result_arc[n] = false;
				result[n ++] = chordal_points[0];
			}
		}
	}
	return n;
}
*/

template<typename T>
inline T polyArea(const boost::geometry::model::d2::point_xy<T> *poly, int n)
{
	T area = T(0);
	for (int i = 1; i + 1 < n; ++i)
		area += cross(poly[i] - poly[0], poly[i + 1] - poly[0]);
	return T(0.5) * area;
}

template<typename T>
boost::geometry::model::d2::point_xy<T> polyCentroid(const boost::geometry::model::d2::point_xy<T> *poly, int n)
{
	boost::geometry::model::d2::point_xy<T> centroid(T(0), T(0));
	for (int i = 0; i < n; ++i)
		centroid += poly[i];
	return (n == 0) ? centroid : (centroid / float(n));
}

void gcode_paint_layer(
	const std::vector<V2f> 	&polyline,
	float					 width,
	float					 thickness,
	A2f 					&acc)
{
	int nc = acc.shape()[1];
	int nr = acc.shape()[0];
//	printf("gcode_paint_layer %d,%d\n", nc, nr);
	for (size_t iLine = 1; iLine != polyline.size(); ++iLine) {
		const V2f &p1 = polyline[iLine - 1];
		const V2f &p2 = polyline[iLine];
		// printf("p1, p2:  %f,%f %f,%f\n", p1.x(), p1.y(), p2.x(), p2.y());
		const V2f  dir = p2 - p1;
		V2f vperp(- dir.y(), dir.x());
		vperp = vperp * 0.5f * width / l2(vperp);
		// Rectangle of the extrusion.
		V2f rect[4] = { p1 + vperp, p1 - vperp, p2 - vperp, p2 + vperp };
		// Bounding box of the extrusion.
		B2f bboxLine(rect[0], rect[0]);
		boost::geometry::expand(bboxLine, rect[1]);
		boost::geometry::expand(bboxLine, rect[2]);
		boost::geometry::expand(bboxLine, rect[3]);
		B2i bboxLinei(
			V2i(std::clamp(int(floor(bboxLine.min_corner().x())), 0, nc-1),
				std::clamp(int(floor(bboxLine.min_corner().y())), 0, nr-1)),
			V2i(std::clamp(int(ceil(bboxLine.max_corner().x())), 0, nc-1),
				std::clamp(int(ceil(bboxLine.max_corner().y())), 0, nr-1)));
		// printf("bboxLinei %d,%d %d,%d\n", bboxLinei.min_corner().x(), bboxLinei.min_corner().y(), bboxLinei.max_corner().x(), bboxLinei.max_corner().y());
#ifdef _DEBUG
		float area = polyArea(rect, 4);
		assert(area > 0.f);
#endif /* _DEBUG */
		for (int j = bboxLinei.min_corner().y(); j + 1 < bboxLinei.max_corner().y(); ++ j) {
			for (int i = bboxLinei.min_corner().x(); i + 1 < bboxLinei.max_corner().x(); ++i) {
				V2f rect2[8];
				memcpy(rect2, rect, sizeof(rect));
				int n = clip_rect_by_AABB(rect2, B2f(V2f(float(i), float(j)), V2f(float(i + 1), float(j + 1))));
				float area = polyArea(rect2, n);
				assert(area >= 0.f && area <= 1.000001f);
				acc[j][i] += area * thickness;
			}
		}
	}
}

void gcode_paint_bitmap(
	const std::vector<V2f> 	&polyline,
	float					 width,
	A2uc 					&bitmap,
	float					 scale)
{
	int nc = bitmap.shape()[1];
	int nr = bitmap.shape()[0];
	float r2 = width * width * 0.25f;
//	printf("gcode_paint_layer %d,%d\n", nc, nr);
	for (size_t iLine = 1; iLine != polyline.size(); ++iLine) {
		const V2f &p1 = polyline[iLine - 1];
		const V2f &p2 = polyline[iLine];
		// printf("p1, p2:  %f,%f %f,%f\n", p1.x(), p1.y(), p2.x(), p2.y());
		V2f dir = p2 - p1;
		dir = dir * 0.5f * width / l2(dir);
		V2f vperp(- dir.y(), dir.x());
		// Rectangle of the extrusion.
		V2f rect[4] = { (p1 + vperp - dir) * scale, (p1 - vperp - dir) * scale, (p2 - vperp + dir) * scale, (p2 + vperp + dir) * scale };
		// Bounding box of the extrusion.
		B2f bboxLine(rect[0], rect[0]);
		boost::geometry::expand(bboxLine, rect[1]);
		boost::geometry::expand(bboxLine, rect[2]);
		boost::geometry::expand(bboxLine, rect[3]);
		B2i bboxLinei(
			V2i(std::clamp(int(floor(bboxLine.min_corner().x())), 0, nc-1),
				std::clamp(int(floor(bboxLine.min_corner().y())), 0, nr-1)),
			V2i(std::clamp(int(ceil(bboxLine.max_corner().x())), 0, nc-1),
				std::clamp(int(ceil(bboxLine.max_corner().y())), 0, nr-1)));
		// printf("bboxLinei %d,%d %d,%d\n", bboxLinei.min_corner().x(), bboxLinei.min_corner().y(), bboxLinei.max_corner().x(), bboxLinei.max_corner().y());
		for (int j = bboxLinei.min_corner().y(); j + 1 < bboxLinei.max_corner().y(); ++ j) {
			for (int i = bboxLinei.min_corner().x(); i + 1 < bboxLinei.max_corner().x(); ++i) {
				float d2 = dist2_to_line(p1, p2, V2f(float(i) + 0.5f, float(j) + 0.5f) / scale);
				if (d2 < r2)
					bitmap[j][i] = 1;
			}
		}
	}
}

struct Cell
{
	// Cell index in the grid.
	V2i   idx;
	// Total volume of the material stored in this cell.
	float volume;
	// Area covered inside this cell, <0,1>.
	float area;
	// Fraction of the area covered by the print head. <0,1>
	float fraction_covered;
	// Height of the covered part in excess to the expected layer height.
	float excess_height;

	bool operator<(const Cell &c2) const {
		return this->excess_height < c2.excess_height;
	}
};

struct ExtrusionPoint {
	V2f   center;
	float radius;
	float height;
};

typedef std::vector<ExtrusionPoint> ExtrusionPoints;

void gcode_spread_points(
	A2f 					&acc,
	const A2f				&mask,
	const ExtrusionPoints   &points, 
	ExtrusionSimulationType simulationType)
{
	int nc = acc.shape()[1];
	int nr = acc.shape()[0];

	// Maximum radius of the spreading points, to allocate a large enough cell array.
	float rmax = 0.f;
	for (ExtrusionPoints::const_iterator it = points.begin(); it != points.end(); ++ it)
		rmax = std::max(rmax, it->radius);
	size_t n_rows_max  = size_t(ceil(rmax * 2.f + 2.f));
	size_t n_cells_max = sqr(n_rows_max);
	std::vector<std::pair<float, float> > spans;
	std::vector<Cell>  cells(n_cells_max, Cell());
	std::vector<float> areas_sum(n_cells_max, 0.f);

	for (ExtrusionPoints::const_iterator it = points.begin(); it != points.end(); ++ it) {
		const V2f  &center = it->center;
		const float radius = it->radius;
		//const float radius2 = radius * radius;
		const float height_target = it->height;
		B2f bbox(center - V2f(radius, radius), center + V2f(radius, radius));
		B2i bboxi(
			V2i(std::clamp(int(floor(bbox.min_corner().x())), 0, nc-1),
				std::clamp(int(floor(bbox.min_corner().y())), 0, nr-1)),
			V2i(std::clamp(int(ceil(bbox.max_corner().x())), 0, nc-1),
				std::clamp(int(ceil(bbox.max_corner().y())), 0, nr-1)));
		/*
		// Fill in the spans, at which the circle intersects the rows.
		int row_first = bboxi.min_corner().y();
		int row_last  = bboxi.max_corner().y();
		for (; row_first <= row_last; ++ row_first) {
			float y     = float(j) - center.y();
			float discr = radius2 - sqr(y);
			if (discr > 0) {
				// Circle intersects the row j at 2 points.
				float d = sqrt(discr);
				spans.push_back(std.pair<float, float>(center.x() - d, center.x() + d)));
				break;
			}
		}
		for (int j = row_first + 1; j <= row_last; ++ j) {
			float y     = float(j) - center.y();
			float discr = radius2 - sqr(y);
			if (discr > 0) {
				// Circle intersects the row j at 2 points.
				float d = sqrt(discr);
				spans.push_back(std.pair<float, float>(center.x() - d, center.x() + d)));
			} else {
				row_last = j - 1;
				break;
			}
		}
		*/
		float area_total     = 0;
		float volume_total   = 0;
		float volume_excess  = 0;
		float volume_deficit = 0;
		size_t n_cells = 0;
		float area_circle_total = 0; 
#if 0
		// The intermediate lines.
		for (int j = row_first; j < row_last; ++ j) {
			const std::pair<float, float> &span1 = spans[j];
			const std::pair<float, float> &span2 = spans[j+1];
			float l1 = span1.first;
			float l2 = span2.first;
			float r1 = span1.second;
			float r2 = span2.second;
			if (l2 < l1)
				std::swap(l1, l2);
			if (r1 > r2)
				std::swap(r1, r2);
			int il1 = int(floor(l1));
			int il2 = int(ceil(l2));
			int ir1 = int(floor(r1));
			int ir2 = int(floor(r2));
			assert(il2 <= ir1);
			for (int i = il1; i < il2; ++ i) {
				Cell &cell = cells[n_cells ++];
				cell.idx.x(i);
				cell.idx.y(j);
				cell.area = area;
			}
			for (int i = il2; i < ir1; ++ i) {
				Cell &cell = cells[n_cells ++];
				cell.idx.x(i);
				cell.idx.y(j);
				cell.area = 1.f;
			}
			for (int i = ir1; i < ir2; ++ i) {
				Cell &cell = cells[n_cells ++];
				cell.idx.x(i);
				cell.idx.y(j);
				cell.area = area;
			}
		}
#else
		for (int j = bboxi.min_corner().y(); j < bboxi.max_corner().y(); ++ j) {
			for (int i = bboxi.min_corner().x(); i < bboxi.max_corner().x(); ++i) {
				B2f bb(V2f(float(i), float(j)), V2f(float(i + 1), float(j + 1)));
				V2f poly[8];
				bool poly_arc[8];
				int n = clip_circle_by_AABB(center, radius, bb, poly, poly_arc);
				float area = polyArea(poly, n);
				assert(area >= 0.f && area <= 1.000001f);
				if (area == 0.f)
					continue;
				Cell &cell = cells[n_cells ++];
				cell.idx.x(i);
				cell.idx.y(j);
				cell.volume  = acc[j][i];
				cell.area    = mask[j][i];
				assert(cell.area >= 0.f && cell.area <= 1.000001f);
				area_circle_total += area;
				if (cell.area < area)
					cell.area = area;
				cell.fraction_covered = std::clamp((cell.area > 0) ? (area / cell.area) : 0, 0.f, 1.f);
				if (cell.fraction_covered == 0) {
					-- n_cells;
					continue;
				}
				float cell_height = cell.volume / cell.area;
				cell.excess_height = cell_height - height_target;
				if (cell.excess_height > 0.f)
					volume_excess  += cell.excess_height * cell.area * cell.fraction_covered;
				else
					volume_deficit -= cell.excess_height * cell.area * cell.fraction_covered;
				volume_total += cell.volume * cell.fraction_covered;
				area_total   += cell.area * cell.fraction_covered;
			}
		}
#endif
//		float area_circle_total2 = float(M_PI) * sqr(radius);
//		float area_err = fabs(area_circle_total2 - area_circle_total) / area_circle_total2;
//		printf("area_circle_total: %f, %f, %f\n", area_circle_total, area_circle_total2, area_err);
		float volume_full = float(M_PI) * sqr(radius) * height_target;
//		if (true) {
//		printf("volume_total: %f, volume_full: %f, fill factor: %f\n", volume_total, volume_full, 100.f - 100.f * volume_total / volume_full);
//		printf("volume_full: %f, volume_excess+deficit: %f, volume_excess: %f, volume_deficit: %f\n", volume_full, volume_excess+volume_deficit, volume_excess, volume_deficit);
		if (simulationType == ExtrusionSimulationSpreadFull || volume_total <= volume_full) {
			// The volume under the circle is spreaded fully.
			float height_avg = volume_total / area_total;
			for (size_t i = 0; i < n_cells; ++ i) {
				const Cell &cell = cells[i];
				acc[cell.idx.y()][cell.idx.x()] = (1.f - cell.fraction_covered) * cell.volume + cell.fraction_covered * cell.area * height_avg;
			}
		} else if (simulationType == ExtrusionSimulationSpreadExcess) {
			// The volume under the circle does not fit.
			// 1) Fill the underfilled cells and remove them from the list.
			float volume_borrowed_total = 0.;
			for (size_t i = 0; i < n_cells;) {
				Cell &cell = cells[i];
				if (cell.excess_height <= 0) {
					// Fill in the part of the cell below the circle.
					float volume_borrowed = - cell.excess_height * cell.area * cell.fraction_covered;
					assert(volume_borrowed >= 0.f);
					acc[cell.idx.y()][cell.idx.x()] = cell.volume + volume_borrowed;
					volume_borrowed_total += volume_borrowed;
					cell = cells[-- n_cells];
				} else
					++ i;
			}
			// 2) Sort the remaining cells by their excess height.
			std::sort(cells.begin(), cells.begin() + n_cells);
			// 3) Prefix sum the areas per excess height.
			// The excess height is discrete with the number of excess cells.
			areas_sum[n_cells-1] = cells[n_cells-1].area * cells[n_cells-1].fraction_covered;
			for (int i = n_cells - 2; i >= 0; -- i) {
				const Cell &cell = cells[i];
				areas_sum[i] = areas_sum[i + 1] + cell.area * cell.fraction_covered;
			}
			// 4) Find the excess height, where the volume_excess is over the volume_borrowed_total.
			float volume_current = 0.f;
			float excess_height_prev = 0.f;
			size_t i_top = n_cells;
			for (size_t i = 0; i < n_cells; ++ i) {
				const Cell &cell = cells[i];
				volume_current += (cell.excess_height - excess_height_prev) * areas_sum[i];
				excess_height_prev = cell.excess_height;
				if (volume_current > volume_borrowed_total) {
					i_top = i;
					break;
				}
			}
			// 5) Remove material from the cells with deficit.
			// First remove all the excess material from the cells, where the deficit is low.
			for (size_t i = 0; i < i_top; ++ i) {
				const Cell &cell = cells[i];
				float volume_removed = cell.excess_height * cell.area * cell.fraction_covered;
				acc[cell.idx.y()][cell.idx.x()] = cell.volume - volume_removed;
				volume_borrowed_total -= volume_removed;
			}
			// Second remove some excess material from the cells, where the deficit is high.
			if (i_top < n_cells) {
				float height_diff = volume_borrowed_total / areas_sum[i_top];
				for (size_t i = i_top; i < n_cells; ++ i) {
					const Cell &cell = cells[i];
					acc[cell.idx.y()][cell.idx.x()] = cell.volume - height_diff * cell.area * cell.fraction_covered;
				}
			}
		}
	}
}

inline std::vector<V3uc> CreatePowerColorGradient24bit()
{
	int i;
	int iColor = 0;
	std::vector<V3uc> out(6 * 255 + 1, V3uc(0, 0, 0));
	for (i = 0; i < 256; ++i)
		out[iColor++] = V3uc(0, 0, i);
	for (i = 1; i < 256; ++i)
		out[iColor++] = V3uc(0, i, 255);
	for (i = 1; i < 256; ++i)
		out[iColor++] = V3uc(0, 255, 256 - i);
	for (i = 1; i < 256; ++i)
		out[iColor++] = V3uc(i, 255, 0);
	for (i = 1; i < 256; ++i)
		out[iColor++] = V3uc(255, 256 - i, 0);
	for (i = 1; i < 256; ++i)
		out[iColor++] = V3uc(255, 0, i);
	return out;
}

class ExtrusionSimulatorImpl {
public:
	std::vector<unsigned char>  image_data;
	A2f							accumulator;
	A2uc						bitmap;
	unsigned int 				bitmap_oversampled;
	ExtrusionPoints 			extrusion_points;
	// RGB gradient to color map the fullness of an accumulator bucket into the output image.
	std::vector<boost::geometry::model::point<unsigned char, 3, boost::geometry::cs::cartesian> > color_gradient;
};

ExtrusionSimulator::ExtrusionSimulator() :
	pimpl(new ExtrusionSimulatorImpl)
{
	pimpl->color_gradient = CreatePowerColorGradient24bit();
	pimpl->bitmap_oversampled = 4;
}

ExtrusionSimulator::~ExtrusionSimulator()
{
	delete pimpl;
	pimpl = NULL;
}

void ExtrusionSimulator::set_image_size(const Point &image_size)
{
	// printf("ExtrusionSimulator::set_image_size()\n");
	if (this->image_size.x() == image_size.x() &&
		this->image_size.y() == image_size.y())
		return;

	// printf("Setting image size: %d, %d\n", image_size.x, image_size.y);
	this->image_size = image_size;
	// Allocate the image data in an RGBA format.
	// printf("Allocating image data, size %d\n", image_size.x * image_size.y * 4);
	pimpl->image_data.assign(image_size.x() * image_size.y() * 4, 0);
	// printf("Allocating image data, allocated\n");

	//FIXME fill the image with red vertical lines.
	for (size_t r = 0; r < size_t(image_size.y()); ++ r) {
		for (size_t c = 0; c < size_t(image_size.x()); c += 2) {
			// Color red
			pimpl->image_data[r * image_size.x() * 4 + c * 4] = 255;
			// Opacity full
			pimpl->image_data[r * image_size.x() * 4 + c * 4 + 3] = 255;
		}
	}
	// printf("Allocating image data, set\n");
}

void ExtrusionSimulator::set_viewport(const BoundingBox &viewport)
{
	// printf("ExtrusionSimulator::set_viewport(%d, %d, %d, %d)\n", viewport.min.x, viewport.min.y, viewport.max.x, viewport.max.y);
	if (this->viewport != viewport) {
		this->viewport = viewport;
		Point sz = viewport.size();
		pimpl->accumulator.resize(boost::extents[sz.y()][sz.x()]);
		pimpl->bitmap.resize(boost::extents[sz.y()*pimpl->bitmap_oversampled][sz.x()*pimpl->bitmap_oversampled]);
		// printf("Accumulator size: %d, %d\n", sz.y, sz.x);
	}
}

void ExtrusionSimulator::set_bounding_box(const BoundingBox &bbox)
{
	this->bbox = bbox;
}

const void* ExtrusionSimulator::image_ptr() const 
{
	return (pimpl->image_data.empty()) ? NULL : (void*)&pimpl->image_data.front();
}

void ExtrusionSimulator::reset_accumulator()
{
	// printf("ExtrusionSimulator::reset_accumulator()\n");
	Point sz = viewport.size();
	// printf("Reset accumulator, Accumulator size: %d, %d\n", sz.y, sz.x);
	memset(&pimpl->accumulator[0][0], 0, sizeof(float) * sz.x() * sz.y());
	memset(&pimpl->bitmap[0][0], 0, sz.x() * sz.y() * pimpl->bitmap_oversampled * pimpl->bitmap_oversampled);
	pimpl->extrusion_points.clear();
	// printf("Reset accumulator, done.\n");
}

void ExtrusionSimulator::extrude_to_accumulator(const ExtrusionPath &path, const Point &shift, ExtrusionSimulationType simulationType)
{
	// printf("Extruding a path. Nr points: %d, width: %f, height: %f\r\n", path.polyline.points.size(), path.width, path.height);
	// Convert the path to V2f points, shift and scale them to the viewport.
	std::vector<V2f> polyline;
	polyline.reserve(path.polyline.points.size());
	float scalex  = float(viewport.size().x()) / float(bbox.size().x());
	float scaley  = float(viewport.size().y()) / float(bbox.size().y());
	float w = scale_(path.width) * scalex;
	//float h = scale_(path.height) * scalex;
	w = scale_(path.mm3_per_mm / path.height) * scalex;
	// printf("scalex: %f, scaley: %f\n", scalex, scaley);
	// printf("bbox: %d,%d %d,%d\n", bbox.min.x(), bbox.min.y, bbox.max.x(), bbox.max.y);
	for (Points::const_iterator it = path.polyline.points.begin(); it != path.polyline.points.end(); ++ it) {
		// printf("point %d,%d\n", it->x+shift.x(), it->y+shift.y);
		ExtrusionPoint ept;
		ept.center = V2f(float((*it)(0)+shift.x()-bbox.min.x()) * scalex, float((*it)(1)+shift.y()-bbox.min.y()) * scaley);
		ept.radius = w/2.f;
		ept.height = 0.5f;
		polyline.push_back(ept.center);
		pimpl->extrusion_points.push_back(ept);
	}
	// Extrude the polyline into an accumulator.
	// printf("width scaled: %f, height scaled: %f\n", w, h);
	gcode_paint_layer(polyline, w, 0.5f, pimpl->accumulator);

 	if (simulationType > ExtrusionSimulationDontSpread)
		gcode_paint_bitmap(polyline, w, pimpl->bitmap, pimpl->bitmap_oversampled);
    // double path.mm3_per_mm;  // mm^3 of plastic per mm of linear head motion
    // float path.width;
    // float path.height;
}

void ExtrusionSimulator::evaluate_accumulator(ExtrusionSimulationType simulationType)
{
	// printf("ExtrusionSimulator::evaluate_accumulator()\n");
	Point sz = viewport.size();

	if (simulationType > ExtrusionSimulationDontSpread) {
		// Average the cells of a bitmap into a lower resolution floating point mask.
		A2f mask(boost::extents[sz.y()][sz.x()]);
		for (int r = 0; r < sz.y(); ++r) {
			for (int c = 0; c < sz.x(); ++c) {
				float p = 0;
				for (unsigned int j = 0; j < pimpl->bitmap_oversampled; ++ j) {
					for (unsigned int i = 0; i < pimpl->bitmap_oversampled; ++ i) {
						if (pimpl->bitmap[r * pimpl->bitmap_oversampled + j][c * pimpl->bitmap_oversampled + i])
							p += 1.f;
					}
				}
				p /= float(pimpl->bitmap_oversampled * pimpl->bitmap_oversampled * 2);
				mask[r][c] = p;
			}
		}

		// Spread the excess of the material.
		gcode_spread_points(pimpl->accumulator, mask, pimpl->extrusion_points, simulationType);
	}

	// Color map the accumulator.
	for (int r = 0; r < sz.y(); ++r) {
		unsigned char *ptr = &pimpl->image_data[(image_size.x() * (viewport.min.y() + r) + viewport.min.x()) * 4];
		for (int c = 0; c < sz.x(); ++c) {
			#if 1
			float p   = pimpl->accumulator[r][c];
			#else
			float p = mask[r][c];
			#endif
			int   idx = int(floor(p * float(pimpl->color_gradient.size()) + 0.5f));
			V3uc  clr = pimpl->color_gradient[std::clamp(idx, 0, int(pimpl->color_gradient.size()-1))];
			*ptr ++ = clr.get<0>();
			*ptr ++ = clr.get<1>();
			*ptr ++ = clr.get<2>();
			*ptr ++ = (idx == 0) ? 0 : 255;
		}
	}
}

} // namespace Slic3r
