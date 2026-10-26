#pragma once

// How a spool is drawn in the filament link panels: a chip for each look of the filament, split in
// bands when the spool has several colors, and a tile that carries the chips with the spool's name.

#include <functional>
#include <string>
#include <vector>

#include <wx/colour.h>
#include <wx/gdicmn.h>
#include <wx/panel.h>
#include <wx/string.h>
#include <wx/window.h>

namespace Slic3r { namespace GUI {

constexpr int SPOOL_TILE_WIDTH = 190;

inline const wxColour SPOOL_LINE_COLOR(0x8C, 0x8C, 0x8C);

// What a tile draws for each look of the filament: the plain chips, or the filament chip of the
// send-print mapping.
enum class SpoolLook { Chips, MappingChip };

// A color chip that is still visible when the filament is white: the fill is the filament
// color, the outline is always drawn. A chip given more height than width keeps the chip square
// and spends the rest on the underline that marks a color with a profile of its own.
class ColorSwatch : public wxWindow
{
public:
    // The variant is a color as spool_variant_color gives it. Empty draws the chip of a color nobody
    // has reported.
    ColorSwatch(wxWindow* parent, const std::string& variant, const wxSize& size);

    void set_marked(bool marked) { m_marked = marked; }

private:
    void on_paint(wxPaintEvent& event);
    void draw_bands(wxDC& dc, int width, int box, const wxColour& under) const;

    std::vector<std::string> m_colors;
    bool                     m_marked = false;
};

// The spool's chips over its name, with a line under it for whatever the panel wants to say about
// it. Clicking anywhere on it runs the handler.
class SpoolTile : public wxPanel
{
public:
    // The material is written on the mapping chips; the plain chips have no room for it.
    SpoolTile(wxWindow*                       parent,
              const std::vector<std::string>& variants,
              const wxString&                 title,
              const wxString&                 subtitle,
              SpoolLook                       look     = SpoolLook::Chips,
              const wxString&                 material = wxString());

    void on_click(std::function<void()> handler);
};

}} // namespace Slic3r::GUI
