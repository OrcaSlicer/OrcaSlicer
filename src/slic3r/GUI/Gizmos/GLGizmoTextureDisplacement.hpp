#ifndef slic3r_GLGizmoTextureDisplacement_hpp_
#define slic3r_GLGizmoTextureDisplacement_hpp_

#include "GLGizmoPainterBase.hpp"
#include "libslic3r/TextureDisplacement.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "slic3r/GUI/GLTexture.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/IconManager.hpp"
#include "slic3r/GUI/TextureLibrary.hpp"

#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <string>

namespace Slic3r::GUI {

class TextureProjectorFrame;

// Paint-style gizmo that assigns one or more texture-displacement "layers" (see
// libslic3r/TextureDisplacement.hpp) to painted areas of a model, and can bake the result into
// real mesh geometry. See the project plan for the overall architecture; in short:
//  - each layer owns its own independent paint mask (ModelVolume::texture_displacement_facets),
//    reusing the same TriangleSelector/FacetsAnnotation machinery as every other paint gizmo -
//    only one layer is "active" (paintable) at a time, selected in the panel below;
//  - "Bake" runs build_texture_displacement() in a background job and commits the result exactly
//    like the Emboss/SVG "project on surface" gizmo does.
class GLGizmoTextureDisplacement : public GLGizmoPainterBase
{
public:
    GLGizmoTextureDisplacement(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);

    // The whole of mesh preparation - remesh, carry the paint across, refine where the texture bends -
    // as one pure function over plain data: no ModelVolume, no Model, no GUI, no undo. That is what lets
    // TextureDisplacementPrepareJob run it on the job worker instead of on the UI thread, where CGAL's
    // remesher and a several-hundred-thousand-triangle refinement together freeze the window for tens
    // of seconds with nothing to look at and no way to cancel.
    //
    // `progress` is called with 0..100 and aborts the run when it returns false; an aborted run reports
    // an empty result. An empty result is also how "nothing needed doing" is reported - see
    // TextureDisplacementPrepareResult.
    static TextureDisplacementPrepareResult prepare_mesh(const indexed_triangle_set                  &base,
                                                         const TextureDisplacementFacetsData         &masks,
                                                         const std::vector<TextureDisplacementLayer> &layers,
                                                         const TextureDisplacementPrepareParams      &params,
                                                         const std::vector<PrintableColor>           &palette,
                                                         const DisplacementProgressFn                &progress);
    // The volume's eight texture-displacement masks, gathered into the array every pure function here
    // (and every job input) takes.
    static TextureDisplacementFacetsData facets_data_of(const ModelVolume &mv);

    using PaletteEntry = PrintableColor;

    // The printable palette: the loaded filaments (clamped to the sixteen mmu_segmentation_facets can
    // address), plus - when `mixing` - every pair of them at evenly spaced ratios.
    //
    // Mixes are averaged in **CIELAB**, not RGB and not subtractively: two filaments interleaved too
    // finely to resolve are averaged by the eye, which is what a perceptual space models. Yellow and
    // blue banded together read as a desaturated grey-green, and that is what the preview must promise
    // - blending them subtractively would show a green the printer cannot produce this way.
    //
    // How many ratios depends on how many filaments there are, so the palette stays bounded: the
    // quantizer's lookup cube costs one DeltaE00 per cell per entry to fill, and with sixteen
    // filaments there are already plenty of colours without mixing any of them.
    static std::vector<PaletteEntry> make_palette(const std::vector<ColorRGBA> &filaments, bool mixing);

    // Maps an image colour to the closest entry of `palette`, perceptually (CIEDE2000 over CIELAB - a
    // plain RGB distance picks visibly wrong filaments, most obviously between a saturated colour and
    // a grey of similar brightness).
    //
    // Precomputed into a lookup cube rather than matched per call: the subdivision's colour criterion
    // samples up to seven points per triangle and re-samples both children of every split, so a live
    // match would dominate the refinement. The returned closure owns the cube, so it is safe to hand
    // to a worker thread and outlives the palette it was built from.
    static ColorQuantizeFn make_palette_quantizer(const std::vector<PaletteEntry> &palette);

    // Turns a palette index plus a position into the filament to print there, interleaving the two
    // filaments of a mixed entry per `mode`. `layer_height` sizes the Z bands; `cell_mm` the dither
    // cells. See ColorResolveFn for why this is separate from the quantizer.
    static ColorResolveFn make_mix_resolver(const std::vector<PaletteEntry> &palette, ColorMixMode mode,
                                            float layer_height, float cell_mm);

    // Everything the jobs need to colour with, for the current volume: palette, mix mode, layer
    // height, despeckle. Empty when no layer is actually colouring.
    TextureColorSettings color_settings_for(const ModelVolume &mv);

    // The printable palette for the current filaments and mixing setting, rebuilt only when either
    // actually changes - see the definition for why that caching is not optional.
    const std::vector<PaletteEntry> &cached_palette();
    std::vector<PaletteEntry>  m_palette_cache;
    std::vector<ColorRGBA>     m_palette_filaments;
    bool                       m_palette_mixing = false;
    ColorQuantizeFn            m_palette_quantizer;

    // The loaded filaments, clamped to the sixteen mmu_segmentation_facets can address.
    static std::vector<ColorRGBA> filament_palette();
    // The print's layer height, which sizes ColorMixMode::ZBands. Falls back to 0.2 mm if it cannot be
    // read - a wrong band size is a cosmetic error, not a reason to refuse to colour anything.
    static float print_layer_height();

    // The Normal preview's triangles, grouped by the filament they will print in. Colour is per facet
    // and there are at most sixteen filaments, so the mesh is uploaded once with its index buffer
    // sorted by colour and drawn as one GLModel::render(range) per group - which needs no per-vertex
    // colour attribute, and so no change to GLModel's vertex layouts.
    //
    // The *index buffer* is what gets reordered, never m_preview_its: the paint overlay and the
    // wireframe index into that by the volume's own triangle numbering (the bake is
    // topology-preserving), and permuting it would silently misplace both.
    struct PreviewColorRun
    {
        std::pair<size_t, size_t> range; // into the GLModel's index buffer, in elements
        ColorRGBA                 color;
    };
    std::vector<PreviewColorRun> m_preview_color_runs;
    // True if any of the volume's layers would actually colour something: colour turned on, and a
    // texture that has colour to give. What decides whether a palette is captured into a job at all,
    // and so whether the colour criterion and the mmu write ever run.
    static bool any_layer_colors(const ModelVolume &mv);

