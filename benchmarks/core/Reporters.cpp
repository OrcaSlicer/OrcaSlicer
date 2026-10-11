#include "core/Reporters.hpp"

#include "core/Compare.hpp"
#include "core/Document.hpp"
#include "core/Result.hpp"
#include "core/RunEvents.hpp"
#include "core/Summary.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace Slic3r { namespace Bench {

namespace {

constexpr std::size_t line_width  = 80;
constexpr std::size_t stage_width = 29;
constexpr std::size_t time_width  = 8;
constexpr std::size_t cv_width    = 7;
constexpr std::size_t share_width = 7;
constexpr std::size_t cpu_width   = 8;
constexpr std::size_t peak_width  = 10;

double in_seconds(Clock::duration duration) { return std::chrono::duration<double>(duration).count(); }

std::string fixed(double value, int decimals)
{
    char text[64];
    std::snprintf(text, sizeof(text), "%.*f", decimals, value);
    return text;
}

// One decimal, where a nonzero figure too small to print as 0.1 prints as <0.1, never as a zero.
std::string figure(double value) { return value > 0 && value < 0.05 ? "<0.1" : fixed(value, 1); }

std::string milliseconds(Millis time) { return figure(time.count()); }

std::string percent(double ratio) { return fixed(100 * ratio, 1) + "%"; }

std::string mebibytes(std::uint64_t bytes) { return std::to_string(std::llround(double(bytes) / (1024.0 * 1024.0))); }

// 606972 as 606,972.
std::string grouped(std::uint64_t value)
{
    std::string digits = std::to_string(value);
    for (std::size_t at = digits.size(); at > 3; at -= 3)
        digits.insert(at - 3, ",");
    return digits;
}

// The first eight of the sixteen hex digits the document writes.
std::string short_hash(std::uint64_t hash)
{
    char text[9];
    std::snprintf(text, sizeof(text), "%08llx", static_cast<unsigned long long>(hash >> 32));
    return text;
}

// How a console view marks text beyond its words, which carry the meaning without it.
enum class Paint { Plain, Worse, Better, Alarm, Strong, Faint };

// The text wrapped in the paint's escape sequence when coloring, where blank text stays as it is.
std::string painted(const std::string& text, Paint paint, bool color)
{
    if (!color || text.find_first_not_of(' ') == std::string::npos)
        return text;
    switch (paint) {
    case Paint::Plain: return text;
    case Paint::Worse: return "\x1b[1;38;5;166m" + text + "\x1b[0m";
    case Paint::Better: return "\x1b[1;38;5;32m" + text + "\x1b[0m";
    case Paint::Alarm: return "\x1b[7m" + text + "\x1b[0m";
    case Paint::Strong: return "\x1b[1m" + text + "\x1b[0m";
    case Paint::Faint: return "\x1b[2m" + text + "\x1b[0m";
    }
    return text;
}

// The columns the text takes on screen, without its escape sequences.
std::size_t shown_width(const std::string& text)
{
    std::size_t width = 0;
    for (std::size_t at = 0; at < text.size(); ++at) {
        if (text[at] == '\x1b')
            at = std::min(text.find('m', at), text.size());
        else
            ++width;
    }
    return width;
}

std::string padded(const std::string& text, std::size_t width)
{
    const std::size_t shown = shown_width(text);
    return shown < width ? text + std::string(width - shown, ' ') : text;
}

// The text flush with the column's right edge, or one space before a text too wide for that, so it never
// runs into the column before.
std::string right(const std::string& text, std::size_t width)
{
    const std::size_t shown = shown_width(text);
    return shown < width ? std::string(width - shown, ' ') + text : " " + text;
}

// A name with a text flush with the view's right edge, or two spaces after a name too long for that.
std::string name_line(const std::string& name, const std::string& text, std::size_t width = line_width)
{
    const std::size_t shown = shown_width(text);
    const std::size_t room  = shown < width ? width - shown : 0;
    return padded(name, std::max(room, shown_width(name) + 2)) + text + "\n";
}

// The name of the row that folded stages add up to, such as "other (3 stages, 1 not run)".
std::string other_name(std::size_t stages, std::size_t not_run)
{
    std::string name = "other (" + std::to_string(stages) + (stages == 1 ? " stage" : " stages");
    if (not_run > 0)
        name += ", " + std::to_string(not_run) + " not run";
    return name + ")";
}

// The column naming a row, with one space after a name too long for it.
std::string name_cell(const std::string& name, std::size_t width = stage_width)
{
    return "  " + (shown_width(name) <= width ? padded(name, width) : name + " ");
}

std::string label(const Properties& properties, const char* key)
{
    const auto found = properties.find(key);
    return found == properties.end() ? "-" : found->second;
}

// Whether the last optimization level in the flags, the one the compiler applies, turns optimization
// off, as OrcaSlicer's RelWithDebInfo does, or the flags name no level, so the compiler's default of
// none applies, as in a GCC or Clang Debug build. Empty flags, from a build that recorded none, are not marked.
bool unoptimized(const std::string& flags)
{
    constexpr std::string_view levels[] = {"/Od", "/O1", "/O2", "/Ox", "-O0", "-O", "-O1", "-O2", "-O3", "-Os", "-Oz", "-Ofast", "-Og"};
    std::istringstream         words(flags);
    std::string                word, level;
    bool                       any = false;
    while (words >> word) {
        any = true;
        if (std::find(std::begin(levels), std::end(levels), word) != std::end(levels))
            level = word;
    }
    return any && (level.empty() || level == "/Od" || level == "-O0");
}

// Whether the run's measurement lists the metric among those it collects.
bool collects(const Properties& measurement, std::string_view metric)
{
    std::istringstream metrics(label(measurement, MeasurementKey::metrics));
    std::string        name;
    while (std::getline(metrics, name, ','))
        if (name == metric)
            return true;
    return false;
}

class ConsoleReporter : public Reporter
{
public:
    ConsoleReporter(std::ostream& out, const ReportOptions& options) : m_out(out), m_options(options) {}

