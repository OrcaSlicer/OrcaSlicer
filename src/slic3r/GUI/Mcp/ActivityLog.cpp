#include "ActivityLog.hpp"

#include "JobRegistry.hpp"
#include "McpReplay.hpp"
#include "VideoOverlay.hpp"

#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/CommandTypes.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;

namespace {

int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string text_of(const json& v, const char* key)
{
    return v.is_object() && v.contains(key) && v[key].is_string() ? v[key].get<std::string>() : std::string();
}

// `value`, or what stands for it once it is over ActivityLog::kMaxBytes as JSON. Text that is not UTF-8 is counted as the
// pane sends it, with the bad bytes replaced.
json capped(json value)
{
    const std::size_t bytes = value.dump(-1, ' ', false, json::error_handler_t::replace).size();
    if (bytes <= ActivityLog::kMaxBytes)
        return value;
    return {{"truncated", true}, {"bytes", bytes}};
}

// Whether `value` nests arrays and objects deeper than ActivityLog::kMaxDepth, found without recursing.
bool too_deep(const json& value)
{
    std::vector<std::pair<const json*, int>> stack{{&value, 0}};
    while (!stack.empty()) {
        const auto [v, depth] = stack.back();
        stack.pop_back();
        if (!v->is_structured())
            continue;
        if (depth >= ActivityLog::kMaxDepth)
            return true;
        for (const json& item : *v)
            stack.emplace_back(&item, depth + 1);
    }
    return false;
}

// `value` as the log takes it in: itself, or the mask when it nests too deep for the log to read it, since masking and
// reading what calls hide walk it recursively; what can't be read is not shown.
json readable(const json& value) { return too_deep(value) ? json(kMask) : value; }

// As readable(), for a value whose depth `deep` tells, without a copy.
const json& readable(const json& value, bool deep)
{
    static const json mask(kMask);
    return deep ? mask : value;
}

// The JSON a string holds when it is JSON text, an object or an array, comments allowed as a replay's text allows them;
// null for any other text. Neither the parser nor destroying what it made recurses, but copying it would: it is only
// moved until too_deep() has passed it.
json json_text(const std::string& text)
{
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos || (text[first] != '{' && text[first] != '[' && text[first] != '/'))
        return {};
    json parsed = json::parse(text, nullptr, false, true);
    if (!parsed.is_structured())
        return {};
    return parsed;
}

// `value` masked as mask_hidden() masks it, with each string that is JSON text (a replay given as its text) read as JSON:
// when that masks something, it is written out again masked, so that what the text escapes (h for h) is masked too.
// JSON text too deep to read is not shown.
json masked(json value, const std::vector<std::string>& values)
{
    const std::function<void(json&)> read_texts = [&](json& v) {
        if (v.is_structured()) {
            for (json& item : v)
                read_texts(item);
            return;
        }
        if (!v.is_string())
            return;
        const json parsed = json_text(v.get_ref<const std::string&>());
        if (parsed.is_null())
            return;
        if (too_deep(parsed)) {
            v = kMask;
            return;
        }
        json out = parsed;
        read_texts(out);
        out = mask_hidden(std::move(out), values);
        if (out != parsed)
            v = out.dump(-1, ' ', false, json::error_handler_t::replace);
    };
    read_texts(value);
    return mask_hidden(std::move(value), values);
}

// `hidden` with each value once: masking looks for every value in every string.
std::vector<std::string> unique_values(std::vector<std::string> hidden)
{
    std::sort(hidden.begin(), hidden.end());
    hidden.erase(std::unique(hidden.begin(), hidden.end()), hidden.end());
    return hidden;
}

// A payload as an entry shows it: past kMaxBytes as JSON, what stands for its size, which shows nothing to mask; the mask
// when checking it against what it must hide would scan more than kMaxMaskWork bytes, holding up the call's thread and
// the pane, which waits for the log; else masked.
json shown(json value, const std::vector<std::string>& hidden)
{
    const std::size_t bytes = value.dump(-1, ' ', false, json::error_handler_t::replace).size();
    if (bytes > ActivityLog::kMaxBytes)
        return {{"truncated", true}, {"bytes", bytes}};
    const std::vector<std::string> values = unique_values(hidden);
    if (values.size() * bytes > ActivityLog::kMaxMaskWork)
        return kMask;
    // Masking may lengthen it: a short value becomes the mask.
    return capped(masked(std::move(value), values));
}

