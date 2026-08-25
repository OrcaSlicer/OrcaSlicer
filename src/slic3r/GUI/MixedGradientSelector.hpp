#pragma once

#include "GUI_App.hpp"
#include "Widgets/Label.hpp"
#include "I18N.hpp"
#include "libslic3r/filament_mixer.h"

#include <wx/panel.h>
#include <wx/dcbuffer.h>

#include <algorithm>
#include <vector>

namespace Slic3r { namespace GUI {

wxColour blend_pair_filament_mixer(const wxColour &left, const wxColour &right, float t);

class MixedGradientSelector : public wxPanel
{
public:
    // Min/max limits for ratio mode (in percent)
    static constexpr int MIN_RATIO_PERCENT = 10;
    static constexpr int MAX_RATIO_PERCENT = 90;

    MixedGradientSelector(wxWindow *parent, const wxColour &left, const wxColour &right, int value_percent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
        , m_left(left)
        , m_right(right)
        , m_value(std::clamp(value_percent, MIN_RATIO_PERCENT, MAX_RATIO_PERCENT))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(wxSize(FromDIP(96), FromDIP(24)));
        Bind(wxEVT_PAINT,              &MixedGradientSelector::on_paint,        this);
        Bind(wxEVT_LEFT_DOWN,          &MixedGradientSelector::on_left_down,    this);
        Bind(wxEVT_LEFT_UP,            &MixedGradientSelector::on_left_up,      this);
        Bind(wxEVT_MOTION,             &MixedGradientSelector::on_mouse_move,   this);
        Bind(wxEVT_MOUSE_CAPTURE_LOST, &MixedGradientSelector::on_capture_lost, this);
    }

    ~MixedGradientSelector() override
    {
        if (HasCapture())
            ReleaseMouse();
    }

    int  value()         const { return m_value; }
    bool is_multi_mode() const { return m_multi_mode; }

    void set_value(int value_percent)
    {
        m_value = std::clamp(value_percent, m_min_percent, m_max_percent);
        Refresh();
    }

    void set_colors(const wxColour &left, const wxColour &right)
    {
        m_left  = left;
        m_right = right;
        m_multi_mode = false;
        m_multi_colors.clear();
        m_multi_weights.clear();
        Refresh();
    }

    void set_multi_preview(const std::vector<wxColour> &corner_colors, const std::vector<int> &weights)
    {
        m_multi_mode    = corner_colors.size() >= 3;
        m_multi_colors  = corner_colors;
        m_multi_weights = weights;
        Refresh();
    }

    void set_min_max(int min_pct, int max_pct)
    {
        m_min_percent = std::max(0, min_pct);
        m_max_percent = std::min(100, max_pct);
        m_value = std::clamp(m_value, m_min_percent, m_max_percent);
        Refresh();
    }

private:
    wxRect gradient_rect() const;
    int    value_from_x(int x) const;
    void   update_from_x(int x, bool notify);

    void on_paint(wxPaintEvent &);
    void on_left_down(wxMouseEvent &evt);
    void on_left_up(wxMouseEvent &evt);
    void on_mouse_move(wxMouseEvent &evt);
    void on_capture_lost(wxMouseCaptureLostEvent &);

    wxColour              m_left;
    wxColour              m_right;
    bool                  m_multi_mode    {false};
    std::vector<wxColour> m_multi_colors;
    std::vector<int>      m_multi_weights;
    int                   m_value         {50};
    bool                  m_dragging      {false};
    int                   m_min_percent   {MIN_RATIO_PERCENT};
    int                   m_max_percent   {MAX_RATIO_PERCENT};
};

}} // namespace Slic3r::GUI
