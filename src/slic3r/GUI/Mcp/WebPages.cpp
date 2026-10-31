#include "WebPages.hpp"

#include "McpTypes.hpp"
#include "ModalTracker.hpp"
#include "UiRefs.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/KeyChord.hpp"
#include "slic3r/GUI/Widgets/WebView.hpp"

#include "libslic3r/Utils.hpp"

#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>

#include <wx/app.h>
#include <wx/defs.h>
#include <wx/dialog.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/timer.h>
#include <wx/toplevel.h>
#include <wx/weakref.h>
#include <wx/webview.h>
#include <wx/window.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <ios>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;
using namespace std::chrono_literals;

namespace {

// Sends `request` to `web`'s agent and hands the outcome to `done` on the UI thread; a page without
// the agent (or with another version) is sent it with the request once. UI thread only.
void call_page(wxWebView* web, const json& request, std::function<void(PageReply)> done, bool install = false)
{
    wxWeakRef<wxWebView> weak = web;
    WebView::RunScriptAsync(web, from_u8(page_script(request, install)),
                            [weak, request, install, done = std::move(done)](bool ok, const std::string& text) {
                                PageReply reply;
                                if (!ok) {
                                    reply.state = PageReply::State::Failed;
                                    reply.error = text;
                                    done(std::move(reply));
                                    return;
                                }
                                json value = json::parse(text, nullptr, false);
                                if (value.is_discarded() || !value.is_object()) {
                                    // A script that throws or does not parse comes back as no value.
                                    reply.state = PageReply::State::Failed;
                                    reply.error = install && (value.is_null() || text.empty()) ?
                                                      "The page agent could not run in this page" :
                                                      "The page's reply is not what the page agent sends: " + text.substr(0, 200);
                                    done(std::move(reply));
                                    return;
                                }
                                if (!install && value.value("code", std::string()) == "install") {
                                    if (wxWebView* again = weak.get())
                                        call_page(again, request, done, true);
                                    else
                                        done(PageReply{PageReply::State::Destroyed, json(), std::string()});
                                    return;
                                }
                                reply.state = PageReply::State::Answered;
                                reply.value = std::move(value);
                                done(std::move(reply));
                            });
}

} // namespace

struct PageCalls::Shared
{
    struct Slot
    {
        bool      done = false;
        PageReply reply;
    };
    std::mutex              mutex;
    std::condition_variable cv;
    std::vector<Slot>       slots;
    // UI thread only: the destruction watchers, deleted on the UI thread with the calls.
    std::vector<wxEvtHandler*> watches;

    void finish(std::size_t slot, PageReply reply)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (slots[slot].done)
                return;
            slots[slot].done  = true;
            slots[slot].reply = std::move(reply);
        }
        cv.notify_all();
    }
};

namespace {

// Ends a page call's wait when its web view is destroyed first. Created, used and deleted on the
// UI thread, like its weak reference to the web view.
class DestroyWatch : public wxEvtHandler
{
public:
    DestroyWatch(wxWebView* web, std::shared_ptr<PageCalls::Shared> shared, std::size_t slot)
        : m_web(web), m_shared(std::move(shared)), m_slot(slot)
    {
        web->Bind(wxEVT_DESTROY, &DestroyWatch::on_destroy, this);
    }
    ~DestroyWatch() override
    {
        if (wxWebView* web = m_web.get())
            web->Unbind(wxEVT_DESTROY, &DestroyWatch::on_destroy, this);
    }

private:
    void on_destroy(wxWindowDestroyEvent& evt)
    {
        evt.Skip();
        if (evt.GetEventObject() == m_web.get())
            m_shared->finish(m_slot, PageReply{PageReply::State::Destroyed, json(), std::string()});
    }

    wxWeakRef<wxWebView>               m_web;
    std::shared_ptr<PageCalls::Shared> m_shared;
    std::size_t                        m_slot;
};

} // namespace

namespace {

// The handles' web views. UI thread only; never destroyed, so no ref outlives wx at exit.
std::map<WebHandle, wxWeakRef<wxWebView>>& web_handles()
{
    static auto* handles = new std::map<WebHandle, wxWeakRef<wxWebView>>();
    return *handles;
}

} // namespace

