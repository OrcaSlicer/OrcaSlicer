#pragma once

#include <string>

namespace Slic3r { namespace GUI { namespace Mcp {

// The agents Preferences copies a setup for, in its picker's order.
enum class Agent { ClaudeCode, Codex, OpenCode, DeepSeekHarness, OtherApps };

// How a setup connects the agent, in Preferences' order. HTTP reaches the running server at an
// address that stays the same whichever OrcaSlicer build runs, so it is the default. Stdio runs
// the executable with --mcp-stdio, which relays to the server and starts OrcaSlicer when none is
// running, but breaks when the program moves; it also serves apps that only run servers as commands.
enum class Transport { Http, Stdio };

// The text that adds OrcaSlicer to `agent`, in the form the agent takes it: a command to run or an
// entry for its configuration file. An HTTP setup holds the server's `url` and `token`; a stdio
// setup holds the executable `exe` (UTF-8) and no token, since --mcp-stdio reads it from the
// discovery file.
std::string agent_setup(Agent agent, Transport transport, const std::string& url, const std::string& token, const std::string& exe);

}}} // namespace Slic3r::GUI::Mcp
