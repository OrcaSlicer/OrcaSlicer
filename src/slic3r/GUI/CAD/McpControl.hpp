#ifndef slic3r_GUI_McpControl_hpp_
#define slic3r_GUI_McpControl_hpp_

// MCP control surface: a local JSON-RPC 2.0 server, line-delimited over a Unix domain
// socket, that lets an external MCP bridge drive and perceive the Design tab. Off unless the
// CAD feature is enabled in Preferences AND the env var ORCA_CAD_MCP is set:
//   ORCA_CAD_MCP=1                -> socket at /tmp/orca-cad-mcp.sock
//   ORCA_CAD_MCP=/path/to.sock    -> socket at that path (an existing non-socket file there is
//                                    left alone and the server does not start)
// The socket is created 0600 and removed at exit. All CAD work is marshalled onto the wx main
// thread and runs through the SAME CadDocument kernel the GUI uses (no parallel engine);
// describe_tools lists the methods. A method that changes the document is refused (-32002)
// while the Design tab is busy with it — a rebuild, an open feature card, a sketch session.
//
// The socket is POSIX only; on Windows the same methods are reached through the app's MCP server
// (its cad_call tool), which calls cad_control_call().

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>

namespace Slic3r { namespace GUI {

// Start the server thread iff ORCA_CAD_MCP is set. Safe to call once the MainFrame exists —
// the Design panel is built on the first request. No-op when the env var is unset or on Windows.
void start_mcp_control_if_enabled();

// A control-surface method's JSON-RPC error.
struct CadCallError : std::runtime_error
{
    int code;
    CadCallError(int code, const std::string& message) : std::runtime_error(message), code(code) {}
};

// Runs one control-surface method (describe_tools lists them) with the socket's busy and stale-id
// checks and returns its result. Throws CadCallError. UI thread only.
nlohmann::json cad_control_call(const std::string& method, const nlohmann::json& params);

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_McpControl_hpp_
