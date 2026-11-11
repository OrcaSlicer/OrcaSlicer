#include "McpJournal.hpp"
#include "McpPaths.hpp"
#include "McpTypes.hpp"

#include <boost/filesystem/path.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;

namespace {

// The size a call's arguments count against the journal's byte bound. Invalid UTF-8 a tool recorded is
// counted as replacement characters rather than thrown on: the journal only needs the size.
std::size_t serialized_size(const json& args) { return args.dump(-1, ' ', false, json::error_handler_t::replace).size(); }

} // namespace

json without_expected_ids(const std::string& tool, json args)
{
    // The tools that take expected ids; any other tool's arguments (a plugin operation's own fields among
    // them) are recorded as given.
    static const std::set<std::string> takes_ids{"set_settings", "transform", "add_part", "set_height_ranges",
                                                 "set_variable_layer_height", "select_objects"};
    if (!args.is_object())
        return args;
    if (tool == "call_tool") {
        if (args.contains("args") && args["args"].is_object())
            args["args"] = without_expected_ids(args.value("tool", std::string()), std::move(args["args"]));
        return args;
    }
    if (tool == "run_replay") {
        // An inline replay given as JSON text is recorded as the object it parses to, so its steps are
        // stripped too. Text that doesn't parse stays as given: the call fails, and no replay keeps it.
        if (args.contains("replay") && args["replay"].is_string()) {
            json parsed = json::parse(args["replay"].get<std::string>(), nullptr, /*allow_exceptions=*/false, /*ignore_comments=*/true);
            if (parsed.is_object())
                args["replay"] = std::move(parsed);
        }
        if (args.contains("replay") && args["replay"].is_object() && args["replay"].contains("steps") && args["replay"]["steps"].is_array())
            for (json& step : args["replay"]["steps"])
                if (step.is_object() && step.contains("args"))
                    step["args"] = without_expected_ids(step.value("tool", std::string()), std::move(step["args"]));
        return args;
    }
    if (takes_ids.count(tool) != 0)
        for (const char* key : {"object_id", "part_id", "object_ids", "part_ids", "instance_object_ids"})
            args.erase(key);
    return args;
}

uint64_t Journal::begin(const std::string& tool, const nlohmann::json& args, bool answers_dialog)
{
    // The copy and its size are made before the lock: serializing a large call takes a while, and
    // every other call's begin and end waits on the lock.
    Entry e;
    e.tool           = tool;
    e.args           = without_expected_ids(tool, args);
    e.bytes          = serialized_size(e.args);
    e.answers_dialog = answers_dialog;

    std::lock_guard<std::mutex> lock(m_mutex);
    if (answers_dialog)
        for (Entry& running : m_entries)
            if (!running.job.empty() && !running.job_ended)
                running.dialog_answered = true;
    e.seq              = m_next++;
    const uint64_t seq = e.seq;
    m_bytes += e.bytes;
    m_entries.push_back(std::move(e));
    trim();
    return seq;
}

void Journal::set_args(uint64_t seq, nlohmann::json args)
{
    // As in begin(), the copy and its size are made outside the lock; only the call's tool, which
    // decides what the copy leaves out, is read under it.
    auto find = [this, seq] {
        return std::find_if(m_entries.begin(), m_entries.end(), [seq](const Entry& e) { return e.seq == seq; });
    };
    std::string tool;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (const auto it = find(); it != m_entries.end())
            tool = it->tool;
        else
            return;
    }
    args                    = without_expected_ids(tool, std::move(args));
    const std::size_t bytes = serialized_size(args);

    std::lock_guard<std::mutex> lock(m_mutex);
    // export_replay {clear:true} may have emptied the journal meanwhile.
    const auto it = find();
    if (it == m_entries.end())
        return;
    m_bytes -= it->bytes;
    it->args  = std::move(args);
    it->bytes = bytes;
    m_bytes += bytes;
    trim();
}

void Journal::end(uint64_t seq, const ToolResult& result)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (Entry& e : m_entries)
        if (e.seq == seq) {
            e.finished    = true;
            e.failed      = e.failed || result.is_error;
            e.menu_listed = !result.is_error && result.data.is_object() && result.data.contains("popup_menu");
            // The envelope names the call's own job; a `wait` that timed out names another one.
            if (result.is_error || e.job.empty() || !result.data.is_object())
                continue;
            const auto job    = result.data.find("job");
            const auto status = result.data.find("status");
            if (job != result.data.end() && job->is_string() && job->get<std::string>() == e.job && status != result.data.end() &&
                (*status == "running" || *status == "blocked_by_dialog"))
                e.envelope = status->get<std::string>();
        }
    trim();
}

void Journal::job_started(uint64_t seq, const std::string& job)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (Entry& e : m_entries)
        if (e.seq == seq)
            e.job = job;
}

