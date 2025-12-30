#ifndef slic3r_GUI_Color_hpp_
#define slic3r_GUI_Color_hpp_
#include <wx/colour.h>
#include "libslic3r/Color.hpp"
#include "slic3r/Utils/ColorSpaceConvert.hpp"

struct ColorDistValue
{
    int   id;
    float distance;
};

namespace Slic3r { namespace GUI {
wxColour convert_to_wxColour(const RGBA &color);
RGBA     convert_to_rgba(const wxColour &color);
float    calc_color_distance(wxColour c1, wxColour c2);
float    calc_color_distance(RGBA c1, RGBA c2);
} // namespace GUI
} // namespace Slic3r

#endif /* slic3r_GUI_Color_hpp_ */
