#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "../libslic3r.h"
#include "../BoundingBox.hpp"
#include "../ExPolygon.hpp"
#include "../Point.hpp"
#include "../Polyline.hpp"
#include "../PrintConfig.hpp"

namespace Slic3r {

// Depth inside an object sampled from its slices: 0 at the surface, 1 at the deepest point.
class TpmsDepthField
{
public:
    struct Slice
    {
        coordf_t          bottom_z;
        coordf_t          top_z;
        const ExPolygons *expolygons;
    };

    // Slices sorted by z, in the XY coordinates of the fill and the print Z.
    TpmsDepthField(const std::vector<Slice> &slices, const BoundingBox &bbox, const std::function<void()> &throw_if_canceled);

    // Unscaled coordinates.
    float depth(const Vec3d &pt) const;

private:
    Vec3d              m_origin;
    double             m_cell;
    Vec3i32            m_size;
    std::vector<float> m_depth;
};

using TpmsDepthFieldPtr = std::unique_ptr<TpmsDepthField>;

struct AdaptiveTpms
{
    // Implicit TPMS equation with a period of 2 PI.
    float (*equation)(float x, float y, float z);
    // Density fractions and the matching pattern frequencies in radians per mm.
    double               surface_density;
    double               interior_density;
    double               surface_frequency;
    double               interior_frequency;
    TpmsAdaptiveGradient gradient;
};

// Infill lines in the fill frame, the object frame rotated by -angle; z is the print_z of the layer.
Polylines make_adaptive_tpms(const AdaptiveTpms &tpms, const TpmsDepthField &depth_field, const BoundingBox &bbox,
                             coordf_t z, coordf_t layer_height, coordf_t spacing, float angle);

} // namespace Slic3r
