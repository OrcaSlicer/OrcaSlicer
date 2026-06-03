#pragma once
#include <wx/panel.h>
#include <wx/timer.h>
#include <wx/bitmap.h>
#include <wx/colour.h>
#include <vector>
#include <array>
#include <functional>

namespace Slic3r { namespace GUI {

// ---------------------------------------------------------------------------
// MixedFilamentColorMapPanel
//
// Interactive colour-map widget that lets the user pick a multi-filament
// blend by dragging a cursor across a geometry-specific gradient map.
//
// Extracted verbatim from FullSpectrum Plater.cpp:3087-3781 (Task 17).
// ---------------------------------------------------------------------------
class MixedFilamentColorMapPanel : public wxPanel
{
public:
    MixedFilamentColorMapPanel(wxWindow                        *parent,
                               const std::vector<unsigned int> &filament_ids,
                               const std::vector<wxColour>     &palette,
                               const std::vector<int>          &initial_weights,
                               const wxSize                    &min_size);

    ~MixedFilamentColorMapPanel() override;

    // Returns the current normalised per-filament weights (sum == 100).
    std::vector<int> normalized_weights() const;

    // Returns the blended wxColour that corresponds to the current cursor position.
    wxColour selected_color() const;

    // Programmatically update weights; notify==true fires wxEVT_SLIDER.
    void set_normalized_weights(const std::vector<int> &weights, bool notify);

    // Minimum per-component percentage below which the region is dimmed/striped.
    void set_min_component_percent(int min_component_percent);

private:
    // -----------------------------------------------------------------------
    // Private nested types (verbatim from FullSpectrum Plater.cpp:3160-3180)
    // -----------------------------------------------------------------------
    enum class GeometryMode {
        Point,
        Line,
        Triangle,
        TriangleWithCenter,
        Radial
    };

    struct AnchorPoint {
        double x { 0.5 };
        double y { 0.5 };
    };

    struct Vec2 {
        double x { 0.0 };
        double y { 0.0 };
    };

    // -----------------------------------------------------------------------
    // Geometry helpers (all inlined in .cpp)
    // -----------------------------------------------------------------------
    GeometryMode geometry_mode() const;
    wxRect       canvas_rect() const;

    static Vec2   make_vec(double x, double y);
    static Vec2   add_vec(const Vec2 &lhs, const Vec2 &rhs);
    static Vec2   sub_vec(const Vec2 &lhs, const Vec2 &rhs);
    static Vec2   scale_vec(const Vec2 &value, double factor);
    static double dot_vec(const Vec2 &lhs, const Vec2 &rhs);
    static double length_sq(const Vec2 &value);
    static double dist_sq(const Vec2 &lhs, const Vec2 &rhs);

    std::array<Vec2, 3>      simplex_vertices() const;
    Vec2                     simplex_center() const;
    std::vector<AnchorPoint> radial_anchor_points() const;
    std::vector<AnchorPoint> anchor_points() const;

    static std::array<double, 3> triangle_barycentric(const Vec2 &point, const std::array<Vec2, 3> &triangle);
    static bool                  point_in_triangle(const Vec2 &point, const std::array<Vec2, 3> &triangle);
    static Vec2                  closest_point_on_segment(const Vec2 &point, const Vec2 &start, const Vec2 &end);
    static Vec2                  closest_point_on_triangle(const Vec2 &point, const std::array<Vec2, 3> &triangle);

    Vec2 normalized_point_from_mouse(const wxMouseEvent &evt) const;
    Vec2 clamp_point_to_geometry(const Vec2 &point) const;

    std::vector<double> simplex_weights_from_pos(const Vec2 &point) const;
    Vec2                triangle_point_from_weights() const;
    void                initialize_cursor_from_grid_search();
    std::vector<double> raw_weights_from_pos(double normalized_x, double normalized_y) const;
    std::vector<int>    normalized_weights_from_pos(double normalized_x, double normalized_y) const;
    void                initialize_cursor_from_weights();

    // -----------------------------------------------------------------------
    // Rendering helpers
    // -----------------------------------------------------------------------
    void emit_changed();
    void update_from_mouse(const wxMouseEvent &evt, bool notify);

    wxColour canvas_background_color() const;
    bool     cached_bitmap_matches(const wxSize &size, const wxColour &background) const;
    void     schedule_cached_bitmap_render();
    void     invalidate_cached_bitmap();
    void     render_cached_bitmap(const wxSize &size, const wxColour &background);
    void     draw_cached_bitmap(wxAutoBufferedPaintDC &dc, const wxRect &rect);

    // -----------------------------------------------------------------------
    // wx event handlers
    // -----------------------------------------------------------------------
    void on_paint(wxPaintEvent &evt);
    void on_left_down(wxMouseEvent &evt);
    void on_left_up(wxMouseEvent &evt);
    void on_mouse_move(wxMouseEvent &evt);
    void on_capture_lost(wxMouseCaptureLostEvent &evt);
    void on_size(wxSizeEvent &evt);
    void on_render_timer(wxTimerEvent &evt);

    // -----------------------------------------------------------------------
    // Member variables (verbatim from FullSpectrum Plater.cpp:3761-3779)
    // -----------------------------------------------------------------------
    std::vector<wxColour> m_colors;
    std::vector<int>      m_weights;
    wxBitmap              m_cached_bitmap;
    wxSize                m_cached_bitmap_size;
    wxColour              m_cached_background;
    wxTimer               m_render_timer;
    int                   m_min_component_percent { 0 };
    double                m_cursor_x { 0.5 };
    double                m_cursor_y { 0.5 };
    bool                  m_dragging { false };
};

} } // namespace Slic3r::GUI
