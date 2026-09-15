#ifndef slic3r_FirstLayerPlane_hpp_
#define slic3r_FirstLayerPlane_hpp_

#include "libslic3r.h"
#include "Point.hpp"
#include "BoundingBox.hpp"
#include "PrintConfig.hpp"

namespace Slic3r {

// Decides which extrusions get "first layer" treatment (no fan, slow speed,
// initial-layer accel/jerk, deferred temperature drop) by reference to a
// configurable plane in slicing-frame coordinates rather than the slicing
// layer index.
//
// On a normal flat-bed printer the plane is XY at slicing_Z = 0 and the
// evaluator is INACTIVE — every call site short-circuits back to the legacy
// `Layer::id() == 0` test.  On a belt printer with a Z-from-Y shear the
// belt surface (machine_Z = 0) maps to a plane in slicing-frame coordinates
// derived from the gcode axis remap, so layer-index-based detection no
// longer matches the physical first printed surface.
//
// Plane representation: unit normal `n` (slicing frame) and offset along
// the normal such that the plane equation is `n · p == offset`.  Signed
// perpendicular distance is `d(p) = n · p - offset`.  Positive distance
// means "away from the belt surface", negative means "below the plane".
class FirstLayerPlane
{
public:
    explicit FirstLayerPlane(const PrintConfig &config);

    // Inactive when the legacy XY layer-index path should be used.  This
    // covers all non-belt printers and any belt printer where the user
    // explicitly picked XY mode.
    bool                is_active() const     { return m_active; }
    FirstLayerPlaneMode effective_mode() const{ return m_mode; }
    double              band_thickness_mm() const { return m_thickness_mm; }
    const Vec3d &       normal() const        { return m_normal; }
    double              plane_offset() const  { return m_offset; }

    // Signed perpendicular distance from a slicing-frame point to the plane.
    double distance_from_plane(const Vec3d &point_slicing_mm) const;

    // True if perpendicular distance < first_layer_height_mm.  When the
    // evaluator is inactive this returns false (call sites should fall back
    // to the legacy per-layer path before reaching this function).
    bool is_first_layer(const Vec3d &point_slicing_mm,
                        double first_layer_height_mm) const;

    // floor((distance - 0) / band_thickness), clamped to [0, +inf).  Used
    // for "first N layers" thresholds (fan, slow_down_layers).  Returns 0
    // for points within the band.  Returns INT_MAX/2 when inactive.
    int effective_layer_index(const Vec3d &point_slicing_mm) const;

    // Min effective index over a 2D bbox at a fixed slicing_Z.  Used for
    // layer-level decisions (e.g. temperature transition gate) where we
    // don't want to walk every extrusion in the layer.  For axis-aligned
    // planes this is exact; for tilted planes it's a tight lower bound
    // (the plane projection of the bbox's extreme corner).
    int min_effective_index_for_xy_bbox(const BoundingBoxf &xy_bbox_mm,
                                        double              slicing_z_mm) const;

    // Same as above but the bbox spans a Z range too.
    int min_effective_index_for_bbox3(const BoundingBoxf3 &bbox_mm) const;

private:
    bool                m_active        = false;
    FirstLayerPlaneMode m_mode          = FirstLayerPlaneMode::XY;
    Vec3d               m_normal        = Vec3d::UnitZ();  // unit, slicing frame
    double              m_offset        = 0.0;             // n·p == m_offset
    double              m_thickness_mm  = 0.0;
};

} // namespace Slic3r

#endif // slic3r_FirstLayerPlane_hpp_
