#ifndef slic3r_ExPolygonsIndex_hpp_
#define slic3r_ExPolygonsIndex_hpp_

#include "ExPolygon.hpp"
namespace Slic3r {

/// <summary>
/// Index into ExPolygons
/// Identify expolygon, its contour (or hole) and point
/// </summary>
struct ExPolygonsIndex
{
    // index of ExPolygons
    uint32_t expolygons_index;

    // index of Polygon
    // 0 .. contour
    // N .. hole[N-1]
    uint32_t polygon_index;

    // index of point in polygon
    uint32_t point_index;

    bool is_contour() const { return polygon_index == 0; }
    bool is_hole() const { return polygon_index != 0; }
    uint32_t hole_index() const { return polygon_index - 1; }
};

/// <summary>
/// Keep conversion from ExPolygonsIndex to Index and vice versa
/// ExPolygonsIndex .. contour(or hole) point from ExPolygons
/// Index           .. continous number
/// 
/// index is used to address lines and points as result from function 
/// Slic3r::to_lines, Slic3r::to_points
/// </summary>
class ExPolygonsIndices
{
    std::vector<std::vector<uint32_t>> m_offsets;
    // for check range of index
    uint32_t m_count; // count of points
public:
    ExPolygonsIndices(const ExPolygons &shapes);

    /// <summary>
    /// Convert to one index number
    /// </summary>
    /// <param name="id">Compose of adress into expolygons</param>
    /// <returns>Index</returns>
    uint32_t cvt(const ExPolygonsIndex &id) const;

    /// <summary>
    /// Separate to multi index
    /// </summary>
    /// <param name="index">adress into expolygons</param>
    /// <returns></returns>
    ExPolygonsIndex cvt(uint32_t index) const;

    /// <summary>
    /// Check whether id is last point in polygon
    /// </summary>
    /// <param name="id">Identify point in expolygon</param>
    /// <returns>True when id is last point in polygon otherwise false</returns>
    bool is_last_point(const ExPolygonsIndex &id) const;

    /// <summary>
    /// Count of points in expolygons
    /// </summary>
    /// <returns>Count of points in expolygons</returns>
    uint32_t get_count() const;
};

} // namespace Slic3r
#endif // slic3r_ExPolygonsIndex_hpp_