    void started(const Result& header) override
    {
        m_header                       = header;
        m_marks                        = {};
        const BuildIdentity&   build   = header.build;
        const MachineIdentity& machine = header.machine;
        m_out << paint("orca_bench", Paint::Strong) << "  " << label(header.measurement, MeasurementKey::policy)
              << "  threads=" << label(header.measurement, MeasurementKey::threads)
              << "  " << build.compiler << " " << build.compiler_version
              << "  " << build.config << (unoptimized(build.flags) ? " " + paint("(unoptimized)", Paint::Worse) : "")
              << "  " << build.revision << (build.dirty ? " dirty" : "") << "\n"
              << machine.host << "  " << machine.os << "  " << machine.cpu << "  " << std::to_string(machine.logical_cores) << " cores\n"
              << std::flush;
    }

    void workload(const WorkloadResult& workload) override
    {
        m_out << "\n";
        if (workload.outcome == Outcome::Ran)
            write_ran(workload);
        else
            m_out << name_line(paint(workload.name, Paint::Strong),
                               workload.outcome == Outcome::Skipped ? paint("SKIPPED", Paint::Strong) : paint("FAILED", Paint::Alarm))
                  << "  " << workload.reason << "\n";
        m_out << std::flush;
    }

    void finished(const Result& result) override
    {
        const std::string bar = fixed(100 * significance_bar, 0) + "%";
        std::string       legends;
        if (m_marks.noisy)
            legends += paint("~ CV above the " + bar + " significance bar, so a change that size there is noise", Paint::Faint) + "\n";
        if (m_marks.shared)
            legends += paint("* readings shared with stages that ran at the same time", Paint::Faint) + "\n";
        if (m_marks.unfinished)
            legends += paint("!", Paint::Worse) +
                       paint(" started at least once without finishing, so some of its time is in unaccounted", Paint::Faint) + "\n";
        if (m_marks.floored)
            legends += paint("CPU left out where " + fixed(Millis(recorded_cpu_time_step(m_header.machine)).count(), 1) +
                                 " ms steps in CPU time could skew it past " + fixed(100 * cpu_error_limit, 0) + "%",
                             Paint::Faint) +
                       "\n";
        if (!legends.empty())
            m_out << "\n" << legends;
        m_out << "\n"
              << count_of(result, Outcome::Ran) << " ran, " << count_of(result, Outcome::Skipped) << " skipped, "
              << count_of(result, Outcome::Failed) << " failed   " << fixed(in_seconds(result.suite.duration), 1) << "s\n"
              << std::flush;
    }

private:
    std::string paint(const std::string& text, Paint as) const { return painted(text, as, m_options.color); }

    void write_ran(const WorkloadResult& workload)
    {
        std::vector<std::string> footer;
        if (workload.iterations.empty()) {
            m_out << paint(workload.name, Paint::Strong) << "\n";
        } else {
            const std::size_t count   = workload.iterations.size();
            const std::string warmup  = label(m_header.measurement, MeasurementKey::warmup);
            std::string       passes  = std::to_string(count) + (count == 1 ? " iteration" : " iterations");
            if (warmup != "-" && warmup != "0")
                passes = warmup + " warmup + " + passes;
            m_out << name_line(paint(workload.name, Paint::Strong), passes);
            WorkloadSummary summary = summarize(workload, m_header, m_options.verbose);
            write_table(summary);
            m_out << "\n";
            if (summary.peak_rss_bytes)
                footer.push_back("peak RSS " + mebibytes(*summary.peak_rss_bytes) + " MiB");
            if (summary.cpu)
                footer.push_back("CPU " + fixed(*summary.cpu, 1) + "x");
            else if (summary.cpu_below_floor)
                footer.push_back("CPU -");
            m_marks.floored = m_marks.floored || summary.cpu_below_floor;
        }
        if (workload.work) {
            footer.push_back("moves " + grouped(workload.work->moves));
            footer.push_back("layers " + grouped(workload.work->layers));
        }
        if (workload.output_hash)
            footer.push_back("hash " + short_hash(*workload.output_hash));
        else if (collects(m_header.measurement, "hash"))
            footer.push_back("hash none");
        std::string line;
        for (const std::string& part : footer)
            line += (line.empty() ? "  " : "   ") + part;
        if (!line.empty())
            m_out << line << "\n";
    }

