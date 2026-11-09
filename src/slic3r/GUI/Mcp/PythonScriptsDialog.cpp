#include "PythonScriptsDialog.hpp"

#include "McpOptions.hpp"
#include "McpTypes.hpp"

#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/RunScriptPanel.hpp"
#include "slic3r/GUI/Widgets/CheckBox.hpp"
#include "slic3r/GUI/Widgets/DialogButtons.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include "slic3r/GUI/Widgets/StateColor.hpp"

#include <wx/colour.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/toplevel.h>
#include <wx/window.h>

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {

constexpr int kTextWidth = 420;

class PythonScriptsDialog : public DPIDialog
{
public:
    explicit PythonScriptsDialog(wxWindow* parent)
        : DPIDialog(parent ? parent : static_cast<wxWindow*>(wxGetApp().mainframe), wxID_ANY, _L("Python scripts"), wxDefaultPosition,
                    wxDefaultSize, wxCAPTION | wxCLOSE_BOX, kHiddenFromAutomation)
    {
        SetBackgroundColour(*wxWHITE);
        SetFont(Label::Body_14);

        auto* sizer = new wxBoxSizer(wxVERTICAL);

        m_about = new Label(this, Label::Body_13,
                            _L("When this is on, you can run Python scripts (.py files) in the Run Script panel and with "
                               "--mcp-run, and AI agents connected to the MCP server can send Python code to run inside "
                               "OrcaSlicer."));
        m_about->Wrap(FromDIP(kTextWidth));
        sizer->Add(m_about, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

        m_warning = new Label(this, Label::Body_13,
                              _L("A Python script runs with OrcaSlicer's full access to your computer: it can read and change "
                                 "your files and reach the network. Turning this on for your own scripts also lets connected "
                                 "agents send scripts, so turn it on only while you trust them."));
        m_warning->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#B05A00")));
        m_warning->Wrap(FromDIP(kTextWidth));
        sizer->Add(m_warning, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

        auto* row  = new wxBoxSizer(wxHORIZONTAL);
        m_allow    = new ::CheckBox(this);
        m_allow->SetValue(python_scripts_allowed(*wxGetApp().app_config));
        auto* text = new Label(this, _L("Allow Python scripts"));
        row->Add(m_allow, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        row->Add(text, 0, wxALIGN_CENTER_VERTICAL);
        sizer->Add(row, 0, wxALL, FromDIP(16));

        auto* buttons = new DialogButtons(this, {"OK", "Cancel"});
        buttons->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_OK); });
        buttons->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
        sizer->Add(buttons, 0, wxEXPAND);

        SetSizerAndFit(sizer);
        CenterOnParent();
        wxGetApp().UpdateDlgDarkUI(this);
    }

    bool allow() const { return m_allow->GetValue(); }

protected:
    void on_dpi_changed(const wxRect&) override
    {
        m_allow->Rescale();
        m_about->Wrap(FromDIP(kTextWidth));
        m_warning->Wrap(FromDIP(kTextWidth));
        GetSizer()->SetSizeHints(this);
        Refresh();
    }

private:
    Label*      m_about   = nullptr;
    Label*      m_warning = nullptr;
    ::CheckBox* m_allow   = nullptr;
};

} // namespace

bool python_scripts_allowed(const AppConfig& config) { return config.get_bool(kPythonScriptsKey); }

bool show_python_scripts_dialog(wxWindow* parent)
{
    PythonScriptsDialog dialog(parent);
    if (dialog.ShowModal() != wxID_OK)
        return false;
    AppConfig&  config = *wxGetApp().app_config;
    const bool  allow  = dialog.allow();
    if (allow == python_scripts_allowed(config))
        return false;
    config.set_bool(kPythonScriptsKey, allow);
    if (RunScriptPanel* panel = wxGetApp().run_script_panel_if_built())
        panel->python_setting_changed();
    return true;
}

}}} // namespace Slic3r::GUI::Mcp
