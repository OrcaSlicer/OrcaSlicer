#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>

#include "slic3r/GUI/CanvasUi/Availability.hpp"
#include "slic3r/GUI/CanvasUi/Button.hpp"
#include "slic3r/GUI/CanvasUi/Field.hpp"
#include "slic3r/GUI/CanvasUi/NumberField.hpp"
#include "slic3r/GUI/CanvasUi/PositionField.hpp"
#include "slic3r/GUI/CanvasUi/Tabs.hpp"
#include "slic3r/GUI/CanvasUi/ToggleGroup.hpp"
#include "slic3r/GUI/CanvasUi/CanvasFrame.hpp"
#include "slic3r/GUI/CanvasUi/Context.hpp"
#include "slic3r/GUI/CanvasUi/Corners.hpp"
#include "slic3r/GUI/CanvasUi/Font.hpp"
#include "slic3r/GUI/CanvasUi/Tokens.hpp"
#include "slic3r/GUI/CanvasUi/FontMeasure.hpp"
#include "slic3r/GUI/CanvasUi/Icons.hpp"
#include "slic3r/GUI/CanvasUi/ImGuiBridge.hpp"
#include "slic3r/GUI/CanvasUi/Input.hpp"
#include "slic3r/GUI/CanvasUi/Layout.hpp"
#include "slic3r/GUI/CanvasUi/Measure.hpp"
#include "slic3r/GUI/CanvasUi/Menu.hpp"
#include "slic3r/GUI/CanvasUi/Metrics.hpp"
#include "slic3r/GUI/CanvasUi/MenuPopup.hpp"
#include "slic3r/GUI/CanvasUi/Paint.hpp"
#include "slic3r/GUI/CanvasUi/Card.hpp"
#include "slic3r/GUI/CanvasUi/Collapsible.hpp"
#include "slic3r/GUI/CanvasUi/ScrollArea.hpp"
#include "slic3r/GUI/CanvasUi/Theme.hpp"
#include "slic3r/GUI/CanvasUi/Types.hpp"

#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r::GUI;
using Catch::Matchers::WithinAbs;

TEST_CASE("Contrast runs from 1 for equal colors to 21 for black on white", "[CanvasUi][Theme]")
{
    CHECK_THAT(CanvasUi::contrast_ratio(CanvasUi::Color(1, 1, 1), CanvasUi::Color(0, 0, 0)), WithinAbs(21.0, 0.01));
    CHECK_THAT(CanvasUi::contrast_ratio(CanvasUi::Color(.5f, .5f, .5f), CanvasUi::Color(.5f, .5f, .5f)), WithinAbs(1.0, 0.001));
}

namespace {

ImU32 to_u32(const CanvasUi::Color& c) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, c.a)); }

} // namespace

TEST_CASE("A colour made of bytes gives back the same bytes", "[CanvasUi][Theme]")
{
    for (int b = 0; b < 256; ++b) {
        const uint8_t byte = uint8_t(b);
        CHECK(to_u32(CanvasUi::Color::from_bytes(byte, byte, byte)) == IM_COL32(b, b, b, 255));
        CHECK(to_u32(CanvasUi::rgb((b << 16) | (b << 8) | b, float(b) / 255.f)) == IM_COL32(b, b, b, b));
    }
}

TEST_CASE("Mixing and fading land on whole colour bytes", "[CanvasUi][Theme]")
{
    CHECK(to_u32(CanvasUi::mix(CanvasUi::rgb(0x000000), CanvasUi::rgb(0xffffff), .5f)) == IM_COL32(128, 128, 128, 255));
    const CanvasUi::Color mixed = CanvasUi::mix(CanvasUi::rgb(0x3d3d43), CanvasUi::rgb(0xff7a8c), .3f);
    CHECK(to_u32(mixed) == IM_COL32(119, 79, 89, 255));
    CHECK_THAT(mixed.r, WithinAbs(119.f / 255.f, 1e-6));
    CHECK(to_u32(CanvasUi::faded(CanvasUi::rgb(0x009688, .45f), .5f)) == IM_COL32(0x00, 0x96, 0x88, 58));
    CHECK(to_u32(CanvasUi::faded(CanvasUi::rgb(0x009688, .45f), 1.5f)) == IM_COL32(0x00, 0x96, 0x88, 115));
    CHECK(to_u32(CanvasUi::faded(CanvasUi::rgb(0x009688), 0.f)) == IM_COL32(0x00, 0x96, 0x88, 0));
}

TEST_CASE("Text on a preview panel reads at 4.5:1 or more", "[CanvasUi][Theme]")
{
    const bool          dark  = GENERATE(false, true);
    const CanvasUi::Theme& theme = CanvasUi::theme(dark);

    CHECK(CanvasUi::contrast_ratio(theme.foreground, theme.card) >= 4.5f);
    CHECK(CanvasUi::contrast_ratio(theme.muted_foreground, theme.card) >= 4.5f);
    CHECK(CanvasUi::contrast_ratio(theme.inactive_foreground, CanvasUi::blend_over(theme.secondary, theme.card)) >= 4.5f);
    CHECK(CanvasUi::contrast_ratio(theme.inactive_foreground, theme.tabs_list) >= 4.5f);
    CHECK(CanvasUi::contrast_ratio(theme.primary_strong, theme.card) >= 4.5f);
    CHECK(CanvasUi::contrast_ratio(theme.foreground, CanvasUi::blend_over(theme.selection, theme.card)) >= 4.5f);
    CHECK(CanvasUi::contrast_ratio(theme.tooltip_foreground, theme.tooltip) >= 4.5f);
    CHECK(CanvasUi::contrast_ratio(CanvasUi::blend_over(theme.tooltip_muted_foreground, theme.tooltip), theme.tooltip) >= 4.5f);
    const CanvasUi::Color warning_surface = CanvasUi::blend_over(theme.warning_surface, theme.card);
    CHECK(CanvasUi::contrast_ratio(theme.foreground, warning_surface) >= 4.5f);
    CHECK(CanvasUi::contrast_ratio(theme.warning, warning_surface) >= 3.f);
}

TEST_CASE("On a hovered surface text reads at 4.5:1 and marks at 3:1", "[CanvasUi][Theme]")
{
    const bool             dark  = GENERATE(false, true);
    const CanvasUi::Theme& theme = CanvasUi::theme(dark);
    const CanvasUi::Color  hover = CanvasUi::blend_over(theme.accent, theme.card);

    CHECK(CanvasUi::contrast_ratio(theme.foreground, hover) >= 4.5f);
    CHECK(CanvasUi::contrast_ratio(theme.primary_strong, hover) >= 3.f);
    CHECK(CanvasUi::contrast_ratio(theme.muted_foreground, hover) >= 3.f);
}

TEST_CASE("What shows the state of a control stands out at 3:1 or more", "[CanvasUi][Theme]")
{
    const bool             dark  = GENERATE(false, true);
    const CanvasUi::Theme& theme = CanvasUi::theme(dark);
    const CanvasUi::Color  track = CanvasUi::blend_over(theme.secondary, theme.card);

    CHECK(CanvasUi::contrast_ratio(theme.input, theme.card) >= 3.f);
    CHECK(CanvasUi::contrast_ratio(theme.input, track) >= 3.f);
    CHECK(CanvasUi::contrast_ratio(theme.toggle_pressed_border, track) >= 3.f);
    CHECK(CanvasUi::contrast_ratio(theme.switch_thumb, theme.input) >= 3.f);
    CHECK(CanvasUi::contrast_ratio(theme.primary, theme.card) >= 3.f);
    CHECK(CanvasUi::contrast_ratio(theme.primary, track) >= 3.f);
    CHECK(CanvasUi::contrast_ratio(theme.card, theme.primary) >= 3.f);
}

