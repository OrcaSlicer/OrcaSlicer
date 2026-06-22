#pragma once

#include "Result.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Forward declarations of libslic3r types referenced via raw().
//
// During Phase 0.4a these are exposed as a migration scaffold so the Transform A
// rewriter can map every `wxGetApp().preset_bundle->X` site to `session.presets().raw().X`
// uniformly. The typed accessors below are the long-term surface; raw() use is
// expected to shrink over Phase 0.4a's residual-cleanup pass and Phase 1.
namespace Slic3r {
class PresetBundle;
class DynamicPrintConfig;
} // namespace Slic3r

namespace orca {

class Session;

enum class PresetType {
    Print,
    Printer,
    Filament,
    SlaPrint,
    SlaMaterial,
    PhysicalPrinter,
};

// Where a config value is read from or written to.
// - Full           composed full_config view (printer + print + filaments + project overlay)
// - Project        project_config overlay only
// - PrintPreset    currently-selected print preset's config
// - PrinterPreset  currently-selected printer preset's config
// - FilamentPreset currently-selected filament preset for slot 0; use filament_opt<T>(idx, key) for multi-extruder reads
enum class ConfigScope {
    Full,
    Project,
    PrintPreset,
    PrinterPreset,
    FilamentPreset,
};

enum class SubstitutionRule {
    Disable,
    Enable,
    EnableSilent,
    EnableSystemSilent,
};

enum class SelectCompatibleAction {
    Never,
    OnlyIfWasCompatible,
    Always,
};

struct PresetRef {
    std::string name;
    std::string alias;
    bool        is_system   = false;
    bool        is_external = false;
    bool        is_dirty    = false;
};

struct VendorRef {
    std::string id;
    std::string name;
    bool        is_bbl = false;
};

struct PresetSubstitution {
    PresetType  preset_type   = PresetType::Print;
    std::string preset_name;
    std::string opt_key;
    std::string old_value;
    std::string new_value;
};

struct LoadVendorResult {
    std::vector<PresetSubstitution> substitutions;
    std::size_t                     presets_loaded = 0;
};

// Wraps a libslic3r ConfigOption*Nullable: nullopt represents the "null" sentinel
// the upstream type carries; has_value() means the value is present.
template <class T>
struct NullableValue {
    std::optional<T> value;

    bool has_value() const noexcept { return value.has_value(); }
    bool is_null()   const noexcept { return !value.has_value(); }

    const T& operator*()  const { return *value; }
    T&       operator*()        { return *value; }
};

class Presets {
public:
    ~Presets();

    Presets(const Presets&)            = delete;
    Presets& operator=(const Presets&) = delete;
    Presets(Presets&&)                 = delete;
    Presets& operator=(Presets&&)      = delete;

    // ---------- Read ----------

    std::vector<PresetRef> list(PresetType type) const;
    PresetRef              current(PresetType type) const;

    int                      extruder_count() const;
    int                      filament_count() const;
    std::vector<std::string> filament_names() const;

    template <class T>
    std::optional<T> opt(ConfigScope scope, std::string_view key) const;

    // Reads element [idx] of a vector-typed option (e.g. nozzle_diameter[0]).
    template <class T>
    std::optional<T> opt_at(ConfigScope scope, std::string_view key, std::size_t idx) const;

    // Reads a scalar key from the filament preset currently bound to the given slot.
    template <class T>
    std::optional<T> filament_opt(int filament_slot, std::string_view key) const;

    template <class E>
    std::optional<E> enum_opt(ConfigScope scope, std::string_view key) const;

    template <class T>
    NullableValue<T> nullable_opt(ConfigScope scope, std::string_view key) const;

    // ---------- Write (auto-creates the option if missing) ----------

    template <class T>
    Result<void> set(ConfigScope scope, std::string_view key, T value);

    template <class T>
    Result<void> set_at(ConfigScope scope, std::string_view key, std::size_t idx, T value);

    template <class T>
    Result<void> set_values(ConfigScope scope, std::string_view key, std::vector<T> values);

    template <class E>
    Result<void> set_enum(ConfigScope scope, std::string_view key, E value);

