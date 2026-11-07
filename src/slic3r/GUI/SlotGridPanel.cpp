#include "SlotGridPanel.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <exception>
#include <string>

#include <wx/colour.h>
#include <utility>
#include <wx/anybutton.h>
#include <wx/dc.h>
#include <vector>
#include <slic3r/GUI/Widgets/PopupWindow.hpp>
#include <wx/dcbuffer.h>
#include <wx/dcclient.h>
#include <wx/dcgraph.h>
#include <wx/dcmemory.h>
#include <wx/display.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/string.h>
#include <wx/tooltip.h>
#include <wx/window.h>

#include "libslic3r/FilamentInventory.hpp"
#include "libslic3r/Preset.hpp"

#include "DragDropPanel.hpp" // Hex2Color
#include "FilamentInventoryStore.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"
#include "wxExtensions.hpp"

namespace Slic3r {
namespace GUI {

namespace {

// 8 tiles of 64 DIP and their gaps fit the 675 DIP sync dialog.
constexpr size_t TILES_PER_ROW = 8;
// Taller grids scroll, leaving the rest of the screen for the dialog around them.
constexpr int MAX_SCREEN_SHARE_PERCENT = 60;

const wxColour EMPTY_FILL(0xD9, 0xD9, 0xD9);
const wxColour EMPTY_STROKE(0x9A, 0x9A, 0x9A);
const wxColour TEXT_DARK(0x26, 0x2E, 0x30);
const wxColour TEXT_DISABLED(0xCE, 0xCE, 0xCE);
const wxColour BORDER_IDLE("#DBDBDB");
const wxColour BORDER_SELECTED("#009688"); // Orca accent teal

// Not StateColor::darkModeColorFor: the text sits on the filament colour, which ignores the theme.
wxColour text_colour_for(const wxColour& fill) { return fill.GetLuminance() < 0.6 ? *wxWHITE : TEXT_DARK; }

// wxGCDC for alpha and anti-aliasing (MSW GDI has neither); offscreen bitmap on MSW against flicker.
void paint_gc(wxWindow* window, const std::function<void(wxDC&)>& draw)
{
    wxPaintDC dc(window);
#ifdef __WXMSW__
    const wxSize size = window->GetSize();
    wxMemoryDC   memdc;
    wxBitmap   bmp(size.x, size.y);
    memdc.SelectObject(bmp);
    {
        wxGCDC gc(memdc);
        gc.SetBackground(wxBrush(window->GetParent()->GetBackgroundColour()));
        gc.Clear();
        draw(gc);
    }
    memdc.SelectObject(wxNullBitmap);
    dc.DrawBitmap(bmp, 0, 0);
#else
    wxGCDC gc(dc);
    gc.SetBackground(wxBrush(window->GetParent()->GetBackgroundColour()));
    gc.Clear();
    draw(gc);
#endif
}

} // namespace

class SlotTile : public wxPanel
{
public:
    static constexpr int WIDTH_DIP  = 64;
    static constexpr int HEIGHT_DIP = 72;

