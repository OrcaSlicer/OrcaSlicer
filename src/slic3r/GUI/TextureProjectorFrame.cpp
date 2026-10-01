#include "TextureProjectorFrame.hpp"

#include <algorithm>

#include <wx/dcclient.h>
#include <wx/image.h>

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"

namespace Slic3r { namespace GUI {

TextureProjectorFrame::TextureProjectorFrame(wxWindow *parent)
    : wxFrame(parent, wxID_ANY, _L("Projection frame - drag over the model, then Apply"), wxDefaultPosition,
              wxSize(360, 360),
              // Caption and resize border so moving and sizing are the native gestures the user
              // already knows - "align it by moving the window" only works if the window moves the
              // ordinary way. FLOAT_ON_PARENT keeps it above the 3D view without the antisocial
              // always-on-top-of-everything behaviour of wxSTAY_ON_TOP.
              wxCAPTION | wxRESIZE_BORDER | wxCLOSE_BOX | wxFRAME_NO_TASKBAR | wxFRAME_FLOAT_ON_PARENT)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, &TextureProjectorFrame::on_paint, this);
    // A resize changes the gate, so the texture has to be re-stretched under it.
    Bind(wxEVT_SIZE, [this](wxSizeEvent &evt) { Refresh(); evt.Skip(); });
    SetTransparent(wxByte(m_alpha));

    // Hide rather than destroy: the gizmo owns this window's lifetime, and reopening should keep the
    // frame exactly where it was left - its position is the placement.
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent &evt) {
        if (evt.CanVeto()) {
            evt.Veto();
            Hide();
        } else
            evt.Skip();
    });
}

void TextureProjectorFrame::set_texture(const std::vector<unsigned char> &gray, int width, int height)
{
    if (width <= 0 || height <= 0 || gray.size() < size_t(width) * size_t(height)) {
        m_bitmap = wxBitmap();
        Refresh();
        return;
    }

    wxImage img(width, height);
    unsigned char *dst = img.GetData();
    for (size_t i = 0, n = size_t(width) * size_t(height); i < n; ++i) {
        const unsigned char v = gray[i];
        dst[i * 3 + 0] = v;
        dst[i * 3 + 1] = v;
        dst[i * 3 + 2] = v;
    }
    m_bitmap = wxBitmap(img);
    Refresh();
}

void TextureProjectorFrame::set_opacity(int alpha)
{
    m_alpha = std::clamp(alpha, 20, 255);
    SetTransparent(wxByte(m_alpha));
    Refresh();
}

wxRect TextureProjectorFrame::client_rect_on_screen() const
{
    const wxSize sz = GetClientSize();
    return wxRect(ClientToScreen(wxPoint(0, 0)), sz);
}

void TextureProjectorFrame::on_paint(wxPaintEvent &)
{
    wxPaintDC    dc(this);
    const wxSize sz = GetClientSize();
    if (sz.x <= 0 || sz.y <= 0)
        return;

    dc.SetBackground(wxBrush(wxColour(20, 20, 20)));
    dc.Clear();

    if (m_bitmap.IsOk()) {
        // Stretched to fill the client area rather than kept at its own aspect: the gate maps to the
        // uv unit square whatever its shape, so a non-square window genuinely does project a
        // stretched texture. Showing it any other way would misrepresent the bake.
        wxImage scaled = m_bitmap.ConvertToImage().Scale(sz.x, sz.y, wxIMAGE_QUALITY_NORMAL);
        dc.DrawBitmap(wxBitmap(scaled), 0, 0, false);
    }

    // The border is the projection's hard edge, so it is drawn explicitly - with the window
    // translucent, the native frame alone reads poorly against a busy 3D scene.
    dc.SetPen(wxPen(wxColour(0, 200, 180), 2));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawRectangle(0, 0, sz.x, sz.y);
}

}} // namespace Slic3r::GUI
