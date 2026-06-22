// Phase 3.3 — WebView plugin host implementation.

#include "WebViewPluginHost.hpp"

#include "slic3r/GUI/Widgets/WebView.hpp"

#include "orca/Session.hpp"
#include "orca/Presets.hpp"
#include "orca/Events.hpp"
#include "orca/EventTypes.hpp"
#include "orca/c_api.h"
#include "orca/plugin_api.h"
#include "orca/PlaceholderProvider.hpp"
#include "libslic3r/PlaceholderParser.hpp"

#include "nlohmann/json.hpp"

#include <wx/app.h>
#include <wx/sizer.h>
#include <wx/webview.h>
#include <wx/weakref.h>

#include <cstdio>
#include <string>

namespace Slic3r { namespace GUI {

namespace {

// JS shim injected at every page load. Defines window.orca.* methods
// that wrap postMessage into request_id-tracked promises, resolved by
// C++ calling window.__orca_resolve.
//
// The shim is idempotent — re-injection across navigations is safe.
constexpr const char* kOrcaWindowShim = R"JS(
(function () {
    if (window.orca && window.__orca_resolve) return;
    let __next_id = 1;
    const __pending = new Map();

    window.__orca_resolve = function (id, result, error) {
        const entry = __pending.get(id);
        if (!entry) return;
        __pending.delete(id);
        if (error) entry.reject(new Error(error));
        else       entry.resolve(result);
    };

    function call(action, args) {
        return new Promise((resolve, reject) => {
            const id = __next_id++;
            __pending.set(id, { resolve, reject });
            const msg = JSON.stringify({ action, args, request_id: id });
            window.orca_native.postMessage(msg);
        });
    }

    // Engine-event subscriptions are dispatched back through
    // window.__orca_event_dispatch(js_sub_id, payload). Keyed by the JS
    // subscription id we hand back from events.on.
    const __event_handlers = new Map();
    let __next_event_id = 1;

    window.__orca_event_dispatch = function (js_sub_id, payload) {
        const cb = __event_handlers.get(js_sub_id);
        if (cb) try { cb(payload); } catch (_e) {}
    };

    window.orca = {
        log: (level, message) => call('log', { level, message }),
        checkPermission: (bit) => call('check_permission', { bit }),
        placeholderSetString: (name, value) =>
            call('placeholder_set_string', { name, value }),
        placeholderSetInt: (name, value) =>
            call('placeholder_set_int', { name, value }),
        placeholderSetFloat: (name, value) =>
            call('placeholder_set_float', { name, value }),
        loadProfilePack: (dir) => call('load_profile_pack', { dir }),

        presets: {
            getString: (key) => call('presets_get_string', { key }),
            setString: (key, value) => call('presets_set_string', { key, value }),
        },

        events: {
            on(kind, cb) {
                const js_sub_id = __next_event_id++;
                __event_handlers.set(js_sub_id, cb);
                // Engine-side subscription happens via the host; the
                // call resolves with the id so the caller can
                // events.off(id) later.
                call('events_subscribe', { kind, js_sub_id });
                return js_sub_id;
            },
            off(js_sub_id) {
                __event_handlers.delete(js_sub_id);
                return call('events_unsubscribe', { js_sub_id });
            },
        },
    };
})();
)JS";

// Convert ORCA_ERR_* values to a stable string error tag the JS side can
// pattern-match against. Matches the names in plugin_api.h.
const char* error_tag(int rc) {
    switch (rc) {
        case ORCA_OK:                    return "ok";
        case ORCA_ERR_INVALID_ARGUMENT:  return "invalid_argument";
        case ORCA_ERR_NOT_FOUND:         return "not_found";
        case ORCA_ERR_ALREADY_EXISTS:    return "already_exists";
        case ORCA_ERR_IO:                return "io_error";
        case ORCA_ERR_PARSE:             return "parse_error";
        case ORCA_ERR_CANCELLED:         return "cancelled";
        case ORCA_ERR_UNSUPPORTED:       return "unsupported";
        case ORCA_ERR_PERMISSION_DENIED: return "permission_denied";
        default:                         return "unknown";
    }
}

} // namespace

WebViewPluginHost::WebViewPluginHost(wxWindow*       parent,
                                     std::string     plugin_id,
                                     std::uint64_t   permissions,
                                     orca::Session*  session,
                                     const wxString& url)
    : wxPanel(parent), plugin_id_(std::move(plugin_id)),
      permissions_(permissions), session_(session)
{
    webview_ = WebView::CreateWebView(this, url);
    if (!webview_) return; // fallback FakeWebView already handled by WebView::CreateWebView

    // Pull the orca_native message channel and inject the window.orca
    // shim before the page evaluates. The handler name doubles as the
    // JS-side accessor: `window.orca_native.postMessage`.
    webview_->AddScriptMessageHandler(wxString::FromUTF8("orca_native"));
    webview_->AddUserScript(wxString::FromUTF8(kOrcaWindowShim));

    Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED,
         &WebViewPluginHost::OnScriptMessage, this, webview_->GetId());

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(webview_, 1, wxEXPAND);
    SetSizer(sizer);
}

