#ifndef slic3r_DesignCanvas_hpp_
#define slic3r_DesignCanvas_hpp_

#include <wx/panel.h>
#include <wx/popupwin.h>

#include <functional>
#include <memory>
#include <string>

#include "slic3r/GUI/3DBed.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/CAD/SketchEngine.hpp"
#include "slic3r/GUI/CAD/DesignSketchTool.hpp"

class wxGLCanvas;
class wxFrame;
class wxStaticText;

namespace Slic3r {

class TriangleMesh;

namespace GUI {

class GLCanvas3D;
class SketchInlineEditor;

class DesignCanvas : public wxPanel
{
public:
    explicit DesignCanvas(wxWindow* parent);
    ~DesignCanvas() override;

    void set_mesh(const TriangleMesh& mesh);
    // Multi-body display: one GLVolume per body, each coloured distinctly (per-body colour).
    // `visible` (optional, indexed by body) hides bodies whose flag is false.
    void set_bodies(const std::vector<TriangleMesh>& body_meshes,
                    const std::vector<bool>& visible = {});
    void clear_mesh();

    void set_preview_mesh(const TriangleMesh& mesh);
    void clear_preview();

    void fit_view();
    void set_view(const std::string& view_name);

    void begin_sketch(const SketchPlane& plane, DesignSketchTool::Mode mode);
    // Re-open a committed entity sketch for full in-canvas editing (load geometry +
    // constraints, re-detect feature groups). Re-commits via finish_sketch().
    void edit_sketch(const std::vector<SketchEntity>& entities,
                     const std::vector<SketchEntityConstraintDef>& constraints,
                     const SketchPlane& plane);
    void set_sketch_tool(DesignSketchTool::Mode mode);
    void set_sketch_plane(const SketchPlane& plane);   // re-plane the live sketch when a reference plane is clicked in 3D
    void set_sketch_construction(bool c);
    // Text / SVG art into the LIVE sketch, as ordinary editable lines. False = no session.
    bool add_sketch_regions(const std::vector<std::vector<std::vector<Vec2d>>>& regions);
    void set_sketch_polygon_sides(int n);
    void set_sketch_polygon_circumscribed(bool c);
    void finish_sketch();
    bool is_sketching() const;
    void refresh_bed();   // re-sync the bed to the current printer (call on tab activation)
    void set_show_bed(bool b);   // view option: draw the printer bed + plate grid, or not
    void cancel_sketch();
    void set_on_sketch_commit(std::function<void(const SketchProfile&, const SketchPlane&)> cb);
    void set_on_sketch_entities_commit(
        std::function<void(const std::vector<SketchEntity>&,
                           const std::vector<SketchEntityConstraintDef>&,
                           const SketchPlane&)> cb);

    // Line tool: pending-segment length entry + live readout (Phase 2).
    void set_on_segment_drawn(std::function<void(double, double)> cb);
    void set_on_cursor_metrics(std::function<void(double, double, bool)> cb);
    void set_on_solve_state(std::function<void(int, bool, bool)> cb);  // dof, ok, has_constraints
    void apply_segment_length(double len);  // exact length, then commit & repaint
    void keep_segment_as_drawn();           // commit as-drawn & repaint

