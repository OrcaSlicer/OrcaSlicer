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
#include <algorithm>

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

    const auto& ori_config = gcode.m_print->ori_full_print_config();
    const bool has_self_index = (ori_config.option("filament_self_index") != nullptr);

    int idx = ori_config.get_index_for_extruder(
        filament_id + 1,
        "filament_self_index",
        extruder_type,
        nozzle_volume_type,
        "filament_extruder_variant"
    );

    if (idx >= 0 && has_self_index) {
        // filament_self_index present → idx is the real absolute index in ori_full_print_config.
        return idx;
    }

    // filament_self_index absent: get_index_for_extruder found the first globally matching
    // variant entry (e.g. 0 for Standard, 1 for HighFlow). This is a LOCAL offset within
    // one filament's variant block, not an absolute index.
    //
    // ori_full_print_config concatenates each filament's variants in slot order:
    //   [F0_std, F0_hf, F1_std, F1_hf, ..., Fn_std, Fn_hf]
    // Compute absolute index as: filament_id * variants_per_filament + local_offset.
    int local_offset = (idx >= 0) ? idx : 0;
    const int num_filaments = static_cast<int>(gcode.m_config.filament_type.values.size());
    auto variant_opt = dynamic_cast<const Slic3r::ConfigOptionStrings*>(
        ori_config.option("filament_extruder_variant"));
    if (variant_opt && num_filaments > 0) {
        const int total_variants = static_cast<int>(variant_opt->values.size());
        const int variants_per_filament = total_variants / num_filaments;
        if (variants_per_filament > 0 && local_offset < variants_per_filament)
            return filament_id * variants_per_filament + local_offset;
    }

    return filament_id;
}

size_t GCode::get_logical_filament_count(const Slic3r::GCode& gcode)
{
    // BBL compresses per-filament arrays to N_physical_extruders after slicing.
    // ori_full_print_config retains the original N_logical_slots * N_variants layout.
    // filament_self_index[i] == slot number (1-based), so max(filament_self_index) == N_slots.
    size_t n = gcode.m_config.filament_type.values.size();
    if (gcode.m_print) {
        const auto& ori_config = gcode.m_print->ori_full_print_config();
        auto* self_idx = dynamic_cast<const Slic3r::ConfigOptionVector<int>*>(
            ori_config.option("filament_self_index"));
        if (self_idx && !self_idx->values.empty()) {
            int max_slot = *std::max_element(self_idx->values.begin(), self_idx->values.end());
            if (max_slot > 0)
                n = static_cast<size_t>(max_slot);
        }
    }
    return (n > 0) ? n : 1;
}

float GCode::get_filament_retract_length_nc(const Slic3r::GCode& gcode, int filament_id)
{
    size_t nf = get_logical_filament_count(gcode);
    auto rv   = remap_floats_by_filament_vortek(gcode, "filament_retract_length_nc", nf);

    double val = 0.0;
    if (filament_id >= 0 && filament_id < (int)rv.size())
        val = rv[filament_id];

    // Fallback for third-party filament presets that don't define the H2C-specific
    // filament_retract_length_nc field (stored as 0 / nullable-nil):
    // use retraction_distances_when_ec for the physical extruder, scaled to 0.1 mm units.
    if (val == 0.0) {
        int extruder_id = 0;
        auto group_result = gcode.m_print ? gcode.m_print->get_layered_nozzle_group_result() : nullptr;
        if (group_result && filament_id >= 0) {
            auto nozzle_info = group_result->get_first_nozzle_for_filament(filament_id);
            if (nozzle_info)
                extruder_id = nozzle_info->extruder_id;
        }
        val = gcode.m_config.retraction_distances_when_ec.get_at(extruder_id) * 10.0;
    }

    return (float)val;
}

