// Phase 4.4 — MarketplaceDialog implementation.
//
// Bridge pattern mirrors `slic3r/Utils/WebViewPluginHost.cpp` — same
// shim shape, same `window.__orca_resolve(id, result, err)` resolution
// callback, same `wxTheApp->CallAfter` marshalling for off-thread
// replies. Keeping the shim shape identical means the marketplace SPA
// and any future window.orca.* extension share the same JS-side
// reception logic.

#include "MarketplaceDialog.hpp"

#include "I18N.hpp"
#include "PluginPermissionDialog.hpp"
#include "Widgets/WebView.hpp"

#include "../Utils/MarketplaceClient.hpp"
#include "../Utils/PluginInstaller.hpp"

#include "orca/Globals.hpp"
#include "orca/Session.hpp"
#include "orca/Slicer.hpp"

#include "libslic3r/Utils.hpp"   // Slic3r::resources_dir, Slic3r::data_dir

#include "nlohmann/json.hpp"

#include <wx/app.h>
#include <wx/filename.h>
#include <wx/filesys.h>
#include <wx/sizer.h>
#include <wx/utils.h>
#include <wx/webview.h>
#include <wx/weakref.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI {

namespace {

using nlohmann::json;

// JS shim that runs before any SPA script. Identical resolution surface
// to the WebViewPluginHost shim (`window.__orca_resolve`); we just
// namespace the API under `window.orca.marketplace.*`.
//
// Idempotent across navigations — the SPA can be reloaded in-dialog
// without leaking state.
constexpr const char* kMarketplaceShim = R"JS(
(function () {
    if (window.orca && window.orca.marketplace && window.__orca_resolve) return;
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
            const msg = JSON.stringify({ action, args: args || {}, request_id: id });
            window.orca_native.postMessage(msg);
        });
    }

    window.orca = window.orca || {};
    window.orca.marketplace = {
        config:        ()              => call('config', {}),
        list:          ()              => call('list', {}),
        installed:     ()              => call('installed', {}),
        install:       (id, version)   => call('install',   { id, version }),
        update:        (id, version)   => call('update',    { id, version }),
        uninstall:     (id)            => call('uninstall', { id }),
        open_external: (url)           => call('open_external', { url }),
    };
})();
)JS";

// Inline data URL shown when no marketplace base URL is configured. Renders
// without touching the SPA so the dialog works even on a fresh upstream
// build that left ORCA_MARKETPLACE_DEFAULT_URL empty.
wxString unconfigured_html_url() {
    constexpr const char* kHtml =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<title>Marketplace</title>"
        "<style>body{font-family:system-ui,sans-serif;padding:2.5em;color:#222;"
        "max-width:560px;margin:0 auto;}h2{margin-top:0;}code{background:#f3f3f3;"
        "padding:.1em .35em;border-radius:3px;}p{line-height:1.5;}</style>"
        "</head><body>"
        "<h2>No marketplace configured</h2>"
        "<p>OrcaSlicer ships without a default plugin marketplace. To browse "
        "and install plugins, set a marketplace registry URL in "
        "<strong>Preferences &rarr; Online &rarr; Marketplace URL</strong>, "
        "then reopen this dialog.</p>"
        "<p>Power users can also set the <code>ORCA_MARKETPLACE_URL</code> "
        "environment variable, or build with "
        "<code>-DORCA_MARKETPLACE_DEFAULT_URL=&lt;url&gt;</code>.</p>"
        "</body></html>";
    wxString data_url = "data:text/html;charset=utf-8,";
    data_url << wxString::FromUTF8(kHtml);
    return data_url;
}

