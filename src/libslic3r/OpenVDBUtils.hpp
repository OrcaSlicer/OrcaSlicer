#ifndef OPENVDBUTILS_HPP
#define OPENVDBUTILS_HPP

#include <libslic3r/TriangleMesh.hpp>

#ifdef _MSC_VER
// Suppress warning C4146 in include/gmp.h(2177,31): unary minus operator applied to unsigned type, result still unsigned 
#pragma warning(push)
#pragma warning(disable : 4146)
#endif // _MSC_VER
#include <openvdb/openvdb.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif // _MSC_VER

namespace Slic3r {

inline Vec3f to_vec3f(const openvdb::Vec3s &v) { return Vec3f{v.x(), v.y(), v.z()}; }
inline Vec3d to_vec3d(const openvdb::Vec3s &v) { return to_vec3f(v).cast<double>(); }
inline Vec3i32 to_vec3i(const openvdb::Vec3I &v) { return Vec3i32{int(v[0]), int(v[1]), int(v[2])}; }

// Here voxel_scale defines the scaling of voxels which affects the voxel count.
// 1.0 value means a voxel for every unit cube. 2 means the model is scaled to
// be 2x larger and the voxel count is increased by the increment in the scaled
// volume, thus 4 times. This kind a sampling accuracy selection is not
// achievable through the Transform parameter. (TODO: or is it?)
// The resulting grid will contain the voxel_scale in its metadata under the
// "voxel_scale" key to be used in grid_to_mesh function.
openvdb::FloatGrid::Ptr mesh_to_grid(const indexed_triangle_set &    mesh,
                                     const openvdb::math::Transform &tr = {},
                                     float voxel_scale                  = 1.f,
                                     float exteriorBandWidth = 3.0f,
                                     float interiorBandWidth = 3.0f,
                                     int   flags             = 0);

indexed_triangle_set grid_to_mesh(const openvdb::FloatGrid &grid,
                                  double                    isovalue   = 0.0,
                                  double                    adaptivity = 0.0,
                                  bool relaxDisorientedTriangles = true);

openvdb::FloatGrid::Ptr redistance_grid(const openvdb::FloatGrid &grid,
                                        double                    iso,
                                        double ext_range = 3.,
                                        double int_range = 3.);

} // namespace Slic3r

#endif // OPENVDBUTILS_HPP
