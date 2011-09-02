#ifndef slic3r_SurfaceDrag_hpp_
#define slic3r_SurfaceDrag_hpp_

#include <optional>
#include "libslic3r/Point.hpp" // Vec2d, Transform3d
#include "slic3r/Utils/RaycastManager.hpp"
#include "wx/event.h" // wxMouseEvent
#include <functional>

namespace Slic3r {
class GLVolume;
class ModelVolume;
} // namespace Slic3r

namespace Slic3r::GUI {
class GLCanvas3D;
class Selection;
class TransformationType;
struct Camera;

// Data for drag&drop over surface with mouse
struct SurfaceDrag
{
    // hold screen coor offset of cursor from object center
    Vec2d mouse_offset;

    // Start dragging text transformations to world
    Transform3d world;

    // Invers transformation of text volume instance
    // Help convert world transformation to instance space
    Transform3d instance_inv;

    // Dragged gl volume
    GLVolume *gl_volume;

    // condition for raycaster
    RaycastManager::AllowVolumes condition;

    // initial rotation in Z axis of volume
    std::optional<float> start_angle;

    // initial Z distance from surface
    std::optional<float> start_distance;

    // Flag whether coordinate hit some volume
    bool exist_hit = true;

    //  hold screen coor offset of cursor from object center without SLA shift
    Vec2d mouse_offset_without_sla_shift;
};

// Limit direction of up vector on model
// Between side and top surface
constexpr double UP_LIMIT = 0.9;

/// <summary>
/// Mouse event handler, when move(drag&drop) volume over model surface
/// NOTE: Dragged volume has to be selected. And also has to be hovered on start of dragging.
/// </summary>
/// <param name="mouse_event">Contain type of event and mouse position</param>
/// <param name="camera">Actual viewport of camera</param>
/// <param name="surface_drag">Structure which keep information about dragging</param>
/// <param name="canvas">Contain gl_volumes and selection</param>
/// <param name="raycast_manager">AABB trees for raycast in object
/// Refresh state inside of function </param>
/// <param name="up_limit">When set than use correction of up vector</param>
/// <returns>True when event is processed otherwise false</returns>
bool on_mouse_surface_drag(const wxMouseEvent         &mouse_event,
                           const Camera               &camera,
                           std::optional<SurfaceDrag> &surface_drag,
                           GLCanvas3D                 &canvas,
                           RaycastManager             &raycast_manager,
                           const std::optional<double>&up_limit = {});

/// <summary>
/// Calculate translation of volume onto surface of model
/// </summary>
/// <param name="selection">Must contain only one selected volume, Transformation of current instance</param>
/// <param name="raycast_manager">AABB trees of object. Actualize object</param>
/// <returns>Offset of volume in volume coordinate</returns>
std::optional<Vec3d> calc_surface_offset(const Selection &selection, RaycastManager &raycast_manager);

/// <summary>
/// Calculate distance by ray to surface of object in emboss direction
/// </summary>
/// <param name="gl_volume">Define embossed volume</param>
/// <param name="raycaster">Way to cast rays to object</param>
/// <param name="canvas">Contain model</param>
/// <returns>Calculated distance from surface</returns>
std::optional<float> calc_distance(const GLVolume &gl_volume, RaycastManager &raycaster, GLCanvas3D &canvas);
std::optional<float> calc_distance(const GLVolume &gl_volume, const RaycastManager &raycaster, 
    const RaycastManager::ISkip *condition, const std::optional<Slic3r::Transform3d>& fix);

/// <summary>
/// Calculate up vector angle
/// </summary>
/// <param name="selection">Calculation of angle is for selected one volume</param>
/// <returns></returns>
std::optional<float> calc_angle(const Selection &selection);

/// <summary>
/// Get transformation to world
/// - use fix after store to 3mf when exists
/// </summary>
/// <param name="gl_volume">Scene volume</param>
/// <param name="objects">To identify Model volume with fix transformation</param>
/// <returns>Fixed Transformation of gl_volume</returns>
Transform3d world_matrix_fixed(const GLVolume &gl_volume, const ModelObjectPtrs& objects);

/// <summary>
/// Get transformation to world
/// - use fix after store to 3mf when exists
/// NOTE: when not one volume selected return identity
/// </summary>
/// <param name="selection">Selected volume</param>
/// <returns>Fixed Transformation of selected volume in selection</returns>
Transform3d world_matrix_fixed(const Selection &selection);

/// <summary>
/// Wrap function around selection transformation to apply fix transformation
/// Fix transformation is needed because of (store/load) volume (to/from) 3mf
/// </summary>
/// <param name="selection">Selected gl volume will be modified</param>
/// <param name="selection_transformation_fnc">Function modified Selection transformation</param>
void selection_transform(Selection &selection, const std::function<void()>& selection_transformation_fnc);

/// <summary>
/// Apply camera direction for emboss direction
/// </summary>
/// <param name="camera">Define view vector</param>
/// <param name="canvas">Containe Selected ModelVolume to modify orientation</param>
/// <param name="wanted_up_limit">[Optional]Limit for direction of up vector</param>
/// <returns>True when apply change otherwise false</returns>
bool face_selected_volume_to_camera(const Camera &camera, GLCanvas3D &canvas, const std::optional<double> &wanted_up_limit = {});

/// <summary>
/// Rotation around z Axis(emboss direction)
/// </summary>
/// <param name="selection">Selected volume for rotation</param>
/// <param name="relative_angle">Relative angle to rotate around emboss direction</param>
void do_local_z_rotate(Selection &selection, double relative_angle);

/// <summary>
/// Translation along local z Axis (emboss direction)
/// </summary>
/// <param name="selection">Selected volume for translate</param>
/// <param name="relative_move">Relative move along emboss direction</param>
void do_local_z_move(Selection &selection, double relative_move);

/// <summary>
/// Distiguish between object and volume
/// Differ in possible transformation type
/// </summary>
/// <param name="selection">Contain selected volume/object</param>
/// <returns>Transformation to use</returns>
TransformationType get_drag_transformation_type(const Selection &selection);

/// <summary>
/// On dragging rotate gizmo func
/// Transform GLVolume from selection
/// </summary>
/// <param name="gizmo_angle">GLGizmoRotate::get_angle()</param>
/// <param name="current_angle">In/Out current angle visible in UI</param>
/// <param name="start_angle">Cache for start dragging angle</param>
/// <param name="selection">Selected only Actual embossed volume</param>
void dragging_rotate_gizmo(double gizmo_angle, std::optional<float>& current_angle, std::optional<float> &start_angle, Selection &selection);

} // namespace Slic3r::GUI
#endif // slic3r_SurfaceDrag_hpp_