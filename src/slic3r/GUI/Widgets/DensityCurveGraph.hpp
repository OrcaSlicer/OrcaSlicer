#pragma once

#include <cstddef>
#include <functional>
#include <limits>
#include <vector>

#include <wx/colour.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/geometry.h>
#include <wx/string.h>
#include <wx/window.h>

// What a DensityCurveGraph draws: an infill density from 0 to 100% against a depth on the x axis, from 0 to x_max, with
// the points that shape it. The owner builds it from its settings.
struct DensityCurve
{
    enum class HandleKind {
        // A dot dragged in both directions.
        Point,
        // A dot drawn hollow, for a point that is only in use once dragged.
        Ring,
        // A vertical grip dragged sideways, with a line across the graph at its depth.
        Edge,
    };
    struct Handle
    {
        HandleKind kind { HandleKind::Point };
        double     x { 0. };
        // As a fraction; Edge handles sit at mid height.
        double     density { 0. };
        // Shown next to the handle while it is hovered or dragged.
        wxString   readout;
    };

    // Density at x, as a fraction.
    std::function<double(double)> density;
    double                        x_max { 10. };
    wxString                      x_title;
    wxString                      y_title;
    // A band around the curve, from density * (1 - spread) to density * (1 + spread), for a density that varies.
    double                        spread { 0. };
    std::vector<Handle>           handles;
    // Dashed lines across the graph, at these depths.
    std::vector<double>           guides;
    // A tinted area from this depth to the end of the axis, titled along the right edge, in the colour of the Edge
    // handles. Infinite for none.
    double                        shade_from { std::numeric_limits<double>::infinity() };
    wxString                      shade_title;
    // A greyed area from this depth to the end of the axis, marked by a line with a label. Infinite for none.
    double                        mask_from { std::numeric_limits<double>::infinity() };
    wxString                      mask_label;
};

// A graph of a DensityCurve whose handles the user drags. The graph only reports the drags: the owner clamps or snaps
// the values, updates its settings, and sets the curve made from them again.
class DensityCurveGraph : public wxWindow
{
public:
    struct Drag
    {
        enum class Phase {
            // Before the first move; the owner may keep its settings to restore them on Cancel.
            Begin,
            Move,
            // The handle was released.
            End,
            // The mouse was taken away mid drag: the owner restores its settings from Begin.
            Cancel,
        };
        Phase  phase;
        size_t handle;
        // Where the mouse is, the density as a fraction clamped to [0, 1]. Not clamped to the axis.
        double x;
        double density;
    };
    std::function<void(const Drag &)> on_drag;

    DensityCurveGraph(wxWindow *parent, const wxSize &min_size);
    ~DensityCurveGraph() override;

    bool AcceptsFocus() const override { return false; }

    // While a handle is dragged, the x axis keeps its extent, so the handle stays under the mouse.
    void set_curve(DensityCurve curve);
    void set_shade_colour(const wxColour &colour);
    // The x axis labels are formatted with this many decimals.
    void set_x_digits(int digits);
    void rescale();

private:
    DensityCurve m_curve;
    wxSize       m_min_size;
    wxColour     m_shade_colour { "#E14747" };
    int          m_x_digits { 0 };
    double       m_x_max { 10. };
    // Handle under the mouse and handle dragged, or none.
    static constexpr size_t None = size_t(-1);
    size_t                  m_hover { None };
    size_t                  m_drag { None };

    wxRect  plot_rect() const;
    double  tick_step() const;
    double  x_px(double x) const;
    double  y_px(double density) const;
    double  x_at(int px) const;
    double  density_at(int py) const;
    wxPoint2DDouble handle_pos(const DensityCurve::Handle &handle) const;
    size_t  hit(const wxPoint &p) const;
    void    set_hover(size_t handle);
    void    send(Drag::Phase phase, const wxPoint &p);

    void on_paint(wxPaintEvent &);
    void on_left_down(wxMouseEvent &e);
    void on_left_up(wxMouseEvent &e);
    void on_motion(wxMouseEvent &e);
};