    // Sketch selection (Mode::Select).
    void set_on_sketch_selection_changed(std::function<void(int)> cb);
    void set_on_sketch_face_selected(std::function<void(int)> cb);  // closed loop clicked: region index passed
    void set_on_display_sketch_selected(std::function<void(int, int, int)> cb);  // committed loop clicked: (feature, region, entity)
    void set_on_display_sketch_activated(std::function<void(int)> cb);      // committed sketch DOUBLE-clicked: edit it
    std::vector<SketchEntity> selected_loop_entities() const;  // entities of the click-selected loop
    std::vector<std::vector<int>> region_entity_indices(const std::vector<SketchEntity>& ents) const;
    // Like region_entity_indices, but each region's entry is its OWN entities followed by the
    // entities of each of its holes — the same order selected_loop_entities() hands the kernel.
    // A per-loop extrude of a region WITH holes stores exactly this, so this is the shape a
    // consumed loop must be compared against.
    std::vector<std::vector<int>> region_entity_indices_with_holes(const std::vector<SketchEntity>& ents) const;
    void clear_loop_pick();  // drop the click-selected loop highlight (e.g. after extrude)
    void set_loop_pick(int feature, int region);  // adopt a loop pick made before the commit
    void set_escalate_on_repick(bool on);         // off while a card has armed a face/edge pick
    // Solid whole/face/edge selection: point the tool at the bodies + concatenated
    // tessellation (with per-triangle face & body ids), and a callback fired on each
    // whole->face->edge cycle (level, body index, face id, edge id).
    void set_solid_pick(const std::vector<CadBody>* bodies, const TriangleMesh* mesh,
                        const std::vector<int>* tri_face, const std::vector<int>* tri_body,
                        const std::vector<bool>* visible = nullptr,
                        const std::vector<Transform3d>* xform = nullptr);
    void set_on_solid_selection_changed(std::function<void(int, int, int, int)> cb);
    void set_on_place_on_face(std::function<bool()> cb);   // F key: Place on Face
    void select_body(int body);   // Parts-list -> highlight a whole body by index
    // Effective display colour of a body: the per-body override (Color tool) when set,
    // otherwise the auto body-index palette. Single source of truth shared with reload().
    ColorRGBA body_color(int body) const;
    // Move-body gizmo (M5): three world-axis drag arrows on a body; drag fires the move
    // callback with the body index + accumulated translation (display-only, host applies it).
    // body_radius = bounding-sphere radius of the body in world mm; the gizmo scales with it so
    // the rotation rings sit OUTSIDE the solid (Orca's Prepare gizmos do the same).
    void begin_move_body(int body, const Vec3d& pivot, const Transform3d& base_xform,
                         double body_radius);
    void clear_move_gizmo();
    bool moving_body() const;
    void set_on_body_move_changed(std::function<void(int, const Transform3d&)> cb);
    // Visual Fillet/Chamfer radius gizmo: when a solid edge is picked, anchor a radius arrow on
    // it; drag/edit fire the radius callback. Returns false if no edge is currently picked.
    bool begin_fillet_gizmo(const Vec3d& body_centroid, double radius);
    void clear_fillet_gizmo();
    bool filleting() const;
    void set_on_fillet_radius_changed(std::function<void(double)> cb);
    // Visual Hole gizmo: the panel feeds the hole plane + position + diameter/depth/through while
    // its Hole card is open; drag/edit fire the hole callback (x, y, diameter, depth).
    void begin_hole_gizmo(const SketchPlane& plane, double x, double y,
                          double diameter, double depth, bool through);
    void set_hole_face_bounds(bool has, double umin, double umax, double vmin, double vmax);
    void clear_hole_gizmo();
    bool holing() const;
    void set_on_hole_changed(std::function<void(double, double, double, double)> cb);
    // Visual Thread gizmo: footprint circle + radius/length arrows + draggable centre.
    void begin_thread_gizmo(const SketchPlane& plane, double x, double y,
                            double radius, double height);
    void clear_thread_gizmo();
    bool threading() const;
    void set_on_thread_changed(std::function<void(double, double, double, double)> cb);
    // Visual Shell gizmo: inward thickness arrow at the picked open-face centroid.
    void begin_shell_gizmo(const Vec3d& face_centroid, const Vec3d& inward_dir, double thickness);
    void clear_shell_gizmo();
    bool shelling() const;
    void set_on_shell_thickness_changed(std::function<void(double)> cb);
    // Visual Revolve angle-arc gizmo: the panel feeds the sketch plane + profile centroid + axis
    // (0=plane X, 1=plane Y) + angle + flip while its Revolve card is open; drag/edit fire the
    // angle callback.
    void begin_revolve_gizmo(const SketchPlane& plane, const Vec2d& centroid,
                             int axis_sel, double angle, bool flip);
    void clear_revolve_gizmo();
    bool revolving() const;
    void set_on_revolve_angle_changed(std::function<void(double)> cb);
    // Visual Draft angle-arc gizmo: the panel feeds the face centroid + face normal + angle while
    // its Draft card is open; drag/edit fire the angle callback.
    void set_draft_gizmo(const Vec3d& face_centroid, const Vec3d& face_normal, double angle);
    void clear_draft_gizmo();
    bool drafting() const;
    void set_on_draft_angle_changed(std::function<void(double)> cb);
    // Visual Cut gizmo: plane-rectangle preview + draggable normal offset arrow while
    // the Cut card is open; drag fires the offset callback.
    void set_cut_gizmo(const SketchPlane& plane, double offset, const Vec3d& body_center, double half_extent);
    void clear_cut_gizmo();
    bool cutting() const;
    void set_on_cut_offset_changed(std::function<void(double)> cb);
    // Visual Pattern gizmo: the panel feeds the (world XY) plane + target body centroid + mode +
    // count/dir/spacing/angle while its Pattern card is open; drag/edit fire the value callback.
    void begin_pattern_gizmo(const SketchPlane& plane, const Vec3d& body_centroid, bool circular,
                             int count, int dir, double spacing, double angle);
    void clear_pattern_gizmo();
    bool patterning() const;
    void set_on_pattern_changed(std::function<void(double)> cb);
    // Visual Extrude depth-arrow gizmo (C5b): the panel feeds the profile plane + centroid +
    // live depths/flags while its Extrude card is open; drag/edit fire the depth callback.
    void set_extrude_gizmo(const SketchPlane& plane, const Vec2d& centroid,
                           double depth, double depth2, bool two_sided, bool flip);
    void clear_extrude_gizmo();
    void set_on_extrude_depth_changed(std::function<void(double, bool)> cb);
    void set_datum_gizmo(const SketchPlane& plane, double usize, double vsize,
                         const Vec3d& base_origin, const Vec3d& base_normal,
                         double offset, bool offset_on);          // C3 resize handles + offset arrow
    void clear_datum_gizmo();
    void set_on_datum_size_changed(std::function<void(double, double)> cb);
    void set_on_datum_offset_changed(std::function<void(double)> cb);
    void set_helix_gizmo(const SketchPlane& plane, double radius, double pitch, double height,
                         double taper, bool left_handed);          // helix curve + 3 drag handles
    void clear_helix_gizmo();
    void set_on_helix_changed(std::function<void(double, double, double)> cb);
    void set_rib_gizmo(const SketchPlane& plane, const Vec2d& p0, const Vec2d& p1, double thickness);  // rib slab footprint + 2 thickness handles
    void clear_rib_gizmo();
    void set_on_rib_thickness_changed(std::function<void(double)> cb);
    void set_base_pick(std::vector<SketchPlane> planes, std::vector<int> bases,
                       std::vector<std::string> labels = {});   // clickable labelled reference planes
    void clear_base_pick();
    void set_on_datum_base_picked(std::function<void(int)> cb);
    void set_on_sketch_exit(std::function<void()> cb);           // Esc -> exit the tool
    void set_on_undo_redo(std::function<void(bool /*redo*/)> cb); // Ctrl+Z / Ctrl+Shift+Z
    // Persistently draw committed sketches (un-consumed ones stay visible).
    void set_display_sketches(std::vector<DesignSketchTool::DisplaySketch> ds);
    void set_highlight_sketches(std::vector<std::pair<int, ColorRGBA>> hl);
    void set_datum_planes(std::vector<SketchPlane> planes,
                           std::vector<Vec2d> sizes = {});     // draw datum/reference planes (u/v extents)
    // Mate connectors, drawn as frames so their verse and polarity are visible (snaporca-wgsc).
    void set_mate_connectors(std::vector<DesignSketchTool::MateConnectorGlyph> g);
    void set_mate_links(std::vector<std::pair<Vec3d, Vec3d>> l);
    void set_body_highlight(bool on);   // tint the solid when its feature is tree-selected
    // The status line, shown along the BASE OF THE VIEWPORT rather than in the side panel:
    // the panel clips it at ~73 characters with no warning (snaporca-8cc), the viewport's
    // bottom margin has the whole window width to spare. Empty text hides it.
    void set_status_text(const wxString& text, const wxColour& colour);
    // Take the status line down / bring it back when the Design page leaves and re-enters view.
    // A popup is a TOP-LEVEL window: hiding the page it belongs to does not hide it. Keeps the
    // text, so coming back needs no re-selection.
    void show_status_hud(bool on);
    void set_operand_bodies(int target_body, int tool_body);  // -1,-1 clears
    void set_body_translucent(bool on); // render the solid see-through (fillet/chamfer preview)
    void set_xray_focus(int body);      // >=0: fade+lock out every other body (CoordSys picking)
    void set_body_hidden(bool on);      // preview-only: hide base bodies, show only the result ghost
    void set_on_move_exit(std::function<void()> cb);   // right-click finished the move-body gizmo
    // Right-click (or its platform equivalent) on the viewport with no tool running: open the
    // object-driven offer there. Fires with SCREEN coordinates. Deliberately NOT fired while a
    // tool is live — right-click already ends a polyline chain and finishes the move gizmo, and
    // taking those over would break two working interactions in order to add a third.
    void set_on_context_menu(std::function<void(const wxPoint&)> cb);
    void delete_selected_sketch_entities();
    bool inline_busy() const;                         // a sketch value field is open (guard keys)
    bool live_sketch_has_work() const;                // the live sketch holds entities a cancel would destroy
    bool undo_last_sketch_entity();                   // Ctrl+Z in a sketch: drop the last entity
    bool delete_selected_or_last_sketch_entity();     // Delete in a sketch: selected, else last
    void clear_sketch_selection();

