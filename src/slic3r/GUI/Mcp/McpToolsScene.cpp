// object_list, object_select, object_transform, part_add, object_arrange, plate_slice and job_cancel: the model on the plates, the
// arrange and Fill bed jobs, and the slice operation, which reports its own outcome per plate (see
// Operations/RunStatus). The object and part commands made from menu items: those that add a primitive, delete, rename,
// set the number of instances, make instances separate objects, split, assemble, order, mirror, centre, scale to fit,
// drop and orient objects and change a part's type take their targets (McpTargets.hpp) and run their operations
// (Operations/ObjectOps, ProjectOps); the others run their menu items' functions (NativeCommandMenuItems.hpp) on the selection.
// The object and part commands of menu items with no Speed Dial action, which mark objects printable, turn their Auto
// Drop on or off, repair their meshes, clone them, reload them from their files and replace a part's mesh, take their
// targets and run their operations too.

#include "CanvasTarget.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "JobRegistry.hpp"
#include "McpCore.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Point.hpp"
#include "McpPaths.hpp"
#include "AutomationRuntime.hpp"
#include "McpArguments.hpp"
#include "McpTargets.hpp"
#include "McpTools.hpp"
#include "UiInspector.hpp"
#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/ActionRegistry.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/CommandTypes.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_Factories.hpp"
#include "slic3r/GUI/GUI_Geometry.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosManager.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/NativeCommandMenuItems.hpp"
#include "slic3r/GUI/NativeCommands.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/ObjectDataViewModel.hpp"
#include "slic3r/GUI/Operations/ObjectOps.hpp"
#include "slic3r/GUI/Operations/Outcome.hpp"
#include "slic3r/GUI/Operations/PlateOps.hpp"
#include "slic3r/GUI/Operations/ProjectOps.hpp"
#include "slic3r/GUI/Operations/RunStatus.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Selection.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/EmbossShape.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/GCodeStatistics.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TextConfiguration.hpp"
#include "libslic3r/TextureDisplacement.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/libslic3r.h"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/path.hpp>
#include <wx/arrstr.h>
#include <wx/dataview.h>
#include <wx/string.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;
using Ops::PlateObservation;
using Ops::PlateOutcome;
using Ops::SliceObservation;
using Ops::SliceTicketState;
using Ops::TicketStatus;

namespace {

json vec3(const Vec3d& v) { return {v.x(), v.y(), v.z()}; }

Vec3d to_vec3(const json& v, const char* what)
{
    // Each element too: a schema may give the array no items, and get<double>() throws on anything but a number.
    if (!v.is_array() || v.size() != 3 || !v[0].is_number() || !v[1].is_number() || !v[2].is_number())
        throw ToolFailure(std::string(what) + " must be [x, y, z], three numbers", {{"code", "invalid_argument"}});
    return {v[0].get<double>(), v[1].get<double>(), v[2].get<double>()};
}

// The canvas whose selection the view shown has, switching no view: the assemble view while it is shown, else the
// Prepare view's, whose selection stays while Preview or another tab is shown.
GLCanvas3D& selection_canvas(Plater& plater)
{
    if (GLCanvas3D* assemble = plater.get_assmeble_canvas3D(); assemble != nullptr && plater.get_current_canvas3D() == assemble)
        return *assemble;
    return *plater.get_view3D_canvas3D();
}

// The 3D canvas object operations act on: the assemble view while it is shown, else the Prepare
// view, switched to when another is shown.
GLCanvas3D& object_canvas()
{
    Plater&     plater = ui_plater();
    GLCanvas3D& canvas = selection_canvas(plater);
    if (&canvas != plater.get_assmeble_canvas3D() && !plater.is_view3D_shown()) {
        plater.select_view_3D("3D");
        if (MainFrame* mf = wxGetApp().mainframe)
            mf->select_tab(TAB_ID_PREPARE);
    }
    return canvas;
}

// What Deselect All does, for object_select {none: true} and a pick of its action: the selection of `canvas` clears
// and its open gizmo closes. UI thread.
void clear_selection(GLCanvas3D& canvas)
{
    canvas.deselect_all();
    canvas.set_as_dirty();
}

// What Select All does, for object_select {all: true} and a pick of its action: every object of `canvas` selected, and
// the object list showing it. Refused while a gizmo of that canvas keeps its selection, where GLCanvas3D::select_all()
// would select nothing. UI thread.
std::optional<Ops::Refusal> select_everything(GLCanvas3D& canvas)
{
    if (!canvas.get_gizmos_manager().is_allow_select_all())
        return Ops::gizmo_keeps_selection();
    canvas.select_all();
    wxGetApp().obj_list()->update_selections();
    canvas.set_as_dirty();
    return std::nullopt;
}

} // namespace

// Refused in the assemble view, which shows another canvas, and while a gizmo owns the selection.
GLCanvas3D& require_prepare_scene(Plater& plater)
{
    if (GLCanvas3D* assemble = plater.get_assmeble_canvas3D(); assemble != nullptr && plater.get_current_canvas3D() == assemble)
        throw ToolFailure("Leave the assemble view first (view_set {canvas: \"3d\"})");
    GLCanvas3D& canvas = object_canvas();
    if (const auto type = canvas.get_gizmos_manager().get_current_type(); type != GLGizmosManager::Undefined)
        throw ToolFailure("Close the " + gizmo_name(type) + " gizmo first (widget_click {target: {gizmo: \"" + gizmo_name(type) + "\"}})");
    // A canvas hidden while the model changed loads its scene on its next draw; the selection needs
    // it now, as when the Prepare tab is requested while shown (Plater::priv::set_current_panel()).
    if (canvas.is_reload_delayed())
        canvas.reload_scene(true);
    return canvas;
}

// The 3D view then selects a part on instance 0, since nothing else is selected.
void select_rows(ModelObject* object, ModelVolume* volume)
{
    ui_plater().deselect_all();
    wxGetApp().obj_list()->select_items(std::vector<ObjectVolumeID>{{object, volume}});
}

namespace {

// A scale argument: one factor or [x, y, z] factors, all positive.
Vec3d scale_factors(const json& s)
{
    const Vec3d f = s.is_number() ? Vec3d::Constant(s.get<double>()) : to_vec3(s, "scale");
    if (f.minCoeff() <= 0.)
        throw ToolFailure("scale factors must be positive", {{"code", "invalid_argument"}});
    return f;
}

// Where a part is on the bed, measured on instance 0: the Move panel's World position.
Vec3d part_position(const ModelObject& object, const ModelVolume& volume)
{
    return (object.instances.front()->get_matrix() * volume.get_matrix()).translation();
}

// A part's size along its own axes, in the object's frame: the instance's scale is left out.
Vec3d part_size(const ModelVolume& volume) { return volume.mesh().bounding_box().size().cwiseProduct(volume.get_scaling_factor().cwiseAbs()); }

// Facets painted in each state of an annotation: [none, state 1, state 2, ...], empty when unpainted.
std::vector<std::size_t> painted_facets(const FacetsAnnotation& paint, const ModelVolume& volume)
{
    std::vector<std::size_t> out;
    if (paint.empty())
        return out;
    std::vector<indexed_triangle_set> per_state;
    paint.get_facets(volume, per_state);
    for (const indexed_triangle_set& its : per_state)
        out.push_back(its.indices.size());
    return out;
}

std::size_t painted_in(const std::vector<std::size_t>& counts, EnforcerBlockerType state)
{
    return std::size_t(state) < counts.size() ? counts[std::size_t(state)] : 0;
}

json degrees(const Vec3d& radians) { return vec3(radians * Geometry::rad2deg(1.)); }

json box_json(const BoundingBoxf3& box)
{
    if (!box.defined)
        return nullptr;
    return {{"min", vec3(box.min)}, {"max", vec3(box.max)}, {"size", vec3(box.size())}};
}

// The filament a config's own "extruder" names, 0 when it names none ("Default").
int own_filament(const ModelConfig& config)
{
    const ConfigOption* opt = config.option("extruder");
    return opt != nullptr ? opt->getInt() : 0;
}

int object_filament(const ModelObject& object) { return std::max(own_filament(object.config), 1); }

// The filament the object list's column shows for a part: a model part prints with its own, else
// its object's; a modifier overrides the filament of what it covers only when it has its own; other
// volumes print nothing. 0 for none.
int volume_filament(const ModelVolume& volume)
{
    const int own = own_filament(volume.config);
    if (volume.is_model_part())
        return own > 0 ? own : object_filament(*volume.get_object());
    return volume.is_modifier() ? own : 0;
}

// An object's mesh errors as object_list and object_fix report them.
json mesh_errors_json(const Ops::MeshErrors& errors)
{
    return {{"open_edges", errors.open_edges},           {"manifold", errors.manifold},
            {"degenerate_facets", errors.degenerate_facets}, {"edges_fixed", errors.edges_fixed},
            {"facets_removed", errors.facets_removed},   {"facets_reversed", errors.facets_reversed},
            {"backwards_edges", errors.backwards_edges}};
}

// An instance as object_list describes it, except its plate and canvas rectangle, which the GUI knows.
json instance_info(const ModelObject& object, std::size_t index)
{
    const ModelInstance& inst = *object.instances[index];
    return {{"index", index},
            {"offset", vec3(inst.get_offset())},
            {"rotation_deg", degrees(inst.get_rotation())},
            {"scale", vec3(inst.get_scaling_factor())},
            // Whether, not along which axis: a mirror in X equals one in Y plus a half turn.
            {"mirrored", inst.is_left_handed()},
            {"printable", inst.printable},
            {"bbox", box_json(object.instance_bounding_box(index))}};
}

// The rectangle of logical canvas pixels an instance covers in the shown 3D view, or null when none
// of it is on screen.
json screen_rect(GLCanvas3D& canvas, int object, int instance)
{
    BoundingBoxf3 box;
    for (const GLVolume* v : canvas.get_volumes().volumes)
        if (v->object_idx() == object && v->instance_idx() == instance && !v->is_wipe_tower)
            box.merge(v->transformed_bounding_box());
    if (!box.defined)
        return nullptr;
    const Camera& camera = wxGetApp().plater()->get_camera();
    CanvasView    view{camera.get_view_matrix(), camera.get_projection_matrix(), camera.get_viewport(), canvas.get_scale()};
    BoundingBoxf  rect;
    for (int corner = 0; corner < 8; ++corner)
        if (const auto p = project_point(view, Vec3d(corner & 1 ? box.max.x() : box.min.x(), corner & 2 ? box.max.y() : box.min.y(),
                                                     corner & 4 ? box.max.z() : box.min.z())))
            rect.merge(*p);
    if (!rect.defined)
        return nullptr;
    return {std::lround(rect.min.x()), std::lround(rect.min.y()), std::lround(rect.size().x()), std::lround(rect.size().y())};
}

json list_objects(const json& args)
{
    Plater&        plater = ui_plater();
    PartPlateList& plates = plater.get_partplate_list();
    // Screen rectangles are of the 3D view shown, Prepare's or the assemble view's.
    GLCanvas3D* canvas = nullptr;
    if (arg_bool(args, "screen", false)) {
        canvas = plater.get_current_canvas3D();
        if (canvas == nullptr || canvas == plater.get_preview_canvas3D())
            throw ToolFailure("screen needs the Prepare view or the assemble view shown (view_set)");
    }
    std::vector<std::string> names;
    for (int i = 0; i < plates.get_plate_count(); ++i)
        names.push_back(plates.get_plate(i)->get_plate_name());
    // A rectangle belongs to an instance's entry, which the detailed listing has.
    json listing = args;
    if (canvas != nullptr)
        listing["detail"] = true;
    json out = objects_by_plate(plater.model(), names,
                                [&](std::size_t object, std::size_t instance) { return instance_plate(plates, int(object), int(instance)); },
                                listing);
    if (canvas != nullptr) {
        const auto add_screen = [&](json& objects) {
            for (json& object : objects)
                for (json& instance : object["instances"])
                    instance["screen"] = screen_rect(*canvas, object["index"].get<int>(), instance["index"].get<int>());
        };
        for (json& plate : out["plates"])
            add_screen(plate["objects"]);
        if (out.contains("outside"))
            add_screen(out["outside"]);
    }
    return out;
}

// The plate_slice tool's view of its request: slice_status()'s state, which adds when the tool started
// waiting to what the operation's ticket holds.
struct SliceTicket
{
    SliceTicketState state;
    std::vector<int> plates;
};

// object_transform on an instance: written straight into the ModelInstance, then dropped, registered with
// its plate and reloaded. Arguments are checked before anything changes.
json transform_instance(Plater& plater, int oi, const json& args)
{
    ModelObject* object = plater.model().objects[size_t(oi)];
    const int    ii     = arg_int(args, "instance", 0);
    if (ii < 0 || ii >= int(object->instances.size()))
        throw ToolFailure("instance index out of range");
    ModelInstance* inst     = object->instances[size_t(ii)];
    const bool     absolute = arg_string(args, "mode", "relative") == "absolute";

    std::optional<Vec3d> translate, rotate, scale;
    if (args.contains("translate"))
        translate = to_vec3(args["translate"], "translate");
    else if (args.contains("position"))
        translate = to_vec3(args["position"], "position");
    if (args.contains("rotate"))
        rotate = to_vec3(args["rotate"], "rotate") * Geometry::deg2rad(1.0);
    if (args.contains("scale"))
        scale = absolute ? scale_factors(args["scale"]) : Vec3d(inst->get_scaling_factor().cwiseProduct(scale_factors(args["scale"])));
    else if (args.contains("size"))
        // Along the object's own axes: its model parts unrotated, times the instance's scale.
        scale = Vec3d(inst->get_scaling_factor().cwiseProduct(
            size_factors(object->raw_mesh_bounding_box().size().cwiseProduct(inst->get_scaling_factor().cwiseAbs()), args["size"])));
    PartPlateList& plates = plater.get_partplate_list();
    Vec3d          to_plate = Vec3d::Zero();
    if (args.contains("plate")) {
        const int to = arg_int(args, "plate", 0);
        if (to >= plates.get_plate_count())
            throw ToolFailure(no_such_plate(to, plates.get_plate_count()) + "; action_run plate_add makes one");
        const int from = plates.find_instance(oi, ii);
        if (from < 0)
            throw ToolFailure("instance " + std::to_string(ii) + " is off every plate; place it with translate (absolute) first");
        to_plate = plates.get_plate(to)->get_origin() - plates.get_plate(from)->get_origin();
    }

    plater.take_snapshot("Transform object");
    if (translate)
        inst->set_offset(absolute || args.contains("position") ? *translate : Vec3d(inst->get_offset() + *translate));
    if (rotate) {
        if (absolute)
            inst->set_rotation(*rotate);
        else {
            const Transform3d rot = Geometry::rotation_transform(*rotate) * Geometry::rotation_transform(inst->get_rotation());
            inst->set_rotation(Geometry::extract_euler_angles(rot));
        }
    }
    if (scale)
        inst->set_scaling_factor(*scale);
    // The plate move is one more relative translate, after the others.
    inst->set_offset(inst->get_offset() + to_plate);
    Ops::placed_instance(oi, ii, arg_bool(args, "drop_to_bed", true));
    return {{"object", oi},
            {"instance", ii},
            {"offset", vec3(inst->get_offset())},
            {"scale", vec3(inst->get_scaling_factor())},
            {"plate", plates.find_instance(oi, ii)}};
}

// object_transform on a part: the Object manipulation panel's path with the part selected, so do_move(),
// do_rotate() and do_scale() write the part, drop the object (Auto Drop), register its instances
// with their plates and update the object list. Translate and rotate are along the bed's axes on
// instance 0, scale and size along the part's own axes. The part is one resolve_targets() checked.
json transform_part(Plater& plater, int oi, const json& args)
{
    ModelObject* object = plater.model().objects[size_t(oi)];
    const int    vi     = arg_int(args, "part", 0);
    // A single part has no row of its own in the object list, so the panel can't select it.
    if (object->volumes.size() == 1)
        throw ToolFailure("object " + std::to_string(oi) + " has one part; transform the instance");
    if (arg_int(args, "instance", 0) != 0)
        throw ToolFailure("a part belongs to its object and moves on every instance; leave instance out");
    if (args.contains("plate"))
        throw ToolFailure("plate moves an instance; leave part out");
    const bool absolute = arg_string(args, "mode", "relative") == "absolute";
    if (absolute && args.contains("rotate"))
        throw ToolFailure("rotate a part relative to its current rotation (mode \"relative\")");
    ModelVolume* volume = object->volumes[size_t(vi)];
    GLCanvas3D&  canvas = require_prepare_scene(plater);

    std::optional<Vec3d> translate, rotate, scale;
    if (args.contains("translate")) {
        const Vec3d t = to_vec3(args["translate"], "translate");
        translate     = absolute ? Vec3d(t - part_position(*object, *volume)) : t;
    } else if (args.contains("position")) {
        translate = Vec3d(to_vec3(args["position"], "position") - part_position(*object, *volume));
    }
    if (args.contains("rotate"))
        rotate = to_vec3(args["rotate"], "rotate") * Geometry::deg2rad(1.0);
    // Factors on the current scale, as the panel's change_scale_value() and change_size_value()
    // compute them: Selection::scale() multiplies a part's scale whatever the transformation type.
    if (args.contains("scale"))
        scale = absolute ? Vec3d(scale_factors(args["scale"]).cwiseQuotient(volume->get_scaling_factor())) : scale_factors(args["scale"]);
    else if (args.contains("size"))
        scale = size_factors(part_size(*volume), args["size"]);

    select_rows(object, volume);
    Selection& sel = canvas.get_selection();
    if (!sel.is_single_volume_or_modifier())
        throw ToolFailure("part " + std::to_string(vi) + " of object " + std::to_string(oi) + " could not be selected");
    plater.take_snapshot("Transform part", UndoRedo::SnapshotType::GizmoAction);
    if (translate) {
        sel.setup_cache();
        sel.translate(*translate, TransformationType::World_Relative_Joint);
        canvas.do_move("");
    }
    if (rotate) {
        sel.setup_cache();
        sel.rotate(*rotate, TransformationType::World_Relative_Joint);
        canvas.do_rotate("");
    }
    if (scale) {
        sel.setup_cache();
        sel.scale(*scale, TransformationType::Local_Relative_Joint);
        canvas.do_scale("");
    }
    return {{"object", oi}, {"part", vi}, {"size", vec3(part_size(*volume))}, {"part_info", volume_info(*volume, size_t(vi))}};
}

// An arrange or Fill bed the call started, and what its reply compares with once the job is done.
struct ArrangeStart
{
    json                                  settings;
    json                                  skipped = json::array(); // plates the all-plates arrange leaves alone
    std::string                           note;
    std::set<std::pair<int, std::string>> notifications;           // shown before it started
    int                                   fill_object = -1;
    bool                                  copies      = false;
    std::size_t                           before      = 0;         // the fill object's instances, or the objects for copies
};

std::set<std::pair<int, std::string>> shown_notifications(Plater& plater)
{
    std::set<std::pair<int, std::string>> out;
    for (const NotificationSummary& n : plater.get_notification_manager()->summaries())
        out.emplace(n.type, n.text);
    return out;
}

// Validates object_arrange's arguments, applies its settings and starts the job the toolbar's Arrange, a
// plate's Arrange icon or Fill bed starts. UI thread.
ArrangeStart start_arrange(Plater& plater, const json& args)
{
    const bool fill = args.contains("fill_object");
    // Fill bed selects its object first, so it needs what every tool that selects needs. An arrangement
    // runs from whatever view and with whatever gizmo is open, as the toolbar's Arrange does.
    GLCanvas3D&            canvas  = fill ? require_prepare_scene(plater) : Ops::shown_prepare_canvas();
    PartPlateList&         plates  = plater.get_partplate_list();
    const ModelObjectPtrs& objects = plater.model().objects;
    ArrangeStart           start;
    if (fill && args.contains("plates"))
        throw ToolFailure("fill_object fills the object's own plate; leave plates out");
    if (!fill && args.contains("fill_with"))
        throw ToolFailure("fill_with goes with fill_object");
    if (const std::optional<Ops::Refusal> refusal = Ops::arrange_refusal({}))
        throw refused(*refusal);
    int plate = -1; // every unlocked plate
    if (fill) {
        start.fill_object = arg_int(args, "fill_object", 0);
        if (start.fill_object >= int(objects.size()))
            throw ToolFailure("object index out of range");
        select_rows(objects[size_t(start.fill_object)], nullptr);
        if (!plater.can_increase_instances())
            throw ToolFailure("Fill bed refuses object " + std::to_string(start.fill_object) +
                              ": it is a cut object or has an instance that is not printable");
    } else if (const json p = args.value("plates", json("all")); p.is_string()) {
        if (p == "current")
            plate = plates.get_curr_plate_index();
        else if (p != "all")
            throw ToolFailure("plates is \"all\", \"current\" or a plate index");
    } else {
        plate = p.get<int>();
        if (plate < 0 || plate >= plates.get_plate_count())
            throw ToolFailure(no_such_plate(plate, plates.get_plate_count()));
    }
    const std::string plate_name = "Plate " + std::to_string(plate);
    if (plate >= 0 && plates.get_plate(plate)->is_locked())
        throw ToolFailure(plate_name + " is locked; unlock it first (plate_lock {\"plate\": " + std::to_string(plate) +
                          ", \"locked\": false})");
    if (plate >= 0 && plates.get_plate(plate)->empty())
        throw ToolFailure(plate_name + " has no objects to arrange");

    // Applied and saved as the panel does, once the call is known to go ahead.
    GLCanvas3D::ArrangeSettings& settings = canvas.get_arrange_settings();
    start.note                            = apply_arrange_args(settings, args);
    if (args.contains("spacing") || args.contains("rotate") || args.contains("multi_material"))
        canvas.store_arrange_settings();
    start.settings              = arrange_settings_json(settings);
    start.settings["by_object"] = settings.is_seq_print;

    // A plate whose print sequence differs from the project's is arranged with automatic spacing on
    // its own (init_arrange_params()), and left alone by the all-plates arrange (prepare_all()).
    for (int i = 0; i < plates.get_plate_count(); ++i) {
        PartPlate* pp   = plates.get_plate(i);
        bool       same = true;
        pp->get_real_print_seq(&same);
        if (i == plate && !same && settings.distance > 0.f)
            start.note += std::string(start.note.empty() ? "" : " ") + plate_name +
                          "'s print sequence differs from the project's, so its spacing was automatic.";
        if (plate < 0 && !fill && !pp->empty() && (pp->is_locked() || !same))
            start.skipped.push_back({{"plate", i},
                                     {"reason", pp->is_locked() ? "locked" : "its print sequence differs from the project's; arrange it by index"}});
    }

    start.notifications = shown_notifications(plater);
    if (fill) {
        start.copies = arg_string(args, "fill_with", "instances") == "copies";
        start.before = start.copies ? objects.size() : objects[size_t(start.fill_object)]->instances.size();
        if (start.copies)
            plater.fill_bed_with_copies();
        else
            plater.fill_bed_with_instances();
    } else {
        Ops::ArrangeArgs arrange;
        if (plate >= 0)
            arrange.plate = plate;
        if (const Ops::Outcome<Ops::Done> started = Ops::start_arrange(arrange); !started)
            throw refused(started.refusal());
    }
    if (plater.get_ui_job_worker().is_idle())
        throw ToolFailure("The arrangement did not start");
    return start;
}

// object_arrange's reply once the worker is idle. UI thread.
json arrange_reply(Plater& plater, const ArrangeStart& start)
{
    PartPlateList&         plates  = plater.get_partplate_list();
    const ModelObjectPtrs& objects = plater.model().objects;
    json                   out{{"settings", start.settings}, {"plates", plates.get_plate_count()}};
    if (start.fill_object >= 0 && start.fill_object < int(objects.size())) {
        const std::size_t now = start.copies ? objects.size() : objects[size_t(start.fill_object)]->instances.size();
        out.update({{"object", start.fill_object},
                    {"filled_with", start.copies ? "copies" : "instances"},
                    {"added", int(now) - int(start.before)},
                    {start.copies ? "objects" : "instances", now},
                    {"plate", plates.find_instance(start.fill_object, 0)}});
    }
    // Instances arrange could not fit: the job reports them only in its status line. Measured rather
    // than read from the plate registry, which Fill bed leaves without its new instances.
    json off_plate = json::array();
    for (std::size_t o = 0; o < objects.size(); ++o)
        for (std::size_t i = 0; i < objects[o]->instances.size(); ++i) {
            bool on_plate = false;
            for (int p = 0; p < plates.get_plate_count() && !on_plate; ++p)
                on_plate = plates.get_plate(p)->intersect_instance(int(o), int(i));
            if (!on_plate)
                off_plate.push_back({o, i});
        }
    out["off_plate"] = std::move(off_plate);
    if (!start.skipped.empty())
        out["skipped_plates"] = start.skipped;
    // What the job told the person: objects too large for any bed (a plain notification), zero-size
    // objects and locked plates (warnings). Selection info and progress are left out.
    json notes = json::array();
    for (const NotificationSummary& n : plater.get_notification_manager()->summaries())
        if ((n.level == "warning" || n.level == "error" || n.type == int(NotificationType::CustomNotification)) &&
            start.notifications.count({n.type, n.text}) == 0)
            notes.push_back({{"level", n.level}, {"text", n.text}});
    if (!notes.empty())
        out["notifications"] = std::move(notes);
    if (!start.note.empty())
        out["note"] = start.note;
    return out;
}

// The object list's name of a part_add shape, which its menus and the Primitive size dialog use;
// nullptr for another shape.
const char* shape_type_name(const std::string& shape)
{
    static const std::pair<const char*, const char*> names[] = {{"cube", "Cube"}, {"cylinder", "Cylinder"}, {"sphere", "Sphere"},
                                                                {"cone", "Cone"}, {"disc", "Disc"},         {"torus", "Torus"}};
    for (const auto& [shape_name, type_name] : names)
        if (shape == shape_name)
            return type_name;
    return nullptr;
}

// part_add: what the object's context menu Add items do, on object `oi`, which resolve_targets() checked, with the
// size or the file given instead of asked for, then a move to `position` along the Move panel's path. Everything the
// arguments can tell is checked before anything changes.
json add_part(Plater& plater, int oi, const json& args)
{
    const ModelObjectPtrs& objects = plater.model().objects;
    const ModelVolumeType  type    = volume_type_from_name(arg_string(args, "type"));
    if (type == ModelVolumeType::INVALID)
        throw ToolFailure("type must be part, negative, modifier, support_blocker, support_enforcer or precise_seam");
    if (args.contains("shape") == args.contains("path"))
        throw ToolFailure("Give shape or path");
    const std::string   shape = arg_string(args, "shape");
    std::vector<double> size;
    wxArrayString       files;
    if (!shape.empty())
        size = part_shape_size(shape, args.value("size", json()));
    else {
        if (args.contains("size"))
            throw ToolFailure("size goes with shape; a file's part keeps the file's size");
        std::string err;
        const auto  file = resolve_input_path(arg_string(args, "path"), err);
        if (!file)
            throw ToolFailure(err);
        // The SVG gizmo makes SVG parts, placed where the camera looks.
        if (boost::algorithm::iends_with(path_to_utf8(*file), ".svg"))
            throw ToolFailure("SVG parts come from object_add_svg (a part of the selected object) or the SVG gizmo");
        files.Add(from_path(*file));
    }
    std::optional<Vec3d> position;
    if (args.contains("position"))
        position = to_vec3(args["position"], "position");
    ModelObject* object = objects[size_t(oi)];
    // Adding a shape folds instance 0's transformation into the parts, which other instances
    // would not follow.
    if (object->instances.size() > 1)
        throw ToolFailure("object " + std::to_string(oi) + " has " + std::to_string(object->instances.size()) +
                          " instances; add the part before adding instances");
    GLCanvas3D& canvas = require_prepare_scene(plater);

    // The menu's functions add to the object selected in the object list.
    select_rows(object, nullptr);
    ObjectList* list = wxGetApp().obj_list();
    if (list->get_selected_obj_idx() != oi || !list->is_instance_or_object_selected())
        throw ToolFailure("object " + std::to_string(oi) + " could not be selected");

    // The add's own snapshot ("Add primitive", "Load Part", "Load Modifier") is the call's only undo step.
    Plater::SingleSnapshot              single(&plater);
    const std::set<const ModelVolume*> before(object->volumes.begin(), object->volumes.end());
    if (!files.IsEmpty()) {
        std::string error;
        list->load_subobject(type, files, false, &error);
        if (!error.empty())
            throw ToolFailure("The file could not be loaded: " + error);
    } else
        list->load_generic_subobject(shape_type_name(shape), type, false, size);
    const auto added = std::find_if(object->volumes.begin(), object->volumes.end(),
                                    [&before](const ModelVolume* v) { return before.count(v) == 0; });
    if (added == object->volumes.end())
        throw ToolFailure("The part was not added");
    ModelVolume* volume = *added;

    if (position) {
        select_rows(object, volume);
        Selection& sel = canvas.get_selection();
        if (!sel.is_single_volume_or_modifier())
            throw ToolFailure("The part was added but could not be selected to move it");
        sel.setup_cache();
        sel.translate(*position - part_position(*object, *volume), TransformationType::World_Relative_Joint);
        canvas.do_move("");
    }
    const auto vi = std::size_t(std::find(object->volumes.begin(), object->volumes.end(), volume) - object->volumes.begin());
    return {{"object", oi}, {"part", vi}, {"part_count", object->volumes.size()}, {"part_info", volume_info(*volume, vi)}};
}

} // namespace

