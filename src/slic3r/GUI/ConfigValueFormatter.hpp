#pragma once

#include <string>

#include <wx/string.h>

namespace Slic3r {

class DynamicPrintConfig;

namespace GUI {

// Human-readable value of opt_key (may carry a "#<index>" suffix) in config.
wxString get_string_value(const std::string& opt_key, const DynamicPrintConfig& config);

// Full label of opt_key; "N/A" when the option is not set.
wxString get_full_label(const std::string& opt_key, const DynamicPrintConfig& config);

// Strip the "#<index>" suffix (if any) from the option key.
std::string get_pure_opt_key(const std::string& opt_key);

// Localized label of the currently selected value of an enum option.
wxString get_string_from_enum(const std::string& opt_key, const DynamicPrintConfig& config, bool is_infill = false, int idx = -1);

} // namespace GUI
} // namespace Slic3r
