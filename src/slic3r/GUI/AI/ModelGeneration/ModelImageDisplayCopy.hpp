#pragma once
#include <boost/filesystem.hpp>
#include <wx/image.h>
#include <wx/log.h>

namespace Slic3r::GUI {
// Default presentation derivative when available. Never return it as a model
// generation input. Updating the source invalidates an older display derivative.
inline wxImage load_model_image_display_copy(const boost::filesystem::path& source) {
    if (source.empty()) return {};
    auto display = source; display += ".display.png";
    boost::system::error_code error;
    if (!boost::filesystem::is_regular_file(display, error) || error) return {};
    const auto source_time = boost::filesystem::last_write_time(source, error);
    if (error) return {};
    const auto display_time = boost::filesystem::last_write_time(display, error);
    if (error || display_time < source_time) return {};
    wxLogNull quiet;
    return wxImage(display.wstring());
}
}
