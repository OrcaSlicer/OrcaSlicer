#include "core/Options.hpp"

#include "core/Policy.hpp"
#include "core/Reporters.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <locale>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Slic3r { namespace Bench {

namespace {

ColorChoice color_choice(std::string_view value)
{
    if (value == "auto")
        return ColorChoice::Auto;
    if (value == "always")
        return ColorChoice::Always;
    if (value == "never")
        return ColorChoice::Never;
    throw OptionsError("--color needs auto|always|never, not '" + std::string(value) + "'");
}

unsigned whole_number(const char* flag, std::string_view value)
{
    unsigned          number = 0;
    const char* const end    = value.data() + value.size();
    const auto [last, error] = std::from_chars(value.data(), end, number);
    if (value.empty() || error != std::errc() || last != end)
        throw OptionsError(std::string(flag) + " needs a whole number, not '" + std::string(value) + "'");
    return number;
}

std::string reporter(std::string_view value)
{
    const std::vector<std::string> names = reporter_names();
    if (std::find(names.begin(), names.end(), value) == names.end())
        throw OptionsError("--reporter needs console|json|null, not '" + std::string(value) + "'");
    return std::string(value);
}

// A percent as a share, read the same in every locale.
double percent(std::string_view value)
{
    std::istringstream in {std::string(value)};
    in.imbue(std::locale::classic());
    double number = -1.;
    in >> number;
    if (value.empty() || !in || in.peek() != std::char_traits<char>::eof() || number < 0. || number > 100.)
        throw OptionsError("--collapse-below needs a percent from 0 to 100, not '" + std::string(value) + "'");
    return number / 100.;
}

SortBy sort_by(std::string_view value)
{
    if (value == "time")
        return SortBy::Time;
    if (value == "start")
        return SortBy::Start;
    throw OptionsError("--sort-by needs time|start, not '" + std::string(value) + "'");
}

StageSet stages(std::string_view value)
{
    try {
        return parse_stages(value);
    } catch (const PolicyError& error) {
        throw OptionsError(std::string("--stages: ") + error.what());
    }
}

struct Flag
{
    const char* name;
    // What the usage calls each value that follows the flag, one word per value.
    const char* values;
    void (*set)(Options& options, const std::string_view* values);
    const char* help;
};

// Every flag, which both the parser and the usage text read, so neither can miss one.
constexpr Flag flags[] = {
    {"--list", "", [](Options& options, const std::string_view*) { options.list = true; }, "print the catalog's workloads"},
    {"--policy", "name", [](Options& options, const std::string_view* values) { options.policy = std::string(values[0]); },
     "run the catalog under a policy, such as quick or precise"},
    {"--filter", "pattern", [](Options& options, const std::string_view* values) { options.selection.filter = std::string(values[0]); },
     "take only the workloads whose names match, where * stands for any text"},
    {"--exclude", "pattern", [](Options& options, const std::string_view* values) { options.selection.exclude = std::string(values[0]); },
     "leave out the workloads whose names match"},
    {"--kind", "name", [](Options& options, const std::string_view* values) { options.selection.kind = std::string(values[0]); },
     "take only the workloads of a kind, such as slice or load"},
    {"--tag", "name", [](Options& options, const std::string_view* values) { options.selection.tag = std::string(values[0]); },
     "take only the workloads with a tag, such as handy or procedural"},
    {"--iterations", "n",
     [](Options& options, const std::string_view* values) { options.overrides.iterations = whole_number("--iterations", values[0]); },
     "time n passes of each workload instead of the policy's count"},
    {"--warmup", "n",
     [](Options& options, const std::string_view* values) { options.overrides.warmup = whole_number("--warmup", values[0]); },
     "run n untimed passes first instead of the policy's count"},
    {"--threads", "n",
     [](Options& options, const std::string_view* values) { options.overrides.threads = whole_number("--threads", values[0]); },
     "cap TBB at n threads, where 0 means every hardware thread"},
    {"--stages", "list", [](Options& options, const std::string_view* values) { options.overrides.stages = stages(values[0]); },
     "time only these of load,process,export, running what they need untimed first"},
    {"--out", "file.json", [](Options& options, const std::string_view* values) { options.out = std::string(values[0]); },
     "write the run's result to the file"},
    {"--compare", "a.json b.json",
     [](Options& options, const std::string_view* values) {
         options.compare = CompareFiles {std::string(values[0]), std::string(values[1])};
     },
     "compare result a, from before a change, with result b, from after it"},
    {"--allow-mismatch", "", [](Options& options, const std::string_view*) { options.allow_mismatch = true; },
     "compare results that measured differently"},
    {"--reporter", "console|json|null", [](Options& options, const std::string_view* values) { options.reporter = reporter(values[0]); },
     "print the run as tables, as the result document or not at all"},
    {"--verbose", "", [](Options& options, const std::string_view*) { options.verbose = true; },
     "give each object its own rows and fold none"},
    {"--collapse-below", "percent",
     [](Options& options, const std::string_view* values) { options.collapse_below = percent(values[0]); },
     "fold the stages under this percent of the time into one row, 1 unless set"},
    {"--sort-by", "time|start", [](Options& options, const std::string_view* values) { options.sort_by = sort_by(values[0]); },
     "order the stages by mean time, or by first start for pipeline order"},
    {"--quiet", "", [](Options& options, const std::string_view*) { options.quiet = true; }, "print no progress line"},
    {"--check-determinism", "", [](Options& options, const std::string_view*) { options.check_determinism = true; },
     "run each workload twice under verify and fail one whose output changes"},
    {"--dump-gcode", "dir", [](Options& options, const std::string_view* values) { options.dump_gcode = std::string(values[0]); },
     "keep each pass's G-code there, as <workload>/<pass>.gcode"},
    {"--color", "auto|always|never", [](Options& options, const std::string_view* values) { options.color = color_choice(values[0]); },
     "color the output, auto for a terminal unless TERM=dumb or NO_COLOR is non-empty"},
    {"--help", "", [](Options& options, const std::string_view*) { options.help = true; }, "print this text"},
};

std::size_t value_count(const Flag& flag)
{
    const std::string_view values = flag.values;
    return values.empty() ? 0 : 1 + static_cast<std::size_t>(std::count(values.begin(), values.end(), ' '));
}

std::string synopsis(const Flag& flag) { return value_count(flag) == 0 ? flag.name : std::string(flag.name) + " " + flag.values; }

// Throws for the first flag given without what it needs.
void require(std::initializer_list<std::pair<const char*, bool>> given, bool met, const char* needs)
{
    for (const auto& [flag, set] : given)
        if (set && !met)
            throw OptionsError(std::string(flag) + " needs " + needs);
}

} // namespace

