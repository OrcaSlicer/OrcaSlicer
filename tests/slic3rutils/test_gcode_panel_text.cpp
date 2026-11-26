#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/GCodePanelText.hpp"
#include "slic3r/GUI/CanvasUi/Font.hpp"
#include "slic3r/GUI/CanvasUi/Theme.hpp"
#include "slic3r/GUI/CanvasUi/Types.hpp"

#include <cstddef>
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r::GUI;

namespace {

std::string row_text(const std::vector<GCodeSpan> &spans)
{
    std::string text;
    for (const GCodeSpan &span : spans) {
        if (text.size() < span.column)
            text.resize(span.column, ' ');
        text.replace(span.column, span.text.size(), span.text);
    }
    return text;
}

// A line of G-code as the panel lays it out, in text.
std::string laid_out(const std::string &line, bool align, GCodeCommentMode comments)
{
    return row_text(layout_gcode_line(parse_gcode_line(line), align, comments));
}

// A file of n moves, "G1 X<id>", with a plain comment on every line listed in comments.
std::vector<std::string> numbered_file(size_t n, const std::vector<uint64_t> &comments = {})
{
    std::vector<std::string> lines;
    for (uint64_t id = 1; id <= n; ++id)
        lines.push_back(std::find(comments.begin(), comments.end(), id) != comments.end() ? "; note" : "G1 X" + std::to_string(id));
    return lines;
}

std::vector<uint64_t> row_lines(const std::vector<GCodeRow> &rows)
{
    std::vector<uint64_t> ids;
    for (const GCodeRow &row : rows)
        ids.push_back(row.line);
    return ids;
}

std::vector<GCodeRow> rows_of(const std::vector<std::string> &file, uint64_t current, size_t count, const GCodePanelSettings &settings,
                              size_t chars = 80)
{
    return gcode_rows_around(current, file.size(), count, settings, chars, 0, [&file](uint64_t id) { return file[id - 1]; });
}

} // namespace

TEST_CASE("A move splits into its command and letter words", "[GCodePanelText]")
{
    const GCodeLineParts p = parse_gcode_line("G1 X114.898 Y136.342 E.02065");

    CHECK(p.command == "G1");
    CHECK(p.kind == GCodeCommandKind::Move);
    REQUIRE(p.words.size() == 3);
    CHECK(p.words[0].letter == 'X');
    CHECK(p.words[0].value == "114.898");
    CHECK(p.words[2].letter == 'E');
    CHECK(p.words[2].value == ".02065");
    CHECK_FALSE(p.has_comment);
}

TEST_CASE("A macro keeps its arguments as written", "[GCodePanelText]")
{
    const GCodeLineParts p = parse_gcode_line("SET_VELOCITY_LIMIT ACCEL=3000 ACCEL_TO_DECEL=1500");

    CHECK(p.kind == GCodeCommandKind::Macro);
    CHECK(p.words.empty());
    CHECK(p.arguments == "ACCEL=3000 ACCEL_TO_DECEL=1500");
}

TEST_CASE("An M-code is shown like a macro", "[GCodePanelText]")
{
    const GCodeLineParts p = parse_gcode_line("M106 S0");

    CHECK(p.kind == GCodeCommandKind::Macro);
    REQUIRE(p.words.size() == 1);
    CHECK(p.words[0].letter == 'S');
}

TEST_CASE("Tag comments are told apart from plain ones", "[GCodePanelText]")
{
    CHECK(parse_gcode_line(";WIPE_START").tag);
    CHECK(parse_gcode_line("; WIPE_END").tag);
    CHECK(parse_gcode_line(";TYPE:Outer wall").tag);
    CHECK(parse_gcode_line("; EXECUTABLE_BLOCK_END").tag);
    CHECK_FALSE(parse_gcode_line("; stop printing object Cube id:0 copy 0").tag);
    CHECK_FALSE(parse_gcode_line("; filament end gcode").tag);
}

