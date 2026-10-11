#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include "core/Document.hpp"
#include "core/Policy.hpp"
#include "core/Result.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>

using namespace Slic3r::Bench;
using namespace std::chrono_literals;

namespace {

using WallTime = decltype(Suite::started_at);

std::string fixture()
{
    std::ifstream file(std::string(TEST_DATA_DIR) + "/orcabench/result_v1.json");
    REQUIRE(file);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

// Replaces text that occurs exactly once in the document.
std::string edited(std::string document, const std::string& from, const std::string& to)
{
    const auto at = document.find(from);
    REQUIRE(at != std::string::npos);
    REQUIRE(document.find(from, at + 1) == std::string::npos);
    document.replace(at, from.size(), to);
    return document;
}

// The document without the whitespace between tokens, so a comparison covers keys, their order
// and values, and not how the writer indents.
std::string compact(const std::string& document)
{
    std::string out;
    bool        in_string = false;
    for (std::size_t i = 0; i < document.size(); ++i) {
        const char c = document[i];
        if (in_string) {
            out += c;
            if (c == '\\' && i + 1 < document.size())
                out += document[++i];
            else if (c == '"')
                in_string = false;
        } else if (c == '"') {
            in_string = true;
            out += c;
        } else if (c != ' ' && c != '\n' && c != '\r' && c != '\t') {
            out += c;
        }
    }
    return out;
}

StageSpan span_at(std::string stage, std::string scope, Clock::time_point base, Clock::duration at,
                  Clock::duration length, Metrics metrics = {})
{
    StageSpan span;
    span.stage      = std::move(stage);
    span.scope      = std::move(scope);
    span.started_at = base + at;
    span.done_at    = span.started_at + length;
    span.metrics    = std::move(metrics);
    return span;
}

// The result that result_v1.json describes, on a base other than the epoch so the fixture
// matches only offsets measured from each iteration's origin().
Result sample()
{
    const Clock::time_point base = Clock::time_point {} + 5h;

    Result result;
    result.suite.started_at = WallTime(std::chrono::milliseconds(1790613727123)); // 2026-09-28T16:42:07.123Z
    result.suite.duration   = 14320000000ns;

    result.measurement = {{MeasurementKey::policy, "quick"},
                          {MeasurementKey::warmup, "1"},
                          {MeasurementKey::iterations, "3"},
                          {MeasurementKey::threads, "20"},
                          {MeasurementKey::stages, "load,process,export"},
                          {MeasurementKey::metrics, "wall,rss,hash,work"},
                          {MeasurementKey::corpus, "embedded,handy"},
                          {MeasurementKey::affinity, "none"},
                          {MeasurementKey::sampling, "5ms"}};

    result.build.revision         = "a83e6b7b74";
    result.build.dirty            = true;
    result.build.compiler         = "Clang";
    result.build.compiler_version = "22.1.3";
    result.build.config           = "Release";
    result.build.flags            = "/DWIN32 /D_WINDOWS /GR /EHsc /W4 /MD /O2 /Ob2 /DNDEBUG";
    result.build.properties       = {{"pgo", "none"}};

    result.machine.host          = "BENCH-PC";
    result.machine.os            = "Windows 10.0.26200";
    result.machine.cpu           = "13th Gen Intel(R) Core(TM) i5-13600K";
    result.machine.logical_cores = 20;
    result.machine.properties    = {{"cores", "6P+8E"}};

    WorkStats work;
    work.moves         = 606972;
    work.layers        = 190;
    work.gcode_bytes   = 12604416;
    work.travel_moves  = 41233;
    work.extrusion_mm3 = 48211.7;
    work.metrics       = {{"extrusion_mm3.support", 3910.2}, {"moves.perimeter", 214887.0}};

    IterationResult first;
    first.wall           = 1133700000ns;
    first.peak_rss_bytes = 431906816;
    first.cpu            = 8389380000ns;
    first.timeline       = {span_at("posSlice", "object:0", base, 0ns, 12900000ns),
                            span_at("posInfill", "object:0", base, 231400000ns, 93000000ns, {{"peak_rss_bytes", 431906816.0}}),
                            span_at("psGCodeExport", "print", base, 324600000ns, 808900000ns)};
    first.unfinished     = {{"posEstimateCurledExtrusions", "object:0", base + 324500000ns}};
    first.not_run        = {{"posContouring", "object:0"}};

    IterationResult second;
    second.wall           = 1126900000ns;
    second.peak_rss_bytes = 430071808;
    second.cpu            = 8451750000ns;
    second.timeline       = {span_at("posSlice", "object:0", base, 0ns, 12400000ns),
                             span_at("posInfill", "object:0", base, 229800000ns, 90000000ns, {{"peak_rss_bytes", 430071808.0}}),
                             span_at("psGCodeExport", "print", base, 320000000ns, 804600000ns)};
    second.unfinished     = {{"posEstimateCurledExtrusions", "object:0", base + 319900000ns}};
    second.not_run        = {{"posContouring", "object:0"}};

    result.workloads ={WorkloadResult::ran("slice/extruder-idler/standard-0.20", 0x3bbecdbaa64c8fe8, work, {first, second}),
                        WorkloadResult::skipped("slice/benchy/standard-0.20", "fixture not available: corpus 'external' not fetched"),
                        WorkloadResult::failed("slice/overhangs/tree-supports", "threw std::bad_alloc in posSupportMaterial")};
    return result;
}

} // namespace

TEST_CASE("a result is written exactly as the schema lays it out", "[OrcaBench][Document]")
{
    // The fixture is written by hand from the schema, so never regenerate it from this writer.
    CHECK(compact(write_document(sample())) == compact(fixture()));
}

TEST_CASE("the fixture's measurement is what quick records at 20 threads", "[OrcaBench][Document]")
{
    PolicyOverrides overrides;
    overrides.threads = 20;
    CHECK(read_document(fixture()).measurement == Policy::resolve("quick", overrides, 20).identity());
}

TEST_CASE("a document reads and writes back unchanged", "[OrcaBench][Document]")
{
    const std::string document = fixture();
    CHECK(compact(write_document(read_document(document))) == compact(document));
}

TEST_CASE("an iteration read back starts at the epoch, keeping its offsets and durations", "[OrcaBench][Document]")
{
    const Result before = sample();
    const Result after  = read_document(write_document(before));

    const IterationResult& written = before.workloads.front().iterations.front();
    const IterationResult& read    = after.workloads.front().iterations.front();
    REQUIRE(read.timeline.size() == written.timeline.size());
    CHECK(origin(read) == Clock::time_point {});
    for (std::size_t i = 0; i < read.timeline.size(); ++i) {
        CAPTURE(i);
        CHECK(read.timeline[i].started_at - origin(read) == written.timeline[i].started_at - origin(written));
        CHECK(read.timeline[i].done_at - read.timeline[i].started_at == written.timeline[i].done_at - written.timeline[i].started_at);
    }
    REQUIRE(read.unfinished.size() == 1);
    CHECK(read.unfinished.front().started_at - origin(read) == written.unfinished.front().started_at - origin(written));
}

TEST_CASE("an unfinished step that started first is the offset every other start is written from", "[OrcaBench][Document]")
{
    const Clock::time_point base = Clock::time_point {} + 5h;
    IterationResult         iteration;
    iteration.timeline   = {span_at("posSlice", "object:0", base, 5ms, 1ms)};
    iteration.unfinished = {{"posEstimateCurledExtrusions", "object:0", base}};
    Result result;
    result.workloads = {WorkloadResult::ran("slice/idler", 1, {}, {iteration})};

    const std::string text = compact(write_document(result));
    CHECK(text.find(R"({"stage":"posSlice","scope":"object:0","at_ns":5000000,)") != std::string::npos);
    CHECK(text.find(R"({"stage":"posEstimateCurledExtrusions","scope":"object:0","at_ns":0})") != std::string::npos);
}

TEST_CASE("timestamps are written as UTC and read back to the millisecond", "[OrcaBench][Document]")
{
    // Fixed instants, since a calendar wrong in both directions would still round-trip.
    const auto [unix_ms, text] = GENERATE(table<std::int64_t, std::string>({
        {0, "1970-01-01T00:00:00.000Z"},
        {-1, "1969-12-31T23:59:59.999Z"},
        {-1000, "1969-12-31T23:59:59.000Z"},
        {951782400000, "2000-02-29T00:00:00.000Z"},
        {2147483648000, "2038-01-19T03:14:08.000Z"},
        {4107542400000, "2100-03-01T00:00:00.000Z"},
    }));
    CAPTURE(unix_ms, text);

    Result result;
    result.suite.started_at = WallTime(std::chrono::milliseconds(unix_ms));
    const std::string document = write_document(result);

    CHECK(compact(document).find("\"started_at\":\"" + text + "\"") != std::string::npos);
    CHECK(read_document(document).suite.started_at == result.suite.started_at);
}

TEST_CASE("a malformed document throws DocumentError", "[OrcaBench][Document]")
{
    CHECK_THROWS_AS(read_document("{"), DocumentError);
    CHECK_THROWS_AS(read_document("[]"), DocumentError);

    CHECK_THROWS_AS(read_document(edited(fixture(), R"("dirty": true)", R"("dirty": "yes")")), DocumentError);
}

TEST_CASE("a document is refused when its schema major differs or its version is not major.minor", "[OrcaBench][Document]")
{
    const std::string version = GENERATE(as<std::string> {}, "2.0", "0.9", "1", "1.", ".0", "1.0.0", "1.x", "1.-1", "v1.0");
    CAPTURE(version);
    CHECK_THROWS_AS(read_document(edited(fixture(), R"("schema": "1.0")", "\"schema\": \"" + version + "\"")), DocumentError);
}

TEST_CASE("a document of a newer minor version is read, ignoring the fields this build does not know", "[OrcaBench][Document]")
{
    // An unknown field in every kind of block, so a reader strict in any one of them fails.
    const std::pair<std::string, std::string> additions[] = {
        {R"("schema": "1.0")", R"("schema": "1.7", "notes": {"by": "ci"})"},
        {R"("duration_ns": 14320000000)", R"("duration_ns": 14320000000, "timezone": "UTC")"},
        {R"("flags": )", R"("linker": "lld", "flags": )"},
        {R"("logical_cores": 20)", R"("logical_cores": 20, "memory_bytes": 34359738368)"},
        {R"("output_hash": )", R"("tags": ["nightly"], "output_hash": )"},
        {R"("travel_moves": 41233)", R"("travel_moves": 41233, "retractions": 12)"},
        {R"("wall_ns": 1133700000)", R"("wall_ns": 1133700000, "threads_used": 20)"},
        {R"("duration_ns": 12900000)", R"("duration_ns": 12900000, "thread": 3)"},
        {R"("at_ns": 324500000)", R"("at_ns": 324500000, "thread": 5)"},
        {"\"object:0\"\n            }\n          ]\n        },",
         "\"object:0\", \"reason\": \"no fuzzy skin\"\n            }\n          ]\n        },"},
        {R"("skipped": )", R"("retry": true, "skipped": )"},
    };
    std::string document = fixture();
    for (const auto &[from, to] : additions)
        document = edited(document, from, to);

    // Written back as this build's version, with nothing but the new fields gone.
    CHECK(compact(write_document(read_document(document))) == compact(fixture()));
}

TEST_CASE("measurement keys and stage names this build has never seen are kept", "[OrcaBench][Document]")
{
    Result result = sample();
    result.measurement["seed"] = "42";
    result.workloads.front().iterations.front().timeline.front().stage = "posFutureStep";

    const Result read = read_document(write_document(result));
    CHECK(read.measurement == result.measurement);
    CHECK(read.workloads.front().iterations.front().timeline.front().stage == "posFutureStep");
}

TEST_CASE("a skip or failure is not written without a reason or with iterations", "[OrcaBench][Document]")
{
    WorkloadResult skip;
    skip.name    = "slice/benchy/standard-0.20";
    skip.outcome = Outcome::Skipped;
    Result no_reason;
    no_reason.workloads = {skip};
    CHECK_THROWS_AS(write_document(no_reason), DocumentError);

    Result with_iterations;
    with_iterations.workloads = {WorkloadResult::failed("slice/overhangs/tree-supports", "threw std::bad_alloc")};
    with_iterations.workloads.front().iterations.emplace_back();
    CHECK_THROWS_AS(write_document(with_iterations), DocumentError);
}

TEST_CASE("a value the run did not collect is left out and read back absent", "[OrcaBench][Document]")
{
    Result          result   = sample();
    WorkloadResult& workload = result.workloads.front();
    workload.output_hash.reset();
    workload.work.reset();
    for (IterationResult& iteration : workload.iterations) {
        iteration.peak_rss_bytes.reset();
        // The spans' metrics use the same key.
        for (StageSpan& span : iteration.timeline)
            span.metrics.clear();
    }
    const std::string text = write_document(result);
    CHECK(text.find("output_hash") == std::string::npos);
    CHECK(text.find("\"work\"") == std::string::npos);
    CHECK(text.find("peak_rss_bytes") == std::string::npos);

    const WorkloadResult read = read_document(text).workloads.front();
    CHECK_FALSE(read.output_hash.has_value());
    CHECK_FALSE(read.work.has_value());
    REQUIRE(read.iterations.size() == 2);
    for (const IterationResult& iteration : read.iterations)
        CHECK_FALSE(iteration.peak_rss_bytes.has_value());
}

TEST_CASE("a workload that ran is not written with a reason", "[OrcaBench][Document]")
{
    Result result;
    result.workloads = {WorkloadResult::ran("slice/idler", 1, {}, {})};
    result.workloads.front().reason = "retried after std::bad_alloc";
    CHECK_THROWS_AS(write_document(result), DocumentError);
}

TEST_CASE("a document naming a workload twice is neither written nor read", "[OrcaBench][Document]")
{
    Result twice;
    twice.workloads = {WorkloadResult::skipped("slice/benchy/standard-0.20", "fixture not available"),
                       WorkloadResult::failed("slice/benchy/standard-0.20", "threw std::bad_alloc")};
    CHECK_THROWS_AS(write_document(twice), DocumentError);
    const std::string repeated = edited(fixture(), R"("name": "slice/overhangs/tree-supports")", R"("name": "slice/benchy/standard-0.20")");
    CHECK_THROWS_AS(read_document(repeated), DocumentError);
}

TEST_CASE("a skip or failure is refused without a reason, with iterations, or when marked as both", "[OrcaBench][Document]")
{
    const auto [from, to] = GENERATE(table<std::string, std::string>({
        {R"("skipped": "fixture not available: corpus 'external' not fetched")", R"("skipped": "")"},
        // A valid iteration, so the refusal is for its presence alone.
        {R"("failed": )", R"("iterations": [{"wall_ns": 1, "peak_rss_bytes": 1, "cpu_ns": 1, "spans": []}], "failed": )"},
        {R"("skipped": )", R"("failed": "threw", "skipped": )"},
    }));
    CAPTURE(to);
    CHECK_THROWS_AS(read_document(edited(fixture(), from, to)), DocumentError);
}

TEST_CASE("a negative, fractional or oversized integer field is refused", "[OrcaBench][Document]")
{
    // A negative in every integer field, since any one of them could be read without the check.
    const auto [from, to] = GENERATE(table<std::string, std::string>({
        {R"("duration_ns": 14320000000)", R"("duration_ns": -14320000000)"},
        {R"("logical_cores": 20)", R"("logical_cores": -20)"},
        {R"("moves": 606972)", R"("moves": -606972)"},
        {R"("layers": 190)", R"("layers": -190)"},
        {R"("gcode_bytes": 12604416)", R"("gcode_bytes": -12604416)"},
        {R"("travel_moves": 41233)", R"("travel_moves": -41233)"},
        {R"("wall_ns": 1133700000)", R"("wall_ns": -1133700000)"},
        {R"("peak_rss_bytes": 431906816,)", R"("peak_rss_bytes": -431906816,)"},
        {R"("cpu_ns": 8389380000)", R"("cpu_ns": -8389380000)"},
        {R"("at_ns": 231400000)", R"("at_ns": -231400000)"},
        {R"("duration_ns": 12900000)", R"("duration_ns": -12900000)"},
        {R"("at_ns": 324500000)", R"("at_ns": -324500000)"},
        {R"("layers": 190)", R"("layers": 1.5)"},
        {R"("logical_cores": 20)", R"("logical_cores": 4294967296)"},
    }));
    CAPTURE(to);
    CHECK_THROWS_AS(read_document(edited(fixture(), from, to)), DocumentError);
}

TEST_CASE("an unfinished or not-run stage missing a field is refused", "[OrcaBench][Document]")
{
    const auto [from, to] = GENERATE(table<std::string, std::string>({
        {R"("at_ns": 324500000)", R"("at": 324500000)"},
        {"\"scope\": \"object:0\"\n            }\n          ]\n        },",
         "\"where\": \"object:0\"\n            }\n          ]\n        },"},
    }));
    CAPTURE(to);
    CHECK_THROWS_AS(read_document(edited(fixture(), from, to)), DocumentError);
}

TEST_CASE("a span that would end past the largest time the clock can hold is refused", "[OrcaBench][Document]")
{
    CHECK_THROWS_AS(read_document(edited(fixture(), R"("at_ns": 231400000)", R"("at_ns": 9223372036854775807)")), DocumentError);
}

TEST_CASE("a value that would not read back is not written", "[OrcaBench][Document]")
{
    Result backwards = sample();
    StageSpan& span = backwards.workloads.front().iterations.front().timeline.back();
    span.done_at = span.started_at - 1ns;
    CHECK_THROWS_AS(write_document(backwards), DocumentError);

    Result not_a_number = sample();
    not_a_number.workloads.front().iterations.front().metrics["rate"] = std::numeric_limits<double>::quiet_NaN();
    CHECK_THROWS_AS(write_document(not_a_number), DocumentError);

    Result infinite = sample();
    infinite.workloads.front().work->extrusion_mm3 = std::numeric_limits<double>::infinity();
    CHECK_THROWS_AS(write_document(infinite), DocumentError);

    Result far_future = sample();
    far_future.suite.started_at = WallTime(std::chrono::milliseconds(253402300800000)); // 10000-01-01T00:00:00.000Z
    CHECK_THROWS_AS(write_document(far_future), DocumentError);

    Result before_year_zero = sample();
    before_year_zero.suite.started_at = WallTime(std::chrono::milliseconds(-62167219200001)); // -0001-12-31T23:59:59.999Z
    CHECK_THROWS_AS(write_document(before_year_zero), DocumentError);
}

TEST_CASE("a document is read from its file, and an error names the file", "[OrcaBench][Document]")
{
    const std::string path = std::string(TEST_DATA_DIR) + "/orcabench/result_v1.json";
    CHECK(read_document_file(path).workloads.size() == 3);
    CHECK_THROWS_WITH(read_document_file(path + ".missing"), "cannot read " + path + ".missing");
    const std::string newer = std::string(TEST_DATA_DIR) + "/orcabench/result_v9.json";
    CHECK_THROWS_WITH(read_document_file(newer), newer + ": schema 9.0 cannot be read by this build, which reads 1.x");
}

TEST_CASE("a document's path is UTF-8", "[OrcaBench][Document]")
{
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / std::filesystem::u8path("orcabench-\xc3\xa9");
    std::filesystem::create_directories(directory);
    std::filesystem::copy_file(std::string(TEST_DATA_DIR) + "/orcabench/result_v1.json", directory / "result.json",
                               std::filesystem::copy_options::overwrite_existing);
    CHECK(read_document_file((directory / "result.json").u8string()).workloads.size() == 3);
    std::filesystem::remove_all(directory);
}
