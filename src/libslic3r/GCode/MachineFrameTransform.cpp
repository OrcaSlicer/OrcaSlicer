#include "MachineFrameTransform.hpp"
#include "../Geometry.hpp"

#include <cmath>

namespace Slic3r {

bool MachineFrameTransform::init_from_config(const PrintConfig &config)
{
    m_active            = false;
    m_transform         = Transform3d::Identity();
    m_transform_inverse = Transform3d::Identity();

    if (!config.belt_printer.value)
        return false;

    // The machine-frame transform is derived from the single belt tilt (axis +
    // angle) that also drives the pre-slice mesh rotation.  Expert decouple lets
    // the machine-frame angle differ from the slicing rotation; otherwise both
    // use belt_slice_rotation_angle.
    const BeltRotationAxis axis = config.belt_slice_rotation.value;
    if (axis == BeltRotationAxis::None || axis == BeltRotationAxis::Z)
        return false; // Z is an in-plane spin: no machine-frame tilt.

    const double angle_deg = config.belt_frame_tilt_decouple.value
        ? config.belt_frame_tilt_angle.value
        : config.belt_slice_rotation_angle.value;
    if (std::abs(angle_deg) <= EPSILON)
        return false;

    const double angle_rad = Geometry::deg2rad(angle_deg);
    const double sin_a      = std::sin(angle_rad);
    if (std::abs(sin_a) <= EPSILON)
        return false;
    const double cot_a   = std::cos(angle_rad) / sin_a;
    const double inv_sin = 1.0 / std::abs(sin_a);

    // This stage runs after the conventional belt axis swap. For an X-axis
    // slicing rotation, remapped Y is model height and remapped Z is travel
    // along the belt. Convert those Cartesian coordinates to machine axes with
    // the established belt-printer convention:
    //   machine gantry = model height / sin(a)
    //   machine belt   = model belt + model height * cot(a)
    // The Y-rotation case is the same mapping on X/Z, with the rotation sign.
    // At 45 degrees tan/cot and sin/cos are equal, which previously hid the
    // incorrect complementary-angle formulas used by this unified transform.
    Matrix3d shear = Matrix3d::Identity();
    Matrix3d scale = Matrix3d::Identity();
    if (axis == BeltRotationAxis::X) {
        shear(2, 1) =  cot_a;   // Z from Y
        scale(1, 1) =  inv_sin; // Y
    } else { // BeltRotationAxis::Y
        shear(2, 0) = -cot_a;   // Z from X
        scale(0, 0) =  inv_sin; // X
    }

    // Apply shear first, then scale (the historical default ShearThenScale order:
    // result = scale * shear * p).  For the canonical 45°/X belt this maps
    // (x,y,z) -> (x, y/sin, y + z), matching the previous per-axis config.
    Transform3d combined = Transform3d::Identity();
    combined.linear() = scale * shear;

    if (combined.isApprox(Transform3d::Identity()))
        return false;

    m_transform         = combined;
    m_transform_inverse = combined.inverse();
    m_active            = true;
    return true;
}

Vec3d MachineFrameTransform::apply(const Vec3d &pos) const
{
    if (!m_active)
        return pos;
    return m_transform * pos;
}

Vec3d MachineFrameTransform::apply_inverse(const Vec3d &pos) const
{
    if (!m_active)
        return pos;
    return m_transform_inverse * pos;
}

} // namespace Slic3r