int instance_plate(PartPlateList& plates, int object, int instance)
{
    int plate = plates.find_instance(object, instance);
    for (int p = 0; plate < 0 && p < plates.get_plate_count(); ++p)
        if (plates.get_plate(p)->intersect_instance(object, instance))
            plate = p;
    return plate;
}

json selection_state(GLCanvas3D& canvas)
{
    const Selection&              sel = canvas.get_selection();
    std::set<int>                 objects;
    std::set<std::pair<int, int>> instances, volumes;
    bool                          wipe_tower = false;
    for (unsigned int idx : sel.get_volume_idxs()) {
        const GLVolume* v = sel.get_volume(idx);
        if (v == nullptr)
            continue;
        if (v->is_wipe_tower) {
            wipe_tower = true;
            continue;
        }
        objects.insert(v->object_idx());
        instances.emplace(v->object_idx(), v->instance_idx());
        if (v->volume_idx() >= 0)
            volumes.emplace(v->object_idx(), v->volume_idx());
    }
    json out{{"canvas", canvas_name(&canvas)}, {"empty", sel.is_empty()}, {"objects", objects}, {"instances", instances}};
    // A whole instance brings all its parts along; only a selection of parts lists them.
    if (sel.get_mode() == Selection::Volume && !volumes.empty())
        out["parts"] = volumes;
    if (wipe_tower)
        out["wipe_tower"] = true;
    return out;
}

const char* volume_type_name(ModelVolumeType type)
{
    if (is_precise_seam(type))
        return "precise_seam";
    switch (type) {
    case ModelVolumeType::MODEL_PART: return "part";
    case ModelVolumeType::NEGATIVE_VOLUME: return "negative";
    case ModelVolumeType::PARAMETER_MODIFIER: return "modifier";
    case ModelVolumeType::SUPPORT_BLOCKER: return "support_blocker";
    case ModelVolumeType::SUPPORT_ENFORCER: return "support_enforcer";
    default: return "other";
    }
}

ModelVolumeType volume_type_from_name(const std::string& name)
{
    for (const ModelVolumeType type : {ModelVolumeType::MODEL_PART, ModelVolumeType::NEGATIVE_VOLUME, ModelVolumeType::PARAMETER_MODIFIER,
                                       ModelVolumeType::SUPPORT_BLOCKER, ModelVolumeType::SUPPORT_ENFORCER, ModelVolumeType::PRECISE_SEAM_CENTER})
        if (name == volume_type_name(type))
            return type;
    return ModelVolumeType::INVALID;
}

// The subtype's name is the 3MF type name without its "precise_seam_" prefix, as volume_info() gives it.
ModelVolumeType precise_seam_subtype(const std::string& name)
{
    for (const ModelVolumeType seam : {ModelVolumeType::PRECISE_SEAM_CENTER, ModelVolumeType::PRECISE_SEAM_LEFT, ModelVolumeType::PRECISE_SEAM_RIGHT,
                                       ModelVolumeType::PRECISE_SEAM_ENFORCED, ModelVolumeType::PRECISE_SEAM_BLOCKED,
                                       ModelVolumeType::PRECISE_SEAM_NEUTRAL})
        if ("precise_seam_" + name == ModelVolume::type_to_string(seam))
            return seam;
    return ModelVolumeType::INVALID;
}

std::vector<double> part_shape_size(const std::string& shape, const json& size)
{
    const char* type_name = shape_type_name(shape);
    if (type_name == nullptr)
        throw ToolFailure("shape must be cube, cylinder, sphere, cone, disc or torus");
    if (size.is_null())
        return {};
    const std::vector<const char*> names = primitive_size_names(type_name);
    std::vector<double>            out;
    if (size.is_array() && std::all_of(size.begin(), size.end(), [](const json& value) { return value.is_number(); }))
        out = size.get<std::vector<double>>();
    if (!is_primitive_size(type_name, out)) {
        std::string fields;
        for (const char* name : names)
            fields += (fields.empty() ? "" : ", ") + std::string(name);
        throw ToolFailure("A " + shape + "'s size is [" + fields + "] in mm, each above 0");
    }
    return out;
}

json object_info(const ModelObject& object, std::size_t index)
{
    json instances = json::array();
    for (std::size_t j = 0; j < object.instances.size(); ++j)
        instances.push_back(instance_info(object, j));
    json volumes = json::array();
    for (std::size_t v = 0; v < object.volumes.size(); ++v)
        volumes.push_back(volume_info(*object.volumes[v], v));
    json overrides = json::array();
    for (const std::string& key : object.config.keys())
        overrides.push_back(key);
    json out{{"index", index},
             {"id", object.id().id},
             {"name", object.name},
             {"filament", object_filament(object)},
             {"auto_drop", object.auto_drop},
             {"instances", std::move(instances)},
             {"parts", std::move(volumes)},
             {"bbox", object.instances.empty() ? json(nullptr) : box_json(object.instance_bounding_box(0))},
             {"brim_ears", object.brim_points.size()},
             {"overrides", std::move(overrides)},
             {"mesh_errors", mesh_errors_json(Ops::mesh_errors(object))}};
    if (!object.layer_config_ranges.empty())
        out["layer_config_ranges"] = layer_config_ranges_json(object.layer_config_ranges);
    if (json profile = layer_height_profile_json(object.layer_height_profile.get()); !profile.is_null())
        out["layer_height_profile"] = std::move(profile);
    return out;
}

json volume_info(const ModelVolume& v, std::size_t index)
{
    const TriangleMesh& mesh        = v.mesh();
    const double        mesh_volume = its_volume(mesh.its);
    json out{{"index", index},
             {"id", v.id().id},
             {"name", v.name},
             {"type", volume_type_name(v.type())},
             {"rotation_deg", degrees(v.get_rotation())},
             {"scale", vec3(v.get_scaling_factor())},
             {"mirrored", v.is_left_handed()},
             {"triangles", mesh.its.indices.size()},
             {"mesh_volume", std::round(mesh_volume * 1000.) / 1000.}};
    if (v.is_precise_seam()) // the 3MF type name without its "precise_seam_" prefix: center, left, ...
        out["subtype"] = ModelVolume::type_to_string(v.type()).substr(std::strlen("precise_seam_"));
    if (const int filament = volume_filament(v); filament > 0)
        out["filament"] = filament;
    // On the bed, measured on instance 0 as the object's bbox is.
    if (const ModelObject* object = v.get_object(); object != nullptr && !object->instances.empty()) {
        const Transform3d placed = object->instances.front()->get_matrix() * v.get_matrix();
        out["position"]          = vec3(placed.translation());
        out["bbox"]              = box_json(mesh.transformed_bounding_box(placed));
        out["scaled_volume"]     = std::round(mesh_volume * std::abs(placed.linear().determinant()) * 1000.) / 1000.;
    }
    json painted = json::object();
    if (const auto c = painted_facets(v.supported_facets, v); !c.empty())
        painted["supports"] = {{"enforcers", painted_in(c, EnforcerBlockerType::ENFORCER)}, {"blockers", painted_in(c, EnforcerBlockerType::BLOCKER)}};
    if (const auto c = painted_facets(v.seam_facets, v); !c.empty())
        painted["seam"] = {{"enforcers", painted_in(c, EnforcerBlockerType::ENFORCER)}, {"blockers", painted_in(c, EnforcerBlockerType::BLOCKER)}};
    if (const auto c = painted_facets(v.fuzzy_skin_facets, v); !c.empty())
        painted["fuzzy_skin"] = painted_in(c, EnforcerBlockerType::FUZZY_SKIN);
    if (const auto c = painted_facets(v.mmu_segmentation_facets, v); !c.empty()) {
        json mmu = json::object();
        for (std::size_t state = 1; state < c.size(); ++state)
            if (c[state] > 0)
                mmu[std::to_string(state)] = c[state]; // filament (extruder) number
        painted["mmu"] = std::move(mmu);
    }
    if (!v.texture_displacement_layers.empty()) {
        json layers = json::array();
        for (const TextureDisplacementLayer& layer : v.texture_displacement_layers) {
            json l{{"slot", layer.slot}, {"name", layer.name}, {"path", layer.path}, {"depth_mm", layer.depth_mm}};
            if (layer.slot >= 0 && layer.slot < int(TEXTURE_DISPLACEMENT_MAX_LAYERS))
                l["painted"] = painted_in(painted_facets(v.texture_displacement_facet(layer.slot), v), EnforcerBlockerType::ENFORCER);
            layers.push_back(std::move(l));
        }
        painted["texture"] = std::move(layers);
    }
    if (!painted.empty())
        out["painted"] = std::move(painted);
    if (v.text_configuration) {
        const EmbossStyle& style = v.text_configuration->style;
        out["text"] = {{"text", v.text_configuration->text},
                       {"style", style.name},
                       {"font", style.prop.face_name ? *style.prop.face_name : style.path}};
    }
    if (v.emboss_shape && v.emboss_shape->svg_file)
        out["svg"] = {{"path", v.emboss_shape->svg_file->path}, {"path_in_3mf", v.emboss_shape->svg_file->path_in_3mf}};
    return out;
}

