// The web page helpers pull in wx, which pulls in <windows.h>; same prologue as test_mcp_ui.cpp.
#ifdef WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
#endif

#include <catch2/catch_test_macros.hpp>

#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/Mcp/McpTypes.hpp"
#include "slic3r/GUI/Mcp/ModalTracker.hpp"
#include "slic3r/GUI/Mcp/UiRefs.hpp"
#include "slic3r/GUI/Mcp/WebPages.hpp"

#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>

#include <wx/app.h>
#include <wx/button.h>
#include <wx/control.h>
#include <wx/defs.h>
#include <wx/dialog.h>
#include <wx/gdicmn.h>
#include <wx/init.h>
#include <wx/msgdlg.h>
#include <wx/sharedptr.h>
#include <wx/string.h>
#include <wx/vector.h>
#include <wx/webview.h>
#include <wx/window.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using json = nlohmann::json;
using namespace Slic3r;
using namespace Slic3r::GUI::Mcp;

namespace {

// Points resources_dir() at the repo's own tree, where the page agent is, for the lifetime of a test.
struct ScopedRepoResourcesDir
{
    std::string previous{resources_dir()};

    ScopedRepoResourcesDir() { set_resources_dir(boost::filesystem::path(PROFILES_DIR).parent_path().string()); }
    ~ScopedRepoResourcesDir() { set_resources_dir(previous); }

    ScopedRepoResourcesDir(const ScopedRepoResourcesDir&)            = delete;
    ScopedRepoResourcesDir& operator=(const ScopedRepoResourcesDir&) = delete;
};

// A web view with no browser behind it: a native child window of `parent`, as a real one is.
class StubWebView final : public wxWebView
{
public:
    explicit StubWebView(wxWindow* parent) { wxControl::Create(parent, wxID_ANY); }

    bool Create(wxWindow*, wxWindowID, const wxString&, const wxPoint&, const wxSize&, long, const wxString&) override { return true; }
    wxString GetCurrentTitle() const override { return {}; }
    wxString GetCurrentURL() const override { return {}; }
    bool IsBusy() const override { return false; }
    bool IsEditable() const override { return false; }
    void LoadURL(const wxString&) override {}
    void Print() override {}
    void RegisterHandler(wxSharedPtr<wxWebViewHandler>) override {}
    void Reload(wxWebViewReloadFlags) override {}
    void SetEditable(bool) override {}
    void Stop() override {}
    bool CanGoBack() const override { return false; }
    bool CanGoForward() const override { return false; }
    void GoBack() override {}
    void GoForward() override {}
    void ClearHistory() override {}
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
    bool RunScript(const wxString&, wxString*) const override { return false; }

protected:
    void DoSetPage(const wxString&, const wxString&) override {}
};

// wx for a test that creates windows; false where there is no display to create them on.
bool have_display()
{
#if defined(__linux__)
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY"))
        return false;
#endif
    if (wxTheApp == nullptr)
        wxApp::SetInstance(new wxApp);
    return true;
}

} // namespace

TEST_CASE("a page's lines go under its web view, in the window's logical pixels", "[Mcp]")
{
    // A page at 1.5 logical pixels per CSS pixel, whose web view is at 100,50 in its window.
    const json page{{"nodes",
                     {{{"k", 3}, {"kind", "button"}, {"label", "Next"}, {"name", "#AcceptBtn"}, {"depth", 0}, {"x", 10}, {"y", 20}, {"w", 40}, {"h", 8}},
                      {{"k", 4}, {"kind", "checkbox"}, {"label", "Agree"}, {"checked", 1}, {"enabled", false}, {"focused", true}, {"depth", 1}},
                      {{"k", "7/2/5"}, {"kind", "text"}, {"label", "Speed"}, {"value", "7"}, {"note", "relayed"}, {"depth", 2}},
                      {{"kind", "label"}, {"label", "Some text"}, {"depth", 1}}}}};
    const std::vector<PageNode> nodes = page_nodes(page, 2, wxPoint(100, 50), 1.5);
    REQUIRE(nodes.size() == 4);

    const UiNode& next = nodes[0].node;
    CHECK(next.kind == "button");
    CHECK(next.label == "Next");
    CHECK(next.name == "#AcceptBtn");
    CHECK(next.depth == 2);
    CHECK(next.x == 100 + 15);
    CHECK(next.y == 50 + 30);
    CHECK(next.w == 60);
    CHECK(next.h == 12);
    CHECK(next.checked == -1);
    CHECK(nodes[0].key == json(3));

    const UiNode& agree = nodes[1].node;
    CHECK(agree.depth == 3);
    CHECK(agree.checked == 1);
    CHECK_FALSE(agree.enabled);
    CHECK(agree.focused);

    // A relayed frame's element keeps its path as its key.
    CHECK(nodes[2].node.value == "7");
    CHECK(nodes[2].node.note == "relayed");
    CHECK(nodes[2].key == json("7/2/5"));

    // Text with no element around it has no key to act on.
    CHECK(nodes[3].key.is_null());
}