// Resolve the SPA bundle's entry point. We look first under
// `<resources_dir>/marketplace-ui/index.html` (the production layout —
// deploy step is documented at the top of the constructor) and fall
// back to a `Resources/`-cased variant for macOS bundles where the
// trailing slash of `Slic3r::resources_dir()` may differ.
//
// DEPLOYMENT NOTE: the sibling `marketplace-ui/` Vite project produces
// a static bundle that MUST be copied to `<resources>/marketplace-ui/`
// before the slicer ships. CI / packaging is responsible for that step;
// missing-bundle behaviour at runtime is a friendly error page below.
wxString resolve_spa_url() {
    const std::filesystem::path resources = Slic3r::resources_dir();
    const std::filesystem::path candidate = resources / "marketplace-ui" / "index.html";

    std::error_code ec;
    if (std::filesystem::is_regular_file(candidate, ec)) {
        // wxWebView accepts file:// URLs; wxFileName::FileNameToURL handles
        // the platform-appropriate escaping (drive letters on Windows, etc).
        const wxFileName fn(wxString::FromUTF8(candidate.string()));
        return wxFileSystem::FileNameToURL(fn);
    }

    // Bundle not deployed — show an inline data: URL with instructions so
    // developers running an undeployed build don't see a blank window.
    constexpr const char* kMissingHtml =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<title>Marketplace</title>"
        "<style>body{font-family:system-ui,sans-serif;padding:2em;color:#222;}"
        "code{background:#f3f3f3;padding:.1em .3em;border-radius:3px;}</style>"
        "</head><body>"
        "<h2>Marketplace UI bundle not found.</h2>"
        "<p>The marketplace SPA was not deployed to "
        "<code>&lt;resources&gt;/marketplace-ui/</code>. Build the "
        "<code>marketplace-ui</code> Vite project and copy its <code>dist/</code> "
        "output to that location, then reopen this dialog.</p>"
        "</body></html>";
    wxString data_url = "data:text/html;charset=utf-8,";
    data_url << wxString::FromUTF8(kMissingHtml);
    return data_url;
}

// Serialize a single Session::ManifestInfo for the `installed()` reply.
json manifest_to_json(const ::orca::Session::ManifestInfo& m, bool loaded) {
    return {
        {"id",          m.id},
        {"name",        m.name},
        {"version",     m.version},
        {"author",      m.author},
        {"description", m.description},
        {"permissions", m.permissions},
        {"loaded",      loaded},
    };
}

// Serialize a MarketplaceVersion for the `list()` reply.
json version_to_json(const MarketplaceVersion& v) {
    return {
        {"version",       v.version},
        {"kind",          v.kind},
        {"provides",      v.provides},
        {"permissions",   v.permissions},
        {"engine_compat", v.engine_compat},
        {"url",           v.url},
        {"sha256",        v.sha256},
        {"size_bytes",    v.size_bytes},
        {"published_at",  v.published_at},
    };
}

// Serialize a MarketplacePlugin (with all its versions).
json plugin_to_json(const MarketplacePlugin& p) {
    json versions = json::array();
    for (const auto& v : p.versions) versions.push_back(version_to_json(v));

    json j = {
        {"id",             p.id},
        {"name",           p.name},
        {"description",    p.description},
        {"category",       p.category},
        {"tags",           p.tags},
        {"author",         p.author},
        {"screenshots",    p.screenshots},
        {"latest_version", p.latest_version},
        {"versions",       versions},
    };
    if (p.homepage) j["homepage"] = *p.homepage;
    else            j["homepage"] = nullptr;
    if (p.repo)     j["repo"]     = *p.repo;
    else            j["repo"]     = nullptr;
    if (p.license)  j["license"]  = *p.license;
    else            j["license"]  = nullptr;
    return j;
}

json catalog_to_json(const MarketplaceCatalog& c) {
    json plugins = json::array();
    for (const auto& p : c.plugins) plugins.push_back(plugin_to_json(p));
    return {
        {"revision_sha256", c.revision_sha256},
        {"generated_at",    c.generated_at},
        {"plugins",         plugins},
    };
}

// Sanitize a plugin id for use as a file/dir name. The marketplace plugin id
// scheme is reverse-DNS so this is mostly a defence-in-depth check —
// reject anything with a path separator or a leading dot.
bool is_safe_plugin_id(const std::string& id) {
    if (id.empty() || id.front() == '.') return false;
    for (char c : id) {
        if (c == '/' || c == '\\' || c == '\0') return false;
    }
    return true;
}

} // namespace

