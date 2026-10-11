#pragma once

#include "core/Catalog.hpp"
#include "core/Policy.hpp"
#include "core/Reporters.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Slic3r { namespace Bench {

// Thrown for a command line orca_bench cannot use.
class OptionsError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// When the console views color their output.
enum class ColorChoice { Auto, Always, Never };

// The result documents --compare names, the one from before a change first.
struct CompareFiles
{
    std::string a;
    std::string b;
};

// What the command line asks for.
struct Options
{
    bool                        help = false;
    bool                        list = false;
    std::optional<std::string>  policy;
    PolicyOverrides             overrides;
    Selection                   selection;
    std::optional<std::string>  out;
    std::optional<CompareFiles> compare;
    bool                        allow_mismatch = false;
    // The reporter a run prints through, console unless set.
    std::optional<std::string>  reporter;
    // The layout of a run's console and of a comparison, where unset keeps the reporter's.
    bool                        verbose = false;
    std::optional<double>       collapse_below;
    std::optional<SortBy>       sort_by;
    bool                        quiet             = false;
    ColorChoice                 color             = ColorChoice::Auto;
    bool                        check_determinism = false;
    // Where a run keeps each pass's G-code.
    std::optional<std::string>  dump_gcode;
};

// The arguments after the program name, and none when argc is 0, which an exec with an empty argv
// allows.
std::vector<std::string_view> program_arguments(int argc, const char* const* argv);

// Reads the arguments that follow the program name.
Options parse_options(const std::vector<std::string_view>& arguments);

// What --help prints, one line per option, then the exit statuses.
std::string usage();

// Whether the console views color their output, where auto colors a terminal unless NO_COLOR holds
// anything, as no-color.org asks, or TERM is dumb.
bool use_color(ColorChoice choice, bool terminal, const char* no_color, const char* term);

}} // namespace Slic3r::Bench
