#include "../GCode.hpp"
#include "CoolingBuffer.hpp"
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/log/trivial.hpp>
#include <iostream>
#include <float.h>
#include <system_error>
#include <unordered_map>

#if 0
    #define DEBUG
    #define _DEBUG
    #undef NDEBUG
#endif

#include <assert.h>

namespace Slic3r {

CoolingBuffer::CoolingBuffer(GCode &gcodegen) : m_config(gcodegen.config()), m_toolchange_prefix(gcodegen.writer().toolchange_prefix()), m_current_extruder(0)
{
    this->reset(gcodegen.writer().get_position());

    const std::vector<Extruder> &extruders = gcodegen.writer().extruders();
    m_extruder_ids.reserve(extruders.size());
    for (const Extruder &ex : extruders) {
        m_num_extruders = std::max(ex.id() + 1, m_num_extruders);
        m_extruder_ids.emplace_back(ex.id());
    }
}

void CoolingBuffer::reset(const Vec3d &position)
{
    // BBS: add I and J axis to store center of arc
    m_current_pos.assign(7, 0.f);
    m_current_pos[0] = float(position.x());
    m_current_pos[1] = float(position.y());
    m_current_pos[2] = float(position.z());
    m_current_pos[4] = float(m_config.travel_speed.value);
    m_fan_speed = -1;
    m_additional_fan_speed = -1;
    m_current_fan_speed = -1;
}

struct CoolingLine
{
    enum Type {
        TYPE_SET_TOOL           = 1 << 0,
        TYPE_EXTRUDE_END        = 1 << 1,
        TYPE_OVERHANG_FAN_START = 1 << 2,
        TYPE_OVERHANG_FAN_END   = 1 << 3,
        TYPE_G0                 = 1 << 4,
        TYPE_G1                 = 1 << 5,
        TYPE_ADJUSTABLE         = 1 << 6,
        TYPE_EXTERNAL_PERIMETER = 1 << 7,
        // The line sets a feedrate.
        TYPE_HAS_F              = 1 << 8,
        TYPE_WIPE               = 1 << 9,
        TYPE_G4                 = 1 << 10,
        TYPE_G92                = 1 << 11,
        //BBS: add G2 G3 type
        TYPE_G2                 = 1 << 12,
        TYPE_G3                 = 1 << 13,
        TYPE_FORCE_RESUME_FAN   = 1 << 14,
        TYPE_SUPPORT_INTERFACE_FAN_START     = 1 << 15,
        TYPE_SUPPORT_INTERFACE_FAN_END       = 1 << 16,
        // ORCA: Add support for separate internal bridge fan speed control
        TYPE_INTERNAL_BRIDGE_FAN_START = 1 << 17,
        TYPE_INTERNAL_BRIDGE_FAN_END   = 1 << 18,
        // ORCA: Add support for ironing fan speed control
        TYPE_IRONING_FAN_START         = 1 << 19,
        TYPE_IRONING_FAN_END           = 1 << 20,
    };

    CoolingLine(unsigned int type, size_t  line_start, size_t  line_end) :
        type(type), line_start(line_start), line_end(line_end),
        length(0.f), feedrate(0.f), time(0.f), time_max(0.f), slowdown(false) {}

    bool adjustable(bool slowdown_external_perimeters) const {
        return (this->type & TYPE_ADJUSTABLE) && 
               (! (this->type & TYPE_EXTERNAL_PERIMETER) || slowdown_external_perimeters) &&
               this->time < this->time_max;
    }

    bool adjustable() const {
        return (this->type & TYPE_ADJUSTABLE) && this->time < this->time_max;
    }

