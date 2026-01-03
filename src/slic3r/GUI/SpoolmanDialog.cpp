#include "SpoolmanDialog.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "OptionsGroup.hpp"
#include "Plater.hpp"
#include "Spoolman.hpp"
#include "SpoolmanConfig.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/LabeledStaticBox.hpp"
#include "wx/sizer.h"
#include "format.hpp"
#include "Widgets/ComboBox.hpp"
#define EM wxGetApp().em_unit()

namespace Slic3r::GUI {

wxDEFINE_EVENT(EVT_FINISH_LOADING, wxCommandEvent);
wxDEFINE_EVENT(EVT_SPOOL_WIDGET_SELECTION, wxCommandEvent);
static BitmapCache cache;

SpoolInfoWidget::SpoolInfoWidget(wxWindow* parent, const Preset* preset) : wxPanel(parent, wxID_ANY), m_preset(preset)
{
    this->SetBackgroundColour(*wxWHITE);
    auto main_sizer = new wxStaticBoxSizer(new LabeledStaticBox(this), wxVERTICAL);

    auto bitmap = cache.load_svg("spool", EM * 10, EM * 10, false, false,
                                 {{"#009688", m_preset->config.opt_string("default_filament_colour", 0)}});

    m_spool_bitmap = new wxStaticBitmap(this, wxID_ANY, *bitmap);
    m_spool_bitmap->SetMinSize({EM * 10, EM * 10});
    main_sizer->Add(m_spool_bitmap, 0, wxALIGN_CENTER_HORIZONTAL | wxALL, EM);

    m_preset_name_label = new Label(this, wxString::FromUTF8(preset->name));
    m_preset_name_label->SetFont(Label::Body_12);
    main_sizer->Add(m_preset_name_label, 0, wxALIGN_CENTER_HORIZONTAL | wxDOWN | wxLEFT | wxRIGHT, EM);

    wxWindow* control;
    if (preset->spoolman_enabled()) {
        m_combobox           = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0, nullptr, wxCB_READONLY);
        control               = m_combobox;
        m_combobox->GetDropDown().SetUseContentWidth(true);
        m_combobox->Bind(wxEVT_COMBOBOX, [&](wxCommandEvent& e) {
            auto current_sel = e.GetInt();
            auto evt = new wxCommandEvent(EVT_SPOOL_WIDGET_SELECTION);
            evt->SetString(m_preset->name);
            evt->SetInt(current_sel);
            evt->SetClientData(m_combobox->GetClientData(current_sel));
            evt->SetEventObject(this);
            wxQueueEvent(m_parent, evt);
        });

        const auto spoolman   = Spoolman::get_instance();
        const auto curr_spool = spoolman->get_spoolman_spool_by_id(preset->config.opt_int("spoolman_spool_id", 0));
        for (const auto& [spool_id, spool] : spoolman->get_spoolman_spools()) {
            if (spool->filament == curr_spool->filament) {
                m_combobox->Append(format("#%1% - %2% g / %3% g", spool_id, double_to_string(spool->remaining_weight, 2),
                                          double_to_string(spool->filament->weight, 2)),
                                   wxNullBitmap, &spool->id);
                if (curr_spool == spool)
                    m_combobox->SetSelection(m_combobox->GetCount() - 1);
            }
        }

        m_combobox->SetMinSize({m_combobox->GetDropDown().GetSize().GetX(), -1});
    } else {
        auto label = new Label(this);
        control = label;
        label->SetLabelText(_L("Not Spoolman enabled"));
        label->SetForegroundColour(*wxRED);
        control->SetFont(Label::Body_12);
    }
    main_sizer->Add(control, 0, wxALIGN_CENTER_HORIZONTAL | wxDOWN | wxLEFT | wxRIGHT, EM);

    wxGetApp().UpdateDarkUIWin(this);
    this->SetSizer(main_sizer);
}

void SpoolInfoWidget::rescale()
{
    auto bitmap = cache.load_svg("spool", EM * 10, EM * 10, false, false,
                                 {{"#009688", m_preset->config.opt_string("default_filament_colour", 0)}});
    m_spool_bitmap->SetBitmap(*bitmap);
    m_spool_bitmap->SetMinSize({EM * 10, EM * 10});
    if (m_combobox)
        m_combobox->Rescale();
}

