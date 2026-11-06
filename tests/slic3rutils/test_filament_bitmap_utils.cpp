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

#include <string>
#include "libslic3r/Config.hpp"
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>

#include <catch2/catch_all.hpp>

#include <cmath>
#include <algorithm>

#include <wx/colour.h>
#ifdef __WXMSW__
#include <wx/bitmap.h>
#include <wx/brush.h>
#include <wx/image.h>
#include <wx/dc.h>
#include <wx/dcgraph.h>
#include <wx/dcmemory.h>
#include <wx/font.h>
#include <wx/gdicmn.h>
#include <wx/graphics.h>
#include <wx/menuitem.h>
#include <wx/pen.h>
#include <wx/string.h>
#include <cstdlib>
#include <utility>
#include <catch2/generators/catch_generators.hpp>
#endif
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

// The headless GTK/macOS suite does not initialize GUI display/font stock objects.
// Exercise native bitmap pixels on Windows without adding that dependency elsewhere.
#ifdef __WXMSW__
TEST_CASE("Rounded custom swatches display every gradient stop in order", "[FilamentBitmapUtils][AMSMaterialsSetting]")
{
    const int count = GENERATE(3, 7);
    const bool reverse = GENERATE(false, true);
    std::vector<wxColour> colors = count == 3 ? std::vector<wxColour>{wxColour(255, 0, 0), wxColour(0, 255, 0), wxColour(0, 0, 255)}
                                             : std::vector<wxColour>{wxColour(255, 0, 0), wxColour(255, 128, 0), wxColour(255, 255, 0),
                                                                     wxColour(0, 255, 0), wxColour(0, 255, 255), wxColour(0, 0, 255), wxColour(255, 0, 255)};
    if (reverse)
        std::reverse(colors.begin(), colors.end());
    const wxColour background(40, 40, 40);
    const wxRect rect(2, 2, 120, 25);
    wxBitmap bitmap(124, 29, 24);
    {
        wxMemoryDC dc(bitmap);
        dc.SetBackground(wxBrush(background));
        dc.Clear();
        Slic3r::GUI::draw_rounded_filament_swatch(dc, rect, colors, true, 7);
    }
    const wxImage image = bitmap.ConvertToImage();
    REQUIRE(image.IsOk());
    // Allow two raster pixels of native gradient alignment error within one color interval.
    const int tolerance = (2 * 255 * (count - 1) + rect.width - 1) / rect.width;
    for (int i = 0; i < count; ++i) {
        const int x = std::clamp(rect.x + rect.width * i / (count - 1), rect.x + 1, rect.GetRight());
        CAPTURE(count, reverse, i, x);
        REQUIRE(std::abs(int(image.GetRed(x, 14)) - colors[i].Red()) <= tolerance);
        REQUIRE(std::abs(int(image.GetGreen(x, 14)) - colors[i].Green()) <= tolerance);
        REQUIRE(std::abs(int(image.GetBlue(x, 14)) - colors[i].Blue()) <= tolerance);
    }
    require_same_rgb(wxColour(image.GetRed(rect.x, rect.y), image.GetGreen(rect.x, rect.y), image.GetBlue(rect.x, rect.y)), background);
}

TEST_CASE("Rounded custom swatches retain visible stops after a transparent first color", "[FilamentBitmapUtils][AMSMaterialsSetting]")
{
    const wxColour background(40, 40, 40);
    const std::vector<wxColour> colors{wxColour(255, 0, 0, 0), wxColour(0, 255, 0, 128), wxColour(0, 0, 255, 255)};
    wxBitmap bitmap(124, 29, 24);
    {
        wxMemoryDC dc(bitmap);
        dc.SetBackground(wxBrush(background));
        dc.Clear();
        Slic3r::GUI::draw_rounded_filament_swatch(dc, wxRect(2, 2, 120, 25), colors, true, 7);
    }
    const wxImage image = bitmap.ConvertToImage();
    REQUIRE(image.IsOk());
    for (const auto& sample : {std::pair<int, wxColour>{3, wxColour(255, 255, 255)}, {62, wxColour(127, 255, 127)}, {121, wxColour(0, 0, 255)}}) {
        REQUIRE(std::abs(int(image.GetRed(sample.first, 14)) - sample.second.Red()) <= 8);
        REQUIRE(std::abs(int(image.GetGreen(sample.first, 14)) - sample.second.Green()) <= 8);
        REQUIRE(std::abs(int(image.GetBlue(sample.first, 14)) - sample.second.Blue()) <= 8);
    }
    require_same_rgb(wxColour(image.GetRed(2, 2), image.GetGreen(2, 2), image.GetBlue(2, 2)), background);
}

