#ifndef slic3r_GLGizmoCutPlanes_hpp_
#define slic3r_GLGizmoCutPlanes_hpp_

#include "GLGizmoCut.hpp"
#include "slic3r/GUI/MeshUtils.hpp"
#include "libslic3r/CutPlaneSegmentation.hpp"

#include <memory>
#include <optional>

namespace Slic3r {

struct CutPlane;

namespace GUI {

// Declarative cut-plane authoring: edits ModelObject::cut_planes, a non-destructive per-object
// list of planes consumed at slice time. Unlike GLGizmoCut3D / GLGizmoCutChunks3D it NEVER splits
// the object.
//
// UX model (deliberately the stock Cut tool's): the gizmo always shows one freely editable plane
// with the standard grabbers and panel controls (cut position, reset, flip). "Add cut plane"
// records the current pose into the stack; selecting a stored plane (list row or clicking its
// quad) binds the gizmo to it — drags then commit back to the model on drag-end under undo
// snapshots. m_active_plane == -1 is the free "authoring a new plane" mode.
class GLGizmoCutPlanes : public GLGizmoCut3D
{
public:
    GLGizmoCutPlanes(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id)
        : GLGizmoCut3D(parent, icon_filename, sprite_id)
    {
        m_connectors_editing = false;
        m_hide_cut_plane     = false; // the editing plane is always visible, like the Cut tool
        // Keep the base-rendered editing plane close to the object's size instead of the stock
        // oversized square (stored planes get exact cross-section-fitted quads).
        m_cut_plane_radius_koef = 1.05f;
    }

protected:
    bool on_init() override
    {
        const bool r   = GLGizmoCut3D::on_init();
        m_shortcut.reset();   // no shortcut: only reached from its own toolbar entry
        return r;
    }

    std::string on_get_name() const override { return _u8L("Cut Planes"); }

    // UNLAYERED_MOD -- greyed out unless the selected printer has non-planar printing on. Cut
    // planes are meaningless otherwise: the object slices in one piece regardless of how many
    // planes are authored. Matches GLGizmoSkeleton, which gates the same way.
    bool on_is_activable() const override;

    void on_set_state() override;
    void data_changed(bool is_serializing) override;
    void on_stop_dragging() override;
    // UNLAYERED_MOD -- overridden purely to hold the plane at the printer's tilt limit while it is
    // being dragged, so it stops where it is drawn instead of snapping back when released.
    void on_dragging(const UpdateData &data) override;
    void on_render() override;
    bool on_mouse(const wxMouseEvent &mouse_event) override;
    void on_load(cereal::BinaryInputArchive &ar) override;
    void on_save(cereal::BinaryOutputArchive &ar) const override;

    // The object is never visually clipped or split by this gizmo.
    void render_clipper_cut() override {}
    // The editing plane renders as a contour-shaped surface at the current pose plus a much
    // fainter square (the infinite-plane affordance). Mid-drag only the square is shown; the
    // surface snaps to the new cut on release.
    void render_cut_plane() override;
    void perform_cut(const Selection &selection, bool surpress_diag) override { (void) selection; (void) surpress_diag; }

    void render_cut_plane_input_window(CutConnectors &connectors, float x, float y, float bottom_limit) override;
    // The base reads the object clipper, which this gizmo never renders, so its answer is stale
    // (and the "placed outside" warning wrong). Answer from the active-pose slice instead.
    bool has_valid_contour() const override { return m_active_surface_touches; }

