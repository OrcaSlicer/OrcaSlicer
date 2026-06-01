// ============================================================================
// VortekWipeTowerNozzle.inl
//
// Inline include — extracted from WipeTower.cpp.
// Contains H2C-specific WipeTower member function bodies:
//   - nozzle_change()     — legacy nozzle-change extrusion path
//   - is_need_ramming()   — check if nozzle change needs ramming
//   - is_same_extruder()  — check if two filaments share an extruder
//   - is_same_nozzle()    — check if two filaments share a nozzle
//
// MUST be #include'd inside WipeTower.cpp where WipeTowerWriter,
// box_coordinates, wipe_tower_wall_infill_overlap etc. are visible.
//
// BBL parity: BambuStudio WipeTower.cpp (commit 3f2570c)
// ============================================================================

// ---- nozzle_change() -------------------------------------------------------
// H2C: Legacy nozzle-change — extrude nozzle-change block in wipe tower.
// Called from tool_change() when is_need_ramming() returns true.
// BBL ref: BambuStudio WipeTower.cpp:2103-2310 (commit 3f2570c)
WipeTower::NozzleChangeResult WipeTower::nozzle_change(int old_filament_id, int new_filament_id)
{
    float wipe_depth               = 0.f;
    float wipe_length              = 0.f;
    float purge_volume             = 0.f;
    int   nozzle_change_line_count = 0;

    // Finds this toolchange info
    if (new_filament_id != (unsigned int) (-1)) {
        for (const auto &b : m_layer_info->tool_changes)
            if (b.new_tool == new_filament_id) {
                wipe_length              = b.wipe_length;
                wipe_depth               = b.required_depth;
                purge_volume             = b.purge_volume;
                if (has_tpu_filament())
                    nozzle_change_line_count = ((b.nozzle_change_depth + WT_EPSILON) / m_nozzle_change_perimeter_width) / 2;
                else
                    nozzle_change_line_count = (b.nozzle_change_depth + WT_EPSILON) / m_nozzle_change_perimeter_width;
                break;
            }
    } else {
        // Otherwise we are going to Unload only. And m_layer_info would be invalid.
    }

    float nozzle_change_speed = 60.0f * m_filpar[m_current_tool].max_e_speed / m_extrusion_flow;
    if (is_tpu_filament(m_current_tool)) {
        nozzle_change_speed *= 0.25;
    }

    WipeTowerWriter writer(m_layer_height, m_perimeter_width, m_gcode_flavor, m_filpar);
    writer.set_extrusion_flow(m_extrusion_flow)
        .set_z(m_z_pos)
        .set_initial_tool(m_current_tool)
        .set_extrusion_flow(m_extrusion_flow)
        .set_y_shift(m_y_shift + (new_filament_id != (unsigned int) (-1) && (m_current_shape == SHAPE_REVERSED) ? m_layer_info->depth - m_layer_info->toolchanges_depth() : 0.f))
        // H2C FIX: Emit structured NozzleChangeStart tag with ON/NN nozzle IDs.
        // Ref: BambuStudio WipeTower.cpp:2210-2213 (commit 3f2570c)
        .append([&]() {
            char buff[64];
            int old_noz = get_nozzle_id(old_filament_id, m_cur_layer_id);
            int new_noz = get_nozzle_id(new_filament_id, m_cur_layer_id);
            snprintf(buff, sizeof(buff), ";%s OF%d NF%d ON%d NN%d\n",
                     GCodeProcessor::reserved_tag(GCodeProcessor::ETags::NozzleChangeStart).c_str(),
                     old_filament_id, new_filament_id, old_noz, new_noz);
            return std::string(buff);
        }());

    box_coordinates cleaning_box(Vec2f(m_perimeter_width, m_perimeter_width), m_wipe_tower_width - 2 * m_perimeter_width,
                                 (new_filament_id != (unsigned int) (-1) ? wipe_depth + m_depth_traversed - m_perimeter_width : m_wipe_tower_depth - m_perimeter_width));

    Vec2f initial_position = cleaning_box.ld + Vec2f(0.f, m_depth_traversed);
    writer.set_initial_position(initial_position, m_wipe_tower_width, m_wipe_tower_depth, m_internal_rotation);

    const float &xl = cleaning_box.ld.x();
    const float &xr = cleaning_box.rd.x();

    float dy = m_layer_info->extra_spacing * m_perimeter_width;
    if (has_tpu_filament())
        dy = 2 * m_perimeter_width;

    float start_y = writer.y();

    m_left_to_right = true;

    bool need_change_flow = false;
    // now the wiping itself:
    for (int i = 0; true; ++i) {
        if (m_left_to_right)
            writer.extrude(xr + wipe_tower_wall_infill_overlap * m_perimeter_width, writer.y(), nozzle_change_speed);
        else
            writer.extrude(xl - wipe_tower_wall_infill_overlap * m_perimeter_width, writer.y(), nozzle_change_speed);

        if (writer.y() - float(EPSILON) > cleaning_box.lu.y())
            break; // in case next line would not fit

        if (i == nozzle_change_line_count - 1)
            break;

        // stepping to the next line:
        writer.extrude(writer.x(), writer.y() + dy);
        m_left_to_right = !m_left_to_right;
    }

    writer.set_extrusion_flow(m_extrusion_flow); // Reset the extrusion flow.

    m_depth_traversed += nozzle_change_line_count * dy;

    NozzleChangeResult result;

    if (is_tpu_filament(m_current_tool))
    {
        bool left_to_right = !m_left_to_right;
        double tpu_travel_length        = 5;
        double e_flow                   = extrusion_flow(m_layer_height);
        double length                   = tpu_travel_length / e_flow;
        int    tpu_line_count = length / (m_wipe_tower_width - 2 * m_perimeter_width) + 1;

        writer.travel(writer.x(), writer.y() - m_perimeter_width);

        for (int i = 0; true; ++i) {
            if (left_to_right)
                writer.travel(xr - m_perimeter_width, writer.y(), nozzle_change_speed);
            else
                writer.travel(xl + m_perimeter_width, writer.y(), nozzle_change_speed);

            if (i == tpu_line_count - 1)
                break;

            writer.travel(writer.x(), writer.y() - dy);
            left_to_right = !left_to_right;
        }
    }
    else {
        result.wipe_path.push_back(writer.pos());
        if (m_left_to_right) {
             result.wipe_path.push_back(Vec2f(0, writer.y()));
        } else {
             result.wipe_path.push_back(Vec2f(m_wipe_tower_width, writer.y()));
        }
    }

    // H2C FIX: Matching NozzleChangeEnd tag with ON/NN nozzle IDs.
    // Ref: BambuStudio WipeTower.cpp:2306 (commit 3f2570c)
    {
        char buff[64];
        int old_noz = get_nozzle_id(old_filament_id, m_cur_layer_id);
        int new_noz = get_nozzle_id(new_filament_id, m_cur_layer_id);
        snprintf(buff, sizeof(buff), ";%s OF%d NF%d ON%d NN%d\n",
                 GCodeProcessor::reserved_tag(GCodeProcessor::ETags::NozzleChangeEnd).c_str(),
                 old_filament_id, new_filament_id, old_noz, new_noz);
        writer.append(std::string(buff));
    }

    result.start_pos = writer.start_pos_rotated();
    result.end_pos   = writer.pos();
    result.gcode     = std::move(writer.gcode());
    return result;
}

// ---- is_need_ramming / is_same_extruder / is_same_nozzle -------------------
// H2C: Nozzle routing helpers — query NozzleGroupResult for filament mapping.
// All three treat a missing nozzle group result as "single-extruder,
// single-nozzle printer" so AMS-style multi-color keeps working.
// BBL ref: BambuStudio WipeTower.cpp:2505-2520 (commit 3f2570c)

bool WipeTower::is_need_ramming(int filament_id_1, int filament_id_2, int layer_id)
{
    if (!m_multi_nozzle_group_result)
        return false;
    return !m_multi_nozzle_group_result->are_filaments_same_nozzle(filament_id_1, filament_id_2, layer_id);
}

bool WipeTower::is_same_extruder(int filament_id_1, int filament_id_2, int layer_id)
{
    if (!m_multi_nozzle_group_result) return true;
    return m_multi_nozzle_group_result->are_filaments_same_extruder(filament_id_1, filament_id_2, layer_id);
}

bool WipeTower::is_same_nozzle(int filament_id_1, int filament_id_2, int layer_id)
{
    if (!m_multi_nozzle_group_result) return true;
    return m_multi_nozzle_group_result->are_filaments_same_nozzle(filament_id_1, filament_id_2, layer_id);
}
