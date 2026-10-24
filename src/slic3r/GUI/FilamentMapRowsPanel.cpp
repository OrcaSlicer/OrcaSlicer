#include "FilamentMapRowsPanel.hpp"

#include "FilamentMapPanel.hpp" // wxEVT_INVALID_MANUAL_MAP
#include "AmsMappingPopup.hpp"  // MaterialSyncItem
#include "ActivePrinterSession.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Widgets/StateColor.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <wx/colour.h>
#include <wx/gdicmn.h>
#include <wx/dc.h>
#include <wx/font.h>
#include <vector>
#include <string>
#include <libslic3r/FilamentInventory.hpp>
#include <cstddef>
#include <wx/panel.h>
#include <wx/chartype.h>
#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/event.h>
#include <slic3r/GUI/Widgets/Label.hpp>
#include <slic3r/GUI/SlotGridPanel.hpp>
#include <wx/wrapsizer.h>

#include <algorithm>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace Slic3r { namespace GUI {

static const wxColour TextNormalGreyColor = wxColour("#6B6B6B");

// Opaque: MaterialSyncItem::doRender paints an alpha-0 colour as transparent-filament checkerboard.
static const wxColour PlaceholderColour = wxColour(0xCE, 0xCE, 0xCE);

// Wider than messure_size()'s 65dip so the corner badge clears the centered label.
static constexpr int TILE_WIDTH_DIP = 82;

// messure_size() resets the width to 65dip on construction and on every set_ams_info/
// reset_ams_info, so call this after each.
static void WidenTile(MaterialSyncItem *tile)
{
    const wxSize target(tile->FromDIP(TILE_WIDTH_DIP), tile->GetSize().GetHeight());
    tile->SetMinSize(target);
    tile->SetMaxSize(target);
    tile->SetSize(target);
}

// Paints the auto-matched corner badge and the shared-extruder outline onto the tile itself;
// a child window would paint a square over the tile's header.
class BadgedSyncItem : public MaterialSyncItem
{
public:
    using MaterialSyncItem::MaterialSyncItem;

    void set_auto_matched(bool matched)
    {
        if (m_auto_matched == matched) return;
        m_auto_matched = matched;
        Refresh();
    }

    void set_shared_extruder(bool shared)
    {
        if (m_shared_extruder == shared) return;
        m_shared_extruder = shared;
        SetToolTip(shared ? _L("Two filaments on the same extruder: the printer will swap and purge between them.") : wxString());
        Refresh();
    }

protected:
    void doRender(wxDC &dc) override
    {
        MaterialSyncItem::doRender(dc);
        if (m_shared_extruder) {
            const wxSize size = GetSize();
            dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour("#F5A623")), FromDIP(2)));
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.DrawRoundedRectangle(1, 1, size.x - 2, size.y - 2, FromDIP(4));
        }
        if (!m_auto_matched) return;

        static constexpr int BADGE_SIZE_DIP = 16;
        const wxSize size  = GetSize();
        const int    badge = FromDIP(BADGE_SIZE_DIP);
        const int    x0    = size.x - badge;

        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(StateColor::darkModeColorFor(wxColour("#009688")))); // Orca accent teal
        wxPoint triangle[3] = {wxPoint(x0, 0), wxPoint(size.x, 0), wxPoint(size.x, badge)};
        dc.DrawPolygon(3, triangle);

        wxFont font = GetFont();
        font.SetPointSize(std::max(6, font.GetPointSize() - 3));
        font.SetWeight(wxFONTWEIGHT_BOLD);
        dc.SetFont(font);
        dc.SetTextForeground(StateColor::darkModeColorFor(*wxWHITE));
        const wxString label     = "A";
        const wxSize   text_size = dc.GetTextExtent(label);
        // Placed in the triangle's visual mass (top-right), not its geometric center.
        dc.DrawText(label, size.x - text_size.GetWidth() - FromDIP(2), FromDIP(1));
    }

private:
    bool m_auto_matched{false};
    bool m_shared_extruder{false};
};

