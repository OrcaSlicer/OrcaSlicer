#ifndef slic3r_GradientCurveEditor_hpp_
#define slic3r_GradientCurveEditor_hpp_

#include <vector>
#include <wx/colour.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/panel.h>

#include "libslic3r/FilamentMixer.hpp"

namespace Slic3r {
namespace GUI {

// Photoshop-style curve editor for "Z progress -> first-component ratio" mapping.
// Curve evaluation uses cubic Hermite with PCHIP defaults plus optional per-anchor
// tangent overrides (m_in / m_out, NaN = use PCHIP default). The same evaluator
// (FilamentMixer::sample_gradient_curve) is shared with the slicing backend so what
// the editor renders matches the G-code output 1:1.
//
// Interaction model (PS Curves style):
//   - Click or press-and-drag on the line body inserts a new anchor at the cursor x
//     (snapped to the current smooth curve, NaN tangents) and starts dragging it.
//     A pure click leaves an anchor sitting exactly on the previous curve shape; a
//     drag moves the new anchor freely so the bump follows the cursor 1:1.
//   - Dragging an existing anchor moves (x, y) and clears its m_in / m_out so the
//     local curve returns to the PCHIP default shape around it.
//   - Right-click on an interior anchor deletes it; endpoints stay locked.
class GradientCurveEditor : public wxPanel
{
public:
    using PointList = std::vector<GradientAnchor>;

    GradientCurveEditor(wxWindow* parent,
                        const wxColour& color_low  = wxColour(217, 217, 217),
                        const wxColour& color_high = wxColour(217, 217, 217));

    // Replace the entire point list. The widget enforces x in [0,1], y in [0,1],
    // sorts by x, and clamps the first / last x to 0 / 1. Tangent overrides are
    // preserved as-is (NaN entries continue to use PCHIP defaults).
    void set_points(const PointList& pts);
    const PointList& get_points() const { return m_points; }

    void set_colors(const wxColour& color_low, const wxColour& color_high);

    // Which curve currently responds to drag / add / delete and is drawn with the thick stroke.
    // 0 = first component (color_low), 1 = second component (color_high). Storage layer is
    // unaffected: m_points always represents component 0's ratio.
    void set_selected_curve(int curve_idx);
    int  get_selected_curve() const { return m_selected_curve; }

    // Reset to a two-point linear curve from y0 at t=0 to y1 at t=1.
    // Clears all tangent overrides.
    void reset_to_linear(double y0, double y1);
    // Flip the curve top to bottom (all y -> 1 - y; tangents negated to mirror shape).
    void reverse();

private:
    enum class DragMode {
        None,    // nothing armed
        Anchor,  // dragging an anchor (either existing or just inserted from a line hit)
    };

    void normalize_points();
    void emit_changed();

    void on_paint(wxPaintEvent& evt);
    void on_left_down(wxMouseEvent& evt);
    void on_left_up(wxMouseEvent& evt);
    void on_right_down(wxMouseEvent& evt);
    void on_motion(wxMouseEvent& evt);
    void on_leave(wxMouseEvent& evt);
    void on_size(wxSizeEvent& evt);

    // Coordinate mapping between data (x, y in [0,1]) and pixels in plot area.
    wxRect plot_rect() const;
    wxPoint data_to_px(double x, double y) const;
    void    px_to_data(int px, int py, double& x, double& y) const;
    // Anchor hit test for the currently-selected curve (uses translated visual y).
    int     hit_test(int px, int py) const; // returns point index or -1
    // Line-body hit test across both curves. Returns 0/1 for which curve was hit, -1 if none.
    // Prefers the selected curve when both are within threshold. seg_out (when non-null)
    // receives the left-anchor index of the segment that was hit on the returned curve;
    // on_left_down uses it to know where in m_points to insert a freshly-added anchor.
    int     hit_test_curve(int px, int py, int* seg_out = nullptr) const;

    // Sample the curve in stored space (component 0) at x.
    double sample_curve_y(double x) const;

    // Symmetric translation between visual y (what the user sees / clicks) and stored y
    // (component 0's ratio in m_points).
    static double to_stored_y(int curve_idx, double visual_y) {
        return (curve_idx == 0) ? visual_y : (1.0 - visual_y);
    }
    static double to_visual_y(int curve_idx, double stored_y) {
        return (curve_idx == 0) ? stored_y : (1.0 - stored_y);
    }

    PointList m_points;
    wxColour  m_color_low;
    wxColour  m_color_high;

    int      m_selected_curve = 0;
    DragMode m_drag_mode      = DragMode::None;
    int      m_drag_idx       = -1;   // valid when m_drag_mode == Anchor
    bool     m_dragged_moved  = false;
};

// Custom event raised when the curve is edited (drag / add / remove / reset / reverse).
wxDECLARE_EVENT(wxEVT_GRADIENT_CURVE_CHANGED, wxCommandEvent);

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GradientCurveEditor_hpp_
