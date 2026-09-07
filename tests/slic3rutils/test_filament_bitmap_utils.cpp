// recompute_mixed_slot_colors lives in libslic3r_gui; this is the only suite that links it.
// Same Windows include prologue as test_dev_mapping.cpp (wx pulls in <windows.h>; keep
// WIN32_LEAN_AND_MEAN / NOMINMAX ahead of the Catch2 headers).
#ifdef WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
#endif

#include <catch2/catch_all.hpp>

#include <cmath>

#include <wx/colour.h>
#include <wx/string.h>

#include "libslic3r/FilamentMixer.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/FilamentBitmapUtils.hpp"

using namespace Slic3r;
using Slic3r::GUI::recompute_mixed_slot_colors;

namespace {

// Two physical slots (1 = red, 2 = blue) and mixed slot 3 built from them.
DynamicPrintConfig mixed_config(const std::string& components = "1,2", const std::string& ratios = "0.5,0.5")
{
    DynamicPrintConfig cfg;
    cfg.set_key_value("filament_is_mixed", new ConfigOptionBools({false, false, true}));
    cfg.set_key_value("filament_mixed_components", new ConfigOptionStrings({"", "", components}));
    cfg.set_key_value("filament_mixed_sublayer_ratios", new ConfigOptionStrings({"", "", ratios}));
    cfg.set_key_value("filament_mixed_gradient", new ConfigOptionBools({false, false, false}));
    cfg.set_key_value("filament_colour", new ConfigOptionStrings({"#FF0000", "#0000FF", "#000000"}));
    return cfg;
}

wxColour expected_blend(const std::vector<std::string>& hex, const std::vector<int>& weights)
{
    return wxColour(wxString(blend_color_multi(hex, weights)));
}

// Compare channels one at a time so a failure names the channel.
void require_same_rgb(const wxColour& actual, const wxColour& expected)
{
    REQUIRE(int(actual.Red()) == int(expected.Red()));
    REQUIRE(int(actual.Green()) == int(expected.Green()));
    REQUIRE(int(actual.Blue()) == int(expected.Blue()));
}

} // namespace

TEST_CASE("recompute_mixed_slot_colors blends a mixed slot from its components' colours", "[FilamentBitmapUtils]")
{
    std::vector<wxColour> colors{wxColour(255, 0, 0), wxColour(0, 0, 255)};
    recompute_mixed_slot_colors(colors, mixed_config());

    REQUIRE(colors.size() == 3);
    require_same_rgb(colors[2], expected_blend({"#FF0000", "#0000FF"}, {5000, 5000}));
    REQUIRE(int(colors[2].Alpha()) == 255);
    // Physical slots are left alone.
    require_same_rgb(colors[0], wxColour(255, 0, 0));
    require_same_rgb(colors[1], wxColour(0, 0, 255));
}

TEST_CASE("recompute_mixed_slot_colors leaves the colours alone without mixed slots", "[FilamentBitmapUtils]")
{
    std::vector<wxColour> colors{wxColour(255, 0, 0), wxColour(0, 0, 255)};

    SECTION("no mixed keys at all") {
        recompute_mixed_slot_colors(colors, DynamicPrintConfig{});
    }
    SECTION("mixed flags present but all false") {
        DynamicPrintConfig cfg;
        cfg.set_key_value("filament_is_mixed", new ConfigOptionBools({false, false}));
        cfg.set_key_value("filament_mixed_components", new ConfigOptionStrings({"", ""}));
        recompute_mixed_slot_colors(colors, cfg);
    }
    REQUIRE(colors.size() == 2);
    require_same_rgb(colors[0], wxColour(255, 0, 0));
    require_same_rgb(colors[1], wxColour(0, 0, 255));
}

TEST_CASE("recompute_mixed_slot_colors falls back to grey for a broken component reference", "[FilamentBitmapUtils]")
{
    const wxColour grey(128, 128, 128, 255);
    std::vector<wxColour> colors{wxColour(255, 0, 0), wxColour(0, 0, 255)};

    SECTION("dangling component id") {
        recompute_mixed_slot_colors(colors, mixed_config("1,9"));
    }
    SECTION("empty component list") {
        recompute_mixed_slot_colors(colors, mixed_config(""));
    }
    REQUIRE(colors.size() == 3);
    require_same_rgb(colors[2], grey);
}

TEST_CASE("recompute_mixed_slot_colors uses the project colour when a slot colour is unset", "[FilamentBitmapUtils]")
{
    // Slot 2 carries no colour in the vector; filament_colour[1] = "#0000FF" is used instead.
    std::vector<wxColour> colors{wxColour(255, 0, 0), wxColour()};
    recompute_mixed_slot_colors(colors, mixed_config());
    require_same_rgb(colors[2], expected_blend({"#FF0000", "#0000FF"}, {5000, 5000}));
}

