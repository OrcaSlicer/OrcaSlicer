#pragma once

// Displacement along surface normals.
//
// The mesh is non-indexed, so at a shared edge two triangles hold the same position with different
// face normals; displacing each copy along its own normal sends them to different points and opens a
// crack. So one smooth (area-weighted) normal per unique position drives both the sample lookup and
// the displacement direction, every copy moves by the same vector, and the result is watertight by
// construction. Displaced normals are then smooth at hard edges, but the geometry is still faceted,
// so printed edges stay sharp.

#include <cstdint>
#include <functional>
#include <vector>

#include "TextureBakeIndex.hpp"

namespace Slic3r {
namespace TextureBake {

// Height at a point, called once per unique welded position. `smooth_normal` is the vector the
// displacement will move along; `blend_normal` is that after smoothing, for projection blend weights.
using HeightSampleFn = std::function<float(const Vec3f &position, const Vec3f &smooth_normal,
                                           const Vec3f &blend_normal)>;

struct DisplaceSettings
{
    // Displacement height in mm, applied to the sampled value.
    float amplitude = 0.4f;

    // Sample around a mid-grey rest level rather than displacing outward only.
    bool symmetric = false;

    // Faces flatter than these (degrees from horizontal) are held back, leaving bed-contact and top
    // surfaces alone. 0 disables that side.
    float bottom_angle_limit = 5.f;
    float top_angle_limit    = 0.f;

    // Never move a vertex below its original Z, so no new overhang. The sideways component is kept.
    bool no_downward_z = false;

    // Distance in mm over which displacement ramps up from a mask boundary. 0 leaves a hard edge.
    float boundary_falloff = 0.f;

    // Laplacian iterations on the blend normal only - the displacement direction must stay the exact
    // smooth normal or copies of a position move differently and the mesh cracks. Inside a blend band
    // the weight gradient is largest, so a few degrees of vertex-to-vertex jitter multiplies the
    // difference between two unrelated height samples into visible seam noise. A no-op on an
    // already-smooth surface.
    int blend_normal_smoothing = 32;
};

// Model extents; only the minimum Z is read, for the bottom-plane clamp.
struct DisplaceBounds
{
    Vec3f min = Vec3f::Zero();
    Vec3f max = Vec3f::Zero();
};

// Returns false to cancel.
using DisplaceProgressFn = std::function<bool(double fraction)>;

TriSoup apply_displacement(const TriSoup &geometry, const HeightSampleFn &sample,
                           const DisplaceSettings &settings, const DisplaceBounds &bounds,
                           const DisplaceProgressFn &on_progress = {});

} // namespace TextureBake
} // namespace Slic3r
