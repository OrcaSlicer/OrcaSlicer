#include "MixedMixPreview.hpp"
#include "GUI_App.hpp"         // wxGetApp() / dark_mode()

#include <wx/dcbuffer.h>       // wxAutoBufferedPaintDC
#include <algorithm>
#include <cmath>

namespace Slic3r { namespace GUI {

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

MixedMixPreview::MixedMixPreview(wxWindow *parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinSize(wxSize(FromDIP(120), FromDIP(20)));
    Bind(wxEVT_PAINT, &MixedMixPreview::on_paint, this);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void MixedMixPreview::set_data(const std::vector<wxColour>     &palette,
                                const std::vector<unsigned int> &sequence,
                                bool                             same_layer_mode,
                                const std::vector<double>       &surface_offsets_mm,
                                const wxColour                  &fallback,
                                const wxString                  &left_overlay,
                                const wxString                  &right_overlay)
{
    m_palette            = palette;
    m_sequence           = sequence;
    m_same_layer         = same_layer_mode;
    m_surface_offsets_mm = surface_offsets_mm;
    m_fallback           = fallback;
    m_left_overlay       = left_overlay;
    m_right_overlay      = right_overlay;
    Refresh();
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

wxRect MixedMixPreview::preview_rect() const
{
    const int margin_x = FromDIP(1);
    const int margin_y = FromDIP(1);
    const wxSize sz    = GetClientSize();
    return wxRect(margin_x, margin_y,
                  std::max(1, sz.GetWidth()  - margin_x * 2),
                  std::max(1, sz.GetHeight() - margin_y * 2));
}

wxColour MixedMixPreview::color_for_extruder(unsigned int extruder_id) const
{
    if (extruder_id >= 1 && extruder_id <= m_palette.size())
        return m_palette[extruder_id - 1];
    return m_fallback;
}

double MixedMixPreview::max_active_surface_offset_mm() const
{
    double max_offset = 0.0;
    for (double offset_mm : m_surface_offsets_mm)
        max_offset = std::max(max_offset, std::abs(offset_mm));
    return std::max(0.001, max_offset);
}

int MixedMixPreview::slot_inset_for_extruder(unsigned int extruder_id, int slot_extent) const
{
    if (extruder_id == 0 || extruder_id >= m_surface_offsets_mm.size() || slot_extent <= 2)
        return 0;

    const double offset_mm = m_surface_offsets_mm[extruder_id];
    if (std::abs(offset_mm) <= EPSILON)
        return 0;

    const double normalized = std::clamp(std::abs(offset_mm) / max_active_surface_offset_mm(), 0.0, 1.0);
    const int inset = int(std::round(normalized * slot_extent * 0.45))
                      * (offset_mm < 0.0 ? -1 : 1);
    return std::clamp(inset,
                      -std::max(0, slot_extent / 2),
                       std::max(0, slot_extent / 2));
}

// ---------------------------------------------------------------------------
// Paint handler
// ---------------------------------------------------------------------------

void MixedMixPreview::on_paint(wxPaintEvent &)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(GetBackgroundColour()));
    dc.Clear();

    const wxRect rect = preview_rect();
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(m_fallback));
    dc.DrawRectangle(rect);

    if (!m_sequence.empty()) {
        if (m_same_layer) {
            // Same-layer preview: full-height stripe lines.
            const int stripes  = 24;
            const int stripe_w = std::max(1, rect.GetWidth() / stripes);
            const size_t seq_len = m_sequence.size();
            for (int s = 0; s < stripes; ++s) {
                const size_t idx         = size_t(s % int(seq_len));
                const unsigned int extruder_id = m_sequence[idx];
                dc.SetBrush(wxBrush(color_for_extruder(extruder_id)));
                const int x     = rect.GetLeft() + s * stripe_w;
                const int w     = (s == stripes - 1) ? (rect.GetRight() - x + 1) : stripe_w;
                const int inset = slot_inset_for_extruder(extruder_id, w);
                wxRect draw_rect(x + inset / 2, rect.GetTop(),
                                 std::max(1, w - inset), rect.GetHeight());
                draw_rect.Intersect(rect);
                if (draw_rect.GetWidth() > 0)
                    dc.DrawRectangle(draw_rect);
            }
        } else {
            const int bars  = 24;
            const int bar_w = std::max(1, rect.GetWidth() / bars);
            for (int i = 0; i < bars; ++i) {
                size_t idx = 0;
                if (m_sequence.size() > size_t(bars))
                    idx = (size_t(i) * m_sequence.size()) / size_t(bars);
                else
                    idx = size_t(i) % m_sequence.size();
                const unsigned int extruder_id = m_sequence[idx];
                dc.SetBrush(wxBrush(color_for_extruder(extruder_id)));
                const int x     = rect.GetLeft() + i * bar_w;
                const int w     = (i == bars - 1) ? (rect.GetRight() - x + 1) : bar_w;
                const int inset = slot_inset_for_extruder(extruder_id, w);
                wxRect draw_rect(x + inset / 2, rect.GetTop(),
                                 std::max(1, w - inset), rect.GetHeight());
                draw_rect.Intersect(rect);
                if (draw_rect.GetWidth() > 0)
                    dc.DrawRectangle(draw_rect);
            }
        }
    }

    auto draw_outlined_text = [this, &dc](const wxString &text, int x, int y) {
        if (text.empty())
            return;
        dc.SetTextForeground(wxColour(255, 255, 255));
        const int outline_radius = std::max(2, FromDIP(2));
        for (int ox = -outline_radius; ox <= outline_radius; ++ox) {
            for (int oy = -outline_radius; oy <= outline_radius; ++oy) {
                if (ox == 0 && oy == 0)
                    continue;
                dc.DrawText(text, x + ox, y + oy);
            }
        }
        dc.SetTextForeground(wxColour(22, 22, 22));
        dc.DrawText(text, x, y);
    };

    wxCoord left_w  = 0, left_h  = 0;
    wxCoord right_w = 0, right_h = 0;
    dc.GetTextExtent(m_left_overlay,  &left_w,  &left_h);
    dc.GetTextExtent(m_right_overlay, &right_w, &right_h);
    const int text_y = rect.GetTop()
                       + std::max(0, (rect.GetHeight() - int(std::max(left_h, right_h))) / 2);
    const int pad = FromDIP(6);
    if (!m_left_overlay.empty())
        draw_outlined_text(m_left_overlay, rect.GetLeft() + pad, text_y);
    if (!m_right_overlay.empty())
        draw_outlined_text(m_right_overlay, rect.GetRight() - pad - int(right_w), text_y);

    const bool is_dark = wxGetApp().dark_mode();
    dc.SetPen(wxPen(is_dark ? wxColour(110, 110, 110) : wxColour(170, 170, 170), 1));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawRectangle(rect);
}

} } // namespace Slic3r::GUI
