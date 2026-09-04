#pragma once

// Sliver removal by short-edge collapse.
//
// Subdivision turns tessellation needles into chains of slivers that are within the edge-length
// budget but still poor triangles. A sliver's three vertices land on three unrelated texels, so the
// relief picks up noise that is an artifact of the tessellation rather than of the image.
//
// A candidate's edge is collapsed to its midpoint only if it passes three gates: no affected
// triangle may exceed the target edge times a slack factor; every affected triangle must keep its
// face normal within a bound of its *original* direction (which is what stops curved surfaces being
// flattened); and the link condition must hold, or the result would be non-manifold. Boundary and
// non-manifold edges are skipped outright. Rounds repeat until one achieves nothing.

#include <cstdint>
#include <vector>

#include "TextureBakeIndex.hpp"

namespace Slic3r {
namespace TextureBake {

struct RegularizeOptions
{
    // Candidate threshold. Set to catch real slivers - chains measure in the hundreds - without
    // sweeping up moderate fillet triangles, which sit between 2 and 5.
    double aspect_threshold = 5.0;

    // The base tier is loose on purpose: non-sliver boundary collapses must keep succeeding, since
    // those give a chain the room to dissolve. A tight base leaves chains worse than before. The
    // aggressive tier applies when at least one wing is an extreme sliver.
    double slack            = 3.0;
    double aggressive_slack = 8.0;

    // Thinness above which a wing counts as extreme: longest edge over shortest altitude.
    double extreme_sliver_aspect = 8.0;

    // Measured against each triangle's normal from before any collapse ran, so rounds of small
    // allowed drift cannot compound into corner damage. Asymmetric two-tier: the loose bound needs
    // *both* wings extreme, which matches a needle chain on a curved face but not a sliver beside a
    // fillet, so fillets keep the tight bound.
    double max_normal_delta_cos        = 0.965925826289; // cos(15 degrees)
    double aggressive_normal_delta_cos = 0.906307787037; // cos(25 degrees)

    // Vertices on edges sharper than this are frozen, so hard features keep every original vertex.
    double sharp_edge_cos = 0.866025403784; // cos(30 degrees)

    int maxrounds = 8;

    // Freeze excluded faces entirely, so untextured geometry is never modified.
    bool preserve_excluded = false;
};

// Which gate blocked a collapse - the only practical way to tell why a region failed to merge.
struct RegularizeRejectStats
{
    size_t frozen = 0, wing_count = 0, link_condition = 0, edge_cap = 0, normal_change = 0,
           degenerate = 0, folded_apex = 0;
};

struct RegularizeResult
{
    TriSoup               geometry;
    std::vector<int>      face_parent_id;
    size_t                collapse_count = 0;
    RegularizeRejectStats reject_stats;
};

RegularizeResult regularize_mesh(const TriSoup &geometry, const std::vector<int> &face_parent_id,
                                 double max_edge_length, const RegularizeOptions &opts = {});

} // namespace TextureBake
} // namespace Slic3r