void GCode::apply_tcr_flush_config(const Slic3r::GCode& gcode, bool skip_cooling,
                                   Slic3r::DynamicConfig& config)
{
    size_t nf = get_logical_filament_count(gcode);
    auto flush_v_speed        = remap_floats_by_filament_vortek(gcode, "filament_flush_volumetric_speed", nf);
    auto filament_max_v       = remap_floats_by_filament_vortek(gcode, "filament_max_volumetric_speed",   nf);
    auto flush_temps          = remap_ints_by_filament_vortek(gcode,   "filament_flush_temp",             nf);
    auto cooling_before_tower = remap_floats_by_filament_vortek(gcode, "filament_cooling_before_tower",   nf);
    auto range_highs          = remap_nozzle_ints_by_filament_vortek(gcode,   "nozzle_temperature_range_high",   nf);

    for (size_t idx = 0; idx < nf; ++idx) {
        if (flush_v_speed[idx] == 0)
            flush_v_speed[idx] = filament_max_v[idx];
        if (flush_temps[idx] == 0)
            flush_temps[idx] = range_highs[idx];
    }
    if (skip_cooling)
        std::fill(cooling_before_tower.begin(), cooling_before_tower.end(), 0.0);

    config.set_key_value("flush_volumetric_speeds",
        new Slic3r::ConfigOptionFloats(flush_v_speed));
    config.set_key_value("flush_temperatures",
        new Slic3r::ConfigOptionInts(flush_temps));
    config.set_key_value("filament_cooling_before_tower",
        new Slic3r::ConfigOptionFloats(cooling_before_tower));
}

std::vector<double> GCode::remap_floats_by_filament_vortek(const Slic3r::GCode& gcode, const std::string& key, size_t num_filaments)
{
    std::vector<double> dst(num_filaments, 0.0);
    // Primary source: ori_full_print_config holds pre-expansion per-variant data.
    // get_original_filament_index() resolves the correct variant-aware index for each filament.
    auto opt = dynamic_cast<const Slic3r::ConfigOptionVector<double>*>(
        gcode.m_print->ori_full_print_config().option(key));
    for (size_t i = 0; i < num_filaments; ++i) {
        int idx = get_original_filament_index(gcode, (int)i);
        if (opt) {
            // For printer/nozzle options that are not expanded per-variant (size == num_filaments),
            // fallback to the logical slot index `i` if the resolved variant index is out of bounds.
            int final_idx = (idx >= 0 && idx < (int)opt->values.size()) ? idx : (int)i;
            if (final_idx >= 0 && final_idx < (int)opt->values.size()) {
                dst[i] = opt->get_at(final_idx);
            }
        }
        // No m_config fallback: m_config may not have expanded ConfigOptionFloatsNullable
        // options correctly (BBL core skips nullable types in the expansion loop).
    }
    return dst;
}

std::vector<int> GCode::remap_ints_by_filament_vortek(const Slic3r::GCode& gcode, const std::string& key, size_t num_filaments)
{
    std::vector<int> dst(num_filaments, 0);
    // Primary source: ori_full_print_config holds pre-expansion per-variant data.
    // get_original_filament_index() resolves the correct variant-aware index for each filament.
    auto opt = dynamic_cast<const Slic3r::ConfigOptionVector<int>*>(
        gcode.m_print->ori_full_print_config().option(key));
    for (size_t i = 0; i < num_filaments; ++i) {
        int idx = get_original_filament_index(gcode, (int)i);
        if (opt) {
            // For printer/nozzle options that are not expanded per-variant (size == num_filaments),
            // fallback to the logical slot index `i` if the resolved variant index is out of bounds.
            int final_idx = (idx >= 0 && idx < (int)opt->values.size()) ? idx : (int)i;
            if (final_idx >= 0 && final_idx < (int)opt->values.size()) {
                int val = opt->get_at(final_idx);
                // ConfigOptionIntsNullable uses INT_MAX as the nil sentinel — treat nil as 0.
                if (val != std::numeric_limits<int>::max())
                    dst[i] = val;
            }
        }
        // No m_config fallback: m_config expansion is skipped for ConfigOptionIntsNullable
        // by BBL core (dynamic_cast<ConfigOptionInts*> fails for nullable template variant).
    }
    return dst;
}

std::vector<int> GCode::remap_nozzle_ints_by_filament_vortek(const Slic3r::GCode& gcode, const std::string& key, size_t num_filaments)
{
    std::vector<int> dst(num_filaments, 0);
    auto opt = dynamic_cast<const Slic3r::ConfigOptionVector<int>*>(
        gcode.m_config.option(key));
    if (opt) {
        auto group_result = gcode.m_print ? gcode.m_print->get_layered_nozzle_group_result() : nullptr;
        for (size_t i = 0; i < num_filaments; ++i) {
            int extruder_id = 0;
            if (group_result) {
                auto nozzle_info = group_result->get_first_nozzle_for_filament((int)i);
                if (nozzle_info)
                    extruder_id = nozzle_info->extruder_id;
            }
            dst[i] = opt->get_at(extruder_id);
        }
    }
    return dst;
}

