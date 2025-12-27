#include <libslic3r/SLA/ConcaveHull.hpp>
#include <libslic3r/SLA/SpatIndex.hpp>

#include <libslic3r/MTUtils.hpp>
#include <libslic3r/ClipperUtils.hpp>

#include <boost/log/trivial.hpp>

namespace Slic3r {
namespace sla {

inline Vec3d to_vec3(const Vec2crd &v2) { return {double(v2(X)), double(v2(Y)), 0.}; }
inline Vec3d to_vec3(const Vec2d &v2) { return {v2(X), v2(Y), 0.}; }
inline Vec2crd to_vec2(const Vec3d &v3) { return {coord_t(v3(X)), coord_t(v3(Y))}; }

Point ConcaveHull::centroid(const Points &pp)
{
    Point c;
    switch(pp.size()) {
    case 0: break;
    case 1: c = pp.front(); break;
    case 2: c = (pp[0] + pp[1]) / 2; break;
    default: {
        auto MAX = std::numeric_limits<Point::coord_type>::max();
        auto MIN = std::numeric_limits<Point::coord_type>::min();
        Point min = {MAX, MAX}, max = {MIN, MIN};

        for(auto& p : pp) {
            if(p(0) < min(0)) min(0) = p(0);
            if(p(1) < min(1)) min(1) = p(1);
            if(p(0) > max(0)) max(0) = p(0);
            if(p(1) > max(1)) max(1) = p(1);
        }
        c(0) = min(0) + (max(0) - min(0)) / 2;
        c(1) = min(1) + (max(1) - min(1)) / 2;
        break;
    }
    }

    return c;
}

Points ConcaveHull::calculate_centroids() const
{
    // We get the centroids of all the islands in the 2D slice
    Points centroids;
    centroids.reserve(m_polys.size());
    std::transform(m_polys.begin(), m_polys.end(),
                   std::back_inserter(centroids),
                   [](const Polygon &poly) { return centroid(poly); });

    return centroids;
}

void ConcaveHull::merge_polygons() { m_polys = get_contours(union_ex(m_polys)); }

void ConcaveHull::add_connector_rectangles(const Points &centroids,
                                           coord_t       max_dist,
                                           ThrowOnCancel thr)
{
    // Centroid of the centroids of islands. This is where the additional
    // connector sticks are routed.
    Point cc = centroid(centroids);

    PointIndex ctrindex;
    unsigned  idx = 0;
    for(const Point &ct : centroids) ctrindex.insert(to_vec3(ct), idx++);

    m_polys.reserve(m_polys.size() + centroids.size());

    idx = 0;
    for (const Point &c : centroids) {
        thr();

        double dx = c.x() - cc.x(), dy = c.y() - cc.y();
        double l  = std::sqrt(dx * dx + dy * dy);
        double nx = dx / l, ny = dy / l;

        const Point &ct = centroids[idx];

        std::vector<PointIndexEl> result = ctrindex.nearest(to_vec3(ct), 2);

        double dist = max_dist;
        for (const PointIndexEl &el : result)
            if (el.second != idx) {
                dist = Line(to_vec2(el.first), ct).length();
                break;
            }

        idx++;

        if (dist >= max_dist) return;

        Polygon r;
        r.points.reserve(3);
        r.points.emplace_back(cc);

        Point n(scaled(nx), scaled(ny));
        r.points.emplace_back(c + Point(n.y(), -n.x()));
        r.points.emplace_back(c + Point(-n.y(), n.x()));
        offset(r, scaled<float>(1.));

        m_polys.emplace_back(r);
    }
}

ConcaveHull::ConcaveHull(const Polygons &polys, double mergedist, ThrowOnCancel thr)
{
    if(polys.empty()) return;

    m_polys = polys;
    merge_polygons();

    if(m_polys.size() == 1) return;

    Points centroids = calculate_centroids();

    add_connector_rectangles(centroids, scaled(mergedist), thr);

    merge_polygons();
}

ExPolygons ConcaveHull::to_expolygons() const
{
    auto ret = reserve_vector<ExPolygon>(m_polys.size());
    for (const Polygon &p : m_polys) ret.emplace_back(ExPolygon(p));
    return ret;
}

ExPolygons offset_waffle_style_ex(const ConcaveHull &hull, coord_t delta)
{
    return to_expolygons(offset_waffle_style(hull, delta));
}

Polygons offset_waffle_style(const ConcaveHull &hull, coord_t delta)
{
    auto arc_tolerance = scaled<double>(0.01);
    Polygons res = closing(hull.polygons(), 2 * delta, delta, ClipperLib::jtRound, arc_tolerance);

    auto it = std::remove_if(res.begin(), res.end(), [](Polygon &p) { return p.is_clockwise(); });
    res.erase(it, res.end());

    return res;
}

}} // namespace Slic3r::sla
