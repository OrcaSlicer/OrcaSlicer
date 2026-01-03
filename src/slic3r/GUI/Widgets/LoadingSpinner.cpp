#include "LoadingSpinner.hpp"

#include "slic3r/GUI/GUI_App.hpp"

namespace Slic3r::GUI {
LoadingSpinner::LoadingSpinner(
    wxWindow* parent, const wxSize& size)
        : m_timer_interval(20), m_animation_duration(1200)
{
    Create(parent, wxID_ANY, wxDefaultPosition, size, wxNO_BORDER);

    wxWindowBase::SetBackgroundColour(*wxWHITE);
    wxGetApp().UpdateDarkUI(this);

    // An odd size causes jittering
    const auto bmp_size = size / 2 * 2;
    m_base_image = m_bitmap_cache.load_svg("loading_spinner", bmp_size.GetWidth(), bmp_size.GetHeight())->ConvertToImage();
    m_rendered_image = m_base_image;
    m_timer.SetOwner(this);

    this->Bind(wxEVT_PAINT, &LoadingSpinner::OnPaint, this);
    this->Bind(wxEVT_TIMER, &LoadingSpinner::OnTimer, this);
    this->Bind(wxEVT_SIZE, &LoadingSpinner::OnSizeChanged, this);

    this->Start();
}

void LoadingSpinner::Start()
{
    m_timer_start = std::chrono::steady_clock::now();
    m_timer.Start(m_timer_interval);
}

void LoadingSpinner::Stop()
{
    m_timer.Stop();
}

void LoadingSpinner::SetUpdateInterval(const int update_interval_ms)
{
    m_timer_interval = update_interval_ms;
}

void LoadingSpinner::SetAnimationDuration(const int duration_ms)
{
    m_animation_duration = duration_ms;
}

void LoadingSpinner::OnPaint(wxPaintEvent& event)
{
    wxPaintDC dc(this);
    dc.Clear();

    wxRealPoint point(m_rendered_image.GetWidth() / 2., m_rendered_image.GetHeight() / 2.);
    point -= wxRealPoint(this->GetSize().GetWidth() / 2., this->GetSize().GetHeight() / 2.);
    point = wxRealPoint(point.x * -1, point.y * -1);
    const wxBitmap bitmap(m_rendered_image);
    dc.DrawBitmap(bitmap, point);
}

void LoadingSpinner::OnTimer(wxTimerEvent& event)
{
    const auto start_time_diff = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_timer_start).count();
    const auto current_animation_progress = static_cast<double>(start_time_diff % m_animation_duration) / m_animation_duration;

    static constexpr double RAD = 2 * M_PI;
    m_rendered_image = m_base_image.Rotate(RAD * (1 - current_animation_progress), m_center_of_rotation);
    Refresh();
}

void LoadingSpinner::OnSizeChanged(wxSizeEvent& event)
{
    const auto size = event.GetSize() / 2 * 2;
    m_base_image = m_bitmap_cache.load_svg("loading_spinner", size.GetWidth(), size.GetHeight())->ConvertToImage();
    m_center_of_rotation = wxPoint(size.GetWidth() / 2, size.GetHeight() / 2);
}

bool LoadingSpinner::Show(bool show)
{
    if (show)
        this->Start();
    else
        this->Stop();

    return wxWindow::Show(show);
}
} // namespace Slic3r::GUI