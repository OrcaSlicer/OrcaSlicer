#include "slic3r/GUI/CAD/DesignRowList.hpp"

#include "slic3r/GUI/Widgets/StateColor.hpp"
#include "slic3r/GUI/wxExtensions.hpp"

#include <wx/control.h>
#include <wx/dc.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/graphics.h>
#include <wx/scrolwin.h>
#include <wx/string.h>
#include <wx/textctrl.h>
#include <wx/types.h>
#include <wx/vlbox.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI {

// Row and icon cell geometry, in DIP.
static constexpr int kIconPx  = 16;   // type and action icons
static constexpr int kCellDip = 20;   // square action cell, hover chip included
static constexpr int kPadDip  = 4;    // row edges, and the gap before the action cells
static constexpr int kGapDip  = 6;    // type icon to label

DesignRowList::DesignRowList(wxWindow* parent, int max_visible)
    // wxVListBox defaults to wxBORDER_THEME; the sidebar's lists take a simple frame.
    : wxVListBox(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE)
    , m_max_visible(std::max(max_visible, 1))
{
    Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) { if (on_select) on_select(); });
    Bind(wxEVT_LISTBOX_DCLICK, [this](wxCommandEvent&) { if (on_activate) on_activate(); });

    Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
        set_hover(hit_test(e.GetPosition()));
        e.Skip();
    });
    Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& e) {
        set_hover(Hit{});
        m_pressed = Hit{};
        e.Skip();
    });
    // A press on an action cell is remembered and skipped, so the list still selects the row.
    Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
        const Hit h = hit_test(e.GetPosition());
        m_pressed = h.cell >= 0 ? h : Hit{};
        e.Skip();
    });
    // The second press of a double-click on a cell is a press too, and is not skipped: the list
    // would otherwise turn it into a row double-click, so a quick double toggle of the eye would
    // also open the feature for editing.
    Bind(wxEVT_LEFT_DCLICK, [this](wxMouseEvent& e) {
        const Hit h = hit_test(e.GetPosition());
        if (h.cell < 0) { m_pressed = Hit{}; e.Skip(); return; }
        m_pressed = h;
    });
    Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& e) {
        e.Skip();
        const Hit pressed = m_pressed;
        m_pressed = Hit{};
        if (pressed.cell < 0 || hit_test(e.GetPosition()) != pressed)
            return;
        const int id = m_rows[pressed.row].actions[pressed.cell].id;
        // After the click has finished dispatching: the action may rebuild these rows. The row
        // must still exist and still be the selected one, or the click is dropped rather than
        // applied to whatever row took its place.
        CallAfter([this, row = pressed.row, id] {
            if (row < int(GetItemCount()) && row == GetSelection() && on_action)
                on_action(row, id);
        });
    });
    // Right-click targets the row under the pointer. Skipped, because on MSW a handled right
    // press suppresses the wxEVT_CONTEXT_MENU that follows it.
    Bind(wxEVT_RIGHT_DOWN, [this](wxMouseEvent& e) {
        const Hit h = hit_test(e.GetPosition());
        if (h.row != wxNOT_FOUND) select(h.row);
        e.Skip();
    });
    Bind(wxEVT_CONTEXT_MENU, [this](wxContextMenuEvent& e) {
        wxPoint screen = e.GetPosition();   // screen coordinates; wxDefaultPosition from the keyboard
        int     row    = GetSelection();
        if (screen == wxDefaultPosition) {
            if (row == wxNOT_FOUND) return;
            screen = ClientToScreen(GetItemRect(row).GetBottomLeft());
        } else {
            row = VirtualHitTest(ScreenToClient(screen).y);
        }
        if (row == wxNOT_FOUND || row >= int(m_rows.size()))
            return;
        // On MSW the menu comes with the button's release, and the pointer may have moved to
        // another row since the press selected one: the menu is for the row it opens over.
        select(row);
        if (on_menu) on_menu(row, screen);
    });
    // wxVListBox takes every wheel event, even with nothing to scroll, so a short list would stop
    // the sidebar it sits in from scrolling under the pointer. Hand it on instead. Not on GTK,
    // where the sidebar scrolls natively and passes on an unhandled wheel by itself.
    Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent& e) {
        end_rename(true);
#ifndef __WXGTK__
        if (GetVisibleRowsBegin() == 0 && GetVisibleRowsEnd() >= GetItemCount()) {
            for (wxWindow* w = GetParent(); w != nullptr; w = w->GetParent())
                if (auto* sw = dynamic_cast<wxScrolledWindow*>(w)) {
                    wxMouseEvent fwd(e);
                    fwd.SetEventObject(sw);
                    sw->GetEventHandler()->ProcessEvent(fwd);
                    return;
                }
        }
#endif
        e.Skip();
    });
    Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
        end_rename(true);   // the editor sits on a row position the new size may have moved
        e.Skip();
    });
    measure_row();
    fit_rows();
}

