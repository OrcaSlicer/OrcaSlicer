#include "CurveEditorPanel.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <wx/dcbuffer.h>
#include "GUI_App.hpp"
#include "Widgets/StateColor.hpp"

namespace Slic3r::GUI {
namespace {

struct AxisTick {
    double value;
    wxString label;
};

std::vector<AxisTick> axis_ticks(double minimum, double maximum, int pixels, wxDC& dc, bool horizontal, int gap)
{
    const double range = maximum - minimum;
    if (!(range > 0.0) || !std::isfinite(range) || pixels <= 0)
        return {};
    const int spacing = horizontal ? dc.GetTextExtent("0000").x + gap : dc.GetCharHeight() + gap;
    const int intervals = std::max(1, pixels / std::max(1, spacing));
    const double target = range / intervals;
    double magnitude = std::pow(10.0, std::floor(std::log10(target)));
    if (magnitude == 0.0)
        magnitude = std::numeric_limits<double>::denorm_min();
    // Increase through 1, 2, 5 x 10^n until the actual labels fit.
    for (int attempt = 0; attempt < 12; ++attempt) {
        for (double multiplier : {1.0, 2.0, 5.0}) {
            const double step = magnitude * multiplier;
            if (step < target)
                continue;
            if (!std::isfinite(step))
                return {};
            std::vector<AxisTick> ticks;
            const double first = std::ceil(minimum / step);
            double previous_end = -std::numeric_limits<double>::infinity();
            bool fits = true;
            for (int i = 0; i <= intervals; ++i) {
                const double value = (first + i) * step;
                if (!std::isfinite(value) || value > maximum)
                    break;
                if (value < minimum)
                    continue;
                const double largest = std::max(std::abs(minimum), std::abs(maximum));
                const int precision = std::clamp(int(std::ceil(std::log10(largest) - std::log10(step))) + 2, 6, 17);
                const wxString label = value == 0.0 ? wxString("0") : wxString::Format("%.*g", precision, value);
                const wxSize extent = dc.GetTextExtent(label);
                const double position = (value - minimum) / range * pixels;
                const int size = horizontal ? extent.x : extent.y;
                if (position - size / 2.0 < previous_end + gap) {
                    fits = false;
                    break;
                }
                previous_end = position + size / 2.0;
                ticks.push_back({value, label});
            }
            if (fits)
                return ticks;
        }
        magnitude *= 10.0;
    }
    return {};
}

} // namespace

CurveEditorPanel::CurveEditorPanel(wxWindow* parent, const CurveEditorAppearance& appearance)
    : wxPanel(parent), m_appearance(appearance)
{
    SetMinSize(FromDIP(wxSize(640, 230)));
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint_chart(); });
    Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        m_hovered_point = -1;
        SetCursor(wxCursor(wxCURSOR_ARROW));
        Refresh();
        event.Skip();
    });
    Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) {
        if (before_drag) before_drag();
        m_dragged_point = hit_test(event.GetPosition());
        if (m_dragged_point < 0) return;
        m_hovered_point = m_dragged_point;
        m_drag_start = event.GetPosition();
        m_drag_previous = m_drag_start;
        m_drag_x = m_x[m_dragged_point];
        m_drag_y = m_y[m_dragged_point];
        select_point(m_dragged_point);
        if (on_select) on_select(m_dragged_point);
        SetFocus();
        CaptureMouse();
    });
    Bind(wxEVT_MOTION, [this](wxMouseEvent& event) {
        if (m_dragged_point >= 0) {
            if (event.LeftIsDown()) drag_point(event.GetPosition());
            else finish_drag();
        } else {
            const int hovered = hit_test(event.GetPosition());
            if (hovered != m_hovered_point) {
                m_hovered_point = hovered;
                Refresh();
            }
            SetCursor(wxCursor(hovered >= 0 ? wxCURSOR_HAND : wxCURSOR_ARROW));
        }
    });
    Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) {
        m_hovered_point = -1;
        if (m_dragged_point < 0) SetCursor(wxCursor(wxCURSOR_ARROW));
        Refresh();
    });
    Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& event) {
        finish_drag();
        m_hovered_point = hit_test(event.GetPosition());
        SetCursor(wxCursor(m_hovered_point >= 0 ? wxCURSOR_HAND : wxCURSOR_ARROW));
        Refresh();
    });
    Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) { finish_drag(); });
}

