#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "../libslic3r.h"
#include "FillBase.hpp"
#include "../BoundingBox.hpp"
#include "../ExPolygon.hpp"
#include "../Point.hpp"
#include "../Polyline.hpp"
#include "../PrintConfig.hpp"

namespace Slic3r {

// Radial coordinate inside the lobes of the bodies of an object, sampled from its slices: 0 at the center of a
// lobe, 1 at its surface. Lobes are parts of a body separated by a neck, like two spheres united. In the 2D modes,
// every section normal to the axis has its own bodies and lobes, and distances are measured within the section. The
// modes following the distance to the surface use the depth of every point instead; Distance warp also the lobes.
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
    TpmsRadialField(const std::vector<Slice> &slices, const BoundingBox &bbox, TpmsAdaptiveMode mode,
                    const std::function<void()> &throw_if_canceled);

    // Lobes blended near the sides between them, from a body or from each of the two sections around a point.
    static constexpr size_t MaxMorph = 4;
    using Radials                    = std::array<Radial, 2 * MaxMorph>;

    // Without a body, as when the object is thinner than the grid cells.
    bool empty() const { return m_bodies.empty(); }

    // Radial coordinates of pt in unscaled coordinates towards the lobe it belongs to, and towards the neighbouring
    // lobes near the sides between them, with weights summing to 1. In the 2D modes, those of the two sections around
    // pt. Returns their count.
    size_t radial(const Vec3d &pt, Radials &out) const;

    // Axis normal to the sections in the 2D modes, -1 in the modes graded in 3D.
    int              axis() const { return m_axis; }
    TpmsAdaptiveMode mode() const { return m_mode; }

    // In the modes following the distance to the surface: depth relative to the deepest point of the body, from 0 at
    // the surface to 1.
    double depth(const Vec3d &pt) const;

private:
    struct Lobe
    {
        Vec3d  center;
        double depth;
        // Distance from the center to the surface on a latitude-longitude grid of directions.
        std::vector<float> reach;
        // Distance warp: mean depth over the ball along each direction, sampled up to the reach.
        std::vector<float> mean_depth;
    };

    struct Body
    {
        size_t first_lobe;
        size_t lobes;
        // Distance from the deepest point to the surface.
        double depth;
    };

    double radial(const Lobe &lobe, const Vec3d &pt) const;
    size_t body_radial(size_t node, const Vec3d &pt, float weight, Radial *out) const;
    // Offset of pt from a center, within the section in the 2D modes.
    Vec3d  offset(const Vec3d &pt, const Vec3d &center) const;
    int    directions() const { return m_axis < 0 ? Polar * Azimuth : Azimuth; }

    // Directions of the reach of a lobe, on a latitude-longitude grid, or a circle in the 2D modes.
    static constexpr int Polar   = 24;
    static constexpr int Azimuth = 48;

    TpmsAdaptiveMode  m_mode;
    int               m_axis;
    Vec3d             m_origin;
    double            m_cell;
    Vec3i32           m_size;
    // Nearest body of every grid node.
    std::vector<int>  m_body;
    std::vector<Body> m_bodies;
    std::vector<Lobe> m_lobes;
    // In the 2D modes, the nearest section with a body to every section.
    std::vector<int>  m_section;
    // In the modes following the distance to the surface, the depth of every grid node.
    std::vector<float> m_depth;
};

using TpmsRadialFieldPtr = std::unique_ptr<TpmsRadialField>;
// A field for every adaptive mode in use, indexed by the mode.
using TpmsRadialFields = std::array<TpmsRadialFieldPtr, size_t(TpmsAdaptiveMode::Count)>;

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

struct TpmsShell
{
    float      density;
    ExPolygons expolygons;
};

// Stepped shells: the parts of an expolygon in the object frame at each density, from the surface inwards; z is the
// middle of the layer.
std::vector<TpmsShell> make_tpms_shells(const TpmsRadialField &field, const ExPolygon &expolygon, coordf_t z,
                                        float surface_density, float interior_density, TpmsAdaptiveGradient gradient);

// Stepped shells: fills every shell with fill_shell at its density, each shrunk by half a line like a filled region,
// so the lines connected along the boundaries of two shells don't overlap.
void fill_tpms_shells(const TpmsRadialField &field, const ExPolygon &expolygon, coordf_t z, const FillParams &params, coordf_t spacing,
                      const std::function<void(const FillParams &, const ExPolygon &)> &fill_shell);

} // namespace Slic3r
