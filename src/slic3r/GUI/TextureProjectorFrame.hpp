#ifndef slic3r_TextureProjectorFrame_hpp_
#define slic3r_TextureProjectorFrame_hpp_

// The projection-frame overlay for TextureProjectionMethod::ViewProjected.
//
// A semi-transparent, resizable window that the user drags over the 3D view like a slide projector's
// gate: whatever the model shows through this window is what the texture is projected onto, and the
// window's border is the hard edge of the projection. "Apply" then reads the window's client
// rectangle, converts it into the 3D canvas's own pixel space, and builds an exact projective map
// from it (see GLGizmoTextureDisplacement::apply_projection_frame()).
//
// The window itself is deliberately dumb - it owns no placement state and reports nothing
// continuously. Its position and size *are* the placement, and they are read on demand at Apply,
// which is also when the (expensive) visible-facet selection runs. Moving the window is therefore
// free, and nothing recomputes until the user asks for it.
//
// Plain 2D (wxGraphicsContext), not a wxGLCanvas: a second GL canvas would have to share the app's
// one real wxGLContext, the cause of bugs #10 and #14 in TEXTURE_DISPLACEMENT.md. A paint-DC window
// has no such failure mode, and this one only ever draws a bitmap and a border.

#include <vector>

#include <wx/bitmap.h>
#include <wx/frame.h>

namespace Slic3r { namespace GUI {

class TextureProjectorFrame : public wxFrame
{
public:
    explicit TextureProjectorFrame(wxWindow *parent);

    // The same 8-bit grayscale pixels build_texture_displacement() samples, so what is aligned here
    // is what gets baked. Pass width/height <= 0 to clear it.
    void set_texture(const std::vector<unsigned char> &grayscale_pixels, int width, int height);

    // Whole-window opacity, 0..255. Low enough to see the model through it, high enough to judge
    // where the texture lands - the useful range is roughly 60..200.
    void set_opacity(int alpha);
    int  opacity() const { return m_alpha; }

    // The client area (the gate itself, excluding caption and borders) in screen coordinates. This
    // is what the projection is built from, so it deliberately excludes the window decorations -
    // the user aligns what they see, which is the client area.
    wxRect client_rect_on_screen() const;

private:
    wxBitmap m_bitmap;
    int      m_alpha = 140;

    void on_paint(wxPaintEvent &);
};

}} // namespace Slic3r::GUI

#endif // slic3r_TextureProjectorFrame_hpp_
