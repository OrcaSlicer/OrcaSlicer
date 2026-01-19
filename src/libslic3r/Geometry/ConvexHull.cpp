#include "libslic3r.h"
#include "ConvexHull.hpp"
#include "BoundingBox.hpp"
#include "../Geometry.hpp"

#include <boost/multiprecision/integer.hpp>

namespace Slic3r { namespace Geometry {

// This implementation is based on Andrew's monotone chain 2D convex hull algorithm
Polygon convex_hull(Points pts)
{
    std::sort(pts.begin(), pts.end(), [](const Point& a, const Point& b) { return a.x() < b.x() || (a.x() == b.x() && a.y() < b.y()); });
    pts.erase(std::unique(pts.begin(), pts.end(), [](const Point& a, const Point& b) { return a.x() == b.x() && a.y() == b.y(); }), pts.end());

    Polygon hull;
    int n = (int)pts.size();
    if (n >= 3) {
        int k = 0;
        hull.points.resize(2 * n);
        // Build lower hull
        for (int i = 0; i < n; ++ i) {
            while (k >= 2 && Geometry::orient(pts[i], hull[k-2], hull[k-1]) != Geometry::ORIENTATION_CCW)
                -- k;
            hull[k ++] = pts[i];
        }
        // Build upper hull
        for (int i = n-2, t = k+1; i >= 0; i--) {
            while (k >= t && Geometry::orient(pts[i], hull[k-2], hull[k-1]) != Geometry::ORIENTATION_CCW)
                -- k;
            hull[k ++] = pts[i];
        }
        hull.points.resize(k);
        assert(hull.points.front() == hull.points.back());
        hull.points.pop_back();
    }
    return hull;
}

Pointf3s convex_hull(Pointf3s points)
{
    assert(points.size() >= 3);
    // sort input points
    std::sort(points.begin(), points.end(), [](const Vec3d &a, const Vec3d &b){ return a.x() < b.x() || (a.x() == b.x() && a.y() < b.y()); });

    int n = points.size(), k = 0;
    Pointf3s hull;

    if (n >= 3)
    {
        hull.resize(2 * n);

        // Build lower hull
        for (int i = 0; i < n; ++i)
        {
            Point p = Point::new_scale(points[i](0), points[i](1));
            while (k >= 2)
            {
                Point k1 = Point::new_scale(hull[k - 1](0), hull[k - 1](1));
                Point k2 = Point::new_scale(hull[k - 2](0), hull[k - 2](1));

                if (Geometry::orient(p, k2, k1) != Geometry::ORIENTATION_CCW)
                    --k;
                else
                    break;
            }

            hull[k++] = points[i];
        }

        // Build upper hull
        for (int i = n - 2, t = k + 1; i >= 0; --i)
        {
            Point p = Point::new_scale(points[i](0), points[i](1));
            while (k >= t)
            {
                Point k1 = Point::new_scale(hull[k - 1](0), hull[k - 1](1));
                Point k2 = Point::new_scale(hull[k - 2](0), hull[k - 2](1));

                if (Geometry::orient(p, k2, k1) != Geometry::ORIENTATION_CCW)
                    --k;
                else
                    break;
            }

            hull[k++] = points[i];
        }

        hull.resize(k);

        assert(hull.front() == hull.back());
        hull.pop_back();
    }

    return hull;
}

Polygon convex_hull(const Polygons &polygons)
{
    Points pp;
    for (Polygons::const_iterator p = polygons.begin(); p != polygons.end(); ++p) {
        pp.insert(pp.end(), p->points.begin(), p->points.end());
    }
    return convex_hull(std::move(pp));
}

Polygon convex_hull(const ExPolygons &expolygons)
{
    Points pp;
    size_t sz = 0;
    for (const auto &expoly : expolygons)
        sz += expoly.contour.size();
    pp.reserve(sz);
    for (const auto &expoly : expolygons)
        pp.insert(pp.end(), expoly.contour.points.begin(), expoly.contour.points.end());
    return convex_hull(pp);
}

Polygon convex_hulll(const Polylines &polylines)
{
    Points pp;
    size_t sz = 0;
    for (const auto &polyline : polylines)
        sz += polyline.points.size();
    pp.reserve(sz);
    for (const auto &polyline : polylines)
        pp.insert(pp.end(), polyline.points.begin(), polyline.points.end());
    return convex_hull(pp);
}

namespace rotcalip {

using int256_t = boost::multiprecision::int256_t;
using int128_t = boost::multiprecision::int128_t;

template<class Scalar = int64_t>
inline Scalar magnsq(const Point &p)
{
    return Scalar(p.x()) * p.x() + Scalar(p.y()) * p.y();
}

template<class Scalar = int64_t>
inline Scalar dot(const Point &a, const Point &b)
{
    return Scalar(a.x()) * b.x() + Scalar(a.y()) * b.y();
}

template<class Scalar = int64_t>
inline Scalar dotperp(const Point &a, const Point &b)
{
    return Scalar(a.x()) * b.y() - Scalar(a.y()) * b.x();
}

using boost::multiprecision::abs;

// Compares the angle enclosed by vectors dir and dirA (alpha) with the angle
// enclosed by -dir and dirB (beta). Returns -1 if alpha is less than beta, 0
// if they are equal and 1 if alpha is greater than beta. Note that dir is
// reversed for beta, because it represents the opposite side of a caliper.
int cmp_angles(const Point &dir, const Point &dirA, const Point &dirB) {
    int128_t dotA = dot(dir, dirA);
    int128_t dotB = dot(-dir, dirB);
    int256_t dcosa = int256_t(magnsq(dirB)) * int256_t(abs(dotA)) * dotA;
    int256_t dcosb = int256_t(magnsq(dirA)) * int256_t(abs(dotB)) * dotB;
    int256_t diff = dcosa - dcosb;

    return diff > 0? -1 : (diff < 0 ? 1 : 0);
}

// A helper class to navigate on a polygon. Given a vertex index, one can
// get the edge belonging to that vertex, the coordinates of the vertex, the
// next and previous edges. Stuff that is needed in the rotating calipers algo.
class Idx
{
    size_t m_idx;
    const Polygon *m_poly;
public:
    explicit Idx(const Polygon &p): m_idx{0}, m_poly{&p} {}
    explicit Idx(size_t idx, const Polygon &p): m_idx{idx}, m_poly{&p} {}