json object_summary(const ModelObject& object, std::size_t index)
{
    const BoundingBoxf3 box = object.instances.empty() ? BoundingBoxf3() : object.instance_bounding_box(0);
    return {{"index", index},
            {"id", object.id().id},
            {"name", object.name},
            {"filament", object_filament(object)},
            {"part_count", object.volumes.size()},
            {"size", box.defined ? vec3(box.size()) : json(nullptr)}};
}

json objects_by_plate(const Model& model, const std::vector<std::string>& plate_names,
                      const std::function<int(std::size_t, std::size_t)>& plate_of, const json& args)
{
    const bool detail      = arg_bool(args, "detail", false);
    const int  plate_count = int(plate_names.size());
    const int only        = arg_int(args, "plate", -1);
    if (args.contains("plate") && (only < 0 || only >= plate_count))
        throw ToolFailure(no_such_plate(only, plate_count));

    // The plate of every instance (-1: none), and the objects to list: all, or those on the plate asked for.
    std::vector<std::vector<int>> on(model.objects.size());
    std::vector<std::size_t>      listed;
    for (std::size_t i = 0; i < model.objects.size(); ++i) {
        for (std::size_t j = 0; j < model.objects[i]->instances.size(); ++j)
            on[i].push_back(std::max(-1, plate_of(i, j)));
        if (only < 0 || std::find(on[i].begin(), on[i].end(), only) != on[i].end())
            listed.push_back(i);
    }

    json plates = json::array();
    for (int plate = 0; plate < plate_count; ++plate)
        if (only < 0 || plate == only)
            plates.push_back({{"index", plate}, {"name", plate_names[std::size_t(plate)]}, {"objects", json::array()}});
    json              outside = json::array();
    const std::size_t total   = listed.size();
    const std::size_t offset  = std::min<std::size_t>(std::size_t(std::max(0, arg_int(args, "offset", 0))), total);
    const std::size_t limit   = std::size_t(std::max(1, arg_int(args, "limit", 200)));
    for (std::size_t n = offset; n < total && n - offset < limit; ++n) {
        const std::size_t i    = listed[n];
        json              item = detail ? object_info(*model.objects[i], i) : object_summary(*model.objects[i], i);
        // Each instance's entry with detail, else its index.
        const json every = detail ? std::move(item["instances"]) : json();
        // The object's row where it has an instance; one without any instance is on no plate either.
        const auto add_row = [&](int plate, json& into) {
            json instances = json::array();
            for (std::size_t j = 0; j < on[i].size(); ++j)
                if (on[i][j] == plate)
                    instances.push_back(detail ? every[j] : json(j));
            if (instances.empty() && (plate >= 0 || !on[i].empty()))
                return;
            json row         = item;
            row["instances"] = std::move(instances);
            into.push_back(std::move(row));
        };
        for (json& plate : plates)
            add_row(plate["index"].get<int>(), plate["objects"]);
        if (only < 0)
            add_row(-1, outside);
    }
    json out{{"plates", std::move(plates)}, {"total", total}, {"offset", offset}};
    if (only < 0)
        out["outside"] = std::move(outside);
    if (offset + limit < total)
        out["next_offset"] = offset + limit;
    return out;
}

std::string apply_arrange_args(GLCanvas3D::ArrangeSettings& settings, const json& args)
{
    if (args.contains("spacing"))
        settings.distance = std::max(0.f, args["spacing"].get<float>());
    if (args.contains("rotate"))
        settings.enable_rotation = args["rotate"].get<bool>();
    if (args.contains("multi_material"))
        settings.allow_multi_materials_on_same_plate = args["multi_material"].get<bool>();
    if (args.contains("align_y"))
        settings.align_to_y_axis = args["align_y"].get<bool>();
    // The panel disables Align to Y axis while rotation is on, and clears it.
    if (settings.apply_option_rules() && arg_bool(args, "align_y", false))
        return "align_y is off while rotate is on, as in the Arrange options panel.";
    return {};
}

json arrange_settings_json(const GLCanvas3D::ArrangeSettings& settings)
{
    return {{"spacing", std::round(double(settings.distance) * 100.) / 100.},
            {"rotate", settings.enable_rotation},
            {"align_y", settings.align_to_y_axis},
            {"multi_material", settings.allow_multi_materials_on_same_plate}};
}

Vec3d size_factors(const Vec3d& current, const json& size)
{
    if (!size.is_array() || size.size() != 3)
        throw ToolFailure("size must be [x, y, z] mm, with null for an axis to keep");
    Vec3d factors = Vec3d::Ones();
    int   given = 0, axis = 0;
    for (int i = 0; i < 3; ++i) {
        if (size[i].is_null())
            continue;
        if (!size[i].is_number() || size[i].get<double>() <= 0.)
            throw ToolFailure("sizes must be positive numbers of mm");
        if (current[i] < EPSILON)
            throw ToolFailure(std::string("it is flat along ") + "XYZ"[i] + ", so that size can't be set");
        factors[i] = size[i].get<double>() / current[i];
        ++given;
        axis = i;
    }
    if (given == 0)
        throw ToolFailure("size needs a number for at least one axis");
    return given == 1 ? Vec3d::Constant(factors[axis]) : factors;
}

json plate_summary(const json& statistics, const GCodeProcessorResult& result)
{
    const auto   hundredths = [](double value) { return std::round(value * 100.) / 100.; };
    const json&  summary    = statistics.at("summary");
    const double seconds    = summary.at("time").get<double>();
    json         out{{"time_s", std::lround(seconds)},
                     {"time", get_time_dhms(float(seconds))},
                     {"filament_g", hundredths(summary.at("weight").get<double>())},
                     {"filament_cost", summary.at("filament_cost")},
                     {"layers", summary.at("layer_count")},
                     {"filament_changes", summary.at("filament_changes")}};
    // The processor leaves the silent-mode time 0 when the printer has no such estimate.
    const PrintEstimatedStatistics& estimates = result.print_statistics;
    const double silent = estimates.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Stealth)].time;
    if (silent > 0.)
        out["silent_time_s"] = std::lround(silent);
    if (estimates.total_extruder_changes > 0)
        out["extruder_changes"] = estimates.total_extruder_changes;
    double max_z = 0.;
    for (const json& layer : statistics.at("layers"))
        max_z = std::max(max_z, layer.at("z").get<double>());
    out["max_z"] = max_z;

    json filaments = json::array();
    const auto add_slot = [&filaments] {
        filaments.push_back({{"slot", filaments.size() + 1}, {"m", 0.}, {"g", 0.}, {"model_g", 0.}, {"support_g", 0.},
                             {"flush_g", 0.}, {"tower_g", 0.}});
    };
    while (filaments.size() < result.filaments_count)
        add_slot();
    double length = 0.;
    for (const json& used : summary.at("filaments")) {
        const size_t slot = used.at("id").get<size_t>();
        while (filaments.size() < slot)
            add_slot();
        const auto grams = [&used, &hundredths](const char* use) { return hundredths(used.at(use).at("weight").get<double>()); };
        json&      f     = filaments[slot - 1];
        f["m"]           = hundredths(used.at("length").get<double>() / 1000.);
        f["g"]           = hundredths(used.at("weight").get<double>());
        f["model_g"]     = grams("model");
        f["support_g"]   = grams("support");
        f["flush_g"]     = grams("flush");
        f["tower_g"]     = grams("prime_tower");
        length += used.at("length").get<double>();
    }
    out["filament_m"] = hundredths(length / 1000.);
    out["filaments"]  = std::move(filaments);
    return out;
}

