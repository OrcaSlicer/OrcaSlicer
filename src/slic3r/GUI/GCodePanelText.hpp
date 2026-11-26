#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "CanvasUi/Font.hpp"
#include "CanvasUi/Types.hpp"

namespace Slic3r {
class AppConfig;

namespace GUI {
namespace CanvasUi {
struct Theme;
}

enum class GCodeCommandKind : unsigned char { None, Move, Machine, Macro };

struct GCodeWord
{
    char        letter{0};
    std::string value;
};

struct GCodeLineParts
{
    std::string            command;
    GCodeCommandKind       kind{GCodeCommandKind::None};
    std::vector<GCodeWord> words;
    // The arguments of a macro, as written: KEY=value pairs or anything that is not a letter word.
    std::string            arguments;
    std::string            comment;
    bool                   has_comment{false};
    bool                   tag{false};

    bool comment_only() const { return command.empty() && has_comment; }
};

GCodeLineParts parse_gcode_line(std::string_view line);
bool           is_gcode_tag_comment(std::string_view comment);

enum class GCodeCommentMode : unsigned char { All, Tags, None };

bool gcode_line_shown(const GCodeLineParts &parts, GCodeCommentMode mode);

struct GCodePanelSettings
{
    size_t           lines{0};
    bool             upper_third{false};
    bool             wrap{false};
    GCodeCommentMode comments{GCodeCommentMode::All};
    CanvasUi::MonoSize font_size{CanvasUi::MonoSize::Code};
    bool             align{true};
    bool             color_axes{true};
    bool             stripe{true};
    bool             shade_unprinted{true};
};

// The line counts the Lines setting offers; 0 is Auto.
inline constexpr std::array<size_t, 5> GCODE_PANEL_LINES = {0, 5, 15, 25, 35};

// The settings, as the app config keeps them under gcode_panel_* keys. A value missing or not understood reads as the default.
GCodePanelSettings load_gcode_panel_settings(const AppConfig &config);
void               save_gcode_panel_settings(AppConfig &config, const GCodePanelSettings &settings);

// How many settings differ from their defaults.
size_t changed_settings(const GCodePanelSettings &settings);

// The height the open settings take under the header, in whole pixels: all they need, or the room left above
// min_rows rows of code, the rest scrolled.
float settings_view_height(float content_height, float room, float row_height, size_t min_rows);

struct GCodeBodySize
{
    size_t rows{0};
    float  height{0.f};
};

// The rows of code in the room under the header and the settings, in whole pixels. With lines 0 they reach the bottom
// of the room and the last one is cut there, so the bottom stays put; otherwise lines, as many of them as fit whole.
GCodeBodySize gcode_body_size(float room, float row_height, size_t lines);

inline constexpr std::string_view GCODE_ALIGNED_LETTERS = "XYZEF";


enum class GCodeSpanStyle : unsigned char { Command, Macro, Letter, Value, Key, Argument, Comment, Tag };

struct GCodeSpan
{
    size_t         column{0};
    std::string    text;
    GCodeSpanStyle style{GCodeSpanStyle::Value};
    char           letter{0};
    // Columns taken past the text, as by the badge drawn around a tag comment.
    size_t         padding{0};

    size_t end() const;
};

size_t gcode_text_length(std::string_view text);

// Aligned, the words of a move go into fixed columns: X and Y always keep theirs, so that moves line
// up whatever lines are around them; Z, E and F take room only on a line that has them.
std::vector<GCodeSpan> layout_gcode_line(const GCodeLineParts &parts, bool align, GCodeCommentMode mode, size_t tag_padding = 0);

std::vector<std::vector<GCodeSpan>> wrap_gcode_spans(const std::vector<GCodeSpan> &spans, size_t width);

// A row of the panel: a line, or a piece of it when long lines wrap.
struct GCodeRow
{
    uint64_t               line{0};
    // The first row of its line, which shows the line's number.
    bool                   first{true};
    bool                   overflow{false};
    std::vector<GCodeSpan> spans;
};

// Where line id (from 1) starts and ends in a file of file_size bytes whose lines end at lines_ends, kept inside
// the file, as the file may have shrunk since it was indexed; an empty range for a line outside it.
std::pair<size_t, size_t> gcode_line_range(const std::vector<size_t> &lines_ends, size_t file_size, uint64_t id);

// The text to copy: the lines of rows once each, or the line alone without rows, without line breaks at their ends.
std::string gcode_copy_text(const std::vector<GCodeRow> &rows, uint64_t line, const std::function<std::string(uint64_t)> &read_line);

// The colours of G-code syntax over the theme's.
struct GCodePalette
{
    CanvasUi::Color x, y, z, e, f;
    CanvasUi::Color macro;
    CanvasUi::Color macro_value;
    CanvasUi::Color tag_text;
    CanvasUi::Color unprinted_bg;
};
const GCodePalette &gcode_palette(bool dark);
CanvasUi::Color     axis_color(const GCodePalette &palette, const CanvasUi::Theme &theme, char letter);

struct GCodePositionThumb
{
    float top{0.f};
    float bottom{0.f};
};

std::optional<GCodePositionThumb> gcode_position_thumb(uint64_t first, uint64_t last, uint64_t total, float track_top, float track_height,
                                                       float thumb_height);

// The count rows around the current line of a file of total lines, chars wide: the current line in the middle
// or a third from the top, without the comments the settings hide, and kept inside the file at its ends.
std::vector<GCodeRow> gcode_rows_around(uint64_t current, uint64_t total, size_t count, const GCodePanelSettings &settings, size_t chars,
                                        size_t tag_padding, const std::function<std::string(uint64_t)> &read_line);

} // namespace GUI
} // namespace Slic3r
