#pragma once

#include "McpPersonRun.hpp"

#include "libslic3r/CustomGCode.hpp"
#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/CommandTypes.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Slicing.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"

#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <iosfwd>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

class AppConfig;
class PythonSession;
struct Calib_Params;
class ConfigOptionDef;
class DynamicPrintConfig;
class Model;
class ModelInstance;
class ModelObject;
class ModelVolume;
enum class ModelVolumeType : int;
class Preset;
class PresetCollection;
struct GCodeProcessorResult;
namespace CustomGCode { struct Info; }

namespace GUI {

struct AppAction;
struct AppActionRunResult;
class IMSlider;
class PartPlate;
class PartPlateList;
class Plater;

namespace Ops {
struct ExportTicket;
struct PrinterStatus;
struct Preference;
struct Refusal;
enum class PresetChanges;
} // namespace Ops

namespace Mcp {

class AutomationRuntime;
struct Replay;
struct ReplayContext;

// Each tool family adds its commands to the catalog (NativeCommands::register_all_commands(), which gives each the
// group of its place), and so do the Design tab's (register_cad_commands(), McpToolsCad.hpp). A command's function
// reaches the automation runtime through its call's context (ToolContext::runtime()), so the catalog is built
// without one.
// `cad`: the catalog holds the Design tab's commands, which undo's and redo's examples call.
void register_app_tools(CommandCatalog& catalog, bool cad);
void register_action_tools(CommandCatalog& catalog);
void register_project_tools(CommandCatalog& catalog);
void register_config_tools(CommandCatalog& catalog);
// The printer models and system filaments a person installs (McpToolsInstall.cpp).
void register_install_tools(CommandCatalog& catalog);
// The Preferences dialog's preferences, read and written (McpToolsPreferences.cpp).
void register_preference_tools(CommandCatalog& catalog);
void register_scene_tools(CommandCatalog& catalog);
// The commands that open a gizmo on a named target, and act from arguments where its gizmo has an entry point that asks
// nothing (McpToolsGizmos.cpp).
void register_gizmo_tools(CommandCatalog& catalog);
// A gizmo command's acting arguments: a call with any of them does the job and opens no gizmo; a call with none opens
// it. Empty for a command that only opens, and for any other command.
const std::vector<std::string>& gizmo_acting_args(const std::string& command);
// print_start, printer_send, printer_test_host, printer_status, print_pause, print_resume and print_stop
// (McpToolsPrinter.cpp).
void register_printer_tools(CommandCatalog& catalog);
// printer_status's reply: {"connected": false} with no printer, else the status's fields, a name the Device tab would
// not give being null.
nlohmann::json printer_status_reply(const std::optional<Ops::PrinterStatus>& status);
// The plates argument of print_start and printer_send, as project_export takes it: a plate's index into `plate`,
// "all" as `all`, or "current" (also when left out) as neither. Returns what is wrong with it, else empty.
std::string read_print_plates(const nlohmann::json& args, std::optional<int>& plate, bool& all);
void register_view_tools(CommandCatalog& catalog);
void register_plugin_tools(CommandCatalog& catalog);
// The error of a plugin command refused by `refusal` (McpToolsPlugins.cpp), the plugin's row (plugin_list's) being
// `before` the call and `after` it, null where it is not listed. Where the call changed how the plugin stands (installed,
// enabled or its error: a cloud plugin downloaded before the refusal, or its download's error recorded), the error says
// how it is left and carries its row under "plugin"; else it is the refusal alone.
ToolFailure plugin_refusal_after(const Ops::Refusal& refusal, const nlohmann::json& before, const nlohmann::json& after);
// plugin_enable's reply once the call has ended (McpToolsPlugins.cpp), `row` being the plugin's row then, null where it is
// not listed: {plugin, changed}, with a note while the load the call started (`load_started`) goes on (`still_loading`), or
// where the plugin is no longer listed. A load that ended with the plugin not enabled, or no longer listed, is an error
// (not_loaded) that says how the plugin is left and carries its row.
nlohmann::json plugin_enable_reply(const std::string& plugin, bool changed, bool load_started, bool still_loading,
                                   const nlohmann::json& row);
void register_ui_tools(CommandCatalog& catalog);
void register_replay_tools(CommandCatalog& catalog);
void register_record_tools(CommandCatalog& catalog);
void register_python_tools(CommandCatalog& catalog);
// `design_index` ends tool_list's description: what it says of the Design commands (design_command_index(),
// McpToolsCad.hpp) in a catalog that holds them, and nothing in one that does not.
void register_api_tools(CommandCatalog& catalog, const std::string& design_index);
// Adds the source of the operations the loaded plugins offer (plugin.<plugin>.<name>), which come and go with their
// plugins, in group Plugins: tool_call runs one by name and tool_list lists it; tools/list does not show them.
void register_plugin_operations(CommandCatalog& catalog);

// How run_replay_job() runs a replay beyond what the replay says.
struct ReplayRunOptions
{
    std::optional<std::size_t> only_step;    // 1-based; the whole replay when empty
    bool                       write_result = true; // result.json in the output folder
    // Called on the job thread as each step starts and ends (see ReplayHost).
    std::function<void(std::size_t)>           step_started;
    std::function<void(const nlohmann::json&)> step_finished;
};

// Runs a parsed replay as the calling job, as replay_run does: steps are child calls, dialog rules
// the replay registers end with it, and result.json goes to sctx.out_dir (created if missing). It
// stops before the next step, and stops waiting on a step's job, once the call is cancelled: the
// runtime stops, or a stop was asked for any job in the call's lineage (JobRegistry::request_stop), so
// a nested replay_run stops with the run that holds it. Returns run_replay()'s result with out_dir (and result_file).
nlohmann::json run_replay_job(AutomationRuntime& runtime, ToolContext& ctx, const Replay& replay, ReplayContext& sctx,
                              const ReplayRunOptions& options);

// ---- schema helpers --------------------------------------------------------------------------

using Property = std::pair<const char*, nlohmann::json>;

// {"type":"object","properties":{...},"required":[...],"additionalProperties":false}
nlohmann::json object_schema(std::initializer_list<Property> properties, std::vector<std::string> required = {});
nlohmann::json prop(const char* type, const std::string& description);
nlohmann::json enum_prop(std::vector<std::string> values, const std::string& description);
nlohmann::json array_prop(nlohmann::json items, const std::string& description);

// The typed tools are listed commands (tools/list shows them) made with NativeCommands::long_command(), their
// function unchanged. A command's group is its place's, which the catalog gives it (NativeCommands::place_heading()).
// Pure observation: inline, read-only, inspect guard, not journaled.
NativeCommand inspect_tool(std::string name, std::string title, std::string description, nlohmann::json schema, CommandFn handler);
// `command`, naming the operations (GUI/Operations) its function reaches, by function name.
NativeCommand reaching(std::vector<std::string> operations, NativeCommand command);
// Anything that changes the app: a job under the mutate guard, journaled.
NativeCommand mutate_tool(std::string name, std::string title, std::string description, nlohmann::json schema, CommandFn handler,
                          bool destructive = false);
// A command made from menu items (NativeCommandMenuItems.hpp): NativeCommands::ui_command(), listed, with `description` and
// the items as its `actions`. `fn` does what the menu item a call's variant argument names does, as a person's pick of that item
// does: on the targets the call names (McpTargets.hpp) where the command takes them, else on what is selected. A menu item
// with no argument form is no command of the tool list: its declaration sets listed false.
NativeCommand menu_command(std::string name, std::string title, std::string description, nlohmann::json schema, nlohmann::json actions,
                          std::function<AppActionRunResult(const nlohmann::json& args)> fn);
// What a command made from menu items replies to a call, which a pick does not show: `reply`.
AppActionRunResult menu_reply(const nlohmann::json& reply);
// What a command made from menu items answers for a refusal after a dialog the person answered or read, which told them why:
// a pick shows nothing more, and a call learns the refusal.
AppActionRunResult quiet_refusal(const Ops::Refusal& refusal);

// ---- object, part and instance ids -----------------------------------------------------------

// The schema of an id argument: the id object_list reports for an object, part or instance (`of`), which orca.host
// shows too, so a script addresses what it read. Its description only names what it is: that an id names its target
// in place of the index, or beside it as a guard, is said once, in the server instructions, rather than in each of
// the many schemas that have one.
nlohmann::json id_prop(const char* of);
// `command` with its id_args, the id arguments its schema has (kIdArguments), and with a function that runs the
// command's own on the arguments with the index form of each target the call names by id (with_indices(), read on
// the UI thread just before), and journals the call so: ids are allocated per process, and a replay addresses by
// index (without_guard_ids()). The ids stay beside the indices, so the command checks them where it acts.
NativeCommand with_target_ids(NativeCommand command);
// Refuses the call when `args` names an object_id, part_id or instance_id that is not the id of `object`, `volume`
// or `instance`, which its index names now: the model changed since the caller read the index. Call it before the
// tool changes anything.
void check_expected_ids(const ModelObject& object, const ModelVolume* volume, const ModelInstance* instance,
                        const nlohmann::json& args);

// ---- handler helpers -------------------------------------------------------------------------

// One more UI round trip after a mutation, so events it queued are handled before the reply, and
// any 3D view it showed has its GL surface (on Wayland, a moment after it is shown).
void settle(ToolContext& ctx);

// The plater, or ToolFailure while the main window is not up yet (startup, language change).
// UI thread only.
Plater& ui_plater();

// What a tool says of a plate index the project does not have: the index, and the plates there are
// (Ops::no_such_plate()'s text).
std::string no_such_plate(int plate, int count);

// What a tool replies when an operation refuses: the refusal's English text, then `hint` when the tool
// has something to add for an agent, with the refusal's code as `code`, the names it carries under
// `names_key` when the tool gives one, and refusal_hint()'s as `hint`.
ToolFailure refused(const Ops::Refusal& refusal, const std::string& hint = {}, const char* names_key = nullptr);

// What an agent's call does about `refusal` in the commands' terms, where the refusal's text, which a person reads too,
// asks for a GUI step or a selection that a call can do instead (close the toolbar menus, deselect, select a part);
// empty for the others, those whose step only a person takes (sign in, connect a printer) among them. Every route by
// which a refusal reaches an agent adds it to the error's details as `hint`: refused() and
// NativeCommands::from_run_result().
std::string refusal_hint(const Ops::Refusal& refusal);

// Typed argument access with defaults; schema validation has already checked the types. arg_int() refuses
// (ToolFailure, invalid_argument) a number that no int holds, rather than narrowing it into another value.
std::string arg_string(const nlohmann::json& args, const char* key, const std::string& fallback = {});
int         arg_int(const nlohmann::json& args, const char* key, int fallback);
double      arg_double(const nlohmann::json& args, const char* key, double fallback);
bool        arg_bool(const nlohmann::json& args, const char* key, bool fallback);

// ---- tool_list and tool_describe ---------------------------------------------------------------

// tool_list's answer from `catalog`: {items: [{name, group, heading?, does}], use, note?}, the commands it holds and
// those its sources hold now that tools/list does not show, in that order; does is the whole description up to 300
// characters, else its first sentence, an operation's after the "Offered by a plugin." its description starts with.
// With `group`, compared in any case with every group the catalog's commands are in, that group's alone, and a note
// that tools/list shows a group's commands when none is left to list. ToolFailure for a name no command's group has
// (invalid_argument, with the groups), and for Design while the catalog leaves its commands out (design_unavailable,
// with the reason they were left out with). Any thread.
nlohmann::json unlisted_commands(const CommandCatalog& catalog, const std::string& group);

// Where tool_describe's manual sends an agent for the values a built-in command's parameter takes when its schema does
// not list them. Source: the tools that list them, in order ("tools/list" stands for the tool list); Described: the
// parameter's own description lists them; Free: the caller's own value (a number, text, a path, a new name, a shape to
// draw). A parameter with fields of its own is read through its fields; the manual names a source for it besides only
// where its command's own entry gives one (printer_install's models).
struct ParameterClass
{
    enum Kind { Source, Described, Free };
    Kind                     kind = Free;
    std::vector<std::string> tools;
};
// The class of parameter `path` of the built-in command `command`, `path` dotted with [] for an array's items
// ("add[].layer"): the entry for that command and path, else the class of the path's last name, else nullopt.
std::optional<ParameterClass> parameter_class(const std::string& command, const std::string& path);

// tool_describe's reply, `command`'s manual: {name, title, group, heading?, listed, call, access, description,
// parameters, rules, menu_items, examples, complete, input_schema?}. parameters follow the schema at every depth, each
// {name, type, required, default?, values?, min?, max?, exclusive_min?, min_items?, max_items?, min_length?,
// max_length?, pattern?, format?, goes_with?, values_from?, items?, properties?, other_keys?, schema?, description}.
// required and default are the schema's alone; what a handler requires or supplies is in the description. A part of a
// parameter's schema the manual can't express is kept raw under that parameter's schema, a part of the top level in
// input_schema alone; either way the manual is then not complete and carries the whole input_schema. Any thread.
nlohmann::json command_manual(const NativeCommand& command);
// tool_describe's answer for `name` in `catalog`: the manual of the command lookup() resolves, else ToolFailure with the
// catalog's text and code for the name (a left-out command's reason and code) and the closest names. Any thread.
nlohmann::json describe_command(const CommandCatalog& catalog, const std::string& name);

// Why action_run refuses `args` for `action`, or empty when it does not: an action that collects a value (input) takes
// exactly that value, a command's own action takes its command's arguments, which the command checks, an item of a
// command's actions (a folded item's, with a preset) takes none or exactly its preset, and every other action (a
// plugin's script or page, a plate's, a setting's or a recent project's) takes none.
std::string action_args_error(const AppAction& action, const nlohmann::json& args);
// The arguments action_run's journal step keeps for a command's action of fixed arguments (AppAction::preset), run with
// `args`, which action_run accepted: what it takes for that action, the action's own arguments, or for an action that
// collects a value, `args` (that value alone). Null for an action without a preset, a Design command's own action among
// them, which takes its command's arguments and keeps the command's rewrite of them (Mcp::ActionContext).
nlohmann::json action_journal_args(const AppAction& action, const nlohmann::json& args);

// What action_run answers for `id`, an action id or the name inside one, that no action of the app's state has, with
// a code that says why. For a Design tab action's: when `catalog` leaves the action's command out
// (CommandCatalog::leave_out()), why, with the code it was left out with; with the CAD feature off now (`cad_on`
// false), which drops every Design action, that a person enables it and restarts (design_unavailable); otherwise when
// that action is there and the tool_call that runs it from any tab (its command, with the item's arguments for a
// folded item's action), as wrong_mode. For any other, where an action or a command is found (invalid_argument).
ToolFailure unknown_action(const std::string& id, const CommandCatalog& catalog, bool cad_on);

// A setting value from JSON in the serialized form set_deserialize accepts: "3", "0.2", "1"/"0"
// for booleans, "a,b" for arrays. A list of strings (`key` names a coStrings option, such as
// post_process) is quoted and separated by semicolons, as ConfigOptionStrings writes it, since its
// entries may contain commas. Throws ToolFailure for values with no such form.
std::string to_config_string(const nlohmann::json& value, const std::string& key = {});

// ---- Python (McpToolsPython.cpp) -------------------------------------------------------------

// The Python API reference script_list_api and script_describe_api read: every public module, class, function, method, property, named instance (such
// as orca.host.preset.process) and constant (such as orca.host.ui.PD_CAN_ABORT) of the orca package, in
// the order the package lists them, each {name, kind, signature, doc}. signature holds the signature lines
// pybind11 starts a docstring with (a class's are its constructors, joined with " | "), and doc the rest
// of the docstring: a class's own followed by what its constructors say, and for a constant, which has
// none, the entries whose doc names it. An enum's values are in its class's doc, not entries of their own.
// Takes the GIL; the caller keeps the interpreter running (a PythonRuntimeLease). Throws ToolFailure when
// the package can't be read.
nlohmann::json python_api_reference();

// The module script_list_api lists without `within`.
inline constexpr const char* kPythonApiRoot = "orca";
// script_list_api's items: the entries of `reference` (python_api_reference()) directly inside `within`, a module's or a
// class's dotted name (a module's modules, classes, functions, constants and named instances; a class's methods and
// properties), in the reference's order, each {name, kind, signature, summary}, the summary its doc's first sentence.
// ToolFailure (invalid_argument) with the package's `modules` when `within` names no module or class. Any thread.
nlohmann::json python_api_children(const nlohmann::json& reference, const std::string& within);
// script_describe_api's answer: the entry of `reference` called `name`, else ToolFailure (invalid_argument) with the three
// closest names by edit distance under `did_you_mean`. Any thread.
nlohmann::json python_api_entry(const nlohmann::json& reference, const std::string& name);

// Whether numpy is installed in OrcaSlicer's Python, which the mesh and matrix arrays of orca.host need.
// Takes the GIL, as python_api_reference() does.
bool python_has_numpy();

// The Run Script panel's session as the calling job, a person's script_run job.
// The job is idle for `app_wait {for: "idle"}`, and owns what the session's tool calls leave behind (auto-answer rules, a
// recording, a held press) until the session ends, with its rules paused between works and its presses released as each
// work ends. The calls hang under a lineage node of the job, which a stop cancels, inline calls included, and which the
// next work replaces. Hosts `session` (host_python_session()) under `filename` until it ends; a job cancel ends it.
ToolResult run_python_session_job(AutomationRuntime& runtime, ToolContext& ctx, PythonSession& session, const std::string& filename);

// Hosts the Run Script panel's `session` as an unjournaled person's script_run job (run_python_session_job()), which
// `runtime` follows on a thread of its own that its stop() joins and stop_followed() stops. `file` is the session's
// file; empty for an unsaved script. `events.finished` is posted only for a job refused before its handler ran or a
// handler that refused (Python scripts off), as {"error": message}; the session's end is PythonSession::ended(),
// which a refused job sets too. nullptr when the runtime is not started or is stopping, or when the system cannot
// start a thread. UI thread.
std::shared_ptr<PersonRun> start_python_session(AutomationRuntime& runtime, std::shared_ptr<PythonSession> session,
                                                const boost::filesystem::path& file, PersonRun::Events events);

// What an editor suggests for each listed command: [{name, title (translated), args: {name: {type?, enum?}}}]. The
// argument descriptions are written for agents, in English, and stay out.
nlohmann::json editor_tool_catalog(const CommandCatalog& catalog);

// ---- recording (McpToolsRecord.cpp) ----------------------------------------------------------

// Starts recording_encode with `args` as an unjournaled job of its own and returns at once: the job's envelope, or
// the error that refused it. For a recording's encode that recording_stop starts, which a replay of the recording
// asks for again. Any thread.
ToolResult start_encode(AutomationRuntime& runtime, const nlohmann::json& args);

// ---- settings (McpToolsConfig.cpp) -----------------------------------------------------------

// Why settings_get (or, with `writes`, settings_set) refuses `args` before reading anything, or empty when it does
// not: a target the type has not (an object's, a part's and a height range's settings are the process's), an object,
// part or height range given with a target that does not take it, a target part named by its object alone, a target
// height_range without min_z and max_z, or nothing named, worded for the tool called (settings_get: no keys, query or
// compare with the target global; settings_set: no values or reset_all), and an extruders_count set with anything
// else or on another type than the printer. Any thread.
std::string settings_request_error(const nlohmann::json& args, bool writes = false);

// The keys the target object (or, with `is_part`, the target part) takes: those of `offered`
// (SettingsFactory::get_options()) that the object list offers with `filaments` filament slots, and
// extruder, the object's or part's filament.
std::vector<std::string> model_scope_options(const std::vector<std::string>& offered, bool is_part, int filaments);

// The process settings a target of the settings tools takes, as its rows in the Process panel's Objects mode offer
// them on an FFF printer with `filaments` filament slots: object, an object or an assembly, and part, a part or a
// modifier (SettingsFactory::get_fff_options(), model_scope_options()); height_range, one of an object's height
// ranges (layer_range_option_keys()). Each with extruder, the filament its row sets. Empty for another target.
std::vector<std::string> target_options(const std::string& target, int filaments);
// The targets whose target_options() hold `key`, in the order object, part, height_range.
std::vector<std::string> targets_taking(const std::string& key, int filaments);
// Why `target` does not take the process setting `key`, saying where it is set, from targets_taking(): "layer_height is
// set on an object or a height range, not on a part", or for a key no target takes, that it is set globally only.
// Empty when the target takes it.
std::string untaken_setting_error(const std::string& key, const std::string& target, int filaments);

// settings_list's page of `items`: all of them, unless the items carry their meta (include_meta, which where implies),
// which pages them by 100; limit and offset as given.
nlohmann::json settings_list_page(const nlohmann::json& items, const nlohmann::json& args);

// What `key` is for an object (`volume` and `range` null), one of its parts, or one of its height ranges (`range`, its
// settings), and where it comes from, in the order the slicer applies them: {"effective", "source"}, the height range's
// own value ("height_range"), else the part's ("part"), else the object's ("object"), else the process's, the global
// value ("global"), else the option's default ("default"; extruder 1). An extruder of 0 is no override. For an object
// or a height range, "parts" lists [{part, value}] for the parts and modifiers overriding `key`, each applying only
// inside its own volume (inside a height range that sets the key, its modifiers alone); for an object, one of its parts
// or a region modifier that sets none of `key`, "height_ranges" lists [{min_z, max_z, value}] for the object's height
// ranges overriding it, each applying only between its heights. Inside a region modifier (a mesh or a painted one) that
// sets none of `key`, the value of the region it overlaps applies: a model part's, or that of a region modifier before it
// in the object's list. Where one of those sets the key, which it overlaps is not worked out: "effective" is null,
// "source" "overlapped_part", "object_effective" and "object_source" give what the object's chain resolves to, and
// "parts" lists [{part, value}] for those that set it.
nlohmann::json effective_setting(const std::string& key, const DynamicPrintConfig& process, const ModelObject& object,
                                 const ModelVolume* volume, const DynamicPrintConfig* range = nullptr);

// Checks preset_save's `name` against `presets` as the Save preset dialog does, and returns whether the
// preset is kept inside the project: where an existing preset of that name is, else `inside_project`,
// else where the edited preset is. Throws ToolFailure for a name the dialog refuses, for replacing
// another existing preset without `overwrite`, and for an `inside_project` that contradicts where the
// existing preset is, which the dialog does not let a person change.
bool save_preset_inside_project(const PresetCollection& presets, const std::string& name, const nlohmann::json& args);

// settings_get's meta of an option: label, description (its whole tooltip: what it does, when to change
// it, what it affects), type, mode, category, unit, range, and for an enum its values with their labels
// (the untranslated texts the GUI shows), in the same order.
nlohmann::json option_meta(const ConfigOptionDef& def);

// settings_list_pages' reply: those of `keys`, in the order settings_list lists them, that have a place ("Page > Group", as
// the settings tab's rows give it) in `places`, as {pages: [{page, groups: [{group, settings}]}]}, pages and groups in
// alphabetical order, each group's settings being how many of `keys` it holds. Any thread.
nlohmann::json settings_pages(const std::map<std::string, std::string>& places, const std::vector<std::string>& keys);


// For a per-variant or per-extruder vector `key` of a preset's `config` that has more than one entry:
// {"variants": [what each entry is for], "active": [the entry each extruder prints with, extruder 1
// first]}; a per-extruder key has no "active", since each extruder uses its own entry. `extruder_types`
// and `nozzle_volume_types` are the printer's and the project's, one per extruder. Null for any other key.
nlohmann::json variant_entries(const std::string& key, const DynamicPrintConfig& config, const std::vector<int>& extruder_types,
                               const std::vector<int>& nozzle_volume_types);

// The serialized value settings_set writes for `key` of the preset whose config is `config`: `value`
// itself, or for {"index": i, "value": v} the whole vector with entry i replaced, as editing one variant
// in the tab leaves it. Throws ToolFailure, naming the entries, for a value with another number of
// entries than a per-variant or per-extruder vector of several (a scalar would collapse it), and for a
// bad index or entry.
std::string preset_setting_value(const std::string& key, const nlohmann::json& value, const DynamicPrintConfig& config,
                                 const std::vector<int>& extruder_types, const std::vector<int>& nozzle_volume_types);

// The settings among `keys` that differ between two presets of one type, as the Compare presets dialog
// finds them: [{key, value, other}], or with "index" for one entry of a printer or filament vector.
// Values are serialized, and null where a preset lacks the key or entry. Bookkeeping *_settings_id keys
// are left out.
nlohmann::json preset_diff(const Preset& left, const Preset& right, const std::vector<std::string>& keys);

// settings_set's "changed": {key: value} for every key whose value differs between `before` and `after`,
// with null for a key `after` no longer has.
nlohmann::json changed_settings(const DynamicPrintConfig& before, const DynamicPrintConfig& after);

// The keys of `changes`, unsaved changes as the tab marks them ({key: value}, or key#index for one entry of
// a vector), whose value `config` no longer holds; each key once.
nlohmann::json discarded_changes(const nlohmann::json& changes, const DynamicPrintConfig& config);

// What a command that changes presets (preset_select, printer_install, printer_uninstall) replies when an operation
// refuses: the refusal's text with what an agent can do about it, and the names it carries under the key each refusal
// has.
ToolFailure preset_refused(const Ops::Refusal& refusal);

// The answer a call's preset_changes gives about the presets' unsaved changes, as the preset tools take it: transfer or
// discard them; Ask where the call gives none.
Ops::PresetChanges preset_changes_answer(const nlohmann::json& args);

// The process, filament and printer presets being edited that have unsaved changes:
// {type: {name, changes: {key or key#index: value}}}. UI thread.
nlohmann::json unsaved_changes();

// What a call did to the unsaved changes `before` (from unsaved_changes()): "discarded", the keys whose
// value is gone, and "modified", the keys still unsaved, each {type: {name, keys}} with only the presets
// that have keys, and left out when there are none. UI thread.
nlohmann::json unsaved_changes_outcome(const nlohmann::json& before);

// Checks that preset_delete may delete the preset `name` of `presets`, as the GUI offers and checks it, and
// returns the presets deleted with it: for a custom printer (a base printer that is not a system one),
// {"process": [names], "filament": [names]} of the user presets of `prints` and `filaments` made for it
// alone; otherwise an empty object. Throws ToolFailure for an unknown name (with similar names), a system,
// default or bundle preset, a base preset other presets inherit from (details: inherited_by), and a custom
// printer with presets attached unless `delete_attached` (details: attached).
nlohmann::json check_preset_deletable(const PresetCollection& presets, const std::string& name, bool delete_attached,
                                      const PresetCollection& prints, const PresetCollection& filaments);

// The slot colours filament_set_colour writes: one colour per slot of `current`, each #RRGGBB (stored in
// upper case, as the colour pickers store it) or the slot's current colour in any case. `mixed` flags the
// mixed slots, whose colour comes from their components and cannot change. Throws ToolFailure otherwise.
std::vector<std::string> check_filament_colours(std::vector<std::string> colours, const std::vector<std::string>& current,
                                                const std::vector<unsigned char>& mixed);

// filament_set_flushing's flush_multiplier to write: one per nozzle, or one value for all, each within the
// Flushing volumes dialog's 0..3. Throws ToolFailure otherwise.
std::vector<double> check_flush_multipliers(std::vector<double> multipliers, std::size_t nozzles);

// filament_set_flushing's flush_volumes_matrix to write: per nozzle, `slots` x `slots` volumes (row = from,
// column = to) in config order over every slot, as project_config and the G-code header hold it, limited
// as the Flushing volumes dialog limits them (g_max_flush_volume / the nozzle's multiplier). Throws
// ToolFailure for another length, a negative volume, a non-zero diagonal, or a changed entry of a mixed
// slot (flagged in `mixed`), which is never flushed and which the dialog keeps as `current` holds it.
std::vector<double> check_flush_matrix(std::vector<double> matrix, std::size_t slots, const std::vector<double>& multipliers,
                                       const std::vector<double>& current, const std::vector<unsigned char>& mixed);

// The keys of a plate's own settings, which plate_get_settings reads and plate_set_settings writes: what the Plate
// Settings dialog sets on one plate, and the filament grouping its filament map icon opens (filament_map_mode,
// filament_map). Its name and its lock are set by its icons, as plate_rename and plate_lock set them.
const std::vector<std::string>& plate_setting_keys();

// plate_list's place of a plate on the bed: {origin, size}, the [x, y] of its front left corner and its [width, depth]
// in mm, in the bed coordinates object positions are given in, read from the plate's rectangle on the bed.
nlohmann::json plate_place(PartPlate& plate);

// Why plate_set_settings refuses `map` as a plate's filament_map (the 1-based extruder of each filament), or empty
// when it does not: the map is the plate's own grouping, "Manual" as its `mode` (filament_map_mode, "" where the plate
// follows the project's grouping), with one extruder per filament of the `filaments` and each of the printer's
// `extruders`.
std::string filament_map_error(const std::string& mode, const std::vector<int>& map, int filaments, int extruders);

// preset_list's filters over `items`, one per preset as preset_list builds them: query (part of the name or alias),
// vendor and filament_type (in either case), source (system, user or project; a preset kept in the project is no user
// one), and the items marked compatible: false or installed: false only with include_incompatible or
// include_uninstalled. With `printer`, `fits` says whether an item's preset fits that printer preset: it stands for the
// item's compatible flag, which is for the selected printer, and sets it.
nlohmann::json filter_preset_items(nlohmann::json items, const nlohmann::json& args,
                                   const std::function<bool(const nlohmann::json&)>& fits);

// settings_set's extruders_count for the printer: what the Extruders field of the Printer settings takes, a whole
// number from 1 to MAXIMUM_EXTRUDER_NUMBER (or a string of one), and nothing while single extruder multi-material is
// on, which greys the field out. Throws ToolFailure otherwise.
std::size_t extruders_count_value(const nlohmann::json& value, bool single_extruder_multi_material);

// The printer variant settings_set's nozzle_diameter switches to, as the sidebar's nozzle diameter does: the plain
// diameter among `variants` (the printer model's, PresetCollection::diameters_of_selected_printer()) that every
// extruder is given, unless it is the `current` variant. Empty when the extruders differ, no variant has the diameter
// or the printer has it already: the value is then typed into the Printer settings.
std::string nozzle_diameter_variant(const std::vector<double>& diameters, const std::vector<std::string>& variants,
                                    const std::string& current);

// A plate's filament order as the Plate Settings dialog's drag canvas gives it: every slot 1..`slots` once.
// Throws ToolFailure otherwise.
std::vector<int> filament_order(const nlohmann::json& order, int slots);

// A plate's other_layers_print_sequence from [{from, to?, order}] (LayerPrintSequence: {{from, to}, order}),
// sorted by layer: layers from 2 (the first layer has its own order), `to` at least `from` and without it
// to the last layer, as the dialog's "End", ranges not overlapping, and each order a filament_order().
// Throws ToolFailure otherwise.
std::vector<std::pair<std::pair<int, int>, std::vector<int>>> layer_sequences(const nlohmann::json& ranges, int slots);

// The inverse of layer_sequences(): [{from, to, order}], `to` left out for a range up to the last layer.
nlohmann::json layer_sequences_json(const std::vector<std::pair<std::pair<int, int>, std::vector<int>>>& ranges);

// The heights of a plate's G-code layers as the Preview's layer slider lists them, read from its slice
// result: one layer per run of moves with one layer id (from the second move, as the viewer loads them),
// at the height of its last extrusion that is not custom G-code, or for a belt print at its print Z.
std::vector<double> gcode_layer_zs(const GCodeProcessorResult& result);

// plate_list's layer_gcode of a plate's codes: [{z, layer, type, filament, color, gcode}] by height, z
// rounded to the G-code's 0.001 mm; layer (1-based, as the slider numbers it) where `layer_zs` has a layer
// at or above z, the slider's rule for a stored height; filament and color for a filament or colour
// change, gcode for custom G-code.
nlohmann::json layer_gcode_json(const CustomGCode::Info& codes, const std::vector<double>& layer_zs);

// preferences_get's reply: {total, items}, an item for each preference of `shown` (the rows the Preferences dialog shows,
// Ops::shown_preferences()) that `args`' keys, tab and query leave (query: each of its words in the key, the label or
// the section, in any case), with its value typed by its kind: the text `shown_texts` gives for its key, which is what
// the dialog shows where that is not the stored value (Ops::shown_texts()), else the one in `config`'s app section; then
// {key, value} as stored for each named key outside `shown` that the section holds, and unknown_keys, with a note, for
// the named keys it lacks. The section also holds account and device state, so nothing outside `shown` is listed unasked.
nlohmann::json preferences_settings(const AppConfig& config, const std::vector<Ops::Preference>& shown, const nlohmann::json& args,
                                    const std::map<std::string, std::string>& shown_texts = {});

// Points the filament tab at the preset of filament slot `slot` (1-based, in range), as opening that slot's
// settings does, through the tab's switch, which asks about its unsaved changes. Returns the preset the tab
// edited before, or empty when it already edited that one. Throws ToolFailure when the switch is cancelled.
// UI thread.
std::string edit_filament_slot(int slot);

// ---- calibration (McpToolsProject.cpp) -------------------------------------------------------

// calibration_generate_test's arguments as the test's dialog takes them on `printer`: its defaults for what is left out (the
// pressure advance ones by the printer's extruder type, an input shaping Y range or frequency by the X one),
// temperatures clamped to the dialog field's 155-500 °C and rounded down to its 5 °C, and the dialog's rules
// (check_calib_params()). Adds to `warnings` what the dialog would say without refusing. Throws ToolFailure
// for an argument the test does not take, a value the dialog refuses, or a choice the printer does not offer.
Calib_Params calib_params_from_args(const nlohmann::json& args, const DynamicPrintConfig& printer, std::vector<std::string>& warnings);

// The parameters `params` holds for calibration_generate_test's `args`, in its argument names and units: what calibration_generate_test
// reports and records in the journal, so a replay does not depend on the dialogs' defaults.
nlohmann::json calib_params_json(const nlohmann::json& args, const Calib_Params& params, const DynamicPrintConfig& printer);

// Page a JSON array: {total, offset, items:[...]}.
nlohmann::json page(const nlohmann::json& items, const nlohmann::json& args, std::size_t default_limit = 100);

// Waits for a G-code export started between Plater export sequence numbers `before` and `after`
// (unchanged = it never started: `rejection` explains why) and reports how it ended; `revision` is
// the project revision when it started.
ToolResult await_gcode_export(ToolContext& ctx, const Ops::ExportTicket& ticket);

// Polls `poll` on the UI thread every 200 ms until it returns a non-null value or `timeout`
// passes; throws ToolFailure on timeout or cancellation.
nlohmann::json poll_ui(ToolContext& ctx,
                       const std::function<nlohmann::json()>& poll,
                       std::chrono::milliseconds timeout,
                       const std::string& what);
// Polls `poll` on the UI thread, under the inspect guard, every `interval` until it returns a non-null
// value, which it returns, or until `timeout` passes or the call is cancelled, when it returns null: for
// a wait that replies with what it got rather than failing.
nlohmann::json poll_until(ToolContext& ctx,
                          const std::function<nlohmann::json()>& poll,
                          std::chrono::milliseconds timeout,
                          std::chrono::milliseconds interval);

// Why the UI can't change when OrcaSlicer's window is not being drawn, or empty when it is drawn or
// can't tell. A compositor stops drawing a window while the display is off, and GTK then neither lays
// it out nor paints it: waits for the UI time out, lists can't scroll to a row and the 3D views capture
// blank. Asks for a frame and waits up to half a second for it, so only for a call that failed. Tool
// thread.
std::string undrawn_window(ToolContext& ctx);

// ---- app (McpToolsApp.cpp) -------------------------------------------------------------------

// app_log's lines of the log read from `in`: the last `lines` of those at or above `min_level` (trace, debug, info,
// warning, error, fatal).
std::vector<std::string> log_tail(std::istream& in, const std::string& min_level, std::size_t lines);

// ---- view (McpToolsView.cpp) -----------------------------------------------------------------

// What view_set sets outside the Design tab, as app_state's view gives it, under the names of view_set's arguments so
// that it can be passed back: sidebar_collapsed and sidebar_width (pixels), legend_collapsed and gcode_window, the 3D
// camera's projection (perspective or orthographic), the section view of Prepare and the Preview ({on}), and the display
// switches labels, overhangs, outline, wireframe, gridlines, axes and navigator. UI thread.
nlohmann::json view_state(Plater& plater);

// The page of the main window's notebook that view_set's `tab` names, of `pages`, the notebook's page ids: the page
// of one of the words of kTabWords (CommandCatalog.hpp), device being the page monitor; else the page of that id,
// when no word names that page (a plugin's page, the web Device page); else nothing.
std::optional<std::string> view_tab_page(const std::string& tab, const std::vector<std::string>& pages);

// ---- windows (McpToolsUi.cpp) ----------------------------------------------------------------

// What a window's title bar offers and where it runs, for window_request_refusal().
struct WindowAbilities
{
    bool main            = false; // the main window: it offers maximize and minimize
    bool modal           = false; // a modal dialog never minimizes
    bool can_maximize    = false; // wxMAXIMIZE_BOX in its style
    bool can_minimize    = false; // wxMINIMIZE_BOX in its style
    bool positions_known = true;  // false on Wayland (window_positions_known())
};
// Why window_set can't apply `args` to such a window, or empty when it can. Checks everything
// before anything changes.
std::string window_request_refusal(const nlohmann::json& args, const WindowAbilities& window);
// How window_set's wait for a window ended, for add_window_notes().
struct WindowSettling
{
    bool        limited    = false; // the size was held to the window's minimum or maximum size
    bool        held       = false; // the window held still in the state asked for
    bool        drawn      = false; // and the main window's 3D view drew at its size, or needn't
    bool        activation = false; // the request restored a minimized window by activating it (Wayland)
    int         seconds    = 3;     // how long the wait for the window lasted at most
    std::string undrawn;            // undrawn_window()'s reason the 3D view isn't drawn, or empty
};
// Adds to window_set's `reply`, which holds the size, position and state the window reads back,
// adjusted, the fields of `args` the window differs from, and a note on why: a size limit, the window
// manager, a compositor that refused to activate a minimized window, or a 3D view that didn't draw.
void add_window_notes(const nlohmann::json& args, const WindowSettling& settling, nlohmann::json& reply);
// Whether the shown 3D view has drawn at its canvas's size, asking for a frame when it hasn't: until it
// has, the camera's viewport is the old size, which a fit or a screen_rect would use. True when no 3D
// view is shown. UI thread.
bool view_drawn_at_size();
// {index, x, y, width, height}: the work area of display `index` (the screen less the taskbar or the
// dock and menu bar), in the units window_list reports. UI thread.
nlohmann::json display_area(unsigned index);

// ---- scene -----------------------------------------------------------------------------------

// The Prepare canvas, for edits made through the object list and the canvas selection as a person makes
// them: switches to it from another tab; ToolFailure in the assemble view or while a gizmo is open. UI thread.
GLCanvas3D& require_prepare_scene(Plater& plater);
// Selects an object's row, or with `volume` a part's, in the object list and nothing else, as clicking
// it does. UI thread.
void select_rows(ModelObject* object, ModelVolume* volume);

// Scale factors that turn `current` (mm per axis) into the sizes of object_transform's `size` argument, [x, y, z]
// with null for an axis that keeps its size. With one number and two nulls, every axis scales by that
// axis's factor. Throws ToolFailure for another length, no number, or a size that is not positive.
Vec3d size_factors(const Vec3d& current, const nlohmann::json& size);

// Applies object_arrange's spacing, rotate, align_y and multi_material to `settings` by the Arrange options
// panel's rules: Align to Y axis stays off while rotation is on. Settings not given keep their values.
// Returns why an asked-for value did not apply, or "".
std::string apply_arrange_args(GLCanvas3D::ArrangeSettings& settings, const nlohmann::json& args);
// The settings as object_arrange replies with them and records them for replay.
nlohmann::json arrange_settings_json(const GLCanvas3D::ArrangeSettings& settings);

// An object as object_list describes it: its filament, auto_drop, overrides, mesh errors, instances (without the
// plate, which the GUI's plate registry knows), parts, and its height ranges and variable layer height
// profile when it has them. Bounding boxes, on the bed and of model parts only, are {min, max, size}.
nlohmann::json object_info(const ModelObject& object, std::size_t index);
// An object as object_list lists it without detail: enough to name it in a call and tell it from the others. Its
// index, id, name, filament, part count, and the size of its bounding box as object_info() gives it (null for an
// object with no instance).
nlohmann::json object_summary(const ModelObject& object, std::size_t index);
// A part as object_list describes it: its type, filament, placement on the bed measured on instance 0,
// rotation and scale in the object's frame, mesh, paint, and what a text or SVG part was made from.
nlohmann::json volume_info(const ModelVolume& volume, std::size_t index);
// What object_list replies with, laid out as the Objects panel is: `plates`, each {index, name, objects}
// with the objects that have an instance on it, and `outside`, the objects with an instance on no plate.
// An object row lists the instances of its plate only, so an object on two plates has a row under each.
// A row is object_summary()'s with `instances` the indices of those instances and `instance_ids` their ids in
// the same order, or with `detail` in `args` object_info()'s with their entries, each with its id.
// `plate_of(object, instance)` is the plate an instance is on, or -1, and `plate_names` has a name per
// plate. Of `args`, plate lists that plate alone, without `outside`, and limit (default 200) and offset
// page the objects listed, in the model's order: total counts them, each once, and next_offset is there
// while more follow. A page lists its objects under every plate they are on.
nlohmann::json objects_by_plate(const Model& model, const std::vector<std::string>& plate_names,
                                const std::function<int(std::size_t object, std::size_t instance)>& plate_of,
                                const nlohmann::json& args);

// The plate an instance is on, or -1 on none: the plate registry's answer or, for an instance the
// registry does not hold, as Fill bed leaves its new ones, the first plate it stands on. UI thread.
int instance_plate(PartPlateList& plates, int object, int instance);

// What `canvas` has selected, in the indices object_select takes: its objects, the instances [[object, instance]]
// they are selected in and, when the selection holds parts rather than whole instances, those parts
// [[object, part]]; wipe_tower when a prime tower is selected. UI thread.
nlohmann::json selection_state(GLCanvas3D& canvas);
// A part's type as object_list and part_add name it: part, negative, modifier, support_blocker,
// support_enforcer, precise_seam (any subtype), else other.
const char* volume_type_name(ModelVolumeType type);
// The type part_add's `type` adds, as the context menu's Add items do (precise_seam: a Center seam);
// ModelVolumeType::INVALID for another name.
ModelVolumeType volume_type_from_name(const std::string& name);
// The precise seam a subtype name stands for, as object_list names a seam's subtype (center, left, right, enforced,
// blocked, neutral); ModelVolumeType::INVALID for another name.
ModelVolumeType precise_seam_subtype(const std::string& name);
// part_add's size of a shape (cube, cylinder, sphere, cone, disc, torus): one positive number of mm
// per field of the Primitive size dialog, in its order. Empty when size is null (the built-in size).
// Throws ToolFailure naming the fields.
std::vector<double> part_shape_size(const std::string& shape, const nlohmann::json& size);

// ---- layers (McpToolsLayers.cpp) -------------------------------------------------------------

void register_layer_tools(CommandCatalog& catalog);

// The settings a height range modifier takes: layer_height, the per-region process settings the object
// list offers a part or a range (with `filaments` filament slots), and extruder, the range's filament.
std::vector<std::string> layer_range_option_keys(int filaments);
// A height range as messages give it: "0-5", "12.5-20".
std::string height_range_text(const Slic3r::t_layer_height_range& range);
// object_set_height_ranges' ranges, each {min_z, max_z}, as an object's layer_config_ranges: a range of `current`, the
// object's, with the same heights keeps its settings, and any other starts from `defaults`, what the object list gives
// a new range. Throws ToolFailure for a range that is not 0 <= min_z < max_z or that overlaps another (touching is
// fine).
t_layer_config_ranges layer_config_ranges_from_json(const nlohmann::json& ranges, const t_layer_config_ranges& current,
                                                    const DynamicPrintConfig& defaults);
// settings_set's check of the `values` it writes to a height range whose settings are `range`: a layer_height given is
// within `layer_height_bounds(extruder)` for the range's filament after the call (the one given, else the range's), as the
// range editor checks what a person types. Throws ToolFailure otherwise.
void check_height_range_layer_height(const DynamicPrintConfig& values, const DynamicPrintConfig& range,
                                     const std::function<std::pair<double, double>(int extruder)>& layer_height_bounds);
// An object's height range modifiers as object_list and object_set_height_ranges report them:
// [{min_z, max_z, values: {key: value serialized as settings_get returns it}}], lowest first.
nlohmann::json layer_config_ranges_json(const t_layer_config_ranges& ranges);
// An object's layer_height_profile ([z0, h0, z1, h1, ...]) as object_list and object_set_variable_layer_height
// report it: {points, min_height, max_height}, heights to 0.001 mm; null unless the object list marks the
// object's layer height as variable (more than 4 entries).
nlohmann::json layer_height_profile_json(const std::vector<double>& profile);

// The names plate_set_custom_gcode and plate_list give the types of layer codes: pause, custom, template,
// change_filament, and color_change for an old project's colour change.
const char* custom_gcode_type_name(CustomGCode::Type type);
// The type plate_set_custom_gcode adds for a name; CustomGCode::Unknown for any other, color_change included,
// since the layer slider adds none.
CustomGCode::Type custom_gcode_type_from_name(const std::string& name);
// plate_set_custom_gcode's clear, remove and add, in that order, on a layer slider that holds a plate's layers
// and codes. Everything is checked against the slider's layers and codes before it changes, with the
// rules of its menus: one code per layer, a template only when the printer has one (`has_template`), a
// filament change only with two or more filaments, as many as the slider has (`filaments`, the slots now),
// and while the slider offers it (else `no_filament_change` says why). Then the slider's own edits run:
// delete, add, and on a layer with a code of the same type Edit Custom G-code or Change Filament. Returns
// whether the codes changed; the slider then has their hand-over to the plater pending. Throws ToolFailure.
bool edit_layer_gcode(IMSlider& slider, const nlohmann::json& args, int filaments, bool has_template,
                      const std::string& no_filament_change);

}}} // namespace Slic3r::GUI::Mcp

