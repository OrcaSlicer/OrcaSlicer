#include "wxMediaCtrl3.h"
#include "AVVideoDecoder.hpp"
#include "I18N.hpp"
#include "libslic3r/Utils.hpp"
#include <boost/log/trivial.hpp>
#include <wx/dcclient.h>
#ifdef __WIN32__
#include <versionhelpers.h>
#include <wx/msw/registry.h>
#include <shellapi.h>
#endif

wxDEFINE_EVENT(EVT_MEDIA_CTRL_STAT, wxCommandEvent);

BEGIN_EVENT_TABLE(wxMediaCtrl3, wxWindow)

// catch paint events
EVT_PAINT(wxMediaCtrl3::paintEvent)

END_EVENT_TABLE()

struct StaticBambuLib : BambuLib
{
    static StaticBambuLib &get(BambuLib *);
};

wxMediaCtrl3::wxMediaCtrl3(wxWindow *parent)
    : wxWindow(parent, wxID_ANY)
    , BambuLib(StaticBambuLib::get(this))
    , m_thread([this] { PlayThread(); })
{
    SetBackgroundColour("#000001ff");
}

wxMediaCtrl3::~wxMediaCtrl3()
{
    {
        std::unique_lock<std::mutex> lk(m_mutex);
        m_url.reset(new wxURI);
        m_frame = wxImage(m_idle_image);
        m_cond.notify_all();
    }
    m_thread.join();
}

void wxMediaCtrl3::Load(wxURI url)
{
    std::unique_lock<std::mutex> lk(m_mutex);
    m_video_size = wxDefaultSize;
    m_error = 0;
    m_url.reset(new wxURI(url));
    m_cond.notify_all();
}

void wxMediaCtrl3::Play()
{
    std::unique_lock<std::mutex> lk(m_mutex);
    if (m_state != wxMEDIASTATE_PLAYING) {
        m_state = wxMEDIASTATE_PLAYING;
        wxMediaEvent event(wxEVT_MEDIA_STATECHANGED);
        event.SetId(GetId());
        event.SetEventObject(this);
        wxPostEvent(this, event);
    }
}

void wxMediaCtrl3::Stop()
{
    std::unique_lock<std::mutex> lk(m_mutex);
    m_url.reset();
    m_frame = wxImage(m_idle_image);
    NotifyStopped();
    m_cond.notify_all();
    Refresh();
}

void wxMediaCtrl3::SetIdleImage(wxString const &image)
{
    if (m_idle_image == image)
        return;
    m_idle_image = image;
    if (m_url == nullptr) {
        std::unique_lock<std::mutex> lk(m_mutex);
        m_frame = wxImage(m_idle_image);
        assert(m_frame.IsOk());
        Refresh();
    }
}

wxMediaState wxMediaCtrl3::GetState()
{
    std::unique_lock<std::mutex> lk(m_mutex);
    return m_state;
}

int wxMediaCtrl3::GetLastError()
{
    std::unique_lock<std::mutex> lk(m_mutex);
    return m_error;
}

wxSize wxMediaCtrl3::GetVideoSize()
{
    std::unique_lock<std::mutex> lk(m_mutex);
    return m_video_size;
}

wxSize wxMediaCtrl3::DoGetBestSize() const
{
    return {-1, -1};
}

static void adjust_frame_size(wxSize & frame, wxSize const & video, wxSize const & window)
{
    if (video.x * window.y < video.y * window.x)
        frame = { video.x * window.y / video.y, window.y };
    else
        frame = { window.x, video.y * window.x / video.x };
}

void wxMediaCtrl3::paintEvent(wxPaintEvent &evt)
{
    wxPaintDC dc(this);
    auto      size = GetSize();
    if (size.x <= 0 || size.y <= 0)
        return;
    std::unique_lock<std::mutex> lk(m_mutex);
    if (!m_frame.IsOk())
        return;
    auto size2 = m_frame.GetSize();
    if (size2.x != m_frame_size.x && size2.y == m_frame_size.y)
        size2.x = m_frame_size.x;
    auto size3 = (size - size2) / 2;
    if (size2.x != size.x && size2.y != size.y) {
        double scale = 1.;
        if (size.x * size2.y > size.y * size2.x) {
            size3 = {size.x * size2.y / size.y, size2.y};
            scale = double(size.y) / size2.y;
        } else {
            size3 = {size2.x, size.y * size2.x / size.x};
            scale = double(size.x) / size2.x;
        }
        dc.SetUserScale(scale, scale);
        size3 = (size3 - size2) / 2;
    }
    dc.DrawBitmap(m_frame, size3.x, size3.y);
}

void wxMediaCtrl3::DoSetSize(int x, int y, int width, int height, int sizeFlags)
{
    wxWindow::DoSetSize(x, y, width, height, sizeFlags);
    if (sizeFlags == wxSIZE_USE_EXISTING) return;
    wxMediaCtrl_OnSize(this, m_video_size, width, height);
    std::unique_lock<std::mutex> lk(m_mutex);
    adjust_frame_size(m_frame_size, m_video_size, GetSize());
    Refresh();
}

