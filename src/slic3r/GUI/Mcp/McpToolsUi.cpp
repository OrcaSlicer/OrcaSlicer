// GUI automation: look at the widget tree, click, type, press keys, drag on the 3D canvas, answer
// dialogs and run JavaScript in web views.

#include "JobRegistry.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "McpCore.hpp"
#include "McpPaths.hpp"
#include "McpService.hpp"
#include "McpTools.hpp"
#include "McpTypes.hpp"
#include "ModalTracker.hpp"
#include "ScreenCapture.hpp"
#include "UiInspector.hpp"
#include "WebPages.hpp"
#include "WindowControl.hpp"
#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoSVG.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoTextureDisplacement.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosManager.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Plater.hpp"

#include "libslic3r/Point.hpp"

#include <wx/app.h>
#include <wx/defs.h>
#include <wx/dialog.h>
#include <wx/display.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/glcanvas.h>
#include <wx/popupwin.h>
#include <wx/toplevel.h>
#include <wx/vector.h>
#include <wx/webview.h>
#include <wx/weakref.h>
#include <wx/window.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;
using namespace std::chrono_literals;

namespace {

// The locator grammar is spelled out once, in list_widgets's description, which hands out the refs.
const json kTargetSchema = {{"type", json::array({"string", "object"})},
                            {"description", "A ref from the latest list_widgets call (\"w12\") or a locator, such as {label: \"OK\", "
                                            "within: \"Preferences\"} or {menu: \"File>Export>Export G-code\"}; list_widgets's "
                                            "description lists every kind."}};

const json kPageTimeoutSchema = {{"type", "integer"},
                                 {"minimum", 1},
                                 {"maximum", 120},
                                 {"description", "For a web page's element: seconds the page gets to answer (default 10)."}};

const json kModifiersSchema = {{"type", "array"},
                               {"items", {{"type", "string"}, {"enum", {"ctrl", "cmd", "rawctrl", "shift", "alt"}}}},
                               {"description", "Modifier keys held during the click, as in press_keys: ctrl and cmd are both Command on macOS "
                                               "and Ctrl elsewhere; rawctrl is the macOS Control key."}};

bool in_top_dialog(McpService& service, wxWindow* window)
{
    wxDialog* top = service.modals().top();
    return top != nullptr && window != nullptr && owning_window(window) == top;
}

// GUI input tools run under the recover guard so they can work in the topmost modal dialog; a
// target anywhere else gets the checks a mutate call would: no other call's nested loop, no open
// modal, and operation admission (no MCP operation holding the project, no busy UI job worker).
void guard_target(McpService& service, wxWindow* window)
{
    if (in_top_dialog(service, window))
        return;
    service.ui().require_mutate();
    service.core().admit({});
}

// The window a target is in: a widget's own, a row's list.
wxWindow* target_window(const UiTarget& t)
{
    return t.kind == UiTarget::Kind::Window || t.kind == UiTarget::Kind::Row ? t.window.get() : nullptr;
}

// The journal replays locators, never refs: refs mean nothing in another session.
void journal_with_locator(ToolContext& ctx, const json& args, const json& locator)
{
    json recorded = args;
    if (recorded.contains("target") && recorded["target"].is_string() && !locator.is_null())
        recorded["target"] = locator;
    ctx.set_journal_args(std::move(recorded));
}

NativeCommand recover_input_tool(std::string group, std::string name, std::string title, std::string description, json schema,
                                 CommandFn handler)
{
    NativeCommand d = mutate_tool(std::move(group), std::move(name), std::move(title), std::move(description), std::move(schema),
                                  std::move(handler));
    d.guard   = GuardClass::Recover;
    return d;
}

const json kPopupItemSchema = prop("string", "Run this item of the menu the input opens (a right click's context menu, a button's menu), "
                                             "by its label path, \"Add Modifier>Cylinder\"; \"#2\" picks a repeated label's second item. "
                                             "Without it, such a menu is listed in the reply (popup_menu), not shown.");

// The menu an input call may open, which the inspector takes instead of showing it
// (UiInspector::arm_menu()): armed in the closure that delivers the input, read once the events the
// input queued have run. A call that fails before reading it drops it.
struct InputMenu
{
    McpService&  service;
    ToolContext& ctx;
    std::string  item;
    int          token = 0;
    // Also reply the dialogs open once the input's events ran: an input whose work opens a dialog
    // from a posted event (a plate icon) returns before the dialog shows.
    bool         dialogs = false;
    // Set by a call queued on the 3D view behind the click, which the view posts to itself and opens
    // its menu from: once it has run, so has the click (or the click's command is in a dialog's loop,
    // which runs it too).
    std::shared_ptr<bool> marked;

    InputMenu(McpService& service, ToolContext& ctx, std::string item) : service(service), ctx(ctx), item(std::move(item)) {}
    InputMenu(const InputMenu&)            = delete;
    InputMenu& operator=(const InputMenu&) = delete;
    ~InputMenu()
    {
        if (token == 0)
            return;
        try {
            ctx.observe_ui([this] {
                service.inspector().drop_menu(token);
                return json();
            });
        } catch (const ToolFailure&) {
        }
    }

