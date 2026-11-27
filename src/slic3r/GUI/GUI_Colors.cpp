#include "GUI_Colors.hpp"
#include "imgui/imgui.h"
#include "libvgcode/include/ColorRange.hpp"
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>


namespace Slic3r{

ImVec4 RenderColor::colors[RenderCol_Count] = { };

const char* GetRenderColName(RenderCol idx)
{
    switch (idx)
    {
    case RenderCol_3D_Background: return "3D Background";
    case RenderCol_Plate_Unselected: return "Plate Unselected";
    case RenderCol_Plate_Selected: return "Plate Selected";
    case RenderCol_Plate_Default: return "Plate Default";
    case RenderCol_Plate_Line_Top: return "Plate Line Top";
    case RenderCol_Plate_Line_Bottom: return "Plate Line Bottom";
    case RenderCol_Model_Disable: return "Model Disable";
    case RenderCol_Model_Unprintable: return "Model Unprintable";
    case RenderCol_Model_Neutral: return "Model Neutral";
    case RenderCol_Part: return "Part";
    case RenderCol_Modifier: return "Modifier";
    case RenderCol_Negtive_Volume: return "Negtive Volume";
    case RenderCol_Support_Enforcer: return "Support Enforcer";
    case RenderCol_Support_Blocker: return "Support Blocker";
    case RenderCol_Axis_X: return "Axis X";
    case RenderCol_Axis_Y: return "Axis Y";
    case RenderCol_Axis_Z: return "Axis Z";
    case RenderCol_Grabber_X: return "Grabber X";
    case RenderCol_Grabber_Y: return "Grabber Y";
    case RenderCol_Grabber_Z: return "Grabber Z";
    case RenderCol_Flatten_Plane: return "Flatten Plane";
    case RenderCol_Flatten_Plane_Hover: return "Flatten Plane Hover";
    }
    return "Unknown";
}

namespace GUI {

// ORCA: colors from "#RRGGBB"
static ColorRGBA rgba(const char* hex) { ColorRGBA color; decode_color(hex, color); return color; }
static ColorRGB  rgb(const char* hex)  { ColorRGB  color; decode_color(hex, color); return color; }
static libvgcode::Palette palette(std::initializer_list<const char*> hexes)
{
    libvgcode::Palette colors;
    for (const char* hex : hexes) {
        const ColorRGB color = rgb(hex);
        colors.push_back({ uint8_t(std::lround(color.r() * 255.f)), uint8_t(std::lround(color.g() * 255.f)), uint8_t(std::lround(color.b() * 255.f)) });
    }
    return colors;
}

// ORCA: every color that depends on the color set. To add a set, add it to UIColorSet, to the choices of the
// "ui_color_set" preference in PreferenceOps.cpp and here
static std::array<UIColors, size_t(UIColorSet::Count)> make_ui_colors()
{
    std::array<UIColors, size_t(UIColorSet::Count)> sets;

    // Default: the colors as they are without the preference
    UIColors& def = sets[size_t(UIColorSet::Default)];
    def.axes         = {{ rgb("#FF3C5B"), rgb("#64C818"), rgb("#2F88E9") }}; // what libslic3r/Color.hpp starts with
    def.enforcer     = rgba("#80FF80");
    def.blocker      = rgba("#FF8080");
    def.overhang     = rgb("#C70000");
    def.measure_pair = {{ rgba("#40BFBF"), rgba("#BF40BF") }};
    def.cut_pair     = {{ rgba("#00FFFF"), rgba("#FF00FF") }};
    def.ranges       = libvgcode::DEFAULT_RANGES_COLORS;

    // Deutan. Every other set starts as the default one and changes what it needs to.
    // Brushes: yellow green and brick red over a muted purple overhang marking. The model, the overhangs, the two
    // brushes and the two brushes over an overhang then all stay apart with protanopia and deuteranopia, which the
    // default green and red over a red overhang marking do not. The red is much darker than the green as the two
    // differ only in lightness with a red-green deficiency. The overhang color is weak in blue so that the green,
    // a yellow with these deficiencies, keeps its hue where the two overlap: against a strong blue it turns into
    // a grey like the model.
    // Pairs: blue and green, as yellow tones are taken by the grabber of the cut gizmo.
    // Ranges: no two of its stops become alike with protanopia, deuteranopia or tritanopia.
    UIColors& deutan = sets[size_t(UIColorSet::Deutan)];
    deutan              = def;
    deutan.axes         = {{ rgb("#FF5A87"), rgb("#B6FF00"), rgb("#0075FF") }};
    deutan.enforcer     = rgba("#85FF01");
    deutan.blocker      = rgba("#9C2B00");
    deutan.overhang     = rgb("#6A549D");
    deutan.measure_pair = {{ rgba("#0070D2"), rgba("#0FAD4E") }};
    deutan.cut_pair     = deutan.measure_pair;
    deutan.ranges       = palette({
        "#600059", // bluish
        "#4126CA",
        "#1281D5",
        "#0DB7D8",
        "#19E8FE",
        "#E7FFC9", // center
        "#FEF000",
        "#D9BE20",
        "#CC8600",
        "#DA4C5F",
        "#AA2E37"  // reddish
    });

    return sets;
}

static const std::array<UIColors, size_t(UIColorSet::Count)> UI_COLORS = make_ui_colors();

static UIColorSet s_ui_color_set = UIColorSet::Default;

UIColorSet      ui_color_set() { return s_ui_color_set; }
const UIColors& ui_colors()    { return UI_COLORS[size_t(s_ui_color_set)]; }

void set_ui_color_set(int idx)
{
    // a value that is not a set, as nothing stored is, stands for the default set
    s_ui_color_set = 0 <= idx && idx < int(UIColorSet::Count) ? UIColorSet(idx) : UIColorSet::Default;
    const std::array<ColorRGB, 3>& axes = ui_colors().axes;
    ColorRGB::set_axes(axes[0], axes[1], axes[2]);
}

} // namespace GUI

}