void Journal::job_ended(uint64_t seq, const ToolResult& result)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (Entry& e : m_entries)
        if (e.seq == seq) {
            e.job_ended = true;
            e.failed    = e.failed || result.is_error;
        }
    // The job may end after its reply, which then trimmed nothing for this call.
    trim();
}

void Journal::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.clear();
    m_bytes = 0;
}

void Journal::trim()
{
    // A session kept open for days, or one that sends thousands of large calls (a CAD run's sketch_add),
    // must not grow without bound; a replay is exported from the recent calls anyway (export_replay
    // {clear:true} starts afresh). The oldest calls whose outcome is known go first; a call still
    // waiting for its reply or its job keeps its place, even when that holds the journal over a bound
    // until it finishes. One pass moves the kept calls forward and one erase drops the rest, so a large
    // call that pushes out thousands of small ones costs one walk of the journal under the lock.
    std::size_t count = m_entries.size();
    if (count <= kMaxEntries && m_bytes <= kMaxArgBytes)
        return;
    auto kept = m_entries.begin();
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
        if (!it->running() && (count > kMaxEntries || m_bytes > kMaxArgBytes)) {
            --count;
            m_bytes -= it->bytes;
            continue;
        }
        if (kept != it)
            *kept = std::move(*it);
        ++kept;
    }
    m_entries.erase(kept, m_entries.end());
}

nlohmann::json Journal::to_replay(const std::string& name, nlohmann::json* left_out_running) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    nlohmann::json                     steps = nlohmann::json::array();
    std::map<std::string, std::string> job_names;  // live job id -> replay name
    std::map<std::string, std::string> recordings; // a recording's path_compare_key() -> its path in the replay
    if (left_out_running != nullptr)
        *left_out_running = nlohmann::json::array();
    // Whether the next call in the replay is `e` again, choosing an item of the menu it listed.
    auto repeated_with_item = [this](std::vector<Entry>::const_iterator e) {
        const auto next = std::find_if(std::next(e), m_entries.end(), [](const Entry& n) { return n.kept(); });
        if (next == m_entries.end() || next->tool != e->tool || !next->args.is_object() || !next->args.contains("popup_item"))
            return false;
        nlohmann::json args = next->args;
        args.erase("popup_item");
        return args == e->args;
    };
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
        const Entry& e = *it;
        if (!e.kept()) {
            if (!e.failed && left_out_running != nullptr) {
                nlohmann::json call{{"tool", e.tool}};
                if (!e.job.empty())
                    call["job"] = e.job;
                left_out_running->push_back(std::move(call));
            }
            continue;
        }
        if (e.menu_listed && repeated_with_item(it))
            continue;
        nlohmann::json step{{"tool", e.tool}, {"args", e.args}};
        if (e.tool == "wait" && e.args.value("for", "") == "job") {
            auto it = job_names.find(e.args.value("job", ""));
            if (it == job_names.end())
                continue; // waits on a job the journal did not see start
            step["args"]["job"] = "${jobs." + it->second + "}";
        }
        auto in_out_dir = [&](const char* key) {
            const std::string file = path_to_utf8(path_from_utf8(e.args[key].get<std::string>()).filename());
            return "${out_dir}/" + file;
        };
        auto has_path = [&](const char* key) { return e.args.contains(key) && e.args[key].is_string(); };
        if ((e.tool == "screenshot" || e.tool == "encode_recording") && has_path("save_path"))
            step["args"]["save_path"] = in_out_dir("save_path");
        if (e.tool == "start_recording" && has_path("save_path")) {
            step["args"]["save_path"]                                = in_out_dir("save_path");
            recordings[path_compare_key(path_from_utf8(e.args["save_path"].get<std::string>()))] = step["args"]["save_path"];
        }
        // An encode of a recording this replay makes reads it where the replay writes it; any other
        // source stays where it is.
        if (e.tool == "encode_recording" && has_path("path"))
            if (auto it = recordings.find(path_compare_key(path_from_utf8(e.args["path"].get<std::string>()))); it != recordings.end())
                step["args"]["path"] = it->second;
        if (e.unawaited()) {
            const std::string job_name = "job" + std::to_string(job_names.size() + 1);
            job_names[e.job]           = job_name;
            step["await"]              = false;
            step["save_as"]            = job_name;
        }
        if (e.answers_dialog)
            steps.push_back({{"tool", "wait"}, {"args", {{"for", "dialog"}, {"timeout_s", 50}}}, {"expect", {{"/satisfied", {{"eq", true}}}}}});
        steps.push_back(std::move(step));
    }
    return {{"orca_replay", 1}, {"name", name}, {"steps", std::move(steps)}};
}

}}} // namespace Slic3r::GUI::Mcp