TEST_CASE("Toolkit sizes follow the scale of the interface", "[CanvasUi]")
{
    const CanvasUi::Metrics normal(1.f);
    const CanvasUi::Metrics doubled(2.f);

    CHECK_THAT(doubled.card_radius, WithinAbs(2. * normal.card_radius, 1e-6));
    CHECK_THAT(doubled.menu_row, WithinAbs(2. * normal.menu_row, 1e-6));
    CHECK_THAT(doubled.icon_button, WithinAbs(2. * normal.icon_button, 1e-6));
    CHECK_THAT(doubled.outline, WithinAbs(normal.outline, 1e-6));
}

TEST_CASE("Row heights are whole pixels at any scale", "[CanvasUi]")
{
    const float          scale = GENERATE(1.f, 1.25f, 1.5f, 1.75f, 2.f);
    const CanvasUi::Metrics m(scale);

    for (const float height : {m.menu_row, m.item_gap, m.control_height, m.small_button, m.toggle_gap, m.field_height, m.field_label_gap,
                               m.field_stack_bottom, m.tabs_list_height, m.tab_height, m.switch_text_padding, m.menu_group_label,
                               m.menu_separator})
        CHECK_THAT(height, WithinAbs(std::round(height), 1e-6));
}

TEST_CASE("A menu is as wide as its widest row, key included, and as tall as its rows", "[CanvasUi]")
{
    using CanvasUi::MenuItem;
    const CanvasUi::Metrics          m(1.f);
    const CanvasUi::MonospaceMeasure measure(7.f, 14.f);
    const auto size = [&](const std::vector<MenuItem>& items) { return CanvasUi::menu_size(m, measure, items); };
    const CanvasUi::Size top = size({MenuItem::item(1, "Top")});

    CHECK_THAT(size({MenuItem::item(1, "Top", "Ctrl+1")}).width - top.width, WithinAbs(m.menu_key_gap + 6 * 7.f, 1.));
    CHECK_THAT(size({MenuItem::item(1, "Top"), MenuItem::item(0, "Isometric")}).width, WithinAbs(size({MenuItem::item(0, "Isometric")}).width, 1e-4));
    CHECK_THAT(size({MenuItem::item(1, "Top"), MenuItem::item(0, "Isometric")}).height - top.height, WithinAbs(m.menu_row, 1.));
    CHECK_THAT(size({MenuItem::item(1, "Top"), MenuItem::separator()}).height - top.height, WithinAbs(m.menu_separator, 1.));
    CHECK_THAT(size({MenuItem::item(1, "Top"), MenuItem::group_label("S")}).height - top.height, WithinAbs(m.menu_group_label, 1.));

    CHECK(CanvasUi::menu_size(m, measure, {MenuItem::group_label("A long section title"), MenuItem::item(0, "A")}).width >
          CanvasUi::menu_size(m, measure, {MenuItem::item(0, "A")}).width);
    CHECK_THAT(CanvasUi::menu_size(m, measure, {MenuItem::item(0, "A")}, 300.f).width, WithinAbs(300.f, 1e-4));
}

TEST_CASE("A menu keeps the column of indicators only when one of its items has an indicator", "[CanvasUi]")
{
    using CanvasUi::MenuItem;
    const CanvasUi::Metrics m(1.f);
    CHECK_THAT(CanvasUi::menu_text_left(m, {MenuItem::item(0, "Copy line"), MenuItem::item(1, "Copy visible lines")}), WithinAbs(m.menu_text_inset, 1e-6));
    CHECK_THAT(CanvasUi::menu_text_left(m, {MenuItem::item(0, "Fit view"), MenuItem::radio_item(1, "Isometric", false)}), WithinAbs(m.menu_text_left, 1e-6));
    CHECK_THAT(CanvasUi::menu_text_left(m, {MenuItem::checkbox_item(0, "Background", false)}), WithinAbs(m.menu_text_left, 1e-6));
}

TEST_CASE("A menu is a whole number of pixels across and down at any scale", "[CanvasUi]")
{
    using CanvasUi::MenuItem;
    const CanvasUi::MonospaceMeasure measure(7.3f, 14.f);
    for (const float scale : {1.f, 1.25f, 1.5f, 1.75f, 2.f}) {
        const CanvasUi::Size size = CanvasUi::menu_size(CanvasUi::Metrics(scale), measure,
                                                        {MenuItem::radio_item(0, "Isometric", true), MenuItem::separator(), MenuItem::item(1, "Top", "Ctrl+1")});
        CHECK_THAT(size.width, WithinAbs(std::round(size.width), 1e-6));
        CHECK_THAT(size.height, WithinAbs(std::round(size.height), 1e-6));
    }
}

TEST_CASE("A menu opens its preferred way, flips when only the other way fits, and stays on screen", "[CanvasUi]")
{
    const CanvasUi::PositionerAxis right_of{100.f, 40.f, true};
    CHECK_THAT(CanvasUi::position_popup_axis(right_of, 50.f, 0.f, 500.f), WithinAbs(100.f, 1e-4));
    CHECK_THAT(CanvasUi::position_popup_axis(right_of, 30.f, 0.f, 120.f), WithinAbs(10.f, 1e-4));
    CHECK_THAT(CanvasUi::position_popup_axis(right_of, 80.f, 0.f, 150.f), WithinAbs(70.f, 1e-4));

    const CanvasUi::PositionerAxis above{10.f, 300.f, false};
    CHECK_THAT(CanvasUi::position_popup_axis(above, 100.f, 0.f, 500.f), WithinAbs(200.f, 1e-4));
    CHECK_THAT(CanvasUi::position_popup_axis(above, 100.f, 250.f, 500.f), WithinAbs(250.f, 1e-4));
    CHECK_THAT(CanvasUi::position_popup_axis(CanvasUi::PositionerAxis{20.f, 50.f, false}, 100.f, 0.f, 500.f), WithinAbs(20.f, 1e-4));

    CHECK_THAT(CanvasUi::position_popup_axis(CanvasUi::Positioner::point(480.f, 0.f).x, 60.f, 0.f, 500.f), WithinAbs(420.f, 1e-4));
    const CanvasUi::Positioner beside = CanvasUi::Positioner::beside(CanvasUi::Rect(10.f, 100.f, 60.f, 140.f), 8.f);
    CHECK_THAT(CanvasUi::position_popup_axis(beside.x, 50.f, 0.f, 500.f), WithinAbs(68.f, 1e-4));
    CHECK_THAT(CanvasUi::position_popup_axis(beside.y, 30.f, 0.f, 500.f), WithinAbs(110.f, 1e-4));
    CHECK_THAT(CanvasUi::position_popup_axis(right_of, 900.f, 0.f, 500.f), WithinAbs(0.f, 1e-4));
}

TEST_CASE("A value on the right of an item widens the menu as a key does", "[CanvasUi]")
{
    using CanvasUi::MenuItem;
    const CanvasUi::Metrics          m(1.f);
    const CanvasUi::MonospaceMeasure measure(7.f, 14.f);
    MenuItem with_value = MenuItem::item(0, "Speed");
    with_value.value    = "10 - 200 mm/s";
    MenuItem with_key   = MenuItem::item(0, "Speed", "10 - 200 mm/s");

    const float plain = CanvasUi::menu_size(m, measure, {MenuItem::item(0, "Speed")}).width;
    CHECK_THAT(CanvasUi::menu_size(m, measure, {with_value}).width - plain, WithinAbs(m.menu_key_gap + 13 * 7.f, 1.f));
    CHECK_THAT(CanvasUi::menu_size(m, measure, {with_value}).width, WithinAbs(CanvasUi::menu_size(m, measure, {with_key}).width, 1e-6));
}

TEST_CASE("Text too wide is cut with an ellipsis and fits", "[CanvasUi]")
{
    const CanvasUi::MonospaceMeasure measure(7.f, 14.f);
    CHECK(CanvasUi::ellipsize(measure, "Outer wall", 100.f) == "Outer wall");
    const std::string cut = CanvasUi::ellipsize(measure, "Внутрішнє суцільне заповнення", 100.f);
    CHECK(measure.width(cut) <= 100.f);
    CHECK(cut.size() > 3);
    CHECK(cut.substr(cut.size() - 3) == "\xE2\x80\xA6");
}

