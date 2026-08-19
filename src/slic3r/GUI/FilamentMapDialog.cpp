#include "FilamentMapDialog.hpp"
#include "PartPlate.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "I18N.hpp"
#include "GUI_App.hpp"
#include "CapsuleButton.hpp"
#include "MsgDialog.hpp"

namespace Slic3r { namespace GUI {

static bool get_pop_up_remind_flag()
{
    auto &app_config = wxGetApp().app_config;
    return app_config->get_bool("pop_up_filament_map_dialog");
}

static void set_pop_up_remind_flag(bool remind)
{
    auto &app_config = wxGetApp().app_config;
    app_config->set_bool("pop_up_filament_map_dialog", remind);
}

static FilamentMapMode get_applied_map_mode(DynamicConfig& proj_config, const Plater* plater_ref, const PartPlate* partplate_ref, const bool sync_plate)
{
    if (sync_plate)
        return partplate_ref->get_real_filament_map_mode(proj_config);
    return plater_ref->get_global_filament_map_mode();
}

static std::vector<int> get_applied_map(DynamicConfig& proj_config, const Plater* plater_ref, const PartPlate* partplate_ref, const bool sync_plate)
{
    if (sync_plate)
        return partplate_ref->get_real_filament_maps(proj_config);
    return plater_ref->get_global_filament_map();
}

static std::vector<int> get_applied_volume_map(DynamicConfig& proj_config, const Plater* plater_ref, const PartPlate* partplate_ref, const bool sync_plate)
{
    if (sync_plate)
        return partplate_ref->get_real_filament_volume_maps(proj_config);
    return plater_ref->get_global_filament_volume_map();
}

extern std::string& get_left_extruder_unprintable_text();
extern std::string& get_right_extruder_unprintable_text();

// Orca: minimal smart-filament toggle. When a filament track switch is ready every AMS filament is
// reachable from both nozzles, so one filament can be assigned to multiple nozzles to maximize
// savings. The checkbox drives the enable_filament_dynamic_map project flag.
class SmartFilamentPanel : public wxPanel
{
    static constexpr int spacing = 20;

public:
    SmartFilamentPanel(wxWindow *parent) : wxPanel(parent)
    {
        SetBackgroundColour(*wxWHITE);
        wxBoxSizer *main_sizer = new wxBoxSizer(wxVERTICAL);

        main_sizer->AddSpacer(FromDIP(spacing));

        auto *separator = new wxPanel(this);
        separator->SetBackgroundColour(wxColour("#EEEEEE"));
        main_sizer->Add(separator, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(15));

        main_sizer->AddSpacer(FromDIP(spacing));

        m_smart_filament_checkbox = new CheckBox(this);
        if (auto *opt = enable_filament_dynamic_map())
            m_smart_filament_checkbox->SetValue(opt->value);
        m_smart_filament_checkbox->Bind(wxEVT_TOGGLEBUTTON, &SmartFilamentPanel::on_smart_filament_checkbox, this);

        auto *label = new Label(this, _L("Enable smart filament assign: Assign one filament to multiple nozzles to maximize savings"));
        label->SetFont(Label::Body_12);

        // Orca: dropped the vendor "Learn more" tracking link (no Orca help page for this feature).

        auto *smart_sizer = new wxBoxSizer(wxHORIZONTAL);
        smart_sizer->Add(m_smart_filament_checkbox, 0, wxALIGN_CENTER_VERTICAL);
        smart_sizer->Add(label, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(3));

        main_sizer->Add(smart_sizer, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(15));

        SetSizer(main_sizer);
        Layout();
        Fit();
        wxGetApp().UpdateDarkUIWin(this);
    }

private:
    static ConfigOptionBool *enable_filament_dynamic_map()
    {
        auto &config = wxGetApp().preset_bundle->project_config;
        return dynamic_cast<ConfigOptionBool *>(config.option("enable_filament_dynamic_map"));
    }

