#include "ToolheadLaneOverview.hpp"

#include "Label.hpp"
#include "StateColor.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"

#include <wx/control.h>
#include <wx/dcbuffer.h>
#include <wx/dcgraph.h>

#include <algorithm>
#include <utility>

wxDEFINE_EVENT(EVT_TOOLHEAD_LANE_OVERVIEW_SELECT, wxCommandEvent);

namespace Slic3r { namespace GUI {

namespace {

// Geometry in DIPs, top to bottom: material text, lane chips, paths, nozzle, toolhead label.
constexpr int TEXT_TOP      = 4;
constexpr int CHIP_TOP      = 20;
constexpr int CHIP_W        = 18;
constexpr int CHIP_H        = 26;
constexpr int CHIP_GAP      = 6;
constexpr int MERGE_Y       = 64;
constexpr int NOZZLE_TOP    = 72;
constexpr int NOZZLE_H      = 16;
constexpr int NOZZLE_TOP_W  = 22;
constexpr int NOZZLE_TIP_W  = 8;
constexpr int LABEL_TOP     = 92;
constexpr int TOTAL_H       = 112;
constexpr int COLUMN_MIN_W  = 58;
constexpr int COLUMN_PAD    = 8;
constexpr int COLUMN_GAP    = 6;

const wxColour ACCENT("#009688");

wxColour text_colour() { return wxGetApp().dark_mode() ? wxColour("#EFEFF0") : wxColour("#262E30"); }
wxColour idle_colour() { return wxGetApp().dark_mode() ? wxColour("#5A5A5E") : wxColour("#C8C8C8"); }
wxColour selected_fill() { return wxGetApp().dark_mode() ? wxColour("#33393B") : wxColour("#E6F4F2"); }

} // namespace

ToolheadLaneOverview::ToolheadLaneOverview(wxWindow* parent)
{
    // The paint handler clears and covers every pixel.
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    wxWindow::Create(parent, wxID_ANY);
    SetCursor(wxCursor(wxCURSOR_HAND));

    Bind(wxEVT_PAINT, &ToolheadLaneOverview::on_paint, this);
    Bind(wxEVT_LEFT_DOWN, &ToolheadLaneOverview::on_left_down, this);
    Bind(wxEVT_MOTION, &ToolheadLaneOverview::on_motion, this);
    update_min_size();
}

void ToolheadLaneOverview::SetToolheads(std::vector<Toolhead> toolheads, int active_toolhead, int selected_toolhead)
{
    const auto same_lane = [](const Lane& a, const Lane& b) {
        return a.colour == b.colour && a.material == b.material && a.name == b.name && a.loaded == b.loaded &&
               a.external == b.external;
    };
    const bool same = toolheads.size() == m_toolheads.size() && active_toolhead == m_active && selected_toolhead == m_selected &&
                      std::equal(toolheads.begin(), toolheads.end(), m_toolheads.begin(), [&](const Toolhead& a, const Toolhead& b) {
                          return std::equal(a.begin(), a.end(), b.begin(), b.end(), same_lane);
                      });
    if (same)
        return;

    m_toolheads      = std::move(toolheads);
    m_active         = active_toolhead;
    m_selected       = selected_toolhead;
    m_tooltip_column = -2;
    update_min_size();
    Refresh();
}

void ToolheadLaneOverview::SetSelectedToolhead(int toolhead)
{
    if (toolhead == m_selected)
        return;
    m_selected = toolhead;
    Refresh();
}

void ToolheadLaneOverview::Rescale()
{
    update_min_size();
    Refresh();
}

int ToolheadLaneOverview::column_width(const Toolhead& toolhead) const
{
    const int lanes = std::max<int>(1, static_cast<int>(toolhead.size()));
    const int chips = lanes * FromDIP(CHIP_W) + (lanes - 1) * FromDIP(CHIP_GAP);
    return std::max(FromDIP(COLUMN_MIN_W), chips + 2 * FromDIP(COLUMN_PAD));
}

wxRect ToolheadLaneOverview::column_rect(int index) const
{
    int x = 0;
    for (int i = 0; i < index; ++i)
        x += column_width(m_toolheads[i]) + FromDIP(COLUMN_GAP);
    return wxRect(x, 0, column_width(m_toolheads[index]), FromDIP(TOTAL_H));
}

int ToolheadLaneOverview::column_at(int x) const
{
    for (int i = 0; i < static_cast<int>(m_toolheads.size()); ++i)
        if (column_rect(i).Contains(x, 1))
            return i;
    return -1;
}

void ToolheadLaneOverview::update_min_size()
{
    int width = 0;
    for (const auto& toolhead : m_toolheads)
        width += column_width(toolhead);
    if (!m_toolheads.empty())
        width += (static_cast<int>(m_toolheads.size()) - 1) * FromDIP(COLUMN_GAP);
    const wxSize size(width, FromDIP(TOTAL_H));
    if (size != GetMinSize()) {
        SetMinSize(size);
        SetSize(size);
        if (GetParent())
            GetParent()->Layout();
    }
}

void ToolheadLaneOverview::on_paint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC paint_dc(this);
    wxGCDC                dc(paint_dc);
    dc.SetBackground(wxBrush(GetParent() ? GetParent()->GetBackgroundColour() : GetBackgroundColour()));
    dc.Clear();

    const wxColour text = text_colour();
    const wxColour idle = idle_colour();

