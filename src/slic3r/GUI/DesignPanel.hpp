#ifndef slic3r_DesignPanel_hpp_
#define slic3r_DesignPanel_hpp_

#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/treebase.h>   // wxTreeItemId

#include <vector>
#include <memory>
#include <functional>
#include <map>

#include "libslic3r/CadDocument.hpp"

class wxChoice;
class wxCheckBox;
class wxCheckListBox;
class wxSpinCtrl;
class wxSpinCtrlDouble;
class wxTreeCtrl;
class wxImageList;
class wxStaticText;
class wxStaticLine;
class Button;      // Orca-styled button (Widgets/Button.hpp)
class wxSizer;
class wxButton;
class wxPanel;
class ScalableButton;

namespace Slic3r { namespace GUI {

class DesignCanvas;

// Design (CAD) tab: a sketch-first, Onshape-style form-driven CAD panel.
// Sketch and Extrude are independent tools: the user creates a Sketch first,
// then selects it and Extrudes to produce a solid.
class DesignPanel : public wxPanel
{
public:
    explicit DesignPanel(wxWindow* parent);
    void on_tab_shown();        // re-sync bed to the active printer when the Design tab is activated
    // Rebuild off the UI thread (progress dialog only if it turns out to be slow), so a feature
    // op on a heavy imported solid does not freeze the window. Returns m_doc.recompute()'s result.
    bool recompute_guarded(const wxString& message);

    // MCP control hooks: let the external control server (McpControl.cpp) drive and
    // perceive the SAME kernel the GUI uses. Called only on the wx main thread.
    CadDocument& mcp_doc()        { return m_doc; }            // live document (read + mutate)
    void         mcp_after_change() { after_tree_edit(true); } // refresh tree + viewport + status

private:
    enum class Tool { None, Sketch, Extrude, Dressup, Hole, Thread, Shell, Revolve, Sweep, Pattern, Plane, Loft, Draft, Boolean, Cut, Insert };
    // Plane tool: which datum reference the next solid pick fills (declared early so the
    // method decls + card lambdas below can name it).
    enum class PlanePick { None, FaceA, FaceB, EdgeA, EdgeB };

    // Onshape-style contextual top toolbar: only the active mode's tool group is
    // shown (Feature = sketch/extrude/dress/hole/thread; Sketch = entity tools;
    // Constrain = constraints + edit ops). Replaces the old always-visible wall.
    enum class UiMode { Feature, Sketch, Constrain };
    void set_ui_mode(UiMode m);
    // Unified action-bar dispatch: one Confirm / one Cancel for every tool and mode.
    void tool_confirm();        // ✓ : commit the active feature / sketch / constrain session
    void tool_cancel();         // ✗ / Esc : cancel the active feature / discard / exit
    void update_action_bar();   // show the ✓/✗ bar iff a tool or mode is active

