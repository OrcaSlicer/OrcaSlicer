#ifndef slic3r_GUI_McpControl_hpp_
#define slic3r_GUI_McpControl_hpp_

// MCP control surface (slice 1): a local JSON-RPC 2.0 server, line-delimited over a
// Unix domain socket, that lets an external MCP bridge drive and perceive the Design
// tab. Off unless the env var SNAPORCA_MCP is set:
//   SNAPORCA_MCP=1                -> socket at /tmp/snaporca-mcp.sock
//   SNAPORCA_MCP=/path/to.sock    -> socket at that path
// All CAD work is marshalled onto the wx main thread and runs through the SAME
// CadDocument kernel the GUI uses (no parallel engine). Slice-1 methods:
//   describe_tools, describe_scene, extrude.
//
// ponytail: Unix-socket only (POSIX). Windows compiles this to a no-op; add a named
// pipe transport when a Windows agent actually needs it.

namespace Slic3r { namespace GUI {

// Start the server thread iff SNAPORCA_MCP is set. Safe to call once after the
// MainFrame + DesignPanel exist. No-op when the env var is unset or on Windows.
void start_mcp_control_if_enabled();

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_McpControl_hpp_
