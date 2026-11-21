//
//  wxMediaCtrl3.h
//  libslic3r_gui
//
//  Created by cmguo on 2024/6/22.
//

#ifndef wxMediaCtrl3_h
#define wxMediaCtrl3_h

#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/string.h>
#include <cstddef>
#include <memory>
#include <mutex>
#include "wx/window.h"
#include "wx/bitmap.h"
#include "wx/uri.h"
#include "wx/mediactrl.h"
#include "IMediaController.hpp"

wxDECLARE_EVENT(EVT_MEDIA_CTRL_STAT, wxCommandEvent);

void wxMediaCtrl_OnSize(wxWindow * ctrl, wxSize const & videoSize, int width, int height);

#define BAMBU_DYNAMIC
#include <atomic>
#include <condition_variable>
#include <thread>
#include <wx/image.h>
#include "Printer/BambuTunnel.h"
#include "libslic3r/DropOldestQueue.hpp"

class AVVideoDecoder;

class wxMediaCtrl3 : public wxWindow, public Slic3r::GUI::IMediaController, BambuLib
{
public:
    wxMediaCtrl3(wxWindow *parent);

    ~wxMediaCtrl3() override;

    void Load(wxURI url) override;

    void Play() override;

    void Stop() override;

    // Render frames supplied by a controller which owns its own transport.
    // The frame is copied while m_mutex is held; callers may release it after
    // this method returns.
    void SetExternalFrame(const wxImage& frame, wxSize videoSize);
#ifdef _WIN32
    void SetExternalFrame(const wxBitmap& frame, wxSize videoSize);
#endif
    void BeginExternalStream();
    void EndExternalStream();

    void SetIdleImage(wxString const & image);

    wxMediaState GetState() override;

    int GetLastError() const override;

    wxSize GetVideoSize() const override;

protected:
    DECLARE_EVENT_TABLE()

    void paintEvent(wxPaintEvent &evt);

    wxSize DoGetBestSize() const override;

    void DoSetSize(int x, int y, int width, int height, int sizeFlags) override;

    static void bambu_log(void *ctx, int level, tchar const *msg);
    static int ffmpeg_interrupt_callback(void *opaque);

    void PlayThread();
    int PlayFfmpeg(std::shared_ptr<wxURI> const &url, std::unique_lock<std::mutex> &lock);
    void DisplayThread(std::shared_ptr<wxURI> url, int frame_rate, size_t queue_capacity);
    void RequestRefresh();

    void NotifyStopped();

private:
#ifdef _WIN32
    using Frame = wxBitmap;
#else
    using Frame = wxImage;
#endif

    wxString m_idle_image;
    wxMediaState m_state  = wxMEDIASTATE_STOPPED;
    int m_error  = 0;
    wxSize m_video_size = wxDefaultSize;
    wxSize m_frame_size = wxDefaultSize;
    Frame m_frame;

    std::shared_ptr<wxURI> m_url;
    std::shared_ptr<wxURI> m_active_url;
    bool m_external = false;
    mutable std::mutex m_mutex;
    std::condition_variable m_cond;
    std::thread m_thread;
    std::atomic_bool m_refresh_pending{false};

    // Tunnel streams: the play thread receives and decodes frames into this queue, and the display
    // thread shows them at the stream's frame rate. The queue drops the oldest frame when full, so a
    // display that falls behind (a busy UI, a backgrounded app) skips frames instead of running late.
    // Each frame has one owner until it is copied into m_frame under m_mutex: wxImage and wxBitmap
    // share their data through a reference count that is not thread-safe.
    Slic3r::DropOldestQueue<std::unique_ptr<Frame>> m_frame_queue{1};
    std::thread m_display_thread;
    std::atomic_bool m_display_exit{false};
};

#endif /* wxMediaCtrl3_h */