TEST_CASE("A toggle group is sized by its options' widest face, side by side or one under another", "[CanvasUi]")
{
    const CanvasUi::Metrics           m(1.f);
    const CanvasUi::MonospaceMeasure  choices(8.f, 14.f);
    const std::vector<std::string> options = {"Clip", "Wrap##long"};
    const float natural = 3.f * m.toggle_gap + 2.f * (4 * 8.f + 2.f * m.toggle_padding);

    CHECK_THAT(CanvasUi::toggle_width(m, choices, "Wrap##long"), WithinAbs(4 * 8.f + 2.f * m.toggle_padding, 1e-4));
    CHECK_THAT(CanvasUi::toggle_group_width(m, choices, options), WithinAbs(natural, 1e-4));
    CHECK_THAT(CanvasUi::toggle_group_height(m, choices, options, natural), WithinAbs(m.control_height, 1e-4));
    CHECK_THAT(CanvasUi::toggle_group_height(m, choices, options, natural - 1.f),
               WithinAbs(2.f * (m.control_height - m.toggle_gap) + m.toggle_gap, 1e-4));
}

TEST_CASE("A field puts its control under the label only when both do not fit, the label then wrapping", "[CanvasUi]")
{
    const CanvasUi::Metrics           m(1.f);
    const CanvasUi::MonospaceMeasure  measure(7.f, 14.f);
    const CanvasUi::MonospaceMeasure  choices(8.f, 14.f);
    const std::vector<std::string> options = {"A", "B"};
    const float control = CanvasUi::toggle_group_width(m, choices, options);
    const float wide    = 5 * 7.f + m.field_gap + control;

    CHECK_THAT(CanvasUi::field_toggle_group_height(m, measure, choices, "Lines", options, wide), WithinAbs(m.field_height, 1e-4));
    CHECK_THAT(CanvasUi::field_toggle_group_height(m, measure, choices, "Lines", options, wide - 1.f),
               WithinAbs(14.f + m.field_label_gap + m.control_height + m.field_stack_bottom, 1e-4));
    // Ten characters a line: "A label" and "that wraps".
    CHECK_THAT(CanvasUi::field_toggle_group_height(m, measure, choices, "A label that wraps", options, 70.f),
               WithinAbs(2 * 14.f + m.field_label_gap + m.control_height + m.field_stack_bottom, 1e-4));

    const float switch_width = 10 * 7.f + m.field_gap + m.switch_width;
    CHECK_THAT(CanvasUi::switch_label_width(m, switch_width), WithinAbs(70.f, 1e-4));
    CHECK_THAT(CanvasUi::field_switch_height(m, measure, "Short", switch_width), WithinAbs(m.field_height, 1e-4));
    CHECK_THAT(CanvasUi::field_switch_height(m, measure, "Some words that wrap onto more lines", switch_width),
               WithinAbs(std::max(m.field_height, 4 * 14.f + 2.f * m.switch_text_padding), 1e-4));
}

TEST_CASE("A switch as wide as its width function keeps its label on one line", "[CanvasUi]")
{
    const float                      scale = GENERATE(1.f, 1.5f);
    const CanvasUi::Metrics          m(scale);
    const CanvasUi::MonospaceMeasure measure(7.f * scale, 14.f * scale);
    const std::string                label = "Include travel moves in the scale##travel";
    const float                      width = CanvasUi::field_switch_width(m, measure, label);

    CHECK(width >= measure.width("Include travel moves in the scale") + m.field_gap + m.switch_width);
    CHECK_THAT(CanvasUi::field_switch_height(m, measure, label, width), WithinAbs(m.field_height, 1e-4));
}

TEST_CASE("Stacked components are the item gap apart", "[CanvasUi]")
{
    const CanvasUi::Metrics m(2.f);
    CHECK_THAT(CanvasUi::stack_height(m, {10.f, 20.f, 30.f}), WithinAbs(60.f + 2.f * m.item_gap, 1e-4));
    CHECK_THAT(CanvasUi::stack_height(m, {}), WithinAbs(0.f, 1e-6));
}

TEST_CASE("A row keeps its slots the gap apart and gives the growing one what is left", "[CanvasUi]")
{
    const std::vector<CanvasUi::RowSpan> spans = CanvasUi::row_layout(10.f, 210.f, 4.f, {{24.f}, {50.f}, {0.f, true}, {60.f}, {24.f}});
    REQUIRE(spans.size() == 5);
    CHECK_THAT(spans[1].left, WithinAbs(spans[0].right() + 4.f, 1e-4));
    CHECK_THAT(spans[2].width, WithinAbs(200.f - 24.f - 50.f - 60.f - 24.f - 4.f * 4.f, 1e-4));
    CHECK_THAT(spans[4].left, WithinAbs(spans[3].right() + 4.f, 1e-4));
    CHECK_THAT(spans[4].right(), WithinAbs(210.f, 1e-4));
    CHECK_THAT(CanvasUi::row_width(4.f, {{24.f}, {50.f}, {0.f, true}, {60.f}, {24.f}}), WithinAbs(158.f + 4.f * 4.f, 1e-4));
}

TEST_CASE("An overfull row narrows the slots allowed to shrink and keeps the rest within it", "[CanvasUi]")
{
    CanvasUi::RowSlot title{120.f};
    title.shrink = true;
    const std::vector<CanvasUi::RowSpan> spans = CanvasUi::row_layout(0.f, 200.f, 4.f, {{24.f}, title, {0.f, true}, {80.f}, {24.f}});
    CHECK_THAT(spans[1].width, WithinAbs(200.f - 24.f - 80.f - 24.f - 4.f * 4.f, 1e-4));
    CHECK_THAT(spans[2].width, WithinAbs(0.f, 1e-4));
    CHECK_THAT(spans[4].right(), WithinAbs(200.f, 1e-4));

    const std::vector<CanvasUi::RowSpan> fitting = CanvasUi::row_layout(0.f, 400.f, 4.f, {{24.f}, title, {0.f, true}, {80.f}, {24.f}});
    CHECK_THAT(fitting[1].width, WithinAbs(120.f, 1e-4));
}

TEST_CASE("Tabs are as wide as their widest face, the gap apart", "[CanvasUi]")
{
    const CanvasUi::Metrics          m(1.f);
    const CanvasUi::MonospaceMeasure choices(8.f, 14.f);
    CHECK_THAT(CanvasUi::tab_width(m, choices, "View"), WithinAbs(4 * 8.f + 2.f * m.tab_padding, 1e-4));
    CHECK_THAT(CanvasUi::tabs_list_width(m, choices, {"View", "Highlighting"}),
               WithinAbs(16 * 8.f + 4.f * m.tab_padding + m.tab_gap, 1e-4));
}

TEST_CASE("A position field is as wide as its numbers and its padding", "[CanvasUi]")
{
    const CanvasUi::MonospaceMeasure measure(7.f, 14.f);
    CHECK_THAT(CanvasUi::position_field_width(measure, 374353, 375140, 6.f), WithinAbs(6.f * 7.f + 9.f * 7.f + 12.f, 1e-4));
}

TEST_CASE("A number field takes a whole decimal number with the minus only in front", "[CanvasUi]")
{
    CHECK(CanvasUi::parse_integer("120") == 120);
    CHECK(CanvasUi::parse_integer("-7") == -7);
    CHECK(CanvasUi::parse_integer("9223372036854775807") == 9223372036854775807LL);
    for (const char* rejected : {"", "-", "1-2", "12-", "--3", "+4", " 5", "6 ", "9223372036854775808", "-9223372036854775809"})
        CHECK_FALSE(CanvasUi::parse_integer(rejected).has_value());
}

