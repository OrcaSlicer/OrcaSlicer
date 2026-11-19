// The printing commands: print_start and printer_send open the dialog the Print button's Print and Send open for the
// selected printer, and printer_test_host asks the printer's print host whether it answers, as the Test button of its
// connection dialog does. What is printed or sent is left to the dialog, which a client answers with the dialog tools.

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

json plates_prop(const char* all)
{
    return {{"type", {"string", "integer"}},
            {"description", std::string("\"current\" (default), a plate index, which is selected first, or \"all\" for ") + all +
                                ", where the Print button offers it."}};
}

} // namespace

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
        "result, and it is refused (not_sliced) where the button would be disabled, as for a plate not sliced: slice with "
        "plate_slice first. The call replies blocked_by_dialog while the dialog is open: read and set its fields with "
        "widget_list and widget_set_value, and press its buttons with dialog_answer. Nothing prints until its button is "
        "pressed. Refused: all_plates_not_offered for plates \"all\" where the button offers no Print all, no_print_host for "
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
        "button that sends to it, and is refused (not_sliced) for a plate not sliced. For a printer with a print host "
        "(OctoPrint, Moonraker, PrusaLink, Duet...) it is the Send G-code to printer host dialog: set the file name with "
        "widget_set_value and press Upload or Upload and Print with dialog_answer; the plate is sliced if needed, then "
        "uploaded in the background: app_state.uploads shows the row's state, and a failed upload also opens an error "
        "dialog. The call replies blocked_by_dialog while the dialog is open. A replay sends again. Refused: "
        "all_plates_not_offered for plates \"all\" where the button offers no Send all, no_print_host for a printer with no "
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
        "Refused: no_print_host for a printer with no print host address, host_cannot_be_tested for a kind of host that has "
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
}

}}} // namespace Slic3r::GUI::Mcp