TEST_CASE("Rounded single colors composite alpha over checkerboard cells without painting the corners", "[FilamentBitmapUtils][AMSMaterialsSetting]")
{
    const auto alpha = GENERATE(0, 128, 254, 255);
    const int scale = GENERATE(1, 2);
    const wxColour background(40, 40, 40);
    const wxRect rect(0, 0, 120 * scale, 25 * scale);
    wxBitmap bitmap(rect.width, rect.height, 24);
    {
        wxMemoryDC dc(bitmap);
        dc.SetBackground(wxBrush(background));
        dc.Clear();
        Slic3r::GUI::draw_rounded_filament_swatch(dc, rect, {wxColour(0, 0, 255, alpha)}, false, 7 * scale);
    }
    const wxImage image = bitmap.ConvertToImage();
    REQUIRE(image.IsOk());
    const int y = 12 * scale;
    const int first = image.GetRed(22 * scale, y);
    const int second = image.GetRed(27 * scale, y);
    const int dark = 217 * (255 - alpha) / 255;
    const int light = 255 - alpha;
    REQUIRE(std::abs(std::min(first, second) - dark) <= 1);
    REQUIRE(std::abs(std::max(first, second) - light) <= 1);
    REQUIRE(image.GetGreen(22 * scale, y) == first);
    REQUIRE(image.GetGreen(27 * scale, y) == second);
    REQUIRE(std::abs(int(image.GetBlue(22 * scale, y)) - (first + alpha)) <= 1);
    REQUIRE(std::abs(int(image.GetBlue(27 * scale, y)) - (second + alpha)) <= 1);
    require_same_rgb(wxColour(image.GetRed(0, 0), image.GetGreen(0, 0), image.GetBlue(0, 0)), background);
}

TEST_CASE("Nonopaque interior colors reveal checkerboard beneath opaque endpoints", "[FilamentBitmapUtils][AMSMaterialsSetting]")
{
    const auto alpha = GENERATE(0, 128);
    const bool gradient = GENERATE(false, true);
    const std::vector<wxColour> colors{wxColour(255, 0, 0), wxColour(0, 0, 255, alpha), wxColour(0, 255, 0)};
    wxBitmap bitmap(120, 25, 24);
    {
        wxMemoryDC dc(bitmap);
        dc.SetBackground(*wxBLACK_BRUSH);
        dc.Clear();
        Slic3r::GUI::draw_rounded_filament_swatch(dc, wxRect(0, 0, 120, 25), colors, gradient, 7);
    }
    const wxImage image = bitmap.ConvertToImage();
    REQUIRE(image.IsOk());
    const int first = image.GetRed(60, 12);
    const int second = image.GetRed(60, 17);
    REQUIRE(std::abs(std::min(first, second) - 217 * (255 - alpha) / 255) <= 8);
    REQUIRE(std::abs(std::max(first, second) - (255 - alpha)) <= 8);
    REQUIRE(std::abs(first - second) >= (alpha == 0 ? 30 : 12));
    REQUIRE(image.GetRed(1, 12) >= 247);
    REQUIRE(image.GetGreen(119, 12) >= 247);
}

TEST_CASE("Rounded custom swatches keep segmented colors distinct from gradients", "[FilamentBitmapUtils][AMSMaterialsSetting]")
{
    const std::vector<wxColour> colors{wxColour(255, 0, 0), wxColour(0, 255, 0), wxColour(0, 0, 255)};
    wxBitmap bitmap(120, 25, 24);
    {
        wxMemoryDC dc(bitmap);
        dc.SetBackground(*wxWHITE_BRUSH);
        dc.Clear();
        Slic3r::GUI::draw_rounded_filament_swatch(dc, wxRect(0, 0, 120, 25), colors, false, 7);
    }
    const wxImage image = bitmap.ConvertToImage();
    REQUIRE(image.IsOk());
    for (int i = 0; i < 3; ++i) {
        const int x = 20 + 40 * i;
        require_same_rgb(wxColour(image.GetRed(x, 12), image.GetGreen(x, 12), image.GetBlue(x, 12)), colors[i]);
    }
}