// Every row is one height. Measured once per font and scale rather than on each OnMeasureItem,
// which wxVListBox calls for every row it walks on every paint and hit test.
void DesignRowList::measure_row()
{
    m_row_h = std::max(GetCharHeight() + 8, FromDIP(kCellDip + 2));
}

void DesignRowList::set_rows(std::vector<Row> rows)
{
    end_rename(false);   // the row it names may no longer be that row
    SetSelection(wxNOT_FOUND);
    m_rows    = std::move(rows);
    m_hover   = Hit{};
    m_pressed = Hit{};
    UnsetToolTip();
    load_icons();
    SetItemCount(m_rows.size());
    fit_rows();
    Refresh();
}

void DesignRowList::select(int row)
{
    if (row < 0 || row >= int(GetItemCount()))
        row = wxNOT_FOUND;
    if (row == GetSelection())
        return;
    SetSelection(row);
    if (on_select) on_select();
}

void DesignRowList::fit_rows()
{
    const int shown = std::clamp(int(GetItemCount()), 1, m_max_visible);
    // Plus the frame, so the last row is not clipped by it.
    const int h = shown * m_row_h + (GetSize().y - GetClientSize().y);
    SetMinSize(wxSize(-1, h));
    SetMaxSize(wxSize(-1, h));
}

void DesignRowList::Rescale()
{
    m_icons.clear();
    load_icons();
    measure_row();
    // The scrollbar's total is only recomputed when the item count is set; RefreshAll() alone
    // would leave it at the old scale.
    SetItemCount(GetItemCount());
    fit_rows();
    Refresh();
}

void DesignRowList::begin_rename(int row)
{
    CallAfter([this, row] { open_editor(row); });
}

void DesignRowList::open_editor(int row)
{
    if (row < 0 || row >= int(m_rows.size()))
        return;
    end_rename(false);
    if (m_editor == nullptr) {
        // A raw wxTextCtrl, as wxTreeCtrl's own label editor is: it is a transient overlay on one
        // row, not a form field, and ::TextInput's frame would not fit inside the row.
        m_editor = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                  wxTE_PROCESS_ENTER | wxBORDER_SIMPLE);
        m_editor->Hide();
        m_editor->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) {
            end_rename(true);
            SetFocus();
        });
        // Esc must stop here. CHAR_HOOK reaches the focused editor before its ancestors, and the
        // Design panel's own hook answers Esc whatever holds focus, so a skipped Esc would close
        // the open tool as well as the editor.
        m_editor->Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
            if (e.GetKeyCode() != WXK_ESCAPE) { e.Skip(); return; }
            end_rename(false);
            SetFocus();
        });
        m_editor->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) {
            e.Skip();
            end_rename(true);
        });
    }
    if (!IsRowVisible(row))
        ScrollToRow(row);
    const Row&   r  = m_rows[row];
    const wxRect rc = label_rect(GetItemRect(row), r);
    m_editor->SetFont(GetFont());
    m_editor->SetBackgroundColour(GetBackgroundColour());
    m_editor->SetForegroundColour(r.colour.IsOk() ? r.colour : GetForegroundColour());
    m_editor->SetValue(r.edit_text.empty() ? r.label : r.edit_text);
    const int h = std::max(rc.height, m_editor->GetBestSize().y);
    const int y = std::clamp(rc.y + (rc.height - h) / 2, 0, std::max(0, GetClientSize().y - h));
    m_editor->SetSize(rc.x - FromDIP(2), y, rc.width + FromDIP(2), h);
    m_edit_row = row;
    m_editor->Show();
    m_editor->SetFocus();
    m_editor->SelectAll();
}

void DesignRowList::end_rename(bool commit)
{
    if (m_edit_row == wxNOT_FOUND)
        return;
    // Cleared first: hiding the focused editor moves the focus, whose kill-focus comes back here.
    const int row  = m_edit_row;
    wxString  text = m_editor->GetValue();
    m_edit_row = wxNOT_FOUND;
    m_editor->Hide();
    text.Trim(true).Trim(false);
    if (commit && on_rename && !text.empty())   // a nameless row is worse than a badly named one
        CallAfter([this, row, text] {
            if (on_rename && row < int(GetItemCount()))
                on_rename(row, text);
        });
}

wxCoord DesignRowList::OnMeasureItem(size_t) const
{
    return m_row_h;
}

void DesignRowList::OnDrawBackground(wxDC& dc, const wxRect& rect, size_t n) const
{
    // ObjectList's selection teal, and the dropdowns' lighter hover teal; both are dark-mapped.
    wxColour bg;
    if (IsSelected(n))
        bg = StateColor::darkModeColorFor(wxColour("#BFE1DE"));
    else if (int(n) == m_hover.row)
        bg = StateColor::darkModeColorFor(wxColour("#E5F0EE"));
    if (!bg.IsOk())
        return;   // the list already cleared to its background colour
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(bg));
    dc.DrawRectangle(rect);
}