WebHandle web_handle(wxWebView* web)
{
    static WebHandle last = 0;
    if (web == nullptr)
        return 0;
    std::map<WebHandle, wxWeakRef<wxWebView>>& handles = web_handles();
    for (auto it = handles.begin(); it != handles.end();) {
        if (it->second.get() == nullptr)
            it = handles.erase(it);
        else if (it->second.get() == web)
            return it->first;
        else
            ++it;
    }
    handles.emplace(++last, web);
    return last;
}

wxWebView* web_view_of(WebHandle handle)
{
    const std::map<WebHandle, wxWeakRef<wxWebView>>& handles = web_handles();
    const auto                                        it      = handles.find(handle);
    return it == handles.end() ? nullptr : it->second.get();
}

PageCalls::PageCalls() : m_shared(std::make_shared<Shared>()) {}

PageCalls::~PageCalls()
{
    std::vector<wxEvtHandler*> watches;
    {
        std::lock_guard<std::mutex> lock(m_shared->mutex);
        watches.swap(m_shared->watches);
    }
    if (!watches.empty() && wxTheApp != nullptr)
        wxTheApp->CallAfter([watches] {
            for (wxEvtHandler* w : watches)
                delete w;
        });
}

std::size_t PageCalls::start(wxWebView* web, const json& request)
{
    std::size_t slot = 0;
    {
        std::lock_guard<std::mutex> lock(m_shared->mutex);
        slot = m_shared->slots.size();
        m_shared->slots.emplace_back();
        m_shared->watches.push_back(new DestroyWatch(web, m_shared, slot));
    }
    std::shared_ptr<Shared> shared = m_shared;
    call_page(web, request, [shared, slot](PageReply reply) { shared->finish(slot, std::move(reply)); });
    return slot;
}

void PageCalls::wait(ToolContext& ctx, std::chrono::steady_clock::time_point deadline)
{
    std::unique_lock<std::mutex> lock(m_shared->mutex);
    auto all_done = [&] {
        return std::all_of(m_shared->slots.begin(), m_shared->slots.end(), [](const Shared::Slot& s) { return s.done; });
    };
    while (!all_done() && !ctx.cancelled() && std::chrono::steady_clock::now() < deadline)
        m_shared->cv.wait_until(lock, std::min(deadline, std::chrono::steady_clock::now() + 200ms));
}

PageReply PageCalls::reply(std::size_t slot) const
{
    std::lock_guard<std::mutex> lock(m_shared->mutex);
    const Shared::Slot& s = m_shared->slots.at(slot);
    return s.done ? s.reply : PageReply{};
}

std::size_t PageCalls::size() const
{
    std::lock_guard<std::mutex> lock(m_shared->mutex);
    return m_shared->slots.size();
}

long long page_expiry(std::chrono::steady_clock::time_point wait_end)
{
    const auto end = std::chrono::system_clock::now() + (wait_end - std::chrono::steady_clock::now()) - 100ms;
    return std::chrono::duration_cast<std::chrono::milliseconds>(end.time_since_epoch()).count();
}

