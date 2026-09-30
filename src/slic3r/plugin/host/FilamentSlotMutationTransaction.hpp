#pragma once

#include "PersistentPresetMutationTransaction.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r {

class PresetBundle;

struct FilamentSlotMutationResult {
    size_t slot_index { size_t(-1) };
    std::string target_preset_name;
    PersistentPresetMutationState state { PersistentPresetMutationState::Ready };
    bool committed { false };
    bool rollback_attempted { false };
    bool rollback_verified { false };
    bool selection_changed { false };
    bool dirty_before { false };
    bool dirty_after { false };
    bool mapping_changed { false };
    std::map<std::string, std::string> applied;
    std::map<std::string, std::string> persisted;
    std::vector<std::string> side_effects;
    std::string error_code;
};

#if defined(SLIC3R_ENABLE_PLUGIN_HOST_TEST_API)
// Copied, immutable observation for an explicitly enabled native test build.
// It is not part of the distributed Plugin Host API and exposes no live host
// object or mutable container.
struct FilamentSlotMutationSnapshot {
    size_t slot_index { size_t(-1) };
    std::string active_slot_name;
    std::string target_preset_name;
    std::string target_preset_file;
    bool target_is_default { false };
    bool target_is_system { false };
    bool target_is_external { false };
    bool target_is_project_embedded { false };
    std::string target_bundle_id;
    bool is_mixed { false };
    std::vector<size_t> shared_slot_indices;
    std::vector<std::string> filament_presets;
    std::vector<size_t> physical_filament_config_indices;
    bool filament_is_mixed_present { false };
    std::vector<bool> filament_is_mixed;
    bool filament_map_present { false };
    std::vector<int> filament_map;
    bool filament_nozzle_map_present { false };
    std::vector<int> filament_nozzle_map;
    bool filament_volume_map_present { false };
    std::vector<int> filament_volume_map;
    size_t collection_selected_index { size_t(-1) };
    std::string collection_selected_name;
    std::string collection_edited_name;
    bool collection_selected_dirty { false };
    bool collection_edited_dirty { false };
};
#endif

// Opaque, UI-thread-only, one-shot transaction for one unique physical user
// Filament slot. It never exposes or retains live host handles across Python.
class FilamentSlotMutationTransaction {
public:
    struct Snapshot;

    struct TestingHooks {
        bool force_save_failure { false };
        bool force_readback_mismatch { false };
        bool force_rollback_failure { false };
    };

    static std::shared_ptr<FilamentSlotMutationTransaction> capture_live(size_t slot_index);
    // Native test seam only; never registered with pybind.
    static std::shared_ptr<FilamentSlotMutationTransaction> create_for_testing(
        PresetBundle& bundle, size_t slot_index, TestingHooks hooks = {});
    ~FilamentSlotMutationTransaction();

    FilamentSlotMutationTransaction(const FilamentSlotMutationTransaction&) = delete;
    FilamentSlotMutationTransaction& operator=(const FilamentSlotMutationTransaction&) = delete;

    FilamentSlotMutationResult execute(
        const std::map<std::string, std::string>& patch, bool protected_approved);
#if defined(SLIC3R_ENABLE_PLUGIN_HOST_TEST_API)
    // Native test-build seam only; never registered on the production host API.
    FilamentSlotMutationSnapshot snapshot_for_test() const;
#endif
    PersistentPresetMutationState state() const noexcept { return m_state; }

private:
    explicit FilamentSlotMutationTransaction(PresetBundle& bundle, std::unique_ptr<Snapshot> before,
                                             bool test_mode, TestingHooks hooks);

    FilamentSlotMutationResult rollback(const std::map<std::string, std::string>& requested,
                                        const std::string& error_code, bool mapping_changed);

    PresetBundle* m_bundle;
    std::unique_ptr<Snapshot> m_before;
    PersistentPresetMutationState m_state { PersistentPresetMutationState::Ready };
    bool m_test_mode { false };
    TestingHooks m_testing_hooks;
};

} // namespace Slic3r
