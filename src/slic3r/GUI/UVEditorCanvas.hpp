#ifndef slic3r_UVEditorCanvas_hpp_
#define slic3r_UVEditorCanvas_hpp_

#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

// Must come before wx/glcanvas.h in any translation unit that includes this header: glcanvas.h
// pulls in the platform's real GL/gl.h, and glad/gl.h errors out if that happens first (it wants
// to be the one to define the standard include guards GL/gl.h itself defines).
#include <glad/gl.h>
#include <wx/glcanvas.h>
#include <wx/panel.h>
#include <wx/button.h>
#include <wx/tglbtn.h>
#include <wx/stattext.h>
#include <wx/sizer.h>

#include "libslic3r/Point.hpp"
#include "GLModel.hpp"
#include "GLTexture.hpp"

namespace Slic3r::GUI {

// Standalone 2D viewer/editor for a flattened (UV-unwrapped) mesh patch - shows the result of
// GLGizmoTextureDisplacement's LSCM projection method as its own resizable pane (see Plater's
// "uv_editor" AUI pane) rather than folding 2D UV-space rendering into the main 3D viewport.
//
// Islands can be laid out by hand, roughly the way Blender's UV editor works: click one to select
// it, drag to move it, right-drag or press R to rotate it, S to scale it, both about its own centre.
// Islands are free to overlap - nothing re-packs them behind the user's back. The texture underneath
// is always drawn upright and axis-aligned, and it is the islands that move over it, which is what
// makes "rotate this island" a meaningful gesture rather than just spinning the whole texture.
//
// **Geometry is uploaded in the unwrap's own (raw, mm) coordinates, once**, and each island is drawn
// through its own affine matrix passed as a shader uniform. That matters: a patch can easily run to
// a million triangles, and the earlier design - which pre-transformed every UV on the CPU and
// re-uploaded the whole wireframe on every mouse-move event - made a drag cost a couple of hundred
// milliseconds per frame. Moving an island now touches a 2x3 matrix and nothing else.
//
// Uses the app's single shared wxGLContext (via wxGetApp().init_glcontext(), the same call
// View3D/Preview/AssembleView each make in GUI_Preview.cpp) rather than an independent context of
// its own, specifically so it can reuse the app's already-registered "flat"/"flat_texture"
// shaders and GLModel as-is - GLModel::render() looks up its shader via a GUI_App-wide "current
// shader", which only means anything for canvases sharing the app's one real GL context.
class UVEditorCanvas : public wxGLCanvas
{
public:
    explicit UVEditorCanvas(wxWindow *parent);

    // Maps one island's raw unwrap coordinate to a texture UV: the island's own hand placement and
    // then the layer's tiling/rotation/offset, composed into a single affine (columns: x basis,
    // y basis, translation).
    using IslandTransform = Eigen::Matrix<float, 2, 3>;

    // The unwrap to display, in the unwrap's own mm coordinates - *not* texture UVs. Changing this
    // is the expensive path (it rebuilds every vertex buffer), so it must only be called when the
    // unwrap itself changes, never merely because an island moved. Pass an empty `indices` to show
    // nothing.
    struct Islands
    {
        std::vector<Vec2f>               uvs;
        std::vector<Vec3i32>             indices;
        std::vector<int>                 vertex_island;  // per uv
        std::vector<std::pair<int, int>> boundary_edges; // island outlines, indices into uvs
        int                              island_count = 0;
    };
    void set_islands(Islands islands);

    // The cheap path: one transform per island. Safe to call on every mouse-move of a drag.
    void set_island_transforms(std::vector<IslandTransform> transforms);

    // One fill colour per island, overriding the default light-green wash - used to paint the UV
    // distortion heatmap over the islands when the gizmo's "Distortion" check mode is on (#7/#14).
    // Pass empty to go back to the default wash. Cheap: it never touches a vertex buffer.
    void set_island_fill_colors(std::vector<ColorRGBA> colors);

