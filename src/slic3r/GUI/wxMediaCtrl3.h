//
//  wxMediaCtrl3.h
//  libslic3r_gui
//
//  Created by cmguo on 2024/6/22.
//

#ifndef wxMediaCtrl3_h
#define wxMediaCtrl3_h

#include "wx/uri.h"
#include "wx/mediactrl.h"

wxDECLARE_EVENT(EVT_MEDIA_CTRL_STAT, wxCommandEvent);

void wxMediaCtrl_OnSize(wxWindow * ctrl, wxSize const & videoSize, int width, int height);

#define BAMBU_DYNAMIC
#include <condition_variable>
#include <thread>
#ifndef _WIN32
#include <wx/image.h>
#endif
#include "Printer/BambuTunnel.h"

class AVVideoDecoder;

class wxMediaCtrl3 : public wxWindow, BambuLib
{
public:
    wxMediaCtrl3(wxWindow *parent);

    ~wxMediaCtrl3();

    void Load(wxURI url);

    void Play();

    void Stop();

    void SetIdleImage(wxString const & image);

    wxMediaState GetState();

    int GetLastError();

    wxSize GetVideoSize();

protected:
    DECLARE_EVENT_TABLE()

    void paintEvent(wxPaintEvent &evt);

    wxSize DoGetBestSize() const override;

    void DoSetSize(int x, int y, int width, int height, int sizeFlags) override;

    static void bambu_log(void *ctx, int level, tchar const *msg);

    void PlayThread();

    void NotifyStopped();

private:
    wxString m_idle_image;
    wxMediaState m_state  = wxMEDIASTATE_STOPPED;
    int m_error  = 0;
    wxSize m_video_size = wxDefaultSize;
    wxSize m_frame_size = wxDefaultSize;
#ifdef _WIN32
    wxBitmap m_frame;
#else
    wxImage m_frame;
#endif

    std::shared_ptr<wxURI> m_url;
    std::uint64_t m_last_PTS{0};
    std::chrono::system_clock::time_point m_last_PTS_expected;
    std::chrono::system_clock::time_point m_last_PTS_practical;
    std::mutex m_mutex;
    std::condition_variable m_cond;
    std::thread m_thread;
};

#endif /* wxMediaCtrl3_h */
