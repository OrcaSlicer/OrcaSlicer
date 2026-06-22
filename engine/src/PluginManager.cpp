// PluginManager — Phase 1.2.1 skeleton.
//
// Discovers <plugins_dir>/<id>/manifest.json entries, dlopens the entry
// binary, resolves the three mandatory orca_plugin_* exports, runs the
// debug/release sanity check + ABI handshake, and tracks loaded plugins.
// Mirrors src/slic3r/Utils/BBLNetworkPlugin.cpp (load: 116-146,
// unload: 185-212, get_function: 314-328, debug check: 222-232).
//
// Scope: just enough to load and unload. Slot vtable dispatch and the
// real per-slot host calls (load_profile_pack, placeholder injection,
// register_printer_agent, raw event subscribers) are stubs returning
// ORCA_ERR_UNSUPPORTED until Phase 2 wires them.

#include "PluginManager.hpp"
#include "PluginRegistry.hpp"

#include "orca/Session.hpp"
#include "orca/c_api.h"
#include "orca/PlaceholderProvider.hpp"
#include "libslic3r/PlaceholderParser.hpp"

#include "runtime/WasmHost.hpp"
#include "runtime/WasmPlugin.hpp"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <utility>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace orca {

// ---------------------------------------------------------------------------
// Function-pointer typedefs for the three mandatory plugin exports.
// Names + signatures from engine/include/orca/plugin_api.h.
// ---------------------------------------------------------------------------
using orca_plugin_register_fn_t =
    orca_error_code_t (*)(uint32_t,
                          orca_plugin_registry_t*,
                          const orca_plugin_host_t*);
using orca_plugin_unregister_fn_t =
    void (*)(void);
using orca_plugin_check_debug_consistent_fn_t =
    int (*)(int);

// ---------------------------------------------------------------------------
// LoadedPlugin — per-plugin record. Owns the OS module handle and the
// resolved entry-point function pointers.
// ---------------------------------------------------------------------------
struct PluginManager::LoadedPlugin {
    std::string id;
    std::string name;
    std::string version;
    std::string author;
    std::string description;
    uint64_t    permissions = 0;
    std::filesystem::path source_dir;   ///< Where the plugin was loaded from (for reload).

#if defined(_WIN32)
    HMODULE handle = nullptr;
#else
    void* handle = nullptr;
#endif

    orca_plugin_register_fn_t              fn_register   = nullptr;
    orca_plugin_unregister_fn_t            fn_unregister = nullptr;
    orca_plugin_check_debug_consistent_fn_t fn_check_debug = nullptr;

    // Data-only (profile pack) plugins: no entry binary, no exports. The
    // pack dir path is heap-stored in owned_strings so the c_str() handed to
    // the slot vtable stays valid for the plugin's lifetime.
    std::vector<std::string>                  owned_strings;
    std::unique_ptr<orca_slot_profile_pack_t> profile_pack_vtable;

    // Phase 3.2.6 — wasm plugin (kind=="wasm"). When non-null, this
    // LoadedPlugin is wasm-backed: no dlopen handle, no fn_* exports. Its
    // destructor calls orca_plugin_unregister automatically.
    std::unique_ptr<wasm::WasmPlugin> wasm_plugin;
};

// ---------------------------------------------------------------------------
// Impl — owns bound session/registry, the host vtable handed to every
// loaded plugin, and the map of loaded plugins. The host vtable itself
// dispatches via the static s_instance below; that's how the C-callable
// thunks find their way back to this PluginManager.
// ---------------------------------------------------------------------------
struct PluginManager::Impl {
    Session*        session  = nullptr;
    PluginRegistry* registry = nullptr;

    orca_plugin_host_t host_vtable{};

    mutable std::mutex mtx;

    std::unordered_map<std::string, std::unique_ptr<LoadedPlugin>> plugins;
    std::vector<std::string>                                       load_order;

    // Phase 3.2.6 — shared wasmtime engine. Lazily constructed on the
    // first wasm plugin load so installs that never touch wasm don't
    // pay the wasm_engine_new cost.
    std::unique_ptr<wasm::WasmHost> wasm_host;
};