    void render_painter_gizmo() override;

    // Intercepts mouse input while "Adjust Texture" mode is on (dragging the on-canvas offset/
    // rotation handles instead of painting); otherwise forwards to the normal painting handling.
    bool on_mouse(const wxMouseEvent &mouse_event) override;

protected:
    void        on_render_input_window(float x, float y, float bottom_limit) override;
    std::string on_get_name() const override;

    wxString handle_snapshot_action_name(bool shift_down, Button button_down) const override;

    std::string get_gizmo_entering_text() const override { return _u8L("Entering Texture displacement painting"); }
    std::string get_gizmo_leaving_text() const override { return _u8L("Leaving Texture displacement painting"); }
    std::string get_action_snapshot_name() const override { return _u8L("Texture displacement editing"); }

    EnforcerBlockerType get_left_button_state_type() const override { return EnforcerBlockerType::ENFORCER; }
    EnforcerBlockerType get_right_button_state_type() const override { return EnforcerBlockerType::NONE; }

private:
    bool on_init() override;
    void update_model_object() override;
    void update_from_model_object(bool first_update) override;
    void on_opening() override {}
    void on_shutdown() override;
    PainterGizmoType get_painter_type() const override;

    // Phase 1 restricts the texture layer list to the first model-part volume of the current
    // object (the common single-volume case); multi-part objects only get texture layers on
    // their first part until a later phase. Returns nullptr if there is no model part.
    ModelVolume*       texture_volume();
    const ModelVolume* texture_volume() const;

    void add_texture_layer();
    void remove_texture_layer(int slot);
    void set_active_layer(int slot); // flushes the previous layer's edits, then reloads selectors
    // `own_snapshot` false when the caller has already taken an undo step that is meant to cover the
    // displacement too - see bake_standard().
    void bake(bool own_snapshot = true);

    // Standard (0) vs Pro (1), driven by the two-position slider in the panel header.
    //
    // Pro is the panel as it has always been: every geometry-preparation control is visible and the
    // user drives Remesh, Subdivide and Bake themselves, in whatever order they like. Standard hides
    // all of that, pins it to one fixed recipe, and folds it into the Bake button - paint, press Bake,
    // done - so the common case does not require knowing that a height map can only move vertices that
    // already exist. Nothing about Pro changed when Standard was added.
    int  m_panel_mode = 0;
    bool pro_mode() const { return m_panel_mode != 0; }
    // Pins every control Standard mode hides to its preset value. Idempotent, called each frame while
    // Standard is active so what Preview shows is always what Bake will do. Returns true if it actually
    // changed something, so the caller can invalidate the preview.
    bool apply_standard_mode_presets(ModelVolume *mv);
    // Standard mode's Bake: remesh to an even density, refine where the texture bends, then displace.
    // The order matters and is the whole reason this is one button - a height map can only move
    // existing vertices, so the mesh has to be prepared first, and remeshing after painting would drop
    // the paint if it were not carried across (see prepare_mesh()).
    void bake_standard();

    // Queues one prepare_mesh() run on the job worker and commits its result when it lands. Every
    // mesh-preparation button goes through here - Pro's Remesh, Pro's adaptive Subdivide and Standard's
    // Bake differ only in which stages `params` enables and in what happens afterwards:
    //  - `snapshot_name` is the single undo step the commit opens. With `then_bake` it is also the step
    //    the displacement job that follows commits into, rather than pushing its own - an undo landing
    //    between the two would leave a mesh carrying every added triangle and no relief on it, and
    //    baking again from there would prepare it a second time.
    //  - `unchanged_msg`, when not empty, is shown if the run had nothing to do. Standard's Bake passes
    //    nothing: a mesh that already meets the criteria is not an error there, it just goes straight
    //    on to the displacement.
    void queue_prepare(const TextureDisplacementPrepareParams &params, const std::string &snapshot_name,
                       bool then_bake, const std::string &unchanged_msg);
    // Set from queue_prepare() until its job's result has been committed. Distinct from
    // m_bake_in_progress because Standard's Bake sets both in turn, and because every button that would
    // read or replace the mesh has to stay disabled for the whole of it.
    bool m_prepare_in_progress = false;

    // How one layer's paint sits on the pre-subdivision mesh, precise enough to carry across the
    // refinement without rounding each source triangle to wholly painted or not.
    //
    // Rounding is what made the outline of a painted region come out ragged: a source triangle near a
    // smooth brush boundary is wholly painted essentially at random, so "painted iff the source was
    // full" turns a clean curve into a noisy fringe of isolated painted and unpainted triangles - and
    // once the border band refines the mesh there, that fringe is reproduced faithfully instead of
    // being blurred away by coarse geometry.
    struct LayerPaintMap
    {
        std::vector<uint8_t>              full;       // per source triangle: covered edge to edge
        std::vector<int>                  part_start; // CSR offsets into `part`, size (source tris + 1)
        std::vector<std::array<Vec3f, 3>> part;       // painted pieces of partly covered source triangles
        bool empty() const { return full.empty(); }
    };
    // Rebuilds every layer's mask on a subdivided mesh from `source` (new triangle -> the input triangle
    // it descends from) and the pre-subdivision coverage in `paint`.
    static TextureDisplacementFacetsData masks_after_subdivision(
        const TriangleMesh &new_mesh, const std::vector<int> &source,
        const std::array<LayerPaintMap, TEXTURE_DISPLACEMENT_MAX_LAYERS> &paint);
    // False means the remesh failed or changed nothing (CGAL signals failure by handing the input back),
    // and `out` must not be used. `target_edge_mm` is a request rather than a promise: it is clamped
    // against the part's surface area first, because CGAL's cost grows with the square of 1/target.
    static bool plan_remesh(const indexed_triangle_set &src, float target_edge_mm, float sharp_angle_deg,
                            indexed_triangle_set &out);

    // Marks every facet of every model-part volume as painted for the currently active layer -
    // "whole model" as an alternative to brushing/clicking every triangle by hand.
    void select_whole_model();

