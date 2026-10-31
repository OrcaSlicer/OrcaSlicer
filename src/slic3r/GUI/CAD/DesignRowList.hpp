#ifndef slic3r_DesignRowList_hpp_
#define slic3r_DesignRowList_hpp_

#include <wx/colour.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/string.h>

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI {

// The Design tab's Feature tree and Bodies lists as DATA: rows that carry their own action icons, a single selection and the
// owner's callbacks. The ImGui sidebar draws them (DesignSidebarMirror.cpp) and calls back into the owner; nothing here is a
// window.
//
// Selection follows wxTreeCtrl's contract, which DesignPanel's mutual exclusion between the two lists relies on: on_select
// runs on every change, whether the user or select() made it, and selecting the row that is already selected changes nothing
// and notifies nobody.
//
// A list built non-selectable is a row of switches, not of objects: a click never selects a row. Its action icons and its
// context menu are the only things that answer.
class DesignRowList : public wxEvtHandler
{
public:
    struct Action {
        int         id;     // the owner's code for it, handed back to on_action
        std::string icon;   // icon name, e.g. "design_eye"; empty for a blank cell that only holds a column
        wxString    tip;
    };
    struct Row {
        std::string         icon;        // type icon at the left; empty for none
        wxString            label;
        wxString            edit_text;   // what the rename editor opens with; empty for the label
        wxColour            colour;      // label colour
        std::vector<Action> actions;     // icon cells at the right end, in display order
    };

    // The hover chip behind an icon button, a light colour dark-mapped at paint (the sidebar's icon buttons hover in it too).
    static constexpr const char* hover_chip = "#D4D4D4";

    // The list is as tall as its rows, at least one and at most `max_visible`; past that it scrolls.
    explicit DesignRowList(int max_visible, bool selectable = true) : m_max_visible(max_visible), m_selectable(selectable) {}

    // Replace every row. Clears the selection without notifying; the owner re-selects the row it keeps, which notifies.
    void set_rows(std::vector<Row> rows) { m_rows = std::move(rows); m_sel = -1; owner = -1; }
    // The row whose feature made the face picked in the viewport: drawn tinted, never selected. Cleared by set_rows and any selection.
    int  owner{-1};
    int  selection() const { return m_sel; }
    const std::vector<Row>& rows() const { return m_rows; }
    unsigned GetItemCount() const { return unsigned(m_rows.size()); }
    int  max_visible() const { return m_max_visible; }
    bool selectable() const { return m_selectable; }
    // Select `row` (-1 clears) and call on_select if that changed the selection.
    void select(int row)
    {
        if (row < 0 || row >= int(m_rows.size())) row = -1;
        if (row == m_sel) return;
        m_sel = row;
        owner = -1;
        if (on_select) on_select();
    }
    void unselect() { select(-1); }
    // Open the in-place editor on a row (the sidebar takes it over through on_begin_rename).
    void begin_rename(int row)
    {
        if (on_begin_rename && row >= 0 && row < int(m_rows.size())) {
            const Row& r = m_rows[size_t(row)];
            on_begin_rename(row, r.edit_text.IsEmpty() ? r.label : r.edit_text);
        }
    }

    std::function<void()>                              on_select;    // the selection changed
    std::function<void()>                              on_activate;  // the selected row was double-clicked
    // A row's action icon was clicked, after the click has finished dispatching.
    std::function<void(int row, int id)>               on_action;
    // Context menu on a row, at a screen position; the row under the pointer is selected first, unless the list is non-selectable.
    std::function<void(int row, const wxPoint& screen)> on_menu;
    // The editor committed `name` for `row`, trimmed and never empty (an empty commit cancels).
    std::function<void(int row, const wxString& name)> on_rename;
    // The sidebar takes a rename over: it gets the row and the text the editor opens with.
    std::function<bool(int row, const wxString& text)> on_begin_rename;

private:
    std::vector<Row> m_rows;
    int              m_max_visible;
    bool             m_selectable;
    int              m_sel{-1};
};

}} // namespace Slic3r::GUI

#endif // slic3r_DesignRowList_hpp_