TEST_CASE("Circular AMS gradients reveal interior alpha and leave their surrounding corners clear", "[FilamentBitmapUtils][AMSMaterialsSetting]")
{
    const int alpha = GENERATE(0, 128, 255);
    const int scale = GENERATE(1, 2);
    const bool selected = GENERATE(false, true);
    const wxSize size(25 * scale, 25 * scale);
    const int radius = size.x / 2 - scale - (selected ? scale : 0);
    const wxRect rect(size.x / 2 - radius, size.y / 2 - radius, 2 * radius, 2 * radius);
    const wxColour background(40, 40, 40);
    const std::vector<wxColour> colors{wxColour(255, 0, 0), wxColour(0, 0, 255, alpha), wxColour(0, 255, 0)};
    wxBitmap bitmap(size.x, size.y, 24);
    {
        wxMemoryDC dc(bitmap);
        dc.SetBackground(wxBrush(background));
        dc.Clear();
        Slic3r::GUI::draw_rounded_filament_swatch(dc, rect, colors, true, radius);
    }
    const wxImage image = bitmap.ConvertToImage();
    REQUIRE(image.IsOk());
    const int x = size.x / 2, y = size.y / 2;
    const int cell = std::max(1, rect.height / 5);
    const int contrast = std::abs(int(image.GetRed(x, y)) - int(image.GetRed(x, y + cell)));
    if (alpha == 255)
        REQUIRE(contrast == 0);
    else
        REQUIRE(contrast >= (alpha == 0 ? 24 : 12));
    REQUIRE(image.GetBlue(x, y) >= image.GetRed(x, y));
    for (const wxPoint& point : {wxPoint(0, 0), wxPoint(size.x - 1, 0), wxPoint(0, size.y - 1), wxPoint(size.x - 1, size.y - 1)})
        require_same_rgb(wxColour(image.GetRed(point.x, point.y), image.GetGreen(point.x, point.y), image.GetBlue(point.x, point.y)), background);
}

TEST_CASE("AMS card gradient checkerboards respect rounded corners and the remaining material region", "[FilamentBitmapUtils][AMSItem]")
{
    const int alpha = GENERATE(0, 128, 255);
    const int scale = GENERATE(1, 2);
    const int remaining = GENERATE(35, 100);
    const wxSize size(52 * scale, 80 * scale);
    const int height = size.y * remaining / 100;
    const bool external = GENERATE(false, true);
    // AMSLib uses the same geometry for gradients as its single-color card fill.
    const wxRect rect = external ? wxRect(scale, scale, size.x - 2 * scale, size.y - scale) :
                                  wxRect(2 * scale, scale + size.y - height, size.x - 4 * scale, height);
    const wxColour background(40, 40, 40);
    const std::vector<wxColour> colors{wxColour(255, 0, 0), wxColour(0, 0, 255, alpha), wxColour(0, 255, 0)};
    wxBitmap bitmap(size.x, size.y, 24);
    {
        wxMemoryDC dc(bitmap);
        dc.SetBackground(wxBrush(background));
        dc.Clear();
        Slic3r::GUI::draw_rounded_filament_swatch(dc, rect, colors, true, 3);
    }
    const wxImage image = bitmap.ConvertToImage();
    REQUIRE(image.IsOk());
    require_same_rgb(wxColour(image.GetRed(rect.x, rect.y), image.GetGreen(rect.x, rect.y), image.GetBlue(rect.x, rect.y)), background);
    require_same_rgb(wxColour(image.GetRed(rect.GetRight(), rect.y), image.GetGreen(rect.GetRight(), rect.y), image.GetBlue(rect.GetRight(), rect.y)), background);
    const int cell = std::max(1, rect.height / 5);
    const int x = size.x / 2, y = rect.y + cell + 1;
    const int contrast = std::abs(int(image.GetRed(x, y)) - int(image.GetRed(x, y + cell)));
    if (alpha == 255)
        REQUIRE(contrast == 0);
    else
        REQUIRE(contrast >= (alpha == 0 ? 24 : 12));
    if (rect.y > 0)
        require_same_rgb(wxColour(image.GetRed(x, rect.y - 1), image.GetGreen(x, rect.y - 1), image.GetBlue(x, rect.y - 1)), background);
    require_same_rgb(wxColour(image.GetRed(0, y), image.GetGreen(0, y), image.GetBlue(0, y)), background);
    require_same_rgb(wxColour(image.GetRed(size.x - 1, y), image.GetGreen(size.x - 1, y), image.GetBlue(size.x - 1, y)), background);
}

