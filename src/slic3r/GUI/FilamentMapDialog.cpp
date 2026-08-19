#include "FilamentMapDialog.hpp"
#include "PartPlate.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "I18N.hpp"
#include "GUI_App.hpp"
#include "CapsuleButton.hpp"
#include "MsgDialog.hpp"

#include <wx/choice.h>
#include <wx/scrolwin.h>

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

extern std::string& get_left_extruder_unprintable_text();
extern std::string& get_right_extruder_unprintable_text();

namespace {

class CoExtrusionColorMappingDialog : public wxDialog
{
public:
    CoExtrusionColorMappingDialog(wxWindow *parent,
                                  const std::vector<std::string> &logical_colors,
                                  const std::vector<std::string> &sector_colors,
                                  const std::vector<int> &mapping)
        : wxDialog(parent, wxID_ANY, _L("Co-extrusion color mapping"), wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    {
        SetBackgroundColour(*wxWHITE);
        SetMinSize(wxSize(FromDIP(560), FromDIP(360)));

        auto *main_sizer = new wxBoxSizer(wxVERTICAL);
        auto *description = new wxStaticText(this, wxID_ANY,
            _L("Assign each color in the 3MF model to a physical color sector in the selected co-extrusion filament. Auto uses the closest configured sector color."));
        description->Wrap(FromDIP(520));
        main_sizer->Add(description, 0, wxEXPAND | wxALL, FromDIP(16));

        auto *scroller = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
        scroller->SetScrollRate(0, FromDIP(12));
        scroller->SetBackgroundColour(*wxWHITE);
        auto *rows = new wxBoxSizer(wxVERTICAL);

        for (size_t logical = 0; logical < logical_colors.size(); ++logical) {
            auto *row = new wxBoxSizer(wxHORIZONTAL);
            auto *swatch = new wxPanel(scroller, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(28), FromDIP(28)), wxBORDER_SIMPLE);
            const wxColour color(from_u8(logical_colors[logical]));
            if (color.IsOk())
                swatch->SetBackgroundColour(color);
            row->Add(swatch, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));

            auto *label = new wxStaticText(scroller, wxID_ANY,
                wxString::Format(_L("3MF color %d: %s"), int(logical + 1), from_u8(logical_colors[logical])));
            row->Add(label, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));

            auto *choice = new wxChoice(scroller, wxID_ANY);
            choice->Append(_L("Auto (closest color)"));
            for (size_t sector = 0; sector < sector_colors.size(); ++sector)
                choice->Append(wxString::Format(_L("Sector %d: %s"), int(sector + 1), from_u8(sector_colors[sector])));
            const int selected = logical < mapping.size() && mapping[logical] >= 0 &&
                                 size_t(mapping[logical]) <= sector_colors.size() ? mapping[logical] : 0;
            choice->SetSelection(selected);
            m_choices.push_back(choice);
            row->Add(choice, 0, wxALIGN_CENTER_VERTICAL);

            rows->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
        }

        scroller->SetSizer(rows);
        main_sizer->Add(scroller, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));

        auto *bottom = new wxPanel(this);
        bottom->SetBackgroundColour(*wxWHITE);
        auto *bottom_sizer = new wxBoxSizer(wxHORIZONTAL);
        auto *buttons = new DialogButtons(bottom, {"OK", "Cancel"});
        bottom_sizer->AddStretchSpacer();
        bottom_sizer->Add(buttons, 0, wxALL, FromDIP(12));
        bottom->SetSizer(bottom_sizer);
        main_sizer->Add(bottom, 0, wxEXPAND);

        buttons->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_OK); });
        buttons->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });
        SetEscapeId(wxID_CANCEL);
        SetSizer(main_sizer);
        CenterOnParent();
        wxGetApp().UpdateDlgDarkUI(this);
    }

    std::vector<int> mapping() const
    {
        std::vector<int> result;
        result.reserve(m_choices.size());
        for (const wxChoice *choice : m_choices)
            result.push_back(choice->GetSelection());
        return result;
    }

private:
    std::vector<wxChoice *> m_choices;
};

} // namespace

