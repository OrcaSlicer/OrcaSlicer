#include "core/Document.hpp"

#include "core/Result.hpp"
#include "nlohmann/json.hpp"

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Slic3r { namespace Bench {

namespace {

// Keeps insertion order, so a document lists fields in the order the schema gives them.
using Json = nlohmann::ordered_json;

using WallTime = decltype(Suite::started_at);

constexpr std::int64_t seconds_per_day = 86400;
constexpr std::int64_t ms_per_second   = 1000;

// Rounds toward negative infinity, unlike integer division, so a time before the epoch gets the
// correct second and day.
std::int64_t floor_div(std::int64_t value, std::int64_t divisor)
{
    return (value >= 0 ? value : value - divisor + 1) / divisor;
}

std::int64_t to_ns(Clock::duration duration)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
}

// Hex text, since jq and JavaScript read every JSON number as a double and would round a 64-bit
// hash.
std::string to_hex(std::uint64_t value)
{
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
    return text;
}

std::uint64_t from_hex(const std::string& text)
{
    std::uint64_t value = 0;
    const char* last = text.data() + text.size();
    const auto [end, error] = std::from_chars(text.data(), last, value, 16);
    if (text.size() != 16 || error != std::errc() || end != last)
        throw DocumentError("output_hash is not sixteen hex digits: \"" + text + "\"");
    return value;
}

// Howard Hinnant's civil calendar algorithms, converting between days since 1970-01-01 and a
// date without the platform-specific UTC calls.
std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto         yoe = static_cast<unsigned>(y - era * 400);
    const unsigned     doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned     doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

void civil_from_days(std::int64_t z, std::int64_t& y, unsigned& m, unsigned& d)
{
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto         doe = static_cast<unsigned>(z - era * 146097);
    const unsigned     yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned     doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned     mp  = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = static_cast<std::int64_t>(yoe) + era * 400 + (m <= 2);
}

// Accepts only the form to_iso8601() writes.
WallTime from_iso8601(const std::string& text)
{
    const auto malformed = [&text]() { return DocumentError("started_at is not YYYY-MM-DDTHH:MM:SS.mmmZ: \"" + text + "\""); };
    if (text.size() != 24 || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' || text[16] != ':' ||
        text[19] != '.' || text[23] != 'Z')
        throw malformed();
    const auto field = [&](std::size_t at, std::size_t width) {
        unsigned value = 0;
        const char* first = text.data() + at;
        const auto [end, error] = std::from_chars(first, first + width, value);
        if (error != std::errc() || end != first + width)
            throw malformed();
        return value;
    };
    const unsigned year = field(0, 4), month = field(5, 2), day = field(8, 2);
    const unsigned hour = field(11, 2), minute = field(14, 2), second = field(17, 2), millisecond = field(20, 3);
    const std::int64_t days = days_from_civil(year, month, day);

    // Converting back rejects a date that does not exist, such as February 30.
    std::int64_t check_year  = 0;
    unsigned     check_month = 0;
    unsigned     check_day   = 0;
    civil_from_days(days, check_year, check_month, check_day);
    if (check_year != year || check_month != month || check_day != day || hour > 23 || minute > 59 || second > 59)
        throw malformed();
    const std::int64_t seconds = days * seconds_per_day + hour * 3600 + minute * 60 + second;
    return WallTime(std::chrono::milliseconds(seconds * ms_per_second + millisecond));
}

// Checked when writing as well as reading, so every document written reads back.
void require_reason_without_data(const std::string& name, const std::string& reason, bool has_iterations)
{
    if (reason.empty())
        throw DocumentError(name + " did not run and gives no reason");
    if (has_iterations)
        throw DocumentError(name + " did not run but carries iterations");
}

// Results are keyed by workload name, so one named twice is refused both ways.
void require_unique_names(const std::vector<WorkloadResult>& workloads)
{
    std::set<std::string> names;
    for (const WorkloadResult& workload : workloads)
        if (!names.insert(workload.name).second)
            throw DocumentError(workload.name + " appears twice");
}

// Refuses NaN and infinity, which nlohmann writes as null, and negative integers, which the
// reader would reject.
void require_numbers_read_back(const Json& json, std::string& path)
{
    if (json.is_structured()) {
        for (const auto& item : json.items()) {
            const std::size_t length = path.size();
            path += '/';
            path += item.key();
            require_numbers_read_back(item.value(), path);
            path.resize(length);
        }
        return;
    }
    if (json.is_number_float() && !std::isfinite(json.get<double>()))
        throw DocumentError(path + " is not a finite number");
    if (json.is_number_integer() && !json.is_number_unsigned() && json.get<std::int64_t>() < 0)
        throw DocumentError(path + " is negative: " + json.dump());
}

void put_unless_empty(Json& json, const char* key, Json value)
{
    if (!value.empty())
        json[key] = std::move(value);
}

template<class Map> Map optional_map(const Json& json, const char* key)
{
    const auto it = json.find(key);
    return it == json.end() ? Map() : it->get<Map>();
}

// Refuses anything but a whole number the field can hold, since nlohmann's get() would silently
// wrap -5 and truncate 1.5.
template<class T> T integer_at(const Json& json, const char* key)
{
    const Json&    value = json.at(key);
    constexpr auto most  = static_cast<std::uint64_t>(std::numeric_limits<T>::max());
    // nlohmann parses a number as unsigned only when it has no sign, fraction or exponent.
    if (!value.is_number_unsigned() || value.get<std::uint64_t>() > most)
        throw DocumentError(std::string(key) + " is not a whole number from 0 to " + std::to_string(most) + ": " + value.dump());
    return static_cast<T>(value.get<std::uint64_t>());
}

Clock::duration duration_at(const Json& json, const char* key)
{
    return std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(integer_at<std::int64_t>(json, key)));
}

const Json& array_at(const Json& json, const char* key)
{
    const Json& value = json.at(key);
    if (!value.is_array())
        throw DocumentError(std::string(key) + " is not an array");
    return value;
}

const Json& optional_array_at(const Json& json, const char* key)
{
    static const Json none = Json::array();
    return json.contains(key) ? array_at(json, key) : none;
}

void require_readable(const std::string& version)
{
    const auto whole = [](const char* first, const char* last, unsigned& value) {
        const auto [end, error] = std::from_chars(first, last, value);
        return error == std::errc() && end == last;
    };
    unsigned    major = 0;
    unsigned    minor = 0;
    const auto  dot   = version.find('.');
    const char* text  = version.data();
    if (dot == std::string::npos || !whole(text, text + dot, major) || !whole(text + dot + 1, text + version.size(), minor))
        throw DocumentError("schema is not major.minor: \"" + version + "\"");
    if (major != schema_major)
        throw DocumentError("schema " + version + " cannot be read by this build, which reads " + std::to_string(schema_major) + ".x");
}

Json write_span(const StageSpan& span, Clock::time_point zero)
{
    Json json;
    json["stage"]       = span.stage;
    json["scope"]       = span.scope;
    json["at_ns"]       = to_ns(span.started_at - zero);
    json["duration_ns"] = to_ns(span.done_at - span.started_at);
    put_unless_empty(json, "metrics", Json(span.metrics));
    return json;
}

StageSpan read_span(const Json& json)
{
    StageSpan span;
    span.stage = json.at("stage").get<std::string>();
    span.scope = json.at("scope").get<std::string>();
    const Clock::duration at     = duration_at(json, "at_ns");
    const Clock::duration length = duration_at(json, "duration_ns");
    if (length > Clock::duration::max() - at)
        throw DocumentError("a " + span.stage + " span ends past the largest time the clock can hold");
    // Rebases onto the epoch, since the writer's steady_clock means nothing in this process.
    span.started_at = Clock::time_point {} + at;
    span.done_at    = span.started_at + length;
    span.metrics    = optional_map<Metrics>(json, "metrics");
    return span;
}

Json write_iteration(const IterationResult& iteration)
{
    Json json;
    json["wall_ns"] = to_ns(iteration.wall);
    if (iteration.peak_rss_bytes)
        json["peak_rss_bytes"] = *iteration.peak_rss_bytes;
    json["cpu_ns"] = to_ns(iteration.cpu);
    Json spans = Json::array();
    const Clock::time_point zero = origin(iteration);
    for (const StageSpan& span : iteration.timeline)
        spans.push_back(write_span(span, zero));
    json["spans"] = std::move(spans);
    Json unfinished = Json::array();
    for (const UnfinishedStage& stage : iteration.unfinished)
        unfinished.push_back(Json {{"stage", stage.stage}, {"scope", stage.scope}, {"at_ns", to_ns(stage.started_at - zero)}});
    put_unless_empty(json, "unfinished", std::move(unfinished));
    Json not_run = Json::array();
    for (const StageNotRun& stage : iteration.not_run)
        not_run.push_back(Json {{"stage", stage.stage}, {"scope", stage.scope}});
    put_unless_empty(json, "not_run", std::move(not_run));
    put_unless_empty(json, "metrics", Json(iteration.metrics));
    return json;
}

IterationResult read_iteration(const Json& json)
{
    IterationResult iteration;
    iteration.wall = duration_at(json, "wall_ns");
    if (json.contains("peak_rss_bytes"))
        iteration.peak_rss_bytes = integer_at<std::uint64_t>(json, "peak_rss_bytes");
    iteration.cpu = duration_at(json, "cpu_ns");
    for (const Json& span : array_at(json, "spans"))
        iteration.timeline.push_back(read_span(span));
    for (const Json& stage : optional_array_at(json, "unfinished")) {
        const Clock::time_point started_at = Clock::time_point {} + duration_at(stage, "at_ns");
        iteration.unfinished.push_back({stage.at("stage").get<std::string>(), stage.at("scope").get<std::string>(), started_at});
    }
    for (const Json& stage : optional_array_at(json, "not_run"))
        iteration.not_run.push_back({stage.at("stage").get<std::string>(), stage.at("scope").get<std::string>()});
    iteration.metrics = optional_map<Metrics>(json, "metrics");
    return iteration;
}

Json write_work(const WorkStats& work)
{
    Json json;
    json["moves"]         = work.moves;
    json["layers"]        = work.layers;
    json["gcode_bytes"]   = work.gcode_bytes;
    json["travel_moves"]  = work.travel_moves;
    json["extrusion_mm3"] = work.extrusion_mm3;
    put_unless_empty(json, "metrics", Json(work.metrics));
    return json;
}

WorkStats read_work(const Json& json)
{
    WorkStats work;
    work.moves         = integer_at<std::uint64_t>(json, "moves");
    work.layers        = integer_at<std::uint64_t>(json, "layers");
    work.gcode_bytes   = integer_at<std::uint64_t>(json, "gcode_bytes");
    work.travel_moves  = integer_at<std::uint64_t>(json, "travel_moves");
    work.extrusion_mm3 = json.at("extrusion_mm3").get<double>();
    work.metrics       = optional_map<Metrics>(json, "metrics");
    return work;
}

Json write_workload(const WorkloadResult& workload)
{
    Json json;
    json["name"] = workload.name;
    if (workload.outcome != Outcome::Ran) {
        require_reason_without_data(workload.name, workload.reason, !workload.iterations.empty());
        json[workload.outcome == Outcome::Skipped ? "skipped" : "failed"] = workload.reason;
        return json;
    }
    if (!workload.reason.empty())
        throw DocumentError(workload.name + " ran but has a reason: " + workload.reason);
    if (workload.output_hash)
        json["output_hash"] = to_hex(*workload.output_hash);
    if (workload.work)
        json["work"] = write_work(*workload.work);
    Json iterations = Json::array();
    for (const IterationResult& iteration : workload.iterations)
        iterations.push_back(write_iteration(iteration));
    json["iterations"] = std::move(iterations);
    return json;
}

WorkloadResult read_workload(const Json& json)
{
    std::string name    = json.at("name").get<std::string>();
    const bool  skipped = json.contains("skipped");
    const bool  failed  = json.contains("failed");
    if (skipped && failed)
        throw DocumentError(name + " is both skipped and failed");
    if (skipped || failed) {
        std::string reason = json.at(skipped ? "skipped" : "failed").get<std::string>();
        require_reason_without_data(name, reason, json.contains("iterations"));
        return skipped ? WorkloadResult::skipped(std::move(name), std::move(reason))
                       : WorkloadResult::failed(std::move(name), std::move(reason));
    }
    std::vector<IterationResult> iterations;
    for (const Json& iteration : array_at(json, "iterations"))
        iterations.push_back(read_iteration(iteration));
    std::optional<std::uint64_t> hash;
    if (json.contains("output_hash"))
        hash = from_hex(json.at("output_hash").get<std::string>());
    std::optional<WorkStats> work;
    if (json.contains("work"))
        work = read_work(json.at("work"));
    return WorkloadResult::ran(std::move(name), hash, std::move(work), std::move(iterations));
}

Json write_build(const BuildIdentity& build)
{
    Json json;
    json["revision"]         = build.revision;
    json["dirty"]            = build.dirty;
    json["compiler"]         = build.compiler;
    json["compiler_version"] = build.compiler_version;
    json["config"]           = build.config;
    json["flags"]            = build.flags;
    put_unless_empty(json, "properties", Json(build.properties));
    return json;
}

BuildIdentity read_build(const Json& json)
{
    BuildIdentity build;
    build.revision         = json.at("revision").get<std::string>();
    build.dirty            = json.at("dirty").get<bool>();
    build.compiler         = json.at("compiler").get<std::string>();
    build.compiler_version = json.at("compiler_version").get<std::string>();
    build.config           = json.at("config").get<std::string>();
    build.flags            = json.at("flags").get<std::string>();
    build.properties       = optional_map<Properties>(json, "properties");
    return build;
}

Json write_machine(const MachineIdentity& machine)
{
    Json json;
    json["host"]          = machine.host;
    json["os"]            = machine.os;
    json["cpu"]           = machine.cpu;
    json["logical_cores"] = machine.logical_cores;
    put_unless_empty(json, "properties", Json(machine.properties));
    return json;
}

MachineIdentity read_machine(const Json& json)
{
    MachineIdentity machine;
    machine.host          = json.at("host").get<std::string>();
    machine.os            = json.at("os").get<std::string>();
    machine.cpu           = json.at("cpu").get<std::string>();
    machine.logical_cores = integer_at<unsigned>(json, "logical_cores");
    machine.properties    = optional_map<Properties>(json, "properties");
    return machine;
}

} // namespace