TEST_CASE("AMS solid and gradient transparency use identical checkerboard cells and outlines", "[FilamentBitmapUtils][AMSItem]")
{
    const int alpha = GENERATE(0, 128, 254);
    const int count = GENERATE(2, 4);
    const int scale = GENERATE(1, 2);
    const double radius = GENERATE(0.0, 3.0);
    const int remaining = GENERATE(35, 100);
    const wxSize size(52 * scale, 80 * scale);
    const int height = (size.y - 2 * scale) * remaining / 100;
    const wxRect rect(2 * scale, size.y - scale - height, size.x - 4 * scale, height);
    CAPTURE(alpha, count, scale, radius, remaining);
    const wxColour background(40, 40, 40);
    const wxColour solid(80, 160, 240, alpha);
    std::vector<wxColour> gradient(count, solid);
    if (alpha == 0) {
        // Fully transparent endpoints must reveal the same grid regardless of their RGB values.
        gradient.front() = wxColour(255, 0, 0, 0);
        gradient.back() = wxColour(0, 255, 0, 0);
    }
    const auto render = [&](const std::vector<wxColour>& colors, bool is_gradient) {
        wxBitmap bitmap(size.x, size.y, 24);
        {
            wxMemoryDC dc(bitmap);
            dc.SetBackground(wxBrush(background));
            dc.Clear();
            Slic3r::GUI::draw_rounded_filament_swatch(dc, rect, colors, is_gradient, radius);
        }
        return bitmap.ConvertToImage();
    };
    const wxImage single = render({solid}, false);
    const wxImage multiple = render(gradient, true);
    REQUIRE(single.IsOk());
    REQUIRE(multiple.IsOk());
    REQUIRE(std::equal(single.GetData(), single.GetData() + size.x * size.y * 3, multiple.GetData()));
}

TEST_CASE("Ordered gradient fills preserve middle nodes and cover narrow swatches", "[FilamentBitmapUtils][AMSMaterialsSetting]")
{
    const int width = GENERATE(1, 2, 3, 7, 13, 17);
    const bool reverse = GENERATE(false, true);
    std::vector<wxColour> colors{wxColour(255, 0, 0), wxColour(0, 255, 0), wxColour(0, 0, 255)};
    if (reverse)
        std::reverse(colors.begin(), colors.end());
    wxBitmap bitmap(width + 4, 5, 24);
    {
        wxMemoryDC dc(bitmap);
        Slic3r::GUI::fill_gradient_rect_east(dc, wxRect(2, 1, width, 3), colors);
    }
    const wxImage image = bitmap.ConvertToImage();
    REQUIRE(image.IsOk());
    auto color_at = [&image](int x) { return wxColour(image.GetRed(x, 2), image.GetGreen(x, 2), image.GetBlue(x, 2)); };
    require_same_rgb(color_at(2), colors.front());
    require_same_rgb(color_at(width + 1), width == 1 ? colors.front() : colors.back());
    if (width >= 3)
        require_same_rgb(color_at(2 + width / 2), colors[1]);
}

