#pragma once

#include <functional>
#include <optional>
#include <vector>
#include <wx/panel.h>

namespace Slic3r::GUI {

struct CurveEditorView {
    double min_x = 0.0;
    double max_x = 1.0;
    double min_y = 0.0;
    double max_y = 1.0;
};

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
    using Interpolator = std::function<double(double)>;
    CurveEditorPanel(wxWindow* parent, const CurveEditorAppearance& appearance);
    void set_data(const std::vector<double>& x, const std::vector<double>& y,
                  const std::vector<wxString>& tooltips, Interpolator interpolate);
    void set_view(const CurveEditorView& view);
    const CurveEditorView& view() const { return m_view; }
    void select_point(int row);
    void finish_drag();

    std::function<void()> before_drag;
    std::function<void(int)> on_select;
    std::function<void(int, double, double)> on_move;

private:
    wxRect chart_rect() const;
    wxPoint chart_point(double x, double y, const wxRect& plot) const;
    bool point_visible(double x, double y) const;
    int hit_test(const wxPoint& position) const;
    void drag_point(const wxPoint& position);
    void paint_chart();

    CurveEditorAppearance m_appearance;
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
};

} // namespace Slic3r::GUI