    // View toggles (keys P / A): origin planes, world axis triad. Each returns the new on/off
    // state so the caller can echo it in the status bar.
    bool toggle_planes();
    bool toggle_axes();

    // Section views (non-destructive): the panel owns the named "Section View N" list; the canvas
    // just applies/clears one horizontal clip at a time. model_mid_z() is the default cut height.
    void   set_section_plane(bool on, double z, bool keep_upper = false);
    double model_mid_z() const;

    // Dimension tool: act on the current sketch selection.
    DesignSketchTool::DimType sketch_dimension_kind() const;
    double sketch_dimension_current() const;
    void   apply_sketch_dimension(double v);

    // Open the in-canvas value editor at the cursor for a host-driven value (the
    // committed-feature Constrain path uses this instead of a docked numeric card).
    void   open_inline_value(double current, std::function<void(double)> commit,
                             std::function<void()> cancel = {});

    // Dimension tool (Mode::Dimension): click-to-place quotes. The pick-complete
    // callback lets the panel pop the value card; set/cancel apply or keep the value.
    void set_on_dimension_pick_complete(std::function<void(double)> cb);
    DesignSketchTool::DimType pending_dimension_type() const;
    void set_sketch_dimension_value(double v);
    void cancel_sketch_dimension();

    // Constrain mode: load a committed profile for picking + constraint editing.
    void begin_constrain(const SketchProfile& prof, const SketchPlane& plane);
    // Leave constrain mode and clear any picked-entity highlight from the overlay.
    void end_constrain();
    bool is_constraining() const;
    bool selected_segment(int& a, int& b) const;
    void update_constrain_profile(const std::vector<Vec2d>& pts);

