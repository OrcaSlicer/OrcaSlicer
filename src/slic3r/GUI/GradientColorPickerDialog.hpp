#pragma once

#include "GUI_Utils.hpp"
#include "libslic3r/FilamentGradient.hpp"

#include <wx/wx.h>
#include <wx/spinctrl.h>
#include <functional>

namespace Slic3r { namespace GUI {

// ---------------------------------------------------------------------------
// GradientBarWidget — custom wxWindow that renders the gradient bar and
// lets the user add, move and remove color stops.
// ---------------------------------------------------------------------------
class GradientBarWidget : public wxWindow
{
public:
    GradientBarWidget(wxWindow* parent, const FilamentGradient& gradient);

    FilamentGradient get_gradient() const;

    // Selected-stop access for the hosting dialog (hex entry, add/remove buttons).
    bool get_selected_color(ColorRGBA& out) const;
    void set_selected_color(const ColorRGBA& c);
    void add_stop();              // add a stop in the largest gap and select it
    void remove_selected_stop();  // remove the selected stop (keeps >= 2 stops)

    // Invoked whenever the selected stop or its colour changes (so the dialog can refresh).
    std::function<void()> on_selection_changed;

    // Draw everything.
    void on_paint(wxPaintEvent&);

    // Mouse interaction.
    void on_left_down(wxMouseEvent& e);
    void on_left_up(wxMouseEvent& e);
    void on_motion(wxMouseEvent& e);
    void on_double_click(wxMouseEvent& e);
    void on_right_down(wxMouseEvent& e);

    DECLARE_EVENT_TABLE()

private:
    static constexpr int BAR_H       = 28;  // height of the gradient bar strip
    static constexpr int HANDLE_H    = 10;  // height of the triangular handle below the bar
    static constexpr int TOTAL_H     = BAR_H + HANDLE_H + 4;
    static constexpr int MARGIN_X    = 12;  // left/right margin so handles can reach the edges
    static constexpr int HANDLE_W    = 9;   // half-width of the handle triangle base

    // Convert stop position [0,1] to pixel x inside the bar.
    int pos_to_x(float pos) const;
    // Convert pixel x to stop position [0,1].
    float x_to_pos(int x) const;

    // Return index of handle under point, or -1.
    int hit_test(wxPoint pt) const;

    // Redraw the cached gradient bitmap.
    void rebuild_gradient_bitmap();

    // Open the system colour picker for stop[index].
    void pick_color_for(int index);

    std::vector<GradientColorStop> m_stops;
    int   m_dragging { -1 };   // index of stop being dragged, -1 = none
    int   m_selected { -1 };   // index of currently selected stop
    wxBitmap m_gradient_bmp;   // cached rendered gradient bar
    bool     m_bmp_dirty { true };
};


// ---------------------------------------------------------------------------
// GradientColorPickerDialog — wraps GradientBarWidget with cycle length,
// start offset, and OK/Cancel controls.
// ---------------------------------------------------------------------------
class GradientColorPickerDialog : public DPIDialog
{
public:
    GradientColorPickerDialog(wxWindow* parent, const FilamentGradient& gradient,
                              double filament_diameter_mm = 1.75, double filament_density_g_cm3 = 1.24);

    FilamentGradient get_gradient() const;

protected:
    void on_dpi_changed(const wxRect&) override {}

private:
    // Convert between a filament weight (g) and extruded length (mm) for this filament,
    // using its diameter and density. No-ops if either is non-positive.
    double grams_to_mm(double grams) const;
    double mm_to_grams(double mm) const;

    void refresh_hex_field();   // sync the hex text box to the selected stop's colour
    void commit_hex_field();    // parse the hex box and apply it to the selected stop

    GradientBarWidget* m_bar       { nullptr };
    wxTextCtrl*        m_hex       { nullptr };  // "#RRGGBB" for the selected stop
    wxSpinCtrlDouble*  m_cycle     { nullptr };
    wxChoice*          m_cycle_unit{ nullptr };  // "mm" (0) or "g" (1)
    wxSpinCtrlDouble*  m_offset    { nullptr };
    double             m_diameter  { 1.75 };     // mm
    double             m_density   { 1.24 };     // g/cm^3
};

}} // namespace Slic3r::GUI
