#pragma once

// Plugin loader. Discovers <data_dir>/plugins/<id>/manifest.json entries,
// dlopens the entry binary, resolves the three mandatory orca_plugin_*
// exports, runs the debug/release sanity check + ABI handshake, and tracks
// loaded plugins. Mirrors the production pattern in
// src/slic3r/Utils/BBLNetworkPlugin.cpp.
//
// Phase 1.2.1: skeleton + native (.so/.dll/.dylib) loader.
// Phase 1.2.2 owns PluginRegistry; Phase 2 wires real per-slot dispatch.

#include "orca/plugin_api.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace orca {

class Session;
class PluginRegistry;

class PluginManager {
public:
    PluginManager();
    ~PluginManager();
    PluginManager(const PluginManager&)            = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    void bind_session(Session* session, PluginRegistry* registry);

    // Walks <plugins_dir>/<id>/manifest.json entries and loads each one.
    // Returns the number of plugins successfully loaded.
    std::size_t discover_and_load(const std::filesystem::path& plugins_dir);

    // Load a specific plugin by directory (must contain manifest.json + entry binary).
    // Returns ORCA_OK on success.
    orca_error_code_t load_plugin(const std::filesystem::path& plugin_dir);

    // Unload by id. ORCA_ERR_NOT_FOUND if no such plugin loaded.
    orca_error_code_t unload_plugin(const std::string& plugin_id);

    /// Phase 3.1 — convenience: unload + reload from the cached source dir.
    /// Returns NotFound if plugin_id is not currently loaded.
    orca_error_code_t reload_plugin(const std::string& plugin_id);

    // Unload all in reverse load order.
    void unload_all();

    // Read-only inspection.
    std::vector<std::string> loaded_plugin_ids() const;
    bool                     is_loaded(const std::string& plugin_id) const;

    // pImpl is forward-declared as public so the .cpp's file-local C thunks
    // (which need a stable static dispatch target for the orca_plugin_host_t
    // vtable) can name the type. The definition lives in PluginManager.cpp;
    // no other TU sees its members.
    struct Impl;

private:
    struct LoadedPlugin;
    std::unique_ptr<Impl> impl_;
};

} // namespace orca
