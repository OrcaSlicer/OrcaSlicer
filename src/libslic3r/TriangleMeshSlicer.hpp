#ifndef slic3r_TriangleMeshSlicer_hpp_
#define slic3r_TriangleMeshSlicer_hpp_

#include <functional>
#include <vector>
#include "Polygon.hpp"
#include "ExPolygon.hpp"

namespace Slic3r {

struct MeshSlicingParams
{
    enum class SlicingMode : uint32_t {
        // Regular slicing, maintain all contours and their orientation.
        // slice_mesh_ex() applies ClipperLib::pftNonZero rule to the result of slice_mesh().
        Regular,
        // For slicing 3DLabPrints plane models (aka to be compatible with S3D default strategy).
        // slice_mesh_ex() applies ClipperLib::pftEvenOdd rule. slice_mesh() slices EvenOdd as Regular.
        EvenOdd,
        // Maintain all contours, orient all contours CCW.
        // slice_mesh_ex() applies ClipperLib::pftNonZero rule, thus holes will be closed.
        Positive,
        // Orient all contours CCW and keep only the contour with the largest area.
        // This mode is useful for slicing complex objects in vase mode.
        PositiveLargestContour,
    };

    SlicingMode   mode { SlicingMode::Regular };
    // For vase mode: below this layer a different slicing mode will be used to produce a single contour.
    // 0 = ignore.
    size_t        slicing_mode_normal_below_layer { 0 };
    // Mode to apply below slicing_mode_normal_below_layer. Ignored if slicing_mode_nromal_below_layer == 0.
    SlicingMode   mode_below { SlicingMode::Regular };
    // Transforming faces during the slicing.
    Transform3d   trafo { Transform3d::Identity() };
};

struct MeshSlicingParamsEx : public MeshSlicingParams
{
    // Morphological closing operation when creating output expolygons, unscaled.
    float         closing_radius { 0 };
    // Positive offset applied when creating output expolygons, unscaled.
    float         extra_offset { 0 };
    // Resolution for contour simplification, unscaled.
    // 0 = don't simplify.
    double        resolution { 0 };
};

// All the following slicing functions shall produce consistent results with the same mesh, same transformation matrix and slicing parameters.
// Namely, slice_mesh_slabs() shall produce consistent results with slice_mesh() and slice_mesh_ex() in the sense, that projections made by 
// slice_mesh_slabs() shall fall onto slicing planes produced by slice_mesh().
//
// If a slicing plane slices a horizontal face of a mesh exactly,
// an upward facing horizontal face is is considered on slicing plane,
// while a downward facing horizontal face is considered not on slicing plane.
// 
// slice_mesh_slabs() thus projects an upward facing horizontal slice to the slicing plane,
// while slice_mesh_slabs() projects a downward facing horizontal slice to the slicing plane above if it exists.

std::vector<Polygons>           slice_mesh(
    const indexed_triangle_set       &mesh,
    const std::vector<float>         &zs,
    const MeshSlicingParams          &params,
    std::function<void()>             throw_on_cancel = []{});

// Specialized version for a single slicing plane only, running on a single thread.
Polygons                        slice_mesh(
    const indexed_triangle_set       &mesh,
    const float                       plane_z,
    const MeshSlicingParams          &params);

std::vector<ExPolygons>         slice_mesh_ex(
    const indexed_triangle_set       &mesh,
    const std::vector<float>         &zs,
    const MeshSlicingParamsEx        &params,
    std::function<void()>             throw_on_cancel = []{});

inline std::vector<ExPolygons>  slice_mesh_ex(
    const indexed_triangle_set       &mesh,
    const std::vector<float>         &zs,
    std::function<void()>             throw_on_cancel = []{})
{
    return slice_mesh_ex(mesh, zs, MeshSlicingParamsEx{}, throw_on_cancel);
}

inline std::vector<ExPolygons>  slice_mesh_ex(
    const indexed_triangle_set       &mesh,
    const std::vector<float>         &zs,
    float                             closing_radius,
    std::function<void()>             throw_on_cancel = []{})
{
    MeshSlicingParamsEx params;
    params.closing_radius = closing_radius;
    return slice_mesh_ex(mesh, zs, params, throw_on_cancel);
}

// Slice a triangle set with a set of Z slabs (thick layers).
// The effect is similar to producing the usual top / bottom layers from a sliced mesh by 
// subtracting layer[i] from layer[i - 1] for the top surfaces resp.
// subtracting layer[i] from layer[i + 1] for the bottom surfaces,
// with the exception that the triangle set this function processes may not cover the whole top resp. bottom surface.
// top resp. bottom surfaces are calculated only if out_top resp. out_bottom is not null.
void slice_mesh_slabs(
    const indexed_triangle_set       &mesh,
    // Unscaled Zs
    const std::vector<float>         &zs,
    const Transform3d                &trafo,
    std::vector<Polygons>            *out_top,
    std::vector<Polygons>            *out_bottom,
    std::vector<std::pair<Vec3f, Vec3f>>   *vertical_points,
    std::function<void()>             throw_on_cancel);

// Project mesh upwards pointing surfaces / downwards pointing surfaces into 2D polygons.
void project_mesh(
    const indexed_triangle_set       &mesh,
    const Transform3d                &trafo,
    Polygons                         *out_top,
    Polygons                         *out_bottom,
    std::function<void()>             throw_on_cancel);

// Project mesh into 2D polygons.
Polygons project_mesh(
    const indexed_triangle_set       &mesh,
    const Transform3d                &trafo,
    std::function<void()>             throw_on_cancel);

void cut_mesh(
    const indexed_triangle_set      &mesh,
    float                            z,
    indexed_triangle_set            *upper,
    indexed_triangle_set            *lower,
    bool                             triangulate_caps = true);

} // namespace Slic3r

#endif // slic3r_TriangleMeshSlicer_hpp_