TEST_CASE("A trailing comment stays with its command", "[GCodePanelText]")
{
    const GCodeLineParts p = parse_gcode_line("G1 F6000 ; travel");

    CHECK(p.command == "G1");
    CHECK(p.has_comment);
    CHECK(p.comment == "travel");
    CHECK_FALSE(p.comment_only());
}

TEST_CASE("The comment mode hides plain comment lines but keeps tag comments", "[GCodePanelText]")
{
    const GCodeLineParts plain  = parse_gcode_line("; stop printing object");
    const GCodeLineParts marker = parse_gcode_line(";WIPE_START");
    const GCodeLineParts move   = parse_gcode_line("G1 X1 ; note");

    CHECK(gcode_line_shown(plain, GCodeCommentMode::All));
    CHECK_FALSE(gcode_line_shown(plain, GCodeCommentMode::Tags));
    CHECK(gcode_line_shown(marker, GCodeCommentMode::Tags));
    CHECK_FALSE(gcode_line_shown(marker, GCodeCommentMode::None));
    CHECK(gcode_line_shown(move, GCodeCommentMode::None));
    CHECK(row_text(layout_gcode_line(move, false, GCodeCommentMode::None)) == "G1 X1");
}

TEST_CASE("Aligned moves keep X and Y in the same columns whatever their other words", "[GCodePanelText]")
{
    const std::string plain   = laid_out("G1 X114.898 Y136.342 E.02065", true, GCodeCommentMode::All);
    const std::string feed    = laid_out("G1 F6000", true, GCodeCommentMode::All);
    const std::string travel  = laid_out("G1 X114.388 Y136.342 F21000", true, GCodeCommentMode::All);
    const std::string lift    = laid_out("G1 X1.5 Y2.5 Z.4 E.1", true, GCodeCommentMode::All);

    CHECK(plain.find('Y') == travel.find('Y'));
    CHECK(plain.find('Y') == lift.find('Y'));
    CHECK(feed.find('F') == travel.find('F'));
    CHECK(plain.find('E') == travel.find('F'));
    CHECK(lift.find('E') > plain.find('E'));
}

TEST_CASE("Without alignment the words follow each other", "[GCodePanelText]")
{
    CHECK(laid_out("G1 F6000", false, GCodeCommentMode::All) == "G1 F6000");
}

TEST_CASE("Macro keys and values become separate spans", "[GCodePanelText]")
{
    const std::vector<GCodeSpan> spans = layout_gcode_line(parse_gcode_line("SET_VELOCITY_LIMIT ACCEL=3000"), false, GCodeCommentMode::All);

    REQUIRE(spans.size() == 3);
    CHECK(spans[0].style == GCodeSpanStyle::Macro);
    CHECK(spans[1].style == GCodeSpanStyle::Key);
    CHECK(spans[1].text == "ACCEL=");
    CHECK(spans[2].style == GCodeSpanStyle::Argument);
}

TEST_CASE("A long line wraps its last word to the next row", "[GCodePanelText]")
{
    const std::vector<GCodeSpan> spans = layout_gcode_line(parse_gcode_line("SET_VELOCITY_LIMIT ACCEL=3000 ACCEL_TO_DECEL=1500"), false,
                                                           GCodeCommentMode::All);
    const std::vector<std::vector<GCodeSpan>> rows = wrap_gcode_spans(spans, 30);

    REQUIRE(rows.size() == 2);
    for (const auto &row : rows)
        for (const GCodeSpan &span : row)
            CHECK(span.end() <= 30);
    CHECK(row_text(rows[0]) == "SET_VELOCITY_LIMIT ACCEL=3000");
    CHECK(row_text(rows[1]) == "ACCEL_TO_DECEL=1500");
}