TEST_CASE("recompute_mixed_slot_colors blends a gradient slot from its end points only", "[FilamentBitmapUtils]")
{
    DynamicPrintConfig cfg;
    cfg.set_key_value("filament_is_mixed", new ConfigOptionBools({false, false, false, true}));
    cfg.set_key_value("filament_mixed_components", new ConfigOptionStrings({"", "", "", "1,2,3"}));
    cfg.set_key_value("filament_mixed_sublayer_ratios", new ConfigOptionStrings({"", "", "", "0.2,0.3,0.5"}));
    cfg.set_key_value("filament_mixed_gradient", new ConfigOptionBools({false, false, false, true}));
    cfg.set_key_value("filament_colour", new ConfigOptionStrings({"#FF0000", "#00FF00", "#0000FF", "#000000"}));

    std::vector<wxColour> colors{wxColour(255, 0, 0), wxColour(0, 255, 0), wxColour(0, 0, 255)};
    recompute_mixed_slot_colors(colors, cfg);

    REQUIRE(colors.size() == 4);
    require_same_rgb(colors[3], expected_blend({"#FF0000", "#0000FF"}, {5000, 5000}));
}

TEST_CASE("recompute_mixed_slot_colors honours the configured ratios and is idempotent", "[FilamentBitmapUtils]")
{
    std::vector<wxColour> colors{wxColour(255, 0, 0), wxColour(0, 0, 255)};
    const DynamicPrintConfig cfg = mixed_config("1,2", "0.7,0.3");
    recompute_mixed_slot_colors(colors, cfg);
    const wxColour first = colors[2];
    // The configured 70/30 ratio must reach the blend (it is not the equal-share default).
    require_same_rgb(first, expected_blend({"#FF0000", "#0000FF"}, {7000, 3000}));
    REQUIRE(first != expected_blend({"#FF0000", "#0000FF"}, {5000, 5000}));
    recompute_mixed_slot_colors(colors, cfg);
    require_same_rgb(colors[2], first);
}

// --- mixed_gradient_ramp / sample_gradient_ramp -----------------------------------------
//
// The ramp is what every mixed filament swatch is drawn from, so these pin the three things
// a plain fade between two endpoint colours cannot express: the reserved ratio band, the
// component order, and the custom curve.

namespace {

// Slot 3 (index 2) is a gradient mix of physical slots 1 (red) and 2 (blue).
DynamicPrintConfig gradient_config(const std::string& components = "1,2",
                                   const std::string& range      = "0.9,0.1",
                                   const std::string& curve      = "")
{
    DynamicPrintConfig cfg;
    cfg.set_key_value("filament_is_mixed", new ConfigOptionBools({false, false, true}));
    cfg.set_key_value("filament_mixed_components", new ConfigOptionStrings({"", "", components}));
    cfg.set_key_value("filament_mixed_gradient", new ConfigOptionBools({false, false, true}));
    cfg.set_key_value("filament_mixed_gradient_range", new ConfigOptionStrings({"", "", range}));
    cfg.set_key_value("filament_mixed_gradient_curve", new ConfigOptionStrings({"", "", curve}));
    cfg.set_key_value("filament_colour", new ConfigOptionStrings({"#FF0000", "#0000FF", "#000000"}));
    return cfg;
}

} // namespace

TEST_CASE("mixed_gradient_ramp runs bottom to top and never reaches a pure component", "[FilamentBitmapUtils]")
{
    // range "0.9,0.1": component 1 (red) is the majority at the bottom and the minority at the top.
    const auto ramp = Slic3r::GUI::mixed_gradient_ramp(gradient_config(), 2, 16);
    REQUIRE(ramp.size() == 16);

    // Neither end is the pure component colour - the slicer clamps the blend to
    // [kGradientMinRatio, kGradientMaxRatio], which a fade between the pure colours would ignore.
    REQUIRE(ramp.front() != wxColour(255, 0, 0));
    REQUIRE(ramp.back() != wxColour(0, 0, 255));

    // Red falls and blue rises monotonically from bottom to top.
    for (size_t i = 1; i < ramp.size(); ++i) {
        REQUIRE(int(ramp[i].Red()) <= int(ramp[i - 1].Red()));
        REQUIRE(int(ramp[i].Blue()) >= int(ramp[i - 1].Blue()));
    }
}

