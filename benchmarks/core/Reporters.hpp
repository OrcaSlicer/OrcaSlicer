#pragma once

#include "core/Compare.hpp"
#include "core/Result.hpp"
#include "core/RunEvents.hpp"
#include "core/Summary.hpp"

#include <cstddef>
#include <functional>
#include <iosfwd>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Slic3r { namespace Bench {

enum class SortBy { Time, Start };

// How the console lays out a run, which json and null ignore.
struct ReportOptions
{
    double collapse_below = collapse_share;
    bool   verbose        = false;
    SortBy sort_by        = SortBy::Time;
    // Colors what needs attention with escape sequences, which only a terminal shows.
    bool   color          = false;
};

// Turns a run into what orca_bench prints, fed the identities before the first workload, each
// workload as it finishes and the whole result at the end.
class Reporter
{
public:
    virtual ~Reporter() = default;

    virtual void started(const Result& header)            = 0;
    virtual void workload(const WorkloadResult& workload) = 0;
    virtual void finished(const Result& result)           = 0;
};

std::vector<std::string> reporter_names();

// Throws std::invalid_argument for a name reporter_names() does not list.
std::unique_ptr<Reporter> make_reporter(std::string_view name, std::ostream& out, const ReportOptions& options);

// Feeds a finished result through a reporter the way a run does.
void report(Reporter& reporter, const Result& result);

// The line that shows where a run is, redrawn after each pass in a terminal and printed once per
// workload in a log.
class Progress
{
public:
    Progress(std::ostream& out, bool terminal, std::function<Clock::time_point()> now = Clock::now);

    void workload_started(std::size_t index, std::size_t count, const std::string& name);
    void pass_done(const PassDone& pass);

    // Erases a redrawn line, so the report can print where it was.
    void clear();

private:
    // Redraws the workload followed by the status.
    void draw(const std::string& status);

    std::ostream&                      m_out;
    bool                               m_terminal;
    std::function<Clock::time_point()> m_now;
    std::string                        m_workload;
    Clock::time_point                  m_workload_started {};
    std::size_t                        m_drawn = 0;
};

// The events that feed a reporter and, when given one, a progress line it clears before each
// workload is reported.
RunEvents report_events(Reporter& reporter, Progress* progress);

// How write_comparison() lays out a comparison, which the command line fills.
struct CompareView
{
    // What the header calls each run, such as the file it was read from.
    std::string label_a        = "a";
    std::string label_b        = "b";
    double      collapse_below = collapse_share;
    SortBy      sort_by        = SortBy::Time;
    // Colors what needs attention with escape sequences, which only a terminal shows.
    bool        color          = false;
};

// Writes changed output first, then a summary of the walls, then a table per workload and the
// workloads not compared, each part as wide as its columns.
void write_comparison(std::ostream& out, const Comparison& comparison, const CompareView& view);

// The message with each control character but a line break as a space where it is whitespace and as ?
// otherwise, so an error that quotes a document cannot send the terminal a sequence of its own.
std::string printable_lines(const std::string& message);

}} // namespace Slic3r::Bench
