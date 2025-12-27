#include "SurfaceDrag.hpp"

#include <libslic3r/Model.hpp> // ModelVolume
#include <libslic3r/Emboss.hpp>

#include "slic3r/Utils/RaycastManager.hpp"

#include "GLCanvas3D.hpp"
#include "Camera.hpp"
#include "CameraUtils.hpp"
#include "I18N.hpp"
#include "GUI_App.hpp"
#include "Gizmos/GizmoObjectManipulation.hpp"


using namespace Slic3r;
using namespace Slic3r::GUI;

namespace{
// Distance of embossed volume from surface to be represented as distance surface
// Maximal distance is also enlarge by size of emboss depth
constexpr Slic3r::MinMax<double> surface_distance_sq{1e-4, 10.}; // [in mm]

/// <summary>
/// Extract position of mouse from mouse event
/// </summary>
/// <param name="mouse_event">Event</param>
/// <returns>Position</returns>
Vec2d mouse_position(const wxMouseEvent &mouse_event);

bool start_dragging(const Vec2d                 &mouse_pos,
                    const Camera                &camera,
                    std::optional<SurfaceDrag>  &surface_drag,
                    GLCanvas3D                  &canvas,
                    RaycastManager              &raycast_manager,
                    const std::optional<double> &up_limit);

bool dragging(const Vec2d                 &mouse_pos,
              const Camera                &camera,
              SurfaceDrag                 &surface_drag, // need to write whether exist hit
              GLCanvas3D                  &canvas,
              const RaycastManager        &raycast_manager,
              const std::optional<double> &up_limit);

Transform3d get_volume_transformation(
    Transform3d world, // from volume
    const Vec3d& world_dir, // wanted new direction
    const Vec3d& world_position, // wanted new position
    const std::optional<Transform3d>& fix, // [optional] fix matrix
    // Invers transformation of text volume instance
    // Help convert world transformation to instance space 
    const Transform3d& instance_inv,
    // initial rotation in Z axis
    std::optional<float> current_angle = {},    
    const std::optional<double> &up_limit = {}); 

// distinguish between transformation of volume inside object 
// and object(single full instance with one volume)
bool is_embossed_object(const Selection &selection);

/// <summary>
/// Get fix transformation for selected volume
/// Fix after store to 3mf
/// </summary>
/// <param name="selection">Select only wanted volume</param>
/// <returns>Pointer on fix transformation from ModelVolume when exists otherwise nullptr</returns>
const Transform3d *get_fix_transformation(const Selection &selection);
}

