#include "slic3r/GUI/ColorPickerDialog.hpp"
#include "libslic3r/Color.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <optional>
#include <string>
#include <vector>
#include <cstddef>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace Slic3r;
using namespace Slic3r::GUI;

TEST_CASE("Picker callers can lower the gradient limit without changing the shared default", "[FilamentColorPicker]")
{
    const auto limit = GENERATE(std::size_t{2}, std::size_t{4}, COLOR_PICKER_MAX_GRADIENT_COLORS);
    const ColorPickerOptions options{true, true, limit};
    REQUIRE(options.gradient_color_limit() == limit);
    REQUIRE_FALSE(options.supports_gradient(1));
    REQUIRE(options.supports_gradient(2));
    REQUIRE(options.supports_gradient(limit));
    REQUIRE_FALSE(options.supports_gradient(limit + 1));
    REQUIRE_FALSE(ColorPickerOptions{false, true, limit}.supports_gradient(2));
    REQUIRE(ColorPickerOptions{true, true}.supports_gradient(COLOR_PICKER_MAX_GRADIENT_COLORS));
}

TEST_CASE("Picker gradient limits stay within supported storage bounds", "[FilamentColorPicker]")
{
    const auto requested = GENERATE(std::size_t{0}, std::size_t{1}, COLOR_PICKER_MAX_GRADIENT_COLORS + 1);
    const ColorPickerOptions options{true, true, requested};
    REQUIRE(options.gradient_color_limit() == (requested < 2 ? 2 : COLOR_PICKER_MAX_GRADIENT_COLORS));
    REQUIRE(options.supports_gradient(2));
    REQUIRE_FALSE(options.supports_gradient(COLOR_PICKER_MAX_GRADIENT_COLORS + 1));
}

// One color more than an ordered gradient may hold.
static std::vector<std::string> too_many_gradient_colors()
{
    return std::vector<std::string>(COLOR_PICKER_MAX_GRADIENT_COLORS + 1, "#FF0000");
}

TEST_CASE("Project gradient direction survives editing and RGB serialization", "[FilamentColorPicker]")
{
    const std::vector<std::string> endpoints = {"#FF0000", "#0000FF"};
    const FilamentColorPickerValue stored{endpoints, true};
    const auto initial = filament_color_picker_selection(stored);
    REQUIRE(initial);
    REQUIRE(filament_color_picker_result(*initial).colors == endpoints);
    REQUIRE_FALSE(filament_color_picker_changed(*initial, initial));
    REQUIRE_FALSE(filament_color_picker_changed(*initial, std::nullopt));
    const auto reverse = filament_color_picker_selection({{endpoints[1], endpoints[0]}, true});
    REQUIRE(filament_color_picker_changed(*initial, reverse));
    REQUIRE(filament_color_picker_result(*reverse).colors.front() == endpoints.back());
}

TEST_CASE("Unrepresentable project colors remain unchanged until a different selection is confirmed", "[FilamentColorPicker]")
{
    const auto value = GENERATE(FilamentColorPickerValue{too_many_gradient_colors(), true},
                                FilamentColorPickerValue{{"#FF0000", "#0000FF"}, false});
    REQUIRE_FALSE(filament_color_picker_selection(value));
    const auto seed = filament_color_picker_initial(value);
    REQUIRE_FALSE(filament_color_picker_changed(seed, seed));
    REQUIRE_FALSE(filament_color_picker_changed(seed, std::nullopt));
    const auto changed = filament_color_picker_selection({{"#FE0000"}, false});
    REQUIRE(filament_color_picker_changed(seed, changed));
    REQUIRE(filament_color_picker_result(*changed).colors == std::vector<std::string>{"#FE0000"});
}