TEST_CASE("a page that sent no lines lists none", "[Mcp]")
{
    CHECK(page_nodes(json{{"note", "its page did not answer in time"}}, 1, wxPoint(), 1.0).empty());
    CHECK(page_nodes(json{{"nodes", "not a list"}}, 1, wxPoint(), 1.0).empty());
}

TEST_CASE("a URL shows a credential's value as REDACTED", "[Mcp]")
{
    // A plugin page's bridge token, in the fragment.
    CHECK(redact_url("https://rigcad.com/?host=orca#orca-token=abc123&orca-transport=pages-v1") ==
          "https://rigcad.com/?host=orca#orca-token=REDACTED&orca-transport=pages-v1");
    CHECK(redact_url("http://localhost/cb?code=xyz&state=s1") == "http://localhost/cb?code=REDACTED&state=s1");
    CHECK(redact_url("https://h/x?apiKey=1&accessToken=2&X-Amz-Signature=3") == "https://h/x?apiKey=REDACTED&accessToken=REDACTED&X-Amz-Signature=REDACTED");
    // An escaped name is read as the server decodes it, and keeps its spelling.
    CHECK(redact_url("https://h/x?access%5Ftoken=s&q=1#tok%65n=t") == "https://h/x?access%5Ftoken=REDACTED&q=1#tok%65n=REDACTED");
    CHECK(redact_url("https://idp/acs?SAMLResponse=PHNhbWw&RelayState=r") == "https://idp/acs?SAMLResponse=REDACTED&RelayState=r");
    // A fragment route's own query.
    CHECK(redact_url("app://p/#/login?access_token=t&next=/home") == "app://p/#/login?access_token=REDACTED&next=/home");
    CHECK(redact_url("https://user:pw@host:8080/p") == "https://REDACTED:REDACTED@host:8080/p");
    CHECK(redact_url("https://user@host/p?q=1") == "https://REDACTED@host/p?q=1");
    // Words of a name, not parts of words, so harmless parameters stay.
    CHECK(redact_url("https://h/search?keyword=benchy&design=3&monkey=1") == "https://h/search?keyword=benchy&design=3&monkey=1");
    CHECK(redact_url("about:blank#bottom") == "about:blank#bottom");
    CHECK(redact_url("") == "");
}

TEST_CASE("css and web locators name page elements, widget locators do not", "[Mcp]")
{
    CHECK(is_web_locator(json{{"css", "#AcceptBtn"}}));
    CHECK(is_web_locator(json{{"web", "Next"}, {"kind", "button"}, {"within", "Setup Wizard"}}));
    CHECK_FALSE(is_web_locator(json{{"label", "Next"}}));
    CHECK_FALSE(is_web_locator(json{{"class", "webview"}, {"within", "Preset Bundle"}}));
    CHECK_FALSE(is_web_locator(json("d12")));
}

TEST_CASE("a key chord becomes the DOM key a page expects", "[Mcp]")
{
    const json enter = web_key("Return");
    CHECK(enter["key"]["key"] == "Enter");
    CHECK(enter["key"]["keyCode"] == 13);
    CHECK_FALSE(enter["key"].contains("text"));

    CHECK(web_key("F5")["key"]["key"] == "F5");
    CHECK(web_key("F5")["key"]["keyCode"] == 116);
    CHECK(web_key("Left")["key"]["key"] == "ArrowLeft");

    // A letter types its lower case, and its upper case with Shift; the code names the key.
    const json a = web_key("A");
    CHECK(a["key"]["key"] == "a");
    CHECK(a["key"]["code"] == "KeyA");
    CHECK(a["key"]["text"] == "a");
    CHECK(a["mods"]["shift"] == false);
    const json shift_a = web_key("Shift+A");
    CHECK(shift_a["key"]["key"] == "A");
    CHECK(shift_a["mods"]["shift"] == true);
    CHECK(web_key("5")["key"]["code"] == "Digit5");
}

