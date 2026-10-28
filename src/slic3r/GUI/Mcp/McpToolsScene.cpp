// list_objects, select, transform, slice and cancel: the model on the plates, and the slice
// operation, which reports its own outcome per plate (see McpOperations).

#include "JobRegistry.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Point.hpp"
#include "McpOperations.hpp"
#include "McpService.hpp"
#include "McpTools.hpp"
#include "McpTypes.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Selection.hpp"

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Utils.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;

namespace {

json vec3(const Vec3d& v) { return {v.x(), v.y(), v.z()}; }

Vec3d to_vec3(const json& v, const char* what)
{
    if (!v.is_array() || v.size() != 3)
        throw ToolFailure(std::string(what) + " must be [x, y, z]");
    return {v[0].get<double>(), v[1].get<double>(), v[2].get<double>()};
}

const char* volume_type_name(ModelVolumeType type)
{
    switch (type) {
    case ModelVolumeType::MODEL_PART: return "part";
    case ModelVolumeType::NEGATIVE_VOLUME: return "negative";
    case ModelVolumeType::PARAMETER_MODIFIER: return "modifier";
    case ModelVolumeType::SUPPORT_BLOCKER: return "support_blocker";
    case ModelVolumeType::SUPPORT_ENFORCER: return "support_enforcer";
    default: return "other";
    }
}

// The Prepare view and its selection: object operations act on the 3D view.
void show_prepare()
{
    Plater& plater = ui_plater();
    if (!plater.is_view3D_shown()) {
        plater.select_view_3D("3D");
        if (MainFrame* mf = wxGetApp().mainframe)
            mf->select_tab(TAB_ID_PREPARE);
    }
}

json list_objects(const json& args)
{
    Plater&          plater = ui_plater();
    PartPlateList&   plates = plater.get_partplate_list();
    const ModelObjectPtrs& objects = plater.model().objects;
    const int        only_plate = arg_int(args, "plate", -1);
    json             items = json::array();
    for (size_t i = 0; i < objects.size(); ++i) {
        const ModelObject* o = objects[i];
        json instances = json::array();
        bool on_plate  = only_plate < 0;
        for (size_t j = 0; j < o->instances.size(); ++j) {
            const ModelInstance* inst  = o->instances[j];
            const int            plate = plates.find_instance(int(i), int(j));
            on_plate                   = on_plate || plate == only_plate;
            const Vec3d          rot   = inst->get_rotation();
            instances.push_back({{"index", j},
                                 {"plate", plate},
                                 {"offset", vec3(inst->get_offset())},
                                 {"rotation_deg", vec3(Vec3d(Geometry::rad2deg(rot.x()), Geometry::rad2deg(rot.y()), Geometry::rad2deg(rot.z())))},
                                 {"scale", vec3(inst->get_scaling_factor())},
                                 {"printable", inst->printable}});
        }
        if (!on_plate)
            continue;
        json volumes = json::array();
        for (size_t v = 0; v < o->volumes.size(); ++v)
            volumes.push_back({{"index", v}, {"name", o->volumes[v]->name}, {"type", volume_type_name(o->volumes[v]->type())}});
        const BoundingBoxf3 bb = o->instance_bounding_box(0);
        json overrides = json::array();
        for (const std::string& key : o->config.keys())
            overrides.push_back(key);
        items.push_back({{"index", i},
                         {"name", o->name},
                         {"instances", std::move(instances)},
                         {"volumes", std::move(volumes)},
                         {"bbox", {{"min", vec3(bb.min)}, {"max", vec3(bb.max)}, {"size", vec3(bb.size())}}},
                         {"overrides", std::move(overrides)}});
    }
    json out    = page(items, args, 200);
    out["plates"] = plates.get_plate_count();
    return out;
}

json selection_summary()
{
    Plater&          plater = ui_plater();
    const Selection& sel    = plater.get_selection();
    json             out{{"empty", sel.is_empty()}};
    if (!sel.is_empty()) {
        out["object"]   = sel.get_object_idx();
        out["instance"] = sel.get_instance_idx();
        std::set<int> objects;
        for (unsigned int idx : sel.get_volume_idxs())
            if (const GLVolume* v = sel.get_volume(idx))
                objects.insert(v->object_idx());
        out["objects"] = std::vector<int>(objects.begin(), objects.end());
    }
    return out;
}

// Per-plate statistics of a valid slice result.
json plate_stats(PartPlate& plate)
{
    json out = json::object();
    GCodeProcessorResult* result = plate.get_slice_result();
    if (result == nullptr)
        return out;
    const auto& stats = result->print_statistics;
    const double seconds = stats.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time;
    double meters = 0., grams = 0.;
    for (const auto& [role, used] : stats.used_filaments_per_role) {
        meters += used.first;
        grams += used.second;
    }
    out["time_s"]     = std::lround(seconds);
    out["time"]       = get_time_dhms(float(seconds));
    out["filament_m"] = std::round(meters * 100.) / 100.;
    out["filament_g"] = std::round(grams * 100.) / 100.;
    return out;
}

struct SliceTicket
{
    SliceTicketState state;
    std::vector<int> plates;
};

} // namespace

