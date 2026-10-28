#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/Mcp/McpOperations.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r::GUI::Mcp;
using namespace std::chrono_literals;

namespace {

// A run that started for request `started_for` (5, the test ticket's own, unless given).
SliceRunRecord run(uint64_t seq, uint64_t revision, uint64_t request, bool cancelled = false, bool finished = true,
                   uint64_t started_for = 5, int plate = 0)
{
    SliceRunRecord r;
    r.seq                   = seq;
    r.finished              = finished;
    r.cancelled             = cancelled;
    r.ok                    = finished && !cancelled;
    r.revision_at_finish    = revision;
    r.request_seq_at_finish = request;
    r.request_seq_at_start  = started_for;
    r.plate                 = plate;
    return r;
}

SliceRunRecord plate_run(uint64_t seq, int plate, std::string error = {}, bool cancelled = false)
{
    SliceRunRecord r = run(seq, 1, 5, cancelled, true, 5, plate);
    r.ok             = r.ok && error.empty();
    r.error          = std::move(error);
    return r;
}

std::vector<PlateOutcome> outcomes(const std::vector<PlateResult>& results)
{
    std::vector<PlateOutcome> out;
    for (const PlateResult& r : results)
        out.push_back(r.outcome);
    return out;
}

SliceTicketState ticket(uint64_t request_mine, uint64_t run_before, uint64_t revision)
{
    SliceTicketState t;
    t.request_seq_mine  = request_mine;
    t.run_seq_before    = run_before;
    t.revision_at_start = revision;
    t.seen_running      = true;
    t.started           = std::chrono::steady_clock::now() - 10s;
    return t;
}

SliceObservation observe(uint64_t request, uint64_t revision, std::vector<SliceRunRecord> runs, bool slicing = false)
{
    SliceObservation o;
    o.slicing_any      = slicing;
    o.request_seq      = request;
    o.current_revision = revision;
    o.runs_since_start = std::move(runs);
    return o;
}

} // namespace

TEST_CASE("classify_plates reports ok, error, empty, skipped and cancelled", "[Mcp]")
{
    const std::vector<PlateObservation> plates{{0, true, false}, {1, false, false}, {2, false, true}, {3, false, false}};
    const auto results = classify_plates({0, 1, 2, 3, 7}, plates, {plate_run(11, 0), plate_run(12, 1, "boom")});
    CHECK(outcomes(results) == std::vector<PlateOutcome>{PlateOutcome::Ok, PlateOutcome::Error, PlateOutcome::Empty,
                                                         PlateOutcome::Skipped, PlateOutcome::Skipped});
    CHECK(results[1].error == "boom");
    CHECK(outcomes(classify_plates({3}, plates, {plate_run(11, 0, {}, true)})) == std::vector<PlateOutcome>{PlateOutcome::Cancelled});
    // Finished without a result and without saying why.
    SliceRunRecord no_result = plate_run(11, 1);
    no_result.ok             = false;
    const auto failed        = classify_plates({1}, plates, {no_result});
    CHECK(failed[0].outcome == PlateOutcome::Error);
    CHECK(failed[0].error == "the slice finished without a result");
}

TEST_CASE("a plate whose result a later edit replaced still reports its slice ok", "[Mcp]")
{
    CHECK(outcomes(classify_plates({0}, {{0, false, false}}, {plate_run(11, 0)})) == std::vector<PlateOutcome>{PlateOutcome::Ok});
}

TEST_CASE("Slice All stopping at a failing plate leaves the later plates skipped", "[Mcp]")
{
    const std::vector<PlateObservation> plates{{0, true, false}, {1, false, false}, {2, false, false}};
    const auto results = classify_plates({0, 1, 2}, plates, {plate_run(11, 0), plate_run(12, 1, "Floating regions")});
    CHECK(outcomes(results) == std::vector<PlateOutcome>{PlateOutcome::Ok, PlateOutcome::Error, PlateOutcome::Skipped});
    CHECK(results[2].error.empty());
}

TEST_CASE("a plate the validation refused reports that error", "[Mcp]")
{
    // Refused at the request: Plater records a finished run carrying the validation message.
    auto results = classify_plates({0}, {{0, false, false}}, {plate_run(11, 0, "Nozzle too small")});
    CHECK(results[0].outcome == PlateOutcome::Error);
    CHECK(results[0].error == "Nozzle too small");
    // Refused before the request (the setting change validated it), so no run at all.
    results = classify_plates({0}, {{0, false, false, "Layer height cannot exceed nozzle diameter"}}, {});
    CHECK(results[0].outcome == PlateOutcome::Error);
    CHECK(results[0].error == "Layer height cannot exceed nozzle diameter");
}

TEST_CASE("a slice stays pending until it was seen running or the grace period passed", "[Mcp]")
{
    SliceTicketState t = ticket(5, 10, 1);
    t.seen_running     = false;
    t.started          = std::chrono::steady_clock::now();
    CHECK(slice_status(t, observe(5, 1, {}), t.started + 100ms) == TicketStatus::Pending);
    CHECK(slice_status(t, observe(5, 1, {}), t.started + kSliceStartGrace + 1ms) == TicketStatus::Done);
    CHECK(slice_status(ticket(5, 10, 1), observe(5, 1, {}, true), std::chrono::steady_clock::now()) == TicketStatus::Pending);
}

TEST_CASE("a slice is superseded by another request or an edit while it ran", "[Mcp]")
{
    const auto now = std::chrono::steady_clock::now();
    const SliceTicketState t = ticket(5, 10, 1);
    CHECK(slice_status(t, observe(6, 1, {run(11, 1, 6)}), now) == TicketStatus::Superseded); // a GUI slice request during it
    CHECK(slice_status(t, observe(5, 2, {run(11, 2, 5)}), now) == TicketStatus::Superseded); // an edit before its run finished
    CHECK(slice_status(t, observe(5, 1, {run(11, 1, 5)}), now) == TicketStatus::Done);
}

