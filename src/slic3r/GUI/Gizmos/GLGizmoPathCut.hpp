#pragma once

#include "GLGizmoBase.hpp"
#include "GLGizmosCommon.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "libslic3r/ObjectID.hpp"
#include "libslic3r/PathCutConnectors.hpp"
#include "libslic3r/PathCutGeometry.hpp"
#include "libslic3r/Point.hpp"

#include <wx/event.h>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r {
class ModelObject;
class AABBMesh;
namespace GUI {

struct PathCutJob;

// Cut an object along a user-drawn path instead of a plane.
//  * View path: pick points on the object from one view; the smoothed path is extruded along the
//    view direction and splits the whole object (e.g. separate a head along the hair line).
//  * Loop: pick points around a limb on its surface; a curved cap spanning the loop detaches only
//    that region (e.g. an arm or a leg), following a non-planar loop.
// The geometry and the cut itself live in libslic3r/PathCutGeometry.hpp; the cut runs as a background job.
class GLGizmoPathCut : public GLGizmoBase
{
public:
    enum class Mode : int { ViewPath = 0, Loop = 1 };

    GLGizmoPathCut(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id);

    bool on_mouse(const wxMouseEvent &mouse_event) override;
    std::string get_tooltip() const override;
    // Keyboard (from GLGizmosManager): Enter computes the preview, Esc goes back to editing.
    // Return false when the key is not used here (Esc then closes the tool, as before).
    bool on_enter_key();
    bool on_escape_key();

protected:
    bool               on_init() override;
    std::string        on_get_name() const override;
    bool               on_is_activable() const override;
    void               on_render() override;
    void               on_set_state() override;
    CommonGizmosDataID on_get_requirements() const override;
    void               on_render_input_window(float x, float y, float bottom_limit) override;

private:
    bool raycast(const Vec2d &mouse_pos, Vec3d &hit_world) const;
    void reset_path();
    void rebuild_preview();
    void perform_cut();
    std::vector<Vec3d> preview_curve() const;
    void               update_loop_points();
    std::vector<Vec3d> cut_points() const;
    Vec3d              sweep_dir() const;
    PathCut::ViewLimits view_limits() const;
    void               add_point(const Vec3d &hit);
    void               apply_cut(std::shared_ptr<PathCutJob> job); // by value: callers pass m_pending
    void               do_apply_cut(const std::shared_ptr<PathCutJob> &job);
    void               cut_failed();
    void               clear_pending();
    void               rebuild_cutter_preview();
    // Manual connectors: after the cut, the cut face is shown and each click on it places a connector.
    void               start_placing(const std::shared_ptr<PathCutJob> &job);
    void               rebuild_face();
    bool               raycast_face(const Vec2d &mouse_pos, Vec3d &hit_world) const;
    void               add_connector(const Vec3d &hit_world);
    void               connectors_changed(); // connector settings edited while placing
    void               rebuild_connector_preview();
    // While placing, the connector under the mouse is shown as it would be placed: green where it
    // fits, red where it does not (a click there places nothing). Its tooltip says why.
    void               update_hover(const Vec2d &mouse_pos);
    bool               force_connector(PathCut::ConnectorFit why) const;
    // The direction orb while placing connectors (see the .cpp).
    std::vector<Vec3d> orb_ring(int ring) const;
    int                orb_pick(const Vec2d &mouse_pos) const;
    bool               orb_plane_dir(const Vec2d &mouse_pos, int ring, Vec3d &dir) const;
    bool               on_orb_mouse(const wxMouseEvent &mouse_event, const Vec2d &mouse_pos);
    void               rebuild_orb();
    void               render_orb();
    std::string        orb_tooltip() const;
    void               clear_hover();
    // Cuts already made on an object still in one piece (multi-cut): their faces and where it is still
    // connected, world coordinates, shown while the next cut is marked.
    void               rebuild_done();
    void               sync_done();
    void               render_done();
    // Preview: the cut is computed and shown with the pieces apart; the object is untouched until
    // "Perform cut" applies exactly what is shown. Editing (any change) drops it.
    void               enter_preview(const std::shared_ptr<PathCutJob> &job);
    void               rebuild_pieces_preview();
    void               back_to_edit();
    bool               previewing() const { return m_pending != nullptr && !m_placing; }
    void               show_object(bool show);