// ---------------------------------------------------------------------------
// Static instance pointer for the C thunks. Set in bind_session and cleared
// in the destructor so the host vtable can be assembled without per-callback
// context. This is the same trick BBLNetworkPlugin uses for its globals.
// Only one PluginManager per engine is expected; if that ever changes the
// thunks need a different dispatch strategy.
// ---------------------------------------------------------------------------
namespace {
std::atomic<PluginManager::Impl*> s_instance{nullptr};

// =====================================================================
// Cross-platform dynamic-library helpers (mirrors BBLNetworkPlugin.cpp).
// =====================================================================

#if defined(_WIN32)
HMODULE dl_open(const std::filesystem::path& path)
{
    // Wide-char path through LoadLibraryW, matching BBLNetworkPlugin's
    // MultiByteToWideChar + LoadLibrary pattern (line 116-134).
    std::wstring wpath = path.wstring();
    return ::LoadLibraryW(wpath.c_str());
}
void dl_close(HMODULE h) { if (h) ::FreeLibrary(h); }
void* dl_sym(HMODULE h, const char* name)
{
    return reinterpret_cast<void*>(::GetProcAddress(h, name));
}
std::string dl_last_error()
{
    DWORD err = ::GetLastError();
    std::ostringstream oss;
    oss << "LoadLibrary/GetProcAddress failed, code=" << err;
    return oss.str();
}
constexpr const char* kPlatformExt = ".dll";
#else
void* dl_open(const std::filesystem::path& path)
{
    return ::dlopen(path.c_str(), RTLD_LAZY);
}
void dl_close(void* h) { if (h) ::dlclose(h); }
void* dl_sym(void* h, const char* name) { return ::dlsym(h, name); }
std::string dl_last_error()
{
    const char* e = ::dlerror();
    return e ? std::string{e} : std::string{"unknown dlopen/dlsym error"};
}
#  if defined(__APPLE__)
constexpr const char* kPlatformExt = ".dylib";
#  else
constexpr const char* kPlatformExt = ".so";
#  endif
#endif

// =====================================================================
// Permission-name → bit lookup. Mirrors the ORCA_PERM_* bits declared
// in plugin_api.h. Unknown names are silently ignored (logged at debug
// level by the caller).
// =====================================================================
uint64_t permission_bit(const std::string& name)
{
    if (name == "network")           return ORCA_PERM_NETWORK;
    if (name == "filesystem.read")   return ORCA_PERM_FILESYSTEM_READ;
    if (name == "filesystem.write")  return ORCA_PERM_FILESYSTEM_WRITE;
    if (name == "settings.read")     return ORCA_PERM_SETTINGS_READ;
    if (name == "settings.write")    return ORCA_PERM_SETTINGS_WRITE;
    if (name == "profiles.install")  return ORCA_PERM_PROFILES_INSTALL;
    if (name == "device.control")    return ORCA_PERM_DEVICE_CONTROL;
    if (name == "slice.intercept")   return ORCA_PERM_SLICE_INTERCEPT;
    if (name == "gcode.modify")      return ORCA_PERM_GCODE_MODIFY;
    if (name == "ui.attach")         return ORCA_PERM_UI_ATTACH;
    if (name == "events.raw")        return ORCA_PERM_EVENTS_RAW;
    return 0;
}

// =====================================================================
// Simple stderr logger. BOOST_LOG_TRIVIAL is fine too, but the engine
// keeps boost-logging out of its core surface so std::cerr is enough
// for Phase 1. Pairs with the host's log thunk below.
// =====================================================================
void log_line(int level, const char* msg)
{
    static const char* kLevels[] = {"trace", "debug", "info", "warn", "error"};
    const char* lvl = (level >= 0 && level <= 4) ? kLevels[level] : "?";
    std::cerr << "[orca][plugin][" << lvl << "] "
              << (msg ? msg : "") << '\n';
}

// =====================================================================
// Host vtable thunks. All callbacks dispatch through s_instance because
// the C ABI has no per-callback context. They forward into the engine's
// existing C ABI (c_api.cpp) for the bits already implemented and return
// ORCA_ERR_UNSUPPORTED for the bits Phase 2 owns.
// =====================================================================

extern "C" void host_log_thunk(int level, const char* msg)
{
    log_line(level, msg);
}

extern "C" orca_session_t* host_session_thunk(void)
{
    auto* impl = s_instance.load(std::memory_order_acquire);
    if (!impl || !impl->session) return nullptr;
    return reinterpret_cast<orca_session_t*>(impl->session);
}

extern "C" orca_subscription_id_t host_events_subscribe_thunk(
    orca_event_kind_t     kind,
    orca_event_callback_t cb,
    void*                 user_data)
{
    auto* impl = s_instance.load(std::memory_order_acquire);
    if (!impl || !impl->session) return 0;
    orca_events_t* ev = orca_session_events(
        reinterpret_cast<orca_session_t*>(impl->session));
    if (!ev) return 0;
    return orca_events_subscribe(ev, kind, cb, user_data);
}

extern "C" void host_events_unsubscribe_thunk(orca_subscription_id_t id)
{
    auto* impl = s_instance.load(std::memory_order_acquire);
    if (!impl || !impl->session) return;
    orca_events_t* ev = orca_session_events(
        reinterpret_cast<orca_session_t*>(impl->session));
    if (ev) orca_events_unsubscribe(ev, id);
}

extern "C" orca_subscription_id_t host_events_subscribe_raw_thunk(
    uint64_t                  /*kind_mask*/,
    orca_raw_event_callback_t /*cb*/,
    void*                     /*user_data*/)
{
    // Phase 2: raw escape-hatch subscriber. Until then signal "no subscription".
    return 0;
}

extern "C" const orca_presets_t* host_presets_const_thunk(void)
{
    auto* impl = s_instance.load(std::memory_order_acquire);
    if (!impl || !impl->session) return nullptr;
    return orca_session_presets(
        reinterpret_cast<orca_session_t*>(impl->session));
}

extern "C" orca_presets_t* host_presets_mut_thunk(void)
{
    auto* impl = s_instance.load(std::memory_order_acquire);
    if (!impl || !impl->session) return nullptr;
    return orca_session_presets(
        reinterpret_cast<orca_session_t*>(impl->session));
}

extern "C" orca_error_code_t host_load_profile_pack_thunk(const char* dir)
{
    // Forward into Presets::load_vendor_configs_from_json with the silent rule:
    // plugin packs are additive and should not throw on unknown option values.
    // The caller (plugin) only needs OK/non-OK here — the underlying
    // substitution list is consumed by the engine-side bundle directly.
    if (!dir || !*dir) return ORCA_ERR_INVALID_ARGUMENT;
    auto* impl = s_instance.load(std::memory_order_acquire);
    if (!impl || !impl->session) return ORCA_ERR_UNKNOWN;
    try {
        auto result = impl->session->presets().load_vendor_configs_from_json(
            std::filesystem::path{dir},
            orca::SubstitutionRule::EnableSilent);
        if (result.ok()) return ORCA_OK;
        switch (result.error().code) {
            case orca::ErrorCode::InvalidArgument: return ORCA_ERR_INVALID_ARGUMENT;
            case orca::ErrorCode::NotFound:        return ORCA_ERR_NOT_FOUND;
            case orca::ErrorCode::IoError:         return ORCA_ERR_IO;
            case orca::ErrorCode::ParseError:      return ORCA_ERR_PARSE;
            case orca::ErrorCode::Unsupported:     return ORCA_ERR_UNSUPPORTED;
            case orca::ErrorCode::NotImplemented:  return ORCA_ERR_UNSUPPORTED;
            default:                               return ORCA_ERR_UNKNOWN;
        }
    } catch (...) {
        return ORCA_ERR_UNKNOWN;
    }
}

extern "C" orca_error_code_t host_placeholder_set_string_thunk(
    const char* name, const char* value)
{
    if (!name || !value)  return ORCA_ERR_INVALID_ARGUMENT;
    auto* pp = orca::placeholder_tls::current();
    if (!pp)              return ORCA_ERR_INVALID_ARGUMENT;
    pp->set(name, std::string{value});
    return ORCA_OK;
}

extern "C" orca_error_code_t host_placeholder_set_int_thunk(
    const char* name, int64_t value)
{
    if (!name)            return ORCA_ERR_INVALID_ARGUMENT;
    auto* pp = orca::placeholder_tls::current();
    if (!pp)              return ORCA_ERR_INVALID_ARGUMENT;
    // PlaceholderParser::set(string, int) is the closest available overload.
    // Reject values outside int range so we never silently truncate.
    if (value < static_cast<int64_t>(std::numeric_limits<int>::min()) ||
        value > static_cast<int64_t>(std::numeric_limits<int>::max()))
        return ORCA_ERR_INVALID_ARGUMENT;
    pp->set(name, static_cast<int>(value));
    return ORCA_OK;
}

extern "C" orca_error_code_t host_placeholder_set_float_thunk(
    const char* name, double value)
{
    if (!name)            return ORCA_ERR_INVALID_ARGUMENT;
    auto* pp = orca::placeholder_tls::current();
    if (!pp)              return ORCA_ERR_INVALID_ARGUMENT;
    pp->set(name, value);
    return ORCA_OK;
}

extern "C" orca_error_code_t host_register_printer_agent_thunk(
    const char* agent_id, const void* vtable, void* user_data)
{
    if (!agent_id || !*agent_id || !vtable) return ORCA_ERR_INVALID_ARGUMENT;
    auto* impl = s_instance.load(std::memory_order_acquire);
    if (!impl || !impl->registry) return ORCA_ERR_UNKNOWN;
    const auto id = impl->registry->add_slot(
        ORCA_SLOT_PRINTER_AGENT, vtable, user_data, /*priority=*/0);
    return id != 0 ? ORCA_OK : ORCA_ERR_UNKNOWN;
}

extern "C" void host_string_free_thunk(const char* s)
{
    // The engine always allocates owned UTF-8 buffers with new char[N].
    // Keep both ends on the same allocator so cross-DLL frees stay safe.
    delete[] s;
}

} // namespace

