#include "GLGizmoMeshBoolean.hpp"
#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoBase.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosCommon.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Jobs/Job.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp"
#include "slic3r/GUI/MeshUtils.hpp"
#include "slic3r/GUI/ObjectDataViewModel.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Selection.hpp"
#include "slic3r/GUI/Shortcuts.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/MeshBoolean.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <boost/log/trivial.hpp>
#include <imgui.h>
#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui/imgui_internal.h>
#include <wx/dataview.h>

namespace Slic3r {
namespace GUI {

namespace {

const ColorRGBA LIST_A_COLOR{0.0f, 150.0f / 255.0f, 136.0f / 255.0f, 1.0f};
const ColorRGBA LIST_B_COLOR{1.0f, 0.55f, 0.0f, 1.0f};

bool contains(const std::vector<ObjectID> &list, const ObjectID &id) { return std::find(list.begin(), list.end(), id) != list.end(); }

int volume_index(const ModelObject &mo, const ObjectID &id)
{
    for (size_t i = 0; i < mo.volumes.size(); ++i)
        if (mo.volumes[i]->id() == id)
            return int(i);
    return -1;
}

int object_index(const Model &model, const ObjectID &id)
{
    for (size_t i = 0; i < model.objects.size(); ++i)
        if (model.objects[i]->id() == id)
            return int(i);
    return -1;
}

// Appended to the source's name; not translated, as before.
std::string operation_suffix(MeshBooleanOperation operation)
{
    switch (operation) {
    case MeshBooleanOperation::Union: return "union";
    case MeshBooleanOperation::Difference: return "difference";
    case MeshBooleanOperation::Intersection: return "intersection";
    }
    return {};
}

std::string warning_message(MeshBooleanWarning warning, MeshBooleanTarget target)
{
    const bool objects = target == MeshBooleanTarget::Object;
    switch (warning) {
    case MeshBooleanWarning::Failed:
        return objects ? _u8L("Unable to perform boolean operation on selected objects") : _u8L("Unable to perform boolean operation on selected parts");
    case MeshBooleanWarning::NoOverlap:
        return objects ? _u8L("The selected objects have no overlapping volume.") : _u8L("The selected parts have no overlapping volume.");
    case MeshBooleanWarning::InputsChanged:
        return objects ? _u8L("The objects changed while the boolean operation was running. Nothing was applied.") :
                         _u8L("The parts changed while the boolean operation was running. Nothing was applied.");
    case MeshBooleanWarning::None: break;
    }
    return {};
}

} // namespace

// Everything a running job needs, copied when it starts so the model can change meanwhile.
struct GLGizmoMeshBoolean::JobState
{
    struct Input
    {
        ObjectID                                       id;
        std::shared_ptr<const TriangleMesh>            mesh;
        Transform3d                                    volume_matrix;
        std::optional<TriangleSelector::SavedPainting> painting;
    };

    // Object mode: one whole object, all its instances and volumes.
    struct ObjectInput
    {
        struct Volume
        {
            Input           input;
            ModelVolumeType type;
        };
        ObjectID                 id;
        std::vector<ObjectID>    instance_ids;
        std::vector<Transform3d> instance_matrices;
        std::vector<Volume>      volumes;
    };

    MeshBooleanTarget    target{MeshBooleanTarget::Part};
    MeshBooleanOperation operation{MeshBooleanOperation::Union};
    bool                 delete_input{false};
    // Part mode.
    ObjectID             object_id;
    ObjectID             instance_id;
    Transform3d          instance_matrix;
    std::vector<Input>   a;
    std::vector<Input>   b;
    // Object mode: the listed objects, and the selection they were picked from.
    std::vector<ObjectInput> objects_a;
    std::vector<ObjectInput> objects_b;
    std::vector<ObjectID>    selected_objects;

    std::atomic<bool> cancel{false};
    // Set when process() returns, so closing the gizmo no longer cancels the worker's current job.
    std::atomic<bool> processed{false};

