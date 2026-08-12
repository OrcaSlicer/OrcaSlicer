#include "GradientCurveEditor.hpp"
#include "GUI_App.hpp"
#include "GuiColor.hpp"
#include "I18N.hpp"
#include "Widgets/StateColor.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include <wx/dcbuffer.h>
#include <wx/dcclient.h>
#include <wx/dcgraph.h>
#include <wx/settings.h>

namespace Slic3r {
namespace GUI {

wxDEFINE_EVENT(wxEVT_GRADIENT_CURVE_CHANGED, wxCommandEvent);

namespace {
// Layout (Figma "Property 1=Default", 214.06 x 179.63 px reference).
// Plot rect occupies the upper-left region; right + bottom margins host axis arrows / labels.
constexpr double kPlotLeftRatio   = 0.0316;
constexpr double kPlotRightRatio  = 0.6766;
constexpr double kPlotTopRatio    = 0.1529;
constexpr double kPlotBottomRatio = 0.8474;
constexpr int    kGridDivisions   = 9;     // 10 grid lines including the outer borders.

// Hit / stroke (DIP).
constexpr int kHitRadius        = 6;
constexpr int kCurveHitRadius   = 5;
constexpr int kPointRadius      = 4;   // anchor outer radius (DIP)
constexpr int kStrokeUnselected = 2;
constexpr int kStrokeSelected   = 4;
constexpr int kStrokeAxis       = 2;   // axis line width (px, no DPI scaling - matches kGridColor pen and 2DBed convention)
constexpr int kAxisArrowHalf    = 5;   // half-base of the axis arrow triangle (DIP)
constexpr int kAxisArrowLen     = 10;  // length of the axis arrow triangle (DIP)

// Light-mode design tokens from Figma. Resolved through StateColor::darkModeColorFor()
// at paint time so the editor follows the app theme (#EEEEEE -> #4C4C55, #6B6B6B ->
// #818183, #262E30 -> #EFEFF0, *wxWHITE -> #2D2D31). Don't read these directly in paint;
// always go through the resolved locals declared at the top of on_paint().
const wxColour kGridColor   (238, 238, 238);   // #EEEEEE grey 300
const wxColour kAxisColor   (107, 107, 107);   // #6B6B6B grey 700
const wxColour kLabelMuted  (107, 107, 107);   // #6B6B6B grey 700
const wxColour kLabelStrong ( 38,  46,  48);   // #262E30 grey 900

// LAB (DeltaE76) threshold for "curve color is too close to the background". Below this
// we paint a subtle axis-color outline so the curve doesn't visually vanish; above this
// we draw the curve plain. ~15 is "perceptible but still close", looser than the strict
// 5.0 used by FlushPredict::is_similar_color but loose enough that a pastel pink on white
// or a charcoal on #2B2B2B still triggers an outline.
constexpr float kBgSimilarThreshold = 15.0f;
constexpr int   kOutlineExtraDip    = 2;
} // namespace

GradientCurveEditor::GradientCurveEditor(wxWindow* parent,
                                         const wxColour& color_low,
                                         const wxColour& color_high)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
    , m_color_low(color_low)
    , m_color_high(color_high)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetBackgroundColour(wxGetApp().get_window_default_clr());
    // Wide enough so the X-axis "Material Ratio" label fits past the arrow tip without overlap.
    // 260 (was 240): adds room for the "Material Ratio" label that gets shifted right by the
    // longer axis arrow; the hosting MixedFilamentDialog grows to 470 DIP to accommodate.
    SetMinSize(FromDIP(wxSize(260, 200)));

    reset_to_linear(0.10, 0.90);

