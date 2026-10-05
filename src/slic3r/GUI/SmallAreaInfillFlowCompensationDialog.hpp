#pragma once

#include <string>
#include <vector>
#include "CurveEditorDialog.hpp"

namespace Slic3r::GUI {

class SmallAreaInfillFlowCompensationDialog : public CurveEditorDialog
{
public:
    SmallAreaInfillFlowCompensationDialog(wxWindow* parent, const std::vector<std::string>& parameters);
};

} // namespace Slic3r::GUI
