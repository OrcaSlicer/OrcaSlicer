#include "McpSetup.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace Slic3r { namespace GUI { namespace Mcp {

std::string agent_setup(Agent agent, Transport transport, const std::string& url, const std::string& token, const std::string& exe)
{
    using nlohmann::ordered_json;
    const bool        http   = transport == Transport::Http;
    const std::string bearer = "Bearer " + token;
    switch (agent) {
    case Agent::ClaudeCode:
        return http ? "claude mcp add --transport http --scope user orcaslicer " + url + " --header \"Authorization: " + bearer + "\"" :
                      "claude mcp add --scope user orcaslicer -- \"" + exe + "\" --mcp-stdio";
    case Agent::Codex: // a table for ~/.codex/config.toml: Codex's command line takes a token only from an environment variable
        return "[mcp_servers.orcaslicer]\n" +
               (http ? "url = \"" + url + "\"\nhttp_headers = { \"Authorization\" = \"" + bearer + "\" }\n" :
                       // A JSON string is also a TOML basic string, with the path's backslashes escaped.
                       "command = " + ordered_json(exe).dump() + "\nargs = [\"--mcp-stdio\"]\n");
    case Agent::OpenCode:
        return ordered_json{{"mcp", {{"orcaslicer", http ? ordered_json{{"type", "remote"}, {"url", url}, {"headers", {{"Authorization", bearer}}}} :
                                                           ordered_json{{"type", "local"}, {"command", ordered_json::array({exe, "--mcp-stdio"})}}}}}}
            .dump(2);
    case Agent::DeepSeekHarness: { // an insert row for ~/.dsh/cordis.patch.yml, which every profile applies
        std::string quoted_exe; // a single-quoted YAML scalar, which doubles its quotes and keeps backslashes
        for (const char c : exe)
            quoted_exe += c == '\'' ? std::string("''") : std::string(1, c);
        return "- insert:\n"
               "    - id: mcp-orcaslicer\n"
               "      name: '@deepseek-ai/dsh-mcp-client'\n"
               "      config:\n"
               "        serverName: orcaslicer\n" +
               (http ? "        transport: streamable-http\n"
                       "        url: '" + url + "'\n"
                       "        headers:\n"
                       "          Authorization: '" + bearer + "'\n" :
                       "        transport: stdio\n"
                       "        command: '" + quoted_exe + "'\n"
                       "        args: ['--mcp-stdio']\n");
    }
    case Agent::OtherApps: // the mcpServers shape most apps read
        return ordered_json{{"mcpServers", {{"orcaslicer", http ? ordered_json{{"type", "http"}, {"url", url}, {"headers", {{"Authorization", bearer}}}} :
                                                                  ordered_json{{"command", exe}, {"args", ordered_json::array({"--mcp-stdio"})}}}}}}
            .dump(2);
    }
    return {};
}

}}} // namespace Slic3r::GUI::Mcp
