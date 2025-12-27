#include "BoundingBox.hpp"
#include "Polygon.hpp"
#include <algorithm>
#include <assert.h>

#include <Eigen/Dense>

namespace Slic3r {

template BoundingBoxBase<Point, Points>::BoundingBoxBase(const Points &points);
template BoundingBoxBase<Vec2d>::BoundingBoxBase(const std::vector<Vec2d> &points);

template BoundingBox3Base<Vec3d>::BoundingBox3Base(const std::vector<Vec3d> &points);

void BoundingBox::polygon(Polygon* polygon) const
{
    polygon->points = { 
        this->min,
        { this->max.x(), this->min.y() },
        this->max,
        { this->min.x(), this->max.y() }
    };
}

Polygon BoundingBox::polygon() const
{
    Polygon p;
    this->polygon(&p);
    return p;
}

BoundingBox BoundingBox::rotated(double angle) const
{
    BoundingBox out;
    out.merge(this->min.rotated(angle));
    out.merge(this->max.rotated(angle));
    out.merge(Point(this->min.x(), this->max.y()).rotated(angle));
    out.merge(Point(this->max.x(), this->min.y()).rotated(angle));
    return out;
}

BoundingBox BoundingBox::rotated(double angle, const Point &center) const
{
    BoundingBox out;
    out.merge(this->min.rotated(angle, center));
    out.merge(this->max.rotated(angle, center));
    out.merge(Point(this->min.x(), this->max.y()).rotated(angle, center));
    out.merge(Point(this->max.x(), this->min.y()).rotated(angle, center));
    return out;
}

BoundingBox BoundingBox::scaled(double factor) const
{
    BoundingBox out(*this);
    out.scale(factor);
    return out;
}

template <class PointType, typename APointsType> void
BoundingBoxBase<PointType, APointsType>::scale(double factor)
{
    this->min *= factor;
    this->max *= factor;
}
template void BoundingBoxBase<Point, Points>::scale(double factor);
template void BoundingBoxBase<Vec2d>::scale(double factor);
template void BoundingBoxBase<Vec3d>::scale(double factor);

template <class PointType, typename APointsType> void
BoundingBoxBase<PointType, APointsType>::merge(const PointType &point)
{
    if (this->defined) {
        this->min = this->min.cwiseMin(point);
        this->max = this->max.cwiseMax(point);
    } else {
        this->min = point;
        this->max = point;
        this->defined = true;
    }
}
template void BoundingBoxBase<Point, Points>::merge(const Point &point);
template void BoundingBoxBase<Vec2f>::merge(const Vec2f &point);
template void BoundingBoxBase<Vec2d>::merge(const Vec2d &point);

template <class PointType, typename APointsType> void
BoundingBoxBase<PointType, APointsType>::merge(const PointsType &points)
{
    this->merge(BoundingBoxBase(points));
}
template void BoundingBoxBase<Point, Points>::merge(const Points &points);
template void BoundingBoxBase<Vec2d>::merge(const Pointfs &points);

template <class PointType, typename APointsType> void
BoundingBoxBase<PointType, APointsType>::merge(const BoundingBoxBase<PointType, PointsType> &bb)
{
    assert(bb.defined || bb.min.x() >= bb.max.x() || bb.min.y() >= bb.max.y());
    if (bb.defined) {
        if (this->defined) {
            this->min = this->min.cwiseMin(bb.min);
            this->max = this->max.cwiseMax(bb.max);
        } else {
            this->min = bb.min;
            this->max = bb.max;
            this->defined = true;
        }
    }
}
template void BoundingBoxBase<Point, Points>::merge(const BoundingBoxBase<Point, Points> &bb);
template void BoundingBoxBase<Vec2f>::merge(const BoundingBoxBase<Vec2f> &bb);
template void BoundingBoxBase<Vec2d>::merge(const BoundingBoxBase<Vec2d> &bb);

//BBS
template <class PointType>
Polygon BoundingBox3Base<PointType>::polygon(bool is_scaled) const
{
    Polygon polygon;
    polygon.points.clear();
    polygon.points.resize(4);
    double scale_factor = 1 / (is_scaled ? SCALING_FACTOR : 1);
    polygon.points[0](0) = this->min(0) * scale_factor;
    polygon.points[0](1) = this->min(1) * scale_factor;
    polygon.points[1](0) = this->max(0) * scale_factor;
    polygon.points[1](1) = this->min(1) * scale_factor;
    polygon.points[2](0) = this->max(0) * scale_factor;
    polygon.points[2](1) = this->max(1) * scale_factor;
    polygon.points[3](0) = this->min(0) * scale_factor;
    polygon.points[3](1) = this->max(1) * scale_factor;
    return polygon;
}
template Polygon BoundingBox3Base<Vec3f>::polygon(bool is_scaled) const;
template Polygon BoundingBox3Base<Vec3d>::polygon(bool is_scaled) const;

template <class PointType> void
BoundingBox3Base<PointType>::merge(const PointType &point)
{
    if (this->defined) {
        this->min = this->min.cwiseMin(point);
        this->max = this->max.cwiseMax(point);
    } else {
        this->min = point;
        this->max = point;
        this->defined = true;
    }
}
template void BoundingBox3Base<Vec3f>::merge(const Vec3f &point);
template void BoundingBox3Base<Vec3d>::merge(const Vec3d &point);

template <class PointType> void
BoundingBox3Base<PointType>::merge(const PointsType &points)
{
    this->merge(BoundingBox3Base(points));
}
template void BoundingBox3Base<Vec3d>::merge(const Pointf3s &points);

template <class PointType> void
BoundingBox3Base<PointType>::merge(const BoundingBox3Base<PointType> &bb)
{
    assert(bb.defined || bb.min.x() >= bb.max.x() || bb.min.y() >= bb.max.y() || bb.min.z() >= bb.max.z());
    if (bb.defined) {
        if (this->defined) {
            this->min = this->min.cwiseMin(bb.min);
            this->max = this->max.cwiseMax(bb.max);
        } else {
            this->min = bb.min;
            this->max = bb.max;
            this->defined = true;
        }
    }
}
template void BoundingBox3Base<Vec3d>::merge(const BoundingBox3Base<Vec3d> &bb);

template <class PointType, typename APointsType> PointType
BoundingBoxBase<PointType, APointsType>::size() const
{
    return this->max - this->min;
}
template Point BoundingBoxBase<Point, Points>::size() const;
template Vec2f BoundingBoxBase<Vec2f>::size() const;
template Vec2d BoundingBoxBase<Vec2d>::size() const;

template <class PointType> PointType
BoundingBox3Base<PointType>::size() const
{
    return this->max - this->min;
}
template Vec3f BoundingBox3Base<Vec3f>::size() const;
template Vec3d BoundingBox3Base<Vec3d>::size() const;

template <class PointType, typename APointsType> double BoundingBoxBase<PointType, APointsType>::radius() const
{
    assert(this->defined);
    return 0.5 * (this->max - this->min).template cast<double>().norm();
}
template double BoundingBoxBase<Point, Points>::radius() const;
template double BoundingBoxBase<Vec2d>::radius() const;

template <class PointType> double BoundingBox3Base<PointType>::radius() const
{
    return 0.5 * (this->max - this->min).template cast<double>().norm();
}
template double BoundingBox3Base<Vec3d>::radius() const;

template <class PointType, typename APointsType> void
BoundingBoxBase<PointType, APointsType>::offset(coordf_t delta)
{
    PointType v(delta, delta);
    this->min -= v;
    this->max += v;
}
template void BoundingBoxBase<Point, Points>::offset(coordf_t delta);
template void BoundingBoxBase<Vec2d>::offset(coordf_t delta);

template <class PointType> void
BoundingBox3Base<PointType>::offset(coordf_t delta)
{
    PointType v(delta, delta, delta);
    this->min -= v;
    this->max += v;
}
template void BoundingBox3Base<Vec3d>::offset(coordf_t delta);

template <class PointType, typename APointsType> PointType
BoundingBoxBase<PointType, APointsType>::center() const
{
    return (this->min + this->max) / 2;
}
template Point BoundingBoxBase<Point, Points>::center() const;
template Vec2f BoundingBoxBase<Vec2f>::center() const;
template Vec2d BoundingBoxBase<Vec2d>::center() const;

template <class PointType> PointType
BoundingBox3Base<PointType>::center() const
{
    return (this->min + this->max) / 2;
}
template Vec3f BoundingBox3Base<Vec3f>::center() const;
template Vec3d BoundingBox3Base<Vec3d>::center() const;

template <class PointType> coordf_t
BoundingBox3Base<PointType>::max_size() const
{
    PointType s = size();
    return std::max(s.x(), std::max(s.y(), s.z()));
}
template coordf_t BoundingBox3Base<Vec3f>::max_size() const;
template coordf_t BoundingBox3Base<Vec3d>::max_size() const;

void BoundingBox::align_to_grid(const coord_t cell_size)
{
    if (this->defined) {
        min.x() = Slic3r::align_to_grid(min.x(), cell_size);
        min.y() = Slic3r::align_to_grid(min.y(), cell_size);
    }
}

BoundingBoxf3 BoundingBoxf3::transformed(const Transform3d& matrix) const
{
    typedef Eigen::Matrix<double, 3, 8, Eigen::DontAlign> Vertices;

    Vertices src_vertices;
    src_vertices(0, 0) = min.x(); src_vertices(1, 0) = min.y(); src_vertices(2, 0) = min.z();
    src_vertices(0, 1) = max.x(); src_vertices(1, 1) = min.y(); src_vertices(2, 1) = min.z();
    src_vertices(0, 2) = max.x(); src_vertices(1, 2) = max.y(); src_vertices(2, 2) = min.z();
    src_vertices(0, 3) = min.x(); src_vertices(1, 3) = max.y(); src_vertices(2, 3) = min.z();
    src_vertices(0, 4) = min.x(); src_vertices(1, 4) = min.y(); src_vertices(2, 4) = max.z();
    src_vertices(0, 5) = max.x(); src_vertices(1, 5) = min.y(); src_vertices(2, 5) = max.z();
    src_vertices(0, 6) = max.x(); src_vertices(1, 6) = max.y(); src_vertices(2, 6) = max.z();
    src_vertices(0, 7) = min.x(); src_vertices(1, 7) = max.y(); src_vertices(2, 7) = max.z();

    Vertices dst_vertices = matrix * src_vertices.colwise().homogeneous();

    Vec3d v_min(dst_vertices(0, 0), dst_vertices(1, 0), dst_vertices(2, 0));
    Vec3d v_max = v_min;

    for (int i = 1; i < 8; ++i)
    {
        for (int j = 0; j < 3; ++j)
        {
            v_min(j) = std::min(v_min(j), dst_vertices(j, i));
            v_max(j) = std::max(v_max(j), dst_vertices(j, i));
        }
    }

    return BoundingBoxf3(v_min, v_max);
}

}
