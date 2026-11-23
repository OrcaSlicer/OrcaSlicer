#include "Layout.hpp"

#include <imgui/imgui.h>

#include "ImGuiBridge.hpp"
#include "Types.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

void place(const Point& pos) { ImGui::SetCursorScreenPos(im(pos)); }

Group::Group() { ImGui::BeginGroup(); }

Group::~Group() { ImGui::EndGroup(); }

ScopedId::ScopedId(int id) { ImGui::PushID(id); }

ScopedId::~ScopedId() { ImGui::PopID(); }

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