    void on_shape_changed();
    void on_add_sketch();
    void on_add_extrude();
    void on_add_dressup();
    void on_add_hole();
    void on_add_thread();
    void apply_thread_standard();   // fill pitch/depth/radius from m_thread_std selection
    void infer_thread_spec(double diameter);  // nearest M-standard from a picked cylinder diameter
    void on_add_revolve();
    void on_add_sweep();
    void on_add_loft();
    void on_add_pattern();
    void on_add_plane();
    void arm_plane_pick(PlanePick target);   // Plane tool: next solid pick fills this reference
    void apply_plane_refs(CadFeature& f) const; // copy type + face/edge refs + sizes from the card
    void refresh_plane_labels();             // update the 4 pick labels from the captured refs
    void reset_plane_refs();                 // clear captured refs (fresh Plane add)
    void on_add_shell();
    void on_add_draft();
    void on_add_boolean();
    void on_add_cut();              // commit a plane Cut (split-by-plane)
    // Fill m_bool_target / m_bool_tool / m_cut_target. as_of_feature < 0 = current bodies (add);
    // >= 0 = the bodies as they existed just before that feature index (Boolean re-edit, so a
    // consumed tool body still appears and its saved selection round-trips).
    void populate_body_choices(int as_of_feature = -1);
    // Import rigid 2D art (Text / SVG) as a new Sketch feature carrying
    // imported_regions (no solver entities). on_add_text/on_import_svg gather
    // input; add_imported_sketch builds the feature, refreshes tree + display.
    void on_add_text();
    void on_import_svg();
    void on_import_step();   // STEP -> editable B-rep body (keeps the OCCT solid, not a mesh)
    void on_import_mesh();   // STL/OBJ -> B-rep body via GeometryEngine::mesh_to_brep
    bool place_on_face();    // Prepare's Place on Face (F): lay the selected body face on the bed
    void add_imported_sketch(const std::vector<std::vector<std::vector<Vec2d>>>& regions,
                             const wxString& base_name);
    // Imported Text/SVG art is placed/sized in-canvas then explicitly committed via a
    // small Confirm/Cancel card (Onshape Button->Dialog->Preview->Confirm). The feature
    // is added provisionally by add_imported_sketch; Confirm keeps it, Cancel undoes it.
    void open_insert_card(const wxString& base_name);
    void finalize_insert();   // Confirm: keep the placed art, leave the placement gizmo
    void cancel_insert();     // Cancel: undo the provisional insert
    // Move / enlarge / stretch (independent X/Y) an imported Text/SVG sketch:
    // a modal dialog editing the feature's placement transform in place.
    void on_transform_imported(int feat_idx);
    void on_commit();
    void on_export_step();   // write all bodies to a .step file (native B-rep)
    // Rehydrate the parametric model from a project's saved recipe (3MF
    // Metadata/SnapOrca_cad.bin): deserialize -> recompute -> refresh viewport + tree.
    void load_recipe(const std::string& blob);
    void refresh_tree();
    void set_status_ok();

    // Feature-tree editing (Onshape-style): act on the selected tree row.
    void on_delete_feature();
    void on_new_design();
    void on_move_feature(int delta);   // -1 = up, +1 = down
    void on_toggle_visibility();       // show/hide the selected feature (CadFeature::enabled)

    // Constrain mode: enter on the tree-selected sketch, then apply a geometric
    // constraint to the in-canvas picked segment and re-solve in the kernel.
    void on_begin_constrain(int sel_override = -1);
    // Sketch-toolbar Constrain entry: commit the live sketch in place, then enter Constrain
    // mode on it (so the constraint palette + Trim/Extend are reachable without leaving the
    // sketch flow). Returns true if constrain mode was entered.
    bool enter_constrain_inline();
    void apply_constraint(SketchConstraintType type);
    void apply_entity_constraint(SketchConstraintType type);  // Fase 4.2 entity path
    enum class EditOp { Mirror, Offset, Fillet, Trim, Extend, Array, Move, Chamfer, Rotate, Scale, PolarArray }; // Fase 4.4/4.5/4.6 sketch edit ops
    void apply_edit_op(EditOp op);                            // mutate selected sketch entities
    // Onshape-style docked value entry (replaces wxGetTextFromUser popups for
    // Angle/Radius/Diameter constraints + Offset/Fillet edit ops). request_value
    // shows the card and stows a continuation run by confirm_value().
    void request_value(const wxString& label, double def, double mn, double mx,
                       std::function<void(double)> cont,
                       std::function<void()> on_cancel = nullptr);
    void confirm_value();
    void cancel_value();
    void commit_entity_constraint(const SketchEntityConstraintDef& def); // shared solve/refresh tail
    void commit_entity_constraints(const std::vector<SketchEntityConstraintDef>& defs); // multi-def (Symmetric)

