#pragma once

#include <string>

namespace Slic3r { namespace GUI {

// The part a wxAuiManager layout string (wxAuiManager::SavePerspective) holds for `pane_name`, in the
// form wxAuiManager::LoadPaneInfo() takes, or empty when the layout has no such pane.
std::string aui_pane_layout_entry(const std::string& layout, const std::string& pane_name);

}} // namespace Slic3r::GUI
