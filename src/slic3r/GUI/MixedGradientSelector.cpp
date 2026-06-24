#include "MixedGradientSelector.hpp"

namespace Slic3r { namespace GUI {

wxColour blend_pair_filament_mixer(const wxColour &left, const wxColour &right, float t)
{
    const wxColour safe_left  = left.IsOk()  ? left  : wxColour("#26A69A");
    const wxColour safe_right = right.IsOk() ? right : wxColour("#26A69A");

    unsigned char out_r = static_cast<unsigned char>(safe_left.Red());
    unsigned char out_g = static_cast<unsigned char>(safe_left.Green());
    unsigned char out_b = static_cast<unsigned char>(safe_left.Blue());
    ::Slic3r::filament_mixer_lerp(
        static_cast<unsigned char>(safe_left.Red()),
        static_cast<unsigned char>(safe_left.Green()),
        static_cast<unsigned char>(safe_left.Blue()),
        static_cast<unsigned char>(safe_right.Red()),
        static_cast<unsigned char>(safe_right.Green()),
        static_cast<unsigned char>(safe_right.Blue()),
        std::clamp(t, 0.f, 1.f),
        &out_r, &out_g, &out_b);
    return wxColour(out_r, out_g, out_b);
}

wxRect MixedGradientSelector::gradient_rect() const
{
    const int margin_x = FromDIP(2);
    const int margin_y = FromDIP(2);
    const wxSize sz = GetClientSize();
    return wxRect(margin_x, margin_y,
                  std::max(1, sz.GetWidth()  - margin_x * 2),
                  std::max(1, sz.GetHeight() - margin_y * 2));
}

int MixedGradientSelector::value_from_x(int x) const
{
    const wxRect rect = gradient_rect();
    const int min_x   = rect.GetLeft();
    const int max_x   = rect.GetLeft() + rect.GetWidth();
    const int cx      = std::clamp(x, min_x, max_x);
    int raw_value = ((cx - min_x) * 100 + rect.GetWidth() / 2) / rect.GetWidth();
    return std::clamp(raw_value, m_min_percent, m_max_percent);
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

void MixedGradientSelector::on_paint(wxPaintEvent &)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(GetBackgroundColour()));
    dc.Clear();
    const bool is_dark = wxGetApp().dark_mode();

    const wxRect rect = gradient_rect();
    if (m_multi_mode && m_multi_colors.size() >= 3) {
        const wxPoint tl(rect.GetLeft(),  rect.GetTop());
        const wxPoint tr(rect.GetRight(), rect.GetTop());
        const wxPoint br(rect.GetRight(), rect.GetBottom());
        const wxPoint bl(rect.GetLeft(),  rect.GetBottom());
        const wxPoint cc(rect.GetLeft() + rect.GetWidth() / 2,
                         rect.GetTop()  + rect.GetHeight() / 2);
        auto draw_tri = [&dc](const wxColour &color,
                              const wxPoint &a, const wxPoint &b, const wxPoint &c) {
            wxPoint pts[3] = {a, b, c};
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
            draw_tri(m_multi_colors[0], tl, bl, cc);
            draw_tri(m_multi_colors[1], tl, tr, cc);
            draw_tri(m_multi_colors[2], bl, br, cc);
        }
        if (m_multi_weights.size() == m_multi_colors.size()) {
            dc.SetTextForeground(is_dark ? wxColour(236,236,236) : wxColour(20,20,20));
            dc.SetFont(Label::Body_10);
            const int pad = FromDIP(2);
            if (m_multi_colors.size() >= 4) {
                dc.DrawText(wxString::Format("%d%%", m_multi_weights[0]), rect.GetLeft()  + pad,          rect.GetTop()    + pad);
                dc.DrawText(wxString::Format("%d%%", m_multi_weights[1]), rect.GetRight() - FromDIP(28),  rect.GetTop()    + pad);
                dc.DrawText(wxString::Format("%d%%", m_multi_weights[2]), rect.GetRight() - FromDIP(28),  rect.GetBottom() - FromDIP(14));
                dc.DrawText(wxString::Format("%d%%", m_multi_weights[3]), rect.GetLeft()  + pad,          rect.GetBottom() - FromDIP(14));
            } else {
                dc.DrawText(wxString::Format("%d%%", m_multi_weights[0]), rect.GetLeft()  + pad,         rect.GetTop() + rect.GetHeight()/2 - FromDIP(6));
                dc.DrawText(wxString::Format("%d%%", m_multi_weights[1]), rect.GetRight() - FromDIP(28), rect.GetTop()    + pad);
                dc.DrawText(wxString::Format("%d%%", m_multi_weights[2]), rect.GetRight() - FromDIP(28), rect.GetBottom() - FromDIP(14));
            }
        }
    } else {
        const int w = rect.GetWidth();
        const int h = rect.GetHeight();
        wxImage img(w, h);
        unsigned char *data = img.GetData();
        if (data != nullptr) {
            for (int x = 0; x < w; ++x) {
                const float t   = (w > 1) ? float(x) / float(w - 1) : 0.5f;
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

    // Outer border (Figma: #E7E8EA, solid, 1px, on full panel)
    const wxSize sz = GetClientSize();
    const wxColour border_color = is_dark ? wxColour(80, 80, 86) : wxColour(0xE7, 0xE8, 0xEA);
    dc.SetPen(wxPen(border_color, 1));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawRectangle(0, 0, sz.GetWidth(), sz.GetHeight());

    if (m_multi_mode) {
        dc.SetTextForeground(is_dark ? wxColour(236,236,236) : wxColour(30,30,30));
        dc.SetFont(Label::Body_10);
        const wxString hint = _L("Click to edit");
        wxSize text_sz = dc.GetTextExtent(hint);
        dc.DrawText(hint, rect.GetRight() - text_sz.GetWidth() - FromDIP(4), rect.GetTop() + FromDIP(2));
        return;
    }

    // Thumb marker: white bar with shadow, full panel height (Figma: 5x24, shadow)
    int thumb_center = rect.GetLeft() + (rect.GetWidth() * m_value + 50) / 100;
    thumb_center = std::clamp(thumb_center, rect.GetLeft() + FromDIP(2), rect.GetRight() - FromDIP(2));
    const int thumb_w = FromDIP(5);
    const int thumb_half = thumb_w / 2;

    // Shadow layers (simulating Figma box-shadow)
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(is_dark ? wxColour(60, 60, 60) : wxColour(220, 220, 220)));
    dc.DrawRectangle(thumb_center - thumb_half - FromDIP(1), 0,
                     thumb_w + FromDIP(2), sz.GetHeight());
    dc.SetBrush(wxBrush(is_dark ? wxColour(80, 80, 80) : wxColour(200, 200, 200)));
    dc.DrawRectangle(thumb_center - thumb_half + FromDIP(1), FromDIP(1),
                     thumb_w, sz.GetHeight() - FromDIP(1));

    // White thumb
    dc.SetBrush(*wxWHITE_BRUSH);
    dc.DrawRectangle(thumb_center - thumb_half, 0, thumb_w, sz.GetHeight());
}

void MixedGradientSelector::on_left_down(wxMouseEvent &evt)
{
    if (m_multi_mode) return;
    if (!HasCapture()) CaptureMouse();
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
    if (m_dragging) update_from_x(evt.GetX(), true);
    m_dragging = false;
    if (HasCapture()) ReleaseMouse();
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

}} // namespace Slic3r::GUI
