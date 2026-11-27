#include "GUI_Colors.hpp"
#include "imgui/imgui.h"
#include "libvgcode/include/ColorRange.hpp"
#include <libvgcode/include/Types.hpp>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <libslic3r/Color.hpp>

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

// ORCA: colors of every set. To add a set: UIColorSet, the "ui_color_set" choices in PreferenceOps.cpp, and here
static std::array<UIColors, size_t(UIColorSet::Count)> make_ui_colors()
{
    std::array<UIColors, size_t(UIColorSet::Count)> sets;

    // DEFAULT: the colors as they are without the preference
    /////////////////////////////////////////////////////////
    UIColors& def = sets[size_t(UIColorSet::Default)];
    def.axes         = {{ // as ColorRGB::X(), Y() and Z() of libslic3r/Color.hpp
        rgba("#FF3C5B"), // X
        rgba("#64C818"), // Y
        rgba("#2F88E9")  // Z
    }}; 
    def.enforcer     = rgba("#80FF80");
    def.blocker      = rgba("#FF8080");
    def.erase_cursor = rgba("#00009940");
    def.overhang     = rgb("#C70000");
    def.measure_pair = {{
        rgba("#40BFBF"), // selected_1st_color
        rgba("#BF40BF")  // selected_2nd_color
    }};
    def.cut_pair     = {{
        rgba("#00FFFF"), // Upper
        rgba("#FF00FF")  // Lower
    }};
    def.ranges       = libvgcode::DEFAULT_RANGES_COLORS;

    // DEUTAN: starts as the default set and changes what it needs to
    /////////////////////////////////////////////////////////
    // Brushes and overhang: stay apart with protanopia and deuteranopia, also where a brush covers an overhang
    // Pairs: blue and green, as yellow is taken by the grabber of the cut gizmo
    // Ranges: no two stops look alike with protanopia, deuteranopia or tritanopia
    UIColors& deutan = sets[size_t(UIColorSet::Deutan)];
    deutan              = def;
    deutan.axes         = {{
        rgba("#FF5A87"), // X
        rgba("#B6FF00"), // Y
        rgba("#0075FF")  // Z
    }};
    deutan.enforcer     = rgba("#85FF01");
    deutan.blocker      = rgba("#9C2B00");
    deutan.overhang     = rgb("#6A549D");
    deutan.measure_pair = {{
        rgba("#0070D2"), // selected_1st_color
        rgba("#0FAD4E")  // selected_2nd_color
    }};
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

// ORCA: the names the preference stores the sets with, in the order of UIColorSet
static const std::array<const char*, size_t(UIColorSet::Count)> UI_COLOR_SET_NAMES = {{ "default", "deutan" }};

void set_ui_color_set(const std::string& name)
{
    // unknown names, and nothing stored, mean the default set
    s_ui_color_set = UIColorSet::Default;
    for (size_t i = 0; i < UI_COLOR_SET_NAMES.size(); ++i) {
        if (name == UI_COLOR_SET_NAMES[i])
            s_ui_color_set = UIColorSet(i);
    }
}

} // namespace GUI

}