    // The mesh raycasters are built one per model-part volume, in that order; this is the texture
    // volume's slot among them, or -1 if it has none (no selection, or the lists disagree).
    int texture_volume_raycaster_index() const;

    // Paints exactly the facets currently visible from the camera onto the active layer, replacing
    // whatever that layer had painted. "Visible" is two tests: the facet faces the camera, and its
    // centroid is not hidden behind other geometry (a real raycast, so a concave part's far inner
    // wall is correctly excluded). When `uv_clip` is given (the projection frame's matrix), facets
    // whose centroid falls outside the frame's uv unit square are skipped first - which both clips
    // the selection to the frame and spares the raycast for everything outside it. Costs one ray
    // query per surviving facet, so it is a one-shot action, never a per-frame one. Returns the
    // number of facets selected.
    int select_visible_faces(const std::array<float, 12> *uv_clip = nullptr);

    // When set, "Capture current view" also re-selects the visible faces, so the viewpoint the
    // projector was captured from and the area it projects onto stay the same. Independent of the
    // projection frame below: this takes every visible facet, the frame clips to its rectangle.
    // Off by default, because turning it on replaces whatever the layer had painted.
    bool m_project_only_visible = false;

    // The projection-frame overlay for a ViewProjected layer: a semi-transparent window dragged over
    // the 3D view whose border becomes the projection's edge. Created lazily and owned here; hidden
    // rather than destroyed when closed, so reopening keeps it where the user left it.
    TextureProjectorFrame *m_projector_frame   = nullptr;
    int                    m_projector_opacity = 140;
    // What the overlay's texture was last built from, so repeated updates don't rebuild the bitmap
    // from unchanged pixels. Same shape as the m_thumbnail_source/m_thumbnail_smoothing pair above.
    const void *m_projector_tex_source    = nullptr;
    float       m_projector_tex_smoothing = -1.f;
    void show_projector(bool show);
    // Pushes the active layer's texture into the overlay. Cheap, and a no-op while it is hidden.
    void update_projector();
    // Reads the overlay's rectangle and commits it as the layer's projection: builds the exact
    // projective local->uv matrix from the camera and that rectangle, turns tiling off so the border
    // is a hard edge, and repaints the layer with the visible facets inside the frame. Returns the
    // number of facets selected, or -1 if the frame could not be used at all.
    int apply_projection_frame();

    // Uniformly subdivides the volume's mesh (see libslic3r::subdivide_mesh_uniform()) so a
    // low-poly input model has enough vertices to actually show texture-displacement detail.
    // A real, committed geometry change (like Bake), so it needs its own snapshot; unlike Bake it
    // has no target region, so any not-yet-baked paint on the volume is dropped rather than
    // remapped (texture-displacement paint has no remap-across-topology-change support yet).
    void subdivide_model();

    // The layer height map's width / height, for apply_uv_transform()'s non-square handling. 1 when
    // there is no usable texture.
    static float layer_texture_aspect(const TextureDisplacementLayer &layer);

    // Returns a cached GPU thumbnail of layer's texture (decoding + uploading it the first time it
    // is requested, or whenever its image_data changes), or nullptr if it has no usable texture.
    // Panel-sized: box-filtered down to THUMBNAIL_MAX_PX, which is right for a list row and wrong for
    // anything the shader samples - see get_layer_height_texture().
    GLTexture *get_layer_thumbnail(const TextureDisplacementLayer &layer);

    // The same texture at full resolution, for the fast-preview shader. One slot, shared by whichever
    // layer is active, because that is the only one the bump shader ever shades.
    GLTexture *get_layer_height_texture(const TextureDisplacementLayer &layer);
    // The layer's colour texture for the fast preview's per-fragment quantization. Null when the
    // layer is not colouring or its texture is grayscale.
    GLTexture *get_layer_color_texture(const TextureDisplacementLayer &layer);

    // A texture from the picker's library (see slic3r/GUI/TextureLibrary.hpp), read and uploaded
    // once and then kept for the gizmo's lifetime. The decoded bytes are held alongside the GPU
    // thumbnail so that picking the texture can hand the layer this very same image_data buffer -
    // which both avoids re-reading the file and lets decode_height_texture()'s own cache (keyed by
    // exactly this pointer) hit immediately on the first bake/preview.
    struct LibraryTexture
    {
        std::shared_ptr<std::vector<unsigned char>> image_data;
        std::unique_ptr<GLTexture>                  thumbnail;
    };
    const LibraryTexture *get_library_texture(const std::string &path);

    // The layer's texture chooser: a drop-down whose closed state and every one of whose entries
    // shows a large preview image on the left and the texture's name on the right, plus an adjacent
    // button that imports an image file from disk into the user texture folder. Shipped and
    // user-imported textures are listed under separate headings.
    void render_texture_picker(TextureDisplacementLayer &layer);
    void set_layer_texture(TextureDisplacementLayer &layer, const TextureLibraryEntry &entry);
    void import_custom_texture(TextureDisplacementLayer &layer);
    // Draws a picker row (image left, name right) on top of a full-width Selectable, and leaves the
    // cursor below it. Shared by the drop-down's closed state and its individual entries so the two
    // cannot drift apart. Returns true when the row is clicked.
    bool  texture_row(const char *id, const std::string &name, GLTexture *thumbnail, bool selected, float width);
    float texture_row_height() const;

    // "Adjust Texture" mode: instead of painting, dragging an on-canvas handle changes the active
    // layer's offset. The handle is a flat panel lying in the paint patch's own tangent plane
    // (a "pan" - drag anywhere on it for free 2D movement), plus two arrows along the patch's
    // own U/V axes that constrain the drag to just that one axis for precise nudging. Anchored to
    // the centroid/average-normal of the active layer's current paint patch (see
    // libslic3r::compute_layer_paint_anchor()), so nothing is drawn if it has nothing painted yet.
    //
    // NOTE: the drag direction/sign below is this session's best-effort reasoning about which way
    // the texture should appear to move as the handle is dragged - it could not be visually
    // confirmed while writing it (no way to render/see pixels in this environment), so it may
    // need a one-line sign flip once actually tested.
    bool update_adjust_anchor(); // recomputes m_adjust_anchor_pos/normal; false if nothing painted
    bool on_mouse_adjust_texture(const wxMouseEvent &mouse_event);
    void render_adjust_texture_gizmo();
    // Draws a small '+'/'-' next to the mouse over the 3D view while painting/selecting, so it is
    // obvious whether the next stroke adds paint (default) or erases it (Shift). Uses ImGui's
    // foreground draw list, so it must be called from inside the gizmo's ImGui frame.
    void render_paint_cursor_hint();
    // Mesh-local tangent-plane basis at m_adjust_anchor_normal, matching project_planar()'s
    // dominant-axis convention so dragging on-canvas maps consistently onto offset.
    void adjust_tangent_basis(Vec3f &u_axis, Vec3f &v_axis) const;

