#include "GCodeWriter.hpp"
#include "CustomGCode.hpp"
#include "PrintConfig.hpp"
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <map>
#include <assert.h>
#include <GCode/GCodeProcessor.hpp>

#ifdef __APPLE__
    #include <boost/spirit/include/karma.hpp>
#endif

#define FLAVOR_IS(val) this->config.gcode_flavor == val
#define FLAVOR_IS_NOT(val) this->config.gcode_flavor != val

namespace Slic3r {

bool GCodeWriter::full_gcode_comment = true;

bool GCodeWriter::supports_separate_travel_acceleration(GCodeFlavor flavor)
{
    return (flavor == gcfRepetier || flavor == gcfMarlinFirmware ||  flavor == gcfRepRapFirmware);
}

void GCodeWriter::apply_print_config(const PrintConfig &print_config)
{
    this->config.apply(print_config, true);
    m_single_extruder_multi_material = print_config.single_extruder_multi_material.value;
    bool use_mach_limits = print_config.gcode_flavor.value == gcfMarlinLegacy || print_config.gcode_flavor.value == gcfMarlinFirmware ||
                           print_config.gcode_flavor.value == gcfKlipper || print_config.gcode_flavor.value == gcfRepRapFirmware;
    m_max_acceleration = std::lrint(use_mach_limits ? print_config.machine_max_acceleration_extruding.values.front() : 0);
    m_max_travel_acceleration = static_cast<unsigned int>(
        std::round((use_mach_limits && supports_separate_travel_acceleration(print_config.gcode_flavor.value)) ?
                       print_config.machine_max_acceleration_travel.values.front() :
                       0));
    if (use_mach_limits) {
        m_max_jerk_x  = std::lrint(print_config.machine_max_jerk_x.values.front());
        m_max_jerk_y  = std::lrint(print_config.machine_max_jerk_y.values.front());
        m_max_junction_deviation  = (print_config.machine_max_junction_deviation.values.front());
    };
    m_max_jerk_z = print_config.machine_max_jerk_z.values.front();
    m_max_jerk_e = print_config.machine_max_jerk_e.values.front();
    m_resolution = print_config.resolution.value;
}

void GCodeWriter::set_extruders(std::vector<unsigned int> extruder_ids)
{
    std::sort(extruder_ids.begin(), extruder_ids.end());
    m_filament_extruders.clear();
    m_filament_extruders.reserve(extruder_ids.size());
    for (unsigned int extruder_id : extruder_ids)
        m_filament_extruders.emplace_back(Extruder(extruder_id, &this->config, config.single_extruder_multi_material.value));

    /*  we enable support for multiple extruder if any extruder greater than 0 is used
        (even if prints only uses that one) since we need to output Tx commands
        first extruder has index 0 */
    this->multiple_extruders = (*std::max_element(extruder_ids.begin(), extruder_ids.end())) > 0;
}

std::string GCodeWriter::preamble()
{
    std::ostringstream gcode;

    if (FLAVOR_IS_NOT(gcfMakerWare)) {
        gcode << "G90\n";
        gcode << "G21\n";
    }
    if (FLAVOR_IS(gcfRepRapSprinter) ||
        FLAVOR_IS(gcfRepRapFirmware) ||
        FLAVOR_IS(gcfMarlinLegacy) ||
        FLAVOR_IS(gcfMarlinFirmware) ||
        FLAVOR_IS(gcfTeacup) ||
        FLAVOR_IS(gcfRepetier) ||
        FLAVOR_IS(gcfSmoothie) ||
        FLAVOR_IS(gcfKlipper))
    {
        if (this->config.use_relative_e_distances) {
            gcode << "M83 ; use relative distances for extrusion\n";
        } else {
            gcode << "M82 ; use absolute distances for extrusion\n";
        }
        gcode << this->reset_e(true);
    }

    return gcode.str();
}

std::string GCodeWriter::postamble() const
{
    std::ostringstream gcode;
    if (FLAVOR_IS(gcfMachinekit))
          gcode << "M2 ; end of program\n";
    return gcode.str();
}

std::string GCodeWriter::set_temperature(unsigned int temperature, GCodeFlavor flavor, bool wait, int tool, std::string comment){
    if (wait && (flavor == gcfMakerWare || flavor == gcfSailfish))
        return "";

    std::string code;
    if (wait && flavor != gcfTeacup && flavor != gcfRepRapFirmware) {
        code    = "M109";
        if(comment.empty())
            comment = "set nozzle temperature and wait for it to be reached";
    } else {
        if (flavor == gcfRepRapFirmware) { // M104 is deprecated on RepRapFirmware
            code = "G10";
        } else {
            code = "M104";
        }
        if(comment.empty())
            comment = "set nozzle temperature";
    }

    std::ostringstream gcode;
    gcode << code << " ";
    if (flavor == gcfMach3 || flavor == gcfMachinekit) {
        gcode << "P";
    } else {
        gcode << "S";
    }
    gcode << temperature;
    if (tool != -1) {
        if (flavor == gcfRepRapFirmware) {
            gcode << " P" << tool;
        } else {
            gcode << " T" << tool;
        }
    }
    gcode << " ; " << comment << "\n";

    if ((flavor == gcfTeacup || flavor == gcfRepRapFirmware) && wait)
        gcode << "M116 ; wait for temperature to be reached\n";

    return gcode.str();
}

std::string GCodeWriter::set_temperature(unsigned int temperature, bool wait, int tool) const
{
    // set tool to -1 to make sure we won't emit T parameter for single extruder or SEMM
    if (!this->multiple_extruders || m_single_extruder_multi_material)
        tool = -1;
    return set_temperature(temperature, this->config.gcode_flavor, wait, tool);
}

// BBS
std::string GCodeWriter::set_bed_temperature(int temperature, bool wait)
{
    if (temperature == m_last_bed_temperature && (! wait || m_last_bed_temperature_reached))
        return std::string();

    m_last_bed_temperature = temperature;
    m_last_bed_temperature_reached = wait;

    std::string code, comment;
    std::ostringstream gcode;

    if (wait) {
        code = "M190";
        comment = "set bed temperature and wait for it to be reached";
    }
    else {
        code = "M140";
        comment = "set bed temperature";
    }

    gcode << code << " S" << temperature << " ; " << comment << "\n";
    return gcode.str();
}

std::string GCodeWriter::set_chamber_temperature(int temperature, bool wait)
{
    std::string code, comment;
    std::ostringstream gcode;

    if (wait)
    {
        // Orca: should we let the M191 command to turn on the auxiliary fan?
        if (config.auxiliary_fan)
            gcode << "M106 P2 S255 \n";
        gcode << "M191 S" << std::to_string(temperature) << " ;"
              << "set chamber_temperature and wait for it to be reached\n";
        if (config.auxiliary_fan)
            gcode << "M106 P2 S0 \n";
    }
    else {
        code = "M141";
        comment = "set chamber_temperature";
        gcode << code << " S" << temperature << ";" << comment << "\n";
    }
    return gcode.str();
}

// copied from PrusaSlicer
std::string GCodeWriter::set_acceleration_internal(Acceleration type, unsigned int acceleration)
{
    // Clamp the acceleration to the allowed maximum.
    if (type == Acceleration::Print && m_max_acceleration > 0 && acceleration > m_max_acceleration)
        acceleration = m_max_acceleration;
    if (type == Acceleration::Travel && m_max_travel_acceleration > 0 && acceleration > m_max_travel_acceleration)
        acceleration = m_max_travel_acceleration;

    // Are we setting travel acceleration for a flavour that supports separate travel and print acc?
    bool separate_travel = (type == Acceleration::Travel && supports_separate_travel_acceleration(this->config.gcode_flavor));

    auto& last_value = separate_travel ? m_last_travel_acceleration : m_last_acceleration ;
    if (acceleration == 0 || acceleration == last_value)
        return std::string();

    last_value = acceleration;

    std::ostringstream gcode;
    if (FLAVOR_IS(gcfRepetier))
        gcode << (separate_travel ? "M202 X" : "M201 X") << acceleration << " Y" << acceleration;
    else if (FLAVOR_IS(gcfRepRapFirmware) || FLAVOR_IS(gcfMarlinFirmware))
        gcode << (separate_travel ? "M204 T" : "M204 P") << acceleration;
    else if (FLAVOR_IS(gcfKlipper)) {
        gcode << "SET_VELOCITY_LIMIT ACCEL=" << acceleration;
        if (this->config.accel_to_decel_enable) {
            gcode << " ACCEL_TO_DECEL=" << acceleration * this->config.accel_to_decel_factor / 100;
            if (GCodeWriter::full_gcode_comment)
                gcode << " ; adjust ACCEL_TO_DECEL";
        }
    }
    else
        gcode << "M204 S" << acceleration;

    if (GCodeWriter::full_gcode_comment) gcode << " ; adjust acceleration";
    gcode << "\n";

    return gcode.str();
}

std::string GCodeWriter::set_jerk_xy(double jerk)
{
    if (jerk < 0.01 || is_approx(jerk, m_last_jerk))
        return std::string();
    
    m_last_jerk = jerk;

    std::ostringstream gcode;
    if (FLAVOR_IS(gcfKlipper)) {
        // Clamp the jerk to the allowed maximum.
        if (m_max_jerk_x > 0 && jerk > m_max_jerk_x)
            jerk = m_max_jerk_x;
        if (m_max_jerk_y > 0 && jerk > m_max_jerk_y)
            jerk = m_max_jerk_y;
        
        gcode << "SET_VELOCITY_LIMIT SQUARE_CORNER_VELOCITY=" << jerk;
    } else {
        double jerk_x = jerk;
        double jerk_y = jerk;
        // Clamp the axis jerk to the allowed maximum.
        if (m_max_jerk_x > 0 && jerk > m_max_jerk_x)
            jerk_x = m_max_jerk_x;
        if (m_max_jerk_y > 0 && jerk > m_max_jerk_y)
            jerk_y = m_max_jerk_y;
        
        gcode << "M205 X" << jerk_x << " Y" << jerk_y;
    }
      
    if (m_is_bbl_printers)
        gcode << std::setprecision(2) << " Z" << m_max_jerk_z << " E" << m_max_jerk_e;

    if (GCodeWriter::full_gcode_comment) gcode << " ; adjust jerk";
    gcode << "\n";

    return gcode.str();

}

std::string GCodeWriter::set_accel_and_jerk(unsigned int acceleration, double jerk)
{
    // Only Klipper supports setting acceleration and jerk at the same time. Throw an error if we try to do this on other flavours.
    if(FLAVOR_IS_NOT(gcfKlipper))
        throw std::runtime_error("set_accel_and_jerk() is only supported by Klipper");

    // Clamp the acceleration to the allowed maximum.
    if (m_max_acceleration > 0 && acceleration > m_max_acceleration)
        acceleration = m_max_acceleration;
    
    bool is_empty = true;
    std::ostringstream gcode;
    gcode << "SET_VELOCITY_LIMIT";
    if (acceleration != 0 && acceleration != m_last_acceleration) {
        gcode << " ACCEL=" << acceleration;
        if (this->config.accel_to_decel_enable) {
            gcode << " ACCEL_TO_DECEL=" << acceleration * this->config.accel_to_decel_factor / 100;
        }
        m_last_acceleration = acceleration;
        is_empty = false;
    }
    // Clamp the jerk to the allowed maximum.
    if (m_max_jerk_x > 0 && jerk > m_max_jerk_x)
        jerk = m_max_jerk_x;
    if (m_max_jerk_y > 0 && jerk > m_max_jerk_y)
        jerk = m_max_jerk_y;

    if (jerk > 0.01 && !is_approx(jerk, m_last_jerk)) {
        gcode << " SQUARE_CORNER_VELOCITY=" << jerk;
        m_last_jerk = jerk;
        is_empty = false;
    }

    if(is_empty)
        return std::string();

    if (GCodeWriter::full_gcode_comment)
        gcode << " ; adjust VELOCITY_LIMIT(accel/jerk)";
    gcode << "\n";

    return gcode.str();

}

std::string GCodeWriter::set_junction_deviation(double junction_deviation){
    std::ostringstream gcode;
    if (FLAVOR_IS(gcfMarlinFirmware) && junction_deviation > 0 && m_max_junction_deviation > 0) {
        // Clamp the junction deviation to the allowed maximum.
        gcode << "M205 J";
        if (junction_deviation <= m_max_junction_deviation) {
            gcode << std::fixed << std::setprecision(3) << junction_deviation;
        } else {
            gcode << std::fixed << std::setprecision(3) << m_max_junction_deviation;
        }
        if (GCodeWriter::full_gcode_comment) {
            gcode << " ; Junction Deviation";
        }
        gcode << "\n";
    }
    return gcode.str();
}

std::string GCodeWriter::set_pressure_advance(double pa) const
{
    std::ostringstream gcode;
    if (pa < 0)
        return gcode.str();
    if(m_is_bbl_printers){
        //SoftFever: set L1000 to use linear model
        gcode << "M900 K" <<std::setprecision(4)<< pa << " L1000 M10 ; Override pressure advance value\n";
    }
    else{
        if (FLAVOR_IS(gcfKlipper))
            gcode << "SET_PRESSURE_ADVANCE ADVANCE=" << std::setprecision(4) << pa << "; Override pressure advance value\n";
        else if(FLAVOR_IS(gcfRepRapFirmware))
            gcode << ("M572 D0 S") << std::setprecision(4) << pa << "; Override pressure advance value\n";
        else
            gcode << "M900 K" <<std::setprecision(4)<< pa << "; Override pressure advance value\n";
    }
    return gcode.str();
}

std::string GCodeWriter::set_input_shaping(char axis, float damp, float freq, std::string type) const
{
    if (FLAVOR_IS(gcfMarlinLegacy))
        throw std::runtime_error("Input shaping is not supported by Marlin < 2.1.2.\nCheck your firmware version and update your G-code flavor to ´Marlin 2´");
    if (freq < 0.0f || damp < 0.f || damp > 1.0f || (axis != 'X' && axis != 'Y' && axis != 'Z' && axis != 'A'))// A = all axis
    {
    throw std::runtime_error("Invalid input shaping parameters: freq=" + std::to_string(freq) + ", damp=" + std::to_string(damp));
    }
    std::ostringstream gcode;
    if (FLAVOR_IS(gcfKlipper)) {
        gcode << "SET_INPUT_SHAPER";
        if (!type.empty() && type != "Default") {
                gcode << " SHAPER_TYPE=" << type;
        }
        if (axis != 'A')
        {
            if (freq > 0.0f) {
                gcode << " SHAPER_FREQ_" << axis << "=" << std::fixed << std::setprecision(2) << freq;
            }
            if (damp > 0.0f){
                gcode  << " DAMPING_RATIO_" << axis << "=" << std::fixed << std::setprecision(3) << damp;
            }
        } else {
            if (freq > 0.0f) {
                gcode << " SHAPER_FREQ_X=" << std::fixed << std::setprecision(2) << freq << " SHAPER_FREQ_Y=" << std::fixed << std::setprecision(2) << freq;
            }
            if (damp > 0.0f) {
                gcode << " DAMPING_RATIO_X=" << std::fixed << std::setprecision(3) << damp << " DAMPING_RATIO_Y=" << std::fixed << std::setprecision(3) << damp;
            }
        }
    } else if (FLAVOR_IS(gcfRepRapFirmware)) {
        gcode << "M593";
        if (!type.empty() && type != "Default" && type != "DAA") {
            gcode << " P\"" << type << "\"";
        }
        if (freq > 0.0f) {
            gcode << " F" << std::fixed << std::setprecision(2) << freq;
        }
        if (damp > 0.0f){
            gcode  << " S" << std::fixed << std::setprecision(3) << damp;
        }
    } else if (FLAVOR_IS(gcfMarlinFirmware)) {
        gcode << "M593";
        if (axis != 'A')
        {
            gcode << " " << axis;
        }
        if (freq > 0.0f)
        {
            gcode << " F" << std::fixed << std::setprecision(2) << freq;
        }
        if (damp > 0.0f)
        {
            gcode << " D" << std::fixed << std::setprecision(3) << damp;
        }
    } else {
        throw std::runtime_error("Input shaping is only supported by Klipper, RepRapFirmware and Marlin 2");
    }
    if (GCodeWriter::full_gcode_comment){
        gcode << " ; Override input shaping";
    }
    gcode << "\n";
    return gcode.str();
}


std::string GCodeWriter::reset_e(bool force)
{
    if (FLAVOR_IS(gcfMach3)
        || FLAVOR_IS(gcfMakerWare)
        || FLAVOR_IS(gcfSailfish))
        return "";

    if (m_curr_extruder_id!=-1 && m_curr_filament_extruder[m_curr_extruder_id] != nullptr) {
        if (is_zero(m_curr_filament_extruder[m_curr_extruder_id]->E()) && ! force)
            return "";
        m_curr_filament_extruder[m_curr_extruder_id]->reset_E();
    }

    if (! this->config.use_relative_e_distances) {
        std::ostringstream gcode;
        gcode << "G92 E0";
        //BBS
        if (GCodeWriter::full_gcode_comment) gcode << " ; reset extrusion distance";
        gcode << "\n";
        return gcode.str();
    } else {
        return "";
    }
}

std::string GCodeWriter::enable_power_loss_recovery(PowerLossRecoveryMode mode)
{
    std::ostringstream gcode;

    if (mode == PowerLossRecoveryMode::PrinterConfiguration)
        return std::string();

    const bool enable = mode == PowerLossRecoveryMode::Enable;

    if (m_is_bbl_printers) {
        gcode << "M1003 S" << (enable ? "1" : "0");
    }
    else if (FLAVOR_IS(gcfMarlinFirmware)) {
        gcode << "M413 S" << (enable ? "1" : "0");
    } else {
        return std::string();
    }
    if (GCodeWriter::full_gcode_comment) gcode << " ; set Power-loss Recovery";
    gcode << "\n";
    return gcode.str();
}

std::string GCodeWriter::update_progress(unsigned int num, unsigned int tot, bool allow_100) const
{
    if (FLAVOR_IS_NOT(gcfMakerWare) && FLAVOR_IS_NOT(gcfSailfish))
        return "";

    if (config.disable_m73) {
        return "";
    }

    unsigned int percent = (unsigned int)floor(100.0 * num / tot + 0.5);
    if (!allow_100) percent = std::min(percent, (unsigned int)99);

    std::ostringstream gcode;
    gcode << "M73 P" << percent;
    //BBS
    if (GCodeWriter::full_gcode_comment) gcode << " ; update progress";
    gcode << "\n";
    return gcode.str();
}

std::string GCodeWriter::toolchange_prefix() const
{
    return config.manual_filament_change ? ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Manual_Tool_Change) + "T":
           FLAVOR_IS(gcfMakerWare) ? "M135 T" :
           FLAVOR_IS(gcfSailfish)  ? "M108 T" : "T";
}