    std::string get_gizmo_entering_text() const override { return _u8L("Entering Cut Planes gizmo"); }
    std::string get_gizmo_leaving_text() const override { return _u8L("Leaving Cut Planes gizmo"); }
    std::string get_action_snapshot_name() const override { return _u8L("Cut Planes gizmo editing"); }

public:
    std::string get_tooltip() const override;

private:
    // "Select face": while armed, the next click on the object snaps the editing plane to the
    // clicked triangle (hit point + face normal), then commits it if a stored plane is bound.
    // Works on the real merged mesh, so any face qualifies -- concave pockets included -- unlike
    // Lay on Face, which only offers convex-hull facets. Right-click or the button cancels.
    bool                           m_face_pick_mode { false };
    std::unique_ptr<MeshRaycaster> m_face_raycaster;                        // over merged_mesh(), object-local
    bool                           m_face_hover_valid { false };
    Vec3d                          m_face_hover_point  { Vec3d::Zero() };   // object-local
    Vec3d                          m_face_hover_normal { Vec3d::UnitZ() };  // object-local
    GLModel                        m_face_hover_model;                      // the hovered triangle, object-local
    bool set_face_pick_mode(bool on);
    bool update_face_hover(const Vec2d &mouse_pos);
    void align_plane_to_hovered_face();
    void render_face_hover();
    // Connected, coplanar facet groups of merged_mesh() -- a "face" in the user's sense. The hover
    // highlight covers the whole group under the pointer and its normal is what the plane snaps
    // to; the largest groups are drawn as faint hints while picking, the way Lay on Face shows
    // its candidates (which are convex-hull facets only; these come from the real mesh).
    struct FacePatch {
        std::vector<int>                 facets;
        std::vector<std::pair<int, int>> boundary;   // vertex index pairs of the patch's outline
        Vec3d                            normal { Vec3d::UnitZ() };
        double                           area { 0. };
    };
    std::vector<FacePatch> m_face_patches;
    std::vector<int>       m_facet_patch;              // facet -> patch
    int                    m_face_hover_patch { -1 };
    GLModel                m_face_hint_model;          // the largest patches, one model
    std::vector<int>       m_face_hint_patches;        // which patches the hint model holds
    GLModel                m_face_hint_outline;        // their outlines (lines)
    GLModel                m_face_hover_outline;       // the hovered patch's outline (lines)
    // How far (mm, object-local) the outline vertices sit above the surface. Follows the zoom
    // (about a pixel and a half) so the lift never reads as a gap when zoomed in; the outline
    // models are rebuilt when it changes noticeably. See render_face_hover().
    float                  m_face_outline_lift { 0.05f };
    void compute_face_patches();
    void rebuild_face_outlines();

    ModelObject *selected_model_object() const;
    // The single model part the gizmo is scoped to when one part of a multi-part object is
    // selected, else nullptr (whole object). Everything mesh-derived -- contours, the editing
    // surface, face picking and the chunk preview -- then sees that part alone, and the preview
    // hides only it. The plane stack and the object build direction stay per object.
    const ModelVolume *scoped_volume() const;
    // Full instance matrix of the selected instance (identity when unavailable). All local<->world
    // conversions must use this — NOT the offset-only shortcut of get_cut_matrix() — so planes
    // stay glued to the object under instance rotation, scale and mirror.
    Transform3d  instance_matrix() const;

    // Mirror planes[m_active_plane] (object-local) into the gizmo pose; drops to free mode (-1)
    // when the index is invalid.
    void sync_gizmo_from_active_plane();
    // Write the current gizmo pose back into planes[m_active_plane]. The caller is responsible
    // for the undo snapshot. No-op in free mode.
    void commit_gizmo_to_active_plane();

    // UNLAYERED_MOD -- the virtual-5-axis tilt limit, in degrees from vertical, that applies here.
    // 0 means there is none: not a virtual printer, or one whose gantry height nobody has set.
    //
    // Per stored plane, because the limit depends on how far the chunk seated on that plane reaches
    // (m_plane_reach); and for the whole-object build direction, where the object prints as one
    // tilted piece so its own footprint is the run.
    //
    // And per direction, because the hotend is not round: `azimuth` is the way the build normal
    // leans (tilt_azimuth()), absent = unknown, which yields the strictest direction.
    double plane_tilt_limit_deg(int plane_index, std::optional<double> azimuth = std::nullopt) const;
    double object_tilt_limit_deg(std::optional<double> azimuth = std::nullopt) const;
    // The azimuth a build normal leans toward, or none when it is vertical and leans nowhere.
    static std::optional<double> tilt_azimuth(const Vec3d &n_world);
    static std::optional<double> tilt_azimuth_bc(double b_deg, double c_deg);
    // Lean `n_world` back to `limit_deg` from vertical, keeping its azimuth. Returned unchanged
    // when it is already inside the limit, or when there is no limit.
    static Vec3d clamp_build_normal_to_tilt(const Vec3d &n_world, double limit_deg);
    // Hold the live gizmo pose at the active plane's limit. Called every drag frame.
    // True when it had to lean the pose back (and records that in m_tilt_limit_hit_*).
    bool clamp_rotation_to_tilt_limit();

