#pragma once

#include <array>
#include <cstddef>
#include <string_view>

#include "Tokens.hpp"

struct ImFont;

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

enum class MonoStyle : unsigned char { Regular, Bold, Italic };
inline constexpr size_t MONO_STYLE_COUNT = 3;
// The monospaced sizes, in the order of MONO_FONT_SIZES: tags and line numbers, a position, code.
enum class MonoSize : unsigned char { Caption, Counter, Code };

// The faces the canvas has loaded for the toolkit; a missing one is drawn in the current font.
struct Fonts
{
    ImFont*                                                     bold{nullptr};
    std::array<ImFont*, MONO_STYLE_COUNT * MONO_FONT_SIZES.size()> mono{};
};

// A face of the interface's fonts at a size, as the context gives it to panels to measure and paint text with.
class Font
{
public:
    float size() const { return m_size; }
    Font  resized(float size) const { return Font(m_face, size); }
    float width(std::string_view text) const;
    float ascent() const;
    bool  has_glyphs(std::string_view text) const;

private:
    friend ImFont* face_of(const Font& font);
    friend class Context;
    Font(ImFont* face, float size) : m_face(face), m_size(size) {}

    ImFont* m_face;
    float   m_size;
};

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