    // The layer's own tiling scale and rotation. Needed to map a gesture, which happens in texture-UV
    // space, back into the unwrap's mm space - which is where a TextureIsland's offset actually
    // lives (see apply_uv_transform()). The tile settings come along because the background has to
    // repeat exactly the way the height sampler does, or the pane would stop showing what gets baked.
    void set_uv_transform(float tiling_scale, float rotation_deg, bool tile_enabled, bool tile_mirrored);

    // Same 8-bit grayscale pixels build_texture_displacement()'s height sampling uses, shown
    // beneath the wireframe (expanded to RGBA on upload) so the unwrap can be checked against the
    // texture it will actually sample. Pass width/height <= 0 to clear it.
    void set_background_texture(const std::vector<unsigned char> &grayscale_pixels, int width, int height);

    // Snap a dragged island's boundary to a neighbouring island's when they come close (#2). Off is
    // the honest default for overlap-friendly layouts; the pane toolbar toggles it.
    void set_snap_enabled(bool enabled) { m_snap_enabled = enabled; }
    bool snap_enabled() const { return m_snap_enabled; }

    // High-level actions the pane toolbar triggers. The canvas handles the view-only ones (framing,
    // the snap toggle) itself and forwards the rest to whoever owns the island data (the gizmo), via
    // the command callback - the canvas has the selection and the view, the gizmo has the layer.
    enum class Command { FrameAll, ToggleSnap, AverageScale, CutSelectedIsland, ProjectFromView, JoinSelected, UnjoinSelected };
    void run_command(Command cmd);
    using CommandFn = std::function<void(Command)>;
    void set_command_callback(CommandFn fn) { m_on_command = std::move(fn); }

    // Called whenever the one-line status/hint text changes (current gesture + the shortcuts that
    // apply right now), so the pane can show it Blender-style along the bottom.
    using StatusFn = std::function<void(const wxString &)>;
    void set_status_callback(StatusFn fn) { m_on_status = std::move(fn); }

    // Reports an island edit as it happens. The deltas are *incremental* (one mouse event's worth)
    // and already converted into the units a TextureIsland stores - unwrap mm, degrees, and a scale
    // *factor* to multiply the island's existing scale by. They are incremental on purpose: the owner
    // applies them and hands back fresh transforms, and if the gesture tracked geometry rather than
    // raw mouse motion that round trip would feed back into itself. `finished` marks the end of a
    // gesture, so the owner can rebuild the 3D preview once rather than on every motion event.
    using IslandEditFn =
        std::function<void(int island, const Vec2f &offset_delta, float rotation_delta, float scale_factor, bool finished)>;
    void set_island_edit_callback(IslandEditFn fn) { m_on_island_edit = std::move(fn); }

    // What a click grabs: a whole island (move/rotate/scale, groups move together), a single vertex, or
    // a single edge (both its endpoints). Vertex/Edge are free-form UV editing - they move the actual
    // unwrap coordinates, which the owner then folds into the layer's per-vertex UV overrides so the
    // change is baked, not just shown (see set_vertex_edit_callback).
    enum class SelectMode { Island, Vertex, Edge };
    void       set_select_mode(SelectMode mode);
    SelectMode select_mode() const { return m_select_mode; }

    // Reports a committed vertex/edge edit: the list of (unwrapped-vertex index, its new raw-unwrap
    // coordinate in mm). Fired once, on mouse release, since it re-solves the displacement preview; the
    // pane shows the edit live from its own geometry in the meantime. The owner maps the unwrapped index
    // to a mesh vertex (via the unwrap's source_vertex) and stores the override.
    using UVVertexEditFn = std::function<void(const std::vector<std::pair<int, Vec2f>> &edits)>;
    void set_vertex_edit_callback(UVVertexEditFn fn) { m_on_vertex_edit = std::move(fn); }