    // UI thread, in the input's closure, before the input.
    void arm() { token = service.inspector().arm_menu(item); }
    // UI thread, after an input to a 3D view, on the handler the input posted its work to.
    void mark(wxEvtHandler* queue)
    {
        if (queue == nullptr)
            return;
        marked = std::make_shared<bool>(false);
        queue->CallAfter([m = marked] { *m = true; });
    }
    // Job thread: settles, then {popup_menu} or {ran} (with the dialogs a command it ran left open),
    // or {} when no menu opened.
    json read()
    {
        for (int i = 0; marked && i < 100 && !ctx.observe_ui([this] { return json(*marked); }).get<bool>(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        settle(ctx);
        const int t = std::exchange(token, 0);
        return ctx.observe_ui([&] {
            json out = service.inspector().disarm_menu(t);
            if ((dialogs || out.contains("ran")) && service.modals().any_open())
                out["dialogs"] = service.modals().snapshot();
            return out;
        });
    }
};

// Every shown web view, main window first.
std::vector<wxWebView*> visible_webviews()
{
    std::vector<wxWebView*> out;
    std::function<void(wxWindow*)> visit = [&](wxWindow* w) {
        if (!w->IsShownOnScreen())
            return;
        if (auto* web = dynamic_cast<wxWebView*>(w)) {
            out.push_back(web);
            return;
        }
        // A dialog is its parent's child too; it is visited once, as a top-level window.
        for (wxWindow* child : w->GetChildren())
            if (dynamic_cast<wxTopLevelWindow*>(child) == nullptr && !skipped_by_walks(child))
                visit(child);
    };
    if (wxGetApp().mainframe != nullptr)
        visit(wxGetApp().mainframe);
    for (wxWindow* tlw : wxTopLevelWindows)
        if (tlw != wxGetApp().mainframe && tlw->GetName() != kHiddenFromAutomation)
            visit(tlw);
    return out;
}

// The web view a script without a target runs in: the one in the topmost modal dialog while one is
// open, else the only one shown.
wxWebView* default_webview(McpService& service)
{
    std::vector<wxWebView*> webs = visible_webviews();
    std::string             where;
    if (wxDialog* top = service.modals().top()) {
        std::vector<wxWebView*> in_top;
        for (wxWebView* w : webs)
            if (owning_window(w) == top)
                in_top.push_back(w);
        if (!in_top.empty()) {
            webs  = std::move(in_top);
            where = " in \"" + into_u8(top->GetTitle()) + "\"";
        }
    }
    if (webs.empty())
        throw ToolFailure("No web view is shown (the Home tab has one: set_view tab=home)");
    if (webs.size() > 1) {
        json urls = json::array();
        for (wxWebView* w : webs)
            urls.push_back(redact_url(into_u8(w->GetCurrentURL())));
        throw ToolFailure("Several web views are shown" + where + "; pass target (list_widgets query=webview)", {{"web_views", urls}});
    }
    return webs.front();
}

// A web view in a dialog under the topmost modal one is refused, one in the topmost takes input as
// that dialog's widgets do (and the call works in that dialog), and any other gets the checks a
// mutate call would.
void guard_page(McpService& service, ToolContext& ctx, wxWebView* web)
{
    const std::vector<wxDialog*>& stack = service.modals().stack();
    const auto                    in    = std::find(stack.begin(), stack.end(), owning_window(web));
    if (in != stack.end() && *in != stack.back())
        throw ToolFailure("\"" + into_u8((*in)->GetTitle()) + "\" is under \"" + into_u8(stack.back()->GetTitle()) +
                          "\"; answer the topmost dialog first");
    if (in != stack.end())
        ctx.works_in_dialog(service.modals().serial(stack.back()));
    guard_target(service, web);
}

// ---- web pages ----------------------------------------------------------------------------------
// The input tools reach a page element through its page's agent (WebPages.hpp): resolve it, guard
// its web view as any widget's window, ask the page to act, and observe what followed.

// A "d" ref from list_widgets, also written {ref: "d12"}, or a {css}/{web} locator.
bool is_web_target(const json& target)
{
    const json& ref = target.is_object() && target.size() == 1 && target.contains("ref") ? target["ref"] : target;
    if (ref.is_string()) {
        const std::string r = ref.get<std::string>();
        return r.size() > 1 && r[0] == 'd' && std::all_of(r.begin() + 1, r.end(), [](unsigned char c) { return std::isdigit(c); });
    }
    return is_web_locator(target);
}

// A widget target that is a web view itself (its `w` ref, or a {class: "webview"} locator), whose page
// wx events never reach: its handle, `within` and locator, or 0 for any other target.
WebHandle web_view_target(McpService& service, ToolContext& ctx, const json& target, std::string& within, json* locator = nullptr)
{
    WebHandle out = 0;
    ctx.run_ui([&]() -> json {
        const UiTarget t = service.inspector().resolve(target);
        if (auto* web = dynamic_cast<wxWebView*>(target_window(t))) {
            out    = web_handle(web);
            within = within_of(web);
            if (locator != nullptr)
                *locator = service.inspector().locator_for(t);
        }
        return json();
    });
    return out;
}

// click and set_widget_value on a web view: the page's elements are the targets.
void refuse_web_view(McpService& service, ToolContext& ctx, const json& target)
{
    std::string within;
    if (web_view_target(service, ctx, target, within) != 0)
        throw ToolFailure("That is a web view, whose page wx events don't reach; target one of its page elements: a d ref from "
                          "list_widgets, or a {web} / {css} locator");
}

// A page element an input tool acts on.
struct WebTarget
{
    WebHandle   web = 0;
    json        key; // null: the page's focused element (keys without a target)
    std::string doc, kind, label, within;
};

std::chrono::seconds web_timeout(const json& args) { return std::chrono::seconds(std::clamp(arg_int(args, "timeout_s", 10), 1, 120)); }

// Sends `request` to the pages of `webs` at once and waits for their replies until `deadline`; the
// calls start on the UI thread under the tool's guard when `guarded`, else as an observation. A page
// whose plugin configuration frames have yet to answer replies pending, and its request is repeated,
// the same round each time, for up to a second; the last repeat says it is the last, and a frame still
// silent then is listed as not answering. A web view gone before its call is Destroyed.
std::vector<PageReply> ask_pages(ToolContext& ctx, const std::vector<WebHandle>& webs, json request,
                                 std::chrono::steady_clock::time_point deadline, bool guarded = false)
{
    static std::atomic<int> rounds{0};
    request["round"]   = ++rounds;
    request["expires"] = page_expiry(deadline);
    std::vector<PageReply>   out(webs.size());
    std::vector<std::size_t> todo(webs.size());
    for (std::size_t i = 0; i < todo.size(); ++i)
        todo[i] = i;
    const auto relay_end = std::min(deadline, std::chrono::steady_clock::now() + 1s);
    for (bool last = false; !todo.empty();) {
        if (last)
            request["last"] = true;
        PageCalls                calls;
        std::vector<std::size_t> slot_of(webs.size(), SIZE_MAX);
        auto start = [&]() -> json {
            for (std::size_t i : todo) {
                if (wxWebView* web = web_view_of(webs[i]))
                    slot_of[i] = calls.start(web, request);
                else
                    out[i].state = PageReply::State::Destroyed;
            }
            return json();
        };
        guarded ? ctx.run_ui(start) : ctx.observe_ui(start);
        calls.wait(ctx, deadline);
        std::vector<std::size_t> again;
        for (std::size_t i : todo) {
            if (slot_of[i] == SIZE_MAX)
                continue;
            out[i] = calls.reply(slot_of[i]);
            if (!last && out[i].state == PageReply::State::Answered && out[i].value.value("pending", false))
                again.push_back(i);
        }
        todo = std::move(again);
        if (todo.empty() || ctx.cancelled() || std::chrono::steady_clock::now() >= deadline)
            break;
        std::this_thread::sleep_for(50ms);
        last = std::chrono::steady_clock::now() >= relay_end;
    }
    return out;
}

PageReply ask_page(ToolContext& ctx, WebHandle web, json request, std::chrono::steady_clock::time_point deadline)
{
    return ask_pages(ctx, {web}, std::move(request), deadline).front();
}

// Sends each page of `webs` its own request of `requests` at once and waits for the replies until
// `deadline`, as an observation. A web view gone before its call is Destroyed.
std::vector<PageReply> ask_each(ToolContext& ctx, const std::vector<WebHandle>& webs, std::vector<json> requests,
                                std::chrono::steady_clock::time_point deadline)
{
    std::vector<PageReply>   out(webs.size());
    std::vector<std::size_t> slot(webs.size(), SIZE_MAX);
    PageCalls                calls;
    ctx.observe_ui([&]() -> json {
        for (std::size_t i = 0; i < webs.size(); ++i) {
            requests[i]["expires"] = page_expiry(deadline);
            if (wxWebView* web = web_view_of(webs[i]))
                slot[i] = calls.start(web, requests[i]);
            else
                out[i].state = PageReply::State::Destroyed;
        }
        return json();
    });
    calls.wait(ctx, deadline);
    for (std::size_t i = 0; i < webs.size(); ++i)
        if (slot[i] != SIZE_MAX)
            out[i] = calls.reply(slot[i]);
    return out;
}

// The replies of plugin configuration frames to requests their pages forwarded (`rids`, each page's
// id for its request), polled from the pages together until `deadline`: Answered with the frame's
// reply, the poll that failed, or NoAnswer for a frame that never answered.
std::vector<PageReply> follow_relays(ToolContext& ctx, const std::vector<WebHandle>& webs, const std::vector<json>& rids,
                                     std::chrono::steady_clock::time_point deadline)
{
    std::vector<PageReply>   out(webs.size());
    std::vector<std::size_t> waiting(webs.size());
    for (std::size_t i = 0; i < waiting.size(); ++i)
        waiting[i] = i;
    for (;;) {
        std::vector<WebHandle> polled;
        std::vector<json>      polls;
        for (std::size_t i : waiting) {
            polled.push_back(webs[i]);
            polls.push_back({{"op", "relay"}, {"rid", rids[i]}});
        }
        const std::vector<PageReply> p = ask_each(ctx, polled, std::move(polls), deadline);
        std::vector<std::size_t>     again;
        for (std::size_t j = 0; j < waiting.size(); ++j) {
            const std::size_t i = waiting[j];
            if (p[j].state != PageReply::State::Answered || p[j].value.contains("error"))
                out[i] = p[j];
            else if (p[j].value.value("done", false))
                out[i] = PageReply{PageReply::State::Answered, p[j].value["reply"], std::string()};
            else
                again.push_back(i);
        }
        waiting = std::move(again);
        if (waiting.empty() || std::chrono::steady_clock::now() >= deadline || ctx.cancelled())
            return out;
        std::this_thread::sleep_for(30ms);
    }
}

// Throws the failure a page call's outcome stands for. A request that changes the page (`acting`)
// and got no answer has an unknown outcome: it may have run, or run once the page is free.
void check_page_reply(const PageReply& r, std::chrono::seconds timeout, bool acting, ToolContext& ctx)
{
    switch (r.state) {
    case PageReply::State::Answered: break;
    case PageReply::State::Destroyed: throw ToolFailure("The web view closed before its page answered");
    case PageReply::State::Failed: throw ToolFailure("The page could not run the request: " + r.error);
    case PageReply::State::NoAnswer:
        if (ctx.cancelled())
            throw ToolFailure("Cancelled: OrcaSlicer is closing or the replay or script was stopped");
        throw ToolFailure("The page did not answer within " + std::to_string(timeout.count()) + " s" +
                          (acting ? ", so the outcome is unknown: the action may have run, or may still run once the page is free. Call "
                                    "list_widgets before trying again (timeout_s waits longer)" :
                                    "; it may be busy"));
    }
    if (r.value.contains("error")) {
        if (r.value.value("code", std::string()) == "expired")
            throw ToolFailure("The page was busy past the request's deadline and refused it, so nothing was done; try again (timeout_s "
                              "waits longer)");
        throw ToolFailure(r.value["error"].get<std::string>());
    }
}

// The pages a locator looks in: the shown ones within `within`, in snapshot order. Without `within`
// while the topmost modal dialog shows a page, only that dialog's pages, the ones list_widgets reads
// and the only ones input can reach, so a page behind it never makes a label ambiguous.
std::vector<wxWebView*> locator_pages(McpService& service, const std::string& within)
{
    std::vector<wxWebView*> webs = service.inspector().web_views(within);
    if (wxDialog* top = within.empty() ? service.modals().top() : nullptr) {
        std::vector<wxWebView*> in_top;
        for (wxWebView* w : webs)
            if (owning_window(w) == top)
                in_top.push_back(w);
        if (!in_top.empty())
            webs = std::move(in_top);
    }
    return webs;
}

// Finds a page element by ref or locator; `index` picks among the matches of all locator_pages().
WebTarget resolve_web(McpService& service, ToolContext& ctx, const json& target, std::chrono::seconds timeout)
{
    WebTarget out;
    if (!is_web_locator(target)) {
        ctx.run_ui([&]() -> json {
            const UiTarget t = service.inspector().resolve(target);
            if (t.kind != UiTarget::Kind::WebElement)
                throw ToolFailure("The ref is not a page element");
            auto* web = dynamic_cast<wxWebView*>(t.window.get());
            if (web == nullptr)
                throw ToolFailure("The page's web view is gone; call list_widgets again");
            out.web    = web_handle(web);
            out.key    = t.web_key;
            out.doc    = t.web_doc;
            out.kind   = t.web_kind;
            out.label  = t.web_label;
            out.within = t.web_within;
            return json();
        });
        if (out.key.is_null())
            throw ToolFailure("That line of the page is text with no element to act on; act on the element around it");
        return out;
    }
    const std::string within  = target.value("within", "");
    json              locator = json::object();
    for (const char* k : {"css", "web", "kind"})
        if (target.contains(k))
            locator[k] = target[k];
    struct Match
    {
        std::size_t page;
        json        match;
        std::string doc;
    };
    std::vector<WebHandle>   webs;
    std::vector<std::string> withins;
    std::vector<Match>       matches;
    json                     unread;
    // What a page or a configuration frame holds is unknown when it did not answer in time or its
    // agent failed the search; a page whose script could not run at all holds nothing reachable.
    std::size_t unknown_pages = 0, unknown_frames = 0, failed = 0;
    // Up to 3 s for a match to show, as pages fill in after they load and a dialog's page loads after
    // the click that opened it has returned.
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const auto show_end = std::chrono::steady_clock::now() + std::min(timeout, std::chrono::seconds(3));
    for (;;) {
        webs.clear();
        withins.clear();
        ctx.run_ui([&]() -> json {
            const std::vector<wxWebView*> pages = locator_pages(service, within);
            for (wxWebView* w : pages) {
                webs.push_back(web_handle(w));
                withins.push_back(within_of(w));
            }
            if (wxDialog* top = service.modals().top(); top != nullptr && !pages.empty() &&
                                                         std::all_of(pages.begin(), pages.end(), [top](wxWebView* w) { return owning_window(w) == top; }))
                ctx.works_in_dialog(service.modals().serial(top));
            return json();
        });
        matches.clear();
        unread        = json::array();
        unknown_pages = unknown_frames = failed = 0;
        const std::vector<PageReply> replies = webs.empty() ? std::vector<PageReply>() :
                                                              ask_pages(ctx, webs, {{"op", "find"}, {"locator", locator}}, deadline);
        for (std::size_t i = 0; i < replies.size(); ++i) {
            const PageReply& r = replies[i];
            if (r.state == PageReply::State::Answered && !r.value.contains("error")) {
                for (const json& m : r.value.value("matches", json::array()))
                    matches.push_back({i, m, r.value.value("doc", std::string())});
                if (const int frames = r.value.value("unanswered", 0); frames > 0) {
                    unknown_frames += std::size_t(frames);
                    unread.push_back(std::to_string(frames) + (frames == 1 ? " configuration frame" : " configuration frames") +
                                     " of a page did not answer or failed the search");
                }
            } else if (r.state == PageReply::State::Answered) {
                // The agent's error, a deadline the page was busy past included.
                ++unknown_pages;
                unread.push_back(r.value.value("error", std::string()));
            } else if (r.state == PageReply::State::NoAnswer) {
                ++unknown_pages;
                unread.push_back("did not answer");
            } else if (r.state == PageReply::State::Failed) {
                ++failed;
                unread.push_back(r.error);
            }
        }
        // A page or frame not heard from has had its wait already (the call's deadline, the relay's rounds).
        if (!matches.empty() || ctx.cancelled() || std::chrono::steady_clock::now() + 150ms >= show_end)
            break;
        std::this_thread::sleep_for(150ms);
    }
    if (webs.empty())
        throw ToolFailure(within.empty() ? "No web page is shown" : "No web page is shown in \"" + within + "\"");
    auto count = [](std::size_t n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); };
    std::string unknown;
    if (unknown_pages > 0)
        unknown = count(unknown_pages, "page", "pages");
    if (unknown_frames > 0)
        unknown += (unknown.empty() ? "" : " and ") + count(unknown_frames, "configuration frame", "configuration frames");
    // A page or frame not heard from may hold more matches, so neither uniqueness nor an index is
    // known. `within` can leave out other windows' pages, not a frame of the matching page.
    if (!matches.empty() && !unknown.empty())
        throw ToolFailure("A page element matches " + target.dump() + ", but " + unknown +
                              " did not answer or failed the search, so it may not be the only match; nothing was done. Try again "
                              "once they answer, " + (unknown_pages > 0 ? "add within (the window's title), " : "") + "or use a ref",
                          {{"unread_pages", unread}});
    const int index = target.value("index", -1);
    if (matches.empty() && !unread.empty())
        throw ToolFailure("No page element matches " + target.dump() + " in what answered" +
                              (unknown.empty() ? "" : "; " + unknown + " did not answer or failed the search") +
                              (failed == 0 ? "" : "; " + count(failed, "page", "pages") + " could not run the page agent") +
                              "; nothing was done. " + (unknown.empty() ? "Call list_widgets" : "Try again once they answer"),
                          {{"unread_pages", unread}});
    if (matches.empty())
        throw ToolFailure("No page element matches " + target.dump() + "; call list_widgets");
    if (matches.size() > 1 && index < 0) {
        json listed = json::array();
        for (const Match& m : matches)
            if (listed.size() < 30)
                listed.push_back({{"kind", m.match.value("kind", "")}, {"label", m.match.value("label", "")}});
        throw ToolFailure(std::to_string(matches.size()) + " page elements match " + target.dump() + "; add \"index\" (0-based) or use a ref",
                          {{"matches", listed}});
    }
    if (index >= int(matches.size()))
        throw ToolFailure("index " + std::to_string(index) + " is past the " + std::to_string(matches.size()) + " matches");
    const Match& hit = matches[index < 0 ? 0 : std::size_t(index)];
    out.web          = webs[hit.page];
    out.key          = hit.match["k"];
    out.doc          = hit.doc;
    out.kind         = hit.match.value("kind", "");
    out.label        = hit.match.value("label", "");
    out.within       = withins[hit.page];
    return out;
}

// The locator the journal records for the element an action reached: the page's own, with `within`,
// and `index` among the matches of every page a locator looks in.
json web_locator(McpService& service, ToolContext& ctx, const WebTarget& t, const json& reply, std::chrono::seconds timeout)
{
    if (!reply.contains("loc") || !reply["loc"].is_object())
        return json();
    json loc = reply["loc"];
    if (!t.within.empty())
        loc["within"] = t.within;
    std::vector<WebHandle> webs;
    ctx.observe_ui([&]() -> json {
        for (wxWebView* w : locator_pages(service, t.within))
            webs.push_back(web_handle(w));
        return json();
    });
    // Asked even of one page: a replay searches the page with its configuration frames, whose matches a
    // frame element's own index leaves out.
    const std::vector<PageReply> replies = ask_pages(ctx, webs, {{"op", "find"}, {"locator", reply["loc"]}},
                                                     std::chrono::steady_clock::now() + timeout);
    // Without a known index a replay that finds several asks for one rather than taking another.
    if (const std::optional<int> index = journal_index(webs, replies, t.web, t.key))
        loc["index"] = *index;
    return loc;
}

// Why answer_dialog does not press a button refused_press() refuses.
std::string refused_button(const std::string& button, DialogPress refused)
{
    if (refused == DialogPress::Hidden)
        return "The dialog's \"" + button + "\" button is on a page it does not show; its message shows what it shows now";
    return "The dialog's \"" + button + "\" button is disabled; its message shows what the dialog still needs";
}

// `closed: true`, with the dialogs still open under the one that closed (the filament picker under the
// colour picker it opened), whose answer is still to give. Any thread.
json closed_reply(McpService& service)
{
    json out{{"closed", true}};
    if (json open = service.modals().snapshot(); !open.empty())
        out["dialogs"] = std::move(open);
    return out;
}

// What the page and the dialogs were before an action, to tell what it did.
struct PageBefore
{
    std::uint64_t modal = 0; // the serial of the dialog the page is in, while it is the topmost
    std::size_t   stack = 0;
    std::string   url;
    std::string   watch; // the id the page outlined itself under before the action (watch_id()), or none
};

// A new id for an action, which its page outlines itself under first so what the action changed can be
// told. Unique for the process, so no page mistakes one action's outline for another's.
std::string watch_id()
{
    static std::atomic<std::uint64_t> next{0};
    return std::to_string(++next);
}

// What followed an action whose reply is `reply`: once the page has posted the messages held while it
// ran, and wx has run their events, `closed` (the web view is gone or its dialog left the modal stack),
// `dialogs` (another modal opened) or `navigated` with `url`, looked for during 300 ms. Navigation is
// told by the web view's URL, the page's own URL before and after the action (a fragment change,
// which WebView2 reports no URL for on a page set as HTML), and, once the 300 ms have passed, a new
// document behind the page (a reload, or a form posted to the same URL). Not IsBusy(), which WebView2
// also reports while a page only works. Failing those, for an action the page outlined itself before
// (before.watch), `page`: whether what the page lists changed by then (changed), the lines it lists
// that it did not (added) and those it no longer does (removed), as list_widgets writes them, partial
// when a configuration frame could not be compared (then changed only when a line was found), and how
// many messages the page itself sent OrcaSlicer during the action (posted). A change after that is the
// next snapshot's to show.
json observe_page(McpService& service, ToolContext& ctx, const WebTarget& t, const PageBefore& before, const json& reply)
{
    const std::string href       = reply.value("href", std::string());
    const bool        moved_hash = reply.contains("href0") && reply.value("href0", std::string()) != href;
    auto              navigated  = [&](const std::string& url) { return json{{"navigated", true}, {"url", redact_url(url.empty() ? href : url)}}; };
    if (reply.value("held", 0) > 0 && reply.contains("flush")) {
        const auto end = std::chrono::steady_clock::now() + 1s;
        while (std::chrono::steady_clock::now() < end) {
            const PageReply f = ask_page(ctx, t.web, {{"op", "flushed"}, {"id", reply["flush"]}}, end);
            if (f.state != PageReply::State::Answered || f.value.value("done", false))
                break;
            std::this_thread::sleep_for(20ms);
        }
    }
    settle(ctx);
    for (int i = 0; i < 10; ++i) {
        json change;
        try {
            change = ctx.observe_ui([&]() -> json {
                wxWebView*  web   = web_view_of(t.web);
                const auto& stack = service.modals().stack();
                if (web == nullptr || (before.modal != 0 && !service.modals().is_open(before.modal)))
                    return closed_reply(service);
                if (stack.size() > before.stack)
                    return {{"dialogs", service.modals().snapshot()}};
                // The page's own URL is the new one when it moved; the web view's may lag behind it.
                if (moved_hash)
                    return navigated(href);
                if (const std::string url = into_u8(web->GetCurrentURL()); url != before.url)
                    return navigated(url);
                return json();
            });
        } catch (const ToolFailure&) {
            // A native dialog the action opened can hold the UI thread's queue (macOS).
            return service.modals().any_open() ? json{{"dialogs", service.modals().snapshot()}} : json::object();
        }
        if (!change.is_null())
            return change;
        std::this_thread::sleep_for(30ms);
    }
    // The page's document token, from a page that still answers: another one is a new document.
    const std::string doc = reply.value("doc", std::string());
    if (!doc.empty()) {
        const PageReply now = ask_page(ctx, t.web, {{"op", "doc"}}, std::chrono::steady_clock::now() + 1s);
        if (now.state == PageReply::State::Destroyed)
            return closed_reply(service);
        if (now.state == PageReply::State::Answered && now.value.contains("doc") && now.value.value("doc", std::string()) != doc)
            return navigated(now.value.value("href", std::string()));
    }
    if (before.watch.empty())
        return json::object();
    // Three seconds, as a snapshot's ask: ask_pages() repeats a round for a configuration frame yet to
    // answer for up to one, and the last round, which compares without that frame, gets the rest, so
    // it is not past its deadline when a busy page answers the rounds before it slowly.
    const PageReply c = ask_page(ctx, t.web, {{"op", "changes"}, {"watch", before.watch}}, std::chrono::steady_clock::now() + 3s);
    if (c.state == PageReply::State::Destroyed)
        return closed_reply(service);
    // A page that lost the outline (one past its minute) or failed to compare says nothing.
    if (c.state != PageReply::State::Answered || !c.value.contains("added") || c.value.contains("error"))
        return json::object();
    const json added   = c.value.value("added", json::array());
    const json removed = c.value.value("removed", json::array());
    const bool partial = c.value.value("partial", false);
    // A frame not compared may hold a change, so only lines found say the page changed then.
    json page = json::object();
    if (!added.empty() || !removed.empty() || !partial)
        page["changed"] = !added.empty() || !removed.empty();
    if (partial)
        page["partial"] = true;
    if (!added.empty())
        page["added"] = added;
    if (!removed.empty())
        page["removed"] = removed;
    if (const int more = c.value.value("more", 0); more > 0)
        page["more"] = more;
    if (const int posted = reply.value("held", 0); posted > 0)
        page["posted"] = posted;
    return {{"page", std::move(page)}};
}

struct WebActed
{
    json reply = json::object(); // the agent's: kind, label, state, ...; empty when the page closed first
    json after;   // what followed (observe_page())
    json locator; // for the journal
};

// A mouse_gesture down's press (WebPresses), which web_act marks sent with its request.
struct DownPress
{
    std::uint64_t id     = 0;
    bool          client = false; // a client's, not a run's
};

// Runs `action` on a page element, waiting up to `timeout` for the page.
WebActed web_act(McpService& service, ToolContext& ctx, const WebTarget& t, const std::string& action, json args, std::chrono::seconds timeout,
                 const DownPress& down = {})
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    PageBefore before;
    before.watch = watch_id();
    json request{{"op", "act"}, {"doc", t.doc}, {"k", t.key}, {"action", action}, {"watch", before.watch}, {"expires", page_expiry(deadline)}};
    PageCalls  calls;
    ctx.run_ui([&]() -> json {
        // A client's down sent after the endpoint closed would leave a press no one releases.
        if (down.client && !service.endpoint_open())
            throw ToolFailure("The MCP server was turned off before this gesture ran; it was not performed");
        wxWebView* web = web_view_of(t.web);
        if (web == nullptr || !web->IsShownOnScreen())
            throw ToolFailure("The page's web view is gone or hidden; call list_widgets again");
        guard_page(service, ctx, web);
        if (in_top_dialog(service, web))
            before.modal = service.modals().serial(service.modals().top());
        before.stack     = service.modals().stack().size();
        before.url       = into_u8(web->GetCurrentURL());
        args["scale"]    = web->GetContentScaleFactor();
        request["args"]  = args;
        calls.start(web, request);
        // From here on a release sent to the page runs after the down.
        if (down.id != 0)
            service.web_presses().sent(down.id);
        return json();
    });
    calls.wait(ctx, deadline);
    WebActed  out;
    PageReply r = calls.reply(0);
    if (r.state == PageReply::State::Destroyed) {
        out.after = closed_reply(service);
        return out;
    }
    check_page_reply(r, timeout, true, ctx);
    json value = r.value;
    if (value.contains("relay")) {
        // An element of a plugin configuration frame: its page forwarded the action to the frame.
        const PageReply p = follow_relays(ctx, {t.web}, {value["relay"]}, deadline).front();
        if (p.state == PageReply::State::Destroyed) {
            out.after = closed_reply(service);
            return out;
        }
        check_page_reply(p, timeout, true, ctx);
        value = p.value;
        // What the dialog's page did is told by its own URLs and document, not the frame's.
        for (const char* key : {"href0", "href", "doc"}) {
            if (r.value.contains(key))
                value[key] = r.value[key];
            else
                value.erase(key);
        }
    }
    out.reply   = value;
    out.locator = web_locator(service, ctx, t, value, std::min(timeout, std::chrono::seconds(3)));
    out.after   = observe_page(service, ctx, t, before, value);
    return out;
}

// The DOM modifier flags of a click or gesture's `modifiers`, named as press_keys chords name them: ctrl
// and cmd are Command on macOS and Ctrl elsewhere, rawctrl the macOS Control key.
json web_modifiers(const json& args)
{
    json mods{{"ctrl", false}, {"meta", false}, {"shift", false}, {"alt", false}};
    for (const std::string& m : args.value("modifiers", std::vector<std::string>{})) {
#ifdef __APPLE__
        if (m == "ctrl" || m == "cmd")
            mods["meta"] = true;
        else if (m == "rawctrl")
            mods["ctrl"] = true;
#else
        if (m == "ctrl" || m == "cmd" || m == "rawctrl")
            mods["ctrl"] = true;
#endif
        else if (m == "shift" || m == "alt")
            mods[m] = true;
    }
    return mods;
}

// The journal records the locator an action on a ref reached, never the ref.
void journal_web(ToolContext& ctx, const json& args, const json& locator)
{
    json recorded = args;
    if (args.contains("target") && !is_web_locator(args["target"]) && !locator.is_null())
        recorded["target"] = locator;
    ctx.set_journal_args(std::move(recorded));
}

PressAgent agent_of(const WebTarget& t) { return press_agent(t.web, t.doc, t.key); }

// Ends every press mouse_gesture downs left on pages but `keep`'s, each released where it was pressed,
// all asked at once so an agent that doesn't answer holds up none of the others. Every gesture,
// click and answer_dialog does this first, even one the tool then refuses, except that a move or an
// up keeps the press of the agent it is on, which it belongs to; keys and input leave the presses, as
// a person can type while holding the mouse. One whose agent did not take the release (busy,
// failing) stays on record, so the next gesture asks again; a release taken late is past its
// deadline and not run. A gone web view or frame holds nothing. The releases get one second, so a
// call waits on them for less than the soft budget and still names the dialog it works in (which its
// target's lookup does next) before an open dialog would read as what the call waits on.
void end_web_press(McpService& service, ToolContext& ctx, const std::optional<PressAgent>& keep = std::nullopt)
{
    WebPresses&                          presses = service.web_presses();
    const std::vector<WebPresses::Press> held    = presses.releasable(keep);
    if (held.empty())
        return;
    const auto             end = std::chrono::steady_clock::now() + 1s;
    std::vector<WebHandle> webs;
    std::vector<json>      requests;
    for (const WebPresses::Press& p : held) {
        webs.push_back(p.agent.web);
        requests.push_back({{"op", "release"}, {"doc", p.agent.doc}, {"frame", p.agent.frame}, {"press", p.id}});
    }
    std::vector<PageReply> r = ask_each(ctx, webs, std::move(requests), end);
    // A frame's release is answered by the frame, through its page's relay.
    std::vector<std::size_t> relayed;
    std::vector<WebHandle>   relay_webs;
    std::vector<json>        rids;
    for (std::size_t i = 0; i < r.size(); ++i)
        if (r[i].state == PageReply::State::Answered && r[i].value.contains("relay")) {
            relayed.push_back(i);
            relay_webs.push_back(webs[i]);
            rids.push_back(r[i].value["relay"]);
        }
    if (!relayed.empty()) {
        const std::vector<PageReply> f = follow_relays(ctx, relay_webs, rids, end);
        for (std::size_t j = 0; j < relayed.size(); ++j)
            r[relayed[j]] = f[j];
    }
    for (std::size_t i = 0; i < r.size(); ++i)
        if (r[i].state == PageReply::State::Destroyed || (r[i].state == PageReply::State::Answered && !r[i].value.contains("error")))
            presses.drop(held[i].id);
}

ToolResult web_click(McpService& service, ToolContext& ctx, const json& args)
{
    if (args.contains("popup_item"))
        throw ToolFailure("popup_item is for the menus OrcaSlicer opens; a page's own menu is part of the page, so call list_widgets after "
                          "the click");
    const auto timeout = web_timeout(args);
    WebTarget  t       = resolve_web(service, ctx, args["target"], timeout);
    json       a{{"button", arg_string(args, "button", "left")}, {"double", arg_bool(args, "double", false)}, {"modifiers", web_modifiers(args)}};
    if (args.contains("x") || args.contains("y")) {
        a["x"] = arg_int(args, "x", 0);
        a["y"] = arg_int(args, "y", 0);
    }
    WebActed acted = web_act(service, ctx, t, "click", a, timeout);
    journal_web(ctx, args, acted.locator);
    // A text line is clicked through its block, whose own kind and name the page replies with; the
    // reply names what was listed.
    const bool text = t.kind == "label";
    json       out{{"clicked", text ? t.label : acted.reply.value("label", t.label)}, {"kind", text ? t.kind : acted.reply.value("kind", t.kind)}};
    if (acted.reply.contains("state") && acted.reply["state"].contains("checked"))
        out["checked"] = acted.reply["state"]["checked"];
    out.update(acted.after);
    return ToolResult::ok(out);
}

ToolResult web_input(McpService& service, ToolContext& ctx, const json& args)
{
    const auto timeout = web_timeout(args);
    WebTarget  t       = resolve_web(service, ctx, args["target"], timeout);
    WebActed   acted   = web_act(service, ctx, t, "input", {{"value", args["value"]}, {"submit", arg_bool(args, "submit", true)}}, timeout);
    journal_web(ctx, args, acted.locator);
    json       out{{"label", acted.reply.value("label", t.label)}};
    const json state = acted.reply.value("state", json::object());
    if (state.contains("checked"))
        out["checked"] = state["checked"];
    else if (state.contains("value"))
        out["value"] = state["value"];
    out.update(acted.after);
    return ToolResult::ok(out);
}

// Keys for a page: its element `t`, or with a null key the page's focused element.
ToolResult web_keys(McpService& service, ToolContext& ctx, const json& args, WebTarget t)
{
    if (args.contains("popup_item"))
        throw ToolFailure("popup_item is for the menus OrcaSlicer opens; a page's own menu is part of the page, so call list_widgets after "
                          "the keys");
    const std::string keys = arg_string(args, "keys"), text = arg_string(args, "text");
    json              a{{"text", text}};
    if (!keys.empty()) {
        const json key = web_key(keys);
        a["key"]       = key["key"];
        a["mods"]      = key["mods"];
    }
    WebActed acted = web_act(service, ctx, t, "key", a, web_timeout(args));
    journal_web(ctx, args, acted.locator);
    json out{{"sent", keys.empty() ? json() : json(keys)}, {"typed", text}, {"to", acted.reply.value("to", std::string("page"))}};
    out.update(acted.after);
    return ToolResult::ok(out);
}

ToolResult web_mouse(McpService& service, ToolContext& ctx, const json& args)
{
    // A move or an up finds its target first, to know whether it belongs to a press held there.
    const std::string action = arg_string(args, "action", "click");
    const bool        joins  = action == "move" || action == "up";
    if (!joins)
        end_web_press(service, ctx);
    if (args.contains("at") || args.contains("canvas"))
        throw ToolFailure("A page element's gesture takes x/y from its top left, not at or canvas");
    if (args.contains("popup_item"))
        throw ToolFailure("popup_item is for the menus OrcaSlicer opens; a page's own menu is part of the page");
    const auto timeout = web_timeout(args);
    WebTarget  t;
    if (joins) {
        // One whose target can't be found ends the presses held, as any other gesture does.
        try {
            t = resolve_web(service, ctx, args["target"], timeout);
        } catch (...) {
            end_web_press(service, ctx);
            throw;
        }
        end_web_press(service, ctx, agent_of(t));
    } else
        t = resolve_web(service, ctx, args["target"], timeout);
    json       a{{"action", action}, {"button", arg_string(args, "button", "left")},
                 {"double", arg_bool(args, "double", false)},   {"modifiers", web_modifiers(args)},
                 {"steps", arg_int(args, "steps", 10)},          {"delta", arg_int(args, "delta", 0)}};
    for (const char* k : {"x", "y"})
        if (args.contains(k))
            a[k] = arg_int(args, k, 0);
    auto point = [](const json& p) {
        if (!p.is_array() || p.size() != 2 || !p[0].is_number() || !p[1].is_number())
            throw ToolFailure("A page element's points are [x, y] from its top left");
        return p;
    };
    if (args.contains("to"))
        a["to"] = point(args["to"]);
    if (args.contains("waypoints")) {
        a["path"] = json::array();
        for (const json& p : args["waypoints"])
            a["path"].push_back(point(p));
    }
    // A down is on record before it is sent, as one whose reply never comes may still have pressed;
    // it belongs to the replay or script running the call, as a widget's press does, for the cleanup when that
    // ends. An up ends the presses its agent held when it was sent, not one a later down made. One
    // that failed or timed out ends none: past its deadline the page does not run it, so the press may
    // still be held, and a release of one that is not only answers.
    WebPresses&                      presses = service.web_presses();
    const PressAgent                 agent   = agent_of(t);
    const std::string                owner   = service.run_owner(service.jobs().ancestry(ctx.job_id()));
    const std::uint64_t              pressed = action == "down" ? presses.hold(agent, owner) : 0;
    const std::vector<std::uint64_t> ended   = action == "up" ? presses.of(agent) : std::vector<std::uint64_t>();
    // The page keeps the press's id, so a release ends that press and no later one.
    if (pressed != 0)
        a["press"] = pressed;
    WebActed acted;
    try {
        acted = web_act(service, ctx, t, "mouse", a, timeout, {pressed, owner.empty()});
    } catch (...) {
        // A down refused before it was sent is released as one that was: its release only answers.
        if (pressed != 0)
            presses.sent(pressed);
        throw;
    }
    for (std::uint64_t id : ended)
        presses.drop(id);
    journal_web(ctx, args, acted.locator);
    json out{{"done", a["action"]}, {"kind", acted.reply.value("kind", t.kind)}, {"label", acted.reply.value("label", t.label)}};
    out.update(acted.after);
    return ToolResult::ok(out);
}

// The page keys without a target go to: the focused web view, or the topmost dialog's only web view
// when the focus is on none of the dialog's own widgets. Null when the keys are a widget's. UI thread.
wxWebView* key_page(McpService& service)
{
    wxWindow* target = service.inspector().key_target(nullptr);
    if (auto* web = dynamic_cast<wxWebView*>(target))
        return web;
    auto* dialog = dynamic_cast<wxDialog*>(target);
    if (dialog == nullptr || dialog != service.modals().top())
        return nullptr;
    std::vector<wxWebView*> webs;
    for (wxWebView* w : visible_webviews())
        if (owning_window(w) == dialog)
            webs.push_back(w);
    wxWindow* focus = wxWindow::FindFocus();
    const bool own_widget = focus != nullptr && focus != dialog && owning_window(focus) == dialog && dynamic_cast<wxWebView*>(focus) == nullptr;
    return webs.size() == 1 && !own_widget ? webs.front() : nullptr;
}

// The window set_window's target names: the main window, the topmost modal dialog, or the window a ref or
// locator is in. Refuses one no tool may size or move. `locator` gets what the journal records for a
// ref. UI thread.
wxTopLevelWindow& window_target(McpService& service, const json& target, json& locator)
{
    wxWindow* window = nullptr;
    if (target == "main") {
        window = wxGetApp().mainframe;
        if (window == nullptr)
            throw ToolFailure("The main window is not up yet");
    } else if (target == "dialog") {
        window = service.modals().top();
        if (window == nullptr)
            throw ToolFailure(no_modal_dialog(service));
    } else {
        UiTarget t;
        try {
            t = service.inspector().resolve(target);
        } catch (const ToolFailure& e) {
            // A window's ref goes stale once the window is destroyed, and list_windows gives the open ones'.
            if (!target.is_string())
                throw;
            throw ToolFailure(std::string(e.what()) + ", or list_windows for the open windows", e.details);
        }
        locator = service.inspector().locator_for(t);
        window  = t.window.get() != nullptr ? wxGetTopLevelParent(t.window.get()) : nullptr;
    }
    if (dynamic_cast<wxPopupWindow*>(window) != nullptr)
        throw ToolFailure("The target is in a popup, which the window it opened from places; give that window (list_windows)");
    auto* tlw = dynamic_cast<wxTopLevelWindow*>(window);
    if (tlw == nullptr)
        throw ToolFailure("The target is in no window");
    if (tlw->GetName() == kHiddenFromAutomation)
        throw ToolFailure("The window is the person's own, which no tool reaches");
    if (auto* dlg = dynamic_cast<wxDialog*>(tlw); dlg != nullptr && is_native_dialog(dlg))
        throw ToolFailure("A native dialog is placed by the system; answer it with answer_dialog");
    // A closed dialog may be hidden rather than destroyed, so its ref still resolves.
    if (!tlw->IsShown())
        throw ToolFailure("The window is closed; list_windows lists the open windows");
    return *tlw;
}

// `window` while it is still one of the app's top-level windows, else null: set_window keeps a plain
// pointer across its UI closures and checks it before following it. UI thread.
wxTopLevelWindow* live_window(wxTopLevelWindow* window)
{
    for (wxWindow* w : wxTopLevelWindows)
        if (w == window)
            return window;
    return nullptr;
}

// set_window: checks the request and applies it in one UI closure, waits for the window to take it,
// then replies with what the window became. A size or position applies to the normal window, so a
// maximized or minimized one is restored first. Where the window system restores it later (GTK), the call
// waits for that before sizing, as a size given meanwhile is lost: on Wayland the compositor's answer to
// the restore sizes the window again, and wx would size it by the maximized window's decorations. A window
// brought back from the minimized state is finished restoring once it is back (finish_restore(), macOS).
ToolResult set_window(McpService& service, ToolContext& ctx, const json& args)
{
    const json target = args.value("target", json("main"));
    json       locator;
    // Never followed off the UI thread, and checked by live_window() on it: a wxWeakRef can't be made or
    // dropped on this thread.
    wxTopLevelWindow* window = nullptr;
    bool              main = false, modal = false, restoring = false, unminimizing = false;
    WindowSettling    settling;
    const bool        placing = args.contains("size") || args.contains("position");
    const std::string state   = arg_string(args, "state");
    const auto        minimized = [](const wxTopLevelWindow& w) { return std::string(window_state(w)) == "minimized"; };
    // The request, less the restore that comes before a size or position. UI thread.
    const auto apply = [&](wxTopLevelWindow& w) {
        if (args.contains("size"))
            settling.limited = resize_window(w, wxSize(args["size"][0].get<int>(), args["size"][1].get<int>()));
        // -1 is a place on a screen left of or above the primary one, not "keep this coordinate".
        if (args.contains("position"))
            w.Move(args["position"][0].get<int>(), args["position"][1].get<int>(), wxSIZE_ALLOW_MINUS_ONE);
        if (state == "maximized") {
            maximize_window(w);
        } else if (state == "minimized") {
            minimize_window(w);
        } else if (state == "normal") {
            settling.activation = settling.activation || (restore_needs_activation() && minimized(w));
            unminimizing        = unminimizing || minimized(w);
            restore_window(w);
        }
        if (arg_bool(args, "raise", false))
            raise_window(w);
    };
    ctx.run_ui([&] {
        wxTopLevelWindow& w   = window_target(service, target, locator);
        auto*             dlg = dynamic_cast<wxDialog*>(&w);
        main                  = &w == wxGetApp().mainframe;
        modal                 = dlg != nullptr && dlg->IsModal();
        WindowAbilities abilities;
        abilities.main            = main;
        abilities.modal           = modal;
        abilities.can_maximize    = w.HasFlag(wxMAXIMIZE_BOX);
        abilities.can_minimize    = w.HasFlag(wxMINIMIZE_BOX);
        abilities.positions_known = window_positions_known();
        if (const std::string refusal = window_request_refusal(args, abilities); !refusal.empty())
            throw ToolFailure(refusal);
        window = &w;
        if (placing && std::string(window_state(w)) != "normal") {
            settling.activation = restore_needs_activation() && minimized(w);
            unminimizing        = minimized(w);
            restoring           = restore_window(w);
        }
        if (!restoring)
            apply(w);
        return json();
    });
    journal_with_locator(ctx, args, locator);

    // The window manager may apply a request later (X11) or animate it (macOS): a wait ends at two equal
    // readings in a row in the state asked for. `holds` takes a reading and says whether the window held
    // still in `wanted` (any state when empty). UI thread.
    const char* closed = "The request was applied before the window closed";
    json        last;
    const auto  holds = [&](const std::string& wanted) {
        wxTopLevelWindow* w = live_window(window);
        if (w == nullptr)
            throw ToolFailure(closed);
        const wxPoint     pos  = w->GetScreenPosition();
        const wxSize      size = window_size(*w);
        const std::string now  = window_state(*w);
        const json        reading{pos.x, pos.y, size.x, size.y, now};
        return reading == std::exchange(last, reading) && (wanted.empty() || now == wanted);
    };
    const auto cancelled = [] { return ToolFailure("Cancelled: OrcaSlicer is closing or the replay or script was stopped"); };
    if (restoring) {
        // The rest of the request goes to the window the restore left, normal or not.
        poll_until(ctx, [&] { return holds("normal") ? json(true) : json(); }, state_change_timeout("normal"), std::chrono::milliseconds(50));
        if (ctx.cancelled())
            throw cancelled();
        ctx.run_ui([&] {
            wxTopLevelWindow* w = live_window(window);
            if (w == nullptr)
                throw ToolFailure(closed);
            apply(*w);
            return json();
        });
        last = json();
    }

    // For the main window the wait also lasts until its 3D view has drawn at the new size, which a
    // minimized window never does.
    std::string wanted = state;
    if (wanted.empty() && placing)
        wanted = "normal";
    const std::chrono::seconds timeout = state_change_timeout(wanted);
    settling.seconds                   = int(timeout.count());
    const json done                    = poll_until(
        ctx,
        [&]() -> json {
            if (wxTopLevelWindow* w = live_window(window); w != nullptr && unminimizing)
                unminimizing = !finish_restore(*w);
            settling.held = holds(wanted);
            if (!settling.held)
                return json();
            return !main || minimized(*live_window(window)) || view_drawn_at_size() ? json(true) : json();
        },
        timeout, std::chrono::milliseconds(50));
    if (done.is_null() && ctx.cancelled())
        throw cancelled();
    settling.drawn = !done.is_null();
    if (settling.held && !settling.drawn)
        settling.undrawn = undrawn_window(ctx);

    json out = ctx.observe_ui([&] {
        wxTopLevelWindow* w = live_window(window);
        if (w == nullptr)
            throw ToolFailure(closed);
        // list_windows's own entry, so the two report a window alike.
        const json entry = service.inspector().window_entry(*w, modal);
        json       reply{{"title", entry["title"]}, {"kind", entry["kind"]}, {"ref", entry["ref"]}, {"size", {entry["width"], entry["height"]}}};
        if (entry.contains("x"))
            reply["position"] = {entry["x"], entry["y"]};
        reply["state"]     = entry["state"];
        reply["resizable"] = entry["resizable"];
        reply["scale"]     = w->GetContentScaleFactor();
        // A minimized window may be on no display; the primary one stands for it then.
        int index = wxDisplay::GetFromWindow(w);
        for (unsigned i = 0; index == wxNOT_FOUND && i < wxDisplay::GetCount(); ++i)
            if (wxDisplay(i).IsPrimary())
                index = int(i);
        reply["display"] = display_area(unsigned(std::max(index, 0)));
        if (arg_bool(args, "raise", false))
            reply["active"] = w->IsActive();
        return reply;
    });
    add_window_notes(args, settling, out);
    return ToolResult::ok(out);
}

} // namespace

