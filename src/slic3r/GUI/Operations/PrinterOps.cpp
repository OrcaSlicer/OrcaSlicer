#include "PrinterOps.hpp"

#include "Outcome.hpp"
#include "PlateOps.hpp"

#include "slic3r/GUI/Event.hpp"
#include "slic3r/GUI/GLToolbar.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/Utils/PrintHost.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <algorithm>
#include <cassert>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <wx/string.h>
#include <wx/thread.h>
#include <wx/utils.h>

namespace Slic3r { namespace GUI { namespace Ops {

namespace {

Refusal no_print_host()
{
    return {RefusalKind::Precondition, "no_print_host",
            L("This printer has no print host. Set one in the printer's connection settings.")};
}

Refusal not_sliced(bool all)
{
    if (all)
        return {RefusalKind::Precondition, "not_sliced",
                L("Not every plate is sliced and ready to print. Slice all plates first.")};
    return {RefusalKind::Precondition, "not_sliced", L("The plate is not sliced and ready to print. Slice it first.")};
}

// The edited printer preset's print_host address, which the print host routes and the host test need.
std::string print_host_address(const DynamicPrintConfig& config)
{
    const auto* host = config.option<ConfigOptionString>("print_host");
    return host != nullptr ? host->value : std::string();
}

} // namespace

Outcome<PrintRoute> print_route(bool send, bool all, const std::vector<MainFrame::PrintSelectType>& offered, bool has_print_host)
{
    const auto has = [&](MainFrame::PrintSelectType action) { return std::find(offered.begin(), offered.end(), action) != offered.end(); };
    // A printer on the Bambu network or with printer agents is offered Send; one through a print host is not.
    const bool network = has(MainFrame::eSendToPrinter);
    PrintRoute route;
    if (send) {
        if (all) {
            if (!has(MainFrame::eSendToPrinterAll))
                return Refusal{RefusalKind::Precondition, "all_plates_not_offered",
                               L("For this printer the Print button offers no action on all plates. Name one plate.")};
            route = {PrintDialog::Storage, MainFrame::eSendToPrinterAll};
        } else if (network)
            route = {PrintDialog::Storage, MainFrame::eSendToPrinter};
        else
            route = {PrintDialog::HostUpload, std::nullopt};
    } else if (all) {
        if (!has(MainFrame::ePrintAll))
            return Refusal{RefusalKind::Precondition, "all_plates_not_offered",
                           L("For this printer the Print button offers no action on all plates. Name one plate.")};
        route = {network ? PrintDialog::Print : PrintDialog::HostUpload, MainFrame::ePrintAll};
    } else if (has(MainFrame::ePrintPlate))
        route = {network ? PrintDialog::Print : PrintDialog::HostUpload, MainFrame::ePrintPlate};
    else
        // Another vendor's printer: the button's Print is the host's upload.
        route = {PrintDialog::HostUpload, MainFrame::eSendGcode};
    if (route.dialog == PrintDialog::HostUpload && !has_print_host)
        return no_print_host();
    return route;
}

Outcome<Done> open_print_dialog(const OpenPrintDialogArgs& args)
{
    assert(wxThread::IsMain());
    MainFrame&                  frame  = *wxGetApp().mainframe;
    Plater&                     plater = *wxGetApp().plater();
    const std::string           host   = print_host_address(wxGetApp().preset_bundle->printers.get_edited_preset().config);
    const Outcome<PrintRoute>   route  = print_route(args.send, args.all, frame.available_print_actions(), !host.empty());
    if (!route)
        return route.refusal();

    const int previous = plater.get_partplate_list().get_curr_plate_index();
    if (args.plate)
        if (const Outcome<Done> selected = select_plate({*args.plate}); !selected)
            return selected.refusal();
    const std::optional<MainFrame::PrintSelectType> action = route.value().action;
    if (action) {
        // What the button's click does before it opens anything: the slicing result applied, then its rule asked.
        plater.apply_background_progress();
        if (!frame.print_action_enabled(*action)) {
            if (args.plate && *args.plate != previous)
                plater.select_plate(previous);
            return not_sliced(args.all);
        }
    }

    // The button's own call. Print plate and Print all go through the plater's handler of the button's event, which
    // opens the Print dialog or the host's upload as the printer has it.
    if (action == MainFrame::ePrintPlate || action == MainFrame::ePrintAll) {
        SimpleEvent event(action == MainFrame::ePrintAll ? EVT_GLTOOLBAR_PRINT_ALL : EVT_GLTOOLBAR_PRINT_PLATE);
        plater.GetEventHandler()->ProcessEvent(event);
    } else if (route.value().dialog == PrintDialog::Storage)
        plater.send_to_printer(args.all);
    else
        plater.send_gcode_legacy();
    return Done{};
}

std::optional<Refusal> host_test_refusal(const PrintHost* host, const std::string& address)
{
    // The connection dialog fills in a cloud host's own address, so an empty one is no print host for any kind.
    if (host == nullptr || address.empty())
        return no_print_host();
    if (!host->can_test())
        return Refusal{RefusalKind::Precondition, "host_cannot_be_tested",
                       L("This printer's kind of print host has no connection test.")};
    return std::nullopt;
}

Outcome<HostTested> test_print_host()
{
    assert(wxThread::IsMain());
    // A copy, as the connection dialog tests its own: nothing the host reads or keeps reaches the preset.
    DynamicPrintConfig               config = wxGetApp().preset_bundle->printers.get_edited_preset().config;
    const std::unique_ptr<PrintHost> host(PrintHost::get_print_host(&config));
    if (std::optional<Refusal> refusal = host_test_refusal(host.get(), print_host_address(config)))
        return *refusal;
    wxString message;
    bool     ok = false;
    {
        // The test blocks the UI, as the Test button's does.
        wxBusyCursor wait;
        ok = host->test(message);
    }
    return HostTested{ok, into_u8(ok ? host->get_test_ok_msg() : host->get_test_failed_msg(message))};
}

}}} // namespace Slic3r::GUI::Ops
