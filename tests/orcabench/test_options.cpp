#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "core/Options.hpp"
#include "core/Policy.hpp"
#include "core/Reporters.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace Slic3r::Bench;

TEST_CASE("a command line without a program name has no arguments", "[OrcaBench][Options]")
{
    const char* const only_program[] = {"orca_bench"};
    CHECK(program_arguments(0, nullptr).empty());
    CHECK(program_arguments(1, only_program).empty());
}

TEST_CASE("the arguments are the ones after the program name", "[OrcaBench][Options]")
{
    const char* const command_line[] = {"orca_bench", "--list", "--help"};
    CHECK(program_arguments(3, command_line) == std::vector<std::string_view> {"--list", "--help"});
}

TEST_CASE("no arguments ask for nothing", "[OrcaBench][Options]")
{
    const Options options = parse_options({});
    CHECK_FALSE(options.help);
    CHECK_FALSE(options.list);
    CHECK_FALSE(options.policy);
    CHECK_FALSE(options.selection.filter);
    CHECK_FALSE(options.selection.exclude);
    CHECK_FALSE(options.selection.kind);
    CHECK_FALSE(options.selection.tag);
    CHECK_FALSE(options.overrides.iterations);
    CHECK_FALSE(options.overrides.warmup);
    CHECK_FALSE(options.overrides.threads);
    CHECK_FALSE(options.overrides.stages);
    CHECK_FALSE(options.out);
    CHECK_FALSE(options.compare);
    CHECK_FALSE(options.allow_mismatch);
}

TEST_CASE("the list flag asks for the catalog", "[OrcaBench][Options]")
{
    const Options options = parse_options({"--list"});
    CHECK(options.list);
    CHECK_FALSE(options.help);
}

TEST_CASE("the help flag asks for the usage", "[OrcaBench][Options]")
{
    const Options options = parse_options({"--help"});
    CHECK(options.help);
    CHECK_FALSE(options.list);
}

TEST_CASE("the policy, filter and out flags ask for a run of the workloads the pattern selects, and where its result goes",
          "[OrcaBench][Options]")
{
    const Options options = parse_options({"--policy", "quick", "--filter", "slice/3dbenchy/*", "--out", "run.json"});
    CHECK(options.policy == std::optional<std::string>("quick"));
    CHECK(options.selection.filter == std::optional<std::string>("slice/3dbenchy/*"));
    CHECK(options.out == std::optional<std::string>("run.json"));
    CHECK(parse_options({"--list", "--filter", "slice/*"}).selection.filter == std::optional<std::string>("slice/*"));
}

TEST_CASE("a selection needs a run or a listing, an override or an out file needs a run, and a run, a comparison and a "
          "listing each run alone",
          "[OrcaBench][Options]")
{
    const auto [arguments, refusal] = GENERATE(table<std::vector<std::string_view>, std::string>({
        {{"--filter", "slice/*"}, "--filter needs --policy or --list"},
        {{"--exclude", "slice/*"}, "--exclude needs --policy or --list"},
        {{"--kind", "load"}, "--kind needs --policy or --list"},
        {{"--tag", "handy"}, "--tag needs --policy or --list"},
        {{"--list", "--iterations", "2"}, "--iterations needs --policy"},
        {{"--list", "--warmup", "0"}, "--warmup needs --policy"},
        {{"--list", "--threads", "1"}, "--threads needs --policy"},
        {{"--list", "--stages", "process"}, "--stages needs --policy"},
        {{"--list", "--reporter", "json"}, "--reporter needs --policy"},
        {{"--list", "--quiet"}, "--quiet needs --policy"},
        {{"--list", "--verbose"}, "--verbose needs --policy or --compare"},
        {{"--list", "--collapse-below", "1"}, "--collapse-below needs --policy or --compare"},
        {{"--list", "--sort-by", "time"}, "--sort-by needs --policy or --compare"},
        {{"--list", "--dump-gcode", "out"}, "--dump-gcode needs --policy or --check-determinism"},
        {{"--check-determinism", "--policy", "quick"}, "--check-determinism and --policy cannot run together"},
        {{"--check-determinism", "--iterations", "3"}, "--check-determinism and --iterations cannot run together"},
        {{"--check-determinism", "--stages", "load,process"}, "--check-determinism and --stages cannot run together"},
        {{"--compare", "a.json", "b.json", "--check-determinism"}, "--check-determinism and --compare cannot run together"},
        {{"--list", "--policy", "quick"}, "--list and --policy cannot run together"},
        {{"--list", "--compare", "a.json", "b.json"}, "--list and --compare cannot run together"},
        {{"--list", "--check-determinism"}, "--list and --check-determinism cannot run together"},
        {{"--out", "run.json"}, "--out needs --policy"},
        {{"--list", "--out", "run.json"}, "--out needs --policy"},
        {{"--policy", "quick", "--compare", "a.json", "b.json"}, "--policy and --compare cannot run together"},
    }));
    CAPTURE(arguments);
    CHECK_THROWS_WITH(parse_options(arguments), refusal);
}