// ---------------------------------------------------------------------------
// PluginManager — ctor / dtor.
// ---------------------------------------------------------------------------
PluginManager::PluginManager() : impl_(std::make_unique<Impl>()) {}

PluginManager::~PluginManager()
{
    try { unload_all(); } catch (...) { /* destructor swallows */ }

    // Clear the static instance only if it still points to us — if a future
    // multi-manager arrangement reassigned it, leave that arrangement alone.
    PluginManager::Impl* expected = impl_.get();
    s_instance.compare_exchange_strong(expected, nullptr,
                                       std::memory_order_acq_rel);
}

// ---------------------------------------------------------------------------
// bind_session — wire the session pointer, assemble the host vtable, and
// publish ourselves as the dispatch target for the C thunks. Plugins
// loaded after this call receive the same &host_vtable pointer.
// ---------------------------------------------------------------------------
void PluginManager::bind_session(Session* session, PluginRegistry* registry)
{
    std::lock_guard<std::mutex> guard(impl_->mtx);

    impl_->session  = session;
    impl_->registry = registry;

    auto& h = impl_->host_vtable;
    h.struct_size               = static_cast<uint32_t>(sizeof(orca_plugin_host_t));
    h.log                       = &host_log_thunk;
    h.session                   = &host_session_thunk;
    h.events_subscribe          = &host_events_subscribe_thunk;
    h.events_unsubscribe        = &host_events_unsubscribe_thunk;
    h.events_subscribe_raw      = &host_events_subscribe_raw_thunk;
    h.presets_const             = &host_presets_const_thunk;
    h.presets_mut               = &host_presets_mut_thunk;
    h.load_profile_pack         = &host_load_profile_pack_thunk;
    h.placeholder_set_string    = &host_placeholder_set_string_thunk;
    h.placeholder_set_int       = &host_placeholder_set_int_thunk;
    h.placeholder_set_float     = &host_placeholder_set_float_thunk;
    h.register_printer_agent    = &host_register_printer_agent_thunk;
    h.string_free               = &host_string_free_thunk;

    s_instance.store(impl_.get(), std::memory_order_release);
}