    void write_table(WorkloadSummary& summary)
    {
        std::vector<StageRow>& rows = summary.rows;
        if (m_options.sort_by == SortBy::Start)
            std::sort(rows.begin(), rows.end(), [](const StageRow& a, const StageRow& b) {
                return std::tie(a.first_start, a.stage, a.scope) < std::tie(b.first_start, b.stage, b.scope);
            });
        else
            std::sort(rows.begin(), rows.end(), [](const StageRow& a, const StageRow& b) {
                return a.mean != b.mean ? a.mean > b.mean : std::tie(a.stage, a.scope) < std::tie(b.stage, b.scope);
            });
        const std::optional<OtherRow> other = m_options.verbose ? std::nullopt : collapse(rows, m_options.collapse_below);

        m_out << "  " << std::string(stage_width, ' ')
              << paint(right("mean", time_width) + right("min", time_width) + right("CV", cv_width) + " " + right("share", share_width) +
                           right("CPU", cpu_width) + right("peak MiB", peak_width),
                       Paint::Faint)
              << "\n";
        for (const StageRow& row : rows)
            m_out << row_line(row);
        if (other) {
            m_out << name_cell(other_name(other->stages, other->not_run)) << right(milliseconds(other->mean), time_width)
                  << right("-", time_width) << right("-", cv_width) << " " << right(percent(other->share), share_width) << "\n";
        }
        m_out << "  " << paint(std::string(line_width - 2, '-'), Paint::Faint) << "\n"
              << name_cell("summed work") << right(milliseconds(summary.summed_work), time_width) << "\n"
              << name_cell("wall envelope") << right(milliseconds(summary.wall), time_width) << "\n"
              << name_cell("unaccounted") << right(milliseconds(summary.unaccounted), time_width);
        if (summary.wall.count() > 0)
            m_out << right(percent(summary.unaccounted / summary.wall), time_width + cv_width + 1 + share_width);
        m_out << "\n";
    }

    std::string row_line(const StageRow& row)
    {
        std::string name = row.scope.empty() ? row.stage : row.stage + " " + row.scope;
        if (row.shared)
            name += paint("*", Paint::Faint);
        const bool partly_unfinished = row.unfinished && row.state != StageState::Unfinished;
        if (partly_unfinished)
            name += paint("!", Paint::Worse);
        m_marks.shared     = m_marks.shared || row.shared;
        m_marks.unfinished = m_marks.unfinished || partly_unfinished;
        switch (row.state) {
        case StageState::Unfinished: return name_cell(name) + right(paint("never finished", Paint::Worse), time_width) + "\n";
        case StageState::NotRun: return name_cell(name) + right("not run", time_width) + "\n";
        case StageState::Instant: return name_cell(name) + right("instant", time_width) + "\n";
        case StageState::Ran: break;
        }
        const bool noisy = row.cv && *row.cv > significance_bar;
        m_marks.noisy    = m_marks.noisy || noisy;
        m_marks.floored  = m_marks.floored || row.cpu_below_floor;
        return name_cell(name) + right(milliseconds(row.mean), time_width) + right(milliseconds(row.min), time_width) +
               right(row.cv ? percent(*row.cv) : "-", cv_width) + (noisy ? paint("~", Paint::Faint) : " ") +
               right(percent(row.share), share_width) + right(row.cpu ? fixed(*row.cpu, 1) + "x" : "-", cpu_width) +
               right(row.peak_rss_bytes ? mebibytes(*row.peak_rss_bytes) : "-", peak_width) + "\n";
    }

    // The marks this run's rows used, each explained by a legend at the end.
    struct Marks
    {
        bool noisy      = false;
        bool shared     = false;
        bool unfinished = false;
        bool floored    = false;
    };

    std::ostream& m_out;
    ReportOptions m_options;
    Result        m_header;
    Marks         m_marks;
};

class JsonReporter : public Reporter
{
public:
    explicit JsonReporter(std::ostream& out) : m_out(out) {}

    void started(const Result&) override {}
    void workload(const WorkloadResult&) override {}