    Bind(wxEVT_PAINT,       &GradientCurveEditor::on_paint,      this);
    Bind(wxEVT_LEFT_DOWN,   &GradientCurveEditor::on_left_down,  this);
    Bind(wxEVT_LEFT_UP,     &GradientCurveEditor::on_left_up,    this);
    Bind(wxEVT_RIGHT_DOWN,  &GradientCurveEditor::on_right_down, this);
    Bind(wxEVT_MOTION,      &GradientCurveEditor::on_motion,     this);
    Bind(wxEVT_LEAVE_WINDOW,&GradientCurveEditor::on_leave,      this);
    Bind(wxEVT_SIZE,        &GradientCurveEditor::on_size,       this);
    Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) {
        m_drag_mode     = DragMode::None;
        m_drag_idx      = -1;
        m_dragged_moved = false;
    });
}

void GradientCurveEditor::set_points(const PointList& pts)
{
    m_points = pts;
    normalize_points();
    Refresh();
}

void GradientCurveEditor::set_colors(const wxColour& color_low, const wxColour& color_high)
{
    m_color_low  = color_low;
    m_color_high = color_high;
    Refresh();
}

void GradientCurveEditor::set_selected_curve(int curve_idx)
{
    const int new_sel = (curve_idx == 0) ? 0 : 1;
    if (m_selected_curve == new_sel) return;
    m_selected_curve = new_sel;
    Refresh();
}

void GradientCurveEditor::reset_to_linear(double y0, double y1)
{
    auto clamp_y = [](double v) {
        return std::max(kGradientMinRatio, std::min(kGradientMaxRatio, v));
    };
    m_points.clear();
    GradientAnchor a0; a0.x = 0.0; a0.y = clamp_y(y0);
    GradientAnchor a1; a1.x = 1.0; a1.y = clamp_y(y1);
    m_points.push_back(a0);
    m_points.push_back(a1);
    m_selected_curve = 0;
    Refresh();
    emit_changed();
}

void GradientCurveEditor::reverse()
{
    // Mirror y around 0.5. Tangents are slopes dy/dx so they flip sign to keep the
    // local shape consistent across the mirror; NaN tangents remain "use PCHIP default".
    for (auto& p : m_points) {
        p.y = 1.0 - p.y;
        if (std::isfinite(p.m_in))  p.m_in  = -p.m_in;
        if (std::isfinite(p.m_out)) p.m_out = -p.m_out;
    }
    Refresh();
    emit_changed();
}

void GradientCurveEditor::normalize_points()
{
    if (m_points.empty()) {
        GradientAnchor a0; a0.x = 0.0; a0.y = kGradientMinRatio;
        GradientAnchor a1; a1.x = 1.0; a1.y = kGradientMaxRatio;
        m_points.push_back(a0);
        m_points.push_back(a1);
        return;
    }

    for (auto& p : m_points) {
        p.x = std::max(0.0, std::min(1.0, p.x));
        p.y = std::max(kGradientMinRatio, std::min(kGradientMaxRatio, p.y));
    }
    std::sort(m_points.begin(), m_points.end(),
              [](const GradientAnchor& a, const GradientAnchor& b) {
                  return a.x < b.x;
              });

    if (m_points.size() < 2) {
        GradientAnchor tail; tail.x = 1.0; tail.y = m_points.front().y;
        m_points.push_back(tail);
    }

    m_points.front().x = 0.0;
    m_points.back().x  = 1.0;
}

void GradientCurveEditor::emit_changed()
{
    wxCommandEvent evt(wxEVT_GRADIENT_CURVE_CHANGED, GetId());
    evt.SetEventObject(this);
    ProcessWindowEvent(evt);
}

wxRect GradientCurveEditor::plot_rect() const
{
    const wxSize sz = GetClientSize();
    const int x  = static_cast<int>(std::lround(sz.x * kPlotLeftRatio));
    const int y  = static_cast<int>(std::lround(sz.y * kPlotTopRatio));
    const int x2 = static_cast<int>(std::lround(sz.x * kPlotRightRatio));
    const int y2 = static_cast<int>(std::lround(sz.y * kPlotBottomRatio));
    // Force square 1:1 so X/Y axes share the same scale and grid cells stay square. Anchor at
    // the top-left so the "100%" labels on the bottom/right still align with the plot edges.
    const int side = std::max(1, std::min(x2 - x, y2 - y));
    return wxRect(x, y, side, side);
}

