// ============================================================================
// VortekPreCooling.cpp
//
// Extracted from GCodeProcessor.cpp — PreCoolingInjector member function
// implementations. The class declaration stays in GCodeProcessor.hpp.
//
// BBL parity: BambuStudio GCodeProcessor.cpp:6412-6786 (commit 3f2570c)
//
// Contains 5 methods:
//   - process_pre_cooling_and_heating
//   - build_extruder_free_blocks
//   - inject_cooling_heating_command
//   - build_by_filament_blocks
//   - build_by_extruder_blocks
// ============================================================================

#include "GCode/GCodeProcessor.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <vector>

#include <boost/log/trivial.hpp>
#include <boost/format.hpp>

namespace Slic3r {

// H2C PreCooling: Top-level entry — iterate free blocks, compute temps, inject M104.
// BBL ref: BambuStudio GCodeProcessor.cpp:6412-6458
void GCodeProcessor::PreCoolingInjector::process_pre_cooling_and_heating(TimeProcessor::InsertedLinesMap& inserted_operation_lines)
{
    bool is_multiple_nozzle = std::any_of(extruder_max_nozzle_count.begin(), extruder_max_nozzle_count.end(), [](auto& elem) {return elem > 1; });
    auto get_nozzle_temp = [this, is_multiple_nozzle](int filament_id, bool is_first_layer, bool from_or_to, bool consider_preheat_temperature_delta) {
        if (filament_id == -1)
            return from_or_to ? 140 : 0; // default temp
        double temp = (is_first_layer ? filament_nozzle_temps_initial_layer[filament_id] : filament_nozzle_temps[filament_id]);
        if (consider_preheat_temperature_delta)
            return (int) (temp - filament_preheat_temperature_delta[filament_id]);
        else
            return (int)(temp);
    };

    // Temporary workaround for X2D: when extruder types are mixed (e.g. DirectDrive + Bowden),
    // limit pre-heating target to avoid overshooting on the slower-responding extruder.
    bool has_mixed_extruder_types = extruder_types.size() > 1 &&
        std::adjacent_find(extruder_types.begin(), extruder_types.end(), std::not_equal_to<>()) != extruder_types.end();
    float first_nozzle_dia = nozzle_diameter.empty() ? 0.4 : nozzle_diameter.front();
    float switcher_temp_offset = (first_nozzle_dia >= 0.6 - EPSILON) ? 40.f : 20.f;

    std::map<int, std::vector<ExtruderFreeBlock>> per_extruder_free_blocks;

    for (auto& block : m_extruder_free_blocks)
        per_extruder_free_blocks[block.extruder_id].emplace_back(block);

    for (auto& elem : per_extruder_free_blocks) {
        int extruder_id = elem.first;
        auto& extruder_free_blcoks = elem.second;
        for (auto iter = extruder_free_blcoks.begin(); iter != extruder_free_blcoks.end(); ++iter) {
            bool is_end = std::next(iter) == extruder_free_blcoks.end();
            bool apply_pre_cooling = true;
            bool apply_pre_heating = is_end ? false : true;
            float curr_temp = get_nozzle_temp(iter->last_filament_id, false, true, false);
            float target_temp = get_nozzle_temp(iter->next_filament_id, false, false, !iter->ignore_cooling_before_tower);
            // X2D temporary workaround: only apply temp offset when extruder types are mixed
            if (has_filament_switcher && has_mixed_extruder_types && apply_pre_heating) {
                float print_temp = get_nozzle_temp(iter->next_filament_id, false, false, false);
                target_temp = std::min(target_temp, print_temp - switcher_temp_offset);
            }
            inject_cooling_heating_command(inserted_operation_lines, *iter, curr_temp, target_temp, apply_pre_cooling, apply_pre_heating);
        }
    }
}

// H2C PreCooling: Dispatch — use filament or extruder blocks depending on count.
// BBL ref: BambuStudio GCodeProcessor.cpp:6460-6465
void GCodeProcessor::PreCoolingInjector::build_extruder_free_blocks(const std::vector<ExtruderPreHeating::FilamentUsageBlock>& filament_usage_blocks, const std::vector<ExtruderPreHeating::ExtruderUsageBlcok>& extruder_usage_blocks)
{
    if (extruder_usage_blocks.size() <= 1)
        build_by_filament_blocks(filament_usage_blocks);
    else
        build_by_extruder_blocks(extruder_usage_blocks);
}

// H2C PreCooling: Core injection logic — compute cooling/heating temps and timing,
// emit M104 commands at the correct gcode line IDs.
// BBL ref: BambuStudio GCodeProcessor.cpp:6467-6683
void GCodeProcessor::PreCoolingInjector::inject_cooling_heating_command(TimeProcessor::InsertedLinesMap& inserted_operation_lines, const ExtruderFreeBlock& block, float curr_temp, float target_temp, bool pre_cooling, bool pre_heating)
{
    auto get_valid_extruder_id = [&](int last_nozzle_id) {
        auto nozzle_opt = nozzle_group_result.get_nozzle_from_id(last_nozzle_id);
        return nozzle_opt ? nozzle_opt->extruder_id : 0;
    };

    auto is_pre_cooling_valid = [&nozzle_temps = this->filament_nozzle_temps, &pre_cooling_temps = this->filament_pre_cooling_temps](int idx) ->bool {
        if (idx < 0)
            return false;
        return pre_cooling_temps[idx] > 0 && pre_cooling_temps[idx] < nozzle_temps[idx];
    };

    bool is_multiple_nozzle = std::any_of(extruder_max_nozzle_count.begin(), extruder_max_nozzle_count.end(), [](auto& elem) {return elem > 1; });

    auto get_partial_free_cooling_thres = [&](int idx) -> float {
        if (idx < 0)
            return 30.f;
        float temp_in_tower = filament_nozzle_temps[idx];
        return temp_in_tower - (float)(filament_pre_cooling_temps[idx]);
    };

    auto gcode_move_comp = [](const GCodeProcessorResult::MoveVertex& a, unsigned int gcode_id) {
        return a.gcode_id < gcode_id;
    };

    auto find_skip_block_end = [&skippable_blocks = this->skippable_blocks](unsigned int gcode_id) -> unsigned int {
        auto it = std::upper_bound(
            skippable_blocks.begin(), skippable_blocks.end(), gcode_id,
            [](unsigned int id, const std::pair<unsigned int, unsigned int>& block) { return id < block.first; }
        );
        if (it != skippable_blocks.begin()) {
            auto candidate = std::prev(it);
            if (gcode_id >= candidate->first && gcode_id <= candidate->second)
                return candidate->second;
        }
        return 0;
    };

    auto find_skip_block_start = [&skippable_blocks = this->skippable_blocks](unsigned int gcode_id) -> unsigned int {
        auto it = std::upper_bound(
            skippable_blocks.begin(), skippable_blocks.end(), gcode_id,
            [](unsigned int id, const std::pair<unsigned int, unsigned int>& block) { return id < block.first; }
        );
        if (it != skippable_blocks.begin()) {
            auto candidate = std::prev(it);
            if (gcode_id >= candidate->first && gcode_id <= candidate->second)
                return candidate->first;
        }
        return 0;
    };

    auto adjust_iter = [&](std::vector<GCodeProcessorResult::MoveVertex>::const_iterator iter,
                       const std::vector<GCodeProcessorResult::MoveVertex>::const_iterator& begin,
                       const std::vector<GCodeProcessorResult::MoveVertex>::const_iterator& end,
                       bool forward) -> std::vector<GCodeProcessorResult::MoveVertex>::const_iterator
    {
        if (forward) {
            while (iter != end) {
                unsigned current_id = iter->gcode_id;
                unsigned skip_block_end = find_skip_block_end(current_id);
                if (skip_block_end == 0)
                    break;
                iter = std::lower_bound(iter, end, skip_block_end + 1, gcode_move_comp);
            }
        }
        else {
            while (iter != begin) {
                unsigned current_id = iter->gcode_id;
                unsigned skip_block_start = find_skip_block_start(current_id);
                if (skip_block_start == 0)
                    break;
                auto new_iter = std::lower_bound(begin, iter, skip_block_start, gcode_move_comp);
                if (new_iter == begin)
                    break;
                iter = std::prev(new_iter);
            }
        }
        return iter;
    };

    if (!pre_cooling && !pre_heating && block.free_upper_gcode_id <= block.free_lower_gcode_id)
        return;

    auto move_iter_lower = std::lower_bound(moves.begin(), moves.end(), block.free_lower_gcode_id, gcode_move_comp);
    auto move_iter_upper = std::lower_bound(moves.begin(), moves.end(), block.free_upper_gcode_id, gcode_move_comp);

    if (move_iter_lower == moves.end() || move_iter_upper == moves.begin())
        return;
    --move_iter_upper;
    float complete_free_time_gap = 0;
    if (move_iter_lower == moves.begin())
        complete_free_time_gap = move_iter_upper->time[valid_machine_id];
    else
        complete_free_time_gap = move_iter_upper->time[valid_machine_id] - std::prev(move_iter_lower)->time[valid_machine_id];

    auto partial_free_move_lower = std::lower_bound(moves.begin(), moves.end(), block.partial_free_lower_id, gcode_move_comp);
    auto partial_free_move_upper = std::lower_bound(moves.begin(), moves.end(), block.partial_free_upper_id, gcode_move_comp);
    if (partial_free_move_lower == moves.end() || partial_free_move_upper == moves.begin())
        return;
    --partial_free_move_upper;
    float partial_free_time_gap = 0;
    if (partial_free_move_lower == moves.begin())
        partial_free_time_gap = partial_free_move_upper->time[valid_machine_id];
    else
        partial_free_time_gap = partial_free_move_upper->time[valid_machine_id] - std::prev(partial_free_move_lower)->time[valid_machine_id];

    if (move_iter_lower >= move_iter_upper)
        return;

    bool apply_cooling_when_partial_free = is_pre_cooling_valid(block.last_filament_id) && pre_cooling;

    if (apply_cooling_when_partial_free && partial_free_time_gap + complete_free_time_gap < inject_time_threshold)
        return;

    if (!apply_cooling_when_partial_free && complete_free_time_gap < inject_time_threshold)
        return;

    int extruder_id = get_valid_extruder_id(block.last_nozzle_id);
    float ext_heating_rate = heating_rate[extruder_id];
    float ext_cooling_rate = cooling_rate[extruder_id];

    std::vector<std::string> line_buf;
    auto add_M104_lines = [&](int gcode_id, int target_extruder, int target_temp, int target_filament, bool skippable, int next_filament_idx, int next_nozzle_id, TimeProcessor::InsertLineType type, const std::string& comment = std::string()) {

        auto format_line_M104 = [&](int target_extruder, int target_temp, int target_filament, bool skippable, int next_filament_idx, int next_nozzle_id, const std::string& comment = std::string()) -> std::vector<std::string> {
            std::vector<std::string> buffer;
            if (skippable) {
                const bool support_dynamic_nozzle_map = this->nozzle_group_result.is_support_dynamic_nozzle_map();
                std::string m632_line = "M632 S" + std::to_string(next_filament_idx);
                if (support_dynamic_nozzle_map)
                    m632_line += " H" + std::to_string(next_nozzle_id);
                if (extruder_max_nozzle_count[target_extruder] > 1)
                    m632_line += " N R";
                m632_line += " W\n";
                buffer.emplace_back(std::move(m632_line));
            }
            buffer.emplace_back("M400\n");
            std::string M104_line = "M104";
            if (handle_hotend_as_extruder) {
                M104_line += (" I" + std::to_string(target_filament == -1 ? next_filament_idx : target_filament));
            }
            else if (target_extruder != -1) {
                M104_line += (" T" + std::to_string(physical_extruder_map[target_extruder]));
            }

            M104_line += " S" + std::to_string(target_temp);
            M104_line += " N0"; // N0 means the gcode is generated by slicer

            if (!comment.empty())
                M104_line += " ;" + comment;
            M104_line += '\n';

            buffer.emplace_back(M104_line);

            if (skippable)
                buffer.emplace_back("M633\n");

            return buffer;
        };

        std::vector<std::string> line_buf = format_line_M104(target_extruder, target_temp, target_filament, skippable, next_filament_idx, next_nozzle_id, comment);
        for (auto& line : line_buf)
            inserted_operation_lines[gcode_id].emplace_back(line, type);
    };

    constexpr float room_temperature = 25.f;

    if (apply_cooling_when_partial_free) {
        float max_cooling_temp = std::min(curr_temp, std::min(get_partial_free_cooling_thres(block.last_filament_id), partial_free_time_gap * ext_cooling_rate));
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": partial cooling for %1% %2%") % max_cooling_temp % curr_temp;
        curr_temp = std::max(room_temperature, curr_temp - max_cooling_temp);
        add_M104_lines(block.partial_free_lower_id, extruder_id, curr_temp, block.last_filament_id, false, block.next_filament_id, block.next_nozzle_id, TimeProcessor::InsertLineType::PreCooling, "Multi extruder pre cooling in post extrusion");
    }

