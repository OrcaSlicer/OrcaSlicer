// list_objects, select, transform, add_part, arrange, slice and cancel: the model on the plates, the
// arrange and Fill bed jobs, and the slice operation, which reports its own outcome per plate (see
// McpOperations).

#include "CanvasTarget.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "JobRegistry.hpp"
#include "McpCore.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Point.hpp"
#include "McpOperations.hpp"
#include "McpPaths.hpp"
#include "McpService.hpp"
#include "McpTools.hpp"
#include "McpTypes.hpp"
#include "UiInspector.hpp"
#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_Geometry.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosManager.hpp"
#include "slic3r/GUI/Jobs/Job.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
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
#include <wx/string.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <optional>
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

// The 3D canvas object operations act on: the assemble view while it is shown, else the Prepare
// view, switched to when another is shown.
GLCanvas3D& object_canvas()
{
    Plater& plater = ui_plater();
    if (GLCanvas3D* assemble = plater.get_assmeble_canvas3D(); assemble != nullptr && plater.get_current_canvas3D() == assemble)
        return *assemble;
    if (!plater.is_view3D_shown()) {
        plater.select_view_3D("3D");
        if (MainFrame* mf = wxGetApp().mainframe)
            mf->select_tab(TAB_ID_PREPARE);
    }
    return *plater.canvas3D();
}

} // namespace

// Refused in the assemble view, which shows another canvas, and while a gizmo owns the selection.
GLCanvas3D& require_prepare_scene(Plater& plater)
{
    if (GLCanvas3D* assemble = plater.get_assmeble_canvas3D(); assemble != nullptr && plater.get_current_canvas3D() == assemble)
        throw ToolFailure("Leave the assemble view first (set_view {view: \"3d\"})");
    GLCanvas3D& canvas = object_canvas();
    if (const auto type = canvas.get_gizmos_manager().get_current_type(); type != GLGizmosManager::Undefined)
        throw ToolFailure("Close the " + gizmo_name(type) + " gizmo first (ui_click {gizmo: \"" + gizmo_name(type) + "\"})");
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
        throw ToolFailure("scale factors must be positive");
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

// An instance as list_objects describes it, except its plate and canvas rectangle, which the GUI knows.
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
    Plater&          plater = ui_plater();
    PartPlateList&   plates = plater.get_partplate_list();
    const ModelObjectPtrs& objects = plater.model().objects;
    const int        only_plate = arg_int(args, "plate", -1);
    // Screen rectangles are of the 3D view shown, Prepare's or the assemble view's.
    GLCanvas3D* canvas = nullptr;
    if (arg_bool(args, "screen", false)) {
        canvas = plater.get_current_canvas3D();
        if (canvas == nullptr || canvas == plater.get_preview_canvas3D())
            throw ToolFailure("screen needs the Prepare view or the assemble view shown (set_view)");
    }
    json             items = json::array();
    for (size_t i = 0; i < objects.size(); ++i) {
        const ModelObject* o = objects[i];
        std::vector<int>   on(o->instances.size());
        bool               on_plate = only_plate < 0;
        for (size_t j = 0; j < o->instances.size(); ++j) {
            on[j]    = plates.find_instance(int(i), int(j));
            on_plate = on_plate || on[j] == only_plate;
        }
        if (!on_plate)
            continue;
        json item = object_info(*o, i);
        for (size_t j = 0; j < o->instances.size(); ++j) {
            json& inst    = item["instances"][j];
            inst["plate"] = on[j];
            if (canvas != nullptr)
                inst["screen"] = screen_rect(*canvas, int(i), int(j));
        }
        items.push_back(std::move(item));
    }
    json out    = page(items, args, 200);
    out["plates"] = plates.get_plate_count();
    return out;
}