TEST_CASE("actions after a slice finished do not supersede it", "[Mcp]")
{
    const auto now = std::chrono::steady_clock::now();
    const SliceTicketState t = ticket(5, 10, 1);
    // Another request and an edit, both after the run recorded its finish.
    CHECK(slice_status(t, observe(6, 2, {run(11, 1, 5)}), now) == TicketStatus::Done);
}

TEST_CASE("a slice with no run of its own never reads an earlier run's completion", "[Mcp]")
{
    const auto now = std::chrono::steady_clock::now();
    // Empty slice after a project reset: the last earlier run (seq 10) finished at another revision.
    SliceTicketState empty = ticket(5, 10, 7);
    empty.seen_running     = false;
    CHECK(slice_status(empty, observe(5, 7, {run(10, 3, 4)}), now) == TicketStatus::Done);
    // Cached slice whose start revision differs from the last earlier run's.
    CHECK(slice_status(empty, observe(5, 7, {}), now) == TicketStatus::Done);
    // No run, but the project changed before the terminal poll.
    CHECK(slice_status(empty, observe(5, 8, {}), now) == TicketStatus::Superseded);
}

TEST_CASE("cancellation is taken only from the operation's own runs", "[Mcp]")
{
    const SliceTicketState t = ticket(5, 10, 1);
    CHECK_FALSE(slice_cancelled(t, observe(5, 1, {run(10, 1, 5, true), run(11, 1, 5)})));
    CHECK(slice_cancelled(t, observe(5, 1, {run(11, 1, 5, true)})));
    // A person's slice started during the operation (a later request) and cancelled is not ours.
    CHECK_FALSE(slice_cancelled(t, observe(6, 1, {run(11, 1, 5), run(12, 1, 6, true, true, 6)})));
}

TEST_CASE("a later request supersedes at once only when it came while the slice ran", "[Mcp]")
{
    const auto             now = std::chrono::steady_clock::now();
    const SliceTicketState t   = ticket(5, 10, 1);
    // A person's slice for request 6, still running, restarted ours.
    SliceObservation during = observe(6, 1, {run(11, 1, 6, true), run(12, 0, 0, false, false, 6)}, true);
    during.interrupted      = true;
    CHECK(slice_status(t, during, now) == TicketStatus::Superseded);
    // Slice All asked again between two of its plates: the first plate's run finished cleanly.
    SliceObservation between = observe(6, 1, {run(11, 1, 5), run(12, 0, 0, false, false, 6)}, true);
    between.interrupted      = true;
    CHECK(slice_status(t, between, now) == TicketStatus::Superseded);
    // Requested after ours had finished: our result stands, and their slice is not waited for.
    CHECK(slice_status(t, observe(6, 1, {run(11, 1, 5), run(12, 0, 0, false, false, 6)}, true), now) == TicketStatus::Done);
    // Our own run still running, nobody else's: pending.
    CHECK(slice_status(t, observe(5, 1, {run(11, 0, 0, false, false)}, true), now) == TicketStatus::Pending);
}

TEST_CASE("a run already slicing that the request left running is the operation's", "[Mcp]")
{
    const auto       now = std::chrono::steady_clock::now();
    SliceTicketState t   = ticket(5, 10, 1);
    t.adopted_run        = 10;
    // Started for an earlier request, finished under ours with an error.
    SliceRunRecord adopted = run(10, 1, 5, false, true, 4);
    adopted.ok             = false;
    adopted.error          = "Floating regions";
    const SliceObservation obs = observe(5, 1, {adopted});
    REQUIRE(own_runs(t, obs).size() == 1);
    CHECK(slice_status(t, obs, now) == TicketStatus::Done);
    const auto results = classify_plates({0}, {{0, false, false}}, own_runs(t, obs));
    CHECK(results[0].outcome == PlateOutcome::Error);
    CHECK(results[0].error == "Floating regions");
    // Not adopted, the same run is someone else's.
    CHECK(own_runs(ticket(5, 10, 1), obs).empty());
}

TEST_CASE("early runs of a long Slice All are still seen", "[Mcp]")
{
    const SliceTicketState      t = ticket(5, 100, 1);
    std::vector<SliceRunRecord> runs;
    for (uint64_t i = 1; i <= 20; ++i)
        runs.push_back(run(100 + i, 1, 5, i == 2));
    CHECK(slice_cancelled(t, observe(5, 1, runs)));
    runs[1].cancelled          = false;
    runs[2].revision_at_finish = 9;
    CHECK(slice_status(t, observe(5, 1, runs), std::chrono::steady_clock::now()) == TicketStatus::Superseded);
}

TEST_CASE("export status follows its own sequence number", "[Mcp]")
{
    ExportRecord rec;
    rec.seq = 4;
    CHECK(export_status(3, 3, std::nullopt, 1) == ExportStatus::Rejected);
    CHECK(export_status(3, 4, rec, 1) == ExportStatus::Pending);
    rec.finished           = true;
    rec.ok                 = true;
    rec.revision_at_finish = 1;
    CHECK(export_status(3, 4, rec, 1) == ExportStatus::Ok);
    rec.revision_at_finish = 2;
    CHECK(export_status(3, 4, rec, 1) == ExportStatus::Superseded);
    rec.ok = false;
    CHECK(export_status(3, 4, rec, 2) == ExportStatus::Failed);
    rec.cancelled = true;
    CHECK(export_status(3, 4, rec, 2) == ExportStatus::Cancelled);
}
