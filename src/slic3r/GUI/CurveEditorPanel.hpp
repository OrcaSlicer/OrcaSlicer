#pragma once

#include <functional>
#include <optional>
#include <vector>
#include <wx/gdicmn.h>
#include <wx/panel.h>
#include <wx/recguard.h>
#include <wx/string.h>
#include <wx/timer.h>
#include "libslic3r/CurveModel.hpp"

namespace Slic3r::GUI {

using CurveEditorView = CurveView;

struct CurveEditorAppearance {
    wxString x_label;
    wxString y_label;
    std::optional<double> reference_y;
    wxString reference_label;
    double fill_min = 0.0;
    double fill_max = 1.0;
};

// A numeric XY plot. It owns interaction and presentation, not model constraints or serialization.
class CurveEditorPanel : public wxPanel
{
public:
    using Interpolator = CurveModel::Interpolator;
    CurveEditorPanel(wxWindow* parent, const CurveEditorAppearance& appearance, const CurveViewLimits& view_limits);
    ~CurveEditorPanel() override;
    void set_data(const std::vector<double>& x, const std::vector<double>& y,
                  const std::vector<wxString>& tooltips, Interpolator interpolate);
    void set_view(const CurveEditorView& view);
    void set_view_limits(const CurveViewLimits& limits);
    const CurveEditorView& view() const { return m_view; }
    void select_point(int row);
    void finish_drag();
    void set_touchpad_controls(bool enabled);
    bool has_automatic_touchpad_controls() const { return m_automatic_touchpad_controls; }

    std::function<void()> before_drag;
    std::function<void(int)> on_select;
    std::function<void(int, double, double)> on_move;
    // Apply both offsets in model units together, once per navigation frame.
    std::function<void(double, double)> on_pan;
    std::function<void(double, bool, double)> on_zoom;

private:
    wxRect chart_rect() const;
    wxPoint chart_point(double x, double y, const wxRect& plot) const;
    bool point_visible(double x, double y) const;
    int hit_test(const wxPoint& position) const;
    void drag_point(const wxPoint& position);
    void zoom_view(double steps, bool vertical, const wxPoint& position);
    void pinch_zoom(double factor, const wxPoint& position);
    struct NativeScroll {
        bool touchpad;
        std::optional<double> pixels;
    };
    NativeScroll scroll_input(bool horizontal) const;
#ifdef __WXOSX__
    std::optional<NativeScroll> mac_scroll_input(bool horizontal) const;
#endif
    void queue_pan(double pixels_x, double pixels_y);
    void flush_pan();
    void cancel_pan();
    void paint_chart();

    CurveEditorAppearance m_appearance;
    CurveViewLimits m_view_limits;
    CurveEditorView m_view;
    std::vector<double> m_x;
    std::vector<double> m_y;
    std::vector<wxString> m_tooltips;
    Interpolator m_interpolate;
    int m_selected_point = -1;
    int m_dragged_point = -1;
    int m_hovered_point = -1;
    wxPoint m_drag_start;
    wxPoint m_drag_previous;
    double m_drag_x = 0.0;
    double m_drag_y = 0.0;
    wxRecursionGuardFlag m_zoom_depth = 0;
    bool m_touchpad_controls = false;
    bool m_automatic_touchpad_controls = false;
    bool m_pan_gesture_active = false;
    bool m_zoom_gesture_active = false;
    wxTimer m_pan_timer;
    double m_pending_pan_x = 0.0;
    double m_pending_pan_y = 0.0;
    double m_gesture_zoom_factor = 1.0;
};

} // namespace Slic3r::GUI