    SlotTile(wxWindow* parent, size_t index, const SlotGridSlot& slot, const SlotGridOptions& options,
             std::function<void()> on_click, std::function<void()> on_edit)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, parent->FromDIP(wxSize(WIDTH_DIP, HEIGHT_DIP)))
        , m_index(index), m_slot(slot), m_options(options), m_on_click(std::move(on_click))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        m_checked_bmp = ScalableBitmap(this, "mapping_item_checked", FromDIP(16));
        if (options.show_edit_pencil) {
            m_edit_btn = new ScalableButton(this, wxID_ANY, "edit", wxEmptyString, wxDefaultSize, wxDefaultPosition,
                                            wxBU_EXACTFIT | wxNO_BORDER, true, 14);
            m_edit_btn->SetBackgroundColour(*wxWHITE); // on the badge render() draws behind it
            m_edit_btn->Bind(wxEVT_BUTTON, [on_edit = std::move(on_edit)](wxCommandEvent&) { if (on_edit) on_edit(); });
            // Hidden, not disabled: a disabled button looks enabled here and gets no tooltip on MSW.
            m_edit_btn->Show(!slot.edit_disabled);
            m_edit_btn->SetToolTip(slot.edit_disabled ? slot.tooltip : _L("Edit filament"));
            wxBoxSizer* overlay = new wxBoxSizer(wxHORIZONTAL);
            overlay->AddStretchSpacer();
            overlay->Add(m_edit_btn, 0, wxTOP | wxRIGHT, FromDIP(2));
            wxBoxSizer* v = new wxBoxSizer(wxVERTICAL);
            v->Add(overlay, 0, wxEXPAND);
            SetSizer(v); // fixed footprint; the pencil overlays the corner
        }
        if (!slot.tooltip.IsEmpty())
            SetToolTip(slot.tooltip);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint_gc(this, [this](wxDC& dc) { doRender(dc); }); });
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
            // A wrong-type tile stays enabled for its tooltip (none on a disabled control on MSW).
            if (m_options.pick_mode && !m_slot.disabled && !m_slot.wrong_type && m_on_click)
                m_on_click();
        });
        if (m_options.pick_mode && m_slot.disabled)
            Enable(false);
    }

    void SetWashed(bool washed)
    {
        if (m_washed == washed) return;
        m_washed = washed;
        Refresh();
    }

private:
    void doRender(wxDC& dc)
    {
        const wxSize   size = GetSize();
        const wxColour fill = m_slot.empty ? EMPTY_FILL : m_slot.colour;
        dc.SetPen(wxPen(StateColor::darkModeColorFor(BORDER_IDLE), 1));
        dc.SetBrush(wxBrush(fill));
        dc.DrawRoundedRectangle(0, 0, size.x - 1, size.y - 1, FromDIP(6));
        if (m_slot.empty) {
            dc.SetPen(wxPen(EMPTY_STROKE, FromDIP(2)));
            dc.DrawLine(FromDIP(6), FromDIP(6), size.x - FromDIP(6), size.y - FromDIP(6));
        }

        wxColour text = text_colour_for(fill);
        if (m_slot.empty || m_slot.wrong_type || (m_options.pick_mode && m_slot.disabled))
            text = m_slot.empty ? TEXT_DARK : TEXT_DISABLED;
        dc.SetTextForeground(text);

        // Badge so the pencil reads on any filament colour.
        if (m_edit_btn != nullptr && m_edit_btn->IsShown()) {
            wxRect badge = m_edit_btn->GetRect();
            badge.Inflate(FromDIP(2));
            dc.SetPen(wxPen(TEXT_DARK, 1));
            dc.SetBrush(*wxWHITE_BRUSH);
            dc.DrawRoundedRectangle(badge, FromDIP(3));
        }

        const int reserved = m_edit_btn != nullptr && m_edit_btn->IsShown() ? m_edit_btn->GetSize().GetWidth() + FromDIP(4)
                             : m_slot.checked ? m_checked_bmp.GetBmpWidth() + FromDIP(4) : 0;
        const int max_w = std::max(0, size.x - FromDIP(8) - reserved);
        int y = FromDIP(6);

        // Falls back to the slot index when the virtual tool is unknown.
        dc.SetFont(::Label::Head_14);
        const wxString tool = wxString::Format("T%d", (m_slot.virtual_tool >= 0 ? m_slot.virtual_tool : (int) m_index) + 1);
        dc.DrawText(tool, FromDIP(6), y);
        y += dc.GetTextExtent(tool).GetHeight() + FromDIP(2);

        if (!m_slot.empty) {
            dc.SetFont(::Label::Body_12);
            const wxString type = wxControl::Ellipsize(m_slot.type, dc, wxELLIPSIZE_END, max_w);
            dc.DrawText(type, FromDIP(6), y);
            y += dc.GetTextExtent(type).GetHeight() + FromDIP(2);
        }
        if (m_options.show_extruder_on_tile && m_slot.extruder >= 0) {
            dc.SetFont(::Label::Body_12);
            dc.DrawText(wxString::Format("E%d", m_slot.extruder + 1), FromDIP(6), y);
        }

        if (m_slot.checked)
            dc.DrawBitmap(m_checked_bmp.bmp(), size.x - m_checked_bmp.GetBmpWidth() - FromDIP(4), FromDIP(4));

        // Filtered-out slots fade rather than vanish, so the layout holds.
        if (m_washed) {
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(wxColour(255, 255, 255, 160)));
            dc.DrawRoundedRectangle(0, 0, size.x - 1, size.y - 1, FromDIP(6));
        }
    }

    size_t                 m_index;
    SlotGridSlot           m_slot;
    SlotGridOptions        m_options;
    std::function<void()>  m_on_click;
    ScalableButton*        m_edit_btn{nullptr};
    ScalableBitmap         m_checked_bmp;
    bool                   m_washed{false};
};

