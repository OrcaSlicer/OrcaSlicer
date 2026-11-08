#ifndef slic3r_Fill_MeshGenerator_hpp_
#define slic3r_Fill_MeshGenerator_hpp_

#include <functional>
#include <vector>

#include "../BoundingBox.hpp"
#include "../ExPolygon.hpp"
#include "../Point.hpp"
#include "../PrintConfig.hpp"
#include "../TriangleMesh.hpp"

namespace Slic3r {

// Build a closed-manifold mesh of the band { p : |f(p)| <= thickness } in
// the coordinate frame of `aabb` and the inputs to `f`. The output is the
// zero level set of phi(p) = |f(p)| - thickness — two surfaces (f = +thickness
// and f = -thickness) wrapping the original implicit surface f = 0 in a thin
// shell. Watertight by construction (OpenVDB volumeToMesh on a level-set
// grid).
//
// Populate strategy is the surface walker required by Phase 2: the grid is
// seeded on a coarse 3D lattice with spacing `seed_stride`, then BFS expands
// through 6-connected neighbours of voxels whose phi falls within OpenVDB's
// narrow band. Cost scales with surface area, not bounding volume, which is
// what makes the 0.1 mm / 200 mm-cube case fit the Phase 2 memory budget.
//
// Parameters:
//   f            implicit scalar field evaluated at points in the same frame
//                as `aabb`. The mesh boundary is { p : |f(p)| = thickness }.
//                Must approximate signed distance in world units near its zero
//                set (|∇f| ≈ 1) — `thickness`, `voxel_size`, and the narrow
//                band are all in world units and are compared against f
//                directly. Dimensionless periodic fields (gyroid & friends)
//                must be rescaled by their gradient, e.g. multiplied by
//                period/2π, before being passed here.
//   aabb         object-space bounds; the caller must already have composed
//                `trafo_centered()` (and any pattern-specific `align_to_grid`)
//                into this box.
//   thickness    band half-width in world units. Must be > 0.
//   voxel_size   grid spacing in world units. Must be > 0. Smaller → finer
//                mesh, more memory; the Phase 2 acceptance target is 0.1 mm.
//   seed_stride  world-space spacing between seed points. Must be smaller
//                than the smallest feature scale of `f` the caller cares
//                about — for periodic implicits, smaller than the period.
//                A reasonable default for gyroid-like patterns is
//                `period / 4`.
//   adaptivity   [0, 1] mesh decimation passed to OpenVDB volumeToMesh.
//                0 = uniform marching output (most triangles), 1 = maximum
//                collapse of low-curvature areas. Facet count at fine voxel
//                sizes is the dominant memory cost of the whole pipeline, so
//                nonzero values matter for large objects.
//
// Returns an empty TriangleMesh on degenerate input (undefined or sub-voxel
// AABB, non-positive thickness/voxel_size/seed_stride, or no surface within
// the AABB).
TriangleMesh make_implicit_shell(
    const std::function<float(const Vec3f&)>& f,
    const BoundingBoxf3& aabb,
    double thickness,
    double voxel_size,
    double seed_stride,
    double adaptivity = 0.);

// Zero-thickness variant: meshes the signed field's f = 0 level set — the
// closed boundary between the field's two labyrinths (capped where it meets
// the AABB). Slicing it yields the labyrinth cross-section as area polygons
// whose boundaries ARE the pattern's centerline curves (plus AABB-cap
// segments that the per-region clip removes) — no medial axis needed for
// single-wall output. Because a sign change is captured at any resolution,
// voxel_size only needs to resolve the pattern period, not a band width.
// `thickness` does not apply; `f` must still be sign-correct (any monotone
// rescaling of a signed distance is fine — magnitudes only matter within
// ±3 voxels of the surface).
TriangleMesh make_implicit_surface(
    const std::function<float(const Vec3f&)>& f,
    const BoundingBoxf3& aabb,
    double voxel_size,
    double seed_stride,
    double adaptivity = 0.);

// Opt-in switch for the experimental mesh-backed Gyroid / TPMS-D / TPMS-FK route
// (UNLAYERED_MESH_INFILL=1 in the environment): the fill factory, prepare and the
// GUI preview all read this one function so they can never disagree.
bool mesh_backed_tpms_enabled();

// Which representation the generators produce / consumers expect.
enum class ImplicitMeshMode : unsigned char {
    SolidBand, // |f| - t closed shell; sliced polygons are thin band outlines
    Surface,   // signed f = 0 boundary; sliced polygon boundaries are centerlines
    Lines,     // open-surface (sheet) user mesh; the slices are polylines printed as single lines
};

// The TPMS field for a pattern at fill-relevant parameters, derived exactly
// as the analytic Fillers derive theirs (see each pattern's
// _fill_surface_single). Shared by PrintObject::prepare_mesh_infill_data and
// the GUI infill preview so the two can never disagree on period/phase.
// `spacing` must be the region flow spacing (Flow::spacing()), NOT the
// configured line width. Returns period <= 0 for unsupported patterns.
struct MeshInfillField
{
    float  period { 0.f };  // mm
    double omega  { 1. };   // gyroid-only Z anisotropy
    float  inv_grad { 1.f };
    std::function<float(const Vec3f&)> field; // SDF-normalized (mm-ish)
};
MeshInfillField make_mesh_infill_field(
    InfillPattern pattern, double density_fraction, double spacing, int multiline, double layer_height);

// Monolithic preview mesh at the same defaults prepare uses (voxel period/20,
// surface adaptivity 0 / band 0.5, band half-width wall_count*spacing/2).
// Intended for the GUI "preview mesh infill" path; large objects pay the
// monolithic cost (no z-streaming since nothing is sliced).
TriangleMesh make_mesh_infill_preview_mesh(
    const MeshInfillField &field_spec,
    const BoundingBoxf3   &aabb,
    ImplicitMeshMode       mode,
    int                    wall_count,
    double                 spacing);

// Streaming variant: generate the shell in Z slabs of ~slab_height, slice each
// slab at the zs that fall inside it, and discard the slab mesh before moving
// on. Output is aligned 1:1 with `zs`. Peak memory is bounded by the surface
// area of one slab instead of the whole object — this is what makes 100 mm+
// objects fit in memory (the monolithic path peaked at 12.8 GB on a 100 mm
// cube; see research/mesh-infill/02-mvp-plan.md).
//
// Each slab's AABB is extended by (thickness + 3 * voxel_size) beyond its
// outermost z so the slab's closing end-caps lie strictly outside the sliced
// range — slicing exactly through a cap plane would produce solid cross
// sections instead of the band outline.
//
// closing_radius is forwarded to slice_mesh_ex (MeshSlicingParamsEx).
// If merged_out is non-null every slab mesh is also accumulated there
// (debug/export use — costs the memory the streaming exists to avoid).
// If total_facets is non-null it receives the summed facet count.
std::vector<ExPolygons> make_implicit_shell_slices(
    const std::function<float(const Vec3f&)>& f,
    const BoundingBoxf3& aabb,
    double thickness,
    double voxel_size,
    double seed_stride,
    double adaptivity,
    const std::vector<float>& zs,
    float closing_radius,
    const std::function<void()>& throw_on_cancel,
    double slab_height = 8.,
    TriangleMesh* merged_out = nullptr,
    size_t* total_facets = nullptr,
    ImplicitMeshMode mode = ImplicitMeshMode::SolidBand);

} // namespace Slic3r

#endif // slic3r_Fill_MeshGenerator_hpp_
