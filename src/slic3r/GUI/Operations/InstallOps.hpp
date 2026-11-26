#pragma once

#include "Outcome.hpp"
#include "PresetOps.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace Slic3r {

class AppConfig;
class PresetBundle;

// What a person installs: the printer models the Printer Selection dialog lists and the system filaments the System
// Filaments dialog lists (GuideFrame, WebGuideDialog.cpp). Installed is what the app configuration's models and
// filaments sections name, which the presets' visibility follows (Preset::set_visible_from_appconfig()): the selectors
// offer an installed printer or filament, and the others only by name (preset_list include_uninstalled).
namespace GUI { namespace Ops {

// ---- Printer models ----

// The data the Printer Selection dialog lists, of every vendor the app ships or has installed: {model: [{model, name,
// vendor, nozzle_diameter ("0.4;0.6"), materials, cover, ...}], machine: {printer preset: {model, nozzle}}, filament:
// {preset: {name, vendor, type, models, ...}}, process: [{name, sub_path}]}. Read from
// <data_dir>/cache/wizard_profile_data.json while its vendor stamps (each vendor's name and version, in load order) are
// those of the vendors found in <data_dir>/system and the shipped profiles, the installed copy first; else every vendor
// is loaded into a preset bundle of its own, the data built from it and the cache written. It touches no window and
// runs on any thread: the dialog and printer_list_models run it off the UI thread, as the load takes a while. Once
// *cancel is set, no further vendor starts loading.
// Refuses: no_vendors, cancelled, vendors_not_loaded (with the vendors that failed), profile_data_unreadable.
Outcome<nlohmann::json> printer_profile_data(const std::atomic<bool>* cancel = nullptr);

// The data printer_profile_data() lists, built from `bundle`: its vendors' models, its system printers, filaments and
// visible processes. Throws std::exception where a preset's data cannot be read; the data has no machine when the
// bundle holds no system printer.
nlohmann::json profile_data_from(const PresetBundle& bundle);

// What the two dialogs' Confirm writes: the app configuration's models and filaments sections.
struct InstallSet
{
    std::map<std::string, std::map<std::string, std::set<std::string>>> printers; // vendor -> model -> nozzles (AppConfig::vendors())
    std::map<std::string, std::string>                                  filaments; // preset name -> "true"
};

// The installed set `config` holds: its models section, and its filaments section or, where it has none, which installs
// every filament, `offered_filaments` (the system filaments the selectors offer), so that a set written back keeps them
// installed.
InstallSet installed_set_from(const AppConfig& config, const std::vector<std::string>& offered_filaments);

// installed_set_from() of the app configuration and the filaments the selectors offer. UI thread.
InstallSet installed_set();

// A printer model and nozzles of it that a call names.
struct ModelNozzles
{
    std::string              vendor; // the vendor's id ("BBL"); empty: the one vendor that has the model
    std::string              model;  // the model's id, as printer_list_models lists it
    std::vector<std::string> nozzles; // the model's nozzle variants ("0.4"); empty: all of them
};

// `installed` with the nozzles of `add` added and those of `remove` removed, each under its vendor and model: a model
// left with no nozzle is removed, and a vendor left with no model. The other models and the filaments are kept.
InstallSet changed_install_set(const InstallSet& installed, const std::vector<ModelNozzles>& add, const std::vector<ModelNozzles>& remove);

// The refusal of removing `remove` from `installed` where it would leave no printer model installed (last_printer), as
// the Printer Selection dialog requires one; none otherwise.
std::optional<Refusal> uninstall_refusal(const InstallSet& installed, const std::vector<ModelNozzles>& remove);

// A printer model the app ships or has installed, as the Printer Selection dialog lists it.
struct PrinterModel
{
    std::string              vendor, model;
    std::vector<std::string> nozzles, installed_nozzles;
};

// The models of `profile` (printer_profile_data()'s), in its order, with the nozzles `installed` holds for each under
// its vendor: those of `vendor` (any case) when one is given, and those whose id or name holds `query` (any case).
std::vector<PrinterModel> printer_models_from(const nlohmann::json& profile, const InstallSet& installed, const std::string& vendor,
                                              const std::string& query);

struct PrinterModelsArgs
{
    std::string              vendor;    // any case; empty: every vendor
    std::string              query;     // a part of a model's id or name, any case
    InstallSet               installed; // installed_set(), read on the UI thread
    const std::atomic<bool>* cancel = nullptr;
};
// Every printer model of printer_profile_data(), as printer_models_from() filters it. Off the UI thread, as
// printer_profile_data().
// Refuses: what printer_profile_data() refuses, and no_such_vendor (with the closest vendors).
Outcome<std::vector<PrinterModel>> printer_models(const PrinterModelsArgs& args);

// The models `asked` names, each resolved against `known` (every model): its vendor where one vendor has a model of that
// id (in any case), all its nozzles where it names none. Every model and nozzle is checked.
// Refuses: no_such_vendor, no_such_model (each with the closest names), ambiguous_model (with the vendors that have
//          it), no_such_nozzle (with the model's nozzles).
Outcome<std::vector<ModelNozzles>> resolve_models(const std::vector<PrinterModel>& known, const std::vector<ModelNozzles>& asked);

// Applies `set` as the dialogs' Confirm does: asks about the presets' unsaved changes where the set differs from the
// installed one (`changes` Ask: the prompt with Transfer, Save and Discard; Transfer and Discard answer it), installs the
// vendor bundles a model of it needs from the shipped profiles, writes both sections, reloads the presets and selects a
// newly added printer (PresetBundle::apply_vendor_config()), puts the changes kept back on the presets selected, and
// brings the mode and the plates' bed types in line. Not in the undo history. UI thread.
// Refuses: selection_cancelled (the prompt's Cancel, before any change), vendors_not_installed (a bundle could not be
//          installed: the sections are not written and the presets not reloaded, but the unsaved changes are as the
//          answer left them, saved or discarded, and the bundles copied before the failure stay in the configuration
//          folder).
Outcome<Done> apply_install_set(const InstallSet& set, PresetChanges changes);

struct InstallPrintersArgs
{
    std::vector<ModelNozzles> models;
    std::vector<PrinterModel> known;                       // printer_models() of every vendor
    PresetChanges             changes = PresetChanges::Ask; // Ask: refused while presets have unsaved changes
};
struct InstalledPrinters
{
    std::vector<ModelNozzles> models;   // the models and nozzles the call named, resolved
    std::vector<std::string>  added;    // printer presets the selectors offer that they did not
    std::vector<std::string>  removed;  // printer presets the selectors no longer offer
    std::string               selected; // the printer preset selected afterwards
    std::vector<ModelNozzles> missing;  // named nozzles the selectors do not offer after an install, or still offer
    // System filaments the reload installed or uninstalled: a printer's default filaments where no installed filament
    // fits it (PresetBundle::load_installed_filaments()), and, where a vendor's profile lists default filaments of its
    // own (an INI bundle; the shipped JSON profiles list none), a @System filament swapped for the vendor's, as the
    // dialogs' Confirm swaps it (PresetBundle::apply_vendor_config()).
    std::vector<std::string> filaments_installed, filaments_uninstalled;
};
// Adds the models' nozzles to the installed printers and applies the set (apply_install_set()) with the installed
// filaments (installed_set()), then refreshes what a person's Confirm refreshes after the dialog closes and saves the
// app configuration. A set that does not change is not applied. Opens nothing: unsaved changes need `changes`. UI
// thread.
// Refuses: what resolve_models() refuses, unsaved_preset_changes, vendors_not_installed (as apply_install_set(): the
//          error says that the installed printers are as they were, what became of the unsaved changes and that copied
//          bundles stay), presets_not_reloaded (the sections were written, the presets not reloaded: the error says so).
Outcome<InstalledPrinters> install_printers(const InstallPrintersArgs& args);
// As install_printers(), removing the models' nozzles.
// Refuses: also what uninstall_refusal() refuses.
Outcome<InstalledPrinters> uninstall_printers(const InstallPrintersArgs& args);

// ---- System filaments ----

// A system filament preset the app has loaded, as a row of the System Filaments dialog lists it.
struct FilamentRow
{
    std::string name;  // the preset's
    std::string alias; // the short name the filament selectors show
    std::string vendor, type;
    bool        fits = false; // an installed printer fits it: its compatible_printers name one, or none at all
};

// The system filaments the app has loaded, the Template vendor's left out. UI thread.
std::vector<FilamentRow> loaded_filaments();

// The presets `name` stands for: the preset of that name, else each preset whose short name it is and that an installed
// printer fits, as a row of the System Filaments dialog stands for them; none for a name no row has.
std::vector<std::string> filament_presets_named(const std::vector<FilamentRow>& loaded, const std::string& name);

struct InstallFilamentsArgs
{
    std::vector<std::string> names;  // presets' names or short names
    std::string              vendor; // with filament_type, in place of names: every filament of them an installed printer fits
    std::string              filament_type;
};
// The presets `args` names in `loaded`, each once, in their order: those of each name (filament_presets_named()), or of
// the vendor and the filament type (any case). Every name is checked.
// Refuses: no_such_filament (with the closest names), fits_no_printer (a short name of presets no installed printer fits,
//          with them), no_filament_vendor (with the closest vendors), no_matching_filament (with the vendor's types).
Outcome<std::vector<std::string>> filaments_named(const std::vector<FilamentRow>& loaded, const InstallFilamentsArgs& args);

// The refusal of uninstalling `presets` where a filament slot of the project uses one of them (filament_in_use), naming
// the first such slot (1-based) and its preset; `slot_presets` are the slots' presets, slot 1 first.
std::optional<Refusal> filament_uninstall_refusal(const std::vector<std::string>& presets, const std::vector<std::string>& slot_presets);

struct InstalledFilaments
{
    std::vector<std::string> changed;   // named presets installed (or uninstalled) by the call
    std::vector<std::string> unchanged; // named presets that were installed (or not installed) already
    // System filaments installed again as an installed printer's default filaments, which the app keeps installed while
    // no other installed filament fits the printer (PresetBundle::load_installed_filaments()); named ones among them.
    std::vector<std::string> defaults_installed;
};
// Adds the named presets to the app configuration's filaments section, which installs them for good, then applies the
// section as the app's start does (PresetBundle::load_installed_filaments()): the filaments it installs are shown in
// the filament selectors, and an installed printer that no installed filament fits gets its default filaments again;
// a printer shown for this session only, which the app configuration does not install, gets none.
// The app configuration is saved and the selectors refreshed. The presets are not reloaded, so their unsaved changes are
// kept. A configuration with no filaments section, which installs every filament, is left as it is. UI thread.
// Refuses: what filaments_named() refuses.
Outcome<InstalledFilaments> install_filaments(const InstallFilamentsArgs& args);
// As install_filaments(), taking the named presets (and the names they were renamed from) out of the section. A
// configuration with no filaments section gets one, of every other system filament the selectors offer.
// Refuses: what filaments_named() refuses, and what filament_uninstall_refusal() refuses for the project's slots.
Outcome<InstalledFilaments> uninstall_filaments(const InstallFilamentsArgs& args);

}} // namespace GUI::Ops
} // namespace Slic3r
