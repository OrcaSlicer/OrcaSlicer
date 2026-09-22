#include "FilamentSlotMutationTransaction.hpp"

#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Tab.hpp"

#include <wx/app.h>
#include <wx/thread.h>

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <utility>

namespace Slic3r {
namespace {

constexpr size_t max_patch_entries = 64;
constexpr size_t max_key_bytes = 256;
constexpr size_t max_value_bytes = 8192;
constexpr size_t max_patch_bytes = 32768;

enum class KeyClass { Ordinary, Protected, Confidential };

struct PresetIdentity {
    std::string name;
    std::string file;
    bool is_default { false };
    bool is_system { false };
    bool is_external { false };
    bool is_project_embedded { false };
    std::string bundle_id;

    bool operator==(const PresetIdentity& other) const
    {
        return name == other.name && file == other.file && is_default == other.is_default &&
               is_system == other.is_system && is_external == other.is_external &&
               is_project_embedded == other.is_project_embedded && bundle_id == other.bundle_id;
    }
};

struct BoolArray {
    bool present { false };
    std::vector<unsigned char> values;

    bool operator==(const BoolArray& other) const { return present == other.present && values == other.values; }
};

struct IntArray {
    bool present { false };
    std::vector<int> values;

    bool operator==(const IntArray& other) const { return present == other.present && values == other.values; }
};

struct MappingSnapshot {
    std::vector<std::string> filament_presets;
    std::vector<size_t> physical_indices;
    BoolArray filament_is_mixed;
    IntArray filament_map;
    IntArray filament_nozzle_map;
    IntArray filament_volume_map;

    bool operator==(const MappingSnapshot& other) const
    {
        return filament_presets == other.filament_presets && physical_indices == other.physical_indices &&
               filament_is_mixed == other.filament_is_mixed && filament_map == other.filament_map &&
               filament_nozzle_map == other.filament_nozzle_map && filament_volume_map == other.filament_volume_map;
    }
};

struct CollectionSnapshot {
    size_t selected_index { size_t(-1) };
    std::string selected_name;
    std::string edited_name;
    DynamicPrintConfig selected_config;
    DynamicPrintConfig edited_config;
    bool selected_dirty { false };
    bool edited_dirty { false };

    bool equals(const CollectionSnapshot& other) const
    {
        return selected_index == other.selected_index && selected_name == other.selected_name &&
               edited_name == other.edited_name &&
               selected_dirty == other.selected_dirty && edited_dirty == other.edited_dirty &&
               selected_config.equals(other.selected_config) && edited_config.equals(other.edited_config);
    }
};

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
    return contains_folded(key, "gcode") ? KeyClass::Protected : KeyClass::Ordinary;
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
        (void)unused;
        values.emplace(key, config.opt_serialize(key));
    }
    return values;
}

PresetIdentity copy_identity(const Preset& preset)
{
    return { preset.name, preset.file, preset.is_default, preset.is_system, preset.is_external,
             preset.is_project_embedded, preset.bundle_id };
}

BoolArray copy_bool_array(const DynamicPrintConfig& config, const char* key)
{
    const auto* option = config.option<ConfigOptionBools>(key);
    return option == nullptr ? BoolArray{} : BoolArray{ true, option->values };
}

IntArray copy_int_array(const DynamicPrintConfig& config, const char* key)
{
    const auto* option = config.option<ConfigOptionInts>(key);
    return option == nullptr ? IntArray{} : IntArray{ true, option->values };
}

MappingSnapshot copy_mapping(const PresetBundle& bundle)
{
    const DynamicPrintConfig& project = bundle.project_config;
    return {
        bundle.filament_presets,
        bundle.physical_filament_config_indices(),
        copy_bool_array(project, "filament_is_mixed"),
        copy_int_array(project, "filament_map"),
        copy_int_array(project, "filament_nozzle_map"),
        copy_int_array(project, "filament_volume_map"),
    };
}

CollectionSnapshot copy_collection(const PresetCollection& filaments)
{
    if (filaments.get_selected_idx() == size_t(-1))
        throw std::invalid_argument("filament_collection_unselected");
    const Preset& selected = filaments.get_selected_preset();
    const Preset& edited = filaments.get_edited_preset();
    return { filaments.get_selected_idx(), filaments.get_selected_preset_name(), edited.name, selected.config, edited.config,
             selected.is_dirty, edited.is_dirty };
}

