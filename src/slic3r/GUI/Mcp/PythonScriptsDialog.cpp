#include "PythonScriptsDialog.hpp"

#include "McpOptions.hpp"
#include "McpTypes.hpp"

#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Widgets/CheckBox.hpp"
#include "slic3r/GUI/Widgets/DialogButtons.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include "slic3r/plugin/PluginAuditManager.hpp"

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
                            _L("Python scripts are subject to the same permission checks as plugins: the scripts you run in "
                               "the Run Script panel or with --mcp-run, and the ones AI agents send through the MCP server. "
                               "When this option is on, they bypass the permission checks, which is useful for fully "
                               "automated workflows."));
        m_about->Wrap(FromDIP(kTextWidth));
        sizer->Add(m_about, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

        auto* row  = new wxBoxSizer(wxHORIZONTAL);
        m_allow    = new ::CheckBox(this);
        m_allow->SetValue(python_scripts_unrestricted(*wxGetApp().app_config));
        auto* text = new Label(this, _L("Run Python scripts unrestricted"));
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
        GetSizer()->SetSizeHints(this);
        Refresh();
    }

private:
    Label*      m_about = nullptr;
    ::CheckBox* m_allow = nullptr;
};

} // namespace

bool python_scripts_unrestricted(const AppConfig& config) { return config.get_bool(kPythonUnrestrictedKey); }

void apply_python_scripts_setting(const AppConfig& config)
{
    PluginAuditManager::instance().set_scripts_unrestricted(python_scripts_unrestricted(config));
}

bool show_python_scripts_dialog(wxWindow* parent)
{
    PythonScriptsDialog dialog(parent);
    if (dialog.ShowModal() != wxID_OK)
        return false;
    AppConfig&  config = *wxGetApp().app_config;
    const bool  allow  = dialog.allow();
    if (allow == python_scripts_unrestricted(config))
        return false;
    config.set_bool(kPythonUnrestrictedKey, allow);
    apply_python_scripts_setting(config);
    return true;
}

}}} // namespace Slic3r::GUI::Mcp