namespace {

// The Prepare view's selection, the view shown first when another is, as the object menu items read it: what a command made
// from menu items acts on when it is given no target. Null for a call that names its targets, which leaves the view shown.
const Selection* prepare_selection(const json& args)
{
    return names_targets(args) ? nullptr : &Ops::shown_prepare_canvas().get_selection();
}

// What a command made from menu items answers for a refusal: as information where the person can select something else or
// wait and pick it again, as the items told those, else as an error.
AppActionRunResult menu_refused(const Ops::Refusal& refusal)
{
    const std::string code  = refusal.code;
    const bool        again = refusal.kind == Ops::RefusalKind::Busy || code == "nothing_selected" || code == "wrong_selection" ||
                       code == "one_object_only";
    return NativeCommands::refused(refusal, again ? AppActionRunResult::Level::Info : AppActionRunResult::Level::Error);
}

// A refusal a command made from menu items answers as information, where its item told that refusal, or the check it stands
// for, as information.
AppActionRunResult menu_info(const Ops::Refusal& refusal) { return NativeCommands::refused(refusal, AppActionRunResult::Level::Info); }

constexpr unsigned kEveryTarget = kObjects | kInstances | kParts;

// The object list's selected rows as targets, for a command whose menu item acted on them (Assemble, Change Type), read in
// the Prepare view, shown first, as the menu item read them: an object row as its object, an instance row as [object,
// instance] and a part row as [object, part], by the part's index in the model. Refuses nothing_selected for no row and
// wrong_selection for a row of none of `types` (ItemType bits). UI thread.
Ops::Outcome<Targets> listed_rows(int types)
{
    Ops::show_prepare_view();
    ObjectList&          list = *wxGetApp().obj_list();
    ObjectDataViewModel& rows = *list.GetModel();
    wxDataViewItemArray  selected;
    list.GetSelections(selected);
    if (selected.IsEmpty())
        return Ops::nothing_selected();
    Targets out;
    out.from_selection = true;
    for (const wxDataViewItem& row : selected) {
        const ItemType type   = rows.GetItemType(row);
        const int      object = rows.GetObjectIdByItem(row);
        if ((type & types) == 0 || object < 0)
            return Ops::wrong_selection();
        if (type & itObject)
            out.objects.push_back(object);
        else if (type & itInstance)
            out.instances.emplace_back(object, rows.GetInstanceIdByItem(row));
        else if (type & itVolume)
            out.parts.emplace_back(object, rows.get_real_volume_index_in_3d(object, rows.GetVolumeIdByItem(row)));
        else
            return Ops::wrong_selection();
    }
    return out;
}

// What Split takes from the object list, as ObjectList::is_splittable() reads it in the Prepare view, shown first: its
// one selected row. Into objects, the object of its row or of a row of the object's own other than a part's; into
// parts, the part of a part row, else the object of the row. `object_row` says whether the row is the object's.
struct SplitRow
{
    Targets target;
    bool    object_row = false;
};
Ops::Outcome<SplitRow> split_row(bool to_objects)
{
    Ops::show_prepare_view();
    ObjectList&          list = *wxGetApp().obj_list();
    ObjectDataViewModel& rows = *list.GetModel();
    wxDataViewItemArray  selected;
    list.GetSelections(selected);
    if (selected.IsEmpty())
        return Ops::nothing_selected();
    if (selected.size() > 1)
        return Ops::one_object_only();
    const wxDataViewItem row    = selected.front();
    const ItemType       type   = rows.GetItemType(row);
    const int            object = rows.GetObjectIdByItem(row);
    if (object < 0 || (to_objects && (type == itVolume || (type != itObject && rows.GetItemType(rows.GetParent(row)) != itObject))))
        return Ops::wrong_selection();
    SplitRow out;
    out.target.from_selection = true;
    out.object_row            = type == itObject;
    if (!to_objects && type == itVolume)
        out.target.parts.emplace_back(object, rows.GetVolumeIdByItem(row));
    else
        out.target.objects.push_back(object);
    return out;
}

// What stands on plate `plate`: the objects whose every instance is on it, and the instances on it of the others.
Targets plate_targets(Plater& plater, int plate)
{
    PartPlateList& plates = plater.get_partplate_list();
    if (plate < 0 || plate >= plates.get_plate_count())
        throw ToolFailure(no_such_plate(plate, plates.get_plate_count()));
    const ModelObjectPtrs& objects = plater.model().objects;
    Targets                out;
    for (std::size_t o = 0; o < objects.size(); ++o) {
        std::vector<int> on;
        for (std::size_t i = 0; i < objects[o]->instances.size(); ++i)
            if (instance_plate(plates, int(o), int(i)) == plate)
                on.push_back(int(i));
        if (!on.empty() && on.size() == objects[o]->instances.size())
            out.objects.push_back(int(o));
        else
            for (int i : on)
                out.instances.emplace_back(int(o), i);
    }
    if (out.objects.empty() && out.instances.empty())
        throw ToolFailure("Plate " + std::to_string(plate) + " has no objects");
    return out;
}

// The entries of `named` (targets_reply()'s) for the targets `which` names by the same indices, under the same keys.
json entries_of(const json& named, const Targets& which)
{
    const auto has = [](const auto& list, const auto& item) { return std::find(list.begin(), list.end(), item) != list.end(); };
    json       out = json::object();
    const auto keep = [&out, &named](const char* key, const std::function<bool(const json&)>& wanted) {
        json entries = json::array();
        for (const json& entry : named.value(key, json::array()))
            if (wanted(entry))
                entries.push_back(entry);
        if (!entries.empty())
            out[key] = std::move(entries);
    };
    const auto pair_of = [](const json& e, const char* second) { return std::pair(e.at("object").get<int>(), e.at(second).get<int>()); };
    keep("objects", [&](const json& e) { return has(which.objects, e.at("index").get<int>()); });
    keep("instances", [&](const json& e) { return has(which.instances, pair_of(e, "instance")); });
    keep("parts", [&](const json& e) { return has(which.parts, pair_of(e, "part")); });
    return out;
}

// object_delete: what the call names, what stands on its plate, every object with all, else the selection. Only its
// person's form, a call with no arguments, leaves a cut object and a last solid part to the object list, which asks
// its question or shows its message, as a person's Delete does; any other call is refused with them.
AppActionRunResult delete_command(const json& args)
{
    const bool all = arg_bool(args, "all", false);
    if (all && args.contains("plate"))
        throw ToolFailure("Give all or plate, not both");
    if ((all || args.contains("plate")) && names_targets(args))
        throw ToolFailure(all ? "all deletes every object; leave the targets out" : "plate deletes what stands on it; leave the targets out");
    Plater&      plater = ui_plater();
    const Model& model  = plater.model();

    Ops::DeleteObjectsArgs op;
    op.all    = all;
    op.checks = person_form(args) ? Ops::DeleteChecks::ObjectList : Ops::DeleteChecks::Refuse;
    if (args.contains("plate")) {
        op.targets = plate_targets(plater, arg_int(args, "plate", 0));
    } else if (!all) {
        const Ops::Outcome<Targets> found = resolve_targets(model, prepare_selection(args), args, kEveryTarget, false);
        if (!found)
            return menu_refused(found.refusal());
        op.targets = found.value();
    }
    // Named before the delete renumbers what follows.
    Targets doomed = Ops::deletion(model, op.targets);
    if (all)
        for (std::size_t i = 0; i < model.objects.size(); ++i)
            doomed.objects.push_back(int(i));
    const json named = targets_reply(model, doomed);
    const Ops::Outcome<Ops::DeletedTargets> deleted = Ops::delete_objects(op);
    if (!deleted) {
        if (std::string(deleted.refusal().code) != "nothing_deleted")
            return menu_refused(deleted.refusal());
        // The object list told the person why, or they answered its question themself: the pick shows nothing more,
        // and a call learns that nothing was deleted.
        AppActionRunResult kept{AppActionRunResult::Level::Info, {}, false};
        kept.refusal = std::make_shared<const Ops::Refusal>(deleted.refusal());
        return kept;
    }
    json reply = entries_of(named, deleted.value().deleted);
    if (const json kept = targets_reply(model, deleted.value().kept); !kept.empty())
        reply["kept"] = kept;
    reply["object_count"] = model.objects.size();
    return menu_reply(reply);
}

// object_rename: the object or part the call names, else the one selected. A call with no arguments asks for the name
// in the Rename dialog, as the menu item does; any other gives it.
AppActionRunResult rename_command(const json& args)
{
    const bool        asks = person_form(args);
    const std::string name = arg_string(args, "name");
    // Before anything is read, so a name that cannot be given leaves the view as it is.
    if (!asks)
        if (const std::optional<Ops::Refusal> refusal = Ops::rename_refusal(name))
            return menu_refused(*refusal);
    Plater&                     plater = ui_plater();
    const Ops::Outcome<Targets> found  = resolve_targets(plater.model(), prepare_selection(args), args, kObjects | kParts, true);
    if (!found)
        return menu_refused(std::string(found.refusal().code) == "wrong_selection" ? Ops::rename_wrong_selection() : found.refusal());
    const Targets& target = found.value();
    const int      object = target.parts.empty() ? target.objects.front() : target.parts.front().first;
    const int      part   = target.parts.empty() ? -1 : target.parts.front().second;
    if (asks) {
        if (const std::optional<Ops::Refusal> refusal = Ops::unlisted_part_refusal(plater.model(), object, part))
            return menu_refused(*refusal);
        // The dialog renames the object list's selected row, which is the target's once every instance of an object
        // selected stands for it. A part's row is by its place among the rows, which leave a cut's connectors out.
        ObjectList*           list  = wxGetApp().obj_list();
        ObjectDataViewModel*  rows  = list->GetModel();
        ModelObject*          owner = plater.model().objects[std::size_t(object)];
        const wxDataViewItem  item  = part < 0 ? rows->GetItemById(object) :
                                                 rows->GetItemByVolumeId(object, rows->get_real_volume_index_in_ui(object, part));
        if (list->GetSelection() != item)
            select_rows(owner, part < 0 ? nullptr : owner->volumes[std::size_t(part)]);
        list->rename_item();
    } else if (const Ops::Outcome<std::string> renamed = Ops::rename_object({object, part, name}); !renamed) {
        return menu_refused(renamed.refusal());
    }
    return menu_reply(targets_reply(plater.model(), target));
}

// object_set_instances: the object the call names, else the one selected, or the object of the selected instance or
// part, as the item reads it. A call with no arguments asks for the count in the item's dialog; delta is the
// toolbar's + and -.
AppActionRunResult instances_command(const json& args)
{
    const bool asks = person_form(args);
    if (args.contains("count") && args.contains("delta"))
        throw ToolFailure("Give count or delta, not both");
    if (!asks && !args.contains("count") && !args.contains("delta"))
        throw ToolFailure("Give count, or delta 1 or -1; with no arguments the dialog asks for the count");
    Plater&                     plater = ui_plater();
    const Ops::Outcome<Targets> found =
        resolve_targets(plater.model(), prepare_selection(args), args, kObjects | kObjectOfSelection, true);
    if (!found)
        return menu_refused(found.refusal());
    const int         object = found.value().objects.front();
    const std::size_t had    = plater.model().objects[std::size_t(object)]->instances.size();
    if (asks) {
        // The asking path: the item's dialog, on the selected object, enabled as the menu enables it.
        if (const std::optional<Ops::Refusal> refusal = Ops::add_instances_refusal(plater.model(), object))
            return menu_refused(*refusal);
        if (const std::optional<Ops::Refusal> refusal = Ops::instance_change_blocked())
            return menu_refused(*refusal);
        plater.set_number_of_copies();
    } else {
        const int count = args.contains("count") ? args.at("count").get<int>() : int(had) + args.at("delta").get<int>();
        if (const Ops::Outcome<Ops::InstanceCount> set = Ops::set_instance_count({object, count}); !set)
            return menu_refused(set.refusal());
    }
    json reply         = targets_reply(plater.model(), found.value());
    reply["object"]    = object;
    reply["instances"] = plater.model().objects[std::size_t(object)]->instances.size();
    return menu_reply(reply);
}

// object_separate_instances: the object the call names or the instances of one object, else the selection, where a part
// selected stands for its instance, as Set as Individual Objects reads it.
AppActionRunResult separate_command(const json& args)
{
    Plater&                     plater = ui_plater();
    const Ops::Outcome<Targets> found  = resolve_targets(plater.model(), prepare_selection(args), args,
                                                         kObjects | kInstances | kInstanceOfSelectedPart, false);
    if (!found)
        return menu_refused(found.refusal());
    const Ops::Outcome<Targets> holders = Ops::separate_instances(found.value());
    if (!holders)
        return menu_refused(holders.refusal());
    return menu_reply(targets_reply(plater.model(), holders.value()));
}

// object_split: the object the call names, else the object list's one selected row (split_row()), as the Split menu items
// read it. A call with an action's arguments asks what Split asks (the Auto-Drop question, a message for an object of one
// piece); any other answers the question with keep_z, and is refused without it. The menu items told every refusal of theirs
// as information.
AppActionRunResult split_command(const json& args)
{
    const std::string    into       = arg_string(args, "into");
    const bool           to_objects = into == "objects";
    const bool           asks       = args == json{{"into", into}};
    Plater&              plater     = ui_plater();
    Ops::SplitObjectArgs op;
    if (names_targets(args)) {
        const Ops::Outcome<Targets> found = resolve_targets(plater.model(), nullptr, args, kObjects, true);
        if (!found)
            return menu_info(found.refusal());
        op.target = found.value();
    } else {
        const Ops::Outcome<SplitRow> found = split_row(to_objects);
        if (!found)
            return menu_info(found.refusal());
        op.target = found.value().target;
        op.expand = found.value().object_row;
    }
    op.to_objects = to_objects;
    op.ask        = asks;
    if (args.contains("keep_z"))
        op.keep_z = args.at("keep_z").get<bool>();
    const Ops::Outcome<Targets> made = Ops::split_object(op);
    if (!made)
        return asks && std::string(made.refusal().code) == "one_piece" ? quiet_refusal(made.refusal()) : menu_info(made.refusal());
    json reply            = targets_reply(plater.model(), made.value());
    reply["object_count"] = plater.model().objects.size();
    return menu_reply(reply);
}

// object_assemble: the objects the call names, else the object and instance rows selected in the object list, which
// the Assemble menu item merged. The menu item told a selection of fewer than two, or of parts, as information, and a cut object as
// an error.
AppActionRunResult assemble_command(const json& args)
{
    Plater&                     plater = ui_plater();
    const Model&                model  = plater.model();
    const Ops::Outcome<Targets> found  = names_targets(args) ? resolve_targets(model, nullptr, args, kObjects | kInstances, false) :
                                                               listed_rows(itObject | itInstance);
    if (!found)
        return menu_info(std::string(found.refusal().code) == "wrong_selection" ? *Ops::assemble_refusal(model, Targets{}) :
                                                                                  found.refusal());
    const Ops::Outcome<int> assembled = Ops::assemble_objects(found.value());
    if (!assembled)
        return std::string(assembled.refusal().code) == "too_few_objects" ? menu_info(assembled.refusal()) :
                                                                            menu_refused(assembled.refusal());
    const int index = assembled.value();
    Targets   made;
    made.objects    = {index};
    json reply      = targets_reply(model, made);
    reply["object"] = index;
    reply["name"]   = model.objects[std::size_t(index)]->name;
    return menu_reply(reply);
}

// part_change_type: the parts the call names, else the part rows selected in the object list, which the Change Type
// menu items changed. The menu item told a selection of anything but parts as information, in its own words, and why a
// part keeps its type as an error.
AppActionRunResult part_type_command(const json& args)
{
    std::optional<ModelVolumeType> subtype;
    if (args.contains("subtype"))
        subtype = precise_seam_subtype(arg_string(args, "subtype"));
    Plater&                     plater = ui_plater();
    const Ops::Outcome<Targets> found  = names_targets(args) ? resolve_targets(plater.model(), nullptr, args, kParts, false) :
                                                               listed_rows(itVolume);
    if (!found)
        return menu_info(std::string(found.refusal().code) == "wrong_selection" ? Ops::part_wrong_selection() : found.refusal());
    const Ops::Outcome<Targets> changed = Ops::change_part_type({found.value(), volume_type_from_name(arg_string(args, "type")), subtype});
    if (!changed)
        return menu_refused(changed.refusal());
    json reply = targets_reply(plater.model(), changed.value());
    for (json& part : reply["parts"]) {
        const ModelVolume& volume = *plater.model().objects[part["object"].get<std::size_t>()]->volumes[part["part"].get<std::size_t>()];
        part["type"]              = volume_type_name(volume.type());
    }
    return menu_reply(reply);
}

// object_order's arguments, checked before the call reads the app: by, or the objects in order, which names one plate's.
void check_order_arguments(const json& args)
{
    const bool by    = args.contains("by");
    const bool order = args.contains("order");
    if (by && order)
        throw ToolFailure("Give by or order, not both");
    if (!by && !order)
        throw ToolFailure("Give by, or the objects in order");
    if (order && args.contains("all"))
        throw ToolFailure("order names the objects of one plate: leave all out");
    if (order && args.contains("descending"))
        throw ToolFailure("descending goes with by: order gives the objects in their order");
    if (arg_bool(args, "all", false) && args.contains("plate"))
        throw ToolFailure("Give plate or all, not both");
}

// The objects of plate `sorted`, or of every plate with `all`, in their order, as object_order replies them. UI thread.
json plate_objects(int sorted, bool all)
{
    Plater&                plater  = ui_plater();
    PartPlateList&         plates  = plater.get_partplate_list();
    const ModelObjectPtrs& objects = plater.model().objects;
    json                   order   = json::array();
    for (int plate = 0; plate < plates.get_plate_count(); ++plate)
        if (all || plate == sorted)
            for (std::size_t i = 0; i < objects.size(); ++i)
                if (plates.find_instance(int(i), 0) == plate)
                    order.push_back({{"object", i}, {"name", objects[i]->name}, {"plate", plate}});
    return order;
}

// object_order: the current plate, the plate the call names, or every plate with all. Its action's call ({by}) shows
// the Prepare view and reverses an order the objects already have, as the plate menu's Order By does; any other call
// with by is ascending unless descending says otherwise. With order, the plate's objects go in that order, as drags in
// the object list put them. The menu item told a plate with no objects as information.
AppActionRunResult order_command(const json& args)
{
    if (const auto order = args.find("order"); order != args.end()) {
        Ops::ReorderObjectsArgs op;
        op.order = order->get<std::vector<int>>();
        if (args.contains("plate"))
            op.plate = arg_int(args, "plate", 0);
        const Ops::Outcome<std::vector<int>> ordered = Ops::reorder_objects(op);
        if (!ordered)
            return menu_refused(ordered.refusal());
        const int plate = op.plate.value_or(ui_plater().get_partplate_list().get_curr_plate_index());
        return menu_reply({{"order", plate_objects(plate, false)}});
    }
    const std::string by  = arg_string(args, "by");
    const bool        all = arg_bool(args, "all", false);
    const bool asks = args == json{{"by", by}};
    if (asks)
        Ops::show_prepare_view();
    static const std::pair<const char*, ObjectOrder> orders[] = {
        {"name", ObjectOrder::Name}, {"filament", ObjectOrder::Filament}, {"height", ObjectOrder::Height}, {"size", ObjectOrder::Size}};
    Ops::OrderObjectsArgs op{};
    for (const auto& [name, order] : orders)
        if (by == name)
            op.order = order;
    op.all = all;
    if (args.contains("plate"))
        op.plate = arg_int(args, "plate", 0);
    if (args.contains("descending") || !asks)
        op.descending = arg_bool(args, "descending", false);
    const Ops::Outcome<bool> ordered = Ops::order_objects(op);
    if (!ordered)
        return std::string(ordered.refusal().code) == "no_objects_to_order" ? menu_info(ordered.refusal()) :
                                                                              menu_refused(ordered.refusal());

    // The sorted plates' objects in their new order.
    const int sorted = op.plate.value_or(ui_plater().get_partplate_list().get_curr_plate_index());
    return menu_reply({{"order", plate_objects(sorted, all)}, {"descending", ordered.value()}});
}

// object_mirror: the objects and parts the call names, else the selection, which Mirror takes of one instance or parts
// of one. The menu item told every refusal as information.
AppActionRunResult mirror_command(const json& args)
{
    const std::string           axis   = arg_string(args, "axis");
    Plater&                     plater = ui_plater();
    const Ops::Outcome<Targets> found  = resolve_targets(plater.model(), prepare_selection(args), args, kEveryTarget, false);
    if (!found)
        return menu_info(found.refusal());
    const Ops::Outcome<Ops::Done> mirrored = Ops::mirror_objects({found.value(), axis == "x" ? Axis::X : axis == "y" ? Axis::Y : Axis::Z});
    if (!mirrored)
        return menu_info(mirrored.refusal());
    return menu_reply(targets_reply(plater.model(), found.value()));
}

// object_center: the objects the call names, else the selection; on the plate each stands on, or on plate. Its action's
// call ({}) is Center on the selection. The menu item told a selection to make as information.
AppActionRunResult center_command(const json& args)
{
    Plater&            plater = ui_plater();
    std::optional<int> plate;
    if (args.contains("plate")) {
        plate = arg_int(args, "plate", 0);
        // Before the selection is read, so a plate the project lacks is named whatever is selected.
        if (const int count = plater.get_partplate_list().get_plate_count(); *plate >= count)
            return menu_refused(Ops::no_such_plate(*plate, count));
    }
    const Ops::Outcome<Targets> found = resolve_targets(plater.model(), prepare_selection(args), args, kEveryTarget, false);
    if (!found)
        return menu_info(found.refusal());
    const Ops::Outcome<std::map<int, std::vector<int>>> centred = Ops::center_objects({found.value(), plate});
    if (!centred)
        return menu_refused(centred.refusal());
    json reply  = targets_reply(plater.model(), found.value());
    json plates = json::array();
    for (const auto& [on, objects] : centred.value())
        plates.push_back({{"plate", on}, {"objects", objects}});
    reply["plates"] = std::move(plates);
    return menu_reply(reply);
}

// object_scale_to_fit: the objects the call names, else the objects and instances selected, as Scale to Fit Print Volume
// takes them. The menu item told every refusal as information.
AppActionRunResult scale_to_fit_command(const json& args)
{
    Plater&                     plater = ui_plater();
    const Ops::Outcome<Targets> found  = resolve_targets(plater.model(), prepare_selection(args), args, kObjects | kInstances, false);
    if (!found)
        return menu_info(found.refusal());
    if (const Ops::Outcome<Ops::Done> fitted = Ops::scale_to_fit(found.value()); !fitted)
        return menu_info(fitted.refusal());
    const Model& model = plater.model();
    std::vector<std::pair<int, int>> instances = found.value().instances;
    for (int object : found.value().objects)
        for (std::size_t i = 0; i < model.objects[std::size_t(object)]->instances.size(); ++i)
            instances.emplace_back(object, int(i));
    json scales = json::array();
    for (const auto& [object, instance] : instances)
        scales.push_back({{"object", object},
                          {"instance", instance},
                          {"scale", vec3(model.objects[std::size_t(object)]->instances[std::size_t(instance)]->get_scaling_factor())}});
    json reply      = targets_reply(model, found.value());
    reply["scales"] = std::move(scales);
    return menu_reply(reply);
}

// object_drop: the objects and parts the call names, else the selection. What already rests on the bed is no error: the
// pick tells it, as information, and the call's reply says that nothing moved.
AppActionRunResult drop_command(const json& args)
{
    Plater&                     plater = ui_plater();
    const Ops::Outcome<Targets> found  = resolve_targets(plater.model(), prepare_selection(args), args, kEveryTarget, false);
    if (!found)
        return menu_info(found.refusal());
    const Ops::Outcome<bool> dropped = Ops::drop_objects(found.value());
    if (!dropped)
        return menu_info(dropped.refusal());
    json reply     = targets_reply(plater.model(), found.value());
    reply["moved"] = dropped.value();
    if (dropped.value())
        return menu_reply(reply);
    AppActionRunResult resting = menu_info(Ops::already_on_bed());
    resting.done               = true;
    resting.reply              = reply.dump();
    return resting;
}

// object_auto_orient's targets: the objects named, else the selection, an instance or a part standing for its object as
// the job reads it, and with nothing selected (or the prime tower) every object, as Auto-Orient does; plates add theirs.
// Shows the Prepare view where it reads the selection. UI thread.
Ops::Outcome<Ops::OrientObjectsArgs> orient_args(Plater& plater, const json& args)
{
    Ops::OrientObjectsArgs op;
    for (const json& plate : args.value("plates", json::array()))
        op.plates.push_back(plate.get<int>());
    if (op.plates.empty() || names_targets(args)) {
        const Ops::Outcome<Targets> found =
            resolve_targets(plater.model(), prepare_selection(args), args, kObjects | kObjectOfSelection, false);
        if (found)
            op.targets = found.value();
        else if (const std::string code = found.refusal().code; code == "nothing_selected" || code == "wrong_selection")
            op.targets.from_selection = true;
        else
            return found.refusal();
    }
    return op;
}

// What object_auto_orient replies once the job has ended: the rotation of each instance it turned (Ops::orientable()),
// of the objects named or of the plates named, else as the job turns a selection: the instances of the selection it
// can turn, or, with none of those and none of the selection on a locked plate, of every object.
json orient_reply(Plater& plater, const Ops::OrientObjectsArgs& op)
{
    const Model&                        model  = plater.model();
    PartPlateList&                      plates = plater.get_partplate_list();
    const std::function<bool(int, int)> locked = Ops::on_locked_plate(plates);
    std::set<int>                       objects(op.targets.objects.begin(), op.targets.objects.end());
    for (std::size_t o = 0; o < model.objects.size(); ++o)
        for (std::size_t i = 0; i < model.objects[o]->instances.size(); ++i)
            if (std::find(op.plates.begin(), op.plates.end(), instance_plate(plates, int(o), int(i))) != op.plates.end())
                objects.insert(int(o));
    std::vector<std::pair<int, int>> turned = Ops::orientable(model, objects, locked);
    const auto                       some_locked = [&]() {
        for (int object : objects)
            for (std::size_t i = 0; i < model.objects[std::size_t(object)]->instances.size(); ++i)
                if (locked(object, int(i)))
                    return true;
        return false;
    };
    if (turned.empty() && op.targets.from_selection && op.plates.empty() && !some_locked()) {
        std::set<int> every;
        for (std::size_t o = 0; o < model.objects.size(); ++o)
            every.insert(int(o));
        turned = Ops::orientable(model, every, locked);
    }
    Targets oriented;
    json    rotations = json::array();
    for (const auto& [object, instance] : turned) {
        if (oriented.objects.empty() || oriented.objects.back() != object)
            oriented.objects.push_back(object);
        const ModelInstance& turned_instance = *model.objects[std::size_t(object)]->instances[std::size_t(instance)];
        rotations.push_back({{"object", object}, {"instance", instance}, {"rotation_deg", degrees(turned_instance.get_rotation())}});
    }
    json reply         = targets_reply(model, oriented);
    reply["rotations"] = std::move(rotations);
    return reply;
}

// object_add_primitive: a call with the action's arguments ({shape}) is Add Primitive, refused while an object is
// selected and asking for the size; any other adds the shape at its size, or the built-in one, whatever is selected, on
// plate, at position, named name, in one undo step (Ops::add_primitive()).
AppActionRunResult add_primitive_command(const json& args)
{
    const std::string     shape = arg_string(args, "shape");
    Ops::AddPrimitiveArgs op;
    // The size first: it refuses a shape of another name, which shape_type_name() has none for.
    op.size  = part_shape_size(shape, args.value("size", json()));
    op.shape = shape_type_name(shape);
    op.ask   = args == json{{"shape", shape}};
    if (args.contains("plate"))
        op.plate = arg_int(args, "plate", 0);
    if (args.contains("name"))
        op.name = arg_string(args, "name");
    if (args.contains("position"))
        op.position = to_vec3(args["position"], "position");

    Plater&                 plater = ui_plater();
    const Ops::Outcome<int> added  = Ops::add_primitive(op);
    if (!added)
        // The size dialog cancelled told the person nothing more is to come.
        return op.ask && std::string(added.refusal().code) == "not_added" ? quiet_refusal(added.refusal()) : menu_refused(added.refusal());
    const int index = added.value();
    Targets made;
    made.objects    = {index};
    json reply      = targets_reply(plater.model(), made);
    reply["object"] = index;
    reply["plate"]  = instance_plate(plater.get_partplate_list(), index, 0);
    reply["offset"] = vec3(plater.model().objects[std::size_t(index)]->instances.front()->get_offset());
    return menu_reply(reply);
}

// The object and part commands made from menu items: each acts on the targets it is given or, given none, on what is
// selected in the Prepare view, as a person's pick of the item does. Those of the object's and part's menus read their
// targets through resolve_targets() and run their operations; the handy models, Text and SVG run the menu item their
// variant argument names. Their actions are the menu items, under the items' ids, titles, sections and icons.
void register_object_menu_commands(CommandCatalog& catalog)
{
    using NativeCommands::action;
    const char* const object_section = L("Object");

    // ---- Add Primitive and Add Handy models ---- (the Add menu; each creates a new object)
    const json shape_size = array_prop({{"type", "number"}}, "mm, the Primitive size dialog's fields, in place of the dialog: cube "
                                                             "`[x, y, z]`; cylinder, cone, disc `[diameter, height]`; sphere `[diameter]`; "
                                                             "torus `[inner diameter, tube diameter]`; each above 0.");
    // A shape the plate may then turn toward the part-cooling fan.
    const std::string turned = " The shape may then be turned toward the part-cooling fan, so the size is along its own axes before that turn.";
    const auto primitive = [&turned](const char* key, const char* title, const char* icon, const char* shape, bool turns) {
        return action(key, title, L("Add Primitive"), icon, {{"shape", shape}},
                      std::string("Add a ") + shape + " object to the current plate at the size its dialog asks for; fails while an "
                                                      "object is selected. object_add_primitive with a size, or silent, skips the "
                                                      "dialog." +
                          (turns ? turned : std::string()));
    };
    // With shape alone the size dialog asks, as the actions do; a size or silent skips it.
    NativeCommand add_primitive = menu_command(
        "object_add_primitive", L("Add a primitive"),
        "Add a primitive shape (cube, cylinder, sphere, cone, disc or torus) as a new object, as Add Primitive does: on the "
        "current plate, or on plate, which becomes the current one; at position, in the bed coordinates `object_list` and "
        "`object_transform` use, else where Add Primitive puts it; named name, else after its shape. With a size, at that "
        "size; with silent, at the shape's built-in size. With shape alone, as its Speed Dial actions, the Primitive size "
        "dialog asks for the size (`blocked_by_dialog`), unless a person turned that dialog off, and the call is refused "
        "while an object is selected (`part_add` adds a shape to an object); any other call adds the object whatever is "
        "selected. One undo step; the new object is selected. Reply: the object (objects, object), its plate and offset." +
            turned,
        object_schema({{"shape", enum_prop({"cube", "cylinder", "sphere", "cone", "disc", "torus"}, "The shape.")},
                       {"size", shape_size},
                       {"plate", {{"type", "integer"}, {"minimum", 0}, {"description", "The plate to add it on. Default: the current plate."}}},
                       {"position", array_prop({{"type", "number"}}, "`[x, y, z]` mm: where its first instance goes, as `object_transform`'s "
                                                                     "position; it then rests on the bed.")},
                       {"name", prop("string", "The object's name. Default: its shape's.")}},
                      {"shape"}),
        json::array({primitive("add_primitive_cube", L("Cube"), "menu_obj_cube", "cube", false),
                     primitive("add_primitive_cylinder", L("Cylinder"), "menu_obj_cylinder", "cylinder", true),
                     primitive("add_primitive_sphere", L("Sphere"), "menu_obj_sphere", "sphere", false),
                     primitive("add_primitive_cone", L("Cone"), "menu_obj_cone", "cone", true),
                     primitive("add_primitive_disc", L("Disc"), "menu_obj_disc", "disc", true),
                     primitive("add_primitive_torus", L("Torus"), "menu_obj_torus", "torus", true)}),
        add_primitive_command);
    add_primitive.input_schema = with_silent(std::move(add_primitive.input_schema), person_forms(add_primitive));
    catalog.add(reaching({"add_primitive", "rename_object"}, std::move(add_primitive)));

    // The two handy models of several files, which the menu arranges once they are loaded.
    static const std::pair<const char*, const char*> arranged[] = {
        {"orca_cube", "Add the Orca Cube and its plug to the current plate, then arrange the objects as the toolbar's Arrange "
                      "does, which moves the others too."},
        {"orcasliced_combo", "Add the OrcaSliced Combo (the OrcaSliced model, the Orca Cube and its plug) to the current plate, "
                             "then arrange the objects as the toolbar's Arrange does, which moves the others too."}};
    std::vector<std::string> models;
    json                     handy_actions = json::array();
    for (const MenuFactory::HandyModel& model : MenuFactory::handy_models()) {
        models.emplace_back(model.key);
        std::string description = std::string("Add the ") + model.label + " model to the current plate.";
        for (const auto& [key, text] : arranged)
            if (models.back() == key)
                description = text;
        handy_actions.push_back(action("add_handy_" + models.back(), model.label, L("Add Handy models"), "", {{"model", model.key}},
                                       description));
    }
    catalog.add(menu_command(
        "object_add_handy_model", L("Add a handy model"),
        "Add one of the test models OrcaSlicer ships to the current plate, as Add Handy models does; `orca_cube` and "
        "`orcasliced_combo`, of several files, then arrange the objects as the toolbar's Arrange does, which moves the others too. "
        "Reply: the objects added (objects: index, id and name).",
        object_schema({{"model", enum_prop(models, "The model.")}}, {"model"}), std::move(handy_actions), [](const json& args) {
            const std::string                           model = arg_string(args, "model");
            const std::vector<MenuFactory::HandyModel>& all   = MenuFactory::handy_models();
            const auto it = std::find_if(all.begin(), all.end(), [&model](const MenuFactory::HandyModel& m) { return model == m.key; });
            const Model&       scene  = ui_plater().model();
            const std::size_t  before = scene.objects.size();
            AppActionRunResult added  = MenuItems::add_handy(std::size_t(it - all.begin()));
            // The loader appends a model's objects after the others.
            Targets made;
            for (std::size_t i = before; i < scene.objects.size(); ++i)
                made.objects.push_back(int(i));
            if (!made.objects.empty())
                added.reply = targets_reply(scene, made).dump();
            return added;
        }));

    // What a text or an SVG takes beyond the action's form: the object it becomes a part of, and the part's type.
    const auto part_props = [](std::initializer_list<Property> own) {
        json schema = object_schema(own);
        schema["properties"]["type"] = enum_prop({"part", "negative", "modifier"},
                                                 "The part's type, as the object's Add menu offers it. Default part. Without "
                                                 "object, on the selected object, or the object of the parts selected, "
                                                 "which is selected whole first: refused (`nothing_selected`, "
                                                 "`one_object_only`) unless one object is selected.");
        return with_targets(std::move(schema), kObjects, true);
    };
    const std::string text = "Add a text: a part on the selected object, or a new object when nothing is selected. It is created "
                             "in the background (`app_wait {for: \"idle\"}` waits for it), and the Text gizmo opens on it.";
    catalog.add(with_expected_ids(menu_command(
        "object_add_text", L("Text"),
        text + " With object, the text becomes a part of that object, which is selected first; type makes it a negative part "
               "or a modifier, as the object's Add menu does. Reply: only that it ran (`ran`, `level`), since the text is made "
               "later: once `app_wait {for: \"idle\"}` returns, `object_list` shows it and `app_state` the gizmo.",
        part_props({}),
        json::array({action("add_primitive_text", L("Text"), L("Add Primitive"), "menu_obj_text", json::object(), text)}),
        [](const json& args) { return MenuItems::add_primitive_text(args); })));

    catalog.add(with_expected_ids(menu_command(
        "object_add_svg", L("SVG"),
        "Add an SVG as a new object, or as a part of the selected object. With a path, uses that file without a dialog; the "
        "part is created in the background (`app_wait {for: \"idle\"}` waits for it), and the SVG gizmo opens on it. With "
        "object, the SVG becomes a part of that object, which is selected first; type makes it a negative part or a modifier, "
        "as the object's Add menu does. Without path the file dialog opens, as the action's does, and the object and type "
        "apply to the file picked there. Reply: only that it ran (`ran`, `level`), since the part is made later: once "
        "`app_wait {for: \"idle\"}` returns, `object_list` shows it and `app_state` the gizmo.",
        part_props({{"path", MenuItems::input_file("an .svg file")}}),
        json::array({action("add_primitive_svg", L("SVG"), L("Add Primitive"), "menu_obj_svg", json::object(),
                            "Add an SVG as a new object, or as a part of the selected object, from the file a dialog asks for; "
                            "object_add_svg with a path uses that file without one. The part is created in the background "
                            "(app_wait {for: \"idle\"} waits for it), and the SVG gizmo opens on it.")}),
        [](const json& args) { return MenuItems::add_primitive_svg(args); })));

    // ---- Object ---- (the object's context menu and the Edit menu)
    NativeCommand del = menu_command(
        "object_delete", L("Delete objects"),
        "Delete objects, instances or parts, as Delete does: those named (objects, instances, parts), what stands on a plate "
        "(plate: its objects, or the instances on it of an object on two plates), every object (all, as Delete All does), or "
        "with none of these what is selected in the Prepare view. An object whose every instance or every part is named goes "
        "whole. An object's last solid part alone, and a cut object or a solid or negative part of one, are refused with "
        "the message or the question Delete gives; a call with no arguments leaves them to the object list, which shows "
        "the message or asks the question (`blocked_by_dialog`), and all deletes cut objects without asking, as Delete All "
        "does. One undo step. Reply: what was deleted (objects, instances, parts, with the indices they had); kept, what "
        "was named and stayed, with its indices now (the object list deletes from the last named and stops at what it "
        "refuses or asks about); and `object_count`, the objects left; the objects after a deleted one move down.",
        with_targets(object_schema({{"plate", {{"type", "integer"}, {"minimum", 0}, {"description", "Delete what stands on this plate."}}},
                                    {"all", prop("boolean", "Delete every object. Default false: the targets.")}}),
                     kEveryTarget, false),
        json::array({action("obj_delete", L("Delete Selected"), object_section, "delete", json::object(),
                            "Delete the selected objects or parts."),
                     action("obj_delete_all", L("Delete All Objects"), object_section, "delete", {{"all", true}}, "Delete all objects.")}),
        delete_command);
    del.input_schema = with_silent(std::move(del.input_schema), person_forms(del));
    catalog.add(reaching({"delete_objects"}, with_expected_ids(std::move(del))));

    // The Rename dialog's name, which every call but the action's gives.
    NativeCommand rename = menu_command(
        "object_rename", L("Rename"),
        "Rename an object, or a part of it (object and part), or with neither the object or part selected in the Prepare "
        "view. With a name, skips the Rename dialog; an empty name, or one with characters a file name cannot hold, is "
        "refused. Reply: what was renamed, with its name (objects or parts).",
        with_targets(object_schema({{"name", prop("string", "The new name, in place of the Rename dialog.")}}, {"name"}), kObjects | kParts, true),
        json::array({action("obj_rename", L("Rename"), object_section, "", json::object(),
                            "Rename the selected object or part (select one) in the Rename dialog; object_rename with a name skips it.")}),
        rename_command);
    rename.input_schema = with_silent(std::move(rename.input_schema), person_forms(rename));
    catalog.add(reaching({"rename_object"}, with_expected_ids(std::move(rename))));

    // The count the dialog asks for, which every call but the actions' gives; delta is the toolbar's + and -.
    NativeCommand instances = menu_command(
        "object_set_instances", L("Set the number of instances"),
        "Give an object this many instances (count), as Set Number of Instances does, or one more or one fewer (delta 1 "
        "or -1), as the toolbar's + and - do: the object named, else the one selected in the Prepare view, or the object "
        "of the selected instance or part. With no arguments the dialog asks for the count (`blocked_by_dialog`). New "
        "instances overlap; arrange afterwards. One undo step; the object is selected. Reply: the object (objects, "
        "object) and instances, how many it has.",
        with_targets(object_schema({{"count", {{"type", "integer"},
                                               {"minimum", 1},
                                               {"description", "The total number of instances, up to 1000, in place of the dialog."}}},
                                    {"delta", {{"type", "integer"},
                                               {"enum", {1, -1}},
                                               {"description", "1 adds an instance and -1 removes one, as the toolbar's + and - do."}}}}),
                     kObjects, true),
        json::array({action("obj_set_instances", L("Set Number of Instances"), object_section, "instance_add", json::object(),
                            "Give the selected object the number of instances the dialog asks for, in one undo step; "
                            "object_set_instances with a count skips the dialog. New instances overlap; arrange "
                            "afterwards."),
                     action("obj_instances_up", L("Increase Instances"), object_section, "instance_add", {{"delta", 1}},
                            "Add an instance of the selected object."),
                     action("obj_instances_down", L("Decrease Instances"), object_section, "instance_remove", {{"delta", -1}},
                            "Remove an instance of the selected object.")}),
        instances_command);
    instances.input_schema = with_silent(std::move(instances.input_schema), person_forms(instances));
    // Its silent form needs count or delta, which a schema of this server cannot require as one of two.
    instances.input_schema["properties"]["silent"]["description"] =
        "true runs the command without asking; it then needs count or delta. A call with no arguments, or with exactly an "
        "action's arguments, does what a person's pick of that action does.";
    catalog.add(reaching({"set_instance_count"}, with_expected_ids(std::move(instances))));

    NativeCommand separate = menu_command(
        "object_separate_instances", L("Set as Individual Objects"),
        "Make each instance of an object a separate object (object), or some instances of one object one new object "
        "(instances), as Set as Individual Objects does; with neither, what is selected in the Prepare view: an object, or "
        "some of its instances. New objects are added last. Reply: the objects that hold those instances afterwards "
        "(objects): the object and its new objects, or the new object.",
        with_targets(with_targets(object_schema({}), kObjects, true), kInstances, false),
        json::array({action("obj_instances_to_objects", L("Set as Individual Objects"), object_section, "", json::object(),
                            "Make each instance of the selected object a separate object (object_select {objects: [n]}); with some "
                            "of its instances selected, only those. New objects are added last; the reply lists the objects that "
                            "hold those instances (objects).")}),
        separate_command);
    catalog.add(reaching({"separate_instances"}, with_expected_ids(std::move(separate))));

    // The answer to Split's Auto-Drop question, which every call but the actions' gives where the split asks it.
    NativeCommand split = menu_command(
        "object_split", L("Split an object"),
        "Split an object into separate objects, or into parts of it, as Split > To Objects and To Parts do: the object named, "
        "else the one row selected in the object list, as Split reads it: an object's row, into objects also a row of the "
        "object's own other than a part's, and into parts a part's row, or another row of the object. A mesh of one shell "
        "is refused, and into parts so is an object of several parts, whose part is selected to split it. Into objects, "
        "the object is removed and its pieces are added last and selected. Where some of them "
        "would float above the bed of an object that drops, `keep_z` answers the question Split asks (true keeps their "
        "height, false lets them drop), and without it the call is refused (`auto_drop_question`); with only an action's "
        "arguments Split asks it (`blocked_by_dialog`). One undo step. Reply: the new objects (objects) or the parts the "
        "part became (parts), with their indices afterwards, and `object_count`.",
        with_targets(object_schema({{"into", enum_prop({"objects", "parts"}, "Separate objects, or parts of the object.")},
                                    {"keep_z", prop("boolean", "Into objects, the answer to the question whether pieces that would "
                                                               "float above the bed keep their height: `true` keeps it, `false` "
                                                               "lets them drop.")}},
                                   {"into"}),
                     kObjects, true),
        json::array({action("obj_split_objects", L("Split to Objects"), object_section, "menu_split_objects", {{"into", "objects"}},
                            "Split the selected object into separate objects."),
                     action("obj_split_parts", L("Split to Parts"), object_section, "menu_split_parts", {{"into", "parts"}},
                            "Split the selected object into parts.")}),
        split_command);
    split.input_schema = with_silent(std::move(split.input_schema), person_forms(split));
    catalog.add(reaching({"split_object"}, with_expected_ids(std::move(split))));

    catalog.add(reaching(
        {"assemble_objects"},
        with_expected_ids(menu_command(
            "object_assemble", L("Assemble"),
            "Assemble two or more objects into one object of parts named Assembly, as Assemble does: those named, else the "
            "object and instance rows selected in the object list, an instance becoming an object of its own first. Cut objects "
            "are refused. They are removed and the new object is added last and selected, so object indices change. One "
            "undo step. Reply: the new object's index (object) and name, and objects.",
            with_targets(object_schema({}), kObjects, false),
            json::array({action("obj_assemble", L("Assemble"), object_section, "", json::object(),
                                "Assemble two or more selected objects into one object of parts named Assembly. They are removed "
                                "and it is added last, so object indices change; the reply has its index (object) and name.")}),
            assemble_command))));

    // The plate menu's Order By.
    struct OrderMenuItem
    {
        const char* key;
        const char* title;
        const char* by;
        const char* what;
    };
    // Titles are L("...")-marked here so xgettext extracts them.
    static const OrderMenuItem order_menu_items[] = {
        {"obj_order_by_name", L("Order by Name"), "name", "name"},
        {"obj_order_by_filament", L("Order by Filament"), "filament", "filament"},
        {"obj_order_by_height", L("Order by Height"), "height", "height"},
        {"obj_order_by_size", L("Order by Size"), "size", "size (bounding box volume)"},
    };
    json order_actions = json::array();
    for (const OrderMenuItem& item : order_menu_items)
        order_actions.push_back(action(item.key, item.title, object_section, "", {{"by", item.by}},
                                       std::string("Order the current plate's objects by ") + item.what +
                                           ", which sets the object list and print order, as the plate menu's Order By does. "
                                           "Choosing the order they already have reverses it. Object indices change; the reply "
                                           "lists the new order (order: [{object, name, plate}]) and descending."));
    NativeCommand order = reaching(
        {"order_objects", "reorder_objects"},
        menu_command("object_order", L("Order objects"),
                    "Order the objects of a plate (the current one, or plate), or with all of every plate, by name, filament, "
                    "height or size (bounding box volume), which sets the object list and print order, as the plate menu's "
                    "Order By does: ascending, or with descending true descending. Called with by alone, as its Speed Dial "
                    "actions, it shows the Prepare view and, as Order By does, reverses the order the objects already have. "
                    "With order in place of by, the plate's objects go in the order given, as dragging them in the object list "
                    "puts them: order names each of the plate's objects once, by index, and anything else is refused; the "
                    "other plates' objects keep their order. One undo step, none when the order stays as it was; object indices "
                    "change. Reply: the new order (order: "
                    "[`{object, name, plate}`]), and descending with by.",
                    object_schema({{"plate", {{"type", "integer"}, {"minimum", 0}, {"description", "Order this plate's objects. Default: the current plate."}}},
                                   {"by", enum_prop({"name", "filament", "height", "size"}, "What to order by.")},
                                   {"order", {{"type", "array"},
                                              {"minItems", 1},
                                              {"items", {{"type", "integer"}, {"minimum", 0}}},
                                              {"description", "In place of by: the plate's objects, by index (`object_list`), each "
                                                              "once, in the order wanted."}}},
                                   {"descending", prop("boolean", "With by: descending order. Default false.")},
                                   {"all", prop("boolean", "With by: order the objects of every plate. Default false: one plate's.")}}),
                    std::move(order_actions), order_command));
    // The arguments are checked before the call reads the app.
    order.run = [run = std::move(order.run)](const json& args, ToolContext& ctx) {
        check_order_arguments(args);
        return run(args, ctx);
    };
    catalog.add(std::move(order));

    catalog.add(reaching(
        {"drop_objects"},
        with_expected_ids(menu_command(
            "object_drop", L("Drop to Bed"),
            "Drop objects or parts onto the bed, as Drop does: those named, each instance and each part that does not rest on "
            "the bed dropped on its own, else what is selected in the Prepare view, dropped as one by its lowest point. What "
            "already rests on the bed is no error: moved false says that nothing moved. One undo step; the targets are left "
            "selected. Reply: what it acted on (objects, instances, parts) and moved.",
            with_targets(object_schema({}), kObjects | kParts, false),
            json::array({action("obj_drop", L("Drop to Bed"), object_section, "toolbar_flatten", json::object(),
                                "Drop the selection onto the bed.")}),
            drop_command))));

    catalog.add(reaching(
        {"mirror_objects"},
        with_expected_ids(menu_command(
            "object_mirror", L("Mirror objects"),
            "Mirror objects or parts along an axis, as Mirror > Along X Axis, Y Axis and Z Axis do: those named, each instance "
            "of an object and the parts of each object in turn, else what is selected in the Prepare view, which Mirror takes "
            "of one instance or parts of one. Cut objects are refused. One undo step; the targets are left selected. Reply: what "
            "it mirrored (objects, instances, parts).",
            with_targets(object_schema({{"axis", enum_prop({"x", "y", "z"}, "The axis to mirror along.")}}, {"axis"}), kObjects | kParts, false),
            json::array({action("obj_mirror_x", L("Mirror X"), object_section, "menu_mirror_x", {{"axis", "x"}},
                                "Mirror the selection along X."),
                         action("obj_mirror_y", L("Mirror Y"), object_section, "menu_mirror_y", {{"axis", "y"}},
                                "Mirror the selection along Y."),
                         action("obj_mirror_z", L("Mirror Z"), object_section, "menu_mirror_z", {{"axis", "z"}},
                                "Mirror the selection along Z.")}),
            mirror_command))));

    catalog.add(reaching(
        {"center_objects"},
        with_expected_ids(menu_command(
            "object_center", L("Center Selected on Plate"),
            "Centre objects on their plate, as Center does: those named, each plate's as one group on the plate it stands on (an "
            "object on no plate goes with the current plate's), or all on plate; with none named, what is selected in the "
            "Prepare view as one group, on plate, or with no arguments, as its Speed Dial action, on the current plate. A "
            "group moves as one, its box to the plate's centre, its objects keeping their places relative to each other, so "
            "a group spread over several plates and sent to plate may not fit on its bed. One undo step; the targets are "
            "left selected. Reply: what it centred (objects, instances, parts) and the groups (plates: [`{plate, objects}`]).",
            with_targets(object_schema({{"plate", {{"type", "integer"}, {"minimum", 0}, {"description", "Centre them all on this plate."}}}}),
                         kObjects, false),
            json::array({action("obj_center", L("Center Selected on Plate"), object_section, "", json::object(),
                                "Center the selection on its plate.")}),
            center_command))));

    catalog.add(reaching(
        {"scale_to_fit"},
        with_expected_ids(menu_command(
            "object_scale_to_fit", L("Scale to Fit Print Volume"),
            "Scale objects as one to fit the current plate's print volume and move them into it, as Scale to build volume does: "
            "those named, else the objects and instances selected in the Prepare view. Cut objects and a bed of another shape "
            "than a rectangle or a circle are refused. One undo step; the targets are left selected. Reply: what it scaled "
            "(objects, instances) and each instance's scale (scales: [`{object, instance, scale}`]).",
            with_targets(object_schema({}), kObjects, false),
            json::array({action("obj_fit_volume", L("Scale to Fit Print Volume"), object_section, "", json::object(),
                                "Scale the selection to fit the print volume.")}),
            scale_to_fit_command))));

    // Auto-Orient waits for its job; a person's pick starts it in place, as the toolbar does.
    const std::string orient_action = "Auto-orient the selection, or every object when nothing is selected; the call returns when it "
                                      "is done.";
    NativeCommand     orient        = reaching(
        {"orient_objects"},
        with_expected_ids(mutate_tool(
            "object_auto_orient", L("Auto-Orient"),
            "Turn objects to the orientation that prints best, as Auto-Orient does: those named and those that stand on plates, "
            "else what is selected in the Prepare view, an instance or a part standing for its object, or every object when "
            "nothing is selected. An object on a locked plate, or one that is not printable, keeps its orientation. A call "
            "that names objects or plates turns no other object: a plate named that holds no objects, a locked plate named, "
            "and objects named none of which can be turned (not printable, or on a locked plate) are refused. A selection of "
            "objects that are not printable, none on a locked plate, leaves the job to turn every other object, as the "
            "toolbar's does. "
            "Waits for the job to end. One undo step; objects named are left selected. Reply: the objects it turned (objects) "
            "and the rotation of each instance it turned (rotations: [`{object, instance, rotation_deg}`]).",
            with_targets(object_schema({{"plates", array_prop({{"type", "integer"}}, "Plate indices: orient the objects that stand on them.")}}),
                         kObjects, false),
            [](const json& args, ToolContext& ctx) {
                AutomationRuntime&     runtime = ctx.runtime();
                Ops::OrientObjectsArgs op;
                ctx.run_ui([&] {
                    Plater&                                    plater = ui_plater();
                    const Ops::Outcome<Ops::OrientObjectsArgs> found  = orient_args(plater, args);
                    if (!found)
                        throw refused(found.refusal());
                    op = found.value();
                    if (const Ops::Outcome<Ops::Done> started = Ops::orient_objects(op); !started)
                        throw refused(started.refusal());
                    return json();
                });
                const auto timeout = std::chrono::seconds(300);
                try {
                    return ToolResult::ok(poll_ui(
                        ctx,
                        [&]() -> json {
                            Plater& plater = ui_plater();
                            return plater.get_ui_job_worker().is_idle() ? orient_reply(plater, op) : json();
                        },
                        timeout, "the orientation"));
                } catch (const ToolFailure&) {
                    if (ctx.cancelled())
                        throw;
                    // McpCore keeps the call's admission until the cancelled job has really stopped.
                    ctx.observe_ui([&] { return json(runtime.core().cancel_worker(ctx.job_id())); });
                    throw ToolFailure("The orientation did not finish within " + std::to_string(timeout.count()) + " s, so it was cancelled");
                }
            })));
    orient.actions = json::array({action("obj_orient", L("Auto-Orient"), object_section, "toolbar_orient", json::object(), orient_action)});
    // A person's pick of the action starts the orientation in place and returns while it runs; an automated call of the
    // action's arguments is the command's own function, which waits and replies. Both are orient_objects() on the same
    // targets.
    catalog.add(NativeCommands::with_menu_item_forms(
        std::move(orient),
        [](const json&) {
            Plater* plater = wxGetApp().plater();
            if (plater == nullptr)
                return AppActionRunResult{};
            const Ops::Outcome<Ops::OrientObjectsArgs> found = orient_args(*plater, json::object());
            if (!found)
                return menu_info(found.refusal());
            if (const Ops::Outcome<Ops::Done> started = Ops::orient_objects(found.value()); !started)
                return menu_info(started.refusal());
            return AppActionRunResult{AppActionRunResult::Level::Succeeded};
        },
        /*person_form_behind_dialogs=*/false, std::vector<json>{json::object()}));

    // ---- Parts ---- (Change Type, in the part menus only)
    struct TypeMenuItem
    {
        const char* key;
        const char* title;
        const char* type;
        const char* description;
    };
    // Titles are L("...")-marked here so xgettext extracts them.
    const TypeMenuItem type_menu_items[] = {
        {"obj_type_part", L("Change Type to Part"), "part",
         "Make the selected parts (object_select {parts} first) solid parts, as Change Type does. Parts are listed by type, so "
         "part indices change."},
        {"obj_type_negative", L("Change Type to Negative Part"), "negative",
         "Make the selected parts negative parts, as Change Type does; not the object's last solid part. Parts are listed by "
         "type, so part indices change."},
        {"obj_type_modifier", L("Change Type to Modifier"), "modifier",
         "Make the selected parts modifiers, as Change Type does; not the object's last solid part. Parts are listed by type, "
         "so part indices change."},
        {"obj_type_support_blocker", L("Change Type to Support Blocker"), "support_blocker",
         "Make the selected parts support blockers, as Change Type does; not the object's last solid part, nor a text or SVG "
         "part. Parts are listed by type, so part indices change."},
        {"obj_type_support_enforcer", L("Change Type to Support Enforcer"), "support_enforcer",
         "Make the selected parts support enforcers, as Change Type does; not the object's last solid part, nor a text or SVG "
         "part. Parts are listed by type, so part indices change."},
        {"obj_type_precise_seam", L("Change Type to Precise Seam"), "precise_seam",
         "Make the selected parts Center precise seams (seams keep their subtype), as Change Type does; not the object's last "
         "solid part, nor a text or SVG part. Parts are listed by type, so part indices change."},
    };
    json type_actions = json::array();
    for (const TypeMenuItem& item : type_menu_items)
        type_actions.push_back(action(item.key, item.title, object_section, "", {{"type", item.type}}, item.description));
    catalog.add(reaching(
        {"change_part_type"},
        with_expected_ids(menu_command(
            "part_change_type", L("Change a part's type"),
            "Change what parts are, as Change Type does: those named, else the part rows selected in the object list. type is "
            "part, negative, modifier, `support_blocker`, `support_enforcer`, or `precise_seam`, a Center precise seam where a seam "
            "keeps its subtype; subtype, with `precise_seam` alone, sets the seam's subtype as the Precise Seam Type menu does. "
            "Not an object's last solid part; a text or SVG part does not become a support blocker or enforcer or a precise "
            "seam; a painted modifier keeps its type. One undo step. Parts are listed by type, so part indices change. Reply: "
            "each part with its index afterwards and its type (parts).",
            with_targets(object_schema({{"type", enum_prop({"part", "negative", "modifier", "support_blocker", "support_enforcer", "precise_seam"},
                                                           "What the parts become.")},
                                        {"subtype", enum_prop({"center", "left", "right", "enforced", "blocked", "neutral"},
                                                              "With type `precise_seam`: the seam's subtype, which every part named "
                                                              "takes. Default: a seam keeps its own, another part becomes center.")}},
                                       {"type"}),
                         kParts, false),
            std::move(type_actions), part_type_command))));
}

// What object_fix replies: what it repaired (objects whole, parts), and for each object or part in turn whether its mesh
// changed, why it could not be repaired, the object's mesh errors before and after, whether painting was removed, and
// whether it is the one a cancel stopped, listed because its mesh had changed by then.
json fix_reply(const Model& model, const Ops::FixedModels& fixed)
{
    Targets repaired;
    json    repairs = json::array();
    for (const Ops::FixedModel& m : fixed.models) {
        if (m.part < 0)
            repaired.objects.push_back(m.object);
        else
            repaired.parts.emplace_back(m.object, m.part);
        json item{{"object", m.object},
                  {"name", m.name},
                  {"changed", m.changed},
                  {"mesh_errors_before", mesh_errors_json(m.before)},
                  {"mesh_errors_after", mesh_errors_json(m.after)},
                  {"painting_removed", m.painting_removed}};
        if (m.part >= 0)
            item["part"] = m.part;
        if (!m.failed.empty())
            item["failed"] = m.failed;
        if (m.cancelled)
            item["cancelled"] = true;
        repairs.push_back(std::move(item));
    }
    json reply       = targets_reply(model, repaired);
    reply["repairs"] = std::move(repairs);
    return reply;
}

// The object and part commands of the GUI's menus that have no Speed Dial action: Printable, Auto Drop, Fix Model,
// Clone, Reload from disk and Replace 3D file. Each reads its targets through resolve_targets() and runs its operation.
void register_object_tools(CommandCatalog& catalog)
{
    NativeCommand printable = reaching(
        {"set_objects_printable"},
        with_expected_ids(mutate_tool(
            "object_set_printable", L("Set printable"),
            "Mark objects, or some of their instances, printable or not, as the object list's Printable cell does: those "
            "named (objects: every instance of each; instances: those alone), else what is selected in the Prepare view. An "
            "instance that is not printable is greyed in the 3D view and left out of slicing. One undo step. Reply: every "
            "instance it set, with printable (instances), and the objects named (objects).",
            with_targets(object_schema({{"printable", prop("boolean", "true marks them printable, false not printable.")}}, {"printable"}),
                         kObjects | kInstances, false),
            [](const json& args, ToolContext& ctx) {
                json out = ctx.run_ui([&] {
                    Plater&                     plater = ui_plater();
                    const Ops::Outcome<Targets> found =
                        resolve_targets(plater.model(), prepare_selection(args), args, kObjects | kInstances, false);
                    if (!found)
                        throw refused(found.refusal());
                    const bool                                           printable = arg_bool(args, "printable", true);
                    const Ops::Outcome<std::vector<std::pair<int, int>>> set = Ops::set_objects_printable({found.value(), printable});
                    if (!set)
                        throw refused(set.refusal());
                    Targets acted;
                    acted.objects   = found.value().objects;
                    acted.instances = set.value();
                    json reply      = targets_reply(plater.model(), acted);
                    for (json& instance : reply["instances"])
                        instance["printable"] = printable;
                    return reply;
                });
                settle(ctx);
                return ToolResult::ok(out);
            })));
    printable.examples = json::parse(R"json([
        {"title": "Leave one instance out of the print", "calls": [
            {"tool": "object_set_instances", "args": {"object": 0, "count": 3}},
            {"tool": "object_set_printable", "args": {"instances": [[0, 1]], "printable": false}}]},
        {"title": "Print the selected objects again", "calls": [
            {"tool": "object_set_printable", "args": {"printable": true}}]}])json");
    catalog.add(std::move(printable));