TEST_CASE("A decimal field takes a point or a comma before the fraction", "[CanvasUi]")
{
    for (const auto& [text, value] : {std::pair<const char*, double>{"4.5", 4.5}, {"4,5", 4.5}, {"-0.25", -0.25}, {".5", 0.5}, {"120", 120.0}}) {
        CAPTURE(text);
        const std::optional<double> parsed = CanvasUi::parse_decimal(text);
        REQUIRE(parsed.has_value());
        CHECK_THAT(*parsed, WithinAbs(value, 1e-12));
    }
    for (const char* rejected : {"", "-", "1.2.3", "1,2.3", "+4", " 5", "6 ", "1-2"})
        CHECK_FALSE(CanvasUi::parse_decimal(rejected).has_value());
}

namespace {

using Clock = CanvasUi::Context::Clock;

void frame_at(CanvasUi::Context& context, Clock::time_point t) { CanvasUi::CanvasFrame(context).begin(false, 1.f, t, 0, nullptr, {}); }

} // namespace

TEST_CASE("A tooltip is due the tooltip delay after its item is first hovered, and asks for that frame", "[CanvasUi]")
{
    CanvasUi::Context          context;
    const Clock::time_point start = Clock::now();

    frame_at(context, start);
    CHECK_FALSE(context.tooltip_due(1));
    REQUIRE(CanvasUi::CanvasFrame(context).wake_at().has_value());
    CHECK(*CanvasUi::CanvasFrame(context).wake_at() == start + CanvasUi::duration::tooltip);
    CanvasUi::CanvasFrame(context).end();

    frame_at(context, start + CanvasUi::duration::tooltip / 2);
    CHECK_FALSE(context.tooltip_due(1));
    CanvasUi::CanvasFrame(context).end();

    frame_at(context, start + CanvasUi::duration::tooltip);
    CHECK(context.tooltip_due(1));
    CHECK_FALSE(CanvasUi::CanvasFrame(context).wake_at().has_value());
}

TEST_CASE("Hovering another item or nothing starts the tooltip wait again", "[CanvasUi]")
{
    CanvasUi::Context          context;
    const Clock::time_point start = Clock::now();

    frame_at(context, start);
    context.tooltip_due(1);
    CanvasUi::CanvasFrame(context).end();

    frame_at(context, start + CanvasUi::duration::tooltip * 4 / 5);
    CHECK_FALSE(context.tooltip_due(2));
    CanvasUi::CanvasFrame(context).end();
    frame_at(context, start + CanvasUi::duration::tooltip * 6 / 5);
    CHECK_FALSE(context.tooltip_due(2));
    CanvasUi::CanvasFrame(context).end();

    frame_at(context, start + CanvasUi::duration::tooltip * 7 / 5);
    CanvasUi::CanvasFrame(context).end();
    frame_at(context, start + CanvasUi::duration::tooltip * 2);
    CHECK_FALSE(context.tooltip_due(2));
}

TEST_CASE("A frame asks for nothing unless a component does, and keeps the earliest wake", "[CanvasUi]")
{
    CanvasUi::Context          context;
    const Clock::time_point start = Clock::now();

    frame_at(context, start);
    CHECK_FALSE(CanvasUi::CanvasFrame(context).frame_requested());
    CHECK_FALSE(CanvasUi::CanvasFrame(context).wake_at().has_value());

    context.request_frame_at(start + std::chrono::milliseconds(300));
    context.request_frame_at(start + std::chrono::milliseconds(100));
    context.request_frame();
    CHECK(CanvasUi::CanvasFrame(context).frame_requested());
    CHECK(*CanvasUi::CanvasFrame(context).wake_at() == start + std::chrono::milliseconds(100));

    frame_at(context, start + std::chrono::milliseconds(50));
    CHECK_FALSE(CanvasUi::CanvasFrame(context).frame_requested());
    CHECK_FALSE(CanvasUi::CanvasFrame(context).wake_at().has_value());
}

TEST_CASE("A context made current inside another gives the outer one back", "[CanvasUi]")
{
    CanvasUi::Context outer;
    CanvasUi::Context inner;
    {
        const CanvasUi::ScopedContext outer_scope(outer);
        CHECK(CanvasUi::Context::current() == &outer);
        {
            const CanvasUi::ScopedContext inner_scope(inner);
            CHECK(CanvasUi::Context::current() == &inner);
        }
        CHECK(CanvasUi::Context::current() == &outer);
    }
}

namespace {

// ImGui frames without OpenGL, the font atlas built on the CPU, with the components drawn in a card at the
// top left corner of an 800 × 600 display.
class Frames
{
public:
    explicit Frames(float scale = 1.f, float bold_size = 0.f)
        : m_scale(scale), m_previous(ImGui::GetCurrentContext()), m_imgui(ImGui::CreateContext())
    {
        ImGui::SetCurrentContext(m_imgui);
        ImGuiIO& io    = ImGui::GetIO();
        io.DisplaySize = ImVec2(800.f, 600.f);
        io.KeyMap[ImGuiKey_Enter]  = 13;
        io.KeyMap[ImGuiKey_Escape] = 27;
        io.DeltaTime   = 1.f / 60.f;
        io.IniFilename = nullptr;
        if (bold_size > 0.f) {
            io.Fonts->AddFontDefault();
            ImFontConfig bold;
            bold.SizePixels = bold_size;
            m_fonts.bold    = io.Fonts->AddFontDefault(&bold);
        }
        unsigned char* pixels = nullptr;
        int            width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }
    ~Frames()
    {
        ImGui::DestroyContext(m_imgui);
        ImGui::SetCurrentContext(m_previous);
    }
    Frames(const Frames&)            = delete;
    Frames& operator=(const Frames&) = delete;

    template<class Draw> void frame(const ImVec2& mouse, bool down, Draw draw)
    {
        ImGuiIO& io     = ImGui::GetIO();
        io.MousePos     = mouse;
        io.MouseDown[0] = down;
        ImGui::NewFrame();
        CanvasUi::CanvasFrame(m_ui).begin(false, m_scale, m_now, ImGui::GetFrameCount(), nullptr, m_fonts);
        {
            const CanvasUi::ScopedContext scope(m_ui);
            CanvasUi::CardStyle           style;
            style.padding = 0.f;
            style.shadow  = false;
            const CanvasUi::Card card("##test", CanvasUi::Point(0.f, 0.f), CanvasUi::Size(400.f, 300.f), style);
            draw();
        }
        CanvasUi::CanvasFrame(m_ui).end();
        ImGui::EndFrame();
        m_now += std::chrono::milliseconds(16);
    }

    // Moves to at, presses and lets go; whether draw reported a change or a press in any of those frames.
    template<class Draw> bool click(const ImVec2& at, Draw draw)
    {
        bool reported = false;
        const auto record = [&]() { reported |= draw(); };
        frame(ImVec2(-1.f, -1.f), false, record);
        frame(at, false, record);
        frame(at, true, record);
        frame(at, false, record);
        return reported;
    }

    bool frame_requested() { return CanvasUi::CanvasFrame(m_ui).frame_requested(); }

    template<class Draw> void hover(const ImVec2& at, std::chrono::milliseconds time, Draw draw)
    {
        for (const auto end = m_now + time; m_now <= end;)
            frame(at, false, draw);
    }

private:
    float                             m_scale;
    CanvasUi::Fonts                   m_fonts;
    ImGuiContext*                     m_previous;
    ImGuiContext*                     m_imgui;
    CanvasUi::Context                    m_ui;
    CanvasUi::Context::Clock::time_point m_now{};
};

const ImGuiWindow* active_tooltip()
{
    for (const ImGuiWindow* window : GImGui->Windows)
        if (window->Active && std::strncmp(window->Name, "##Tooltip_", 10) == 0)
            return window;
    return nullptr;
}

bool tooltip_shown()
{
    const ImGuiWindow* tooltip = active_tooltip();
    return tooltip != nullptr && !tooltip->Hidden;
}

} // namespace

TEST_CASE("A button reports a click, and a disabled one does not", "[CanvasUi]")
{
    Frames frames;
    CHECK(frames.click(ImVec2(10.f, 10.f), []() { return CanvasUi::button("##apply", "Apply"); }));
    CanvasUi::ButtonOptions disabled;
    disabled.availability = CanvasUi::Availability{false, "Not now"};
    CHECK_FALSE(frames.click(ImVec2(10.f, 10.f), [&]() { return CanvasUi::button("##apply", "Apply", disabled); }));
    CHECK_FALSE(frames.click(ImVec2(300.f, 200.f), []() { return CanvasUi::button("##apply", "Apply"); }));
}

