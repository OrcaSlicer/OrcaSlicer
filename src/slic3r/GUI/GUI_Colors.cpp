#include "GUI_Colors.hpp"
#include "imgui/imgui.h"


namespace Slic3r{

ImVec4 RenderColor::colors[RenderCol_Count] = { };

const char* GetRenderColName(RenderCol idx)
{
    switch (idx)
    {
    case RenderCol_3D_Background: return "3D Background";
    case RenderCol_Plate_Unselected: return "Plate Unselected";
    case RenderCol_Plate_Selected: return "Plate Selected";
    case RenderCol_Plate_Default: return "Plate Default";
    case RenderCol_Plate_Line_Top: return "Plate Line Top";
    case RenderCol_Plate_Line_Bottom: return "Plate Line Bottom";
    case RenderCol_Model_Disable: return "Model Disable";
    case RenderCol_Model_Unprintable: return "Model Unprintable";
    case RenderCol_Model_Neutral: return "Model Neutral";
    case RenderCol_Part: return "Part";
    case RenderCol_Modifier: return "Modifier";
    case RenderCol_Negtive_Volume: return "Negtive Volume";
    case RenderCol_Support_Enforcer: return "Support Enforcer";
    case RenderCol_Support_Blocker: return "Support Blocker";
    case RenderCol_Axis_X: return "Axis X";
    case RenderCol_Axis_Y: return "Axis Y";
    case RenderCol_Axis_Z: return "Axis Z";
    case RenderCol_Grabber_X: return "Grabber X";
    case RenderCol_Grabber_Y: return "Grabber Y";
    case RenderCol_Grabber_Z: return "Grabber Z";
    case RenderCol_Flatten_Plane: return "Flatten Plane";
    case RenderCol_Flatten_Plane_Hover: return "Flatten Plane Hover";
    }
    return "Unknown";
}

}