namespace {

std::string load_page_agent()
{
    const boost::filesystem::path path = boost::filesystem::path(resources_dir()) / "web" / "mcp" / "page_agent.js";
    boost::filesystem::ifstream file(path, std::ios::binary);
    if (!file)
        throw ToolFailure("The page agent is missing from the resources: " + path.string());
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

} // namespace

const std::string& page_agent_source()
{
    static const std::string source = load_page_agent();
    return source;
}

const std::string& page_agent_version()
{
    static const std::string version = [] {
        char hex[2 * sizeof(std::size_t) + 1];
        std::snprintf(hex, sizeof(hex), "%zx", std::hash<std::string>()(page_agent_source()));
        return std::string(hex);
    }();
    return version;
}

std::string page_script(const json& request, bool install)
{
    const std::string version = json(page_agent_version()).dump();
    const std::string req     = request.dump(-1, ' ', false, json::error_handler_t::replace);
    if (!install)
        return "(function(){var a=window.__orcaMcp;if(!a||a.v!==" + version + ")return '{\"code\":\"install\"}';return JSON.stringify(a.call(" +
               req + "));})()";
    // The source runs as code, not through eval, so pages whose CSP forbids eval take it; it is also
    // passed as text for the plugin configuration frames the agent relays to.
    const std::string& source = page_agent_source();
    return "(function(){(function(V,SRC){" + source + "\n})(" + version + "," +
           json(source).dump(-1, ' ', false, json::error_handler_t::replace) + ");return JSON.stringify(window.__orcaMcp.call(" + req + "));})()";
}

std::vector<PageNode> page_nodes(const json& page, int depth, const wxPoint& origin, double scale)
{
    std::vector<PageNode> out;
    if (!page.contains("nodes") || !page["nodes"].is_array())
        return out;
    auto px = [scale](const json& n, const char* key) { return int(std::lround(n.value(key, 0.0) * scale)); };
    for (const json& n : page["nodes"]) {
        PageNode p;
        UiNode&  u = p.node;
        u.kind     = n.value("kind", std::string());
        u.cls      = n.value("cls", std::string());
        u.label    = n.value("label", std::string());
        u.value    = n.value("value", std::string());
        u.name     = n.value("name", std::string());
        u.note     = n.value("note", std::string());
        u.checked  = n.contains("checked") && n["checked"].is_number() ? n["checked"].get<int>() : -1;
        u.enabled  = n.value("enabled", true);
        u.focused  = n.value("focused", false);
        u.depth    = depth + n.value("depth", 0);
        u.x        = origin.x + px(n, "x");
        u.y        = origin.y + px(n, "y");
        u.w        = px(n, "w");
        u.h        = px(n, "h");
        if (n.contains("k"))
            p.key = n["k"];
        out.push_back(std::move(p));
    }
    return out;
}

double page_scale(wxWebView* web, double device_pixel_ratio)
{
    const double content = web != nullptr ? web->GetContentScaleFactor() : 1.0;
    return device_pixel_ratio > 0 && content > 0 ? device_pixel_ratio / content : 1.0;
}

bool is_web_locator(const json& target) { return target.is_object() && (target.contains("css") || target.contains("web")); }

namespace {

// `s` with its %XX escapes decoded, as a server reads a parameter name.
std::string percent_decoded(const std::string& s)
{
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += char(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else
            out += s[i];
    }
    return out;
}

// Whether a parameter name names a credential: one of its words, split at punctuation and where a
// capital starts a word (accessToken, SAMLResponse), is one of the credential words. The name is
// decoded first, so an escaped one (access%5Ftoken) is told as the server reads it.
bool names_credential(const std::string& raw)
{
    static const std::set<std::string> words = {"token", "secret", "password", "passwd", "pwd", "key", "apikey", "auth",
                                                "authorization", "session", "sessionid", "sid", "signature", "sig",
                                                "credential", "credentials", "code", "ticket", "jwt", "bearer", "otp", "saml"};
    const std::string name = percent_decoded(raw);
    auto              at   = [&name](std::size_t i) { return i < name.size() ? static_cast<unsigned char>(name[i]) : 0; };
    std::string       word;
    auto              is_credential = [&] { return !word.empty() && words.count(word) > 0; };
    for (std::size_t i = 0; i <= name.size(); ++i) {
        const unsigned char c = at(i);
        // A capital after a lowercase letter or digit, or the last capital of a run before a lowercase
        // letter (the R of SAMLResponse).
        const bool camel = std::isupper(c) && i > 0 &&
                           (std::islower(at(i - 1)) || std::isdigit(at(i - 1)) || (std::isupper(at(i - 1)) && std::islower(at(i + 1))));
        const bool split = c == 0 || !std::isalnum(c) || camel;
        if (split) {
            if (is_credential())
                return true;
            word.clear();
        }
        if (c != 0 && std::isalnum(c))
            word += char(std::tolower(c));
    }
    return false;
}

// `params` (a query or fragment) with the value of each credential parameter REDACTED. A fragment
// may hold a route and its own query (#/login?access_token=...), so '?' separates parameters too.
std::string redact_params(const std::string& params)
{
    std::string out;
    std::size_t start = 0;
    while (start <= params.size()) {
        const std::size_t end = std::min(params.find_first_of("&;?", start), params.size());
        const std::string part = params.substr(start, end - start);
        const std::size_t eq   = part.find('=');
        out += eq != std::string::npos && names_credential(part.substr(0, eq)) ? part.substr(0, eq + 1) + "REDACTED" : part;
        if (end < params.size())
            out += params[end];
        start = end + 1;
    }
    return out;
}

} // namespace

std::string redact_url(const std::string& url)
{
    const std::size_t hash  = url.find('#');
    const std::size_t query = url.substr(0, hash).find('?');
    const std::size_t head  = std::min(query, hash);
    std::string       out   = url.substr(0, head);
    // user:password@host, each part redacted
    if (const std::size_t scheme = out.find("://"); scheme != std::string::npos) {
        const std::size_t host = scheme + 3;
        const std::size_t path = std::min(out.find('/', host), out.size());
        const std::size_t at   = out.rfind('@', path);
        if (at != std::string::npos && at >= host)
            out.replace(host, at - host, out.substr(host, at - host).find(':') != std::string::npos ? "REDACTED:REDACTED" : "REDACTED");
    }
    if (query != std::string::npos)
        out += "?" + redact_params(url.substr(query + 1, (hash == std::string::npos ? url.size() : hash) - query - 1));
    if (hash != std::string::npos)
        out += "#" + redact_params(url.substr(hash + 1));
    return out;
}

std::optional<int> journal_index(const std::vector<WebHandle>& webs, const std::vector<PageReply>& replies, WebHandle web, const json& key)
{
    int position = -1, count = 0;
    for (std::size_t i = 0; i < replies.size() && i < webs.size(); ++i) {
        const PageReply& r = replies[i];
        if (r.state == PageReply::State::Destroyed)
            continue;
        // A page that did not answer, failed the search or could not run the agent here (which it
        // may in a replay), or one with a silent frame: what it holds is unknown.
        if (r.state != PageReply::State::Answered || r.value.contains("error") || r.value.value("unanswered", 0) > 0)
            return std::nullopt;
        for (const json& m : r.value.value("matches", json::array())) {
            if (webs[i] == web && m.value("k", json()) == key)
                position = count;
            ++count;
        }
    }
    return position >= 0 && count > 1 ? std::optional<int>(position) : std::nullopt;
}

json web_key(const std::string& chord_text)
{
    const auto chord = KeyChord::parse(chord_text);
    if (!chord || !chord->valid())
        throw ToolFailure("Cannot parse key chord \"" + chord_text + "\" (examples: Ctrl+S, Shift+Tab, Escape, Return, Delete, F5, Left)");
    json      key;
    const int code = chord->key;
    auto      named = [&](const char* name, const char* dom_code, int key_code) {
        key = {{"key", name}, {"code", dom_code}, {"keyCode", key_code}, {"charCode", 0}};
    };
    switch (code) {
    case WXK_RETURN: case WXK_NUMPAD_ENTER: named("Enter", "Enter", 13); break;
    case WXK_ESCAPE: named("Escape", "Escape", 27); break;
    case WXK_TAB: named("Tab", "Tab", 9); break;
    case WXK_BACK: named("Backspace", "Backspace", 8); break;
    case WXK_DELETE: case WXK_NUMPAD_DELETE: named("Delete", "Delete", 46); break;
    case WXK_INSERT: named("Insert", "Insert", 45); break;
    case WXK_LEFT: named("ArrowLeft", "ArrowLeft", 37); break;
    case WXK_UP: named("ArrowUp", "ArrowUp", 38); break;
    case WXK_RIGHT: named("ArrowRight", "ArrowRight", 39); break;
    case WXK_DOWN: named("ArrowDown", "ArrowDown", 40); break;
    case WXK_HOME: named("Home", "Home", 36); break;
    case WXK_END: named("End", "End", 35); break;
    case WXK_PAGEUP: named("PageUp", "PageUp", 33); break;
    case WXK_PAGEDOWN: named("PageDown", "PageDown", 34); break;
    default:
        if (code >= WXK_F1 && code <= WXK_F24) {
            const std::string f = "F" + std::to_string(code - WXK_F1 + 1);
            named(f.c_str(), f.c_str(), 112 + (code - WXK_F1));
        } else if (code >= 32 && code < 127) {
            const bool  shift = (chord->modifiers & wxMOD_SHIFT) != 0;
            char        c     = char(code);
            std::string dom_code;
            int         key_code = 0;
            if (c >= 'A' && c <= 'Z') {
                dom_code = std::string("Key") + c;
                key_code = c;
                if (!shift)
                    c = char(c - 'A' + 'a');
            } else if (c >= '0' && c <= '9') {
                dom_code = std::string("Digit") + c;
                key_code = c;
            } else if (c == ' ') {
                dom_code = "Space";
                key_code = 32;
            }
            key = {{"key", std::string(1, c)}, {"code", dom_code}, {"keyCode", key_code}, {"charCode", int(c)}, {"text", std::string(1, c)}};
        } else
            throw ToolFailure("The key in \"" + chord_text + "\" has no counterpart in a web page");
    }
#ifdef __APPLE__
    const bool command = (chord->modifiers & wxMOD_CONTROL) != 0, control = (chord->modifiers & wxMOD_RAW_CONTROL) != 0;
    json       mods{{"ctrl", control}, {"meta", command || (chord->modifiers & wxMOD_META) != 0}};
#else
    json mods{{"ctrl", (chord->modifiers & wxMOD_CONTROL) != 0}, {"meta", (chord->modifiers & wxMOD_META) != 0}};
#endif
    mods["shift"] = (chord->modifiers & wxMOD_SHIFT) != 0;
    mods["alt"]   = (chord->modifiers & wxMOD_ALT) != 0;
    return {{"key", std::move(key)}, {"mods", std::move(mods)}};
}

wxWebView* web_view_in(wxWindow* window, bool shown)
{
    if (window == nullptr || (shown && !window->IsShown()))
        return nullptr;
    if (auto* web = dynamic_cast<wxWebView*>(window))
        return web;
    for (wxWindow* child : window->GetChildren())
        if (dynamic_cast<wxTopLevelWindow*>(child) == nullptr)
            if (wxWebView* web = web_view_in(child, shown))
                return web;
    return nullptr;
}

PressAgent press_agent(WebHandle web, const std::string& doc, const json& key)
{
    const std::string k     = key.is_string() ? key.get<std::string>() : std::string();
    const auto        slash = k.rfind('/');
    return {web, doc, slash == std::string::npos ? std::string() : k.substr(0, slash)};
}

std::uint64_t WebPresses::hold(const PressAgent& agent, const std::string& owner)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_held.push_back({{agent, ++m_last, owner}, true});
    return m_last;
}