TEST_CASE("A toggle group takes the choice clicked", "[CanvasUi]")
{
    Frames frames;
    int    selected = 0;
    const std::vector<std::string> choices = {"Clip", "Wrap"};
    CHECK(frames.click(ImVec2(150.f, 12.f), [&]() { return CanvasUi::toggle_group("##long", selected, choices, {200.f}); }));
    CHECK(selected == 1);
    CHECK_FALSE(frames.click(ImVec2(150.f, 12.f), [&]() { return CanvasUi::toggle_group("##long", selected, choices, {200.f}); }));
    CHECK(selected == 1);
}

TEST_CASE("A switch flips on a click anywhere in its row, unless it is disabled", "[CanvasUi]")
{
    Frames frames;
    bool   value = false;
    CHECK(frames.click(ImVec2(20.f, 10.f), [&]() { return CanvasUi::field_switch("##shade", value, {"Shade", 300.f}); }));
    CHECK(value);
    CHECK_FALSE(frames.click(ImVec2(20.f, 10.f),
                             [&]() { return CanvasUi::field_switch("##shade", value, {"Shade", 300.f, CanvasUi::Availability{false, "Not now"}}); }));
    CHECK(value);
}

TEST_CASE("A menu returns the id of the item picked, and nothing for a disabled one", "[CanvasUi]")
{
    const CanvasUi::Metrics m(1.f);
    const ImVec2         first_row(150.f, 100.f + m.menu_padding_y + .5f * m.menu_row);
    const ImVec2         second_row(first_row.x, first_row.y + m.menu_row);

    const auto run = [&](const ImVec2& at) {
        Frames             frames;
        std::optional<int> picked;
        bool               opened = false;
        frames.click(at, [&]() {
            if (!opened)
                ImGui::OpenPopup("##menu");
            opened = true;
            const auto items = []() {
                CanvasUi::MenuItem off = CanvasUi::MenuItem::item(4, "Reset");
                off.availability.enabled = false;
                return std::vector<CanvasUi::MenuItem>{CanvasUi::MenuItem::item(7, "Top"), off};
            };
            if (const std::optional<int> id = CanvasUi::menu_popup("##menu", CanvasUi::Positioner::point(100.f, 100.f), items))
                picked = id;
            return false;
        });
        return picked;
    };
    CHECK(run(first_row) == 7);
    CHECK_FALSE(run(second_row).has_value());
}

TEST_CASE("A disabled button shows its reason after the hover delay", "[CanvasUi]")
{
    Frames               frames;
    CanvasUi::ButtonOptions disabled;
    disabled.availability = CanvasUi::Availability{false, "Not now"};
    const auto draw = [&]() { CanvasUi::button("##apply", "Apply", disabled); };

    const auto half = std::chrono::duration_cast<std::chrono::milliseconds>(CanvasUi::duration::tooltip) * 3 / 5;
    frames.hover(ImVec2(10.f, 10.f), half, draw);
    CHECK_FALSE(tooltip_shown());
    frames.hover(ImVec2(10.f, 10.f), half, draw);
    CHECK(tooltip_shown());
}

TEST_CASE("A tooltip asks for the frame that shows it, as ImGui hides it in the frame it appears", "[CanvasUi]")
{
    Frames               frames;
    CanvasUi::ButtonOptions disabled;
    disabled.availability = CanvasUi::Availability{false, "Not now"};
    const auto draw = [&]() { CanvasUi::button("##apply", "Apply", disabled); };

    bool appeared = false;
    for (int i = 0; i < 60 && !appeared; ++i) {
        frames.frame(ImVec2(10.f, 10.f), false, draw);
        appeared = active_tooltip() != nullptr;
    }
    REQUIRE(appeared);
    CHECK(active_tooltip()->Hidden);
    CHECK(frames.frame_requested());
}

namespace {

const ImGuiWindow* open_popup_window()
{
    for (const ImGuiWindow* window : GImGui->Windows)
        if (window->Active && (window->Flags & ImGuiWindowFlags_Popup) != 0)
            return window;
    return nullptr;
}

float glyphs_right(const ImDrawList& list)
{
    const ImVec2 white = ImGui::GetIO().Fonts->TexUvWhitePixel;
    float        right = -FLT_MAX;
    for (const ImDrawVert& vertex : list.VtxBuffer)
        if (vertex.uv.x != white.x || vertex.uv.y != white.y)
            right = std::max(right, vertex.pos.x);
    return right;
}

} // namespace

TEST_CASE("Components take the size their size functions give, and a stack advances by its stack height", "[CanvasUi]")
{
    const float scale = GENERATE(1.f, 1.25f);
    const std::vector<std::string> options = {"Auto", "5", "15"};
    bool        on       = true;
    int         selected = 0;
    std::vector<std::pair<ImVec2, ImVec2>> sizes;
    float       advance  = 0.f;
    float       expected = 0.f;
    {
        Frames frames(scale, std::round(16.f * scale));
        frames.frame(ImVec2(-1.f, -1.f), false, [&]() {
            const CanvasUi::Metrics&      m = CanvasUi::Context::current()->metrics();
            const CanvasUi::FontMeasure   measure;
            const CanvasUi::ChoiceMeasure choices;
            const float                width = 300.f;
            const float                top   = ImGui::GetCursorScreenPos().y;
            std::vector<float>         heights;
            const auto drawn = [&](const ImVec2& size) {
                sizes.emplace_back(ImGui::GetItemRectSize(), size);
                heights.push_back(size.y);
            };

            CanvasUi::button("##apply", "Apply");
            drawn(ImVec2(CanvasUi::button_width(m, measure, "Apply"), m.control_height));
            int tab = 0;
            CanvasUi::tabs_list("##tabs", tab, {"View", "Highlighting"});
            drawn(ImVec2(CanvasUi::tabs_list_width(m, choices, {"View", "Highlighting"}), m.tabs_list_height));
            CanvasUi::ButtonOptions reset;
            reset.variant = CanvasUi::ButtonVariant::Ghost;
            reset.icon    = CanvasUi::Icon::Reset;
            reset.size    = CanvasUi::ButtonSize::Sm;
            CanvasUi::button("##reset", "Reset", reset);
            drawn(ImVec2(CanvasUi::button_width(m, measure, "Reset", CanvasUi::ButtonSize::Sm, true), m.small_button));
            CanvasUi::toggle_group("##choice", selected, options, {width});
            drawn(ImVec2(width, CanvasUi::toggle_group_height(m, choices, options, width)));
            CanvasUi::field_toggle_group("##inline", selected, options, {"Lines", width});
            drawn(ImVec2(width, CanvasUi::field_toggle_group_height(m, measure, choices, "Lines", options, width)));
            CanvasUi::field_toggle_group("##stacked", selected, options, {"A label too long to sit beside its control", 120.f});
            drawn(ImVec2(120.f, CanvasUi::field_toggle_group_height(m, measure, choices, "A label too long to sit beside its control", options, 120.f)));
            CanvasUi::field_switch("##switch", on, {"Shade lines not yet printed, a label that wraps", 160.f});
            drawn(ImVec2(160.f, CanvasUi::field_switch_height(m, measure, "Shade lines not yet printed, a label that wraps", 160.f)));

            advance  = ImGui::GetCursorScreenPos().y - top;
            expected = CanvasUi::stack_height(m, heights) + m.item_gap;
        });
    }
    for (const auto& [drawn_size, measured] : sizes) {
        CHECK_THAT(drawn_size.x, WithinAbs(measured.x, 1e-3));
        CHECK_THAT(drawn_size.y, WithinAbs(measured.y, 1e-3));
    }
    CHECK_THAT(advance, WithinAbs(expected, 1e-3));

    {
        Frames frames(scale, std::round(16.f * scale));
        ImGui::GetStyle().AntiAliasedLinesUseTex = false;
        const std::vector<CanvasUi::MenuItem> items = {CanvasUi::MenuItem::group_label("View"), CanvasUi::MenuItem::radio_item(0, "Isometric", true),
                                                       CanvasUi::MenuItem::separator(),
                                                       CanvasUi::MenuItem::item(1, "A long item that sets the width of the menu")};
        bool opened = false;
        for (int i = 0; i < 2; ++i)
            frames.frame(ImVec2(-1.f, -1.f), false, [&]() {
                if (!opened)
                    CanvasUi::open_popup("##menu");
                opened = true;
                CanvasUi::menu_popup("##menu", CanvasUi::Positioner::point(10.f, 10.f), [&items]() { return items; });
            });
        const ImGuiWindow* menu = open_popup_window();
        REQUIRE(menu != nullptr);
        CHECK_THAT(menu->DC.CursorMaxPos.y + menu->WindowPadding.y, WithinAbs(menu->Pos.y + menu->Size.y, 1.));
        CHECK(glyphs_right(*menu->DrawList) <= menu->Pos.x + menu->Size.x - menu->WindowPadding.x + 1.f);
    }

}