TEST_CASE("mixed_gradient_ramp follows the range's direction rather than the component order", "[FilamentBitmapUtils]")
{
    const auto rising  = Slic3r::GUI::mixed_gradient_ramp(gradient_config("1,2", "0.1,0.9"), 2, 16);
    const auto falling = Slic3r::GUI::mixed_gradient_ramp(gradient_config("1,2", "0.9,0.1"), 2, 16);
    REQUIRE(rising.size() == 16);
    REQUIRE(falling.size() == 16);

    // "0.1,0.9" starts blue-heavy at the bottom; "0.9,0.1" starts red-heavy. Reversing the
    // range must reverse the ramp, which endpoint colours ordered by HSV cannot express.
    REQUIRE(int(rising.front().Blue()) > int(rising.front().Red()));
    REQUIRE(int(falling.front().Red()) > int(falling.front().Blue()));
    require_same_rgb(rising.front(), falling.back());
}

TEST_CASE("mixed_gradient_ramp bends with a custom curve", "[FilamentBitmapUtils]")
{
    // Component 1 holds near its maximum for the first half, then drops - a shape a straight
    // fade between two endpoints cannot draw.
    const auto curved = Slic3r::GUI::mixed_gradient_ramp(
        gradient_config("1,2", "0.9,0.1", "0,0.9|0.5,0.85|1,0.1"), 2, 16);
    const auto linear = Slic3r::GUI::mixed_gradient_ramp(gradient_config("1,2", "0.9,0.1"), 2, 16);
    REQUIRE(curved.size() == 16);

    // The curve holds component 1 high through the lower half, so every band up to mid height
    // is at least as red as the straight fade and mid height is strictly redder.
    for (size_t i = 0; i <= curved.size() / 2; ++i)
        REQUIRE(int(curved[i].Red()) >= int(linear[i].Red()));
    REQUIRE(int(curved[curved.size() / 2].Red()) > int(linear[linear.size() / 2].Red()));
    // It still ends blue-dominant, like the straight fade.
    REQUIRE(int(curved.back().Blue()) > int(curved.back().Red()));
}

TEST_CASE("mixed_gradient_ramp is empty for anything but a two-component gradient slot", "[FilamentBitmapUtils]")
{
    SECTION("slot is not mixed") {
        REQUIRE(Slic3r::GUI::mixed_gradient_ramp(gradient_config(), 0, 16).empty());
    }
    SECTION("gradient is off") {
        DynamicPrintConfig cfg = gradient_config();
        cfg.set_key_value("filament_mixed_gradient", new ConfigOptionBools({false, false, false}));
        REQUIRE(Slic3r::GUI::mixed_gradient_ramp(cfg, 2, 16).empty());
    }
    SECTION("three components") {
        REQUIRE(Slic3r::GUI::mixed_gradient_ramp(gradient_config("1,2,3"), 2, 16).empty());
    }
    SECTION("slot out of range") {
        REQUIRE(Slic3r::GUI::mixed_gradient_ramp(gradient_config(), 9, 16).empty());
    }
    SECTION("no mixed keys at all") {
        REQUIRE(Slic3r::GUI::mixed_gradient_ramp(DynamicPrintConfig{}, 0, 16).empty());
    }
}

TEST_CASE("sample_gradient_ramp blends each step through the shared blender", "[FilamentBitmapUtils]")
{
    // A flat curve makes every step the same 30/70 mix, which must come out as the blend the
    // dialog's own swatches are drawn with - not a channel lerp between the two components.
    GradientCurve curve;
    curve.points = {{0.0, 0.3, NAN, NAN}, {1.0, 0.3, NAN, NAN}};
    const auto ramp = Slic3r::GUI::sample_gradient_ramp(wxColour(255, 0, 0), wxColour(0, 0, 255), curve, 4);
    REQUIRE(ramp.size() == 4);

    const wxColour expected = Slic3r::GUI::blend_n_colors({wxColour(255, 0, 0), wxColour(0, 0, 255)}, {0.3, 0.7});
    for (const wxColour& c : ramp)
        require_same_rgb(c, expected);
}

TEST_CASE("sample_gradient_ramp returns nothing without a usable curve or step count", "[FilamentBitmapUtils]")
{
    GradientCurve curve;
    REQUIRE(Slic3r::GUI::sample_gradient_ramp(wxColour(255, 0, 0), wxColour(0, 0, 255), curve, 8).empty());
    curve.points = {{0.0, kGradientMaxRatio, NAN, NAN}, {1.0, kGradientMinRatio, NAN, NAN}};
    REQUIRE(Slic3r::GUI::sample_gradient_ramp(wxColour(255, 0, 0), wxColour(0, 0, 255), curve, 0).empty());
}