std::vector<size_t> shared_slot_indices(const PresetBundle& bundle, const std::string& resolved_name)
{
    std::vector<size_t> indices;
    for (size_t index = 0; index < bundle.filament_presets.size(); ++index) {
        const Preset* slot = bundle.filaments.find_preset(bundle.filament_presets[index], false);
        if (slot != nullptr && slot->name == resolved_name)
            indices.push_back(index);
    }
    return indices;
}

std::map<std::string, std::string> persisted_values_from_disk(
    const std::string& file, const DynamicPrintConfig& persisted_base,
    const std::map<std::string, std::string>& patch)
{
    DynamicPrintConfig file_values;
    std::map<std::string, std::string> key_values;
    std::string reason;
    file_values.load_from_json(file, ForwardCompatibilitySubstitutionRule::Disable, key_values, reason);
    if (!reason.empty())
        throw std::runtime_error("persisted_readback_failed");
    DynamicPrintConfig independent = persisted_base;
    independent.apply(std::move(file_values));
    return serialized_values(independent, patch);
}

template <class Option, class Array>
void restore_project_array(DynamicPrintConfig& project, const char* key, const Array& before)
{
    if (before.present) {
        project.option<Option>(key, true)->values = before.values;
    } else if (project.option<Option>(key) != nullptr) {
        project.erase(key);
    }
}

void restore_mapping(PresetBundle& bundle, const MappingSnapshot& before)
{
    bundle.filament_presets = before.filament_presets;
    DynamicPrintConfig& project = bundle.project_config;
    restore_project_array<ConfigOptionBools>(project, "filament_is_mixed", before.filament_is_mixed);
    restore_project_array<ConfigOptionInts>(project, "filament_map", before.filament_map);
    restore_project_array<ConfigOptionInts>(project, "filament_nozzle_map", before.filament_nozzle_map);
    restore_project_array<ConfigOptionInts>(project, "filament_volume_map", before.filament_volume_map);
}

void restore_original_collection(PresetCollection& filaments, const CollectionSnapshot& before)
{
    if (filaments.get_selected_idx() != before.selected_index)
        filaments.select_preset(before.selected_index);
    if (filaments.get_selected_idx() != before.selected_index ||
        filaments.get_selected_preset_name() != before.selected_name)
        throw std::runtime_error("collection_selection_restore_failed");
    filaments.get_selected_preset().config = before.selected_config;
    filaments.get_edited_preset().config = before.edited_config;
    filaments.get_edited_preset().name = before.edited_name;
    filaments.get_selected_preset().is_dirty = before.selected_dirty;
    filaments.get_edited_preset().is_dirty = before.edited_dirty;
}

void refresh_lifecycle(PresetBundle& bundle, bool test_mode)
{
    bundle.update_compatible(PresetSelectCompatibleType::Never);
    if (!test_mode) {
        if (GUI::Tab* tab = GUI::wxGetApp().get_tab(Preset::TYPE_FILAMENT)) {
            tab->update_dirty();
            return;
        }
    }
    bundle.filaments.update_dirty();
}

} // namespace

struct FilamentSlotMutationTransaction::Snapshot {
    size_t slot_index { size_t(-1) };
    std::string active_slot_name;
    PresetIdentity target;
    bool is_mixed { false };
    std::vector<size_t> shared_slots;
    MappingSnapshot mapping;
    CollectionSnapshot collection;
    DynamicPrintConfig target_config;
};

