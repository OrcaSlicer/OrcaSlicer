#pragma once

#include <functional>

class wxMenu;
class wxWindow;

namespace Slic3r {
namespace GUI {

class GCodeViewer;

// The Preview Layout submenu of the View menu: where the legend and the G-code panel stand in the Preview scene.
void append_preview_layout_menu(wxMenu* view_menu, wxWindow* parent, std::function<bool()> in_preview, std::function<GCodeViewer&()> viewer,
                                std::function<void()> refresh);

} // namespace GUI
} // namespace Slic3r
