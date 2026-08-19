#include "PluginPickerDialog.hpp"

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/valgen.h>

#include "GUI.hpp"
#include "I18N.hpp"

namespace Slic3r { namespace GUI {

PluginPickerDialog::PluginPickerDialog(wxWindow* parent,
                                       const wxString& plugin_type_label,
                                       const std::vector<Slic3r::PluginDescriptor>& plugins)
    : wxDialog(parent, wxID_ANY, wxString::Format(_L("Select %s Plugin"), plugin_type_label))
    , m_plugins(plugins)
    , m_capability_mode(false)
{
    build_ui(plugin_type_label);
    CentreOnParent();
}

PluginPickerDialog::PluginPickerDialog(wxWindow* parent,
                                       const wxString& plugin_type_label,
                                       std::vector<CapabilityEntry> capabilities)
    : wxDialog(parent, wxID_ANY, wxString::Format(_L("Select %s Plugin"), plugin_type_label))
    , m_capabilities(std::move(capabilities))
    , m_capability_mode(true)
{
    build_capability_ui(plugin_type_label);
    CentreOnParent();
}

void PluginPickerDialog::build_ui(const wxString& plugin_type_label)
{
    const bool has_plugins = !m_plugins.empty();

    auto* top_sizer = new wxBoxSizer(wxVERTICAL);
    auto* info_text = new wxStaticText(this, wxID_ANY,
        wxString::Format(_L("Choose a %s plugin from the list below."), plugin_type_label));
    top_sizer->Add(info_text, 0, wxALL | wxEXPAND, 10);

    wxArrayString choices;
    choices.reserve(m_plugins.size());
    for (const auto& plugin : m_plugins) {
        wxString label = from_u8(plugin.name);
        if (!plugin.version.empty())
            label += wxString::Format(" (%s)", from_u8(plugin.version));
        choices.Add(label);
    }

    m_choice = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, choices);
    if (has_plugins) {
        m_choice->SetSelection(0);
        m_choice->Bind(wxEVT_CHOICE, [this](wxCommandEvent& evt) {
            update_description(evt.GetSelection());
        });
    } else {
        m_choice->Enable(false);
    }

    top_sizer->Add(m_choice, 0, wxLEFT | wxRIGHT | wxEXPAND, 10);

    m_description = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_description->Wrap(400);
    top_sizer->Add(m_description, 0, wxALL | wxEXPAND, 10);

    if (has_plugins)
        update_description(0);
    else
        m_description->SetLabel(_L("No plugins found for this type."));

    auto* button_sizer = new wxStdDialogButtonSizer();
    auto* ok_button = new wxButton(this, wxID_OK);
    ok_button->Enable(has_plugins);
    button_sizer->AddButton(ok_button);
    button_sizer->AddButton(new wxButton(this, wxID_CANCEL));
    button_sizer->Realize();

    top_sizer->Add(button_sizer, 0, wxALL | wxALIGN_RIGHT, 10);

    SetSizerAndFit(top_sizer);
}

void PluginPickerDialog::build_capability_ui(const wxString& plugin_type_label)
{
    const bool has_capabilities = !m_capabilities.empty();

    auto* top_sizer = new wxBoxSizer(wxVERTICAL);
    auto* info_text = new wxStaticText(this, wxID_ANY,
        wxString::Format(_L("Choose a %s plugin from the list below."), plugin_type_label));
    top_sizer->Add(info_text, 0, wxALL | wxEXPAND, 10);

    wxArrayString choices;
    choices.reserve(m_capabilities.size());
    for (const auto& cap : m_capabilities)
        choices.Add(cap.label);

    m_choice = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, choices);
    if (has_capabilities) {
        m_choice->SetSelection(0);
        m_choice->Bind(wxEVT_CHOICE, [this](wxCommandEvent& evt) {
            update_capability_description(evt.GetSelection());
        });
    } else {
        m_choice->Enable(false);
    }

    top_sizer->Add(m_choice, 0, wxLEFT | wxRIGHT | wxEXPAND, 10);

    m_description = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_description->Wrap(400);
    top_sizer->Add(m_description, 0, wxALL | wxEXPAND, 10);

    if (has_capabilities)
        update_capability_description(0);
    else
        m_description->SetLabel(_L("No plugins found for this type."));

    auto* button_sizer = new wxStdDialogButtonSizer();
    auto* ok_button = new wxButton(this, wxID_OK);
    ok_button->Enable(has_capabilities);
    button_sizer->AddButton(ok_button);
    button_sizer->AddButton(new wxButton(this, wxID_CANCEL));
    button_sizer->Realize();

    top_sizer->Add(button_sizer, 0, wxALL | wxALIGN_RIGHT, 10);

    SetSizerAndFit(top_sizer);
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

}} // namespace Slic3r::GUI