TEST_CASE("Ordered gradients composite independent node alpha over the existing background", "[FilamentBitmapUtils][AMSMaterialsSetting]")
{
    const bool graphics_dc = GENERATE(false, true);
    const std::vector<wxColour> colors{wxColour(255, 0, 0, 0), wxColour(0, 255, 0, 128), wxColour(0, 0, 255, 255)};
    wxBitmap bitmap(9, 3, 24);
    {
        wxMemoryDC memory(bitmap);
        memory.SetBackground(*wxWHITE_BRUSH);
        memory.Clear();
        if (graphics_dc) {
            wxGCDC dc(memory);
            Slic3r::GUI::fill_gradient_rect_east(dc, wxRect(0, 0, 9, 3), colors);
        } else {
            Slic3r::GUI::fill_gradient_rect_east(memory, wxRect(0, 0, 9, 3), colors);
        }
    }
    const wxImage image = bitmap.ConvertToImage();
    REQUIRE(image.IsOk());
    require_same_rgb(wxColour(image.GetRed(0, 1), image.GetGreen(0, 1), image.GetBlue(0, 1)), wxColour(255, 255, 255));
    REQUIRE(std::abs(int(image.GetRed(4, 1)) - 127) <= 1);
    REQUIRE(image.GetGreen(4, 1) == 255);
    REQUIRE(std::abs(int(image.GetBlue(4, 1)) - 127) <= 1);
    require_same_rgb(wxColour(image.GetRed(8, 1), image.GetGreen(8, 1), image.GetBlue(8, 1)), wxColour(0, 0, 255));
}

TEST_CASE("Filament gradient pixels follow both endpoint directions without changing input", "[FilamentBitmapUtils]")
{
    const bool reverse = GENERATE(false, true);
    const std::vector<wxColour> colors = reverse ? std::vector<wxColour>{wxColour(0, 0, 255), wxColour(255, 0, 0)}
                                                 : std::vector<wxColour>{wxColour(255, 0, 0), wxColour(0, 0, 255)};
    const auto original = colors;
    const wxBitmap bitmap = Slic3r::GUI::create_filament_bitmap(colors, wxSize(64, 16), true);
    REQUIRE(bitmap.IsOk());
    const wxImage image = bitmap.ConvertToImage();
    REQUIRE(image.IsOk());
    // Interior pixels avoid backend-specific rasterization at the gradient boundary.
    for (const auto& sample : {std::pair<int, wxColour>{2, colors.front()}, {61, colors.back()}}) {
        REQUIRE(std::abs(int(image.GetRed(sample.first, 8)) - int(sample.second.Red())) <= 16);
        REQUIRE(std::abs(int(image.GetBlue(sample.first, 8)) - int(sample.second.Blue())) <= 16);
    }
    REQUIRE(colors == original);
}

TEST_CASE("Filament halves preserve the same caller color order as gradients", "[FilamentBitmapUtils]")
{
    const bool reverse = GENERATE(false, true);
    const std::vector<wxColour> colors = reverse ? std::vector<wxColour>{wxColour(0, 0, 255), wxColour(255, 0, 0)}
                                                 : std::vector<wxColour>{wxColour(255, 0, 0), wxColour(0, 0, 255)};
    const auto original = colors;
    const wxBitmap bitmap = Slic3r::GUI::create_filament_bitmap(colors, wxSize(64, 16), false);
    REQUIRE(bitmap.IsOk());
    const wxImage image = bitmap.ConvertToImage();
    REQUIRE(image.IsOk());
    require_same_rgb(wxColour(image.GetRed(16, 8), image.GetGreen(16, 8), image.GetBlue(16, 8)), colors.front());
    require_same_rgb(wxColour(image.GetRed(48, 8), image.GetGreen(48, 8), image.GetBlue(48, 8)), colors.back());
    REQUIRE(colors == original);
}

TEST_CASE("Three color filament stripes retain caller order", "[FilamentBitmapUtils]")
{
    const std::vector<wxColour> colors{wxColour(0, 0, 255), wxColour(0, 255, 0), wxColour(255, 0, 0)};
    const auto original = colors;
    const wxBitmap bitmap = Slic3r::GUI::create_filament_bitmap(colors, wxSize(72, 24), false);
    REQUIRE(bitmap.IsOk());
    const wxImage image = bitmap.ConvertToImage();
    REQUIRE(image.IsOk());
    for (int i = 0; i < 3; ++i) {
        const int x = 12 + i * 24;
        require_same_rgb(wxColour(image.GetRed(x, 12), image.GetGreen(x, 12), image.GetBlue(x, 12)), colors[i]);
    }
    REQUIRE(colors == original);
}

