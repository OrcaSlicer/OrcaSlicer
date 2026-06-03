// VortekGCodeInit.inl — H2C G-code init, ported 1:1 from BBL Studio
// Source: BambuStudio/src/libslic3r/GCode.cpp:2460-2530, 2772-2787
//
// This file is #included inside GCode::_do_export() inside the
// `if (is_h2c_multi_nozzle)` block. It replaces the Orca-specific
// placeholder parser setup and VT comment generation for H2C printers.
//
// Variables expected to be in scope (from _do_export):
//   initial_extruder_id, initial_non_support_extruder_id,
//   first_filaments, first_non_support_filaments,
//   has_wipe_tower, tool_ordering, max_additional_fan

// ─── BBL: match filament to physical extruder ───
{
    auto match_physical_extruder_for_each_filament = [](std::vector<int> &filaments, const FullPrintConfig &config) {
        std::vector<int> physicial_first_filaments;
        physicial_first_filaments.resize(filaments.size());
        for (size_t extruder_id = 0; extruder_id < filaments.size(); extruder_id++) {
            physicial_first_filaments[config.physical_extruder_map.get_at(extruder_id)] = filaments[extruder_id];
        }
        filaments = physicial_first_filaments;
    };
    match_physical_extruder_for_each_filament(first_filaments, m_config);
    this->placeholder_parser().set("first_tools", new ConfigOptionInts(first_filaments));
    this->placeholder_parser().set("first_filaments", new ConfigOptionInts(first_filaments));
    this->placeholder_parser().set("initial_tool", initial_extruder_id);
    this->placeholder_parser().set("initial_extruder", initial_extruder_id);
    match_physical_extruder_for_each_filament(first_non_support_filaments, m_config);

    // ─── BBL: group_result-based hotend mapping ───
    auto group_result = m_print->get_layered_nozzle_group_result();

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

    this->placeholder_parser().set("first_non_support_tools", new ConfigOptionInts(first_non_support_filaments));
    this->placeholder_parser().set("first_non_support_filaments", new ConfigOptionInts(first_non_support_filaments));
    this->placeholder_parser().set("first_non_support_hotend", new ConfigOptionInts(first_non_support_hotends));
    this->placeholder_parser().set("initial_no_support_tool", initial_non_support_extruder_id);
    this->placeholder_parser().set("initial_no_support_extruder", initial_non_support_extruder_id);
    this->placeholder_parser().set("initial_no_support_hotend",
        NOZZLE_ID_FOR_GCODE(group_result,
            group_result->get_first_nozzle_for_filament(initial_non_support_extruder_id)->group_id));
    this->placeholder_parser().set("current_extruder", initial_extruder_id);
    this->placeholder_parser().set("current_hotend",
        NOZZLE_ID_FOR_GCODE(group_result,
            group_result->get_first_nozzle_for_filament(initial_extruder_id)->group_id));
    this->placeholder_parser().set("initial_filament_id", (int)initial_extruder_id);
    this->placeholder_parser().set("initial_extruder_id", (int)get_extruder_id(initial_extruder_id));
    this->placeholder_parser().set("initial_nozzle_id",
        group_result->get_first_nozzle_for_filament(initial_extruder_id)->group_id);
    this->placeholder_parser().set("initial_no_support_filament_id", (int)initial_non_support_extruder_id);
    this->placeholder_parser().set("initial_no_support_extruder_id", (int)get_extruder_id(initial_non_support_extruder_id));
    this->placeholder_parser().set("initial_no_support_nozzle_id",
        group_result->get_first_nozzle_for_filament(initial_non_support_extruder_id)->group_id);
    this->placeholder_parser().set("nozzle_diameter_at_nozzle_id",
        new ConfigOptionFloats(get_nozzle_diameters_by_nozzle_id(group_result.get())));

    // ─── BBL: scalar compatibility keys ───
    // BBL uses get_filament_config_index() — Orca doesn't have it, use initial_extruder_id directly
    this->placeholder_parser().set("retraction_distance_when_cut", m_config.retraction_distances_when_cut.get_at(initial_extruder_id));
    this->placeholder_parser().set("long_retraction_when_cut", m_config.long_retractions_when_cut.get_at(initial_extruder_id));
    this->placeholder_parser().set("retraction_distance_when_ec", m_config.retraction_distances_when_ec.get_at(initial_extruder_id));
    this->placeholder_parser().set("long_retraction_when_ec", m_config.long_retractions_when_ec.get_at(initial_extruder_id));
}