FilamentMapRowsPanel::FilamentMapRowsPanel(wxWindow                       *parent,
                                           const std::vector<std::string> &filament_colors,
                                           const std::vector<std::string> &filament_types,
                                           const std::vector<std::string> &filament_names,
                                           const std::vector<int>         &plate_filaments,
                                           const FilamentInventory        &inventory,
                                           size_t                          tool_count)
    : wxPanel(parent)
    , m_filament_types(filament_types)
    , m_filament_count(filament_colors.size())
    , m_tool_count(tool_count)
{
    SetName(wxT("FilamentMapRowsPanel"));
    SetBackgroundColour(*wxWHITE);
    m_memm = tool_count > 1 && !inventory.dialect.empty();

    // Must agree with compute_physical_map_proposal, which proposes nothing then.
    m_bootstrap_mode = inventory_all_unset(inventory);
    m_options        = slot_grid_rows(inventory, wxGetApp().preset_bundle->filaments);
    for (const PhysicalFilament &pf : inventory.slots)
        m_option_ids.push_back(pf.empty() ? 0 : pf.id);
    if (m_options.size() < m_tool_count) {
        SlotGridSlot empty_slot;
        empty_slot.empty   = true;
        empty_slot.tooltip = _L("No filament loaded");
        m_options.resize(m_tool_count, empty_slot);
        m_option_ids.resize(m_tool_count, 0);
    }

    auto *top_sizer = new wxBoxSizer(wxVERTICAL);
    // Not wxWRAPSIZER_DEFAULT_FLAGS: wxEXTEND_LAST_ON_EACH_LINE would stretch fixed-size tiles.
    auto *tiles_sizer = new wxWrapSizer(wxHORIZONTAL, wxREMOVE_LEADING_SPACES);

    m_rows.reserve(plate_filaments.size());
    for (size_t plate_index = 0; plate_index < plate_filaments.size(); ++plate_index) {
        const int f = plate_filaments[plate_index];
        if (f < 1 || f > (int) filament_colors.size()) continue;

        wxString mname = from_u8(filament_types[f - 1]);

        auto *tile = new BadgedSyncItem(this, colour_or(filament_colors[f - 1], PlaceholderColour), mname);
        WidenTile(tile);
        tile->set_material_index_str(std::to_string(f));
        wxString full_name = filament_names.size() > (size_t) (f - 1) ? from_u8(filament_names[f - 1]) : wxString();
        tile->SetToolTip(full_name.IsEmpty() ? mname : (mname + " - " + full_name));

        tiles_sizer->Add(tile, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));

        const size_t row_index = m_rows.size();
        m_rows.push_back({plate_index, f, tile, -1, false});
        ApplySelectionToTile(row_index);

        tile->Bind(wxEVT_LEFT_DOWN, [this, row_index](wxMouseEvent &) { OnTileClicked(row_index); });
    }

    top_sizer->Add(tiles_sizer, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(4));

    top_sizer->AddSpacer(FromDIP(10));
    m_footer = new Label(this, Label::Body_13);
    m_footer->SetForegroundColour(TextNormalGreyColor);
    top_sizer->Add(m_footer, 0, wxALIGN_LEFT);

    m_warning_label = new Label(this, Label::Body_13);
    m_warning_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#FF6F00")));
    m_warning_label->Hide();
    top_sizer->Add(m_warning_label, 0, wxALIGN_LEFT | wxTOP, FromDIP(2));

    SetSizer(top_sizer);
    Layout();
    Fit();

    UpdateFooter();

    GUI::wxGetApp().UpdateDarkUIWin(this);
}

void FilamentMapRowsPanel::ApplyProposal(const std::vector<int> &proposal)
{
    for (size_t row_index = 0; row_index < m_rows.size(); ++row_index) {
        Row      &row  = m_rows[row_index];
        const int prop = row.plate_index < proposal.size() ? proposal[row.plate_index] : 0;
        const auto it  = prop > 0 ? std::find(m_option_ids.begin(), m_option_ids.end(), prop) : m_option_ids.end();
        row.selected_index = it != m_option_ids.end() ? (int) (it - m_option_ids.begin()) : -1;
        row.auto_matched   = row.selected_index >= 0;
        ApplySelectionToTile(row_index);
    }
    UpdateFooter();
}

std::vector<SlotGridSlot> FilamentMapRowsPanel::PickerRows(const Row &row, bool allow_bare_pick) const
{
    std::string row_type; // "" = no restriction
    if (row.filament_id >= 1 && row.filament_id <= (int) m_filament_types.size())
        row_type = m_filament_types[row.filament_id - 1];

    std::vector<SlotGridSlot> rows = m_options;
    for (size_t i = 0; i < rows.size(); ++i) {
        SlotGridSlot     &r         = rows[i];
        const std::string slot_type = into_u8(r.type);
        r.checked  = (int) i == row.selected_index;
        r.disabled = !allow_bare_pick && m_option_ids[i] <= 0;
        if (r.disabled)
            r.tooltip = _L("No filament is loaded in this slot -- load one, or sync from the printer, before selecting it.");
        r.wrong_type = !r.disabled && m_option_ids[i] > 0 && !row_type.empty() && !slot_type.empty() &&
                       !Slic3r::type_compatible(row_type, slot_type);
        if (r.wrong_type)
            r.tooltip = wxString::Format(_L("Material mismatch: this row prints %s but the slot holds %s. Only matching material families can be mapped."),
                                         from_u8(row_type), r.type);
    }
    return rows;
}