    size_t  type;
    // Start of this line at the G-code snippet.
    size_t  line_start;
    // End of this line at the G-code snippet.
    size_t  line_end;
    // XY Euclidian length of this segment.
    float   length;
    // Current feedrate, possibly adjusted.
    float   feedrate;
    // Current duration of this segment.
    float   time;
    // Maximum duration of this segment.
    float   time_max;
    // If marked with the "slowdown" flag, the line has been slowed down.
    bool    slowdown;
};

// Calculate the required per extruder time stretches.
struct PerExtruderAdjustments 
{
    // Calculate the total elapsed time per this extruder, adjusted for the slowdown.
    float elapsed_time_total() const {
        float time_total = 0.f;
        for (const CoolingLine &line : lines)
            time_total += line.time;
        return time_total;
    }
    // Calculate the total elapsed time when slowing down 
    // to the minimum extrusion feed rate defined for the current material.
    float maximum_time_after_slowdown(bool slowdown_external_perimeters) const {
        float time_total = 0.f;
        for (const CoolingLine &line : lines)
            if (line.adjustable(slowdown_external_perimeters)) {
                if (line.time_max == FLT_MAX)
                    return FLT_MAX;
                else
                    time_total += line.time_max;
            } else
                time_total += line.time;
        return time_total;
    }
    // Calculate the adjustable part of the total time.
    float adjustable_time(bool slowdown_external_perimeters) const {
        float time_total = 0.f;
        for (const CoolingLine &line : lines)
            if (line.adjustable(slowdown_external_perimeters))
                time_total += line.time;
        return time_total;
    }
    // Calculate the non-adjustable part of the total time.
    float non_adjustable_time(bool slowdown_external_perimeters) const {
        float time_total = 0.f;
        for (const CoolingLine &line : lines)
            if (! line.adjustable(slowdown_external_perimeters))
                time_total += line.time;
        return time_total;
    }
    // Slow down the adjustable extrusions to the minimum feedrate allowed for the current extruder material.
    // Used by both proportional and non-proportional slow down.
    float slowdown_to_minimum_feedrate(bool slowdown_external_perimeters) {
        float time_total = 0.f;
        for (CoolingLine &line : lines) {
            if (line.adjustable(slowdown_external_perimeters)) {
                assert(line.time_max >= 0.f && line.time_max < FLT_MAX);
                line.slowdown = true;
                line.time     = line.time_max;
                if (line.time > 0.f)
                    line.feedrate = line.length / line.time;
            }
            time_total += line.time;
        }
        this->time_total = time_total;
        return time_total;
    }
    // Slow down each adjustable G-code line proportionally by a factor.
    // Used by the proportional slow down.
    float slow_down_proportional(float factor, bool slowdown_external_perimeters) {
        assert(factor >= 1.f);
        float time_total = 0.f;
        for (CoolingLine &line : lines) {
            if (line.adjustable(slowdown_external_perimeters)) {
                line.slowdown = true;
                line.time     = std::min(line.time_max, line.time * factor);
                if (line.time > 0.f)
                    line.feedrate = line.length / line.time;
            }
            time_total += line.time;
        }
        this->time_total = time_total;
        return time_total;
    }

    // Sort the lines, adjustable first, higher feedrate first.
    // Used by non-proportional slow down.
    void sort_lines_by_decreasing_feedrate() {
        std::sort(lines.begin(), lines.end(), [](const CoolingLine &l1, const CoolingLine &l2) {
            bool adj1 = l1.adjustable();
            bool adj2 = l2.adjustable();
            return (adj1 == adj2) ? l1.feedrate > l2.feedrate : adj1;
        });
        for (n_lines_adjustable = 0; 
            n_lines_adjustable < lines.size() && this->lines[n_lines_adjustable].adjustable();
            ++ n_lines_adjustable);
        time_non_adjustable = 0.f;
        for (size_t i = n_lines_adjustable; i < lines.size(); ++ i)
            time_non_adjustable += lines[i].time;
    }

    // Calculate the maximum time stretch when slowing down to min_feedrate.
    // Slowdown to min_feedrate shall be allowed for this extruder's material.
    // Used by non-proportional slow down.
    float time_stretch_when_slowing_down_to_feedrate(float min_feedrate) const {
        float time_stretch = 0.f;
        assert(this->slow_down_min_speed < min_feedrate + EPSILON);
        for (size_t i = 0; i < n_lines_adjustable; ++ i) {
            const CoolingLine &line = lines[i];
            if (line.feedrate > min_feedrate)
                time_stretch += line.time * (line.feedrate / min_feedrate - 1.f);
        }
        return time_stretch;
    }

    // Slow down all adjustable lines down to min_feedrate.
    // Slowdown to min_feedrate shall be allowed for this extruder's material.
    // Used by non-proportional slow down.
    void slow_down_to_feedrate(float min_feedrate) {
        assert(this->slow_down_min_speed < min_feedrate + EPSILON);
        float time_total = 0.f;
        for (size_t i = 0; i < n_lines_adjustable; ++ i) {
            CoolingLine &line = lines[i];
            if (line.feedrate > min_feedrate) {
                line.time *= std::max(1.f, line.feedrate / min_feedrate);
                line.feedrate = min_feedrate;
                line.slowdown = true;
            }
            time_total += line.time;
        }
        this->time_total = time_total;
    }

    // Extruder, for which the G-code will be adjusted.
    unsigned int                extruder_id         = 0;
    // Is the cooling slow down logic enabled for this extruder's material?
    bool                        cooling_slow_down_enabled = false;
    // Slow down the print down to slow_down_min_speed if the total layer time is below slow_down_layer_time.
    float                       slow_down_layer_time = 0.f;
    // Minimum print speed allowed for this extruder.
    float                       slow_down_min_speed     = 0.f;
    
    bool                        dont_slow_down_outer_wall = false;


    // Parsed lines.
    std::vector<CoolingLine>    lines;
    // The following two values are set by sort_lines_by_decreasing_feedrate():
    // Number of adjustable lines, at the start of lines.
    size_t                      n_lines_adjustable  = 0;
    // Non-adjustable time of lines starting with n_lines_adjustable. 
    float                       time_non_adjustable = 0;
    // Current total time for this extruder.
    float                       time_total          = 0;
    // Maximum time for this extruder, when the maximum slow down is applied.
    float                       time_maximum        = 0;