// ---- slice results ---------------------------------------------------------------------------

namespace libvgcode {
enum class EGCodeExtrusionRole : std::uint8_t;
enum class EOptionType : std::uint8_t;
} // namespace libvgcode

namespace Slic3r {

struct GCodeProcessorResult;

namespace GUI {

struct Camera;
class GCodeViewer;

namespace Mcp {

// The camera view_set leaves, as its reply's camera_state and app_state.camera give it: type ("ortho"
// or "perspective"), look_at, the point it looks at, and position ([x, y, z] in mm, to 0.001), zoom (viewport
// pixels per mm at look_at's depth) and click_px_per_mm (the zoom in the logical pixels the input tools take, the zoom over
// `canvas_scale`), both to 0.0001, and the elevation and azimuth orbit uses, in whole degrees.
nlohmann::json camera_state(Camera& camera, double canvas_scale);

// What a sliced plate reports in plate_slice's reply: print time, filament totals with the flush, cost,
// layer count, max Z, filament changes, and every filament slot of the project (zeros for slots the
// plate does not use, so filaments[i] is slot i + 1) split into model, support, flush and prime
// tower. Read from `statistics`, the plate's gcode_statistics_json() parsed, so the reply agrees with
// the legend and the statistics files; the silent-mode time and extruder changes, which that file
// leaves out, come from `result`.
nlohmann::json plate_summary(const nlohmann::json& statistics, const GCodeProcessorResult& result);

// plate_slice_stats's tables, from the same parsed statistics, which sum the moves as the Preview does.
// Per feature type, keyed by the English names of the G-code's ;FEATURE: tags: seconds, share of
// the print time and grams, plus a Travel row with its distance. The shares need not add up to 100:
// the print time also counts retractions, wipes, Z moves, tool changes and the prepare time.
nlohmann::json preview_features(const nlohmann::json& statistics);
// Every layer, numbered from 1 as the Preview's layer slider and view_set count them, with its Z,
// its height above the layer below (a layer lower than that one, the next object of a by-object
// print, starts again from the bed), seconds and grams; with `with_features`, the seconds of each
// feature type on it.
nlohmann::json preview_layers(const nlohmann::json& statistics, bool with_features);
// Grams per object instance, model and support. The G-code names an instance by its object's name
// and its index among that object's instances, so an entry gets the object_list index `object`
// only when exactly one of `object_names` (the model's objects, in order) has its name.
nlohmann::json preview_objects(const nlohmann::json& statistics, const std::vector<std::string>& object_names);
// What the Preview shows: view type, time mode, the layer slider's range (1-based) with the Z of
// both ends, and the layer count; null when it has no toolpaths. UI thread.
nlohmann::json preview_state(GCodeViewer& viewer);
// Whether the Preview's toolpaths are `result`'s. It loads a plate's only while it is shown, so after
// another plate is selected it keeps the last one's. UI thread.
bool viewer_holds(const GCodeViewer& viewer, const GCodeProcessorResult* result);
// The names plate_slice_stats and view_set give the legend's rows: a feature type by the English name of
// its ;FEATURE: tag, an option by a key; null for the options that are markers, not rows.
std::string legend_feature_key(libvgcode::EGCodeExtrusionRole role);
const char* legend_option_key(libvgcode::EOptionType type);

}} // namespace GUI::Mcp
} // namespace Slic3r
