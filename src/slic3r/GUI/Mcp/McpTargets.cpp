#include "McpTargets.hpp"

#include "McpArguments.hpp"
#include "McpTools.hpp"

#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/CommandTypes.hpp"
#include "slic3r/GUI/Operations/ObjectOps.hpp"
#include "slic3r/GUI/Operations/Outcome.hpp"
#include "slic3r/GUI/Selection.hpp"

#include "libslic3r/Model.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;

namespace {

// Every argument that names a target, in one form or the other.
constexpr const char* kTargetArgs[] = {"object", "objects", "object_id", "object_ids", "instance", "instances", "instance_object_ids",
                                       "part", "parts", "part_id", "part_ids"};

template<class T> void add_once(std::vector<T>& list, const T& item)
{
    if (std::find(list.begin(), list.end(), item) == list.end())
        list.push_back(item);
}

// The indices there are of `count` things called `noun` ("object"), as a refusal gives them, so that an agent learns
// the range from the refusal: "objects 0..2", "object 0 only" or "no objects".
std::string index_range(const char* noun, std::size_t count)
{
    if (count == 0)
        return std::string("no ") + noun + "s";
    if (count == 1)
        return std::string(noun) + " 0 only";
    return std::string(noun) + "s 0.." + std::to_string(count - 1);
}

// Index `v` among `count` things, or -1 where it is none of them. It is compared as the call gives it, so that neither
// an integer wider than an int (4294967296), nor an unsigned one past int64, nor a whole float (is_whole_number(), as
// the schema's integer type takes one) is narrowed into another index first.
std::int64_t index_among(const json& v, std::size_t count)
{
    if (v.is_number_unsigned())
        return v.get<std::uint64_t>() < count ? std::int64_t(v.get<std::uint64_t>()) : -1;
    if (v.is_number_integer()) {
        const std::int64_t i = v.get<std::int64_t>();
        return i >= 0 && std::uint64_t(i) < count ? i : -1;
    }
    if (!is_whole_number(v))
        return -1;
    const double d = v.get<double>();
    return d >= 0 && d < double(count) ? std::int64_t(d) : -1;
}

// The checks of a target's indices against the model, with the texts object_select gives, each naming the index as
// the call gave it.
struct IndexCheck
{
    const Model& model;

    // The index of the object `v` names.
    int object(const json& v) const
    {
        const std::int64_t index = index_among(v, model.objects.size());
        if (index < 0)
            throw ToolFailure("object index " + v.dump() + " is out of range (the project has " +
                                  index_range("object", model.objects.size()) + ")",
                              {{"code", "invalid_argument"}});
        return int(index);
    }

    // The index of the instance, or with !instance the part, of object `owner` (checked) that `v` names.
    int member(int owner, const json& v, bool instance) const
    {
        const ModelObject& o     = *model.objects[std::size_t(owner)];
        const std::size_t  count = instance ? o.instances.size() : o.volumes.size();
        const std::int64_t index = index_among(v, count);
        if (index < 0)
            throw ToolFailure("object " + std::to_string(owner) + " has no " + (instance ? "instance " : "part ") + v.dump() +
                                  " (it has " + index_range(instance ? "instance" : "part", count) + ")",
                              {{"code", "invalid_argument"}});
        return int(index);
    }