    // Entity-aware Constrain (Fase 4.2): pick Line entities of a committed sketch.
    void begin_constrain_entities(const std::vector<SketchEntity>& ents, const SketchPlane& plane);
    bool is_constraining_entities() const;
    // Sketch selection, for the offer menu: how many entities are selected and what the first
    // one is. Returns 0 when nothing is selected.
    int  sketch_selection_count() const;
    bool sketch_first_selected_type(SketchEntity::Type& out) const;

    // In-canvas bbox transform of imported Text/SVG art (replaces the Move/Scale dialog).
    void begin_imported_transform(int feat,
                                  const std::vector<std::vector<std::vector<Vec2d>>>& base_regions,
                                  const SketchPlane& plane, const Vec2d& offset,
                                  double scale_x, double scale_y);
    void set_on_imported_transform(std::function<void(int, Vec2d, double, double)> cb);
    bool selected_constrain_entities(int& e0, int& e1) const;
    int  selected_constrain_axis() const;  // third pick slot (Symmetric axis), -1 if unset
    bool pick0_point(Vec2d& out) const;   // plane-coords of the slot-0 pick (trim/extend)
    void update_constrain_entities(const std::vector<SketchEntity>& ents);
    // Constraint manager (C3.4): highlight the entities referenced by a selected
    // constraint (yellow tint in Constrain mode); empty clears the highlight.
    void set_constraint_highlight(std::vector<int> entities);
    // Constraint glyph badges (C3.4b): the feature's constraints, drawn as iconic
    // marks near their entities in Constrain mode; empty clears them.
    void set_constraint_glyphs(std::vector<SketchEntityConstraintDef> cons);
    // Repaint the embedded canvas the right way for the active GL backend:
    // hardware GL gets a scheduled wxEVT_PAINT (render() runs inside the paint
    // cycle); software GL (llvmpipe etc.) gets a direct render() because a
    // scheduled Refresh() is frequently dropped there. Backend cached on first use.
    // Public: DesignPanel calls it after a tree edit to force a frame on software GL.
    void request_repaint();
    // Repaint synchronously, once the pending show/resize has settled. Needed when the
    // notebook re-shows the Design page: an invalidation issued while the page is still
    // being shown is dropped on hardware GL and no wxEVT_PAINT ever follows, leaving the
    // canvas blank until another tab switch forces an expose.
    void force_repaint();
    // Repaint synchronously, for use while a modal popup (the offer menu) owns the event loop:
    // a queued Refresh() is not serviced until the popup closes, so a hover ghost drawn behind it
    // would never appear. Mirrors DesignPanel's m_status->Update() flush.
    void repaint_now();

private:
    void reload(bool keep_view);