// An error's text as an entry shows it: masked, then cut to kMaxBytes; the mask past kMaxMaskWork, as for a payload.
std::string shown_text(const std::string& text, const std::vector<std::string>& hidden)
{
    const std::vector<std::string> values = unique_values(hidden);
    if (values.size() * text.size() > ActivityLog::kMaxMaskWork)
        return kMask;
    return shorten(masked(json(text), values).get<std::string>(), ActivityLog::kMaxBytes);
}

// A call's response as JSON: what it returned, or its error with the details it gave; data that nests too deep
// (`deep`), the mask.
json response_of(const ToolResult& r, bool deep)
{
    const json& data = readable(r.data, deep);
    if (!r.is_error)
        return data;
    json out{{"isError", true}, {"message", r.message}};
    if (!r.data.is_null() && !(r.data.is_object() && r.data.empty()))
        out["details"] = data;
    return out;
}

void nested_hidden(const json& value, std::vector<std::string>& out, std::size_t& budget);

// A replay given as an object, or as its JSON text, read; null for anything else, and for one too deep to read, whose
// text masked() then doesn't show.
json replay_of(const json& value)
{
    if (value.is_string()) {
        json replay = json_text(value.get<std::string>());
        if (!replay.is_object() || too_deep(replay))
            return {};
        return replay;
    }
    if (!value.is_object() || too_deep(value))
        return {};
    return value;
}

// The text a replay puts in for a placeholder: a string as it is, any other value as JSON.
std::string var_text(const json& var)
{
    return var.is_string() ? var.get<std::string>() : var.dump(-1, ' ', false, json::error_handler_t::replace);
}

// The placeholders `text` holds (`${BODY}`), by their bodies, in order: "vars.p", "env:HOME", ...
std::vector<std::string> placeholders(const std::string& text)
{
    std::vector<std::string> out;
    for (std::size_t at = text.find("${"); at != std::string::npos; at = text.find("${", at + 2)) {
        const std::size_t end = text.find('}', at + 2);
        if (end == std::string::npos)
            break;
        out.push_back(text.substr(at + 2, end - at - 2));
    }
    return out;
}

// The values a replay puts in for the placeholders `texts` hold, and for those the values hold in turn, however long the
// chain, each once. A null puts in nothing to hide.
std::vector<json> named_values(std::vector<std::string> texts, const Replay& replay, const ReplayContext& context)
{
    std::set<std::string> seen;
    std::vector<json>     out;
    while (!texts.empty()) {
        const std::string text = std::move(texts.back());
        texts.pop_back();
        for (const std::string& body : placeholders(text))
            if (seen.insert(body).second)
                if (std::optional<json> value = resolve_placeholder(body, replay, context); value && !value->is_null()) {
                    texts.push_back(var_text(*value));
                    out.push_back(std::move(*value));
                }
    }
    return out;
}

void call_hidden(const std::string& tool, const json& args, std::vector<std::string>& out, std::size_t& budget);

// Adds to `out` what a replay must not show as it runs: every value of a sensitive key anywhere in it, its variables
// included; what each step hides, as written and with its placeholders put in as the replay puts them in
// (substitute_args()), with the replay's own variables (which its text holds) and with `vars` the caller's, which win;
// and each value a placeholder in a hidden value puts in, directly or through another one (a variable, an environment
// variable), as written, put in and by every string and number inside it, which the replay's and the caller's `vars`
// show apart from the text it goes into ("password=${vars.p}"). What the placeholders put in is charged to `budget`
// (substitute_lenient()).
void replay_hidden(const json& replay, const json& vars, std::vector<std::string>& out, std::size_t& budget)
{
    if (!replay.is_object())
        return;
    const std::vector<std::string> sensitive = hidden_values({}, replay);
    out.insert(out.end(), sensitive.begin(), sensitive.end());
    Replay own;
    if (replay.contains("vars") && replay["vars"].is_object())
        own.vars = replay["vars"];
    Replay all;
    all.vars = own.vars;
    if (vars.is_object() && !too_deep(vars))
        all.vars.update(vars);
    if (!replay.contains("steps") || !replay["steps"].is_array())
        return;
    // The log has no saved jobs or folders: those placeholders put in nothing secret.
    const ReplayContext context;
    static const json   no_args = json::object();
    for (const json& step : replay["steps"]) {
        if (!step.is_object() || !step.contains("tool") || !step["tool"].is_string())
            continue;
        const std::string&       tool = step["tool"].get_ref<const std::string&>();
        const json&              args = step.contains("args") ? step["args"] : no_args;
        std::vector<std::string> written;
        call_hidden(tool, args, written, budget);
        out.insert(out.end(), written.begin(), written.end());
        for (const Replay* set : {&own, &all}) {
            call_hidden(tool, substitute_args_lenient(tool, args, *set, context, budget), out, budget);
            for (const json& value : named_values(written, *set, context)) {
                const json put = substitute_lenient(value, *set, context, budget);
                out.push_back(var_text(value));
                out.push_back(var_text(put));
                scalars(value, out);
                scalars(put, out);
            }
        }
    }
}

