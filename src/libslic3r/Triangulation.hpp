#ifndef libslic3r_Triangulation_hpp_
#define libslic3r_Triangulation_hpp_

#include <vector>
#include <set>
#include <libslic3r/Point.hpp>
#include <libslic3r/Polygon.hpp>
#include <libslic3r/ExPolygon.hpp>

namespace Slic3r {

class Triangulation
{
public:
    Triangulation() = delete;

    // define oriented connection of 2 vertices(defined by its index)
    using HalfEdge  = std::pair<uint32_t, uint32_t>;
    using HalfEdges = std::vector<HalfEdge>;
    using Indices   = std::vector<Vec3i32>;

    /// <summary>
    /// Connect points by triangulation to create filled surface by triangles
    /// Input points have to be unique
    /// Inspiration for make unique points is Emboss::dilate_to_unique_points
    /// </summary>
    /// <param name="points">Points to connect</param>
    /// <param name="edges">Constraint for edges, pair is from point(first) to
    /// point(second), sorted lexicographically</param> 
    /// <returns>Triangles</returns>
    static Indices triangulate(const Points &points,
                               const HalfEdges &half_edges);
    static Indices triangulate(const Polygon &polygon);
    static Indices triangulate(const Polygons &polygons);
    static Indices triangulate(const ExPolygon &expolygon);
    static Indices triangulate(const ExPolygons &expolygons);

    // Map for convert original index to set without duplication
    //              from_index<to_index>
    using Changes = std::vector<uint32_t>;

    /// <summary>
    /// Create conversion map from original index into new 
    /// with respect of duplicit point
    /// </summary>
    /// <param name="points">input set of points</param>
    /// <param name="duplicits">duplicit points collected from points</param>
    /// <returns>Conversion map for point index</returns>
    static Changes create_changes(const Points &points, const Points &duplicits);

    /// <summary>
    /// Triangulation for expolygons, speed up when points are already collected
    /// NOTE: Not working properly for ExPolygons with multiple point on same coordinate
    /// You should check it by "collect_changes"
    /// </summary>
    /// <param name="expolygons">Input shape to triangulation - define edges</param>
    /// <param name="points">Points from expolygons</param>
    /// <returns>Triangle indices</returns>
    static Indices triangulate(const ExPolygons &expolygons, const Points& points);

    /// <summary>
    /// Triangulation for expolygons containing multiple points with same coordinate
    /// </summary>
    /// <param name="expolygons">Input shape to triangulation - define edge</param>
    /// <param name="points">Points from expolygons</param>
    /// <param name="changes">Changes swap for indicies into points</param>
    /// <returns>Triangle indices</returns>
    static Indices triangulate(const ExPolygons &expolygons, const Points& points, const Changes& changes);
};

} // namespace Slic3r
#endif // libslic3r_Triangulation_hpp_