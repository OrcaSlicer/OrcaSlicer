// PluginRegistry — slot vtable storage keyed by orca_slot_kind_t.
//
// Snapshot-under-lock dispatch pattern: snapshot() copies the slot vector for
// a given kind under a shared lock and returns it by value, so the slicing
// worker thread can iterate without holding any lock and without being
// invalidated by concurrent add/remove. Mirrors engine/src/Events.cpp:50-58.
//
// Lifetime: vtable pointers are owned by the plugin; PluginManager guarantees
// remove_by_plugin runs before the plugin's shared library is unloaded so
// the registry never dereferences a dangling vtable.

#include "PluginRegistry.hpp"

#include <algorithm>
#include <utility>

namespace orca {

struct PluginRegistry::Impl {
    mutable std::shared_mutex                mtx;
    std::atomic<orca_plugin_slot_id_t>       next_id{1};  // 0 reserved invalid

    std::unordered_map<orca_slot_kind_t, std::vector<SlotEntry>> by_kind;
    std::unordered_map<orca_plugin_slot_id_t, orca_slot_kind_t>  id_to_kind;
    std::unordered_map<std::string, StoredManifest>              manifests_by_id;

    // Set by PluginManager around each plugin's orca_plugin_register call so
    // the registry knows who owns the slot being added.
    std::string current_plugin_id;
};

PluginRegistry::PluginRegistry()  : impl_(std::make_unique<Impl>()) {}
PluginRegistry::~PluginRegistry() = default;

void PluginRegistry::set_current_plugin_id(std::string id) {
    std::unique_lock<std::shared_mutex> lock(impl_->mtx);
    impl_->current_plugin_id = std::move(id);
}

void PluginRegistry::clear_current_plugin_id() {
    std::unique_lock<std::shared_mutex> lock(impl_->mtx);
    impl_->current_plugin_id.clear();
}

Result<void> PluginRegistry::set_manifest(const StoredManifest& m) {
    std::unique_lock<std::shared_mutex> lock(impl_->mtx);
    if (impl_->current_plugin_id.empty()) {
        return err<void>(ErrorCode::InvalidArgument,
                         "set_manifest called without an active plugin id");
    }
    if (impl_->current_plugin_id != m.id) {
        return err<void>(ErrorCode::InvalidArgument,
                         "manifest id does not match the active plugin id");
    }
    impl_->manifests_by_id[m.id] = m;
    return ok();
}

std::vector<StoredManifest> PluginRegistry::manifests() const {
    std::shared_lock<std::shared_mutex> lock(impl_->mtx);
    std::vector<StoredManifest> out;
    out.reserve(impl_->manifests_by_id.size());
    for (const auto& kv : impl_->manifests_by_id) {
        out.push_back(kv.second);
    }
    return out;
}

orca_plugin_slot_id_t PluginRegistry::add_slot(orca_slot_kind_t kind,
                                               const void* vtable,
                                               void* user_data,
                                               int priority) {
    std::unique_lock<std::shared_mutex> lock(impl_->mtx);
    if (impl_->current_plugin_id.empty()) {
        // PluginManager forgot to bind a plugin id. Refuse to store an
        // unowned slot — it could never be cleaned up on unload.
        return 0;
    }
    const orca_plugin_slot_id_t slot_id =
        impl_->next_id.fetch_add(1, std::memory_order_relaxed);

    SlotEntry entry;
    entry.slot_id   = slot_id;
    entry.plugin_id = impl_->current_plugin_id;
    entry.kind      = kind;
    entry.vtable    = vtable;
    entry.user_data = user_data;
    entry.priority  = priority;

    impl_->by_kind[kind].push_back(std::move(entry));
    impl_->id_to_kind[slot_id] = kind;
    return slot_id;
}

Result<void> PluginRegistry::remove_slot(orca_plugin_slot_id_t slot_id) {
    std::unique_lock<std::shared_mutex> lock(impl_->mtx);
    auto it = impl_->id_to_kind.find(slot_id);
    if (it == impl_->id_to_kind.end()) {
        return err<void>(ErrorCode::NotFound, "slot id not registered");
    }
    const orca_slot_kind_t kind = it->second;
    auto vec_it = impl_->by_kind.find(kind);
    if (vec_it != impl_->by_kind.end()) {
        auto& vec = vec_it->second;
        vec.erase(std::remove_if(vec.begin(), vec.end(),
                                 [slot_id](const SlotEntry& e) {
                                     return e.slot_id == slot_id;
                                 }),
                  vec.end());
    }
    impl_->id_to_kind.erase(it);
    return ok();
}

void PluginRegistry::remove_by_plugin(const std::string& plugin_id) {
    std::unique_lock<std::shared_mutex> lock(impl_->mtx);

    // Collect every slot id we need to drop before mutating id_to_kind so we
    // do not invalidate iterators while walking it.
    std::vector<orca_plugin_slot_id_t> dropped;
    for (auto& kv : impl_->by_kind) {
        auto& vec = kv.second;
        for (const auto& e : vec) {
            if (e.plugin_id == plugin_id) {
                dropped.push_back(e.slot_id);
            }
        }
        vec.erase(std::remove_if(vec.begin(), vec.end(),
                                 [&plugin_id](const SlotEntry& e) {
                                     return e.plugin_id == plugin_id;
                                 }),
                  vec.end());
    }
    for (auto id : dropped) {
        impl_->id_to_kind.erase(id);
    }
    impl_->manifests_by_id.erase(plugin_id);
}

std::vector<SlotEntry> PluginRegistry::snapshot(orca_slot_kind_t kind) const {
    // Copy the vector by value under a shared lock; the caller iterates the
    // copy without holding any registry lock. This is the hot-path entry
    // point hit from the slicing worker thread.
    std::vector<SlotEntry> snap;
    {
        std::shared_lock<std::shared_mutex> lock(impl_->mtx);
        auto it = impl_->by_kind.find(kind);
        if (it == impl_->by_kind.end()) return snap;
        snap = it->second;
    }
    std::stable_sort(snap.begin(), snap.end(),
                     [](const SlotEntry& a, const SlotEntry& b) {
                         if (a.priority != b.priority) return a.priority < b.priority;
                         return a.slot_id < b.slot_id;
                     });
    return snap;
}

std::size_t PluginRegistry::slot_count() const {
    std::shared_lock<std::shared_mutex> lock(impl_->mtx);
    std::size_t total = 0;
    for (const auto& kv : impl_->by_kind) {
        total += kv.second.size();
    }
    return total;
}

} // namespace orca