    // A document is valid only whole, so it waits for the end.
    void finished(const Result& result) override { m_out << write_document(result) << std::flush; }

private:
    std::ostream& m_out;
};

class NullReporter : public Reporter
{
public:
    void started(const Result&) override {}
    void workload(const WorkloadResult&) override {}
    void finished(const Result&) override {}
};

struct Named
{
    const char* name;
    std::unique_ptr<Reporter> (*make)(std::ostream& out, const ReportOptions& options);
};

// Each reporter's name and factory, which make_reporter() and reporter_names() both read, so a new
// reporter is a class and a line here.
constexpr Named reporters[] = {
    {"console", [](std::ostream& out, const ReportOptions& options) -> std::unique_ptr<Reporter> {
         return std::make_unique<ConsoleReporter>(out, options);
     }},
    {"json", [](std::ostream& out, const ReportOptions&) -> std::unique_ptr<Reporter> { return std::make_unique<JsonReporter>(out); }},
    {"null", [](std::ostream&, const ReportOptions&) -> std::unique_ptr<Reporter> { return std::make_unique<NullReporter>(); }},
};

// The compare view's column widths, which make a workload's table 120 wide.
constexpr std::size_t compare_width = 120;
constexpr std::size_t figure_width  = 8;
constexpr std::size_t verdict_width = 7;
constexpr std::size_t small_width   = 6;
constexpr std::size_t summary_width = 41;
constexpr std::size_t note_width    = 14;
constexpr std::size_t mark_width    = 18;

// The words a judged change ends with, worse first.
struct Words
{
    const char* worse;
    const char* better;
};

constexpr Words speed_words {"slower", "faster"};
constexpr Words length_words {"longer", "shorter"};
constexpr Words memory_words {"more", "less"};

// A signed figure with one decimal, whose zero prints as +0.0 whatever its sign.
std::string signed_figure(double value)
{
    std::string text = fixed(value, 1);
    if (text.find_first_not_of("-0.") == std::string::npos)
        text = fixed(0.0, 1);
    return (text[0] == '-' ? "" : "+") + text;
}

std::string signed_percent(double ratio) { return signed_figure(100 * ratio) + "%"; }

std::string mib(double bytes) { return fixed(bytes / (1024.0 * 1024.0), 1); }

// Removes the spaces that blank trailing columns leave.
std::string trimmed_end(std::string text)
{
    text.erase(text.find_last_not_of(' ') + 1);
    return text;
}

// "2026-09-30 09:14 UTC" from the time the document records.
std::string utc_minute(decltype(Suite::started_at) time)
{
    const std::string iso = to_iso8601(time);
    return iso.substr(0, 10) + " " + iso.substr(11, 5) + " UTC";
}

// A line per difference after the indent, with - for the run that lacks the key.
std::string difference_lines(const std::vector<PropertyDifference>& differences, const std::string& indent)
{
    std::string lines;
    for (const PropertyDifference& difference : differences)
        lines += indent + difference.key + "  " + difference.a.value_or("-") + " -> " + difference.b.value_or("-") + "\n";
    return lines;
}

std::string cv_text(const std::optional<double>& cv) { return cv ? percent(*cv) : "-"; }

std::string cpu_text(const std::optional<double>& cpu) { return cpu ? fixed(*cpu, 1) + "x" : "-"; }

std::string state_of(const std::optional<StageRow>& row)
{
    switch (row->state) {
    case StageState::Ran: return milliseconds(row->min);
    case StageState::Instant: return "instant";
    case StageState::NotRun: return "not run";
    case StageState::Unfinished: return "never finished";
    }
    return "";
}

// A stage that did not run measurably in both runs, by what each run did.
std::string state_text(const StagePair& pair)
{
    if (!pair.a)
        return "only in b";
    if (!pair.b)
        return "only in a";
    if (pair.a->state == pair.b->state && pair.a->state != StageState::Ran)
        return state_of(pair.a) + " in both";
    return state_of(pair.a) + " -> " + state_of(pair.b);
}

std::string work_label(const std::string& name)
{
    if (name == "gcode_bytes")
        return "G-code bytes";
    if (name == "travel_moves")
        return "travel moves";
    if (name == "extrusion_mm3")
        return "extrusion";
    return name;
}

// A count grouped in thousands, or the extrusion in cm3.
std::string work_value(const std::string& name, double value)
{
    if (name == "extrusion_mm3")
        return fixed(value / 1000, 1);
    // The cast is undefined for a value past the largest count, which a hand-edited result may hold.
    if (value >= 0 && value < double(std::numeric_limits<std::uint64_t>::max()) && value == std::floor(value))
        return grouped(static_cast<std::uint64_t>(value));
    return fixed(value, 1);
}

std::string work_text(const WorkDifference& difference)
{
    const auto  value = [&difference](const std::optional<double>& side) { return side ? work_value(difference.name, *side) : "none"; };
    std::string text  = work_label(difference.name) + " " + value(difference.a) + " -> " + value(difference.b);
    if (difference.name == "extrusion_mm3")
        text += " cm3";
    if (difference.a && difference.b && *difference.a > 0)
        text += "  " + signed_percent(*difference.b / *difference.a - 1);
    return text;
}

std::string hash_text(const std::optional<std::uint64_t>& hash) { return hash ? short_hash(*hash) : "none"; }

double largest(const StagePair& pair)
{
    double most = 0.0;
    if (pair.a)
        most = std::max(most, pair.a->min.count());
    if (pair.b)
        most = std::max(most, pair.b->min.count());
    return most;
}

Clock::duration earliest(const StagePair& pair)
{
    Clock::duration first = Clock::duration::max();
    if (pair.a)
        first = std::min(first, pair.a->first_start);
    if (pair.b)
        first = std::min(first, pair.b->first_start);
    return first;
}

// The text with each control character as a space where it is whitespace and as ? otherwise, so text
// a document carries cannot send the terminal a sequence of its own.
std::string printable(const std::string& text)
{
    std::string shown;
    for (std::size_t at = 0; at < text.size(); ++at) {
        const auto          byte = static_cast<unsigned char>(text[at]);
        const unsigned char next = at + 1 < text.size() ? static_cast<unsigned char>(text[at + 1]) : 0;
        // UTF-8 writes the C1 controls, U+0080 to U+009F, as C2 80 to C2 9F.
        const bool c1 = byte == 0xC2 && next >= 0x80 && next < 0xA0;
        if (c1 || byte < 0x20 || byte == 0x7F)
            shown += byte >= '\t' && byte <= '\r' ? ' ' : '?';
        else
            shown += text[at];
        if (c1)
            ++at;
    }
    return shown;
}

// The comparison with every text it took from the documents printable.
Comparison printable(Comparison comparison)
{
    const auto clean = [](Properties& properties) {
        Properties shown;
        for (const auto& [key, value] : properties)
            shown.emplace(printable(key), printable(value));
        properties = std::move(shown);
    };
    for (Result* run : {&comparison.a, &comparison.b}) {
        clean(run->measurement);
        clean(run->build.properties);
        clean(run->machine.properties);
        for (std::string* text : {&run->build.revision, &run->build.compiler, &run->build.compiler_version, &run->build.config,
                                  &run->build.flags, &run->machine.host, &run->machine.os, &run->machine.cpu})
            *text = printable(*text);
    }
    for (PropertyDifference& difference : comparison.measurement) {
        difference.key = printable(difference.key);
        if (difference.a)
            difference.a = printable(*difference.a);
        if (difference.b)
            difference.b = printable(*difference.b);
    }
    for (WorkloadComparison& workload : comparison.workloads) {
        workload.name         = printable(workload.name);
        workload.not_compared = printable(workload.not_compared);
        for (WorkDifference& difference : workload.work_differences)
            difference.name = printable(difference.name);
        for (StagePair& pair : workload.stages) {
            pair.stage = printable(pair.stage);
            pair.scope = printable(pair.scope);
        }
    }
    return comparison;
}

class CompareWriter
{
public:
    CompareWriter(std::ostream& out, const Comparison& comparison, const CompareView& view)
        : m_out(out), m_comparison(comparison), m_view(view)
    {}