    // Invalidate the current slice after a cut-plane mutation (posts a background-process event).
    void schedule_background_process();
    // Bind the gizmo to a stored plane (or -1 for free mode).
    void set_active_plane(int idx);
    // Record the current gizmo pose as a new stored plane (under a snapshot). The pose is left in
    // place so the user can shift it and add the next plane.
    void add_plane();
    void delete_plane(size_t idx);
    // Swap plane idx with its neighbor (dir = -1 up / +1 down) under a snapshot. Order matters:
    // it is the plane numbering and the tie-break key of the segmentation's base-plane choice.
    void move_plane(size_t idx, int dir);

    // Stored planes render as dimmed quads (the bound one is drawn by the base at the gizmo pose).
    void render_stored_planes();
    // Per-plane intersection contours with the object, sliced in the object-local frame and
    // rendered under the instance matrix (GLGizmoSkeleton::precompute_cut_contours pattern).
    void precompute_plane_contours();
    void render_plane_contours();
    // Pick the closest unbound stored plane quad under the mouse; -1 if none.
    int  pick_stored_plane(const Vec2d &mouse_pos) const;
    // Per-plane B/C build-direction editor (world-frame machine angles; storage stays a normal).
    void render_build_direction_editor(CutPlaneStack &stack);
    // Whole-object slice direction (the per-object layer_rotation_b/c settings), set from the
    // editing plane's normal: pick a face or pose the plane, then "Use plane as build direction".
    void render_object_build_direction();
    // Segments only print correctly under the By-group sequence; offer to switch when the user
    // authors the first thing that needs it. `message` names that thing.
    void offer_by_group_sequence(const wxString &message);

    int m_active_plane { -1 };

    GLModel m_contours_model;          // contours of unbound stored planes
    GLModel m_contours_model_active;   // contour of the bound plane (highlighted)
    bool    m_contours_dirty { true };

    // Per-plane quads fitted to the object's cross-section (+ a few mm margin) instead of one big
    // square — built in OBJECT-LOCAL coordinates in precompute_plane_contours() and rendered under
    // the instance matrix, exactly like the contours. m_plane_quad_rects holds each quad's 2D rect
    // in the plane's slice frame (for click picking).
    struct PlaneQuadRect { Vec2d min2 { 0., 0. }; Vec2d max2 { 0., 0. }; bool valid { false }; };
    std::vector<GLModel>       m_plane_quads;
    std::vector<PlaneQuadRect> m_plane_quad_rects;

    // Merged object-local model-part mesh (or the scoped part alone), shared by the contour/surface
    // builders and the chunk preview. Rebuilt lazily when the model data or the scope changes
    // (not on plane edits).
    const indexed_triangle_set &merged_mesh();
    indexed_triangle_set m_merged_its;
    bool                 m_merged_dirty { true };
    const ModelVolume   *m_merged_scope { nullptr };   // the part m_merged_its was built from, if scoped

    // Contour-shaped surface of the ACTIVE editing pose (object-local), rebuilt whenever the pose
    // settles on a new plane (never mid-drag).
    void    update_active_surface();
    GLModel m_active_surface;
    Vec3d   m_active_surface_point  { Vec3d::Zero() };
    Vec3d   m_active_surface_normal { Vec3d::Zero() };
    // Whether the active pose meets the mesh: a real cross-section, or a plane lying exactly on
    // a flat face (where the section is degenerate but the plane is anything but "outside").
    bool    m_active_surface_touches { false };

