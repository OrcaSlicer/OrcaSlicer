#include "McpOperations.hpp"

#include <algorithm>

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

std::vector<PlateOutcome> classify_plates(const std::vector<int>& requested, const std::vector<PlateObservation>& plates, bool cancelled)
{
    std::vector<PlateOutcome> out;
    out.reserve(requested.size());
    for (int index : requested) {
        auto it = std::find_if(plates.begin(), plates.end(), [index](const PlateObservation& p) { return p.index == index; });
        if (it == plates.end())
            out.push_back(PlateOutcome::Skipped); // the plate no longer exists
        else if (it->empty)
            out.push_back(PlateOutcome::Empty);
        else if (it->valid)
            out.push_back(PlateOutcome::Ok);
        else if (it->error)
            out.push_back(PlateOutcome::Error);
        else
            out.push_back(cancelled ? PlateOutcome::Cancelled : PlateOutcome::Skipped);
    }
    return out;
}

TicketStatus slice_status(const SliceTicketState& ticket, const SliceObservation& obs, std::chrono::steady_clock::time_point now)
{
    if (obs.slicing_any)
        return TicketStatus::Pending;
    if (!ticket.seen_running && obs.runs_since_start.empty() && now - ticket.started < kSliceStartGrace)
        return TicketStatus::Pending;
    bool any_run = false;
    for (const SliceRunRecord& run : obs.runs_since_start) {
        if (run.seq <= ticket.run_seq_before)
            continue;
        if (!run.finished)
            return TicketStatus::Pending;
        any_run = true;
        if (run.request_seq_at_finish != ticket.request_seq_mine || run.revision_at_finish != ticket.revision_at_start)
            return TicketStatus::Superseded;
    }
    if (!any_run && (obs.request_seq != ticket.request_seq_mine || obs.current_revision != ticket.revision_at_start))
        return TicketStatus::Superseded;
    return TicketStatus::Done;
}

bool slice_cancelled(const SliceTicketState& ticket, const SliceObservation& obs)
{
    for (const SliceRunRecord& run : obs.runs_since_start)
        if (run.seq > ticket.run_seq_before && run.cancelled)
            return true;
    return false;
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
