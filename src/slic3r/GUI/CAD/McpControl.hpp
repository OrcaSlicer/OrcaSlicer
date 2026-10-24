#ifndef slic3r_GUI_McpControl_hpp_
#define slic3r_GUI_McpControl_hpp_

// MCP control surface: a local JSON-RPC 2.0 server, line-delimited over a Unix domain
// socket, that lets an external MCP bridge drive and perceive the Design tab. On whenever the
// CAD feature is enabled in Preferences; the env var ORCA_CAD_MCP selects the path or opts out:
//   unset, empty or 1             -> socket at /tmp/orca-cad-mcp.sock
//   ORCA_CAD_MCP=0 | off          -> no socket
//   ORCA_CAD_MCP=/path/to.sock    -> socket at that path (an existing non-socket file there is
//                                    left alone and the server does not start; a socket that
//                                    still answers is never taken over)
// The socket is created 0600 and removed at exit. All CAD work is marshalled onto the wx main
// thread and runs through the SAME CadDocument kernel the GUI uses (no parallel engine);
// describe_tools lists the methods. A method that changes the document is refused (-32002)
// while the Design tab is busy with it — a rebuild, an open feature card, a sketch session.
//
// ponytail: Unix-socket only (POSIX). Windows compiles this to a no-op; add a named
// pipe transport when a Windows agent actually needs it.

namespace Slic3r { namespace GUI {

// Start the server thread unless ORCA_CAD_MCP is 0/off. Safe to call once the MainFrame exists —
// the Design panel is built on the first request. No-op when opted out or on Windows.
void start_mcp_control_if_enabled();

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_McpControl_hpp_