std::string GCodeWriter::toolchange(unsigned int filament_id)
{
    // set the new extruder
    auto filament_extruder_iter = Slic3r::lower_bound_by_predicate(m_filament_extruders.begin(), m_filament_extruders.end(), [filament_id](const Extruder &e) { return e.id() < filament_id; });
    assert(filament_extruder_iter != m_filament_extruders.end() && filament_extruder_iter->id() == filament_id);
    m_curr_extruder_id = filament_extruder_iter->extruder_id();
    m_curr_filament_extruder[m_curr_extruder_id] = &*filament_extruder_iter;

    // return the toolchange command
    // if we are running a single-extruder setup, just set the extruder and return nothing
    std::ostringstream gcode;
    if (this->multiple_extruders || (this->config.filament_diameter.values.size() > 1 && !is_bbl_printers())) {
        // BBS
        if (this->m_is_bbl_printers)
            gcode << "M1020 S" << filament_id;
        else
            gcode << this->toolchange_prefix() << filament_id;
        //BBS
        if (GCodeWriter::full_gcode_comment)
            gcode << " ; change extruder";
        gcode << "\n";
        gcode << this->reset_e(true);
    }
    return gcode.str();
}

std::string GCodeWriter::set_speed(double F, const std::string &comment, const std::string &cooling_marker)
{
    assert(F > 0.);
    assert(F < 100000.);
    
    m_current_speed = F;
    GCodeG1Formatter w;
    w.emit_f(F);
    //BBS
    w.emit_comment(GCodeWriter::full_gcode_comment, comment);
    w.emit_string(cooling_marker);
    return w.string();
}