    // Constraint manager (C3.4): a docked list of the constrained sketch's
    // entity-constraints with per-row select (highlight the referenced entities in
    // the viewport) and delete (drop the constraint + re-solve). Shown in Constrain
    // mode only; operates on m_doc.features[m_constrain_feat].entity_constraints.
    void rebuild_constraint_list();                                      // refill m_constraint_rows
    void delete_constraint(int idx);                                     // erase + re-solve + refresh
    void highlight_constraint_entities(int idx);                         // push referenced entities to viewport
    void refresh_constrain_dof();                                        // re-solve feature, mirror DoF readout
    wxString constraint_label(const SketchEntityConstraintDef& d) const; // human-readable row text
    void after_edit_op();                                                // shared edit-op refresh tail
    void on_edit_feature();            // reopen the selected feature's dialog populated
    void after_tree_edit(bool ok);     // shared post-op refresh of tree/viewport/status
    void load_feature_into_dialog(const CadFeature& f);
    void reset_edit_state();           // back to add-mode (m_edit_index = -1)

    // Onshape loop: Button -> open_tool (show dialog) -> refresh_preview (ghost) ->
    // confirm_tool (commit) / cancel_tool (abort).
    void       open_tool(Tool t);
    void       close_tool();
    void       refresh_preview();
    void       confirm_tool();
    void       cancel_tool();
    // Ctrl+Z / Ctrl+Shift+Z (Ctrl+Y) from the viewport. With a tool/dialog open it
    // cancels that (Esc-like); otherwise it undoes/redoes the committed feature history.
    void       do_undo_redo(bool redo);
    // The plane the Hole tool drills on: a picked face (inward, centred) or the dropdown.
    SketchPlane hole_plane() const;
    // The plane the Thread tool builds on: a picked cylindrical face (axis) or the dropdown.
    SketchPlane thread_plane() const;
    CadFeature build_candidate(Tool t) const;
    int        resolve_extrude_sketch() const;
    // Plane pickers: fill a choice with XY/XZ/YZ + the document's datum planes, and
    // map a choice row back to the actual SketchPlane (rows 0-2 base, 3+ datum).
    void        populate_plane_choices(wxChoice* c) const;
    SketchPlane plane_from_choice(int row) const;
    // True when Extrude should build only the click-selected loop (a region of the
    // resolved sketch is selected and it carries entities).
    bool       extrude_uses_loop() const;
    void       sync_sketch_display();   // push un-consumed committed sketches to the viewport
    // Feed the viewport's visual Extrude depth-arrow gizmo (C5b) with the current profile
    // plane + centroid + live depths while the Extrude card is open (self-gates on m_active).
    void       update_extrude_gizmo();
    void       update_fillet_gizmo();     // edge-anchored radius arrow (Dressup card)
    void       update_hole_gizmo();       // footprint circle + diameter/depth arrows (Hole card)
    void       update_thread_gizmo();     // footprint circle + radius/length arrows (Thread card)
    void       update_shell_gizmo();      // inward thickness arrow on the picked face (Shell card)
    void       update_revolve_gizmo();    // angle-arc around the axis (Revolve card)
    void       update_draft_gizmo();      // angle-arc around the face centroid (Draft card)
    void       update_cut_gizmo();        // plane-rectangle + offset arrow (Cut card)
    void       update_operand_highlight(); // Boolean/Sweep/Loft operand tinting on the canvas
    void       update_pattern_gizmo();    // linear spacing arrow / circular angle-arc (Pattern card)
    void       update_datum_gizmo();      // resize handles on the datum plane being created/edited (C3)
    void       refresh_datum_planes();    // push resolved datum frames + per-plane u/v extents to viewport
    void       update_reference_planes(); // persistent XY/XZ/YZ reference planes (fallback when no object)

    CadDocument m_doc;

    Tool      m_active{Tool::None};

