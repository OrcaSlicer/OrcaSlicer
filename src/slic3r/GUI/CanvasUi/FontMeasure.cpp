#include "FontMeasure.hpp"

#include <algorithm>
#include <cfloat>
#include <string_view>

#include <imgui/imgui.h>

#include "Context.hpp"
#include "ImGuiBridge.hpp"
#include "Font.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

FontMeasure::FontMeasure() : m_font(Context::current()->regular_font()) {}

float FontMeasure::height(std::string_view text, float wrap_width) const
{
    return face_of(m_font)->CalcTextSizeA(m_font.size(), FLT_MAX, wrap_width, text.data(), text.data() + text.size()).y;
}

ChoiceMeasure::ChoiceMeasure() : m_bold(Context::current()->bold_font().resized(ImGui::GetFontSize())) {}

float ChoiceMeasure::width(std::string_view text) const { return std::max(m_regular.width(text), m_bold.width(text)); }

float ChoiceMeasure::height(std::string_view text, float wrap_width) const
{
    return std::max(m_regular.height(text, wrap_width), m_bold.height(text, wrap_width));
}

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
