#include "Point.hpp"
#include "Exception.hpp"
#include "Line.hpp"
#include "MultiPoint.hpp"
#include "Polyline.hpp"
#include "Int128.hpp"
#include "BoundingBox.hpp"
#include <algorithm>

namespace Slic3r {

std::vector<Vec3f> transform(const std::vector<Vec3f>& points, const Transform3f& t)
{
    unsigned int vertices_count = (unsigned int)points.size();
    if (vertices_count == 0)
        return std::vector<Vec3f>();

    unsigned int data_size = 3 * vertices_count * sizeof(float);

    Eigen::MatrixXf src(3, vertices_count);
    ::memcpy((void*)src.data(), (const void*)points.data(), data_size);

    Eigen::MatrixXf dst(3, vertices_count);
    dst = t * src.colwise().homogeneous();

    std::vector<Vec3f> ret_points(vertices_count, Vec3f::Zero());
    ::memcpy((void*)ret_points.data(), (const void*)dst.data(), data_size);
    return ret_points;
}

Pointf3s transform(const Pointf3s& points, const Transform3d& t)
{
    unsigned int vertices_count = (unsigned int)points.size();
    if (vertices_count == 0)
        return Pointf3s();

    unsigned int data_size = 3 * vertices_count * sizeof(double);

    Eigen::MatrixXd src(3, vertices_count);
    ::memcpy((void*)src.data(), (const void*)points.data(), data_size);

    Eigen::MatrixXd dst(3, vertices_count);
    dst = t * src.colwise().homogeneous();

    Pointf3s ret_points(vertices_count, Vec3d::Zero());
    ::memcpy((void*)ret_points.data(), (const void*)dst.data(), data_size);
    return ret_points;
}

void Point::rotate(double angle, const Point &center)
{
    Vec2d  cur = this->cast<double>();
    double s   = ::sin(angle);
    double c   = ::cos(angle);
    Vec2d  d   = cur - center.cast<double>();
    this->x() = fast_round_up<coord_t>(center.x() + c * d.x() - s * d.y());
    this->y() = fast_round_up<coord_t>(center.y() + s * d.x() + c * d.y());
}

/* Three points are a counter-clockwise turn if ccw > 0, clockwise if
 * ccw < 0, and collinear if ccw = 0 because ccw is a determinant that
 * gives the signed area of the triangle formed by p1, p2 and this point.
 * In other words it is the 2D cross product of p1-p2 and p1-this, i.e.
 * z-component of their 3D cross product.
 * We return double because it must be big enough to hold 2*max(|coordinate|)^2
 */
double Point::ccw(const Point &p1, const Point &p2) const
{
    // static_assert(sizeof(coord_t) == 4, "Point::ccw() requires a 32 bit coord_t");
    // return cross2((p2 - p1).cast<int64_t>(), (*this - p1).cast<int64_t>());
   return cross2((p2 - p1).cast<double>(), (*this - p1).cast<double>());
}

double Point::ccw(const Line &line) const
{
    return this->ccw(line.a, line.b);
}

// returns the CCW angle between this-p1 and this-p2
// i.e. this assumes a CCW rotation from p1 to p2 around this
double Point::ccw_angle(const Point &p1, const Point &p2) const
{
    //FIXME this calculates an atan2 twice! Project one vector into the other!
    double angle = atan2(p1.x() - (*this).x(), p1.y() - (*this).y())
                 - atan2(p2.x() - (*this).x(), p2.y() - (*this).y());
    // we only want to return only positive angles
    return angle <= 0 ? angle + 2*PI : angle;
}

Point Point::projection_onto(const MultiPoint &poly) const
{
    Point running_projection = poly.first_point();
    double running_min = (running_projection - *this).cast<double>().norm();
    
    Lines lines = poly.lines();
    for (Lines::const_iterator line = lines.begin(); line != lines.end(); ++line) {
        Point point_temp = this->projection_onto(*line);
        if ((point_temp - *this).cast<double>().norm() < running_min) {
	        running_projection = point_temp;
	        running_min = (running_projection - *this).cast<double>().norm();
        }
    }
    return running_projection;
}

Point Point::projection_onto(const Line &line) const
{
    if (line.a == line.b) return line.a;
    
    /*
        (Ported from VisiLibity by Karl J. Obermeyer)
        The projection of point_temp onto the line determined by
        line_segment_temp can be represented as an affine combination
        expressed in the form projection of
        Point = theta*line_segment_temp.first + (1.0-theta)*line_segment_temp.second.
        If theta is outside the interval [0,1], then one of the Line_Segment's endpoints
        must be closest to calling Point.
    */
    double lx = (double)(line.b(0) - line.a(0));
    double ly = (double)(line.b(1) - line.a(1));
    double theta = ( (double)(line.b(0) - (*this)(0))*lx + (double)(line.b(1)- (*this)(1))*ly ) 
          / ( sqr<double>(lx) + sqr<double>(ly) );
    
    if (0.0 <= theta && theta <= 1.0)
        return (theta * line.a.cast<coordf_t>() + (1.0-theta) * line.b.cast<coordf_t>()).cast<coord_t>();
    
    // Else pick closest endpoint.
    return ((line.a - *this).cast<double>().squaredNorm() < (line.b - *this).cast<double>().squaredNorm()) ? line.a : line.b;
}

bool has_duplicate_points(Points &&pts)
{
    std::sort(pts.begin(), pts.end());
    for (size_t i = 1; i < pts.size(); ++ i)
        if (pts[i - 1] == pts[i])
            return true;
    return false;
}

Points collect_duplicates(Points pts /* Copy */)
{
    std::sort(pts.begin(), pts.end());
    Points duplicits;
    const Point *prev = &pts.front();
    for (size_t i = 1; i < pts.size(); ++i) {
        const Point *act = &pts[i];
        if (*prev == *act) {
            // duplicit point
            if (!duplicits.empty() && duplicits.back() == *act)
                continue; // only unique duplicits
            duplicits.push_back(*act);
        }
        prev = act;
    }
    return duplicits;
}

template<bool IncludeBoundary>
BoundingBox get_extents(const Points &pts)
{ 
    BoundingBox out;
    BoundingBox::construct<IncludeBoundary>(out, pts.begin(), pts.end());
    return out;
}
template BoundingBox get_extents<false>(const Points &pts);
template BoundingBox get_extents<true>(const Points &pts);

// if IncludeBoundary, then a bounding box is defined even for a single point.
// otherwise a bounding box is only defined if it has a positive area.
template<bool IncludeBoundary>
BoundingBox get_extents(const VecOfPoints &pts)
{
    BoundingBox bbox;
    for (const Points &p : pts)
        bbox.merge(get_extents<IncludeBoundary>(p));
    return bbox;
}
template BoundingBox get_extents<false>(const VecOfPoints &pts);
template BoundingBox get_extents<true>(const VecOfPoints &pts);

BoundingBoxf get_extents(const std::vector<Vec2d> &pts)
{
    BoundingBoxf bbox;
    for (const Vec2d &p : pts)
        bbox.merge(p);
    return bbox;
}


int Point::nearest_point_index(const Points &points) const
{
    PointConstPtrs p;
    p.reserve(points.size());
    for (Points::const_iterator it = points.begin(); it != points.end(); ++it)
        p.push_back(&*it);
    return this->nearest_point_index(p);
}

int Point::nearest_point_index(const PointConstPtrs &points) const
{
    int idx = -1;
    double distance = -1;  // double because long is limited to 2147483647 on some platforms and it's not enough
    
    for (PointConstPtrs::const_iterator it = points.begin(); it != points.end(); ++it) {
        /* If the X distance of the candidate is > than the total distance of the
           best previous candidate, we know we don't want it */
        double d = sqr<double>((*this)(0) - (*it)->x());
        if (distance != -1 && d > distance) continue;
        
        /* If the Y distance of the candidate is > than the total distance of the
           best previous candidate, we know we don't want it */
        d += sqr<double>((*this)(1) - (*it)->y());
        if (distance != -1 && d > distance) continue;
        
        idx = it - points.begin();
        distance = d;
        
        if (distance < EPSILON) break;
    }
    
    return idx;
}

int Point::nearest_point_index(const PointPtrs &points) const
{
    PointConstPtrs p;
    p.reserve(points.size());
    for (PointPtrs::const_iterator it = points.begin(); it != points.end(); ++it)
        p.push_back(*it);
    return this->nearest_point_index(p);
}

bool Point::nearest_point(const Points &points, Point* point) const
{
    int idx = this->nearest_point_index(points);
    if (idx == -1) return false;
    *point = points.at(idx);
    return true;
}

std::ostream& operator<<(std::ostream &stm, const Vec2d &pointf)
{
    return stm << pointf(0) << "," << pointf(1);
}

namespace int128 {

int orient(const Vec2crd &p1, const Vec2crd &p2, const Vec2crd &p3)
{
    Slic3r::Vector v1(p2 - p1);
    Slic3r::Vector v2(p3 - p1);
    return Int128::sign_determinant_2x2_filtered(v1.x(), v1.y(), v2.x(), v2.y());
}

int cross(const Vec2crd &v1, const Vec2crd &v2)
{
    return Int128::sign_determinant_2x2_filtered(v1.x(), v1.y(), v2.x(), v2.y());
}

}

// Point3 utility functions for ZAA (Z Anti-Aliasing)
Polyline to_polyline(const Points &points) { return Polyline(points); }
Polyline3 to_polyline(const Points3 &points) { return Polyline3(points); }

Points to_points(const Points3 &points3) {
    Points points2;
    points2.reserve(points3.size());
    for (const Point3 &pt : points3) {
        points2.emplace_back(pt.to_point());
    }
    return points2;
}

Points3 to_points3(const Points& points)
{
    Points3 points3;
    points3.reserve(points.size());
    for (const Point& pt : points) {
        points3.emplace_back(pt);
    }
    return points3;
}

// Point3 method implementations
void Point3::rotate(double angle, const Point3 &center) {
    Vec3crd diff = *this - center;
    Point3 temp(diff.x(), diff.y(), diff.z());
    temp.rotate(angle);
    Vec3crd sum = temp + center;
    *this = Point3(sum.x(), sum.y(), sum.z());
}

int Point3::nearest_point_index(const Points &points) const {
    return this->to_point().nearest_point_index(points);
}

bool Point3::nearest_point(const Points &points, Point3* point) const {
    Point pt2d;
    bool result = this->to_point().nearest_point(points, &pt2d);
    if (result && point) {
        *point = Point3(pt2d, this->z());
    }
    return result;
}

double Point3::ccw(const Point3 &p1, const Point3 &p2) const {
    return this->to_point().ccw(p1.to_point(), p2.to_point());
}

double Point3::ccw(const Line3 &line) const {
    // Convert to 2D and use existing Point ccw implementation
    Point a2d(line.a.x(), line.a.y());
    Point b2d(line.b.x(), line.b.y());
    return this->to_point().ccw(Line(a2d, b2d));
}

double Point3::ccw_angle(const Point3 &p1, const Point3 &p2) const {
    return this->to_point().ccw_angle(p1.to_point(), p2.to_point());
}

Point3 Point3::projection_onto(const MultiPoint3 &poly) const {
    // TODO: Implement proper 3D projection when MultiPoint3 conversion methods are ready
    // For now, stub implementation
    throw RuntimeError("Point3::projection_onto(MultiPoint3) not implemented yet");
    return *this;
}

Point3 Point3::projection_onto(const Line3 &line) const {
    // Project in 2D plane and interpolate Z
    Point pt2d = this->to_point();
    Point line_a(line.a.x(), line.a.y());
    Point line_b(line.b.x(), line.b.y());
    Line line2d(line_a, line_b);
    Point proj2d = pt2d.projection_onto(line2d);

    // Interpolate Z coordinate
    double line_len = line.length();
    if (line_len < EPSILON) {
        return Point3(proj2d, line.a.z());
    }
    double dist_from_a = (proj2d - line_a).cast<double>().norm();
    double t = dist_from_a / line_len;
    t = std::clamp(t, 0.0, 1.0);
    coord_t z = coord_t(line.a.z() + t * (line.b.z() - line.a.z()));
    return Point3(proj2d, z);
}

}
