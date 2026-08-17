#include "ColorConvert.hpp"

#include <boost/algorithm/string.hpp>
#include <wx/colordlg.h>

#include <algorithm>
#include <vector>

std::string color_to_string(const wxColour& color)
{
    std::string str = std::to_string(color.Red()) + "," + std::to_string(color.Green()) + "," + std::to_string(color.Blue()) + "," +
                      std::to_string(color.Alpha());
    return str;
}

wxColour string_to_wxColor(const std::string& str)
{
    wxColour color;
    std::vector<std::string> result;
    boost::split(result, str, boost::is_any_of(","));
    if (result.size() == 4) {
        color = wxColour(std::stoi(result[0]), std::stoi(result[1]), std::stoi(result[2]), std::stoi(result[3]));
    }
    return color;
}
