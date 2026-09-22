#include <catch2/catch_all.hpp>

#include "libslic3r/PresetBundle.hpp"
#include "slic3r/plugin/host/FilamentSlotMutationTransaction.hpp"
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

Slic3r::PresetBundle printer_bundle(const boost::filesystem::path& root)
{
    Slic3r::PresetBundle bundle;
    Slic3r::Preset& selected = bundle.printers.get_selected_preset();
    selected.name = "G4 Printer Test";
    selected.file = (root / "g4-printer-test.json").string();
    selected.is_default = false;
    selected.is_system = false;
    selected.is_external = false;
    bundle.printers.get_edited_preset() = selected;
    return bundle;
}

Slic3r::PresetBundle filament_bundle(const boost::filesystem::path& root)
{
    Slic3r::PresetBundle bundle;
    Slic3r::Preset& target = bundle.filaments.get_selected_preset();
    target.name = "G4 Filament Target";
    target.file = (root / "g4-filament-target.json").string();
    target.is_default = false;
    target.is_system = false;
    target.is_external = false;
    target.is_project_embedded = false;
    target.bundle_id.clear();
    bundle.filaments.get_edited_preset() = target;

    Slic3r::DynamicPrintConfig other_config = target.config;
    Slic3r::Preset& other = bundle.filaments.load_preset(
        (root / "g4-filament-slot-zero.json").string(), "G4 Filament Slot Zero", other_config, false);
    other.is_default = false;
    other.is_system = false;
    other.is_external = false;
    other.is_project_embedded = false;
    other.bundle_id.clear();
    bundle.filament_presets = { other.name, target.name };
    bundle.set_num_filaments(2);
    bundle.filaments.select_preset_by_name(target.name, true);
    return bundle;
}

