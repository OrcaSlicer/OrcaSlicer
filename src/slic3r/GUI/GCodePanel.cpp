#include "GCodePanel.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>

#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/GCodePanelText.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/PreviewPanelSlots.hpp"
#include "slic3r/GUI/CanvasUi/Availability.hpp"
#include "slic3r/GUI/CanvasUi/Button.hpp"
#include "slic3r/GUI/CanvasUi/Collapsible.hpp"
#include "slic3r/GUI/CanvasUi/NumberField.hpp"
#include "slic3r/GUI/CanvasUi/PositionField.hpp"
#include "slic3r/GUI/CanvasUi/ToggleGroup.hpp"
#include "slic3r/GUI/CanvasUi/Context.hpp"
#include "slic3r/GUI/CanvasUi/Font.hpp"
#include "slic3r/GUI/CanvasUi/FontMeasure.hpp"
#include "slic3r/GUI/CanvasUi/Icons.hpp"
#include "slic3r/GUI/CanvasUi/Input.hpp"
#include "slic3r/GUI/CanvasUi/Layout.hpp"
#include "slic3r/GUI/CanvasUi/Measure.hpp"
#include "slic3r/GUI/CanvasUi/Metrics.hpp"
#include "slic3r/GUI/CanvasUi/Menu.hpp"
#include "slic3r/GUI/CanvasUi/MenuPopup.hpp"
#include "slic3r/GUI/CanvasUi/Paint.hpp"
#include "slic3r/GUI/CanvasUi/SettingsForm.hpp"
#include "slic3r/GUI/CanvasUi/Card.hpp"
#include "slic3r/GUI/CanvasUi/PanelCard.hpp"
#include "slic3r/GUI/CanvasUi/Theme.hpp"
#include "slic3r/GUI/CanvasUi/Tokens.hpp"
#include "slic3r/GUI/CanvasUi/Types.hpp"
#include "slic3r/GUI/format.hpp"

namespace Slic3r {
namespace GUI {

void GCodePanel::load_gcode(const std::string& filename, const std::vector<size_t> &lines_ends)
{
    assert(! m_file.is_open());
    if (m_file.is_open())
        return;

    m_filename   = filename;
    m_lines_ends = lines_ends;

    m_rows.clear();
    m_rows_line  = 0;
    m_rows_count = 0;

    try
    {
        m_file.open(boost::filesystem::path(m_filename));
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": mapping file " << m_filename;
    }
    catch (...)
    {
        BOOST_LOG_TRIVIAL(error) << "Unable to map file " << m_filename << ". Cannot show G-code window.";
        reset();
    }
}

void GCodePanel::load_settings(const AppConfig& config)
{
    m_settings = load_gcode_panel_settings(config);
    collapse.load(load_preview_panel_collapsed(config, PreviewPanel::GCode));
}

struct GCodePanel::Sizes
{
    explicit Sizes(const CanvasUi::Metrics& metrics) : m(metrics) {}

    const CanvasUi::Metrics& m;
    float row{m.whole_px(CanvasUi::TextRole::code.leading)};
    // The number column: at least this wide, and this far from the stripe.
    float number_min{m.px(CanvasUi::space<50>())};
    float number_gap{m.px(CanvasUi::space<8>())};
    float stripe{m.px(CanvasUi::space<4>())};
    float text_right{m.px(CanvasUi::space<10>())};
    float current_line{m.px(CanvasUi::space<2>())};
    // The current line shown in the header of the collapsed panel.
    float collapsed_line{m.px(CanvasUi::space<22>())};
    float collapsed_number_inset{m.px(CanvasUi::space<8>())};
    float collapsed_text_right{m.px(CanvasUi::space<8>())};
    float counter_padding{m.px(CanvasUi::space<4>())};
    // The badge of a tag comment such as ;WIPE_START.
    float tag_padding{m.px(CanvasUi::space<6>())};
    float tag_half_height{.5f * m.px(CanvasUi::space<14>())};
    float tag_radius{m.px(CanvasUi::Radius::sm)};
    float tag_dot_radius{.5f * m.px(CanvasUi::space<6>())};
    float tag_dot_gap{m.px(CanvasUi::space<4>())};

