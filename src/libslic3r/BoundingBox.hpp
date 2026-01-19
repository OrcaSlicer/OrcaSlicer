#ifndef slic3r_BoundingBox_hpp_
#define slic3r_BoundingBox_hpp_

#include "libslic3r.h"
#include "Exception.hpp"
#include "Point.hpp"
#include "Polygon.hpp"
#include <ostream>

namespace Slic3r {

template <typename PointType, typename APointsType = std::vector<PointType>>
class BoundingBoxBase
{
public:
    using PointsType = APointsType;
    PointType min;
    PointType max;
    bool defined;
    
    BoundingBoxBase() : min(PointType::Zero()), max(PointType::Zero()), defined(false) {}
    BoundingBoxBase(const PointType &pmin, const PointType &pmax) : 
        min(pmin), max(pmax), defined(pmin.x() < pmax.x() && pmin.y() < pmax.y()) {}
    BoundingBoxBase(const PointType &p1, const PointType &p2, const PointType &p3) :
        min(p1), max(p1), defined(false) { merge(p2); merge(p3); }

    template<class It, class = IteratorOnly<It>>
    BoundingBoxBase(It from, It to)
        { construct(*this, from, to); }

    BoundingBoxBase(const PointsType &points)
        : BoundingBoxBase(points.begin(), points.end())
    {}

    void reset() { this->defined = false; this->min = PointType::Zero(); this->max = PointType::Zero(); }
    void merge(const PointType &point);
    void merge(const PointsType &points);
    void merge(const BoundingBoxBase<PointType, PointsType> &bb);
    void scale(double factor);
    PointType size() const;
    double radius() const;
    double area() const { return double(this->max(0) - this->min(0)) * (this->max(1) - this->min(1));    } // BBS
    void translate(coordf_t x, coordf_t y) { assert(this->defined); PointType v(x, y); this->min += v; this->max += v; }
    void translate(const PointType &v) { this->min += v; this->max += v; }
    void offset(coordf_t delta);
    BoundingBoxBase<PointType, PointsType> inflated(coordf_t delta) const throw() { BoundingBoxBase<PointType, PointsType> out(*this); out.offset(delta); return out; }
    PointType center() const;
    bool contains(const PointType &point) const {
        return point.x() >= this->min.x() && point.x() <= this->max.x()
            && point.y() >= this->min.y() && point.y() <= this->max.y();
    }
    bool contains(const BoundingBoxBase<PointType, PointsType> &other) const {
        return contains(other.min) && contains(other.max);
    }
    bool overlap(const BoundingBoxBase<PointType, PointsType> &other) const {
        return ! (this->max.x() < other.min.x() || this->min.x() > other.max.x() ||
                  this->max.y() < other.min.y() || this->min.y() > other.max.y());
    }
    PointType operator[](size_t idx) const {
        switch (idx) {
        case 0:
            return min;
            break;
        case 1:
            return PointType(max(0), min(1));
            break;
        case 2:
            return max;
            break;
        case 3:
            return PointType(min(0), max(1));
            break;
        default:
            return PointType();
            break;
        }
        return PointType();
    }
    bool operator==(const BoundingBoxBase<PointType, PointsType> &rhs) { return this->min == rhs.min && this->max == rhs.max; }
    bool operator!=(const BoundingBoxBase<PointType, PointsType> &rhs) { return ! (*this == rhs); }
    friend std::ostream &operator<<(std::ostream &os, const BoundingBoxBase &bbox)
    {
        os << "[" << bbox.max(0) - bbox.min(0) << " x " << bbox.max(1) - bbox.min(1) << "] from (" << bbox.min(0) << ", " << bbox.min(1) << ")";
        return os;
    }
    
private:
    // to access construct()
    friend BoundingBox get_extents<false>(const Points &pts);
    friend BoundingBox get_extents<true>(const Points &pts);

    // if IncludeBoundary, then a bounding box is defined even for a single point.
    // otherwise a bounding box is only defined if it has a positive area.
    // The output bounding box is expected to be set to "undefined" initially.
    template<bool IncludeBoundary = false, class BoundingBoxType, class It, class = IteratorOnly<It>>
    static void construct(BoundingBoxType &out, It from, It to)
    {
        if (from != to) {
            auto it = from;
            out.min = it->template cast<typename PointType::Scalar>();
            out.max = out.min;
            for (++ it; it != to; ++ it) {
                auto vec = it->template cast<typename PointType::Scalar>();
                out.min = out.min.cwiseMin(vec);
                out.max = out.max.cwiseMax(vec);
            }
            out.defined = IncludeBoundary || (out.min.x() < out.max.x() && out.min.y() < out.max.y());
        }
    }
};

template <class PointType>
class BoundingBox3Base : public BoundingBoxBase<PointType, std::vector<PointType>>
{
public:
    using PointsType = std::vector<PointType>;