    void write()
    {
        header();
        measured_differently();
        changed_output();
        bool        tables   = summary();
        std::size_t compared = 0;
        std::size_t skipped  = 0;
        for (const WorkloadComparison& workload : m_comparison.workloads) {
            if (!workload.not_compared.empty())
                continue;
            ++compared;
            tables = write_workload(workload) || tables;
        }
        for (const WorkloadComparison& workload : m_comparison.workloads) {
            if (workload.not_compared.empty())
                continue;
            ++skipped;
            m_out << "\n" << name_line(paint(workload.name, Paint::Strong), paint("NOT COMPARED", Paint::Strong), compare_width) << "  "
                  << workload.not_compared << "\n";
        }
        legend(tables);
        m_out << "\n"
              << compared << " compared, " << m_comparison.changed_outputs << " with changed output, " << skipped << " not compared"
              << (m_comparison.failed_only_in_b > 0 ? ", " + std::to_string(m_comparison.failed_only_in_b) + " failed only in b" : "") << "\n"
              << std::flush;
    }

private:
    void header()
    {
        const Result&     a          = m_comparison.a;
        const Result&     b          = m_comparison.b;
        const std::string iterations = label(a.measurement, MeasurementKey::iterations);
        const std::string warmup     = label(a.measurement, MeasurementKey::warmup);
        std::string       passes     = iterations + (iterations == "1" ? " iteration" : " iterations");
        if (warmup != "-" && warmup != "0")
            passes = warmup + " warmup + " + passes;
        m_out << paint("orca_bench compare", Paint::Strong) << "  " << label(a.measurement, MeasurementKey::policy)
              << "  threads=" << label(a.measurement, MeasurementKey::threads) << "  " << passes
              << "  stages " << label(a.measurement, MeasurementKey::stages) << "\n";

        const auto revision  = [](const BuildIdentity& build) { return build.revision + (build.dirty ? " dirty" : ""); };
        const auto toolchain = [this](const BuildIdentity& build) {
            return build.compiler + " " + build.compiler_version + "  " + build.config +
                   (unoptimized(build.flags) ? " " + paint("(unoptimized)", Paint::Worse) : "");
        };
        const std::size_t revisions  = std::max(revision(a.build).size(), revision(b.build).size());
        const std::size_t toolchains = std::max(shown_width(toolchain(a.build)), shown_width(toolchain(b.build)));
        // The file comes last, where a long path wraps without moving the columns before it.
        const auto run_line = [&](const char* side, const Result& run, const std::string& name) {
            m_out << side << "  " << utc_minute(run.suite.started_at) << "  " << padded(revision(run.build), revisions) << "  "
                  << padded(toolchain(run.build), toolchains) << "  " << name << "\n";
        };
        run_line("a", a, m_view.label_a);
        run_line("b", b, m_view.label_b);
        if (a.build.flags != b.build.flags)
            m_out << "   a flags  " << a.build.flags << "\n   b flags  " << b.build.flags << "\n";
        m_out << difference_lines(property_differences(a.build.properties, b.build.properties), "   ");

        const auto machine_text = [](const MachineIdentity& machine) {
            return machine.host + "  " + machine.os + "  " + machine.cpu + "  " + std::to_string(machine.logical_cores) + " cores";
        };
        if (machine_text(a.machine) == machine_text(b.machine))
            m_out << "   " << machine_text(a.machine) << "\n";
        else
            m_out << "   a  " << machine_text(a.machine) << "\n   b  " << machine_text(b.machine) << "\n";
        m_out << difference_lines(property_differences(a.machine.properties, b.machine.properties), "   ");
    }

    void measured_differently()
    {
        if (m_comparison.measurement.empty())
            return;
        m_out << "\n" << paint("MEASURED DIFFERENTLY", Paint::Alarm) << ", compared because that was allowed\n"
              << difference_lines(m_comparison.measurement, "  ");
    }

    void changed_output()
    {
        std::vector<const WorkloadComparison*> changed;
        for (const WorkloadComparison& workload : m_comparison.workloads)
            if (workload.output_changed)
                changed.push_back(&workload);
        if (changed.empty())
            return;
        m_out << "\n" << paint("OUTPUT CHANGED", Paint::Alarm) << " in " << changed.size()
              << (changed.size() == 1 ? " workload, so its times below measure different work\n"
                                      : " workloads, so their times below measure different work\n");
        for (const WorkloadComparison* workload : changed) {
            std::vector<std::string> items;
            if (workload->hash_a != workload->hash_b)
                items.push_back("hash " + hash_text(workload->hash_a) + " -> " + hash_text(workload->hash_b));
            for (const WorkDifference& difference : workload->work_differences)
                items.push_back(work_text(difference));
            if (workload->work_in_both && workload->work_differences.empty())
                items.push_back("every work stat the same, so values changed rather than the work");
            for (std::size_t i = 0; i < items.size(); ++i)
                m_out << "  " << padded(i == 0 ? workload->name : "", summary_width) << " " << items[i] << "\n";
        }
    }

