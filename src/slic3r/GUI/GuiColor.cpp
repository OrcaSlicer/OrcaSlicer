#include "GuiColor.hpp"

namespace Slic3r { namespace GUI {
wxColour convert_to_wxColour(const RGBA &color)
{
    auto     r = std::clamp((int) (color[0] * 255.f), 0, 255);
    auto     g = std::clamp((int) (color[1] * 255.f), 0, 255);
    auto     b = std::clamp((int) (color[2] * 255.f), 0, 255);
    auto     a = std::clamp((int) (color[3] * 255.f), 0, 255);
    wxColour wx_color(r, g, b, a);
    return wx_color;
}

RGBA convert_to_rgba(const wxColour &color)
{
    RGBA rgba;
    rgba[0] = std::clamp(color.Red() / 255.f, 0.f, 1.f);
    rgba[1] = std::clamp(color.Green() / 255.f, 0.f, 1.f);
    rgba[2] = std::clamp(color.Blue() / 255.f, 0.f, 1.f);
    rgba[3] = std::clamp(color.Alpha() / 255.f, 0.f, 1.f);
    return rgba;
}

float calc_color_distance(wxColour c1, wxColour c2)
{
    float lab[2][3];
    RGB2Lab(c1.Red(), c1.Green(), c1.Blue(), &lab[0][0], &lab[0][1], &lab[0][2]);
    RGB2Lab(c2.Red(), c2.Green(), c2.Blue(), &lab[1][0], &lab[1][1], &lab[1][2]);

    return DeltaE76(lab[0][0], lab[0][1], lab[0][2], lab[1][0], lab[1][1], lab[1][2]);
}

float calc_color_distance(RGBA c1, RGBA c2)
{
    float lab[2][3];
    RGB2Lab(c1[0], c1[1], c1[2], &lab[0][0], &lab[0][1], &lab[0][2]);
    RGB2Lab(c2[0], c2[1], c2[2], &lab[1][0], &lab[1][1], &lab[1][2]);

    return DeltaE76(lab[0][0], lab[0][1], lab[0][2], lab[1][0], lab[1][1], lab[1][2]);
}

} }