// ---------------------------------------------------------------------------
// discover_and_load — walk plugins_dir for subdirectories containing a
// manifest.json and try each one. Continues past individual failures.
// ---------------------------------------------------------------------------
std::size_t PluginManager::discover_and_load(const std::filesystem::path& plugins_dir)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(plugins_dir, ec))
        return 0;

    std::size_t loaded = 0;
    for (const auto& entry : std::filesystem::directory_iterator(plugins_dir, ec)) {
        if (ec) break;
        if (!entry.is_directory()) continue;
        const auto manifest_path = entry.path() / "manifest.json";
        if (!std::filesystem::exists(manifest_path)) continue;
        if (load_plugin(entry.path()) == ORCA_OK)
            ++loaded;
    }
    return loaded;
}

// ---------------------------------------------------------------------------
// load_plugin — the full load sequence:
//   1. parse manifest.json
//   2. dedupe against already-loaded plugins
//   3. resolve entry binary path
//   4. dlopen / LoadLibrary
//   5. resolve the three mandatory exports
//   6. debug/release sanity check
//   7. ABI handshake via orca_plugin_register
// Any failure unwinds the OS handle before returning.
// ---------------------------------------------------------------------------
orca_error_code_t PluginManager::load_plugin(const std::filesystem::path& plugin_dir)
{
    // --- 1. parse manifest.json -----------------------------------------
    const auto manifest_path = plugin_dir / "manifest.json";
    std::ifstream ifs(manifest_path);
    if (!ifs.is_open()) {
        log_line(4, ("manifest not found: " + manifest_path.string()).c_str());
        return ORCA_ERR_NOT_FOUND;
    }

    nlohmann::json j;
    try {
        ifs >> j;
    } catch (const std::exception& ex) {
        log_line(4, (std::string{"manifest parse failed: "} + ex.what()).c_str());
        return ORCA_ERR_PARSE;
    }

    if (!j.is_object() || !j.contains("id") || !j["id"].is_string()) {
        log_line(4, "manifest missing required string field 'id'");
        return ORCA_ERR_PARSE;
    }

    auto plugin = std::make_unique<LoadedPlugin>();
    plugin->id          = j["id"].get<std::string>();
    plugin->name        = j.value("name",        std::string{});
    plugin->version     = j.value("version",     std::string{});
    plugin->author      = j.value("author",      std::string{});
    plugin->description = j.value("description", std::string{});
    plugin->source_dir  = plugin_dir;

    if (j.contains("permissions") && j["permissions"].is_array()) {
        for (const auto& p : j["permissions"]) {
            if (!p.is_string()) continue;
            plugin->permissions |= permission_bit(p.get<std::string>());
        }
    }

    // --- 2. dedupe -------------------------------------------------------
    {
        std::lock_guard<std::mutex> guard(impl_->mtx);
        if (impl_->plugins.find(plugin->id) != impl_->plugins.end()) {
            log_line(3, ("plugin already loaded: " + plugin->id).c_str());
            return ORCA_ERR_ALREADY_EXISTS;
        }
    }

    // --- 2.5: data-only branch (profile pack plugin) --------------------
    // Plugins shipping only profile data — no native code — declare
    // `"provides": { "profile_dir": "..." }` and OMIT the entry binary.
    // PluginManager loads the profile pack via the engine Presets API and
    // registers an ORCA_SLOT_PROFILE_PACK slot whose vtable carries the
    // absolute dir path (heap-owned via plugin->owned_strings).
    if (j.contains("provides") && j["provides"].is_object()
        && j["provides"].contains("profile_dir") && j["provides"]["profile_dir"].is_string())
    {
        const std::string rel = j["provides"]["profile_dir"].get<std::string>();
        const auto abs_dir = (rel.empty() || std::filesystem::path{rel}.is_absolute())
                             ? std::filesystem::path{rel}
                             : (plugin_dir / rel);
        if (!std::filesystem::is_directory(abs_dir)) {
            log_line(4, ("profile pack dir not found: " + abs_dir.string()).c_str());
            return ORCA_ERR_NOT_FOUND;
        }

        if (impl_->session == nullptr) {
            log_line(4, "no session attached — cannot load profile pack");
            return ORCA_ERR_UNSUPPORTED;
        }

        // Load the vendor configs through the engine Presets API.
        auto load_result = impl_->session->presets().load_vendor_configs_from_json(
            abs_dir, orca::SubstitutionRule::EnableSilent);
        if (!load_result.ok()) {
            log_line(4, ("profile pack load failed: " + load_result.error().message).c_str());
            return ORCA_ERR_IO;
        }

        // Stash the path string somewhere whose lifetime matches the plugin
        // registration so the slot vtable's profile_dir pointer stays valid
        // until unload.
        plugin->owned_strings.push_back(abs_dir.string());
        const std::string& stable_dir = plugin->owned_strings.back();

        // Allocate the slot vtable on the heap (lifetime tied to LoadedPlugin).
        auto vt = std::make_unique<orca_slot_profile_pack_t>();
        vt->struct_size = sizeof(*vt);
        vt->profile_dir = stable_dir.c_str();

        if (impl_->registry) {
            impl_->registry->set_current_plugin_id(plugin->id);
            const auto slot_id = impl_->registry->add_slot(
                ORCA_SLOT_PROFILE_PACK,
                vt.get(),
                /*user_data=*/nullptr,
                /*priority=*/0);
            impl_->registry->clear_current_plugin_id();
            if (slot_id == 0) {
                log_line(4, "profile pack slot registration failed");
                return ORCA_ERR_UNKNOWN;
            }
        }

        // Transfer vtable ownership to the LoadedPlugin so it dies with the
        // plugin record (unload tears slots down per-plugin via PluginRegistry).
        plugin->profile_pack_vtable = std::move(vt);

        // No dlopen. No fn_* exports. Just register the plugin record so
        // unload_plugin can find it.
        std::string id_copy = plugin->id;
        std::string version_copy = plugin->version;
        {
            std::lock_guard<std::mutex> guard(impl_->mtx);
            impl_->load_order.push_back(id_copy);
            impl_->plugins.emplace(id_copy, std::move(plugin));
        }
        log_line(2, ("loaded profile pack " + id_copy + " v" + version_copy).c_str());
        return ORCA_OK;
    }

    // --- 2.7: webview branch --------------------------------------------
    // Plugins declaring "kind": "webview" ship HTML/JS instead of a native
    // binary. PluginManager treats them as metadata-only: the actual UI
    // surface is mounted by Slic3r::GUI::WebViewPluginHost when GUI code
    // iterates loaded plugins. PluginManager records id / version /
    // permissions / source_dir so the manager UI can render the listing
    // and unload_plugin can scrub the record on demand.
    if (j.value("kind", std::string{}) == "webview") {
        // entry.webview must point at a file that exists alongside the
        // manifest; everything else is GUI's problem.
        std::string entry;
        if (j.contains("entry") && j["entry"].is_object() &&
            j["entry"].contains("webview") && j["entry"]["webview"].is_string())
            entry = j["entry"]["webview"].get<std::string>();

        if (entry.empty()) {
            log_line(4, ("webview plugin missing entry.webview: " + plugin->id).c_str());
            return ORCA_ERR_INVALID_ARGUMENT;
        }
        const auto html = plugin_dir / entry;
        if (!std::filesystem::is_regular_file(html)) {
            log_line(4, ("webview entry not found: " + html.string()).c_str());
            return ORCA_ERR_NOT_FOUND;
        }

        std::string id_copy      = plugin->id;
        std::string version_copy = plugin->version;
        {
            std::lock_guard<std::mutex> guard(impl_->mtx);
            impl_->load_order.push_back(id_copy);
            impl_->plugins.emplace(id_copy, std::move(plugin));
        }
        log_line(2, ("loaded webview plugin " + id_copy + " v" + version_copy).c_str());
        return ORCA_OK;
    }

    // --- 2.6: wasm branch ------------------------------------------------
    // Plugins shipping WebAssembly declare `"kind": "wasm"` in their
    // manifest. The entry file is resolved the same way the native branch
    // resolves binaries: <id>_<version>.wasm preferred, plain <id>.wasm
    // as fallback. WasmPlugin::load runs the lifecycle handshake and the
    // resulting unique_ptr is stashed on LoadedPlugin so unload_plugin can
    // drop it (the destructor fires orca_plugin_unregister).
    const bool is_wasm = j.value("kind", std::string{}) == "wasm";
    if (is_wasm) {
        std::filesystem::path wasm_path;
        if (!plugin->version.empty()) {
            const auto v = plugin_dir / (plugin->id + "_" + plugin->version + ".wasm");
            if (std::filesystem::exists(v)) wasm_path = v;
        }
        if (wasm_path.empty()) {
            const auto u = plugin_dir / (plugin->id + ".wasm");
            if (std::filesystem::exists(u)) wasm_path = u;
        }
        if (wasm_path.empty()) {
            log_line(4, ("wasm entry not found in " + plugin_dir.string()).c_str());
            return ORCA_ERR_NOT_FOUND;
        }

        if (!impl_->wasm_host)
            impl_->wasm_host = std::make_unique<wasm::WasmHost>();

        wasm::WasmPlugin::Manifest wm;
        wm.id          = plugin->id;
        wm.version     = plugin->version;
        wm.permissions = plugin->permissions;
        wm.wasm_path   = wasm_path;

        auto wp_res = wasm::WasmPlugin::load(*impl_->wasm_host, wm, impl_->session, impl_->registry);
        if (!wp_res.ok()) {
            log_line(4, ("wasm plugin load failed for " + plugin->id +
                         ": " + wp_res.error().message).c_str());
            switch (wp_res.error().code) {
                case ErrorCode::PermissionDenied: return ORCA_ERR_PERMISSION_DENIED;
                case ErrorCode::InvalidArgument:  return ORCA_ERR_INVALID_ARGUMENT;
                case ErrorCode::NotFound:         return ORCA_ERR_NOT_FOUND;
                case ErrorCode::ParseError:       return ORCA_ERR_PARSE;
                case ErrorCode::Unsupported:      return ORCA_ERR_UNSUPPORTED;
                default:                          return ORCA_ERR_UNKNOWN;
            }
        }
        plugin->wasm_plugin = std::move(wp_res).value();

        std::string id_copy      = plugin->id;
        std::string version_copy = plugin->version;
        {
            std::lock_guard<std::mutex> guard(impl_->mtx);
            impl_->load_order.push_back(id_copy);
            impl_->plugins.emplace(id_copy, std::move(plugin));
        }
        log_line(2, ("loaded wasm plugin " + id_copy + " v" + version_copy).c_str());
        return ORCA_OK;
    }

    // --- 3. resolve entry binary path -----------------------------------
    // Two conventions accepted, tried in order:
    //   (a) <plugin_dir>/<id>_<version><platform_ext>  — versioned, lets a
    //       plugin coexist with another build of itself in the same dir.
    //   (b) <plugin_dir>/<id><platform_ext>           — unversioned fallback,
    //       used by the loader test fixture and simple single-binary plugins.
    std::filesystem::path entry_path;
    if (!plugin->version.empty()) {
        auto versioned = plugin_dir / (plugin->id + "_" + plugin->version + kPlatformExt);
        if (std::filesystem::exists(versioned))
            entry_path = versioned;
    }
    if (entry_path.empty()) {
        const auto unversioned = plugin_dir / (plugin->id + kPlatformExt);
        if (std::filesystem::exists(unversioned))
            entry_path = unversioned;
    }
    if (entry_path.empty()) {
        log_line(4, ("entry binary not found in " + plugin_dir.string()
                     + " (tried " + plugin->id + "_" + plugin->version + kPlatformExt
                     + " and " + plugin->id + kPlatformExt + ")").c_str());
        return ORCA_ERR_NOT_FOUND;
    }

    // --- 4. dlopen ------------------------------------------------------
    plugin->handle = dl_open(entry_path);
    if (!plugin->handle) {
        log_line(4, ("dlopen failed for " + entry_path.string() + ": " + dl_last_error()).c_str());
        return ORCA_ERR_IO;
    }

    // --- 5. resolve the three mandatory exports -------------------------
    plugin->fn_register = reinterpret_cast<orca_plugin_register_fn_t>(
        dl_sym(plugin->handle, "orca_plugin_register"));
    plugin->fn_unregister = reinterpret_cast<orca_plugin_unregister_fn_t>(
        dl_sym(plugin->handle, "orca_plugin_unregister"));
    plugin->fn_check_debug = reinterpret_cast<orca_plugin_check_debug_consistent_fn_t>(
        dl_sym(plugin->handle, "orca_plugin_check_debug_consistent"));

    if (!plugin->fn_register || !plugin->fn_unregister || !plugin->fn_check_debug) {
        log_line(4, ("missing mandatory orca_plugin_* export in " + entry_path.string()).c_str());
        dl_close(plugin->handle);
        return ORCA_ERR_PARSE;
    }

    // --- 6. debug/release consistency check -----------------------------
    // Matches BBLNetworkPlugin.cpp:222-232 — zero means consistent. The
    // engine reports its own status: NDEBUG defined → release (0), else
    // debug (1). Any non-zero return aborts the load.
#if defined(NDEBUG)
    const int engine_is_debug = 0;
#else
    const int engine_is_debug = 1;
#endif
    if (plugin->fn_check_debug(engine_is_debug) != 0) {
        log_line(3, ("debug/release mismatch for " + plugin->id + "; aborting load").c_str());
        dl_close(plugin->handle);
        return ORCA_ERR_UNSUPPORTED;
    }

    // --- 7. ABI handshake -----------------------------------------------
    // Set the current plugin id on the registry BEFORE calling the plugin's
    // register fn so its add_slot / set_manifest calls succeed (the registry
    // refuses ownerless additions).
    if (impl_->registry)
        impl_->registry->set_current_plugin_id(plugin->id);

    auto* registry_opaque =
        reinterpret_cast<orca_plugin_registry_t*>(impl_->registry);

    const orca_error_code_t rc =
        plugin->fn_register(ORCA_PLUGIN_ABI_VERSION,
                            registry_opaque,
                            &impl_->host_vtable);

    // Always clear the current plugin id after the register call returns —
    // subsequent host-side slot operations should not be attributed to it.
    if (impl_->registry)
        impl_->registry->clear_current_plugin_id();

    if (rc != ORCA_OK) {
        log_line(4, ("orca_plugin_register returned non-OK for " + plugin->id).c_str());
        dl_close(plugin->handle);
        return rc;
    }

    // --- success: stash + log -------------------------------------------
    std::string id_copy = plugin->id;
    std::string version_copy = plugin->version;
    {
        std::lock_guard<std::mutex> guard(impl_->mtx);
        impl_->load_order.push_back(id_copy);
        impl_->plugins.emplace(id_copy, std::move(plugin));
    }
    log_line(2, ("loaded plugin " + id_copy + " v" + version_copy).c_str());
    return ORCA_OK;
}

