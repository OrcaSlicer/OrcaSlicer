#pragma once

#include "McpCore.hpp"

#include "slic3r/GUI/CommandTypes.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

class JobRegistry;

// What the Agent Activity pane lists: every call automation makes, reads included (an agent's, a run's from Action
// Replay, the Run Script panel or the command line, a plugin's), and the calls the protocol refused before they ran. A
// person's own picks are left out, with every call made under them.
//
// An entry nests under the entry of the run that made it: a child call under the nearest of its parent jobs that has an
// entry. A Run Script panel session has no entry of its own: each work it runs has one (work_started()), and a call made
// in the session nests under the work running at the time, or is at the top level between works. A replay or script run
// whose result reports a failure (a step that failed, a script that raised) is failed, as its caller reads it.
//
// Arguments, requests, responses and errors are masked as captions are (hidden_values(), mask_hidden()), JSON text
// inside them read as JSON: an entry hides what its own call hides, what each call written inside it hides (a replay's
// steps, with the variables they use, in its arguments and its result), and, for a call that names a job (app_wait), what
// that job's call hid as it ends; with no record of that, its result isn't shown. A dialog hides what the calls running
// as it is answered hide, also where its message was cut. Each past kMaxBytes shows only its size, unmasked. What nests deeper than kMaxDepth,
// a payload or JSON text in one, is not read and not shown. The log keeps kMaxEntries entries, dropping the oldest finished ones
// first, with what is nested under them.
//
// It lists only while recording (set_recording()), which the pane turns on while it is open. What comes while it is off
// is not listed, but what it already lists still ends, and it keeps what placing and masking later entries needs: each
// job's marks (m_marks), so that a run started before recording lists its later calls at the top level under its
// source and a person's pick's calls stay out, and each job call's masking record (m_job_hidden).
//
// The pane reads it on the UI thread with changes_since(). No wx: the runtime makes it before the core, which reports
// every call to it on the call's thread. Thread-safe; every method is quick.
class ActivityLog final : public CallObserver
{
public:
    static constexpr std::size_t kMaxEntries = 2000;
    static constexpr std::size_t kMaxBytes   = 32 * 1024;
    // The masking records of job calls, kept for later waits on their jobs, are bounded by this many bytes.
    static constexpr std::size_t kMaxKeptHiddenBytes = 4 * 1024 * 1024;
    // A payload, or JSON text inside one, that nests arrays and objects deeper than this is not read (masking recurses
    // through what it reads) and not shown; a job call's whose arguments or result nest deeper has no masking record.
    static constexpr int kMaxDepth = 64;
    // A dialog answered while the calls running hide more values, or more bytes of them, than this shows neither title
    // nor message: checking them all would hold up the UI thread that reports the answer. Nor does one answered while a
    // call whose arguments couldn't be read runs.
    static constexpr std::size_t kMaxDialogValues = 10000;
    // A payload or an error checked against what it must hide by scanning more bytes than this (values × its length) is
    // not shown, so that masking never holds up a call's thread or the pane for long.
    static constexpr std::size_t kMaxMaskWork = 256 * 1024 * 1024;
    static constexpr std::size_t kMaxDialogBytes  = 1024 * 1024;
    // What the variables of the replays written in a call may put into their steps, in bytes, as the log reads what the
    // call must hide; past it, the rest is kept as written. Variables that put each other in over and over
    // grow as the power of the nesting, which a replay allows nine levels deep.
    static constexpr std::size_t kMaxPutIn = 1024 * 1024;

    // `jobs` gives a child call's parent chain. `changed` is called, on the thread that changed the log, when it changes
    // while no change waits for changes_since(): at most once between two reads. It must be quick; what it throws is
    // dropped.
    ActivityLog(const JobRegistry& jobs, std::function<void()> changed);

    // Whether the log lists what comes: on while the pane is open. While off it adds no entry, but the entries it holds
    // still change and end, and it keeps the marks of the runs going and the masking records of job calls (see the class
    // comment), so what it lists once on again is placed and masked. Off at first. Any thread.
    void set_recording(bool on);

    void call_queued(uint64_t call, const std::string& job, const std::string& parent, const nlohmann::json& origin) override;
    void call_started(uint64_t call, const NativeCommand& command, const nlohmann::json& args, const std::string& parent,
                      const std::string& job, const nlohmann::json& origin) override;
    void call_args(uint64_t call, const nlohmann::json& args) override;
    void call_ended(uint64_t call, const ToolResult& result) override;

