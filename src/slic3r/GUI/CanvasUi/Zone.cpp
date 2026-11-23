#include "Zone.hpp"

#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>

#include "Context.hpp"
#include "ImGuiBridge.hpp"
#include "Types.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

ZoneState zone(const char* id, const Rect& rect)
{
    const ImGuiID item = ImGui::GetCurrentWindow()->GetID(id);
    const ImRect  bounds(rect.left, rect.top, rect.right, rect.bottom);
    ZoneState     state;
    if (ImGui::ItemAdd(bounds, item)) {
        state.pressed = ImGui::ButtonBehavior(bounds, item, &state.hovered, &state.held, ImGuiButtonFlags_PressedOnClick);
        if (state.pressed)
            Context::current()->request_frame();
    }
    state.released       = ImGui::IsItemDeactivated();
    state.double_clicked = state.pressed && Context::current()->double_click(true, point(ImGui::GetIO().MousePos));
    return state;
}

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