TEST_CASE("Four color filament quadrants retain caller order", "[FilamentBitmapUtils]")
{
    const std::vector<wxColour> colors{wxColour(0, 0, 255), wxColour(0, 255, 0), wxColour(255, 0, 0), wxColour(255, 255, 0)};
    const auto original = colors;
    const wxBitmap bitmap = Slic3r::GUI::create_filament_bitmap(colors, wxSize(64, 32), false);
    REQUIRE(bitmap.IsOk());
    const wxImage image = bitmap.ConvertToImage();
    REQUIRE(image.IsOk());
    for (int i = 0; i < 4; ++i) {
        const int x = i % 2 ? 48 : 16, y = i / 2 ? 24 : 8;
        require_same_rgb(wxColour(image.GetRed(x, y), image.GetGreen(x, y), image.GetBlue(x, y)), colors[i]);
    }
    REQUIRE(colors == original);
}

TEST_CASE("Filament single color icons composite every RGBA channel over both checker shades", "[FilamentBitmapUtils]")
{
    const int alpha = GENERATE(0, 1, 128, 254);
    const int width = GENERATE(20, 44);
    const int scale = GENERATE(1, 2);
    const wxColour color(wxString::Format("#204080%02X", alpha));
    REQUIRE(color.IsOk());
    REQUIRE(int(color.Alpha()) == alpha);
    const wxBitmap bitmap = Slic3r::GUI::create_filament_bitmap({color}, wxSize(width * scale, 20 * scale));
    REQUIRE(bitmap.IsOk());
    REQUIRE_FALSE(bitmap.HasAlpha());
    const wxImage image = bitmap.ConvertToImage();
    REQUIRE(image.IsOk());
    REQUIRE_FALSE(image.HasAlpha());
    const int y = 10 * scale;
    for (const auto& channels : {std::vector<int>{32, image.GetRed(6 * scale, y), image.GetRed(10 * scale, y)},
                                 std::vector<int>{64, image.GetGreen(6 * scale, y), image.GetGreen(10 * scale, y)},
                                 std::vector<int>{128, image.GetBlue(6 * scale, y), image.GetBlue(10 * scale, y)}}) {
        const auto composite = [&](int background) { return (channels[0] * alpha + background * (255 - alpha) + 127) / 255; };
        REQUIRE(std::abs(std::min(channels[1], channels[2]) - composite(217)) <= 1);
        REQUIRE(std::abs(std::max(channels[1], channels[2]) - composite(255)) <= 1);
    }
}

TEST_CASE("Filament gradient icons reveal interior alpha while retaining opaque endpoints", "[FilamentBitmapUtils]")
{
    const int alpha = GENERATE(0, 128);
    const bool reverse = GENERATE(false, true);
    std::vector<wxColour> colors{wxColour(255, 0, 0), wxColour(0, 0, 255, alpha), wxColour(0, 255, 0)};
    if (reverse)
        std::reverse(colors.begin(), colors.end());
    const wxBitmap bitmap = Slic3r::GUI::create_filament_bitmap(colors, wxSize(121, 20), true);
    REQUIRE(bitmap.IsOk());
    REQUIRE_FALSE(bitmap.HasAlpha());
    const wxImage image = bitmap.ConvertToImage();
    REQUIRE(image.IsOk());
    REQUIRE_FALSE(image.HasAlpha());
    const int first = image.GetRed(60, 6), second = image.GetRed(60, 10);
    REQUIRE(std::abs(std::min(first, second) - 217 * (255 - alpha) / 255) <= 1);
    REQUIRE(std::abs(std::max(first, second) - (255 - alpha)) <= 1);
    REQUIRE(std::abs(int(image.GetBlue(60, 6)) - (first + alpha)) <= 1);
    require_same_rgb(wxColour(image.GetRed(0, 10), image.GetGreen(0, 10), image.GetBlue(0, 10)), colors.front());
    require_same_rgb(wxColour(image.GetRed(120, 10), image.GetGreen(120, 10), image.GetBlue(120, 10)), colors.back());
}