    if (pre_cooling && !pre_heating) {
        if (target_temp >= curr_temp)
            return;
        int clamped_target = std::max((int)room_temperature, (int)target_temp);
        add_M104_lines(block.free_lower_gcode_id, extruder_id, clamped_target, block.last_filament_id, false, block.next_filament_id, block.next_nozzle_id, TimeProcessor::InsertLineType::PreCooling, "Multi extruder pre cooling");
        return;
    }
    if (!pre_cooling && pre_heating) {
        if (target_temp <= curr_temp)
            return;
        float heating_start_time = move_iter_upper->time[valid_machine_id] - (target_temp - curr_temp) / ext_heating_rate;
        auto heating_move_iter = std::upper_bound(move_iter_lower, move_iter_upper + 1, heating_start_time, [valid_machine_id = this->valid_machine_id](float time, const GCodeProcessorResult::MoveVertex& a) {return time < a.time[valid_machine_id]; });
        if (heating_move_iter == move_iter_lower) {
            add_M104_lines(block.free_lower_gcode_id, extruder_id, target_temp, block.next_filament_id, true, block.next_filament_id, block.next_nozzle_id, TimeProcessor::InsertLineType::PreHeating, "Multi extruder pre heating");
        }
        else {
            --heating_move_iter;
            heating_move_iter = adjust_iter(heating_move_iter, move_iter_lower, move_iter_upper, false);
            add_M104_lines(heating_move_iter->gcode_id, extruder_id, target_temp, block.next_filament_id, true, block.next_filament_id, block.next_nozzle_id, TimeProcessor::InsertLineType::PreHeating, "Multi extruder pre heating");
        }
        return;
    }
    // perform cooling first and then perform heating
    float mid_temp = std::max(room_temperature, (curr_temp * ext_heating_rate + target_temp * ext_cooling_rate - complete_free_time_gap * ext_cooling_rate * ext_heating_rate) / (ext_cooling_rate + ext_heating_rate));
    float heating_temp = target_temp - mid_temp;
    float heating_start_time = move_iter_upper->time[valid_machine_id] - heating_temp / ext_heating_rate;
    auto heating_move_iter = std::upper_bound(move_iter_lower, move_iter_upper + 1, heating_start_time, [valid_machine_id = this->valid_machine_id](float time, const GCodeProcessorResult::MoveVertex& a) {return time < a.time[valid_machine_id]; });
    if (heating_move_iter == move_iter_lower)
        return;
    --heating_move_iter;
    heating_move_iter = adjust_iter(heating_move_iter, move_iter_lower, move_iter_upper, false);

