#pragma once

#include <string>

class wxColour;
std::string color_to_string(const wxColour& color);
wxColour string_to_wxColor(const std::string& str);
