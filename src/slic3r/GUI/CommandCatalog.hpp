#pragma once

#include "CommandTypes.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// The command type and the catalog that holds every command. No wx here: this header is part of the library
// libslic3r_automation, which is built without wx.
namespace Slic3r { namespace GUI {

// The group of the Design tab's commands (Mcp::register_design_tools()), an untranslated msgid as every
// group is. Their actions are the Speed Dial's only while the Design tab is shown.
inline constexpr const char* kDesignGroup = "Design";

// When a command's effect finishes. Immediate: it has finished by the time the call returns. Deferred: it
// lands later, when the background work it starts (a G-code export today) is done, and a call that gave the
// file waits for it.
enum class CommandCompletion { Immediate, Deferred };

// A command's function: the arguments match `input_schema`; the context is the call's, a person's pick
// (ToolContext::person()) or an automated call.
using CommandFn = std::function<ToolResult(const nlohmann::json& args, ToolContext& ctx)>;

// One command of the app: identity, what a person sees of it, what an agent reads about it and how to
// run it. NativeCommands::make_action() wraps one as a thin AppAction for the Speed Dial, so the catalog
// is the single source of truth for the behaviour (`run`, which the Speed Dial, a menu and every automated
// caller call), the presentation (title/group/input/icon) and what automation needs to call it.
struct NativeCommand
{
    std::string key;
    std::string title; // untranslated English msgid; make_action() translates it for display
    // The group tool_list lists it under. Outside the Design tab, the English heading of the command's place
    // (NativeCommands::place_heading()): Objects, Plates, The app, the place its name starts with but for the two
    // screenshot commands, which are under Recordings, replays and scripts; a plugin's operation is in Plugins. A
    // Design command's is kDesignGroup, an untranslated msgid, which is also the Speed Dial section of its own action
    // and of its actions.
    std::string group;
    // The heading it comes under inside its group, which tool_list gives beside it: a Design command's heading of the
    // index in tool_list's description (DesignTool::section). Empty elsewhere, where the group is the place's heading
    // already.
    std::string heading;
    // The Speed Dial section of the command's own action (has_own_action), an untranslated msgid; empty where that
    // action is in `group`, as a Design command's is.
    std::string action_group;
    // "percent"/"tab": the Speed Dial's second phase, which collects the value the command needs before it
    // runs; "" to run at once.
    std::string input;
    std::string icon;  // tile pictogram: an SVG base name under resources/images, or "" for a blank tile
    std::string       description; // English, for agents; not shown in the Speed Dial
    CommandCompletion completion = CommandCompletion::Immediate;
    // The command's function. With no arguments it does what a person's pick does; with arguments that a
    // dialog would otherwise ask for, it runs without asking.
    CommandFn      run;
    nlohmann::json input_schema = {{"type", "object"}, {"properties", nlohmann::json::object()}};
    // Calls that document the command: [{title, calls: [{tool, args}], needs?}], or null.
    nlohmann::json examples;
    // The actions of the menu items folded into the command, [{key, title, group?, icon, description, args, input?,
    // input_arg?}] (NativeCommands::action()), or null: each, under the id of its key, runs the command with `args`
    // (NativeCommands::make_action()). Its title is an untranslated msgid in title_context(), as the command's is.
    // group is its Speed Dial section, an untranslated msgid; a Design tab action has none and is in the command's
    // group. input is the Speed Dial's token for the value its second phase collects ("percent", "tab"), which the
    // action passes on under input_arg, the command's argument, or under the token where it has no input_arg.
    nlohmann::json actions;
    // shown in tools/list: every command outside the Design tab that runs from its arguments, all but the menu items
    // that take none
    bool listed         = false;
    bool has_own_action = false; // a person can pick it: it has an action of its own, under its key
    // The access labels sent as MCP's annotations. Clients act on them: Codex asks a person before every call
    // of a destructive command, and Claude Code runs read-only calls in parallel and retries idempotent ones.
    // read_only: it changes nothing in the app or the project; window_screenshot is, since its PNG replaces no file
    // without overwrite, and replay_export, which can clear the journal, is not. destructive: every call deletes
    // what a person made, with no undo: preset_delete and plugin_uninstall. A command that can lose work only when an
    // argument asks for it is not: project_new, calibration_generate_test and project_open drop unsaved changes only
    // with discard_changes, and an output replaces a file only with overwrite. Nor is a command that runs what its
    // arguments name (widget_click and the other input tools, action_run, tool_call, replay_run, script_run), though
    // what it runs may be. The client sees both in the arguments, and the label would ask on every call. Nor is
    // recording_encode, which deletes only a recording this session made, once its MP4 has replaced it, nor a command
    // whose deletion has undo (object_delete, plate_delete). A command made from menu items is neither read_only nor
    // destructive. open_world: it can reach outside OrcaSlicer (a printer, the cloud account, the network, or anything
    // the computer allows), including through what it runs: of the commands made from menu items, printer_send, preset_sync,
    // filament_sync_ams, app_check_for_updates, app_open_web_page and plugin_refresh; and plugin_enable and plugin_reload,
    // which run a plugin's code and download a cloud plugin, and plugin_uninstall, whose Unsubscribe reaches the account.
    bool       read_only = false, destructive = false, idempotent = false, open_world = false;
    GuardClass guard     = GuardClass::Mutate;
    // inline_call: pure observation, which runs on the request thread and is never a job; everything that can
    // activate UI runs as a job instead, because activating UI can open a modal that blocks the call. journal:
    // recorded by the session journal for replay_export; observation commands turn it off, except app_wait and
    // window_screenshot, which a replay needs.
    bool inline_call = false, journal = true;
    // allowed_during_operation: may run while an MCP slice/export operation holds the project (job_cancel).
    // cancellable: `job_cancel {job}` stops a running call that is not an operation (recording_encode); the
    // function sees ToolContext::cancelled() and ends.
    bool allowed_during_operation = false, cancellable = false;
    // How long a call may run before the reply returns a job envelope and the call continues.
    int budget_ms = 45000;
    // The arguments that name an object, a part or an instance by id (Mcp::kIdArguments): the session journal records
    // a call by the indices they named, and leaves out those given beside their indices (Mcp::with_target_ids()).
    std::vector<std::string> id_args;
    // The operations (GUI/Operations) the command reaches when it is given arguments, by function name. A
    // command in the Speed Dial that takes arguments names them or is on the exception list of the catalog test.
    std::vector<std::string> operations;
};

// The context a command's title is translated in, or nullptr for none: "Design" for a Design command, whose
// title is its entry's name, which the Design table marks in that context for every entry (DesignOffer.hpp), with
// a menu item or without, the context the Design tab's right-click menu translates an item's name in. Every place
// that shows a command's title translates it with this (NativeCommands::action_title(), a recording's caption).
inline const char* title_context(const NativeCommand& command)
{
    return command.group == kDesignGroup ? "Design" : nullptr;
}

// A word view_set's tab takes for one of the main window's own pages, and the id of that page (MainFrame's TAB_ID_*).
struct TabWord
{
    const char* word;
    const char* page;
};
// The words of the main window's own pages. A word is its page's id, but for the Device tab's: device, whose page is
// monitor. The window's other pages (the web Device page, a plugin's) have no word and are named by their ids.
inline constexpr TabWord kTabWords[] = {{"home", "home"},       {"design", "design"},
                                        {"prepare", "prepare"}, {"preview", "preview"},
                                        {"device", "monitor"},  {"multi_device", "multi_device"},
                                        {"project", "project"}, {"calibration", "calibration"}};

// The first of `command`'s actions that collect no value (no input) whose args are `args`, compared as JSON values, a
// null `args` as {}; nullptr when none is.
const nlohmann::json* action_with_args(const NativeCommand& command, const nlohmann::json& args);

// The arguments a pick of `action`, an item of a command's actions, runs its command with, given `args` (what
// action_run or the Speed Dial's second phase gives): the action's args, and for an action with an input the value
// `args` holds under that token, put under its input_arg, or under the token where it has none; a tab's page id that a
// word of kTabWords names is given as that word (the Device tab's monitor as device). For a command's own action
// (`action` null), `args` as they are, the call's own arguments. A person's pick, action_run and the description of a
// replay's step all run an action with it.
nlohmann::json action_call_args(const nlohmann::json* action, const nlohmann::json& args);

// What a call of `command` with `args` is titled where a person is shown it (a recording's caption): the title of
// action_with_args(), else the command's. Untranslated, in title_context(command).
std::string call_title(const NativeCommand& command, const nlohmann::json& args);

// What the reference says of a command: key, title, group, description, input_schema, listed, has_own_action, the four
// access labels, guard ("inspect", "recover", "mutate"), completion ("immediate", "deferred") and operations; icon,
// input and action_group when they are not empty, and examples and actions when it has some. All of it is the
// command's English text and constants, never translated.
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
    // Before seal(); after it, throws std::logic_error. Records that this catalog leaves out `key`, a command
    // a run of the app with other options holds, `why`, which unknown_command() gives after the key, and `code`, the
    // code of an error naming it (unknown_command_code()), which says what keeps it from this run.
    void leave_out(const std::string& key, std::string why, std::string code);
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
    // Why this catalog leaves out `key` (leave_out()), or nullptr when it does not.
    const std::string* left_out(const std::string& key) const;
    // Why this catalog leaves out the commands it left out with `code`, or nullptr when it left out none with it: what
    // a whole set of commands left out together gives, such as the Design tab's (design_unavailable), which all share
    // one reason.
    const std::string* left_out_reason(const std::string& code) const;
    // Up to `limit` keys of added commands closest to `key`, for "did you mean" errors: a key that
    // contains `key` first, then by edit distance, none further than max(3, half the length of `key`).
    std::vector<std::string> similar(const std::string& key, std::size_t limit = 3) const;

