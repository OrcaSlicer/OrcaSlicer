#ifndef slic3r_MarkdownTip_hpp_
#define slic3r_MarkdownTip_hpp_

#include <wx/popupwin.h>
#include <wx/timer.h>
#include <wx/webview.h>


namespace Slic3r { namespace GUI {

class MarkdownTip : public wxPopupTransientWindow
{
public:
    static bool ShowTip(std::string const &tip, std::string const &tooltip, wxPoint pos);

    static void ExitTip();

    static void Reload();

    static void Recreate(wxWindow *parent);

    static wxWindow* AttachTo(wxWindow * parent);

    static wxWindow* DetachFrom(wxWindow * parent);

private:
    static MarkdownTip* markdownTip(bool create = true);

    MarkdownTip();

    ~MarkdownTip();

    void LoadStyle();

    bool ShowTip(wxPoint pos, std::string const &tip, std::string const & tooltip);

    std::string LoadTip(std::string const &tip, std::string const &tooltip);

    void RunScript(std::string const& script);

private:
    wxWebView* CreateTipView(wxWindow* parent);

    void OnLoaded(wxWebViewEvent& event);

    void OnTitleChanged(wxWebViewEvent& event);

    void OnError(wxWebViewEvent& event);

    void OnTimer(wxTimerEvent& event);
    
private:
    wxWebView * _tipView = nullptr;
    std::string _lastTip;
    std::string _pendingScript = " ";
    std::string _language;
    wxPoint _requestPos;
    double _lastHeight = 0;
    wxTimer* _timer = nullptr;
    bool _hide = false;
    bool _data_dir = false;
};

}
}

#endif
