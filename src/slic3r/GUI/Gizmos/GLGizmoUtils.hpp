#ifndef GL_GIZMO_UTIL_HPP
#define GL_GIZMO_UTIL_HPP

#include <map>
#include <vector>
#include <string>
#include <wx/string.h>
#include "imgui.h"

namespace Slic3r::GUI {

// Forward declaration
class ImGuiWrapper;
class GLCanvas3D;

namespace GLGizmoUtils {

// Renders a tooltip button using the provided shortcuts
void render_tooltip_button(
    ImGuiWrapper* imgui_wrapper, const GLCanvas3D& canvas, const std::vector<std::pair<wxString, wxString>>& shortcuts, float x, float y);

} // namespace GLGizmoUtils
} // namespace Slic3r::GUI

#endif // GL_GIZMO_UTIL_HPP