#ifndef slic3r_Utils_ColorSpaceConvert_hpp_
#define slic3r_Utils_ColorSpaceConvert_hpp_
#include <string>
#include "libslic3r/ColorSpaceConvert.hpp"

const int CUSTOM_COLOR_COUNT = 16;

class wxColour;
std::string color_to_string(const wxColour &color);
wxColour    string_to_wxColor(const std::string &str);
#endif /* slic3r_Utils_ColorSpaceConvert_hpp_ */
