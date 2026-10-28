#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

// The decision logic of the slice and export operations, kept free of wx and Plater so it can be
// unit-tested: the tools observe Plater on the UI thread and feed the observations in here.

enum class PlateOutcome { Ok, Error, Cancelled, Empty, Skipped };

struct PlateObservation
{
    int         index = 0;
    bool        valid = false; // a current slice result exists
    bool        empty = false; // no printable object on the plate
    std::string error;         // the validation error that keeps it from slicing
};

const char* to_string(PlateOutcome outcome);

// One background run, or a slice request the validation refused before any run (finished, with the
// validation error), as Plater numbers them.
struct SliceRunRecord
{
    uint64_t    seq       = 0;
    bool        finished  = false;
    bool        cancelled = false;
    bool        ok        = false; // finished with a result
    std::string error;
    uint64_t    revision_at_finish    = 0;
    uint64_t    request_seq_at_finish = 0;
    uint64_t    request_seq_at_start  = 0; // the slice request it runs for
    int         plate                 = -1;
};

struct PlateResult
{
    PlateOutcome outcome = PlateOutcome::Skipped;
    std::string  error;
};

// Per-plate outcome once the operation is terminal, from the operation's own runs (see own_runs())
// and the plates' state: the plate's last own run decides (Error with its message, Cancelled, or Ok
// when it finished with a result), whatever someone did to the plate after it finished. A plate no
// own run reached is Empty when it has nothing to print, Ok when it was already sliced, Error when
// its configuration fails validation (validated before the request, so no run was even attempted),
// Cancelled when the operation was cancelled, and Skipped otherwise (Slice All stops at a failing
// plate, or the plate is gone).
std::vector<PlateResult> classify_plates(const std::vector<int>& requested, const std::vector<PlateObservation>& plates,
                                         const std::vector<SliceRunRecord>& own_runs);

struct SliceTicketState
{
    uint64_t                              request_seq_mine  = 0; // Plater's request sequence right after our request
    uint64_t                              run_seq_before    = 0; // Plater's run sequence before our request
    uint64_t                              revision_at_start = 0;
    // A run already slicing when the request was made that the request left running (it returned
    // early, e.g. while a gizmo was in editing mode); its result is the one the request gets. 0 if none.
    uint64_t                              adopted_run       = 0;
    bool                                  seen_running      = false;
    std::chrono::steady_clock::time_point started;
};

// What the tool observed on the UI thread at one poll.
struct SliceObservation
{
    bool                        slicing_any      = false;
    uint64_t                    request_seq      = 0;
    // A later request came while the operation's own request was still slicing.
    bool                        interrupted      = false;
    uint64_t                    current_revision = 0;
    std::vector<SliceRunRecord> runs_since_start; // the adopted run, if any, and runs numbered after run_seq_before
};

// The runs that belong to the operation: the adopted run, and runs numbered after it began and
// started for its own request. Runs for a later request are someone else's slice; Slice All's later
// plates run for the same request, so they count.
std::vector<SliceRunRecord> own_runs(const SliceTicketState& ticket, const SliceObservation& obs);

// Whether the plate's current result can be the operation's: no run touched the plate since the
// operation began (it was already sliced), or the latest run that did is one of its own.
bool plate_result_is_own(int plate, const SliceTicketState& ticket, const SliceObservation& obs);

enum class TicketStatus { Pending, Done, Superseded };

// How long a slice that never visibly started may stay pending before it counts as finished (all
// requested plates were already valid or empty, so no run was needed).
inline constexpr std::chrono::milliseconds kSliceStartGrace{1500};

// A later request (someone sliced again) supersedes the operation at once if it came while the
// operation was still slicing; there is no point waiting for their slice. One that came after the
// operation finished leaves its result standing and is not waited for either. Otherwise terminal
// once slicing was seen running (or the grace period passed with nothing running) and nothing is
// slicing any more; then superseded when the project changed, or another slice was requested, while
// the operation ran - judged from snapshots only: the revision and request sequence each of its own
// runs recorded at its finish, or, with no run, the observation at the terminal poll itself. Never
// from an earlier completion. The caller freezes the first terminal status it gets.
TicketStatus slice_status(const SliceTicketState& ticket, const SliceObservation& obs, std::chrono::steady_clock::time_point now);

// True when one of the operation's own runs (own_runs()) reported cancellation.
bool slice_cancelled(const SliceTicketState& ticket, const SliceObservation& obs);

struct ExportRecord
{
    uint64_t    seq = 0;
    std::string path;
    bool        finished  = false;
    bool        ok        = false;
    bool        cancelled = false;
    std::string error;
    uint64_t    revision_at_finish = 0;
};

enum class ExportStatus { Rejected, Pending, Ok, Failed, Cancelled, Superseded };

const char* to_string(ExportStatus status);

// `seq_before`/`seq_after` are Plater's export sequence around the call that should have started the
// export; an unchanged sequence means it never started. `record` is the record of `seq_after`.
ExportStatus export_status(uint64_t seq_before, uint64_t seq_after, const std::optional<ExportRecord>& record, uint64_t revision_at_start);

}}} // namespace Slic3r::GUI::Mcp
