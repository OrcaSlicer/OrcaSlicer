#include "PythonScriptsDialog.hpp"

#include "McpOptions.hpp"
#include "WindowNames.hpp"

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

#include <atomic>

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
                               "the Run Script panel or with --run, and the ones AI agents send through the MCP server. "
                               "When this option is on, they bypass the permission checks, which is useful for fully "
                               "automated workflows."));
        m_about->Wrap(FromDIP(kTextWidth));
        sizer->Add(m_about, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

        auto* row  = new wxBoxSizer(wxHORIZONTAL);
        m_allow    = new ::CheckBox(this);
        m_allow->SetValue(python_scripts_setting(*wxGetApp().app_config));
        auto* text = new Label(this, _L("Run Python scripts unrestricted"));
        row->Add(m_allow, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        row->Add(text, 0, wxALIGN_CENTER_VERTICAL);
        sizer->Add(row, 0, wxALL, FromDIP(16));

        // The checkbox is the saved setting, which the command line's flag does not change.
        if (python_scripts_set_for_run()) {
            // TRN Python scripts window. --unrestricted-scripts is a command line option: keep it as it is.
            const wxString note = _L("OrcaSlicer was started with --unrestricted-scripts, so scripts run unrestricted until "
                                     "it closes, whatever this setting says.");
            m_run_note = new Label(this, Label::Body_13, note);
            m_run_note->Wrap(FromDIP(kTextWidth));
            sizer->Add(m_run_note, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
        }

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
        if (m_run_note != nullptr)
            m_run_note->Wrap(FromDIP(kTextWidth));
        GetSizer()->SetSizeHints(this);
        Refresh();
    }

private:
    Label*      m_about    = nullptr;
    Label*      m_run_note = nullptr;
    ::CheckBox* m_allow    = nullptr;
};

std::atomic<bool> g_scripts_unrestricted_for_run{false};

} // namespace

bool python_scripts_setting(const AppConfig& config) { return config.get_bool(kPythonUnrestrictedKey); }

bool python_scripts_unrestricted(const AppConfig& config) { return python_scripts_set_for_run() || python_scripts_setting(config); }

void set_python_scripts_for_run(bool unrestricted) { g_scripts_unrestricted_for_run = unrestricted; }

bool python_scripts_set_for_run() { return g_scripts_unrestricted_for_run; }

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
    if (allow == python_scripts_setting(config))
        return false;
    config.set_bool(kPythonUnrestrictedKey, allow);
    apply_python_scripts_setting(config);
    return true;
}

}}} // namespace Slic3r::GUI::Mcp
