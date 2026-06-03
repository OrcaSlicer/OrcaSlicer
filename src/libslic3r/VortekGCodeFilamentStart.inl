// VortekGCodeFilamentStart.inl — H2C filament_start_gcode + VT comment
// Source: BambuStudio/src/libslic3r/GCode.cpp:2772-2787
//
// #included inside GCode::_do_export() after machine_start_gcode,
// inside `if (is_h2c_multi_nozzle)` block.
//
// Variables expected in scope:
//   initial_extruder_id, initial_non_support_extruder_id, file, print

{
    auto group_result = m_print->get_layered_nozzle_group_result();

    m_writer.init_extruder(initial_non_support_extruder_id);
    // add the missing filament start gcode in machine start gcode
    {
        DynamicConfig config;
        config.set_key_value("filament_extruder_id", new ConfigOptionInt((int)(initial_non_support_extruder_id)));
        config.set_key_value("current_filament_id", new ConfigOptionInt((int)(initial_non_support_extruder_id)));
        config.set_key_value("current_extruder_id", new ConfigOptionInt((int)get_extruder_id(initial_non_support_extruder_id)));
        config.set_key_value("current_nozzle_id",
            new ConfigOptionInt(group_result->get_first_nozzle_for_filament(initial_non_support_extruder_id)->group_id));
        config.set_key_value("nozzle_diameter_at_nozzle_id",
            new ConfigOptionFloats(get_nozzle_diameters_by_nozzle_id(group_result.get())));
        config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
        std::string filament_start_gcode = this->placeholder_parser_process("filament_start_gcode",
            print.config().filament_start_gcode.values.at(initial_non_support_extruder_id),
            initial_non_support_extruder_id, &config);
        file.writeln(filament_start_gcode);
        // mark the first filament used in print — BBL GCode.cpp:2785-2786
        int initial_nozzle_id = NOZZLE_ID_FOR_GCODE(group_result,
            group_result->get_first_nozzle_for_filament(initial_extruder_id)->group_id);
        file.write_format(";VT%d H%d\n", initial_extruder_id, initial_nozzle_id);
    }
}
