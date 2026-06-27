#include "VariantController.hpp"

namespace Slic3r { namespace GUI {

// ────────────────────────────────────────────────────────────────
// save_outgoing  (Phase 1: BEFORE apply())
// ────────────────────────────────────────────────────────────────
void VariantController::save_outgoing(
    DynamicPrintConfig& edited_cfg,
    ModelConfig*        old_object,
    int                 variant_idx)
{
    if (!old_object || variant_idx < 0 || m_keys.empty())
        return;

    auto& old_vo = const_cast<DynamicPrintConfig&>(old_object->get()).variant_overrides();
    if (old_vo.empty())
        return;

    // Save current scalars → edited VO
    edited_cfg.save_variant_overrides(variant_idx, m_keys);

    // Copy ONLY keys that already exist in per-object VO
    const auto& src_vo = edited_cfg.variant_overrides();
    for (const auto& [key, vals] : old_vo.floats) {
        if (src_vo.has(key))
            old_vo.copy_key_from(key, src_vo);
    }
}

// ────────────────────────────────────────────────────────────────
// load_incoming  (Phase 2: AFTER apply() + Global VO restore)
// ────────────────────────────────────────────────────────────────
void VariantController::load_incoming(
    DynamicPrintConfig& edited_cfg,
    DynamicPrintConfig& selected_cfg,
    ModelConfig*        new_object,
    int                 variant_idx)
{
    // edited_cfg already has Global VO base (restored by Tab).
    // Merge per-object overlay on top.
    if (new_object) {
        const auto& mc_vo = new_object->get().variant_overrides();
        if (!mc_vo.empty()) {
            auto& ed_vo = edited_cfg.variant_overrides();
            for (const auto& [key, vals] : mc_vo.floats)
                ed_vo.copy_key_from(key, mc_vo);
            for (const auto& [key, vals] : mc_vo.strings)
                ed_vo.strings[key] = vals;
        }
    }

    // selected_cfg always reflects Global (for dirty detection)
    if (m_global)
        selected_cfg.variant_overrides() = m_global->variant_overrides();
}

// ────────────────────────────────────────────────────────────────
// switch_variant
// ────────────────────────────────────────────────────────────────
void VariantController::switch_variant(
    DynamicPrintConfig& edited_cfg,
    DynamicPrintConfig& selected_cfg,
    int                 old_vi,
    int                 new_vi)
{
    if (old_vi >= 0)
        edited_cfg.save_variant_overrides(old_vi, m_keys);

    // Apply VO[new_vi] → scalars. Keys with NaN at new_vi are SKIPPED
    // by apply_to_config, so the scalar keeps the stale old-extruder value.
    edited_cfg.apply_variant_overrides(new_vi, m_keys);
    selected_cfg.apply_variant_overrides(new_vi, m_keys);

    // For keys where VO[new_vi] is NaN (reset / no per-object override),
    // restore the scalar from selected_cfg (parent/global baseline).
    // This ensures switching to an extruder without override shows the
    // parent value, not the stale value from the other extruder.
    const auto& vo = edited_cfg.variant_overrides();
    for (const auto& key : m_keys) {
        if (vo.has(key) && !vo.has_variant(key, new_vi)) {
            // NaN slot — restore from selected_cfg's current scalar
            // (selected_cfg just had apply_variant_overrides too,
            //  but it uses Global VO which should have valid values)
            ConfigOption* edited_opt  = edited_cfg.option(key, false);
            const ConfigOption* sel_opt = selected_cfg.option(key, false);
            if (edited_opt && sel_opt)
                edited_opt->set(sel_opt);
        }
    }
}

// ────────────────────────────────────────────────────────────────
// on_value_changed
// ────────────────────────────────────────────────────────────────
void VariantController::on_value_changed(
    DynamicPrintConfig& edited_cfg,
    const std::string&  key,
    int                 variant_idx,
    ModelConfig*        object_config)
{
    // Per-object: if the new value equals the global (parent) value,
    // treat it as a reset — remove the override instead of storing it.
    // Storing a value == parent creates a phantom dirty state.
    if (object_config && m_global) {
        const ConfigOption* edited_opt = edited_cfg.option(key);
        const ConfigOption* global_opt = m_global->option(key);
        if (edited_opt && global_opt && *edited_opt == *global_opt) {
            reset_key(edited_cfg, key, variant_idx, object_config);
            return;
        }
    }

    edited_cfg.save_variant_overrides(variant_idx, {key}, /*force=*/true);

    if (object_config) {
        auto& model_vo = const_cast<DynamicPrintConfig&>(object_config->get()).variant_overrides();
        model_vo.copy_key_from(key, edited_cfg.variant_overrides());
    }
}

// ────────────────────────────────────────────────────────────────
// reset_key
// ────────────────────────────────────────────────────────────────
void VariantController::reset_key(
    DynamicPrintConfig& edited_cfg,
    const std::string&  key,
    int                 variant_idx,
    ModelConfig*        object_config)
{
    edited_cfg.variant_overrides().erase_variant(key, variant_idx);

    if (object_config) {
        auto& model_cfg = const_cast<DynamicPrintConfig&>(object_config->get());
        model_cfg.variant_overrides().erase_variant(key, variant_idx);

        // If ALL VO variants for this key are gone, remove the option from
        // ModelConfig entirely. Otherwise a phantom option (value == parent)
        // stays in ModelConfig and marks the field as a per-object override.
        if (!model_cfg.variant_overrides().has(key))
            model_cfg.erase(key);
    }
}

// ────────────────────────────────────────────────────────────────
// reset_all
// ────────────────────────────────────────────────────────────────
void VariantController::reset_all(
    DynamicPrintConfig& /*edited_cfg*/,
    DynamicPrintConfig& /*selected_cfg*/,
    ModelConfig*        object_config)
{
    if (object_config) {
        auto& vo = const_cast<DynamicPrintConfig&>(object_config->get()).variant_overrides();
        vo.clear();
    }
}

}} // namespace Slic3r::GUI
