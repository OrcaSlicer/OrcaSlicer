#include "GCodeWriter.hpp"
#include "Config.hpp"
#include "Extruder.hpp"
#include "Geometry.hpp"
#include "I18N.hpp"
#include "Point.hpp"
#include "Polygon.hpp"
#include "PrintConfig.hpp"
#include "IMEXHelpers.hpp"
#include "ClipperUtils.hpp"
#include "Geometry/ArcWelder.hpp"
#include "Line.hpp"
#include "LocalesUtils.hpp"
#include "libslic3r.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <array>
#include <cstdint>
#include <charconv>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <assert.h>
#include <boost/format.hpp>
#include <GCode/GCodeProcessor.hpp>
#include <string>
#include <vector>
#include <utility>
#include <sstream>
#include <stdexcept>
#include <math.h>
#include <memory>

#ifdef __APPLE__
    #include <boost/spirit/include/karma.hpp>
#endif

#define FLAVOR_IS(val) this->config.gcode_flavor == val
#define FLAVOR_IS_NOT(val) this->config.gcode_flavor != val

namespace Slic3r {

// UNLAYERED_MOD — sliced-frame point -> emitted (bed-frame) point. `trafo` is the print object's
// post_trafo_inverse(); the firmware is told the matching table orientation via B/C.
//
// `extruder_offset` is the active nozzle's XY offset in the MACHINE frame. GCode::point_to_gcode
// has already subtracted it from v's XY, so without a correction the rotation below carries that
// machine-frame quantity into the chunk's frame: the emitted point becomes R*(P - eo) + t when it
// should be R*P + t - eo. The difference is (R - I) * eo, which is exactly zero for an untilted
// segment — so a tilted chunk ends up displaced relative to the root it sits on, by the full offset
// and along a different axis. Adding it back puts the subtraction where it belongs.
static inline Vec3d apply_test_rotation2(const Vec3d& v, const Transform3d& trafo,
                                         const Vec3d& extruder_offset = Vec3d::Zero())
{
    const Vec3d out = trafo.rotation() * v + trafo.translation();
    if (extruder_offset.isZero())
        return out;                      // the usual case: nothing to correct, output unchanged
    return out + trafo.rotation() * extruder_offset - extruder_offset;
}

bool GCodeWriter::full_gcode_comment = true;

// A lift emitted through _travel_to_z() re-emits the stored logical X/Y under a
// mapping that must emit every axis. While the position is unknown that X/Y is
// the uninitialised origin, which maps to a real but wrong machine point, so the
// lift has to be skipped rather than commanded.
bool GCodeWriter::must_skip_lift_now() const
{
    return m_kinematics->suppress_lift_at_unknown_position() && ! this->is_current_position_clear();
}

bool GCodeWriter::point_on_first_layer(const Vec3d &point_logical) const
{
    if (m_first_layer_point_test)
        return m_first_layer_point_test(point_logical);
    // BELT_MOD -- the contact-aware first layer's band, measured up from the bed.
    if (m_first_layer_band >= 0.)
        return point_logical.z() < m_first_layer_band;
    return m_is_first_layer;
}

void GCodeWriter::set_axis_remap(int rx, int ry, int rz)
{
    m_remap_x = rx;
    m_remap_y = ry;
    m_remap_z = rz;
    m_kinematics->set_axis_remap(rx, ry, rz);
}

void GCodeWriter::set_build_volume_max(const Vec3d &max)
{
    m_build_vol_max = max;
    m_kinematics->set_build_volume_max(max);
}

void GCodeWriter::set_kinematics(std::unique_ptr<MachineKinematics> kinematics)
{
    assert(kinematics);
    m_kinematics = std::move(kinematics);
    // Replay whatever was configured on the previous strategy so callers may
    // install the kinematics before or after set_axis_remap/set_build_volume_max.
    m_kinematics->set_axis_remap(m_remap_x, m_remap_y, m_remap_z);
    m_kinematics->set_build_volume_max(m_build_vol_max);
}

// Kept as the writer-facing name for "this move must emit every axis word".
bool GCodeWriter::has_axis_remap() const
{
    return m_kinematics->must_emit_all_axes();
}

Vec3d GCodeWriter::apply_axis_remap(const Vec3d &pos) const
{
    return m_kinematics->to_machine(pos);
}

bool GCodeWriter::supports_separate_travel_acceleration(GCodeFlavor flavor)
{
    return (flavor == gcfRepetier || flavor == gcfMarlinFirmware ||  flavor == gcfRepRapFirmware);
}

void GCodeWriter::apply_print_config(const PrintConfig &print_config)
{
    this->config.apply(print_config, true);

    // UNLAYERED_MOD — see extruder_offset_xy(). GCodeConfig does not carry extruder_offset, so the
    // per-extruder XY offsets are cached here, where the full PrintConfig is in hand.
    m_extruder_offsets = print_config.extruder_offset.values;
    // UNLAYERED_MOD — see the clamp in lazy_lift()/eager_lift().
    m_nonplanar = print_config.enable_nonplanar.value;

    // Some machine limits are stride-2 (normal, silent) pairs, here we extract the value that will be used,
    // which is always normal mode at the moment
    // TODO: support silent? Any printer actually have that?
    auto get_machine_limits = [](const std::string key, const ConfigOptionFloats& opt) -> std::vector<double> {
        unsigned int stride = 1;
        unsigned int offset = 0;
        if (printer_options_with_variant_2.count(key) > 0) {
            stride = 2;
            // offset = <TODO: current print mode>;
        }

        std::vector<double> results;
        results.reserve(opt.values.size() / stride);

        for (unsigned int i = offset; i < opt.values.size(); i += stride) {
            results.emplace_back(opt.values[i]);
        }

        return results;
    };
    auto rounded = [](std::vector<double>&& vec) -> std::vector<double>&&{
        std::transform(vec.cbegin(), vec.cend(), vec.begin(), [](const double v) { return std::round(v); });
        return std::move(vec);
    };
    auto to_uint = [](const std::vector<double>& vec) {
        std::vector<unsigned int> r;
        std::transform(vec.begin(), vec.end(), std::back_inserter(r), [](const double v) { return static_cast<unsigned int>(v); });
        return r;
    };
#define LIMITS(OPT) get_machine_limits(#OPT, print_config.OPT)
#define LIMITS_UINT(OPT) to_uint(rounded(LIMITS(OPT)))

    m_single_extruder_multi_material = print_config.single_extruder_multi_material.value;
    bool use_mach_limits = print_config.gcode_flavor.value == gcfMarlinLegacy || print_config.gcode_flavor.value == gcfMarlinFirmware ||
                           print_config.gcode_flavor.value == gcfKlipper || print_config.gcode_flavor.value == gcfRepRapFirmware;
    if (use_mach_limits) {
        // For Klipper, SET_VELOCITY_LIMIT ACCEL= applies to all moves, so the effective cap
        // is the minimum of the extruding limit and the per-axis X/Y limits.
        // This ensures user-configured Motion Ability limits are honoured (#12244).
        auto extruding_limit = LIMITS_UINT(machine_max_acceleration_extruding);
        if (print_config.gcode_flavor.value == gcfKlipper) {
            auto x_limit = LIMITS_UINT(machine_max_acceleration_x);
            auto y_limit = LIMITS_UINT(machine_max_acceleration_y);

            for (size_t i = 0; i < extruding_limit.size(); i++) {
                if (x_limit[i] > 0) extruding_limit[i] = std::min(extruding_limit[i], x_limit[i]);
                if (y_limit[i] > 0) extruding_limit[i] = std::min(extruding_limit[i], y_limit[i]);
            }
        }
        m_max_acceleration = std::move(extruding_limit);
    } else {
        m_max_acceleration.clear();
    }
    if (use_mach_limits && supports_separate_travel_acceleration(print_config.gcode_flavor.value)) {
        m_max_travel_acceleration = LIMITS_UINT(machine_max_acceleration_travel);
    } else {
        m_max_travel_acceleration.clear();
    }
    if (use_mach_limits) {
        m_max_jerk_x             = rounded(LIMITS(machine_max_jerk_x));
        m_max_jerk_y             = rounded(LIMITS(machine_max_jerk_y));
        m_max_junction_deviation = LIMITS(machine_max_junction_deviation);
    } else {
        m_max_jerk_x.clear();
        m_max_jerk_y.clear();
        m_max_junction_deviation.clear();
    }
    m_max_jerk_z = LIMITS(machine_max_jerk_z);
    m_max_jerk_e = LIMITS(machine_max_jerk_e);
    m_resolution = print_config.resolution.value;
#undef LIMITS
#undef LIMITS_UINT
    // Orca: capture the printable area(s) so a spiral lift can be skipped when its
    // circle would leave the boundary and collide with the print limits. Full polygons
    // are stored (not a bounding box) so the check stays correct for non-rectangular
    // beds, and per-extruder areas are kept so printers with different boundaries per
    // extruder use the right limit for whichever extruder is active.
    auto to_scaled_polygon = [](const Pointfs &pts) {
        Polygon poly;
        poly.points.reserve(pts.size());
        for (const Vec2d &p : pts)
            poly.points.emplace_back(coord_t(scale_(p.x())), coord_t(scale_(p.y())));
        poly.make_counter_clockwise();
        return poly;
    };

    m_bed_printable_area.points.clear();
    m_extruder_printable_areas.clear();

    if (print_config.printable_area.values.size() >= 3)
        m_bed_printable_area = to_scaled_polygon(print_config.printable_area.values);

    const std::vector<Pointfs> &extruder_areas = print_config.extruder_printable_area.values;
    if (!extruder_areas.empty()) {
        m_extruder_printable_areas.resize(extruder_areas.size());
        for (size_t i = 0; i < extruder_areas.size(); ++i) {
            if (extruder_areas[i].size() < 3) {
                // No dedicated area for this extruder: it can reach the whole bed.
                m_extruder_printable_areas[i] = m_bed_printable_area;
                continue;
            }
            Polygon extruder_poly = to_scaled_polygon(extruder_areas[i]);
            if (m_bed_printable_area.points.size() < 3) {
                m_extruder_printable_areas[i] = std::move(extruder_poly);
                continue;
            }
            // The reachable area is the extruder area clipped to the bed. Bed shapes are
            // convex in practice, so keep the largest resulting contour.
            Polygons clipped = intersection(extruder_poly, m_bed_printable_area);
            const Polygon *largest = nullptr;
            double          best_area = 0.;
            for (const Polygon &p : clipped) {
                double a = std::abs(p.area());
                if (a > best_area) { best_area = a; largest = &p; }
            }
            m_extruder_printable_areas[i] = largest ? *largest : std::move(extruder_poly);
        }
    }
}

const Polygon *GCodeWriter::active_printable_area() const
{
    if (const Extruder *e = this->filament()) {
        size_t id = e->extruder_id();
        if (id < m_extruder_printable_areas.size() && m_extruder_printable_areas[id].points.size() >= 3)
            return &m_extruder_printable_areas[id];
    }
    if (m_bed_printable_area.points.size() >= 3)
        return &m_bed_printable_area;
    return nullptr;
}

bool GCodeWriter::spiral_lift_fits_printable_area(const Vec2d &center, double radius) const
{
    const Polygon *area = this->active_printable_area();
    if (area == nullptr)
        return true; // Boundary unknown: don't restrict (preserve previous behavior).

    const Point  c        = Point::new_scale(center.x(), center.y());
    const double r_scaled = scale_(radius);
    const double r2       = r_scaled * r_scaled;

    // The spiral traces a full circle of `radius` around `center`, so the center must lie
    // inside the printable area and every edge must be at least `radius` away from it.
    if (!area->contains(c))
        return false;
    const Points &pts = area->points;
    for (size_t i = 0, n = pts.size(); i < n; ++i)
        if (Line::distance_to_squared(c, pts[i], pts[(i + 1) % n]) < r2)
            return false;
    return true;
}

void GCodeWriter::set_extruders(std::vector<unsigned int> extruder_ids)
{
    std::sort(extruder_ids.begin(), extruder_ids.end());
    m_filament_extruders.clear();
    //ORCA: Reset current extruder ID and clear pointers to prevent dangling pointers when extruders are recreated.
    m_curr_extruder_id = -1;
    m_cached_extruder_idx = 0;
    std::fill(m_curr_filament_extruder.begin(), m_curr_filament_extruder.end(), nullptr);
    m_filament_extruders.reserve(extruder_ids.size());
    for (unsigned int extruder_id : extruder_ids)
        m_filament_extruders.emplace_back(Extruder(extruder_id, &this->config, config.single_extruder_multi_material.value));

    /*  we enable support for multiple extruder if any extruder greater than 0 is used
        (even if prints only uses that one) since we need to output Tx commands
        first extruder has index 0 */
    //ORCA: Fix undefined behavior by checking if the vector is empty before taking max_element.
    this->multiple_extruders = !extruder_ids.empty() && (*std::max_element(extruder_ids.begin(), extruder_ids.end())) > 0;
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
    if (!this->multiple_extruders || m_single_extruder_multi_material) {
        tool = -1;
    } else {
        // Every caller of this overload addresses filaments by LOGICAL id, but M104/M109
        // name a physical heater -- so translate at the one point they all pass through.
        // The static overload below is already physical-in (GCode.cpp:5952,
        // GCode/GCodeProcessor.cpp:1410) and must not be remapped, which is why the
        // translation lives here and not there.
        tool = imex_physical_heater_for(this->config.is_imex.value, this->config.physical_extruder_map, tool);
    }
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

#define EXTRUDER_LIMIT(OPT) \
    (filament() ? ((OPT).size() <= filament()->extruder_id() ? 0 : (OPT)[filament()->extruder_id()]) : \
                  ((OPT).empty() ? 0 : *std::max_element((OPT).cbegin(), (OPT).cend())))

// copied from PrusaSlicer
std::string GCodeWriter::set_acceleration_internal(Acceleration type, unsigned int acceleration)
{
    // Clamp the acceleration to the allowed maximum.
    if (type == Acceleration::Print && EXTRUDER_LIMIT(m_max_acceleration) > 0 && acceleration > EXTRUDER_LIMIT(m_max_acceleration))
        acceleration = EXTRUDER_LIMIT(m_max_acceleration);
    if (type == Acceleration::Travel && EXTRUDER_LIMIT(m_max_travel_acceleration) > 0 && acceleration > EXTRUDER_LIMIT(m_max_travel_acceleration))
        acceleration = EXTRUDER_LIMIT(m_max_travel_acceleration);

    // Are we setting travel acceleration for a flavour that supports separate travel and print acc?
    bool separate_travel = (type == Acceleration::Travel && supports_separate_travel_acceleration(this->config.gcode_flavor));

    auto& last_value = separate_travel ? m_last_travel_acceleration : m_last_acceleration ;
    if (acceleration == 0 || acceleration == last_value)
        return std::string();

    last_value = acceleration;

    const std::string value = std::to_string(acceleration);
    std::string       gcode;
    if (FLAVOR_IS(gcfRepetier)) {
        gcode += separate_travel ? "M202 X" : "M201 X";
        gcode += value;
        gcode += " Y";
        gcode += value;
    } else if (FLAVOR_IS(gcfRepRapFirmware) || FLAVOR_IS(gcfMarlinFirmware)) {
        gcode += separate_travel ? "M204 T" : "M204 P";
        gcode += value;
    } else if (FLAVOR_IS(gcfKlipper)) {
        gcode.reserve(128);
        gcode += "SET_VELOCITY_LIMIT ACCEL=";
        gcode += value;
        if (this->config.accel_to_decel_enable) {
            append_klipper_accel_to_decel(gcode, acceleration, this->config.accel_to_decel_factor.value);
            if (GCodeWriter::full_gcode_comment)
                gcode += " ; adjust ACCEL_TO_DECEL";
        }
    } else {
        gcode += "M204 S";
        gcode += value;
    }

    if (GCodeWriter::full_gcode_comment) gcode += " ; adjust acceleration";
    gcode += "\n";

    return gcode;
}

void GCodeWriter::append_klipper_accel_to_decel(std::string &gcode, unsigned int acceleration, double accel_to_decel_factor)
{
    gcode += " ACCEL_TO_DECEL=";
    gcode += float_to_string_decimal_point(acceleration * accel_to_decel_factor / 100);
    gcode += " MINIMUM_CRUISE_RATIO=";
    gcode += float_to_string_decimal_point(1. - accel_to_decel_factor / 100);
}

std::string GCodeWriter::set_jerk_xy(double jerk)
{
    if (jerk < 0.01 || is_approx(jerk, m_last_jerk))
        return std::string();
    
    m_last_jerk = jerk;

    std::ostringstream gcode;
    if (FLAVOR_IS(gcfKlipper)) {
        // Clamp the jerk to the allowed maximum.
        if (EXTRUDER_LIMIT(m_max_jerk_x) > 0 && jerk > EXTRUDER_LIMIT(m_max_jerk_x))
            jerk = EXTRUDER_LIMIT(m_max_jerk_x);
        if (EXTRUDER_LIMIT(m_max_jerk_y) > 0 && jerk > EXTRUDER_LIMIT(m_max_jerk_y))
            jerk = EXTRUDER_LIMIT(m_max_jerk_y);
        
        gcode << "SET_VELOCITY_LIMIT SQUARE_CORNER_VELOCITY=" << jerk;
        
    } else if (FLAVOR_IS(gcfRepetier)) {
        // Repetier uses M207 for temporary Jerk and combines X/Y into a single 'X' parameter.
        double jerk_xy = jerk;
        
        // Clamp against the X machine limit
        if (EXTRUDER_LIMIT(m_max_jerk_x) > 0 && jerk_xy > EXTRUDER_LIMIT(m_max_jerk_x))
            jerk_xy = EXTRUDER_LIMIT(m_max_jerk_x);
            
        // Clamp against the Y machine limit as well to be safe
        if (EXTRUDER_LIMIT(m_max_jerk_y) > 0 && jerk_xy > EXTRUDER_LIMIT(m_max_jerk_y))
            jerk_xy = EXTRUDER_LIMIT(m_max_jerk_y);
            
        // Output the lowest safe limit using ONLY the X parameter
        gcode << "M207 X" << jerk_xy;
    } else {
        double jerk_x = jerk;
        double jerk_y = jerk;
        // Clamp the axis jerk to the allowed maximum.
        if (EXTRUDER_LIMIT(m_max_jerk_x) > 0 && jerk > EXTRUDER_LIMIT(m_max_jerk_x))
            jerk_x = EXTRUDER_LIMIT(m_max_jerk_x);
        if (EXTRUDER_LIMIT(m_max_jerk_y) > 0 && jerk > EXTRUDER_LIMIT(m_max_jerk_y))
            jerk_y = EXTRUDER_LIMIT(m_max_jerk_y);
        
        gcode << "M205 X" << jerk_x << " Y" << jerk_y;
    }
    //the is_bbl check should be in the else statement above so that it doesn't inadverently added Z & E to klipper  
    if (m_is_bbl_printers)
        gcode << std::setprecision(2) << " Z" << EXTRUDER_LIMIT(m_max_jerk_z) << " E" << EXTRUDER_LIMIT(m_max_jerk_e);

    if (GCodeWriter::full_gcode_comment) gcode << " ; adjust jerk";
    gcode << "\n";

    return gcode.str();

}

std::string GCodeWriter::set_accel_and_jerk(unsigned int acceleration, double jerk)
{
    // Only Klipper supports setting acceleration and jerk at the same time. Throw an error if we try to do this on other flavours.
    if(FLAVOR_IS_NOT(gcfKlipper))
        throw std::runtime_error(_u8L("set_accel_and_jerk() is only supported by Klipper"));

    // Clamp the acceleration to the allowed maximum.
    if (EXTRUDER_LIMIT(m_max_acceleration) > 0 && acceleration > EXTRUDER_LIMIT(m_max_acceleration))
        acceleration = EXTRUDER_LIMIT(m_max_acceleration);
    
    // Clamp the jerk to the allowed maximum.
    if (EXTRUDER_LIMIT(m_max_jerk_x) > 0 && jerk > EXTRUDER_LIMIT(m_max_jerk_x))
        jerk = EXTRUDER_LIMIT(m_max_jerk_x);
    if (EXTRUDER_LIMIT(m_max_jerk_y) > 0 && jerk > EXTRUDER_LIMIT(m_max_jerk_y))
        jerk = EXTRUDER_LIMIT(m_max_jerk_y);

    const bool set_acceleration = acceleration != 0 && acceleration != m_last_acceleration;
    const bool set_jerk         = jerk > 0.01 && !is_approx(jerk, m_last_jerk);
    if (!set_acceleration && !set_jerk)
        return std::string();

    std::string gcode;
    gcode.reserve(128);
    gcode += "SET_VELOCITY_LIMIT";
    if (set_acceleration) {
        gcode += " ACCEL=";
        gcode += std::to_string(acceleration);
        if (this->config.accel_to_decel_enable)
            append_klipper_accel_to_decel(gcode, acceleration, this->config.accel_to_decel_factor.value);
        m_last_acceleration = acceleration;
    }
    if (set_jerk) {
        gcode += " SQUARE_CORNER_VELOCITY=";
        gcode += float_to_string_decimal_point(jerk);
        m_last_jerk = jerk;
    }

    if (GCodeWriter::full_gcode_comment)
        gcode += " ; adjust VELOCITY_LIMIT(accel/jerk)";
    gcode += "\n";

    return gcode;

}

std::string GCodeWriter::set_junction_deviation(double junction_deviation){
    std::ostringstream gcode;
    if (FLAVOR_IS(gcfMarlinFirmware) && EXTRUDER_LIMIT(m_max_junction_deviation) > 0 && junction_deviation > 0) {
        // Clamp the junction deviation to the allowed maximum.
        gcode << "M205 J";
        if (junction_deviation <= EXTRUDER_LIMIT(m_max_junction_deviation)) {
            gcode << std::fixed << std::setprecision(3) << junction_deviation;
        } else {
            gcode << std::fixed << std::setprecision(3) << EXTRUDER_LIMIT(m_max_junction_deviation);
        }
        if (GCodeWriter::full_gcode_comment) {
            gcode << " ; Junction Deviation";
        }
        gcode << "\n";
    }
    return gcode.str();
}

std::string GCodeWriter::set_pressure_advance(double pa, int tool) const
{
    std::ostringstream gcode;
    if (pa < 0)
        return gcode.str();
    if (m_is_bbl_printers) {
        // SoftFever: set L1000 to use linear model
        gcode << "M900 K" << std::setprecision(4) << pa << " L1000 M10 ; Override pressure advance value\n";
    } else if (FLAVOR_IS(gcfKlipper)) {
        // Klipper routes PA by extruder name, not active tool.
        // Convention: first extruder is "extruder", subsequent are "extruder1", "extruder2", etc.
        gcode << "SET_PRESSURE_ADVANCE ADVANCE=" << std::setprecision(4) << pa;
        if (tool > 0)
            gcode << " EXTRUDER=extruder" << tool;
        else if (tool == 0)
            gcode << " EXTRUDER=extruder";
        gcode << "; Override pressure advance value\n";
    } else if (FLAVOR_IS(gcfRepRapFirmware)) {
        // RRF: M572 D<n> targets a specific extruder; a bare M572 applies to whatever tool is
        // currently selected and errors when there isn't one. Callers with no tool index keep
        // the historical D0 rather than the bare form: PA would otherwise depend on
        // tool-selection state for every RRF user, none of whom are asking for IMEX.
        gcode << "M572 D" << (tool >= 0 ? tool : 0)
              << " S" << std::setprecision(4) << pa << "; Override pressure advance value\n";
    } else if (FLAVOR_IS(gcfRepetier)) {
        // Repetier M233: X is quadratic (K), Y is linear (L).
        // Applying the value to both parameters simultaneously.
        gcode << "M233 X" << std::setprecision(4) << pa << " Y" << std::setprecision(4) << pa << " ; Override pressure advance value\n";
    } else if (FLAVOR_IS(gcfMarlinFirmware)) {
        // Marlin 2.x supports T parameter for per-extruder LA
        gcode << "M900 K" << std::setprecision(4) << pa;
        if (tool >= 0)
            gcode << " T" << tool;
        gcode << "; Override pressure advance value\n";
    } else {
        // Marlin Legacy and everything else: single-extruder M900, no tool parameter
        gcode << "M900 K" << std::setprecision(4) << pa << "; Override pressure advance value\n";
    }
    return gcode.str();
}

// Orca: input shaping support
std::string GCodeWriter::set_input_shaping(char axis, float damp, float freq, std::string type) const
{
    bool disable = type == "Disable";
    if (disable){
        freq = 0.0f;
        damp = 0.0f;
        axis = 'A';
        type = "Default";
    } else if (freq < 0.0f || damp < 0.f || damp > 1.0f || (axis != 'X' && axis != 'Y' && axis != 'Z' && axis != 'A')) { // A = all axis
        throw std::runtime_error("Invalid input shaping parameters: axis=" + std::string(1, axis) + ", freq=" + std::to_string(freq) + ", damp=" + std::to_string(damp));
    }
    std::ostringstream gcode;
    std::ostringstream params;
    switch (this->config.gcode_flavor) {
    case gcfKlipper: {
        if (!type.empty() && type != "Default") {
            params << " SHAPER_TYPE=" << type;
        }
        if (axis != 'A')
        {
            if (freq > 0.0f) {
                params << " SHAPER_FREQ_" << axis << "=" << std::fixed << std::setprecision(2) << freq;
            }
            if (damp > 0.0f) {
                params << " DAMPING_RATIO_" << axis << "=" << std::fixed << std::setprecision(3) << damp;
            }
        } else {
            if (freq > 0.0f || disable) {
                params << " SHAPER_FREQ_X=" << std::fixed << std::setprecision(2) << freq << " SHAPER_FREQ_Y=" << std::fixed << std::setprecision(2) << freq;
            }
            if (damp > 0.0f || disable) {
                params << " DAMPING_RATIO_X=" << std::fixed << std::setprecision(3) << damp << " DAMPING_RATIO_Y=" << std::fixed << std::setprecision(3) << damp;
            }
        }
        if (!params.str().empty()) {
            gcode << "SET_INPUT_SHAPER" << params.str();
        }
        break;
    }
    case gcfRepRapFirmware: {
        if (!type.empty() && type != "Default" && type != "DAA") {
            params << " P\"" << type << "\"";
        }
        if (freq > 0.0f || disable) {
            params << " F" << std::fixed << std::setprecision(2) << freq;
        }
        if (damp > 0.0f || disable) {
            params << " S" << std::fixed << std::setprecision(3) << damp;
        }
        if (!params.str().empty()) {
            gcode << "M593" << params.str();
        }
        break;
    }
    case gcfMarlinFirmware: {
        if (axis != 'A') {
            params << " " << axis;
        }
        if (freq > 0.0f || disable) {
            params << " F" << std::fixed << std::setprecision(2) << freq;
        }
        if (damp > 0.0f || disable) {
            params << " D" << std::fixed << std::setprecision(3) << damp;
        }
        if (!params.str().empty()) {
            gcode << "M593" << params.str();
        }
        break;
    }
    case gcfMarlinLegacy: {
        throw std::runtime_error(_u8L("Input shaping is not supported by Marlin < 2.1.2.\nCheck your firmware version and update your G-code flavor to ´Marlin 2´."));
    }
    default:
        throw std::runtime_error(_u8L("Input shaping is only supported by Klipper, RepRapFirmware and Marlin 2."));
    }
    if (!gcode.str().empty()) {
        if (GCodeWriter::full_gcode_comment) {
            gcode << " ; Override input shaping";
        }
        gcode << "\n";
    }
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
    // Orca: the manual-filament-change tag must stay ahead of the flavor selection so
    // MMU manual-change handling keeps working.
    if (config.manual_filament_change)
        return ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Manual_Tool_Change) + "T";
    return FLAVOR_IS(gcfMakerWare) ? "M135 T" :
           FLAVOR_IS(gcfSailfish)  ? "M108 T" : "T";
}

std::string GCodeWriter::toolchange(unsigned int filament_id, int nozzle_id)
{
    // set the new extruder
    auto filament_extruder_iter = Slic3r::lower_bound_by_predicate(m_filament_extruders.begin(), m_filament_extruders.end(), [filament_id](const Extruder &e) { return e.id() < filament_id; });
    assert(filament_extruder_iter != m_filament_extruders.end() && filament_extruder_iter->id() == filament_id);
    m_curr_extruder_id = filament_extruder_iter->extruder_id();
    m_curr_filament_extruder[m_curr_extruder_id] = &*filament_extruder_iter;
    m_cached_extruder_idx = get_extruder_index(this->config, filament_id);

    // return the toolchange command
    // if we are running a single-extruder setup, just set the extruder and return nothing
    std::ostringstream gcode;
    // Orca: also emit for non-BBL single-extruder multi-filament setups (MMU-style).
    if (this->multiple_extruders || (this->config.filament_diameter.values.size() > 1 && !is_bbl_printers())) {
        // Orca: manual filament change keeps its tag line even on BBL machines, so the
        // M1020 form must not shadow it. nozzle_id is signed: the null-safe nozzle
        // lookup legitimately yields -1 ("no specific nozzle"), matching the literal
        // H-1 the stock change templates emit; an unsigned would wrap.
        if (m_is_bbl_printers && !config.manual_filament_change)
            gcode << "M1020 S" << filament_id << " H" << nozzle_id;
        else
            gcode << this->toolchange_prefix() << filament_id;
        if (GCodeWriter::full_gcode_comment)
            gcode << " ; change extruder";
        gcode << "\n";
        gcode << this->reset_e(true);
    }
    return gcode.str();
}

// Current parked-retract length of the filament's extruder, share-aware. m_filament_extruders is
// sorted by id (see toolchange), so a lower_bound lookup finds the entry; unknown filament ids
// degrade to 0 rather than dereferencing end().
double GCodeWriter::get_extruder_retracted_length(const int filament_id)
{
    double res = 0.0;
    auto filament_extruder_iter = Slic3r::lower_bound_by_predicate(m_filament_extruders.begin(), m_filament_extruders.end(),
        [filament_id](const Extruder &e) { return (int) e.id() < filament_id; });
    if (filament_extruder_iter == m_filament_extruders.end() || (int) filament_extruder_iter->id() != filament_id)
        return res;

    if (filament_extruder_iter->is_share_extruder())
        res = filament_extruder_iter->get_share_retracted_length();
    else
        res = filament_extruder_iter->get_single_retracted_length();

    return res;
}

std::string GCodeWriter::set_speed(double F, const std::string &comment, const std::string &cooling_marker)
{
    std::string gcode;
    this->set_speed(gcode, F, comment, cooling_marker);
    return gcode;
}

void GCodeWriter::set_speed(std::string &out, double F, const std::string &comment, const std::string &cooling_marker)
{
    assert(F > 0.);
    assert(F < 100000.);
    
    m_current_speed = F;
    GCodeG1Formatter w(config.high_precision_gcode.value);
    w.emit_f(F);
    //BBS
    w.emit_comment(GCodeWriter::full_gcode_comment, comment);
    w.emit_string(cooling_marker);
    w.append_to(out);
}

std::string GCodeWriter::travel_to_xy(const Vec2d &point, const std::string &comment)
{
    m_pos(0) = point(0);
    m_pos(1) = point(1);

    this->set_current_position_clear(true);
    //BBS: take plate offset into consider
    Vec2d point_on_plate = { point(0) - m_x_offset, point(1) - m_y_offset };

    GCodeG1Formatter w(high_precision());
    if (has_axis_remap()) {
        // BELT_MOD: the mapping may couple XY with Z; emit full XYZ in machine coordinates.
        w.emit_xyz(this->apply_axis_remap(Vec3d(point_on_plate.x(), point_on_plate.y(), m_pos.z())));
    } else {
        w.emit_xy(point_on_plate);
    }
    const Vec3d first_layer_probe(point.x(), point.y(), m_pos.z());
    auto speed = this->travel_xy_speed(true, &first_layer_probe);
    w.emit_f(speed * 60.0);
    //BBS
    w.emit_comment(GCodeWriter::full_gcode_comment, comment);
    return w.string();
}

double GCodeWriter::travel_xy_speed(bool first_layer_aware, const Vec3d *bed_point) const
{
    if (m_travel_speed_override > 0.)
        return m_travel_speed_override;
    // BELT_MOD: with a bed-frame destination, the contact-aware first-layer band decides (it falls
    // back to the per-layer flag when no band is set).
    const bool first_layer = bed_point != nullptr ? this->point_on_first_layer(*bed_point) : m_is_first_layer;
    return first_layer_aware && first_layer
        ? this->config.get_abs_value_at("initial_layer_travel_speed", m_cached_extruder_idx) : this->config.travel_speed.get_at(m_cached_extruder_idx);
}
//HEAVILY UNLAYERED_MOD

/*  If this method is called more than once before calling unlift(),
it will not perform subsequent lifts, even if Z was raised manually
(i.e. with travel_to_z()) and thus _lifted was reduced. */
std::string GCodeWriter::lazy_lift(LiftType lift_type, bool spiral_vase)
{
    // UNLAYERED_MOD — only a straight lift is safe when printing non-planar. A lift works on m_pos,
    // which is the MACHINE-frame position, so NormalLift rises along machine Z and is correct under
    // any table tilt. Spiral and slope lifts sweep in XY while they climb, which on a tilted chunk
    // drives the nozzle sideways into the material it has just laid; the spiral additionally emits
    // G2/G3, which cannot describe a non-planar path at all, and its printable-area test is flat.
    // Clamped here rather than in GCode::to_lift_type() because several sites pick SpiralLift or
    // SlopeLift directly and never consult z_hop_types (GCode.cpp ~1135, ~6022, ~7663, ~9613,
    // ~9900, ~9934, ~10407) -- lazy_lift/eager_lift are the only choke points every lift passes.
    // BELT_MOD: a kinematics mapping can force the same (m_force_normal_lift).
    if (m_nonplanar || m_force_normal_lift)
        lift_type = LiftType::NormalLift;
    // check whether the above/below conditions are met
    double target_lift = 0;
    {
        //BBS
        // The filament's column, like Extruder::z_hop(): a filament can override the printer's lift limits.
        const int config_index = filament()->config_index();
        double above = this->config.retract_lift_above.get_at(config_index);
        double below = this->config.retract_lift_below.get_at(config_index);
        if (m_pos.z() >= above && (m_pos.z() <= below || below == 0.))
            target_lift = this->config.z_hop.get_at(config_index);
    }
    // BBS
    if (m_lifted == 0 && m_to_lift == 0 && target_lift > 0) {
        if (spiral_vase) {
            if (this->must_skip_lift_now())
                // Record no lift, so a later unlift() does not descend from a height that was
                // never commanded.
                return "";
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
    // UNLAYERED_MOD — see lazy_lift(): straight lift only when printing non-planar (or when the
    // kinematics mapping forces it, BELT_MOD).
    const LiftType effective_type = (m_nonplanar || m_force_normal_lift) ? LiftType::NormalLift : type;
    std::string lift_move;
    double target_lift = 0;
    {
        //BBS
        // The filament's column, like Extruder::z_hop(): a filament can override the printer's lift limits.
        const int config_index = filament()->config_index();
        double above = this->config.retract_lift_above.get_at(config_index);
        double below = this->config.retract_lift_below.get_at(config_index);
        if (m_pos.z() >= above && (m_pos.z() <= below || below == 0.))
            target_lift = this->config.z_hop.get_at(config_index);
    }

    // BBS: spiral lift only safe with known position
    if (effective_type == LiftType::SpiralLift && this->is_current_position_clear()) {
        double radius = target_lift / (2 * PI * atan(filament()->travel_slope()));
        // static spiral alignment when no move in x,y plane.
        // spiral centra is a radius distance to the right (y=0)
        Vec2d ij_offset = { radius, 0 };
        // Orca: keep the spiral inside the active extruder's printable area, otherwise
        // fall back to a normal lift to avoid colliding with the print boundary. m_pos
        // includes the plate offset, so remove it to match the printable area coordinates.
        const Vec2d spiral_center = { m_pos.x() - m_x_offset + ij_offset.x(), m_pos.y() - m_y_offset + ij_offset.y() };
        if (target_lift > 0 && this->spiral_lift_fits_printable_area(spiral_center, radius)) {
            lift_move = this->_spiral_travel_to_z(m_pos(2) + target_lift, ij_offset, "spiral lift Z");
        } else if (target_lift > 0) {
            lift_move = _travel_to_z(m_pos(2) + target_lift, "normal lift Z");
        }
    }
    //BBS: if position is unknown use normal lift
    else if (target_lift > 0) {
        if (this->must_skip_lift_now())
            // Skipped, not deferred: leave m_lifted at zero below so unlift() does not descend
            // from a height that was never commanded.
            target_lift = 0.;
        else
            lift_move = _travel_to_z(m_pos(2) + target_lift, "normal lift Z");
    }
    m_lifted = target_lift;
    m_to_lift = 0;
    return lift_move;
}

std::string GCodeWriter::travel_to_xyz(const Vec3d &point, const std::string &comment, bool force_z, Transform3d trafo)
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

    Vec3d dest_point = apply_test_rotation2(point, trafo, this->extruder_offset_xy());
    auto travel_speed = this->travel_xy_speed(true, &dest_point);
    //BBS: a z_hop need to be handle when travel
    if (std::abs(m_to_lift) > EPSILON) {
        assert(std::abs(m_lifted) < EPSILON);
        //BBS: don't need to do real lift if the current position is absolutely same with target.
        //This ususally happens when the last extrusion line is short and the end of wipe position
        //is same with the traget point by chance.
        //UNLAYERED_MOD
        /*if ((!this->is_current_position_clear() || m_pos != dest_point) &&
            m_to_lift + m_pos(2) > point(2)) {
            m_lifted = m_to_lift + m_pos(2) - point(2);
            dest_point(2) = m_to_lift + m_pos(2);
        }*/
        Vec3d liftVec = {0, 0, m_to_lift};

        liftVec = trafo.rotation() * liftVec;
        

        m_to_lift = 0.;

        std::string slop_move;
        //BBS: minus plate offset
        Vec3d source = { m_pos(0) - m_x_offset, m_pos(1) - m_y_offset, m_pos(2) };
        Vec3d target = { dest_point(0) - m_x_offset, dest_point(1) - m_y_offset, dest_point(2) };
        Vec3d delta = target - source;
        Vec2d delta_no_z = { delta(0), delta(1) };
        // UNLAYERED_MOD: the BBS spiral/slope lift variants are not used here — the hop is taken
        // along the chunk's plane normal (liftVec), so it is always a plain XYZ move.
        //
        // UNLAYERED_MOD: only lift in place when we actually know where the head is. After custom
        // start / tool-change / timelapse G-code the writer's position is fictional — m_pos is still
        // the origin — so this move commands an X/Y the head was never at. With a plate offset
        // applied that lands a whole plate stride off the bed: plate 2 of a multi-plate project on a
        // 250x210 bed emits "G1 X-300 Y0", which RepRapFirmware 3.7 rejects as outside the machine
        // limits and abandons the print (3.6 silently clamped it, which is why this went unnoticed).
        // Skipping it is safe — the travel below rises to target + liftVec regardless, so the head
        // still clears the part. Same rule as eager_lift's "spiral lift only safe with known position".
        // This also covers BELT_MOD's must_skip_lift_now() (a remap at an unknown position).
        if (this->is_current_position_clear()) {
            GCodeG1Formatter w0(config.high_precision_gcode.value);
            w0.emit_xyz(this->apply_axis_remap(source + liftVec));
            w0.emit_f(travel_speed * 60.0);
            // BBS
            w0.emit_comment(GCodeWriter::full_gcode_comment, comment);
            slop_move = w0.string();
        }
        //added this back to deal with z hop issue for now
        std::string xy_z_move;
        {
            GCodeG1Formatter w0(config.high_precision_gcode.value);
            w0.emit_xyz(this->apply_axis_remap(target + liftVec));
            w0.emit_f(travel_speed * 60.0);
            w0.emit_comment(GCodeWriter::full_gcode_comment, comment);

            GCodeG1Formatter w1(config.high_precision_gcode.value);
            w1.emit_xyz(this->apply_axis_remap(target));
            w1.emit_f(travel_speed * 60.0);
            w1.emit_comment(GCodeWriter::full_gcode_comment, comment);

            xy_z_move = w0.string() + w1.string();
        }
        m_pos = dest_point;
        this->set_current_position_clear(true);
        return slop_move + xy_z_move;

    } else if (!force_z && !this->will_move_z(dest_point(2))) {
        double nominal_z = m_pos(2) - m_lifted;
        m_lifted -= (dest_point(2) - nominal_z);
        // In case that z_hop == layer_height we could end up with almost zero in_m_lifted
        // and a retract could be skipped
        if (std::abs(m_lifted) < EPSILON)
            m_lifted = 0.;
        //BBS
        this->set_current_position_clear(true);
        return this->travel_to_xy(to_2d(dest_point));
    }
    else {
        /*  In all the other cases, we perform an actual XYZ move and cancel
            the lift. */

        m_lifted = 0;
    }

    //BBS: take plate offset into consider
    Vec3d point_on_plate = { dest_point(0) - m_x_offset, dest_point(1) - m_y_offset, dest_point(2) };
    std::string out_string;
    GCodeG1Formatter w(config.high_precision_gcode.value);

    // UNLAYERED_MOD — approach a ROTATED frame's target along the chunk normal.
    //
    // The xy-then-z split below is a clearing manoeuvre: travel at the hop height, then come down.
    // That works in an unrotated frame, where the head already sits at the layer's height so the XY
    // leg travels level. In a rotated frame a layer is a tilted plane whose emitted Z varies across
    // it, so the start of one layer can sit MILLIMETRES above the end of the last (measured 5.1 mm
    // on a 20-degree segment). The XY leg then crosses the part at the lower height and the hop
    // never clears anything -- the hotend collision check caught exactly that: tip at Z10.87 with
    // earlier material 4.5 mm directly overhead, one hit per layer change.
    //
    // Branch A above already does the right thing (hop along the normal, traverse, descend) but
    // only fires when a lift is PENDING (m_to_lift). Nothing is pending at a layer change, so this
    // tail runs instead. Do the same manoeuvre here: arrive ABOVE the target along the normal, then
    // descend onto it -- still two moves, and both absolute, so it does not depend on m_pos being
    // known (which is precisely why the split branch exists). The hop is added and then taken back
    // off, so it cancels exactly instead of leaving the head displaced along Z only.
    //
    // Skipped when z_hop is 0 (no clearance asked for) and in an unrotated frame, so planar output
    // is unchanged.
    Vec3d approach_vec = Vec3d::Zero();
    if (! trafo.linear().isApprox(Eigen::Matrix3d::Identity())) {
        double hop = 0.;
        if (const Extruder *e = this->filament(); e != nullptr)
            hop = this->config.z_hop.get_at(e->id());
        if (hop > EPSILON)
            approach_vec = trafo.rotation() * Vec3d(0., 0., hop);
    }
    if (! approach_vec.isZero()) {
        GCodeG1Formatter w0(config.high_precision_gcode.value);
        w0.emit_xyz(this->apply_axis_remap(point_on_plate + approach_vec));
        w0.emit_f(travel_speed * 60.0);
        w0.emit_comment(GCodeWriter::full_gcode_comment, comment);
        GCodeG1Formatter w1(config.high_precision_gcode.value);
        w1.emit_xyz(this->apply_axis_remap(point_on_plate));
        w1.emit_f(travel_speed * 60.0);
        w1.emit_comment(GCodeWriter::full_gcode_comment, comment);
        out_string = w0.string() + w1.string();
        m_lifted = 0;
        m_pos    = dest_point;
        this->set_current_position_clear(true);
        return out_string;
    }

    if (has_axis_remap()) {
        // BELT_MOD: the mapping may couple XY with Z; emit full XYZ in machine coordinates.
        w.emit_xyz(this->apply_axis_remap(point_on_plate));
        w.emit_f(this->config.travel_speed.get_at(m_cached_extruder_idx) * 60.0);
        w.emit_comment(GCodeWriter::full_gcode_comment, comment);
        out_string = w.string();
    } else if (!this->is_current_position_clear())
    {
        //force to move xy first then z after filament change
        w.emit_xy(Vec2d(point_on_plate.x(), point_on_plate.y()));
        w.emit_f(this->travel_xy_speed(false) * 60.0);
        w.emit_comment(GCodeWriter::full_gcode_comment, comment);
        out_string = w.string() + _travel_to_z(point_on_plate.z(), comment);
    } else {
        GCodeG1Formatter w(config.high_precision_gcode.value);
        w.emit_xyz(point_on_plate);
        w.emit_f(this->travel_xy_speed(false) * 60.0);
        w.emit_comment(GCodeWriter::full_gcode_comment, comment);
        out_string = w.string();
    }

    m_pos = dest_point;
    this->set_current_position_clear(true);
    return out_string;
}

// UNLAYERED_MOD — see header.
double GCodeWriter::_clamp_feed_for_rotary(double linear_dist, double bc_angular_dist_deg, double travel_feed) const
{
    static constexpr double ROT_FEED = 5000.; // deg/min; matches trafo_to_rotation_gcode
    if (bc_angular_dist_deg <= 1e-3 || linear_dist <= 0.)
        return travel_feed;
    return std::min(travel_feed, linear_dist * ROT_FEED / bc_angular_dist_deg);
}

// UNLAYERED_MOD — see header. The point is already in the bed frame; emit it verbatim.
std::string GCodeWriter::travel_to_xyz_direct(const Vec3d &bed_point, const std::string &comment)
{
    const double travel_speed = this->travel_xy_speed();

    GCodeG1Formatter w(config.high_precision_gcode.value);
    w.emit_xyz(this->apply_axis_remap(Vec3d(bed_point(0) - m_x_offset, bed_point(1) - m_y_offset, bed_point(2))));
    w.emit_f(travel_speed * 60.0);
    w.emit_comment(GCodeWriter::full_gcode_comment, comment);

    m_pos     = bed_point;
    m_lifted  = 0.;
    m_to_lift = 0.;
    this->set_current_position_clear(true);
    return w.string();
}

// UNLAYERED_MOD — see header. F is emitted so a G1-style interpretation of G0 does not inherit a
// stale print feed; a firmware that treats G0 as a true rapid ignores it.
std::string GCodeWriter::rapid_to_xyzbc(const Vec3d &bed_point, std::optional<double> angle_b_deg, std::optional<double> angle_c_deg,
                                        const std::string &comment)
{
    const double travel_speed = this->travel_xy_speed();

    GCodeG0Formatter w(config.high_precision_gcode.value);
    w.emit_xyz(this->apply_axis_remap(Vec3d(bed_point(0) - m_x_offset, bed_point(1) - m_y_offset, bed_point(2))));
    if (angle_b_deg)
        w.emit_axis('B', *angle_b_deg, GCodeFormatter::xyz_digits(config.high_precision_gcode.value));
    if (angle_c_deg)
        w.emit_axis('C', *angle_c_deg, GCodeFormatter::xyz_digits(config.high_precision_gcode.value));
    w.emit_f(travel_speed * 60.0);
    w.emit_comment(GCodeWriter::full_gcode_comment, comment);

    m_pos     = bed_point;
    m_lifted  = 0.;
    m_to_lift = 0.;
    this->set_current_position_clear(true);
    return w.string();
}

// UNLAYERED_MOD — see header. Traverse at the current machine-vertical height, then descend.
std::string GCodeWriter::travel_to_xyz_high_then_down(const Vec3d &local_point, const Transform3d &trafo, const std::string &comment,
                                                      bool rotate, double angle_b_deg, double angle_c_deg, double bc_angular_dist_deg)
{
    // Destination in machine coordinates (orientation already set by the caller's rotation gcode).
    Vec3d dest = apply_test_rotation2(local_point, trafo, this->extruder_offset_xy());
    double travel_speed = this->travel_xy_speed(true, &dest);
    const double travel_feed = travel_speed * 60.0; // mm/min

    // Keep the current (lifted) machine Z for the XY traverse, then drop straight down. Coordinates
    // are emitted with the plate offset removed, matching travel_to_xyz / _travel_to_z.
    Vec3d over   = { dest(0) - m_x_offset, dest(1) - m_y_offset, m_pos(2) };
    Vec3d target = { dest(0) - m_x_offset, dest(1) - m_y_offset, dest(2) };

    std::string out;

    if (rotate && bc_angular_dist_deg > 1e-3) {
        // ByGroup hop: swing B/C simultaneously with the high XY traverse. The firmware finishes
        // every axis of a G1 together, so a large rotary swing over a short XY hop would whip the
        // B/C axes. F is interpreted over the XY distance (T = d_xy / F), so bounding the rotary
        // speed to ROT_FEED (deg/min) means F <= d_xy * ROT_FEED / d_bc.
        const double rot_feed = 5000.0; // matches the standalone trafo_to_rotation_gcode feedrate
        const double d_xy = std::hypot(over(0) - m_pos(0), over(1) - m_pos(1));
        if (d_xy >= 1.0) {
            const double f = this->_clamp_feed_for_rotary(d_xy, bc_angular_dist_deg, travel_feed);
            GCodeG1Formatter w0(config.high_precision_gcode.value);
            w0.emit_xyz(this->apply_axis_remap(over));
            w0.emit_axis('B', angle_b_deg, GCodeFormatter::xyz_digits(config.high_precision_gcode.value));
            w0.emit_axis('C', angle_c_deg, GCodeFormatter::xyz_digits(config.high_precision_gcode.value));
            w0.emit_f(f);
            w0.emit_comment(GCodeWriter::full_gcode_comment, comment);
            out += w0.string();
        } else {
            // Negligible XY traverse: a combined move would force F ~ 0, so rotate in place at the
            // safe height (bounded to ROT_FEED) and then traverse the small XY remainder.
            GCodeG1Formatter wr(config.high_precision_gcode.value);
            wr.emit_axis('B', angle_b_deg, GCodeFormatter::xyz_digits(config.high_precision_gcode.value));
            wr.emit_axis('C', angle_c_deg, GCodeFormatter::xyz_digits(config.high_precision_gcode.value));
            wr.emit_f(rot_feed);
            wr.emit_comment(GCodeWriter::full_gcode_comment, comment);
            out += wr.string();
            GCodeG1Formatter w0(config.high_precision_gcode.value);
            w0.emit_xyz(this->apply_axis_remap(over));
            w0.emit_f(travel_feed);
            w0.emit_comment(GCodeWriter::full_gcode_comment, comment);
            out += w0.string();
        }
    } else {
        GCodeG1Formatter w0(config.high_precision_gcode.value);
        w0.emit_xyz(this->apply_axis_remap(over));
        w0.emit_f(travel_feed);
        w0.emit_comment(GCodeWriter::full_gcode_comment, comment);
        out += w0.string();
    }

    GCodeG1Formatter w1(config.high_precision_gcode.value);
    w1.emit_xyz(this->apply_axis_remap(target));
    w1.emit_f(travel_feed);
    w1.emit_comment(GCodeWriter::full_gcode_comment, comment);
    out += w1.string();

    m_pos     = dest;
    m_lifted  = 0.;
    m_to_lift = 0.;
    this->set_current_position_clear(true);
    return out;
}

std::pair<double, double> GCodeWriter::planned_travel_z(const Vec3d &point, const bool force_z) const
{
    double destination_z = point.z();
    if (std::abs(m_to_lift) > EPSILON) {
        if ((!this->is_current_position_clear() || m_pos != point) &&
            m_to_lift + m_pos.z() > point.z())
            destination_z = m_to_lift + m_pos.z();
    } else if (!force_z && !this->will_move_z(point.z())) {
        destination_z = m_pos.z();
    }
    return { m_pos.z(), destination_z };
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

    double speed = this->config.travel_speed_z.get_at(m_cached_extruder_idx);
    if (speed == 0.) {
        speed = this->point_on_first_layer(Vec3d(m_pos.x() - m_x_offset, m_pos.y() - m_y_offset, z))
                    ? this->config.get_abs_value_at("initial_layer_travel_speed", m_cached_extruder_idx)
                    : this->config.travel_speed.get_at(m_cached_extruder_idx);
    }

    GCodeG1Formatter w(config.high_precision_gcode.value);
    if (has_axis_remap()) {
        // BELT_MOD: the mapping may make machine Z depend on logical X/Y, so a Z-only word is
        // not enough; re-emit the current logical X/Y through the mapping.
        w.emit_xyz(this->apply_axis_remap(Vec3d(m_pos.x() - m_x_offset, m_pos.y() - m_y_offset, z)));
    } else {
        w.emit_z(z);
    }
    w.emit_f(speed * 60.0);
    //BBS
    w.emit_comment(GCodeWriter::full_gcode_comment, comment);
    return w.string();
}

std::string GCodeWriter::_spiral_travel_to_z(double z, const Vec2d &ij_offset, const std::string &comment)
{
    // BELT_MOD: a spiral is planned in the logical XY plane; under a mapping that must emit
    // every axis it cannot be expressed, so fall back to a plain lift.
    if (has_axis_remap())
        return this->_travel_to_z(z, comment);
    std::string output;
    double speed = this->config.travel_speed_z.get_at(m_cached_extruder_idx);

    if (speed == 0.) {
        speed = m_is_first_layer ? this->config.get_abs_value_at("initial_layer_travel_speed", m_cached_extruder_idx)
                                 : this->config.travel_speed.get_at(m_cached_extruder_idx);
    }

    if (this->config.enable_arc_fitting.value == ArcFittingMode::Disabled) { // Orca: if arc fitting is disabled, approximate the arc with small linear segments
        const double z_start = m_pos(2); // starting Z height

        const double px = m_pos(0) - m_x_offset;        // take plate offset into consideration
        const double py = m_pos(1) - m_y_offset;        // take plate offset into consideration
        const double cx = px + ij_offset(0);            // center x
        const double cy = py + ij_offset(1);            // center y
        const double radius = ij_offset.norm();         // radius

        // Number of linear segments approximating the circle, chosen so that a chord never deviates
        // from the true arc by more than the slicing resolution. A resolution of 0 means "no
        // simplification", which has no finite segment count, so it takes the upper bound.
        constexpr size_t min_segments = 8;              // keep a small spiral visibly round
        constexpr size_t max_segments = 128;            // bound the emitted G-code
        const int segments = int(m_resolution > 0. ?
            std::clamp(Geometry::ArcWelder::arc_discretization_steps(radius, 2. * M_PI, m_resolution), min_segments, max_segments) :
            max_segments);

        const double a0 = std::atan2(py - cy, px - cx); // start angle

        auto emit_point = [this, &output](const Vec3d &point) {
            GCodeG1Formatter w(config.high_precision_gcode.value);
            w.emit_xyz(point);
            output += w.string();
        };

        output.reserve(size_t(segments) * 40);          // ~40 characters per emitted G1 line

        // set feedrate
        GCodeG1Formatter w(config.high_precision_gcode.value);
        w.emit_f(speed * 60.0);
        w.emit_comment(GCodeWriter::full_gcode_comment, comment);
        output += w.string();

        // approximate the arc with small linear segments (without the last point which is added later to ensure exactness)
        for (int i = 1; i < segments; ++i) {
            const double t = double(i) / segments;      // parametric position along arc
            const double a = a0 + 2. * M_PI * t;        // CCW arc param, full circle
            emit_point(Vec3d(cx + radius * std::cos(a), // point on circle
                             cy + radius * std::sin(a),
                             z_start + (z - z_start) * t)); // interpolated Z height
        }

        emit_point(Vec3d(px, py, z));                   // final point to ensure exactness
    } else { // Orca: if arc fitting is enabled emit a G2/G3 command for the spiral lift
        output = std::string("G17") + (full_gcode_comment ? " ; XY plane for arc\n" : "\n");

        GCodeG2G3Formatter w(true, config.high_precision_gcode.value);
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
    std::string gcode;
    this->extrude_to_xy(gcode, point, dE, comment, force_no_extrusion);
    return gcode;
}

void GCodeWriter::extrude_to_xy(std::string &out, const Vec2d &point, double dE, const std::string &comment, bool force_no_extrusion)
{
    m_pos(0) = point(0);
    m_pos(1) = point(1);
    if(std::abs(dE) <= std::numeric_limits<double>::epsilon())
        force_no_extrusion = true;

    if (!force_no_extrusion) {
        filament()->extrude(dE);
        m_max_extrusion_z = std::max(m_max_extrusion_z, m_pos.z());   // UNLAYERED_MOD
    }

    //BBS: take plate offset into consider
    Vec2d point_on_plate = { point(0) - m_x_offset, point(1) - m_y_offset };

    const bool high_precision = this->high_precision();
    GCodeG1Formatter w(high_precision);
    if (has_axis_remap()) {
        // BELT_MOD: the mapping may couple XY with Z; emit full XYZ in machine coordinates.
        w.emit_xyz(this->apply_axis_remap(Vec3d(point_on_plate.x(), point_on_plate.y(), m_pos.z())));
    } else {
        w.emit_xy(point_on_plate);
    }
    if (!force_no_extrusion)
        w.emit_e(filament()->E_to_emit(high_precision));
    //BBS
    w.emit_comment(GCodeWriter::full_gcode_comment, comment);
    w.append_to(out);
}

// Approximate an arc with linear extrusions, for machine mappings that cannot
// express a G2/G3 (see extrude_arc_to_xy). center_offset is I/J: the centre
// relative to the CURRENT position, which is why this must run before m_pos is
// updated.
void GCodeWriter::extrude_arc_as_polyline(std::string &out, const Vec2d &point, const Vec2d &center_offset,
                                          double dE, const bool is_ccw,
                                          const std::string &comment, bool force_no_extrusion)
{
    const Vec2d start  = Vec2d(m_pos.x(), m_pos.y());
    const Vec2d centre = start + center_offset;
    const double r     = (start - centre).norm();
    if (r < EPSILON) {
        // Degenerate: no arc to speak of, so a single move is exact.
        this->extrude_to_xy(out, point, dE, comment, force_no_extrusion);
        return;
    }

    double a0 = std::atan2(start.y() - centre.y(), start.x() - centre.x());
    double a1 = std::atan2(point.y() - centre.y(), point.x() - centre.x());
    double sweep = a1 - a0;
    if (is_ccw) { while (sweep <= 0.) sweep += 2. * PI; }
    else        { while (sweep >= 0.) sweep -= 2. * PI; }

    // Segment count from a chord-deviation bound: r*(1-cos(dtheta/2)) <= tol.
    const double tol  = 0.005;                       // mm
    const double dmax = (tol >= r) ? PI : 2. * std::acos(1. - tol / r);
    const int    n    = std::max(2, int(std::ceil(std::abs(sweep) / std::max(dmax, EPSILON))));

    for (int i = 1; i <= n; ++ i) {
        const double a = a0 + sweep * (double(i) / double(n));
        const Vec2d  p = (i == n) ? point
                                  : Vec2d(centre.x() + r * std::cos(a), centre.y() + r * std::sin(a));
        this->extrude_to_xy(out, p, dE / double(n), i == n ? comment : std::string(), force_no_extrusion);
    }
}

//BBS: generate G2 or G3 extrude which moves by arc
//point is end point which means X and Y axis
//center_offset is I and J axis
std::string GCodeWriter::extrude_arc_to_xy(const Vec2d& point, const Vec2d& center_offset, double dE, const bool is_ccw, const std::string& comment, bool force_no_extrusion)
{
    std::string gcode;
    this->extrude_arc_to_xy(gcode, point, center_offset, dE, is_ccw, comment, force_no_extrusion);
    return gcode;
}

void GCodeWriter::extrude_arc_to_xy(std::string &out, const Vec2d& point, const Vec2d& center_offset, double dE, const bool is_ccw, const std::string& comment, bool force_no_extrusion)
{
    // Arcs emit only X/Y/I/J, so a mapping that moves logical X or Y cannot be
    // expressed as a G2/G3. GCode::should_disable_arc_fitting() normally stops
    // arcs being generated at all for such a mapping, but this is public API, so
    // define the behaviour rather than asserting.
    //
    // This check MUST precede every state mutation below: falling through to
    // extrude_to_xy() after filament()->extrude(dE) would advance E twice.
    //
    // A single chord is not a safe substitute either -- a semicircle would become
    // its diameter and a full circle a stationary blob -- so approximate the arc
    // with linear segments bounded by a chord tolerance, splitting dE between
    // them in proportion to arc length.
    if (! m_kinematics->supports_arc_moves()) {
        this->extrude_arc_as_polyline(out, point, center_offset, dE, is_ccw, comment, force_no_extrusion);
        return;
    }

    m_pos(0) = point(0);
    m_pos(1) = point(1);
    if (!force_no_extrusion) {
        filament()->extrude(dE);
        m_max_extrusion_z = std::max(m_max_extrusion_z, m_pos.z());   // UNLAYERED_MOD
    }

    Vec2d point_on_plate = { point(0) - m_x_offset, point(1) - m_y_offset };

    const bool high_precision = this->high_precision();
    GCodeG2G3Formatter w(is_ccw, high_precision);
    w.emit_xy(point_on_plate);
    w.emit_ij(center_offset);
    if (!force_no_extrusion)
        w.emit_e(filament()->E_to_emit(high_precision));
    //BBS
    w.emit_comment(GCodeWriter::full_gcode_comment, comment);
    w.append_to(out);
}

std::string GCodeWriter::extrude_to_xyz(const Vec3d &point, double dE, const std::string &comment, bool force_no_extrusion, Transform3d trafo, bool is_wipe)
{
    std::string gcode;
    this->extrude_to_xyz(gcode, point, dE, comment, force_no_extrusion, trafo, is_wipe);
    return gcode;
}

void GCodeWriter::extrude_to_xyz(std::string &out, const Vec3d &point, double dE, const std::string &comment, bool force_no_extrusion, Transform3d trafo, bool is_wipe)
{
    // Check if Z actually changes (at export precision) before emitting it.
    // ZAA sloped extrusions call this for every segment, but many consecutive
    // segments share the same quantized Z — emitting it every time is redundant.
    // Compare in the MACHINE frame: `point` is slice-frame, m_pos is the last machine position.
    const bool  high_precision = this->high_precision();
    const Vec3d new_pos        = apply_test_rotation2(point, trafo, this->extruder_offset_xy());
    const bool  z_changed      = (GCodeFormatter::quantize_xyz(new_pos(2), high_precision) != GCodeFormatter::quantize_xyz(m_pos(2), high_precision));

    m_pos = new_pos;
    // Machine-space floor check. This invariant used to live in GCode::_extrude, where it tested
    // m_nominal_z + z_diff -- SLICE-frame z, which equals the nozzle height only for planar prints.
    // A cut-plane segment slices in its own rebased frame, where a negative z is both legitimate
    // and meaningless as a height; the un-rotation into machine coordinates happens on the line
    // above. Testing m_pos.z() applies the same 0.1mm invariant to the coordinate actually being
    // written, so planar output is bit-identical while a rotated segment is judged on where the
    // nozzle really goes.
    // Only for moves that actually lay material. The checks this replaced sat on the two extrusion
    // paths in GCode::_extrude, so guarding wipes and pre-travel inward moves here would be new
    // behaviour for ordinary prints -- those legitimately move without extruding, and a policy
    // check does not belong in a motion primitive.
    // UNLAYERED_MOD: require the point to be low in BOTH frames. A legacy layer_rotation_b/c chunk
    // standing tilted on the bed has first-layer wall points at machine z ~ 0 where the tilted slice
    // meets the bottom face; the slice-frame check this replaced accepted those and such prints
    // have been made, so a machine-only test turned every one of them into "GCode: very low z".
    // Planar prints are unchanged (both frames coincide), and a cut-plane segment's rebased frame
    // still passes a legitimately negative slice z as long as the nozzle really is above the bed.
    // UNLAYERED_MOD: the limit scales with a thin first layer (see set_min_extrusion_z); found by
    // fuzzing, every belt print with a 0.1 mm first layer and a brim was refused here.
    if (! force_no_extrusion && ! is_wipe && m_pos.z() < m_min_extrusion_z && point.z() < m_min_extrusion_z)
        // Say where: without the coordinates a refused move cannot be traced back to its path.
        throw RuntimeError((boost::format("GCode: very low z (%1$.3f, %2$.3f, %3$.3f -> %4$.3f, %5$.3f, %6$.3f; floor %7$.3f)")
                            % m_pos.x() % m_pos.y() % m_pos.z() % point.x() % point.y() % point.z() % m_min_extrusion_z).str());
    m_lifted = 0;
    if (!force_no_extrusion) {
        filament()->extrude(dE);
        if (! is_wipe)
            m_max_extrusion_z = std::max(m_max_extrusion_z, m_pos.z());   // UNLAYERED_MOD
    }

    //BBS: take plate offset into consider
    Vec3d point_on_plate = { m_pos(0) - m_x_offset, m_pos(1) - m_y_offset, m_pos(2) };

    GCodeG1Formatter w(high_precision);
    if (has_axis_remap())
        // BELT_MOD: the mapping may couple XY with Z; emit full XYZ in machine coordinates.
        w.emit_xyz(this->apply_axis_remap(point_on_plate));
    else if (z_changed)
        w.emit_xyz(point_on_plate);
    else
        w.emit_xy(Vec2d(point_on_plate.x(), point_on_plate.y()));
    if (!force_no_extrusion)
        w.emit_e(filament()->E_to_emit(high_precision));
    //BBS
    w.emit_comment(GCodeWriter::full_gcode_comment, comment);
    w.append_to(out);
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
            const bool high_precision = this->high_precision();
            GCodeG1Formatter w(high_precision);
            w.emit_e(filament()->E_to_emit(high_precision));
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

std::string GCodeWriter::unretract(float extra_retract)
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
            const bool high_precision = this->high_precision();
            GCodeG1Formatter w(high_precision);
            // extra_retract over-extrudes for the PETG pre-extrusion; 0 by
            // default -> identical to the plain deretract E position.
            w.emit_e(filament()->E_to_emit(high_precision, extra_retract));
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

std::string GCodeWriter::set_fan(const GCodeFlavor gcode_flavor, unsigned int speed, unsigned int part_cooling_fan_min_pwm)
{
    std::ostringstream gcode;
    // ORCA: clamp non-zero fan commands up to the configured PWM floor so fans that can't spool at low duty
    // cycles still start reliably. Zero (fan off) is preserved exactly so disable-fan commands are never altered.
    if (speed > 0 && part_cooling_fan_min_pwm > 0 && speed < part_cooling_fan_min_pwm)
        speed = part_cooling_fan_min_pwm;
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
    // ORCA: pick up the per-printer PWM floor from the active config.
    return GCodeWriter::set_fan(this->config.gcode_flavor, speed,
                                static_cast<unsigned int>(std::max(0, this->config.part_cooling_fan_min_pwm.value)));
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

std::string GCodeWriter::set_exhaust_fan(int speed)
{
    std::ostringstream gcode;
    gcode << "M106" << " P3" << " S" << (int)(speed / 100.0 * 255);

    if (GCodeWriter::full_gcode_comment) {
        if (speed == 0)
            gcode << " ; disable exhaust fan ";
        else
            gcode << " ; enable exhaust fan ";
    }

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
    // Orca: writer-only context (calibration paths) has no nozzle grouping; the
    // filament's own extruder id is the correct degenerate nozzle value.
    return this->need_toolchange(filament_id) ? this->toolchange(filament_id, (int) extruder_id) : "";
}

void GCodeWriter::init_extruder(unsigned int filament_id)
{
    if (m_curr_extruder_id == -1 && filament_id != -1) {
        auto filament_extruder_iter = Slic3r::lower_bound_by_predicate(m_filament_extruders.begin(), m_filament_extruders.end(), [filament_id](const Extruder &e) { return e.id() < filament_id; });
        assert(filament_extruder_iter != m_filament_extruders.end() && filament_extruder_iter->id() == filament_id);
        m_curr_extruder_id = filament_extruder_iter->extruder_id();
        m_curr_filament_extruder[m_curr_extruder_id] = &*filament_extruder_iter;
        m_cached_extruder_idx = get_extruder_index(this->config, filament_id);
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
