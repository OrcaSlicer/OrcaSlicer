#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Measurement.hpp"
#include "core/Result.hpp"
#include "core/Sampler.hpp"

#include <chrono>
#include <limits>
#include <stdexcept>

using namespace Slic3r::Bench;
using namespace std::chrono_literals;

TEST_CASE("a span keeps what the workload reported", "[OrcaBench][Measurement]")
{
    const Clock::time_point start = Clock::time_point {} + 1s;
    Measurement measurement(Clock::time_point {});
    measurement.span("posSlice", Scope::object(2), start, start + 5ms, {{"layers", 1024.0}});
    REQUIRE(measurement.timeline().size() == 1);
    const StageSpan& span = measurement.timeline().front();
    CHECK(span.stage == "posSlice");
    CHECK(span.scope == "object:2");
    CHECK(span.started_at == start);
    CHECK(span.done_at == start + 5ms);
    REQUIRE(span.metrics.count("layers") == 1);
    CHECK_THAT(span.metrics.at("layers"), Catch::Matchers::WithinAbs(1024.0, 0.0));
}

TEST_CASE("a span that ends before it starts is refused", "[OrcaBench][Measurement]")
{
    const Clock::time_point start = Clock::time_point {} + 1s;
    Measurement measurement(Clock::time_point {});
    CHECK_THROWS_AS(measurement.span("posSlice", Scope::print(), start, start - 1ms), std::invalid_argument);
}

TEST_CASE("a span without a stage is refused", "[OrcaBench][Measurement]")
{
    Measurement measurement(Clock::time_point {});
    CHECK_THROWS_AS(measurement.span("", Scope::print(), Clock::time_point {}, Clock::time_point {}), std::invalid_argument);
}

TEST_CASE("a span metric under a key the sampler writes is refused", "[OrcaBench][Measurement]")
{
    const std::string key =
        GENERATE(as<std::string> {}, SampledMetric::peak_rss_bytes, SampledMetric::cpu_ns, SampledMetric::cpu_window_ns);
    CAPTURE(key);
    Measurement measurement(Clock::time_point {});
    CHECK_THROWS_AS(measurement.span("posSlice", Scope::print(), Clock::time_point {}, Clock::time_point {}, {{key, 1.0}}),
                    std::invalid_argument);
}

TEST_CASE("a span that starts before its iteration is refused", "[OrcaBench][Measurement]")
{
    const Clock::time_point start = Clock::time_point {} + 1s;
    Measurement measurement(start);
    CHECK_THROWS_AS(measurement.span("posSlice", Scope::print(), start - 1ms, start + 5ms), std::invalid_argument);
    CHECK_NOTHROW(measurement.span("posSlice", Scope::print(), start, start + 5ms));
}

TEST_CASE("a span that ends after it is reported is refused", "[OrcaBench][Measurement]")
{
    const Clock::time_point now = Clock::now();
    Measurement measurement(now);
    CHECK_THROWS_AS(measurement.span("posSlice", Scope::print(), now, now + 1h), std::invalid_argument);
}

TEST_CASE("a step that never finished or never ran keeps its stage and scope", "[OrcaBench][Measurement]")
{
    const Clock::time_point start = Clock::time_point {} + 1s;
    Measurement measurement(Clock::time_point {});
    measurement.unfinished("posEstimateCurledExtrusions", Scope::object(1), start);
    measurement.not_run("posContouring", Scope::object(1));
    REQUIRE(measurement.unfinished_stages().size() == 1);
    const UnfinishedStage& unfinished = measurement.unfinished_stages().front();
    CHECK(unfinished.stage == "posEstimateCurledExtrusions");
    CHECK(unfinished.scope == "object:1");
    CHECK(unfinished.started_at == start);
    REQUIRE(measurement.stages_not_run().size() == 1);
    CHECK(measurement.stages_not_run().front().stage == "posContouring");
    CHECK(measurement.stages_not_run().front().scope == "object:1");
}

TEST_CASE("an unfinished step is refused without a stage or with a start outside its iteration", "[OrcaBench][Measurement]")
{
    const Clock::time_point now = Clock::now();
    Measurement measurement(now);
    CHECK_THROWS_AS(measurement.unfinished("", Scope::print(), now), std::invalid_argument);
    CHECK_THROWS_AS(measurement.unfinished("posEstimateCurledExtrusions", Scope::print(), now - 1ms), std::invalid_argument);
    CHECK_THROWS_AS(measurement.unfinished("posEstimateCurledExtrusions", Scope::print(), now + 1h), std::invalid_argument);
    CHECK_NOTHROW(measurement.unfinished("posEstimateCurledExtrusions", Scope::print(), now));
}

