#pragma once

#include "libslic3r.h"
#include "Point.hpp"
#include "BoundingBox.hpp"
#include "PrintConfig.hpp"
#include "Geometry.hpp"
#include "Config.hpp"

#include <cmath>

namespace Slic3r {

class ModelObject;

// Shared belt-printer transform math.
//
// The pre-slice pipeline applied in PrintObjectSlice.cpp is:
//     trafo_out = z_shift * rotation * trafo_in
//
// Rotation is the sole mesh-side belt transform; shear & scale are applied
// to the g-code instead (see MachineFrameTransform).  This class provides the
// building blocks so every call site uses the same implementation.  z_shift is
// object-dependent (computed from mesh vertex bounds) and is NOT included in
// build_forward_transform().  The machine-frame shear/scale is derived directly
// from the tilt angle in MachineFrameTransform and no longer lives here.
//
// Design note: this mesh-rotation approach replaced an earlier pre-shear
// method (now removed).  While that initial pre-shear method was instrumental
// in getting belt printer slicing off the ground in the first place, its place is
// in the past.  A big thank you goes to the Unlayered3D team, who recommended 
// switching to a pre-slice rotation stage instead.  Doing so keeps the slicing 
// operation isometric — no distortion of the sliced geometry — while the 
// non-orthogonal machine-axis compensation is confined to a g-code-side shear/scale
// derived from the same tilt angle.
// 
// This fixed a number of issues, including several issues noticed by hotcubcar
// regarding adaptive infills not working, gyroid becoming anisotropic, and more 
// that were all mostly resolved as a result of the switch.
// 
// This also means that the pre-slice rotation transform methodology can be used 
// more cleanly on non-belt printers.
// - HarrierPigeon (Joseph Robertson)

class BeltTransformPipeline
{
public:
    // ---- Identity checks --------------------------------------------------

    // Whether the G-code axis remap applies at all. The remap fields are only
    // offered in the belt printer group, so a value left in a profile must not
    // change a non-belt print: with belt mode off every belt-only key is a no-op.
    // This is the one place to widen if a non-belt use ever needs them.
    static bool axis_remap_enabled(const PrintConfig &config) { return config.belt_printer.value; }

    static bool has_rotation(const PrintConfig &config)
    {
        return config.belt_slice_rotation.value != BeltRotationAxis::None &&
               std::abs(config.belt_slice_rotation_angle.value) > EPSILON;
    }

    // Physical belt tilt derived from the slicing rotation — the single source of
    // truth for bed rendering, support gravity tilt and the bed-exclusion
    // projection.  Returns the tilt magnitude in degrees split onto the X and Y
    // build-plate tilt axes according to the rotation axis:
    //   rotation about X  → tilt_x = angle   (gantry tilts in the YZ plane)
    //   rotation about Y  → tilt_y = angle   (gantry tilts in the XZ plane)
    //   rotation about Z / None → no tilt    (in-plane spin doesn't tilt the belt)
    // The magnitude uses abs(angle) so a negative rotation still reports a positive
    // physical tilt.
    struct PhysicalTilt { double tilt_x_deg = 0.; double tilt_y_deg = 0.; };

    static PhysicalTilt physical_tilt(BeltRotationAxis axis, double angle_deg)
    {
        PhysicalTilt t;
        double mag = std::abs(angle_deg);
        switch (axis) {
        case BeltRotationAxis::X: t.tilt_x_deg = mag; break;
        case BeltRotationAxis::Y: t.tilt_y_deg = mag; break;
        default: break; // Z / None: no physical tilt
        }
        return t;
    }

    // ---- Matrix builders --------------------------------------------------

    // Build the 3x3 rotation matrix from belt_slice_rotation* config.
    // Returns Identity if rotation axis is None or angle is ~0.
    // Also sets has_rot_out if non-null.
    static Matrix3d build_rotation_matrix(const PrintConfig &config, bool *has_rot_out = nullptr);

    // Forward transform (the rotation) — the mesh-side belt transform that
    // BeltSliceStrategy applies and BeltBackTransform inverts.
    // Does NOT include the per-object Z-shift.
    static Transform3d build_forward_transform(const PrintConfig &config);

    // ---- Belt floor parameters --------------------------------------------

    struct BeltFloorParams {
        double shear_factor = 0.0;
        int    from_axis    = 1;
        double z_shift      = 0.0;
    };

    // Shear factor and from-axis of the belt floor in the rotated slicer frame
    // (z_floor = shear_factor * u, u = the from-axis coordinate), for the
    // rotation the config selects.  z_shift is left at 0.  Returns false (and
    // zero shear) when the config has no tilt.
    static bool floor_shear(const PrintConfig &config, BeltFloorParams &out);

    // How far below the lowest belt-floor point under the footprint the slicing
    // frame starts, in slicing Z.  A support column meeting the belt is wider at
    // its base than at its tip, so under a leading overhang the base reaches ahead
    // of the part along the belt, and the layers that trim it to the belt plane
    // lie below that lowest point: 10 mm along the belt.
    static double frame_margin(const BeltFloorParams &fp) { return 10. * std::abs(fp.shear_factor); }

    // Result of computing belt height + floor params.
    struct BeltHeightResult {
        double          object_height;  // Effective object height after shear/scale
        BeltFloorParams floor_params;
    };

    // Compute effective object height and belt floor parameters from config
    // and the object's bounding box.  original_height is the input height
    // (bb.size().z() or model_object.max_z()).
    static BeltHeightResult compute_belt_height_and_floor(
        const PrintConfig &config, const BoundingBoxf3 &bbox,
        double original_height);
};

} // namespace Slic3r
