// The place Printers and prints. printer_list_models, printer_install and printer_uninstall: what the Printer
// Selection dialog lists and its Confirm installs, on the operations of GUI/Operations/InstallOps.hpp. The printing
// commands: print_start and printer_send open the dialog the Print button's Print and Send open for the selected
// printer, and printer_test_host asks the printer's print host whether it answers, as the Test button of its
// connection dialog does. What is printed or sent is left to the dialog, which a client answers with the dialog
// tools. printer_status reads what the Device tab shows of the printer it is connected to, and print_pause,
// print_resume and print_stop send that printer the tab's job commands.

#include "slic3r/GUI/Mcp/McpTools.hpp"
#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/CommandTypes.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/NativeCommandMenuItems.hpp"
#include "slic3r/GUI/NativeCommands.hpp"
#include "slic3r/GUI/Operations/InstallOps.hpp"
#include "slic3r/GUI/Operations/Outcome.hpp"
#include "slic3r/GUI/Operations/PrinterOps.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <optional>
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

json temperature(const Ops::Temperature& t)
{
    return {{"current", t.current ? json(*t.current) : json()}, {"target", t.target ? json(*t.target) : json()}};
}

// print_pause, print_resume or print_stop: `command` sent to the connected printer.
NativeCommand job_command(const char* name, const char* title, Ops::JobCommand command, const std::string& description,
                          const char* examples, bool destructive)
{
    NativeCommand job = mutate_tool(
        name, title,
        description + " The printer's new state arrives when it reports: read it with `printer_status`. Reply: printer (its "
                      "name), state (the state it was in) and sent. A replay sends the command again.",
        object_schema({}),
        [command](const json&, ToolContext& ctx) {
            json reply = ctx.run_ui([command] {
                const Ops::Outcome<Ops::JobCommandSent> sent = Ops::send_job_command(Ops::connected_printer(), command);
                if (!sent)
                    throw refused(sent.refusal());
                return json{{"printer", sent.value().printer}, {"state", sent.value().state}, {"sent", true}};
            });
            return ToolResult::ok(std::move(reply));
        },
        destructive);
    // It sends the printer a command.
    job.open_world = true;
    job.examples   = json::parse(examples);
    return reaching({"send_job_command"}, std::move(job));
}

json plates_prop(const char* all)
{
    return {{"type", {"string", "integer"}},
            {"description", std::string("\"current\" (default), a plate index, which is selected first, or \"all\" for ") + all +
                                ", where the Print button offers it."}};
}

} // namespace

json printer_status_reply(const std::optional<Ops::PrinterStatus>& status)
{
    if (!status)
        return {{"connected", false}};
    json nozzles = json::array();
    for (const Ops::Temperature& nozzle : status->nozzles)
        nozzles.push_back(temperature(nozzle));
    return {{"connected", true},
            {"name", status->name},
            {"model", status->model.empty() ? json() : json(status->model)},
            {"printer_type", status->printer_type},
            {"online", status->online},
            {"state", status->state},
            {"stage", status->stage},
            {"percent", status->percent},
            {"remaining_seconds", status->remaining_seconds},
            {"layer", status->layer},
            {"total_layers", status->total_layers},
            {"file", status->file},
            {"error_code", status->error_code},
            {"temperatures",
             {{"nozzles", std::move(nozzles)}, {"bed", temperature(status->bed)}, {"chamber", temperature(status->chamber)}}}};
}

std::string read_print_plates(const json& args, std::optional<int>& plate, bool& all)
{
    plate.reset();
    all = false;
    if (!args.contains("plates"))
        return {};
    const json& plates = args.at("plates");
    if (plates.is_number_integer()) {
        plate = plates.get<int>();
        return {};
    }
    if (plates.is_string() && plates.get<std::string>() == "all")
        all = true;
    else if (!plates.is_string() || plates.get<std::string>() != "current")
        return "plates is \"current\", \"all\" or a plate index";
    return {};
}

