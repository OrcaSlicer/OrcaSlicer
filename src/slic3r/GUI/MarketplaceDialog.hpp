// Phase 4.4 — MarketplaceDialog.
//
// Modal dialog that hosts a wxWebView running the configured plugin marketplace
// SPA. The SPA's HTML/JS/CSS bundle is built by the sibling
// `marketplace-ui/` Vite project and deployed at
// `<resources_dir>/marketplace-ui/index.html`.
//
// The SPA talks to native code through a `window.orca.marketplace.*`
// bridge that mirrors the WebViewPluginHost bridge pattern (see
// `slic3r/Utils/WebViewPluginHost.cpp` for the canonical version):
//
//   window.orca.marketplace.list()            -> Promise<MarketplaceCatalog>
//   window.orca.marketplace.installed()       -> Promise<{plugins: [...]}>
//   window.orca.marketplace.install(id, ver)  -> Promise<{status, message?}>
//   window.orca.marketplace.update(id, ver)   -> Promise<{status, message?}>
//   window.orca.marketplace.uninstall(id)     -> Promise<{status, message?}>
//   window.orca.marketplace.open_external(u)  -> Promise<void>
//
// On the wire each call is `JSON.stringify({action, args, request_id})`
// posted via `window.orca_native.postMessage(...)`. C++ replies with
// `window.__orca_resolve(<id>, <result_json>, <err_or_null>)`.
//
// All wx state mutations happen on the UI thread. Long-running HTTP /
// install work is dispatched to background `std::thread`s; results are
// marshalled back via `wxTheApp->CallAfter` (same pattern as
// WebViewPluginHost::SubscribeEvent dispatchers).
#pragma once

#include <wx/dialog.h>
#include <wx/webview.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class wxWindowDestroyEvent;

namespace Slic3r {
class MarketplaceClient;
struct MarketplaceCatalog;

namespace GUI {

class MarketplaceDialog : public wxDialog {
public:
    explicit MarketplaceDialog(wxWindow* parent);
    ~MarketplaceDialog() override;

private:
    void OnScriptMessage(wxWebViewEvent& event);
    void OnDestroy(wxWindowDestroyEvent& event);

    // Bridge handlers. Each one is dispatched off of an action name on
    // the UI thread. They may either:
    //   - resolve synchronously by calling resolve() / reject() before
    //     returning, or
    //   - kick off background work and resolve later from a CallAfter.
    void handle_config    (std::int64_t request_id, const std::string& args_json);
    void handle_list      (std::int64_t request_id, const std::string& args_json);
    void handle_installed (std::int64_t request_id, const std::string& args_json);
    void handle_install   (std::int64_t request_id, const std::string& args_json);
    void handle_update    (std::int64_t request_id, const std::string& args_json);
    void handle_uninstall (std::int64_t request_id, const std::string& args_json);
    void handle_open_external(std::int64_t request_id, const std::string& args_json);

    void resolve(std::int64_t request_id, const std::string& result_json);
    void reject (std::int64_t request_id, const std::string& error_message);

    // Returns true if there is no in-flight task for (action, plugin_id).
    // Adds an entry on true, leaves the table untouched on false. Always
    // call `finish_task` from the resolving CallAfter regardless of
    // whether the work succeeded.
    bool begin_task (const std::string& action, const std::string& plugin_id);
    void finish_task(const std::string& action, const std::string& plugin_id);

    wxWebView* webview_ = nullptr;

    // The HTTP client is owned by the dialog. It's synchronous, so any
    // call to fetch_catalog / download_plugin must happen on a worker
    // thread.
    std::unique_ptr<MarketplaceClient> client_;

    // Last successfully-fetched catalog. Populated by handle_list and
    // read by handle_install / handle_update for the version lookup.
    // Guarded only by the UI-thread contract: it's written and read on
    // the UI thread; background download lambdas take a copy of the
    // MarketplaceVersion before they detach.
    std::unique_ptr<MarketplaceCatalog> cached_catalog_;

    // In-flight task table — one entry per (action, plugin_id). Prevents
    // a double-click on Install from starting two concurrent downloads
    // for the same plugin. The dialog is single-instance and the table
    // is always touched on the UI thread.
    struct ActiveTask { std::string action; std::string plugin_id; };
    std::vector<ActiveTask> active_tasks_;

    static constexpr const char* kHandlerName = "orca_marketplace";
};

}} // namespace Slic3r::GUI
