#ifndef slic3r_GUI_Colors_hpp_
#define slic3r_GUI_Colors_hpp_

#include "imgui/imgui.h"
#include "libslic3r/Color.hpp"
#include "libvgcode/include/Types.hpp"
#include <array>

enum RenderCol_ {
    RenderCol_3D_Background = 0,
    RenderCol_Plate_Unselected,
    RenderCol_Plate_Selected,
    RenderCol_Plate_Default,
    RenderCol_Plate_Line_Top,
    RenderCol_Plate_Line_Bottom,
    RenderCol_Model_Disable,
    RenderCol_Model_Unprintable,
    RenderCol_Model_Neutral,
    RenderCol_Part,
    RenderCol_Modifier,
    RenderCol_Negtive_Volume,
    RenderCol_Support_Enforcer,
    RenderCol_Support_Blocker,
    RenderCol_Axis_X,
    RenderCol_Axis_Y,
    RenderCol_Axis_Z,
    RenderCol_Grabber_X,
    RenderCol_Grabber_Y,
    RenderCol_Grabber_Z,
    RenderCol_Flatten_Plane,
    RenderCol_Flatten_Plane_Hover,
    RenderCol_Count,
};

typedef int RenderCol;

namespace Slic3r {

class RenderColor {
public:
    static ImVec4      colors[RenderCol_Count];
};
const char* GetRenderColName(RenderCol idx);

namespace GUI {

// ORCA: color sets of the "ui_color_set" preference, stored by index. Deutan covers deuteranopia and protanopia
enum class UIColorSet : unsigned char { Default, Deutan, Count };

// ORCA: colors that depend on the color set, defined by make_ui_colors() in GUI_Colors.cpp
struct UIColors
{
    std::array<ColorRGB, 3>  axes;         // X, Y, Z
    ColorRGBA                enforcer;     // positive brush
    ColorRGBA                blocker;      // negative brush
    ColorRGBA                erase_cursor; // painting cursor while erasing
    ColorRGB                 overhang;     // overhang highlight
    std::array<ColorRGBA, 2> measure_pair; // 1st and 2nd selection of measure and assembly
    std::array<ColorRGBA, 2> cut_pair;     // upper and lower part of cut
    libvgcode::Palette       ranges;       // color scales of the sliced preview
};

// ORCA: the color set in use and its colors
UIColorSet      ui_color_set();
const UIColors& ui_colors();
// ORCA: `idx` is the value of the preference. Call GLCanvas3D::update_ui_colors(), which also refreshes kept copies
void            set_ui_color_set(int idx);

} // namespace GUI

}

#endif