std::string GCodeWriter::travel_to_xy(const Vec2d &point, const std::string &comment)
{
    m_pos(0) = point(0);
    m_pos(1) = point(1);

    this->set_current_position_clear(true);
    //BBS: take plate offset into consider
    Vec2d point_on_plate = { point(0) - m_x_offset, point(1) - m_y_offset };

    GCodeG1Formatter w;
    w.emit_xy(point_on_plate);
    auto speed = m_is_first_layer
        ? this->config.get_abs_value("initial_layer_travel_speed") : this->config.travel_speed.value;
    w.emit_f(speed * 60.0);
    //BBS
    w.emit_comment(GCodeWriter::full_gcode_comment, comment);
    return w.string();
}

/*  If this method is called more than once before calling unlift(),
it will not perform subsequent lifts, even if Z was raised manually
(i.e. with travel_to_z()) and thus _lifted was reduced. */
std::string GCodeWriter::lazy_lift(LiftType lift_type, bool spiral_vase)
{
    // check whether the above/below conditions are met
    double target_lift = 0;
    {
        //BBS
        int extruder_id = filament()->extruder_id();
        int filament_id = filament()->id();
        double above = this->config.retract_lift_above.get_at(extruder_id);
        double below = this->config.retract_lift_below.get_at(extruder_id);
        if (m_pos.z() >= above && m_pos.z() <= below)
            target_lift = this->config.z_hop.get_at(filament_id);
    }
    // BBS
    if (m_lifted == 0 && m_to_lift == 0 && target_lift > 0) {
        if (spiral_vase) {
            m_lifted = target_lift;
            return this->_travel_to_z(m_pos(2) + target_lift, "lift Z");
        }
        else {
            m_to_lift = target_lift;
            m_to_lift_type = lift_type;
        }
    }
    return "";
}