    // A row per workload compared with a wall time, and whether there was one.
    bool summary()
    {
        std::vector<const WorkloadComparison*> rows;
        for (const WorkloadComparison& workload : m_comparison.workloads)
            if (workload.not_compared.empty() && workload.wall)
                rows.push_back(&workload);
        if (rows.empty())
            return false;
        const std::string heading = both("wall a", "wall b", figure_width) + right("change", figure_width) +
                                    std::string(2 + note_width, ' ') + right("diff", figure_width) + both("CV a", "CV b", small_width) +
                                    both("MiB a", "MiB b", figure_width);
        m_out << "\n" << trimmed_end(name_cell("", summary_width) + paint(heading, Paint::Faint)) << "\n";
        for (const WorkloadComparison* workload : rows) {
            // A workload whose output changed carries that note in place of a verdict.
            const Change&                wall    = *workload->wall;
            const std::optional<Change>& peak    = workload->peak_rss_bytes;
            const bool                   changed = workload->output_changed;
            const Paint                  row     = changed ? Paint::Plain : row_paint(wall);
            const Paint                  verdict = changed ? Paint::Plain : verdict_paint(wall);
            const std::string note = changed ? paint("output changed", Paint::Alarm) : paint(verdict_word(wall, speed_words), verdict);
            m_out << trimmed_end(name_cell(paint(workload->name, row), summary_width) +
                                 both(figure(wall.a.min), figure(wall.b.min), figure_width, row) +
                                 right(paint(signed_percent(wall.change), verdict), figure_width) + paint(noise_mark(wall), Paint::Faint) +
                                 " " + padded(note, note_width) + right(paint(signed_figure(wall.b.min - wall.a.min), row), figure_width) +
                                 both(cv_text(wall.a.cv), cv_text(wall.b.cv), small_width, row) +
                                 both(peak ? mib(peak->a.min) : "-", peak ? mib(peak->b.min) : "-", figure_width, row))
                  << "\n";
        }
        if (m_comparison.wall_geometric_mean) {
            const std::size_t count = m_comparison.geometric_mean_of;
            const std::string what  = "geometric mean over " + std::to_string(count) + (count == 1 ? " workload" : " workloads") +
                                     " with unchanged output";
            m_out << "  " << padded(what, summary_width + 2 * figure_width)
                  << right(signed_percent(*m_comparison.wall_geometric_mean), figure_width) << "\n";
        }
        return true;
    }

    // The workload's table, and whether it had one.
    bool write_workload(const WorkloadComparison& workload)
    {
        const std::string output =
            workload.output_changed ? paint("OUTPUT CHANGED", Paint::Alarm) : paint("output unchanged", Paint::Faint);
        m_out << "\n" << name_line(paint(workload.name, Paint::Strong), output, compare_width);
        if (!workload.wall && workload.stages.empty()) {
            m_out << "  no timed passes\n";
            return false;
        }
        std::vector<StagePair> pairs = workload.stages;
        if (m_view.sort_by == SortBy::Start)
            std::sort(pairs.begin(), pairs.end(), [](const StagePair& x, const StagePair& y) {
                return std::make_tuple(earliest(x), x.stage, x.scope) < std::make_tuple(earliest(y), y.stage, y.scope);
            });
        else
            std::sort(pairs.begin(), pairs.end(), [](const StagePair& x, const StagePair& y) {
                return largest(x) != largest(y) ? largest(x) > largest(y) : std::tie(x.stage, x.scope) < std::tie(y.stage, y.scope);
            });
        const std::optional<OtherPair> other = m_comparison.verbose ? std::nullopt : collapse(pairs, m_view.collapse_below);

        const std::string heading = both("min a", "min b", figure_width) + right("change", figure_width) +
                                    std::string(2 + verdict_width, ' ') + right("diff", figure_width) + right("by mean", figure_width) +
                                    both("CV a", "CV b", small_width) + both("CPU a", "CPU b", small_width) +
                                    both("MiB a", "MiB b", figure_width);
        m_out << trimmed_end(name_cell("") + paint(heading, Paint::Faint)) << "\n";
        const Words& time_words = workload.output_changed ? length_words : speed_words;
        for (const StagePair& pair : pairs)
            m_out << pair_row(pair, time_words);
        if (other) {
            std::string row = name_cell(other_name(other->stages, other->not_run)) +
                              both(milliseconds(other->a), milliseconds(other->b), figure_width);
            if (other->a.count() > 0)
                row += right(signed_percent(other->b / other->a - 1), figure_width) + std::string(2 + verdict_width, ' ') +
                       right(signed_figure((other->b - other->a).count()), figure_width);
            m_out << trimmed_end(row) << "\n";
        }
        m_out << "  " << paint(std::string(compare_width - 2, '-'), Paint::Faint) << "\n";
        figure_row("summed work", workload.summed_work, time_words);
        figure_row("unaccounted", workload.unaccounted, time_words);
        if (workload.wall)
            m_used.floored = m_used.floored || workload.cpu_below_floor_a || workload.cpu_below_floor_b;
        figure_row("wall", workload.wall, time_words, 1.0, cpu_text(workload.cpu_a), cpu_text(workload.cpu_b));
        if (workload.output_changed) {
            figure_row("per 1M moves", workload.per_million_moves, speed_words);
            figure_row("per layer", workload.per_layer, speed_words);
            figure_row("per cm3", workload.per_cm3, speed_words);
        }
        figure_row("peak MiB", workload.peak_rss_bytes, memory_words, 1.0 / (1024.0 * 1024.0));
        return true;
    }

