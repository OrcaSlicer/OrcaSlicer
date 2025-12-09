#include "BoundingBox.hpp"
#include "ClipperUtils.hpp"
#include "Exception.hpp"
#include "Polygon.hpp"
#include "Polyline.hpp"

#include <cmath>

namespace Slic3r {

double Polygon::length() const
{
    double l = 0;
    if (this->points.size() > 1) {
        l = (this->points.back() - this->points.front()).cast<double>().norm();
        for (size_t i = 1; i < this->points.size(); ++ i)
            l += (this->points[i] - this->points[i - 1]).cast<double>().norm();
    }
    return l;
}

Lines Polygon::lines() const
{
    return to_lines(*this);
}

Polyline Polygon::split_at_vertex(const Point &point) const
{
    // find index of point
    for (const Point &pt : this->points)
        if (pt == point)
            return this->split_at_index(int(&pt - &this->points.front()));
    throw Slic3r::InvalidArgument("Point not found");
    return Polyline();
}

// Split a closed polygon into an open polyline, with the split point duplicated at both ends.
Polyline Polygon::split_at_index(int index) const
{
    Polyline polyline;
    polyline.points.reserve(this->points.size() + 1);
    for (Points::const_iterator it = this->points.begin() + index; it != this->points.end(); ++it)
        polyline.points.push_back(*it);
    for (Points::const_iterator it = this->points.begin(); it != this->points.begin() + index + 1; ++it)
        polyline.points.push_back(*it);
    return polyline;
}

double Polygon::area(const Points &points)
{
    double a = 0.;
    if (points.size() >= 3) {
        Vec2d p1 = points.back().cast<double>();
        for (const Point &p : points) {
            Vec2d p2 = p.cast<double>();
            a += cross2(p1, p2);
            p1 = p2;
        }
    }
    return 0.5 * a;
}

double Polygon::area() const
{
    return Polygon::area(points);
}

bool Polygon::is_counter_clockwise() const
{
    return ClipperLib::Orientation(this->points);
}

bool Polygon::is_clockwise() const
{
    return !this->is_counter_clockwise();
}

bool Polygon::make_counter_clockwise()
{
    if (!this->is_counter_clockwise()) {
        this->reverse();
        return true;
    }
    return false;
}

bool Polygon::make_clockwise()
{
    if (this->is_counter_clockwise()) {
        this->reverse();
        return true;
    }
    return false;
}

void Polygon::douglas_peucker(double tolerance)
{
    this->points.push_back(this->points.front());
    Points p = MultiPoint::_douglas_peucker(this->points, tolerance);
    p.pop_back();
    this->points = std::move(p);
}

Polygons Polygon::simplify(double tolerance) const
{
    // Works on CCW polygons only, CW contour will be reoriented to CCW by Clipper's simplify_polygons()!
    assert(this->is_counter_clockwise());

    // repeat first point at the end in order to apply Douglas-Peucker
    // on the whole polygon
    Points points = this->points;
    points.push_back(points.front());
    Polygon p(MultiPoint::_douglas_peucker(points, tolerance));
    p.points.pop_back();
    
    Polygons pp;
    pp.push_back(p);
    return simplify_polygons(pp);
}

// Only call this on convex polygons or it will return invalid results
void Polygon::triangulate_convex(Polygons* polygons) const
{
    for (Points::const_iterator it = this->points.begin() + 2; it != this->points.end(); ++it) {
        Polygon p;
        p.points.reserve(3);
        p.points.push_back(this->points.front());
        p.points.push_back(*(it-1));
        p.points.push_back(*it);
        
        // this should be replaced with a more efficient call to a merge_collinear_segments() method
        if (p.area() > 0) polygons->push_back(p);
    }
}

// center of mass
// source: https://en.wikipedia.org/wiki/Centroid
Point Polygon::centroid() const
{
    double area_sum = 0.;
    Vec2d  c(0., 0.);
    if (points.size() >= 3) {
        Vec2d p1 = points.back().cast<double>();
        for (const Point &p : points) {
            Vec2d p2 = p.cast<double>();
            double a = cross2(p1, p2);
            area_sum += a;
            c += (p1 + p2) * a;
            p1 = p2;
        }
    }
    return Point(Vec2d(c / (3. * area_sum)));
}

bool Polygon::intersection(const Line &line, Point *intersection) const
{
    if (this->points.size() < 2)
        return false;
    if (Line(this->points.front(), this->points.back()).intersection(line, intersection))
        return true;
    for (size_t i = 1; i < this->points.size(); ++ i)
        if (Line(this->points[i - 1], this->points[i]).intersection(line, intersection))
            return true;
    return false;
}

bool Polygon::first_intersection(const Line& line, Point* intersection) const
{
    if (this->points.size() < 2)
        return false;

    bool   found = false;
    double dmin  = 0.;
    Line l(this->points.back(), this->points.front());
    for (size_t i = 0; i < this->points.size(); ++ i) {
        l.b = this->points[i];
        Point ip;
        if (l.intersection(line, &ip)) {
            if (! found) {
                found = true;
                dmin = (line.a - ip).cast<double>().squaredNorm();
                *intersection = ip;
            } else {
                double d = (line.a - ip).cast<double>().squaredNorm();
                if (d < dmin) {
                    dmin = d;
                    *intersection = ip;
                }
            }
        }
        l.a = l.b;
    }
    return found;
}

bool Polygon::intersections(const Line &line, Points *intersections) const
{
    if (this->points.size() < 2)
        return false;

    size_t intersections_size = intersections->size();
    Line l(this->points.back(), this->points.front());
    for (size_t i = 0; i < this->points.size(); ++ i) {
        l.b = this->points[i];
        Point intersection;
        if (l.intersection(line, &intersection))
            intersections->emplace_back(std::move(intersection));
        l.a = l.b;
    }
    return intersections->size() > intersections_size;
}

bool Polygon::overlaps(const Polygons& other) const
{
    if (this->empty() || other.empty())
        return false;
    Polylines pl_out = intersection_pl(to_polylines(other), *this);

    // See unit test SCENARIO("Clipper diff with polyline", "[Clipper]")
    // for in which case the intersection_pl produces any intersection.
    return !pl_out.empty() ||
        // If *this is completely inside other, then pl_out is empty, but the expolygons overlap. Test for that situation.
        std::any_of(other.begin(), other.end(), [this](auto& poly) {return poly.contains(this->points.front()); });
}

// Filter points from poly to the output with the help of FilterFn.
// filter function receives two vectors:
// v1: this_point - previous_point
// v2: next_point - this_point
// and returns true if the point is to be copied to the output.
template<typename FilterFn>
Points filter_points_by_vectors(const Points &poly, FilterFn filter)
{
    // Last point is the first point visited.
    Point p1 = poly.back();
    // Previous vector to p1.
    Vec2d v1 = (p1 - *(poly.end() - 2)).cast<double>();

    Points out;
    for (Point p2 : poly) {
        // p2 is next point to the currently visited point p1.
        Vec2d v2 = (p2 - p1).cast<double>();
	// std::cerr << ((void*) &poly) << ": p1=" << p1 << "\tp2=" << p2 << "\tv1="<<v1<<"\tv2="<<v2;
        if (filter(v1, v2))
            out.emplace_back(p1);
	// std::cerr << "\n";
        v1 = v2;
        p1 = p2;
    }
    
    return out;
}

/**
 * @brief Filters points in a polygon based on a minimum angle threshold and a convex/concave criterion.
 *
 * This function iterates through the vertices of the input polygon and selects
 * points where the internal angle meets or exceeds the specified \p angle_threshold
 * and the point satisfies the condition defined by the \p convex_concave_filter.
 *
 * @tparam ConvexConcaveFilterFn The type of the callable filter object (e.g., function pointer, lambda, functor)
 * that determines if a point is considered convex or concave.
 * First vector is incoming line segment (ending at point), second vector is leaving current point.
 * It should have the signature `bool(const Vec2d&, const Vec2d&)`
 * @param poly Vertices of the input polygon.
 * @param angle_threshold The **minimum** angle (in radians) that the internal angle at a vertex must meet or exceed. Must be less than Pi (because angle is always positive) and greater than zero.
 * @param convex_concave_filter A callable object that returns `true` if the point should be included
 * (e.g., if it's convex), and `false` otherwise.
 * @return Points Point objects that meet both the angle threshold and the convex/concave filter criterion.
 */
template<typename ConvexConcaveFilterFn>
Points filter_convex_concave_points_by_angle_threshold(const Points &poly, double angle_threshold, ConvexConcaveFilterFn convex_concave_filter)
{
    // The filter function is typically cross2(v1, v2) {>,<} 0
    assert(angle_threshold >= 0.);
    assert(angle_threshold < M_PI);
    if (angle_threshold > EPSILON) {
	// The methods con{cave,vex}_points are documented as
	//   "with the angle at the vertex larger than a threshold."
	// Due to the imprecision of floating point, this is difficult to get exactly right.
	// So I'm adding just enough here that an input of (M_PI/2) does not match a right angle.
	// Which doesn't mean it'll be correct for all values.
	// And we might learn people actually want "at or larger than threshold" instead.
        double cos_threshold  = cos(std::nextafter(angle_threshold, +INFINITY));
        return filter_points_by_vectors(poly, [convex_concave_filter, cos_threshold](const Vec2d &v1, const Vec2d &v2){
	    if (!convex_concave_filter(v1, v2)) { /*std::cerr << "FIL_FALS";*/ return false; }
	    // Math lesson: Dot product is the product of the magnitudes and the cos(angle) between them.
	    // So if we normalize both, their magnitudes are 1 and, thus, the dot product is cos(angle)
	    // So we want to ensure we only pick angles *bigger* than our angle_threshold
	    // cos(θ) goes 1->-1 as θ=0->Pi , the opposite direction
	    // So if we want angle_vectors > angle_threshold
	    // we must check dot_product_of_vectors < cos(angle_threshold)
	    auto vec_dot = v1.normalized().dot(v2.normalized());
	    // std::cerr << "\tvec_dot="<<vec_dot<<"\tcos_threshold="<<cos_threshold;
            if ( vec_dot < cos_threshold ) {
		// std::cerr << "TRUE";
		return true;
	    }
	    // std::cerr <<"DOT_FALS";
	    return false;
        });
    } else {
        return filter_points_by_vectors(poly, convex_concave_filter);
    }
}

Points Polygon::convex_points(double angle_threshold) const
{
    return filter_convex_concave_points_by_angle_threshold(this->points, angle_threshold, [](const Vec2d &v1, const Vec2d &v2){ return cross2(v1, v2) > 0.; });
}

Points Polygon::concave_points(double angle_threshold) const
{
    return filter_convex_concave_points_by_angle_threshold(this->points, angle_threshold, [](const Vec2d &v1, const Vec2d &v2){ return cross2(v1, v2) < 0.; });
}

// Projection of a point onto the polygon.
Point Polygon::point_projection(const Point &point) const
{
    Point proj = point;
    double dmin = std::numeric_limits<double>::max();
    if (! this->points.empty()) {
        for (size_t i = 0; i < this->points.size(); ++ i) {
            const Point &pt0 = this->points[i];
            const Point &pt1 = this->points[(i + 1 == this->points.size()) ? 0 : i + 1];
            double d = (point - pt0).cast<double>().norm();
            if (d < dmin) {
                dmin = d;
                proj = pt0;
            }
            d = (point - pt1).cast<double>().norm();
            if (d < dmin) {
                dmin = d;
                proj = pt1;
            }
            Vec2d v1(coordf_t(pt1(0) - pt0(0)), coordf_t(pt1(1) - pt0(1)));
            coordf_t div = v1.squaredNorm();
            if (div > 0.) {
                Vec2d v2(coordf_t(point(0) - pt0(0)), coordf_t(point(1) - pt0(1)));
                coordf_t t = v1.dot(v2) / div;
                if (t > 0. && t < 1.) {
                    Point foot(coord_t(floor(coordf_t(pt0(0)) + t * v1(0) + 0.5)), coord_t(floor(coordf_t(pt0(1)) + t * v1(1) + 0.5)));
                    d = (point - foot).cast<double>().norm();
                    if (d < dmin) {
                        dmin = d;
                        proj = foot;
                    }
                }
            }
        }
    }
    return proj;
}

std::vector<float> Polygon::parameter_by_length() const
{
    // Parametrize the polygon by its length.
    std::vector<float> lengths(points.size()+1, 0.);
    for (size_t i = 1; i < points.size(); ++ i)
        lengths[i] = lengths[i-1] + (points[i] - points[i-1]).cast<float>().norm();
    lengths.back() = lengths[lengths.size()-2] + (points.front() - points.back()).cast<float>().norm();
    return lengths;
}

void Polygon::densify(float min_length, std::vector<float>* lengths_ptr)
{
    std::vector<float> lengths_local;
    std::vector<float>& lengths = lengths_ptr ? *lengths_ptr : lengths_local;

    if (! lengths_ptr) {
        // Length parametrization has not been provided. Calculate our own.
        lengths = this->parameter_by_length();
    }

    assert(points.size() == lengths.size() - 1);

    for (size_t j=1; j<=points.size(); ++j) {
        bool last = j == points.size();
        int i = last ? 0 : j;

        if (lengths[j] - lengths[j-1] > min_length) {
            Point diff = points[i] - points[j-1];
            float diff_len = lengths[j] - lengths[j-1];
            float r = (min_length/diff_len);
            Point new_pt = points[j-1] + Point(r*diff[0], r*diff[1]);
            points.insert(points.begin() + j, new_pt);
            lengths.insert(lengths.begin() + j, lengths[j-1] + min_length);
        }
    }
    assert(points.size() == lengths.size() - 1);
}

Polygon Polygon::transform(const Transform3d& trafo) const
{
    unsigned int vertices_count = (unsigned int)points.size();
    Polygon dstpoly;
    dstpoly.points.resize(vertices_count);
    if (vertices_count == 0)
        return dstpoly;

    unsigned int data_size = 3 * vertices_count * sizeof(float);

    Eigen::MatrixXd src(3, vertices_count);
    for (size_t i = 0; i < vertices_count; i++)
    {
        src.col(i) = Vec3d{ double(points[i].x()), double(points[i].y()),0. };
    }

    Eigen::MatrixXd dst(3, vertices_count);
    dst = trafo * src.colwise().homogeneous();

    for (size_t i = 0; i < vertices_count; i++)
    {
        dstpoly.points[i] = { dst(0,i),dst(1,i) };
    }
    return dstpoly;
}

BoundingBox get_extents(const Polygon &poly) 
{ 
    return poly.bounding_box();
}

BoundingBox get_extents(const Polygons &polygons)
{
    BoundingBox bb;
    if (! polygons.empty()) {
        bb = get_extents(polygons.front());
        for (size_t i = 1; i < polygons.size(); ++ i)
            bb.merge(get_extents(polygons[i]));
    }
    return bb;
}

BoundingBox get_extents_rotated(const Polygon &poly, double angle) 
{ 
    return get_extents_rotated(poly.points, angle);
}

BoundingBox get_extents_rotated(const Polygons &polygons, double angle)
{
    BoundingBox bb;
    if (! polygons.empty()) {
        bb = get_extents_rotated(polygons.front().points, angle);
        for (size_t i = 1; i < polygons.size(); ++ i)
            bb.merge(get_extents_rotated(polygons[i].points, angle));
    }
    return bb;
}

extern std::vector<BoundingBox> get_extents_vector(const Polygons &polygons)
{
    std::vector<BoundingBox> out;
    out.reserve(polygons.size());
    for (Polygons::const_iterator it = polygons.begin(); it != polygons.end(); ++ it)
        out.push_back(get_extents(*it));
    return out;
}

// Polygon must be valid (at least three points), collinear points and duplicate points removed.
bool polygon_is_convex(const Points &poly)
{
    if (poly.size() < 3)
        return false;

    Point p0 = poly[poly.size() - 2];
    Point p1 = poly[poly.size() - 1];
    for (size_t i = 0; i < poly.size(); ++ i) {
        Point p2 = poly[i];
        auto det = cross2((p1 - p0).cast<int64_t>(), (p2 - p1).cast<int64_t>());
        if (det < 0)
            return false;
        p0 = p1;
        p1 = p2;
    }
    return true;
}

bool has_duplicate_points(const Polygons &polys)
{
#if 1
    // Check globally.
    Points allpts;
    allpts.reserve(count_points(polys));
    for (const Polygon &poly : polys)
        allpts.insert(allpts.end(), poly.points.begin(), poly.points.end());
    return has_duplicate_points(std::move(allpts));
#else
    // Check per contour.
    for (const Polygon &poly : polys)
        if (has_duplicate_points(poly))
            return true;
    return false;
#endif
}

bool remove_same_neighbor(Polygon &polygon)
{
    Points &points = polygon.points;
    if (points.empty())
        return false;
    auto last = std::unique(points.begin(), points.end());

    // remove first and last neighbor duplication
    if (const Point &last_point = *(last - 1); last_point == points.front()) {
        --last;
    }

    // no duplicits
    if (last == points.end())
        return false;

    points.erase(last, points.end());
    return true;
}

bool remove_same_neighbor(Polygons &polygons)
{
    if (polygons.empty())
        return false;
    bool exist = false;
    for (Polygon &polygon : polygons)
        exist |= remove_same_neighbor(polygon);
    // remove empty polygons
    polygons.erase(std::remove_if(polygons.begin(), polygons.end(), [](const Polygon &p) { return p.points.size() <= 2; }), polygons.end());
    return exist;
}

static inline bool is_stick(const Point &p1, const Point &p2, const Point &p3)
{
    Point v1 = p2 - p1;
    Point v2 = p3 - p2;
    int64_t dir = int64_t(v1(0)) * int64_t(v2(0)) + int64_t(v1(1)) * int64_t(v2(1));
    if (dir > 0)
        // p3 does not turn back to p1. Do not remove p2.
        return false;
    double l2_1 = double(v1(0)) * double(v1(0)) + double(v1(1)) * double(v1(1));
    double l2_2 = double(v2(0)) * double(v2(0)) + double(v2(1)) * double(v2(1));
    if (dir == 0)
        // p1, p2, p3 may make a perpendicular corner, or there is a zero edge length.
        // Remove p2 if it is coincident with p1 or p2.
        return l2_1 == 0 || l2_2 == 0;
    // p3 turns back to p1 after p2. Are p1, p2, p3 collinear?
    // Calculate distance from p3 to a segment (p1, p2) or from p1 to a segment(p2, p3),
    // whichever segment is longer
    double cross = double(v1(0)) * double(v2(1)) - double(v2(0)) * double(v1(1));
    double dist2 = cross * cross / std::max(l2_1, l2_2);
    return dist2 < EPSILON * EPSILON;
}

bool remove_sticks(Polygon &poly)
{
    bool modified = false;
    size_t j = 1;
    for (size_t i = 1; i + 1 < poly.points.size(); ++ i) {
        if (! is_stick(poly[j-1], poly[i], poly[i+1])) {
            // Keep the point.
            if (j < i)
                poly.points[j] = poly.points[i];
            ++ j;
        }
    }
    if (++ j < poly.points.size()) {
        poly.points[j-1] = poly.points.back();
        poly.points.erase(poly.points.begin() + j, poly.points.end());
        modified = true;
    }
    while (poly.points.size() >= 3 && is_stick(poly.points[poly.points.size()-2], poly.points.back(), poly.points.front())) {
        poly.points.pop_back();
        modified = true;
    }
    while (poly.points.size() >= 3 && is_stick(poly.points.back(), poly.points.front(), poly.points[1]))
        poly.points.erase(poly.points.begin());
    return modified;
}

bool remove_sticks(Polygons &polys)
{
    bool modified = false;
    size_t j = 0;
    for (size_t i = 0; i < polys.size(); ++ i) {
        modified |= remove_sticks(polys[i]);
        if (polys[i].points.size() >= 3) {
            if (j < i) 
                std::swap(polys[i].points, polys[j].points);
            ++ j;
        }
    }
    if (j < polys.size())
        polys.erase(polys.begin() + j, polys.end());
    return modified;
}

bool remove_degenerate(Polygons &polys)
{
    bool modified = false;
    size_t j = 0;
    for (size_t i = 0; i < polys.size(); ++ i) {
        if (polys[i].points.size() >= 3) {
            if (j < i) 
                std::swap(polys[i].points, polys[j].points);
            ++ j;
        } else
            modified = true;
    }
    if (j < polys.size())
        polys.erase(polys.begin() + j, polys.end());
    return modified;
}

bool remove_small(Polygons &polys, double min_area)
{
    bool modified = false;
    size_t j = 0;
    for (size_t i = 0; i < polys.size(); ++ i) {
        if (std::abs(polys[i].area()) >= min_area) {
            if (j < i) 
                std::swap(polys[i].points, polys[j].points);
            ++ j;
        } else
            modified = true;
    }
    if (j < polys.size())
        polys.erase(polys.begin() + j, polys.end());
    return modified;
}

void remove_collinear(Polygon &poly)
{
    if (poly.points.size() > 2) {
        // copy points and append both 1 and last point in place to cover the boundaries
        Points pp;
        pp.reserve(poly.points.size()+2);
        pp.push_back(poly.points.back());
        pp.insert(pp.begin()+1, poly.points.begin(), poly.points.end());
        pp.push_back(poly.points.front());
        // delete old points vector. Will be re-filled in the loop
        poly.points.clear();

        size_t i = 0;
        size_t k = 0;
        while (i < pp.size()-2) {
            k = i+1;
            const Point &p1 = pp[i];
            while (k < pp.size()-1) {
                const Point &p2 = pp[k];
                const Point &p3 = pp[k+1];
                Line l(p1, p3);
                if(l.distance_to(p2) < SCALED_EPSILON) {
                    k++;
                } else {
                    if(i > 0) poly.points.push_back(p1); // implicitly removes the first point we appended above
                    i = k;
                    break;
                }
            }
            if(k > pp.size()-2) break; // all remaining points are collinear and can be skipped
        }
        poly.points.push_back(pp[i]);
    }
}

void remove_collinear(Polygons &polys)
{
	for (Polygon &poly : polys)
		remove_collinear(poly);
}

Polygons polygons_simplify(const Polygons &source_polygons, double tolerance, bool strictly_simple /* = true */)
{
    Polygons out;
    out.reserve(source_polygons.size());
    for (const Polygon &source_polygon : source_polygons) {
        // Run Douglas / Peucker simplification algorithm on an open polyline (by repeating the first point at the end of the polyline),
        Points simplified = MultiPoint::_douglas_peucker(to_polyline(source_polygon).points, tolerance);
        // then remove the last (repeated) point.
        simplified.pop_back();
        // Simplify the decimated contour by ClipperLib.
        bool ccw = ClipperLib::Area(simplified) > 0.;
        for (Points &path : ClipperLib::SimplifyPolygons(ClipperUtils::SinglePathProvider(simplified), ClipperLib::pftNonZero, strictly_simple)) {
            if (! ccw)
                // ClipperLib likely reoriented negative area contours to become positive. Reverse holes back to CW.
                std::reverse(path.begin(), path.end());
            out.emplace_back(std::move(path));
        }
    }
    return out;
}

// Do polygons match? If they match, they must have the same topology,
// however their contours may be rotated.
bool polygons_match(const Polygon &l, const Polygon &r)
{
    if (l.size() != r.size())
        return false;
    auto it_l = std::find(l.points.begin(), l.points.end(), r.points.front());
    if (it_l == l.points.end())
        return false;
    auto it_r = r.points.begin();
    for (; it_l != l.points.end(); ++ it_l, ++ it_r)
        if (*it_l != *it_r)
            return false;
    it_l = l.points.begin();
    for (; it_r != r.points.end(); ++ it_l, ++ it_r)
        if (*it_l != *it_r)
            return false;
    return true;
}

bool overlaps(const Polygons& polys1, const Polygons& polys2)
{
    for (const Polygon& poly1 : polys1) {
        if (poly1.overlaps(polys2))
            return true;
    }
    return false;
}

bool contains(const Polygon &polygon, const Point &p, bool border_result)
{
    if (const int poly_count_inside = ClipperLib::PointInPolygon(p, polygon.points); 
        poly_count_inside == -1)
        return border_result;
    else
        return (poly_count_inside % 2) == 1;
}

bool contains(const Polygons &polygons, const Point &p, bool border_result)
{
    int poly_count_inside = 0;
    for (const Polygon &poly : polygons) {
        const int is_inside_this_poly = ClipperLib::PointInPolygon(p, poly.points);
        if (is_inside_this_poly == -1)
            return border_result;
        poly_count_inside += is_inside_this_poly;
    }
    return (poly_count_inside % 2) == 1;
}

Polygon make_circle(double radius, double error)
{
    double angle = 2. * acos(1. - error / radius);
    size_t num_segments = size_t(ceil(2. * M_PI / angle));
    return make_circle_num_segments(radius, num_segments);
}

Polygon make_circle_num_segments(double radius, size_t num_segments)
{
    Polygon out;
    out.points.reserve(num_segments);
    double angle_inc = 2.0 * M_PI / num_segments;
    for (size_t i = 0; i < num_segments; ++ i) {
        const double angle = angle_inc * i;
        out.points.emplace_back(coord_t(cos(angle) * radius), coord_t(sin(angle) * radius));
    }
    return out;
}
}
