#include "MixedFilamentDialog.hpp"

#include "GUI_App.hpp"
#include "GUI.hpp"
#include "MainFrame.hpp"
#include "Plater.hpp"
#include "Selection.hpp"
#include "wxExtensions.hpp"

#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Model.hpp"

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/button.h>

#include <algorithm>
#include <string>

namespace Slic3r {
namespace GUI {

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
    m_spin_ratio_a = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(100), -1),
                                    wxSP_ARROW_KEYS, 0, 99, 1);
    m_spin_ratio_b = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(100), -1),
                                    wxSP_ARROW_KEYS, 0, 99, 1);
    m_pattern = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(160), -1));
    m_pattern->SetToolTip(_L("Optional cycle pattern (1-based tools). Empty = ratio cadence. "
                             "Example: 112 → A,A,B when A=1,B=2 → T0,T0,T1."));

    m_enabled      = new wxCheckBox(this, wxID_ANY, _L("Enabled"));
    m_apply_object = new wxCheckBox(this, wxID_ANY, _L("Apply to selected object"));
    m_enable_tower = new wxCheckBox(this, wxID_ANY, _L("Enable prime tower"));

    m_enabled->SetValue(true);
    m_apply_object->SetValue(true);
    m_enable_tower->SetValue(true);

    add_row(_L("Component A (1-based)"), m_spin_a);
    add_row(_L("Component B (1-based)"), m_spin_b);
    add_row(_L("Ratio A"), m_spin_ratio_a);
    add_row(_L("Ratio B"), m_spin_ratio_b);
    add_row(_L("Pattern (optional)"), m_pattern);

    root->Add(grid, 0, wxEXPAND | wxALL, FromDIP(12));
    root->Add(m_enabled, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    root->Add(m_apply_object, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    root->Add(m_enable_tower, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

    auto *hint = new wxStaticText(this, wxID_ANY,
        _L("Creates one virtual mix (physical count + 1). Assign that filament ID to the object. "
           "Save project and reopen to verify persistence."));
    hint->Wrap(FromDIP(360));
    root->Add(hint, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

    auto *btns = CreateStdDialogButtonSizer(wxOK | wxCANCEL);
    root->Add(btns, 0, wxEXPAND | wxALL, FromDIP(8));

    SetSizer(root);
    root->Fit(this);
    CentreOnParent();

    load_from_config();

    Bind(wxEVT_BUTTON, [this](wxCommandEvent &evt) {
        if (evt.GetId() == wxID_OK) {
            apply_to_project();
            EndModal(wxID_OK);
        } else {
            evt.Skip();
        }
    });

    wxGetApp().UpdateDlgDarkUI(this);
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
    if (mgr.enabled_count() == 0)
        return;

    const MixedFilament &mf = mgr.mixed_filaments().front();
    m_spin_a->SetValue(int(mf.component_a));
    m_spin_b->SetValue(int(mf.component_b));
    m_spin_ratio_a->SetValue(mf.ratio_a);
    m_spin_ratio_b->SetValue(mf.ratio_b);
    m_enabled->SetValue(mf.enabled);
    if (!mf.manual_pattern.empty())
        m_pattern->SetValue(wxString::FromUTF8(mf.manual_pattern.c_str()));

    if (const ConfigOptionBool *ept = bundle->prints.get_edited_preset().config.option<ConfigOptionBool>("enable_prime_tower"))
        m_enable_tower->SetValue(ept->value);
}

void MixedFilamentDialog::apply_to_project()
{
    PresetBundle *bundle = wxGetApp().preset_bundle;
    Plater       *plater = wxGetApp().plater();
    if (bundle == nullptr || plater == nullptr)
        return;

    const unsigned int component_a = unsigned(std::max(1, m_spin_a->GetValue()));
    const unsigned int component_b = unsigned(std::max(1, m_spin_b->GetValue()));
    const int          ratio_a     = std::max(0, m_spin_ratio_a->GetValue());
    const int          ratio_b     = std::max(0, m_spin_ratio_b->GetValue());
    const bool         enabled     = m_enabled->GetValue();
    const std::string  pattern     = MixedFilamentManager::normalize_manual_pattern(into_u8(m_pattern->GetValue()));

    std::string serialized;
    if (enabled) {
        std::string row = std::to_string(component_a) + "," + std::to_string(component_b) + ",1," +
                          std::to_string(ratio_a) + "," + std::to_string(ratio_b);
        if (!pattern.empty())
            row += "," + pattern;
        MixedFilamentManager holder;
        holder.load_definitions(row);
        serialized = holder.serialize_definitions();
    }

    // Persist on both print preset and project_config (belt and suspenders).
    DynamicPrintConfig &print_cfg = bundle->prints.get_edited_preset().config;
    print_cfg.set_key_value("mixed_filament_definitions", new ConfigOptionString(serialized));
    bundle->project_config.set_key_value("mixed_filament_definitions", new ConfigOptionString(serialized));

    if (m_enable_tower->GetValue())
        print_cfg.set_key_value("enable_prime_tower", new ConfigOptionBool(true));

    // Virtual ID = physical filament count + 1 for the first enabled mix.
    const size_t num_physical = bundle->filament_presets.size();
    const int    virtual_id   = int(num_physical) + 1;

    if (m_apply_object->GetValue() && enabled && !plater->model().objects.empty()) {
        ModelObject *target = nullptr;
        const Selection &sel = plater->get_selection();
        const int obj_idx = sel.get_object_idx();
        if (obj_idx >= 0 && size_t(obj_idx) < plater->model().objects.size())
            target = plater->model().objects[size_t(obj_idx)];
        if (target == nullptr)
            target = plater->model().objects.front();

        plater->take_snapshot(std::string("Mixed filament assign"));
        target->config.set_key_value("extruder", new ConfigOptionInt(virtual_id));
        // Clear per-volume extruder so object-level mix applies whole-object.
        for (ModelVolume *mv : target->volumes) {
            if (mv)
                mv->config.erase("extruder");
        }
    }

    plater->on_config_change(bundle->full_config());
    plater->schedule_background_process();
}

} // namespace GUI
} // namespace Slic3r
