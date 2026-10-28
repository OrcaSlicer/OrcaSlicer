#include "McpOperations.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

const char* to_string(PlateOutcome outcome)
{
    switch (outcome) {
    case PlateOutcome::Ok: return "ok";
    case PlateOutcome::Error: return "error";
    case PlateOutcome::Cancelled: return "cancelled";
    case PlateOutcome::Empty: return "empty";
    case PlateOutcome::Skipped: return "skipped";
    }
    return "skipped";
}

const char* to_string(ExportStatus status)
{
    switch (status) {
    case ExportStatus::Rejected: return "rejected";
    case ExportStatus::Pending: return "pending";
    case ExportStatus::Ok: return "ok";
    case ExportStatus::Failed: return "failed";
    case ExportStatus::Cancelled: return "cancelled";
    case ExportStatus::Superseded: return "superseded";
    }
    return "failed";
}

std::vector<SliceRunRecord> own_runs(const SliceTicketState& ticket, const SliceObservation& obs)
{
    std::vector<SliceRunRecord> out;
    for (const SliceRunRecord& run : obs.runs_since_start)
        if ((run.seq > ticket.run_seq_before && run.request_seq_at_start == ticket.request_seq_mine && run.for_request) ||
            (ticket.adopted_run != 0 && run.seq == ticket.adopted_run))
            out.push_back(run);
    return out;
}

std::vector<PlateResult> classify_plates(const std::vector<int>& requested, const std::vector<PlateObservation>& plates,
                                         const std::vector<SliceRunRecord>& own_runs)
{
    const bool cancelled = std::any_of(own_runs.begin(), own_runs.end(), [](const SliceRunRecord& r) { return r.cancelled; });
    std::vector<PlateResult> out;
    for (int index : requested) {
        auto it = std::find_if(plates.begin(), plates.end(), [index](const PlateObservation& p) { return p.index == index; });
        const SliceRunRecord* last = nullptr;
        for (const SliceRunRecord& run : own_runs)
            if (run.plate == index && run.finished)
                last = &run;
        // The operation's own run decides, whatever happened to the plate after it finished.
        if (last != nullptr && !last->error.empty())
            out.push_back({PlateOutcome::Error, last->error});
        else if (last != nullptr && last->cancelled)
            out.push_back({PlateOutcome::Cancelled, {}});
        else if (last != nullptr && last->ok)
            out.push_back({PlateOutcome::Ok, {}});
        else if (last != nullptr)
            out.push_back({PlateOutcome::Error, "the slice finished without a result"});
        else if (it == plates.end())
            out.push_back({PlateOutcome::Skipped, {}}); // the plate no longer exists
        else if (it->empty)
            out.push_back({PlateOutcome::Empty, {}});
        else if (it->valid)
            out.push_back({PlateOutcome::Ok, {}}); // already sliced, so no run was needed
        else if (!it->error.empty())
            out.push_back({PlateOutcome::Error, it->error});
        else
            out.push_back({cancelled ? PlateOutcome::Cancelled : PlateOutcome::Skipped, {}});
    }
    return out;
}

bool plate_result_is_own(int plate, const SliceTicketState& ticket, const SliceObservation& obs)
{
    const SliceRunRecord* latest = nullptr;
    for (const SliceRunRecord& run : obs.runs_since_start)
        if (run.plate == plate && (latest == nullptr || run.seq > latest->seq))
            latest = &run;
    if (latest == nullptr)
        return true;
    const std::vector<SliceRunRecord> mine = own_runs(ticket, obs);
    return std::any_of(mine.begin(), mine.end(), [latest](const SliceRunRecord& run) { return run.seq == latest->seq; });
}

TicketStatus slice_status(const SliceTicketState& ticket, const SliceObservation& obs, std::chrono::steady_clock::time_point now)
{
    const bool later_request = obs.request_seq != ticket.request_seq_mine;
    if (later_request && obs.interrupted)
        return TicketStatus::Superseded;
    // After a later request, whatever is slicing is someone else's.
    if (obs.slicing_any && !later_request)
        return TicketStatus::Pending;
    const std::vector<SliceRunRecord> mine = own_runs(ticket, obs);
    if (!later_request && !ticket.seen_running && mine.empty() && now - ticket.started < kSliceStartGrace)
        return TicketStatus::Pending;
    for (const SliceRunRecord& run : mine) {
        if (!run.finished)
            return TicketStatus::Pending;
        if (run.request_seq_at_finish != ticket.request_seq_mine || run.revision_at_finish != ticket.revision_at_start)
            return TicketStatus::Superseded;
    }
    if (mine.empty() && obs.current_revision != ticket.revision_at_start)
        return TicketStatus::Superseded;
    return TicketStatus::Done;
}

bool slice_cancelled(const SliceTicketState& ticket, const SliceObservation& obs)
{
    const std::vector<SliceRunRecord> mine = own_runs(ticket, obs);
    return std::any_of(mine.begin(), mine.end(), [](const SliceRunRecord& r) { return r.cancelled; });
}

ExportStatus export_status(uint64_t seq_before, uint64_t seq_after, const std::optional<ExportRecord>& record, uint64_t revision_at_start)
{
    if (seq_after == seq_before)
        return ExportStatus::Rejected;
    if (!record || !record->finished)
        return ExportStatus::Pending;
    if (record->cancelled)
        return ExportStatus::Cancelled;
    if (!record->ok)
        return ExportStatus::Failed;
    if (record->revision_at_finish != revision_at_start)
        return ExportStatus::Superseded;
    return ExportStatus::Ok;
}

}}} // namespace Slic3r::GUI::Mcp
