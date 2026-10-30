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
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

uint64_t Journal::begin(const std::string& tool, const nlohmann::json& args, bool answers_dialog)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (answers_dialog)
        for (Entry& running : m_entries)
            if (!running.job.empty() && !running.job_ended)
                running.dialog_answered = true;
    Entry e;
    e.seq            = m_next++;
    e.tool           = tool;
    e.args           = args;
    e.answers_dialog = answers_dialog;
    m_entries.push_back(std::move(e));
    // A session kept open for days must not grow without bound; a script is exported from the
    // recent calls anyway (export_script {clear:true} starts afresh). The oldest call whose outcome
    // is known goes; a call still waiting for its reply or its job keeps its place.
    if (m_entries.size() > kMaxEntries) {
        const auto oldest = std::find_if(m_entries.begin(), m_entries.end(), [](const Entry& entry) { return !entry.running(); });
        if (oldest != m_entries.end())
            m_entries.erase(oldest);
    }
    return m_entries.back().seq;
}

void Journal::set_args(uint64_t seq, nlohmann::json args)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (Entry& e : m_entries)
        if (e.seq == seq)
            e.args = std::move(args);
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
}

void Journal::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.clear();
}

nlohmann::json Journal::to_script(const std::string& name, nlohmann::json* left_out_running) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    nlohmann::json                     steps = nlohmann::json::array();
    std::map<std::string, std::string> job_names;  // live job id -> script name
    std::map<std::string, std::string> recordings; // a recording's path_compare_key() -> its path in the script
    if (left_out_running != nullptr)
        *left_out_running = nlohmann::json::array();
    // Whether the next call in the script is `e` again, choosing an item of the menu it listed.
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
        // An encode of a recording this script makes reads it where the replay writes it; any other
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
    return {{"orca_script", 1}, {"name", name}, {"steps", std::move(steps)}};
}

}}} // namespace Slic3r::GUI::Mcp