class ExtruderTile : public wxPanel
{
public:
    static constexpr int WIDTH_DIP  = 64;
    static constexpr int HEIGHT_DIP = 44;

    ExtruderTile(wxWindow* parent, int extruder, size_t slot_count, std::function<void()> on_click)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, parent->FromDIP(wxSize(WIDTH_DIP, HEIGHT_DIP)))
        , m_extruder(extruder), m_slot_count(slot_count), m_on_click(std::move(on_click))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetToolTip(wxString::Format(_L("Extruder %d: %d filament slots. Click to show only its slots."), extruder + 1, (int) slot_count));
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint_gc(this, [this](wxDC& dc) { doRender(dc); }); });
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) { if (m_on_click) m_on_click(); });
    }

    void SetSelected(bool selected)
    {
        if (m_selected == selected) return;
        m_selected = selected;
        Refresh();
    }

private:
    void doRender(wxDC& gc)
    {
        const wxSize size = GetSize();
        gc.SetPen(wxPen(m_selected ? BORDER_SELECTED : StateColor::darkModeColorFor(BORDER_IDLE), m_selected ? FromDIP(2) : 1));
        gc.SetBrush(wxBrush(StateColor::darkModeColorFor(*wxWHITE)));
        gc.DrawRoundedRectangle(1, 1, size.x - 2, size.y - 2, FromDIP(6));
        gc.SetTextForeground(StateColor::darkModeColorFor(TEXT_DARK));
        gc.SetFont(::Label::Head_14);
        const wxString label = wxString::Format("E%d", m_extruder + 1);
        wxSize ext = gc.GetTextExtent(label);
        gc.DrawText(label, (size.x - ext.x) / 2, FromDIP(4));
        gc.SetFont(::Label::Body_12);
        const wxString count = wxString::Format("(%d)", (int) m_slot_count);
        ext = gc.GetTextExtent(count);
        gc.DrawText(count, (size.x - ext.x) / 2, FromDIP(4) + gc.GetTextExtent(label).y);
    }

    int                   m_extruder;
    size_t                m_slot_count;
    bool                  m_selected{false};
    std::function<void()> m_on_click;
};

SlotGridPanel::SlotGridPanel(wxWindow* parent, const SlotGridOptions& options)
    : wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL), m_options(options)
{
    SetBackgroundColour(parent->GetBackgroundColour());
    SetScrollRate(0, FromDIP(20));
    m_sizer = new wxBoxSizer(wxVERTICAL);
    SetSizer(m_sizer);
}

