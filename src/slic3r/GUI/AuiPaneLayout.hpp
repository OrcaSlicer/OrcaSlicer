#pragma once

#include <string>

namespace Slic3r { namespace GUI {

// The part a wxAuiManager layout string (wxAuiManager::SavePerspective) holds for `pane_name`, in the
// form wxAuiManager::LoadPaneInfo() takes, or empty when the layout has no such pane.
std::string aui_pane_layout_entry(const std::string& layout, const std::string& pane_name);

// The dock size recorded in `layout` (wxAuiManager::SavePerspective) for a dock identified by
// (direction, layer, row), formatted as `dock_size(dir,layer,row)=<size>`. Returns 0 when not found.
int aui_dock_layout_size(const std::string& layout, int direction, int layer, int row);

}} // namespace Slic3r::GUI
