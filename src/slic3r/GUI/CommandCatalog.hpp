#pragma once

#include "Mcp/McpTypes.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// The command type and the catalog that holds every command. No wx here, so that the wx-free units of
// the MCP server can include it.
namespace Slic3r { namespace GUI {

// The group of the Design tab's commands (Mcp::register_cad_commands()), an untranslated msgid as every
// group is. Their tiles are the Speed Dial's only while the Design tab is shown.
inline constexpr const char* kDesignGroup = "Design";

// Commands whose effect finishes after they return. Export: the G-code export runs in the background
// process, so a call that gives the file waits for it.
enum class CommandCompletion { None, Export };

// A command's function: the arguments match `input_schema`; the context is the call's, a person's pick
// (ToolContext::person()) or an automated call.
using CommandFn = std::function<Mcp::ToolResult(const nlohmann::json& args, Mcp::ToolContext& ctx)>;

// One command of the app: identity, what a person sees of it, what an agent reads about it and how to
// run it. NativeCommands::make_action() wraps one as a thin AppAction for the Speed Dial, so the catalog
// is the single source of truth for the behaviour (`run`, which the Speed Dial, a menu and every automated
// caller call), the presentation (title/group/input/icon) and what automation needs to call it.
struct NativeCommand
{
    std::string key;
    std::string title; // untranslated English msgid; make_action() translates it for display
    // A tile's section, an untranslated msgid as the title is. A typed tool without a tile is in the group of the way it
    // acts, which search_tools filters by: core runs the session, recovers from dialogs and runs Python, api acts on
    // project data by index and name with JSON in and out, and ui drives the GUI itself.
    std::string group;
    // "percent"/"tab": the palette's second phase, which collects the value the command needs before it
    // runs; "" to run at once.
    std::string input;
    std::string icon;  // tile pictogram: an SVG base name under resources/images, or "" for a blank tile
    std::string       description; // English, for agents; not shown in the palette
    CommandCompletion completion = CommandCompletion::None;
    // The command's function. With no arguments it does what a person's pick does; with arguments that a
    // dialog would otherwise ask for, it runs without asking.
    CommandFn      run;
    nlohmann::json input_schema = {{"type", "object"}, {"properties", nlohmann::json::object()}};
    // Calls that document the command: [{title, calls: [{tool, args}], needs?}], or null.
    nlohmann::json examples;
    bool listed     = false; // shown in tools/list: a typed tool
    bool in_palette = false; // a person can pick it: it has a tile
    // The access labels sent as MCP's annotations. Clients act on them: Codex asks a person before every call
    // of a destructive command, and Claude Code runs read-only calls in parallel and retries idempotent ones.
    // read_only: it changes nothing in the app or the project; screenshot is, since its PNG replaces no file
    // without overwrite, and export_replay, which can clear the journal, is not. destructive: every call deletes
    // what a person made, with no undo: delete_preset. A command that can lose work only when an argument asks
    // for it is not: new_project, generate_calibration_test and open_files drop unsaved changes only with
    // discard_changes, an output replaces a file only with overwrite, and encode_recording deletes its source
    // only with delete_source. Nor is a command that runs what its arguments name (click and the other input
    // tools, run_action, call_tool, run_replay, run_script), though what it runs may be. The client sees both in
    // the arguments, and the label would ask on every call. open_world: it can reach outside OrcaSlicer (a
    // printer, the cloud account, the network, or anything the computer allows), including through what it runs.
    bool            read_only = false, destructive = false, idempotent = false, open_world = false;
    Mcp::GuardClass guard     = Mcp::GuardClass::Mutate;
    // inline_call: pure observation, which runs on the request thread and is never a job; everything that can
    // activate UI runs as a job instead, because activating UI can open a modal that blocks the call. journal:
    // recorded by the session journal for export_replay; observation commands turn it off, except wait and
    // screenshot, which a replay needs.
    bool inline_call = false, journal = true;
    // allowed_during_operation: may run while an MCP slice/export operation holds the project (cancel_job).
    // cancellable: `cancel_job {job}` stops a running call that is not an operation (encode_recording); the
    // function sees ToolContext::cancelled() and ends.
    bool allowed_during_operation = false, cancellable = false;
    // How long a call may run before the reply returns a job envelope and the call continues.
    int budget_ms = 45000;
    // The arguments that carry an expected object or part id, which the session journal leaves out.
    std::vector<std::string> id_args;
    // The operations (GUI/Operations) the command reaches when it is given arguments, by function name. A
    // palette command that takes arguments names them or is on the exception list of the catalog test.
    std::vector<std::string> operations;
};

// The context a command's title is translated in, or nullptr for none: "Design" for a Design command, whose
// title is its entry's name, which the Design table marks in that context for every entry (DesignOffer.hpp), with
// a menu row or without, the context the Design tab's right-click menu translates a row's name in. Every place
// that shows a command's title translates it with this (NativeCommands::tile_title(), a recording's caption).
inline const char* title_context(const NativeCommand& command)
{
    return command.group == kDesignGroup ? "Design" : nullptr;
}

// What the reference says of a command: key, title, group, description, input_schema, examples when it has
// some, listed, in_palette, the four access labels, guard ("inspect", "recover", "mutate"), completion
// ("none", "export") and operations. All of it is the command's English text and constants, never translated.
nlohmann::json command_reference(const NativeCommand& command);

// Commands that come and go while the app runs (plugin operations). Any thread.
class CommandSource
{
public:
    virtual ~CommandSource() = default;
    virtual std::shared_ptr<const NativeCommand>              find(const std::string& key) const = 0;
    virtual std::vector<std::shared_ptr<const NativeCommand>> all() const                        = 0;
};

// Every command of the process: those added once while it is built, then sealed, and those its sources
// hold. Built on one thread; once sealed it never changes and is read on any thread.
class CommandCatalog
{
public:
    // Before seal(). A key added twice, or an addition after seal(), throws std::logic_error.
    void add(NativeCommand command);
    // Before seal(); after it, throws std::logic_error. A source's commands are not checked against the
    // added ones: lookup() finds an added command first.
    void add_source(std::unique_ptr<CommandSource> source);
    void seal();
    bool sealed() const { return m_sealed; }