MarketplaceDialog::MarketplaceDialog(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, _L("Plugin Marketplace"),
               wxDefaultPosition,
               // 1024x720 default, FromDIP-scaled so HiDPI users get a
               // sensibly-sized dialog without manual maximisation.
               parent ? parent->FromDIP(wxSize(1024, 720)) : wxSize(1024, 720),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    // Construct the marketplace client up front — the constructor is
    // cheap (no network); the synchronous fetch_catalog only happens on
    // a worker thread inside handle_list.
    try {
        client_ = std::make_unique<MarketplaceClient>(
            MarketplaceClient::default_instance());
    } catch (const std::exception& e) {
        std::fprintf(stderr,
            "[orca][marketplace] failed to construct MarketplaceClient: %s\n",
            e.what());
    }

    // Build the webview. If the marketplace base URL is unconfigured we
    // render a native empty-state instead of loading the SPA — the SPA
    // would either fetch nothing or render its own dev fixtures, neither
    // of which is what a user expects in production.
    const wxString url = MarketplaceClient::is_configured()
        ? resolve_spa_url()
        : unconfigured_html_url();
    webview_ = WebView::CreateWebView(this, url);

    if (webview_) {
        webview_->AddScriptMessageHandler(wxString::FromUTF8("orca_native"));
        webview_->AddUserScript(wxString::FromUTF8(kMarketplaceShim));

        Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED,
             &MarketplaceDialog::OnScriptMessage, this, webview_->GetId());
    }

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    if (webview_) sizer->Add(webview_, 1, wxEXPAND);
    SetSizer(sizer);
    Layout();

    Bind(wxEVT_DESTROY, &MarketplaceDialog::OnDestroy, this);
}

MarketplaceDialog::~MarketplaceDialog() {
    // MarketplaceClient is synchronous and lives on worker threads; any
    // in-flight std::thread was detached at launch time. We rely on
    // wxWeakRef guards inside the CallAfter lambdas to no-op cleanly
    // once `this` is gone — see resolve()/reject().
}

void MarketplaceDialog::OnDestroy(wxWindowDestroyEvent& event) {
    event.Skip();
}

void MarketplaceDialog::resolve(std::int64_t request_id,
                                const std::string& result_json)
{
    if (!webview_) return;
    wxString js;
    js << "window.__orca_resolve(" << static_cast<long long>(request_id) << ", "
       << wxString::FromUTF8(result_json) << ", null);";
    webview_->RunScriptAsync(js);
}

void MarketplaceDialog::reject(std::int64_t request_id,
                               const std::string& error_message)
{
    if (!webview_) return;
    // JSON-encode the message so embedded quotes / newlines survive the
    // splice into the JS string literal.
    json j_msg = error_message;
    wxString js;
    js << "window.__orca_resolve(" << static_cast<long long>(request_id) << ", null, "
       << wxString::FromUTF8(j_msg.dump()) << ");";
    webview_->RunScriptAsync(js);
}

bool MarketplaceDialog::begin_task(const std::string& action,
                                   const std::string& plugin_id)
{
    const auto it = std::find_if(active_tasks_.begin(), active_tasks_.end(),
        [&](const ActiveTask& t) {
            return t.action == action && t.plugin_id == plugin_id;
        });
    if (it != active_tasks_.end()) return false;
    active_tasks_.push_back({action, plugin_id});
    return true;
}

void MarketplaceDialog::finish_task(const std::string& action,
                                    const std::string& plugin_id)
{
    const auto it = std::find_if(active_tasks_.begin(), active_tasks_.end(),
        [&](const ActiveTask& t) {
            return t.action == action && t.plugin_id == plugin_id;
        });
    if (it != active_tasks_.end()) active_tasks_.erase(it);
}

void MarketplaceDialog::OnScriptMessage(wxWebViewEvent& evt) {
    json j;
    try {
        j = json::parse(evt.GetString().utf8_string());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[orca][marketplace] malformed message: %s\n", e.what());
        return;
    }

    const std::int64_t request_id =
        j.contains("request_id") && j["request_id"].is_number_integer()
            ? j["request_id"].get<std::int64_t>()
            : 0;
    const std::string action = j.value("action", std::string{});
    const std::string args_json =
        j.contains("args") ? j["args"].dump() : std::string{"{}"};

    try {
        if      (action == "config")        handle_config(request_id, args_json);
        else if (action == "list")          handle_list(request_id, args_json);
        else if (action == "installed")     handle_installed(request_id, args_json);
        else if (action == "install")       handle_install(request_id, args_json);
        else if (action == "update")        handle_update(request_id, args_json);
        else if (action == "uninstall")     handle_uninstall(request_id, args_json);
        else if (action == "open_external") handle_open_external(request_id, args_json);
        else {
            reject(request_id, std::string{"unknown action: "} + action);
        }
    } catch (const std::exception& e) {
        reject(request_id, e.what());
    }
}

