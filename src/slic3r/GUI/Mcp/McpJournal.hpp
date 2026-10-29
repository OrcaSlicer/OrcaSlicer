#pragma once

#include "McpTypes.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

// The session's client calls, in the order they were made, for export_script. A call that returned
// a job envelope is replayed with `await:false` and a name its later `wait` refers to, so a step
// that opened a dialog can be followed by the steps that answered it. An input call that only listed
// the menu it opened is kept, since it may have changed the selection, unless the next call repeats
// it with a popup_item.
//
// A call that runs as a job takes its job's final result, not its reply: a job that fails after its
// reply came back as running is left out like a call that failed at once, and one still running is
// left out because its outcome is not known yet. A job whose reply said it waits on a dialog stays
// in the script whatever becomes of it, since the steps after it answer that dialog. So does a job
// that was running while a later call answered a dialog: the dialog may be one the job opened, after
// its reply or before it, and the answer depends on it. Such a job replays with `await:false`, and a
// call that answered a dialog replays after a `wait {for:"dialog"}`, since the dialog may open later
// than the job's replayed reply. Thread-safe.
class Journal
{
public:
    // Starts an entry; returns its sequence number. Calls made by scripts are not recorded.
    // `answers_dialog`: the call acts while a modal dialog is open (dialog_respond, input to it).
    uint64_t begin(const std::string& tool, const nlohmann::json& args, bool answers_dialog = false);
    void     set_args(uint64_t seq, nlohmann::json args);
    // The call's reply.
    void     end(uint64_t seq, const ToolResult& result);
    // The call runs as `job`; its outcome is open until job_ended().
    void     job_started(uint64_t seq, const std::string& job);
    // The job's final result, reported before a `wait` on the job can see it, so an export right
    // after that wait already knows the outcome.
    void     job_ended(uint64_t seq, const ToolResult& result);
    void     clear();

    // {"orca_script":1, "name":..., "steps":[...]} with job names substituted and screenshot paths
    // rewritten to ${out_dir}/<file name>. The calls left out because their outcome is still open go
    // to `left_out_running` as [{tool, job}] (no job for a call that is not one).
    nlohmann::json to_script(const std::string& name, nlohmann::json* left_out_running = nullptr) const;

private:
    struct Entry
    {
        uint64_t       seq = 0;
        std::string    tool;
        nlohmann::json args;
        bool           finished = false;    // the call replied
        bool           failed   = false;    // the reply, or the job's final result, was an error
        bool           menu_listed = false; // the reply listed a menu the call opened (popup_menu)
        std::string    job;                 // the call's job, when it runs as one
        bool           job_ended = false;
        std::string    envelope;            // "running" or "blocked_by_dialog" when the reply was the job's envelope
        bool           dialog_answered = false; // a later call answered a dialog while the job ran
        bool           answers_dialog  = false; // the call acted while a modal dialog was open

        // Whether the outcome is still open: the call has not replied, or its job has not ended.
        bool running() const { return job.empty() ? !finished : !job_ended; }
        // Whether the call goes into a script.
        bool kept() const { return envelope == "blocked_by_dialog" || dialog_answered || (!failed && !running()); }
        // Whether the step replays without waiting for its job: the steps after it answer its dialog.
        bool unawaited() const { return !envelope.empty() || dialog_answered; }
    };

    mutable std::mutex m_mutex;
    static constexpr std::size_t kMaxEntries = 5000;
    std::vector<Entry>           m_entries;
    uint64_t           m_next = 1;
};

}}} // namespace Slic3r::GUI::Mcp
