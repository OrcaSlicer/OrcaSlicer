#include "BeltTransform.hpp"
#include "Model.hpp"

#include <limits>

namespace Slic3r {

// ---- Matrix builders ------------------------------------------------------

Transform3d BeltTransformPipeline::build_preslice_remap(const PrintConfig &config)
{
    Transform3d pre_remap = Transform3d::Identity();
    if (!has_preslice_remap(config))
        return pre_remap;

    int pre_rx = int(config.preslice_remap_x.value);
    int pre_ry = int(config.preslice_remap_y.value);
    int pre_rz = int(config.preslice_remap_z.value);

    // Each remap value selects a source axis and sign.
    auto remap_column = [](int r) -> Vec3d {
        int axis = r % 3;
        Vec3d col = Vec3d::Zero();
        if (r < 3)      col[axis] =  1.0;  // +axis
        else if (r < 6) col[axis] = -1.0;  // -axis
        else            col[axis] = -1.0;  // Rev: max - pos = -(pos - max)
        return col;
    };

    Matrix3d remap_lin;
    remap_lin.col(0) = remap_column(pre_rx);
    remap_lin.col(1) = remap_column(pre_ry);
    remap_lin.col(2) = remap_column(pre_rz);
    pre_remap.linear() = remap_lin;

    // Translation for Rev modes (needs build volume extents).
    if (pre_rx >= 6 || pre_ry >= 6 || pre_rz >= 6) {
        BoundingBoxf bbox_bed(config.printable_area.values);
        Vec3d vol_max(bbox_bed.max.x(), bbox_bed.max.y(),
                      config.printable_height.value);
        Vec3d remap_trans = Vec3d::Zero();
        auto add_rev = [&](int r, int out) {
            if (r >= 6) remap_trans[out] = vol_max[r % 3];
        };
        add_rev(pre_rx, 0);
        add_rev(pre_ry, 1);
        add_rev(pre_rz, 2);
        pre_remap.translation() = remap_trans;
    }

    return pre_remap;
}

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
    // Mesh-side belt transform: rotation applied after the pre-slice axis remap.
    // (Shear & scale are a g-code-side stage, not part of the mesh transform.)
    Transform3d pre_remap = build_preslice_remap(config);
    Matrix3d    rot       = build_rotation_matrix(config);

    Transform3d combined = Transform3d::Identity();
    combined.linear() = rot;
    combined = combined * pre_remap;
    return combined;
}

// ---- Bounding box remap ---------------------------------------------------

BoundingBoxf3 BeltTransformPipeline::remap_bbox(const BoundingBoxf3 &bb, const PrintConfig &config)
{
    int pre_rx = int(config.preslice_remap_x.value);
    int pre_ry = int(config.preslice_remap_y.value);
    int pre_rz = int(config.preslice_remap_z.value);

    if (pre_rx == int(RemapAxis::PosX) &&
        pre_ry == int(RemapAxis::PosY) &&
        pre_rz == int(RemapAxis::PosZ))
        return bb;  // Identity remap.

    auto remap_coord = [](int r, const Vec3d &v) -> double {
        int axis = r % 3;
        if (r < 3) return v[axis];
        return -v[axis];
    };

    Vec3d mn = bb.min.cast<double>(), mx = bb.max.cast<double>();
    BoundingBoxf3 rbb;
    for (int i = 0; i < 8; ++i) {
        Vec3d c((i & 1) ? mx.x() : mn.x(),
                (i & 2) ? mx.y() : mn.y(),
                (i & 4) ? mx.z() : mn.z());
        Vec3d rc(remap_coord(pre_rx, c), remap_coord(pre_ry, c), remap_coord(pre_rz, c));
        if (i == 0) rbb = BoundingBoxf3(rc, rc);
        else rbb.merge(rc);
    }
    return rbb;
}

BoundingBoxf3 BeltTransformPipeline::remap_bbox(const ModelObject &model_object, const PrintConfig &config)
{
    return remap_bbox(model_object.raw_bounding_box(), config);
}

