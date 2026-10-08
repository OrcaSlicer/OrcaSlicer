#include "SmallAreaInfillFlowCompensationDialog.hpp"

#include <memory>
#include <string>
#include <vector>
#include "CurveEditorDialog.hpp"
#include "I18N.hpp"
#include "libslic3r/GCode/SmallAreaInfillFlowCompensationModel.hpp"

namespace Slic3r::GUI {

SmallAreaInfillFlowCompensationDialog::SmallAreaInfillFlowCompensationDialog(
    wxWindow* parent, const std::vector<std::string>& parameters)
    : CurveEditorDialog(parent, _L("Flow Compensation Model"),
        _L("Drag points on the graph or edit their extrusion length and flow correction factor in the table.\n"
           "Lengths and factors must strictly increase. The first length must be 0 and the last factor must be 1."),
        {_L("Extrusion length") + " (mm)", _L("Flow correction factor"), 1.0, "1.0", 0.0, 1.0},
        std::make_unique<SmallAreaInfillFlowCompensationModel>(parameters))
{}

} // namespace Slic3r::GUI
