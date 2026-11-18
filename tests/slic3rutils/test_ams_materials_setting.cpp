#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "slic3r/GUI/AMSMaterialsSetting.hpp"
#include "slic3r/GUI/ColorPickerDialog.hpp"
#include "libslic3r/Color.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>
#include <wx/colour.h>

using namespace Slic3r;
using namespace Slic3r::GUI;
using ColorItem = ColorPickerPopup::ColorItem;

TEST_CASE("AMS custom solid colors preserve every RGBA byte with either firmware capability", "[AMSMaterialsSetting]")
{
    const bool allow_gradient = GENERATE(false, true);
    for (int byte = 0; byte < 256; ++byte) {
        const ColorItem original{{wxColour(byte, 255 - byte, byte ^ 0x55, byte)}, 2};
        const auto selection = original.picker_selection(allow_gradient);
        REQUIRE(selection);
        REQUIRE(std::holds_alternative<ColorRGBA>(*selection));
        const ColorItem result = ColorItem::from_picker_selection(*selection);
        REQUIRE(result.ctype == 2);
        REQUIRE(result.colors == original.colors);
    }
}

TEST_CASE("AMS custom gradients retain repeated nodes direction and independent alpha", "[AMSMaterialsSetting]")
{
    const std::vector<wxColour> colors{wxColour(255, 0, 0, 0), wxColour(0, 255, 0, 1),
                                      wxColour(0, 0, 255, 128), wxColour(255, 0, 0, 0),
                                      wxColour(255, 255, 0, 254), wxColour(0, 255, 255, 255),
                                      wxColour(128, 64, 32, 80)};
    const bool reverse = GENERATE(false, true);
    for (size_t count = 2; count <= std::min(colors.size(), ColorPickerPopup::MAX_EDITABLE_COLORS); ++count) {
        ColorItem original{{colors.begin(), colors.begin() + count}, 0};
        if (reverse)
            std::reverse(original.colors.begin(), original.colors.end());
        const auto selection = original.picker_selection(true);
        REQUIRE(selection);
        REQUIRE(std::holds_alternative<ColorGradient>(*selection));
        const ColorItem result = ColorItem::from_picker_selection(*selection);
        REQUIRE(result.ctype == 0);
        REQUIRE(result.colors == original.colors);
        REQUIRE_FALSE(filament_color_picker_changed(*selection, result.picker_selection(true)));
        REQUIRE_FALSE(filament_color_picker_changed(*selection, std::nullopt));
    }
}

TEST_CASE("AMS custom editing only accepts representable gradients on capable firmware", "[AMSMaterialsSetting]")
{
    const std::vector<wxColour> colors{wxColour(255, 0, 0, 0), wxColour(0, 0, 255, 128)};
    REQUIRE_FALSE(ColorItem{}.picker_selection(true));
    REQUIRE_FALSE(ColorItem{colors, 0}.picker_selection(false));
    REQUIRE_FALSE(ColorItem{colors, 1}.picker_selection(true));
    REQUIRE_FALSE(ColorItem{colors, 2}.picker_selection(true));
    REQUIRE_FALSE(ColorItem{std::vector<wxColour>(ColorPickerPopup::MAX_EDITABLE_COLORS + 1, colors.front()), 0}.picker_selection(true));
    REQUIRE(ColorItem{colors, 0}.picker_selection(true));
}

TEST_CASE("AMS editing rejects five through seven colors while project editing retains them", "[AMSMaterialsSetting]")
{
    const auto count = GENERATE(std::size_t{5}, std::size_t{6}, COLOR_PICKER_MAX_GRADIENT_COLORS);
    const ColorItem original{std::vector<wxColour>(count, wxColour(12, 34, 56, 78)), 0};
    REQUIRE_FALSE(original.picker_selection(true));
    const auto project_selection = filament_color_picker_selection({std::vector<std::string>(count, "#0C22384E"), true});
    REQUIRE(project_selection);
    REQUIRE(ColorItem::from_picker_selection(*project_selection).colors == original.colors);
}

TEST_CASE("AMS alpha only edits and switching a gradient to solid replace the complete selection", "[AMSMaterialsSetting]")
{
    const ColorItem original{{wxColour(255, 0, 0, 0), wxColour(0, 0, 255, 128)}, 0};
    const auto initial = original.picker_selection(true);
    REQUIRE(initial);
    const ColorItem edited{{wxColour(255, 0, 0, 1), wxColour(0, 0, 255, 129)}, 0};
    const auto selection = edited.picker_selection(true);
    REQUIRE(selection);
    REQUIRE(filament_color_picker_changed(*initial, selection));
    REQUIRE(ColorItem::from_picker_selection(*selection).colors == edited.colors);

    const ColorSelection solid = ColorRGBA(static_cast<unsigned char>(255), static_cast<unsigned char>(0),
                                           static_cast<unsigned char>(0), static_cast<unsigned char>(0));
    REQUIRE(filament_color_picker_changed(*initial, solid));
    const ColorItem result = ColorItem::from_picker_selection(solid);
    REQUIRE(result.ctype == 2);
    REQUIRE(result.colors == std::vector<wxColour>{original.colors.front()});
}