TEST_CASE("Ctrl in a chord is the platform's command key on a page", "[Mcp]")
{
    const json mods = web_key("Ctrl+S")["mods"];
#ifdef __APPLE__
    // Command on macOS, as ui_key's Ctrl is for widgets; RawCtrl is the Control key.
    CHECK(mods["meta"] == true);
    CHECK(mods["ctrl"] == false);
    const json raw = web_key("RawCtrl+S")["mods"];
    CHECK(raw["ctrl"] == true);
    CHECK(raw["meta"] == false);
#else
    CHECK(mods["ctrl"] == true);
    CHECK(mods["meta"] == false);
#endif
}

TEST_CASE("a chord with no page key is refused", "[Mcp]")
{
    CHECK_THROWS_AS(web_key("Ctrl+"), ToolFailure);
    CHECK_THROWS_AS(web_key("NoSuchKey"), ToolFailure);
}

TEST_CASE("a page refuses a request from 100 ms before the tool stops waiting for it", "[Mcp]")
{
    const auto      now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const long long expiry = page_expiry(std::chrono::steady_clock::now() + std::chrono::seconds(2));
    // 2 s ahead less 100 ms, on the page's clock; the slack is the time between the two reads.
    CHECK(expiry >= now_ms + 1900);
    CHECK(expiry <= now_ms + 1900 + 200);
}

TEST_CASE("a request script asks the page's agent, and installs it only when told to", "[Mcp]")
{
    ScopedRepoResourcesDir resources;
    const json             request{{"op", "snapshot"}, {"max", 300}};
    const std::string      version = page_agent_version();
    REQUIRE_FALSE(version.empty());

    // The light script answers {"code":"install"} from a page whose agent is missing or older.
    const std::string light = page_script(request, false);
    CHECK(light.find(json(version).dump()) != std::string::npos);
    CHECK(light.find("{\"code\":\"install\"}") != std::string::npos);
    CHECK(light.find(request.dump()) != std::string::npos);
    CHECK(light.find(page_agent_source().substr(0, 200)) == std::string::npos);

    // The install script carries the agent's source, as code and as text for the relayed frames.
    const std::string install = page_script(request, true);
    CHECK(install.find(page_agent_source().substr(0, 200)) != std::string::npos);
    CHECK(install.find(json(page_agent_source()).dump().substr(0, 200)) != std::string::npos);
    CHECK(install.find(request.dump()) != std::string::npos);
}

TEST_CASE("an auto-answer rule is searched in title and message, or in a lone title", "[Mcp]")
{
    // A web dialog has no message, so a rule anchored to its title matches it; one written for the
    // "title\n" it was searched in before matches as it did.
    CHECK(rule_matches(make_rule("^MCP web fixture$", "Submit"), "MCP web fixture", ""));
    CHECK(rule_matches(make_rule("^MCP web fixture\\n$", "Submit"), "MCP web fixture", ""));
    CHECK_FALSE(rule_matches(make_rule("^MCP web fixture$", "Submit"), "MCP web fixture window", ""));
    // Case aside.
    CHECK(rule_matches(make_rule("^mcp WEB", "Submit"), "MCP web fixture", ""));
    // With a message, the text is "title\nmessage" as it always was: a pattern anchored to the
    // message's start finds nothing there, so which of two rules answers a dialog stays the same.
    CHECK(rule_matches(make_rule("preset\\nThis", "ok"), "Delete preset", "This cannot be undone"));
    CHECK(rule_matches(make_rule("^Warning", "no"), "Warning", "Delete all cached files?"));
    CHECK_FALSE(rule_matches(make_rule("^Delete", "yes"), "Warning", "Delete all cached files?"));
    CHECK_FALSE(rule_matches(make_rule("Export", "ok"), "Delete preset", "This cannot be undone"));
}

