// The gizmo commands: a named target's gizmo opened on it (object_paint, object_measure, object_simplify, part_boolean,
// object_cut). A call that names only its targets opens the gizmo and the client works it with the GUI tools; a command
// whose gizmo has an entry point that asks nothing also acts from its acting arguments (gizmo_acting_args()), with no
// gizmo: object_cut's cut by a plane.

#include "McpTargets.hpp"
#include "McpTools.hpp"
#include "UiInspector.hpp"

#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/CommandTypes.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosManager.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Operations/GizmoOps.hpp"
#include "slic3r/GUI/Operations/ObjectOps.hpp"
#include "slic3r/GUI/Operations/Outcome.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Selection.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/libslic3r.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;

namespace {

using EType = GLGizmosManager::EType;

// How a call's targets become what its gizmo opens on: `kinds` and `one` read them (resolve_targets()), and `to_gizmo`
// turns them into the selection the gizmo works on.
struct GizmoTargets
{
    unsigned kinds = kObjects;
    bool     one   = true;
    Targets (*to_gizmo)(const Model& model, Targets named) = nullptr;
};

// One instance of one object: the instance named, else the object's first.
Targets one_instance(const Model&, Targets named)
{
    if (!named.objects.empty()) {
        named.instances = {{named.objects.front(), 0}};
        named.objects.clear();
    }
    return named;
}

// One part: the part named, else the only part of an object of one part. An object of several parts stays as it is,
// which the operation refuses as Simplify's panel would.
Targets one_part(const Model& model, Targets named)
{
    if (!named.objects.empty() && model.objects[std::size_t(named.objects.front())]->volumes.size() == 1) {
        named.parts = {{named.objects.front(), 0}};
        named.objects.clear();
    }
    return named;
}

// Opens the gizmo of `type` on what `args` name, else on the selection as it is: a call that names no target does not
// read the selection as targets, so the gizmo's own condition decides what it opens on (Measure on a part). The reply
// is the gizmo, that it is open, and what is selected.
json open_on(const json& args, EType type, const GizmoTargets& reading)
{
    Plater&           plater = ui_plater();
    Ops::OpenGizmoArgs op;
    op.type = type;
    if (names_targets(args)) {
        const Ops::Outcome<Targets> named = resolve_targets(plater.model(), nullptr, args, reading.kinds, reading.one);
        if (!named)
            throw refused(named.refusal());
        op.targets = reading.to_gizmo != nullptr ? reading.to_gizmo(plater.model(), named.value()) : named.value();
    } else
        op.targets.from_selection = true;
    const Ops::Outcome<Ops::OpenedGizmo> opened = Ops::open_gizmo(op);
    if (!opened)
        throw refused(opened.refusal());
    json reply      = targets_reply(plater.model(), opened.value().on);
    reply["gizmo"]  = gizmo_name(int(type));
    reply["active"] = true;
    return reply;
}

// What every gizmo command's description says of working the open gizmo.
const char* const kWorkTheGizmo =
    " Read the gizmo's panel with widget_list {include_imgui: true}, set its fields with widget_set_value, press its "
    "buttons with widget_click, and click or drag on the model with mouse_gesture; object_select {none: true} closes it. "
    "The gizmo already open on the same target stays open. Reply: gizmo (its name, as app_state.gizmo gives it), active, "
    "and what is selected (objects, instances, parts).";

// Whether a call of `command` gives one of its acting arguments, and so does the job with no gizmo.
bool acts(const json& args, const std::string& command)
{
    for (const std::string& name : gizmo_acting_args(command))
        if (args.contains(name))
            return true;
    return false;
}

Vec3d to_vec3(const json& v, const char* what)
{
    if (!v.is_array() || v.size() != 3)
        throw ToolFailure(std::string(what) + " must be [x, y, z]");
    return {v[0].get<double>(), v[1].get<double>(), v[2].get<double>()};
}

Ops::CutPlacement placement(const std::string& name)
{
    return name == "as_cut" ? Ops::CutPlacement::AsCut : name == "flip" ? Ops::CutPlacement::Flip : Ops::CutPlacement::PlaceOnCut;
}

// What an object of the model is in a cut's reply: its index, id and name, and the open edges its parts have, which
// object_fix repairs.
json piece(const Model& model, int index)
{
    const ModelObject& object     = *model.objects[std::size_t(index)];
    std::size_t        open_edges = 0;
    for (const ModelVolume* volume : object.volumes)
        open_edges += its_num_open_edges(volume->mesh().its);
    return {{"index", index}, {"object_id", object.id().id}, {"name", object.name}, {"open_edges", open_edges}};
}

// object_cut from its acting arguments. The arguments the Cut gizmo's panel could not set together are refused before
// anything is read: z with plane, a placement for a piece not kept, and placements or one piece with Cut to parts.
json cut_from_args(const json& args)
{
    if (args.contains("z") && args.contains("plane"))
        throw ToolFailure("Give z or plane, not both.");
    if (!args.contains("z") && !args.contains("plane"))
        throw refused(Ops::Refusal{Ops::RefusalKind::Precondition, "needs_plane",
                                   L("A cut needs its plane: give z or plane with keep, upper, lower or to_parts.")});
    Ops::CutObjectArgs op;
    if (args.contains("plane")) {
        op.point  = to_vec3(args["plane"]["point"], "plane.point");
        op.normal = to_vec3(args["plane"]["normal"], "plane.normal");
        if (op.normal.norm() < EPSILON)
            throw ToolFailure("plane.normal is zero: give the direction the upper piece lies in.");
    }
    const std::string keep = args.value("keep", std::string("both"));
    op.keep_upper          = keep != "lower";
    op.keep_lower          = keep != "upper";
    if (args.contains("upper") && !op.keep_upper)
        throw ToolFailure("upper places the upper piece, but the upper piece is not kept (keep \"lower\").");
    if (args.contains("lower") && !op.keep_lower)
        throw ToolFailure("lower places the lower piece, but the lower piece is not kept (keep \"upper\").");
    op.to_parts = args.value("to_parts", false);
    if (op.to_parts && keep != "both")
        throw ToolFailure("to_parts keeps both pieces, as two parts of one object; leave keep out.");
    if (op.to_parts && (args.contains("upper") || args.contains("lower")))
        throw ToolFailure("to_parts places no piece: both stay as cut; leave upper and lower out.");
    if (args.contains("upper"))
        op.upper = placement(args["upper"].get<std::string>());
    if (args.contains("lower"))
        op.lower = placement(args["lower"].get<std::string>());

    Plater&                     plater = ui_plater();
    const Ops::Outcome<Targets> named  = resolve_targets(plater.model(), &plater.get_view3D_canvas3D()->get_selection(), args,
                                                         kObjects | kInstances, true);
    if (!named)
        throw refused(named.refusal());
    std::tie(op.object, op.instance) = one_instance(plater.model(), named.value()).instances.front();
    if (args.contains("z")) {
        // A level plane that high above the bed, through the instance.
        op.point = plater.model().objects[std::size_t(op.object)]->instances[std::size_t(op.instance)]->get_offset();
        op.point.z() = args["z"].get<double>();
        op.normal    = Vec3d::UnitZ();
    }

    const Ops::Outcome<Targets> cut = Ops::cut_object(op);
    if (!cut)
        throw refused(cut.refusal());
    const Model& model = plater.model();
    json         reply;
    if (op.to_parts) {
        reply["object"] = piece(model, cut.value().objects.front());
        reply["parts"]  = targets_reply(model, cut.value()).value("parts", json::array());
    } else {
        reply["pieces"] = json::array();
        for (int index : cut.value().objects)
            reply["pieces"].push_back(piece(model, index));
    }
    return reply;
}

NativeCommand gizmo_command(std::string name, std::string title, std::string description, json schema, EType type,
                            GizmoTargets reading, json examples)
{
    NativeCommand command = with_expected_ids(mutate_tool(std::move(name), std::move(title), description + kWorkTheGizmo,
                                                          std::move(schema), [type, reading](const json& args, ToolContext& ctx) {
                                                              json out = ctx.run_ui([&] { return open_on(args, type, reading); });
                                                              settle(ctx);
                                                              return ToolResult::ok(out);
                                                          }));
    command.examples = std::move(examples);
    return reaching({"open_gizmo"}, std::move(command));
}

// The painting gizmos, by object_paint's type.
const std::vector<std::pair<std::string, EType>> kPaintTypes{
    {"supports", EType::FdmSupports}, {"seam", EType::Seam}, {"color", EType::MmSegmentation}, {"fuzzy_skin", EType::FuzzySkin}};

} // namespace