    // Keyboard shortcuts (Onshape-style, three scoped layers). Keys are encoded as the
    // upper-cased letter, OR'd with 0x10000 when Shift is required. m_keys_sketch fires only
    // while a sketch is open (single letters = sketch tools); m_keys_feature fires only when
    // no sketch is open (Shift+letter = feature tools; single letters = view toggles/section).
    static constexpr int SC_SHIFT = 0x10000;
    std::map<int, std::function<void()>> m_keys_sketch;
    std::map<int, std::function<void()>> m_keys_feature;

    wxSizer*  m_box_sketch{nullptr};
    wxSizer*  m_box_extrude{nullptr};
    wxSizer*  m_box_dressup{nullptr};
    wxSizer*  m_box_hole{nullptr};
    wxSizer*  m_box_thread{nullptr};
    wxSizer*  m_box_shell{nullptr};
    wxSizer*  m_box_revolve{nullptr};
    wxSizer*  m_box_sweep{nullptr};
    wxSizer*  m_box_pattern{nullptr};
    wxSizer*  m_box_plane{nullptr};
    wxSizer*  m_box_loft{nullptr};
    wxSizer*  m_box_draft{nullptr};
    wxSizer*  m_box_boolean{nullptr};
    wxSizer*  m_box_cut{nullptr};
    wxSizer*  m_box_insert{nullptr};   // Confirm/Cancel card for placing Text/SVG art
    int       m_insert_feat{-1};       // provisional imported-art feature awaiting Confirm
    // Move-body gizmo runs through the unified action bar too: Confirm keeps the placement,
    // Cancel reverts to the pose captured when the move started.
    int         m_move_body{-1};
    Transform3d m_move_prev{Transform3d::Identity()};

    // Onshape-style dialog-card title rows (icon + bold feature name), retitled
    // per tool in open_tool() (edit-mode shows the feature's actual name).
    wxStaticText* m_hdr_sketch{nullptr};
    // Onshape sketch-entry card (plane/orientation) that opens on "New sketch" and
    // persists until Finish (Phase 3).
    wxSizer*      m_box_sketch_session{nullptr};
    wxStaticText* m_hdr_sketch_session{nullptr};
    wxStaticText* m_hdr_extrude{nullptr};
    wxStaticText* m_hdr_dressup{nullptr};
    wxStaticText* m_hdr_hole{nullptr};
    wxStaticText* m_hdr_thread{nullptr};
    wxStaticText* m_hdr_shell{nullptr};
    wxStaticText* m_hdr_revolve{nullptr};
    wxStaticText* m_hdr_sweep{nullptr};
    wxStaticText* m_hdr_pattern{nullptr};
    wxStaticText* m_hdr_plane{nullptr};
    wxStaticText* m_hdr_loft{nullptr};
    wxStaticText* m_hdr_draft{nullptr};
    wxStaticText* m_hdr_boolean{nullptr};
    wxStaticText* m_hdr_cut{nullptr};
    wxStaticText* m_hdr_insert{nullptr};

    wxScrolledWindow* m_form{nullptr};
    DesignCanvas*     m_viewport{nullptr};

    // Top contextual toolbar (parented to the panel, above the form/viewport row).
    UiMode    m_ui_mode{UiMode::Feature};
    wxScrolledWindow* m_toolbar{nullptr};   // horizontally scrollable so the action bar stays reachable on narrow windows
    wxSizer*  m_tb_feature{nullptr};
    wxSizer*  m_tb_sketch{nullptr};
    wxSizer*  m_tb_constrain{nullptr};
    // Unified Confirm/Cancel action bar (right end of the ribbon). Shown whenever any
    // tool or mode is active; the single confirm/cancel surface for the whole tab.
    wxSizer*  m_tb_action{nullptr};
    // Persistent Undo/Redo group at the left of the ribbon — always visible, independent
    // of the mode-gated tool groups. The buttons are greyed per the document history and
    // the do_undo_redo gate (see update_undo_redo_buttons).
    wxSizer*        m_tb_history{nullptr};
    ScalableButton* m_btn_undo{nullptr};
    ScalableButton* m_btn_redo{nullptr};
    void update_undo_redo_buttons();   // enable/disable Undo/Redo from can_undo/can_redo + gate
    // All tool buttons, for the active-tool teal highlight (Onshape-style).
    std::vector<ScalableButton*> m_tool_btns;
    ScalableButton*              m_active_tool_btn{nullptr};
    void set_active_tool_btn(ScalableButton* b);   // nullptr clears the highlight
    // Owns the themed DropDown flyouts (and the item vectors they hold by ref).
    std::vector<std::shared_ptr<void>> m_flyout_keepalive;
    wxCheckBox*       m_construction{nullptr};   // sketch-mode construction toggle
    wxSpinCtrl*       m_sides{nullptr};          // polygon sides
    wxCheckBox*       m_poly_circ{nullptr};      // polygon circumscribed toggle