bool view_drawn_at_size()
{
    GLCanvas3D* canvas = shown_canvas();
    if (canvas == nullptr || !canvas->get_wxglcanvas()->IsShownOnScreen())
        return true;
    // What render() gives the viewport: the canvas's size in GL pixels, at least 10.
    const Size                size     = canvas->get_canvas_size();
    const std::array<int, 4>& viewport = ui_plater().get_camera().get_viewport();
    if (viewport[2] == std::max(10, size.get_width()) && viewport[3] == std::max(10, size.get_height()))
        return true;
    canvas->set_as_dirty();
    return false;
}

void add_window_notes(const json& args, const WindowSettling& settling, json& reply)
{
    json adjusted = json::array();
    for (const std::string field : {"size", "position", "state"})
        if (args.contains(field) && reply.contains(field) && reply[field] != args[field])
            adjusted.push_back(field);
    std::string note = settling.limited ? "size is held to the window's minimum or maximum size" : "";
    const auto  add  = [&note](const std::string& why) { note += (note.empty() ? "" : "; ") + why; };
    if (!settling.held && settling.activation)
        add("the compositor did not bring the window back: Wayland lets an app restore its minimized window only by "
            "activating it, which the compositor allows only while the app may take the focus, so the person restores it "
            "from the taskbar");
    else if (!settling.held)
        add("the window manager did not apply the request within " + std::to_string(settling.seconds) + " s");
    else if (!settling.drawn)
        add("the 3D view did not draw at the window's size within " + std::to_string(settling.seconds) + " s" +
            (settling.undrawn.empty() ? "" : ": " + settling.undrawn));
    else if (note.empty() && !adjusted.empty())
        note = "the window manager adjusted the request";
    if (!adjusted.empty())
        reply["adjusted"] = std::move(adjusted);
    if (!note.empty())
        reply["note"] = note;
}