void WebPresses::sent(std::uint64_t id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (Held& h : m_held)
        if (h.press.id == id)
            h.sending = false;
}

std::vector<std::uint64_t> WebPresses::of(const PressAgent& agent) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::uint64_t>  ids;
    for (const Held& h : m_held)
        if (!h.sending && h.press.agent == agent)
            ids.push_back(h.press.id);
    return ids;
}

std::vector<WebPresses::Press> WebPresses::owned(const std::string* owner) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<Press>          out;
    for (const Held& h : m_held)
        if (!h.sending && (owner == nullptr || h.press.owner == *owner))
            out.push_back(h.press);
    return out;
}

std::vector<WebPresses::Press> WebPresses::releasable(const std::optional<PressAgent>& keep) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<Press>          out;
    for (const Held& h : m_held)
        if (!h.sending && !(keep && h.press.agent == *keep))
            out.push_back(h.press);
    return out;
}

void WebPresses::drop(std::uint64_t id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_held.erase(std::remove_if(m_held.begin(), m_held.end(), [id](const Held& h) { return h.press.id == id; }), m_held.end());
}

void release_web_presses(const std::shared_ptr<WebPresses>& presses, const std::string* owner)
{
    const std::weak_ptr<WebPresses> weak = presses;
    for (const WebPresses::Press& p : presses->owned(owner)) {
        wxWebView* web = web_view_of(p.agent.web);
        if (web == nullptr) {
            presses->drop(p.id);
            continue;
        }
        // No deadline: a page busy now runs it once free, and as it names its press it can only
        // end that one, whenever it runs.
        const json request{{"op", "release"}, {"doc", p.agent.doc}, {"frame", p.agent.frame}, {"press", p.id}};
        call_page(web, request, [weak, id = p.id](PageReply reply) {
            const std::shared_ptr<WebPresses> held = weak.lock();
            if (held && (reply.state == PageReply::State::Destroyed ||
                         (reply.state == PageReply::State::Answered && !reply.value.contains("error") && !reply.value.contains("relay"))))
                held->drop(id);
        });
    }
}