TEST_CASE("Filament seeds preserve primary alpha when colors are invalid or absent", "[FilamentColorPicker]")
{
    const FilamentColorPickerValue stale{{"invalid", "#00FF00"}, false, "#12345680"};
    REQUIRE_FALSE(filament_color_picker_selection(stale));
    REQUIRE(filament_color_picker_result(filament_color_picker_initial(stale)).colors == std::vector<std::string>{"#12345680"});
    const auto value = filament_color_picker_selection({{"#12345680", "#ABCDEF01"}, true});
    REQUIRE(value);
    REQUIRE(filament_color_picker_result(*value).colors == std::vector<std::string>{"#12345680", "#ABCDEF01"});
    const auto missing = filament_color_picker_initial({});
    REQUIRE(filament_color_picker_result(missing).colors == std::vector<std::string>{"#000000"});
}

TEST_CASE("Filament project colors round trip every alpha boundary with compatible opaque spelling", "[FilamentColorPicker]")
{
    const auto hex = GENERATE(std::string("#12345600"), std::string("#12345601"),
                              std::string("#12345680"), std::string("#123456FE"),
                              std::string("#123456FF"), std::string("#123456"));
    const FilamentColorPickerValue stored{{hex}, false};
    const auto selection = filament_color_picker_selection(stored);
    REQUIRE(selection);
    REQUIRE_FALSE(filament_color_picker_changed(*selection, selection));
    REQUIRE_FALSE(filament_color_picker_changed(*selection, std::nullopt));
    const auto result = filament_color_picker_result(*selection);
    const std::string expected = hex == "#123456FF" ? "#123456" : hex;
    REQUIRE(result.colors == std::vector<std::string>{expected});
    ColorRGBA decoded;
    REQUIRE(decode_color(result.colors.front(), decoded));
    REQUIRE(filament_color_picker_result(ColorSelection{decoded}).colors == result.colors);
    const auto reloaded = filament_color_picker_selection(result);
    REQUIRE(reloaded);
    REQUIRE(filament_color_picker_result(*reloaded).colors == result.colors);
}

TEST_CASE("Alpha only filament edits preserve ordered independent gradient endpoint alpha", "[FilamentColorPicker]")
{
    const auto original = filament_color_picker_selection({{"#FF000000", "#0000FF80"}, true});
    const auto edited = filament_color_picker_selection({{"#FF000001", "#0000FFFE"}, true});
    REQUIRE(original);
    REQUIRE(edited);
    REQUIRE(filament_color_picker_changed(*original, edited));
    const auto result = filament_color_picker_result(*edited);
    REQUIRE(result.gradient);
    REQUIRE(result.colors == std::vector<std::string>{"#FF000001", "#0000FFFE"});
    const auto reloaded = filament_color_picker_selection(result);
    REQUIRE(reloaded);
    REQUIRE_FALSE(filament_color_picker_changed(*edited, reloaded));
    const auto reversed = filament_color_picker_selection({{result.colors[1], result.colors[0]}, true});
    REQUIRE(filament_color_picker_changed(*edited, reversed));
    const auto solid = filament_color_picker_selection({{"#12345680"}, false});
    const auto alpha_only = filament_color_picker_selection({{"#12345681"}, false});
    REQUIRE(solid);
    REQUIRE(alpha_only);
    REQUIRE(filament_color_picker_changed(*solid, alpha_only));
    REQUIRE(filament_color_picker_result(*alpha_only).colors == std::vector<std::string>{"#12345681"});
    const auto mixed = filament_color_picker_selection({{"#FF0000FF", "#0000FF00"}, true});
    REQUIRE(mixed);
    REQUIRE(filament_color_picker_result(*mixed).colors == std::vector<std::string>{"#FF0000", "#0000FF00"});
}

