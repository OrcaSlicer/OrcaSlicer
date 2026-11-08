#ifndef slic3r_BeltBrim_hpp_
#define slic3r_BeltBrim_hpp_

#include "ExPolygon.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "Point.hpp"
#include "Polyline.hpp"

#include <cmath>
#include <vector>

// BELT_MOD -- belt-printer brim geometry, ported from OrcaSlicer's belt branch.
//
// A belt object is sliced in a ROTATED frame, so the belt surface is not the Z = 0 plane but a
// tilted plane in slicing space, PrintObject::bed_plane_in_slice_frame():
//
//     bed_z(p) = n . p + k,   the belt is bed_z = 0
//
// With the gantry tilted about X, n has no X component and the belt height in the slice frame
// depends on y alone: z_belt(u) = shear * u + z0 with u = y. Consequences that drive this file:
//
//   * A horizontal slicing layer touches the belt only along a narrow strip at its leading edge,
//     layer_height / |shear| wide (~0.2 mm at 45 degrees). The object's belt footprint (its bottom
//     face) is therefore spread over every layer, not contained in layer 0.
//   * Distances measured in slicing XY are NOT on-belt distances: moving du along the shear axis
//     travels du / cos(tilt) across the belt. Brim offsets are taken in a "flattened" space where
//     the shear axis is stretched by 1 / cos(tilt), then mapped back.
//   * Brim ahead of the part (downhill) lies at slicing Z BELOW the object's first layer, because
//     the object's layer 0 is precisely its leading contact with the belt.
//
// Everything here is pure geometry on ExPolygons/Polylines so it can be unit tested without a
// Print. Keep user-visible strings out of this file.

namespace Slic3r {

// Tilt window within which the BELT plane, not the bed plane, is the adhesion surface. Below
// about 1 degree a belt is a flat bed as far as adhesion goes; above about 85 degrees the whole
// brim compresses into a sliver and is not worth generating.
inline constexpr double BELT_BRIM_MIN_TILT_DEG = 1.;
inline constexpr double BELT_BRIM_MAX_TILT_DEG = 85.;

// The tilted belt plane, reduced to what the brim geometry needs.
struct BeltBrimFrame
{
    // dz/du of the belt plane in the slice frame (tan(tilt) up to sign). Sign selects downhill.
    double shear     = 0.;
    // 0 = X, 1 = Y: the slice-XY axis along which the belt height varies.
    int    from_axis = 1;
    // z_belt(u) = shear * u + z0.
    double z0        = 0.;

    // 1 / cos(tilt): turns a projected distance along `from_axis` into the true distance across the belt.
    double u_stretch() const { return std::sqrt(1. + shear * shear); }
    double cos_tilt()  const { return 1. / this->u_stretch(); }
    // Downhill is where the belt surface is lower, i.e. printed earlier: the leading edge.
    int    downhill_sign() const { return shear > 0. ? -1 : +1; }
    // Where the belt plane crosses height z, and the belt height at u.
    double cutoff_u(double z) const { return (z - z0) / shear; }
    double floor_z(double u)  const { return shear * u + z0; }
};

// Scale only the `from_axis` component by `factor`, rounding to nearest (MultiPoint::scale
// truncates toward zero, which drifts every vertex on a round trip).
ExPolygons belt_scale_u(const ExPolygons &src, const BeltBrimFrame &frame, double factor);
Polylines  belt_scale_u(const Polylines  &src, const BeltBrimFrame &frame, double factor);

// Into / out of the space where Euclidean offsets equal true on-belt distances.
inline ExPolygons belt_flatten(const ExPolygons &src, const BeltBrimFrame &frame)
    { return belt_scale_u(src, frame, frame.u_stretch()); }
inline ExPolygons belt_unflatten(const ExPolygons &src, const BeltBrimFrame &frame)
    { return belt_scale_u(src, frame, frame.cos_tilt()); }

// Minkowski sum of `src` with the segment [0, t]: the region swept by sliding `src` along t.
ExPolygons sweep_ex(const ExPolygons &src, const Point &t);

// Brim region for one already-flattened belt footprint (all lengths scaled, in the flattened
// metric). `leading` sweeps the footprint DOWNHILL along the belt (leading_brim_length);
// `lateral` sweeps it BOTH WAYS across the belt (extra_brim_width). Neither applies to the inner ring.
ExPolygons belt_brim_region(const ExPolygons   &footprint_flat,
                            bool                has_outer,
                            bool                has_inner,
                            coord_t             brim_width,
                            coord_t             object_gap,
                            coord_t             leading,
                            coord_t             lateral,
                            const BeltBrimFrame &frame);

// Brim line positions for one layer band on a fixed lattice `u_anchor + k * pitch_u`, half-open
// [u_lo, u_hi) so every lattice point belongs to exactly one band.
std::vector<coord_t> belt_brim_line_positions(coord_t u_lo, coord_t u_hi, coord_t pitch_u, coord_t u_anchor);

// One brim-only layer printed BEFORE the object's first layer: the apron ahead of the part.
// Deliberately not a Layer: a synthetic Layer would leak its id() into initial-layer temperature
// selection, the spiral vase probe and cooling. `height` is the layer height (Z move and ordering);
// each path inside `fills` carries its own height, its nozzle-to-belt clearance.
struct BeltBrimBand
{
    coordf_t                  print_z = 0.;
    coordf_t                  height  = 0.;
    ExtrusionEntityCollection fills;    // erBrim paths, object-local slice frame
    ExPolygons                areas;    // their footprint, for the first-layer hull / bbox
};

class PrintObject;

// Build the frame from the object's belt plane. False when the object's floor is not a belt tilted
// about one axis within the brim's tilt window.
bool belt_brim_frame(const PrintObject &object, BeltBrimFrame &frame);

// Generate the belt brim for one object: its per-layer bands and its apron prologue. No-op unless
// PrintObject::has_belt_brim(). Runs inside posSupportMaterial, because the prologue print_z values
// must exist before ToolOrdering is built at psWipeTower.
void make_belt_brim(PrintObject &object);

} // namespace Slic3r

#endif // slic3r_BeltBrim_hpp_
