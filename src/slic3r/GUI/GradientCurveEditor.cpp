#include "GradientCurveEditor.hpp"
#include "FilamentBitmapUtils.hpp"
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
// Hit / stroke (DIP). The plot-rect ratios, grid divisions, axis/arrow geometry and the
// near-background outline threshold now live in FilamentBitmapUtils so the read-only Publish
// preview and this editor stay pixel-identical.
constexpr int kHitRadius        = 6;
constexpr int kCurveHitRadius   = 5;
constexpr int kStrokeUnselected = 2;
constexpr int kStrokeSelected   = 4;

// Light-mode design tokens. Resolved through StateColor::darkModeColorFor()
// at paint time so the editor follows the app theme (#EEEEEE -> #4C4C55, #6B6B6B ->
// #818183, #262E30 -> #EFEFF0, #ACACAC -> #65656A, *wxWHITE -> #2D2D31). Don't read these
// directly in paint; always go through the resolved locals declared at the top of on_paint().
const wxColour kGridColor   (238, 238, 238);   // #EEEEEE grey 300
const wxColour kAxisColor   (107, 107, 107);   // #6B6B6B grey 700
const wxColour kLabelMuted  (107, 107, 107);   // #6B6B6B grey 700
const wxColour kLabelStrong ( 38,  46,  48);   // #262E30 grey 900
const wxColour kOutlineColor(172, 172, 172);   // #ACACAC dimmed elements
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

GradientCurveEditor::~GradientCurveEditor()
{
    // See MixedFilamentDialog::~MixedFilamentDialog: a widget destroyed while it
    // still holds the capture wedges mouse input for the whole application.
    if (HasCapture())
        ReleaseMouse();
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
    // Square 1:1 plot, shared with the Publish dialog's read-only preview.
    return mixed_gradient_plot_rect(GetClientSize());
}

wxPoint2DDouble GradientCurveEditor::data_to_px_f(double x, double y) const
{
    const wxRect r = plot_rect();
    // y axis is inverted: y=1 should sit at the top.
    return wxPoint2DDouble(r.x + x * r.width, r.y + (1.0 - y) * r.height);
}

wxPoint GradientCurveEditor::data_to_px(double x, double y) const
{
    const wxPoint2DDouble p = data_to_px_f(x, y);
    return wxPoint(static_cast<int>(std::lround(p.m_x)), static_cast<int>(std::lround(p.m_y)));
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
    // Softer than axis_color: the curve outline only has to lift the curve off the
    // background, it must not compete with the structural axis / grid.
    const wxColour outline_color = StateColor::darkModeColorFor(kOutlineColor);

    wxAutoBufferedPaintDC raw_dc(this);
    raw_dc.SetBackground(wxBrush(bg));
    raw_dc.Clear();

    // Render the plot (grid, axes, labels, curves, anchors) through the shared painter so the
    // interactive editor and the Publish dialog's read-only preview stay pixel-identical. The
    // curves are handed over as sub-pixel polylines and anti-alias inside the helper.
    std::vector<MixedGradientCurve> curves;
    std::vector<wxPoint2DDouble>    anchors;
    if (m_points.size() >= 2) {
        auto color_for_curve = [&](int curve_idx) -> wxColour {
            wxColour c = (curve_idx == 0) ? m_color_low : m_color_high;
            // Transparent filaments (alpha == 0, e.g. #FFFFFF00) would be invisible.
            if (c.Alpha() == 0)
                c.Set(c.Red(), c.Green(), c.Blue(), 150);
            return c;
        };

        auto build_polyline = [&](int curve_idx) -> std::vector<wxPoint2DDouble> {
            const int samples = std::max(128, plot_rect().width * 2);
            std::vector<wxPoint2DDouble> poly;
            poly.reserve(samples + 1);
            for (int s = 0; s <= samples; ++s) {
                const double x  = double(s) / samples;
                const double y0 = sample_curve_y(x);
                const double vy = to_visual_y(curve_idx, y0);
                poly.push_back(data_to_px_f(x, vy));
            }
            return poly;
        };

        // Draw unselected first so the selected curve sits on top.
        const int other = 1 - m_selected_curve;
        for (const int idx : {other, m_selected_curve}) {
            std::vector<wxPoint2DDouble> pts = build_polyline(idx);
            if (pts.empty())
                continue;
            curves.push_back({std::move(pts), color_for_curve(idx), idx == m_selected_curve ? kStrokeSelected : kStrokeUnselected});
        }

        // Control points (selected curve only).
        anchors.reserve(m_points.size());
        for (size_t i = 0; i < m_points.size(); ++i) {
            const double vy = to_visual_y(m_selected_curve, m_points[i].y);
            anchors.push_back(data_to_px_f(m_points[i].x, vy));
        }
    }

    const MixedGradientTheme theme{bg, grid_color, axis_color, label_muted, label_strong, outline_color, point_fill};
    draw_mixed_gradient_plot(raw_dc, GetClientSize(), curves, anchors, theme);
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
    //    and immediately enter Anchor drag mode. Bending the segment without
    //    inserting an anchor is not an option: a single cubic between two existing
    //    anchors cannot put its peak under an off-center cursor.
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