void GCode::update_placeholder_parser_with_variant_params(Slic3r::GCode& gcode)
{
    // Derive logical filament slot count (NOT the compressed physical-extruder count)
    // via get_logical_filament_count(), which reads filament_self_index from
    // ori_full_print_config — the config snapshot taken before per-extruder compression.
    size_t num_filaments = get_logical_filament_count(gcode);
    if (num_filaments == 0)
        return;

    gcode.placeholder_parser().set("filament_max_volumetric_speed",       new Slic3r::ConfigOptionFloats(remap_floats_by_filament_vortek(gcode, "filament_max_volumetric_speed", num_filaments)));
    gcode.placeholder_parser().set("filament_pre_cooling_temperature",    new Slic3r::ConfigOptionInts(remap_ints_by_filament_vortek(gcode, "filament_pre_cooling_temperature", num_filaments)));

    // filament_pre_cooling_temperature_nc: pass through as-is from preset.
    // BBL empirically outputs P0 when the preset value is 0 (e.g. PA filament) —
    // firmware handles nil values internally. No synthetic fallback.
    gcode.placeholder_parser().set("filament_pre_cooling_temperature_nc",
        new Slic3r::ConfigOptionInts(remap_ints_by_filament_vortek(gcode, "filament_pre_cooling_temperature_nc", num_filaments)));

    gcode.placeholder_parser().set("filament_ramming_travel_time",       new Slic3r::ConfigOptionFloats(remap_floats_by_filament_vortek(gcode, "filament_ramming_travel_time", num_filaments)));
    gcode.placeholder_parser().set("filament_ramming_travel_time_nc",    new Slic3r::ConfigOptionFloats(remap_floats_by_filament_vortek(gcode, "filament_ramming_travel_time_nc", num_filaments)));
    gcode.placeholder_parser().set("filament_cooling_before_tower",       new Slic3r::ConfigOptionFloats(remap_floats_by_filament_vortek(gcode, "filament_cooling_before_tower", num_filaments)));
    gcode.placeholder_parser().set("nozzle_temperature_initial_layer",    new Slic3r::ConfigOptionInts(remap_ints_by_filament_vortek(gcode, "nozzle_temperature_initial_layer", num_filaments)));
    gcode.placeholder_parser().set("nozzle_temperature",                  new Slic3r::ConfigOptionInts(remap_ints_by_filament_vortek(gcode, "nozzle_temperature", num_filaments)));
    gcode.placeholder_parser().set("first_layer_temperature",             new Slic3r::ConfigOptionInts(remap_ints_by_filament_vortek(gcode, "nozzle_temperature_initial_layer", num_filaments)));

    // retraction_distances_when_cut is a printer-extruder-variant option (indexed by
    // physical extruder + nozzle variant), NOT a per-filament-slot option.
    // ori_full_print_config has 4 entries (2 nozzles × 2 variants) for H2C, but
    // get_original_filament_index() returns filament-variant indices (up to 2*N_slots-1)
    // which are out-of-bounds → silently zero. Fix: map via physical extruder id.
    {
        auto group_result_r = gcode.m_print ? gcode.m_print->get_layered_nozzle_group_result() : nullptr;
        std::vector<double> retract_when_cut(num_filaments, 0.0);
        for (size_t i = 0; i < num_filaments; ++i) {
            int extruder_id = 0;
            if (group_result_r) {
                auto nozzle_info = group_result_r->get_first_nozzle_for_filament((int)i);
                if (nozzle_info)
                    extruder_id = nozzle_info->extruder_id;
            }
            retract_when_cut[i] = gcode.m_config.retraction_distances_when_cut.get_at(extruder_id);
        }
        gcode.placeholder_parser().set("retraction_distances_when_cut",
            new Slic3r::ConfigOptionFloats(retract_when_cut));
    }
    gcode.placeholder_parser().set("filament_map", new Slic3r::ConfigOptionInts(gcode.m_config.filament_map));

    auto range_highs = remap_nozzle_ints_by_filament_vortek(gcode, "nozzle_temperature_range_high", num_filaments);
    gcode.placeholder_parser().set("nozzle_temperature_range_high",    new Slic3r::ConfigOptionInts(range_highs));
    gcode.placeholder_parser().set("nozzle_temperature_range_low",     new Slic3r::ConfigOptionInts(remap_nozzle_ints_by_filament_vortek(gcode, "nozzle_temperature_range_low", num_filaments)));

    {
        auto flush_v_speed  = remap_floats_by_filament_vortek(gcode, "filament_flush_volumetric_speed", num_filaments);
        auto filament_max_v = remap_floats_by_filament_vortek(gcode, "filament_max_volumetric_speed", num_filaments);
        auto flush_temps    = remap_ints_by_filament_vortek(gcode, "filament_flush_temp", num_filaments);
        for (size_t i = 0; i < num_filaments; ++i) {
            if (flush_v_speed[i] == 0)
                flush_v_speed[i] = filament_max_v[i];
            // BBL formula: flush_temperatures = max(filament_flush_temp, nozzle_temperature_range_high).
            // Remap nozzle_temperature_range_high using variant-aware logic to bypass the
            // compressed m_config (which has size 2 for H2C).
            int range_high = range_highs[i];
            if (range_high > 0)
                flush_temps[i] = std::max(flush_temps[i], range_high);
        }
        gcode.placeholder_parser().set("flush_volumetric_speeds", new Slic3r::ConfigOptionFloats(flush_v_speed));
        gcode.placeholder_parser().set("flush_temperatures",      new Slic3r::ConfigOptionInts(flush_temps));
    }
}

