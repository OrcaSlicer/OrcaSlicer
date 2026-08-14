#ifndef slic3r_ConfigValueFormatter_hpp_
#define slic3r_ConfigValueFormatter_hpp_

#include <string>

#include <wx/string.h>

namespace Slic3r {

class DynamicPrintConfig;

namespace GUI {

// Return the value of the given option (identified by opt_key, which may contain
// a "#<index>" suffix) formatted as a human readable string.
wxString get_string_value(const std::string& opt_key, const DynamicPrintConfig& config);

// Return the full label of the given option (identified by opt_key, which may contain
// a "#<index>" suffix). Returns "N/A" when the option is not set.
wxString get_full_label(const std::string& opt_key, const DynamicPrintConfig& config);

// Strip the "#<index>" suffix (if any) from the given option key.
std::string get_pure_opt_key(const std::string& opt_key);

// Return the localized label of the currently selected value of an enum option.
wxString get_string_from_enum(const std::string& opt_key, const DynamicPrintConfig& config, bool is_infill = false, int idx = -1);

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_ConfigValueFormatter_hpp_
