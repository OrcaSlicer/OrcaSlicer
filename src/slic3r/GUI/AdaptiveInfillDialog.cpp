#include "AdaptiveInfillDialog.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>

#include <wx/dialog.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/string.h>
#include <wx/toplevel.h>

#include "libslic3r/Config.hpp"
#include "libslic3r/Fill/AdaptiveInfillField.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "Field.hpp"
#include "format.hpp"
#include "GUI_App.hpp"
#include "GUI_Utils.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "Tab.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/DensityCurveGraph.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"

namespace Slic3r { namespace GUI {

namespace {

// Handles of the graph.
enum GraphHandle : size_t { Surface, Center };

const char *const EditedKeys[] = { "sparse_infill_density", "adaptive_infill_interior_density", "adaptive_infill_gradient" };

} // namespace

AdaptiveInfillDialog::AdaptiveInfillDialog(wxWindow *parent, Tab *tab)
    : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe), wxID_ANY, _L("Adaptive Infill Density Graph"), wxDefaultPosition,
                wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_tab(tab)
{
    SetBackgroundColour(*wxWHITE);
    SetFont(Label::Body_14);
    const DynamicPrintConfig &tab_config = *tab->get_config();
    for (const char *key : EditedKeys)
        m_config.set_key_value(key, tab_config.option(key)->clone());

    auto *sizer = new wxBoxSizer(wxVERTICAL);

    const ConfigOptionDef *gradient_def = print_config_def.get("adaptive_infill_gradient");
    auto                  *header       = new wxBoxSizer(wxHORIZONTAL);
    header->AddStretchSpacer();
    auto *gradient_label = new wxStaticText(this, wxID_ANY, _(gradient_def->label));
    gradient_label->SetToolTip(_(gradient_def->tooltip));
    m_gradient = new ::ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, FromDIP(wxSize(150, -1)), 0, nullptr, wxCB_READONLY);
    for (const std::string &label : gradient_def->enum_labels)
        m_gradient->Append(_(label));
    m_gradient->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &) {
        m_config.set_key_value("adaptive_infill_gradient", new ConfigOptionEnum<AdaptiveInfillGradient>(AdaptiveInfillGradient(m_gradient->GetSelection())));
        update();
    });
    header->Add(gradient_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    header->Add(m_gradient, 0, wxALIGN_CENTER_VERTICAL);
    sizer->Add(header, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    m_graph = new DensityCurveGraph(this, wxSize(520, 280));
    m_graph->SetToolTip(_L("Drag the points to set the density at the surface and at the center of the object. The curve is the "
                           "density the infill aims at; Stepped shells and Smooth blend reach it in steps."));
    m_graph->on_drag = [this](const DensityCurveGraph::Drag &drag) {
        switch (drag.phase) {
        case DensityCurveGraph::Drag::Phase::Begin: m_drag_start = m_config; return;
        case DensityCurveGraph::Drag::Phase::Cancel: m_config = m_drag_start; break;
        default: {
            // Whole percentages, like the inputs. At 100% the surface would be solid, and adaptive infill off.
            const double percent = std::round(drag.density * 100.);
            if (drag.handle == Surface)
                m_config.set_key_value("sparse_infill_density", new ConfigOptionPercent(std::clamp(percent, 1., 99.)));
            else
                m_config.set_key_value("adaptive_infill_interior_density", new ConfigOptionPercent(std::clamp(percent, 1., 100.)));
        }
        }
        update();
    };
    sizer->Add(m_graph, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

    auto *buttons = new DialogButtons(this, { "Cancel", "Apply" });
    buttons->GetAPPLY()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        DynamicPrintConfig new_config = *m_tab->get_config();
        for (const char *key : EditedKeys)
            new_config.set_key_value(key, m_config.option(key)->clone());
        m_tab->load_config(new_config);
        EndModal(wxID_OK);
    });
    buttons->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });
    sizer->Add(buttons, 0, wxEXPAND | wxTOP, FromDIP(10));

    update();
    SetSizerAndFit(sizer);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

DensityCurve AdaptiveInfillDialog::curve() const
{
    const double                 surface  = m_config.option<ConfigOptionPercent>("sparse_infill_density")->value / 100.;
    const double                 interior = m_config.option<ConfigOptionPercent>("adaptive_infill_interior_density")->value / 100.;
    const AdaptiveInfillGradient gradient = m_config.opt_enum<AdaptiveInfillGradient>("adaptive_infill_gradient");
    DensityCurve                 curve;
    curve.density = [surface, interior, gradient](double x) {
        return surface * adaptive_infill_target_scale(interior / surface, gradient, x / 100.);
    };
    curve.x_max   = 100.;
    curve.x_title = _L("Depth from the surface to the center (%)");
    curve.y_title = _L("Infill density");
    // TRN: The density of the adaptive infill at the surface of the object, on its density graph. %1% is a percentage.
    const wxString surface_readout = format_wxstr(_L("Surface: %1%%%"), double_to_string(std::round(surface * 100.), 0));
    // TRN: The density of the adaptive infill at the center of the object, on its density graph. %1% is a percentage.
    const wxString center_readout = format_wxstr(_L("Center: %1%%%"), double_to_string(std::round(interior * 100.), 0));
    curve.handles.push_back({ DensityCurve::HandleKind::Point, 0., surface, surface_readout });
    curve.handles.push_back({ DensityCurve::HandleKind::Point, 100., interior, center_readout });
    return curve;
}

void AdaptiveInfillDialog::update()
{
    m_gradient->SetSelection(int(m_config.opt_enum<AdaptiveInfillGradient>("adaptive_infill_gradient")));
    m_graph->set_curve(curve());
}

void AdaptiveInfillDialog::on_dpi_changed(const wxRect &)
{
    m_gradient->Rescale();
    m_gradient->SetMinSize(FromDIP(wxSize(150, -1)));
    m_graph->rescale();
    GetSizer()->SetSizeHints(this);
    Refresh();
}

void AdaptiveInfillDialog::on_sys_color_changed() { Refresh(); }

}} // namespace Slic3r::GUI