TEST_CASE("A scroll area scrolls taller content up and down, never sideways", "[CanvasUi]")
{
    Frames frames;
    float  max_x = -1.f, max_y = -1.f;
    const auto draw = [&]() {
        const CanvasUi::ScrollArea scroll("##scroll", CanvasUi::Size(200.f, 80.f), 300.f);
        ImGui::Dummy(ImVec2(180.f, 20.f));
        max_x = ImGui::GetScrollMaxX();
        max_y = ImGui::GetScrollMaxY();
    };
    frames.frame(ImVec2(-1.f, -1.f), false, draw);
    frames.frame(ImVec2(-1.f, -1.f), false, draw);
    CHECK(max_y > 0.f);
    CHECK_THAT(max_x, WithinAbs(0.f, 1e-6));
}

namespace {

// Focuses the field, types text, then presses key; returns whether the field reported a confirmed value.
bool type_into_number_field(long long& value, const std::string& text, int key, bool enabled = true)
{
    Frames           frames;
    bool             confirmed = false;
    bool             focus     = true;
    CanvasUi::NumberFieldOptions options{120.f, CanvasUi::Availability{enabled, ""}};
    const auto draw = [&]() {
        if (focus)
            ImGui::SetKeyboardFocusHere();
        focus = false;
        confirmed |= CanvasUi::number_field("##number", value, options);
    };
    frames.frame(ImVec2(-1.f, -1.f), false, draw);
    frames.frame(ImVec2(-1.f, -1.f), false, draw);
    for (const char c : text) {
        ImGui::GetIO().AddInputCharacter(static_cast<unsigned int>(c));
        frames.frame(ImVec2(-1.f, -1.f), false, draw);
    }
    ImGui::GetIO().KeysDown[key] = true;
    frames.frame(ImVec2(-1.f, -1.f), false, draw);
    ImGui::GetIO().KeysDown[key] = false;
    frames.frame(ImVec2(-1.f, -1.f), false, draw);
    return confirmed;
}

} // namespace

TEST_CASE("A number field confirms with Enter, reverts with Esc, and a disabled one confirms nothing", "[CanvasUi]")
{
    long long value = 42;
    CHECK(type_into_number_field(value, "120", 13));
    CHECK(value == 120);

    value = 42;
    CHECK_FALSE(type_into_number_field(value, "120", 27));
    CHECK(value == 42);

    value = 42;
    CHECK_FALSE(type_into_number_field(value, "120", 13, false));
    CHECK(value == 42);
}

TEST_CASE("A click on a tab selects it", "[CanvasUi]")
{
    Frames frames;
    int    selected = 0;
    const std::vector<std::string> labels = {"View", "Highlighting"};
    CHECK(frames.click(ImVec2(120.f, 20.f), [&]() { return CanvasUi::tabs_list("##tabs", selected, labels); }));
    CHECK(selected == 1);
    CHECK_FALSE(frames.click(ImVec2(120.f, 20.f), [&]() { return CanvasUi::tabs_list("##tabs", selected, labels); }));
}

TEST_CASE("A painter paints in the colour given, and its scopes give the clip and the fringe back", "[CanvasUi]")
{
    Frames frames;
    frames.frame(ImVec2(-1.f, -1.f), false, []() {
        const CanvasUi::Painter paint  = CanvasUi::Painter::background();
        ImDrawList&          list   = *ImGui::GetBackgroundDrawList();
        const CanvasUi::Color   accent = CanvasUi::Context::current()->theme().primary;
        paint.fill_rect(CanvasUi::Rect(10.f, 10.f, 20.f, 20.f), accent);
        CHECK(list.VtxBuffer.back().col == ImGui::GetColorU32(ImVec4(accent.r, accent.g, accent.b, accent.a)));

        const int clips = list._ClipRectStack.Size;
        {
            const CanvasUi::Painter::Clip outer(paint, CanvasUi::Rect(0.f, 0.f, 100.f, 100.f));
            const CanvasUi::Painter::Clip inner(paint, CanvasUi::Rect(50.f, 50.f, 200.f, 200.f));
            const ImVec4               clip = list._ClipRectStack.back();
            CHECK_THAT(clip.x, WithinAbs(50., 1e-6));
            CHECK_THAT(clip.y, WithinAbs(50., 1e-6));
            CHECK_THAT(clip.z, WithinAbs(100., 1e-6));
            CHECK_THAT(clip.w, WithinAbs(100., 1e-6));
        }
        CHECK(list._ClipRectStack.Size == clips);

        REQUIRE((list.Flags & ImDrawListFlags_AntiAliasedFill) != 0);
        {
            const CanvasUi::Painter::SharpFills sharp(paint);
            CHECK((list.Flags & ImDrawListFlags_AntiAliasedFill) == 0);
        }
        CHECK((list.Flags & ImDrawListFlags_AntiAliasedFill) != 0);

        const float fringe = list._FringeScale;
        paint.fill_convex({CanvasUi::Point(0.f, 0.f), CanvasUi::Point(10.f, 0.f), CanvasUi::Point(10.f, 10.f)}, accent, 5.f);
        CHECK_THAT(list._FringeScale, WithinAbs(fringe, 1e-6));
    });
}

TEST_CASE("The context gives the faces the canvas loaded, and the current font for a missing one", "[CanvasUi]")
{
    Frames frames;
    frames.frame(ImVec2(-1.f, -1.f), false, []() {
        CanvasUi::Context        context;
        ImFont* const         face = ImGui::GetIO().Fonts->Fonts[0];
        CanvasUi::Fonts          fonts;
        fonts.mono[size_t(CanvasUi::MonoStyle::Bold) * CanvasUi::MONO_FONT_SIZES.size() + size_t(CanvasUi::MonoSize::Code)] = face;
        CanvasUi::CanvasFrame(context).begin(false, 1.f, Clock::now(), 0, nullptr, fonts);
        CHECK(CanvasUi::face_of(context.mono_font(CanvasUi::MonoStyle::Bold, CanvasUi::MonoSize::Code)) == face);
        CHECK(context.mono_font(CanvasUi::MonoStyle::Bold, CanvasUi::MonoSize::Code).size() == face->FontSize * face->Scale);
        CHECK(CanvasUi::face_of(context.mono_font(CanvasUi::MonoStyle::Italic, CanvasUi::MonoSize::Caption)) == ImGui::GetFont());
        CHECK(CanvasUi::face_of(context.bold_font()) == ImGui::GetFont());
    });
}

TEST_CASE("A font knows the characters it lacks", "[CanvasUi]")
{
    Frames frames;
    frames.frame(ImVec2(-1.f, -1.f), false, []() {
        const CanvasUi::Font font = CanvasUi::Context::current()->regular_font();
        CHECK(font.has_glyphs("G1 X120.815"));
        CHECK_FALSE(font.has_glyphs("G1 \xD0\x96"));
        CHECK_THAT(font.width("G1"), WithinAbs(ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0.f, "G1").x, 1e-6));
    });
}

