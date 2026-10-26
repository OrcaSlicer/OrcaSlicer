#include "DensityCurveGraph.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <utility>

#include <wx/brush.h>
#include <wx/colour.h>
#include <wx/cursor.h>
#include <wx/dcbuffer.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/geometry.h>
#include <wx/graphics.h>
#include <wx/pen.h>
#include <wx/peninfobase.h>
#include <wx/string.h>

#include "libslic3r/libslic3r.h"

#include "../Field.hpp"
#include "../GUI_App.hpp"
#include "Label.hpp"

namespace {

constexpr const char *Accent = "#009688";

// Colours picked when painting, so they follow a theme switch.
wxColour themed(const char *light, const char *dark) { return wxColour(Slic3r::GUI::wxGetApp().dark_mode() ? dark : light); }

} // namespace

DensityCurveGraph::DensityCurveGraph(wxWindow *parent, const wxSize &min_size)
    : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxFULL_REPAINT_ON_RESIZE), m_min_size(min_size)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinSize(FromDIP(m_min_size));
    Bind(wxEVT_PAINT, &DensityCurveGraph::on_paint, this);
    Bind(wxEVT_LEFT_DOWN, &DensityCurveGraph::on_left_down, this);
    Bind(wxEVT_LEFT_UP, &DensityCurveGraph::on_left_up, this);
    Bind(wxEVT_MOTION, &DensityCurveGraph::on_motion, this);
    Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent &e) {
        if (m_drag == None)
            set_hover(None);
        e.Skip();
    });
    // Losing the mouse cancels the drag.
    Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent &) {
        if (m_drag == None)
            return;
        const size_t handle = m_drag;
        m_drag              = None;
        m_x_max             = m_curve.x_max;
        Refresh();
        if (on_drag)
            on_drag({ Drag::Phase::Cancel, handle, 0., 0. });
    });
}

DensityCurveGraph::~DensityCurveGraph()
{
    if (HasCapture())
        ReleaseMouse();
}

void DensityCurveGraph::set_curve(DensityCurve curve)
{
    m_curve = std::move(curve);
    if (m_drag == None)
        m_x_max = m_curve.x_max;
    if (m_hover != None && m_hover >= m_curve.handles.size())
        m_hover = None;
    Refresh();
}

void DensityCurveGraph::set_shade_colour(const wxColour &colour)
{
    m_shade_colour = colour;
    Refresh();
}

void DensityCurveGraph::set_x_digits(int digits)
{
    m_x_digits = digits;
    Refresh();
}

void DensityCurveGraph::rescale()
{
    SetMinSize(FromDIP(m_min_size));
    Refresh();
}

wxRect DensityCurveGraph::plot_rect() const
{
    const wxSize size = GetClientSize();
    const int    left = FromDIP(52), right = FromDIP(30), top = FromDIP(12), bottom = FromDIP(42);
    return wxRect(left, top, std::max(1, size.x - left - right), std::max(1, size.y - top - bottom));
}

double DensityCurveGraph::tick_step() const
{
    for (double step : { 1., 2., 5., 10., 20., 50. })
        if (m_x_max / step <= 12.)
            return step;
    return 100.;
}

double DensityCurveGraph::x_px(double x) const
{
    const wxRect r = plot_rect();
    return r.x + x / m_x_max * r.width;
}

double DensityCurveGraph::y_px(double density) const
{
    const wxRect r = plot_rect();
    return r.y + (1. - density) * r.height;
}

double DensityCurveGraph::x_at(int px) const
{
    const wxRect r = plot_rect();
    return double(px - r.x) / r.width * m_x_max;
}

double DensityCurveGraph::density_at(int py) const
{
    const wxRect r = plot_rect();
    return std::clamp(1. - double(py - r.y) / r.height, 0., 1.);
}

wxPoint2DDouble DensityCurveGraph::handle_pos(const DensityCurve::Handle &handle) const
{
    const wxRect r = plot_rect();
    const double x = x_px(std::clamp(handle.x, 0., m_x_max));
    return handle.kind == DensityCurve::HandleKind::Edge ? wxPoint2DDouble(x, r.y + r.height / 2.) : wxPoint2DDouble(x, y_px(handle.density));
}

size_t DensityCurveGraph::hit(const wxPoint &p) const
{
    // The nearest dot within reach, else an edge within reach.
    size_t best      = None;
    double best_dist = FromDIP(9);
    for (size_t i = 0; i < m_curve.handles.size(); ++i)
        if (m_curve.handles[i].kind != DensityCurve::HandleKind::Edge) {
            const wxPoint2DDouble q    = handle_pos(m_curve.handles[i]);
            const double          dist = std::hypot(q.m_x - p.x, q.m_y - p.y);
            if (dist <= best_dist) {
                best      = i;
                best_dist = dist;
            }
        }
    if (best != None)
        return best;
    const wxRect r = plot_rect();
    if (p.y < r.y || p.y > r.GetBottom())
        return None;
    for (size_t i = 0; i < m_curve.handles.size(); ++i)
        if (m_curve.handles[i].kind == DensityCurve::HandleKind::Edge && std::abs(handle_pos(m_curve.handles[i]).m_x - p.x) <= FromDIP(5))
            return i;
    return None;
}