    // Before a tag's badge: its ';' and half a column.
    static float tag_lead(float char_w) { return 1.5f * char_w; }
    // What a tag's badge takes past its text, the WIPE dot included.
    float tag_badge_room(float char_w) const { return tag_lead(char_w) + 2.f * tag_padding + 2.f * tag_dot_radius + tag_dot_gap; }
    // The fade over the end of a clipped long line.
    float fade{m.px(CanvasUi::space<28>())};
    // The scroll position indicator at the right edge.
    float position_indicator{m.px(CanvasUi::space<4>())};
    float position_indicator_inset{m.px(CanvasUi::space<4>())};
    float position_indicator_thumb{m.px(CanvasUi::space<14>())};
};

CanvasUi::SettingsFormOptions GCodePanel::settings_form(float width)
{
    CanvasUi::SettingsFormOptions form;
    form.width = width;
    form.tabs  = {_u8L_CONTEXT("View", "G-code window"), _u8L_CONTEXT("Highlighting", "G-code window")};
    form.tab   = &m_settings_tab;
    const auto choice = [&form](const char* id, const std::string& label, std::vector<std::string> options, std::function<int()> get,
                                std::function<void(int)> set) {
        CanvasUi::SettingsField field;
        field.id      = id;
        field.label   = label;
        field.options = std::move(options);
        field.get     = std::move(get);
        field.set     = std::move(set);
        form.fields.push_back(std::move(field));
    };
    std::vector<std::string> counts;
    for (const size_t lines : GCODE_PANEL_LINES)
        counts.push_back(lines == 0 ? _u8L_CONTEXT("Auto", "G-code window") : std::to_string(lines));
    // TRN The number of G-code lines the window shows.
    choice("##lines", _u8L_CONTEXT("Lines", "G-code window"), std::move(counts),
           [this]() { return int(std::find(GCODE_PANEL_LINES.begin(), GCODE_PANEL_LINES.end(), m_settings.lines) - GCODE_PANEL_LINES.begin()); },
           [this](int i) { m_settings.lines = GCODE_PANEL_LINES[std::min(size_t(i), GCODE_PANEL_LINES.size() - 1)]; });
    std::vector<std::string> sizes;
    for (const int size : CanvasUi::MONO_FONT_SIZES)
        sizes.push_back(std::to_string(size));
    choice("##font", _u8L_CONTEXT("Font size", "G-code window"), std::move(sizes), [this]() { return int(m_settings.font_size); },
           [this](int i) { m_settings.font_size = CanvasUi::MonoSize(i); });
    choice("##current", _u8L_CONTEXT("Current line", "G-code window"),
           {_u8L_CONTEXT("Center", "G-code window"), _u8L_CONTEXT("Upper third", "G-code window")}, [this]() { return m_settings.upper_third ? 1 : 0; },
           [this](int i) { m_settings.upper_third = i == 1; });
    choice("##long", _u8L_CONTEXT("Long lines", "G-code window"),
           // TRN Long G-code lines: cut at the window's edge, or continued on the next row.
           {_u8L_CONTEXT("Clip", "G-code window"), _u8L_CONTEXT("Wrap", "G-code window")}, [this]() { return m_settings.wrap ? 1 : 0; },
           [this](int i) { m_settings.wrap = i == 1; });
    choice("##comments", _u8L_CONTEXT("Comments", "G-code window"),
           {_u8L_CONTEXT("All", "G-code window"),
            // TRN Show only the tag comments Orca writes, such as ;TYPE: or ;WIPE_START.
            _u8L_CONTEXT("Tags", "G-code window"),
            // TRN Show no comments.
            _u8L_CONTEXT("None", "G-code window")},
           [this]() { return int(m_settings.comments); }, [this](int i) { m_settings.comments = GCodeCommentMode(i); });
    const auto highlight = [&form](CanvasUi::SettingsField field) {
        field.tab = 1;
        form.fields.push_back(std::move(field));
    };
    highlight(CanvasUi::settings_switch("##align", _u8L_CONTEXT("Align parameters in columns", "G-code window"), m_settings.align));
    highlight(CanvasUi::settings_switch("##axes", _u8L_CONTEXT("Color axis letters", "G-code window"), m_settings.color_axes));
    // TRN A coloured stripe left of each line, in the colour of its line type.
    highlight(CanvasUi::settings_switch("##stripe", _u8L_CONTEXT("Line type stripe", "G-code window"), m_settings.stripe));
    highlight(CanvasUi::settings_switch("##shade", _u8L_CONTEXT("Shade lines not yet printed", "G-code window"), m_settings.shade_unprinted));
    form.changed = changed_settings(m_settings);
    // TRN The number of settings of the G-code window that differ from their defaults.
    form.changed_count = format(_u8L_CONTEXT("Changed: %1%", "G-code window"), form.changed);
    form.reset_label   = _u8L_CONTEXT("Reset", "G-code window");
    form.reset         = [this]() { m_settings = Settings(); };
    return form;
}

std::string GCodePanel::read_line(uint64_t id) const
{
    // m_lines_ends indexes into a memory mapping, so it must be clamped to the mapping. If the file was
    // modified behind our back (an in-place post-processing script that shrank it), an unchecked read is
    // an access violation, which the caller's try/catch cannot catch on Windows.
    const auto [start, end] = gcode_line_range(m_lines_ends, m_file.size(), id);
    return end > start ? std::string(m_file.data() + start, end - start) : std::string();
}

void GCodePanel::update_rows(uint64_t curr_line_id, size_t count)
{
    if (m_rows_line == curr_line_id && m_rows_count == count)
        return;
    m_rows.clear();
    m_rows_line  = curr_line_id;
    m_rows_count = count;
    if (count == 0)
        return;

    m_rows = gcode_rows_around(curr_line_id, m_lines_ends.size(), count, m_settings, m_chars, m_tag_padding,
                               [this](uint64_t id) { return read_line(id); });
}

GCodePanel::RowGeometry GCodePanel::row_geometry(const Sizes& s, float left, float right, float number_right, float text_right,
                                                 float row_height, float char_width)
{
    RowGeometry g;
    g.left         = left;
    g.right        = right;
    g.row_height   = row_height;
    g.char_width   = char_width;
    g.number_right = number_right;
    g.stripe_x     = g.number_right + s.number_gap;
    g.text_left    = g.stripe_x + s.stripe + s.number_gap;
    g.chars        = size_t(std::max(0.f, (right - text_right - g.text_left) / char_width));
    return g;
}

static constexpr std::string_view TYPE_PREFIX = "TYPE:";

void GCodePanel::draw_tag_badge(const CanvasUi::Painter& paint, const GCodeSpan& span, float x, float top, const RowGeometry& g,
                                const Sizes& s, const CanvasUi::Color& background) const
{
    const CanvasUi::Context& ui      = *CanvasUi::Context::current();
    const CanvasUi::Theme&   theme   = ui.theme();
    const GCodePalette&   palette = gcode_palette(theme.dark);
    const CanvasUi::Font     regular = ui.mono_font(CanvasUi::MonoStyle::Regular, m_settings.font_size);
    const auto            draw    = [&paint, &g, top](const CanvasUi::Font& font, float at, const CanvasUi::Color& color, const std::string& text) {
        paint.text(font, CanvasUi::Rect(at, top, at, top + g.row_height), color, text);
    };
    const bool  wipe    = span.text.rfind("WIPE_", 0) == 0;
    const bool  type    = span.text.rfind(TYPE_PREFIX, 0) == 0;
    const float badge_x = x + Sizes::tag_lead(g.char_width);
    const float pad     = s.tag_padding;
    const float dot     = wipe ? 2.f * s.tag_dot_radius + s.tag_dot_gap : 0.f;
    const CanvasUi::Font tag_font = ui.mono_font(CanvasUi::MonoStyle::Regular, CanvasUi::MonoSize::Caption);
    const float       tag_char = tag_font.width("0");
    const float       text_w   = float(gcode_text_length(span.text)) * tag_char;
    const float middle = top + 0.5f * g.row_height;
    draw(regular, x, theme.muted_foreground, ";");
    paint.fill_rect(CanvasUi::Rect(badge_x, middle - s.tag_half_height, badge_x + pad + dot + text_w + pad, middle + s.tag_half_height), theme.accent,
                    s.tag_radius);
    if (wipe) {
        const CanvasUi::Point center(badge_x + pad + s.tag_dot_radius, middle);
        paint.fill_circle(center, s.tag_dot_radius, wipe_color);
        if (CanvasUi::contrast_ratio(wipe_color, background) < 3.f)
            paint.stroke_circle(center, s.tag_dot_radius, theme.contrast_outline);
    }
    const float text_x = badge_x + pad + dot;
    if (type) {
        draw(tag_font, text_x, palette.tag_text, std::string(TYPE_PREFIX));
        draw(tag_font, text_x + float(TYPE_PREFIX.size()) * tag_char, theme.foreground, span.text.substr(TYPE_PREFIX.size()));
    } else
        draw(tag_font, text_x, palette.tag_text, span.text);
}

CanvasUi::Color GCodePanel::body_row_background(const CanvasUi::Card& card, const Row& row, float top, const RowGeometry& g, uint64_t curr_line_id,
                                                const Sizes& s) const
{
    const CanvasUi::Theme& theme  = CanvasUi::Context::current()->theme();
    const float            bottom = top + g.row_height;
    if (row.line == curr_line_id) {
        card.row_background(top, bottom, theme.selection);
        {
            // the left edge of the row's own shape, so the bar follows the card's rounded corners
            const CanvasUi::Painter::Clip clip(card.painter(), CanvasUi::Rect(g.left, top, g.left + s.current_line, bottom));
            card.row_background(top, bottom, theme.current_line);
        }
        return CanvasUi::blend_over(theme.selection, theme.card);
    }
    if (m_settings.shade_unprinted && row.line > curr_line_id) {
        const CanvasUi::Color& unprinted = gcode_palette(theme.dark).unprinted_bg;
        card.row_background(top, bottom, unprinted);
        return unprinted;
    }
    return theme.card;
}

CanvasUi::Color GCodePanel::collapsed_line_background(const CanvasUi::Painter& paint, float top, const RowGeometry& g, const Sizes& s) const
{
    const CanvasUi::Theme& theme  = CanvasUi::Context::current()->theme();
    const float            bottom = top + g.row_height;
    paint.fill_rect(CanvasUi::Rect(g.left, top, g.right, bottom), theme.selection, g.rounding);
    // On the straight part of the background's edge: a slice of its corners would end square.
    paint.fill_rect(CanvasUi::Rect(g.left, top + .5f * g.rounding, g.left + s.current_line, bottom - .5f * g.rounding), theme.current_line,
                    .5f * s.current_line);
    return CanvasUi::blend_over(theme.selection, theme.card);
}

std::optional<GCodePanel::StripeRing> GCodePanel::render_row(const CanvasUi::Painter& paint, const Row& row, float top, const RowGeometry& g,
                                                              uint64_t curr_line_id, const CanvasUi::Color& background, const Sizes& s) const
{
    std::optional<StripeRing> ring;
    const CanvasUi::Context& ui             = *CanvasUi::Context::current();
    const CanvasUi::Theme&   theme          = ui.theme();
    const GCodePalette&   palette        = gcode_palette(theme.dark);
    const bool            current        = row.line == curr_line_id;
    const float           bottom         = top + g.row_height;
    const CanvasUi::MonoSize size          = m_settings.font_size;
    const CanvasUi::Font     regular        = ui.mono_font(CanvasUi::MonoStyle::Regular, size);
    const CanvasUi::Font     bold           = ui.mono_font(CanvasUi::MonoStyle::Bold, size);
    const CanvasUi::Font     italic         = ui.mono_font(CanvasUi::MonoStyle::Italic, size);
    const CanvasUi::Font     number_regular = ui.mono_font(CanvasUi::MonoStyle::Regular, CanvasUi::MonoSize::Caption);
    const CanvasUi::Font     number_bold    = ui.mono_font(CanvasUi::MonoStyle::Bold, CanvasUi::MonoSize::Caption);
    const auto            draw           = [&paint, &g, top](const CanvasUi::Font& font, float x, const CanvasUi::Color& color, const std::string& text) {
        paint.text(font, CanvasUi::Rect(x, top, x, top + g.row_height), color, text);
    };

    if (row.first) {
        const std::string number      = std::to_string(row.line);
        const CanvasUi::Font number_font = current ? number_bold : number_regular;
        const float          baseline    = top + .5f * (g.row_height - regular.size()) + regular.ascent();
        paint.text(number_font, CanvasUi::Point(g.number_right - number_font.width(number), baseline - number_font.ascent()),
                   current ? theme.foreground : theme.muted_foreground, number);
        if (m_settings.stripe && line_color)
            if (const std::optional<CanvasUi::Color> color = line_color(row.line)) {
                const CanvasUi::Rect stripe(g.stripe_x, top, g.stripe_x + s.stripe, bottom);
                paint.fill_rect(stripe, *color);
                if (CanvasUi::contrast_ratio(*color, background) < 3.f)
                    ring = StripeRing{stripe, *color};
            }
    }

    const float text_right = g.text_left + float(g.chars) * g.char_width;
    {
        const CanvasUi::Painter::Clip clip(paint, CanvasUi::Rect(g.text_left, top, text_right, bottom));
        for (const GCodeSpan& span : row.spans) {
            const float x = g.text_left + float(span.column) * g.char_width;
            if (span.style == GCodeSpanStyle::Tag) {
                draw_tag_badge(paint, span, x, top, g, s, background);
                continue;
            }
            CanvasUi::Font font = span.style == GCodeSpanStyle::Command ? bold : span.style == GCodeSpanStyle::Comment ? italic : regular;
            if (!font.has_glyphs(span.text))
                font = ui.regular_font();
            CanvasUi::Color color = theme.foreground;
            switch (span.style) {
            case GCodeSpanStyle::Macro: color = palette.macro; break;
            case GCodeSpanStyle::Argument: color = palette.macro_value; break;
            case GCodeSpanStyle::Key:
            case GCodeSpanStyle::Comment: color = theme.muted_foreground; break;
            case GCodeSpanStyle::Letter: color = m_settings.color_axes ? axis_color(palette, theme, span.letter) : theme.muted_foreground; break;
            default: break;
            }
            // The tint of the current line takes contrast from the syntax colours; nearer the foreground they stay readable on it.
            if (current)
                color = CanvasUi::mix(color, theme.foreground, .3f);
            draw(font, x, color, span.text);
        }
    }

    if (row.overflow) {
        CanvasUi::Color clear = background;
        clear.a            = 0.f;
        paint.fill_gradient(CanvasUi::Rect(text_right - s.fade, top, text_right, bottom), clear, background, CanvasUi::GradientAxis::Horizontal);
    }
    return ring;
}

std::string GCodePanel::copy_text(bool visible) const
{
    return gcode_copy_text(visible ? m_rows : std::vector<Row>(), m_menu_line, [this](uint64_t id) { return read_line(id); });
}

void GCodePanel::render_copy_menu()
{
    enum CopyItem { CopyLine, CopyVisibleLines };
    const auto items = []() {
        return std::vector<CanvasUi::MenuItem>{CanvasUi::MenuItem::item(CopyLine, _u8L_CONTEXT("Copy line", "G-code window")),
                                            CanvasUi::MenuItem::item(CopyVisibleLines, _u8L_CONTEXT("Copy visible lines", "G-code window"))};
    };
    if (const std::optional<int> picked = CanvasUi::menu_popup("##gcode_copy", CanvasUi::Positioner::point(m_menu_pos.x, m_menu_pos.y), items))
        CanvasUi::copy_to_clipboard(copy_text(*picked == CopyVisibleLines));
}

void GCodePanel::open_copy_menu_at(uint64_t line, const CanvasUi::Point& pos)
{
    m_menu_line = line;
    m_menu_pos  = pos;
    CanvasUi::open_popup("##gcode_copy");
}

void GCodePanel::render_header(const CanvasUi::PanelCard& frame, const Sizes& s, uint64_t curr_line_id, std::vector<GCodeSpan> current_spans,
                               const RowGeometry& body)
{
    const CanvasUi::Context& ui         = *CanvasUi::Context::current();
    const CanvasUi::Theme&   theme      = ui.theme();
    const CanvasUi::Metrics& m          = ui.metrics();
    const CanvasUi::Painter  paint      = frame.card().painter();
    const CanvasUi::Point    origin     = frame.origin();
    const CanvasUi::Font     title_font = collapse.collapsed() ? ui.regular_font() : ui.bold_font();
    const std::string     title      = _u8L("G-code");
    const float           button     = m.panel_header_button;
    const CanvasUi::Font     counter_font = ui.mono_font(CanvasUi::MonoStyle::Regular, CanvasUi::MonoSize::Counter);
    const uint64_t        total      = m_lines_ends.size();

    // The settings button closes the row, after the position, which only the expanded window has.
    enum HeaderSlot : size_t { CollapseSlot, TitleSlot, LineSlot, PositionSlot };
    std::vector<CanvasUi::RowSlot> slots = {{button}, {title_font.width(title), false, true}, {0.f, true}};
    if (!collapse.collapsed())
        slots.push_back({CanvasUi::position_field_width(CanvasUi::FontMeasure(counter_font), curr_line_id, total, s.counter_padding)});
    slots.push_back({button});
    const std::vector<CanvasUi::RowSpan> header   = CanvasUi::row_layout(origin.x + m.inline_gap, origin.x + frame.width() - m.inline_gap, m.inline_gap, slots);
    const float                       button_y = origin.y + 0.5f * (frame.header() - button);

    if (frame.collapse_button("##gcode_collapse", collapse.collapsed(),
                          collapse.collapsed() ? _u8L_CONTEXT("Expand", "G-code window") : _u8L_CONTEXT("Collapse", "G-code window")))
        collapse.toggle_next_frame();

    {
        const CanvasUi::Rect          band(header[TitleSlot].left, origin.y, header[TitleSlot].right(), origin.y + frame.header());
        const CanvasUi::Painter::Clip clip(paint, band);
        paint.text(title_font, band, collapse.collapsed() ? theme.muted_foreground : theme.foreground, title);
    }

    CanvasUi::place(CanvasUi::Point(header.back().left, button_y));
    CanvasUi::ButtonOptions gear;
    gear.size                 = CanvasUi::ButtonSize::Icon;
    gear.width                = button;
    gear.icon                 = CanvasUi::Icon::Sliders;
    gear.tooltip              = _u8L_CONTEXT("Settings", "G-code window");
    if (CanvasUi::collapsible_trigger("##gcode_settings", "", m_settings_open, gear))
        m_settings_tab = 0;

    if (collapse.collapsed()) {
        const float number_w   = ui.mono_font(CanvasUi::MonoStyle::Bold, CanvasUi::MonoSize::Caption).width("0");
        const float top        = origin.y + 0.5f * (frame.header() - s.collapsed_line);
        RowGeometry g          = row_geometry(s, header[LineSlot].left, header[LineSlot].right(),
                                              header[LineSlot].left + s.collapsed_number_inset + float(std::to_string(curr_line_id).size()) * number_w,
                                              s.collapsed_text_right, s.collapsed_line, body.char_width);
        g.rounding             = m.icon_button_radius;
        Row row{curr_line_id, true, false, std::move(current_spans)};
        row.overflow = std::any_of(row.spans.begin(), row.spans.end(), [&g](const GCodeSpan& span) { return span.end() > g.chars; });
        const CanvasUi::Painter::Clip clip(paint, CanvasUi::Rect(g.left, top, g.right, top + s.collapsed_line));
        if (const std::optional<StripeRing> ring = render_row(paint, row, top, g, curr_line_id, collapsed_line_background(paint, top, g, s), s))
            paint.stroke_rect(ring->rect, theme.contrast_outline);
    } else {
        const CanvasUi::RowSpan& counter = header[PositionSlot];
        CanvasUi::place(CanvasUi::Point(counter.left, button_y));
        if (const std::optional<long long> line = CanvasUi::position_field("##goto", m_goto, curr_line_id, total,
                                                                        {counter_font, ui.mono_font(CanvasUi::MonoStyle::Bold, CanvasUi::MonoSize::Counter),
                                                                         CanvasUi::Size(counter.width, button), s.counter_padding}))
            if (*line > 0 && go_to_line)
                go_to_line(std::min<uint64_t>(uint64_t(*line), total));
    }
}

void GCodePanel::render_body(const CanvasUi::Card& card, const Sizes& s, const RowGeometry& g, float body_top, float body_h,
                             uint64_t curr_line_id)
{
    const CanvasUi::Context& ui     = *CanvasUi::Context::current();
    const CanvasUi::Theme&   theme  = ui.theme();
    const CanvasUi::Painter  paint  = card.painter();
    {
        const CanvasUi::Painter::Clip clip(paint, CanvasUi::Rect(g.left, body_top, g.right, body_top + body_h));
        std::vector<StripeRing> rings;
        for (size_t i = 0; i < m_rows.size(); ++i) {
            const float top = body_top + float(i) * s.row;
            if (const std::optional<StripeRing> ring =
                    render_row(paint, m_rows[i], top, g, curr_line_id, body_row_background(card, m_rows[i], top, g, curr_line_id, s), s)) {
                StripeRing* last = rings.empty() ? nullptr : &rings.back();
                if (last != nullptr && last->rect.bottom == ring->rect.top && last->color == ring->color)
                    last->rect.bottom = ring->rect.bottom;
                else
                    rings.push_back(*ring);
            }
        }
        for (const StripeRing& ring : rings)
            paint.stroke_rect(ring.rect, theme.contrast_outline);

        const float track = body_top + s.position_indicator_inset;
        const float span  = body_h - 2.f * s.position_indicator_inset;
        if (const std::optional<GCodePositionThumb> thumb =
                gcode_position_thumb(m_rows.front().line, m_rows.back().line, m_lines_ends.size(), track, span, s.position_indicator_thumb)) {
            const float x = card.rect().right - s.position_indicator_inset - s.position_indicator;
            paint.fill_rect(CanvasUi::Rect(x, track, x + s.position_indicator, track + span), theme.position_indicator, .5f * s.position_indicator);
            paint.fill_rect(CanvasUi::Rect(x, thumb->top, x + s.position_indicator, thumb->bottom), theme.position_indicator_thumb, .5f * s.position_indicator);
        }
    }

    const CanvasUi::Pointer mouse = CanvasUi::pointer();
    if (card.hovered() && mouse.context_pressed && mouse.pos.y >= body_top && mouse.pos.y < body_top + body_h)
        open_copy_menu_at(m_rows[std::min(m_rows.size() - 1, size_t((mouse.pos.y - body_top) / s.row))].line, mouse.pos);
}

void GCodePanel::render(const PreviewPanelPlacement& place, float bottom, uint64_t curr_line_id)
{
    m_last_rect = CanvasUi::Rect();
    if (m_filename.empty() || m_lines_ends.empty() || curr_line_id == 0 || curr_line_id > m_lines_ends.size())
        return;
    if (collapse.take_pending())
        m_goto.editing = false;

    const CanvasUi::Context& ui       = *CanvasUi::Context::current();
    const CanvasUi::Metrics& m        = ui.metrics();
    const Sizes              s(m);
    // The rows scroll by line rather than in a scroll area, so the frame gives the card and the header, and the body is drawn here.
    CanvasUi::PanelCardOptions options;
    options.at         = CanvasUi::Point(place.x, place.top);
    options.right      = place.is_right();
    options.room       = bottom - place.top;
    const float width  = options.card_width(m);
    const float header = m.panel_header;
    const float           char_w   = ui.mono_font(CanvasUi::MonoStyle::Regular, m_settings.font_size).width("0");
    const float           digits   = float(std::max<size_t>(6, std::to_string(m_lines_ends.size()).size()));
    const float           number_w = ui.mono_font(CanvasUi::MonoStyle::Bold, CanvasUi::MonoSize::Caption).width("0");
    // Relative to the card's left edge until the card is placed.
    RowGeometry g = row_geometry(s, 0.f, width, std::max(s.number_min, digits * number_w + s.number_gap) - s.number_gap,
                                 s.text_right, s.row, char_w);
    CanvasUi::collapsible_close_on_escape(m_settings_open);

    std::optional<CanvasUi::SettingsFormOptions> form;
    if (m_settings_open)
        form = settings_form(width);
    const float room             = bottom - place.top - header;
    const float settings_h       = form ? settings_view_height(CanvasUi::settings_form_height(m, CanvasUi::FontMeasure(), CanvasUi::ChoiceMeasure(), *form), room, s.row,
                                                               collapse.collapsed() ? 0 : 3)
                                        : 0.f;
    const GCodeBodySize body = collapse.collapsed() ? GCodeBodySize{} : gcode_body_size(room - settings_h, s.row, m_settings.lines);
    const size_t tag_padding = size_t(std::ceil(s.tag_badge_room(char_w) / char_w));
    if (m_chars != g.chars || m_tag_padding != tag_padding) {
        m_chars       = g.chars;
        m_tag_padding = tag_padding;
        m_rows_count  = 0;
    }
    std::vector<GCodeSpan> current_spans;
    try {
        update_rows(curr_line_id, body.rows);
        if (collapse.collapsed())
            current_spans = layout_gcode_line(parse_gcode_line(read_line(curr_line_id)), false, m_settings.comments, m_tag_padding);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Cannot show the G-code window for " << m_filename << ": " << e.what();
        m_rows.clear();
        m_rows_count = 0;
        return;
    }
    const float body_h = std::min(float(m_rows.size()) * s.row, body.height);
    const float height = header + settings_h + body_h;

    options.extra = settings_h;
    float                       rows_height = body_h;
    const CanvasUi::PanelCard   frame("G-code", options, rows_height);
    const CanvasUi::Card&       card   = frame.card();
    const CanvasUi::Point       origin = frame.origin();

    render_header(frame, s, curr_line_id, std::move(current_spans), g);
    if (!collapse.collapsed() || m_settings_open)
        card.painter().divider(origin.x, origin.x + width, origin.y + header);

    if (!m_rows.empty()) {
        g.left  = origin.x + m.outline;
        g.right = origin.x + width - m.outline;
        g.number_right += origin.x;
        g.stripe_x += origin.x;
        g.text_left += origin.x;
        render_body(card, s, g, origin.y + header + settings_h, body_h, curr_line_id);
    } else if (collapse.collapsed() && card.hovered() && CanvasUi::pointer().context_pressed)
        open_copy_menu_at(curr_line_id, CanvasUi::pointer().pos);
    if (settings_h >= 1.f) {
        form->height = settings_h;
        form->below  = m_rows.empty() ? 0.f : body_h;
        if (CanvasUi::settings_form("##settings", CanvasUi::Point(origin.x, origin.y + header), *form)) {
            if (save_settings)
                save_settings(m_settings);
            m_rows_count = 0;
            CanvasUi::Context::current()->request_frame();
        }
    }
    render_copy_menu();

    m_last_rect = {origin.x, origin.y, origin.x + width, origin.y + height};
}

void GCodePanel::stop_mapping_file()
{
    if (m_file.is_open()) {
        m_file.close();
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": finished mapping file " << m_filename;
    }
}

} // namespace GUI
} // namespace Slic3r