void register_printer_tools(CommandCatalog& catalog)
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

    NativeCommand print = mutate_tool(
        "print_start", L("Print plate"),
        "Open the dialog the Print button's Print plate opens for the selected printer, for the current plate or the plate "
        "plates names, or with plates \"all\" Print all's. For a printer on the Bambu network or with printer agents it is the "
        "Print dialog, which leaves the printer, bed levelling, flow calibration, timelapse, the AMS mapping and the button "
        "that prints to it; for a printer with a print host (OctoPrint, Moonraker, PrusaLink, Duet...) it is the host's Send "
        "G-code to printer host dialog, with Upload and Upload and Print. As the button does, it first applies the slicing "
        "result, and it is refused (`not_sliced`) where the button would be disabled, as for a plate not sliced: slice with "
        "`plate_slice` first. The call replies `blocked_by_dialog` while the dialog is open: read and set its fields with "
        "`widget_list` and `widget_set_value`, and press its buttons with `dialog_answer`. Nothing prints until its button is "
        "pressed. Refused: `all_plates_not_offered` for plates \"all\" where the button offers no Print all, `no_print_host` for "
        "a printer with no print host address.",
        object_schema({{"plates", plates_prop("Print all")}}), [](const json& args, ToolContext& ctx) {
            Ops::OpenPrintDialogArgs op;
            if (const std::string err = read_print_plates(args, op.plate, op.all); !err.empty())
                throw ToolFailure(err);
            ctx.run_ui([&] {
                if (const Ops::Outcome<Ops::Done> opened = Ops::open_print_dialog(op); !opened)
                    throw refused(opened.refusal());
                return json();
            });
            settle(ctx);
            return ToolResult::ok();
        });
    // Its dialog prints on a printer.
    print.open_world = true;
    print.examples   = json::parse(R"json([
        {"title": "Open the Print dialog for the current plate", "calls": [{"tool": "print_start", "args": {}}]},
        {"title": "Print the second plate after slicing it", "calls": [
            {"tool": "plate_slice", "args": {"plates": 1}}, {"tool": "print_start", "args": {"plates": 1}}]}])json");
    catalog.add(reaching({"open_print_dialog"}, std::move(print)));

    const std::string send =
        "Open the dialog the Print button's Send opens for the selected printer, for the current plate or the plate plates "
        "names. For a printer on the Bambu network or with printer agents it is the dialog that sends the sliced plate to the "
        "printer's storage (with plates \"all\", Send all's), which leaves the printer, the file name, the storage and the "
        "button that sends to it, and is refused (`not_sliced`) for a plate not sliced. For a printer with a print host "
        "(OctoPrint, Moonraker, PrusaLink, Duet...) it is the Send G-code to printer host dialog: set the file name with "
        "`widget_set_value` and press Upload or Upload and Print with `dialog_answer`; the plate is sliced if needed, then "
        "uploaded in the background: `app_state.uploads` shows the row's state, and a failed upload also opens an error "
        "dialog. The call replies `blocked_by_dialog` while the dialog is open. A replay sends again. Refused: "
        "`all_plates_not_offered` for plates \"all\" where the button offers no Send all, `no_print_host` for a printer with no "
        "print host address.";
    const json    send_action  = NativeCommands::action("send_gcode", L("Send G-code to printer host"), L("Slice & Export"), "printer",
                                                        json::object(), send);
    NativeCommand printer_send = reaching({"open_print_dialog"},
                                          menu_command("printer_send", L("Send G-code to printer host"), send,
                                                       object_schema({{"plates", plates_prop("Send all")}}), json::array({send_action}),
                                                       [](const json& args) { return MenuItems::send_gcode(args); }));
    // It uploads to the printer's print host, or sends to the printer.
    printer_send.open_world = true;
    catalog.add(std::move(printer_send));

    NativeCommand test_host = mutate_tool(
        "printer_test_host", L("Test the print host"),
        "Ask the selected printer's print host whether it answers, as the Test button of the printer's connection dialog "
        "does, with its connection settings as they stand. Reply: ok, whether the host answered, and message, the button's "
        "message in the app's language; branch on ok. A cloud host that wants a login is a failed test with the host's "
        "message: logging in stays in the printer's connection dialog. The window does not redraw while the host is asked. "
        "Refused: `no_print_host` for a printer with no print host address, `host_cannot_be_tested` for a kind of host that has "
        "no test.",
        object_schema({}), [](const json&, ToolContext& ctx) {
            json reply = ctx.run_ui([] {
                const Ops::Outcome<Ops::HostTested> tested = Ops::test_print_host();
                if (!tested)
                    throw refused(tested.refusal());
                return json{{"ok", tested.value().ok}, {"message", tested.value().message}};
            });
            return ToolResult::ok(std::move(reply));
        });
    // It asks the host over the network.
    test_host.open_world = true;
    test_host.examples   = json::parse(R"json([
        {"title": "Check the print host after setting its address", "calls": [
            {"tool": "settings_set", "args": {"type": "printer", "values": {"host_type": "octoprint", "print_host": "192.168.1.20"}}},
            {"tool": "printer_test_host", "args": {}}]}])json");
    catalog.add(reaching({"test_print_host"}, std::move(test_host)));

    NativeCommand status = inspect_tool(
        "printer_status", L("Printer status"),
        "Read what the Device tab shows of the printer it is connected to, without sending it anything. Reply: connected; "
        "with a printer, name, model (null where the tab would say Unknown), `printer_type` (the model's id), online, state "
        "(IDLE, PREPARE, RUNNING, PAUSE, FINISH, FAILED...), stage (a number), percent, `remaining_seconds`, layer, "
        "`total_layers`, file (the job's name), `error_code`, and temperatures: nozzles (one `{current, target}` each), bed and "
        "chamber, null where the tab shows none. With no printer connected the reply is `{\"connected\": false}`, which is no "
        "error. The values are what the printer last reported.",
        object_schema({}), [](const json&, ToolContext& ctx) {
            return ToolResult::ok(ctx.run_ui([] { return printer_status_reply(Ops::printer_status(Ops::connected_printer())); }));
        });
    status.examples = json::parse(R"json([
        {"title": "Read the connected printer's state and temperatures", "calls": [{"tool": "printer_status", "args": {}}]}])json");
    catalog.add(std::move(status));

    catalog.add(job_command(
        "print_pause", L("Pause the print"), Ops::JobCommand::Pause,
        "Pause the print running on the printer the Device tab is connected to, as the tab's Pause button does. Refused: "
        "`no_printer_connected` with no printer connected, `printer_state`, naming the state, unless it is RUNNING.",
        R"json([{"title": "Pause the print once printer_status reads RUNNING", "calls": [
            {"tool": "printer_status", "args": {}}, {"tool": "print_pause", "args": {}}]}])json",
        false));
    catalog.add(job_command(
        "print_resume", L("Resume the print"), Ops::JobCommand::Resume,
        "Resume the paused print on the printer the Device tab is connected to, as the tab's Resume button does. Refused: "
        "`no_printer_connected` with no printer connected, `printer_state`, naming the state, unless it is PAUSE, and "
        "`resume_at_printer`, in the app's own sentence, for a job (such as laser processing) that resumes only at the printer.",
        R"json([{"title": "Resume the print once printer_status reads PAUSE", "calls": [
            {"tool": "printer_status", "args": {}}, {"tool": "print_resume", "args": {}}]}])json",
        false));
    catalog.add(job_command(
        "print_stop", L("Stop the print"), Ops::JobCommand::Stop,
        "Stop the print on the printer the Device tab is connected to, as the tab's Stop button does, but without its "
        "question \"Are you sure you want to stop this print?\": it asks nothing, and a stopped print cannot be resumed. "
        "Refused: `no_printer_connected` with no printer connected, `printer_state`, naming the state, unless it is RUNNING, "
        "PAUSE, PREPARE or SLICING.",
        R"json([{"title": "Stop the print", "calls": [{"tool": "print_stop", "args": {}}]}])json",
        true));
}

}}} // namespace Slic3r::GUI::Mcp