void FilamentMapRowsPanel::OnTileClicked(size_t row_index)
{
    if (row_index >= m_rows.size()) return;
    Row &row = m_rows[row_index];

    // Offline, an unset slot is unknown rather than known empty.
    const bool allow_bare_pick = m_bootstrap_mode || !active_printer_session().live();
    const std::vector<SlotGridSlot> rows = PickerRows(row, allow_bare_pick);

    if (m_memm) {
        SlotPickDialog dlg(this, rows, m_tool_count, row.selected_index);
        if (dlg.ShowModal() == wxID_OK && dlg.Picked() >= 0)
            ApplyPick(row_index, dlg.Picked());
        return;
    }

    if (!m_picker_popup)
        m_picker_popup = new SlotPickPopup(this);
    if (m_picker_popup->IsShown())
        return;
    m_picker_popup->Rebuild(rows, [this, row_index](size_t idx) { ApplyPick(row_index, (int) idx); });
    m_picker_popup->PopupBelow(row.tile);
}

void FilamentMapRowsPanel::ApplyPick(size_t row_index, int option_index)
{
    if (row_index >= m_rows.size()) return;
    m_rows[row_index].selected_index = option_index;
    m_rows[row_index].auto_matched   = false;
    ApplySelectionToTile(row_index);
    UpdateFooter();

    wxCommandEvent evt(wxEVT_INVALID_MANUAL_MAP);
    evt.SetInt(AllRowsAssigned() ? 1 : 0);
    ProcessEvent(evt);
}

void FilamentMapRowsPanel::ApplySelectionToTile(size_t row_index)
{
    if (row_index >= m_rows.size()) return;
    Row &row = m_rows[row_index];

    if (row.selected_index >= 0 && (size_t) row.selected_index < m_options.size()) {
        const SlotGridSlot &opt = m_options[row.selected_index];
        wxString label = wxString::Format("T%d", (opt.virtual_tool >= 0 ? opt.virtual_tool : row.selected_index) + 1);
        if (m_memm && opt.extruder >= 0)
            label += wxString::Format(" E%d", opt.extruder + 1);
        row.tile->set_ams_info(opt.empty ? PlaceholderColour : opt.colour, label);
    } else {
        row.tile->reset_ams_info();
    }
    WidenTile(row.tile);
    row.tile->Refresh();

    // Every row tile is a BadgedSyncItem.
    static_cast<BadgedSyncItem *>(row.tile)->set_auto_matched(row.selected_index >= 0 && row.auto_matched);
    UpdateSharedExtruderMarks();
}

void FilamentMapRowsPanel::UpdateSharedExtruderMarks()
{
    // Without a reported topology, each slot is its own extruder.
    std::vector<int> extruder_of_row(m_rows.size(), -1);
    std::unordered_map<int, int> rows_on_extruder;
    for (size_t i = 0; i < m_rows.size(); ++i) {
        const int sel = m_rows[i].selected_index;
        if (sel < 0 || (size_t) sel >= m_options.size()) continue;
        extruder_of_row[i] = m_options[sel].extruder >= 0 ? m_options[sel].extruder : sel;
        ++rows_on_extruder[extruder_of_row[i]];
    }
    for (size_t i = 0; i < m_rows.size(); ++i)
        static_cast<BadgedSyncItem *>(m_rows[i].tile)->set_shared_extruder(extruder_of_row[i] >= 0 && rows_on_extruder[extruder_of_row[i]] > 1);
}

bool FilamentMapRowsPanel::ResolveRow(const Row &row, int &tool, int &id) const
{
    int sel = row.selected_index;
    if (sel < 0 || (size_t) sel >= m_options.size())
        return false;
    tool = sel;
    id   = m_option_ids[sel];
    return true;
}

bool FilamentMapRowsPanel::RowTargetsEmptyTool(const Row &row) const
{
    if (m_bootstrap_mode) return false;
    int sel = row.selected_index;
    if (sel < 0 || (size_t) sel >= m_options.size())
        return false;
    return m_option_ids[sel] <= 0;
}

wxString FilamentMapRowsPanel::EmptyToolWarningText() const
{
    std::set<int> empty_tools;
    for (const Row &row : m_rows)
        if (RowTargetsEmptyTool(row))
            empty_tools.insert(row.selected_index);
    if (empty_tools.empty())
        return wxString();

    wxString tool_list;
    for (int t : empty_tools) {
        if (!tool_list.empty()) tool_list += ", ";
        tool_list += wxString::Format(_L("Tool %d"), t + 1);
    }
    // TRN %s is a comma-separated list of tools, e.g. "Tool 1, Tool 3".
    return wxString::Format(_L_PLURAL("%s has no filament loaded", "%s have no filament loaded", empty_tools.size()), tool_list);
}