TEST_CASE("The pointer reads the mouse of the frame, and a panel's place is where its next component starts", "[CanvasUi]")
{
    Frames             frames;
    CanvasUi::Pointer     seen;
    ImVec2             cursor;
    const auto draw = [&]() {
        seen = CanvasUi::pointer();
        CanvasUi::place(CanvasUi::Point(30.f, 40.f));
        cursor = ImGui::GetCursorScreenPos();
    };
    frames.frame(ImVec2(30.f, 40.f), false, draw);
    frames.frame(ImVec2(30.f, 40.f), true, draw);
    CHECK_THAT(seen.pos.x, WithinAbs(30., 1e-6));
    CHECK_THAT(seen.pos.y, WithinAbs(40., 1e-6));
    CHECK(seen.pressed);
    CHECK(seen.held);
    CHECK_FALSE(seen.context_pressed);
    CHECK(seen.over_panel);
    CHECK_THAT(cursor.x, WithinAbs(30., 1e-6));
    CHECK_THAT(cursor.y, WithinAbs(40., 1e-6));
    frames.frame(ImVec2(35.f, 40.f), true, draw);
    CHECK_FALSE(seen.pressed);
    CHECK(seen.held);
    CHECK_THAT(seen.delta.x, WithinAbs(5., 1e-6));
    frames.frame(ImVec2(600.f, 500.f), false, draw);
    frames.frame(ImVec2(600.f, 500.f), false, draw);
    CHECK_FALSE(seen.over_panel);
}

TEST_CASE("Esc is pressed in the frame its key goes down", "[CanvasUi]")
{
    Frames frames;
    bool   pressed = false;
    ImGui::GetIO().KeysDown[27] = true;
    frames.frame(ImVec2(-1.f, -1.f), false, [&]() { pressed = CanvasUi::escape_pressed(); });
    CHECK(pressed);
    frames.frame(ImVec2(-1.f, -1.f), false, [&]() { pressed = CanvasUi::escape_pressed(); });
    CHECK_FALSE(pressed);
    ImGui::GetIO().KeysDown[27] = false;
}

namespace {

void press_key(Frames& frames, int key, const std::function<void()>& draw)
{
    ImGui::GetIO().KeysDown[key] = true;
    frames.frame(ImVec2(-1.f, -1.f), false, draw);
    ImGui::GetIO().KeysDown[key] = false;
    frames.frame(ImVec2(-1.f, -1.f), false, draw);
}

} // namespace

TEST_CASE("A click turns the position field into a number field, Enter jumps and Esc leaves it", "[CanvasUi]")
{
    Frames                   frames;
    CanvasUi::PositionField     field;
    std::optional<long long> jump;
    const auto               draw = [&]() {
        const CanvasUi::Font font = CanvasUi::Context::current()->regular_font();
        if (const std::optional<long long> line = CanvasUi::position_field("##position", field, 374353, 375140, {font, font, CanvasUi::Size(200.f, 32.f), 6.f}))
            jump = line;
        return false;
    };
    frames.click(ImVec2(50.f, 16.f), draw);
    CHECK(field.editing);
    CHECK(field.value == 374353);
    CHECK(frames.frame_requested());

    frames.frame(ImVec2(-1.f, -1.f), false, draw);
    CHECK(frames.frame_requested());
    frames.frame(ImVec2(-1.f, -1.f), false, draw);
    CHECK(ImGui::GetActiveID() != 0);
    for (const char c : std::string("120")) {
        ImGui::GetIO().AddInputCharacter(static_cast<unsigned int>(c));
        frames.frame(ImVec2(-1.f, -1.f), false, draw);
    }
    press_key(frames, 13, draw);
    CHECK(jump == 120);
    CHECK_FALSE(field.editing);

    jump.reset();
    frames.click(ImVec2(50.f, 16.f), draw);
    REQUIRE(field.editing);
    frames.frame(ImVec2(-1.f, -1.f), false, draw);
    frames.frame(ImVec2(-1.f, -1.f), false, draw);
    press_key(frames, 27, draw);
    CHECK_FALSE(field.editing);
    CHECK_FALSE(jump.has_value());
}

TEST_CASE("A position field left out of a frame, as by a hidden panel, shows the position again", "[CanvasUi]")
{
    Frames                  frames;
    CanvasUi::PositionField field;
    const auto              draw = [&]() {
        const CanvasUi::Font font = CanvasUi::Context::current()->regular_font();
        CanvasUi::position_field("##position", field, 374353, 375140, {font, font, CanvasUi::Size(200.f, 32.f), 6.f});
        return false;
    };
    frames.click(ImVec2(50.f, 16.f), draw);
    frames.frame(ImVec2(-1.f, -1.f), false, draw);
    REQUIRE(field.editing);

    frames.frame(ImVec2(-1.f, -1.f), false, []() {});
    frames.frame(ImVec2(-1.f, -1.f), false, draw);
    CHECK_FALSE(field.editing);
}

TEST_CASE("A button draws its label without the id after ##", "[CanvasUi]")
{
    Frames frames;
    int    plain = 0, with_id = 0;
    frames.frame(ImVec2(-1.f, -1.f), false, [&]() {
        ImDrawList& list  = *ImGui::GetWindowDrawList();
        const int   start = list.VtxBuffer.Size;
        CanvasUi::button("##plain", "Reset");
        plain         = list.VtxBuffer.Size - start;
        const int mid = list.VtxBuffer.Size;
        CanvasUi::button("##with_id", "Reset##x");
        with_id = list.VtxBuffer.Size - mid;
    });
    CHECK(plain > 0);
    CHECK(with_id == plain);
}

TEST_CASE("Input is active while a field holds the keyboard and for the frame after", "[CanvasUi]")
{
    Frames    frames;
    long long value = 42;
    bool      focus = true, active = false;
    const auto draw = [&]() {
        active = CanvasUi::input_active();
        if (focus)
            ImGui::SetKeyboardFocusHere();
        focus = false;
        CanvasUi::number_field("##number", value, {120.f});
    };
    frames.frame(ImVec2(-1.f, -1.f), false, draw);
    CHECK_FALSE(active);
    frames.frame(ImVec2(-1.f, -1.f), false, draw);
    frames.frame(ImVec2(-1.f, -1.f), false, draw);
    CHECK(active);
}

TEST_CASE("A card paints into itself even from inside a scroll area of it", "[CanvasUi]")
{
    Frames frames;
    int    card_before = 0, card_after = 0;
    frames.frame(ImVec2(-1.f, -1.f), false, [&]() {
        const CanvasUi::Card card("##outer", CanvasUi::Point(420.f, 10.f), CanvasUi::Size(200.f, 200.f));
        ImDrawList&       list = *ImGui::GetWindowDrawList();
        {
            const CanvasUi::ScrollArea scroll("##scroll", CanvasUi::Size(180.f, 100.f), 300.f);
            card_before = list.VtxBuffer.Size;
            card.painter().fill_rect(CanvasUi::Rect(430.f, 20.f, 440.f, 30.f), CanvasUi::Color(1.f, 0.f, 0.f));
            card_after = list.VtxBuffer.Size;
        }
    });
    CHECK(card_after > card_before);
}

