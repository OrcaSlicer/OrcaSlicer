#include "SettingsTransferDialog.hpp"

#include <algorithm>

#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/checklst.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include "GUI_App.hpp"
#include "Widgets/Label.hpp"

namespace Slic3r {
namespace GUI {

namespace {

wxString option_key_to_label(const SettingsTransferOption &option)
{
    wxString label = wxString::FromUTF8(option.option_key.c_str());
    label.Replace("_", " ");
    if (!label.empty())
        label[0] = static_cast<wxUniChar>(wxToupper(label[0]));

    const wxString target_value = wxString::FromUTF8(option.target_value.c_str());
    const wxString source_value = wxString::FromUTF8(option.source_value.c_str());
    if (!target_value.empty() || !source_value.empty())
        label += wxString::Format(": %s -> %s", target_value, source_value);

    return label;
}

bool contains_key(const std::vector<std::string> &keys, const std::string &key)
{
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

wxPanel* create_category_row(wxWindow *parent, wxCheckBox *&checkbox, const wxString &title, const wxString &description, bool checked)
{
    auto *panel = new wxPanel(parent, wxID_ANY);
    panel->SetBackgroundColour(*wxWHITE);

    auto *sizer = new wxBoxSizer(wxVERTICAL);
    checkbox = new wxCheckBox(panel, wxID_ANY, title);
    checkbox->SetValue(checked);
    checkbox->SetFont(::Label::Head_13);

    auto *description_label = new wxStaticText(panel, wxID_ANY, description);
    description_label->SetFont(::Label::Body_13);
    description_label->SetForegroundColour(wxColour(98, 102, 107));
    description_label->Wrap(panel->FromDIP(460));

    sizer->Add(checkbox, 0, wxBOTTOM, panel->FromDIP(4));
    sizer->Add(description_label, 0, wxLEFT, panel->FromDIP(24));
    panel->SetSizer(sizer);
    return panel;
}

} // namespace

SettingsTransferDialog::SettingsTransferDialog(wxWindow *parent, const std::vector<SettingsTransferTargetProfile> &target_profiles, const std::string &initial_target_profile, bool preselect_conditional)
    : DPIDialog(parent, wxID_ANY, _L("Transfer Settings to New Printer?"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
    , m_target_profiles(target_profiles)
{
    SetBackgroundColour(*wxWHITE);

    auto *main_sizer = new wxBoxSizer(wxVERTICAL);

    auto *description = new wxStaticText(
        this,
        wxID_ANY,
        _L("Some settings may not be compatible with the selected printer. Choose which settings to keep."));
    description->SetFont(::Label::Body_13);
    description->SetForegroundColour(wxColour(50, 58, 61));
    description->Wrap(this->FromDIP(520));

    main_sizer->Add(description, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, this->FromDIP(20));

    if (!target_profiles.empty()) {
        auto *profile_label = new wxStaticText(this, wxID_ANY, _L("Target print profile"));
        profile_label->SetFont(::Label::Head_13);
        profile_label->SetForegroundColour(wxColour(50, 58, 61));
        main_sizer->Add(profile_label, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, this->FromDIP(20));

        m_target_profile_choice = new wxChoice(this, wxID_ANY);
        m_target_profile_choice->SetMinSize(wxSize(this->FromDIP(360), -1));
        for (const SettingsTransferTargetProfile &target_profile : target_profiles)
            m_target_profile_choice->Append(wxString::FromUTF8(target_profile.profile_name.c_str()));

        int selection = m_target_profile_choice->FindString(wxString::FromUTF8(initial_target_profile.c_str()));
        if (selection == wxNOT_FOUND && m_target_profile_choice->GetCount() > 0)
            selection = 0;
        if (selection != wxNOT_FOUND)
            m_target_profile_choice->SetSelection(selection);

        main_sizer->Add(m_target_profile_choice, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, this->FromDIP(12));
    }

    auto *content = new wxPanel(this, wxID_ANY);
    content->SetBackgroundColour(*wxWHITE);
    auto *content_sizer = new wxBoxSizer(wxVERTICAL);

    content_sizer->Add(
        create_category_row(
            content,
            m_safe_checkbox,
            _L("Safe"),
            _L("Layer height, line width, walls, top and bottom, infill, supports, adhesion"),
            true),
        0,
        wxEXPAND | wxBOTTOM,
        this->FromDIP(16));

    content_sizer->Add(
        create_category_row(
            content,
            m_conditional_checkbox,
            _L("Conditional"),
            _L("Retraction, cooling, bridging"),
            preselect_conditional),
        0,
        wxEXPAND | wxBOTTOM,
        this->FromDIP(16));

    content_sizer->Add(
        create_category_row(
            content,
            m_hardware_checkbox,
            _L("Hardware-specific"),
            _L("Speed, acceleration and jerk, flow, temperature, pressure advance"),
            false),
        0,
        wxEXPAND,
        0);

    m_hardware_checkbox->SetToolTip(_L("These depend on hardware and may cause print issues"));

    auto *options_label = new wxStaticText(content, wxID_ANY, _L("Values to transfer"));
    options_label->SetFont(::Label::Head_13);
    options_label->SetForegroundColour(wxColour(50, 58, 61));
    content_sizer->Add(options_label, 0, wxEXPAND | wxBOTTOM, this->FromDIP(8));

    m_option_checklist = new wxCheckListBox(content, wxID_ANY);
    m_option_checklist->SetMinSize(wxSize(this->FromDIP(520), this->FromDIP(220)));
    content_sizer->Add(m_option_checklist, 1, wxEXPAND);

    content->SetSizer(content_sizer);
    main_sizer->Add(content, 1, wxEXPAND | wxALL, this->FromDIP(20));

    auto *button_sizer = new wxBoxSizer(wxHORIZONTAL);
    button_sizer->AddStretchSpacer();

    m_cancel_button = new ::Button(this, _L("Cancel"));
    m_cancel_button->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
    m_cancel_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });

    m_confirm_button = new ::Button(this, _L("Transfer"));
    m_confirm_button->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
    m_confirm_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_OK); });

