#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r {

struct AgentCliResult
{
    int exit_code = 0;
    bool forward_to_legacy = false;
    std::vector<std::string> legacy_args;
    std::string stdout_text;
};

bool agent_invocation(int argc, char **argv);
AgentCliResult agent_cli_run(int argc, char **argv);

nlohmann::json agent_help_json();
nlohmann::json agent_search_json(const std::string &group, const std::string &query, const std::string &category, int limit, int offset);
nlohmann::json agent_describe_json(const std::string &id);
AgentCliResult agent_invoke_json(const nlohmann::json &recipe);

}
