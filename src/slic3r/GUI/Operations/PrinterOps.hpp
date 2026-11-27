#pragma once

#include "Outcome.hpp"

#include "slic3r/GUI/MainFrame.hpp"

#include <optional>
#include <string>
#include <vector>

namespace Slic3r {
class MachineObject;
class PresetBundle;
class PrintHost;
}

// Printing and the printer: the dialogs the Print button opens, the print host's connection test, and the job and status
// of the printer the Device tab is connected to. Each does what the button, the connection dialog's Test button, or the
// Device tab's controls do for a person, for the printer selected as it stands.
namespace Slic3r { namespace GUI { namespace Ops {

// The dialog a print or send call opens: the Print dialog of a printer on the Bambu network or with printer agents
// (SelectMachineDialog), the dialog that sends to such a printer's storage (SendToPrinterDialog), or the print host's
// upload dialog (Send G-code to printer host).
enum class PrintDialog { Print, Storage, HostUpload };

struct PrintRoute
{
    PrintDialog dialog = PrintDialog::HostUpload;
    // The Print button's action the call stands for, whose rule (MainFrame::print_action_enabled()) must hold before
    // the dialog opens; none for a send to a print host, whose upload slices the plate itself.
    std::optional<MainFrame::PrintSelectType> action;
};

// The route of a print call (`send` false: print_start) or a send call (printer_send), for the current plate or with
// `all` every plate, from `offered`, the actions the Print button offers for the selected printer
// (MainFrame::available_print_actions()), so that a call is what the button's action of that name does. `has_print_host`:
// the printer's print_host address is not empty, whether or not the project has objects.
// Refuses: all_plates_not_offered (the button offers no such action for every plate), no_print_host (a route through a
// print host and no address).
Outcome<PrintRoute> print_route(bool send, bool all, const std::vector<MainFrame::PrintSelectType>& offered, bool has_print_host);

struct OpenPrintDialogArgs
{
    bool               send = false;
    std::optional<int> plate; // made the current plate first; none: the current plate
    bool               all = false;
};
// Opens the dialog of the call's route, as the Print button's action does: it applies the slicing result and asks the
// button's rule for the action, then runs the button's own handler. A refusal after the named plate was selected makes
// the plate that was current before current again. The dialog is modal, so this returns once it closes; what is printed
// or sent is what the person, or a client's dialog_answer, chooses there. UI thread.
// Refuses: what print_route() gives, no_such_plate, plate_not_selected, not_sliced (the button would be disabled for
// the action).
Outcome<Done> open_print_dialog(const OpenPrintDialogArgs& args);

// Why the print host `host`, at the address `address`, cannot be tested, or none: no_print_host (no host of that type,
// or an empty address), host_cannot_be_tested (its kind has no test, can_test() false).
std::optional<Refusal> host_test_refusal(const PrintHost* host, const std::string& address);

struct HostTested
{
    bool        ok = false;
    std::string message; // the Test button's message, in the app's language
};
// Asks the selected printer's print host, as its connection settings stand, whether it answers, as the Test button of
// the printer's connection dialog does. It opens no dialog: where the button goes on to a cloud host's login, this
// stops at the failed test, with the host's message. The UI waits while the host is asked. UI thread.
// Refuses: what host_test_refusal() gives.
Outcome<HostTested> test_print_host();

// The printer the Device tab is connected to (DeviceManager::get_selected_machine()), or null with none. UI thread.
MachineObject* connected_printer();

// The Device tab's job buttons.
enum class JobCommand { Pause, Resume, Stop };

// Why `command` is not sent to `printer`, or none: no_printer_connected (null), printer_state (its state, print_status,
// does not allow the command: can_pause(), can_resume() or can_abort()), resume_at_printer (a job that resumes only at
// the printer, in the sentence the Device tab's Resume shows for it).
std::optional<Refusal> job_command_refusal(MachineObject* printer, JobCommand command);

struct JobCommandSent
{
    std::string printer; // its name
    std::string state;   // the state it was in when the command was sent
};
// Sends `command` to `printer` as the Device tab's button does, without the tab's question before a stop. The printer's
// new state arrives when it reports. UI thread.
// Refuses: what job_command_refusal() gives, what job_command_failure() gives for a command not sent.
Outcome<JobCommandSent> send_job_command(MachineObject* printer, JobCommand command);

// What a non-zero return of MachineObject::publish_json() means: command_not_supported for a command the printer's agent
// has no translation for, or a capability the printer lacks (for which publish_json() has shown the app's notice, as for
// a person's click), else not_sent with the code.
Refusal job_command_failure(int published);

// The printer's model as the Device tab names it, untranslated, or empty where the tab would say "Unknown". The vendors'
// models are read only through a `bundle` that is not null.
std::string printer_model_name(const MachineObject& printer, const PresetBundle* bundle);

// A temperature the Device tab shows, and its target; none where the tab shows a blank.
struct Temperature
{
    std::optional<float> current, target;
};

// What the Device tab's status shows for the connected printer.
struct PrinterStatus
{
    std::string              name;
    std::string              model;        // printer_model_name(); empty: unknown
    std::string              printer_type; // the model's id, as the printer reports it
    bool                     online = false;
    std::string              state; // print_status: IDLE, PREPARE, RUNNING, PAUSE, FINISH, FAILED...
    int                      stage = 0; // mc_print_stage, a number: its name is translated
    int                      percent = 0;
    int                      remaining_seconds = 0;
    int                      layer = 0, total_layers = 0;
    std::string              file; // the job's name (subtask_name)
    int                      error_code = 0;
    std::vector<Temperature> nozzles; // one per nozzle
    Temperature              bed, chamber;
};
// What `printer` holds for the Device tab, read without publishing anything; none with no printer. The serial number is
// not in it. UI thread.
std::optional<PrinterStatus> printer_status(MachineObject* printer);

}}} // namespace Slic3r::GUI::Ops