wxPoint GradientCurveEditor::data_to_px(double x, double y) const
{
    const wxRect r = plot_rect();
    const int px = r.x + static_cast<int>(std::lround(x * r.width));
    // y axis is inverted: y=1 should sit at the top.
    const int py = r.y + static_cast<int>(std::lround((1.0 - y) * r.height));
    return wxPoint(px, py);
}

void GradientCurveEditor::px_to_data(int px, int py, double& x, double& y) const
{
    const wxRect r = plot_rect();
    const double w = std::max(1, r.width);
    const double h = std::max(1, r.height);
    x = std::max(0.0, std::min(1.0, (px - r.x) / w));
    y = std::max(0.0, std::min(1.0, 1.0 - (py - r.y) / h));
}

double GradientCurveEditor::sample_curve_y(double x) const
{
    GradientCurve gc;
    gc.points = m_points;
    return sample_gradient_curve(gc, x);
}

int GradientCurveEditor::hit_test(int px, int py) const
{
    const int tol = FromDIP(kHitRadius);
    int best_idx = -1;
    int best_d2  = tol * tol;
    for (size_t i = 0; i < m_points.size(); ++i) {
        // Anchor visual y is curve-specific: component 1's anchor sits at (x, 1 - stored_y).
        const double vy = to_visual_y(m_selected_curve, m_points[i].y);
        const wxPoint p = data_to_px(m_points[i].x, vy);
        const int dx = px - p.x;
        const int dy = py - p.y;
        const int d2 = dx * dx + dy * dy;
        if (d2 <= best_d2) {
            best_idx = static_cast<int>(i);
            best_d2  = d2;
        }
    }
    return best_idx;
}

int GradientCurveEditor::hit_test_curve(int px, int py, int* seg_out) const
{
    if (seg_out) *seg_out = -1;
    if (m_points.size() < 2) return -1;
    const int tol  = FromDIP(kCurveHitRadius);
    const int tol2 = tol * tol;

    auto dist2_to_seg = [&](int ax, int ay, int bx, int by) -> int {
        const double dx = bx - ax;
        const double dy = by - ay;
        const double l2 = dx * dx + dy * dy;
        if (l2 == 0.0) {
            const double ddx = px - ax;
            const double ddy = py - ay;
            return static_cast<int>(ddx * ddx + ddy * ddy);
        }
        double t = ((px - ax) * dx + (py - ay) * dy) / l2;
        t = std::max(0.0, std::min(1.0, t));
        const double ex = ax + t * dx;
        const double ey = ay + t * dy;
        const double ddx = px - ex;
        const double ddy = py - ey;
        return static_cast<int>(ddx * ddx + ddy * ddy);
    };

    // Hit-test against the same dense Hermite polyline that on_paint draws, so the
    // clickable line follows the visual curve exactly (no offset on the bent parts).
    // When a hit is found, also report the index of the left anchor of the data-space
    // segment that covers cursor x; needed by the segment-bend interaction.
    const wxRect rc = plot_rect();
    const int samples = std::max(128, rc.width * 2);
    auto seg_for_x = [&](double cursor_x) -> int {
        for (size_t i = 1; i < m_points.size(); ++i) {
            if (cursor_x <= m_points[i].x)
                return static_cast<int>(i - 1);
        }
        return static_cast<int>(m_points.size() - 2);
    };

    auto curve_hit = [&](int curve_idx) -> bool {
        wxPoint prev;
        for (int s = 0; s <= samples; ++s) {
            const double x  = double(s) / samples;
            const double y0 = sample_curve_y(x);
            const double vy = to_visual_y(curve_idx, y0);
            const wxPoint cur = data_to_px(x, vy);
            if (s > 0 && dist2_to_seg(prev.x, prev.y, cur.x, cur.y) <= tol2)
                return true;
            prev = cur;
        }
        return false;
    };

    // Prefer the selected curve so overlapping segments don't unintentionally steal focus.
    if (curve_hit(m_selected_curve)) {
        if (seg_out) {
            double nx = 0, dummy = 0;
            px_to_data(px, py, nx, dummy);
            *seg_out = seg_for_x(nx);
        }
        return m_selected_curve;
    }
    const int other = 1 - m_selected_curve;
    if (curve_hit(other)) {
        if (seg_out) {
            double nx = 0, dummy = 0;
            px_to_data(px, py, nx, dummy);
            *seg_out = seg_for_x(nx);
        }
        return other;
    }
    return -1;
}

