#ifndef slic3r_TerminalDialog_hpp_
#define slic3r_TerminalDialog_hpp_

#include "Widgets/WebViewHostDialog.hpp"

#include <memory>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI {

class ProcessRunner;

class TerminalDialog : public Slic3r::GUI::WebViewHostDialog
{
public:
    TerminalDialog(wxWindow* parent,
                   wxWindowID id         = wxID_ANY,
                   const wxString& title = wxT(""),
                   const wxPoint& pos    = wxDefaultPosition,
                   const wxSize& size    = wxDefaultSize,
                   long style            = wxSYSTEM_MENU | wxCAPTION | wxCLOSE_BOX | wxMAXIMIZE_BOX);

    ~TerminalDialog() override;

private:
    void on_script_message(const nlohmann::json& payload) override;

    void resolve_and_run(const std::string& cmd);

    std::unique_ptr<ProcessRunner> m_runner;
};

}} // namespace Slic3r::GUI

#endif