WebViewPluginHost::~WebViewPluginHost() {
    // Drain engine-side subscriptions BEFORE the wxWebView (and `this`)
    // wind down. After this loop no engine callback can fire into our
    // RunScriptAsync — any in-flight publish has either already crossed
    // the unsubscribe lock or is no longer registered.
    if (session_) {
        auto& events = session_->events();
        for (auto& [js_sub_id, engine_sub_id] : event_subscriptions_)
            events.unsubscribe(engine_sub_id);
    }
    event_subscriptions_.clear();
}

void WebViewPluginHost::Reply(int request_id,
                              const wxString& json_or_null_payload)
{
    if (!webview_) return;
    wxString js;
    js << "window.__orca_resolve(" << request_id << ", "
       << json_or_null_payload << ", null);";
    webview_->RunScriptAsync(js);
}

void WebViewPluginHost::ReplyError(int request_id, const wxString& message)
{
    if (!webview_) return;
    // JSON-escape the message before splicing it into a string literal.
    nlohmann::json j_msg = message.utf8_string();
    wxString js;
    js << "window.__orca_resolve(" << request_id << ", null, "
       << wxString::FromUTF8(j_msg.dump()) << ");";
    webview_->RunScriptAsync(js);
}

void WebViewPluginHost::OnScriptMessage(wxWebViewEvent& evt)
{
    using nlohmann::json;

    json j;
    try {
        j = json::parse(evt.GetString().utf8_string());
    } catch (const std::exception& e) {
        std::fprintf(stderr,
            "[orca][webview][%s] malformed message: %s\n",
            plugin_id_.c_str(), e.what());
        return;
    }

    const int request_id = j.value("request_id", 0);
    const std::string action = j.value("action", std::string{});
    const json args = j.value("args", json::object());

    auto has_perm = [&](std::uint64_t bit) {
        return (permissions_ & bit) == bit;
    };

    if (action == "log") {
        const int level     = args.value("level", 1);
        const std::string m = args.value("message", std::string{});
        std::fprintf(stderr, "[orca][webview][%s][%d] %s\n",
                     plugin_id_.c_str(), level, m.c_str());
        Reply(request_id, "null");
        return;
    }

    if (action == "check_permission") {
        // Accept either number (bitfield int) or hex string ("0x1").
        std::uint64_t bit = 0;
        if (args["bit"].is_number_unsigned())
            bit = args["bit"].get<std::uint64_t>();
        else if (args["bit"].is_number_integer())
            bit = static_cast<std::uint64_t>(args["bit"].get<std::int64_t>());
        else if (args["bit"].is_string())
            bit = std::stoull(args["bit"].get<std::string>(), nullptr, 0);
        Reply(request_id, has_perm(bit) ? "true" : "false");
        return;
    }

    if (action == "placeholder_set_string" ||
        action == "placeholder_set_int"    ||
        action == "placeholder_set_float")
    {
        auto* parser = ::orca::placeholder_tls::current();
        if (!parser) {
            ReplyError(request_id, "no placeholder parser bound");
            return;
        }
        const std::string name = args.value("name", std::string{});
        if (name.empty()) {
            ReplyError(request_id, "name is required");
            return;
        }
        if (action == "placeholder_set_string") {
            parser->set(name, args.value("value", std::string{}));
        } else if (action == "placeholder_set_int") {
            parser->set(name, static_cast<int>(args.value("value", 0)));
        } else {
            parser->set(name, args.value("value", 0.0));
        }
        Reply(request_id, "null");
        return;
    }

    if (action == "events_subscribe") {
        if (!session_) {
            ReplyError(request_id, error_tag(ORCA_ERR_INVALID_ARGUMENT));
            return;
        }
        const std::string kind   = args.value("kind", std::string{});
        const std::uint32_t js   = args.value("js_sub_id", 0u);
        if (kind.empty() || js == 0) {
            ReplyError(request_id, error_tag(ORCA_ERR_INVALID_ARGUMENT));
            return;
        }
        const auto engine_id = SubscribeEvent(kind, js);
        if (engine_id == 0) {
            ReplyError(request_id, "unsupported event kind: " + kind);
            return;
        }
        event_subscriptions_.emplace(js, engine_id);
        Reply(request_id, wxString::Format("%u", js));
        return;
    }

    if (action == "events_unsubscribe") {
        const std::uint32_t js = args.value("js_sub_id", 0u);
        if (js == 0 || !session_) {
            Reply(request_id, "null");
            return;
        }
        auto it = event_subscriptions_.find(js);
        if (it != event_subscriptions_.end()) {
            session_->events().unsubscribe(it->second);
            event_subscriptions_.erase(it);
        }
        Reply(request_id, "null");
        return;
    }

    if (action == "presets_get_string") {
        if (!session_) {
            ReplyError(request_id, error_tag(ORCA_ERR_INVALID_ARGUMENT));
            return;
        }
        if (!has_perm(ORCA_PERM_SETTINGS_READ)) {
            ReplyError(request_id, error_tag(ORCA_ERR_PERMISSION_DENIED));
            return;
        }
        const std::string key = args.value("key", std::string{});
        if (key.empty()) {
            ReplyError(request_id, error_tag(ORCA_ERR_INVALID_ARGUMENT));
            return;
        }
        auto* presets = reinterpret_cast<orca_presets_t*>(&session_->presets());
        const char* out = nullptr;
        const auto rc = orca_presets_opt_string(
            presets, ORCA_SCOPE_FULL, key.c_str(), &out);
        if (rc == ORCA_OK && out) {
            nlohmann::json j = std::string{out};
            Reply(request_id, wxString::FromUTF8(j.dump()));
        } else if (rc == ORCA_ERR_NOT_FOUND) {
            Reply(request_id, "null");
        } else {
            ReplyError(request_id, error_tag(rc));
        }
        return;
    }

    if (action == "presets_set_string") {
        if (!session_) {
            ReplyError(request_id, error_tag(ORCA_ERR_INVALID_ARGUMENT));
            return;
        }
        if (!has_perm(ORCA_PERM_SETTINGS_WRITE)) {
            ReplyError(request_id, error_tag(ORCA_ERR_PERMISSION_DENIED));
            return;
        }
        const std::string key   = args.value("key",   std::string{});
        const std::string value = args.value("value", std::string{});
        if (key.empty()) {
            ReplyError(request_id, error_tag(ORCA_ERR_INVALID_ARGUMENT));
            return;
        }
        auto* presets = reinterpret_cast<orca_presets_t*>(&session_->presets());
        const auto rc = orca_presets_set_string(
            presets, ORCA_SCOPE_FULL, key.c_str(), value.c_str());
        if (rc == ORCA_OK) Reply(request_id, "null");
        else               ReplyError(request_id, error_tag(rc));
        return;
    }

    if (action == "load_profile_pack") {
        if (!has_perm(ORCA_PERM_PROFILES_INSTALL)) {
            ReplyError(request_id, error_tag(ORCA_ERR_PERMISSION_DENIED));
            return;
        }
        if (!session_) {
            ReplyError(request_id, error_tag(ORCA_ERR_INVALID_ARGUMENT));
            return;
        }
        const std::string dir = args.value("dir", std::string{});
        if (dir.empty()) {
            ReplyError(request_id, error_tag(ORCA_ERR_INVALID_ARGUMENT));
            return;
        }
        auto rc = session_->presets().load_vendor_configs_from_json(
            dir, ::orca::SubstitutionRule::Disable);
        if (!rc.ok()) ReplyError(request_id, error_tag(ORCA_ERR_IO));
        else          Reply(request_id, "null");
        return;
    }

    ReplyError(request_id, wxString::Format(
        "unknown action: %s", wxString::FromUTF8(action)));
}

