#pragma once

#include "Outcome.hpp"

#include "libslic3r/PresetBundle.hpp"

#include <boost/filesystem/path.hpp>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <wx/string.h>

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
//          candidates), preset_incompatible (with the incompatible presets of that name, when no
//          compatible preset carries it), preset_not_offered (with the closest names).
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

// Starts the sync of the user presets with the cloud account, as Sync Presets does; the sync goes on in the
// background.
// Refuses: not_signed_in.
Outcome<Done> sync_presets();

// ---- Filament slots: the + and - buttons above the filament list, a slot's Delete and Merge with, the mixed filament list ----

// Adds a physical filament slot after the physical ones, as the + button does: it takes the next colour and the last
// slot's preset, and the mixed filaments, with the objects on them, move up by one. The slots are independent of the
// printer's extruders, so a toolchanger adds one too. Returns the new slot's number (1-based).
// Refuses: filaments_unavailable (a printer of another technology, or a G-code file shown in place of a project, which
//          has a filament list of its own), too_many_filaments.
Outcome<int> add_filament_slot();

// What keeps filament slot `slot` (1-based) of a project of `slots` slots, mixed ones among them, from being deleted, or
// merged into slot `merge_into`, or none: the only slot (last_filament_slot), a slot the project lacks (no_such_slot),
// and a merge into the slot itself (merge_into_itself).
std::optional<Refusal> filament_delete_refusal(int slots, int slot, std::optional<int> merge_into);

struct DeleteFilamentArgs
{
    std::optional<int>  filament_slot; // 1-based; none: the last physical slot, as the - button
    std::optional<int>  merge_into;    // 1-based; none: the objects on the slot take the first filament, as its Delete does
    // The answer to the question Merge with asks before it merges a physical filament into a mixed filament that mixes
    // it: true merges, false merges nothing; none asks it.
    std::optional<bool> break_mix;
    // The answer to the question Delete and Merge with ask before a physical slot deletes or merges into a later one on
    // a non-Bambu Lab printer with more than one nozzle and without single_extruder_multi_material, where the later
    // slots' tool numbers shift: true deletes, false deletes nothing; none asks it.
    std::optional<bool> confirm_shift;
};
struct DeletedFilament
{
    int                filament_slot = 0; // the slot deleted, by its number before the delete
    std::optional<int> merged_into;       // the slot its objects went to, by its number afterwards
    std::vector<int>   broken_mixed;      // the mixed filaments the sidebar marks broken afterwards, 1-based, as numbered then
};
// Deletes a filament slot as its Delete menu item does, or, with merge_into, as its Merge with item does
// (Sidebar::change_filament()): the objects on it go to that slot. A mixed filament is deleted as the mixed filament
// list deletes it. A physical filament a mixed filament mixes is deleted as the sidebar deletes it, without a question:
// the mix keeps a component that is gone and is broken until it is edited (edit_mixed_filament()). Not in the undo
// history, as none of them is.
// Refuses: filaments_unavailable, the only physical slot (last_filament_slot), a mixed
//          filament while one physical filament is left, whose list the sidebar then hides (mixed_list_hidden), what
//          filament_delete_refusal() gives, a merge of a mixed filament (mixed_filament_slot), a delete or merge before
//          a later slot on a non-Bambu Lab printer with more than one nozzle and without single_extruder_multi_material
//          answered no ahead (shifts_tools, confirm_shift false) or at its question (delete_cancelled), a merge into a
//          mixed filament that mixes the slot answered no ahead (mixed_uses_slot, break_mix false) or at its question
//          (merge_cancelled), and a delete the sidebar did not make (not_deleted).
Outcome<DeletedFilament> delete_filament_slot(const DeleteFilamentArgs& args);

// What keeps a mixed filament of `components` (1-based filament slots) in `ratios` (percentages, one per component)
// from being made in a project whose filaments that mix, its physical ones, have the filament types `types` (slot 1
// first), or none: a filament named twice (duplicate_component), which the Add Mixed Filament dialog's selectors
// never offer, fewer than two filaments (too_few_components), a slot that is not one of them (no_such_slot),
// filaments of different types (mixed_types), which the dialog does not take and the sidebar marks broken, and ratios
// that are not one per component, each above 0, adding up to 100 (bad_ratios).
std::optional<Refusal> mixed_filament_refusal(const std::vector<std::string>& types, const std::vector<int>& components,
                                              const std::vector<double>& ratios);

struct AddMixedFilamentArgs
{
    std::vector<int>    components; // 1-based physical filament slots; none, with no ratios: the dialog
    std::vector<double> ratios;     // whole percentages, one per component, as the dialog sets them
};
struct MixedFilament
{
    int  filament_slot = 0;     // 1-based; 0 when the dialog added none
    bool added         = false; // false: the project mixes these filaments in these ratios already, in that slot, or
                                // the dialog was cancelled
};
// Adds a mixed filament as the Add Mixed Filament dialog's OK does for these filaments and ratios with no gradient
// (Sidebar::ensure_mixed_filament()), in a slot after every other; a mix the project has already is that slot. With
// neither components nor ratios, opens that dialog as the sidebar's Add Mixed Filament button does, and the mix is what
// its OK makes, gradient included.
// Refuses: filaments_unavailable, components_and_ratios (one given without the other), what mixed_filament_refusal()
//          gives, too_few_components (for the dialog, fewer than two physical filaments, where the sidebar offers no
//          mix), too_many_filaments, and not_mixed where the sidebar made none.
Outcome<MixedFilament> add_mixed_filament(const AddMixedFilamentArgs& args);