    std::string pair_row(const StagePair& pair, const Words& words)
    {
        const std::string name = pair.scope.empty() ? pair.stage : pair.stage + " " + pair.scope;
        if (!pair.a || !pair.b || pair.a->state != StageState::Ran || pair.b->state != StageState::Ran) {
            const auto  unfinished = [](const std::optional<StageRow>& row) { return row && row->state == StageState::Unfinished; };
            const Paint named      = pair.significant ? Paint::Strong : Paint::Plain;
            const Paint state      = unfinished(pair.a) || unfinished(pair.b) ? Paint::Worse : named;
            return name_cell(paint(name, named)) + paint(state_text(pair), state) + "\n";
        }
        const Paint row    = pair.time ? row_paint(*pair.time) : Paint::Plain;
        const bool  shared = pair.a->shared || pair.b->shared;
        const bool  partly = pair.a->unfinished || pair.b->unfinished;
        m_used.shared      = m_used.shared || shared;
        m_used.unfinished  = m_used.unfinished || partly;
        const std::string marked = paint(name, row) + (shared ? paint("*", Paint::Faint) : "") + (partly ? paint("!", Paint::Worse) : "");
        // A minimum of zero in a gives no change, so the row shows only the minimums.
        if (!pair.time)
            return trimmed_end(name_cell(marked) + both(milliseconds(pair.a->min), milliseconds(pair.b->min), figure_width)) + "\n";
        const std::string cells = change_cells(*pair.time, words, 1.0) + both(stage_cpu(*pair.a), stage_cpu(*pair.b), small_width, row) +
                                  both(stage_peak(*pair.a), stage_peak(*pair.b), figure_width, row);
        return trimmed_end(name_cell(marked) + cells) + "\n";
    }

    void figure_row(const std::string& name, const std::optional<Change>& change, const Words& words, double scale = 1.0,
                    const std::string& cpu_a = "", const std::string& cpu_b = "")
    {
        if (!change)
            return;
        const Paint row = row_paint(*change);
        m_out << trimmed_end(name_cell(paint(name, row)) + change_cells(*change, words, scale) + both(cpu_a, cpu_b, small_width, row))
              << "\n";
    }

    // Both runs' figures, the change with its verdict, the difference and the mean's change, and each
    // run's CV.
    std::string change_cells(const Change& change, const Words& words, double scale)
    {
        const Paint       row        = row_paint(change);
        const Paint       verdict    = verdict_paint(change);
        const std::string word       = paint(verdict_word(change, words), verdict);
        const double      difference = (change.b.min - change.a.min) * scale;
        return both(figure(change.a.min * scale), figure(change.b.min * scale), figure_width, row) +
               right(paint(signed_percent(change.change), verdict), figure_width) + paint(noise_mark(change), Paint::Faint) + " " +
               padded(word, verdict_width) + right(paint(signed_figure(difference), row), figure_width) +
               right(paint(signed_percent(change.mean_change), row), figure_width) +
               both(cv_text(change.a.cv), cv_text(change.b.cv), small_width, row);
    }

    std::string paint(const std::string& text, Paint as) const { return painted(text, as, m_view.color); }

    // Each run's cell in the paint, right-aligned in `width`.
    std::string both(const std::string& a, const std::string& b, std::size_t width, Paint as = Paint::Plain) const
    {
        return right(paint(a, as), width) + right(paint(b, as), width);
    }

    // A judged change's color, and faint for a change below the bar or inside a run's CV.
    static Paint verdict_paint(const Change& change)
    {
        switch (change.verdict) {
        case Verdict::Worse: return Paint::Worse;
        case Verdict::Better: return Paint::Better;
        case Verdict::Unchanged:
        case Verdict::Noise: return Paint::Faint;
        case Verdict::Unjudged: return Paint::Plain;
        }
        return Paint::Plain;
    }

    // Bold for the rest of a row whose change was judged.
    static Paint row_paint(const Change& change)
    {
        return change.verdict == Verdict::Worse || change.verdict == Verdict::Better ? Paint::Strong : Paint::Plain;
    }

    std::string verdict_word(const Change& change, const Words& words)
    {
        m_used.unjudged = m_used.unjudged || change.verdict == Verdict::Unjudged;
        if (change.verdict != Verdict::Worse && change.verdict != Verdict::Better)
            return "";
        if (&words == &speed_words)
            m_used.speed = true;
        else if (&words == &length_words)
            m_used.length = true;
        else
            m_used.memory = true;
        return change.verdict == Verdict::Worse ? words.worse : words.better;
    }

    std::string noise_mark(const Change& change)
    {
        if (change.verdict != Verdict::Noise)
            return " ";
        m_used.noise = true;
        return "~";
    }

    std::string stage_cpu(const StageRow& row)
    {
        m_used.floored = m_used.floored || row.cpu_below_floor;
        return cpu_text(row.cpu);
    }

    static std::string stage_peak(const StageRow& row) { return row.peak_rss_bytes ? mib(double(*row.peak_rss_bytes)) : "-"; }