// ---- config ----------------------------------------------------------

void MarketplaceDialog::handle_config(std::int64_t request_id,
                                      const std::string& /*args_json*/)
{
    // The SPA reads `registry_url` to decide whether to render its own
    // empty-state. `user_overridable` is always true here — the only path
    // by which the SPA can run is via the slicer's own dialog, which
    // exposes the Preferences row.
    json reply = {
        {"registry_url",    MarketplaceClient::resolved_base_url()},
        {"user_overridable", true},
    };
    resolve(request_id, reply.dump());
}

// ---- list ------------------------------------------------------------

void MarketplaceDialog::handle_list(std::int64_t request_id,
                                    const std::string& /*args_json*/)
{
    if (!client_) {
        reject(request_id, "marketplace client unavailable");
        return;
    }
    if (!begin_task("list", "")) {
        reject(request_id, "list already in flight");
        return;
    }

    wxWeakRef<MarketplaceDialog> weak_self(this);
    // MarketplaceClient is move-constructible / copyable per its declared
    // surface (default_instance returns by value). We hold our own
    // pointer; the thread captures a borrowed pointer that's kept alive
    // by the dialog (the thread is detached but completes synchronously
    // before any plausible dialog teardown — and the weak_self guard
    // in the CallAfter is what actually protects against races).
    MarketplaceClient* client_ptr = client_.get();

    std::thread worker([weak_self, client_ptr, request_id]() {
        std::string result_json;
        std::string err;
        std::unique_ptr<MarketplaceCatalog> fetched;
        try {
            fetched = std::make_unique<MarketplaceCatalog>(client_ptr->fetch_catalog());
            result_json = catalog_to_json(*fetched).dump();
        } catch (const std::exception& e) {
            err = e.what();
        }

        // Marshal the result + cache update back to the UI thread.
        auto* shared_catalog = fetched.release();
        wxTheApp->CallAfter(
            [weak_self, request_id, result_json, err, shared_catalog]() {
                std::unique_ptr<MarketplaceCatalog> owned(shared_catalog);
                auto* self = weak_self.get();
                if (!self) return;
                self->finish_task("list", "");
                if (!err.empty()) {
                    self->reject(request_id, err);
                    return;
                }
                self->cached_catalog_ = std::move(owned);
                self->resolve(request_id, result_json);
            });
    });
    worker.detach();
}

// ---- installed -------------------------------------------------------

void MarketplaceDialog::handle_installed(std::int64_t request_id,
                                         const std::string& /*args_json*/)
{
    if (!::orca::has_session()) {
        reject(request_id, "engine session not available");
        return;
    }
    auto& session = ::orca::session();
    const auto manifests  = session.plugin_manifests();
    const auto loaded_ids = session.loaded_plugin_ids();

    json plugins = json::array();
    for (const auto& m : manifests) {
        const bool loaded = std::find(loaded_ids.begin(), loaded_ids.end(), m.id)
                            != loaded_ids.end();
        plugins.push_back(manifest_to_json(m, loaded));
    }
    json result = { {"plugins", plugins} };
    resolve(request_id, result.dump());
}

// ---- install / update shared implementation -------------------------

namespace {

// Find a matching (plugin, version) entry inside a catalog. Returns
// nullptr if either is missing.
const MarketplaceVersion* find_version(const MarketplaceCatalog* catalog,
                                       const std::string& id,
                                       const std::string& version)
{
    if (!catalog) return nullptr;
    auto pit = std::find_if(catalog->plugins.begin(), catalog->plugins.end(),
        [&](const MarketplacePlugin& p) { return p.id == id; });
    if (pit == catalog->plugins.end()) return nullptr;
    // Empty version string means "latest".
    const std::string target = version.empty() ? pit->latest_version : version;
    auto vit = std::find_if(pit->versions.begin(), pit->versions.end(),
        [&](const MarketplaceVersion& v) { return v.version == target; });
    if (vit == pit->versions.end()) return nullptr;
    return &*vit;
}

} // namespace