    // Temporaries for processing the slow down. Both thresholds go from 0 to n_lines_adjustable.
    size_t                      idx_line_begin      = 0;
    size_t                      idx_line_end        = 0;
};

// Calculate a new feedrate when slowing down by time_stretch for segments faster than min_feedrate.
// Used by non-proportional slow down.
float new_feedrate_to_reach_time_stretch(
    std::vector<PerExtruderAdjustments*>::const_iterator it_begin, std::vector<PerExtruderAdjustments*>::const_iterator it_end, 
    float min_feedrate, float time_stretch, size_t max_iter = 20)
{
	float new_feedrate = min_feedrate;
    for (size_t iter = 0; iter < max_iter; ++ iter) {
        double nomin = 0;
        double denom = time_stretch;
        for (auto it = it_begin; it != it_end; ++ it) {
			assert((*it)->slow_down_min_speed < min_feedrate + EPSILON);
			for (size_t i = 0; i < (*it)->n_lines_adjustable; ++i) {
				const CoolingLine &line = (*it)->lines[i];
                if (line.feedrate > min_feedrate) {
                    nomin += (double)line.time * (double)line.feedrate;
                    denom += (double)line.time;
                }
            }
        }
        assert(denom > 0);
        if (denom < 0)
            return min_feedrate;
        new_feedrate = (float)(nomin / denom);
        assert(new_feedrate > min_feedrate - EPSILON);
        if (new_feedrate < min_feedrate + EPSILON)
            goto finished;
        for (auto it = it_begin; it != it_end; ++ it)
			for (size_t i = 0; i < (*it)->n_lines_adjustable; ++i) {
				const CoolingLine &line = (*it)->lines[i];
                if (line.feedrate > min_feedrate && line.feedrate < new_feedrate)
                    // Some of the line segments taken into account in the calculation of nomin / denom are now slower than new_feedrate, 
                    // which makes the new_feedrate lower than it should be.
                    // Re-run the calculation with a new min_feedrate limit, so that the segments with current feedrate lower than new_feedrate
                    // are not taken into account.
                    goto not_finished_yet;
            }
        goto finished;
not_finished_yet:
        min_feedrate = new_feedrate;
    }
    // Failed to find the new feedrate for the time_stretch.

finished:
    // Test whether the time_stretch was achieved.
#ifndef NDEBUG
    {
        float time_stretch_final = 0.f;
        for (auto it = it_begin; it != it_end; ++ it)
            time_stretch_final += (*it)->time_stretch_when_slowing_down_to_feedrate(new_feedrate);
        assert(std::abs(time_stretch - time_stretch_final) < EPSILON);
    }
#endif /* NDEBUG */

	return new_feedrate;
}

std::string CoolingBuffer::process_layer(std::string &&gcode, size_t layer_id, bool flush)
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
        std::vector<PerExtruderAdjustments> per_extruder_adjustments = this->parse_layer_gcode(m_gcode, m_current_pos);
        float layer_time_stretched = this->calculate_layer_slowdown(per_extruder_adjustments);
        out = this->apply_layer_cooldown(m_gcode, layer_id, layer_time_stretched, per_extruder_adjustments);
        m_gcode.clear();
    }
    return out;
}

