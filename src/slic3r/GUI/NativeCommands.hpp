#pragma once

#include "ActionRegistry.hpp" // for AppAction / AppActionRunResult
#include "CommandCatalog.hpp"
#include "Mcp/McpTypes.hpp"

#include <nlohmann/json.hpp>
#include <wx/string.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI {

namespace NativeCommands {
// Which optional families of commands the process catalog holds. cad: the Design tab is on in this run, so the
// catalog holds its commands (in a build with CAD; a build without it has none).
struct CatalogOptions
{
    bool cad = false;
};

// Adds the built-in commands to `catalog`: the Speed Dial's, each with in_palette, then the typed tools, each
// listed, family by family (Mcp::register_app_tools() and the others), and with options.cad the Design tab's
// (Mcp::register_cad_commands()), not listed, of which those with an action have a tile the Speed Dial shows
// in the Design tab; without it, in a build with CAD, it records that it leaves them out and why
// (Mcp::leave_out_cad_commands()). new_project, save_project, start_recording and stop_recording are both:
// typed tools with a tile (with_person_form()). undo and redo act on the Design tab's history or the
// project's, a person's pick as the Edit menu's item does; with options.cad their examples show the Design
// tab's.
void register_all_commands(CommandCatalog& catalog, const CatalogOptions& options);

// Builds and seals the process catalog. UI thread, once, at startup; a second call, or a call after
// instance() built the catalog by itself, throws std::logic_error.
void build(const CatalogOptions& options);

// The process catalog, sealed. In the app, GUI_App's startup builds it before anything reads it; where
// build() never runs, in the unit tests and in the stdio bridge answering with no app running, the first
// read builds it with CatalogOptions{}. It holds no translated text, so it stays valid across a UI language
// switch. Any thread.
const CommandCatalog& instance();

// The commands a person picks from the Speed Dial whatever tab is shown: those with in_palette, in the order
// they were added, but the Design tab's (group kDesignGroup), whose tiles the Speed Dial has only while that tab
// is shown (ActionRegistry::materialize_design_actions()).
std::vector<const NativeCommand*> palette_commands(const CommandCatalog& catalog);

// A command whose function runs on the UI thread and says what a person's pick shows. Its run: for a
// person's pick (ToolContext::person()), `fn(args)` in place, its result as the reply's notice. For an
// automated call, `fn(args)` in one UI closure, then one more pass of the UI's events (Mcp::settle()), and
// the reply from_run_result() makes of it. With `completion` Export and arguments, the call also holds the
// project as an operation from before `fn` starts the export, and waits for the file it writes; called with
// no arguments, an Export command does its person's form and waits for nothing.
NativeCommand ui_command(std::string key, std::string title, std::string group, nlohmann::json schema,
                         std::function<AppActionRunResult(const nlohmann::json& args)> fn,
                         CommandCompletion completion = CommandCompletion::None);

// A command whose function runs where its call does: on the job's thread for an automated call. Picked by a
// person it is started as a job (ToolContext::submit()) and returns at once, unless `inline_call` says it
// runs on the calling thread, where its function then runs.
NativeCommand long_command(std::string key, std::string title, std::string group, nlohmann::json schema, CommandFn fn,
                           bool inline_call = false);

// A typed tool that a person picks too: `tool` with the tile's `title`, `group` and `icon`, in_palette, and its
// schema through Mcp::with_silent(). Its run, called with no arguments, is the person's form: `person` on the
// UI thread, as ui_command() runs a function. Called with arguments, or silent: true, it is the tool's own
// function, given the arguments as they are, silent included, so that what the function journals keeps it.
// An agent's call of the person's form is refused while a modal dialog is open or another call's nested loop
// runs (Mcp::UiInvoker::require_mutate()), also where the tool's guard lets it run behind dialogs, unless
// `person_form_behind_dialogs` says the form opens nothing and may run where the tool does.
NativeCommand with_person_form(NativeCommand tool, std::string title, std::string group, std::string icon,
                               std::function<AppActionRunResult()> person, bool person_form_behind_dialogs = false);

// The person's forms of start_recording and stop_recording. UI thread.
// The Start Recording dialog, with the settings it remembers, and the person's recording it starts: the main
// window, with the overlays the settings ask for, for up to an hour, into the folder the settings name.
AppActionRunResult start_person_recording();
// Stops the running recording, whoever started it, without waiting for its file, and shows the person a
// notification naming it; the reply has its path.
AppActionRunResult stop_recording_in_app();

// What a command's result replies to an automated call: {"ran": key, "level", "message"?} with the fields
// of its `reply`. It is an error for a Busy or Error level and for a command that did nothing; an operation's
// refusal is worded in English, with {"code", "names"?} as the error's details.
Mcp::ToolResult from_run_result(const std::string& key, const AppActionRunResult& result);

// Runs command `key` as a person's pick: checks `args` (Mcp::argument_error()), then runs the command with a
// Mcp::PersonContext. Returns what the pick shows, the reply's notice, or an Error holding the message when
// the arguments were refused, the command threw, or it failed with no notice. Unknown keys return a quiet
// Info. UI thread only.
AppActionRunResult run(const std::string& key, const nlohmann::json& args = nlohmann::json::object());

// Shows what a command run from the palette or a menu said: its message as a notification at its level,
// nothing for an empty message or a Busy one. UI thread only.
void notify_result(const AppActionRunResult& result);

// Whether a recording is being written, whoever started it. UI thread only.
bool recording_active();

// The process catalog's command `key`, or nullptr. Any thread.
const NativeCommand* find(const std::string& key);

// A command's title as its tile shows it, in the UI's language, translated in its title_context(): a Design
// command's, its entry's name, in the Design context, as the right-click menu names a row; any other command's
// title with no context. The Speed Dial's tiles, the replay and script editors and the notice of a job a person
// started show a command's title with it; a recording's caption translates the title in the same context on its
// own (title_context()). UI thread only.
wxString tile_title(const NativeCommand& command);

// Materialises one catalog entry as a runnable AppAction, titled tile_title() with the English title/group
// kept as search aliases. Keeps the catalog's identity, presentation and behaviour as the single source of
// truth; ActionRegistry only stores and dispatches the result. UI thread only.
std::unique_ptr<AppAction> make_action(const NativeCommand& command);
} // namespace NativeCommands

}} // namespace Slic3r::GUI
