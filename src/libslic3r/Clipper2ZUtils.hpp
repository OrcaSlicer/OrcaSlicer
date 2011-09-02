#ifndef slic3r_Clipper2ZUtils_hpp_
#define slic3r_Clipper2ZUtils_hpp_
#include <numeric>
#include <vector>
#include <algorithm>

#include <clipper2/clipper2_z.hpp>
#include <libslic3r/Point.hpp>
namespace Slic3r { namespace Clipper2ZUtils {

using ZPoint64  = Clipper2Lib_Z::Point64;
using ZPoints64 = Clipper2Lib_Z::Path64;
using ZPath64  = Clipper2Lib_Z::Path64;
using ZPaths64 = Clipper2Lib_Z::Paths64;

inline bool zpoint64_lower(const ZPoint64 &l, const ZPoint64 &r) {
    return l.x < r.x || (l.x == r.x && (l.y < r.y || (l.y == r.y && l.z < r.z)));
}

// Convert a single path to zpath with a given Z coordinate.
// If Open, then duplicate the first point at the end.
template<bool Open = false>
inline ZPath64 to_zpath64(const Points &path, int64_t z)
{
    ZPath64 out;
    if (!path.empty()) {
        out.reserve(path.size() + (Open ? 1 : 0));
        for (const Point &p : path) out.emplace_back(p.x(), p.y(), z);
        if (Open) out.emplace_back(out.front());
    }
    return out;
}

template<bool Open = false>
inline ZPath64 to_zpath64(const Clipper2Lib_Z::Path64 &path, int64_t z)
{
    ZPath64 out;
    if (!path.empty()) {
        out.reserve(path.size() + (Open ? 1 : 0));
        for (const Clipper2Lib_Z::Point64 &p : path) out.emplace_back(p.x, p.y, z);
        if (Open) out.emplace_back(out.front());
    }
    return out;
}

// Convert multiple paths to zpaths with a given Z coordinate.
template<bool Open = false>
inline ZPaths64 to_zpaths64(const VecOfPoints &paths, int64_t z)
{
    ZPaths64 out;
    out.reserve(paths.size());
    for (const Points &path : paths) out.emplace_back(to_zpath64<Open>(path, z));
    return out;
}

template<bool Open = false>
inline ZPaths64 to_zpaths64(const Clipper2Lib_Z::Paths64 &paths, int64_t z)
{
    ZPaths64 out;
    out.reserve(paths.size());
    for (const Clipper2Lib_Z::Path64 &path : paths) out.emplace_back(to_zpath64<Open>(path, z));
    return out;
}

// Convert multiple expolygons into zpaths with Z specified by index
// offset by base_idx.
template<bool Open = false>
inline ZPaths64 expolygons_to_zpaths64(const ExPolygons &src, int64_t &base_idx)
{
    ZPaths64 out;
    out.reserve(std::accumulate(src.begin(), src.end(), size_t(0),
        [](const size_t acc, const ExPolygon &expoly) { return acc + expoly.num_contours(); }));
    for (const ExPolygon &expoly : src) {
        out.emplace_back(to_zpath64<Open>(expoly.contour.points, base_idx));
        for (const Polygon &hole : expoly.holes)
            out.emplace_back(to_zpath64<Open>(hole.points, base_idx));
        ++base_idx;
    }
    return out;
}

// Convert multiple expolygons into zpaths with the same Z.
template<bool Open = false>
inline ZPaths64 expolygons_to_zpaths64_with_same_z(const ExPolygons &src, int64_t z)
{
    ZPaths64 out;
    out.reserve(std::accumulate(src.begin(), src.end(), size_t(0),
        [](const size_t acc, const ExPolygon &expoly) { return acc + expoly.num_contours(); }));
    for (const ExPolygon &expoly : src) {
        out.emplace_back(to_zpath64<Open>(expoly.contour.points, z));
        for (const Polygon &hole : expoly.holes)
            out.emplace_back(to_zpath64<Open>(hole.points, z));
    }
    return out;
}

// Convert a zpath back to 2D Points.
// If Open, then duplicate the first point at the end.
template<bool Open = false>
inline Points from_zpath64(const ZPath64 &path)
{
    Points out;
    if (!path.empty()) {
        out.reserve(path.size() + (Open ? 1 : 0));
        for (const ZPoint64 &p : path) out.emplace_back(p.x, p.y);
        if (Open) out.emplace_back(out.front());
    }
    return out;
}

// Convert multiple zpaths back to 2D paths.
template<bool Open = false>
inline void from_zpaths64(const ZPaths64 &paths, VecOfPoints &out)
{
    out.reserve(out.size() + paths.size());
    for (const ZPath64 &path : paths) out.emplace_back(from_zpath64<Open>(path));
}
template<bool Open = false>
inline VecOfPoints from_zpaths64(const ZPaths64 &paths)
{
    VecOfPoints out;
    from_zpaths64<Open>(paths, out);
    return out;
}

// Intersection visitor for Clipper2 (zCallback_).
class Clipper2ZIntersectionVisitor
{
public:
    using Intersection  = std::pair<int64_t, int64_t>;
    using Intersections = std::vector<Intersection>;

    Clipper2ZIntersectionVisitor(Intersections &intersections) : m_intersections(intersections) {}

    void reset() { m_intersections.clear(); }

    void operator()(const ZPoint64 &e1bot, const ZPoint64 &e1top, const ZPoint64 &e2bot, const ZPoint64 &e2top, ZPoint64 &pt)
    {
        std::array<int64_t, 4> srcs{e1bot.z, e1top.z, e2bot.z, e2top.z};
        std::sort(srcs.begin(), srcs.end());
        auto it = std::unique(srcs.begin(), srcs.end());
        int new_size = std::distance(srcs.begin(), it);
        assert(new_size == 1 || new_size == 2);
        if (new_size == 1) {
            pt.z = srcs[0];
        }
        else if(new_size == 2){
            m_intersections.emplace_back(srcs[0], srcs[1]);
            pt.z = -int64_t(m_intersections.size());
        }
    }

    auto clipper_callback()
    {
        return [this](const ZPoint64 &e1bot, const ZPoint64 &e1top,
                      const ZPoint64 &e2bot, const ZPoint64 &e2top, ZPoint64 &pt) {
            return (*this)(e1bot, e1top, e2bot, e2top, pt); };
    }

    const Intersections &intersections() const { return m_intersections; }

private:
    Intersections &m_intersections;
};

}} // namespace Slic3r::Clipper2ZUtils
#endif // slic3r_Clipper2ZUtils_hpp_