    // The primary (last-clicked) island, still the pivot for rotate/scale and the target of the
    // single-island toolbar commands (Cut/Join/Unjoin). -1 if nothing is selected.
    int  selected_island() const { return m_selected_island; }
    // The full multi-selection (Shift adds, Ctrl toggles). Always contains m_selected_island when it is
    // >= 0. The gizmo reads this to decide which islands a drag moves together, unioned with each
    // selected island's join group.
    const std::vector<int> &selected_islands() const { return m_selection; }
    void reset_view();

private:
    void on_paint(wxPaintEvent &evt);
    void on_size(wxSizeEvent &evt);
    void on_mouse(wxMouseEvent &evt);
    void on_key(wxKeyEvent &evt);
    void on_leave(wxMouseEvent &evt); // drops the +/- cursor hint when the pointer leaves the canvas
    void on_erase_background(wxEraseEvent &evt) {} // required to avoid flicker on MSW, deliberately a no-op

    void render();
    void rebuild_island_models();
    void rebuild_background_texture();
    void rebuild_background_quad();
    void rebuild_grid();
    // The UV region worth looking at: every island, plus always at least the texture's first tile, so
    // there is something sensibly framed even before anything is painted.
    void content_bounds(Vec2f &min_uv, Vec2f &max_uv) const;
    // Frames content_bounds(). Bound to Home, and run once each time an unwrap first appears.
    void fit_view_to_content();

    // Half-extents of the visible UV region. Split out because both rendering and every mouse
    // gesture need them, and they have to agree exactly or picking lands in the wrong place.
    void  view_half_extents(float &half_w, float &half_h) const;
    Vec2f screen_to_uv(const wxPoint &px) const;
    // Raw unwrap coordinate -> texture UV, through the island's own transform.
    Vec2f island_uv(size_t vertex) const;
    // The island under `uv`, or -1. Prefers the current selection when islands overlap, so that
    // dragging one that sits under another doesn't hand the drag to its neighbour halfway through.
    int   island_at(const Vec2f &uv) const;
    // Nearest unwrapped vertex to `uv` within a screen-space threshold, or -1 (Vertex mode picking).
    int   vertex_at(const Vec2f &uv) const;
    // Nearest island-boundary edge to `uv` within a screen-space threshold, as its two unwrapped-vertex
    // indices, or {-1,-1} (Edge mode picking).
    std::pair<int, int> edge_at(const Vec2f &uv) const;
    // Moves one unwrapped vertex by a texture-UV delta, converting it back into the vertex's own raw
    // unwrap space through its island's inverse transform, and marks the mesh dirty so it redraws.
    void  move_vertex_raw(int unwrapped_vertex, const Vec2f &delta_uv);
    Vec2f island_centroid(int island) const;
    // Converts a delta in texture-UV space into the unwrap's mm space, undoing the layer's scale and
    // rotation - the inverse of what apply_uv_transform() did on the way in.
    Vec2f uv_delta_to_unwrap(const Vec2f &delta_uv) const;
    // The correction that would bring the selected island's nearest boundary vertex onto a boundary
    // vertex of some *other* island, in texture-UV space. Zero if nothing is within reach (#2).
    Vec2f snap_correction(int island) const;
    void  end_gesture();
    // Rebuilds the status line from the current gesture/selection and pushes it to m_on_status.
    void  update_status();

    wxGLContext *m_context = nullptr; // owned by OpenGLManager/GUI_App, not by this canvas

    Islands                      m_islands;
    std::vector<IslandTransform> m_transforms;
    // Per-island fill colour override (distortion heatmap); empty means use the default wash (#7).
    std::vector<ColorRGBA>       m_island_fill_colors;
    // Boundary vertices per island, for snapping - a patch's boundary is a tiny fraction of it, and
    // rescanning the whole uv array on every snap test would not be.
    std::vector<std::vector<int>> m_island_boundary_verts;