TEST_CASE("A word longer than the width is cut across rows", "[GCodePanelText]")
{
    const std::vector<GCodeSpan> spans = layout_gcode_line(parse_gcode_line("; " + std::string(25, 'a')), false, GCodeCommentMode::All);
    const std::vector<std::vector<GCodeSpan>> rows = wrap_gcode_spans(spans, 10);

    REQUIRE(rows.size() == 3);
    CHECK(row_text(rows[0]) == "; aaaaaaaa");
    CHECK(row_text(rows[1]) == "aaaaaaaaaa");
}

TEST_CASE("Columns count characters, not bytes", "[GCodePanelText]")
{
    const std::vector<GCodeSpan> spans = layout_gcode_line(parse_gcode_line("G1 F6000 ; привет"), false, GCodeCommentMode::All);

    REQUIRE(spans.size() == 4);
    CHECK(spans[3].column == 9);
    CHECK(spans[3].end() == 17);
    CHECK(gcode_text_length("привет") == 6);
}

TEST_CASE("Aligned columns have fixed widths after a four-character command", "[GCodePanelText]")
{
    CHECK(laid_out("G1 X113.658 Y135.612 E.01215", true, GCodeCommentMode::All) == "G1  X113.658 Y135.612 E.01215");
    CHECK(laid_out("G1 E-.56 F1800", true, GCodeCommentMode::All) == "G1                    E-.56   F1800");
}

TEST_CASE("A value wider than its column pushes the next one", "[GCodePanelText]")
{
    const std::string row = laid_out("G1 X12345.6789 Y1", true, GCodeCommentMode::All);

    CHECK(row.find('Y') > row.find("6789") + 4);
}

TEST_CASE("Wrapping keeps a word and a macro key with its value together", "[GCodePanelText]")
{
    const std::vector<GCodeSpan> spans = layout_gcode_line(parse_gcode_line("SET_VELOCITY_LIMIT ACCEL=3000 ACCEL_TO_DECEL=1500"), false,
                                                           GCodeCommentMode::All);
    const std::vector<std::vector<GCodeSpan>> rows = wrap_gcode_spans(spans, 27);

    REQUIRE(rows.size() == 3);
    CHECK(row_text(rows[0]) == "SET_VELOCITY_LIMIT");
    CHECK(row_text(rows[1]) == "ACCEL=3000");
    CHECK(row_text(rows[2]) == "ACCEL_TO_DECEL=1500");
}

TEST_CASE("An arc keeps the order of its words instead of being aligned", "[GCodePanelText]")
{
    CHECK(laid_out("G2 X10 Y10 I5 J5 F3000", true, GCodeCommentMode::All) == "G2 X10 Y10 I5 J5 F3000");
}

TEST_CASE("Open settings leave three rows of code under them and scroll when the room is short", "[GCodePanelText]")
{
    CHECK_THAT(settings_view_height(300.f, 600.f, 18.f, 3), Catch::Matchers::WithinAbs(300.f, 1e-4));
    CHECK_THAT(settings_view_height(300.f, 200.f, 18.f, 3), Catch::Matchers::WithinAbs(146.f, 1e-4));
    CHECK_THAT(settings_view_height(300.f, 40.f, 18.f, 3), Catch::Matchers::WithinAbs(0.f, 1e-4));
    CHECK_THAT(settings_view_height(300.5f, 900.f, 18.f, 3), Catch::Matchers::WithinAbs(300.f, 1e-4));
}

TEST_CASE("With Lines Auto the code reaches the bottom of its room, and a set count shows only whole rows", "[GCodePanelText]")
{
    for (const float settings : {0.f, 146.f, 152.f}) {
        const GCodeBodySize auto_body = gcode_body_size(700.5f - settings, 18.f, 0);
        CHECK_THAT(settings + auto_body.height, Catch::Matchers::WithinAbs(700.f, 1e-4));
        CHECK(float(auto_body.rows) * 18.f >= auto_body.height);
        CHECK(float(auto_body.rows - 1) * 18.f < auto_body.height);
    }
    CHECK(gcode_body_size(540.f, 18.f, 0).rows == 30);

    const GCodeBodySize set = gcode_body_size(550.f, 18.f, 35);
    CHECK(set.rows == 30);
    CHECK_THAT(set.height, Catch::Matchers::WithinAbs(540.f, 1e-4));
    CHECK(gcode_body_size(550.f, 18.f, 12).rows == 12);
    CHECK(gcode_body_size(-5.f, 18.f, 0).rows == 0);
}