// Adds to `out` what a call of `tool` with `args` hides by its own tool's rule: its hidden values and, for a
// replay_run, what its replay, given as an object or as its JSON text, hides with its variables.
void tool_hidden(const std::string& tool, const json& args, std::vector<std::string>& out, std::size_t& budget)
{
    const std::vector<std::string> hidden = hidden_values(tool, args);
    out.insert(out.end(), hidden.begin(), hidden.end());
    if (tool == "replay_run" && args.is_object() && args.contains("replay"))
        replay_hidden(replay_of(args["replay"]), args.contains("vars") ? args["vars"] : json(), out, budget);
}

// Adds to `out` what the calls written inside `value` must not show: every object with a `tool` and its `args`, as a
// call, a replay's steps, a tool_call's target and a replay's result write them, by its own tool's rule
// (tool_hidden()), with what their replays' placeholders put in charged to `budget`.
void nested_hidden(const json& value, std::vector<std::string>& out, std::size_t& budget)
{
    if (value.is_object()) {
        if (value.contains("tool") && value["tool"].is_string() && value.contains("args"))
            tool_hidden(value["tool"].get_ref<const std::string&>(), value["args"], out, budget);
        for (const auto& [key, member] : value.items())
            nested_hidden(member, out, budget);
    } else if (value.is_array()) {
        for (const json& item : value)
            nested_hidden(item, out, budget);
    }
}

void nested_hidden(const json& value, std::vector<std::string>& out)
{
    std::size_t budget = ActivityLog::kMaxPutIn;
    nested_hidden(value, out, budget);
}

// Adds to `out` what a call of `tool` with `args` must not show: its own hidden values and those of the calls written
// inside its arguments.
void call_hidden(const std::string& tool, const json& args, std::vector<std::string>& out, std::size_t& budget)
{
    tool_hidden(tool, args, out, budget);
    nested_hidden(args, out, budget);
}

std::vector<std::string> call_hidden(const std::string& tool, const json& args)
{
    std::vector<std::string> hidden;
    std::size_t              budget = ActivityLog::kMaxPutIn;
    call_hidden(tool, args, hidden, budget);
    return hidden;
}

// What a call's result repeats of the calls written in it (a replay's steps), for one not too deep to read.
std::vector<std::string> repeated_hidden(const json& data, bool deep)
{
    std::vector<std::string> out;
    if (!deep)
        nested_hidden(data, out);
    return out;
}

// A run's failure that its tool reports in a successful result, for the pane: {message} for the message of a script
// that raised, {script: status} for one that ended otherwise (stopped, out of time, Python unavailable, refused) or
// without a message, which the pane words; {step, why} for a replay's failed step, without `step` when the result
// names none. Null for none. A Python script that raised, was cancelled or ran out of time, and a replay whose step
// failed, end without an error for their caller, which reads the outcome.
json reported_failure(const std::string& tool, const json& data)
{
    if (!data.is_object())
        return {};
    if (tool == "script_run" && data.contains("status") && data["status"].is_string() && data["status"] != "finished") {
        const std::string status  = data["status"].get<std::string>();
        const std::string message = text_of(data, "message");
        // The runner's own message for a stop, a time-out or no Python is agent-facing; the pane says it in the UI's
        // language. A script's exception is its own text.
        const bool worded =
            message.empty() || status == "cancelled" || status == "timed_out" || status == "unavailable" || status == "refused";
        return worded ? json{{"script", status}} : json{{"message", message}};
    }
    if (tool == "replay_run" && data.contains("passed") && data["passed"] == false) {
        const int   failed = data.contains("failed_step") && data["failed_step"].is_number_integer() ? data["failed_step"].get<int>() : 0;
        // The step by its number: a run of one step reports only that one.
        std::string why;
        if (failed > 0 && data.contains("steps") && data["steps"].is_array())
            for (const json& step : data["steps"])
                if (step.is_object() && step.contains("index") && step["index"] == failed) {
                    why = text_of(step, "expect_failure");
                    if (why.empty())
                        why = text_of(step, "error");
                }
        json out{{"why", why}};
        if (failed > 0)
            out["step"] = failed;
        return out;
    }
    return {};
}

