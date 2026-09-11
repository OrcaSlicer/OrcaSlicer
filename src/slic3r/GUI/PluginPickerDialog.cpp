#include "PluginPickerDialog.hpp"

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/valgen.h>

#include "GUI.hpp"
#include "I18N.hpp"

#include "GUI_App.hpp"

#include "Widgets/DialogButtons.hpp"

namespace Slic3r { namespace GUI {

PluginPickerDialog::PluginPickerDialog(wxWindow* parent,
                                       const wxString& plugin_type_label,
                                       const std::vector<Slic3r::PluginDescriptor>& plugins)
    : DPIDialog(parent, wxID_ANY, wxString::Format(_L("Select %s Plugin"), plugin_type_label))
    , m_plugins(plugins)
{
    build_ui(plugin_type_label);
    CentreOnParent();
}

PluginPickerDialog::PluginPickerDialog(wxWindow* parent,
                                       const wxString& plugin_type_label,
                                       std::vector<CapabilityEntry> capabilities)
    : DPIDialog(parent, wxID_ANY, wxString::Format(_L("Select %s Plugin"), plugin_type_label))
    , m_capabilities(std::move(capabilities))
{
    build_capability_ui(plugin_type_label);
    CentreOnParent();
}

void PluginPickerDialog::build_ui(const wxString& plugin_type_label)
{
    SetBackgroundColour(*wxWHITE);

    const bool has_plugins = !m_plugins.empty();

    auto* top_sizer = new wxBoxSizer(wxVERTICAL);
    auto* info_text = new wxStaticText(this, wxID_ANY,
        wxString::Format(_L("Choose a %s plugin from the list below."), plugin_type_label));
    info_text->SetFont(Label::Body_14);
    info_text->SetForegroundColour(wxColour("#363636"));
    top_sizer->Add(info_text, 0, wxALL | wxEXPAND, FromDIP(10));

    top_sizer->AddSpacer(FromDIP(5));

    wxArrayString choices;
    choices.reserve(m_plugins.size());
    for (const auto& plugin : m_plugins) {
        wxString label = from_u8(plugin.name);
        if (!plugin.version.empty())
            label += wxString::Format(" (%s)", from_u8(plugin.version));
        choices.Add(label);
    }

    m_choice = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0, NULL, wxCB_READONLY);
    for (const wxString &opt : choices) { m_choice->Append(opt); }

    if (has_plugins) {
        m_choice->SetSelection(0);
        m_choice->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent& evt) {
            update_description(evt.GetSelection());
        });
    } else {
        m_choice->Enable(false);
    }

    top_sizer->Add(m_choice, 0, wxLEFT | wxRIGHT | wxEXPAND, FromDIP(10));

    m_description = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_description->SetFont(Label::Body_14);
    m_description->SetForegroundColour(wxColour("#363636"));
    m_description->Wrap(400);
    top_sizer->Add(m_description, 0, wxALL | wxEXPAND, FromDIP(10));

    if (has_plugins)
        update_description(0);
    else
        m_description->SetLabel(_L("No plugins found for this type."));

    auto dlg_btns = new DialogButtons(this, {"OK", "Cancel"});

    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) { EndModal(wxID_OK); });
    dlg_btns->GetOK()->Enable(has_plugins);

    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) { EndModal(wxID_CANCEL); });

    top_sizer->Add(dlg_btns, 0, wxEXPAND);

    SetSizerAndFit(top_sizer);

    wxGetApp().UpdateDlgDarkUI(this);
}

void PluginPickerDialog::build_capability_ui(const wxString& plugin_type_label)
{
    SetBackgroundColour(*wxWHITE);

    const bool has_capabilities = !m_capabilities.empty();

    auto* top_sizer = new wxBoxSizer(wxVERTICAL);
    auto* info_text = new wxStaticText(this, wxID_ANY,
        wxString::Format(_L("Choose a %s plugin from the list below."), plugin_type_label));
    info_text->SetFont(Label::Body_14);
    info_text->SetForegroundColour(wxColour("#363636"));

    top_sizer->Add(info_text, 0, wxALL | wxEXPAND, FromDIP(10));

    top_sizer->AddSpacer(FromDIP(5));

    wxArrayString choices;
    choices.reserve(m_capabilities.size());
    for (const auto& cap : m_capabilities)
        choices.Add(cap.label);

    m_choice = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0, NULL, wxCB_READONLY);
    for (const wxString &opt : choices) { m_choice->Append(opt); }

    if (has_capabilities) {
        m_choice->SetSelection(0);
        m_choice->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent& evt) {
            update_capability_description(evt.GetSelection());
        });
    } else {
        m_choice->Enable(false);
    }

    top_sizer->Add(m_choice, 0, wxLEFT | wxRIGHT | wxEXPAND, FromDIP(10));

    m_description = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_description->SetFont(Label::Body_14);
    m_description->SetForegroundColour(wxColour("#363636"));
    m_description->Wrap(400);
    top_sizer->Add(m_description, 0, wxALL | wxEXPAND, FromDIP(10));

    if (has_capabilities)
        update_capability_description(0);
    else
        m_description->SetLabel(_L("No plugins found for this type."));

    auto dlg_btns = new DialogButtons(this, {"OK", "Cancel"});

    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) { EndModal(wxID_OK); });
    dlg_btns->GetOK()->Enable(has_capabilities);

    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) { EndModal(wxID_CANCEL); });

    top_sizer->Add(dlg_btns, 0, wxEXPAND);

    SetSizerAndFit(top_sizer);

    wxGetApp().UpdateDlgDarkUI(this);
}

PluginPickerDialog::CapabilityEntry PluginPickerDialog::selected_capability() const
{
    if (!m_choice || !m_choice->IsEnabled())
        return {};
    int sel = m_choice->GetSelection();
    if (sel < 0 || static_cast<size_t>(sel) >= m_capabilities.size())
        return {};
    return m_capabilities[static_cast<size_t>(sel)];
}

void PluginPickerDialog::update_capability_description(int selection)
{
    if (!m_description)
        return;
    if (selection < 0 || static_cast<size_t>(selection) >= m_capabilities.size()) {
        m_description->SetLabel(wxEmptyString);
        return;
    }
    const auto& cap = m_capabilities[static_cast<size_t>(selection)];
    m_description->SetLabel(cap.description.empty() ? cap.label : cap.description);
    m_description->Wrap(400);
    Layout();
}

std::string PluginPickerDialog::selected_plugin_key() const
{
    if (!m_choice || !m_choice->IsEnabled())
        return {};
    int selection = m_choice->GetSelection();
    if (selection < 0 || static_cast<size_t>(selection) >= m_plugins.size())
        return {};
    const auto& plugin = m_plugins[static_cast<size_t>(selection)];
    return plugin.plugin_key;
}

void PluginPickerDialog::update_description(int selection)
{
    if (!m_description)
        return;
    if (selection < 0 || static_cast<size_t>(selection) >= m_plugins.size()) {
        m_description->SetLabel(wxEmptyString);
        return;
    }

    const auto& plugin = m_plugins[static_cast<size_t>(selection)];
    wxString desc;
    if (!plugin.description.empty())
        desc = from_u8(plugin.description);
    else
        desc = wxString::Format(_L("Plugin file: %s"), from_u8(plugin.entry_path));

    m_description->SetLabel(desc);
    m_description->Wrap(400);
    Layout();
}

void PluginPickerDialog::on_dpi_changed(const wxRect &suggested_rect) {}

}} // namespace Slic3r::GUI
