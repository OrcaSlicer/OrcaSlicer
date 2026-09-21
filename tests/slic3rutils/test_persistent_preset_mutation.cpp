#include <catch2/catch_all.hpp>

#include "libslic3r/PresetBundle.hpp"
#include "slic3r/plugin/host/PersistentPresetMutationTransaction.hpp"
#include "test_utils.hpp"

#include <vector>

namespace {

Slic3r::PresetBundle process_bundle(const boost::filesystem::path& root)
{
    Slic3r::PresetBundle bundle;
    Slic3r::Preset& selected = bundle.prints.get_selected_preset();
    selected.name = "G4 Process Test";
    selected.file = (root / "g4-process-test.json").string();
    selected.is_default = false;
    selected.is_system = false;
    selected.is_external = false;
    bundle.prints.get_edited_preset() = selected;
    return bundle;
}

} // namespace

TEST_CASE("Process persistent mutation commits copied canonical settings", "[PluginHost][PersistentMutation]")
{
    ScopedTemporaryDir root("orca-process-mutation");
    Slic3r::PresetBundle bundle = process_bundle(root.path());
    auto transaction = Slic3r::PersistentPresetMutationTransaction::create_for_testing(bundle);

    const auto result = transaction->execute({ { "outer_wall_speed", "123" } }, false);

    CHECK(result.committed);
    CHECK(result.persisted.at("outer_wall_speed") == "123");
    CHECK(bundle.prints.get_selected_preset_name() == "G4 Process Test");
    CHECK_FALSE(result.selection_changed);
    CHECK(result.dirty_before == result.dirty_after);
    CHECK(result.side_effects == std::vector<std::string>{
        "process_preset_persisted", "process_compatibility_recalculated" });
}

TEST_CASE("Process persistent mutation rejects policy and strict-stage failures without writes", "[PluginHost][PersistentMutation]")
{
    ScopedTemporaryDir root("orca-process-mutation-reject");
    for (const auto& patch : {
             std::map<std::string, std::string>{ { "outer_wall_speed", "not-a-number" } },
             std::map<std::string, std::string>{ { "machine_start_gcode", "G28" } },
             std::map<std::string, std::string>{ { "api_token", "never-returned" } } }) {
        Slic3r::PresetBundle bundle = process_bundle(root.path());
        const std::string before = bundle.prints.get_selected_preset().config.opt_serialize("outer_wall_speed");
        auto transaction = Slic3r::PersistentPresetMutationTransaction::create_for_testing(bundle);
        const auto result = transaction->execute(patch, false);
        CHECK_FALSE(result.committed);
        CHECK(result.rollback_verified);
        CHECK(bundle.prints.get_selected_preset().config.opt_serialize("outer_wall_speed") == before);
    }
}

TEST_CASE("Process persistent mutation validates the full staged configuration before writes", "[PluginHost][PersistentMutation]")
{
    ScopedTemporaryDir root("orca-process-mutation-validate");
    Slic3r::PresetBundle bundle = process_bundle(root.path());
    const std::string before = bundle.prints.get_selected_preset().config.opt_serialize("layer_height");
    auto transaction = Slic3r::PersistentPresetMutationTransaction::create_for_testing(bundle);

    const auto result = transaction->execute({ { "layer_height", "0" } }, false);

    CHECK_FALSE(result.committed);
    CHECK(result.error_code == "staged_config_validation_failed");
    CHECK(result.rollback_verified);
    CHECK_FALSE(result.rollback_attempted);
    CHECK(bundle.prints.get_selected_preset().config.opt_serialize("layer_height") == before);
}

TEST_CASE("Process persistent mutation redacts confidential input", "[PluginHost][PersistentMutation]")
{
    ScopedTemporaryDir root("orca-process-mutation-confidential");
    Slic3r::PresetBundle bundle = process_bundle(root.path());
    auto transaction = Slic3r::PersistentPresetMutationTransaction::create_for_testing(bundle);

    const auto result = transaction->execute({ { "api_token", "never-returned" } }, false);

    CHECK_FALSE(result.committed);
    CHECK(result.error_code == "confidential_setting_rejected");
    CHECK(result.applied.empty());
    CHECK(result.persisted.empty());
    CHECK(result.error_code.find("never-returned") == std::string::npos);
}

TEST_CASE("Process persistent mutation restores persisted and unsaved state on injected failure", "[PluginHost][PersistentMutation]")
{
    ScopedTemporaryDir root("orca-process-mutation-rollback");
    for (const auto hooks : { Slic3r::PersistentPresetMutationTransaction::TestingHooks{true, false, false},
                              Slic3r::PersistentPresetMutationTransaction::TestingHooks{false, true, false} }) {
        Slic3r::PresetBundle bundle = process_bundle(root.path());
        bundle.prints.get_edited_preset().config.set_deserialize_strict("inner_wall_speed", "77");
        bundle.prints.update_dirty();
        auto transaction = Slic3r::PersistentPresetMutationTransaction::create_for_testing(bundle, hooks);
        const auto result = transaction->execute({ { "outer_wall_speed", "123" } }, false);
        CHECK_FALSE(result.committed);
        CHECK(result.rollback_attempted);
        CHECK(result.rollback_verified);
        CHECK(result.dirty_before == result.dirty_after);
        CHECK(bundle.prints.get_edited_preset().config.opt_serialize("inner_wall_speed") == "77");
        CHECK(bundle.prints.get_selected_preset_name() == "G4 Process Test");
    }
}

TEST_CASE("Process persistent mutation blocks further execution after failed recovery", "[PluginHost][PersistentMutation]")
{
    ScopedTemporaryDir root("orca-process-mutation-failed-recovery");
    Slic3r::PresetBundle bundle = process_bundle(root.path());
    Slic3r::PersistentPresetMutationTransaction::TestingHooks hooks;
    hooks.force_save_failure = true;
    hooks.force_rollback_failure = true;
    auto transaction = Slic3r::PersistentPresetMutationTransaction::create_for_testing(bundle, hooks);
    CHECK(transaction->execute({ { "outer_wall_speed", "123" } }, false).state ==
          Slic3r::PersistentPresetMutationState::FailedRecovery);
    CHECK_THROWS(transaction->execute({ { "outer_wall_speed", "124" } }, false));
}