    // get the insert pos of heat cmd and recalculate time gap and delta temp
    float real_cooling_time = heating_move_iter->time[valid_machine_id] - move_iter_lower->time[valid_machine_id];
    int real_delta_temp = std::min((int)(real_cooling_time * ext_cooling_rate), (int)curr_temp);
    if (real_delta_temp == 0)
        return;
    int cooling_temp = std::max((int)room_temperature, (int)curr_temp - real_delta_temp);
    add_M104_lines(block.free_lower_gcode_id, extruder_id, cooling_temp, block.last_filament_id, false, block.next_filament_id, block.next_nozzle_id, TimeProcessor::InsertLineType::PreCooling, "Multi extruder pre cooling");
    add_M104_lines(heating_move_iter->gcode_id, extruder_id, target_temp, block.next_filament_id, true, block.next_filament_id, block.next_nozzle_id, TimeProcessor::InsertLineType::PreHeating, "Multi extruder pre heating");
}

// H2C PreCooling: Build free blocks from per-filament usage blocks.
// BBL ref: BambuStudio GCodeProcessor.cpp:6685-6730
void GCodeProcessor::PreCoolingInjector::build_by_filament_blocks(const std::vector<ExtruderPreHeating::FilamentUsageBlock>& filament_usage_blocks_)
{
    m_extruder_free_blocks.clear();

    std::map<int, std::vector<ExtruderPreHeating::FilamentUsageBlock>> per_extruder_usage_blocks;
    for (auto& block : filament_usage_blocks_) {
        per_extruder_usage_blocks[block.extruder_id].emplace_back(block);
    }
    ExtruderPreHeating::FilamentUsageBlock start_filament_block(-1, -1, -1, 0, machine_start_gcode_end_id);
    ExtruderPreHeating::FilamentUsageBlock end_filament_block(-1, -1, -1, machine_end_gcode_start_id, std::numeric_limits<unsigned int>::max());

    for (auto& elem : per_extruder_usage_blocks) {
        auto& blocks = elem.second;
        blocks.insert(blocks.begin(), start_filament_block);
        blocks.emplace_back(end_filament_block);
    }

    for (auto& elem : per_extruder_usage_blocks) {
        size_t extruder_id = elem.first;
        const auto& filament_blocks = elem.second;

        for (auto iter = filament_blocks.begin(); iter < filament_blocks.end(); ++iter) {
            auto niter = std::next(iter);
            if (niter == filament_blocks.end())
                break;
            ExtruderFreeBlock block;
            block.free_lower_gcode_id = iter->upper_gcode_id;
            block.last_filament_id = iter->filament_id;
            block.last_nozzle_id = iter->nozzle_id;
            block.free_upper_gcode_id = niter->lower_gcode_id;
            block.next_filament_id = niter->filament_id;
            block.next_nozzle_id = niter->nozzle_id;
            if (block.last_nozzle_id == -1)
                block.last_nozzle_id = block.next_nozzle_id;
            block.extruder_id = extruder_id;
            block.partial_free_lower_id = block.free_lower_gcode_id;
            block.partial_free_upper_id = block.free_lower_gcode_id;
            m_extruder_free_blocks.emplace_back(block);
        }
    }
    std::for_each(m_extruder_free_blocks.begin(), m_extruder_free_blocks.end(), [](ExtruderFreeBlock& block) { block.ignore_cooling_before_tower = true; });
    sort(m_extruder_free_blocks.begin(), m_extruder_free_blocks.end(), [](const auto& a, const auto& b) {
        return a.free_lower_gcode_id < b.free_lower_gcode_id || (a.free_lower_gcode_id == b.free_lower_gcode_id && a.free_upper_gcode_id < b.free_upper_gcode_id);
    });
}