    // [object, index] of a pair argument.
    std::pair<int, int> pair(const json& entry, bool instance) const
    {
        if (!entry.is_array() || entry.size() != 2 || !is_whole_number(entry[0]) || !is_whole_number(entry[1]))
            throw ToolFailure(std::string("an entry of ") + (instance ? "instances" : "parts") + " is [object, " +
                              (instance ? "instance" : "part") + "]", {{"code", "invalid_argument"}});
        const int owner = object(entry[0]);
        return {owner, member(owner, entry[1], instance)};
    }
};

// The ids `ids_key` expects for the entries of `list_key`, one each, checked against `expected` (the object or part
// each entry names) as check_expected_ids() checks one.
template<class Expected> void check_ids(const json& args, const char* list_key, const char* ids_key, const char* id_key, std::size_t entries,
                                        const Expected& expected)
{
    if (!args.contains(ids_key))
        return;
    const json& ids = args.at(ids_key);
    if (!ids.is_array() || ids.size() != entries)
        throw ToolFailure(std::string(ids_key) + " takes one id per entry of " + list_key, {{"code", "invalid_argument"}});
    for (std::size_t k = 0; k < entries; ++k)
        expected(k, json{{id_key, ids[k]}});
}

Targets named_targets(const Model& model, const json& args, unsigned kinds)
{
    using Forms = std::pair<const char*, const char*>;
    for (const Forms& forms : {Forms{"object", "objects"}, Forms{"instance", "instances"}, Forms{"part", "parts"}})
        if (args.contains(forms.first) && args.contains(forms.second))
            throw ToolFailure(std::string("Give ") + forms.first + " or " + forms.second + ", not both", {{"code", "invalid_argument"}});
    const IndexCheck check{model};
    Targets          out;

    // One target: the object, or an instance or a part of it.
    if (args.contains("object")) {
        const int          index  = check.object(args.at("object"));
        const ModelObject& object = *model.objects[std::size_t(index)];
        check_expected_ids(object, nullptr, args);
        if ((kinds & kParts) && args.contains("part")) {
            const int part = check.member(index, args.at("part"), false);
            check_expected_ids(object, object.volumes[std::size_t(part)], args);
            out.parts.emplace_back(index, part);
        } else if ((kinds & kInstances) && args.contains("instance")) {
            const int instance = check.member(index, args.at("instance"), true);
            out.instances.emplace_back(index, instance);
        } else if (kinds & kObjects) {
            if (args.contains("part_id"))
                throw ToolFailure("part_id goes with part", {{"code", "invalid_argument"}});
            out.objects.push_back(index);
        } else {
            throw ToolFailure(std::string("object names the object of a ") + (kinds & kParts ? "part: give part too" : "instance: give instance too"), {{"code", "invalid_argument"}});
        }
    } else {
        for (const char* key : {"instance", "part", "object_id", "part_id"})
            if (args.contains(key))
                throw ToolFailure(std::string(key) + " goes with object", {{"code", "invalid_argument"}});
    }

    // Several targets, each checked with its expected id before any is taken.
    const json objects   = kinds & kObjects ? args.value("objects", json::array()) : json::array();
    const json instances = kinds & kInstances ? args.value("instances", json::array()) : json::array();
    const json parts     = kinds & kParts ? args.value("parts", json::array()) : json::array();
    std::vector<int>                 object_list;
    std::vector<std::pair<int, int>> instance_list, part_list;
    for (const json& entry : objects) {
        if (!is_whole_number(entry))
            throw ToolFailure("an entry of objects is an object index", {{"code", "invalid_argument"}});
        object_list.push_back(check.object(entry));
    }
    for (const json& entry : instances)
        instance_list.push_back(check.pair(entry, true));
    for (const json& entry : parts)
        part_list.push_back(check.pair(entry, false));
    check_ids(args, "objects", "object_ids", "object_id", object_list.size(), [&](std::size_t k, const json& id) {
        check_expected_ids(*model.objects[std::size_t(object_list[k])], nullptr, id);
    });
    check_ids(args, "instances", "instance_object_ids", "object_id", instance_list.size(), [&](std::size_t k, const json& id) {
        check_expected_ids(*model.objects[std::size_t(instance_list[k].first)], nullptr, id);
    });
    check_ids(args, "parts", "part_ids", "part_id", part_list.size(), [&](std::size_t k, const json& id) {
        const ModelObject& object = *model.objects[std::size_t(part_list[k].first)];
        check_expected_ids(object, object.volumes[std::size_t(part_list[k].second)], id);
    });
    for (int index : object_list)
        add_once(out.objects, index);
    for (const auto& instance : instance_list)
        add_once(out.instances, instance);
    for (const auto& part : part_list)
        add_once(out.parts, part);
    if (out.objects.empty() && out.instances.empty() && out.parts.empty())
        throw ToolFailure("The call names no target; give one, or none to act on the selection", {{"code", "invalid_argument"}});
    return out;
}

// Whether `selection` holds parts rather than whole instances.
bool holds_parts(const Selection& selection)
{
    return selection.get_mode() == Selection::Volume && !selection.is_single_full_instance() && !selection.is_multiple_full_instance() &&
           !selection.is_single_full_object() && !selection.is_multiple_full_object();
}

Ops::Outcome<Targets> selected_targets(const Model& model, const Selection* selection, unsigned kinds)
{
    if (selection == nullptr || selection->is_empty())
        return Ops::nothing_selected();
    if (selection->is_wipe_tower())
        return Ops::wrong_selection();
    Targets out;
    out.from_selection = true;
    const auto in_model  = [&model](int object) { return object >= 0 && std::size_t(object) < model.objects.size(); };
    const bool as_object = (kinds & kObjects) && (kinds & kObjectOfSelection);
    if (holds_parts(*selection)) {
        for (unsigned int index : selection->get_volume_idxs()) {
            const GLVolume* volume = selection->get_volume(index);
            if (volume == nullptr || volume->is_wipe_tower || volume->volume_idx() < 0 || !in_model(volume->object_idx()))
                continue;
            if (kinds & kParts)
                add_once(out.parts, std::make_pair(volume->object_idx(), volume->volume_idx()));
            else if ((kinds & kInstances) && (kinds & kInstanceOfSelectedPart))
                add_once(out.instances, std::make_pair(volume->object_idx(), volume->instance_idx()));
            else if (as_object)
                add_once(out.objects, volume->object_idx());
            else
                return Ops::wrong_selection();
        }
    } else {
        for (const auto& [object, instances] : selection->get_content()) {
            if (!in_model(object))
                continue;
            const bool whole = instances.size() == model.objects[std::size_t(object)]->instances.size();
            if ((kinds & kObjects) && (whole || (as_object && !(kinds & kInstances))))
                add_once(out.objects, object);
            else if (kinds & kInstances)
                for (int instance : instances)
                    add_once(out.instances, std::make_pair(object, instance));
            else
                return Ops::wrong_selection();
        }
    }
    if (out.objects.empty() && out.instances.empty() && out.parts.empty())
        return Ops::nothing_selected();
    return out;
}

} // namespace