    // Written by the worker, read in finalize.
    std::vector<std::optional<TriangleMesh>> results;
    // The inputs themselves could not be built, or the job threw.
    bool                                     failed{false};
};

GLGizmoMeshBoolean::GLGizmoMeshBoolean(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
{
}

GLGizmoMeshBoolean::~GLGizmoMeshBoolean()
{
    if (m_job)
        m_job->cancel = true;
}

ModelObject *GLGizmoMeshBoolean::selected_object() const
{
    const Selection &selection = m_parent.get_selection();
    const int        obj_idx   = selection.get_object_idx();
    return obj_idx >= 0 && obj_idx < int(selection.get_model()->objects.size()) ? selection.get_model()->objects[obj_idx] : nullptr;
}

bool GLGizmoMeshBoolean::gizmo_event(SLAGizmoEventType action, const Vec2d& mouse_position, bool shift_down, bool alt_down, bool control_down)
{
    if (action != SLAGizmoEventType::LeftDown || m_running)
        return true;

    if (m_target == MeshBooleanTarget::Object) {
        // Only the objects selected when the gizmo opened can be picked; the canvas selection stays as it is.
        const int hover = m_parent.get_first_hover_volume_idx();
        if (hover < 0 || hover >= int(m_parent.get_volumes().volumes.size()))
            return true;
        const int    obj_idx = m_parent.get_volumes().volumes[hover]->object_idx();
        const Model &model   = *m_parent.get_selection().get_model();
        if (obj_idx >= 0 && obj_idx < int(model.objects.size()) && contains(m_objects, model.objects[obj_idx]->id()))
            toggle_part(model.objects[obj_idx]->id());
        return true;
    }

    const ModelObject *mo        = selected_object();
    const int          inst_idx  = m_parent.get_selection().get_instance_idx();
    const auto        *raycaster = m_c->raycaster();
    if (mo == nullptr || inst_idx < 0 || inst_idx >= int(mo->instances.size()) || raycaster == nullptr ||
        raycaster->raycasters().size() != mo->volumes.size())
        return true;
    const ClippingPlane *clipping_plane = m_c->object_clipper() ? m_c->object_clipper()->get_clipping_plane() : nullptr;
    const ModelInstance *mi             = mo->instances[inst_idx];
    const Camera        &camera         = wxGetApp().plater()->get_camera();

    // Cast a ray on all meshes and pick the closest hit.
    double closest_hit_squared_distance = std::numeric_limits<double>::max();
    int    closest_hit_mesh_id          = -1;
    for (int mesh_id = 0; mesh_id < int(mo->volumes.size()); ++mesh_id) {
        const Transform3d trafo = mi->get_transformation().get_matrix() * mo->volumes[mesh_id]->get_matrix();
        Vec3f             hit    = Vec3f::Zero();
        Vec3f             normal = Vec3f::Zero();
        size_t            facet  = 0;
        if (raycaster->raycasters()[mesh_id]->unproject_on_mesh(mouse_position, trafo, camera, hit, normal, clipping_plane, &facet)) {
            const double hit_squared_distance = (camera.get_position() - trafo * hit.cast<double>()).squaredNorm();
            if (hit_squared_distance < closest_hit_squared_distance) {
                closest_hit_squared_distance = hit_squared_distance;
                closest_hit_mesh_id          = mesh_id;
            }
        }
    }

    if (closest_hit_mesh_id >= 0)
        toggle_part(mo->volumes[closest_hit_mesh_id]->id());
    return true;
}

bool GLGizmoMeshBoolean::on_mouse(const wxMouseEvent &mouse_event)
{
    // When control is down we allow scene pan and rotation even when clicking over some object.
    const bool grabber_contains_mouse = (get_hover_id() != -1);
    if (mouse_event.LeftDown() && (!mouse_event.CmdDown() || grabber_contains_mouse)) {
        const Vec2d mouse_pos(mouse_event.GetX(), mouse_event.GetY());
        return gizmo_event(SLAGizmoEventType::LeftDown, mouse_pos, mouse_event.ShiftDown(), mouse_event.AltDown(), false);
    }
    return false;
}

bool GLGizmoMeshBoolean::on_init()
{
    m_shortcut = Shortcut::GizmoMeshBoolean;
    return true;
}

std::string GLGizmoMeshBoolean::on_get_name() const
{
    return _u8L("Mesh Boolean");
}

std::optional<MeshBooleanTarget> GLGizmoMeshBoolean::target_for(const Selection &selection)
{
    if (selection.is_single_full_instance() && selection.get_volume_idxs().size() > 1)
        return MeshBooleanTarget::Part;
    if (!selection.is_multiple_full_object() || selection.is_wipe_tower())
        return std::nullopt;

    // Whole objects, none of them cut, every instance fully on one plate.
    const Model&   model  = *selection.get_model();
    PartPlateList& plates = wxGetApp().plater()->get_partplate_list();
    int            plate  = -1;
    for (const auto &[obj_idx, instances] : selection.get_content()) {
        if (obj_idx < 0 || obj_idx >= int(model.objects.size()) || model.objects[obj_idx]->is_cut())
            return std::nullopt;
        for (int inst_idx : instances) {
            const int p = plates.find_instance(obj_idx, inst_idx);
            if (p < 0 || (plate >= 0 && p != plate) || !plates.get_plate(p)->contain_instance_totally(obj_idx, inst_idx))
                return std::nullopt;
            plate = p;
        }
    }
    return MeshBooleanTarget::Object;
}

bool GLGizmoMeshBoolean::on_is_activable() const
{
    return target_for(m_parent.get_selection()).has_value();
}

std::string GLGizmoMeshBoolean::entry_name(const ObjectID &id) const
{
    const Model &model = *m_parent.get_selection().get_model();
    if (m_target == MeshBooleanTarget::Object) {
        if (const int idx = object_index(model, id); idx >= 0)
            return model.objects[idx]->name;
    } else if (const ModelObject *mo = selected_object()) {
        if (const int idx = volume_index(*mo, id); idx >= 0)
            return mo->volumes[idx]->name;
    }
    return {};
}

void GLGizmoMeshBoolean::toggle_part(const ObjectID &id)
{
    PartList &target = uses_two_lists() && m_picking_b ? m_list_b : m_list_a;
    PartList &other  = &target == &m_list_a ? m_list_b : m_list_a;
    other.erase(std::remove(other.begin(), other.end(), id), other.end());
    if (auto it = std::find(target.begin(), target.end(), id); it != target.end())
        target.erase(it);
    else
        target.push_back(id);
    m_warning = MeshBooleanWarning::None;
    m_parent.set_as_dirty();
}

void GLGizmoMeshBoolean::reset_lists()
{
    m_list_a.clear();
    m_list_b.clear();
    m_picking_b = false;
    m_warning   = MeshBooleanWarning::None;
}

void GLGizmoMeshBoolean::sync_with_selection()
{
    const Selection&                       selection = m_parent.get_selection();
    const std::optional<MeshBooleanTarget> target    = target_for(selection);
    if (!target)
        return;

    // The lists only make sense for the selection they were picked from.
    const ModelObject*    mo = *target == MeshBooleanTarget::Part ? selected_object() : nullptr;
    std::vector<ObjectID> objects;
    if (*target == MeshBooleanTarget::Object)
        for (const auto &[obj_idx, instances] : selection.get_content())
            objects.push_back(selection.get_model()->objects[obj_idx]->id());
    const ObjectID object_id = mo ? mo->id() : ObjectID();
    if (*target != m_target || objects != m_objects || object_id != m_object_id) {
        reset_lists();
        m_target    = *target;
        m_objects   = std::move(objects);
        m_object_id = object_id;
        return;
    }
    const auto gone = [this, mo](const ObjectID &id) { return mo ? volume_index(*mo, id) < 0 : !contains(m_objects, id); };
    for (PartList *list : {&m_list_a, &m_list_b})
        list->erase(std::remove_if(list->begin(), list->end(), gone), list->end());
}

void GLGizmoMeshBoolean::update_highlights()
{
    const Selection& selection = m_parent.get_selection();
    const Model&     model     = *selection.get_model();
    bool             changed   = false;
    for (GLVolume* v : m_parent.get_volumes().volumes) {
        std::optional<ColorRGBA> color;
        const int obj_idx = v->object_idx();
        if (obj_idx >= 0 && obj_idx < int(model.objects.size()) && v->volume_idx() >= 0 &&
            v->volume_idx() < int(model.objects[obj_idx]->volumes.size())) {
            const ModelObject *mo = model.objects[obj_idx];
            ObjectID           id;
            if (m_target == MeshBooleanTarget::Object)
                id = mo->id();
            else if (obj_idx == selection.get_object_idx() && v->instance_idx() == selection.get_instance_idx())
                id = mo->volumes[v->volume_idx()]->id();
            if (id.valid() && contains(m_list_a, id))
                color = LIST_A_COLOR;
            else if (id.valid() && contains(m_list_b, id))
                color = LIST_B_COLOR;
        }
        if (v->color_override != color) {
            v->color_override = color;
            changed           = true;
        }
    }
    m_highlighted = true;
    // The volumes were drawn before the gizmo, so show the new colours in the next frame.
    if (changed)
        m_parent.request_extra_frame();
}

void GLGizmoMeshBoolean::clear_highlights()
{
    if (!m_highlighted)
        return;
    for (GLVolume* v : m_parent.get_volumes().volumes)
        v->color_override.reset();
    m_highlighted = false;
    m_parent.set_as_dirty();
}

void GLGizmoMeshBoolean::on_render()
{
    sync_with_selection();
    update_highlights();
}

void GLGizmoMeshBoolean::on_set_state()
{
    // Runs before the common gizmo data is updated, so it reads only the selection and the model.
    if (m_state == EState::On) {
        m_operation_mode     = MeshBooleanOperation::Union;
        m_diff_delete_input  = false;
        m_inter_delete_input = false;
        m_objects.clear();
        m_object_id = ObjectID();
        sync_with_selection();
        reset_lists();
    } else if (m_state == EState::Off) {
        // Cancel without waiting; the job's finalize sees the new token or the closed gizmo and drops its result.
        // The worker is only cancelled while it still runs this job, not one queued after it.
        if (m_running && m_job) {
            m_job->cancel = true;
            if (!m_job->processed)
                wxGetApp().plater()->get_ui_job_worker().cancel();
        }
        ++m_job_token;
        m_running = false;
        m_job.reset();
        reset_lists();
        clear_highlights();
    }
}

CommonGizmosDataID GLGizmoMeshBoolean::on_get_requirements() const
{
    if (m_c && m_c->raycaster_ptr())
        m_c->raycaster_ptr()->set_only_support_model_part_flag(false);
    return CommonGizmosDataID(
        int(CommonGizmosDataID::SelectionInfo)
        | int(CommonGizmosDataID::InstancesHider)
        | int(CommonGizmosDataID::Raycaster)
        | int(CommonGizmosDataID::ObjectClipper));
}

bool GLGizmoMeshBoolean::can_execute() const
{
    if (m_running || !wxGetApp().plater()->get_ui_job_worker().is_idle())
        return false;
    return uses_two_lists() ? !m_list_a.empty() && !m_list_b.empty() : m_list_a.size() >= 2;
}

void GLGizmoMeshBoolean::execute()
{
    if (!can_execute())
        return;

    auto job          = std::make_shared<JobState>();
    job->target       = m_target;
    job->operation    = m_operation_mode;
    job->delete_input = m_operation_mode == MeshBooleanOperation::Difference ? m_diff_delete_input : m_inter_delete_input;

    // Paintings are read here, on the main thread; the meshes are shared and never change in place.
    const bool keep_painting = wxGetApp().app_config->get_bool("keep_painting");
    const auto input_of      = [keep_painting](const ModelVolume &mv) {
        return JobState::Input{mv.id(), mv.mesh_ptr(), mv.get_matrix(), keep_painting ? mv.save_painting() : std::nullopt};
    };
    if (m_target == MeshBooleanTarget::Part) {
        ModelObject *mo       = selected_object();
        const int    inst_idx = m_parent.get_selection().get_instance_idx();
        if (mo == nullptr || inst_idx < 0 || inst_idx >= int(mo->instances.size()))
            return;
        const ModelInstance *mi = mo->instances[inst_idx];
        job->object_id          = mo->id();
        job->instance_id        = mi->id();
        job->instance_matrix    = mi->get_matrix();
        for (auto [list, inputs] : {std::pair{&m_list_a, &job->a}, std::pair{&m_list_b, &job->b}})
            for (const ObjectID &id : *list) {
                const int idx = volume_index(*mo, id);
                if (idx < 0)
                    return;
                inputs->push_back(input_of(*mo->volumes[idx]));
            }
    } else {
        const Model &model = *m_parent.get_selection().get_model();
        for (auto [list, inputs] : {std::pair{&m_list_a, &job->objects_a}, std::pair{&m_list_b, &job->objects_b}})
            for (const ObjectID &id : *list) {
                const int idx = object_index(model, id);
                if (idx < 0)
                    return;
                const ModelObject    *mo = model.objects[idx];
                JobState::ObjectInput input{id, {}, {}, {}};
                for (const ModelInstance *mi : mo->instances) {
                    input.instance_ids.push_back(mi->id());
                    input.instance_matrices.push_back(mi->get_matrix());
                }
                for (const ModelVolume *mv : mo->volumes)
                    input.volumes.push_back({input_of(*mv), mv->type()});
                inputs->push_back(std::move(input));
            }
        job->selected_objects = m_objects;
    }

    m_job                     = job;
    m_running                 = true;
    m_warning                 = MeshBooleanWarning::None;
    const std::uint64_t token = ++m_job_token;
    const std::string   status = on_get_name();
    std::weak_ptr<char> alive  = m_alive;

    queue_job(
        wxGetApp().plater()->get_ui_job_worker(),
        [job, status](Job::Ctl &ctl) {
            struct MarkProcessed
            {
                JobState &job;
                ~MarkProcessed() { job.processed = true; }
            } mark_processed{*job};
            const auto cancel = [&ctl, &job]() { return job->cancel || ctl.was_canceled(); };
            // Maps 0-100 onto [first, first + span] of the job's progress.
            const auto progress_in = [&ctl, &status](float first, float span) {
                return [&ctl, &status, first, span](float percent) { ctl.update_status(int(first + span * percent / 100.f), status); };
            };
            const auto to_world = [](TriangleMesh mesh, const Transform3d &matrix) {
                mesh.transform(matrix, true);
                MeshBoolean::ops::normalize_input(mesh);
                return mesh;
            };
            // Part mode: the listed volumes. Object mode: each listed object's printed solid over all its instances,
            // which takes the first half of the progress.
            const size_t object_count = job->objects_a.size() + job->objects_b.size();
            size_t       objects_done = 0;
            const auto   meshes       = [&](const std::vector<JobState::Input> &parts, const std::vector<JobState::ObjectInput> &objects) {
                std::vector<TriangleMesh> result;
                for (const JobState::Input &input : parts)
                    result.push_back(to_world(*input.mesh, job->instance_matrix * input.volume_matrix));
                for (const JobState::ObjectInput &object : objects) {
                    std::vector<TriangleMesh> solids, negatives;
                    for (const Transform3d &instance : object.instance_matrices)
                        for (const JobState::ObjectInput::Volume &volume : object.volumes) {
                            if (volume.type == ModelVolumeType::MODEL_PART)
                                solids.push_back(to_world(*volume.input.mesh, instance * volume.input.volume_matrix));
                            else if (volume.type == ModelVolumeType::NEGATIVE_VOLUME)
                                negatives.push_back(to_world(*volume.input.mesh, instance * volume.input.volume_matrix));
                        }
                    std::optional<TriangleMesh> solid =
                        MeshBoolean::ops::solid(solids, negatives, cancel, progress_in(50.f * objects_done / object_count, 50.f / object_count));
                    ++objects_done;
                    if (!solid)
                        return std::vector<TriangleMesh>();
                    result.push_back(std::move(*solid));
                }
                return result;
            };
            const auto op_progress = job->target == MeshBooleanTarget::Object ? progress_in(50.f, 50.f) : progress_in(0.f, 100.f);

            const std::vector<TriangleMesh> a = meshes(job->a, job->objects_a);
            if (cancel())
                return;
            if (a.empty()) {
                job->failed = true;
                return;
            }
            switch (job->operation) {
            case MeshBooleanOperation::Union:
                job->results = {MeshBoolean::ops::union_all(a, cancel, op_progress)};
                break;
            case MeshBooleanOperation::Intersection:
                job->results = {MeshBoolean::ops::intersect_all(a, cancel, op_progress, &job->failed)};
                break;
            case MeshBooleanOperation::Difference: {
                const std::vector<TriangleMesh> b = meshes(job->b, job->objects_b);
                if (cancel())
                    return;
                if (b.empty()) {
                    job->failed = true;
                    return;
                }
                // A part is cut by each tool in turn; an object by the union of the tool objects.
                job->results = job->target == MeshBooleanTarget::Part ? MeshBoolean::ops::subtract_each(a, b, cancel, op_progress) :
                                                                         MeshBoolean::ops::subtract_union(a, b, cancel, op_progress);
                break;
            }
            }
        },
        [this, alive, token, job](bool canceled, std::exception_ptr &eptr) {
            job->processed = true;
            if (eptr) {
                try {
                    std::rethrow_exception(eptr);
                } catch (const std::exception &e) {
                    BOOST_LOG_TRIVIAL(error) << "Mesh boolean failed: " << e.what();
                } catch (...) {
                    BOOST_LOG_TRIVIAL(error) << "Mesh boolean failed";
                }
                // Shown as the gizmo's warning instead of an error dialog.
                eptr        = nullptr;
                job->failed = true;
            }
            if (!alive.expired())
                on_job_finished(token, canceled, *job);
        });
}

void GLGizmoMeshBoolean::on_job_finished(std::uint64_t token, bool canceled, const JobState &job)
{
    if (token != m_job_token)
        return;
    m_running = false;
    m_job.reset();
    m_parent.set_as_dirty();
    if (canceled || job.cancel || m_state != EState::On)
        return;

    const bool has_result = !job.failed && std::any_of(job.results.begin(), job.results.end(), [](const auto &r) { return r.has_value(); });
    if (!has_result) {
        // Only an intersection that ran cleanly and found nothing means the inputs don't overlap.
        m_warning = job.operation == MeshBooleanOperation::Intersection && !job.failed ? MeshBooleanWarning::NoOverlap :
                                                                                          MeshBooleanWarning::Failed;
        return;
    }
    if (job.target == MeshBooleanTarget::Part)
        apply_part_result(job);
    else
        apply_object_result(job);
}

void GLGizmoMeshBoolean::apply_part_result(const JobState &job)
{
    const Selection &selection = m_parent.get_selection();
    ModelObject     *mo        = selected_object();
    const int        obj_idx   = selection.get_object_idx();
    const int        inst_idx  = selection.get_instance_idx();

    // Apply only to exactly the parts the job read, while the gizmo still shows them.
    const auto unchanged = [mo](const JobState::Input &input) {
        const int idx = volume_index(*mo, input.id);
        return idx >= 0 && mo->volumes[idx]->mesh_ptr() == input.mesh && mo->volumes[idx]->get_matrix().matrix() == input.volume_matrix.matrix();
    };
    if (m_target != MeshBooleanTarget::Part || mo == nullptr || inst_idx < 0 || inst_idx >= int(mo->instances.size()) ||
        mo->id() != job.object_id || mo->instances[inst_idx]->id() != job.instance_id ||
        mo->instances[inst_idx]->get_matrix().matrix() != job.instance_matrix.matrix() || !std::all_of(job.a.begin(), job.a.end(), unchanged) ||
        !std::all_of(job.b.begin(), job.b.end(), unchanged)) {
        m_warning = MeshBooleanWarning::InputsChanged;
        return;
    }

    // One undo step for the whole operation; the deletes below take none of their own.
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Mesh Boolean");
    const std::string    suffix = operation_suffix(job.operation);

    // Puts the result where `like` is, in its frame, with its name, type and settings, and carries over the paintings.
    std::vector<const ModelVolume*> new_volumes;
    const auto replace = [&](TriangleMesh mesh, const JobState::Input &like, const std::vector<const JobState::Input *> &painted) {
        const int    idx     = volume_index(*mo, like.id);
        ModelVolume* old_vol = mo->volumes[idx];
        mesh.transform((job.instance_matrix * like.volume_matrix).inverse(), true);
        ModelVolume* new_vol = mo->add_volume(std::move(mesh), old_vol->type(), false);
        new_vol->name        = old_vol->name + " - " + suffix;
        new_vol->config.apply(old_vol->config);
        new_vol->set_material_id(old_vol->material_id());
        new_vol->set_transformation(old_vol->get_transformation());
        for (const JobState::Input *input : painted) {
            if (!input->painting)
                continue;
            std::optional<TriangleSelector::SavedPainting> painting = input->painting;
            painting->mesh.transform(like.volume_matrix.inverse() * input->volume_matrix);
            new_vol->restore_painting(painting, true);
        }
        // The result takes the old volume's slot.
        std::swap(mo->volumes[idx], mo->volumes.back());
        mo->delete_volume(mo->volumes.size() - 1);
        new_volumes.push_back(new_vol);
    };

    std::vector<const JobState::Input *> to_delete;
    if (job.operation == MeshBooleanOperation::Difference) {
        for (size_t i = 0; i < job.a.size(); ++i)
            if (i < job.results.size() && job.results[i])
                replace(*job.results[i], job.a[i], {&job.a[i]});
        if (job.delete_input)
            for (const JobState::Input &input : job.b)
                to_delete.push_back(&input);
    } else {
        std::vector<const JobState::Input *> all;
        for (const JobState::Input &input : job.a)
            all.push_back(&input);
        replace(*job.results.front(), job.a.front(), all);
        // Union always consumes its inputs; Intersection keeps them unless asked.
        if (job.operation == MeshBooleanOperation::Union || job.delete_input)
            to_delete.assign(all.begin() + 1, all.end());
    }

    std::vector<ItemForDelete> items;
    for (const JobState::Input *input : to_delete)
        items.emplace_back(ItemType::itVolume, obj_idx, volume_index(*mo, input->id));
    std::sort(items.begin(), items.end(), [](const ItemForDelete &l, const ItemForDelete &r) { return l.sub_obj_idx < r.sub_obj_idx; });
    if (!items.empty())
        wxGetApp().obj_list()->delete_from_model_and_list(items);

    reset_lists();
    wxGetApp().plater()->update();
    wxGetApp().obj_list()->select_item([obj_idx, new_volumes]() {
        wxDataViewItemArray items = wxGetApp().obj_list()->reorder_volumes_and_get_selection(obj_idx, [&new_volumes](const ModelVolume *volume) {
            return std::find(new_volumes.begin(), new_volumes.end(), volume) != new_volumes.end();
        });
        return items.IsEmpty() ? wxDataViewItem() : items.front();
    });
}

void GLGizmoMeshBoolean::apply_object_result(const JobState &job)
{
    Model &model = wxGetApp().plater()->model();
    // Apply only to exactly the objects the job read, while the gizmo still shows the same selection.
    const auto unchanged = [&model](const JobState::ObjectInput &input) {
        const int idx = object_index(model, input.id);
        if (idx < 0)
            return false;
        const ModelObject *mo = model.objects[idx];
        if (mo->instances.size() != input.instance_ids.size() || mo->volumes.size() != input.volumes.size())
            return false;
        for (size_t i = 0; i < mo->instances.size(); ++i)
            if (mo->instances[i]->id() != input.instance_ids[i] || mo->instances[i]->get_matrix().matrix() != input.instance_matrices[i].matrix())
                return false;
        for (size_t i = 0; i < mo->volumes.size(); ++i) {
            const JobState::Input &volume = input.volumes[i].input;
            if (mo->volumes[i]->id() != volume.id || mo->volumes[i]->mesh_ptr() != volume.mesh ||
                mo->volumes[i]->get_matrix().matrix() != volume.volume_matrix.matrix())
                return false;
        }
        return true;
    };
    if (m_target != MeshBooleanTarget::Object || m_objects != job.selected_objects ||
        !std::all_of(job.objects_a.begin(), job.objects_a.end(), unchanged) || !std::all_of(job.objects_b.begin(), job.objects_b.end(), unchanged)) {
        m_warning = MeshBooleanWarning::InputsChanged;
        return;
    }

    // One undo step for the whole operation; the deletes below take none of their own.
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Mesh Boolean");
    const std::string    suffix = operation_suffix(job.operation);

    // A new object in the place of `like`'s first instance, with its settings. The paintings of `from` are carried over,
    // and so are their modifiers and support blockers and enforcers; negative volumes are already in the mesh. Painted
    // modifiers are left out: they belong to a part of the source object.
    std::vector<ModelObject *> new_objects;
    const auto create = [&](TriangleMesh mesh, const JobState::ObjectInput &like, const std::vector<const JobState::ObjectInput *> &from) {
        const ModelObject *src = model.objects[object_index(model, like.id)];
        ModelObject       *obj = model.add_object();
        obj->name              = src->name + " - " + suffix;
        obj->config.assign_config(src->config);
        ModelInstance *instance = obj->add_instance(*src->instances.front());
        if (!instance->is_assemble_initialized())
            instance->set_assemble_transformation(instance->get_transformation());
        const Transform3d to_local = instance->get_matrix().inverse();

        mesh.transform(to_local, true);
        ModelVolume *result = obj->add_volume(std::move(mesh), ModelVolumeType::MODEL_PART, false);
        result->name        = obj->name;
        for (const JobState::ObjectInput *input : from) {
            const ModelObject *source = model.objects[object_index(model, input->id)];
            for (size_t v = 0; v < input->volumes.size(); ++v) {
                const JobState::ObjectInput::Volume &volume = input->volumes[v];
                if (volume.type == ModelVolumeType::NEGATIVE_VOLUME || volume.type == ModelVolumeType::PAINTED_MODIFIER)
                    continue;
                for (const Transform3d &instance_matrix : input->instance_matrices) {
                    const Transform3d to_result = to_local * instance_matrix * volume.input.volume_matrix;
                    if (volume.type != ModelVolumeType::MODEL_PART) {
                        ModelVolume *carried = obj->add_volume(*source->volumes[v]);
                        carried->set_transformation(Geometry::Transformation(to_result));
                    } else if (volume.input.painting) {
                        std::optional<TriangleSelector::SavedPainting> painting = volume.input.painting;
                        painting->mesh.transform(to_result);
                        result->restore_painting(painting, true);
                    }
                }
            }
        }
        obj->invalidate_bounding_box();
        new_objects.push_back(obj);
    };

    std::vector<const JobState::ObjectInput *> to_delete;
    if (job.operation == MeshBooleanOperation::Difference) {
        for (size_t i = 0; i < job.objects_a.size(); ++i)
            if (i < job.results.size() && job.results[i]) {
                create(*job.results[i], job.objects_a[i], {&job.objects_a[i]});
                to_delete.push_back(&job.objects_a[i]);
            }
        if (job.delete_input)
            for (const JobState::ObjectInput &input : job.objects_b)
                to_delete.push_back(&input);
    } else {
        std::vector<const JobState::ObjectInput *> all;
        for (const JobState::ObjectInput &input : job.objects_a)
            all.push_back(&input);
        create(*job.results.front(), job.objects_a.front(), all);
        // The result replaces the first object. Union always consumes the others; Intersection keeps them unless asked.
        if (job.operation == MeshBooleanOperation::Union || job.delete_input)
            to_delete = all;
        else
            to_delete.push_back(all.front());
    }

    // Add the new objects and tell the plates first, then delete the sources.
    ObjectList *obj_list = wxGetApp().obj_list();
    for (ModelObject *obj : new_objects) {
        const int idx = object_index(model, obj->id());
        obj_list->notify_instance_updated(idx);
        obj_list->add_object_to_list(size_t(idx));
    }
    std::vector<ItemForDelete> items;
    for (const JobState::ObjectInput *input : to_delete)
        items.emplace_back(ItemType::itObject, object_index(model, input->id), -1);
    std::sort(items.begin(), items.end(), [](const ItemForDelete &l, const ItemForDelete &r) { return l.obj_idx < r.obj_idx; });
    if (!items.empty())
        obj_list->delete_from_model_and_list(items);

    reset_lists();
    wxGetApp().plater()->update();
    obj_list->select_item(obj_list->GetModel()->GetItemById(object_index(model, new_objects.front()->id())));
    obj_list->update_selections_on_canvas();
}

bool GLGizmoMeshBoolean::render_list(PartList &list, const char *imgui_id)
{
    bool removed = false;
    for (size_t i = 0; i < list.size() && !removed; ++i) {
        ImGui::PushID(imgui_id);
        ImGui::PushID(int(i));
        ImGui::AlignTextToFramePadding();
        m_imgui->text(entry_name(list[i]));
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, {0, 0, 0, 0});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_Button));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_Button));
        ImGui::PushStyleColor(ImGuiCol_Border, {0, 0, 0, 0});
        // Drawn even while a job runs, disabled, so the row keeps its layout.
        ImGui::PushItemFlag(ImGuiItemFlags_Disabled, m_running);
        if (ImGui::Button(into_u8(ImGui::TextSearchCloseIcon).c_str(), {18, 18})) {
            list.erase(list.begin() + i);
            removed = true;
        }
        ImGui::PopItemFlag();
        ImGui::PopStyleColor(4);
        ImGui::PopID();
        ImGui::PopID();
    }
    return removed;
}

