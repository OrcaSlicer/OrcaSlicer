#pragma once

// Declarative cut planes: the support-generation view of one segment's bounding half-spaces.
//
// A segment is confined by the clips on its CutSegmentDescriptor (object-local half-spaces,
// `normal.dot(p) - offset <= 0`). This class re-expresses those clips in the segment's own SLICE
// frame, where support generation works, and answers the two questions the generators ask:
//
//   * where support is allowed at a given layer   -> valid_region_polygon()
//   * how far down a branch may descend at an XY  -> floor_z()
//
// Roles matter. A far-side clip (CutClipRole::Floor) is load-bearing: the segment rests on that cut
// face, so it defines the floor and is a legitimate surface for branches to stem from. A near-side
// clip (CutClipRole::Ceiling) is a plane the segment sits BELOW -- the cut face of a segment printed
// LATER. Support must stay below it but must never rest on it, which is why ceilings clip the valid
// region yet are excluded from floor_z(). A downward-V segment genuinely rests on two cut faces, so
// every far-side clip is a floor, not just the base plane.
//
// The root segment additionally rests on the build plate; every other segment rests on its parent
// through its base plane. A tilted segment that also reaches the plate carries a Bed clip
// (CutClipRole::Bed), which is a floor here like any far-side clip.
//
// This is the declarative-segment replacement for the old world-frame CutPlaneFloor: the clips are
// already exact and object-local, so there is no config round-trip through world coordinates and no
// dependency on cut_group_id / cut_floor / support_ceilings.

#include "../libslic3r.h"
#include "../CutPlaneSegmentation.hpp"
#include "../Point.hpp"
#include "../Polygon.hpp"

#include <vector>

namespace Slic3r {

class PrintObject;

class SegmentSupportBounds
{
public:
    SegmentSupportBounds() = default;

    // Build from a descriptor and the frame that maps object-local coordinates to the coordinates
    // the segment is actually sliced in (PrintObject::object_to_slice()). bed_clearance raises the
    // build-plate floor by that many mm so support rests a first-layer height ABOVE the bed rather
    // than exactly on it, keeping the un-rotated G-code clear of the machine's minimum Z. Only the
    // bed plane is raised; cut planes (resting on the parent segment) are unaffected.
    // BELT_MOD: root_on_plate = false skips the root's horizontal plate plane; the caller adds the
    // machine's real build surface instead (a belt is a tilted plane in the root's slice frame).
    SegmentSupportBounds(const CutSegmentDescriptor &segment,
                         const Transform3d          &object_to_slice,
                         double                      bed_clearance = 0.,
                         bool                        root_on_plate = true);

    // Same, for a PrintObject that is one segment of a cut-plane partition. For an ordinary object,
    // which carries no descriptor, it holds only the build plate, and only when the plate is tilted
    // in the object's slice frame (PrintObject::support_floor_is_tilted(): a belt printer, or a
    // whole-object layer rotation); otherwise it is inactive -- and so a no-op for every caller.
    explicit SegmentSupportBounds(const PrintObject &object, double bed_clearance = 0.);

    bool is_active() const { return m_active; }

    // XY region at layer print_z where support is ALLOWED: the segment side of ALL bounds
    // (intersection of the half-planes), floors and ceilings alike.
    Polygons valid_region_polygon(coordf_t print_z) const;
    // The same, letting support reach `overlap` past the ceilings -- but only inside `column`, the
    // support-on-support column at this height (PrintObject::segment_support_column_at). Closes
    // the up-to-one-layer seam whole layers leave under a tilted plane; never under the model.
    Polygons valid_region_polygon(coordf_t print_z, const Polygons &column, double overlap) const;

    // The part of `xy` (scaled slice-frame polygons) that lies inside EVERY bound once lifted onto
    // the plane z = a*x + b*y + d (unscaled mm). Used by the support cascade to hand a child's
    // footprint only to the receivers whose floor/ceiling slab it actually enters at the child's
    // base plane: each bound is a half-space, which restricted to that plane is a half-plane.
    Polygons on_plane_inside(const Polygons &xy, double a, double b, double d) const;

    // Floor height (slice-frame, unscaled mm) at a scaled XY point: the highest floor there, since
    // a segment rests on whichever of its cut faces is uppermost. Returns 0 when no floor covers
    // the point, so callers read "no floor to descend to" rather than an unbounded descent.
    double floor_z(const Point &p) const;

    // XY region where a FLOOR passes through the layer slab [z_lo, z_hi]: material there rests on
    // the floor (the belt, or the parent segment's cut face), not on the layer below. Overhang
    // detection adds it to the lower layer's polygons, otherwise the leading strip of every tilted
    // layer reads as a floating region. Empty when inactive or without floors.
    Polygons floor_band(coordf_t z_lo, coordf_t z_hi) const;
    // Half-plane polygon at layer z for an arbitrary slice-frame plane `n . x >= d`, in the same
    // centered slice coordinates the layer polygons use; inside_side selects the n . x >= d side.
    // Public and static because PrintObject::slice_volumes() needs exactly this construction to
    // clip layers against the build plate -- for ordinary objects as well as segments, so it
    // cannot go through an instance of this class -- and a second copy of the maths would drift.
    static Polygons half_plane_at(const Vec3d &n, double d, coordf_t print_z, bool inside_side);

private:
    // n . x = d, n pointing INTO the segment; a point is inside the bound iff n.x >= d. `ceiling`
    // marks a bound the segment sits below, which clips but never bears load (see the class note).
    struct Plane { Vec3d n; double d; bool ceiling = false; };

    // A large half-plane polygon for the segment side (segment_side = true) or the outside
    // (false) of one bound, at layer print_z. Both directions are kept: valid_region_polygon()
    // needs the inside, and half_plane's own degenerate handling is written in terms of both.
    Polygons half_plane(const Plane &pl, coordf_t print_z, bool segment_side) const;

    bool               m_active = false;
    std::vector<Plane> m_planes;
};

} // namespace Slic3r
