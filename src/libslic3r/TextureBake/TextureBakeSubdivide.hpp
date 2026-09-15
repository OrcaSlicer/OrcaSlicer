#pragma once

// Adaptive subdivision to a target edge length, by global marked-edge (red-green) refinement rather
// than longest-edge bisection. Marking is global, so two triangles sharing an edge always agree and
// the result is crack-free by construction. A triangle is rebuilt from its marked-edge count: 0
// keeps, 1 bisects, 2 fans into three, 3 does the regular 1->4 split.
//
// The 1->4 case is what keeps the tessellation regular - its children are similar to the parent. An
// irregular one shows up after displacement as adjacent triangles tilting alternately, i.e. noise.

#include <functional>
#include <vector>

#include "TextureBakeIndex.hpp"

namespace Slic3r {
namespace TextureBake {

// Memory guard for the stages downstream. At roughly 145 bytes per triangle this is about 2.9 GB.
static constexpr int SUBDIVIDE_SAFETY_CAP = 16'000'000;

// Vertices at one position stay separate when their faces disagree by more than this: a cube keeps
// hard edges, a cylinder keeps averaged ones.
static constexpr double SUBDIVIDE_SHARP_ANGLE_DEG = 30.0;

// A depth bound, not a work bound: the loop stops as soon as a pass changes nothing.
static constexpr int SUBDIVIDE_MAX_ITERATIONS = 12;

// Built by the indexers, appended to by the passes. Double precision so repeated midpointing does
// not drift.
struct VertStore
{
    std::vector<Vec3d>  pos;
    std::vector<Vec3d>  nrm;
    std::vector<double> wgt;   // exclusion weights; empty when the caller supplied none
    std::vector<int>    canon; // canonical position ids; empty in fast mode

    size_t count() const { return pos.size(); }
    int    push(const Vec3d &p, const Vec3d &n)
    {
        const int idx = int(pos.size());
        pos.push_back(p);
        nrm.push_back(n);
        return idx;
    }
};

struct IndexedMesh
{
    VertStore         verts;
    std::vector<int>  indices; // 3 per triangle
    QuantizedPointMap pos_canon_map{ WELD_GRID_GEOMETRY, 256 };
    bool              has_canon = false;
};

// Fraction, triangle count, longest remaining edge. Returning false cancels; what comes back is
// still watertight, because passes apply whole or not at all.
using SubdivideProgressFn = std::function<bool(double fraction, size_t triangles, double longest_edge)>;

struct SubdivideResult
{
    TriSoup geometry;
    // Output triangle -> input triangle it descends from, so per-face data survives with no remap.
    std::vector<int> face_parent_id;
    bool             safety_cap_hit = false;
};

// `face_excluded`: one entry per input triangle; non-zero means its interior is never refined. Its
// edges still split when an included neighbour marks them, so no T-junction appears at the boundary.
// `fast` selects the cheap position-only indexer for previews.
SubdivideResult subdivide(const TriSoup &geometry, double max_edge_length,
                          const std::vector<uint8_t> &face_excluded = {}, bool fast = false,
                          int safety_cap = SUBDIVIDE_SAFETY_CAP,
                          const SubdivideProgressFn &on_progress = {});

// Displacement needs the same welding and sharp-edge clustering.
IndexedMesh to_indexed(const TriSoup &geometry);
IndexedMesh to_indexed_fast(const TriSoup &geometry);
TriSoup     to_non_indexed(const VertStore &verts, const std::vector<int> &indices,
                           const std::vector<uint8_t> &face_excluded);

} // namespace TextureBake
} // namespace Slic3r
