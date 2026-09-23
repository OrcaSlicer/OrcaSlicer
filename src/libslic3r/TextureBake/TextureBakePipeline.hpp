#pragma once

// The bake pipeline:
//
//   subdivide -> [regularize -> re-subdivide] -> [paint test] -> [relocate] -> [flip edges]
//             -> displace -> [decimate] -> bottom clamp -> bottom snap -> [resolve T-junctions]
//
// Regularization sits between two subdivisions on purpose: it dissolves the slivers refinement
// inherited, which lengthens some edges past the target, and the second pass brings those back.
// Before any subdivision it would have nothing to work on, since the slivers come from refining a
// needle; after a single pass it would leave the mesh coarser than asked for.
//
// Decimation and repair are export-only - decimation drops the output-to-input face mapping a bake
// needs to carry per-face data forward.

#include <cstdint>
#include <functional>
#include <vector>

#include "TextureBakeDecimate.hpp"
#include "TextureBakeDisplace.hpp"
#include "TextureBakeIndex.hpp"
#include "TextureBakeRegularize.hpp"
#include "TextureBakeFlip.hpp"
#include "TextureBakeRelocate.hpp"
#include "TextureBakeRepair.hpp"
#include "TextureBakeSubdivide.hpp"

namespace Slic3r {

// Optional step-by-step capture; see TextureBakeDebug.hpp. A pointer, and forward declared, so the
// pipeline header stays free of the mesh types the recorder converts into.
class BakeStageRecorder;

namespace TextureBake {

enum class PipelineMode
{
    // Keeps the face-parent mapping; skips decimation and repair.
    Bake,
    // The full sequence, including decimation and repair.
    Export,
};

struct PipelineSettings
{
    // Target edge length for the refinement, in mm.
    double refine_length = 1.0;

    // Sliver removal between the two subdivision passes.
    bool              regularize = true;
    RegularizeOptions regularize_opts;
    // Slightly above the first pass, so it recovers the edges regularization lengthened instead of
    // re-refining what it just merged.
    double regularize_second_pass_mul = 1.1;

    // Slide vertices onto the height map's own edges before displacing, so a step lands on a mesh
    // edge instead of being quantised to wherever the grid fell.
    bool             relocate = false;
    RelocateSettings relocate_opts;

    // Choose each quad's diagonal to follow the height field before displacing, so a step that crosses
    // the grid at an angle comes out as a straight wall instead of a sawtooth. See TextureBakeFlip.hpp.
    bool         flip_edges = true;
    FlipSettings flip_opts;

    DisplaceSettings displace;

    // Optional. Asked once per refined face (its centroid, in the soup's coordinates) after the
    // refinement stages and before displacement, for faces whose source triangle was included:
    // false marks the face as unpainted (no displacement), so paint finer than the input triangles
    // is honoured. Faces excluded from the start are never asked. Called from several threads at
    // once, so it must be safe to call concurrently.
    std::function<bool(const Vec3f &centroid)> painted;

    // Export mode only.
    // What this bake may spend on what it refines. Geometry it only preserves (see preserve_untextured)
    // is counted on top of it, so an earlier bake's relief does not have to be evicted to fit this one.
    size_t max_triangles = 750'000;
    // Keep removing zero-cost flat faces past the target. Only applies when decimation runs, i.e. when
    // the displaced mesh is over the budget - an under-budget mesh is never decimated.
    bool   harvest_flat  = true;
    double harvest_tol   = DECIMATE_DEFAULT_HARVEST_TOL;
    // Lock the untextured region against both regularization and decimation.
    bool preserve_untextured = true;

    // Push anything that displaced below the plate back up to it. Downward movement is otherwise left
    // alone, so relief on the underside is kept - only what would sink through the plate is stopped.
    bool clamp_below_plate = false;

    // Snap vertices within this of the bottom plane onto it. 0 disables.
    double bottom_snap_tol = 0.1;

    int  safety_cap = SUBDIVIDE_SAFETY_CAP;
};

// Stage name and a fraction within it. Returning false cancels the run.
using PipelineProgressFn = std::function<bool(const char *stage, double fraction)>;
// Colour class of a point of the surface (a palette index, -1 for none), for the decimation's
// colour-boundary creases. Only consulted when the mesh is over budget.
using ColorSampleFn = std::function<int(const Vec3f &centroid, const Vec3f &normal)>;

struct PipelineResult
{
    TriSoup geometry;
    // Output face -> input face. Empty in Export mode, where decimation invalidates it.
    std::vector<int> face_parent_id;
    bool             safety_cap_hit     = false;
    bool             locked_over_budget = false;
    size_t           collapse_count     = 0;
    bool             canceled           = false;
    // What the refinement produced, before decimation, and the count it had to fit into (the budget
    // plus the preserved geometry). budget_limited says the refined mesh did not fit: the result
    // carries less of the texture than the resolution asked for, which is what a caller warns about.
    size_t           triangles_refined  = 0;
    size_t           triangles_budget   = 0;
    bool             budget_limited     = false;
};

// `debug`, when given and enabled, receives the mesh after every stage that ran - which is the only
// way to tell which stage a bad result came from, since each one rewrites the whole mesh.
PipelineResult run_pipeline(const TriSoup &input, const HeightSampleFn &sample,
                            const PipelineSettings &settings, const DisplaceBounds &bounds,
                            PipelineMode mode, const std::vector<uint8_t> &face_excluded = {},
                            const PipelineProgressFn &on_progress = {},
                            BakeStageRecorder *debug = nullptr, const ColorSampleFn &color_sample = {});

// Snap anything that ended below the model's original bottom back up to it.
void clamp_below_bottom(TriSoup &geometry, float bottom_z);

// Flatten the bed-contact surface by snapping positions within `tol` of the bottom plane onto it.
//
// Gated, not unconditional: an unconditional band snap also flattens the undersides of texture relief
// near the base, folding them coplanar into the bottom face. Folded faces overlap the plate, so edges
// there pick up four incident faces - non-manifold edges and phantom shells on re-import. All copies
// of a position move together, and the move is rejected if any incident triangle would go degenerate
// or rotate more than about 75 degrees. A real bed-contact sliver rotates by a fraction of a degree
// and still snaps. Returns how many triangles moved.
size_t snap_bottom_to_flat(TriSoup &geometry, float bottom_z, double tol = 0.1);

} // namespace TextureBake
} // namespace Slic3r