void DesignRowList::OnDrawItem(wxDC& dc, const wxRect& rect, size_t n) const
{
    if (n >= m_rows.size())
        return;
    const Row& r = m_rows[n];

    if (const ScalableBitmap* bmp = icon(r.icon)) {
        const wxSize sz = bmp->GetBmpSize();
        dc.DrawBitmap(bmp->bmp(), rect.x + FromDIP(kPadDip), rect.y + (rect.height - sz.y) / 2, true);
    }

    const wxRect lr = label_rect(rect, r);
    dc.SetFont(GetFont());
    dc.SetTextForeground(r.colour.IsOk() ? r.colour : GetForegroundColour());
    const wxString text = wxControl::Ellipsize(r.label, dc, wxELLIPSIZE_END, std::max(lr.width, 0));
    dc.DrawText(text, lr.x, lr.y + (lr.height - dc.GetCharHeight()) / 2);

    for (size_t i = 0; i < r.actions.size(); ++i) {
        const wxRect cell = cell_rect(rect, r.actions.size(), i);
        if (m_hover.row == int(n) && m_hover.cell == int(i)) {
            // Drawn through a graphics context so the rounded corners are anti-aliased on MSW too,
            // and released before the icon goes on top.
            const wxColour chip = StateColor::darkModeColorFor(wxColour(hover_chip));
            const double   rad  = FromDIP(4);
            std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::CreateFromUnknownDC(dc));
            if (gc) {
                gc->SetPen(*wxTRANSPARENT_PEN);
                gc->SetBrush(wxBrush(chip));
                gc->DrawRoundedRectangle(cell.x, cell.y, cell.width, cell.height, rad);
            } else {
                dc.SetPen(*wxTRANSPARENT_PEN);
                dc.SetBrush(wxBrush(chip));
                dc.DrawRoundedRectangle(cell, rad);
            }
        }
        if (const ScalableBitmap* bmp = icon(r.actions[i].icon)) {
            const wxSize sz = bmp->GetBmpSize();
            dc.DrawBitmap(bmp->bmp(), cell.x + (cell.width - sz.x) / 2, cell.y + (cell.height - sz.y) / 2, true);
        }
    }
}

DesignRowList::Hit DesignRowList::hit_test(const wxPoint& pt) const
{
    Hit h;
    h.row = VirtualHitTest(pt.y);
    if (h.row == wxNOT_FOUND || h.row >= int(m_rows.size()))
        return Hit{};
    const wxRect rect = GetItemRect(h.row);
    const auto&  acts = m_rows[h.row].actions;
    for (size_t i = 0; i < acts.size(); ++i)
        if (cell_rect(rect, acts.size(), i).Contains(pt)) {
            h.cell = int(i);
            break;
        }
    return h;
}

wxRect DesignRowList::cell_rect(const wxRect& row, size_t count, size_t i) const
{
    const int side = FromDIP(kCellDip);
    const int x    = row.GetRight() + 1 - FromDIP(kPadDip) - int(count - i) * side;
    return wxRect(x, row.y + (row.height - side) / 2, side, side);
}

wxRect DesignRowList::label_rect(const wxRect& row, const Row& r) const
{
    int left = row.x + FromDIP(kPadDip);
    if (const ScalableBitmap* bmp = icon(r.icon))
        left += bmp->GetBmpSize().x + FromDIP(kGapDip);
    const int right = r.actions.empty() ? row.GetRight() + 1 - FromDIP(kPadDip)
                                        : cell_rect(row, r.actions.size(), 0).x - FromDIP(kPadDip);
    return wxRect(left, row.y, std::max(right - left, 0), row.height);
}

// nullptr for no icon: load_icons() never stores an empty name.
const ScalableBitmap* DesignRowList::icon(const std::string& name) const
{
    const auto it = m_icons.find(name);
    return it == m_icons.end() ? nullptr : &it->second;
}

void DesignRowList::load_icons()
{
    auto load = [this](const std::string& name) {
        if (!name.empty() && m_icons.find(name) == m_icons.end())
            m_icons.emplace(name, ScalableBitmap(this, name, kIconPx));
    };
    for (const Row& r : m_rows) {
        load(r.icon);
        for (const Action& a : r.actions)
            load(a.icon);
    }
}

void DesignRowList::set_hover(const Hit& h)
{
    if (h == m_hover)
        return;
    const int old = m_hover.row;
    m_hover = h;
    if (old != wxNOT_FOUND && old < int(GetItemCount()))
        RefreshRow(old);
    if (h.row != wxNOT_FOUND && h.row != old)
        RefreshRow(h.row);

    // The tip names the icon under the pointer, or the whole label where the row cuts it short.
    wxString tip;
    if (h.row != wxNOT_FOUND) {
        const Row& r = m_rows[h.row];
        if (h.cell >= 0)
            tip = r.actions[h.cell].tip;
        else if (GetTextExtent(r.label).x > label_rect(GetItemRect(h.row), r).width)
            tip = r.label;
    }
    if (tip.empty())
        UnsetToolTip();
    else if (tip != GetToolTipText())
        SetToolTip(tip);
}

}} // namespace Slic3r::GUI
