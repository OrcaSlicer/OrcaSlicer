#include "OptionStateRegistry.hpp"

#include "I18N.hpp"
#include "format.hpp"

#include <string>
#include <wx/string.h>

namespace Slic3r::GUI {

wxString OptionStateRegistry::description(const std::string& key) const
{
    wxString text;
    if (const wxString* reason = hidden_reason(key))
        text = reason->empty() ? _L("Hidden by the current configuration.") : format_wxstr(_L("Hidden: %1%"), *reason);
    if (const wxString* reason = disabled_reason(key)) {
        if (!text.empty())
            text += "\n\n";
        text += reason->empty() ? _L("Disabled by the current configuration.") : format_wxstr(_L("Disabled: %1%"), *reason);
    }
    return text;
}

} // namespace Slic3r::GUI
