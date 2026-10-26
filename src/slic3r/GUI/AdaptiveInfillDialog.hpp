#pragma once

#include <wx/gdicmn.h>
#include <wx/window.h>

#include "libslic3r/PrintConfig.hpp"

#include "GUI_Utils.hpp"

class ComboBox;
class DensityCurveGraph;
struct DensityCurve;

namespace Slic3r { namespace GUI {

class Tab;

// Shapes the adaptive infill of the process preset on a graph of its target density from the surface to the center:
// the surface density, the interior density and the gradient between them. Apply writes them into the Process tab.
class AdaptiveInfillDialog : public DPIDialog
{
public:
    AdaptiveInfillDialog(wxWindow *parent, Tab *tab);

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;
    void on_sys_color_changed() override;

private:
    Tab               *m_tab;
    DynamicPrintConfig m_config;
    // The settings when a drag began, restored if it is cancelled.
    DynamicPrintConfig m_drag_start;
    DensityCurveGraph *m_graph { nullptr };
    ::ComboBox        *m_gradient { nullptr };

    DensityCurve curve() const;
    void         update();
};

}} // namespace Slic3r::GUI
