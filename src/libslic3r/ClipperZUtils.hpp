#ifndef slic3r_ClipperZUtils_hpp_
#define slic3r_ClipperZUtils_hpp_

#include <functional>
#include <numeric>
#include <vector>

#include <libslic3r/ClipperUtils.hpp>
#include <libslic3r/Point.hpp>
#include <libslic3r/ExPolygon.hpp>

namespace Slic3r {

namespace ClipperZUtils {

using ZPoint  = Vec3crd;
using ZPoints = std::vector<ZPoint, PointsAllocator<ZPoint>>;
using ZPath   = ZPoints;
using ZPaths  = std::vector<ZPath, PointsAllocator<ZPath>>;

// Sets Z of an intersection point from the edges crossing there.
using ZFillCallback = std::function<void(const ZPoint &e1bot, const ZPoint &e1top, const ZPoint &e2bot, const ZPoint &e2top, ZPoint &pt)>;

inline bool zpoint_lower(const ZPoint &l, const ZPoint &r)
{
    return l.x() < r.x() || (l.x() == r.x() && (l.y() < r.y() || (l.y() == r.y() && l.z() < r.z())));
}

// Convert a single path to path with a given Z coordinate.
// If Open, then duplicate the first point at the end.
template<bool Open = false>
inline ZPath to_zpath(const Points &path, coord_t z)
{
    ZPath out;
    if (! path.empty()) {
        out.reserve((path.size() + Open) ? 1 : 0);
        for (const Point &p : path)
            out.emplace_back(p.x(), p.y(), z);
        if (Open)
            out.emplace_back(out.front());
    }
    return out;
}

// Convert multiple paths to paths with a given Z coordinate.
// If Open, then duplicate the first point of each path at its end.
template<bool Open = false>
inline ZPaths to_zpaths(const VecOfPoints &paths, coord_t z)
{
    ZPaths out;
    out.reserve(paths.size());
    for (const Points &path : paths)
        out.emplace_back(to_zpath<Open>(path, z));
    return out;
}

// Convert multiple expolygons into z-paths with Z specified by an index of the source expolygon
// offsetted by base_index.
// If Open, then duplicate the first point of each path at its end.
template<bool Open = false>
inline ZPaths expolygons_to_zpaths(const ExPolygons &src, coord_t &base_idx)
{
    ZPaths out;
    out.reserve(std::accumulate(src.begin(), src.end(), size_t(0),
        [](const size_t acc, const ExPolygon &expoly) { return acc + expoly.num_contours(); }));
    for (const ExPolygon &expoly : src) {
        out.emplace_back(to_zpath<Open>(expoly.contour.points, base_idx));
        for (const Polygon &hole : expoly.holes)
            out.emplace_back(to_zpath<Open>(hole.points, base_idx));
        ++ base_idx;
    }
    return out;
}

// Convert a single path to path with a given Z coordinate.
// If Open, then duplicate the first point at the end.
template<bool Open = false>
inline Points from_zpath(const ZPoints &path)
{
    Points out;
    if (! path.empty()) {
        out.reserve((path.size() + Open) ? 1 : 0);
        for (const ZPoint &p : path)
            out.emplace_back(p.x(), p.y());
        if (Open)
            out.emplace_back(out.front());
    }
    return out;
}

// Convert multiple paths to paths with a given Z coordinate.
// If Open, then duplicate the first point of each path at its end.
template<bool Open = false>
inline void from_zpaths(const ZPaths &paths, VecOfPoints &out)
{
    out.reserve(out.size() + paths.size());
    for (const ZPoints &path : paths)
        out.emplace_back(from_zpath<Open>(path));
}
template<bool Open = false>
inline VecOfPoints from_zpaths(const ZPaths &paths)
{
    VecOfPoints out;
    from_zpaths(paths, out);
    return out;
}

// Signed area, positive for a CCW path.
double area(const ZPath &path);

// Non-zero rule boolean, the subject may be open. zfill only sees intersections off the input vertices.
ZPaths clip_zpaths(ClipType clip_type, const ZPaths &subject, bool subject_open, const ZPaths &clip, const ZFillCallback &zfill, bool preserve_collinear = false);

class ClipperZIntersectionVisitor {
public:
    using Intersection  = std::pair<coord_t, coord_t>;
    using Intersections = std::vector<Intersection>;
    ClipperZIntersectionVisitor(Intersections &intersections) : m_intersections(intersections) {}
    void reset() { m_intersections.clear(); }
    void operator()(const ZPoint &e1bot, const ZPoint &e1top, const ZPoint &e2bot, const ZPoint &e2top, ZPoint &pt) {
        coord_t srcs[4]{ e1bot.z(), e1top.z(), e2bot.z(), e2top.z() };
        coord_t *begin = srcs;
        coord_t *end = srcs + 4;
        //FIXME bubble sort manually?
        std::sort(begin, end);
        end = std::unique(begin, end);
        if (begin + 1 == end) {
            // Self intersection may happen on source contour. Just copy the Z value.
            pt.z() = *begin;
        } else {
            assert(begin + 2 == end);
            if (begin + 2 <= end) {
                // store a -1 based negative index into the "intersections" vector here.
                m_intersections.emplace_back(srcs[0], srcs[1]);
                pt.z() = -coord_t(m_intersections.size());
            }
        }
    }
    ZFillCallback clipper_callback() {
        return [this](const ZPoint &e1bot, const ZPoint &e1top,
                 const ZPoint &e2bot, const ZPoint &e2top, ZPoint &pt)
        { return (*this)(e1bot, e1top, e2bot, e2top, pt); };
    }

    const std::vector<std::pair<coord_t, coord_t>>& intersections() const { return m_intersections; }

private:
    std::vector<std::pair<coord_t, coord_t>> &m_intersections;
};

} // namespace ClipperZUtils
} // namespace Slic3r

#endif // slic3r_ClipperZUtils_hpp_