void DensityCurveGraph::set_hover(size_t handle)
{
    if (handle == m_hover)
        return;
    m_hover = handle;
    if (handle == None)
        SetCursor(wxNullCursor);
    else
        SetCursor(wxCursor(m_curve.handles[handle].kind == DensityCurve::HandleKind::Edge ? wxCURSOR_SIZEWE : wxCURSOR_HAND));
    Refresh();
}

void DensityCurveGraph::send(Drag::Phase phase, const wxPoint &p)
{
    if (on_drag)
        on_drag({ phase, m_drag, x_at(p.x), density_at(p.y) });
}

void DensityCurveGraph::on_left_down(wxMouseEvent &e)
{
    if (const size_t handle = hit(e.GetPosition()); handle != None) {
        m_drag = handle;
        if (!HasCapture())
            CaptureMouse();
        Refresh();
        send(Drag::Phase::Begin, e.GetPosition());
    }
    e.Skip();
}

void DensityCurveGraph::on_motion(wxMouseEvent &e)
{
    if (m_drag != None)
        send(Drag::Phase::Move, e.GetPosition());
    else
        set_hover(hit(e.GetPosition()));
    e.Skip();
}

void DensityCurveGraph::on_left_up(wxMouseEvent &e)
{
    if (HasCapture())
        ReleaseMouse();
    if (m_drag != None) {
        send(Drag::Phase::End, e.GetPosition());
        m_drag  = None;
        m_x_max = m_curve.x_max;
        m_hover = None;
        set_hover(hit(e.GetPosition()));
        Refresh();
    }
    e.Skip();
}