// H2C PreCooling: Build free blocks from per-extruder usage blocks (multi-extruder path).
// BBL ref: BambuStudio GCodeProcessor.cpp:6732-6786
void GCodeProcessor::PreCoolingInjector::build_by_extruder_blocks(const std::vector<ExtruderPreHeating::ExtruderUsageBlcok>& extruder_usage_blocks_)
{
    m_extruder_free_blocks.clear();
    std::map<int, std::vector<ExtruderPreHeating::ExtruderUsageBlcok>> per_extruder_usage_blocks;
    for (auto& block : extruder_usage_blocks_)
        per_extruder_usage_blocks[block.extruder_id].emplace_back(block);

    for (auto& elem : per_extruder_usage_blocks) {
        size_t extruder_id = elem.first;
        auto& blocks = elem.second;
        ExtruderPreHeating::ExtruderUsageBlcok start_filament_block;
        start_filament_block.initialize_step_1(extruder_id, 0, -1, -1);
        start_filament_block.initialize_step_2(machine_start_gcode_end_id);
        start_filament_block.initialize_step_3(machine_start_gcode_end_id, -1, machine_start_gcode_end_id, -1);

        ExtruderPreHeating::ExtruderUsageBlcok end_filament_block;
        end_filament_block.initialize_step_1(extruder_id, machine_end_gcode_start_id, -1, -1);
        end_filament_block.initialize_step_2(std::numeric_limits<int>::max());
        end_filament_block.initialize_step_3(std::numeric_limits<int>::max(), -1, std::numeric_limits<int>::max(), -1);

        blocks.insert(blocks.begin(), start_filament_block);
        blocks.emplace_back(end_filament_block);
    }

    for (auto& elem : per_extruder_usage_blocks) {
        size_t extruder_id = elem.first;
        const auto& extruder_usage_blocks = elem.second;
        for (auto iter = extruder_usage_blocks.begin(); iter != extruder_usage_blocks.end(); ++iter) {
            auto niter = std::next(iter);
            if (niter == extruder_usage_blocks.end())
                break;
            ExtruderFreeBlock block;
            block.free_lower_gcode_id = iter->end_id;
            block.last_filament_id = iter->end_filament;
            block.last_nozzle_id = iter->end_nozzle_id;
            block.free_upper_gcode_id = niter->start_id;
            block.next_filament_id = niter->start_filament;
            block.next_nozzle_id = niter->start_nozzle_id;
            if (block.last_nozzle_id == -1)
                block.last_nozzle_id = block.next_nozzle_id;
            block.extruder_id = extruder_id;
            block.partial_free_lower_id = iter->post_extrusion_start_id;
            block.partial_free_upper_id = iter->post_extrusion_end_id;
            block.ignore_cooling_before_tower = niter->ignore_cooling_before_tower;
            m_extruder_free_blocks.emplace_back(block);
        }
    }

    sort(m_extruder_free_blocks.begin(), m_extruder_free_blocks.end(), [](const auto& a, const auto& b) {
        return a.free_lower_gcode_id < b.free_lower_gcode_id || (a.free_lower_gcode_id == b.free_lower_gcode_id && a.free_upper_gcode_id < b.free_upper_gcode_id);
    });
}

} /* namespace Slic3r */
