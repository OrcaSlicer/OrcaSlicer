#ifndef slic3r_PersistentPresetMutationTransaction_hpp_
#define slic3r_PersistentPresetMutationTransaction_hpp_

#include <libslic3r/Preset.hpp>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r {

enum class PersistentPresetMutationState {
    Ready,
    Committed,
    RolledBack,
    FailedRecovery,
};

struct PersistentPresetMutationResult {
    PersistentPresetMutationState state { PersistentPresetMutationState::Ready };
    bool committed { false };
    bool rollback_attempted { false };
    bool rollback_verified { false };
    bool selection_changed { false };
    bool dirty_before { false };
    bool dirty_after { false };
    std::map<std::string, std::string> applied;
    std::map<std::string, std::string> persisted;
    std::vector<std::string> side_effects;
    std::string error_code;
};

// Opaque, UI-thread-only, one-shot native transaction. It is deliberately Process
// only: no mutable Preset, PresetCollection, or DynamicPrintConfig crosses Python.
class PersistentPresetMutationTransaction {
public:
    struct TestingHooks {
        bool force_save_failure { false };
        bool force_readback_mismatch { false };
        bool force_rollback_failure { false };
    };

    static std::shared_ptr<PersistentPresetMutationTransaction> capture_live_process();
    // Native test seam only; never registered with pybind.
    static std::shared_ptr<PersistentPresetMutationTransaction> create_for_testing(
        PresetBundle& bundle, TestingHooks hooks = {});
    ~PersistentPresetMutationTransaction() = default;

    PersistentPresetMutationTransaction(const PersistentPresetMutationTransaction&) = delete;
    PersistentPresetMutationTransaction& operator=(const PersistentPresetMutationTransaction&) = delete;

    PersistentPresetMutationResult execute(
        const std::map<std::string, std::string>& patch, bool protected_approved);
    PersistentPresetMutationState state() const noexcept { return m_state; }

private:
    struct Snapshot {
        Snapshot()
            : selected(Preset::TYPE_PRINT, ""), edited(Preset::TYPE_PRINT, "") {}

        size_t selected_index { size_t(-1) };
        std::string selected_name;
        Preset selected;
        Preset edited;
        bool selected_dirty { false };
        bool edited_dirty { false };
    };

    explicit PersistentPresetMutationTransaction(PresetBundle& bundle, Snapshot before,
                                                 bool test_mode = false,
                                                 TestingHooks hooks = {});

    PersistentPresetMutationResult rollback(const std::map<std::string, std::string>& requested,
                                             const std::string& error_code);
    void restore_before_state();
    void refresh_process_lifecycle();

    PresetBundle* m_bundle;
    Snapshot m_before;
    PersistentPresetMutationState m_state { PersistentPresetMutationState::Ready };
    bool m_test_mode { false };
    TestingHooks m_testing_hooks;
};

const char* persistent_preset_mutation_state_name(PersistentPresetMutationState state) noexcept;

} // namespace Slic3r

#endif // slic3r_PersistentPresetMutationTransaction_hpp_
