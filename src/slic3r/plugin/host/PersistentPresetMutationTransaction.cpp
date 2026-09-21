#include "PersistentPresetMutationTransaction.hpp"

#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Tab.hpp"

#include <wx/app.h>
#include <wx/thread.h>

#include <algorithm>
#include <cctype>
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
    if (wxTheApp == nullptr)
        throw std::runtime_error("OrcaSlicer application is not initialized");
    if (!wxIsMainThread())
        throw std::runtime_error("Process preset mutation must run on the UI thread");
    PresetBundle* bundle = GUI::wxGetApp().preset_bundle;
    if (bundle == nullptr)
        throw std::runtime_error("Preset bundle is not available");

    PresetCollection& presets = bundle->prints;
    Snapshot before;
    before.selected_index = presets.get_selected_idx();
    before.selected_name = presets.get_selected_preset_name();
    before.selected = presets.get_selected_preset();
    before.edited = presets.get_edited_preset();
    before.selected_dirty = before.selected.is_dirty;
    before.edited_dirty = before.edited.is_dirty;
    return std::shared_ptr<PersistentPresetMutationTransaction>(
        new PersistentPresetMutationTransaction(*bundle, std::move(before)));
}

PersistentPresetMutationTransaction::PersistentPresetMutationTransaction(PresetBundle& bundle,
                                                                           Snapshot before,
                                                                           bool test_mode,
                                                                           TestingHooks hooks)
    : m_bundle(&bundle), m_before(std::move(before)), m_test_mode(test_mode),
      m_testing_hooks(hooks) {}

std::shared_ptr<PersistentPresetMutationTransaction>
PersistentPresetMutationTransaction::create_for_testing(PresetBundle& bundle, TestingHooks hooks)
{
    PresetCollection& presets = bundle.prints;
    Snapshot before;
    before.selected_index = presets.get_selected_idx();
    before.selected_name = presets.get_selected_preset_name();
    before.selected = presets.get_selected_preset();
    before.edited = presets.get_edited_preset();
    before.selected_dirty = before.selected.is_dirty;
    before.edited_dirty = before.edited.is_dirty;
    return std::shared_ptr<PersistentPresetMutationTransaction>(
        new PersistentPresetMutationTransaction(bundle, std::move(before), true, hooks));
}

void PersistentPresetMutationTransaction::refresh_process_lifecycle()
{
    // This is the same compatibility direction used by save_changes_for_preset().
    m_bundle->update_compatible(PresetSelectCompatibleType::Never);
    if (!m_test_mode) {
        if (GUI::Tab* tab = GUI::wxGetApp().get_tab(Preset::TYPE_PRINT)) {
            tab->update_dirty();
            return;
        }
    }
    {
        m_bundle->prints.update_dirty();
    }
}

void PersistentPresetMutationTransaction::restore_before_state()
{
    PresetCollection& presets = m_bundle->prints;
    if (presets.get_selected_preset_name() != m_before.selected_name)
        presets.select_preset_by_name(m_before.selected_name, true);
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
        PresetCollection& presets = m_bundle->prints;
        restore_before_state();
        // Persist restoration through the same Process preset lifecycle, then restore the
        // original edited working copy so unrelated unsaved edits remain unsaved.
        m_bundle->save_changes_for_preset(m_before.selected_name, Preset::TYPE_PRINT, {});
        restore_before_state();
        refresh_process_lifecycle();
        const auto read_back = persisted_values_from_disk(m_before.selected, requested);
        const auto expected = serialized_values(m_before.selected.config, requested);
        result.rollback_verified = read_back == expected &&
            presets.get_selected_preset_name() == m_before.selected_name;
        result.selection_changed = false;
        result.dirty_before = m_before.edited_dirty;
        result.dirty_after = presets.get_edited_preset().is_dirty;
        result.side_effects.emplace_back("process_compatibility_recalculated");
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
        throw std::runtime_error("Process preset mutation is blocked after failed recovery");
    if (m_state != PersistentPresetMutationState::Ready)
        throw std::runtime_error("Process preset mutation is one-shot");
    if (!wxIsMainThread())
        throw std::runtime_error("Process preset mutation must run on the UI thread");

    bool persistence_started = false;
    try {
        validate_patch_shape(patch);
        for (const auto& [key, unused] : patch) {
            (void) unused;
            const KeyClass key_class = classify_key(key);
            if (key_class == KeyClass::Confidential)
                throw std::invalid_argument("confidential_setting_rejected");
            if (key_class == KeyClass::Protected && !protected_approved)
                throw std::invalid_argument("protected_setting_requires_approval");
        }

        PresetCollection& presets = m_bundle->prints;
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
        m_bundle->save_changes_for_preset(m_before.selected_name, Preset::TYPE_PRINT, {});
        const std::map<std::string, std::string> persisted =
            persisted_values_from_disk(m_before.selected, patch);
        if (m_testing_hooks.force_readback_mismatch || persisted != staged)
            return rollback(patch, "persisted_readback_mismatch");

        presets.get_edited_preset().config = staged_edited;
        refresh_process_lifecycle();
        if (presets.get_selected_preset_name() != m_before.selected_name)
            return rollback(patch, "selection_invariant_failed");

        PersistentPresetMutationResult result;
        result.state = PersistentPresetMutationState::Committed;
        result.committed = true;
        result.selection_changed = false;
        result.dirty_before = m_before.edited_dirty;
        result.dirty_after = presets.get_edited_preset().is_dirty;
        result.applied = staged;
        result.persisted = persisted;
        result.side_effects = { "process_preset_persisted", "process_compatibility_recalculated" };
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
