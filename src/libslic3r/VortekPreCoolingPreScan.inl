// ============================================================================
// VortekPreCoolingPreScan.inl
//
// Inline include — extracted from GCodeProcessor::run_post_process().
// This block pre-scans the gcode file to build filament/extruder usage blocks,
// creates PreCoolingInjector, and generates InsertedLinesMap (M104 commands).
//
// MUST be #include'd inside GCodeProcessor::run_post_process() body,
// after the lambdas but before the main line-by-line processing loop.
//
// Inputs (from enclosing scope):
//   - in.f                        : FILE* gcode input
//   - m_enable_pre_heating        : bool
//   - m_nozzle_group_result       : std::optional<NozzleGroupResult>
//   - m_filament_nozzle_temp      : vector<double>
//   - m_filament_nozzle_temp_first_layer : vector<double>
//   - m_result.moves              : vector<MoveVertex>
//   - m_time_processor.machines[] : time machine array
//   - m_physical_extruder_map     : vector<int>
//   - m_inject_time_threshold     : float
//   - m_has_filament_switcher     : bool
//   - m_pre_cooling_temp          : vector<int>
//   - m_cooling_rate / m_heating_rate : vector<float>
//   - m_extruder_max_nozzle_count : vector<int>
//   - m_filament_preheat_temperature_delta : vector<double>
//   - m_filament_max_temperature_drop_when_ec : vector<double>
//   - m_filament_types            : vector<string>
//   - m_extruder_types            : vector<ExtruderType>
//   - m_nozzle_diameter           : vector<float>
//
// Output (set in enclosing scope):
//   - precooling_inserted_lines   : TimeProcessor::InsertedLinesMap
//
// BBL parity: BambuStudio GCodeProcessor.cpp:948-1191 (commit 3f2570c)
// ============================================================================

