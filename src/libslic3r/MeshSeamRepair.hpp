#ifndef slic3r_MeshSeamRepair_hpp_
#define slic3r_MeshSeamRepair_hpp_

#include "TriangleMesh.hpp"

namespace Slic3r {

// Texture seams may duplicate positions while their triangles already form a
// closed surface. Join exact duplicates before treating those seams as holes.
// Face order and coordinates stay unchanged, so face painting remains valid.
// Genuine open boundaries are left to the general repair path.
inline bool stitch_exact_mesh_seams(TriangleMesh& mesh)
{
    indexed_triangle_set stitched = mesh.its;
    if (its_merge_vertices(stitched) == 0 || its_num_open_edges(stitched) != 0)
        return false;

    TriangleMesh repaired(std::move(stitched));
    repaired.set_init_shift(mesh.get_init_shift());
    mesh = std::move(repaired);
    return true;
}

} // namespace Slic3r

#endif