void SlotGridPanel::SetSlots(const std::vector<SlotGridSlot>& slots, size_t extruder_count)
{
    m_sizer->Clear(true);
    m_tiles.clear();
    m_extruders.clear();
    m_slots = slots;

    const int gap = FromDIP(6);

    if (m_options.show_extruder_row && extruder_count > 1) {
        wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
        for (size_t e = 0; e < extruder_count; ++e) {
            const size_t fed = std::count_if(slots.begin(), slots.end(), [e](const SlotGridSlot& s) { return s.extruder == (int) e; });
            auto* tile = new ExtruderTile(this, (int) e, fed, [this, e]() { OnExtruderClicked((int) e); });
            m_extruders.push_back(tile);
            row->Add(tile, 0, wxRIGHT, gap);
        }
        m_sizer->Add(row, 0, wxBOTTOM, gap);
        m_sizer->Add(new wxStaticLine(this), 0, wxEXPAND | wxBOTTOM, gap);
    }

    m_tiles.resize(slots.size(), nullptr);
    for (size_t i = 0; i < slots.size(); ++i)
        m_tiles[i] = new SlotTile(this, i, slots[i], m_options, [this, i]() { OnSlotClicked(i); },
                                  [this, i]() { if (m_on_edit) m_on_edit(i); });

    std::vector<wxString> units;
    for (const SlotGridSlot& s : slots)
        if (!s.unit.IsEmpty() && std::find(units.begin(), units.end(), s.unit) == units.end())
            units.push_back(s.unit);

    auto ordered = [&slots](const wxString& unit) {
        std::vector<size_t> indices;
        for (size_t i = 0; i < slots.size(); ++i)
            if (slots[i].unit == unit) indices.push_back(i);
        std::stable_sort(indices.begin(), indices.end(), [&slots](size_t a, size_t b) { return slots[a].slot < slots[b].slot; });
        return indices;
    };

    // Wraps after TILES_PER_ROW, so a long unit or many unit-less slots stay inside a dialog.
    auto tile_rows = [gap](size_t count) {
        return new wxFlexGridSizer(0, (int) std::min(count, TILES_PER_ROW), gap, gap);
    };

    // Units share a row while it stays within one full row of tiles, so small units sit side by side.
    const int    tile_w  = FromDIP(SlotTile::WIDTH_DIP);
    int          max_row = 0;
    wxBoxSizer*  line    = nullptr;
    int          line_w  = 0;
    for (const wxString& unit : units) {
        const std::vector<size_t> members = ordered(unit);
        const wxString title = members.empty() || slots[members.front()].unit_label.IsEmpty() ? unit : slots[members.front()].unit_label;
        auto* box  = new wxStaticBoxSizer(wxVERTICAL, this, title);
        auto* rows = tile_rows(members.size());
        for (size_t i : members) {
            m_tiles[i]->Reparent(box->GetStaticBox());
            rows->Add(m_tiles[i]);
        }
        box->Add(rows, 0, wxALL, gap / 2);

        const int box_w = box->GetMinSize().x;
        if (max_row == 0) {
            int border_top = 0, border_side = 0;
            box->GetStaticBox()->GetBordersForSizer(&border_top, &border_side);
            max_row = 2 * border_side + gap + (int) TILES_PER_ROW * tile_w + ((int) TILES_PER_ROW - 1) * gap;
        }
        if (line == nullptr || line_w + gap + box_w > max_row) {
            line = new wxBoxSizer(wxHORIZONTAL);
            m_sizer->Add(line, 0, wxBOTTOM, gap);
            line_w = 0;
        }
        line->Add(box, 0, line_w > 0 ? wxLEFT : 0, gap);
        line_w += (line_w > 0 ? gap : 0) + box_w;
    }
    const std::vector<size_t> direct = ordered(wxString());
    if (!direct.empty()) {
        if (!units.empty()) {
            auto* heading = new wxStaticText(this, wxID_ANY, _L("Direct"));
            heading->SetFont(::Label::Body_12);
            m_sizer->Add(heading, 0, wxBOTTOM, gap / 2);
        }
        auto* rows = tile_rows(direct.size());
        for (size_t i : direct)
            rows->Add(m_tiles[i]);
        m_sizer->Add(rows, 0);
    }

    SelectExtruder(m_selected_extruder < (int) extruder_count ? m_selected_extruder : -1);

    const wxSize content = m_sizer->GetMinSize();
    const int    cap     = wxDisplay(this).GetClientArea().GetHeight() * MAX_SCREEN_SHARE_PERCENT / 100;
    const int    bar     = content.y > cap ? wxSystemSettings::GetMetric(wxSYS_VSCROLL_X, this) : 0;
    SetMinSize(wxSize(content.x + bar, std::min(content.y, cap)));
    FitInside();
    Layout();
    InvalidateBestSize();
    if (GetParent()) GetParent()->Layout();
}

void SlotGridPanel::SelectExtruder(int extruder)
{
    m_selected_extruder = extruder;
    for (size_t e = 0; e < m_extruders.size(); ++e)
        m_extruders[e]->SetSelected((int) e == extruder);
    for (size_t i = 0; i < m_tiles.size(); ++i)
        if (m_tiles[i] != nullptr)
            m_tiles[i]->SetWashed(extruder >= 0 && m_slots[i].extruder != extruder);
}