TEST_CASE("the compare flag takes the results from before and after a change", "[OrcaBench][Options]")
{
    const Options options = parse_options({"--compare", "before.json", "after.json", "--allow-mismatch"});
    REQUIRE(options.compare);
    CHECK(options.compare->a == "before.json");
    CHECK(options.compare->b == "after.json");
    CHECK(options.allow_mismatch);
}

TEST_CASE("the compare flag without two results after it is refused", "[OrcaBench][Options]")
{
    const std::vector<std::string_view> arguments = GENERATE(values<std::vector<std::string_view>>({
        {"--compare"},
        {"--compare", "a.json"},
        {"--compare", "a.json", "--allow-mismatch"},
        {"--compare", "--list", "b.json"},
    }));
    CAPTURE(arguments);
    CHECK_THROWS_WITH(parse_options(arguments), "--compare needs a.json b.json");
}

TEST_CASE("the allow-mismatch flag without the compare flag is refused", "[OrcaBench][Options]")
{
    CHECK_THROWS_WITH(parse_options({"--allow-mismatch"}), "--allow-mismatch needs --compare");
    CHECK_THROWS_WITH(parse_options({"--list", "--allow-mismatch"}), "--allow-mismatch needs --compare");
}

TEST_CASE("an unknown argument is refused", "[OrcaBench][Options]")
{
    const std::string argument = GENERATE(as<std::string> {}, "--bogus", "list", "-l", "--list=1", "");
    CAPTURE(argument);
    CHECK_THROWS_AS(parse_options({argument}), OptionsError);
}

TEST_CASE("the usage lists each option with its values, then the exit statuses", "[OrcaBench][Options]")
{
    CHECK(usage() == "usage: orca_bench [options]\n"
                     "  --list                        print the catalog's workloads\n"
                     "  --policy name                 run the catalog under a policy, such as quick or precise\n"
                     "  --filter pattern              take only the workloads whose names match, where * stands for any text\n"
                     "  --exclude pattern             leave out the workloads whose names match\n"
                     "  --kind name                   take only the workloads of a kind, such as slice or load\n"
                     "  --tag name                    take only the workloads with a tag, such as handy or procedural\n"
                     "  --iterations n                time n passes of each workload instead of the policy's count\n"
                     "  --warmup n                    run n untimed passes first instead of the policy's count\n"
                     "  --threads n                   cap TBB at n threads, where 0 means every hardware thread\n"
                     "  --stages list                 time only these of load,process,export, running what they need untimed first\n"
                     "  --out file.json               write the run's result to the file\n"
                     "  --compare a.json b.json       compare result a, from before a change, with result b, from after it\n"
                     "  --allow-mismatch              compare results that measured differently\n"
                     "  --reporter console|json|null  print the run as tables, as the result document or not at all\n"
                     "  --verbose                     give each object its own rows and fold none\n"
                     "  --collapse-below percent      fold the stages under this percent of the time into one row, 1 unless set\n"
                     "  --sort-by time|start          order the stages by mean time, or by first start for pipeline order\n"
                     "  --quiet                       print no progress line\n"
                     "  --check-determinism           run each workload twice under verify and fail one whose output changes\n"
                     "  --dump-gcode dir              keep each pass's G-code there, as <workload>/<pass>.gcode\n"
                     "  --color auto|always|never     color the output, auto for a terminal unless TERM=dumb or NO_COLOR is non-empty\n"
                     "  --help                        print this text\n"
                     "exit status: 0 done, 1 error or a failed workload, 2 bad command line, 3 --compare found changed output\n");
}

TEST_CASE("the color flag says when to color the output", "[OrcaBench][Options]")
{
    CHECK(parse_options({}).color == ColorChoice::Auto);
    CHECK(parse_options({"--color", "always"}).color == ColorChoice::Always);
    CHECK(parse_options({"--color", "never"}).color == ColorChoice::Never);
    CHECK(parse_options({"--color", "auto"}).color == ColorChoice::Auto);
    CHECK_THROWS_WITH(parse_options({"--color", "sometimes"}), "--color needs auto|always|never, not 'sometimes'");
    CHECK_THROWS_WITH(parse_options({"--color"}), "--color needs auto|always|never");
}

TEST_CASE("auto colors a terminal unless NO_COLOR holds something or TERM is dumb, and always and never decide alone",
          "[OrcaBench][Options]")
{
    const auto [choice, terminal, no_color, term, colored] = GENERATE(table<ColorChoice, bool, const char*, const char*, bool>({
        {ColorChoice::Auto, true, nullptr, nullptr, true},
        {ColorChoice::Auto, true, "", "xterm-256color", true},
        {ColorChoice::Auto, true, "1", nullptr, false},
        {ColorChoice::Auto, true, nullptr, "dumb", false},
        {ColorChoice::Auto, false, nullptr, nullptr, false},
        {ColorChoice::Always, false, "1", "dumb", true},
        {ColorChoice::Never, true, nullptr, nullptr, false},
    }));
    CHECK(use_color(choice, terminal, no_color, term) == colored);
}