// `text` with the start of `value` masked where the text ends in it and then "...": ModalTracker cuts a long message
// there, and a value can straddle the cut. A start shorter than three bytes is left, as mask_hidden() leaves a value that
// short inside other text, so a value of three bytes or less has no start to mask.
void mask_cut_start(std::string& text, const std::string& value)
{
    static const std::string cut = "...";
    if (value.size() < 4 || text.size() < cut.size() || text.compare(text.size() - cut.size(), cut.size(), cut) != 0)
        return;
    const std::size_t body = text.size() - cut.size();
    // As it is, and as text that dumps JSON writes it, as mask_hidden() matches a value.
    std::string escaped;
    if (value.find_first_of("\"\\\b\f\n\r\t") != std::string::npos) {
        escaped = json(value).dump(-1, ' ', false, json::error_handler_t::replace);
        escaped = escaped.substr(1, escaped.size() - 2);
    }
    const std::string* forms[] = {&value, &escaped};
    for (const std::string* form : forms)
        for (std::size_t n = std::min(form->size() - (form->empty() ? 0 : 1), body); n >= 3; --n)
            if (text.compare(body - n, n, *form, 0, n) == 0) {
                text.replace(body - n, n, kMask);
                return;
            }
}

} // namespace

ActivityLog::ActivityLog(const JobRegistry& jobs, std::function<void()> changed) : m_jobs(jobs), m_changed(std::move(changed)) {}

ActivityLog::Entry& ActivityLog::add(Entry e)
{
    e.id      = m_next_id++;
    e.started = now_ms();
    const bool numbered = e.kind != "dialog";
    if (e.parent != 0) {
        const auto parent = m_entries.find(e.parent);
        if (parent == m_entries.end())
            e.parent = 0;
        else if (numbered)
            e.number = ++parent->second.nested;
    }
    if (e.parent == 0 && numbered)
        e.number = m_next_number++;
    e.args    = shown(std::move(e.args), e.hidden);
    e.request = shown(std::move(e.request), e.hidden);
    touch(e);
    const uint64_t id = e.id;
    if (!e.job.empty())
        m_job_entries[e.job] = id;
    m_entries.emplace(id, std::move(e));
    trim();
    // The new entry is the newest of all and running or just failed; trim() drops older ones.
    return m_entries.at(id);
}

void ActivityLog::finish(Entry& e, const ToolResult& result, bool unread, bool deep, const std::vector<std::string>& nested)
{
    // A call whose own arguments couldn't be read can't be masked. One that reads a job repeats what that job's call
    // hid as it is now: its arguments may have been rewritten, or its record dropped, since this call started.
    e.withheld = unread;
    if (!e.reads_job.empty()) {
        const std::vector<std::string>* of_job = job_hidden(e.reads_job);
        if (of_job != nullptr)
            e.hidden.insert(e.hidden.end(), of_job->begin(), of_job->end());
        e.withheld = e.withheld || of_job == nullptr;
    }
    json response = response_of(result, deep);
    // A replay's result repeats its steps' arguments.
    e.hidden.insert(e.hidden.end(), nested.begin(), nested.end());
    json failure = result.is_error ? json{{"message", result.message}} : reported_failure(e.tool, result.data);
    // A result too deep to read still tells how the run went, from fields read without going down its deep branches, but
    // not why: its text may repeat what the log couldn't read to hide. The pane words the failure without it.
    if (!result.is_error && deep) {
        if (failure.contains("message"))
            failure = {{"script", text_of(result.data, "status")}};
        else if (failure.contains("why"))
            failure["why"] = "";
    }
    e.status = failure.is_null() ? "done" : "failed";
    if (!e.withheld) {
        e.response = shown(std::move(response), e.hidden);
        // The row shows the error's first line; the whole of it is in the response.
        if (failure.contains("message"))
            e.error = shown_text(failure["message"].get<std::string>(), e.hidden);
        else {
            if (failure.contains("why"))
                failure["why"] = shown_text(failure["why"].get<std::string>(), e.hidden);
            e.failure = std::move(failure);
        }
    }
    e.ended = now_ms();
    std::vector<std::string>().swap(e.hidden);
    touch(e);
}

