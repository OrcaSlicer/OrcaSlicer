#include "IntersectionPoints.hpp"
#include <libslic3r/AABBTreeLines.hpp>

//NOTE: using CGAL SweepLines is slower !!! (example in git history)

namespace {    
using namespace Slic3r;
IntersectionsLines compute_intersections(const Lines &lines)
{
    if (lines.size() < 3)
        return {};    

    auto tree = AABBTreeLines::build_aabb_tree_over_indexed_lines(lines);
    IntersectionsLines result;
    for (uint32_t li = 0; li < lines.size()-1; ++li) {
        const Line &l = lines[li];
        auto intersections = AABBTreeLines::get_intersections_with_line<false, Point, Line>(lines, tree, l);
        for (const auto &[p, node_index] : intersections) {
            if (node_index - 1 <= li)
                continue;            
            if (const Line &l_ = lines[node_index];
                l_.a == l.a ||
                l_.a == l.b ||
                l_.b == l.a ||
                l_.b == l.b )
                // it is duplicit point not intersection
                continue; 

            // NOTE: fix AABBTree to compute intersection with double preccission!!
            Vec2d intersection_point = p.cast<double>();

            result.push_back(IntersectionLines{li, static_cast<uint32_t>(node_index), intersection_point});
        }
    }
    return result;
}
} // namespace

namespace Slic3r {
IntersectionsLines get_intersections(const Lines &lines)           { return compute_intersections(lines); }
IntersectionsLines get_intersections(const Polygon &polygon)       { return compute_intersections(to_lines(polygon)); }
IntersectionsLines get_intersections(const Polygons &polygons)     { return compute_intersections(to_lines(polygons)); }
IntersectionsLines get_intersections(const ExPolygon &expolygon)   { return compute_intersections(to_lines(expolygon)); }
IntersectionsLines get_intersections(const ExPolygons &expolygons) { return compute_intersections(to_lines(expolygons)); }
}
