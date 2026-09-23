#include <catch2/catch_all.hpp>

#include <slic3r/GUI/WebMediaController.hpp>

#include <wx/webview.h>

#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::GUI;

namespace {

class StubWebView final : public wxWebView
{
public:
    bool SetBackgroundColour(const wxColour&) override
    {
        events.emplace_back("background");
        return true;
    }

    bool Create(wxWindow*, wxWindowID, const wxString&, const wxPoint&, const wxSize&, long, const wxString&) override { return true; }
    wxString GetCurrentTitle() const override { return {}; }
    wxString GetCurrentURL() const override { return {}; }
    bool IsBusy() const override { return false; }
    bool IsEditable() const override { return false; }
    void LoadURL(const wxString& url) override
    {
        events.emplace_back("url");
        loaded_url = url.ToStdString();
    }
    void Print() override {}
    void RegisterHandler(wxSharedPtr<wxWebViewHandler>) override {}
    void Reload(wxWebViewReloadFlags) override {}
    void SetEditable(bool) override {}
    void Stop() override { events.emplace_back("stop"); }
    bool CanGoBack() const override { return false; }
    bool CanGoForward() const override { return false; }
    void GoBack() override {}
    void GoForward() override {}
    void ClearHistory() override { events.emplace_back("history"); }
    void EnableHistory(bool) override {}
    wxVector<wxSharedPtr<wxWebViewHistoryItem>> GetBackwardHistory() override { return {}; }
    wxVector<wxSharedPtr<wxWebViewHistoryItem>> GetForwardHistory() override { return {}; }
    void LoadHistoryItem(wxSharedPtr<wxWebViewHistoryItem>) override {}
    bool CanSetZoomType(wxWebViewZoomType) const override { return false; }
    float GetZoomFactor() const override { return 1.0f; }
    wxWebViewZoomType GetZoomType() const override { return wxWEBVIEW_ZOOM_TYPE_LAYOUT; }
    void SetZoomFactor(float) override {}
    void SetZoomType(wxWebViewZoomType) override {}
    bool CanUndo() const override { return false; }
    bool CanRedo() const override { return false; }
    void Undo() override {}
    void Redo() override {}
    void* GetNativeBackend() const override { return nullptr; }

    bool RunScript(const wxString& javascript, wxString*) const override
    {
        events.emplace_back("script");
        script = javascript.ToStdString();
        return true;
    }

protected:
    void DoSetPage(const wxString& html, const wxString& base_url) override
    {
        events.emplace_back("page");
        page      = html.ToStdString();
        page_base = base_url.ToStdString();
    }

public:
    mutable std::vector<std::string> events;
    std::string              page;
    std::string              page_base;
    mutable std::string      script;
    std::string              loaded_url;
};

} // namespace

TEST_CASE("Web media controller tears down a snapshot lifecycle", "[WebMediaController][integration]")
{
    StubWebView         view;
    WebMediaController  controller(&view);

    controller.set_mode(CameraStreamMode::http_snapshot);
    controller.Load(wxURI("http://camera.example/frame.jpg"));
    controller.Play();

    REQUIRE(view.events.size() == 3);
    CHECK(view.events[0] == "background");
    CHECK(view.events[1] == "page");
    CHECK(view.events[2] == "page");
    CHECK(view.page.find("stopCameraRefresh") != std::string::npos);
    CHECK(view.page.find("http://camera.example/frame.jpg") != std::string::npos);

    controller.Stop();

    REQUIRE(view.events.size() == 7);
    CHECK(view.events[3] == "script");
    CHECK(view.events[4] == "stop");
    CHECK(view.events[5] == "page");
    CHECK(view.events[6] == "history");
    CHECK(view.script == "if(typeof stopCameraRefresh==='function') stopCameraRefresh();");
    CHECK(view.page.empty());
    CHECK(view.page_base == "about:blank");

    controller.Play();
    CHECK(view.page.find("http://camera.example/frame.jpg") == std::string::npos);
}