TEST_CASE("A tag comment takes the columns of its badge, and wraps whole when the badge does not fit", "[GCodePanelText]")
{
    const std::vector<GCodeSpan> spans = layout_gcode_line(parse_gcode_line("G1 X1 ;WIPE_START"), false, GCodeCommentMode::All, 4);
    REQUIRE_FALSE(spans.empty());
    const GCodeSpan& tag = spans.back();
    CHECK(tag.style == GCodeSpanStyle::Tag);
    CHECK(tag.end() == tag.column + std::string("WIPE_START").size() + 4);

    const std::vector<std::vector<GCodeSpan>> rows = wrap_gcode_spans(spans, tag.end() - 1);
    REQUIRE(rows.size() == 2);
    REQUIRE(rows.back().size() == 1);
    CHECK(rows.back().front().text == "WIPE_START");
    CHECK(rows.back().front().padding == 4);
}

TEST_CASE("A tag comment at the start of a line whose badge alone overflows keeps one row, its badge cut to it", "[GCodePanelText]")
{
    const std::vector<GCodeSpan> spans = layout_gcode_line(parse_gcode_line(";WIPE_START"), false, GCodeCommentMode::All, 4);
    REQUIRE(spans.size() == 1);
    const size_t width = spans.front().end() - 2;

    const std::vector<std::vector<GCodeSpan>> rows = wrap_gcode_spans(spans, width);
    REQUIRE(rows.size() == 1);
    REQUIRE(rows.front().size() == 1);
    CHECK(rows.front().front().text == "WIPE_START");
    CHECK(rows.front().front().end() == width);
}

TEST_CASE("A badge wider than the row still wraps into rows that end within it", "[GCodePanelText]")
{
    const std::vector<GCodeSpan> spans = layout_gcode_line(parse_gcode_line(";AB"), false, GCodeCommentMode::All, 4);
    REQUIRE(spans.size() == 1);

    const std::vector<std::vector<GCodeSpan>> rows = wrap_gcode_spans(spans, 3);
    REQUIRE_FALSE(rows.empty());
    for (const std::vector<GCodeSpan>& row : rows)
        for (const GCodeSpan& span : row)
            CHECK(span.end() <= 3);
}

TEST_CASE("The current line stands in the middle of the rows, or a third from the top", "[GCodePanelText]")
{
    const std::vector<std::string> file = numbered_file(100);
    GCodePanelSettings             settings;
    CHECK(row_lines(rows_of(file, 50, 5, settings)) == std::vector<uint64_t>{48, 49, 50, 51, 52});
    settings.upper_third = true;
    CHECK(row_lines(rows_of(file, 50, 7, settings)) == std::vector<uint64_t>{48, 49, 50, 51, 52, 53, 54});
}

TEST_CASE("At the ends of the file the rows stay inside it, the current line off the middle", "[GCodePanelText]")
{
    const std::vector<std::string> file = numbered_file(20);
    const GCodePanelSettings       settings;
    CHECK(row_lines(rows_of(file, 1, 5, settings)) == std::vector<uint64_t>{1, 2, 3, 4, 5});
    CHECK(row_lines(rows_of(file, 2, 5, settings)) == std::vector<uint64_t>{1, 2, 3, 4, 5});
    CHECK(row_lines(rows_of(file, 20, 5, settings)) == std::vector<uint64_t>{16, 17, 18, 19, 20});
    CHECK(row_lines(rows_of(numbered_file(3), 2, 5, settings)) == std::vector<uint64_t>{1, 2, 3});
}

