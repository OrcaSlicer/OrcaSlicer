#include "MarkdownTip.hpp"
#include "GUI_App.hpp"
#include "GUI.hpp"
#include "MainFrame.hpp"
#include "Widgets/WebView.hpp"

#include "libslic3r/Utils.hpp"
#include "I18N.hpp"

#include <wx/display.h>

namespace fs = boost::filesystem;

namespace Slic3r { namespace GUI {

// CMGUO

static std::string url_encode(const std::string& value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;
    for (std::string::const_iterator i = value.begin(), n = value.end(); i != n; ++i) {
        std::string::value_type c = (*i);

        // Keep alphanumeric and other accepted characters intact
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
            continue;
        }

        // Any other characters are percent-encoded
        escaped << std::uppercase;
        escaped << '%' << std::setw(2) << int((unsigned char)c);
        escaped << std::nouppercase;
    }
    return escaped.str();
}
/*
 * Edge browser not support WebViewHandler
 * 
class MyWebViewHandler : public wxWebViewArchiveHandler
{
public:
    MyWebViewHandler() : wxWebViewArchiveHandler("tooltip") {}
    wxFSFile* GetFile(const wxString& uri) override {
        // file:///resources/tooltip/test.md
        wxFSFile* direct = wxWebViewArchiveHandler::GetFile(uri);
        if (direct)
            return direct;
        // file:///data/tooltips.zip;protocol=zip/test.md
        int n = uri.Find("resources/tooltip");
        if (n == wxString::npos)
            return direct;
        set_var_dir(data_dir());
        auto url = var("tooltips.zip");
        std::replace(url.begin(), url.end(), '\\', '/');
        auto uri2 = "file:///" + wxString(url) + ";protocol=zip" + uri.substr(n + 17);
        return wxWebViewArchiveHandler::GetFile(uri2);
    } 
};
*/

/*
TODO:
1. Fix height correctly now h * 1.25 + 50
2. Async RunScript avoid long call stack risc
3. Fetch markdown content in javascript (*)
4. Use scheme handler to support zip archive & make code tidy
*/

MarkdownTip::MarkdownTip()
    : wxPopupTransientWindow(wxGetApp().mainframe, wxBORDER_NONE)
{
    wxBoxSizer* topsizer = new wxBoxSizer(wxVERTICAL);

    _tipView = CreateTipView(this);
    topsizer->Add(_tipView, wxSizerFlags().Expand().Proportion(1));

    SetSizer(topsizer);
    SetSize({400, 300});

    LoadStyle();

    _timer = new wxTimer;
    _timer->Bind(wxEVT_TIMER, &MarkdownTip::OnTimer, this);
}

MarkdownTip::~MarkdownTip() { delete _timer; }

void MarkdownTip::LoadStyle()
{
    _language = GUI::into_u8(GUI::wxGetApp().current_language_code());
    fs::path ph(data_dir());
    ph /= "resources/tooltip/common/styled.html";
    _data_dir = true;
    if (!fs::exists(ph)) {
        ph = resources_dir();
        ph /= "tooltip/styled.html";
        _data_dir = false;
    }
    auto url = ph.string();
    std::replace(url.begin(), url.end(), '\\', '/');
    url = "file:///" + url;
    _tipView->LoadURL(from_u8(url));
    _lastTip.clear();
}

bool MarkdownTip::ShowTip(wxPoint pos, std::string const &tip, std::string const &tooltip)
{
    if (tip.empty()) {
        if (_tipView->GetParent() != this)
            return false;
        if (pos.x) {
            _hide = true;
            BOOST_LOG_TRIVIAL(info) << "MarkdownTip::ShowTip: hide soon on empty tip.";
            this->Hide();
        }
        else if (!_hide) {
            _hide = true;
            BOOST_LOG_TRIVIAL(info) << "MarkdownTip::ShowTip: start hide timer (300)...";
            _timer->StartOnce(300);
        }
        return false;
    }
    bool tipChanged = _lastTip != tip;
    if (tipChanged) {
        auto content = LoadTip(tip, tooltip);
        if (content.empty()) {
            _hide = true;
            this->Hide();
            BOOST_LOG_TRIVIAL(info) << "MarkdownTip::ShowTip: hide soon on empty content.";
            return false;
        }
        auto script = "window.showMarkdown('" + url_encode(content) + "', true);";
        if (!_pendingScript.empty()) {
            _pendingScript = script;
        }
        else {
            RunScript(script);
        }
        _lastTip = tip;
        if (_tipView->GetParent() == this)
            this->Hide();
    }
    if (_tipView->GetParent() == this) {
        wxSize size = wxDisplay(this).GetClientArea().GetSize();
        _requestPos = pos;
        if (pos.y + this->GetSize().y > size.y)
            pos.y = size.y - this->GetSize().y;
        this->SetPosition(pos);
        if (tipChanged || _hide) {
            _hide = false;
            BOOST_LOG_TRIVIAL(info) << "MarkdownTip::ShowTip: start show timer (500)...";
            _timer->StartOnce(500);
        }
    }
    return true;
}

std::string MarkdownTip::LoadTip(std::string const &tip, std::string const &tooltip)
{
    fs::path ph;
    wxString file;
    wxFile   f;
    if (_data_dir) {
        if (!_language.empty()) {
            ph = data_dir();
            ph /= "resources/tooltip/" + _language +  "/" + tip + ".md";
            file = from_u8(ph.string());
            if (wxFile::Exists(file) && f.Open(file)) {
                std::string content(f.Length(), 0);
                f.Read(&content[0], content.size());
                return content;
            }
        }
        ph = data_dir();
        ph /= "resources/tooltip/common/" + tip + ".md";
        file = from_u8(ph.string());
        if (wxFile::Exists(file) && f.Open(file)) {
            std::string content(f.Length(), 0);
            f.Read(&content[0], content.size());
            return content;
        }
    }
    /*
    file = var("tooltips.zip");
    if (wxFile::Exists(file) && f.Open(file)) {
        wxFileInputStream fs(f);
        wxZipInputStream zip(fs);
        file = tip + ".md";
        while (auto e = zip.GetNextEntry()) {
            if (e->GetName() == file) {
                if (zip.OpenEntry(*e)) {
                    std::string content(f.Length(), 0);
                    zip.Read(&content[0], content.size());
                    return content;
                }
                break;
            }
        }
    }
    */
    ph = resources_dir();
    ph /= "tooltip/" + _language + "/" + tip + ".md";
    file = from_u8(ph.string());
    if (wxFile::Exists(file) && f.Open(file)) {
        std::string content(f.Length(), 0);
        f.Read(&content[0], content.size());
        return content;
    }
    ph = resources_dir();
    ph /= "tooltip/" + tip + ".md";
    file = from_u8(ph.string());
    if (wxFile::Exists(file) && f.Open(file)) {
        std::string content(f.Length(), 0);
        f.Read(&content[0], content.size());
        return content;
    }
    if (!tooltip.empty()) return "#### " + _utf8(tip) + "\n" + tooltip;
    return (_tipView->GetParent() == this && tip.empty()) ? "" : LoadTip("", "");
}

void MarkdownTip::RunScript(std::string const& script)
{
    WebView::RunScript(_tipView, script);
}

wxWebView* MarkdownTip::CreateTipView(wxWindow* parent)
{
    wxWebView *tipView = WebView::CreateWebView(parent, "");
    Bind(wxEVT_WEBVIEW_LOADED, &MarkdownTip::OnLoaded, this);
    Bind(wxEVT_WEBVIEW_TITLE_CHANGED, &MarkdownTip::OnTitleChanged, this);
    Bind(wxEVT_WEBVIEW_ERROR, &MarkdownTip::OnError, this);
    return tipView;
}

void MarkdownTip::OnLoaded(wxWebViewEvent& event)
{
}

void MarkdownTip::OnTitleChanged(wxWebViewEvent& event)
{
    if (!_pendingScript.empty()) {
        RunScript(_pendingScript);
        _pendingScript.clear();
        return;
    }
#ifdef __linux__
    wxString str = "0";
#else
    wxString str = event.GetString();
#endif
    double height = 0;
    if (str.ToDouble(&height)) {
        if (height > _lastHeight - 10 && height < _lastHeight + 10)
            return;
        _lastHeight = height;
        height *= 1.25; height += 50;
        wxSize size = wxDisplay(this).GetClientArea().GetSize();
        if (height > size.y)
            height = size.y;
        wxPoint pos = _requestPos;
        if (pos.y + height > size.y)
            pos.y = size.y - height;
        this->SetSize({ 400, (int)height });
        this->SetPosition(pos);
    }
}
void MarkdownTip::OnError(wxWebViewEvent& event)
{
}

void MarkdownTip::OnTimer(wxTimerEvent& event)
{
    if (_hide) {
        wxPoint pos = ScreenToClient(wxGetMousePosition());
        if (GetClientRect().Contains(pos)) {
            BOOST_LOG_TRIVIAL(info) << "MarkdownTip::OnTimer: restart hide timer...";
            _timer->StartOnce();
            return;
        }
        BOOST_LOG_TRIVIAL(info) << "MarkdownTip::OnTimer: hide.";
        this->Hide();
    } else {
        BOOST_LOG_TRIVIAL(info) << "MarkdownTip::OnTimer: show.";
        this->Show();
    }
}

MarkdownTip* MarkdownTip::markdownTip(bool create)
{
    static MarkdownTip * markdownTip = nullptr;
    if (markdownTip == nullptr && create)
        markdownTip = new MarkdownTip;
    return markdownTip;
}

bool MarkdownTip::ShowTip(std::string const& tip, std::string const & tooltip, wxPoint pos)
{
#ifdef NDEBUG
    return false;
#endif
    return markdownTip()->ShowTip(pos, tip, tooltip);
}

void MarkdownTip::ExitTip()
{
    //if (auto tip = markdownTip(false))
    //    tip->Destroy();
}

void MarkdownTip::Reload()
{
    if (auto tip = markdownTip(false)) 
        tip->LoadStyle();
}

void MarkdownTip::Recreate(wxWindow *parent)
{
    if (auto tip = markdownTip(false)) {
        tip->Reparent(parent);
        tip->LoadStyle(); // switch language
    }
}

wxWindow* MarkdownTip::AttachTo(wxWindow* parent)
{
    MarkdownTip& tip = *markdownTip();
    tip._tipView = tip.CreateTipView(parent);
    tip._pendingScript = " ";
    return tip._tipView;
}

wxWindow* MarkdownTip::DetachFrom(wxWindow* parent)
{
    MarkdownTip& tip = *markdownTip();
    if (tip._tipView->GetParent() == parent) {
        tip.Destroy();
    }
    return NULL;
}

}
}