// Assumes system_clock counts from the Unix epoch, as every implementation does and C++20 requires.
std::string to_iso8601(WallTime time)
{
    const std::int64_t ms      = time.time_since_epoch().count();
    const std::int64_t seconds = floor_div(ms, ms_per_second);
    const std::int64_t days    = floor_div(seconds, seconds_per_day);
    const std::int64_t of_day  = seconds - days * seconds_per_day;
    std::int64_t year  = 0;
    unsigned     month = 0;
    unsigned     day   = 0;
    civil_from_days(days, year, month, day);
    if (year < 0 || year > 9999)
        throw DocumentError("started_at is outside the years 0000 to 9999");
    char text[40];
    std::snprintf(text, sizeof(text), "%04lld-%02u-%02uT%02lld:%02lld:%02lld.%03lldZ", static_cast<long long>(year), month,
                  day, static_cast<long long>(of_day / 3600), static_cast<long long>(of_day % 3600 / 60),
                  static_cast<long long>(of_day % 60), static_cast<long long>(ms - seconds * ms_per_second));
    return text;
}

std::string write_document(const Result& result)
{
    Json json;
    json["schema"] = std::to_string(schema_major) + "." + std::to_string(schema_minor);
    json["suite"]  = Json {{"started_at", to_iso8601(result.suite.started_at)}, {"duration_ns", to_ns(result.suite.duration)}};
    json["measurement"] = Json(result.measurement);
    json["build"]       = write_build(result.build);
    json["machine"]     = write_machine(result.machine);
    require_unique_names(result.workloads);
    Json workloads = Json::array();
    for (const WorkloadResult& workload : result.workloads)
        workloads.push_back(write_workload(workload));
    json["workloads"] = std::move(workloads);
    std::string path;
    require_numbers_read_back(json, path);
    // Replaces invalid UTF-8, which a host name in the system code page can contain.
    return json.dump(2, ' ', false, Json::error_handler_t::replace) + "\n";
}

Result read_document(std::string_view text)
{
    try {
        const Json json = Json::parse(text.begin(), text.end());
        require_readable(json.at("schema").get<std::string>());
        Result result;
        const Json& suite        = json.at("suite");
        result.suite.started_at  = from_iso8601(suite.at("started_at").get<std::string>());
        result.suite.duration    = duration_at(suite, "duration_ns");
        result.measurement       = json.at("measurement").get<MeasurementIdentity>();
        result.build             = read_build(json.at("build"));
        result.machine           = read_machine(json.at("machine"));
        for (const Json& workload : array_at(json, "workloads"))
            result.workloads.push_back(read_workload(workload));
        require_unique_names(result.workloads);
        return result;
    } catch (const Json::exception& error) {
        // Converts the parser's exceptions, so no caller needs json.hpp to catch them.
        throw DocumentError(error.what());
    }
}

Result read_document_file(const std::string& path)
{
    std::ifstream     file(std::filesystem::u8path(path), std::ios::binary);
    const std::string text {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    if (!file.is_open() || file.bad())
        throw DocumentError("cannot read " + path);
    try {
        return read_document(text);
    } catch (const DocumentError& error) {
        throw DocumentError(path + ": " + error.what());
    }
}

}} // namespace Slic3r::Bench
