#ifndef slic3r_DesignPanel_hpp_
#define slic3r_DesignPanel_hpp_

#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/treebase.h>   // wxTreeItemId

#include <vector>
#include <memory>
#include <functional>
#include <map>

#include "libslic3r/CAD/CadDocument.hpp"

class ComboBox;    // Orca dropdown (Widgets/ComboBox.hpp) — replaces wxChoice everywhere here
class StaticBox;   // Orca rounded card frame (Widgets/StaticBox.hpp)
class wxCheckBox;
class wxCheckListBox;
class wxSpinCtrl;
class wxSpinCtrlDouble;
class wxTreeCtrl;
class wxImageList;
class wxStaticText;
class wxStaticLine;
class Button;      // Orca-styled button (Widgets/Button.hpp)
class CheckBox;    // Orca teal checkbox (Widgets/CheckBox.hpp)
class wxSizer;
// wxBoxSizer, wxTextCtrl and wxListCtrl are used here as pointers only, so a forward
// declaration is enough — but they must be declared. Every ordinary build happened to pull
// them in transitively through the wx/panel.h + wx/scrolwin.h chain. The Snapmaker fork's
// Flatpak build does not, and it failed to compile this header with "'wxTextCtrl' does not
// name a type; did you mean 'wxTreeCtrl'?". Declaring them keeps the header self-contained
// instead of relying on whatever a particular wx configuration happens to include.
class wxBoxSizer;
class wxTextCtrl;
class wxListCtrl;
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
    void on_tab_hidden();       // another tab took over: take the viewport status line down with us
    void unbind_canvas_event_handlers();   // app close / language switch, from the plater's teardown
    void reset_canvas_volumes();
    void clear_document();      // New Project / Open Project: drop the document with the project
    // Rebuild off the UI thread (progress dialog only if it turns out to be slow), so a feature
    // op on a heavy imported solid does not freeze the window. Returns m_doc.recompute()'s result.
    // Push the document's recipe into the Model so ANY save path persists it (snaporca-vjk5).
    void sync_recipe_to_model();
    bool recompute_guarded(const wxString& message);

    // MCP control hooks: let the external control server (McpControl.cpp) drive and
    // perceive the SAME kernel the GUI uses. Called only on the wx main thread.
    CadDocument& mcp_doc()        { return m_doc; }            // live document (read + mutate)
    void         mcp_after_change() { after_tree_edit(true); } // refresh tree + viewport + status
    DesignCanvas* mcp_viewport()  { return m_viewport; }        // live sketch + 3D view
    // Put the PANEL into (or out of) sketch mode, not just the canvas tool. Measured on the
    // rig: a sketch started straight through DesignCanvas::begin_sketch leaves m_ui_mode at
    // Feature, and the keyboard map is dispatched on `m_ui_mode == UiMode::Sketch` while the
    // offer menu is dispatched on the looser sketch_map_applies() — so the menu offered the
    // line's verbs while every sketch shortcut was dead (KEYTRACE: key=81 ui_mode=0
    // is_sketching=1). Half-entering a mode is worse than not entering it.
    void mcp_set_sketch_mode(bool on)
    {
        set_ui_mode(on ? UiMode::Sketch : UiMode::Feature);
        update_action_bar();
    }
    // The offer-table vocabulary without a right-click: the external controller asks which verbs
    // exist (and which apply to the current selection) and fires one by id, so a deck key names a
    // verb instead of spending a letter and every verb is reachable — including the rows with no
    // keyboard shortcut, which are otherwise invisible to anything that parses key tables.
    int  mcp_offer_selection_kind() const { return offer_selection_kind(); }  // OfferSel as int
    void mcp_run_action(const char* action) { run_offer_action(action); }     // dispatch an action string
    // Defined out of line in DesignPanel.cpp: it needs kOfferVerbs, which this header deliberately
    // does not include (the table is generated and belongs to the offer-menu code).
    bool mcp_run_verb(const char* verb_id);