namespace Slic3r::GUI {
 // Calculate scale in world for check in debug
[[maybe_unused]] static std::optional<double> calc_scale(const Matrix3d &from, const Matrix3d &to, const Vec3d &dir)
{
    Vec3d  from_dir      = from * dir;
    Vec3d  to_dir        = to * dir;
    double from_scale_sq = from_dir.squaredNorm();
    double to_scale_sq   = to_dir.squaredNorm();
    if (is_approx(from_scale_sq, to_scale_sq, 1e-3))
        return {}; // no scale
    return sqrt(from_scale_sq / to_scale_sq);
}

bool on_mouse_surface_drag(const wxMouseEvent         &mouse_event,
                           const Camera               &camera,
                           std::optional<SurfaceDrag> &surface_drag,
                           GLCanvas3D                 &canvas,
                           RaycastManager             &raycast_manager,
                           const std::optional<double>&up_limit)
{
    // Fix when leave window during dragging
    // Fix when click right button
    if (surface_drag.has_value() && !mouse_event.Dragging()) {
        // write transformation from UI into model
        canvas.do_move(L("Move over surface"));
        wxGetApp().obj_manipul()->set_dirty();

        // allow moving with object again
        canvas.enable_moving(true);
        canvas.enable_picking(true);
        surface_drag.reset();

        // only left up is correct
        // otherwise it is fix state and return false
        return mouse_event.LeftUp();
    }

    if (mouse_event.Moving())
        return false;

    if (mouse_event.LeftDown())
        return start_dragging(mouse_position(mouse_event), camera, surface_drag, canvas, raycast_manager, up_limit);

    // Dragging starts out of window
    if (!surface_drag.has_value())
        return false;

    if (mouse_event.Dragging())
        return dragging(mouse_position(mouse_event), camera, *surface_drag, canvas, raycast_manager, up_limit);

    return false;
}

std::optional<Vec3d> calc_surface_offset(const Selection &selection, RaycastManager &raycast_manager) {
    const GLVolume *gl_volume_ptr = get_selected_gl_volume(selection);
    if (gl_volume_ptr == nullptr)
        return {};
    const GLVolume& gl_volume = *gl_volume_ptr;

    const ModelObjectPtrs &objects = selection.get_model()->objects;
    const ModelVolume* volume = get_model_volume(gl_volume, objects);
    if (volume == nullptr)
        return {};

    const ModelInstance* instance = get_model_instance(gl_volume, objects);
    if (instance == nullptr)
        return {};

    // Move object on surface
    auto cond = RaycastManager::SkipVolume(volume->id().id);
    raycast_manager.actualize(*instance, &cond);

    Transform3d to_world = world_matrix_fixed(gl_volume, objects);
    Vec3d point = to_world.translation();
    Vec3d dir = -get_z_base(to_world);
    // ray in direction of text projection(from volume zero to z-dir)
    std::optional<RaycastManager::Hit> hit_opt = raycast_manager.closest_hit(point, dir, &cond);

    // Try to find closest point when no hit object in emboss direction
    if (!hit_opt.has_value()) {
        std::optional<RaycastManager::ClosePoint> close_point_opt = raycast_manager.closest(point);

        // It should NOT appear. Closest point always exists.
        assert(close_point_opt.has_value());
        if (!close_point_opt.has_value())
            return {};

        // It is no neccesary to move with origin by very small value
        if (close_point_opt->squared_distance < EPSILON)
            return {};

        const RaycastManager::ClosePoint &close_point = *close_point_opt;
        Transform3d hit_tr = raycast_manager.get_transformation(close_point.tr_key);
        Vec3d    hit_world = hit_tr * close_point.point;
        Vec3d offset_world = hit_world - point; // vector in world
        Vec3d offset_volume = to_world.inverse().linear() * offset_world;
        return offset_volume;
    }

    // It is no neccesary to move with origin by very small value
    const RaycastManager::Hit &hit = *hit_opt;
    if (hit.squared_distance < EPSILON)
        return {};
    Transform3d hit_tr = raycast_manager.get_transformation(hit.tr_key);
    Vec3d hit_world    = hit_tr * hit.position;
    Vec3d offset_world = hit_world - point; // vector in world
    // TIP: It should be close to only z move
    Vec3d offset_volume = to_world.inverse().linear() * offset_world;
    return offset_volume;
}

std::optional<float> calc_distance(const GLVolume &gl_volume, RaycastManager &raycaster, GLCanvas3D &canvas)
{
    const ModelObject *object = get_model_object(gl_volume, canvas.get_model()->objects);
    assert(object != nullptr);
    if (object == nullptr)
        return {};

    const ModelInstance *instance = get_model_instance(gl_volume, *object);
    const ModelVolume   *volume   = get_model_volume(gl_volume, *object);
    assert(instance != nullptr && volume != nullptr);
    if (object == nullptr || instance == nullptr || volume == nullptr)
        return {};

    if (volume->is_the_only_one_part())
        return {};

    if (!volume->emboss_shape.has_value())
        return {};
        
    RaycastManager::AllowVolumes condition = create_condition(object->volumes, volume->id());
    RaycastManager::Meshes meshes = create_meshes(canvas, condition);
    raycaster.actualize(*instance, &condition, &meshes);
    return calc_distance(gl_volume, raycaster, &condition, volume->emboss_shape->fix_3mf_tr);
}

std::optional<float> calc_distance(const GLVolume &gl_volume, const RaycastManager &raycaster, 
    const RaycastManager::ISkip *condition, const std::optional<Slic3r::Transform3d>& fix) {
    Transform3d w = gl_volume.world_matrix();
    if (fix.has_value())
        w = w * fix->inverse();
    Vec3d p = w.translation();
    Vec3d dir = -get_z_base(w);
    auto hit_opt = raycaster.closest_hit(p, dir, condition);
    if (!hit_opt.has_value())
        return {};

    const RaycastManager::Hit &hit = *hit_opt;
    // NOTE: hit.squared_distance is in volume space not world

    const Transform3d &tr = raycaster.get_transformation(hit.tr_key);
    Vec3d hit_world = tr * hit.position;
    Vec3d p_to_hit = hit_world - p;
    double distance_sq = p_to_hit.squaredNorm();

    // too small distance is calculated as zero distance
    if (distance_sq < ::surface_distance_sq.min)
        return {};

    // check maximal distance
    const BoundingBoxf3& bb = gl_volume.bounding_box();
    double max_squared_distance = std::max(std::pow(2 * bb.size().z(), 2), ::surface_distance_sq.max);
    if (distance_sq > max_squared_distance)
        return {};   
    
    // calculate sign
    float sign = (p_to_hit.dot(dir) > 0)? 1.f : -1.f;

    // distiguish sign
    return sign * static_cast<float>(sqrt(distance_sq));
}

std::optional<float> calc_angle(const Selection &selection)
{
    const GLVolume *gl_volume = selection.get_first_volume();
    assert(gl_volume != nullptr);
    if (gl_volume == nullptr)
        return {};

    Transform3d to_world = gl_volume->world_matrix();
    const ModelVolume *volume = get_model_volume(*gl_volume, selection.get_model()->objects);
    assert(volume != nullptr);
    assert(volume->emboss_shape.has_value());
    if (volume == nullptr || !volume->emboss_shape.has_value() || !volume->emboss_shape->fix_3mf_tr)
        return Emboss::calc_up(to_world, UP_LIMIT);

    // exist fix matrix and must be applied before calculation
    to_world = to_world * volume->emboss_shape->fix_3mf_tr->inverse();
    return Emboss::calc_up(to_world, UP_LIMIT);
}

Transform3d world_matrix_fixed(const GLVolume &gl_volume, const ModelObjectPtrs &objects)
{
    Transform3d res = gl_volume.world_matrix();

    const ModelVolume *mv = get_model_volume(gl_volume, objects);
    if (!mv)
        return res;

    const std::optional<EmbossShape> &es = mv->emboss_shape;
    if (!es.has_value())
        return res;

    const std::optional<Transform3d> &fix = es->fix_3mf_tr;
    if (!fix.has_value())
        return res;

    return res * fix->inverse();
}

Transform3d world_matrix_fixed(const Selection &selection)
{
    const GLVolume *gl_volume = get_selected_gl_volume(selection);
    assert(gl_volume != nullptr);
    if (gl_volume == nullptr)
        return Transform3d::Identity();

    return world_matrix_fixed(*gl_volume, selection.get_model()->objects);
}

void selection_transform(Selection &selection, const std::function<void()> &selection_transformation_fnc)
{   
    if (const Transform3d *fix = get_fix_transformation(selection); fix != nullptr) {        
        // NOTE: need editable gl volume .. can't use selection.get_first_volume()
        GLVolume *gl_volume = selection.get_volume(*selection.get_volume_idxs().begin());
        Transform3d volume_tr = gl_volume->get_volume_transformation().get_matrix();
        gl_volume->set_volume_transformation(volume_tr * fix->inverse());
        selection.setup_cache();

        selection_transformation_fnc();

        volume_tr = gl_volume->get_volume_transformation().get_matrix();
        gl_volume->set_volume_transformation(volume_tr * (*fix));
        selection.setup_cache();
    } else {
        selection_transformation_fnc();
    }

    if (selection.is_single_full_instance())
        selection.synchronize_unselected_instances(Selection::SyncRotationType::GENERAL);
}

bool face_selected_volume_to_camera(const Camera &camera, GLCanvas3D &canvas, const std::optional<double> &wanted_up_limit)
{
    GLVolume *gl_volume_ptr = get_selected_gl_volume(canvas);
    if (gl_volume_ptr == nullptr)
        return false;
    GLVolume &gl_volume = *gl_volume_ptr;

    const ModelObjectPtrs &objects = canvas.get_model()->objects;
    ModelObject *object_ptr = get_model_object(gl_volume, objects);
    assert(object_ptr != nullptr);
    if (object_ptr == nullptr)
        return false;
    ModelObject &object = *object_ptr;

    const ModelInstance *instance_ptr = get_model_instance(gl_volume, object);
    assert(instance_ptr != nullptr);
    if (instance_ptr == nullptr)
        return false;
    const ModelInstance &instance = *instance_ptr;

    ModelVolume *volume_ptr = get_model_volume(gl_volume, object);
    assert(volume_ptr != nullptr);
    if (volume_ptr == nullptr)
        return false;
    ModelVolume &volume = *volume_ptr;

    // Calculate new volume transformation
    Transform3d volume_tr = volume.get_matrix();
    std::optional<Transform3d> fix;
    if (volume.emboss_shape.has_value()) {
        fix = volume.emboss_shape->fix_3mf_tr;
        if (fix.has_value())
            volume_tr = volume_tr * fix->inverse();
    }

    Transform3d instance_tr     = instance.get_matrix();
    Transform3d instance_tr_inv = instance_tr.inverse();
    Transform3d world_tr        = instance_tr * volume_tr; // without sla !!!
    std::optional<float> current_angle;
    if (wanted_up_limit.has_value())
        current_angle = Emboss::calc_up(world_tr, *wanted_up_limit);

    Vec3d world_position = gl_volume.world_matrix()*Vec3d::Zero();

    assert(camera.get_type() == Camera::EType::Perspective || 
           camera.get_type() == Camera::EType::Ortho);
    Vec3d wanted_direction = (camera.get_type() == Camera::EType::Perspective) ?
        Vec3d(camera.get_position() - world_position) : 
        (-camera.get_dir_forward());
    
    Transform3d new_volume_tr = get_volume_transformation(world_tr, wanted_direction, world_position,
        fix, instance_tr_inv, current_angle, wanted_up_limit);

    Selection &selection = canvas.get_selection();
    if (is_embossed_object(selection)) {
        // transform instance instead of volume
        Transform3d new_instance_tr = instance_tr * new_volume_tr * volume.get_matrix().inverse();        
        gl_volume.set_instance_transformation(new_instance_tr);
        
        // set same transformation to other instances when instance is embossed object
        if (selection.is_single_full_instance()) 
            selection.synchronize_unselected_instances(Selection::SyncRotationType::GENERAL);
    } else {
        // write result transformation
        gl_volume.set_volume_transformation(new_volume_tr);
    }

    if (volume.type() == ModelVolumeType::MODEL_PART) {
        object.invalidate_bounding_box();
        object.ensure_on_bed();
    }

    canvas.do_rotate(L("Face the camera"));
    wxGetApp().obj_manipul()->set_dirty();
    return true;
}

void do_local_z_rotate(Selection &selection, double relative_angle) {
    assert(!selection.is_empty());
    if(selection.is_empty()) return;

    bool is_single_volume = selection.volumes_count() == 1;
    assert(is_single_volume);
    if (!is_single_volume) return;

    // Fix angle for mirrored volume
    bool is_mirrored = false;
    const GLVolume* gl_volume = selection.get_first_volume();
    if (gl_volume != nullptr) {
        const ModelInstance *instance = get_model_instance(*gl_volume, selection.get_model()->objects);
        bool is_instance_mirrored = (instance != nullptr)? has_reflection(instance->get_matrix()) : false;
        if (is_embossed_object(selection)) {
                is_mirrored = is_instance_mirrored;
        } else {
            const ModelVolume *volume = get_model_volume(*gl_volume, selection.get_model()->objects);
            if (volume != nullptr)
                is_mirrored = is_instance_mirrored != has_reflection(volume->get_matrix());
        }
    }
    if (is_mirrored)
        relative_angle *= -1;

    selection.setup_cache();
    auto selection_rotate_fnc = [&selection, &relative_angle](){
        selection.rotate(Vec3d(0., 0., relative_angle), get_drag_transformation_type(selection));
    };
    selection_transform(selection, selection_rotate_fnc);
}

void do_local_z_move(Selection &selection, double relative_move) {
    assert(!selection.is_empty());
    if (selection.is_empty()) return;

    selection.setup_cache();
    auto selection_translate_fnc = [&selection, relative_move]() {
        Vec3d translate = Vec3d::UnitZ() * relative_move;
        selection.translate(translate, TransformationType::Local);
    };
    selection_transform(selection, selection_translate_fnc);
}

TransformationType get_drag_transformation_type(const Selection &selection)
{
    return is_embossed_object(selection) ?
        TransformationType::Instance_Relative_Joint : 
        TransformationType::Local_Relative_Joint;
}

void dragging_rotate_gizmo(double gizmo_angle, std::optional<float>& current_angle, std::optional<float> &start_angle, Selection &selection)
{
    if (!start_angle.has_value())
        // create cache for initial angle
        start_angle = current_angle.value_or(0.f);

    gizmo_angle -= PI / 2; // Grabber is upward

    double new_angle = gizmo_angle + *start_angle;

    const GLVolume *gl_volume = selection.get_first_volume();
    assert(gl_volume != nullptr);
    if (gl_volume == nullptr)
        return;

    bool is_volume_mirrored   = has_reflection(gl_volume->get_volume_transformation().get_matrix());
    bool is_instance_mirrored = has_reflection(gl_volume->get_instance_transformation().get_matrix());
    if (is_volume_mirrored != is_instance_mirrored)
        new_angle = -gizmo_angle + *start_angle;

    // move to range <-M_PI, M_PI>
    Geometry::to_range_pi_pi(new_angle);

    const Transform3d* fix = get_fix_transformation(selection);
    double z_rotation = (fix!=nullptr) ? (new_angle - current_angle.value_or(0.f)) : // relative angle
                                         gizmo_angle; // relativity is keep by selection cache

    auto selection_rotate_fnc = [z_rotation, &selection]() {
        selection.rotate(Vec3d(0., 0., z_rotation), get_drag_transformation_type(selection));
    };
    selection_transform(selection, selection_rotate_fnc);

    // propagate angle into property
    current_angle = static_cast<float>(new_angle);

    // do not store zero
    if (is_approx(*current_angle, 0.f))
        current_angle.reset();
}

} // namespace Slic3r::GUI

