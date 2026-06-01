// ============================================================================
// VortekWipeTowerInit.inl
//
// Inline include — extracted from WipeTower constructor and set_extruder().
// Contains H2C-specific initialization patches:
//   1. filament change-length tables (m_filaments_change_length)
//   2. max wipe tower speed override (m_max_speed)
//   3. multi-nozzle detection (m_is_multiple_nozzle)
//   4. nozzle-change perimeter width lookup table
//
// MUST be #include'd from WipeTower.cpp where config, nozzle_diameter,
// m_perimeter_width etc. are in scope.
//
// BBL parity: BambuStudio WipeTower.cpp (commit 3f2570c)
// ============================================================================

// --- Section 1: called from WipeTower constructor, right after initializer list ---
// Macro-switched: the caller defines H2C_WIPE_TOWER_INIT_SECTION to select.

#if defined(H2C_WIPE_TOWER_INIT_CTOR)

// H2C: populate the per-filament change-length tables consumed by plan_toolchange.
// Mirrors BBL WipeTower.cpp:1764-1765. .first is the extruder-change length,
// .second is the nozzle-change length. Without this, m_filaments_change_length.first
// is empty and the H2C dual-nozzle plan_toolchange path indexes OOB → SIGSEGV.
m_filaments_change_length.first  = config.filament_change_length.values;
m_filaments_change_length.second = config.filament_change_length_nc.values;

// H2C: use configurable max wipe tower speed instead of hardcoded 5400 mm/min.
// Mirrors BBL: m_max_speed = config.prime_tower_max_speed * 60.f
// Orca already has wipe_tower_max_purge_speed (default 90 mm/s) in PrintConfig.
m_max_speed = float(config.wipe_tower_max_purge_speed) * 60.f;
if (m_max_speed <= 0.f)
    m_max_speed = 5400.f; // fallback to 90 mm/s

// H2C FIX: Detect whether this printer has multi-nozzle extruders (e.g. H2C = 2 nozzles
// per extruder). This flag gates several downstream decisions:
//   - plan_toolchange() uses it to distinguish nozzle-change vs. extruder-change flush volumes
//   - tool_change() uses it for TPU pre-extrusion travel paths
//   - finish_layer_new() uses it for gap-wall calculations
//
// Ref: BambuStudio WipeTower.cpp:1834 (commit 3f2570c)
m_is_multiple_nozzle = std::any_of(config.extruder_max_nozzle_count.values.begin(), config.extruder_max_nozzle_count.values.end(), [](auto &elem) { return elem > 1; });

// H2C: Initialize the physical_extruder_map so that format_line_M104 in
// ramming() can emit correct "T<physical_id>" parameters.
// Without this the vector is empty and any [index] → SIGSEGV.
// Ref: BambuStudio WipeTower constructor (commit 3f2570c)
{
    auto* opt = config.option<ConfigOptionInts>("physical_extruder_map");
    if (opt && !opt->values.empty())
        m_physical_extruder_map = opt->values;
    else {
        // Fallback: identity map [0, 1, ..., nozzle_count-1]
        m_physical_extruder_map.resize(config.nozzle_diameter.size());
        std::iota(m_physical_extruder_map.begin(), m_physical_extruder_map.end(), 0);
    }
}

// H2C: Initialize m_hotend_heating_rate — used by toolchange_wipe_new()
// to compute heat_time for speed_factor. Without this → OOB on empty vector.
// Ref: BambuStudio WipeTower constructor (commit 3f2570c)
{
    size_t n = config.nozzle_diameter.size();
    m_hotend_heating_rate.resize(n, 2.0); // default 2 °C/s
    for (size_t i = 0; i < n; ++i) {
        if (i < config.hotend_heating_rate.size() && !config.hotend_heating_rate.is_nil(i)
            && config.hotend_heating_rate.values[i] > 0)
            m_hotend_heating_rate[i] = config.hotend_heating_rate.values[i];
    }
}

#elif defined(H2C_WIPE_TOWER_INIT_SET_EXTRUDER)

// --- Section 2: called from WipeTower::set_extruder() ---
// H2C FIX: Nozzle-change perimeter width — the line width used when extruding the
// ramming / nozzle-change block inside the wipe tower. BBL uses a tuned lookup table
// keyed by physical nozzle diameter, NOT a simple 2× multiplier.
//
// BBL defines this as a file-scope static const map:
//   Ref: BambuStudio WipeTower.cpp:25 (commit 3f2570c)
//     static const std::map<float, float> nozzle_diameter_to_nozzle_change_width{
//         {0.2f, 0.5f}, {0.4f, 1.0f}, {0.6f, 1.2f}, {0.8f, 1.4f}
//     };
//
// We use .find() + fallback instead of .at() to avoid exceptions on unsupported diameters.
{
    static const std::map<float, float> nozzle_diameter_to_nozzle_change_width{
        {0.2f, 0.5f}, {0.4f, 1.0f}, {0.6f, 1.2f}, {0.8f, 1.4f}
    };
    auto it = nozzle_diameter_to_nozzle_change_width.find(nozzle_diameter);
    m_nozzle_change_perimeter_width = (it != nozzle_diameter_to_nozzle_change_width.end())
        ? it->second : 2 * m_perimeter_width;
}

#endif