    NativeCommand auto_drop = reaching(
        {"set_objects_auto_drop"},
        with_expected_ids(mutate_tool(
            "object_set_auto_drop", L("Set Auto Drop"),
            "Turn Auto Drop on or off for objects, as the 3D view's Auto Drop does: those named, else the objects of what is "
            "selected in the Prepare view, an instance or a part standing for its object. With it off an object stays where "
            "`object_transform` or the Move gizmo leaves it, above the bed; with it on it drops onto the bed when it is next "
            "moved. An object's instances share it. One undo step. Reply: the objects (objects) and `auto_drop`.",
            with_targets(object_schema({{"auto_drop", prop("boolean", "true lets the objects drop onto the bed, false lets them "
                                                                      "stay above it.")}},
                                       {"auto_drop"}),
                         kObjects, false),
            [](const json& args, ToolContext& ctx) {
                json out = ctx.run_ui([&] {
                    Plater&                     plater = ui_plater();
                    const Ops::Outcome<Targets> found =
                        resolve_targets(plater.model(), prepare_selection(args), args, kObjects | kObjectOfSelection, false);
                    if (!found)
                        throw refused(found.refusal());
                    const bool                            auto_drop = arg_bool(args, "auto_drop", true);
                    const Ops::Outcome<std::vector<int>> set       = Ops::set_objects_auto_drop({found.value(), auto_drop});
                    if (!set)
                        throw refused(set.refusal());
                    Targets acted;
                    acted.objects      = set.value();
                    json reply         = targets_reply(plater.model(), acted);
                    reply["auto_drop"] = auto_drop;
                    return reply;
                });
                settle(ctx);
                return ToolResult::ok(out);
            })));
    auto_drop.examples = json::parse(R"json([
        {"title": "Let an object stay above the bed where it is moved to", "calls": [
            {"tool": "object_set_auto_drop", "args": {"objects": [0], "auto_drop": false}}]},
        {"title": "Let the selected objects drop onto the bed again", "calls": [
            {"tool": "object_set_auto_drop", "args": {"auto_drop": true}}]}])json");
    catalog.add(std::move(auto_drop));

    // The repair runs in one UI closure, which job_cancel {job} stops through the progress it reports. A cancel fails the
    // call with the reply in its details: the models repaired before it, and the one it stopped when that one's mesh had
    // changed (a part of an object repaired before the cancel keeps its new mesh).
    NativeCommand fix = reaching(
        {"fix_objects"},
        with_expected_ids(mutate_tool(
            "object_fix", L("Fix Model"),
            "Repair objects' meshes, as Fix Model does: those named (objects whole, or parts), the objects on a plate (plate, "
            "as Fix All on the current plate), every object (all), else what is selected in the Prepare view, an instance "
            "standing for its object. A painted modifier is left as it is, and painting is kept only with the `keep_painting` "
            "preference on. It opens no progress dialog and shows no notification; the call waits for the repair, during "
            "which the window does not redraw, and `job_cancel {job}` cancels it: the call fails with the reply in its "
            "details, and what was repaired before the cancel, a part of the object it stopped in among them, keeps its new "
            "mesh. Refused while a gizmo is open. One undo step. Reply: what it repaired (objects, parts) and repairs, for "
            "each in turn: changed, failed (why it could not be repaired), the object's `mesh_errors_before` and "
            "`mesh_errors_after` (as `object_list` gives them), `painting_removed`, and `cancelled: true` on the one a cancel "
            "stopped, listed when its mesh had changed by then.",
            with_targets(object_schema({{"plate", {{"type", "integer"},
                                                   {"minimum", 0},
                                                   {"description", "Repair the objects on this plate."}}},
                                        {"all", prop("boolean", "Repair every object. Default false: the targets.")}}),
                         kObjects | kParts, false),
            [](const json& args, ToolContext& ctx) {
                const bool all = arg_bool(args, "all", false);
                if (all && args.contains("plate"))
                    throw ToolFailure("Give all or plate, not both");
                if ((all || args.contains("plate")) && names_targets(args))
                    throw ToolFailure(all ? "all repairs every object; leave the targets out" :
                                            "plate repairs the objects on it; leave the targets out");
                json out = ctx.run_ui([&] {
                    Plater&             plater = ui_plater();
                    Ops::FixObjectsArgs op;
                    op.all = all;
                    if (args.contains("plate")) {
                        op.plate = arg_int(args, "plate", 0);
                    } else if (!all) {
                        const Ops::Outcome<Targets> found = resolve_targets(plater.model(), prepare_selection(args), args,
                                                                            kObjects | kParts | kObjectOfSelection, false);
                        if (!found)
                            throw refused(found.refusal());
                        op.targets = found.value();
                    }
                    const Ops::Outcome<Ops::FixedModels> fixed = Ops::fix_objects(op, [&ctx](std::size_t, int, const std::string&) {
                        return !ctx.cancelled();
                    });
                    if (!fixed)
                        throw refused(fixed.refusal());
                    json reply = fix_reply(plater.model(), fixed.value());
                    if (fixed.value().cancelled)
                        throw ToolFailure("The repair was cancelled; what it repaired before keeps its new mesh (repairs)", reply);
                    return reply;
                });
                settle(ctx);
                return ToolResult::ok(out);
            })));
    fix.cancellable = true;
    fix.examples = json::parse(R"json([
        {"title": "Repair an imported object whose mesh has holes", "calls": [
            {"tool": "object_list", "args": {}},
            {"tool": "object_fix", "args": {"objects": [0]}}]},
        {"title": "Repair the objects on the first plate", "calls": [
            {"tool": "object_fix", "args": {"plate": 0}}]}])json");
    catalog.add(std::move(fix));

    // Clone waits for the arrangement it starts, as object_arrange does.
    NativeCommand clone = reaching(
        {"clone_objects"},
        with_expected_ids(mutate_tool(
            "object_clone", L("Clone"),
            "Make copies of objects as new objects, as the Clone dialog does: those named, else what is selected in the "
            "Prepare view (an object whole, or some of its instances, which a copy holds as one new object). count copies of "
            "each (default 1, as the dialog's field starts), added after the other objects, placed near them on the current "
            "plate; the last copies are left selected. With arrange true the current plate is then arranged, as the dialog's "
            "Auto arrange plate after cloning does; without arrange, the dialog's choice: the `auto_arrange` preference. The "
            "call waits for the arrangement. One undo step for the copies, and one for the arrangement. Reply: the new objects "
            "(objects), what was copied (copied: its objects, or its instances where instances were selected), `object_count` "
            "and arranged.",
            with_targets(object_schema({{"count", {{"type", "integer"},
                                                   {"minimum", 1},
                                                   {"maximum", 1000},
                                                   {"description", "Copies of each, up to 1000 as the Clone dialog takes. Default 1."}}},
                                        {"arrange", prop("boolean", "Arrange the current plate afterwards. Default: the Clone dialog's "
                                                                    "choice, the `auto_arrange` preference.")}}),
                         kObjects, false),
            [](const json& args, ToolContext& ctx) {
                AutomationRuntime& runtime  = ctx.runtime();
                bool               arranged = false;
                json               reply    = ctx.run_ui([&] {
                    Plater&                     plater = ui_plater();
                    const Ops::Outcome<Targets> found =
                        resolve_targets(plater.model(), prepare_selection(args), args, kObjects | kInstances, false);
                    if (!found)
                        throw refused(found.refusal());
                    Ops::CloneObjectsArgs op;
                    op.targets = found.value();
                    op.count   = arg_int(args, "count", 1);
                    if (args.contains("arrange"))
                        op.arrange = arg_bool(args, "arrange", false);
                    // Read before the copies are added after the other objects, which leaves these indices as they are.
                    json copied = targets_reply(plater.model(), found.value());
                    const Ops::Outcome<Ops::ClonedObjects> cloned = Ops::clone_objects(op);
                    if (!cloned)
                        throw refused(cloned.refusal());
                    arranged            = cloned.value().arranged;
                    json out            = targets_reply(plater.model(), cloned.value().objects);
                    out["copied"]       = std::move(copied);
                    out["object_count"] = plater.model().objects.size();
                    out["arranged"]     = arranged;
                    return out;
                });
                if (arranged) {
                    const auto timeout = std::chrono::seconds(300);
                    try {
                        poll_ui(
                            ctx, []() -> json { return ui_plater().get_ui_job_worker().is_idle() ? json(true) : json(); }, timeout,
                            "the arrangement");
                    } catch (const ToolFailure&) {
                        if (ctx.cancelled())
                            throw;
                        // McpCore keeps the call's admission until the cancelled job has really stopped.
                        ctx.observe_ui([&] { return json(runtime.core().cancel_worker(ctx.job_id())); });
                        throw ToolFailure("The arrangement did not finish within " + std::to_string(timeout.count()) +
                                          " s, so it was cancelled; the copies stay");
                    }
                }
                settle(ctx);
                return ToolResult::ok(reply);
            })));
    clone.examples = json::parse(R"json([
        {"title": "Make three copies of an object and arrange the plate", "calls": [
            {"tool": "object_clone", "args": {"objects": [0], "count": 3, "arrange": true}}]},
        {"title": "Copy the selected objects once, where they are", "calls": [
            {"tool": "object_clone", "args": {"arrange": false}}]}])json");
    catalog.add(std::move(clone));

    NativeCommand reload = reaching(
        {"reload_parts"},
        with_expected_ids(mutate_tool(
            "object_reload_from_disk", L("Reload from disk"),
            "Load parts again from the files they were loaded from, as Reload from disk does: every part of the objects "
            "named, the parts named, every object's (all, as Reload All does, cut objects left out), else what is selected in "
            "the Prepare view. A part's file is the one it was loaded from, else the file of that name beside its object's "
            "file. A part whose file is gone is not asked for: it is listed under missing. A part keeps its settings, type, "
            "filament and placement, and its painting when the `keep_painting` preference is on. Parts loaded from no file "
            "(primitives, the built-in models) are left out; cut objects are refused, since a reload would replace a cut part "
            "with its whole file, and so is a call while a gizmo is open. One undo step, none when every part's file is "
            "missing and nothing is reloaded. Reply: reloaded (parts), missing "
            "(with each part's path), unmatched (parts their file holds no part to take for) and unreadable (a file that "
            "could not be read, after which nothing more was loaded).",
            with_targets(object_schema({{"all", prop("boolean", "Reload every object but the cut ones. Default false: the targets.")}}),
                         kObjects | kParts, false),
            [](const json& args, ToolContext& ctx) {
                const bool all = arg_bool(args, "all", false);
                if (all && names_targets(args))
                    throw ToolFailure("all reloads every object; leave the targets out");
                json out = ctx.run_ui([&] {
                    Plater&                 plater = ui_plater();
                    Ops::ReloadFromDiskArgs op;
                    op.all = all;
                    if (!all) {
                        const Ops::Outcome<Targets> found = resolve_targets(plater.model(), prepare_selection(args), args,
                                                                            kObjects | kParts | kObjectOfSelection, false);
                        if (!found)
                            throw refused(found.refusal());
                        op.targets = found.value();
                    }
                    const Ops::Outcome<Ops::ReloadedParts> reloaded = Ops::reload_parts(op);
                    if (!reloaded)
                        throw refused(reloaded.refusal());
                    const Model& model = plater.model();
                    // Each part with the file it was loaded from.
                    const auto parts = [&model](const std::vector<std::pair<int, int>>& list) {
                        Targets named;
                        named.parts = list;
                        json items  = targets_reply(model, named).value("parts", json::array());
                        for (json& item : items)
                            item["path"] = model.objects[item["object"].get<std::size_t>()]
                                               ->volumes[item["part"].get<std::size_t>()]
                                               ->source.input_file;
                        return items;
                    };
                    json reply{{"reloaded", parts(reloaded.value().reloaded)},
                               {"missing", parts(reloaded.value().missing)},
                               {"unmatched", parts(reloaded.value().unmatched)}};
                    if (!reloaded.value().unreadable.empty())
                        reply["unreadable"] = reloaded.value().unreadable;
                    return reply;
                });
                settle(ctx);
                return ToolResult::ok(out);
            })));
    reload.examples = json::parse(R"json([
        {"title": "Reload an imported object after its file changed", "calls": [
            {"tool": "object_reload_from_disk", "args": {"objects": [0]}}]},
        {"title": "Reload every object", "calls": [
            {"tool": "object_reload_from_disk", "args": {"all": true}}]}])json");
    catalog.add(std::move(reload));

    NativeCommand replace = reaching(
        {"replace_part"},
        with_expected_ids(mutate_tool(
            "part_replace", L("Replace 3D file"),
            "Replace a part's mesh with the mesh of a model file, as Replace 3D file does: the part named (object and part, or "
            "an object of one part), else the one selected in the Prepare view. The part keeps its settings, type, filament "
            "and placement, and its painting when the `keep_painting` preference is on; with it off the painting is copied as it "
            "is, which fits the same mesh alone. A file that loads as more than one object or part is refused, as Replace 3D "
            "file refuses it. path names the file, so no file dialog opens; the loader may still ask, a STEP file for its mesh "
            "options (`blocked_by_dialog`). Cut objects are refused, and so is a call while a gizmo is open. One undo step. "
            "Reply: the part (parts), with its new `part_id` and its index, which moves when the object's parts are not in type "
            "order (the replacement sorts them), and its `object_list` entry (`part_info`).",
            with_targets(object_schema({{"path", prop("string", "Absolute path of the model file (STL, OBJ, 3MF, STEP, ...).")}}, {"path"}),
                         kParts, true),
            [](const json& args, ToolContext& ctx) {
                json out = ctx.run_ui([&] {
                    Plater&                     plater = ui_plater();
                    const Ops::Outcome<Targets> found  = resolve_targets(plater.model(), prepare_selection(args), args,
                                                                         kObjects | kParts | kObjectOfSelection, true);
                    if (!found)
                        throw refused(found.refusal());
                    const Ops::Outcome<std::pair<int, int>> part = Ops::replaced_part(plater.model(), found.value());
                    if (!part)
                        throw refused(part.refusal());
                    const int               object   = part.value().first;
                    const Ops::Outcome<int> replaced = Ops::replace_part({object, part.value().second, arg_string(args, "path")});
                    if (!replaced)
                        throw refused(replaced.refusal());
                    const std::size_t index = std::size_t(replaced.value());
                    Targets           acted;
                    acted.parts        = {{object, replaced.value()}};
                    json reply         = targets_reply(plater.model(), acted);
                    reply["part_info"] = volume_info(*plater.model().objects[std::size_t(object)]->volumes[index], index);
                    return reply;
                });
                settle(ctx);
                return ToolResult::ok(out);
            })));
    replace.examples = json::parse(R"json([
        {"title": "Replace an object's only part with a new version of its file", "calls": [
            {"tool": "part_replace", "args": {"object": 0, "path": "/models/bracket_v2.stl"}}]},
        {"title": "Replace a modifier with a mesh of another shape", "calls": [
            {"tool": "part_replace", "args": {"object": 0, "part": 1, "path": "/models/modifier.stl"}}]}])json");
    catalog.add(std::move(replace));
}

} // namespace