void DensityCurveGraph::on_paint(wxPaintEvent &)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
    dc.Clear();
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc)
        return;
    const bool     dark = Slic3r::GUI::wxGetApp().dark_mode();
    const wxRect   r    = plot_rect();
    const wxColour text = themed("#6B6B6B", "#B3B3B5");
    const wxColour accent(Accent);

    gc->SetPen(wxPen(themed("#DBDBDB", "#4A4A51"), 1));
    gc->SetBrush(wxBrush(themed("#FAFAFA", "#323236")));
    gc->DrawRectangle(r.x, r.y, r.width, r.height);

    // Grid.
    gc->SetPen(wxPen(themed("#E8E8E8", "#45454B"), 1));
    for (int i = 1; i < 4; ++i)
        gc->StrokeLine(r.x, r.y + r.height * i / 4., r.GetRight(), r.y + r.height * i / 4.);
    const double step = tick_step();
    for (double d = step; d < m_x_max - 1e-6; d += step)
        gc->StrokeLine(x_px(d), r.y, x_px(d), r.GetBottom());

    if (m_curve.shade_from < m_x_max) {
        const double x0 = x_px(std::max(0., m_curve.shade_from));
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(wxColour(m_shade_colour.Red(), m_shade_colour.Green(), m_shade_colour.Blue(), dark ? 70 : 45)));
        gc->DrawRectangle(x0, r.y, r.GetRight() - x0, r.height);
    }

    constexpr int samples = 160;
    auto curve_y = [this](double x, double factor) {
        return y_px(std::clamp(m_curve.density ? m_curve.density(x) * factor : 0., 0., 1.));
    };
    if (m_curve.spread > 0. && m_curve.density) {
        wxGraphicsPath band = gc->CreatePath();
        for (int i = 0; i <= samples; ++i) {
            const double x = m_x_max * i / samples;
            i == 0 ? band.MoveToPoint(x_px(x), curve_y(x, 1. + m_curve.spread)) : band.AddLineToPoint(x_px(x), curve_y(x, 1. + m_curve.spread));
        }
        for (int i = samples; i >= 0; --i) {
            const double x = m_x_max * i / samples;
            band.AddLineToPoint(x_px(x), curve_y(x, 1. - m_curve.spread));
        }
        band.CloseSubpath();
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(wxColour(accent.Red(), accent.Green(), accent.Blue(), dark ? 50 : 35)));
        gc->FillPath(band);
    }

    gc->SetPen(wxPen(themed("#B8B8B8", "#5E5E66"), 1, wxPENSTYLE_SHORT_DASH));
    for (double x : m_curve.guides)
        if (x > 0. && x < m_x_max)
            gc->StrokeLine(x_px(x), r.y, x_px(x), r.GetBottom());

    if (m_curve.density) {
        wxGraphicsPath curve = gc->CreatePath();
        for (int i = 0; i <= samples; ++i) {
            const double x = m_x_max * i / samples;
            i == 0 ? curve.MoveToPoint(x_px(x), curve_y(x, 1.)) : curve.AddLineToPoint(x_px(x), curve_y(x, 1.));
        }
        gc->SetPen(wxPen(accent, FromDIP(2)));
        gc->StrokePath(curve);
    }

    if (m_curve.mask_from < m_x_max) {
        const double x = x_px(std::max(0., m_curve.mask_from));
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(dark ? wxColour(20, 20, 22, 150) : wxColour(255, 255, 255, 170)));
        gc->DrawRectangle(x, r.y, r.GetRight() - x, r.height);
        gc->SetPen(wxPen(themed("#262E30", "#EFEFF0"), 1));
        gc->StrokeLine(x, r.y, x, r.GetBottom());
        gc->SetFont(Label::Body_12, themed("#262E30", "#EFEFF0"));
        double lw, lh;
        gc->GetTextExtent(m_curve.mask_label, &lw, &lh);
        // Beside the line, on the side with room for it.
        gc->DrawText(m_curve.mask_label, x + FromDIP(4) + lw <= r.GetRight() ? x + FromDIP(4) : x - FromDIP(4) - lw, r.y + FromDIP(4));
    }

    for (size_t i = 0; i < m_curve.handles.size(); ++i) {
        const DensityCurve::Handle &handle = m_curve.handles[i];
        const wxPoint2DDouble       q      = handle_pos(handle);
        const bool                  active = i == m_hover || i == m_drag;
        if (handle.kind == DensityCurve::HandleKind::Edge) {
            if (handle.x < m_x_max) {
                gc->SetPen(wxPen(m_shade_colour, FromDIP(active ? 2 : 1)));
                gc->StrokeLine(q.m_x, r.y, q.m_x, r.GetBottom());
            }
            const double w = FromDIP(6), h = FromDIP(24);
            gc->SetPen(wxPen(m_shade_colour, 1));
            gc->SetBrush(wxBrush(active ? m_shade_colour : themed("#FFFFFF", "#2D2D31")));
            gc->DrawRoundedRectangle(q.m_x - w / 2., q.m_y - h / 2., w, h, w / 2.);
            continue;
        }
        const bool   ring   = handle.kind == DensityCurve::HandleKind::Ring;
        const double radius = FromDIP(active ? 6 : ring ? 4 : 5);
        gc->SetPen(wxPen(accent, FromDIP(2)));
        gc->SetBrush(ring && !active ? wxBrush(themed("#FAFAFA", "#323236")) : wxBrush(themed("#FFFFFF", "#F0F0F0")));
        gc->DrawEllipse(q.m_x - radius, q.m_y - radius, 2. * radius, 2. * radius);
    }

    // Axes.
    gc->SetFont(Label::Body_12, text);
    double tw, th;
    for (int i = 0; i <= 4; ++i) {
        const wxString s = wxString::Format("%d%%", i * 25);
        gc->GetTextExtent(s, &tw, &th);
        gc->DrawText(s, r.x - FromDIP(6) - tw, r.y + r.height * (4 - i) / 4. - th / 2.);
    }
    for (double d = 0.; d <= m_x_max + 1e-6; d += step) {
        const wxString s = Slic3r::GUI::double_to_string(d, m_x_digits);
        gc->GetTextExtent(s, &tw, &th);
        gc->DrawText(s, std::min(x_px(d) - tw / 2., double(r.GetRight()) - tw), r.GetBottom() + FromDIP(4));
    }
    gc->GetTextExtent(m_curve.x_title, &tw, &th);
    gc->DrawText(m_curve.x_title, r.x + (r.width - tw) / 2., r.GetBottom() + FromDIP(6) + th);
    gc->GetTextExtent(m_curve.y_title, &tw, &th);
    gc->DrawText(m_curve.y_title, FromDIP(2), r.y + (r.height + tw) / 2., M_PI / 2.);
    if (!m_curve.shade_title.IsEmpty()) {
        gc->SetFont(Label::Body_12, m_shade_colour);
        gc->GetTextExtent(m_curve.shade_title, &tw, &th);
        gc->DrawText(m_curve.shade_title, r.GetRight() + FromDIP(6) + th, r.y + (r.height - tw) / 2., -M_PI / 2.);
    }

    // Value of the handle under the mouse.
    const size_t shown = m_drag != None ? m_drag : m_hover;
    if (shown != None && shown < m_curve.handles.size() && !m_curve.handles[shown].readout.IsEmpty()) {
        const wxString       &s = m_curve.handles[shown].readout;
        const wxPoint2DDouble q = handle_pos(m_curve.handles[shown]);
        gc->SetFont(Label::Body_12, themed("#262E30", "#EFEFF0"));
        gc->GetTextExtent(s, &tw, &th);
        const double pad = FromDIP(4);
        const double x   = std::clamp(q.m_x + FromDIP(10), double(r.x), r.GetRight() - tw - 2. * pad);
        const double y   = std::clamp(q.m_y - th - FromDIP(14), double(r.y), r.GetBottom() - th - 2. * pad);
        gc->SetPen(wxPen(themed("#DBDBDB", "#4A4A51"), 1));
        gc->SetBrush(wxBrush(themed("#FFFFFF", "#2D2D31")));
        gc->DrawRoundedRectangle(x, y, tw + 2. * pad, th + 2. * pad, FromDIP(3));
        gc->DrawText(s, x + pad, y + pad);
    }
}