void GradientCurveEditor::on_paint(wxPaintEvent& /*evt*/)
{
    // Resolve theme colors every paint so dark-mode toggles (no re-construction) take
    // effect without an explicit listener. Window bg is read from GUI_App, not
    // GetBackgroundColour(), since the latter is snapshotted at construction time.
    const wxColour bg            = wxGetApp().get_window_default_clr();
    const wxColour grid_color    = StateColor::darkModeColorFor(kGridColor);
    const wxColour axis_color    = StateColor::darkModeColorFor(kAxisColor);
    const wxColour label_muted   = StateColor::darkModeColorFor(kLabelMuted);
    const wxColour label_strong  = StateColor::darkModeColorFor(kLabelStrong);
    const wxColour point_fill    = StateColor::darkModeColorFor(*wxWHITE);

    wxAutoBufferedPaintDC raw_dc(this);
    raw_dc.SetBackground(wxBrush(bg));
    raw_dc.Clear();

    // Render through wxGCDC so curves, arrows and anchor circles get anti-aliased; the buffered
    // DC is the actual back buffer that gets blitted to the window.
    wxGCDC dc(raw_dc);

    const wxRect rc = plot_rect();
    if (rc.width <= 0 || rc.height <= 0)
        return;

    // 10x10 light grid (10 lines including outer borders, 9 equal divisions).
    dc.SetPen(wxPen(grid_color, 1));
    for (int i = 0; i <= kGridDivisions; ++i) {
        const int x = rc.x + rc.width  * i / kGridDivisions;
        const int y = rc.y + rc.height * i / kGridDivisions;
        dc.DrawLine(x, rc.y, x, rc.y + rc.height);
        dc.DrawLine(rc.x, y, rc.x + rc.width, y);
    }

    // Set the label font first so text width measurements drive arrow / label placement.
    wxFont label_font = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
    label_font.SetPointSize(std::max(7, label_font.GetPointSize() - 1));
    dc.SetFont(label_font);

    const wxString axis_y_title = _L("Material Ratio");
    const wxString axis_x_title = _L("Model Height");
    const wxString pct_text     = wxT("100%");
    const wxSize   x_title_sz   = dc.GetTextExtent(axis_x_title);
    const wxSize   y_title_sz   = dc.GetTextExtent(axis_y_title);

    wxFont strong_font = label_font;
    strong_font.SetWeight(wxFONTWEIGHT_SEMIBOLD);
    dc.SetFont(strong_font);
    const wxSize pct_text_sz = dc.GetTextExtent(pct_text);
    dc.SetFont(label_font);

    // Axes (grey 700) with filled triangle arrows. Y-axis extends above the plot top to the
    // canvas top edge; X-axis extends past the plot right toward the canvas right edge.
    const int arrow_half = FromDIP(kAxisArrowHalf);
    const int arrow_len  = FromDIP(kAxisArrowLen);
    const wxSize sz      = GetClientSize();
    dc.SetPen(wxPen(axis_color, kStrokeAxis));
    dc.SetBrush(wxBrush(axis_color));

    // Y-axis: vertical line at plot_left, from arrow tip near canvas top down to plot bottom.
    const int y_axis_x   = rc.x;
    const int y_title_pct_gap = FromDIP(1);
    const int y_title_bottom_pad = FromDIP(2);
    const int y_title_y = std::max(0, rc.y - y_title_sz.y - y_title_pct_gap - pct_text_sz.y - y_title_bottom_pad);
    const int y_arrow_tip_y = y_title_y;
    const int y_arrow_ty = y_arrow_tip_y + arrow_len;
    dc.DrawLine(y_axis_x, y_arrow_ty, y_axis_x, rc.y + rc.height);
    {
        wxPoint tri[3] = {
            wxPoint(y_axis_x,              y_arrow_tip_y),
            wxPoint(y_axis_x - arrow_half, y_arrow_ty),
            wxPoint(y_axis_x + arrow_half, y_arrow_ty),
        };
        dc.DrawPolygon(3, tri);
    }

    // X-axis arrow tip: stays just past the plot ideally, but is clamped so the trailing
    // "Material Ratio" label still fits inside the canvas without overlapping the arrow.
    const int x_axis_y       = rc.y + rc.height;
    const int x_label_gap    = FromDIP(4);
    const int x_edge_pad     = FromDIP(6);
    const int x_arrow_ideal  = rc.x + rc.width + FromDIP(10);
    const int x_arrow_max    = sz.x - x_title_sz.x - x_label_gap - x_edge_pad - arrow_len;
    const int x_arrow_tx     = std::max(rc.x + rc.width + arrow_len,
                                        std::min(x_arrow_ideal, x_arrow_max));
    const int x_arrow_tip_x  = x_arrow_tx + arrow_len;
    const int x_title_x      = x_arrow_tip_x + x_label_gap;
    dc.DrawLine(rc.x, x_axis_y, x_arrow_tx, x_axis_y);
    {
        wxPoint tri[3] = {
            wxPoint(x_arrow_tip_x,          x_axis_y),
            wxPoint(x_arrow_tx,             x_axis_y - arrow_half),
            wxPoint(x_arrow_tx,             x_axis_y + arrow_half),
        };
        dc.DrawPolygon(3, tri);
    }

    // Labels.
    // "Model Height" and "100%" share the same left x; the gap is larger than the
    // axis-arrow half-base so the text never visually touches the Y-axis arrow.
    const int label_left_x = y_axis_x + FromDIP(10);
    dc.SetTextForeground(label_muted);
    dc.DrawText(axis_y_title, label_left_x, y_title_y);

    dc.SetFont(strong_font);
    dc.SetTextForeground(label_strong);
    dc.DrawText(pct_text, label_left_x, y_title_y + y_title_sz.y + y_title_pct_gap);

    // Bottom-right "100%" sits under the right end of the plot; "Material Ratio" follows the
    // X-axis arrow tip (placement was already clamped above to leave room).
    dc.DrawText(pct_text, rc.x + rc.width - pct_text_sz.x, x_axis_y);
    dc.SetFont(label_font);
    dc.SetTextForeground(label_muted);
    dc.DrawText(axis_x_title, x_title_x, x_axis_y - x_title_sz.y / 2);

    if (m_points.size() < 2)
        return;

    auto color_for_curve = [&](int curve_idx) -> wxColour {
        wxColour c = (curve_idx == 0) ? m_color_low : m_color_high;
        // Transparent filaments (alpha == 0, e.g. #FFFFFF00) would be invisible.
        // Lift alpha so the curve stays visible while still hinting at transparency.
        if (c.Alpha() == 0)
            c.Set(c.Red(), c.Green(), c.Blue(), 150);
        return c;
    };

    auto build_polyline = [&](int curve_idx) -> std::vector<wxPoint> {
        const int samples = std::max(128, rc.width * 2);
        std::vector<wxPoint> poly;
        poly.reserve(samples + 1);
        for (int s = 0; s <= samples; ++s) {
            const double x  = double(s) / samples;
            const double y0 = sample_curve_y(x);
            const double vy = to_visual_y(curve_idx, y0);
            poly.push_back(data_to_px(x, vy));
        }
        return poly;
    };

    auto draw_polyline = [&](const std::vector<wxPoint>& poly, const wxColour& col, int stroke_dip) {
        dc.SetPen(wxPen(col, FromDIP(stroke_dip)));
        dc.DrawLines(static_cast<int>(poly.size()), poly.data());
    };

    // Outline only when the curve color is perceptually close to the background; otherwise
    // the plain filament color reads fine and the extra stroke would look heavy.
    // Outline tone is intentionally softer than axis_color so it disambiguates the curve
    // from the bg without competing with the structural axis/grid: light mode uses a pale
    // grey, dark mode uses a slightly-above-bg grey (gDarkColors has no entry for these).
    const wxColour outline_color = wxGetApp().dark_mode()
        ? wxColour(90,  90,  94)    // > bg #2B2B2B, < axis #818183
        : wxColour(200, 200, 200);  // > grid #EEEEEE, < axis #6B6B6B
    auto needs_outline = [&](const wxColour& c) {
        return calc_color_distance(c, bg) < kBgSimilarThreshold;
    };

    auto draw_one = [&](int curve_idx, int stroke_dip) {
        const auto       poly = build_polyline(curve_idx);
        const wxColour   col  = color_for_curve(curve_idx);
        if (needs_outline(col))
            draw_polyline(poly, outline_color, stroke_dip + kOutlineExtraDip);
        draw_polyline(poly, col, stroke_dip);
    };

    // Draw unselected first so the selected curve sits on top.
    const int other = 1 - m_selected_curve;
    draw_one(other,            kStrokeUnselected);
    draw_one(m_selected_curve, kStrokeSelected);

    // Control points (selected curve only): hollow circle with axis-color border, theme-aware fill.
    const int r = FromDIP(kPointRadius);
    dc.SetPen(wxPen(axis_color, 1));
    dc.SetBrush(wxBrush(point_fill));
    for (size_t i = 0; i < m_points.size(); ++i) {
        const double vy = to_visual_y(m_selected_curve, m_points[i].y);
        const wxPoint p = data_to_px(m_points[i].x, vy);
        dc.DrawCircle(p.x, p.y, r);
    }
}

