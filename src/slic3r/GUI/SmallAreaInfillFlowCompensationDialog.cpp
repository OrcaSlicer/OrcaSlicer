#include "SmallAreaInfillFlowCompensationDialog.hpp"

#include <cmath>
#include <memory>
#include <string>
#include <vector>
#include "CurveEditorDialog.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/GCode/SmallAreaInfillFlowCompensationModel.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"

namespace Slic3r::GUI {
namespace {

double printer_bed_diagonal()
{
    if (!wxGetApp().preset_bundle) return 0.0;
    const auto& config = wxGetApp().preset_bundle->printers.get_edited_preset().config;
    const auto* area = config.option<ConfigOptionPoints>("printable_area");
    if (!area || area->values.empty()) return 0.0;
    for (const auto& point : area->values)
        if (!std::isfinite(point.x()) || !std::isfinite(point.y())) return 0.0;
    const BoundingBoxf bounds(area->values);
    if (!bounds.defined) return 0.0;
    const Vec2d size = bounds.size();
    return std::hypot(size.x(), size.y());
}

} // namespace

SmallAreaInfillFlowCompensationDialog::SmallAreaInfillFlowCompensationDialog(
    wxWindow* parent, const std::vector<std::string>& parameters)
    : CurveEditorDialog(parent, _L("Flow Compensation Model"),
        _L("Drag points on the graph or edit their extrusion length and flow correction factor in the table.\n"
           "Lengths and factors must strictly increase. The first length must be 0 and the last factor must be 1."),
        {_L("Extrusion length") + " (mm)", _L("Flow correction factor"), 1.0, "1.0", 0.0, 1.0},
        std::make_unique<SmallAreaInfillFlowCompensationModel>(parameters, printer_bed_diagonal()),
        "small_area_infill_flow_compensation_editor")
{}

} // namespace Slic3r::GUI