void GLGizmoMeshBoolean::on_render_input_window(float x, float y, float bottom_limit)
{
    y = std::min(y, bottom_limit - ImGui::GetWindowHeight());

    static float last_y = 0.0f;
    static float last_w = 0.0f;

    const float currt_scale = m_parent.get_scale();
    ImGuiWrapper::push_toolbar_style(currt_scale);
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
    GizmoImguiBegin("MeshBoolean", ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    const int max_tab_length = 2 * ImGui::GetStyle().FramePadding.x + std::max(ImGui::CalcTextSize(_u8L("Union").c_str()).x,
        std::max(ImGui::CalcTextSize(_u8L("Difference").c_str()).x, ImGui::CalcTextSize(_u8L("Intersection").c_str()).x));
    const float wrap_width = 3.0f * max_tab_length;

    auto selectable = [](const std::string& label, bool selected, const ImVec2& size_arg) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0,0 });

        ImGuiWindow* window = ImGui::GetCurrentWindow();
        const ImVec2 label_size = ImGui::CalcTextSize(label.c_str(), NULL, true);
        ImVec2 pos = window->DC.CursorPos;
        ImVec2 size = ImGui::CalcItemSize(size_arg, label_size.x + ImGui::GetStyle().FramePadding.x * 2.0f, label_size.y + ImGui::GetStyle().FramePadding.y * 2.0f);
        bool hovered = ImGui::IsMouseHoveringRect(pos, pos + size);

        const ImVec4 accent = ImGuiWrapper::to_ImVec4(LIST_A_COLOR);
        if (selected || hovered) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Button, accent);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, accent);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, accent);
        }
        else {
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, accent);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, accent);
        }

        bool res = ImGui::Button(label.c_str(), size_arg);

        if (selected || hovered) {
            ImGui::PopStyleColor(4);
        }
        else {
            ImGui::PopStyleColor(2);
        }

        ImGui::PopStyleVar(1);
        return res;
    };

    auto operate_button = [this](const wxString &label, bool enable) {
        if (!enable) {
            ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
            if (m_is_dark_mode) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(39.0f / 255.0f, 39.0f / 255.0f, 39.0f / 255.0f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(108.0f / 255.0f, 108.0f / 255.0f, 108.0f / 255.0f, 1.0f));
            }
            else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(230.0f / 255.0f, 230.0f / 255.0f, 230.0f / 255.0f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(163.0f / 255.0f, 163.0f / 255.0f, 163.0f / 255.0f, 1.0f));
            }
        }

        bool res = m_imgui->button(label.c_str());

        if (!enable) {
            ImGui::PopItemFlag();
            ImGui::PopStyleColor(2);
        }
        return res;
    };

    // Switching keeps the picks: with two picked, the first is the source, the second the tool.
    auto set_mode = [this](MeshBooleanOperation mode) {
        if (mode == m_operation_mode || m_running)
            return;
        if (mode == MeshBooleanOperation::Difference) {
            if (m_list_b.empty() && m_list_a.size() > 1) {
                m_list_b.assign(m_list_a.begin() + 1, m_list_a.end());
                m_list_a.resize(1);
            }
        } else {
            m_list_a.insert(m_list_a.end(), m_list_b.begin(), m_list_b.end());
            m_list_b.clear();
        }
        m_picking_b      = false;
        m_warning        = MeshBooleanWarning::None;
        m_operation_mode = mode;
        m_parent.set_as_dirty();
    };

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0);
    if (selectable(_u8L("Union"), m_operation_mode == MeshBooleanOperation::Union, ImVec2(max_tab_length, 0.0f)))
        set_mode(MeshBooleanOperation::Union);
    ImGui::SameLine(0, 0);
    if (selectable(_u8L("Difference"), m_operation_mode == MeshBooleanOperation::Difference, ImVec2(max_tab_length, 0.0f)))
        set_mode(MeshBooleanOperation::Difference);
    ImGui::SameLine(0, 0);
    if (selectable(_u8L("Intersection"), m_operation_mode == MeshBooleanOperation::Intersection, ImVec2(max_tab_length, 0.0f)))
        set_mode(MeshBooleanOperation::Intersection);
    ImGui::PopStyleVar();

    const bool objects = m_target == MeshBooleanTarget::Object;
    m_imgui->text_wrapped(objects ? _u8L("Click objects in the 3D view to add or remove them.") :
                                    _u8L("Click parts in the 3D view to add or remove them."),
                          wrap_width);

    bool lists_changed = false;
    if (uses_two_lists()) {
        // The highlighted Select button is the list that clicks in the 3D view go to.
        const int select_btn_length = 2 * ImGui::GetStyle().FramePadding.x + ImGui::CalcTextSize(_u8L("Select").c_str()).x;
        auto      list_header       = [&](const std::string &caption, bool is_b, const char *id) {
            ImGui::AlignTextToFramePadding();
            m_imgui->text(caption);
            ImGui::SameLine();
            if (selectable(_u8L("Select") + id, m_picking_b == is_b, ImVec2(select_btn_length, 0)))
                m_picking_b = is_b;
        };
        list_header(_u8L("Subtract from"), false, "##select_a");
        lists_changed |= render_list(m_list_a, "list_a");
        list_header(_u8L("Subtract with"), true, "##select_b");
        lists_changed |= render_list(m_list_b, "list_b");
        m_imgui->bbl_checkbox(_L("Delete input"), m_diff_delete_input);
    } else {
        m_imgui->text(objects ? _u8L("Objects") : _u8L("Parts"));
        lists_changed |= render_list(m_list_a, "list_a");
        if (!m_list_a.empty())
            m_imgui->text_wrapped(objects ? _u8L("The result replaces the first object and takes its settings.") :
                                            _u8L("The result replaces the first part and takes its settings."),
                                  wrap_width);
        if (m_operation_mode == MeshBooleanOperation::Intersection)
            m_imgui->bbl_checkbox(_L("Delete input"), m_inter_delete_input);
    }
    if (lists_changed) {
        m_warning = MeshBooleanWarning::None;
        m_parent.set_as_dirty();
    }

    wxString label;
    switch (m_operation_mode) {
    case MeshBooleanOperation::Union: label = _L("Union"); break;
    case MeshBooleanOperation::Difference: label = _L("Difference"); break;
    case MeshBooleanOperation::Intersection: label = _L("Intersection"); break;
    }
    if (operate_button(label + "##btn", can_execute()))
        execute();

    if (m_warning != MeshBooleanWarning::None) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_width);
        m_imgui->warning_text(warning_message(m_warning, m_target).c_str());
        ImGui::PopTextWrapPos();
    }

    float win_w = ImGui::GetWindowWidth();
    if (last_w != win_w || last_y != y) {
        // ask canvas for another frame to render the window in the correct position
        m_imgui->set_requires_extra_frame();
        m_parent.set_as_dirty();
        m_parent.request_extra_frame();
        if (last_w != win_w)
            last_w = win_w;
        if (last_y != y)
            last_y = y;
    }

    GizmoImguiEnd();
    ImGuiWrapper::pop_toolbar_style();
}

void GLGizmoMeshBoolean::on_load(cereal::BinaryInputArchive &ar)
{
    std::vector<size_t> list_a, list_b;
    ar(m_operation_mode, m_diff_delete_input, m_inter_delete_input, m_picking_b, list_a, list_b);
    m_list_a.assign(list_a.begin(), list_a.end());
    m_list_b.assign(list_b.begin(), list_b.end());
}

void GLGizmoMeshBoolean::on_save(cereal::BinaryOutputArchive &ar) const
{
    std::vector<size_t> list_a, list_b;
    for (const ObjectID &id : m_list_a)
        list_a.push_back(id.id);
    for (const ObjectID &id : m_list_b)
        list_b.push_back(id.id);
    ar(m_operation_mode, m_diff_delete_input, m_inter_delete_input, m_picking_b, list_a, list_b);
}

} // namespace GUI
} // namespace Slic3r