void register_scene_tools(McpService& service)
{
    ToolRegistry& reg = service.registry();

    reg.add(inspect_tool(
        "list_objects", "List objects",
        "Objects in the project with their instances (plate, position, rotation in degrees, scale), parts and modifiers, "
        "bounding box and the settings they override. Indices are what select, transform and the object/volume settings "
        "scopes take.",
        object_schema({{"plate", {{"type", "integer"}, {"minimum", 0}, {"description", "Only objects on this plate."}}},
                       {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 500}}},
                       {"offset", {{"type", "integer"}, {"minimum", 0}}}}),
        [](const json& args, ToolContext& ctx) { return ToolResult::ok(ctx.run_ui([&] { return list_objects(args); })); }));

    reg.add(mutate_tool(
        "select", "Select objects",
        "Select objects, instances or parts in the 3D view (switching to Prepare), as clicking them would. Give objects, "
        "instances ([[object, instance], ...]) or volumes ([[object, part], ...]); add keeps the current selection; all / "
        "none select everything or nothing.",
        object_schema({{"objects", array_prop({{"type", "integer"}}, "Object indices.")},
                       {"instances", array_prop({{"type", "array"}}, "[[object, instance], ...]")},
                       {"volumes", array_prop({{"type", "array"}}, "[[object, part], ...]")},
                       {"add", prop("boolean", "Add to the selection instead of replacing it.")},
                       {"all", prop("boolean", "Select all objects.")},
                       {"none", prop("boolean", "Clear the selection.")}}),
        [](const json& args, ToolContext& ctx) {
            json out = ctx.run_ui([&] {
                Plater& plater = ui_plater();
                show_prepare();
                if (arg_bool(args, "none", false)) {
                    plater.deselect_all();
                    return selection_summary();
                }
                if (arg_bool(args, "all", false)) {
                    plater.select_all();
                    return selection_summary();
                }
                Selection&    sel   = plater.canvas3D()->get_selection();
                const size_t  count = plater.model().objects.size();
                bool          first = !arg_bool(args, "add", false);
                auto check_obj = [&](int o) {
                    if (o < 0 || size_t(o) >= count)
                        throw ToolFailure("object index " + std::to_string(o) + " is out of range");
                };
                for (const json& o : args.value("objects", json::array())) {
                    check_obj(o.get<int>());
                    sel.add_object(unsigned(o.get<int>()), first);
                    first = false;
                }
                for (const json& p : args.value("instances", json::array())) {
                    check_obj(p.at(0).get<int>());
                    sel.add_instance(unsigned(p.at(0).get<int>()), unsigned(p.at(1).get<int>()), first);
                    first = false;
                }
                for (const json& p : args.value("volumes", json::array())) {
                    check_obj(p.at(0).get<int>());
                    sel.add_volume(unsigned(p.at(0).get<int>()), unsigned(p.at(1).get<int>()), 0, first);
                    first = false;
                }
                wxGetApp().obj_list()->update_selections();
                plater.canvas3D()->set_as_dirty();
                return selection_summary();
            });
            settle(ctx);
            return ToolResult::ok(out);
        }));

    reg.add(mutate_tool(
        "transform", "Move, rotate or scale an instance",
        "Change one instance's placement. translate is in mm, rotate in degrees about X/Y/Z, scale a factor (one number or "
        "[x, y, z]). mode \"relative\" (default) adds to the current placement, \"absolute\" sets it. The object is dropped "
        "back onto the bed afterwards unless drop_to_bed is false. One undo step.",
        object_schema({{"object", {{"type", "integer"}, {"minimum", 0}}},
                       {"instance", {{"type", "integer"}, {"minimum", 0}, {"description", "Default 0."}}},
                       {"translate", array_prop({{"type", "number"}}, "[x, y, z] mm")},
                       {"rotate", array_prop({{"type", "number"}}, "[x, y, z] degrees")},
                       {"scale", {{"type", {"number", "array"}}, {"description", "Factor or [x, y, z] factors."}}},
                       {"mode", enum_prop({"relative", "absolute"}, "Default relative.")},
                       {"drop_to_bed", prop("boolean", "Default true.")}},
                      {"object"}),
        [](const json& args, ToolContext& ctx) {
            json out = ctx.run_ui([&]() -> json {
                Plater&          plater  = ui_plater();
                ModelObjectPtrs& objects = plater.model().objects;
                const int        oi      = arg_int(args, "object", -1);
                if (oi < 0 || oi >= int(objects.size()))
                    throw ToolFailure("object index out of range");
                ModelObject* object = objects[size_t(oi)];
                const int    ii     = arg_int(args, "instance", 0);
                if (ii < 0 || ii >= int(object->instances.size()))
                    throw ToolFailure("instance index out of range");
                ModelInstance* inst     = object->instances[size_t(ii)];
                const bool     absolute = arg_string(args, "mode", "relative") == "absolute";
                plater.take_snapshot("Transform object");
                if (args.contains("translate")) {
                    const Vec3d t = to_vec3(args["translate"], "translate");
                    inst->set_offset(absolute ? t : Vec3d(inst->get_offset() + t));
                }
                if (args.contains("rotate")) {
                    const Vec3d r = to_vec3(args["rotate"], "rotate") * Geometry::deg2rad(1.0);
                    if (absolute)
                        inst->set_rotation(r);
                    else {
                        const Transform3d rot = Geometry::rotation_transform(r) * Geometry::rotation_transform(inst->get_rotation());
                        inst->set_rotation(Geometry::extract_euler_angles(rot));
                    }
                }
                if (args.contains("scale")) {
                    const json& s = args["scale"];
                    const Vec3d f = s.is_number() ? Vec3d(s.get<double>(), s.get<double>(), s.get<double>()) : to_vec3(s, "scale");
                    if (f.minCoeff() <= 0.)
                        throw ToolFailure("scale factors must be positive");
                    inst->set_scaling_factor(absolute ? f : Vec3d(inst->get_scaling_factor().cwiseProduct(f)));
                }
                if (arg_bool(args, "drop_to_bed", true))
                    object->ensure_on_bed();
                object->invalidate_bounding_box();
                plater.changed_object(oi);
                return {{"object", oi},
                        {"instance", ii},
                        {"offset", vec3(inst->get_offset())},
                        {"scale", vec3(inst->get_scaling_factor())}};
            });
            settle(ctx);
            return ToolResult::ok(out);
        }));

    ToolDef slice = mutate_tool(
        "slice", "Slice",
        "Slice the current plate, all plates, or one plate by index, the way the toolbar's Slice button does, and wait for "
        "the result (or reply with a job id when it takes longer than the reply budget). Each requested plate reports ok "
        "(with print time and filament), error, cancelled, empty or skipped; the whole operation is superseded when "
        "someone else started a slice or edited the project while it ran. Other changes are refused until it finishes; "
        "cancel stops it.",
        object_schema({{"plates", {{"type", {"string", "integer"}}, {"description", "\"current\" (default), \"all\", or a plate index."}}},
                       {"timeout_s", {{"type", "integer"}, {"minimum", 10}, {"maximum", 7200}, {"description", "Give up after this long (default 1800)."}}}}),
        [](const json& args, ToolContext& ctx) {
            const json plates_arg = args.value("plates", json("current"));
            auto       ticket     = std::make_shared<SliceTicket>();
            // A change made just before (set_settings, open_files) is validated by an update
            // scheduled a moment later; a slice requested first would be refused on the old state.
            poll_ui(
                ctx, []() -> json { return ui_plater().is_background_process_update_scheduled() ? json() : json(true); },
                std::chrono::seconds(10), "the pending update of the slicing state");
            ctx.run_ui([&] {
                // Held from before the slice starts, in the same UI step, so no call admitted
                // meanwhile can change the project under it.
                ctx.begin_operation();
                Plater& plater = ui_plater();
                if (plater.printer_technology() != ptFFF)
                    throw ToolFailure("Slicing through MCP supports FFF printers only");
                PartPlateList& list = plater.get_partplate_list();
                bool           all  = false;
                if (plates_arg.is_string() && plates_arg.get<std::string>() == "all") {
                    all = true;
                    for (int i = 0; i < list.get_plate_count(); ++i)
                        ticket->plates.push_back(i);
                } else if (plates_arg.is_number_integer()) {
                    const int index = plates_arg.get<int>();
                    if (index < 0 || index >= list.get_plate_count())
                        throw ToolFailure("plate index out of range");
                    if (index != list.get_curr_plate_index())
                        plater.select_plate(index);
                    ticket->plates.push_back(index);
                } else {
                    ticket->plates.push_back(list.get_curr_plate_index());
                }
                ticket->state.run_seq_before    = plater.slice_run_seq();
                ticket->state.revision_at_start = plater.project_revision();
                ticket->state.started           = std::chrono::steady_clock::now();
                plater.request_slice(all);
                ticket->state.request_seq_mine = plater.slice_request_seq();
                ticket->state.seen_running     = plater.is_slicing_any();
                return json();
            });
            const auto timeout = std::chrono::seconds(arg_int(args, "timeout_s", 1800));
            json result = poll_ui(
                ctx,
                [ticket]() -> json {
                    Plater&          plater = ui_plater();
                    SliceObservation obs;
                    obs.slicing_any      = plater.is_slicing_any();
                    obs.request_seq      = plater.slice_request_seq();
                    obs.current_revision = plater.project_revision();
                    for (uint64_t seq = ticket->state.run_seq_before + 1; seq <= plater.slice_run_seq(); ++seq)
                        if (auto run = plater.slice_run(seq))
                            obs.runs_since_start.push_back({run->seq, run->finished, run->cancelled, run->error, run->revision_at_finish,
                                                            run->request_seq_at_finish, run->request_seq_at_start, run->plate});
                    ticket->state.seen_running = ticket->state.seen_running || obs.slicing_any || !obs.runs_since_start.empty();
                    const TicketStatus status  = slice_status(ticket->state, obs, std::chrono::steady_clock::now());
                    if (status == TicketStatus::Pending)
                        return json();
                    json out{{"status", status == TicketStatus::Done ? "done" : "superseded"}};
                    PartPlateList&                list = plater.get_partplate_list();
                    std::vector<PlateObservation> observed;
                    for (int index : ticket->plates)
                        if (PartPlate* plate = list.get_plate(index))
                            observed.push_back({index, plate->is_slice_result_valid(), !plate->has_printable_instances(),
                                                plater.plate_validation_error(index)});
                    const auto results = classify_plates(ticket->plates, observed, own_runs(ticket->state, obs));
                    json       plates  = json::array();
                    for (size_t i = 0; i < ticket->plates.size(); ++i) {
                        json p{{"index", ticket->plates[i]}, {"outcome", to_string(results[i].outcome)}};
                        if (results[i].outcome == PlateOutcome::Ok)
                            if (PartPlate* plate = list.get_plate(ticket->plates[i]))
                                p.update(plate_stats(*plate));
                        if (!results[i].error.empty())
                            p["error"] = results[i].error;
                        plates.push_back(std::move(p));
                    }
                    out["plates"] = std::move(plates);
                    json notes = json::array();
                    for (const NotificationSummary& n : plater.get_notification_manager()->summaries())
                        if (n.level == "warning" || n.level == "error")
                            notes.push_back({{"level", n.level}, {"text", n.text}});
                    if (!notes.empty())
                        out["notifications"] = std::move(notes);
                    return out;
                },
                timeout, "slicing to finish");
            return ToolResult::ok(result);
        });
    slice.budget_ms = 45000;
    reg.add(slice);

    ToolDef cancel = mutate_tool(
        "cancel", "Cancel slicing",
        "Stop the running slice or G-code export, as the cancel button on the slicing progress does. The slice or export "
        "job then reports cancelled. With job, only when that job is the running slice or export. Allowed while a slice or "
        "export holds the project.",
        object_schema({{"job", prop("string", "The slice or export job to cancel, e.g. \"j12\".")}}),
        [&service](const json& args, ToolContext& ctx) {
            if (args.contains("job")) {
                const std::string job = arg_string(args, "job");
                const auto        ops = service.jobs().running_operations();
                if (std::find(ops.begin(), ops.end(), job) == ops.end())
                    throw ToolFailure("Job " + job + " is not a running slice or export", {{"running", ops}});
            }
            json out = ctx.run_ui([] {
                Plater&    plater  = ui_plater();
                const bool running = plater.is_slicing_any();
                plater.cancel_slicing();
                return json{{"was_running", running}};
            });
            return ToolResult::ok(out);
        });
    cancel.allowed_during_operation = true;
    // Recovery: it must get through a dialog the operation opened and another call's nested loop.
    cancel.guard = GuardClass::Recover;
    reg.add(cancel);
}

}}} // namespace Slic3r::GUI::Mcp
