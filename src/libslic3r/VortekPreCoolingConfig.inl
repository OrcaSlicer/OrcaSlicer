// ============================================================================
// VortekPreCoolingConfig.inl
//
// Inline include — extracted from GCodeProcessor::apply_config().
// H2C PreCooling configuration: reads cooling/heating rates, pre-cooling
// temperatures, preheat deltas, filament types, extruder types, nozzle diameter.
//
// MUST be #include'd inside GCodeProcessor::apply_config() after filament_count
// is set and config is in scope.
//
// BBL parity: BambuStudio GCodeProcessor.cpp (commit 3f2570c)
// ============================================================================

// H2C PreCooling config
m_enable_pre_heating = config.enable_pre_heating.value;
{
    // cooling/heating rates — Nullable, defaults to 2.0 deg/s
    m_cooling_rate.resize(filament_count, 2.0);
    m_heating_rate.resize(filament_count, 2.0);
    for (size_t i = 0; i < filament_count; ++i) {
        if (i < config.hotend_cooling_rate.size() && config.hotend_cooling_rate.values[i] > 0)
            m_cooling_rate[i] = config.hotend_cooling_rate.values[i];
        if (i < config.hotend_heating_rate.size() && config.hotend_heating_rate.values[i] > 0)
            m_heating_rate[i] = config.hotend_heating_rate.values[i];
    }

    // pre-cooling temperature
    m_pre_cooling_temp.resize(filament_count, 0);
    for (size_t i = 0; i < filament_count; ++i) {
        if (i < config.filament_pre_cooling_temperature_nc.size())
            m_pre_cooling_temp[i] = config.filament_pre_cooling_temperature_nc.get_at(i);
    }

    // preheat temperature delta
    m_filament_preheat_temperature_delta.resize(filament_count, 0.0);
    for (size_t i = 0; i < filament_count; ++i) {
        if (i < config.filament_preheat_temperature_delta.size())
            m_filament_preheat_temperature_delta[i] = config.filament_preheat_temperature_delta.get_at(i);
    }

    // max temperature drop when extruder change — not in Orca PrintConfig yet, default 0
    m_filament_max_temperature_drop_when_ec.resize(filament_count, 0.0);

    // filament types
    m_filament_types.resize(filament_count);
    for (size_t i = 0; i < filament_count; ++i) {
        if (i < config.filament_type.size())
            m_filament_types[i] = config.filament_type.values[i];
    }

    // extruder types
    m_extruder_types.clear();
    for (size_t i = 0; i < config.extruder_type.size(); ++i)
        m_extruder_types.push_back(static_cast<ExtruderType>(config.extruder_type.values[i]));

    // nozzle diameter
    m_nozzle_diameter.resize(config.nozzle_diameter.size());
    for (size_t i = 0; i < config.nozzle_diameter.size(); ++i)
        m_nozzle_diameter[i] = config.nozzle_diameter.values[i];
}