    // A command added with add(), or nullptr. The pointer stays valid while the catalog lives once it is
    // sealed.
    const NativeCommand* find(const std::string& key) const;
    // That, as a pointer that does not own it, else the first source's that holds `key`, which keeps it
    // alive through a call; nullptr when neither has it.
    std::shared_ptr<const NativeCommand> lookup(const std::string& key) const;
    // The commands added with add(), in the order they were added.
    const std::vector<NativeCommand>& commands() const { return m_commands; }
    // What the sources hold now, source by source.
    std::vector<std::shared_ptr<const NativeCommand>> source_commands() const;
    // Up to `limit` keys of added commands closest to `key`, for "did you mean" errors: a key that
    // contains `key` first, then by edit distance, none further than max(3, half the length of `key`).
    std::vector<std::string> similar(const std::string& key, std::size_t limit = 3) const;

    // The error for a key lookup() does not resolve: "Unknown tool: <key>", with `suggest` followed by the
    // closest key.
    std::string unknown_command(const std::string& key, bool suggest = false) const;

private:
    std::vector<NativeCommand>                   m_commands;
    std::unordered_map<std::string, std::size_t> m_index; // key -> position in m_commands
    std::vector<std::unique_ptr<CommandSource>>  m_sources;
    bool                                         m_sealed = false;
};

// The reference of `catalog`: {"commands": [command_reference(), ...]} of the commands added with add(), in
// the order they were added. Its sources' commands are left out: they are a plugin's, not the build's.
// tests/data/command_catalog.json is this reference of the catalog a run with the Design tab holds, and the
// skill's list of the Speed Dial's commands is generated from that file (tools/mcp/gen_skill_reference.py).
nlohmann::json catalog_reference(const CommandCatalog& catalog);

}} // namespace Slic3r::GUI
