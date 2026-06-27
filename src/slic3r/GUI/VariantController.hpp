#ifndef slic3r_GUI_VariantController_hpp_
#define slic3r_GUI_VariantController_hpp_

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/VariantOverrides.hpp"
#include <set>
#include <string>

namespace Slic3r { namespace GUI {

/// Isolation layer: owns ALL VO orchestration logic.
/// Tab only calls hooks — no VO logic remains in Tab.
///
/// Authoritative sources:
///   - Global mode:     Presets::edited_preset().config.variant_overrides()
///   - Per-object mode: ModelObject::config.get().variant_overrides()
///
/// Tab's edited_cfg/selected_cfg are working copies for display/dirty detection.
/// VariantController ensures they stay in sync with the authoritative source.
class VariantController {
public:
    // ── Setup ──

    /// Set the global config source (parent Tab's live config).
    void set_global_config(DynamicPrintConfig* cfg) { m_global = cfg; }

    /// Set the variant-aware key set (print_options_with_variant).
    void set_variant_keys(const std::set<std::string>& keys) { m_keys = keys; }

    // ── Object switching (two-phase: SAVE → apply() → LOAD) ──

    /// Phase 1: Save current scalars → VO → outgoing ModelConfig.
    /// Must be called BEFORE Tab::apply() which resets VO.
    void save_outgoing(
        DynamicPrintConfig& edited_cfg,
        ModelConfig*        old_object,
        int                 variant_idx
    );

    /// Phase 2: Load incoming object's VO merged on top of Global base.
    /// Must be called AFTER Tab restores Global VO base (post-apply).
    void load_incoming(
        DynamicPrintConfig& edited_cfg,
        DynamicPrintConfig& selected_cfg,
        ModelConfig*        new_object,
        int                 variant_idx
    );

    // ── Variant switching (Left ↔ Right) ──

    /// Save scalars → VO[old_vi], then apply VO[new_vi] → scalars.
    /// Operates on whatever VO is currently in edited_cfg (global or per-object).
    void switch_variant(
        DynamicPrintConfig& edited_cfg,
        DynamicPrintConfig& selected_cfg,
        int                 old_vi,
        int                 new_vi
    );

    // ── Value editing ──

    /// User changed a value in Tab UI.
    /// Saves scalar → VO at variant_idx, writes to ModelConfig if per-object.
    void on_value_changed(
        DynamicPrintConfig& edited_cfg,
        const std::string&  key,
        int                 variant_idx,
        ModelConfig*        object_config   // nullptr = global edit
    );

    // ── Reset ──

    /// Reset single key for specific variant_idx: erase from per-object VO + ModelConfig.
    /// Only erases the active extruder's slot (NaN sentinel), not all variants.
    /// Tab should then display the Global value (from selected_cfg baseline).
    void reset_key(
        DynamicPrintConfig& edited_cfg,
        const std::string&  key,
        int                 variant_idx,
        ModelConfig*        object_config
    );

    /// Reset all per-object VO. Clears edited, selected, and ModelConfig VO.
    void reset_all(
        DynamicPrintConfig& edited_cfg,
        DynamicPrintConfig& selected_cfg,
        ModelConfig*        object_config
    );

private:
    DynamicPrintConfig*   m_global = nullptr;    // Global preset live config (NOT owned)
    std::set<std::string> m_keys;                // Variant-aware key set
public:
    const std::set<std::string>& keys() const { return m_keys; }
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_VariantController_hpp_
