#include "K3DAccuracyDialog.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <wx/button.h>
#include <wx/image.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Plater.hpp"
#include "Tab.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"

namespace Slic3r { namespace GUI {

namespace {
constexpr std::array<const char*, 10> k_dim_names = {
    "XWS", "XWL", "XLS", "XLL", "XLF",
    "YWS", "YWL", "YLS", "YLL", "YLF"
};
constexpr std::array<double, 10> k_dim_nominals = {
    15.0, 15.0, 30.0, 100.0, 145.0,
    15.0, 15.0, 30.0, 100.0, 145.0
};
} // namespace

K3DAccuracyDialog::K3DAccuracyDialog(wxWindow* parent, Plater* plater)
    : DPIDialog(parent, wxID_ANY, _L("K3D Accuracy Calibration (v3.0)"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_plater(plater)
{
    auto* top_sizer = new wxBoxSizer(wxVERTICAL);

    // Top settings row
    auto* settings_sizer = new wxFlexGridSizer(3, 2, 6, 12);
    settings_sizer->AddGrowableCol(1, 1);

    settings_sizer->Add(new wxStaticText(this, wxID_ANY, _L("Model scale (%):")), 0, wxALIGN_CENTER_VERTICAL);
    m_model_scale_ctrl = new wxSpinCtrlDouble(this, wxID_ANY, "100.0", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 50.0, 200.0, 100.0, 1.0);
    m_model_scale_ctrl->SetDigits(2);
    settings_sizer->Add(m_model_scale_ctrl, 0, wxEXPAND);

    settings_sizer->Add(new wxStaticText(this, wxID_ANY, _L("Optimization goal:")), 0, wxALIGN_CENTER_VERTICAL);
    m_goal_choice = new wxChoice(this, wxID_ANY);
    m_goal_choice->Append(_L("Absolute sizes (mm)"));
    m_goal_choice->Append(_L("Relative sizes (%)"));
    m_goal_choice->Append(_L("Balance"));
    m_goal_choice->SetSelection(0);
    settings_sizer->Add(m_goal_choice, 0, wxEXPAND);

    settings_sizer->Add(new wxStaticText(this, wxID_ANY, _L("Zero equidistant:")), 0, wxALIGN_CENTER_VERTICAL);
    m_zero_eq_check = new wxCheckBox(this, wxID_ANY, _L("Disable equidistant (scale only)"));
    settings_sizer->Add(m_zero_eq_check, 0, wxALIGN_CENTER_VERTICAL);

    top_sizer->Add(settings_sizer, 0, wxALL | wxEXPAND, 10);

    // Measurements grid: Name | Nominal | Measured | Theoretical
    auto* grid = new wxFlexGridSizer(11, 4, 4, 12);
    grid->Add(new wxStaticText(this, wxID_ANY, _L("Dimension")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(new wxStaticText(this, wxID_ANY, _L("Nominal (mm)")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(new wxStaticText(this, wxID_ANY, _L("Measured (mm)")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(new wxStaticText(this, wxID_ANY, _L("Theoretical (mm)")), 0, wxALIGN_CENTER_VERTICAL);

    for (size_t i = 0; i < 10; ++i) {
        grid->Add(new wxStaticText(this, wxID_ANY, k_dim_names[i]), 0, wxALIGN_CENTER_VERTICAL);
        m_nom_labels[i] = new wxStaticText(this, wxID_ANY, wxString::Format("%.2f", k_dim_nominals[i]));
        grid->Add(m_nom_labels[i], 0, wxALIGN_CENTER_VERTICAL);

        m_meas_ctrls[i] = new wxSpinCtrlDouble(this, wxID_ANY, wxString::Format("%.2f", k_dim_nominals[i]),
                                               wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1.0, 400.0, k_dim_nominals[i], 0.01);
        m_meas_ctrls[i]->SetDigits(2);
        grid->Add(m_meas_ctrls[i], 0, wxEXPAND);

        m_theo_labels[i] = new wxStaticText(this, wxID_ANY, wxString::Format("%.2f", k_dim_nominals[i]));
        grid->Add(m_theo_labels[i], 0, wxALIGN_CENTER_VERTICAL);

        m_meas_ctrls[i]->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) { recalculate(); });
        m_meas_ctrls[i]->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { recalculate(); });
    }

    auto* body_sizer = new wxBoxSizer(wxHORIZONTAL);
    body_sizer->Add(grid, 0, wxEXPAND | wxRIGHT, 12);

    const boost::filesystem::path img_path = boost::filesystem::path(Slic3r::resources_dir()) / "images" / "k3d_accuracy_diagram.png";
    if (boost::filesystem::exists(img_path)) {
        wxImage img(wxString::FromUTF8(img_path.string()), wxBITMAP_TYPE_PNG);
        if (img.IsOk()) {
            const int target_w = em_unit() * 34;
            const int target_h = std::max(1, (img.GetHeight() * target_w) / std::max(1, img.GetWidth()));
            img.Rescale(target_w, target_h, wxIMAGE_QUALITY_HIGH);
            m_diagram_bmp = new wxStaticBitmap(this, wxID_ANY, wxBitmap(img));
            body_sizer->Add(m_diagram_bmp, 0, wxALIGN_CENTER_VERTICAL);
        }
    }
    top_sizer->Add(body_sizer, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

    // Results box
    auto* res_sizer = new wxFlexGridSizer(3, 2, 4, 12);
    res_sizer->Add(new wxStaticText(this, wxID_ANY, _L("Size compensation (X):")), 0, wxALIGN_CENTER_VERTICAL);
    m_out_scale_x = new wxStaticText(this, wxID_ANY, "100.00%");
    res_sizer->Add(m_out_scale_x, 0, wxALIGN_CENTER_VERTICAL);

    res_sizer->Add(new wxStaticText(this, wxID_ANY, _L("Size compensation (Y):")), 0, wxALIGN_CENTER_VERTICAL);
    m_out_scale_y = new wxStaticText(this, wxID_ANY, "100.00%");
    res_sizer->Add(m_out_scale_y, 0, wxALIGN_CENTER_VERTICAL);

    res_sizer->Add(new wxStaticText(this, wxID_ANY, _L("XY equidistant:")), 0, wxALIGN_CENTER_VERTICAL);
    m_out_equidistant = new wxStaticText(this, wxID_ANY, "0.000 mm");
    res_sizer->Add(m_out_equidistant, 0, wxALIGN_CENTER_VERTICAL);
    top_sizer->Add(res_sizer, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

    // Action buttons
    auto* btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto* load_btn  = new wxButton(this, wxID_ANY, _L("Load K3D v8 Model"));
    auto* apply_btn = new wxButton(this, wxID_ANY, _L("Apply to Current Filament"));
    auto* close_btn = new wxButton(this, wxID_CLOSE, _L("Close"));
    btn_sizer->Add(load_btn, 0, wxRIGHT, 8);
    btn_sizer->Add(apply_btn, 0, wxRIGHT, 8);
    btn_sizer->AddStretchSpacer();
    btn_sizer->Add(close_btn, 0);
    top_sizer->Add(btn_sizer, 0, wxALL | wxEXPAND, 10);

    m_model_scale_ctrl->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) { recalculate(); });
    m_goal_choice->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { recalculate(); });
    m_zero_eq_check->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { recalculate(); });
    load_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_load_model(); });
    apply_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_apply_to_filament(); });
    close_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Close(); });

    recalculate();
    SetSizerAndFit(top_sizer);
    CentreOnParent();
}

