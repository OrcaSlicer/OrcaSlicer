#include "AuiPaneLayout.hpp"

namespace Slic3r { namespace GUI {

std::string aui_pane_layout_entry(const std::string& layout, const std::string& pane_name)
{
    // Panes are separated by '|'; SavePerspective() escapes a '|' inside a caption as "\|".
    const std::string prefix = "name=" + pane_name + ";";
    size_t            begin  = 0;
    for (size_t i = 0; i <= layout.size(); ++i) {
        if (i < layout.size() && (layout[i] != '|' || (i > 0 && layout[i - 1] == '\\')))
            continue;
        if (layout.compare(begin, prefix.size(), prefix) == 0)
            return layout.substr(begin, i - begin);
        begin = i + 1;
    }
    return {};
}

}} // namespace Slic3r::GUI
