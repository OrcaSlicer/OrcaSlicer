// ============================================================================
// VortekWipeTower.cpp
//
// Implements the Vortek::WipeTower class to delegate WipeTower setups
// and nozzle change generation.
// ============================================================================

#include "VortekWipeTower.hpp"
#include "GCode/WipeTowerWriter.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/MultiNozzleUtils.hpp"
#include "GCode/GCodeProcessor.hpp"

#include <numeric>
#include <map>
#include <algorithm>

namespace Vortek {

bool WipeTower::is_h2c_printer(const Slic3r::Print* print)
{
    if (!print) return false;
    return print->is_BBL_printer() &&
           (print->config().nozzle_diameter.size() > 1) &&
           (print->config().extruder_max_nozzle_count.values.size() > 1) &&
           (print->config().extruder_max_nozzle_count.values[1] > 1);
}

bool WipeTower::is_h2c_printer(const Slic3r::PrintConfig& config)
{
    return (config.nozzle_diameter.size() > 1) &&
           (config.extruder_max_nozzle_count.values.size() > 1) &&
           (config.extruder_max_nozzle_count.values[1] > 1);
}

bool WipeTower::is_h2c_printer(const std::string& printer_model)
{
    return printer_model == "Bambu Lab H2C";
}

WipeTower::FilamentChangeTimeResult WipeTower::calculate_filament_change_time(
    const std::string& printer_model,
    int new_extruder_id,
    int next_filament_id,
    int old_filament_in_extruder,
    int old_filament_in_nozzle,
    bool filament_in_nozzle_change,
    bool nozzle_in_extruder_change,
    const std::vector<unsigned char>& m_filament_id,
    const std::function<float(size_t)>& get_filament_unload_time,
    const std::function<float(size_t)>& get_filament_load_time)
{
    FilamentChangeTimeResult res;
    if (!is_h2c_printer(printer_model)) {
        res.performed = false;
        return res;
    }
    res.performed = true;

    // H2C has two physical extruders: 
    // Left extruder (id=0) has 1 physical nozzle.
    // Right extruder (id=1) has 1 physical Vortek nozzle fed by AMS.
    // Therefore, any filament change on the right extruder requires physical AMS unloading/loading.
    if (new_extruder_id == 1) {
        int last_filament = (m_filament_id.size() > 1 && m_filament_id[1] != (unsigned char)(-1)) ? (int)m_filament_id[1] : -1;
        bool perform_static_time_calc = (last_filament != next_filament_id);
        if (perform_static_time_calc) {
            if (last_filament >= 0)
                res.extra_time += get_filament_unload_time(static_cast<size_t>(last_filament));
            res.extruder_unloaded = false;
            res.extra_time += get_filament_load_time(static_cast<size_t>(next_filament_id));
            if (last_filament != -1)
                res.flush_filament_changed = true;
        }
    } else {
        bool perform_static_time_calc = filament_in_nozzle_change;
        if (perform_static_time_calc) {
            if (old_filament_in_extruder >= 0)
                res.extra_time += get_filament_unload_time(static_cast<size_t>(old_filament_in_extruder));
            res.extruder_unloaded = false;
            res.extra_time += get_filament_load_time(static_cast<size_t>(next_filament_id));
            if (old_filament_in_nozzle != -1)
                res.flush_filament_changed = true;
        }
    }
    return res;
}

/**
 * @brief Initializes WipeTower instance variables using values from the print configuration.
 * 
 * Sets up the filament change lengths, wipe tower max speeds, multi-nozzle configuration flag,
 * physical extruder maps, and hotend heating rates.
 */
void WipeTower::init_ctor(Slic3r::WipeTower& tower, const Slic3r::PrintConfig& config)
{
    // Configure filament change lengths (first = standard change length, second = non-cut change length)
    tower.m_filaments_change_length.first  = config.filament_change_length.values;
    tower.m_filaments_change_length.second = config.filament_change_length_nc.values;

    // Convert max purge speed from mm/s to mm/min
    tower.m_max_speed = float(config.wipe_tower_max_purge_speed) * 60.f;
    if (tower.m_max_speed <= 0.f)
        tower.m_max_speed = 5400.f;

    // Constrain first layer speed to a safe maximum of 90mm/s
    tower.m_first_layer_max_speed = std::min(tower.m_max_speed, 5400.f);

    // Verify if any extruder is configured to support multiple nozzles (rotating turret carousel)
    tower.m_is_multiple_nozzle = std::any_of(config.extruder_max_nozzle_count.values.begin(), config.extruder_max_nozzle_count.values.end(),
                                             [](auto& elem) { return elem > 1; });

    // Load or generate the logical-to-physical extruder index mapping table
    {
        auto* opt = config.option<Slic3r::ConfigOptionInts>("physical_extruder_map");
        if (opt && !opt->values.empty())
            tower.m_physical_extruder_map = opt->values;
        else {
            tower.m_physical_extruder_map.resize(config.nozzle_diameter.size());
            std::iota(tower.m_physical_extruder_map.begin(), tower.m_physical_extruder_map.end(), 0);
        }
    }

    // Set up hotend heating rate values (C/s) per nozzle. Default fallback is 2.0 C/s.
    {
        size_t n = config.nozzle_diameter.size();
        tower.m_hotend_heating_rate.resize(n, 2.0);
        for (size_t i = 0; i < n; ++i) {
            if (i < config.hotend_heating_rate.size() && !config.hotend_heating_rate.is_nil(i) && config.hotend_heating_rate.values[i] > 0)
                tower.m_hotend_heating_rate[i] = config.hotend_heating_rate.values[i];
        }
    }
}

/**
 * @brief Configures extruder-specific properties such as the perimeter width of nozzle changes.
 * 
 * Assigns safe widths for extrusion paths based on standard nozzle diameters.
 */
void WipeTower::init_set_extruder(Slic3r::WipeTower& tower, float nozzle_diameter)
{
    // Hardcoded mapping table for standard nozzle sizes to target nozzle change perimeter widths.
    static const std::map<float, float> nozzle_diameter_to_nozzle_change_width{{0.2f, 0.5f}, {0.4f, 1.0f}, {0.6f, 1.2f}, {0.8f, 1.4f}};
    auto it                               = nozzle_diameter_to_nozzle_change_width.find(nozzle_diameter);
    tower.m_nozzle_change_perimeter_width = (it != nozzle_diameter_to_nozzle_change_width.end()) ? it->second : 2 * tower.m_perimeter_width;
}

/**
 * @brief Generates G-code instructions for rotating the nozzle turret inside the wipe tower bounds.
 * 
 * Calculates the required wipe depths and lengths based on the current layer information,
 * sets up the WipeTowerWriter with extrusion flows and coordinates, outputs NOZZLE_CHANGE_START/END
 * firmware tags, and lays down the physical G1 extrusion segments for cleaning/priming the new nozzle.
 */
Slic3r::WipeTower::NozzleChangeResult WipeTower::nozzle_change(Slic3r::WipeTower& tower, int old_filament_id, int new_filament_id)
{
    float wipe_depth             = 0.f;
    int nozzle_change_line_count = 0;

    // Extract tool change metrics (required depth) from current layer info
    if (new_filament_id != (unsigned int) (-1)) {
        for (const auto& b : tower.m_layer_info->tool_changes)
            if (b.new_tool == new_filament_id) {
                wipe_depth   = b.required_depth;
                // TPU filaments require larger spacing/lower count to prevent jamming
                if (tower.has_tpu_filament())
                    nozzle_change_line_count = ((b.nozzle_change_depth + tower.WT_EPSILON) / tower.m_nozzle_change_perimeter_width) / 2;
                else
                    nozzle_change_line_count = (b.nozzle_change_depth + tower.WT_EPSILON) / tower.m_nozzle_change_perimeter_width;
                break;
            }
    }

    // Determine target extrusion speed based on max volumetric flow and filament characteristics
    float nozzle_change_speed = 60.0f * tower.m_filpar[tower.m_current_tool].max_e_speed / tower.m_extrusion_flow;
    if (tower.is_tpu_filament(tower.m_current_tool)) {
        nozzle_change_speed *= 0.25; // Slow down for soft TPU
    }

    // Initialize the G-code output writer
    Slic3r::WipeTowerWriter writer(tower.m_layer_height, tower.m_perimeter_width, tower.m_gcode_flavor, tower.m_filpar);
    writer.set_extrusion_flow(tower.m_extrusion_flow)
        .set_z(tower.m_z_pos)
        .set_initial_tool(tower.m_current_tool)
        .set_extrusion_flow(tower.m_extrusion_flow)
        .set_y_shift(tower.m_y_shift +
                     (new_filament_id != (unsigned int) (-1) && (tower.m_current_shape == Slic3r::WipeTower::SHAPE_REVERSED) ?
                          tower.m_layer_info->depth - tower.m_layer_info->toolchanges_depth() :
                          0.f))
        .append([&]() {
            // Write structured comment tag block for the post-processor to identify the nozzle change boundaries
            char buff[64];
            int old_noz = tower.get_nozzle_id(old_filament_id, tower.m_cur_layer_id);
            int new_noz = tower.get_nozzle_id(new_filament_id, tower.m_cur_layer_id);
            snprintf(buff, sizeof(buff), ";%s OF%d NF%d ON%d NN%d\n",
                     Slic3r::GCodeProcessor::reserved_tag(Slic3r::GCodeProcessor::ETags::NozzleChangeStart).c_str(), old_filament_id,
                     new_filament_id, old_noz, new_noz);
            return std::string(buff);
        }());

    // Calculate bounding box coordinates of the active wipe tower area
    Slic3r::WipeTower::box_coordinates cleaning_box(Slic3r::Vec2f(tower.m_perimeter_width, tower.m_perimeter_width),
                                                     tower.m_wipe_tower_width - 2 * tower.m_perimeter_width,
                                                     (new_filament_id != (unsigned int) (-1) ?
                                                          wipe_depth + tower.m_depth_traversed - tower.m_perimeter_width :
                                                          tower.m_wipe_tower_depth - tower.m_perimeter_width));

    Slic3r::Vec2f initial_position = cleaning_box.ld + Slic3r::Vec2f(0.f, tower.m_depth_traversed);
    writer.set_initial_position(initial_position, tower.m_wipe_tower_width, tower.m_wipe_tower_depth, tower.m_internal_rotation);

    const float& xl = cleaning_box.ld.x();
    const float& xr = cleaning_box.rd.x();

    // Determine spacing between cleaning lines
    float dy = tower.m_layer_info->extra_spacing * tower.m_perimeter_width;
    if (tower.has_tpu_filament())
        dy = 2 * tower.m_perimeter_width;

    tower.m_left_to_right = true;

    // Generate cleaning path lines (zig-zag pattern) inside the wipe tower area
    for (int i = 0; true; ++i) {
        if (tower.m_left_to_right)
            writer.extrude(xr + Slic3r::wipe_tower_wall_infill_overlap * tower.m_perimeter_width, writer.y(), nozzle_change_speed);
        else
            writer.extrude(xl - Slic3r::wipe_tower_wall_infill_overlap * tower.m_perimeter_width, writer.y(), nozzle_change_speed);

        if (writer.y() - float(tower.WT_EPSILON) > cleaning_box.lu.y())
            break;

        if (i == nozzle_change_line_count - 1)
            break;

        writer.extrude(writer.x(), writer.y() + dy);
        tower.m_left_to_right = !tower.m_left_to_right;
    }

    writer.set_extrusion_flow(tower.m_extrusion_flow);
    tower.m_depth_traversed += nozzle_change_line_count * dy;

    Slic3r::WipeTower::NozzleChangeResult result;

    // Add final travel paths or wipe sequences to prevent strings/blobs
    if (tower.is_tpu_filament(tower.m_current_tool)) {
        bool left_to_right       = !tower.m_left_to_right;
        double tpu_travel_length = 5;
        double e_flow            = tower.extrusion_flow(tower.m_layer_height);
        double length            = tpu_travel_length / e_flow;
        int tpu_line_count       = length / (tower.m_wipe_tower_width - 2 * tower.m_perimeter_width) + 1;

        writer.travel(writer.x(), writer.y() - tower.m_perimeter_width);

        for (int i = 0; true; ++i) {
            if (left_to_right)
                writer.travel(xr - tower.m_perimeter_width, writer.y(), nozzle_change_speed);
            else
                writer.travel(xl + tower.m_perimeter_width, writer.y(), nozzle_change_speed);

            if (i == tpu_line_count - 1)
                break;

            writer.travel(writer.x(), writer.y() - dy);
            left_to_right = !left_to_right;
        }
    } else {
        result.wipe_path.push_back(writer.pos());
        if (tower.m_left_to_right) {
            result.wipe_path.push_back(Slic3r::Vec2f(0, writer.y()));
        } else {
            result.wipe_path.push_back(Slic3r::Vec2f(tower.m_wipe_tower_width, writer.y()));
        }
    }

    // Write ending comment tag block
    {
        char buff[64];
        int old_noz = tower.get_nozzle_id(old_filament_id, tower.m_cur_layer_id);
        int new_noz = tower.get_nozzle_id(new_filament_id, tower.m_cur_layer_id);
        snprintf(buff, sizeof(buff), ";%s OF%d NF%d ON%d NN%d\n",
                 Slic3r::GCodeProcessor::reserved_tag(Slic3r::GCodeProcessor::ETags::NozzleChangeEnd).c_str(), old_filament_id,
                 new_filament_id, old_noz, new_noz);
        writer.append(std::string(buff));
    }

    // Return the generated G-code package
    result.start_pos = writer.start_pos_rotated();
    result.end_pos   = writer.pos();
    result.gcode     = std::move(writer.gcode());
    return result;
}

/**
 * @brief Checks if filament ramming is needed.
 * 
 * Ramming is only skipped if both filaments map to the same physical nozzle.
 */
bool WipeTower::is_need_ramming(const Slic3r::WipeTower& tower, int filament_id_1, int filament_id_2, int layer_id)
{
    if (!tower.m_multi_nozzle_group_result)
        return false;
    return !tower.m_multi_nozzle_group_result->are_filaments_same_nozzle(filament_id_1, filament_id_2, layer_id);
}

/**
 * @brief Checks if both filaments map to the same physical extruder.
 */
bool WipeTower::is_same_extruder(const Slic3r::WipeTower& tower, int filament_id_1, int filament_id_2, int layer_id)
{
    if (!tower.m_multi_nozzle_group_result)
        return true;
    return tower.m_multi_nozzle_group_result->are_filaments_same_extruder(filament_id_1, filament_id_2, layer_id);
}

/**
 * @brief Checks if both filaments map to the same nozzle index.
 */
bool WipeTower::is_same_nozzle(const Slic3r::WipeTower& tower, int filament_id_1, int filament_id_2, int layer_id)
{
    if (!tower.m_multi_nozzle_group_result)
        return true;
    return tower.m_multi_nozzle_group_result->are_filaments_same_nozzle(filament_id_1, filament_id_2, layer_id);
}

void WipeTower::initialize_nozzle_status(
    Slic3r::MultiNozzleUtils::NozzleStatusRecorder& recorder,
    const Slic3r::MultiNozzleUtils::LayeredNozzleGroupResult& group_result,
    const Slic3r::Print* print)
{
    if (!is_h2c_printer(print)) return;

    std::set<int> initialized_nozzles;

    // Collect filament IDs to seed from: prefer per-layer sequences, fall back to used_filaments.
    // The 3-arg LayeredNozzleGroupResult::create() does NOT populate _layer_filament_sequences,
    // so the recorder would remain empty — causing get_filament_in_nozzle() to always return -1
    // and filament_in_nozzle_change to be true on every tool change, adding spurious 56s penalties.
    const auto& layer_seqs = group_result.get_layer_filament_sequences();
    if (!layer_seqs.empty()) {
        for (const auto& seq : layer_seqs) {
            for (const auto filament_id : seq) {
                auto nozzle_info = group_result.get_nozzle_for_filament(filament_id, 0);
                if (nozzle_info) {
                    int nozzle_id = nozzle_info->group_id;
                    if (initialized_nozzles.find(nozzle_id) == initialized_nozzles.end()) {
                        recorder.set_nozzle_status(nozzle_id, filament_id, nozzle_info->extruder_id);
                        initialized_nozzles.insert(nozzle_id);
                    }
                }
            }
        }
    } else {
        // Fallback: layer sequences are empty (3-arg create path).
        // Seed from used_filaments via the default nozzle map.
        for (const auto filament_id : group_result.get_used_filaments()) {
            auto nozzle_info = group_result.get_nozzle_for_filament(static_cast<int>(filament_id));
            if (nozzle_info) {
                int nozzle_id = nozzle_info->group_id;
                if (initialized_nozzles.find(nozzle_id) == initialized_nozzles.end()) {
                    recorder.set_nozzle_status(nozzle_id, static_cast<int>(filament_id), nozzle_info->extruder_id);
                    initialized_nozzles.insert(nozzle_id);
                }
            }
        }
    }
}

void WipeTower::adjust_prime_volumes(
    int prev_nozzle_filament,
    int new_filament_id,
    float& wipe_volume_ec,
    float& wipe_volume_nc)
{
    if (prev_nozzle_filament == new_filament_id) {
        wipe_volume_ec = 0.f;
        wipe_volume_nc = 0.f;
    }
}

} // namespace Vortek
