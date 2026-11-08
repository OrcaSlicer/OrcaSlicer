#ifndef slic3r_GLGizmoSkeleton_hpp_
#define slic3r_GLGizmoSkeleton_hpp_

#include "GLGizmoBase.hpp"
#include "GLGizmoCut.hpp"
#include "slic3r/GUI/GLSelectionRectangle.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/CutUtils.hpp"
#include "imgui/imgui.h"

namespace Slic3r {
enum class CutConnectorType : int;
class ModelVolume;
class GLShaderProgram;
struct CutConnectorAttributes;

namespace GUI {

class GLGizmoSkeleton : public GLGizmoCut3D
{
public:
    GLGizmoSkeleton(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
        : GLGizmoCut3D(parent, icon_filename, sprite_id)
    {
        m_place_on_cut_upper     = false;
        m_show_skeleton          = true;
        m_hide_cut_plane         = true;
        m_skel_collapse_angle    = 0.0f; // disabled — resample handles density
        m_skel_collapse_edge_len = 0.0f;
    }

protected:
    bool on_init() override
    {
        bool r = GLGizmoCut3D::on_init();
        m_shortcut.reset();   // no shortcut: only reached from its own toolbar entry
        return r;
    }

    std::string on_get_name() const override { return _u8L("Pipe Cut"); }

    // UNLAYERED_MOD — the cut tags each arm with layer_rotation_b/c, which slicing only honours
    // when the printer has enable_nonplanar set. Don't offer a cut that would do nothing.
    bool on_is_activable() const override;

    void on_set_state() override;
    void data_changed(bool is_serializing) override;
    void on_render() override;

    bool on_mouse(const wxMouseEvent& evt) override;

    void render_clipper_cut() override {} // replaced by per-edge contours in render_skeleton_cut_planes()

    // Object-local plane through the skeleton edge midpoint, facing the smoothed tangent.
    Slic3r::CutPlane make_edge_plane(int par, int child) const;
    // One-time "switch to By group?" prompt, mirrored from GLGizmoCutPlanes::add_plane().
    void prompt_by_group_sequence();
    void perform_skeleton_cut(const Selection& selection);
    void precompute_cut_contours();
    void render_skeleton_cut_planes();
    void render_node_axis_gizmo(const Vec3d& node_world, float arrow_len, const Camera& camera);
    void delete_skeleton_node(int idx);
    // Roots, DAG, tangents and cut list from m_skeleton, in WORLD space (rebuild_skeleton_topology),
    // then the green root markers. Every recompute in this gizmo goes through here.
    void rebuild_skeleton_dag();
    void set_root_markers(const std::vector<int>& roots);
    void render_cut_plane_input_window(CutConnectors& connectors, float x, float y, float bottom_limit) override;

    std::vector<std::pair<int,int>> build_cut_edges(const Transform3d& world) const;
    void rebuild_step_contour();
    void sync_skeleton_cache(); // writes m_skeleton back to s_skeleton_cache for the active object
    void apply_skeleton_result(const SkeletonResult& res) override;

    GLModel m_cut_contours_model; // cached cross-section contour lines at each skeleton edge midpoint
    GLModel m_step_contour_model; // highlighted contour for the current step-through preview edge

    // Node axis gizmo state
    int   m_selected_node{-1};        // node whose XYZ gizmo is shown (-1 = none)
    int   m_drag_axis{-1};            // 0=X 1=Y 2=Z during drag (-1 = none)
    Vec3d m_drag_node_world_start;    // world pos of node when drag began
    Vec3d m_drag_grabber_world_start; // world pos of axis arrow tip when drag began
    Vec3d m_drag_axis_dir;            // world-space unit axis direction

    // Active editing tool
    enum class SkelTool { Move, Delete, Connect };
    SkelTool m_skel_tool{SkelTool::Move};

    // Node context menu
    bool  m_node_ctx_pending{false};  // open popup on next render
    int   m_connect_source{-1};       // source node for Connect tool (-1 = awaiting first click)

    // Generator settings panel toggle
    bool  m_skel_show_generator{false};

    // The cut list, derived with the DAG in world space: the seat under a planted foot is only
    // recognisable there, so it cannot be re-derived from the object-local m_skeleton_dag.
    std::vector<std::pair<int,int>> m_skel_cut_edges;

    // Step-through preview state
    std::vector<std::pair<int,int>> m_preview_edges;
    int         m_preview_step{-1};   // -1 = not in step mode
    Transform3d m_preview_world;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoSkeleton_hpp_
