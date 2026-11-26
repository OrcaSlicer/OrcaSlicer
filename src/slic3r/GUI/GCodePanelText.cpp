#include "GCodePanelText.hpp"

#include <algorithm>
#include <cmath>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/CanvasUi/Font.hpp"
#include "slic3r/GUI/CanvasUi/Measure.hpp"
#include "slic3r/GUI/CanvasUi/NumberField.hpp"
#include "slic3r/GUI/CanvasUi/Theme.hpp"
#include "slic3r/GUI/CanvasUi/Tokens.hpp"
#include "slic3r/GUI/CanvasUi/Types.hpp"

namespace Slic3r {
namespace GUI {

size_t gcode_text_length(std::string_view text)
{
    return size_t(std::count_if(text.begin(), text.end(), [](char c) { return !CanvasUi::is_utf8_continuation(c); }));
}

size_t GCodeSpan::end() const { return column + gcode_text_length(text) + padding; }

static size_t byte_offset(std::string_view text, size_t chars)
{
    size_t i = 0;
    for (; i < text.size() && chars > 0; --chars)
        do
            ++i;
        while (i < text.size() && CanvasUi::is_utf8_continuation(text[i]));
    return i;
}

static std::string_view trim(std::string_view s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return s;
}

static bool is_letter_word(std::string_view token)
{
    if (token.size() < 2 || !std::isalpha(static_cast<unsigned char>(token.front())))
        return false;
    return std::all_of(token.begin() + 1, token.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '+'; });
}

static GCodeCommandKind command_kind(std::string_view command)
{
    if (command == "G0" || command == "G1" || command == "G2" || command == "G3")
        return GCodeCommandKind::Move;
    if (command.size() >= 2 && (command.front() == 'G' || command.front() == 'M' || command.front() == 'T') &&
        std::all_of(command.begin() + 1, command.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) || c == '.'; }))
        return command.front() == 'G' ? GCodeCommandKind::Machine : GCodeCommandKind::Macro;
    return GCodeCommandKind::Macro;
}

bool is_gcode_tag_comment(std::string_view comment)
{
    static constexpr std::array<std::string_view, 16> prefixes = {
        "TYPE:",           "FEATURE:",         "WIPE_START",  "WIPE_END",       "WIPE_TOWER_START", "WIPE_TOWER_END",
        "LAYER_CHANGE",    "CHANGE_LAYER",     "Z:",          "HEIGHT:",        "LAYER_HEIGHT:",    "WIDTH:",
        "LINE_WIDTH:",     "COLOR_CHANGE",     "PAUSE_PRINT", "CUSTOM_GCODE"};
    comment = trim(comment);
    if (std::any_of(prefixes.begin(), prefixes.end(), [comment](std::string_view p) { return comment.substr(0, p.size()) == p; }))
        return true;
    const auto ends_with = [comment](std::string_view s) { return comment.size() >= s.size() && comment.substr(comment.size() - s.size()) == s; };
    return comment.find(' ') == std::string_view::npos && (ends_with("_BLOCK_START") || ends_with("_BLOCK_END"));
}

GCodeLineParts parse_gcode_line(std::string_view line)
{
    GCodeLineParts parts;
    const size_t   semicolon = line.find(';');
    if (semicolon != std::string_view::npos) {
        parts.has_comment = true;
        parts.comment     = std::string(trim(line.substr(semicolon + 1)));
        parts.tag      = is_gcode_tag_comment(parts.comment);
        line              = line.substr(0, semicolon);
    }
    line = trim(line);
    if (line.empty())
        return parts;

    const size_t space = line.find_first_of(" \t");
    parts.command      = std::string(line.substr(0, space));
    parts.kind         = command_kind(parts.command);
    std::string_view rest = space == std::string_view::npos ? std::string_view() : trim(line.substr(space));
    if (parts.kind == GCodeCommandKind::Macro && parts.command.find('_') != std::string::npos) {
        parts.arguments = std::string(rest);
        return parts;
    }
    while (!rest.empty()) {
        const size_t           end   = rest.find_first_of(" \t");
        const std::string_view token = rest.substr(0, end);
        if (is_letter_word(token))
            parts.words.push_back({char(std::toupper(static_cast<unsigned char>(token.front()))), std::string(token.substr(1))});
        else {
            parts.arguments = std::string(rest);
            break;
        }
        rest = end == std::string_view::npos ? std::string_view() : trim(rest.substr(end));
    }
    return parts;
}

