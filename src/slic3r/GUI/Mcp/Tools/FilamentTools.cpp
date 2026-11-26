// The place Filaments: filament_list, the filament list as the sidebar shows it; the filament slot commands made from
// the + and - buttons above it and a slot's menu items; the mixed filaments, a slot's colour and the flushing, set
// as their dialogs' OK sets them; filament_sync_ams; and filament_install and filament_uninstall, what the System
// Filaments dialog's Confirm installs, on the operations of GUI/Operations/InstallOps.hpp.

#include "slic3r/GUI/Mcp/McpTools.hpp"
#include "slic3r/GUI/ActionRegistry.hpp"
#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/CommandTypes.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MixedFilamentDialog.hpp"
#include "slic3r/GUI/NativeCommandMenuItems.hpp"
#include "slic3r/GUI/NativeCommands.hpp"
#include "slic3r/GUI/Operations/InstallOps.hpp"
#include "slic3r/GUI/Operations/Outcome.hpp"
#include "slic3r/GUI/Operations/PresetOps.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PresetComboBoxes.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/FilamentMixer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <wx/defs.h>
#include <wx/event.h>
#include <wx/string.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;

namespace {

// What a slot's colour button does with the colour its picker gives: the slot's colour, then its
// EVT_FILAMENT_COLOR_CHANGED, which recalculates the flushing volumes where Auto flush after changing is on. UI thread.
void apply_slot_colour(std::size_t slot, const std::string& colour)
{
    auto& combos = ui_plater().sidebar().combos_filament();
    auto  combo  = std::find_if(combos.begin(), combos.end(),
                                [slot](PlaterPresetComboBox* c) { return c != nullptr && c->get_filament_idx() == int(slot); });
    if (combo == combos.end())
        throw ToolFailure("Filament slot " + std::to_string(slot + 1) + " has no colour button");
    (*combo)->sync_colour_config({colour}, false);
    wxCommandEvent* evt = new wxCommandEvent(EVT_FILAMENT_COLOR_CHANGED);
    evt->SetInt(int(slot));
    wxQueueEvent(&ui_plater(), evt);
}

// filament_set_colour: one slot's colour, checked as check_filament_colours() checks a colour list. The reply reads it
// back once the change has settled.
ToolResult set_filament_colour(const json& args, ToolContext& ctx)
{
    const int         slot   = arg_int(args, "filament_slot", 1);
    const std::string colour = arg_string(args, "colour");
    bool              changed = false;
    ctx.run_ui([&]() -> json {
        ui_plater();
        const std::vector<std::string>& current =
            wxGetApp().preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour")->values;
        if (slot < 1 || std::size_t(slot) > current.size())
            throw ToolFailure("filament_slot must be 1.." + std::to_string(current.size()));
        std::vector<std::string> colours = current;
        colours[std::size_t(slot) - 1]   = colour;
        colours                          = check_filament_colours(colours, current, mixed_slots());
        changed                          = colours[std::size_t(slot) - 1] != current[std::size_t(slot) - 1];
        if (changed)
            apply_slot_colour(std::size_t(slot) - 1, colours[std::size_t(slot) - 1]);
        return json();
    });
    settle(ctx);
    return ToolResult::ok(ctx.run_ui([&]() -> json {
        const std::vector<std::string>& colours =
            wxGetApp().preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour")->values;
        return {{"filament_slot", slot}, {"colour", colours[std::size_t(slot) - 1]}, {"changed", changed}};
    }));
}

// filament_list: the filament list as the sidebar shows it, a row per slot in config order (the physical filaments,
// then the mixed ones), with the flushing its Flushing volumes and Purge mode buttons set. UI thread.
json filament_list()
{
    Sidebar&                        sidebar    = ui_plater().sidebar();
    PresetBundle&                   bundle     = *wxGetApp().preset_bundle;
    const std::vector<std::string>& colours    = bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values;
    const auto*                     components = bundle.project_config.option<ConfigOptionStrings>("filament_mixed_components");
    const std::vector<std::size_t>  broken     = sidebar.broken_mixed_filaments();
    json                            rows       = json::array();
    for (std::size_t i = 0; i < colours.size(); ++i) {
        const bool mixed = bundle.is_mixed_filament(i);
        json       row{{"filament_slot", i + 1}, {"mixed", mixed}, {"colour", colours[i]}};
        if (!mixed) {
            const std::string name   = i < bundle.filament_presets.size() ? bundle.filament_presets[i] : std::string();
            Preset*           preset = bundle.filaments.find_preset(name);
            std::string       display;
            row["preset"] = name;
            row["type"]   = preset != nullptr ? preset->config.get_filament_type(display) : std::string();
            rows.push_back(std::move(row));
            continue;
        }
        // The components as stored, a deleted one as 0; the ratios and the gradient as the Edit Mixed Filament dialog
        // reads them.
        json parts = json::array();
        if (components != nullptr && i < components->size())
            for (const unsigned int component : parse_mixed_components(components->values[i]))
                parts.push_back(component);
        const MixedFilamentResult mix = sidebar.mixed_filament_result(i);
        row["components"]             = std::move(parts);
        row["ratios"]                 = mix.ratios;
        row["gradient"]               = nullptr;
        if (mix.gradient_enabled && mix.components.size() == 2) {
            // Direction 0 starts with the first component's share high: it leads at the bottom (filament_mixed_gradient_range).
            const bool first_at_bottom = mix.gradient_direction == 0;
            row["gradient"]            = {{"bottom", first_at_bottom ? mix.components[0] : mix.components[1]},
                                          {"top", first_at_bottom ? mix.components[1] : mix.components[0]},
                                          {"per_part", mix.per_part_gradient},
                                          {"curve", mix.gradient_curve.empty() ? "linear" : "custom"}};
        }
        row["broken"] = std::find(broken.begin(), broken.end(), i) != broken.end();
        rows.push_back(std::move(row));
    }
    const DynamicPrintConfig settings = project_settings();
    json                     out{{"filaments", std::move(rows)},
                                 {"flush_multiplier", settings.option<ConfigOptionFloats>("flush_multiplier")->values},
                                 {"flush_volumes_matrix", settings.option<ConfigOptionFloats>("flush_volumes_matrix")->values},
                                 {"prime_volume_mode", settings.opt_serialize("prime_volume_mode")}};
    if (std::vector<std::string> offered = project_offered("prime_volume_mode"); !offered.empty())
        out["prime_volume_modes"] = std::move(offered);
    return out;
}

// ---- The filament slot commands made from menu items ----

// What a filament slot command answers for a refusal: as information where its menu item told it so (no slots to change, no
// + and - buttons for the printer, no room for another slot, the only slot), else as an error.
AppActionRunResult slot_refused(const Ops::Refusal& refusal)
{
    const std::string code = refusal.code;
    const bool info = code == "filaments_unavailable" || code == "one_filament_per_extruder" || code == "too_many_filaments" ||
                      code == "last_filament_slot";
    return NativeCommands::refused(refusal, info ? AppActionRunResult::Level::Info : AppActionRunResult::Level::Error);
}

// What a filament slot command that ran shows a person, `message`, and replies to a call: `reply` with how many slots
// the project has afterwards.
AppActionRunResult filament_reply(const wxString& message, json reply)
{
    reply["filament_slots"] = wxGetApp().preset_bundle->filament_presets.size();
    AppActionRunResult result{AppActionRunResult::Level::Succeeded, message};
    result.reply = reply.dump();
    return result;
}

AppActionRunResult filament_add_command(const json&)
{
    const Ops::Outcome<int> added = Ops::add_filament_slot();
    if (!added)
        return slot_refused(added.refusal());
    // TRN %1% is a filament slot number.
    return filament_reply(format_wxstr(_L("Added filament slot %1%."), added.value()), {{"filament_slot", added.value()}});
}

AppActionRunResult filament_delete_command(const json& args)
{
    Ops::DeleteFilamentArgs op;
    if (args.contains("filament_slot"))
        op.filament_slot = arg_int(args, "filament_slot", 1);
    if (args.contains("merge_into"))
        op.merge_into = arg_int(args, "merge_into", 1);
    if (args.contains("break_mix"))
        op.break_mix = arg_bool(args, "break_mix", false);
    const Ops::Outcome<Ops::DeletedFilament> deleted = Ops::delete_filament_slot(op);
    if (!deleted)
        return slot_refused(deleted.refusal());
    json reply{{"filament_slot", deleted.value().filament_slot}};
    if (deleted.value().merged_into)
        reply["merged_into"] = *deleted.value().merged_into;
    if (!deleted.value().broken_mixed.empty())
        reply["broken_mixed"] = deleted.value().broken_mixed;
    // TRN %1% is a filament slot number.
    return filament_reply(format_wxstr(_L("Deleted filament slot %1%."), deleted.value().filament_slot), std::move(reply));
}

// filament_sync_ams: with no arguments, as its Speed Dial action, the sync dialog's run, which asks and warns; with
// mode, the sync with that choice of the dialog, which opens nothing. Reply: each slot's preset and colour afterwards.
AppActionRunResult sync_ams_command(const json& args)
{
    if (!args.contains("mode"))
        return MenuItems::sync_ams();
    const Ops::AmsSyncMode             mode   = arg_string(args, "mode") == "map" ? Ops::AmsSyncMode::Map : Ops::AmsSyncMode::Overwrite;
    const Ops::Outcome<Ops::AmsSynced> synced = Ops::sync_filaments_from_ams(mode);
    if (!synced)
        return NativeCommands::refused(synced.refusal(), AppActionRunResult::Level::Error);
    json slots = json::array();
    for (std::size_t slot = 0; slot < synced.value().presets.size(); ++slot) {
        json item{{"filament_slot", slot + 1}, {"preset", synced.value().presets[slot]}};
        if (slot < synced.value().colours.size())
            item["colour"] = synced.value().colours[slot];
        slots.push_back(std::move(item));
    }
    json reply{{"mode", arg_string(args, "mode")}, {"filaments", std::move(slots)}};
    if (!synced.value().unknowns.empty())
        reply["mapped_to_generic"] = synced.value().unknowns;
    return menu_reply(reply);
}

// What the filament commands reply when an operation refuses: the names a refusal carries under the key that says
// what they are.
ToolFailure filament_refused(const Ops::Refusal& refusal)
{
    const std::string code = refusal.code;
    if (code == "no_such_filament" || code == "no_filament_vendor")
        return refused(refusal, "preset_list {type: \"filament\", include_uninstalled: true} lists the system filaments.", "similar");
    if (code == "fits_no_printer")
        return refused(refusal, "Name one of its presets.", "presets");
    if (code == "no_matching_filament")
        return refused(refusal, "", "filament_types");
    if (code == "filament_in_use")
        return refused(refusal, "Select another filament for the slot first (preset_select with filament_slot).", "presets");
    return refused(refusal);
}

// filament_install's and filament_uninstall's function: the names, or the vendor and filament type, a call gives.
CommandFn change_filaments(bool install)
{
    return [install](const json& args, ToolContext& ctx) {
        Ops::InstallFilamentsArgs op;
        if (const auto names = args.find("names"); names != args.end())
            for (const json& name : *names)
                op.names.push_back(name.get<std::string>());
        op.vendor        = arg_string(args, "vendor");
        op.filament_type = arg_string(args, "filament_type");
        if (op.names.empty() && op.vendor.empty() && op.filament_type.empty())
            throw ToolFailure("Give names, or vendor and filament_type");
        if (!op.names.empty() && (args.contains("vendor") || args.contains("filament_type")))
            throw ToolFailure("Give names, or vendor and filament_type, not both");
        if (op.names.empty() && (op.vendor.empty() || op.filament_type.empty()))
            throw ToolFailure("vendor and filament_type go together");
        json reply = ctx.run_ui([&] {
            ui_plater();
            const Ops::Outcome<Ops::InstalledFilaments> done = install ? Ops::install_filaments(op) : Ops::uninstall_filaments(op);
            if (!done)
                throw filament_refused(done.refusal());
            json out{{install ? "installed" : "uninstalled", done.value().changed}};
            if (!done.value().unchanged.empty())
                out[install ? "installed_already" : "not_installed"] = done.value().unchanged;
            if (!done.value().defaults_installed.empty())
                out["filaments_installed"] = done.value().defaults_installed;
            return out;
        });
        settle(ctx);
        return ToolResult::ok(reply);
    };
}

} // namespace