// ---------------------------------------------------------------------------
// unload_plugin — call orca_plugin_unregister, dlclose, drop from maps.
// ---------------------------------------------------------------------------
orca_error_code_t PluginManager::unload_plugin(const std::string& plugin_id)
{
    std::unique_ptr<LoadedPlugin> taken;
    {
        std::lock_guard<std::mutex> guard(impl_->mtx);
        auto it = impl_->plugins.find(plugin_id);
        if (it == impl_->plugins.end())
            return ORCA_ERR_NOT_FOUND;
        taken = std::move(it->second);
        impl_->plugins.erase(it);

        auto lit = std::find(impl_->load_order.begin(), impl_->load_order.end(), plugin_id);
        if (lit != impl_->load_order.end())
            impl_->load_order.erase(lit);
    }

    if (taken->fn_unregister) {
        try {
            taken->fn_unregister();
        } catch (...) {
            log_line(4, ("orca_plugin_unregister threw for " + plugin_id).c_str());
        }
    }

    // Drop every slot the plugin registered. fn_unregister is advisory — the
    // engine owns the registry, so we don't trust the plugin to have cleaned
    // up its own slot entries.
    if (impl_->registry)
        impl_->registry->remove_by_plugin(plugin_id);

    dl_close(taken->handle);
    taken->handle = nullptr;

    log_line(2, ("unloaded plugin " + plugin_id).c_str());
    return ORCA_OK;
}

