#pragma once

#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/CommandTypes.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

// For a call of tool_call ({tool, args}): replaces `command` and `args` with its target's, normalized and
// validated, so the call runs and is journaled as the target's own. Returns the call's error, or nothing (also
// for any other command, which it leaves as it is): invalid_argument for a target that names no command, tool_call
// itself, or arguments the target refuses; for a target the catalog leaves out, the code it was left out with
// (CommandCatalog::unknown_command_code()).
std::optional<ToolFailure> resolve_call(const CommandCatalog& catalog, std::shared_ptr<const NativeCommand>& command,
                                        nlohmann::json& args);

// ---- arguments and name matching (McpArguments.cpp) ------------------------------------------

// Validates `args` against the subset of JSON Schema the tool catalog uses (object properties with
// type, enum, minimum/maximum, required, additionalProperties:false, array items, and the "if" and "then"
// of with_silent()). Returns an empty string when valid, else a message naming the offending property.
std::string validate_arguments(const nlohmann::json& schema, const nlohmann::json& args);

// Whether `v` is a whole number as the schemas' integer type takes one: a JSON integer, or a float with no fraction
// within the int64 range the callers read it into. A float is checked without a cast, which past that range would be
// undefined. The checks that read an integer argument themselves take the same numbers (target indices, generation).
bool is_whole_number(const nlohmann::json& v);

// Whether `args` is a call with no arguments: an object with no members, or null. A Design command and a menu
// item's function do their person's form then (they ask, as a person's pick does).
bool person_form(const nlohmann::json& args);
// The arguments of each of `command`'s person's forms: {} for its own action (has_own_action), and the args of each
// item of its actions that collects no value (no input), in their order.
std::vector<nlohmann::json> person_forms(const NativeCommand& command);
// Whether `args`, null as {}, are a person's form of `command`: exactly the arguments of one of person_forms(). Such
// a call does what a person's pick of that action does, and may ask; any other call is its silent form.
bool person_form(const NativeCommand& command, const nlohmann::json& args);
// `args` without `silent`, which only chooses the silent form.
nlohmann::json silent_args(nlohmann::json args);
// The schema of a two-form command from its silent form's `schema`: it gains the property `silent`, which
// takes only true and asks for the silent form with its defaults, described by what that form still requires and by
// which calls are the person's forms (none, an action's arguments, or both), and a top-level `required` list moves under
// "then", with an "if" that holds for every call but the person's forms, `person_args` (never empty), so that a
// person's form needs none of them: {"minProperties": 1} where the only one is the call with no arguments, else
// {"not": {"enum": [<each of person_args>]}}.
nlohmann::json with_silent(nlohmann::json schema, const std::vector<nlohmann::json>& person_args = {nlohmann::json::object()});

// Readies a call's arguments for `command`, as every caller of a command does: normalizes `args` in place
// (normalize_arguments()) and returns why validate_arguments() refuses them; empty when they are valid.
std::string argument_error(const NativeCommand& command, nlohmann::json& args);

// The refusal of a call whose arguments for the command `name` the schema check found wrong, `error` saying how:
// "Invalid arguments for <name>: <error>", code invalid_argument, and a hint that tool_describe gives the command's
// parameters. Every caller that checks a call's arguments against a schema refuses with it.
ToolFailure argument_refusal(const std::string& name, const std::string& error);

// Rewrites, before validation, what agents often send in place of what the schema asks for: an
// argument under a name the tool does not take but that stands for one it does, which the call leaves
// out and the value fits (ref for target, key for keys, text for value, action for id or button, object for objects,
// plate for plates, filter for query, settings for values, path for save_path, index for feature, params or
// values for fields, and type for the one argument that picks a command's variant where that has another name, as
// project_export's format, when it takes exactly one such and no type), a value written in
// another JSON form (a number, boolean, array or object as a string, a single item for an array),
// a value outside its argument's enum that only one omitted argument's enum takes (view_set's canvas
// "front" as camera), and the x and y, or width and height, that window_list reports, both given, as
// the position or size array of a tool that takes that array, of integers, and neither part. Anything
// that still does not fit is left for validate_arguments to report.
void normalize_arguments(const nlohmann::json& schema, nlohmann::json& args);

// The Levenshtein distance between `a` and `b`: the fewest insertions, deletions and substitutions of one
// character that turn one into the other. A "did you mean" ranks the names a mistyped one may stand for by it,
// each lookup with its own threshold.
std::size_t edit_distance(const std::string& a, const std::string& b);

// The first sentence of `text` on one line, for a listing: up to the first full stop followed by a space
// or the end, "e.g." and "i.e." aside; the whole text when it has none.
std::string first_sentence(const std::string& text);

}}} // namespace Slic3r::GUI::Mcp