bool gcode_line_shown(const GCodeLineParts &parts, GCodeCommentMode mode)
{
    if (!parts.comment_only())
        return true;
    return mode == GCodeCommentMode::All || (mode == GCodeCommentMode::Tags && parts.tag);
}

static size_t aligned_index(char letter) { return GCODE_ALIGNED_LETTERS.find(letter); }

using GCodeColumnWidths = std::array<size_t, GCODE_ALIGNED_LETTERS.size()>;

static GCodeColumnWidths line_columns(const GCodeLineParts &parts)
{
    static constexpr GCodeColumnWidths fixed  = {9, 9, 8, 8, 7};
    GCodeColumnWidths                  widths = {fixed[0], fixed[1], 0, 0, 0};
    for (const GCodeWord &word : parts.words)
        if (const size_t i = aligned_index(word.letter); i != std::string_view::npos)
            widths[i] = fixed[i];
    return widths;
}

std::vector<GCodeSpan> layout_gcode_line(const GCodeLineParts &parts, bool align, GCodeCommentMode mode, size_t tag_padding)
{
    std::vector<GCodeSpan> spans;
    size_t                 column = 0;
    const auto add = [&spans, &column](std::string text, GCodeSpanStyle style, char letter = 0) {
        if (text.empty())
            return;
        const size_t length = gcode_text_length(text);
        spans.push_back({column, std::move(text), style, letter});
        column += length;
    };
    const auto gap = [&column, &spans]() {
        if (!spans.empty())
            column += 1;
    };

    if (!parts.command.empty()) {
        add(parts.command, parts.kind == GCodeCommandKind::Macro ? GCodeSpanStyle::Macro : GCodeSpanStyle::Command);
        const bool aligned = align && parts.kind == GCodeCommandKind::Move &&
                             std::all_of(parts.words.begin(), parts.words.end(),
                                         [](const GCodeWord &word) { return aligned_index(word.letter) != std::string_view::npos; });
        std::vector<const GCodeWord *> extra;
        if (aligned) {
            const GCodeColumnWidths columns = line_columns(parts);
            size_t start = std::max<size_t>(parts.command.size() + 1, 4);
            std::array<const GCodeWord *, GCODE_ALIGNED_LETTERS.size()> by_column{};
            for (const GCodeWord &word : parts.words) {
                const size_t i = aligned_index(word.letter);
                if (i != std::string_view::npos && by_column[i] == nullptr)
                    by_column[i] = &word;
                else
                    extra.push_back(&word);
            }
            for (size_t i = 0; i < by_column.size(); ++i) {
                if (columns[i] == 0)
                    continue;
                size_t next = start + columns[i];
                if (const GCodeWord *word = by_column[i]) {
                    spans.push_back({start, std::string(1, word->letter), GCodeSpanStyle::Letter, word->letter});
                    spans.push_back({start + 1, word->value, GCodeSpanStyle::Value, word->letter});
                    next = std::max(next, spans.back().end() + 1);
                }
                start = next;
            }
            column = start - 1;
        } else
            for (const GCodeWord &word : parts.words)
                extra.push_back(&word);
        for (const GCodeWord *word : extra) {
            gap();
            add(std::string(1, word->letter), GCodeSpanStyle::Letter, word->letter);
            add(word->value, GCodeSpanStyle::Value, word->letter);
        }
        if (!parts.arguments.empty()) {
            std::string_view rest = parts.arguments;
            while (!rest.empty()) {
                const size_t     end   = rest.find(' ');
                std::string_view token = rest.substr(0, end);
                rest                   = end == std::string_view::npos ? std::string_view() : trim(rest.substr(end));
                if (token.empty())
                    continue;
                gap();
                if (const size_t eq = token.find('='); eq != std::string_view::npos && eq > 0) {
                    add(std::string(token.substr(0, eq + 1)), GCodeSpanStyle::Key);
                    add(std::string(token.substr(eq + 1)), GCodeSpanStyle::Argument);
                } else
                    add(std::string(token), GCodeSpanStyle::Argument);
            }
        }
    }

    if (parts.has_comment && mode != GCodeCommentMode::None && (mode == GCodeCommentMode::All || parts.tag)) {
        gap();
        if (parts.tag) {
            const size_t before = spans.size();
            add(parts.comment, GCodeSpanStyle::Tag);
            if (spans.size() > before)
                spans.back().padding = tag_padding;
        }
        else
            add("; " + parts.comment, GCodeSpanStyle::Comment);
    }
    return spans;
}