// private implementation
namespace {

Vec2d mouse_position(const wxMouseEvent &mouse_event){
    // wxCoord == int --> wx/types.h
    Vec2i32 mouse_coord(mouse_event.GetX(), mouse_event.GetY());
    return mouse_coord.cast<double>();
}

bool start_dragging(const Vec2d                &mouse_pos,
                    const Camera               &camera,
                    std::optional<SurfaceDrag> &surface_drag,
                    GLCanvas3D                 &canvas,
                    RaycastManager             &raycast_manager,
                    const std::optional<double>&up_limit)
{
    // selected volume
    GLVolume *gl_volume_ptr = get_selected_gl_volume(canvas);
    if (gl_volume_ptr == nullptr)
        return false;
    const GLVolume &gl_volume = *gl_volume_ptr;

    // is selected volume closest hovered?
    const GLVolumePtrs &gl_volumes = canvas.get_volumes().volumes;
    if (int hovered_idx = canvas.get_first_hover_volume_idx(); hovered_idx < 0)
        return false;
    else if (auto hovered_idx_ = static_cast<size_t>(hovered_idx);
             hovered_idx_ >= gl_volumes.size() || gl_volumes[hovered_idx_] != gl_volume_ptr)
        return false;

    const ModelObjectPtrs &objects = canvas.get_model()->objects;
    const ModelObject     *object  = get_model_object(gl_volume, objects);
    assert(object != nullptr);
    if (object == nullptr)
        return false;

    const ModelInstance *instance = get_model_instance(gl_volume, *object);
    const ModelVolume   *volume   = get_model_volume(gl_volume, *object);
    assert(instance != nullptr && volume != nullptr);
    if (object == nullptr || instance == nullptr || volume == nullptr)
        return false;

    // allowed drag&drop by canvas for object
    if (volume->is_the_only_one_part())
        return false;

    RaycastManager::AllowVolumes condition = create_condition(object->volumes, volume->id());
    RaycastManager::Meshes meshes = create_meshes(canvas, condition);
    // initialize raycasters
    // INFO: It could slows down for big objects
    // (may be move to thread and do not show drag until it finish)
    raycast_manager.actualize(*instance, &condition, &meshes);

    // world_matrix_fixed() without sla shift
    Transform3d to_world = world_matrix_fixed(gl_volume, objects);

    // zero point of volume in world coordinate system
    Vec3d volume_center = to_world.translation();
    // screen coordinate of volume center
    auto coor                           = CameraUtils::project(camera, volume_center);
    Vec2d mouse_offset                   = coor.cast<double>() - mouse_pos;
    Vec2d mouse_offset_without_sla_shift = mouse_offset;
    if (double sla_shift = gl_volume.get_sla_shift_z(); !is_approx(sla_shift, 0.)) {
        Transform3d to_world_without_sla_move = instance->get_matrix() * volume->get_matrix();
        if (volume->emboss_shape.has_value() && volume->emboss_shape->fix_3mf_tr.has_value())
            to_world_without_sla_move = to_world_without_sla_move * (*volume->emboss_shape->fix_3mf_tr);
        // zero point of volume in world coordinate system
        volume_center = to_world_without_sla_move.translation();
        // screen coordinate of volume center
        coor                           = CameraUtils::project(camera, volume_center);
        mouse_offset_without_sla_shift = coor.cast<double>() - mouse_pos;
    }

    Transform3d volume_tr = gl_volume.get_volume_transformation().get_matrix();

    // fix baked transformation from .3mf store process
    if (const std::optional<EmbossShape> &es_opt = volume->emboss_shape; es_opt.has_value()) {
        const std::optional<Slic3r::Transform3d> &fix = es_opt->fix_3mf_tr;
        if (fix.has_value())
            volume_tr = volume_tr * fix->inverse();
    }

    Transform3d instance_tr     = instance->get_matrix();
    Transform3d instance_tr_inv = instance_tr.inverse();
    Transform3d world_tr        = instance_tr * volume_tr;
    std::optional<float> start_angle;
    if (up_limit.has_value()) {
        start_angle = Emboss::calc_up(world_tr, *up_limit);
        if (start_angle.has_value() && has_reflection(world_tr))
            start_angle = -(*start_angle);
    }

    std::optional<float> start_distance;
    if (!volume->emboss_shape->projection.use_surface)
        start_distance = calc_distance(gl_volume, raycast_manager, &condition, volume->emboss_shape->fix_3mf_tr);
    surface_drag = SurfaceDrag{mouse_offset,   world_tr,  instance_tr_inv,
                               gl_volume_ptr,  condition, start_angle,
                               start_distance, true,      mouse_offset_without_sla_shift};

    // disable moving with object by mouse
    canvas.enable_moving(false);
    canvas.enable_picking(false);
    return true;
}

Transform3d get_volume_transformation(
    Transform3d world, // from volume
    const Vec3d& world_dir, // wanted new direction
    const Vec3d& world_position, // wanted new position
    const std::optional<Transform3d>& fix, // [optional] fix matrix
    // Invers transformation of text volume instance
    // Help convert world transformation to instance space 
    const Transform3d& instance_inv,
    // initial rotation in Z axis
    std::optional<float> current_angle,    
    const std::optional<double> &up_limit) 
{
    auto world_linear = world.linear().eval();
    // Calculate offset: transformation to wanted position
    {
        // Reset skew of the text Z axis:
        // Project the old Z axis into a new Z axis, which is perpendicular to the old XY plane.
        Vec3d old_z         = world_linear.col(2);
        Vec3d new_z         = world_linear.col(0).cross(world_linear.col(1));
        world_linear.col(2) = new_z * (old_z.dot(new_z) / new_z.squaredNorm());
    }

    Vec3d       text_z_world     = world_linear.col(2); // world_linear * Vec3d::UnitZ()
    auto        z_rotation       = Eigen::Quaternion<double, Eigen::DontAlign>::FromTwoVectors(text_z_world, world_dir);
    Transform3d world_new        = z_rotation * world;
    auto        world_new_linear = world_new.linear().eval();

    // Fix direction of up vector to zero initial rotation
    if(up_limit.has_value()){
        Vec3d z_world = world_new_linear.col(2);
        z_world.normalize();
        Vec3d wanted_up = Emboss::suggest_up(z_world, *up_limit);

        Vec3d y_world    = world_new_linear.col(1);
        auto  y_rotation = Eigen::Quaternion<double, Eigen::DontAlign>::FromTwoVectors(y_world, wanted_up);

        world_new        = y_rotation * world_new;
        world_new_linear = world_new.linear();
    }
    
    // Edit position from right
    Transform3d volume_new{Eigen::Translation<double, 3>(instance_inv * world_position)};
    volume_new.linear() = instance_inv.linear() * world_new_linear;

    // Check that transformation matrix is valid transformation
    assert(volume_new.matrix()(0, 0) == volume_new.matrix()(0, 0)); // Check valid transformation not a NAN
    if (volume_new.matrix()(0, 0) != volume_new.matrix()(0, 0))
        return Transform3d::Identity();

    // Check that scale in world did not changed
    assert(!calc_scale(world_linear, world_new_linear, Vec3d::UnitY()).has_value());
    assert(!calc_scale(world_linear, world_new_linear, Vec3d::UnitZ()).has_value());

    // fix baked transformation from .3mf store process
    if (fix.has_value())
        volume_new = volume_new * (*fix);

    // apply move in Z direction and rotation by up vector
    Emboss::apply_transformation(current_angle, {}, volume_new);

    return volume_new;    
}

bool dragging(const Vec2d                 &mouse_pos,
              const Camera                &camera,
              SurfaceDrag                 &surface_drag,
              GLCanvas3D                  &canvas,
              const RaycastManager        &raycast_manager,
              const std::optional<double> &up_limit)
{
    Vec2d offseted_mouse = mouse_pos + surface_drag.mouse_offset_without_sla_shift;
    std::optional<RaycastManager::Hit> hit = ray_from_camera(
        raycast_manager, offseted_mouse, camera, &surface_drag.condition);

    surface_drag.exist_hit = hit.has_value();
    if (!hit.has_value()) {
        // cross hair need redraw
        canvas.set_as_dirty();
        return true;
    }

    const ModelVolume *volume = get_model_volume(*surface_drag.gl_volume, canvas.get_model()->objects);
    std::optional<Transform3d> fix;
    if (volume !=nullptr && 
        volume->emboss_shape.has_value() && 
        volume->emboss_shape->fix_3mf_tr.has_value())
        fix = volume->emboss_shape->fix_3mf_tr;
    Transform3d volume_new = get_volume_transformation(surface_drag.world, hit->normal, hit->position,
        fix, surface_drag.instance_inv, surface_drag.start_angle, up_limit);

    // Update transformation for all instances
    for (GLVolume *vol : canvas.get_volumes().volumes) {
        if (vol->object_idx() != surface_drag.gl_volume->object_idx() || 
            vol->volume_idx() != surface_drag.gl_volume->volume_idx())
            continue;
        vol->set_volume_transformation(volume_new);
    }

    canvas.set_as_dirty();
    // Show current position in manipulation panel
    wxGetApp().obj_manipul()->set_dirty();
    return true;
}

bool is_embossed_object(const Selection &selection)
{
    assert(selection.volumes_count() == 1);
    return selection.is_single_full_object() || selection.is_single_full_instance();
}

const Transform3d *get_fix_transformation(const Selection &selection) {
    const GLVolume *gl_volume = get_selected_gl_volume(selection);
    assert(gl_volume != nullptr);
    if (gl_volume == nullptr)
        return nullptr;

    const ModelVolume *volume = get_model_volume(*gl_volume, selection.get_model()->objects);
    assert(volume != nullptr);
    if (volume == nullptr)
        return nullptr;

    const std::optional<EmbossShape> &es = volume->emboss_shape;
    if (!volume->emboss_shape.has_value())
        return nullptr;
    if (!es->fix_3mf_tr.has_value())
        return nullptr;
    return &(*es->fix_3mf_tr);
}

} // namespace
