#pragma once

#include <wx/aui/framemanager.h>

namespace Slic3r { namespace GUI {

// The wxAuiManager behind Orca's docked sidebars: Orca's caption and sash art in both themes,
// floating frames that follow the app theme, and no floating on Wayland.
class AuiMgr : public wxAuiManager
{
public:
    // Manages `window`, whose own wxEVT_SIZE handlers must Skip().
    void init(wxWindow* window);
    // Captions, sashes and borders in the current theme.
    void apply_color_mode();
    // Keeps the docked size of `window`'s pane in its best_size, which re-docking and a saved layout
    // use: wxAUI does not record a dragged sash there.
    void track_docked_size(wxWindow* window);

    // A tab's sidebar pane: docked left by default, movable to the right or floating, no close button.
    static wxAuiPaneInfo sidebar_pane_info();

    wxAuiFloatingFrame* CreateFloatingFrame(wxWindow* parent, const wxAuiPaneInfo& pane) override;
};

}} // namespace Slic3r::GUI
