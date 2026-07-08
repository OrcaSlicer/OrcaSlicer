#include "GCodeProcessor.hpp"

#include "libslic3r/libslic3r.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string_view>

namespace Slic3r {
namespace {

bool equals_case_insensitive(std::string_view lhs, std::string_view rhs)
{
    return lhs.size() == rhs.size() && std::equal(lhs.begin(), lhs.end(), rhs.begin(), [](unsigned char l, unsigned char r) {
        return std::tolower(l) == std::tolower(r);
    });
}

float get_clamped_param(const GCodeReader::GCodeLine& line, char axis, float default_value, float min_value, float max_value)
{
    float value = default_value;
    line.has_value(axis, value);
    return std::clamp(value, min_value, max_value);
}

float extrusion_time(float e_length, float feedrate)
{
    return feedrate > 0.0f && e_length > 0.0f ? e_length / feedrate * 60.0f : 0.0f;
}

float retract_time(float e_length)
{
    static constexpr float retract_feedrate = 1800.0f;
    return extrusion_time(std::max(e_length, 0.0f), retract_feedrate);
}

float s819_time(float e_length, float feedrate)
{
    static constexpr float s819_tail_flush_length = 10.0f;
    static constexpr float s819_tail_feedrate     = 400.0f;

    const float tail_length = std::min(std::max(e_length, 0.0f), s819_tail_flush_length);
    const float main_length = std::max(e_length - tail_length, 0.0f);
    return extrusion_time(main_length, feedrate) + extrusion_time(tail_length, s819_tail_feedrate);
}

float estimate_M6211_time_for_centauri_carbon(const GCodeReader::GCodeLine& line, float length, double current_x,
                                              double current_y)
{
    static constexpr float max_segment_length    = 73.0f;
    static constexpr float wipe_after_flush_time = 2.8f;
    static constexpr float main_feedrate         = 500.0f;
    static constexpr float tail_feedrate         = 400.0f;
    static constexpr float travel_feedrate       = 5000.0f;
    static constexpr double parking_x            = 256.0;
    static constexpr double parking_y            = 0.0;

    const float flush_length = std::clamp(length, 10.0f, 1000.0f);
    const float cool_time    = get_clamped_param(line, 'P', 5000.0f, 0.0f, 20000.0f) * 0.001f;
    const float travel_time  = static_cast<float>(std::abs(current_y - parking_y) + std::abs(current_x - parking_x)) /
                              travel_feedrate * 60.0f;

    // Initial time, including: material change, heating, etc.
    float m6211_time = 18.2f + travel_time;

    float remaining_flush_length = std::max(flush_length, 0.0f);
    while (remaining_flush_length > 0.0f) {
        const float segment_length = std::min(remaining_flush_length, max_segment_length);
        remaining_flush_length -= segment_length;
        if (segment_length >= max_segment_length) {
            // Full segment: 3-phase extrusion (30+35+10=75mm) + retract
            m6211_time += extrusion_time(30.0f, main_feedrate) + extrusion_time(35.0f, main_feedrate) +
                          extrusion_time(10.0f, tail_feedrate) + extrusion_time(2.0f, tail_feedrate) + cool_time +
                          wipe_after_flush_time;
        } else {
            // Partial last segment: simple extrude at F500 + retract at F400
            m6211_time += extrusion_time(segment_length, main_feedrate) + extrusion_time(2.0f, tail_feedrate) + cool_time +
                          wipe_after_flush_time;
        }
    }

    return m6211_time;
}

float estimate_M6211_time_for_centauri_carbon_2(const GCodeReader::GCodeLine& line, float length, float new_extruder_temp)
{
    const float flush_length            = std::clamp(length, 10.0f, 1000.0f);
    const float flush_length_single     = get_clamped_param(line, 'K', 75.0f, 10.0f, 300.0f);
    const float old_filament_e_feedrate = get_clamped_param(line, 'M', 300.0f, 10.0f, 600.0f);
    const float new_filament_e_feedrate = get_clamped_param(line, 'N', 300.0f, 10.0f, 600.0f);
    const float cool_time               = get_clamped_param(line, 'P', 3000.0f, 0.0f, 20000.0f) * 0.001f;

    // The flush length of the old material, unit: mm
    static constexpr float e_flush_dist          = 15.0f;
    // Wipe time after flush, in seconds
    static constexpr float wipe_after_flush_time = 5.0f;

    const float flush_length_after_start = std::max(flush_length - e_flush_dist, 0.0f);
    const int   flush_times              = std::max(1, static_cast<int>(std::ceil(flush_length_after_start / flush_length_single)));
    const float flush_length_actual      = flush_length_single;

    // Initial time, including: material change, heating, moving, etc.
    float m6211_time = 31.0f;
    m6211_time += extrusion_time(std::min(e_flush_dist, flush_length), old_filament_e_feedrate);

    const int   intermediate_flush_times = flush_times - 1;
    const float intermediate_flush_time  = s819_time(flush_length_actual, new_filament_e_feedrate) + retract_time(6.0f) + cool_time +
                                          wipe_after_flush_time;
    m6211_time += static_cast<float>(intermediate_flush_times) * intermediate_flush_time;
    m6211_time += s819_time(flush_length_actual, new_filament_e_feedrate * 0.8f) + retract_time(4.0f) + cool_time + wipe_after_flush_time;

    static constexpr float cooling_rate = 1.36f;
    const float            r_temp       = get_clamped_param(line, 'R', new_extruder_temp + 20.0f, 185.0f, 350.0f);
    const float            s_temp       = get_clamped_param(line, 'S', 250.0f, 185.0f, 350.0f);

    if (s_temp < r_temp)
        m6211_time += (r_temp - s_temp) / cooling_rate;

    return m6211_time;
}

float estimate_M6211_time(const GCodeReader::GCodeLine& line, std::string_view printer_model, float length, float new_extruder_temp, double current_x, double current_y)
{
    if (equals_case_insensitive(printer_model, "Elegoo Centauri Carbon") || equals_case_insensitive(printer_model, "Elegoo Centauri")) {
        return estimate_M6211_time_for_centauri_carbon(line, length, current_x, current_y);
    } else if (equals_case_insensitive(printer_model, "Elegoo Centauri Carbon 2") ||
               equals_case_insensitive(printer_model, "Elegoo Centauri 2")) {
        return estimate_M6211_time_for_centauri_carbon_2(line, length, new_extruder_temp);
    }
    return 0.0f;
}

} // namespace

void GCodeProcessor::process_elegoo_M6211(const GCodeReader::GCodeLine& line)
{
    float length = 0.0f;
    if (!line.has_value('L', length) || length <= 0.0f)
        return;

    float t = -1.0f;
    if (!line.has_value('T', t) || t < 0.0f)
        return;

    const int filament_id = static_cast<int>(std::round(t));
    if (filament_id < 0 || filament_id >= m_result.filaments_count)
        return;

    const int extruder_id = m_filament_maps[filament_id];

    float new_extruder_temp = 0.0f;
    if (line.has_value('S', new_extruder_temp)) {
        if (extruder_id >= 0 && static_cast<size_t>(extruder_id) < m_extruder_temps.size())
            m_extruder_temps[static_cast<size_t>(extruder_id)] = new_extruder_temp;
    }

    const float m6211_time         = estimate_M6211_time(line, m_printer_model, length, new_extruder_temp,
                                                         m_start_position[X], m_start_position[Y]);
    const int   curr_filament_id   = get_filament_id(false);
    const bool  is_first_extrusion = (curr_filament_id == -1) || (filament_id == curr_filament_id);

    m_time_processor.filament_unload_times = 0;
    m_time_processor.filament_load_times = m6211_time;
    process_filament_change(filament_id);

    if (extruder_id >= 0 && static_cast<size_t>(extruder_id) < m_remaining_volume.size()) {
        const float remaining_volume = static_cast<size_t>(extruder_id) < m_nozzle_volume.size() ?
                                           m_nozzle_volume[extruder_id] :
                                           0.0f;
        const float filament_diameter = static_cast<size_t>(filament_id) < m_result.filament_diameters.size() ?
                                            m_result.filament_diameters[filament_id] :
                                            m_result.filament_diameters.back();
        const float area_filament_cross_section = static_cast<float>(M_PI) * sqr(0.5f * filament_diameter);
        const float volume_flushed_filament     = area_filament_cross_section * length;

        if (volume_flushed_filament >= remaining_volume) {
            if (!is_first_extrusion)
                m_used_filaments.update_flush_per_filament(curr_filament_id, remaining_volume);

            m_used_filaments.update_flush_per_filament(filament_id, volume_flushed_filament - remaining_volume);
            m_remaining_volume[extruder_id] = 0.0f;
        } else {
            m_used_filaments.update_flush_per_filament(filament_id, volume_flushed_filament);
            m_remaining_volume[extruder_id] -= volume_flushed_filament;
        }
    }
}

} // namespace Slic3r