void MarketplaceDialog::handle_install(std::int64_t request_id,
                                       const std::string& args_json)
{
    json args;
    try { args = json::parse(args_json); }
    catch (const std::exception& e) { reject(request_id, e.what()); return; }

    const std::string id      = args.value("id", std::string{});
    const std::string version = args.value("version", std::string{});
    if (!is_safe_plugin_id(id)) {
        reject(request_id, "invalid plugin id");
        return;
    }
    if (!::orca::has_session()) {
        reject(request_id, "engine session not available");
        return;
    }

    const MarketplaceVersion* mv = find_version(cached_catalog_.get(), id, version);
    if (!mv) {
        // No cached catalog or no matching entry — the SPA should call
        // list() first. Resolve with a structured `status:"error"` so
        // the UI can render a retry prompt.
        json reply = { {"status", "error"},
                       {"message", "catalog entry not found; call list() first"} };
        resolve(request_id, reply.dump());
        return;
    }

    // Block install if a slice is in progress — same constraint
    // PluginManagerDialog enforces, for the same reason: live plugin
    // vtables must not be yanked out from under the slicer.
    auto& session = ::orca::session();
    if (session.slicer().is_busy()) {
        json reply = { {"status", "error"},
                       {"message", "cannot install while a slice is in progress"} };
        resolve(request_id, reply.dump());
        return;
    }

    // Permission prompt — uses the requested permissions reported by
    // the catalog. The names come back as strings; map them to the
    // ORCA_PERM_* bit positions used by PluginPermissionDialog. The
    // post-download `peek_identity` on the actual archive will also
    // surface any mismatch.
    auto perm_bit = [](const std::string& name) -> std::uint64_t {
        if (name == "NETWORK")          return 1ull << 0;
        if (name == "FILESYSTEM_READ")  return 1ull << 1;
        if (name == "FILESYSTEM_WRITE") return 1ull << 2;
        if (name == "SETTINGS_READ")    return 1ull << 3;
        if (name == "SETTINGS_WRITE")   return 1ull << 4;
        if (name == "PROFILES_INSTALL") return 1ull << 5;
        if (name == "DEVICE_CONTROL")   return 1ull << 6;
        if (name == "SLICE_INTERCEPT")  return 1ull << 7;
        if (name == "GCODE_MODIFY")     return 1ull << 8;
        if (name == "UI_ATTACH")        return 1ull << 9;
        if (name == "EVENTS_RAW")       return 1ull << 10;
        return 0;
    };
    std::uint64_t requested_bits = 0;
    for (const auto& name : mv->permissions) requested_bits |= perm_bit(name);

    // Look up the plugin name from the catalog for a friendlier prompt.
    std::string display_name = id;
    for (const auto& p : cached_catalog_->plugins)
        if (p.id == id) { display_name = p.name; break; }

    PluginPermissionDialog perms(this, id, display_name, mv->version, requested_bits);
    if (perms.ShowModal() != wxID_OK) {
        json reply = { {"status", "denied"}, {"message", "user cancelled"} };
        resolve(request_id, reply.dump());
        return;
    }

    if (!begin_task("install", id)) {
        json reply = { {"status", "error"}, {"message", "install already in flight"} };
        resolve(request_id, reply.dump());
        return;
    }

    // Copy everything the worker thread needs off-stack.
    const MarketplaceVersion version_copy = *mv;
    MarketplaceClient* client_ptr = client_.get();
    wxWeakRef<MarketplaceDialog> weak_self(this);

    std::thread worker([weak_self, request_id, id, version_copy, client_ptr]() {
        json reply;
        try {
            if (!client_ptr) throw std::runtime_error("marketplace client unavailable");

            // Stage the download under <data_dir>/marketplace/downloads/.
            const auto downloads_dir =
                std::filesystem::path(Slic3r::data_dir()) / "marketplace" / "downloads";
            std::error_code ec;
            std::filesystem::create_directories(downloads_dir, ec);
            if (ec) throw std::runtime_error(
                "failed to create downloads dir: " + ec.message());

            const auto archive = downloads_dir /
                (id + "-" + version_copy.version + ".orcaplugin");
            client_ptr->download_plugin(version_copy, archive);

            // Hand off to PluginInstaller + Session::load_plugin on the
            // UI thread — load_plugin touches engine state that's not
            // expected to be hammered from worker threads, and the
            // post-install permission dialog already ran.
            wxTheApp->CallAfter([weak_self, request_id, id, archive]() {
                auto* self = weak_self.get();
                if (!self) return;
                json reply;
                try {
                    auto& session = ::orca::session();
                    const auto plugins_root =
                        std::filesystem::path(Slic3r::data_dir()) / "plugins";
                    std::error_code ec2;
                    std::filesystem::create_directories(plugins_root, ec2);

                    if (session.is_plugin_loaded(id))
                        session.unload_plugin(id);

                    auto inst = Slic3r::PluginInstaller::install(
                        archive, plugins_root, /*replace_existing=*/false);
                    if (inst.status != Slic3r::PluginInstaller::Status::Ok) {
                        reply = { {"status", "error"},
                                  {"message", inst.error_message.empty()
                                      ? std::string{"install failed"}
                                      : inst.error_message} };
                    } else {
                        auto load_rc = session.load_plugin(inst.install_dir);
                        if (!load_rc.ok()) {
                            reply = { {"status", "error"},
                                      {"message", load_rc.error().message} };
                        } else {
                            reply = { {"status", "ok"},
                                      {"id", id},
                                      {"version", inst.identity.version} };
                        }
                    }
                } catch (const std::exception& e) {
                    reply = { {"status", "error"}, {"message", e.what()} };
                }
                self->finish_task("install", id);
                self->resolve(request_id, reply.dump());
            });
            return;
        } catch (const std::exception& e) {
            reply = { {"status", "error"}, {"message", e.what()} };
        }
        wxTheApp->CallAfter([weak_self, request_id, id, reply]() {
            auto* self = weak_self.get();
            if (!self) return;
            self->finish_task("install", id);
            self->resolve(request_id, reply.dump());
        });
    });
    worker.detach();
}

