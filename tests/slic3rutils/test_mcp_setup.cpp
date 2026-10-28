#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/Mcp/McpSetup.hpp"

#include <nlohmann/json.hpp>

#include <string>

using namespace Slic3r::GUI::Mcp;
using nlohmann::json;

namespace {

const std::string kUrl   = "http://127.0.0.1:13619/mcp";
const std::string kToken = "t0ken";
// Backslashes, a space and a quote, which each format has to escape or keep.
const std::string kExe = R"(C:\Program Files\Orca's\orca-slicer.exe)";

std::string setup(Agent agent, Transport transport) { return agent_setup(agent, transport, kUrl, kToken, kExe); }

} // namespace

TEST_CASE("an HTTP setup holds the server's address and token", "[Mcp]")
{
    CHECK(setup(Agent::ClaudeCode, Transport::Http) ==
          R"(claude mcp add --transport http --scope user orcaslicer http://127.0.0.1:13619/mcp --header "Authorization: Bearer t0ken")");
    CHECK(setup(Agent::Codex, Transport::Http) == "[mcp_servers.orcaslicer]\n"
                                                  "url = \"http://127.0.0.1:13619/mcp\"\n"
                                                  "http_headers = { \"Authorization\" = \"Bearer t0ken\" }\n");
    CHECK(json::parse(setup(Agent::OpenCode, Transport::Http)) ==
          json{{"mcp", {{"orcaslicer", {{"type", "remote"}, {"url", kUrl}, {"headers", {{"Authorization", "Bearer t0ken"}}}}}}}});
    CHECK(setup(Agent::DeepSeekHarness, Transport::Http) == "- insert:\n"
                                                            "    - id: mcp-orcaslicer\n"
                                                            "      name: '@deepseek-ai/dsh-mcp-client'\n"
                                                            "      config:\n"
                                                            "        serverName: orcaslicer\n"
                                                            "        transport: streamable-http\n"
                                                            "        url: 'http://127.0.0.1:13619/mcp'\n"
                                                            "        headers:\n"
                                                            "          Authorization: 'Bearer t0ken'\n");
    CHECK(json::parse(setup(Agent::OtherApps, Transport::Http)) ==
          json{{"mcpServers", {{"orcaslicer", {{"type", "http"}, {"url", kUrl}, {"headers", {{"Authorization", "Bearer t0ken"}}}}}}}});
}

TEST_CASE("a stdio setup runs the executable with --mcp-stdio", "[Mcp]")
{
    CHECK(setup(Agent::ClaudeCode, Transport::Stdio) ==
          R"(claude mcp add --scope user orcaslicer -- "C:\Program Files\Orca's\orca-slicer.exe" --mcp-stdio)");
    CHECK(setup(Agent::Codex, Transport::Stdio) == "[mcp_servers.orcaslicer]\n"
                                                   R"(command = "C:\\Program Files\\Orca's\\orca-slicer.exe")"
                                                   "\n"
                                                   "args = [\"--mcp-stdio\"]\n");
    CHECK(json::parse(setup(Agent::OpenCode, Transport::Stdio)) ==
          json{{"mcp", {{"orcaslicer", {{"type", "local"}, {"command", json::array({kExe, "--mcp-stdio"})}}}}}});
    CHECK(setup(Agent::DeepSeekHarness, Transport::Stdio) == "- insert:\n"
                                                             "    - id: mcp-orcaslicer\n"
                                                             "      name: '@deepseek-ai/dsh-mcp-client'\n"
                                                             "      config:\n"
                                                             "        serverName: orcaslicer\n"
                                                             "        transport: stdio\n"
                                                             R"(        command: 'C:\Program Files\Orca''s\orca-slicer.exe')"
                                                             "\n"
                                                             "        args: ['--mcp-stdio']\n");
    CHECK(json::parse(setup(Agent::OtherApps, Transport::Stdio)) ==
          json{{"mcpServers", {{"orcaslicer", {{"command", kExe}, {"args", json::array({"--mcp-stdio"})}}}}}});

    // The bridge reads the token from the discovery file, so a stdio setup never carries it.
    for (const Agent agent : {Agent::ClaudeCode, Agent::Codex, Agent::OpenCode, Agent::DeepSeekHarness, Agent::OtherApps})
        CHECK(setup(agent, Transport::Stdio).find(kToken) == std::string::npos);
}
