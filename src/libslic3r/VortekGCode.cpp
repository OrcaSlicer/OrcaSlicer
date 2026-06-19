// ============================================================================
// VortekGCode.cpp
//
// Implements the Vortek::GCode class to handle parser integration
// and filament start G-code blocks.
// ============================================================================

#include "VortekGCode.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/MultiNozzleUtils.hpp"
#include "PlaceholderParser.hpp"

#include <vector>
#include <string>

// Helper macro to get the correct nozzle ID for G-code context.
// Returns -1 if dynamic nozzle mapping is disabled or unsupported.
#define NOZZLE_ID_FOR_GCODE(RESULT, ID) ((RESULT) && (RESULT)->is_support_dynamic_nozzle_map() ? (ID) : -1)

namespace Vortek {

/**
 * @brief Internal helper to build nozzle diameter lookup arrays based on the multi-nozzle configuration.
 * @param group_result Description of active nozzle groupings.
 * @return List of nozzle diameters ordered by nozzle group IDs.
 */
static std::vector<double> get_nozzle_diameters_by_nozzle_id(const Slic3r::MultiNozzleUtils::NozzleGroupResultBase *group_result)
{
    std::vector<double> diameters;
    if (!group_result)
        return diameters;
    for (int id = 0;; ++id) {
        auto nozzle = group_result->get_nozzle_from_id(id);
        if (!nozzle)
            break;
        diameters.push_back(std::stod(nozzle->diameter));
    }
    return diameters;
}

/**
 * @brief Registers all H2C/Vortek custom parameters into the G-code generator's placeholder parser.
 * 
 * Sets up properties such as nozzle diameters, starting filament mapping tables, active initial hotends,
 * retraction distances when cut, and retraction distances when performing nozzle changes (EC - Extruder Change).
 */
void GCode::init(
    Slic3r::GCode& gcode,
    int initial_extruder_id,
    int initial_non_support_extruder_id,
    std::vector<int>& first_filaments,
    std::vector<int>& first_non_support_filaments
)
{
    // Local helper to align first filaments order with the physical extruders map
    auto match_physical_extruder_for_each_filament = [](std::vector<int> &filaments, const Slic3r::FullPrintConfig &config) {
        std::vector<int> physicial_first_filaments;
        physicial_first_filaments.resize(filaments.size());
        for (size_t extruder_id = 0; extruder_id < filaments.size(); extruder_id++) {
            physicial_first_filaments[config.physical_extruder_map.get_at(extruder_id)] = filaments[extruder_id];
        }
        filaments = physicial_first_filaments;
    };

    match_physical_extruder_for_each_filament(first_filaments, gcode.m_config);

    // Register active first tools, filaments, and initial extruder parameters in parser context
    gcode.placeholder_parser().set("first_tools", new Slic3r::ConfigOptionInts(first_filaments));
    gcode.placeholder_parser().set("first_filaments", new Slic3r::ConfigOptionInts(first_filaments));
    gcode.placeholder_parser().set("initial_tool", initial_extruder_id);
    gcode.placeholder_parser().set("initial_extruder", initial_extruder_id);

    match_physical_extruder_for_each_filament(first_non_support_filaments, gcode.m_config);

    auto group_result = gcode.m_print->get_layered_nozzle_group_result();

    // Map non-support tools/hotends to their respective starting nozzles
    std::vector<int> first_non_support_hotends;
    first_non_support_hotends.reserve(first_non_support_filaments.size());
    for (int filament_id : first_non_support_filaments) {
        if (filament_id < 0) {
            first_non_support_hotends.push_back(-1);
            continue;
        }
        first_non_support_hotends.push_back(
            NOZZLE_ID_FOR_GCODE(group_result,
                group_result->get_first_nozzle_for_filament(filament_id)->group_id));
    }

    // Register non-support tool mappings
    gcode.placeholder_parser().set("first_non_support_tools", new Slic3r::ConfigOptionInts(first_non_support_filaments));
    gcode.placeholder_parser().set("first_non_support_filaments", new Slic3r::ConfigOptionInts(first_non_support_filaments));
    gcode.placeholder_parser().set("first_non_support_hotend", new Slic3r::ConfigOptionInts(first_non_support_hotends));
    gcode.placeholder_parser().set("initial_no_support_tool", initial_non_support_extruder_id);
    gcode.placeholder_parser().set("initial_no_support_extruder", initial_non_support_extruder_id);
    gcode.placeholder_parser().set("initial_no_support_hotend",
        NOZZLE_ID_FOR_GCODE(group_result,
            group_result->get_first_nozzle_for_filament(initial_non_support_extruder_id)->group_id));
    
    // Register active extruder and hotend parameters
    gcode.placeholder_parser().set("current_extruder", initial_extruder_id);
    gcode.placeholder_parser().set("current_hotend",
        NOZZLE_ID_FOR_GCODE(group_result,
            group_result->get_first_nozzle_for_filament(initial_extruder_id)->group_id));
    
    gcode.placeholder_parser().set("initial_filament_id", (int)initial_extruder_id);
    gcode.placeholder_parser().set("initial_extruder_id", (int)gcode.get_extruder_id(initial_extruder_id));
    gcode.placeholder_parser().set("initial_nozzle_id",
        group_result->get_first_nozzle_for_filament(initial_extruder_id)->group_id);
    
    gcode.placeholder_parser().set("initial_no_support_filament_id", (int)initial_non_support_extruder_id);
    gcode.placeholder_parser().set("initial_no_support_extruder_id", (int)gcode.get_extruder_id(initial_non_support_extruder_id));
    gcode.placeholder_parser().set("initial_no_support_nozzle_id",
        group_result->get_first_nozzle_for_filament(initial_non_support_extruder_id)->group_id);
    
    gcode.placeholder_parser().set("nozzle_diameter_at_nozzle_id",
        new Slic3r::ConfigOptionFloats(get_nozzle_diameters_by_nozzle_id(group_result.get())));

    // Register hardware-specific retraction distance configurations
    gcode.placeholder_parser().set("retraction_distance_when_cut", gcode.m_config.retraction_distances_when_cut.get_at(initial_extruder_id));
    gcode.placeholder_parser().set("long_retraction_when_cut", gcode.m_config.long_retractions_when_cut.get_at(initial_extruder_id));
    gcode.placeholder_parser().set("retraction_distance_when_ec", gcode.m_config.retraction_distances_when_ec.get_at(initial_extruder_id));
    gcode.placeholder_parser().set("long_retraction_when_ec", gcode.m_config.long_retractions_when_ec.get_at(initial_extruder_id));
}

/**
 * @brief Parses and outputs the startup G-code script for initial filaments.
 * 
 * Inserts the initial nozzle mapping comments (e.g. `;VT0 H0`) to sync the printer's extruder state.
 */
void GCode::write_filament_start(
    Slic3r::GCode& gcode,
    int initial_extruder_id,
    int initial_non_support_extruder_id,
    Slic3r::GCode::GCodeOutputStream& file,
    Slic3r::Print& print
)
{
    auto group_result = gcode.m_print->get_layered_nozzle_group_result();

    gcode.m_writer.init_extruder(initial_non_support_extruder_id);
    {
        Slic3r::DynamicConfig config;
        config.set_key_value("filament_extruder_id", new Slic3r::ConfigOptionInt((int)(initial_non_support_extruder_id)));
        config.set_key_value("current_filament_id", new Slic3r::ConfigOptionInt((int)(initial_non_support_extruder_id)));
        config.set_key_value("current_extruder_id", new Slic3r::ConfigOptionInt((int)gcode.get_extruder_id(initial_non_support_extruder_id)));
        config.set_key_value("current_nozzle_id",
            new Slic3r::ConfigOptionInt(group_result->get_first_nozzle_for_filament(initial_non_support_extruder_id)->group_id));
        config.set_key_value("nozzle_diameter_at_nozzle_id",
            new Slic3r::ConfigOptionFloats(get_nozzle_diameters_by_nozzle_id(group_result.get())));
        config.set_key_value("layer_num", new Slic3r::ConfigOptionInt(gcode.m_layer_index));

        // Evaluate filament start G-code blocks using placeholder replacements
        std::string filament_start_gcode = gcode.placeholder_parser_process("filament_start_gcode",
            print.config().filament_start_gcode.values.at(initial_non_support_extruder_id),
            initial_non_support_extruder_id, &config);
        file.writeln(filament_start_gcode);
        
        // Write the custom Vortek tag matching initial extruder to initial nozzle index
        int initial_nozzle_id = NOZZLE_ID_FOR_GCODE(group_result,
            group_result->get_first_nozzle_for_filament(initial_extruder_id)->group_id);
        file.write_format(";VT%d H%d\n", initial_extruder_id, initial_nozzle_id);
    }
}

int GCode::get_original_filament_index(const Slic3r::GCode& gcode, int filament_id)
{
    auto group_result = gcode.m_print->get_layered_nozzle_group_result();
    if (!group_result)
        return filament_id;

    auto nozzle_info = group_result->get_first_nozzle_for_filament(filament_id);
    if (!nozzle_info)
        return filament_id;

    auto opt_extruder_type = dynamic_cast<const Slic3r::ConfigOptionEnumsGeneric*>(gcode.m_config.option("extruder_type"));
    if (!opt_extruder_type)
        return filament_id;

    Slic3r::ExtruderType extruder_type = Slic3r::ExtruderType(opt_extruder_type->get_at(nozzle_info->extruder_id));
    Slic3r::NozzleVolumeType nozzle_volume_type = nozzle_info->volume_type;

    int idx = gcode.m_print->ori_full_print_config().get_index_for_extruder(
        filament_id + 1,
        "filament_self_index",
        extruder_type,
        nozzle_volume_type,
        "filament_extruder_variant"
    );

    return idx >= 0 ? idx : filament_id;
}

std::vector<double> GCode::remap_floats_by_filament_vortek(const Slic3r::GCode& gcode, const std::string& key, size_t num_filaments)
{
    std::vector<double> dst(num_filaments, 0.0);
    auto opt = dynamic_cast<const Slic3r::ConfigOptionFloats*>(gcode.m_print->ori_full_print_config().option(key));
    for (size_t i = 0; i < num_filaments; ++i) {
        int idx = get_original_filament_index(gcode, i);
        if (opt && idx >= 0 && idx < (int)opt->values.size()) {
            dst[i] = opt->get_at(idx);
        } else {
            auto fallback_opt = dynamic_cast<const Slic3r::ConfigOptionFloats*>(gcode.m_config.option(key));
            if (fallback_opt && i < fallback_opt->values.size())
                dst[i] = fallback_opt->get_at(i);
        }
    }
    return dst;
}

std::vector<int> GCode::remap_ints_by_filament_vortek(const Slic3r::GCode& gcode, const std::string& key, size_t num_filaments)
{
    std::vector<int> dst(num_filaments, 0);
    auto opt = dynamic_cast<const Slic3r::ConfigOptionInts*>(gcode.m_print->ori_full_print_config().option(key));
    for (size_t i = 0; i < num_filaments; ++i) {
        int idx = get_original_filament_index(gcode, i);
        if (opt && idx >= 0 && idx < (int)opt->values.size()) {
            dst[i] = opt->get_at(idx);
        } else {
            auto fallback_opt = dynamic_cast<const Slic3r::ConfigOptionInts*>(gcode.m_config.option(key));
            if (fallback_opt && i < fallback_opt->values.size())
                dst[i] = fallback_opt->get_at(i);
        }
    }
    return dst;
}

void GCode::update_placeholder_parser_with_variant_params(Slic3r::GCode& gcode)
{
    size_t num_filaments = gcode.m_config.filament_type.values.size();
    if (num_filaments == 0)
        return;

    gcode.placeholder_parser().set("filament_max_volumetric_speed",       new Slic3r::ConfigOptionFloats(remap_floats_by_filament_vortek(gcode, "filament_max_volumetric_speed", num_filaments)));
    gcode.placeholder_parser().set("filament_pre_cooling_temperature",    new Slic3r::ConfigOptionInts(remap_ints_by_filament_vortek(gcode, "filament_pre_cooling_temperature", num_filaments)));
    gcode.placeholder_parser().set("filament_pre_cooling_temperature_nc", new Slic3r::ConfigOptionInts(remap_ints_by_filament_vortek(gcode, "filament_pre_cooling_temperature_nc", num_filaments)));
    gcode.placeholder_parser().set("filament_ramming_travel_time",       new Slic3r::ConfigOptionFloats(remap_floats_by_filament_vortek(gcode, "filament_ramming_travel_time", num_filaments)));
    gcode.placeholder_parser().set("filament_ramming_travel_time_nc",    new Slic3r::ConfigOptionFloats(remap_floats_by_filament_vortek(gcode, "filament_ramming_travel_time_nc", num_filaments)));
    gcode.placeholder_parser().set("filament_cooling_before_tower",       new Slic3r::ConfigOptionFloats(remap_floats_by_filament_vortek(gcode, "filament_cooling_before_tower", num_filaments)));
    gcode.placeholder_parser().set("nozzle_temperature_initial_layer",    new Slic3r::ConfigOptionInts(remap_ints_by_filament_vortek(gcode, "nozzle_temperature_initial_layer", num_filaments)));
    gcode.placeholder_parser().set("nozzle_temperature",                  new Slic3r::ConfigOptionInts(remap_ints_by_filament_vortek(gcode, "nozzle_temperature", num_filaments)));
    gcode.placeholder_parser().set("first_layer_temperature",             new Slic3r::ConfigOptionInts(remap_ints_by_filament_vortek(gcode, "nozzle_temperature_initial_layer", num_filaments)));

    gcode.placeholder_parser().set("retraction_distances_when_cut",       new Slic3r::ConfigOptionFloats(remap_floats_by_filament_vortek(gcode, "retraction_distances_when_cut", num_filaments)));
    gcode.placeholder_parser().set("filament_map", new Slic3r::ConfigOptionInts(gcode.m_config.filament_map));

    {
        auto flush_v_speed  = remap_floats_by_filament_vortek(gcode, "filament_flush_volumetric_speed", num_filaments);
        auto filament_max_v = remap_floats_by_filament_vortek(gcode, "filament_max_volumetric_speed", num_filaments);
        auto flush_temps    = remap_ints_by_filament_vortek(gcode, "filament_flush_temp", num_filaments);
        for (size_t i = 0; i < num_filaments; ++i) {
            if (flush_v_speed[i] == 0)
                flush_v_speed[i] = filament_max_v[i];
            if (flush_temps[i] == 0) {
                auto opt = dynamic_cast<const Slic3r::ConfigOptionInts*>(gcode.m_config.option("nozzle_temperature_range_high"));
                if (opt && i < opt->values.size())
                    flush_temps[i] = opt->get_at(i);
            }
        }
        gcode.placeholder_parser().set("flush_volumetric_speeds", new Slic3r::ConfigOptionFloats(flush_v_speed));
        gcode.placeholder_parser().set("flush_temperatures",      new Slic3r::ConfigOptionInts(flush_temps));
    }
}

} // namespace Vortek