std::vector<Property> target_props(unsigned kinds, bool one)
{
    std::vector<Property> out;
    if (one) {
        out.emplace_back("object", json{{"type", "integer"}, {"minimum", 0}, {"description", "Object index (object_list)."}});
        out.emplace_back("object_id", expected_id_prop("object"));
        if (kinds & kInstances)
            out.emplace_back("instance", json{{"type", "integer"}, {"minimum", 0}, {"description", "An instance of the object (object_list)."}});
        if (kinds & kParts) {
            out.emplace_back("part", json{{"type", "integer"}, {"minimum", 0}, {"description", "A part of the object (object_list)."}});
            out.emplace_back("part_id", expected_id_prop("part"));
        }
        return out;
    }
    if (kinds & kObjects) {
        out.emplace_back("objects", array_prop({{"type", "integer"}}, "Object indices."));
        out.emplace_back("object_ids", array_prop({{"type", "integer"}}, "Optional guard: the objects' ids in object_list, one each."));
    }
    if (kinds & kInstances) {
        out.emplace_back("instances", array_prop({{"type", "array"}}, "[[object, instance], ...]"));
        out.emplace_back("instance_object_ids", array_prop({{"type", "integer"}}, "Optional guard: the ids in object_list of the "
                                                                                  "instances' objects, one each."));
    }
    if (kinds & kParts) {
        out.emplace_back("parts", array_prop({{"type", "array"}}, "[[object, part], ...]"));
        out.emplace_back("part_ids", array_prop({{"type", "integer"}}, "Optional guard: the parts' ids in object_list, one each."));
    }
    return out;
}

json with_targets(json schema, unsigned kinds, bool one)
{
    for (Property& property : target_props(kinds, one))
        schema["properties"][property.first] = std::move(property.second);
    return schema;
}

bool names_targets(const json& args)
{
    return args.is_object() && std::any_of(std::begin(kTargetArgs), std::end(kTargetArgs), [&args](const char* key) { return args.contains(key); });
}

bool names_empty_lists(const json& args)
{
    return names_targets(args) && std::all_of(std::begin(kTargetArgs), std::end(kTargetArgs), [&args](const char* key) {
               return !args.contains(key) || (args.at(key).is_array() && args.at(key).empty());
           });
}

Ops::Outcome<Targets> resolve_targets(const Model& model, const Selection* selection, const json& args, unsigned kinds, bool one)
{
    Targets targets;
    if (names_targets(args)) {
        targets = named_targets(model, args, kinds);
    } else {
        Ops::Outcome<Targets> selected = selected_targets(model, selection, kinds);
        if (!selected)
            return selected;
        targets = selected.value();
    }
    if (one && targets.objects.size() + targets.instances.size() + targets.parts.size() > 1)
        return Ops::one_object_only();
    return targets;
}

json targets_reply(const Model& model, const Targets& targets)
{
    const auto object_at = [&model](int index) -> const ModelObject* {
        return index >= 0 && std::size_t(index) < model.objects.size() ? model.objects[std::size_t(index)] : nullptr;
    };
    json out = json::object();
    json objects = json::array(), instances = json::array(), parts = json::array();
    for (int index : targets.objects)
        if (const ModelObject* object = object_at(index))
            objects.push_back({{"index", index}, {"object_id", object->id().id}, {"name", object->name}});
    for (const auto& [index, instance] : targets.instances)
        if (const ModelObject* object = object_at(index))
            instances.push_back({{"object", index}, {"instance", instance}, {"object_id", object->id().id}, {"name", object->name}});
    for (const auto& [index, part] : targets.parts)
        if (const ModelObject* object = object_at(index); object != nullptr && part >= 0 && std::size_t(part) < object->volumes.size())
            parts.push_back({{"object", index}, {"part", part}, {"part_id", object->volumes[std::size_t(part)]->id().id},
                             {"name", object->volumes[std::size_t(part)]->name}});
    if (!targets.objects.empty())
        out["objects"] = std::move(objects);
    if (!targets.instances.empty())
        out["instances"] = std::move(instances);
    if (!targets.parts.empty())
        out["parts"] = std::move(parts);
    return out;
}

}}} // namespace Slic3r::GUI::Mcp