    // The plane a drag is measured against: the paint patch's anchor, lifted clear of the surface.
    // Deliberately *fixed* - independent of the layer's offset - so that moving the handle cannot
    // move the plane the handle's own motion is derived from, which would be a feedback loop.
    Vec3f adjust_plane_point() const;

    // Where the handle is actually drawn, in mesh-local coordinates. This is NOT just the patch's
    // centroid: the handle *represents the texture's placement*, so it has to travel as `offset`
    // changes. Pinning it to the centroid is why dragging it looked broken - the texture slid but
    // the handle stayed put. Undoing apply_uv_transform()'s scale and rotation turns the layer's
    // offset back into a displacement in mm within the patch's tangent plane, which is what gets
    // added to the anchor here. That is exactly consistent with the drag arithmetic in
    // on_mouse_adjust_texture(): the handle then tracks the cursor 1:1, and sits back on the anchor
    // precisely when offset is zero.
    Vec3f adjust_handle_center(const TextureDisplacementLayer &layer) const;

    // The layer painted by the active slot, or nullptr if that slot has no layer yet.
    TextureDisplacementLayer       *active_layer();
    const TextureDisplacementLayer *active_layer() const;

    // Recomputes m_preview_glmodel from the volume's current (unbaked) paint state, using the same
    // build_texture_displacement() algorithm as Bake. Called whenever the paint mask changes
    // (stroke end, layer switch, undo/redo reload, post-bake refresh) rather than every frame -
    // this is real mesh work (PNG sampling, vertex welding), not something to redo per paint stroke
    // drag sample or idle repaint. With several painted layers this can be slow, so the actual
    // computation runs in a background TextureDisplacementPreviewJob; this function only queues
    // it and returns immediately, and m_preview_glmodel is updated later when it completes.
    void rebuild_preview();
    void render_preview_mesh();

    // Alternate, GPU-only preview: perturbs shading normals from the active layer's height texture
    // (a classic bump map) instead of actually moving vertices, using the
    // resources/shaders/*/texture_displacement_bump.* shader. Faster than the true-displacement
    // preview (no CPU meshing at all - just a per-vertex paint-weight buffer built at the same
    // cadence as rebuild_preview()) but only shows the *active* layer, and any bump is a shading
    // illusion, not real geometry - "Bake" always produces the true, exact result either way.
    void rebuild_bump_preview_mesh();
    void render_bump_preview_mesh();

    // Feeds the active layer's painted patch + LSCM unwrap (if it's using that projection method)
    // into Plater's docked UV-editor pane and shows it, or hides the pane if the active layer
    // isn't using LSCM (or nothing is painted). Called whenever something that could change what
    // the pane should show happens: paint changes, layer switch, projection method change, bake,
    // and on shutdown (to hide it).
    void update_uv_editor();

    // Applies one island edit reported by the UV editor's drag/rotate gestures to the active layer.
    // Deltas are incremental (see UVEditorCanvas::IslandEditFn); `finished` ends the gesture, which
    // is when - and only when - the 3D preview is rebuilt, since doing that per mouse-move would
    // queue a mesh recompute for every pixel of a drag.
    void on_island_edited(int island, const Vec2f &offset_delta, float rotation_delta, float scale_factor, bool finished);
    // Applies a committed vertex/edge edit from the UV editor's Vertex/Edge modes: each entry is an
    // unwrapped-vertex index and its new raw-unwrap coordinate. Maps the unwrapped index to a mesh
    // vertex and stores a per-vertex UV override on the layer (see lscm_uv_overrides), then rebuilds the
    // preview so the baked geometry follows.
    void on_uv_vertex_edited(const std::vector<std::pair<int, Vec2f>> &edits);
    // UV-editor sub-element select mode, mirrored into the canvas: 0 = Island, 1 = Vertex, 2 = Edge.
    int  m_uv_select_mode = 0;
    // One affine per island (columns: x basis, y basis, translation), mapping the unwrap's raw mm
    // coordinates to texture UVs - the same type as UVEditorCanvas::IslandTransform, spelled out
    // here so this header needn't drag in wxGLCanvas/glad. Cheap to recompute (it is per *island*,
    // not per vertex), which is what lets an island drag update the pane without re-uploading a
    // single vertex.
    std::vector<Eigen::Matrix<float, 2, 3>> uv_editor_island_transforms(const TextureDisplacementLayer &layer);
    // Handles a toolbar command forwarded from the UV pane that needs the layer data the canvas
    // doesn't hold (average island scale, cut island). Takes the command as an int (a cast of
    // UVEditorCanvas::Command) so this header needn't pull in glad/wxGLCanvas via the canvas header.
    void on_uv_command(int cmd);
    // Splits one unwrap chart in two by marking the mesh edges that straddle the plane through its
    // 3D centroid, perpendicular to its longest axis, as seams (#17). The re-unwrap then separates it.
    void cut_island(TextureDisplacementLayer &layer, int chart);

    // Captures the current camera's right/up axes into the layer's projector (#6), transformed into
    // the volume's local space so the projection is stable as the object is later moved/rotated.
    void capture_view_projection(TextureDisplacementLayer &layer);