json selection_summary(const Selection& sel)
{
    json out{{"empty", sel.is_empty()}};
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

struct SliceTicket
{
    SliceTicketState state;
    std::vector<int> plates;
};

// transform on an instance: written straight into the ModelInstance, then dropped, registered with
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
            throw ToolFailure("plate " + std::to_string(to) + " does not exist; run_action plate_add makes one");
        const int from = plates.find_instance(oi, ii);
        if (from < 0)
            throw ToolFailure("instance " + std::to_string(ii) + " is off every plate; place it with translate (absolute) first");
        to_plate = plates.get_plate(to)->get_origin() - plates.get_plate(from)->get_origin();
    }

    plater.take_snapshot("Transform object");
    if (translate)
        inst->set_offset(absolute ? *translate : Vec3d(inst->get_offset() + *translate));
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
    // The bounding box is cached: ensure_on_bed() must measure the new placement.
    object->invalidate_bounding_box();
    if (arg_bool(args, "drop_to_bed", true))
        object->ensure_on_bed();
    // Slicing and the object list read the plate an instance is on from the plate registry, which
    // GLCanvas3D::do_move() updates for a person's moves.
    plates.notify_instance_update(oi, ii);
    wxGetApp().obj_list()->update_plate_values_for_items();
    plater.changed_object(oi);
    return {{"object", oi},
            {"instance", ii},
            {"offset", vec3(inst->get_offset())},
            {"scale", vec3(inst->get_scaling_factor())},
            {"plate", plates.find_instance(oi, ii)}};
}

// transform on a part: the Object manipulation panel's path with the part selected, so do_move(),
// do_rotate() and do_scale() write the part, drop the object (Auto Drop), register its instances
// with their plates and update the object list. Translate and rotate are along the bed's axes on
// instance 0, scale and size along the part's own axes.
json transform_part(Plater& plater, int oi, const json& args)
{
    ModelObject* object = plater.model().objects[size_t(oi)];
    const int    vi     = arg_int(args, "volume", 0);
    if (vi >= int(object->volumes.size()))
        throw ToolFailure("volume index out of range");
    // A single part has no row of its own in the object list, so the panel can't select it.
    if (object->volumes.size() == 1)
        throw ToolFailure("object " + std::to_string(oi) + " has one part; transform the instance");
    if (arg_int(args, "instance", 0) != 0)
        throw ToolFailure("a part belongs to its object and moves on every instance; leave instance out");
    if (args.contains("plate"))
        throw ToolFailure("plate moves an instance; leave volume out");
    const bool absolute = arg_string(args, "mode", "relative") == "absolute";
    if (absolute && args.contains("rotate"))
        throw ToolFailure("rotate a part relative to its current rotation (mode \"relative\")");
    ModelVolume* volume = object->volumes[size_t(vi)];
    GLCanvas3D&  canvas = require_prepare_scene(plater);

    std::optional<Vec3d> translate, rotate, scale;
    if (args.contains("translate")) {
        const Vec3d t = to_vec3(args["translate"], "translate");
        translate     = absolute ? Vec3d(t - part_position(*object, *volume)) : t;
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
    return {{"object", oi}, {"volume", vi}, {"size", vec3(part_size(*volume))}, {"volume_info", volume_info(*volume, size_t(vi))}};
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

// Validates arrange's arguments, applies its settings and starts the job the toolbar's Arrange, a
// plate's Arrange icon or Fill bed starts. UI thread.
ArrangeStart start_arrange(Plater& plater, const json& args)
{
    GLCanvas3D&            canvas  = require_prepare_scene(plater);
    PartPlateList&         plates  = plater.get_partplate_list();
    const ModelObjectPtrs& objects = plater.model().objects;
    ArrangeStart           start;
    const bool             fill = args.contains("fill");
    if (fill && args.contains("plates"))
        throw ToolFailure("fill fills the object's own plate; leave plates out");
    if (!fill && args.contains("fill_with"))
        throw ToolFailure("fill_with goes with fill");
    if (objects.empty())
        throw ToolFailure("There are no objects to arrange");
    int plate = -1; // every unlocked plate
    if (fill) {
        start.fill_object = arg_int(args, "fill", 0);
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
            throw ToolFailure("plate " + std::to_string(plate) + " does not exist; there are " + std::to_string(plates.get_plate_count()));
    }
    const std::string plate_name = "Plate " + std::to_string(plate);
    if (plate >= 0 && plates.get_plate(plate)->is_locked())
        throw ToolFailure(plate_name + " is locked; unlock it first (run_action plate_goto with param " + std::to_string(plate) +
                          ", then plate_toggle_lock)");
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
    } else if (plate < 0) {
        plater.set_prepare_state(Job::PREPARE_STATE_DEFAULT);
        plater.arrange();
    } else {
        if (plate != plates.get_curr_plate_index() && plater.select_plate(plate) != 0)
            throw ToolFailure(plate_name + " could not be selected");
        plater.set_prepare_state(Job::PREPARE_STATE_MENU);
        plater.arrange();
    }
    if (plater.get_ui_job_worker().is_idle())
        throw ToolFailure("The arrangement did not start");
    return start;
}

// arrange's reply once the worker is idle. UI thread.
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

// The object list's name of an add_part shape, which its menus and the Primitive size dialog use;
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

// add_part: what the object's context menu Add items do, with the size or the file given instead
// of asked for, then a move to `position` along the Move panel's path. Everything the arguments
// can tell is checked before anything changes.
json add_part(Plater& plater, const json& args)
{
    const ModelObjectPtrs& objects = plater.model().objects;
    const int              oi      = arg_int(args, "object", -1);
    if (oi < 0 || oi >= int(objects.size()))
        throw ToolFailure("object index out of range");
    const ModelVolumeType type = volume_type_from_name(arg_string(args, "type"));
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
            throw ToolFailure("SVG parts come from run_action add_primitive_svg (a part of the selected object) or the SVG gizmo");
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
    return {{"object", oi}, {"volume", vi}, {"volume_count", object->volumes.size()}, {"volume_info", volume_info(*volume, vi)}};
}

} // namespace

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