std::vector<std::vector<GCodeSpan>> wrap_gcode_spans(const std::vector<GCodeSpan> &spans, size_t width)
{
    std::vector<std::vector<GCodeSpan>> rows(1);
    width = std::max<size_t>(width, 1);
    size_t shift = 0;
    for (size_t first = 0; first < spans.size();) {
        size_t last = first + 1;
        while (last < spans.size() && spans[last].column == spans[last - 1].end())
            ++last;
        const size_t word_start = spans[first].column - shift;
        const size_t word_end   = spans[last - 1].end() - shift;
        if (word_end > width && word_start > 0 && word_end - word_start <= width) {
            shift += word_start;
            rows.emplace_back();
        }
        for (size_t i = first; i < last; ++i) {
            GCodeSpan span = spans[i];
            span.column -= shift;
            span.padding = std::min(span.padding, width);
            while (span.end() > width) {
                if (span.column >= width) {
                    shift += span.column;
                    span.column = 0;
                    rows.emplace_back();
                    continue;
                }
                const size_t room = width - span.column;
                // The text fits and only the badge around it does not: the badge is cut to the row.
                if (gcode_text_length(span.text) <= room) {
                    span.padding = room - gcode_text_length(span.text);
                    break;
                }
                const size_t fits = byte_offset(span.text, room);
                GCodeSpan    head = span;
                head.text         = span.text.substr(0, fits);
                head.padding      = 0;
                rows.back().push_back(head);
                span.text   = span.text.substr(fits);
                shift      += width;
                span.column = 0;
                rows.emplace_back();
            }
            if (!span.text.empty())
                rows.back().push_back(span);
        }
        first = last;
    }
    return rows;
}

GCodePanelSettings load_gcode_panel_settings(const AppConfig &config)
{
    const auto value = [&config](const std::string &name) { return config.get("gcode_panel_" + name); };
    const auto flag  = [&value](const std::string &name, bool fallback) {
        const std::string text = value(name);
        return AppConfig::is_true(text) ? true : text == "false" || text == "0" ? false : fallback;
    };
    const auto number = [&value](const std::string &name) { return CanvasUi::parse_integer(value(name)).value_or(-1); };

    const GCodePanelSettings defaults;
    GCodePanelSettings       settings;
    const size_t lines         = size_t(std::max<long long>(0, number("lines")));
    settings.lines             = std::find(GCODE_PANEL_LINES.begin(), GCODE_PANEL_LINES.end(), lines) != GCODE_PANEL_LINES.end() ? lines : defaults.lines;
    const auto font            = std::find(CanvasUi::MONO_FONT_SIZES.begin(), CanvasUi::MONO_FONT_SIZES.end(), number("font_size"));
    settings.font_size         = font != CanvasUi::MONO_FONT_SIZES.end() ? CanvasUi::MonoSize(font - CanvasUi::MONO_FONT_SIZES.begin()) : defaults.font_size;
    settings.upper_third       = value("current_line") == "upper_third";
    settings.wrap              = value("long_lines") == "wrap";
    const std::string comments = value("comments");
    settings.comments          = comments == "tags" ? GCodeCommentMode::Tags : comments == "none" ? GCodeCommentMode::None : GCodeCommentMode::All;
    settings.align             = flag("align", defaults.align);
    settings.color_axes        = flag("axis_colors", defaults.color_axes);
    settings.stripe            = flag("stripe", defaults.stripe);
    settings.shade_unprinted   = flag("shade_unprinted", defaults.shade_unprinted);
    return settings;
}