    void legend(bool tables)
    {
        const std::string bar   = fixed(100 * significance_bar, 0) + "%";
        const auto        entry = [this](const std::string& marks, const std::string& meaning) {
            return padded(marks, mark_width) + paint(meaning, Paint::Faint) + "\n";
        };
        const auto words = [this](const Words& pair) { return paint(pair.worse, Paint::Worse) + ", " + paint(pair.better, Paint::Better); };
        std::string lines;
        if (tables)
            lines += paint("min a, min b, wall a and wall b are minimums over the iterations, in ms unless the row says otherwise",
                           Paint::Faint) +
                     "\n";
        if (m_used.speed)
            lines += entry(words(speed_words), "the minimum moved at least " + bar + " and more than either run's CV");
        if (m_used.length)
            lines += entry(words(length_words), "the same, where the output changed, so the work differs too");
        if (m_used.memory)
            lines += entry(words(memory_words), "the same test, for peak memory");
        if (m_used.noise)
            lines += entry(paint("~", Paint::Faint), "at least " + bar + " but within a run's CV, so it may be noise");
        if (m_used.shared)
            lines += entry(paint("*", Paint::Faint), "readings shared with stages that ran at the same time");
        if (m_used.unfinished)
            lines += entry(paint("!", Paint::Worse), "started at least once without finishing, so some of its time is in unaccounted");
        if (m_used.floored)
            lines += paint("CPU left out where a run's steps in CPU time could skew it past " + fixed(100 * cpu_error_limit, 0) + "%",
                           Paint::Faint) +
                     "\n";
        if (m_used.unjudged)
            lines += paint("a change is not judged where a run has no CV, as with one iteration or a mean of zero", Paint::Faint) + "\n";
        if (!lines.empty())
            m_out << "\n" << lines;
    }

    // The marks and words the view used, each explained by a legend line.
    struct Used
    {
        bool speed      = false;
        bool length     = false;
        bool memory     = false;
        bool noise      = false;
        bool shared     = false;
        bool unfinished = false;
        bool floored    = false;
        bool unjudged   = false;
    };

    std::ostream&      m_out;
    const Comparison&  m_comparison;
    const CompareView& m_view;
    Used               m_used;
};

} // namespace

std::vector<std::string> reporter_names()
{
    std::vector<std::string> names;
    for (const Named& reporter : reporters)
        names.emplace_back(reporter.name);
    return names;
}

std::unique_ptr<Reporter> make_reporter(std::string_view name, std::ostream& out, const ReportOptions& options)
{
    for (const Named& reporter : reporters)
        if (name == reporter.name)
            return reporter.make(out, options);
    std::string names;
    for (const Named& reporter : reporters)
        names += (names.empty() ? "" : ", ") + std::string(reporter.name);
    throw std::invalid_argument("no reporter is called '" + std::string(name) + "'; the reporters are " + names);
}

void report(Reporter& reporter, const Result& result)
{
    reporter.started(Result {result.suite, result.measurement, result.build, result.machine, {}});
    for (const WorkloadResult& workload : result.workloads)
        reporter.workload(workload);
    reporter.finished(result);
}

Progress::Progress(std::ostream& out, bool terminal, std::function<Clock::time_point()> now)
    : m_out(out), m_terminal(terminal), m_now(std::move(now))
{}

void Progress::workload_started(std::size_t index, std::size_t count, const std::string& name)
{
    m_workload         = "[" + std::to_string(index) + "/" + std::to_string(count) + "] " + name;
    m_workload_started = m_now();
    if (m_terminal)
        draw("");
    else
        m_out << m_workload << "\n" << std::flush;
}

void Progress::pass_done(const PassDone& pass)
{
    if (!m_terminal)
        return;
    std::string line;
    if (pass.pass <= pass.warmups)
        line += "  warmup " + std::to_string(pass.pass) + "/" + std::to_string(pass.warmups);
    else
        line += "  timed " + std::to_string(pass.pass - pass.warmups) + "/" + std::to_string(pass.passes - pass.warmups);
    line += "  last " + fixed(in_seconds(pass.wall), 2) + " s";
    // The passes left at the pace of those so far, setup included.
    const double left = in_seconds(m_now() - m_workload_started) / double(pass.pass) * double(pass.passes - pass.pass);
    if (left >= 1)
        line += "  about " + std::to_string(std::llround(left)) + " s left";
    draw(line);
}

void Progress::clear()
{
    if (m_drawn == 0)
        return;
    m_out << '\r' << std::string(m_drawn, ' ') << '\r' << std::flush;
    m_drawn = 0;
}

void Progress::draw(const std::string& status)
{
    // '\r' cannot redraw the rows of a line that wraps, so a line as wide as the view shortens the workload's name.
    std::string line = m_workload + status;
    if (line.size() >= line_width && status.size() + 4 < line_width)
        line = m_workload.substr(0, line_width - 4 - status.size()) + "..." + status;
    m_out << '\r' << line;
    if (line.size() < m_drawn)
        m_out << std::string(m_drawn - line.size(), ' ');
    m_out << std::flush;
    m_drawn = line.size();
}

RunEvents report_events(Reporter& reporter, Progress* progress)
{
    RunEvents events;
    events.started = [&reporter](const Result& header) { reporter.started(header); };
    if (progress) {
        events.workload_started = [progress](std::size_t index, std::size_t count, const std::string& name) {
            progress->workload_started(index, count, name);
        };
        events.pass_done = [progress](const PassDone& pass) { progress->pass_done(pass); };
    }
    events.workload_done = [&reporter, progress](const WorkloadResult& workload) {
        if (progress)
            progress->clear();
        reporter.workload(workload);
    };
    events.finished = [&reporter](const Result& result) { reporter.finished(result); };
    return events;
}

void write_comparison(std::ostream& out, const Comparison& comparison, const CompareView& view)
{
    CompareView shown = view;
    shown.label_a     = printable(view.label_a);
    shown.label_b     = printable(view.label_b);
    CompareWriter(out, printable(comparison), shown).write();
}

std::string printable_lines(const std::string& message)
{
    std::string shown;
    for (std::size_t from = 0; from <= message.size();) {
        const std::size_t end = std::min(message.find('\n', from), message.size());
        shown += printable(message.substr(from, end - from)) + (end < message.size() ? "\n" : "");
        from = end + 1;
    }
    return shown;
}

}} // namespace Slic3r::Bench