struct EditMixedFilamentArgs
{
    int                 filament_slot = 0; // 1-based, a mixed filament
    std::vector<int>    components;        // 1-based physical filament slots; none, with no ratios: the dialog
    std::vector<double> ratios;            // whole percentages, one per component
};
struct EditedMixedFilament
{
    int  filament_slot = 0; // 1-based, as before the edit
    bool changed       = false;
};
// Changes a mixed filament as the Edit Mixed Filament dialog's OK does for these filaments and ratios
// (Sidebar::apply_mixed_filament_edit()): the slot keeps its number and the objects on it, its colour becomes the
// blend, and a gradient stays for a mix of two filaments and goes for more, which the dialog shows no gradient for.
// With neither components nor ratios, opens that dialog on the mix as a click on its swatch does, and the mix is what
// its OK makes. A mix the sidebar marks broken is repaired by an edit that names its filaments again.
// Refuses: filaments_unavailable, no_such_slot, not_mixed_filament (a physical slot), components_and_ratios, and what
//          mixed_filament_refusal() gives.
Outcome<EditedMixedFilament> edit_mixed_filament(const EditMixedFilamentArgs& args);

// ---- Syncing the filament list from the AMS: Synchronize Filament List from AMS and its dialog ----

// The sync dialog's two choices: map each of the project's filaments to a slot of the printer, or overwrite the filament
// list with the slots' filaments.
enum class AmsSyncMode { Map, Overwrite };

// What the sync reads of the printer selected on the Device tab and of the project.
struct AmsSyncState
{
    bool        printer = false;             // a printer is selected on the Device tab
    std::string printer_name;                // its name
    std::string selected_printer;            // the printer the printer selector shows
    bool        slots_listed        = false; // the printer listed AMS or external spool slots
    bool        printer_answered    = false; // it is online and has sent its status
    bool        filament_loaded     = false; // a slot holds a filament of a known type
    bool        filament_installed  = false; // the printer reports filament installed
    bool        same_printer        = false; // it is of the selected printer's model, ready to sync
    bool        project_has_objects = false; // a plate has objects: the dialog then asks how to sync
};
// The state of the printer selected on the Device tab, once Sidebar::load_ams_list() has read its slots. UI thread.
AmsSyncState ams_sync_state();
// Why the filament list cannot be synced from the AMS, or none; `mode` none for the sync dialog's own run.
// Refuses: no_printer, no_filament_system (the printer answered with no slot), not_connected, no_filament,
//          unknown_filaments (every filament of an unknown type), other_printer (not the selected printer's model), and
//          nothing_to_map (Map for a project with no objects, where the dialog does not ask and overwrites the list).
std::optional<Refusal> ams_sync_refusal(const AmsSyncState& state, std::optional<AmsSyncMode> mode);

// What the sync dialog's Synchronize now takes, as the sync applies it.
struct AmsSyncChoice
{
    bool                      direct_sync = true;        // overwrite the list; else map by sync_maps
    std::map<int, AMSMapInfo> sync_maps;                 // each project filament's slot, by its 0-based index
    bool                      every_slot_changed = true; // each slot's filament counts as changed since the last sync
    // Told, in the UI's language, the slots whose filament is unknown or incompatible and was mapped to a generic preset,
    // before the list changes; none tells no one.
    std::function<void(const wxString& detail)> unknowns_found;
    // Told the slots' detail where no filament is compatible and nothing is synced; none tells no one.
    std::function<void(const wxString& detail)> none_compatible;
};
struct AmsSynced
{
    std::vector<std::string> presets;  // each filament slot's preset afterwards, slot 1 first
    std::vector<std::string> colours;  // each slot's colour afterwards
    std::vector<std::string> unknowns; // the slots mapped to a generic preset: "tray(type) reason"
};
// Syncs the filament list from the AMS list read last (Sidebar::apply_ams_sync()), as the sync dialog's Synchronize now
// does with `choice`. Not in the undo history.
// Refuses: no_compatible_filaments (with the slots), where nothing is synced.
Outcome<AmsSynced> apply_ams_sync(const AmsSyncChoice& choice);
// Syncs the filament list from the AMS of the printer selected on the Device tab with the dialog's choice `mode`, as
// Synchronize now does on what the dialog first shows: Map maps the project's filaments to the slots as the dialog's
// Mapping first does. Opens nothing.
// Refuses: what ams_sync_refusal() gives, and what apply_ams_sync() refuses.
Outcome<AmsSynced> sync_filaments_from_ams(AmsSyncMode mode);

}} // namespace GUI::Ops
} // namespace Slic3r