namespace {

class WebAnswer : public wxTimer
{
public:
    WebAnswer(ModalTracker& tracker, wxDialog* dialog, int layer, std::string pattern, std::string label, int fallback)
        : m_tracker(tracker), m_dialog(dialog), m_layer(layer), m_pattern(std::move(pattern)), m_label(std::move(label)), m_fallback(fallback)
    {}

    void Notify() override
    {
        wxDialog* dialog = m_dialog.get();
        if (dialog == nullptr || m_tracker.top() != dialog || !m_tracker.rule_in_force(m_layer, m_pattern, m_label)) {
            finish();
            return;
        }
        if (m_pending)
            return;
        wxWebView* web = web_view_in(dialog);
        // A dialog whose web view stays hidden has no page to press on: it is answered as one whose
        // page lacks the button, after 2 s.
        if (web == nullptr) {
            if (++m_unshown >= 8)
                give_up("shows no page");
            return;
        }
        m_unshown = 0;
        if (web->IsBusy()) {
            m_complete_since.reset();
            return;
        }
        m_pending = true;
        wxWeakRef<wxTimer> self(this);
        // Each try may press, so the presses held end first; sent before it, they run before it.
        m_tracker.before_press();
        const json request{{"op", "press"}, {"label", m_label}, {"expires", page_expiry(std::chrono::steady_clock::now() + 5s)}};
        call_page(web, request, [self](PageReply reply) {
            if (auto* answer = static_cast<WebAnswer*>(self.get()))
                answer->on_reply(reply);
        });
    }

private:
    void on_reply(const PageReply& reply)
    {
        m_pending = false;
        if (m_done || reply.state != PageReply::State::Answered || reply.value.contains("error"))
            return;
        if (reply.value.contains("pressed") && reply.value["pressed"].is_string()) {
            m_tracker.count_use(m_layer, m_pattern, m_label);
            BOOST_LOG_TRIVIAL(info) << "MCP: auto-answered web dialog \"" << title() << "\" with its page's \"" << m_label << "\" (rule \""
                                    << m_pattern << "\")";
            finish();
            return;
        }
        // The wait counts from the moment the current document is complete.
        const std::string doc   = reply.value.value("doc", std::string());
        const bool        ready = reply.value.value("ready", std::string()) == "complete";
        const auto        now   = std::chrono::steady_clock::now();
        if (doc != m_doc || !ready) {
            m_doc = doc;
            if (ready)
                m_complete_since = now;
            else
                m_complete_since.reset();
            return;
        }
        if (!m_complete_since)
            m_complete_since = now;
        if (now - *m_complete_since >= 10s)
            give_up("has no such button");
    }

