#include "Font.hpp"

#include <cfloat>
#include <string_view>

#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

float Font::width(std::string_view text) const
{
    return m_face->CalcTextSizeA(m_size, FLT_MAX, 0.f, text.data(), text.data() + text.size()).x;
}

float Font::ascent() const { return m_face->Ascent * m_size / m_face->FontSize; }

bool Font::has_glyphs(std::string_view text) const
{
    for (const char *c = text.data(), *end = text.data() + text.size(); c < end;) {
        unsigned int codepoint = 0;
        c += ImTextCharFromUtf8(&codepoint, c, end);
        if (m_face->FindGlyphNoFallback(ImWchar(codepoint)) == nullptr)
            return false;
    }
    return true;
}

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