bool edit_coextrusion_color_mapping(wxWindow *parent, bool force)
{
    PresetBundle *preset_bundle = wxGetApp().preset_bundle;
    Plater *plater = wxGetApp().plater();
    if (preset_bundle == nullptr || plater == nullptr)
        return true;

    DynamicPrintConfig full_config = preset_bundle->full_config();
    const auto *machine_enabled = full_config.option<ConfigOptionBool>("coextrusion_c_axis_enable");
    const auto *filament_enabled = full_config.option<ConfigOptionBool>("filament_coextrusion_enable");
    const DynamicPrintConfig &edited_filament_config = preset_bundle->filaments.get_edited_preset().config;
    const auto *edited_filament_enabled = edited_filament_config.option<ConfigOptionBool>("filament_coextrusion_enable");
    const bool use_edited_filament_config = force && edited_filament_enabled != nullptr && edited_filament_enabled->value;
    const bool use_filament_config = use_edited_filament_config || (filament_enabled != nullptr && filament_enabled->value);
    // The settings-page button may prepare a project mapping before the
    // printer capability is enabled. Automatic pre-slice prompting still
    // requires an enabled C axis.
    if ((machine_enabled == nullptr || !machine_enabled->value) && !(force && use_filament_config))
        return true;
    const auto *sector_colors = use_edited_filament_config ?
        edited_filament_config.option<ConfigOptionStrings>("filament_coextrusion_colors") :
        full_config.option<ConfigOptionStrings>(use_filament_config ? "filament_coextrusion_colors" : "coextrusion_c_axis_colors");
    const auto *source_colors = full_config.option<ConfigOptionStrings>("coextrusion_source_colors");
    const auto *filament_colors = full_config.option<ConfigOptionStrings>("filament_colour");
    const ConfigOptionStrings *logical_colors = source_colors != nullptr && !source_colors->values.empty() ? source_colors : filament_colors;
    auto *mapping_option = preset_bundle->project_config.option<ConfigOptionInts>("coextrusion_color_mapping", true);
    if (sector_colors == nullptr || sector_colors->values.empty() || logical_colors == nullptr)
        return true;

    if (!force && mapping_option->values.size() == logical_colors->values.size())
        return true;

    if (logical_colors->values.size() <= 1 && !force) {
        mapping_option->values.assign(logical_colors->values.size(), 0);
        return true;
    }

    CoExtrusionColorMappingDialog dialog(parent, logical_colors->values, sector_colors->values, mapping_option->values);
    if (dialog.ShowModal() != wxID_OK)
        return false;

    mapping_option->values = dialog.mapping();
    plater->update_project_dirty_from_presets();
    plater->on_config_change(preset_bundle->full_config());
    plater->update();
    return true;
}


bool try_pop_up_before_slice(bool is_slice_all, Plater* plater_ref, PartPlate* partplate_ref, bool force_pop_up)
{
    if (!edit_coextrusion_color_mapping(plater_ref, false))
        return false;

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
    applied_maps.resize(filament_colors.size(), 1);

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
        if (sync_plate) {
            if (is_slice_all) {
                auto plate_list = plater_ref->get_partplate_list().get_plate_list();
                for (int i = 0; i < plate_list.size(); ++i) {
                    plate_list[i]->set_filament_map_mode(new_mode);
                    if(new_mode == fmmManual)
                        plate_list[i]->set_filament_maps(new_maps);
                }
            }
            else {
                partplate_ref->set_filament_map_mode(new_mode);
                if (new_mode == fmmManual)
                    partplate_ref->set_filament_maps(new_maps);
            }
        }
        else {
            plater_ref->set_global_filament_map_mode(new_mode);
            if (new_mode == fmmManual)
                plater_ref->set_global_filament_map(new_maps);
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
                                     const std::vector<int>         &filaments,
                                     const FilamentMapMode           mode,
                                     bool                            machine_synced,
                                     bool                            show_default,
                                     bool                            with_checkbox)
    : wxDialog(parent, wxID_ANY, _L("Filament grouping"), wxDefaultPosition, wxDefaultSize,wxDEFAULT_DIALOG_STYLE), m_filament_color(filament_color), m_filament_type(filament_type), m_filament_map(filament_map)
{
    SetBackgroundColour(*wxWHITE);

    SetMinSize(wxSize(FromDIP(580), -1));
    SetMaxSize(wxSize(FromDIP(580), -1));

    if (mode < fmmManual)
        m_page_type = PageType::ptAuto;
    else if (mode == fmmManual)
        m_page_type = PageType::ptManual;
    else
        m_page_type = PageType::ptDefault;

    wxBoxSizer *main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->AddSpacer(FromDIP(22));

    wxBoxSizer *mode_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_auto_btn   = new CapsuleButton(this, PageType::ptAuto, _L("Auto"), false);
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

    FilamentMapMode default_auto_mode = mode >= fmmManual ? fmmAutoForFlush :
        mode == fmmAutoForMatch && !machine_synced ? fmmAutoForFlush :
        mode;

    m_manual_map_panel                = new FilamentMapManualPanel(this, m_filament_color, m_filament_type, filaments, filament_map);
    m_auto_map_panel                  = new FilamentMapAutoPanel(this, default_auto_mode, machine_synced);
    if (show_default)
        m_default_map_panel = new FilamentMapDefaultPanel(this);
    else
        m_default_map_panel = nullptr;

    panel_sizer->Add(m_manual_map_panel, 0, wxEXPAND);
    panel_sizer->Add(m_auto_map_panel, 0, wxEXPAND);
    if (show_default) panel_sizer->Add(m_default_map_panel, 0, wxEXPAND);
    main_sizer->Add(panel_sizer, 0, wxEXPAND);

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
        std::vector<int> left_filaments  = m_manual_map_panel->GetLeftFilaments();
        std::vector<int> right_filaments = m_manual_map_panel->GetRightFilaments();

        for (int i = 0; i < m_filament_map.size(); ++i) {
            if (std::find(left_filaments.begin(), left_filaments.end(), i + 1) != left_filaments.end()) {
                m_filament_map[i] = 1;
            } else if (std::find(right_filaments.begin(), right_filaments.end(), i + 1) != right_filaments.end()) {
                m_filament_map[i] = 2;
            }
        }
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
