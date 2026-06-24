#pragma once

#include "libslic3r/filament_mixer.h"

#include <wx/panel.h>
#include <wx/dcbuffer.h>
#include <wx/timer.h>

#include <algorithm>
#include <vector>

namespace Slic3r { namespace GUI {
wxColour blend_multi_filament_mixer(const std::vector<wxColour>& colors, const std::vector<double>& weights);

class MixedFilamentColorMapPanel : public wxPanel
{
public:
    MixedFilamentColorMapPanel(wxWindow*                        parent,
                               const std::vector<unsigned int>& filament_ids,
                               const std::vector<wxColour>&     palette,
                               const std::vector<int>&          initial_weights,
                               const wxSize&                    min_size);

    ~MixedFilamentColorMapPanel() override;

    std::vector<int> normalized_weights() const { return m_weights; }

    wxColour selected_color() const;

    void set_normalized_weights(const std::vector<int>& weights, bool notify);

    void set_min_component_percent(int min_component_percent);

private:
    enum class GeometryMode { Point, Line, Triangle, TriangleWithCenter, Radial };

    struct AnchorPoint
    {
        double x{0.5};
        double y{0.5};
    };

    struct Vec2
    {
        double x{0.0};
        double y{0.0};
    };

    GeometryMode geometry_mode() const;

    wxRect canvas_rect() const;

    static Vec2 make_vec(double x, double y) { return Vec2{x, y}; }
    static Vec2 add_vec(const Vec2& lhs, const Vec2& rhs) { return Vec2{lhs.x + rhs.x, lhs.y + rhs.y}; }
    static Vec2 sub_vec(const Vec2& lhs, const Vec2& rhs) { return Vec2{lhs.x - rhs.x, lhs.y - rhs.y}; }
    static Vec2 scale_vec(const Vec2& value, double factor) { return Vec2{value.x * factor, value.y * factor}; }
    static double dot_vec(const Vec2& lhs, const Vec2& rhs) { return lhs.x * rhs.x + lhs.y * rhs.y; }
    static double length_sq(const Vec2& value) { return dot_vec(value, value); }
    static double dist_sq(const Vec2& lhs, const Vec2& rhs) { return length_sq(sub_vec(lhs, rhs)); }

    std::array<Vec2, 3> simplex_vertices() const;
    Vec2 simplex_center() const;
    std::vector<AnchorPoint> radial_anchor_points() const;
    std::vector<AnchorPoint> anchor_points() const;

    static std::array<double, 3> triangle_barycentric(const Vec2& point, const std::array<Vec2, 3>& triangle);
    static bool point_in_triangle(const Vec2& point, const std::array<Vec2, 3>& triangle);
    static Vec2 closest_point_on_segment(const Vec2& point, const Vec2& start, const Vec2& end);
    static Vec2 closest_point_on_triangle(const Vec2& point, const std::array<Vec2, 3>& triangle);

    Vec2 normalized_point_from_mouse(const wxMouseEvent& evt) const;
    Vec2 clamp_point_to_geometry(const Vec2& point) const;
    std::vector<double> simplex_weights_from_pos(const Vec2& point) const;
    Vec2 triangle_point_from_weights() const;
    void initialize_cursor_from_grid_search();
    std::vector<double> raw_weights_from_pos(double normalized_x, double normalized_y) const;
    std::vector<int> normalized_weights_from_pos(double normalized_x, double normalized_y) const;
    void initialize_cursor_from_weights();

    void emit_changed();
    void update_from_mouse(const wxMouseEvent& evt, bool notify);

    wxColour canvas_background_color() const { return GetBackgroundColour().IsOk() ? GetBackgroundColour() : wxColour(245, 245, 245); }

    bool cached_bitmap_matches(const wxSize& size, const wxColour& background) const;
    void schedule_cached_bitmap_render();
    void invalidate_cached_bitmap();
    bool color_match_raw_weights_within_range(const std::vector<double>& weights, int min_component_percent);
    void render_cached_bitmap(const wxSize& size, const wxColour& background);
    void draw_cached_bitmap(wxAutoBufferedPaintDC& dc, const wxRect& rect);

    void on_paint(wxPaintEvent&);
    void on_left_down(wxMouseEvent& evt);
    void on_left_up(wxMouseEvent& evt);
    void on_mouse_move(wxMouseEvent& evt);
    void on_capture_lost(wxMouseCaptureLostEvent&);
    void on_size(wxSizeEvent& evt);
    void on_render_timer(wxTimerEvent&);

    std::vector<wxColour> m_colors;
    std::vector<int>      m_weights;
    wxBitmap              m_cached_bitmap;
    wxSize                m_cached_bitmap_size;
    wxColour              m_cached_background;
    wxTimer               m_render_timer;
    int                   m_min_component_percent{0};
    double                m_cursor_x{0.5};
    double                m_cursor_y{0.5};
    bool                  m_dragging{false};
};

inline MixedFilamentColorMapPanel* create_color_map_panel(wxWindow*                        parent,
                                                   const std::vector<unsigned int>& filament_ids,
                                                   const std::vector<wxColour>&     palette,
                                                   const std::vector<int>&          initial_weights,
                                                   const wxSize&                    min_size)
{
    return new MixedFilamentColorMapPanel(parent, filament_ids, palette, initial_weights, min_size);
}
}} // namespace Slic3r::GUI