// Parse the layer G-code for the moves, which could be adjusted.
// Return the list of parsed lines, bucketed by an extruder.
std::vector<PerExtruderAdjustments> CoolingBuffer::parse_layer_gcode(const std::string &gcode, std::vector<float> &current_pos) const
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
        // ORCA: To enable dont slow down external perimeters feature per filament (extruder)
        adj.dont_slow_down_outer_wall   = m_config.dont_slow_down_outer_wall.get_at(extruder_id);
        map_extruder_to_per_extruder_adjustment[extruder_id] = i;
    }

    unsigned int      current_extruder  = m_current_extruder;
    PerExtruderAdjustments *adjustment  = &per_extruder_adjustments[map_extruder_to_per_extruder_adjustment[current_extruder]];
    const char       *line_start = gcode.c_str();
    const char       *line_end   = line_start;
    // Index of an existing CoolingLine of the current adjustment, which holds the feedrate setting command
    // for a sequence of extrusion moves.
    size_t            active_speed_modifier = size_t(-1);

    // Orca: Whether we had our first extrusion in this layer.
    // Time of any other movements before the first extrusion will be excluded from the layer time.
    bool layer_had_extrusion = false;

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
            if (external_perimeter)
                line.type |= CoolingLine::TYPE_EXTERNAL_PERIMETER;
            if (wipe)
                line.type |= CoolingLine::TYPE_WIPE;

            // Orca: only slow down movements since the first extrusion
            if (boost::contains(sline, ";_EXTRUDE_SET_SPEED"))
                layer_had_extrusion = true;
            
            // ORCA: Dont slowdown external perimeters for layer time feature
            // use the adjustment pointer to ensure the value for the current extruder (filament) is used.
            bool adjust_external = true;
            if(adjustment->dont_slow_down_outer_wall && external_perimeter) adjust_external = false;
            
            // ORCA: Dont slowdown external perimeters for layer time works by not marking the external perimeter as adjustable, 
            // hence the slowdown algorithm ignores it.
            if (boost::contains(sline, ";_EXTRUDE_SET_SPEED") && ! wipe && adjust_external) {
                line.type |= CoolingLine::TYPE_ADJUSTABLE;
                active_speed_modifier = adjustment->lines.size();
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
                assert((line.type & CoolingLine::TYPE_ADJUSTABLE) == 0 || line.feedrate > 0.f);
                if (line.length > 0)
                    line.time = line.length / line.feedrate;
                line.time_max = line.time;
                if ((line.type & CoolingLine::TYPE_ADJUSTABLE) || active_speed_modifier != size_t(-1))
                    line.time_max = (adjustment->slow_down_min_speed == 0.f) ? FLT_MAX : std::max(line.time, line.length / adjustment->slow_down_min_speed);
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
                    }
                    // Don't store this line.
                    line.type = 0;
                }
            }
            current_pos = std::move(new_pos);
        } else if (boost::starts_with(sline, ";_EXTRUDE_END")) {
            line.type = CoolingLine::TYPE_EXTRUDE_END;
            active_speed_modifier = size_t(-1);
        } else if (boost::starts_with(sline, m_toolchange_prefix)) {
            unsigned int new_extruder = 0;
            auto ret = std::from_chars(sline.data() + m_toolchange_prefix.size(), sline.data() + sline.size(), new_extruder);
            if (std::errc::invalid_argument != ret.ec) {
                // Only change extruder in case the number is meaningful. User could provide an out-of-range index through custom gcodes -
                // those shall be ignored.
                if (new_extruder < map_extruder_to_per_extruder_adjustment.size()) {
                    if (new_extruder != current_extruder) {
                        // Switch the tool.
                        line.type        = CoolingLine::TYPE_SET_TOOL;
                        current_extruder = new_extruder;
                        adjustment       = &per_extruder_adjustments[map_extruder_to_per_extruder_adjustment[current_extruder]];
                    }
                } else {
                    // Only log the error in case of MM printer. Single extruder printers likely ignore any T anyway.
                    if (map_extruder_to_per_extruder_adjustment.size() > 1)
                        BOOST_LOG_TRIVIAL(error) << "CoolingBuffer encountered an invalid toolchange, maybe from a custom gcode: " << sline;
                }
            }
        } else if (boost::starts_with(sline, ";_OVERHANG_FAN_START")) {
            line.type = CoolingLine::TYPE_OVERHANG_FAN_START;
        } else if (boost::starts_with(sline, ";_OVERHANG_FAN_END")) {
            line.type = CoolingLine::TYPE_OVERHANG_FAN_END;
        } else if (boost::starts_with(sline, ";_INTERNAL_BRIDGE_FAN_START")) { // ORCA: Add support for separate internal bridge fan speed control
            line.type = CoolingLine::TYPE_INTERNAL_BRIDGE_FAN_START;
        } else if (boost::starts_with(sline, ";_INTERNAL_BRIDGE_FAN_END")) { // ORCA: Add support for separate internal bridge fan speed control
            line.type = CoolingLine::TYPE_INTERNAL_BRIDGE_FAN_END;
        } else if (boost::starts_with(sline, ";_SUPP_INTERFACE_FAN_START")) {
            line.type = CoolingLine::TYPE_SUPPORT_INTERFACE_FAN_START;
        } else if (boost::starts_with(sline, ";_SUPP_INTERFACE_FAN_END")) {
            line.type = CoolingLine::TYPE_SUPPORT_INTERFACE_FAN_END;
        } else if (boost::starts_with(sline, ";_IRONING_FAN_START")) { // ORCA: Add support for ironing fan speed control
            line.type = CoolingLine::TYPE_IRONING_FAN_START;
        } else if (boost::starts_with(sline, ";_IRONING_FAN_END")) { // ORCA: Add support for ironing fan speed control
            line.type = CoolingLine::TYPE_IRONING_FAN_END;
        } else if (boost::starts_with(sline, "G4 ")) {
            // Parse the wait time.
            line.type = CoolingLine::TYPE_G4;
            size_t pos_S = sline.find('S', 3);
            size_t pos_P = sline.find('P', 3);
            assert(is_decimal_separator_point()); // for atof
            line.time = line.time_max = float(
                (pos_S > 0) ? atof(sline.c_str() + pos_S + 1) :
                (pos_P > 0) ? atof(sline.c_str() + pos_P + 1) * 0.001 : 0.);
        } else if (boost::starts_with(sline, ";_FORCE_RESUME_FAN_SPEED")) {
            line.type = CoolingLine::TYPE_FORCE_RESUME_FAN;
        }

        // Orca: For any movements before this layer's first ever extrusion, we exclude them from the layer time calculation.
        if (!layer_had_extrusion) {
            assert((line.type & CoolingLine::TYPE_ADJUSTABLE) == 0);
            line.time = line.time_max = 0;
        }

        if (line.type != 0)
            adjustment->lines.emplace_back(std::move(line));
    }

    return per_extruder_adjustments;
}

