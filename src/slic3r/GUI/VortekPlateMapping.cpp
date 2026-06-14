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
        // Propagate has_filament_switcher into project config so that
        // slice_info.config serialization (bbs_3mf.cpp) picks up the correct value.
        // H2C dual-nozzle printers always have a filament track switcher (FTS).
        preset_bundle.project_config.set_key_value("has_filament_switcher",
            new Slic3r::ConfigOptionBool(true));
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

void PlateMapping::patch_export_config(Slic3r::DynamicPrintConfig& cfg)
{
    // H2C (SEMM dual-nozzle) printers always have a filament track switcher.
    auto* nozzle_diam = cfg.option<Slic3r::ConfigOptionFloats>("nozzle_diameter");
    auto* semm = cfg.option<Slic3r::ConfigOptionBool>("single_extruder_multi_material");
    if (nozzle_diam && nozzle_diam->values.size() > 1 && semm && semm->value) {
        cfg.set_key_value("has_filament_switcher", new Slic3r::ConfigOptionBool(true));
    }
}

// ─── H2C export-time patching ───────────────────────────────────────────────
//
// The upstream nozzle_group_result pipeline is fragile:
//   Print::export_gcode → get_layered_nozzle_group_result →
//   GCodeProcessorResult::nozzle_group_result →
//   PlateData::parse_filament_info → dynamic_pointer_cast →
//   PlateData::nozzle_group_result → bbs_3mf serialization
//
// When any link in this chain fails (e.g. wipe tower not generated,
// backend creates wrong subtype, shared_ptr is nullptr), the serializer
// falls back to a dumb filament_maps-based heuristic that produces
// group_id=1 for ALL filaments on extruder 2 and writes a single
// <nozzle> entry.
//
// This method provides a DIRECT, ISOLATED path: it reads the plate's own
// filament_nozzle_map (which Vortek::sync_after_slicing always maintains)
// and builds nozzles_info + patches FilamentInfo::group_id WITHOUT touching
// nozzle_group_result at all.
//
void PlateMapping::patch_plate_data_for_export(
    Slic3r::PlateData* plate_data,
    const Slic3r::GUI::PartPlate* plate,
    const Slic3r::DynamicPrintConfig& config)
{
    if (!plate_data || !plate) return;

    // ── Guard: only for H2C multi-nozzle printers ──
    auto* nozzle_diam_opt = config.option<Slic3r::ConfigOptionFloats>("nozzle_diameter");
    // extruder_max_nozzle_count is ConfigOptionIntsNullable in PrintConfig.hpp
    auto* max_nozzle_count_opt = config.option<Slic3r::ConfigOptionIntsNullable>("extruder_max_nozzle_count");
    if (!nozzle_diam_opt || nozzle_diam_opt->values.size() <= 1)
        return; // single-extruder — not H2C
    if (!max_nozzle_count_opt || max_nozzle_count_opt->values.size() <= 1 ||
        max_nozzle_count_opt->values[1] <= 1)
        return; // not a carousel multi-nozzle system

    // ── Gather plate mapping data ──
    const std::vector<int> filament_nozzle_map = plate->get_filament_nozzle_maps();
    const std::vector<int> filament_volume_map = plate->get_filament_volume_maps();
    const std::vector<int> filament_maps       = plate->get_filament_maps();

    if (filament_nozzle_map.empty())
        return; // nothing to patch

    const size_t extruder_count = nozzle_diam_opt->values.size();
    auto* nozzle_volume_type_opt = config.option<Slic3r::ConfigOptionEnumsGeneric>("nozzle_volume_type");

    // Helper: get nozzle diameter string for a given extruder
    auto get_diameter_str = [&](int extruder_id) -> std::string {
        double diam = 0.4;
        if (extruder_id >= 0 && extruder_id < static_cast<int>(nozzle_diam_opt->values.size()))
            diam = nozzle_diam_opt->values[extruder_id];
        else if (!nozzle_diam_opt->values.empty())
            diam = nozzle_diam_opt->values.back();
        // Format to canonical string like "0.40"
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.2f", diam);
        return std::string(buf);
    };

    // Helper: get volume type for a given extruder
    auto get_volume_type = [&](int extruder_id) -> Slic3r::NozzleVolumeType {
        if (!nozzle_volume_type_opt || nozzle_volume_type_opt->values.empty())
            return Slic3r::NozzleVolumeType::nvtStandard;
        if (extruder_id >= 0 && extruder_id < static_cast<int>(nozzle_volume_type_opt->values.size()))
            return static_cast<Slic3r::NozzleVolumeType>(nozzle_volume_type_opt->values[extruder_id]);
        return static_cast<Slic3r::NozzleVolumeType>(nozzle_volume_type_opt->values.back());
    };

    // ── Build unique nozzle list from used filaments ──
    // The upstream ToolOrdering pipeline collapses all carousel filaments into
    // a single nozzle group (e.g., nozzle_id=1) because they share the same
    // (extruder, diameter, volume_type). For H2C export, each physical carousel
    // slot must have a unique nozzle ID so the firmware routes filaments correctly.
    //
    // Strategy: extruder-1 filaments → nozzle 0, carousel filaments → unique IDs
    // starting from 1, assigned sequentially per filament.

    std::map<int, Slic3r::MultiNozzleUtils::NozzleInfo> nozzle_map;

    // First pass: determine extruder_id for each filament in slice_filaments_info
    // and reassign unique nozzle IDs for carousel (extruder 2) filaments.
    int next_carousel_nozzle_id = 1; // carousel nozzle IDs start at 1
    // Map from filament_id → reassigned nozzle_id
    std::map<int, int> reassigned_nozzle_ids;

    for (const auto& fil_info : plate_data->slice_filaments_info) {
        int fil_id = fil_info.id;
        if (fil_id < 0 || fil_id >= static_cast<int>(filament_maps.size()))
            continue;

        int extruder_id = 1; // default: carousel side
        if (filament_maps[fil_id] > 0)
            extruder_id = filament_maps[fil_id] - 1; // filament_maps is 1-indexed

        if (extruder_id == 0) {
            // Extruder 1 (left head) — always nozzle 0
            reassigned_nozzle_ids[fil_id] = 0;
        } else {
            // Carousel (extruder 2) — each filament gets its own unique nozzle slot
            reassigned_nozzle_ids[fil_id] = next_carousel_nozzle_id++;
        }
    }

    {
        // Diagnostic: dump input data for debugging nozzle assignment
        std::string nm_str = "[", fm_str = "[", vm_str = "[", rn_str = "[";
        for (size_t i = 0; i < filament_nozzle_map.size(); ++i) {
            if (i) nm_str += ",";
            nm_str += std::to_string(filament_nozzle_map[i]);
        }
        nm_str += "]";
        for (size_t i = 0; i < filament_maps.size(); ++i) {
            if (i) fm_str += ",";
            fm_str += std::to_string(filament_maps[i]);
        }
        fm_str += "]";
        for (size_t i = 0; i < filament_volume_map.size(); ++i) {
            if (i) vm_str += ",";
            vm_str += std::to_string(filament_volume_map[i]);
        }
        vm_str += "]";
        for (auto& [fid, nid] : reassigned_nozzle_ids) {
            if (rn_str.size() > 1) rn_str += ",";
            rn_str += std::to_string(fid) + "->" + std::to_string(nid);
        }
        rn_str += "]";
        BOOST_LOG_TRIVIAL(info) << "Vortek::patch_plate_data_for_export: "
            << "filament_nozzle_map=" << nm_str
            << " filament_maps=" << fm_str
            << " filament_volume_map=" << vm_str
            << " reassigned=" << rn_str
            << " slice_filaments_info.size=" << plate_data->slice_filaments_info.size()
            << " nozzle_group_result.has_value=" << plate_data->nozzle_group_result.has_value();
    }

    // Second pass: patch each FilamentInfo and build the nozzle list
    for (auto& fil_info : plate_data->slice_filaments_info) {
        int fil_id = fil_info.id;
        auto it = reassigned_nozzle_ids.find(fil_id);
        if (it == reassigned_nozzle_ids.end())
            continue;

        int nozzle_id = it->second;

        // Determine which physical extruder (0-indexed) this filament is on
        int extruder_id = 1; // default: extruder 2 (carousel side) for H2C
        if (fil_id < static_cast<int>(filament_maps.size()) && filament_maps[fil_id] > 0)
            extruder_id = filament_maps[fil_id] - 1; // filament_maps is 1-indexed

        // Determine volume type from filament's volume map
        Slic3r::NozzleVolumeType vol_type = Slic3r::NozzleVolumeType::nvtStandard;
        if (fil_id < static_cast<int>(filament_volume_map.size()))
            vol_type = static_cast<Slic3r::NozzleVolumeType>(filament_volume_map[fil_id]);
        if (vol_type == Slic3r::NozzleVolumeType::nvtStandard)
            vol_type = get_volume_type(extruder_id);

        // Patch FilamentInfo with unique group_id per carousel slot
        fil_info.group_id = {nozzle_id};
        fil_info.nozzle_diameter = nozzle_diam_opt->values[
            std::min(static_cast<size_t>(extruder_id), nozzle_diam_opt->values.size() - 1)];
        fil_info.nozzle_volume_type = Slic3r::get_nozzle_volume_type_string(vol_type);

        // Register nozzle if not seen
        if (nozzle_map.find(nozzle_id) == nozzle_map.end()) {
            Slic3r::MultiNozzleUtils::NozzleInfo ni;
            ni.group_id    = nozzle_id;
            ni.extruder_id = extruder_id;
            ni.diameter    = get_diameter_str(extruder_id);
            ni.volume_type = vol_type;
            nozzle_map[nozzle_id] = ni;
        }
    }

    // ── Write nozzles_info to PlateData ──
    // This populates Tier 2 in bbs_3mf serialization.
    plate_data->nozzles_info.clear();
    plate_data->nozzles_info.reserve(nozzle_map.size());
    for (auto& [id, ni] : nozzle_map) {
        plate_data->nozzles_info.push_back(ni);
    }

    // ── CRITICAL: Reset nozzle_group_result so Tier 1 is skipped ──
    // The bbs_3mf serializer (bbs_3mf.cpp:8395) checks nozzle_group_result
    // (Tier 1) BEFORE nozzles_info (Tier 2). The upstream pipeline
    // (ToolOrdering → GCodeProcessorResult → parse_filament_info) produces
    // a LayeredNozzleGroupResult that maps ALL carousel filaments to the same
    // group_id (typically 1), because the PAM clustering treats the entire
    // extruder-2 carousel as a single nozzle group.
    //
    // By resetting nozzle_group_result to nullopt, the serializer falls
    // through to Tier 2 where our correct per-filament nozzle assignments
    // are stored. This is safe because nozzles_info contains exactly the
    // same information that nozzle_group_result would provide, but with
    // correct carousel slot assignments.
    plate_data->nozzle_group_result.reset();

    BOOST_LOG_TRIVIAL(info) << "Vortek::patch_plate_data_for_export: patched "
                            << plate_data->slice_filaments_info.size() << " filaments, "
                            << plate_data->nozzles_info.size() << " nozzles for H2C export"
                            << " (nozzle_group_result reset to force Tier 2)";
}

} // namespace Vortek

