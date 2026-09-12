#pragma once

// The bake pipeline:
//
//   subdivide -> [regularize -> re-subdivide] -> displace -> [decimate]
//             -> bottom clamp -> bottom snap -> [resolve T-junctions]
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
#include "TextureBakeRepair.hpp"
#include "TextureBakeSubdivide.hpp"

namespace Slic3r {
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

    DisplaceSettings displace;

    // Export mode only.
    size_t max_triangles = 750'000;
    bool   harvest_flat  = true;
    double harvest_tol   = DECIMATE_DEFAULT_HARVEST_TOL;
    // Lock the untextured region against both regularization and decimation.
    bool preserve_untextured = true;

    // Snap vertices within this of the bottom plane onto it. 0 disables.
    double bottom_snap_tol = 0.1;

    int  safety_cap = SUBDIVIDE_SAFETY_CAP;
};

// Stage name and a fraction within it. Returning false cancels the run.
using PipelineProgressFn = std::function<bool(const char *stage, double fraction)>;

struct PipelineResult
{
    TriSoup geometry;
    // Output face -> input face. Empty in Export mode, where decimation invalidates it.
    std::vector<int> face_parent_id;
    bool             safety_cap_hit     = false;
    bool             locked_over_budget = false;
    size_t           collapse_count     = 0;
    bool             canceled           = false;
};

PipelineResult run_pipeline(const TriSoup &input, const HeightSampleFn &sample,
                            const PipelineSettings &settings, const DisplaceBounds &bounds,
                            PipelineMode mode, const std::vector<uint8_t> &face_excluded = {},
                            const PipelineProgressFn &on_progress = {});

// Snap anything that ended below the model's original bottom back up to it.
void clamp_below_bottom(TriSoup &geometry, float bottom_z);

// Flatten the bed-contact surface by snapping positions within `tol` of the bottom plane onto it.
//
// Gated, not unconditional: an unconditional band snap also flattens the undersides of texture bumps
// near the base, folding them coplanar into the bottom face. Folded faces overlap the plate, so edges
// there pick up four incident faces - non-manifold edges and phantom shells on re-import. All copies
// of a position move together, and the move is rejected if any incident triangle would go degenerate
// or rotate more than about 75 degrees. A real bed-contact sliver rotates by a fraction of a degree
// and still snaps. Returns how many triangles moved.
size_t snap_bottom_to_flat(TriSoup &geometry, float bottom_z, double tol = 0.1);

} // namespace TextureBake
} // namespace Slic3r
