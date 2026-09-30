#pragma once

#include <array>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "../libslic3r.h"
#include "../BoundingBox.hpp"
#include "../ExPolygon.hpp"
#include "../Point.hpp"
#include "../Polyline.hpp"
#include "../PrintConfig.hpp"

namespace Slic3r {

// Radial coordinate inside the lobes of the bodies of an object, sampled from its slices: 0 at the center of a
// lobe, 1 at its surface. Lobes are parts of a body separated by a neck, like two spheres united.
class TpmsRadialField
{
public:
    struct Slice
    {
        coordf_t          bottom_z;
        coordf_t          top_z;
        const ExPolygons *expolygons;
    };

    struct Radial
    {
        Vec3d  center;
        double t;
        float  weight;
    };

    // Slices sorted by z, in the XY coordinates of the fill and the print Z.
    TpmsRadialField(const std::vector<Slice> &slices, const BoundingBox &bbox, const std::function<void()> &throw_if_canceled);

    // Radial coordinates of pt in unscaled coordinates towards the lobe it belongs to, and towards a neighbouring
    // lobe near the side between them, with weights summing to 1. Returns their count.
    size_t radial(const Vec3d &pt, std::array<Radial, 2> &out) const;

private:
    struct Lobe
    {
        Vec3d  center;
        double depth;
        // Distance from the center to the surface on a latitude-longitude grid of directions.
        std::vector<float> reach;
    };

    struct Body
    {
        size_t first_lobe;
        size_t lobes;
    };

    double radial(const Lobe &lobe, const Vec3d &pt) const;

    Vec3d             m_origin;
    double            m_cell;
    Vec3i32           m_size;
    // Nearest body of every grid node.
    std::vector<int>  m_body;
    std::vector<Body> m_bodies;
    std::vector<Lobe> m_lobes;
};

using TpmsRadialFieldPtr = std::unique_ptr<TpmsRadialField>;

struct AdaptiveTpms
{
    // Implicit TPMS equation with a period of 2 PI.
    float (*equation)(float x, float y, float z);
    // Pattern frequencies at the surface and at the center, in radians per mm.
    double               surface_frequency;
    double               interior_frequency;
    TpmsAdaptiveGradient gradient;
};

// Infill lines in the fill frame, the object frame rotated by -angle; z is the print_z of the layer.
Polylines make_adaptive_tpms(const AdaptiveTpms &tpms, const TpmsRadialField &field, BoundingBox bbox,
                             coordf_t z, coordf_t layer_height, coordf_t spacing, float angle);

} // namespace Slic3r
