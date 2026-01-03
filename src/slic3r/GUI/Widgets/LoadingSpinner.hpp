#ifndef ORCASLICER_LOADINGSPINNER_HPP
#define ORCASLICER_LOADINGSPINNER_HPP
#include <chrono>
#include "slic3r/GUI/BitmapCache.hpp"
#include "wx/control.h"

namespace Slic3r::GUI {
class LoadingSpinner : public wxControl
{
public:
    LoadingSpinner(wxWindow* parent, const wxSize& size);

    void Start();
    void Stop();
    void SetUpdateInterval(int update_interval_ms);
    void SetAnimationDuration(int duration_ms);

    bool Show(bool show) override;

protected:
    void OnPaint(wxPaintEvent& event);
    void OnTimer(wxTimerEvent& event);
    void OnSizeChanged(wxSizeEvent& event);

    wxTimer                               m_timer;
    int                                   m_timer_interval;
    int                                   m_animation_duration;
    std::chrono::steady_clock::time_point m_timer_start;
    BitmapCache                           m_bitmap_cache;
    wxImage                               m_base_image;
    wxImage                               m_rendered_image;
    wxPoint                               m_center_of_rotation;
};

} // namespace Slic3r::GUI

#endif // ORCASLICER_LOADINGSPINNER_HPP
