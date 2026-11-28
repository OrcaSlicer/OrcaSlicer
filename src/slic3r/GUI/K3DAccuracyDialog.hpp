#pragma once

#include <array>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/spinctrl.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>

#include "GUI_Utils.hpp"

namespace Slic3r { namespace GUI {

class Plater;

/// K3D Accuracy Calibration v3.0 calculator and v8 model loader dialog.
class K3DAccuracyDialog : public DPIDialog
{
public:
    K3DAccuracyDialog(wxWindow* parent, Plater* plater);

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    enum class OptGoal {
        Absolute = 0,
        Relative = 1,
        Balance  = 2,
    };

    void recalculate();
    void on_load_model();
    void on_apply_to_filament();

    Plater* m_plater{ nullptr };

    wxSpinCtrlDouble* m_model_scale_ctrl{ nullptr };
    wxChoice*         m_goal_choice{ nullptr };
    wxCheckBox*       m_zero_eq_check{ nullptr };

    // Order: XWS, XWL, XLS, XLL, XLF, YWS, YWL, YLS, YLL, YLF
    std::array<wxSpinCtrlDouble*, 10> m_meas_ctrls{};
    std::array<wxStaticText*, 10>     m_nom_labels{};
    std::array<wxStaticText*, 10>     m_theo_labels{};

    wxStaticText* m_out_scale_x{ nullptr };
    wxStaticText* m_out_scale_y{ nullptr };
    wxStaticText* m_out_equidistant{ nullptr };
    wxStaticBitmap* m_diagram_bmp{ nullptr };

    double m_calc_scale_x{ 100.0 };
    double m_calc_scale_y{ 100.0 };
    double m_calc_equidistant{ 0.0 };
};

}} // namespace Slic3r::GUI
