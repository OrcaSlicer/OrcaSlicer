#include "McpJournal.hpp"
#include "McpArguments.hpp"
#include "McpPaths.hpp"
#include "McpReplay.hpp"

#include "slic3r/GUI/ActionIds.hpp"
#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/CommandTypes.hpp"

#include <boost/filesystem/path.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;

namespace {

// The size a call's arguments count against the journal's byte bound. Invalid UTF-8 a tool recorded is
// counted as replacement characters rather than thrown on: the journal only needs the size.
std::size_t serialized_size(const json& args) { return args.dump(-1, ' ', false, json::error_handler_t::replace).size(); }

// The name a journaled call of `tool` with `args` runs under, and the arguments it runs with: the call's own, or, for
// action_run of a command's action, by its full id or the key inside it, that key and the arguments inside the action's
// {id, args}. A command's own action is under its command's key, so a Design command's action is rewritten as its
// command is; the action of a menu item folded into a command is under a key of its own, and its call's arguments, none,
// the action's own or the value it collects, hold no path to rewrite.
std::pair<std::string, json*> called_command(const std::string& tool, json& args)
{
    if (tool != "action_run" || !args.is_object() || !args.value("id", json()).is_string() || !args.value("args", json()).is_object())
        return {tool, &args};
    const std::string key = command_action_key(args["id"].get<std::string>());
    if (key.empty())
        return {tool, &args};
    return {key, &args["args"]};
}

} // namespace

json without_guard_ids(const CommandCatalog& catalog, const std::string& command, json args)
{
    if (!args.is_object())
        return args;
    if (command == "tool_call") {
        if (args.contains("args") && args["args"].is_object())
            args["args"] = without_guard_ids(catalog, args.value("tool", std::string()), std::move(args["args"]));
        return args;
    }
    if (command == "action_run") {
        if (const auto [key, inner] = called_command(command, args); inner != &args)
            *inner = without_guard_ids(catalog, key, std::move(*inner));
        return args;
    }
    if (command == "replay_run") {
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
                    step["args"] = without_guard_ids(catalog, step.value("tool", std::string()), std::move(step["args"]));
        return args;
    }
    if (const std::shared_ptr<const NativeCommand> def = catalog.lookup(command))
        for (const std::string& key : def->id_args)
            if (const char* index = index_argument(key); index != nullptr && args.contains(index))
                args.erase(key);
    return args;
}

uint64_t Journal::begin(const std::string& tool, const nlohmann::json& args, bool answers_dialog)
{
    // The copy and its size are made before the lock: serializing a large call takes a while, and
    // every other call's begin and end waits on the lock.
    Entry e;
    e.tool           = tool;
    e.args           = args;
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
    // As in begin(), the size is measured outside the lock.
    const std::size_t bytes = serialized_size(args);

    std::lock_guard<std::mutex> lock(m_mutex);
    // replay_export {clear:true} may have emptied the journal meanwhile.
    const auto it = std::find_if(m_entries.begin(), m_entries.end(), [seq](const Entry& e) { return e.seq == seq; });
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
            // The envelope names the call's own job; an `app_wait` that timed out names another one.
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
    // A session kept open for days, or one that sends thousands of large calls (a CAD run's sketch_draw),
    // must not grow without bound; a replay is exported from the recent calls anyway (replay_export
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
        if (e.tool == "app_wait" && e.args.value("for", "") == "job") {
            auto it = job_names.find(e.args.value("job", ""));
            if (it == job_names.end())
                continue; // waits on a job the journal did not see start
            step["args"]["job"] = "${jobs." + it->second + "}";
        }
        // The name the step runs under and its arguments: action_run of a command's action runs as the action's key, with
        // the arguments inside the action's {id, args}, which the step keeps, so that its replay runs the action again.
        const auto [tool, called_args] = called_command(e.tool, step["args"]);
        nlohmann::json& args           = *called_args;
        auto in_out_dir = [&](const char* key) {
            const std::string file = path_to_utf8(path_from_utf8(args[key].get<std::string>()).filename());
            return "${out_dir}/" + file;
        };
        auto has_path = [&](const char* key) { return args.contains(key) && args[key].is_string(); };
        if ((tool == "window_screenshot" || tool == "recording_encode") && has_path("save_path"))
            args["save_path"] = in_out_dir("save_path");
        if (tool == "recording_start" && has_path("save_path")) {
            const std::string key = path_compare_key(path_from_utf8(args["save_path"].get<std::string>()));
            args["save_path"]     = in_out_dir("save_path");
            recordings[key]       = args["save_path"];
        }
        // An encode of a recording this replay makes reads it where the replay writes it; any other
        // source stays where it is.
        if (tool == "recording_encode" && has_path("path"))
            if (auto it = recordings.find(path_compare_key(path_from_utf8(args["path"].get<std::string>()))); it != recordings.end())
                args["path"] = it->second;
        if (e.unawaited()) {
            const std::string job_name = "job" + std::to_string(job_names.size() + 1);
            job_names[e.job]           = job_name;
            step["await"]              = false;
            step["save_as"]            = job_name;
        }
        if (e.answers_dialog)
            steps.push_back({{"tool", "app_wait"}, {"args", {{"for", "dialog"}, {"timeout_s", 50}}}, {"expect", {{"/satisfied", {{"eq", true}}}}}});
        steps.push_back(std::move(step));
    }
    return {{"orca_replay_version", REPLAY_VERSION}, {"name", name}, {"steps", std::move(steps)}};
}

}}} // namespace Slic3r::GUI::Mcp
