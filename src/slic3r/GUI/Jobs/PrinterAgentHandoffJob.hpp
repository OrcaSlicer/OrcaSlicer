#pragma once

#include "Job.hpp"
#include "slic3r/Utils/bambu_networking.hpp"

#include <exception>

namespace Slic3r { namespace GUI {

// Hands a sliced plate to a printer agent that shows its own print and send UI, so none of the
// storage, LAN or device-tab handling of PrintJob and SendJob applies.
class PrinterAgentHandoffJob : public Job
{
public:
    enum class Action { print, send };

    PrinterAgentHandoffJob(Action action, PrintParams params);

    void process(Ctl& ctl) override;
    void finalize(bool canceled, std::exception_ptr& eptr) override;

private:
    Action      m_action;
    PrintParams m_params;
    int         m_result = 0;
};

}} // namespace Slic3r::GUI