// ---- Belt floor parameters ------------------------------------------------

// Shared implementation for both PrintConfig and DynamicPrintConfig.
// Template avoids duplicating the math for the two config types.
namespace {

template<typename Config>
BeltTransformPipeline::BeltHeightResult compute_belt_height_and_floor_impl(
    const Config &config, const BoundingBoxf3 &bb, double original_height)
{
    BeltTransformPipeline::BeltHeightResult result;
    result.object_height = original_height;

    // Extract the mesh rotation from config (the sole mesh-side belt transform).
    BeltRotationAxis rot_axis;
    double           rot_angle;

    if constexpr (std::is_same_v<Config, PrintConfig>) {
        rot_axis  = config.belt_slice_rotation.value;
        rot_angle = config.belt_slice_rotation_angle.value;
    } else {
        // DynamicPrintConfig path
        auto get_float = [&](const char *key) {
            auto *opt = config.template option<ConfigOptionFloat>(key);
            return opt ? opt->value : 0.0;
        };
        auto get_rot_axis = [&](const char *key) {
            auto *opt = config.template option<ConfigOptionEnum<BeltRotationAxis>>(key);
            return opt ? opt->value : BeltRotationAxis::None;
        };
        rot_axis  = get_rot_axis("belt_slice_rotation");
        rot_angle = get_float("belt_slice_rotation_angle");
    }

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
    double min_rz = std::numeric_limits<double>::max();
    double max_rz = std::numeric_limits<double>::lowest();
    for (int i = 0; i < 8; ++i) {
        Vec3d c((i & 1) ? bb.max.x() : bb.min.x(),
                (i & 2) ? bb.max.y() : bb.min.y(),
                (i & 4) ? bb.max.z() : bb.min.z());
        double z = (R * c).z();
        min_rz = std::min(min_rz, z);
        max_rz = std::max(max_rz, z);
    }
    result.object_height = max_rz - min_rz;

    // Belt floor in slicer-frame is the image of z_machine = 0 under R.
    //   R(+α, X): point (·, y, 0) → (·, cos α · y, sin α · y) ⇒ z = tan(α) · y_s
    //   R(+α, Y): point (x, ·, 0) → (cos α · x, ·, -sin α · x) ⇒ z = -tan(α) · x_s
    //   R(+α, Z): point (·, ·, 0) → (·, ·, 0); no tilt → no floor
    double sin_a = std::sin(angle_rad), cos_a = std::cos(angle_rad);
    switch (rot_axis) {
    case BeltRotationAxis::X:
        result.floor_params.shear_factor = (std::abs(cos_a) > EPSILON) ?  sin_a / cos_a : 0.;
        result.floor_params.from_axis    = 1; // Y
        break;
    case BeltRotationAxis::Y:
        result.floor_params.shear_factor = (std::abs(cos_a) > EPSILON) ? -sin_a / cos_a : 0.;
        result.floor_params.from_axis    = 0; // X
        break;
    case BeltRotationAxis::Z:
    default:
        result.floor_params.shear_factor = 0.0;
        result.floor_params.from_axis    = 1;
        break;
    }
    result.floor_params.z_shift = bb.min.z() + ((min_rz < 0.) ? -min_rz : 0.);

    return result;
}

} // anonymous namespace

BeltTransformPipeline::BeltHeightResult BeltTransformPipeline::compute_belt_height_and_floor(
    const PrintConfig &config, const BoundingBoxf3 &remapped_bbox, double original_height)
{
    return compute_belt_height_and_floor_impl(config, remapped_bbox, original_height);
}

BeltTransformPipeline::BeltHeightResult BeltTransformPipeline::compute_belt_height_and_floor(
    const DynamicPrintConfig &config, const BoundingBoxf3 &remapped_bbox, double original_height)
{
    return compute_belt_height_and_floor_impl(config, remapped_bbox, original_height);
}

} // namespace Slic3r