void register_scene_tools(CommandCatalog& catalog)
{
    catalog.add(inspect_tool(
        "object_list", L("List objects"),
        "The project as the Objects panel lists it. `plates`: each `{index, name, objects}`, with the objects that have an "
        "instance on it, each time with that plate's instances; `outside`: those with an instance on no plate. An object has "
        "`index`, `id`, `name`, `filament`, `part_count`, `size` (mm, of its bbox) and `instances`, the indices of its "
        "instances there. With `detail` it also has `auto_drop`, `bbox` (instance 0), `brim_ears` (a count), `overrides` "
        "(the keys of the settings it overrides, which `settings_get` target object reads), `mesh_errors` as the list's "
        "warning icon reads them (`open_edges`, manifold, and the loader's or a repair's `degenerate_facets`, `edges_fixed`, "
        "`facets_removed`, `facets_reversed` and `backwards_edges`; `object_fix` repairs them), `instances` as entries "
        "(`index`, `offset`, `rotation_deg`, `scale`, `mirrored`, `bbox`), and `parts`: parts, negative parts, modifiers, "
        "support blockers and enforcers and precise seams, each with `type` (`subtype` for a precise seam), `filament` (a "
        "part's own or its object's, a modifier's only when it sets one), `position`, `bbox` and `scaled_volume` (mm3) on "
        "the bed for instance 0, `rotation_deg` and `scale` in the object's frame, `mirrored`, `triangles`, `mesh_volume` "
        "(the unscaled mesh's mm3), `painted` facets (supports, seam enforcers/blockers, `fuzzy_skin`, mmu per filament, "
        "texture per layer), and a text part's text and font or an SVG part's file. When it has them: "
        "`layer_config_ranges`, its height range modifiers (`[{min_z, max_z, values}]`, for `object_set_height_ranges` and "
        "`settings_set` target `height_range`), and `layer_height_profile` (`{points, min_height, max_height}`). A bbox is "
        "`{min, max, size}` in mm of model parts only, so unlike the panel's Size it leaves out modifiers. An object's index "
        "numbers it in the project and a part's in its object, whatever the plate; an id stays with its object or part when "
        "a delete, split or assemble shifts the indices, for the guards (`object_id`, `part_id`, `*_ids`). `limit` and "
        "`offset` page the objects and `total` counts them, each once.",
        object_schema({{"plate", {{"type", "integer"}, {"minimum", 0}, {"description", "List this plate alone, without `outside`."}}},
                       {"detail", prop("boolean", "Each object whole: its instances, parts, bounding boxes, mesh errors, paint and "
                                                  "overrides (default false).")},
                       {"screen", prop("boolean", "Add each instance's rectangle in the shown 3D view, logical pixels `[x, y, w, h]`, "
                                                  "to its entry, as with detail.")},
                       {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 500}, {"description", "Objects, default 200."}}},
                       {"offset", {{"type", "integer"}, {"minimum", 0}}}}),
        [](const json& args, ToolContext& ctx) { return ToolResult::ok(ctx.run_ui([&] { return list_objects(args); })); }));

    NativeCommand select = with_expected_ids(mutate_tool(
        "object_select", L("Select objects"),
        "Select objects, instances or parts in the 3D view (the assemble view while it is shown, else Prepare, switching to it), as clicking them would. Give objects, "
        "instances (`[[object, instance], ...]`) or parts (`[[object, part], ...]`); add keeps the current selection; all / "
        "none select everything or nothing, whatever else is given; none also closes the open gizmo, as Deselect All does, "
        "and all is refused while a gizmo that keeps its selection is open. The reply is the selection as "
        "`app_state.selection` gives it, for all and none alone (Select All and Deselect All) too.",
        with_targets(object_schema({{"add", prop("boolean", "Add to the selection instead of replacing it.")},
                                    {"all", prop("boolean", "Select all objects.")},
                                    {"none", prop("boolean", "Clear the selection and close the open gizmo.")}}),
                     kEveryTarget, false),
        [](const json& args, ToolContext& ctx) {
            json out = ctx.run_ui([&] {
                Plater&     plater = ui_plater();
                GLCanvas3D& canvas = object_canvas();
                Selection&  sel    = canvas.get_selection();
                if (arg_bool(args, "none", false)) {
                    clear_selection(canvas);
                    return selection_state(canvas);
                }
                if (arg_bool(args, "all", false)) {
                    if (const std::optional<Ops::Refusal> refusal = select_everything(canvas))
                        throw refused(*refusal);
                    return selection_state(canvas);
                }
                // Every index and expected id is checked before the selection changes. A call that names nothing, or
                // only empty lists, leaves it as it is.
                if (names_targets(args) && !names_empty_lists(args)) {
                    const Ops::Outcome<Targets> named = resolve_targets(plater.model(), nullptr, args, kEveryTarget, false);
                    if (!named)
                        throw refused(named.refusal());
                    bool first = !arg_bool(args, "add", false);
                    for (int o : named.value().objects) {
                        sel.add_object(unsigned(o), first);
                        first = false;
                    }
                    for (const auto& [o, i] : named.value().instances) {
                        sel.add_instance(unsigned(o), unsigned(i), first);
                        first = false;
                    }
                    for (const auto& [o, p] : named.value().parts) {
                        sel.add_volume(unsigned(o), unsigned(p), 0, first);
                        first = false;
                    }
                }
                wxGetApp().obj_list()->update_selections();
                canvas.set_as_dirty();
                return selection_state(canvas);
            });
            settle(ctx);
            return ToolResult::ok(out);
        }));
    select.actions = json::array({NativeCommands::action("select_all", L("Select All Objects"), L("Object"), "", {{"all", true}},
                                                         "Select every object in the 3D view (the assemble view while it is shown, "
                                                         "else Prepare, switching to it), and reply with the selection; refused "
                                                         "while a gizmo that keeps its selection is open. A person's pick shows "
                                                         "Prepare and selects there."),
                                  NativeCommands::action("deselect_all", L("Deselect All"), L("Object"), "", {{"none", true}},
                                                         "Clear the selection in the 3D view (the assemble view while it is shown, "
                                                         "else Prepare, switching to it), close the open gizmo, and reply with the "
                                                         "selection. A person's pick clears it where it is, switching no tab.")});
    // A person's pick runs in place, as its menu item did: Select All shows Prepare and select_everything() selects there,
    // Deselect All is clear_selection() on the view shown, switching no tab. An automated call of either action's
    // arguments is object_select's own function, which the two functions serve too, on the view shown, and replies
    // with the selection.
    catalog.add(NativeCommands::with_menu_item_forms(
        std::move(select),
        [](const json& action_args) {
            Plater* plater = wxGetApp().plater();
            if (arg_bool(action_args, "all", false)) {
                if (plater == nullptr)
                    return AppActionRunResult{};
                Ops::show_prepare_view();
                if (const std::optional<Ops::Refusal> refusal = select_everything(*plater->get_view3D_canvas3D()))
                    return NativeCommands::refused(*refusal, AppActionRunResult::Level::Info);
            } else if (plater != nullptr)
                clear_selection(selection_canvas(*plater));
            return AppActionRunResult{AppActionRunResult::Level::Succeeded};
        },
        /*person_form_behind_dialogs=*/false, std::vector<json>{json{{"none", true}}, json{{"all", true}}}));

    catalog.add(with_expected_ids(mutate_tool(
        "object_transform", L("Move, rotate or scale an instance or a part"),
        "Change one instance's placement, or one part's with part. translate is in mm, position where it goes (mm, as "
        "the Move panel shows it, whatever mode says), rotate in degrees about X/Y/Z, "
        "scale a factor (one number or `[x, y, z]`), size `[x, y, z]` mm along the object's or part's own axes, plate moves "
        "the instance onto that plate at the same place. mode \"relative\" (default) adds to the current placement (a scale "
        "multiplies it), \"absolute\" sets it. An instance is dropped back onto the bed, unless its object's `auto_drop` is "
        "off; `drop_to_bed` false lets it sink into the bed, but an object of one part left above the bed still drops. A part "
        "moves as the "
        "Object manipulation panel moves it: translate and rotate along the bed's axes, measured on instance 0, scale and "
        "size along its own axes; every instance shows the change, and Auto Drop may lower the object afterwards. The "
        "instance is the object's instance (default 0) and the part one of its parts; with no object, the instance or part "
        "selected in the Prepare view, or instance 0 of the object selected. One undo step. Reply: the instance's offset, "
        "scale and plate, or the part's size along its own axes and its `object_list` entry (`part_info`), and what it moved "
        "(instances or parts).",
        with_targets(object_schema({{"translate", array_prop({{"type", "number"}}, "[x, y, z] mm")},
                                    {"position", array_prop({{"type", "number"}}, "`[x, y, z]` mm: the position to move to. Not with translate.")},
                                    {"rotate", array_prop({{"type", "number"}}, "`[x, y, z]` degrees; relative only for a part")},
                                    {"scale", {{"type", {"number", "array"}}, {"description", "Factor or `[x, y, z]` factors."}}},
                                    {"size", array_prop({{"type", {"number", "null"}}}, "`[x, y, z]` mm, null keeps an axis; one number "
                                                                                         "and two nulls keep the proportions. Not with scale.")},
                                    {"plate", {{"type", "integer"}, {"minimum", 0}, {"description", "Move the instance to this plate."}}},
                                    {"mode", enum_prop({"relative", "absolute"}, "Default relative.")},
                                    {"drop_to_bed", prop("boolean", "Default true; instances only.")}}),
                     kEveryTarget, true),
        [](const json& args, ToolContext& ctx) {
            json out = ctx.run_ui([&]() -> json {
                Plater& plater = ui_plater();
                if (args.contains("scale") && args.contains("size"))
                    throw ToolFailure("give scale or size, not both");
                if (args.contains("translate") && args.contains("position"))
                    throw ToolFailure("give translate or position, not both");
                const Ops::Outcome<Targets> found = resolve_targets(plater.model(), &plater.get_view3D_canvas3D()->get_selection(), args,
                                                                    kEveryTarget, true);
                if (!found)
                    throw refused(found.refusal());
                // The call as the transforms read it: the object, and the instance (a whole object's is instance 0) or
                // the part.
                const Targets& target = found.value();
                json           call   = args;
                Targets        moved;
                if (!target.parts.empty()) {
                    call["object"] = target.parts.front().first;
                    call["part"]   = target.parts.front().second;
                    moved.parts    = target.parts;
                } else {
                    const std::pair<int, int> instance = target.instances.empty() ?
                                                             std::make_pair(target.objects.front(), arg_int(args, "instance", 0)) :
                                                             target.instances.front();
                    call["object"]   = instance.first;
                    call["instance"] = instance.second;
                    moved.instances  = {instance};
                }
                const int oi    = call.at("object").get<int>();
                json      reply = call.contains("part") ? transform_part(plater, oi, call) : transform_instance(plater, oi, call);
                reply.update(targets_reply(plater.model(), moved));
                return reply;
            });
            settle(ctx);
            return ToolResult::ok(out);
        })));

    catalog.add(with_expected_ids(mutate_tool(
        "part_add", L("Add a part or modifier to an object"),
        "Add a part, negative part, modifier, support blocker or enforcer, or precise seam to an object, as its context "
        "menu's Add items do: a primitive shape at a size, or a model file as Load... adds it. The part lands where the "
        "menu puts it (a shape beside the object, a file at its coordinates relative to the object's first part) unless "
        "position is given. Like the menu, adding a shape folds the instance's rotation and scale into the object's parts "
        "(the instance's scale then reads 1). Refused for an object with several instances: add parts first. A STEP file "
        "may ask for mesh options (`blocked_by_dialog`). The object is the one named, else the one selected in the Prepare "
        "view, or the object of the selected instance or part. The new part is selected; one undo step. Reply: its index, "
        "which is not always the last (parts are ordered by type), the object's part count and the part's `object_list` entry "
        "(`part_info`), and the object (objects).",
        with_targets(object_schema({{"type", enum_prop({"part", "negative", "modifier", "support_blocker", "support_enforcer", "precise_seam"},
                                                       "What the new part is.")},
                                    {"shape", enum_prop({"cube", "cylinder", "sphere", "cone", "disc", "torus"}, "A primitive shape.")},
                                    {"size", array_prop({{"type", "number"}}, "mm, the Primitive size dialog's fields: cube `[x, y, z]`; "
                                                                              "cylinder, cone, disc `[diameter, height]`; sphere "
                                                                              "`[diameter]`; torus `[inner diameter, tube diameter]`. "
                                                                              "Default: the built-in size.")},
                                    {"path", prop("string", "Instead of shape: absolute path of a model file (STL, OBJ, 3MF, STEP, ...).")},
                                    {"position", array_prop({{"type", "number"}}, "`[x, y, z]` mm on the bed for the part's centre, on "
                                                                                  "instance 0, as the Move panel shows it.")}},
                                   {"type"}),
                     kObjects, true),
        [](const json& args, ToolContext& ctx) {
            json out = ctx.run_ui([&] {
                Plater&                     plater = ui_plater();
                const Ops::Outcome<Targets> found  = resolve_targets(plater.model(), &plater.get_view3D_canvas3D()->get_selection(), args,
                                                                     kObjects | kObjectOfSelection, true);
                if (!found)
                    throw refused(found.refusal());
                json reply = add_part(plater, found.value().objects.front(), args);
                reply.update(targets_reply(plater.model(), found.value()));
                return reply;
            });
            settle(ctx);
            return ToolResult::ok(out);
        })));

    register_object_menu_commands(catalog);
    register_object_tools(catalog);

    NativeCommand arrange = reaching({"start_arrange"}, mutate_tool(
        "object_arrange", L("Arrange objects or fill the bed"),
        "Arrange the objects of every unlocked plate as the toolbar's Arrange does, or of one plate (plates: an index or "
        "\"current\") as its Arrange icon does. With `fill_object`, fill that object's plate with more of it instead, as Fill "
        "bed with instances (or copies, by `fill_with`) does (reply: added, how many). spacing, rotate, `align_y` and `multi_material` are the Arrange options "
        "panel's settings: the values given stay in the panel, so later arranges use them too. Waits for the arrangement "
        "(a longer one replies with a job). Reply: the settings in force, instances left off every plate, plates the "
        "all-plates arrange skipped and the job's notifications. An arrange may reorder objects, so `object_list` again. "
        "One undo step. Called with no arguments it is Auto-Arrange, the toolbar's Arrange with the panel's settings, and "
        "waits and replies as any other call.",
        object_schema({{"plates", {{"type", {"string", "integer"}}, {"description", "\"all\" (default), \"current\" or a plate index."}}},
                       {"spacing", {{"type", "number"}, {"minimum", 0}, {"description", "mm between objects; 0 means auto."}}},
                       {"rotate", prop("boolean", "Auto rotate for arrangement.")},
                       {"align_y", prop("boolean", "Align to Y axis (off while rotate is on).")},
                       {"multi_material", prop("boolean", "Allow multiple materials on same plate.")},
                       {"fill_object", {{"type", "integer"}, {"minimum", 0}, {"description", "An object index: instead of arranging, fill that object's plate with more of it."}}},
                       {"fill_with", enum_prop({"instances", "copies"}, "Default instances.")},
                       {"timeout_s", {{"type", "integer"}, {"minimum", 10}, {"maximum", 1800}, {"description", "Cancel it after this long (default 300)."}}}}),
        [](const json& args, ToolContext& ctx) {
            AutomationRuntime& runtime = ctx.runtime();
            ArrangeStart start;
            ctx.run_ui([&] {
                start = start_arrange(ui_plater(), args);
                return json();
            });
            // A replay arranges with the settings in force here, whatever the replaying machine saved.
            json recorded = args;
            for (const auto& [key, value] : start.settings.items())
                if (key != "by_object")
                    recorded[key] = value;
            ctx.set_journal_args(std::move(recorded));
            const auto timeout = std::chrono::seconds(arg_int(args, "timeout_s", 300));
            try {
                return ToolResult::ok(poll_ui(
                    ctx,
                    [&]() -> json {
                        Plater& plater = ui_plater();
                        return plater.get_ui_job_worker().is_idle() ? arrange_reply(plater, start) : json();
                    },
                    timeout, "the arrangement"));
            } catch (const ToolFailure&) {
                if (ctx.cancelled())
                    throw;
                // McpCore keeps the call's admission until the cancelled job has really stopped.
                ctx.observe_ui([&] { return json(runtime.core().cancel_worker(ctx.job_id())); });
                throw ToolFailure("The arrangement did not finish within " + std::to_string(timeout.count()) + " s, so it was cancelled");
            }
        }));
    arrange.actions = json::array({NativeCommands::action("obj_arrange", L("Auto-Arrange"), L("Object"), "toolbar_arrange", json::object(),
                                                          "Arrange all objects on the unlocked plates with the saved Arrange options, as "
                                                          "the toolbar does; the call waits for the arrangement and replies as "
                                                          "object_arrange does: the settings in force, the instances left off every "
                                                          "plate and the plates skipped. object_arrange with arguments also takes "
                                                          "options, one plate or fill bed.")});
    // A person's pick starts the arrangement in place as the toolbar does, and returns while it runs; an automated call
    // of the action's arguments is object_arrange's own function, which waits and replies. Both are Ops::start_arrange()
    // on every unlocked plate with the panel's settings.
    catalog.add(NativeCommands::with_menu_item_forms(
        std::move(arrange), [](const json&) { return MenuItems::obj_arrange(); }, /*person_form_behind_dialogs=*/false,
        std::vector<json>{json::object()}));

    NativeCommand slice = reaching({"start_slice"}, mutate_tool(
        "plate_slice", L("Slice"),
        "Slice the current plate, all plates, or one plate by index, the way the toolbar's Slice button does, and wait for "
        "the result (or reply with a job id when it takes longer than the reply budget). Each requested plate reports ok, "
        "error, cancelled, empty or skipped; the whole operation is superseded when someone else started a slice or "
        "edited the project while it ran. An ok plate has `time_s` (`silent_time_s` when the printer estimates it), "
        "`filament_m` and `filament_g` including flush, `filament_cost`, layers, `max_z`, `filament_changes`, and filaments[i] for "
        "slot i+1 with m, g and the grams of `model_g`, `support_g`, `flush_g` and `tower_g`. Other changes are refused until it "
        "finishes; `job_cancel` stops it. The Preview tab shows the result, as after the Slice button; `preview: true`, alone "
        "or with plates \"all\", is the call of Slice and Preview or Slice All Plates, whose pick starts the slice as a job "
        "and shows its result as a notification.",
        object_schema({{"plates", {{"type", {"string", "integer"}}, {"description", "\"current\" (default), \"all\", or a plate index."}}},
                       {"timeout_s", {{"type", "integer"}, {"minimum", 10}, {"maximum", 7200}, {"description", "Stop waiting after this long (default 1800): the call fails, and the slice goes on unless `job_cancel` stops it."}}},
                       {"preview", prop("boolean", "Show the Preview tab, as every slice does: the argument of the Slice and "
                                                   "Preview and Slice All Plates actions.")}}),
        [](const json& args, ToolContext& ctx) {
            const json plates_arg = args.value("plates", json("current"));
            if (plates_arg.is_string() && plates_arg != "all" && plates_arg != "current")
                throw ToolFailure("plates is \"all\", \"current\" or a plate index");
            auto       ticket     = std::make_shared<SliceTicket>();
            // Held from before the wait below, so no call admitted meanwhile can change the project
            // under the slice, and a cancel meanwhile finds the operation.
            const uint64_t cancels_before = ctx.run_ui([&] {
                ctx.begin_operation();
                return json(ui_plater().slice_cancel_seq());
            }).get<uint64_t>();
            // A change made just before (settings_set, project_open) is validated by an update
            // scheduled a moment later; a slice requested first would be refused on the old state.
            poll_ui(
                ctx,
                [cancels_before]() -> json {
                    const Plater& plater = ui_plater();
                    const bool    pending = plater.is_background_process_update_scheduled();
                    return pending && plater.slice_cancel_seq() == cancels_before ? json() : json(true);
                },
                std::chrono::seconds(10), "the pending update of the slicing state");
            const bool cancelled = ctx.run_ui([&] {
                Plater&        plater = ui_plater();
                Ops::SliceArgs slice;
                slice.all_plates = plates_arg.is_string() && plates_arg.get<std::string>() == "all";
                if (plates_arg.is_number_integer())
                    slice.plate = plates_arg.get<int>();
                const Ops::Outcome<std::vector<int>> plates = Ops::slice_plates(slice);
                if (!plates)
                    throw refused(plates.refusal());
                ticket->plates = plates.value();
                // A cancel that came while the update was pending stops the slice before it starts.
                if (plater.slice_cancel_seq() != cancels_before)
                    return json(true);
                ticket->state.started = std::chrono::steady_clock::now();
                const Ops::Outcome<Ops::SliceTicket> started = Ops::start_slice(slice);
                if (!started)
                    throw refused(started.refusal());
                ticket->state.run_seq_before    = started.value().run_seq_before;
                ticket->state.revision_at_start = started.value().revision;
                ticket->state.request_seq_mine  = started.value().request_seq;
                ticket->state.seen_running      = started.value().slicing;
                ticket->state.adopted_run       = started.value().adopted_run;
                return json(false);
            }).get<bool>();
            if (cancelled) {
                json plates = json::array();
                for (int index : ticket->plates)
                    plates.push_back({{"index", index}, {"outcome", to_string(PlateOutcome::Cancelled)}});
                return ToolResult::ok({{"status", "done"}, {"plates", std::move(plates)}});
            }
            const auto timeout = std::chrono::seconds(arg_int(args, "timeout_s", 1800));
            json result = poll_ui(
                ctx,
                [ticket]() -> json {
                    Plater&          plater = ui_plater();
                    SliceObservation obs;
                    obs.slicing_any      = plater.is_slicing_any();
                    obs.request_seq      = plater.slice_request_seq();
                    obs.interrupted      = plater.slice_request_interrupted(ticket->state.request_seq_mine);
                    obs.current_revision = plater.project_revision();
                    const uint64_t first = ticket->state.adopted_run != 0 ? ticket->state.adopted_run : ticket->state.run_seq_before + 1;
                    for (uint64_t seq = first; seq <= plater.slice_run_seq(); ++seq)
                        if (auto run = plater.slice_run(seq))
                            obs.runs_since_start.push_back({run->seq, run->finished, run->cancelled, run->ok, run->error,
                                                            run->revision_at_finish, run->request_seq_at_finish,
                                                            run->request_seq_at_start, run->for_request, run->plate});
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
                        if (results[i].outcome == PlateOutcome::Ok) {
                            // The statistics are the plate's current result, so only while the
                            // project is as the operation sliced it and nobody sliced the plate since.
                            // They are read in this closure, which decides the outcome, since a later UI
                            // call would wait behind the preview's loading of the new result; the
                            // serializer's one pass over the moves is short next to that.
                            PartPlate*                  plate = list.get_plate(ticket->plates[i]);
                            const GCodeProcessorResult* gcode = plate != nullptr ? plate->get_slice_result() : nullptr;
                            if (gcode != nullptr && plate->is_slice_result_valid() &&
                                plater.project_revision() == ticket->state.revision_at_start &&
                                plate_result_is_own(ticket->plates[i], ticket->state, obs))
                                p.update(plate_summary(json::parse(gcode_statistics_json(*gcode)), *gcode));
                            else
                                p["note"] = "The plate changed after it was sliced; slice again for its statistics.";
                        }
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
        }));
    slice.budget_ms = 45000;
    slice.actions   = json::array(
        {NativeCommands::action("slice_and_preview", L("Slice and Preview"), L("Slice & Export"), "media_play", {{"preview", true}},
                                "Slice the current plate and show the Preview tab; the call waits for the result, and a pick starts "
                                "the slice as a job."),
         NativeCommands::action("slice_all", L("Slice All Plates"), L("Slice & Export"), "media_play",
                                {{"plates", "all"}, {"preview", true}},
                                "Slice every plate and show the Preview tab; the call waits for the results, and a pick starts the "
                                "slice as a job.")});
    catalog.add(std::move(slice));

    NativeCommand cancel = mutate_tool(
        "job_cancel", L("Cancel a job"),
        "Stop the running operation: a slice or G-code export, as the slicing progress's cancel button does, or a "
        "background job an MCP call started (an arrange, orient or fill bed). Its job then reports cancelled; a background "
        "job keeps other changes out until it has really stopped. With job, only that job is stopped: the running "
        "operation, an `object_fix`, a `script_run` run or a `recording_encode`. Allowed while an operation holds the project. Reply: "
        "`was_running`, and job when one was stopped.",
        object_schema({{"job", prop("string", "The operation's job to cancel, e.g. \"j12\".")}}),
        [](const json& args, ToolContext& ctx) {
            AutomationRuntime& runtime = ctx.runtime();
            const auto ops = runtime.jobs().running_operations();
            // A plugin stops only what its own calls started, so it names the job.
            if (!ctx.caller_plugin().empty() && !args.contains("job"))
                throw ToolFailure("A plugin cancels only the jobs of its own calls: give the job");
            if (args.contains("job"))
                require_own_job(ctx, runtime.jobs(), arg_string(args, "job"), "cancels");
            if (args.contains("job")) {
                const std::string job = arg_string(args, "job");
                if (std::find(ops.begin(), ops.end(), job) == ops.end()) {
                    // A call that is no operation but can be stopped: its handler watches for the stop.
                    const NativeCommand* def = NativeCommands::instance().find(runtime.jobs().running_tool(job));
                    if (def == nullptr || !def->cancellable)
                        throw ToolFailure("Job " + job + " is not a running operation", {{"running", ops}});
                    runtime.jobs().request_stop(job);
                    return ToolResult::ok({{"was_running", true}, {"job", job}});
                }
            }
            json out = ctx.run_ui([&]() -> json {
                // Only a job an MCP call started: a person's (Text typing) is theirs to stop.
                for (const std::string& op : ops)
                    if ((!args.contains("job") || op == arg_string(args, "job")) && runtime.core().cancel_worker(op))
                        return {{"was_running", true}, {"job", op}};
                Plater& plater = ui_plater();
                // A slice operation still waiting to request its slice counts: the cancel keeps it from starting.
                const bool running = plater.is_slicing_any() || !ops.empty();
                plater.cancel_slicing();
                return json{{"was_running", running}};
            });
            return ToolResult::ok(out);
        });
    cancel.allowed_during_operation = true;
    // Recovery: it must get through a dialog the operation opened and another call's nested loop.
    cancel.guard = GuardClass::Recover;
    catalog.add(cancel);
}

}}} // namespace Slic3r::GUI::Mcp