void save_gcode_panel_settings(AppConfig &config, const GCodePanelSettings &settings)
{
    config.set("gcode_panel_lines", std::to_string(settings.lines));
    config.set("gcode_panel_font_size", std::to_string(CanvasUi::MONO_FONT_SIZES[size_t(settings.font_size)]));
    config.set("gcode_panel_current_line", settings.upper_third ? "upper_third" : "center");
    config.set("gcode_panel_long_lines", settings.wrap ? "wrap" : "clip");
    config.set("gcode_panel_comments", settings.comments == GCodeCommentMode::Tags ? "tags" : settings.comments == GCodeCommentMode::None ? "none" : "all");
    config.set_bool("gcode_panel_align", settings.align);
    config.set_bool("gcode_panel_axis_colors", settings.color_axes);
    config.set_bool("gcode_panel_stripe", settings.stripe);
    config.set_bool("gcode_panel_shade_unprinted", settings.shade_unprinted);
}

size_t changed_settings(const GCodePanelSettings &settings)
{
    const GCodePanelSettings defaults;
    return size_t(settings.lines != defaults.lines) + size_t(settings.upper_third != defaults.upper_third) +
           size_t(settings.wrap != defaults.wrap) + size_t(settings.comments != defaults.comments) +
           size_t(settings.font_size != defaults.font_size) + size_t(settings.align != defaults.align) +
           size_t(settings.color_axes != defaults.color_axes) + size_t(settings.stripe != defaults.stripe) +
           size_t(settings.shade_unprinted != defaults.shade_unprinted);
}

float settings_view_height(float content_height, float room, float row_height, size_t min_rows)
{
    return std::floor(std::min(content_height, std::max(0.f, room - float(min_rows) * row_height)));
}

GCodeBodySize gcode_body_size(float room, float row_height, size_t lines)
{
    room = std::floor(std::max(0.f, room));
    if (lines == 0)
        return {size_t(std::ceil(room / row_height)), room};
    const size_t rows = std::min(lines, size_t(room / row_height));
    return {rows, float(rows) * row_height};
}

std::pair<size_t, size_t> gcode_line_range(const std::vector<size_t> &lines_ends, size_t file_size, uint64_t id)
{
    if (id == 0 || id > lines_ends.size())
        return {0, 0};
    const size_t start = id == 1 ? 0 : std::min(lines_ends[id - 2], file_size);
    return {start, std::max(start, std::min(lines_ends[id - 1], file_size))};
}

std::string gcode_copy_text(const std::vector<GCodeRow> &rows, uint64_t line, const std::function<std::string(uint64_t)> &read_line)
{
    const auto trimmed = [&read_line](uint64_t id) {
        std::string text = read_line(id);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
            text.pop_back();
        return text;
    };
    if (rows.empty())
        return trimmed(line);
    std::string text;
    uint64_t    last = 0;
    for (const GCodeRow &row : rows) {
        if (row.line == last)
            continue;
        if (!text.empty())
            text += '\n';
        text += trimmed(row.line);
        last = row.line;
    }
    return text;
}