void register_filament_tools(CommandCatalog& catalog)
{
    using NativeCommands::action;

    // The + and - buttons above the filament list and a slot's Delete menu item, each named as the button or menu item is, in the Speed
    // Dial's section Filament.
    const std::string slot_buttons = "Refused for a printer that has one filament per extruder, which shows no + and - buttons.";
    catalog.add(inspect_tool(
        "filament_list", L("List the filaments"),
        "Read the filament list as the sidebar shows it. filaments has a row per slot in config order, the physical "
        "filaments first, then the mixed ones: `filament_slot` (1-based), mixed, colour (#RRGGBB) and, for a physical "
        "filament, preset and type; for a mixed filament, components (the physical slots it mixes, 1-based, 0 for one "
        "deleted), ratios (whole percentages, as `filament_add_mixed` takes them), gradient (null, or `{bottom, top: the "
        "components that lead at the bottom and at the top, per_part, curve: linear or custom}`) and broken (a component "
        "deleted or of another filament type: the sidebar marks it, slicing an object on it is refused, and "
        "`filament_edit_mixed` repairs it). Then the flushing the list's Flushing volumes and Purge mode buttons set: "
        "`flush_multiplier` (one per nozzle), `flush_volumes_matrix` (per nozzle, slots x slots from row to column in config "
        "order over every slot, before the multiplier: the dialog shows each entry multiplied by its nozzle's multiplier, "
        "and the G-code purges that product) and `prime_volume_mode`, with `prime_volume_modes` where the printer offers a "
        "choice. The printer keeps all of it across new projects and restarts; a saved project keeps its own.",
        object_schema({}),
        [](const json&, ToolContext& ctx) {
            return ToolResult::ok(ctx.run_ui([] {
                ui_plater();
                return filament_list();
            }));
        }));
    catalog.add(reaching({"add_filament_slot"},
                         own_action(menu_command("filament_add", L("Add Filament Slot"),
                                                "Add a filament slot, as the + button above the filament list does. It takes the next "
                                                "colour and the last slot's preset (change them with `filament_set_colour` and "
                                                "`preset_select` `filament_slot`). It goes after the physical slots: mixed "
                                                "filaments move up by one, and objects on them follow. " +
                                                    slot_buttons + " Reply: `filament_slot` (the new slot) and `filament_slots` (how many the "
                                                                   "project has).",
                                                object_schema({}), json(), filament_add_command),
                                    L("Filament"), "add_filament")));
    catalog.add(reaching(
        {"delete_filament_slot"},
        own_action(menu_command("filament_delete", L("Delete Filament Slot"),
                               "Delete a filament slot, as its Delete menu item does, the objects on it taking the first filament; "
                               "with `merge_into`, as its Merge with item does, the objects on it taking that slot. The slots after "
                               "it move down by one. A mixed filament (`filament_list`'s mixed rows) is deleted as the mixed "
                               "filament list deletes it, and not merged. A physical filament that a mixed filament mixes is "
                               "deleted as the sidebar deletes it: the mix is broken (`broken_mixed`) until `filament_edit_mixed` "
                               "names its filaments again. A merge into a mixed filament that mixes the slot asks first, as Merge "
                               "with does, and replies `blocked_by_dialog`, unless `break_mix` answers: `true` merges, `false` refuses "
                               "(`mixed_uses_slot`). Refused: the only physical slot, and a mixed filament while one physical "
                               "slot is left, where the sidebar hides the mixed filaments (`filament_add` first). " +
                                   slot_buttons + " Not in the undo history. Reply: `filament_slot` (the slot deleted), `merged_into` "
                                                  "(the slot its objects went to, as numbered afterwards), `filament_slots`, and "
                                                  "`broken_mixed` (the mixed filaments broken afterwards, as numbered then) where "
                                                  "there are any.",
                               object_schema({{"filament_slot", {{"type", "integer"},
                                                                 {"minimum", 1},
                                                                 {"description", "The 1-based filament slot. Default: the last "
                                                                                 "physical slot, as the - button."}}},
                                              {"merge_into", {{"type", "integer"},
                                                              {"minimum", 1},
                                                              {"description", "The 1-based slot the objects on it go to, as Merge "
                                                                              "with does. Default: they take the first filament."}}},
                                              {"break_mix", prop("boolean", "The answer to Merge with's question about a merge into "
                                                                            "a mixed filament that mixes the slot: `true` merges, "
                                                                            "`false` merges nothing. Default: the question opens.")}}),
                               json(), filament_delete_command),
                   L("Filament"), "delete_filament")));

    // The Add and Edit Mixed Filament dialogs' controls, for an agent that drives a dialog to set a gradient.
    const std::string mixed_dialog =
        "replies `blocked_by_dialog`. Its controls are named `filament_1` to `filament_3` (the components), gradient, "
        "`gradient_direction` (item 0 puts the first filament at the bottom, item 1 the second) and `per_part_gradient`, for "
        "`widget_set_value` with within the dialog's title; the gradient shows for a mix of two filaments only, and its curve "
        "is drawn, changed only by dragging it (`mouse_gesture`). Its OK makes what it shows; Cancel changes nothing.";
    const auto mix_props = [](const char* components) {
        return std::vector<Property>{
            {"components", array_prop({{"type", "integer"}, {"minimum", 1}}, components)},
            {"ratios", array_prop({{"type", "integer"}, {"exclusiveMinimum", 0}},
                                  "Each component's share in percent, in the order of components, adding up to 100.")}};
    };
    const auto mix_args = [](const json& args, std::vector<int>& components, std::vector<double>& ratios) {
        for (const json& component : args.value("components", json::array()))
            components.push_back(component.get<int>());
        for (const json& ratio : args.value("ratios", json::array()))
            ratios.push_back(ratio.get<double>());
    };
    const std::vector<Property> add_props = mix_props("The physical filament slots to mix, 1-based, two or more, each once, of "
                                                      "filaments of one type. Omitted with ratios: the dialog opens.");
    NativeCommand mixed = reaching(
        {"add_mixed_filament"},
        mutate_tool(
            "filament_add_mixed", L("Add Mixed Filament"),
            "Add a mixed filament: a slot that prints its physical filaments in turn, layer by layer, in the ratios given, as "
            "the Add Mixed Filament dialog's OK does with no gradient. components are two or more of the physical slots "
            "(1-based), each named once, whose filaments are of one type; ratios a whole percentage for each, adding up to "
            "100. It goes after every other slot. A mix the project has already is not added again: the reply names its "
            "slot. With neither components nor ratios it opens the Add Mixed Filament dialog, as the sidebar's button does, "
            "and " + mixed_dialog + " Not in the undo history. Reply: `filament_slot` (1-based, where one was added or "
            "found) and added (false for a mix the project had, or a dialog cancelled).",
            object_schema({add_props[0], add_props[1]}),
            [mix_args](const json& args, ToolContext& ctx) {
                Ops::AddMixedFilamentArgs op;
                mix_args(args, op.components, op.ratios);
                json reply = ctx.run_ui([&] {
                    ui_plater();
                    const Ops::Outcome<Ops::MixedFilament> mixed = Ops::add_mixed_filament(op);
                    if (!mixed)
                        throw refused(mixed.refusal());
                    json out{{"added", mixed.value().added}};
                    if (mixed.value().filament_slot > 0)
                        out["filament_slot"] = mixed.value().filament_slot;
                    return out;
                });
                settle(ctx);
                return ToolResult::ok(reply);
            }));
    mixed.examples = json::parse(R"json([
        {"title": "Mix the first two filaments half and half", "calls": [
            {"tool": "filament_add_mixed", "args": {"components": [1, 2], "ratios": [50, 50]}}]},
        {"title": "Mix three filaments, the third the most", "calls": [
            {"tool": "filament_add_mixed", "args": {"components": [1, 2, 3], "ratios": [25, 25, 50]}}]}])json");
    catalog.add(std::move(mixed));

    const std::vector<Property> edit_props = mix_props("The physical filament slots it mixes from now on, as `filament_add_mixed` "
                                                       "takes them. Omitted with ratios: the dialog opens.");
    NativeCommand edit_mixed = reaching(
        {"edit_mixed_filament"},
        mutate_tool(
            "filament_edit_mixed", L("Edit Mixed Filament"),
            "Change a mixed filament (`filament_list`'s mixed rows) as the Edit Mixed Filament dialog's OK does: components "
            "and ratios as `filament_add_mixed` takes them. The slot keeps its number and the objects on it, and its colour "
            "becomes the blend; a gradient stays for a mix of two filaments and goes for more, as the dialog shows none for "
            "them. An edit that names its filaments again repairs a broken mix. With `filament_slot` alone it opens the Edit "
            "Mixed Filament dialog on the mix, as a click on its swatch does, and " + mixed_dialog +
            " Not in the undo history. Reply: `filament_slot` and changed.",
            object_schema({{"filament_slot", {{"type", "integer"}, {"minimum", 1}, {"description", "The mixed filament's 1-based slot."}}},
                           edit_props[0], edit_props[1]},
                          {"filament_slot"}),
            [mix_args](const json& args, ToolContext& ctx) {
                Ops::EditMixedFilamentArgs op;
                op.filament_slot = arg_int(args, "filament_slot", 1);
                mix_args(args, op.components, op.ratios);
                json reply = ctx.run_ui([&] {
                    ui_plater();
                    const Ops::Outcome<Ops::EditedMixedFilament> edited = Ops::edit_mixed_filament(op);
                    if (!edited)
                        throw refused(edited.refusal());
                    return json{{"filament_slot", edited.value().filament_slot}, {"changed", edited.value().changed}};
                });
                settle(ctx);
                return ToolResult::ok(reply);
            }));
    edit_mixed.examples = json::parse(R"json([
        {"title": "Make the mixed filament in slot 3 mostly its first filament", "calls": [
            {"tool": "filament_edit_mixed", "args": {"filament_slot": 3, "components": [1, 2], "ratios": [75, 25]}}]}])json");
    catalog.add(std::move(edit_mixed));

    catalog.add(mutate_tool(
        "filament_set_colour", L("Set a filament's colour"),
        "Set a physical filament slot's colour, as its colour button's picker does: colour is #RRGGBB. Where Auto flush "
        "after changing is on, the flushing volumes are recalculated, so set `flush_volumes_matrix` (`filament_set_flushing`) "
        "after the colours. A mixed filament's colour is its components' blend and is not set. Reply: `filament_slot`, "
        "colour (as stored, upper case) and changed.",
        object_schema({{"filament_slot", {{"type", "integer"}, {"minimum", 1}, {"description", "The 1-based filament slot."}}},
                       {"colour", prop("string", "#RRGGBB.")}},
                      {"filament_slot", "colour"}),
        [](const json& args, ToolContext& ctx) { return set_filament_colour(args, ctx); }));

    catalog.add(mutate_tool(
        "filament_set_flushing", L("Set the flushing"),
        "Change the flushing the filament list's Flushing volumes and Purge mode buttons set, as their dialogs' OK does, "
        "any of: `flush_multiplier` (one per nozzle, or one value for all, within the Flushing volumes dialog's range), "
        "`flush_volumes_matrix` (per nozzle, slots x slots from row to column in config order over every slot, before the "
        "multiplier; whole, or one entry as `{index, value}`; a mixed filament's entries, which are never flushed, must be "
        "given as they are) and `prime_volume_mode` (one of `filament_list`'s `prime_volume_modes`). A colour change "
        "(`filament_set_colour`) can recalculate the matrix, so set it after the colours. Values are checked before "
        "anything changes. Reply: applied and changed.",
        object_schema({{"flush_multiplier", {{"type", {"number", "array"}}, {"description", "One value, or one per nozzle."}}},
                       {"flush_volumes_matrix", {{"type", {"array", "object"}}, {"description", "The whole matrix, or `{index, value}`."}}},
                       {"prime_volume_mode", prop("string", "The purge mode, e.g. \"Default\".")}}),
        [](const json& args, ToolContext& ctx) {
            json values = json::object();
            for (const char* key : {"flush_multiplier", "flush_volumes_matrix", "prime_volume_mode"})
                if (args.contains(key))
                    values[key] = args[key];
            if (values.empty())
                throw ToolFailure("Give flush_multiplier, flush_volumes_matrix or prime_volume_mode");
            return set_project_values(values, ctx, true);
        }));

    const std::string ams = "Synchronize the filament list from the connected printer's AMS.";
    NativeCommand     sync_ams = reaching(
        {"sync_filaments_from_ams"},
        menu_command("filament_sync_ams", L("Synchronize Filament List from AMS"),
                    ams + " With mode, as the sync dialog's Synchronize now does with that choice, without the dialog: map maps "
                          "each of the project's filaments to a slot, as the dialog's Mapping first does, which takes a project "
                          "with objects; overwrite replaces the filament list with the slots' filaments. Called with no arguments, "
                          "as its Speed Dial action, it opens the sync dialog where the project has objects (`blocked_by_dialog`), and "
                          "a warning where it cannot sync. With no printer connected, no AMS or no filament it is refused, saying so. "
                          "Not in the undo history. Reply: mode, filaments (each slot's preset and colour afterwards) and "
                          "`mapped_to_generic` (slots whose filament is unknown, mapped to a generic preset).",
                    object_schema({{"mode", enum_prop({"map", "overwrite"}, "The sync dialog's choice: map or overwrite.")}}),
                    json::array({action("sync_ams", L("Synchronize Filament List from AMS"), L("Printer"), "ams_fila_sync", json::object(),
                                        ams)}),
                    sync_ams_command));
    // It reads the connected printer's AMS.
    sync_ams.open_world = true;
    catalog.add(std::move(sync_ams));

    const json names_prop = {{"type", "array"},
                             {"minItems", 1},
                             {"items", {{"type", "string"}}},
                             {"description", "System filaments: a preset's name, or the short name the filament selector "
                                             "shows (\"Bambu PLA Basic\"), which stands for its preset of every installed "
                                             "printer it fits."}};
    const std::string kept = "The app keeps an installed printer's default filaments installed while no other installed filament "
                             "fits it: where the call leaves one so, they are installed again and listed under "
                             "`filaments_installed`, as the next start would. The other filaments, and the presets' unsaved "
                             "changes, are kept, and the selectors are refreshed. Not in the undo history.";
    NativeCommand install_filaments = reaching(
        {"install_filaments"},
        mutate_tool("filament_install", L("Install system filaments"),
                    "Install system filaments, for good, as ticking them in the System Filaments dialog and pressing Confirm "
                    "does: the filament selectors offer them. Name them by names, or with vendor and `filament_type` every "
                    "filament of that vendor and type that an installed printer fits. `preset_list {type: \"filament\", "
                    "include_uninstalled: true}` lists the system filaments, those not installed with `installed: false`. A name "
                    "no filament has is refused with the closest names. " +
                        kept + " Reply: installed (the presets installed), `installed_already`, `filaments_installed`.",
                    object_schema({{"names", names_prop},
                                   {"vendor", prop("string", "With `filament_type`, in place of names: a filament maker (\"Bambu "
                                                             "Lab\", \"Generic\"), in any case.")},
                                   {"filament_type", prop("string", "With vendor: a filament type (\"PLA\"), in any case.")}}),
                    change_filaments(true)));
    install_filaments.examples = json::parse(R"json([
        {"title": "Install a filament by the name the selector shows", "calls": [
            {"tool": "preset_list", "args": {"type": "filament", "include_uninstalled": true, "query": "PETG HF"}},
            {"tool": "filament_install", "args": {"names": ["Bambu PETG HF"]}}]},
        {"title": "Install every filament of a vendor and type", "calls": [
            {"tool": "filament_install", "args": {"vendor": "Generic", "filament_type": "PETG"}}]}])json");
    catalog.add(std::move(install_filaments));

    NativeCommand uninstall_filaments = reaching(
        {"uninstall_filaments"},
        mutate_tool("filament_uninstall", L("Uninstall system filaments"),
                    "Uninstall system filaments, for good, as unticking them in the System Filaments dialog and pressing "
                    "Confirm does: the filament selectors no longer offer them. names as `filament_install` takes them. A "
                    "filament a slot of the project uses is refused, naming the slot. " +
                        kept + " Reply: uninstalled (the presets uninstalled), `not_installed`, `filaments_installed`.",
                    object_schema({{"names", names_prop}}, {"names"}), change_filaments(false)));
    uninstall_filaments.examples = json::parse(R"json([
        {"title": "Uninstall a filament", "calls": [
            {"tool": "filament_uninstall", "args": {"names": ["Bambu PETG HF"]}}]}])json");
    catalog.add(std::move(uninstall_filaments));
}

}}} // namespace Slic3r::GUI::Mcp
