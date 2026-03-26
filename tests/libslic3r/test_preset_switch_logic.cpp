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

} // namespace Slic3r::GUI