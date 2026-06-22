#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// Pulling in the service headers here makes `#include "orca/Session.hpp"` the
// single entry point a consumer needs — they can call session.presets().X()
// without a second include. The headers are themselves cheap (forward-decls
// of libslic3r types + small POD vocabulary types). If a consumer wants the
// minimal surface they can include the specific service header directly.
#include "Presets.hpp"
#include "Project.hpp"
#include "Slicer.hpp"
#include "Export.hpp"
#include "Events.hpp"
#include "PrinterAgent.hpp"
#include "Result.hpp"
#include "plugin_api.h"

// Forward declarations of libslic3r types referenced by the migration-scaffold
// attach_* methods below. Phase 0.4a–0.6 use these to BORROW GUI/CLI-owned
// objects; Phase 1 collapses ownership into Session.
namespace Slic3r {
class PresetBundle;
class Model;
} // namespace Slic3r

namespace orca {

class PluginRegistry;

// Top-level entry point to the orca engine. Owns the four services and the
// event bus; all engine state lives behind this object. A single long-lived
// instance is the expected usage; multi-instance is supported but untested.
class Session {
public:
    static std::unique_ptr<Session> create();
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&)                 = delete;
    Session& operator=(Session&&)      = delete;

    Presets&        presets();
    const Presets&  presets() const;

    Project&        project();
    const Project&  project() const;

    Slicer&         slicer();
    const Slicer&   slicer() const;

    Exporter&       exporter();
    const Exporter& exporter() const;

    Events&         events();
    const Events&   events() const;

    // ---------- Migration scaffold (Phase 0.4a–0.6) ----------
    //
    // During the incremental migration, the GUI/CLI continues to own the
    // PresetBundle and Model. Session BORROWS them via these setters; the
    // wrappers in presets()/project() then route into the borrowed objects
    // so rewritten call sites compile and run identically.
    //
    // Phase 1 collapses ownership into Session and removes these setters.
    void attach_preset_bundle(Slic3r::PresetBundle* bundle);
    void detach_preset_bundle();

    void attach_model(Slic3r::Model* model);
    void detach_model();

    // ---------- Plugin host (Phase 1) ----------
    //
    // The Session owns a PluginManager (loader) and a PluginRegistry (slot
    // dispatch table). Both are engine-internal C++ types; consumers drive
    // them through these Session methods or the C ABI in plugin_api.h.

    // Walks <plugins_dir>/<id>/manifest.json entries and loads each native
    // plugin found. Returns the number of plugins successfully loaded.
    std::size_t discover_and_load_plugins(const std::filesystem::path& plugins_dir);

    // Load a single plugin by directory. ORCA_OK on success.
    Result<void> load_plugin(const std::filesystem::path& plugin_dir);

    // Unload by id; ORCA_ERR_NOT_FOUND if not loaded.
    Result<void> unload_plugin(const std::string& plugin_id);

    /// Convenience: unload + reload from the original directory. Returns
    /// NotFound if plugin_id is not currently loaded.
    Result<void> reload_plugin(const std::string& plugin_id);

    void unload_all_plugins();

    std::vector<std::string> loaded_plugin_ids() const;
    bool                     is_plugin_loaded(const std::string& plugin_id) const;

    /// Public-shape mirror of the engine-internal StoredManifest. Used by
    /// UI consumers (PluginManagerDialog) that don't want to drag
    /// engine/src/ into their include path.
    struct ManifestInfo {
        std::string   id;
        std::string   name;
        std::string   version;
        std::string   author;
        std::string   description;
        std::uint64_t permissions = 0;
    };
    std::vector<ManifestInfo> plugin_manifests() const;

    /// Public-shape mirror of the engine-internal SlotEntry for UI
    /// dispatchers that need to iterate slots by kind without dragging
    /// engine/src/ headers into their include path.
    struct PluginSlotInfo {
        std::uint64_t slot_id   = 0;
        std::string   plugin_id;
        std::uint32_t kind      = 0;    // orca_slot_kind_t value
        const void*   vtable    = nullptr;
        void*         user_data = nullptr;
        int           priority  = 0;
    };
    std::vector<PluginSlotInfo> plugin_slots_of(std::uint32_t kind) const;

    // Read-only count of registered slots across all kinds. Useful for tests.
    std::size_t registered_slot_count() const;

    /// Direct access to the slot dispatch table — engine-internal type;
    /// callers must include "engine/src/PluginRegistry.hpp" to use it.
    /// Used by Slicer to snapshot pipeline observer/interceptor slots
    /// at slice-start time.
    PluginRegistry&       plugin_registry();
    const PluginRegistry& plugin_registry() const;

    // ---------- Printer agents (Phase 2.4) ----------
    //
    // Printer agents are plugin-provided implementations of the
    // orca::PrinterAgent C++ interface, exposed through the
    // ORCA_SLOT_PRINTER_AGENT slot. The Session enumerates the registered
    // agent kinds and instantiates them on demand; the returned object owns
    // the underlying plugin-allocated instance and tears it down on
    // destruction.

    /// List the printer agent kinds currently registered through any plugin's
    /// ORCA_SLOT_PRINTER_AGENT slot. Snapshot semantics — the returned vector
    /// is a point-in-time view; mid-call (un)registration is invisible.
    std::vector<PrinterAgentInfo> list_printer_agents() const;

    /// True if at least one ORCA_SLOT_PRINTER_AGENT slot matches agent_id.
    bool has_printer_agent(const std::string& agent_id) const;

    /// Instantiate the printer agent registered under agent_id. The returned
    /// pointer is ready to use — connect / send_command / etc. apply directly.
    /// Returns InvalidArgument if agent_id is empty, NotFound if no slot
    /// matches, InvalidState if the plugin's create_instance returned NULL.
    Result<std::unique_ptr<PrinterAgent>>
    create_printer_agent(const std::string& agent_id);

    /// Convenience helper used by in-tree printer agent registrations to avoid
    /// taking a dependency on the engine-internal PluginRegistry header. Runs
    /// the set_current_plugin_id / add_slot / clear_current_plugin_id sequence
    /// against the contained PluginRegistry. Returns the new slot id, or 0 on
    /// failure (matches PluginRegistry::add_slot's failure convention).
    orca_plugin_slot_id_t add_printer_agent_slot(
        const std::string&               owning_plugin_id,
        const orca_slot_printer_agent_t* vtable,
        void*                            user_data,
        int                              priority = 0);

private:
    Session();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace orca
