#include "CurveEditorPanel.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>
#include <wx/brush.h>
#include <wx/colour.h>
#include <wx/dc.h>
#include <wx/dcbuffer.h>
#include <wx/gdicmn.h>
#include <wx/pen.h>
#include <wx/peninfobase.h>
#include <wx/string.h>
#include "GUI_App.hpp"
#include "Widgets/StateColor.hpp"

namespace Slic3r::GUI {

void CurveEditorPanel::paint_chart()
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(GetBackgroundColour()));
    dc.Clear();
    dc.SetTextForeground(GetForegroundColour());
    dc.SetFont(GetFont());
    const wxRect plot = chart_rect(&dc);
    const int left = plot.x, top = plot.y, width = plot.width, height = plot.height;
    if (width <= 0 || height <= 0)
        return;
    const int gap = FromDIP(8), tick_size = FromDIP(2), label_gap = FromDIP(2);
    const int bottom = top + height;
    const bool dark = wxGetApp().dark_mode();
    const wxColour colour("#009688");
    const wxColour accent(dark ? "#223C3C" : "#BFE1DE");
    std::vector<wxPoint> curve;
    std::vector<double> curve_factors;
    if (!m_x.empty() && m_interpolate) {
        // Share interpolation samples between the fill and the curve outline.
        curve.reserve(width + 1);
        curve_factors.reserve(width + 1);
        for (int pixel = 0; pixel <= width; ++pixel) {
            const double x = m_view.min_x + (m_view.max_x - m_view.min_x) * (double(pixel) / width);
            const double y = m_interpolate(x);
            if (!std::isfinite(y)) {
                curve.clear();
                break;
            }
            curve.emplace_back(left + pixel, chart_point(x, y, plot).y);
            curve_factors.push_back(y);
        }
        auto blend = [](const wxColour& foreground, const wxColour& background, double alpha) {
            return wxColour(wxColour::AlphaBlend(foreground.Red(), background.Red(), alpha),
                            wxColour::AlphaBlend(foreground.Green(), background.Green(), alpha),
                            wxColour::AlphaBlend(foreground.Blue(), background.Blue(), alpha));
        };
        // Preblend with the background: plain wxDC alpha drawing differs across platforms.
        // Tint follows Y in the configured color range; the grid remains readable above it.
        // Neighbouring pixel columns of one colour are filled together, as one polygon stepped along them.
        const wxColour background = GetBackgroundColour();
        std::vector<wxPoint> area;
        wxColour area_colour;
        auto fill_area = [&] {
            if (area.empty()) return;
            const int first = area.front().x, last = area.back().x;
            area.emplace_back(last, bottom);
            area.emplace_back(first, bottom);
            dc.SetBrush(wxBrush(area_colour));
            dc.DrawPolygon(int(area.size()), area.data());
            area.clear();
        };
        dc.SetPen(*wxTRANSPARENT_PEN);
        for (size_t i = 0; i < curve.size(); ++i) {
            const double factor = m_appearance.fill_max > m_appearance.fill_min ?
                std::clamp((curve_factors[i] - m_appearance.fill_min) / (m_appearance.fill_max - m_appearance.fill_min), 0.0, 1.0) : 0.0;
            const wxColour fill = blend(blend(accent, colour, factor), background, dark ? 0.22 : 0.14);
            if (!area.empty() && fill != area_colour) fill_area();
            area_colour = fill;
            const int y = std::clamp(curve[i].y, top, bottom);
            area.emplace_back(curve[i].x, y);
            area.emplace_back(curve[i].x + 1, y);
        }
        fill_area();
    }
    dc.DrawText(m_appearance.y_label, left, 0);
    const wxString& x_label = m_appearance.x_label;
    dc.DrawText(x_label, left + (width - dc.GetTextExtent(x_label).x) / 2,
                bottom + tick_size + 2 * gap + dc.GetCharHeight());
    const wxPen grid_pen(StateColor::darkModeColorFor(wxColour("#DBDBDB")), 1);
    const wxPen axis_pen(GetForegroundColour(), 1);
    for (const auto& tick : axis_ticks(m_view.min_x, m_view.max_x, width, dc, true, gap)) {
        const int x = chart_point(tick.value, m_view.min_y, plot).x;
        dc.SetPen(grid_pen);
        dc.DrawLine(x, top, x, bottom);
        dc.SetPen(axis_pen);
        dc.DrawLine(x, bottom, x, bottom + tick_size);
        const int label_width = dc.GetTextExtent(tick.label).x;
        const int label_x = std::clamp(x - label_width / 2, left, std::max(left, left + width - label_width));
        dc.DrawText(tick.label, label_x, bottom + tick_size + gap);
    }
    const double reference = m_appearance.reference_y.value_or(0.0);
    const bool reference_visible = m_appearance.reference_y && m_view.min_y <= reference && m_view.max_y >= reference;
    const int reference_y = chart_point(m_view.min_x, reference, plot).y;
    for (const auto& tick : axis_ticks(m_view.min_y, m_view.max_y, height, dc, false, gap)) {
        const int y = chart_point(m_view.min_x, tick.value, plot).y;
        // Reserve space for the reference label even when it is not a regular tick.
        if (reference_visible && std::abs(y - reference_y) < dc.GetCharHeight() + gap)
            continue;
        dc.SetPen(grid_pen);
        dc.DrawLine(left, y, left + width, y);
        dc.SetPen(axis_pen);
        dc.DrawLine(left - tick_size, y, left, y);
        const wxSize extent = dc.GetTextExtent(tick.label);
        dc.DrawText(tick.label, left - tick_size - label_gap - extent.x, y - extent.y / 2);
    }
    // Reference lines belong to the model, independently of the automatic tick step.
    if (reference_visible) {
        dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour("#808080")), FromDIP(2), wxPENSTYLE_SHORT_DASH));
        dc.DrawLine(left, reference_y, left + width, reference_y);
        dc.SetPen(axis_pen);
        dc.DrawLine(left - tick_size, reference_y, left, reference_y);
        const wxSize reference_size = dc.GetTextExtent(m_appearance.reference_label);
        dc.DrawText(m_appearance.reference_label, left - tick_size - label_gap - reference_size.x, reference_y - reference_size.y / 2);
    }
    dc.SetPen(wxPen(GetForegroundColour()));
    dc.DrawLine(left, top, left, top + height);
    dc.DrawLine(left, top + height, left + width, top + height);
    if (m_x.empty())
        return;

    dc.SetPen(wxPen(colour, FromDIP(2)));
    if (curve.size() > 1) {
        wxDCClipper clip(dc, wxRect(left, top, width + 1, height + 1));
        dc.DrawLines(int(curve.size()), curve.data());
    }
    for (size_t i = 0; i < m_x.size(); ++i) {
        if (!point_visible(m_x[i], m_y[i]))
            continue;
        const bool selected = int(i) == m_selected_point;
        const bool active = int(i) == m_dragged_point || int(i) == m_hovered_point;
        const wxPoint position = chart_point(m_x[i], m_y[i], plot);
        dc.SetPen(wxPen(colour, FromDIP(2)));
        dc.SetBrush(wxBrush(selected ? colour : GetBackgroundColour()));
        dc.DrawCircle(position, FromDIP(selected ? 5 : 4));
        if (active) {
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.SetPen(wxPen(colour, FromDIP(2)));
            dc.DrawCircle(position, FromDIP(6));
        }
    }
    const int tooltip_point = m_dragged_point >= 0 ? m_dragged_point : m_hovered_point;
    if (tooltip_point >= 0 && tooltip_point < int(m_x.size())) {
        const wxString& text = m_tooltips[tooltip_point];
        const wxSize text_size = dc.GetMultiLineTextExtent(text);
        const wxSize box_size = text_size + wxSize(2 * gap, 2 * gap);
        const wxPoint position = chart_point(m_x[tooltip_point], m_y[tooltip_point], plot);
        const wxSize client_size = GetClientSize();
        const int x = std::clamp(position.x + 2 * gap, gap, std::max(gap, client_size.x - box_size.x - gap));
        int y = position.y - box_size.y - 2 * gap;
        if (y < gap)
            y = position.y + 2 * gap;
        y = std::clamp(y, gap, std::max(gap, client_size.y - box_size.y - gap));
        dc.SetBrush(wxBrush(StateColor::darkModeColorFor(wxColour("#F1F1F1"))));
        dc.SetPen(wxPen(accent, 1));
        dc.DrawRoundedRectangle(wxRect(wxPoint(x, y), box_size), FromDIP(4));
        dc.DrawLabel(text, wxRect(wxPoint(x + gap, y + gap), text_size));
    }
}

} // namespace Slic3r::GUI
