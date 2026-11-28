#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "core/Policy.hpp"
#include "core/Result.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

using namespace Slic3r::Bench;

namespace {

constexpr unsigned hardware = 8;

Policy resolved(std::string_view preset, const PolicyOverrides& overrides = {})
{
    return Policy::resolve(preset, overrides, hardware);
}

// Every field, so a comparison covers the whole policy and a new field stops this compiling.
auto fields(const Policy& policy)
{
    const auto& [name, threads, warmup, iterations, stages, metrics, pgo_eligible_only] = policy;
    return std::tie(name, threads, warmup, iterations, stages, metrics, pgo_eligible_only);
}

} // namespace

TEST_CASE("each preset resolves to its own settings", "[OrcaBench][Policy]")
{
    const auto [name, threads, warmup, iterations, stages, metrics, pgo_only] =
        GENERATE(table<std::string, unsigned, unsigned, unsigned, std::string, std::string, bool>({
            {"quick", hardware, 1, 3, "load,process,export", "wall,rss,hash,work", false},
            {"precise", 1, 2, 10, "load,process,export", "wall,rss,hash,work", false},
            {"verify", hardware, 0, 1, "load,process,export", "hash", false},
            {"pgo", hardware, 0, 1, "load,process,export", "none", true},
        }));
    CAPTURE(name);
    const Policy policy = resolved(name);
    CHECK(policy.name == name);
    CHECK(policy.threads == threads);
    CHECK(policy.warmup == warmup);
    CHECK(policy.iterations == iterations);
    CHECK(to_string(policy.stages) == stages);
    CHECK(to_string(policy.metrics) == metrics);
    CHECK(policy.pgo_eligible_only == pgo_only);
}

TEST_CASE("an unknown preset is refused", "[OrcaBench][Policy]")
{
    CHECK_THROWS_AS(resolved("fast"), PolicyError);
}

TEST_CASE("the refusal of an unknown preset names each preset", "[OrcaBench][Policy]")
{
    const std::string preset = GENERATE(as<std::string> {}, "quick", "precise", "verify", "pgo");
    CHECK_THROWS_WITH(resolved("fast"), Catch::Matchers::ContainsSubstring(preset));
}

TEST_CASE("each override replaces only its own setting", "[OrcaBench][Policy]")
{
    struct Change
    {
        const char*                            field;
        std::function<void(PolicyOverrides&)> set_override;
        std::function<void(Policy&)>          set_expected;
    };
    const std::vector<Change> changes = {
        {"threads", [](PolicyOverrides& overrides) { overrides.threads = 4; }, [](Policy& policy) { policy.threads = 4; }},
        {"warmup", [](PolicyOverrides& overrides) { overrides.warmup = 0; }, [](Policy& policy) { policy.warmup = 0; }},
        {"iterations", [](PolicyOverrides& overrides) { overrides.iterations = 7; }, [](Policy& policy) { policy.iterations = 7; }},
        {"stages", [](PolicyOverrides& overrides) { overrides.stages = StageSet {Stage::Load, Stage::Process, Stage::Export}; },
         [](Policy& policy) { policy.stages = {Stage::Load, Stage::Process, Stage::Export}; }},
    };
    const Policy precise = resolved("precise");
    for (const Change& change : changes) {
        CAPTURE(change.field);
        PolicyOverrides overrides;
        change.set_override(overrides);
        Policy expected = precise;
        change.set_expected(expected);
        const Policy actual = resolved("precise", overrides);
        CHECK(fields(actual) == fields(expected));
    }
}

TEST_CASE("an override of 0 threads means every hardware thread", "[OrcaBench][Policy]")
{
    PolicyOverrides overrides;
    overrides.threads = 0;
    CHECK(resolved("precise", overrides).threads == hardware);
}

TEST_CASE("a thread count above the hardware's is refused", "[OrcaBench][Policy]")
{
    PolicyOverrides overrides;
    overrides.threads = hardware + 1;
    CHECK_THROWS_AS(resolved("quick", overrides), PolicyError);
}

TEST_CASE("a run without an iteration is refused", "[OrcaBench][Policy]")
{
    PolicyOverrides overrides;
    overrides.iterations = 0;
    CHECK_THROWS_AS(resolved("quick", overrides), PolicyError);
}

