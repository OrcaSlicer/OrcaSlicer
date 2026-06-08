// ============================================================================
// VortekPreCooling.cpp
//
// Implements the Vortek::PreCooling class to handle temperature scheduling,
// configuration loading, pre-scanning, and command injection.
// ============================================================================

#include "VortekPreCooling.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "GCodeReader.hpp"
#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <vector>
#include <regex>
#include <sstream>

#include <boost/log/trivial.hpp>
#include <boost/format.hpp>

namespace Vortek {

PreCooling::PreCooling(
    const std::vector<Slic3r::GCodeProcessorResult::MoveVertex>& moves_,
    const std::vector<std::string>& filament_types_,
    const Slic3r::MultiNozzleUtils::LayeredNozzleGroupResult& nozzle_group_result_,
    const std::vector<int>& filament_nozzle_temps_,
    const std::vector<int>& filament_nozzle_temps_initial_layer_,
    const std::vector<int>& physical_extruder_map_,
    int valid_machine_id_,
    float inject_time_threshold_,
    bool handle_hotend_as_extruder_,
    bool has_filament_switcher_,
    const std::vector<int>& pre_cooling_temp_,
    const std::vector<double>& cooling_rate_,
    const std::vector<double>& heating_rate_,
    const std::vector<std::pair<unsigned int, unsigned int>>& skippable_blocks_,
    const std::vector<int>& extruder_max_nozzle_count_,
    const std::vector<double>& filament_preheat_temperature_delta_,
    const std::vector<double>& filament_max_temperature_drop_when_ec_,
    unsigned int machine_start_gcode_end_id_,
    unsigned int machine_end_gcode_start_id_,
    const std::vector<Slic3r::ExtruderType>& extruder_types_,
    const std::vector<double>& nozzle_diameter_
) :
    moves(moves_),
    filament_types(filament_types_),
    nozzle_group_result(nozzle_group_result_),
    filament_nozzle_temps(filament_nozzle_temps_),
    filament_nozzle_temps_initial_layer(filament_nozzle_temps_initial_layer_),
    physical_extruder_map(physical_extruder_map_),
    valid_machine_id(valid_machine_id_),
    inject_time_threshold(inject_time_threshold_),
    handle_hotend_as_extruder(handle_hotend_as_extruder_),
    has_filament_switcher(has_filament_switcher_),
    filament_pre_cooling_temps(pre_cooling_temp_),
    cooling_rate(cooling_rate_),
    heating_rate(heating_rate_),
    skippable_blocks(skippable_blocks_),
    extruder_max_nozzle_count(extruder_max_nozzle_count_),
    filament_preheat_temperature_delta(filament_preheat_temperature_delta_),
    filament_max_temperature_drop_when_ec(filament_max_temperature_drop_when_ec_),
    machine_start_gcode_end_id(machine_start_gcode_end_id_),
    machine_end_gcode_start_id(machine_end_gcode_start_id_),
    extruder_types(extruder_types_),
    nozzle_diameter(nozzle_diameter_)
{
}

void PreCooling::process_pre_cooling_and_heating(Slic3r::GCodeProcessor::TimeProcessor::InsertedLinesMap& inserted_operation_lines)
{
    auto get_nozzle_temp = [this](int filament_id, bool is_first_layer, bool from_or_to, bool consider_preheat_temperature_delta) {
        if (filament_id == -1)
            return from_or_to ? 140 : 0; // default temp
        double temp = (is_first_layer ? filament_nozzle_temps_initial_layer[filament_id] : filament_nozzle_temps[filament_id]);
        if (consider_preheat_temperature_delta)
            return (int) (temp - filament_preheat_temperature_delta[filament_id]);
        else
            return (int)(temp);
    };


    bool has_mixed_extruder_types = extruder_types.size() > 1 &&
        std::adjacent_find(extruder_types.begin(), extruder_types.end(), std::not_equal_to<>()) != extruder_types.end();
    float first_nozzle_dia = nozzle_diameter.empty() ? 0.4 : nozzle_diameter.front();
    float switcher_temp_offset = (first_nozzle_dia >= 0.6 - EPSILON) ? 40.f : 20.f;

    std::map<int, std::vector<ExtruderFreeBlock>> per_extruder_free_blocks;

    for (auto& block : m_extruder_free_blocks)
        per_extruder_free_blocks[block.extruder_id].emplace_back(block);

    for (auto& elem : per_extruder_free_blocks) {
        auto& extruder_free_blcoks = elem.second;
        for (auto iter = extruder_free_blcoks.begin(); iter != extruder_free_blcoks.end(); ++iter) {
            bool is_end = std::next(iter) == extruder_free_blcoks.end();
            bool apply_pre_cooling = true;
            bool apply_pre_heating = is_end ? false : true;
            float curr_temp = get_nozzle_temp(iter->last_filament_id, false, true, false);
            float target_temp = get_nozzle_temp(iter->next_filament_id, false, false, !iter->ignore_cooling_before_tower);
            if (has_filament_switcher && has_mixed_extruder_types && apply_pre_heating) {
                float print_temp = get_nozzle_temp(iter->next_filament_id, false, false, false);
                target_temp = std::min(target_temp, print_temp - switcher_temp_offset);
            }
            inject_cooling_heating_command(inserted_operation_lines, *iter, curr_temp, target_temp, apply_pre_cooling, apply_pre_heating);
        }
    }
}

void PreCooling::build_extruder_free_blocks(
    const std::vector<Slic3r::ExtruderPreHeating::FilamentUsageBlock>& filament_usage_blocks,
    const std::vector<Slic3r::ExtruderPreHeating::ExtruderUsageBlcok>& extruder_usage_blocks
)
{
    if (extruder_usage_blocks.size() <= 1)
        build_by_filament_blocks(filament_usage_blocks);
    else
        build_by_extruder_blocks(extruder_usage_blocks);
}

void PreCooling::inject_cooling_heating_command(
    Slic3r::GCodeProcessor::TimeProcessor::InsertedLinesMap& inserted_operation_lines,
    const ExtruderFreeBlock& block,
    float curr_temp,
    float target_temp,
    bool pre_cooling,
    bool pre_heating
)
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