    wxChoice*         m_draw_plane{nullptr};
    wxChoice*         m_shape{nullptr};
    wxChoice*         m_plane{nullptr};
    wxChoice*         m_mode{nullptr};
    wxSpinCtrlDouble* m_width{nullptr};
    wxSpinCtrlDouble* m_height{nullptr};
    wxSpinCtrlDouble* m_radius{nullptr};
    wxSpinCtrlDouble* m_distance{nullptr};
    wxChoice*         m_extrude_end{nullptr};   // Blind/Symmetric/TwoSided/ThroughAll/UpTo*
    wxSpinCtrlDouble* m_distance2{nullptr};     // second-side depth (Two-sided)
    wxSpinCtrlDouble* m_taper{nullptr};         // draft angle (deg)
    wxCheckBox*       m_flip{nullptr};          // reverse extrude direction

    wxStaticText*     m_extrude_sketch_label{nullptr};
    int               m_extrude_sketch_ref{-1};

    // Revolve controls (sweep a sketch profile about an in-plane axis).
    wxStaticText*     m_revolve_sketch_label{nullptr};
    wxSpinCtrlDouble* m_revolve_angle{nullptr};
    wxChoice*         m_revolve_axis{nullptr};   // 0 = plane X, 1 = plane Y
    wxChoice*         m_revolve_mode{nullptr};   // New/Add/Cut/Intersect
    wxCheckBox*       m_revolve_flip{nullptr};
    int               m_revolve_sketch_ref{-1};

    // Sweep controls (sweep a profile sketch along a path sketch).
    wxStaticText*     m_sweep_profile_label{nullptr};
    wxChoice*         m_sweep_path{nullptr};       // path Sketch picker (feature index in client data)
    wxChoice*         m_sweep_mode{nullptr};       // New/Add/Cut/Intersect
    int               m_sweep_profile_ref{-1};
    int               m_sweep_path_ref{-1};        // path Sketch feature index (for re-edit pre-select)

    // Loft controls (skin a solid through 2+ ordered profile Sketches).
    wxCheckListBox*   m_loft_list{nullptr};        // every Sketch; check 2+ in list order = profiles
    wxCheckBox*       m_loft_ruled{nullptr};       // ruled (straight) vs smooth sections
    wxChoice*         m_loft_mode{nullptr};        // New/Add/Cut/Intersect
    std::vector<int>  m_loft_sketch_idx;           // feature index for each row in m_loft_list
    std::vector<int>  m_loft_refs;                 // chosen profile refs (for re-edit pre-check)