    Mode               m_mode{Mode::ViewPath};
    std::vector<Vec3d> m_points;                 // picked points, world coordinates
    Vec3d              m_view_dir{Vec3d::UnitY()}; // locked at the first point (view path mode)
    float              m_kerf{0.2f};             // clearance between parts [mm]
    float              m_margin{1.5f};           // loop: how far the cut may run from the loop (exact) / min. cap reach past it (flat) [mm]
    bool               m_flat_cap{false};        // loop: flat cut face through the mean plane (a slab) instead of exactly along the loop
    bool               m_follow_creases{true};   // loop: follow the model's creases between the picked points
    std::vector<Vec3d> m_loop_pts;               // ... that loop, world coordinates (empty: use the picked points)
    std::shared_ptr<PathCut::CreaseGraph> m_creases; // edges of the object and how sharply it bends across them
    const ModelObject *m_creases_object{nullptr};
    size_t             m_creases_tris{0};
    Transform3d        m_creases_matrix{Transform3d::Identity()};
    enum class Sweep : int { View = 0, Level = 1, Vertical = 2, Custom = 3 };
    Sweep              m_sweep{Sweep::View};     // view path: direction the path is swept along
    float              m_turn{0.f}, m_tilt{0.f}; // custom sweep: around the vertical axis / below horizontal [deg]
    // View path: optional limits of the wall [mm]: in front of / behind the picked points along the sweep,
    // past the first / last point along the path. Unchecked = through the whole object.
    struct Limit { bool on{false}; float mm{3.f}; };
    Limit              m_lim_front, m_lim_back{false, 20.f}, m_lim_start, m_lim_end;
    // Straight connectors across the cut face (pegs into holes, or separate pins), placed automatically.
    PathCut::ConnectorParams m_conn;
    bool               m_conn_manual{false};     // the user clicks where each connector goes
    bool               m_conn_force{false};      // ... and it goes there even where the checks say it does not fit
    std::string        m_message;
    bool               m_message_is_error{false};
    const ModelObject *m_object{nullptr};        // object the points were picked on

    GLModel m_curve;
    GLModel m_sphere;
    GLModel m_cutter;                            // translucent preview of the wall / loop cap
    GLModel m_routes_model;                      // where a cut that did not separate is still connected
    GLModel m_piece_stay, m_piece_moved;         // preview: the remaining piece and the detached one, object coordinates
    GLModel m_preview_pegs;                      // preview: the automatic connectors, on the piece they stick out of
    bool    m_pegs_on_moved{false};
    Vec3d   m_piece_shift{Vec3d::Zero()};        // preview: how far the detached piece is drawn apart
    Vec3d   m_preview_offset{Vec3d::Zero()};     // preview: object coordinates -> world
    bool    m_object_hidden{false};              // the object is hidden while its preview is shown

    // A finished cut that leaves the sides connected elsewhere: shown with its connection routes,
    // applied only on request.
    std::shared_ptr<PathCutJob>     m_pending;
    std::vector<std::vector<Vec3d>> m_routes;
    // Placing connectors by hand on the cut face of m_pending.
    bool                                m_placing{false};
    std::vector<PathCut::ConnectorSpot> m_spots;          // object coordinates (m_pending's)
    GLModel                             m_face_model;     // the cut face, object coordinates
    GLModel                             m_conn_model;     // the connectors placed so far
    std::shared_ptr<AABBMesh>           m_face_aabb;      // picks on the cut face (an open surface: first hit)
    Vec3d                               m_placing_offset{Vec3d::Zero()}; // object coordinates -> world
    GLModel                             m_hover_model;    // the connector under the mouse, object coordinates
    bool                                m_hover_on{false};
    PathCut::ConnectorFit               m_hover_fit{PathCut::ConnectorFit::Ok};
    Vec2d                               m_hover_pos{Vec2d::Zero()};
    // Faces of the cuts that left the object in one piece, and where it is still connected (world). Kept
    // while the tool is closed, for that object as it was placed then (offset, rotation and scale).
    indexed_triangle_set                m_done_faces;
    std::vector<std::vector<Vec3d>>     m_done_routes;
    ObjectID                            m_done_object;
    Transform3d                         m_done_matrix{Transform3d::Identity()};
    GLModel                             m_done_faces_model, m_done_routes_model;
    Vec3d                               m_orb_center{Vec3d::Zero()}; // object coordinates (of m_pending)
    double                              m_orb_radius{10.};
    Vec3d                               m_orb_axis{Vec3d::UnitZ()};  // the connectors' direction shown
    Vec3d                               m_orb_start_axis{Vec3d::UnitZ()}, m_orb_start_dir{Vec3d::UnitX()};
    int                                 m_orb_ring{-1}, m_orb_hover{-1}; // ring dragged / under the mouse
    GLModel                             m_orb_models[3];                 // two rings, the arrow

    // A left press on the object adds a point on release, unless the mouse was dragged: then it orbits.
    bool  m_press_on_object{false};
    bool  m_orbiting{false};
    Vec2d m_press_pos{Vec2d::Zero()}, m_last_pos{Vec2d::Zero()};
    Vec3d m_press_hit{Vec3d::Zero()};
    bool  m_press_adds_point{false};             // the press hit the part (not a modifier): a click adds a point
    bool  m_right_press{false};                  // a right click (not a drag) removes the last point
    Vec2d m_right_pos{Vec2d::Zero()};
    Transform3d m_inst_matrix{Transform3d::Identity()}; // of the instance when the points were placed
    unsigned    m_generation{0};                 // bumped when the tool closes: a cut finishing later is dropped
    std::shared_ptr<PathCutJob> m_running;       // the cut in flight, to stop it when the tool closes
    bool    m_preview_dirty{true};
    bool    m_busy{false};                     // a cut job is running
    float   m_window_height{0.f};               // of the settings window, last frame
};

} // namespace GUI
} // namespace Slic3r