    wxGLCanvas* m_canvas_widget{nullptr};
    GLCanvas3D* m_canvas{nullptr};
    int         m_sw_gl{-1};   // -1 unknown, 0 hardware GL, 1 software GL

    std::function<void(const wxPoint&)> m_on_context_menu;
    bool        m_ctx_bound{false};   // bind the RIGHT_UP handler once, however often the cb is set
    wxPoint     m_ctx_press{0, 0};    // right-press origin: a right-DRAG pans, it must not offer

    Bed3D       m_bed;
    Model       m_model;
    bool        m_first_frame{true};
    bool        m_body_selected{false};   // tree selected a body feature → tint the solid
    int         m_hl_body_target{-1};
    int         m_hl_body_tool{-1};
    bool        m_body_translucent{false};// fillet/chamfer preview → render the body see-through
    int         m_xray_focus{-1};          // >=0: only this body is opaque+clickable (CoordSys picking)
    bool        m_body_hidden{false};     // preview-only mode → hide base bodies, ghost = the result
    std::vector<bool> m_body_visible;     // per-body visibility (empty => all visible)
    // Live pointer to the document's bodies (stable address: m_doc.bodies), stashed by
    // set_solid_pick so reload()/body_color() can read each body's colour override.
    const std::vector<CadBody>* m_color_bodies{nullptr};

    DesignSketchTool m_sketch_tool;

    // Section view: whether a horizontal clip is currently applied (guards Alt+Wheel). The cut
    // height and the named-view list live in DesignPanel; the canvas is a dumb applier.
    bool m_section_on{false};

    std::unique_ptr<SketchInlineEditor> m_inline_editor;  // floating in-canvas value editor
    // Bottom-right viewport HUD: a borderless float label over the GL canvas showing the
    // active tool's current values (fed by the tool's on_readout). Empty text hides it.
    wxFrame*      m_hud{nullptr};
    wxStaticText* m_hud_label{nullptr};
    std::string   m_hud_last;
    void set_readout(const std::string& text);

    // Bottom-LEFT viewport HUD: the selection / tool status line, written by DesignPanel.
    // A wxPopupWindow, NOT the wxFrame the readout HUD uses: a frame accepts keyboard focus,
    // and this one is on screen permanently and re-raised on every status change, so it stole
    // the keyboard from the canvas and killed every sketch shortcut in the tab.
    wxPopupWindow* m_status_hud{nullptr};
    wxStaticText* m_status_hud_label{nullptr};
    wxString      m_status_hud_last;
    wxColour      m_status_hud_colour;
    void place_status_hud();          // re-anchors to the canvas corner (also on resize)
    void apply_status_label();        // SetLabel + Wrap to the canvas width + Fit, always together
    std::function<void(const SketchProfile&, const SketchPlane&)> m_on_sketch_commit;
    std::function<void(const std::vector<SketchEntity>&,
                       const std::vector<SketchEntityConstraintDef>&,
                       const SketchPlane&)> m_on_sketch_entities_commit;
};

}} // namespace Slic3r::GUI

#endif // slic3r_DesignCanvas_hpp_
