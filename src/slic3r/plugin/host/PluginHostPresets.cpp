#include "PluginHostBindings.hpp"
#include "PluginHostUi.hpp"
#include "slic3r/plugin/PluginBindingUtils.hpp"

#include <memory>
#include <pybind11/pytypes.h>
#include <pybind11/cast.h>
#include <pybind11/pybind11.h>

#include <cstddef>
#include <libslic3r/AppConfig.hpp>
#include <libslic3r/Preset.hpp>
#include <libslic3r/PresetBundle.hpp>

#include <slic3r/GUI/GUI.hpp>
#include <slic3r/GUI/GUI_App.hpp>
#include <slic3r/GUI/MainFrame.hpp>
#include <slic3r/GUI/Operations/Outcome.hpp>
#include <slic3r/GUI/Operations/PresetOps.hpp>
#include <slic3r/GUI/Plater.hpp>
#include <slic3r/GUI/PresetComboBoxes.hpp> // TabPresetComboBox::set_filament_idx
#include <slic3r/GUI/Tab.hpp>

#include <pybind11/stl.h>

#include <wx/string.h>

#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace py = pybind11;

namespace Slic3r {
namespace {

py::list current_filament_presets(PresetBundle& bundle)
{
    py::list presets;
    for (const std::string& preset_name : bundle.filament_presets) {
        Preset* preset = bundle.filaments.find_preset(preset_name);
        if (preset == nullptr)
            presets.append(py::none());
        else
            presets.append(py::cast(preset, py::return_value_policy::reference));
    }
    return presets;
}

PresetCollection& printer_presets(PresetBundle& bundle)
{
    return static_cast<PresetCollection&>(bundle.printers);
}

// Handle to one category of currently edited preset. It is a category, not a snapshot: it always
// resolves the preset the matching tab edits now, so it stays valid across preset switches.
struct PresetEditor
{
    Preset::Type type = Preset::TYPE_INVALID;
};

const char* preset_tab_name(Preset::Type type)
{
    switch (type) {
    case Preset::TYPE_PRINT:    return "process";
    case Preset::TYPE_FILAMENT: return "filament";
    case Preset::TYPE_PRINTER:  return "printer";
    default:                    return "preset";
    }
}

PresetCollection& collection_for(PresetBundle& bundle, Preset::Type type)
{
    switch (type) {
    case Preset::TYPE_PRINT:    return bundle.prints;
    case Preset::TYPE_FILAMENT: return bundle.filaments;
    case Preset::TYPE_PRINTER:  return printer_presets(bundle);
    default:                    throw std::invalid_argument("Unsupported preset category");
    }
}

// Marker for a user-declined selection: Tab::select_preset() returned false. It derives from
// std::runtime_error (pybind11 maps the base to RuntimeError), so callers see it as an ordinary
// failed transaction and no staged setting or slot assignment is created.
struct SelectionCancelled : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// --------------------------------------------------------------------------
// The staged preset update lifecycle described in docs/HLSD/preset-plugin-api.md.
//
// begin_update() resolves and binds one target (selecting it through the normal UI flow when
// the caller asked for a preset that is not already active), returns a PresetTransaction that
// stages changes with set(), and applies them only on commit(). The transaction holds names,
// never preset pointers, so it stays valid across reloads; a target or filament slot that changed
// between begin_update() and commit() is rejected instead of silently editing the new mapping.
//
// begin_update() is synchronous: it runs on the background Script worker, marshals the target
// selection to the UI thread with PluginHostUi::run_on_ui_thread(), and blocks that worker until
// the selection and any unsaved-edit dialog settle. The UI thread stays responsive because the
// Script capability owns its worker thread. No GUI or preset pointer outlives the call; the
// returned transaction holds copied category/name/slot values. The update() shorthand shares the
// same target resolution and is safe only from that worker, never from the UI thread or a slicing
// pipeline hook.
//
// Known limits of this implementation, kept explicit rather than hidden:
//   * A requested preset_name is resolved by GUI::Ops::resolve_preset(), the rule every caller that
//     selects a preset by name shares: the exact name a dropdown entry shows, its display label or
//     its stored item alias (the internal preset name). A name two entries carry is rejected as
//     ambiguous instead of being resolved by position. Process and printer resolve against the tab
//     dropdown; a filament preset_name resolves against the requested slot's own sidebar selector,
//     not the tab's, so its compatibility filter is the one that applies. A hidden compatible
//     preset the name stands for is shown in the selectors first, where it stays until the app
//     restarts. A target the selector does not offer fails before the flow's unsaved-edit prompt. A
//     process or printer selection that leaves another preset edited is refused
//     (GUI::Ops::select_preset()).
//   * A filament preset_name that names a different preset for a slot first makes the filament tab
//     edit it through Tab::select_preset() (the normal flow, honouring unsaved-edit prompts), since
//     the transaction edits that preset, and then assigns it to the slot with
//     GUI::Ops::assign_filament_preset(), which does what picking the preset in the slot's sidebar
//     selector does: the slot's colour follows the preset, the mapping, the flushing volumes and the
//     project state are updated and every plate's slice result is invalidated.
//   * A user-declined selection does not undo decisions the user already accepted in an earlier
//     dialog. It aborts before any slot assignment or transaction is created, so no staged
//     setting is applied.
// --------------------------------------------------------------------------

// Outcome of one committed transaction. Identifies the target, whether selecting it switched
// anything, and for filament (whose preset may be shared) which slots share the edited preset.
struct PresetUpdateResult
{
    Preset::Type             type                    = Preset::TYPE_INVALID;
    std::string              target_name;
    bool                     selection_changed       = false;
    int                      slot_number             = -1; // 1-based for filament, -1 otherwise
    bool                     slot_assignment_changed = false;
    std::vector<int>         shared_slots;
    std::vector<std::string> keys;
    bool                     dirty                   = false;
};

struct PresetTransaction
{
    Preset::Type  type                    = Preset::TYPE_INVALID;
    std::string   target_name;
    bool          filament                = false;
    int           slot_number             = 0; // 1-based, filament only
    std::string   slot_assignment_snapshot;    // slot's assigned preset as of begin_update()
    bool          selection_changed       = false;
    bool          slot_assignment_changed = false;