std::string window_request_refusal(const json& args, const WindowAbilities& window)
{
    const bool size = args.contains("size"), position = args.contains("position");
    if (!size && !position && !args.contains("state") && !args.contains("raise"))
        return "Give at least one of size, position, state or raise; list_windows reads a window's geometry without changing it";
    // Two whole numbers that fit wx's int coordinates, both positive for a size. Whole floats pass, as
    // they pass the schema's integer type.
    const auto is_pair = [](const json& value, bool positive) {
        const auto whole = [positive](const json& n) {
            if (!n.is_number())
                return false;
            const double d = n.get<double>();
            return d == std::floor(d) && d >= (positive ? 1.0 : double(std::numeric_limits<int>::min())) &&
                   d <= double(std::numeric_limits<int>::max());
        };
        return value.is_array() && value.size() == 2 && std::all_of(value.begin(), value.end(), whole);
    };
    if (size && !is_pair(args.at("size"), true))
        return "Give size as [width, height], two positive integers";
    if (position && !is_pair(args.at("position"), false))
        return "Give position as [x, y], two integers";
    const json state = args.value("state", json());
    if ((state == "maximized" || state == "minimized") && (size || position))
        return "A " + state.get<std::string>() +
               " window's size and position are set by the system: give state alone, or size and position without state";
    if (state == "maximized" && !window.main && !window.can_maximize)
        return "This window's title bar has no maximize button: give it a size instead";
    if (state == "minimized" && window.modal)
        return "A modal dialog never minimizes, as the app would stay blocked with nothing showing: close the dialog instead";
    if (state == "minimized" && !window.main && !window.can_minimize)
        return "This window's title bar has no minimize button: move it aside with position, or close it, instead";
    if (position && !window.positions_known)
        return "position needs X11: Wayland gives an app no way to place its window or to read where it is. Leave position "
               "out, or start OrcaSlicer with GDK_BACKEND=x11 to run it under XWayland, where every field works";
    return {};
}