    bool m_mesh_dirty = true;
    // One set of models per island, so an island can be drawn through its own transform. Built once
    // per unwrap, never on a drag.
    std::vector<GLModel> m_island_wireframe; // interior edges
    std::vector<GLModel> m_island_boundary;  // outline
    std::vector<GLModel> m_island_fill;      // filled, for the selected island's wash

    GLModel m_tile_outline_glmodel; // the texture's first tile, [0,1]^2 - the "you are here"
    GLModel m_grid_glmodel;
    float   m_grid_step = 0.f; // the UV step m_grid_glmodel was built for; 0 = not built

    float m_tiling_scale  = 1.f;
    float m_rotation_deg  = 0.f;
    bool  m_tile_enabled  = true;
    bool  m_tile_mirrored = false;
    bool  m_snap_enabled  = false;

    std::vector<unsigned char> m_background_pixels; // RGBA, expanded from the grayscale input
    int                        m_background_width = 0, m_background_height = 0;
    bool                       m_background_dirty = false;     // the pixels need (re)uploading
    bool                       m_background_quad_dirty = true; // only the quad's extent changed
    GLTexture                  m_background_texture;
    // Covers content_bounds(), not just [0,1]: the unwrap is packed in mm and then divided by the
    // layer's tile size, so it routinely spans many tiles, and a single-unit-square backdrop would
    // leave most of the islands sitting over bare background. Texcoord == position, so the GL wrap
    // mode repeats it exactly the way DecodedHeightTexture::sample() does.
    GLModel                    m_background_glmodel;

    // 2D pan/zoom. m_pan is the UV-space point at the center of the view, m_zoom half the UV-space
    // extent visible across the shorter screen edge. v runs *down* the screen, matching both the
    // texture's own row order and every other UV editor's convention.
    Vec2f m_pan       = Vec2f(0.5f, 0.5f);
    float m_zoom      = 0.75f;
    bool  m_needs_fit = true; // fit the view to the next unwrap that arrives

    enum class Gesture
    {
        None,
        Pan,
        MoveIsland,
        RotateIsland,      // right-drag: rotation tracks the mouse, ends when the button is released
        RotateIslandModal, // 'R': rotation tracks the mouse until a click confirms or Esc cancels
        ScaleIslandModal,  // 'S': likewise, distance from the centre drives the scale
        MoveVertex,        // Vertex mode: drag one unwrapped vertex
        MoveEdge,          // Edge mode: drag both endpoints of one boundary edge
    };
    Gesture m_gesture         = Gesture::None;

