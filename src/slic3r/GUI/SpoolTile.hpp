#pragma once

// How a spool is drawn in the filament link panels: a picture of the spool with the filament's
// name under it, and a chip for each color of the filament with the profile that color prints with.

#include <functional>
#include <string>
#include <vector>

#include <wx/colour.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/panel.h>
#include <wx/string.h>
#include <wx/window.h>

#include "wxExtensions.hpp"

namespace Slic3r { namespace GUI {

constexpr int SPOOL_TILE_WIDTH = 110;

inline const wxColour SPOOL_LINE_COLOR(0x8C, 0x8C, 0x8C);

// A spool seen from the side. One color fills the filament on it; a filament seen in several
// colors shows a small square for each of them instead.
class SpoolPicture : public wxWindow
{
public:
    SpoolPicture(wxWindow* parent, const std::vector<std::string>& variants);

private:
    void on_paint(wxPaintEvent& event);
    void draw_color_squares(wxDC& dc, const wxColour& under) const;
    void set_hover(bool hover);

    std::vector<std::string> m_variants;
    ScalableBitmap           m_spool;
    ScalableBitmap           m_spool_hover;
    bool                     m_hover = false;
};

// One color of a filament: the color on top, the profile it prints with under it. As wide as the
// profile name needs, so the name is never cut.
class VariantChip : public wxWindow
{
public:
    // A color on a profile of its own writes the profile in bold, to tell it from the colors that
    // follow the filament, and carries the icon that puts it back on the filament's profile.
    VariantChip(wxWindow* parent, const std::string& variant, const wxString& profile, bool own_profile);

    // Where the profile is written, in the chip's own coordinates.
    wxRect profile_area() const;
    // Runs on a click anywhere on the chip but the forget icon.
    void on_pick(std::function<void()> handler);
    void on_forget(std::function<void()> handler);

private:
    void   on_paint(wxPaintEvent& event);
    void   on_left_down(wxMouseEvent& event);
    wxRect forget_icon_rect() const;

    std::vector<std::string> m_colors;
    wxString                 m_profile;
    bool                     m_own_profile;
    ScalableBitmap           m_forget_icon;
    std::function<void()>    m_on_pick;
    std::function<void()>    m_on_forget;
};

// The spool's picture over its name. Clicking anywhere on it runs the handler.
class SpoolTile : public wxPanel
{
public:
    SpoolTile(wxWindow* parent, const std::vector<std::string>& variants, const wxString& title);

    void on_click(std::function<void()> handler);
};

}} // namespace Slic3r::GUI