void GCode::patch_toolchange_dyn_config(
    Slic3r::DynamicConfig&  dyn_config,
    Slic3r::GCode&          gcode,
    int                     old_filament_id,
    int                     new_filament_id,
    float                   filament_area
)
{
    // Guard: only runs for H2C multi-nozzle printers.
    // Single-extruder or non-H2C printers use standard Orca logic unchanged.
    if (!gcode.m_print)
        return;

    const size_t nf = get_logical_filament_count(gcode);
    if (nf == 0)
        return;

    // --- Temperatures ---------------------------------------------------------
    // m_config.nozzle_temperature is compressed (2 entries for H2C).
    // Re-read from ori_full_print_config via variant-aware remap so all 7 logical
    // slots get their correct temperature.
    const bool on_first_layer = gcode.on_first_layer();
    auto nozzle_temps_il = remap_ints_by_filament_vortek(gcode, "nozzle_temperature_initial_layer", nf);
    auto nozzle_temps    = remap_ints_by_filament_vortek(gcode, "nozzle_temperature", nf);

    // old_filament_temp
    if (old_filament_id >= 0 && old_filament_id < (int)nf) {
        int old_temp = on_first_layer
            ? nozzle_temps_il[old_filament_id]
            : nozzle_temps[old_filament_id];
        if (old_temp > 0)
            dyn_config.set_key_value("old_filament_temp", new Slic3r::ConfigOptionInt(old_temp));
    }

    // new_filament_temp
    if (new_filament_id >= 0 && new_filament_id < (int)nf) {
        int new_temp = on_first_layer
            ? nozzle_temps_il[new_filament_id]
            : nozzle_temps[new_filament_id];
        if (new_temp > 0) {
            // Only override if the value currently in dyn_config came from the
            // compressed m_config.  If toolchange_temp_override was active, the
            // value is different from m_config.get_at() and we leave it alone.
            int compressed_val = on_first_layer
                ? gcode.m_config.nozzle_temperature_initial_layer.get_at(new_filament_id)
                : gcode.m_config.nozzle_temperature.get_at(new_filament_id);
            const auto* existing = dynamic_cast<const Slic3r::ConfigOptionInt*>(
                dyn_config.option("new_filament_temp"));
            if (!existing || existing->value == compressed_val)
                dyn_config.set_key_value("new_filament_temp", new Slic3r::ConfigOptionInt(new_temp));
        }
    }

    // nozzle_temperature_range_high[previous_extruder / next_extruder] is accessed
    // directly via array index in the G-code template ({nozzle_temperature_range_high[i]})
    // so the full array already set by update_placeholder_parser_with_variant_params
    // is correct.  No patching needed here.

    // --- Filament feedrate (volumetric speed → mm/min) ----------------------
    // m_config.filament_max_volumetric_speed is compressed; re-read full array.
    if (filament_area > 0.f) {
        auto max_v = remap_floats_by_filament_vortek(gcode, "filament_max_volumetric_speed", nf);

        // old_filament_e_feedrate
        if (old_filament_id >= 0 && old_filament_id < (int)nf) {
            int feedrate = (int)(60.0 * max_v[old_filament_id] / filament_area);
            feedrate = feedrate == 0 ? 100 : feedrate;
            dyn_config.set_key_value("old_filament_e_feedrate", new Slic3r::ConfigOptionInt(feedrate));
        }

        // new_filament_e_feedrate
        if (new_filament_id >= 0 && new_filament_id < (int)nf) {
            int feedrate = (int)(60.0 * max_v[new_filament_id] / filament_area);
            feedrate = feedrate == 0 ? 100 : feedrate;
            dyn_config.set_key_value("new_filament_e_feedrate", new Slic3r::ConfigOptionInt(feedrate));
        }
    }

    // --- filament_retract_length_nc -----------------------------------------
    // This scalar is the outgoing filament's NC retraction length.
    // m_config.filament_retract_length_nc.get_at() clamps on compressed array.
    if (old_filament_id >= 0) {
        float nc_len = get_filament_retract_length_nc(gcode, old_filament_id);
        if (nc_len > 0.f)
            dyn_config.set_key_value("filament_retract_length_nc",
                new Slic3r::ConfigOptionFloat(nc_len));
    }

    // --- filament_tower_interface_print_temp ---------------------------------
    // This temperature fallback should also use the correct slot-specific
    // nozzle_temperature_range_high instead of clamping on compressed m_config.
    if (new_filament_id >= 0 && new_filament_id < (int)nf) {
        int interface_temp = gcode.m_config.filament_tower_interface_print_temp.get_at(new_filament_id);
        if (interface_temp == -1) {
            auto range_highs = remap_nozzle_ints_by_filament_vortek(gcode, "nozzle_temperature_range_high", nf);
            interface_temp = range_highs[new_filament_id];
        }
        if (interface_temp > 0) {
            dyn_config.set_key_value("filament_tower_interface_print_temp", new Slic3r::ConfigOptionInt(interface_temp));
            gcode.placeholder_parser().set("filament_tower_interface_print_temp", new Slic3r::ConfigOptionInt(interface_temp));
        }
    }

    // --- flush_temperatures --------------------------------------------------
    // GCode::set_extruder() writes a compressed nozzle-count-sized (2-element)
    // flush_temperatures array into dyn_config from m_config.filament_flush_temp.
    // The template indexes it as flush_temperatures[current_extruder] where
    // current_extruder is the logical filament slot (0-6), so get_at() clamps
    // to the last element → always returns the slot-1 value (e.g. 270 for ASA).
    //
    // BBL computes flush_temperatures as max(filament_flush_temp, nozzle_temperature_range_high):
    //   ASA: flush_temp=270, range_high=280 → 280.
    //
    // Fix: overwrite dyn_config["flush_temperatures"] with the correctly remapped
    // nf-element array using the same max() logic.
    {
        auto flush_temps = remap_ints_by_filament_vortek(gcode, "filament_flush_temp", nf);
        auto range_highs = remap_nozzle_ints_by_filament_vortek(gcode, "nozzle_temperature_range_high", nf);

        for (size_t i = 0; i < nf; ++i) {
            // BBL formula: flush_temperatures = max(filament_flush_temp, nozzle_temperature_range_high).
            // Remap nozzle_temperature_range_high using variant-aware logic to bypass the
            // compressed m_config (which has size 2 for H2C).
            int range_high = range_highs[i];
            if (range_high > 0)
                flush_temps[i] = std::max(flush_temps[i], range_high);
        }
        // Overwrite dyn_config (covers [variable] bracket substitutions)
        // AND placeholder_parser (covers {expression} curly-brace evaluator).
        // Both are needed: GCode::set_extruder() writes the wrong 2-element
        // compressed array via L8596, and the {expr} evaluator reads from
        // placeholder_parser which is set per-layer from m_config (same 2-elem).
        dyn_config.set_key_value("flush_temperatures",
            new Slic3r::ConfigOptionInts(flush_temps));
        gcode.placeholder_parser().set("flush_temperatures",
            new Slic3r::ConfigOptionInts(flush_temps));
    }
}

} // namespace Vortek
