#include "VortekPlateMapping.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/MultiNozzleUtils.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include <boost/algorithm/string.hpp>

namespace Vortek {

// Check if printer is H2C dual-nozzle system
bool PlateMapping::is_h2c_multi_nozzle(const Slic3r::Print* print)
{
    if (!print) return false;
    const auto& config = print->config();
    return print->is_BBL_printer() &&
           (config.nozzle_diameter.size() > 1) &&
           (config.extruder_max_nozzle_count.values.size() > 1) &&
           (config.extruder_max_nozzle_count.values[1] > 1);
}

// Synchronize filament, volume, and nozzle maps after slicing finishes
void PlateMapping::sync_after_slicing(
    Slic3r::GUI::PartPlate* current_plate, 
    const Slic3r::Print* print, 
    Slic3r::PresetBundle& preset_bundle
)
{
    if (!current_plate || !print) return;

    bool is_h2c = is_h2c_multi_nozzle(print);

    // Update nozzle/volume flow mapping options in UI configuration
    if (is_h2c || current_plate->get_real_filament_map_mode(preset_bundle.project_config) < Slic3r::FilamentMapMode::fmmManual) {
        current_plate->set_filament_maps(print->get_filament_maps());
        current_plate->set_filament_volume_maps(print->get_filament_volume_maps());
    }
    if (is_h2c || current_plate->get_real_filament_map_mode(preset_bundle.project_config) != Slic3r::FilamentMapMode::fmmNozzleManual) {
        current_plate->set_filament_nozzle_maps(print->get_filament_nozzle_maps());
    }
    // For H2C, update preset_bundle project config vectors directly to align with slicing output
    if (is_h2c) {
        preset_bundle.project_config.option<Slic3r::ConfigOptionInts>("filament_map", true)->values = print->get_filament_maps();
        preset_bundle.project_config.option<Slic3r::ConfigOptionInts>("filament_volume_map", true)->values = print->get_filament_volume_maps();
        preset_bundle.project_config.option<Slic3r::ConfigOptionInts>("filament_nozzle_map", true)->values = print->get_filament_nozzle_maps();
    }
}

// Resizes custom mapping vectors to stay in sync with updated filament count
void PlateMapping::handle_filament_count_changed(Slic3r::GUI::PartPlate* plate, int filament_count)
{
    if (!plate) return;
    auto* config = plate->config();
    if (config->has("filament_nozzle_map")) {
        auto& filament_nozzle_maps = config->option<Slic3r::ConfigOptionInts>("filament_nozzle_map")->values;
        filament_nozzle_maps.resize(filament_count, 0);
    }
    if (config->has("filament_volume_map")) {
        auto& filament_volume_maps = config->option<Slic3r::ConfigOptionInts>("filament_volume_map")->values;
        filament_volume_maps.resize(filament_count, 0);
    }
}

// Appends placeholder value to vectors when new filament is added
void PlateMapping::handle_filament_added(Slic3r::GUI::PartPlate* plate)
{
    if (!plate) return;
    auto* config = plate->config();
    if (config->has("filament_nozzle_map")) {
        config->option<Slic3r::ConfigOptionInts>("filament_nozzle_map")->values.push_back(0);
    }
    if (config->has("filament_volume_map")) {
        config->option<Slic3r::ConfigOptionInts>("filament_volume_map")->values.push_back(0);
    }
}

// Erases specific mapping entry when filament is removed
void PlateMapping::handle_filament_deleted(Slic3r::GUI::PartPlate* plate, int filament_id)
{
    if (!plate) return;
    auto* config = plate->config();
    if (config->has("filament_nozzle_map")) {
        auto& filament_nozzle_maps = config->option<Slic3r::ConfigOptionInts>("filament_nozzle_map")->values;
        if (filament_id >= 0 && filament_id < (int)filament_nozzle_maps.size())
            filament_nozzle_maps.erase(filament_nozzle_maps.begin() + filament_id);
    }
    if (config->has("filament_volume_map")) {
        auto& filament_volume_maps = config->option<Slic3r::ConfigOptionInts>("filament_volume_map")->values;
        if (filament_id >= 0 && filament_id < (int)filament_volume_maps.size())
            filament_volume_maps.erase(filament_volume_maps.begin() + filament_id);
    }
}

// Clear custom plate mappings (returns them to defaults)
void PlateMapping::clear_mappings(Slic3r::GUI::PartPlate* plate)
{
    if (!plate) return;
    plate->clear_filament_nozzle_map();
    plate->clear_filament_volume_map();
}

// Parse nozzle and volume type attributes from loaded 3MF metadata
void PlateMapping::load_from_3mf_structure(
    Slic3r::GUI::PartPlate* plate,
    const Slic3r::PlateData* plate_data,
    int filament_count,
    Slic3r::GCodeProcessorResult* gcode_result
)
{
    if (!plate || !plate_data || !gcode_result) return;

    // Helper lambda to tokenize value strings
    auto parse_values = [](const std::string& value, const char* seps, auto to_value) {
        using T = decltype(to_value(std::string()));
        std::vector<T> result;
        if (value.empty()) {
            return result;
        }
        std::vector<std::string> tokens;
        boost::split(tokens, value, boost::is_any_of(seps), boost::token_compress_on);
        for (const auto& token : tokens) {
            if (token.empty()) {
                continue;
            }
            try {
                result.emplace_back(to_value(token));
            } catch (...) {}
        }
        return result;
    };

    // Parse nozzle volume types and diameters
    std::vector<int> nozzle_volume_type_values = parse_values(plate_data->nozzle_volume_types, " ",
                                                              [](const std::string& token) { return std::stoi(token); });

    std::vector<double> nozzle_diameter_values = parse_values(plate_data->nozzle_diameters, " ,",
                                                              [](const std::string& token) { return std::stod(token); });

    std::vector<Slic3r::NozzleVolumeType> extruder_volume_types(nozzle_volume_type_values.size(), Slic3r::NozzleVolumeType::nvtStandard);

    if (!nozzle_volume_type_values.empty()) {
        for (size_t idx = 0; idx < nozzle_volume_type_values.size(); ++idx) {
            if (nozzle_volume_type_values[idx] >= 0) {
                extruder_volume_types[idx] = static_cast<Slic3r::NozzleVolumeType>(nozzle_volume_type_values[idx]);
            }
        }
    }

    // Resolve compatibility and group nozzle configurations
    auto nozzle_infos = Slic3r::MultiNozzleUtils::load_nozzle_infos_with_compatibility(plate_data->nozzles_info,
                                                                                plate_data->slice_filaments_info,
                                                                                plate_data->filament_maps, extruder_volume_types,
                                                                                nozzle_diameter_values);

    std::vector<unsigned int> used_fils;
    for (const auto& fil : plate_data->slice_filaments_info) {
        if (fil.used_for_object || fil.used_for_support) {
            used_fils.push_back(static_cast<unsigned int>(fil.id));
        }
    }
    std::sort(used_fils.begin(), used_fils.end());
    used_fils.erase(std::unique(used_fils.begin(), used_fils.end()), used_fils.end());
    if (used_fils.empty()) {
        for (int f_id = 0; f_id < filament_count; ++f_id) {
            used_fils.push_back(static_cast<unsigned int>(f_id));
        }
    }

    // Build local mapping vectors from loaded slice filaments info
    std::vector<int> filament_nozzle_map(filament_count, 0);
    std::vector<int> filament_volume_map(filament_count, 0);
    auto volume_type_str_to_enum = Slic3r::ConfigOptionEnum<Slic3r::NozzleVolumeType>::get_enum_values();
    for (const auto& fil_info : plate_data->slice_filaments_info) {
        if (fil_info.id >= 0 && fil_info.id < filament_count) {
            filament_nozzle_map[fil_info.id] = fil_info.group_id.empty() ? 0 : fil_info.group_id.front();
            if (volume_type_str_to_enum.count(fil_info.nozzle_volume_type)) {
                filament_volume_map[fil_info.id] = volume_type_str_to_enum.at(fil_info.nozzle_volume_type);
            }
        }
    }

    // Store maps onto active plate
    plate->set_filament_nozzle_maps(filament_nozzle_map);
    plate->set_filament_volume_maps(filament_volume_map);

    // Initialize layered nozzle groups inside gcode_result
    auto group_result = Slic3r::MultiNozzleUtils::LayeredNozzleGroupResult::create(filament_nozzle_map, nozzle_infos, used_fils);
    if (group_result)
        gcode_result->nozzle_group_result = std::make_shared<Slic3r::MultiNozzleUtils::LayeredNozzleGroupResult>(group_result.value());
    else
        gcode_result->nozzle_group_result = nullptr;
}

// Resize vectors inside configuration to match filament count when loading projects
void PlateMapping::sync_project_config_on_load(Slic3r::DynamicConfig& proj_cfg, int filament_count)
{
    Slic3r::ConfigOptionInts* filament_nozzle_map = proj_cfg.opt<Slic3r::ConfigOptionInts>("filament_nozzle_map", true);
    if (filament_nozzle_map->size() != filament_count) {
        filament_nozzle_map->values.resize(filament_count, 0);
    }

    Slic3r::ConfigOptionInts* filament_volume_map = proj_cfg.opt<Slic3r::ConfigOptionInts>("filament_volume_map", true);
    if (filament_volume_map->size() != filament_count) {
        filament_volume_map->values.resize(filament_count, 0);
    }
}

} // namespace Vortek
