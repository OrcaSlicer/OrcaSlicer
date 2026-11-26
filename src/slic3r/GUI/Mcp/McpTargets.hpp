#pragma once

#include "McpTools.hpp"

#include "slic3r/GUI/Operations/ObjectOps.hpp"
#include "slic3r/GUI/Operations/Outcome.hpp"

#include <nlohmann/json.hpp>

#include <vector>

namespace Slic3r {

class Model;

namespace GUI {

class Selection;

// What a command acts on, read from its arguments, or from the Prepare view's selection when they name nothing: one
// reading of the target arguments for every command that takes objects, instances or parts. The struct is the
// operations' (Ops::Targets), since the operations take it.
namespace Mcp {

using Ops::Targets;

// The kinds of target a command takes, or'ed, with how the selection is read for a command that asks for it.
enum TargetKind : unsigned {
    kObjects   = 1,
    kInstances = 2,
    kParts     = 4,
    // An instance or a part selected stands for its object, for a command that takes objects and not them. Asked for by
    // object_set_instances, whose items read the selected object so (Plater::get_selected_object_idx()), and by the
    // commands of one object that have no menu item: part_add, object_set_variable_layer_height and object_set_height_ranges.
    kObjectOfSelection = 8,
    // A part selected stands for its instance, for a command that takes instances and not parts. Asked for by
    // object_separate_instances, as Set as Individual Objects reads it (ObjectList::split_instances()).
    kInstanceOfSelectedPart = 16,
};

// The schema properties that name a command's targets of `kinds`, worded as object_select and object_transform word
// them: objects, object_ids, instances ([[object, instance], ...]), instance_ids, parts ([[object, part], ...]) and
// part_ids; or with `one`, for a command of one target, object and object_id, then instance and instance_id, and part
// and part_id, each of the object. Each target is named by index or by id, or by both, the id then a guard.
std::vector<Property> target_props(unsigned kinds, bool one);

// `args` with the index form of every object, part and instance they name by id alone (kIdArguments): object_id gives
// object, part_id and instance_id their object and their index in it, object_ids, part_ids and instance_ids the lists
// of indices, and fill_object_id, order_ids and replace_ids, which name objects outside the targets, fill_object,
// order and replace. The ids stay beside the indices: a target's id is checked where the targets are read
// (check_expected_ids()), and fill_object_id, order_ids and replace_ids given with their indices are checked here, as
// no reading of the targets reads them. An id the model has nothing with, a part's or an instance's id of another
// object than the call names, and an id that is no whole number are refused (ToolFailure, invalid_argument).
nlohmann::json with_indices(const Model& model, nlohmann::json args);
// `schema` (object_schema()) with target_props(kinds, one) among its properties.
nlohmann::json with_targets(nlohmann::json schema, unsigned kinds, bool one);

// Whether `args` name a target in any of the forms target_props() gives.
bool names_targets(const nlohmann::json& args);
// Whether the target arguments `args` give are all empty lists (objects: [] and the like), which object_select reads
// as naming nothing, where resolve_targets() refuses them.
bool names_empty_lists(const nlohmann::json& args);

// The targets of `kinds` that `args` name, by index or by id (with_indices()), each once, in the order named. Every
// index is checked to be in range and every id given beside one to match (check_expected_ids()) before anything is
// returned, and a miss throws ToolFailure naming it, as do both forms of one target at once (object and objects), an
// instance or a part without its object, and arguments that name nothing (empty lists). An index out of range that is
// an id the model has is refused with what that id names.
// With none named: what `selection` holds, from_selection set. Every instance of an object selected is the object,
// for a command that takes objects; an instance or a part selected stands for its object only with
// kObjectOfSelection, and a part for its instance only with kInstanceOfSelectedPart. The selection is refused with
// Ops::nothing_selected() when it is empty or null, and Ops::wrong_selection() when it holds a kind the command does
// not take. With `one`, more than one target is refused with Ops::one_object_only().
Ops::Outcome<Targets> resolve_targets(const Model& model, const Selection* selection, const nlohmann::json& args, unsigned kinds, bool one);

// What a reply says a command acted on: {"objects": [{index, object_id, name}], "instances": [{object, instance,
// object_id, instance_id, name}], "parts": [{object, part, part_id, name}]}, each key there when `targets` holds that
// kind. The indices, ids and names are as `model` has them now; an instance index the object no longer has (a deleted
// instance's) has no instance_id.
nlohmann::json targets_reply(const Model& model, const Targets& targets);

} // namespace Mcp
} // namespace GUI
} // namespace Slic3r