    // Manual seam marking (#9): a mode where clicking the model toggles the nearest mesh edge in the
    // active layer's lscm_seam_edges, so the unwrap can be cut exactly where the user wants - the
    // Blender "mark seam" workflow. Painting is suppressed while it is on.
    bool    m_seam_edit_mode = false;
    GLModel m_seam_glmodel; // the current seam edges, highlighted on the mesh
    bool    on_mouse_seam(const wxMouseEvent &mouse_event);
    void    toggle_seam_at(const Vec2d &mouse_pos);
    void    rebuild_seam_overlay();
    void    render_seam_overlay();
    // The mesh edge nearest the mouse, in the volume's own vertex indices, or {-1,-1} if the ray misses.
    // Factored out of toggle_seam_at() so the same pick can drive a live hover highlight (below) that
    // shows which edge a click would toggle - the "I don't know how it works" feedback the user hit.
    std::pair<int, int> seam_edge_at(const Vec2d &mouse_pos) const;
    std::pair<int, int> m_seam_hover_edge{ -1, -1 };
    // The vertex a click would pick in shortest-path mode, so the target is visible on hover the same
    // way the edge is in normal mode. -1 when nothing is under the cursor (or not in path mode).
    int                 m_seam_hover_vertex = -1;
    GLModel             m_seam_hover_glmodel;
    void                rebuild_seam_hover_overlay();

    // Shortest-path seam marking, for dense meshes where clicking every single triangle edge is
    // tedious: in this sub-mode a click picks the nearest vertex, and the next click marks every edge
    // on the shortest surface path between the two as a seam - so a whole seam line is drawn with two
    // clicks. The end vertex becomes the next start, so a multi-segment seam chains click by click.
    bool    m_seam_path_mode   = false;
    int     m_seam_path_anchor = -1;      // mesh vertex the path starts from, or -1
    GLModel m_seam_anchor_glmodel;        // the anchor's incident edges, highlighted
    int     seam_vertex_at(const Vec2d &mouse_pos) const;    // nearest mesh vertex under the cursor
    void    mark_seam_path(int v_from, int v_to);            // seam every edge on the shortest path
    void    rebuild_seam_anchor_overlay();
    // Set while an island gesture is in flight, so the undo snapshot is taken once at the start of
    // the drag (capturing the state *before* it) rather than on every motion event.
    bool m_island_drag_active = false;

    // Which of the up to TEXTURE_DISPLACEMENT_MAX_LAYERS paint masks the brush currently writes
    // into. Always a valid slot index (0 by default) so the base class's per-volume selector
    // machinery always has something to work with, even before any texture has been added -
    // painting into a slot with no texture assigned is harmless, it just has no visible/bake
    // effect until a texture is added to that slot.
    int  m_active_layer_slot = 0;
    bool m_bake_in_progress  = false;

    // When set, the true-displacement geometry is rebuilt on every parameter change (live), instead of
    // only once the slider being dragged is released. On by default so painting/added textures show
    // straight away without needing to nudge a slider first.
    bool m_auto_update = true;

    // Subdivision is now count-based (split the whole mesh 1..5 times) rather than a target edge
    // length, and is previewed as a wireframe before it is committed: nothing is written to the model
    // until "Apply". While previewing, the would-be subdivided mesh is drawn as a wireframe overlay so
    // the added density is visible; "Done" ends the preview without touching the model. The normal
    // "Show mesh wireframe" toggle is left alone, so a wireframe the user already had on stays on.
    // 0 is a real value meaning "no subdivision": it previews nothing and Apply is a no-op. Apply
    // snaps the slider back to it, because each pass quadruples the triangle count - leaving the
    // count where it was would immediately re-preview N more passes on top of the mesh that was just
    // committed, i.e. the most expensive thing the panel can do, on every Apply.
    int     m_subdivide_count         = 1;
    bool    m_subdivide_editing       = false;
    int     m_subdivide_preview_tris  = -1; // triangle count of the previewed result, shown in the panel
    GLModel m_subdivide_preview_glmodel;
    void    rebuild_subdivide_preview();
    void    render_subdivide_preview();

    // Adaptive subdivision: refine only the painted area, down to a target edge length, via
    // conformal longest-edge bisection (subdivide_mesh_adaptive()). Unlike the count-based uniform
    // path it does not touch the unpainted rest of the model, and - because it is driven by the paint
    // - it can carry that paint forward across the topology change (children of a painted triangle
    // are painted), so the region survives the subdivision instead of being dropped.
    bool  m_subdivide_adaptive   = false;
    float m_subdivide_target_mm  = 0.f; // 0 = not yet seeded; filled from the mesh on first show
    // Feature-adaptive sub-mode: put the triangles where the *displaced surface* bends (texture
    // curvature) rather than spreading them evenly. `detail_mm` is the chord-error tolerance ("Detail"
    // slider: how far the true surface may sit off the flat triangle before it is split); the target
    // above stays in play as a coarse baseline ("Max edge"), and `min_edge_mm` is the hard floor
    // ("Min edge"). See subdivide_mesh_adaptive().
    bool  m_subdivide_feature     = false;
    float m_subdivide_detail_mm   = 0.05f;
    float m_subdivide_min_edge_mm = 0.1f;
    // Edge length the band straddling the paint's boundary is refined to (0 = leave it alone). Applies
    // in both adaptive sub-modes, because it is not a texture-detail criterion: the bake steps the
    // surface from full displacement to zero across that boundary whatever the texture is doing, and
    // the chord-error test cannot see that step at all - its sampler has no per-point paint test, so
    // just outside the paint it goes on reporting the same smooth height field. Without this the
    // transition keeps the input's density and the rim of an unpainted island comes out as a ring of
    // large, steeply tilted triangles. See collect_paint_region() and subdivide_mesh_adaptive().
    float m_subdivide_border_mm   = 0.4f;
    // Edge length triangles straddling a *colour* boundary are refined to (0 = ignore colour). Its own
    // control rather than a share of "Detail (mm)" because the two measure different things: Detail is
    // a chord error in mm of surface deviation, this is a triangle size in mm along a step the chord
    // test cannot see at all - the height field is perfectly smooth across a change of filament, so
    // without this a colour boundary lands on whatever triangles the relief happened to need, which on
    // a flat surface is none. Only ever costs anything where a boundary actually runs.
    float m_subdivide_color_mm    = 0.3f;
    // How many thousand triangles refinement may *add* (the mesh's own count is added on before it is
    // passed as subdivide_mesh_adaptive()'s absolute cap, so the control still means something on a
    // dense model). Refinement is worst-error-first, so hitting the budget still yields the best mesh
    // that many triangles can buy - and it is what keeps a fine "Detail" over a noisy texture from
    // turning into an out-of-memory, or an unrenderable preview wireframe.
    //
    // The default used to be 1500 (i.e. +1.5 M triangles), which is what made Standard mode's Bake
    // take minutes: every stage after the subdivision - the displacement itself, the convex hull, the
    // GLModel upload, and the re-slice changed_object() triggers - then runs on a mesh two orders of
    // magnitude denser than the input. 300k is still far finer than any FDM nozzle resolves at the
    // 0.02 mm detail tolerance Standard uses, and the slider goes to 2000 for anyone who wants more.
    int   m_subdivide_budget_k    = 300;
    void  subdivide_model_adaptive();
    // Fills `region` (per current-mesh triangle, a REFINE_* bitmask) from the union of every layer's
    // painted area plus the band straddling its edge. If `paint` is non-null, also fills the per-layer
    // coverage map the subdivision carries forward - the expensive half, skipped by the live preview,
    // which only needs the region. Returns false when nothing is painted at all.
    static bool collect_paint_region(const TriangleMesh &mesh, const TextureDisplacementFacetsData &facets,
                                     std::vector<uint8_t> &region,
                                     std::array<LayerPaintMap, TEXTURE_DISPLACEMENT_MAX_LAYERS> *paint);

