#ifndef ROTCALIPERS_HPP
#define ROTCALIPERS_HPP

#include <numeric>
#include <functional>
#include <array>
#include <cmath>

#include <libnest2d/geometry_traits.hpp>

namespace libnest2d {

template<class Pt, class Unit = TCompute<Pt>> class RotatedBox {
    Pt axis_;
    Unit bottom_ = Unit(0), right_ = Unit(0);
public:
    
    RotatedBox() = default;
    RotatedBox(const Pt& axis, Unit b, Unit r):
        axis_(axis), bottom_(b), right_(r) {}
    
    inline long double area() const { 
        long double asq = pl::magnsq<Pt, long double>(axis_);
        return cast<long double>(bottom_) * cast<long double>(right_) / asq;
    }
    
    inline long double width() const { 
        return abs(bottom_) / std::sqrt(pl::magnsq<Pt, long double>(axis_));
    }
    
    inline long double height() const { 
        return abs(right_) / std::sqrt(pl::magnsq<Pt, long double>(axis_));
    }
    
    inline Unit bottom_extent() const { return bottom_; }
    inline Unit right_extent() const { return right_;  }
    inline const Pt& axis() const { return axis_; }
    
    inline Radians angleToX() const {
        double ret = std::atan2(getY(axis_), getX(axis_));
        auto s = std::signbit(ret);
        if(s) ret += Pi_2;
        return -ret;
    }
};

template <class Poly, class Pt = TPoint<Poly>, class Unit = TCompute<Pt>> 
Poly removeCollinearPoints(const Poly& sh, Unit eps = Unit(0))
{
    Poly ret; sl::reserve(ret, sl::contourVertexCount(sh));
    
    Pt eprev = *sl::cbegin(sh) - *std::prev(sl::cend(sh));
    
    auto it  = sl::cbegin(sh);
    auto itx = std::next(it);
    if(itx != sl::cend(sh)) while (it != sl::cend(sh))
    {
        Pt enext = *itx - *it;

        auto dp = pl::dotperp<Pt, Unit>(eprev, enext);
        if(abs(dp) > eps) sl::addVertex(ret, *it);
        
        eprev = enext;
        if (++itx == sl::cend(sh)) itx = sl::cbegin(sh);
        ++it;
    }
    
    return ret;
}

// The area of the bounding rectangle with the axis dir and support vertices
template<class Pt, class Unit = TCompute<Pt>, class R = TCompute<Pt>> 
inline R rectarea(const Pt& w, // the axis
                  const Pt& vb, const Pt& vr, 
                  const Pt& vt, const Pt& vl) 
{
    Unit a = pl::dot<Pt, Unit>(w, vr - vl); 
    Unit b = pl::dot<Pt, Unit>(-pl::perp(w), vt - vb);
    R m = R(a) / pl::magnsq<Pt, Unit>(w);
    m = m * b;
    return m;
};

template<class Pt, 
         class Unit = TCompute<Pt>,
         class R = TCompute<Pt>,
         class It = typename std::vector<Pt>::const_iterator>
inline R rectarea(const Pt& w, const std::array<It, 4>& rect)
{
    return rectarea<Pt, Unit, R>(w, *rect[0], *rect[1], *rect[2], *rect[3]);
}

template<class Pt, class Unit = TCompute<Pt>, class R = TCompute<Pt>>
inline R rectarea(const Pt& w, // the axis
                  const Unit& a,
                  const Unit& b)
{
    R m = R(a) / pl::magnsq<Pt, Unit>(w);
    m = m * b;
    return m;
};

template<class R, class Pt, class Unit>
inline R rectarea(const RotatedBox<Pt, Unit> &rb)
{
    return rectarea<Pt, Unit, R>(rb.axis(), rb.bottom_extent(), rb.right_extent());
};

// This function is only applicable to counter-clockwise oriented convex
// polygons where only two points can be collinear witch each other.
template <class RawShape,
          class Unit = TCompute<RawShape>,
          class Ratio = TCompute<RawShape>,
          class VisitFn>
void rotcalipers(const RawShape& sh, VisitFn &&visitfn)
{
    using Point = TPoint<RawShape>;
    using Iterator = typename TContour<RawShape>::const_iterator;
    using pointlike::dot; using pointlike::magnsq; using pointlike::perp;

    // Get the first and the last vertex iterator
    auto first = sl::cbegin(sh);
    auto last = std::prev(sl::cend(sh));

    // Check conditions and return undefined box if input is not sane.
    if(last == first) return;
    if(getX(*first) == getX(*last) && getY(*first) == getY(*last)) --last;
    if(last - first < 2) return;

    RawShape shcpy; // empty at this point
    {
        Point p = *first, q = *std::next(first), r = *last;

        // Determine orientation from first 3 vertex (should be consistent)
        Unit d = (Unit(getY(q)) - getY(p)) * (Unit(getX(r)) - getX(p)) -
                 (Unit(getX(q)) - getX(p)) * (Unit(getY(r)) - getY(p));

        if(d > 0) {
            // The polygon is clockwise. A flip is needed (for now)
            sl::reserve(shcpy, last - first);
            auto it = last; while(it != first) sl::addVertex(shcpy, *it--);
            sl::addVertex(shcpy, *first);
            first = sl::cbegin(shcpy); last = std::prev(sl::cend(shcpy));
        }
    }

    // Cyclic iterator increment
    auto inc = [&first, &last](Iterator& it) {
        if(it == last) it = first; else ++it;
    };

    // Cyclic previous iterator
    auto prev = [&first, &last](Iterator it) {
        return it == first ? last : std::prev(it);
    };

    // Cyclic next iterator
    auto next = [&first, &last](Iterator it) {
        return it == last ? first : std::next(it);
    };

    // Establish initial (axis aligned) rectangle support verices by determining
    // polygon extremes:

    auto it = first;
    Iterator minX = it, maxX = it, minY = it, maxY = it;

    do { // Linear walk through the vertices and save the extreme positions

        Point v = *it, d = v - *minX;
        if(getX(d) < 0 || (getX(d) == 0 && getY(d) < 0)) minX = it;

        d = v - *maxX;
        if(getX(d) > 0 || (getX(d) == 0 && getY(d) > 0)) maxX = it;

        d = v - *minY;
        if(getY(d) < 0 || (getY(d) == 0 && getX(d) > 0)) minY = it;

        d = v - *maxY;
        if(getY(d) > 0 || (getY(d) == 0 && getX(d) < 0)) maxY = it;

    } while(++it != std::next(last));

    // Update the vertices defining the bounding rectangle. The rectangle with
    // the smallest rotation is selected and the supporting vertices are
    // returned in the 'rect' argument.
    auto update = [&next, &inc]
        (const Point& w, std::array<Iterator, 4>& rect)
    {
        Iterator B = rect[0], Bn = next(B);
        Iterator R = rect[1], Rn = next(R);
        Iterator T = rect[2], Tn = next(T);
        Iterator L = rect[3], Ln = next(L);

        Point b = *Bn - *B, r = *Rn - *R, t = *Tn - *T, l = *Ln - *L;
        Point pw = perp(w);
        using Pt = Point;

        Unit dotwpb = dot<Pt, Unit>( w, b), dotwpr = dot<Pt, Unit>(-pw, r);
        Unit dotwpt = dot<Pt, Unit>(-w, t), dotwpl = dot<Pt, Unit>( pw, l);
        Unit dw     = magnsq<Pt, Unit>(w);

        std::array<Ratio, 4> angles;
        angles[0] = (Ratio(dotwpb) / magnsq<Pt, Unit>(b)) * dotwpb;
        angles[1] = (Ratio(dotwpr) / magnsq<Pt, Unit>(r)) * dotwpr;
        angles[2] = (Ratio(dotwpt) / magnsq<Pt, Unit>(t)) * dotwpt;
        angles[3] = (Ratio(dotwpl) / magnsq<Pt, Unit>(l)) * dotwpl;

        using AngleIndex = std::pair<Ratio, size_t>;
        std::vector<AngleIndex> A; A.reserve(4);

        for (size_t i = 3, j = 0; j < 4; i = j++) {
            if(rect[i] != rect[j] && angles[i] < dw) {
                auto iv = std::make_pair(angles[i], i);
                auto it = std::lower_bound(A.begin(), A.end(), iv,
                                           [](const AngleIndex& ai,
                                              const AngleIndex& aj)
                {
                    return ai.first > aj.first;
                });

                A.insert(it, iv);
            }
        }

        // The polygon is supposed to be a rectangle.
        if(A.empty()) return false;

        auto amin = A.front().first;
        auto imin = A.front().second;
        for(auto& a : A) if(a.first == amin) inc(rect[a.second]);

        std::rotate(rect.begin(), rect.begin() + imin, rect.end());

        return true;
    };

    Point w(1, 0);
    std::array<Iterator, 4> rect = {minY, maxX, maxY, minX};

    {
        Unit a = dot<Point, Unit>(w, *rect[1] - *rect[3]);
        Unit b = dot<Point, Unit>(-perp(w), *rect[2] - *rect[0]);
        if (!visitfn(RotatedBox<Point, Unit>{w, a, b}))
            return;
    }

    // An edge might be examined twice in which case the algorithm terminates.
    size_t c = 0, count = last - first + 1;
    std::vector<bool> edgemask(count, false);

    while(c++ < count)
    {
        // Update the support vertices, if cannot be updated, break the cycle.
        if(! update(w, rect)) break;

        size_t eidx = size_t(rect[0] - first);

        if(edgemask[eidx]) break;
        edgemask[eidx] = true;

        // get the unnormalized direction vector
        w = *rect[0] - *prev(rect[0]);

        Unit a = dot<Point, Unit>(w, *rect[1] - *rect[3]);
        Unit b = dot<Point, Unit>(-perp(w), *rect[2] - *rect[0]);
        if (!visitfn(RotatedBox<Point, Unit>{w, a, b}))
            break;
    }
}

// This function is only applicable to counter-clockwise oriented convex
// polygons where only two points can be collinear witch each other.
template <class S,
          class Unit = TCompute<S>,
          class Ratio = TCompute<S>>
RotatedBox<TPoint<S>, Unit> minAreaBoundingBox(const S& sh)
{
    RotatedBox<TPoint<S>, Unit> minbox;
    Ratio minarea = std::numeric_limits<Unit>::max();
    auto minfn = [&minarea, &minbox](const RotatedBox<TPoint<S>, Unit> &rbox){
        Ratio area = rectarea<Ratio>(rbox);
        if (area <= minarea)  {
            minarea = area;
            minbox = rbox;
        }

        return true; // continue search
    };

    rotcalipers<S, Unit, Ratio>(sh, minfn);

    return minbox;
}

template <class RawShape> Radians minAreaBoundingBoxRotation(const RawShape& sh)
{
    return minAreaBoundingBox(sh).angleToX();
}

// Function to find a rotation for a shape that makes it fit into a box.
//
// The method is based on finding a pair of rotations from the rotating calipers
// algorithm such that the aspect ratio is changing from being smaller than
// that of the target to being bigger or vice versa. So that the correct
// AR is somewhere between the obtained pair of angles. Then bisecting that
// interval is sufficient to find the correct angle.
//
// The argument eps is the absolute error limit for the searched angle interval.
template<class S, class Unit = TCompute<S>, class Ratio = TCompute<S>>
Radians fitIntoBoxRotation(const S &shape, const _Box<TPoint<S>> &box, Radians eps = 1e-4)
{
    constexpr auto get_aspect_r = [](const auto &b) -> double {
        return double(b.width()) / b.height();
    };

    auto aspect_r = get_aspect_r(box);

    RotatedBox<TPoint<S>, Unit> prev_rbox;
    Radians a_from = 0., a_to = 0.;
    auto visitfn = [&](const RotatedBox<TPoint<S>, Unit> &rbox) {
        bool lower_prev    = get_aspect_r(prev_rbox) < aspect_r;
        bool lower_current = get_aspect_r(rbox) < aspect_r;

        if (lower_prev != lower_current) {
            a_from = prev_rbox.angleToX();
            a_to   = rbox.angleToX();
            return false;
        }

        return true;
    };

    rotcalipers<S, Unit, Ratio>(shape, visitfn);

    auto rot_shape_bb = [&shape](Radians r) {
        auto s = shape;
        sl::rotate(s, r);
        return sl::boundingBox(s);
    };

    auto rot_aspect_r = [&rot_shape_bb, &get_aspect_r](Radians r) {
        return get_aspect_r(rot_shape_bb(r));
    };

    // Lets bisect the retrieved interval where the correct aspect ratio is.
    double ar_from = rot_aspect_r(a_from);
    auto would_fit = [&box](const _Box<TPoint<S>> &b) {
        return b.width() < box.width() && b.height() < box.height();
    };

    Radians middle = (a_from + a_to) / 2.;
    _Box<TPoint<S>> box_middle = rot_shape_bb(middle);
    while (!would_fit(box_middle) && std::abs(a_to - a_from) > eps)
    {
        double ar_middle = get_aspect_r(box_middle);
        if ((ar_from < aspect_r) != (ar_middle < aspect_r))
            a_to = middle;
        else
            a_from = middle;

        ar_from = rot_aspect_r(a_from);
        middle = (a_from + a_to) / 2.;
        box_middle = rot_shape_bb(middle);
    }

    return middle;
}

} // namespace libnest2d

#endif // ROTCALIPERS_HPP
