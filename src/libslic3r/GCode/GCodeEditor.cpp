#include "../GCode.hpp"
#include "GCodeEditor.hpp"
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/log/trivial.hpp>
#include <iostream>
#include <float.h>

#if 0
    #define DEBUG
    #define _DEBUG
    #undef NDEBUG
#endif

#include <assert.h>

namespace Slic3r {

GCodeEditor::GCodeEditor(GCode &gcodegen) : m_config(gcodegen.config()), m_toolchange_prefix(gcodegen.writer().toolchange_prefix()), m_current_extruder(0)
{
    this->reset(gcodegen.writer().get_position());

    const std::vector<Extruder> &extruders = gcodegen.writer().extruders();
    m_extruder_ids.reserve(extruders.size());
    for (const Extruder &ex : extruders) {
        m_num_extruders = std::max(ex.id() + 1, m_num_extruders);
        m_extruder_ids.emplace_back(ex.id());
    }
}

void GCodeEditor::reset(const Vec3d &position)
{
    // BBS: add I and J axis to store center of arc
    m_current_pos.assign(7, 0.f);
    m_current_pos[0] = float(position.x());
    m_current_pos[1] = float(position.y());
    m_current_pos[2] = float(position.z());
    m_current_pos[4] = float(m_config.travel_speed.get_at(get_config_idx_for_filament(m_config, m_current_extruder)));
    m_fan_speed = -1;
    m_additional_fan_speed = -1;
    m_current_fan_speed = -1;
}

static void record_wall_lines(bool &flag, int &line_idx, PerExtruderAdjustments *adjustment, const std::pair<int, int> &node_pos)
{
    if (flag && line_idx < adjustment->lines.size()) {
        CoolingLine &ptr        = adjustment->lines[line_idx];
        ptr.outwall_smooth_mark = true;
        ptr.object_id           = node_pos.first;
        ptr.cooling_node_id     = node_pos.second;
        flag                    = false;
    }
}

static void mark_node_pos(
    bool &flag, int &line_idx, std::pair<int, int> &node_pos, const std::vector<int> &object_label, int cooling_node_id, int object_id, PerExtruderAdjustments *adjustment)
{
    for (size_t object_idx = 0; object_idx < object_label.size(); ++object_idx) {
        if (object_label[object_idx] == object_id) {
            if (cooling_node_id == -1) break;
            line_idx        = adjustment->lines.size();
            flag            = true;
            node_pos.first  = object_idx;
            node_pos.second = cooling_node_id;
            break;
        }
    }
}


std::string GCodeEditor::process_layer(std::string &&                       gcode,
                                         bool&                               not_set_additional_fan,
                                         const size_t                         layer_id,
                                         std::vector<PerExtruderAdjustments> &per_extruder_adjustments,
                                         const std::vector<int> &             object_label,
                                         const bool                           flush,
                                         const bool                           spiral_vase)
{
    // Cache the input G-code.
    if (m_gcode.empty())
        m_gcode = std::move(gcode);
    else
        m_gcode += gcode;

    std::string out;
    if (flush) {
        // This is either an object layer or the very last print layer. Calculate cool down over the collected support layers
        // and one object layer.
        // record parse gcode info to per_extruder_adjustments
        per_extruder_adjustments = this->parse_layer_gcode(m_gcode, not_set_additional_fan, m_current_pos, object_label, spiral_vase, layer_id > 0);
        out = m_gcode;
        m_gcode.clear();
    }
    return out;
}

//native-resource://sandbox_fs/webcontent/resource/assets/img/41ecc25c56.png
// Parse the layer G-code for the moves, which could be adjusted.
// Return the list of parsed lines, bucketed by an extruder.
std::vector<PerExtruderAdjustments> GCodeEditor::parse_layer_gcode(  const                           std::string &gcode,
                                                                     bool&                           not_set_additional_fan,
                                                                     std::vector<float> &            current_pos,
                                                                     const std::vector<int> &        object_label,
                                                                     bool                            spiral_vase,
                                                                     bool                            join_z_smooth)
{
    std::vector<PerExtruderAdjustments> per_extruder_adjustments(m_extruder_ids.size());
    std::vector<size_t>                 map_extruder_to_per_extruder_adjustment(m_num_extruders, 0);
    for (size_t i = 0; i < m_extruder_ids.size(); ++ i) {
        PerExtruderAdjustments &adj         = per_extruder_adjustments[i];
        unsigned int            extruder_id = m_extruder_ids[i];
        adj.extruder_id               = extruder_id;
        adj.cooling_slow_down_enabled = m_config.slow_down_for_layer_cooling.get_at(extruder_id);
        adj.slow_down_layer_time = float(m_config.slow_down_layer_time.get_at(extruder_id));
        adj.slow_down_min_speed           = float(m_config.slow_down_min_speed.get_at(extruder_id));

        // Read ConsistentSurface cooling settings
        if (m_config.cooling_slowdown_logic.values.size() > extruder_id)
            adj.cooling_slowdown_logic = static_cast<CoolingSlowdownLogicType>(m_config.cooling_slowdown_logic.values[extruder_id]);
        if (m_config.cooling_perimeter_transition_distance.values.size() > extruder_id)
            adj.cooling_perimeter_transition_distance = float(m_config.cooling_perimeter_transition_distance.values[extruder_id]);

        map_extruder_to_per_extruder_adjustment[extruder_id] = i;
    }

    unsigned int      current_extruder  = m_parse_gcode_extruder;
    PerExtruderAdjustments *adjustment  = &per_extruder_adjustments[map_extruder_to_per_extruder_adjustment[current_extruder]];
    const char       *line_start = gcode.c_str();
    const char       *line_end   = line_start;
    // Index of an existing CoolingLine of the current adjustment, which holds the feedrate setting command
    // for a sequence of extrusion moves.
    size_t            active_speed_modifier = size_t(-1);
    int object_id = -1;
    int cooling_node_id = -1;
    std::string object_id_string = "; OBJECT_ID: ";
    std::string cooling_node_label = "; COOLING_NODE: ";
    bool append_wall_ptr = false;
    bool append_inner_wall_ptr = false;
    bool not_join_cooling = false;
    std::pair<int, int> node_pos;
    int line_idx = -1;
    for (; *line_start != 0; line_start = line_end)
    {
        while (*line_end != '\n' && *line_end != 0)
            ++ line_end;
        // sline will not contain the trailing '\n'.
        std::string sline(line_start, line_end);
        // CoolingLine will contain the trailing '\n'.
        if (*line_end == '\n')
            ++ line_end;
        CoolingLine line(0, line_start - gcode.c_str(), line_end - gcode.c_str());
        if (boost::starts_with(sline, "G0 "))
            line.type = CoolingLine::TYPE_G0;
        else if (boost::starts_with(sline, "G1 "))
            line.type = CoolingLine::TYPE_G1;
        else if (boost::starts_with(sline, "G92 "))
            line.type = CoolingLine::TYPE_G92;
        else if (boost::starts_with(sline, "G2 "))
            line.type = CoolingLine::TYPE_G2;
        else if (boost::starts_with(sline, "G3 "))
            line.type = CoolingLine::TYPE_G3;
         //BBS: parse object id & node id
        else if (boost::starts_with(sline, object_id_string)) {
            std::string sub = sline.substr(object_id_string.size());
            object_id       = std::stoi(sub);
        } else if (boost::starts_with(sline, cooling_node_label)) {
            std::string sub = sline.substr(cooling_node_label.size());
            cooling_node_id = std::stoi(sub);
        } else if (boost::contains(sline, ";not reset fan")){
            not_set_additional_fan = true;
        }

        if (line.type) {
            // G0, G1 or G92
            // Parse the G-code line.
            std::vector<float> new_pos(current_pos);
            const char *c = sline.data() + 3;
            for (;;) {
                // Skip whitespaces.
                for (; *c == ' ' || *c == '\t'; ++ c);
                if (*c == 0 || *c == ';')
                    break;

                assert(is_decimal_separator_point()); // for atof
                //BBS: Parse the axis.
                size_t axis = (*c >= 'X' && *c <= 'Z') ? (*c - 'X') :
                              (*c == 'E') ? 3 : (*c == 'F') ? 4 :
                              (*c == 'I') ? 5 : (*c == 'J') ? 6 : size_t(-1);
                if (axis != size_t(-1)) {
                    new_pos[axis] = float(atof(++c));
                    if (axis == 4) {
                        // Convert mm/min to mm/sec.
                        new_pos[4] /= 60.f;
                        if ((line.type & CoolingLine::TYPE_G92) == 0)
                            // This is G0 or G1 line and it sets the feedrate. This mark is used for reducing the duplicate F calls.
                            line.type |= CoolingLine::TYPE_HAS_F;
                    } else if (axis == 5 || axis == 6) {
                        // BBS: get position of arc center
                        new_pos[axis] += current_pos[axis - 5];
                    }
                }
                // Skip this word.
                for (; *c != ' ' && *c != '\t' && *c != 0; ++ c);
            }
            bool external_perimeter = boost::contains(sline, ";_EXTERNAL_PERIMETER");
            bool wipe               = boost::contains(sline, ";_WIPE");

            record_wall_lines(append_inner_wall_ptr, line_idx, adjustment, node_pos);

            if (wipe)
                line.type |= CoolingLine::TYPE_WIPE;
            if (boost::contains(sline, ";_EXTRUDE_SET_SPEED") && !wipe && !not_join_cooling) {
                line.type |= CoolingLine::TYPE_ADJUSTABLE;
                active_speed_modifier = adjustment->lines.size();
            }

            record_wall_lines(append_wall_ptr, line_idx, adjustment, node_pos);

            if (external_perimeter) {
                line.type |= CoolingLine::TYPE_EXTERNAL_PERIMETER;
                if (m_config.no_slow_down_for_cooling_on_outwalls.get_at(adjustment->extruder_id))
                    line.type &= ~CoolingLine::TYPE_ADJUSTABLE; // Dont slowdown external perimeters for layer cooling flag
                if (line.type & CoolingLine::TYPE_ADJUSTABLE && join_z_smooth && !spiral_vase) {
                    // BBS: collect outwall info
                    mark_node_pos(append_wall_ptr, line_idx, node_pos, object_label, cooling_node_id, object_id, adjustment);
                }
            }

            if ((line.type & CoolingLine::TYPE_G92) == 0) {
                //BBS: G0, G1, G2, G3. Calculate the duration.
                if (m_config.use_relative_e_distances.value)
                    // Reset extruder accumulator.
                    current_pos[3] = 0.f;
                float dif[4];
                for (size_t i = 0; i < 4; ++ i)
                    dif[i] = new_pos[i] - current_pos[i];
                float dxy2 = 0;
                //BBS: support to calculate length of arc
                if (line.type & CoolingLine::TYPE_G2 || line.type & CoolingLine::TYPE_G3) {
                    Vec3f start(current_pos[0], current_pos[1], 0);
                    Vec3f end(new_pos[0], new_pos[1], 0);
                    Vec3f center(new_pos[5], new_pos[6], 0);
                    bool is_ccw = line.type & CoolingLine::TYPE_G3;
                    float dxy = ArcSegment::calc_arc_length(start, end, center, is_ccw);
                    dxy2 = dxy * dxy;
                } else {
                    dxy2 = dif[0] * dif[0] + dif[1] * dif[1];
                }
                float dxyz2 = dxy2 + dif[2] * dif[2];
                if (dxyz2 > 0.f) {
                    // Movement in xyz, calculate time from the xyz Euclidian distance.
                    line.length = sqrt(dxyz2);
                } else if (std::abs(dif[3]) > 0.f) {
                    // Movement in the extruder axis.
                    line.length = std::abs(dif[3]);
                }
                line.feedrate = new_pos[4];
                line.origin_feedrate = new_pos[4];

                assert((line.type & CoolingLine::TYPE_ADJUSTABLE) == 0 || line.feedrate > 0.f);
                if (line.length > 0)
                    line.time = line.length / line.feedrate;

                if (line.feedrate == 0)
                    line.time = 0;

                line.time_max = line.time;
                if ((line.type & CoolingLine::TYPE_ADJUSTABLE) || active_speed_modifier != size_t(-1))
                    line.time_max = (adjustment->slow_down_min_speed == 0.f) ? FLT_MAX : std::max(line.time, line.length / adjustment->slow_down_min_speed);
                line.origin_time_max = line.time_max;
                // BBS: add G2 and G3 support
                if (active_speed_modifier < adjustment->lines.size() && ((line.type & CoolingLine::TYPE_G1) ||
                                                                         (line.type & CoolingLine::TYPE_G2) ||
                                                                         (line.type & CoolingLine::TYPE_G3))) {
                    // Inside the ";_EXTRUDE_SET_SPEED" blocks, there must not be a G1 Fxx entry.
                    assert((line.type & CoolingLine::TYPE_HAS_F) == 0);
                    CoolingLine &sm = adjustment->lines[active_speed_modifier];
                    assert(sm.feedrate > 0.f);
                    sm.length   += line.length;
                    sm.time     += line.time;
                    if (sm.time_max != FLT_MAX) {
                        if (line.time_max == FLT_MAX)
                            sm.time_max = FLT_MAX;
                        else
                            sm.time_max += line.time_max;

                        sm.origin_time_max = sm.time_max;
                    }
                    // Don't store this line.
                    line.type = 0;
                }
            }
            current_pos = std::move(new_pos);
        } else if (boost::starts_with(sline, "; Slow Down Start")) {
            not_join_cooling = true;
        } else if (boost::starts_with(sline, "; Slow Down End")) {
            not_join_cooling = false;
        } else if (boost::starts_with(sline, ";_EXTRUDE_END")) {
            line.type = CoolingLine::TYPE_EXTRUDE_END;
            active_speed_modifier = size_t(-1);
        } else if (boost::starts_with(sline, m_toolchange_prefix)) {
            unsigned int new_extruder = (unsigned int)atoi(sline.c_str() + m_toolchange_prefix.size());
            // Only change extruder in case the number is meaningful. User could provide an out-of-range index through custom gcodes - those shall be ignored.
            if (new_extruder < map_extruder_to_per_extruder_adjustment.size()) {
                if (new_extruder != current_extruder) {
                    // Switch the tool.
                    line.type = CoolingLine::TYPE_SET_TOOL;
                    current_extruder = new_extruder;
                    adjustment         = &per_extruder_adjustments[map_extruder_to_per_extruder_adjustment[current_extruder]];
                }
            }
            else {
                // Only log the error in case of MM printer. Single extruder printers likely ignore any T anyway.
                if (map_extruder_to_per_extruder_adjustment.size() > 1)
                    BOOST_LOG_TRIVIAL(error) << "CoolingBuffer encountered an invalid toolchange, maybe from a custom gcode: " << sline;
            }

        } else if (boost::starts_with(sline, ";_OVERHANG_FAN_START")) {
            line.type = CoolingLine::TYPE_OVERHANG_FAN_START;
        } else if (boost::starts_with(sline, ";_OVERHANG_FAN_END")) {
            line.type = CoolingLine::TYPE_OVERHANG_FAN_END;
        } else if (boost::starts_with(sline, "G4 ")) {
            // Parse the wait time.
            line.type = CoolingLine::TYPE_G4;
            size_t pos_S = sline.find('S', 3);
            size_t pos_P = sline.find('P', 3);
            assert(is_decimal_separator_point()); // for atof
            line.time = line.time_max = float(
                (pos_S > 0) ? atof(sline.c_str() + pos_S + 1) :
                (pos_P > 0) ? atof(sline.c_str() + pos_P + 1) * 0.001 : 0.);
            line.origin_time_max      = line.time_max;
        } else if (boost::starts_with(sline, ";_FORCE_RESUME_FAN_SPEED")) {
            line.type = CoolingLine::TYPE_FORCE_RESUME_FAN;
        } else if (boost::starts_with(sline, ";_SET_FAN_SPEED_CHANGING_LAYER")) {
            line.type = CoolingLine::TYPE_SET_FAN_CHANGING_LAYER;
        } else if (boost::starts_with(sline, "M624")) {
            line.type = CoolingLine::TYPE_OBJECT_START;
        } else if (boost::starts_with(sline, "M625")) {
            line.type = CoolingLine::TYPE_OBJECT_END;
        } else if (boost::contains(sline, ";set fan changing filament")) {
            line.type = CoolingLine::TYPE_SET_FAN_CHANGING_FILAMENT;
        } else if (boost::contains(sline, ";not set fan changing filament")) {
            line.type = CoolingLine::TYPE_NOT_SET_FAN_CHANGING_FILAMENT;
        }
        if (line.type != 0)
            adjustment->lines.emplace_back(std::move(line));
    }
    m_parse_gcode_extruder = current_extruder;
    return per_extruder_adjustments;
}

// Apply slow down over G-code lines stored in per_extruder_adjustments, enable fan if needed.
// Returns the adjusted G-code.
std::string GCodeEditor::write_layer_gcode(
    // Source G-code for the current layer.
    const std::string                      &gcode,
    const bool                             &not_set_additional_fan,
    // ID of the current layer, used to disable fan for the first n layers.
    size_t                                  layer_id,
    // Total time of this layer after slow down, used to control the fan.
    float                                   layer_time,
    // Per extruder list of G-code lines and their cool down attributes.
    std::vector<PerExtruderAdjustments>    &per_extruder_adjustments)
{
    if (gcode.empty())
        return gcode;

    // First sort the adjustment lines by of multiple extruders by their position in the source G-code.
    std::vector<const CoolingLine*> lines;
    {
        size_t n_lines = 0;
        for (const PerExtruderAdjustments &adj : per_extruder_adjustments)
            n_lines += adj.lines.size();
        lines.reserve(n_lines);
        for (const PerExtruderAdjustments &adj : per_extruder_adjustments)
            for (const CoolingLine &line : adj.lines)
                lines.emplace_back(&line);
        std::sort(lines.begin(), lines.end(), [](const CoolingLine *ln1, const CoolingLine *ln2) { return ln1->line_start < ln2->line_start; } );
    }
    // Second generate the adjusted G-code.
    std::string new_gcode;
    new_gcode.reserve(gcode.size() * 2);
    bool overhang_fan_control= false;
    int  overhang_fan_speed   = 0;
    float pre_start_overhang_fan_time = 0.f;

    enum class SetFanType {
        sfChangingLayer = 0,
        sfChangingFilament,
        sfImmediatelyApply
    };

    auto change_extruder_set_fan = [this, layer_id, layer_time, &new_gcode, &overhang_fan_control, &overhang_fan_speed, &pre_start_overhang_fan_time, not_set_additional_fan](SetFanType type) {
#define EXTRUDER_CONFIG(OPT) m_config.OPT.get_at(m_current_extruder)
        int fan_min_speed = EXTRUDER_CONFIG(fan_min_speed);
        int fan_speed_new = EXTRUDER_CONFIG(reduce_fan_stop_start_freq) ? fan_min_speed : 0;
        //BBS
        int additional_fan_speed_new = EXTRUDER_CONFIG(additional_cooling_fan_speed);
        int close_fan_the_first_x_layers = EXTRUDER_CONFIG(close_fan_the_first_x_layers);
        // Is the fan speed ramp enabled?
        int full_fan_speed_layer = EXTRUDER_CONFIG(full_fan_speed_layer);
        if (close_fan_the_first_x_layers <= 0 && full_fan_speed_layer > 0) {
            // When ramping up fan speed from close_fan_the_first_x_layers to full_fan_speed_layer, force close_fan_the_first_x_layers above zero,
            // so there will be a zero fan speed at least at the 1st layer.
            close_fan_the_first_x_layers = 1;
        }
        if (int(layer_id) >= close_fan_the_first_x_layers) {
            int   fan_max_speed             = EXTRUDER_CONFIG(fan_max_speed);
            float slow_down_layer_time = float(EXTRUDER_CONFIG(slow_down_layer_time));
            float fan_cooling_layer_time      = float(EXTRUDER_CONFIG(fan_cooling_layer_time));
            //BBS: always enable the fan speed interpolation according to layer time
            //if (EXTRUDER_CONFIG(cooling)) {
                if (layer_time < slow_down_layer_time) {
                    // Layer time very short. Enable the fan to a full throttle.
                    fan_speed_new = fan_max_speed;
                } else if (layer_time < fan_cooling_layer_time) {
                    // Layer time quite short. Enable the fan proportionally according to the current layer time.
                    assert(layer_time >= slow_down_layer_time);
                    double t = (layer_time - slow_down_layer_time) / (fan_cooling_layer_time - slow_down_layer_time);
                    fan_speed_new = int(floor(t * fan_min_speed + (1. - t) * fan_max_speed) + 0.5);
                }
            //}
            overhang_fan_speed   = EXTRUDER_CONFIG(overhang_fan_speed);
            if (int(layer_id) >= close_fan_the_first_x_layers && int(layer_id) + 1 < full_fan_speed_layer) {
                // Ramp up the fan speed from close_fan_the_first_x_layers to full_fan_speed_layer.
                float factor = float(int(layer_id + 1) - close_fan_the_first_x_layers) / float(full_fan_speed_layer - close_fan_the_first_x_layers);
                fan_speed_new    = std::clamp(int(float(fan_speed_new) * factor + 0.5f), 0, 255);
                overhang_fan_speed = std::clamp(int(float(overhang_fan_speed) * factor + 0.5f), 0, 255);
            }

            overhang_fan_control= overhang_fan_speed > fan_speed_new;
        } else {
            overhang_fan_control= false;
            overhang_fan_speed   = 0;
            fan_speed_new      = 0;
            additional_fan_speed_new = EXTRUDER_CONFIG(first_x_layer_fan_speed);
        }
#undef EXTRUDER_CONFIG
        if (fan_speed_new != m_fan_speed) {
            m_fan_speed = fan_speed_new;
            //BBS
            m_current_fan_speed = fan_speed_new;
            if (type == SetFanType::sfImmediatelyApply)
                new_gcode  += GCodeWriter::set_fan(m_config.gcode_flavor, m_fan_speed);
            else if (type == SetFanType::sfChangingLayer)
                this->m_set_fan_changing_layer = true;
            //BBS: don't need to handle change filament, because we are always force to resume fan speed when filament change is finished
        }
        //BBS
        if (additional_fan_speed_new != m_additional_fan_speed) {
            m_additional_fan_speed = additional_fan_speed_new;
            if (type == SetFanType::sfImmediatelyApply && m_set_fan_changing_filament_start && m_config.auxiliary_fan.value)
                new_gcode += GCodeWriter::set_additional_fan(m_additional_fan_speed);
            else if (type == SetFanType::sfChangingLayer && !not_set_additional_fan)
                this->m_set_addition_fan_changing_layer = true;
            //BBS: don't need to handle change filament, because we are always force to resume fan speed when filament change is finished
        }
        //BBS: set fan pre start time value
        pre_start_overhang_fan_time = overhang_fan_control ? m_config.pre_start_fan_time.get_at(m_current_extruder) : 0.f;
    };

    const char         *pos               = gcode.c_str();
    int                 current_feedrate  = 0;
    //BBS
    m_set_fan_changing_layer = false;
    m_set_addition_fan_changing_layer = false;
    change_extruder_set_fan(SetFanType::sfChangingLayer);

    //BBS: start the fan earlier for overhangs
    float cumulative_time = 0.f;
    float search_time     = 0.f;

    for (int i = 0,j = 0; i < lines.size(); i++) {
        const CoolingLine *line = lines[i];
        if (pre_start_overhang_fan_time > 0.f && overhang_fan_speed > m_fan_speed) {
            cumulative_time += line->time;
            j = j<i ? i : j;
            search_time = search_time<cumulative_time ? cumulative_time : search_time;
            // bbs: search for the next overhang line in xx seconds
            for (; search_time - cumulative_time < pre_start_overhang_fan_time && j < lines.size() && overhang_fan_control && m_current_fan_speed < overhang_fan_speed; j++) {
                const CoolingLine *line_iter = lines[j];
                //do not change fan speed for changing filament gcode
                if (line_iter->type & CoolingLine::TYPE_FORCE_RESUME_FAN) {
                    //stop search when find a force resume fan command
                    break;
                }
                search_time += line_iter->time;
                if (line_iter->type & CoolingLine::TYPE_OVERHANG_FAN_START) {
                    m_current_fan_speed = overhang_fan_speed;
                    new_gcode += GCodeWriter::set_fan(m_config.gcode_flavor, overhang_fan_speed);
                    break;
                }
            }
        }
        const char *line_start  = gcode.c_str() + line->line_start;
        const char *line_end    = gcode.c_str() + line->line_end;
        if (line_start > pos)
            new_gcode.append(pos, line_start - pos);
        if(line->type & CoolingLine::TYPE_SET_FAN_CHANGING_FILAMENT)
            m_set_fan_changing_filament_start = true;
        else if(line->type & CoolingLine::TYPE_NOT_SET_FAN_CHANGING_FILAMENT){
            m_set_fan_changing_filament_start = false;
        }else if (line->type & CoolingLine::TYPE_SET_TOOL) {
            unsigned int new_extruder = (unsigned int)atoi(line_start + m_toolchange_prefix.size());
            if (new_extruder != m_current_extruder) {
                m_current_extruder = new_extruder;
                change_extruder_set_fan(SetFanType::sfChangingFilament); //BBS: will force to resume fan speed when filament change is finished
                cumulative_time             = 0.f;
                search_time                 = 0.f;
            }
            new_gcode.append(line_start, line_end - line_start);
        } else if (line->type & CoolingLine::TYPE_OVERHANG_FAN_START) {
            if (overhang_fan_control && m_current_fan_speed < overhang_fan_speed) {
                //BBS
                m_current_fan_speed = overhang_fan_speed;
                new_gcode += GCodeWriter::set_fan(m_config.gcode_flavor, overhang_fan_speed);
            }
        } else if (line->type & CoolingLine::TYPE_OVERHANG_FAN_END) {
            if (overhang_fan_control) {
                //BBS
                m_current_fan_speed = m_fan_speed;
                new_gcode += GCodeWriter::set_fan(m_config.gcode_flavor, m_fan_speed);
            }
        } else if (line->type & CoolingLine::TYPE_FORCE_RESUME_FAN) {
            //BBS: force to write a fan speed command again
            if (m_current_fan_speed != -1)
                new_gcode += GCodeWriter::set_fan(m_config.gcode_flavor, m_current_fan_speed);
            if (m_additional_fan_speed != -1 && m_set_fan_changing_filament_start && m_config.auxiliary_fan.value)
                new_gcode += GCodeWriter::set_additional_fan(m_additional_fan_speed);
        } else if (line->type & CoolingLine::TYPE_SET_FAN_CHANGING_LAYER) {
            //BBS: check whether fan speed need to changed when change layer
            if (m_current_fan_speed != -1 && m_set_fan_changing_layer) {
                new_gcode += GCodeWriter::set_fan(m_config.gcode_flavor, m_current_fan_speed);
                m_set_fan_changing_layer = false;
            }
            if (m_additional_fan_speed != -1 && m_set_addition_fan_changing_layer && m_config.auxiliary_fan.value) {
                new_gcode += GCodeWriter::set_additional_fan(m_additional_fan_speed);
                m_set_addition_fan_changing_layer = false;
            }
        }
        else if (line->type & CoolingLine::TYPE_EXTRUDE_END) {
            // Just remove this comment.
        } else if (line->type & (CoolingLine::TYPE_ADJUSTABLE | CoolingLine::TYPE_EXTERNAL_PERIMETER | CoolingLine::TYPE_WIPE | CoolingLine::TYPE_HAS_F)) {
            // Find the start of a comment, or roll to the end of line.
            const char *end = line_start;
            for (; end < line_end && *end != ';'; ++ end);
            // Find the 'F' word.
            const char *fpos            = strstr(line_start + 2, " F") + 2;
            int         new_feedrate    = current_feedrate;
            // Modify the F word of the current G-code line.
            bool        modify          = false;
            // Remove the F word from the current G-code line.
            bool        remove          = false;
            assert(fpos != nullptr);
            new_feedrate = line->slowdown ? int(floor(60. * line->feedrate + 0.5)) : atoi(fpos);
            if (new_feedrate == current_feedrate) {
                // No need to change the F value.
                if ((line->type & (CoolingLine::TYPE_ADJUSTABLE | CoolingLine::TYPE_EXTERNAL_PERIMETER | CoolingLine::TYPE_WIPE)) || line->length == 0.)
                    // Feedrate does not change and this line does not move the print head. Skip the complete G-code line including the G-code comment.
                    end = line_end;
                else
                    // Remove the feedrate from the G0/G1 line. The G-code line may become empty!
                    remove = true;
            } else if (line->slowdown) {
                // The F value will be overwritten.
                modify = true;
            } else {
                // The F value is different from current_feedrate, but not slowed down, thus the G-code line will not be modified.
                // Emit the line without the comment.
                new_gcode.append(line_start, end - line_start);
                current_feedrate = new_feedrate;
            }
            if (modify || remove) {
                if (modify) {
                    // Replace the feedrate.
                    new_gcode.append(line_start, fpos - line_start);
                    current_feedrate = new_feedrate;
                    char buf[64];
                    sprintf(buf, "%d", int(current_feedrate));
                    new_gcode += buf;
                } else {
                    // Remove the feedrate word.
                    const char *f = fpos;
                    // Roll the pointer before the 'F' word.
                    for (f -= 2; f > line_start && (*f == ' ' || *f == '\t'); -- f);

                    if ((f - line_start == 1) && *line_start == 'G' && (*f == '1' || *f == '0')) {
                        // BBS: only remain "G1" or "G0" of this line after remove 'F' part, don't save
                    } else {
                        // Append up to the F word, without the trailing whitespace.
                        new_gcode.append(line_start, f - line_start + 1);
                    }
                }
                // Skip the non-whitespaces of the F parameter up the comment or end of line.
                for (; fpos != end && *fpos != ' ' && *fpos != ';' && *fpos != '\n'; ++ fpos);
                // Append the rest of the line without the comment.
                if (fpos < end)
                    // The G-code line is not empty yet. Emit the rest of it.
                    new_gcode.append(fpos, end - fpos);
                else if (remove && new_gcode == "G1") {
                    // The G-code line only contained the F word, now it is empty. Remove it completely including the comments.
                    new_gcode.resize(new_gcode.size() - 2);
                    end = line_end;
                }
            }
            // Process the rest of the line.
            if (end < line_end) {
                if (line->type & (CoolingLine::TYPE_ADJUSTABLE | CoolingLine::TYPE_EXTERNAL_PERIMETER | CoolingLine::TYPE_WIPE)) {
                    // Process comments, remove ";_EXTRUDE_SET_SPEED", ";_EXTERNAL_PERIMETER", ";_WIPE"
                    std::string comment(end, line_end);
                    boost::replace_all(comment, ";_EXTRUDE_SET_SPEED", "");
                    if (line->type & CoolingLine::TYPE_EXTERNAL_PERIMETER)
                        boost::replace_all(comment, ";_EXTERNAL_PERIMETER", "");
                    if (line->type & CoolingLine::TYPE_WIPE)
                        boost::replace_all(comment, ";_WIPE", "");
                    new_gcode += comment;
                } else {
                    // Just attach the rest of the source line.
                    new_gcode.append(end, line_end - end);
                }
            }
        } else if (line->type & CoolingLine::TYPE_OBJECT_START) {
            new_gcode.append(line_start, line_end - line_start);
            if (pre_start_overhang_fan_time > 0.f && m_current_fan_speed > m_fan_speed)
                new_gcode += GCodeWriter::set_fan(m_config.gcode_flavor, m_current_fan_speed);
        } else if (line->type & CoolingLine::TYPE_OBJECT_END) {
            if (pre_start_overhang_fan_time > 0.f && m_current_fan_speed > m_fan_speed)
                new_gcode += GCodeWriter::set_fan(m_config.gcode_flavor, m_fan_speed);
            new_gcode.append(line_start, line_end - line_start);
        }else {
            new_gcode.append(line_start, line_end - line_start);
        }
        pos = line_end;
    }
    const char *gcode_end = gcode.c_str() + gcode.size();
    if (pos < gcode_end)
        new_gcode.append(pos, gcode_end - pos);

    return new_gcode;
}

} // namespace Slic3r