// ---------------------------------------------------------------------------
// reload_plugin — Phase 3.1 convenience: snapshot the source dir, run a full
// unload, then load the same directory back. Returns NotFound if plugin_id
// is not currently loaded.
// ---------------------------------------------------------------------------
orca_error_code_t PluginManager::reload_plugin(const std::string& plugin_id)
{
    std::filesystem::path source_dir;
    {
        std::lock_guard<std::mutex> guard(impl_->mtx);
        auto it = impl_->plugins.find(plugin_id);
        if (it == impl_->plugins.end())
            return ORCA_ERR_NOT_FOUND;
        source_dir = it->second->source_dir;
    }
    const auto unload_rc = unload_plugin(plugin_id);
    if (unload_rc != ORCA_OK)
        return unload_rc;
    return load_plugin(source_dir);
}

// ---------------------------------------------------------------------------
// unload_all — iterate in reverse load order so plugins that registered
// late (potentially depending on earlier ones) tear down first.
// ---------------------------------------------------------------------------
void PluginManager::unload_all()
{
    std::vector<std::string> order_snapshot;
    {
        std::lock_guard<std::mutex> guard(impl_->mtx);
        order_snapshot = impl_->load_order;
    }
    for (auto it = order_snapshot.rbegin(); it != order_snapshot.rend(); ++it)
        unload_plugin(*it);
}

