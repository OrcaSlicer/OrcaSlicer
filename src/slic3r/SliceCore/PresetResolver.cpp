// PresetResolver.cpp
//
// Implements resolve() — merges named presets and/or raw-JSON config files into a
// single DynamicPrintConfig ready for slicing.
//
// PATH CHOSEN: Path A (full PresetBundle) + Path B (raw-JSON back-compat).
//
// Path A uses:
//   set_data_dir(datadir)                      — libslic3r/Utils.hpp:183
//   Slic3r::AppConfig                          — libslic3r/AppConfig.hpp
//   PresetBundle::load_presets(AppConfig&, …)  — libslic3r/PresetBundle.hpp:185
//   PresetCollection::find_preset(name, false) — libslic3r/Preset.hpp:3122 (Preset.cpp)
//   PresetBundle::filament_presets             — libslic3r/PresetBundle.hpp:324
//   PresetBundle::full_config()                — libslic3r/PresetBundle.hpp:367
//   All bundle/collection members: printers, prints, filaments — PresetBundle.hpp:314-320
//
// Path B raw-JSON loader reuses:
//   DynamicPrintConfig::load_from_json(file, rule, key_values, reason)
//     — libslic3r/Config.hpp:2737  (same call as OrcaSlicer.cpp:1965)
//
// Merge order inside resolve() (low → high precedence):
//   1. full_config() result from named presets (Path A, if any names given)
//   2. Each path in sel.load_settings (machine/process JSON), applied in order
//   3. Each path in sel.load_filaments, applied in order
//   sel.overrides is NOT applied here — the caller does that.
//
// Filament-vector composition:
//   When sel.filament_names is non-empty we populate bundle.filament_presets with the
//   requested names, then call full_config() which runs full_fff_config().  That
//   function iterates bundle.filament_presets and builds per-extruder vector options
//   exactly as the GUI does (PresetBundle.cpp:3954-4090).  When sel.load_filaments is
//   also provided those raw configs are then applied on top, key-by-key, overwriting
//   whatever full_config() produced.

#include "PresetResolver.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"    // set_data_dir / data_dir

#include <boost/filesystem/operations.hpp>
#include <boost/system/error_code.hpp>

#include <map>
#include <string>

