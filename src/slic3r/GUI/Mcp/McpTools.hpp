#pragma once

#include "McpTypes.hpp"

#include "libslic3r/CustomGCode.hpp"
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
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

class AppConfig;
struct Calib_Params;
class ConfigOptionDef;
class DynamicPrintConfig;
class ModelObject;
class ModelVolume;
enum class ModelVolumeType : int;
class Preset;
class PresetCollection;
struct GCodeProcessorResult;
namespace CustomGCode { struct Info; }

namespace GUI {

class IMSlider;
class Plater;

namespace Mcp {

class McpService;
struct Script;
struct ScriptContext;

// Each tool family registers itself with the service.
void register_app_tools(McpService& service);
void register_action_tools(McpService& service);
void register_project_tools(McpService& service);
void register_config_tools(McpService& service);
void register_scene_tools(McpService& service);
void register_view_tools(McpService& service);
void register_ui_tools(McpService& service);
void register_script_tools(McpService& service);
void register_record_tools(McpService& service);
void register_cad_tools(McpService& service);

// How run_script_job() runs a script beyond what the script says.
struct ScriptRunOptions
{
    std::optional<std::size_t> only_step;    // 1-based; the whole script when empty
    bool                       write_result = true; // result.json in the output folder
    // Called on the job thread as each step starts and ends (see ScriptHost).
    std::function<void(std::size_t)>           step_started;
    std::function<void(const nlohmann::json&)> step_finished;
};

// Runs a parsed script as the calling job, as run_script does: steps are child calls, dialog rules
// the script registers end with it, and result.json goes to sctx.out_dir (created if missing). It
// stops before the next step, and stops waiting on a step's job, once the call is cancelled: the
// service stops, or a stop was asked for any job in the call's lineage (McpService::request_stop), so
// a nested run_script stops with the run that holds it. Returns run_script()'s result with out_dir (and result_file).
nlohmann::json run_script_job(McpService& service, ToolContext& ctx, const Script& script, ScriptContext& sctx,
                              const ScriptRunOptions& options);

// ---- schema helpers --------------------------------------------------------------------------

using Property = std::pair<const char*, nlohmann::json>;

// {"type":"object","properties":{...},"required":[...],"additionalProperties":false}
nlohmann::json object_schema(std::initializer_list<Property> properties, std::vector<std::string> required = {});
nlohmann::json prop(const char* type, const std::string& description);
nlohmann::json enum_prop(std::vector<std::string> values, const std::string& description);
nlohmann::json array_prop(nlohmann::json items, const std::string& description);

// Pure observation: inline, read-only, inspect guard, not journaled.
ToolDef inspect_tool(std::string name, std::string title, std::string description, nlohmann::json schema, ToolHandler handler);
// Anything that changes the app: a job under the mutate guard, journaled.
ToolDef mutate_tool(std::string name, std::string title, std::string description, nlohmann::json schema, ToolHandler handler,
                    bool destructive = false);

// ---- handler helpers -------------------------------------------------------------------------

// One more UI round trip after a mutation, so events it queued are handled before the reply, and
// any 3D view it showed has its GL surface (on Wayland, a moment after it is shown).
void settle(ToolContext& ctx);

// The plater, or ToolFailure while the main window is not up yet (startup, language change).
// UI thread only.
Plater& ui_plater();

// Typed argument access with defaults; schema validation has already checked the types.
std::string arg_string(const nlohmann::json& args, const char* key, const std::string& fallback = {});
int         arg_int(const nlohmann::json& args, const char* key, int fallback);
double      arg_double(const nlohmann::json& args, const char* key, double fallback);
bool        arg_bool(const nlohmann::json& args, const char* key, bool fallback);

// A setting value from JSON in the serialized form set_deserialize accepts: "3", "0.2", "1"/"0"
// for booleans, "a,b" for arrays. A list of strings (`key` names a coStrings option, such as
// post_process) is quoted and separated by semicolons, as ConfigOptionStrings writes it, since its
// entries may contain commas. Throws ToolFailure for values with no such form.
std::string to_config_string(const nlohmann::json& value, const std::string& key = {});

// ---- settings scopes (McpToolsConfig.cpp) ----------------------------------------------------

// The keys the object scope (or, with `is_part`, the volume scope) takes: those of `offered`
// (SettingsFactory::get_options()) that the object list offers with `filaments` filament slots, and
// extruder, the object's or part's filament.
std::vector<std::string> model_scope_options(const std::vector<std::string>& offered, bool is_part, int filaments);

// What `key` is for an object (`volume` null) or one of its parts, and where it comes from:
// {"effective", "source"}, the part's override ("part"), else the object's ("object"), else the
// process's ("process"), else the option's default ("default"; extruder 1). An extruder of 0 is no
// override. For an object, "parts" lists [{volume, value}] for the parts and modifiers overriding
// `key`, each applying only inside its own volume.
nlohmann::json effective_setting(const std::string& key, const DynamicPrintConfig& process, const ModelObject& object,
                                 const ModelVolume* volume);

// Checks save_preset's `name` against `presets` as the Save preset dialog does, and returns whether the
// preset is kept inside the project: where an existing preset of that name is, else `inside_project`,
// else where the edited preset is. Throws ToolFailure for a name the dialog refuses, for replacing
// another existing preset without `overwrite`, and for an `inside_project` that contradicts where the
// existing preset is, which the dialog does not let a person change.
bool save_preset_inside_project(const PresetCollection& presets, const std::string& name, const nlohmann::json& args);

// get_settings' meta of an option: label, description (its whole tooltip: what it does, when to change
// it, what it affects), type, mode, category, unit, range, and for an enum its values with their labels
// (the untranslated texts the GUI shows), in the same order.
nlohmann::json option_meta(const ConfigOptionDef& def);

// get_settings' search: the keys of `within` that `query` describes, best first, at most `limit`.
// A query word matches a word of a key (split at _), its label or its tooltip that starts with it, so
// "retract" finds "retraction". Settings matching more of the query's words come first, then those
// matching more of them in the key or label, so of two that match alike, the one the query names
// outranks one whose help text only mentions it. Words of fewer than three letters are left out when
// the query has longer ones.
std::vector<std::string> rank_settings(const std::string& query, const std::vector<std::string>& within, std::size_t limit);

// For a per-variant or per-extruder vector `key` of a preset's `config` that has more than one entry:
// {"variants": [what each entry is for], "active": [the entry each extruder prints with, extruder 1
// first]}; a per-extruder key has no "active", since each extruder uses its own entry. `extruder_types`
// and `nozzle_volume_types` are the printer's and the project's, one per extruder. Null for any other key.
nlohmann::json variant_entries(const std::string& key, const DynamicPrintConfig& config, const std::vector<int>& extruder_types,
                               const std::vector<int>& nozzle_volume_types);

// The serialized value set_settings writes for `key` of the preset whose config is `config`: `value`
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

// set_settings' "changed": {key: value} for every key whose value differs between `before` and `after`,
// with null for a key `after` no longer has.
nlohmann::json changed_settings(const DynamicPrintConfig& before, const DynamicPrintConfig& after);

// The keys of `changes`, unsaved changes as the tab marks them ({key: value}, or key#index for one entry of
// a vector), whose value `config` no longer holds; each key once.
nlohmann::json discarded_changes(const nlohmann::json& changes, const DynamicPrintConfig& config);

// The process, filament and printer presets being edited that have unsaved changes:
// {type: {name, changes: {key or key#index: value}}}. UI thread.
nlohmann::json unsaved_changes();

// What a call did to the unsaved changes `before` (from unsaved_changes()): "discarded", the keys whose
// value is gone, and "modified", the keys still unsaved, each {type: {name, keys}} with only the presets
// that have keys, and left out when there are none. UI thread.
nlohmann::json unsaved_changes_outcome(const nlohmann::json& before);

// Checks that delete_preset may delete the preset `name` of `presets`, as the GUI offers and checks it, and
// returns the presets deleted with it: for a custom printer (a base printer that is not a system one),
// {"process": [names], "filament": [names]} of the user presets of `prints` and `filaments` made for it
// alone; otherwise an empty object. Throws ToolFailure for an unknown name (with similar names), a system,
// default or bundle preset, a base preset other presets inherit from (details: inherited_by), and a custom
// printer with presets attached unless `delete_attached` (details: attached).
nlohmann::json check_preset_deletable(const PresetCollection& presets, const std::string& name, bool delete_attached,
                                      const PresetCollection& prints, const PresetCollection& filaments);

// The project scope's filament_colour to write: one colour per slot of `current`, each #RRGGBB (stored in
// upper case, as the colour pickers store it) or the slot's current colour in any case. `mixed` flags the
// mixed slots, whose colour comes from their components and cannot change. Throws ToolFailure otherwise.
std::vector<std::string> check_filament_colours(std::vector<std::string> colours, const std::vector<std::string>& current,
                                                const std::vector<unsigned char>& mixed);

// The project scope's flush_multiplier to write: one per nozzle, or one value for all, each within the
// Flushing volumes dialog's 0..3. Throws ToolFailure otherwise.
std::vector<double> check_flush_multipliers(std::vector<double> multipliers, std::size_t nozzles);

// The project scope's flush_volumes_matrix to write: per nozzle, `slots` x `slots` volumes (row = from,
// column = to) in config order over every slot, as project_config and the G-code header hold it, limited
// as the Flushing volumes dialog limits them (g_max_flush_volume / the nozzle's multiplier). Throws
// ToolFailure for another length, a negative volume, a non-zero diagonal, or a changed entry of a mixed
// slot (flagged in `mixed`), which is never flushed and which the dialog keeps as `current` holds it.
std::vector<double> check_flush_matrix(std::vector<double> matrix, std::size_t slots, const std::vector<double>& multipliers,
                                       const std::vector<double>& current, const std::vector<unsigned char>& mixed);

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

// list_plates' layer_gcode of a plate's codes: [{z, layer, type, filament, color, gcode}] by height, z
// rounded to the G-code's 0.001 mm; layer (1-based, as the slider numbers it) where `layer_zs` has a layer
// at or above z, the slider's rule for a stored height; filament and color for a filament or colour
// change, gcode for custom G-code.
nlohmann::json layer_gcode_json(const CustomGCode::Info& codes, const std::vector<double>& layer_zs);

// get_settings on the preferences scope: [{key, value}] for the `keys` of `args` that `config`'s app section,
// where the Preferences dialog keeps its settings, holds, paged, and unknown_keys for the others with a note
// that a preference never changed is not stored. Values are as stored. Throws ToolFailure without keys: the
// section also holds account and device state, so nothing is listed unasked.
nlohmann::json preferences_settings(const AppConfig& config, const nlohmann::json& args);

// Points the filament tab at the preset of filament slot `slot` (1-based, in range), as opening that slot's
// settings does, through the tab's switch, which asks about its unsaved changes. Returns the preset the tab
// edited before, or empty when it already edited that one. Throws ToolFailure when the switch is cancelled.
// UI thread.
std::string edit_filament_slot(int slot);

// ---- calibration (McpToolsProject.cpp) -------------------------------------------------------

// calibrate's arguments as the test's dialog takes them on `printer`: its defaults for what is left out (the
// pressure advance ones by the printer's extruder type, an input shaping Y range or frequency by the X one),
// temperatures clamped to the dialog field's 155-500 °C and rounded down to its 5 °C, and the dialog's rules
// (check_calib_params()). Adds to `warnings` what the dialog would say without refusing. Throws ToolFailure
// for an argument the test does not take, a value the dialog refuses, or a choice the printer does not offer.
Calib_Params calib_params_from_args(const nlohmann::json& args, const DynamicPrintConfig& printer, std::vector<std::string>& warnings);

// The parameters `params` holds for calibrate's `args`, in its argument names and units: what calibrate
// reports and records in the journal, so a replay does not depend on the dialogs' defaults.
nlohmann::json calib_params_json(const nlohmann::json& args, const Calib_Params& params, const DynamicPrintConfig& printer);

// Page a JSON array: {total, offset, items:[...]}.
nlohmann::json page(const nlohmann::json& items, const nlohmann::json& args, std::size_t default_limit = 100);

// Waits for a G-code export started between Plater export sequence numbers `before` and `after`
// (unchanged = it never started: `rejection` explains why) and reports how it ended; `revision` is
// the project revision when it started.
ToolResult await_gcode_export(ToolContext& ctx, uint64_t before, uint64_t after, uint64_t revision, const std::string& rejection);

// Polls `poll` on the UI thread every 200 ms until it returns a non-null value or `timeout`
// passes; throws ToolFailure on timeout or cancellation.
nlohmann::json poll_ui(ToolContext& ctx,
                       const std::function<nlohmann::json()>& poll,
                       std::chrono::milliseconds timeout,
                       const std::string& what);

// Why the UI can't change when OrcaSlicer's window is not being drawn, or empty when it is drawn or
// can't tell. A compositor stops drawing a window while the display is off, and GTK then neither lays
// it out nor paints it: waits for the UI time out, lists can't scroll to a row and the 3D views capture
// blank. Asks for a frame and waits up to half a second for it, so only for a call that failed. Tool
// thread.
std::string undrawn_window(ToolContext& ctx);

// ---- scene -----------------------------------------------------------------------------------

// The Prepare canvas, for edits made through the object list and the canvas selection as a person makes
// them: switches to it from another tab; ToolFailure in the assemble view or while a gizmo is open. UI thread.
GLCanvas3D& require_prepare_scene(Plater& plater);
// Selects an object's row, or with `volume` a part's, in the object list and nothing else, as clicking
// it does. UI thread.
void select_rows(ModelObject* object, ModelVolume* volume);

// Scale factors that turn `current` (mm per axis) into the sizes of transform's `size` argument, [x, y, z]
// with null for an axis that keeps its size. With one number and two nulls, every axis scales by that
// axis's factor. Throws ToolFailure for another length, no number, or a size that is not positive.
Vec3d size_factors(const Vec3d& current, const nlohmann::json& size);

// Applies arrange's spacing, rotate, align_y and multi_material to `settings` by the Arrange options
// panel's rules: Align to Y axis stays off while rotation is on. Settings not given keep their values.
// Returns why an asked-for value did not apply, or "".
std::string apply_arrange_args(GLCanvas3D::ArrangeSettings& settings, const nlohmann::json& args);
// The settings as arrange replies with them and records them for replay.
nlohmann::json arrange_settings_json(const GLCanvas3D::ArrangeSettings& settings);

// An object as list_objects describes it: its filament, auto_drop, overrides, instances (without the
// plate, which the GUI's plate registry knows), parts, and its height ranges and variable layer height
// profile when it has them. Bounding boxes, on the bed and of model parts only, are {min, max, size}.
nlohmann::json object_info(const ModelObject& object, std::size_t index);
// A part as list_objects describes it: its type, filament, placement on the bed measured on instance 0,
// rotation and scale in the object's frame, mesh, paint, and what a text or SVG part was made from.
nlohmann::json volume_info(const ModelVolume& volume, std::size_t index);

// A part's type as list_objects and add_part name it: part, negative, modifier, support_blocker,
// support_enforcer, precise_seam (any subtype), else other.
const char* volume_type_name(ModelVolumeType type);
// The type add_part's `type` adds, as the context menu's Add items do (precise_seam: a Center seam);
// ModelVolumeType::INVALID for another name.
ModelVolumeType volume_type_from_name(const std::string& name);
// add_part's size of a shape (cube, cylinder, sphere, cone, disc, torus): one positive number of mm
// per field of the Primitive size dialog, in its order. Empty when size is null (the built-in size).
// Throws ToolFailure naming the fields.
std::vector<double> part_shape_size(const std::string& shape, const nlohmann::json& size);

// ---- layers (McpToolsLayers.cpp) -------------------------------------------------------------

void register_layer_tools(McpService& service);

// The settings a height range modifier takes: layer_height, the per-region process settings the object
// list offers a part or a range (with `filaments` filament slots), and extruder, the range's filament.
std::vector<std::string> layer_range_option_keys(int filaments);
// set_layer_ranges' ranges as an object's layer_config_ranges. Each starts from `defaults`, what the
// object list gives a new range, and takes its values, parsed strictly. Throws ToolFailure for a range
// that is not 0 <= min_z < max_z or that overlaps another (touching is fine), a key a range does not
// take, an invalid value, an extruder above `filaments`, or a layer_height given outside
// `layer_height_bounds(extruder)`.
t_layer_config_ranges layer_config_ranges_from_json(const nlohmann::json& ranges, const DynamicPrintConfig& defaults, int filaments,
                                                    const std::function<std::pair<double, double>(int extruder)>& layer_height_bounds);
// An object's height range modifiers as list_objects and set_layer_ranges report them:
// [{min_z, max_z, values: {key: value serialized as get_settings returns it}}], lowest first.
nlohmann::json layer_config_ranges_json(const t_layer_config_ranges& ranges);
// An object's layer_height_profile ([z0, h0, z1, h1, ...]) as list_objects and set_layer_height_profile
// report it: {points, min_height, max_height}, heights to 0.001 mm; null unless the object list marks the
// object's layer height as variable (more than 4 entries).
nlohmann::json layer_height_profile_json(const std::vector<double>& profile);

// The names set_layer_gcode and list_plates give the types of layer codes: pause, custom, template,
// change_filament, and color_change for an old project's colour change.
const char* custom_gcode_type_name(CustomGCode::Type type);
// The type set_layer_gcode adds for a name; CustomGCode::Unknown for any other, color_change included,
// since the layer slider adds none.
CustomGCode::Type custom_gcode_type_from_name(const std::string& name);
// set_layer_gcode's clear, remove and add, in that order, on a layer slider that holds a plate's layers
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

class GCodeViewer;

namespace Mcp {

// What a sliced plate reports in the slice reply: print time, filament totals with the flush, cost,
// layer count, max Z, filament changes, and every filament slot of the project (zeros for slots the
// plate does not use, so filaments[i] is slot i + 1) split into model, support, flush and prime
// tower. Read from `statistics`, the plate's gcode_statistics_json() parsed, so the reply agrees with
// the legend and the statistics files; the silent-mode time and extruder changes, which that file
// leaves out, come from `result`.
nlohmann::json plate_summary(const nlohmann::json& statistics, const GCodeProcessorResult& result);

// get_preview's tables, from the same parsed statistics, which sum the moves as the Preview does.
// Per feature type, keyed by the English names of the G-code's ;FEATURE: tags: seconds, share of
// the print time and grams, plus a Travel row with its distance. The shares need not add up to 100:
// the print time also counts retractions, wipes, Z moves, tool changes and the prepare time.
nlohmann::json preview_features(const nlohmann::json& statistics);
// Every layer, numbered from 1 as the Preview's layer slider and set_view count them, with its Z,
// its height above the layer below (a layer lower than that one, the next object of a by-object
// print, starts again from the bed), seconds and grams; with `with_features`, the seconds of each
// feature type on it.
nlohmann::json preview_layers(const nlohmann::json& statistics, bool with_features);
// Grams per object instance, model and support. The G-code names an instance by its object's name
// and its index among that object's instances, so an entry gets the list_objects index `object`
// only when exactly one of `object_names` (the model's objects, in order) has its name.
nlohmann::json preview_objects(const nlohmann::json& statistics, const std::vector<std::string>& object_names);
// What the Preview shows: view type, time mode, the layer slider's range (1-based) with the Z of
// both ends, and the layer count; null when it has no toolpaths. UI thread.
nlohmann::json preview_state(GCodeViewer& viewer);
// Whether the Preview's toolpaths are `result`'s. It loads a plate's only while it is shown, so after
// another plate is selected it keeps the last one's. UI thread.
bool viewer_holds(const GCodeViewer& viewer, const GCodeProcessorResult* result);
// The names get_preview and set_view give the legend's rows: a feature type by the English name of
// its ;FEATURE: tag, an option by a key; null for the options that are markers, not rows.
std::string legend_feature_key(libvgcode::EGCodeExtrusionRole role);
const char* legend_option_key(libvgcode::EOptionType type);

}} // namespace GUI::Mcp
} // namespace Slic3r