void MarketplaceDialog::handle_update(std::int64_t request_id,
                                      const std::string& args_json)
{
    json args;
    try { args = json::parse(args_json); }
    catch (const std::exception& e) { reject(request_id, e.what()); return; }

    const std::string id      = args.value("id", std::string{});
    const std::string version = args.value("version", std::string{});
    if (!is_safe_plugin_id(id)) {
        reject(request_id, "invalid plugin id");
        return;
    }
    if (!::orca::has_session()) {
        reject(request_id, "engine session not available");
        return;
    }

    const MarketplaceVersion* mv = find_version(cached_catalog_.get(), id, version);
    if (!mv) {
        json reply = { {"status", "error"},
                       {"message", "catalog entry not found; call list() first"} };
        resolve(request_id, reply.dump());
        return;
    }

    auto& session = ::orca::session();
    if (session.slicer().is_busy()) {
        json reply = { {"status", "error"},
                       {"message", "cannot update while a slice is in progress"} };
        resolve(request_id, reply.dump());
        return;
    }

    if (!begin_task("update", id)) {
        json reply = { {"status", "error"}, {"message", "update already in flight"} };
        resolve(request_id, reply.dump());
        return;
    }

    const MarketplaceVersion version_copy = *mv;
    MarketplaceClient* client_ptr = client_.get();
    wxWeakRef<MarketplaceDialog> weak_self(this);

    std::thread worker([weak_self, request_id, id, version_copy, client_ptr]() {
        json reply;
        try {
            if (!client_ptr) throw std::runtime_error("marketplace client unavailable");

            const auto downloads_dir =
                std::filesystem::path(Slic3r::data_dir()) / "marketplace" / "downloads";
            std::error_code ec;
            std::filesystem::create_directories(downloads_dir, ec);
            if (ec) throw std::runtime_error(
                "failed to create downloads dir: " + ec.message());

            const auto archive = downloads_dir /
                (id + "-" + version_copy.version + ".orcaplugin");
            client_ptr->download_plugin(version_copy, archive);

            wxTheApp->CallAfter([weak_self, request_id, id, archive]() {
                auto* self = weak_self.get();
                if (!self) return;
                json reply;
                try {
                    auto& session = ::orca::session();
                    const auto plugins_root =
                        std::filesystem::path(Slic3r::data_dir()) / "plugins";

                    // For update we explicitly replace the existing
                    // install directory. PluginInstaller takes the same
                    // unload-then-extract flow as the manual installer.
                    if (session.is_plugin_loaded(id))
                        session.unload_plugin(id);

                    auto inst = Slic3r::PluginInstaller::install(
                        archive, plugins_root, /*replace_existing=*/true);
                    if (inst.status != Slic3r::PluginInstaller::Status::Ok) {
                        reply = { {"status", "error"},
                                  {"message", inst.error_message.empty()
                                      ? std::string{"update failed"}
                                      : inst.error_message} };
                    } else {
                        // After a replace-install the plugin is no
                        // longer loaded — load_plugin to bring the new
                        // bits online. reload_plugin would fail because
                        // we just unloaded it; load_plugin from the new
                        // install dir is the correct primitive here.
                        auto load_rc = session.load_plugin(inst.install_dir);
                        if (!load_rc.ok()) {
                            reply = { {"status", "error"},
                                      {"message", load_rc.error().message} };
                        } else {
                            reply = { {"status", "ok"},
                                      {"id", id},
                                      {"version", inst.identity.version} };
                        }
                    }
                } catch (const std::exception& e) {
                    reply = { {"status", "error"}, {"message", e.what()} };
                }
                self->finish_task("update", id);
                self->resolve(request_id, reply.dump());
            });
            return;
        } catch (const std::exception& e) {
            reply = { {"status", "error"}, {"message", e.what()} };
        }
        wxTheApp->CallAfter([weak_self, request_id, id, reply]() {
            auto* self = weak_self.get();
            if (!self) return;
            self->finish_task("update", id);
            self->resolve(request_id, reply.dump());
        });
    });
    worker.detach();
}