    // The error for a key lookup() does not resolve: "Unknown tool: <key>", followed by why the catalog leaves
    // it out (leave_out()), or else with `suggest` by the closest key.
    std::string unknown_command(const std::string& key, bool suggest = false) const;
    // The code of the error for a key lookup() does not resolve, given as a call's argument (tool_call's tool,
    // tool_describe's, a script's or plugin's call): leave_out()'s code for a key the catalog leaves out, whose name is
    // right, else invalid_argument.
    std::string unknown_command_code(const std::string& key) const;

private:
    struct LeftOut
    {
        std::string why;
        std::string code;
    };
    std::vector<NativeCommand>                   m_commands;
    std::unordered_map<std::string, std::size_t> m_index;    // key -> position in m_commands
    std::unordered_map<std::string, LeftOut>     m_left_out; // key -> why the catalog leaves it out, and the code
    std::vector<std::unique_ptr<CommandSource>>  m_sources;
    bool                                         m_sealed = false;
};

// The command of `catalog` that has an action under `key`, with that action: the command `key` with nullptr when its
// own action is (has_own_action), else the command with the item of its `actions` whose key is `key`; nullptrs when no
// command added with add() has one.
std::pair<const NativeCommand*, const nlohmann::json*> find_command_action(const CommandCatalog& catalog, const std::string& key);

// What a call of `command` with `args` is titled where a person is shown it (a recording's caption), and the command
// whose title_context() that title is translated in: for action_run of a command's action in `catalog` (args.id, the
// action's id or the key inside it), the action's title and that command, the command's own title for its own action;
// for any other call, call_title() and `command`. Untranslated.
std::pair<std::string, const NativeCommand*> shown_title(const CommandCatalog& catalog, const NativeCommand& command,
                                                         const nlohmann::json& args);

// The reference of `catalog`: {"commands": [command_reference(), ...]} of the commands added with add(), in
// the order they were added. Its sources' commands are left out: they are a plugin's, not the build's.
// tests/data/command_catalog.json is this reference of the catalog a run with the Design tab holds, and the
// skill's list of the Speed Dial's commands is generated from that file (tools/mcp/gen_skill_reference.py).
nlohmann::json catalog_reference(const CommandCatalog& catalog);

}} // namespace Slic3r::GUI
