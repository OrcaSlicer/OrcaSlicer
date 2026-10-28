#include "McpJournal.hpp"
#include "McpTypes.hpp"

#include <boost/filesystem/path.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace Slic3r { namespace GUI { namespace Mcp {

uint64_t Journal::begin(const std::string& tool, const nlohmann::json& args)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    Entry e;
    e.seq  = m_next++;
    e.tool = tool;
    e.args = args;
    m_entries.push_back(std::move(e));
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
            e.finished = true;
            e.failed   = result.is_error;
            if (!result.is_error && result.data.is_object() && result.data.contains("job") && result.data.contains("status"))
                e.job = result.data["job"].get<std::string>();
        }
}

void Journal::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.clear();
}

nlohmann::json Journal::to_script(const std::string& name) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    nlohmann::json                     steps = nlohmann::json::array();
    std::map<std::string, std::string> job_names; // live job id -> script name
    for (const Entry& e : m_entries) {
        if (e.failed)
            continue;
        nlohmann::json step{{"tool", e.tool}, {"args", e.args}};
        if (e.tool == "wait" && e.args.value("for", "") == "job") {
            auto it = job_names.find(e.args.value("job", ""));
            if (it == job_names.end())
                continue; // waits on a job the journal did not see start
            step["args"]["job"] = "${jobs." + it->second + "}";
        }
        if (e.tool == "screenshot" && e.args.contains("save_path") && e.args["save_path"].is_string()) {
            const std::string file = boost::filesystem::path(e.args["save_path"].get<std::string>()).filename().string();
            step["args"]["save_path"] = "${out_dir}/" + file;
        }
        if (!e.job.empty()) {
            const std::string job_name = "job" + std::to_string(job_names.size() + 1);
            job_names[e.job]           = job_name;
            step["await"]              = false;
            step["save_as"]            = job_name;
        }
        steps.push_back(std::move(step));
    }
    return {{"orca_script", 1}, {"name", name}, {"steps", std::move(steps)}};
}

}}} // namespace Slic3r::GUI::Mcp