    void on_smart_filament_checkbox(wxCommandEvent &event)
    {
        if (auto *opt = enable_filament_dynamic_map())
            opt->value = m_smart_filament_checkbox->GetValue();
        wxGetApp().plater()->update();
        event.Skip();
    }

private:
    CheckBox *m_smart_filament_checkbox{nullptr};
};


bool try_pop_up_before_slice(bool is_slice_all, Plater* plater_ref, PartPlate* partplate_ref, bool force_pop_up)
{
    auto full_config = wxGetApp().preset_bundle->full_config();
    const auto nozzle_diameters = full_config.option<ConfigOptionFloats>("nozzle_diameter");
    if (nozzle_diameters->size() <= 1)
        return true;

    // The filament-grouping dialog is specifically designed for BBL dual-nozzle printers
    // (e.g. H2D) where filaments must be assigned to a left or right nozzle.
    // For toolchangers (≥3 tools) and all non-BBL printers the dialog is irrelevant and
    // confusing; skip it entirely so slicing proceeds without interruption. (#12390)
    PresetBundle* preset = wxGetApp().preset_bundle;
    if (!preset || !preset->is_bbl_vendor() || nozzle_diameters->size() != 2)
        return true;

    bool sync_plate = true;

    std::vector<std::string> filament_colors = full_config.option<ConfigOptionStrings>("filament_colour")->values;
    std::vector<std::string> filament_types = full_config.option<ConfigOptionStrings>("filament_type")->values;
    FilamentMapMode applied_mode = get_applied_map_mode(full_config, plater_ref,partplate_ref, sync_plate);
    std::vector<int> applied_maps = get_applied_map(full_config, plater_ref, partplate_ref, sync_plate);
    std::vector<int> applied_volume_maps = get_applied_volume_map(full_config, plater_ref, partplate_ref, sync_plate);
    applied_maps.resize(filament_colors.size(), 1);
    applied_volume_maps.resize(filament_colors.size(), 0);

    if (!force_pop_up && applied_mode != fmmManual)
        return true;

    std::vector<int> filament_lists;
    if (is_slice_all) {
        filament_lists.resize(filament_colors.size());
        std::iota(filament_lists.begin(), filament_lists.end(), 1);
    }
    else {
        filament_lists = partplate_ref->get_extruders();
    }

    FilamentMapDialog map_dlg(plater_ref,
        filament_colors,
        filament_types,
        applied_maps,
        applied_volume_maps,
        filament_lists,
        applied_mode,
        plater_ref->get_machine_sync_status(),
        false,
        false
    );
    auto ret = map_dlg.ShowModal();

    if (ret == wxID_OK) {
        FilamentMapMode new_mode = map_dlg.get_mode();
        std::vector<int> new_maps = map_dlg.get_filament_maps();
        std::vector<int> new_volume_maps = map_dlg.get_filament_volume_maps();
        if (sync_plate) {
            if (is_slice_all) {
                auto plate_list = plater_ref->get_partplate_list().get_plate_list();
                for (int i = 0; i < plate_list.size(); ++i) {
                    plate_list[i]->set_filament_map_mode(new_mode);
                    if (new_mode == fmmManual) {
                        plate_list[i]->set_filament_maps(new_maps);
                        plate_list[i]->set_filament_volume_maps(new_volume_maps);
                    }
                }
            }
            else {
                partplate_ref->set_filament_map_mode(new_mode);
                if (new_mode == fmmManual) {
                    partplate_ref->set_filament_maps(new_maps);
                    partplate_ref->set_filament_volume_maps(new_volume_maps);
                }
            }
        }
        else {
            plater_ref->set_global_filament_map_mode(new_mode);
            if (new_mode == fmmManual) {
                plater_ref->set_global_filament_map(new_maps);
                plater_ref->set_global_filament_volume_map(new_volume_maps);
            }
        }
        plater_ref->update();
        // check whether able to slice, if not, return false
        if (!get_left_extruder_unprintable_text().empty() || !get_right_extruder_unprintable_text().empty()){
            return false;
        }
        return true;
    }
    return false;
}

FilamentMapDialog::FilamentMapDialog(wxWindow                       *parent,
                                     const std::vector<std::string> &filament_color,
                                     const std::vector<std::string> &filament_type,
                                     const std::vector<int>         &filament_map,
                                     const std::vector<int>         &filament_volume_map,
                                     const std::vector<int>         &filaments,
                                     const FilamentMapMode           mode,
                                     bool                            machine_synced,
                                     bool                            show_default,
                                     bool                            with_checkbox)
    : wxDialog(parent, wxID_ANY, _L("Filament grouping"), wxDefaultPosition, wxDefaultSize,wxDEFAULT_DIALOG_STYLE)
    , m_filament_color(filament_color)
    , m_filament_type(filament_type)
    , m_filament_map(filament_map)
    , m_filament_volume_map(filament_volume_map)
{
    SetBackgroundColour(*wxWHITE);

    SetMinSize(wxSize(FromDIP(580), -1));
    SetMaxSize(wxSize(FromDIP(580), -1));

    // Orca: when a filament track switch is ready, every AMS filament reaches both nozzles, so the
    // Match/Convenience sub-mode is dropped and Auto is presented purely as a filament-saving mode.
    // Orca has no fmmAutoForQuality, so "only saving" collapses to "switch ready" and the remaining
    // auto mode set is { fmmAutoForFlush }.
    m_fila_switch_ready              = wxGetApp().sidebar().is_fila_switch_ready();
    const bool only_saving_mode     = m_fila_switch_ready;
    const bool auto_match_available = machine_synced && !m_fila_switch_ready;

    if (mode < fmmManual)
        m_page_type = PageType::ptAuto;
    else if (mode == fmmManual || mode == fmmNozzleManual)
        // Orca: there is no dedicated nozzle-manual page, so treat an fmmNozzleManual plate as a manual variant and show the Custom page instead of misfiling it as "Same as Global".
        m_page_type = PageType::ptManual;
    else
        m_page_type = PageType::ptDefault;

    wxBoxSizer *main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->AddSpacer(FromDIP(22));

    wxBoxSizer *mode_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_auto_btn   = new CapsuleButton(this, PageType::ptAuto, only_saving_mode ? _L("Fila Saving") : _L("Auto"), false);
    m_manual_btn = new CapsuleButton(this, PageType::ptManual, _L("Custom"), false);
    if (show_default)
        m_default_btn = new CapsuleButton(this, PageType::ptDefault, _L("Same as Global"), true);
    else
        m_default_btn = nullptr;

    const int button_padding = FromDIP(2);
    mode_sizer->AddStretchSpacer();
    mode_sizer->Add(m_auto_btn, 1, wxALIGN_CENTER | wxLEFT | wxRIGHT, button_padding);
    mode_sizer->Add(m_manual_btn, 1, wxALIGN_CENTER | wxLEFT | wxRIGHT, button_padding);
    if (show_default) mode_sizer->Add(m_default_btn, 1, wxALIGN_CENTER | wxLEFT | wxRIGHT, button_padding);
    mode_sizer->AddStretchSpacer();

    main_sizer->Add(mode_sizer, 0, wxEXPAND);
    main_sizer->AddSpacer(FromDIP(24));

    auto            panel_sizer       = new wxBoxSizer(wxHORIZONTAL);

    // Orca: fall back to the saving mode whenever Match is unavailable, which now also covers the
    // filament-track-switch-ready case (auto_match_available folds in !m_fila_switch_ready).
    FilamentMapMode default_auto_mode = mode >= fmmManual ? fmmAutoForFlush :
        mode == fmmAutoForMatch && !auto_match_available ? fmmAutoForFlush :
        mode;

    m_manual_map_panel                = new FilamentMapManualPanel(this, m_filament_color, m_filament_type, filaments, filament_map, filament_volume_map);
    // A manual grouping can point filaments at a nozzle volume the extruder does not physically
    // carry; the panel's validation timer reports that here so OK is gated on a printable map.
    m_manual_map_panel->Bind(wxEVT_INVALID_MANUAL_MAP, [this](wxCommandEvent &event) {
        if (m_page_type != PageType::ptManual) {
            if (!m_ok_btn->IsEnabled()) { m_ok_btn->Enable(); }
            return;
        }
        if (event.GetInt()) {
            if (!m_ok_btn->IsEnabled()) { m_ok_btn->Enable(); }
        } else {
            if (m_ok_btn->IsEnabled()) { m_ok_btn->Disable(); }
        }
    });
    m_auto_map_panel                  = new FilamentMapAutoPanel(this, default_auto_mode, auto_match_available);
    if (show_default)
        m_default_map_panel = new FilamentMapDefaultPanel(this);
    else
        m_default_map_panel = nullptr;

    panel_sizer->Add(m_manual_map_panel, 0, wxEXPAND);
    panel_sizer->Add(m_auto_map_panel, 0, wxEXPAND);
    if (show_default) panel_sizer->Add(m_default_map_panel, 0, wxEXPAND);
    main_sizer->Add(panel_sizer, 0, wxEXPAND);

    // Smart filament section, shown only in filament-saving (flush) mode when the switch is ready.
    if (m_fila_switch_ready) {
        m_smart_filament = new SmartFilamentPanel(this);
        m_smart_filament->Show(get_mode() == fmmAutoForFlush);
        main_sizer->Add(m_smart_filament, 0, wxEXPAND);
    }

    wxPanel* bottom_panel = new wxPanel(this);
    bottom_panel->SetBackgroundColour(*wxWHITE);
    wxBoxSizer *bottom_sizer = new wxBoxSizer(wxHORIZONTAL);
    bottom_panel->SetSizer(bottom_sizer);
    bottom_sizer->Fit(bottom_panel);

    if(with_checkbox)
    {
        auto* checkbox_sizer = new wxBoxSizer(wxHORIZONTAL);
        m_checkbox = new CheckBox(bottom_panel);
        m_checkbox->Bind(wxEVT_TOGGLEBUTTON, &FilamentMapDialog::on_checkbox, this);
        checkbox_sizer->Add(m_checkbox, 0, wxALIGN_CENTER, 0);

        auto* checkbox_label = new Label(bottom_panel, _L("Don't remind me again"));
        checkbox_label->SetFont(Label::Body_12);
        checkbox_sizer->Add(checkbox_label, 0, wxLEFT| wxALIGN_CENTER , FromDIP(3));

        bottom_sizer->Add(checkbox_sizer, 0 ,  wxALIGN_CENTER | wxALL, FromDIP(15));
    }

    bottom_sizer->AddStretchSpacer();

    {
        auto dlg_btns = new DialogButtons(bottom_panel, {"OK", "Cancel"});
        m_ok_btn      = dlg_btns->GetOK();
        m_cancel_btn  = dlg_btns->GetCANCEL();

        bottom_sizer->Add(dlg_btns, 0, wxEXPAND);
    }
    main_sizer->Add(bottom_panel, 0, wxEXPAND);

    m_ok_btn->Bind(wxEVT_BUTTON, &FilamentMapDialog::on_ok, this);
    m_cancel_btn->Bind(wxEVT_BUTTON, &FilamentMapDialog::on_cancel, this);
    SetEscapeId(wxID_CANCEL);
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
        if (e.GetKeyCode() == WXK_ESCAPE) {
            if (IsModal())
                EndModal(wxID_CANCEL);
            else
                Close();
            return;
        }
        e.Skip();
    });

