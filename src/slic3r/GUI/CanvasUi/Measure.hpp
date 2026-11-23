#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

struct Metrics;

inline bool is_utf8_continuation(char byte) { return (static_cast<unsigned char>(byte) & 0xC0) == 0x80; }

enum class ButtonSize : unsigned char { Default, Sm, Icon };

// The width of text in a font, so that sizes can be worked out, and tested, without drawing.
class TextMeasure
{
public:
    virtual ~TextMeasure() = default;
    virtual float width(std::string_view text) const = 0;
    virtual float line_height() const = 0;
    virtual float height(std::string_view text, float wrap_width) const = 0;
};

// Every character the same width, as in a monospaced font and in tests.
class MonospaceMeasure : public TextMeasure
{
public:
    MonospaceMeasure(float char_width, float line_height) : m_char_width(char_width), m_line_height(line_height) {}
    float width(std::string_view text) const override;
    float line_height() const override { return m_line_height; }
    float height(std::string_view text, float wrap_width) const override;

private:
    float m_char_width;
    float m_line_height;
};

// The text cut to width with an ellipsis when it does not fit, never inside a character.
std::string ellipsize(const TextMeasure& measure, std::string_view text, float width);

// The sizes of components, worked out without drawing: the components draw to them and panels lay out by
// them. A label may carry an ImGui id after "##".
std::string_view visible_label(std::string_view label);
float button_width(const Metrics& metrics, const TextMeasure& measure, std::string_view label, ButtonSize size = ButtonSize::Default,
                   bool with_icon = false);
float button_height(const Metrics& metrics, ButtonSize size = ButtonSize::Default);
// Segments take the measure of their options' text, which turns bold when selected.
float toggle_width(const Metrics& metrics, const TextMeasure& choices, std::string_view option);
float toggle_group_width(const Metrics& metrics, const TextMeasure& choices, const std::vector<std::string>& options);
// Taller than a control when the options do not fit side by side and go one under another.
float toggle_group_height(const Metrics& metrics, const TextMeasure& choices, const std::vector<std::string>& options, float width);
// Tabs take the measure of their labels' text, which turns bold when selected.
float tab_width(const Metrics& metrics, const TextMeasure& choices, std::string_view label);
float tabs_list_width(const Metrics& metrics, const TextMeasure& choices, const std::vector<std::string>& labels);
// The position field of current and total in its regular face, padding at each end.
float position_field_width(const TextMeasure& measure, uint64_t current, uint64_t total, float padding);
bool  field_fits_inline(const Metrics& metrics, const TextMeasure& measure, std::string_view label, float control_width, float width);
// A label over its control wraps at the row's width.
float field_toggle_group_height(const Metrics& metrics, const TextMeasure& measure, const TextMeasure& choices, std::string_view label,
                            const std::vector<std::string>& options, float width);
float switch_label_width(const Metrics& metrics, float width);
float field_switch_height(const Metrics& metrics, const TextMeasure& measure, std::string_view label, float width);
float field_switch_width(const Metrics& metrics, const TextMeasure& measure, std::string_view label);
// The height of components stacked in a card, the item gap between each two.
float stack_height(const Metrics& metrics, const std::vector<float>& heights);

struct RowSlot
{
    float width{0.f};
    // Takes a share of the room the other slots leave.
    bool  grow{false};
    // Gives up width, in proportion to its own, when the slots do not fit.
    bool  shrink{false};
};

struct RowSpan
{
    float left;
    float width;
    float right() const { return left + width; }
};

// The width of slots side by side, gap apart, the growing ones taking none.
float row_width(float gap, const std::vector<RowSlot>& slots);
// Slots laid out from left to right across [left, right], gap apart, the growing ones sharing what is left
// and the shrinking ones narrowed when the row is overfull.
std::vector<RowSpan> row_layout(float left, float right, float gap, const std::vector<RowSlot>& slots);

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