    BoundingBox3Base() : BoundingBoxBase<PointType>() {}
    BoundingBox3Base(const PointType &pmin, const PointType &pmax) : 
        BoundingBoxBase<PointType>(pmin, pmax) 
        { if (pmin.z() >= pmax.z()) BoundingBoxBase<PointType>::defined = false; }
    BoundingBox3Base(const PointType &p1, const PointType &p2, const PointType &p3) :
        BoundingBoxBase<PointType>(p1, p1) { merge(p2); merge(p3); }

    template<class It, class = IteratorOnly<It> > BoundingBox3Base(It from, It to)
    {
        if (from == to)
            throw Slic3r::InvalidArgument("Empty point set supplied to BoundingBox3Base constructor");

        auto it = from;
        this->min = it->template cast<typename PointType::Scalar>();
        this->max = this->min;
        for (++ it; it != to; ++ it) {
            auto vec = it->template cast<typename PointType::Scalar>();
            this->min = this->min.cwiseMin(vec);
            this->max = this->max.cwiseMax(vec);
        }
        this->defined = (this->min.x() < this->max.x()) && (this->min.y() < this->max.y()) && (this->min.z() < this->max.z());
    }

    BoundingBox3Base(const PointsType &points)
        : BoundingBox3Base(points.begin(), points.end())
    {}

    Polygon polygon(bool is_scaled = false) const;//BBS: 2D footprint polygon
    void merge(const PointType &point);
    void merge(const PointsType &points);
    void merge(const BoundingBox3Base<PointType> &bb);
    PointType size() const;
    double radius() const;
    void translate(coordf_t x, coordf_t y, coordf_t z) { assert(this->defined); PointType v(x, y, z); this->min += v; this->max += v; }
    void translate(const Vec3d &v) { this->min += v; this->max += v; }
    void offset(coordf_t delta);
    BoundingBox3Base<PointType> inflated(coordf_t delta) const throw() { BoundingBox3Base<PointType> out(*this); out.offset(delta); return out; }
    PointType center() const;
    coordf_t max_size() const;

    bool contains(const PointType &point) const {
        return BoundingBoxBase<PointType>::contains(point) && point.z() >= this->min.z() && point.z() <= this->max.z();
    }

    bool contains(const BoundingBox3Base<PointType>& other) const {
        return contains(other.min) && contains(other.max);
    }

    // Intersects without boundaries.
    bool intersects(const BoundingBox3Base<PointType>& other) const {
        return this->min.x() < other.max.x() && this->max.x() > other.min.x() && this->min.y() < other.max.y() && this->max.y() > other.min.y() && 
            this->min.z() < other.max.z() && this->max.z() > other.min.z();
    }
};

// Will prevent warnings caused by non existing definition of template in hpp
extern template void     BoundingBoxBase<Point, Points>::scale(double factor);
extern template void     BoundingBoxBase<Vec2d>::scale(double factor);
extern template void     BoundingBoxBase<Vec3d>::scale(double factor);
extern template void     BoundingBoxBase<Point, Points>::offset(coordf_t delta);
extern template void     BoundingBoxBase<Vec2d>::offset(coordf_t delta);
extern template void     BoundingBoxBase<Point, Points>::merge(const Point &point);
extern template void     BoundingBoxBase<Vec2f>::merge(const Vec2f &point);
extern template void     BoundingBoxBase<Vec2d>::merge(const Vec2d &point);
extern template void     BoundingBoxBase<Point, Points>::merge(const Points &points);
extern template void     BoundingBoxBase<Vec2d>::merge(const Pointfs &points);
extern template void     BoundingBoxBase<Point, Points>::merge(const BoundingBoxBase<Point, Points> &bb);
extern template void     BoundingBoxBase<Vec2f>::merge(const BoundingBoxBase<Vec2f> &bb);
extern template void     BoundingBoxBase<Vec2d>::merge(const BoundingBoxBase<Vec2d> &bb);
extern template Point    BoundingBoxBase<Point, Points>::size() const;
extern template Vec2f    BoundingBoxBase<Vec2f>::size() const;
extern template Vec2d    BoundingBoxBase<Vec2d>::size() const;
extern template double   BoundingBoxBase<Point, Points>::radius() const;
extern template double   BoundingBoxBase<Vec2d>::radius() const;
extern template Point    BoundingBoxBase<Point, Points>::center() const;
extern template Vec2f    BoundingBoxBase<Vec2f>::center() const;
extern template Vec2d    BoundingBoxBase<Vec2d>::center() const;
extern template void     BoundingBox3Base<Vec3f>::merge(const Vec3f &point);
extern template void     BoundingBox3Base<Vec3d>::merge(const Vec3d &point);
extern template void     BoundingBox3Base<Vec3d>::merge(const Pointf3s &points);
extern template void     BoundingBox3Base<Vec3d>::merge(const BoundingBox3Base<Vec3d> &bb);
extern template Vec3f    BoundingBox3Base<Vec3f>::size() const;
extern template Vec3d    BoundingBox3Base<Vec3d>::size() const;
extern template double   BoundingBox3Base<Vec3d>::radius() const;
extern template void     BoundingBox3Base<Vec3d>::offset(coordf_t delta);
extern template Vec3f    BoundingBox3Base<Vec3f>::center() const;
extern template Vec3d    BoundingBox3Base<Vec3d>::center() const;
extern template coordf_t BoundingBox3Base<Vec3f>::max_size() const;
extern template coordf_t BoundingBox3Base<Vec3d>::max_size() const;

class BoundingBox : public BoundingBoxBase<Point, Points>
{
public:
    void polygon(Polygon* polygon) const;
    Polygon polygon() const;
    BoundingBox rotated(double angle) const;
    BoundingBox rotated(double angle, const Point &center) const;
    void rotate(double angle) { (*this) = this->rotated(angle); }
    void rotate(double angle, const Point &center) { (*this) = this->rotated(angle, center); }
    // Align the min corner to a grid of cell_size x cell_size cells,
    // to encompass the original bounding box.
    void align_to_grid(const coord_t cell_size);
    
