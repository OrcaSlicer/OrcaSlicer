#include "ScrollArea.hpp"

#include <imgui/imgui.h>

#include "Context.hpp"
#include "ImGuiBridge.hpp"
#include "Metrics.hpp"
#include "Paint.hpp"
#include "Theme.hpp"
#include "Types.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

ScrollArea::ScrollArea(const char* id, const Size& size, float content_height)
{
    const Context& ui = *Context::current();
    const Theme&   t  = ui.theme();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.f, 0.f, 0.f, 0.f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0.f, 0.f, 0.f, 0.f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, im(t.scrollbar_thumb));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, im(t.scrollbar_thumb_hover));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, im(t.scrollbar_thumb_active));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, ui.metrics().scrollbar);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, .5f * ui.metrics().scrollbar);
    ImGui::BeginChild(id, im(size), false, ImGuiWindowFlags_NoSavedSettings);
    ImGui::Dummy(ImVec2(0.f, content_height));
    ImGui::SetCursorPos(ImVec2(0.f, 0.f));
    m_origin = point(ImGui::GetCursorScreenPos());
    m_list   = ImGui::GetWindowDrawList();
}

Painter ScrollArea::painter() const { return Painter(*m_list); }

ScrollArea::~ScrollArea()
{
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(5);
}

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