std::vector<double> part_shape_size(const std::string& shape, const json& size)
{
    const char* type_name = shape_type_name(shape);
    if (type_name == nullptr)
        throw ToolFailure("shape must be cube, cylinder, sphere, cone, disc or torus");
    if (size.is_null())
        return {};
    const std::vector<const char*> names = primitive_size_names(type_name);
    std::vector<double>            out;
    if (size.is_array() && size.size() == names.size())
        for (const json& value : size)
            if (value.is_number() && std::isfinite(value.get<double>()) && value.get<double>() > 0.)
                out.push_back(value.get<double>());
    if (out.size() != names.size()) {
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
             {"name", object.name},
             {"filament", object_filament(object)},
             {"auto_drop", object.auto_drop},
             {"instances", std::move(instances)},
             {"volumes", std::move(volumes)},
             {"bbox", object.instances.empty() ? json(nullptr) : box_json(object.instance_bounding_box(0))},
             {"brim_ears", object.brim_points.size()},
             {"overrides", std::move(overrides)}};
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
    if (settings.enable_rotation && settings.align_to_y_axis) {
        settings.align_to_y_axis = false;
        if (arg_bool(args, "align_y", false))
            return "align_y is off while rotate is on, as in the Arrange options panel.";
    }
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

void register_scene_tools(McpService& service)
{
    ToolRegistry& reg = service.registry();

    reg.add(inspect_tool(
        "list_objects", L("List objects"),
        "Objects in the project: filament, auto_drop, bounding box (instance 0), brim ear count and the settings they "
        "override; their instances (plate, offset, rotation in degrees, scale, mirrored, bbox); their parts and modifiers "
        "(type, with subtype for a precise seam; filament: a part's own or its object's, a modifier's only when it sets "
        "one; position, bbox and scaled_volume in mm3 on the bed, measured on instance 0; rotation_deg and scale in the "
        "object's frame; mirrored; triangle count; mesh_volume, the unscaled mesh's mm3; painted facets: supports and "
        "seam enforcers/blockers, fuzzy_skin, mmu per filament, texture per layer; a text part's text and font, an SVG "
        "part's file); its height range modifiers when it has any (layer_config_ranges: [{min_z, max_z, values}], as "
        "set_layer_ranges takes them) and its variable layer height profile when it has one (layer_height_profile: "
        "{points, min_height, max_height}). A bbox is {min, max, size} in mm of model parts only, so unlike the panel's Size it "
        "leaves out modifiers. Indices are what select, transform, ui_mouse targets and the object/volume settings scopes take.",
        object_schema({{"plate", {{"type", "integer"}, {"minimum", 0}, {"description", "Only objects on this plate."}}},
                       {"screen", prop("boolean", "Add each instance's rectangle in the shown 3D view, logical pixels [x, y, w, h].")},
                       {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 500}}},
                       {"offset", {{"type", "integer"}, {"minimum", 0}}}}),
        [](const json& args, ToolContext& ctx) { return ToolResult::ok(ctx.run_ui([&] { return list_objects(args); })); }));

    reg.add(mutate_tool(
        "select", L("Select objects"),
        "Select objects, instances or parts in the 3D view (the assemble view while it is shown, else Prepare, switching to it), as clicking them would. Give objects, "
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
                Plater&     plater   = ui_plater();
                GLCanvas3D& canvas   = object_canvas();
                Selection&  sel      = canvas.get_selection();
                const bool  assemble = &canvas == plater.get_assmeble_canvas3D();
                if (arg_bool(args, "none", false)) {
                    if (assemble)
                        sel.remove_all();
                    else
                        plater.deselect_all();
                    canvas.set_as_dirty();
                    return selection_summary(sel);
                }
                if (arg_bool(args, "all", false)) {
                    if (assemble)
                        sel.add_all();
                    else
                        plater.select_all();
                    canvas.set_as_dirty();
                    return selection_summary(sel);
                }
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
                canvas.set_as_dirty();
                return selection_summary(sel);
            });
            settle(ctx);
            return ToolResult::ok(out);
        }));

    reg.add(mutate_tool(
        "transform", L("Move, rotate or scale an instance or a part"),
        "Change one instance's placement, or one part's with volume. translate is in mm, rotate in degrees about X/Y/Z, "
        "scale a factor (one number or [x, y, z]), size [x, y, z] mm along the object's or part's own axes, plate moves "
        "the instance onto that plate at the same place. mode \"relative\" (default) adds to the current placement, "
        "\"absolute\" sets it. An instance is dropped back onto the bed unless drop_to_bed is false. A part moves as the "
        "Object manipulation panel moves it: translate and rotate along the bed's axes, measured on instance 0, scale and "
        "size along its own axes; every instance shows the change, and Auto Drop may lower the object afterwards. One "
        "undo step. Replies with the instance's offset, scale and plate, or the part's size along its own axes and its "
        "list_objects entry (volume_info).",
        object_schema({{"object", {{"type", "integer"}, {"minimum", 0}}},
                       {"instance", {{"type", "integer"}, {"minimum", 0}, {"description", "Default 0."}}},
                       {"volume", {{"type", "integer"}, {"minimum", 0}, {"description", "Transform this part instead of the instance."}}},
                       {"translate", array_prop({{"type", "number"}}, "[x, y, z] mm")},
                       {"rotate", array_prop({{"type", "number"}}, "[x, y, z] degrees; relative only for a part")},
                       {"scale", {{"type", {"number", "array"}}, {"description", "Factor or [x, y, z] factors."}}},
                       {"size", array_prop({{"type", {"number", "null"}}}, "[x, y, z] mm, null keeps an axis; one number and two "
                                                                            "nulls keep the proportions. Not with scale.")},
                       {"plate", {{"type", "integer"}, {"minimum", 0}, {"description", "Move the instance to this plate."}}},
                       {"mode", enum_prop({"relative", "absolute"}, "Default relative.")},
                       {"drop_to_bed", prop("boolean", "Default true; instances only.")}},
                      {"object"}),
        [](const json& args, ToolContext& ctx) {
            json out = ctx.run_ui([&]() -> json {
                Plater&   plater = ui_plater();
                const int oi     = arg_int(args, "object", -1);
                if (oi < 0 || oi >= int(plater.model().objects.size()))
                    throw ToolFailure("object index out of range");
                if (args.contains("scale") && args.contains("size"))
                    throw ToolFailure("give scale or size, not both");
                return args.contains("volume") ? transform_part(plater, oi, args) : transform_instance(plater, oi, args);
            });
            settle(ctx);
            return ToolResult::ok(out);
        }));

    reg.add(mutate_tool(
        "add_part", L("Add a part or modifier to an object"),
        "Add a part, negative part, modifier, support blocker or enforcer, or precise seam to an object, as its context "
        "menu's Add items do: a primitive shape at a size, or a model file as Load... adds it. The part lands where the "
        "menu puts it (a shape beside the object, a file at its coordinates relative to the object's first part) unless "
        "position is given. Like the menu, adding a shape folds the instance's rotation and scale into the object's parts "
        "(the instance's scale then reads 1). Refused for an object with several instances: add parts first. A STEP file "
        "may ask for mesh options (blocked_by_dialog). The new part is selected; one undo step. Replies with its index, which is not always the last (parts are ordered by type), "
        "the object's part count and the part's list_objects entry (volume_info).",
        object_schema({{"object", {{"type", "integer"}, {"minimum", 0}}},
                       {"type", enum_prop({"part", "negative", "modifier", "support_blocker", "support_enforcer", "precise_seam"},
                                          "What the new part is.")},
                       {"shape", enum_prop({"cube", "cylinder", "sphere", "cone", "disc", "torus"}, "A primitive shape.")},
                       {"size", array_prop({{"type", "number"}}, "mm, the Primitive size dialog's fields: cube [x, y, z]; cylinder, "
                                                                 "cone, disc [diameter, height]; sphere [diameter]; torus [inner "
                                                                 "diameter, tube diameter]. Default: the built-in size.")},
                       {"path", prop("string", "Instead of shape: absolute path of a model file (STL, OBJ, 3MF, STEP, ...).")},
                       {"position", array_prop({{"type", "number"}}, "[x, y, z] mm on the bed for the part's centre, on "
                                                                     "instance 0, as the Move panel shows it.")}},
                      {"object", "type"}),
        [](const json& args, ToolContext& ctx) {
            json out = ctx.run_ui([&] { return add_part(ui_plater(), args); });
            settle(ctx);
            return ToolResult::ok(out);
        }));

    reg.add(mutate_tool(
        "arrange", L("Arrange objects or fill the bed"),
        "Arrange objects as the toolbar's Arrange does (every unlocked plate) or as one plate's Arrange icon does (plates: "
        "an index or \"current\"), or fill object fill's plate with more of it, as Fill bed with instances or copies does. "
        "spacing, rotate, align_y and multi_material are the Arrange options panel's settings: values given are kept as the "
        "panel keeps them, so later arranges use them too. Waits for the arrangement (a longer one replies with a job); "
        "the reply has the settings in force, instances left off every plate, plates the all-plates arrange skipped and "
        "the job's notifications. An arrange may reorder objects, so list_objects again. One undo step.",
        object_schema({{"plates", {{"type", {"string", "integer"}}, {"description", "\"all\" (default), \"current\" or a plate index."}}},
                       {"spacing", {{"type", "number"}, {"minimum", 0}, {"description", "mm between objects; 0 means auto."}}},
                       {"rotate", prop("boolean", "Auto rotate for arrangement.")},
                       {"align_y", prop("boolean", "Align to Y axis (off while rotate is on).")},
                       {"multi_material", prop("boolean", "Allow multiple materials on same plate.")},
                       {"fill", {{"type", "integer"}, {"minimum", 0}, {"description", "Instead of arranging, fill this object's plate with more of it."}}},
                       {"fill_with", enum_prop({"instances", "copies"}, "Default instances.")},
                       {"timeout_s", {{"type", "integer"}, {"minimum", 10}, {"maximum", 1800}, {"description", "Cancel it after this long (default 300)."}}}}),
        [&service](const json& args, ToolContext& ctx) {
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
                ctx.observe_ui([&] { return json(service.core().cancel_worker(ctx.job_id())); });
                throw ToolFailure("The arrangement did not finish within " + std::to_string(timeout.count()) + " s, so it was cancelled");
            }
        }));

    ToolDef slice = mutate_tool(
        "slice", L("Slice"),
        "Slice the current plate, all plates, or one plate by index, the way the toolbar's Slice button does, and wait for "
        "the result (or reply with a job id when it takes longer than the reply budget). Each requested plate reports ok, "
        "error, cancelled, empty or skipped; the whole operation is superseded when someone else started a slice or "
        "edited the project while it ran. An ok plate has time_s (silent_time_s when the printer estimates it), "
        "filament_m and filament_g including flush, filament_cost, layers, max_z, filament_changes, and filaments[i] for "
        "slot i+1 with m, g and the grams of model_g, support_g, flush_g and tower_g. Other changes are refused until it "
        "finishes; cancel stops it.",
        object_schema({{"plates", {{"type", {"string", "integer"}}, {"description", "\"current\" (default), \"all\", or a plate index."}}},
                       {"timeout_s", {{"type", "integer"}, {"minimum", 10}, {"maximum", 7200}, {"description", "Give up after this long (default 1800)."}}}}),
        [](const json& args, ToolContext& ctx) {
            const json plates_arg = args.value("plates", json("current"));
            auto       ticket     = std::make_shared<SliceTicket>();
            // Held from before the wait below, so no call admitted meanwhile can change the project
            // under the slice, and a cancel meanwhile finds the operation.
            const uint64_t cancels_before = ctx.run_ui([&] {
                ctx.begin_operation();
                if (ui_plater().printer_technology() != ptFFF)
                    throw ToolFailure("Slicing through MCP supports FFF printers only");
                return json(ui_plater().slice_cancel_seq());
            }).get<uint64_t>();
            // A change made just before (set_settings, open_files) is validated by an update
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
                PartPlateList& list   = plater.get_partplate_list();
                const bool     all    = plates_arg.is_string() && plates_arg.get<std::string>() == "all";
                if (all) {
                    for (int i = 0; i < list.get_plate_count(); ++i)
                        ticket->plates.push_back(i);
                } else if (plates_arg.is_number_integer()) {
                    const int index = plates_arg.get<int>();
                    if (index < 0 || index >= list.get_plate_count())
                        throw ToolFailure("plate index out of range");
                    ticket->plates.push_back(index);
                } else {
                    ticket->plates.push_back(list.get_curr_plate_index());
                }
                // A cancel that came while the update was pending stops the slice before it starts.
                if (plater.slice_cancel_seq() != cancels_before)
                    return json(true);
                if (!all && ticket->plates.front() != list.get_curr_plate_index())
                    plater.select_plate(ticket->plates.front());
                ticket->state.run_seq_before    = plater.slice_run_seq();
                ticket->state.revision_at_start = plater.project_revision();
                ticket->state.started           = std::chrono::steady_clock::now();
                plater.request_slice(all);
                ticket->state.request_seq_mine = plater.slice_request_seq();
                ticket->state.seen_running     = plater.is_slicing_any();
                if (plater.slice_run_seq() == ticket->state.run_seq_before)
                    if (auto running = plater.slice_run(ticket->state.run_seq_before); running && !running->finished)
                        ticket->state.adopted_run = running->seq;
                // The Preview tab shows the result, as after the Slice button.
                if (MainFrame* frame = wxGetApp().mainframe)
                    frame->select_tab(wxString(TAB_ID_PREVIEW));
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
        });
    slice.budget_ms = 45000;
    reg.add(slice);

    ToolDef cancel = mutate_tool(
        "cancel", L("Cancel an operation"),
        "Stop the running slice or G-code export, as the cancel button on the slicing progress does, or the background job "
        "(an arrange, orient or fill bed) an MCP call started. That job then reports cancelled; a background job keeps "
        "other changes out until it has really stopped. With job, only when that job is the running operation. Allowed "
        "while an operation holds the project.",
        object_schema({{"job", prop("string", "The operation's job to cancel, e.g. \"j12\".")}}),
        [&service](const json& args, ToolContext& ctx) {
            const auto ops = service.jobs().running_operations();
            if (args.contains("job")) {
                const std::string job = arg_string(args, "job");
                if (std::find(ops.begin(), ops.end(), job) == ops.end())
                    throw ToolFailure("Job " + job + " is not a running operation", {{"running", ops}});
            }
            json out = ctx.run_ui([&]() -> json {
                // Only a job an MCP call started: a person's (Text typing) is theirs to stop.
                for (const std::string& op : ops)
                    if ((!args.contains("job") || op == arg_string(args, "job")) && service.core().cancel_worker(op))
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
    reg.add(cancel);
}

}}} // namespace Slic3r::GUI::Mcp
