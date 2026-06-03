#pragma once
#include <wx/panel.h>
#include <wx/colour.h>
#include <vector>
#include <functional>

namespace Slic3r { namespace GUI {

// ---------------------------------------------------------------------------
// MixedGradientSelector
//
// A small horizontal panel that renders a two-colour gradient (or a
// multi-colour preview in "multi mode") and lets the user drag a marker
// to pick a blend percentage.  In multi mode the panel renders coloured
// triangles showing corner weights and emits wxEVT_BUTTON on click so the
// owner can open MixedGradientWeightsDialog.
//
// Extracted from FullSpectrum Plater.cpp:4290-4505.
// ---------------------------------------------------------------------------
class MixedGradientSelector : public wxPanel
{
public:
    MixedGradientSelector(wxWindow *parent,
                          const wxColour &left,
                          const wxColour &right,
                          int             value_percent);

    ~MixedGradientSelector() override;

    // Current blend value 0-100.
    int  value()        const { return m_value; }
    bool is_multi_mode() const { return m_multi_mode; }

    // Switch to two-colour gradient mode.
    void set_colors(const wxColour &left, const wxColour &right);

    // Switch to multi-colour preview mode (>= 3 corner colours required).
    void set_multi_preview(const std::vector<wxColour> &corner_colors,
                           const std::vector<int>       &weights);

private:
    wxRect gradient_rect() const;
    int    value_from_x(int x) const;
    void   update_from_x(int x, bool notify);

    void on_paint(wxPaintEvent &evt);
    void on_left_down(wxMouseEvent &evt);
    void on_left_up(wxMouseEvent &evt);
    void on_mouse_move(wxMouseEvent &evt);
    void on_capture_lost(wxMouseCaptureLostEvent &evt);

    wxColour              m_left;
    wxColour              m_right;
    bool                  m_multi_mode   { false };
    std::vector<wxColour> m_multi_colors;
    std::vector<int>      m_multi_weights;
    int                   m_value        { 50 };
    bool                  m_dragging     { false };
};

} } // namespace Slic3r::GUI