void K3DAccuracyDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    Layout();
    Fit();
}

void K3DAccuracyDialog::recalculate()
{
    const double m_scale = std::max(0.01, m_model_scale_ctrl->GetValue() / 100.0);
    const auto   goal    = static_cast<OptGoal>(std::clamp(m_goal_choice->GetSelection(), 0, 2));
    const bool   zero_eq = m_zero_eq_check->GetValue();

    std::array<double, 10> nom{};
    std::array<double, 10> meas{};
    for (size_t i = 0; i < 10; ++i) {
        nom[i]  = k_dim_nominals[i] * m_scale;
        meas[i] = std::max(0.01, m_meas_ctrls[i]->GetValue());
        m_nom_labels[i]->SetLabel(wxString::Format("%.2f", nom[i]));
    }

    // Indices: 0:XWS, 1:XWL, 2:XLS, 3:XLL, 4:XLF, 5:YWS, 6:YWL, 7:YLS, 8:YLL, 9:YLF
    double sx = 1.0;
    double sy = 1.0;
    double eq = 0.0;

    if (!zero_eq) {
        const double w_s_abs = nom[2] / (nom[2] + nom[3]);
        const double w_l_abs = nom[3] / (nom[2] + nom[3]);
        double w_s = w_s_abs;
        double w_l = w_l_abs;
        if (goal == OptGoal::Relative) {
            w_s = 0.5;
            w_l = 0.5;
        } else if (goal == OptGoal::Balance) {
            w_s = 0.5 * (w_s_abs + 0.5);
            w_l = 0.5 * (w_l_abs + 0.5);
        }

        const double xls_rel = meas[2] / nom[2];
        const double xll_rel = meas[3] / nom[3];
        const double yls_rel = meas[7] / nom[7];
        const double yll_rel = meas[8] / nom[8];

        sx = 1.0 / (xls_rel * w_s + xll_rel * w_l);
        sy = 1.0 / (yls_rel * w_s + yll_rel * w_l);

        // XWS, XWL are widths of X-aligned beams (measured along Y); YWS, YWL are measured along X.
        const double d_xws = meas[0] * sy - nom[0];
        const double d_xwl = meas[1] * sy - nom[1];
        const double d_xlf = meas[4] * sx - nom[4];
        const double d_yws = meas[5] * sx - nom[5];
        const double d_ywl = meas[6] * sx - nom[6];
        const double d_ylf = meas[9] * sy - nom[9];
        const double avg_2delta = (d_xws + d_xwl + d_xlf + d_yws + d_ywl + d_ylf) / 6.0;
        eq = -avg_2delta / 2.0;
    } else {
        const double sum5 = nom[0] + nom[1] + nom[2] + nom[3] + nom[4];
        std::array<double, 5> w{};
        for (size_t i = 0; i < 5; ++i) {
            const double w_abs = nom[i] / sum5;
            if (goal == OptGoal::Absolute)
                w[i] = w_abs;
            else if (goal == OptGoal::Relative)
                w[i] = 0.2;
            else
                w[i] = 0.5 * (w_abs + 0.2);
        }

        const double raw_x = (meas[5] / nom[5]) * w[0] + (meas[6] / nom[6]) * w[1] +
                             (meas[2] / nom[2]) * w[2] + (meas[3] / nom[3]) * w[3] + (meas[4] / nom[4]) * w[4];
        const double raw_y = (meas[0] / nom[0]) * w[0] + (meas[1] / nom[1]) * w[1] +
                             (meas[7] / nom[7]) * w[2] + (meas[8] / nom[8]) * w[3] + (meas[9] / nom[9]) * w[4];
        sx = 1.0 / raw_x;
        sy = 1.0 / raw_y;
        eq = 0.0;
    }

    m_calc_scale_x     = sx * 100.0;
    m_calc_scale_y     = sy * 100.0;
    m_calc_equidistant = eq;

    m_out_scale_x->SetLabel(wxString::Format("%.2f%%", m_calc_scale_x));
    m_out_scale_y->SetLabel(wxString::Format("%.2f%%", m_calc_scale_y));
    m_out_equidistant->SetLabel(wxString::Format("%.3f mm", m_calc_equidistant));

    // Theoretical post-calibration dimensions
    const std::array<double, 10> theo = {
        meas[0] * sy + 2.0 * eq,
        meas[1] * sy + 2.0 * eq,
        meas[2] * sx,
        meas[3] * sx,
        meas[4] * sx + 2.0 * eq,
        meas[5] * sx + 2.0 * eq,
        meas[6] * sx + 2.0 * eq,
        meas[7] * sy,
        meas[8] * sy,
        meas[9] * sy + 2.0 * eq,
    };
    for (size_t i = 0; i < 10; ++i)
        m_theo_labels[i]->SetLabel(wxString::Format("%.2f", theo[i]));
}