TEST_CASE("Hidden comment lines take no rows, but the current line shows even when it is one", "[GCodePanelText]")
{
    const std::vector<std::string> file = numbered_file(30, {9, 11, 12});
    GCodePanelSettings             settings;
    settings.comments = GCodeCommentMode::None;
    CHECK(row_lines(rows_of(file, 10, 5, settings)) == std::vector<uint64_t>{7, 8, 10, 13, 14});
    CHECK(row_lines(rows_of(file, 11, 3, settings)) == std::vector<uint64_t>{10, 11, 13});
    settings.comments = GCodeCommentMode::All;
    CHECK(row_lines(rows_of(file, 10, 5, settings)) == std::vector<uint64_t>{8, 9, 10, 11, 12});
}

TEST_CASE("Wrapped lines take a row for each piece, above and below the current line", "[GCodePanelText]")
{
    std::vector<std::string> file = numbered_file(30);
    file[9 - 1]                   = "G1 X1.000 Y2.000 Z3.000 E4.000 F5000";
    file[11 - 1]                  = "G1 X1.000 Y2.000 Z3.000 E4.000 F5000";
    GCodePanelSettings settings;
    settings.wrap  = true;
    settings.align = false;
    const std::vector<GCodeRow> rows = rows_of(file, 10, 5, settings, 20);
    CHECK(row_lines(rows) == std::vector<uint64_t>{9, 9, 10, 11, 11});
    CHECK(rows[0].first);
    CHECK_FALSE(rows[1].first);
    CHECK(rows[2].first);

    settings.wrap                     = false;
    const std::vector<GCodeRow> clips = rows_of(file, 10, 3, settings, 20);
    CHECK(row_lines(clips) == std::vector<uint64_t>{9, 10, 11});
    CHECK(clips[0].overflow);
    CHECK_FALSE(clips[1].overflow);
}

TEST_CASE("With tag comments only, a plain comment after a command goes and a tag after it stays", "[GCodePanelText]")
{
    const GCodeLineParts plain = parse_gcode_line("G1 X1 ; a note");
    CHECK(gcode_line_shown(plain, GCodeCommentMode::Tags));
    CHECK(row_text(layout_gcode_line(plain, false, GCodeCommentMode::Tags)) == "G1 X1");
    CHECK(row_text(layout_gcode_line(plain, false, GCodeCommentMode::All)) == "G1 X1 ; a note");

    const std::vector<GCodeSpan> tagged = layout_gcode_line(parse_gcode_line("G1 X1 ;TYPE:Inner wall"), false, GCodeCommentMode::Tags);
    REQUIRE_FALSE(tagged.empty());
    CHECK(tagged.back().style == GCodeSpanStyle::Tag);
    CHECK(tagged.back().text == "TYPE:Inner wall");
}

TEST_CASE("Machine G-codes are commands with letter words, and a tool change reads like an M-code", "[GCodePanelText]")
{
    const GCodeLineParts home = parse_gcode_line("G28");
    CHECK(home.kind == GCodeCommandKind::Machine);
    CHECK(home.words.empty());
    const GCodeLineParts reset = parse_gcode_line("G92 E0");
    CHECK(reset.kind == GCodeCommandKind::Machine);
    REQUIRE(reset.words.size() == 1);
    CHECK(reset.words[0].letter == 'E');
    CHECK(layout_gcode_line(reset, true, GCodeCommentMode::All).front().style == GCodeSpanStyle::Command);

    const GCodeLineParts tool = parse_gcode_line("T0");
    CHECK(tool.kind == GCodeCommandKind::Macro);
    CHECK(tool.words.empty());
    CHECK(row_text(layout_gcode_line(tool, true, GCodeCommentMode::All)) == "T0");
}

