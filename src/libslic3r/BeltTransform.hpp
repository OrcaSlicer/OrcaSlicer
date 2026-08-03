#pragma once

#include "libslic3r.h"
#include "Point.hpp"
#include "BoundingBox.hpp"
#include "PrintConfig.hpp"
#include "Geometry.hpp"

#include <cmath>

namespace Slic3r {

class ModelObject;

// Shared belt-printer transform math.
//
// The pre-slice pipeline applied in PrintObjectSlice.cpp is:
//     trafo_out = z_shift * rotation * pre_remap * trafo_in
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

    static bool has_preslice_remap(const PrintConfig &config)
    {
        return int(config.preslice_remap_x.value) != int(RemapAxis::PosX) ||
               int(config.preslice_remap_y.value) != int(RemapAxis::PosY) ||
               int(config.preslice_remap_z.value) != int(RemapAxis::PosZ);
    }

    // Overload accepting DynamicPrintConfig (used in static slicing_parameters).
    static bool has_preslice_remap(const DynamicPrintConfig &config)
    {
        auto get_int = [&](const char *key) -> int {
            auto *opt = config.option<ConfigOptionEnum<RemapAxis>>(key);
            return opt ? int(opt->value) : 0;
        };
        return get_int("preslice_remap_x") != int(RemapAxis::PosX) ||
               get_int("preslice_remap_y") != int(RemapAxis::PosY) ||
               get_int("preslice_remap_z") != int(RemapAxis::PosZ);
    }

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

    static PhysicalTilt physical_tilt(const PrintConfig &config)
    {
        return physical_tilt(config.belt_slice_rotation.value,
                             config.belt_slice_rotation_angle.value);
    }

    // ---- Matrix builders --------------------------------------------------

    // Build the pre-slice axis remap transform (includes Rev-mode translation).
    static Transform3d build_preslice_remap(const PrintConfig &config);

    // Build the 3x3 rotation matrix from belt_slice_rotation* config.
    // Returns Identity if rotation axis is None or angle is ~0.
    // Also sets has_rot_out if non-null.
    static Matrix3d build_rotation_matrix(const PrintConfig &config, bool *has_rot_out = nullptr);

    // Combined forward transform (rotation * pre_remap) — the mesh-side belt
    // transform that BeltSliceStrategy applies and BeltBackTransform inverts.
    // Does NOT include the per-object Z-shift.
    static Transform3d build_forward_transform(const PrintConfig &config);

    // ---- Bounding box remap -----------------------------------------------

    // Remap a bounding box through the pre-slice axis remap.
    // Returns the original bbox if remap is identity.
    static BoundingBoxf3 remap_bbox(const BoundingBoxf3 &bb, const PrintConfig &config);
    static BoundingBoxf3 remap_bbox(const ModelObject &model_object, const PrintConfig &config);

    // ---- Belt floor parameters --------------------------------------------

    struct BeltFloorParams {
        double shear_factor = 0.0;
        int    from_axis    = 1;
        double z_shift      = 0.0;
    };

    // Result of computing belt height + floor params.
    struct BeltHeightResult {
        double          object_height;  // Effective object height after shear/scale
        BeltFloorParams floor_params;
    };

    // Compute effective object height and belt floor parameters from config
    // and pre-remapped bounding box.  original_height is the input height
    // (bb.size().z() or model_object.max_z()).
    static BeltHeightResult compute_belt_height_and_floor(
        const PrintConfig &config, const BoundingBoxf3 &remapped_bbox,
        double original_height);

    // Overload for DynamicPrintConfig (used by static slicing_parameters).
    static BeltHeightResult compute_belt_height_and_floor(
        const DynamicPrintConfig &config, const BoundingBoxf3 &remapped_bbox,
        double original_height);
};

} // namespace Slic3r
