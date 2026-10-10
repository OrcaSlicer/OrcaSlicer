#include "BeltTransform.hpp"
#include "Model.hpp"
#include "BoundingBox.hpp"
#include "Config.hpp"
#include "Geometry.hpp"
#include "Point.hpp"
#include "PrintConfig.hpp"
#include "libslic3r.h"

#include <limits>
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace Slic3r {

// ---- Matrix builders ------------------------------------------------------

Matrix3d BeltTransformPipeline::build_rotation_matrix(const PrintConfig &config, bool *has_rot_out)
{
    BeltRotationAxis axis = config.belt_slice_rotation.value;
    double angle_deg = config.belt_slice_rotation_angle.value;
    bool active = axis != BeltRotationAxis::None && std::abs(angle_deg) > EPSILON;
    if (has_rot_out) *has_rot_out = active;
    if (!active)
        return Matrix3d::Identity();
    double angle_rad = Geometry::deg2rad(angle_deg);
    Vec3d unit_axis;
    switch (axis) {
    case BeltRotationAxis::X: unit_axis = Vec3d::UnitX(); break;
    case BeltRotationAxis::Y: unit_axis = Vec3d::UnitY(); break;
    case BeltRotationAxis::Z: unit_axis = Vec3d::UnitZ(); break;
    default:                  return Matrix3d::Identity();
    }
    return Eigen::AngleAxisd(angle_rad, unit_axis).toRotationMatrix();
}

Transform3d BeltTransformPipeline::build_forward_transform(const PrintConfig &config)
{
    // Mesh-side belt transform: the rotation. (Shear & scale are a g-code-side
    // stage, not part of the mesh transform.)
    Transform3d combined = Transform3d::Identity();
    combined.linear() = build_rotation_matrix(config);
    return combined;
}

// ---- Belt floor parameters ------------------------------------------------

namespace {

// Belt floor in the rotated slicer frame: the image of z_machine = 0 under R.
//   R(+α, X): point (·, y, 0) → (·, cos α · y, sin α · y) ⇒ z = tan(α) · y_s
//   R(+α, Y): point (x, ·, 0) → (cos α · x, ·, -sin α · x) ⇒ z = -tan(α) · x_s
//   R(+α, Z): point (·, ·, 0) → (·, ·, 0); no tilt → no floor
void belt_floor_shear(BeltRotationAxis rot_axis, double angle_rad, BeltTransformPipeline::BeltFloorParams &out)
{
    double sin_a = std::sin(angle_rad), cos_a = std::cos(angle_rad);
    switch (rot_axis) {
    case BeltRotationAxis::X:
        out.shear_factor = (std::abs(cos_a) > EPSILON) ?  sin_a / cos_a : 0.;
        out.from_axis    = 1; // Y
        break;
    case BeltRotationAxis::Y:
        out.shear_factor = (std::abs(cos_a) > EPSILON) ? -sin_a / cos_a : 0.;
        out.from_axis    = 0; // X
        break;
    case BeltRotationAxis::Z:
    default:
        out.shear_factor = 0.0;
        out.from_axis    = 1;
        break;
    }
}

// Z of the belt floor directly under a point of the rotated (unshifted) frame.
inline double belt_floor_z(const BeltTransformPipeline::BeltFloorParams &fp, const Vec3d &pt)
{
    return fp.shear_factor * (fp.from_axis == 0 ? pt.x() : pt.y());
}


BeltTransformPipeline::BeltHeightResult compute_belt_height_and_floor_impl(
    const PrintConfig &config, const BoundingBoxf3 &bb, double original_height)
{
    BeltTransformPipeline::BeltHeightResult result;
    result.object_height = original_height;

    // The mesh rotation (the sole mesh-side belt transform).
    const BeltRotationAxis rot_axis  = config.belt_slice_rotation.value;
    const double           rot_angle = config.belt_slice_rotation_angle.value;

    bool has_rotation = rot_axis != BeltRotationAxis::None && std::abs(rot_angle) > EPSILON;
    if (!has_rotation)
        return result;

    // Rotation path: sweep the 8 bbox corners through R to get the rotated height,
    // then derive the belt floor (the image of machine-Z = 0 under R).
    double angle_rad = Geometry::deg2rad(rot_angle);
    Vec3d unit_axis;
    switch (rot_axis) {
    case BeltRotationAxis::X: unit_axis = Vec3d::UnitX(); break;
    case BeltRotationAxis::Y: unit_axis = Vec3d::UnitY(); break;
    case BeltRotationAxis::Z: unit_axis = Vec3d::UnitZ(); break;
    default:                  unit_axis = Vec3d::UnitX(); break;
    }
    Matrix3d R = Eigen::AngleAxisd(angle_rad, unit_axis).toRotationMatrix();
    belt_floor_shear(rot_axis, angle_rad, result.floor_params);
    // The slicing frame starts at the lowest point of the support region: the
    // lowest belt-floor point under the footprint, not the lowest vertex.  The
    // belt under the leading end of an overhang lies below every vertex of the
    // part, and supports have to be able to reach it (see
    // BeltSliceStrategy::apply_preslice_transforms for the exact vertex-scan
    // counterpart of this bbox estimate).
    double min_rz = std::numeric_limits<double>::max();
    double max_rz = std::numeric_limits<double>::lowest();
    for (int i = 0; i < 8; ++i) {
        Vec3d c((i & 1) ? bb.max.x() : bb.min.x(),
                (i & 2) ? bb.max.y() : bb.min.y(),
                (i & 4) ? bb.max.z() : bb.min.z());
        Vec3d  rc = R * c;
        double z  = rc.z();
        min_rz = std::min(min_rz, z);
        max_rz = std::max(max_rz, z);
        min_rz = std::min(min_rz, belt_floor_z(result.floor_params, rc));
    }
    min_rz -= BeltTransformPipeline::frame_margin(result.floor_params);
    result.object_height = max_rz - min_rz;

    result.floor_params.z_shift = bb.min.z() + ((min_rz < 0.) ? -min_rz : 0.);

    return result;
}

} // anonymous namespace

BeltTransformPipeline::BeltHeightResult BeltTransformPipeline::compute_belt_height_and_floor(
    const PrintConfig &config, const BoundingBoxf3 &bbox, double original_height)
{
    return compute_belt_height_and_floor_impl(config, bbox, original_height);
}

bool BeltTransformPipeline::floor_shear(const PrintConfig &config, BeltFloorParams &out)
{
    out = BeltFloorParams{};
    const BeltRotationAxis rot_axis  = config.belt_slice_rotation.value;
    const double           rot_angle = config.belt_slice_rotation_angle.value;
    if (rot_axis == BeltRotationAxis::None || std::abs(rot_angle) <= EPSILON)
        return false;
    belt_floor_shear(rot_axis, Geometry::deg2rad(rot_angle), out);
    return std::abs(out.shear_factor) > EPSILON;
}

} // namespace Slic3r