// Slow down an extruder range to slow_down_layer_time.
// Return the total time for the complete layer.
static inline void extruder_range_slow_down_non_proportional(
    std::vector<PerExtruderAdjustments*>::iterator it_begin,
    std::vector<PerExtruderAdjustments*>::iterator it_end,
    float time_stretch)
{
    // Slow down. Try to equalize the feedrates.
    std::vector<PerExtruderAdjustments*> by_min_print_speed(it_begin, it_end);
    // Find the next highest adjustable feedrate among the extruders.
    float feedrate = 0;
    for (PerExtruderAdjustments *adj : by_min_print_speed) {
        adj->idx_line_begin = 0;
        adj->idx_line_end   = 0;
        if (adj->idx_line_begin < adj->n_lines_adjustable && adj->lines[adj->idx_line_begin].feedrate> feedrate)
            feedrate = adj->lines[adj->idx_line_begin].feedrate;
    }
    assert(feedrate > 0.f);
    // Sort by slow_down_min_speed, maximum speed first.
    std::sort(by_min_print_speed.begin(), by_min_print_speed.end(), 
        [](const PerExtruderAdjustments *p1, const PerExtruderAdjustments *p2){ return p1->slow_down_min_speed > p2->slow_down_min_speed; });
    // Slow down, fast moves first.
    for (;;) {
        // For each extruder, find the span of lines with a feedrate close to feedrate.
        for (PerExtruderAdjustments *adj : by_min_print_speed) {
            for (adj->idx_line_end = adj->idx_line_begin;
                adj->idx_line_end < adj->n_lines_adjustable && adj->lines[adj->idx_line_end].feedrate > feedrate - EPSILON;
                 ++ adj->idx_line_end) ;
        }
        // Find the next highest adjustable feedrate among the extruders.
        float feedrate_next = 0.f;
        for (PerExtruderAdjustments *adj : by_min_print_speed)
            if (adj->idx_line_end < adj->n_lines_adjustable && adj->lines[adj->idx_line_end].feedrate > feedrate_next)
                feedrate_next = adj->lines[adj->idx_line_end].feedrate;
        // Slow down, limited by max(feedrate_next, slow_down_min_speed).
        for (auto adj = by_min_print_speed.begin(); adj != by_min_print_speed.end();) {
            // Slow down at most by time_stretch.
            if ((*adj)->slow_down_min_speed == 0.f) {
                // All the adjustable speeds are now lowered to the same speed,
                // and the minimum speed is set to zero.
                float time_adjustable = 0.f;
                for (auto it = adj; it != by_min_print_speed.end(); ++ it)
                    time_adjustable += (*it)->adjustable_time(true);
                float rate = (time_adjustable + time_stretch) / time_adjustable;
                for (auto it = adj; it != by_min_print_speed.end(); ++ it)
                    (*it)->slow_down_proportional(rate, true);
                return;
            } else {
                float feedrate_limit = std::max(feedrate_next, (*adj)->slow_down_min_speed);
                bool  done           = false;
                float time_stretch_max = 0.f;
                for (auto it = adj; it != by_min_print_speed.end(); ++ it)
                    time_stretch_max += (*it)->time_stretch_when_slowing_down_to_feedrate(feedrate_limit);
                if (time_stretch_max >= time_stretch) {
                    feedrate_limit = new_feedrate_to_reach_time_stretch(adj, by_min_print_speed.end(), feedrate_limit, time_stretch, 20);
                    done = true;
                } else
                    time_stretch -= time_stretch_max;
                for (auto it = adj; it != by_min_print_speed.end(); ++ it)
                    (*it)->slow_down_to_feedrate(feedrate_limit);
                if (done)
                    return;
            }
            // Skip the other extruders with nearly the same slow_down_min_speed, as they have been processed already.
            auto next = adj;
            for (++ next; next != by_min_print_speed.end() && (*next)->slow_down_min_speed > (*adj)->slow_down_min_speed - EPSILON; ++ next);
            adj = next;
        }
        if (feedrate_next == 0.f)
            // There are no other extrusions available for slow down.
            break;
        for (PerExtruderAdjustments *adj : by_min_print_speed) {
            adj->idx_line_begin = adj->idx_line_end;
            feedrate = feedrate_next;
        }
    }
}

