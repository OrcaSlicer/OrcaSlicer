#pragma once

// UNLAYERED_MOD -- support direction: growing support along a direction other than the layer normal.
//
// A layer-based support generator can only stack support straight along the slice frame's Z. To
// lean it along another unit direction u (slice frame, u.z > 0) without touching the three
// generators, the whole problem is sheared instead: every layer's geometry is translated by
// -z * (u.xy / u.z), which maps a column along u onto a vertical column. The unmodified generator
// then runs against that sheared object, and everything it produced is translated back by
// +z * (u.xy / u.z) at the layer's own print_z. A shear preserves z, so layers stay layers and an
// object/support contact at a given z stays a contact.
//
// What follows from that choice:
//   * overhang detection runs in the sheared frame too, so a face is judged against the support
//     direction rather than the layer normal -- the same call the OrcaSlicer belt branch makes for
//     its build-plate tilt, and the one that makes "world vertical" mean "what would fall";
//   * cut-plane floors and ceilings (SegmentSupportBounds) are re-expressed through the same shear,
//     so a segment's support still terminates on its cut face, and the cascade reads the footprint
//     it hands to the parent from the un-sheared result;
//   * the lean is limited to the object's support_direction_max_lean: a leaning column overhangs
//     itself by exactly the lean angle, so past safe_lean_deg() it needs tuning and test prints,
//     and generation warns. A direction past the limit keeps its azimuth and is pulled back to
//     the limit (a direction pointing straight down has no azimuth and falls back to following
//     the layers).

#include "../libslic3r.h"
#include "../Geometry.hpp"
#include "../Point.hpp"

#include <cmath>

namespace Slic3r {

class PrintObject;

// The lean in effect for one object's support generation.
struct SupportLean
{
    // XY drift of a support column per unit of slice-frame z, i.e. u.xy / u.z. Zero = follow layers.
    Vec2d slope   { 0., 0. };
    // The configured direction exceeded the object's max lean and was pulled back to it.
    bool  clamped { false };

    // Past this lean a support column overhangs itself beyond what prints reliably; the option
    // allows more, with a warning.
    static constexpr double safe_lean_deg() { return 45.; }

    bool        active() const { return slope.squaredNorm() > 1e-18; }
    // The lean actually in effect, in degrees away from the layer normal.
    double      lean_deg() const { return Geometry::rad2deg(std::atan(slope.norm())); }
    // Scaled translation that shears geometry lying at print_z into the frame where the lean is
    // vertical. The inverse is the negated point, exactly, so a round trip loses nothing.
    Point       shift_at(double print_z) const { return Point::new_scale(- print_z * slope.x(), - print_z * slope.y()); }
    // The same shear as an affine map on unscaled slice-frame coordinates.
    Transform3d shear() const;
};

// The lean the object's config asks for, resolved into its slice frame. Inactive for the default
// "follow layers", and for any direction that coincides with the layer normal.
SupportLean support_lean(const PrintObject &object);

// Holds the object's layers in the sheared frame for the lifetime of the scope and puts everything
// back -- layers, inherited cascade demand and the support layers generated meanwhile -- when it
// ends, including on an exception. Constructing it on an object whose lean is inactive is a no-op.
class ScopedSupportShear
{
public:
    explicit ScopedSupportShear(PrintObject &object);
    ~ScopedSupportShear();
    ScopedSupportShear(const ScopedSupportShear &) = delete;
    ScopedSupportShear& operator=(const ScopedSupportShear &) = delete;

    bool   active() const   { return m_lean.active(); }
    bool   clamped() const  { return m_lean.clamped; }
    double lean_deg() const { return m_lean.lean_deg(); }

private:
    PrintObject &m_object;
    SupportLean  m_lean;
};

} // namespace Slic3r
