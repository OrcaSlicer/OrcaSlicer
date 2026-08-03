#include "BeltGCodeWriter.hpp"
#include "FirstLayerPlane.hpp"
#include "Geometry.hpp"
#include <boost/log/trivial.hpp>

namespace Slic3r {

namespace {

// Decide whether a particular destination point gets first-layer treatment.
// When the plane evaluator is active, distance from the plane wins; otherwise
// fall back to the layer-coarse m_is_first_layer flag set by the caller.
inline bool belt_point_on_first_layer(
    const FirstLayerPlane *plane,
    double                 first_layer_thickness_mm,
    bool                   layer_first_flag,
    const Vec3d           &point_slicing_mm)
{
    if (plane && plane->is_active())
        return plane->is_first_layer(point_slicing_mm, first_layer_thickness_mm);
    return layer_first_flag;
}

} // namespace

// ---- Belt configuration ---------------------------------------------------

void BeltGCodeWriter::set_belt_back_transform(const PrintConfig &config)
{
    m_belt_back_transform.init_from_config(config);
}

void BeltGCodeWriter::set_machine_frame_transform(const PrintConfig &config)
{
    m_machine_frame_transform.init_from_config(config);
}

Vec3d BeltGCodeWriter::to_machine_coords(const Vec3d &pos) const
{
    // Step 1+2: To Cartesian (back_transform + axis_remap).
    // In world-coordinates mode (PA line / PA pattern calibration) the input
    // already describes a point relative to the belt surface, so the
    // slicer->world back-transform is skipped and only the machine kinematics
    // (axis remap + frame shear/scale) are applied.
    Vec3d after_back = m_world_coordinates ? pos : m_belt_back_transform.apply(pos);
    Vec3d result     = apply_axis_remap(after_back);
    Vec3d after_remap = result;
    // Step 3: Machine-frame transform (belt frame tilt) applied LAST so it acts
    // as a global linear transform on the placed coords.
    Vec3d final = m_machine_frame_transform.apply(result);

    // [BELT-DEBUG] One-shot log per layer transition (i.e. when the input Z
    // crosses an integer mm boundary) to keep the log volume manageable while
    // still capturing one sample per ~5 layers.  Shows the full pipeline so
    // Case A vs Case B can be compared step-by-step.
    static thread_local int s_last_logged_z = std::numeric_limits<int>::min();
    int z_bucket = static_cast<int>(std::floor(pos.z() * 5.0));  // every 0.2mm
    if (z_bucket != s_last_logged_z) {
        s_last_logged_z = z_bucket;
        BOOST_LOG_TRIVIAL(trace) << "[BELT-DEBUG] to_machine_coords"
            << " slicer_in=(" << pos.x() << "," << pos.y() << "," << pos.z() << ")"
            << " after_back=(" << after_back.x() << "," << after_back.y() << "," << after_back.z() << ")"
            << " after_remap=(" << after_remap.x() << "," << after_remap.y() << "," << after_remap.z() << ")"
            << " final=(" << final.x() << "," << final.y() << "," << final.z() << ")"
            << " mft_active=" << m_machine_frame_transform.is_active()
            << " back_active=" << m_belt_back_transform.is_active();
    }
    return final;
}

// ---- Overridden movement methods ------------------------------------------

std::string BeltGCodeWriter::travel_to_xy(const Vec2d &point, const std::string &comment)
{
    m_pos(0) = point(0);
    m_pos(1) = point(1);

    this->set_current_position_clear(true);
    Vec2d point_on_plate = { point(0) - m_x_offset, point(1) - m_y_offset };

    // Belt printer: transform to machine coordinates (XY travel also needs Z due to YZ rotation)
    Vec3d machine = to_machine_coords(Vec3d(point_on_plate.x(), point_on_plate.y(), m_pos.z()));

    GCodeG1Formatter w;
    w.emit_xyz(machine);
    const bool first_layer_for_point = belt_point_on_first_layer(
        m_first_layer_plane, m_first_layer_thickness_mm, m_is_first_layer,
        Vec3d(point.x(), point.y(), m_pos.z()));
    auto speed = first_layer_for_point
        ? this->config.get_abs_value_at("initial_layer_travel_speed", m_cached_extruder_idx)
        : this->config.travel_speed.get_at(m_cached_extruder_idx);
    w.emit_f(speed * 60.0);
    w.emit_comment(GCodeWriter::full_gcode_comment, comment);
    return w.string();
}

std::string BeltGCodeWriter::lazy_lift(LiftType lift_type, bool spiral_vase)
{
    // Belt printer: force NormalLift since SpiralLift and SlopeLift compute
    // slope angles that don't account for the YZ coordinate rotation.
    return GCodeWriter::lazy_lift(LiftType::NormalLift, spiral_vase);
}

std::string BeltGCodeWriter::eager_lift(const LiftType type)
{
    // Belt printer: force NormalLift (SpiralLift/SlopeLift don't account for YZ rotation).
    return GCodeWriter::eager_lift(LiftType::NormalLift);
}

std::string BeltGCodeWriter::_travel_to_z(double z, const std::string &comment)
{
    m_pos(2) = z;

    double speed = this->config.travel_speed_z.get_at(m_cached_extruder_idx);
    if (speed == 0.) {
        const bool first_layer_for_point = belt_point_on_first_layer(
            m_first_layer_plane, m_first_layer_thickness_mm, m_is_first_layer,
            Vec3d(m_pos.x(), m_pos.y(), z));
        speed = first_layer_for_point ? this->config.get_abs_value_at("initial_layer_travel_speed", m_cached_extruder_idx)
                                      : this->config.travel_speed.get_at(m_cached_extruder_idx);
    }

    // Belt printer: a Z-only move in slicing frame needs to emit both Y and Z in machine coords.
    Vec3d machine = to_machine_coords(Vec3d(m_pos.x() - m_x_offset, m_pos.y() - m_y_offset, z));

    GCodeG1Formatter w;
    w.emit_xyz(machine);
    w.emit_f(speed * 60.0);
    w.emit_comment(GCodeWriter::full_gcode_comment, comment);
    return w.string();
}

std::string BeltGCodeWriter::extrude_to_xy(const Vec2d &point, double dE, const std::string &comment, bool force_no_extrusion)
{
    m_pos(0) = point(0);
    m_pos(1) = point(1);
    if (std::abs(dE) <= std::numeric_limits<double>::epsilon())
        force_no_extrusion = true;

    if (!force_no_extrusion)
        filament()->extrude(dE);

    Vec2d point_on_plate = { point(0) - m_x_offset, point(1) - m_y_offset };

    // Belt printer: transform and emit XYZ (Y and Z are coupled)
    Vec3d machine = to_machine_coords(Vec3d(point_on_plate.x(), point_on_plate.y(), m_pos.z()));

    GCodeG1Formatter w;
    w.emit_xyz(machine);
    if (!force_no_extrusion)
        w.emit_e(filament()->E());
    w.emit_comment(GCodeWriter::full_gcode_comment, comment);
    return w.string();
}

std::string BeltGCodeWriter::extrude_to_xyz(const Vec3d &point, double dE, const std::string &comment, bool force_no_extrusion)
{
    m_pos = point;
    m_lifted = 0;
    if (!force_no_extrusion)
        filament()->extrude(dE);

    Vec3d point_on_plate = { point(0) - m_x_offset, point(1) - m_y_offset, point(2) };
    point_on_plate = to_machine_coords(point_on_plate);

    GCodeG1Formatter w;
    w.emit_xyz(point_on_plate);
    if (!force_no_extrusion)
        w.emit_e(filament()->E());
    w.emit_comment(GCodeWriter::full_gcode_comment, comment);
    return w.string();
}

std::string BeltGCodeWriter::travel_to_xyz(const Vec3d &point, const std::string &comment, bool force_z)
{
    // Belt-specific override of travel_to_xyz.
    // Key differences from base:
    // 1. All coordinates go through to_machine_coords()
    // 2. Always emit full XYZ (can't split XY and Z due to coupling)
    // 3. Lift type forced to NormalLift (handled by lazy_lift/eager_lift overrides)

    Vec3d dest_point = point;
    const bool first_layer_for_point = belt_point_on_first_layer(
        m_first_layer_plane, m_first_layer_thickness_mm, m_is_first_layer, point);
    auto travel_speed =
        first_layer_for_point ? this->config.get_abs_value_at("initial_layer_travel_speed", m_cached_extruder_idx)
                              : this->config.travel_speed.get_at(m_cached_extruder_idx);

    // Handle pending z_hop
    if (std::abs(m_to_lift) > EPSILON) {
        assert(std::abs(m_lifted) < EPSILON);
        if ((!this->is_current_position_clear() || m_pos != dest_point) &&
            m_to_lift + m_pos(2) > point(2)) {
            m_lifted = m_to_lift + m_pos(2) - point(2);
            dest_point(2) = m_to_lift + m_pos(2);
        }
        m_to_lift = 0.;

        std::string slop_move;
        Vec3d source = { m_pos(0) - m_x_offset, m_pos(1) - m_y_offset, m_pos(2) };
        Vec3d target = { dest_point(0) - m_x_offset, dest_point(1) - m_y_offset, dest_point(2) };
        Vec3d delta = target - source;
        Vec2d delta_no_z = { delta(0), delta(1) };

        if (delta(2) > 0 && delta_no_z.norm() != 0.0f) {
            // Belt: SpiralLift and SlopeLift are disabled (lazy_lift forces NormalLift),
            // but handle NormalLift and fallthrough.
            if (m_to_lift_type == LiftType::SlopeLift &&
                this->is_current_position_clear() &&
                atan2(delta(2), delta_no_z.norm()) < this->filament()->travel_slope()) {
                Vec2d temp = delta_no_z.normalized() * delta(2) / tan(this->filament()->travel_slope());
                Vec3d slope_top_point = Vec3d(temp(0), temp(1), delta(2)) + source;
                slope_top_point = to_machine_coords(slope_top_point);
                GCodeG1Formatter w0;
                w0.emit_xyz(slope_top_point);
                w0.emit_f(travel_speed * 60.0);
                w0.emit_comment(GCodeWriter::full_gcode_comment, comment);
                slop_move = w0.string();
            }
            else if (m_to_lift_type == LiftType::NormalLift && this->is_current_position_clear()) {
                // Only lift-in-place when the current position is known. On a normal
                // printer _travel_to_z emits a Z-only move, but in belt mode Z is coupled
                // to Y/X, so _travel_to_z re-emits the current m_pos through the belt
                // shear. At print start (and after custom gcode) m_pos.xy is still the
                // uninitialised origin (0,0), which shears into a bogus machine point
                // (e.g. X=bed_max, Y=layer_z) far up the gantry. Skipping the separate
                // lift here is safe: there is nothing to lift over yet, and the
                // xy_z_move below travels straight to the destination with full XYZ,
                // establishing the correct position. This mirrors the SlopeLift branch
                // above, which already guards on is_current_position_clear().
                slop_move = _travel_to_z(target.z(), "normal lift Z");
            }
        }

        std::string xy_z_move;
        {
            Vec3d emit_target = to_machine_coords(target);
            GCodeG1Formatter w0;
            // Belt mode: always emit full XYZ since Y and Z are coupled
            w0.emit_xyz(emit_target);
            w0.emit_f(travel_speed * 60.0);
            w0.emit_comment(GCodeWriter::full_gcode_comment, comment);
            xy_z_move = w0.string();
        }
        m_pos = dest_point;
        this->set_current_position_clear(true);
        return slop_move + xy_z_move;
    }
    else if (!force_z && !this->will_move_z(point(2))) {
        double nominal_z = m_pos(2) - m_lifted;
        m_lifted -= (point(2) - nominal_z);
        if (std::abs(m_lifted) < EPSILON)
            m_lifted = 0.;
        this->set_current_position_clear(true);
        return this->travel_to_xy(to_2d(point));
    }
    else {
        m_lifted = 0;
    }

    Vec3d point_on_plate = { dest_point(0) - m_x_offset, dest_point(1) - m_y_offset, dest_point(2) };
    point_on_plate = to_machine_coords(point_on_plate);

    // Belt mode: always emit full XYZ
    GCodeG1Formatter w;
    w.emit_xyz(point_on_plate);
    w.emit_f(this->config.travel_speed.get_at(m_cached_extruder_idx) * 60.0);
    w.emit_comment(GCodeWriter::full_gcode_comment, comment);

    m_pos = dest_point;
    this->set_current_position_clear(true);
    return w.string();
}

} // namespace Slic3r