// Calculate slow down for all the extruders.
float CoolingBuffer::calculate_layer_slowdown(std::vector<PerExtruderAdjustments> &per_extruder_adjustments)
{
    // Sort the extruders by an increasing slow_down_layer_time.
    // The layers with a lower slow_down_layer_time are slowed down
    // together with all the other layers with slow_down_layer_time above.
    std::vector<PerExtruderAdjustments*> by_slowdown_time;
    by_slowdown_time.reserve(per_extruder_adjustments.size());
    // Only insert entries, which are adjustable (have cooling enabled and non-zero stretchable time).
    // Collect total print time of non-adjustable extruders.
    float elapsed_time_total0 = 0.f;
    for (PerExtruderAdjustments &adj : per_extruder_adjustments) {
        // Curren total time for this extruder.
        adj.time_total  = adj.elapsed_time_total();
        // Maximum time for this extruder, when all extrusion moves are slowed down to min_extrusion_speed.
        adj.time_maximum = adj.maximum_time_after_slowdown(true);
        if (adj.cooling_slow_down_enabled && adj.lines.size() > 0) {
            by_slowdown_time.emplace_back(&adj);
            // sorts the lines, also sets adj.time_non_adjustable
            adj.sort_lines_by_decreasing_feedrate();
        } else
            elapsed_time_total0 += adj.elapsed_time_total();
    }

    std::sort(by_slowdown_time.begin(), by_slowdown_time.end(),
        [](const PerExtruderAdjustments *adj1, const PerExtruderAdjustments *adj2)
            { return adj1->slow_down_layer_time < adj2->slow_down_layer_time; });

    for (auto cur_begin = by_slowdown_time.begin(); cur_begin != by_slowdown_time.end(); ++ cur_begin) {
        PerExtruderAdjustments &adj = *(*cur_begin);
        // Calculate the current adjusted elapsed_time_total over the non-finalized extruders.
        float total = elapsed_time_total0;
        for (auto it = cur_begin; it != by_slowdown_time.end(); ++ it)
            total += (*it)->time_total;
        float slow_down_layer_time = adj.slow_down_layer_time * 1.001f;
        if (total > slow_down_layer_time) {
            // The current total time is above the minimum threshold of the rest of the extruders, don't adjust anything.
        } else {
            // Adjust this and all the following (higher m_config.slow_down_layer_time) extruders.
            // Sum maximum slow down time as if everything was slowed down including the external perimeters.
            float max_time = elapsed_time_total0;
            for (auto it = cur_begin; it != by_slowdown_time.end(); ++ it)
                max_time += (*it)->time_maximum;
            if (max_time > slow_down_layer_time) {
                extruder_range_slow_down_non_proportional(cur_begin, by_slowdown_time.end(), slow_down_layer_time - total);
            } else {
                // Slow down to maximum possible.
                for (auto it = cur_begin; it != by_slowdown_time.end(); ++ it)
                    (*it)->slowdown_to_minimum_feedrate(true);
            }
        }
        elapsed_time_total0 += adj.elapsed_time_total();
    }

    return elapsed_time_total0;
}