    // Pattern controls (replicate the target body: linear or circular).
    wxChoice*         m_pattern_type{nullptr};      // 0 = Linear, 1 = Circular
    wxSpinCtrlDouble* m_pattern_count{nullptr};     // total instances incl. seed
    wxSpinCtrlDouble* m_pattern_spacing{nullptr};   // linear step (mm)
    wxChoice*         m_pattern_dir{nullptr};        // linear direction: 0 = plane X, 1 = plane Y
    wxSpinCtrlDouble* m_pattern_angle{nullptr};     // circular total angle (deg)
    // Boolean controls (combine two existing bodies).
    wxChoice*         m_bool_op{nullptr};            // 0 = Union, 1 = Subtract, 2 = Intersect
    wxChoice*         m_bool_target{nullptr};        // body that survives (selection == body index)
    wxChoice*         m_bool_tool{nullptr};          // body consumed (selection == body index)
    wxCheckBox*       m_bool_keep{nullptr};          // keep the tool body after the op
    wxSpinCtrlDouble* m_bool_tol{nullptr};           // OCCT fuzzy tolerance (mm); robust cut on near-coincident faces

    // Plane Cut (split-by-plane): a reference plane + offset splits the target body into
    // two separate bodies (both pieces kept).
    wxChoice*         m_cut_plane{nullptr};          // XY/XZ/YZ + datum planes (cut plane)
    wxChoice*         m_cut_target{nullptr};         // body to cut (selection == body index)
    wxSpinCtrlDouble* m_cut_offset{nullptr};         // offset along the plane normal (mm)
    // Datum plane controls (derive a selectable sketch plane: offset + tilt from a base).
    wxChoice*         m_plane_base{nullptr};         // 0=XY,1=XZ,2=YZ, 3+N = Nth datum plane
    wxSpinCtrlDouble* m_plane_offset{nullptr};       // offset along base normal (mm)
    wxSpinCtrlDouble* m_plane_tilt{nullptr};         // tilt about a base axis (deg) / Angle / Tangent angle
    wxChoice*         m_plane_tilt_axis{nullptr};    // 0 = base X, 1 = base Y
    // Plane construction method + contextual face/edge reference picks (Onshape/Fusion parity).
    wxChoice*         m_plane_type{nullptr};         // PlaneType: Offset/Angle/Midplane/Tangent/TwoEdges/Coincident
    wxButton*         m_plane_pick_faceA{nullptr};   wxStaticText* m_plane_faceA_lbl{nullptr};
    wxButton*         m_plane_pick_faceB{nullptr};   wxStaticText* m_plane_faceB_lbl{nullptr};
    wxButton*         m_plane_pick_edgeA{nullptr};   wxStaticText* m_plane_edgeA_lbl{nullptr};
    wxButton*         m_plane_pick_edgeB{nullptr};   wxStaticText* m_plane_edgeB_lbl{nullptr};
    wxSpinCtrlDouble* m_plane_usize{nullptr};        // datum rectangle extent u (mm) — also driven by drag handles
    wxSpinCtrlDouble* m_plane_vsize{nullptr};        // datum rectangle extent v (mm)
    // Captured references for the candidate datum (body index + face/edge index, -1 = none).
    int m_pl_faceA_body{-1}, m_pl_faceA{-1};
    int m_pl_faceB_body{-1}, m_pl_faceB{-1};
    int m_pl_edgeA_body{-1}, m_pl_edgeA{-1};
    int m_pl_edgeB_body{-1}, m_pl_edgeB{-1};
    PlanePick m_plane_pick{PlanePick::None};         // which ref the next solid pick fills
    // Plate loop selection (click a committed sketch loop): the Sketch feature + the
    // clicked closed-region index, so Extrude builds just that one loop. -1 = none.
    int               m_sel_sketch_feat{-1};
    int               m_sel_sketch_region{-1};
    // Click-selected solid topology (whole/face/edge cycle): face id for up-to-face / dress-up.
    int               m_sel_solid_body{-1};   // which body the face/edge selection is on
    int               m_sel_solid_face{-1};
    int               m_sel_solid_edge{-1};
    // Face-as-profile extrude (Onshape): when Extrude is opened on a picked solid face with
    // no sketch source, this carries that global face id so the kernel extrudes the face.
    // -1 = ordinary sketch/loop extrude. Set when opening the Extrude card, consumed on add.
    int               m_extrude_face_src{-1};

