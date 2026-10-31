#pragma once

#include "UiRefs.hpp"

#include <nlohmann/json.hpp>

#include <wx/gdicmn.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

class wxDialog;
class wxWebView;
class wxWindow;

namespace Slic3r { namespace GUI { namespace Mcp {

class ModalTracker;
class ToolContext;

// Web pages as the GUI tools see them. Every wxWebView of OrcaSlicer - its dialogs and tabs, sign-in
// pages, plugin dialogs, windows, pages and dock panels - is read and driven through the page agent
// (resources/web/mcp/page_agent.js), which a call installs in the page when it is not there yet. No
// host loads it, so it reaches every web view on every port; scripts go through
// WebView::RunScriptAsync(), whose completion holds no pointer to the web view, so a page may close
// its dialog at any time. Plugin configuration UIs, in sandboxed frames the page cannot read, run
// their own copy through the relay in resources/web/dialog/js/plugin-config-ui.js.

// A web view as the job and request threads hold it: a number the UI thread maps back to the web
// view while it lives, never reused. A wxWeakRef can't serve there: making or dropping one edits the
// window's list of refs, which the UI thread edits too.
using WebHandle = std::uint64_t;
// UI thread: `web`'s handle (0 for null), the same for as long as it lives.
WebHandle web_handle(wxWebView* web);
// UI thread: the web view of `handle`, or null once it is destroyed.
wxWebView* web_view_of(WebHandle handle);

// One page call's outcome.
struct PageReply
{
    enum class State { NoAnswer, Answered, Failed, Destroyed };
    State          state = State::NoAnswer;
    nlohmann::json value;  // Answered: the agent's reply
    std::string    error;  // Failed: why the script did not run
};

// Requests to the agents of several web views at once. start() runs on the UI thread, wait() on the
// job thread; a web view destroyed meanwhile ends its wait at once as Destroyed. A page whose agent
// is missing or of another version is sent the agent with the request, on the UI thread, without
// another wait.
class PageCalls
{
public:
    PageCalls();
    ~PageCalls();
    PageCalls(const PageCalls&)            = delete;
    PageCalls& operator=(const PageCalls&) = delete;

    // UI thread: sends `request` to `web`'s agent; returns its slot.
    std::size_t start(wxWebView* web, const nlohmann::json& request);
    // Job thread: until every slot has its reply, `deadline` passes or the call is cancelled.
    void      wait(ToolContext& ctx, std::chrono::steady_clock::time_point deadline);
    PageReply reply(std::size_t slot) const;
    std::size_t size() const;