    // ---------- Phase 0.4d typed surface (key-tag templated) ----------
    //
    // Each call site replaces `cfg.option<ConfigOptionX>("foo")->value` (or
    // `cfg.opt_int("foo")` etc.) with `presets().get<keys::foo>(scope)`. The
    // type is fixed at the key tag, so the call site can't request the wrong
    // ConfigOption subclass. See orca/ConfigKeys.hpp for the 850 generated
    // key tags; see config.hpp for the free-function variant used at Adhoc
    // receivers (where the DynamicPrintConfig isn't tied to a preset scope).
    template <class K>
    auto get(ConfigScope scope) const -> std::optional<typename K::type>;

    template <class K>
    auto get_at(ConfigScope scope, std::size_t idx) const -> std::optional<typename K::type>;

    template <class K>
    auto get_vec(ConfigScope scope) const -> std::optional<std::vector<typename K::type>>;

    template <class K>
    Result<void> put(ConfigScope scope, typename K::type value);

    Result<void> set_current(PresetType type, std::string_view name);
    Result<void> set_filament(int extruder_idx, std::string_view name);
    Result<void> set_num_filaments(int count, std::string_view default_color = {});
    Result<void> set_num_filaments(int count, std::vector<std::string> colors);

    // ---------- Lifecycle ----------

    Result<std::vector<PresetSubstitution>> load_user_presets(
        std::string_view  user,
        SubstitutionRule  rule);

    Result<LoadVendorResult> load_vendor_configs_from_json(
        const std::filesystem::path& path,
        SubstitutionRule             rule);

    Result<void> save_changes_for_preset(PresetType type, std::string_view name);

    // ---------- Compatibility / multi-material ----------

    void         update_compatible(SelectCompatibleAction action);
    Result<void> update_multi_material_filament_presets();

    // ---------- Vendor / BBL identity ----------

    bool                   is_bbl_vendor() const;
    bool                   use_bbl_network() const;
    bool                   use_bbl_device_tab() const;
    std::vector<VendorRef> vendors() const;

    // ---------- Physical printer ----------

    std::vector<PresetRef> physical_printer_list() const;
    Result<void>           select_physical_printer(std::string_view name);

    // ---------- Migration scaffold (Phase 0.4a) ----------
    //
    // raw() returns the underlying Slic3r::PresetBundle owned/borrowed by the
    // engine. Transform A rewrites every `wxGetApp().preset_bundle` call site
    // to `orca::session().presets().raw_ptr()` — the *_ptr() variant is what
    // the rewriter targets because the original receiver is a pointer; this
    // preserves `->` chains and any pointer-typed local/member aliases for
    // free. raw() is the reference variant for new code. Phase 1 removes both.
    Slic3r::PresetBundle*       raw_ptr();
    const Slic3r::PresetBundle* raw_ptr() const;
    Slic3r::PresetBundle&       raw();
    const Slic3r::PresetBundle& raw() const;
    bool                        has_bundle() const noexcept;

private:
    friend class Session;
    Presets();

    // Wire the back-pointer to the owning Session so this service can reach
    // the event bus (session->events()). Mirrors Slicer::bind_session /
    // Exporter::bind_session — called once by Session::Session().
    void bind_session(Session* session);

    // Borrow the GUI/CLI-owned PresetBundle for the lifetime of this Session.
    // Called by Session::attach_preset_bundle().
    void attach_bundle(Slic3r::PresetBundle* bundle);
    void detach_bundle();

    // Resolve a ConfigScope to the underlying DynamicPrintConfig. Returns
    // nullptr if no bundle is attached or the scope is currently unresolvable
    // (e.g. FilamentPreset when no filaments are selected). Defined out-of-line
    // in Presets.cpp; consumed by the templated typed accessors below.
    const Slic3r::DynamicPrintConfig* config_for_scope(ConfigScope scope) const;
    Slic3r::DynamicPrintConfig*       config_for_scope(ConfigScope scope);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace orca

// Template definitions for the scoped typed surface live in Config.hpp, which
// brings together: (a) ConfigKeys.hpp (the key tags), (b) the orca::config
// free-function helpers, and (c) the Presets::get/get_at/get_vec/put template
// defs that delegate to those helpers. Including Config.hpp here would create
// a circular-include hazard (ConfigKeys.hpp pulls Presets.hpp for ConfigScope),
// so consumers that want the typed surface include orca/Config.hpp directly.