    wxChoice*         m_dressup_type{nullptr};
    wxChoice*         m_face_group{nullptr};
    wxSpinCtrlDouble* m_dressup_size{nullptr};

    wxChoice*         m_hole_plane{nullptr};
    wxSpinCtrlDouble* m_hole_diameter{nullptr};
    wxSpinCtrlDouble* m_hole_depth{nullptr};
    wxCheckBox*       m_hole_through{nullptr};
    wxSpinCtrlDouble* m_hole_x{nullptr};
    wxSpinCtrlDouble* m_hole_y{nullptr};
    // #2: when the Hole tool is opened on a picked solid face, drill on that face centred
    // on it (origin = face centroid, normal = inward). m_hole_x/y then read as the offset
    // from the face centre. Falls back to the m_hole_plane dropdown when no face is picked.
    bool              m_hole_on_face{false};
    SketchPlane       m_hole_face_plane;
    int               m_hole_face_body{-1};
    // #2 Part B: the picked face's (u,v) bounds in m_hole_face_plane, so the hole's construction
    // dims read as distance from the face sides (umin/vmin edges) rather than from the centre.
    bool              m_hole_has_bounds{false};
    double            m_hole_umin{0}, m_hole_umax{0}, m_hole_vmin{0}, m_hole_vmax{0};

    wxChoice*         m_thread_plane{nullptr};
    wxChoice*         m_thread_std{nullptr};   // standard designation (M6, 1/4-20 UNC, ...)
    wxSpinCtrlDouble* m_thread_radius{nullptr};
    wxSpinCtrlDouble* m_thread_pitch{nullptr};
    wxSpinCtrlDouble* m_thread_height{nullptr};
    wxSpinCtrlDouble* m_thread_depth{nullptr};
    wxCheckBox*       m_thread_internal{nullptr};
    wxSpinCtrlDouble* m_thread_x{nullptr};
    wxSpinCtrlDouble* m_thread_y{nullptr};
    // #3: when the Thread tool is opened on a picked cylindrical face (a hole bore or a
    // cylinder), thread that surface — plane on its axis, radius/internal derived from it.
    bool              m_thread_on_face{false};
    SketchPlane       m_thread_face_plane;
    int               m_thread_face_body{-1};

    wxSpinCtrlDouble* m_shell_thickness{nullptr};
    wxStaticText*     m_shell_face_label{nullptr};   // shows the picked face to remove

    // Draft controls (taper a single picked solid face about the body bottom).
    wxSpinCtrlDouble* m_draft_angle{nullptr};
    wxStaticText*     m_draft_face_label{nullptr};   // shows the picked face to draft

    // Onshape-style docked value-entry card (Angle/Radius/Diameter/Offset/Fillet).
    wxSizer*          m_box_value{nullptr};
    wxStaticText*     m_value_label{nullptr};
    wxTextCtrl*       m_value_input{nullptr};   // plain text field: forces en ('.') decimals
    double            m_value_min{0.0};         // range for confirm-time clamping
    double            m_value_max{0.0};
    std::function<void(double)> m_value_cont;   // deferred apply, run on Confirm
    std::function<void()>       m_value_cancel; // optional action when the card is cancelled

    // Feature tree: a wxTreeCtrl with per-feature-type icons. Callers keep using
    // integer row indices via tree_selection()/set_tree_selection(); m_tree_items
    // maps feature order -> tree node, rebuilt by refresh_tree().
    wxTreeCtrl*               m_tree{nullptr};
    wxTreeCtrl*               m_parts{nullptr};        // Bodies list under the feature tree
    wxStaticText*             m_parts_label{nullptr};  // its "Bodies" caption (hidden when empty)
    wxBoxSizer*               m_parts_hdr{nullptr};    // Bodies card header (icon + title)
    wxStaticLine*             m_parts_rule{nullptr};   // rule under that header
    wxBoxSizer*               m_hdr_tree_row{nullptr}; // Feature tree header: title + row actions
    wxStaticText*             m_hdr_tree{nullptr};     // its title label
    wxImageList*              m_tree_images{nullptr};
    std::vector<wxTreeItemId> m_tree_items;
    // Parts list: tree rows for each body (parallel to m_doc.bodies). Selecting one
    // highlights that body and makes it the target for the next op.
    std::vector<wxTreeItemId> m_tree_body_items;