    for (int index = 0; index < static_cast<int>(m_toolheads.size()); ++index) {
        const Toolhead& lanes  = m_toolheads[index];
        const wxRect    column = column_rect(index);
        const int       centre = column.x + column.width / 2;

        if (index == m_selected) {
            dc.SetPen(wxPen(ACCENT, FromDIP(1)));
            dc.SetBrush(wxBrush(selected_fill()));
            dc.DrawRoundedRectangle(column, FromDIP(6));
        }

        // Material above the chips: the lane's own for one lane, else how many are loaded.
        int loaded_count = 0;
        const Lane* only_loaded = nullptr;
        for (const auto& lane : lanes)
            if (lane.loaded) {
                ++loaded_count;
                only_loaded = &lane;
            }
        wxString caption = lanes.size() == 1 ? (lanes[0].loaded ? lanes[0].material : _L("Empty"))
                                             : (loaded_count == 1 ? only_loaded->material
                                                                  : format_wxstr(_L("%1% loaded"), loaded_count));
        dc.SetFont(Label::Body_10);
        dc.SetTextForeground(text);
        caption = wxControl::Ellipsize(caption, dc, wxELLIPSIZE_END, column.width - FromDIP(4));
        const wxSize caption_size = dc.GetTextExtent(caption);
        dc.DrawText(caption, centre - caption_size.x / 2, FromDIP(TEXT_TOP));

        // Lane chips, each with its path to the merge point above the nozzle.
        const int count      = static_cast<int>(lanes.size());
        const int chips_w    = count * FromDIP(CHIP_W) + std::max(0, count - 1) * FromDIP(CHIP_GAP);
        int       chip_x     = centre - chips_w / 2;
        const int merge_y    = FromDIP(MERGE_Y);
        const int nozzle_top = FromDIP(NOZZLE_TOP);
        for (const auto& lane : lanes) {
            const int lane_centre = chip_x + FromDIP(CHIP_W) / 2;
            const int chip_bottom = FromDIP(CHIP_TOP + CHIP_H);

            const wxColour path_colour = lane.loaded ? lane.colour : idle;
            dc.SetPen(wxPen(path_colour, FromDIP(lane.loaded ? 3 : 2)));
            dc.DrawLine(lane_centre, chip_bottom, lane_centre, merge_y);
            dc.DrawLine(lane_centre, merge_y, centre, nozzle_top);

            const wxRect chip(chip_x, FromDIP(CHIP_TOP), FromDIP(CHIP_W), FromDIP(CHIP_H));
            wxPen border(idle, FromDIP(1), lane.external ? wxPENSTYLE_SHORT_DASH : wxPENSTYLE_SOLID);
            dc.SetPen(border);
            dc.SetBrush(lane.loaded ? wxBrush(lane.colour) : *wxTRANSPARENT_BRUSH);
            dc.DrawRoundedRectangle(chip, FromDIP(4));

            chip_x += FromDIP(CHIP_W) + FromDIP(CHIP_GAP);
        }
        if (lanes.empty()) {
            dc.SetPen(wxPen(idle, FromDIP(2)));
            dc.DrawLine(centre, FromDIP(CHIP_TOP), centre, nozzle_top);
        }

        // Nozzle: a downward trapezoid, filled for the printer's active toolhead.
        const bool active = index == m_active;
        const int  half_top = FromDIP(NOZZLE_TOP_W) / 2;
        const int  half_tip = FromDIP(NOZZLE_TIP_W) / 2;
        const wxPoint nozzle[] = {{centre - half_top, nozzle_top},
                                  {centre + half_top, nozzle_top},
                                  {centre + half_tip, nozzle_top + FromDIP(NOZZLE_H)},
                                  {centre - half_tip, nozzle_top + FromDIP(NOZZLE_H)}};
        dc.SetPen(wxPen(active ? ACCENT : idle, FromDIP(1)));
        dc.SetBrush(wxBrush(active ? ACCENT : idle));
        dc.DrawPolygon(4, nozzle);

        wxFont label_font = Label::Body_12;
        if (active)
            label_font.MakeBold();
        dc.SetFont(label_font);
        dc.SetTextForeground(active ? ACCENT : text);
        const wxString label      = wxString::Format("T%d", index + 1);
        const wxSize   label_size = dc.GetTextExtent(label);
        dc.DrawText(label, centre - label_size.x / 2, FromDIP(LABEL_TOP));
    }
}

void ToolheadLaneOverview::on_left_down(wxMouseEvent& event)
{
    const int column = column_at(event.GetX());
    if (column >= 0 && column != m_selected) {
        SetSelectedToolhead(column);
        wxCommandEvent select(EVT_TOOLHEAD_LANE_OVERVIEW_SELECT, GetId());
        select.SetEventObject(this);
        select.SetInt(column);
        ProcessWindowEvent(select);
    }
    event.Skip();
}

void ToolheadLaneOverview::on_motion(wxMouseEvent& event)
{
    const int column = column_at(event.GetX());
    if (column != m_tooltip_column) {
        m_tooltip_column = column;
        if (column < 0) {
            UnsetToolTip();
        } else {
            wxString tip = wxString::Format("T%d", column + 1);
            for (const auto& lane : m_toolheads[column])
                tip += "\n" + lane.name + ": " + (lane.loaded ? lane.material : _L("Empty"));
            SetToolTip(tip);
        }
    }
    event.Skip();
}

}} // namespace Slic3r::GUI