TEST_CASE("A row's background is the card's own shape cut to the row, so rows near a corner meet without notches", "[CanvasUi]")
{
    Frames frames;
    std::vector<std::pair<float, float>> rows;
    std::vector<float>                   shape_heights;
    std::vector<ImVec4>                  clips;
    frames.frame(ImVec2(-1.f, -1.f), false, [&]() {
        CanvasUi::CardStyle style;
        style.padding = 0.f;
        style.shadow  = false;
        const CanvasUi::Card card("##rows", CanvasUi::Point(420.f, 10.f), CanvasUi::Size(200.f, 200.f), style);
        ImDrawList&          list = *ImGui::GetWindowDrawList();
        for (const float top : {100.f, 196.f, 205.f}) {
            const int vertices = list.VtxBuffer.Size;
            card.row_background(top, top + 9.f, CanvasUi::Color(1.f, 0.f, 0.f));
            float low = FLT_MAX, high = -FLT_MAX;
            for (int i = vertices; i < list.VtxBuffer.Size; ++i) {
                low  = std::min(low, list.VtxBuffer[i].pos.y);
                high = std::max(high, list.VtxBuffer[i].pos.y);
            }
            shape_heights.push_back(high - low);
            rows.emplace_back(top, std::min(top + 9.f, 209.f));
        }
        for (const ImDrawCmd& cmd : list.CmdBuffer)
            if (cmd.ElemCount > 0)
                clips.push_back(cmd.ClipRect);
    });
    REQUIRE(shape_heights.size() == 3);
    for (const float height : shape_heights)
        CHECK(height > 150.f);
    for (const auto& [top, bottom] : rows)
        CHECK(std::any_of(clips.begin(), clips.end(), [top = top, bottom = bottom](const ImVec4& clip) {
            return std::abs(clip.y - top) < 1e-3f && std::abs(clip.w - bottom) < 1e-3f;
        }));
}

namespace {

// Clockwise on the screen and convex, once repeated points of square corners are dropped.
bool convex_clockwise(const std::vector<CanvasUi::Point>& outline)
{
    std::vector<CanvasUi::Point> points;
    for (const CanvasUi::Point& p : outline)
        if (points.empty() || std::hypot(p.x - points.back().x, p.y - points.back().y) > 1e-4f)
            points.push_back(p);
    for (size_t i = 0; i < points.size(); ++i) {
        const CanvasUi::Point& a = points[i];
        const CanvasUi::Point& b = points[(i + 1) % points.size()];
        const CanvasUi::Point& c = points[(i + 2) % points.size()];
        if ((b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x) < -1e-3f)
            return false;
    }
    return true;
}

} // namespace

TEST_CASE("Two presses make a double click by the frame's clock, not ImGui's", "[CanvasUi]")
{
    Frames                    frames;
    std::vector<bool>         double_clicks;
    const auto                draw = [&]() {
        const CanvasUi::Pointer p = CanvasUi::pointer();
        if (p.pressed)
            double_clicks.push_back(p.double_clicked);
    };
    const ImVec2 at(100.f, 50.f);
    frames.hover(at, std::chrono::milliseconds(400), draw);
    frames.frame(at, true, draw);
    frames.frame(at, false, draw);
    frames.frame(ImVec2(102.f, 51.f), true, draw);
    frames.frame(at, false, draw);
    frames.hover(at, std::chrono::milliseconds(400), draw);
    frames.frame(at, true, draw);
    frames.frame(at, false, draw);
    frames.frame(ImVec2(160.f, 50.f), true, draw);
    frames.frame(at, false, draw);
    CHECK(double_clicks == std::vector<bool>{false, true, false, false});
}

TEST_CASE("A rounded outline is convex, inside its rectangle and as long whatever its corners", "[CanvasUi]")
{
    const float smoothing = GENERATE(0.f, .6f, 1.f);
    const CanvasUi::Rect rect = GENERATE(CanvasUi::Rect(0.f, 0.f, 200.f, 100.f), CanvasUi::Rect(0.f, 0.f, 40.f, 20.f));
    for (const CanvasUi::Corners corners : {CanvasUi::Corners::All, CanvasUi::Corners::Top, CanvasUi::Corners::None}) {
        const std::vector<CanvasUi::Point> outline = CanvasUi::rounded_rect_outline(rect, 12.f, smoothing, corners);
        CHECK(outline.size() == 4 * size_t(CanvasUi::CORNER_POINTS));
        CHECK(convex_clockwise(outline));
        for (const CanvasUi::Point& p : outline) {
            CHECK(p.x >= rect.left - 1e-3f);
            CHECK(p.x <= rect.right + 1e-3f);
            CHECK(p.y >= rect.top - 1e-3f);
            CHECK(p.y <= rect.bottom + 1e-3f);
        }
    }
}

TEST_CASE("Without smoothing a corner is a circular arc, and a short side takes the smoothing off before the radius", "[CanvasUi]")
{
    const CanvasUi::Rect               rect(0.f, 0.f, 200.f, 100.f);
    const std::vector<CanvasUi::Point> arc = CanvasUi::rounded_rect_outline(rect, 12.f, 0.f);
    for (int i = CanvasUi::CORNER_POINTS; i < 2 * CanvasUi::CORNER_POINTS; ++i)
        CHECK_THAT(std::hypot(arc[size_t(i)].x - 188.f, arc[size_t(i)].y - 12.f), WithinAbs(12., 1e-2));

    const std::vector<CanvasUi::Point> smooth = CanvasUi::rounded_rect_outline(rect, 12.f, 1.f);
    CHECK_THAT(smooth[size_t(CanvasUi::CORNER_POINTS)].x, WithinAbs(176., 1e-3));

    const std::vector<CanvasUi::Point> pill = CanvasUi::rounded_rect_outline(CanvasUi::Rect(0.f, 0.f, 40.f, 20.f), 12.f, 1.f);
    CHECK_THAT(pill[size_t(CanvasUi::CORNER_POINTS)].x, WithinAbs(30., 1e-3));
    CHECK_THAT(pill[size_t(2 * CanvasUi::CORNER_POINTS - 1)].y, WithinAbs(10., 1e-3));
}

TEST_CASE("An open collapsible panel as tall as its height function shows its content under its bar with room below", "[CanvasUi]")
{
    const float scale   = GENERATE(1.f, 1.5f);
    const float content = GENERATE(0.f, 60.f);
    Frames      frames(scale);
    const float top = 20.f;
    float       bar = 0.f, height = 0.f, content_top = 0.f;
    frames.frame(ImVec2(-1.f, -1.f), false, [&]() {
        const CanvasUi::Metrics& m = CanvasUi::Context::current()->metrics();
        bar                        = m.tabs_list_height;
        height                     = CanvasUi::collapsible_panel_height(m, content);
        CanvasUi::CollapsiblePanelOptions options;
        options.width          = 300.f;
        options.height         = height;
        options.content_height = content;
        const CanvasUi::CollapsiblePanel panel("##panel", CanvasUi::Point(0.f, top), options);
        content_top = ImGui::GetCursorScreenPos().y;
        ImGui::Dummy(ImVec2(panel.content_width(), content));
    });
    const float above = content_top - (top + bar);
    CHECK(above > 0.f);
    CHECK(top + height - (content_top + content) >= above);
    CHECK_THAT(height - CanvasUi::collapsible_panel_height(CanvasUi::Metrics(scale), 0.f), WithinAbs(content, 1e-4));
}

TEST_CASE("A collapsible trigger opens and closes its panel, and the panel stacks its content under its bar", "[CanvasUi]")
{
    Frames frames;
    bool   open = false;
    CHECK(frames.click(ImVec2(10.f, 10.f), [&]() { return CanvasUi::collapsible_trigger("##trigger", "Settings", open, {}); }));
    CHECK(open);

    ImVec2 content;
    float  width = 0.f;
    frames.frame(ImVec2(-1.f, -1.f), false, [&]() {
        CanvasUi::CollapsiblePanelOptions options;
        options.width          = 300.f;
        options.height         = 100.f;
        options.content_height = 40.f;
        options.reset_label    = "Reset";
        const CanvasUi::CollapsiblePanel panel("##panel", CanvasUi::Point(0.f, 50.f), options);
        content = ImGui::GetCursorScreenPos();
        width   = panel.content_width();
    });
    const CanvasUi::Metrics m(1.f);
    CHECK_THAT(content.x, WithinAbs(m.card_padding, 1e-4));
    CHECK_THAT(content.y, WithinAbs(50.f + m.tabs_list_height + m.collapsible_content_top, 1e-4));
    CHECK_THAT(width, WithinAbs(300.f - 2.f * m.card_padding, 1e-4));
}
