#include "MixedFilamentDialog.hpp"

#include "GUI_App.hpp"
#include "GUI.hpp"
#include "GUI_ObjectList.hpp"
#include "MainFrame.hpp"
#include "Plater.hpp"
#include "Selection.hpp"
#include "wxExtensions.hpp"
#include "MsgDialog.hpp"

#include "libslic3r/Color.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/MixedFilamentCookbook.hpp"
#include "libslic3r/MixedFilamentMatch.hpp"
#include "libslic3r/MixedFilamentSwatch.hpp"
#include "libslic3r/MixedFilamentPaintBake.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Model.hpp"

#include <wx/clrpicker.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/stattext.h>
#include <wx/button.h>
#include <wx/listbox.h>
#include <wx/notebook.h>
#include <wx/panel.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>

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
    if (mf.gradient_enabled)
        row += ",g";
    if (mf.perimeter_modulation)
        row += ",p";
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

MixedFilamentDialog::MixedFilamentDialog(wxWindow *parent, int initial_row)
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
    m_list = new wxListBox(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(260), FromDIP(180)));
    auto *list_btns = new wxBoxSizer(wxVERTICAL);
    m_btn_add          = new wxButton(this, wxID_ANY, _L("Add"));
    m_btn_remove       = new wxButton(this, wxID_ANY, _L("Remove"));
    m_btn_recommended  = new wxButton(this, wxID_ANY, _L("Add recommended"));
    m_btn_recommended->SetToolTip(
        wxString::Format(_L("Adds missing 1:1 pairs of slots 1–4, then period-4 cookbook extras. "
                            "Skips duplicates. Stops at enabled-mix cap %d. Does not change existing rows. "
                            "PeggyPalette-class is volume Mix N; paint Map still 16 dest IDs."),
                         int(SPECTRUM_MIX_ENABLED_CAP)));
    list_btns->Add(m_btn_add, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    list_btns->Add(m_btn_remove, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    list_btns->Add(m_btn_recommended, 0, wxEXPAND);
    list_row->Add(m_list, 1, wxEXPAND);
    list_row->Add(list_btns, 0, wxLEFT, FromDIP(8));
    root->Add(list_row, 0, wxEXPAND | wxALL, FromDIP(12));

    // Stock wxNotebook is acceptable (Orca DarkMode prefers a custom notebook; do not block on it).
    m_notebook = new wxNotebook(this, wxID_ANY);
    auto *page_ratio = new wxPanel(m_notebook);
    auto *page_cycle = new wxPanel(m_notebook);
    auto *page_match = new wxPanel(m_notebook);
    auto *page_grad  = new wxPanel(m_notebook);
    page_ratio->SetBackgroundColour(*wxWHITE);
    page_cycle->SetBackgroundColour(*wxWHITE);
    page_match->SetBackgroundColour(*wxWHITE);
    page_grad->SetBackgroundColour(*wxWHITE);

    auto *grid = new wxFlexGridSizer(2, FromDIP(6), FromDIP(12));
    grid->AddGrowableCol(1, 1);
    auto add_row = [&](wxWindow *parent, wxSizer *sizer, const wxString &label, wxWindow *ctrl) {
        auto *lbl = new wxStaticText(parent, wxID_ANY, label);
        sizer->Add(lbl, 0, wxALIGN_CENTER_VERTICAL);
        sizer->Add(ctrl, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    };

    m_spin_a = new wxSpinCtrl(page_ratio, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(100), -1),
                              wxSP_ARROW_KEYS, 1, 16, 1);
    m_spin_b = new wxSpinCtrl(page_ratio, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(100), -1),
                              wxSP_ARROW_KEYS, 1, 16, 2);
    m_spin_c = new wxSpinCtrl(page_ratio, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(100), -1),
                              wxSP_ARROW_KEYS, 0, 16, 0);
    m_spin_ratio_a = new wxSpinCtrl(page_ratio, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(100), -1),
                                    wxSP_ARROW_KEYS, 0, 99, 1);
    m_spin_ratio_b = new wxSpinCtrl(page_ratio, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(100), -1),
                                    wxSP_ARROW_KEYS, 0, 99, 1);
    m_spin_ratio_c = new wxSpinCtrl(page_ratio, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(100), -1),
                                    wxSP_ARROW_KEYS, 0, 99, 1);
    m_spin_c->SetToolTip(_L("Third physical (1-based). 0 = pair mix. Period ratio A+B+C must be at most 4."));
    m_spin_ratio_c->SetToolTip(_L("Layer count for C in the A then B then C cadence. Ignored when C is 0."));
    m_offset_a = new wxTextCtrl(page_ratio, wxID_ANY, "0", wxDefaultPosition, wxSize(FromDIP(100), -1));
    m_offset_b = new wxTextCtrl(page_ratio, wxID_ANY, "0", wxDefaultPosition, wxSize(FromDIP(100), -1));
    m_offset_a->SetToolTip(_L("Component A surface offset (mm). Positive contracts. Applied only when Subdivide Mix Layer is off."));
    m_offset_b->SetToolTip(_L("Component B surface offset (mm). Positive contracts. Applied only when Subdivide Mix Layer is off."));
    add_row(page_ratio, grid, _L("Component A (1-based)"), m_spin_a);
    add_row(page_ratio, grid, _L("Component B (1-based)"), m_spin_b);
    add_row(page_ratio, grid, _L("Component C (0 = pair)"), m_spin_c);
    add_row(page_ratio, grid, _L("Ratio A"), m_spin_ratio_a);
    add_row(page_ratio, grid, _L("Ratio B"), m_spin_ratio_b);
    add_row(page_ratio, grid, _L("Ratio C"), m_spin_ratio_c);
    add_row(page_ratio, grid, _L("Bias A (mm)"), m_offset_a);
    add_row(page_ratio, grid, _L("Bias B (mm)"), m_offset_b);
    auto *ratio_sizer = new wxBoxSizer(wxVERTICAL);
    ratio_sizer->Add(grid, 0, wxEXPAND | wxALL, FromDIP(12));
    page_ratio->SetSizer(ratio_sizer);

    m_pattern = new wxTextCtrl(page_cycle, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(160), -1));
    m_pattern->SetToolTip(_L("Optional cycle pattern (1-based tools). Empty = ratio cadence. "
                             "Example: 112 → A,A,B when A=1,B=2 → T0,T0,T1."));
    auto *cycle_sizer = new wxBoxSizer(wxVERTICAL);
    cycle_sizer->Add(new wxStaticText(page_cycle, wxID_ANY, _L("Pattern (optional)")), 0, wxBOTTOM, FromDIP(6));
    cycle_sizer->Add(m_pattern, 0, wxEXPAND);
    auto *cycle_pad = new wxBoxSizer(wxVERTICAL);
    cycle_pad->Add(cycle_sizer, 0, wxEXPAND | wxALL, FromDIP(12));
    page_cycle->SetSizer(cycle_pad);

    auto *match_row = new wxBoxSizer(wxHORIZONTAL);
    match_row->Add(new wxStaticText(page_match, wxID_ANY, _L("Target color")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT,
                   FromDIP(8));
    m_clr_target = new wxColourPickerCtrl(page_match, wxID_ANY, wxColour(255, 255, 255));
    m_hex_target = new wxTextCtrl(page_match, wxID_ANY, "#FFFFFF", wxDefaultPosition, wxSize(FromDIP(100), -1),
                                  wxTE_PROCESS_ENTER);
    m_btn_create_mix = new wxButton(page_match, wxID_ANY, _L("Create mix from color"));
    match_row->Add(m_clr_target, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    match_row->Add(m_hex_target, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    match_row->Add(m_btn_create_mix, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    match_row->Add(new wxStaticText(page_match, wxID_ANY, _L("Predicted mix")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT,
                   FromDIP(8));
    m_clr_predicted = new wxColourPickerCtrl(page_match, wxID_ANY, wxColour(255, 255, 255));
    m_clr_predicted->Enable(false);
    m_clr_predicted->SetToolTip(_L("Predicted printable mix colour."));
    match_row->Add(m_clr_predicted, 0, wxALIGN_CENTER_VERTICAL);
    m_candidate_list = new wxListBox(page_match, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(360), FromDIP(110)));
    auto *min_row = new wxBoxSizer(wxHORIZONTAL);
    m_min_mix_slider = new wxSlider(page_match, wxID_ANY, 25, 25, 50, wxDefaultPosition, wxSize(FromDIP(180), -1));
    m_min_mix_label  = new wxStaticText(page_match, wxID_ANY, _L("Min mix: 25%"));
    m_min_mix_slider->SetToolTip(
        _L("Period-4 floor is 25%; Snapmaker 15% not available; useful stops 25 / 33 / 34+."));
    min_row->Add(m_min_mix_slider, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    min_row->Add(m_min_mix_label, 0, wxALIGN_CENTER_VERTICAL);
    auto *match_sizer = new wxBoxSizer(wxVERTICAL);
    match_sizer->Add(match_row, 0, wxEXPAND | wxALL, FromDIP(12));
    match_sizer->Add(m_candidate_list, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    match_sizer->Add(min_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    m_lut_status = new wxStaticText(page_match, wxID_ANY, wxEmptyString);
    match_sizer->Add(m_lut_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    page_match->SetSizer(match_sizer);

    m_gradient = new wxCheckBox(page_grad, wxID_ANY, _L("Height gradient (A bottom → B top)"));
    m_gradient->SetToolTip(
        _L("Requires Subdivide Mix Layer and Full domain. Applies to this mix only; siblings stay ratio/pattern. "
           "Prefer layer height 0.24 mm (0.16–0.28 mm band)."));
    auto *grad_sizer = new wxBoxSizer(wxVERTICAL);
    grad_sizer->Add(m_gradient, 0, wxALL, FromDIP(12));
    page_grad->SetSizer(grad_sizer);

    m_notebook->AddPage(page_ratio, _L("Ratio"));
    m_notebook->AddPage(page_cycle, _L("Cycle"));
    m_notebook->AddPage(page_match, _L("Match"));
    m_notebook->AddPage(page_grad, _L("Gradient"));
    root->Add(m_notebook, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

    m_enabled      = new wxCheckBox(this, wxID_ANY, _L("Enabled"));
    m_apply_object = new wxCheckBox(this, wxID_ANY, _L("Apply to selected volume"));
    m_enable_tower = new wxCheckBox(this, wxID_ANY, _L("Enable prime tower"));
    m_local_z      = new wxCheckBox(this, wxID_ANY, _L("Subdivide Mix Layer"));
    m_full_domain  = new wxCheckBox(this, wxID_ANY, _L("Full domain"));
    m_perimeter    = new wxCheckBox(this, wxID_ANY, _L("Outer-wall dither"));

    m_local_z->SetToolTip(_L("Split mixed layers into ratio-proportional sub-layers (e.g. 0.24 mm at 2:1 → 0.16 + 0.08)."));
    m_full_domain->SetToolTip(_L("Apply Subdivide Mix Layer to the whole object when its extruder is a virtual mix (no paint required)."));
    m_perimeter->SetToolTip(
        _L("Expands A / recesses B by ~0.4×nozzle (max 0.35 mm). Not line-width. "
           "Ignored when Subdivide Mix Layer is on. Typed Bias A/B mm override."));

    m_enabled->SetValue(true);
    m_apply_object->SetValue(true);
    m_enable_tower->SetValue(true);

    root->Add(m_enabled, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    root->Add(m_apply_object, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    root->Add(m_enable_tower, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    root->Add(m_local_z, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    root->Add(m_full_domain, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    root->Add(m_perimeter, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

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
    m_btn_recommended->Bind(wxEVT_BUTTON, &MixedFilamentDialog::on_add_recommended, this);
    m_clr_target->Bind(wxEVT_COLOURPICKER_CHANGED, &MixedFilamentDialog::on_target_colour_changed, this);
    m_btn_create_mix->Bind(wxEVT_BUTTON, &MixedFilamentDialog::on_create_mix_from_color, this);
    m_candidate_list->Bind(wxEVT_LISTBOX, &MixedFilamentDialog::on_candidate_select, this);
    m_candidate_list->Bind(wxEVT_KEY_UP, [this](wxKeyEvent &evt) {
        wxCommandEvent dummy;
        on_candidate_select(dummy);
        evt.Skip();
    });
    m_min_mix_slider->Bind(wxEVT_SLIDER, &MixedFilamentDialog::on_min_mix_slider, this);
    m_hex_target->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent &) { on_hex_target_changed(); });
    m_hex_target->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent &evt) {
        on_hex_target_changed();
        evt.Skip();
    });
    m_enabled->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) {
        if (m_suppress_events)
            return;
        if (m_selected_row >= 0 && size_t(m_selected_row) < m_rows.size()) {
            const bool currently_on = m_rows[size_t(m_selected_row)].enabled;
            if (!currently_on && m_enabled->GetValue()) {
                if (!spectrum_mix_enabled_fits(enabled_mix_count())) {
                    m_suppress_events = true;
                    m_enabled->SetValue(false);
                    m_suppress_events = false;
                    MessageDialog(this,
                                  wxString::Format(_L("Enabled mixes cannot exceed %d."),
                                                   int(SPECTRUM_MIX_ENABLED_CAP)),
                                  _L("Mixed Filaments"), wxOK | wxICON_WARNING)
                        .ShowModal();
                    return;
                }
            }
        }
        store_editors_into_selected_row();
        refresh_list_labels();
    });
    m_perimeter->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) {
        store_editors_into_selected_row();
        refresh_list_labels();
    });
    m_gradient->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) {
        if (m_suppress_events)
            return;
        const bool on = m_gradient->GetValue();
        if (on) {
            m_spin_c->SetValue(0);
            m_pattern->SetValue(wxEmptyString);
            if (m_selected_row >= 0 && size_t(m_selected_row) < m_rows.size()) {
                MixedFilament &mf = m_rows[size_t(m_selected_row)];
                mf.component_c      = 0;
                mf.manual_pattern.clear();
                mf.gradient_enabled = true;
            }
        } else if (m_selected_row >= 0 && size_t(m_selected_row) < m_rows.size()) {
            m_rows[size_t(m_selected_row)].gradient_enabled = false;
        }
        store_editors_into_selected_row();
        refresh_list_labels();
    });

    load_from_config();
    if (initial_row >= 0)
        select_row(initial_row);
    refresh_candidates();

    m_notebook->Bind(wxEVT_NOTEBOOK_PAGE_CHANGED, [this](wxBookCtrlEvent &) {
        if (m_suppress_events)
            return;
        store_editors_into_selected_row();
        // Ratio store already cleared leftover Cycle pattern; skip load (would bounce Gradient mixes).
    });

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

size_t MixedFilamentDialog::enabled_mix_count() const
{
    size_t n = 0;
    for (const MixedFilament &mf : m_rows) {
        if (mf.enabled)
            ++n;
    }
    return n;
}

std::vector<ColorRGB> MixedFilamentDialog::live_physical_colors() const
{
    std::vector<ColorRGB> out;
    if (PresetBundle *bundle = wxGetApp().preset_bundle) {
        if (const ConfigOptionStrings *opt = bundle->project_config.option<ConfigOptionStrings>("filament_colour")) {
            out.reserve(opt->values.size());
            for (const std::string &hex : opt->values) {
                ColorRGB c;
                const std::string norm = normalize_mix_match_hex(hex);
                if ((!norm.empty() && decode_color(norm, c)) || decode_color(hex, c))
                    out.push_back(c);
                else
                    out.push_back(ColorRGB::WHITE());
            }
        }
    }
    if (out.empty()) {
        if (Plater *plater = wxGetApp().plater()) {
            for (const ColorRGBA &c : plater->get_extruders_colors())
                out.push_back(to_rgb(c));
        }
    }
    // Preview uses every live physical (same list as the gizmo). Do not
    // truncate to 4 — Create-mix lattice prefix is match_printable_mix min(n,4).
    return out;
}

void MixedFilamentDialog::refresh_predicted_swatch()
{
    if (m_clr_predicted == nullptr)
        return;
    MixedFilament mf;
    if (m_selected_row >= 0 && size_t(m_selected_row) < m_rows.size())
        mf = m_rows[size_t(m_selected_row)];
    const ColorRGB pred = predicted_swatch_for_mix(mf, live_physical_colors());
    m_clr_predicted->SetColour(wxColour(pred.r_uchar(), pred.g_uchar(), pred.b_uchar()));
}

bool MixedFilamentDialog::parse_target_color(ColorRGB &out) const
{
    if (m_hex_target == nullptr)
        return false;
    const std::string hex_raw  = into_u8(m_hex_target->GetValue());
    const std::string hex_norm = normalize_mix_match_hex(hex_raw);
    const bool        hex_blank =
        std::all_of(hex_raw.begin(), hex_raw.end(), [](unsigned char ch) { return std::isspace(ch) != 0; });
    if (!hex_norm.empty())
        return decode_color(hex_norm, out);
    if (!hex_blank)
        return false;
    if (m_clr_target == nullptr)
        return false;
    const wxColour c = m_clr_target->GetColour();
    out = ColorRGB(static_cast<unsigned char>(c.Red()), static_cast<unsigned char>(c.Green()),
                   static_cast<unsigned char>(c.Blue()));
    return true;
}

wxString MixedFilamentDialog::candidate_label(const MixMatchResult &r,
                                              const std::vector<std::string> *slot_names) const
{
    if (r.kind == MixMatchResult::Kind::Physical) {
        if (slot_names != nullptr && r.physical_id >= 1 &&
            size_t(r.physical_id - 1) < slot_names->size()) {
            return wxString::Format("%s (filament %u)  ΔE %.1f%s",
                                    wxString::FromUTF8((*slot_names)[size_t(r.physical_id - 1)].c_str()),
                                    r.physical_id, double(r.distance),
                                    r.measured ? "  measured" : "");
        }
        return wxString::Format("filament %u  ΔE %.1f%s", r.physical_id, double(r.distance),
                                r.measured ? "  measured" : "");
    }

    int period = r.mix.ratio_a + r.mix.ratio_b;
    int mn     = std::min(r.mix.ratio_a, r.mix.ratio_b);
    if (r.mix.component_c != 0) {
        period += r.mix.ratio_c;
        mn = std::min(mn, r.mix.ratio_c);
    }
    const int min_share = period > 0 ? (mn * 100) / period : 0;
    const std::string recipe = spectrum_with_opaque_blend_marker(
        mix_recipe_label(r.mix, slot_names), wxGetApp().filament_opaque_flags(), r.mix,
        wxGetApp().spectrum_lut_loaded_for_live_batch());
    return wxString::Format("%s  ΔE %.1f  min %d%%%s", wxString::FromUTF8(recipe.c_str()),
                            double(r.distance), min_share, r.measured ? "  measured" : "");
}

void MixedFilamentDialog::refresh_candidates(bool preserve_selection)
{
    if (m_candidate_list == nullptr || m_min_mix_slider == nullptr)
        return;

    MixMatchResult keep;
    if (preserve_selection) {
        const int prev_sel = m_candidate_list->GetSelection();
        if (prev_sel >= 0 && size_t(prev_sel) < m_candidates.size())
            keep = m_candidates[size_t(prev_sel)];
    }

    ColorRGB target;
    m_candidates.clear();
    m_candidate_list->Clear();
    m_candidates_target_valid = false;
    if (!parse_target_color(target)) {
        refresh_predicted_swatch();
        return;
    }

    const std::vector<ColorRGB> physicals = live_physical_colors();
    const int                   min_pct   = m_min_mix_slider->GetValue();
    std::vector<std::string>    names;
    const SwatchLUT            *lut = nullptr;
    if (PresetBundle *bundle = wxGetApp().preset_bundle) {
        names = bundle->filament_presets;
        const std::string live_batch = spectrum_compute_batch_key(physicals, names);
        if (m_lut_status != nullptr) {
            if (spectrum_lut_is_stale(live_batch))
                m_lut_status->SetLabel(_L("stale — filament batch changed"));
            else if (spectrum_lut_for_batch(live_batch) != nullptr)
                m_lut_status->SetLabel(_L("measured"));
            else
                m_lut_status->SetLabel(wxEmptyString);
        }
        lut = spectrum_lut_for_batch(live_batch);
    } else if (m_lut_status != nullptr) {
        m_lut_status->SetLabel(wxEmptyString);
    }
    m_candidates = match_printable_candidates(target, physicals, nullptr, 4, min_pct, 12, 100, lut);
    m_candidates_target       = target;
    m_candidates_target_valid = true;

    const std::vector<std::string> names_cmik{"C", "M", "Y", "K"};
    const std::vector<std::string> *slot_names =
        (physicals.size() == 4) ? &names_cmik : nullptr;

    int select = -1;
    for (size_t i = 0; i < m_candidates.size(); ++i) {
        m_candidate_list->Append(candidate_label(m_candidates[i], slot_names));
        if (keep.valid && select < 0) {
            const MixMatchResult &c = m_candidates[i];
            if (c.kind == keep.kind) {
                if (c.kind == MixMatchResult::Kind::Physical && c.physical_id == keep.physical_id)
                    select = int(i);
                else if (c.kind == MixMatchResult::Kind::Mix && c.recipe_row == keep.recipe_row)
                    select = int(i);
            }
        }
    }

    if (m_candidates.empty()) {
        refresh_predicted_swatch();
        return;
    }
    // Target hex/picker: ranked best ([0]), including Physical when dark-neutral/pure wins.
    // Min Mix slider: keep prior recipe/Physical when it survived the filter (SHOULD).
    if (select < 0)
        select = 0;
    m_suppress_events = true;
    m_candidate_list->SetSelection(select);
    m_suppress_events = false;
    const MixMatchResult &chosen = m_candidates[size_t(select)];
    m_clr_predicted->SetColour(
        wxColour(chosen.predicted.r_uchar(), chosen.predicted.g_uchar(), chosen.predicted.b_uchar()));
}

void MixedFilamentDialog::on_candidate_select(wxCommandEvent &)
{
    if (m_suppress_events || m_candidate_list == nullptr || m_clr_predicted == nullptr)
        return;
    const int sel = m_candidate_list->GetSelection();
    if (sel < 0 || size_t(sel) >= m_candidates.size()) {
        refresh_predicted_swatch();
        return;
    }
    const ColorRGB &pred = m_candidates[size_t(sel)].predicted;
    m_clr_predicted->SetColour(wxColour(pred.r_uchar(), pred.g_uchar(), pred.b_uchar()));
}

void MixedFilamentDialog::on_min_mix_slider(wxCommandEvent &)
{
    if (m_min_mix_slider == nullptr || m_min_mix_label == nullptr)
        return;
    m_min_mix_label->SetLabel(wxString::Format(_L("Min mix: %d%%"), m_min_mix_slider->GetValue()));
    refresh_candidates(true);
}

void MixedFilamentDialog::on_target_colour_changed(wxColourPickerEvent &)
{
    if (m_suppress_events || m_hex_target == nullptr || m_clr_target == nullptr)
        return;
    const wxColour c = m_clr_target->GetColour();
    const ColorRGB rgb(static_cast<unsigned char>(c.Red()), static_cast<unsigned char>(c.Green()),
                       static_cast<unsigned char>(c.Blue()));
    m_hex_target->ChangeValue(wxString::FromUTF8(encode_color(rgb).c_str()));
    if (spectrum_match_same_target(m_candidates_target_valid, m_candidates_target, rgb))
        return;
    refresh_candidates();
}

void MixedFilamentDialog::on_hex_target_changed()
{
    if (m_suppress_events || m_hex_target == nullptr || m_clr_target == nullptr)
        return;
    ColorRGB target;
    const std::string hex_norm = normalize_mix_match_hex(into_u8(m_hex_target->GetValue()));
    if (!hex_norm.empty() && decode_color(hex_norm, target)) {
        m_suppress_events = true;
        m_clr_target->SetColour(wxColour(target.r_uchar(), target.g_uchar(), target.b_uchar()));
        m_suppress_events = false;
    }
    ColorRGB parsed;
    if (parse_target_color(parsed) &&
        spectrum_match_same_target(m_candidates_target_valid, m_candidates_target, parsed))
        return;
    refresh_candidates();
}

void MixedFilamentDialog::on_create_mix_from_color(wxCommandEvent &)
{
    store_editors_into_selected_row();

    ColorRGB target;
    const std::string hex_raw  = into_u8(m_hex_target->GetValue());
    const std::string hex_norm = normalize_mix_match_hex(hex_raw);
    const bool        hex_blank =
        std::all_of(hex_raw.begin(), hex_raw.end(), [](unsigned char ch) { return std::isspace(ch) != 0; });
    if (!hex_norm.empty()) {
        if (!decode_color(hex_norm, target)) {
            MessageDialog(this, _L("Invalid color. Enter a hex value like #99401B or #99401BFF."),
                          _L("Mixed Filaments"), wxOK | wxICON_WARNING)
                .ShowModal();
            return;
        }
    } else if (!hex_blank) {
        MessageDialog(this, _L("Invalid color. Enter a hex value like #99401B or #99401BFF."),
                      _L("Mixed Filaments"), wxOK | wxICON_WARNING)
            .ShowModal();
        return;
    } else if (!parse_target_color(target)) {
        return;
    }

    // Rebuild only when the list is stale for this target; keep the user's pick when it survives.
    const bool same_target = m_candidates_target_valid && m_candidates_target == target;
    refresh_candidates(same_target);
    if (m_candidates.empty()) {
        MessageDialog(this, _L("Could not match a printable mix for that color."), _L("Mixed Filaments"),
                      wxOK | wxICON_WARNING)
            .ShowModal();
        return;
    }

    const int sel = (m_candidate_list != nullptr) ? m_candidate_list->GetSelection() : 0;
    const MixMatchResult *selected =
        (sel >= 0 && size_t(sel) < m_candidates.size()) ? &m_candidates[size_t(sel)] : &m_candidates.front();

    // Selected Mix → append. Selected / best Physical → informational modal (0010 behavior).
    // No selection → first Mix if any, else Physical modal.
    const MixMatchResult *mix_to_add = nullptr;
    if (selected->kind == MixMatchResult::Kind::Mix) {
        mix_to_add = selected;
    } else if (sel < 0) {
        for (const MixMatchResult &c : m_candidates) {
            if (c.kind == MixMatchResult::Kind::Mix) {
                mix_to_add = &c;
                break;
            }
        }
    }
    if (mix_to_add == nullptr) {
        if (selected->kind == MixMatchResult::Kind::Physical) {
            MessageDialog(this,
                          wxString::Format(_L("Closest is filament %u"), selected->physical_id),
                          _L("Mixed Filaments"), wxOK | wxICON_INFORMATION)
                .ShowModal();
            return;
        }
        MessageDialog(this, _L("Could not match a printable mix for that color."), _L("Mixed Filaments"),
                      wxOK | wxICON_WARNING)
            .ShowModal();
        return;
    }

    if (!spectrum_mix_enabled_fits(enabled_mix_count())) {
        MessageDialog(this,
                      wxString::Format(_L("Enabled mixes cannot exceed %d."),
                                       int(SPECTRUM_MIX_ENABLED_CAP)),
                      _L("Mixed Filaments"), wxOK | wxICON_WARNING)
            .ShowModal();
        return;
    }

    m_rows.push_back(mix_to_add->mix);
    m_selected_row = int(m_rows.size()) - 1;
    wxGetApp().maybe_warn_opaque_blend(this, serialize_mix_row(mix_to_add->mix));
    refresh_list();
    load_selected_row_into_editors();
}

wxString MixedFilamentDialog::row_label(size_t idx, size_t num_physical) const
{
    if (idx >= m_rows.size())
        return {};
    const MixedFilament &mf = m_rows[idx];
    const std::vector<std::string> names_cmik{"C", "M", "Y", "K"};
    const std::vector<std::string> *slot_names =
        (live_physical_colors().size() == 4) ? &names_cmik : nullptr;
    const std::string recipe = spectrum_with_opaque_blend_marker(
        mix_recipe_label(mf, slot_names), wxGetApp().filament_opaque_flags(), mf,
        wxGetApp().spectrum_lut_loaded_for_live_batch());
    const wxString pair = wxString::FromUTF8(recipe.c_str());
    const int      vid  = virtual_id_for_row(idx, num_physical);
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

void MixedFilamentDialog::select_row(int idx)
{
    if (idx < 0 || m_rows.empty() || size_t(idx) >= m_rows.size())
        return;
    if (m_selected_row >= 0 && size_t(m_selected_row) < m_rows.size() && idx != m_selected_row)
        store_editors_into_selected_row();
    m_selected_row = idx;
    if (m_list != nullptr) {
        m_suppress_events = true;
        m_list->SetSelection(idx);
        m_suppress_events = false;
    }
    load_selected_row_into_editors();
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
    m_gradient->SetValue(mf.gradient_enabled);
    m_perimeter->SetValue(mf.perimeter_modulation);
    refresh_predicted_swatch();

    if (m_notebook != nullptr) {
        const std::string pattern = MixedFilamentManager::normalize_manual_pattern(mf.manual_pattern);
        const bool        prev    = m_suppress_events;
        m_suppress_events         = true;
        if (!pattern.empty())
            m_notebook->SetSelection(1); // Cycle
        else if (mf.gradient_enabled)
            m_notebook->SetSelection(3); // Gradient
        else
            m_notebook->SetSelection(0); // Ratio
        m_suppress_events = prev;
    }
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
    mf.enabled = m_enabled->GetValue();
    if (m_notebook != nullptr && m_notebook->GetSelection() == 0) {
        mf.manual_pattern.clear();
        m_pattern->SetValue(wxEmptyString);
    } else {
        mf.manual_pattern = MixedFilamentManager::normalize_manual_pattern(into_u8(m_pattern->GetValue()));
    }
    double offset_a = 0.0;
    double offset_b = 0.0;
    m_offset_a->GetValue().ToDouble(&offset_a);
    m_offset_b->GetValue().ToDouble(&offset_b);
    mf.component_a_surface_offset = float(offset_a);
    mf.component_b_surface_offset = float(offset_b);
    mf.gradient_enabled           = m_gradient->GetValue();
    mf.perimeter_modulation       = m_perimeter->GetValue();
    if (mf.gradient_enabled) {
        mf.component_c = 0;
        mf.manual_pattern.clear();
        m_spin_c->SetValue(0);
        m_pattern->SetValue(wxEmptyString);
    }
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
    if (!spectrum_mix_enabled_fits(enabled_mix_count())) {
        MessageDialog(this,
                      wxString::Format(_L("Enabled mixes cannot exceed %d."),
                                       int(SPECTRUM_MIX_ENABLED_CAP)),
                      _L("Mixed Filaments"), wxOK | wxICON_WARNING)
            .ShowModal();
        return;
    }
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

void MixedFilamentDialog::on_add_recommended(wxCommandEvent &)
{
    store_editors_into_selected_row();

    const size_t n = physical_filament_count();
    if (n < 2) {
        MessageDialog(this, _L("Cannot recommend mixes with fewer than two physical filaments."),
                      _L("Mixed Filaments"), wxOK | wxICON_INFORMATION)
            .ShowModal();
        return;
    }

    const MixCookbookAppend r = spectrum_cookbook_append(m_rows, n);
    if (r.added.empty()) {
        // Cap hit with nothing added (even if some recipes were also duplicates) → mix-row cap modal.
        if (r.skipped_cap != 0) {
            MessageDialog(this,
                          wxString::Format(_L("Enabled mixes cannot exceed %d."),
                                           int(SPECTRUM_MIX_ENABLED_CAP)),
                          _L("Mixed Filaments"), wxOK | wxICON_WARNING)
                .ShowModal();
        } else {
            MessageDialog(this, _L("All recommended mixes are already in the list."), _L("Mixed Filaments"),
                          wxOK | wxICON_INFORMATION)
                .ShowModal();
        }
        return;
    }

    m_rows.insert(m_rows.end(), r.added.begin(), r.added.end());
    m_selected_row = int(m_rows.size()) - 1;
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

    const DynamicPrintConfig &print_cfg = bundle->prints.get_edited_preset().config;
    if (const ConfigOptionBool *ept = print_cfg.option<ConfigOptionBool>("enable_prime_tower"))
        m_enable_tower->SetValue(ept->value);
    if (const ConfigOptionBool *lz = print_cfg.option<ConfigOptionBool>("dithering_local_z_mode"))
        m_local_z->SetValue(lz->value);
    if (const ConfigOptionBool *fd = print_cfg.option<ConfigOptionBool>("dithering_local_z_whole_objects"))
        m_full_domain->SetValue(fd->value);

    // 0005 legacy: process key on with no `,g` yet → stamp pair-capable editor rows.
    bool process_gradient = false;
    if (const ConfigOptionBool *gr = print_cfg.option<ConfigOptionBool>("mixed_filament_gradient_mode"))
        process_gradient = gr->value;
    spectrum_stamp_legacy_process_gradient(m_rows, process_gradient);

    refresh_list();
    load_selected_row_into_editors();
}

bool MixedFilamentDialog::apply_to_project()
{
    PresetBundle *bundle = wxGetApp().preset_bundle;
    Plater       *plater = wxGetApp().plater();
    if (bundle == nullptr || plater == nullptr)
        return false;

    store_editors_into_selected_row();
    if (!spectrum_mix_enabled_fits(enabled_mix_count(), 0)) {
        MessageDialog(this,
                      wxString::Format(_L("Enabled mixes cannot exceed %d."),
                                       int(SPECTRUM_MIX_ENABLED_CAP)),
                      _L("Mixed Filaments"), wxOK | wxICON_WARNING)
            .ShowModal();
        return false;
    }
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
    wxGetApp().maybe_warn_opaque_blend(this, serialized);

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

    DynamicPrintConfig &print_cfg = bundle->prints.get_edited_preset().config;
    bool pre_process_gradient = false;
    if (const ConfigOptionBool *gr = print_cfg.option<ConfigOptionBool>("mixed_filament_gradient_mode"))
        pre_process_gradient = gr->value;

    if (mixed_filament_painted_ids_would_shift(old_serialized, serialized, num_physical, painted_ids,
                                               pre_process_gradient)) {
        MessageDialog(this,
            _L("Disabling a mix in the middle of the list changes Mix IDs. Painted regions using later mix IDs will follow the new enabled-row order."),
            _L("Mixed Filaments"), wxOK | wxICON_WARNING).ShowModal();
    }

    SpectrumMixDialogUndoKeys pre;
    pre.mixed_filament_definitions = old_serialized;
    if (const ConfigOptionBool *ept = print_cfg.option<ConfigOptionBool>("enable_prime_tower"))
        pre.enable_prime_tower = ept->value;
    if (const ConfigOptionBool *lz = print_cfg.option<ConfigOptionBool>("dithering_local_z_mode"))
        pre.dithering_local_z_mode = lz->value;
    if (const ConfigOptionBool *fd = print_cfg.option<ConfigOptionBool>("dithering_local_z_whole_objects"))
        pre.dithering_local_z_whole_objects = fd->value;
    pre.mixed_filament_gradient_mode = pre_process_gradient;

    bool any_enabled_g = false;
    for (const MixedFilament &mf : m_rows) {
        if (mf.enabled && mf.gradient_enabled) {
            any_enabled_g = true;
            break;
        }
    }

    SpectrumMixDialogUndoKeys post;
    post.mixed_filament_definitions    = serialized;
    post.enable_prime_tower            = m_enable_tower->GetValue() ? true : pre.enable_prime_tower;
    post.dithering_local_z_mode        = m_local_z->GetValue();
    post.dithering_local_z_whole_objects = m_full_domain->GetValue();
    post.mixed_filament_gradient_mode  = any_enabled_g;

    const int virtual_id = (m_selected_row >= 0) ? virtual_id_for_row(size_t(m_selected_row), num_physical) : -1;

    // Resolve assign target before the no-op skip so unchanged keys + checkbox-on
    // with no volume/object selection does not create an empty undo snapshot.
    ModelVolume *assign_volume = nullptr;
    ModelObject *assign_object = nullptr;
    if (m_apply_object->GetValue() && virtual_id > 0 && !plater->model().objects.empty()) {
        if (ObjectList *list = wxGetApp().obj_list())
            assign_volume = list->get_selected_model_volume();
        const Selection &sel = plater->get_selection();
        if (assign_volume == nullptr) {
            int obj_idx = -1;
            int vol_idx = -1;
            assign_volume = sel.get_selected_single_volume(obj_idx, vol_idx);
        }
        if (assign_volume == nullptr) {
            const int obj_idx = sel.get_object_idx();
            if (obj_idx >= 0 && size_t(obj_idx) < plater->model().objects.size())
                assign_object = plater->model().objects[size_t(obj_idx)];
        }
    }
    const bool will_assign = assign_volume != nullptr || assign_object != nullptr;

    const bool keys_unchanged =
        pre.mixed_filament_definitions == post.mixed_filament_definitions &&
        pre.enable_prime_tower == post.enable_prime_tower &&
        pre.dithering_local_z_mode == post.dithering_local_z_mode &&
        pre.dithering_local_z_whole_objects == post.dithering_local_z_whole_objects &&
        pre.mixed_filament_gradient_mode == post.mixed_filament_gradient_mode;
    if (keys_unchanged && !will_assign)
        return true;

    plater->apply_mixed_filament_dialog_keys(pre, post);

    if (any_enabled_g && (!m_local_z->GetValue() || !m_full_domain->GetValue())) {
        MessageDialog(this,
                      _L("Height gradient mixes require Subdivide Mix Layer and Full domain to slice. "
                         "Settings were saved; enable both process options to print the gradient."),
                      _L("Mixed Filaments"), wxOK | wxICON_INFORMATION)
            .ShowModal();
    }

    if (will_assign) {
        if (assign_volume != nullptr) {
            // Palette path: assign only the selected volume. Do not touch siblings.
            assign_extruder(assign_volume->config, virtual_id);
        } else if (assign_object != nullptr) {
            // Persist mix rows even when nothing is selected. Do not assign objects.front().
            assign_extruder(assign_object->config, virtual_id);
        }
    }

    plater->on_config_change(bundle->full_config());
    plater->schedule_background_process();
    return true;
}

} // namespace GUI
} // namespace Slic3r
