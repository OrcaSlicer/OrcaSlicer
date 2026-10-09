#include "SmallAreaInfillFlowCompensationDialog.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include <wx/defs.h>
#include <wx/display.h>
#include <wx/gdicmn.h>
#include "CurveEditorDialog.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "libslic3r/GCode/SmallAreaInfillFlowCompensationModel.hpp"

namespace Slic3r::GUI {
namespace {

constexpr const char* window_name = "small_area_infill_flow_compensation_editor";

} // namespace

SmallAreaInfillFlowCompensationDialog::SmallAreaInfillFlowCompensationDialog(
    wxWindow* parent, const std::vector<std::string>& parameters)
    : CurveEditorDialog(parent, _L("Flow Compensation Model"),
        _L("Drag points on the graph or edit their extrusion length and flow correction factor in the table.\n"
           "Lengths and factors must strictly increase. The first length must be 0 and the last factor must be 1."),
        {_L("Extrusion length") + " (mm)", _L("Flow correction factor"), 1.0, "1.0", 0.0, 1.0},
        std::make_unique<SmallAreaInfillFlowCompensationModel>(parameters))
{
    if (wxGetApp().window_pos_restore(this, window_name)) {
        wxSize size = GetSize();
        size.IncTo(GetMinSize());
        SetSize(size);
        wxGetApp().window_pos_sanitize(this);
        // Sanitizing can shrink the requested rect below the enforced minimum size.
        // Move the actual window fully onto the display after that size is enforced.
        const int display_index = wxDisplay::GetFromWindow(this);
        const wxRect area = wxDisplay(display_index == wxNOT_FOUND ? 0u : static_cast<unsigned>(display_index)).GetClientArea();
        const wxRect rect = GetScreenRect();
        Move(wxPoint(std::clamp(rect.x, area.x, std::max(area.x, area.GetRight() + 1 - rect.width)),
                     std::clamp(rect.y, area.y, std::max(area.y, area.GetBottom() + 1 - rect.height))));
    }
}

SmallAreaInfillFlowCompensationDialog::~SmallAreaInfillFlowCompensationDialog()
{
    // Modal OK/Cancel/Escape hide the window without sending a close event.
    wxGetApp().window_pos_save(this, window_name);
}

} // namespace Slic3r::GUI