TEST_CASE("Filament segmented icons preserve stripes and quadrants over a continuous checkerboard", "[FilamentBitmapUtils]")
{
    const int count = GENERATE(2, 3, 4);
    const int scale = GENERATE(1, 2);
    const std::vector<wxColour> all{wxColour(255, 0, 0), wxColour(0, 0, 255, 128),
                                   wxColour(0, 255, 0, 0), wxColour(255, 255, 0)};
    const std::vector<wxColour> colors(all.begin(), all.begin() + count);
    const wxBitmap bitmap = Slic3r::GUI::create_filament_bitmap(colors, wxSize(44 * scale, 20 * scale), false);
    REQUIRE(bitmap.IsOk());
    REQUIRE_FALSE(bitmap.HasAlpha());
    const wxImage image = bitmap.ConvertToImage();
    REQUIRE(image.IsOk());
    REQUIRE_FALSE(image.HasAlpha());
    for (int i = 0; i < count; ++i) {
        const int x = (count == 4 ? 22 * (i % 2) + 5 : (44 / count) * i + 5) * scale;
        const int y = (count == 4 ? 10 * (i / 2) + 5 : 10) * scale;
        CAPTURE(count, scale, i);
        for (const auto& channels : {std::vector<int>{colors[i].Red(), image.GetRed(x, y), image.GetRed(x + 4 * scale, y)},
                                     std::vector<int>{colors[i].Green(), image.GetGreen(x, y), image.GetGreen(x + 4 * scale, y)},
                                     std::vector<int>{colors[i].Blue(), image.GetBlue(x, y), image.GetBlue(x + 4 * scale, y)}}) {
            const int opacity = colors[i].Alpha();
            const auto composite = [&](int background) { return (channels[0] * opacity + background * (255 - opacity) + 127) / 255; };
            REQUIRE(std::abs(std::min(channels[1], channels[2]) - composite(217)) <= 1);
            REQUIRE(std::abs(std::max(channels[1], channels[2]) - composite(255)) <= 1);
        }
    }
}

TEST_CASE("Vertical filament ramps composite alpha over the same checkerboard as other icons", "[FilamentBitmapUtils]")
{
    const std::vector<wxColour> ramp{wxColour(255, 0, 0), wxColour(0, 0, 255, 128), wxColour(0, 255, 0, 0)};
    const wxBitmap bitmap = Slic3r::GUI::create_gradient_ramp_bitmap(ramp, wxSize(44, 21));
    REQUIRE(bitmap.IsOk());
    REQUIRE_FALSE(bitmap.HasAlpha());
    const wxImage image = bitmap.ConvertToImage();
    REQUIRE(image.IsOk());
    REQUIRE_FALSE(image.HasAlpha());
    REQUIRE(std::min(image.GetRed(6, 0), image.GetRed(10, 0)) == 217);
    REQUIRE(std::max(image.GetRed(6, 0), image.GetRed(10, 0)) == 255);
    REQUIRE(std::abs(int(std::min(image.GetRed(6, 10), image.GetRed(10, 10))) - 108) <= 1);
    REQUIRE(std::abs(int(std::max(image.GetRed(6, 10), image.GetRed(10, 10))) - 127) <= 1);
    require_same_rgb(wxColour(image.GetRed(6, 20), image.GetGreen(6, 20), image.GetBlue(6, 20)), ramp.front());
}

