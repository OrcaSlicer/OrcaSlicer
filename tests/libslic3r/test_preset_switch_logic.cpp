#include "slic3r/GUI/PresetSwitchLogic.hpp"

#include <catch2/catch_all.hpp>

namespace Slic3r::GUI {

TEST_CASE("Preset selection followup uses printer-specific flow for printer presets", "[PresetSwitchLogic]")
{
    REQUIRE(preset_selection_followup(Preset::TYPE_PRINTER, true, false) == PresetSelectionFollowup::RunPrinterSelectionFlow);
}

TEST_CASE("Preset selection followup uses generic tab selection for compatible non-printer presets", "[PresetSwitchLogic]")
{
    REQUIRE(preset_selection_followup(Preset::TYPE_PRINT, true, false) == PresetSelectionFollowup::RunGenericTabSelection);
    REQUIRE(preset_selection_followup(Preset::TYPE_FILAMENT, true, false) == PresetSelectionFollowup::RunGenericTabSelection);
}

TEST_CASE("Preset selection followup uses combo-only update for multifilament filament changes", "[PresetSwitchLogic]")
{
    REQUIRE(preset_selection_followup(Preset::TYPE_FILAMENT, true, true) == PresetSelectionFollowup::UpdateComboOnly);
    REQUIRE(preset_selection_followup(Preset::TYPE_PRINT, true, true) == PresetSelectionFollowup::RunGenericTabSelection);
}

TEST_CASE("Preset selection followup returns none when no tab selection should happen", "[PresetSwitchLogic]")
{
    REQUIRE(preset_selection_followup(Preset::TYPE_PRINT, false, false) == PresetSelectionFollowup::None);
    REQUIRE(preset_selection_followup(Preset::TYPE_PRINTER, false, false) == PresetSelectionFollowup::None);
}

TEST_CASE("Transfer preset origin describes project snapshots and stored preset kinds", "[PresetSwitchLogic]")
{
    Preset system_preset(Preset::TYPE_PRINT, "System process");
    system_preset.is_system = true;

    Preset default_preset(Preset::TYPE_PRINT, "Default process");
    default_preset.is_default = true;

    Preset user_preset(Preset::TYPE_PRINT, "User process");

    Preset bundle_preset(Preset::TYPE_PRINT, "Bundle process");
    bundle_preset.bundle_id = "bundle";

    Preset project_preset(Preset::TYPE_PRINT, "Project process");
    project_preset.is_project_embedded = true;

    REQUIRE(transfer_preset_origin(&system_preset, false, false) == TransferPresetOrigin::System);
    REQUIRE(transfer_preset_origin(&default_preset, false, false) == TransferPresetOrigin::Default);
    REQUIRE(transfer_preset_origin(&user_preset, false, false) == TransferPresetOrigin::User);
    REQUIRE(transfer_preset_origin(&bundle_preset, false, false) == TransferPresetOrigin::Bundle);
    REQUIRE(transfer_preset_origin(&project_preset, false, false) == TransferPresetOrigin::ProjectFile);
    REQUIRE(transfer_preset_origin(&system_preset, true, false) == TransferPresetOrigin::System);
    REQUIRE(transfer_preset_origin(&system_preset, true, true) == TransferPresetOrigin::ProjectFile);
    REQUIRE(transfer_preset_origin(nullptr, true, false) == TransferPresetOrigin::ProjectFile);
    REQUIRE(transfer_preset_origin(nullptr, false, false) == TransferPresetOrigin::Unknown);
}

TEST_CASE("Transfer preset identity comparison ignores the standard modified prefix", "[PresetSwitchLogic]")
{
    const std::string modified_name = Preset::suffix_modified() + std::string("Process profile");

    REQUIRE(transfer_preset_identities_equal(modified_name, "Process profile"));
    REQUIRE_FALSE(transfer_preset_identities_equal("Process profile", "Other profile"));
    REQUIRE_FALSE(transfer_preset_identities_equal("", "Process profile"));
}

} // namespace Slic3r::GUI
