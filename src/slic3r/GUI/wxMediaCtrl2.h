//
//  wxMediaCtrl2.h
//  libslic3r_gui
//
//  Created by cmguo on 2021/12/7.
//

#ifndef wxMediaCtrl2_h
#define wxMediaCtrl2_h

#include "wx/uri.h"
#include "wx/mediactrl.h"

wxDECLARE_EVENT(EVT_MEDIA_CTRL_STAT, wxCommandEvent);

#ifdef __WXMAC__

class wxMediaCtrl2 : public wxWindow
{
public:
    wxMediaCtrl2(wxWindow * parent);
    
    ~wxMediaCtrl2();

    void Load(wxURI url);

    void Play();

    void Stop();

    void SetIdleImage(wxString const & image);

    wxMediaState GetState() const;

    wxSize GetVideoSize() const;

    int GetLastError() const { return m_error; }

    static constexpr wxMediaState MEDIASTATE_BUFFERING = (wxMediaState) 6;

protected:
    void DoSetSize(int x, int y, int width, int height, int sizeFlags) override;

    static void bambu_log(void const * ctx, int level, char const * msg);
    
    void NotifyStopped();

private:
    void create_player();
    void * m_player = nullptr;
    wxMediaState m_state = wxMEDIASTATE_STOPPED;
    int          m_error  = 0;
    wxSize       m_video_size{16, 9};
};

#else

class wxMediaCtrl2 : public wxMediaCtrl
{
public:
    wxMediaCtrl2(wxWindow *parent);

    void Load(wxURI url);

    void Play();

    void Stop();

    void SetIdleImage(wxString const & image);

    int GetLastError() const;

    wxSize GetVideoSize() const;

protected:
    wxSize DoGetBestSize() const override;

    void DoSetSize(int x, int y, int width, int height, int sizeFlags) override;

#ifdef __WIN32__
    WXLRESULT MSWWindowProc(WXUINT   nMsg,
                            WXWPARAM wParam,
                            WXLPARAM lParam) override;
#endif

private:
    wxString m_idle_image;
    int      m_error = 0;
    bool     m_loaded = false;
    wxSize   m_video_size{16, 9};
};

#endif

#endif /* wxMediaCtrl2_h */
