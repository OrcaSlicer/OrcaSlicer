// printer_list_models, printer_install and printer_uninstall: what the Printer Selection dialog lists and its Confirm
// installs; filament_install and filament_uninstall: what the System Filaments dialog's Confirm installs. On the
// operations of GUI/Operations/InstallOps.hpp.

#include "McpTools.hpp"
#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/CommandTypes.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/NativeCommands.hpp"
#include "slic3r/GUI/Operations/InstallOps.hpp"
#include "slic3r/GUI/Operations/Outcome.hpp"
#include "slic3r/GUI/Operations/PresetOps.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;

namespace {

// Runs `load` on a thread of its own with a flag that turns true once the call is cancelled (job_cancel, a stop of
// the replay or script it runs in, the app closing), which the load of the vendors reads between vendors.
template<class Result> Result cancellable(ToolContext& ctx, const std::function<Result(const std::atomic<bool>*)>& load)
{
    std::atomic<bool>   cancel{false};
    std::future<Result> result = std::async(std::launch::async, [&load, &cancel] { return load(&cancel); });
    while (result.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready)
        if (ctx.cancelled())
            cancel = true;
    return result.get();
}

// What the install commands reply when an operation refuses: the names a refusal carries under the key that says
// what they are, and the preset tools' hint where the presets have unsaved changes.
ToolFailure install_refused(const Ops::Refusal& refusal)
{
    const std::string code = refusal.code;
    if (code == "no_such_vendor" || code == "no_such_model")
        return refused(refusal, "printer_list_models lists them.", "similar");
    if (code == "ambiguous_model" || code == "vendors_not_loaded")
        return refused(refusal, "", "vendors");
    if (code == "no_such_nozzle")
        return refused(refusal, "", "nozzles");
    return preset_refused(refusal);
}

// Every printer model the app ships or has installed, which a call's names are checked against. On the call's thread,
// as the first read after a profile update loads every vendor.
std::vector<Ops::PrinterModel> known_models(ToolContext& ctx)
{
    using Known = Ops::Outcome<std::vector<Ops::PrinterModel>>;
    const Known known = cancellable<Known>(ctx, [](const std::atomic<bool>* cancel) {
        Ops::PrinterModelsArgs all;
        all.cancel = cancel;
        return Ops::printer_models(all);
    });
    if (!known)
        throw install_refused(known.refusal());
    return known.value();
}

json models_json(const std::vector<Ops::ModelNozzles>& models)
{
    json out = json::array();
    for (const Ops::ModelNozzles& model : models)
        out.push_back({{"vendor", model.vendor}, {"model", model.model}, {"nozzles", model.nozzles}});
    return out;
}

// printer_install's and printer_uninstall's function: the models a call names, applied on the UI thread.
CommandFn change_printers(bool install)
{
    return [install](const json& args, ToolContext& ctx) {
        Ops::InstallPrintersArgs op;
        for (const json& item : args.at("models")) {
            Ops::ModelNozzles model{arg_string(item, "vendor"), arg_string(item, "model"), {}};
            if (const auto nozzles = item.find("nozzles"); nozzles != item.end())
                for (const json& nozzle : *nozzles)
                    model.nozzles.push_back(nozzle.get<std::string>());
            op.models.push_back(std::move(model));
        }
        op.changes = preset_changes_answer(args);
        op.known   = known_models(ctx);
        json reply = ctx.run_ui([&] {
            ui_plater();
            const json                                 before = unsaved_changes();
            const Ops::Outcome<Ops::InstalledPrinters> done   = install ? Ops::install_printers(op) : Ops::uninstall_printers(op);
            if (!done)
                throw install_refused(done.refusal());
            const Ops::InstalledPrinters& printers = done.value();
            json out{{"models", models_json(printers.models)}, {"selected", printers.selected}};
            out[install ? "added" : "removed"] = install ? printers.added : printers.removed;
            if (!(install ? printers.removed : printers.added).empty())
                out[install ? "removed" : "added"] = install ? printers.removed : printers.added;
            if (!printers.missing.empty())
                out[install ? "not_offered" : "still_offered"] = models_json(printers.missing);
            if (!printers.filaments_installed.empty())
                out["filaments_installed"] = printers.filaments_installed;
            if (!printers.filaments_uninstalled.empty())
                out["filaments_uninstalled"] = printers.filaments_uninstalled;
            out.update(unsaved_changes_outcome(before));
            return out;
        });
        settle(ctx);
        return ToolResult::ok(reply);
    };
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

void register_install_tools(CommandCatalog& catalog)
{
    const json models_prop = {
        {"type", "array"},
        {"minItems", 1},
        {"items", object_schema({{"model", prop("string", "The model, as `printer_list_models` lists it (\"Bambu Lab X1 Carbon\").")},
                                 {"vendor", prop("string", "Its vendor (\"BBL\"), needed where several vendors have a model of that "
                                                           "name.")},
                                 {"nozzles", {{"type", "array"},
                                              {"minItems", 1},
                                              {"items", {{"type", "string"}}},
                                              {"description", "Its nozzles, as `printer_list_models` lists them (\"0.4\"). Default: "
                                                              "all of them, as the dialog's tick of a model."}}}},
                                {"model"})},
        {"description", "The printer models, each with its nozzles."}};
    const json preset_changes_prop = enum_prop({"transfer", "discard"},
                                               "The presets' unsaved changes, which reloading the presets drops: move them to the "
                                               "presets selected after it, or drop them, as the Transfer and Discard of the question "
                                               "the dialog's Confirm asks. Needed while presets have unsaved changes; without it the "
                                               "call is refused.");
    const std::string reload = "Reloading the presets drops their unsaved changes, so while there are some `preset_changes` says what "
                               "becomes of them; without it the call is refused. A call that changes nothing reloads nothing. The "
                               "reload installs a printer's default filaments where no installed filament fits it, as the dialog "
                               "does, and where a vendor's profile lists default filaments of its own, which none of the shipped "
                               "profiles does, swaps a @System filament for the vendor's: those are under `filaments_installed` and "
                               "`filaments_uninstalled`. Two errors come after a change and say what it was: `vendors_not_installed`, "
                               "where a vendor's profiles cannot be installed (the installed printers are as they were, the "
                               "unsaved changes discarded if `preset_changes` said so, and the profiles copied before it stay in "
                               "the configuration folder), and `presets_not_reloaded` (the installed printers were written, the "
                               "presets not reloaded). Not in the undo history.";

    NativeCommand list = NativeCommands::long_command(
        "printer_list_models", L("List printer models"), {},
        object_schema({{"vendor", prop("string", "A vendor's models only (\"BBL\", \"Prusa\"), in any case.")},
                       {"query", prop("string", "Models whose name holds this, in any case.")},
                       {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 500}, {"description", "Default 100."}}},
                       {"offset", {{"type", "integer"}, {"minimum", 0}}}}),
        [](const json& args, ToolContext& ctx) {
            Ops::PrinterModelsArgs op;
            op.vendor = arg_string(args, "vendor");
            op.query  = arg_string(args, "query");
            ctx.run_ui([&op] {
                ui_plater();
                op.installed = Ops::installed_set();
                return json();
            });
            using Models        = Ops::Outcome<std::vector<Ops::PrinterModel>>;
            const Models models = cancellable<Models>(ctx, [&op](const std::atomic<bool>* cancel) {
                Ops::PrinterModelsArgs run = op;
                run.cancel                 = cancel;
                return Ops::printer_models(run);
            });
            if (!models)
                throw install_refused(models.refusal());
            json items = json::array();
            for (const Ops::PrinterModel& model : models.value())
                items.push_back({{"vendor", model.vendor},
                                 {"model", model.model},
                                 {"nozzles", model.nozzles},
                                 {"installed_nozzles", model.installed_nozzles}});
            return ToolResult::ok(page(items, args, 100));
        });
    list.description = "Every printer model OrcaSlicer ships or has installed, of every vendor, as the Printer Selection dialog "
                       "lists them: each with its vendor (the vendor's id, \"BBL\"), model, nozzles and `installed_nozzles`, the "
                       "nozzles whose printer presets the printer selector offers. vendor keeps one vendor's models, and query "
                       "those whose name holds it. The first call after the shipped profiles change reads every vendor, which "
                       "takes a while and can reply with a job to wait for; later calls read the result kept in the configuration "
                       "folder. `printer_install` and `printer_uninstall` change what is installed.";
    list.listed      = true;
    list.read_only   = true;
    list.idempotent  = true;
    list.guard       = GuardClass::Inspect;
    list.journal     = false;
    list.cancellable = true;
    list.operations  = {"printer_models"};
    list.examples    = json::parse(R"json([
        {"title": "Find a printer model and its nozzles", "calls": [
            {"tool": "printer_list_models", "args": {"query": "X1 Carbon"}}]},
        {"title": "List one vendor's printer models", "calls": [
            {"tool": "printer_list_models", "args": {"vendor": "Prusa"}}]}])json");
    catalog.add(std::move(list));

    NativeCommand install = reaching(
        {"install_printers"},
        mutate_tool("printer_install", L("Install printer models"),
                    "Install printer models, for good, as ticking them in the Printer Selection dialog and pressing Confirm "
                    "does: a vendor's profiles are installed from the ones OrcaSlicer ships, the presets reloaded, and a printer "
                    "newly added selected. The other installed models are kept, and the installed filaments but for what the "
                    "reload changes. A name no model has, a vendor or a nozzle a model lacks, is refused with the closest "
                    "names. " +
                        reload +
                        " Reply: models (what was named, resolved), added (the printer presets the printer selector offers "
                        "that it did not before the call), selected, `not_offered` (named nozzles whose presets it still does "
                        "not offer), and discarded/modified as `preset_select`'s.",
                    object_schema({{"models", models_prop}, {"preset_changes", preset_changes_prop}}, {"models"}),
                    change_printers(true)));
    install.examples = json::parse(R"json([
        {"title": "Install a printer model with one of its nozzles", "calls": [
            {"tool": "printer_list_models", "args": {"query": "Velo"}},
            {"tool": "printer_install", "args": {"models": [{"vendor": "Goofoo", "model": "Goofoo Velo+", "nozzles": ["0.4"]}]}}]},
        {"title": "Install every nozzle of a model, dropping the presets' unsaved changes", "calls": [
            {"tool": "printer_install", "args": {"models": [{"model": "Bambu Lab X1 Carbon"}], "preset_changes": "discard"}}]}])json");
    catalog.add(std::move(install));

    NativeCommand uninstall = reaching(
        {"uninstall_printers"},
        mutate_tool("printer_uninstall", L("Uninstall printer models"),
                    "Uninstall printer models, for good, as unticking them in the Printer Selection dialog and pressing "
                    "Confirm does: the printer selector no longer offers their printer presets, and the vendor's profiles stay "
                    "on disk. Without nozzles a model goes whole. A printer removed while selected is replaced by another "
                    "installed one. Removing the last installed printer model is refused, and so are names as `printer_install` "
                    "refuses them. " +
                        reload +
                        " Reply: models, removed (the printer presets the printer selector no longer offers), selected, "
                        "`still_offered` (named nozzles whose presets it still offers), and discarded/modified as `preset_select`'s.",
                    object_schema({{"models", models_prop}, {"preset_changes", preset_changes_prop}}, {"models"}),
                    change_printers(false)));
    uninstall.examples = json::parse(R"json([
        {"title": "Uninstall a printer model", "calls": [
            {"tool": "printer_uninstall", "args": {"models": [{"vendor": "Goofoo", "model": "Goofoo Velo+"}]}}]}])json");
    catalog.add(std::move(uninstall));

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