const std::vector<std::string>* ActivityLog::job_hidden(const std::string& job) const
{
    const auto kept = m_job_hidden.find(job);
    return kept != m_job_hidden.end() ? &kept->second : nullptr;
}

const ActivityLog::Entry* ActivityLog::job_entry(const std::string& job) const
{
    const auto it    = m_job_entries.find(job);
    const auto entry = it != m_job_entries.end() ? m_entries.find(it->second) : m_entries.end();
    return entry != m_entries.end() ? &entry->second : nullptr;
}

void ActivityLog::keep_hidden(const std::string& job, const std::vector<std::string>& hidden, bool start)
{
    auto it = m_job_hidden.find(job);
    if (it == m_job_hidden.end()) {
        // A record dropped since its call started is not made again with only part of what the call hid.
        if (!start)
            return;
        it = m_job_hidden.emplace(job, std::vector<std::string>()).first;
        m_job_hidden_order.push_back(job);
    }
    for (const std::string& v : hidden) {
        if (v.size() > kMaxBytes) {
            drop_hidden(job);
            return;
        }
        it->second.push_back(v);
        m_job_hidden_bytes += v.size();
    }
    while (!m_job_hidden_order.empty() && (m_job_hidden_order.size() > kMaxEntries || m_job_hidden_bytes > kMaxKeptHiddenBytes)) {
        drop_hidden(m_job_hidden_order.front());
        m_job_hidden_order.pop_front();
    }
}

void ActivityLog::drop_hidden(const std::string& job)
{
    const auto record = m_job_hidden.find(job);
    if (record == m_job_hidden.end())
        return;
    for (const std::string& v : record->second)
        m_job_hidden_bytes -= v.size();
    m_job_hidden.erase(record);
}

void ActivityLog::set_recording(bool on)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_recording = on;
}

void ActivityLog::mark(const std::string& job, const std::string& parent, const json& origin)
{
    if (job.empty() || m_marks.count(job) != 0)
        return;
    json              marks;
    const std::string above = parent.empty() ? text_of(origin, "under") : parent;
    if (!above.empty())
        for (const std::string& up : m_jobs.ancestry(above))
            if (const auto it = m_marks.find(up); it != m_marks.end()) {
                marks = it->second;
                break;
            }
    if (marks.is_null()) {
        const std::string source = text_of(origin, "source");
        if (source == "person")
            marks = {{"person", true}};
        else if (source == "run_script" && parent.empty())
            marks = {{"session", true}};
        else
            marks = {{"source", source}, {"client", text_of(origin, "client")}, {"plugin", text_of(origin, "plugin")}};
    }
    m_marks[job] = std::move(marks);
}

void ActivityLog::call_queued(uint64_t call, const std::string& job, const std::string& parent, const json& origin)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    mark(job, parent, origin);
    m_running[call] = {0, job, {}};
}

void ActivityLog::call_started(uint64_t call, const NativeCommand& def, const json& given_args, const std::string& parent,
                               const std::string& job, const json& origin)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // For a call that was not queued: an inline one has no job, and a caller may report a job call only as it starts.
        mark(job, parent, origin);
        // Arguments too deep to read are not shown, and leave the job without a record; neither is what the call returns,
        // nor a dialog answered while it runs, which may repeat them.
        const bool deep = too_deep(given_args);
        m_running[call] = {0, job, def.key, deep};
        // A call with no job leaves nothing for later calls to read (mark() makes no marks for it): unlisted, it needs
        // no more.
        if (job.empty() && !m_recording)
            return;
        const json&              args   = readable(given_args, deep);
        std::vector<std::string> hidden = call_hidden(def.key, args);
        // Every job call's, listed or not, for a later call that repeats its result.
        if (!job.empty() && !deep)
            keep_hidden(job, hidden, true);
        // A call another call started to outlive it is placed as that call's child.
        const std::string above_of = parent.empty() ? text_of(origin, "under") : parent;
        // A person's pick is the person's own doing, and a Run Script session is shown by the works it runs.
        if (const auto marked = m_marks.find(job); above_of.empty() && marked != m_marks.end() &&
                                                   (marked->second.contains("person") || marked->second.contains("session")))
            return;
        if (!m_recording)
            return;
        Entry e;
        e.kind   = "call";
        e.tool   = def.key;
        e.read   = def.guard == GuardClass::Inspect;
        e.job    = job;
        e.hidden = std::move(hidden);
        // A call that reads a job (app_wait's result, job_cancel's) may repeat what that job's call hid; with no record
        // of it as it ends, what it returns is not shown (finish()).
        if (args.is_object() && args.contains("job") && args["job"].is_string()) {
            e.reads_job = args["job"].get<std::string>();
            if (const std::vector<std::string>* of_job = job_hidden(e.reads_job))
                e.hidden.insert(e.hidden.end(), of_job->begin(), of_job->end());
        }
        e.source = text_of(origin, "source");
        e.client = text_of(origin, "client");
        if (above_of.empty())
            e.plugin = text_of(origin, "plugin");
        else if (!place(above_of, job, e))
            return;
        // An agent's own request is the message it sent; anyone else's, and a call made under another, the call as the
        // app runs it.
        e.request = above_of.empty() && e.source == "agent" && origin.contains("request") ? readable(origin["request"]) :
                                                                                            json{{"tool", def.key}, {"args", args}};
        e.args                = args;
        m_running[call].entry = add(std::move(e)).id;
    }
    notify();
}