TEST_CASE("the exclude, kind and tag flags narrow what a run or a listing takes", "[OrcaBench][Options]")
{
    const Options options = parse_options({"--list", "--exclude", "*/stl", "--kind", "load", "--tag", "handy"});
    CHECK(options.selection.exclude == std::optional<std::string>("*/stl"));
    CHECK(options.selection.kind == std::optional<std::string>("load"));
    CHECK(options.selection.tag == std::optional<std::string>("handy"));
}

TEST_CASE("the iterations, warmup, threads and stages flags override the policy's", "[OrcaBench][Options]")
{
    const Options options =
        parse_options({"--policy", "quick", "--iterations", "5", "--warmup", "0", "--threads", "4", "--stages", "process, export"});
    CHECK(options.overrides.iterations == std::optional<unsigned>(5));
    CHECK(options.overrides.warmup == std::optional<unsigned>(0));
    CHECK(options.overrides.threads == std::optional<unsigned>(4));
    CHECK(options.overrides.stages == std::optional<StageSet>(StageSet {Stage::Process, Stage::Export}));
}

TEST_CASE("a count that is not a whole number is refused", "[OrcaBench][Options]")
{
    const std::string value = GENERATE(as<std::string> {}, "x", "-1", "+2", "3x", "2.5", "99999999999");
    CAPTURE(value);
    CHECK_THROWS_WITH(parse_options({"--policy", "quick", "--iterations", value}),
                      "--iterations needs a whole number, not '" + value + "'");
}

TEST_CASE("a stage list naming a stage the pipeline does not have is refused", "[OrcaBench][Options]")
{
    CHECK_THROWS_AS(parse_options({"--policy", "quick", "--stages", "process,slice"}), OptionsError);
    CHECK_THROWS_WITH(parse_options({"--policy", "quick", "--stages", "process,slice"}), Catch::Matchers::StartsWith("--stages: "));
}

TEST_CASE("the reporter, layout and quiet flags shape what a run prints", "[OrcaBench][Options]")
{
    const Options run = parse_options(
        {"--policy", "quick", "--reporter", "json", "--verbose", "--collapse-below", "2.5", "--sort-by", "start", "--quiet"});
    CHECK(run.reporter == std::optional<std::string>("json"));
    CHECK(run.verbose);
    CHECK(run.collapse_below == std::optional<double>(0.025));
    CHECK(run.sort_by == std::optional<SortBy>(SortBy::Start));
    CHECK(run.quiet);

    const Options comparison = parse_options({"--compare", "a.json", "b.json", "--verbose", "--collapse-below", "0", "--sort-by", "time"});
    CHECK(comparison.verbose);
    CHECK(comparison.collapse_below == std::optional<double>(0.));
    CHECK(comparison.sort_by == std::optional<SortBy>(SortBy::Time));
}

TEST_CASE("a reporter, percent or order the flags do not know is refused", "[OrcaBench][Options]")
{
    const auto [arguments, refusal] = GENERATE(table<std::vector<std::string_view>, std::string>({
        {{"--policy", "quick", "--reporter", "xml"}, "--reporter needs console|json|null, not 'xml'"},
        {{"--policy", "quick", "--collapse-below", "x"}, "--collapse-below needs a percent from 0 to 100, not 'x'"},
        {{"--policy", "quick", "--collapse-below", "1%"}, "--collapse-below needs a percent from 0 to 100, not '1%'"},
        {{"--policy", "quick", "--collapse-below", "-1"}, "--collapse-below needs a percent from 0 to 100, not '-1'"},
        {{"--policy", "quick", "--collapse-below", "101"}, "--collapse-below needs a percent from 0 to 100, not '101'"},
        {{"--policy", "quick", "--sort-by", "name"}, "--sort-by needs time|start, not 'name'"},
    }));
    CAPTURE(arguments);
    CHECK_THROWS_WITH(parse_options(arguments), refusal);
}

TEST_CASE("the check-determinism flag runs the verify preset at two passes, and the dump flag keeps their G-code", "[OrcaBench][Options]")
{
    const Options options = parse_options({"--check-determinism", "--filter", "slice/*", "--dump-gcode", "passes"});
    CHECK(options.policy == std::optional<std::string>("verify"));
    CHECK(options.overrides.iterations == std::optional<unsigned>(2));
    CHECK(options.selection.filter == std::optional<std::string>("slice/*"));
    CHECK(options.dump_gcode == std::optional<std::string>("passes"));
    CHECK(parse_options({"--policy", "quick", "--dump-gcode", "passes"}).dump_gcode == std::optional<std::string>("passes"));
}
