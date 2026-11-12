#pragma once

#include "Outcome.hpp"

#include <boost/filesystem/path.hpp>

#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

class PresetCollection;

// Presets: the one a printer or process tab edits, the one a filament slot holds, and preset files
// imported.
//
// A caller selects in two steps. resolve_preset() turns the name a person or an agent gives into the
// preset the selector takes for it; select_preset() or assign_filament_preset() then acts on the name
// it returned. A caller with a step of its own between the two (the plugin API switches the preset its
// transaction edits) takes it there.
namespace GUI { namespace Ops {

enum class PresetKind { Printer, Process, Filament };
// What becomes of the unsaved changes of the preset a tab leaves: the owner's Transfer-or-discard
// prompt opens (Ask), or it is answered for the person.
enum class PresetChanges { Ask, Transfer, Discard };

// The refusal for a filament slot (1-based) that is a mixed-color virtual one (mixed_filament_slot),
// or none. Such a slot is made of other slots' filaments and holds no preset of its own.
std::optional<Refusal> mixed_slot_refusal(int filament_slot);

struct ResolvePresetArgs
{
    PresetKind  kind = PresetKind::Process;
    std::string name;              // as the selector shows it, or the internal name
    int         filament_slot = 1; // Filament only, 1-based
};
struct ResolvedPreset
{
    std::string name;  // internal name
    std::string shown; // the hidden preset that was shown, or ""
};
// The preset the selector takes for the name: the slot's selector for a filament, the tab's otherwise;
// where no selector is populated, or the settings tab is missing, the tab's for a filament slot, then
// internal names. A hidden compatible preset the name stands for is shown in the selectors first, and
// stays shown until the app restarts, whatever comes of the call after it.
// Refuses: mixed_filament_slot, no_such_slot, ambiguous_preset_name, ambiguous_hidden_preset (with the
//          candidates), preset_incompatible (with the incompatible presets of that name),
//          preset_not_offered (with the closest names).
Outcome<ResolvedPreset> resolve_preset(const ResolvePresetArgs& args);

struct SelectPresetArgs
{
    PresetKind    kind = PresetKind::Process; // Printer or Process
    std::string   preset;                     // internal name, as resolve_preset() returned it
    PresetChanges changes = PresetChanges::Ask;
};
// Makes the preset the one its tab edits, as picking it in the tab's selector does.
// Refuses: tab_unavailable, transfer_not_offered, selection_cancelled, other_preset_selected (the tab
//          ended on another preset, which stays selected).
Outcome<Done> select_preset(const SelectPresetArgs& args);

struct AssignFilamentArgs
{
    int           filament_slot = 1; // 1-based
    std::string   preset;            // internal name, as resolve_preset() returned it
    PresetChanges changes    = PresetChanges::Ask; // for the filament tab's switch on a printer with one filament
    bool          switch_tab = true;               // false: the caller has made the tab edit the preset already
};
// Assigns the preset to the slot as picking it in the slot's selector does (Sidebar::select_filament_preset()).
// Returns the preset the slot holds afterwards. A declined prompt on a printer with one filament is no
// refusal: the slot is assigned before the tab asks.
// Refuses: mixed_filament_slot, no_such_slot, transfer_not_offered, and tab_unavailable where the
//          assignment would switch the filament tab (one filament, `switch_tab`) and there is none.
Outcome<std::string> assign_filament_preset(const AssignFilamentArgs& args);

struct ImportPresetsArgs
{
    boost::filesystem::path path;              // a preset .json, a .zip of them, or an .orca_printer, .orca_filament or .orca_bundle
    bool                    overwrite = false; // replace a preset that exists already; else keep it
};
struct ImportedPresets
{
    std::vector<std::string> imported; // presets that are new
    std::vector<std::string> replaced;
    std::vector<std::string> kept;     // presets that exist already and were left as they are
};
// Imports the file's presets as the Import Configs dialog does, with its question about each preset
// that exists already answered by `overwrite`.
// Refuses: no_presets_imported.
Outcome<ImportedPresets> import_presets(const ImportPresetsArgs& args);

// The names of `presets` closest to `name`, those compatible with the printer first: what a refusal
// of an unknown preset name carries, for a caller that refuses one itself.
std::vector<std::string> similar_preset_names(const PresetCollection& presets, const std::string& name);

}} // namespace GUI::Ops
} // namespace Slic3r