json display_area(unsigned index)
{
    const wxRect area = wxDisplay(index).GetClientArea();
    return {{"index", index}, {"x", area.x}, {"y", area.y}, {"width", area.width}, {"height", area.height}};
}

void register_ui_tools(CommandCatalog& catalog)
{
    catalog.add(inspect_tool(
        "ui", "list_windows", L("List the windows"),
        "List OrcaSlicer's open windows: the main window, then the windows that don't block the app (Plugins, an "
        "object's settings, a dock panel floating on its own), then the modal dialogs in the order they opened, the "
        "topmost last. A window's ref goes to screenshot as target to capture that window, to list_widgets as target to "
        "list it alone, and to press_keys as target; its title is the within of locators for its widgets.\n"
        "Reply: each window's title, kind (main, window, dialog, or panel: a dock panel floating on its own), modal, "
        "ref, web (it shows a web page), x and y on screen (not on Wayland, which doesn't say), width and height, state "
        "(normal, maximized or minimized) and resizable (it has a resize border); set_window changes them.\n"
        "A native dialog (a message box, a file picker) has native: true and no ref: answer_dialog answers it. hidden: "
        "true marks the person's own windows (Action Replay, the Run Script panel and their prompts, the Python scripts "
        "setting), which no tool reaches and screenshots leave out.",
        object_schema({{"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 200}, {"description", "Default 100."}}},
                       {"offset", {{"type", "integer"}, {"minimum", 0}}}}),
        [](const json& args, ToolContext& ctx) {
            McpService& service = ctx.service();
            return ToolResult::ok(page(ctx.run_ui([&] { return service.inspector().windows(); }), args));
        }));

    {
        NativeCommand def = recover_input_tool(
            "ui", "set_window", L("Size or move a window"),
            "Size, move, maximize, minimize or raise a window: the main window (default), the topmost modal dialog, or one "
            "list_windows lists, also while a modal dialog is open. Size the main window before set_view fits the camera and "
            "before start_recording, as a recording keeps its first frame's size. size and position are in the units "
            "list_windows reports, whose x/y and width/height are read as position and size too. A size or position restores "
            "a maximized or minimized window first. A window takes only the states its title bar offers; a modal dialog "
            "never minimizes. A refused call changes nothing. Replies once the window holds still and the 3D view has drawn "
            "at its new size (3 s at most): title, kind, ref, size, position, state, resizable, scale (screenshot pixels per "
            "window unit), display (its work area), active with raise, and adjusted with a note: what the OS or a size limit "
            "changed.",
            object_schema({{"target", {{"type", json::array({"string", "object"})},
                                       {"description", "main (default), dialog (the topmost modal "
                                                       "dialog), or a window's ref or locator from list_windows; a widget's ref or "
                                                       "locator picks the window it is in."}}},
                           {"size", array_prop({{"type", "integer"}}, "[width, height], held to the window's minimum and maximum size.")},
                           {"position", array_prop({{"type", "integer"}}, "[x, y] of the window's top-left corner on screen; not on Wayland.")},
                           {"state", enum_prop({"normal", "maximized", "minimized"},
                                               "normal restores a maximized or minimized window; maximized and minimized take no size "
                                               "or position.")},
                           {"raise", prop("boolean", "Bring the window in front of the app's other windows; active in the reply says "
                                                     "whether it became the active one.")}}),
            [](const json& args, ToolContext& ctx) { return set_window(ctx.service(), ctx, args); });
        def.allowed_during_operation = true;
        catalog.add(def);
    }

    {
        NativeCommand def = inspect_tool(
            "ui", "list_widgets", L("List the widgets"),
            "Describe what is on screen as an indented tree, one element per line with a ref to pass to click, set_widget_value, "
            "press_keys and run_javascript (a page element's to mouse_gesture too): the widgets of every shown window, the rows of lists, "
            "trees and tables, each shown popup after the window it opened from, the main menus, the 3D canvas's toolbar "
            "items and gizmos, each ImGui panel drawn in the last frame with its items and text, and the elements of each "
            "shown web page (dialogs, the setup wizard, sign-in, plugin windows) under its web view. With a modal dialog "
            "open, only the topmost dialog's pages are read. Refs go stale when their element goes away: call list_widgets "
            "again then. Prefer run_action and the typed tools when one fits.\n"
            "A locator names an element without a ref, in any session (export_replay records them); index (0-based) picks "
            "one of several matches:\n"
            "- a widget: {label, class, name, within, index}, within being its window's title; a notebook page: {tab: "
            "\"General\", within}\n"
            "- a setting's field: {option: \"layer_height\"}\n"
            "- {menu: \"File>Export>Export G-code\"}, {gltoolbar: \"arrange\"}, {gizmo: \"Move\"}\n"
            "- an ImGui item: {imgui: \"label\", imgui_name?: \"name\", window?, canvas?, index?}, by its label, its name or both\n"
            "- a row of a list, tree, table or drop-down list: {row: \"Plate 1>Cube>Cylinder\", column?, within?, index?}, by "
            "its last labels; column names the cell to act on\n"
            "- a plate or one of its icons in the Prepare view: {plate_icon: \"lock\", plate: 0}, the icon being plate, "
            "delete, orient, arrange, lock, settings, name, move_to_front, filament_map or imex_mode\n"
            "- a web page's element: {web: \"Next\", kind?: \"button\", css?: \"#AcceptBtn\", within?, index?}, by the label "
            "list_widgets lists (exactly, else ignoring case, else a part of it), its kind, a CSS selector (the id a line "
            "shows as name: name=#save is {css: \"#save\"}), or together. Without within it looks in the topmost dialog's "
            "pages while one is open, else in every shown page, and it waits up to 3 s for its element.",
            object_schema({{"target", prop("string", "all (default), main, dialog (the topmost modal), popup, or a window's ref (list_windows).")},
                           {"query", prop("string", "Case-insensitive text to look for in labels, values, names and classes.")},
                           {"max_nodes", {{"type", "integer"}, {"minimum", 1}, {"maximum", 5000}, {"description", "Default 400."}}},
                           {"include_imgui", prop("boolean", "List ImGui items (default true).")},
                           {"include_menus", prop("boolean", "List menu items (default true).")},
                           {"include_web", prop("boolean", "List the elements of web pages (default true).")},
                           {"max_web_nodes",
                            {{"type", "integer"}, {"minimum", 1}, {"maximum", 5000}, {"description", "Page elements, apart from max_nodes; default 300."}}}}),
            [](const json& args, ToolContext& ctx) {
                McpService& service = ctx.service();
                SnapshotOptions o;
                o.root          = arg_string(args, "target", "all");
                o.query         = arg_string(args, "query");
                o.max_nodes     = arg_int(args, "max_nodes", 400);
                o.include_imgui = arg_bool(args, "include_imgui", true);
                o.include_menus = arg_bool(args, "include_menus", true);
                o.include_web   = arg_bool(args, "include_web", true);
                o.max_web_nodes = arg_int(args, "max_web_nodes", 300);
                // The pages are read before the walk, since a page answers a while after it is asked; one
                // still loading gets up to 2 s first.
                std::vector<WebHandle> pages, behind;
                for (int i = 0; i < 20; ++i) {
                    const bool busy = ctx.run_ui([&] {
                                             std::vector<wxWebView*> later;
                                             const std::vector<wxWebView*> read = service.inspector().snapshot_pages(o, &later);
                                             pages.clear();
                                             behind.clear();
                                             std::transform(read.begin(), read.end(), std::back_inserter(pages), web_handle);
                                             std::transform(later.begin(), later.end(), std::back_inserter(behind), web_handle);
                                             return json(std::any_of(read.begin(), read.end(), [](wxWebView* w) { return w->IsBusy(); }));
                                         }).get<bool>();
                    if (!busy)
                        break;
                    std::this_thread::sleep_for(100ms);
                }
                const std::vector<PageReply> read = ask_pages(ctx, pages, {{"op", "snapshot"}, {"max", o.max_web_nodes}, {"query", o.query}},
                                                              std::chrono::steady_clock::now() + 3s);
                json snap = ctx.run_ui([&] {
                    for (std::size_t i = 0; i < pages.size(); ++i) {
                        const wxWebView* web = web_view_of(pages[i]);
                        const PageReply& r   = read[i];
                        if (web == nullptr || r.state == PageReply::State::Destroyed)
                            continue;
                        if (r.state == PageReply::State::Answered && !r.value.contains("error"))
                            o.pages[web] = r.value;
                        else
                            o.pages[web] = {{"note", r.state == PageReply::State::Answered ? "its page could not be read: " + r.value["error"].get<std::string>() :
                                                     r.state == PageReply::State::Failed ? "its page could not be read: " + r.error :
                                                                                           std::string("its page did not answer in time")}};
                    }
                    for (WebHandle h : behind)
                        if (const wxWebView* web = web_view_of(h))
                            o.pages[web] = {{"note", "behind a modal dialog"}};
                    return service.inspector().snapshot(o);
                });
                ToolResult r = ToolResult::ok();
                const std::string tree = snap["tree"].get<std::string>();
                snap.erase("tree");
                r.data = std::move(snap);
                r.content.push_back({{"type", "text"}, {"text", tree.empty() ? std::string("(nothing shown)") : tree}});
                return r;
            });
        catalog.add(def);
    }

    catalog.add(recover_input_tool(
        "ui", "click", L("Click an element"),
        "Click a GUI element as a person would. Menu items, buttons, checkboxes, toggles, radio buttons, toolbar items and "
        "gizmos are activated the way their owners expect: a gizmo opens and a second click closes it (Text on an object "
        "without text creates a text part: created). Other widgets and ImGui items get a mouse click once scrolled into "
        "view; with x/y, at that point of a 3D canvas or custom-drawn widget, in logical pixels from its top left. A row is "
        "selected as a click on its cell selects it (ctrl adds, shift extends; in the object list the named column acts, as "
        "Printable toggles), double activates it and a right click opens its menu; a drop-down list's row is picked. A "
        "plate icon does what it does in the Prepare view: lock replies locked, arrange and orient reply done once their "
        "job ends, settings and filament_map list the dialog they open (dialogs). Modifiers reach the 3D canvas, the object "
        "list and handlers that read them. A menu the click opens is never shown: the reply lists it as popup_menu, or "
        "popup_item runs one of its items (ran). A click that opens a dialog replies blocked_by_dialog.\n"
        "A web page's element gets the pointer and mouse events of a click at its centre (or x/y) once scrolled into view, "
        "refused when something else covers that point. The reply reads back checked, and has closed when the click closed "
        "its dialog (with dialogs, those still open under it, which still need an answer), navigated (with url) when it "
        "changed the page, or dialogs when it opened another. Otherwise page says what changed on the page within 300 ms: "
        "changed (false when its lines are as before), the list_widgets lines added and removed, partial when a plugin "
        "configuration frame could not be compared, and posted, how many messages the page sent OrcaSlicer.",
        object_schema({{"target", kTargetSchema},
                       {"button", enum_prop({"left", "right", "middle"}, "Default left.")},
                       {"double", prop("boolean", "Double click.")},
                       {"modifiers", kModifiersSchema},
                       {"x", prop("integer", "x of the point to click, in a 3D canvas, a custom-drawn widget or a page element.")},
                       {"y", prop("integer", "y of the point to click.")},
                       {"popup_item", kPopupItemSchema},
                       {"timeout_s", kPageTimeoutSchema}},
                      {"target"}),
        [](const json& args, ToolContext& ctx) {
            McpService& service = ctx.service();
            end_web_press(service, ctx); // a click ends a press held on a page, even one refused here
            if (is_web_target(args["target"]))
                return web_click(service, ctx, args);
            refuse_web_view(service, ctx, args["target"]);
            ClickOptions o;
            o.button       = arg_string(args, "button", "left");
            o.double_click = arg_bool(args, "double", false);
            o.modifiers    = args.value("modifiers", std::vector<std::string>{});
            o.at_point     = args.contains("x") || args.contains("y");
            o.x            = arg_int(args, "x", 0);
            o.y            = arg_int(args, "y", 0);
            json         locator;
            GLCanvas3D*  canvas = nullptr;
            int          gizmo  = -1;
            InputMenu    menu(service, ctx, arg_string(args, "popup_item"));
            // A list's rows get their places a frame after they change (on GTK; a dialog's tree rebuilt
            // by a preset choice), so a row is scrolled to first and given up to 1 s to take its place.
            bool placed = false;
            for (int i = 0; i < 20; ++i) {
                placed = ctx.run_ui([&] {
                                UiTarget t = service.inspector().resolve(args["target"]);
                                guard_target(service, target_window(t));
                                return json(service.inspector().place_row(t));
                            })
                             .get<bool>();
                if (placed)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            // Without frames the row never takes its place, and the click would blame the window's size.
            if (!placed)
                if (const std::string undrawn = undrawn_window(ctx); !undrawn.empty())
                    throw ToolFailure("The row can't be scrolled into the list's view: " + undrawn);
            json out = ctx.run_ui([&] {
                UiTarget t = service.inspector().resolve(args["target"]);
                locator    = service.inspector().locator_for(t);
                guard_target(service, target_window(t));
                canvas = t.canvas;
                gizmo  = t.gizmo;
                menu.arm();
                json clicked = service.inspector().click(t, o);
                // A 3D view posts a click's work to itself; a plate icon posts its dialogs to the plater.
                const bool  plate_icon = t.kind == UiTarget::Kind::PlateIcon;
                GLCanvas3D* view       = service.inspector().canvas_of(&t);
                if (plate_icon && o.button == "left")
                    menu.mark(&ui_plater());
                else if (view != nullptr)
                    menu.mark(view->get_wxglcanvas());
                menu.dialogs = plate_icon;
                return clicked;
            });
            journal_with_locator(ctx, args, locator);
            if (out.value("started", false)) {
                // A plate's arrange or orient icon started a job on the UI job worker: the reply
                // follows its end.
                out.update(poll_ui(
                    ctx, [] { return ui_plater().get_ui_job_worker().is_idle() ? json{{"done", true}} : json(); },
                    std::chrono::minutes(30), "the plate's " + out.value("clicked", std::string()) + " job"));
            }
            if (out.value("creating", false)) {
                // The Text gizmo's icon with no text selected queued a job that creates a text part
                // and opens the gizmo when it ends.
                out = poll_ui(
                    ctx,
                    [&]() -> json {
                        if (!ui_plater().get_ui_job_worker().is_idle())
                            return json();
                        const auto canvases = live_canvases();
                        const bool active   = std::find(canvases.begin(), canvases.end(), canvas) != canvases.end() &&
                                            int(canvas->get_gizmos_manager().get_current_type()) == gizmo;
                        return json{{"gizmo", out["gizmo"]}, {"created", true}, {"active", active}};
                    },
                    std::chrono::seconds(30), "the new text part");
            }
            out.update(menu.read());
            return ToolResult::ok(out);
        }));

    catalog.add(recover_input_tool(
        "ui", "set_widget_value", L("Set a widget's value"),
        "Set a widget's value and send the events a person's edit would. Widgets: text fields (submit, the default, also "
        "presses Enter and leaves the field, which is when Orca's setting fields commit), spin controls, sliders, choice, "
        "combo and list boxes (by option text or index), checkboxes, toggles and a row's checkbox (true or false), and a "
        "radio button (an Orca radio box too) or a selector's option, as the dialog message shows them (*) / ( ) (true "
        "selects it; false on the "
        "selected one is refused, as only selecting another unselects it). "
        "ImGui items, by their kind, with the value read back: a field is typed into (a slider through the number field "
        "beside it, multiline text with \\n), a combo's option is picked by its text or 0-based index, and a checkbox, radio "
        "or toggle is clicked when it differs. A web page's fields, as typing would leave them: text, number, range, colour "
        "and date fields and editable text get the value with input and change events (submit then leaves the field; "
        "press_keys Return presses Enter), a select its option by text, value or index, and a checkbox or radio is clicked when "
        "it differs; the value is read back and page says what changed, as for click. File fields can't be set. For "
        "settings, set_settings is more reliable.",
        object_schema({{"target", kTargetSchema},
                       {"value", {{"type", json::array({"string", "number", "boolean"})}, {"description", "The new value."}}},
                       {"submit", prop("boolean", "Press Enter / commit after setting (default true).")},
                       {"timeout_s", kPageTimeoutSchema}},
                      {"target", "value"}),
        [](const json& args, ToolContext& ctx) {
            McpService& service = ctx.service();
            if (is_web_target(args["target"]))
                return web_input(service, ctx, args);
            refuse_web_view(service, ctx, args["target"]);
            json locator;
            json out = ctx.run_ui([&] {
                UiTarget t = service.inspector().resolve(args["target"]);
                locator    = service.inspector().locator_for(t);
                guard_target(service, target_window(t));
                return service.inspector().input(t, args["value"], arg_bool(args, "submit", true));
            });
            journal_with_locator(ctx, args, locator);
            settle(ctx);
            return ToolResult::ok(out);
        }));

    catalog.add(recover_input_tool(
        "ui", "press_keys", L("Press keys"),
        "Send a key chord (\"Ctrl+S\", \"Escape\", \"Return\", \"Delete\", \"Shift+Tab\", \"Left\", \"F5\") and/or type text to "
        "a target or, without one, to the focused window (the topmost dialog while one is open, the shown 3D view when "
        "nothing has focus). As with a real key press, the shortcut handling of the target's panels and window sees the keys "
        "first. Typing reaches ImGui fields and key handlers; fill a widget's text field with set_widget_value. A menu the keys open "
        "is listed as popup_menu, or popup_item runs one of its items. Keys for a web page (its element, or without a "
        "target the focused web view or the topmost dialog's only page) go to the page's focused element with the "
        "browser's default actions: typing, Backspace/Delete, Enter (submits a form, activates a button), Space, Tab moving "
        "the focus, arrows stepping a select, range or radio group; page says what they changed, as for click.",
        object_schema({{"target", kTargetSchema},
                       {"keys", prop("string", "Key chord.")},
                       {"text", prop("string", "Characters to type.")},
                       {"popup_item", kPopupItemSchema},
                       {"timeout_s", kPageTimeoutSchema}}),
        [](const json& args, ToolContext& ctx) {
            McpService& service = ctx.service();
            const std::string keys = arg_string(args, "keys"), text = arg_string(args, "text");
            if (keys.empty() && text.empty())
                throw ToolFailure("Give keys, text or both");
            if (args.contains("target") && is_web_target(args["target"]))
                return web_keys(service, ctx, args, resolve_web(service, ctx, args["target"], web_timeout(args)));
            // Keys for a web view itself, or for none while a page has the focus, go to that page's
            // focused element.
            WebTarget page;
            json      view_locator;
            if (args.contains("target"))
                page.web = web_view_target(service, ctx, args["target"], page.within, &view_locator);
            else
                ctx.run_ui([&] {
                    if (wxWebView* web = key_page(service)) {
                        page.web    = web_handle(web);
                        page.within = within_of(web);
                    }
                    return json();
                });
            if (page.web != 0) {
                ToolResult r = web_keys(service, ctx, args, page);
                journal_with_locator(ctx, args, view_locator);
                return r;
            }
            json         locator;
            InputMenu    menu(service, ctx, arg_string(args, "popup_item"));
            json out = ctx.run_ui([&] {
                std::optional<UiTarget> t;
                if (args.contains("target")) {
                    t       = service.inspector().resolve(args["target"]);
                    locator = service.inspector().locator_for(*t);
                }
                guard_target(service, service.inspector().key_target(t ? &*t : nullptr));
                menu.arm();
                return service.inspector().key(t ? &*t : nullptr, keys, text);
            });
            journal_with_locator(ctx, args, locator);
            out.update(menu.read());
            return ToolResult::ok(out);
        }));

    const json target_schema = {
        {"type", "object"},
        {"description", "A point of the model: {object, instance (default 0), part (default 0), and point [x, y, z] in "
                        "the part's own mesh coordinates (frame \"part\", the default) or the scene's (frame \"world\"), or "
                        "spot: top | bottom | front | back | left | right, where a ray along that axis first meets the part}."},
        {"properties",
         {{"object", {{"type", "integer"}, {"minimum", 0}}},
          {"instance", {{"type", "integer"}, {"minimum", 0}}},
          {"part", {{"type", "integer"}, {"minimum", 0}}},
          {"point", array_prop({{"type", "number"}}, "[x, y, z] mm")},
          {"frame", enum_prop({"part", "world"}, "Default part.")},
          {"spot", enum_prop({"top", "bottom", "front", "back", "left", "right"}, "A side of the part.")}}},
        {"required", {"object"}},
        {"additionalProperties", false}};
    const json pixel_or_target = {{"type", json::array({"array", "object"})},
                                  {"description", "A canvas pixel [x, y] or a point of the model, as for at."}};
    std::vector<std::string> mouse_canvases{"3d", "preview", "assemble", "current"};
#ifdef SLIC3R_CAD
    mouse_canvases.insert(mouse_canvases.end() - 1, "design");
#endif
    catalog.add(recover_input_tool(
        "ui", "mouse_gesture", L("Mouse gesture on the 3D view"),
        "Make a mouse gesture on a 3D canvas as a person would: move, down, up, click, drag or wheel. It starts at `at`, a "
        "point of the model, which is projected through the camera and by default checked to land on that part (check "
        "\"point\" only projects it), or at x/y, logical pixels from the canvas's top left. A drag goes through `waypoints` to "
        "`to`, `steps` moves per leg: on an object it moves the object, in a painting gizmo it paints from a press on the "
        "object being painted (waypoints zigzagging over an area paint all of it), and elsewhere it turns or pans the camera "
        "as this install's mouse settings say, so turn the camera with set_view camera or orbit instead. A wheel turns delta "
        "notches; positive zooms in or scrolls a panel up. Modifiers are held as keys for the gesture (shift for Measure's "
        "point picking, alt for erasing). A down keeps its button and modifiers held, so moves drag, until an up or another "
        "gesture. Grabbers are set through the gizmo's panel fields or transform, not dragged. A menu a right click opens "
        "is listed as popup_menu, or popup_item (with click or up) runs one of its items. The reply's camera_moved says the "
        "gesture moved the camera, so earlier screenshots and pixels no longer match the view.\n"
        "With target, the gesture is on a web page's element instead (a canvas such as the colour picker's spectrum, a "
        "slider, a sortable or scrolled list): x/y, to and waypoints are logical pixels from its top left, a drag keeps the "
        "pointer captured where the page asks for it, and a wheel scrolls what is under the pointer unless the page takes "
        "it; page says what the gesture changed there, as for click.",
        object_schema({{"action", enum_prop({"move", "down", "up", "click", "drag", "wheel"}, "Default click.")},
                       {"canvas", enum_prop(mouse_canvases, "Default current, the view shown. design takes x/y only.")},
                       {"at", target_schema},
                       {"x", prop("integer", "Start x, instead of at.")},
                       {"y", prop("integer", "Start y, instead of at.")},
                       {"to", pixel_or_target},
                       {"waypoints", array_prop(pixel_or_target, "Where a drag goes after the start, before to.")},
                       {"check", enum_prop({"surface", "point"}, "For model points: surface (default) or projection only.")},
                       {"tolerance", {{"type", "number"}, {"minimum", 0}, {"description", "Surface check distance in mm (default 1% of the part's size, at least 0.1)."}}},
                       {"steps", {{"type", "integer"}, {"minimum", 1}, {"maximum", 200}, {"description", "Moves per drag leg (default 10)."}}},
                       {"button", enum_prop({"left", "right", "middle"}, "Default left.")},
                       {"double", prop("boolean", "Double click.")},
                       {"delta", prop("integer", "Wheel notches.")},
                       {"modifiers", kModifiersSchema},
                       {"popup_item", kPopupItemSchema},
                       {"target", {{"type", json::array({"string", "object"})},
                                   {"description", "A web page's element (its ref or a {web}/{css} locator) to gesture on instead of a 3D canvas."}}},
                       {"timeout_s", kPageTimeoutSchema}}),
        [](const json& args, ToolContext& ctx) {
            McpService& service = ctx.service();
            if (args.contains("target") && is_web_target(args["target"]))
                return web_mouse(service, ctx, args);
            end_web_press(service, ctx); // a gesture elsewhere ends a press held on a page, even one refused here
            if (args.contains("target"))
                throw ToolFailure("mouse_gesture's target is a web page's element; a 3D canvas is chosen with canvas");
            const std::string action = arg_string(args, "action", "click");
            if (args.contains("popup_item") && action != "click" && action != "up")
                throw ToolFailure("popup_item goes with action click or up, which open a right click's menu");
            const std::string owner = service.run_owner(service.jobs().ancestry(ctx.job_id()));
            InputMenu         menu(service, ctx, arg_string(args, "popup_item"));
            json out = ctx.run_ui([&] {
                // A client's press that runs after the endpoint closed would stay held, with no one left
                // to release it.
                if (owner.empty() && !service.endpoint_open())
                    throw ToolFailure("The MCP server was turned off before this gesture ran; it was not performed");
                // The canvases are in the main window, which takes no input while a dialog is open.
                guard_target(service, wxGetApp().mainframe);
                try {
                    ui_plater(); // refuses while OrcaSlicer starts or closes
                    const std::string name   = arg_string(args, "canvas", "current");
                    GLCanvas3D*       canvas = name == "current" ? shown_canvas() : canvas_by_name(name);
                    if (canvas == nullptr || !canvas->get_wxglcanvas()->IsShownOnScreen())
                        throw ToolFailure(name == "design" ? "The Design view is not shown; set_view {\"tab\": \"design\"} first" :
                                                             "That 3D view is not shown; switch to it with set_view first");
                    // The Design view sets up its GL at its first frame, as DesignCanvas guards its own renders.
                    if (canvas == design_canvas() && !canvas->is_initialized())
                        throw ToolFailure("The Design view has not drawn yet");
                    // A gesture off the model turns, pans or zooms the camera as the user's mouse settings
                    // say, which leaves earlier screenshots and pixels showing another view.
                    const Camera&     camera = ui_plater().get_camera();
                    const Transform3d view   = camera.get_view_matrix();
                    const double      zoom   = camera.get_zoom();
                    menu.arm();
                    json done = service.inspector().mouse(*canvas, args, owner);
                    menu.mark(canvas->get_wxglcanvas());
                    if (canvas != design_canvas() && (!camera.get_view_matrix().isApprox(view) || camera.get_zoom() != zoom))
                        done["camera_moved"] = true;
                    return done;
                } catch (...) {
                    // A gesture that fails ends a press held from before it, as any other gesture would.
                    service.inspector().release_held(nullptr);
                    throw;
                }
            });
            out.update(menu.read());
            return ToolResult::ok(out);
        }));

    catalog.add(mutate_tool(
        "ui", "pick_gizmo_file", L("Pick a file for the open gizmo"),
        "Do what a file picker inside the open gizmo does, with a path instead of its dialog: svg_change loads another SVG "
        "file into the SVG part being edited, svg_save_as writes that part's SVG to a new file (an existing one only with "
        "overwrite), texture_import imports a height-map image into the texture library and puts it on Texture "
        "Displacement's active layer. To add an SVG part, use run_action add_primitive_svg with a path. Gizmos open with "
        "click {target: {gizmo: name}}, and their panels take set_widget_value / click on the ImGui items list_widgets lists.",
        object_schema({{"action", enum_prop({"svg_change", "svg_save_as", "texture_import"}, "What to do.")},
                       {"path", prop("string", "Absolute path of the file to read or write.")},
                       {"overwrite", prop("boolean", "svg_save_as: replace an existing file.")}},
                      {"action", "path"}),
        [](const json& args, ToolContext& ctx) {
            const std::string action = arg_string(args, "action");
            json out = ctx.run_ui([&]() -> json {
                GLCanvas3D* canvas = ui_plater().get_current_canvas3D();
                if (canvas == nullptr)
                    throw ToolFailure("No 3D view is shown");
                GLGizmosManager&                 gizmos = canvas->get_gizmos_manager();
                const GLGizmosManager::EType     needed = action == "texture_import" ? GLGizmosManager::TextureDisplacement : GLGizmosManager::Svg;
                if (gizmos.get_current_type() != needed)
                    throw ToolFailure(needed == GLGizmosManager::Svg ? "Open the SVG gizmo on an SVG part first" :
                                                                       "Open the Texture displacement gizmo first");
                std::string err;
                const auto  path = action == "svg_save_as" ? resolve_output_path(arg_string(args, "path"), arg_bool(args, "overwrite", false), err) :
                                                             resolve_input_path(arg_string(args, "path"), err);
                if (!path)
                    throw ToolFailure(err);
                const std::string file = path_to_utf8(*path);
                if (action == "texture_import") {
                    if (!dynamic_cast<GLGizmoTextureDisplacement*>(gizmos.get_current())->import_texture(file, err))
                        throw ToolFailure(err);
                    return {{"imported", file}};
                }
                auto* svg = dynamic_cast<GLGizmoSVG*>(gizmos.get_current());
                if (action == "svg_change") {
                    if (!svg->change_file(file))
                        throw ToolFailure("The file has no shapes the SVG gizmo can use");
                    return {{"changed_to", file}};
                }
                if (!svg->save_svg_as(file))
                    throw ToolFailure("Could not write " + file);
                return {{"saved", file}};
            });
            settle(ctx);
            return ToolResult::ok(out);
        }));

    catalog.add(recover_input_tool(
        "core", "answer_dialog", L("Answer a dialog"),
        "Press a button of the topmost modal dialog: yes, no, ok, cancel, close, apply, a button's label, or a numeric id. "
        "A disabled button (enabled: false) is refused, the dialog's message showing what it still needs, and so is one on "
        "a page the dialog does not show, except a hidden cancel or close, which ends the dialog. The label of an option of a "
        "selector made of buttons (Orca's radio groups, tab bars and multi-switches, shown as (*) / ( ) in the message) "
        "selects that option and leaves the dialog open; set other radio buttons and switches with click or set_widget_value "
        "true, and notebook tabs with click. "
        "dialog.title, text the dialog's title contains, makes sure which dialog that is; it must be the topmost. Reply: "
        "closed: true when it closed, and dialogs, those still open. A native message box (native: true) is pressed through "
        "the OS, by one of the buttons get_app_state.dialogs lists for it; a file or folder picker can't be answered here, nor "
        "any native dialog on macOS (its buttons list is empty): register auto_answer_dialogs before opening them. A web "
        "dialog's buttons are on its page: a label is pressed there, waiting up to 3 s for it to show, so the page's own "
        "handler runs. With no such page button, or a page busy for those 3 s, cancel and close end the dialog, and ok, yes, "
        "no and apply are refused rather than skip the page's commit; page_buttons then lists the page's buttons.",
        object_schema({{"button", prop("string", "Button to press.")},
                       {"dialog", {{"type", "object"},
                                   {"properties", {{"title", prop("string", "Text the dialog's title contains (case-insensitive).")}}},
                                   {"additionalProperties", false},
                                   {"description", "Which dialog; default the topmost."}}}},
                      {"button"}),
        [](const json& args, ToolContext& ctx) {
            McpService& service = ctx.service();
            end_web_press(service, ctx); // pressing a button ends a press held on a page, even one refused here
            const std::string button = arg_string(args, "button");
            const std::string title  = args.contains("dialog") ? args["dialog"].value("title", "") : std::string();
            // The dialog to answer, found again on the UI thread for each step.
            auto find_dialog = [&]() -> wxDialog* {
                const auto& stack = service.modals().stack();
                if (stack.empty())
                    throw ToolFailure(no_modal_dialog(service));
                wxDialog* dialog = stack.back();
                if (!title.empty()) {
                    auto contains = [&](wxDialog* d) {
                        std::string t = into_u8(d->GetTitle()), want = title;
                        std::transform(t.begin(), t.end(), t.begin(), ::tolower);
                        std::transform(want.begin(), want.end(), want.begin(), ::tolower);
                        return t.find(want) != std::string::npos;
                    };
                    if (!contains(dialog)) {
                        for (wxDialog* d : stack)
                            if (contains(d))
                                throw ToolFailure("\"" + into_u8(d->GetTitle()) + "\" is under \"" + into_u8(dialog->GetTitle()) +
                                                  "\"; answer the topmost dialog first");
                        throw ToolFailure("No open dialog's title contains \"" + title + "\"", {{"dialogs", service.modals().snapshot()}});
                    }
                }
                if (is_native_dialog(dialog) && message_box_buttons(dialog).empty())
                    throw ToolFailure("This native dialog (a file or folder picker, or a message box on macOS) can't be pressed; "
                                      "register auto_answer_dialogs before the action that opens it",
                                      {{"dialog", describe_dialog(dialog)}});
                return dialog;
            };
            WebTarget       page;
            const wxDialog* web_dialog = nullptr;
            std::string     dialog_title;
            std::uint64_t   serial = 0;
            bool            native = false;
            json out = ctx.run_ui([&]() -> json {
                wxDialog* dialog = find_dialog();
                dialog_title     = into_u8(dialog->GetTitle());
                serial           = service.modals().serial(dialog);
                int id           = dialog_button_by_label(dialog, button);
                if (id == wxID_NONE) {
                    if (wxWebView* web = web_view_in(dialog)) {
                        page.web   = web_handle(web);
                        web_dialog = dialog;
                        ctx.works_in_dialog(serial);
                        return json();
                    }
                    id = resolve_dialog_button(dialog, button);
                }
                // A message box can only end through one of its buttons.
                native = is_native_dialog(dialog);
                if (id == wxID_NONE || (native && !has_dialog_button(dialog, id)))
                    throw ToolFailure("The dialog has no button \"" + button + "\"", {{"dialog", describe_dialog(dialog)}});
                if (const std::optional<DialogPress> refused = refused_press(dialog, id))
                    throw ToolFailure(refused_button(button, *refused), {{"dialog", describe_dialog(dialog)}});
                const DialogPress press = press_dialog_button(dialog, id);
                if (press == DialogPress::Failed)
                    throw ToolFailure("The message box is not shown, so its button could not be pressed; call list_widgets again",
                                      {{"dialog", describe_dialog(dialog)}});
                return {{"dialog", dialog_title}, {"button", button}, {"id", id}, {"pressed", press == DialogPress::Pressed}};
            });
            if (!out.is_null()) {
                settle(ctx);
                // A message box closes once its own loop runs again, which a press always makes it do;
                // that loop may run after the UI thread has served other calls.
                auto open = [&] { return ctx.observe_ui([&] { return json(service.modals().is_open(serial)); }).get<bool>(); };
                bool still_open = open();
                for (const auto end = std::chrono::steady_clock::now() + 1s; native && still_open && std::chrono::steady_clock::now() < end;) {
                    std::this_thread::sleep_for(20ms);
                    still_open = open();
                }
                if (!still_open)
                    out.update(closed_reply(service));
                return ToolResult::ok(out);
            }
            // A web dialog: its page's button by that label, which may take up to 3 s to show. Every ask
            // ends with those 3 s, so a press the page only gets later is refused there.
            json       page_buttons = json::array();
            const int  standard     = standard_dialog_button(button);
            const bool ends_dialog  = standard == wxID_CANCEL || standard == wxID_CLOSE;
            const auto window       = std::chrono::seconds(3);
            const auto end          = std::chrono::steady_clock::now() + window;
            for (;;) {
                PageBefore before;
                PageCalls  calls;
                before.watch = watch_id();
                const json request{{"op", "press"}, {"label", button}, {"watch", before.watch}, {"expires", page_expiry(end)}};
                ctx.run_ui([&]() -> json {
                    wxDialog*  dialog = find_dialog();
                    wxWebView* web    = web_view_of(page.web);
                    if (dialog != web_dialog || web == nullptr)
                        throw ToolFailure("The dialog changed while its page was being asked; call list_widgets again");
                    before.modal = service.modals().serial(dialog);
                    before.stack = service.modals().stack().size();
                    before.url   = into_u8(web->GetCurrentURL());
                    calls.start(web, request);
                    return json();
                });
                calls.wait(ctx, end);
                const PageReply r = calls.reply(0);
                if (r.state == PageReply::State::Destroyed)
                {
                    json gone{{"dialog", dialog_title}, {"button", button}, {"web", true}};
                    gone.update(closed_reply(service));
                    return ToolResult::ok(gone);
                }
                // A page that can't be asked (busy, or its agent failing) leaves cancel and close to the
                // dialog itself, and so does a page the last ask reached past the window; any other
                // answer needs the page.
                // A page that answered before has said which buttons it has, which the refusal lists.
                if (r.state != PageReply::State::Answered || r.value.contains("error")) {
                    if (ends_dialog || !page_buttons.empty())
                        break;
                    check_page_reply(r, window, true, ctx);
                }
                if (r.value.contains("pressed") && r.value["pressed"].is_string()) {
                    out = {{"dialog", dialog_title}, {"button", r.value["pressed"]}, {"pressed", true}, {"web", true}};
                    out.update(observe_page(service, ctx, page, before, r.value));
                    return ToolResult::ok(out);
                }
                page_buttons = r.value.value("buttons", json::array());
                // Cancel and close always end the dialog, so a loaded page without them is not waited on.
                const bool loaded = r.value.value("ready", std::string()) == "complete";
                if (std::chrono::steady_clock::now() + 200ms >= end || ctx.cancelled() || (loaded && ends_dialog))
                    break;
                std::this_thread::sleep_for(200ms);
            }
            // No page button has that label: cancel and close end the dialog, any other answer needs a wx button.
            out = ctx.run_ui([&]() -> json {
                wxDialog* dialog = find_dialog();
                const int id     = standard;
                if (const std::optional<DialogPress> refused = id != wxID_NONE ? refused_press(dialog, id) : std::nullopt)
                    throw ToolFailure(refused_button(button, *refused), {{"dialog", describe_dialog(dialog)}});
                if (id != wxID_NONE && (ends_dialog || has_dialog_button(dialog, id)))
                    return {{"dialog", dialog_title},
                            {"button", button},
                            {"id", id},
                            {"pressed", press_dialog_button(dialog, id) == DialogPress::Pressed}};
                throw ToolFailure("The dialog's page has no button \"" + button + "\"",
                                  {{"dialog", describe_dialog(dialog)}, {"page_buttons", page_buttons}});
            });
            settle(ctx);
            const bool gone = ctx.observe_ui([&]() -> json {
                                     const auto& stack = service.modals().stack();
                                     return std::find(stack.begin(), stack.end(), web_dialog) == stack.end();
                                 }).get<bool>();
            if (gone)
                out.update(closed_reply(service));
            return ToolResult::ok(out);
        }));

    {
        NativeCommand def = recover_input_tool(
            "core", "auto_answer_dialogs", L("Answer dialogs automatically"),
            "Answer modal dialogs automatically when their title or message matches a rule's regex (case-insensitive). "
            "get_app_state.auto_answer_rules lists the rules in force. Called from a replay or script, the rules last until it "
            "ends; otherwise they replace the ones this session set before, and an empty list clears them. A native dialog "
            "returns the rule's button at once and never shows. A file or folder picker takes button ok with an absolute path "
            "and returns it without showing (on Linux such a rule cancels it); without a path a picker can only be cancelled, "
            "and a multi-select picker takes no path. A wx dialog has the button pressed as answer_dialog does, so its "
            "handler runs. A matching dialog already open is answered too "
            "(answered_open_dialog), so a prompt that shows around the same time, such as the restore prompt after startup, "
            "is caught either way. A web dialog is matched by its title: a button that names one of its shown wx buttons (by "
            "label, standard name or id) is pressed as for a wx dialog, any other on its page once it shows there, for up to "
            "10 s after the page loads; past that, cancel and close end the dialog and any other button gives up.",
            object_schema({{"rules", array_prop({{"type", "object"},
                                                 {"properties", {{"match", prop("string", "Regex searched in \"title\\nmessage\", in \"title\\ntext\" (the dialog's own static text, one line each, without its controls), and in the title alone for a dialog with no text (a web dialog), so ^Title$ matches it.")},
                                                                 {"button", prop("string", "yes | no | ok | cancel | close | apply | label | id")},
                                                                 {"path", prop("string", "Absolute path a matching file or folder picker returns, with button ok. A picker has no title until it shows, so the rule matches its prompt and must not anchor on a title with ^. The rule answers pickers only: an open picker needs an existing file, a folder picker an existing folder, and a save picker a new file. A picker the path can't serve is cancelled and the reason logged.")},
                                                                 {"overwrite", prop("boolean", "A save picker may return an existing file at path (default false).")}}},
                                                 {"required", {"match", "button"}},
                                                 {"additionalProperties", false}},
                                                "Rules, first match wins.")}},
                          {"rules"}),
            [](const json& args, ToolContext& ctx) {
                McpService& service = ctx.service();
                std::vector<AutoAnswerRule> rules;
                for (const json& r : args["rules"])
                    rules.push_back(make_rule(r["match"].get<std::string>(), r["button"].get<std::string>(), r.value("path", std::string()),
                                              r.value("overwrite", false)));
                const std::vector<AutoAnswerRule> registered = rules;
                const AutoAnswerSet set = service.set_auto_answer(std::move(rules), service.jobs().ancestry(ctx.job_id()));
                json out{{"scope", set.scope}, {"rules", service.modals().rules_json()}};
                // Paused rules leave the dialog that is open now to the person, as they leave the ones that open later.
                json answered = set.paused ? json() : ctx.run_ui([&]() -> json {
                    wxDialog* top = service.modals().top();
                    // A message box is pressed as answer_dialog presses it; a picker can't be.
                    const bool native = top != nullptr && is_native_dialog(top);
                    if (top == nullptr || (native && message_box_buttons(top).empty()))
                        return json();
                    std::string text;
                    const json  info = describe_dialog(top, &text);
                    for (const AutoAnswerRule& rule : registered)
                        // A path rule answers a picker only as it opens.
                        if (rule.path.empty() && rule_matches(rule, info["title"].get<std::string>(), info["message"].get<std::string>(), text)) {
                            // The rule may have been paused (a Run Script work ended) or replaced since it was registered.
                            if (!service.modals().rule_in_force(set.layer, rule.pattern, rule.button))
                                return json();
                            // A web dialog is answered on its page, as ModalTracker::Enter() does.
                            if (info.value("web", false) && native_dialog_button(top, rule.button) == wxID_NONE) {
                                const int standard = standard_dialog_button(rule.button);
                                start_web_answer(service.modals(), top, set.layer, rule.pattern, rule.button,
                                                 standard == wxID_CANCEL || standard == wxID_CLOSE ? standard : int(wxID_NONE));
                                return {{"dialog", info["title"]}, {"button", rule.button}, {"web", "answering on its page"}};
                            }
                            const int id = resolve_dialog_button(top, rule.button);
                            if (id == wxID_NONE || (native && !has_dialog_button(top, id)))
                                continue;
                            // The first rule that names a button decides; a disabled or hidden one leaves the
                            // dialog open.
                            if (const std::optional<DialogPress> refused = refused_press(top, id))
                                return {{"dialog", info["title"]}, {"button", rule.button}, {*refused == DialogPress::Hidden ? "hidden" : "disabled", true}};
                            // As before any other press, the presses mouse_gesture downs left on pages end first.
                            service.modals().before_press();
                            if (press_dialog_button(top, id) != DialogPress::Failed) {
                                service.modals().count_use(set.layer, rule.pattern, rule.button);
                                return {{"dialog", info["title"]}, {"button", rule.button}};
                            }
                        }
                    return json();
                });
                if (!answered.is_null()) {
                    out["answered_open_dialog"] = std::move(answered);
                    settle(ctx);
                    out["rules"] = service.modals().rules_json();
                }
                return ToolResult::ok(std::move(out));
            });
        def.idempotent = true;
        catalog.add(def);
    }

    NativeCommand eval = recover_input_tool(
        "ui", "run_javascript", L("Run JavaScript in a web view"),
        "Evaluate JavaScript in one of OrcaSlicer's web views (Home, the device page, dialogs, plugin windows). Prefer "
        "list_widgets and the input tools (click, set_widget_value, press_keys, mouse_gesture), which see and drive pages as they do widgets. The code runs in the target's page: "
        "a web view's (kind webview in list_widgets), or a page element's, with the element as `el`; without a target, in "
        "the topmost dialog's web view, else in the only shown one. While a modal dialog is open, only its pages run "
        "code. Reply: result, the value of the last expression (JSON values come back parsed), and url; closed: true when "
        "the code closed its dialog, navigated: true when it changed the page, dialogs when it opened another. Messages "
        "the page posts to OrcaSlicer reach it once the code has returned. It runs through eval, which a page whose "
        "Content-Security-Policy forbids it may refuse (list_widgets and the input tools still work there).",
        object_schema({{"code", prop("string", "JavaScript to run.")},
                       {"target", kTargetSchema},
                       {"timeout_s", {{"type", "integer"}, {"minimum", 1}, {"maximum", 120}, {"description", "Seconds the code gets (default 10)."}}}},
                      {"code"}),
        [](const json& args, ToolContext& ctx) {
            McpService& service = ctx.service();
            const std::string code     = arg_string(args, "code");
            const auto        timeout  = std::chrono::seconds(std::clamp(arg_int(args, "timeout_s", 10), 1, 120));
            const bool        element  = args.contains("target") && is_web_target(args["target"]);
            WebTarget         page     = element ? resolve_web(service, ctx, args["target"], timeout) : WebTarget();
            const auto        deadline = std::chrono::steady_clock::now() + timeout;
            json              request{{"op", "eval"}, {"script", code}, {"expires", page_expiry(deadline)}};
            if (element) {
                request["doc"] = page.doc;
                request["k"]   = page.key;
            }
            json       locator;
            PageBefore before;
            PageCalls  calls;
            ctx.run_ui([&]() -> json {
                wxWebView* web = nullptr;
                if (element)
                    web = web_view_of(page.web);
                else if (args.contains("target")) {
                    UiTarget t = service.inspector().resolve(args["target"]);
                    locator    = service.inspector().locator_for(t);
                    web        = dynamic_cast<wxWebView*>(target_window(t));
                    if (web == nullptr)
                        throw ToolFailure("The target is not a web view (list_widgets lists them as kind webview)");
                } else
                    web = default_webview(service);
                if (web == nullptr)
                    throw ToolFailure("The page's web view is gone; call list_widgets again");
                guard_page(service, ctx, web);
                page.web     = web_handle(web);
                before.modal = in_top_dialog(service, web) ? service.modals().serial(service.modals().top()) : 0;
                before.stack = service.modals().stack().size();
                before.url   = into_u8(web->GetCurrentURL());
                calls.start(web, request);
                return json();
            });
            if (element) {
                // A ref means nothing in another session: the element by the label and kind it was listed with.
                json recorded = args;
                if (!is_web_locator(args["target"])) {
                    recorded["target"] = {{"web", page.label}, {"kind", page.kind}};
                    if (!page.within.empty())
                        recorded["target"]["within"] = page.within;
                }
                ctx.set_journal_args(std::move(recorded));
            } else
                journal_with_locator(ctx, args, locator);
            calls.wait(ctx, deadline);
            const PageReply r = calls.reply(0);
            json            out{{"result", json()}, {"url", redact_url(before.url)}};
            if (r.state == PageReply::State::Destroyed) {
                out["closed"] = true;
                return ToolResult::ok(out);
            }
            if (r.state == PageReply::State::Answered && r.value.value("code", std::string()) == "error")
                throw ToolFailure("The JavaScript failed: " + r.value.value("error", std::string()));
            check_page_reply(r, timeout, true, ctx);
            const std::string text  = r.value.value("value", std::string());
            json              value = json::parse(text, nullptr, false);
            out["result"]           = value.is_discarded() ? json(text) : value;
            // What followed, the page's new url with navigated, overrides the one from before.
            out.update(observe_page(service, ctx, page, before, r.value));
            return ToolResult::ok(out);
        });
    // Its script runs with the page's own reach: the device page and Home talk to printers and the cloud.
    eval.open_world = true;
    catalog.add(eval);
}

}}} // namespace Slic3r::GUI::Mcp