const GCodePalette &gcode_palette(bool dark)
{
    using CanvasUi::rgb;
    static const GCodePalette light{rgb(0xe0504a), rgb(0x3f9a44), rgb(0x3d86d1), rgb(0xcf6a1f), rgb(0x868c90), rgb(0x6a4c9c),
                                    rgb(0x3d4548), rgb(0x5f666a), rgb(0xf4f5f6)};
    static const GCodePalette dark_palette{rgb(0xd9534f), rgb(0x3e9443), rgb(0x4a86c8), rgb(0xb86e26), rgb(0x7d8188), rgb(0xb9a6d8),
                                           rgb(0xc8c8ce), rgb(0xb4b4ba), rgb(0x262629)};
    return dark ? dark_palette : light;
}

CanvasUi::Color axis_color(const GCodePalette& palette, const CanvasUi::Theme& theme, char letter)
{
    switch (letter) {
    case 'X': return palette.x;
    case 'Y': return palette.y;
    case 'Z': return palette.z;
    case 'E': return palette.e;
    case 'F': return palette.f;
    default: return theme.muted_foreground;
    }
}

std::optional<GCodePositionThumb> gcode_position_thumb(uint64_t first, uint64_t last, uint64_t total, float track_top, float track_height,
                                                       float thumb_height)
{
    if (first == 0 || last < first || (first == 1 && last >= total))
        return std::nullopt;
    const uint64_t shown = last - first + 1;
    const float    thumb = std::min(thumb_height, track_height);
    const double   share = total > shown ? double(first - 1) / double(total - shown) : 1.0;
    const float    top   = track_top + (track_height - thumb) * float(std::clamp(share, 0.0, 1.0));
    return GCodePositionThumb{top, top + thumb};
}

std::vector<GCodeRow> gcode_rows_around(uint64_t current, uint64_t total, size_t count, const GCodePanelSettings &settings, size_t chars,
                                        size_t tag_padding, const std::function<std::string(uint64_t)> &read_line)
{
    if (count == 0 || current == 0 || current > total)
        return {};
    // Read past the rows shown, so that wrapped and hidden lines still leave enough of them.
    const size_t           reach = count + 8;
    const GCodeCommentMode mode  = settings.comments;
    std::vector<std::pair<uint64_t, GCodeLineParts>> above;
    for (uint64_t id = current; id > 1 && above.size() < reach;) {
        GCodeLineParts parts = parse_gcode_line(read_line(--id));
        if (gcode_line_shown(parts, mode))
            above.emplace_back(id, std::move(parts));
    }
    std::vector<std::pair<uint64_t, GCodeLineParts>> lines(above.rbegin(), above.rend());
    lines.emplace_back(current, parse_gcode_line(read_line(current)));
    for (uint64_t id = current + 1; id <= total && lines.size() < above.size() + 1 + reach; ++id) {
        GCodeLineParts parts = parse_gcode_line(read_line(id));
        if (gcode_line_shown(parts, mode))
            lines.emplace_back(id, std::move(parts));
    }

    std::vector<GCodeRow> rows;
    size_t                current_row = 0;
    for (const auto &[id, line] : lines) {
        if (id == current)
            current_row = rows.size();
        std::vector<GCodeSpan> spans = layout_gcode_line(line, settings.align, mode, tag_padding);
        if (settings.wrap) {
            bool first = true;
            for (std::vector<GCodeSpan> &row : wrap_gcode_spans(spans, chars)) {
                rows.push_back({id, first, false, std::move(row)});
                first = false;
            }
        } else {
            const bool overflow = std::any_of(spans.begin(), spans.end(), [chars](const GCodeSpan &span) { return span.end() > chars; });
            rows.push_back({id, true, overflow, std::move(spans)});
        }
    }

    const size_t before = settings.upper_third ? (count - 1) / 3 : (count - 1) / 2;
    size_t       first  = current_row - std::min(before, current_row);
    const size_t last   = std::min(rows.size(), first + count);
    first               = last > count ? std::min(first, last - count) : 0;
    return std::vector<GCodeRow>(std::make_move_iterator(rows.begin() + first), std::make_move_iterator(rows.begin() + last));
}

} // namespace GUI
} // namespace Slic3r
