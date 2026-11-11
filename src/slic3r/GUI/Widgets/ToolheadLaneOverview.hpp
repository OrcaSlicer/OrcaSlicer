#ifndef slic3r_GUI_ToolheadLaneOverview_hpp_
#define slic3r_GUI_ToolheadLaneOverview_hpp_

#include <wx/colour.h>
#include <wx/event.h>
#include <wx/string.h>
#include <wx/window.h>

#include <vector>

// Emitted when a toolhead column is clicked; GetInt() is the toolhead index.
wxDECLARE_EVENT(EVT_TOOLHEAD_LANE_OVERVIEW_SELECT, wxCommandEvent);

namespace Slic3r { namespace GUI {

// Every toolhead of a generic multi-toolhead printer side by side: the lanes feeding each
// toolhead, drawn in their filament colour, with a path from each lane down to its toolhead.
// The printer's active toolhead is highlighted; the toolhead whose details are shown below is
// marked as selected, and clicking a column selects it.
class ToolheadLaneOverview : public wxWindow
{
public:
    struct Lane
    {
        wxColour colour;
        wxString material;
        wxString name;              // shown in the tooltip, e.g. "Lane 1" or "External"
        bool     loaded   = false;
        bool     external = false;  // the toolhead's external spool, not an AMS lane
    };
    using Toolhead = std::vector<Lane>;

    explicit ToolheadLaneOverview(wxWindow* parent);

    // Redraws only when something changed.
    void SetToolheads(std::vector<Toolhead> toolheads, int active_toolhead, int selected_toolhead);
    void SetSelectedToolhead(int toolhead);
    int  GetSelectedToolhead() const { return m_selected; }

    void Rescale();

private:
    void   on_paint(wxPaintEvent& event);
    void   on_left_down(wxMouseEvent& event);
    void   on_motion(wxMouseEvent& event);
    void   update_min_size();
    int    column_width(const Toolhead& toolhead) const;
    int    column_at(int x) const;
    wxRect column_rect(int index) const;

    std::vector<Toolhead> m_toolheads;
    int                   m_active   = -1;
    int                   m_selected = -1;
    int                   m_tooltip_column = -2;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_ToolheadLaneOverview_hpp_