namespace {

// Marshal each known engine event payload to a JSON string the JS side
// can consume directly via __orca_event_dispatch.
nlohmann::json to_json(const ::orca::SlicingProgress& e) {
    return { {"handle", e.handle}, {"progress", e.progress}, {"message", e.message} };
}
nlohmann::json to_json(const ::orca::SlicingFinished& e) {
    return { {"handle", e.handle}, {"success", e.success}, {"error", e.error} };
}
nlohmann::json to_json(const ::orca::ExportFinished& e) {
    return { {"handle", e.handle}, {"success", e.success},
             {"line_count", e.line_count}, {"error", e.error} };
}

// Dispatch a marshalled payload to the JS side on the UI thread.
// `weak_self` keeps us from running into a destroyed host; `js_sub_id`
// matches the id JS registered when it called events.on.
template <typename EventT>
auto make_dispatcher(wxWeakRef<Slic3r::GUI::WebViewPluginHost> weak_self,
                     std::uint32_t js_sub_id)
{
    return [weak_self, js_sub_id](const EventT& e) mutable {
        const auto j = to_json(e).dump();
        wxString js;
        js << "window.__orca_event_dispatch(" << js_sub_id << ", "
           << wxString::FromUTF8(j) << ");";
        wxTheApp->CallAfter([weak_self, js]() {
            auto* host = weak_self.get();
            if (!host || !host->webview()) return;
            host->webview()->RunScriptAsync(js);
        });
    };
}

} // namespace

std::uint64_t WebViewPluginHost::SubscribeEvent(const std::string& kind_name,
                                                std::uint32_t      js_sub_id)
{
    if (!session_) return 0;
    auto& events = session_->events();

    wxWeakRef<WebViewPluginHost> weak_self(this);

    if (kind_name == "SlicingProgress")
        return events.subscribe<::orca::SlicingProgress>(
            make_dispatcher<::orca::SlicingProgress>(weak_self, js_sub_id));
    if (kind_name == "SlicingFinished")
        return events.subscribe<::orca::SlicingFinished>(
            make_dispatcher<::orca::SlicingFinished>(weak_self, js_sub_id));
    if (kind_name == "ExportFinished")
        return events.subscribe<::orca::ExportFinished>(
            make_dispatcher<::orca::ExportFinished>(weak_self, js_sub_id));
    return 0;
}

}} // namespace Slic3r::GUI