void CurveEditorPanel::set_data(const std::vector<double>& x, const std::vector<double>& y,
                               const std::vector<wxString>& tooltips, Interpolator interpolate)
{
    wxCHECK_RET(x.size() == y.size() && x.size() == tooltips.size(), "Curve points and tooltips must have equal sizes");
    if (x.size() != m_x.size()) finish_drag();
    m_x = x;
    m_y = y;
    m_tooltips = tooltips;
    m_interpolate = std::move(interpolate);
    if (m_dragged_point < 0) {
        m_hovered_point = -1;
        SetCursor(wxCursor(wxCURSOR_ARROW));
    }
    Refresh();
}

void CurveEditorPanel::set_view(const CurveEditorView& view)
{
    wxCHECK_RET(std::isfinite(view.min_x) && std::isfinite(view.max_x) &&
                std::isfinite(view.min_y) && std::isfinite(view.max_y) &&
                view.min_x < view.max_x && view.min_y < view.max_y &&
                std::isfinite(view.max_x - view.min_x) && std::isfinite(view.max_y - view.min_y), "Invalid curve viewport");
    finish_drag();
    m_view = view;
    m_hovered_point = -1;
    Refresh();
}

void CurveEditorPanel::select_point(int row)
{
    m_selected_point = row;
    Refresh();
}

void CurveEditorPanel::finish_drag()
{
    m_dragged_point = -1;
    if (HasCapture()) ReleaseMouse();
    SetCursor(wxCursor(wxCURSOR_ARROW));
    Refresh();
}

void CurveEditorPanel::drag_point(const wxPoint& position)
{
    if (position == m_drag_previous) return;
    m_drag_previous = position;
    const wxRect plot = chart_rect();
    const wxPoint delta = position - m_drag_start;
    const double x_span = m_view.max_x - m_view.min_x;
    const double y_span = m_view.max_y - m_view.min_y;
    const double x = delta.x == 0 ? m_drag_x : m_view.min_x +
        std::clamp((m_drag_x - m_view.min_x) / x_span + double(delta.x) / plot.width, 0.0, 1.0) * x_span;
    const double y = delta.y == 0 ? m_drag_y : m_view.min_y +
        std::clamp((m_drag_y - m_view.min_y) / y_span - double(delta.y) / plot.height, 0.0, 1.0) * y_span;
    if (on_move) on_move(m_dragged_point, x, y);
}

bool CurveEditorPanel::point_visible(double x, double y) const
{
    return x >= m_view.min_x && x <= m_view.max_x && y >= m_view.min_y && y <= m_view.max_y;
}

wxRect CurveEditorPanel::chart_rect() const
{
    wxClientDC dc(const_cast<CurveEditorPanel*>(this));
    dc.SetFont(GetFont());
    const wxSize size = GetClientSize();
    const int gap = FromDIP(8);
    const int top = dc.GetCharHeight() + gap + dc.GetCharHeight() / 2;
    const int bottom = 2 * dc.GetCharHeight() + 3 * gap;
    const int height = std::max(1, size.y - top - bottom);
    int label_width = dc.GetTextExtent(m_appearance.reference_label).x;
    for (const auto& tick : axis_ticks(m_view.min_y, m_view.max_y, height, dc, false, gap))
        label_width = std::max(label_width, dc.GetTextExtent(tick.label).x);
    int left = label_width + 2 * gap + FromDIP(4);
    int x_label_width = dc.GetTextExtent(wxString::Format("%.6g", m_view.max_x)).x;
    for (const auto& tick : axis_ticks(m_view.min_x, m_view.max_x, std::max(1, size.x - left), dc, true, gap))
        x_label_width = std::max(x_label_width, dc.GetTextExtent(tick.label).x);
    const int right = x_label_width / 2 + gap;
    left = std::max(left, right);
    return wxRect(left, top, std::max(1, size.x - left - right), height);
}

