#include "PrinterOps.hpp"

#include "Outcome.hpp"
#include "PlateOps.hpp"

#include "slic3r/GUI/DeviceCore/DevBed.h"
#include "slic3r/GUI/DeviceCore/DevConfig.h"
#include "slic3r/GUI/DeviceCore/DevConfigUtil.h"
#include "slic3r/GUI/DeviceCore/DevDefs.h"
#include "slic3r/GUI/DeviceCore/DevExtruderSystem.h"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/Event.hpp"
#include "slic3r/GUI/GLToolbar.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/Utils/IPrinterAgent.hpp"
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

#include <wx/app.h>
#include <wx/busycursor.h>
#include <wx/string.h>
#include <wx/thread.h>

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

MachineObject* connected_printer()
{
    assert(wxThread::IsMain());
    DeviceManager* devices = wxGetApp().getDeviceManager();
    return devices != nullptr ? devices->get_selected_machine() : nullptr;
}

std::optional<Refusal> job_command_refusal(MachineObject* printer, JobCommand command)
{
    if (printer == nullptr)
        return Refusal{RefusalKind::Precondition, "no_printer_connected",
                       L("No printer is connected. Connect one on the Device tab.")};
    // The state the Device tab's buttons are enabled in, each with its own sentence.
    switch (command) {
    case JobCommand::Pause:
        if (!printer->can_pause())
            return Refusal{RefusalKind::Precondition, "printer_state", L("The printer's state is %1%, in which its print cannot be paused."),
                           {printer->print_status}};
        break;
    case JobCommand::Resume:
        if (!printer->can_resume())
            return Refusal{RefusalKind::Precondition, "printer_state", L("The printer's state is %1%, in which no print is paused to resume."),
                           {printer->print_status}};
        if (printer->resume_only_at_printer())
            return Refusal{RefusalKind::Precondition, "resume_at_printer",
                           L("To ensure your safety, certain processing tasks (such as laser) can only be resumed on printer.")};
        break;
    case JobCommand::Stop:
        if (!printer->can_abort())
            return Refusal{RefusalKind::Precondition, "printer_state", L("The printer's state is %1%, in which no print runs to stop."),
                           {printer->print_status}};
        break;
    }
    return std::nullopt;
}

Refusal job_command_failure(int published)
{
    if (published == ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED || published == ORCA_NETWORK_ERR_CAP_NOT_AVAILABLE)
        return Refusal{RefusalKind::Failed, "command_not_supported", L("The printer does not support this command.")};
    return Refusal{RefusalKind::Failed, "not_sent", L("The command was not sent to the printer (error %1%)."), {std::to_string(published)}};
}

Outcome<JobCommandSent> send_job_command(MachineObject* printer, JobCommand command)
{
    if (std::optional<Refusal> refusal = job_command_refusal(printer, command))
        return *refusal;
    JobCommandSent sent{printer->get_dev_name(), printer->print_status};
    const int published = command == JobCommand::Pause  ? printer->command_task_pause() :
                          command == JobCommand::Resume ? printer->command_task_resume() :
                                                          printer->command_task_abort();
    if (published != 0)
        return job_command_failure(published);
    return sent;
}

std::string printer_model_name(const MachineObject& printer, const PresetBundle* bundle)
{
    // As MachineObject::get_printer_type_display_str() names it, without its translated "Unknown".
    std::string name = DevPrinterConfigUtil::get_printer_display_name(printer.printer_type);
    if (name.empty() && bundle != nullptr)
        for (const auto& [vendor_id, vendor] : bundle->vendors)
            for (const auto& model : vendor.models)
                if (model.model_id == printer.printer_type)
                    name = model.name;
    if (name.empty() && printer.printer_type == "orcasonar")
        name = "OrcaSonar Printer";
    return name;
}

std::optional<PrinterStatus> printer_status(MachineObject* printer)
{
    if (printer == nullptr)
        return std::nullopt;
    PrinterStatus status;
    status.name = printer->get_dev_name();
    // The app's preset bundle, which a unit test, with no app of OrcaSlicer's, has none of.
    const GUI_App* app       = dynamic_cast<const GUI_App*>(wxTheApp);
    status.model             = printer_model_name(*printer, app != nullptr ? app->preset_bundle : nullptr);
    status.printer_type      = printer->printer_type;
    status.online            = printer->is_online();
    status.state             = printer->print_status;
    status.stage             = printer->mc_print_stage;
    status.percent           = printer->mc_print_percent;
    status.remaining_seconds = printer->mc_left_time;
    status.layer             = printer->curr_layer;
    status.total_layers      = printer->total_layers;
    status.file              = printer->subtask_name;
    status.error_code        = printer->print_error;
    // The Device tab's temperature fields: a nozzle per extruder, the bed, and the chamber's current temperature on the
    // X1 series and its target where the printer lets it be set, blank otherwise.
    if (const DevExtderSystem* extruders = printer->GetExtderSystem())
        for (int id = 0; id < extruders->GetTotalExtderCount(); ++id)
            status.nozzles.push_back({float(extruders->GetNozzleTempCurrent(id)), float(extruders->GetNozzleTempTarget(id))});
    if (DevBed* bed = printer->GetBed())
        status.bed = {bed->GetBedTemp(), bed->GetBedTempTarget()};
    if (printer->get_printer_series() == PrinterSeries::SERIES_X1)
        status.chamber.current = printer->chamber_temp;
    if (const DevConfig* config = printer->GetConfig(); config != nullptr && config->SupportChamberEdit())
        status.chamber.target = printer->chamber_temp_target;
    return status;
}

}}} // namespace Slic3r::GUI::Ops
