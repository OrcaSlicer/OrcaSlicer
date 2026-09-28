#include "SlotGridPanel.hpp"

#include <algorithm>
#include <map>

#include <wx/dcbuffer.h>
#include <wx/dcgraph.h>
#include <wx/dcmemory.h>
#include <wx/statbox.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/tooltip.h>

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

const wxColour EMPTY_FILL(0xD9, 0xD9, 0xD9);
const wxColour EMPTY_STROKE(0x9A, 0x9A, 0x9A);
const wxColour TEXT_DARK(0x26, 0x2E, 0x30);
const wxColour TEXT_DISABLED(0xCE, 0xCE, 0xCE);
const wxColour BORDER_IDLE("#DBDBDB");
const wxColour BORDER_SELECTED("#009688"); // Orca accent teal

// Auto-contrast off the tile's own absolute colour -- NOT through StateColor::darkModeColorFor:
// the text sits on the filament colour, which does not change with the theme (see
// FilamentInventoryEditor's card for the field report that taught this).
wxColour text_colour_for(const wxColour& fill) { return fill.GetLuminance() < 0.6 ? *wxWHITE : TEXT_DARK; }

} // namespace

// One physical slot.
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
            m_edit_btn->SetBackgroundColour(*wxWHITE); // on the badge doRender draws behind it
            m_edit_btn->Bind(wxEVT_BUTTON, [on_edit = std::move(on_edit)](wxCommandEvent&) { if (on_edit) on_edit(); });
            // Hidden, not merely disabled: a disabled ScalableButton looks identical to an enabled
            // one on these coloured tiles, and MSW never delivers mouse events (so no tooltip) to
            // a disabled control.
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
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { wxPaintDC dc(this); render(dc); });
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
            // A wrong-type tile stays enabled (a disabled control shows no tooltip on Windows);
            // the click is swallowed here instead.
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
    // Everything through a wxGCDC so the alpha wash and the anti-aliased diagonal draw the same
    // on every backend (the MSW GDI DC has neither); on MSW through an offscreen bitmap first,
    // the flicker mitigation every custom tile in AmsMappingPopup.cpp uses.
    void render(wxPaintDC& dc)
    {
        const wxSize size = GetSize();
#ifdef __WXMSW__
        wxMemoryDC memdc;
        wxBitmap   bmp(size.x, size.y);
        memdc.SelectObject(bmp);
        {
            wxGCDC gc(memdc);
            gc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
            gc.Clear();
            doRender(gc);
        }
        memdc.SelectObject(wxNullBitmap);
        dc.DrawBitmap(bmp, 0, 0);
#else
        wxGCDC gc(dc);
        gc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
        gc.Clear();
        doRender(gc);
#endif
    }

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

        // The badge behind the pencil, so it reads on every filament colour (field report).
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

        // The virtual tool the printer maps the slot to now; the slot's own index when unknown.
        // 1-indexed, like every number the user sees.
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

        // The filter: slots on another extruder fade rather than vanish, so the layout holds.
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

// One physical extruder: E<n> and how many slots feed it. Click to filter.
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
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { wxPaintDC dc(this); render(dc); });
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) { if (m_on_click) m_on_click(); });
    }

    void SetSelected(bool selected)
    {
        if (m_selected == selected) return;
        m_selected = selected;
        Refresh();
    }

private:
    void render(wxPaintDC& dc)
    {
        const wxSize size = GetSize();
#ifdef __WXMSW__
        wxMemoryDC memdc;
        wxBitmap   bmp(size.x, size.y);
        memdc.SelectObject(bmp);
        {
            wxGCDC gc(memdc);
            doRender(gc);
        }
        memdc.SelectObject(wxNullBitmap);
        dc.DrawBitmap(bmp, 0, 0);
#else
        wxGCDC gc(dc);
        doRender(gc);
#endif
    }

    void doRender(wxGCDC& gc)
    {
        const wxSize size = GetSize();
        gc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
        gc.Clear();
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

SlotGridPanel::SlotGridPanel(wxWindow* parent, const SlotGridOptions& options) : wxPanel(parent, wxID_ANY), m_options(options)
{
    SetBackgroundColour(parent->GetBackgroundColour());
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

    // Tiles are created in inventory order (m_tiles[i] is slot i); the sizers then place them by
    // unit and position.
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

    for (const wxString& unit : units) {
        const std::vector<size_t> members = ordered(unit);
        const wxString title = members.empty() || slots[members.front()].unit_label.IsEmpty() ? unit : slots[members.front()].unit_label;
        auto* box = new wxStaticBoxSizer(wxHORIZONTAL, this, title);
        for (size_t i : members) {
            m_tiles[i]->Reparent(box->GetStaticBox());
            box->Add(m_tiles[i], 0, wxALL, gap / 2);
        }
        m_sizer->Add(box, 0, wxBOTTOM, gap);
    }
    const std::vector<size_t> direct = ordered(wxString());
    if (!direct.empty()) {
        if (!units.empty()) {
            auto* heading = new wxStaticText(this, wxID_ANY, _L("Direct"));
            heading->SetFont(::Label::Body_12);
            m_sizer->Add(heading, 0, wxBOTTOM, gap / 2);
        }
        wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
        for (size_t i : direct)
            row->Add(m_tiles[i], 0, wxRIGHT, gap);
        m_sizer->Add(row, 0);
    }

    SelectExtruder(m_selected_extruder < (int) extruder_count ? m_selected_extruder : -1);
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
        row.name         = wxString::FromUTF8(pf.name);
        row.unit         = wxString::FromUTF8(pf.unit);
        row.unit_label   = wxString::FromUTF8(pf.unit_label);
        row.slot         = pf.slot;
        row.extruder     = pf.extruder;
        row.virtual_tool = pf.virtual_tool;
        row.colour       = EMPTY_FILL;
        if (!pf.color.empty()) {
            try {
                row.colour = Hex2Color(pf.color);
            } catch (const std::exception&) {
                row.colour = EMPTY_FILL;
            }
        }
        // Everything the tile leaves out: preset, vendor, slot name, unit, extruder.
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

SlotPickDialog::SlotPickDialog(wxWindow* parent, const std::vector<SlotGridSlot>& slots, size_t extruder_count, int current)
    : wxDialog(parent, wxID_ANY, _L("Pick the printer's filament"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
{
    SetBackgroundColour(*wxWHITE);
    wxGetApp().UpdateDlgDarkUI(this);
    SlotGridOptions opts;
    opts.pick_mode             = true;
    opts.show_extruder_row     = true;
    opts.show_extruder_on_tile = true;
    auto* grid = new SlotGridPanel(this, opts);
    grid->SetOnSlotClicked([this](size_t i) {
        m_picked = (int) i;
        EndModal(wxID_OK);
    });
    grid->SetSlots(slots, extruder_count);
    // Start filtered on the current pick's extruder, so the swap-and-purge cost of choosing
    // another slot on the same extruder is in view from the first click.
    if (current >= 0 && (size_t) current < slots.size() && slots[current].extruder >= 0)
        grid->SelectExtruder(slots[current].extruder);
    auto* hint = new wxStaticText(this, wxID_ANY,
                                  _L("Click an extruder to see its slots, then a slot. Two filaments on one extruder are swapped and purged during the print."));
    hint->SetFont(::Label::Body_12);
    hint->Wrap(FromDIP(420));
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(hint, 0, wxALL, FromDIP(12));
    sizer->Add(grid, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    SetSizerAndFit(sizer);
    SetEscapeId(wxID_CANCEL);
    CenterOnParent();
}

} // namespace GUI
} // namespace Slic3r
