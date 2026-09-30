#include "PersistentPresetMutationTransaction.hpp"

#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Tab.hpp"

#include <wx/app.h>
#include <wx/thread.h>

#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>

namespace Slic3r {
namespace {

constexpr size_t max_patch_entries = 64;
constexpr size_t max_key_bytes = 256;
constexpr size_t max_value_bytes = 8192;
constexpr size_t max_patch_bytes = 32768;

enum class KeyClass { Ordinary, Protected, Confidential };

bool contains_folded(const std::string& text, const char* needle)
{
    std::string folded = text;
    std::transform(folded.begin(), folded.end(), folded.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return folded.find(needle) != std::string::npos;
}

KeyClass classify_key(const std::string& key)
{
    for (const char* fragment : { "password", "secret", "token", "api_key", "apikey",
                                  "authorization", "access_code" })
        if (contains_folded(key, fragment))
            return KeyClass::Confidential;
    for (const char* protected_key : { "post_process", "slicing_pipeline_plugin", "plugins",
                                       "machine_start_gcode", "machine_end_gcode" })
        if (key == protected_key)
            return KeyClass::Protected;
    if (contains_folded(key, "gcode") || contains_folded(key, "machine"))
        return KeyClass::Protected;
    return KeyClass::Ordinary;
}

bool is_printer_policy_key(const std::string& key)
{
    static const std::set<std::string> bed_and_firmware_keys {
        "printable_area", "extruder_printable_area", "bed_exclude_area",
        "parallel_printheads_bed_exclude_areas", "support_parallel_printheads",
        "parallel_printheads_count", "printable_height", "extruder_printable_height",
        "extruder_clearance_radius", "extruder_clearance_height_to_lid",
        "extruder_clearance_height_to_rod", "nozzle_height", "z_offset",
        "bed_custom_texture", "bed_custom_model", "wrapping_exclude_area",
        "head_wrap_detect_zone", "gcode_flavor", "gcode_skip_config_block",
        "emit_machine_limits_to_gcode", "use_firmware_retraction",
    };
    const bool input_shaping = key.rfind("input_shaping_", 0) == 0;
    const bool custom_gcode = key.size() >= 6 && key.compare(key.size() - 6, 6, "_gcode") == 0;
    if (bed_and_firmware_keys.count(key) != 0 || input_shaping || custom_gcode)
        return true;
    const auto& machine_limits = Preset::machine_limits_options();
    if (std::find(machine_limits.begin(), machine_limits.end(), key) != machine_limits.end())
        return true;
    const auto& print_host = PhysicalPrinter::printer_options();
    return std::find(print_host.begin(), print_host.end(), key) != print_host.end();
}

KeyClass classify_key(Preset::Type type, const std::string& key)
{
    const KeyClass basic = classify_key(key);
    if (basic != KeyClass::Ordinary)
        return basic;
    if (type == Preset::TYPE_PRINTER &&
        (key == "flashforge_serial_number" || key == "printhost_password" ||
         key == "printhost_apikey"))
        return KeyClass::Confidential;
    return type == Preset::TYPE_PRINTER && is_printer_policy_key(key)
        ? KeyClass::Protected : KeyClass::Ordinary;
}

void validate_patch_shape(const std::map<std::string, std::string>& patch)
{
    if (patch.empty() || patch.size() > max_patch_entries)
        throw std::invalid_argument("invalid_patch_shape");
    size_t bytes = 0;
    for (const auto& [key, value] : patch) {
        if (key.empty() || key.size() > max_key_bytes || value.size() > max_value_bytes)
            throw std::invalid_argument("invalid_patch_shape");
        bytes += key.size() + value.size();
        if (bytes > max_patch_bytes)
            throw std::invalid_argument("invalid_patch_shape");
    }
}

std::map<std::string, std::string> serialized_values(
    const DynamicPrintConfig& config, const std::map<std::string, std::string>& patch)
{
    std::map<std::string, std::string> values;
    for (const auto& [key, unused] : patch) {
        (void) unused;
        values.emplace(key, config.opt_serialize(key));
    }
    return values;
}

std::map<std::string, std::string> persisted_values_from_disk(
    const Preset& before_persisted, const std::map<std::string, std::string>& patch)
{
    // Read the real preset file into a new config. Seed it with the captured
    // persisted state because Orca user preset files may contain only a diff.
    DynamicPrintConfig file_values;
    std::map<std::string, std::string> key_values;
    std::string reason;
    file_values.load_from_json(before_persisted.file,
                               ForwardCompatibilitySubstitutionRule::Disable,
                               key_values, reason);
    if (!reason.empty())
        throw std::runtime_error("persisted_readback_failed");
    DynamicPrintConfig independent = before_persisted.config;
    independent.apply(std::move(file_values));
    return serialized_values(independent, patch);
}

} // namespace

const char* persistent_preset_mutation_state_name(PersistentPresetMutationState state) noexcept
{
    switch (state) {
    case PersistentPresetMutationState::Ready: return "ready";
    case PersistentPresetMutationState::Committed: return "committed";
    case PersistentPresetMutationState::RolledBack: return "rolled_back";
    case PersistentPresetMutationState::FailedRecovery: return "failed_recovery";
    }
    return "unknown";
}

std::shared_ptr<PersistentPresetMutationTransaction>
PersistentPresetMutationTransaction::capture_live_process()
{
    return capture_live(Preset::TYPE_PRINT);
}

std::shared_ptr<PersistentPresetMutationTransaction>
PersistentPresetMutationTransaction::capture_live_printer()
{
    return capture_live(Preset::TYPE_PRINTER);
}

std::shared_ptr<PersistentPresetMutationTransaction>
PersistentPresetMutationTransaction::capture_live(Preset::Type type)
{
    if (wxTheApp == nullptr)
        throw std::runtime_error("OrcaSlicer application is not initialized");
    if (!wxIsMainThread())
        throw std::runtime_error("Process preset mutation must run on the UI thread");
    PresetBundle* bundle = GUI::wxGetApp().preset_bundle;
    if (bundle == nullptr)
        throw std::runtime_error("Preset bundle is not available");

    PresetCollection& presets = type == Preset::TYPE_PRINT
        ? static_cast<PresetCollection&>(bundle->prints)
        : static_cast<PresetCollection&>(bundle->printers);
    Snapshot before;
    before.selected = Preset(type, "");
    before.edited = Preset(type, "");
    before.selected_index = presets.get_selected_idx();
    before.selected_name = presets.get_selected_preset_name();
    before.selected = presets.get_selected_preset();
    before.edited = presets.get_edited_preset();
    before.selected_dirty = before.selected.is_dirty;
    before.edited_dirty = before.edited.is_dirty;
    return std::shared_ptr<PersistentPresetMutationTransaction>(
        new PersistentPresetMutationTransaction(*bundle, type, std::move(before)));
}

PersistentPresetMutationTransaction::PersistentPresetMutationTransaction(PresetBundle& bundle, Preset::Type type,
                                                                           Snapshot before,
                                                                           bool test_mode,
                                                                           TestingHooks hooks)
    : m_bundle(&bundle), m_type(type), m_before(std::move(before)), m_test_mode(test_mode),
      m_testing_hooks(hooks) {}

std::shared_ptr<PersistentPresetMutationTransaction>
PersistentPresetMutationTransaction::create_for_testing(PresetBundle& bundle, TestingHooks hooks)
{
    return create_for_testing(bundle, Preset::TYPE_PRINT, hooks);
}

std::shared_ptr<PersistentPresetMutationTransaction>
PersistentPresetMutationTransaction::create_printer_for_testing(PresetBundle& bundle, TestingHooks hooks)
{
    return create_for_testing(bundle, Preset::TYPE_PRINTER, hooks);
}

std::shared_ptr<PersistentPresetMutationTransaction>
PersistentPresetMutationTransaction::create_for_testing(PresetBundle& bundle, Preset::Type type,
                                                          TestingHooks hooks)
{
    PresetCollection& presets = type == Preset::TYPE_PRINT
        ? static_cast<PresetCollection&>(bundle.prints)
        : static_cast<PresetCollection&>(bundle.printers);
    Snapshot before;
    before.selected = Preset(type, "");
    before.edited = Preset(type, "");
    before.selected_index = presets.get_selected_idx();
    before.selected_name = presets.get_selected_preset_name();
    before.selected = presets.get_selected_preset();
    before.edited = presets.get_edited_preset();
    before.selected_dirty = before.selected.is_dirty;
    before.edited_dirty = before.edited.is_dirty;
    return std::shared_ptr<PersistentPresetMutationTransaction>(
        new PersistentPresetMutationTransaction(bundle, type, std::move(before), true, hooks));
}

PresetCollection& PersistentPresetMutationTransaction::collection() const
{
    return m_type == Preset::TYPE_PRINT
        ? static_cast<PresetCollection&>(m_bundle->prints)
        : static_cast<PresetCollection&>(m_bundle->printers);
}

void PersistentPresetMutationTransaction::refresh_lifecycle()
{
    // This is the same compatibility direction used by save_changes_for_preset().
    m_bundle->update_compatible(PresetSelectCompatibleType::Never);
    if (!m_test_mode) {
        if (GUI::Tab* tab = GUI::wxGetApp().get_tab(m_type)) {
            tab->update_dirty();
            return;
        }
    }
    {
        collection().update_dirty();
    }
}

void PersistentPresetMutationTransaction::restore_before_state()
{
    PresetCollection& presets = collection();
    if (presets.get_selected_idx() != m_before.selected_index)
        presets.select_preset(m_before.selected_index);
    presets.get_selected_preset() = m_before.selected;
    presets.get_edited_preset() = m_before.edited;
    presets.get_selected_preset().is_dirty = m_before.selected_dirty;
    presets.get_edited_preset().is_dirty = m_before.edited_dirty;
}

PersistentPresetMutationResult PersistentPresetMutationTransaction::rollback(
    const std::map<std::string, std::string>& requested, const std::string& error_code)
{
    PersistentPresetMutationResult result;
    result.rollback_attempted = true;
    result.error_code = error_code;
    try {
        if (m_testing_hooks.force_rollback_failure)
            throw std::runtime_error("injected rollback failure");
        PresetCollection& presets = collection();
        restore_before_state();
        // Persist restoration through the same Process preset lifecycle, then restore the
        // original edited working copy so unrelated unsaved edits remain unsaved.
        m_bundle->save_changes_for_preset(m_before.selected_name, m_type, {});
        restore_before_state();
        refresh_lifecycle();
        const auto read_back = persisted_values_from_disk(m_before.selected, requested);
        const auto expected = serialized_values(m_before.selected.config, requested);
        result.rollback_verified = read_back == expected &&
            presets.get_selected_preset_name() == m_before.selected_name &&
            presets.get_selected_idx() == m_before.selected_index;
        result.selection_changed = false;
        result.dirty_before = m_before.edited_dirty;
        result.dirty_after = presets.get_edited_preset().is_dirty;
        result.side_effects.emplace_back(m_type == Preset::TYPE_PRINT
            ? "process_compatibility_recalculated" : "printer_compatibility_recalculated");
        if (result.rollback_verified) {
            result.state = PersistentPresetMutationState::RolledBack;
            m_state = result.state;
        } else {
            result.state = PersistentPresetMutationState::FailedRecovery;
            m_state = result.state;
        }
    } catch (...) {
        result.state = PersistentPresetMutationState::FailedRecovery;
        m_state = result.state;
    }
    return result;
}

PersistentPresetMutationResult PersistentPresetMutationTransaction::execute(
    const std::map<std::string, std::string>& patch, bool protected_approved)
{
    if (m_state == PersistentPresetMutationState::FailedRecovery)
        throw std::runtime_error("persistent preset mutation is blocked after failed recovery");
    if (m_state != PersistentPresetMutationState::Ready)
        throw std::runtime_error("Process preset mutation is one-shot");
    if (!wxIsMainThread())
        throw std::runtime_error("Process preset mutation must run on the UI thread");

    bool persistence_started = false;
    try {
        validate_patch_shape(patch);
        for (const auto& [key, unused] : patch) {
            (void) unused;
            const KeyClass key_class = classify_key(m_type, key);
            if (key_class == KeyClass::Confidential)
                throw std::invalid_argument("confidential_setting_rejected");
            if (key_class == KeyClass::Protected && !protected_approved)
                throw std::invalid_argument("protected_setting_requires_approval");
        }

        PresetCollection& presets = collection();
        DynamicPrintConfig staged_persisted = m_before.selected.config;
        DynamicPrintConfig staged_edited = m_before.edited.config;
        for (const auto& [key, value] : patch) {
            staged_persisted.set_deserialize_strict(key, value);
            staged_edited.set_deserialize_strict(key, value);
        }
        if (!staged_persisted.validate().empty())
            throw std::invalid_argument("staged_config_validation_failed");
        const std::map<std::string, std::string> staged = serialized_values(staged_persisted, patch);

        // save_changes_for_preset is the real Orca persistence/compatibility lifecycle.
        // It force-selects the saved name; restoring staged_edited immediately afterward
        // preserves all unrelated pre-existing user edits.
        presets.get_edited_preset().config = staged_persisted;
        persistence_started = true;
        if (m_testing_hooks.force_save_failure)
            throw std::runtime_error("injected save failure");
        m_bundle->save_changes_for_preset(m_before.selected_name, m_type, {});
        const std::map<std::string, std::string> persisted =
            persisted_values_from_disk(m_before.selected, patch);
        if (m_testing_hooks.force_readback_mismatch || persisted != staged)
            return rollback(patch, "persisted_readback_mismatch");

        presets.get_edited_preset().config = staged_edited;
        refresh_lifecycle();
        if (presets.get_selected_preset_name() != m_before.selected_name ||
            presets.get_selected_idx() != m_before.selected_index)
            return rollback(patch, "selection_invariant_failed");

        PersistentPresetMutationResult result;
        result.state = PersistentPresetMutationState::Committed;
        result.committed = true;
        result.selection_changed = false;
        result.dirty_before = m_before.edited_dirty;
        result.dirty_after = presets.get_edited_preset().is_dirty;
        result.applied = staged;
        result.persisted = persisted;
        result.side_effects = m_type == Preset::TYPE_PRINT
            ? std::vector<std::string>{ "process_preset_persisted", "process_compatibility_recalculated" }
            : std::vector<std::string>{ "printer_preset_persisted", "printer_compatibility_recalculated" };
        m_state = result.state;
        return result;
    } catch (const std::exception& error) {
        if (persistence_started)
            return rollback(patch, "persistent_mutation_failed");
        // Strict staging, policy and complete-config validation have not touched live state.
        PersistentPresetMutationResult result;
        result.state = PersistentPresetMutationState::RolledBack;
        result.rollback_verified = true;
        const std::string message = error.what();
        result.error_code = message == "protected_setting_requires_approval" ||
                            message == "confidential_setting_rejected" ||
                            message == "staged_config_validation_failed" ||
                            message == "invalid_patch_shape"
            ? message : "strict_deserialization_failed";
        result.dirty_before = m_before.edited_dirty;
        result.dirty_after = m_before.edited_dirty;
        m_state = result.state;
        return result;
    } catch (...) {
        if (persistence_started)
            return rollback(patch, "persistent_mutation_failed");
        PersistentPresetMutationResult result;
        result.state = PersistentPresetMutationState::RolledBack;
        result.rollback_verified = true;
        result.error_code = "strict_deserialization_failed";
        result.dirty_before = m_before.edited_dirty;
        result.dirty_after = m_before.edited_dirty;
        m_state = result.state;
        return result;
    }
}

} // namespace Slic3r