void wxMediaCtrl3::bambu_log(void *ctx, int level, tchar const *msg2)
{
#ifdef _WIN32
    wxString msg(msg2);
#else
    wxString msg = wxString::FromUTF8(msg2);
#endif
    if (level == 1) {
        if (msg.EndsWith("]")) {
            int n = msg.find_last_of('[');
            if (n != wxString::npos) {
                long val = 0;
                wxMediaCtrl3 *ctrl = (wxMediaCtrl3 *) ctx;
                if (msg.SubString(n + 1, msg.Length() - 2).ToLong(&val)) {
                    std::unique_lock<std::mutex> lk(ctrl->m_mutex);
                    ctrl->m_error = (int) val;
                }
            }
        } else if (msg.Contains("stat_log")) {
            wxCommandEvent evt(EVT_MEDIA_CTRL_STAT);
            wxMediaCtrl3 *ctrl = (wxMediaCtrl3 *) ctx;
            evt.SetEventObject(ctrl);
            evt.SetString(msg.Mid(msg.Find(' ') + 1));
            wxPostEvent(ctrl, evt);
        }
    }
    BOOST_LOG_TRIVIAL(info) << msg.ToUTF8().data();
}

void wxMediaCtrl3::PlayThread()
{
    using namespace std::chrono_literals;
    std::shared_ptr<wxURI> url;
    std::unique_lock<std::mutex> lk(m_mutex);
    while (true) {
        m_cond.wait(lk, [this, &url] { return m_url != url; });
        url = m_url;
        if (url == nullptr)
            continue;
        if (!url->HasScheme())
            break;
        lk.unlock();
        Bambu_Tunnel tunnel = nullptr;
        int error = Bambu_Create(&tunnel, m_url->BuildURI().ToUTF8());
        if (error == 0) {
            Bambu_SetLogger(tunnel, &wxMediaCtrl3::bambu_log, this);
            error = Bambu_Open(tunnel);
            if (error == 0)
                error = Bambu_would_block;
        }
        lk.lock();
        while (error == int(Bambu_would_block)) {
            m_cond.wait_for(lk, 100ms);
            if (m_url != url) {
                error = 1;
                break;
            }
            lk.unlock();
            error = Bambu_StartStream(tunnel, true);
            lk.lock();
        }
        Bambu_StreamInfo info;
        if (error == 0)
            error = Bambu_GetStreamInfo(tunnel, 0, &info);
        AVVideoDecoder decoder;
        int minFrameDuration = 0;
        if (error == 0) {
            decoder.open(info);
            m_video_size = { info.format.video.width, info.format.video.height };
            adjust_frame_size(m_frame_size, m_video_size, GetSize());
            minFrameDuration = 800 / info.format.video.frame_rate; // 80%
            NotifyStopped();
        }
        Bambu_Sample sample;
        while (error == 0) {
            lk.unlock();
            error = Bambu_ReadSample(tunnel, &sample);
            lk.lock();
            while (error == int(Bambu_would_block)) {
                m_cond.wait_for(lk, 100ms);
                if (m_url != url) {
                    error = 1;
                    break;
                }
                lk.unlock();
                error = Bambu_ReadSample(tunnel, &sample);
                lk.lock();
            }
            if (error == 0) {
                auto frame_size = m_frame_size;
                lk.unlock();
                decoder.decode(sample);
#ifdef _WIN32
                wxBitmap bm;
                decoder.toWxBitmap(bm, frame_size);
#else
                wxImage bm;
                decoder.toWxImage(bm, frame_size);
#endif
                lk.lock();
                if (m_url != url) {
                    error = 1;
                    break;
                }
                if (bm.IsOk()) {
                    auto now = std::chrono::system_clock::now();
                    if (m_last_PTS && (sample.decode_time - m_last_PTS) < 30000000ULL) { // 3s
                        auto next_PTS_expected = m_last_PTS_expected + std::chrono::milliseconds((sample.decode_time - m_last_PTS) / 10000ULL);
                        // The frame is late, catch up a little
                        auto next_PTS_practical = m_last_PTS_practical + std::chrono::milliseconds(minFrameDuration);
                        auto next_PTS = std::max(next_PTS_expected, next_PTS_practical);
                        if(now < next_PTS)
                            std::this_thread::sleep_until(next_PTS);
                        else
                            next_PTS = now;
                        //auto text = wxString::Format(L"wxMediaCtrl3 pts diff %ld\n", std::chrono::duration_cast<std::chrono::milliseconds>(next_PTS - next_PTS_expected).count());
                        //OutputDebugString(text);
                        m_last_PTS = sample.decode_time;
                        m_last_PTS_expected = next_PTS_expected;
                        m_last_PTS_practical = next_PTS;
                    } else {
                        // Resync
                        m_last_PTS           = sample.decode_time;
                        m_last_PTS_expected  = now;
                        m_last_PTS_practical = now;
                    }
                    m_frame = bm;
                }
                CallAfter([this] { Refresh(); });
            }
        }
        if (tunnel) {
            lk.unlock();
            Bambu_Close(tunnel);
            Bambu_Destroy(tunnel);
            tunnel = nullptr;
            lk.lock();
        }
        if (m_url == url)
            m_error = error;
        m_frame_size = wxDefaultSize;
        m_video_size = wxDefaultSize;
        NotifyStopped();
    }

}

void wxMediaCtrl3::NotifyStopped()
{
    m_state = wxMEDIASTATE_STOPPED;
    wxMediaEvent event(wxEVT_MEDIA_STATECHANGED);
    event.SetId(GetId());
    event.SetEventObject(this);
    wxPostEvent(this, event);
}