bool ActivityLog::place(const std::string& above, const std::string& own_job, Entry& e) const
{
    const auto by_marks = [&](const json& marks) {
        if (marks.contains("person"))
            return false;
        // A Run Script session has no entry: what is made in it goes under the work it runs.
        if (marks.contains("session")) {
            e.source = "run_script";
            if (const auto work = m_entries.find(m_work); work != m_entries.end() && work->second.status == "running")
                e.parent = m_work;
            return true;
        }
        // A run that started before recording has no entry: what it makes goes to the top level as its own.
        e.source = text_of(marks, "source");
        e.client = text_of(marks, "client");
        e.plugin = text_of(marks, "plugin");
        return true;
    };
    // The nearest job above that has an entry, past any that has none: a run started while the log didn't record may be
    // nested in one it lists.
    const std::vector<std::string> chain = m_jobs.ancestry(above);
    for (const std::string& job : chain)
        if (const Entry* run = job_entry(job)) {
            e.parent = run->id;
            e.source = run->source;
            e.client = run->client;
            e.plugin = run->plugin;
            return true;
        }
    for (const std::string& job : chain)
        if (const auto marked = m_marks.find(job); marked != m_marks.end())
            return by_marks(marked->second);
    // Every job above has ended: a job call has the marks it copied from them as it was queued.
    if (const auto own = m_marks.find(own_job); !own_job.empty() && own != m_marks.end())
        return by_marks(own->second);
    return true;
}

void ActivityLog::call_args(uint64_t call, const json& given_args)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto                  it = m_running.find(call);
        if (it == m_running.end())
            return;
        const bool deep   = too_deep(given_args);
        it->second.unread = it->second.unread || deep;
        if (deep)
            drop_hidden(it->second.job);
        // A later call that repeats the job's result must hide what the rewrite reveals, whether or not this one is
        // listed; with neither a record nor an entry to take it, nothing does.
        const bool kept  = job_hidden(it->second.job) != nullptr;
        const auto entry = m_entries.find(it->second.entry);
        if (!kept && entry == m_entries.end())
            return;
        const json&                    args  = readable(given_args, deep);
        const std::vector<std::string> extra = call_hidden(it->second.tool, args);
        if (kept)
            keep_hidden(it->second.job, extra, false);
        if (entry == m_entries.end())
            return;
        Entry& e = entry->second;
        e.hidden.insert(e.hidden.end(), extra.begin(), extra.end());
        e.args = shown(args, e.hidden);
        // The request shows the call as made; what the rewrite reveals is masked there too.
        e.request = shown(std::move(e.request), e.hidden);
        touch(e);
    }
    notify();
}

void ActivityLog::call_ended(uint64_t call, const ToolResult& result)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto                  it = m_running.find(call);
        if (it == m_running.end())
            return;
        const Running run = std::move(it->second);
        m_running.erase(it);
        // A job's marks go with its call: the jobs under it copied them as they were queued.
        m_marks.erase(run.job);
        const auto e = m_entries.find(run.entry);
        if (run.job.empty() && e == m_entries.end())
            return;
        // What its result repeats of the calls written in it (a replay's steps) joins its record and its entry's mask.
        const bool                     deep   = too_deep(result.data);
        const std::vector<std::string> nested = repeated_hidden(result.data, deep);
        if (!run.job.empty() && deep)
            drop_hidden(run.job);
        else if (!run.job.empty())
            keep_hidden(run.job, nested, false);
        if (e == m_entries.end())
            return;
        finish(e->second, result, run.unread, deep, nested);
    }
    notify();
}