void FilamentMapRowsPanel::ComputeStats(int &idle, int &swaps, int &merged) const
{
    std::vector<int>                    assigned_counts(m_tool_count, 0);
    std::vector<std::unordered_set<int>> tool_ids(m_tool_count);
    std::unordered_map<int, int>        id_counts;
    int synthetic_id = 0;

    for (const Row &row : m_rows) {
        int tool, id;
        if (!ResolveRow(row, tool, id)) continue;
        if (tool < 0 || tool >= (int) m_tool_count) continue;
        ++assigned_counts[tool];
        if (m_bootstrap_mode) continue;
        if (id <= 0) {
            // Bare picks share no identity: never merged, each a distinct target on its tool.
            tool_ids[tool].insert(--synthetic_id);
            continue;
        }
        tool_ids[tool].insert(id);
        ++id_counts[id];
    }

    idle  = 0;
    swaps = 0;
    for (size_t t = 0; t < m_tool_count; ++t) {
        if (assigned_counts[t] == 0) { ++idle; continue; }
        int distinct_targets = m_bootstrap_mode ? assigned_counts[t] : (int) tool_ids[t].size();
        swaps += std::max(0, distinct_targets - 1);
    }

    merged = 0;
    if (!m_bootstrap_mode)
        for (const auto &kv : id_counts)
            merged += std::max(0, kv.second - 1);
}

void FilamentMapRowsPanel::UpdateFooter()
{
    for (Row &row : m_rows) {
        int tool, id;
        if (ResolveRow(row, tool, id) && !RowTargetsEmptyTool(row))
            row.tile->on_normal();
        else
            row.tile->on_warning();
    }

    int idle, swaps, merged;
    ComputeStats(idle, swaps, merged);

    wxString text;
    auto append_stat = [&text](const wxString &segment) {
        if (segment.empty()) return;
        // FromUTF8: a raw non-ASCII literal becomes mojibake on Windows.
        if (!text.empty()) text += wxString::FromUTF8(" · ");
        text += segment;
    };
    if (idle > 0) append_stat(wxString::Format(_L_PLURAL("%d tool idle", "%d tools idle", idle), idle));
    if (swaps > 0) append_stat(wxString::Format(_L_PLURAL("%d swap on this plate", "%d swaps on this plate", swaps), swaps));
    // TRN %d is the number of project filaments that share a spool with another filament.
    if (merged > 0) append_stat(wxString::Format(_L_PLURAL("%d merged", "%d merged", merged), merged));
    m_footer->SetLabel(text);

    if (m_warning_label) {
        wxString combined       = m_footer_warning;
        wxString empty_tool_txt = EmptyToolWarningText();
        if (!empty_tool_txt.empty()) {
            if (!combined.empty()) combined += "\n";
            combined += empty_tool_txt;
        }
        m_warning_label->SetLabel(combined);
        // Wrap() edits the current label, so it follows every SetLabel. Before the first layout
        // the panel has no width yet.
        if (!combined.empty()) {
            int wrap_w = GetClientSize().GetWidth();
            if (wrap_w < FromDIP(200))
                wrap_w = FromDIP(520);
            m_warning_label->Wrap(wrap_w);
        }
        m_warning_label->Show(!combined.empty());
    }

    Layout();
    // No Fit(): the wrap sizer's CalcMin assumes one tile per row. The host view re-fits.
    if (GetParent())
        GetParent()->Layout();
}

void FilamentMapRowsPanel::SetFooterWarning(const wxString &text)
{
    m_footer_warning = text;
    UpdateFooter();
}

std::vector<int> FilamentMapRowsPanel::GetFilamentMaps() const
{
    std::vector<int> result(m_filament_count, 0);

    for (const Row &row : m_rows) {
        int tool, id;
        if (!ResolveRow(row, tool, id)) continue;
        if (tool < 0 || tool >= (int) m_tool_count) continue;
        if (row.filament_id >= 1 && row.filament_id <= (int) result.size())
            result[row.filament_id - 1] = tool + 1;
    }
    return result;
}

std::vector<int> FilamentMapRowsPanel::GetPhysicalMaps() const
{
    std::vector<int> result(m_filament_count, 0);
    for (const Row &row : m_rows) {
        int tool, id;
        if (!ResolveRow(row, tool, id)) continue;
        if (row.filament_id >= 1 && row.filament_id <= (int) result.size())
            result[row.filament_id - 1] = id > 0 ? id : 0;
    }
    return result;
}

bool FilamentMapRowsPanel::AllRowsAssigned() const
{
    return std::all_of(m_rows.begin(), m_rows.end(), [this](const Row &row) {
        int tool, id;
        return ResolveRow(row, tool, id) && !RowTargetsEmptyTool(row);
    });
}

}} // namespace Slic3r::GUI