    // The replies and the destruction watchers, shared with the calls' completions (WebPages.cpp).
    struct Shared;

private:
    std::shared_ptr<Shared> m_shared;
};

// The request deadline the agent refuses to start after, in the page's clock (epoch milliseconds):
// 100 ms before the job thread stops waiting at `wait_end`, so a request the tool gave up on never
// starts afterwards.
long long page_expiry(std::chrono::steady_clock::time_point wait_end);

// The page agent's source, read from resources once, and its version. Throws ToolFailure when the
// file is missing.
const std::string& page_agent_source();
const std::string& page_agent_version();
// The script that asks `request` of the agent: when `install`, it installs the agent first.
std::string page_script(const nlohmann::json& request, bool install);

// A page snapshot (the agent's reply) as ui_snapshot lines at `depth`, in the window-client frame
// widgets use: CSS pixels times `scale` (logical pixels per CSS pixel), offset by `origin`, the web
// view's place in its window. `key` is the element the line stands for (null for none).
struct PageNode
{
    UiNode         node;
    nlohmann::json key;
};
std::vector<PageNode> page_nodes(const nlohmann::json& page, int depth, const wxPoint& origin, double scale);
// Logical pixels per CSS pixel of `web` for a page with this device pixel ratio.
double page_scale(wxWebView* web, double device_pixel_ratio);

// {css} or {web} names a page element.
bool is_web_locator(const nlohmann::json& target);

// `url` as a reply shows a page's address: the user and password of its authority, and the value of each
// query or fragment parameter one of whose name's words names a credential (token, key, secret, password,
// auth, session, signature, code and the like: orca-token, access_token, apiKey, X-Amz-Signature) read
// REDACTED. A page may keep a secret in its URL, as a plugin's page does its bridge token, and what a
// reply says lands in the agent's transcript. The page's own URLs stay as they are for telling a
// navigation, which compares them.
std::string redact_url(const std::string& url);

// The index the journal records for the element `key` of the page `web` among the matches of every
// candidate page (`webs`, whose `find` replies are `replies`), as a replay searches them: none when it
// is the only match, when it is no longer among them (the action took it away), and when a page or a
// configuration frame was not heard from, since it may hold more matches.
std::optional<int> journal_index(const std::vector<WebHandle>& webs, const std::vector<PageReply>& replies, WebHandle web,
                                 const nlohmann::json& key);

// What a ui_key chord is to the agent: {key: {key, code, keyCode, charCode, text?}, mods: {ctrl,
// shift, alt, meta}}, with Ctrl/Cmd the Command key on macOS and RawCtrl its Control key. Throws
// ToolFailure for a chord KeyChord::parse() refuses.
nlohmann::json web_key(const std::string& chord);

// The first web view inside `window` (shown ones only when `shown`), or null.
wxWebView* web_view_in(wxWindow* window, bool shown = true);

// An agent that runs its own synthetic pointer: in the page document `doc` of the web view `web`, the
// page's own (`frame` empty) or one of its configuration frames' ("<frame key>/<frame document>", as
// in the frame's element keys). Every client's gestures share each agent's pointer, as they share
// the widgets' one.
struct PressAgent
{
    WebHandle   web = 0;
    std::string doc, frame;
    bool        operator==(const PressAgent& o) const { return web == o.web && doc == o.doc && frame == o.frame; }
};
// The agent of the element `key` (a page's number, or a frame's "<frame key>/<frame document>/<key>")
// of the page document `doc`.
PressAgent press_agent(WebHandle web, const std::string& doc, const nlohmann::json& key);

// The presses ui_mouse downs left on pages, each on record from before its down is sent until its
// agent confirms a release, so the records cover every press a page may hold; one already released
// only answers its release at once. Each press has its own id, so a release confirmed for one never
// takes a later press of the same agent off record. A down not yet sent is left out of what an up
// ends and what is released, since either could overtake it on the page; once sent, a page runs it
// before any request sent after it. Any thread.
class WebPresses
{
public:
    struct Press
    {
        PressAgent    agent;
        std::uint64_t id = 0;
        std::string   owner; // as UiInspector's held press: "" for a client, else the script's job
    };
    // A down about to be sent to `agent` for `owner`: its id. The agent's earlier presses stay on
    // record, as the down releases them only if the page takes it.
    std::uint64_t hold(const PressAgent& agent, const std::string& owner);
    // The down of press `id` is sent, or was never sent and won't be.
    void sent(std::uint64_t id);
    // The sent presses on record for `agent`, which an up ends.
    std::vector<std::uint64_t> of(const PressAgent& agent) const;
    // The sent presses to release: all but `keep`'s.
    std::vector<Press> releasable(const std::optional<PressAgent>& keep) const;
    // The sent presses `owner` made (every one when null).
    std::vector<Press> owned(const std::string* owner) const;
    // Press `id` is released.
    void drop(std::uint64_t id);

private:
    struct Held
    {
        Press press;
        bool  sending = true;
    };
    mutable std::mutex m_mutex;
    std::vector<Held>  m_held;
    std::uint64_t      m_last = 0;
};

// Asks the agents of the presses `owner` made (every one when null) to release them, without
// waiting, where no call is there to wait: as a script ends, the endpoint closes, the service stops
// or an auto-answer rule presses a button. Each release names its press, which the page ends only if
// it still holds it, so it carries no deadline: a page busy now releases once free. A press its page
// confirms is dropped. A frame's release, which its page forwards, reaches the frame before any
// later request, but stays on record until a gesture's release (which waits for the frame) confirms
// it. UI thread only.
void release_web_presses(const std::shared_ptr<WebPresses>& presses, const std::string* owner);

// Answers a web dialog for an auto-answer rule: every 250 ms while `dialog` is the topmost modal and
// the rule is in force, its page is asked to press `label` (press_dialog_button() is not used: the
// page's button runs the page's own handler). The first time the button shows enabled it is clicked
// and the answer ends. The 10 s limit counts only time a complete document is shown and restarts
// when the document changes, so a dialog's bootstrap page replaced by its content is waited out;
// script errors, navigation and pending replies never end it. Past the limit, `fallback` (cancel or
// close, else wxID_NONE) ends the dialog natively, otherwise the answer gives up. UI thread only;
// `tracker` outlives every answer.
void start_web_answer(ModalTracker& tracker, wxDialog* dialog, int layer, const std::string& pattern, const std::string& label,
                      int fallback);

}}} // namespace Slic3r::GUI::Mcp