TEST_CASE("Rounded swatches match the main AMS renderer for opaque gradients and bands", "[FilamentBitmapUtils][AMSItem]")
{
    const int count = GENERATE(2, 3, 7);
    const bool gradient = GENERATE(false, true);
    const bool graphics_dc = GENERATE(false, true);
    const int radius = GENERATE(0, 7, 12);
    const std::vector<wxColour> all{wxColour(255, 0, 0), wxColour(255, 128, 0), wxColour(255, 255, 0),
                                   wxColour(0, 255, 0), wxColour(0, 255, 255), wxColour(0, 0, 255), wxColour(255, 0, 255)};
    const std::vector<wxColour> colors(all.begin(), all.begin() + count);
    const wxRect rect(2, 2, 96, 24);
    wxBitmap actual(100, 28, 24), reference(100, 28, 24);
    {
        wxMemoryDC dc(actual);
        dc.SetBackground(*wxBLACK_BRUSH);
        dc.Clear();
        if (graphics_dc) {
            wxGCDC graphics(dc);
            Slic3r::GUI::draw_rounded_filament_swatch(graphics, rect, colors, gradient, radius);
        } else {
            Slic3r::GUI::draw_rounded_filament_swatch(dc, rect, colors, gradient, radius);
        }
    }
    {
        wxMemoryDC memory(reference);
        memory.SetBackground(*wxBLACK_BRUSH);
        memory.Clear();
        wxGCDC dc(memory);
        wxGraphicsContext* gc = dc.GetGraphicsContext();
        REQUIRE(gc);
        // Baseline: AMSPreview::doRender's primary drawing path from main at d4fabbf7c8.
        wxGraphicsGradientStops stops(colors.front(), colors.back());
        if (gradient) {
            for (size_t i = 1; i + 1 < colors.size(); ++i)
                stops.Add(colors[i], static_cast<float>(i) / (colors.size() - 1));
        } else {
            for (size_t i = 1; i < colors.size(); ++i) {
                const float position = static_cast<float>(i) / colors.size();
                stops.Add(colors[i - 1], position);
                stops.Add(colors[i], position);
            }
        }
        gc->PushState();
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(gc->CreateLinearGradientBrush(rect.x, rect.y, rect.x + rect.width, rect.y, stops));
        gc->DrawRoundedRectangle(rect.x, rect.y, rect.width, rect.height, radius);
        gc->PopState();
    }
    const wxImage image = actual.ConvertToImage(), expected = reference.ConvertToImage();
    REQUIRE(image.IsOk());
    REQUIRE(expected.IsOk());
    REQUIRE(std::equal(image.GetData(), image.GetData() + image.GetWidth() * image.GetHeight() * 3, expected.GetData()));
}

TEST_CASE("Windows menu gradient labels retain opaque pixels after native text drawing", "[FilamentBitmapUtils][Regression]")
{
    const int alpha = GENERATE(0, 128, 255);
    const double scale = GENERATE(1.0, 1.5, 2.0);
    const wxString label = GENERATE("3", "10");
    const int size = static_cast<int>(20 * scale);
    wxBitmap bitmap = Slic3r::GUI::create_filament_bitmap({wxColour(0, 0, 255, alpha), wxColour(255, 0, 0, alpha)},
                                                         wxSize(size, size), true);
    REQUIRE(bitmap.IsOk());
    bitmap.SetScaleFactor(scale);
    {
        wxMemoryDC dc(bitmap);
        dc.SetFont(wxFont(wxFontInfo(wxSize(0, static_cast<int>(14 * scale))).Family(wxFONTFAMILY_SWISS)));
        dc.SetBackgroundMode(wxTRANSPARENT);
        const wxSize text = dc.GetTextExtent(label);
        const int x = (size - text.x) / 2, y = (size - text.y) / 2;
        dc.SetTextForeground(wxColour("#262E30"));
        dc.DrawText(label, x - 1, y);
        dc.DrawText(label, x + 1, y);
        dc.DrawText(label, x, y - 1);
        dc.DrawText(label, x, y + 1);
        dc.SetTextForeground(*wxWHITE);
        dc.DrawText(label, x, y);
    }
    wxMenuItem item(nullptr, wxID_ANY, "Filament");
    item.SetBitmap(bitmap);
    const wxImage image = item.GetBitmap().ConvertToImage();
    REQUIRE(image.IsOk());
    REQUIRE(image.HasAlpha());
    std::size_t nonopaque = 0, foreground = 0;
    for (int y = 0; y < image.GetHeight(); ++y) {
        for (int x = 0; x < image.GetWidth(); ++x) {
            if (image.GetAlpha(x, y) != 255)
                ++nonopaque;
            if (image.GetRed(x, y) > 230 && image.GetGreen(x, y) > 230 && image.GetBlue(x, y) > 230)
                ++foreground;
        }
    }
    CAPTURE(alpha, scale, label, bitmap.GetDepth(), bitmap.HasAlpha());
    REQUIRE(nonopaque == 0);
    REQUIRE(foreground > 0);
}

#endif // __WXMSW__
