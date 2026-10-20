#include "PrinterAgentHandoffJob.hpp"

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"
#include "slic3r/Utils/bambu_networking.hpp"

#include <exception>
#include <string>
#include <utility>

namespace Slic3r { namespace GUI {

PrinterAgentHandoffJob::PrinterAgentHandoffJob(Action action, PrintParams params)
    : m_action(action), m_params(std::move(params))
{}

void PrinterAgentHandoffJob::process(Ctl& ctl)
{
    const std::string msg = m_action == Action::print ? _u8L("Sending print job over LAN") : _u8L("Sending G-code file over LAN");
    ctl.update_status(0, msg);

    NetworkAgent* agent = wxGetApp().getAgent();
    if (!agent) {
        m_result = -1;
        return;
    }

    auto update_fn = [&ctl, &msg](int stage, int code, std::string info) {
        if (stage == SendingPrintJobStage::PrintingStageUpload && code >= 0 && code <= 100)
            ctl.update_status(code, info.empty() ? msg : msg + format("(%s)", info));
        else if (stage == SendingPrintJobStage::PrintingStageFinished)
            ctl.update_status(100, msg);
    };
    auto cancel_fn = [&ctl]() { return ctl.was_canceled(); };

    m_result = m_action == Action::print ? agent->start_local_print(m_params, update_fn, cancel_fn) :
                                           agent->start_send_gcode_to_sdcard(m_params, update_fn, cancel_fn, nullptr);
}

void PrinterAgentHandoffJob::finalize(bool canceled, std::exception_ptr& eptr)
{
    if (canceled || eptr || m_result >= 0 || m_result == BAMBU_NETWORK_ERR_CANCELED)
        return;
    show_error(wxGetApp().plater(), _L("Failed to send the print job. Please try again."));
}

}} // namespace Slic3r::GUI
