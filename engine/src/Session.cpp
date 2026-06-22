// Session — top-level engine entry point.
//
// Owns the four services + the event bus. Borrows GUI/CLI-owned PresetBundle
// and Model via attach_*() during the Phase 0.4a–0.6 migration; Phase 1
// collapses ownership in here.

#include "orca/Session.hpp"

#include "orca/Events.hpp"
#include "orca/Export.hpp"
#include "orca/Presets.hpp"
#include "orca/Project.hpp"
#include "orca/Slicer.hpp"

#include "PluginManager.hpp"
#include "PluginRegistry.hpp"
#include "PrinterAgentAdapter.hpp"

#include "orca/plugin_api.h"

#include <utility>

namespace orca {

// Project::Impl carries the borrowed Slic3r::Model* — we reach in via a
// trivial friend-equivalent helper so Session can wire attach_model. The
// helper is a private member of Project (added inline below as a free
// function in this TU), keeping the bridge code local to Session.cpp.
// We declare the body here as a friend-style backdoor; Project.hpp does not
// need to grow a public setter.

namespace detail {
// Trampolines defined alongside the service impls — see Presets.cpp / Project.cpp
// for the bundle/model borrow accessors. Session wires through them.
} // namespace detail

struct Session::Impl {
    Presets        presets;
    Project        project;
    Slicer         slicer;
    Exporter       exporter;
    Events         events;
    PluginRegistry registry;
    PluginManager  manager;
};

std::unique_ptr<Session> Session::create() {
    // Can't use std::make_unique because the ctor is private and friend is
    // limited to this class.
    return std::unique_ptr<Session>(new Session());
}

Session::Session() : impl_(std::make_unique<Impl>()) {
    impl_->presets.bind_session(this);
    impl_->project.bind_session(this);
    impl_->slicer.bind_session(this);
    impl_->exporter.bind_session(this);
    impl_->manager.bind_session(this, &impl_->registry);
}
Session::~Session() {
    // Unload plugins BEFORE the rest of Session::Impl tears down so plugin
    // destructors can still reach the bus / presets / etc. through the host
    // vtable. If we let unique_ptr destruction order run naturally the manager
    // would unload after `events` is gone.
    impl_->manager.unload_all();
}

Presets&       Session::presets()       { return impl_->presets; }
const Presets& Session::presets() const { return impl_->presets; }

Project&       Session::project()       { return impl_->project; }
const Project& Session::project() const { return impl_->project; }

Slicer&        Session::slicer()        { return impl_->slicer; }
const Slicer&  Session::slicer() const  { return impl_->slicer; }

Exporter&       Session::exporter()       { return impl_->exporter; }
const Exporter& Session::exporter() const { return impl_->exporter; }

Events&        Session::events()        { return impl_->events; }
const Events&  Session::events() const  { return impl_->events; }

void Session::attach_preset_bundle(Slic3r::PresetBundle* bundle) {
    impl_->presets.attach_bundle(bundle);
}

void Session::detach_preset_bundle() {
    impl_->presets.detach_bundle();
}

void Session::attach_model(Slic3r::Model* model) {
    impl_->project.attach_model(model);
}

void Session::detach_model() {
    impl_->project.detach_model();
}

// ---------- Plugin host (Phase 1) ----------

std::size_t Session::discover_and_load_plugins(const std::filesystem::path& plugins_dir) {
    return impl_->manager.discover_and_load(plugins_dir);
}

Result<void> Session::load_plugin(const std::filesystem::path& plugin_dir) {
    orca_error_code_t rc = impl_->manager.load_plugin(plugin_dir);
    if (rc == ORCA_OK) return ok();
    // Map the C ABI code back to a C++ Result. The detailed message is on the
    // PluginManager side (logged); the Result here carries only the code.
    ErrorCode code = ErrorCode::Unknown;
    switch (rc) {
        case ORCA_ERR_INVALID_ARGUMENT: code = ErrorCode::InvalidArgument; break;
        case ORCA_ERR_NOT_FOUND:        code = ErrorCode::NotFound;        break;
        case ORCA_ERR_ALREADY_EXISTS:   code = ErrorCode::AlreadyExists;   break;
        case ORCA_ERR_IO:               code = ErrorCode::IoError;         break;
        case ORCA_ERR_PARSE:            code = ErrorCode::ParseError;      break;
        case ORCA_ERR_UNSUPPORTED:      code = ErrorCode::Unsupported;     break;
        default:                        code = ErrorCode::Unknown;         break;
    }
    return Result<void>{Error{code, "Session::load_plugin failed"}};
}

Result<void> Session::unload_plugin(const std::string& plugin_id) {
    orca_error_code_t rc = impl_->manager.unload_plugin(plugin_id);
    if (rc == ORCA_OK) return ok();
    return Result<void>{Error{
        rc == ORCA_ERR_NOT_FOUND ? ErrorCode::NotFound : ErrorCode::Unknown,
        "Session::unload_plugin failed"}};
}

Result<void> Session::reload_plugin(const std::string& plugin_id) {
    orca_error_code_t rc = impl_->manager.reload_plugin(plugin_id);
    if (rc == ORCA_OK) return ok();
    // Mirror load_plugin's full code mapping — reload re-runs the load
    // sequence so any of its failure codes can propagate here.
    ErrorCode code = ErrorCode::Unknown;
    switch (rc) {
        case ORCA_ERR_INVALID_ARGUMENT: code = ErrorCode::InvalidArgument; break;
        case ORCA_ERR_NOT_FOUND:        code = ErrorCode::NotFound;        break;
        case ORCA_ERR_ALREADY_EXISTS:   code = ErrorCode::AlreadyExists;   break;
        case ORCA_ERR_IO:               code = ErrorCode::IoError;         break;
        case ORCA_ERR_PARSE:            code = ErrorCode::ParseError;      break;
        case ORCA_ERR_UNSUPPORTED:      code = ErrorCode::Unsupported;     break;
        default:                        code = ErrorCode::Unknown;         break;
    }
    return Result<void>{Error{code, "Session::reload_plugin failed"}};
}

void Session::unload_all_plugins() {
    impl_->manager.unload_all();
}

std::vector<std::string> Session::loaded_plugin_ids() const {
    return impl_->manager.loaded_plugin_ids();
}

bool Session::is_plugin_loaded(const std::string& plugin_id) const {
    return impl_->manager.is_loaded(plugin_id);
}

std::size_t Session::registered_slot_count() const {
    return impl_->registry.slot_count();
}

std::vector<Session::PluginSlotInfo>
Session::plugin_slots_of(std::uint32_t kind) const {
    std::vector<PluginSlotInfo> out;
    for (const auto& e : impl_->registry.snapshot(
             static_cast<orca_slot_kind_t>(kind))) {
        out.push_back(PluginSlotInfo{
            e.slot_id, e.plugin_id,
            static_cast<std::uint32_t>(e.kind),
            e.vtable, e.user_data, e.priority});
    }
    return out;
}

std::vector<Session::ManifestInfo> Session::plugin_manifests() const {
    std::vector<Session::ManifestInfo> out;
    for (const auto& m : impl_->registry.manifests()) {
        out.push_back(ManifestInfo{
            m.id, m.name, m.version, m.author, m.description, m.permissions});
    }
    return out;
}

PluginRegistry& Session::plugin_registry() {
    return impl_->registry;
}

const PluginRegistry& Session::plugin_registry() const {
    return impl_->registry;
}

// ---------- Printer agents (Phase 2.4) ----------

namespace {

PrinterAgentInfo info_from_vtable(const orca_slot_printer_agent_t* vt) {
    PrinterAgentInfo info;
    info.id          = (vt->agent_id          != nullptr) ? vt->agent_id          : "";
    info.name        = (vt->agent_name        != nullptr) ? vt->agent_name        : "";
    info.version     = (vt->agent_version     != nullptr) ? vt->agent_version     : "";
    info.description = (vt->agent_description != nullptr) ? vt->agent_description : "";
    return info;
}

} // namespace

std::vector<PrinterAgentInfo> Session::list_printer_agents() const {
    std::vector<PrinterAgentInfo> out;
    const auto snap = impl_->registry.snapshot(ORCA_SLOT_PRINTER_AGENT);
    out.reserve(snap.size());
    for (const auto& e : snap) {
        const auto* vt = static_cast<const orca_slot_printer_agent_t*>(e.vtable);
        if (vt == nullptr || vt->agent_id == nullptr)
            continue;
        out.push_back(info_from_vtable(vt));
    }
    return out;
}

bool Session::has_printer_agent(const std::string& agent_id) const {
    if (agent_id.empty())
        return false;
    const auto snap = impl_->registry.snapshot(ORCA_SLOT_PRINTER_AGENT);
    for (const auto& e : snap) {
        const auto* vt = static_cast<const orca_slot_printer_agent_t*>(e.vtable);
        if (vt != nullptr && vt->agent_id != nullptr && agent_id == vt->agent_id)
            return true;
    }
    return false;
}

Result<std::unique_ptr<PrinterAgent>>
Session::create_printer_agent(const std::string& agent_id) {
    if (agent_id.empty())
        return err<std::unique_ptr<PrinterAgent>>(
            ErrorCode::InvalidArgument, "printer agent id is empty");

    const auto snap = impl_->registry.snapshot(ORCA_SLOT_PRINTER_AGENT);
    for (const auto& e : snap) {
        const auto* vt = static_cast<const orca_slot_printer_agent_t*>(e.vtable);
        if (vt == nullptr || vt->agent_id == nullptr || agent_id != vt->agent_id)
            continue;

        PrinterAgentInfo info = info_from_vtable(vt);
        auto adapter = std::make_unique<PrinterAgentAdapter>(
            vt, e.user_data, std::move(info));
        auto init = adapter->initialize();
        if (!init.ok())
            return err<std::unique_ptr<PrinterAgent>>(
                init.error().code, init.error().message);
        return Result<std::unique_ptr<PrinterAgent>>(
            std::unique_ptr<PrinterAgent>(std::move(adapter)));
    }

    return err<std::unique_ptr<PrinterAgent>>(
        ErrorCode::NotFound, "no printer agent registered under that id");
}

orca_plugin_slot_id_t Session::add_printer_agent_slot(
    const std::string&               owning_plugin_id,
    const orca_slot_printer_agent_t* vtable,
    void*                            user_data,
    int                              priority) {
    if (vtable == nullptr)
        return 0;
    impl_->registry.set_current_plugin_id(owning_plugin_id);
    const orca_plugin_slot_id_t id = impl_->registry.add_slot(
        ORCA_SLOT_PRINTER_AGENT, vtable, user_data, priority);
    impl_->registry.clear_current_plugin_id();
    return id;
}

} // namespace orca
