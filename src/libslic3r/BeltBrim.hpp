#ifndef slic3r_BeltBrim_hpp_
#define slic3r_BeltBrim_hpp_

#include "ExPolygon.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "Point.hpp"
#include "Polyline.hpp"

#include <cmath>
#include <vector>

// Belt-printer brim geometry.
//
// A belt printer slices in a ROTATED frame, so the belt surface is not the
// Z=0 bed plane but a tilted plane in slicing space:
//
//     z_slicing(u) = shear * u + floor_offset + z_shift,   u = X or Y
//
// where `shear == tan(tilt)` (SlicingParameters::belt_floor_shear_factor) and
// the axis is selected by SlicingParameters::belt_floor_from_axis.  See
// Support/BeltFloorContext.hpp for the canonical accessors.
//
// Consequences that drive everything in this file:
//
//   * A horizontal slicing layer touches the belt only along a narrow strip at
//     its leading edge, `layer_height / shear` wide (~0.2 mm at 45 degrees).
//     The object's belt footprint - its bottom face - is therefore spread over
//     every layer, not contained in layer 0.
//   * Distances measured in slicing XY are NOT on-belt distances: moving `du`
//     along the shear axis travels `du / cos(tilt)` across the belt.  So brim
//     offsets have to be taken in a "flattened" space where the shear axis is
//     stretched by `1 / cos(tilt)`, then mapped back.
//   * Brim ahead of the part (downhill) lies at slicing Z BELOW the object's
//     first layer, because the object's layer 0 is precisely its leading
//     contact with the belt.
//
// Everything here is pure geometry on ExPolygons/Polylines so it can be unit
// tested without a Print.  Keep user-visible strings out of this file: it is
// not listed in localization/i18n/list.txt.

namespace Slic3r {

// Tilt window within which the BELT plane, not the bed plane, is the adhesion
// surface.  Below ~1 degree a belt is a flat bed as far as adhesion goes, and the
// contact band would be layer_height/sin(tilt) - tens of millimetres - so the
// ordinary plate brim is both correct and cheaper.  Above ~85 degrees the whole
// brim compresses into a sliver and is not worth generating.
inline constexpr double BELT_BRIM_MIN_TILT_DEG = 1.;
inline constexpr double BELT_BRIM_MAX_TILT_DEG = 85.;

// Description of the tilted belt plane, reduced to what the brim geometry needs.
struct BeltBrimFrame
{
    // tan(tilt).  Sign selects which way is downhill.
    double shear     = 0.;
    // 0 = X, 1 = Y.  Matches BeltFloorContext::from_axis().
    int    from_axis = 1;

