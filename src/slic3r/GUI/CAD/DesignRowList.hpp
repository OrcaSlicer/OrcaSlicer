#ifndef slic3r_DesignRowList_hpp_
#define slic3r_DesignRowList_hpp_

#include <wx/vlbox.h>
#include <wx/colour.h>
#include <wx/dc.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/string.h>
#include <wx/types.h>

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "slic3r/GUI/wxExtensions.hpp"   // ScalableBitmap

class wxTextCtrl;

namespace Slic3r { namespace GUI {

// The Design tab's Feature tree and Bodies lists: a single-selection list whose rows carry their
// own action icons. A wxTreeCtrl cannot host a clickable icon per row (it is native on MSW), so
// this list draws its own: each row draws its actions at its right end, highlights the one under
// the pointer, and takes its icons from the owner, so the eye can show whether the row is hidden.
//
// Selection follows wxTreeCtrl's contract, which DesignPanel's mutual exclusion between the two
// lists relies on: on_select runs on every change, whether the user or select() made it, and
// selecting the row that is already selected changes nothing and notifies nobody.
class DesignRowList : public wxVListBox
{
public:
    struct Action {
        int         id;     // the owner's code for it, handed back to on_action
        std::string icon;   // icon name, e.g. "design_eye"
        wxString    tip;
    };
    struct Row {
        std::string         icon;        // type icon at the left; empty for none
        wxString            label;
        wxString            edit_text;   // what the rename editor opens with; empty for the label
        wxColour            colour;      // label colour
        std::vector<Action> actions;     // icon cells at the right end, in display order
    };

    // The hover chip behind an action icon, a light colour dark-mapped at paint. The sidebar's
    // icon buttons hover in it too.
    static constexpr const char* hover_chip = "#D4D4D4";

    // The list is as tall as its rows, at least one and at most `max_visible`; past that it scrolls.
    DesignRowList(wxWindow* parent, int max_visible);

    // Replace every row. Clears the selection without notifying and cancels a rename in progress;
    // the owner re-selects the row it keeps, which notifies.
    void set_rows(std::vector<Row> rows);
    int  selection() const { return GetSelection(); }
    // Select `row` (wxNOT_FOUND clears) and call on_select if that changed the selection.
    void select(int row);
    void unselect() { select(wxNOT_FOUND); }
    // Open the in-place editor on a row, once the current event has finished: opened from inside
    // a popup menu's nested loop it would never appear. Enter or clicking away commits, Esc cancels.
    void begin_rename(int row);
    // Re-rasterise the icons and re-measure the rows after a DPI or theme change.
    void Rescale();

    std::function<void()>                              on_select;    // the selection changed
    std::function<void()>                              on_activate;  // the selected row was double-clicked
    // A row's action icon was clicked. Runs after the click has finished dispatching, and only
    // while that row is still the selected one; the click itself selected it.
    std::function<void(int row, int id)>              on_action;
    // Context menu on a row, at a screen position; the row under the pointer is selected first.
    std::function<void(int row, const wxPoint& screen)> on_menu;
    // The editor committed `name` for `row`, trimmed and never empty (an empty commit cancels).
    // Runs after the editor's own events have finished, so the owner may rebuild the rows here.
    std::function<void(int row, const wxString& name)> on_rename;

protected:
    void    OnDrawItem(wxDC& dc, const wxRect& rect, size_t n) const override;
    void    OnDrawBackground(wxDC& dc, const wxRect& rect, size_t n) const override;
    wxCoord OnMeasureItem(size_t n) const override;

private:
    struct Hit {
        int row{wxNOT_FOUND};
        int cell{-1};   // index into the row's actions, -1 for the rest of the row
        bool operator==(const Hit& o) const { return row == o.row && cell == o.cell; }
        bool operator!=(const Hit& o) const { return !(*this == o); }
    };

    Hit    hit_test(const wxPoint& pt) const;
    wxRect cell_rect(const wxRect& row, size_t count, size_t i) const;
    wxRect label_rect(const wxRect& row, const Row& r) const;
    const ScalableBitmap* icon(const std::string& name) const;
    void   load_icons();
    void   set_hover(const Hit& h);
    void   open_editor(int row);
    void   end_rename(bool commit);
    void   measure_row();
    void   fit_rows();

    std::vector<Row>                      m_rows;
    std::map<std::string, ScalableBitmap> m_icons;
    int                                   m_max_visible;
    int                                   m_row_h{0};          // every row's height, from measure_row()
    Hit                                   m_hover;
    Hit                                   m_pressed;           // action cell under the last left press
    wxTextCtrl*                           m_editor{nullptr};   // created on the first rename, then reused
    int                                   m_edit_row{wxNOT_FOUND};
};

}} // namespace Slic3r::GUI

#endif // slic3r_DesignRowList_hpp_