void ActivityLog::refused(const std::string& tool, const json& origin, const ToolResult& error)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_recording)
            return;
        Entry e;
        e.kind    = "call";
        e.tool    = tool;
        e.source  = text_of(origin, "source");
        e.client  = text_of(origin, "client");
        e.request = origin.is_object() && origin.contains("request") ? readable(origin["request"]) : json();
        const json& params    = e.request.is_object() && e.request.contains("params") ? e.request["params"] : json();
        const json  arguments = params.is_object() && params.contains("arguments") ? params["arguments"] : json::object();
        // What the request must not show, for the tool it called and, through tool_call, the tool that names.
        e.hidden = call_hidden(text_of(params, "name"), arguments);
        // A tool_call is the call of the tool it names, with that tool's arguments.
        e.args = text_of(params, "name") == "tool_call" && tool != "tool_call" && arguments.is_object() && arguments.contains("args") ?
                     arguments["args"] :
                     arguments;
        const bool deep = too_deep(error.data);
        finish(add(std::move(e)), error, false, deep, repeated_hidden(error.data, deep));
    }
    notify();
}

void ActivityLog::rules_set(const std::string& job, int layer)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (layer == 0)
        return;
    Layer& set = m_layers[layer];
    if (const Entry* listed = job_entry(job)) {
        set.entry  = listed->id;
        set.client = listed->client;
    } else if (const auto marked = m_marks.find(job); marked != m_marks.end())
        // Not listed: the answers still name the client that set the rules.
        set.client = text_of(marked->second, "client");
    // A layer whose call is listed stays while it is; the latest others stay for their client, up to 2 × kMaxEntries
    // layers in all.
    for (auto it = m_layers.begin(); it != m_layers.end() && m_layers.size() > 2 * kMaxEntries;)
        it = m_entries.count(it->second.entry) != 0 ? std::next(it) : m_layers.erase(it);
}

void ActivityLog::dialog_answered(int layer, const std::string& run, const std::string& plugin, const json& dialog)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_recording)
            return;
        Entry e;
        e.kind   = "dialog";
        e.status = "done";
        e.source = !plugin.empty() ? "plugin" : "agent";
        e.plugin = plugin;
        // The layer is the rules of one dialog_auto_answer call, which no later call changes.
        if (const auto set = m_layers.find(layer); set != m_layers.end()) {
            if (m_entries.count(set->second.entry) != 0)
                e.rule = set->second.entry;
            if (run.empty() && plugin.empty())
                e.client = set->second.client;
        }
        if (!run.empty() && !place(run, {}, e))
            return;
        // The dialog may show what the calls running as it is answered hide (a key a call set, then refused in a message),
        // also where ModalTracker cut a long message. Past kMaxDialogValues of them, which would hold up the UI thread
        // that reports the answer, its title and message are not shown.
        e.request = dialog;
        for (const char* key : {"title", "message"})
            if (!e.request.contains(key) || !e.request[key].is_string())
                e.request[key] = "";
        std::vector<const std::vector<std::string>*> running_hidden;
        std::size_t                                  count = 0, bytes = 0;
        bool                                         unread = false;
        for (const auto& [call, running] : m_running) {
            unread = unread || running.unread;
            if (const auto it = m_entries.find(running.entry); it != m_entries.end())
                running_hidden.push_back(&it->second.hidden);
            if (const std::vector<std::string>* kept = job_hidden(running.job))
                running_hidden.push_back(kept);
        }
        for (const std::vector<std::string>* values : running_hidden) {
            count += values->size();
            for (std::size_t i = 0; i < values->size() && count <= kMaxDialogValues && bytes <= kMaxDialogBytes; ++i)
                bytes += (*values)[i].size();
        }
        if (unread || count > kMaxDialogValues || bytes > kMaxDialogBytes) {
            e.request["title"]   = kMask;
            e.request["message"] = kMask;
        } else
            for (const std::vector<std::string>* values : running_hidden)
                for (const std::string& v : *values) {
                    mask_cut_start(e.request["message"].get_ref<std::string&>(), v);
                    e.hidden.push_back(v);
                }
        Entry& added = add(std::move(e));
        added.ended  = added.started;
        std::vector<std::string>().swap(added.hidden);
    }
    notify();
}