// BBS: immediately execute an undelayed lift move with a spiral lift pattern
// designed specifically for subsequent gcode injection (e.g. timelapse) 
std::string GCodeWriter::eager_lift(const LiftType type) {
    std::string lift_move;
    double target_lift = 0;
    {
        //BBS
        int extruder_id = filament()->extruder_id();
        int filament_id = filament()->id();
        double above = this->config.retract_lift_above.get_at(extruder_id);
        double below = this->config.retract_lift_below.get_at(extruder_id);
        if (m_pos.z() >= above && m_pos.z() <= below)
            target_lift = this->config.z_hop.get_at(filament_id);
    }

    // BBS: spiral lift only safe with known position
    // TODO: check the arc will move within bed area
    if (type == LiftType::SpiralLift && this->is_current_position_clear()) {
        double radius = target_lift / (2 * PI * atan(filament()->travel_slope()));
        // static spiral alignment when no move in x,y plane.
        // spiral centra is a radius distance to the right (y=0) 
        Vec2d ij_offset = { radius, 0 };
        if (target_lift > 0) {
            lift_move = this->_spiral_travel_to_z(m_pos(2) + target_lift, ij_offset, "spiral lift Z");
        }
    }
    //BBS: if position is unknown use normal lift
    else if (target_lift > 0) {
        lift_move = _travel_to_z(m_pos(2) + target_lift, "normal lift Z");
    }
    m_lifted = target_lift;
    m_to_lift = 0;
    return lift_move;
}