    // Runs the volume's TextureDisplacementOptions smoothing over the *already committed* geometry,
    // restricted to the painted area. The same settings are folded into Preview/Bake automatically;
    // this is the escape hatch for relief that has already been baked in, where there is no
    // displacement pass left to attach them to. Topology-preserving, so unlike subdivide and remesh it
    // keeps every paint channel - including texture displacement - exactly as it was.
    void  smooth_model();

    // Isotropic remeshing (CGAL) to even out wildly varying triangle sizes so displacement has a
    // consistent density to work with. Target edge length in mm; 0 means "not yet initialised", filled
    // with the mesh's mean edge length the first time the control is shown. Like subdivide, it replaces
    // the geometry, but unlike subdivide it keeps every paint channel: prepare_mesh() carries the
    // texture-displacement masks across spatially, which is also what lets Standard mode remesh *after*
    // the user has painted.
    float m_remesh_target_edge_mm = 0.f;
    // Dihedral angle above which an edge counts as a hard feature and is held fixed by the remesher.
    // Off by default would round every sharp edge off, so this is on; 0 disables the protection.
    float m_remesh_sharp_angle_deg   = 40.f;
    bool  m_remesh_keep_sharp_edges  = true;
    void  remesh_model();

    // Live, pre-bake preview of the true displaced geometry (built by the same algorithm Bake
    // uses). Empty/uninitialized whenever nothing is painted yet, in which case the gizmo falls
    // back to the standard paint-mask overlay like every other painting gizmo.
    GLModel m_preview_glmodel;
    // Set while a layer parameter slider has changed since the last rebuild_preview() call but the
    // mouse button driving the drag hasn't been released yet - see on_render_input_window().
    bool m_preview_params_dirty = false;

    // See rebuild_bump_preview_mesh()/render_bump_preview_mesh(). On by default: it is the cheap,
    // instant-updating preview, so it is the better first impression while painting. The true-
    // displacement view (a background CPU remesh) is one click away in the View row when the user
    // wants an exact look at what Bake will produce.
    bool    m_use_bump_preview = true;
    // Set from the UV editor's per-move island edits instead of rebuilding the (potentially large) bump
    // mesh synchronously inside that mouse handler - doing the rebuild there stalled both the UV pane
    // and the 3D view. The rebuild is instead coalesced to once per 3D frame (render_painter_gizmo).
    bool    m_bump_preview_dirty = false;
    GLModel m_bump_preview_glmodel;

    // Translucent tint over the active layer's painted triangles, drawn on top of whichever preview
    // is showing. The base painter's own opaque paint highlight (render_triangles()) cannot be used
    // in either preview mode - it is coincident with the surface and simply covers it - so the only
    // paint feedback the gizmo had was the relief itself, which meant erasing showed nothing at all
    // until the stroke ended and the whole preview rebuilt. This is that feedback: cheap (the painted
    // patch only), translucent (the preview stays visible through it) and rebuilt live during a
    // stroke.
    GLModel m_paint_overlay_glmodel;
    // Set on every paint event, cleared when the overlay is rebuilt in render_painter_gizmo(). Kept
    // separate from m_bump_preview_dirty so a stroke refreshes only the small painted patch per frame,
    // not the bump mesh (which also carries every *unpainted* triangle of the volume).
    bool    m_paint_overlay_dirty = false;
    void    rebuild_paint_overlay();
    void    render_paint_overlay();
    // Whether render_bump_preview_mesh() would actually draw something. Checked before the real volume
    // is hidden: with no layer, no texture or no shader the bump path draws nothing, and hiding the
    // volume for it left the model invisible.
    bool    bump_preview_ready() const;
    // Whether the current bump mesh carries a precomputed per-vertex uv (LSCM) that the shader
    // should sample at directly, rather than projecting in-shader. Set by rebuild_bump_preview_mesh().
    bool    m_bump_preview_uses_vertex_uv = false;
    // The palette the fast preview's per-triangle filament indices were built against, captured when
    // the mesh was. Empty when the active layer is not colouring, which is what tells the shader to
    // fall back to the model's own colour. Held rather than re-read at draw time so the indices baked
    // into the mesh can never be resolved against a different set of filaments than they were computed
    // from - loading a filament mid-session would otherwise recolour a stale preview at random.
    std::vector<PaletteEntry> m_bump_preview_palette;

    // GPU island drag: while an island is dragged in the UV editor, the bump mesh is baked once (with
    // the dragged island's vertices flagged, v_normal.y = 1) and then moved purely through the shader's
    // island_delta uniform - one uniform update per mouse move, no rebuild - so it tracks the cursor
    // as smoothly as Adjust placement. m_bump_active_chart is the dragged island (or -1);
    // m_bump_active_vertex flags its base vertices; m_bump_baked_active_xf is that island's placement
    // baked into the current mesh, against which the live delta is measured; m_bump_island_delta is the
    // resulting final-uv-space affine handed to the shader (identity except mid-drag).
    int                        m_bump_active_chart = -1;
    std::vector<uint8_t>       m_bump_active_vertex;
    Eigen::Matrix<float, 2, 3> m_bump_baked_active_xf = Eigen::Matrix<float, 2, 3>::Identity();
    Eigen::Matrix<float, 2, 3> m_bump_island_delta    = Eigen::Matrix<float, 2, 3>::Identity();
    void                       compute_bump_active_vertices(const std::vector<int> &charts);