TEST_CASE("Text cut across rows is cut between characters, not inside one", "[GCodePanelText]")
{
    const std::vector<GCodeSpan> spans = layout_gcode_line(parse_gcode_line("; Привет, мир"), false, GCodeCommentMode::All);
    const std::vector<std::vector<GCodeSpan>> rows = wrap_gcode_spans(spans, 6);
    REQUIRE(rows.size() == 3);
    CHECK(row_text(rows[0]) == "; Прив");
    CHECK(row_text(rows[1]) == "ет, ми");
    CHECK(row_text(rows[2]) == "р");
}

TEST_CASE("Each setting changed from its default counts once", "[GCodePanelText]")
{
    GCodePanelSettings settings;
    size_t             expected = 0;
    const auto         changed  = [&](auto change) {
        change(settings);
        CHECK(changed_settings(settings) == ++expected);
    };
    changed([](GCodePanelSettings &s) { s.lines = 15; });
    changed([](GCodePanelSettings &s) { s.upper_third = true; });
    changed([](GCodePanelSettings &s) { s.wrap = true; });
    changed([](GCodePanelSettings &s) { s.comments = GCodeCommentMode::Tags; });
    changed([](GCodePanelSettings &s) { s.font_size = Slic3r::GUI::CanvasUi::MonoSize::Caption; });
    changed([](GCodePanelSettings &s) { s.align = false; });
    changed([](GCodePanelSettings &s) { s.color_axes = false; });
    changed([](GCodePanelSettings &s) { s.stripe = false; });
    changed([](GCodePanelSettings &s) { s.shade_unprinted = false; });
    CHECK(changed_settings(GCodePanelSettings()) == 0);
}

namespace {

void check_same_settings(const GCodePanelSettings &read, const GCodePanelSettings &expected)
{
    CHECK(read.lines == expected.lines);
    CHECK(read.upper_third == expected.upper_third);
    CHECK(read.wrap == expected.wrap);
    CHECK(read.comments == expected.comments);
    CHECK(read.font_size == expected.font_size);
    CHECK(read.align == expected.align);
    CHECK(read.color_axes == expected.color_axes);
    CHECK(read.stripe == expected.stripe);
    CHECK(read.shade_unprinted == expected.shade_unprinted);
}

GCodePanelSettings every_setting_changed()
{
    GCodePanelSettings changed;
    changed.lines           = 25;
    changed.upper_third     = true;
    changed.wrap            = true;
    changed.comments        = GCodeCommentMode::None;
    changed.font_size       = CanvasUi::MonoSize::Caption;
    changed.align           = false;
    changed.color_axes      = false;
    changed.stripe          = false;
    changed.shade_unprinted = false;
    return changed;
}

GCodePanelSettings some_settings_changed()
{
    GCodePanelSettings changed;
    changed.lines      = 5;
    changed.comments   = GCodeCommentMode::Tags;
    changed.color_axes = false;
    return changed;
}

} // namespace

TEST_CASE("The G-code panel's settings read back as saved, each of them", "[GCodePanelText]")
{
    const GCodePanelSettings saved = GENERATE(every_setting_changed(), some_settings_changed(), GCodePanelSettings());
    Slic3r::AppConfig        config;
    save_gcode_panel_settings(config, saved);
    check_same_settings(load_gcode_panel_settings(config), saved);
}

TEST_CASE("A G-code panel setting not understood reads as the default", "[GCodePanelText]")
{
    Slic3r::AppConfig odd;
    odd.set("gcode_panel_lines", "7");
    odd.set("gcode_panel_font_size", "13");
    odd.set("gcode_panel_comments", "markers");
    odd.set("gcode_panel_current_line", "top");
    odd.set("gcode_panel_long_lines", "fold");
    odd.set("gcode_panel_align", "no");
    odd.set("gcode_panel_axis_colors", "off");
    odd.set("gcode_panel_stripe", "none");
    odd.set("gcode_panel_shade_unprinted", "nope");
    check_same_settings(load_gcode_panel_settings(odd), GCodePanelSettings());
}

