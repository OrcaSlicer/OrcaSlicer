#ifndef slic3r_NormalUtils_hpp_
#define slic3r_NormalUtils_hpp_

#include "Point.hpp"
#include "Model.hpp"

namespace Slic3r {

/// <summary>
/// Collection of static function
/// to create normals
/// </summary>
class NormalUtils
{
public:
    using Normal = Vec3f;
    using Normals = std::vector<Normal>;
    NormalUtils() = delete; // only static functions

    enum class VertexNormalType {
        AverageNeighbor,
        AngleWeighted,
        NelsonMaxWeighted
    };

    /// <summary>
    /// Create normal for triangle defined by indices from vertices
    /// </summary>
    /// <param name="indices">index into vertices</param>
    /// <param name="vertices">vector of vertices</param>
    /// <returns>normal to triangle(normalized to size 1)</returns>
    static Normal create_triangle_normal(
        const stl_triangle_vertex_indices &indices,
        const std::vector<stl_vertex> &    vertices);

    /// <summary>
    /// Create normals for each vertices
    /// </summary>
    /// <param name="its">indices and vertices</param>
    /// <returns>Vector of normals</returns>
    static Normals create_triangle_normals(const indexed_triangle_set &its);

    /// <summary>
    /// Create normals for each vertex by averaging neighbor triangles normal
    /// </summary>
    /// <param name="its">Triangle indices and vertices</param>
    /// <param name="type">Type of calculation normals</param>
    /// <returns>Normal for each vertex</returns>
    static Normals create_normals(
        const indexed_triangle_set &its,
        VertexNormalType type = VertexNormalType::NelsonMaxWeighted);
    static Normals create_normals_average_neighbor(const indexed_triangle_set &its);
    static Normals create_normals_angle_weighted(const indexed_triangle_set &its);
    static Normals create_normals_nelson_weighted(const indexed_triangle_set &its);

    /// <summary>
    /// Calculate angle of trinagle side.
    /// </summary>
    /// <param name="i">index to indices, define angle point</param>
    /// <param name="indice">address to vertices</param>
    /// <param name="vertices">vertices data</param>
    /// <returns>Angle [in radian]</returns>
    static float indice_angle(int                            i,
                              const Vec3i32 &                indice,
                              const std::vector<stl_vertex> &vertices);
};

} // namespace Slic3r
#endif // slic3r_NormalUtils_hpp_