void GradientCurveEditor::on_left_down(wxMouseEvent& evt)
{
    const wxPoint pos = evt.GetPosition();
    m_dragged_moved = false;

    // 1) Anchor on the selected curve takes precedence over everything else.
    //    Dragging an anchor resets its tangent overrides so the surrounding curve
    //    returns to PCHIP-default shape (matches user expectation that pulling an
    //    anchor "straightens out" the local mess).
    const int idx = hit_test(pos.x, pos.y);
    if (idx >= 0) {
        m_drag_mode = DragMode::Anchor;
        m_drag_idx  = idx;
        // Only emit a change event when clearing the tangents actually mutates
        // the curve. A plain click on an already-default anchor must not trigger
        // re-slicing through the changed-event listener.
        const bool had_tangent = std::isfinite(m_points[idx].m_in)
                              || std::isfinite(m_points[idx].m_out);
        m_points[idx].m_in  = std::numeric_limits<double>::quiet_NaN();
        m_points[idx].m_out = std::numeric_limits<double>::quiet_NaN();
        if (!HasCapture())
            CaptureMouse();
        Refresh();
        if (had_tangent)
            emit_changed();
        return;
    }

    // 2) Line-body hit. Determine which curve and which segment.
    int seg = -1;
    const int curve_hit = hit_test_curve(pos.x, pos.y, &seg);
    if (curve_hit < 0) {
        m_drag_mode = DragMode::None;
        evt.Skip();
        return;
    }

    // 3) Non-selected curve hit -> switch selection only, no drag arming.
    if (curve_hit != m_selected_curve) {
        m_selected_curve = curve_hit;
        m_drag_mode      = DragMode::None;
        Refresh();
        evt.Skip();
        return;
    }

    // 4) Selected curve line body hit -> insert a new anchor at cursor x (snapped
    //    to the current smooth curve so the initial click is visually invisible)
    //    and immediately enter Anchor drag mode. PS Curves style: the drag-bend
    //    interaction has no separate "bend without anchor" mode; pressing and
    //    dragging on the line is equivalent to clicking to add then dragging the
    //    fresh anchor. Trades the previous (failed) "no anchor on drag" promise
    //    for genuine cursor tracking, since a single cubic between two existing
    //    anchors mathematically cannot put its peak under an off-center cursor.
    double nx = 0, dummy = 0;
    px_to_data(pos.x, pos.y, nx, dummy);
    if (nx <= 0.0 || nx >= 1.0 || seg < 0) {
        m_drag_mode = DragMode::None;
        evt.Skip();
        return;
    }
    GradientAnchor a;
    a.x = nx;
    a.y = sample_curve_y(nx);
    const size_t insert_idx = static_cast<size_t>(seg) + 1;
    m_points.insert(m_points.begin() + insert_idx, a);

    m_drag_mode = DragMode::Anchor;
    m_drag_idx  = static_cast<int>(insert_idx);
    if (!HasCapture())
        CaptureMouse();
    Refresh();
    emit_changed();
}