std::string GCodeWriter::travel_to_xyz(const Vec3d &point, const std::string &comment, bool force_z)
{
    // FIXME: This function was not being used when travel_speed_z was separated (bd6badf).
    // Calculation of feedrate was not updated accordingly. If you want to use
    // this function, fix it first.
    //std::terminate();

    /*  If target Z is lower than current Z but higher than nominal Z we
        don't perform the Z move but we only move in the XY plane and
        adjust the nominal Z by reducing the lift amount that will be
        used for unlift. */
        // BBS
    Vec3d dest_point = point;
    auto travel_speed =
        m_is_first_layer ? this->config.get_abs_value("initial_layer_travel_speed") : this->config.travel_speed.value;
    //BBS: a z_hop need to be handle when travel
    if (std::abs(m_to_lift) > EPSILON) {
        assert(std::abs(m_lifted) < EPSILON);
        //BBS: don't need to do real lift if the current position is absolutely same with target.
        //This ususally happens when the last extrusion line is short and the end of wipe position
        //is same with the traget point by chance.
        if ((!this->is_current_position_clear() || m_pos != dest_point) &&
            m_to_lift + m_pos(2) > point(2)) {
            m_lifted = m_to_lift + m_pos(2) - point(2);
            dest_point(2) = m_to_lift + m_pos(2);
        }
        m_to_lift = 0.;

        std::string slop_move;
        //BBS: minus plate offset
        Vec3d source = { m_pos(0) - m_x_offset, m_pos(1) - m_y_offset, m_pos(2) };
        Vec3d target = { dest_point(0) - m_x_offset, dest_point(1) - m_y_offset, dest_point(2) };
        Vec3d delta = target - source;
        Vec2d delta_no_z = { delta(0), delta(1) };
        //BBS: don'need slope travel because we don't know where is the source position the first time
        //BBS: Also don't need to do slope move or spiral lift if x-y distance is absolute zero
        if (delta(2) > 0 && delta_no_z.norm() != 0.0f)    {
            //BBS: SpiralLift
            if (m_to_lift_type == LiftType::SpiralLift && this->is_current_position_clear()) {
                //BBS: todo: check the arc move all in bed area, if not, then use lazy lift
                double radius = delta(2) / (2 * PI * atan(this->filament()->travel_slope()));
                Vec2d ij_offset = radius * delta_no_z.normalized();
                ij_offset = { -ij_offset(1), ij_offset(0) };
                slop_move = this->_spiral_travel_to_z(target(2), ij_offset, "spiral lift Z");
            }
            //BBS: SlopeLift
            else if (m_to_lift_type == LiftType::SlopeLift &&
                this->is_current_position_clear() &&
                atan2(delta(2), delta_no_z.norm()) < this->filament()->travel_slope()) {
                //BBS: check whether we can make a travel like
                //   _____
                //  /       to make the z list early to avoid to hit some warping place when travel is long.
                Vec2d temp = delta_no_z.normalized() * delta(2) / tan(this->filament()->travel_slope());
                Vec3d slope_top_point = Vec3d(temp(0), temp(1), delta(2)) + source;
                GCodeG1Formatter w0;
                w0.emit_xyz(slope_top_point);
                w0.emit_f(travel_speed * 60.0);
                //BBS
                w0.emit_comment(GCodeWriter::full_gcode_comment, comment);
                slop_move = w0.string();
            }
            else if (m_to_lift_type == LiftType::NormalLift) {
                slop_move = _travel_to_z(target.z(), "normal lift Z");
            }
        }

        std::string xy_z_move;
        {
            GCodeG1Formatter w0;
            if (this->is_current_position_clear()) {
                w0.emit_xyz(target);
                w0.emit_f(travel_speed * 60.0);
                w0.emit_comment(GCodeWriter::full_gcode_comment, comment);
                xy_z_move = w0.string();
            }
            else {
                w0.emit_xy(Vec2d(target.x(), target.y()));
                w0.emit_f(travel_speed * 60.0);
                w0.emit_comment(GCodeWriter::full_gcode_comment, comment);
                xy_z_move = w0.string() + _travel_to_z(target.z(), comment);
            }
        }
        m_pos = dest_point;
        this->set_current_position_clear(true);
        return slop_move + xy_z_move;
    }
    else if (!force_z && !this->will_move_z(point(2))) {
        double nominal_z = m_pos(2) - m_lifted;
        m_lifted -= (point(2) - nominal_z);
        // In case that z_hop == layer_height we could end up with almost zero in_m_lifted
        // and a retract could be skipped
        if (std::abs(m_lifted) < EPSILON)
            m_lifted = 0.;
        //BBS
        this->set_current_position_clear(true);
        return this->travel_to_xy(to_2d(point));
    }
    else {
        /*  In all the other cases, we perform an actual XYZ move and cancel
            the lift. */
        m_lifted = 0;
    }

    //BBS: take plate offset into consider
    Vec3d point_on_plate = { dest_point(0) - m_x_offset, dest_point(1) - m_y_offset, dest_point(2) };
    std::string out_string;
    GCodeG1Formatter w;
    if (!this->is_current_position_clear())
    {
        //force to move xy first then z after filament change
        w.emit_xy(Vec2d(point_on_plate.x(), point_on_plate.y()));
        w.emit_f(this->config.travel_speed.value * 60.0);
        w.emit_comment(GCodeWriter::full_gcode_comment, comment);
        out_string = w.string() + _travel_to_z(point_on_plate.z(), comment);
    } else {
        GCodeG1Formatter w;
        w.emit_xyz(point_on_plate);
        w.emit_f(this->config.travel_speed.value * 60.0);
        w.emit_comment(GCodeWriter::full_gcode_comment, comment);
        out_string = w.string();
    }

    m_pos = dest_point;
    this->set_current_position_clear(true);
    return out_string;
}

