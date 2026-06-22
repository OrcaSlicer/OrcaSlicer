// Presets — engine-side ConfigService.
//
// Phase 0.4a deliverable: a working wrapper around the GUI/CLI-owned
// Slic3r::PresetBundle. The migration-scaffold accessor raw() exposes the
// underlying bundle so Transform A can rewrite call sites uniformly:
//
//     wxGetApp().preset_bundle->X        ->   session.presets().raw().X
//
// The typed DTO/scope API (opt<T>, set<T>, list, current, …) declared in
// Presets.hpp is the long-term surface — implementations are filled in as
// rewriter passes need them. For 0.4a all that needs to LINK is raw(),
// attach/detach, has_bundle, and the predicate pass-throughs; the rest are
// either stubs returning defaults or will be added on demand.

#include "orca/Presets.hpp"
#include "orca/Events.hpp"
#include "orca/EventTypes.hpp"
#include "orca/Session.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <cassert>
#include <exception>
#include <memory>
#include <string>

namespace orca {

struct Presets::Impl {
    Slic3r::PresetBundle* bundle  = nullptr;  // borrowed; not owned
    Session*              session = nullptr;  // borrowed back-pointer for events()

    // Owned snapshot for ConfigScope::Full reads. full_config() returns a
    // value type; we stash it here so config_for_scope(Full) can hand out a
    // stable pointer for the duration of the const-call. mutable so const
    // methods can refresh it.
    mutable std::unique_ptr<Slic3r::DynamicPrintConfig> full_snapshot;
};

Presets::Presets() : impl_(std::make_unique<Impl>()) {}
Presets::~Presets() = default;

void Presets::bind_session(Session* session) {
    impl_->session = session;
}

void Presets::attach_bundle(Slic3r::PresetBundle* bundle) {
    impl_->bundle = bundle;

    // Publish a PresetChanged for each preset type whose "current" name just
    // became valid. attach_bundle is the moment the engine's preset state
    // transitions from "none" to "loaded" — downstream consumers (plugins,
    // GUI sync, tests) treat this as the canonical initial publish. Skip on
    // detach (bundle == nullptr): the "what's current" contract has no
    // meaningful answer when nothing is attached.
    if (bundle != nullptr && impl_->session != nullptr) {
        Events& bus = impl_->session->events();
        bus.publish(PresetChanged{PresetType::Print,    bundle->prints.get_selected_preset_name()});
        bus.publish(PresetChanged{PresetType::Printer,  bundle->printers.get_selected_preset_name()});
        bus.publish(PresetChanged{PresetType::Filament, bundle->filaments.get_selected_preset_name()});
    }
}

void Presets::detach_bundle() {
    impl_->bundle = nullptr;
}

bool Presets::has_bundle() const noexcept {
    return impl_->bundle != nullptr;
}

// Resolve a ConfigScope to the underlying DynamicPrintConfig. Phase 0.4d
// typed-accessor templates consume this; the GUI-borrowed PresetBundle is
// the source of truth for every Scope value here.
//
// `Full` returns a heap-allocated snapshot held inside Impl so the returned
// pointer stays valid for the duration of the call — full_config() is a
// composed view that's recomputed on demand, and we don't want to hand out
// references to a stack temporary. Phase 1 may swap this for a cached
// invalidation-tracked snapshot.
const Slic3r::DynamicPrintConfig* Presets::config_for_scope(ConfigScope scope) const {
    if (!impl_->bundle) return nullptr;
    auto& b = *impl_->bundle;
    switch (scope) {
        case ConfigScope::Full:
            impl_->full_snapshot = std::make_unique<Slic3r::DynamicPrintConfig>(b.full_config());
            return impl_->full_snapshot.get();
        case ConfigScope::Project:
            return &b.project_config;
        case ConfigScope::PrintPreset:
            return &b.prints.get_edited_preset().config;
        case ConfigScope::PrinterPreset:
            return &b.printers.get_edited_preset().config;
        case ConfigScope::FilamentPreset:
            return &b.filaments.get_edited_preset().config;
    }
    return nullptr;
}

Slic3r::DynamicPrintConfig* Presets::config_for_scope(ConfigScope scope) {
    // Writes need a mutable view. Reuse the const variant for the read paths;
    // for write paths, return mutable pointers directly. Full is intentionally
    // NOT writable through this path — full_config() is a derived view, and
    // writing to its snapshot wouldn't propagate. Writers must target a
    // specific preset's config or project_config.
    if (!impl_->bundle) return nullptr;
    auto& b = *impl_->bundle;
    switch (scope) {
        case ConfigScope::Full:           return nullptr;  // not writable
        case ConfigScope::Project:        return &b.project_config;
        case ConfigScope::PrintPreset:    return &b.prints.get_edited_preset().config;
        case ConfigScope::PrinterPreset:  return &b.printers.get_edited_preset().config;
        case ConfigScope::FilamentPreset: return &b.filaments.get_edited_preset().config;
    }
    return nullptr;
}

Slic3r::PresetBundle* Presets::raw_ptr() {
    // Returns the borrowed pointer directly. Unlike raw(), no assertion —
    // a null bundle is part of the engine's bootstrap state (call sites that
    // happen before attach are expected to be rare and explicit).
    return impl_->bundle;
}

const Slic3r::PresetBundle* Presets::raw_ptr() const {
    return impl_->bundle;
}

Slic3r::PresetBundle& Presets::raw() {
    assert(impl_->bundle && "Presets::raw() called before attach_preset_bundle()");
    return *impl_->bundle;
}

const Slic3r::PresetBundle& Presets::raw() const {
    assert(impl_->bundle && "Presets::raw() called before attach_preset_bundle()");
    return *impl_->bundle;
}

// ---------- BBL / vendor predicates ----------

bool Presets::is_bbl_vendor() const {
    return impl_->bundle && const_cast<Slic3r::PresetBundle*>(impl_->bundle)->is_bbl_vendor();
}

bool Presets::use_bbl_network() const {
    return impl_->bundle && const_cast<Slic3r::PresetBundle*>(impl_->bundle)->use_bbl_network();
}

bool Presets::use_bbl_device_tab() const {
    return impl_->bundle && const_cast<Slic3r::PresetBundle*>(impl_->bundle)->use_bbl_device_tab();
}

// ---------- DTO surface (long-term, filled in as rewriters need them) ----------

std::vector<PresetRef> Presets::list(PresetType /*type*/) const {
    // TODO(0.4a.5): implement when Transform B rewrites preset enumeration sites.
    return {};
}

PresetRef Presets::current(PresetType /*type*/) const {
    return {};
}

int Presets::extruder_count() const {
    return impl_->bundle ? impl_->bundle->get_printer_extruder_count() : 0;
}

int Presets::filament_count() const {
    return impl_->bundle ? static_cast<int>(impl_->bundle->filament_presets.size()) : 0;
}

std::vector<std::string> Presets::filament_names() const {
    if (!impl_->bundle) return {};
    return impl_->bundle->filament_presets;
}

std::vector<VendorRef> Presets::vendors() const {
    if (!impl_->bundle) return {};
    std::vector<VendorRef> out;
    out.reserve(impl_->bundle->vendors.size());
    for (const auto& kv : impl_->bundle->vendors) {
        VendorRef ref;
        ref.id     = kv.second.id;
        ref.name   = kv.second.name;
        ref.is_bbl = (kv.second.id == "BBL");
        out.push_back(std::move(ref));
    }
    return out;
}

std::vector<PresetRef> Presets::physical_printer_list() const {
    return {};
}

// ---------- Write surface (stubs — Transform B fills these in) ----------

Result<void> Presets::set_current(PresetType type, std::string_view name) {
    // Phase 1.3.1: publish the intent on the bus so subscribers can observe
    // the *attempt* (this lets tests verify the bus wiring before Phase 2
    // fills in the real selection logic). Guarded on session + bundle so the
    // bootstrap state (no Session, no PresetBundle attached) stays silent.
    if (impl_->session != nullptr && impl_->bundle != nullptr) {
        impl_->session->events().publish(PresetChanged{type, std::string(name)});
    }
    return err<void>(ErrorCode::NotImplemented, "Presets::set_current not yet implemented");
}

Result<void> Presets::set_filament(int /*extruder_idx*/, std::string_view name) {
    // Phase 1.3.1: publish the intent on the bus (Filament-typed) before the
    // NotImplemented return — same rationale as set_current.
    if (impl_->session != nullptr && impl_->bundle != nullptr) {
        impl_->session->events().publish(PresetChanged{PresetType::Filament, std::string(name)});
    }
    return err<void>(ErrorCode::NotImplemented, "Presets::set_filament not yet implemented");
}

Result<void> Presets::set_num_filaments(int count, std::string_view default_color) {
    if (!impl_->bundle) return err<void>(ErrorCode::InvalidState, "no PresetBundle attached");
    impl_->bundle->set_num_filaments(static_cast<unsigned int>(count), std::string(default_color));
    return ok();
}

Result<void> Presets::set_num_filaments(int count, std::vector<std::string> colors) {
    if (!impl_->bundle) return err<void>(ErrorCode::InvalidState, "no PresetBundle attached");
    impl_->bundle->set_num_filaments(static_cast<unsigned int>(count), std::move(colors));
    return ok();
}

Result<void> Presets::select_physical_printer(std::string_view /*name*/) {
    return err<void>(ErrorCode::NotImplemented, "Presets::select_physical_printer not yet implemented");
}

// ---------- Lifecycle ----------

Result<std::vector<PresetSubstitution>> Presets::load_user_presets(
    std::string_view  /*user*/,
    SubstitutionRule  /*rule*/)
{
    return err<std::vector<PresetSubstitution>>(
        ErrorCode::NotImplemented, "Presets::load_user_presets not yet implemented");
}

Result<LoadVendorResult> Presets::load_vendor_configs_from_json(
    const std::filesystem::path& path,
    SubstitutionRule             rule)
{
    if (!impl_->bundle)
        return err<LoadVendorResult>(ErrorCode::InvalidState,
                                     "Presets has no PresetBundle attached");

    // Resolve vendor_name by locating the single root JSON in `path`. Slic3r's
    // PresetBundle::load_vendor_configs_from_json expects `<path>/<vendor>.json`
    // alongside the `machine/`, `process/`, `filament/` subdirs — pick the JSON
    // up-front so plugin profile packs whose directory name differs from the
    // vendor stem still load. Falls back to the directory basename if the dir
    // can't be enumerated (covers the case where the caller already knows the
    // dir name matches the vendor).
    std::string vendor_name;
    {
        std::error_code ec;
        if (std::filesystem::is_directory(path, ec)) {
            std::filesystem::path picked;
            std::size_t           json_count = 0;
            for (const auto& entry : std::filesystem::directory_iterator(path, ec)) {
                if (ec) break;
                if (!entry.is_regular_file()) continue;
                if (entry.path().extension() == ".json") {
                    picked = entry.path();
                    ++json_count;
                    if (json_count > 1) break;
                }
            }
            if (json_count == 1)
                vendor_name = picked.stem().string();
        }
        if (vendor_name.empty())
            vendor_name = path.filename().string();
    }

    if (vendor_name.empty())
        return err<LoadVendorResult>(ErrorCode::InvalidArgument,
                                     "could not determine vendor name from path: " + path.string());

    // Map orca::SubstitutionRule -> Slic3r::ForwardCompatibilitySubstitutionRule.
    // The Slic3r enum is unscoped (defined in libslic3r/Config.hpp:218); names
    // match 1:1 with the orca enum (Disable / Enable / EnableSilent /
    // EnableSystemSilent), with EnableSilentDisableSystem as a Slic3r-only
    // extension we don't surface.
    Slic3r::ForwardCompatibilitySubstitutionRule slic3r_rule = Slic3r::Disable;
    switch (rule) {
        case SubstitutionRule::Disable:            slic3r_rule = Slic3r::Disable;            break;
        case SubstitutionRule::Enable:             slic3r_rule = Slic3r::Enable;             break;
        case SubstitutionRule::EnableSilent:       slic3r_rule = Slic3r::EnableSilent;       break;
        case SubstitutionRule::EnableSystemSilent: slic3r_rule = Slic3r::EnableSystemSilent; break;
    }

    // Map Slic3r::Preset::Type -> orca::PresetType for substitutions translation.
    auto map_preset_type = [](Slic3r::Preset::Type t) -> PresetType {
        switch (t) {
            case Slic3r::Preset::TYPE_PRINT:            return PresetType::Print;
            case Slic3r::Preset::TYPE_SLA_PRINT:        return PresetType::SlaPrint;
            case Slic3r::Preset::TYPE_FILAMENT:         return PresetType::Filament;
            case Slic3r::Preset::TYPE_SLA_MATERIAL:     return PresetType::SlaMaterial;
            case Slic3r::Preset::TYPE_PRINTER:          return PresetType::Printer;
            case Slic3r::Preset::TYPE_PHYSICAL_PRINTER: return PresetType::PhysicalPrinter;
            default:                                    return PresetType::Print;
        }
    };

    // LoadVendorOnly: load the vendor + its sub-presets without resetting the
    // existing bundle and without touching user/system bookkeeping. Plugin
    // profile packs are additive; the caller is responsible for any reset.
    std::pair<Slic3r::PresetsConfigSubstitutions, std::size_t> raw_result;
    try {
        raw_result = impl_->bundle->load_vendor_configs_from_json(
            path.string(),
            vendor_name,
            Slic3r::PresetBundle::LoadVendorOnly,
            slic3r_rule);
    } catch (const Slic3r::ConfigurationError& e) {
        return err<LoadVendorResult>(ErrorCode::InvalidArgument,
                                     std::string{"vendor config error: "} + e.what());
    } catch (const std::exception& e) {
        return err<LoadVendorResult>(ErrorCode::Unknown,
                                     std::string{"vendor config load failed: "} + e.what());
    }

    LoadVendorResult result;
    result.presets_loaded = raw_result.second;

    // Flatten Slic3r's per-preset substitution list into the engine-flat shape
    // (one orca::PresetSubstitution per opt-key change).
    for (const Slic3r::PresetConfigSubstitutions& entry : raw_result.first) {
        const PresetType type = map_preset_type(entry.preset_type);
        for (const Slic3r::ConfigSubstitution& sub : entry.substitutions) {
            PresetSubstitution out;
            out.preset_type = type;
            out.preset_name = entry.preset_name;
            out.opt_key     = sub.opt_def ? sub.opt_def->opt_key : std::string{};
            out.old_value   = sub.old_value;
            out.new_value   = sub.new_value ? sub.new_value->serialize() : std::string{};
            result.substitutions.push_back(std::move(out));
        }
    }

    return ok(std::move(result));
}

Result<void> Presets::save_changes_for_preset(PresetType /*type*/, std::string_view /*name*/) {
    return err<void>(ErrorCode::NotImplemented, "Presets::save_changes_for_preset not yet implemented");
}

// ---------- Compatibility / multi-material ----------

void Presets::update_compatible(SelectCompatibleAction action) {
    if (!impl_->bundle) return;

    auto map_action = [](SelectCompatibleAction a) {
        switch (a) {
            case SelectCompatibleAction::Never:               return Slic3r::PresetSelectCompatibleType::Never;
            case SelectCompatibleAction::OnlyIfWasCompatible: return Slic3r::PresetSelectCompatibleType::OnlyIfWasCompatible;
            case SelectCompatibleAction::Always:              return Slic3r::PresetSelectCompatibleType::Always;
        }
        return Slic3r::PresetSelectCompatibleType::Never;
    };

    impl_->bundle->update_compatible(map_action(action));
}

Result<void> Presets::update_multi_material_filament_presets() {
    if (!impl_->bundle) return err<void>(ErrorCode::InvalidState, "no PresetBundle attached");
    impl_->bundle->update_multi_material_filament_presets();
    return ok();
}

} // namespace orca
