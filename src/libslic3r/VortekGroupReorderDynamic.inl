// ============================================================================
// VortekGroupReorderDynamic.inl
//
// Inline include — extracted from ToolOrdering::reorder_extruders_for_minimum_flush_volume().
// H2C dynamic GroupReorder: per-combo-range optimization that tracks nozzle
// state across ranges, replacing single-pass filament-to-nozzle assignment.
//
// MUST be #include'd inside reorder_extruders_for_minimum_flush_volume()
// where support_multi_nozzle, m_print, layer_filaments, filament_lists,
// filament_sequences, get_custom_seq, nozzle_flush_mtx, map_mode,
// m_initial_nozzle_status, m_nozzle_status are in scope.
//
// BBL ref: BambuStudio/src/libslic3r/GCode/ToolOrdering.cpp:1906-1940 (wire-up),
//          BambuStudio/src/libslic3r/GCode/ToolOrdering.cpp:2544-2570 (reorder path)
// ============================================================================

if(support_multi_nozzle && m_print->get_layered_nozzle_group_result()){
    // H2C port: Dynamic GroupReorder — per combo range optimization.
    if (m_print->is_dynamic_group_reorder()) {
        BOOST_LOG_TRIVIAL(info) << "[H2C-GR] Using dynamic GroupReorder (per combo range)";

        auto layer_data = collect_layer_and_unprintable_data();

        auto grouping_context = GroupReorder::build_filament_group_context(
            m_print, layer_data.layer_filaments, layer_data.physical_unprintables,
            layer_data.geometric_unprintables, layer_data.filament_unprintable_volumes,
            map_mode, m_initial_nozzle_status.get_nozzle_filament_map());

        OrderingContext order_ctx;
        order_ctx.filament_lists = filament_lists;
        order_ctx.get_custom_seq = get_custom_seq;
        order_ctx.support_multi_nozzle = support_multi_nozzle;
        order_ctx.support_dynamic_map = true;

        MultiNozzleUtils::NozzleStatusRecorder best_nozzle_status = m_initial_nozzle_status;
        auto dynamic_plan_res = plan_filament_mapping_and_order_by_combo_ranges(
            m_print, grouping_context, order_ctx, FilamentMapMode::fmmAutoForFlush,
            layer_data.physical_unprintables, layer_data.geometric_unprintables,
            layer_data.filament_unprintable_volumes, &best_nozzle_status);

        if (!dynamic_plan_res.empty()) {
            filament_sequences.resize(layer_filaments.size());
            for (size_t layer_id = 0; layer_id < dynamic_plan_res.size(); ++layer_id) {
                auto& res = dynamic_plan_res[layer_id];
                filament_sequences[layer_id].resize(res.fil_order.size());
                std::transform(res.fil_order.begin(), res.fil_order.end(),
                    filament_sequences[layer_id].begin(), [](int v){ return (unsigned int)v; });
            }
            m_nozzle_status = best_nozzle_status;
            BOOST_LOG_TRIVIAL(info) << "[H2C-GR] Dynamic plan produced " << dynamic_plan_res.size() << " layer results";
        } else {
            BOOST_LOG_TRIVIAL(warning) << "[H2C-GR] Dynamic plan empty, falling back to static reorder";
            reorder_filaments_for_multi_nozzle_extruder(
                filament_lists,
                *m_print->get_layered_nozzle_group_result(),
                layer_filaments,
                nozzle_flush_mtx,
                get_custom_seq,
                &filament_sequences
            );
        }
    } else {
        reorder_filaments_for_multi_nozzle_extruder(
            filament_lists,
            *m_print->get_layered_nozzle_group_result(),
            layer_filaments,
            nozzle_flush_mtx,
            get_custom_seq,
            &filament_sequences
        );
    }
}
else{
    reorder_filaments_for_minimum_flush_volume(
        filament_lists,
        m_print->is_BBL_printer() ? filament_maps : maps_without_group,
        layer_filaments,
        nozzle_flush_mtx,
        get_custom_seq,
        &filament_sequences
    );
}