SpoolmanDialog::SpoolmanDialog(wxWindow* parent)
    : DPIDialog(parent, wxID_ANY, _L("Spoolman"), wxDefaultPosition, {-1, 45 * EM}, wxDEFAULT_DIALOG_STYLE)
{
    this->SetBackgroundColour(*wxWHITE);

    auto window_sizer = new wxBoxSizer(wxVERTICAL);
    window_sizer->SetMinSize({wxDefaultCoord, 45 * EM});

    // Main panel
    m_main_panel = new wxPanel(this);
    m_main_panel->SetBackgroundColour(*wxWHITE);
    auto main_panel_sizer = new wxBoxSizer(wxVERTICAL);
    m_main_panel->SetSizer(main_panel_sizer);

    m_config = new SpoolmanDynamicConfig(wxGetApp().app_config);
    m_optgroup = new ConfigOptionsGroup(m_main_panel, _L("Spoolman Options"), wxEmptyString, m_config);
    build_options_group();
    m_optgroup->m_on_change = [&](const std::string& key, const boost::any& value) {
        m_dirty_settings = true;
        if (key == "spoolman_host")
            m_dirty_host = true;
    };
    main_panel_sizer->Add(m_optgroup->sizer, 0, wxEXPAND | wxLEFT | wxTOP | wxRIGHT, EM);

    m_spoolman_error_label_sizer = new wxBoxSizer(wxVERTICAL);

    m_spoolman_error_label = new Label(m_main_panel);
    m_spoolman_error_label->SetFont(Label::Body_16);
    m_spoolman_error_label->SetForegroundColour(*wxRED);

    m_spoolman_error_label_sizer->AddStretchSpacer(1);
    m_spoolman_error_label_sizer->Add(m_spoolman_error_label, 0, wxALIGN_CENTER);
    m_spoolman_error_label_sizer->AddStretchSpacer(1);
    main_panel_sizer->Add(m_spoolman_error_label_sizer, 1, wxALL | wxALIGN_CENTER | wxEXPAND, EM);

    m_info_widgets_parent_sizer = new wxBoxSizer(wxVERTICAL);
    m_info_widgets_grid_sizer = new wxGridSizer(2, EM, EM);
    m_info_widgets_parent_sizer->Add(m_info_widgets_grid_sizer, 0, wxALIGN_CENTER);
    m_info_widgets_parent_sizer->AddStretchSpacer();
    main_panel_sizer->Add(m_info_widgets_parent_sizer, 1, wxALL | wxALIGN_CENTER, EM);

    m_buttons = new DialogButtons(m_main_panel, {"Refresh", "OK"});
    m_buttons->UpdateButtons();
    m_buttons->GetButtonFromLabel(_L("Refresh"))->Bind(wxEVT_BUTTON, &SpoolmanDialog::OnRefresh, this);
    m_buttons->GetOK()->Bind(wxEVT_BUTTON, &SpoolmanDialog::OnOK, this);
    main_panel_sizer->Add(m_buttons, 0, wxALIGN_BOTTOM | wxEXPAND | wxLEFT | wxRIGHT, EM);
    window_sizer->Add(m_main_panel, 1, wxEXPAND);

    // Loading Panel
    m_loading_panel = new wxPanel(this);
    m_loading_panel->SetBackgroundColour(*wxWHITE);
    auto loading_panel_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_loading_panel->SetSizer(loading_panel_sizer);
    m_loading_panel->SetMinSize({main_panel_sizer->CalcMin().GetWidth(), wxDefaultCoord});
    loading_panel_sizer->AddStretchSpacer(1);

    m_loading_spinner = new LoadingSpinner(m_loading_panel, {5 * EM, 5 * EM});
    loading_panel_sizer->Add(m_loading_spinner, 0, wxALIGN_CENTER | wxRIGHT, EM);

    auto loading_label = new Label(m_loading_panel, Label::Body_16, _L("Loading..."));
    loading_panel_sizer->Add(loading_label, 0, wxALIGN_CENTER);

    loading_panel_sizer->AddStretchSpacer(1);
    window_sizer->Add(m_loading_panel, 1, wxEXPAND);

    build_spool_info();

    this->SetSizerAndFit(window_sizer);
    this->SetMinSize(wxDefaultSize);

    this->Bind(EVT_FINISH_LOADING, &SpoolmanDialog::OnFinishLoading, this);
    this->Bind(EVT_SPOOL_WIDGET_SELECTION, &SpoolmanDialog::OnSpoolWidgetSelection, this);

    wxGetApp().UpdateDlgDarkUI(this);
    this->ShowModal();
}