namespace {

std::unique_ptr<FilamentSlotMutationTransaction::Snapshot> capture_snapshot(PresetBundle& bundle, size_t slot_index)
{
    if (slot_index >= bundle.filament_presets.size())
        throw std::invalid_argument("filament_slot_out_of_range");
    if (bundle.is_mixed_filament(slot_index))
        throw std::invalid_argument("mixed_filament_slot_rejected");

    const std::string active_name = bundle.filament_presets[slot_index];
    Preset* target = bundle.filaments.find_preset(active_name, false, true);
    if (target == nullptr)
        throw std::invalid_argument("filament_slot_preset_unresolved");
    if (!target->is_user() || target->is_default || target->is_system || target->is_external ||
        target->is_project_embedded || target->is_from_bundle())
        throw std::invalid_argument("filament_slot_preset_not_user_owned");

    auto snapshot = std::make_unique<FilamentSlotMutationTransaction::Snapshot>();
    snapshot->slot_index = slot_index;
    snapshot->active_slot_name = active_name;
    snapshot->target = copy_identity(*target);
    snapshot->is_mixed = false;
    snapshot->shared_slots = shared_slot_indices(bundle, target->name);
    if (snapshot->shared_slots.size() != 1 || snapshot->shared_slots.front() != slot_index)
        throw std::invalid_argument("filament_slot_shared_preset_rejected");
    snapshot->mapping = copy_mapping(bundle);
    snapshot->collection = copy_collection(bundle.filaments);
    snapshot->target_config = target->config;
    return snapshot;
}

bool same_identity(const FilamentSlotMutationTransaction::Snapshot& before,
                   const FilamentSlotMutationTransaction::Snapshot& current)
{
    return before.slot_index == current.slot_index && before.active_slot_name == current.active_slot_name &&
           before.target == current.target && before.is_mixed == current.is_mixed &&
           before.shared_slots == current.shared_slots && before.mapping == current.mapping &&
           before.collection.equals(current.collection) && before.target_config.equals(current.target_config);
}

FilamentSlotMutationResult rejected_result(const FilamentSlotMutationTransaction::Snapshot& before,
                                           const std::string& error_code, bool mapping_changed)
{
    FilamentSlotMutationResult result;
    result.slot_index = before.slot_index;
    result.target_preset_name = before.target.name;
    result.state = PersistentPresetMutationState::RolledBack;
    result.rollback_verified = true;
    result.dirty_before = before.collection.edited_dirty;
    result.dirty_after = before.collection.edited_dirty;
    result.mapping_changed = mapping_changed;
    result.error_code = error_code;
    return result;
}

} // namespace

FilamentSlotMutationTransaction::FilamentSlotMutationTransaction(
    PresetBundle& bundle, std::unique_ptr<Snapshot> before, bool test_mode, TestingHooks hooks)
    : m_bundle(&bundle), m_before(std::move(before)), m_test_mode(test_mode), m_testing_hooks(hooks)
{}

FilamentSlotMutationTransaction::~FilamentSlotMutationTransaction() = default;

FilamentSlotMutationSnapshot FilamentSlotMutationTransaction::snapshot() const
{
    FilamentSlotMutationSnapshot result;
    result.slot_index = m_before->slot_index;
    result.active_slot_name = m_before->active_slot_name;
    result.target_preset_name = m_before->target.name;
    result.target_preset_file = m_before->target.file;
    result.target_is_default = m_before->target.is_default;
    result.target_is_system = m_before->target.is_system;
    result.target_is_external = m_before->target.is_external;
    result.target_is_project_embedded = m_before->target.is_project_embedded;
    result.target_bundle_id = m_before->target.bundle_id;
    result.is_mixed = m_before->is_mixed;
    result.shared_slot_indices = m_before->shared_slots;
    result.filament_presets = m_before->mapping.filament_presets;
    result.physical_filament_config_indices = m_before->mapping.physical_indices;
    result.filament_is_mixed_present = m_before->mapping.filament_is_mixed.present;
    for (const unsigned char value : m_before->mapping.filament_is_mixed.values)
        result.filament_is_mixed.push_back(value != 0);
    result.filament_map_present = m_before->mapping.filament_map.present;
    result.filament_map = m_before->mapping.filament_map.values;
    result.filament_nozzle_map_present = m_before->mapping.filament_nozzle_map.present;
    result.filament_nozzle_map = m_before->mapping.filament_nozzle_map.values;
    result.filament_volume_map_present = m_before->mapping.filament_volume_map.present;
    result.filament_volume_map = m_before->mapping.filament_volume_map.values;
    result.collection_selected_index = m_before->collection.selected_index;
    result.collection_selected_name = m_before->collection.selected_name;
    result.collection_edited_name = m_before->collection.edited_name;
    result.collection_selected_dirty = m_before->collection.selected_dirty;
    result.collection_edited_dirty = m_before->collection.edited_dirty;
    return result;
}

std::shared_ptr<FilamentSlotMutationTransaction>
FilamentSlotMutationTransaction::capture_live(size_t slot_index)
{
    if (wxTheApp == nullptr)
        throw std::runtime_error("OrcaSlicer application is not initialized");
    if (!wxIsMainThread())
        throw std::runtime_error("Filament slot mutation must run on the UI thread");
    PresetBundle* bundle = GUI::wxGetApp().preset_bundle;
    if (bundle == nullptr)
        throw std::runtime_error("Preset bundle is not available");
    return std::shared_ptr<FilamentSlotMutationTransaction>(
        new FilamentSlotMutationTransaction(*bundle, capture_snapshot(*bundle, slot_index), false, {}));
}