    // The set of islands the current UV-editor drag moves together: the pane's multi-selection unioned
    // with each selected island's join group (see build_island_move_set()). Populated at drag start and
    // cleared when it finishes. A move applies the same offset to every island in it; rotate/scale act
    // only on the primary. Empty when no move drag is in flight.
    std::vector<int> m_island_move_set;
    // All islands that must move with `primary`: the pane's multi-selection plus, for each of those, the
    // charts sharing its join group in `layer`. Always contains `primary`.
    std::vector<int> build_island_move_set(const TextureDisplacementLayer &layer, int primary) const;
    // The join-group id of chart `c`: its explicit entry in `groups`, or `c` itself (its own singleton)
    // when unset. Two charts move together iff this matches.
    static int       island_group_of(const std::vector<int> &groups, int c);
    // Merges chart `b`'s join group into chart `a`'s (materialising `groups` to `chart_count` first).
    static void      join_island_groups(std::vector<int> &groups, int a, int b, int chart_count);
    // Final per-vertex texture uv for the projections the shader can't reconstruct itself - LSCM (an
    // unwrap) and ViewProjected (a projector plane the shader doesn't know). One entry per patch/base
    // vertex, already through apply_uv_transform(). Empty for Triplanar/Cylindrical/Spherical, which
    // the shader projects on its own. Shared by the bump preview and the UV-check overlay.
    std::vector<Vec2f> compute_layer_vertex_uvs(const indexed_triangle_set &patch,
                                                const TextureDisplacementLayer &layer) const;

    // UV-check overlay drawn over the painted patch to sanity-check the unwrap (#13/#14). Built by
    // rebuild_uvcheck_mesh(), drawn by render_uvcheck_mesh() with the "texture_displacement_uvcheck"
    // shader. Checker works for any projection; Distortion needs the per-vertex LSCM uv.
    enum class UVCheckMode { None, Checker, Distortion };
    UVCheckMode m_uv_check_mode = UVCheckMode::None;
    GLModel     m_uvcheck_glmodel;
    bool        m_uvcheck_uses_vertex_uv = false;
    void rebuild_uvcheck_mesh();
    void render_uvcheck_mesh();

    // The UV editor pane is opened only on the user's explicit request (this toggle in the panel),
    // never automatically just because a patch exists - auto-popping it whenever there was "a
    // selection to process" is exactly what the user asked to stop. update_uv_editor() keeps the pane
    // hidden unless this is set. Reset on gizmo shutdown so reopening the gizmo doesn't reopen the pane.
    bool m_show_uv_editor = false;
    // The unwrap is expensive, so it is recomputed only when the user explicitly asks for it (the
    // "Unwrap" button), not on every paint stroke or slider nudge. This is set by that button and
    // consumed by the next update_uv_editor() call, which is the only path that re-solves the unwrap;
    // every other call merely refreshes the cheap per-island affine transforms over the existing one.
    bool m_uv_unwrap_pending = false;
    // Set alongside m_uv_unwrap_pending only by the Unwrap button, so the connected-net auto-layout runs
    // on a genuine re-unwrap but not on a refresh re-solve (a vertex-edit commit or undo), which must
    // leave island placements untouched.
    bool m_uv_apply_connected_net = false;
    // Signature of the per-vertex UV overrides last reflected in the pane. When it changes without the
    // user pressing Unwrap - a vertex/edge edit committing, or an undo/redo reverting one - the pane
    // is re-solved so its geometry follows, even though a plain edit otherwise never re-solves (#Feat2).
    size_t m_uv_overrides_sig = 0;
    // What the UV pane's background currently holds, so update_uv_editor() only re-uploads it when the
    // choice actually changes (the height texture is large; re-sending it every stroke would be waste).
    enum class UVBackground { None, Height, Checker };
    UVBackground m_uv_editor_bg = UVBackground::None;
    float        m_uv_editor_bg_smoothing = -1.f; // smoothing the height backdrop was uploaded at
    // Per-chart distortion heatmap colour for the UV pane (#7/#14), computed once when the unwrap is
    // re-solved (relative stretch doesn't change when islands are merely moved), fed to the canvas only
    // while the Distortion check mode is on. Empty otherwise.
    std::vector<ColorRGBA> m_uv_editor_distortion_colors;
    void                   compute_uv_editor_distortion_colors(const indexed_triangle_set &patch);

    // Plain triangle-edge overlay on the mesh (#8), toggled independently of the check modes.
    bool    m_wireframe_overlay = false;
    GLModel m_wireframe_overlay_glmodel;
    size_t  m_wireframe_overlay_vcount = 0; // topology signature, so it rebuilds only on a real change
    void rebuild_wireframe_overlay();       // from the base mesh (bump/paint mode)
    void build_wireframe_from_its(const indexed_triangle_set &its); // from an explicit mesh, no early-out
    void refresh_wireframe();               // pick base vs displaced source for the current view
    void render_wireframe_overlay();
    // The displaced preview geometry the last preview job produced, kept so the wireframe overlay can be
    // drawn on the raised surface actually shown in the true-displacement view (#: "wireframe in real mode").
    indexed_triangle_set m_preview_its;
    // Bumped on every rebuild_preview() call; a background TextureDisplacementPreviewJob's result
    // is only applied if this hasn't moved on since the job was queued (see rebuild_preview()),
    // so a burst of edits can't have an earlier, now-stale job clobber a later one's result.
    //
    // Shared with the worker thread (hence the atomic) so a running job can notice mid-computation
    // that it has been superseded and abort, instead of running to completion for a result that will
    // only be discarded on arrival.
    std::shared_ptr<std::atomic<uint64_t>> m_preview_generation = std::make_shared<std::atomic<uint64_t>>(0);
    // At most one preview job is ever queued. The UI job worker is a single FIFO queue shared with
    // Bake (and with arrange/orient/send), and rebuild_preview() is called on every stroke end, every
    // slider release and - with "Auto update" on - every frame of a slider drag. Queuing one full
    // displacement per call built a backlog that took minutes to drain: the preview appeared frozen,
    // and a Bake pressed afterwards sat behind the whole queue. So a request made while a job is in
    // flight is recorded here and issued once that job settles, collapsing any number of edits into a
    // single follow-up run.
    bool m_preview_job_running = false;
    bool m_preview_job_pending = false;
    void queue_preview_job();

