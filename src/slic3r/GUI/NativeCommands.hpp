#pragma once

#include "ActionRegistry.hpp" // for AppAction / AppActionRunResult
#include "CommandCatalog.hpp"
#include "CommandTypes.hpp"

#include <nlohmann/json.hpp>
#include <wx/string.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI {

namespace Ops {
struct Refusal;
}

namespace NativeCommands {
// Which optional families of commands the process catalog holds. cad: the Design tab is on in this run, so the
// catalog holds its commands (in a build with CAD; a build without it has none).
struct CatalogOptions
{
    bool cad = false;
};

// Adds the built-in commands to `catalog`, family by family (Mcp::register_app_tools() and the others): the typed
// tools and the commands made from menu items (MenuItems, NativeCommandMenuItems.hpp), whose items are their tiles, eight of them
// with a tile of their own (in_palette). Each is in the group of its place (place_heading()) and is listed, as it runs
// from its arguments, but for the three menu items that take none (window_save_screenshot, script_toggle_panel,
// settings_toggle_developer_mode). They come in the order tools/list gives them: tool_search, tool_call and app_state,
// then place by place (Tools and actions, The app, The project, Objects, Parts, Plates and on), each place's commands
// in the order their families declare them, and Driving the GUI last. Before Driving the GUI, with options.cad, the Design tab's commands
// (Mcp::register_cad_commands()), not listed, of which those with an action have a tile the Speed Dial shows in the
// Design tab; without it, in a build with CAD, it records that it leaves them out and why
// (Mcp::leave_out_cad_commands()). undo and redo act on the Design tab's history or the project's, a person's pick
// as the Edit menu's item does; with options.cad their examples show the Design tab's. Also adds the source of the
// operations the loaded plugins offer (Mcp::register_plugin_operations()).
void register_all_commands(CommandCatalog& catalog, const CatalogOptions& options);

// The heading of the place a command outside the Design tab is under, its group, an English text tool_search filters
// by: the place its name's first word names (object_mirror: Objects; undo and redo: The app), or for the two
// screenshot commands, which are grouped with the recordings and not with the windows, Recordings, replays and
// scripts. nullptr for a key whose first word is no place.
const char* place_heading(const std::string& key);

// Builds and seals the process catalog. UI thread, once, at startup; a second call, or a call after
// instance() built the catalog by itself, throws std::logic_error.
void build(const CatalogOptions& options);

// The process catalog, sealed. In the app, GUI_App's startup builds it before anything reads it; where
// build() never runs, in the unit tests and in the stdio bridge answering with no app running, the first
// read builds it with CatalogOptions{}. It holds no translated text, so it stays valid across a UI language
// switch. Any thread.
const CommandCatalog& instance();

// An item of a command's `tiles`: a menu item folded into the command, which runs it with `args`. `title` and `group`
// (its Speed Dial section) are untranslated msgids, L()-marked by the caller. `input` is the Speed Dial's token for
// the value the item's second phase collects ("percent", "tab"), and `input_arg` the command's argument that value
// goes to where it is not named as the token. The item has `group`, `input` and `input_arg` only when they are
// given: without a group it is in its command's section.
nlohmann::json tile(std::string key, std::string title, std::string group, std::string icon, nlohmann::json args,
                    std::string description, std::string input = {}, std::string input_arg = {});

// A tile a person picks from the Speed Dial: `command`'s own (in_palette) when `tile` is null, else the item of its
// tiles `tile` points at.
struct PaletteTile
{
    const NativeCommand*  command;
    const nlohmann::json* tile;
};

// The tiles a person picks from the Speed Dial whatever tab is shown, in the order their commands were added, a
// command's own first and then each item of its tiles: all but the Design tab's (group kDesignGroup), which the
// Speed Dial has only while that tab is shown (ActionRegistry::materialize_design_actions()).
std::vector<PaletteTile> palette_tiles(const CommandCatalog& catalog);

// A command whose function runs on the UI thread and says what a person's pick shows. Its run: for a
// person's pick (ToolContext::person()), `fn(args)` in place, its result as the reply's notice. For an
// automated call, `fn(args)` in one UI closure, then one more pass of the UI's events (Mcp::settle()), and
// the reply from_run_result() makes of it. With `completion` Export and arguments, the call also holds the
// project as an operation from before `fn` starts the export, and waits for the file it writes; called with
// no arguments, an Export command does its person's form and waits for nothing.
NativeCommand ui_command(std::string key, std::string title, std::string group, nlohmann::json schema,
                         std::function<AppActionRunResult(const nlohmann::json& args)> fn,
                         CommandCompletion completion = CommandCompletion::Immediate);

// A command whose function runs where its call does: on the job's thread for an automated call. Picked by a
// person it is started as a job (ToolContext::submit()) and returns at once, unless `inline_call` says it
// runs on the calling thread, where its function then runs.
NativeCommand long_command(std::string key, std::string title, std::string group, nlohmann::json schema, CommandFn fn,
                           bool inline_call = false);

// A typed tool that a person picks too: `tool`, which has its tiles already (its own, in_palette, or items of its
// tiles), with its person's forms (Mcp::person_forms()): {} for its own tile, and the args of each item that collects
// no value. A call that is one of them (Mcp::person_form(tool, args)) is the person's form: `person` given those
// arguments, on the UI thread, as ui_command() runs a function. Any other call is the tool's own function, given the
// arguments as they are. `own_forms` are the tiles whose menu item does what the tool does (view_set's camera tiles): an
// automated call whose arguments, null as {}, are one of them is the tool's own function, and a person's pick of one
// is `person`, which runs the same function in place; they stay person's forms (Mcp::person_form()) for what reads a
// call as a tile's. A person's pick of a tile that collects a value is `person` too, given the tile's call (its args
// and the value), while an automated call of it is the tool's. An agent's call of the person's form is refused while a
// modal dialog is open or another call's nested loop runs (Mcp::UiInvoker::require_mutate()), also where the tool's
// guard lets it run behind dialogs, unless `person_form_behind_dialogs` says the form opens nothing and may run where
// the tool does. The tool's schema stays as it is: it takes its tiles' arguments, and its own function needs nothing a
// tile leaves out.
NativeCommand with_menu_item_forms(NativeCommand tool, std::function<AppActionRunResult(const nlohmann::json& tile_args)> person,
                             bool person_form_behind_dialogs = false, std::vector<nlohmann::json> own_forms = {});
// with_menu_item_forms() for a tool whose own function requires what its person's forms leave out (a path a dialog asks
// for): its schema goes through Mcp::with_silent() with them, so it also takes silent: true, and a call with silent is
// the tool's own function, given silent with the other arguments, so that what the function journals keeps it.
NativeCommand with_person_form(NativeCommand tool, std::function<AppActionRunResult(const nlohmann::json& tile_args)> person,
                               bool person_form_behind_dialogs = false);

// The person's forms of recording_start and recording_stop. UI thread.
// The Start Recording dialog, with the settings it remembers, and the person's recording it starts: the main
// window, with the overlays the settings ask for, for up to an hour, into the folder the settings name.
AppActionRunResult start_person_recording();
// Stops the running recording, whoever started it, without waiting for its file, and shows the person a
// notification naming it; the reply has its path.
AppActionRunResult stop_recording_in_app();
// What the person's forms of project_save, {} and {"save_as": true}, answer for Plater::save_project()'s `answer`.
// A save that wrote nothing (a dialog cancelled, the file not written) did nothing: the person, who answered
// or read its dialog, is shown nothing more, and automation gets Ops::project_not_saved().
AppActionRunResult project_save_result(int answer);

// What a command answers when an operation, or the shared function a pick runs, refuses: the refusal in the UI's
// language, shown at `level`, as a command that did nothing; automation reads the refusal itself (from_run_result()).
AppActionRunResult refused(const Ops::Refusal& refusal, AppActionRunResult::Level level = AppActionRunResult::Level::Error);

// What a command's result replies to an automated call: {"ran": key, "level", "message"?} with the fields
// of its `reply`. It is an error for a Busy or Error level and for a command that did nothing; an operation's
// refusal is worded in English, with {"code", "names"?} as the error's details.
ToolResult from_run_result(const std::string& key, const AppActionRunResult& result);

// Runs command `key` as a person's pick: checks `args` (Mcp::argument_error()), then runs the command with a
// Mcp::PersonContext. Returns what the pick shows, the reply's notice, or an Error holding the message when
// the arguments were refused, the command refused the call (ToolFailure), or it failed with no notice. Unknown
// keys return a quiet Info, and so does any key while OrcaSlicer is closing. Any other exception is the
// command's defect, not an answer to show: it is logged with the command's key and leaves this function, for
// the application's handler (GUI_App::OnExceptionInMainLoop()) as from any other event handler. UI thread only.
AppActionRunResult run(const std::string& key, const nlohmann::json& args = nlohmann::json::object());
// The same for `command`, which need not be in the process catalog, whether or not OrcaSlicer is closing.
AppActionRunResult run(const NativeCommand& command, const nlohmann::json& args = nlohmann::json::object());

// Shows what a command run from the palette or a menu said: its message as a notification at its level,
// nothing for an empty message or a Busy one. UI thread only.
void notify_result(const AppActionRunResult& result);

// Whether a recording is being written, whoever started it. UI thread only.
bool recording_active();

// The process catalog's command `key`, or nullptr. Any thread.
const NativeCommand* find(const std::string& key);

// A command's title as its tile shows it, in the UI's language, translated in its title_context(): a Design
// command's, its entry's name, in the Design context, as the right-click menu names an item; any other command's
// title with no context. The Speed Dial's tiles, the replay and script editors and the notice of a job a person
// started show a command's title with it; a recording's caption translates the title in the same context on its
// own (title_context()). UI thread only.
wxString tile_title(const NativeCommand& command);

// Materialises the own tile of one catalog entry as a runnable AppAction, titled tile_title() with the English title
// and section kept as search aliases; its section is the command's tile_group, or its group where that is empty. A
// Design command's takes the command's arguments; any other's takes none, its preset {}, as an item of tiles with no
// arguments does. Keeps the catalog's identity, presentation and behaviour as the single source of truth; ActionRegistry only stores
// and dispatches the result. The action runs `command`, which outlives it. UI thread only.
std::unique_ptr<AppAction> make_action(const NativeCommand& command);
// The action of `tile`, one of `command`'s tiles (NativeCommand::tiles): the id of its key, its title translated
// in the command's title_context(), its section (the tile's group, else the command's), its icon and description,
// and its args as the preset. A tile that collects a value takes that value alone, under its input token, typed as
// the command's argument it goes to; any other shows an agent no arguments. Either runs the command with
// tile_call_args() (CommandCatalog.hpp), for a person's pick and action_run alike. UI thread only.
std::unique_ptr<AppAction> make_action(const NativeCommand& command, const nlohmann::json& tile);
} // namespace NativeCommands

}} // namespace Slic3r::GUI