SpoolmanDialog::~SpoolmanDialog()
{
    delete m_config;
}

void SpoolmanDialog::build_options_group() const
{
    m_optgroup->append_single_option_line("spoolman_enabled");
    m_optgroup->append_single_option_line("spoolman_host");
    m_optgroup->append_single_option_line("spoolman_consumption_type");

    m_optgroup->activate();
    m_optgroup->reload_config();
}

void SpoolmanDialog::build_spool_info()
{
    show_loading();
    m_info_widgets_parent_sizer->Show(false);
    m_spoolman_error_label_sizer->Show(false);
    create_thread([&] {
        m_info_widgets_grid_sizer->Clear();
        bool show_widgets = false;
        if (!Spoolman::is_enabled()) {
            m_spoolman_error_label->SetLabelText(_L("Spoolman is not enabled"));
            m_spoolman_error_label_sizer->Show(true);
        } else if (!Spoolman::is_server_valid()) {
            m_spoolman_error_label->SetLabelText(_L("Spoolman server is invalid"));
            m_spoolman_error_label_sizer->Show(true);
        } else {
            show_widgets = true;
        }

        // Finish loading on the main thread
        auto evt = new wxCommandEvent(EVT_FINISH_LOADING);
        evt->SetInt(show_widgets);
        wxQueueEvent(this, evt);
    });
}

void SpoolmanDialog::show_loading(bool show)
{
    m_main_panel->Show(!show);
    m_loading_panel->Show(show);
    Layout();
    if (!show)
        Fit();
    Refresh();
}

void SpoolmanDialog::save_spoolman_settings()
{
    // clear the Spoolman cache and reload if any of the Spoolman settings change
    if (!m_dirty_settings && m_spool_id_updates.empty())
        return;

    if (!m_spool_id_updates.empty()) {
        auto& filaments = wxGetApp().preset_bundle->filaments;
        for (auto& [preset_name, spool_id] : m_spool_id_updates) {
            if (const auto filament = filaments.find_preset(preset_name, false, true)) {
                filament->config.opt_int("spoolman_spool_id", 0) = spool_id;
                if (auto& edited = filaments.get_edited_preset(); edited.name == preset_name) {
                    edited.config.opt_int("spoolman_spool_id", 0) = spool_id;
                }
            }
        }
        m_spool_id_updates.clear();
    }

    // Save config values to the app config
    if (m_dirty_settings)
        m_config->save_to_appconfig(wxGetApp().app_config);

    if (m_dirty_host)
        Spoolman::on_server_changed();
    Spoolman::update_visible_spool_statistics(m_dirty_host);
    m_dirty_settings = false;
    m_dirty_host = false;
}

void SpoolmanDialog::OnFinishLoading(wxCommandEvent& event)
{
    if (event.GetInt()) {
        m_info_widgets_parent_sizer->Show(true);
        auto preset_bundle = wxGetApp().preset_bundle;
        for (auto& filament_preset_name : preset_bundle->filament_presets) {
            m_info_widgets_grid_sizer->Add(new SpoolInfoWidget(m_main_panel, preset_bundle->filaments.find_preset(filament_preset_name)), 0, wxEXPAND);
        }
    }
    show_loading(false);
}

void SpoolmanDialog::OnRefresh(wxCommandEvent& e)
{
    save_spoolman_settings();
    build_spool_info();
}

void SpoolmanDialog::OnOK(wxCommandEvent& e)
{
    save_spoolman_settings();
    this->EndModal(wxID_OK);
}

void SpoolmanDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    GetSizer()->SetMinSize(wxDefaultCoord, 45 * EM);
    m_optgroup->msw_rescale();
    for (auto item : m_info_widgets_grid_sizer->GetChildren())
        if (auto info_widget = dynamic_cast<SpoolInfoWidget*>(item->GetWindow()))
            info_widget->rescale();
    Fit();
    Refresh();
}

void SpoolmanDialog::OnSpoolWidgetSelection(wxCommandEvent& e)
{
    const auto preset_name = e.GetString().ToStdString();
    const auto new_spool_id = *static_cast<int*>(e.GetClientData());
    for (const auto item : m_info_widgets_grid_sizer->GetChildren())
        if (const auto info_widget = dynamic_cast<SpoolInfoWidget*>(item->GetWindow()); info_widget->get_preset_name() == preset_name)
            info_widget->set_combobox_selection(e.GetInt());
    m_spool_id_updates[preset_name] = new_spool_id;
}
} // namespace Slic3r::GUI
