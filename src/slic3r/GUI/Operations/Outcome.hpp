#pragma once

#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <wx/string.h>

// The operations: what the Speed Dial's commands, the MCP tools and the plugin API (orca.host) each
// run by name, one function per thing the app can be asked to do. An operation holds the checks, the
// refusals and their wording, the undo snapshot and the sequence of owner calls; the three callers
// convert their arguments and its result around it. docs/HLSD/mcp-server.md, "Operations", has the
// rules: what is an operation and what stays a method of its owner, and what an operation may include.
//
// Every operation runs on the UI thread and returns at once. One whose work goes on in the background
// returns a ticket, and a status function reads how that work stands.
namespace Slic3r { namespace GUI { namespace Ops {

// Precondition: the caller can change something and call again. Busy: the same call works later.
// Failed: it was tried and did not happen.
enum class RefusalKind { Precondition, Busy, Failed };

// Why an operation did not run, in one wording for every caller.
struct Refusal
{
    RefusalKind              kind = RefusalKind::Precondition;
    const char*              code = "";  // stable id: "slicing_in_progress"
    std::string              msgid;      // English, marked with L() where it is written; %1% placeholders
    std::vector<std::string> args;       // the placeholders' values
    std::vector<std::string> names;      // names that help the caller: candidates, closest matches

    // In the UI's language, for the palette and notifications.
    wxString translated() const;
    // For a tool's reply and a Python exception.
    std::string english() const;
};

// A value or a Refusal, never both.
template<class T> class Outcome
{
public:
    Outcome(T value) : m_state(std::in_place_index<0>, std::move(value)) {}
    Outcome(Refusal why) : m_state(std::in_place_index<1>, std::move(why)) {}

    explicit operator bool() const { return m_state.index() == 0; }
    const T&       value() const { return std::get<0>(m_state); }
    const Refusal& refusal() const { return std::get<1>(m_state); }

private:
    std::variant<T, Refusal> m_state;
};

// What an operation with nothing to report returns.
struct Done
{};

}}} // namespace Slic3r::GUI::Ops