    auto refresh_options = [this](wxCommandEvent &) { refresh_transferable_options(); };
    m_safe_checkbox->Bind(wxEVT_CHECKBOX, refresh_options);
    m_conditional_checkbox->Bind(wxEVT_CHECKBOX, refresh_options);
    m_hardware_checkbox->Bind(wxEVT_CHECKBOX, refresh_options);
    m_option_checklist->Bind(wxEVT_CHECKLISTBOX, &SettingsTransferDialog::on_option_toggled, this);
    if (m_target_profile_choice != nullptr)
        m_target_profile_choice->Bind(wxEVT_CHOICE, refresh_options);

    button_sizer->Add(m_cancel_button, 0, wxRIGHT, this->FromDIP(ButtonProps::ChoiceButtonGap()));
    button_sizer->Add(m_confirm_button, 0);

    main_sizer->Add(button_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, this->FromDIP(20));

    SetSizerAndFit(main_sizer);
    refresh_transferable_options();
    CenterOnScreen();
    wxGetApp().UpdateDlgDarkUI(this);
}

std::vector<ProcessSettingsCategory> SettingsTransferDialog::selected_categories() const
{
    std::vector<ProcessSettingsCategory> categories;
    if (m_safe_checkbox->GetValue())
        categories.emplace_back(ProcessSettingsCategory::Safe);
    if (m_conditional_checkbox->GetValue())
        categories.emplace_back(ProcessSettingsCategory::Conditional);
    if (m_hardware_checkbox->GetValue())
        categories.emplace_back(ProcessSettingsCategory::HardwareSpecific);
    return categories;
}

std::vector<std::string> SettingsTransferDialog::selected_option_keys() const
{
    std::vector<std::string> selected_keys;
    if (m_option_checklist == nullptr)
        return selected_keys;

    const unsigned int count = std::min<unsigned int>(m_option_checklist->GetCount(), static_cast<unsigned int>(m_displayed_options.size()));
    for (unsigned int index = 0; index < count; ++index) {
        if (m_option_checklist->IsChecked(index))
            selected_keys.emplace_back(m_displayed_options[index].option_key);
    }
    return selected_keys;
}

std::string SettingsTransferDialog::selected_target_profile_name() const
{
    if (m_target_profile_choice == nullptr)
        return {};

    const int selection = m_target_profile_choice->GetSelection();
    if (selection == wxNOT_FOUND)
        return {};

    return m_target_profile_choice->GetString(selection).ToUTF8().data();
}

void SettingsTransferDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    wxUnusedVar(suggested_rect);
    if (m_confirm_button)
        m_confirm_button->Rescale();
    if (m_cancel_button)
        m_cancel_button->Rescale();
    Fit();
}

void SettingsTransferDialog::on_option_toggled(wxCommandEvent &event)
{
    if (m_option_checklist == nullptr)
        return;

    const int selection = event.GetInt();
    if (selection == wxNOT_FOUND || selection < 0 || static_cast<size_t>(selection) >= m_displayed_options.size())
        return;

    const std::vector<std::string> default_selected_keys = ProcessSettingsMerger::option_keys_for_categories(selected_categories());
    const std::string &option_key = m_displayed_options[selection].option_key;
    const bool default_checked = contains_key(default_selected_keys, option_key);
    const bool checked = m_option_checklist->IsChecked(static_cast<unsigned int>(selection));

    if (checked == default_checked)
        m_manual_option_overrides.erase(option_key);
    else
        m_manual_option_overrides[option_key] = checked;
}

void SettingsTransferDialog::refresh_transferable_options()
{
    if (m_option_checklist == nullptr)
        return;

    const std::string selected_profile_name = selected_target_profile_name();
    const SettingsTransferTargetProfile *selected_profile = nullptr;
    for (const SettingsTransferTargetProfile &target_profile : m_target_profiles) {
        if (target_profile.profile_name == selected_profile_name) {
            selected_profile = &target_profile;
            break;
        }
    }
    if (selected_profile == nullptr && !m_target_profiles.empty())
        selected_profile = &m_target_profiles.front();

    m_displayed_options = selected_profile != nullptr ? selected_profile->transferable_options : std::vector<SettingsTransferOption>{};
    m_option_checklist->Clear();

    const std::vector<std::string> default_selected_keys = ProcessSettingsMerger::option_keys_for_categories(selected_categories());
    for (const SettingsTransferOption &option : m_displayed_options) {
        const unsigned int idx = m_option_checklist->Append(option_key_to_label(option));
        const auto override_it = m_manual_option_overrides.find(option.option_key);
        const bool default_checked = contains_key(default_selected_keys, option.option_key);
        const bool should_check = override_it != m_manual_option_overrides.end() ? override_it->second : default_checked;
        m_option_checklist->Check(idx, should_check);
    }
}

void SettingsTransferDialog::on_sys_color_changed()
{
    wxGetApp().UpdateDlgDarkUI(this);
    Refresh();
}

} // namespace GUI
} // namespace Slic3r