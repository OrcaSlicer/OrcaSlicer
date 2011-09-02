#ifndef SRC_LIBSLIC3R_ALGORITHM_LINE_SPLIT_HPP_
#define SRC_LIBSLIC3R_ALGORITHM_LINE_SPLIT_HPP_

#include "ClipperZUtils.hpp"

namespace Slic3r {
namespace Algorithm {

struct SplitLineJunction
{
    Point p;

    // true if the line between this point and the next point is inside the clip polygon (or on the edge of the clip polygon)
    bool clipped;

    // Index from the original input.
    // - If this junction is presented in the source polygon/polyline, this is the index of the point with in the source;
    // - if this point in a new point that caused by the intersection, this will be -(1+index of the first point of the source line involved in this intersection);
    // - if this junction came from the clip polygon, it will be treated as new point.
    int64_t src_idx;

    SplitLineJunction(const Point& p, bool clipped, int64_t src_idx)
        : p(p)
        , clipped(clipped)
        , src_idx(src_idx) {}

    bool is_src() const { return src_idx >= 0; }
    size_t get_src_index() const
    {
        if (is_src()) {
            return src_idx;
        } else {
            return -src_idx - 1;
        }
    }
};

using SplittedLine = std::vector<SplitLineJunction>;

SplittedLine do_split_line(const ClipperZUtils::ZPath& path, const ExPolygons& clip, bool closed);

// Return the splitted line, or empty if no intersection found
template<class PathType>
SplittedLine split_line(const PathType& path, const ExPolygons& clip, bool closed)
{
    if (path.empty()) {
        return {};
    }

    // Convert the input path into an open ZPath
    ClipperZUtils::ZPath p;
    p.reserve(path.size() + closed ? 1 : 0);
    ClipperLib_Z::cInt z = 0;
    for (const auto& point : path) {
        p.emplace_back(point.x(), point.y(), z);
        z++;
    }
    if (closed) {
        // duplicate the first point at the end to make a closed path open
        p.emplace_back(p.front());
        p.back().z() = z;
    }

    return do_split_line(p, clip, closed);
}

} // Algorithm
} // Slic3r

#endif /* SRC_LIBSLIC3R_ALGORITHM_LINE_SPLIT_HPP_ */
