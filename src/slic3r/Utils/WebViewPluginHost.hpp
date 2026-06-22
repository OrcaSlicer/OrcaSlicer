// Phase 3.3 — WebView plugin host.
//
// Hosts a single wxWebView per plugin and bridges its JavaScript world to
// the engine's `orca::*` services. The JS side talks to C++ exclusively
// via a `window.orca.*` API injected at page load — every method posts a
// JSON message {action, args, request_id} which C++ dispatches by action,
// runs against the engine, and replies to via
// `window.__orca_resolve(request_id, result, error)`.
//
// Lives in slic3r/Utils/ (GUI side); this file pulls in wxWidgets and is
// NOT linkable from the engine. Plugin lifecycle (load / unload via
// PluginManager) is wx-free; the WebViewPluginHost gets constructed by
// GUI code after PluginManager reports a plugin with a webview entry.
#pragma once

#include <wx/panel.h>
#include <wx/string.h>

#include <cstdint>
#include <string>
#include <unordered_map>

class wxWebView;
class wxWebViewEvent;

namespace orca { class Session; }

namespace Slic3r { namespace GUI {

class WebViewPluginHost : public wxPanel {
public:
    /// `permissions` is the ORCA_PERM_* bitfield from the plugin manifest.
    /// `session` may be null for fixture/test hosts that don't need engine
    /// services; capability-gated actions return permission-denied when
    /// the bit is missing OR when session is null.
    WebViewPluginHost(wxWindow*     parent,
                      std::string   plugin_id,
                      std::uint64_t permissions,
                      orca::Session* session,
                      const wxString& url);
    ~WebViewPluginHost() override;

    WebViewPluginHost(const WebViewPluginHost&)            = delete;
    WebViewPluginHost& operator=(const WebViewPluginHost&) = delete;

    const std::string& plugin_id() const noexcept { return plugin_id_; }
    wxWebView*         webview()   const noexcept { return webview_; }

private:
    void OnScriptMessage(wxWebViewEvent& evt);
    void Reply(int request_id, const wxString& json_or_null_payload);
    void ReplyError(int request_id, const wxString& message);

    // events.on bridge — subscribe the engine event bus to a JS callback.
    // Returns the JS-facing subscription id (0 on failure / unsupported
    // kind). Captures (this, sub_id) into the engine callback; CallAfter
    // marshals the dispatch onto the UI thread for RunScriptAsync.
    std::uint64_t SubscribeEvent(const std::string& kind_name,
                                 std::uint32_t      js_sub_id);

    std::string    plugin_id_;
    std::uint64_t  permissions_ = 0;
    orca::Session* session_     = nullptr;
    wxWebView*     webview_     = nullptr;

    // Active engine subscriptions, keyed by JS subscription id, mapped to
    // the engine subscription id. The destructor unsubscribes all of
    // them through the engine so no callbacks fire after destruction.
    std::unordered_map<std::uint32_t, std::uint64_t> event_subscriptions_;
};

}} // namespace Slic3r::GUI