    // A tools/call the protocol refused before the core saw it (McpProtocol::set_on_refused()): a failed entry.
    void refused(const std::string& tool, const nlohmann::json& origin, const ToolResult& error);
    // The dialog_auto_answer call of the job `job` put its rules in the ModalTracker layer `layer`: an answer by that
    // layer links that call's entry, when it has one. The runtime tells it under the lock an answer's report takes first
    // (AutomationRuntime::set_auto_answer()), so it is told before any answer by the layer.
    void rules_set(const std::string& job, int layer);
    // A rule of the ModalTracker layer `layer` answered a dialog, `dialog` being {title, message, match, button}: an
    // entry under the run whose rule it was (`run`, the run's job), else at the top level as `plugin`'s, else as a
    // client's, linked to the entry of the call that set the layer's rules (rules_set()).
    void dialog_answered(int layer, const std::string& run, const std::string& plugin, const nlohmann::json& dialog);
    // The Run Script panel, which runs one session at a time, starts a work in it: `work` is {work: "cell", "file" (Run
    // all) or "console", file, cell, title, lines: [first, last], code}, the work's request. Its calls nest under it
    // until work_ended(), which `status` (PythonSessionEvent's: finished, raised, cancelled, ...) ends as done when
    // "finished", with the JSON `result` the work left, and as failed otherwise, read as a script_run result
    // {status, message, result} is. The panel ends every work it starts, also one its session ends under or that it lets
    // go of.
    void work_started(nlohmann::json work);
    void work_ended(const std::string& status, const std::string& message, const nlohmann::json& result);

    // The entries added or changed since `version`, which a read returns as `version` for the next:
    // {version, reset, entries: [entry...], removed: [id...]}. With `reset`, the reader drops what it holds: `entries`
    // then holds every entry. An entry is {id, parent (0 at the top level), number, kind ("call"), source, client,
    // plugin, tool, read, args, request, response (null while running), status ("running", "done" or "failed"),
    // error, failure, withheld, job, started, ended (milliseconds since the epoch, 0 while running), rule}. `number`
    // counts the top-level calls and works, and from 1 the calls nested under each; a dialog's is 0. A dialog's entry
    // (kind "dialog") holds the dialog as its request and the id of the call that set the rule as `rule` (0 when the log
    // doesn't hold it). A work's (kind "work") holds the work as its request. `error` is the failure as the call gave it;
    // `failure` is one the log read from a run's result for the pane to word: {step, why} for a replay's failed step
    // (without `step` when the result names none), {script: status} for a script that ended other than by raising, or
    // without a message. A call whose own arguments nest too deep to read, or that repeats a job's result the log has no
    // masking record for as it ends, is `withheld`: its response is null and its error empty.
    nlohmann::json changes_since(uint64_t version);
    // Drops every entry whose calls have all ended, as the pane's Clear list does; running ones stay, and the numbering
    // starts again once none is left. The masking records stay, so a later wait on a cleared job still shows its result.
    void clear();

private:
    struct Entry
    {
        uint64_t       id      = 0;
        uint64_t       parent  = 0;
        uint64_t       version = 0; // the log's version when it last changed
        int            number  = 0;
        int            nested  = 0; // entries added under it, which number the next one
        std::string    kind;
        std::string    source, client, plugin;
        std::string    tool;
        bool           read = false;
        nlohmann::json args, request, response;
        std::string    status = "running";
        std::string    error;
        nlohmann::json failure;
        std::string    job;
        int64_t        started = 0, ended = 0;
        uint64_t       rule    = 0; // a dialog's: the dialog_auto_answer entry that set the rule
        std::string    reads_job;        // the job a call that reads one (app_wait, job_cancel) names
        bool           withheld = false; // its arguments nest too deep to read, or that job had no masking record as it
                                         // ended: its result and error aren't shown
        // What its request and response must not show, until it ends.
        std::vector<std::string> hidden;
    };

    // A core call that has not ended: its entry (0 for one not listed), its job, its tool (empty until it starts), and
    // whether its arguments nest too deep to read.
    struct Running
    {
        uint64_t    entry = 0;
        std::string job, tool;
        bool        unread = false;
    };