TEST_CASE("the journal's index counts every candidate page's matches, and only when all were heard from", "[Mcp]")
{
    auto answered = [](json matches, int unanswered = 0) {
        json value{{"matches", std::move(matches)}};
        if (unanswered > 0)
            value["unanswered"] = unanswered;
        return PageReply{PageReply::State::Answered, value, std::string()};
    };
    const std::vector<WebHandle> webs{7, 9};
    // The second page's element, after the first page's two matches and with a frame's match on its
    // own page ("3/d/1"): 3 among 4.
    const std::vector<PageReply> all{answered(json::array({{{"k", 1}}, {{"k", 2}}})),
                                     answered(json::array({{{"k", "3/d/1"}}, {{"k", 4}}}))};
    REQUIRE(journal_index(webs, all, 9, json(4)).has_value());
    CHECK(*journal_index(webs, all, 9, json(4)) == 3);
    CHECK(*journal_index(webs, all, 9, json("3/d/1")) == 2);
    // The only match needs no index; an element no longer among the matches gets none.
    CHECK_FALSE(journal_index({7}, {answered(json::array({{{"k", 1}}}))}, 7, json(1)).has_value());
    CHECK_FALSE(journal_index(webs, all, 9, json(5)).has_value());
    // A page or frame not heard from may hold more matches: no index then.
    const PageReply silent{PageReply::State::NoAnswer, json(), std::string()};
    const PageReply failed{PageReply::State::Failed, json(), "The page agent could not run in this page"};
    const PageReply erred{PageReply::State::Answered, json{{"error", "styles broken"}, {"code", "error"}}, std::string()};
    CHECK_FALSE(journal_index(webs, {all[0], silent}, 7, json(2)).has_value());
    CHECK_FALSE(journal_index(webs, {all[0], failed}, 7, json(2)).has_value());
    CHECK_FALSE(journal_index(webs, {all[0], erred}, 7, json(2)).has_value());
    CHECK_FALSE(journal_index(webs, {answered(json::array({{{"k", 1}}, {{"k", 2}}}), 1), all[1]}, 7, json(2)).has_value());
    // A page that went away holds nothing.
    const PageReply gone{PageReply::State::Destroyed, json(), std::string()};
    CHECK(*journal_index(webs, {gone, all[1]}, 9, json(4)) == 1);
}

TEST_CASE("a button name stands for its standard id, a number for itself", "[Mcp]")
{
    CHECK(standard_dialog_button("OK") == wxID_OK);
    CHECK(standard_dialog_button("cancel") == wxID_CANCEL);
    CHECK(standard_dialog_button("Close") == wxID_CLOSE);
    CHECK(standard_dialog_button("apply") == wxID_APPLY);
    CHECK(standard_dialog_button("5100") == 5100);
    // A page button's label is no standard name.
    CHECK(standard_dialog_button("Submit") == wxID_NONE);
}

TEST_CASE("a dialog with a web view is described as a web dialog", "[Mcp]")
{
    if (!have_display())
        SKIP("This test needs a display (for example, xvfb-run).");
    wxInitializer init;
    REQUIRE(init.IsOk());
    {
        wxDialog web_dialog(nullptr, wxID_ANY, "Color Picker");
        new StubWebView(&web_dialog);
        wxDialog plain(nullptr, wxID_ANY, "Plain");
        CHECK(describe_dialog(&web_dialog).value("web", false));
        CHECK_FALSE(describe_dialog(&plain).contains("web"));
        // Hidden, the dialog's web view is found only when hidden ones are asked for.
        CHECK(web_view_in(&web_dialog, true) == nullptr);
        CHECK(web_view_in(&web_dialog, false) != nullptr);
    }
}

TEST_CASE("a web dialog's rule names a wx button of it by label, standard name or id", "[Mcp]")
{
    if (!have_display())
        SKIP("This test needs a display (for example, xvfb-run).");
    wxInitializer init;
    REQUIRE(init.IsOk());
    {
        // A web dialog with wx buttons of its own, as the colour picker's fallback Cancel is.
        wxDialog dialog(nullptr, wxID_ANY, "Color Picker");
        new StubWebView(&dialog);
        new wxButton(&dialog, wxID_OK, "Apply colour");
        new wxButton(&dialog, 5100, "Go");
        // Its wx buttons answer the rule, so the rule never waits on the page.
        CHECK(native_dialog_button(&dialog, "ok") == wxID_OK);
        CHECK(native_dialog_button(&dialog, "Apply colour") == wxID_OK);
        CHECK(native_dialog_button(&dialog, "5100") == 5100);
        // A page's button, or a standard name with no wx button, is answered on the page.
        CHECK(native_dialog_button(&dialog, "Submit") == wxID_NONE);
        CHECK(native_dialog_button(&dialog, "cancel") == wxID_NONE);
    }
}

