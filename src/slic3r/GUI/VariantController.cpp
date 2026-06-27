#include "VariantController.hpp"
#include <boost/log/trivial.hpp>

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
    if (old_vo.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "[H2C-VC] save_outgoing: SKIP (no per-object VO)";
        return;
    }

    // Save current scalars → edited VO
    edited_cfg.save_variant_overrides(variant_idx, m_keys);

    // Copy ONLY keys that already exist in per-object VO
    const auto& src_vo = edited_cfg.variant_overrides();
    for (const auto& [key, vals] : old_vo.floats) {
        if (src_vo.has(key))
            old_vo.copy_key_from(key, src_vo);
    }
    BOOST_LOG_TRIVIAL(warning) << "[H2C-VC] save_outgoing: saved per-object keys=" << old_vo.floats.size();
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
            BOOST_LOG_TRIVIAL(warning) << "[H2C-VC] load_incoming: MERGED per-object VO, per_obj_keys="
                << mc_vo.floats.size() << " total_keys=" << ed_vo.floats.size();
        } else {
            BOOST_LOG_TRIVIAL(warning) << "[H2C-VC] load_incoming: no per-object VO, using Global";
        }
    } else {
        BOOST_LOG_TRIVIAL(warning) << "[H2C-VC] load_incoming: no object, using Global VO";
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
    BOOST_LOG_TRIVIAL(warning) << "[H2C-VC] switch_variant: old_vi=" << old_vi
        << " new_vi=" << new_vi;

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
            if (edited_opt && sel_opt) {
                edited_opt->set(sel_opt);
                BOOST_LOG_TRIVIAL(warning) << "[H2C-VC] switch_variant: RESTORE " << key
                    << " from selected (NaN at vi=" << new_vi << ")";
            }
        }
    }

    // Log a sample key to verify values and dirty state
    if (edited_cfg.has("outer_wall_speed")) {
        auto ed_val = edited_cfg.opt_float("outer_wall_speed");
        auto sel_val = selected_cfg.opt_float("outer_wall_speed");
        BOOST_LOG_TRIVIAL(warning) << "[H2C-VC] switch_variant: after apply, outer_wall_speed"
            << " edited=" << ed_val << " selected=" << sel_val
            << " match=" << (ed_val == sel_val)
            << " has_vo=" << edited_cfg.variant_overrides().has("outer_wall_speed")
            << " ed_vo[" << new_vi << "]=" << edited_cfg.variant_overrides().has_variant("outer_wall_speed", new_vi)
            << " sel_vo[" << new_vi << "]=" << selected_cfg.variant_overrides().has_variant("outer_wall_speed", new_vi);
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
    BOOST_LOG_TRIVIAL(warning) << "[H2C-VC] on_value_changed: key=" << key
        << " vi=" << variant_idx
        << " per_object=" << (object_config ? "yes" : "no");

    edited_cfg.save_variant_overrides(variant_idx, {key}, /*force=*/true);

    if (object_config) {
        auto& model_vo = const_cast<DynamicPrintConfig&>(object_config->get()).variant_overrides();
        model_vo.copy_key_from(key, edited_cfg.variant_overrides());
        BOOST_LOG_TRIVIAL(warning) << "[H2C-VC] on_value_changed: wrote to ModelConfig VO, "
            << "model_vo.has(" << key << ")=" << model_vo.has(key)
            << " has_variant[" << variant_idx << "]=" << model_vo.has_variant(key, variant_idx);
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
    BOOST_LOG_TRIVIAL(warning) << "[H2C-VC] reset_key: key=" << key
        << " vi=" << variant_idx
        << " per_object=" << (object_config ? "yes" : "no");

    edited_cfg.variant_overrides().erase_variant(key, variant_idx);

    if (object_config) {
        const_cast<DynamicPrintConfig&>(object_config->get()).variant_overrides().erase_variant(key, variant_idx);
        BOOST_LOG_TRIVIAL(warning) << "[H2C-VC] reset_key: after erase, edited has_key="
            << edited_cfg.variant_overrides().has(key)
            << " model has_key="
            << object_config->get().variant_overrides().has(key);
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
    BOOST_LOG_TRIVIAL(warning) << "[H2C-VC] reset_all: per_object=" << (object_config ? "yes" : "no");

    if (object_config) {
        auto& vo = const_cast<DynamicPrintConfig&>(object_config->get()).variant_overrides();
        BOOST_LOG_TRIVIAL(warning) << "[H2C-VC] reset_all: clearing ModelConfig VO, had "
            << vo.floats.size() << " keys";
        vo.clear();
    }
}

}} // namespace Slic3r::GUI