// ---------------------------------------------------------------------------
// Read-only inspection.
// ---------------------------------------------------------------------------
std::vector<std::string> PluginManager::loaded_plugin_ids() const
{
    std::lock_guard<std::mutex> guard(impl_->mtx);
    return impl_->load_order;
}

bool PluginManager::is_loaded(const std::string& plugin_id) const
{
    std::lock_guard<std::mutex> guard(impl_->mtx);
    return impl_->plugins.find(plugin_id) != impl_->plugins.end();
}

} // namespace orca

// ---------------------------------------------------------------------------
// C ABI registry bridges (declared in engine/include/orca/plugin_api.h).
//
// Plugin shared libraries resolve these symbols against the engine's dynamic
// symbol table — they MUST have C linkage + default visibility. The engine's
// build configuration already exports anything in c_api.cpp/PluginManager.cpp
// via the project-wide -fvisibility=default for libslic3r sources; if you
// later move the engine to -fvisibility=hidden, decorate these with
// __attribute__((visibility("default"))) / __declspec(dllexport).
// ---------------------------------------------------------------------------

extern "C" orca_error_code_t orca_registry_set_manifest(
    orca_plugin_registry_t*       registry,
    const orca_plugin_manifest_t* manifest)
{
    if (!registry || !manifest) return ORCA_ERR_INVALID_ARGUMENT;
    if (manifest->struct_size < sizeof(orca_plugin_manifest_t))
        return ORCA_ERR_INVALID_ARGUMENT;

    auto* reg = reinterpret_cast<orca::PluginRegistry*>(registry);

    orca::StoredManifest m;
    m.id          = manifest->id          ? manifest->id          : "";
    m.name        = manifest->name        ? manifest->name        : "";
    m.version     = manifest->version     ? manifest->version     : "";
    m.author      = manifest->author      ? manifest->author      : "";
    m.description = manifest->description ? manifest->description : "";
    m.permissions = manifest->permissions;

    auto r = reg->set_manifest(m);
    if (!r.ok()) {
        if (r.error().code == orca::ErrorCode::AlreadyExists) return ORCA_ERR_ALREADY_EXISTS;
        if (r.error().code == orca::ErrorCode::InvalidArgument) return ORCA_ERR_INVALID_ARGUMENT;
        return ORCA_ERR_UNKNOWN;
    }
    return ORCA_OK;
}

extern "C" orca_plugin_slot_id_t orca_registry_add_slot(
    orca_plugin_registry_t* registry,
    orca_slot_kind_t        kind,
    const void*             vtable,
    void*                   user_data)
{
    if (!registry) return 0;
    auto* reg = reinterpret_cast<orca::PluginRegistry*>(registry);
    return reg->add_slot(kind, vtable, user_data);
}

extern "C" orca_error_code_t orca_registry_remove_slot(
    orca_plugin_registry_t* registry,
    orca_plugin_slot_id_t   slot_id)
{
    if (!registry) return ORCA_ERR_INVALID_ARGUMENT;
    auto* reg = reinterpret_cast<orca::PluginRegistry*>(registry);
    auto r = reg->remove_slot(slot_id);
    if (!r.ok()) {
        if (r.error().code == orca::ErrorCode::NotFound) return ORCA_ERR_NOT_FOUND;
        return ORCA_ERR_UNKNOWN;
    }
    return ORCA_OK;
}
