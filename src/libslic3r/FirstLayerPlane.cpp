#include "FirstLayerPlane.hpp"
#include "BeltTransform.hpp"

#include <algorithm>
#include <climits>
#include <cmath>

namespace Slic3r {

namespace {

// Build the row of the gcode-axis-remap matrix R that produces machine_Z,
// AS A FUNCTION OF a slicing-frame point in the GCode generator's coordinate
// space.  Without back-transform this is just R.row(2).  With back-transform
// the writer applies F^-1 before R, so the effective row is (R * F^-1).row(2).
//
// Returns a pair (gradient, constant) such that:
//     machine_Z(p_slicing) = gradient.dot(p_slicing) + constant
struct MachineZAffine {
    Vec3d  gradient = Vec3d::UnitZ();
    double constant = 0.0;
};

MachineZAffine compute_machine_z_affine(const PrintConfig &config)
{
    MachineZAffine out;

    // R is the matrix form of GCodeWriter::apply_axis_remap.  Each output axis
    // i picks one slicing-frame component (with sign + optional Rev mode
    // translation) based on m_remap_{x,y,z}.  We only need row 2 (the z output)
    // since machine_Z is what defines the first-layer plane.
    int rz = int(config.gcode_remap_z.value);
    int axis = rz % 3;
    double sign;
    double trans;
    if (rz < int(RemapAxis::NegX)) {            // 0..2 = PosX/Y/Z
        sign  = 1.0;
        trans = 0.0;
    } else if (rz < int(RemapAxis::RevX)) {     // 3..5 = NegX/Y/Z
        sign  = -1.0;
        trans = 0.0;
    } else {                                     // 6..8 = RevX/Y/Z
        sign = -1.0;
        BoundingBoxf bbox_bed(config.printable_area.values);
        Vec3d vol_max(bbox_bed.max.x(),
                      bbox_bed.max.y(),
                      config.printable_height.value);
        trans = vol_max[axis];
    }

    Vec3d r_row = Vec3d::Zero();
    r_row[axis] = sign;

    // Without back-transform, machine_Z(slicing) = r_row · slicing + trans.
    out.gradient = r_row;
    out.constant = trans;

    if (config.gcode_back_transform.value && config.belt_printer.value) {
        // BeltGCodeWriter applies F^-1 before R when back-transform is on.
        // So machine_Z(slicing) = r_row · (F^-1 · slicing) + trans
        //                       = (r_row^T · F^-1) · slicing + trans
        // We need to compose r_row with F^-1 from the LEFT (treating r_row as
        // a row vector).  Eigen makes this easy: it's just F^-1.transpose() * r_row.
        Transform3d forward = BeltTransformPipeline::build_forward_transform(config);
        Transform3d inverse = forward.inverse();
        // Note: forward.translation() is normally zero (per-print transforms
        // don't add a translation; the per-object z_shift is added separately
        // in PrintObjectSlice).  We still incorporate inverse.translation() in
        // case a Rev-mode preslice_remap puts a translation in F.
        Vec3d composed_grad = inverse.linear().transpose() * r_row;
        double composed_trans =
            r_row.dot(inverse.translation()) + trans;
        out.gradient = composed_grad;
        out.constant = composed_trans;
    }

    return out;
}

} // namespace

FirstLayerPlane::FirstLayerPlane(const PrintConfig &config)
{
    // -------- Resolve Auto -------------------------------------------------
    FirstLayerPlaneMode mode = config.first_layer_plane.value;
    if (mode == FirstLayerPlaneMode::Auto) {
        bool belt_affine_active = config.belt_printer.value &&
            config.belt_slice_rotation.value != BeltRotationAxis::None &&
            std::abs(config.belt_slice_rotation_angle.value) > EPSILON;
        mode = belt_affine_active ? FirstLayerPlaneMode::BeltAffine
                                  : FirstLayerPlaneMode::XY;
    }
    m_mode = mode;

    // -------- Band thickness ----------------------------------------------
    // Note: layer_height lives in PrintObjectConfig, not PrintConfig, so we
    // can't fall back to it from here.  initial_layer_print_height is in
    // PrintConfig and is the right default anyway (the legacy first-layer
    // semantics used initial_layer_print_height, not the regular one).
    double thickness = config.first_layer_plane_thickness.value;
    if (thickness <= 0.0)
        thickness = config.initial_layer_print_height.value;
    if (thickness <= 0.0)
        thickness = 0.2;
    m_thickness_mm = thickness;

    const double user_offset = config.first_layer_plane_offset.value;

    // -------- Build the plane ---------------------------------------------
    auto set_axis_aligned = [&](const Vec3d &n_unit, double offset_along_n) {
        m_normal = n_unit;
        m_offset = offset_along_n;
    };

    switch (mode) {
    case FirstLayerPlaneMode::XY:
        // Legacy XY plane.  Inactive: short-circuit to layer-index path.
        set_axis_aligned(Vec3d::UnitZ(), user_offset);
        m_active = false;
        return;

    case FirstLayerPlaneMode::YZ:
        set_axis_aligned(Vec3d::UnitX(), user_offset);
        m_active = true;
        return;

    case FirstLayerPlaneMode::XZ:
        set_axis_aligned(Vec3d::UnitY(), user_offset);
        m_active = true;
        return;

    case FirstLayerPlaneMode::BeltAffine: {
        // Compute the slicing-frame plane that maps to machine_Z = user_offset
        // under the gcode axis remap (and optional back-transform).
        MachineZAffine mz = compute_machine_z_affine(config);
        double cmag = mz.gradient.norm();
        if (cmag < EPSILON) {
            // Degenerate: slicing point doesn't affect machine_Z.  Fall back.
            set_axis_aligned(Vec3d::UnitZ(), user_offset);
            m_active = false;
            return;
        }
        // Plane equation: gradient · slicing = user_offset - constant
        const double K = user_offset - mz.constant;
        m_normal = mz.gradient / cmag;
        m_offset = K / cmag;
        m_active = true;
        return;
    }

    case FirstLayerPlaneMode::Auto:
        // Should have been resolved above.
        m_active = false;
        return;
    }

    m_active = false;
}

double FirstLayerPlane::distance_from_plane(const Vec3d &point_slicing_mm) const
{
    return m_normal.dot(point_slicing_mm) - m_offset;
}

bool FirstLayerPlane::is_first_layer(const Vec3d &point_slicing_mm,
                                     double first_layer_height_mm) const
{
    if (!m_active)
        return false;
    return distance_from_plane(point_slicing_mm) < first_layer_height_mm;
}

int FirstLayerPlane::effective_layer_index(const Vec3d &point_slicing_mm) const
{
    if (!m_active)
        return INT_MAX / 2;  // Effectively "way past first layer".
    double d = distance_from_plane(point_slicing_mm);
    if (d <= 0.0)
        return 0;
    return int(std::floor(d / m_thickness_mm));
}

int FirstLayerPlane::min_effective_index_for_xy_bbox(
    const BoundingBoxf &xy_bbox_mm, double slicing_z_mm) const
{
    if (!m_active)
        return INT_MAX / 2;
    // For the rectangular bbox in (x, y) at fixed z, the smallest value of
    // (n.x*x + n.y*y + n.z*z - offset) is achieved at one of the four
    // corners, with the smaller component picked when the corresponding
    // normal coefficient is positive.
    const double x_for_min = (m_normal.x() >= 0.0)
        ? xy_bbox_mm.min.x() : xy_bbox_mm.max.x();
    const double y_for_min = (m_normal.y() >= 0.0)
        ? xy_bbox_mm.min.y() : xy_bbox_mm.max.y();
    const double dmin = m_normal.x() * x_for_min
                      + m_normal.y() * y_for_min
                      + m_normal.z() * slicing_z_mm
                      - m_offset;
    if (dmin <= 0.0)
        return 0;
    return int(std::floor(dmin / m_thickness_mm));
}

int FirstLayerPlane::min_effective_index_for_bbox3(
    const BoundingBoxf3 &bbox_mm) const
{
    if (!m_active)
        return INT_MAX / 2;
    const double x_for_min = (m_normal.x() >= 0.0)
        ? bbox_mm.min.x() : bbox_mm.max.x();
    const double y_for_min = (m_normal.y() >= 0.0)
        ? bbox_mm.min.y() : bbox_mm.max.y();
    const double z_for_min = (m_normal.z() >= 0.0)
        ? bbox_mm.min.z() : bbox_mm.max.z();
    const double dmin = m_normal.x() * x_for_min
                      + m_normal.y() * y_for_min
                      + m_normal.z() * z_for_min
                      - m_offset;
    if (dmin <= 0.0)
        return 0;
    return int(std::floor(dmin / m_thickness_mm));
}

} // namespace Slic3r