    // No page button to press: cancel and close end the dialog natively, any other answer gives up.
    void give_up(const char* why)
    {
        wxDialog* dialog = m_dialog.get();
        if (m_fallback != wxID_NONE && dialog != nullptr && m_tracker.top() == dialog) {
            m_tracker.count_use(m_layer, m_pattern, m_label);
            BOOST_LOG_TRIVIAL(info) << "MCP: auto-answer rule \"" << m_pattern << "\": the page of \"" << title() << "\" " << why
                                    << " for \"" << m_label << "\"; ended the dialog with " << m_label;
            press_dialog_button(dialog, m_fallback);
        } else
            BOOST_LOG_TRIVIAL(info) << "MCP: auto-answer rule \"" << m_pattern << "\": the page of \"" << title() << "\" " << why
                                    << " for \"" << m_label << "\"; gave up";
        finish();
    }

    std::string title() const { return m_dialog.get() != nullptr ? into_u8(m_dialog->GetTitle()) : std::string(); }

    void finish()
    {
        if (m_done)
            return;
        m_done = true;
        Stop();
        // Not from inside the timer's own callback, which wx may still touch afterwards.
        if (wxTheApp != nullptr)
            wxTheApp->CallAfter([this] { delete this; });
    }

    ModalTracker&                                        m_tracker;
    wxWeakRef<wxDialog>                                  m_dialog;
    int                                                  m_layer;
    std::string                                          m_pattern, m_label;
    int                                                  m_fallback;
    bool                                                 m_pending = false, m_done = false;
    int                                                  m_unshown = 0; // ticks in a row without a shown web view
    std::string                                          m_doc;
    std::optional<std::chrono::steady_clock::time_point> m_complete_since;
};

} // namespace

void start_web_answer(ModalTracker& tracker, wxDialog* dialog, int layer, const std::string& pattern, const std::string& label, int fallback)
{
    auto* answer = new WebAnswer(tracker, dialog, layer, pattern, label, fallback);
    answer->Start(250);
}

}}} // namespace Slic3r::GUI::Mcp