std::string GCodeWriter::travel_to_z(double z, const std::string &comment, bool force)
{
    /*  If target Z is lower than current Z but higher than nominal Z
        we don't perform the move but we only adjust the nominal Z by
        reducing the lift amount that will be used for unlift. */
    if (!force && !this->will_move_z(z)) {
        double nominal_z = m_pos(2) - m_lifted;
        m_lifted -= (z - nominal_z);
        if (std::abs(m_lifted) < EPSILON)
            m_lifted = 0.;
        return "";
    }

    /*  In all the other cases, we perform an actual Z move and cancel
        the lift. */
    m_lifted = 0;
    return this->_travel_to_z(z, comment);
}

std::string GCodeWriter::_travel_to_z(double z, const std::string &comment)
{
    m_pos(2) = z;

    double speed = this->config.travel_speed_z.value;
    if (speed == 0.) {
        speed = m_is_first_layer ? this->config.get_abs_value("initial_layer_travel_speed")
                                 : this->config.travel_speed.value;
    }

    GCodeG1Formatter w;
    w.emit_z(z);
    w.emit_f(speed * 60.0);
    //BBS
    w.emit_comment(GCodeWriter::full_gcode_comment, comment);
    return w.string();
}

std::string GCodeWriter::_spiral_travel_to_z(double z, const Vec2d &ij_offset, const std::string &comment)
{
    std::string output;
    double speed = this->config.travel_speed_z.value;

    if (speed == 0.) {
        speed = m_is_first_layer ? this->config.get_abs_value("initial_layer_travel_speed")
                                 : this->config.travel_speed.value;
    }

    if (!this->config.enable_arc_fitting) { // Orca: if arc fitting is disabled, approximate the arc with small linear segments
        std::ostringstream oss;
        const double z_start = m_pos(2); // starting Z height

        // --------------------------------------------------------------------
        // Determine number of segments based on Resolution
        // --------------------------------------------------------------------
        const double ref_resolution = 0.01; // reference resolution in mm
        const double ref_segments  = 16.0;  // reference number of segments at reference resolution
        
        // number of linear segments to use for approximating the arc, clamp between 4 and 24
        const int segments = std::clamp(int(std::round(ref_segments * (ref_resolution / m_resolution))), 4, 24);
        // --------------------------------------------------------------------

        const double px = m_pos(0) - m_x_offset;        // take plate offset into consideration
        const double py = m_pos(1) - m_y_offset;        // take plate offset into consideration
        const double cx = px + ij_offset(0);            // center x
        const double cy = py + ij_offset(1);            // center y
        const double radius = ij_offset.norm();         // radius
        const double a0 = std::atan2(py - cy, px - cx); // start angle
        const double delta = 2.0 * M_PI;                // CCW full circle

        if (full_gcode_comment)
            oss << ";" << comment << "\n";

        oss << "G1 F" << (speed * 60.0) << "\n";  // set feedrate

        // approximate the arc with small linear segments (without the last point which is added later to ensure exactness)
        for (int i = 1; i < segments; ++i) {
            double t = double(i) / segments;            // parametric position along arc
            double a = a0 + delta * t;                  // CCW arc param
            double x = cx + radius * std::cos(a);       // point on circle
            double y = cy + radius * std::sin(a);       // point on circle
            double zz = z_start + (z - z_start) * t;    // interpolated Z height

            oss << "G1 X" << x << " Y" << y << " Z" << zz << "\n";
        }

        oss << "G1 X" << px << " Y" << py << " Z" << z << "\n";  // final point to ensure exactness
        output = oss.str();
    } else { // Orca: if arc fitting is enabled emit a G2/G3 command for the spiral lift
        output = std::string("G17") + (full_gcode_comment ? " ; XY plane for arc\n" : "\n");

        GCodeG2G3Formatter w(true);
        w.emit_z(z);
        w.emit_ij(ij_offset);
        w.emit_string(" P1 ");
        w.emit_f(speed * 60.0);
        w.emit_comment(GCodeWriter::full_gcode_comment, comment);

        output += w.string();
    }

    m_pos(2) = z;
    return output;
}

bool GCodeWriter::will_move_z(double z) const
{
    /* If target Z is lower than current Z but higher than nominal Z
        we don't perform an actual Z move. */
    if (m_lifted > 0) {
        double nominal_z = m_pos(2) - m_lifted;
        if (z >= nominal_z && z <= m_pos(2))
            return false;
    }
    // BBS.
    // Dont move z if it is the same as target z
    else if (std::abs(m_pos(2) - z) < EPSILON) {
        return false;
    }
    return true;
}

std::string GCodeWriter::extrude_to_xy(const Vec2d &point, double dE, const std::string &comment, bool force_no_extrusion)
{
    m_pos(0) = point(0);
    m_pos(1) = point(1);
    if(std::abs(dE) <= std::numeric_limits<double>::epsilon())
        force_no_extrusion = true;

    if (!force_no_extrusion)
        filament()->extrude(dE);

    //BBS: take plate offset into consider
    Vec2d point_on_plate = { point(0) - m_x_offset, point(1) - m_y_offset };

    GCodeG1Formatter w;
    w.emit_xy(point_on_plate);
    if (!force_no_extrusion)
        w.emit_e(filament()->E());
    //BBS
    w.emit_comment(GCodeWriter::full_gcode_comment, comment);
    return w.string();
}