    m_auto_btn->Bind(wxEVT_BUTTON, &FilamentMapDialog::on_switch_mode, this);
    m_manual_btn->Bind(wxEVT_BUTTON, &FilamentMapDialog::on_switch_mode, this);
    if (show_default) m_default_btn->Bind(wxEVT_BUTTON, &FilamentMapDialog::on_switch_mode, this);

    SetSizer(main_sizer);
    Layout();
    Fit();

    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

FilamentMapMode FilamentMapDialog::get_mode()
{
    if (m_page_type == PageType::ptAuto) return m_auto_map_panel->GetMode();
    if (m_page_type == PageType::ptManual) return fmmManual;
    return fmmDefault;
}

int FilamentMapDialog::ShowModal()
{
    update_panel_status(m_page_type);
    return wxDialog::ShowModal();
}

void FilamentMapDialog::on_checkbox(wxCommandEvent &event)
{
    bool is_checked = m_checkbox->GetValue();
    m_checkbox->SetValue(is_checked);
    set_pop_up_remind_flag(!is_checked);

    if (is_checked) {
        MessageDialog dialog(nullptr, _L("No further pop-up will appear. You can reopen it in 'Preferences'"), _L("Tips"), wxICON_INFORMATION | wxOK);
        dialog.ShowModal();
        this->Close();
    }

    event.Skip();
}

void FilamentMapDialog::on_ok(wxCommandEvent &event)
{
    if (m_page_type == PageType::ptManual) {
        m_filament_map        = m_manual_map_panel->GetFilamentMaps();
        m_filament_volume_map = m_manual_map_panel->GetFilamentVolumeMaps();
    }

    EndModal(wxID_OK);
}

void FilamentMapDialog::on_cancel(wxCommandEvent &event) { EndModal(wxID_CANCEL); }

void FilamentMapDialog::update_panel_status(PageType page)
{
    std::vector<CapsuleButton*>button_list = { m_default_btn,m_manual_btn,m_auto_btn };
    for (auto p : button_list) {
        if (p && p->IsSelected()) {
            p->Select(false);
        }
    }
    std::vector<wxPanel*>panel_list = { m_default_map_panel,m_manual_map_panel,m_auto_map_panel };
    for (auto p : panel_list) {
        if (p && p->IsShown()) {
            p->Hide();
        }
    }

    if (page == PageType::ptDefault) {
        if (m_default_btn && m_default_map_panel) {
            m_default_btn->Select(true);
            m_default_map_panel->Show();
        }
    }
    if (page == PageType::ptManual) {
        m_manual_btn->Select(true);
        m_manual_map_panel->Show();
    }
    if (page == PageType::ptAuto) {
        m_auto_btn->Select(true);
        m_auto_map_panel->Show();
    }

    // The nozzle-availability gate only constrains manual grouping; every other page must
    // leave OK usable even if the manual page had disabled it.
    if (page != PageType::ptManual && m_ok_btn && !m_ok_btn->IsEnabled())
        m_ok_btn->Enable();

    if (m_smart_filament)
        m_smart_filament->Show(get_mode() == fmmAutoForFlush);

    Layout();
    Fit();
}

void FilamentMapDialog::on_switch_mode(wxCommandEvent &event)
{
    int win_id  = event.GetId();
    m_page_type = PageType(win_id);

    update_panel_status(m_page_type);
    event.Skip();
}

void FilamentMapDialog::set_modal_btn_labels(const wxString &ok_label, const wxString &cancel_label)
{
    m_ok_btn->SetLabel(ok_label);
    m_cancel_btn->SetLabel(cancel_label);
}

}} // namespace Slic3r::GUI