    // Per-slot GPU thumbnail cache for the layer list panel, keyed by the image_data pointer that
    // was current the last time each thumbnail was built (see get_layer_thumbnail()).
    std::array<std::unique_ptr<GLTexture>, TEXTURE_DISPLACEMENT_MAX_LAYERS> m_thumbnails;
    std::array<const void *, TEXTURE_DISPLACEMENT_MAX_LAYERS>               m_thumbnail_source{};
    // The smoothing each cached thumbnail was built at, so a smoothing change re-uploads it.
    std::array<float, TEXTURE_DISPLACEMENT_MAX_LAYERS>                      m_thumbnail_smoothing{};

    // Full-resolution height texture for the bump shader, keyed the same way (see
    // get_layer_height_texture()). A smoothing change re-uploads it, so the fast preview shows the
    // blur the bake will apply.
    std::unique_ptr<GLTexture> m_height_tex;
    const void                *m_height_tex_source    = nullptr;
    float                      m_height_tex_smoothing = -1.f;

    // The same, for the layer's *colour*, which the fast preview quantizes per fragment so it shows
    // the image at texel resolution rather than at the mesh's. Null for a grayscale texture.
    std::unique_ptr<GLTexture> m_color_tex;
    const void                *m_color_tex_source    = nullptr;
    float                      m_color_tex_smoothing = -1.f;

    // Library textures the picker has shown at least once, keyed by file path (see LibraryTexture).
    std::map<std::string, LibraryTexture> m_library_textures;

    // Everything the *unwrap* depends on. update_uv_editor() runs from rebuild_preview(), i.e. on
    // every stroke end and every slider release - but depth/tiling/rotation/offset/blend change
    // none of this, so re-extracting the patch and re-solving on those edits would be pure waste.
    // Held as the real values rather than a hash: TriangleSplittingData has an exact operator==, so
    // there is no reason to accept a hash's (however unlikely) chance of showing a stale unwrap.
    struct UVEditorState
    {
        int                                     slot       = -1;
        const void                             *image_data = nullptr;
        float                                   seam_angle = -1.f;
        float                                   padding    = -2.f;
        TriangleSelector::TriangleSplittingData facets;
        // Manual/auto seam edges also change the unwrap, so a change here must force a re-solve just
        // like the facets do (marking a seam leaves the paint mask untouched).
        std::vector<std::pair<int, int>>        seam_edges;

        bool operator==(const UVEditorState &other) const
        {
            return slot == other.slot && image_data == other.image_data && seam_angle == other.seam_angle &&
                   padding == other.padding && facets == other.facets && seam_edges == other.seam_edges;
        }
    };
    UVEditorState m_uv_editor_state;
    // Bounds of the UVs last handed to the pane, purely so the panel can show where the unwrap
    // actually landed - it is packed in mm and then divided by the tile size, so it is easy for it
    // to end up far outside the texture's first tile without any of that being visible.
    Vec2f m_uv_editor_bbox_min = Vec2f::Zero();
    Vec2f m_uv_editor_bbox_max = Vec2f::Zero();
    // The unwrap m_uv_editor_state produced, kept so that changing tiling/rotation/offset only costs
    // re-running apply_uv_transform() over it, not another extraction and solve.
    PatchUnwrap m_uv_editor_unwrap;

    // When set, the panel is a free-floating window the user can drag anywhere (with a title bar to
    // grab), instead of being pinned to the right of the gizmo toolbar. Persisted across gizmo
    // open/close within a session, so the choice sticks while working.
    bool m_undocked = false;

    // See the "Adjust Texture" block of private methods above.
    bool  m_adjust_texture_mode      = false;
    bool  m_adjust_anchor_valid      = false;
    Vec3f m_adjust_anchor_pos        = Vec3f::Zero();  // mesh-local
    Vec3f m_adjust_anchor_normal     = Vec3f::UnitZ(); // mesh-local

    // Pan: free drag anywhere on the flat panel, moves offset along both axes. AxisU/AxisV: drag
    // the corresponding arrow, moves offset along only that one axis.
    enum class AdjustHandle { None, Pan, AxisU, AxisV };
    AdjustHandle m_adjust_drag_handle       = AdjustHandle::None;
    Vec2f        m_adjust_drag_start_offset = Vec2f::Zero();
    // Anchor-relative planar position (see project_planar()) of the point under the mouse at the
    // moment the current drag started; every subsequent frame's delta is measured against this,
    // rather than accumulated frame-to-frame, to avoid drift.
    Vec2f m_adjust_drag_start_planar = Vec2f::Zero();

    // Lazily-built unit quad (the pan panel) and unit line-with-arrowhead (reused, rotated, for
    // both the U and V axis arrows), transformed into place at render time.
    GLModel m_adjust_panel_glmodel;
    GLModel m_adjust_arrow_glmodel;

    std::map<std::string, wxString> m_desc;

    // The tool's SVG (toolbar_texture_displacement.svg) uploaded once as a GL texture, so it can be
    // used as an ImGui image button in the panel (currently the "add layer" affordance next to the
    // Texture layers heading). Lazily loaded on first use, when a GL context is guaranteed current.
    GLTexture    m_tool_icon;
    bool         m_tool_icon_tried = false;
    unsigned int tool_icon_id(); // 0 if the icon could not be loaded

    // Icons for the panel's selection-mode and view-mode button rows. Loaded through IconManager with
    // the same colour/monochrome variants the main toolbar uses, so an inactive button shows the icon in
    // the theme's normal (grey) foreground colour and an active one shows it in its original colours -
    // matching the toolbar's selected/unselected look. Uploaded once on first panel render.
    IconManager                                m_panel_icons;
    std::map<std::string, IconManager::Icons>  m_panel_icon_map; // file name -> [normal, colour, disabled]
    bool                                       m_panel_icons_tried = false;
    void                                       ensure_panel_icons();
};

} // namespace Slic3r::GUI

#endif // slic3r_GLGizmoTextureDisplacement_hpp_
