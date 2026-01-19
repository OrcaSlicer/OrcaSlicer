#include "Geometry.hpp"
#include "Line.hpp"
#include "Polyline.hpp"
#include <algorithm>
#include <cmath>
#include <sstream>

namespace Slic3r {

Linef3 transform(const Linef3& line, const Transform3d& t)
{
    typedef Eigen::Matrix<double, 3, 2> LineInMatrixForm;

    LineInMatrixForm world_line;
    ::memcpy((void*)world_line.col(0).data(), (const void*)line.a.data(), 3 * sizeof(double));
    ::memcpy((void*)world_line.col(1).data(), (const void*)line.b.data(), 3 * sizeof(double));

    LineInMatrixForm local_line = t * world_line.colwise().homogeneous();
    return Linef3(Vec3d(local_line(0, 0), local_line(1, 0), local_line(2, 0)), Vec3d(local_line(0, 1), local_line(1, 1), local_line(2, 1)));
}

bool Line::intersection_infinite(const Line &other, Point* point) const
{
    Vec2d a1 = this->a.cast<double>();
    Vec2d v12 = (other.a - this->a).cast<double>();
    Vec2d v1 = (this->b - this->a).cast<double>();
    Vec2d v2 = (other.b - other.a).cast<double>();
    double denom = cross2(v1, v2);
    if (std::fabs(denom) < EPSILON)
        return false;
    double t1 = cross2(v12, v2) / denom;
    Vec2d result = (a1 + t1 * v1);
    if (result.x() > std::numeric_limits<coord_t>::max() || result.x() < std::numeric_limits<coord_t>::lowest() ||
        result.y() > std::numeric_limits<coord_t>::max() || result.y() < std::numeric_limits<coord_t>::lowest()) {
        // Intersection has at least one of the coordinates much bigger (or smaller) than coord_t maximum value (or minimum).
        // So it can not be stored into the Point without integer overflows. That could mean that input lines are parallel or near parallel.
        return false;
    }
    *point = (result).cast<coord_t>();
    return true;
}

double Line::perp_distance_to(const Point &point) const
{
    const Line  &line = *this;
    const Vec2d  v  = (line.b - line.a).cast<double>();
    const Vec2d  va = (point - line.a).cast<double>();
    if (line.a == line.b)
        return va.norm();
    return std::abs(cross2(v, va)) / v.norm();
}

double Line::orientation() const
{
    double angle = this->atan2_();
    if (angle < 0) angle = 2*PI + angle;
    return angle;
}

double Line::direction() const
{
    double atan2 = this->atan2_();
    return (fabs(atan2 - PI) < EPSILON) ? 0
        : (atan2 < 0) ? (atan2 + PI)
        : atan2;
}

bool Line::parallel_to(double angle) const
{
    return Slic3r::Geometry::directions_parallel(this->direction(), angle);
}

bool Line::parallel_to(const Line& line) const
{
    const Vec2d v1 = (this->b - this->a).cast<double>();
    const Vec2d v2 = (line.b - line.a).cast<double>();
    return sqr(cross2(v1, v2)) < sqr(EPSILON) * v1.squaredNorm() * v2.squaredNorm();
}
bool Line::overlap(const Line &line, double &overlap_length) const
{
    if (!this->parallel_to(line)) return false;
    Line line_(this->a, line.a);
    if (line_.length() > scaled(EPSILON) && !this->parallel_to(line_)) return false;
    coord_t a_min  = std::min(this->a.x(), this->b.x());
    coord_t a_max  = std::max(this->a.x(), this->b.x());
    coord_t b_min  = std::min(line.a.x(), line.b.x());
    coord_t b_max  = std::max(line.a.x(), line.b.x());
    if (a_min>b_max||a_max<b_min) return false;
    overlap_length = std::max((coord_t)0, std::min(a_max, b_max) - std::max(a_min, b_min));
    overlap_length /= ((double) a_max - a_min) / this->length();
    return true;
}
bool Line::perpendicular_to(double angle) const
{
    return Slic3r::Geometry::directions_perpendicular(this->direction(), angle);
}

bool Line::perpendicular_to(const Line& line) const
{
    const Vec2d v1 = (this->b - this->a).cast<double>();
    const Vec2d v2 = (line.b - line.a).cast<double>();
    return sqr(v1.dot(v2)) < sqr(EPSILON) * v1.squaredNorm() * v2.squaredNorm();
}

bool Line::intersection(const Line &l2, Point *intersection) const
{
    return line_alg::intersection(*this, l2, intersection);
}

bool Line::clip_with_bbox(const BoundingBox &bbox)
{
	Vec2d x0clip, x1clip;
	bool result = Geometry::liang_barsky_line_clipping<double>(this->a.cast<double>(), this->b.cast<double>(), BoundingBoxf(bbox.min.cast<double>(), bbox.max.cast<double>()), x0clip, x1clip);
	if (result) {
		this->a = x0clip.cast<coord_t>();
		this->b = x1clip.cast<coord_t>();
	}
	return result;
}

void Line::extend(double offset)
{
    Vector offset_vector = (offset * this->vector().cast<double>().normalized()).cast<coord_t>();
    this->a -= offset_vector;
    this->b += offset_vector;
}

Vec3d Linef3::intersect_plane(double z) const
{
    Vec3d  v = (this->b - this->a).cast<double>();
    double t = (z - this->a(2)) / v(2);
    return Vec3d(this->a(0) + v(0) * t, this->a(1) + v(1) * t, z);
}

BoundingBox get_extents(const Lines &lines)
{
    BoundingBox bbox;
    for (const Line &line : lines) {
        bbox.merge(line.a);
        bbox.merge(line.b);
    }
    return bbox;
}

} // namespace Slic3r