TEST_CASE("A line's range stays inside a file that shrank since it was indexed, and a line outside it is empty", "[GCodePanelText]")
{
    const std::vector<size_t> ends = {10, 20, 30};
    CHECK(gcode_line_range(ends, 30, 1) == std::pair<size_t, size_t>(0, 10));
    CHECK(gcode_line_range(ends, 30, 3) == std::pair<size_t, size_t>(20, 30));
    CHECK(gcode_line_range(ends, 25, 3) == std::pair<size_t, size_t>(20, 25));
    CHECK(gcode_line_range(ends, 15, 3) == std::pair<size_t, size_t>(15, 15));
    CHECK(gcode_line_range(ends, 30, 0) == std::pair<size_t, size_t>(0, 0));
    CHECK(gcode_line_range(ends, 30, 4) == std::pair<size_t, size_t>(0, 0));
}

TEST_CASE("Copying takes each line of the rows once, without its line break, or the line alone", "[GCodePanelText]")
{
    const auto read = [](uint64_t id) { return "G1 X" + std::to_string(id) + "\r\n"; };
    std::vector<GCodeRow> rows(3);
    rows[0].line = 4;
    rows[1].line = 5;
    rows[1].first = true;
    rows[2].line  = 5;
    rows[2].first = false;
    CHECK(gcode_copy_text(rows, 9, read) == "G1 X4\nG1 X5");
    CHECK(gcode_copy_text({}, 9, read) == "G1 X9");
}

TEST_CASE("Axis letters stand out from the background of every row they are drawn on", "[GCodePanelText]")
{
    for (const bool dark : {false, true}) {
        const CanvasUi::Theme&   theme   = CanvasUi::theme(dark);
        const GCodePalette&      palette = gcode_palette(dark);
        const CanvasUi::Color    current = CanvasUi::blend_over(theme.selection, theme.card);
        for (const char letter : {'X', 'Y', 'Z', 'E', 'F'}) {
            INFO((dark ? "dark " : "light ") << letter);
            const CanvasUi::Color color = axis_color(palette, theme, letter);
            CHECK(CanvasUi::contrast_ratio(color, theme.card) >= 3.f);
            CHECK(CanvasUi::contrast_ratio(color, palette.unprinted_bg) >= 3.f);
            CHECK(CanvasUi::contrast_ratio(CanvasUi::mix(color, theme.foreground, .3f), current) >= 3.f);
        }
        INFO((dark ? "dark" : "light") << " current-line bar");
        CHECK(CanvasUi::contrast_ratio(theme.current_line, current) >= 3.f);
    }
}

TEST_CASE("The position indicator runs from the top of its track at the start of the file to its bottom at the end", "[GCodePanelText]")
{
    const float track = 100.f, height = 200.f, thumb = 14.f;
    const std::optional<GCodePositionThumb> start = gcode_position_thumb(1, 25, 40, track, height, thumb);
    const std::optional<GCodePositionThumb> end   = gcode_position_thumb(16, 40, 40, track, height, thumb);
    REQUIRE(start.has_value());
    REQUIRE(end.has_value());
    CHECK_THAT(start->top, Catch::Matchers::WithinAbs(track, 1e-4));
    CHECK_THAT(end->bottom, Catch::Matchers::WithinAbs(track + height, 1e-4));
    CHECK_THAT(end->top, Catch::Matchers::WithinAbs(track + height - thumb, 1e-4));
}

TEST_CASE("No position indicator when the whole file is shown", "[GCodePanelText]")
{
    CHECK_FALSE(gcode_position_thumb(1, 12, 12, 0.f, 200.f, 14.f).has_value());
}

TEST_CASE("The position indicator stays inside a track shorter than its thumb", "[GCodePanelText]")
{
    const std::optional<GCodePositionThumb> thumb = gcode_position_thumb(30, 30, 40, 50.f, 8.f, 14.f);
    REQUIRE(thumb.has_value());
    CHECK(thumb->top >= 50.f);
    CHECK(thumb->bottom <= 58.f);
}