TEST_CASE("a run without a stage is refused", "[OrcaBench][Policy]")
{
    PolicyOverrides overrides;
    overrides.stages = StageSet {};
    CHECK_THROWS_AS(resolved("quick", overrides), PolicyError);
}

TEST_CASE("a run that leaves out export collects no work stats, and one that also leaves out load no hash", "[OrcaBench][Policy]")
{
    const auto [stages, metrics] = GENERATE(table<StageSet, std::string>({
        {{Stage::Process}, "wall,rss"},
        {{Stage::Load}, "wall,rss,hash"},
        {{Stage::Load, Stage::Process}, "wall,rss,hash"},
    }));
    PolicyOverrides overrides;
    overrides.stages = stages;
    CAPTURE(to_string(stages));
    CHECK(to_string(resolved("quick", overrides).metrics) == metrics);
}

TEST_CASE("a run that collects only the hash is refused without export or load", "[OrcaBench][Policy]")
{
    PolicyOverrides process;
    process.stages = StageSet {Stage::Process};
    CHECK_THROWS_WITH(resolved("verify", process), "verify collects only what export or load produces, so it needs one of those stages");
    PolicyOverrides load;
    load.stages = StageSet {Stage::Load};
    CHECK(to_string(resolved("verify", load).metrics) == "hash");
}

TEST_CASE("pgo takes a thread count", "[OrcaBench][Policy]")
{
    PolicyOverrides overrides;
    overrides.threads = 4;
    CHECK(resolved("pgo", overrides).threads == 4);
}

TEST_CASE("pgo refuses a warmup or iteration count", "[OrcaBench][Policy]")
{
    PolicyOverrides warmup;
    warmup.warmup = 1;
    CHECK_THROWS_AS(resolved("pgo", warmup), PolicyError);

    PolicyOverrides iterations;
    iterations.iterations = 3;
    CHECK_THROWS_AS(resolved("pgo", iterations), PolicyError);
}

TEST_CASE("a hardware thread count of 0 is refused", "[OrcaBench][Policy]")
{
    CHECK_THROWS_AS(Policy::resolve("quick", {}, 0), PolicyError);
}

TEST_CASE("stages parse in any order and list in pipeline order", "[OrcaBench][Policy]")
{
    CHECK(to_string(parse_stages("export,load")) == "load,export");
    CHECK(to_string(parse_stages("process,process")) == "process");
}

TEST_CASE("the spaces around a stage name are ignored", "[OrcaBench][Policy]")
{
    CHECK(to_string(parse_stages(" process , export ")) == "process,export");
}

TEST_CASE("an unknown or empty stage name is refused", "[OrcaBench][Policy]")
{
    const std::string list = GENERATE(as<std::string> {}, "", " ", "slice", "process,", ",export", "process,,export", "process, ,export");
    CAPTURE(list);
    CHECK_THROWS_AS(parse_stages(list), PolicyError);
}

TEST_CASE("the identity records every setting compare enforces", "[OrcaBench][Policy]")
{
    const MeasurementIdentity expected = {{"affinity", "none"},
                                          {"corpus", "embedded,handy"},
                                          {"iterations", "3"},
                                          {"metrics", "wall,rss,hash,work"},
                                          {"policy", "quick"},
                                          {"sampling", "5ms"},
                                          {"stages", "load,process,export"},
                                          {"threads", std::to_string(hardware)},
                                          {"warmup", "1"}};
    CHECK(resolved("quick").identity() == expected);
}

TEST_CASE("changing any recorded setting changes the identity", "[OrcaBench][Policy]")
{
    const std::vector<std::pair<std::string, std::function<void(Policy&)>>> changes = {
        {"name", [](Policy& policy) { policy.name = "precise"; }},
        {"threads", [](Policy& policy) { policy.threads = 3; }},
        {"warmup", [](Policy& policy) { policy.warmup = 5; }},
        {"iterations", [](Policy& policy) { policy.iterations = 9; }},
        {"stages", [](Policy& policy) { policy.stages = {Stage::Export}; }},
        {"metrics", [](Policy& policy) { policy.metrics = {Metric::Hash}; }},
    };
    const Policy base = resolved("quick");
    for (const auto& [field, change] : changes) {
        CAPTURE(field);
        Policy changed = base;
        change(changed);
        CHECK(changed.identity() != base.identity());
    }
}