void GradientCurveEditor::on_left_up(wxMouseEvent& evt)
{
    if (HasCapture())
        ReleaseMouse();

    // Anchor mode (either an existing anchor or one freshly inserted by on_left_down)
    // already fired emit_changed on mouse_down; only fire again here if the user
    // actually dragged so the slicer doesn't re-run on a pure click.
    if (m_drag_mode == DragMode::Anchor && m_dragged_moved)
        emit_changed();

    m_drag_mode     = DragMode::None;
    m_drag_idx      = -1;
    m_dragged_moved = false;
    (void)evt;
}

void GradientCurveEditor::on_right_down(wxMouseEvent& evt)
{
    const wxPoint pos = evt.GetPosition();
    const int idx = hit_test(pos.x, pos.y);
    if (idx > 0 && static_cast<size_t>(idx) + 1 < m_points.size()) {
        // Interior anchor on the selected curve -> delete it. Endpoints stay locked.
        m_points.erase(m_points.begin() + idx);
        Refresh();
        emit_changed();
        return;
    }
    // Right-click on the non-selected curve switches selection (never deletes).
    const int curve_hit = hit_test_curve(pos.x, pos.y);
    if (curve_hit >= 0 && curve_hit != m_selected_curve) {
        m_selected_curve = curve_hit;
        Refresh();
        return;
    }
    evt.Skip();
}