TEST_CASE("a native message box lists the buttons it can be answered with", "[Mcp]")
{
    if (!have_display())
        SKIP("This test needs a display (for example, xvfb-run).");
    wxInitializer init;
    REQUIRE(init.IsOk());
    {
        wxMessageDialog box(nullptr, "Plugin \"Fixture\" is requesting permission to create a thread.", "Plugin permission request",
                            wxYES_NO | wxICON_WARNING);
        const json described = describe_dialog(&box);
        CHECK(described.value("native", false));
#if defined(_WIN32) || defined(__WXGTK__)
        CHECK(described["buttons"] == json::array({{{"id", wxID_YES}, {"label", "Yes"}}, {{"id", wxID_NO}, {"label", "No"}}}));
        CHECK(dialog_button_by_label(&box, "no") == wxID_NO);
        CHECK(resolve_dialog_button(&box, "yes") == wxID_YES);
        CHECK(has_dialog_button(&box, wxID_YES));
        // A Yes/No box has no Cancel button to end it with.
        CHECK_FALSE(has_dialog_button(&box, wxID_CANCEL));
        wxMessageDialog ok_cancel(nullptr, "Continue?", "Question", wxOK | wxCANCEL);
        CHECK(message_box_buttons(&ok_cancel) == json::array({{{"id", wxID_OK}, {"label", "OK"}}, {{"id", wxID_CANCEL}, {"label", "Cancel"}}}));
#else
        // macOS: no call reaches an open alert, so none is offered.
        CHECK(described["buttons"].empty());
#endif
        wxDialog plain(nullptr, wxID_ANY, "Plain");
        CHECK(message_box_buttons(&plain).empty());
    }
}

TEST_CASE("a web view's handle stays its own and finds nothing once the web view is gone", "[Mcp]")
{
    if (!have_display())
        SKIP("This test needs a display (for example, xvfb-run).");
    wxInitializer init;
    REQUIRE(init.IsOk());
    {
        CHECK(web_handle(nullptr) == 0);
        wxDialog       dialog(nullptr, wxID_ANY, "Pages");
        auto*          first  = new StubWebView(&dialog);
        auto*          second = new StubWebView(&dialog);
        const WebHandle a     = web_handle(first);
        CHECK(a != 0);
        CHECK(web_handle(first) == a);
        CHECK(web_handle(second) != a);
        CHECK(web_view_of(a) == first);
        delete first;
        CHECK(web_view_of(a) == nullptr);
        // A web view made later never gets the old handle.
        auto* third = new StubWebView(&dialog);
        CHECK(web_handle(third) != a);
    }
}

TEST_CASE("an element's agent is its page's own or its configuration frame's, in its page document", "[Mcp]")
{
    const PressAgent page = press_agent(7, "d1", 12);
    CHECK(page.frame.empty());
    CHECK(page == PressAgent{7, "d1", ""});
    const PressAgent frame = press_agent(7, "d1", "3/f9/12");
    CHECK(frame.frame == "3/f9");
    CHECK_FALSE(frame == page);
    CHECK(press_agent(7, "d1", "3/f9/40") == frame);
    CHECK_FALSE(press_agent(7, "d2", 12) == page);
}

TEST_CASE("a press is released only once its down came back, and a confirmed release never takes a later press", "[Mcp]")
{
    WebPresses       presses;
    const PressAgent a{1, "d", ""}, b{2, "d", ""};
    const auto       first = presses.hold(a, "");
    // Its down is not sent yet: a release or an up now could overtake it.
    CHECK(presses.releasable(std::nullopt).empty());
    CHECK(presses.of(a).empty());
    presses.sent(first);
    const auto asked = presses.releasable(std::nullopt);
    REQUIRE(asked.size() == 1);
    CHECK(asked[0].id == first);
    // Meanwhile another down on the same agent, then the release for the first is confirmed.
    const auto second = presses.hold(a, "");
    presses.sent(second);
    presses.drop(first);
    CHECK(presses.of(a) == std::vector<std::uint64_t>{second});
    // A move or an up on an agent keeps that agent's press, and only its.
    const auto other = presses.hold(b, "");
    presses.sent(other);
    const auto kept = presses.releasable(a);
    REQUIRE(kept.size() == 1);
    CHECK(kept[0].id == other);
}

TEST_CASE("a script's presses are told from a client's for the cleanup when either ends", "[Mcp]")
{
    WebPresses       presses;
    const PressAgent a{1, "d", ""}, b{2, "d", ""};
    const auto       client = presses.hold(a, "");
    const auto       script = presses.hold(b, "job-7");
    presses.sent(client);
    presses.sent(script);
    const std::string job = "job-7", none;
    const auto        by_script = presses.owned(&job);
    REQUIRE(by_script.size() == 1);
    CHECK(by_script[0].id == script);
    const auto by_client = presses.owned(&none);
    REQUIRE(by_client.size() == 1);
    CHECK(by_client[0].id == client);
    CHECK(presses.owned(nullptr).size() == 2);
}