void K3DAccuracyDialog::on_load_model()
{
    if (!m_plater)
        return;
    const boost::filesystem::path stl_path = boost::filesystem::path(Slic3r::resources_dir()) / "handy_models" / "k3d_accuracy_test_v8.stl";
    std::vector<boost::filesystem::path> files{ stl_path };
    m_plater->load_files(files, LoadStrategy::LoadModel);
}

void K3DAccuracyDialog::on_apply_to_filament()
{
    Preset& preset = wxGetApp().preset_bundle->filaments.get_edited_preset();
    DynamicPrintConfig& cfg = preset.config;
    if (auto* opt_x = cfg.opt<ConfigOptionPercents>("filament_size_compensation_x", true))
        opt_x->values[0] = m_calc_scale_x;
    if (auto* opt_y = cfg.opt<ConfigOptionPercents>("filament_size_compensation_y", true))
        opt_y->values[0] = m_calc_scale_y;
    if (auto* opt_eq = cfg.opt<ConfigOptionFloats>("filament_xy_equidistant", true))
        opt_eq->values[0] = m_calc_equidistant;

    if (Tab* tab = wxGetApp().get_tab(Preset::TYPE_FILAMENT)) {
        tab->reload_config();
        tab->update_dirty();
    }
    if (m_plater) {
        std::vector<size_t> idxs(m_plater->model().objects.size());
        for (size_t i = 0; i < idxs.size(); ++i)
            idxs[i] = i;
        if (!idxs.empty())
            m_plater->changed_objects(idxs);
    }

    wxMessageBox(
        wxString::Format(_L("Applied to filament '%s':\nSize compensation (X): %.2f%%\nSize compensation (Y): %.2f%%\nXY equidistant: %.3f mm"),
                         preset.name, m_calc_scale_x, m_calc_scale_y, m_calc_equidistant),
        _L("K3D Accuracy Calibration"), wxOK | wxICON_INFORMATION, this);
}

}} // namespace Slic3r::GUI
