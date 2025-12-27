#ifndef slic3r_MultiPoint_hpp_
#define slic3r_MultiPoint_hpp_

#include "libslic3r.h"
#include <algorithm>
#include <vector>
#include "Line.hpp"
#include "Point.hpp"

namespace Slic3r {

class BoundingBox;
class BoundingBox3;

class MultiPoint
{
public:
    Points points;
    
    MultiPoint() {}
    MultiPoint(const MultiPoint &other) : points(other.points) {}
    MultiPoint(MultiPoint &&other) : points(std::move(other.points)) {}
    MultiPoint(std::initializer_list<Point> list) : points(list) {}
    explicit MultiPoint(const Points &_points) : points(_points) {}
    MultiPoint& operator=(const MultiPoint &other) { points = other.points; return *this; }
    MultiPoint& operator=(MultiPoint &&other) { points = std::move(other.points); return *this; }
    void scale(double factor);
    void scale(double factor_x, double factor_y);
    void translate(double x, double y) { this->translate(Point(coord_t(x), coord_t(y))); }
    void translate(const Point &vector);
    void rotate(double angle) { this->rotate(cos(angle), sin(angle)); }
    void rotate(double cos_angle, double sin_angle);
    void rotate(double angle, const Point &center);
    void reverse() { std::reverse(this->points.begin(), this->points.end()); }

    const Point& front() const { return this->points.front(); }
    const Point& back() const { return this->points.back(); }
    const Point& first_point() const { return this->front(); }
    virtual const Point& last_point() const = 0;
    virtual Lines lines() const = 0;
    size_t size() const { return points.size(); }
    bool   empty() const { return points.empty(); }
    double length() const;
    bool   is_valid() const { return this->points.size() >= 2; }

    // Return index of a polygon point exactly equal to point.
    // Return -1 if no such point exists.
    int  find_point(const Point &point) const;
    // Return index of the closest point to point closer than scaled_epsilon.
    // Return -1 if no such point exists.
    int  find_point(const Point &point, const double scaled_epsilon) const;
    bool has_boundary_point(const Point &point) const;
    int  closest_point_index(const Point &point) const {
        int idx = -1;
        if (! this->points.empty()) {
            idx = 0;
            double dist_min = (point - this->points.front()).cast<double>().norm();
            for (int i = 1; i < int(this->points.size()); ++ i) {
                double d = (this->points[i] - point).cast<double>().norm();
                if (d < dist_min) {
                    dist_min = d;
                    idx = i;
                }
            }
        }
        return idx;
    }
    const Point* closest_point(const Point &point) const { return this->points.empty() ? nullptr : &this->points[this->closest_point_index(point)]; }
    // The distance of polygon to point is defined as:
    //  the minimum distance of all points to that point
    double distance_to(const Point& point) const {
        const Point* cl = closest_point(point);
        return (*cl - point).cast<double>().norm();
    }
    BoundingBox bounding_box() const;
    // Return true if there are exact duplicates.
    bool has_duplicate_points() const;
    // Remove exact duplicates, return true if any duplicate has been removed.
    bool remove_duplicate_points();
    void clear() { this->points.clear(); }
    void append(const Point &point) { this->points.push_back(point); }
    void append(const Points &src) { this->append(src.begin(), src.end()); }
    void append(const Points::const_iterator &begin, const Points::const_iterator &end) { this->points.insert(this->points.end(), begin, end); }
    void append(Points &&src)
    {
        if (this->points.empty()) {
            this->points = std::move(src);
        } else {
            this->points.insert(this->points.end(), src.begin(), src.end());
            src.clear();
        }
    }

    bool intersection(const Line& line, Point* intersection) const;
    bool first_intersection(const Line& line, Point* intersection) const;
    bool intersections(const Line &line, Points *intersections) const;
    void symmetric_y(const coord_t &y_axis);
    static Points _douglas_peucker(const Points &points, const double tolerance);
    static Points visivalingam(const Points& pts, const double tolerance);
    static Points concave_hull_2d(const Points& pts, const double tolerence);
    
    //Orca: Distancing function used by IOI wall ordering algorithm for arachne
    static double minimumDistanceBetweenLinesDefinedByPoints(const Points& A, const Points& B);

    inline auto begin()        { return points.begin(); }
    inline auto begin()  const { return points.begin(); }
    inline auto end()          { return points.end();   }
    inline auto end()    const { return points.end();   }
    inline auto cbegin() const { return points.begin(); }
    inline auto cend()   const { return points.end();   }
    
private:
    //Orca: Distancing function used by IOI wall ordering algorithm for arachne
    static double squaredDistanceToLineSegment(const Point& p, const Point& v, const Point& w);
};

class MultiPoint3
{
public:
    Points3 points;

    void append(const Vec3crd& point) { this->points.push_back(point); }

    void translate(double x, double y);
    void translate(const Point& vector);
    virtual Lines3 lines() const = 0;
    double length() const;
    bool is_valid() const { return this->points.size() >= 2; }

    BoundingBox3 bounding_box() const;

    // Remove exact duplicates, return true if any duplicate has been removed.
    bool remove_duplicate_points();
};

extern BoundingBox get_extents(const MultiPoint &mp);
extern BoundingBox get_extents_rotated(const Points &points, double angle);
extern BoundingBox get_extents_rotated(const MultiPoint &mp, double angle);

inline double length(const Points &pts) {
    double total = 0;
    if (! pts.empty()) {
        auto it = pts.begin();
        for (auto it_prev = it ++; it != pts.end(); ++ it, ++ it_prev)
            total += (*it - *it_prev).cast<double>().norm();
    }
    return total;
}

inline double area(const Points &polygon) {
    double area = 0.;
    for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i ++)
		area += double(polygon[i](0) + polygon[j](0)) * double(polygon[i](1) - polygon[j](1));
    return area;
}

} // namespace Slic3r

#endif
