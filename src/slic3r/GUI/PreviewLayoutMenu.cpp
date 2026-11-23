#include "PreviewLayoutMenu.hpp"

#include <cstddef>
#include <functional>
#include <utility>

#include <wx/event.h>
#include <wx/menu.h>

#include "slic3r/GUI/GCodeViewer.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/PreviewOverlayLayout.hpp"
#include "slic3r/GUI/PreviewPanelSlots.hpp"
#include "slic3r/GUI/wxExtensions.hpp"

namespace Slic3r {
namespace GUI {

void append_preview_layout_menu(wxMenu* view_menu, wxWindow* parent, std::function<bool()> in_preview, std::function<GCodeViewer&()> viewer,
                                std::function<void()> refresh)
{
    wxMenu*    menu      = new wxMenu();
    const auto on_left   = [viewer](PreviewPanel panel) { return viewer().get_panel_slots()[size_t(panel)].column == PreviewColumn::Left; };
    const auto set_slots = [viewer, refresh = std::move(refresh)](const PreviewPanelSlots& slots) {
        viewer().set_panel_slots(slots);
        refresh();
    };
    const auto move_across = [viewer, set_slots](PreviewPanel panel) { set_slots(move_panel_across(viewer().get_panel_slots(), panel)); };
    const auto gcode_first = [viewer]() { return gcode_panel_first(viewer().get_panel_slots()); };

    append_menu_check_item(menu, wxID_ANY, _L("Legend on the Left"), _L("Show the legend at the left edge of the Preview scene."),
                           [move_across](wxCommandEvent&) { move_across(PreviewPanel::Legend); }, parent, in_preview,
                           [on_left]() { return on_left(PreviewPanel::Legend); }, parent);
    append_menu_check_item(menu, wxID_ANY, _L("G-code Window on the Left"), _L("Show the G-code window at the left edge of the Preview scene."),
                           [move_across](wxCommandEvent&) { move_across(PreviewPanel::GCode); }, parent, in_preview,
                           [on_left]() { return on_left(PreviewPanel::GCode); }, parent);
    append_menu_check_item(menu, wxID_ANY, _L("Print Summary on the Left"), _L("Show the print summary at the left edge of the Preview scene."),
                           [move_across](wxCommandEvent&) { move_across(PreviewPanel::Summary); }, parent, in_preview,
                           [on_left]() { return on_left(PreviewPanel::Summary); }, parent);
    append_menu_check_item(menu, wxID_ANY, _L("G-code Window Above the Legend"),
                           _L("When the legend and the G-code window are on the same side, show the G-code window first."),
                           [viewer, set_slots, gcode_first](wxCommandEvent&) {
                               set_slots(put_gcode_panel_first(viewer().get_panel_slots(), !gcode_first()));
                           },
                           parent,
                           [in_preview, on_left]() {
                               return in_preview() && (on_left(PreviewPanel::Legend) == on_left(PreviewPanel::GCode) ||
                                                       on_left(PreviewPanel::Summary) == on_left(PreviewPanel::GCode));
                           },
                           gcode_first, parent);
    append_submenu(view_menu, menu, wxID_ANY, _L("Preview Layout"), _L("Choose where the legend, the print summary and the G-code window stand in the Preview scene."), "",
                   in_preview, parent);
}

} // namespace GUI
} // namespace Slic3r
