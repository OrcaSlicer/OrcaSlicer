#include "MixedFilamentDialog.hpp"

#include "GUI_App.hpp"
#include "GUI.hpp"
#include "GUI_ObjectList.hpp"
#include "MainFrame.hpp"
#include "Plater.hpp"
#include "Selection.hpp"
#include "wxExtensions.hpp"
#include "MsgDialog.hpp"

#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Model.hpp"

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/button.h>
#include <wx/listbox.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace Slic3r {
namespace GUI {

namespace {

std::string serialize_mix_row(const MixedFilament &mf)
{
    std::string row = std::to_string(mf.component_a) + "," + std::to_string(mf.component_b) + "," +
                      (mf.enabled ? "1" : "0") + "," + std::to_string(mf.ratio_a) + "," +
                      std::to_string(mf.ratio_b);
    const std::string pattern = MixedFilamentManager::normalize_manual_pattern(mf.manual_pattern);
    if (!pattern.empty())
        row += "," + pattern;
    if (std::abs(mf.component_a_surface_offset) > 1e-6)
        row += ",xa" + std::to_string(mf.component_a_surface_offset);
    if (std::abs(mf.component_b_surface_offset) > 1e-6)
        row += ",xb" + std::to_string(mf.component_b_surface_offset);
    if (mf.component_c != 0) {
        row += ",c" + std::to_string(mf.component_c);
        row += ",rc" + std::to_string(mf.ratio_c);
    }
    return row;
}

std::string serialize_enabled_rows(const std::vector<MixedFilament> &rows)
{
    std::string joined;
    for (const MixedFilament &mf : rows) {
        if (!mf.enabled)
            continue;
        if (!joined.empty())
            joined += ';';
        joined += serialize_mix_row(mf);
    }
    MixedFilamentManager parsed;
    parsed.load_definitions(joined);
    return parsed.serialize_definitions();
}

void assign_extruder(ModelConfig &config, int virtual_id)
{
    if (config.has("extruder"))
        config.set("extruder", virtual_id);
    else
        config.set_key_value("extruder", new ConfigOptionInt(virtual_id));
}

} // namespace

MixedFilamentDialog::MixedFilamentDialog(wxWindow *parent)
    : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe),
                wxID_ANY,
                _L("Mixed Filaments"),
                wxDefaultPosition,
                wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE)
{
    SetBackgroundColour(*wxWHITE);
    SetFont(Label::Body_14);

    auto *root = new wxBoxSizer(wxVERTICAL);

    auto *list_row = new wxBoxSizer(wxHORIZONTAL);
    m_list = new wxListBox(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(260), FromDIP(88)));
    auto *list_btns = new wxBoxSizer(wxVERTICAL);
    m_btn_add    = new wxButton(this, wxID_ANY, _L("Add"));
    m_btn_remove = new wxButton(this, wxID_ANY, _L("Remove"));
    list_btns->Add(m_btn_add, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    list_btns->Add(m_btn_remove, 0, wxEXPAND);
    list_row->Add(m_list, 1, wxEXPAND);
    list_row->Add(list_btns, 0, wxLEFT, FromDIP(8));
    root->Add(list_row, 0, wxEXPAND | wxALL, FromDIP(12));

    auto *grid = new wxFlexGridSizer(2, FromDIP(6), FromDIP(12));
    grid->AddGrowableCol(1, 1);

    auto add_row = [&](const wxString &label, wxWindow *ctrl) {
        auto *lbl = new wxStaticText(this, wxID_ANY, label);
        grid->Add(lbl, 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(ctrl, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    };

    m_spin_a = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(100), -1),
                              wxSP_ARROW_KEYS, 1, 16, 1);
    m_spin_b = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(100), -1),
                              wxSP_ARROW_KEYS, 1, 16, 2);
    m_spin_c = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(100), -1),
                              wxSP_ARROW_KEYS, 0, 16, 0);
    m_spin_ratio_a = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(100), -1),
                                    wxSP_ARROW_KEYS, 0, 99, 1);
    m_spin_ratio_b = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(100), -1),
                                    wxSP_ARROW_KEYS, 0, 99, 1);
    m_spin_ratio_c = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(100), -1),
                                    wxSP_ARROW_KEYS, 0, 99, 1);
    m_spin_c->SetToolTip(_L("Third physical (1-based). 0 = pair mix. Period ratio A+B+C must be at most 4."));
    m_spin_ratio_c->SetToolTip(_L("Layer count for C in the A then B then C cadence. Ignored when C is 0."));
    m_pattern = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(160), -1));
    m_pattern->SetToolTip(_L("Optional cycle pattern (1-based tools). Empty = ratio cadence. "
                             "Example: 112 → A,A,B when A=1,B=2 → T0,T0,T1."));

    m_offset_a = new wxTextCtrl(this, wxID_ANY, "0", wxDefaultPosition, wxSize(FromDIP(100), -1));
    m_offset_b = new wxTextCtrl(this, wxID_ANY, "0", wxDefaultPosition, wxSize(FromDIP(100), -1));
    m_offset_a->SetToolTip(_L("Component A surface offset (mm). Positive contracts. Applied only when Subdivide Mix Layer is off."));
    m_offset_b->SetToolTip(_L("Component B surface offset (mm). Positive contracts. Applied only when Subdivide Mix Layer is off."));

    m_enabled      = new wxCheckBox(this, wxID_ANY, _L("Enabled"));
    m_apply_object = new wxCheckBox(this, wxID_ANY, _L("Apply to selected volume"));
    m_enable_tower = new wxCheckBox(this, wxID_ANY, _L("Enable prime tower"));
    m_local_z      = new wxCheckBox(this, wxID_ANY, _L("Subdivide Mix Layer"));
    m_full_domain  = new wxCheckBox(this, wxID_ANY, _L("Full domain"));
    m_gradient     = new wxCheckBox(this, wxID_ANY, _L("Height gradient (100:0 bottom → 0:100 top)"));

    m_local_z->SetToolTip(_L("Split mixed layers into ratio-proportional sub-layers (e.g. 0.24 mm at 2:1 → 0.16 + 0.08)."));
    m_full_domain->SetToolTip(_L("Apply Subdivide Mix Layer to the whole object when its extruder is a virtual mix (no paint required)."));
    m_gradient->SetToolTip(_L("Requires Subdivide Mix Layer and Full domain. Interpolates pair ratio by object Z."));

    m_enabled->SetValue(true);
    m_apply_object->SetValue(true);
    m_enable_tower->SetValue(true);

    add_row(_L("Component A (1-based)"), m_spin_a);
    add_row(_L("Component B (1-based)"), m_spin_b);
    add_row(_L("Component C (0 = pair)"), m_spin_c);
    add_row(_L("Ratio A"), m_spin_ratio_a);
    add_row(_L("Ratio B"), m_spin_ratio_b);
    add_row(_L("Ratio C"), m_spin_ratio_c);
    add_row(_L("Pattern (optional)"), m_pattern);
    add_row(_L("Bias A (mm)"), m_offset_a);
    add_row(_L("Bias B (mm)"), m_offset_b);

    root->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    root->Add(m_enabled, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    root->Add(m_apply_object, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    root->Add(m_enable_tower, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    root->Add(m_local_z, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    root->Add(m_full_domain, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    root->Add(m_gradient, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

    auto *hint = new wxStaticText(this, wxID_ANY,
        _L("Creates virtual mixes (physical count + 1, +2, …) for each enabled row. "
           "Select a mix and apply it to the selected volume; sibling volumes are left unchanged. "
           "If only an object is selected, the object extruder is set and child volume extruders are kept. "
           "Subdivide Mix Layer + Full domain splits a 0.24 mm 2:1 mix into 0.16 + 0.08 mm. "
           "Component C 0 = pair; C ≥ 1 adds a third filament (period A+B+C ≤ 4). "
           "Save project and reopen to verify persistence."));
    hint->Wrap(FromDIP(360));
    root->Add(hint, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

    auto *btns = CreateStdDialogButtonSizer(wxOK | wxCANCEL);
    root->Add(btns, 0, wxEXPAND | wxALL, FromDIP(8));

    SetSizerAndFit(root);
    CentreOnParent();

    m_list->Bind(wxEVT_LISTBOX, &MixedFilamentDialog::on_list_select, this);
    m_btn_add->Bind(wxEVT_BUTTON, &MixedFilamentDialog::on_add_row, this);
    m_btn_remove->Bind(wxEVT_BUTTON, &MixedFilamentDialog::on_remove_row, this);
    m_enabled->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) {
        store_editors_into_selected_row();
        refresh_list_labels();
    });

    load_from_config();

    Bind(wxEVT_BUTTON, [this](wxCommandEvent &evt) {
        if (evt.GetId() == wxID_OK) {
            if (apply_to_project())
                EndModal(wxID_OK);
        } else {
            evt.Skip();
        }
    });

    wxGetApp().UpdateDlgDarkUI(this);
}

size_t MixedFilamentDialog::physical_filament_count() const
{
    if (PresetBundle *bundle = wxGetApp().preset_bundle)
        return bundle->filament_presets.size();
    return 0;
}

wxString MixedFilamentDialog::row_label(size_t idx, size_t num_physical) const
{
    if (idx >= m_rows.size())
        return {};
    const MixedFilament &mf  = m_rows[idx];
    const wxString       pair = (mf.component_c != 0)
        ? wxString::Format("%u+%u+%u  %d:%d:%d", mf.component_a, mf.component_b, mf.component_c,
                           mf.ratio_a, mf.ratio_b, mf.ratio_c)
        : wxString::Format("%u+%u  %d:%d", mf.component_a, mf.component_b, mf.ratio_a, mf.ratio_b);
    const int            vid  = virtual_id_for_row(idx, num_physical);
    if (vid > 0)
        return wxString::Format(_L("Mix %d  %s"), vid, pair);
    return wxString::Format(_L("(off)  %s"), pair);
}

int MixedFilamentDialog::virtual_id_for_row(size_t idx, size_t num_physical) const
{
    if (idx >= m_rows.size() || !m_rows[idx].enabled || num_physical == 0)
        return -1;
    int enabled_index = 0;
    for (size_t i = 0; i < idx; ++i) {
        if (m_rows[i].enabled)
            ++enabled_index;
    }
    return int(num_physical) + enabled_index + 1;
}

void MixedFilamentDialog::refresh_list()
{
    m_suppress_events = true;
    const int keep    = m_selected_row;
    m_list->Clear();
    const size_t num_physical = physical_filament_count();
    for (size_t i = 0; i < m_rows.size(); ++i)
        m_list->Append(row_label(i, num_physical));
    if (m_rows.empty()) {
        m_selected_row = -1;
    } else if (keep < 0) {
        m_selected_row = 0;
        m_list->SetSelection(m_selected_row);
    } else if (size_t(keep) >= m_rows.size()) {
        m_selected_row = int(m_rows.size()) - 1;
        m_list->SetSelection(m_selected_row);
    } else {
        m_selected_row = keep;
        m_list->SetSelection(m_selected_row);
    }
    m_suppress_events = false;
}

void MixedFilamentDialog::refresh_list_labels()
{
    const size_t     num_physical = physical_filament_count();
    const unsigned n              = m_list->GetCount();
    for (unsigned i = 0; i < n && size_t(i) < m_rows.size(); ++i)
        m_list->SetString(i, row_label(i, num_physical));
}

void MixedFilamentDialog::load_selected_row_into_editors()
{
    MixedFilament mf;
    if (m_selected_row >= 0 && size_t(m_selected_row) < m_rows.size())
        mf = m_rows[size_t(m_selected_row)];

    m_spin_a->SetValue(int(mf.component_a));
    m_spin_b->SetValue(int(mf.component_b));
    m_spin_c->SetValue(int(mf.component_c));
    m_spin_ratio_a->SetValue(mf.ratio_a);
    m_spin_ratio_b->SetValue(mf.ratio_b);
    m_spin_ratio_c->SetValue(mf.component_c != 0 ? mf.ratio_c : 1);
    m_enabled->SetValue(mf.enabled);
    m_pattern->SetValue(wxString::FromUTF8(mf.manual_pattern.c_str()));
    m_offset_a->SetValue(wxString::FromDouble(double(mf.component_a_surface_offset)));
    m_offset_b->SetValue(wxString::FromDouble(double(mf.component_b_surface_offset)));
}

void MixedFilamentDialog::store_editors_into_selected_row()
{
    if (m_selected_row < 0 || size_t(m_selected_row) >= m_rows.size())
        return;

    MixedFilament &mf = m_rows[size_t(m_selected_row)];
    mf.component_a    = unsigned(std::max(1, m_spin_a->GetValue()));
    mf.component_b    = unsigned(std::max(1, m_spin_b->GetValue()));
    mf.component_c    = unsigned(std::max(0, m_spin_c->GetValue()));
    mf.ratio_a        = std::max(0, m_spin_ratio_a->GetValue());
    mf.ratio_b        = std::max(0, m_spin_ratio_b->GetValue());
    mf.ratio_c        = std::max(0, m_spin_ratio_c->GetValue());
    if (mf.component_c != 0 && mf.ratio_c == 0)
        mf.ratio_c = 1;
    mf.enabled        = m_enabled->GetValue();
    mf.manual_pattern = MixedFilamentManager::normalize_manual_pattern(into_u8(m_pattern->GetValue()));
    double offset_a   = 0.0;
    double offset_b   = 0.0;
    m_offset_a->GetValue().ToDouble(&offset_a);
    m_offset_b->GetValue().ToDouble(&offset_b);
    mf.component_a_surface_offset = float(offset_a);
    mf.component_b_surface_offset = float(offset_b);
}

void MixedFilamentDialog::on_list_select(wxCommandEvent &)
{
    if (m_suppress_events)
        return;
    store_editors_into_selected_row();
    m_selected_row = m_list->GetSelection();
    load_selected_row_into_editors();
    // Labels may change if the previous row's A/B/enabled was edited.
    refresh_list_labels();
}

void MixedFilamentDialog::on_add_row(wxCommandEvent &)
{
    store_editors_into_selected_row();
    m_rows.emplace_back();
    m_selected_row = int(m_rows.size()) - 1;
    refresh_list();
    load_selected_row_into_editors();
}

void MixedFilamentDialog::on_remove_row(wxCommandEvent &)
{
    if (m_selected_row < 0 || size_t(m_selected_row) >= m_rows.size())
        return;
    m_rows.erase(m_rows.begin() + m_selected_row);
    if (m_rows.empty()) {
        m_selected_row = -1;
    } else if (size_t(m_selected_row) >= m_rows.size()) {
        m_selected_row = int(m_rows.size()) - 1;
    }
    refresh_list();
    load_selected_row_into_editors();
}

void MixedFilamentDialog::load_from_config()
{
    PresetBundle *bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return;

    std::string serialized;
    if (const ConfigOptionString *opt = bundle->project_config.option<ConfigOptionString>("mixed_filament_definitions"))
        serialized = opt->value;
    if (serialized.empty()) {
        DynamicPrintConfig &print_cfg = bundle->prints.get_edited_preset().config;
        if (const ConfigOptionString *opt = print_cfg.option<ConfigOptionString>("mixed_filament_definitions"))
            serialized = opt->value;
    }

    MixedFilamentManager mgr;
    mgr.load_definitions(serialized);
    m_rows = mgr.mixed_filaments();
    if (m_rows.empty())
        m_rows.emplace_back();
    m_selected_row = 0;
    refresh_list();
    load_selected_row_into_editors();

    const DynamicPrintConfig &print_cfg = bundle->prints.get_edited_preset().config;
    if (const ConfigOptionBool *ept = print_cfg.option<ConfigOptionBool>("enable_prime_tower"))
        m_enable_tower->SetValue(ept->value);
    if (const ConfigOptionBool *lz = print_cfg.option<ConfigOptionBool>("dithering_local_z_mode"))
        m_local_z->SetValue(lz->value);
    if (const ConfigOptionBool *fd = print_cfg.option<ConfigOptionBool>("dithering_local_z_whole_objects"))
        m_full_domain->SetValue(fd->value);
    if (const ConfigOptionBool *gr = print_cfg.option<ConfigOptionBool>("mixed_filament_gradient_mode"))
        m_gradient->SetValue(gr->value);
}

bool MixedFilamentDialog::apply_to_project()
{
    PresetBundle *bundle = wxGetApp().preset_bundle;
    Plater       *plater = wxGetApp().plater();
    if (bundle == nullptr || plater == nullptr)
        return false;

    store_editors_into_selected_row();
    for (const MixedFilament &mf : m_rows) {
        if (!mf.enabled || mf.component_c == 0)
            continue;
        if (mf.component_c == mf.component_a || mf.component_c == mf.component_b) {
            MessageDialog(this,
                _L("Component C must be different from A and B."),
                _L("Mixed Filaments"), wxOK | wxICON_WARNING).ShowModal();
            return false;
        }
        if (mf.ratio_a + mf.ratio_b + mf.ratio_c > 4) {
            MessageDialog(this,
                _L("Three-component mix period (ratio A+B+C) must be at most 4."),
                _L("Mixed Filaments"), wxOK | wxICON_WARNING).ShowModal();
            return false;
        }
        if (MixedFilamentManager::normalize_manual_pattern(mf.manual_pattern).size() > 4) {
            MessageDialog(this,
                _L("Three-component mix pattern length must be at most 4."),
                _L("Mixed Filaments"), wxOK | wxICON_WARNING).ShowModal();
            return false;
        }
    }
    const std::string serialized = serialize_enabled_rows(m_rows);

    std::string old_serialized;
    if (const ConfigOptionString *opt = bundle->project_config.option<ConfigOptionString>("mixed_filament_definitions"))
        old_serialized = opt->value;
    if (old_serialized.empty()) {
        if (const ConfigOptionString *opt = bundle->prints.get_edited_preset().config.option<ConfigOptionString>("mixed_filament_definitions"))
            old_serialized = opt->value;
    }
    const size_t num_physical = bundle->filament_presets.size();
    std::vector<int> painted_ids;
    for (const ModelObject *obj : plater->model().objects) {
        for (const ModelVolume *vol : obj->volumes) {
            const std::vector<int> ids = vol->get_extruders();
            painted_ids.insert(painted_ids.end(), ids.begin(), ids.end());
        }
    }
    if (mixed_filament_painted_ids_would_shift(old_serialized, serialized, num_physical, painted_ids)) {
        MessageDialog(this,
            _L("Disabling a mix in the middle of the list changes Mix IDs. Painted regions using later mix IDs will follow the new enabled-row order."),
            _L("Mixed Filaments"), wxOK | wxICON_WARNING).ShowModal();
    }

    // Persist on both print preset and project_config (belt and suspenders).
    DynamicPrintConfig &print_cfg = bundle->prints.get_edited_preset().config;
    print_cfg.set_key_value("mixed_filament_definitions", new ConfigOptionString(serialized));
    bundle->project_config.set_key_value("mixed_filament_definitions", new ConfigOptionString(serialized));

    if (m_enable_tower->GetValue())
        print_cfg.set_key_value("enable_prime_tower", new ConfigOptionBool(true));
    print_cfg.set_key_value("dithering_local_z_mode", new ConfigOptionBool(m_local_z->GetValue()));
    print_cfg.set_key_value("dithering_local_z_whole_objects", new ConfigOptionBool(m_full_domain->GetValue()));
    print_cfg.set_key_value("mixed_filament_gradient_mode", new ConfigOptionBool(m_gradient->GetValue()));

    const int virtual_id = (m_selected_row >= 0) ? virtual_id_for_row(size_t(m_selected_row), num_physical) : -1;

    if (m_apply_object->GetValue() && virtual_id > 0 && !plater->model().objects.empty()) {
        ModelVolume *volume = nullptr;
        if (ObjectList *list = wxGetApp().obj_list())
            volume = list->get_selected_model_volume();
        const Selection &sel = plater->get_selection();
        if (volume == nullptr) {
            int obj_idx = -1;
            int vol_idx = -1;
            volume = sel.get_selected_single_volume(obj_idx, vol_idx);
        }

        plater->take_snapshot(std::string("Mixed filament assign"));
        if (volume != nullptr) {
            // Palette path: assign only the selected volume. Do not touch siblings.
            assign_extruder(volume->config, virtual_id);
        } else {
            ModelObject *target = nullptr;
            const int    obj_idx = sel.get_object_idx();
            if (obj_idx >= 0 && size_t(obj_idx) < plater->model().objects.size())
                target = plater->model().objects[size_t(obj_idx)];
            // Persist mix rows even when nothing is selected. Do not assign objects.front().
            if (target != nullptr)
                assign_extruder(target->config, virtual_id);
        }
    }

    plater->on_config_change(bundle->full_config());
    plater->schedule_background_process();
    return true;
}

} // namespace GUI
} // namespace Slic3r
