#pragma once

// Data-dependent edge flipping: choose each quad's diagonal to follow the height field, before any
// displacement happens.
//
// Refinement produces a regular grid, and a step in the height map that crosses that grid at an
// angle lands on alternating corners: one triangle of a quad gets a raised corner, the next does not,
// and the displaced wall comes out as a sawtooth the size of the grid. Finer triangles make the teeth
// smaller, never straight. The cause is the diagonal, not the density: with the diagonal running
// along the step both triangles of the quad sit cleanly on one side or the other, and the wall is a
// straight line between them.
//
// So, for every interior edge, compare the two diagonals of the quad it spans by how well each
// interpolates the field at its own midpoint - the sampled height there against the mean of its two
// endpoints - and keep the better one. A quad whose four corners are level is skipped outright, so
// flat regions cost nothing; a non-planar quad (a model crease) is never touched, nor is one whose
// triangles belong to different source faces or straddle the painted boundary.

#include <cstdint>
#include <vector>

#include "TextureBakeDisplace.hpp"
#include "TextureBakeIndex.hpp"

namespace Slic3r {
namespace TextureBake {

struct FlipSettings
{
    // Passes over all edges. Flips interact through shared triangles, so a pass applies non-conflicting
    // ones and the next pass picks up the rest; two or three settle a grid.
    int passes = 3;

    // A flip has to reduce the midpoint error by at least this fraction of the height range, so noise
    // on a rough surface does not toggle diagonals for nothing.
    double min_gain_fraction = 0.02;

    // The two triangles have to be this coplanar (cosine of their normals' angle) for the quad to have
    // a meaningful alternative diagonal at all: across a real crease there is none.
    double min_planar_cos = 0.985; // ~10 degrees
};

struct FlipResult
{
    TriSoup          geometry;
    std::vector<int> face_parent_id;
    size_t           flipped = 0;
};

// `face_parent_id` may be empty; when given it is carried through unchanged (a flip never crosses a
// parent boundary). `locked` flags triangles that must not change (per triangle, may be empty).
FlipResult flip_edges_to_height(const TriSoup &geometry, const std::vector<int> &face_parent_id,
                                const HeightSampleFn &sample, const FlipSettings &settings,
                                const std::vector<uint8_t> &locked);

} // namespace TextureBake
} // namespace Slic3r
