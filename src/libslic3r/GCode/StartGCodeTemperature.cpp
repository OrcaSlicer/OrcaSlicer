#include "StartGCodeTemperature.hpp"

#include "../I18N.hpp"
#include "../format.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>

#define L(s) (s)
#define _(s) Slic3r::I18N::translate(s)

namespace Slic3r {

namespace {

struct TemperatureCommand
{
    bool numeric;  // the temperature argument parsed as a number
    int  temp;     // parsed temperature, valid when numeric
    bool literal;  // the temperature argument is a plain number, not a placeholder or expression
};

inline bool is_blank(char c) { return c == ' ' || c == '\t'; }
inline bool is_eol(char c) { return c == 0 || c == '\r' || c == '\n'; }
inline bool is_word_end(char c) { return is_blank(c) || c == ';' || is_eol(c); }

// "{endif}", "{else}" or "{elsif ...}" closing a same-line conditional, e.g.
// "{if curr_bed_type == \"Cool Plate\"}M190 S35{endif}".
bool is_conditional_close(const char *p)
{
    return std::strncmp(p, "{endif", 6) == 0 || std::strncmp(p, "{else", 5) == 0;
}

// Skips one balanced {...} or [...] block starting at `p`, without crossing a line end.
const char *skip_block(const char *p)
{
    const char open = *p, close = open == '{' ? '}' : ']';
    int depth = 0;
    for (; !is_eol(*p); ++p) {
        if (*p == open)
            ++depth;
        else if (*p == close && --depth == 0)
            return p + 1;
    }
    return p;
}

// Parses the temperature argument starting at `p` (just after the S or R letter).
TemperatureCommand parse_temperature_word(const char *p)
{
    TemperatureCommand cmd { false, 0, false };
    for (; is_blank(*p); ++p);
    char *end = nullptr;
    long  v   = std::strtol(p, &end, 10);
    if (end == p)
        // Placeholder or expression in the template.
        return cmd;
    cmd.numeric = true;
    cmd.temp    = int(v);
    const char *q = end;
    if (*q == '.')
        for (++q; std::isdigit((unsigned char) *q); ++q);
    cmd.literal = is_word_end(*q) || is_conditional_close(q);
    return cmd;
}

// Calls `fn(const TemperatureCommand&)` for every command of `family` in `gcode` that has a
// temperature argument, in order of appearance. Lines may end in LF, CRLF or CR.
template<typename Fn> void for_each_temperature_command(const std::string &gcode, const TemperatureCommandFamily &family, Fn &&fn)
{
    const char *p = gcode.c_str();
    while (*p != 0) {
        // Skip leading blanks and same-line template blocks such as "{if ...}".
        for (;;) {
            for (; is_blank(*p); ++p);
            if (*p == '{')
                p = skip_block(p);
            else
                break;
        }
        const char letter = char(std::toupper((unsigned char) *p));
        if (letter == 'M' || (letter == 'G' && family.include_g10)) {
            char *end  = nullptr;
            long  code = std::strtol(p + 1, &end, 10);
            bool  match = end != p + 1 && (is_word_end(*end) || std::isalpha((unsigned char) *end)) &&
                         (letter == 'G' ? code == 10 : (code == family.mcode_no_wait || code == family.mcode_wait));
            if (match) {
                const bool waits = letter == 'M' && code == family.mcode_wait;
                std::optional<TemperatureCommand> s, r;
                const char *w = end;
                while (!is_eol(*w) && *w != ';') {
                    for (; is_blank(*w); ++w);
                    if (is_eol(*w) || *w == ';')
                        break;
                    if (*w == '{' || *w == '[') {
                        w = skip_block(w);
                        continue;
                    }
                    const char word = char(std::toupper((unsigned char) *w));
                    if (word == 'S')
                        s = parse_temperature_word(w + 1);
                    else if (word == 'R' && waits)
                        // M109 R / M190 R: wait for the temperature whether heating or cooling.
                        r = parse_temperature_word(w + 1);
                    // Skip the rest of the word, including any placeholder inside it.
                    for (++w; !is_word_end(*w);)
                        w = (*w == '{' || *w == '[') ? skip_block(w) : w + 1;
                }
                if (s)
                    fn(*s);
                else if (r)
                    fn(*r);
            }
        }
        // Advance to the next line.
        for (; !is_eol(*p); ++p);
        if (*p == '\r')
            ++p;
        if (*p == '\n')
            ++p;
    }
}

} // namespace

std::optional<int> last_gcode_temperature(const std::string &gcode, const TemperatureCommandFamily &family)
{
    std::optional<int> out;
    for_each_temperature_command(gcode, family, [&out](const TemperatureCommand &cmd) {
        if (cmd.numeric)
            out = cmd.temp;
    });
    return out;
}

bool gcode_template_last_temperature_is_literal(const std::string &raw_template, const TemperatureCommandFamily &family, int value)
{
    std::optional<TemperatureCommand> last;
    for_each_temperature_command(raw_template, family, [&last](const TemperatureCommand &cmd) { last = cmd; });
    return last && last->literal && last->temp == value;
}

bool gcode_template_sets_temperature(const std::string &raw_template, const TemperatureCommandFamily &family)
{
    bool found = false;
    for_each_temperature_command(raw_template, family, [&found](const TemperatureCommand &) { found = true; });
    return found;
}

std::optional<int> start_gcode_literal_temperature_mismatch(const std::string              &raw_template,
                                                            const std::string              &processed_gcode,
                                                            const TemperatureCommandFamily &family,
                                                            int                             expected)
{
    // The temperature in effect when the slicer takes over is the one set last.
    const std::optional<int> in_effect = last_gcode_temperature(processed_gcode, family);
    // A literal 0 switches the heater off, typically so that a firmware macro such as
    // "START_PRINT EXTRUDER_TEMP=[...]" can heat with the slicer's values; not a mismatch.
    if (!in_effect || *in_effect <= 0 || expected <= 0)
        return std::nullopt;
    if (std::abs(*in_effect - expected) <= START_GCODE_TEMPERATURE_TOLERANCE)
        return std::nullopt;
    // Only a number typed into the template's last temperature command is reported. A value
    // computed from placeholders (for example an intentional offset from the filament
    // temperature) is left alone.
    if (!gcode_template_last_temperature_is_literal(raw_template, family, *in_effect))
        return std::nullopt;
    return in_effect;
}

std::string start_gcode_nozzle_temperature_warning(int gcode_temp, int expected, const std::string &filament_name, bool sequential)
{
    std::string msg = format(_(L("Your printer's start G-code heats the nozzle to %1% °C, but %2% prints at %3% °C. "
                                  "The start G-code's temperature is used until the slicer next changes it.")),
                             gcode_temp, filament_name, expected);
    if (sequential)
        msg += " " + std::string(_(L("Objects are printed one at a time, so the first object prints at the start G-code's temperature.")));
    msg += " " + std::string(_(L("Use [nozzle_temperature_initial_layer] in the start G-code instead.")));
    return msg;
}

std::string start_gcode_bed_temperature_warning(int gcode_temp, int expected)
{
    return format(_(L("Your printer's start G-code heats the bed to %1% °C, but the first layer bed temperature for this plate type is %2% °C. "
                      "The start G-code's temperature is used until the slicer next changes it. "
                      "Use [bed_temperature_initial_layer_single] in the start G-code instead.")),
                  gcode_temp, expected);
}

} // namespace Slic3r
