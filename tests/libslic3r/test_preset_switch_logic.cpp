#include "libslic3r/PresetSwitchLogic.hpp"

#include <catch2/catch_all.hpp>

namespace Slic3r {

TEST_CASE("Generic plater preset selection skips printer follow-up selection", "[PresetSwitchLogic]")
{
    REQUIRE_FALSE(should_run_generic_preset_selection(Preset::TYPE_PRINTER, true, false));
}

TEST_CASE("Generic plater preset selection still runs for other presets when appropriate", "[PresetSwitchLogic]")
{
    REQUIRE(should_run_generic_preset_selection(Preset::TYPE_PRINT, true, false));
    REQUIRE(should_run_generic_preset_selection(Preset::TYPE_FILAMENT, true, false));
    REQUIRE_FALSE(should_run_generic_preset_selection(Preset::TYPE_FILAMENT, true, true));
    REQUIRE_FALSE(should_run_generic_preset_selection(Preset::TYPE_PRINT, false, false));
}

TEST_CASE("Generic plater preset selection suppresses multifilament follow-up selection only for filament presets", "[PresetSwitchLogic]")
{
    REQUIRE_FALSE(should_run_generic_preset_selection(Preset::TYPE_FILAMENT, true, true));
    REQUIRE(should_run_generic_preset_selection(Preset::TYPE_PRINT, true, true));
    REQUIRE_FALSE(should_run_generic_preset_selection(Preset::TYPE_PRINTER, true, true));
}

} // namespace Slic3r