    // 1 / cos(tilt).  Stretch factor that turns a projected distance along
    // `from_axis` into the true distance travelled across the belt.
    double u_stretch() const { return std::sqrt(1. + shear * shear); }
    // cos(tilt).  The inverse mapping.
    double cos_tilt()  const { return 1. / this->u_stretch(); }
    // Downhill is where the belt surface is lower, i.e. printed earlier, i.e.
    // the leading edge of the part.  For shear > 0 that is -u.
    int    downhill_sign() const { return shear > 0. ? -1 : +1; }
};

// Scale only the `from_axis` component by `factor`, rounding to nearest.
//
// Deliberately not MultiPoint::scale(fx, fy) / ExPolygon::scale(fx, fy): those
// truncate toward zero, which is asymmetric about the origin and loses up to a
// full coordinate unit per vertex on every round trip.
ExPolygons belt_scale_u(const ExPolygons &src, const BeltBrimFrame &frame, double factor);
Polylines  belt_scale_u(const Polylines  &src, const BeltBrimFrame &frame, double factor);

// Into / out of the space where Euclidean offsets equal true on-belt distances.
inline ExPolygons belt_flatten(const ExPolygons &src, const BeltBrimFrame &frame)
    { return belt_scale_u(src, frame, frame.u_stretch()); }
inline ExPolygons belt_unflatten(const ExPolygons &src, const BeltBrimFrame &frame)
    { return belt_scale_u(src, frame, frame.cos_tilt()); }

// Minkowski sum of `src` with the segment [0, t]: the region swept by sliding
// `src` along t.  Used to grow the brim downhill for "extra brim width".
//
// Implemented as union_(P, P + t, {parallelogram per boundary edge}) over ALL
// contours including holes, with every parallelogram forced counter-clockwise
// so the non-zero fill rule closes holes narrower than t along the sweep
// direction.  A hole survives exactly when it is wider than |t| measured along
// t - not when it is wider in its narrowest Euclidean direction.
ExPolygons sweep_ex(const ExPolygons &src, const Point &t);

// Brim region for one already-flattened belt footprint.  All lengths are scaled
// and measured in the flattened (true on-belt) metric.
//
// `has_outer` / `has_inner` are the resolved BrimType: belt printers collapse
// Auto / Mouse ear / Painted to outer-only, so the caller does that mapping and
// this function never needs PrintConfig.
//
// Two directional extras are applied to the footprint before the outer offset, so
// each one buys reach in one direction only:
//
//   `leading` (leading_brim_length) sweeps the footprint DOWNHILL along the belt,
//   so every leading-facing edge gains an apron ahead of it.
//   `lateral`  (extra_brim_width)   sweeps it BOTH WAYS across the belt, widening
//   the brim sideways without pushing it further ahead or behind.
//
// Neither is applied to the inner (hole) ring.
ExPolygons belt_brim_region(const ExPolygons   &footprint_flat,
                            bool                has_outer,
                            bool                has_inner,
                            coord_t             brim_width,
                            coord_t             object_gap,
                            coord_t             leading,
                            coord_t             lateral,
                            const BeltBrimFrame &frame);

// Brim line positions for one layer band.
//
// Lines sit on a fixed lattice `u_anchor + k * pitch_u` so the on-belt spacing
// between neighbouring brim lines is constant regardless of how the lattice
// falls across layer bands.  Snapping to band centres instead would quantise
// the spacing to whole bands and under-deposit by ~35% at 45 degrees.
//
// The band is half-open, [u_lo, u_hi), so every lattice point belongs to
// exactly one band: none duplicated at a boundary, none dropped.  A band
// narrower than the pitch simply yields nothing; a band much wider (shallow
// tilt) yields several lines.
std::vector<coord_t> belt_brim_line_positions(coord_t u_lo,
                                              coord_t u_hi,
                                              coord_t pitch_u,
                                              coord_t u_anchor);

// ---------------------------------------------------------------- pipeline

// One brim-only layer printed BEFORE the object's first layer, carrying the
// apron that has to be stuck to the belt ahead of the part.
//
// Deliberately not a Layer subclass.  A synthetic Layer would inherit id()
// semantics that leak into initial-layer temperature selection, the spiral vase
// probe, gradual interpolation, avoid-crossing-perimeters and cooling, all of
// which key off Layer::id() == 0 or off a layer's regions.  A plain record
// carries only what the emitter needs.
//
// `height` is the LAYER height, used for the Z move and ordering metadata only.
// Each extrusion path inside `fills` carries its own height, equal to that
// line's nozzle-to-belt clearance, which varies across the band.
struct BeltBrimBand
{
    coordf_t                  print_z = 0.;
    coordf_t                  height  = 0.;
    // erBrim paths in the object's local slicing frame, untranslated.
    ExtrusionEntityCollection fills;
    // Footprint of those paths, for the first-layer convex hull / bbox.
    ExPolygons                areas;
};

class PrintObject;

// Generate the belt brim for one object: fills its per-object-layer bands and
// its apron prologue.  No-op unless PrintObject::has_belt_brim().
//
// Runs inside posSupportMaterial rather than the brim step, because the prologue
// print_z values must exist before ToolOrdering is built at psWipeTower.
void make_belt_brim(PrintObject &object);

} // namespace Slic3r

#endif // slic3r_BeltBrim_hpp_