    // Chunk preview: the segments implied by the stack, rendered as distinctly-colored meshes in
    // place of the real object. Built from the same derive_cut_segments the slicing backend uses,
    // and from the very meshes that derivation cut — so the preview cannot disagree with the
    // descriptors, and no plane is cut a second time in a different frame.
    //
    // The preview is PROSPECTIVE: it includes the plane currently under the grabbers, which is not
    // in the stack yet (free mode) or not yet committed (mid-drag). Without that, the plane the
    // user can see does not split the colors, which reads as one section spanning a cut plane.
    void rebuild_segment_preview();
    void render_segment_preview();
    // The plane list the preview should be built from: the stored planes with the live gizmo pose
    // substituted for the bound plane, or appended when authoring a new one (appended because
    // add_plane() appends, so the preview matches what committing would produce). A bound plane
    // keeps its stored `enabled` flag — a disabled plane must not split the preview either.
    std::vector<Slic3r::CutPlane> effective_planes(bool *provisional = nullptr) const;
    // The thin-segment bound the slicing backend applies (initial layer height), so the preview
    // goes inert exactly where slicing would.
    double                        preview_min_segment_thickness() const;
    bool                 m_preview_chunks { false };
    std::vector<GLModel> m_segment_models;
    // The plane list the current preview was built from. Comparing against it each frame is what
    // keeps the preview live while a plane is dragged, without a dirty flag at every mutation site.
    std::vector<Slic3r::CutPlane> m_preview_planes;
    bool                          m_preview_valid { false };
    // Per stored plane: did it actually contribute a clip to the derived partition? A plane can be
    // valid and still cut nothing (it misses the mesh), which the plane list annotates.
    std::vector<bool>             m_plane_splits;
    // UNLAYERED_MOD -- per stored plane: the horizontal extent of the largest chunk seated on it,
    // which is the run the virtual-5-axis gantry limit is measured over (see build_reach_max). Only
    // as fresh as the last preview rebuild, so mid-drag on a heavy mesh it describes the pose the
    // preview last settled at. That is deliberate and safe in both directions: it keeps the limit
    // from sliding around under the user's cursor, and Print::validate() re-checks the exact
    // clipped pieces before anything is sliced. 0 = not known, which applies the hotend term alone.
    std::vector<double>           m_plane_reach;
    // The tilt limit (deg) that last held a plane back -- a drag, a typed angle or a face pick --
    // for plane m_tilt_limit_hit_plane; 0 once a tilt drag comes back inside it. Drives the red
    // "limit reached" line in the panel, so a plane that stops following the mouse says why.
    double                        m_tilt_limit_hit_deg{0.};
    int                           m_tilt_limit_hit_plane{-1};
    // Why the last derivation produced nothing, and whether it included a not-yet-committed plane.
    CutSegmentationStatus         m_preview_status { CutSegmentationStatus::StackInactive };
    bool                          m_preview_provisional { false };
    // Rebuilding cuts the whole mesh once per plane, so on a heavy mesh a live rebuild every drag
    // frame would stutter. Above this triangle count the preview waits for the pose to settle
    // (drag end) instead. A fixed geometric bound, not a wall-clock one, so behaviour is the same
    // on every machine.
    // 50k: a per-drag-frame derive over more than that stutters visibly (the old 250k bound did not).
    static constexpr size_t LIVE_PREVIEW_MAX_TRIANGLES = 50000;
    // What the chunk preview hid (object + instance), so leaving the gizmo re-shows exactly that and
    // nothing else -- the unscoped toggle_model_objects_visibility(true) also un-hides the wipe tower
    // and clears every volume's forced colours.
    const ModelObject *m_hidden_object   { nullptr };
    int                m_hidden_instance { -1 };
    const ModelVolume *m_hidden_volume   { nullptr };  // only this part was hidden, when scoped

    // Edit buffers for the B/C fields (committed on deactivate-after-edit).
    double  m_edit_b { 0. };
    double  m_edit_c { 0. };
    int     m_edit_plane { -1 };       // plane the buffers belong to
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoCutPlanes_hpp_