TEST_CASE("a step that did not run is refused without a stage", "[OrcaBench][Measurement]")
{
    Measurement measurement(Clock::time_point {});
    CHECK_THROWS_AS(measurement.not_run("", Scope::print()), std::invalid_argument);
}

TEST_CASE("a stage reported twice for one scope is refused in any form", "[OrcaBench][Measurement]")
{
    const std::string first  = GENERATE(as<std::string> {}, "span", "unfinished", "not run");
    const std::string second = GENERATE(as<std::string> {}, "span", "unfinished", "not run");
    CAPTURE(first, second);
    const Clock::time_point start  = Clock::time_point {} + 1s;
    const auto              report = [start](Measurement& measurement, const std::string& form, const Scope& scope) {
        if (form == "span")
            measurement.span("posInfill", scope, start, start);
        else if (form == "unfinished")
            measurement.unfinished("posInfill", scope, start);
        else
            measurement.not_run("posInfill", scope);
    };
    Measurement measurement(Clock::time_point {});
    report(measurement, first, Scope::object(0));
    CHECK_THROWS_AS(report(measurement, second, Scope::object(0)), std::invalid_argument);
    CHECK_NOTHROW(report(measurement, second, Scope::object(1)));
}

TEST_CASE("a metric belongs to the iteration even after a span", "[OrcaBench][Measurement]")
{
    const Clock::time_point start = Clock::time_point {} + 1s;
    Measurement measurement(Clock::time_point {});
    measurement.span("posSlice", Scope::print(), start, start + 5ms);
    measurement.metric("cpu_ratio", 7.5);
    REQUIRE(measurement.timeline().size() == 1);
    CHECK(measurement.timeline().front().metrics.empty());
    REQUIRE(measurement.metrics().count("cpu_ratio") == 1);
    CHECK_THAT(measurement.metrics().at("cpu_ratio"), Catch::Matchers::WithinAbs(7.5, 0.0));
}

TEST_CASE("a number that is not finite is refused where it is reported", "[OrcaBench][Measurement]")
{
    const double value = GENERATE(std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                                  -std::numeric_limits<double>::infinity());
    CAPTURE(value);

    Measurement span_metric(Clock::time_point {});
    CHECK_THROWS_AS(span_metric.span("posSlice", Scope::print(), Clock::time_point {}, Clock::time_point {}, {{"layers", value}}),
                    std::invalid_argument);

    Measurement iteration_metric(Clock::time_point {});
    CHECK_THROWS_AS(iteration_metric.metric("cpu_ratio", value), std::invalid_argument);

    WorkStats volume;
    volume.extrusion_mm3 = value;
    Measurement work_volume(Clock::time_point {});
    CHECK_THROWS_AS(work_volume.work(volume), std::invalid_argument);

    WorkStats by_role;
    by_role.metrics["extrusion_mm3.support"] = value;
    Measurement work_metric(Clock::time_point {});
    CHECK_THROWS_AS(work_metric.work(by_role), std::invalid_argument);
}

TEST_CASE("a metric reported twice in one iteration is refused", "[OrcaBench][Measurement]")
{
    Measurement measurement(Clock::time_point {});
    measurement.metric("cpu_ratio", 7.5);
    CHECK_THROWS_AS(measurement.metric("cpu_ratio", 7.5), std::invalid_argument);
}

TEST_CASE("work stats and the output hash are kept", "[OrcaBench][Measurement]")
{
    Measurement measurement(Clock::time_point {});
    WorkStats   work;
    work.layers = 190;
    measurement.work(work);
    measurement.output_hash(0x3bbecdbaa64c8fe8);
    REQUIRE(measurement.work_stats().has_value());
    CHECK(measurement.work_stats()->layers == 190);
    REQUIRE(measurement.hash().has_value());
    CHECK(*measurement.hash() == 0x3bbecdbaa64c8fe8);
}

TEST_CASE("work stats or an output hash reported twice is refused", "[OrcaBench][Measurement]")
{
    Measurement measurement(Clock::time_point {});
    measurement.work(WorkStats {});
    CHECK_THROWS_AS(measurement.work(WorkStats {}), std::invalid_argument);
    measurement.output_hash(1);
    CHECK_THROWS_AS(measurement.output_hash(2), std::invalid_argument);
}

TEST_CASE("a scope names the print or one object", "[OrcaBench][Measurement]")
{
    CHECK(Scope::print().text() == "print");
    CHECK(Scope::object(3).text() == "object:3");
}
