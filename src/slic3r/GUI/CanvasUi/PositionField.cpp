#include "PositionField.hpp"

#include <cstdint>
#include <optional>
#include <string>

#include <imgui/imgui.h>

#include "ComponentParts.hpp"
#include "Context.hpp"
#include "FontMeasure.hpp"
#include "ImGuiBridge.hpp"
#include "Measure.hpp"
#include "Metrics.hpp"
#include "Theme.hpp"
#include "Types.hpp"
#include "Font.hpp"
#include "NumberField.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

using namespace detail;

std::optional<long long> position_field(const char* id, PositionField& field, uint64_t current, uint64_t total,
                                        const PositionFieldOptions& options)
{
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + options.size.width, min.y + options.size.height);
    std::optional<long long> confirmed;
    const int                frame = Context::current()->frame();
    if (field.editing && !field.focus && field.drawn_frame != frame - 1)
        field.editing = false;
    field.drawn_frame = frame;
    ImGui::PushID(id);
    if (field.editing) {
        ImGui::PushFont(face_of(options.regular));
        if (field.focus) {
            // The focus takes hold in the next frame; nothing else would ask for it before the user moves.
            ImGui::SetKeyboardFocusHere();
            Context::current()->request_frame();
        }
        field.focus = false;
        if (number_field("##field", field.value, {options.size.width})) {
            confirmed     = field.value;
            field.editing = false;
        } else if (ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Escape)) || ImGui::IsItemDeactivated())
            field.editing = false;
        ImGui::PopFont();
        if (!field.editing)
            Context::current()->request_frame();
    } else {
        const Hit h = hit("##button", im(options.size), {});
        if (h.clicked)
            field = PositionField{true, true, (long long) current, frame};
        ImDrawList& list = *ImGui::GetWindowDrawList();
        if (h.hovered)
            fill_rounded(list, min, max, u32(theme().accent), metrics().icon_button_radius);
        const std::string now   = std::to_string(current);
        const std::string all   = " / " + std::to_string(total);
        const float       all_x = max.x - options.padding - options.regular.width(all);
        const float       y     = min.y + .5f * (options.size.height - options.regular.size());
        list.AddText(face_of(options.regular), options.regular.size(), ImVec2(all_x, y), u32(muted_text(theme(), h.hovered)), all.c_str());
        list.AddText(face_of(options.bold), options.regular.size(), ImVec2(all_x - options.regular.width(now), y), u32(theme().foreground), now.c_str());
    }
    ImGui::PopID();
    return confirmed;
}

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