namespace Slic3r {
namespace SliceCore {

// ---------------------------------------------------------------------------
// Helper: load a single JSON config file into `out`.
// Mirrors OrcaSlicer.cpp:1953-2020 (the load_config_file lambda).
// Returns false and appends to `err` on failure.
// ---------------------------------------------------------------------------
static bool load_json_into(const std::string &file,
                            DynamicPrintConfig &out,
                            std::string        &err)
{
    std::map<std::string, std::string> key_values;
    std::string reason;
    // Same rule used by the CLI (OrcaSlicer.cpp:1351).
    const auto rule = ForwardCompatibilitySubstitutionRule::Enable;
    try {
        out.load_from_json(file, rule, key_values, reason);
        if (!reason.empty()) {
            err = "cannot load \"" + file + "\": " + reason;
            return false;
        }
        out.normalize_fdm();
    } catch (const std::exception &ex) {
        err = "exception loading \"" + file + "\": " + ex.what();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
DynamicPrintConfig resolve(const PresetSelection &sel,
                           const std::string     &datadir,
                           std::string           &err)
{
    err.clear();
    DynamicPrintConfig result;

    try {
        // ------------------------------------------------------------------
        // PATH A — named presets via PresetBundle
        // ------------------------------------------------------------------
        const bool want_named =
            sel.printer_name.has_value() ||
            sel.process_name.has_value() ||
            !sel.filament_names.empty();

        if (want_named) {
            // Point the global data_dir at the caller-supplied directory so
            // that PresetBundle::load_system_presets_from_json() and
            // load_user_presets() find their files.
            set_data_dir(datadir);

            AppConfig app_config;
            // Don't load the on-disk slic3r.ini — we only need the bundle
            // to discover presets; selections come from sel, not from the
            // saved UI state.
            app_config.reset_selections();

            PresetBundle bundle;
            bundle.load_presets(app_config,
                                ForwardCompatibilitySubstitutionRule::EnableSilent);

            // NOTE on not-found handling:
            // Any requested NAME that is missing is fatal. We set `err` and
            // return the partial config immediately — we never select a
            // substitute preset, because a substitute would contribute real
            // (wrong) material/printer/process parameters that could silently
            // be used for slicing. The caller treats a non-empty `err` as
            // fatal, so this partial config is never consumed. Raw-JSON paths
            // (Path B) have no name lookup and are therefore unaffected.

            // --- Printer ---
            if (sel.printer_name.has_value()) {
                const std::string &name = *sel.printer_name;
                Preset *p = bundle.printers.find_preset(name, false);
                if (!p) {
                    err = "printer preset not found: \"" + name + "\"";
                    return result;
                }
                bundle.printers.select_preset_by_name(name, /*force=*/true);
            }

            // --- Process (print profile) ---
            if (sel.process_name.has_value()) {
                const std::string &name = *sel.process_name;
                Preset *p = bundle.prints.find_preset(name, false);
                if (!p) {
                    err = "process preset not found: \"" + name + "\"";
                    return result;
                }
                bundle.prints.select_preset_by_name(name, /*force=*/true);
            }

            // --- Filaments (multi-material) ---
            if (!sel.filament_names.empty()) {
                bundle.filament_presets.clear();
                for (const std::string &name : sel.filament_names) {
                    Preset *p = bundle.filaments.find_preset(name, false);
                    if (!p) {
                        // Do NOT substitute a different filament — that would
                        // feed wrong material params into full_config().
                        err = "filament preset not found: \"" + name + "\"";
                        return result;
                    }
                    bundle.filament_presets.push_back(name);
                }
                // Select the first filament as the "edited" one so that
                // full_config() single-filament path works correctly when
                // only one filament was requested.
                bundle.filaments.select_preset_by_name(
                    bundle.filament_presets.front(), /*force=*/true);
            }

            // full_config() applies FullPrintConfig::defaults(), then printer,
            // filament(s), and print configs, expanding all per-extruder
            // vector options — this is the canonical merge.
            //
            // apply_extruder MUST be true (the header default) so that
            // full_fff_config() composes per-extruder vector options
            // (filament_colour, nozzle_diameter, filament_type, etc.) across
            // every filament slot — exactly as the GUI does. Passing false
            // would skip update_values_to_printer_extruders() and leave
            // multi-material jobs with wrong/default per-extruder settings.
            result = bundle.full_config(/*apply_extruder=*/true);
        }

        // ------------------------------------------------------------------
        // PATH B — raw JSON overrides (load_settings + load_filaments)
        // These win over the named-preset base (applied on top).
        // ------------------------------------------------------------------

        // A failed JSON load is fatal: load_json_into() sets `err` and we
        // return promptly. (It never applies a partially-loaded cfg, so no
        // wrong data leaks in either way — returning early just keeps the
        // contract uniform with the name-not-found paths above.)

        // load_settings: machine and/or process JSON files.
        for (const std::string &file : sel.load_settings) {
            if (file.empty()) continue;
            DynamicPrintConfig cfg;
            if (!load_json_into(file, cfg, err))
                return result;
            result.apply(cfg, /*ignore_nonexistent=*/true);
        }

        // load_filaments: filament JSON files (in slot order).
        // We apply each one on top of the result the same way the CLI does —
        // no vector composition here; for full multi-material support callers
        // should use sel.filament_names.  This keeps raw-JSON back-compat.
        for (const std::string &file : sel.load_filaments) {
            if (file.empty()) continue;
            DynamicPrintConfig cfg;
            if (!load_json_into(file, cfg, err))
                return result;
            result.apply(cfg, /*ignore_nonexistent=*/true);
        }

    } catch (const std::exception &ex) {
        if (!err.empty()) err += "; ";
        err += std::string("resolve() caught exception: ") + ex.what();
    }

    return result;
}

// ---------------------------------------------------------------------------
// enumerate_preset_names
// ---------------------------------------------------------------------------

namespace {

// Collect the names of visible, non-default presets from a PresetCollection.
// PresetCollection::begin()/end() already skip the leading default presets
// (Preset.hpp:480), so we only filter out invisible / explicitly-default
// entries here.
template <typename CollectionT>
std::vector<std::string> collect_preset_names(const CollectionT &coll)
{
    std::vector<std::string> names;
    for (auto it = coll.begin(); it != coll.end(); ++it) {
        const Preset &preset = *it;
        if (!preset.is_visible || preset.is_default)
            continue;
        names.push_back(preset.name);
    }
    return names;
}

} // anonymous namespace

bool enumerate_preset_names(const std::string &datadir,
                             PresetNames       &out,
                             std::string       &err)
{
    if (datadir.empty()) {
        err = "datadir is not configured";
        return false;
    }
    {
        boost::system::error_code ec;
        namespace fs = boost::filesystem;
        if (!fs::exists(datadir, ec) || !fs::is_directory(datadir, ec)) {
            err = "datadir does not exist or is not a directory: " + datadir;
            return false;
        }
    }
    try {
        // Mirror the PresetBundle loading pattern used by resolve() Path A
        // and by ProfileCache::load_locked() — set the global data_dir, build
        // a minimal AppConfig (no on-disk selections), then load all presets.
        set_data_dir(datadir);

        AppConfig app_config;
        app_config.reset_selections();

        PresetBundle bundle;
        bundle.load_presets(app_config,
                            ForwardCompatibilitySubstitutionRule::EnableSilent);

        out.printers  = collect_preset_names(bundle.printers);
        out.processes = collect_preset_names(bundle.prints);
        out.filaments = collect_preset_names(bundle.filaments);
        return true;
    } catch (const std::exception &ex) {
        err = std::string("failed to load preset bundle: ") + ex.what();
        return false;
    } catch (...) {
        err = "failed to load preset bundle: unknown exception";
        return false;
    }
}

} // namespace SliceCore
} // namespace Slic3r
