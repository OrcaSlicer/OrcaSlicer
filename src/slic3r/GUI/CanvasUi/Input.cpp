#include "Input.hpp"

#include <string>

#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>

#include "Context.hpp"
#include "ImGuiBridge.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

Pointer pointer()
{
    const ImGuiIO& io = ImGui::GetIO();
    Pointer        p;
    p.pos             = point(io.MousePos);
    p.delta           = point(io.MouseDelta);
    p.pressed         = io.MouseClicked[ImGuiMouseButton_Left];
    p.held            = io.MouseDown[ImGuiMouseButton_Left];
    p.context_pressed = io.MouseClicked[ImGuiMouseButton_Right];
    p.over_panel    = GImGui->HoveredWindow != nullptr;
    p.alt             = io.KeyAlt;
    p.shift           = io.KeyShift;
    p.double_clicked  = Context::current() != nullptr && Context::current()->double_click(p.pressed, p.pos);
    p.released        = io.MouseReleased[ImGuiMouseButton_Left];
    return p;
}

void capture_mouse() { ImGui::CaptureMouseFromApp(); }

bool escape_pressed() { return ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Escape)); }

bool input_active() { return GImGui->ActiveId != 0 || GImGui->ActiveIdPreviousFrame != 0; }

void copy_to_clipboard(const std::string& text) { ImGui::SetClipboardText(text.c_str()); }

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