private:
    enum class Tool { None, Sketch, Extrude, Dressup, Hole, Thread, Shell, Revolve, Sweep, Pattern, Plane, Loft, Draft, Boolean, Cut, Insert, Axis, CoordSys, SurfaceExtrude, SurfaceRevolve, SurfaceLoft, SurfaceFill, SurfaceOffset, ThickenSurface, Transform, Mirror, Thicken, Rib, Project, DeleteFace, Helix, Mate };
    // Which numeric fields an expression can be bound to, per feature type. A member rather
    // than a file-static helper so Tool — 32 values of purely internal card state — does not
    // have to become part of this panel's public API just to be named in a signature.
    static std::vector<std::string> fields_for_tool(Tool t);
    // Plane tool: which datum reference the next solid pick fills (declared early so the
    // method decls + card lambdas below can name it).
    enum class PlanePick { None, FaceA, FaceB, EdgeA, EdgeB };
    enum class AxisPick { None, Face, Edge };
    enum class CoordSysPick { None, Face, Edge };

    // Onshape-style contextual top toolbar: only the active mode's tool group is
    // shown (Feature = sketch/extrude/dress/hole/thread; Sketch = entity tools;
    // Constrain = constraints + edit ops). Replaces the old always-visible wall.
    enum class UiMode { Feature, Sketch, Constrain };
    void set_ui_mode(UiMode m);
    void apply_dof_status(int dof, bool ok, bool has_constraints);
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
    bool on_add_plane();   // false = refused, card stays open
    void arm_plane_pick(PlanePick target);   // Plane tool: next solid pick fills this reference
    void apply_plane_refs(CadFeature& f) const; // copy type + face/edge refs + sizes from the card
    void refresh_plane_labels();             // update the 4 pick labels from the captured refs
    void reset_plane_refs();                 // clear captured refs (fresh Plane add)
    void on_add_shell();
    void on_add_draft();
    void on_add_boolean();
    void on_add_cut();              // commit a plane Cut (split-by-plane)
    void on_add_axis();
    void arm_axis_pick(AxisPick target);
    void apply_axis_refs(CadFeature& f) const;
    void refresh_axis_labels();
    void reset_axis_refs();
    void on_add_coordsys();
    void arm_coordsys_pick(CoordSysPick target);
    void apply_coordsys_refs(CadFeature& f) const;
    void refresh_coordsys_labels();
    void refresh_cs_body_choice();   // fill the CoordSys body chooser from current document
    void reset_coordsys_refs();
    void on_add_surface_extrude();
    void on_add_surface_revolve();
    void on_add_surface_loft();
    void on_add_surface_fill();
    void on_add_surface_offset();
    void on_add_thicken_surface();
    void on_add_transform();
    void xf_live_preview();    // typed Transform fields -> body display transform (live)
    void xf_clear_preview();   // hand a previewed body back to its pre-card pose
    void on_add_mirror();
    void on_add_thicken();
    void on_add_rib();
    void on_add_project();
    void on_add_delete_face();
    void on_add_helix();
    void on_add_mate();
    void on_check_interference();
    void on_mass_properties();          // read-only report on the selected solid; edits nothing
    // Fill m_bool_target / m_bool_tool / m_cut_target. as_of_feature < 0 = current bodies (add);
    // >= 0 = the bodies as they existed just before that feature index (Boolean re-edit, so a
    // consumed tool body still appears and its saved selection round-trips).
    // Which body a tool should act on when it opens: the one picked in the VIEWPORT, else
    // the first. Selection comes first and the tool consumes it — every body combo used to
    // default to index 0, so picking body 3 and opening Mirror silently mirrored body 1.
    // Clamped to the list, so it is safe to hand straight to SetSelection. snaporca-e1p.
    int  selected_body_default() const;
    void populate_body_choices(int as_of_feature = -1);
    // Fill `c` with the bodies as they existed just before `as_of_feature` and select
    // `want`. Re-editing any feature that stores a body index needs this: the index was
    // recorded against the body list at that point in the timeline, not the final one.
    void fill_body_choice(ComboBox* c, int as_of_feature, int want);
    void populate_sheet_body_choices(ComboBox* c) const;   // bodies where is_sheet_shape() is true
    // Rows of a sheet-filtered picker are not body indices; go through these two, never
    // GetSelection()/SetSelection() directly.
    static int  sheet_choice_body(ComboBox* c);            // real body index of the current row, or -1
    static void select_sheet_choice(ComboBox* c, int body);// select the row holding this body index
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
    // Metadata/orca_cad.bin): deserialize -> recompute -> refresh viewport + tree.
    void load_recipe(const std::string& blob);
    void refresh_tree();
    void set_status_ok();

    // Feature-tree editing (Onshape-style): act on the selected tree row.
    void on_delete_feature();
    // "Delete Body" — the geometry-first counterpart, reached by pointing at a body or any of
    // its faces. Resolves the body to the feature that created it and removes THAT, because a
    // body is a recomputed result and has nothing else to delete.
    void on_delete_body();
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
    // Name the geometry the card has LATCHED, so it never has to be inferred from the viewport.
    // Pass -1 for "none, falling back to the plane dropdown". See snaporca-200.
    void        set_hole_target_label(int face);
    void        set_thread_target_label(int face, int edge);
    CadFeature build_candidate(Tool t) const;
    // Merge per-body ghost meshes with the per-body display transforms applied. The kernel builds
    // a ghost from the untransformed bodies, so without this it floats back at the origin once a
    // body has been moved.
    TriangleMesh ghost_from_bodies(const std::vector<TriangleMesh>& per_body) const;
    // A mate makes no new geometry but it MOVES a body, and the moved assembly is the ghost worth
    // showing. Used both by the Mate card and by hovering a row of the offer's mate palette.
    bool       show_mate_ghost(int kind, int cs_a, int cs_b,
                               double offset, double angle_deg, bool flip, std::string& err);
    int        resolve_extrude_sketch() const;
    // Plane pickers: fill a choice with XY/XZ/YZ + the document's datum planes, and
    // map a choice row back to the actual SketchPlane (rows 0-2 base, 3+ datum).
    void        populate_plane_choices(ComboBox* c) const;
    wxString    ref_plane_name(int row) const;   // "XY" / a datum's name, for the on-geometry hint
    SketchPlane plane_from_choice(int row) const;
    // Where a new sketch goes, resolved from what is SELECTED IN THE VIEWPORT rather than from a
    // list: a picked planar face wins, otherwise the reference plane last clicked in 3D. `what`
    // comes back as something to show the user, so the choice is visible without a combo.
    SketchPlane sketch_plane_from_selection(wxString& what) const;
    // Whether that resolution has anything the USER picked behind it, rather than the default
    // reference plane. Lets a caller say "sketching on XZ" only when it is actually true.
    bool sketch_plane_target(wxString& what) const;
    // True when Extrude should build only the click-selected loop (a region of the
    // resolved sketch is selected and it carries entities).
    bool       extrude_uses_loop() const;
    void       sync_sketch_display();   // push un-consumed committed sketches to the viewport
    // Feed the viewport's visual Extrude depth-arrow gizmo (C5b) with the current profile
    // plane + centroid + live depths while the Extrude card is open (self-gates on m_active).
    void       update_extrude_gizmo();
    void       update_fillet_gizmo();     // edge-anchored radius arrow (Dressup card)
    void       sync_dressup_target();     // Dressup card: show picked edge vs group, gate the combo
    void       update_hole_gizmo();       // footprint circle + diameter/depth arrows (Hole card)
    // A FEATURE button whose tool needs bodies it may not have yet. Greyed with an explanatory
    // tooltip below min_bodies, rather than accepting the click and refusing afterwards.
    struct BodyGate { wxWindow* btn{nullptr}; int min_bodies{1}; wxString tip_live, tip_gated; };
    std::vector<BodyGate> m_body_gates;
    void       update_body_gates();       // re-evaluate them against the current body count
    void       update_thread_gizmo();     // footprint circle + radius/length arrows (Thread card)
    void       update_shell_gizmo();      // inward thickness arrow on the picked face (Shell card)
    void       update_revolve_gizmo();    // angle-arc around the axis (Revolve card)
    void       update_draft_gizmo();      // angle-arc around the face centroid (Draft card)
    void       update_cut_gizmo();        // plane-rectangle + offset arrow (Cut card)
    void       update_operand_highlight(); // Boolean/Sweep/Loft operand tinting on the canvas
    void       update_pattern_gizmo();    // linear spacing arrow / circular angle-arc (Pattern card)
    void       update_datum_gizmo();      // resize handles on the datum plane being created/edited (C3)
    void       update_helix_gizmo();      // live helix curve + radius/height/pitch handles (Helix card)
    void       update_rib_gizmo();        // in-plane slab footprint + thickness handles (Rib card)
    void       refresh_datum_planes();    // push resolved datum frames + per-plane u/v extents to viewport
    void       refresh_mate_connectors(); // push connector frames so verse + polarity are visible
    void       update_reference_planes(); // persistent XY/XZ/YZ reference planes (fallback when no object)

    CadDocument m_doc;

    Tool      m_active{Tool::None};

    // Keyboard shortcuts (Onshape-style, three scoped layers). Keys are encoded as the
    // upper-cased letter, OR'd with 0x10000 when Shift is required. m_keys_sketch fires only
    // while a sketch is open (single letters = sketch tools); m_keys_feature fires only when
    // no sketch is open (Shift+letter = feature tools; single letters = view toggles/section).
    static constexpr int SC_SHIFT = 0x10000;
    // ...and with 0x20000 when Ctrl is required too. The Shift+letter space is full, so an
    // action that arrives late lives on Ctrl+Shift; plain Ctrl-combos are still passed
    // straight through, which is what leaves this layer free.
    static constexpr int SC_CTRL = 0x20000;
    std::map<int, std::function<void()>> m_keys_sketch;
    std::map<int, std::function<void()>> m_keys_feature;

    StaticBox* m_tree_box{nullptr};   // framed feature-tree section
    StaticBox* m_parts_box{nullptr};  // framed bodies section (hidden while empty)
    StaticBox* m_cards{nullptr};      // one framed panel holding every tool dialog (one visible at a time)
    void      update_cards_frame();     // show that frame iff some card inside it is visible
    void      show_move_card(bool show);
    void      apply_move_card();       // numeric move/rotate -> same xform the gizmo builds
    void      push_polygon_params();
    wxSizer*  m_tb_commit{nullptr};   // far-right Commit to Plate, beside Confirm/Cancel
    wxSizer*  m_tb_doc{nullptr};      // toolbar document/view actions (new, commit, export, section, place)
    CheckBox* m_show_bed{nullptr};    // view option: draw the printer bed + plate grid, or not
    wxSizer*  m_box_move{nullptr};      // Move/Rotate numeric options (distance, axis, angle)
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
    wxSizer*  m_box_axis{nullptr};
    wxSizer*  m_box_coordsys{nullptr};
    wxSizer*  m_box_surf_extrude{nullptr};
    wxSizer*  m_box_surf_revolve{nullptr};
    wxSizer*  m_box_surf_loft{nullptr};
    wxSizer*  m_box_surf_fill{nullptr};
    wxSizer*  m_box_surf_offset{nullptr};
    wxSizer*  m_box_surf_thicken{nullptr};
    wxSizer*  m_box_transform{nullptr};
    wxSizer*  m_box_mirror{nullptr};
    wxSizer*  m_box_thicken{nullptr};
    wxSizer*  m_box_rib{nullptr};
    wxSizer*  m_box_project{nullptr};
    wxSizer*  m_box_delete_face{nullptr};
    wxSizer*  m_box_helix{nullptr};
    wxSizer*  m_box_mate{nullptr};
    wxSizer*  m_box_insert{nullptr};   // Confirm/Cancel card for placing Text/SVG art
    wxSizer*  m_box_expr{nullptr};     // expression binding card (visible during edit only)
    int       m_insert_feat{-1};       // provisional imported-art feature awaiting Confirm
    // Move-body gizmo runs through the unified action bar too: Confirm keeps the placement,
    // Cancel reverts to the pose captured when the move started.
    int         m_move_body{-1};
    Transform3d m_move_prev{Transform3d::Identity()};
    // Set while the move gizmo is serving the Transform CARD rather than the Move button.
    // Both use the same gizmo; only this says which card owns the numbers it reports.
    int         m_xf_gizmo_body{-1};
    Transform3d m_xf_gizmo_base{Transform3d::Identity()};   // pose when Transform armed it
    // Which body the Transform card's typed fields are currently previewing on, and the pose to
    // hand it back to. Separate from the gizmo pair because the card can retarget its Body combo.
    int         m_xf_prev_body{-1};
    Transform3d m_xf_prev_base{Transform3d::Identity()};

    // Onshape-style dialog-card title rows (icon + bold feature name), retitled
    // per tool in open_tool() (edit-mode shows the feature's actual name).
    wxStaticText* m_hdr_move{nullptr};
    wxStaticText* m_hdr_sketch{nullptr};
    // Onshape sketch-entry card (plane/orientation) that opens on "New sketch" and
    // persists until Finish (Phase 3).
    wxSizer*      m_box_sketch_session{nullptr};
    wxStaticText* m_hdr_sketch_session{nullptr};
    wxStaticText* m_sketch_hint{nullptr};   // "click a plane" / "drawing on X" — must match the status
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
    wxStaticText* m_hdr_axis{nullptr};
    wxStaticText* m_hdr_coordsys{nullptr};
    wxStaticText* m_hdr_surf_extrude{nullptr};
    wxStaticText* m_hdr_surf_revolve{nullptr};
    wxStaticText* m_hdr_surf_loft{nullptr};
    wxStaticText* m_hdr_surf_fill{nullptr};
    wxStaticText* m_hdr_surf_offset{nullptr};
    wxStaticText* m_hdr_surf_thicken{nullptr};
    wxStaticText* m_hdr_transform{nullptr};
    wxStaticText* m_hdr_mirror{nullptr};
    wxStaticText* m_hdr_thicken{nullptr};
    wxStaticText* m_hdr_rib{nullptr};
    wxStaticText* m_hdr_project{nullptr};
    wxStaticText* m_hdr_delete_face{nullptr};
    wxStaticText* m_hdr_helix{nullptr};
    wxStaticText* m_hdr_mate{nullptr};
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
    wxSpinCtrlDouble* m_move_dx{nullptr};        // Move/Rotate card: world translation
    wxSpinCtrlDouble* m_move_dy{nullptr};
    wxSpinCtrlDouble* m_move_dz{nullptr};
    ComboBox*         m_move_axis{nullptr};      // rotation axis: X/Y/Z
    wxSpinCtrlDouble* m_move_angle{nullptr};     // rotation angle (deg)
    // Polygon's two parameters are chosen FROM THE TOOL, in the offer's Polygon submenu, not
    // from a card on the left: the side count cannot be edited after drawing (the inline editor
    // offers Side and Angle only), so it has to be settled at the moment the tool is armed —
    // which is exactly where the offer already is. snaporca-e1p.
    int               m_poly_sides{6};           // 3..64; the submenu names the common ones
    bool              m_poly_circumscribed{false};

    // Which reference plane a sketch falls back to when no face is picked: 0/1/2 = XY/XZ/YZ,
    // >=3 indexes resolve_datum_planes(). Set by CLICKING a ghost plane in the viewport — there is
    // deliberately no dropdown for it. snaporca-e1p.
    int               m_ref_plane{0};
    // m_ref_plane is always a VALID plane, so it cannot itself distinguish "the user chose XY"
    // from "nobody has chosen anything yet". This does.
    bool              m_plane_picked{false};
    ComboBox*         m_shape{nullptr};
    ComboBox*         m_mode{nullptr};
    wxSpinCtrlDouble* m_width{nullptr};
    wxSpinCtrlDouble* m_height{nullptr};
    wxSpinCtrlDouble* m_radius{nullptr};
    wxSpinCtrlDouble* m_distance{nullptr};
    ComboBox*         m_extrude_end{nullptr};   // Blind/Symmetric/TwoSided/ThroughAll/UpTo*
    wxSpinCtrlDouble* m_distance2{nullptr};     // second-side depth (Two-sided)
    wxSpinCtrlDouble* m_taper{nullptr};         // draft angle (deg)
    CheckBox*         m_flip{nullptr};          // reverse extrude direction

    wxStaticText*     m_extrude_sketch_label{nullptr};
    int               m_extrude_sketch_ref{-1};

    // Revolve controls (sweep a sketch profile about an in-plane axis).
    wxStaticText*     m_revolve_sketch_label{nullptr};
    wxSpinCtrlDouble* m_revolve_angle{nullptr};
    ComboBox*         m_revolve_axis{nullptr};   // 0 = plane X, 1 = plane Y
    ComboBox*         m_revolve_mode{nullptr};   // New/Add/Cut/Intersect
    CheckBox*         m_revolve_flip{nullptr};
    int               m_revolve_sketch_ref{-1};

    // Sweep controls (sweep a profile sketch along a path sketch).
    wxStaticText*     m_sweep_profile_label{nullptr};
    ComboBox*         m_sweep_path{nullptr};       // path Sketch picker (feature index in client data)
    ComboBox*         m_sweep_mode{nullptr};       // New/Add/Cut/Intersect
    int               m_sweep_profile_ref{-1};
    int               m_sweep_path_ref{-1};        // path Sketch feature index (for re-edit pre-select)

    // Loft controls (skin a solid through 2+ ordered profile Sketches).
    wxCheckListBox*   m_loft_list{nullptr};        // every Sketch; check 2+ in list order = profiles
    CheckBox*         m_loft_ruled{nullptr};       // ruled (straight) vs smooth sections
    ComboBox*         m_loft_mode{nullptr};        // New/Add/Cut/Intersect
    std::vector<int>  m_loft_sketch_idx;           // feature index for each row in m_loft_list
    std::vector<int>  m_loft_refs;                 // chosen profile refs (for re-edit pre-check)

    // Surface Extrude controls (sheet from sketch profile).
    wxStaticText*     m_surf_extrude_sketch_label{nullptr};
    wxSpinCtrlDouble* m_surf_extrude_distance{nullptr};
    int               m_surf_extrude_sketch_ref{-1};

    // Surface Revolve controls (sheet from sketch about axis).
    wxStaticText*     m_surf_revolve_sketch_label{nullptr};
    wxSpinCtrlDouble* m_surf_revolve_angle{nullptr};
    ComboBox*         m_surf_revolve_axis{nullptr};   // 0 = plane X, 1 = plane Y
    CheckBox*         m_surf_revolve_flip{nullptr};
    int               m_surf_revolve_sketch_ref{-1};

    // Surface Loft controls (skin a sheet through 2+ ordered profile sketches).
    wxCheckListBox*   m_surf_loft_list{nullptr};        // every Sketch; check 2+ in list order = profiles
    CheckBox*         m_surf_loft_ruled{nullptr};       // ruled (straight) vs smooth sections
    std::vector<int>  m_surf_loft_sketch_idx;           // feature index for each row
    std::vector<int>  m_surf_loft_refs;                 // chosen profile refs (for re-edit pre-check)

    // Surface Fill controls (one-face sheet from a sketch boundary).
    wxStaticText*     m_surf_fill_sketch_label{nullptr};
    int               m_surf_fill_sketch_ref{-1};

    // Surface Offset controls (offset a SHEET body).
    ComboBox*         m_surf_offset_body{nullptr};       // sheet-body picker
    wxSpinCtrlDouble* m_surf_offset_distance{nullptr};

    // Thicken Surface controls (thicken a SHEET body into a solid).
    ComboBox*         m_surf_thicken_body{nullptr};      // sheet-body picker
    wxSpinCtrlDouble* m_surf_thicken_thickness{nullptr};
    CheckBox*         m_surf_thicken_flip{nullptr};

    // Transform controls (rigid move/rotate of a body).
    ComboBox*         m_xf_body{nullptr};            // body to transform
    wxSpinCtrlDouble* m_xf_dx{nullptr};              // translate X
    wxSpinCtrlDouble* m_xf_dy{nullptr};              // translate Y
    wxSpinCtrlDouble* m_xf_dz{nullptr};              // translate Z
    ComboBox*         m_xf_axis{nullptr};            // rotation axis: X/Y/Z
    wxSpinCtrlDouble* m_xf_angle{nullptr};           // rotation angle (deg)
    wxSpinCtrlDouble* m_xf_pivot_x{nullptr};         // pivot X
    wxSpinCtrlDouble* m_xf_pivot_y{nullptr};         // pivot Y
    wxSpinCtrlDouble* m_xf_pivot_z{nullptr};         // pivot Z
    CheckBox*         m_xf_copy{nullptr};            // keep original (make a copy)

    // Mirror controls (reflect a body about a plane).
    ComboBox*         m_mirror_body{nullptr};        // body to mirror
    ComboBox*         m_mirror_plane{nullptr};       // mirror plane (XY/XZ/YZ + datums)
    CheckBox*         m_mirror_keep{nullptr};        // keep original body

    // Thicken controls (offset one solid face into a thin plate).
    ComboBox*         m_thicken_body{nullptr};       // source body
    wxStaticText*     m_thicken_face_label{nullptr}; // picked face
    wxSpinCtrlDouble* m_thicken_thickness{nullptr};
    CheckBox*         m_thicken_flip{nullptr};       // flip direction

    // Rib controls (thin wall from an open sketch line).
    ComboBox*         m_rib_body{nullptr};           // target body
    ComboBox*         m_rib_sketch{nullptr};         // sketch holding the open line (feature index in client data)
    wxSpinCtrl*       m_rib_entity{nullptr};         // entity index within the sketch
    wxSpinCtrlDouble* m_rib_thickness{nullptr};
    wxSpinCtrlDouble* m_rib_depth{nullptr};

    // Project controls (project body edges onto a plane as sketch entities).
    ComboBox*         m_proj_source_body{nullptr};   // source body
    wxStaticText*     m_proj_face_label{nullptr};    // picked face (or "all edges")
    ComboBox*         m_proj_plane{nullptr};         // target plane

    // Delete Face controls (remove faces, heal the solid).
    ComboBox*         m_del_face_body{nullptr};      // target body
    wxButton*         m_del_face_add_btn{nullptr};   // "Add picked face" button
    wxStaticText*     m_del_face_list{nullptr};      // shows the accumulated face ids
    std::vector<int>  m_del_faces;                   // accumulated face list

    // Helix controls (helical curve).
    ComboBox*         m_helix_plane{nullptr};        // axis plane (XY/XZ/YZ + datums)
    wxSpinCtrlDouble* m_helix_radius{nullptr};
    wxSpinCtrlDouble* m_helix_pitch{nullptr};
    wxSpinCtrlDouble* m_helix_height{nullptr};
    CheckBox*         m_helix_left_handed{nullptr};
    wxSpinCtrlDouble* m_helix_taper{nullptr};

    // Mate (assembly) controls
    ComboBox*         m_mate_kind{nullptr};
    ComboBox*         m_mate_cs_a{nullptr};
    ComboBox*         m_mate_cs_b{nullptr};
    wxSpinCtrlDouble* m_mate_offset{nullptr};
    wxSpinCtrlDouble* m_mate_angle{nullptr};
    CheckBox*         m_mate_flip{nullptr};
    wxStaticText*     m_offset_label{nullptr};
    wxStaticText*     m_angle_label{nullptr};

    // Expression binding (per-feature, visible during edit only)
    ComboBox*         m_expr_field{nullptr};     // field-name picker (editable)
    wxTextCtrl*       m_expr_text{nullptr};      // expression string
    wxButton*         m_expr_set_btn{nullptr};   // Apply / bind
    wxButton*         m_expr_clear_btn{nullptr}; // Remove binding
    wxStaticText*     m_expr_status{nullptr};    // shows current bindings for the edited feature
    void              populate_expr_fields(Tool t);   // fill m_expr_field from feature-type fields
    void              on_set_expr();                   // checkpoint + write -> recompute -> undo on fail
    void              on_clear_expr();                 // remove selected binding

    // Document variables panel (below the feature tree / parts)
    StaticBox*        m_var_box{nullptr};
    wxListCtrl*       m_var_list{nullptr};
    wxButton*         m_btn_add_var{nullptr};
    wxButton*         m_btn_edit_var{nullptr};
    wxButton*         m_btn_del_var{nullptr};
    void              refresh_variables();            // rebuild m_var_list from m_doc.variables
    void              on_add_variable();
    void              on_edit_variable();
    void              on_remove_variable();

    // Feature-tree button
    ScalableButton*   m_btn_interfere{nullptr};

    // Pattern controls (replicate the target body: linear or circular).
    ComboBox*         m_pattern_type{nullptr};      // 0 = Linear, 1 = Circular
    wxSpinCtrlDouble* m_pattern_count{nullptr};     // total instances incl. seed
    wxSpinCtrlDouble* m_pattern_spacing{nullptr};   // linear step (mm)
    ComboBox*         m_pattern_dir{nullptr};        // linear direction: 0 = plane X, 1 = plane Y
    wxSpinCtrlDouble* m_pattern_angle{nullptr};     // circular total angle (deg)
    // Boolean controls (combine two existing bodies).
    ComboBox*         m_bool_op{nullptr};            // 0 = Union, 1 = Subtract, 2 = Intersect
    ComboBox*         m_bool_target{nullptr};        // body that survives (selection == body index)
    ComboBox*         m_bool_tool{nullptr};          // body consumed (selection == body index)
    // Which operand the NEXT viewport body pick fills: 0 = target, 1 = tool. Reset when the
    // card opens, so the first two clicks in the viewport always mean "keep this, cut with
    // that" in that order. The combos remain the typed half and mirror whatever is picked.
    int               m_bool_next_slot{0};
    CheckBox*         m_bool_keep{nullptr};          // keep the tool body after the op
    wxSpinCtrlDouble* m_bool_tol{nullptr};           // OCCT fuzzy tolerance (mm); robust cut on near-coincident faces

    // Plane Cut (split-by-plane): a reference plane + offset splits the target body into
    // two separate bodies (both pieces kept).
    ComboBox*         m_cut_plane{nullptr};          // XY/XZ/YZ + datum planes (cut plane)
    ComboBox*         m_cut_target{nullptr};         // body to cut (selection == body index)
    wxSpinCtrlDouble* m_cut_offset{nullptr};         // offset along the plane normal (mm)
    // Datum plane controls (derive a selectable sketch plane: offset + tilt from a base).
    ComboBox*         m_plane_base{nullptr};         // 0=XY,1=XZ,2=YZ, 3+N = Nth datum plane
    wxSpinCtrlDouble* m_plane_offset{nullptr};       // offset along base normal (mm)
    wxSpinCtrlDouble* m_plane_tilt{nullptr};         // tilt about a base axis (deg) / Angle / Tangent angle
    ComboBox*         m_plane_tilt_axis{nullptr};    // 0 = base X, 1 = base Y
    // Plane construction method + contextual face/edge reference picks (Onshape/Fusion parity).
    ComboBox*         m_plane_type{nullptr};         // PlaneType: Offset/Angle/Midplane/Tangent/TwoEdges/Coincident
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
    bool              m_sel_solid_vertex{false};   // a corner is picked (body+point, no face/edge)
    // The face actually under the last solid click, INDEPENDENT of the whole/face/edge cycle level.
    // The first click on a solid selects the WHOLE body, but the ray has already resolved which face
    // it hit and the callback passes it. "Sketch on the face I clicked" must not require discovering
    // that a second click refines the selection, so keep it instead of throwing it away. snaporca-3a2.
    int               m_pick_face_body{-1};
    int               m_pick_face{-1};
    // What the live sketch was actually opened on ("the picked face", "XY", a datum's name), so the
    // hint can say it. Resolved from the selection at begin_sketch, not read back from a combo.
    wxString          m_sketch_on;
    // --- the object-driven offer (charter 4.1) ---------------------------------------------
    // Right-click the geometry -> a vertical list in ratified row order, verbs that do not
    // apply disabled IN PLACE with their reason. The rows come from the generated table in
    // DesignOffer.hpp; this map is how a row reaches the code that already implements it, for
    // the verbs that have no keyboard shortcut to route through.
    std::map<std::string, std::function<void()>> m_verb_actions;
    // Append an offer row with its toolbar glyph. The bitmap must be set BEFORE Append —
    // wxGTK builds the GtkMenuItem there and only makes an image item if one is present.
    // Every status write goes through here so long hints wrap instead of clipping.
    void        set_status(const wxString& text);
    wxString    idle_hint() const;   // what to say when nothing is selected
    // Reason detect_mate_conflicts() recorded for a feature, or nullptr. Marks the tree row and
    // feeds the status line; a conflict is a diagnostic, not a document error.
    const std::string* mate_conflict_reason(int feature) const;

    wxMenuItem* append_offer_item(wxMenu* menu, int id, const wxString& text,
                                  const struct OfferVerb& v);
    void show_offer_menu(const wxPoint& screen_pos);
    // Where the offer opens when no mouse press anchors it: the keyboard route, and the automatic
    // open on entering Sketch. The pointer if it is over the viewport, else the viewport's centre.
    // A raw wxGetMousePosition() can be sitting on the toolbar, on the card column or on another
    // monitor, and the menu would map there — detached from the geometry it is about.
    wxPoint offer_anchor() const;
    int  offer_selection_kind() const;          // an OfferSel, as int to keep the header light
    // Does the SKETCH half of the map apply? A mode question, not a session one: begin_sketch
    // does not run until the first tool is armed, so between "press Sketch" and "pick a tool"
    // is_sketching() is still false — precisely when the drawing tools must be on offer. The
    // is_sketching() arm covers re-opening a committed sketch, which enters the session first.
    bool sketch_map_applies() const;
    void run_offer_action(const char* action);
    // Face-as-profile extrude (Onshape): when Extrude is opened on a picked solid face with
    // no sketch source, this carries that global face id so the kernel extrudes the face.
    // -1 = ordinary sketch/loop extrude. Set when opening the Extrude card, consumed on add.
    int               m_extrude_face_src{-1};

    ComboBox*         m_dressup_type{nullptr};
    ComboBox*         m_face_group{nullptr};
    wxSpinCtrlDouble* m_dressup_size{nullptr};
    wxStaticText*     m_dressup_edge_label{nullptr}; // shows the picked edge, or the group fallback

    ComboBox*         m_hole_plane{nullptr};
    wxSpinCtrlDouble* m_hole_diameter{nullptr};
    wxSpinCtrlDouble* m_hole_depth{nullptr};
    CheckBox*         m_hole_through{nullptr};
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
    // Says which face the latch above is holding. Thicken/Shell/Draft show theirs because their
    // face IS the live selection; this one has to be shown precisely BECAUSE it is not, and the
    // status line goes on saying "Nothing selected" while the ghost keeps drilling. snaporca-200.
    wxStaticText*     m_hole_target_label{nullptr};

    ComboBox*         m_thread_plane{nullptr};
    ComboBox*         m_thread_std{nullptr};   // standard designation (M6, 1/4-20 UNC, ...)
    wxSpinCtrlDouble* m_thread_radius{nullptr};
    wxSpinCtrlDouble* m_thread_pitch{nullptr};
    wxSpinCtrlDouble* m_thread_height{nullptr};
    wxSpinCtrlDouble* m_thread_depth{nullptr};
    CheckBox*         m_thread_internal{nullptr};
    wxSpinCtrlDouble* m_thread_x{nullptr};
    wxSpinCtrlDouble* m_thread_y{nullptr};
    // #3: when the Thread tool is opened on a picked cylindrical face (a hole bore or a
    // cylinder), thread that surface — plane on its axis, radius/internal derived from it.
    bool              m_thread_on_face{false};
    SketchPlane       m_thread_face_plane;
    int               m_thread_face_body{-1};
    wxStaticText*     m_thread_target_label{nullptr};   // the latched face/edge — see m_hole_target_label

    wxSpinCtrlDouble* m_shell_thickness{nullptr};
    wxStaticText*     m_shell_face_label{nullptr};   // shows the picked face to remove

    // Draft controls (taper a single picked solid face about the body bottom).
    wxSpinCtrlDouble* m_draft_angle{nullptr};
    wxStaticText*     m_draft_face_label{nullptr};   // shows the picked face to draft

    // Axis controls (datum axis: line through two points or derived from geometry).
    ComboBox*         m_axis_type{nullptr};          // AxisType: TwoPoints/FaceNormal/CylinderCenterline/PlaneIntersection/AlongEdge
    wxButton*         m_axis_pick_face{nullptr};     wxStaticText* m_axis_face_lbl{nullptr};
    wxButton*         m_axis_pick_edge{nullptr};     wxStaticText* m_axis_edge_lbl{nullptr};
    ComboBox*         m_axis_plane_a{nullptr};
    ComboBox*         m_axis_plane_b{nullptr};
    wxSpinCtrlDouble* m_axis_p1x{nullptr};           wxSpinCtrlDouble* m_axis_p1y{nullptr};           wxSpinCtrlDouble* m_axis_p1z{nullptr};
    wxSpinCtrlDouble* m_axis_p2x{nullptr};           wxSpinCtrlDouble* m_axis_p2y{nullptr};           wxSpinCtrlDouble* m_axis_p2z{nullptr};
    int m_ax_face_body{-1}, m_ax_face{-1};
    int m_ax_edge_body{-1}, m_ax_edge{-1};
    AxisPick m_axis_pick{AxisPick::None};

    // CoordSys controls (datum coordinate system: point + orthonormal frame).
    ComboBox*         m_coordsys_type{nullptr};      // CoordSysType: PointWorld/FaceAndDirection
    ComboBox*         m_cs_body{nullptr};             // body-focus chooser: restrict picking to one body
    wxSpinCtrlDouble* m_cs_x{nullptr};               wxSpinCtrlDouble* m_cs_y{nullptr};               wxSpinCtrlDouble* m_cs_z{nullptr};
    wxButton*         m_cs_pick_face{nullptr};       wxStaticText* m_cs_face_lbl{nullptr};
    wxButton*         m_cs_pick_edge{nullptr};       wxStaticText* m_cs_edge_lbl{nullptr};
    wxSpinCtrlDouble* m_cs_hx{nullptr};              wxSpinCtrlDouble* m_cs_hy{nullptr};              wxSpinCtrlDouble* m_cs_hz{nullptr};
    int m_cs_face_body{-1}, m_cs_face{-1};
    int m_cs_edge_body{-1}, m_cs_edge{-1};
    CoordSysPick m_coordsys_pick{CoordSysPick::None};

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
    ScalableButton* m_section_flip_btn{nullptr};  // toolbar action; enabled only while the section is on
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
    void arm_transform_gizmo();           // arm the move gizmo on the Transform card's body (add mode only)
    void on_set_body_color();             // Color tool: pick a per-body display colour override
    int  tree_selection() const;          // selected feature row, or wxNOT_FOUND
    int  tree_body_selection() const;     // selected Parts-list body index, or -1
    void refresh_parts();                 // rebuild the Bodies list under the feature tree
    void sync_sidebar_width();            // keep the panel as wide as Prepare's sidebar
    void set_tree_selection(int row);
    static int tree_icon_for(CadFeatureType t);

    wxStaticText*     m_status{nullptr};
    // m_status's foreground as created, captured before any caller touches it. Callers signal
    // "no opinion" by setting wxNullColour, which restores exactly this — so it is the only
    // reliable way to tell a chosen colour (the error red) from the default. See set_status().
    wxColour          m_status_default_fg;
    // The guidance sentence for the step the armed sketch tool is on, kept so a transient
    // readout (the live length/angle while a segment is being dragged) can be appended to it
    // instead of replacing it — the guidance used to vanish on the first mouse move after a
    // click, which is precisely when it is needed. snaporca-1c0c.
    wxString          m_sketch_step;
    // mode is a DesignSketchTool::Mode; passed as an int because this header deliberately does
    // not include the tool's, and the .cpp (which does) casts it back.
    void              on_sketch_step(int mode, int step, int picks);
    wxStaticText*     m_dof_status{nullptr};   // DoF / constraint-state readout (P3)
    // Last live-solve result, so entering Constrain can restore the readout without a solve.
    int               m_dof_last{-1};
    bool              m_dof_last_ok{true};
    bool              m_dof_last_has{false};
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
