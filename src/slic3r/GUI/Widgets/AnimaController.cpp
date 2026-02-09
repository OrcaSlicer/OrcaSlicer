#include "AnimaController.hpp"

#include <wx/dcclient.h>
#include <wx/dcgraph.h>
#ifdef __APPLE__
#include "libslic3r/MacUtils.hpp"
#endif

AnimaIcon::AnimaIcon(wxWindow *parent, wxWindowID id, std::vector<std::string> img_list, std::string img_enable, int ivt)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize), m_ivt(ivt), m_img_enable(img_enable), m_img_list(img_list)
{
    auto sizer = new wxBoxSizer(wxHORIZONTAL);
    SetBackgroundColour((wxColour(255, 255, 255)));
    m_size = 25;

    //add ScalableBitmap
    for (const auto &filename : m_img_list) m_images.emplace_back(create_scaled_bitmap(filename, this, m_size));
    m_image_enable = create_scaled_bitmap(m_img_enable, this, m_size-8);

    // show first wxStaticBitmap
    if (!m_images.empty()) m_bitmap = new wxStaticBitmap(this, wxID_ANY, m_images[0], wxDefaultPosition, wxSize(FromDIP(m_size), FromDIP(m_size)));


    m_timer = new wxTimer();
    m_timer->SetOwner(this);

    Bind(wxEVT_TIMER, [this](wxTimerEvent &) {
        if (m_timer->IsRunning() && !m_images.empty()) {
            m_current_frame = (m_current_frame + 1) % 4;
            m_bitmap->SetBitmap(m_images[m_current_frame]);
        }
    });

   m_bitmap->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &e) {
        wxMouseEvent evt(wxEVT_LEFT_DOWN);
        evt.SetEventObject(this);
        wxPostEvent(this, evt);
    });

   m_bitmap->Bind(wxEVT_ENTER_WINDOW, [this](auto &e) {
       if (!m_timer->IsRunning())
          SetCursor(wxCursor(wxCURSOR_HAND));
       else
          SetCursor(wxCursor(wxCURSOR_ARROW));
       e.Skip();
   });
   m_bitmap->Bind(wxEVT_LEAVE_WINDOW, [this](auto &e) {
       SetCursor(wxCursor(wxCURSOR_ARROW));
       e.Skip();
   });
    sizer->Add(m_bitmap, 0, wxALIGN_CENTER, 0);
    SetSizer(sizer);
	SetSize(wxSize(FromDIP(m_size), FromDIP(m_size)));
    SetMaxSize(wxSize(FromDIP(m_size), FromDIP(m_size)));
    SetMinSize(wxSize(FromDIP(m_size), FromDIP(m_size)));
    Layout();
    Fit();
}

AnimaIcon::~AnimaIcon()
{
    if (m_timer) {
        m_timer->Stop();
        delete m_timer;
        m_timer = nullptr;
    }
}

void AnimaIcon::Play()
{
    if (m_timer)
        m_timer->Start(m_ivt);
}

void AnimaIcon::Stop()
{
    m_timer->Stop();
}

bool AnimaIcon::IsPlaying()
{
    return m_timer->IsRunning();
}

void AnimaIcon::Enable()
{
    m_enable = true;
    if (m_bitmap) { m_bitmap->SetBitmap(m_image_enable); }
}


bool AnimaIcon::IsRunning() const
{
    return m_timer ? m_timer->IsRunning() : false;
}

void AnimaIcon::Rescale()
{
    m_images.clear();
    for (const auto& filename : m_img_list) m_images.emplace_back(create_scaled_bitmap(filename, this, m_size));
    m_image_enable = create_scaled_bitmap(m_img_enable, this, m_size - 8);

    if (m_img_list.empty()) {
        return;
    }

    if (IsPlaying()) {
        m_bitmap->SetBitmap(m_images[m_current_frame]);
    } else {
        if(m_enable) {
            m_bitmap->SetBitmap(m_image_enable);
        } else {
            m_bitmap->SetBitmap(m_images[0]);
        }
    }
}