    // Section views (non-destructive): named "Section View N" entries listed in the tree, each a
    // horizontal clip height. View-only — NOT bodies/features, never serialized. Key X adds one;
    // clicking a row activates it (again = off); Delete removes it; Alt+Wheel moves the active one.
    // Section view (single, non-destructive): ONE horizontal clip that hides half the model to
    // inspect inside — solid, no ghost of the hidden half. Toggled on/off; Flip shows the other
    // half. Never a body, no tree entry.
    bool      m_section_on{false};
    double    m_section_cut_z{0.0};
    bool      m_section_upper{false};             // false = keep lower half, true = upper
    Button*   m_section_flip_btn{nullptr};        // enabled only while the section is on
    void toggle_section_view();                   // Section View button / X: on <-> off
    void flip_section_view();                     // Flip button / F: opposite half
    void update_section_flip_btn();               // enable the Flip button iff the section is on
    // Per-body visibility (parallel to m_doc.bodies; index stable across recompute since
    // bodies are appended in feature order). Empty/grown to all-visible by sync_body_visible().
    std::vector<bool> m_body_visible;
    void sync_body_visible();             // grow/shrink m_body_visible to bodies.size()
    // Per-body display translation (Move-body, M5). Parallel to m_doc.bodies; default
    // identity. Applied to the display/pick meshes only — the OCCT shape (and face/edge
    // global ids) is never touched, so dress-up targeting stays stable across a move.
    std::vector<Transform3d>  m_body_xform;
    std::vector<TriangleMesh> m_disp_body_meshes;   // display_body_meshes with m_body_xform applied
    TriangleMesh              m_disp_pick_mesh;      // combined pick mesh with m_body_xform applied
    void sync_body_xform();               // grow m_body_xform to bodies.size() (identity)
    void rebuild_disp_meshes();           // recompute m_disp_* from m_doc + m_body_xform
    void feed_bodies();                   // push m_disp_* + visibility/xform to the viewport
    void on_move_body();                  // start the move gizmo on the selected body
    void on_set_body_color();             // Color tool: pick a per-body display colour override
    int  tree_selection() const;          // selected feature row, or wxNOT_FOUND
    int  tree_body_selection() const;     // selected Parts-list body index, or -1
    void refresh_parts();                 // rebuild the Bodies list under the feature tree
    void sync_sidebar_width();            // keep the panel as wide as Prepare's sidebar
    void set_tree_selection(int row);
    static int tree_icon_for(CadFeatureType t);

    wxStaticText*     m_status{nullptr};
    wxStaticText*     m_dof_status{nullptr};   // DoF / constraint-state readout (P3)
    int               m_feature_counter{0};

    std::vector<wxButton*> m_confirm_btns;

    // Edit-in-place state: add-mode is m_edit_index == -1. Single-feature edit
    // (Sketch or Extrude independently) uses only m_edit_index as the row to replace.
    int               m_edit_index{-1};

    // Tree row of the sketch currently being constrained (-1 = not constraining).
    int               m_constrain_feat{-1};

    // Constraint-manager card (C3.4): header + a rebuildable list of constraint rows.
    wxSizer*          m_box_constraints{nullptr};
    wxStaticText*     m_hdr_constraints{nullptr};
    wxSizer*          m_constraint_rows{nullptr};
    int               m_constraint_sel{-1};   // highlighted constraint row, or -1
};

}} // namespace Slic3r::GUI

#endif // slic3r_DesignPanel_hpp_