void ActivityLog::work_started(json work)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // A work run while not recording is not listed, and neither is a call it makes listed under it.
        m_work = 0;
        if (!m_recording)
            return;
        Entry e;
        e.kind    = "work";
        e.tool    = "script_run";
        e.source  = "run_script";
        e.args    = json::object();
        e.request = std::move(work);
        m_work    = add(std::move(e)).id;
    }
    notify();
}

void ActivityLog::work_ended(const std::string& status, const std::string& message, const json& result)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto                  work = m_entries.find(std::exchange(m_work, 0));
        if (work == m_entries.end() || work->second.status != "running")
            return;
        // As script_run's result reads, which tells its failure (reported_failure()).
        json out{{"status", status}};
        if (!message.empty())
            out["message"] = message;
        if (!result.is_null())
            out["result"] = readable(result);
        const bool                     deep   = too_deep(out);
        const std::vector<std::string> nested = repeated_hidden(out, deep);
        finish(work->second, ToolResult::ok(std::move(out)), false, deep, nested);
    }
    notify();
}

bool ActivityLog::ended_with_nested(uint64_t id, std::vector<uint64_t>& ids) const
{
    // Nested entries come after their parent, so one pass from it collects them all.
    std::set<uint64_t> tree{id};
    for (auto it = m_entries.find(id); it != m_entries.end(); ++it) {
        const Entry& e = it->second;
        if (e.id != id && tree.count(e.parent) == 0)
            continue;
        if (e.status == "running")
            return false;
        tree.insert(e.id);
    }
    ids.insert(ids.end(), tree.begin(), tree.end());
    return true;
}

void ActivityLog::remove(const std::vector<uint64_t>& ids)
{
    for (const uint64_t id : ids) {
        const auto it = m_entries.find(id);
        if (it == m_entries.end())
            continue;
        if (const auto job = m_job_entries.find(it->second.job); job != m_job_entries.end() && job->second == id)
            m_job_entries.erase(job);
        m_entries.erase(it);
        m_removed.emplace_back(++m_version, id);
    }
    // A reader older than a removal this forgets would miss it.
    while (m_removed.size() > kMaxEntries) {
        m_reset_version = std::max(m_reset_version, m_removed.front().first);
        m_removed.pop_front();
    }
}

void ActivityLog::trim()
{
    for (auto it = m_entries.begin(); m_entries.size() > kMaxEntries && it != m_entries.end();) {
        std::vector<uint64_t> ids;
        if (!ended_with_nested(it->first, ids)) {
            ++it;
            continue;
        }
        // What it dropped is `it` and entries after it: the next one left comes after `it`.
        const uint64_t root = it->first;
        remove(ids);
        it = m_entries.upper_bound(root);
    }
}

json ActivityLog::changes_since(uint64_t version)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pending        = false;
    const bool reset = version < m_reset_version || version > m_version;
    json       out{{"version", m_version}, {"reset", reset}, {"entries", json::array()}, {"removed", json::array()}};
    for (const auto& [id, e] : m_entries) {
        if (!reset && e.version <= version)
            continue;
        out["entries"].push_back({{"id", e.id},
                                  {"parent", e.parent},
                                  {"number", e.number},
                                  {"kind", e.kind},
                                  {"source", e.source},
                                  {"client", e.client},
                                  {"plugin", e.plugin},
                                  {"tool", e.tool},
                                  {"read", e.read},
                                  {"args", e.args},
                                  {"request", e.request},
                                  {"response", e.response},
                                  {"status", e.status},
                                  {"error", e.error},
                                  {"failure", e.failure},
                                  {"withheld", e.withheld},
                                  {"job", e.job},
                                  {"started", e.started},
                                  {"ended", e.ended},
                                  {"rule", e.rule}});
    }
    if (!reset)
        for (const auto& [v, id] : m_removed)
            if (v > version)
                out["removed"].push_back(id);
    return out;
}

void ActivityLog::clear()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<uint64_t> ids;
        for (const auto& [id, e] : m_entries)
            if (e.parent == 0) {
                std::vector<uint64_t> tree;
                if (ended_with_nested(id, tree))
                    ids.insert(ids.end(), tree.begin(), tree.end());
            }
        remove(ids);
        if (m_entries.empty())
            m_next_number = 1;
        // Readers start over from what is left.
        m_reset_version = ++m_version;
    }
    notify();
}

void ActivityLog::notify()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_pending)
            return;
        m_pending = true;
    }
    if (!m_changed)
        return;
    try {
        m_changed();
    } catch (...) {
        // The pane reads the change at its next refresh.
    }
}

}}} // namespace Slic3r::GUI::Mcp