std::vector<std::string> filament_mapping(const Slic3r::PresetBundle& bundle)
{
    return bundle.filament_presets;
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

TEST_CASE("Printer persistent mutation commits copied canonical settings", "[PluginHost][PersistentMutation]")
{
    ScopedTemporaryDir root("orca-printer-mutation");
    Slic3r::PresetBundle bundle = printer_bundle(root.path());
    auto transaction = Slic3r::PersistentPresetMutationTransaction::create_printer_for_testing(bundle);

    const auto result = transaction->execute({ { "time_cost", "1" } }, false);

    CHECK(result.committed);
    CHECK(result.persisted.at("time_cost") == "1");
    CHECK(bundle.printers.get_selected_preset_name() == "G4 Printer Test");
    CHECK_FALSE(result.selection_changed);
    CHECK(result.dirty_before == result.dirty_after);
    CHECK(result.side_effects == std::vector<std::string>{
        "printer_preset_persisted", "printer_compatibility_recalculated" });
}

TEST_CASE("Printer persistent mutation rejects strict, validation, and protected policy failures", "[PluginHost][PersistentMutation]")
{
    ScopedTemporaryDir root("orca-printer-mutation-reject");
    for (const auto& patch : {
             std::map<std::string, std::string>{ { "time_cost", "not-a-number" } },
             std::map<std::string, std::string>{ { "machine_max_speed_x", "100" } },
             std::map<std::string, std::string>{ { "printhost_apikey", "never-returned" } } }) {
        Slic3r::PresetBundle bundle = printer_bundle(root.path());
        auto transaction = Slic3r::PersistentPresetMutationTransaction::create_printer_for_testing(bundle);
        const auto result = transaction->execute(patch, false);
        CHECK_FALSE(result.committed);
        CHECK(result.rollback_verified);
        CHECK(result.applied.empty());
        CHECK(result.persisted.empty());
    }

    Slic3r::PresetBundle validation_bundle = printer_bundle(root.path());
    auto validation = Slic3r::PersistentPresetMutationTransaction::create_printer_for_testing(validation_bundle);
    const auto validation_result = validation->execute({ { "nozzle_diameter", "0" } }, false);
    CHECK_FALSE(validation_result.committed);
    CHECK(validation_result.error_code == "staged_config_validation_failed");
}

TEST_CASE("Printer persistent mutation preserves unsaved edits and rolls back failures", "[PluginHost][PersistentMutation]")
{
    ScopedTemporaryDir root("orca-printer-mutation-rollback");
    for (const auto hooks : { Slic3r::PersistentPresetMutationTransaction::TestingHooks{true, false, false},
                              Slic3r::PersistentPresetMutationTransaction::TestingHooks{false, true, false} }) {
        Slic3r::PresetBundle bundle = printer_bundle(root.path());
        bundle.printers.get_edited_preset().config.set_deserialize_strict("printer_notes", "dirty");
        bundle.printers.update_dirty();
        auto transaction = Slic3r::PersistentPresetMutationTransaction::create_printer_for_testing(bundle, hooks);
        const auto result = transaction->execute({ { "time_cost", "1" } }, false);
        CHECK_FALSE(result.committed);
        CHECK(result.rollback_attempted);
        CHECK(result.rollback_verified);
        CHECK(result.dirty_before == result.dirty_after);
        CHECK(bundle.printers.get_edited_preset().config.opt_serialize("printer_notes") == "dirty");
        CHECK(bundle.printers.get_selected_preset_name() == "G4 Printer Test");
    }
}

TEST_CASE("Printer approved protected mutation stays native and failed recovery blocks reuse", "[PluginHost][PersistentMutation]")
{
    ScopedTemporaryDir root("orca-printer-mutation-policy");
    Slic3r::PresetBundle approved_bundle = printer_bundle(root.path());
    auto approved = Slic3r::PersistentPresetMutationTransaction::create_printer_for_testing(approved_bundle);
    const auto approved_result = approved->execute({ { "machine_max_speed_x", "100" } }, true);
    CHECK(approved_result.committed);
    CHECK(approved_result.persisted.at("machine_max_speed_x") == "100");

    Slic3r::PresetBundle failed_bundle = printer_bundle(root.path());
    Slic3r::PersistentPresetMutationTransaction::TestingHooks hooks;
    hooks.force_save_failure = true;
    hooks.force_rollback_failure = true;
    auto failed = Slic3r::PersistentPresetMutationTransaction::create_printer_for_testing(failed_bundle, hooks);
    CHECK(failed->execute({ { "time_cost", "1" } }, false).state ==
          Slic3r::PersistentPresetMutationState::FailedRecovery);
    CHECK_THROWS(failed->execute({ { "time_cost", "2" } }, false));
}

TEST_CASE("Filament slot persistent mutation commits only a unique physical user slot", "[PluginHost][FilamentSlotMutation]")
{
    ScopedTemporaryDir root("orca-filament-slot-mutation");
    Slic3r::PresetBundle bundle = filament_bundle(root.path());
    const auto before_mapping = filament_mapping(bundle);
    const auto before_physical = bundle.physical_filament_config_indices();
    const auto before_mixed = bundle.project_config.option<Slic3r::ConfigOptionBools>("filament_is_mixed")->values;
    const auto before_map = bundle.project_config.option<Slic3r::ConfigOptionInts>("filament_map")->values;
    const auto before_nozzle_map = bundle.project_config.option<Slic3r::ConfigOptionInts>("filament_nozzle_map")->values;
    const auto before_volume_map = bundle.project_config.option<Slic3r::ConfigOptionInts>("filament_volume_map")->values;
    const Slic3r::Preset* target_before = bundle.filaments.find_preset("G4 Filament Target", false, true);
    REQUIRE(target_before != nullptr);
    const Slic3r::DynamicPrintConfig before_target = target_before->config;
    auto transaction = Slic3r::FilamentSlotMutationTransaction::create_for_testing(bundle, 1);
    const auto captured = transaction->snapshot();

    CHECK(captured.slot_index == 1);
    CHECK(captured.active_slot_name == "G4 Filament Target");
    CHECK(captured.target_preset_name == "G4 Filament Target");
    CHECK(captured.shared_slot_indices == std::vector<size_t>{ 1 });
    CHECK(captured.filament_presets == before_mapping);
    CHECK(captured.physical_filament_config_indices == before_physical);
    CHECK(captured.collection_selected_name == bundle.filaments.get_selected_preset_name());

    const auto result = transaction->execute({ { "filament_density", "1.30" } }, false);

    CHECK(result.committed);
    CHECK(result.slot_index == 1);
    CHECK(result.target_preset_name == "G4 Filament Target");
    CHECK(result.applied.at("filament_density") == "1.3");
    CHECK(result.persisted.at("filament_density") == "1.3");
    CHECK_FALSE(result.selection_changed);
    CHECK_FALSE(result.mapping_changed);
    CHECK(bundle.filament_presets == before_mapping);
    CHECK(bundle.physical_filament_config_indices() == before_physical);
    CHECK(bundle.project_config.option<Slic3r::ConfigOptionBools>("filament_is_mixed")->values == before_mixed);
    CHECK(bundle.project_config.option<Slic3r::ConfigOptionInts>("filament_map")->values == before_map);
    CHECK(bundle.project_config.option<Slic3r::ConfigOptionInts>("filament_nozzle_map")->values == before_nozzle_map);
    CHECK(bundle.project_config.option<Slic3r::ConfigOptionInts>("filament_volume_map")->values == before_volume_map);
    CHECK(bundle.filament_presets[0] == "G4 Filament Slot Zero");
    CHECK(result.side_effects == std::vector<std::string>{
        "filament_preset_persisted", "filament_compatibility_recalculated" });

    Slic3r::DynamicPrintConfig file_values;
    std::map<std::string, std::string> key_values;
    std::string reason;
    file_values.load_from_json((root.path() / "g4-filament-target.json").string(),
                               Slic3r::ForwardCompatibilitySubstitutionRule::Disable, key_values, reason);
    CHECK(reason.empty());
    Slic3r::DynamicPrintConfig direct_readback = before_target;
    direct_readback.apply(std::move(file_values));
    CHECK(direct_readback.opt_serialize("filament_density") == "1.3");
}

TEST_CASE("Filament slot mutation rejects unsupported target identities", "[PluginHost][FilamentSlotMutation]")
{
    ScopedTemporaryDir root("orca-filament-slot-reject");
    {
        Slic3r::PresetBundle bundle = filament_bundle(root.path());
        CHECK_THROWS(Slic3r::FilamentSlotMutationTransaction::create_for_testing(bundle, 2));
    }
    {
        Slic3r::PresetBundle bundle = filament_bundle(root.path());
        bundle.project_config.option<Slic3r::ConfigOptionBools>("filament_is_mixed", true)->values[1] = true;
        CHECK_THROWS(Slic3r::FilamentSlotMutationTransaction::create_for_testing(bundle, 1));
    }
    {
        Slic3r::PresetBundle bundle = filament_bundle(root.path());
        bundle.filament_presets[0] = bundle.filament_presets[1];
        CHECK_THROWS(Slic3r::FilamentSlotMutationTransaction::create_for_testing(bundle, 1));
    }
    for (const auto target_kind : { 0, 1, 2, 3, 4 }) {
        Slic3r::PresetBundle bundle = filament_bundle(root.path());
        Slic3r::Preset* target = bundle.filaments.find_preset("G4 Filament Target", false, true);
        REQUIRE(target != nullptr);
        if (target_kind == 0) target->is_default = true;
        if (target_kind == 1) target->is_system = true;
        if (target_kind == 2) target->is_external = true;
        if (target_kind == 3) target->is_project_embedded = true;
        if (target_kind == 4) target->bundle_id = "bundle";
        CHECK_THROWS(Slic3r::FilamentSlotMutationTransaction::create_for_testing(bundle, 1));
    }
}

TEST_CASE("Filament slot mutation rejects below-minimum cost before persistence", "[PluginHost][FilamentSlotMutation]")
{
    ScopedTemporaryDir root("orca-filament-slot-invalid-cost");
    Slic3r::PresetBundle bundle = filament_bundle(root.path());
    const auto before_mapping = filament_mapping(bundle);
    const Slic3r::Preset* target_before = bundle.filaments.find_preset("G4 Filament Target", false, true);
    REQUIRE(target_before != nullptr);
    const Slic3r::DynamicPrintConfig before_target = target_before->config;
    auto transaction = Slic3r::FilamentSlotMutationTransaction::create_for_testing(bundle, 1);

    const auto result = transaction->execute({ { "filament_cost", "-1" } }, false);

    CHECK_FALSE(result.committed);
    CHECK(result.state == Slic3r::PersistentPresetMutationState::RolledBack);
    CHECK_FALSE(result.rollback_attempted);
    CHECK(result.rollback_verified);
    CHECK(result.error_code == "staged_config_validation_failed");
    CHECK(bundle.filament_presets == before_mapping);
    const Slic3r::Preset* target_after = bundle.filaments.find_preset("G4 Filament Target", false, true);
    REQUIRE(target_after != nullptr);
    CHECK(target_after->config.equals(before_target));
}

TEST_CASE("Filament slot mutation rejects stale mapping and target identity before writing", "[PluginHost][FilamentSlotMutation]")
{
    ScopedTemporaryDir root("orca-filament-slot-stale");
    {
        Slic3r::PresetBundle bundle = filament_bundle(root.path());
        auto transaction = Slic3r::FilamentSlotMutationTransaction::create_for_testing(bundle, 1);
        bundle.filament_presets[0] = "G4 Filament Target";
        const auto result = transaction->execute({ { "filament_density", "1.30" } }, false);
        CHECK_FALSE(result.committed);
        CHECK(result.error_code == "stale_slot_identity");
        CHECK(result.mapping_changed);
    }
    {
        Slic3r::PresetBundle bundle = filament_bundle(root.path());
        auto transaction = Slic3r::FilamentSlotMutationTransaction::create_for_testing(bundle, 1);
        Slic3r::Preset* target = bundle.filaments.find_preset("G4 Filament Target", false, true);
        REQUIRE(target != nullptr);
        target->is_external = true;
        const auto result = transaction->execute({ { "filament_density", "1.30" } }, false);
        CHECK_FALSE(result.committed);
        CHECK(result.error_code == "stale_slot_identity");
    }
    {
        Slic3r::PresetBundle bundle = filament_bundle(root.path());
        auto transaction = Slic3r::FilamentSlotMutationTransaction::create_for_testing(bundle, 1);
        Slic3r::Preset* target = bundle.filaments.find_preset("G4 Filament Target", false, true);
        REQUIRE(target != nullptr);
        target->name = "G4 Filament Target Renamed";
        const auto result = transaction->execute({ { "filament_density", "1.30" } }, false);
        CHECK_FALSE(result.committed);
        CHECK(result.error_code == "stale_slot_identity");
    }
    {
        Slic3r::PresetBundle bundle = filament_bundle(root.path());
        auto transaction = Slic3r::FilamentSlotMutationTransaction::create_for_testing(bundle, 1);
        CHECK(bundle.filaments.delete_preset("G4 Filament Target", true));
        const auto result = transaction->execute({ { "filament_density", "1.30" } }, false);
        CHECK_FALSE(result.committed);
        CHECK(result.error_code == "stale_slot_identity");
    }
}

TEST_CASE("Filament slot mutation preserves a different selected collection and rolls back safely", "[PluginHost][FilamentSlotMutation]")
{
    ScopedTemporaryDir root("orca-filament-slot-rollback");
    {
        Slic3r::PresetBundle bundle = filament_bundle(root.path());
        bundle.filaments.select_preset_by_name("G4 Filament Slot Zero", true);
        bundle.filaments.get_edited_preset().config.set_deserialize_strict("filament_cost", "77");
        bundle.filaments.update_dirty();
        auto transaction = Slic3r::FilamentSlotMutationTransaction::create_for_testing(bundle, 1);
        const auto result = transaction->execute({ { "filament_density", "1.30" } }, false);
        CHECK(result.committed);
        CHECK_FALSE(result.selection_changed);
        CHECK(bundle.filaments.get_selected_preset_name() == "G4 Filament Slot Zero");
        CHECK(bundle.filaments.get_edited_preset().config.opt_serialize("filament_cost") == "77");
        CHECK(result.dirty_before == result.dirty_after);
    }
    for (const auto hooks : { Slic3r::FilamentSlotMutationTransaction::TestingHooks{true, false, false},
                              Slic3r::FilamentSlotMutationTransaction::TestingHooks{false, true, false} }) {
        Slic3r::PresetBundle bundle = filament_bundle(root.path());
        bundle.filaments.select_preset_by_name("G4 Filament Slot Zero", true);
        bundle.filaments.get_edited_preset().config.set_deserialize_strict("filament_cost", "77");
        bundle.filaments.update_dirty();
        const auto before_mapping = filament_mapping(bundle);
        auto transaction = Slic3r::FilamentSlotMutationTransaction::create_for_testing(bundle, 1, hooks);
        const auto result = transaction->execute({ { "filament_density", "1.30" } }, false);
        CHECK_FALSE(result.committed);
        CHECK(result.rollback_attempted);
        CHECK(result.rollback_verified);
        CHECK_FALSE(result.mapping_changed);
        CHECK(bundle.filament_presets == before_mapping);
        CHECK(bundle.filaments.get_selected_preset_name() == "G4 Filament Slot Zero");
        CHECK(bundle.filaments.get_edited_preset().config.opt_serialize("filament_cost") == "77");
        CHECK(result.dirty_before == result.dirty_after);
    }

    Slic3r::PresetBundle bundle = filament_bundle(root.path());
    Slic3r::FilamentSlotMutationTransaction::TestingHooks hooks;
    hooks.force_save_failure = true;
    hooks.force_rollback_failure = true;
    auto transaction = Slic3r::FilamentSlotMutationTransaction::create_for_testing(bundle, 1, hooks);
    CHECK(transaction->execute({ { "filament_density", "1.30" } }, false).state ==
          Slic3r::PersistentPresetMutationState::FailedRecovery);
    CHECK_THROWS(transaction->execute({ { "filament_density", "1.31" } }, false));
}