void GradientCurveEditor::on_motion(wxMouseEvent& evt)
{
    if (!evt.LeftIsDown() || m_drag_mode != DragMode::Anchor) {
        evt.Skip();
        return;
    }
    if (static_cast<size_t>(m_drag_idx) >= m_points.size())
        return;

    const wxPoint pos = evt.GetPosition();
    double nx = 0, vy = 0;
    px_to_data(pos.x, pos.y, nx, vy);

    auto& p = m_points[m_drag_idx];
    const bool is_first = (m_drag_idx == 0);
    const bool is_last  = (static_cast<size_t>(m_drag_idx) + 1 == m_points.size());

    // Endpoints stay locked at x=0 / x=1; interior anchors clamp into
    // (left_neighbor.x, right_neighbor.x) so they can't cross or coincide.
    if (!is_first && !is_last) {
        const double xl = m_points[m_drag_idx - 1].x;
        const double xr = m_points[m_drag_idx + 1].x;
        const double eps = 1e-4;
        nx = std::max(xl + eps, std::min(xr - eps, nx));
        p.x = nx;
    }
    // y is constrained to the reserved blend band so neither component ever
    // reaches 0% / 100%, matching the sampler's clamp.
    p.y = std::max(kGradientMinRatio,
                   std::min(kGradientMaxRatio, to_stored_y(m_selected_curve, vy)));
    m_dragged_moved = true;
    Refresh();
}

void GradientCurveEditor::on_leave(wxMouseEvent& evt)
{
    evt.Skip();
}

void GradientCurveEditor::on_size(wxSizeEvent& evt)
{
    Refresh();
    evt.Skip();
}

} // namespace GUI
} // namespace Slic3r