std::shared_ptr<FilamentSlotMutationTransaction>
FilamentSlotMutationTransaction::create_for_testing(PresetBundle& bundle, size_t slot_index, TestingHooks hooks)
{
    return std::shared_ptr<FilamentSlotMutationTransaction>(
        new FilamentSlotMutationTransaction(bundle, capture_snapshot(bundle, slot_index), true, hooks));
}

FilamentSlotMutationResult FilamentSlotMutationTransaction::rollback(
    const std::map<std::string, std::string>& requested, const std::string& error_code, bool mapping_changed)
{
    FilamentSlotMutationResult result;
    result.slot_index = m_before->slot_index;
    result.target_preset_name = m_before->target.name;
    result.rollback_attempted = true;
    result.mapping_changed = mapping_changed;
    result.error_code = error_code;
    try {
        if (m_testing_hooks.force_rollback_failure)
            throw std::runtime_error("injected rollback failure");
        PresetCollection& filaments = m_bundle->filaments;
        Preset* target = filaments.find_preset(m_before->target.name, false, true);
        if (target == nullptr || !(copy_identity(*target) == m_before->target))
            throw std::runtime_error("rollback_target_unavailable");
        Preset restored = *target;
        restored.config = m_before->target_config;
        filaments.save_current_preset(m_before->target.name, false, false, &restored);
        restore_mapping(*m_bundle, m_before->mapping);
        restore_original_collection(filaments, m_before->collection);
        refresh_lifecycle(*m_bundle, m_test_mode);

        const bool mapping_restored = copy_mapping(*m_bundle) == m_before->mapping;
        const bool collection_restored = copy_collection(filaments).equals(m_before->collection);
        const auto read_back = persisted_values_from_disk(m_before->target.file, m_before->target_config, requested);
        const auto expected = serialized_values(m_before->target_config, requested);
        result.rollback_verified = mapping_restored && collection_restored && read_back == expected;
        result.selection_changed = false;
        result.dirty_before = m_before->collection.edited_dirty;
        result.dirty_after = filaments.get_edited_preset().is_dirty;
        result.side_effects = { "filament_compatibility_recalculated" };
        result.state = result.rollback_verified ? PersistentPresetMutationState::RolledBack
                                                : PersistentPresetMutationState::FailedRecovery;
        m_state = result.state;
    } catch (...) {
        result.state = PersistentPresetMutationState::FailedRecovery;
        m_state = result.state;
    }
    return result;
}