// ---- uninstall -------------------------------------------------------

void MarketplaceDialog::handle_uninstall(std::int64_t request_id,
                                         const std::string& args_json)
{
    json args;
    try { args = json::parse(args_json); }
    catch (const std::exception& e) { reject(request_id, e.what()); return; }

    const std::string id = args.value("id", std::string{});
    if (!is_safe_plugin_id(id)) {
        reject(request_id, "invalid plugin id");
        return;
    }
    if (!::orca::has_session()) {
        reject(request_id, "engine session not available");
        return;
    }

    auto& session = ::orca::session();
    if (session.slicer().is_busy()) {
        json reply = { {"status", "error"},
                       {"message", "cannot uninstall while a slice is in progress"} };
        resolve(request_id, reply.dump());
        return;
    }

    if (!begin_task("uninstall", id)) {
        json reply = { {"status", "error"}, {"message", "uninstall already in flight"} };
        resolve(request_id, reply.dump());
        return;
    }

    json reply;
    try {
        if (session.is_plugin_loaded(id)) {
            auto rc = session.unload_plugin(id);
            if (!rc.ok()) {
                reply = { {"status", "error"}, {"message", rc.error().message} };
                finish_task("uninstall", id);
                resolve(request_id, reply.dump());
                return;
            }
        }

        const auto plugin_dir =
            std::filesystem::path(Slic3r::data_dir()) / "plugins" / id;
        std::error_code ec;
        if (std::filesystem::exists(plugin_dir, ec)) {
            std::filesystem::remove_all(plugin_dir, ec);
            if (ec) {
                reply = { {"status", "error"},
                          {"message", "remove_all failed: " + ec.message()} };
                finish_task("uninstall", id);
                resolve(request_id, reply.dump());
                return;
            }
        }
        reply = { {"status", "ok"}, {"id", id} };
    } catch (const std::exception& e) {
        reply = { {"status", "error"}, {"message", e.what()} };
    }
    finish_task("uninstall", id);
    resolve(request_id, reply.dump());
}

// ---- open_external ---------------------------------------------------

void MarketplaceDialog::handle_open_external(std::int64_t request_id,
                                             const std::string& args_json)
{
    json args;
    try { args = json::parse(args_json); }
    catch (const std::exception& e) { reject(request_id, e.what()); return; }

    const std::string url = args.value("url", std::string{});
    if (url.empty()) {
        reject(request_id, "url is required");
        return;
    }
    // Defence-in-depth: only allow http/https. The SPA shouldn't be
    // sending other schemes; if it does, refuse rather than handing
    // arbitrary URIs to the OS handler.
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        reject(request_id, "only http(s) URLs are allowed");
        return;
    }
    wxLaunchDefaultBrowser(wxString::FromUTF8(url));
    resolve(request_id, "null");
}

}} // namespace Slic3r::GUI
