#pragma once

#include <string_view>

#include "Font.hpp"
#include "Measure.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

// Text measured in a font; the current font when none is given.
class FontMeasure : public TextMeasure
{
public:
    FontMeasure();
    explicit FontMeasure(const Font& font) : m_font(font) {}
    float width(std::string_view text) const override { return m_font.width(text); }
    float line_height() const override { return m_font.size(); }
    float height(std::string_view text, float wrap_width) const override;

private:
    Font m_font;
};

// The wider of the regular and the bold face at the current size, for text such as a segment's option that
// turns bold when selected.
class ChoiceMeasure : public TextMeasure
{
public:
    ChoiceMeasure();
    float width(std::string_view text) const override;
    float line_height() const override { return m_regular.line_height(); }
    float height(std::string_view text, float wrap_width) const override;

private:
    FontMeasure m_regular;
    FontMeasure m_bold;
};

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
