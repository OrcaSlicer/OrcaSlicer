// The printing commands: print_start and printer_send open the dialog the Print button's Print and Send open for the
// selected printer, and printer_test_host asks the printer's print host whether it answers, as the Test button of its
// connection dialog does. What is printed or sent is left to the dialog, which a client answers with the dialog tools.
// printer_status reads what the Device tab shows of the printer it is connected to, and print_pause, print_resume and
// print_stop send that printer the tab's job commands.

#include "McpTools.hpp"

#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/CommandTypes.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/NativeCommandMenuItems.hpp"
#include "slic3r/GUI/NativeCommands.hpp"
#include "slic3r/GUI/Operations/Outcome.hpp"
#include "slic3r/GUI/Operations/PrinterOps.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <utility>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;

namespace {

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
