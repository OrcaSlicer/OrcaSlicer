#pragma once

#include <string>
#include <map>
#include <functional>
#include "nlohmann/json.hpp"
#include "MCP/McpApiServer.h"
#include "MCP/McpProtocol.h"

namespace Slic3r { namespace GUI {

class CommandDispatch {
public:
    using json = nlohmann::json;
    using handler_fn = std::function<json(const json& params)>;

    static CommandDispatch& instance();

    void init();

    McpApiServer::Response handle_api_request(
        const std::string& method, const std::string& url, const std::string& body,
        const std::map<std::string, std::string>& headers);

    json dispatch(const std::string& action, const json& params);

    McpApiServer::Response handle_screenshot(const std::string& url);

private:
    CommandDispatch() = default;

    McpProtocol m_mcp_protocol;
    std::map<std::string, handler_fn> m_commands;

    void register_command(const std::string& action, handler_fn fn);

    json call_on_gui_thread(std::function<json()> fn);

    void register_model_commands();
    void register_config_commands();
    void register_diagnostics_commands();
    void register_viewport_commands();
};

// Unwrap the "settings" wrapper from config_set tool arguments.
// The MCP tool schema sends {"settings": {"key": "value"}}, but the handler
// needs to iterate over the inner {"key": "value"} map directly.
inline const nlohmann::json& unwrap_config_settings(const nlohmann::json& params) {
    if (params.contains("settings") && params["settings"].is_object())
        return params["settings"];
    return params;
}

}} // namespace Slic3r::GUI