std::vector<std::string_view> program_arguments(int argc, const char* const* argv)
{
    std::vector<std::string_view> arguments;
    for (int i = 1; i < argc; ++i)
        arguments.emplace_back(argv[i]);
    return arguments;
}

Options parse_options(const std::vector<std::string_view>& arguments)
{
    Options               options;
    std::set<std::string> given;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const std::string_view argument = arguments[i];
        const auto found = std::find_if(std::begin(flags), std::end(flags), [argument](const Flag& flag) { return argument == flag.name; });
        if (found == std::end(flags))
            throw OptionsError("unknown argument '" + std::string(argument) + "'");
        // Only one of the values would count.
        if (!given.insert(found->name).second)
            throw OptionsError(std::string(found->name) + " is given twice");
        const std::size_t count = value_count(*found);
        // An argument starting with -- is a flag, never a value.
        for (std::size_t k = 1; k <= count; ++k)
            if (i + k >= arguments.size() || arguments[i + k].substr(0, 2) == "--")
                throw OptionsError(std::string(found->name) + " needs " + found->values);
        found->set(options, arguments.data() + i + 1);
        i += count;
    }
    if (options.list)
        for (const auto& [flag, set] : {std::pair {"--policy", bool(options.policy)}, std::pair {"--compare", bool(options.compare)},
                                        std::pair {"--check-determinism", options.check_determinism}})
            if (set)
                throw OptionsError(std::string("--list and ") + flag + " cannot run together");
    if (options.check_determinism) {
        for (const auto& [flag, set] : {std::pair {"--policy", bool(options.policy)},
                                        std::pair {"--iterations", bool(options.overrides.iterations)},
                                        std::pair {"--stages", bool(options.overrides.stages)},
                                        std::pair {"--compare", bool(options.compare)}})
            if (set)
                throw OptionsError(std::string("--check-determinism and ") + flag + " cannot run together");
        options.policy               = "verify";
        options.overrides.iterations = 2;
    }
    require({{"--dump-gcode", bool(options.dump_gcode)}}, bool(options.policy), "--policy or --check-determinism");
    require({{"--allow-mismatch", options.allow_mismatch}}, bool(options.compare), "--compare");
    const Selection& selection = options.selection;
    require({{"--filter", bool(selection.filter)}, {"--exclude", bool(selection.exclude)}, {"--kind", bool(selection.kind)},
             {"--tag", bool(selection.tag)}},
            options.policy || options.list, "--policy or --list");
    const PolicyOverrides& overrides = options.overrides;
    require({{"--iterations", bool(overrides.iterations)}, {"--warmup", bool(overrides.warmup)}, {"--threads", bool(overrides.threads)},
             {"--stages", bool(overrides.stages)}, {"--out", bool(options.out)}, {"--reporter", bool(options.reporter)},
             {"--quiet", options.quiet}},
            bool(options.policy), "--policy");
    require({{"--verbose", options.verbose}, {"--collapse-below", bool(options.collapse_below)}, {"--sort-by", bool(options.sort_by)}},
            options.policy || options.compare, "--policy or --compare");
    if (options.policy && options.compare)
        throw OptionsError("--policy and --compare cannot run together");
    return options;
}

std::string usage()
{
    std::size_t widest = 0;
    for (const Flag& flag : flags)
        widest = std::max(widest, synopsis(flag).size());
    std::string text = "usage: orca_bench [options]\n";
    for (const Flag& flag : flags)
        text += "  " + synopsis(flag) + std::string(widest + 2 - synopsis(flag).size(), ' ') + flag.help + "\n";
    return text + "exit status: 0 done, 1 error or a failed workload, 2 bad command line, 3 --compare found changed output\n";
}

bool use_color(ColorChoice choice, bool terminal, const char* no_color, const char* term)
{
    if (choice == ColorChoice::Auto)
        return terminal && (no_color == nullptr || *no_color == '\0') && (term == nullptr || std::string_view(term) != "dumb");
    return choice == ColorChoice::Always;
}

}} // namespace Slic3r::Bench