    SelectMode          m_select_mode  = SelectMode::Island;
    // The sub-element being edited in Vertex/Edge mode (unwrapped-vertex indices), or -1/{-1,-1}. This
    // is the *primary* (last-picked) element of the multi-selection below.
    int                 m_active_vertex = -1;
    std::pair<int, int> m_active_edge{ -1, -1 };
    // Multi-selection for Vertex/Edge modes, mirroring the island selection: plain click replaces, Shift
    // adds, Ctrl toggles, and a drag moves the whole set together. m_active_vertex/m_active_edge stay the
    // primary. Kept as small vectors (tiny, and order doesn't matter here).
    std::vector<int>                 m_sel_vertices;
    std::vector<std::pair<int, int>> m_sel_edges;
    bool is_vertex_selected(int v) const
    {
        return std::find(m_sel_vertices.begin(), m_sel_vertices.end(), v) != m_sel_vertices.end();
    }
    bool is_edge_selected(const std::pair<int, int> &e) const
    {
        return std::find(m_sel_edges.begin(), m_sel_edges.end(), e) != m_sel_edges.end();
    }
    // Unique unwrapped-vertex endpoints of every selected edge (an endpoint shared by two selected edges
    // is returned once, so a drag doesn't move it twice).
    std::vector<int> selected_edge_endpoints() const;
    // Last known mouse position over the canvas, and whether the pointer is currently inside it. Used to
    // draw the +/- add/remove sign next to the cursor in Vertex/Edge mode.
    wxPoint m_cursor_px{ 0, 0 };
    bool    m_cursor_inside = false;
    // Set once a Vertex/Edge drag actually moves, so a bare click (select without drag) doesn't commit a
    // no-op edit and take an undo snapshot for nothing.
    bool                m_vertex_edit_moved = false;
    // Lazily-built small filled square, drawn at an edited/hovered vertex as a handle.
    GLModel m_vertex_marker_glmodel;
    // Rebuilt each frame at the cursor while a +/- add-remove hint is shown (Vertex/Edge mode).
    GLModel m_cursor_sign_glmodel;
    int     m_selected_island = -1;
    // The full multi-selection; m_selected_island is its primary (last-clicked) member. Kept as a small
    // vector rather than a set because it is tiny and iteration order (primary last) is convenient.
    std::vector<int> m_selection;
    bool             is_selected(int island) const
    {
        return std::find(m_selection.begin(), m_selection.end(), island) != m_selection.end();
    }
    wxPoint m_drag_last_px;
    Vec2f   m_gesture_last_uv    = Vec2f::Zero();
    float   m_gesture_last_angle = 0.f;
    float   m_gesture_last_dist  = 0.f;
    // Rotation is tracked as two running totals over the gesture: the raw mouse rotation, and how much
    // has actually been applied. With Shift held the applied total is quantised to 15-degree steps
    // (Blender-style angle snapping), so the two diverge - and driving the applied total off the raw
    // one, rather than snapping each incremental delta, is what makes the snap stable instead of
    // juddering. The raw/applied split also survives crossing +/-180 degrees, which a single wrapped
    // angle would not. m_rot_applied doubles as the modal-rotate undo amount for Esc.
    float   m_rot_raw_deg       = 0.f;
    float   m_rot_applied_deg   = 0.f;
    // The island's absolute on-screen rotation when the gesture began (decoded from its transform), so
    // Shift can snap to *global* 15-degree marks (0/15/30...) rather than 15 degrees relative to
    // wherever the island happened to start (#10). Also drives the angle read-out and the dial.
    float   m_rot_base_deg      = 0.f;
    float   m_rot_display_deg   = 0.f; // current absolute angle, for the status line and dial needle
    float   m_modal_scale_accum = 1.f; // so Esc can undo exactly what the modal scale applied, as a factor

    // A protractor drawn around the island while it rotates: a ring, a tick every 15 degrees, and a
    // needle at the current angle, so the rotation is legible (#11). Rebuilt each frame during a
    // rotation gesture (cheap: a few hundred short lines) and left empty otherwise.
    GLModel m_dial_glmodel;
    void    rebuild_rotation_dial();
    // The island's current absolute rotation in degrees, decoded from its transform's first column.
    float   island_rotation_deg(int island) const;

    IslandEditFn   m_on_island_edit;
    UVVertexEditFn m_on_vertex_edit;
    CommandFn      m_on_command;
    StatusFn       m_on_status;
};

// Hosts a UVEditorCanvas together with a small icon toolbar (frame, snap, average scale, cut,
// project-from-view) and a Blender-style status line along the bottom that names the current gesture
// and the shortcuts in play (#18). This is what actually goes into Plater's "uv_editor" AUI pane;
// the gizmo still talks to the inner canvas, reached via canvas().
class UVEditorPanel : public wxPanel
{
public:
    explicit UVEditorPanel(wxWindow *parent);
    UVEditorCanvas *canvas() { return m_canvas; }

private:
    void on_tool(wxCommandEvent &evt);

    UVEditorCanvas  *m_canvas = nullptr;
    wxToggleButton  *m_snap_button = nullptr;
    wxStaticText    *m_status = nullptr;
};

} // namespace Slic3r::GUI

#endif // slic3r_UVEditorCanvas_hpp_