const std::vector<std::string>& gizmo_acting_args(const std::string& command)
{
    // The gizmo commands' acting arguments: a call with any of them does the job and opens no gizmo.
    static const std::map<std::string, std::vector<std::string>> acting{
        {"object_paint", {}}, {"object_measure", {}}, {"object_simplify", {}}, {"part_boolean", {}},
        {"object_cut", {"z", "plane", "keep", "upper", "lower", "to_parts"}}};
    static const std::vector<std::string> none;
    const auto                            found = acting.find(command);
    return found != acting.end() ? found->second : none;
}

void register_gizmo_tools(CommandCatalog& catalog)
{
    std::vector<std::string> paint_types;
    for (const auto& [name, type] : kPaintTypes)
        paint_types.push_back(name);
    NativeCommand paint = with_expected_ids(mutate_tool(
        "object_paint", L("Paint on an object"),
        std::string("Open a painting gizmo on an object's instance, the first unless instance is given: type supports opens "
                    "Support Painting, seam Seam Painting, color Color Painting (two filaments or more) and fuzzy_skin Paint-on "
                    "fuzzy skin. Painting is left to the gizmo: pick a tool and a brush in its panel and paint with "
                    "mouse_gesture strokes on the model. With no target, it opens on the selection as it is.") +
            kWorkTheGizmo,
        with_targets(object_schema({{"type", enum_prop(paint_types, "The painting gizmo to open.")}}, {"type"}), kObjects | kInstances, true),
        [](const json& args, ToolContext& ctx) {
            EType type = EType::FdmSupports;
            for (const auto& [name, t] : kPaintTypes)
                if (name == args.value("type", std::string()))
                    type = t;
            json out = ctx.run_ui([&] { return open_on(args, type, {kObjects | kInstances, true, one_instance}); });
            settle(ctx);
            return ToolResult::ok(out);
        }));
    paint.examples = json::parse(R"json([
        {"title": "Paint support enforcers on object 0", "calls": [
            {"tool": "object_paint", "args": {"object": 0, "type": "supports"}}]},
        {"title": "Paint colors on the second instance of object 1", "calls": [
            {"tool": "object_paint", "args": {"object": 1, "instance": 1, "type": "color"}}]}])json");
    catalog.add(reaching({"open_gizmo"}, std::move(paint)));

    catalog.add(gizmo_command(
        "object_measure", L("Measure"),
        "Open the Measure gizmo on objects, else on the selection as it is (a part selected is measured as a part). "
        "Measuring is left to the gizmo: click two features of the model with mouse_gesture (Shift-click for a point) and "
        "read the results in its panel.",
        with_targets(object_schema({}), kObjects, false), EType::Measure, {kObjects, false, nullptr},
        json::parse(R"json([
            {"title": "Measure between objects 0 and 1", "calls": [{"tool": "object_measure", "args": {"objects": [0, 1]}}]},
            {"title": "Measure the selected part", "calls": [
                {"tool": "object_select", "args": {"parts": [[0, 1]]}}, {"tool": "object_measure", "args": {}}]}])json")));

    catalog.add(gizmo_command(
        "object_simplify", L("Simplify Model"),
        "Open the Simplify gizmo on a part: the part named, else the only part of the object named (an object of several "
        "parts needs part), else the selected part. The detail level or ratio and Apply are left to the gizmo.",
        with_targets(object_schema({}), kObjects | kParts, true), EType::Simplify, {kObjects | kParts, true, one_part},
        json::parse(R"json([
            {"title": "Simplify object 0, an object of one part", "calls": [{"tool": "object_simplify", "args": {"object": 0}}]},
            {"title": "Simplify part 1 of object 2", "calls": [{"tool": "object_simplify", "args": {"object": 2, "part": 1}}]}])json")));

    catalog.add(gizmo_command(
        "part_boolean", L("Mesh Boolean"),
        "Open the Mesh Boolean gizmo on an object of two parts or more, its first instance. The operation (Union, Difference "
        "or Intersection), the two parts and Delete input are left to the gizmo: pick each part with a mouse_gesture click "
        "on a point of it the camera can see, then press the operation's button.",
        with_targets(object_schema({}), kObjects, true), EType::MeshBoolean, {kObjects, true, one_instance},
        json::parse(R"json([
            {"title": "Open Mesh Boolean on object 0", "calls": [{"tool": "part_boolean", "args": {"object": 0}}]},
            {"title": "Open Mesh Boolean on the selected object", "calls": [{"tool": "part_boolean", "args": {}}]}])json")));

    const std::vector<std::string> placements{"as_cut", "place_on_cut", "flip"};
    json                           point  = array_prop({{"type", "number"}}, "[x, y, z] mm: a point of the plane, in the 3D view's coordinates.");
    json                           normal = array_prop({{"type", "number"}}, "[x, y, z]: the direction the upper piece lies in; any length but zero.");
    point["minItems"] = normal["minItems"] = 3;
    json plane        = object_schema({{"point", std::move(point)}, {"normal", std::move(normal)}}, {"point", "normal"});
    plane["description"] = "Cut along this plane. Not with z.";
    NativeCommand cut    = with_expected_ids(mutate_tool(
        "object_cut", L("Cut"),
        std::string(
            "Cut an object's instance, the first unless instance is given, along a plane, as the Cut gizmo's Perform cut does. "
            "Acts from arguments: z, plane, keep, upper, lower, to_parts. With none of them it opens the Cut gizmo. z cuts "
            "level, that many mm above the bed; plane {point, normal} cuts along any plane, the upper piece being the one "
            "the normal points into. keep: both (default), upper or lower. upper and lower place a kept piece: as_cut, "
            "place_on_cut (laid on its cut face) or flip; by default the upper piece is laid on its cut face and the lower "
            "one stays as cut. to_parts keeps both pieces as two parts of one object, with no placement. A cut is one undo "
            "step and replaces the object: its pieces go to the end of the object list, upper first, and the reply gives "
            "them as pieces [{index, object_id, name, open_edges}], or with to_parts the object and its parts. A piece with "
            "open edges is repaired by object_fix. Connectors, the dovetail and a cut along a contour are left to the gizmo: "
            "open it with object_cut naming only the object.") +
            kWorkTheGizmo,
        with_targets(object_schema({{"z", prop("number", "mm above the bed: cut level at this height. Not with plane.")},
                                    {"plane", std::move(plane)},
                                    {"keep", enum_prop({"both", "upper", "lower"}, "The pieces kept. Default both.")},
                                    {"upper", enum_prop(placements, "Where the upper piece goes. Default place_on_cut.")},
                                    {"lower", enum_prop(placements, "Where the lower piece goes. Default as_cut.")},
                                    {"to_parts", prop("boolean", "Keep both pieces as two parts of one object, as the gizmo's "
                                                                 "Cut to parts does. Default false.")}}),
                     kObjects | kInstances, true),
        [](const json& args, ToolContext& ctx) {
            json out = ctx.run_ui([&] {
                return acts(args, "object_cut") ? cut_from_args(args) : open_on(args, EType::Cut, {kObjects | kInstances, true, one_instance});
            });
            settle(ctx);
            return ToolResult::ok(out);
        }));
    cut.examples = json::parse(R"json([
        {"title": "Cut object 0 in two, 10 mm above the bed", "calls": [{"tool": "object_cut", "args": {"object": 0, "z": 10}}]},
        {"title": "Keep the part of object 1 above a tilted plane", "calls": [
            {"tool": "object_cut", "args": {"object": 1, "plane": {"point": [100, 100, 10], "normal": [1, 0, 1]}, "keep": "upper"}}]}])json");
    catalog.add(reaching({"open_gizmo", "cut_object"}, std::move(cut)));
}

}}} // namespace Slic3r::GUI::Mcp