//BBS: generate G2 or G3 extrude which moves by arc
//point is end point which means X and Y axis
//center_offset is I and J axis
std::string GCodeWriter::extrude_arc_to_xy(const Vec2d& point, const Vec2d& center_offset, double dE, const bool is_ccw, const std::string& comment, bool force_no_extrusion)
{
    m_pos(0) = point(0);
    m_pos(1) = point(1);
    if (!force_no_extrusion)
        filament()->extrude(dE);

    Vec2d point_on_plate = { point(0) - m_x_offset, point(1) - m_y_offset };

    GCodeG2G3Formatter w(is_ccw);
    w.emit_xy(point_on_plate);
    w.emit_ij(center_offset);
    if (!force_no_extrusion)
        w.emit_e(filament()->E());
    //BBS
    w.emit_comment(GCodeWriter::full_gcode_comment, comment);
    return w.string();
}

std::string GCodeWriter::extrude_to_xyz(const Vec3d &point, double dE, const std::string &comment, bool force_no_extrusion)
{
    m_pos = point;
    m_lifted = 0;
    if (!force_no_extrusion)
        filament()->extrude(dE);

    //BBS: take plate offset into consider
    Vec3d point_on_plate = { point(0) - m_x_offset, point(1) - m_y_offset, point(2) };

    GCodeG1Formatter w;
    w.emit_xyz(point_on_plate);
    if (!force_no_extrusion)
        w.emit_e(filament()->E());
    //BBS
    w.emit_comment(GCodeWriter::full_gcode_comment, comment);
    return w.string();
}

std::string GCodeWriter::retract(bool before_wipe, double retract_length)
{
    double factor = before_wipe ? filament()->retract_before_wipe() : 1.;
    assert(factor >= 0. && factor <= 1. + EPSILON);
    return this->_retract(
        retract_length > EPSILON ? retract_length : factor * filament()->retraction_length(),
        factor * filament()->retract_restart_extra(),
        "retract"
    );
}

std::string GCodeWriter::retract_for_toolchange(bool before_wipe, double retract_length)
{
    double factor = before_wipe ? filament()->retract_before_wipe() : 1.;
    assert(factor >= 0. && factor <= 1. + EPSILON);
    return this->_retract(
        retract_length > EPSILON ? retract_length : factor * filament()->retract_length_toolchange(),
        factor * filament()->retract_restart_extra_toolchange(),
        "retract for toolchange"
    );
}

std::string GCodeWriter::_retract(double length, double restart_extra, const std::string &comment)
{
    /*  If firmware retraction is enabled, we use a fake value of 1
    since we ignore the actual configured retract_length which
    might be 0, in which case the retraction logic gets skipped. */
    if (this->config.use_firmware_retraction)
        length = 1;

    std::string gcode;
    if (double dE = filament()->retract(length, restart_extra);  !is_zero(dE)) {
        if (this->config.use_firmware_retraction) {
            gcode = FLAVOR_IS(gcfMachinekit) ? "G22 ; retract\n" : "G10 ; retract\n";
        }
        else {
            // BBS
            GCodeG1Formatter w;
            w.emit_e(filament()->E());
            w.emit_f(filament()->retract_speed() * 60.);
            // BBS
            w.emit_comment(GCodeWriter::full_gcode_comment, comment);
            gcode = w.string();
        }
    }

    if (FLAVOR_IS(gcfMakerWare))
        gcode += "M103 ; extruder off\n";

    return gcode;
}

std::string GCodeWriter::unretract()
{
    std::string gcode;

    if (FLAVOR_IS(gcfMakerWare))
        gcode = "M101 ; extruder on\n";

    if (double dE = filament()->unretract(); !is_zero(dE)) {
        if (this->config.use_firmware_retraction) {
            gcode += FLAVOR_IS(gcfMachinekit) ? "G23 ; unretract\n" : "G11 ; unretract\n";
            gcode += this->reset_e();
        }
        else {
            //BBS
            // use G1 instead of G0 because G0 will blend the restart with the previous travel move
            GCodeG1Formatter w;
            w.emit_e(filament()->E());
            w.emit_f(filament()->deretract_speed() * 60.);
            //BBS
            w.emit_comment(GCodeWriter::full_gcode_comment, " ; unretract");
            gcode += w.string();
        }
    }

    return gcode;
}


std::string GCodeWriter::unlift()
{
    std::string gcode;
    if (m_lifted > 0) {
        gcode += this->_travel_to_z(m_pos(2) - m_lifted, "restore layer Z");
        m_lifted = 0;
    }
    m_to_lift = 0.;
    return gcode;
}

std::string GCodeWriter::set_fan(const GCodeFlavor gcode_flavor, unsigned int speed)
{
    std::ostringstream gcode;
    if (speed == 0) {
        switch (gcode_flavor) {
        case gcfTeacup:
            gcode << "M106 S0"; break;
        case gcfMakerWare:
        case gcfSailfish:
            gcode << "M127";    break;
        default:
            gcode << "M106 S0";    break;
        }
        if (GCodeWriter::full_gcode_comment)
            gcode << " ; disable fan";
        gcode << "\n";
    } else {
        switch (gcode_flavor) {
        case gcfMakerWare:
        case gcfSailfish:
            gcode << "M126";    break;
        case gcfMach3:
        case gcfMachinekit:
            gcode << "M106 P" << static_cast<unsigned int>(255.5 * speed / 100.0); break;
        default:
            gcode << "M106 S" << static_cast<unsigned int>(255.5 * speed / 100.0); break;
        }
        if (GCodeWriter::full_gcode_comment)
            gcode << " ; enable fan";
        gcode << "\n";
    }
    return gcode.str();
}

std::string GCodeWriter::set_fan(unsigned int speed) const
{
    //BBS
    return GCodeWriter::set_fan(this->config.gcode_flavor, speed);
}

//BBS: set additional fan speed for BBS machine only
std::string GCodeWriter::set_additional_fan(unsigned int speed)
{
    std::ostringstream gcode;

    gcode << "M106 " << "P2 " << "S" << (int)(255.0 * speed / 100.0);
    if (GCodeWriter::full_gcode_comment) {
        if (speed == 0)
            gcode << " ; disable additional fan ";
        else
            gcode << " ; enable additional fan ";
    }
    gcode << "\n";
    return gcode.str();
}