// H2C PreCooling: Pre-scan phase
// Scan the gcode file to build filament/extruder usage blocks, then create
// PreCoolingInjector which generates InsertedLinesMap (M104 commands to inject).
{
    using FilamentUsageBlock = ExtruderPreHeating::FilamentUsageBlock;
    using ExtruderUsageBlcok = ExtruderPreHeating::ExtruderUsageBlcok;

    std::vector<FilamentUsageBlock> filament_blocks;
    std::vector<ExtruderUsageBlcok> extruder_blocks = { ExtruderUsageBlcok() }; // dummy first block
    ExtruderUsageBlcok temp_construct_block;

    unsigned int machine_start_gcode_end_line_id = 0;
    unsigned int machine_end_gcode_start_line_id = std::numeric_limits<unsigned int>::max();
    unsigned int pre_scan_layer_id = 0;

    // Lambda: parse OF/NF/ON/NN from NozzleChange tag line
    auto handle_nozzle_change_line = [this](const std::string& line, int& old_filament, int& next_filament,
                                            int& extruder_id, int gcode_id, int& old_nozzle_id, int& new_nozzle_id) -> bool {
        std::regex re(R"(OF(\d+)\s+NF(\d+)\s+ON(\d+)\s+NN(\d+))");
        std::smatch match;
        if (!std::regex_search(line, match, re))
            return false;
        old_filament = std::stoi(match[1]);
        next_filament = std::stoi(match[2]);
        old_nozzle_id = std::stoi(match[3]);
        new_nozzle_id = std::stoi(match[4]);
        auto nozzle_info = m_nozzle_group_result->get_nozzle_from_id(new_nozzle_id);
        extruder_id = nozzle_info ? nozzle_info->extruder_id : -1;
        return true;
    };

    // Lambda: handle filament change (T command)
    auto handle_filament_change = [&](int filament_id, int line_id, int nozzle_id = -1) {
        if (!filament_blocks.empty())
            filament_blocks.back().upper_gcode_id = line_id;

        if (nozzle_id == -1)
            nozzle_id = m_nozzle_group_result->get_nozzle_id(filament_id, pre_scan_layer_id);

        int extruder_id = 0;
        auto nozzle_info = m_nozzle_group_result->get_nozzle_from_id(nozzle_id);
        if (nozzle_info) extruder_id = nozzle_info->extruder_id;

        filament_blocks.emplace_back(filament_id, extruder_id, nozzle_id, line_id, -1);
    };

    // Pre-scan: read entire gcode to collect block data
    std::fseek(in.f, 0, SEEK_SET);
    std::string scan_line;
    std::vector<char> scan_buffer(65536, 0);
    unsigned int scan_line_id = 0;

    for (;;) {
        size_t cnt_read = ::fread(scan_buffer.data(), 1, scan_buffer.size(), in.f);
        if (::ferror(in.f)) break;
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

                // Track layer changes
                if (scan_line.size() > 1 && scan_line.front() == ';') {
                    std::string_view sv(scan_line);
                    while (!sv.empty() && (sv.back() == '\n' || sv.back() == '\r')) sv.remove_suffix(1);
                    sv.remove_prefix(1);
                    if (sv == reserved_tag(ETags::Layer_Change))
                        ++pre_scan_layer_id;
                }

                // Handle T commands
                if (GCodeReader::GCodeLine::cmd_starts_with(scan_line, "T")) {
                    int fid = -1;
                    std::string cmd = GCodeReader::GCodeLine::extract_cmd(scan_line);
                    if (cmd.size() >= 2) {
                        std::istringstream str(cmd.substr(1));
                        str >> fid;
                        if (!str.fail() && fid >= 0 && fid < 255) {
                            int nozzle_id = -1;
                            // Parse H parameter from T command line
                            size_t h_pos = scan_line.find(" H");
                            if (h_pos != std::string::npos) {
                                std::istringstream hstr(scan_line.substr(h_pos + 2));
                                hstr >> nozzle_id;
                            }
                            handle_filament_change(fid, scan_line_id, nozzle_id);
                        }
                    }
                }
                // Handle NozzleChangeStart
                else if (GCodeReader::GCodeLine::cmd_starts_with(scan_line, (std::string(";") + reserved_tag(ETags::NozzleChangeStart)).c_str())) {
                    int prev_filament{-1}, next_filament{-1}, ext_id{-1}, prev_nozzle{-1}, next_nozzle{-1};
                    handle_nozzle_change_line(scan_line, prev_filament, next_filament, ext_id, scan_line_id, prev_nozzle, next_nozzle);
                    if (!extruder_blocks.empty())
                        extruder_blocks.back().initialize_step_2(scan_line_id);
                }
                // Handle NozzleChangeEnd
                else if (GCodeReader::GCodeLine::cmd_starts_with(scan_line, (std::string(";") + reserved_tag(ETags::NozzleChangeEnd)).c_str())) {
                    int prev_filament{-1}, next_filament{-1}, ext_id{-1}, prev_nozzle{-1}, next_nozzle{-1};
                    handle_nozzle_change_line(scan_line, prev_filament, next_filament, ext_id, scan_line_id, prev_nozzle, next_nozzle);
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

    // Finalize blocks
    if (!filament_blocks.empty())
        filament_blocks.back().upper_gcode_id = machine_end_gcode_start_line_id;

    if (!extruder_blocks.empty()) {
        int first_filament = 0, last_filament = 0;
        if (!filament_blocks.empty()) {
            first_filament = filament_blocks.front().filament_id;
            last_filament = filament_blocks.back().filament_id;
        }
        auto nozzle_info = m_nozzle_group_result->get_first_nozzle_for_filament(first_filament);
        int ext_id = nozzle_info ? nozzle_info->extruder_id : -1;
        int start_nozzle = nozzle_info ? nozzle_info->group_id : -1;
        extruder_blocks.front().initialize_step_1(ext_id, machine_start_gcode_end_line_id, first_filament, start_nozzle);
        extruder_blocks.back().initialize_step_2(machine_end_gcode_start_line_id);
        int last_nozzle = filament_blocks.empty() ? -1 : filament_blocks.back().nozzle_id;
        extruder_blocks.back().initialize_step_3(machine_end_gcode_start_line_id, last_filament, machine_end_gcode_start_line_id, last_nozzle);
    }

    // Create PreCoolingInjector and generate M104 injection map
    size_t valid_machine_id = 0;
    for (size_t i = 0; i < static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Count); ++i) {
        if (m_time_processor.machines[i].enabled) {
            valid_machine_id = i;
            break;
        }
    }

    std::vector<int> filament_nozzle_temps_int(m_filament_nozzle_temp.begin(), m_filament_nozzle_temp.end());
    std::vector<int> filament_nozzle_temps_fl_int(m_filament_nozzle_temp_first_layer.begin(), m_filament_nozzle_temp_first_layer.end());
    std::vector<std::pair<unsigned int, unsigned int>> skippable_blocks; // empty for now

    auto pre_cooling_injector = std::make_unique<PreCoolingInjector>(
        m_result.moves,
        m_filament_types,
        *m_nozzle_group_result,
        filament_nozzle_temps_int,
        filament_nozzle_temps_fl_int,
        m_physical_extruder_map,
        valid_machine_id,
        m_inject_time_threshold,
        false, // handle_hotend_as_extruder
        m_has_filament_switcher,
        m_pre_cooling_temp,
        m_cooling_rate,
        m_heating_rate,
        skippable_blocks,
        m_extruder_max_nozzle_count,
        m_filament_preheat_temperature_delta,
        m_filament_max_temperature_drop_when_ec,
        machine_start_gcode_end_line_id,
        machine_end_gcode_start_line_id,
        m_extruder_types,
        m_nozzle_diameter
    );

    pre_cooling_injector->build_extruder_free_blocks(filament_blocks, extruder_blocks);
    pre_cooling_injector->process_pre_cooling_and_heating(precooling_inserted_lines);

    BOOST_LOG_TRIVIAL(info) << "PreCoolingInjector: generated " << precooling_inserted_lines.size() << " injection points";

    // Reset file position for main processing pass
    std::fseek(in.f, 0, SEEK_SET);
}