FilamentSlotMutationResult FilamentSlotMutationTransaction::execute(
    const std::map<std::string, std::string>& patch, bool protected_approved)
{
    if (m_state == PersistentPresetMutationState::FailedRecovery)
        throw std::runtime_error("filament slot mutation is blocked after failed recovery");
    if (m_state != PersistentPresetMutationState::Ready)
        throw std::runtime_error("filament slot mutation is one-shot");
    if (!m_test_mode && !wxIsMainThread())
        throw std::runtime_error("Filament slot mutation must run on the UI thread");

    bool persistence_started = false;
    bool mapping_changed = false;
    try {
        std::unique_ptr<Snapshot> current;
        try {
            current = capture_snapshot(*m_bundle, m_before->slot_index);
        } catch (...) {
            mapping_changed = !(copy_mapping(*m_bundle) == m_before->mapping);
            FilamentSlotMutationResult result = rejected_result(*m_before, "stale_slot_identity", mapping_changed);
            m_state = result.state;
            return result;
        }
        mapping_changed = !(current->mapping == m_before->mapping);
        if (!same_identity(*m_before, *current)) {
            FilamentSlotMutationResult result = rejected_result(*m_before, "stale_slot_identity", mapping_changed);
            m_state = result.state;
            return result;
        }

        validate_patch_shape(patch);
        for (const auto& [key, unused] : patch) {
            (void)unused;
            const KeyClass key_class = classify_key(key);
            if (key_class == KeyClass::Confidential)
                throw std::invalid_argument("confidential_setting_rejected");
            if (key_class == KeyClass::Protected && !protected_approved)
                throw std::invalid_argument("protected_setting_requires_approval");
        }

        PresetCollection& filaments = m_bundle->filaments;
        Preset* target = filaments.find_preset(m_before->target.name, false, true);
        if (target == nullptr || !(copy_identity(*target) == m_before->target)) {
            FilamentSlotMutationResult result = rejected_result(*m_before, "stale_slot_identity", false);
            m_state = result.state;
            return result;
        }

        DynamicPrintConfig staged_target = m_before->target_config;
        DynamicPrintConfig staged_edited = m_before->collection.edited_config;
        const bool target_is_selected = m_before->collection.selected_name == m_before->target.name;
        for (const auto& [key, value] : patch) {
            staged_target.set_deserialize_strict(key, value);
            if (target_is_selected)
                staged_edited.set_deserialize_strict(key, value);
        }
        if (!staged_target.validate().empty())
            throw std::invalid_argument("staged_config_validation_failed");
        const std::map<std::string, std::string> staged = serialized_values(staged_target, patch);

        Preset staged_preset = *target;
        staged_preset.config = staged_target;
        persistence_started = true;
        if (m_testing_hooks.force_save_failure)
            throw std::runtime_error("injected save failure");
        // This targets the exact named user preset. Unlike save_changes_for_preset(TYPE_FILAMENT),
        // it does not synchronize slot 0 or alter the active slot mapping.
        filaments.save_current_preset(m_before->target.name, false, false, &staged_preset);
        const std::map<std::string, std::string> persisted =
            persisted_values_from_disk(m_before->target.file, m_before->target_config, patch);
        if (m_testing_hooks.force_readback_mismatch || persisted != staged)
            return rollback(patch, "persisted_readback_mismatch", false);

        if (!(copy_mapping(*m_bundle) == m_before->mapping))
            return rollback(patch, "mapping_invariant_failed", true);

        if (filaments.get_selected_idx() != m_before->collection.selected_index)
            filaments.select_preset(m_before->collection.selected_index);
        if (filaments.get_selected_idx() != m_before->collection.selected_index ||
            filaments.get_selected_preset_name() != m_before->collection.selected_name)
            return rollback(patch, "selection_invariant_failed", false);
        if (!target_is_selected)
            filaments.get_selected_preset().config = m_before->collection.selected_config;
        filaments.get_edited_preset().config = target_is_selected ? staged_edited : m_before->collection.edited_config;
        filaments.get_selected_preset().is_dirty = m_before->collection.selected_dirty;
        filaments.get_edited_preset().is_dirty = m_before->collection.edited_dirty;
        refresh_lifecycle(*m_bundle, m_test_mode);

        const bool selection_changed = filaments.get_selected_idx() != m_before->collection.selected_index ||
                                       filaments.get_selected_preset_name() != m_before->collection.selected_name;
        const bool final_mapping_changed = !(copy_mapping(*m_bundle) == m_before->mapping);
        if (selection_changed || final_mapping_changed ||
            filaments.get_edited_preset().is_dirty != m_before->collection.edited_dirty)
            return rollback(patch, selection_changed ? "selection_invariant_failed" : "mapping_invariant_failed",
                            final_mapping_changed);

        FilamentSlotMutationResult result;
        result.slot_index = m_before->slot_index;
        result.target_preset_name = m_before->target.name;
        result.state = PersistentPresetMutationState::Committed;
        result.committed = true;
        result.dirty_before = m_before->collection.edited_dirty;
        result.dirty_after = filaments.get_edited_preset().is_dirty;
        result.applied = staged;
        result.persisted = persisted;
        result.side_effects = { "filament_preset_persisted", "filament_compatibility_recalculated" };
        m_state = result.state;
        return result;
    } catch (const std::exception& error) {
        if (persistence_started)
            return rollback(patch, "filament_slot_mutation_failed", mapping_changed);
        const std::string message = error.what();
        const std::string error_code = message == "invalid_patch_shape" ||
                                       message == "protected_setting_requires_approval" ||
                                       message == "confidential_setting_rejected" ||
                                       message == "staged_config_validation_failed"
            ? message : "strict_deserialization_failed";
        FilamentSlotMutationResult result = rejected_result(*m_before, error_code, mapping_changed);
        m_state = result.state;
        return result;
    } catch (...) {
        if (persistence_started)
            return rollback(patch, "filament_slot_mutation_failed", mapping_changed);
        FilamentSlotMutationResult result = rejected_result(*m_before, "strict_deserialization_failed", mapping_changed);
        m_state = result.state;
        return result;
    }
}

} // namespace Slic3r