// Apply slow down over G-code lines stored in per_extruder_adjustments, enable fan if needed.
// Returns the adjusted G-code.
std::string CoolingBuffer::apply_layer_cooldown(
    // Source G-code for the current layer.
    const std::string                      &gcode,
    // ID of the current layer, used to disable fan for the first n layers.
    size_t                                  layer_id, 
    // Total time of this layer after slow down, used to control the fan.
    float                                   layer_time,
    // Per extruder list of G-code lines and their cool down attributes.
    std::vector<PerExtruderAdjustments>    &per_extruder_adjustments)
{
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
    bool internal_bridge_fan_control= false; // ORCA: Add support for separate internal bridge fan speed control
    int  internal_bridge_fan_speed   = 0; // ORCA: Add support for separate internal bridge fan speed control
    bool supp_interface_fan_control= false;
    int  supp_interface_fan_speed = 0;
    bool ironing_fan_control= false; // ORCA: Add support for ironing fan speed control
    int  ironing_fan_speed   = 0; // ORCA: Add support for ironing fan speed control
    auto change_extruder_set_fan = [ this, layer_id, layer_time, &new_gcode,
        &overhang_fan_control, &overhang_fan_speed,
        &internal_bridge_fan_control, &internal_bridge_fan_speed,
        &supp_interface_fan_control, &supp_interface_fan_speed,
        &ironing_fan_control, &ironing_fan_speed
    ](bool immediately_apply) {
#define EXTRUDER_CONFIG(OPT) m_config.OPT.get_at(m_current_extruder)
        float fan_min_speed = EXTRUDER_CONFIG(fan_min_speed);
        float fan_speed_new = EXTRUDER_CONFIG(reduce_fan_stop_start_freq) ? fan_min_speed : 0;
        //BBS
        int additional_fan_speed_new = EXTRUDER_CONFIG(additional_cooling_fan_speed);
        int close_fan_the_first_x_layers = EXTRUDER_CONFIG(close_fan_the_first_x_layers);
        // Is the fan speed ramp enabled?
        int full_fan_speed_layer = EXTRUDER_CONFIG(full_fan_speed_layer);
        supp_interface_fan_speed = EXTRUDER_CONFIG(support_material_interface_fan_speed);

        if (close_fan_the_first_x_layers <= 0 && full_fan_speed_layer > 0) {
            // When ramping up fan speed from close_fan_the_first_x_layers to full_fan_speed_layer, force close_fan_the_first_x_layers above zero,
            // so there will be a zero fan speed at least at the 1st layer.
            close_fan_the_first_x_layers = 1;
        }
        if (int(layer_id) >= close_fan_the_first_x_layers) {
            float   fan_max_speed             = EXTRUDER_CONFIG(fan_max_speed);
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
            supp_interface_fan_speed = EXTRUDER_CONFIG(support_material_interface_fan_speed);
            supp_interface_fan_control = supp_interface_fan_speed >= 0;

            overhang_fan_control = overhang_fan_speed > fan_speed_new;
            
            // ORCA: Add support for separate internal bridge fan speed control
            internal_bridge_fan_speed   = EXTRUDER_CONFIG(internal_bridge_fan_speed);
            internal_bridge_fan_control = internal_bridge_fan_speed >=0;
            
            if( internal_bridge_fan_speed < 0 ) { // ORCA: Backwards compatibility setting for Orca internal bridge fan speed setting - if set at -1 (which is the default) use the overhang fan speed settings.
                internal_bridge_fan_speed = overhang_fan_speed;
                internal_bridge_fan_control = overhang_fan_control;
            }

            // ORCA: Add support for ironing fan speed control
            ironing_fan_speed   = EXTRUDER_CONFIG(ironing_fan_speed);
            ironing_fan_control = ironing_fan_speed >= 0;
#undef EXTRUDER_CONFIG
            
        } else {
            overhang_fan_control = false;
            overhang_fan_speed   = 0;
            fan_speed_new      = 0;
            additional_fan_speed_new = 0;
            supp_interface_fan_control = false; 
            supp_interface_fan_speed   = 0;
            internal_bridge_fan_control = false; // ORCA: Add support for separate internal bridge fan speed control
            internal_bridge_fan_speed = 0; // ORCA: Add support for separate internal bridge fan speed control
            ironing_fan_control = false; // ORCA: Add support for ironing fan speed control
            ironing_fan_speed = 0; // ORCA: Add support for ironing fan speed control
        }
        if (fan_speed_new != m_fan_speed) {
            m_fan_speed = fan_speed_new;
            m_current_fan_speed = fan_speed_new;
            if (immediately_apply)
                new_gcode  += GCodeWriter::set_fan(m_config.gcode_flavor, m_fan_speed);
        }
        //BBS
        if (additional_fan_speed_new != m_additional_fan_speed) {
            m_additional_fan_speed = additional_fan_speed_new;
            if (immediately_apply && m_config.auxiliary_fan.value)
                new_gcode += GCodeWriter::set_additional_fan(m_additional_fan_speed);
        }
    };

    const char         *pos               = gcode.c_str();
    int                 current_feedrate  = 0;
    change_extruder_set_fan(true);

    // Orca: Reduce set fan commands by deferring the GCodeWriter::set_fan calls. Inspired by SuperSlicer
    // define fan_speed_change_requests and initialize it with all possible types fan speed change requests
    std::unordered_map<int, bool> fan_speed_change_requests = {{CoolingLine::TYPE_OVERHANG_FAN_START, false},
                                                               {CoolingLine::TYPE_INTERNAL_BRIDGE_FAN_START, false}, // ORCA: Add support for separate internal bridge fan speed control
                                                               {CoolingLine::TYPE_SUPPORT_INTERFACE_FAN_START, false},
                                                               {CoolingLine::TYPE_IRONING_FAN_START, false}, // ORCA: Add support for ironing fan speed control
                                                               {CoolingLine::TYPE_FORCE_RESUME_FAN, false}};
    bool need_set_fan = false;

    for (const CoolingLine *line : lines) {
        const char *line_start  = gcode.c_str() + line->line_start;
        const char *line_end    = gcode.c_str() + line->line_end;
        if (line_start > pos)
            new_gcode.append(pos, line_start - pos);
        if (line->type & CoolingLine::TYPE_SET_TOOL) {
            unsigned int new_extruder = 0;
            auto ret = std::from_chars(line_start + m_toolchange_prefix.size(), line_end, new_extruder);
            if (std::errc::invalid_argument != ret.ec) {
                if (new_extruder != m_current_extruder) {
                    m_current_extruder = new_extruder;
                    change_extruder_set_fan(true);
                }
            }
            new_gcode.append(line_start, line_end - line_start);
        } else if (line->type & CoolingLine::TYPE_OVERHANG_FAN_START) {
            if (overhang_fan_control && !fan_speed_change_requests[CoolingLine::TYPE_OVERHANG_FAN_START]) {
                need_set_fan = true;
                fan_speed_change_requests[CoolingLine::TYPE_OVERHANG_FAN_START] = true;
           }
        } else if (line->type & CoolingLine::TYPE_OVERHANG_FAN_END) {
            if (overhang_fan_control && fan_speed_change_requests[CoolingLine::TYPE_OVERHANG_FAN_START]) {
                fan_speed_change_requests[CoolingLine::TYPE_OVERHANG_FAN_START] = false;
            }
            need_set_fan = true;
        } else if (line->type & CoolingLine::TYPE_INTERNAL_BRIDGE_FAN_START) { // ORCA: Add support for separate internal bridge fan speed control
            if (internal_bridge_fan_control && !fan_speed_change_requests[CoolingLine::TYPE_INTERNAL_BRIDGE_FAN_START]) {
                need_set_fan = true;
                fan_speed_change_requests[CoolingLine::TYPE_INTERNAL_BRIDGE_FAN_START] = true;
           }
        } else if (line->type & CoolingLine::TYPE_INTERNAL_BRIDGE_FAN_END) { // ORCA: Add support for separate internal bridge fan speed control
            if (internal_bridge_fan_control && fan_speed_change_requests[CoolingLine::TYPE_INTERNAL_BRIDGE_FAN_START]) {
                fan_speed_change_requests[CoolingLine::TYPE_INTERNAL_BRIDGE_FAN_START] = false;
            }
            need_set_fan = true;
        } else if (line->type & CoolingLine::TYPE_SUPPORT_INTERFACE_FAN_START) {
            if (supp_interface_fan_control && !fan_speed_change_requests[CoolingLine::TYPE_SUPPORT_INTERFACE_FAN_START]) {
                fan_speed_change_requests[CoolingLine::TYPE_SUPPORT_INTERFACE_FAN_START] = true;
                need_set_fan = true;
            }
        } else if (line->type & CoolingLine::TYPE_SUPPORT_INTERFACE_FAN_END && fan_speed_change_requests[CoolingLine::TYPE_SUPPORT_INTERFACE_FAN_START]) {
            if (supp_interface_fan_control) {
                fan_speed_change_requests[CoolingLine::TYPE_SUPPORT_INTERFACE_FAN_START] = false;
            }
            need_set_fan = true;
        } else if (line->type & CoolingLine::TYPE_IRONING_FAN_START) {
            if (ironing_fan_control && !fan_speed_change_requests[CoolingLine::TYPE_IRONING_FAN_START]) {
                fan_speed_change_requests[CoolingLine::TYPE_IRONING_FAN_START] = true;
                need_set_fan = true;
            }
        } else if (line->type & CoolingLine::TYPE_IRONING_FAN_END && fan_speed_change_requests[CoolingLine::TYPE_IRONING_FAN_START]) {
            if (ironing_fan_control) {
                fan_speed_change_requests[CoolingLine::TYPE_IRONING_FAN_START] = false;
            }
            need_set_fan = true;
        } else if (line->type & CoolingLine::TYPE_FORCE_RESUME_FAN) {
            // check if any fan speed change request is active
            if (m_fan_speed != -1 && !std::any_of(fan_speed_change_requests.begin(), fan_speed_change_requests.end(), [](const std::pair<int, bool>& p) { return p.second; })){
                fan_speed_change_requests[CoolingLine::TYPE_FORCE_RESUME_FAN] = true;
                need_set_fan = true;
            }
            if (m_additional_fan_speed != -1 && m_config.auxiliary_fan.value)
                new_gcode += GCodeWriter::set_additional_fan(m_additional_fan_speed);
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
        } else {
            new_gcode.append(line_start, line_end - line_start);
        }

        if (need_set_fan) {
            if (fan_speed_change_requests[CoolingLine::TYPE_OVERHANG_FAN_START]){
                new_gcode += GCodeWriter::set_fan(m_config.gcode_flavor, overhang_fan_speed);
                m_current_fan_speed = overhang_fan_speed;
            } else if (fan_speed_change_requests[CoolingLine::TYPE_INTERNAL_BRIDGE_FAN_START]){ // ORCA: Add support for separate internal bridge fan speed control
                new_gcode += GCodeWriter::set_fan(m_config.gcode_flavor, internal_bridge_fan_speed);
                m_current_fan_speed = internal_bridge_fan_speed;
            }
            else if (fan_speed_change_requests[CoolingLine::TYPE_SUPPORT_INTERFACE_FAN_START]){
                new_gcode += GCodeWriter::set_fan(m_config.gcode_flavor, supp_interface_fan_speed);
                m_current_fan_speed = supp_interface_fan_speed;
            }
            else if (fan_speed_change_requests[CoolingLine::TYPE_IRONING_FAN_START]){
                new_gcode += GCodeWriter::set_fan(m_config.gcode_flavor, ironing_fan_speed);
                m_current_fan_speed = ironing_fan_speed;
            }
            else if(fan_speed_change_requests[CoolingLine::TYPE_FORCE_RESUME_FAN] && m_current_fan_speed != -1){
                new_gcode += GCodeWriter::set_fan(m_config.gcode_flavor, m_current_fan_speed);
                fan_speed_change_requests[CoolingLine::TYPE_FORCE_RESUME_FAN] = false;
            }
            else
                new_gcode += GCodeWriter::set_fan(m_config.gcode_flavor, m_fan_speed);
            need_set_fan = false;
        }
        pos = line_end;
    }
    const char *gcode_end = gcode.c_str() + gcode.size();
    if (pos < gcode_end)
        new_gcode.append(pos, gcode_end - pos);

    return new_gcode;
}

} // namespace Slic3r