    std::vector<std::pair<std::string, std::string>> staged;
    std::set<std::string>                            staged_keys;
    bool                                             committed = false;

    void               set(const std::string& key, const std::string& value);
    PresetUpdateResult commit();
};

GUI::GUI_App& require_gui_app()
{
    GUI::GUI_App& app = GUI::wxGetApp();
    if (app.is_closing())
        throw std::runtime_error("OrcaSlicer is closing");
    if (app.preset_bundle == nullptr)
        throw std::runtime_error("Preset bundle is not available");
    if (app.plater() == nullptr)
        throw std::runtime_error("OrcaSlicer main window is not available");
    return app;
}

GUI::Tab* require_tab(GUI::GUI_App& app, Preset::Type type)
{
    GUI::Tab* tab = app.get_tab(type);
    if (tab == nullptr)
        throw std::runtime_error(std::string("The ") + preset_tab_name(type) + " preset tab is not available");
    // The tab for this preset type exists, but the selected printer's technology may not use it
    // (the print tab no-ops under SLA); refuse instead of writing a value the UI cannot apply.
    if (!tab->supports_printer_technology(app.preset_bundle->printers.get_selected_preset().printer_technology()))
        throw std::runtime_error(std::string("The ") + preset_tab_name(type)
                                 + " preset is not available for the selected printer technology");
    return tab;
}

GUI::Ops::PresetKind preset_kind(Preset::Type type)
{
    switch (type) {
    case Preset::TYPE_PRINTER:  return GUI::Ops::PresetKind::Printer;
    case Preset::TYPE_FILAMENT: return GUI::Ops::PresetKind::Filament;
    default:                    return GUI::Ops::PresetKind::Process;
    }
}

// Raises a preset operation's refusal as the exception a plugin has had for it, which pybind11 maps:
// IndexError for a slot the printer lacks, ValueError for a mixed-color slot and for a name that
// resolves to no selectable preset, SelectionCancelled for a declined prompt, RuntimeError
// otherwise. The text is the operation's.
[[noreturn]] void throw_refusal(const GUI::Ops::Refusal& refusal)
{
    const std::string code = refusal.code;
    const std::string text = refusal.english();
    if (code == "no_such_slot")
        throw std::out_of_range(text);
    if (code == "mixed_filament_slot" || code == "ambiguous_preset_name" || code == "ambiguous_hidden_preset" ||
        code == "preset_incompatible" || code == "preset_not_offered")
        throw std::invalid_argument(text);
    if (code == "selection_cancelled")
        throw SelectionCancelled(text);
    throw std::runtime_error(text);
}

// The preset a filament slot currently resolves to, by its internal name. A slot whose stored
// name is empty or does not resolve yields an error identifying the slot.
std::string resolved_slot_preset(PresetBundle& bundle, int slot_number)
{
    if (slot_number < 1 || static_cast<size_t>(slot_number) > bundle.filament_presets.size())
        throw std::out_of_range("Filament slot " + std::to_string(slot_number)
                                + " is out of range; slot numbers are 1-based and this printer has "
                                + std::to_string(bundle.filament_presets.size()) + " filament slot(s)");
    const std::string& assigned_name = bundle.filament_presets[static_cast<size_t>(slot_number) - 1];
    if (assigned_name.empty())
        throw std::runtime_error("Filament slot " + std::to_string(slot_number) + " has no assigned filament preset");
    Preset* preset = bundle.filaments.find_preset(assigned_name);
    if (preset == nullptr)
        throw std::runtime_error("Filament slot " + std::to_string(slot_number) + " is assigned \"" + assigned_name
                                 + "\", which does not resolve to a filament preset");
    return preset->name;
}

void PresetTransaction::set(const std::string& key, const std::string& value)
{
    if (committed)
        throw std::runtime_error("This preset transaction has already been committed; start a new begin_update()");
    if (key.empty())
        throw std::invalid_argument("A setting key is required");
    // Conflicting duplicate writes to one setting are rejected rather than resolved by order.
    if (!staged_keys.insert(key).second)
        throw std::invalid_argument("Setting \"" + key + "\" is already staged in this transaction; "
                                    "conflicting duplicate writes to one setting are rejected");
    staged.emplace_back(key, value);
}

PresetUpdateResult PresetTransaction::commit()
{
    if (committed)
        throw std::runtime_error("This preset transaction has already been committed; start a new begin_update()");
    if (type == Preset::TYPE_INVALID)
        throw std::runtime_error("This preset transaction has no target");

    PresetUpdateResult result;
    PluginHostUi::run_on_ui_thread([&] {
        GUI::GUI_App&     app        = require_gui_app();
        GUI::Tab*         tab        = require_tab(app, type);
        PresetBundle&     bundle     = *app.preset_bundle;
        PresetCollection& collection = collection_for(bundle, type);

        // The target must still be the edited preset: the user may have switched away while the
        // transaction was staged, and editing a different preset would break "explicit target".
        const std::string edited = collection.get_edited_preset().name;
        if (edited != target_name)
            throw std::runtime_error("The edited " + std::string(preset_tab_name(type)) + " preset is \"" + edited
                                     + "\", not this transaction's target \"" + target_name
                                     + "\"; nothing was applied");

        if (filament) {
            // Reject a stale slot mapping instead of editing whatever the slot now points at.
            const std::string now_assigned = resolved_slot_preset(bundle, slot_number);
            if (now_assigned != slot_assignment_snapshot)
                throw std::runtime_error("Filament slot " + std::to_string(slot_number) + " now holds \"" + now_assigned
                                         + "\", not \"" + slot_assignment_snapshot
                                         + "\" as when this transaction was created; the slot mapping changed, so nothing was applied");
        }

        // Apply the whole batch through the tab's batch path: it validates every key and value before
        // touching the preset, rejects duplicate keys, runs the same per-setting commit path a hand
        // edit uses (validation, on_change, dirty state) and refreshes once for the batch. A failure
        // restores the edited preset, so no staged setting survives a failed commit.
        tab->apply_preset_config_values(staged);

        result.type                    = type;
        result.target_name             = target_name;
        result.selection_changed       = selection_changed;
        result.slot_number             = filament ? slot_number : -1;
        result.slot_assignment_changed = slot_assignment_changed;
        result.keys.reserve(staged.size());
        for (const auto& entry : staged)
            result.keys.push_back(entry.first);
        result.dirty = collection.get_edited_preset().is_dirty;

        if (filament) {
            // Every slot whose assigned preset resolves to the edited one is affected: the preset
            // is shared, not a per-slot override.
            for (std::size_t index = 0; index < bundle.filament_presets.size(); ++index) {
                Preset* preset = bundle.filaments.find_preset(bundle.filament_presets[index]);
                if (preset != nullptr && preset->name == target_name)
                    result.shared_slots.push_back(static_cast<int>(index) + 1);
            }
        }
    });

    committed = true;
    return result;
}

// Process/printer target resolution: the active preset when no name is given, otherwise the named
// preset selected through the normal tab-selection flow (which enforces the dropdown/compatibility
// rules and the unsaved-edit prompt, and refuses when it leaves another preset edited).
// Runs on the UI thread with the GIL released, called from begin_preset_update().
void bind_preset_target(Preset::Type type, const std::string& preset_name, PresetTransaction& transaction)
{
    GUI::GUI_App& app = require_gui_app();
    require_tab(app, type);
    PresetCollection& collection = collection_for(*app.preset_bundle, type);

    const std::string current = collection.get_edited_preset().name;
    std::string       target  = current;
    bool              selection_changed = false;

    if (!preset_name.empty()) {
        // Resolved by the name the tab's dropdown shows (display label or item alias), so a plugin
        // can pass exactly what the user sees.
        GUI::Ops::ResolvePresetArgs resolve;
        resolve.kind = preset_kind(type);
        resolve.name = preset_name;
        const GUI::Ops::Outcome<GUI::Ops::ResolvedPreset> resolved = GUI::Ops::resolve_preset(resolve);
        if (!resolved)
            throw_refusal(resolved.refusal());
        target = resolved.value().name;
        if (target != current) {
            // Normal UI selection: honours the unsaved-edit prompt; a declined prompt is a user
            // cancellation, distinct from any other resolution failure.
            const GUI::Ops::Outcome<GUI::Ops::Done> selected = GUI::Ops::select_preset({preset_kind(type), target, GUI::Ops::PresetChanges::Ask});
            if (!selected)
                throw_refusal(selected.refusal());
            selection_changed = true;
        }
    }

    transaction.type              = type;
    transaction.target_name       = target;
    transaction.selection_changed = selection_changed;
}

// Blocking form used by begin_update() and update().
PresetTransaction begin_preset_update(Preset::Type type, const std::string& preset_name)
{
    PresetTransaction transaction;
    PluginHostUi::run_on_ui_thread([&] { bind_preset_target(type, preset_name, transaction); });
    return transaction;
}

// Filament target resolution: slot_number identifies the slot; an omitted preset_name keeps the
// preset assigned to it. A supplied preset_name selects that preset for the slot through the normal
// tab-selection flow, then records the slot assignment and its dependent Plater side effects (the
// ones the sidebar selection path performs) only after that flow succeeds, so a canceled prompt
// changes neither. A mixed-color virtual slot is rejected before any of this, with or without a
// preset_name. A requested preset the requested slot's own dropdown does not offer is rejected
// before the selection flow, so no dialog is raised for it; the one exception is the slot's own
// current assignment, which stays targetable when the dropdown hides it. A different preset the
// tab happens to edit is a globally edited target, not an offered choice for this slot, and is
// rejected.
// Runs on the UI thread with the GIL released, called from begin_filament_update().
void bind_filament_target(int slot_number, const std::string& preset_name, PresetTransaction& transaction)
{
    GUI::GUI_App& app    = require_gui_app();
    PresetBundle& bundle = *app.preset_bundle;

    // A mixed-color virtual slot has no single filament preset behind it, so every form of this
    // call is rejected: with or without a preset_name there is no one preset to edit or assign.
    if (const std::optional<GUI::Ops::Refusal> refusal = GUI::Ops::mixed_slot_refusal(slot_number))
        throw_refusal(*refusal);

    // Resolve and validate before any selection, so a bad slot fails without a prompt.
    const std::string assigned_internal = resolved_slot_preset(bundle, slot_number);

    GUI::Tab* tab = require_tab(app, Preset::TYPE_FILAMENT);

    std::string target = assigned_internal;
    if (!preset_name.empty()) {
        // Resolved against the requested sidebar slot's own selector, not the tab's: that is the
        // model the user picks this slot's filament from, including its compatibility filter.
        GUI::Ops::ResolvePresetArgs resolve;
        resolve.kind          = GUI::Ops::PresetKind::Filament;
        resolve.name          = preset_name;
        resolve.filament_slot = slot_number;
        const GUI::Ops::Outcome<GUI::Ops::ResolvedPreset> resolved = GUI::Ops::resolve_preset(resolve);
        if (resolved) {
            target = resolved.value().name;
        } else {
            // Not shown by this slot's selector. Only the slot's current assignment stays
            // targetable (a hidden or incompatible-but-selected preset the dropdown omits);
            // resolve it by internal name or by display alias and accept only that preset. A
            // different preset the tab happens to edit is a globally edited target, not an
            // offered choice for this slot, and is rejected.
            const std::string code = resolved.refusal().code;
            if (code != "preset_not_offered" && code != "preset_incompatible")
                throw_refusal(resolved.refusal());
            Preset* by_name = bundle.filaments.find_preset(preset_name);
            if (by_name == nullptr) {
                const std::string& alias_name = bundle.filaments.get_preset_name_by_alias(preset_name);
                by_name                       = bundle.filaments.find_preset(alias_name);
            }
            if (by_name == nullptr || by_name->name != assigned_internal)
                throw_refusal(resolved.refusal());
            target = by_name->name;
        }
    }

    bool selection_changed = false;
    if (bundle.filaments.get_edited_preset().name != target) {
        // A declined prompt is a user cancellation: it is thrown before any slot-assignment change,
        // so neither the mapping nor a setting is touched.
        if (!tab->select_preset(target))
            throw SelectionCancelled("Switching the edited filament preset to \"" + target + "\" for filament slot "
                                     + std::to_string(slot_number)
                                     + " was canceled; neither the slot assignment nor any setting was changed");
        if (bundle.filaments.get_edited_preset().name != target)
            throw std::runtime_error("The edited filament preset is \"" + bundle.filaments.get_edited_preset().name
                                     + "\", not \"" + target + "\"; the transaction was not created");
        selection_changed = true;
    }

    bool assignment_changed = false;
    if (!preset_name.empty() && target != assigned_internal) {
        if (app.app_config == nullptr)
            throw std::runtime_error("Application configuration is not available");
        // What picking the preset in the slot's sidebar selector does. The tab edits it already, so
        // it is not switched again: a second switch would ask again about changes the first one
        // transferred.
        GUI::Ops::AssignFilamentArgs assign;
        assign.filament_slot = slot_number;
        assign.preset        = target;
        assign.switch_tab    = false;
        const GUI::Ops::Outcome<std::string> assigned = GUI::Ops::assign_filament_preset(assign);
        if (!assigned)
            throw_refusal(assigned.refusal());
        assignment_changed = true;
    }

    // Match PlaterPresetComboBox::switch_to_tab(): record which 0-based filament slot this tab
    // combobox now edits, so the GUI shows the slot the value was written for.
    if (GUI::TabPresetComboBox* combo = tab->get_combo_box())
        combo->set_filament_idx(slot_number - 1);

    transaction.type                     = Preset::TYPE_FILAMENT;
    transaction.filament                 = true;
    transaction.slot_number              = slot_number;
    transaction.target_name              = target;
    transaction.selection_changed        = selection_changed;
    transaction.slot_assignment_changed  = assignment_changed;
    // Snapshot the mapping after the explicit assignment, as the slot's resolved internal name,
    // so commit() compares like with like (a stored alias resolves to the same internal preset).
    transaction.slot_assignment_snapshot = resolved_slot_preset(bundle, slot_number);
}

// Blocking form used by begin_update() and update().
PresetTransaction begin_filament_update(int slot_number, const std::string& preset_name)
{
    PresetTransaction transaction;
    PluginHostUi::run_on_ui_thread([&] { bind_filament_target(slot_number, preset_name, transaction); });
    return transaction;
}

// Compatibility shorthand: one staged setting, committed immediately through the same
// begin_update()/set()/commit() path, so targeting, selection, validation and commit are shared.
void update_edited_preset(Preset::Type type, const std::string& key, const std::string& value)
{
    PresetTransaction transaction = begin_preset_update(type, std::string());
    transaction.set(key, value);
    transaction.commit();
}

void update_filament_slot(int slot_number, const std::string& key, const std::string& value)
{
    PresetTransaction transaction = begin_filament_update(slot_number, std::string());
    transaction.set(key, value);
    transaction.commit();
}

void reload_local_bundle(const std::string& bundle_id)
{
    bool        reloaded = false;
    std::string error;
    PluginHostUi::run_on_ui_thread([&] {
        GUI::GUI_App& app = GUI::wxGetApp();
        if (app.is_closing())
            throw std::runtime_error("OrcaSlicer is closing");
        if (app.preset_bundle == nullptr)
            throw std::runtime_error("Preset bundle is not available");
        if (app.app_config == nullptr)
            throw std::runtime_error("Application configuration is not available");
        if (app.mainframe == nullptr)
            throw std::runtime_error("OrcaSlicer main window is not available");

        reloaded = app.preset_bundle->reload_local_bundle(app.app_config->get("preset_folder"), bundle_id, &error);
        if (!reloaded)
            return;

        app.mainframe->update_side_preset_ui();
        for (GUI::Tab* tab : app.tabs_list) {
            tab->reload_config();
            tab->update_changed_ui();
        }
        if (GUI::Plater* plater = app.plater())
            plater->sidebar().update_all_preset_comboboxes();
    });
    if (!reloaded)
        throw std::runtime_error(error.empty() ? "Local bundle was not reloaded" : error);
}

std::vector<std::string> list_bundle_ids()
{
    std::vector<std::string> ids;
    PluginHostUi::run_on_ui_thread([&] {
        GUI::GUI_App& app = GUI::wxGetApp();
        if (app.is_closing())
            throw std::runtime_error("OrcaSlicer is closing");
        if (app.preset_bundle == nullptr)
            throw std::runtime_error("Preset bundle is not available");
        if (app.app_config == nullptr)
            throw std::runtime_error("Application configuration is not available");

        ids = app.preset_bundle->list_local_bundle_ids(app.app_config->get("preset_folder"));
    });
    return ids;
}

} // namespace

void host_bindings::register_presets(py::module_& host)
{
    auto preset  = host.def_submodule("preset",
                                      "Preset editing. orca.host.preset.process, orca.host.preset.printer and "
                                      "orca.host.preset.filament are the PresetEditor instances that change the "
                                      "edited process, printer and filament presets: update(key, value) for one "
                                      "setting, begin_update() for a batch. Each accepts only settings of its own "
                                      "category, so a printer setting passed to process is rejected rather than "
                                      "written. bundles reloads local preset bundles. To read presets and settings, "
                                      "use orca.host.preset_bundle().");
    auto bundles = preset.def_submodule("bundles", "Local preset bundle operations");
    bundles.def("reload_local", &reload_local_bundle, py::arg("bundle_id"),
                "Reload one local preset bundle from the active user's _local directory.");
    bundles.def("list_ids", &list_bundle_ids,
                "IDs of local preset bundles in the active user's _local directory that reload_local() accepts, "
                "including bundles added after startup.");

    py::enum_<Preset::Type>(host, "PresetType",
                            "Kind of preset: the type of a Preset, and the category a PresetEditor, PresetTransaction "
                            "or PresetUpdateResult targets.")
        .value("Invalid", Preset::TYPE_INVALID, "No preset type (an unset value).")
        .value("Print", Preset::TYPE_PRINT,
               "Process preset, the print settings the GUI calls \"process\" (PresetBundle.prints).")
        .value("SlaPrint", Preset::TYPE_SLA_PRINT, "SLA print preset; not used by OrcaSlicer's FFF printers.")
        .value("Filament", Preset::TYPE_FILAMENT, "Filament preset (PresetBundle.filaments).")
        .value("SlaMaterial", Preset::TYPE_SLA_MATERIAL, "SLA resin preset; not used by OrcaSlicer's FFF printers.")
        .value("Printer", Preset::TYPE_PRINTER, "Printer (machine) preset (PresetBundle.printers).")
        .value("PhysicalPrinter", Preset::TYPE_PHYSICAL_PRINTER,
               "A physical printer's connection settings; no Preset has this type.")
        .value("Plate", Preset::TYPE_PLATE,
               "Per-plate settings of the project; no PresetBundle collection holds this type.")
        .value("Model", Preset::TYPE_MODEL,
               "Per-object and per-part settings of the project; no PresetBundle collection holds this type.");

    py::class_<PresetUpdateResult>(preset, "PresetUpdateResult",
                                   "Outcome of one committed preset transaction, returned by "
                                   "PresetTransaction.commit(). Filament slot numbers are 1-based.")
        .def_readonly("type", &PresetUpdateResult::type, "PresetType of the edited preset: Print, Printer or Filament.")
        .def_readonly("target_name", &PresetUpdateResult::target_name, "Internal name of the preset that was edited.")
        .def_readonly("selection_changed", &PresetUpdateResult::selection_changed,
                      "True when begin_update() switched the edited preset to reach the target; False when the target "
                      "was already being edited.")
        .def_readonly("slot_number", &PresetUpdateResult::slot_number,
                      "1-based filament slot the transaction targeted; -1 for process and printer.")
        .def_readonly("slot_assignment_changed", &PresetUpdateResult::slot_assignment_changed,
                      "True when begin_update() assigned the requested preset to the filament slot in place of its "
                      "previous one; always False for process and printer.")
        .def_readonly("shared_slots", &PresetUpdateResult::shared_slots,
                      "1-based filament slots whose assigned preset is the edited one, all of which the edit affects; "
                      "empty for process and printer.")
        .def_readonly("keys", &PresetUpdateResult::keys, "Setting keys that were applied, in staging order.")
        .def_readonly("dirty", &PresetUpdateResult::dirty,
                      "True when the edited preset now differs from its saved state and still needs saving.")
        .def_property_readonly(
            "category", [](const PresetUpdateResult& result) { return preset_tab_name(result.type); },
            "Category of the edited preset: \"process\", \"printer\" or \"filament\".")
        .def("__repr__", [](const PresetUpdateResult& result) {
            return std::string("<PresetUpdateResult ") + preset_tab_name(result.type) + " \"" + result.target_name
                   + "\" keys=" + std::to_string(result.keys.size()) + ">";
        });

    py::class_<PresetTransaction>(preset, "PresetTransaction",
                                  "A staged, single-target preset edit returned by begin_update(). set() stages a "
                                  "serialized value without mutating the preset; commit() validates and applies the "
                                  "whole batch through the normal setting-change path. begin_update() blocks the "
                                  "background Script worker until any selection or unsaved-edit dialog is answered, "
                                  "while the UI thread stays responsive; set() and commit() run synchronously on the "
                                  "same worker. Nothing is saved to disk: the edited preset stays dirty until the user "
                                  "saves it.")
        .def_readonly("type", &PresetTransaction::type, "PresetType of the target preset: Print, Printer or Filament.")
        .def_readonly("target_name", &PresetTransaction::target_name,
                      "Internal name of the preset this transaction edits.")
        .def_readonly("selection_changed", &PresetTransaction::selection_changed,
                      "True when begin_update() switched the edited preset to reach the target; False when the target "
                      "was already being edited.")
        .def_property_readonly(
            "slot_number",
            [](const PresetTransaction& transaction) { return transaction.filament ? transaction.slot_number : -1; },
            "1-based filament slot this transaction was created for; -1 for process and printer.")
        .def_property_readonly(
            "category", [](const PresetTransaction& transaction) { return preset_tab_name(transaction.type); },
            "Category of the target preset: \"process\", \"printer\" or \"filament\".")
        .def_property_readonly(
            "staged_keys",
            [](const PresetTransaction& transaction) {
                std::vector<std::string> keys;
                keys.reserve(transaction.staged.size());
                for (const auto& entry : transaction.staged)
                    keys.push_back(entry.first);
                return keys;
            },
            "Setting keys staged with set() so far, in staging order, as a new list.")
        .def("set", &PresetTransaction::set, py::arg("key"), py::arg("value"),
             "Stage one setting change. key must be a setting of the target preset category and value its "
             "serialized string form; staging validates neither, so commit() is where an invalid key or value "
             "fails. Staging the same key twice in one transaction raises ValueError (conflicting duplicate "
             "writes are rejected). After commit() the transaction is closed.")
        .def("commit", &PresetTransaction::commit,
             "Validate and apply every staged setting through the same path as editing it in the GUI, then return "
             "a PresetUpdateResult. An invalid key or value raises ValueError; a changed target or filament slot "
             "mapping raises RuntimeError; a failure leaves none of the staged settings applied. The edited preset "
             "becomes dirty until the user saves it.");

    py::class_<PresetEditor>(preset, "PresetEditor",
                             "Handle to a category of currently edited preset. It is not a snapshot: it always "
                             "resolves the preset the matching tab edits now, so it stays usable across preset "
                             "switches. update() edits the preset in place; the change is not saved to disk.")
        .def_readonly("type", &PresetEditor::type,
                      "PresetType this editor edits: Print for orca.host.preset.process, Printer for "
                      "orca.host.preset.printer, Filament for orca.host.preset.filament.")
        .def(
            "begin_update",
            [](const PresetEditor& editor, const std::string& preset_name) {
                if (editor.type == Preset::TYPE_FILAMENT)
                    throw std::invalid_argument("begin_update on the filament editor takes "
                                                "(slot_number, preset_name=\"\"); use orca.host.preset.filament.begin_update(slot_number, ...)");
                return begin_preset_update(editor.type, preset_name);
            },
            py::arg("preset_name") = "",
            "Resolve and bind this category's target preset and return a PresetTransaction. With no preset_name the "
            "currently active preset is targeted; a preset_name must be a preset this category's selector offers, by "
            "the name it shows or its internal name, and is "
            "selected through the normal preset-selection flow (which prompts about unsaved edits). A hidden preset "
            "that suits the printer is shown in the selectors first and stays shown until the app restarts. The call blocks "
            "the background Script worker until any such dialog is answered, leaving the UI thread responsive. A "
            "selection the user cancels raises RuntimeError; an unavailable or invalid target raises ValueError. "
            "Either failure creates no transaction and stages no setting. Call it from the background Script worker, "
            "never from the UI thread. This form is for process and printer; filament takes "
            "begin_update(slot_number, preset_name=\"\").")
        .def(
            "begin_update",
            [](const PresetEditor& editor, int slot_number, const std::string& preset_name) {
                if (editor.type != Preset::TYPE_FILAMENT)
                    throw std::invalid_argument("The slot_number form of begin_update() is only available on "
                                                "orca.host.preset.filament; call begin_update(preset_name=\"\") on this editor");
                return begin_filament_update(slot_number, preset_name);
            },
            py::arg("slot_number"), py::arg("preset_name") = "",
            "Resolve and bind a filament preset by 1-based slot and return a PresetTransaction. Omitting preset_name "
            "targets the preset assigned to the slot; supplying a different name selects it for the slot through the "
            "normal flow (prompting about unsaved edits) and, once that succeeds, assigns it to the slot as picking it "
            "in the slot's sidebar selector does: the slot's filament colour follows the preset, the flushing volumes "
            "are recalculated unless their automatic calculation is disabled, the project is marked changed and every "
            "plate's slice result is invalidated. A hidden preset that suits the printer is shown in the selectors "
            "first and stays shown until the app restarts. A requested preset "
            "this slot's dropdown still does not offer is rejected before that flow, so it raises no prompt; the slot's own "
            "current assignment stays targetable even when the dropdown hides it. A selection the user cancels raises "
            "RuntimeError and changes neither the slot assignment nor any setting. An out-of-range, empty or "
            "unresolved slot is rejected, and a mixed-color virtual slot is rejected whether or not preset_name is "
            "given. The transaction edits the shared preset, so every slot assigned to it is affected. The call "
            "blocks the background Script worker until the selection settles, leaving the UI thread responsive; call "
            "it from that worker, never from the UI thread.")
        .def(
            "update",
            [](const PresetEditor& editor, const std::string& key, const std::string& value) {
                update_edited_preset(editor.type, key, value);
            },
            py::arg("key"), py::arg("value"),
            "Set one setting of the currently edited preset and run the same committed-change path as "
            "editing the setting in the GUI; the edited preset becomes dirty until saved/exported. "
            "key must be a setting of this preset category and value its serialized string form, "
            "otherwise ValueError is raised and nothing is written. A setting edited through a field "
            "runs that field's validation even before its page has been opened; one the GUI edits "
            "outside a field (such as compatible printers) also recomputes preset compatibility. "
            "Equivalent to begin_update() with the active preset as target, then set(key, value) "
            "and commit(); like begin_update() this call blocks the background Script worker.")
        .def(
            "update",
            [](const PresetEditor& editor, int slot_number, const std::string& key, const std::string& value) {
                if (editor.type != Preset::TYPE_FILAMENT)
                    throw std::invalid_argument("The slot_number form of update() is only available on "
                                                "orca.host.preset.filament; call update(key, value) on this editor");
                update_filament_slot(slot_number, key, value);
            },
            py::arg("slot_number"), py::arg("key"), py::arg("value"),
            "Set one setting of the filament preset assigned to a 1-based filament slot. When that preset "
            "is not the one currently edited the call switches to it through the normal preset-selection "
            "flow, so an unsaved edited preset still prompts the user and a declined prompt cancels the "
            "call with an error and no change. An out-of-range, empty or unresolved slot is rejected with "
            "a clear error, as is a mixed-color virtual slot, which has no single preset to edit. Other "
            "preset editors accept only update(key, value); this form raises "
            "ValueError on them. Equivalent to begin_update(slot_number) with an omitted preset_name "
            "(the slot's current assignment is kept), then set(key, value) and commit(); like "
            "begin_update() this call blocks the background Script worker.");

    preset.attr("process")  = py::cast(PresetEditor{Preset::TYPE_PRINT});
    preset.attr("printer")  = py::cast(PresetEditor{Preset::TYPE_PRINTER});
    preset.attr("filament") = py::cast(PresetEditor{Preset::TYPE_FILAMENT});

    py::class_<Preset, std::unique_ptr<Preset, py::nodelete>>(
        host, "Preset",
        "One named set of process, filament or printer settings. Get presets from orca.host.preset_bundle(): its "
        "prints, filaments and printers collections, or the current_*_preset() shortcuts. A Preset is a live "
        "reference into the app's preset store, not a copy: it reflects later edits, and adding or deleting presets "
        "(saving a new preset, loading a project, reloading a bundle) invalidates it, so look presets up again "
        "instead of keeping them. Read a setting with config_value(key); change one through orca.host.preset.process, "
        ".printer or .filament (PresetEditor), never through this object.")
        .def_readonly("type", &Preset::type, "PresetType of this preset: Print (process), Filament or Printer.")
        .def_readonly("name", &Preset::name,
                      "Internal name, unique within its collection. find_preset(), "
                      "PresetEditor.begin_update(preset_name) and the filament slot assignments use it. A preset "
                      "from an imported bundle is named \"_local/<bundle_id>/<name>\" or "
                      "\"_subscribed/<bundle_id>/<name>\".")
        .def_readonly("alias", &Preset::alias,
                      "Logical name without the printer suffix, such as \"Generic PLA\" for \"Generic PLA @BBL X1C\"; "
                      "label() shows it in place of name. Empty when the preset has none. find_preset() does not "
                      "match it.")
        .def_readonly("file", &Preset::file,
                      "Path of the file this preset was loaded from or is saved to; empty for the built-in default "
                      "preset.")
        .def_readonly("is_default", &Preset::is_default,
                      "True for the built-in default preset at index 0 of each collection (\"Default Setting\", "
                      "\"Default Filament\" or \"Default Printer\"), which holds every setting's default value.")
        .def_readonly("is_external", &Preset::is_external,
                      "True for a preset loaded from a file outside OrcaSlicer's preset folders, such as a config "
                      "file or the settings stored in a G-code or 3MF project.")
        .def_readonly("is_system", &Preset::is_system,
                      "True for a read-only system preset from a vendor profile; changes to it can only be saved as a "
                      "new user preset.")
        .def_readonly("is_visible", &Preset::is_visible,
                      "Whether the preset selectors offer it: False for a system printer whose model or variant, or a "
                      "system filament, is not enabled in the printer and filament selection. The default preset is "
                      "visible only when nothing else is available.")
        .def_readonly("is_dirty", &Preset::is_dirty,
                      "True when the preset has unsaved changes. Only the selected preset and its edited copy "
                      "(PresetCollection.edited_preset()) can be dirty.")
        .def_readonly("is_compatible", &Preset::is_compatible,
                      "Whether it suits the selected printer (its compatible_printers or "
                      "compatible_printers_condition) and, for a filament, the edited process (compatible_prints). The "
                      "selectors hide incompatible presets. Always True for printer presets.")
        .def_readonly("is_project_embedded", &Preset::is_project_embedded,
                      "True for a preset stored in the loaded 3MF project rather than in the user's preset folder.")
        .def_readonly("bundle_id", &Preset::bundle_id,
                      "ID of the imported preset bundle this preset came from (an Orca Cloud UUID, or a name and "
                      "timestamp for an external bundle); empty when it is not from a bundle.")
        .def("is_user", &Preset::is_user,
             "True for a user preset: not the default preset, not a system preset, not embedded in the project and "
             "not from a bundle.")
        .def("is_from_bundle", &Preset::is_from_bundle,
             "True when the preset came from an imported preset bundle, that is when bundle_id is set.")
        .def("label", &Preset::label, py::arg("no_alias") = false,
             "Display label: alias, or name when there is no alias or no_alias is True, prefixed with the GUI's "
             "unsaved-changes marker (\"* \" in English) when is_dirty. Look presets up by name, not by label.")
        .def(
            "config_keys", [](const Preset& preset) { return preset.config.keys(); },
            "Names of every setting in this preset. A preset holds its complete config, so a user preset also lists "
            "the settings it inherits unchanged from its parent.")
        .def(
            "config_value",
            [](const Preset& preset, const std::string& key) { return config_value_or_none(preset.config, key); },
            py::arg("key"),
            "Serialized value of one setting, in the string form PresetTransaction.set() and PresetEditor.update() "
            "take: numbers as text, booleans \"1\"/\"0\", enums by name, percentages with \"%\", vector options "
            "comma-separated (string lists ';'-separated, quoted where needed). None when the preset has no such "
            "setting.");

    py::class_<PresetCollection, std::unique_ptr<PresetCollection, py::nodelete>>(
        host, "PresetCollection",
        "All presets of one type: the prints (process), filaments or printers collection of "
        "orca.host.preset_bundle(). Index 0 is the built-in default preset; hidden and incompatible presets are "
        "included. Items are live Preset references (see Preset). Read a setting with Preset.config_value(key); to "
        "change one, use orca.host.preset.process, .filament or .printer (PresetEditor), which edit the currently "
        "edited preset the way the settings tab does.")
        .def("size", &PresetCollection::size,
             "Number of presets, including the default preset at index 0; preset(index) accepts 0 to size() - 1.")
        .def(
            "get_selected_preset",
            [](PresetCollection& collection) -> Preset& { return collection.get_selected_preset(); },
            py::return_value_policy::reference_internal,
            "The selected preset as last saved, without unsaved changes; edited_preset() has the values in effect. "
            "Same as selected_preset().")
        .def(
            "selected_preset", [](PresetCollection& collection) -> Preset& { return collection.get_selected_preset(); },
            py::return_value_policy::reference_internal,
            "The selected preset as last saved, without unsaved changes; edited_preset() has the values in effect.")
        .def("get_selected_preset_name", &PresetCollection::get_selected_preset_name,
             "Internal name of the selected preset, or \"\" when none is selected. Same as selected_preset_name().")
        .def("selected_preset_name", &PresetCollection::get_selected_preset_name,
             "Internal name of the selected preset, or \"\" when none is selected.")
        .def(
            "get_edited_preset", [](PresetCollection& collection) -> Preset& { return collection.get_edited_preset(); },
            py::return_value_policy::reference_internal,
            "The selected preset including its unsaved changes. Same as edited_preset().")
        .def(
            "edited_preset", [](PresetCollection& collection) -> Preset& { return collection.get_edited_preset(); },
            py::return_value_policy::reference_internal,
            "The selected preset including its unsaved changes: the values the settings tab shows. The object follows "
            "the collection's selection, so once another preset is selected it describes that one. For filaments it is "
            "the preset the Filament tab shows; PresetBundle.current_filament_presets() gives every slot's preset.")
        .def(
            "preset",
            [](PresetCollection& collection, size_t index) -> Preset& {
                if (index >= collection.size())
                    throw py::index_error("preset index out of range");
                return collection.preset(index);
            },
            py::arg("index"), py::return_value_policy::reference_internal,
            "The preset at a 0-based index, 0 being the built-in default preset; at the selected index it is the "
            "edited copy with unsaved changes. Raises IndexError when index >= size().")
        .def(
            "find_preset",
            [](PresetCollection& collection, const std::string& name) -> Preset* {
                return collection.find_preset(name);
            },
            py::arg("name"), py::return_value_policy::reference_internal,
            "The preset with this internal name (a renamed system preset is also found by a former name), or None. "
            "For the selected preset it is the edited copy with unsaved changes. Aliases and display labels do not "
            "match.")
        .def(
            "preset_names",
            [](const PresetCollection& collection) {
                std::vector<std::string> names;
                names.reserve(collection.get_presets().size());
                for (const Preset& preset : collection.get_presets())
                    names.push_back(preset.name);
                return names;
            },
            "Internal names of all presets in index order, so preset_names()[i] is preset(i).name; the default preset "
            "comes first.");

    py::class_<PresetBundle, std::unique_ptr<PresetBundle, py::nodelete>>(
        host, "PresetBundle",
        "All loaded presets and the current selection; orca.host.preset_bundle() returns the app's live bundle. "
        "prints, filaments and printers are the PresetCollections; current_process_preset(), current_printer_preset() "
        "and current_filament_presets() are the presets in effect, and full_config_value(key) reads the merged "
        "settings slicing would use. It only reads: to change a setting use orca.host.preset.process, .filament or "
        ".printer (PresetEditor.update(key, value) or begin_update()), which apply it through the settings tab.")
        .def_property_readonly(
            "prints", [](PresetBundle& bundle) -> PresetCollection& { return bundle.prints; },
            py::return_value_policy::reference_internal, "PresetCollection of process (print settings) presets.")
        .def_property_readonly("printers", &printer_presets, py::return_value_policy::reference_internal,
                               "PresetCollection of printer presets.")
        .def_property_readonly(
            "filaments", [](PresetBundle& bundle) -> PresetCollection& { return bundle.filaments; },
            py::return_value_policy::reference_internal,
            "PresetCollection of filament presets; current_filament_presets() says which one each slot uses.")
        .def_property_readonly(
            "sla_prints", [](PresetBundle& bundle) -> PresetCollection& { return bundle.sla_prints; },
            py::return_value_policy::reference_internal,
            "PresetCollection of SLA print presets; not used by OrcaSlicer's FFF printers.")
        .def_property_readonly(
            "sla_materials", [](PresetBundle& bundle) -> PresetCollection& { return bundle.sla_materials; },
            py::return_value_policy::reference_internal,
            "PresetCollection of SLA resin presets; not used by OrcaSlicer's FFF printers.")
        .def(
            "current_process_preset", [](PresetBundle& bundle) -> Preset& { return bundle.prints.get_edited_preset(); },
            py::return_value_policy::reference_internal,
            "The edited process preset, with its unsaved changes; same as prints.edited_preset().")
        .def(
            "current_print_preset", [](PresetBundle& bundle) -> Preset& { return bundle.prints.get_edited_preset(); },
            py::return_value_policy::reference_internal, "Same as current_process_preset().")
        .def(
            "current_printer_preset",
            [](PresetBundle& bundle) -> Preset& { return bundle.printers.get_edited_preset(); },
            py::return_value_policy::reference_internal,
            "The edited printer preset, with its unsaved changes; same as printers.edited_preset().")
        .def(
            "current_filament_preset_names", [](PresetBundle& bundle) { return bundle.filament_presets; },
            "Preset name stored for each filament slot, as a new list where index i is slot i + 1.")
        .def("current_filament_presets", &current_filament_presets,
             "The preset assigned to each filament slot, as a list where index i is slot i + 1, with None for a slot "
             "whose name matches no preset. A slot assigned the edited filament preset gives the edited copy with "
             "unsaved changes.")
        .def(
            "full_config_keys", [](const PresetBundle& bundle) { return bundle.full_config().keys(); },
            "Names of every setting in the merged config full_config_value() reads.")
        .def(
            "full_config_value",
            [](const PresetBundle& bundle, const std::string& key) {
                return config_value_or_none(bundle.full_config(), key);
            },
            py::arg("key"),
            "Serialized value of one setting from the merged config slicing would use: the edited process and printer "
            "presets, each filament slot's preset and the project settings. Per-filament settings hold one value per "
            "slot, in slot order. None when key is not a setting. Each call rebuilds the merged config.");
}

} // namespace Slic3r
