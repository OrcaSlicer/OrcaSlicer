#include "MixedGradientSelector.hpp"
#include "GUI_App.hpp"            // wxGetApp() / dark_mode()
#include "I18N.hpp"               // _L()
#include "Widgets/Label.hpp"      // Label::Body_10
#include "libslic3r/filament_mixer.h"  // filament_mixer_lerp

#include <wx/dcbuffer.h>
#include <algorithm>

namespace Slic3r { namespace GUI {

// ---------------------------------------------------------------------------
// Anonymous-namespace helper: copied verbatim from FullSpectrum Plater.cpp:2424
// ---------------------------------------------------------------------------
namespace {

wxColour blend_pair_filament_mixer(const wxColour &left, const wxColour &right, float t)
{
    const wxColour safe_left  = left.IsOk()  ? left  : wxColour("#26A69A");
    const wxColour safe_right = right.IsOk() ? right : wxColour("#26A69A");

    unsigned char out_r = static_cast<unsigned char>(safe_left.Red());
    unsigned char out_g = static_cast<unsigned char>(safe_left.Green());
    unsigned char out_b = static_cast<unsigned char>(safe_left.Blue());
    ::Slic3r::filament_mixer_lerp(static_cast<unsigned char>(safe_left.Red()),
                                  static_cast<unsigned char>(safe_left.Green()),
                                  static_cast<unsigned char>(safe_left.Blue()),
                                  static_cast<unsigned char>(safe_right.Red()),
                                  static_cast<unsigned char>(safe_right.Green()),
                                  static_cast<unsigned char>(safe_right.Blue()),
                                  std::clamp(t, 0.f, 1.f),
                                  &out_r, &out_g, &out_b);
    return wxColour(out_r, out_g, out_b);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Constructor / destructor
// ---------------------------------------------------------------------------

MixedGradientSelector::MixedGradientSelector(wxWindow       *parent,
                                              const wxColour &left,
                                              const wxColour &right,
                                              int             value_percent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
    , m_left(left)
    , m_right(right)
    , m_value(std::clamp(value_percent, 0, 100))
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinSize(wxSize(FromDIP(96), FromDIP(12)));
    Bind(wxEVT_PAINT,              &MixedGradientSelector::on_paint,        this);
    Bind(wxEVT_LEFT_DOWN,          &MixedGradientSelector::on_left_down,    this);
    Bind(wxEVT_LEFT_UP,            &MixedGradientSelector::on_left_up,      this);
    Bind(wxEVT_MOTION,             &MixedGradientSelector::on_mouse_move,   this);
    Bind(wxEVT_MOUSE_CAPTURE_LOST, &MixedGradientSelector::on_capture_lost, this);
}

MixedGradientSelector::~MixedGradientSelector()
{
    if (HasCapture())
        ReleaseMouse();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void MixedGradientSelector::set_colors(const wxColour &left, const wxColour &right)
{
    m_left  = left;
    m_right = right;
    m_multi_mode = false;
    m_multi_colors.clear();
    m_multi_weights.clear();
    Refresh();
}

void MixedGradientSelector::set_multi_preview(const std::vector<wxColour> &corner_colors,
                                               const std::vector<int>       &weights)
{
    m_multi_mode    = corner_colors.size() >= 3;
    m_multi_colors  = corner_colors;
    m_multi_weights = weights;
    Refresh();
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

wxRect MixedGradientSelector::gradient_rect() const
{
    const int margin_x = FromDIP(2);
    const int margin_y = FromDIP(1);
    const wxSize sz    = GetClientSize();
    return wxRect(margin_x, margin_y,
                  std::max(1, sz.GetWidth()  - margin_x * 2),
                  std::max(1, sz.GetHeight() - margin_y * 2));
}

int MixedGradientSelector::value_from_x(int x) const
{
    const wxRect rect    = gradient_rect();
    const int    min_x   = rect.GetLeft();
    const int    max_x   = rect.GetLeft() + rect.GetWidth();
    const int    clamp_x = std::clamp(x, min_x, max_x);
    return ((clamp_x - min_x) * 100 + rect.GetWidth() / 2) / rect.GetWidth();
}

void MixedGradientSelector::update_from_x(int x, bool notify)
{
    m_value = value_from_x(x);
    Refresh();

    if (notify) {
        wxCommandEvent evt(wxEVT_SLIDER, GetId());
        evt.SetInt(m_value);
        evt.SetEventObject(this);
        ProcessWindowEvent(evt);
    }
}

// ---------------------------------------------------------------------------
// Event handlers
// ---------------------------------------------------------------------------

void MixedGradientSelector::on_paint(wxPaintEvent &)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(GetBackgroundColour()));
    dc.Clear();
    const bool is_dark = wxGetApp().dark_mode();

    const wxRect rect = gradient_rect();
    if (m_multi_mode && m_multi_colors.size() >= 3) {
        const wxPoint tl(rect.GetLeft(),                          rect.GetTop());
        const wxPoint tr(rect.GetRight(),                         rect.GetTop());
        const wxPoint br(rect.GetRight(),                         rect.GetBottom());
        const wxPoint bl(rect.GetLeft(),                          rect.GetBottom());
        const wxPoint cc(rect.GetLeft() + rect.GetWidth()  / 2,
                         rect.GetTop()  + rect.GetHeight() / 2);

        auto draw_tri = [&dc](const wxColour &color,
                               const wxPoint  &a,
                               const wxPoint  &b,
                               const wxPoint  &c) {
            wxPoint pts[3] = { a, b, c };
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(color));
            dc.DrawPolygon(3, pts);
        };

        if (m_multi_colors.size() >= 4) {
            draw_tri(m_multi_colors[0], tl, tr, cc);
            draw_tri(m_multi_colors[1], tr, br, cc);
            draw_tri(m_multi_colors[2], br, bl, cc);
            draw_tri(m_multi_colors[3], bl, tl, cc);
        } else {
            // 3-colour layout: first colour occupies one full side, two others on the opposite corners.
            draw_tri(m_multi_colors[0], tl, bl, cc);
            draw_tri(m_multi_colors[1], tl, tr, cc);
            draw_tri(m_multi_colors[2], bl, br, cc);
        }

        if (m_multi_weights.size() == m_multi_colors.size()) {
            dc.SetTextForeground(is_dark ? wxColour(236, 236, 236) : wxColour(20, 20, 20));
            dc.SetFont(Label::Body_10);
            const int pad = FromDIP(2);
            if (m_multi_colors.size() >= 4) {
                dc.DrawText(wxString::Format("%d%%", m_multi_weights[0]),
                            rect.GetLeft()  + pad,         rect.GetTop()    + pad);
                dc.DrawText(wxString::Format("%d%%", m_multi_weights[1]),
                            rect.GetRight() - FromDIP(28), rect.GetTop()    + pad);
                dc.DrawText(wxString::Format("%d%%", m_multi_weights[2]),
                            rect.GetRight() - FromDIP(28), rect.GetBottom() - FromDIP(14));
                dc.DrawText(wxString::Format("%d%%", m_multi_weights[3]),
                            rect.GetLeft()  + pad,         rect.GetBottom() - FromDIP(14));
            } else {
                dc.DrawText(wxString::Format("%d%%", m_multi_weights[0]),
                            rect.GetLeft()  + pad,
                            rect.GetTop()   + rect.GetHeight() / 2 - FromDIP(6));
                dc.DrawText(wxString::Format("%d%%", m_multi_weights[1]),
                            rect.GetRight() - FromDIP(28), rect.GetTop()    + pad);
                dc.DrawText(wxString::Format("%d%%", m_multi_weights[2]),
                            rect.GetRight() - FromDIP(28), rect.GetBottom() - FromDIP(14));
            }
        }
    } else {
        const int w = rect.GetWidth();
        const int h = rect.GetHeight();
        wxImage img(w, h);
        unsigned char *data = img.GetData();
        if (data != nullptr) {
            for (int x = 0; x < w; ++x) {
                const float    t   = (w > 1) ? float(x) / float(w - 1) : 0.5f;
                const wxColour col = blend_pair_filament_mixer(m_left, m_right, t);
                const unsigned char r = static_cast<unsigned char>(col.Red());
                const unsigned char g = static_cast<unsigned char>(col.Green());
                const unsigned char b = static_cast<unsigned char>(col.Blue());
                for (int y = 0; y < h; ++y) {
                    const int idx = (y * w + x) * 3;
                    data[idx + 0] = r;
                    data[idx + 1] = g;
                    data[idx + 2] = b;
                }
            }
            dc.DrawBitmap(wxBitmap(img), rect.GetLeft(), rect.GetTop(), false);
        } else {
            dc.GradientFillLinear(rect, m_left, m_right, wxEAST);
        }
    }

    dc.SetPen(wxPen(is_dark ? wxColour(100, 100, 106) : wxColour(170, 170, 170), 1));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawRectangle(rect);

    if (m_multi_mode) {
        dc.SetTextForeground(is_dark ? wxColour(236, 236, 236) : wxColour(30, 30, 30));
        dc.SetFont(Label::Body_10);
        const wxString hint    = _L("Click to edit");
        wxSize         text_sz = dc.GetTextExtent(hint);
        dc.DrawText(hint, rect.GetRight() - text_sz.GetWidth() - FromDIP(4), rect.GetTop() + FromDIP(2));
        return;
    }

    int marker_x = rect.GetLeft() + (rect.GetWidth() * m_value + 50) / 100;
    marker_x = std::clamp(marker_x, rect.GetLeft(), rect.GetRight());
    dc.SetPen(wxPen(wxColour(255, 255, 255), 3));
    dc.DrawLine(marker_x, rect.GetTop(), marker_x, rect.GetBottom());
    dc.SetPen(wxPen(wxColour(33, 33, 33), 1));
    dc.DrawLine(marker_x, rect.GetTop(), marker_x, rect.GetBottom());
}

void MixedGradientSelector::on_left_down(wxMouseEvent &evt)
{
    if (m_multi_mode)
        return;
    if (!HasCapture())
        CaptureMouse();
    m_dragging = true;
    update_from_x(evt.GetX(), false);
}

void MixedGradientSelector::on_left_up(wxMouseEvent &evt)
{
    if (m_multi_mode) {
        wxCommandEvent click_evt(wxEVT_BUTTON, GetId());
        click_evt.SetEventObject(this);
        ProcessWindowEvent(click_evt);
        return;
    }
    if (m_dragging)
        update_from_x(evt.GetX(), true);
    m_dragging = false;
    if (HasCapture())
        ReleaseMouse();
}

void MixedGradientSelector::on_mouse_move(wxMouseEvent &evt)
{
    if (m_dragging && evt.LeftIsDown())
        update_from_x(evt.GetX(), false);
}

void MixedGradientSelector::on_capture_lost(wxMouseCaptureLostEvent &)
{
    m_dragging = false;
}

} } // namespace Slic3r::GUI