std::string GCodeWriter::set_exhaust_fan( int speed,bool add_eol)
{
    std::ostringstream gcode;
    gcode << "M106" << " P3" << " S" << (int)(speed / 100.0 * 255);

    if(add_eol)
        gcode << "\n";
    return gcode.str();
}

void GCodeWriter::add_object_start_labels(std::string& gcode)
{
    if (!m_gcode_label_objects_start.empty()) {
        gcode += m_gcode_label_objects_start;
        m_gcode_label_objects_start = "";
    }
}

void GCodeWriter::add_object_end_labels(std::string& gcode)
{
    if (!m_gcode_label_objects_end.empty()) {
        gcode += m_gcode_label_objects_end;
        m_gcode_label_objects_end = "";

        // Orca: reset E so that e value remain correct after skipping the object
        // ref to: https://github.com/OrcaSlicer/OrcaSlicer/pull/205/commits/7f1fe0bd544077626080aa1a9a0576aa735da1a4#r1083470162
        if (!this->config.use_relative_e_distances)
            gcode += reset_e(true);
    }
}

void GCodeWriter::add_object_change_labels(std::string& gcode)
{
    add_object_end_labels(gcode);
    add_object_start_labels(gcode);
}

std::string GCodeWriter::set_extruder(unsigned int filament_id)
{
    auto filament_ext_it = Slic3r::lower_bound_by_predicate(m_filament_extruders.begin(), m_filament_extruders.end(), [filament_id](const Extruder &e) { return e.id() < filament_id; });
    unsigned int extruder_id = filament_ext_it->extruder_id();
    assert(filament_ext_it != m_filament_extruders.end() && filament_ext_it->id() == filament_id);
    //TODO: optmize here, pass extruder_id to toolchange
    return this->need_toolchange(filament_id) ? this->toolchange(filament_id) : "";
}

void GCodeWriter::init_extruder(unsigned int filament_id)
{
    if (m_curr_extruder_id == -1 && filament_id != -1) {
        auto filament_extruder_iter = Slic3r::lower_bound_by_predicate(m_filament_extruders.begin(), m_filament_extruders.end(), [filament_id](const Extruder &e) { return e.id() < filament_id; });
        assert(filament_extruder_iter != m_filament_extruders.end() && filament_extruder_iter->id() == filament_id);
        m_curr_extruder_id = filament_extruder_iter->extruder_id();
        m_curr_filament_extruder[m_curr_extruder_id] = &*filament_extruder_iter;
    }
}

bool GCodeWriter::need_toolchange(unsigned int filament_id)const
{
    return filament()==nullptr || filament()->id()!=filament_id;
}

void GCodeFormatter::emit_axis(const char axis, const double v, size_t digits) {
    assert(digits <= 9);
    static constexpr const std::array<int, 10> pow_10{1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000};
    *ptr_err.ptr++ = ' '; *ptr_err.ptr++ = axis;

    char *base_ptr = this->ptr_err.ptr;
    auto  v_int    = int64_t(std::round(v * pow_10[digits]));
    // Older stdlib on macOS doesn't support std::from_chars at all, so it is used boost::spirit::karma::generate instead of it.
    // That is a little bit slower than std::to_chars but not much.
#ifdef __APPLE__
    boost::spirit::karma::generate(this->ptr_err.ptr, boost::spirit::karma::int_generator<int64_t>(), v_int);
#else
    // this->buf_end minus 1 because we need space for adding the extra decimal point.
    this->ptr_err = std::to_chars(this->ptr_err.ptr, this->buf_end - 1, v_int);
#endif
    size_t writen_digits = (this->ptr_err.ptr - base_ptr) - (v_int < 0 ? 1 : 0);
    if (writen_digits < digits) {
        // Number is smaller than 10^digits, so that we will pad it with zeros.
        size_t remaining_digits = digits - writen_digits;
        // Move all newly inserted chars by remaining_digits to allocate space for padding with zeros.
        for (char *from_ptr = this->ptr_err.ptr - 1, *to_ptr = from_ptr + remaining_digits; from_ptr >= this->ptr_err.ptr - writen_digits; --to_ptr, --from_ptr)
            *to_ptr = *from_ptr;

        memset(this->ptr_err.ptr - writen_digits, '0', remaining_digits);
        this->ptr_err.ptr += remaining_digits;
    }

    // Move all newly inserted chars by one to allocate space for a decimal point.
    for (char *to_ptr = this->ptr_err.ptr, *from_ptr = to_ptr - 1; from_ptr >= this->ptr_err.ptr - digits; --to_ptr, --from_ptr)
        *to_ptr = *from_ptr;

    *(this->ptr_err.ptr - digits) = '.';
    for (size_t i = 0; i < digits; ++i) {
        if (*this->ptr_err.ptr != '0')
            break;
        this->ptr_err.ptr--;
    }
    if (*this->ptr_err.ptr == '.')
        this->ptr_err.ptr--;
    if ((this->ptr_err.ptr + 1) == base_ptr || *this->ptr_err.ptr == '-')
        *(++this->ptr_err.ptr) = '0';
    this->ptr_err.ptr++;

#if 0 // #ifndef NDEBUG
    {
        // Verify that the optimized formatter produces the same result as the standard sprintf().
        double v1 = atof(std::string(base_ptr, this->ptr_err.ptr).c_str());
        char buf[2048];
        sprintf(buf, "%.*lf", int(digits), v);
        double v2 = atof(buf);
        // Numbers may differ when rounding at exactly or very close to 0.5 due to numerical issues when scaling the double to an integer.
        // Thus the complex assert.
//        assert(v1 == v2);
        assert(std::abs(v1 - v) * pow_10[digits] < 0.50001);
        assert(std::abs(v2 - v) * pow_10[digits] < 0.50001);
    }
#endif // NDEBUG
}

} // namespace Slic3r
