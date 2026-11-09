//
//  wxMediaCtrl3.h
//  libslic3r_gui
//
//  Created by cmguo on 2024/6/22.
//

#ifndef wxMediaCtrl3_h
#define wxMediaCtrl3_h

#include <chrono>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/string.h>
#include <memory>
#include <mutex>
#include <cstdint>
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

class AVVideoDecoder;
class wxDC;

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

    // Unlike SDK log diagnostics, this is nonzero only after playback actually exits with an error.
    int GetPlaybackError() const;

    // Stream dimensions may be known before the first decoded picture is available.
    bool HasVideoFrame() const;

    wxSize GetVideoSize() const override;

protected:
    DECLARE_EVENT_TABLE()

    void paintEvent(wxPaintEvent &evt);
    void draw_frame(wxDC &dc, wxSize size);

    wxSize DoGetBestSize() const override;

    void DoSetSize(int x, int y, int width, int height, int sizeFlags) override;

    static void bambu_log(void *ctx, int level, tchar const *msg);
    static int ffmpeg_interrupt_callback(void *opaque);

    void PlayThread();
    int PlayFfmpeg(std::shared_ptr<wxURI> const &url, std::unique_lock<std::mutex> &lock);

    void NotifyStopped();

private:
    void queue_refresh();

    wxString m_idle_image;
    wxMediaState m_state  = wxMEDIASTATE_STOPPED;
    int m_error  = 0;
    int m_playback_error = 0;
    bool m_has_video_frame = false;
    wxSize m_video_size = wxDefaultSize;
    wxSize m_frame_size = wxDefaultSize;
#ifdef _WIN32
    wxBitmap m_frame;
#else
    wxImage m_frame;
#endif

    std::shared_ptr<wxURI> m_url;
    std::shared_ptr<wxURI> m_active_url;
    bool m_external = false;
    std::uint64_t m_last_PTS{0};
    std::chrono::steady_clock::time_point m_last_PTS_expected;
    std::chrono::steady_clock::time_point m_last_PTS_practical;
    mutable std::mutex m_mutex;
    std::condition_variable m_cond;
    std::thread m_thread;
    std::atomic_bool m_refresh_pending{false};
};

#endif /* wxMediaCtrl3_h */
