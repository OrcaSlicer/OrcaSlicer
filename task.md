Analyze the bug that it failed to load project(3mf) from old version.
It failed pass below check in PresetBundle::load_config_file_config function, hence throw error.
    if (config.option("extruder_variant_list")) {
        //3mf support multiple extruder logic
        size_t extruder_count = config.option<ConfigOptionFloats>("nozzle_diameter")->values.size();
        extruder_variant_count = config.option<ConfigOptionStrings>("filament_extruder_variant", true)->size();
        if ((extruder_variant_count != filament_self_indice.size())
            || (extruder_variant_count < num_filaments)) {
            assert(false);
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": invalid config file %1%, can not find suitable filament_extruder_variant or filament_self_index") % name_or_path;
            throw Slic3r::RuntimeError(std::string("Invalid configuration file: ") + name_or_path);
        }