    BoundingBox() : BoundingBoxBase<Point, Points>() {}
    BoundingBox(const Point &pmin, const Point &pmax) : BoundingBoxBase<Point, Points>(pmin, pmax) {}
    BoundingBox(const Points &points) : BoundingBoxBase<Point, Points>(points) {}

    BoundingBox inflated(coordf_t delta) const noexcept { BoundingBox out(*this); out.offset(delta); return out; }

    BoundingBox scaled(double factor) const;

    friend BoundingBox get_extents_rotated(const Points &points, double angle);
};

using BoundingBoxes = std::vector<BoundingBox>;

class BoundingBox3  : public BoundingBox3Base<Vec3crd> 
{
public:
    BoundingBox3() : BoundingBox3Base<Vec3crd>() {}
    BoundingBox3(const Vec3crd &pmin, const Vec3crd &pmax) : BoundingBox3Base<Vec3crd>(pmin, pmax) {}
    BoundingBox3(const Points3& points) : BoundingBox3Base<Vec3crd>(points) {}
};

class BoundingBoxf : public BoundingBoxBase<Vec2d> 
{
public:
    BoundingBoxf() : BoundingBoxBase<Vec2d>() {}
    BoundingBoxf(const Vec2d &pmin, const Vec2d &pmax) : BoundingBoxBase<Vec2d>(pmin, pmax) {}
    BoundingBoxf(const std::vector<Vec2d> &points) : BoundingBoxBase<Vec2d>(points) {}
};

class BoundingBoxf3 : public BoundingBox3Base<Vec3d> 
{
public:
    using BoundingBox3Base::BoundingBox3Base;

    BoundingBoxf3 transformed(const Transform3d& matrix) const;
};

template<typename PointType, typename PointsType>
inline bool empty(const BoundingBoxBase<PointType, PointsType> &bb)
{
    return ! bb.defined || bb.min.x() >= bb.max.x() || bb.min.y() >= bb.max.y();
}

template<typename PointType>
inline bool empty(const BoundingBox3Base<PointType> &bb)
{
    return ! bb.defined || bb.min.x() >= bb.max.x() || bb.min.y() >= bb.max.y() || bb.min.z() >= bb.max.z();
}

inline BoundingBox scaled(const BoundingBoxf &bb) { return {scaled(bb.min), scaled(bb.max)}; }

template<class T = coord_t>
BoundingBoxBase<Vec<2, T>> scaled(const BoundingBoxf &bb) { return {scaled<T>(bb.min), scaled<T>(bb.max)}; }

template<class T = coord_t>
BoundingBox3Base<Vec<3, T>> scaled(const BoundingBoxf3 &bb) { return {scaled<T>(bb.min), scaled<T>(bb.max)}; }

template<class T = double>
BoundingBoxBase<Vec<2, T>> unscaled(const BoundingBox &bb) { return {unscaled<T>(bb.min), unscaled<T>(bb.max)}; }

template<class T = double>
BoundingBox3Base<Vec<3, T>> unscaled(const BoundingBox3 &bb) { return {unscaled<T>(bb.min), unscaled<T>(bb.max)}; }

template<class Tout, class Tin>
auto cast(const BoundingBoxBase<Tin> &b)
{
    return BoundingBoxBase<Vec<2, Tout>>{b.min.template cast<Tout>(),
                                         b.max.template cast<Tout>()};
}

template<class Tout, class Tin>
auto cast(const BoundingBox3Base<Tin> &b)
{
    return BoundingBox3Base<Vec<3, Tout>>{b.min.template cast<Tout>(),
                                          b.max.template cast<Tout>()};
}

} // namespace Slic3r

// Serialization through the Cereal library
namespace cereal {
	template<class Archive> void serialize(Archive& archive, Slic3r::BoundingBox   &bb) { archive(bb.min, bb.max, bb.defined); }
	template<class Archive> void serialize(Archive& archive, Slic3r::BoundingBox3  &bb) { archive(bb.min, bb.max, bb.defined); }
	template<class Archive> void serialize(Archive& archive, Slic3r::BoundingBoxf  &bb) { archive(bb.min, bb.max, bb.defined); }
	template<class Archive> void serialize(Archive& archive, Slic3r::BoundingBoxf3 &bb) { archive(bb.min, bb.max, bb.defined); }
}

#endif