    size_t idx() const { return m_idx; }
    void set_idx(size_t i) { m_idx = i; }
    size_t next() const { return (m_idx + 1) % m_poly->size(); }
    size_t inc() { return m_idx = (m_idx + 1) % m_poly->size(); }
    Point prev_dir() const {
        return pt() - (*m_poly)[(m_idx + m_poly->size() - 1) % m_poly->size()];
    }

    const Point &pt() const { return (*m_poly)[m_idx]; }
    const Point dir() const { return (*m_poly)[next()] - pt(); }
    const Point  next_dir() const
    {
        return (*m_poly)[(m_idx + 2) % m_poly->size()] - (*m_poly)[next()];
    }
    const Polygon &poly() const { return *m_poly; }
};

enum class AntipodalVisitMode { Full, EdgesOnly };

// Visit all antipodal pairs starting from the initial ia, ib pair which
// has to be a valid antipodal pair (not checked). fn is called for every
// antipodal pair encountered including the initial one.
// The callback Fn has a signiture of bool(size_t i, size_t j, const Point &dir)
// where i,j are the vertex indices of the antipodal pair and dir is the
// direction of the calipers touching the i vertex.
template<AntipodalVisitMode mode = AntipodalVisitMode::Full, class Fn>
void visit_antipodals (Idx& ia, Idx &ib, Fn &&fn)
{
    // Set current caliper direction to be the lower edge angle from X axis
    int cmp = cmp_angles(ia.prev_dir(), ia.dir(), ib.dir());
    Idx *current = cmp <= 0 ? &ia : &ib, *other = cmp <= 0 ? &ib : &ia;
    Idx *initial = current;
    bool visitor_continue = true;

    size_t start = initial->idx();
    bool finished = false;

    while (visitor_continue && !finished) {
        Point current_dir_a = current == &ia ? current->dir() : -current->dir();
        visitor_continue = fn(ia.idx(), ib.idx(), current_dir_a);

        // Parallel edges encountered. An additional pair of antipodals
        // can be yielded.
        if constexpr (mode == AntipodalVisitMode::Full)
            if (cmp == 0 && visitor_continue) {
                visitor_continue = fn(current == &ia ? ia.idx() : ia.next(),
                                      current == &ib ? ib.idx() : ib.next(),
                                      current_dir_a);
            }

        cmp = cmp_angles(current->dir(), current->next_dir(), other->dir());

        current->inc();
        if (cmp > 0) {
            std::swap(current, other);
        }

        if (initial->idx() == start) finished = true;
    }
}

} // namespace rotcalip

bool convex_polygons_intersect(const Polygon &A, const Polygon &B)
{
    using namespace rotcalip;

    // Establish starting antipodals as extremes in XY plane. Use the
    // easily obtainable bounding boxes to check if A and B is disjoint
    // and return false if the are.
    struct BB
    {
        size_t         xmin = 0, xmax = 0, ymin = 0, ymax = 0;
        const Polygon &P;
        static bool cmpy(const Point &l, const Point &u)
        {
            return l.y() < u.y() || (l.y() == u.y() && l.x() < u.x());
        }

        BB(const Polygon &poly): P{poly}
        {
            for (size_t i = 0; i < P.size(); ++i) {
                if (P[i] < P[xmin]) xmin = i;
                if (P[xmax] < P[i]) xmax = i;
                if (cmpy(P[i], P[ymin])) ymin = i;
                if (cmpy(P[ymax], P[i])) ymax = i;
            }
        }
    };

    BB bA{A}, bB{B};
    BoundingBox bbA{{A[bA.xmin].x(), A[bA.ymin].y()}, {A[bA.xmax].x(), A[bA.ymax].y()}};
    BoundingBox bbB{{B[bB.xmin].x(), B[bB.ymin].y()}, {B[bB.xmax].x(), B[bB.ymax].y()}};

//    if (!bbA.overlap(bbB))
//        return false;

    // Establish starting antipodals as extreme vertex pairs in X or Y direction
    // which reside on different polygons. If no such pair is found, the two
    // polygons are certainly not disjoint.
    Idx imin{bA.xmin, A}, imax{bB.xmax, B};
    if (B[bB.xmin] < imin.pt())  imin = Idx{bB.xmin, B};
    if (imax.pt()  < A[bA.xmax]) imax = Idx{bA.xmax, A};
    if (&imin.poly() == &imax.poly()) {
        imin = Idx{bA.ymin, A};
        imax = Idx{bB.ymax, B};
        if (B[bB.ymin] < imin.pt())  imin = Idx{bB.ymin, B};
        if (imax.pt()  < A[bA.ymax]) imax = Idx{bA.ymax, A};
    }

    if (&imin.poly() == &imax.poly())
        return true;

    bool found_divisor = false;
    visit_antipodals<AntipodalVisitMode::EdgesOnly>(
        imin, imax,
        [&imin, &imax, &found_divisor](size_t ia, size_t ib, const Point &dir) {
            //        std::cout << "A" << ia << " B" << ib << " dir " <<
            //        dir.x() << " " << dir.y() << std::endl;
            const Polygon &A = imin.poly(), &B = imax.poly();

            Point ref_a = A[(ia + 2) % A.size()], ref_b = B[(ib + 2) % B.size()];

            bool is_left_a = dotperp( dir, ref_a - A[ia]) > 0;
            bool is_left_b = dotperp(-dir, ref_b - B[ib]) > 0;

            // If both reference points are on the left (or right) of their
            // respective support lines and the opposite support line is to
            // the right (or left), the divisor line is found. We only test
            // the reference point, as by definition, if that is on one side,
            // all the other points must be on the same side of a support
            // line. If the support lines are collinear, the polygons must be
            // on the same side of their respective support lines.

            auto d = dotperp(dir, B[ib] - A[ia]);
            if (d == 0) {
                // The caliper lines are collinear, not just parallel
                found_divisor = (is_left_a && is_left_b) || (!is_left_a && !is_left_b);
            } else if (d > 0) { // B is to the left of (A, A+1)
                found_divisor = !is_left_a && !is_left_b;
            } else { // B is to the right of (A, A+1)
                found_divisor = is_left_a && is_left_b;
            }

            return !found_divisor;
        });

    // Intersects if the divisor was not found
    return !found_divisor;
}

// Decompose source convex hull points into a top / bottom chains with monotonically increasing x,
// creating an implicit trapezoidal decomposition of the source convex polygon.
// The source convex polygon has to be CCW oriented. O(n) time complexity.
std::pair<std::vector<Vec2d>, std::vector<Vec2d>> decompose_convex_polygon_top_bottom(const std::vector<Vec2d> &src)
{
    std::pair<std::vector<Vec2d>, std::vector<Vec2d>> out;
    std::vector<Vec2d> &bottom = out.first;
    std::vector<Vec2d> &top    = out.second;

    // Find the minimum point.
    auto left_bottom  = std::min_element(src.begin(), src.end(), [](const auto &l, const auto &r) { return l.x() < r.x() || (l.x() == r.x() && l.y() < r.y()); });
    auto right_top    = std::max_element(src.begin(), src.end(), [](const auto &l, const auto &r) { return l.x() < r.x() || (l.x() == r.x() && l.y() < r.y()); });
    if (left_bottom != src.end() && left_bottom != right_top) {
        // Produce the bottom and bottom chains.
        if (left_bottom < right_top) {
            bottom.assign(left_bottom, right_top + 1);
            size_t cnt = (src.end() - right_top) + (left_bottom + 1 - src.begin());
            top.reserve(cnt);
            top.assign(right_top, src.end());
            top.insert(top.end(), src.begin(), left_bottom + 1);
        } else {
            size_t cnt = (src.end() - left_bottom) + (right_top + 1 - src.begin());
            bottom.reserve(cnt);
            bottom.assign(left_bottom, src.end());
            bottom.insert(bottom.end(), src.begin(), right_top + 1);
            top.assign(right_top, left_bottom + 1);
        }
        // Remove strictly vertical segments at the end.
        if (bottom.size() > 1) {
            auto it = bottom.end();
            for (-- it; it != bottom.begin() && (it - 1)->x() == bottom.back().x(); -- it) ;
            bottom.erase(it + 1, bottom.end());
        }
        if (top.size() > 1) {
            auto it = top.end();
            for (-- it; it != top.begin() && (it - 1)->x() == top.back().x(); -- it) ;
            top.erase(it + 1, top.end());
        }
        std::reverse(top.begin(), top.end());
    }

    if (top.size() < 2 || bottom.size() < 2) {
        // invalid
        top.clear();
        bottom.clear();
    }
    return out;
}

// Convex polygon check using a top / bottom chain decomposition with O(log n) time complexity.
bool inside_convex_polygon(const std::pair<std::vector<Vec2d>, std::vector<Vec2d>> &top_bottom_decomposition, const Vec2d &pt)
{
    auto it_bottom = std::lower_bound(top_bottom_decomposition.first.begin(),  top_bottom_decomposition.first.end(),  pt, [](const auto &l, const auto &r){ return l.x() < r.x(); });
    auto it_top    = std::lower_bound(top_bottom_decomposition.second.begin(), top_bottom_decomposition.second.end(), pt, [](const auto &l, const auto &r){ return l.x() < r.x(); });
    if (it_bottom == top_bottom_decomposition.first.end()) {
        // Above max x.
        assert(it_top == top_bottom_decomposition.second.end());
        return false;
    }
    if (it_bottom == top_bottom_decomposition.first.begin()) {
        // Below or at min x.
        if (pt.x() < it_bottom->x()) {
            // Below min x.
            assert(pt.x() < it_top->x());
            return false;
        }
        // At min x.
        assert(pt.x() == it_bottom->x());
        assert(pt.x() == it_top->x());
        assert(it_bottom->y() <= pt.y() && pt.y() <= it_top->y());
        return pt.y() >= it_bottom->y() && pt.y() <= it_top->y();
    }

    // Trapezoid or a triangle.
    assert(it_bottom != top_bottom_decomposition.first .begin() && it_bottom != top_bottom_decomposition.first .end());
    assert(it_top    != top_bottom_decomposition.second.begin() && it_top    != top_bottom_decomposition.second.end());
    assert(pt.x() <= it_bottom->x());
    assert(pt.x() <= it_top->x());
    auto it_top_prev    = it_top - 1;
    auto it_bottom_prev = it_bottom - 1;
    assert(pt.x() >= it_top_prev->x());
    assert(pt.x() >= it_bottom_prev->x());
    double det = cross2(*it_bottom - *it_bottom_prev, pt - *it_bottom_prev);
    if (det < 0)
        return false;
    det = cross2(*it_top - *it_top_prev, pt - *it_top_prev);
    return det <= 0;
}

} // namespace Geometry
} // namespace Slic3r

