#include "BoundingBox.hpp"
#include "Config.hpp"
#include "Polygon.hpp"
#include "PrintConfig.hpp"
#include "libslic3r.h"
#include "I18N.hpp"
#include "GCode.hpp"
#include "Exception.hpp"
#include "ExtrusionEntity.hpp"
#include "EdgeGrid.hpp"
#include "Geometry/ConvexHull.hpp"
#include "GCode/PrintExtents.hpp"
#include "GCode/Thumbnails.hpp"
#include "GCode/WipeTower.hpp"
#include "ShortestPath.hpp"
#include "Print.hpp"
#include "Utils.hpp"
#include "ClipperUtils.hpp"
#include "libslic3r.h"
#include "LocalesUtils.hpp"
#include "libslic3r/format.hpp"
#include "Time.hpp"
#include "GCode/ExtrusionProcessor.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <chrono>
#include <iostream>
#include <math.h>
#include <stdlib.h>
#include <string>
#include <utility>
#include <string_view>

#include <regex>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/find.hpp>
#include <boost/foreach.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/beast/core/detail/base64.hpp>

#include <boost/nowide/iostream.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/nowide/cstdlib.hpp>

#include "SVG.hpp"

#include <tbb/parallel_for.h>
#include "calib.hpp"
// Intel redesigned some TBB interface considerably when merging TBB with their oneAPI set of libraries, see GH #7332.
// We are using quite an old TBB 2017 U7. Before we update our build servers, let's use the old API, which is deprecated in up to date TBB.
#if ! defined(TBB_VERSION_MAJOR)
    #include <tbb/version.h>
#endif
#if ! defined(TBB_VERSION_MAJOR)
    static_assert(false, "TBB_VERSION_MAJOR not defined");
#endif
#if TBB_VERSION_MAJOR >= 2021
    #include <tbb/parallel_pipeline.h>
    using slic3r_tbb_filtermode = tbb::filter_mode;
#else
    #include <tbb/pipeline.h>
    using slic3r_tbb_filtermode = tbb::filter;
#endif

#include <Shiny/Shiny.h>

#include "miniz_extension.hpp"

using namespace std::literals::string_view_literals;

#if 0
// Enable debugging and asserts, even in the release build.
#define DEBUG
#define _DEBUG
#undef NDEBUG
#endif

#include <assert.h>

namespace Slic3r {

    //! macro used to mark string used at localization,
    //! return same string
#define L(s) (s)
#define _(s) Slic3r::I18N::translate(s)

static const float g_min_purge_volume = 100.f;
static const float g_purge_volume_one_time = 135.f;
static const int g_max_flush_count = 4;
static const size_t g_max_label_object = 64;

Vec2d travel_point_1;
Vec2d travel_point_2;
Vec2d travel_point_3;
static std::vector<Vec2d> get_path_of_change_filament(const Print& print)
{
    // give safe value in case there is no start_end_points in config
    std::vector<Vec2d> out_points;
    out_points.emplace_back(Vec2d(54, 0));
    out_points.emplace_back(Vec2d(54, 0));
    out_points.emplace_back(Vec2d(54, 245));

    // get the start_end_points from config (20, -3) (54, 245)
    Pointfs points = print.config().start_end_points.values;
    if (points.size() != 2)
        return out_points;

    Vec2d start_point  = points[0];
    Vec2d end_point    = points[1];

    // the cutter area size(18, 28)
    Pointfs excluse_area = print.config().bed_exclude_area.values;
    if (excluse_area.size() != 4)
        return out_points;

    double cutter_area_x = excluse_area[2].x() + 2;
    double cutter_area_y = excluse_area[2].y() + 2;

    double start_x_position = start_point.x();
    double end_x_position   = end_point.x();
    double end_y_position   = end_point.y();

    bool can_travel_form_left = true;

    // step 1: get the x-range intervals of all objects
    std::vector<std::pair<double, double>> object_intervals;
    for (PrintObject *print_object : print.objects()) {
        const PrintInstances &print_instances = print_object->instances();
        BoundingBoxf3 bounding_box = print_instances[0].model_instance->get_object()->bounding_box_exact();

        if (bounding_box.min.x() < start_x_position && bounding_box.min.y() < cutter_area_y)
            can_travel_form_left = false;

        std::pair<double, double> object_scope = std::make_pair(bounding_box.min.x() - 2, bounding_box.max.x() + 2);
        if (object_intervals.empty())
            object_intervals.push_back(object_scope);
        else {
            std::vector<std::pair<double, double>> new_object_intervals;
            bool intervals_intersect = false;
            std::pair<double, double>              new_merged_scope;
            for (auto object_interval : object_intervals) {
                if (object_interval.second >= object_scope.first && object_interval.first <= object_scope.second) {
                    if (intervals_intersect) {
                        new_merged_scope = std::make_pair(std::min(object_interval.first, new_merged_scope.first), std::max(object_interval.second, new_merged_scope.second));
                    } else { // it is the first intersection
                        new_merged_scope = std::make_pair(std::min(object_interval.first, object_scope.first), std::max(object_interval.second, object_scope.second));
                    }
                    intervals_intersect = true;
                } else {
                    new_object_intervals.push_back(object_interval);
                }
            }

            if (intervals_intersect) {
                new_object_intervals.push_back(new_merged_scope);
                object_intervals = new_object_intervals;
            } else
                object_intervals.push_back(object_scope);
        }
    }

    // step 2: get the available x-range
    std::sort(object_intervals.begin(), object_intervals.end(),
              [](const std::pair<double, double> &left, const std::pair<double, double> &right) {
            return left.first < right.first;
    });
    std::vector<std::pair<double, double>> available_intervals;
    double                                 start_position = 0;
    for (auto object_interval : object_intervals) {
        if (object_interval.first > start_position)
            available_intervals.push_back(std::make_pair(start_position, object_interval.first));
        start_position = object_interval.second;
    }
    available_intervals.push_back(std::make_pair(start_position, 255));

    // step 3: get the nearest path
    double new_path     = 255;
    for (auto available_interval : available_intervals) {
        if (available_interval.first > end_x_position) {
            double distance = available_interval.first - end_x_position;
            new_path        = abs(end_x_position - new_path) < distance ? new_path : available_interval.first;
            break;
        } else {
            if (available_interval.second >= end_x_position) {
                new_path = end_x_position;
                break;
            } else if (!can_travel_form_left && available_interval.second < start_x_position) {
                continue;
            } else {
                new_path     = available_interval.second;
            }
        }
    }

    // step 4: generate path points  (new_path == start_x_position means not need to change path)
    Vec2d out_point_1;
    Vec2d out_point_2;
    Vec2d out_point_3;
    if (new_path < start_x_position) {
        out_point_1 = Vec2d(start_x_position, cutter_area_y);
        out_point_2 = Vec2d(new_path, cutter_area_y);
        out_point_3 = Vec2d(new_path, end_y_position);
    } else {
        out_point_1 = Vec2d(new_path, 0);
        out_point_2 = Vec2d(new_path, 0);
        out_point_3 = Vec2d(new_path, end_y_position);
    }

    out_points.clear();
    out_points.emplace_back(out_point_1);
    out_points.emplace_back(out_point_2);
    out_points.emplace_back(out_point_3);

    return out_points;
}

// Only add a newline in case the current G-code does not end with a newline.
    static inline void check_add_eol(std::string& gcode)
    {
        if (!gcode.empty() && gcode.back() != '\n')
            gcode += '\n';
    }


    // Return true if tch_prefix is found in custom_gcode
    static bool custom_gcode_changes_tool(const std::string& custom_gcode, const std::string& tch_prefix, unsigned next_extruder)
    {
        bool ok = false;
        size_t from_pos = 0;
        size_t pos = 0;
        while ((pos = custom_gcode.find(tch_prefix, from_pos)) != std::string::npos) {
            if (pos + 1 == custom_gcode.size())
                break;
            from_pos = pos + 1;
            // only whitespace is allowed before the command
            while (--pos < custom_gcode.size() && custom_gcode[pos] != '\n') {
                if (!std::isspace(custom_gcode[pos]))
                    goto NEXT;
            }
            {
                // we should also check that the extruder changes to what was expected
                std::istringstream ss(custom_gcode.substr(from_pos, std::string::npos));
                unsigned num = 0;
                if (ss >> num)
                    ok = (num == next_extruder);
            }
        NEXT:;
        }
        return ok;
    }

    std::string OozePrevention::pre_toolchange(GCode& gcodegen)
    {
        std::string gcode;

        unsigned int extruder_id = gcodegen.writer().filament()->id();
        const auto& filament_idle_temp = gcodegen.config().idle_temperature;
        if (filament_idle_temp.get_at(extruder_id) == 0) {
            // There is no idle temperature defined in filament settings.
            // Use the delta value from print config.
            if (gcodegen.config().standby_temperature_delta.value != 0) {
                // we assume that heating is always slower than cooling, so no need to block
                gcode += gcodegen.writer().set_temperature
                (this->_get_temp(gcodegen) + gcodegen.config().standby_temperature_delta.value, false, extruder_id);
                gcode.pop_back();
                gcode += " ;cooldown\n"; // this is a marker for GCodeProcessor, so it can supress the commands when needed
            }
        } else {
            // Use the value from filament settings. That one is absolute, not delta.
            gcode += gcodegen.writer().set_temperature(filament_idle_temp.get_at(extruder_id), false, extruder_id);
            gcode.pop_back();
            gcode += " ;cooldown\n"; // this is a marker for GCodeProcessor, so it can supress the commands when needed
        }

        return gcode;
    }

    std::string OozePrevention::post_toolchange(GCode& gcodegen)
    {
        return (gcodegen.config().standby_temperature_delta.value != 0) ?
            gcodegen.writer().set_temperature(this->_get_temp(gcodegen), true, gcodegen.writer().filament()->id()) :
            std::string();
    }

    int OozePrevention::_get_temp(const GCode &gcodegen) const
    {
        // First layer temperature should be used when on the first layer (obviously) and when
        // "other layers" is set to zero (which means it should not be used).
        return (gcodegen.layer() == nullptr || gcodegen.layer()->id() == 0
             || gcodegen.config().nozzle_temperature.get_at(gcodegen.writer().filament()->id()) == 0)
            ? gcodegen.config().nozzle_temperature_initial_layer.get_at(gcodegen.writer().filament()->id())
            : gcodegen.config().nozzle_temperature.get_at(gcodegen.writer().filament()->id());
    }
    
    // Orca:
    // Function to calculate the excess retraction length that should be retracted either before or after wiping
    // in order for the wipe operation to respect the filament retraction speed
    Wipe::RetractionValues Wipe::calculateWipeRetractionLengths(GCode& gcodegen, bool toolchange) {
        auto& writer = gcodegen.writer();
        auto& config = gcodegen.config();
        auto extruder = writer.filament();
        auto extruder_id = extruder->extruder_id();
        auto last_pos = gcodegen.last_pos();
        
        // Declare & initialize retraction lengths
        double retraction_length_remaining = 0,
                retractionBeforeWipe = 0,
                retractionDuringWipe = 0;
        
        // initialise the remaining retraction amount with the full retraction amount.
        retraction_length_remaining = toolchange ? extruder->retract_length_toolchange() : extruder->retraction_length();
        
        // nothing to retract - return early
        if(retraction_length_remaining <=EPSILON) return {0.f,0.f};
        
        // calculate retraction before wipe distance from the user setting. Keep adding to this variable any excess retraction needed
        // to be performed before the wipe.
        retractionBeforeWipe = retraction_length_remaining * extruder->retract_before_wipe();
        retraction_length_remaining -= retractionBeforeWipe; // subtract it from the remaining retraction length
        
        // all of the retraction is to be done before the wipe
        if(retraction_length_remaining <=EPSILON) return {retractionBeforeWipe,0.f};
        
        // Calculate wipe speed
        double wipe_speed = config.role_based_wipe_speed ? writer.get_current_speed() / 60.0 : config.get_abs_value("wipe_speed");
        wipe_speed = std::max(wipe_speed, 10.0);

        // Process wipe path & calculate wipe path length
        double wipe_dist = scale_(config.wipe_distance.get_at(extruder_id));
        Polyline wipe_path = {last_pos};
        wipe_path.append(this->path.points.begin() + 1, this->path.points.end());
        double wipe_path_length = std::min(wipe_path.length(), wipe_dist);

        // Calculate the maximum retraction amount during wipe
        retractionDuringWipe = config.retraction_speed.get_at(extruder_id) * unscale_(wipe_path_length) / wipe_speed;
        // If the maximum retraction amount during wipe is too small, return 0 and retract everything prior to the wipe.
        if(retractionDuringWipe <= EPSILON) return {retractionBeforeWipe,0.f};
        
        // If the maximum retraction amount during wipe is greater than any remaining retraction length
        // return the remaining retraction length to be retracted during the wipe
        if (retractionDuringWipe - retraction_length_remaining > EPSILON) return {retractionBeforeWipe,retraction_length_remaining};
        
        // We will always proceed with incrementing the retraction amount before wiping with the difference
        // and return the maximum allowed wipe amount to be retracted during the wipe move
        retractionBeforeWipe += retraction_length_remaining - retractionDuringWipe;
        return {retractionBeforeWipe, retractionDuringWipe};
    }

    std::string transform_gcode(const std::string &gcode, Vec2f pos, const Vec2f &translation, float angle)
    {
        Vec2f              extruder_offset(0, 0);
        std::istringstream gcode_str(gcode);
        std::string        gcode_out;
        std::string        line;
        Vec2f              transformed_pos = pos;
        Vec2f              old_pos(-1000.1f, -1000.1f);

        while (gcode_str) {
            std::getline(gcode_str, line); // we read the gcode line by line

            if (line.find("G1 ") == 0) {
                bool never_skip = false;
                auto it         = line.find(WipeTower::never_skip_tag());
                if (it != std::string::npos) {
                    // remove the tag and remember we saw it
                    never_skip = true;
                    line.erase(it, it + WipeTower::never_skip_tag().size());
                }
                std::ostringstream line_out;
                std::istringstream line_str(line);
                line_str >> std::noskipws; // don't skip whitespace
                char ch = 0;
                while (line_str >> ch) {
                    if (ch == 'X' || ch == 'Y')
                        line_str >> (ch == 'X' ? pos.x() : pos.y());
                    else
                        line_out << ch;
                }

                transformed_pos = Eigen::Rotation2Df(angle) * pos + translation;

                if (transformed_pos != old_pos || never_skip) {
                    line = line_out.str();
                    std::ostringstream oss;
                    oss << std::fixed << std::setprecision(3) << "G1 ";
                    if (transformed_pos.x() != old_pos.x() || never_skip) oss << " X" << transformed_pos.x() - extruder_offset.x();
                    if (transformed_pos.y() != old_pos.y() || never_skip) oss << " Y" << transformed_pos.y() - extruder_offset.y();
                    oss << " ";
                    line.replace(line.find("G1 "), 3, oss.str());
                    old_pos = transformed_pos;
                }
            }

            gcode_out += line + "\n";
        }
        return gcode_out;
    }

    float get_wipe_avoid_pos_x(const Vec2f &wt_min, const Vec2f &wt_max, float offset)
    {
        float left = 100, right = 250;
        float default_value = 110.f;
        float a = 0.f, b = 0.f;
        a = wt_max.x() + offset;
        b = wt_min.x() - offset;
        if (a > left && a < right) return a;
        if (b > left && b < right) return b;
        return default_value;
    }

    std::string Wipe::wipe(GCode& gcodegen,double length, bool toolchange, bool is_last)
    {
        std::string gcode;

        /*  Reduce feedrate a bit; travel speed is often too high to move on existing material.
            Too fast = ripping of existing material; too slow = short wipe path, thus more blob.  */
        double _wipe_speed = gcodegen.config().get_abs_value("wipe_speed");// gcodegen.writer().config.travel_speed.value * 0.8;
        if(gcodegen.config().role_based_wipe_speed)
            _wipe_speed = gcodegen.writer().get_current_speed() / 60.0;
        if(_wipe_speed < 10)
            _wipe_speed = 10;


        //SoftFever: allow 100% retract before wipe
        if (length >= 0)
        {
            /*  Calculate how long we need to travel in order to consume the required
                amount of retraction. In other words, how far do we move in XY at wipe_speed
                for the time needed to consume retraction_length at retraction_speed?  */
            // BBS
            double wipe_dist = scale_(gcodegen.config().wipe_distance.get_at(gcodegen.writer().filament()->id()));

            /*  Take the stored wipe path and replace first point with the current actual position
                (they might be different, for example, in case of loop clipping).  */
            Polyline wipe_path;
            wipe_path.append(gcodegen.last_pos());
            wipe_path.append(
                this->path.points.begin() + 1,
                this->path.points.end()
            );

            wipe_path.clip_end(wipe_path.length() - wipe_dist);

            // subdivide the retraction in segments
            if (!wipe_path.empty()) {
                // BBS. Handle short path case.
                if (wipe_path.length() < wipe_dist) {
                    wipe_dist = wipe_path.length();
                    //BBS: avoid to divide 0
                    wipe_dist = wipe_dist < EPSILON ? EPSILON : wipe_dist;
                }

                // add tag for processor
                gcode += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Start) + "\n";
                //BBS: don't need to enable cooling makers when this is the last wipe. Because no more cooling layer will clean this "_WIPE"
                //Softfever:
                std::string cooling_mark = "";
                if (gcodegen.enable_cooling_markers() && !is_last)
                    cooling_mark = /*gcodegen.config().role_based_wipe_speed ? ";_EXTERNAL_PERIMETER" : */";_WIPE";

                gcode += gcodegen.writer().set_speed(_wipe_speed * 60, "", cooling_mark);
                for (const Line& line : wipe_path.lines()) {
                    double segment_length = line.length();
                    double dE = length * (segment_length / wipe_dist);
                    //BBS: fix this FIXME
                    //FIXME one shall not generate the unnecessary G1 Fxxx commands, here wipe_speed is a constant inside this cycle.
                    // Is it here for the cooling markers? Or should it be outside of the cycle?
                    //gcode += gcodegen.writer().set_speed(wipe_speed * 60, "", gcodegen.enable_cooling_markers() ? ";_WIPE" : "");
                    gcode += gcodegen.writer().extrude_to_xy(
                        gcodegen.point_to_gcode(line.b),
                        -dE,
                        "wipe and retract"
                    );
                }
                // add tag for processor
                gcode += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_End) + "\n";
                gcodegen.set_last_pos(wipe_path.points.back());
            }

            // prevent wiping again on same path
            this->reset_path();
        }

        return gcode;
    }

    static inline Point wipe_tower_point_to_object_point(GCode& gcodegen, const Vec2f& wipe_tower_pt)
    {
        return Point(scale_(wipe_tower_pt.x() - gcodegen.origin()(0)), scale_(wipe_tower_pt.y() - gcodegen.origin()(1)));
    }

    // set volumetric speed of outer wall ,ignore per obejct & region ,just use default setting
    static float get_outer_wall_volumetric_speed(const FullPrintConfig& config, const Print& print, int filament_id, int extruder_id) {
        float outer_wall_volumetric_speed = 0;
        float filament_max_volumetric_speed = config.filament_max_volumetric_speed.get_at(filament_id);
        const double filament_diameter = config.filament_diameter.get_at(filament_id);
        float outer_wall_line_width = print.default_region_config().get_abs_value("outer_wall_line_width", filament_diameter);
        if (outer_wall_line_width == 0.0) {
            float default_line_width = print.default_object_config().get_abs_value("line_width", filament_diameter);
            outer_wall_line_width = default_line_width == 0.0 ? filament_diameter : default_line_width;
        }
        Flow outer_wall_flow = Flow(outer_wall_line_width, config.layer_height, config.nozzle_diameter.get_at(extruder_id));
        float outer_wall_speed = print.default_region_config().outer_wall_speed.value;
        outer_wall_volumetric_speed = outer_wall_speed * outer_wall_flow.mm3_per_mm();
        if (outer_wall_volumetric_speed > filament_max_volumetric_speed)
            outer_wall_volumetric_speed = filament_max_volumetric_speed;
        return outer_wall_volumetric_speed;
    }


    // Parse the custom G-code, try to find mcode_set_temp_dont_wait and mcode_set_temp_and_wait or optionally G10 with temperature inside the custom G-code.
    // Returns true if one of the temp commands are found, and try to parse the target temperature value into temp_out.
    static bool custom_gcode_sets_temperature(const std::string &gcode, const int mcode_set_temp_dont_wait, const int mcode_set_temp_and_wait, const bool include_g10, int &temp_out)
    {
        temp_out = -1;
        if (gcode.empty())
            return false;

        const char *ptr = gcode.data();
        bool temp_set_by_gcode = false;
        while (*ptr != 0) {
            // Skip whitespaces.
            for (; *ptr == ' ' || *ptr == '\t'; ++ ptr);
            if (*ptr == 'M' || // Line starts with 'M'. It is a machine command.
                (*ptr == 'G' && include_g10)) { // Only check for G10 if requested
                bool is_gcode = *ptr == 'G';
                ++ ptr;
                // Parse the M or G code value.
                char *endptr = nullptr;
                int mgcode = int(strtol(ptr, &endptr, 10));
                if (endptr != nullptr && endptr != ptr &&
                    is_gcode ?
                    // G10 found
                    mgcode == 10 :
                    // M104/M109 or M140/M190 found.
                    (mgcode == mcode_set_temp_dont_wait || mgcode == mcode_set_temp_and_wait)) {
                    ptr = endptr;
                    if (! is_gcode)
                        // Let the caller know that the custom M-code sets the temperature.
                        temp_set_by_gcode = true;
                    // Now try to parse the temperature value.
                    // While not at the end of the line:
                    while (strchr(";\r\n\0", *ptr) == nullptr) {
                        // Skip whitespaces.
                        for (; *ptr == ' ' || *ptr == '\t'; ++ ptr);
                        if (*ptr == 'S') {
                            // Skip whitespaces.
                            for (++ ptr; *ptr == ' ' || *ptr == '\t'; ++ ptr);
                            // Parse an int.
                            endptr = nullptr;
                            long temp_parsed = strtol(ptr, &endptr, 10);
                            if (endptr > ptr) {
                                ptr = endptr;
                                temp_out = temp_parsed;
                                // Let the caller know that the custom G-code sets the temperature
                                // Only do this after successfully parsing temperature since G10
                                // can be used for other reasons
                                temp_set_by_gcode = true;
                            }
                        } else {
                            // Skip this word.
                            for (; strchr(" \t;\r\n\0", *ptr) == nullptr; ++ ptr);
                        }
                    }
                }
            }
            // Skip the rest of the line.
            for (; *ptr != 0 && *ptr != '\r' && *ptr != '\n'; ++ ptr);
            // Skip the end of line indicators.
            for (; *ptr == '\r' || *ptr == '\n'; ++ ptr);
        }
        return temp_set_by_gcode;
    }

    // BBS
    // start_pos refers to the last position before the wipe_tower.
    // end_pos refers to the wipe tower's start_pos.
    // using the print coordinate system
    Polyline WipeTowerIntegration::generate_path_to_wipe_tower(const Point& start_pos,const Point &end_pos , const BoundingBox& avoid_polygon , const BoundingBox& printer_bbx) const
    {
        Polyline    res;
        coord_t         alpha = scaled(2.f); // offset distance
        BoundingBox avoid_polygon_inner = avoid_polygon;
        avoid_polygon_inner.offset(alpha);
        coord_t width = avoid_polygon_inner.max[0] - avoid_polygon_inner.min[0];
        Polygon bed_polygon = printer_bbx.polygon();
        Vec2f v(1, 0);                                                      // the first print direction of end_pos.
        if (abs(end_pos[0] - avoid_polygon_inner.min[0]) < width / 2) v = -v; // judge whether the wipe tower's infill goes to the left or right.
        // Judge whether the avoid_polygon_inner is outside the printer_bbx.
        // If so, do nothing and just go directly to the end_pos.
        bool is_bbx_in_bed = true;
        Points avoid_points  = avoid_polygon_inner.polygon().points;
        for (auto &wipe_tower_bbx_p : avoid_points) {
            if (ClipperLib::PointInPolygon(wipe_tower_bbx_p, bed_polygon.points) != 1) {
                is_bbx_in_bed = false;
                break;
            }
        }
        if (!is_bbx_in_bed) {
            res.points.push_back(end_pos);
            return res;
        }
        // Ray-Line Segment Intersection
        auto ray_intersetion_line = [](const Vec2d &a, const Vec2d &v1, const Vec2d &b, const Vec2d &c) -> std::pair<bool, Point> {
            const Vec2d v2    = c - b;
            double      denom = cross2(v1, v2);
            if (fabs(denom) < EPSILON) return {false, Point(0, 0)};
            const Vec2d v12    = (a - b);
            double      nume_a = cross2(v2, v12);
            double      nume_b = cross2(v1, v12);
            double      t1     = nume_a / denom;
            double      t2     = nume_b / denom;
            if (t1 >= 0 && t2 >= 0 && t2 <= 1.) {
                // Get the intersection point.
                Vec2d res = a + t1 * v1;
                return std::pair<bool, Point>(true, scaled(res));
            }
            return std::pair<bool, Point>(false, {0, 0});
        };
        struct Inter_info
        {
            int   inter_idx0 = -1;
            Point inter_p;
        };
        auto calc_path_len = [](Points &points, Inter_info &beg_info, Inter_info &end_info, bool is_add) -> std::pair<std::vector<Point>, double> {
            int                beg = is_add ? (beg_info.inter_idx0 + 1) % points.size() : beg_info.inter_idx0;
            int                end = is_add ? end_info.inter_idx0 : (end_info.inter_idx0 + 1) % points.size();
            int                i   = beg;
            double             len = 0;
            std::vector<Point> path;
            path.push_back(beg_info.inter_p);
            len += (unscale(beg_info.inter_p) - unscale(points[beg])).squaredNorm();
            while (i != end) {
                int  ni = is_add ? (i + 1) % points.size() : (i - 1 + points.size()) % points.size();
                auto a  = unscale(points[i]);
                auto b  = unscale(points[ni]);
                len += (a - b).squaredNorm();
                path.push_back(points[i]);
                i = ni;
            }
            path.push_back(points[end]);
            path.push_back(end_info.inter_p);
            len += (unscale(end_info.inter_p) - unscale(points[end])).squaredNorm();
            return {path, len};
        };
        // calculate the intersection point of end_pos along vector v with the avoid_polygon.
        // store in inter_info.
        // represent this intersection by 'p'.
        Inter_info inter_info;
        for (size_t i = 0; i < avoid_points.size(); i++) {
            const auto &a                  = avoid_points[i];
            const auto &b                  = avoid_points[(i + 1) % avoid_points.size()];
            auto [is_inter, inter_p] = ray_intersetion_line(unscale(end_pos), v.cast<double>(), unscale(a), unscale(b));
            if (is_inter) {
                inter_info.inter_idx0 = i;
                inter_info.inter_p    = inter_p;
                break;
            }
        }
        if (inter_info.inter_idx0 == -1) {
            res.points.push_back(end_pos);
            return res;
        }
        // calculate the other intersection of start_to_p with the avoid_polygon.
        // represent this intersection by 'p_'.
        Inter_info inter_info2;
        Linef      start_to_p(unscale(start_pos), unscale(inter_info.inter_p));
        for (size_t i = 0; i < avoid_points.size(); i++) {
            if (i == inter_info.inter_idx0) continue;
            Vec2d a = unscale(avoid_points[i]);
            Vec2d b = unscale(avoid_points[(i + 1) % avoid_points.size()]);
            Linef tower_edge(a, b);
            Vec2d inter;
            if (line_alg::intersection(start_to_p, tower_edge, &inter)) {
                inter_info2.inter_p    = scaled(inter);
                inter_info2.inter_idx0 = i;
                break;
            }
        }
        // if p_ does not exist, go directly to p.
        // else p travels along the shorter path on the wipe_tower_offset_polygon to p_
        if (inter_info2.inter_idx0 == -1) {
            res.points.push_back(inter_info.inter_p);
        } else {
            std::vector<Point> path;
            auto [path1, len1] = calc_path_len(avoid_points, inter_info2, inter_info, true);
            auto [path2, len2] = calc_path_len(avoid_points, inter_info2, inter_info, false);
            path               = len1 < len2 ? path1 : path2;
            for (size_t i = 0; i < path.size(); i++) {
                res.points.push_back(path[i]);
            }
        }
        res.points.push_back(end_pos);
        return res;
    }

    std::string WipeTowerIntegration::append_tcr(GCode& gcodegen, const WipeTower::ToolChangeResult& tcr, int new_filament_id, double z) const
    {
        if (new_filament_id != -1 && new_filament_id != tcr.new_tool)
            throw Slic3r::InvalidArgument("Error: WipeTowerIntegration::append_tcr was asked to do a toolchange it didn't expect.");

        int new_extruder_id = get_extruder_index(*m_print_config, new_filament_id);

        bool is_nozzle_change = !tcr.nozzle_change_result.gcode.empty() && (gcodegen.config().nozzle_diameter.size() > 1);

        std::string gcode;

        // Toolchangeresult.gcode assumes the wipe tower corner is at the origin (except for priming lines)
        // We want to rotate and shift all extrusions (gcode postprocessing) and starting and ending position
        float alpha = m_wipe_tower_rotation / 180.f * float(M_PI);

        auto transform_wt_pt = [&alpha, this](const Vec2f& pt) -> Vec2f {
            Vec2f out = Eigen::Rotation2Df(alpha) * pt;
            out += m_wipe_tower_pos + m_rib_offset;
            return out;
        };

        Vec2f start_pos = tcr.start_pos;
        Vec2f end_pos   = tcr.end_pos;
        Vec2f tool_change_start_pos = start_pos;
        if (tcr.is_tool_change)
            tool_change_start_pos = tcr.tool_change_start_pos;
        if (!tcr.priming) {
            start_pos = transform_wt_pt(start_pos);
            end_pos   = transform_wt_pt(end_pos);
            tool_change_start_pos = transform_wt_pt(tool_change_start_pos);
        }

        Vec2f wipe_tower_offset   = (tcr.priming ? Vec2f::Zero() : m_wipe_tower_pos) + m_rib_offset;
        float wipe_tower_rotation = tcr.priming ? 0.f : alpha;

        std::string tcr_rotated_gcode = post_process_wipe_tower_moves(tcr, wipe_tower_offset, wipe_tower_rotation);

        // BBS: add partplate logic
        Vec2f plate_origin_2d(m_plate_origin(0), m_plate_origin(1));

        // BBS: toolchange gcode will move to start_pos,
        // so only perform movement when printing sparse partition to support upper layer.
        // start_pos is the position in plate coordinate.
        if (!tcr.priming && tcr.is_finish_first) {
            // Move over the wipe tower.
            gcode += gcodegen.retract();
            gcodegen.m_avoid_crossing_perimeters.use_external_mp_once();
            gcode += gcodegen.travel_to(wipe_tower_point_to_object_point(gcodegen, start_pos + plate_origin_2d), erMixed,
                                        "Travel to a Wipe Tower");
            gcode += gcodegen.unretract();
        }


        double current_z = gcodegen.writer().get_position().z();
        if (z == -1.) // in case no specific z was provided, print at current_z pos
            z = current_z;
        if (!is_approx(z, current_z)) {
            gcode += gcodegen.writer().retract();
            gcode += gcodegen.writer().travel_to_z(z, "Travel down to the last wipe tower layer.");
            gcode += gcodegen.writer().unretract();
        }

        // Process the end filament gcode.
        bool        add_change_filament_624 = false;
        std::string end_filament_gcode_str;
        if (gcodegen.writer().filament() != nullptr) {
            // Process the custom filament_end_gcode in case of single_extruder_multi_material.
            unsigned int        old_filament_id = gcodegen.writer().filament()->id();
            const std::string& filament_end_gcode = gcodegen.config().filament_end_gcode.get_at(old_filament_id);
            if (gcodegen.writer().filament() != nullptr && !filament_end_gcode.empty()) {
                DynamicConfig config;
                config.set_key_value("layer_num", new ConfigOptionInt(gcodegen.m_layer_index));
                if (!gcodegen.m_filament_instances_code.empty()) {
                    end_filament_gcode_str += ("M624 " + gcodegen.m_filament_instances_code + "\n");
                    gcodegen.m_filament_instances_code = "";
                    add_change_filament_624 = true;
                }
                end_filament_gcode_str += gcodegen.placeholder_parser_process("filament_end_gcode", filament_end_gcode, old_filament_id, &config);
                check_add_eol(end_filament_gcode_str);
            }
        }

        //BBS: increase toolchange count
        gcodegen.m_toolchange_count++;

        std::string toolchange_gcode_str;

        ZHopType z_hope_type = ZHopType(gcodegen.config().z_hop_types.get_at(gcodegen.writer().filament()->id()));
        LiftType auto_lift_type = LiftType::NormalLift;
        if (z_hope_type == ZHopType::zhtAuto || z_hope_type == ZHopType::zhtSpiral || z_hope_type == ZHopType::zhtSlope)
            auto_lift_type = LiftType::SpiralLift;

        // BBS: should be placed before toolchange parsing
        std::string toolchange_retract_str = gcodegen.retract(tcr.is_tool_change && !is_nozzle_change, false, auto_lift_type, true);
        check_add_eol(toolchange_retract_str);

        //BBS: if needed, write the gcode_label_objects_end then priming tower, if the retract, didn't did it.
        std::string object_end_label_temp;
        gcodegen.m_writer.add_object_end_labels(object_end_label_temp);

        // Process the custom change_filament_gcode. If it is empty, provide a simple Tn command to change the filament.
        // Otherwise, leave control to the user completely.
        std::string change_filament_gcode = gcodegen.config().change_filament_gcode.value;

        bool is_used_travel_avoid_perimeter = gcodegen.m_config.prime_tower_skip_points.value;

        // add nozzle change gcode into change filament gcode
        std::string nozzle_change_gcode_trans;
        if (is_nozzle_change) {
            // move to start_pos before nozzle change
            std::string start_pos_str;
            start_pos_str = gcodegen.travel_to(wipe_tower_point_to_object_point(gcodegen, transform_wt_pt(tcr.nozzle_change_result.start_pos) + plate_origin_2d), erMixed,
                "Move to nozzle change start pos");
            check_add_eol(start_pos_str);
            nozzle_change_gcode_trans += start_pos_str;
            nozzle_change_gcode_trans += gcodegen.unretract();
            nozzle_change_gcode_trans += transform_gcode(tcr.nozzle_change_result.gcode, tcr.nozzle_change_result.start_pos, wipe_tower_offset, wipe_tower_rotation);
            gcodegen.set_last_pos(wipe_tower_point_to_object_point(gcodegen, transform_wt_pt(tcr.nozzle_change_result.end_pos) + plate_origin_2d));
            gcodegen.m_wipe.reset_path();
            for (const Vec2f& wipe_pt : tcr.nozzle_change_result.wipe_path)
                gcodegen.m_wipe.path.points.emplace_back(wipe_tower_point_to_object_point(gcodegen, transform_wt_pt(wipe_pt) + plate_origin_2d));
            nozzle_change_gcode_trans += gcodegen.retract(tcr.is_tool_change, false, auto_lift_type, true);
            end_filament_gcode_str = nozzle_change_gcode_trans + end_filament_gcode_str;
        }

        end_filament_gcode_str = toolchange_retract_str + object_end_label_temp + end_filament_gcode_str;

        std::string wipe_next_start_point_str;
        bool        need_travel_after_change_filament_gcode = false; // travel need be after the filament changed to get the correct "m_curr_extruder_id"
        if (! change_filament_gcode.empty()) {
            DynamicConfig config;
            int old_filament_id = gcodegen.writer().filament() ? (int)gcodegen.writer().filament()->id() : -1;
            int old_extruder_id = gcodegen.writer().filament() ? (int)gcodegen.writer().filament()->extruder_id() : -1;

            config.set_key_value("previous_extruder", new ConfigOptionInt(old_filament_id));
            config.set_key_value("next_extruder", new ConfigOptionInt(new_filament_id));
            config.set_key_value("layer_num", new ConfigOptionInt(gcodegen.m_layer_index));
            config.set_key_value("layer_z", new ConfigOptionFloat(tcr.print_z));
            config.set_key_value("toolchange_z", new ConfigOptionFloat(z));
            //            config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
            // BBS
            {
                GCodeWriter& gcode_writer = gcodegen.m_writer;
                FullPrintConfig& full_config = gcodegen.m_config;

                // set volumetric speed of outer wall ,ignore per obejct,just use default setting
                float outer_wall_volumetric_speed = get_outer_wall_volumetric_speed(full_config, *gcodegen.m_print, new_filament_id, gcodegen.get_extruder_id(new_filament_id));
                config.set_key_value("outer_wall_volumetric_speed", new ConfigOptionFloat(outer_wall_volumetric_speed));

                float old_retract_length = (old_filament_id != -1) ? full_config.retraction_length.get_at(old_filament_id) : 0;
                float new_retract_length = full_config.retraction_length.get_at(new_filament_id);
                float old_retract_length_toolchange = (old_filament_id != -1) ? full_config.retract_length_toolchange.get_at(old_filament_id) : 0;
                float new_retract_length_toolchange = full_config.retract_length_toolchange.get_at(new_filament_id);
                int old_filament_temp = (old_filament_id != -1) ? (gcodegen.on_first_layer()? full_config.nozzle_temperature_initial_layer.get_at(old_filament_id) : full_config.nozzle_temperature.get_at(old_filament_id)) : 210;
                int new_filament_temp = gcodegen.on_first_layer() ? full_config.nozzle_temperature_initial_layer.get_at(new_filament_id) : full_config.nozzle_temperature.get_at(new_filament_id);
                Vec3d nozzle_pos = gcode_writer.get_position();

                float purge_volume = tcr.purge_volume < EPSILON ? 0 : std::max(tcr.purge_volume, g_min_purge_volume);
                float filament_area = float((M_PI / 4.f) * pow(full_config.filament_diameter.get_at(new_filament_id), 2));
                float purge_length = purge_volume / filament_area;

                int old_filament_e_feedrate = (old_filament_id != -1) ? (int)(60.0 * full_config.filament_max_volumetric_speed.get_at(old_filament_id) / filament_area) : 200;
                old_filament_e_feedrate = old_filament_e_feedrate == 0 ? 100 : old_filament_e_feedrate;
                int new_filament_e_feedrate = (int)(60.0 * full_config.filament_max_volumetric_speed.get_at(new_filament_id) / filament_area);
                new_filament_e_feedrate = new_filament_e_feedrate == 0 ? 100 : new_filament_e_feedrate;
                float wipe_avoid_pos_x      = 0.f;
                {
                    //set wipe_avoid_pos_x
                    Vec2f box_min = transform_wt_pt(m_wipe_tower_bbx.min.cast<float>());
                    Vec2f box_max = transform_wt_pt(m_wipe_tower_bbx.max.cast<float>());
                    wipe_avoid_pos_x = get_wipe_avoid_pos_x(box_min, box_max, 3.f);
                }

                config.set_key_value("max_layer_z", new ConfigOptionFloat(gcodegen.m_max_layer_z));
                config.set_key_value("relative_e_axis", new ConfigOptionBool(full_config.use_relative_e_distances));
                config.set_key_value("toolchange_count", new ConfigOptionInt((int) gcodegen.m_toolchange_count));
                // BBS: fan speed is useless placeholer now, but we don't remove it to avoid
                // slicing error in old change_filament_gcode in old 3MF
                config.set_key_value("fan_speed", new ConfigOptionInt((int) 0));
                config.set_key_value("old_retract_length", new ConfigOptionFloat(old_retract_length));
                config.set_key_value("new_retract_length", new ConfigOptionFloat(new_retract_length));
                config.set_key_value("old_retract_length_toolchange", new ConfigOptionFloat(old_retract_length_toolchange));
                config.set_key_value("new_retract_length_toolchange", new ConfigOptionFloat(new_retract_length_toolchange));
                config.set_key_value("old_filament_temp", new ConfigOptionInt(old_filament_temp));
                config.set_key_value("new_filament_temp", new ConfigOptionInt(new_filament_temp));
                config.set_key_value("x_after_toolchange", new ConfigOptionFloat(tool_change_start_pos(0)));
                config.set_key_value("y_after_toolchange", new ConfigOptionFloat(tool_change_start_pos(1)));
                config.set_key_value("z_after_toolchange", new ConfigOptionFloat(nozzle_pos(2)));
                config.set_key_value("first_flush_volume", new ConfigOptionFloat(purge_length / 2.f));
                config.set_key_value("second_flush_volume", new ConfigOptionFloat(purge_length / 2.f));
                config.set_key_value("old_filament_e_feedrate", new ConfigOptionInt(old_filament_e_feedrate));
                config.set_key_value("new_filament_e_feedrate", new ConfigOptionInt(new_filament_e_feedrate));
                config.set_key_value("travel_point_1_x", new ConfigOptionFloat(float(travel_point_1.x())));
                config.set_key_value("travel_point_1_y", new ConfigOptionFloat(float(travel_point_1.y())));
                config.set_key_value("travel_point_2_x", new ConfigOptionFloat(float(travel_point_2.x())));
                config.set_key_value("travel_point_2_y", new ConfigOptionFloat(float(travel_point_2.y())));
                config.set_key_value("travel_point_3_x", new ConfigOptionFloat(float(travel_point_3.x())));
                config.set_key_value("travel_point_3_y", new ConfigOptionFloat(float(travel_point_3.y())));

                auto flush_v_speed = m_print_config->filament_flush_volumetric_speed.values;
                auto flush_temps = m_print_config->filament_flush_temp.values;
                for (size_t idx = 0; idx < flush_v_speed.size(); ++idx) {
                    if (flush_v_speed[idx] == 0)
                        flush_v_speed[idx] = m_print_config->filament_max_volumetric_speed.get_at(idx);
                }
                for (size_t idx = 0; idx < flush_temps.size(); ++idx) {
                    if (flush_temps[idx] == 0)
                        flush_temps[idx] = m_print_config->nozzle_temperature_range_high.get_at(idx);
                }
                config.set_key_value("flush_volumetric_speeds", new ConfigOptionFloats(flush_v_speed));
                config.set_key_value("flush_temperatures", new ConfigOptionInts(flush_temps));
                config.set_key_value("flush_length", new ConfigOptionFloat(purge_length));
                config.set_key_value("wipe_avoid_perimeter", new ConfigOptionBool(is_used_travel_avoid_perimeter));
                config.set_key_value("wipe_avoid_pos_x", new ConfigOptionFloat(wipe_avoid_pos_x));

                int   flush_count = std::min(g_max_flush_count, (int) std::round(purge_volume / g_purge_volume_one_time));
                float flush_unit  = purge_length / flush_count;
                int   flush_idx   = 0;
                for (; flush_idx < flush_count; flush_idx++) {
                    char key_value[64] = {0};
                    snprintf(key_value, sizeof(key_value), "flush_length_%d", flush_idx + 1);
                    config.set_key_value(key_value, new ConfigOptionFloat(flush_unit));
                }

                for (; flush_idx < g_max_flush_count; flush_idx++) {
                    char key_value[64] = {0};
                    snprintf(key_value, sizeof(key_value), "flush_length_%d", flush_idx + 1);
                    config.set_key_value(key_value, new ConfigOptionFloat(0.f));
                }
            }
            toolchange_gcode_str = gcodegen.placeholder_parser_process("change_filament_gcode", change_filament_gcode, new_filament_id, &config);

            check_add_eol(toolchange_gcode_str);

            //BBS
            {
                //BBS: current position and fan_speed is unclear after interting change_filament_gcode
                check_add_eol(toolchange_gcode_str);
                toolchange_gcode_str += ";_FORCE_RESUME_FAN_SPEED\n";
                gcodegen.writer().set_current_position_clear(false);
                // BBS: check whether custom gcode changes the z position. Update if changed
                double temp_z_after_tool_change;
                if (GCodeProcessor::get_last_z_from_gcode(toolchange_gcode_str, temp_z_after_tool_change)) {
                    Vec3d pos = gcodegen.writer().get_position();
                    pos(2)    = temp_z_after_tool_change;
                    gcodegen.writer().set_position(pos);
                }
            }
            need_travel_after_change_filament_gcode = true;
        }

        std::string toolchange_command;
        if (tcr.priming || (new_filament_id >= 0 && gcodegen.writer().need_toolchange(new_filament_id)))
            toolchange_command = gcodegen.writer().toolchange(new_filament_id);
        if (!custom_gcode_changes_tool(toolchange_gcode_str, gcodegen.writer().toolchange_prefix(), new_filament_id))
            toolchange_gcode_str += toolchange_command;
        else {
            // We have informed the m_writer about the current extruder_id, we can ignore the generated G-code.
        }

        if (need_travel_after_change_filament_gcode) {
            // move to start_pos for wiping after toolchange
            if (!is_used_travel_avoid_perimeter) {
                std::string start_pos_str = gcodegen.travel_to(wipe_tower_point_to_object_point(gcodegen, tool_change_start_pos + plate_origin_2d), erMixed, "Move to start pos");
                check_add_eol(start_pos_str);
                wipe_next_start_point_str = start_pos_str;
            } else {
                // BBS:change travel_path
                Vec3f gcode_last_pos;
                GCodeProcessor::get_last_position_from_gcode(toolchange_gcode_str, gcode_last_pos);
                Vec2f       gcode_last_pos2d{gcode_last_pos[0], gcode_last_pos[1]};
                Point       gcode_last_pos2d_object = gcodegen.gcode_to_point(gcode_last_pos2d.cast<double>() + plate_origin_2d.cast<double>());
                Point       start_wipe_pos          = wipe_tower_point_to_object_point(gcodegen, tool_change_start_pos + plate_origin_2d);
                BoundingBox avoid_bbx, printer_bbx;
                {
                    // set printer_bbx
                    Pointfs bed_pointsf = gcodegen.m_config.printable_area.values;
                    Points  bed_points;
                    for (auto p : bed_pointsf) { bed_points.push_back(wipe_tower_point_to_object_point(gcodegen, p.cast<float>() + plate_origin_2d)); }
                    printer_bbx = BoundingBox(bed_points);
                }
                {
                    // set avoid_bbx
                    avoid_bbx            = scaled(m_wipe_tower_bbx);
                    Polygon avoid_points = avoid_bbx.polygon();
                    for (auto &p : avoid_points.points) {
                        Vec2f pp = transform_wt_pt(unscale(p).cast<float>());
                        p        = wipe_tower_point_to_object_point(gcodegen, pp + plate_origin_2d);
                    }
                    avoid_bbx = BoundingBox(avoid_points.points);
                }
                std::string travel_to_wipe_tower_gcode;
                Polyline    travel_polyline = generate_path_to_wipe_tower(gcode_last_pos2d_object, start_wipe_pos, avoid_bbx, printer_bbx);

                for (size_t i = 0; i < travel_polyline.points.size(); ++i) {
                    const auto &p = travel_polyline.points[i];
                    if (i == travel_polyline.points.size() - 1) {
                        wipe_next_start_point_str = gcodegen.travel_to(p, erMixed, "Move to start pos");
                        check_add_eol(wipe_next_start_point_str);
                        break;
                    }
                    travel_to_wipe_tower_gcode += gcodegen.travel_to(p, erMixed, "Move to start pos");
                    check_add_eol(travel_to_wipe_tower_gcode);
                }
                toolchange_gcode_str += travel_to_wipe_tower_gcode;
                gcodegen.set_last_pos(start_wipe_pos);
            }
        }

        // do unretract after setting current extruder_id
        std::string toolchange_unretract_str = gcodegen.unretract();
        check_add_eol(toolchange_unretract_str);

        gcodegen.placeholder_parser().set("current_extruder", new_filament_id);
        gcodegen.placeholder_parser().set("retraction_distance_when_cut", gcodegen.m_config.retraction_distances_when_cut.get_at(new_filament_id));
        gcodegen.placeholder_parser().set("long_retraction_when_cut", gcodegen.m_config.long_retractions_when_cut.get_at(new_filament_id));
        gcodegen.placeholder_parser().set("retraction_distance_when_ec", gcodegen.m_config.retraction_distances_when_ec.get_at(new_filament_id));
        gcodegen.placeholder_parser().set("long_retraction_when_ec", gcodegen.m_config.long_retractions_when_ec.get_at(new_filament_id));

        // Process the start filament gcode.
        std::string start_filament_gcode_str;
        const std::string &filament_start_gcode = gcodegen.config().filament_start_gcode.get_at(new_filament_id);
        if (!filament_start_gcode.empty()) {
            // Process the filament_start_gcode for the active filament only.
            DynamicConfig config;
            config.set_key_value("filament_extruder_id", new ConfigOptionInt(new_filament_id));
            start_filament_gcode_str = gcodegen.placeholder_parser_process("filament_start_gcode", filament_start_gcode, new_filament_id, &config);
            if (add_change_filament_624) {
                start_filament_gcode_str += "M625\n";
                add_change_filament_624 = false;
            }
            check_add_eol(start_filament_gcode_str);
        }

        start_filament_gcode_str = start_filament_gcode_str + wipe_next_start_point_str + toolchange_unretract_str;

        // Insert the end filament, toolchange, and start filament gcode into the generated gcode.
        DynamicConfig config;
        config.set_key_value("filament_end_gcode", new ConfigOptionString(end_filament_gcode_str));
        config.set_key_value("change_filament_gcode", new ConfigOptionString(toolchange_gcode_str));
        config.set_key_value("filament_start_gcode", new ConfigOptionString(start_filament_gcode_str));
        std::string tcr_gcode, tcr_escaped_gcode = gcodegen.placeholder_parser_process("tcr_rotated_gcode", tcr_rotated_gcode, new_filament_id, &config);
        unescape_string_cstyle(tcr_escaped_gcode, tcr_gcode);
        gcode += tcr_gcode;
        check_add_eol(toolchange_gcode_str);

        // SoftFever: set new PA for new filament
        if (gcodegen.config().enable_pressure_advance.get_at(new_filament_id)) {
            gcode += gcodegen.writer().set_pressure_advance(gcodegen.config().pressure_advance.get_at(new_filament_id));
            // Orca: Adaptive PA
            // Reset Adaptive PA processor last PA value
            gcodegen.m_pa_processor->resetPreviousPA(gcodegen.config().pressure_advance.get_at(new_filament_id));
        }

        // A phony move to the end position at the wipe tower.
        gcodegen.writer().travel_to_xy((end_pos + plate_origin_2d).cast<double>());
        gcodegen.set_last_pos(wipe_tower_point_to_object_point(gcodegen, end_pos + plate_origin_2d));
        if (!is_approx(z, current_z)) {
            gcode += gcodegen.writer().retract();
            gcode += gcodegen.writer().travel_to_z(current_z, "Travel back up to the topmost object layer.");
            gcode += gcodegen.writer().unretract();
        }

        else {
            // Prepare a future wipe.
            gcodegen.m_wipe.reset_path();
            for (const Vec2f &wipe_pt : tcr.wipe_path)
                gcodegen.m_wipe.path.points.emplace_back(wipe_tower_point_to_object_point(gcodegen, transform_wt_pt(wipe_pt)));
            gcode += gcodegen.retract(false, false, auto_lift_type, true);
        }

        // Let the planner know we are traveling between objects.
        gcodegen.m_avoid_crossing_perimeters.use_external_mp_once();
        return gcode;
    }

    std::string WipeTowerIntegration::append_tcr2(GCode                             &gcodegen,
                                                  const WipeTower::ToolChangeResult &tcr,
                                                  int                                new_extruder_id,
                                                  double                             z) const
    {
        if (new_extruder_id != -1 && new_extruder_id != tcr.new_tool)
            throw Slic3r::InvalidArgument("Error: WipeTowerIntegration::append_tcr was asked to do a toolchange it didn't expect.");

        std::string gcode;

        // Toolchangeresult.gcode assumes the wipe tower corner is at the origin (except for priming lines)
        // We want to rotate and shift all extrusions (gcode postprocessing) and starting and ending position
        float alpha = m_wipe_tower_rotation / 180.f * float(M_PI);

        auto transform_wt_pt = [&alpha, this](const Vec2f &pt) -> Vec2f {
            Vec2f out = Eigen::Rotation2Df(alpha) * pt;
            out += m_wipe_tower_pos;
            return out;
        };

        Vec2f start_pos = tcr.start_pos;
        Vec2f end_pos   = tcr.end_pos;
        if (!tcr.priming) {
            start_pos = transform_wt_pt(start_pos);
            end_pos   = transform_wt_pt(end_pos);
        }

        Vec2f wipe_tower_offset   = tcr.priming ? Vec2f::Zero() : m_wipe_tower_pos;
        float wipe_tower_rotation = tcr.priming ? 0.f : alpha;
        Vec2f plate_origin_2d(m_plate_origin(0), m_plate_origin(1));


        std::string tcr_rotated_gcode = post_process_wipe_tower_moves(tcr, wipe_tower_offset, wipe_tower_rotation);

        gcode += gcodegen.writer().unlift(); // Make sure there is no z-hop (in most cases, there isn't).

        double current_z = gcodegen.writer().get_position().z();


        if (z == -1.) // in case no specific z was provided, print at current_z pos
            z = current_z;

        const bool needs_toolchange = gcodegen.writer().need_toolchange(new_extruder_id);
        const bool will_go_down     = !is_approx(z, current_z);
        const bool is_ramming       = (gcodegen.config().single_extruder_multi_material) ||
                                (!gcodegen.config().single_extruder_multi_material &&
                                 gcodegen.config().filament_multitool_ramming.get_at(tcr.initial_tool));
        const bool should_travel_to_tower = !tcr.priming && (tcr.force_travel     // wipe tower says so
                                                             || !needs_toolchange // this is just finishing the tower with no toolchange
                                                             || will_go_down // Make sure to move to prime tower before moving down
                                                             || is_ramming);

        if (should_travel_to_tower || gcodegen.m_need_change_layer_lift_z) {
            // FIXME: It would be better if the wipe tower set the force_travel flag for all toolchanges,
            // then we could simplify the condition and make it more readable.
            gcode += gcodegen.retract();
            gcodegen.m_avoid_crossing_perimeters.use_external_mp_once();
            gcode += gcodegen.travel_to(wipe_tower_point_to_object_point(gcodegen, start_pos + plate_origin_2d), erMixed, "Travel to a Wipe Tower");
            gcode += gcodegen.unretract();
        } else {
            // When this is multiextruder printer without any ramming, we can just change
            // the tool without travelling to the tower.
        }

        if (will_go_down) {
            gcode += gcodegen.writer().retract();
            gcode += gcodegen.writer().travel_to_z(z, "Travel down to the last wipe tower layer.");
            gcode += gcodegen.writer().unretract();
        }

        std::string toolchange_gcode_str;
        std::string deretraction_str;
        if (tcr.priming || (new_extruder_id >= 0 && needs_toolchange)) {
            if (is_ramming)
                gcodegen.m_wipe.reset_path();                                           // We don't want wiping on the ramming lines.
            toolchange_gcode_str = gcodegen.set_extruder(new_extruder_id, tcr.print_z); // TODO: toolchange_z vs print_z
            if (gcodegen.config().enable_prime_tower) {
                deretraction_str += gcodegen.writer().travel_to_z(z, "Force restore layer Z", true);
                Vec3d position{gcodegen.writer().get_position()};
                position.z() = z;
                gcodegen.writer().set_position(position);
                deretraction_str += gcodegen.unretract();
            }
        }

        // Insert the toolchange and deretraction gcode into the generated gcode.

        DynamicConfig config;
        config.set_key_value("change_filament_gcode", new ConfigOptionString(toolchange_gcode_str));
        config.set_key_value("deretraction_from_wipe_tower_generator", new ConfigOptionString(deretraction_str));
        config.set_key_value("layer_num", new ConfigOptionInt(gcodegen.m_layer_index));
        config.set_key_value("layer_z", new ConfigOptionFloat(tcr.print_z));
        config.set_key_value("toolchange_z", new ConfigOptionFloat(z));

        std::string tcr_gcode,
            tcr_escaped_gcode = gcodegen.placeholder_parser_process("tcr_rotated_gcode", tcr_rotated_gcode, new_extruder_id, &config);
        unescape_string_cstyle(tcr_escaped_gcode, tcr_gcode);
        gcode += tcr_gcode;
        check_add_eol(toolchange_gcode_str);

        // SoftFever: set new PA for new filament
        if (new_extruder_id != -1 && gcodegen.config().enable_pressure_advance.get_at(new_extruder_id)) {
            gcode += gcodegen.writer().set_pressure_advance(gcodegen.config().pressure_advance.get_at(new_extruder_id));
            // Orca: Adaptive PA
            // Reset Adaptive PA processor last PA value
            gcodegen.m_pa_processor->resetPreviousPA(gcodegen.config().pressure_advance.get_at(new_extruder_id));
        }

        // A phony move to the end position at the wipe tower.
        gcodegen.writer().travel_to_xy((end_pos + plate_origin_2d).cast<double>());
        gcodegen.set_last_pos(wipe_tower_point_to_object_point(gcodegen, end_pos + plate_origin_2d));
        if (!is_approx(z, current_z)) {
            gcode += gcodegen.writer().retract();
            gcode += gcodegen.writer().travel_to_z(current_z, "Travel back up to the topmost object layer.");
            gcode += gcodegen.writer().unretract();
        }

        else {
            // Prepare a future wipe.
            gcodegen.m_wipe.reset_path();
            for (const Vec2f& wipe_pt : tcr.wipe_path)
                gcodegen.m_wipe.path.points.emplace_back(wipe_tower_point_to_object_point(gcodegen, transform_wt_pt(wipe_pt) + plate_origin_2d));
        }

        // Let the planner know we are traveling between objects.
        gcodegen.m_avoid_crossing_perimeters.use_external_mp_once();
        return gcode;
    }

    // This function postprocesses gcode_original, rotates and moves all G1 extrusions and returns resulting gcode
    // Starting position has to be supplied explicitely (otherwise it would fail in case first G1 command only contained one coordinate)
    std::string WipeTowerIntegration::post_process_wipe_tower_moves(const WipeTower::ToolChangeResult& tcr, const Vec2f& translation, float angle) const
    {
        Vec2f extruder_offset;
        if (m_single_extruder_multi_material)
            extruder_offset = m_extruder_offsets[0].cast<float>();
        else
            extruder_offset = m_extruder_offsets[tcr.initial_tool].cast<float>();

        std::istringstream gcode_str(tcr.gcode);
        std::string gcode_out;
        std::string line;
        Vec2f pos = tcr.start_pos;
        auto  trans_pos = [wt_rot = Eigen::Rotation2Df(angle), &translation](const Vec2f& p) -> Vec2f { return wt_rot * p + translation; };
        Vec2f transformed_pos = trans_pos(pos);
        Vec2f old_pos(-1000.1f, -1000.1f);

        while (gcode_str) {
            std::getline(gcode_str, line);  // we read the gcode line by line

            // All G1 commands should be translated and rotated. X and Y coords are
            // only pushed to the output when they differ from last time.
            // WT generator can override this by appending the never_skip_tag
            if (line.find("G1 ") == 0 || line.find("G2 ") == 0 || line.find("G3 ") == 0) {
                std::string cur_gcode_start = line.find("G1 ") == 0 ? "G1 " : (line.find("G2 ") == 0 ? "G2 " : "G3 ");
                bool        never_skip      = false;
                auto        it              = line.find(WipeTower::never_skip_tag());
                if (it != std::string::npos) {
                    // remove the tag and remember we saw it
                    never_skip = true;
                    line.erase(it, it + WipeTower::never_skip_tag().size());
                }
                std::ostringstream line_out;
                std::istringstream line_str(line);
                line_str >> std::noskipws; // don't skip whitespace
                char ch = 0;
                while (line_str >> ch) {
                    if (ch == 'X' || ch == 'Y')
                        line_str >> (ch == 'X' ? pos.x() : pos.y());
                    else
                        line_out << ch;
                }

                transformed_pos = trans_pos(pos);

                if (transformed_pos != old_pos || never_skip) {
                    line = line_out.str();
                    std::ostringstream oss;
                    oss << std::fixed << std::setprecision(3) << cur_gcode_start;
                    if (transformed_pos.x() != old_pos.x() || never_skip)
                        oss << " X" << transformed_pos.x() - extruder_offset.x();
                    if (transformed_pos.y() != old_pos.y() || never_skip)
                        oss << " Y" << transformed_pos.y() - extruder_offset.y();
                    oss << " ";
                    line.replace(line.find(cur_gcode_start), 3, oss.str());
                    old_pos = transformed_pos;
                }
            }

            gcode_out += line + "\n";

            // If this was a toolchange command, we should change current extruder offset
            if (line == "[change_filament_gcode]") {
                // BBS
                if (!m_single_extruder_multi_material) {
                    extruder_offset = m_extruder_offsets[tcr.new_tool].cast<float>();

                    // If the extruder offset changed, add an extra move so everything is continuous
                    if (extruder_offset != m_extruder_offsets[tcr.initial_tool].cast<float>()) {
                        std::ostringstream oss;
                        oss << std::fixed << std::setprecision(3)
                            << "G1 X" << transformed_pos.x() - extruder_offset.x()
                            << " Y" << transformed_pos.y() - extruder_offset.y()
                            << "\n";
                        gcode_out += oss.str();
                    }
                }
                old_pos = Vec2f{-1000.1f, -1000.1f};
                pos     = tcr.tool_change_start_pos;
                transformed_pos = pos;
            }
        }
        return gcode_out;
    }

    std::string WipeTowerIntegration::prime(GCode &gcodegen)
    {
        std::string gcode;
        if (!gcodegen.is_BBL_Printer() && !gcodegen.is_QIDI_Printer()) {
            for (const WipeTower::ToolChangeResult &tcr : m_priming) {
                if (!tcr.extrusions.empty())
                    gcode += append_tcr2(gcodegen, tcr, tcr.new_tool);
            }
        }
        return gcode;
    }

    std::string WipeTowerIntegration::tool_change(GCode &gcodegen, int extruder_id, bool finish_layer)
    {
        std::string gcode;

        assert(m_layer_idx >= 0);
        if (m_layer_idx >= (int) m_tool_changes.size())
            return gcode;
        if (!gcodegen.is_BBL_Printer() && !gcodegen.is_QIDI_Printer()) {
            if (gcodegen.writer().need_toolchange(extruder_id) || finish_layer) {
                if (m_layer_idx < (int) m_tool_changes.size()) {
                    if (!(size_t(m_tool_change_idx) < m_tool_changes[m_layer_idx].size()))
                        throw Slic3r::RuntimeError("Wipe tower generation failed, possibly due to empty first layer.");

                    // Calculate where the wipe tower layer will be printed. -1 means that print z will not change,
                    // resulting in a wipe tower with sparse layers.
                    double wipe_tower_z  = -1;
                    bool   ignore_sparse = false;
                    if (gcodegen.config().wipe_tower_no_sparse_layers.value) {
                        wipe_tower_z  = m_last_wipe_tower_print_z;
                        ignore_sparse = (m_tool_changes[m_layer_idx].size() == 1 &&
                                         m_tool_changes[m_layer_idx].front().initial_tool == m_tool_changes[m_layer_idx].front().new_tool &&
                                         m_layer_idx != 0);
                        if (m_tool_change_idx == 0 && !ignore_sparse)
                        wipe_tower_z = m_last_wipe_tower_print_z + m_tool_changes[m_layer_idx].front().layer_height;
                    }

                    if (!ignore_sparse) {
                        gcode += append_tcr2(gcodegen, m_tool_changes[m_layer_idx][m_tool_change_idx++], extruder_id, wipe_tower_z);
                        m_last_wipe_tower_print_z = wipe_tower_z;
                    }
                }
            }
        } else {
            // Calculate where the wipe tower layer will be printed. -1 means that print z will not change,
            // resulting in a wipe tower with sparse layers.
            double wipe_tower_z  = -1;
            bool   ignore_sparse = false;
            if (gcodegen.config().wipe_tower_no_sparse_layers.value) {
                wipe_tower_z  = m_last_wipe_tower_print_z;
                ignore_sparse = (m_tool_changes[m_layer_idx].size() == 1 &&
                                 m_tool_changes[m_layer_idx].front().initial_tool == m_tool_changes[m_layer_idx].front().new_tool);
                if (m_tool_change_idx == 0 && !ignore_sparse)
                    wipe_tower_z = m_last_wipe_tower_print_z + m_tool_changes[m_layer_idx].front().layer_height;
            }

            if ((m_enable_timelapse_print || m_enable_wrapping_detection) && m_is_first_print) {
                gcode += append_tcr(gcodegen, m_tool_changes[m_layer_idx][0], m_tool_changes[m_layer_idx][0].new_tool, wipe_tower_z);
                m_tool_change_idx++;
                m_is_first_print = false;
            }

            if (gcodegen.writer().need_toolchange(extruder_id) || finish_layer) {
                if (!(size_t(m_tool_change_idx) < m_tool_changes[m_layer_idx].size()))
                    throw Slic3r::RuntimeError("Wipe tower generation failed, possibly due to empty first layer.");

                if (!ignore_sparse) {
                    gcode += append_tcr(gcodegen, m_tool_changes[m_layer_idx][m_tool_change_idx++], extruder_id, wipe_tower_z);
                    m_last_wipe_tower_print_z = wipe_tower_z;
                }
            }
        }

        return gcode;
    }

    bool WipeTowerIntegration::is_empty_wipe_tower_gcode(GCode &gcodegen, int extruder_id, bool finish_layer)
    {
        assert(m_layer_idx >= 0);
        if (m_layer_idx >= (int) m_tool_changes.size())
            return true;

        bool   ignore_sparse = false;
        if (gcodegen.config().wipe_tower_no_sparse_layers.value) {
            ignore_sparse = (m_tool_changes[m_layer_idx].size() == 1 && m_tool_changes[m_layer_idx].front().initial_tool == m_tool_changes[m_layer_idx].front().new_tool);
        }

        if ((m_enable_timelapse_print || m_enable_wrapping_detection) && m_is_first_print) {
            return false;
        }

        if (gcodegen.writer().need_toolchange(extruder_id) || finish_layer) {
            if (!(size_t(m_tool_change_idx) < m_tool_changes[m_layer_idx].size()))
                throw Slic3r::RuntimeError("Wipe tower generation failed, possibly due to empty first layer.");

            if (!ignore_sparse) {
                return false;
            }
        }

        return true;
    }

    // Print is finished. Now it remains to unload the filament safely with ramming over the wipe tower.
    std::string WipeTowerIntegration::finalize(GCode &gcodegen)
    {
        std::string gcode;
        if (!gcodegen.is_BBL_Printer() && !gcodegen.is_QIDI_Printer()) {
            if (std::abs(gcodegen.writer().get_position().z() - m_final_purge.print_z) > EPSILON)
                gcode += gcodegen.change_layer(m_final_purge.print_z);
            gcode += append_tcr2(gcodegen, m_final_purge, -1);
        }

        return gcode;
    }

    const std::vector<std::string> ColorPrintColors::Colors = { "#C0392B", "#E67E22", "#F1C40F", "#27AE60", "#1ABC9C", "#2980B9", "#9B59B6" };

#define EXTRUDER_CONFIG(OPT) m_config.OPT.get_at(m_writer.filament()->extruder_id())
#define FILAMENT_CONFIG(OPT) m_config.OPT.get_at(m_writer.filament()->id())

void GCode::PlaceholderParserIntegration::reset()
{
    this->failed_templates.clear();
    this->output_config.clear();
    this->opt_position = nullptr;
    this->opt_zhop      = nullptr;
    this->opt_e_position = nullptr;
    this->opt_e_retracted = nullptr;
    this->opt_e_restart_extra = nullptr;
    this->opt_extruded_volume = nullptr;
    this->opt_extruded_weight = nullptr;
    this->opt_extruded_volume_total = nullptr;
    this->opt_extruded_weight_total = nullptr;
    this->num_extruders = 0;
    this->position.clear();
    this->e_position.clear();
    this->e_retracted.clear();
    this->e_restart_extra.clear();
}

void GCode::PlaceholderParserIntegration::init(const GCodeWriter &writer)
{
    this->reset();
    const std::vector<Extruder> &extruders = writer.extruders();
    if (! extruders.empty()) {
        this->num_extruders = extruders.back().id() + 1;
        this->e_retracted.assign(MAXIMUM_EXTRUDER_NUMBER, 0);
        this->e_restart_extra.assign(MAXIMUM_EXTRUDER_NUMBER, 0);
        this->opt_e_retracted = new ConfigOptionFloats(e_retracted);
        this->opt_e_restart_extra = new ConfigOptionFloats(e_restart_extra);
        this->output_config.set_key_value("e_retracted", this->opt_e_retracted);
        this->output_config.set_key_value("e_restart_extra", this->opt_e_restart_extra);
        if (! writer.config.use_relative_e_distances) {
            e_position.assign(MAXIMUM_EXTRUDER_NUMBER, 0);
            opt_e_position = new ConfigOptionFloats(e_position);
            this->output_config.set_key_value("e_position", opt_e_position);
        }
    }
    this->opt_extruded_volume = new ConfigOptionFloats(this->num_extruders, 0.f);
    this->opt_extruded_weight = new ConfigOptionFloats(this->num_extruders, 0.f);
    this->opt_extruded_volume_total = new ConfigOptionFloat(0.f);
    this->opt_extruded_weight_total = new ConfigOptionFloat(0.f);
    this->parser.set("extruded_volume", this->opt_extruded_volume);
    this->parser.set("extruded_weight", this->opt_extruded_weight);
    this->parser.set("extruded_volume_total", this->opt_extruded_volume_total);
    this->parser.set("extruded_weight_total", this->opt_extruded_weight_total);

    // Reserve buffer for current position.
    this->position.assign(3, 0);
    this->opt_position = new ConfigOptionFloats(this->position);
    this->output_config.set_key_value("position", this->opt_position);
    // Store zhop variable into the parser itself, it is a read-only variable to the script.
    this->opt_zhop = new ConfigOptionFloat(writer.get_zhop());
    this->parser.set("zhop", this->opt_zhop);
}

void GCode::PlaceholderParserIntegration::update_from_gcodewriter(const GCodeWriter &writer)
{
    memcpy(this->position.data(), writer.get_position().data(), sizeof(double) * 3);
    this->opt_position->values = this->position;
    this->opt_zhop->value = writer.get_zhop();

    if (this->num_extruders > 0) {
        const std::vector<Extruder> &extruders = writer.extruders();
        assert(! extruders.empty() && num_extruders == extruders.back().id() + 1);
        this->e_retracted.assign(MAXIMUM_EXTRUDER_NUMBER, 0);
        this->e_restart_extra.assign(MAXIMUM_EXTRUDER_NUMBER, 0);
        this->opt_extruded_volume->values.assign(num_extruders, 0);
        this->opt_extruded_weight->values.assign(num_extruders, 0);
        double total_volume = 0.;
        double total_weight = 0.;
        for (const Extruder &e : extruders) {
            this->e_retracted[e.id()]     = e.retracted();
            this->e_restart_extra[e.id()] = e.restart_extra();
            double v = e.extruded_volume();
            double w = v * e.filament_density() * 0.001;
            this->opt_extruded_volume->values[e.id()] = v;
            this->opt_extruded_weight->values[e.id()] = w;
            total_volume += v;
            total_weight += w;
        }
        opt_extruded_volume_total->value = total_volume;
        opt_extruded_weight_total->value = total_weight;
        opt_e_retracted->values = this->e_retracted;
        opt_e_restart_extra->values = this->e_restart_extra;
        if (! writer.config.use_relative_e_distances) {
            this->e_position.assign(MAXIMUM_EXTRUDER_NUMBER, 0);
            for (const Extruder &e : extruders)
                this->e_position[e.id()] = e.position();
            this->opt_e_position->values = this->e_position;
        }
    }
}

// Throw if any of the output vector variables were resized by the script.
void GCode::PlaceholderParserIntegration::validate_output_vector_variables()
{
    if (this->opt_position->values.size() != 3)
        throw Slic3r::RuntimeError("\"position\" output variable must not be resized by the script.");
    if (this->num_extruders > 0) {
        if (this->opt_e_position && this->opt_e_position->values.size() != MAXIMUM_EXTRUDER_NUMBER)
            throw Slic3r::RuntimeError("\"e_position\" output variable must not be resized by the script.");
        if (this->opt_e_retracted->values.size() != MAXIMUM_EXTRUDER_NUMBER)
            throw Slic3r::RuntimeError("\"e_retracted\" output variable must not be resized by the script.");
        if (this->opt_e_restart_extra->values.size() != MAXIMUM_EXTRUDER_NUMBER)
            throw Slic3r::RuntimeError("\"e_restart_extra\" output variable must not be resized by the script.");
    }
}

// Collect pairs of object_layer + support_layer sorted by print_z.
// object_layer & support_layer are considered to be on the same print_z, if they are not further than EPSILON.
std::vector<GCode::LayerToPrint> GCode::collect_layers_to_print(const PrintObject& object)
{
    std::vector<GCode::LayerToPrint> layers_to_print;
    layers_to_print.reserve(object.layers().size() + object.support_layers().size());

    /*
    // Calculate a minimum support layer height as a minimum over all extruders, but not smaller than 10um.
    // This is the same logic as in support generator.
    //FIXME should we use the printing extruders instead?
    double gap_over_supports = object.config().support_top_z_distance;
    // FIXME should we test object.config().support_material_synchronize_layers ? Currently the support layers are synchronized with object layers iff soluble supports.
    assert(!object.has_support() || gap_over_supports != 0. || object.config().support_material_synchronize_layers);
    if (gap_over_supports != 0.) {
        gap_over_supports = std::max(0., gap_over_supports);
        // Not a soluble support,
        double support_layer_height_min = 1000000.;
        for (auto lh : object.print()->config().min_layer_height.values)
            support_layer_height_min = std::min(support_layer_height_min, std::max(0.01, lh));
        gap_over_supports += support_layer_height_min;
    }*/

    std::vector<std::pair<double, double>> warning_ranges;

    // Pair the object layers with the support layers by z.
    size_t idx_object_layer = 0;
    size_t idx_support_layer = 0;
    const LayerToPrint* last_extrusion_layer = nullptr;
    while (idx_object_layer < object.layers().size() || idx_support_layer < object.support_layers().size()) {
        LayerToPrint layer_to_print;
        double print_z_min = std::numeric_limits<double>::max();
        if (idx_object_layer < object.layers().size()) {
            layer_to_print.object_layer = object.layers()[idx_object_layer++];
            print_z_min = std::min(print_z_min, layer_to_print.object_layer->print_z);
        }

        if (idx_support_layer < object.support_layers().size()) {
            layer_to_print.support_layer = object.support_layers()[idx_support_layer++];
            print_z_min = std::min(print_z_min, layer_to_print.support_layer->print_z);
        }

        if (layer_to_print.object_layer && layer_to_print.object_layer->print_z > print_z_min + EPSILON) {
            layer_to_print.object_layer = nullptr;
            --idx_object_layer;
        }

        if (layer_to_print.support_layer && layer_to_print.support_layer->print_z > print_z_min + EPSILON) {
            layer_to_print.support_layer = nullptr;
            --idx_support_layer;
        }

        layer_to_print.original_object = &object;
        layers_to_print.push_back(layer_to_print);

        bool has_extrusions = (layer_to_print.object_layer && layer_to_print.object_layer->has_extrusions())
            || (layer_to_print.support_layer && layer_to_print.support_layer->has_extrusions());

        // Check that there are extrusions on the very first layer. The case with empty
        // first layer may result in skirt/brim in the air and maybe other issues.
        if (layers_to_print.size() == 1u) {
            if (!has_extrusions)
                throw Slic3r::SlicingError(_(L("One object has empty initial layer and can't be printed. Please Cut the bottom or enable supports.")), object.id().id);
        }

        // In case there are extrusions on this layer, check there is a layer to lay it on.
        if ((layer_to_print.object_layer && layer_to_print.object_layer->has_extrusions())
            // Allow empty support layers, as the support generator may produce no extrusions for non-empty support regions.
            || (layer_to_print.support_layer /* && layer_to_print.support_layer->has_extrusions() */)) {
            double top_cd = object.config().support_top_z_distance;
            double bottom_cd = object.config().support_bottom_z_distance == 0. ? top_cd : object.config().support_bottom_z_distance;
            //if (!object.print()->config().independent_support_layer_height)
            { // the actual support gap may be larger than the configured one due to rounding to layer height for organic support, regardless of independent support layer height
                top_cd    = std::ceil(top_cd / object.config().layer_height) * object.config().layer_height;
                bottom_cd = std::ceil(bottom_cd / object.config().layer_height) * object.config().layer_height;
            }
            double extra_gap = (layer_to_print.support_layer ? bottom_cd : top_cd);

            // raft contact distance should not trigger any warning
            if (last_extrusion_layer && last_extrusion_layer->support_layer) {
                double raft_gap = object.config().raft_contact_distance.value;
                //if (!object.print()->config().independent_support_layer_height)
                {
                    raft_gap = std::ceil(raft_gap / object.config().layer_height) * object.config().layer_height;
                }
                extra_gap = std::max(extra_gap, object.config().raft_contact_distance.value);
            }
            double maximal_print_z = (last_extrusion_layer ? last_extrusion_layer->print_z() : 0.)
                + layer_to_print.layer()->height
                + std::max(0., extra_gap);
            // Negative support_contact_z is not taken into account, it can result in false positives in cases

            if (has_extrusions && layer_to_print.print_z() > maximal_print_z + 2. * EPSILON)
                warning_ranges.emplace_back(std::make_pair((last_extrusion_layer ? last_extrusion_layer->print_z() : 0.), layers_to_print.back().print_z()));
        }
        // Remember last layer with extrusions.
        if (has_extrusions)
            last_extrusion_layer = &layers_to_print.back();
    }

    if (! warning_ranges.empty()) {
        std::string warning;
        size_t i = 0;
        for (i = 0; i < std::min(warning_ranges.size(), size_t(5)); ++i)
            warning += Slic3r::format(_(L("Object can't be printed for empty layer between %1% and %2%.")),
                                      warning_ranges[i].first, warning_ranges[i].second) + "\n";
        warning += Slic3r::format(_(L("Object: %1%")), object.model_object()->name) + "\n"
            + _(L("Maybe parts of the object at these height are too thin, or the object has faulty mesh"));

        const_cast<Print*>(object.print())->active_step_add_warning(
            PrintStateBase::WarningLevel::CRITICAL, warning, PrintStateBase::SlicingEmptyGcodeLayers);
    }

    return layers_to_print;
}

// Prepare for non-sequential printing of multiple objects: Support resp. object layers with nearly identical print_z
// will be printed for  all objects at once.
// Return a list of <print_z, per object LayerToPrint> items.
std::vector<std::pair<coordf_t, std::vector<GCode::LayerToPrint>>> GCode::collect_layers_to_print(const Print& print)
{
    struct OrderingItem {
        coordf_t    print_z;
        size_t      object_idx;
        size_t      layer_idx;
    };

    std::vector<std::vector<LayerToPrint>>  per_object(print.objects().size(), std::vector<LayerToPrint>());
    std::vector<OrderingItem>               ordering;

    std::vector<Slic3r::SlicingError> errors;

    for (size_t i = 0; i < print.objects().size(); ++i) {
        try {
            per_object[i] = collect_layers_to_print(*print.objects()[i]);
        } catch (const Slic3r::SlicingError &e) {
            errors.push_back(e);
            continue;
        }
        OrderingItem ordering_item;
        ordering_item.object_idx = i;
        ordering.reserve(ordering.size() + per_object[i].size());
        const LayerToPrint& front = per_object[i].front();
        for (const LayerToPrint& ltp : per_object[i]) {
            ordering_item.print_z = ltp.print_z();
            ordering_item.layer_idx = &ltp - &front;
            ordering.emplace_back(ordering_item);
        }
    }

    if (!errors.empty()) { throw Slic3r::SlicingErrors(errors); }

    std::sort(ordering.begin(), ordering.end(), [](const OrderingItem& oi1, const OrderingItem& oi2) { return oi1.print_z < oi2.print_z; });

    std::vector<std::pair<coordf_t, std::vector<LayerToPrint>>> layers_to_print;

    // Merge numerically very close Z values.
    for (size_t i = 0; i < ordering.size();) {
        // Find the last layer with roughly the same print_z.
        size_t j = i + 1;
        coordf_t zmax = ordering[i].print_z + EPSILON;
        for (; j < ordering.size() && ordering[j].print_z <= zmax; ++j);
        // Merge into layers_to_print.
        std::pair<coordf_t, std::vector<LayerToPrint>> merged;
        // Assign an average print_z to the set of layers with nearly equal print_z.
        merged.first = 0.5 * (ordering[i].print_z + ordering[j - 1].print_z);
        merged.second.assign(print.objects().size(), LayerToPrint());
        for (; i < j; ++i) {
            const OrderingItem& oi = ordering[i];
            assert(merged.second[oi.object_idx].layer() == nullptr);
            merged.second[oi.object_idx] = std::move(per_object[oi.object_idx][oi.layer_idx]);
        }
        layers_to_print.emplace_back(std::move(merged));
    }

    return layers_to_print;
}

// free functions called by GCode::do_export()
namespace DoExport {
//    static void update_print_estimated_times_stats(const GCodeProcessor& processor, PrintStatistics& print_statistics)
//    {
//        const GCodeProcessorResult& result = processor.get_result();
//        print_statistics.estimated_normal_print_time = get_time_dhms(result.print_statistics.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time);
//        print_statistics.estimated_silent_print_time = processor.is_stealth_time_estimator_enabled() ?
//            get_time_dhms(result.print_statistics.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Stealth)].time) : "N/A";
//    }

    static void update_print_estimated_stats(const GCodeProcessor& processor, const std::vector<Extruder>& extruders, PrintStatistics& print_statistics, const PrintConfig& config)
    {
        const GCodeProcessorResult& result = processor.get_result();
        double normal_print_time = result.print_statistics.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time;
        print_statistics.estimated_normal_print_time = get_time_dhms(normal_print_time);
        print_statistics.estimated_silent_print_time = processor.is_stealth_time_estimator_enabled() ?
            get_time_dhms(result.print_statistics.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Stealth)].time) : "N/A";

        // update filament statictics
        double total_extruded_volume = 0.0;
        double total_used_filament   = 0.0;
        double total_weight          = 0.0;
        double total_cost            = 0.0;

        for (auto volume : result.print_statistics.total_volumes_per_extruder) {
            total_extruded_volume += volume.second;

            size_t extruder_id = volume.first;
            auto extruder = std::find_if(extruders.begin(), extruders.end(), [extruder_id](const Extruder& extr) {return extr.id() == extruder_id; });
            if (extruder == extruders.end())
                continue;

            double s = PI * sqr(0.5* extruder->filament_diameter());
            double weight = volume.second * extruder->filament_density() * 0.001;
            total_used_filament += volume.second/s;
            total_weight        += weight;
            total_cost          += weight * extruder->filament_cost() * 0.001;
        }

        total_cost += config.time_cost.getFloat() * (normal_print_time/3600.0);

        print_statistics.total_extruded_volume = total_extruded_volume;
        print_statistics.total_used_filament   = total_used_filament;
        print_statistics.total_weight          = total_weight;
        print_statistics.total_cost            = total_cost;

        print_statistics.filament_stats = result.print_statistics.model_volumes_per_extruder;
    }

    // if any reserved keyword is found, returns a std::vector containing the first MAX_COUNT keywords found
    // into pairs containing:
    // first: source
    // second: keyword
    // to be shown in the warning notification
    // The returned vector is empty if no keyword has been found
    static std::vector<std::pair<std::string, std::string>> validate_custom_gcode(const Print& print) {
        static const unsigned int MAX_TAGS_COUNT = 5;
        std::vector<std::pair<std::string, std::string>> ret;

        auto check = [&ret](const std::string& source, const std::string& gcode) {
            std::vector<std::string> tags;
            if (GCodeProcessor::contains_reserved_tags(gcode, MAX_TAGS_COUNT, tags)) {
                if (!tags.empty()) {
                    size_t i = 0;
                    while (ret.size() < MAX_TAGS_COUNT && i < tags.size()) {
                        ret.push_back({ source, tags[i] });
                        ++i;
                    }
                }
            }
        };

        const GCodeConfig& config = print.config();
        check(_(L("Machine start G-code")), config.machine_start_gcode.value);
        if (ret.size() < MAX_TAGS_COUNT) check(_(L("Machine end G-code")), config.machine_end_gcode.value);
        if (ret.size() < MAX_TAGS_COUNT) check(_(L("Before layer change G-code")), config.before_layer_change_gcode.value);
        if (ret.size() < MAX_TAGS_COUNT) check(_(L("Layer change G-code")), config.layer_change_gcode.value);
        if (ret.size() < MAX_TAGS_COUNT) check(_(L("Timelapse G-code")), config.time_lapse_gcode.value);
        if (ret.size() < MAX_TAGS_COUNT) check(_(L("Timelapse G-code")), config.wrapping_detection_gcode.value);
        if (ret.size() < MAX_TAGS_COUNT) check(_(L("Change filament G-code")), config.change_filament_gcode.value);
        if (ret.size() < MAX_TAGS_COUNT) check(_(L("Printing by object G-code")), config.printing_by_object_gcode.value);
        //if (ret.size() < MAX_TAGS_COUNT) check(_(L("Color Change G-code")), config.color_change_gcode.value);
        //Orca
        if (ret.size() < MAX_TAGS_COUNT) check(_(L("Change extrusion role G-code")), config.change_extrusion_role_gcode.value);
        if (ret.size() < MAX_TAGS_COUNT) check(_(L("Pause G-code")), config.machine_pause_gcode.value);
        if (ret.size() < MAX_TAGS_COUNT) check(_(L("Template Custom G-code")), config.template_custom_gcode.value);
        if (ret.size() < MAX_TAGS_COUNT) {
            for (const std::string& value : config.filament_start_gcode.values) {
                check(_(L("Filament start G-code")), value);
                if (ret.size() == MAX_TAGS_COUNT)
                    break;
            }
        }
        if (ret.size() < MAX_TAGS_COUNT) {
            for (const std::string& value : config.filament_end_gcode.values) {
                check(_(L("Filament end G-code")), value);
                if (ret.size() == MAX_TAGS_COUNT)
                    break;
            }
        }
        //BBS: no custom_gcode_per_print_z, don't need to check
        //if (ret.size() < MAX_TAGS_COUNT) {
        //    const CustomGCode::Info& custom_gcode_per_print_z = print.model().custom_gcode_per_print_z;
        //    for (const auto& gcode : custom_gcode_per_print_z.gcodes) {
        //        check(_(L("Custom G-code")), gcode.extra);
        //        if (ret.size() == MAX_TAGS_COUNT)
        //            break;
        //    }
        //}

        return ret;
    }
} // namespace DoExport

bool GCode::is_BBL_Printer()
{
    if (m_curr_print)
        return m_curr_print->is_BBL_printer();
    return false;
}

bool GCode::is_QIDI_Printer()
{
    if (m_curr_print)
        return m_curr_print->is_QIDI_printer();
    return false;
}

void GCode::do_export(Print* print, const char* path, GCodeProcessorResult* result, ThumbnailsGeneratorCallback thumbnail_cb)
{
    PROFILE_CLEAR();

    // BBS
    m_curr_print = print;

    GCodeWriter::full_gcode_comment = print->config().gcode_comments;
    CNumericLocalesSetter locales_setter;

    // Does the file exist? If so, we hope that it is still valid.
    if (print->is_step_done(psGCodeExport) && boost::filesystem::exists(boost::filesystem::path(path)))
        return;

    BOOST_LOG_TRIVIAL(info) << boost::format("Will export G-code to %1% soon")%path;

    GCodeProcessor::s_IsBBLPrinter = print->is_BBL_printer();
    m_writer.set_is_bbl_machine(print->is_BBL_printer());
    print->set_started(psGCodeExport);

    // check if any custom gcode contains keywords used by the gcode processor to
    // produce time estimation and gcode toolpaths
    std::vector<std::pair<std::string, std::string>> validation_res = DoExport::validate_custom_gcode(*print);
    if (!validation_res.empty()) {
        std::string reports;
        for (const auto& [source, keyword] : validation_res) {
            reports += source + ": \"" + keyword + "\"\n";
        }
        //print->active_step_add_warning(PrintStateBase::WarningLevel::NON_CRITICAL,
        //    _(L("In the custom G-code were found reserved keywords:")) + "\n" +
        //    reports +
        //    _(L("This may cause problems in g-code visualization and printing time estimation.")));
        std::string temp = "Dangerous keywords in custom Gcode: " + reports + "\nThis may cause problems in g-code visualization and printing time estimation.";
        BOOST_LOG_TRIVIAL(warning) << temp;
    }

    BOOST_LOG_TRIVIAL(info) << "Exporting G-code..." << log_memory_info();

    // Remove the old g-code if it exists.
    boost::nowide::remove(path);

    fs::path file_path(path);
    fs::path folder = file_path.parent_path();
    if (!fs::exists(folder)) {
        fs::create_directory(folder);
        BOOST_LOG_TRIVIAL(error) << "[WARNING]: the parent path " + folder.string() +" is not there, create it!" << std::endl;
    }

    std::string path_tmp(path);
    path_tmp += ".tmp";

    m_processor.initialize(path_tmp);
    m_processor.set_print(print);
    GCodeOutputStream file(boost::nowide::fopen(path_tmp.c_str(), "wb"), m_processor);
    if (! file.is_open()) {
        BOOST_LOG_TRIVIAL(error) << std::string("G-code export to ") + path + " failed.\nCannot open the file for writing.\n" << std::endl;
        if (!fs::exists(folder)) {
            //fs::create_directory(folder);
            BOOST_LOG_TRIVIAL(error) << "the parent path " + folder.string() +" is not there!!!" << std::endl;
        }
        throw Slic3r::RuntimeError(std::string("G-code export to ") + path + " failed.\nCannot open the file for writing.\n");
    }

    try {
        this->_do_export(*print, file, thumbnail_cb);
        file.flush();
        if (file.is_error()) {
            file.close();
            boost::nowide::remove(path_tmp.c_str());
            throw Slic3r::RuntimeError(std::string("G-code export to ") + path + " failed\nIs the disk full?\n");
        }
    } catch (std::exception & /* ex */) {
        // Rethrow on any exception. std::runtime_exception and CanceledException are expected to be thrown.
        // Close and remove the file.
        file.close();
        boost::nowide::remove(path_tmp.c_str());
        throw;
    }
    file.close();

    check_placeholder_parser_failed();

#if ORCA_CHECK_GCODE_PLACEHOLDERS
    if (!m_placeholder_error_messages.empty()){
        std::ostringstream message;
        message << "Some EditGcodeDialog defs were not specified properly. Do so in PrintConfig under SlicingStatesConfigDef:" << std::endl;
        for (const auto& error : m_placeholder_error_messages) {
            message << std::endl << error.first << ": " << std::endl;
            for (const auto& str : error.second)
                message << str << ", ";
            message.seekp(-2, std::ios_base::end);
            message << std::endl;
        }
        throw Slic3r::PlaceholderParserError(message.str());
    }
#endif

    BOOST_LOG_TRIVIAL(debug) << "Start processing gcode, " << log_memory_info();
    // Post-process the G-code to update time stamps.

    m_timelapse_warning_code = 0;
    if (m_config.printer_structure.value == PrinterStructure::psI3 && m_spiral_vase) {
        m_timelapse_warning_code += 1;
    }
    if (m_config.printer_structure.value == PrinterStructure::psI3 && print->config().print_sequence == PrintSequence::ByObject) {
        m_timelapse_warning_code += (1 << 1);
    }
    if (m_config.timelapse_type.value == TimelapseType::tlSmooth && !m_config.enable_prime_tower.value) {
        m_timelapse_warning_code += (1 << 2);
    }
    m_processor.result().timelapse_warning_code = m_timelapse_warning_code;
    m_processor.result().support_traditional_timelapse = m_support_traditional_timelapse;

    bool activate_long_retraction_when_cut = false;
    for (const auto& filament : m_writer.extruders())
        activate_long_retraction_when_cut |= (
            m_config.long_retractions_when_cut.get_at(filament.id())
         && m_config.retraction_distances_when_cut.get_at(filament.id()) > 0
            );

    m_processor.result().long_retraction_when_cut = activate_long_retraction_when_cut;
   
    {   //BBS:check bed and filament compatible
        const ConfigOptionInts *bed_temp_opt = m_config.option<ConfigOptionInts>(get_bed_temp_1st_layer_key(m_config.curr_bed_type));
        std::vector<int> conflict_filament;
        for(auto extruder_id : m_initial_layer_extruders){
            int cur_bed_temp = bed_temp_opt->get_at(extruder_id);
            if (cur_bed_temp == 0) {
                conflict_filament.push_back(extruder_id);
            }
        }

        m_processor.result().filament_printable_reuslt = FilamentPrintableResult(conflict_filament, bed_type_to_gcode_string(m_config.curr_bed_type));
    }
    // check gcode is valid in machine printabele area and multi_extruder printabele area
    int extruder_size = m_print->config().nozzle_diameter.values.size();
    std::vector<Polygons> extruder_unprintable_polys   = m_print->get_extruder_unprintable_polygons();
    Pointfs               plate_printable_area         = m_print->config().printable_area.values;
    Pointfs               wrapping_exclude_area_points = m_print->config().wrapping_exclude_area.values;
    m_processor.check_multi_extruder_gcode_valid(extruder_size, plate_printable_area, m_print->config().printable_height.value, wrapping_exclude_area_points,
                                                 extruder_unprintable_polys, m_print->get_extruder_printable_height(),  m_print->get_filament_maps(),
                                                 m_print->get_physical_unprintable_filaments(m_print->get_slice_used_filaments(false)));

    m_processor.finalize(true);
//    DoExport::update_print_estimated_times_stats(m_processor, print->m_print_statistics);
    DoExport::update_print_estimated_stats(m_processor, m_writer.extruders(), print->m_print_statistics, print->config());
    if (result != nullptr) {
        *result = std::move(m_processor.extract_result());
        // set the filename to the correct value
        result->filename = path;
    }

    //BBS: add some log for error output
    BOOST_LOG_TRIVIAL(debug) << boost::format("Finished processing gcode to %1% ") % path_tmp;

    std::error_code ret = rename_file(path_tmp, path);
    if (ret) {
        throw Slic3r::RuntimeError(
            std::string("Failed to rename the output G-code file from ") + path_tmp + " to " + path + '\n' + "error code " + ret.message() + '\n' +
            "Is " + path_tmp + " locked?" + '\n');
    }
    else {
        BOOST_LOG_TRIVIAL(info) << boost::format("rename_file from %1% to %2% successfully")% path_tmp % path;
    }

    BOOST_LOG_TRIVIAL(info) << "Exporting G-code finished" << log_memory_info();
    print->set_done(psGCodeExport);
    
    if(is_BBL_Printer())
        result->label_object_enabled = m_enable_exclude_object;
    // Write the profiler measurements to file
    PROFILE_UPDATE();
    PROFILE_OUTPUT(debug_out_path("gcode-export-profile.txt").c_str());
}

// free functions called by GCode::_do_export()
namespace DoExport {
    static void init_gcode_processor(const PrintConfig& config, GCodeProcessor& processor, bool& silent_time_estimator_enabled)
    {
        silent_time_estimator_enabled = (config.gcode_flavor == gcfMarlinLegacy || config.gcode_flavor == gcfMarlinFirmware)
                                        && config.silent_mode;
        processor.reset();
        processor.apply_config(config);
        processor.enable_stealth_time_estimator(silent_time_estimator_enabled);
    }

#if 0
	static double autospeed_volumetric_limit(const Print &print)
	{
	    // get the minimum cross-section used in the print
	    std::vector<double> mm3_per_mm;
	    for (auto object : print.objects()) {
	        for (size_t region_id = 0; region_id < object->num_printing_regions(); ++ region_id) {
	            const PrintRegion &region = object->printing_region(region_id);
	            for (auto layer : object->layers()) {
	                const LayerRegion* layerm = layer->regions()[region_id];
	                if (region.config().get_abs_value("inner_wall_speed") == 0 ||
                        // BBS: remove small small_perimeter_speed config, and will absolutely
                        // remove related code if no other issue in the coming release.
	                    //region.config().get_abs_value("small_perimeter_speed") == 0 ||
	                    region.config().outer_wall_speed.value == 0 ||
	                    region.config().get_abs_value("bridge_speed") == 0)
	                    mm3_per_mm.push_back(layerm->perimeters.min_mm3_per_mm());
	                if (region.config().get_abs_value("sparse_infill_speed") == 0 ||
	                    region.config().get_abs_value("internal_solid_infill_speed") == 0 ||
	                    region.config().get_abs_value("top_surface_speed") == 0 ||
                        region.config().get_abs_value("bridge_speed") == 0)
                    {
                        // Minimal volumetric flow should not be calculated over ironing extrusions.
                        // Use following lambda instead of the built-it method.
                        auto min_mm3_per_mm_no_ironing = [](const ExtrusionEntityCollection& eec) -> double {
                            double min = std::numeric_limits<double>::max();
                            for (const ExtrusionEntity* ee : eec.entities)
                                if (ee->role() != erIroning)
                                    min = std::min(min, ee->min_mm3_per_mm());
                            return min;
                        };

                        mm3_per_mm.push_back(min_mm3_per_mm_no_ironing(layerm->fills));
                    }
	            }
	        }
	        if (object->config().get_abs_value("support_speed") == 0 ||
	            object->config().get_abs_value("support_interface_speed") == 0)
	            for (auto layer : object->support_layers())
	                mm3_per_mm.push_back(layer->support_fills.min_mm3_per_mm());
	    }
	    // filter out 0-width segments
	    mm3_per_mm.erase(std::remove_if(mm3_per_mm.begin(), mm3_per_mm.end(), [](double v) { return v < 0.000001; }), mm3_per_mm.end());
	    double volumetric_speed = 0.;
	    if (! mm3_per_mm.empty()) {
	        // In order to honor max_print_speed we need to find a target volumetric
	        // speed that we can use throughout the print. So we define this target
	        // volumetric speed as the volumetric speed produced by printing the
	        // smallest cross-section at the maximum speed: any larger cross-section
	        // will need slower feedrates.
	        volumetric_speed = *std::min_element(mm3_per_mm.begin(), mm3_per_mm.end()) * print.config().max_print_speed.value;
	        // limit such volumetric speed with max_volumetric_speed if set
            //BBS
	        //if (print.config().max_volumetric_speed.value > 0)
	        //    volumetric_speed = std::min(volumetric_speed, print.config().max_volumetric_speed.value);
	    }
	    return volumetric_speed;
	}
#endif

    static void init_ooze_prevention(const Print &print, OozePrevention &ooze_prevention)
	{
	    ooze_prevention.enable = print.config().ooze_prevention.value && ! print.config().single_extruder_multi_material;
	}

	// Fill in print_statistics and return formatted string containing filament statistics to be inserted into G-code comment section.
    static std::string update_print_stats_and_format_filament_stats(
        const bool                   has_wipe_tower,
	    const WipeTowerData         &wipe_tower_data,
	    const std::vector<Extruder> &extruders,
		PrintStatistics 		    &print_statistics)
    {
		std::string filament_stats_string_out;

	    print_statistics.clear();
        print_statistics.total_toolchanges = std::max(0, wipe_tower_data.number_of_toolchanges);
	    if (! extruders.empty()) {
	        std::pair<std::string, unsigned int> out_filament_used_mm ("; filament used [mm] = ", 0);
	        std::pair<std::string, unsigned int> out_filament_used_cm3("; filament used [cm3] = ", 0);
	        std::pair<std::string, unsigned int> out_filament_used_g  ("; filament used [g] = ", 0);
	        std::pair<std::string, unsigned int> out_filament_cost    ("; filament cost = ", 0);
	        for (const Extruder &extruder : extruders) {
	            double used_filament   = extruder.used_filament() + (has_wipe_tower ? wipe_tower_data.used_filament[extruder.id()] : 0.f);
	            double extruded_volume = extruder.extruded_volume() + (has_wipe_tower ? wipe_tower_data.used_filament[extruder.id()] * 2.4052f : 0.f); // assumes 1.75mm filament diameter
	            double filament_weight = extruded_volume * extruder.filament_density() * 0.001;
	            double filament_cost   = filament_weight * extruder.filament_cost()    * 0.001;
                auto append = [&extruder](std::pair<std::string, unsigned int> &dst, const char *tmpl, double value) {
                    assert(is_decimal_separator_point());
	                while (dst.second < extruder.id()) {
	                    // Fill in the non-printing extruders with zeros.
	                    dst.first += (dst.second > 0) ? ", 0" : "0";
	                    ++ dst.second;
	                }
	                if (dst.second > 0)
	                    dst.first += ", ";
	                char buf[64];
					sprintf(buf, tmpl, value);
	                dst.first += buf;
	                ++ dst.second;
	            };
	            append(out_filament_used_mm,  "%.2lf", used_filament);
	            append(out_filament_used_cm3, "%.2lf", extruded_volume * 0.001);
	            if (filament_weight > 0.) {
	                print_statistics.total_weight = print_statistics.total_weight + filament_weight;
	                append(out_filament_used_g, "%.2lf", filament_weight);
	                if (filament_cost > 0.) {
	                    print_statistics.total_cost = print_statistics.total_cost + filament_cost;
	                    append(out_filament_cost, "%.2lf", filament_cost);
	                }
	            }
	            print_statistics.total_used_filament += used_filament;
	            print_statistics.total_extruded_volume += extruded_volume;
	            print_statistics.total_wipe_tower_filament += has_wipe_tower ? used_filament - extruder.used_filament() : 0.;
	            print_statistics.total_wipe_tower_cost += has_wipe_tower ? (extruded_volume - extruder.extruded_volume())* extruder.filament_density() * 0.001 * extruder.filament_cost() * 0.001 : 0.;
	        }
	        filament_stats_string_out += out_filament_used_mm.first;
            filament_stats_string_out += "\n" + out_filament_used_cm3.first;
            if (out_filament_used_g.second)
                filament_stats_string_out += "\n" + out_filament_used_g.first;
            if (out_filament_cost.second)
               filament_stats_string_out += "\n" + out_filament_cost.first;
            filament_stats_string_out += "\n";
        }
        return filament_stats_string_out;
    }
}

#if 0
// Sort the PrintObjects by their increasing Z, likely useful for avoiding colisions on Deltas during sequential prints.
static inline std::vector<const PrintInstance*> sort_object_instances_by_max_z(const Print &print)
{
    std::vector<const PrintObject*> objects(print.objects().begin(), print.objects().end());
    std::sort(objects.begin(), objects.end(), [](const PrintObject *po1, const PrintObject *po2) { return po1->height() < po2->height(); });
    std::vector<const PrintInstance*> instances;
    instances.reserve(objects.size());
    for (const PrintObject *object : objects)
        for (size_t i = 0; i < object->instances().size(); ++ i)
            instances.emplace_back(&object->instances()[i]);
    return instances;
}
#endif

// Produce a vector of PrintObjects in the order of their respective ModelObjects in print.model().
//BBS: add sort logic for seq-print
std::vector<const PrintInstance*> sort_object_instances_by_model_order(const Print& print, bool init_order)
{
    auto find_object_index = [](const Model& model, const ModelObject* obj) {
        for (int index = 0; index < model.objects.size(); index++)
        {
            if (model.objects[index] == obj)
                return index;
        }
        return -1;
    };

    // Build up map from ModelInstance* to PrintInstance*
    std::vector<std::pair<const ModelInstance*, const PrintInstance*>> model_instance_to_print_instance;
    model_instance_to_print_instance.reserve(print.num_object_instances());
    for (const PrintObject *print_object : print.objects())
        for (const PrintInstance &print_instance : print_object->instances())
        {
            if (init_order)
                const_cast<ModelInstance*>(print_instance.model_instance)->arrange_order = find_object_index(print.model(), print_object->model_object());
            model_instance_to_print_instance.emplace_back(print_instance.model_instance, &print_instance);
        }
    std::sort(model_instance_to_print_instance.begin(), model_instance_to_print_instance.end(), [](auto &l, auto &r) { return l.first->arrange_order < r.first->arrange_order; });
    if (init_order) {
        // Re-assign the arrange_order so each instance has a unique order number
        for (int k = 0; k < model_instance_to_print_instance.size(); k++) {
            const_cast<ModelInstance*>(model_instance_to_print_instance[k].first)->arrange_order = k + 1;
        }
    }

    std::vector<const PrintInstance*> instances;
    instances.reserve(model_instance_to_print_instance.size());
    for (const ModelObject *model_object : print.model().objects)
        for (const ModelInstance *model_instance : model_object->instances) {
            auto it = std::lower_bound(model_instance_to_print_instance.begin(), model_instance_to_print_instance.end(), std::make_pair(model_instance, nullptr), [](auto &l, auto &r) { return l.first->arrange_order < r.first->arrange_order; });
            if (it != model_instance_to_print_instance.end() && it->first == model_instance)
                instances.emplace_back(it->second);
        }
    std::sort(instances.begin(), instances.end(), [](auto& l, auto& r) { return l->model_instance->arrange_order < r->model_instance->arrange_order; });
    return instances;
}

enum BambuBedType {
    bbtUnknown = 0,
    bbtCoolPlate = 1,
    bbtEngineeringPlate = 2,
    bbtHighTemperaturePlate = 3,
    bbtTexturedPEIPlate         = 4,
    bbtSuperTackPlate = 5,
};

static BambuBedType to_bambu_bed_type(BedType type)
{
    BambuBedType bambu_bed_type = bbtUnknown;
    if (type == btPC)
        bambu_bed_type = bbtCoolPlate;
    else if (type == btEP)
        bambu_bed_type = bbtEngineeringPlate;
    else if (type == btPEI)
        bambu_bed_type = bbtHighTemperaturePlate;
    else if (type == btPTE)
        bambu_bed_type = bbtTexturedPEIPlate;
    else if (type == btPCT)
        bambu_bed_type = bbtCoolPlate;
    else if (type == btSuperTack)
        bambu_bed_type = bbtSuperTackPlate;

    return bambu_bed_type;
}

void GCode::_do_export(Print& print, GCodeOutputStream &file, ThumbnailsGeneratorCallback thumbnail_cb)
{
    PROFILE_FUNC();

    m_print = &print;
    m_timelapse_pos_picker.init(&print,m_writer.get_xy_offset().cast<coord_t>());

    // modifies m_silent_time_estimator_enabled
    DoExport::init_gcode_processor(print.config(), m_processor, m_silent_time_estimator_enabled);
    const bool is_bbl_printers = print.is_BBL_printer();
    const bool is_qidi_printers = print.is_QIDI_printer();
    m_calib_config.clear();
    // resets analyzer's tracking data
    m_last_height  = 0.f;
    m_last_layer_z = 0.f;
    m_max_layer_z  = 0.f;
    m_last_width = 0.f;
    m_is_role_based_fan_on.fill(false);
#if ENABLE_GCODE_VIEWER_DATA_CHECKING
    m_last_mm3_per_mm = 0.;
#endif // ENABLE_GCODE_VIEWER_DATA_CHECKING

    m_fan_mover.release();
    
    m_writer.set_is_bbl_machine(is_bbl_printers);

    // How many times will be change_layer() called?
    // change_layer() in turn increments the progress bar status.
    m_layer_count = 0;
    if (print.config().print_sequence == PrintSequence::ByObject) {
        // Add each of the object's layers separately.
        for (auto object : print.objects()) {
            std::vector<coordf_t> zs;
            zs.reserve(object->layers().size() + object->support_layers().size());
            for (auto layer : object->layers())
                zs.push_back(layer->print_z);
            for (auto layer : object->support_layers())
                zs.push_back(layer->print_z);
            std::sort(zs.begin(), zs.end());
            //BBS: merge numerically very close Z values.
            auto end_it = std::unique(zs.begin(), zs.end());
            unsigned int temp_layer_count = (unsigned int)(end_it - zs.begin());
            for (auto it = zs.begin(); it != end_it - 1; it++) {
                if (abs(*it - *(it + 1)) < EPSILON)
                    temp_layer_count--;
            }
            m_layer_count += (unsigned int)(object->instances().size() * temp_layer_count);
        }
    } else {
        // Print all objects with the same print_z together.
        std::vector<coordf_t> zs;
        for (auto object : print.objects()) {
            zs.reserve(zs.size() + object->layers().size() + object->support_layers().size());
            for (auto layer : object->layers())
                zs.push_back(layer->print_z);
            for (auto layer : object->support_layers())
                zs.push_back(layer->print_z);
        }
        if (!zs.empty())
        {
            std::sort(zs.begin(), zs.end());
            //BBS: merge numerically very close Z values.
            auto end_it = std::unique(zs.begin(), zs.end());
            m_layer_count = (unsigned int)(end_it - zs.begin());
            for (auto it = zs.begin(); it != end_it - 1; it++) {
                if (abs(*it - *(it + 1)) < EPSILON)
                    m_layer_count--;
            }
        }
    }
    print.throw_if_canceled();

    m_enable_cooling_markers = true;
    this->apply_print_config(print.config());
    m_config.apply(print.default_object_config());
    m_config.apply(print.default_region_config());

    //m_volumetric_speed = DoExport::autospeed_volumetric_limit(print);
    print.throw_if_canceled();

    if (print.config().spiral_mode.value)
        m_spiral_vase = make_unique<SpiralVase>(print.config());

    if (print.config().max_volumetric_extrusion_rate_slope.value > 0){
    		m_pressure_equalizer = make_unique<PressureEqualizer>(print.config());
    		m_enable_extrusion_role_markers = (bool)m_pressure_equalizer;
    } else
	    m_enable_extrusion_role_markers = false;

    if (!print.config().small_area_infill_flow_compensation_model.empty())
        m_small_area_infill_flow_compensator = make_unique<SmallAreaInfillFlowCompensator>(print.config());
    
    // Orca: Don't output Header block if BTT thumbnail is identified in the list
    // Get the thumbnails value as a string
    std::string thumbnails_value = print.config().option<ConfigOptionString>("thumbnails")->value;
    // search string for the BTT_TFT label
    bool has_BTT_thumbnail = (thumbnails_value.find("BTT_TFT") != std::string::npos);
    
    if(!has_BTT_thumbnail){   
        file.write_format("; HEADER_BLOCK_START\n");
        // Write information on the generator.
        file.write_format("; generated by %s on %s\n", Slic3r::header_slic3r_generated().c_str(), Slic3r::Utils::local_timestamp().c_str());
        if (is_bbl_printers)
            file.write_format(";%s\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Estimated_Printing_Time_Placeholder).c_str());
        //BBS: total layer number
        file.write_format(";%s\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Total_Layer_Number_Placeholder).c_str());
        //Orca: extra check for bbl printer
        if (is_bbl_printers) {
            if (print.calib_params().mode == CalibMode::Calib_None) { // Don't support skipping in cali mode
                // list all label_object_id with sorted order here
                m_enable_exclude_object = true;
                m_label_objects_ids.clear();
                m_label_objects_ids.reserve(print.num_object_instances());
                for (const PrintObject *print_object : print.objects())
                    for (const PrintInstance &print_instance : print_object->instances())
                        m_label_objects_ids.push_back(print_instance.model_instance->get_labeled_id());
  
                std::sort(m_label_objects_ids.begin(), m_label_objects_ids.end());
  
                std::string objects_id_list = "; model label id: ";
                for (auto it = m_label_objects_ids.begin(); it != m_label_objects_ids.end(); it++)
                    objects_id_list += (std::to_string(*it) + (it != m_label_objects_ids.end() - 1 ? "," : "\n"));
                file.writeln(objects_id_list);
            } else {
                m_enable_exclude_object = false;
                m_label_objects_ids.clear();
            }
    }

    {
        std::string filament_density_list = "; filament_density: ";
        (filament_density_list+=m_config.filament_density.serialize()) +='\n';
        file.writeln(filament_density_list);

        std::string filament_diameter_list = "; filament_diameter: ";
        (filament_diameter_list += m_config.filament_diameter.serialize()) += '\n';
        file.writeln(filament_diameter_list);

        coordf_t max_height_z = -1;
        for (const auto& object : print.objects())
            max_height_z = std::max(object->layers().back()->print_z, max_height_z);

        std::ostringstream max_height_z_tip;
        max_height_z_tip<<"; max_z_height: " << std::fixed << std::setprecision(2) << max_height_z << '\n';
        file.writeln(max_height_z_tip.str());
    }

    {
        auto used_filaments = print.get_slice_used_filaments(false);
        std::ostringstream out;
        out << "; filament: ";
        for (size_t idx = 0; idx < used_filaments.size(); ++idx) {
            if (idx != 0)
                out << ',';
            out << used_filaments[idx] + 1;
        }
        file.writeln(out.str());
    }

    file.write_format("; HEADER_BLOCK_END\n\n");
    }
    
      // BBS: write global config at the beginning of gcode file because printer
      // need these config information
      // Append full config, delimited by two 'phony' configuration keys
      // CONFIG_BLOCK_START and CONFIG_BLOCK_END. The delimiters are structured
      // as configuration key / value pairs to be parsable by older versions of
      // PrusaSlicer G-code viewer.
    {
        if (is_bbl_printers) {
            file.write("; CONFIG_BLOCK_START\n");
            std::string full_config;
            append_full_config(print, full_config);
            if (!full_config.empty())
                file.write(full_config);

            // SoftFever: write compatiple image
            int first_layer_bed_temperature = get_bed_temperature(0, true, print.config().curr_bed_type);
            file.write_format("; first_layer_bed_temperature = %d\n",
                                first_layer_bed_temperature);
            file.write_format(
                "; first_layer_temperature = %d\n",
                print.config().nozzle_temperature_initial_layer.get_at(0));
            file.write("; CONFIG_BLOCK_END\n\n");
        } else if (thumbnail_cb != nullptr) {
            // generate the thumbnails
            auto [thumbnails, errors] = GCodeThumbnails::make_and_check_thumbnail_list(print.full_print_config());

            if (errors != enum_bitmask<ThumbnailError>()) {
                std::string error_str = format("Invalid thumbnails value:");
                error_str += GCodeThumbnails::get_error_string(errors);
                throw Slic3r::ExportError(error_str);
            }

            if (!thumbnails.empty())
                GCodeThumbnails::export_thumbnails_to_file(
                    thumbnail_cb, print.get_plate_index(), thumbnails, [&file](const char* sz) { file.write(sz); }, [&print]() { print.throw_if_canceled(); });
        }
    }


    // Write some terse information on the slicing parameters.
    const PrintObject *first_object         = print.objects().front();
    const double       layer_height         = first_object->config().layer_height.value;
    const double       initial_layer_print_height   = print.config().initial_layer_print_height.value;
    for (size_t region_id = 0; region_id < print.num_print_regions(); ++ region_id) {
        const PrintRegion &region = print.get_print_region(region_id);
        file.write_format("; external perimeters extrusion width = %.2fmm\n", region.flow(*first_object, frExternalPerimeter, layer_height).width());
        file.write_format("; perimeters extrusion width = %.2fmm\n",          region.flow(*first_object, frPerimeter,         layer_height).width());
        file.write_format("; infill extrusion width = %.2fmm\n",              region.flow(*first_object, frInfill,            layer_height).width());
        file.write_format("; solid infill extrusion width = %.2fmm\n",        region.flow(*first_object, frSolidInfill,       layer_height).width());
        file.write_format("; top infill extrusion width = %.2fmm\n",          region.flow(*first_object, frTopSolidInfill,    layer_height).width());
        if (print.has_support_material())
            file.write_format("; support material extrusion width = %.2fmm\n", support_material_flow(first_object).width());
        if (print.config().initial_layer_line_width.value > 0)
            file.write_format("; first layer extrusion width = %.2fmm\n",   region.flow(*first_object, frPerimeter, initial_layer_print_height, true).width());
        file.write_format("\n");
    }

    file.write_format("; EXECUTABLE_BLOCK_START\n");

    // SoftFever
    if( m_enable_exclude_object)
        file.write(set_object_info(&print));

    // adds tags for time estimators
    file.write_format(";%s\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::First_Line_M73_Placeholder).c_str());

    // Prepare the helper object for replacing placeholders in custom G-code and output filename.
    m_placeholder_parser_integration.parser = print.placeholder_parser();
    m_placeholder_parser_integration.parser.update_timestamp();
    m_placeholder_parser_integration.parser.update_user_name();
    m_placeholder_parser_integration.context.rng = std::mt19937(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    // Enable passing global variables between PlaceholderParser invocations.
    m_placeholder_parser_integration.context.global_config = std::make_unique<DynamicConfig>();
    print.update_object_placeholders(m_placeholder_parser_integration.parser.config_writable(), ".gcode");

    // Get optimal tool ordering to minimize tool switches of a multi-exruder print.
    // For a print by objects, find the 1st printing object.
    ToolOrdering tool_ordering;
    unsigned int initial_extruder_id = (unsigned int)-1;
    //BBS: first non-support filament extruder
    unsigned int initial_non_support_extruder_id = (unsigned int) -1;
    unsigned int final_extruder_id   = (unsigned int)-1;
    bool         has_wipe_tower      = false;
    print.m_statistics_by_extruder_count.clear();
    std::vector<int>                                    first_filaments;
    std::vector<int>                                    first_non_support_filaments;
    std::vector<const PrintInstance*> 					print_object_instances_ordering;
    std::vector<const PrintInstance*>::const_iterator 	print_object_instance_sequential_active;
    std::vector<const PrintInstance *>::const_iterator  first_has_extrude_print_object;
    //resize
    first_non_support_filaments.resize(print.config().nozzle_diameter.size(), -1);
    first_filaments.resize(print.config().nozzle_diameter.size(), -1);
    float max_additional_fan = 0.f;
    if (print.config().print_sequence == PrintSequence::ByObject) {
        // Order object instances for sequential print.
        print_object_instances_ordering = sort_object_instances_by_model_order(print);
//        print_object_instances_ordering = sort_object_instances_by_max_z(print);
        // Find the 1st printing object, find its tool ordering and the initial extruder ID.
        print_object_instance_sequential_active = print_object_instances_ordering.begin();
        first_has_extrude_print_object          = print_object_instance_sequential_active;
        bool find_fist_non_support_filament = false;
        for (; print_object_instance_sequential_active != print_object_instances_ordering.end(); ++ print_object_instance_sequential_active) {
            tool_ordering = ToolOrdering(*(*print_object_instance_sequential_active)->print_object, initial_extruder_id);

            tool_ordering.sort_and_build_data(*(*print_object_instance_sequential_active)->print_object,initial_extruder_id);
            float temp_max_additional_fan = tool_ordering.cal_max_additional_fan(print.config());
            if(temp_max_additional_fan > max_additional_fan )
                        max_additional_fan = temp_max_additional_fan;
            if (!find_fist_non_support_filament && tool_ordering.first_extruder() != (unsigned int) -1) {
                //BBS: try to find the non-support filament extruder if is multi color and initial_extruder is support filament
                if (initial_extruder_id == (unsigned int) -1) {
                    initial_extruder_id = tool_ordering.first_extruder();
                    first_has_extrude_print_object = print_object_instance_sequential_active;
                }

                find_fist_non_support_filament = tool_ordering.cal_non_support_filaments(print.config(), initial_non_support_extruder_id, first_non_support_filaments, first_filaments);
            }
        }
        if (initial_extruder_id == static_cast<unsigned int>(-1))
            // No object to print was found, cancel the G-code export.
            throw Slic3r::SlicingError(_(L("No object can be printed. Maybe too small")));
        // We don't allow switching of extruders per layer by Model::custom_gcode_per_print_z in sequential mode.
        // Use the extruder IDs collected from Regions.
        this->set_extruders(print.extruders());

        has_wipe_tower = print.has_wipe_tower() && tool_ordering.has_wipe_tower();
    } else {
        // Find tool ordering for all the objects at once, and the initial extruder ID.
        // If the tool ordering has been pre-calculated by Print class for wipe tower already, reuse it.
        tool_ordering = print.tool_ordering();
        tool_ordering.assign_custom_gcodes(print);
        float temp_max_additional_fan = tool_ordering.cal_max_additional_fan(print.config());
        if(temp_max_additional_fan > max_additional_fan )
                        max_additional_fan = temp_max_additional_fan;
        if (tool_ordering.all_extruders().empty())
            // No object to print was found, cancel the G-code export.
            throw Slic3r::SlicingError(_(L("No object can be printed. Maybe too small")));
        has_wipe_tower = print.has_wipe_tower() && tool_ordering.has_wipe_tower();
        // Orca: support all extruder priming
        initial_extruder_id = (!is_bbl_printers && has_wipe_tower && !print.config().single_extruder_multi_material_priming && !is_qidi_printers) ?
            // The priming towers will be skipped.
            tool_ordering.all_extruders().back() :
            // Don't skip the priming towers.
            tool_ordering.first_extruder();

        //BBS: try to find the non-support filament extruder if is multi color and initial_extruder is support filament
        if (initial_extruder_id != static_cast<unsigned int>(-1)) {
            // BBS: try to find the non-support filament extruder if is multi color and initial_extruder is support filament
            // check if has non support filaments
            tool_ordering.cal_non_support_filaments(print.config(), initial_non_support_extruder_id, first_non_support_filaments, first_filaments);
        }

        // In non-sequential print, the printing extruders may have been modified by the extruder switches stored in Model::custom_gcode_per_print_z.
        // Therefore initialize the printing extruders from there.
        this->set_extruders(tool_ordering.all_extruders());
        print_object_instances_ordering = 
            // By default, order object instances using a nearest neighbor search.
            print.config().print_order == PrintOrder::Default ? chain_print_object_instances(print)
            // Otherwise same order as the object list
            : sort_object_instances_by_model_order(print);
    }
    if (initial_extruder_id == (unsigned int)-1) {
        // Nothing to print!
        initial_extruder_id = 0;
        initial_non_support_extruder_id = 0;
    }

    //could not find non support filmanet, use fisrt print filament
    if (initial_non_support_extruder_id == (unsigned int) -1)
        initial_non_support_extruder_id = initial_extruder_id;

    print.throw_if_canceled();

    m_cooling_buffer = make_unique<CoolingBuffer>(*this);
    m_cooling_buffer->set_current_extruder(initial_extruder_id);

    int extruder_id = get_extruder_id(initial_extruder_id);

    // Orca: Initialise AdaptivePA processor filter
    m_pa_processor = std::make_unique<AdaptivePAProcessor>(*this, tool_ordering.all_extruders());

    // Emit machine envelope limits for the Marlin firmware.
    this->print_machine_envelope(file, print, initial_extruder_id);

    // Disable fan.
    if (m_config.auxiliary_fan.value && print.config().close_fan_the_first_x_layers.get_at(initial_extruder_id)) {
        file.write(m_writer.set_fan(0));
        //BBS: disable additional fan
        file.write(m_writer.set_additional_fan(0));
    }

    // Update output variables after the extruders were initialized.
    m_placeholder_parser_integration.init(m_writer);

    // Let the start-up script prime the 1st printing tool.

    auto match_physical_extruder_for_each_filament = [](std::vector<int> &filaments, const FullPrintConfig &config) {
        // match the filament to the physical extruder
        std::vector<int> physicial_first_filaments;
        physicial_first_filaments.resize(filaments.size());
        for (int extruder_id = 0; extruder_id < filaments.size(); extruder_id++) {
            physicial_first_filaments[config.physical_extruder_map.get_at(extruder_id)] = filaments[extruder_id];
        }
        filaments = physicial_first_filaments;
    };
    match_physical_extruder_for_each_filament(first_filaments, m_config);
    this->placeholder_parser().set("first_tools", new ConfigOptionInts(first_filaments));
    this->placeholder_parser().set("first_filaments", new ConfigOptionInts(first_filaments));
    this->placeholder_parser().set("initial_tool", initial_extruder_id);
    this->placeholder_parser().set("initial_extruder", initial_extruder_id);
    //BBS
    match_physical_extruder_for_each_filament(first_non_support_filaments, m_config);

    this->placeholder_parser().set("first_non_support_tools", new ConfigOptionInts(first_non_support_filaments));
    this->placeholder_parser().set("first_non_support_filaments", new ConfigOptionInts(first_non_support_filaments));
    this->placeholder_parser().set("initial_no_support_tool", initial_non_support_extruder_id);
    this->placeholder_parser().set("initial_no_support_extruder", initial_non_support_extruder_id);
    this->placeholder_parser().set("current_extruder", initial_extruder_id);
    //Orca: set the key for compatibilty
    this->placeholder_parser().set("retraction_distance_when_cut", m_config.retraction_distances_when_cut.get_at(initial_extruder_id));
    this->placeholder_parser().set("long_retraction_when_cut", m_config.long_retractions_when_cut.get_at(initial_extruder_id));
    this->placeholder_parser().set("retraction_distance_when_ec", m_config.retraction_distances_when_ec.get_at(initial_extruder_id));
    this->placeholder_parser().set("long_retraction_when_ec", m_config.long_retractions_when_ec.get_at(initial_extruder_id));
    this->placeholder_parser().set("temperature", new ConfigOptionInts(print.config().nozzle_temperature));


    this->placeholder_parser().set("retraction_distances_when_cut", new ConfigOptionFloats(m_config.retraction_distances_when_cut));
    this->placeholder_parser().set("long_retractions_when_cut",new ConfigOptionBools(m_config.long_retractions_when_cut));
    this->placeholder_parser().set("retraction_distances_when_ec", new ConfigOptionFloatsNullable(m_config.retraction_distances_when_ec));
    this->placeholder_parser().set("long_retractions_when_ec",new ConfigOptionBoolsNullable(m_config.long_retractions_when_ec));

    this->placeholder_parser().set("max_additional_fan", max_additional_fan);
    this->placeholder_parser().set("first_x_layer_fan_speed", 0); // TODO: Orca hack to support BBL profiles

    auto flush_v_speed = m_config.filament_flush_volumetric_speed.values;
    auto flush_temps = m_config.filament_flush_temp.values;
    for (size_t idx = 0; idx < flush_v_speed.size(); ++idx) {
        if (flush_v_speed[idx] == 0)
            flush_v_speed[idx] = m_config.filament_max_volumetric_speed.get_at(idx);
    }
    for (size_t idx = 0; idx < flush_temps.size(); ++idx) {
        if (flush_temps[idx] == 0)
            flush_temps[idx] = m_config.nozzle_temperature_range_high.get_at(idx);
    }
    this->placeholder_parser().set("flush_volumetric_speeds", new ConfigOptionFloats(flush_v_speed));
    this->placeholder_parser().set("flush_temperatures", new ConfigOptionInts(flush_temps));
    //Set variable for total layer count so it can be used in custom gcode.
    this->placeholder_parser().set("total_layer_count", m_layer_count);
    // Useful for sequential prints.
    this->placeholder_parser().set("current_object_idx", 0);
    // For the start / end G-code to do the priming and final filament pull in case there is no wipe tower provided.
    this->placeholder_parser().set("has_wipe_tower", has_wipe_tower);
    this->placeholder_parser().set("has_single_extruder_multi_material_priming", !is_bbl_printers && has_wipe_tower && print.config().single_extruder_multi_material_priming);
    this->placeholder_parser().set("total_toolchanges", std::max(0, print.wipe_tower_data().number_of_toolchanges)); // Check for negative toolchanges (single extruder mode) and set to 0 (no tool change).
    this->placeholder_parser().set("num_extruders", int(print.config().nozzle_diameter.values.size()));
    this->placeholder_parser().set("retract_length", new ConfigOptionFloats(print.config().retraction_length));

    //Orca: support max MAXIMUM_EXTRUDER_NUMBER extruders/filaments
    std::vector<unsigned char> is_extruder_used(std::max(size_t(MAXIMUM_EXTRUDER_NUMBER), print.config().filament_diameter.size()), 0);
    for (unsigned int extruder : tool_ordering.all_extruders())
        is_extruder_used[extruder] = true;
    this->placeholder_parser().set("is_extruder_used", new ConfigOptionBools(is_extruder_used));

    {
        BoundingBoxf bbox_bed(print.config().printable_area.values);
        Vec2f plate_offset = m_writer.get_xy_offset();
        this->placeholder_parser().set("print_bed_min", new ConfigOptionFloats({ bbox_bed.min.x(), bbox_bed.min.y()}));
        this->placeholder_parser().set("print_bed_max", new ConfigOptionFloats({ bbox_bed.max.x(), bbox_bed.max.y()}));
        this->placeholder_parser().set("print_bed_size", new ConfigOptionFloats({ bbox_bed.size().x(), bbox_bed.size().y() }));

        BoundingBoxf bbox;
        auto pts = std::make_unique<ConfigOptionPoints>();
        if (print.calib_mode() == CalibMode::Calib_PA_Line || print.calib_mode() == CalibMode::Calib_PA_Pattern) {
            bbox = bbox_bed;
            bbox.offset(-25.0);
            // add 4 corner points of bbox into pts
            pts->values.reserve(4);
            pts->values.emplace_back(bbox.min.x(), bbox.min.y());
            pts->values.emplace_back(bbox.max.x(), bbox.min.y());
            pts->values.emplace_back(bbox.max.x(), bbox.max.y());
            pts->values.emplace_back(bbox.min.x(), bbox.max.y());

        } else {
            // Convex hull of the 1st layer extrusions, for bed leveling and placing the initial purge line.
            // It encompasses the object extrusions, support extrusions, skirt, brim, wipe tower.
            // It does NOT encompass user extrusions generated by custom G-code,
            // therefore it does NOT encompass the initial purge line.
            // It does NOT encompass MMU/MMU2 starting (wipe) areas.
            pts->values.reserve(print.first_layer_convex_hull().size());
            for (const Point &pt : print.first_layer_convex_hull().points)
                pts->values.emplace_back(print.translate_to_print_space(pt));
            bbox = BoundingBoxf((pts->values));
        }
        this->placeholder_parser().set("first_layer_print_convex_hull", pts.release());
        this->placeholder_parser().set("first_layer_print_min", new ConfigOptionFloats({bbox.min.x(), bbox.min.y()}));
        this->placeholder_parser().set("first_layer_print_max", new ConfigOptionFloats({bbox.max.x(), bbox.max.y()}));
        this->placeholder_parser().set("first_layer_print_size", new ConfigOptionFloats({ bbox.size().x(), bbox.size().y() }));

        {  
            // use first layer convex_hull union with each object's bbox to check whether in head detect zone
            Polygons object_projections;
            for (auto& obj : print.objects()) {
                for (auto& instance : obj->instances()) {
                    const auto& bbox = instance.get_bounding_box();
                    Point min_p{ coord_t(scale_(bbox.min.x())),coord_t(scale_(bbox.min.y())) };
                    Point max_p{ coord_t(scale_(bbox.max.x())),coord_t(scale_(bbox.max.y())) };
                    Polygon instance_projection = {
                        {min_p.x(),min_p.y()},
                        {max_p.x(),min_p.y()},
                        {max_p.x(),max_p.y()},
                        {min_p.x(),max_p.y()}
                    };
                    object_projections.emplace_back(std::move(instance_projection));
                }
            }
            object_projections.emplace_back(print.first_layer_convex_hull());

            Polygons project_polys = union_(object_projections);
            Polygon  head_wrap_detect_zone;
            for (auto& point : print.config().head_wrap_detect_zone.values)
                head_wrap_detect_zone.append(scale_(point).cast<coord_t>() + scale_(plate_offset).cast<coord_t>());

            this->placeholder_parser().set("in_head_wrap_detect_zone", !intersection_pl(project_polys, {head_wrap_detect_zone}).empty());
        }

        {
            coordf_t max_print_z = 0;
            for (auto& obj : print.objects()) {
                max_print_z = std::max(max_print_z, (*std::max_element(obj->layers().begin(), obj->layers().end(), [](Layer* a, Layer* b) { return a->print_z < b->print_z; }))->print_z);
            }
            this->placeholder_parser().set("max_print_z", new ConfigOptionInt(std::ceil(max_print_z)));
        }

        BoundingBoxf mesh_bbox(m_config.bed_mesh_min, m_config.bed_mesh_max);
        auto         mesh_margin = m_config.adaptive_bed_mesh_margin.value;
        mesh_bbox.min            = mesh_bbox.min.cwiseMax((bbox.min.array() - mesh_margin).matrix());
        mesh_bbox.max            = mesh_bbox.max.cwiseMin((bbox.max.array() + mesh_margin).matrix());
        this->placeholder_parser().set("adaptive_bed_mesh_min", new ConfigOptionFloats({mesh_bbox.min.x(), mesh_bbox.min.y()}));
        this->placeholder_parser().set("adaptive_bed_mesh_max", new ConfigOptionFloats({mesh_bbox.max.x(), mesh_bbox.max.y()}));

        auto probe_dist_x  = std::max(1., m_config.bed_mesh_probe_distance.value.x());
        auto probe_dist_y  = std::max(1., m_config.bed_mesh_probe_distance.value.y());
        int  probe_count_x = std::max(3, (int) std::ceil(mesh_bbox.size().x() / probe_dist_x) + 1);
        int  probe_count_y = std::max(3, (int) std::ceil(mesh_bbox.size().y() / probe_dist_y) + 1);
        auto bed_mesh_algo = "bicubic";
        if (probe_count_x * probe_count_y <= 6) { // lagrange needs up to a total of 6 mesh points
            bed_mesh_algo = "lagrange";
        }
        else
            if(print.config().gcode_flavor == gcfKlipper){
              // bicubic needs 4 probe points per axis
              probe_count_x = std::max(probe_count_x,4);
              probe_count_y = std::max(probe_count_y,4);
            }
        this->placeholder_parser().set("bed_mesh_probe_count", new ConfigOptionInts({probe_count_x, probe_count_y}));
        this->placeholder_parser().set("bed_mesh_algo", bed_mesh_algo);
        // get center without wipe tower
        BoundingBoxf bbox_wo_wt; // bounding box without wipe tower
        for (auto &objPtr : print.objects()) {
            BBoxData data;
            bbox_wo_wt.merge(unscaled(objPtr->get_first_layer_bbox(data.area, data.layer_height, data.name)));
        }
        auto center = bbox_wo_wt.center();
        this->placeholder_parser().set("first_layer_center_no_wipe_tower", new ConfigOptionFloats{ {center.x(),center.y()}});
    }
    bool activate_chamber_temp_control = false;
    auto max_chamber_temp              = 0;
    for (const auto &extruder : m_writer.extruders()) {
        activate_chamber_temp_control |= m_config.activate_chamber_temp_control.get_at(extruder.id());
        max_chamber_temp = std::max(max_chamber_temp, m_config.chamber_temperature.get_at(extruder.id()));
    }
    {
        BedType curr_bed_type = m_config.curr_bed_type;

        int min_temperature_vitrification = std::numeric_limits<int>::max();
        for (const auto& extruder : m_writer.extruders())
            min_temperature_vitrification = std::min(min_temperature_vitrification, m_config.temperature_vitrification.get_at(extruder.id()));


        std::string first_layer_bed_temp_str;
        const ConfigOptionInts* first_bed_temp_opt = m_config.option<ConfigOptionInts>(get_bed_temp_1st_layer_key((BedType)curr_bed_type));
        const ConfigOptionInts* bed_temp_opt = m_config.option<ConfigOptionInts>(get_bed_temp_key((BedType)curr_bed_type));
        int target_bed_temp = 0;
        if (m_config.bed_temperature_formula == BedTempFormula::btfHighestTemp)
            target_bed_temp = get_highest_bed_temperature(true, print);
        else
            target_bed_temp = get_bed_temperature(initial_extruder_id, true, curr_bed_type);

        this->placeholder_parser().set("bbl_bed_temperature_gcode", new ConfigOptionBool(false));
        this->placeholder_parser().set("bed_temperature_initial_layer", new ConfigOptionInts(*first_bed_temp_opt));
        this->placeholder_parser().set("bed_temperature", new ConfigOptionInts(*bed_temp_opt));
        this->placeholder_parser().set("bed_temperature_initial_layer_single", new ConfigOptionInt(target_bed_temp));
        this->placeholder_parser().set("bed_temperature_initial_layer_vector", new ConfigOptionString());
        this->placeholder_parser().set("chamber_temperature", new ConfigOptionInts(m_config.chamber_temperature));
        this->placeholder_parser().set("overall_chamber_temperature", new ConfigOptionInt(max_chamber_temp));
        this->placeholder_parser().set("enable_high_low_temp_mix", new ConfigOptionBool(!print.need_check_multi_filaments_compatibility()));
        this->placeholder_parser().set("min_vitrification_temperature", new ConfigOptionInt(min_temperature_vitrification));

        // SoftFever: support variables `first_layer_temperature` and `first_layer_bed_temperature`
        this->placeholder_parser().set("first_layer_bed_temperature", new ConfigOptionInts(*first_bed_temp_opt));
        this->placeholder_parser().set("first_layer_temperature", new ConfigOptionInts(m_config.nozzle_temperature_initial_layer));
        this->placeholder_parser().set("max_print_height",new ConfigOptionInt(m_config.printable_height));
        this->placeholder_parser().set("z_offset", new ConfigOptionFloat(m_config.z_offset));
        this->placeholder_parser().set("model_name", new ConfigOptionString(print.get_model_name()));
        this->placeholder_parser().set("plate_number", new ConfigOptionString(print.get_plate_number_formatted()));
        this->placeholder_parser().set("plate_name", new ConfigOptionString(print.get_plate_name()));
        this->placeholder_parser().set("first_layer_height", new ConfigOptionFloat(m_config.initial_layer_print_height.value));

        auto used_filaments = print.get_slice_used_filaments(false);
        this->placeholder_parser().set("is_all_bbl_filament", std::all_of(used_filaments.begin(), used_filaments.end(), [&](auto idx) {
            return m_config.filament_vendor.values[idx] == "Bambu Lab";
            }));

        //add during_print_exhaust_fan_speed
        std::vector<int> during_print_exhaust_fan_speed_num;
        during_print_exhaust_fan_speed_num.reserve(m_config.during_print_exhaust_fan_speed.size());
        for (const auto& item : m_config.during_print_exhaust_fan_speed.values)
            during_print_exhaust_fan_speed_num.emplace_back((int)(item / 100.0 * 255));
        this->placeholder_parser().set("during_print_exhaust_fan_speed_num", new ConfigOptionInts(during_print_exhaust_fan_speed_num));

        //BBS: calculate the volumetric speed of outer wall. Ignore pre-object setting and multi-filament, and just use the default setting
        float outer_wall_volumetric_speed = get_outer_wall_volumetric_speed(m_config, print, initial_non_support_extruder_id, get_extruder_id(initial_non_support_extruder_id));
        this->placeholder_parser().set("outer_wall_volumetric_speed", new ConfigOptionFloat(outer_wall_volumetric_speed));

        auto first_layer_filaments = print.get_slice_used_filaments(true);
        bool has_tpu_in_first_layer = std::any_of(first_layer_filaments.begin(), first_layer_filaments.end(), [&](unsigned int idx) { return m_config.filament_type.values[idx] == "TPU"; });
        this->placeholder_parser().set("has_tpu_in_first_layer", new ConfigOptionBool(has_tpu_in_first_layer));

        if (print.calib_params().mode == CalibMode::Calib_PA_Line) {
            this->placeholder_parser().set("scan_first_layer", new ConfigOptionBool(false));
        }
    }
    {                                                                         // hold chamber temp for flat print: Flag
        double print_area_sum_threshold = 40000.0, pring_hight_threshold = 0.3; // thresholds in mm^2 and mm as units

        double   area_sum_temp  = 0.0;
        coordf_t max_hight_temp = -1.0;
        for (ObjectID print_object_ID_t : print.print_object_ids()) {
            const PrintObject *print_object = print.get_object(print_object_ID_t);
            // object hight
            if (!print_object->layers().empty() && print_object->layers().back()->print_z > max_hight_temp) max_hight_temp = print_object->layers().back()->print_z;
            // object area
            if (!print_object->layers().empty() && print_object->layers().front()->print_z < print.config().initial_layer_print_height + EPSILON &&
                !print_object->layers().front()->lslices.empty()) {
                ExPolygons temp_Expolys = print_object->layers().front()->lslices;
                for (ExPolygon &temp_Expoly : temp_Expolys) { area_sum_temp += temp_Expoly.area(); }
            }
            // suport area
            if (!print_object->support_layers().empty() && print_object->support_layers().front()->print_z < print.config().initial_layer_print_height + EPSILON &&
                !print_object->support_layers().front()->support_islands.empty()) {
                ExPolygons temp_Expolys = print_object->support_layers().front()->support_islands;
                for (ExPolygon &temp_Expoly : temp_Expolys) { area_sum_temp += temp_Expoly.area(); }
            }
            // brim area
            if (print.m_brimMap.find(print_object_ID_t) != print.m_brimMap.end() && !print.m_brimMap.at(print_object_ID_t).entities.empty()) { // contain brim
                for (const ExtrusionEntity *entities_temp : print.m_brimMap.at(print_object_ID_t).entities) {
                    Polygons temp_Expolys;
                    entities_temp->polygons_covered_by_spacing(temp_Expolys, 0.0f);
                    for (Polygon &temp_Expoly : temp_Expolys) { area_sum_temp += temp_Expoly.area(); }
                }
            }
        }
        // wipe tower area
        if (has_wipe_tower) {
            Polygon temp_Expoly = print.wipe_tower_data().wipe_tower_mesh_data->bottom;
            area_sum_temp += temp_Expoly.area();
        }
        bool hold_chamber_temp_for_flat_print = max_hight_temp > 0 && max_hight_temp < pring_hight_threshold && area_sum_temp > print_area_sum_threshold * 1.0e10;
        this->placeholder_parser().set("hold_chamber_temp_for_flat_print", new ConfigOptionBool(hold_chamber_temp_for_flat_print));
    }


    std::string machine_start_gcode = this->placeholder_parser_process("machine_start_gcode", print.config().machine_start_gcode.value, initial_extruder_id);
    if (print.config().gcode_flavor != gcfKlipper) {
        // Set bed temperature if the start G-code does not contain any bed temp control G-codes.
        this->_print_first_layer_bed_temperature(file, print, machine_start_gcode, initial_extruder_id, true);
        // Set extruder(s) temperature before and after start G-code.
        this->_print_first_layer_extruder_temperatures(file, print, machine_start_gcode, initial_extruder_id, false);
    }

    // adds tag for processor
    file.write_format(";%s%s\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role).c_str(), ExtrusionEntity::role_to_string(erCustom).c_str());

    // Orca: set chamber temperature at the beginning of gcode file
    if (activate_chamber_temp_control && max_chamber_temp > 0){
        int temp_out =0;
        if(!custom_gcode_sets_temperature(machine_start_gcode,141,191,false,temp_out))
            file.write(m_writer.set_chamber_temperature(max_chamber_temp, true)); // set chamber_temperature
    }

    // Write the custom start G-code
    file.writeln(machine_start_gcode);

    //BBS: gcode writer doesn't know where the real position of extruder is after inserting custom gcode
    m_writer.set_current_position_clear(false);
    m_start_gcode_filament = GCodeProcessor::get_gcode_last_filament(machine_start_gcode);

    if (is_bbl_printers) {
        m_writer.init_extruder(initial_non_support_extruder_id);
        // add the missing filament start gcode in machine start gcode
        {
            DynamicConfig config;
            config.set_key_value("filament_extruder_id", new ConfigOptionInt((int)(initial_non_support_extruder_id)));
            config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
            std::string filament_start_gcode = this->placeholder_parser_process("filament_start_gcode", print.config().filament_start_gcode.values.at(initial_non_support_extruder_id), initial_non_support_extruder_id,&config);
            file.writeln(filament_start_gcode);
            // mark the first filament used in print
            file.write_format(";VT%d\n", initial_extruder_id);
        }
        // Orca: add missing PA settings for initial filament
        if (m_config.enable_pressure_advance.get_at(initial_non_support_extruder_id)) {
            file.write(m_writer.set_pressure_advance(m_config.pressure_advance.get_at(initial_non_support_extruder_id)));
            // Orca: Adaptive PA
            // Reset Adaptive PA processor last PA value
            m_pa_processor->resetPreviousPA(m_config.pressure_advance.get_at(initial_non_support_extruder_id));
        }
    }

    //flush FanMover buffer to avoid modifying the start gcode if it's manual.
    if (!machine_start_gcode.empty() && this->m_fan_mover.get() != nullptr)
        file.write(this->m_fan_mover.get()->process_gcode("", true));

    // Process filament-specific gcode.
   /* if (has_wipe_tower) {
        // Wipe tower will control the extruder switching, it will call the filament_start_gcode.
    } else {
            DynamicConfig config;
            config.set_key_value("filament_extruder_id", new ConfigOptionInt(int(initial_extruder_id)));
            file.writeln(this->placeholder_parser_process("filament_start_gcode", print.config().filament_start_gcode.values[initial_extruder_id], initial_extruder_id, &config));
    }
*/
    if (is_bbl_printers) {
        this->_print_first_layer_extruder_temperatures(file, print, machine_start_gcode, initial_extruder_id, true);
    }
    // Orca: when activate_air_filtration is set on any extruder, find and set the highest during_print_exhaust_fan_speed
    bool activate_air_filtration        = false;
    int  during_print_exhaust_fan_speed = 0;
    for (const auto &extruder : m_writer.extruders()) {
        activate_air_filtration |= m_config.activate_air_filtration.get_at(extruder.id());
        if (m_config.activate_air_filtration.get_at(extruder.id()))
            during_print_exhaust_fan_speed = std::max(during_print_exhaust_fan_speed,
                                                      m_config.during_print_exhaust_fan_speed.get_at(extruder.id()));
    }
    if (activate_air_filtration)
        file.write(m_writer.set_exhaust_fan(during_print_exhaust_fan_speed, true));

    print.throw_if_canceled();

    // Set other general things.
    file.write(this->preamble());

    // Calculate wiping points if needed
    DoExport::init_ooze_prevention(print, m_ooze_prevention);
    print.throw_if_canceled();

    // Collect custom seam data from all objects.
    std::function<void(void)> throw_if_canceled_func = [&print]() { print.throw_if_canceled(); };
    m_seam_placer.init(print, throw_if_canceled_func);

    // BBS: get path for change filament
    if (m_writer.multiple_extruders) {
        std::vector<Vec2d> points = get_path_of_change_filament(print);
        if (points.size() == 3) {
            travel_point_1 = points[0];
            travel_point_2 = points[1];
            travel_point_3 = points[2];
        }
    }

    // Orca: support extruder priming
    if (is_bbl_printers || ! (has_wipe_tower && print.config().single_extruder_multi_material_priming))
    {
        // Set initial extruder only after custom start G-code.
        // Ugly hack: Do not set the initial extruder if the extruder is primed using the MMU priming towers at the edge of the print bed.
        file.write(this->set_extruder(initial_extruder_id, 0.));
    }
    // BBS: set that indicates objs with brim
    for (auto iter = print.m_brimMap.begin(); iter != print.m_brimMap.end(); ++iter) {
        if (!iter->second.empty())
            this->m_objsWithBrim.insert(iter->first);
    }
    for (auto iter = print.m_supportBrimMap.begin(); iter != print.m_supportBrimMap.end(); ++iter) {
        if (!iter->second.empty())
            this->m_objSupportsWithBrim.insert(iter->first);
    }
    if (this->m_objsWithBrim.empty() && this->m_objSupportsWithBrim.empty()) m_brim_done = true;

    // SoftFever: calib
    if (print.calib_params().mode == CalibMode::Calib_PA_Line) {
        std::string gcode;
        gcode += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Layer_Change) + "\n";
        if ((print.default_object_config().outer_wall_acceleration.value > 0 && print.default_object_config().outer_wall_acceleration.value > 0)) {
            gcode += m_writer.set_print_acceleration((unsigned int)floor(print.default_object_config().outer_wall_acceleration.value + 0.5));
        }

        if (print.default_object_config().outer_wall_jerk.value > 0) {
            double jerk = print.default_object_config().outer_wall_jerk.value;
            gcode += m_writer.set_jerk_xy(jerk);
        }

        auto params = print.calib_params();

        CalibPressureAdvanceLine pa_test(this);

        auto fast_speed = CalibPressureAdvance::find_optimal_PA_speed(print.full_print_config(), pa_test.line_width(), pa_test.height_layer());
        auto slow_speed = std::max(10.0, fast_speed / 10.0);
        if (fast_speed < slow_speed + 5)
            fast_speed = slow_speed + 5;

        pa_test.set_speed(fast_speed, slow_speed);
        pa_test.draw_numbers() = print.calib_params().print_numbers;
        gcode += pa_test.generate_test(params.start, params.step, std::llround(std::ceil((params.end - params.start) / params.step)) + 1);

        file.write(gcode);
    } else {
        //BBS: open spaghetti detector
        if (is_bbl_printers) {
            // if (print.config().spaghetti_detector.value)
            file.write("M981 S1 P20000 ;open spaghetti detector\n");
        }

        // Do all objects for each layer.
        if (print.config().print_sequence == PrintSequence::ByObject && !has_wipe_tower) {
            size_t finished_objects = 0;
            print_object_instance_sequential_active = first_has_extrude_print_object;
            const PrintObject *prev_object = (*print_object_instance_sequential_active)->print_object;
            for (; print_object_instance_sequential_active != print_object_instances_ordering.end(); ++ print_object_instance_sequential_active) {
                const PrintObject &object = *(*print_object_instance_sequential_active)->print_object;
                if (&object != prev_object || tool_ordering.first_extruder() != final_extruder_id) {
                    tool_ordering = ToolOrdering(object, final_extruder_id);
                    tool_ordering.sort_and_build_data(object, final_extruder_id);
                    unsigned int new_extruder_id = tool_ordering.first_extruder();
                    if (new_extruder_id == (unsigned int)-1)
                        // Skip this object.
                        continue;
                    initial_extruder_id = new_extruder_id;
                    final_extruder_id   = tool_ordering.last_extruder();
                    assert(final_extruder_id != (unsigned int)-1);
                }
                print.throw_if_canceled();
                this->set_origin(unscale((*print_object_instance_sequential_active)->shift));

                // BBS: prime extruder if extruder change happens before this object instance
                bool prime_extruder = false;
                if (finished_objects > 0) {
                    // Move to the origin position for the copy we're going to print.
                    // This happens before Z goes down to layer 0 again, so that no collision happens hopefully.
                    m_enable_cooling_markers = false; // we're not filtering these moves through CoolingBuffer
                    m_avoid_crossing_perimeters.use_external_mp_once();
                    // BBS. change tool before moving to origin point.
                    if (m_writer.need_toolchange(initial_extruder_id)) {
                        const PrintObjectConfig& object_config = object.config();
                        coordf_t initial_layer_print_height = print.config().initial_layer_print_height.value;

                        if (m_enable_exclude_object && print.config().support_object_skip_flush.value) {
                            m_filament_instances_code = _encode_label_ids_to_base64({(*print_object_instance_sequential_active)->model_instance->get_labeled_id()});
                        }

                        file.write(this->set_extruder(initial_extruder_id, initial_layer_print_height, true));
                        prime_extruder = true;
                    } else {
                        file.write(this->retract());
                    }
                    file.write(m_writer.travel_to_z(m_max_layer_z + m_writer.config.z_hop.get_at(initial_extruder_id)));
                    file.write(this->travel_to(Point(0, 0), erNone, "move to origin position for next object"));
                    m_enable_cooling_markers = true;
                    // Disable motion planner when traveling to first object point.
                    m_avoid_crossing_perimeters.disable_once();
                    // Ff we are printing the bottom layer of an object, and we have already finished
                    // another one, set first layer temperatures. This happens before the Z move
                    // is triggered, so machine has more time to reach such temperatures.
                    this->placeholder_parser().set("current_object_idx", int(finished_objects));
                    std::string printing_by_object_gcode = this->placeholder_parser_process("printing_by_object_gcode", print.config().printing_by_object_gcode.value, initial_extruder_id);
                    // Set first layer bed and extruder temperatures, don't wait for it to reach the temperature.
                    this->_print_first_layer_bed_temperature(file, print, printing_by_object_gcode, initial_extruder_id, false);
                    this->_print_first_layer_extruder_temperatures(file, print, printing_by_object_gcode, initial_extruder_id, false);
                    file.writeln(printing_by_object_gcode);
                }
                // Reset the cooling buffer internal state (the current position, feed rate, accelerations).
                m_cooling_buffer->reset(this->writer().get_position());
                m_cooling_buffer->set_current_extruder(initial_extruder_id);
                // Process all layers of a single object instance (sequential mode) with a parallel pipeline:
                // Generate G-code, run the filters (vase mode, cooling buffer), run the G-code analyser
                // and export G-code into file.
                tool_ordering.cal_most_used_extruder(print.config());
                m_printed_objects.emplace_back(&object);
                this->process_layers(print, tool_ordering, collect_layers_to_print(object), *print_object_instance_sequential_active - object.instances().data(), file,
                                     prime_extruder);
                {
                    // save the flush statitics stored in tool ordering by object
                    print.m_statistics_by_extruder_count.stats_by_single_extruder += tool_ordering.get_filament_change_stats(ToolOrdering::FilamentChangeMode::SingleExt);
                    print.m_statistics_by_extruder_count.stats_by_multi_extruder_best += tool_ordering.get_filament_change_stats(ToolOrdering::FilamentChangeMode::MultiExtBest);
                    print.m_statistics_by_extruder_count.stats_by_multi_extruder_curr += tool_ordering.get_filament_change_stats(ToolOrdering::FilamentChangeMode::MultiExtCurr);
                    // save sorted filament sequences
                    const auto& layer_tools = tool_ordering.layer_tools();
                    for (const auto& lt : layer_tools)
                        m_sorted_layer_filaments.emplace_back(lt.extruders);
                }

                // Orca: disable power loss recovery if it was enabled earlier
                {
                    const auto plr_mode = print.config().enable_power_loss_recovery.value;
                    if (m_second_layer_things_done && plr_mode == PowerLossRecoveryMode::Enable) {
                        file.write(m_writer.enable_power_loss_recovery(PowerLossRecoveryMode::Disable));
                    }
                }
                ++ finished_objects;
                // Flag indicating whether the nozzle temperature changes from 1st to 2nd layer were performed.
                // Reset it when starting another object from 1st layer.
                m_second_layer_things_done = false;
                prev_object = &object;
            }
        } else {
            // Sort layers by Z.
            // All extrusion moves with the same top layer height are extruded uninterrupted.
            std::vector<std::pair<coordf_t, std::vector<LayerToPrint>>> layers_to_print = collect_layers_to_print(print);
            // Prusa Multi-Material wipe tower.
            if (has_wipe_tower && ! layers_to_print.empty()) {
                m_wipe_tower.reset(new WipeTowerIntegration(print.config(), print.get_plate_index(), print.get_plate_origin(), *print.wipe_tower_data().priming.get(),
                                                            print.wipe_tower_data().tool_changes, *print.wipe_tower_data().final_purge.get(), print.get_slice_used_filaments(false)));
                m_wipe_tower->set_wipe_tower_depth(print.get_wipe_tower_depth());
                m_wipe_tower->set_wipe_tower_bbx(print.get_wipe_tower_bbx());
                m_wipe_tower->set_rib_offset(print.get_rib_offset());
                //BBS
                file.write(m_writer.travel_to_z(initial_layer_print_height + m_config.z_offset.value, "Move to the first layer height"));

                if (!is_bbl_printers && print.config().single_extruder_multi_material_priming) {
                    file.write(m_wipe_tower->prime(*this));
                    // Verify, whether the print overaps the priming extrusions.
                    BoundingBoxf bbox_print(get_print_extrusions_extents(print));
                    coordf_t twolayers_printz = ((layers_to_print.size() == 1) ? layers_to_print.front() : layers_to_print[1]).first + EPSILON;
                    for (const PrintObject *print_object : print.objects())
                        bbox_print.merge(get_print_object_extrusions_extents(*print_object, twolayers_printz));
                    bbox_print.merge(get_wipe_tower_extrusions_extents(print, twolayers_printz));
                    BoundingBoxf bbox_prime(get_wipe_tower_priming_extrusions_extents(print));
                    bbox_prime.offset(0.5f);
                    bool overlap = bbox_prime.overlap(bbox_print);

                    if (print.config().gcode_flavor == gcfMarlinLegacy || print.config().gcode_flavor == gcfMarlinFirmware) {
                        file.write(this->retract());
                        file.write("M300 S800 P500\n"); // Beep for 500ms, tone 800Hz.
                        if (overlap) {
                            // Wait for the user to remove the priming extrusions.
                            file.write("M1 Remove priming towers and click button.\n");
                        } else {
                            // Just wait for a bit to let the user check, that the priming succeeded.
                            //TODO Add a message explaining what the printer is waiting for. This needs a firmware fix.
                            file.write("M1 S10\n");
                        }
                    }
                    else {
                        // This is not Marlin, M1 command is probably not supported.
                        if (overlap) {
                           print.active_step_add_warning(PrintStateBase::WarningLevel::CRITICAL,
                               _(L("Your print is very close to the priming regions. "
                                 "Make sure there is no collision.")));
                        } else {
                           // Just continue printing, no action necessary.
                        }
                    }
                }
                print.throw_if_canceled();
            }

            tool_ordering.cal_most_used_extruder(print.config());

            // Process all layers of all objects (non-sequential mode) with a parallel pipeline:
            // Generate G-code, run the filters (vase mode, cooling buffer), run the G-code analyser
            // and export G-code into file.
            this->process_layers(print, tool_ordering, print_object_instances_ordering, layers_to_print, file);
            {
                //save the flush statitics stored in tool ordering
                print.m_statistics_by_extruder_count.stats_by_single_extruder = tool_ordering.get_filament_change_stats(ToolOrdering::FilamentChangeMode::SingleExt);
                print.m_statistics_by_extruder_count.stats_by_multi_extruder_best = tool_ordering.get_filament_change_stats(ToolOrdering::FilamentChangeMode::MultiExtBest);
                print.m_statistics_by_extruder_count.stats_by_multi_extruder_curr = tool_ordering.get_filament_change_stats(ToolOrdering::FilamentChangeMode::MultiExtCurr);
                // save sorted filament sequences
                const auto& layer_tools = tool_ordering.layer_tools();
                for (const auto& lt : layer_tools)
                    m_sorted_layer_filaments.emplace_back(lt.extruders);
            }

            // Orca: disable power loss recovery
            if (m_second_layer_things_done && print.config().enable_power_loss_recovery.value == PowerLossRecoveryMode::Enable) {
                file.write(m_writer.enable_power_loss_recovery(PowerLossRecoveryMode::Disable));
            }
            if (m_wipe_tower)
                // Purge the extruder, pull out the active filament.
                file.write(m_wipe_tower->finalize(*this));
        }
    }
    //BBS: the last retraction
    // Write end commands to file.
    file.write(this->retract(false, true));

    // if needed, write the gcode_label_objects_end
    {
        std::string gcode;
        m_writer.add_object_change_labels(gcode);
        file.write(gcode);
    }

    file.write(m_writer.set_fan(0));
    //BBS: make sure the additional fan is closed when end
    if(m_config.auxiliary_fan.value)
        file.write(m_writer.set_additional_fan(0));
    if (is_bbl_printers) {
        //BBS: close spaghetti detector
        //Note: M981 is also used to tell xcam the last layer is finished, so we need always send it even if spaghetti option is disabled.
        //if (print.config().spaghetti_detector.value)
        file.write("M981 S0 P20000 ; close spaghetti detector\n");
    }

    // adds tag for processor
    file.write_format(";%s%s\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role).c_str(), ExtrusionEntity::role_to_string(erCustom).c_str());

    // Process filament-specific gcode in extruder order.
    {
        DynamicConfig config;
        config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
        //BBS
        config.set_key_value("layer_z",   new ConfigOptionFloat(m_writer.get_position()(2) - m_config.z_offset.value));
        config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));

        if (print.config().single_extruder_multi_material) {
            // Process the filament_end_gcode for the active filament only.
            int extruder_id = m_writer.filament()->id();
            config.set_key_value("filament_extruder_id", new ConfigOptionInt(extruder_id));
            file.writeln(this->placeholder_parser_process("filament_end_gcode", print.config().filament_end_gcode.get_at(extruder_id), extruder_id, &config));
        } else {
            for (const std::string &end_gcode : print.config().filament_end_gcode.values) {
                int extruder_id = (unsigned int)(&end_gcode - &print.config().filament_end_gcode.values.front());
                config.set_key_value("filament_extruder_id", new ConfigOptionInt(extruder_id));
                file.writeln(this->placeholder_parser_process("filament_end_gcode", end_gcode, extruder_id, &config));
            }
        }
        file.writeln(this->placeholder_parser_process("machine_end_gcode", print.config().machine_end_gcode, m_writer.filament()->id(), &config));
    }
    file.write(m_writer.update_progress(m_layer_count, m_layer_count, true)); // 100%
    file.write(m_writer.postamble());

    if (activate_chamber_temp_control && max_chamber_temp > 0)
        file.write(m_writer.set_chamber_temperature(0, false));  //close chamber_temperature

    if (activate_air_filtration) {
        int complete_print_exhaust_fan_speed = 0;
        for (const auto& extruder : m_writer.extruders())
            if (m_config.activate_air_filtration.get_at(extruder.id()))
                complete_print_exhaust_fan_speed = std::max(complete_print_exhaust_fan_speed, m_config.complete_print_exhaust_fan_speed.get_at(extruder.id()));
        file.write(m_writer.set_exhaust_fan(complete_print_exhaust_fan_speed, true));
    }
    // adds tags for time estimators
    file.write_format(";%s\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Last_Line_M73_Placeholder).c_str());
    file.write_format("; EXECUTABLE_BLOCK_END\n\n");

    print.throw_if_canceled();

    // Get filament stats.
    file.write(DoExport::update_print_stats_and_format_filament_stats(
    	// Const inputs
        has_wipe_tower, print.wipe_tower_data(),
        m_writer.extruders(),
        // Modifies
        print.m_print_statistics));
    print.m_print_statistics.initial_tool = initial_extruder_id;
    if (!is_bbl_printers) {
        file.write_format("; total filament used [g] = %.2lf\n",
            print.m_print_statistics.total_weight);
        file.write_format("; total filament cost = %.2lf\n",
            print.m_print_statistics.total_cost);
        if (print.m_print_statistics.total_toolchanges > 0)
            file.write_format("; total filament change = %i\n",
                print.m_print_statistics.total_toolchanges);
        file.write_format("; total layers count = %i\n", m_layer_count);
        file.write_format(
            ";%s\n",
            GCodeProcessor::reserved_tag(
                GCodeProcessor::ETags::Estimated_Printing_Time_Placeholder)
            .c_str());
      file.write("\n");
      file.write("; CONFIG_BLOCK_START\n");
      std::string full_config;
      append_full_config(print, full_config);
      if (!full_config.empty())
        file.write(full_config);

      // SoftFever: write compatiple info
      int first_layer_bed_temperature = get_bed_temperature(0, true, print.config().curr_bed_type);
      file.write_format("; first_layer_bed_temperature = %d\n", first_layer_bed_temperature);
      file.write_format("; bed_shape = %s\n", print.full_print_config().opt_serialize("printable_area").c_str());
      file.write_format("; first_layer_temperature = %d\n", print.config().nozzle_temperature_initial_layer.get_at(0));
      file.write_format("; first_layer_height = %.3f\n", print.config().initial_layer_print_height.value);
        
        //SF TODO
//      file.write_format("; variable_layer_height = %d\n", print.ad.adaptive_layer_height ? 1 : 0);
   
      file.write("; CONFIG_BLOCK_END\n\n");

    }
    file.write("\n");

    print.throw_if_canceled();
}

// export info requested for filament change
void GCode::export_layer_filaments(GCodeProcessorResult* result)
{
    if (result == nullptr)
        return;

    const std::vector<int>filament_map = m_config.filament_map.values; // 1 based
    std::vector<int>prev_filament(m_config.nozzle_diameter.size(), -1);
    for (size_t idx = 0; idx < m_sorted_layer_filaments.size(); ++idx) {
        for (auto f : m_sorted_layer_filaments[idx]) {
            int extruder_idx = filament_map[f] - 1;
            if (prev_filament[extruder_idx] != -1 && f != prev_filament[extruder_idx]) {
                std::pair<int, int> from_to_pair = { prev_filament[extruder_idx],f };
                auto iter = result->filament_change_count_map.find(from_to_pair);
                if (iter == result->filament_change_count_map.end())
                    result->filament_change_count_map.emplace(from_to_pair, 1);
                else
                    iter->second += 1;
            }
            prev_filament[extruder_idx] = f;
        }

        // now we do not need sorted data, so we sort the filaments in id order
        auto layer_filaments = m_sorted_layer_filaments[idx];
        std::sort(layer_filaments.begin(), layer_filaments.end());
        auto iter = result->layer_filaments.find(layer_filaments);
        if (iter == result->layer_filaments.end()) {
            result->layer_filaments[layer_filaments].emplace_back(idx, idx);
        }
        else {
            // if layer id is sequential, expand the range
            if (iter->second.back().second == idx - 1)
                iter->second.back().second = idx;
            else
                iter->second.emplace_back(idx, idx);
        }
    }
}

//BBS
void GCode::check_placeholder_parser_failed()
{
    if (! m_placeholder_parser_integration.failed_templates.empty()) {
        // G-code export proceeded, but some of the PlaceholderParser substitutions failed.
        std::string msg = Slic3r::format(_(L("Failed to generate G-code for invalid custom G-code.\n\n")));
        for (const auto &name_and_error : m_placeholder_parser_integration.failed_templates)
            msg += name_and_error.first + " " + name_and_error.second + "\n";
        msg += Slic3r::format(_(L("Please check the custom G-code or use the default custom G-code.")));
        throw Slic3r::PlaceholderParserError(msg);
    }
}

size_t GCode::get_extruder_id(unsigned int filament_id) const
{
    if (m_print) {
        return m_print->get_extruder_id(filament_id);
    }
    return 0;
}

// Process all layers of all objects (non-sequential mode) with a parallel pipeline:
// Generate G-code, run the filters (vase mode, cooling buffer), run the G-code analyser
// and export G-code into file.
void GCode::process_layers(
    const Print                                                         &print,
    const ToolOrdering                                                  &tool_ordering,
    const std::vector<const PrintInstance*>                             &print_object_instances_ordering,
    const std::vector<std::pair<coordf_t, std::vector<LayerToPrint>>>   &layers_to_print,
    GCodeOutputStream                                                   &output_stream)
{
    // The pipeline is variable: The vase mode filter is optional.
    size_t layer_to_print_idx = 0;
    const auto generator = tbb::make_filter<void, LayerResult>(slic3r_tbb_filtermode::serial_in_order,
        [this, &print, &tool_ordering, &print_object_instances_ordering, &layers_to_print, &layer_to_print_idx](tbb::flow_control& fc) -> LayerResult {
            if (layer_to_print_idx >= layers_to_print.size()) {
                if (layer_to_print_idx == layers_to_print.size() + (m_pressure_equalizer ? 1 : 0)) {
                    fc.stop();
                    return {};
                } else {
                    // Pressure equalizer need insert empty input. Because it returns one layer back.
                    // Insert NOP (no operation) layer;
                    ++layer_to_print_idx;
                    return LayerResult::make_nop_layer_result();
                }
            } else {
                const std::pair<coordf_t, std::vector<LayerToPrint>>& layer = layers_to_print[layer_to_print_idx++];
                const LayerTools& layer_tools = tool_ordering.tools_for_layer(layer.first);
                print.set_status(80, Slic3r::format(_(L("Generating G-code: layer %1%")), std::to_string(layer_to_print_idx)));
                if (m_wipe_tower && layer_tools.has_wipe_tower)
                    m_wipe_tower->next_layer();
                //BBS
                check_placeholder_parser_failed();
                print.throw_if_canceled();
                return this->process_layer(print, layer.second, layer_tools, &layer == &layers_to_print.back(), &print_object_instances_ordering, tool_ordering.get_most_used_extruder(), size_t(-1));
            }
        });
    if (m_spiral_vase) {
        float nozzle_diameter  = EXTRUDER_CONFIG(nozzle_diameter);
        float max_xy_smoothing = m_config.get_abs_value("spiral_mode_max_xy_smoothing", nozzle_diameter);
        this->m_spiral_vase->set_max_xy_smoothing(max_xy_smoothing);
    }
    const auto spiral_mode = tbb::make_filter<LayerResult, LayerResult>(slic3r_tbb_filtermode::serial_in_order,
        [&spiral_mode = *this->m_spiral_vase.get(), &layers_to_print](LayerResult in) -> LayerResult {
        	if (in.nop_layer_result)
                return in;
                
            spiral_mode.enable(in.spiral_vase_enable);
            bool last_layer = in.layer_id == layers_to_print.size() - 1;
            return { spiral_mode.process_layer(std::move(in.gcode), last_layer), in.layer_id, in.spiral_vase_enable, in.cooling_buffer_flush};
        });
    const auto pressure_equalizer = tbb::make_filter<LayerResult, LayerResult>(slic3r_tbb_filtermode::serial_in_order,
        [pressure_equalizer = this->m_pressure_equalizer.get()](LayerResult in) -> LayerResult {
            return pressure_equalizer->process_layer(std::move(in));
        });
    const auto cooling = tbb::make_filter<LayerResult, std::string>(slic3r_tbb_filtermode::serial_in_order,
        [&cooling_buffer = *this->m_cooling_buffer.get()](LayerResult in) -> std::string {
        	if (in.nop_layer_result)
                return in.gcode;
            return cooling_buffer.process_layer(std::move(in.gcode), in.layer_id, in.cooling_buffer_flush);
        });
    const auto pa_processor_filter = tbb::make_filter<std::string, std::string>(slic3r_tbb_filtermode::serial_in_order,
            [&pa_processor = *this->m_pa_processor](std::string in) -> std::string {
                return pa_processor.process_layer(std::move(in));
            }
        );
    
    const auto output = tbb::make_filter<std::string, void>(slic3r_tbb_filtermode::serial_in_order,
        [&output_stream](std::string s) { output_stream.write(s); }
    );

    const auto fan_mover = tbb::make_filter<std::string, std::string>(slic3r_tbb_filtermode::serial_in_order,
            [&fan_mover = this->m_fan_mover, &config = this->config(), &writer = this->m_writer](std::string in)->std::string {

        CNumericLocalesSetter locales_setter;

        if (config.fan_speedup_time.value != 0 || config.fan_kickstart.value > 0) {
            if (fan_mover.get() == nullptr)
                fan_mover.reset(new Slic3r::FanMover(
                    writer,
                    std::abs((float)config.fan_speedup_time.value),
                    config.fan_speedup_time.value > 0,
                    config.use_relative_e_distances.value,
                    config.fan_speedup_overhangs.value,
                    (float)config.fan_kickstart.value));
            //flush as it's a whole layer
            return fan_mover->process_gcode(in, true);
        }
        return in;
    });

    // The pipeline elements are joined using const references, thus no copying is performed.
    if (m_spiral_vase && m_pressure_equalizer)
        tbb::parallel_pipeline(12, generator & spiral_mode & pressure_equalizer & cooling & fan_mover & output);
    else if (m_spiral_vase)
    	tbb::parallel_pipeline(12, generator & spiral_mode & cooling & fan_mover & output);
    else if	(m_pressure_equalizer)
        tbb::parallel_pipeline(12, generator & pressure_equalizer & cooling & fan_mover & pa_processor_filter & output);
    else
    	tbb::parallel_pipeline(12, generator & cooling & fan_mover & pa_processor_filter & output);

}

// Process all layers of a single object instance (sequential mode) with a parallel pipeline:
// Generate G-code, run the filters (vase mode, cooling buffer), run the G-code analyser
// and export G-code into file.
void GCode::process_layers(
    const Print                             &print,
    const ToolOrdering                      &tool_ordering,
    std::vector<LayerToPrint>                layers_to_print,
    const size_t                             single_object_idx,
    GCodeOutputStream                       &output_stream,
    // BBS
    const bool                               prime_extruder)
{
    // The pipeline is variable: The vase mode filter is optional.
    size_t layer_to_print_idx = 0;
    const auto generator = tbb::make_filter<void, LayerResult>(slic3r_tbb_filtermode::serial_in_order,
        [this, &print, &tool_ordering, &layers_to_print, &layer_to_print_idx, single_object_idx, prime_extruder](tbb::flow_control& fc) -> LayerResult {
            if (layer_to_print_idx >= layers_to_print.size()) {
                if (layer_to_print_idx == layers_to_print.size() + (m_pressure_equalizer ? 1 : 0)) {
                    fc.stop();
                    return {};
                } else {
                    // Pressure equalizer need insert empty input. Because it returns one layer back.
                    // Insert NOP (no operation) layer;
                    ++layer_to_print_idx;
                    return LayerResult::make_nop_layer_result();
                }
            } else {
                LayerToPrint &layer = layers_to_print[layer_to_print_idx ++];
                print.set_status(80, Slic3r::format(_(L("Generating G-code: layer %1%")), std::to_string(layer_to_print_idx)));
                //BBS
                check_placeholder_parser_failed();
                print.throw_if_canceled();
                return this->process_layer(print, { std::move(layer) }, tool_ordering.tools_for_layer(layer.print_z()), &layer == &layers_to_print.back(), nullptr, tool_ordering.get_most_used_extruder(), single_object_idx, prime_extruder);
            }
        });
    if (m_spiral_vase) {
        float nozzle_diameter  = EXTRUDER_CONFIG(nozzle_diameter);
        float max_xy_smoothing = m_config.get_abs_value("spiral_mode_max_xy_smoothing", nozzle_diameter);
        this->m_spiral_vase->set_max_xy_smoothing(max_xy_smoothing);
    }
    const auto spiral_mode = tbb::make_filter<LayerResult, LayerResult>(slic3r_tbb_filtermode::serial_in_order,
        [&spiral_mode = *this->m_spiral_vase.get(), &layers_to_print](LayerResult in)->LayerResult {
            if (in.nop_layer_result)
                return in;
            spiral_mode.enable(in.spiral_vase_enable);
            bool last_layer = in.layer_id == layers_to_print.size() - 1;
            return { spiral_mode.process_layer(std::move(in.gcode), last_layer), in.layer_id, in.spiral_vase_enable, in.cooling_buffer_flush };
        });
    const auto pressure_equalizer = tbb::make_filter<LayerResult, LayerResult>(slic3r_tbb_filtermode::serial_in_order,
        [pressure_equalizer = this->m_pressure_equalizer.get()](LayerResult in) -> LayerResult {
             return pressure_equalizer->process_layer(std::move(in));
        });
    const auto cooling = tbb::make_filter<LayerResult, std::string>(slic3r_tbb_filtermode::serial_in_order,
        [&cooling_buffer = *this->m_cooling_buffer.get()](LayerResult in)->std::string {
            if (in.nop_layer_result)
                return in.gcode;
            return cooling_buffer.process_layer(std::move(in.gcode), in.layer_id, in.cooling_buffer_flush);
        });
    const auto pa_processor_filter = tbb::make_filter<std::string, std::string>(slic3r_tbb_filtermode::serial_in_order,
        [&pa_processor = *this->m_pa_processor](std::string in) -> std::string {
            return pa_processor.process_layer(std::move(in));
        }
    );
    
    const auto output = tbb::make_filter<std::string, void>(slic3r_tbb_filtermode::serial_in_order,
        [&output_stream](std::string s) { output_stream.write(s); }
    );

    const auto fan_mover = tbb::make_filter<std::string, std::string>(slic3r_tbb_filtermode::serial_in_order,
        [&fan_mover = this->m_fan_mover, &config = this->config(), &writer = this->m_writer](std::string in)->std::string {

        if (config.fan_speedup_time.value != 0 || config.fan_kickstart.value > 0) {
            if (fan_mover.get() == nullptr)
                fan_mover.reset(new Slic3r::FanMover(
                    writer,
                    std::abs((float)config.fan_speedup_time.value),
                    config.fan_speedup_time.value > 0,
                    config.use_relative_e_distances.value,
                    config.fan_speedup_overhangs.value,
                    (float)config.fan_kickstart.value));
            //flush as it's a whole layer
            return fan_mover->process_gcode(in, true);
        }
        return in;
    });

    // The pipeline elements are joined using const references, thus no copying is performed.
    if (m_spiral_vase && m_pressure_equalizer)
        tbb::parallel_pipeline(12, generator & spiral_mode & pressure_equalizer & cooling & fan_mover & output);
    else if (m_spiral_vase)
    	tbb::parallel_pipeline(12, generator & spiral_mode & cooling & fan_mover & output);
    else if	(m_pressure_equalizer)
        tbb::parallel_pipeline(12, generator & pressure_equalizer & cooling & fan_mover & pa_processor_filter & output);
    else
    	tbb::parallel_pipeline(12, generator & cooling & fan_mover & pa_processor_filter & output);
}

std::string GCode::placeholder_parser_process(const std::string &name, const std::string &templ, unsigned int current_filament_id, const DynamicConfig *config_override)
{
    // Orca: Added CMake config option since debug is rarely used in current workflow.
    // Also changed from throwing error immediately to storing messages till slicing is completed
    // to raise all errors at the same time.
#if ORCA_CHECK_GCODE_PLACEHOLDERS
    if (config_override) {
        const auto& custom_gcode_placeholders = custom_gcode_specific_placeholders();

        // 1-st check: custom G-code "name" have to be present in s_CustomGcodeSpecificPlaceholders;
        //if (custom_gcode_placeholders.count(name) > 0) {
        //    const auto& placeholders = custom_gcode_placeholders.at(name);
        if (auto it = custom_gcode_placeholders.find(name); it != custom_gcode_placeholders.end()) {
            const auto& placeholders = it->second;

            for (const std::string& key : config_override->keys()) {
                // 2-nd check: "key" have to be present in s_CustomGcodeSpecificPlaceholders for "name" custom G-code ;
                if (std::find(placeholders.begin(), placeholders.end(), key) == placeholders.end()) {
                    auto& vector = m_placeholder_error_messages[name + " - option not specified for custom gcode type (s_CustomGcodeSpecificPlaceholders)"];
                    if (std::find(vector.begin(), vector.end(), key) == vector.end())
                        vector.emplace_back(key);
                }
                // 3-rd check: "key" have to be present in CustomGcodeSpecificConfigDef for "key" placeholder;
                if (!custom_gcode_specific_config_def.has(key)) {
                    auto& vector = m_placeholder_error_messages[name + " - option has no definition (CustomGcodeSpecificConfigDef)"];
                    if (std::find(vector.begin(), vector.end(), key) == vector.end())
                        vector.emplace_back(key);
                }
            }
        }
        else {
            auto& vector = m_placeholder_error_messages[name + " - gcode type not found in s_CustomGcodeSpecificPlaceholders"];
            if (vector.empty())
                vector.emplace_back("");
        }
    }
#endif

PlaceholderParserIntegration &ppi = m_placeholder_parser_integration;
    try {
        ppi.update_from_gcodewriter(m_writer);
        std::string output = ppi.parser.process(templ, current_filament_id, config_override, &ppi.output_config, &ppi.context);
        ppi.validate_output_vector_variables();

        if (const std::vector<double> &pos = ppi.opt_position->values; ppi.position != pos) {
            // Update G-code writer.
            m_writer.set_position({ pos[0], pos[1], pos[2] });
            this->set_last_pos(this->gcode_to_point({ pos[0], pos[1] }));
        }

        for (const Extruder &e : m_writer.extruders()) {
            unsigned int eid = e.id();
            assert(eid < ppi.num_extruders);
            if ( eid < ppi.num_extruders) {
                if (! m_writer.config.use_relative_e_distances && ! is_approx(ppi.e_position[eid], ppi.opt_e_position->values[eid]))
                    const_cast<Extruder&>(e).set_position(ppi.opt_e_position->values[eid]);
                if (! is_approx(ppi.e_retracted[eid], ppi.opt_e_retracted->values[eid]) || 
                    ! is_approx(ppi.e_restart_extra[eid], ppi.opt_e_restart_extra->values[eid]))
                    const_cast<Extruder&>(e).set_retracted(ppi.opt_e_retracted->values[eid], ppi.opt_e_restart_extra->values[eid]);
            }
        }

        return output;
    } 
    catch (std::runtime_error &err) 
    {
        // Collect the names of failed template substitutions for error reporting.
        auto it = ppi.failed_templates.find(name);
        if (it == ppi.failed_templates.end())
            // Only if there was no error reported for this template, store the first error message into the map to be reported.
            // We don't want to collect error message for each and every occurence of a single custom G-code section.
            ppi.failed_templates.insert(it, std::make_pair(name, std::string(err.what())));
        // Insert the macro error message into the G-code.
        return
            std::string("\n!!!!! Failed to process the custom G-code template ") + name + "\n" +
            err.what() +
            "!!!!! End of an error report for the custom G-code template " + name + "\n\n";
    }
}


// Print the machine envelope G-code for the Marlin firmware based on the "machine_max_xxx" parameters.
// Do not process this piece of G-code by the time estimator, it already knows the values through another sources.
void GCode::print_machine_envelope(GCodeOutputStream &file, Print &print, int extruder_id)
{
    int matched_machine_limit_idx = get_extruder_id(extruder_id) * 2;
    const auto flavor = print.config().gcode_flavor.value;
    if ((flavor == gcfMarlinLegacy || flavor == gcfMarlinFirmware || flavor == gcfRepRapFirmware) &&
        print.config().emit_machine_limits_to_gcode.value == true) {
        int factor = flavor == gcfRepRapFirmware ? 60 : 1; // RRF M203 and M566 are in mm/min
        file.write_format("M201 X%d Y%d Z%d E%d\n",
            int(print.config().machine_max_acceleration_x.values[matched_machine_limit_idx] + 0.5),
            int(print.config().machine_max_acceleration_y.values[matched_machine_limit_idx] + 0.5),
            int(print.config().machine_max_acceleration_z.values[matched_machine_limit_idx] + 0.5),
            int(print.config().machine_max_acceleration_e.values[matched_machine_limit_idx] + 0.5));
        file.write_format("M203 X%d Y%d Z%d E%d\n",
            int(print.config().machine_max_speed_x.values[matched_machine_limit_idx] * factor + 0.5),
            int(print.config().machine_max_speed_y.values[matched_machine_limit_idx] * factor + 0.5),
            int(print.config().machine_max_speed_z.values[matched_machine_limit_idx] * factor + 0.5),
            int(print.config().machine_max_speed_e.values[matched_machine_limit_idx] * factor + 0.5));

        // Now M204 - acceleration. This one is quite hairy thanks to how Marlin guys care about
        // Legacy Marlin should export travel acceleration the same as printing acceleration.
        // MarlinFirmware has the two separated.
        int travel_acc = flavor == gcfMarlinLegacy
                       ? int(print.config().machine_max_acceleration_extruding.values.front() + 0.5)
                       : int(print.config().machine_max_acceleration_travel.values.front() + 0.5);
        if (flavor == gcfRepRapFirmware)
            file.write_format("M204 P%d T%d ; sets acceleration (P, T), mm/sec^2\n",
                int(print.config().machine_max_acceleration_extruding.values.front() + 0.5),
                travel_acc);
        else if (flavor == gcfMarlinFirmware)
            // New Marlin uses M204 P[print] R[retract] T[travel]
            file.write_format("M204 P%d R%d T%d ; sets acceleration (P, T) and retract acceleration (R), mm/sec^2\n",
                int(print.config().machine_max_acceleration_extruding.values.front() + 0.5),
                int(print.config().machine_max_acceleration_retracting.values.front() + 0.5),
                int(print.config().machine_max_acceleration_travel.values.front() + 0.5));
        else
            file.write_format("M204 P%d R%d T%d\n",
                int(print.config().machine_max_acceleration_extruding.values.front() + 0.5),
                int(print.config().machine_max_acceleration_retracting.values.front() + 0.5),
                travel_acc);

        assert(is_decimal_separator_point());
        file.write_format(flavor == gcfRepRapFirmware
            ? "M566 X%.2lf Y%.2lf Z%.2lf E%.2lf ; sets the jerk limits, mm/min\n"
            : "M205 X%.2lf Y%.2lf Z%.2lf E%.2lf ; sets the jerk limits, mm/sec\n",
            print.config().machine_max_jerk_x.values.front() * factor,
            print.config().machine_max_jerk_y.values.front() * factor,
            print.config().machine_max_jerk_z.values.front() * factor,
            print.config().machine_max_jerk_e.values.front() * factor);

        // New Marlin uses M205 J[mm] for junction deviation (only apply if it is > 0)
        file.write_format(writer().set_junction_deviation(config().machine_max_junction_deviation.values.front()).c_str());
    }
}

// BBS
int GCode::get_bed_temperature(const int extruder_id, const bool is_first_layer, const BedType bed_type) const
{
    std::string bed_temp_key = is_first_layer ? get_bed_temp_1st_layer_key(bed_type) : get_bed_temp_key(bed_type);
    const ConfigOptionInts* bed_temp_opt = m_config.option<ConfigOptionInts>(bed_temp_key);
    return bed_temp_opt->get_at(extruder_id);
}

int GCode::get_highest_bed_temperature(const bool is_first_layer, const Print& print) const
{
    auto bed_type = m_config.curr_bed_type;
    int bed_temp = 0;
    for (auto fidx : print.get_slice_used_filaments(is_first_layer)) {
        bed_temp = std::max(bed_temp, get_bed_temperature(fidx, is_first_layer, bed_type));
    }
    return bed_temp;
}

// Write 1st layer bed temperatures into the G-code.
// Only do that if the start G-code does not already contain any M-code controlling an extruder temperature.
// M140 - Set Extruder Temperature
// M190 - Set Extruder Temperature and Wait
void GCode::_print_first_layer_bed_temperature(GCodeOutputStream &file, Print &print, const std::string &gcode, unsigned int first_printing_extruder_id, bool wait)
{
    // Initial bed temperature based on the first extruder.
    // BBS
    std::vector<int> temps_per_bed;
    int bed_temp = 0;
    if (m_config.bed_temperature_formula.value == BedTempFormula::btfHighestTemp) {
        bed_temp = get_highest_bed_temperature(true, print);
    }
    else {
        bed_temp = get_bed_temperature(first_printing_extruder_id, true, print.config().curr_bed_type);
    }
    // Is the bed temperature set by the provided custom G-code?
    int  temp_by_gcode     = -1;
    bool temp_set_by_gcode = custom_gcode_sets_temperature(gcode, 140, 190, false, temp_by_gcode);
    // BBS
#if 0
    if (temp_set_by_gcode && temp_by_gcode >= 0 && temp_by_gcode < 1000)
        temp = temp_by_gcode;
#endif

    // Always call m_writer.set_bed_temperature() so it will set the internal "current" state of the bed temp as if
    // the custom start G-code emited these.
    std::string set_temp_gcode = m_writer.set_bed_temperature(bed_temp, wait);
    if (! temp_set_by_gcode)
        file.write(set_temp_gcode);
}

// Write 1st layer extruder temperatures into the G-code.
// Only do that if the start G-code does not already contain any M-code controlling an extruder temperature.
// M104 - Set Extruder Temperature
// M109 - Set Extruder Temperature and Wait
// RepRapFirmware: G10 Sxx
void GCode::_print_first_layer_extruder_temperatures(GCodeOutputStream &file, Print &print, const std::string &gcode, unsigned int first_printing_extruder_id, bool wait)
{
    // Is the bed temperature set by the provided custom G-code?
    int  temp_by_gcode = -1;
    bool include_g10   = print.config().gcode_flavor == gcfRepRapFirmware;
    if (custom_gcode_sets_temperature(gcode, 104, 109, include_g10, temp_by_gcode)) {
        // Set the extruder temperature at m_writer, but throw away the generated G-code as it will be written with the custom G-code.
        int temp = print.config().nozzle_temperature_initial_layer.get_at(first_printing_extruder_id);
        if (temp_by_gcode >= 0 && temp_by_gcode < 1000)
            temp = temp_by_gcode;
        m_writer.set_temperature(temp, wait, first_printing_extruder_id);
    } else {
        // Custom G-code does not set the extruder temperature. Do it now.
        if (print.config().single_extruder_multi_material.value) {
            // Set temperature of the first printing extruder only.
            int temp = print.config().nozzle_temperature_initial_layer.get_at(first_printing_extruder_id);
            if (temp > 0)
                file.write(m_writer.set_temperature(temp, wait, first_printing_extruder_id));
        } else {
            // Set temperatures of all the printing extruders.
            for (unsigned int tool_id : print.extruders()) {
                int temp = print.config().nozzle_temperature_initial_layer.get_at(tool_id);
                if (m_ooze_prevention.enable && tool_id != first_printing_extruder_id) {
                    if (print.config().idle_temperature.get_at(tool_id) == 0)
                        temp += print.config().standby_temperature_delta.value;
                    else
                        temp = print.config().idle_temperature.get_at(tool_id);
                }
                if (temp > 0)
                    file.write(m_writer.set_temperature(temp, wait, tool_id));
            }
        }
    }
}

inline GCode::ObjectByExtruder& object_by_extruder(
    std::map<unsigned int, std::vector<GCode::ObjectByExtruder>> &by_extruder,
    unsigned int                                                  extruder_id,
    size_t                                                        object_idx,
    size_t                                                        num_objects)
{
    std::vector<GCode::ObjectByExtruder> &objects_by_extruder = by_extruder[extruder_id];
    if (objects_by_extruder.empty())
        objects_by_extruder.assign(num_objects, GCode::ObjectByExtruder());
    return objects_by_extruder[object_idx];
}

inline std::vector<GCode::ObjectByExtruder::Island>& object_islands_by_extruder(
    std::map<unsigned int, std::vector<GCode::ObjectByExtruder>>  &by_extruder,
    unsigned int                                                   extruder_id,
    size_t                                                         object_idx,
    size_t                                                         num_objects,
    size_t                                                         num_islands)
{
    std::vector<GCode::ObjectByExtruder::Island> &islands = object_by_extruder(by_extruder, extruder_id, object_idx, num_objects).islands;
    if (islands.empty())
        islands.assign(num_islands, GCode::ObjectByExtruder::Island());
    return islands;
}

std::vector<GCode::InstanceToPrint> GCode::sort_print_object_instances(
    std::vector<GCode::ObjectByExtruder> 		&objects_by_extruder,
    const std::vector<LayerToPrint> 			&layers,
    // Ordering must be defined for normal (non-sequential print).
    const std::vector<const PrintInstance*> 	*ordering,
    // For sequential print, the instance of the object to be printing has to be defined.
    const size_t                     		 	 single_object_instance_idx)
{
    std::vector<InstanceToPrint> out;

    if (ordering == nullptr) {
        // Sequential print, single object is being printed.
        for (ObjectByExtruder &object_by_extruder : objects_by_extruder) {
            const size_t       layer_id     = &object_by_extruder - objects_by_extruder.data();
            //BBS:add the support of shared print object
            const PrintObject *print_object = layers[layer_id].original_object;
            //const PrintObject *print_object = layers[layer_id].object();
            if (print_object)
                out.emplace_back(object_by_extruder, layer_id, *print_object, single_object_instance_idx, print_object->instances()[single_object_instance_idx].model_instance->get_labeled_id());
        }
    } else {
        // Create mapping from PrintObject* to ObjectByExtruder*.
        std::vector<std::pair<const PrintObject*, ObjectByExtruder*>> sorted;
        sorted.reserve(objects_by_extruder.size());
        for (ObjectByExtruder &object_by_extruder : objects_by_extruder) {
            const size_t       layer_id     = &object_by_extruder - objects_by_extruder.data();
            //BBS:add the support of shared print object
            const PrintObject *print_object = layers[layer_id].original_object;
            //const PrintObject *print_object = layers[layer_id].object();
            if (print_object)
                sorted.emplace_back(print_object, &object_by_extruder);
        }
        std::sort(sorted.begin(), sorted.end());

        if (! sorted.empty()) {
            out.reserve(sorted.size());
            for (const PrintInstance *instance : *ordering) {
                const PrintObject &print_object = *instance->print_object;
                //BBS:add the support of shared print object
                //const PrintObject* print_obj_ptr = &print_object;
                //if (print_object.get_shared_object())
                //    print_obj_ptr = print_object.get_shared_object();
                std::pair<const PrintObject*, ObjectByExtruder*> key(&print_object, nullptr);
                auto it = std::lower_bound(sorted.begin(), sorted.end(), key);
                if (it != sorted.end() && it->first == &print_object)
                    // ObjectByExtruder for this PrintObject was found.
                    out.emplace_back(*it->second, it->second - objects_by_extruder.data(), print_object, instance - print_object.instances().data(), instance->model_instance->get_labeled_id());
            }
        }
    }
    return out;
}

namespace ProcessLayer
{

    static std::string emit_custom_gcode_per_print_z(
        GCode                                                   &gcodegen,
        const CustomGCode::Item 								*custom_gcode,
        unsigned int                                             current_extruder_id,
        // ID of the first extruder printing this layer.
        unsigned int                                             first_extruder_id,
        const PrintConfig                                       &config)
    {
        std::string gcode;
        // BBS
        bool single_filament_print = config.filament_diameter.size() == 1;

        if (custom_gcode != nullptr) {
            // Extruder switches are processed by LayerTools, they should be filtered out.
            assert(custom_gcode->type != CustomGCode::ToolChange);

            CustomGCode::Type   gcode_type = custom_gcode->type;
            bool  				color_change = gcode_type == CustomGCode::ColorChange;
            bool 				tool_change = gcode_type == CustomGCode::ToolChange;
            // Tool Change is applied as Color Change for a single extruder printer only.
            assert(!tool_change || single_filament_print);

            std::string pause_print_msg;
            int m600_extruder_before_layer = -1;
            if (color_change && custom_gcode->extruder > 0)
                m600_extruder_before_layer = custom_gcode->extruder - 1;
            else if (gcode_type == CustomGCode::PausePrint)
                pause_print_msg = custom_gcode->extra;
            //BBS: inserting color gcode is removed
#if 0
            // we should add or not colorprint_change in respect to nozzle_diameter count instead of really used extruders count
            if (color_change || tool_change)
            {
                assert(m600_extruder_before_layer >= 0);
                // Color Change or Tool Change as Color Change.
                // add tag for processor
                gcode += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Color_Change) + ",T" + std::to_string(m600_extruder_before_layer) + "," + custom_gcode->color + "\n";

                if (!single_filament_print && m600_extruder_before_layer >= 0 && first_extruder_id != (unsigned)m600_extruder_before_layer
                    // && !MMU1
                    ) {
                    //! FIXME_in_fw show message during print pause
                    DynamicConfig cfg;
                    cfg.set_key_value("color_change_extruder", new ConfigOptionInt(m600_extruder_before_layer));
                    gcode += gcodegen.placeholder_parser_process("machine_pause_gcode", config.machine_pause_gcode, current_extruder_id, &cfg);
                    gcode += "\n";
                    gcode += "M117 Change filament for Extruder " + std::to_string(m600_extruder_before_layer) + "\n";
                }
                else {
                    gcode += gcodegen.placeholder_parser_process("color_change_gcode", config.color_change_gcode, current_extruder_id);
                    gcode += "\n";
                    //FIXME Tell G-code writer that M600 filled the extruder, thus the G-code writer shall reset the extruder to unretracted state after
                    // return from M600. Thus the G-code generated by the following line is ignored.
                    // see GH issue #6362
                    gcodegen.writer().unretract();
                }
            }
            else {
#endif
                if (gcode_type == CustomGCode::PausePrint) // Pause print
                {
                    // add tag for processor
                    gcode += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Pause_Print) + "\n";
                    //! FIXME_in_fw show message during print pause
                    //if (!pause_print_msg.empty())
                    //    gcode += "M117 " + pause_print_msg + "\n";
                    gcode += gcodegen.placeholder_parser_process("machine_pause_gcode", config.machine_pause_gcode, current_extruder_id) + "\n";
                }
                else {
                    // add tag for processor
                    gcode += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Custom_Code) + "\n";
                    if (gcode_type == CustomGCode::Template)    // Template Custom Gcode
                        gcode += gcodegen.placeholder_parser_process("template_custom_gcode", config.template_custom_gcode, current_extruder_id);
                    else                                        // custom Gcode
                        gcode += custom_gcode->extra;

                }
                gcode += "\n";
#if 0
            }
#endif
        }

        return gcode;
    }
} // namespace ProcessLayer

namespace Skirt {
    static void skirt_loops_per_extruder_all_printing(const Print &print, const ExtrusionEntityCollection &skirt, const LayerTools &layer_tools, std::map<unsigned int, std::pair<size_t, size_t>> &skirt_loops_per_extruder_out)
    {
        // Prime all extruders printing over the 1st layer over the skirt lines.
        size_t n_loops = skirt.entities.size();
        size_t n_tools = layer_tools.extruders.size();
        size_t lines_per_extruder = (n_loops + n_tools - 1) / n_tools;

        // BBS. Extrude skirt with first extruder if min_skirt_length is zero
        //ORCA: Always extrude skirt with first extruder, independantly of if the minimum skirt length is zero or not. The code below
        // is left as a placeholder for when a multiextruder support is implemented. Then we will need to extrude the skirt loops for each extruder.
        //const PrintConfig &config = print.config();
        //if (config.min_skirt_length.value < EPSILON) {
            skirt_loops_per_extruder_out[layer_tools.extruders.front()] = std::pair<size_t, size_t>(0, n_loops);
        //} else {
        //    for (size_t i = 0; i < n_loops; i += lines_per_extruder)
        //        skirt_loops_per_extruder_out[layer_tools.extruders[i / lines_per_extruder]] = std::pair<size_t, size_t>(i, std::min(i + lines_per_extruder, n_loops));
        //}
    }

    static std::map<unsigned int, std::pair<size_t, size_t>> make_skirt_loops_per_extruder_1st_layer(
        const Print             				&print,
        const ExtrusionEntityCollection &skirt,
        const LayerTools                		&layer_tools,
        // Heights (print_z) at which the skirt has already been extruded.
        std::vector<coordf_t>  			    	&skirt_done)
    {
        // Extrude skirt at the print_z of the raft layers and normal object layers
        // not at the print_z of the interlaced support material layers.
        std::map<unsigned int, std::pair<size_t, size_t>> skirt_loops_per_extruder_out;
        //For sequential print, the following test may fail when extruding the 2nd and other objects.
        // assert(skirt_done.empty());
        if (skirt_done.empty() && print.has_skirt() && ! skirt.entities.empty() && layer_tools.has_skirt) {
            skirt_loops_per_extruder_all_printing(print, skirt, layer_tools, skirt_loops_per_extruder_out);
            skirt_done.emplace_back(layer_tools.print_z);
        }
        return skirt_loops_per_extruder_out;
    }

    static std::map<unsigned int, std::pair<size_t, size_t>> make_skirt_loops_per_extruder_other_layers(
        const Print 							&print,
        const ExtrusionEntityCollection     &skirt,
        const LayerTools                		&layer_tools,
        // Heights (print_z) at which the skirt has already been extruded.
        std::vector<coordf_t>			    	&skirt_done)
    {
        // Extrude skirt at the print_z of the raft layers and normal object layers
        // not at the print_z of the interlaced support material layers.
        std::map<unsigned int, std::pair<size_t, size_t>> skirt_loops_per_extruder_out;
        if (print.has_skirt() && ! skirt.entities.empty() && layer_tools.has_skirt &&
            // Not enough skirt layers printed yet.
            //FIXME infinite or high skirt does not make sense for sequential print!
            (skirt_done.size() < (size_t)print.config().skirt_height.value || print.has_infinite_skirt())) {
            bool valid = ! skirt_done.empty() && skirt_done.back() < layer_tools.print_z - EPSILON;
            assert(valid);
            // This print_z has not been extruded yet (sequential print)
            // FIXME: The skirt_done should not be empty at this point. The check is a workaround
            if (valid) {
#if 0
                // Prime just the first printing extruder. This is original Slic3r's implementation.
                skirt_loops_per_extruder_out[layer_tools.extruders.front()] = std::pair<size_t, size_t>(0, print.config().skirt_loops.value);
#else
                // Prime all extruders planned for this layer, see
                skirt_loops_per_extruder_all_printing(print, skirt, layer_tools, skirt_loops_per_extruder_out);
#endif
                assert(!skirt_done.empty());
                skirt_done.emplace_back(layer_tools.print_z);
            }
        }
        return skirt_loops_per_extruder_out;
    }

    static Point find_start_point(ExtrusionLoop& loop, float start_angle) {
        coord_t min_x = std::numeric_limits<coord_t>::max();
        coord_t max_x = std::numeric_limits<coord_t>::min();
        coord_t min_y = min_x;
        coord_t max_y = max_x;

        Points pts;
        loop.collect_points(pts);
        for (Point pt: pts) {
            if (pt.x() < min_x)
                min_x = pt.x();
            else if (pt.x() > max_x)
                max_x = pt.x();
            if (pt.y() < min_y)
                min_y = pt.y();
            else if (pt.y() > max_y)
                max_y = pt.y();
        }

        Point center((min_x + max_x)/2., (min_y + max_y)/2.);
        double r       = center.distance_to(Point(min_x, min_y));
        double deg     = start_angle * PI / 180;
        double shift_x = r * std::cos(deg);
        double shift_y = r * std::sin(deg);
        return Point(center.x()+shift_x, center.y() + shift_y);
    }

} // namespace Skirt

// Orca: Klipper can't parse object names with spaces and other spetical characters
std::string sanitize_instance_name(const std::string& name) {
    // Replace sequences of non-word characters with an underscore
    std::string result = std::regex_replace(name, std::regex("[ !@#$%^&*()=+\\[\\]{};:\",']+"), "_");
    // Remove leading and trailing underscores
    if (!result.empty() && result.front() == '_') {
        result.erase(result.begin());
    }
    if (!result.empty() && result.back() == '_') {
        result.erase(result.end() - 1);
    }

    return result;
}

inline std::string get_instance_name(const PrintObject *object, size_t inst_id) {
    auto obj_name = sanitize_instance_name(object->model_object()->name);
    auto name = (boost::format("%1%_id_%2%_copy_%3%") % obj_name % object->get_id() % inst_id).str();
    return sanitize_instance_name(name);
}

inline std::string get_instance_name(const PrintObject *object, const PrintInstance &inst) {
    return get_instance_name(object, inst.id);
}

std::string GCode::generate_skirt(const Print &print,
        const ExtrusionEntityCollection &skirt,
        const Point& offset,
        const float skirt_start_angle,
        const LayerTools &layer_tools,
        const Layer& layer,
        unsigned int extruder_id)
{
    
    bool first_layer = (layer.id() == 0 && abs(layer.bottom_z()) < EPSILON);
    std::string gcode;
    // Extrude skirt at the print_z of the raft layers and normal object layers
    // not at the print_z of the interlaced support material layers.
    // Map from extruder ID to <begin, end> index of skirt loops to be extruded with that extruder.
    std::map<unsigned int, std::pair<size_t, size_t>> skirt_loops_per_extruder;
    skirt_loops_per_extruder = first_layer ?
        Skirt::make_skirt_loops_per_extruder_1st_layer(print, skirt, layer_tools, m_skirt_done) :
        Skirt::make_skirt_loops_per_extruder_other_layers(print, skirt, layer_tools, m_skirt_done);

    if (auto loops_it = skirt_loops_per_extruder.find(extruder_id); loops_it != skirt_loops_per_extruder.end()) {
        const std::pair<size_t, size_t> loops = loops_it->second;
       
        set_origin(unscaled(offset));

        m_avoid_crossing_perimeters.use_external_mp();
        Flow layer_skirt_flow = print.skirt_flow().with_height(float(m_skirt_done.back() - (m_skirt_done.size() == 1 ? 0. : m_skirt_done[m_skirt_done.size() - 2])));
        double mm3_per_mm = layer_skirt_flow.mm3_per_mm();
        // Decide where to start looping:
        // - If it’s the first layer or if we do NOT want a single-wall skirt/draft shield,
        //   start from loops.first (all loops).
        // - Otherwise, if single_loop_draft_shield == true (and not the first layer),
        //   start from loops.second - 1 (just one loop).
        const size_t start_idx = (first_layer || !print.m_config.single_loop_draft_shield)
                                 ? loops.first
                                 : (loops.second - 1);

        // Loop over the skirt loops and extrude
        for (size_t i = start_idx; i < loops.second; ++i) {
            // Adjust flow according to this layer's layer height.
            ExtrusionLoop loop = *dynamic_cast<const ExtrusionLoop*>(skirt.entities[i]);
            for (ExtrusionPath &path : loop.paths) {
                path.height = layer_skirt_flow.height();
                path.mm3_per_mm = mm3_per_mm;
            }

            //FIXME using the support_speed of the 1st object printed.
            if (first_layer && i==loops.first) {
                //set skirt start point location
                const Point desired_start_point = Skirt::find_start_point(loop, skirt_start_angle);
                gcode += this->extrude_loop(loop, "skirt", m_config.support_speed.value, {}, &desired_start_point);
            }
            else
                gcode += this->extrude_loop(loop, "skirt", m_config.support_speed.value);

            // If we only want a single wall on non-first layers, break now
            if (!first_layer && print.m_config.single_loop_draft_shield) {
                break;
            }
        }
        m_avoid_crossing_perimeters.use_external_mp(false);
        // Allow a straight travel move to the first object point if this is the first layer (but don't in next layers).
        if (first_layer && loops.first == 0)
            m_avoid_crossing_perimeters.disable_once();
    }
    return gcode;
}

// In sequential mode, process_layer is called once per each object and its copy,
// therefore layers will contain a single entry and single_object_instance_idx will point to the copy of the object.
// In non-sequential mode, process_layer is called per each print_z height with all object and support layers accumulated.
// For multi-material prints, this routine minimizes extruder switches by gathering extruder specific extrusion paths
// and performing the extruder specific extrusions together.
LayerResult GCode::process_layer(
    const Print                    			&print,
    // Set of object & print layers of the same PrintObject and with the same print_z.
    const std::vector<LayerToPrint> 		&layers,
    const LayerTools        		        &layer_tools,
    const bool                               last_layer,
    // Pairs of PrintObject index and its instance index.
    const std::vector<const PrintInstance*> *ordering,
    const int                               most_used_extruder,
    // If set to size_t(-1), then print all copies of all objects.
    // Otherwise print a single copy of a single object.
    const size_t                     		 single_object_instance_idx,
    // BBS
    const bool                               prime_extruder)
{
    assert(! layers.empty());
    // Either printing all copies of all objects, or just a single copy of a single object.
    assert(single_object_instance_idx == size_t(-1) || layers.size() == 1);

    // First object, support and raft layer, if available.
    const Layer         *object_layer  = nullptr;
    const SupportLayer  *support_layer = nullptr;
    const SupportLayer  *raft_layer    = nullptr;
    for (const LayerToPrint &l : layers) {
        if (l.object_layer && ! object_layer)
            object_layer = l.object_layer;
        if (l.support_layer) {
            if (! support_layer)
                support_layer = l.support_layer;
            if (! raft_layer && support_layer->id() < support_layer->object()->slicing_parameters().raft_layers())
                raft_layer = support_layer;
        }
    }

    const Layer* layer_ptr = nullptr;
    if (object_layer != nullptr)
        layer_ptr = object_layer;
    else if (support_layer != nullptr)
        layer_ptr = support_layer;
    const Layer& layer = *layer_ptr;
    LayerResult   result { {}, layer.id(), false, last_layer };
    if (layer_tools.extruders.empty())
        // Nothing to extrude.
        return result;

    // Extract 1st object_layer and support_layer of this set of layers with an equal print_z.
    coordf_t             print_z       = layer.print_z;
    //BBS: using layer id to judge whether the layer is first layer is wrong. Because if the normal
    //support is attached above the object, and support layers has independent layer height, then the lowest support
    //interface layer id is 0.
    bool                 first_layer   = (layer.id() == 0 && abs(layer.bottom_z()) < EPSILON);
    m_writer.set_is_first_layer(first_layer);
    unsigned int         first_extruder_id = layer_tools.extruders.front();

    // Initialize config with the 1st object to be printed at this layer.
    m_config.apply(layer.object()->config(), true);

    // Check whether it is possible to apply the spiral vase logic for this layer.
    // Just a reminder: A spiral vase mode is allowed for a single object, single material print only.
    m_enable_loop_clipping = true;
    if (m_spiral_vase && layers.size() == 1 && support_layer == nullptr) {
        bool enable = (layer.id() > 0 || !print.has_brim()) && (layer.id() >= (size_t)print.config().skirt_height.value && ! print.has_infinite_skirt());
        if (enable) {
            for (const LayerRegion *layer_region : layer.regions())
                if (size_t(layer_region->region().config().bottom_shell_layers.value) > layer.id() ||
                    layer_region->perimeters.items_count() > 1u ||
                    layer_region->fills.items_count() > 0) {
                    enable = false;
                    break;
                }
        }
        result.spiral_vase_enable = enable;
        // If we're going to apply spiralvase to this layer, disable loop clipping.
        m_enable_loop_clipping = !enable;
    }

    std::string gcode;
    assert(is_decimal_separator_point()); // for the sprintfs

    // add tag for processor
    gcode += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Layer_Change) + "\n";
    // export layer z
    char buf[64];
    sprintf(buf, print.is_BBL_printer() ? "; Z_HEIGHT: %g\n" : ";Z:%g\n", print_z);
    gcode += buf;
    // export layer height
    float height = first_layer ? static_cast<float>(print_z) : static_cast<float>(print_z) - m_last_layer_z;
    sprintf(buf, ";%s%g\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height).c_str(), height);
    gcode += buf;
    // update caches
    m_last_layer_z = static_cast<float>(print_z);
    m_max_layer_z  = std::max(m_max_layer_z, m_last_layer_z);
    m_last_height = height;

    // Set new layer - this will change Z and force a retraction if retract_when_changing_layer is enabled.
    if (! m_config.before_layer_change_gcode.value.empty()) {
        DynamicConfig config;
        config.set_key_value("layer_num",   new ConfigOptionInt(m_layer_index + 1));
        config.set_key_value("layer_z",     new ConfigOptionFloat(print_z));
        config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
        gcode += this->placeholder_parser_process("before_layer_change_gcode",
            print.config().before_layer_change_gcode.value, m_writer.filament()->id(), &config)
            + "\n";
    }

    PrinterStructure printer_structure           = m_config.printer_structure.value;
    PrintSequence print_sequence = m_config.print_sequence;
    bool sequence_by_layer = print_sequence == PrintSequence::ByLayer;
    bool is_i3_printer = printer_structure == PrinterStructure::psI3;
    bool is_multi_extruder = m_config.nozzle_diameter.size() > 1;

    bool need_insert_timelapse_gcode_for_traditional = false;
    if ((!m_wipe_tower || !m_wipe_tower->enable_timelapse_print()) && (is_BBL_Printer() || !m_config.time_lapse_gcode.value.empty())) {
        need_insert_timelapse_gcode_for_traditional = ((is_i3_printer && !m_spiral_vase) || is_multi_extruder);
    }

    bool has_insert_timelapse_gcode = false;
    bool has_wipe_tower             = (layer_tools.has_wipe_tower && m_wipe_tower);


    ZHopType z_hope_type = ZHopType(FILAMENT_CONFIG(z_hop_types));
    LiftType auto_lift_type = LiftType::NormalLift;
    if (z_hope_type == ZHopType::zhtAuto || z_hope_type == ZHopType::zhtSpiral || z_hope_type == ZHopType::zhtSlope)
        auto_lift_type = LiftType::SpiralLift;

    // BBS: don't use lazy_raise when enable spiral vase
    gcode += this->change_layer(print_z);  // this will increase m_layer_index
    m_layer = &layer;
    m_object_layer_over_raft = false;

    if (!m_config.time_lapse_gcode.value.empty() && !is_BBL_Printer()) {
        DynamicConfig config;
        config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
        config.set_key_value("layer_z", new ConfigOptionFloat(print_z));
        config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
        gcode += this->placeholder_parser_process("timelapse_gcode",
             print.config().time_lapse_gcode.value, m_writer.filament()->id(), &config)
             + "\n";
    }

    if (!m_config.layer_change_gcode.value.empty()) {
        DynamicConfig config;
        config.set_key_value("most_used_physical_extruder_id", new ConfigOptionInt(m_config.physical_extruder_map.get_at(most_used_extruder)));
        config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
        config.set_key_value("layer_z",   new ConfigOptionFloat(print_z));
        gcode += this->placeholder_parser_process("layer_change_gcode",
            print.config().layer_change_gcode.value, m_writer.filament()->id(), &config)
            + "\n";
        config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
    }
    //BBS: set layer time fan speed after layer change gcode
    gcode += ";_SET_FAN_SPEED_CHANGING_LAYER\n";

    //Calibration Layer-specific GCode
    switch (print.calib_mode()) {
        case CalibMode::Calib_PA_Tower: {
            gcode += writer().set_pressure_advance(print.calib_params().start + static_cast<int>(print_z) * print.calib_params().step);
            break;
        }
        case CalibMode::Calib_Temp_Tower: {
            auto offset = static_cast<unsigned int>(print_z / 10.001) * 5;
            gcode += writer().set_temperature(print.calib_params().start - offset);
            break;
        }
        case CalibMode::Calib_VFA_Tower: {
            auto _speed = print.calib_params().start + std::floor(print_z / 5.0) * print.calib_params().step;
            m_calib_config.set_key_value("outer_wall_speed", new ConfigOptionFloat(std::round(_speed)));
            break;
        }
        case CalibMode::Calib_Vol_speed_Tower: {
            auto _speed = print.calib_params().start + print_z * print.calib_params().step;
            m_calib_config.set_key_value("outer_wall_speed", new ConfigOptionFloat(std::round(_speed)));
            break;
        }
        case CalibMode::Calib_Retraction_tower: {
            auto _length = print.calib_params().start + std::floor(std::max(0.0,print_z-0.4)) * print.calib_params().step;
            DynamicConfig _cfg;
            _cfg.set_key_value("retraction_length", new ConfigOptionFloats{_length});
            writer().config.apply(_cfg);
            sprintf(buf, "; Calib_Retraction_tower: Z_HEIGHT: %g, length:%g\n", print_z, _length);
            gcode += buf;
            break;
        }
        case CalibMode::Calib_Input_shaping_freq: {
            if (m_layer_index == 1){
                gcode += writer().set_input_shaping('A', print.calib_params().start, 0.f, print.calib_params().shaper_type);
                if (m_writer.get_gcode_flavor() == gcfKlipper) {
                    // Disable minimum cruise ratio to ensure consistent motion for calibration
                    gcode += "SET_VELOCITY_LIMIT MINIMUM_CRUISE_RATIO=0\n";
                }
            } else {
                if (print.calib_params().freqStartX == print.calib_params().freqStartY && print.calib_params().freqEndX == print.calib_params().freqEndY) {
                    gcode += writer().set_input_shaping('A', 0.f, this->interpolate_value_across_layers(print.calib_params().freqStartX, print.calib_params().freqEndX), "");
                } else {
                    gcode += writer().set_input_shaping('X', 0.f, this->interpolate_value_across_layers(print.calib_params().freqStartX, print.calib_params().freqEndX), "");
                    gcode += writer().set_input_shaping('Y', 0.f, this->interpolate_value_across_layers(print.calib_params().freqStartY, print.calib_params().freqEndY), "");
                }
            }
            break;
        }
        case CalibMode::Calib_Input_shaping_damp: {
            if (m_layer_index == 1){
                if (m_writer.get_gcode_flavor() == gcfKlipper) {
                    // Disable minimum cruise ratio to ensure consistent motion for calibration
                    gcode += "SET_VELOCITY_LIMIT MINIMUM_CRUISE_RATIO=0\n";
                }
                gcode += writer().set_input_shaping('X', 0.f, print.calib_params().freqStartX, print.calib_params().shaper_type);
                gcode += writer().set_input_shaping('Y', 0.f, print.calib_params().freqStartY, print.calib_params().shaper_type);
            } else {
                gcode += writer().set_input_shaping('A', this->interpolate_value_across_layers(print.calib_params().start, print.calib_params().end), 0.f, "");
            }
            break;
        }
        case CalibMode::Calib_Cornering: {
            if (m_writer.get_gcode_flavor() == gcfMarlinFirmware &&
                !m_config.machine_max_junction_deviation.values.empty() &&
                m_config.machine_max_junction_deviation.values.front() > 0) {
                gcode += writer().set_junction_deviation(this->interpolate_value_across_layers(print.calib_params().start, print.calib_params().end));
            } else {
                gcode += writer().set_jerk_xy(this->interpolate_value_across_layers(print.calib_params().start, print.calib_params().end));
            }
            break;
        }
    }

    //BBS
    if (first_layer) {
        // Orca: we don't need to optimize the Klipper as only set once
        if (m_config.default_acceleration.value > 0 && m_config.initial_layer_acceleration.value > 0) {
            gcode += m_writer.set_print_acceleration((unsigned int)floor(m_config.initial_layer_acceleration.value + 0.5));
        }

        if (m_config.default_jerk.value > 0 && m_config.initial_layer_jerk.value > 0) {
            gcode += m_writer.set_jerk_xy(m_config.initial_layer_jerk.value);
        }

        if (m_writer.get_gcode_flavor() == gcfMarlinFirmware && m_config.default_junction_deviation.value > 0) {
            gcode += m_writer.set_junction_deviation(m_config.default_junction_deviation.value);
        }
    }

    if (!first_layer && !m_second_layer_things_done) {
        // Orca: set power loss recovery
        const auto plr_mode = print.config().enable_power_loss_recovery.value;
        gcode += m_writer.enable_power_loss_recovery(plr_mode);

        if (print.is_BBL_printer()) {
            // BBS: open first layer inspection at second layer
            if (print.config().scan_first_layer.value) {
                // BBS: retract first to avoid droping when scan model
                gcode += this->retract();
                gcode += "M976 S1 P1 ; scan model before printing 2nd layer\n";
                gcode += "M400 P100\n";
                gcode += this->unretract();
            }
        }
      // Reset acceleration at sencond layer
      // Orca: only set once, don't need to call set_accel_and_jerk
      if (m_config.default_acceleration.value > 0 && m_config.initial_layer_acceleration.value > 0) {
        gcode += m_writer.set_print_acceleration((unsigned int) floor(m_config.default_acceleration.value + 0.5));
      }

      if (m_config.default_jerk.value > 0 && m_config.initial_layer_jerk.value > 0) {
        gcode += m_writer.set_jerk_xy(m_config.default_jerk.value);
      }

        // Transition from 1st to 2nd layer. Adjust nozzle temperatures as prescribed by the nozzle dependent
        // nozzle_temperature_initial_layer vs. temperature settings.
        for (const Extruder& extruder : m_writer.extruders()) {
            if ((print.config().single_extruder_multi_material.value || m_ooze_prevention.enable) &&
                extruder.id() != m_writer.filament()->id())
                // In single extruder multi material mode, set the temperature for the current extruder only.
                continue;
            int temperature = print.config().nozzle_temperature.get_at(extruder.id());
            if (temperature > 0 && temperature != print.config().nozzle_temperature_initial_layer.get_at(extruder.id()))
                gcode += m_writer.set_temperature(temperature, false, extruder.id());
        }

        // BBS
        int bed_temp = 0;
        if (m_config.bed_temperature_formula == BedTempFormula::btfHighestTemp)
            bed_temp = get_highest_bed_temperature(false,print);
        else
            bed_temp = get_bed_temperature(first_extruder_id, false, m_config.curr_bed_type);
        gcode += m_writer.set_bed_temperature(bed_temp);
        // Mark the temperature transition from 1st to 2nd layer to be finished.
        m_second_layer_things_done = true;
    }

    if (single_object_instance_idx == size_t(-1)) {
        // Normal (non-sequential) print.
        gcode += ProcessLayer::emit_custom_gcode_per_print_z(*this, layer_tools.custom_gcode, m_writer.filament()->id(), first_extruder_id, print.config());
    }

    // BBS: get next extruder according to flush and soluble
    auto get_next_extruder = [&](int current_extruder,const std::vector<unsigned int>&extruders) {
        std::vector<float> flush_matrix(cast<float>(get_flush_volumes_matrix(m_config.flush_volumes_matrix.values, 0, m_config.nozzle_diameter.values.size())));
        const unsigned int number_of_extruders = (unsigned int)(sqrt(flush_matrix.size()) + EPSILON);
        // Extract purging volumes for each extruder pair:
        std::vector<std::vector<float>> wipe_volumes;
        for (unsigned int i = 0; i < number_of_extruders; ++i)
            wipe_volumes.push_back(std::vector<float>(flush_matrix.begin() + i * number_of_extruders, flush_matrix.begin() + (i + 1) * number_of_extruders));
        unsigned int next_extruder = current_extruder;
        float min_flush = std::numeric_limits<float>::max();
        for (auto extruder_id : extruders) {
            if (print.config().filament_soluble.get_at(extruder_id) || extruder_id == current_extruder)
                continue;
            if (wipe_volumes[current_extruder][extruder_id] < min_flush) {
                next_extruder = extruder_id;
                min_flush = wipe_volumes[current_extruder][extruder_id];
            }
        }
        return next_extruder;
    };
    
    for (const auto &layer_to_print : layers) {
        if (layer_to_print.object_layer) {
            const auto& regions = layer_to_print.object_layer->regions();
            const bool  enable_overhang_speed = std::any_of(regions.begin(), regions.end(), [](const LayerRegion* r) {
                return r->has_extrusions() && r->region().config().enable_overhang_speed;
            });
            if (enable_overhang_speed) {
                m_extrusion_quality_estimator.prepare_for_new_layer(layer_to_print.original_object,
                                                                    layer_to_print.object_layer);
            }
        }
    }

    // Group extrusions by an extruder, then by an object, an island and a region.
    std::map<unsigned int, std::vector<ObjectByExtruder>> by_extruder;
    bool is_anything_overridden = const_cast<LayerTools&>(layer_tools).wiping_extrusions().is_anything_overridden();
    for (const LayerToPrint &layer_to_print : layers) {
        if (layer_to_print.support_layer != nullptr) {
            const SupportLayer &support_layer = *layer_to_print.support_layer;
            const PrintObject& object = *layer_to_print.original_object;
            if (! support_layer.support_fills.entities.empty()) {
                ExtrusionRole   role               = support_layer.support_fills.role();
                bool            has_support        = role == erMixed || role == erSupportMaterial || role == erSupportTransition;
                bool            has_interface      = role == erMixed || role == erSupportMaterialInterface;
                // Extruder ID of the support base. -1 if "don't care".
                unsigned int    support_extruder   = object.config().support_filament.value - 1;
                // Shall the support be printed with the active extruder, preferably with non-soluble, to avoid tool changes?
                bool            support_dontcare   = object.config().support_filament.value == 0;
                // Extruder ID of the support interface. -1 if "don't care".
                unsigned int    interface_extruder = object.config().support_interface_filament.value - 1;
                // Shall the support interface be printed with the active extruder, preferably with non-soluble, to avoid tool changes?
                bool            interface_dontcare = object.config().support_interface_filament.value == 0;

                // BBS: apply wiping overridden extruders
                WipingExtrusions& wiping_extrusions = const_cast<LayerTools&>(layer_tools).wiping_extrusions();
                if (support_dontcare) {
                    int extruder_override = wiping_extrusions.get_support_extruder_overrides(&object);
                    if (extruder_override >= 0) {
                        support_extruder = extruder_override;
                        support_dontcare = false;
                    }
                }

                if (interface_dontcare) {
                    int extruder_override = wiping_extrusions.get_support_interface_extruder_overrides(&object);
                    if (extruder_override >= 0) {
                        interface_extruder = extruder_override;
                        interface_dontcare = false;
                    }
                }

                // BBS: try to print support base with a filament other than interface filament
                if (support_dontcare && !interface_dontcare) {
                    unsigned int dontcare_extruder = first_extruder_id;
                    for (unsigned int extruder_id : layer_tools.extruders) {
                        if (print.config().filament_soluble.get_at(extruder_id))
                            continue;

                        //BBS: now we don't consider interface filament used in other object
                        if (extruder_id == interface_extruder)
                            continue;

                        dontcare_extruder = extruder_id;
                        break;
                    }
                #if 0
                    //BBS: not found a suitable extruder in current layer ,dontcare_extruider==first_extruder_id==interface_extruder
                    if (dontcare_extruder == interface_extruder && (object.config().support_interface_not_for_body && object.config().support_interface_filament.value!=0)) {
                        // BBS : get a suitable extruder from other layer
                        auto all_extruders = print.extruders();
                        dontcare_extruder = get_next_extruder(dontcare_extruder, all_extruders);
                    }
                #endif

                    if (support_dontcare)
                        support_extruder = dontcare_extruder;
                }
                else if (support_dontcare || interface_dontcare) {
                    // Some support will be printed with "don't care" material, preferably non-soluble.
                    // Is the current extruder assigned a soluble filament?
                    unsigned int dontcare_extruder = first_extruder_id;
                    if (print.config().filament_soluble.get_at(dontcare_extruder)) {
                        // The last extruder printed on the previous layer extrudes soluble filament.
                        // Try to find a non-soluble extruder on the same layer.
                        for (unsigned int extruder_id : layer_tools.extruders)
                            if (! print.config().filament_soluble.get_at(extruder_id)) {
                                dontcare_extruder = extruder_id;
                                break;
                            }
                    }
                    if (print.config().filament_is_support.get_at(dontcare_extruder)) {
                        // The last extruder printed on the previous layer extrudes support filament.
                        // Try to find a non-support extruder on the same layer.
                        for (unsigned int extruder_id : layer_tools.extruders)
                            if (!print.config().filament_is_support.get_at(extruder_id)) {
                                dontcare_extruder = extruder_id;
                                break;
                            }
                    }
                    if (support_dontcare)
                        support_extruder = dontcare_extruder;
                    if (interface_dontcare)
                        interface_extruder = dontcare_extruder;
                }
                // Both the support and the support interface are printed with the same extruder, therefore
                // the interface may be interleaved with the support base.
                bool single_extruder = ! has_support || support_extruder == interface_extruder;
                // Assign an extruder to the base.
                ObjectByExtruder &obj = object_by_extruder(by_extruder, has_support ? support_extruder : interface_extruder, &layer_to_print - layers.data(), layers.size());
                obj.support = &support_layer.support_fills;
                obj.support_extrusion_role = single_extruder ? erMixed : erSupportMaterial;
                if (! single_extruder && has_interface) {
                    ObjectByExtruder &obj_interface = object_by_extruder(by_extruder, interface_extruder, &layer_to_print - layers.data(), layers.size());
                    obj_interface.support = &support_layer.support_fills;
                    obj_interface.support_extrusion_role = erSupportMaterialInterface;
                }
            }
        }

        if (layer_to_print.object_layer != nullptr) {
            const Layer &layer = *layer_to_print.object_layer;
            // We now define a strategy for building perimeters and fills. The separation
            // between regions doesn't matter in terms of printing order, as we follow
            // another logic instead:
            // - we group all extrusions by extruder so that we minimize toolchanges
            // - we start from the last used extruder
            // - for each extruder, we group extrusions by island
            // - for each island, we extrude perimeters first, unless user set the infill_first
            //   option
            // (Still, we have to keep track of regions because we need to apply their config)
            size_t n_slices = layer.lslices.size();
            const std::vector<BoundingBox> &layer_surface_bboxes = layer.lslices_bboxes;
            // Traverse the slices in an increasing order of bounding box size, so that the islands inside another islands are tested first,
            // so we can just test a point inside ExPolygon::contour and we may skip testing the holes.
            std::vector<size_t> slices_test_order;
            slices_test_order.reserve(n_slices);
            for (size_t i = 0; i < n_slices; ++ i)
                slices_test_order.emplace_back(i);
            std::sort(slices_test_order.begin(), slices_test_order.end(), [&layer_surface_bboxes](size_t i, size_t j) {
                const Vec2d s1 = layer_surface_bboxes[i].size().cast<double>();
                const Vec2d s2 = layer_surface_bboxes[j].size().cast<double>();
                return s1.x() * s1.y() < s2.x() * s2.y();
            });
            auto point_inside_surface = [&layer, &layer_surface_bboxes](const size_t i, const Point &point) {
                const BoundingBox &bbox = layer_surface_bboxes[i];
                return point(0) >= bbox.min(0) && point(0) < bbox.max(0) &&
                       point(1) >= bbox.min(1) && point(1) < bbox.max(1) &&
                       layer.lslices[i].contour.contains(point);
            };

            for (size_t region_id = 0; region_id < layer.regions().size(); ++ region_id) {
                const LayerRegion *layerm = layer.regions()[region_id];
                if (layerm == nullptr)
                    continue;
                // PrintObjects own the PrintRegions, thus the pointer to PrintRegion would be unique to a PrintObject, they would not
                // identify the content of PrintRegion accross the whole print uniquely. Translate to a Print specific PrintRegion.
                const PrintRegion &region = print.get_print_region(layerm->region().print_region_id());

                // Now we must process perimeters and infills and create islands of extrusions in by_region std::map.
                // It is also necessary to save which extrusions are part of MM wiping and which are not.
                // The process is almost the same for perimeters and infills - we will do it in a cycle that repeats twice:
                std::vector<unsigned int> printing_extruders;
                for (const ObjectByExtruder::Island::Region::Type entity_type : { ObjectByExtruder::Island::Region::INFILL, ObjectByExtruder::Island::Region::PERIMETERS }) {
                    for (const ExtrusionEntity *ee : (entity_type == ObjectByExtruder::Island::Region::INFILL) ? layerm->fills.entities : layerm->perimeters.entities) {
                        // extrusions represents infill or perimeter extrusions of a single island.
                        assert(dynamic_cast<const ExtrusionEntityCollection*>(ee) != nullptr);
                        const auto *extrusions = static_cast<const ExtrusionEntityCollection*>(ee);
                        if (extrusions->entities.empty()) // This shouldn't happen but first_point() would fail.
                            continue;

                        // This extrusion is part of certain Region, which tells us which extruder should be used for it:
                        int correct_extruder_id = layer_tools.extruder(*extrusions, region);

                        // Let's recover vector of extruder overrides:
                        const WipingExtrusions::ExtruderPerCopy *entity_overrides = nullptr;
                        if (! layer_tools.has_extruder(correct_extruder_id)) {
                            // this entity is not overridden, but its extruder is not in layer_tools - we'll print it
                            // by last extruder on this layer (could happen e.g. when a wiping object is taller than others - dontcare extruders are eradicated from layer_tools)
                            correct_extruder_id = layer_tools.extruders.back();
                        }
                        printing_extruders.clear();
                        if (is_anything_overridden) {
                            entity_overrides = const_cast<LayerTools&>(layer_tools).wiping_extrusions().get_extruder_overrides(extrusions, layer_to_print.original_object, correct_extruder_id, layer_to_print.object()->instances().size());
                            if (entity_overrides == nullptr) {
                                printing_extruders.emplace_back(correct_extruder_id);
                            } else {
                                printing_extruders.reserve(entity_overrides->size());
                                for (int extruder : *entity_overrides)
                                    printing_extruders.emplace_back(extruder >= 0 ?
                                        // at least one copy is overridden to use this extruder
                                        extruder :
                                        // at least one copy would normally be printed with this extruder (see get_extruder_overrides function for explanation)
                                        static_cast<unsigned int>(- extruder - 1));
                                Slic3r::sort_remove_duplicates(printing_extruders);
                            }
                        } else
                            printing_extruders.emplace_back(correct_extruder_id);

                        // Now we must add this extrusion into the by_extruder map, once for each extruder that will print it:
                        for (unsigned int extruder : printing_extruders)
                        {
                            std::vector<ObjectByExtruder::Island> &islands = object_islands_by_extruder(
                                by_extruder,
                                extruder,
                                &layer_to_print - layers.data(),
                                layers.size(), n_slices+1);
                            for (size_t i = 0; i <= n_slices; ++ i) {
                                bool   last = i == n_slices;
                                size_t island_idx = last ? n_slices : slices_test_order[i];
                                if (// extrusions->first_point does not fit inside any slice
                                    last ||
                                    // extrusions->first_point fits inside ith slice
                                    point_inside_surface(island_idx, extrusions->first_point())) {
                                    if (islands[island_idx].by_region.empty())
                                        islands[island_idx].by_region.assign(print.num_print_regions(), ObjectByExtruder::Island::Region());
                                    islands[island_idx].by_region[region.print_region_id()].append(entity_type, extrusions, entity_overrides);
                                    break;
                                }
                            }
                        }
                    }
                }
            } // for regions
        }
    } // for objects

    std::map<unsigned int, std::vector<InstanceToPrint>> filament_to_print_instances;
    {
        for (unsigned int filament_id : layer_tools.extruders) {
            auto objects_by_extruder_it = by_extruder.find(filament_id);
            if (objects_by_extruder_it == by_extruder.end()) continue;

            int   plate_idx = print.get_plate_index();
            Point wt_pos(print.config().wipe_tower_x.get_at(plate_idx), print.config().wipe_tower_y.get_at(plate_idx));

            std::vector<GCode::ObjectByExtruder> &objects_by_extruder = objects_by_extruder_it->second;
            std::vector<const PrintObject *>      print_objects;
            for (int obj_idx = 0; obj_idx < objects_by_extruder.size(); obj_idx++) {
                auto &object_by_extruder = objects_by_extruder[obj_idx];
                if (object_by_extruder.islands.empty() && (object_by_extruder.support == nullptr || object_by_extruder.support->empty())) continue;

                print_objects.push_back(print.get_object(obj_idx));
            }

            std::vector<const PrintInstance *> new_ordering = chain_print_object_instances(print_objects, &wt_pos);
            std::reverse(new_ordering.begin(), new_ordering.end());

            if (print.config().print_sequence == PrintSequence::ByObject) {
                filament_to_print_instances[filament_id] = sort_print_object_instances(objects_by_extruder_it->second, layers, ordering, single_object_instance_idx);
            } else {
                filament_to_print_instances[filament_id] = sort_print_object_instances(objects_by_extruder_it->second, layers, &new_ordering, single_object_instance_idx);
            }
        }
    }

    std::set<size_t> layer_object_label_ids;
    for (auto iter = filament_to_print_instances.begin(); iter != filament_to_print_instances.end(); ++iter) {
        for (const InstanceToPrint &instance : iter->second) {
            layer_object_label_ids.insert(instance.label_object_id);
        }
    }

    auto insert_timelapse_gcode = [this, print_z, &print, &most_used_extruder, &layer_object_label_ids,&printed_objects = std::as_const(m_printed_objects)]() -> std::string {
        PosPickCtx ctx;
        ctx.curr_pos = { (coord_t)(scale_(m_writer.get_position().x())),(coord_t)(scale_(m_writer.get_position().y())) };
        ctx.curr_layer = this->layer();
        ctx.curr_extruder_id = m_writer.filament()->extruder_id();
        ctx.picture_extruder_id = most_used_extruder;
        if (m_config.print_sequence == PrintSequence::ByObject)
            ctx.printed_objects = printed_objects;

        auto timelapse_pos=m_timelapse_pos_picker.pick_pos(ctx);

        std::string timelapse_gcode;
        if (!print.config().time_lapse_gcode.value.empty()) {
            DynamicConfig config;
            config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
            config.set_key_value("layer_z", new ConfigOptionFloat(print_z));
            config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
            config.set_key_value("most_used_physical_extruder_id", new ConfigOptionInt(m_config.physical_extruder_map.get_at(most_used_extruder)));
            config.set_key_value("curr_physical_extruder_id", new ConfigOptionInt(m_config.physical_extruder_map.get_at(ctx.curr_extruder_id)));
            config.set_key_value("timelapse_pos_x", new ConfigOptionInt((int)timelapse_pos.x()));
            config.set_key_value("timelapse_pos_y", new ConfigOptionInt((int)timelapse_pos.y()));
            config.set_key_value("has_timelapse_safe_pos", new ConfigOptionBool(timelapse_pos != DefaultTimelapsePos));
            timelapse_gcode = this->placeholder_parser_process("timelapse_gcode", print.config().time_lapse_gcode.value, m_writer.filament()->id(), &config) + "\n";
        }

        if (!timelapse_gcode.empty()) {
            m_writer.set_current_position_clear(false);

            double temp_z_after_tool_change;
            if (GCodeProcessor::get_last_z_from_gcode(timelapse_gcode, temp_z_after_tool_change)) {
                Vec3d pos = m_writer.get_position();
                pos(2)    = temp_z_after_tool_change;
                m_writer.set_position(pos);
            }
        }

        // (layer_object_label_ids.size() < 64) this restriction comes from _encode_label_ids_to_base64()
        if (is_BBL_Printer() &&
            (print.num_object_instances() <= g_max_label_object) && // Don't support too many objects on one plate
            (print.num_object_instances() > 1) &&                 // Don't support skipping single object
            (layer_object_label_ids.size() > 0) &&
            (print.calib_params().mode == CalibMode::Calib_None)) {
            std::ostringstream oss;
            for (auto it = layer_object_label_ids.begin(); it != layer_object_label_ids.end(); ++it) {
                if (it != layer_object_label_ids.begin()) oss << ",";
                oss << *it;
            }

            std::string start_str = std::string("; object ids of layer ") + std::to_string(m_layer_index + 1) + (" start: ") + oss.str() + "\n";
            start_str += "M624 " + _encode_label_ids_to_base64(std::vector<size_t>(layer_object_label_ids.begin(), layer_object_label_ids.end())) + "\n";

            std::string end_str = std::string("; object ids of this layer") + std::to_string(m_layer_index + 1) + (" end: ") + oss.str() + "\n";
            end_str   += "M625\n";

            timelapse_gcode = start_str + timelapse_gcode + end_str;
        }

        return timelapse_gcode;
    };

    if (!need_insert_timelapse_gcode_for_traditional  && is_BBL_Printer()) { // Equivalent to the timelapse gcode placed in layer_change_gcode
        if (FILAMENT_CONFIG(retract_when_changing_layer)) {
            gcode += this->retract(false, false, auto_lift_type, true);
        }
        gcode += insert_timelapse_gcode();
    }

    if (m_wipe_tower)
        m_wipe_tower->set_is_first_print(true);

    auto insert_wrapping_detection_gcode = [this, &print, &print_z, &most_used_extruder]() -> std::string {
        std::string wrapping_gcode;
        if (print.config().enable_wrapping_detection && !print.config().wrapping_detection_gcode.value.empty()) {
            DynamicConfig config;
            config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
            config.set_key_value("layer_z", new ConfigOptionFloat(print_z));
            config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
            config.set_key_value("most_used_physical_extruder_id", new ConfigOptionInt(m_config.physical_extruder_map.get_at(most_used_extruder)));
            config.set_key_value("curr_physical_extruder_id", new ConfigOptionInt(m_config.physical_extruder_map.get_at(m_writer.filament()->extruder_id())));
            wrapping_gcode = this->placeholder_parser_process("wrapping_detection_gcode", print.config().wrapping_detection_gcode.value, m_writer.filament()->id(), &config) +"\n";
        }
        m_writer.set_current_position_clear(false);

        double temp_z_after_tool_change;
        if (GCodeProcessor::get_last_z_from_gcode(wrapping_gcode, temp_z_after_tool_change)) {
            Vec3d pos = m_writer.get_position();
            pos(2)    = temp_z_after_tool_change;
            m_writer.set_position(pos);
        }
        return wrapping_gcode;
    };

    bool has_insert_wrapping_detection_gcode = false;

    // Extrude the skirt, brim, support, perimeters, infill ordered by the extruders.
    for (unsigned int extruder_id : layer_tools.extruders)
    {
        if (print.config().skirt_type == stCombined && !print.skirt().empty())
            gcode += generate_skirt(print, print.skirt(), Point(0, 0), layer.object()->config().skirt_start_angle, layer_tools, layer,
                                    extruder_id);

        if (print.config().print_sequence == PrintSequence::ByLayer && m_enable_exclude_object && print.config().support_object_skip_flush.value) {
            std::vector<size_t> filament_instances_id;
            for (InstanceToPrint &instance : filament_to_print_instances[extruder_id]) filament_instances_id.emplace_back(instance.label_object_id);
            m_filament_instances_code = _encode_label_ids_to_base64(filament_instances_id);
        }

        std::string gcode_toolchange;
        if (has_wipe_tower) {
            if (!m_wipe_tower->is_empty_wipe_tower_gcode(*this, extruder_id, extruder_id == layer_tools.extruders.back())) {
                if (need_insert_timelapse_gcode_for_traditional && !has_insert_timelapse_gcode) {
                    bool should_insert = true;
                    if (m_config.nozzle_diameter.values.size() == 2){
                        if (!writer().filament() || get_extruder_id(writer().filament()->id()) != most_used_extruder) {
                            should_insert = false;
                        }
                    }

                    if (should_insert) {
                        gcode += this->retract(false, false, auto_lift_type, true);
                        m_writer.add_object_change_labels(gcode);

                        gcode += insert_timelapse_gcode();
                        has_insert_timelapse_gcode = true;
                    }
                }

                if (print.config().enable_wrapping_detection && !has_insert_wrapping_detection_gcode) {
                    gcode += this->retract(false, false, auto_lift_type, true);
                    gcode += insert_wrapping_detection_gcode();
                    has_insert_wrapping_detection_gcode = true;
                }
                gcode_toolchange = m_wipe_tower->tool_change(*this, extruder_id, extruder_id == layer_tools.extruders.back());
            }
        } else {
            if (need_insert_timelapse_gcode_for_traditional &&
                !has_insert_timelapse_gcode &&
                m_writer.need_toolchange(extruder_id) &&
                m_config.nozzle_diameter.values.size() == 2 &&
                writer().filament() &&
                (get_extruder_id(writer().filament()->id()) == most_used_extruder)) {
                gcode += this->retract(false, false, auto_lift_type, true);
                m_writer.add_object_change_labels(gcode);

                gcode += insert_timelapse_gcode();
                has_insert_timelapse_gcode = true;
            }

            if (print.config().enable_wrapping_detection && !has_insert_wrapping_detection_gcode) {
                gcode += this->retract(false, false, auto_lift_type, true);
                gcode += insert_wrapping_detection_gcode();
                has_insert_wrapping_detection_gcode = true;
            }

            gcode_toolchange = this->set_extruder(extruder_id, print_z);
        }

        if (!gcode_toolchange.empty()) {
            // Disable vase mode for layers that has toolchange
            result.spiral_vase_enable = false;
        }
        
        gcode += std::move(gcode_toolchange);

        // let analyzer tag generator aware of a role type change
        if (layer_tools.has_wipe_tower && m_wipe_tower)
            m_last_processor_extrusion_role = erWipeTower;

        std::vector<InstanceToPrint> &instances_to_print = filament_to_print_instances[extruder_id];

        // BBS
        if (print.config().skirt_type == stPerObject &&
            print.config().print_sequence == PrintSequence::ByObject &&
            !layer.object()->object_skirt().empty() &&
            ((layer.id() < print.config().skirt_height || print.config().draft_shield == DraftShield::dsEnabled))
           )
        {
            for (InstanceToPrint& instance_to_print : instances_to_print) {
                
                if (instance_to_print.print_object.object_skirt().empty())
                    continue;
                
                if (this->m_objSupportsWithBrim.find(instance_to_print.print_object.id()) != this->m_objSupportsWithBrim.end() &&
                    print.m_supportBrimMap.at(instance_to_print.print_object.id()).entities.size() > 0)
                    continue;

                if (this->m_objsWithBrim.find(instance_to_print.print_object.id()) != this->m_objsWithBrim.end() &&
                    print.m_brimMap.at(instance_to_print.print_object.id()).entities.size() > 0)
                    continue;
                if (first_layer)
                    m_skirt_done.clear();

                if (layer.id() == 1 && m_skirt_done.size() > 1)
                    m_skirt_done.erase(m_skirt_done.begin()+1,m_skirt_done.end());

                const Point& offset = instance_to_print.print_object.instances()[instance_to_print.instance_id].shift;
                gcode += generate_skirt(print, instance_to_print.print_object.object_skirt(), offset, instance_to_print.print_object.config().skirt_start_angle, layer_tools, layer, extruder_id);
            }
        }

        // We are almost ready to print. However, we must go through all the objects twice to print the the overridden extrusions first (infill/perimeter wiping feature):
        std::vector<ObjectByExtruder::Island::Region> by_region_per_copy_cache;
        for (int print_wipe_extrusions = is_anything_overridden; print_wipe_extrusions>=0; --print_wipe_extrusions) {
            if (is_anything_overridden && print_wipe_extrusions == 0)
                gcode+="; PURGING FINISHED\n";

            for (InstanceToPrint &instance_to_print : instances_to_print) {
                if (print.config().skirt_type == stPerObject && 
                    !instance_to_print.print_object.object_skirt().empty() &&
                    print.config().print_sequence == PrintSequence::ByLayer
                    &&
                    (layer.id() < print.config().skirt_height || print.config().draft_shield == DraftShield::dsEnabled))
                {
                    if (first_layer)
                        m_skirt_done.clear();
                    const Point& offset = instance_to_print.print_object.instances()[instance_to_print.instance_id].shift;
                    gcode += generate_skirt(print, instance_to_print.print_object.object_skirt(), offset, instance_to_print.print_object.config().skirt_start_angle, layer_tools, layer, extruder_id);
                    if (instances_to_print.size() > 1 && &instance_to_print != &*(instances_to_print.end() - 1))
                        m_skirt_done.pop_back();
                }
                
                const auto& inst = instance_to_print.print_object.instances()[instance_to_print.instance_id];
                const LayerToPrint &layer_to_print = layers[instance_to_print.layer_id];
                // To control print speed of the 1st object layer printed over raft interface.
                bool object_layer_over_raft = layer_to_print.object_layer && layer_to_print.object_layer->id() > 0 &&
                    instance_to_print.print_object.slicing_parameters().raft_layers() == layer_to_print.object_layer->id();
                m_config.apply(print.default_region_config());
                m_config.apply(instance_to_print.print_object.config(), true);
                m_layer = layer_to_print.layer();
                m_object_layer_over_raft = object_layer_over_raft;
                if (m_config.reduce_crossing_wall)
                    m_avoid_crossing_perimeters.init_layer(*m_layer);

                if (this->config().gcode_label_objects) {
                    gcode += std::string("; printing object ") + instance_to_print.print_object.model_object()->name +
                             " id:" + std::to_string(instance_to_print.print_object.get_id()) + " copy " +
                             std::to_string(inst.id) + "\n";
                }
                // exclude objects
                if (m_enable_exclude_object) {
                    if (is_BBL_Printer()) {
                        m_writer.set_object_start_str(
                            std::string("; start printing object, unique label id: ") +
                            std::to_string(instance_to_print.label_object_id) + "\n" + "M624 " +
                            _encode_label_ids_to_base64({instance_to_print.label_object_id}) + "\n");
                    } else {
                        const auto gflavor = print.config().gcode_flavor.value;
                        if (gflavor == gcfKlipper) {
                            m_writer.set_object_start_str(std::string("EXCLUDE_OBJECT_START NAME=") +
                                                          get_instance_name(&instance_to_print.print_object, inst.id) + "\n");
                        }
                        else if (gflavor == gcfMarlinLegacy || gflavor == gcfMarlinFirmware || gflavor == gcfRepRapFirmware) {
                            std::string str = std::string("M486 S") + std::to_string(inst.unique_id) + "\n";
                            m_writer.set_object_start_str(str);
                        }
                    }
                }

                // Orca(#7946): set current obj regardless of the `enable_overhang_speed` value, because
                // `enable_overhang_speed` is a PrintRegionConfig and here we don't have a region yet.
                // And no side effect doing this even if `enable_overhang_speed` is off, so don't bother
                // checking anything here.
                m_extrusion_quality_estimator.set_current_object(&instance_to_print.print_object);

                // When starting a new object, use the external motion planner for the first travel move.
                const Point &offset = instance_to_print.print_object.instances()[instance_to_print.instance_id].shift;
                std::pair<const PrintObject*, Point> this_object_copy(&instance_to_print.print_object, offset);
                if (m_last_obj_copy != this_object_copy)
                    m_avoid_crossing_perimeters.use_external_mp_once();
                m_last_obj_copy = this_object_copy;
                this->set_origin(unscale(offset));
                if (instance_to_print.object_by_extruder.support != nullptr) {
                    m_layer = layers[instance_to_print.layer_id].support_layer;
                    m_object_layer_over_raft = false;

                    //BBS: print supports' brims first
                    if (this->m_objSupportsWithBrim.find(instance_to_print.print_object.id()) != this->m_objSupportsWithBrim.end() && !print_wipe_extrusions) {
                        this->set_origin(0., 0.);
                        m_avoid_crossing_perimeters.use_external_mp();
                        for (const ExtrusionEntity* ee : print.m_supportBrimMap.at(instance_to_print.print_object.id()).entities) {
                            gcode += this->extrude_entity(*ee, "brim", m_config.support_speed.value);
                        }
                        m_avoid_crossing_perimeters.use_external_mp(false);
                        // Allow a straight travel move to the first object point.
                        m_avoid_crossing_perimeters.disable_once();
                        this->m_objSupportsWithBrim.erase(instance_to_print.print_object.id());
                    }
                    // When starting a new object, use the external motion planner for the first travel move.
                    const Point& offset = instance_to_print.print_object.instances()[instance_to_print.instance_id].shift;
                    std::pair<const PrintObject*, Point> this_object_copy(&instance_to_print.print_object, offset);
                    if (m_last_obj_copy != this_object_copy)
                        m_avoid_crossing_perimeters.use_external_mp_once();
                    m_last_obj_copy = this_object_copy;
                    this->set_origin(unscale(offset));
                    ExtrusionEntityCollection support_eec;

                    // BBS
                    WipingExtrusions& wiping_extrusions = const_cast<LayerTools&>(layer_tools).wiping_extrusions();
                    bool support_overridden = wiping_extrusions.is_support_overridden(layer_to_print.original_object);
                    bool support_intf_overridden = wiping_extrusions.is_support_interface_overridden(layer_to_print.original_object);

                    ExtrusionRole support_extrusion_role = instance_to_print.object_by_extruder.support_extrusion_role;
                    bool is_overridden = support_extrusion_role == erSupportMaterialInterface ? support_intf_overridden : support_overridden;
                    if (is_overridden == (print_wipe_extrusions != 0)) {
                        gcode += this->extrude_support(
                            // support_extrusion_role is erSupportMaterial, erSupportTransition, erSupportMaterialInterface or erMixed for all extrusion paths.
                            *instance_to_print.object_by_extruder.support, support_extrusion_role);

                        // Make sure ironing is the last
                        if (support_extrusion_role == erMixed || support_extrusion_role == erSupportMaterialInterface) {
                            gcode += this->extrude_support(*instance_to_print.object_by_extruder.support, erIroning);
                        }
                    }

                    m_layer = layer_to_print.layer();
                    m_object_layer_over_raft = object_layer_over_raft;
                }
                //FIXME order islands?
                // Sequential tool path ordering of multiple parts within the same object, aka. perimeter tracking (#5511)
                for (ObjectByExtruder::Island &island : instance_to_print.object_by_extruder.islands) {
                    const auto& by_region_specific = is_anything_overridden ? island.by_region_per_copy(by_region_per_copy_cache, static_cast<unsigned int>(instance_to_print.instance_id), extruder_id, print_wipe_extrusions != 0) : island.by_region;
                    //BBS: add brim by obj by extruder
                    if (first_layer) {
                        if (this->m_objsWithBrim.find(instance_to_print.print_object.id()) != this->m_objsWithBrim.end() && !print_wipe_extrusions) {
                            this->set_origin(0., 0.);
                            m_avoid_crossing_perimeters.use_external_mp();
                            for (const ExtrusionEntity* ee : print.m_brimMap.at(instance_to_print.print_object.id()).entities) {
                                gcode += this->extrude_entity(*ee, "brim", m_config.support_speed.value);
                            }
                            m_avoid_crossing_perimeters.use_external_mp(false);
                            // Allow a straight travel move to the first object point.
                            m_avoid_crossing_perimeters.disable_once();
                            this->m_objsWithBrim.erase(instance_to_print.print_object.id());
                        }
                    }
                    // When starting a new object, use the external motion planner for the first travel move.
                    const Point& offset = instance_to_print.print_object.instances()[instance_to_print.instance_id].shift;
                    std::pair<const PrintObject*, Point> this_object_copy(&instance_to_print.print_object, offset);
                    if (m_last_obj_copy != this_object_copy)
                        m_avoid_crossing_perimeters.use_external_mp_once();
                    m_last_obj_copy = this_object_copy;
                    this->set_origin(unscale(offset));
                    //FIXME the following code prints regions in the order they are defined, the path is not optimized in any way.

                    auto has_infill = [](const std::vector<ObjectByExtruder::Island::Region> &by_region) {
                        for (auto region : by_region) {
                            if (!region.infills.empty())
                                return true;
                        }
                        return false;
                    };
                    {
                        // Print perimeters of regions that has is_infill_first == false
                        gcode += this->extrude_perimeters(print, by_region_specific, first_layer, false);
                        if (!has_wipe_tower && need_insert_timelapse_gcode_for_traditional && printer_structure == PrinterStructure::psI3
                            && !has_insert_timelapse_gcode && has_infill(by_region_specific)) {
                            gcode += this->retract(false, false, auto_lift_type, true);

                            gcode += insert_timelapse_gcode();
                            has_insert_timelapse_gcode = true;
                        }
                        // Then print infill
                        gcode += this->extrude_infill(print, by_region_specific, false);
                        // Then print perimeters of regions that has is_infill_first == true
                        gcode += this->extrude_perimeters(print, by_region_specific, first_layer, true);
                    }
                    // ironing
                    gcode += this->extrude_infill(print,by_region_specific, true);
                }

                if (this->config().gcode_label_objects) {
                    gcode += std::string("; stop printing object ") +
                             instance_to_print.print_object.model_object()->name +
                             " id:" + std::to_string(instance_to_print.print_object.get_id()) + " copy " +
                             std::to_string(inst.id) + "\n";
                }
                // exclude objects
                // Don't set m_gcode_label_objects_end if you don't had to write the m_gcode_label_objects_start.
                if (!m_writer.is_object_start_str_empty()) {
                    m_writer.set_object_start_str("");
                } else if (m_enable_exclude_object) {
                    if (is_BBL_Printer()) {
                        m_writer.set_object_end_str(std::string("; stop printing object, unique label id: ") +
                                                    std::to_string(instance_to_print.label_object_id) + "\n" +
                                                    "M625\n");
                    } else {
                        const auto gflavor = print.config().gcode_flavor.value;
                        if (gflavor == gcfKlipper) {
                            m_writer.set_object_end_str(std::string("EXCLUDE_OBJECT_END NAME=") +
                                                        get_instance_name(&instance_to_print.print_object, inst.id) + "\n");
                        } else if (gflavor == gcfMarlinLegacy || gflavor == gcfMarlinFirmware || gflavor == gcfRepRapFirmware) {
                            m_writer.set_object_end_str(std::string("M486 S-1\n"));
                        }
                    }
                }
            }
        }
    }
    if (first_layer) {
        for (auto iter = by_extruder.begin(); iter != by_extruder.end(); ++iter) {
            if (!iter->second.empty())
                m_initial_layer_extruders.insert(iter->first);
        }
    }

#if 0
    // Apply spiral vase post-processing if this layer contains suitable geometry
    // (we must feed all the G-code into the post-processor, including the first
    // bottom non-spiral layers otherwise it will mess with positions)
    // we apply spiral vase at this stage because it requires a full layer.
    // Just a reminder: A spiral vase mode is allowed for a single object per layer, single material print only.
    if (m_spiral_vase)
        gcode = m_spiral_vase->process_layer(std::move(gcode));

    // Apply cooling logic; this may alter speeds.
    if (m_cooling_buffer)
        gcode = m_cooling_buffer->process_layer(std::move(gcode), layer.id(),
            // Flush the cooling buffer at each object layer or possibly at the last layer, even if it contains just supports (This should not happen).
            object_layer || last_layer);

    file.write(gcode);
#endif

    BOOST_LOG_TRIVIAL(trace) << "Exported layer " << layer.id() << " print_z " << print_z <<
    log_memory_info();

    if (need_insert_timelapse_gcode_for_traditional && !has_insert_timelapse_gcode) {
        // The traditional model of thin-walled object will have flaws for I3
        if (m_support_traditional_timelapse
            && printer_structure == PrinterStructure::psI3
            && m_config.timelapse_type.value == TimelapseType::tlTraditional)
            m_support_traditional_timelapse = false;

        // The traditional model will have flaws for multi_extruder when switching extruder
        if (m_config.nozzle_diameter.values.size() == 2
            && m_support_traditional_timelapse
            && m_config.timelapse_type.value == TimelapseType::tlTraditional
            && (writer().filament() && get_extruder_id(writer().filament()->id()) != most_used_extruder)) {
            m_support_traditional_timelapse = false;
        }
        if (FILAMENT_CONFIG(retract_when_changing_layer)) {
            gcode += this->retract(false, false, auto_lift_type, true);
        }
        m_writer.add_object_change_labels(gcode);

        gcode += insert_timelapse_gcode();
    }

    result.gcode = std::move(gcode);
    result.cooling_buffer_flush = object_layer || raft_layer || last_layer;
    return result;
}

void GCode::apply_print_config(const PrintConfig &print_config)
{
    m_writer.apply_print_config(print_config);
    m_config.apply(print_config);
    m_scaled_resolution = scaled<double>(print_config.resolution.value);
    m_enable_exclude_object = m_config.exclude_object;

#if ORCA_CHECK_GCODE_PLACEHOLDERS
    // If the gcode value is empty, set a value so that the check code within the parser is run
    for (auto opt : std::initializer_list<ConfigOptionString*>{
             &m_config.machine_start_gcode,
             &m_config.machine_end_gcode,
             &m_config.before_layer_change_gcode,
             &m_config.layer_change_gcode,
             &m_config.time_lapse_gcode,
             &m_config.change_filament_gcode,
             &m_config.change_extrusion_role_gcode,
             &m_config.printing_by_object_gcode,
             &m_config.machine_pause_gcode,
             &m_config.template_custom_gcode,
         }) {
        if (opt->empty())
            opt->set(new ConfigOptionString(";VALUE FOR TESTING"));
    }
    for (auto opt : std::initializer_list<ConfigOptionStrings*>{
             &m_config.filament_start_gcode,
             &m_config.filament_end_gcode
         }) {
        if (opt->empty())
            for (int i = 0; i < opt->size(); ++i)
                opt->set_at(new ConfigOptionString(";VALUE FOR TESTING"), i, 0);
    }
#endif
}

void GCode::append_full_config(const Print &print, std::string &str)
{
    DynamicPrintConfig cfg = print.full_print_config();
    { // correct the flush_volumes_matrix with flush_multiplier values
        std::vector<double> temp_cfg_flush_multiplier = cfg.option<ConfigOptionFloats>("flush_multiplier")->values;
        std::vector<double> temp_flush_volumes_matrix = cfg.option<ConfigOptionFloats>("flush_volumes_matrix")->values;
        auto                temp_filament_color       = cfg.option<ConfigOptionStrings>("filament_colour")->values;
        size_t              heads_count_tmp           = temp_cfg_flush_multiplier.size(),
               matrix_value_count                     = temp_flush_volumes_matrix.size() / temp_cfg_flush_multiplier.size(),
               filament_count_tmp                     = temp_filament_color.size();
        if (filament_count_tmp * filament_count_tmp * heads_count_tmp == temp_flush_volumes_matrix.size()) {
            for (size_t idx = 0; idx < heads_count_tmp; ++idx) {
                double temp_cfg_flush_multiplier_idx = temp_cfg_flush_multiplier[idx];
                size_t temp_begin_t = idx * matrix_value_count, temp_end_t = (idx + 1) * matrix_value_count;
                std::transform(temp_flush_volumes_matrix.begin() + temp_begin_t, temp_flush_volumes_matrix.begin() + temp_end_t,
                               temp_flush_volumes_matrix.begin() + temp_begin_t,
                               [temp_cfg_flush_multiplier_idx](double inputx) { return inputx * temp_cfg_flush_multiplier_idx; });
            }
            cfg.option<ConfigOptionFloats>("flush_volumes_matrix")->values = temp_flush_volumes_matrix;
        } else if (filament_count_tmp == 1) {
        } // Not applicable to flush matrix situations
        else { // flush_volumes_matrix value count error?
            throw Slic3r::SlicingError(_(L("Flush volumes matrix do not match to the correct size!")));
        }
    }
    // Sorted list of config keys, which shall not be stored into the G-code. Initializer list.
    static const std::set<std::string_view> banned_keys( {
        "compatible_printers"sv,
        "compatible_prints"sv,
        "print_host"sv,
        "print_host_webui"sv,
        "printhost_apikey"sv,
        "printhost_cafile"sv,
        "printhost_user"sv,
        "printhost_password"sv,
        "printhost_port"sv
    });
    auto is_banned = [](const std::string &key) {
        return banned_keys.find(key) != banned_keys.end();
    };
    std::ostringstream ss;
    for (const std::string& key : cfg.keys()) {
        if (!is_banned(key) && !cfg.option(key)->is_nil()) {
            if (key == "wipe_tower_x" || key == "wipe_tower_y") {
                ss << std::fixed << std::setprecision(3) << "; " << key << " = " << dynamic_cast<const ConfigOptionFloats*>(cfg.option(key))->get_at(print.get_plate_index()) << "\n";
            }
            if(key == "extruder_colour")
                ss << "; " << key << " = " << cfg.opt_serialize("filament_colour") << "\n";
            else
                ss << "; " << key << " = " << cfg.opt_serialize(key) << "\n";
        }
    }
    str += ss.str();
}

void GCode::set_extruders(const std::vector<unsigned int> &extruder_ids)
{
    m_writer.set_extruders(extruder_ids);

    // enable wipe path generation if any extruder has wipe enabled
    m_wipe.enable = false;
    for (auto id : extruder_ids)
        if (m_config.wipe.get_at(id)) {
            m_wipe.enable = true;
            break;
        }
}

void GCode::set_origin(const Vec2d &pointf)
{
    // if origin increases (goes towards right), last_pos decreases because it goes towards left
    const Point translate(
        scale_(m_origin(0) - pointf(0)),
        scale_(m_origin(1) - pointf(1))
    );
    m_last_pos += translate;
    m_wipe.path.translate(translate);
    m_origin = pointf;
}

std::string GCode::preamble()
{
    std::string gcode = m_writer.preamble();

    /*  Perform a *silent* move to z_offset: we need this to initialize the Z
        position of our writer object so that any initial lift taking place
        before the first layer change will raise the extruder from the correct
        initial Z instead of 0.  */
    m_writer.travel_to_z(m_config.z_offset.value);

    return gcode;
}

// called by GCode::process_layer()
std::string GCode::change_layer(coordf_t print_z)
{
    std::string gcode;
    if (m_layer_count > 0)
        // Increment a progress bar indicator.
        gcode += m_writer.update_progress(++ m_layer_index, m_layer_count);
    //BBS
    coordf_t z = print_z + m_config.z_offset.value;  // in unscaled coordinates
    if (FILAMENT_CONFIG(retract_when_changing_layer) && m_writer.will_move_z(z)) {
        LiftType lift_type = this->to_lift_type(ZHopType(FILAMENT_CONFIG(z_hop_types)));
        //BBS: force to use SpiralLift when change layer if lift type is auto
        gcode += this->retract(false, false, ZHopType(FILAMENT_CONFIG(z_hop_types)) == ZHopType::zhtAuto ? LiftType::SpiralLift : lift_type);
    }

    m_writer.add_object_change_labels(gcode);

    if (m_spiral_vase) {
        //BBS: force to normal lift immediately in spiral vase mode
        std::ostringstream comment;
        comment << "move to next layer (" << m_layer_index << ")";
        gcode += m_writer.travel_to_z(z, comment.str());
    }

    m_need_change_layer_lift_z = true;

    m_nominal_z = z;
    m_writer.get_position().z() = z;

    // forget last wiping path as wiping after raising Z is pointless
    // BBS. Dont forget wiping path to reduce stringing.
    //m_wipe.reset_path();

    return gcode;
}



static std::unique_ptr<EdgeGrid::Grid> calculate_layer_edge_grid(const Layer& layer)
{
    auto out = make_unique<EdgeGrid::Grid>();

    // Create the distance field for a layer below.
    const coord_t distance_field_resolution = coord_t(scale_(1.) + 0.5);
    out->create(layer.lslices, distance_field_resolution);
    out->calculate_sdf();
#if 0
        {
            static int iRun = 0;
            BoundingBox bbox = (*lower_layer_edge_grid)->bbox();
            bbox.min(0) -= scale_(5.f);
            bbox.min(1) -= scale_(5.f);
            bbox.max(0) += scale_(5.f);
            bbox.max(1) += scale_(5.f);
            EdgeGrid::save_png(*(*lower_layer_edge_grid), bbox, scale_(0.1f), debug_out_path("GCode_extrude_loop_edge_grid-%d.png", iRun++));
        }
#endif
    return out;
}

std::string GCode::extrude_loop(ExtrusionLoop loop, std::string description, double speed, const ExtrusionEntitiesPtr& region_perimeters, const Point* start_point)
{
    
    // get a copy; don't modify the orientation of the original loop object otherwise
    // next copies (if any) would not detect the correct orientation

    bool is_hole = (loop.loop_role() & elrHole) == elrHole;

    if (m_config.spiral_mode && !is_hole) {
        // if spiral vase, we have to ensure that all contour are in the same orientation.
        loop.make_counter_clockwise();
    }
    //if (loop.loop_role() == elrSkirt && (this->m_layer->id() % 2 == 1))
    //    loop.reverse();

    // find the point of the loop that is closest to the current extruder position
    // or randomize if requested;
    // or, if `start_point` is specified, start the loop at point closest to it
    Point last_pos = start_point ? *start_point : this->last_pos();
    float seam_overhang = std::numeric_limits<float>::lowest();
    if (!m_config.spiral_mode && description == "perimeter") {
        assert(m_layer != nullptr);
        m_seam_placer.place_seam(m_layer, loop, last_pos, seam_overhang);
    } else
        loop.split_at(last_pos, false);

    const auto seam_scarf_type = m_config.seam_slope_type.value;
    bool enable_seam_slope = ((seam_scarf_type == SeamScarfType::External && !is_hole) || seam_scarf_type == SeamScarfType::All) &&
        !m_config.spiral_mode &&
        (loop.role() == erExternalPerimeter || (loop.role() == erPerimeter && m_config.seam_slope_inner_walls)) &&
        layer_id() > 0;
    const auto nozzle_diameter = EXTRUDER_CONFIG(nozzle_diameter);
    if (enable_seam_slope && m_config.seam_slope_conditional.value) {
        enable_seam_slope = loop.is_smooth(m_config.scarf_angle_threshold.value * M_PI / 180., nozzle_diameter);
    }

    if (enable_seam_slope && m_config.seam_slope_conditional.value && m_config.scarf_overhang_threshold.value > 0.0f) {
        const auto _line_width = loop.role() == erExternalPerimeter ? m_config.outer_wall_line_width.get_abs_value(nozzle_diameter) :
                                                                      m_config.inner_wall_line_width.get_abs_value(nozzle_diameter);
        enable_seam_slope      = seam_overhang < m_config.scarf_overhang_threshold.value * 0.01f * _line_width;
    }

    // clip the path to avoid the extruder to get exactly on the first point of the loop;
    // if polyline was shorter than the clipping distance we'd get a null polyline, so
    // we discard it in that case
    const double seam_gap = scale_(m_config.seam_gap.get_abs_value(nozzle_diameter));
    const double clip_length = m_enable_loop_clipping && !enable_seam_slope ? seam_gap : 0;

    // get paths
    ExtrusionPaths paths;
    loop.clip_end(clip_length, &paths);
    if (paths.empty()) return "";

    // SoftFever: check loop lenght for small perimeter. 
    double small_peri_speed = -1;
    if (speed == -1 && loop.length() <= SMALL_PERIMETER_LENGTH(m_config.small_perimeter_threshold.value)) {
        if(m_config.small_perimeter_speed == 0)
            small_peri_speed = m_config.outer_wall_speed * 0.5;
        else
            small_peri_speed = m_config.small_perimeter_speed.get_abs_value(m_config.outer_wall_speed);
    }

    // extrude along the path
    std::string gcode;
    
    // Orca:
    // Port of "wipe inside before extruding an external perimeter" feature from super slicer
    // If region perimeters size not greater than or equal to 2, then skip the wipe inside move as we will extrude in mid air
    // as no neighbouring perimeter exists. If an internal perimeter exists, we should find 2 perimeters touching the de-retraction point
    // 1 - the currently printed external perimeter and 2 - the neighbouring internal perimeter.
    if (m_config.wipe_before_external_loop.value && !paths.empty() && paths.front().size() > 1 && paths.back().size() > 1 && paths.front().role() == erExternalPerimeter && region_perimeters.size() > 1) {
        const bool is_full_loop_ccw = loop.polygon().is_counter_clockwise();
        bool is_hole_loop = (loop.loop_role() & ExtrusionLoopRole::elrHole) != 0; // loop.make_counter_clockwise();
        const double nozzle_diam = nozzle_diameter;

        // note: previous & next are inverted to extrude "in the opposite direction, and we are "rewinding"
        Point previous_point = paths.front().polyline.points[1];
        Point current_point = paths.front().polyline.points.front();
        Point next_point = paths.back().polyline.points.back();

        // can happen if seam_gap is null
        if (next_point == current_point) {
            next_point = paths.back().polyline.points[paths.back().polyline.points.size() - 2];
        }

        Point a = next_point;  // second point
        Point b = previous_point;  // second to last point
        if ((is_hole_loop ? !is_full_loop_ccw : is_full_loop_ccw)) {
            // swap points
            std::swap(a, b);
        }

        double angle = current_point.ccw_angle(a, b) / 3;

        // turn outwards if contour, turn inwwards if hole
        if (is_hole_loop ? !is_full_loop_ccw : is_full_loop_ccw) angle *= -1;

        Vec2d current_pos = current_point.cast<double>();
        Vec2d next_pos = next_point.cast<double>();
        Vec2d vec_dist = next_pos - current_pos;
        double vec_norm = vec_dist.norm();
        // Offset distance is the minimum between half the nozzle diameter or half the line width for the upcomming perimeter
        // This is to mimimize potential instances where the de-retraction is performed on top of a neighbouring
        // thin perimeter due to arachne reducing line width.
        coordf_t dist = std::min(scaled(nozzle_diam) * 0.5, scaled(paths.front().width) * 0.5);

        // FIXME Hiding the seams will not work nicely for very densely discretized contours!
        Point pt = (current_pos + vec_dist * (2 * dist / vec_norm)).cast<coord_t>();
        pt.rotate(angle, current_point);
        pt = (current_pos + vec_dist * (2 * dist / vec_norm)).cast<coord_t>();
        pt.rotate(angle, current_point);
        
        // Search region perimeters for lines that are touching the de-retraction point.
        // If an internal perimeter exists, we should find 2 perimeters touching the de-retraction point
        // 1: the currently printed external perimeter and 2: the neighbouring internal perimeter.
        int discoveredTouchingLines = 0;
        for (const ExtrusionEntity* ee : region_perimeters){
            auto potential_touching_line = ee->as_polyline();
            AABBTreeLines::LinesDistancer<Line> potential_touching_line_distancer{potential_touching_line.lines()};
            auto touching_line = potential_touching_line_distancer.all_lines_in_radius(pt, scale_(nozzle_diam));
            if(touching_line.size()){
                discoveredTouchingLines ++;
                if(discoveredTouchingLines > 1) break; // found 2 touching lines. End the search early.
            }
        }
        // found 2 perimeters touching the de-retraction point. Its safe to deretract as the point will be
        // inside the model
        if(discoveredTouchingLines > 1){
            // use extrude instead of travel_to_xy to trigger the unretract
            ExtrusionPath fake_path_wipe(Polyline{pt, current_point}, paths.front());
            fake_path_wipe.set_force_no_extrusion(true);
            fake_path_wipe.mm3_per_mm = 0;
            //fake_path_wipe.set_extrusion_role(erExternalPerimeter);
            gcode += extrude_path(fake_path_wipe, "move inwards before retraction/seam", speed);
        }
    }


    const auto speed_for_path = [&speed, &small_peri_speed](const ExtrusionPath& path) {
        // don't apply small perimeter setting for overhangs/bridges/non-perimeters
        const bool is_small_peri = is_perimeter(path.role()) && !is_bridge(path.role()) && small_peri_speed > 0;
        return is_small_peri ? small_peri_speed : speed;
    };

    
    //Orca: Adaptive PA: calculate average mm3_per_mm value over the length of the loop.
    //This is used for adaptive PA
    m_multi_flow_segment_path_pa_set = false; // always emit PA on the first path of the loop
    m_multi_flow_segment_path_average_mm3_per_mm = 0;
    double weighted_sum_mm3_per_mm = 0.0;
    double total_multipath_length = 0.0;
    for (const ExtrusionPath& path : paths) {
        if(!path.is_force_no_extrusion()){
            double path_length = unscale<double>(path.length()); //path length in mm
            weighted_sum_mm3_per_mm += path.mm3_per_mm * path_length;
            total_multipath_length += path_length;
        }
    }
    if (total_multipath_length > 0.0)
        m_multi_flow_segment_path_average_mm3_per_mm = weighted_sum_mm3_per_mm / total_multipath_length;
    // Orca: end of multipath average mm3_per_mm value calculation
    
    if (!enable_seam_slope) {
        for (ExtrusionPaths::iterator path = paths.begin(); path != paths.end(); ++path) {
            gcode += this->_extrude(*path, description, speed_for_path(*path));
            // Orca: Adaptive PA - dont adapt PA after the first pultipath extrusion is completed
            // as we have already set the PA value to the average flow over the totality of the path
            // in the first extrude move
            // TODO: testing is needed with slope seams and adaptive PA.
            m_multi_flow_segment_path_pa_set = true;
        }
    } else {
        // Create seam slope
        double start_slope_ratio;
        if (m_config.seam_slope_start_height.percent) {
            start_slope_ratio = m_config.seam_slope_start_height.value / 100.;
        } else {
            // Get the ratio against current layer height
            double h = paths.front().height;
            start_slope_ratio = m_config.seam_slope_start_height.value / h;
        }
        if (start_slope_ratio >= 1)
            start_slope_ratio = 0.99;

        double loop_length = 0.;
        for (const auto & path : paths) {
            loop_length += unscale_(path.length());
        }

        const bool   slope_entire_loop        = m_config.seam_slope_entire_loop;
        const double slope_min_length         = slope_entire_loop ? loop_length : std::min(m_config.seam_slope_min_length.value, loop_length);
        const int    slope_steps              = m_config.seam_slope_steps;
        const double slope_max_segment_length = scale_(slope_min_length / slope_steps);

        // Calculate the sloped loop
        ExtrusionLoopSloped new_loop(paths, seam_gap, slope_min_length, slope_max_segment_length, start_slope_ratio, loop.loop_role());
        new_loop.clip_slope(seam_gap);

        // Then extrude it
        for (const auto& p : new_loop.get_all_paths()) {
            gcode += this->_extrude(*p, description, speed_for_path(*p));
            // Orca: Adaptive PA - dont adapt PA after the first pultipath extrusion is completed
            // as we have already set the PA value to the average flow over the totality of the path
            // in the first extrude move
            m_multi_flow_segment_path_pa_set = true;
        }

        // Fix path for wipe
        if (!new_loop.ends.empty()) {
            paths.clear();
            // The start slope part is ignored as it overlaps with the end part
            paths.reserve(new_loop.paths.size() + new_loop.ends.size());
            paths.insert(paths.end(), new_loop.paths.begin(), new_loop.paths.end());
            paths.insert(paths.end(), new_loop.ends.begin(), new_loop.ends.end());
        }
    }

    // BBS
    if (m_wipe.enable && FILAMENT_CONFIG(wipe)) {
        m_wipe.path = Polyline();
        for (ExtrusionPath &path : paths) {
            //BBS: Don't need to save duplicated point into wipe path
            if (!m_wipe.path.empty() && !path.empty() &&
                m_wipe.path.last_point() == path.first_point())
                m_wipe.path.append(path.polyline.points.begin() + 1, path.polyline.points.end());
            else
                m_wipe.path.append(path.polyline);  // TODO: don't limit wipe to last path
        }
    }

    // make a little move inwards before leaving loop
    if (m_config.wipe_on_loops.value && paths.back().role() == erExternalPerimeter && m_layer != NULL && m_config.wall_loops.value > 1 && paths.front().size() >= 2 && paths.back().polyline.points.size() >= 3) {
        // detect angle between last and first segment
        // the side depends on the original winding order of the polygon (inwards for contours, outwards for holes)
        //FIXME improve the algorithm in case the loop is tiny.
        //FIXME improve the algorithm in case the loop is split into segments with a low number of points (see the Point b query).
        Point a = paths.front().polyline.points[1];  // second point
        Point b = *(paths.back().polyline.points.end()-3);       // second to last point
        if (is_hole == loop.is_counter_clockwise()) {
            // swap points
            Point c = a; a = b; b = c;
        }

        double angle = paths.front().first_point().ccw_angle(a, b) / 3;

        // turn inwards if contour, turn outwards if hole
        if (is_hole == loop.is_counter_clockwise()) angle *= -1;

        // create the destination point along the first segment and rotate it
        // we make sure we don't exceed the segment length because we don't know
        // the rotation of the second segment so we might cross the object boundary
        Vec2d  p1 = paths.front().polyline.points.front().cast<double>();
        Vec2d  p2 = paths.front().polyline.points[1].cast<double>();
        Vec2d  v  = p2 - p1;
        double nd = scale_(EXTRUDER_CONFIG(nozzle_diameter));
        double l2 = v.squaredNorm();
        // Shift by no more than a nozzle diameter.
        //FIXME Hiding the seams will not work nicely for very densely discretized contours!
        //BBS. shorten the travel distant before the wipe path
        double threshold = 0.2;
        Point  pt = (p1 + v * threshold).cast<coord_t>();
        if (nd * nd < l2)
            pt = (p1 + threshold * v * (nd / sqrt(l2))).cast<coord_t>();
        //Point pt = ((nd * nd >= l2) ? (p1+v*0.4): (p1 + 0.2 * v * (nd / sqrt(l2)))).cast<coord_t>();
        pt.rotate(angle, paths.front().polyline.points.front());
        // generate the travel move
        gcode += m_writer.extrude_to_xy(this->point_to_gcode(pt), 0,"move inwards before travel",true);
    }

    return gcode;
}

std::string GCode::extrude_multi_path(ExtrusionMultiPath multipath, std::string description, double speed)
{
    // extrude along the path
    std::string gcode;
    
    //Orca: calculate multipath average mm3_per_mm value over the length of the path.
    //This is used for adaptive PA
    m_multi_flow_segment_path_pa_set = false; // always emit PA on the first path of the multi-path
    m_multi_flow_segment_path_average_mm3_per_mm = 0;
    double weighted_sum_mm3_per_mm = 0.0;
    double total_multipath_length = 0.0;
    for (const ExtrusionPath& path : multipath.paths) {
        if(!path.is_force_no_extrusion()){
            double path_length = unscale<double>(path.length()); //path length in mm
            weighted_sum_mm3_per_mm += path.mm3_per_mm * path_length;
            total_multipath_length += path_length;
        }
    }
    if (total_multipath_length > 0.0)
        m_multi_flow_segment_path_average_mm3_per_mm = weighted_sum_mm3_per_mm / total_multipath_length;
    // Orca: end of multipath average mm3_per_mm value calculation
    
    for (ExtrusionPath path : multipath.paths){
        gcode += this->_extrude(path, description, speed);
        // Orca: Adaptive PA - dont adapt PA after the first pultipath extrusion is completed
        // as we have already set the PA value to the average flow over the totality of the path
        // in the first extrude move.
        m_multi_flow_segment_path_pa_set = true;
    }

    // BBS
    if (m_wipe.enable && FILAMENT_CONFIG(wipe)) {
        m_wipe.path = Polyline();
        for (ExtrusionPath &path : multipath.paths) {
            //BBS: Don't need to save duplicated point into wipe path
            if (!m_wipe.path.empty() && !path.empty() &&
                m_wipe.path.last_point() == path.first_point())
                m_wipe.path.append(path.polyline.points.begin() + 1, path.polyline.points.end());
            else
                m_wipe.path.append(path.polyline); // TODO: don't limit wipe to last path
        }
        m_wipe.path.reverse();
    }

    return gcode;
}

std::string GCode::extrude_entity(const ExtrusionEntity &entity, std::string description, double speed, const ExtrusionEntitiesPtr& region_perimeters)
{
    if (const ExtrusionPath* path = dynamic_cast<const ExtrusionPath*>(&entity))
        return this->extrude_path(*path, description, speed);
    else if (const ExtrusionMultiPath* multipath = dynamic_cast<const ExtrusionMultiPath*>(&entity))
        return this->extrude_multi_path(*multipath, description, speed);
    else if (const ExtrusionLoop* loop = dynamic_cast<const ExtrusionLoop*>(&entity))
        return this->extrude_loop(*loop, description, speed, region_perimeters);
    else
        throw Slic3r::InvalidArgument("Invalid argument supplied to extrude()");
    return "";
}

std::string GCode::extrude_path(ExtrusionPath path, std::string description, double speed)
{
    // Orca: Reset average multipath flow as this is a single line, single extrude volumetric speed path
    m_multi_flow_segment_path_pa_set = false;
    m_multi_flow_segment_path_average_mm3_per_mm = 0;
    //    description += ExtrusionEntity::role_to_string(path.role());
    std::string gcode = this->_extrude(path, description, speed);
    if (m_wipe.enable && FILAMENT_CONFIG(wipe)) {
        m_wipe.path = path.polyline;
        if (is_tree(this->config().support_type) && (path.role() == erSupportMaterial || path.role() == erSupportMaterialInterface || path.role() == erSupportTransition)) {
            if ((m_wipe.path.first_point() - m_wipe.path.last_point()).cast<double>().norm() > scale_(0.2)) {
                double min_dist = scale_(0.2);
                int    i        = 0;
                for (; i < path.polyline.points.size(); i++) {
                    double dist = (path.polyline.points[i] - path.last_point()).cast<double>().norm();
                    if (dist < min_dist) min_dist = dist;
                    if (min_dist < scale_(0.2) && dist > min_dist) break;
                }
                m_wipe.path = Polyline(Points(path.polyline.points.begin() + i - 1, path.polyline.points.end()));
            }
        } else
            m_wipe.path.reverse();
    }

    return gcode;
}

// Extrude perimeters: Decide where to put seams (hide or align seams).
std::string GCode::extrude_perimeters(const Print &print, const std::vector<ObjectByExtruder::Island::Region> &by_region, bool is_first_layer, bool is_infill_first)
{
    std::string gcode;
    for (const ObjectByExtruder::Island::Region &region : by_region)
        if (! region.perimeters.empty()) {
            m_config.apply(print.get_print_region(&region - &by_region.front()).config());
            // BBS: for first layer, we always print wall firstly to get better bed adhesive force
            // This behaviour is same with cura
            const bool should_print = is_first_layer ? !is_infill_first
                : (m_config.is_infill_first == is_infill_first);
            if (!should_print) continue;

            for (const ExtrusionEntity* ee : region.perimeters)
                gcode += this->extrude_entity(*ee, "perimeter", -1., region.perimeters);
        }
    return gcode;
}

// Chain the paths hierarchically by a greedy algorithm to minimize a travel distance.
std::string GCode::extrude_infill(const Print &print, const std::vector<ObjectByExtruder::Island::Region> &by_region, bool ironing)
{
    std::string 		 gcode;
    ExtrusionEntitiesPtr extrusions;
    const char*          extrusion_name = ironing ? "ironing" : "infill";
    for (const ObjectByExtruder::Island::Region &region : by_region)
        if (! region.infills.empty()) {
            extrusions.clear();
            extrusions.reserve(region.infills.size());
            for (ExtrusionEntity *ee : region.infills)
                if ((ee->role() == erIroning) == ironing)
                    extrusions.emplace_back(ee);
            if (! extrusions.empty()) {
                m_config.apply(print.get_print_region(&region - &by_region.front()).config());
                chain_and_reorder_extrusion_entities(extrusions, &m_last_pos);
                for (const ExtrusionEntity *fill : extrusions) {
                    auto *eec = dynamic_cast<const ExtrusionEntityCollection*>(fill);
                    if (eec) {
                        for (ExtrusionEntity *ee : eec->chained_path_from(m_last_pos).entities)
                            gcode += this->extrude_entity(*ee, extrusion_name);
                    } else
                        gcode += this->extrude_entity(*fill, extrusion_name);
                }
            }
        }
    return gcode;
}

std::string GCode::extrude_support(const ExtrusionEntityCollection &support_fills, const ExtrusionRole support_extrusion_role)
{
    static constexpr const char *support_label            = "support material";
    static constexpr const char *support_interface_label  = "support material interface";
    static constexpr const char* support_transition_label = "support transition";
    static constexpr const char* support_ironing_label    = "support ironing";

    std::string gcode;
    if (! support_fills.entities.empty()) {

        ExtrusionEntitiesPtr extrusions;
        extrusions.reserve(support_fills.entities.size());
        for (ExtrusionEntity* ee : support_fills.entities) {
            const auto role = ee->role();
            if ((role == support_extrusion_role) || (support_extrusion_role == erMixed && role != erIroning)) {
                extrusions.emplace_back(ee);
            }
        }
        if (extrusions.empty())
            return gcode;

        chain_and_reorder_extrusion_entities(extrusions, &m_last_pos);

        const double  support_speed            = m_config.support_speed.value;
        const double  support_interface_speed  = m_config.get_abs_value("support_interface_speed");
        for (const ExtrusionEntity *ee : extrusions) {
            ExtrusionRole role = ee->role();
            assert(role == erSupportMaterial || role == erSupportMaterialInterface || role == erSupportTransition || role == erIroning);
            const char* label = (role == erSupportMaterial) ? support_label :
                ((role == erSupportMaterialInterface) ? support_interface_label : 
                ((role == erIroning) ? support_ironing_label : support_transition_label));
            // BBS
            //const double speed = (role == erSupportMaterial) ? support_speed : support_interface_speed;
            const double speed = -1.0;
            const ExtrusionPath* path = dynamic_cast<const ExtrusionPath*>(ee);
            const ExtrusionMultiPath* multipath = dynamic_cast<const ExtrusionMultiPath*>(ee);
            const ExtrusionLoop* loop = dynamic_cast<const ExtrusionLoop*>(ee);
            const ExtrusionEntityCollection* collection = dynamic_cast<const ExtrusionEntityCollection*>(ee);
            if (path)
                gcode += this->extrude_path(*path, label, speed);
            else if (multipath) {
                gcode += this->extrude_multi_path(*multipath, label, speed);
            }
            else if (loop) {
                gcode += this->extrude_loop(*loop, label, speed);
            }
            else if (collection) {
                gcode += extrude_support(*collection, support_extrusion_role);
            }
            else {
                throw Slic3r::InvalidArgument("Unknown extrusion type");
            }
        }
    }
    return gcode;
}

bool GCode::GCodeOutputStream::is_error() const
{
    return ::ferror(this->f);
}

void GCode::GCodeOutputStream::flush()
{
    ::fflush(this->f);
}

void GCode::GCodeOutputStream::close()
{
    if (this->f) {
        ::fclose(this->f);
        this->f = nullptr;
    }
}

void GCode::GCodeOutputStream::write(const char *what)
{
    if (what != nullptr) {
        const char* gcode = what;
        // writes string to file
        fwrite(gcode, 1, ::strlen(gcode), this->f);
        //FIXME don't allocate a string, maybe process a batch of lines?
        m_processor.process_buffer(std::string(gcode));
    }
}

void GCode::GCodeOutputStream::writeln(const std::string &what)
{
    if (! what.empty())
        this->write(what.back() == '\n' ? what : what + '\n');
}

void GCode::GCodeOutputStream::write_format(const char* format, ...)
{
    va_list args;
    va_start(args, format);

    int buflen;
    {
        va_list args2;
        va_copy(args2, args);
        buflen =
    #ifdef _MSC_VER
            ::_vscprintf(format, args2)
    #else
            ::vsnprintf(nullptr, 0, format, args2)
    #endif
            + 1;
        va_end(args2);
    }

    char buffer[1024];
    bool buffer_dynamic = buflen > 1024;
    char *bufptr = buffer_dynamic ? (char*)malloc(buflen) : buffer;
    int res = ::vsnprintf(bufptr, buflen, format, args);
    if (res > 0)
        this->write(bufptr);

    if (buffer_dynamic)
        free(bufptr);

    va_end(args);
}

bool GCode::_needSAFC(const ExtrusionPath &path)
{
    if (!m_small_area_infill_flow_compensator || !m_config.small_area_infill_flow_compensation.value)
        return false;

    static const InfillPattern supported_patterns[] = {
        InfillPattern::ipRectilinear,
        InfillPattern::ipAlignedRectilinear,
        InfillPattern::ipMonotonic,
        InfillPattern::ipMonotonicLine,
    };

    return std::any_of(std::begin(supported_patterns), std::end(supported_patterns), [&](const InfillPattern pattern) {
        return this->on_first_layer() && this->config().bottom_surface_pattern == pattern ||
               path.role() == erSolidInfill && this->config().internal_solid_infill_pattern == pattern ||
               path.role() == erTopSolidInfill && this->config().top_surface_pattern == pattern;
    });
}

double GCode::calc_max_volumetric_speed(const double layer_height, const double line_width, const std::string co_str)
{
    std::vector<double> cs;
    std::stringstream   ss(co_str);
    std::string         token;

    while (std::getline(ss, token, ' ')) {
        try {
            cs.push_back(std::stod(token));
        } catch (...) {
            std::cerr << "Transformation failed: " << token << std::endl;
        }
    }
    if (cs.size() != 6 || std::all_of(cs.begin(), cs.end(), [](double v) { return v == 0; })) return std::numeric_limits<double>::max();

    const double x = layer_height;
    const double y = line_width;

    double res = cs[0] * x * x + cs[1] * y * y + cs[2] * x * y + cs[3] * x + cs[4] * y + cs[5];
    return res;
}

std::string GCode::_extrude(const ExtrusionPath &path, std::string description, double speed)
{
    std::string gcode;

    if (is_bridge(path.role()))
        description += " (bridge)";

    const ExtrusionPathSloped* sloped = dynamic_cast<const ExtrusionPathSloped*>(&path);

    const auto get_sloped_z = [&sloped, this](double z_ratio) {
        const auto height = sloped->height;
        return lerp(m_nominal_z - height, m_nominal_z, z_ratio);
    };

    bool slope_need_z_travel = false;
    if (sloped != nullptr && !sloped->is_flat()) {
        auto target_z = get_sloped_z(sloped->slope_begin.z_ratio);
        slope_need_z_travel = m_writer.will_move_z(target_z);
    }
    // Move to first point of extrusion path
    // path is 2D. But in slope lift case, lift z is done in travel_to function.
    // Add m_need_change_layer_lift_z when change_layer in case of no lift if m_last_pos is equal to path.first_point() by chance
    if (!m_last_pos_defined || m_last_pos != path.first_point() || m_need_change_layer_lift_z || slope_need_z_travel) {
        const bool _last_pos_undefined = !m_last_pos_defined;
        gcode += this->travel_to(
            path.first_point(),
            path.role(),
            "move to first " + description + " point",
            sloped == nullptr ? DBL_MAX : get_sloped_z(sloped->slope_begin.z_ratio)
        );
        m_need_change_layer_lift_z = false;
        // Orca: ensure Z matches planned layer height
        if (_last_pos_undefined && !slope_need_z_travel) {
            gcode += this->writer().travel_to_z(m_nominal_z, "ensure Z matches planned layer height", true);
        }
    }


    // if needed, write the gcode_label_objects_end then gcode_label_objects_start
    // should be already done by travel_to, but just in case
    m_writer.add_object_change_labels(gcode);

    // compensate retraction
    gcode += this->unretract();
    m_config.apply(m_calib_config);

    // Orca: optimize for Klipper, set acceleration and jerk in one command
    unsigned int acceleration_i = 0;
    double jerk = 0;
    // adjust acceleration
    if (m_config.default_acceleration.value > 0) {
        double acceleration;
        if (this->on_first_layer() && m_config.initial_layer_acceleration.value > 0) {
            acceleration = m_config.initial_layer_acceleration.value;
#if 0
        } else if (this->object_layer_over_raft() && m_config.first_layer_acceleration_over_raft.value > 0) {
            acceleration = m_config.first_layer_acceleration_over_raft.value;
#endif
        } else if (m_config.get_abs_value("bridge_acceleration") > 0 && is_bridge(path.role())) {
            acceleration = m_config.get_abs_value("bridge_acceleration");
        } else if (m_config.get_abs_value("sparse_infill_acceleration") > 0 && (path.role() == erInternalInfill)) {
            acceleration = m_config.get_abs_value("sparse_infill_acceleration");
        } else if (m_config.get_abs_value("internal_solid_infill_acceleration") > 0 && (path.role() == erSolidInfill)) {
            acceleration = m_config.get_abs_value("internal_solid_infill_acceleration");
        } else if (m_config.outer_wall_acceleration.value > 0 && is_external_perimeter(path.role())) {
            acceleration = m_config.outer_wall_acceleration.value;
        } else if (m_config.inner_wall_acceleration.value > 0 && is_internal_perimeter(path.role())) {
            acceleration = m_config.inner_wall_acceleration.value;
        } else if (m_config.top_surface_acceleration.value > 0 && is_top_surface(path.role())) {
            acceleration = m_config.top_surface_acceleration.value;
        } else {
            acceleration = m_config.default_acceleration.value;
        }
        acceleration_i = (unsigned int)floor(acceleration + 0.5);
    }

    // adjust X Y jerk
    if (m_config.default_jerk.value > 0) {
        if (this->on_first_layer() && m_config.initial_layer_jerk.value > 0) {
            jerk = m_config.initial_layer_jerk.value;
        } else if (m_config.outer_wall_jerk.value > 0 && is_external_perimeter(path.role())) {
             jerk = m_config.outer_wall_jerk.value;
        } else if (m_config.inner_wall_jerk.value > 0 && is_internal_perimeter(path.role())) {
            jerk = m_config.inner_wall_jerk.value;
        } else if (m_config.top_surface_jerk.value > 0 && is_top_surface(path.role())) {
            jerk = m_config.top_surface_jerk.value;
        } else if (m_config.infill_jerk.value > 0 && is_infill(path.role())) {
            jerk = m_config.infill_jerk.value;
        }
        else {
            jerk = m_config.default_jerk.value;
        }
    }

    if (m_writer.get_gcode_flavor() == gcfKlipper) {
        gcode += m_writer.set_accel_and_jerk(acceleration_i, jerk);

    } else {
        gcode += m_writer.set_print_acceleration(acceleration_i);
        gcode += m_writer.set_jerk_xy(jerk);
    }

    // calculate effective extrusion length per distance unit (e_per_mm)
    double filament_flow_ratio = m_config.option<ConfigOptionFloats>("filament_flow_ratio")->get_at(0);
    // We set _mm3_per_mm to effectove flow = Geometric volume * print flow ratio * filament flow ratio * role-based-flow-ratios
    auto _mm3_per_mm = path.mm3_per_mm * this->config().print_flow_ratio;
    _mm3_per_mm *= filament_flow_ratio;

    if (path.role() == erTopSolidInfill) {
        _mm3_per_mm *= m_config.top_solid_infill_flow_ratio;
    } else if (path.role() == erBottomSurface) {
        _mm3_per_mm *= m_config.bottom_solid_infill_flow_ratio;
    } else if (path.role() == erInternalBridgeInfill) {
        _mm3_per_mm *= m_config.internal_bridge_flow;
    } else if (sloped) {
        _mm3_per_mm *= m_config.scarf_joint_flow_ratio;
    }

    if (m_config.set_other_flow_ratios) {
        if (path.role() == erExternalPerimeter) {
            _mm3_per_mm *= m_config.outer_wall_flow_ratio;
        } else if (path.role() == erPerimeter) {
            _mm3_per_mm *= m_config.inner_wall_flow_ratio;
        } else if (path.role() == erOverhangPerimeter) {
            _mm3_per_mm *= m_config.overhang_flow_ratio;
        } else if (path.role() == erInternalInfill) {
            _mm3_per_mm *= m_config.sparse_infill_flow_ratio;
        } else if (path.role() == erSolidInfill) {
            _mm3_per_mm *= m_config.internal_solid_infill_flow_ratio;
        } else if (path.role() == erGapFill) {
            _mm3_per_mm *= m_config.gap_fill_flow_ratio;
        } else if (path.role() == erSupportMaterial) { // Should this condition also cover erSupportTransition?
            _mm3_per_mm *= m_config.support_flow_ratio;
        } else if (path.role() == erSupportMaterialInterface) {
            _mm3_per_mm *= m_config.support_interface_flow_ratio;
        }

        // Additionally, adjust the value if we are on the first layer (except for brims and skirts)
        if (this->on_first_layer() && (path.role() != erBrim && path.role() != erSkirt)) {
            _mm3_per_mm *= m_config.first_layer_flow_ratio;
        }
    }

    // Effective extrusion length per distance unit = (filament_flow_ratio/cross_section) * mm3_per_mm / print flow ratio
    // m_writer.extruder()->e_per_mm3() below is (filament flow ratio / cross-sectional area)
    double e_per_mm = m_writer.filament()->e_per_mm3() * _mm3_per_mm;
    e_per_mm /= filament_flow_ratio;

    // set speed
    if (speed == -1) {
        if (path.role() == erPerimeter) {
            speed = m_config.get_abs_value("inner_wall_speed");
            if (sloped) {
                speed = std::min(speed, m_config.scarf_joint_speed.get_abs_value(m_config.get_abs_value("inner_wall_speed")));
            }
        } else if (path.role() == erExternalPerimeter) {
            speed = m_config.get_abs_value("outer_wall_speed");
            if (sloped) {
                speed = std::min(speed, m_config.scarf_joint_speed.get_abs_value(m_config.get_abs_value("outer_wall_speed")));
            }
        } 
        else if(path.role() == erInternalBridgeInfill) {
            speed = m_config.get_abs_value("internal_bridge_speed");
        } else if (path.role() == erOverhangPerimeter || path.role() == erSupportTransition || path.role() == erBridgeInfill) {
            speed = m_config.get_abs_value("bridge_speed");
        } else if (path.role() == erInternalInfill) {
            speed = m_config.get_abs_value("sparse_infill_speed");
        } else if (path.role() == erSolidInfill) {
            speed = m_config.get_abs_value("internal_solid_infill_speed");
        } else if (path.role() == erTopSolidInfill) {
            speed = m_config.get_abs_value("top_surface_speed");
        } else if (path.role() == erIroning) {
            speed = m_config.get_abs_value("ironing_speed");
        } else if (path.role() == erBottomSurface) {
            speed = m_config.get_abs_value("initial_layer_infill_speed");
        } else if (path.role() == erGapFill) {
            speed = m_config.get_abs_value("gap_infill_speed");
        }
        else if (path.role() == erSupportMaterial ||
                 path.role() == erSupportMaterialInterface) {
            const double  support_speed = m_config.support_speed.value;
            const double  support_interface_speed = m_config.get_abs_value("support_interface_speed");
            speed = (path.role() == erSupportMaterial) ? support_speed : support_interface_speed;
        } else {
            throw Slic3r::InvalidArgument("Invalid speed");
        }
    }
    //BBS: if not set the speed, then use the filament_max_volumetric_speed directly
    double filament_max_volumetric_speed = FILAMENT_CONFIG(filament_max_volumetric_speed);
    if (FILAMENT_CONFIG(filament_adaptive_volumetric_speed)){
        double fitted_value = calc_max_volumetric_speed(path.height, path.width, FILAMENT_CONFIG(volumetric_speed_coefficients));
        filament_max_volumetric_speed = std::min(filament_max_volumetric_speed, fitted_value);
    }

    if (speed == 0)
        speed = filament_max_volumetric_speed / _mm3_per_mm;
    if (this->on_first_layer()) {
        //BBS: for solid infill of initial layer, speed can be higher as long as
        //wall lines have be attached
        if (path.role() != erBottomSurface)
            speed = m_config.get_abs_value("initial_layer_speed");
    }
    else if(m_config.slow_down_layers > 1){
        const auto _layer = layer_id();
        if (_layer > 0 && _layer < m_config.slow_down_layers) {
            const auto first_layer_speed =
                is_perimeter(path.role())
                    ? m_config.get_abs_value("initial_layer_speed")
                    : m_config.get_abs_value("initial_layer_infill_speed");
            if (first_layer_speed < speed) {
                speed = std::min(
                    speed,
                    Slic3r::lerp(first_layer_speed, speed,
                                 (double)_layer / m_config.slow_down_layers));
            }
        }
    }
    // Override skirt speed if set
    if (path.role() == erSkirt) {
        const double skirt_speed = m_config.get_abs_value("skirt_speed");
        if (skirt_speed > 0.0)
        speed = skirt_speed;
    }
    //BBS: remove this config
    //else if (this->object_layer_over_raft())
    //    speed = m_config.get_abs_value("first_layer_speed_over_raft", speed);
    //if (m_config.max_volumetric_speed.value > 0) {
    //    // cap speed with max_volumetric_speed anyway (even if user is not using autospeed)
    //    speed = std::min(
    //        speed,
    //        m_config.max_volumetric_speed.value / _mm3_per_mm
    //    );
    //}
    if (FILAMENT_CONFIG(filament_max_volumetric_speed) > 0) {
        // cap speed with max_volumetric_speed anyway (even if user is not using autospeed)
        speed = std::min(speed, FILAMENT_CONFIG(filament_max_volumetric_speed) / _mm3_per_mm);
    }
    // ORCA: resonance‑avoidance on short external perimeters
{
    double ref_speed = speed;  // stash the pre‑cap speed
    if (path.role() == erExternalPerimeter
        && m_config.resonance_avoidance.value) {

        // if our original speed was above “max”, disable RA for this loop
        if (ref_speed > m_config.max_resonance_avoidance_speed.value) {
            m_resonance_avoidance = false;
        }

        // re‑apply volumetric cap
        if (FILAMENT_CONFIG(filament_max_volumetric_speed) > 0) {
            speed = std::min(
                speed,
                FILAMENT_CONFIG(filament_max_volumetric_speed) / _mm3_per_mm
            );
        }

            // if still in avoidance mode and under "max", adjust speed:
            // - speeds in lower half of range: clamp down to "min"
            // - speeds in upper half of range: boost up to "max"
        if (m_resonance_avoidance && speed < m_config.max_resonance_avoidance_speed.value) {
            if (speed < m_config.min_resonance_avoidance_speed.value +
                            ((m_config.max_resonance_avoidance_speed.value - m_config.min_resonance_avoidance_speed.value) / 2)) {
                speed = std::min(speed, m_config.min_resonance_avoidance_speed.value);
            } else {
                speed = m_config.max_resonance_avoidance_speed.value;
            }
        }

        // reset flag for next segment
        m_resonance_avoidance = true;
    }
}
    
    bool variable_speed = false;
    std::vector<ProcessedPoint> new_points {};

    if (m_config.enable_overhang_speed && !this->on_first_layer() &&
        (is_bridge(path.role()) || is_perimeter(path.role()))) {
            bool is_external = is_external_perimeter(path.role());
            double ref_speed   = is_external ? m_config.get_abs_value("outer_wall_speed") : m_config.get_abs_value("inner_wall_speed");
            if (ref_speed == 0)
                ref_speed = FILAMENT_CONFIG(filament_max_volumetric_speed) / _mm3_per_mm;

            if (EXTRUDER_CONFIG(filament_max_volumetric_speed) > 0) {
                ref_speed = std::min(ref_speed, FILAMENT_CONFIG(filament_max_volumetric_speed) / _mm3_per_mm);
            }
            if (sloped) {
                ref_speed = std::min(ref_speed, m_config.scarf_joint_speed.get_abs_value(ref_speed));
            }
            
            ConfigOptionPercents         overhang_overlap_levels({90, 75, 50, 25, 13, 0});

            if (m_config.slowdown_for_curled_perimeters){
                ConfigOptionFloatsOrPercents dynamic_overhang_speeds(
                    {FloatOrPercent{100, true},
                     (m_config.get_abs_value("overhang_1_4_speed", ref_speed) < 0.5) ?
                         FloatOrPercent{100, true} :
                         FloatOrPercent{m_config.get_abs_value("overhang_1_4_speed", ref_speed) * 100 / ref_speed, true},
                     (m_config.get_abs_value("overhang_2_4_speed", ref_speed) < 0.5) ?
                         FloatOrPercent{100, true} :
                         FloatOrPercent{m_config.get_abs_value("overhang_2_4_speed", ref_speed) * 100 / ref_speed, true},
                     (m_config.get_abs_value("overhang_3_4_speed", ref_speed) < 0.5) ?
                         FloatOrPercent{100, true} :
                         FloatOrPercent{m_config.get_abs_value("overhang_3_4_speed", ref_speed) * 100 / ref_speed, true},
                     (m_config.get_abs_value("overhang_4_4_speed", ref_speed) < 0.5) ?
                         FloatOrPercent{100, true} :
                         FloatOrPercent{m_config.get_abs_value("overhang_4_4_speed", ref_speed) * 100 / ref_speed, true},
                     (m_config.get_abs_value("overhang_4_4_speed", ref_speed) < 0.5) ?
                         FloatOrPercent{100, true} :
                         FloatOrPercent{m_config.get_abs_value("overhang_4_4_speed", ref_speed) * 100 / ref_speed, true}});

                new_points = m_extrusion_quality_estimator.estimate_extrusion_quality(path, overhang_overlap_levels, dynamic_overhang_speeds,
                                                                              ref_speed, speed, m_config.slowdown_for_curled_perimeters);
        	}else{
                ConfigOptionFloatsOrPercents dynamic_overhang_speeds(
                                                                     {FloatOrPercent{100, true},
                     (m_config.get_abs_value("overhang_1_4_speed", ref_speed) < 0.5) ?
                         FloatOrPercent{100, true} :
                         FloatOrPercent{m_config.get_abs_value("overhang_1_4_speed", ref_speed) * 100 / ref_speed, true},
                     (m_config.get_abs_value("overhang_2_4_speed", ref_speed) < 0.5) ?
                         FloatOrPercent{100, true} :
                         FloatOrPercent{m_config.get_abs_value("overhang_2_4_speed", ref_speed) * 100 / ref_speed, true},
                     (m_config.get_abs_value("overhang_3_4_speed", ref_speed) < 0.5) ?
                         FloatOrPercent{100, true} :
                         FloatOrPercent{m_config.get_abs_value("overhang_3_4_speed", ref_speed) * 100 / ref_speed, true},
                      (m_config.get_abs_value("overhang_4_4_speed", ref_speed) < 0.5) ?
                            FloatOrPercent{100, true} :
                            FloatOrPercent{m_config.get_abs_value("overhang_4_4_speed", ref_speed) * 100 / ref_speed, true},
                     FloatOrPercent{m_config.get_abs_value("bridge_speed") * 100 / ref_speed, true}});

                new_points = m_extrusion_quality_estimator.estimate_extrusion_quality(path, overhang_overlap_levels, dynamic_overhang_speeds,
                                                                              ref_speed, speed, m_config.slowdown_for_curled_perimeters);
            }
            variable_speed = std::any_of(new_points.begin(), new_points.end(),
                                         [speed](const ProcessedPoint &p) { return fabs(double(p.speed) - speed) > 1; }); // Ignore small speed variations (under 1mm/sec)
    }

    double F = speed * 60;  // convert mm/sec to mm/min
    
    // Orca: Dynamic PA
    // If adaptive PA is enabled, by default evaluate PA on all extrusion moves
    bool is_pa_calib = m_curr_print->calib_mode() == CalibMode::Calib_PA_Line ||
                       m_curr_print->calib_mode() == CalibMode::Calib_PA_Pattern ||
                       m_curr_print->calib_mode() == CalibMode::Calib_PA_Tower; 
    bool evaluate_adaptive_pa = false;
    bool role_change = (m_last_extrusion_role != path.role());
    if (!is_pa_calib && FILAMENT_CONFIG(adaptive_pressure_advance) && FILAMENT_CONFIG(enable_pressure_advance)) {
        evaluate_adaptive_pa = true;
        // If we have already emmited a PA change because the m_multi_flow_segment_path_pa_set is set
        // skip re-issuing the PA change tag.
        if (m_multi_flow_segment_path_pa_set && evaluate_adaptive_pa)
            evaluate_adaptive_pa = false;
        // TODO: Explore forcing evaluation of PA if a role change is happening mid extrusion.
        // TODO: This would enable adapting PA for overhang perimeters as they are part of the current loop
        // TODO: The issue with simply enabling PA evaluation on a role change is that the speed change
        // TODO: is issued before the overhang perimeter role change is triggered
        // TODO: because for some reason (maybe path segmentation upstream?) there is a short path extruded
        // TODO: with the overhang speed and flow before the role change is flagged in the path.role() function.
        if(role_change)
            evaluate_adaptive_pa = true;
    }
    // Orca: End of dynamic PA trigger flag segment
    
    //Orca: process custom gcode for extrusion role change
    if (path.role() != m_last_extrusion_role && !m_config.change_extrusion_role_gcode.value.empty()) {
            DynamicConfig config;
            config.set_key_value("extrusion_role", new ConfigOptionString(extrusion_role_to_string_for_parser(path.role())));
            config.set_key_value("last_extrusion_role", new ConfigOptionString(extrusion_role_to_string_for_parser(m_last_extrusion_role)));
            config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index + 1));
            config.set_key_value("layer_z", new ConfigOptionFloat(m_layer == nullptr ? m_last_height : m_layer->print_z));
            gcode += this->placeholder_parser_process("change_extrusion_role_gcode",
                                                      m_config.change_extrusion_role_gcode.value, m_writer.filament()->id(), &config)
                     + "\n";
    }

    // extrude arc or line
    if (m_enable_extrusion_role_markers) {
        if (path.role() != m_last_extrusion_role) {
            char buf[32];
            sprintf(buf, ";_EXTRUSION_ROLE:%d\n", int(path.role()));
            gcode += buf;
      }
    }

    m_last_extrusion_role = path.role();

    // adds processor tags and updates processor tracking data
    // PrusaMultiMaterial::Writer may generate GCodeProcessor::Height_Tag lines without updating m_last_height
    // so, if the last role was erWipeTower we force export of GCodeProcessor::Height_Tag lines
    bool last_was_wipe_tower = (m_last_processor_extrusion_role == erWipeTower);
    char buf[64];
    assert(is_decimal_separator_point());

    if (path.role() != m_last_processor_extrusion_role) {
        m_last_processor_extrusion_role = path.role();
        sprintf(buf, ";%s%s\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role).c_str(), ExtrusionEntity::role_to_string(m_last_processor_extrusion_role).c_str());
        gcode += buf;
    }

    if (last_was_wipe_tower || m_last_width != path.width) {
        m_last_width = path.width;
        sprintf(buf, ";%s%g\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Width).c_str(), m_last_width);
        gcode += buf;
    }

#if ENABLE_GCODE_VIEWER_DATA_CHECKING
    if (last_was_wipe_tower || (m_last_mm3_per_mm != path.mm3_per_mm)) {
        m_last_mm3_per_mm = path.mm3_per_mm;
        sprintf(buf, ";%s%f\n", GCodeProcessor::Mm3_Per_Mm_Tag.c_str(), m_last_mm3_per_mm);
        gcode += buf;
    }
#endif // ENABLE_GCODE_VIEWER_DATA_CHECKING

    if (last_was_wipe_tower || std::abs(m_last_height - path.height) > EPSILON) {
        m_last_height = path.height;
        sprintf(buf, ";%s%g\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height).c_str(), m_last_height);
        gcode += buf;
    }
    
    // Orca: Dynamic PA
    // Post processor flag generation code segment when option to emit only at role changes is enabled
    // Variables published to the post processor:
    // 1) Tag to trigger a PA evaluation (because a role change was identified and the user has requested dynamic PA adjustments)
    // 2) Current extruder ID (to identify the PA model for the currently used extruder)
    // 3) mm3_per_mm value (to then multiply by the final model print speed after slowdown for cooling is applied)
    // 4) the current acceleration (to pass to the model for evaluation)
    // 5) whether this is an external perimeter (for future use)
    // 6) whether this segment is triggered because of a role change (to aid in calculation of average speed for the role)
    // This tag simplifies the creation of the gcode post processor while also keeping the feature decoupled from other tags.
    if (evaluate_adaptive_pa) {
        bool isOverhangPerimeter = (path.role() == erOverhangPerimeter);
        if (m_multi_flow_segment_path_average_mm3_per_mm > 0) {
            sprintf(buf, ";%sT%u MM3MM:%g ACCEL:%u BR:%d RC:%d OV:%d\n",
                    GCodeProcessor::reserved_tag(GCodeProcessor::ETags::PA_Change).c_str(),
                    m_writer.filament()->id(),
                    m_multi_flow_segment_path_average_mm3_per_mm,
                    acceleration_i,
                    ((path.role() == erBridgeInfill) ||(path.role() == erOverhangPerimeter)),
                    role_change,
                    isOverhangPerimeter);
            gcode += buf;
        } else if(_mm3_per_mm >0 ){ // Triggered when extruding a single segment path (like a line).
                                    // Check if mm3_mm value is greater than zero as the wipe before external perimeter
                                    // is a zero mm3_mm path to force de-retraction to happen and we dont want
                                    // to issue a zero flow PA change command for this
            sprintf(buf, ";%sT%u MM3MM:%g ACCEL:%u BR:%d RC:%d OV:%d\n",
                    GCodeProcessor::reserved_tag(GCodeProcessor::ETags::PA_Change).c_str(),
                    m_writer.filament()->id(),
                    _mm3_per_mm,
                    acceleration_i,
                    ((path.role() == erBridgeInfill) ||(path.role() == erOverhangPerimeter)),
                    role_change,
                    isOverhangPerimeter);
            gcode += buf;
        }
    }


    auto overhang_fan_threshold = FILAMENT_CONFIG(overhang_fan_threshold);
    auto enable_overhang_bridge_fan = FILAMENT_CONFIG(enable_overhang_bridge_fan);

    //    { "0%", Overhang_threshold_none },
    //    { "10%", Overhang_threshold_1_4 },
    //    { "25%", Overhang_threshold_2_4 },
    //    { "50%", Overhang_threshold_3_4 },
    //    { "75%", Overhang_threshold_4_4 },
    //    { "95%", Overhang_threshold_bridge }
    auto check_overhang_fan = [&overhang_fan_threshold](float overlap, ExtrusionRole role) {
      if (role == erBridgeInfill || role == erOverhangPerimeter) { // ORCA: Split out bridge infill to internal and external to apply separate fan settings
        return true;
      }
      switch (overhang_fan_threshold) {
      case (int)Overhang_threshold_1_4:
        return overlap <= 0.9f;
        break;
      case (int)Overhang_threshold_2_4:
        return overlap <= 0.75f;
        break;
      case (int)Overhang_threshold_3_4:
        return overlap <= 0.5f;
        break;
      case (int)Overhang_threshold_4_4:
        return overlap <= 0.25f;
        break;
      case (int)Overhang_threshold_bridge:
        return overlap <= 0.05f;
        break;
      case (int)Overhang_threshold_none:
        return is_external_perimeter(role);
        break;
      default:
        return false;
      }
    };

    std::string comment;
    if (m_enable_cooling_markers) {
        comment = ";_EXTRUDE_SET_SPEED";
        if (is_external_perimeter(path.role()))
            comment += ";_EXTERNAL_PERIMETER";
    }

    // Change fan speed based on current extrusion role
    auto append_role_based_fan_marker = [this, &gcode](const ExtrusionRole role, const std::string_view& marker_prefix, const bool fan_on) {
        assert(m_enable_cooling_markers);

        if (fan_on) {
            if (!m_is_role_based_fan_on[role]) {
                gcode += ";";
                gcode += marker_prefix;
                gcode += "_FAN_START\n";
                m_is_role_based_fan_on[role] = true;
            }
        } else {
            if (m_is_role_based_fan_on[role]) {
                gcode += ";";
                gcode += marker_prefix;
                gcode += "_FAN_END\n";
                m_is_role_based_fan_on[role] = false;
            }
        }
    };
    auto apply_role_based_fan_speed = [
        &path, &append_role_based_fan_marker,
        supp_interface_fan_speed = FILAMENT_CONFIG(support_material_interface_fan_speed),
        ironing_fan_speed        = FILAMENT_CONFIG(ironing_fan_speed)
    ] {
        append_role_based_fan_marker(erSupportMaterialInterface, "_SUPP_INTERFACE"sv,
                                     supp_interface_fan_speed >= 0 && path.role() == erSupportMaterialInterface);
        append_role_based_fan_marker(erIroning, "_IRONING"sv,
                                     ironing_fan_speed >= 0 && path.role() == erIroning);
    };

    if (!variable_speed) {
        // F is mm per minute.
        if( (std::abs(writer().get_current_speed() - F) > EPSILON) || (std::abs(_mm3_per_mm - m_last_mm3_mm) > EPSILON) ){
            // ORCA: Adaptive PA code segment when adjusting PA within the same feature
            // There is a speed change coming out of an overhang region
            // or a flow change, so emit the flag to evaluate PA for the upcomming extrusion
            // Emit tag before new speed is set so the post processor reads the next speed immediately and uses it.
            // Dont emit tag if it has just already been emitted from a role change above
            if(_mm3_per_mm >0 &&
               FILAMENT_CONFIG(adaptive_pressure_advance) &&
               FILAMENT_CONFIG(enable_pressure_advance) &&
               FILAMENT_CONFIG(adaptive_pressure_advance_overhangs) &&
               !evaluate_adaptive_pa){
                if(writer().get_current_speed() > F){ // Ramping down speed - use overhang logic where the minimum speed is used between current and upcoming extrusion
                    if(m_config.gcode_comments){
                        sprintf(buf, "; Ramp down-non-variable\n");
                        gcode += buf;
                    }
                    sprintf(buf, ";%sT%u MM3MM:%g ACCEL:%u BR:%d RC:%d OV:%d\n",
                            GCodeProcessor::reserved_tag(GCodeProcessor::ETags::PA_Change).c_str(),
                            m_writer.filament()->id(),
                            _mm3_per_mm,
                            acceleration_i,
                            ((path.role() == erBridgeInfill) ||(path.role() == erOverhangPerimeter)),
                            1, // Force a dummy "role change" & "overhang perimeter" for the post processor, as, while technically it is not a role change,
                            // the properties of the extrusion in the overhang are different so it behaves similarly to a role
                            // change for the Adaptive PA post processor.
                            1);
                }else{ // Ramping up speed - use baseline logic where max speed is used between current and upcoming extrusion
                    if(m_config.gcode_comments){ 
                        sprintf(buf, "; Ramp up-non-variable\n");
                        gcode += buf;
                    }
                    sprintf(buf, ";%sT%u MM3MM:%g ACCEL:%u BR:%d RC:%d OV:%d\n",
                            GCodeProcessor::reserved_tag(GCodeProcessor::ETags::PA_Change).c_str(),
                            m_writer.filament()->id(),
                            _mm3_per_mm,
                            acceleration_i,
                            ((path.role() == erBridgeInfill) ||(path.role() == erOverhangPerimeter)),
                            1, // Force a dummy "role change" & "overhang perimeter" for the post processor, as, while technically it is not a role change,
                            // the properties of the extrusion in the overhang are different so it is technically similar to a role
                            // change for the Adaptive PA post processor.
                            0);
                }
                gcode += buf;
                m_last_mm3_mm = _mm3_per_mm;
            }
            // ORCA: End of adaptive PA code segment
        }
        
        gcode += m_writer.set_speed(F, "", comment);
        {
            if (m_enable_cooling_markers) {
                if (enable_overhang_bridge_fan) {
                    // BBS: Overhang_threshold_none means Overhang_threshold_1_4 and forcing cooling for all external
                    // perimeter
                    append_role_based_fan_marker(erOverhangPerimeter, "_OVERHANG"sv,
                                                 (overhang_fan_threshold == Overhang_threshold_none && is_external_perimeter(path.role())) ||
                                                 (path.role() == erBridgeInfill || path.role() == erOverhangPerimeter)); // ORCA: Add support for separate internal bridge fan speed control

                    // ORCA: Add support for separate internal bridge fan speed control
                    append_role_based_fan_marker(erInternalBridgeInfill, "_INTERNAL_BRIDGE"sv, path.role() == erInternalBridgeInfill);
                }

                apply_role_based_fan_speed();
            }
            // BBS: use G1 if not enable arc fitting or has no arc fitting result or in spiral_mode mode or we are doing sloped extrusion
            // Attention: G2 and G3 is not supported in spiral_mode mode
            if (!m_config.enable_arc_fitting || path.polyline.fitting_result.empty() || m_config.spiral_mode || sloped != nullptr) {
                double path_length = 0.;
                double total_length = sloped == nullptr ? 0. : path.polyline.length() * SCALING_FACTOR;
                for (const Line& line : path.polyline.lines()) {
                    std::string tempDescription = description;
                    const double line_length = line.length() * SCALING_FACTOR;
                    if (line_length < EPSILON)
                        continue;
                    path_length += line_length;
                    auto dE = e_per_mm * line_length;
                    if (_needSAFC(path)) {
                        auto oldE = dE;
                        dE = m_small_area_infill_flow_compensator->modify_flow(line_length, dE, path.role());

                        if (m_config.gcode_comments && oldE > 0 && oldE != dE) {
                            tempDescription += Slic3r::format(" | Old Flow Value: %0.5f Length: %0.5f",oldE, line_length);
                        }
                    }
                    if (sloped == nullptr) {
                        // Normal extrusion
                        gcode += m_writer.extrude_to_xy(
                            this->point_to_gcode(line.b),
                            dE,
                            GCodeWriter::full_gcode_comment ? tempDescription : "", path.is_force_no_extrusion());
                    } else {
                        // Sloped extrusion
                        const auto [z_ratio, e_ratio] = sloped->interpolate(path_length / total_length);
                        Vec2d dest2d = this->point_to_gcode(line.b);
                        Vec3d dest3d(dest2d(0), dest2d(1), get_sloped_z(z_ratio));
                        gcode += m_writer.extrude_to_xyz(
                            dest3d,
                            dE * e_ratio,
                            GCodeWriter::full_gcode_comment ? tempDescription : "", path.is_force_no_extrusion());
                    }
                }
            } else {
                // BBS: start to generate gcode from arc fitting data which includes line and arc
                const std::vector<PathFittingData>& fitting_result = path.polyline.fitting_result;
                for (size_t fitting_index = 0; fitting_index < fitting_result.size(); fitting_index++) {
                    std::string tempDescription = description;
                    switch (fitting_result[fitting_index].path_type) {
                    case EMovePathType::Linear_move: {
                        size_t start_index = fitting_result[fitting_index].start_point_index;
                        size_t end_index = fitting_result[fitting_index].end_point_index;
                        for (size_t point_index = start_index + 1; point_index < end_index + 1; point_index++) {
                            tempDescription = description;
                            const Line line = Line(path.polyline.points[point_index - 1], path.polyline.points[point_index]);
                            const double line_length = line.length() * SCALING_FACTOR;
                            if (line_length < EPSILON)
                                continue;
                            auto dE = e_per_mm * line_length;
                            if (_needSAFC(path)) {
                                auto oldE = dE;
                                dE = m_small_area_infill_flow_compensator->modify_flow(line_length, dE, path.role());

                                if (m_config.gcode_comments && oldE > 0 && oldE != dE) {
                                    tempDescription += Slic3r::format(" | Old Flow Value: %0.5f Length: %0.5f",oldE, line_length);
                                }
                            }
                            gcode += m_writer.extrude_to_xy(
                                this->point_to_gcode(line.b),
                                dE,
                                GCodeWriter::full_gcode_comment ? tempDescription : "", path.is_force_no_extrusion());
                        }
                        break;
                    }
                    case EMovePathType::Arc_move_cw:
                    case EMovePathType::Arc_move_ccw: {
                        const ArcSegment& arc = fitting_result[fitting_index].arc_data;
                        const double arc_length = fitting_result[fitting_index].arc_data.length * SCALING_FACTOR;
                        if (arc_length < EPSILON)
                            continue;
                        const Vec2d center_offset = this->point_to_gcode(arc.center) - this->point_to_gcode(arc.start_point);
                        auto dE = e_per_mm * arc_length;
                        if (_needSAFC(path)) {
                            auto oldE = dE;
                            dE = m_small_area_infill_flow_compensator->modify_flow(arc_length, dE, path.role());

                            if (m_config.gcode_comments && oldE > 0 && oldE != dE) {
                                tempDescription += Slic3r::format(" | Old Flow Value: %0.5f Length: %0.5f",oldE, arc_length);
                            }
                        }
                        gcode += m_writer.extrude_arc_to_xy(
                            this->point_to_gcode(arc.end_point),
                            center_offset,
                            dE,
                            arc.direction == ArcDirection::Arc_Dir_CCW,
                            GCodeWriter::full_gcode_comment ? tempDescription : "", path.is_force_no_extrusion());
                        break;
                    }
                    default:
                        // BBS: should never happen that a empty path_type has been stored
                        assert(0);
                        break;
                    }
                }
            }
        }
    } else {
        double last_set_speed = new_points[0].speed * 60.0;

        double total_length = 0;
        if (sloped != nullptr) {
            // Calculate total extrusion length
            Points p;
            p.reserve(new_points.size());
            std::transform(new_points.begin(), new_points.end(), std::back_inserter(p), [](const ProcessedPoint& pp) { return pp.p; });
            Polyline l(p);
            total_length = l.length() * SCALING_FACTOR;
        }
        gcode += m_writer.set_speed(last_set_speed, "", comment);
        Vec2d prev = this->point_to_gcode_quantized(new_points[0].p);
        bool pre_fan_enabled = false;
        bool cur_fan_enabled = false;
        if( m_enable_cooling_markers && enable_overhang_bridge_fan)
            pre_fan_enabled = check_overhang_fan(new_points[0].overlap, path.role());
        
        if(path.role() == erInternalBridgeInfill) // ORCA: Add support for separate internal bridge fan speed control
            pre_fan_enabled = true;

        double path_length = 0.;
        for (size_t i = 1; i < new_points.size(); i++) {
            std::string tempDescription = description;
            const ProcessedPoint &processed_point = new_points[i];
            const ProcessedPoint &pre_processed_point = new_points[i-1];
            Vec2d p = this->point_to_gcode_quantized(processed_point.p);
            if (m_enable_cooling_markers) {
                if (enable_overhang_bridge_fan) {
                    cur_fan_enabled = check_overhang_fan(processed_point.overlap, path.role());
                    append_role_based_fan_marker(erOverhangPerimeter, "_OVERHANG"sv, pre_fan_enabled && cur_fan_enabled);
                    pre_fan_enabled = cur_fan_enabled;

                    // ORCA: Add support for separate internal bridge fan speed control
                    append_role_based_fan_marker(erInternalBridgeInfill, "_INTERNAL_BRIDGE"sv, path.role() == erInternalBridgeInfill);
                }

                apply_role_based_fan_speed();
            }

            const double line_length = (p - prev).norm();
            if(line_length < EPSILON)
                continue;
            path_length += line_length;
            double new_speed = pre_processed_point.speed * 60.0;
            
            if ((std::abs(last_set_speed - new_speed) > EPSILON) || (std::abs(_mm3_per_mm - m_last_mm3_mm) > EPSILON)) {
                // ORCA: Adaptive PA code segment when adjusting PA within the same feature
                // There is a speed change or flow change so emit the flag to evaluate PA for the upcomming extrusion
                // Emit tag before new speed is set so the post processor reads the next speed immediately and uses it.
                if(_mm3_per_mm >0   &&
                   EXTRUDER_CONFIG(adaptive_pressure_advance) &&
                   EXTRUDER_CONFIG(enable_pressure_advance) &&
                   EXTRUDER_CONFIG(adaptive_pressure_advance_overhangs) ){
                    if(last_set_speed > new_speed){ // Ramping down speed - use overhang logic where the minimum speed is used between current and upcoming extrusion
                        if(m_config.gcode_comments) {
                            sprintf(buf, "; Ramp up-variable\n");
                            gcode += buf;
                        }
                        sprintf(buf, ";%sT%u MM3MM:%g ACCEL:%u BR:%d RC:%d OV:%d\n",
                                GCodeProcessor::reserved_tag(GCodeProcessor::ETags::PA_Change).c_str(),
                                m_writer.filament()->id(),
                                _mm3_per_mm,
                                acceleration_i,
                                ((path.role() == erBridgeInfill) ||(path.role() == erOverhangPerimeter)),
                                1, // Force a dummy "role change" & "overhang perimeter" for the post processor, as, while technically it is not a role change,
                                // the properties of the extrusion in the overhang are different so it is technically similar to a role
                                // change for the Adaptive PA post processor.
                                1);
                    }else{ // Ramping up speed - use baseline logic where max speed is used between current and upcoming extrusion
                        if(m_config.gcode_comments) {
                            sprintf(buf, "; Ramp down-variable\n");
                            gcode += buf;
                        }
                        sprintf(buf, ";%sT%u MM3MM:%g ACCEL:%u BR:%d RC:%d OV:%d\n",
                                GCodeProcessor::reserved_tag(GCodeProcessor::ETags::PA_Change).c_str(),
                                m_writer.filament()->id(),
                                _mm3_per_mm,
                                acceleration_i,
                                ((path.role() == erBridgeInfill) ||(path.role() == erOverhangPerimeter)),
                                1, // Force a dummy "role change" & "overhang perimeter" for the post processor, as, while technically it is not a role change,
                                // the properties of the extrusion in the overhang are different so it is technically similar to a role
                                // change for the Adaptive PA post processor.
                                0);
                    }
                    gcode += buf;
                    m_last_mm3_mm = _mm3_per_mm;
                }
            }// ORCA: End of adaptive PA code segment
            
            // Ignore small speed variations - emit speed change if the delta between current and new is greater than 60mm/min / 1mm/sec
            // Reset speed to F if delta to F is less than 1mm/sec
            if ((std::abs(last_set_speed - new_speed) > 60)) {
                gcode += m_writer.set_speed(new_speed, "", comment);
                last_set_speed = new_speed;
            } else if ((std::abs(F - new_speed) <= 60)) {
                gcode += m_writer.set_speed(F, "", comment);
                last_set_speed = F;
            }
            auto dE = e_per_mm * line_length;
            if (_needSAFC(path)) {
                auto oldE = dE;
                dE = m_small_area_infill_flow_compensator->modify_flow(line_length, dE, path.role());

                if (m_config.gcode_comments && oldE > 0 && oldE != dE) {
                    tempDescription += Slic3r::format(" | Old Flow Value: %0.5f Length: %0.5f",oldE, line_length);
                }
            }
            if (sloped == nullptr) {
                // Normal extrusion
                gcode += m_writer.extrude_to_xy(p, dE, GCodeWriter::full_gcode_comment ? tempDescription : "");
            } else {
                // Sloped extrusion
                const auto [z_ratio, e_ratio] = sloped->interpolate(path_length / total_length);
                Vec3d dest3d(p(0), p(1), get_sloped_z(z_ratio));
                gcode += m_writer.extrude_to_xyz(dest3d, dE * e_ratio, GCodeWriter::full_gcode_comment ? tempDescription : "");
            }

            prev = p;

        }
    }
    if (m_enable_cooling_markers) {
            gcode += ";_EXTRUDE_END\n";
    }

    if (path.role() != ExtrusionRole::erGapFill) {
      m_last_notgapfill_extrusion_role = path.role();
    }

    this->set_last_pos(path.last_point());
    return gcode;
}

//Orca: get string name of extrusion role. used for change_extruder_role_gcode
std::string GCode::extrusion_role_to_string_for_parser(const ExtrusionRole & role)
{
    switch (role) {
        case erPerimeter: return "Perimeter";
        case erExternalPerimeter: return "ExternalPerimeter";
        case erOverhangPerimeter: return "OverhangPerimeter";
        case erInternalInfill: return "InternalInfill";
        case erSolidInfill: return "SolidInfill";
        case erTopSolidInfill: return "TopSolidInfill";
        case erBottomSurface: return "BottomSurface";
        case erBridgeInfill:
        case erInternalBridgeInfill: return "BridgeInfill";
        case erGapFill: return "GapFill";
        case erIroning: return "Ironing";
        case erSkirt: return "Skirt";
        case erBrim: return "Brim";
        case erSupportMaterial: return "SupportMaterial";
        case erSupportMaterialInterface: return "SupportMaterialInterface";
        case erSupportTransition: return "SupportTransition";
        case erWipeTower: return "WipeTower";
        case erCustom:
        case erMixed:
        case erCount:
        case erNone:
        default: return "Mixed";
    }
}

// Calculate the interpolated value for the current layer between start_value and end_value
float GCode::interpolate_value_across_layers(float start_value, float end_value) const {
    if (m_layer_index == 1) {
        return start_value;
    } else {
        float ratio = (m_layer_index - 2.0f) / (m_layer_count - 3.0f);
        ratio = std::max(0.0f, std::min(1.0f, ratio)); // clamp
        return start_value + ratio * (end_value - start_value);
    }
}

std::string encodeBase64(uint64_t value)
{
    //Always use big endian mode
    uint8_t src[8];
    for (size_t i = 0; i < 8; i++)
        src[i] = (value >> (8 * i)) & 0xff;

    std::string dest;
    dest.resize(boost::beast::detail::base64::encoded_size(sizeof(src)));
    dest.resize(boost::beast::detail::base64::encode(&dest[0], src, sizeof(src)));
    return dest;
}

std::string GCode::_encode_label_ids_to_base64(std::vector<size_t> ids)
{
    assert(m_label_objects_ids.size() < 64);

    uint64_t bitset = 0;
    for (size_t id : ids) {
        auto index = std::lower_bound(m_label_objects_ids.begin(), m_label_objects_ids.end(), id);
        if (index != m_label_objects_ids.end() && *index == id)
            bitset |= (1ull << (index - m_label_objects_ids.begin()));
        else
            throw Slic3r::LogicError("Unknown label object id!");
    }
    if (bitset == 0)
        throw Slic3r::LogicError("Label object id error!");

    return encodeBase64(bitset);
}

// This method accepts &point in print coordinates.
std::string GCode::travel_to(const Point& point, ExtrusionRole role, std::string comment, double z/* = DBL_MAX*/)
{
    /*  Define the travel move as a line between current position and the taget point.
        This is expressed in print coordinates, so it will need to be translated by
        this->origin in order to get G-code coordinates.  */
    Polyline travel { this->last_pos(), point };

    // check whether a straight travel move would need retraction
    LiftType lift_type = LiftType::SpiralLift;
    bool needs_retraction = this->needs_retraction(travel, role, lift_type);
    // check whether wipe could be disabled without causing visible stringing
    bool could_be_wipe_disabled       = false;
    // Save state of use_external_mp_once for the case that will be needed to call twice m_avoid_crossing_perimeters.travel_to.
    const bool used_external_mp_once  = m_avoid_crossing_perimeters.used_external_mp_once();
    std::string gcode;

    // Orca: we don't need to optimize the Klipper as only set once
    double jerk_to_set = 0.0;
    unsigned int acceleration_to_set = 0;
    
    if (this->on_first_layer()) {
        if (m_config.default_acceleration.value > 0 && m_config.initial_layer_acceleration.value > 0) {
            acceleration_to_set = (unsigned int) floor(m_config.initial_layer_acceleration.value + 0.5);
        }
        
        if (m_config.default_jerk.value > 0 && m_config.initial_layer_jerk.value > 0) {
            jerk_to_set = m_config.initial_layer_jerk.value;
        }
    } else {
        if (m_config.default_acceleration.value > 0) {
            if (role == erExternalPerimeter && travel.length() < scale_(EXTRUDER_CONFIG(retraction_minimum_travel))) {
                if (m_config.outer_wall_acceleration.value > 0)
                    acceleration_to_set = (unsigned int) floor(m_config.outer_wall_acceleration.value + 0.5);
            } else {
                if (m_config.travel_acceleration.value > 0)
                    acceleration_to_set = (unsigned int) floor(m_config.travel_acceleration.value + 0.5);
            }
        }

        if (m_config.default_jerk.value > 0) {
            if (role == erExternalPerimeter && travel.length() < scale_(EXTRUDER_CONFIG(retraction_minimum_travel))) {
                if (m_config.outer_wall_jerk.value > 0)
                    jerk_to_set = m_config.outer_wall_jerk.value;
            } else {
                if (m_config.travel_jerk.value > 0)
                    jerk_to_set = m_config.travel_jerk.value;
            }
        }
    }
    
    if (m_writer.get_gcode_flavor() == gcfKlipper) {
        gcode += m_writer.set_accel_and_jerk(acceleration_to_set, jerk_to_set);
    } else {
        gcode += m_writer.set_travel_acceleration(acceleration_to_set);
        gcode += m_writer.set_jerk_xy(jerk_to_set);
    }

    // if a retraction would be needed, try to use reduce_crossing_wall to plan a
    // multi-hop travel path inside the configuration space
    if (m_config.reduce_crossing_wall
        && !m_avoid_crossing_perimeters.disabled_once()
        && m_writer.is_current_position_clear())
        //BBS: don't generate detour travel paths when current position is unclea
    {
        travel = m_avoid_crossing_perimeters.travel_to(*this, point, &could_be_wipe_disabled);
        // check again whether the new travel path still needs a retraction
        needs_retraction = this->needs_retraction(travel, role, lift_type);
        //if (needs_retraction && m_layer_index > 1) exit(0);
    }

    // Re-allow reduce_crossing_wall for the next travel moves
    m_avoid_crossing_perimeters.reset_once_modifiers();

    // generate G-code for the travel move
    if (needs_retraction) {
        // ORCA: Fix scenario where wipe is disabled when avoid crossing perimeters was enabled even though a retraction move was performed.
        // This replicates the existing behaviour of always wiping when retracting
        /*if (m_config.reduce_crossing_wall && could_be_wipe_disabled)
            m_wipe.reset_path();*/

        Point last_post_before_retract = this->last_pos();
        gcode += this->retract(false, false, lift_type, false, role);
        // When "Wipe while retracting" is enabled, then extruder moves to another position, and travel from this position can cross perimeters.
        // Because of it, it is necessary to call avoid crossing perimeters again with new starting point after calling retraction()
        // FIXME Lukas H.: Try to predict if this second calling of avoid crossing perimeters will be needed or not. It could save computations.
        if (last_post_before_retract != this->last_pos() && m_config.reduce_crossing_wall) {
            // If in the previous call of m_avoid_crossing_perimeters.travel_to was use_external_mp_once set to true restore this value for next call.
            if (used_external_mp_once)
                m_avoid_crossing_perimeters.use_external_mp_once();
            travel = m_avoid_crossing_perimeters.travel_to(*this, point);
            // If state of use_external_mp_once was changed reset it to right value.
            if (used_external_mp_once)
                m_avoid_crossing_perimeters.reset_once_modifiers();
        }
    } else {
        // Reset the wipe path when traveling, so one would not wipe along an old path.
        m_wipe.reset_path();
        // if (m_config.reduce_crossing_wall) {
        //     // If in the previous call of m_avoid_crossing_perimeters.travel_to was use_external_mp_once set to true restore this value for next call.
        //     if (used_external_mp_once) m_avoid_crossing_perimeters.use_external_mp_once();
        //     travel = m_avoid_crossing_perimeters.travel_to(*this, point);
        //     // If state of use_external_mp_once was changed reset it to right value.
        //     if (used_external_mp_once) m_avoid_crossing_perimeters.reset_once_modifiers();
        // }
    }


    // if needed, write the gcode_label_objects_end then gcode_label_objects_start
    m_writer.add_object_change_labels(gcode);

    // use G1 because we rely on paths being straight (G0 may make round paths)
    if (travel.size() >= 2) {
        // Orca: use `travel_to_xyz` to ensure we start at the correct z, in case we moved z in custom/filament change gcode
        if (false/*m_spiral_vase*/) {
            // No lazy z lift for spiral vase mode
            for (size_t i = 1; i < travel.size(); ++i) {
                gcode += m_writer.travel_to_xy(this->point_to_gcode(travel.points[i]), comment);
            }
        } else {
            if (travel.size() == 2) {
                // No extra movements emitted by avoid_crossing_perimeters, simply move to the end point with z change
                const auto& dest2d = this->point_to_gcode(travel.points.back());
                Vec3d dest3d(dest2d(0), dest2d(1), z == DBL_MAX ? m_nominal_z : z);
                gcode += m_writer.travel_to_xyz(dest3d, comment, m_need_change_layer_lift_z);
                m_need_change_layer_lift_z = false;
            } else {
                // Extra movements emitted by avoid_crossing_perimeters, lift the z to normal height at the beginning, then apply the z
                // ratio at the last point
                for (size_t i = 1; i < travel.size(); ++i) {
                    if (i == 1) {
                        // Lift to normal z at beginning
                        Vec2d dest2d = this->point_to_gcode(travel.points[i]);
                        Vec3d dest3d(dest2d(0), dest2d(1), m_nominal_z);
                        gcode += m_writer.travel_to_xyz(dest3d, comment, m_need_change_layer_lift_z);
                        m_need_change_layer_lift_z = false;
                    } else if (z != DBL_MAX && i == travel.size() - 1) {
                        // Apply z_ratio for the very last point
                        Vec2d dest2d = this->point_to_gcode(travel.points[i]);
                        Vec3d dest3d(dest2d(0), dest2d(1), z);
                        gcode += m_writer.travel_to_xyz(dest3d, comment);
                    } else {
                        // For all points in between, no z change
                        gcode += m_writer.travel_to_xy(this->point_to_gcode(travel.points[i]), comment);
                    }
                }
            }
        }
        this->set_last_pos(travel.points.back());
    }

    return gcode;
}

//BBS
LiftType GCode::to_lift_type(ZHopType z_hop_types) {
    switch (z_hop_types)
    {
    case ZHopType::zhtNormal:
        return LiftType::NormalLift;
    case ZHopType::zhtSlope:
        return LiftType::SlopeLift;
    case ZHopType::zhtSpiral:
        return LiftType::SpiralLift;
    default:
        // if no corresponding lift type, use normal lift
        return LiftType::NormalLift;
    }
};

bool GCode::needs_retraction(const Polyline &travel, ExtrusionRole role, LiftType& lift_type)
{
    if (travel.length() < scale_(FILAMENT_CONFIG(retraction_minimum_travel))) {
        // skip retraction if the move is shorter than the configured threshold
        return false;
    }

    //BBS: input travel polyline must be in current plate coordinate system
    auto is_through_overhang = [this](const Polyline& travel) {
        BoundingBox travel_bbox = get_extents(travel);
        travel_bbox.inflated(1);
        travel_bbox.defined = true;

        // do not scale for z
        const float protect_z = 0.4;
        std::pair<float, float> z_range;
        z_range.second = m_layer ? m_layer->print_z : 0.f;
        z_range.first = std::max(0.f, z_range.second - protect_z);
        std::vector<LayerPtrs> layers_of_objects;
        std::vector<BoundingBox> boundingBox_for_objects;
        VecOfPoints objects_instances_shift;
        std::vector<size_t> idx_of_object_sorted = m_curr_print->layers_sorted_for_object(z_range.first, z_range.second, layers_of_objects, boundingBox_for_objects, objects_instances_shift);

        std::vector<bool> is_layers_of_objects_sorted(layers_of_objects.size(), false);

        for (size_t idx : idx_of_object_sorted) {
            for (const Point & instance_shift : objects_instances_shift[idx]) {
                BoundingBox instance_bbox = boundingBox_for_objects[idx];
                if (!instance_bbox.defined)  //BBS: Don't need to check when bounding box of overhang area is empty(undefined)
                    continue;

                instance_bbox.offset(scale_(EPSILON));
                instance_bbox.translate(instance_shift.x(), instance_shift.y());
                if (!instance_bbox.overlap(travel_bbox))
                    continue;

                Polygons temp;
                temp.emplace_back(std::move(instance_bbox.polygon()));
                if (intersection_pl(travel, temp).empty())
                    continue;

                if (!is_layers_of_objects_sorted[idx]) {
                    std::sort(layers_of_objects[idx].begin(), layers_of_objects[idx].end(), [](auto left, auto right) { return left->loverhangs_bbox.area() > right->loverhangs_bbox.area();});
                    is_layers_of_objects_sorted[idx] = true;
                }

                for (const auto& layer : layers_of_objects[idx]) {
                    for (ExPolygon overhang : layer->loverhangs) {
                        overhang.translate(instance_shift);
                        BoundingBox bbox1 = get_extents(overhang);

                        if (!bbox1.overlap(travel_bbox))
                            continue;

                        if (intersection_pl(travel, overhang).empty())
                            continue;

                        return true;
                    }
                }
            }
        }
        return false;
    };

    float max_z_hop = 0.f;
    for (int i = 0; i < m_config.z_hop.size(); i++)
        max_z_hop = std::max(max_z_hop, (float)m_config.z_hop.get_at(i));
    float travel_len_thresh = scale_(max_z_hop / tan(this->writer().filament()->travel_slope()));
    float accum_len = 0.f;
    Polyline clipped_travel;

    clipped_travel.append(Polyline(travel.points[0], travel.points[1]));
    if (clipped_travel.length() > travel_len_thresh)
        clipped_travel.points.back() = clipped_travel.points.front()+(clipped_travel.points.back() - clipped_travel.points.front()) * (travel_len_thresh / clipped_travel.length());
    //BBS: translate to current plate coordinate system
    clipped_travel.translate(Point::new_scale(double(m_origin.x() - m_writer.get_xy_offset().x()), double(m_origin.y() - m_writer.get_xy_offset().y())));

    //BBS: force to retract when leave from external perimeter for a long travel
    //Better way is judging whether the travel move direction is same with last extrusion move.
    if (is_perimeter(m_last_processor_extrusion_role) && m_last_processor_extrusion_role != erPerimeter) {
        if (ZHopType(FILAMENT_CONFIG(z_hop_types)) == ZHopType::zhtAuto) {
            lift_type = is_through_overhang(clipped_travel) ? LiftType::SpiralLift : LiftType::SlopeLift;
        }
        else {
            lift_type = to_lift_type(ZHopType(FILAMENT_CONFIG(z_hop_types)));
        }
        return true;
    }

    if (role == erSupportMaterial || role == erSupportTransition) {
        const SupportLayer* support_layer = dynamic_cast<const SupportLayer*>(m_layer);
        //FIXME support_layer->support_islands.contains should use some search structure!
        if (support_layer != NULL)
            // skip retraction if this is a travel move inside a support material island
            //FIXME not retracting over a long path may cause oozing, which in turn may result in missing material
            // at the end of the extrusion path!
            for (const ExPolygon& support_island : support_layer->support_islands)
                if (support_island.contains(travel))
                    return false;
        //reduce the retractions in lightning infills for tree support
        if (support_layer != NULL && support_layer->support_type==stInnerTree)
            for (auto &area : support_layer->base_areas)
                if (area.contains(travel))
                    return false;
    }
    //BBS: need retract when long moving to print perimeter to avoid dropping of material
    if (!is_perimeter(role) && m_config.reduce_infill_retraction && m_layer != nullptr &&
        m_config.sparse_infill_density.value > 0 && m_retract_when_crossing_perimeters.travel_inside_internal_regions(*m_layer, travel))
        // Skip retraction if travel is contained in an internal slice *and*
        // internal infill is enabled (so that stringing is entirely not visible).
        //FIXME any_internal_region_slice_contains() is potentionally very slow, it shall test for the bounding boxes first.
        return false;

    // retract if reduce_infill_retraction is disabled or doesn't apply when role is perimeter
    if (ZHopType(FILAMENT_CONFIG(z_hop_types)) == ZHopType::zhtAuto) {
        lift_type = is_through_overhang(clipped_travel) ? LiftType::SpiralLift : LiftType::SlopeLift;
    }
    else {
        lift_type = to_lift_type(ZHopType(FILAMENT_CONFIG(z_hop_types)));
    }
    return true;
}

std::string GCode::retract(bool toolchange, bool is_last_retraction, LiftType lift_type, bool apply_instantly, ExtrusionRole role)
{
    std::string gcode;

    if (m_writer.filament() == nullptr)
        return gcode;

    // wipe (if it's enabled for this extruder and we have a stored wipe path and no-zero wipe distance)
    if (FILAMENT_CONFIG(wipe) && m_wipe.has_path() && scale_(FILAMENT_CONFIG(wipe_distance)) > SCALED_EPSILON) {
        Wipe::RetractionValues wipeRetractions = m_wipe.calculateWipeRetractionLengths(*this, toolchange);
        gcode += toolchange ? m_writer.retract_for_toolchange(true,wipeRetractions.retractLengthBeforeWipe) : m_writer.retract(true, wipeRetractions.retractLengthBeforeWipe);
        gcode += m_wipe.wipe(*this,wipeRetractions.retractLengthDuringWipe, toolchange, is_last_retraction);
    }

    /*  The parent class will decide whether we need to perform an actual retraction
        (the extruder might be already retracted fully or partially). We call these
        methods even if we performed wipe, since this will ensure the entire retraction
        length is honored in case wipe path was too short.  */
    if ((!this->on_first_layer()  || this->config().bottom_surface_pattern != InfillPattern::ipHilbertCurve) &&
	    (role != erTopSolidInfill || this->config().top_surface_pattern    != InfillPattern::ipHilbertCurve))
        gcode += toolchange ? m_writer.retract_for_toolchange() : m_writer.retract();

    gcode += m_writer.reset_e();
    // Orca: check if should + can lift (roughly from SuperSlicer)
    RetractLiftEnforceType retract_lift_type = RetractLiftEnforceType(EXTRUDER_CONFIG(retract_lift_enforce));

    bool needs_lift = toolchange
        || m_writer.filament()->retraction_length() > 0
        || m_config.use_firmware_retraction;

    bool last_fill_extrusion_role_top_infill = (this->m_last_notgapfill_extrusion_role == ExtrusionRole::erTopSolidInfill || this->m_last_notgapfill_extrusion_role == ExtrusionRole::erIroning);

    // assume we can lift on retraction; conditions left explicit 
    bool can_lift = true;

    if (retract_lift_type == RetractLiftEnforceType::rletAllSurfaces) {
        can_lift = true;
    }
    else if (this->m_layer_index == 0 && (retract_lift_type == RetractLiftEnforceType::rletBottomOnly || retract_lift_type == RetractLiftEnforceType::rletTopAndBottom)) {
        can_lift = true;
    }
    else if (retract_lift_type == RetractLiftEnforceType::rletTopOnly || retract_lift_type == RetractLiftEnforceType::rletTopAndBottom) {
        can_lift = last_fill_extrusion_role_top_infill;
    }
    else {
        can_lift = false;
    }

    if (needs_lift && can_lift) {
        if (apply_instantly)
            gcode += m_writer.eager_lift(lift_type);
        else
            gcode += m_writer.lazy_lift(lift_type, m_spiral_vase != nullptr);
    }

    return gcode;
}

std::string GCode::set_extruder(unsigned int new_filament_id, double print_z, bool by_object)
{
    int new_extruder_id = get_extruder_id(new_filament_id);
    if (!m_writer.need_toolchange(new_filament_id))
        return "";

    // if we are running a single-extruder setup, just set the extruder and return nothing
    if (!m_writer.multiple_extruders) {
        this->placeholder_parser().set("current_extruder", new_filament_id);
        this->placeholder_parser().set("retraction_distance_when_ec", m_config.retraction_distances_when_ec.get_at(new_filament_id));
        this->placeholder_parser().set("long_retraction_when_ec", m_config.long_retractions_when_ec.get_at(new_filament_id));

        std::string gcode;
        // Append the filament start G-code.
        const std::string &filament_start_gcode = m_config.filament_start_gcode.get_at(new_filament_id);
        if (! filament_start_gcode.empty()) {
            // Process the filament_start_gcode for the filament.
            DynamicConfig config;
            config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
            config.set_key_value("layer_z", new ConfigOptionFloat(this->writer().get_position().z() - m_config.z_offset.value));
            config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
            config.set_key_value("filament_extruder_id", new ConfigOptionInt(int(new_filament_id)));
            config.set_key_value("retraction_distance_when_cut",
                                 new ConfigOptionFloat(m_config.retraction_distances_when_cut.get_at(new_filament_id)));
            config.set_key_value("long_retraction_when_cut", new ConfigOptionBool(m_config.long_retractions_when_cut.get_at(new_filament_id)));

            gcode += this->placeholder_parser_process("filament_start_gcode", filament_start_gcode, new_filament_id, &config);
            check_add_eol(gcode);
        }
        if (m_config.enable_pressure_advance.get_at(new_filament_id)) {
            gcode += m_writer.set_pressure_advance(m_config.pressure_advance.get_at(new_filament_id));
            // Orca: Adaptive PA
            // Reset Adaptive PA processor last PA value
            m_pa_processor->resetPreviousPA(m_config.pressure_advance.get_at(new_filament_id));
        }

        gcode += m_writer.toolchange(new_filament_id);
        return gcode;
    }

    // BBS. Should be placed before retract.
    m_toolchange_count++;

    // prepend retraction on the current extruder
    std::string gcode = this->retract(true, false);

    // Always reset the extrusion path, even if the tool change retract is set to zero.
    m_wipe.reset_path();

    // BBS: insert skip object label before change filament while by object
    if (by_object)
        m_writer.add_object_change_labels(gcode);

    bool add_change_filament_624 = false;
    if (m_writer.filament() != nullptr) {
        // Process the custom filament_end_gcode. set_extruder() is only called if there is no wipe tower
        // so it should not be injected twice.
        unsigned int        old_filament_id = m_writer.filament()->id();
        const std::string  &filament_end_gcode  = m_config.filament_end_gcode.get_at(old_filament_id);
        if (! filament_end_gcode.empty()) {
            DynamicConfig config;
            config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
            config.set_key_value("layer_z",   new ConfigOptionFloat(m_writer.get_position().z() - m_config.z_offset.value));
            config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
            config.set_key_value("filament_extruder_id", new ConfigOptionInt(int(get_extruder_id(old_filament_id))));
            if (!m_filament_instances_code.empty()) {
                gcode += ("M624 " + m_filament_instances_code + "\n");
                m_filament_instances_code = "";
                add_change_filament_624   = true;
            }
            gcode += placeholder_parser_process("filament_end_gcode", filament_end_gcode, old_filament_id, &config);
            check_add_eol(gcode);
        }
    }


    // If ooze prevention is enabled, park current extruder in the nearest
    // standby point and set it to the standby temperature.
    if (m_ooze_prevention.enable && m_writer.filament() != nullptr)
        gcode += m_ooze_prevention.pre_toolchange(*this);

    // BBS
    float new_retract_length = m_config.retraction_length.get_at(new_filament_id);
    float new_retract_length_toolchange = m_config.retract_length_toolchange.get_at(new_filament_id);
    int new_filament_temp = this->on_first_layer() ? m_config.nozzle_temperature_initial_layer.get_at(new_filament_id) : m_config.nozzle_temperature.get_at(new_filament_id);
    // BBS: if print_z == 0 use first layer temperature
    if (abs(print_z) < EPSILON)
        new_filament_temp = m_config.nozzle_temperature_initial_layer.get_at(new_filament_id);

    Vec3d nozzle_pos = m_writer.get_position();
    float old_retract_length, old_retract_length_toolchange, wipe_volume;
    int old_filament_temp, old_filament_e_feedrate;

    float filament_area = float((M_PI / 4.f) * pow(m_config.filament_diameter.get_at(new_filament_id), 2));
    //BBS: add handling for filament change in start gcode
    int old_filament_id = -1;
    if (m_writer.filament() != nullptr || m_start_gcode_filament != -1) {
        std::vector<float> flush_matrix(cast<float>(get_flush_volumes_matrix(m_config.flush_volumes_matrix.values, new_extruder_id, m_config.nozzle_diameter.values.size())));
        const unsigned int number_of_extruders = (unsigned int) (m_config.filament_colour.values.size()); // if is multi_extruder only use the fist extruder matrix
        if (m_writer.filament() != nullptr)
            assert(m_writer.filament()->id() < number_of_extruders);
        else
            assert(m_start_gcode_filament < number_of_extruders);

        old_filament_id = m_writer.filament() != nullptr ? m_writer.filament()->id() : m_start_gcode_filament;
        int old_extruder_id = m_writer.filament() != nullptr ? m_writer.filament()->extruder_id() : get_extruder_id(m_start_gcode_filament);

        old_retract_length = m_config.retraction_length.get_at(old_filament_id);
        old_retract_length_toolchange = m_config.retract_length_toolchange.get_at(old_filament_id);
        old_filament_temp = this->on_first_layer()? m_config.nozzle_temperature_initial_layer.get_at(old_filament_id) : m_config.nozzle_temperature.get_at(old_filament_id);

        //During the filament change, the extruder will extrude an extra length of grab_length for the corresponding detection, so the purge can reduce this length.
        float grab_purge_volume = m_config.grab_length.get_at(new_extruder_id) * 2.4;
        if (old_extruder_id != new_extruder_id) {
            //calc flush volume between the same extruder id
            int old_filament_id_in_new_extruder = m_writer.filament(new_extruder_id) != nullptr ? m_writer.filament(new_extruder_id)->id() : -1;
            if (old_filament_id_in_new_extruder == -1)
                wipe_volume = 0;
            else {
                wipe_volume = flush_matrix[old_filament_id_in_new_extruder * number_of_extruders + new_filament_id];
                wipe_volume *= m_config.flush_multiplier.get_at(new_extruder_id);
            }
        }
        else {
            wipe_volume = flush_matrix[old_filament_id * number_of_extruders + new_filament_id];
            wipe_volume *= m_config.flush_multiplier.get_at(new_extruder_id);  // if is multi_extruder only use the fist extruder matrix
        }
        wipe_volume = std::max(0.f, wipe_volume-grab_purge_volume);

        old_filament_e_feedrate = (int) (60.0 * m_config.filament_max_volumetric_speed.get_at(old_filament_id) / filament_area);
        old_filament_e_feedrate = old_filament_e_feedrate == 0 ? 100 : old_filament_e_feedrate;
        //BBS: must clean m_start_gcode_filament
        m_start_gcode_filament = -1;
    } else {
        old_retract_length = 0.f;
        old_retract_length_toolchange = 0.f;
        old_filament_temp = 0;
        wipe_volume = 0.f;
        old_filament_e_feedrate = 200;
    }
    float wipe_length = wipe_volume / filament_area;
    int new_filament_e_feedrate = (int)(60.0 * m_config.filament_max_volumetric_speed.get_at(new_filament_id) / filament_area);
    new_filament_e_feedrate = new_filament_e_feedrate == 0 ? 100 : new_filament_e_feedrate;

    // set volumetric speed of outer wall ,ignore per obejct,just use default setting
    float outer_wall_volumetric_speed = get_outer_wall_volumetric_speed(m_config, *m_print, new_filament_id, get_extruder_id(new_filament_id));
    float         wipe_avoid_pos_x            = 110.f;
    DynamicConfig dyn_config;
    dyn_config.set_key_value("outer_wall_volumetric_speed", new ConfigOptionFloat(outer_wall_volumetric_speed));
    dyn_config.set_key_value("previous_extruder", new ConfigOptionInt(old_filament_id));
    dyn_config.set_key_value("next_extruder", new ConfigOptionInt((int)new_filament_id));
    dyn_config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
    dyn_config.set_key_value("layer_z", new ConfigOptionFloat(print_z));
    dyn_config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
    dyn_config.set_key_value("relative_e_axis", new ConfigOptionBool(m_config.use_relative_e_distances));
    dyn_config.set_key_value("toolchange_count", new ConfigOptionInt((int)m_toolchange_count));
    //BBS: fan speed is useless placeholer now, but we don't remove it to avoid
    //slicing error in old change_filament_gcode in old 3MF
    dyn_config.set_key_value("fan_speed", new ConfigOptionInt((int)0));
    dyn_config.set_key_value("old_retract_length", new ConfigOptionFloat(old_retract_length));
    dyn_config.set_key_value("new_retract_length", new ConfigOptionFloat(new_retract_length));
    dyn_config.set_key_value("old_retract_length_toolchange", new ConfigOptionFloat(old_retract_length_toolchange));
    dyn_config.set_key_value("new_retract_length_toolchange", new ConfigOptionFloat(new_retract_length_toolchange));
    dyn_config.set_key_value("old_filament_temp", new ConfigOptionInt(old_filament_temp));
    dyn_config.set_key_value("new_filament_temp", new ConfigOptionInt(new_filament_temp));
    dyn_config.set_key_value("x_after_toolchange", new ConfigOptionFloat(nozzle_pos(0)));
    dyn_config.set_key_value("y_after_toolchange", new ConfigOptionFloat(nozzle_pos(1)));
    dyn_config.set_key_value("z_after_toolchange", new ConfigOptionFloat(nozzle_pos(2)));
    dyn_config.set_key_value("first_flush_volume", new ConfigOptionFloat(wipe_length / 2.f));
    dyn_config.set_key_value("second_flush_volume", new ConfigOptionFloat(wipe_length / 2.f));
    dyn_config.set_key_value("old_filament_e_feedrate", new ConfigOptionInt(old_filament_e_feedrate));
    dyn_config.set_key_value("new_filament_e_feedrate", new ConfigOptionInt(new_filament_e_feedrate));
    dyn_config.set_key_value("travel_point_1_x", new ConfigOptionFloat(float(travel_point_1.x())));
    dyn_config.set_key_value("travel_point_1_y", new ConfigOptionFloat(float(travel_point_1.y())));
    dyn_config.set_key_value("travel_point_2_x", new ConfigOptionFloat(float(travel_point_2.x())));
    dyn_config.set_key_value("travel_point_2_y", new ConfigOptionFloat(float(travel_point_2.y())));
    dyn_config.set_key_value("travel_point_3_x", new ConfigOptionFloat(float(travel_point_3.x())));
    dyn_config.set_key_value("travel_point_3_y", new ConfigOptionFloat(float(travel_point_3.y())));
    dyn_config.set_key_value("wipe_avoid_perimeter", new ConfigOptionBool(false));
    dyn_config.set_key_value("wipe_avoid_pos_x", new ConfigOptionFloat(wipe_avoid_pos_x));

    auto flush_v_speed = m_print->config().filament_flush_volumetric_speed.values;
    auto flush_temps =m_print->config().filament_flush_temp.values;
    for (size_t idx = 0; idx < flush_v_speed.size(); ++idx) {
        if (flush_v_speed[idx] == 0)
            flush_v_speed[idx] = m_print->config().filament_max_volumetric_speed.get_at(idx);
    }
    for (size_t idx = 0; idx < flush_temps.size(); ++idx) {
        if (flush_temps[idx] == 0)
            flush_temps[idx] = m_print->config().nozzle_temperature_range_high.get_at(idx);
    }
    dyn_config.set_key_value("flush_volumetric_speeds", new ConfigOptionFloats(flush_v_speed));
    dyn_config.set_key_value("flush_temperatures", new ConfigOptionInts(flush_temps));
    dyn_config.set_key_value("flush_length", new ConfigOptionFloat(wipe_length));

    int flush_count = std::min(g_max_flush_count, (int)std::round(wipe_volume / g_purge_volume_one_time));
    float flush_unit = wipe_length / flush_count;
    int flush_idx = 0;
    for (; flush_idx < flush_count; flush_idx++) {
        char key_value[64] = { 0 };
        snprintf(key_value, sizeof(key_value), "flush_length_%d", flush_idx + 1);
        dyn_config.set_key_value(key_value, new ConfigOptionFloat(flush_unit));
    }

    for (; flush_idx < g_max_flush_count; flush_idx++) {
        char key_value[64] = { 0 };
        snprintf(key_value, sizeof(key_value), "flush_length_%d", flush_idx + 1);
        dyn_config.set_key_value(key_value, new ConfigOptionFloat(0.f));
    }

    // Process the custom change_filament_gcode.
    std::string change_filament_gcode = m_config.change_filament_gcode.value;

    // Move the lift gcode here which is in the change_filament_gcode originally
    change_filament_gcode = this->retract(false, false, LiftType::SpiralLift, true) + change_filament_gcode;

    std::string toolchange_gcode_parsed;
    //Orca: Ignore change_filament_gcode if is the first call for a tool change and manual_filament_change is enabled
    if (!change_filament_gcode.empty() && !(m_config.manual_filament_change.value && m_toolchange_count == 1)) {
        dyn_config.set_key_value("toolchange_z", new ConfigOptionFloat(print_z));

        toolchange_gcode_parsed = placeholder_parser_process("change_filament_gcode", change_filament_gcode, new_filament_id, &dyn_config);
        check_add_eol(toolchange_gcode_parsed);
        gcode += toolchange_gcode_parsed;

        //BBS
        {
            //BBS: gcode writer doesn't know where the extruder is and whether fan speed is changed after inserting tool change gcode
            //Set this flag so that normal lift will be used the first time after tool change.
            gcode += ";_FORCE_RESUME_FAN_SPEED\n";
            m_writer.set_current_position_clear(false);
            //BBS: check whether custom gcode changes the z position. Update if changed
            double temp_z_after_tool_change;
            if (GCodeProcessor::get_last_z_from_gcode(toolchange_gcode_parsed, temp_z_after_tool_change)) {
                Vec3d pos = m_writer.get_position();
                pos(2) = temp_z_after_tool_change;
                m_writer.set_position(pos);
            }
        }
    }

    // BBS. Reset old extruder E-value.
    // Keep retract length because Custom GCode will guarantee retract length be the same as toolchange
    if (m_config.single_extruder_multi_material) {
        m_writer.reset_e();
    }

    //BBS: don't add T[next extruder] if there is no T cmd on filament change
     //We inform the writer about what is happening, but we may not use the resulting gcode.
    std::string toolchange_command = m_writer.toolchange(new_filament_id);
    if (!custom_gcode_changes_tool(toolchange_gcode_parsed, m_writer.toolchange_prefix(), new_filament_id))
        gcode += toolchange_command;
    else {
        // user provided his own toolchange gcode, no need to do anything
    }

    // Set the temperature if the wipe tower didn't (not needed for non-single extruder MM)
    if (m_config.single_extruder_multi_material && !m_config.enable_prime_tower) {
        int temp = (m_layer_index <= 0 ? m_config.nozzle_temperature_initial_layer.get_at(new_filament_id) :
                                         m_config.nozzle_temperature.get_at(new_filament_id));

        gcode += m_writer.set_temperature(temp, false);
    }

    this->placeholder_parser().set("current_extruder", new_filament_id);
    this->placeholder_parser().set("retraction_distance_when_cut", m_config.retraction_distances_when_cut.get_at(new_filament_id));
    this->placeholder_parser().set("long_retraction_when_cut", m_config.long_retractions_when_cut.get_at(new_filament_id));
    this->placeholder_parser().set("retraction_distance_when_ec", m_config.retraction_distances_when_ec.get_at(new_filament_id));
    this->placeholder_parser().set("long_retraction_when_ec", m_config.long_retractions_when_ec.get_at(new_filament_id));


    // Append the filament start G-code.
    const std::string &filament_start_gcode = m_config.filament_start_gcode.get_at(new_filament_id);
    if (! filament_start_gcode.empty()) {
        // Process the filament_start_gcode for the new filament.
        DynamicConfig config;
        config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
        config.set_key_value("layer_z", new ConfigOptionFloat(this->writer().get_position().z() - m_config.z_offset.value));
        config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
        config.set_key_value("filament_extruder_id", new ConfigOptionInt(int(new_filament_id)));
        gcode += this->placeholder_parser_process("filament_start_gcode", filament_start_gcode, new_filament_id, &config);
        if (add_change_filament_624) {
            gcode += "M625\n";
            add_change_filament_624 = false;
        }
        check_add_eol(gcode);
    }
    // Set the new extruder to the operating temperature.
    if (m_ooze_prevention.enable)
        gcode += m_ooze_prevention.post_toolchange(*this);

    if (m_config.enable_pressure_advance.get_at(new_filament_id)) {
        gcode += m_writer.set_pressure_advance(m_config.pressure_advance.get_at(new_filament_id));
        // Orca: Adaptive PA
        // Reset Adaptive PA processor last PA value
        m_pa_processor->resetPreviousPA(m_config.pressure_advance.get_at(new_filament_id));
    }
    //Orca: tool changer or IDEX's firmware may change Z position, so we set it to unknown/undefined
    m_last_pos_defined = false;

    return gcode;
}

inline std::string polygon_to_string(const Polygon &polygon, Print *print, bool is_print_space = false) {
    std::ostringstream gcode;
    gcode << "[";
    for (const Point &p : polygon.points) {
        const auto v = is_print_space ? Vec2d(p.x(), p.y()) : print->translate_to_print_space(p);
        gcode << "[" << v.x() << "," << v.y() << "],";
    }
    const auto first_v = is_print_space ? Vec2d(polygon.points.front().x(), polygon.points.front().y())
                                        : print->translate_to_print_space(polygon.points.front());
    gcode << "[" << first_v.x() << "," << first_v.y() << "]";
    gcode << "]";
    return gcode.str();
}
// this function iterator PrintObject and assign a seqential id to each object.
// this id is used to generate unique object id for each object.
std::string GCode::set_object_info(Print *print) {
    const auto gflavor = print->config().gcode_flavor.value;
    if (print->is_BBL_printer() ||
        (gflavor != gcfKlipper && gflavor != gcfMarlinLegacy && gflavor != gcfMarlinFirmware && gflavor != gcfRepRapFirmware))
        return "";
    std::ostringstream gcode;
    size_t object_id = 0;
    // Orca: check if we are in pa calib mode
    if (print->calib_mode() == CalibMode::Calib_PA_Line || print->calib_mode() == CalibMode::Calib_PA_Pattern) {
        BoundingBoxf bbox_bed(print->config().printable_area.values);
        bbox_bed.offset(-25.0);
        Polygon polygon_bed;
        polygon_bed.append(Point(bbox_bed.min.x(), bbox_bed.min.y()));
        polygon_bed.append(Point(bbox_bed.max.x(), bbox_bed.min.y()));
        polygon_bed.append(Point(bbox_bed.max.x(), bbox_bed.max.y()));
        polygon_bed.append(Point(bbox_bed.min.x(), bbox_bed.max.y()));
        gcode << "EXCLUDE_OBJECT_DEFINE NAME="
              << "Orca-PA-Calibration-Test"
              << " CENTER=" << 0 << "," << 0 << " POLYGON=" << polygon_to_string(polygon_bed, print, true) << "\n";
    } else {
        size_t unique_id = 0;
        for (PrintObject* object : print->objects()) {
            object->set_id(object_id++);
            size_t inst_id = 0;
            for (PrintInstance& inst : object->instances()) {
                inst.unique_id = unique_id++;
                inst.id        = inst_id++;
                auto bbox      = inst.get_bounding_box();
                auto center    = print->translate_to_print_space(Vec2d(bbox.center().x(), bbox.center().y()));
                auto inst_name = get_instance_name(object, inst);
                if (gflavor == gcfKlipper) {
                    gcode << "EXCLUDE_OBJECT_DEFINE NAME=" << inst_name << " CENTER=" << center.x() << "," << center.y()
                          << " POLYGON=" << polygon_to_string(inst.get_convex_hull_2d(), print) << "\n";
                } else if (gflavor == gcfMarlinLegacy || gflavor == gcfMarlinFirmware || gflavor == gcfRepRapFirmware) {
                    gcode << "M486 S" << std::to_string(inst.unique_id);
                    if (gflavor == gcfRepRapFirmware)
                        gcode << " A"
                              << "\"" << inst_name << "\"";
                    else
                        gcode << "\nM486 A" << inst_name;
                    gcode << "\nM486 S-1\n";
                }
            }
        }
    }

    return gcode.str();
}

// convert a model-space scaled point into G-code coordinates
Vec2d GCode::point_to_gcode(const Point &point) const
{
    Vec2d extruder_offset = EXTRUDER_CONFIG(extruder_offset);
    return unscale(point) + m_origin - extruder_offset;
}

// convert a model-space scaled point into G-code coordinates
Point GCode::gcode_to_point(const Vec2d &point) const
{
    Vec2d pt = point - m_origin;
    if (const Extruder *extruder = m_writer.filament(); extruder)
        // This function may be called at the very start from toolchange G-code when the extruder is not assigned yet.
        pt += m_config.extruder_offset.get_at(extruder->extruder_id());
    return scaled<coord_t>(pt);
        
}

Vec2d GCode::point_to_gcode_quantized(const Point& point) const
{
    Vec2d p = this->point_to_gcode(point);
    return { GCodeFormatter::quantize_xyzf(p.x()), GCodeFormatter::quantize_xyzf(p.y()) };
}


// Goes through by_region std::vector and returns reference to a subvector of entities, that are to be printed
// during infill/perimeter wiping, or normally (depends on wiping_entities parameter)
// Fills in by_region_per_copy_cache and returns its reference.
const std::vector<GCode::ObjectByExtruder::Island::Region>& GCode::ObjectByExtruder::Island::by_region_per_copy(std::vector<Region> &by_region_per_copy_cache, unsigned int copy, unsigned int extruder, bool wiping_entities) const
{
    bool has_overrides = false;
    for (const auto& reg : by_region)
        if (! reg.infills_overrides.empty() || ! reg.perimeters_overrides.empty()) {
            has_overrides = true;
            break;
        }

    // Data is cleared, but the memory is not.
    by_region_per_copy_cache.clear();

    if (! has_overrides)
        // Simple case. No need to copy the regions.
        return wiping_entities ? by_region_per_copy_cache : this->by_region;

    // Complex case. Some of the extrusions of some object instances are to be printed first - those are the wiping extrusions.
    // Some of the extrusions of some object instances are printed later - those are the clean print extrusions.
    // Filter out the extrusions based on the infill_overrides / perimeter_overrides:

    for (const auto& reg : by_region) {
        by_region_per_copy_cache.emplace_back(); // creates a region in the newly created Island

        // Now we are going to iterate through perimeters and infills and pick ones that are supposed to be printed
        // References are used so that we don't have to repeat the same code
        for (int iter = 0; iter < 2; ++iter) {
            const ExtrusionEntitiesPtr&										entities    = (iter ? reg.infills : reg.perimeters);
            ExtrusionEntitiesPtr&   										target_eec  = (iter ? by_region_per_copy_cache.back().infills : by_region_per_copy_cache.back().perimeters);
            const std::vector<const WipingExtrusions::ExtruderPerCopy*>& 	overrides   = (iter ? reg.infills_overrides : reg.perimeters_overrides);

            // Now the most important thing - which extrusion should we print.
            // See function ToolOrdering::get_extruder_overrides for details about the negative numbers hack.
            if (wiping_entities) {
                // Apply overrides for this region.
                for (unsigned int i = 0; i < overrides.size(); ++ i) {
                    const WipingExtrusions::ExtruderPerCopy *this_override = overrides[i];
                    // This copy (aka object instance) should be printed with this extruder, which overrides the default one.
                    if (this_override != nullptr && (*this_override)[copy] == int(extruder))
                        target_eec.emplace_back(entities[i]);
                }
            } else {
                // Apply normal extrusions (non-overrides) for this region.
                unsigned int i = 0;
                for (; i < overrides.size(); ++ i) {
                    const WipingExtrusions::ExtruderPerCopy *this_override = overrides[i];
                    // This copy (aka object instance) should be printed with this extruder, which shall be equal to the default one.
                    if (this_override == nullptr || (*this_override)[copy] == -int(extruder)-1)
                        target_eec.emplace_back(entities[i]);
                }
                for (; i < entities.size(); ++ i)
                    target_eec.emplace_back(entities[i]);
            }
        }
    }
    return by_region_per_copy_cache;
}

// This function takes the eec and appends its entities to either perimeters or infills of this Region (depending on the first parameter)
// It also saves pointer to ExtruderPerCopy struct (for each entity), that holds information about which extruders should be used for which copy.
void GCode::ObjectByExtruder::Island::Region::append(const Type type, const ExtrusionEntityCollection* eec, const WipingExtrusions::ExtruderPerCopy* copies_extruder)
{
    // We are going to manipulate either perimeters or infills, exactly in the same way. Let's create pointers to the proper structure to not repeat ourselves:
    ExtrusionEntitiesPtr*									perimeters_or_infills;
    std::vector<const WipingExtrusions::ExtruderPerCopy*>* 	perimeters_or_infills_overrides;

    switch (type) {
    case PERIMETERS:
        perimeters_or_infills 			= &perimeters;
        perimeters_or_infills_overrides = &perimeters_overrides;
        break;
    case INFILL:
        perimeters_or_infills 			= &infills;
        perimeters_or_infills_overrides = &infills_overrides;
        break;
    default:
    	throw Slic3r::InvalidArgument("Unknown parameter!");
    }

    // First we append the entities, there are eec->entities.size() of them:
    size_t old_size = perimeters_or_infills->size();
    size_t new_size = old_size + (eec->can_sort() ? eec->entities.size() : 1);
    perimeters_or_infills->reserve(new_size);
    if (eec->can_sort()) {
        for (auto* ee : eec->entities)
            perimeters_or_infills->emplace_back(ee);
    } else
        perimeters_or_infills->emplace_back(const_cast<ExtrusionEntityCollection*>(eec));

    if (copies_extruder != nullptr) {
        // Don't reallocate overrides if not needed.
        // Missing overrides are implicitely considered non-overridden.
        perimeters_or_infills_overrides->reserve(new_size);
        perimeters_or_infills_overrides->resize(old_size, nullptr);
        perimeters_or_infills_overrides->resize(new_size, copies_extruder);
    }
}

// Index into std::vector<LayerToPrint>, which contains Object and Support layers for the current print_z, collected for
// a single object, or for possibly multiple objects with multiple instances.


} // namespace Slic3r