void SlotGridPanel::OnExtruderClicked(int extruder)
{
    SelectExtruder(extruder == m_selected_extruder ? -1 : extruder); // click again to clear
}

void SlotGridPanel::OnSlotClicked(size_t slot_index)
{
    if (m_on_slot) m_on_slot(slot_index);
}

std::vector<SlotGridSlot> slot_grid_rows(const FilamentInventory& inv, const PresetCollection& filaments)
{
    std::vector<SlotGridSlot> rows;
    rows.reserve(inv.slots.size());
    for (const PhysicalFilament& pf : inv.slots) {
        SlotGridSlot row;
        row.empty        = pf.empty();
        row.type         = wxString::FromUTF8(pf.type);
        row.unit         = wxString::FromUTF8(pf.unit);
        row.unit_label   = wxString::FromUTF8(pf.unit_label);
        row.slot         = pf.slot;
        row.extruder     = pf.extruder;
        row.virtual_tool = pf.virtual_tool;
        row.colour       = colour_or(pf.color, EMPTY_FILL);
        std::vector<wxString> parts;
        if (const std::string preset = slot_display_name(pf, filaments); !preset.empty())
            parts.push_back(wxString::FromUTF8(preset));
        if (const Preset* p = filaments.find_preset(pf.preset, false); p != nullptr)
            if (const std::string vendor = filament_vendor_of(*p); !vendor.empty())
                parts.push_back(wxString::FromUTF8(vendor));
        if (!pf.name.empty()) parts.push_back(wxString::FromUTF8(pf.name));
        if (!pf.unit.empty()) parts.push_back(wxString::FromUTF8(pf.unit_label.empty() ? pf.unit : pf.unit_label));
        if (pf.extruder >= 0) parts.push_back(wxString::Format("E%d", pf.extruder + 1));
        if (pf.empty()) parts.insert(parts.begin(), _L("No filament loaded"));
        for (size_t i = 0; i < parts.size(); ++i)
            row.tooltip += (i == 0 ? "" : wxString::FromUTF8(" \xc2\xb7 ")) + parts[i];
        rows.push_back(std::move(row));
    }
    return rows;
}

wxColour colour_or(const std::string& hex, const wxColour& fallback)
{
    try {
        return Hex2Color(hex);
    } catch (const std::exception&) {
        return fallback;
    }
}

SlotPickPopup::SlotPickPopup(wxWindow* parent) : PopupWindow(parent, wxBORDER_NONE)
{
    SetBackgroundColour(*wxWHITE);
    SlotGridOptions opts;
    opts.pick_mode = true;
    m_grid         = new SlotGridPanel(this, opts);
    m_grid->SetOnSlotClicked([this](size_t i) {
        Dismiss();
        if (m_on_pick) m_on_pick(i);
    });
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_grid, 0, wxALL, FromDIP(8));
    SetSizer(sizer);
    wxGetApp().UpdateDarkUIWin(this);
}

void SlotPickPopup::Rebuild(const std::vector<SlotGridSlot>& slots, std::function<void(size_t)> on_pick)
{
    m_on_pick = std::move(on_pick);
    m_grid->SetSlots(slots, 1);
    GetSizer()->SetSizeHints(this);
    Layout();
}

void SlotPickPopup::PopupBelow(wxWindow* anchor)
{
    wxPoint pos = anchor->ClientToScreen(wxPoint(0, 0));
    pos.y += anchor->GetRect().height;
    // Kept on screen: lifted off the bottom edge, shifted in from the right one.
    const wxRect area = wxDisplay(anchor).GetClientArea();
    if (pos.y + GetSize().y > area.GetBottom())
        pos.y = std::max(area.GetTop(), area.GetBottom() - GetSize().y);
    if (pos.x + GetSize().x > area.GetRight())
        pos.x = std::max(area.GetLeft(), area.GetRight() - GetSize().x);
    Move(pos);
    // Wayland drops the connection when a popup is mapped while the anchor's tooltip is topmost
    // (GDK: "Tried to map a popup with a non-top most parent").
    CallAfter([this] {
        if (!IsShown())
            Popup();
    });
}

} // namespace GUI
} // namespace Slic3r