wxPoint CurveEditorPanel::chart_point(double x_value, double y_value, const wxRect& plot) const
{
    // Bound off-screen coordinates before converting to integers; drawing is clipped to the plot.
    const double x = std::clamp((x_value - m_view.min_x) / (m_view.max_x - m_view.min_x), -1.0, 2.0);
    const double y = std::clamp((y_value - m_view.min_y) / (m_view.max_y - m_view.min_y), -1.0, 2.0);
    return wxPoint(plot.x + int(x * plot.width), plot.y + plot.height - int(y * plot.height));
}

int CurveEditorPanel::hit_test(const wxPoint& position) const
{
    const wxRect plot = chart_rect();
    int closest = -1;
    double distance = double(FromDIP(9)) * FromDIP(9);
    for (size_t i = 0; i < m_x.size(); ++i) {
        if (!point_visible(m_x[i], m_y[i]))
            continue;
        const wxPoint delta = position - chart_point(m_x[i], m_y[i], plot);
        const double squared = double(delta.x) * delta.x + double(delta.y) * delta.y;
        if (squared < distance) {
            closest = int(i);
            distance = squared;
        }
    }
    return closest;
}

void CurveEditorPanel::paint_chart()
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(GetBackgroundColour()));
    dc.Clear();
    dc.SetTextForeground(GetForegroundColour());
    dc.SetFont(GetFont());
    const wxRect plot = chart_rect();
    const int left = plot.x, top = plot.y, width = plot.width, height = plot.height;
    if (width <= 0 || height <= 0)
        return;
    const int gap = FromDIP(8), tick_size = FromDIP(4);
    const int bottom = top + height;
    const bool dark = wxGetApp().dark_mode();
    const wxColour colour(dark ? "#52C7B8" : "#007D70");
    const wxColour accent(dark ? "#79B8FF" : "#2463A6");
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
        for (size_t i = 0; i < curve.size(); ++i) {
            const wxPoint& point = curve[i];
            const double factor = m_appearance.fill_max > m_appearance.fill_min ?
                std::clamp((curve_factors[i] - m_appearance.fill_min) / (m_appearance.fill_max - m_appearance.fill_min), 0.0, 1.0) : 0.0;
            const wxColour tint = blend(accent, colour, factor);
            dc.SetPen(wxPen(blend(tint, GetBackgroundColour(), dark ? 0.22 : 0.14), 1));
            dc.DrawLine(point.x, std::clamp(point.y, top, bottom), point.x, bottom);
        }
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
        dc.DrawText(tick.label, x - dc.GetTextExtent(tick.label).x / 2, bottom + tick_size + gap);
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
        dc.DrawText(tick.label, left - tick_size - gap - extent.x, y - extent.y / 2);
    }
    // Reference lines belong to the model, independently of the automatic tick step.
    if (reference_visible) {
        dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour("#808080")), FromDIP(2), wxPENSTYLE_SHORT_DASH));
        dc.DrawLine(left, reference_y, left + width, reference_y);
        dc.SetPen(axis_pen);
        dc.DrawLine(left - tick_size, reference_y, left, reference_y);
        const wxSize reference_size = dc.GetTextExtent(m_appearance.reference_label);
        dc.DrawText(m_appearance.reference_label, left - tick_size - gap - reference_size.x, reference_y - reference_size.y / 2);
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
        dc.SetPen(wxPen(selected ? accent : colour, FromDIP(2)));
        dc.SetBrush(wxBrush(selected ? accent : GetBackgroundColour()));
        dc.DrawCircle(position, FromDIP(selected ? 5 : 4));
        if (active) {
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.SetPen(wxPen(accent, FromDIP(2)));
            dc.DrawCircle(position, FromDIP(8));
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