    auto get_partial_free_cooling_thres = [&](int idx) -> float {
        if (idx < 0)
            return 30.f;
        float temp_in_tower = filament_nozzle_temps[idx];
        return temp_in_tower - (float)(filament_pre_cooling_temps[idx]);
    };

    auto gcode_move_comp = [](const Slic3r::GCodeProcessorResult::MoveVertex& a, unsigned int gcode_id) {
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

    auto adjust_iter = [&](std::vector<Slic3r::GCodeProcessorResult::MoveVertex>::const_iterator iter,
                       const std::vector<Slic3r::GCodeProcessorResult::MoveVertex>::const_iterator& begin,
                       const std::vector<Slic3r::GCodeProcessorResult::MoveVertex>::const_iterator& end,
                       bool forward) -> std::vector<Slic3r::GCodeProcessorResult::MoveVertex>::const_iterator
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

    auto add_M104_lines = [&](int gcode_id, int target_extruder, int target_temp, int target_filament, bool skippable, int next_filament_idx, int next_nozzle_id, Slic3r::GCodeProcessor::TimeProcessor::InsertLineType type, const std::string& comment = std::string()) {


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
        add_M104_lines(block.partial_free_lower_id, extruder_id, curr_temp, block.last_filament_id, false, block.next_filament_id, block.next_nozzle_id, Slic3r::GCodeProcessor::TimeProcessor::InsertLineType::PreCooling, "Multi extruder pre cooling in post extrusion");
    }

    if (pre_cooling && !pre_heating) {
        if (target_temp >= curr_temp)
            return;
        int clamped_target = std::max((int)room_temperature, (int)target_temp);
        add_M104_lines(block.free_lower_gcode_id, extruder_id, clamped_target, block.last_filament_id, false, block.next_filament_id, block.next_nozzle_id, Slic3r::GCodeProcessor::TimeProcessor::InsertLineType::PreCooling, "Multi extruder pre cooling");
        return;
    }
    if (!pre_cooling && pre_heating) {
        if (target_temp <= curr_temp)
            return;
        float heating_start_time = move_iter_upper->time[valid_machine_id] - (target_temp - curr_temp) / ext_heating_rate;
        auto heating_move_iter = std::upper_bound(move_iter_lower, move_iter_upper + 1, heating_start_time, [valid_machine_id = this->valid_machine_id](float time, const Slic3r::GCodeProcessorResult::MoveVertex& a) {return time < a.time[valid_machine_id]; });
        if (heating_move_iter == move_iter_lower) {
            add_M104_lines(block.free_lower_gcode_id, extruder_id, target_temp, block.next_filament_id, true, block.next_filament_id, block.next_nozzle_id, Slic3r::GCodeProcessor::TimeProcessor::InsertLineType::PreHeating, "Multi extruder pre heating");
        }
        else {
            --heating_move_iter;
            heating_move_iter = adjust_iter(heating_move_iter, move_iter_lower, move_iter_upper, false);
            add_M104_lines(heating_move_iter->gcode_id, extruder_id, target_temp, block.next_filament_id, true, block.next_filament_id, block.next_nozzle_id, Slic3r::GCodeProcessor::TimeProcessor::InsertLineType::PreHeating, "Multi extruder pre heating");
        }
        return;
    }
    // perform cooling first and then perform heating
    float mid_temp = std::max(room_temperature, (curr_temp * ext_heating_rate + target_temp * ext_cooling_rate - complete_free_time_gap * ext_cooling_rate * ext_heating_rate) / (ext_cooling_rate + ext_heating_rate));
    float heating_temp = target_temp - mid_temp;
    float heating_start_time = move_iter_upper->time[valid_machine_id] - heating_temp / ext_heating_rate;
    auto heating_move_iter = std::upper_bound(move_iter_lower, move_iter_upper + 1, heating_start_time, [valid_machine_id = this->valid_machine_id](float time, const Slic3r::GCodeProcessorResult::MoveVertex& a) {return time < a.time[valid_machine_id]; });
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
    add_M104_lines(block.free_lower_gcode_id, extruder_id, cooling_temp, block.last_filament_id, false, block.next_filament_id, block.next_nozzle_id, Slic3r::GCodeProcessor::TimeProcessor::InsertLineType::PreCooling, "Multi extruder pre cooling");
    add_M104_lines(heating_move_iter->gcode_id, extruder_id, target_temp, block.next_filament_id, true, block.next_filament_id, block.next_nozzle_id, Slic3r::GCodeProcessor::TimeProcessor::InsertLineType::PreHeating, "Multi extruder pre heating");
}

void PreCooling::build_by_filament_blocks(const std::vector<Slic3r::ExtruderPreHeating::FilamentUsageBlock>& filament_usage_blocks_)
{
    m_extruder_free_blocks.clear();

    std::map<int, std::vector<Slic3r::ExtruderPreHeating::FilamentUsageBlock>> per_extruder_usage_blocks;
    for (auto& block : filament_usage_blocks_) {
        per_extruder_usage_blocks[block.extruder_id].emplace_back(block);
    }
    Slic3r::ExtruderPreHeating::FilamentUsageBlock start_filament_block(-1, -1, -1, 0, machine_start_gcode_end_id);
    Slic3r::ExtruderPreHeating::FilamentUsageBlock end_filament_block(-1, -1, -1, machine_end_gcode_start_id, std::numeric_limits<unsigned int>::max());

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

void PreCooling::build_by_extruder_blocks(const std::vector<Slic3r::ExtruderPreHeating::ExtruderUsageBlcok>& extruder_usage_blocks_)
{
    m_extruder_free_blocks.clear();
    std::map<int, std::vector<Slic3r::ExtruderPreHeating::ExtruderUsageBlcok>> per_extruder_usage_blocks;
    for (auto& block : extruder_usage_blocks_)
        per_extruder_usage_blocks[block.extruder_id].emplace_back(block);

    for (auto& elem : per_extruder_usage_blocks) {
        size_t extruder_id = elem.first;
        auto& blocks = elem.second;
        Slic3r::ExtruderPreHeating::ExtruderUsageBlcok start_filament_block;
        start_filament_block.initialize_step_1(extruder_id, 0, -1, -1);
        start_filament_block.initialize_step_2(machine_start_gcode_end_id);
        start_filament_block.initialize_step_3(machine_start_gcode_end_id, -1, machine_start_gcode_end_id, -1);

        Slic3r::ExtruderPreHeating::ExtruderUsageBlcok end_filament_block;
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

void PreCooling::apply_config(const Slic3r::PrintConfig& config, size_t filament_count, Slic3r::GCodeProcessor& processor)
{
    processor.m_enable_pre_heating = config.enable_pre_heating.value;
    {
        processor.m_cooling_rate.resize(filament_count, 2.0);
        processor.m_heating_rate.resize(filament_count, 2.0);
        for (size_t i = 0; i < filament_count; ++i) {
            if (i < config.hotend_cooling_rate.size() && config.hotend_cooling_rate.values[i] > 0)
                processor.m_cooling_rate[i] = config.hotend_cooling_rate.values[i];
            if (i < config.hotend_heating_rate.size() && config.hotend_heating_rate.values[i] > 0)
                processor.m_heating_rate[i] = config.hotend_heating_rate.values[i];
        }

        processor.m_pre_cooling_temp.resize(filament_count, 0);
        for (size_t i = 0; i < filament_count; ++i) {
            if (i < config.filament_pre_cooling_temperature_nc.size())
                processor.m_pre_cooling_temp[i] = config.filament_pre_cooling_temperature_nc.get_at(i);
        }

        processor.m_filament_preheat_temperature_delta.resize(filament_count, 0.0);
        for (size_t i = 0; i < filament_count; ++i) {
            if (i < config.filament_preheat_temperature_delta.size())
                processor.m_filament_preheat_temperature_delta[i] = config.filament_preheat_temperature_delta.get_at(i);
        }

        processor.m_filament_max_temperature_drop_when_ec.resize(filament_count, 0.0);

        processor.m_filament_types.resize(filament_count);
        for (size_t i = 0; i < filament_count; ++i) {
            if (i < config.filament_type.size())
                processor.m_filament_types[i] = config.filament_type.values[i];
        }

        processor.m_extruder_types.clear();
        for (size_t i = 0; i < config.extruder_type.size(); ++i)
            processor.m_extruder_types.push_back(static_cast<Slic3r::ExtruderType>(config.extruder_type.values[i]));

        processor.m_nozzle_diameter.resize(config.nozzle_diameter.size());
        for (size_t i = 0; i < config.nozzle_diameter.size(); ++i)
            processor.m_nozzle_diameter[i] = config.nozzle_diameter.values[i];
    }
}

Slic3r::GCodeProcessor::TimeProcessor::InsertedLinesMap PreCooling::run_pre_scan(Slic3r::GCodeProcessor& processor, FILE* f)
{
    using FilamentUsageBlock = Slic3r::ExtruderPreHeating::FilamentUsageBlock;
    using ExtruderUsageBlcok = Slic3r::ExtruderPreHeating::ExtruderUsageBlcok;

    Slic3r::GCodeProcessor::TimeProcessor::InsertedLinesMap precooling_inserted_lines;

    std::vector<FilamentUsageBlock> filament_blocks;
    std::vector<ExtruderUsageBlcok> extruder_blocks = { ExtruderUsageBlcok() };
    ExtruderUsageBlcok temp_construct_block;

    unsigned int machine_start_gcode_end_line_id = 0;
    unsigned int machine_end_gcode_start_line_id = std::numeric_limits<unsigned int>::max();
    unsigned int pre_scan_layer_id = 0;

    auto handle_nozzle_change_line = [&processor](const std::string& line, int& old_filament, int& next_filament,
                                            int& extruder_id, int& old_nozzle_id, int& new_nozzle_id) -> bool {
        std::regex re(R"(OF(\d+)\s+NF(\d+)\s+ON(\d+)\s+NN(\d+))");
        std::smatch match;
        if (!std::regex_search(line, match, re))
            return false;
        old_filament = std::stoi(match[1]);
        next_filament = std::stoi(match[2]);
        old_nozzle_id = std::stoi(match[3]);
        new_nozzle_id = std::stoi(match[4]);
        auto nozzle_info = processor.m_nozzle_group_result->get_nozzle_from_id(new_nozzle_id);
        extruder_id = nozzle_info ? nozzle_info->extruder_id : -1;
        return true;
    };

    auto handle_filament_change = [&](int filament_id, int line_id, int nozzle_id = -1) {
        if (!filament_blocks.empty())
            filament_blocks.back().upper_gcode_id = line_id;

        if (nozzle_id == -1)
            nozzle_id = processor.m_nozzle_group_result->get_nozzle_id(filament_id, pre_scan_layer_id);

        int extruder_id = 0;
        auto nozzle_info = processor.m_nozzle_group_result->get_nozzle_from_id(nozzle_id);
        if (nozzle_info) extruder_id = nozzle_info->extruder_id;

        filament_blocks.emplace_back(filament_id, extruder_id, nozzle_id, line_id, -1);
    };

    std::fseek(f, 0, SEEK_SET);
    std::string scan_line;
    std::vector<char> scan_buffer(65536, 0);
    unsigned int scan_line_id = 0;

    for (;;) {
        size_t cnt_read = ::fread(scan_buffer.data(), 1, scan_buffer.size(), f);
        if (::ferror(f)) break;
        bool eof = cnt_read == 0;
        auto it = scan_buffer.begin();
        auto it_bufend = scan_buffer.begin() + cnt_read;
        while (it != it_bufend || (eof && !scan_line.empty())) {
            bool eol = false;
            auto it_end = it;
            for (; it_end != it_bufend && !(eol = *it_end == '\r' || *it_end == '\n'); ++it_end);
            eol |= eof && it_end == it_bufend;
            scan_line.insert(scan_line.end(), it, it_end);
            it = it_end;
            if (it != it_bufend && *it == '\r') scan_line += *it++;
            if (it != it_bufend && *it == '\n') scan_line += *it++;

            if (eol) {
                ++scan_line_id;

                if (scan_line.size() > 1 && scan_line.front() == ';') {
                    std::string_view sv(scan_line);
                    while (!sv.empty() && (sv.back() == '\n' || sv.back() == '\r')) sv.remove_suffix(1);
                    sv.remove_prefix(1);
                    if (sv == Slic3r::GCodeProcessor::reserved_tag(Slic3r::GCodeProcessor::ETags::Layer_Change))
                        ++pre_scan_layer_id;
                }

                if (Slic3r::GCodeReader::GCodeLine::cmd_starts_with(scan_line, "T")) {
                    int fid = -1;
                    std::string cmd = Slic3r::GCodeReader::GCodeLine::extract_cmd(scan_line);
                    if (cmd.size() >= 2) {
                        std::istringstream str(cmd.substr(1));
                        str >> fid;
                        if (!str.fail() && fid >= 0 && fid < 255) {
                            int nozzle_id = -1;
                            size_t h_pos = scan_line.find(" H");
                            if (h_pos != std::string::npos) {
                                std::istringstream hstr(scan_line.substr(h_pos + 2));
                                hstr >> nozzle_id;
                            }
                            handle_filament_change(fid, scan_line_id, nozzle_id);
                        }
                    }
                }
                else if (Slic3r::GCodeReader::GCodeLine::cmd_starts_with(scan_line, (std::string(";") + Slic3r::GCodeProcessor::reserved_tag(Slic3r::GCodeProcessor::ETags::NozzleChangeStart)).c_str())) {
                    int prev_filament{-1}, next_filament{-1}, ext_id{-1}, prev_nozzle{-1}, next_nozzle{-1};
                    handle_nozzle_change_line(scan_line, prev_filament, next_filament, ext_id, prev_nozzle, next_nozzle);
                    if (!extruder_blocks.empty())
                        extruder_blocks.back().initialize_step_2(scan_line_id);
                }
                else if (Slic3r::GCodeReader::GCodeLine::cmd_starts_with(scan_line, (std::string(";") + Slic3r::GCodeProcessor::reserved_tag(Slic3r::GCodeProcessor::ETags::NozzleChangeEnd)).c_str())) {
                    int prev_filament{-1}, next_filament{-1}, ext_id{-1}, prev_nozzle{-1}, next_nozzle{-1};
                    handle_nozzle_change_line(scan_line, prev_filament, next_filament, ext_id, prev_nozzle, next_nozzle);
                    if (!extruder_blocks.empty())
                        extruder_blocks.back().initialize_step_3(scan_line_id, prev_filament, scan_line_id, prev_nozzle);
                    temp_construct_block.initialize_step_1(ext_id, scan_line_id, next_filament, next_nozzle);
                    extruder_blocks.emplace_back(temp_construct_block);
                    temp_construct_block.reset();
                }

                scan_line.clear();
            }
        }
        if (eof) break;
    }

    if (!filament_blocks.empty())
        filament_blocks.back().upper_gcode_id = machine_end_gcode_start_line_id;

    if (!extruder_blocks.empty()) {
        int first_filament = 0, last_filament = 0;
        if (!filament_blocks.empty()) {
            first_filament = filament_blocks.front().filament_id;
            last_filament = filament_blocks.back().filament_id;
        }
        auto nozzle_info = processor.m_nozzle_group_result->get_first_nozzle_for_filament(first_filament);
        int ext_id = nozzle_info ? nozzle_info->extruder_id : -1;
        int start_nozzle = nozzle_info ? nozzle_info->group_id : -1;
        extruder_blocks.front().initialize_step_1(ext_id, machine_start_gcode_end_line_id, first_filament, start_nozzle);
        extruder_blocks.back().initialize_step_2(machine_end_gcode_start_line_id);
        int last_nozzle = filament_blocks.empty() ? -1 : filament_blocks.back().nozzle_id;
        extruder_blocks.back().initialize_step_3(machine_end_gcode_start_line_id, last_filament, machine_end_gcode_start_line_id, last_nozzle);
    }

    {
        int curr_filament = -1;
        int total_filament_count = 0;
        for (const auto& fb : filament_blocks) {
            if (curr_filament != -1 && curr_filament != fb.filament_id)
                total_filament_count += 1;
            curr_filament = fb.filament_id;
        }
        curr_filament = -1;
        int curr_filament_change_num = 0;
        for (const auto& fb : filament_blocks) {
            if (curr_filament != -1 && curr_filament != fb.filament_id) {
                curr_filament_change_num += 1;
                char buf[64];
                snprintf(buf, sizeof(buf), "M73 E%d\n", total_filament_count - curr_filament_change_num);
                precooling_inserted_lines[fb.lower_gcode_id].emplace_back(
                    std::string(buf), Slic3r::GCodeProcessor::TimeProcessor::InsertLineType::FilamentChangePredict);
            }
            curr_filament = fb.filament_id;
        }
        BOOST_LOG_TRIVIAL(info) << "M73 E: total filament changes = " << total_filament_count;
    }

    size_t valid_machine_id = 0;
    for (size_t i = 0; i < static_cast<size_t>(Slic3r::PrintEstimatedStatistics::ETimeMode::Count); ++i) {
        if (processor.m_time_processor.machines[i].enabled) {
            valid_machine_id = i;
            break;
        }
    }

    std::vector<int> filament_nozzle_temps_int(processor.m_filament_nozzle_temp.begin(), processor.m_filament_nozzle_temp.end());
    std::vector<int> filament_nozzle_temps_fl_int(processor.m_filament_nozzle_temp_first_layer.begin(), processor.m_filament_nozzle_temp_first_layer.end());
    std::vector<std::pair<unsigned int, unsigned int>> skippable_blocks;

    auto pre_cooling_injector = std::make_unique<PreCooling>(
        processor.m_result.moves,
        processor.m_filament_types,
        *processor.m_nozzle_group_result,
        filament_nozzle_temps_int,
        filament_nozzle_temps_fl_int,
        processor.m_physical_extruder_map,
        valid_machine_id,
        processor.m_inject_time_threshold,
        false, // handle_hotend_as_extruder
        processor.m_has_filament_switcher,
        processor.m_pre_cooling_temp,
        processor.m_cooling_rate,
        processor.m_heating_rate,
        skippable_blocks,
        processor.m_extruder_max_nozzle_count,
        processor.m_filament_preheat_temperature_delta,
        processor.m_filament_max_temperature_drop_when_ec,
        machine_start_gcode_end_line_id,
        machine_end_gcode_start_line_id,
        processor.m_extruder_types,
        processor.m_nozzle_diameter
    );

    pre_cooling_injector->build_extruder_free_blocks(filament_blocks, extruder_blocks);
    pre_cooling_injector->process_pre_cooling_and_heating(precooling_inserted_lines);

    BOOST_LOG_TRIVIAL(info) << "PreCoolingInjector: generated " << precooling_inserted_lines.size() << " injection points";

    std::fseek(f, 0, SEEK_SET);
    return precooling_inserted_lines;
}

void PreCooling::inject_lines(
    Slic3r::GCodeProcessor::TimeProcessor::InsertedLinesMap::iterator& precooling_iter,
    const Slic3r::GCodeProcessor::TimeProcessor::InsertedLinesMap& precooling_inserted_lines,
    bool enable_pre_heating,
    unsigned int line_id,
    std::function<void(const std::string&)> append_line_fn
)
{
    if (precooling_iter != precooling_inserted_lines.end()) {
        if (static_cast<int>(line_id) == precooling_iter->first) {
            for (auto& elem : precooling_iter->second) {
                const std::string& str = elem.first;
                const Slic3r::GCodeProcessor::TimeProcessor::InsertLineType type = elem.second;
                switch (type) {
                    case Slic3r::GCodeProcessor::TimeProcessor::InsertLineType::PreCooling:
                    case Slic3r::GCodeProcessor::TimeProcessor::InsertLineType::PreHeating:
                        if (enable_pre_heating)
                            append_line_fn(str);
                        break;
                    case Slic3r::GCodeProcessor::TimeProcessor::InsertLineType::FilamentChangePredict:
                        append_line_fn(str);
                        break;
                    default:
                        break;
                }
            }
            ++precooling_iter;
        }
    }
}

} // namespace Vortek