TEST_CASE("Unrepresentable filament seeds retain first endpoint or valid primary alpha", "[FilamentColorPicker]")
{
    auto too_many = too_many_gradient_colors();
    too_many.front() = "#FF000000";
    const auto value = GENERATE_COPY(FilamentColorPickerValue{too_many, true},
                                FilamentColorPickerValue{{"#FF000000", "#0000FF80"}, false},
                                FilamentColorPickerValue{{"invalid", "#FF0000"}, true, "#FF000000"});
    REQUIRE_FALSE(filament_color_picker_selection(value));
    const auto initial = filament_color_picker_initial(value);
    REQUIRE(filament_color_picker_result(initial).colors == std::vector<std::string>{"#FF000000"});
    REQUIRE_FALSE(filament_color_picker_changed(initial, initial));
    REQUIRE_FALSE(filament_color_picker_changed(initial, std::nullopt));
    const auto edited = filament_color_picker_selection({{"#FF000001"}, false});
    REQUIRE(filament_color_picker_changed(initial, edited));
}

TEST_CASE("Ordered multi-color gradients round trip up to the color limit", "[FilamentColorPicker]")
{
    const std::vector<std::string> all = {"#FF0000", "#FFFF0080", "#00FF00", "#00FFFF01", "#0000FF", "#FF00FF00", "#123456"};
    for (std::size_t count = 2; count <= std::min(all.size(), COLOR_PICKER_MAX_GRADIENT_COLORS); ++count) {
        const std::vector<std::string> colors(all.begin(), all.begin() + count);
        const auto selection = filament_color_picker_selection({colors, true});
        REQUIRE(selection);
        const auto result = filament_color_picker_result(*selection);
        REQUIRE(result.gradient);
        REQUIRE(result.colors == colors);
        REQUIRE_FALSE(filament_color_picker_changed(*selection, filament_color_picker_selection(result)));
        std::vector<std::string> reordered = colors;
        std::swap(reordered.front(), reordered.back());
        REQUIRE(filament_color_picker_changed(*selection, filament_color_picker_selection({reordered, true})));
    }
    REQUIRE_FALSE(filament_color_picker_selection({too_many_gradient_colors(), true}));
}

TEST_CASE("Filament selections require complete hexadecimal colors and exact endpoint counts", "[FilamentColorPicker]")
{
    const std::string hex = GENERATE("", "112233", "#ABC", "#1234567", "#123456789", "#GG2233", "#11223Z", "#112233G0", " #112233", "#112233 ");
    REQUIRE_FALSE(filament_color_picker_selection({{hex}, false}));
    REQUIRE_FALSE(filament_color_picker_selection({{"#112233", hex}, true}));
    const auto malformed = GENERATE(FilamentColorPickerValue{{}, false},
                                   FilamentColorPickerValue{{}, true},
                                   FilamentColorPickerValue{{"#112233"}, true});
    REQUIRE_FALSE(filament_color_picker_selection(malformed));
}

TEST_CASE("Every RGBA byte survives the public filament conversion round trip", "[FilamentColorPicker]")
{
    constexpr char digits[] = "0123456789ABCDEF";
    for (int byte = 0; byte < 256; ++byte) {
        std::string hex = "#";
        for (int channel = 0; channel < 4; ++channel) {
            hex += digits[byte >> 4];
            hex += digits[byte & 15];
        }
        const auto selection = filament_color_picker_selection({{hex}, false});
        REQUIRE(selection);
        const auto result = filament_color_picker_result(*selection);
        REQUIRE(result.colors == std::vector<std::string>{byte == 255 ? hex.substr(0, 7) : hex});
        REQUIRE_FALSE(filament_color_picker_changed(*selection, filament_color_picker_selection(result)));
    }
}

TEST_CASE("Filament serialization rejects invalid native RGBA channels", "[FilamentColorPicker]")
{
    const float invalid = GENERATE(-0.1f, 1.1f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN());
    const int channel = GENERATE(0, 1, 2, 3);
    ColorRGBA color;
    color[channel] = invalid;
    REQUIRE_THROWS_AS(filament_color_picker_result(color), std::invalid_argument);
    REQUIRE_THROWS_AS(filament_color_picker_result(ColorGradient{ColorRGBA::WHITE(), color}), std::invalid_argument);
}
