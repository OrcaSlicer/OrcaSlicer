#pragma once

// Plugin slot registry. Stores vtable pointers keyed by orca_slot_kind_t.
// Dispatch sites snapshot the entries for a kind under a shared lock so the
// slicing worker thread can iterate without blocking writers. The lifetime
// of the vtable pointer is the plugin's responsibility — the registry only
// stores it; PluginManager guarantees no dispatch happens after unload.
//
// Mirrors the snapshot-under-lock pattern in engine/src/Events.cpp:50-58.

#include "orca/plugin_api.h"
#include "orca/Result.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace orca {

struct SlotEntry {
    orca_plugin_slot_id_t slot_id     = 0;
    std::string           plugin_id;
    orca_slot_kind_t      kind        = ORCA_SLOT_PIPELINE_OBSERVER;
    const void*           vtable      = nullptr;
    void*                 user_data   = nullptr;
    int                   priority    = 0;
};

struct StoredManifest {
    std::string id;
    std::string name;
    std::string version;
    std::string author;
    std::string description;
    uint64_t    permissions = 0;
};

class PluginRegistry {
public:
    PluginRegistry();
    ~PluginRegistry();
    PluginRegistry(const PluginRegistry&)            = delete;
    PluginRegistry& operator=(const PluginRegistry&) = delete;

    // Each call from a plugin's orca_plugin_register MUST set a plugin id
    // first so the registry knows who owns the slot. PluginManager calls
    // set_current_plugin_id around the register call.
    void set_current_plugin_id(std::string id);
    void clear_current_plugin_id();

    Result<void> set_manifest(const StoredManifest& m);
    std::vector<StoredManifest> manifests() const;

    // Returns 0 on failure; otherwise a fresh non-zero slot id.
    orca_plugin_slot_id_t add_slot(orca_slot_kind_t kind,
                                   const void* vtable,
                                   void* user_data,
                                   int priority = 0);

    Result<void> remove_slot(orca_plugin_slot_id_t slot_id);
    void         remove_by_plugin(const std::string& plugin_id);

    // Snapshot the entries for a given kind. Ordering: ascending priority,
    // ties broken by slot_id.
    std::vector<SlotEntry> snapshot(orca_slot_kind_t kind) const;

    // Total number of registered slots (across all kinds). For tests.
    std::size_t slot_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace orca
