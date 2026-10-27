#pragma once

#include "McpTypes.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

// The session's client calls, in the order they were made, for export_script. A call that returned
// a job envelope is replayed with `await:false` and a name its later `wait` refers to, so a step
// that opened a dialog can be followed by the steps that answered it. Thread-safe.
class Journal
{
public:
    // Starts an entry; returns its sequence number. Calls made by scripts are not recorded.
    uint64_t begin(const std::string& tool, const nlohmann::json& args);
    void     set_args(uint64_t seq, nlohmann::json args);
    void     end(uint64_t seq, const ToolResult& result);
    void     clear();

    // {"orca_script":1, "name":..., "steps":[...]} with job names substituted and screenshot paths
    // rewritten to ${out_dir}/<file name>.
    nlohmann::json to_script(const std::string& name) const;

private:
    struct Entry
    {
        uint64_t       seq = 0;
        std::string    tool;
        nlohmann::json args;
        bool           finished = false;
        bool           failed   = false;
        std::string    job; // set when the reply was a job envelope
    };

    mutable std::mutex m_mutex;
    std::vector<Entry> m_entries;
    uint64_t           m_next = 1;
};

}}} // namespace Slic3r::GUI::Mcp
