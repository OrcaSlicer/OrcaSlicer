#include "BeltGCode.hpp"
#include "BeltGCodeWriter.hpp"
#include "BeltTransform.hpp"
#include "Print.hpp"

namespace Slic3r {

void BeltGCode::init_belt_writer(Print &print, bool is_bbl_printers)
{
    if (!print.config().belt_printer.value)
        return;

    auto belt_writer = std::make_unique<BeltGCodeWriter>();
    belt_writer->set_is_bbl_machine(is_bbl_printers);
    // Axis remap and build volume max are set by base GCode after init_belt_writer returns.
    belt_writer->set_belt_back_transform(print.config());
    belt_writer->set_machine_frame_transform(print.config());
    m_writer = std::move(belt_writer);
}

void BeltGCode::write_belt_header(GCodeOutputStream &file, const Print &print)
{
    if (!print.config().belt_printer.value)
        return;

    const auto &full_cfg = print.full_print_config();
    // Slicing rotation: the belt tilt (axis + angle) and the single source of truth
    // for the physical tilt the G-code viewer uses to enable belt view.
    file.write_format("; belt_slice_rotation = %s\n", full_cfg.opt_serialize("belt_slice_rotation").c_str());
    file.write_format("; belt_slice_rotation_angle = %.1f\n", print.config().belt_slice_rotation_angle.value);
    file.write_format("; belt_slice_rotation_global = %d\n", print.config().belt_slice_rotation_global.value ? 1 : 0);
    // Pre-slice remap configs
    file.write_format("; preslice_remap_x = %s\n", full_cfg.opt_serialize("preslice_remap_x").c_str());
    file.write_format("; preslice_remap_y = %s\n", full_cfg.opt_serialize("preslice_remap_y").c_str());
    file.write_format("; preslice_remap_z = %s\n", full_cfg.opt_serialize("preslice_remap_z").c_str());
    file.write_format("; preslice_remap_global = %d\n", print.config().preslice_remap_global.value ? 1 : 0);
    file.write_format("; belt_preslice_global = %d\n", print.config().belt_preslice_global.value ? 1 : 0);
    // Machine-frame transform: shear (tan) + scale (1/cos) derived from the belt
    // tilt angle (or belt_frame_tilt_angle when decoupled).
    file.write_format("; belt_frame_tilt_decouple = %d\n", print.config().belt_frame_tilt_decouple.value ? 1 : 0);
    file.write_format("; belt_frame_tilt_angle = %.1f\n", print.config().belt_frame_tilt_angle.value);
}

void BeltGCode::on_set_origin(const PrintObject * /*obj*/, const Point & /*inst_shift*/)
{
    // Global pre-slice mode: adjust origin using computed correction.
    // Transform the origin through the belt pipeline so that
    // back_transform(T * origin) = origin (correct machine position).
    //
    // Flags that trigger this path:
    //   belt_preslice_global         — full pipeline (rotation * remap) is global
    //   preslice_remap_global        — only the pre-slice remap is global
    //   belt_slice_rotation_global   — slicing rotation treated as global (matches
    //                                  the per-instance Z-offset added in PrintObjectSlice.cpp)
    // The XY origin adjustment uses the FULL forward transform, because the
    // back_transform applied during G-code emission is always the inverse of
    // the full pipeline.
    bool use_global = m_config.belt_preslice_global.value
        || (m_config.preslice_remap_global.value
            && BeltTransformPipeline::has_preslice_remap(m_config))
        || (m_config.belt_slice_rotation_global.value
            && m_config.belt_slice_rotation.value != BeltRotationAxis::None
            && std::abs(m_config.belt_slice_rotation_angle.value) > EPSILON);
    if (!use_global || !m_config.belt_printer.value)
        return;

    // Adjust origin: transform through belt forward pipeline so that
    // the back-transform correctly recovers model-space positions.
    Transform3d T = BeltTransformPipeline::build_forward_transform(m_config);
    Vec2d cur_origin = this->origin();
    Vec3d origin3d(cur_origin.x(), cur_origin.y(), 0.);
    Vec3d adjusted = T.linear() * origin3d;
    this->set_origin(Vec2d(adjusted.x(), adjusted.y()));
}

} // namespace Slic3r