    // The client of a ModalTracker layer's rules, and the entry of the call that set them (0 for one not listed).
    struct Layer
    {
        uint64_t    entry = 0;
        std::string client;
    };

    // Adds `e` under `e.parent`, numbering it, and drops old entries past kMaxEntries. Under the lock.
    Entry& add(Entry e);
    // Sets the marks of `job` (see m_marks) from the nearest marked job above it (`parent`, or the call it is `under`),
    // else from `origin`; a job marked already keeps its marks. Under the lock.
    void mark(const std::string& job, const std::string& parent, const nlohmann::json& origin);
    // Where an entry made under the job `above` goes: under the nearest job above it that has an entry (its `parent`,
    // `source`, `client` and `plugin` set from it), else by the nearest marks above it, else, when every job above has
    // ended, by the marks of the entry's own job `own_job`: at the top level as that run's, as a Run Script session's
    // (under the work running), or nowhere (false) under a person's pick. Under the lock.
    bool place(const std::string& above, const std::string& own_job, Entry& e) const;
    // Ends `e` with `result`: its response, error, failure and status. `unread`: the call's arguments nest too deep to
    // read; `deep`: so does `result`'s data; `nested`: what that data repeats of the calls written in it (a replay's
    // steps). Under the lock.
    void finish(Entry& e, const ToolResult& result, bool unread, bool deep, const std::vector<std::string>& nested);
    void touch(Entry& e) { e.version = ++m_version; }
    // What the call of `job` hid; null when the log has no record of it. Under the lock.
    const std::vector<std::string>* job_hidden(const std::string& job) const;
    // The entry of the call of `job`, null for one not listed. Under the lock.
    const Entry* job_entry(const std::string& job) const;
    // Adds `hidden` to the masking record of the call of `job`, which `start` (its call starting) makes, within the bounds
    // of m_job_hidden. A value longer than kMaxBytes, which a capped payload can't hold but a shortened error can show a
    // part of, drops the record: a call that repeats the job's result then shows none of it. Under the lock.
    void keep_hidden(const std::string& job, const std::vector<std::string>& hidden, bool start);
    // Drops the masking record of `job`'s call, if any. Under the lock.
    void drop_hidden(const std::string& job);
    // Whether `id` and every entry nested under it have ended; collects them into `ids`. Under the lock.
    bool ended_with_nested(uint64_t id, std::vector<uint64_t>& ids) const;
    void remove(const std::vector<uint64_t>& ids);
    void trim();
    // Calls `changed` once for changes made since the last read. Not under the lock.
    void notify();

    const JobRegistry&    m_jobs;
    std::function<void()> m_changed;

    std::mutex                  m_mutex;
    std::map<uint64_t, Entry>   m_entries; // by id, oldest first
    uint64_t                    m_next_id     = 1;
    int                         m_next_number = 1;
    uint64_t                    m_version     = 0;
    // A reader whose version is older than this missed what changes_since() can tell it: it starts over.
    uint64_t                    m_reset_version = 0;
    std::deque<std::pair<uint64_t, uint64_t>> m_removed; // (version, id) of the entries dropped lately
    bool                        m_pending = false;      // a change waits for changes_since()

    bool                            m_recording = false;
    std::map<uint64_t, Running>     m_running;     // by call id
    std::map<std::string, uint64_t> m_job_entries; // a call's job -> its entry
    // What a job's call is, for the calls made under it, from when it is queued until it ends, listed or not: a run's
    // {source, client, plugin}, {person: true} for a person's pick, or {session: true} for a Run Script session and the
    // jobs under it. A child job copies its nearest marked ancestor's as it is queued, while that ancestor runs, so a run
    // that outlives its parent keeps them.
    std::map<std::string, nlohmann::json> m_marks;
    // What each job's call hid, possibly nothing, listed or not, by its job, from when it starts: the last kMaxEntries
    // of them, within kMaxKeptHiddenBytes.
    std::map<std::string, std::vector<std::string>> m_job_hidden;
    std::deque<std::string>                         m_job_hidden_order;
    std::size_t                                     m_job_hidden_bytes = 0;
    // The layers rules_set() was told of, by layer: each one whose call is listed, and the latest others, up to
    // 2 × kMaxEntries in all.
    std::map<int, Layer> m_layers;
    uint64_t             m_work = 0; // the entry of the work the Run Script panel runs, 0 for none
};

}}} // namespace Slic3r::GUI::Mcp
