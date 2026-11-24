// One of the files GLGizmoTextureDisplacement is defined across. The class is declared once, in
// GLGizmoTextureDisplacement.hpp; its members are defined in GLGizmoTextureDisplacement.cpp and in the
// siblings named after the part of the gizmo they hold - Chroma, Library, Mesh, Preview, UV. The split
// is along those lines and nothing else: no member moved between parts, and the class did not change.
// Helpers more than one part needs live in GLGizmoTextureDisplacementInternal.hpp.

#include "GLGizmoTextureDisplacement.hpp"
#include "GLGizmoTextureDisplacementInternal.hpp"

#include <Eigen/Core>
#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/system/error_code.hpp>
#include <boost/log/trivial.hpp>

#include "ColorSpaceConvert.hpp"
#include "libslic3r/AABBTreeIndirect.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/FilamentMixer.hpp"
#include "libslic3r/ChromaSplit.hpp"
#include "libslic3r/ChromaStack.hpp"
#include "libslic3r/TransmissionDistance.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/MeshBoolean.hpp"
#include "libslic3r/NSVGUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/format.hpp"

#include <slic3r/GUI/AnimatedTexture.hpp>
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/CameraUtils.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GLToolbar.hpp" // GLToolbar::Default_Icons_Size, to match the toolbar's icon size
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/GuiColor.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/MainFrame.hpp" // wxGetApp().mainframe, as the projector window's parent
#include "slic3r/GUI/MeshUtils.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/OpenGLManager.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Shortcuts.hpp"
#include "slic3r/GUI/ChromaLibraries.hpp"
#include "slic3r/GUI/TextureLibrary.hpp"
#include "slic3r/GUI/TextureProjectorFrame.hpp"
#include "slic3r/GUI/UVEditorCanvas.hpp"
#include "slic3r/GUI/Jobs/TextureDisplacementBakeJob.hpp"
#include "slic3r/GUI/Jobs/TextureDisplacementPrepareJob.hpp"
#include "slic3r/GUI/Jobs/TextureDisplacementDebugJob.hpp"
#include "slic3r/GUI/Jobs/TextureDisplacementPreviewJob.hpp"
#include "libslic3r/ChromaProfile.hpp"
#include "slic3r/Utils/UndoRedo.hpp"
#include "GLGizmoUtils.hpp"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <chrono>
#include <future>
#include <cstdio>
#include <glad/gl.h>
#include <imgui.h>
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include <memory>
#include "slic3r/GUI/GLTexture.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/TextureDisplacement.hpp"
#include <string>
#include "slic3r/GUI/Gizmos/GLGizmoPainterBase.hpp"
#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/GLShader.hpp"
#include <math.h>
#include "slic3r/GUI/Gizmos/GLGizmoBase.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp"
#include "libslic3r/Geometry.hpp"
#include "slic3r/GUI/Event.hpp"
#include "slic3r/GUI/IconManager.hpp"
#include <optional>
#include "libslic3r/Config.hpp"
#include "libslic3r/TextureBake/TextureBakeDebug.hpp"
#include <ratio>
#include <tbb/parallel_for.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <queue>
#include <set>
#include <vector>
#include <utility>
#include <wx/gdicmn.h>
#include <wx/busycursor.h>
#include <wx/string.h>
#include <wx/filedlg.h>
#include <wx/dirdlg.h>

namespace Slic3r::GUI {

using namespace gizmo_td;

namespace {

// Intersects the camera ray through `mouse_pos` (screen coords) with the plane passing through
// `plane_point_local`/`plane_normal_local` (mesh-local coords, transformed to world by `trafo`).
// Returns false if the ray is parallel to the plane or the plane is behind the camera.
bool ray_plane_hit(const Camera &camera, const Vec2d &mouse_pos, const Transform3d &trafo,
                    const Vec3f &plane_point_local, const Vec3f &plane_normal_local, Vec3d &out_world_hit)
{
    Vec3d ray_origin, ray_dir;
    CameraUtils::ray_from_screen_pos(camera, mouse_pos, ray_origin, ray_dir);

    const Vec3d plane_point_world  = trafo * plane_point_local.cast<double>();
    const Vec3d plane_normal_world = (trafo.matrix().block(0, 0, 3, 3).inverse().transpose() * plane_normal_local.cast<double>()).normalized();

    const double denom = ray_dir.dot(plane_normal_world);
    if (std::abs(denom) < 1e-8)
        return false;

    const double t = (plane_point_world - ray_origin).dot(plane_normal_world) / denom;
    if (t < 0.0)
        return false;

    out_world_hit = ray_origin + ray_dir * t;
    return true;
}
} // namespace

GLGizmoTextureDisplacement::GLGizmoTextureDisplacement(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
    : GLGizmoPainterBase(parent, icon_filename, sprite_id)
{
    // No band's second form is open, and -1 rather than 0 is what says so: a zero-initialised array
    // would mean every card opened with its first filament already expanded.
    m_chroma_band_open.fill(-1);
    // The divider starts in the middle, half picture and half print.
    m_chroma_split.fill(0.5f);
}

bool GLGizmoTextureDisplacement::on_init()
{
    m_shortcut = Shortcut::GizmoDisplacement;
    const wxString ctrl  = GUI::shortkey_ctrl_prefix();
    const wxString alt   = GUI::shortkey_alt_prefix();
    const wxString shift = GUI::shortkey_shift_prefix();

    m_desc["cursor_size"]   = _L("Brush size");
    m_desc["circle"]        = _L("Circle");
    m_desc["sphere"]        = _L("Sphere");
    m_desc["remove_layer"]  = _L("Remove");
    m_desc["bake"]          = _L_CONTEXT("Bake", "Texture Displacement");

    m_desc["paint"]            = _L("Paint");
    m_desc["erase"]            = _L("Erase");
    m_desc["gap_area"]         = _L("Gap area");
    m_desc["smart_fill_angle"] = _L("Smart fill angle");
    m_desc["toggle_wireframe"] = _L("Toggle Wireframe");

    std::pair<wxString, wxString> paint_shortcut            = {_L("Left mouse button"),         m_desc["paint"]};
    std::pair<wxString, wxString> erase_shortcut            = {shift + _L("Left mouse button"), m_desc["erase"]};
    std::pair<wxString, wxString> toggle_wireframe_shortcut = {alt + shift + _L_CONTEXT("Enter", "Keyboard Shortcut"), m_desc["toggle_wireframe"]};

    m_shortcuts_brush = {
        paint_shortcut,
        erase_shortcut,
        {ctrl + _L("Mouse wheel"), m_desc["cursor_size"]},
        toggle_wireframe_shortcut
    };

    m_shortcuts_bucket_fill = {
        paint_shortcut,
        erase_shortcut,
        {ctrl + _L("Mouse wheel"), m_desc["smart_fill_angle"]},
        toggle_wireframe_shortcut
    };

    m_shortcuts_gap_fill = {
        {ctrl + _L("Mouse wheel"), m_desc["gap_area"]},
        toggle_wireframe_shortcut
    };

    // Both ways into the same input box on any slider in the panel: ImGui turns a slider into one when
    // it is Ctrl+clicked or when it is given the keyboard focus, which is what the edit icon asks for.
    m_desc["type_value"] = _L("Type an exact value into a slider");
    m_shortcuts_sliders  = {
        {ctrl + _L("Left mouse button"), m_desc["type_value"]},
        {_L("Edit icon at a slider's right end"), m_desc["type_value"]},
    };
    for (std::vector<std::pair<wxString, wxString>> *list : {&m_shortcuts_brush, &m_shortcuts_bucket_fill, &m_shortcuts_gap_fill})
        list->insert(list->end(), m_shortcuts_sliders.begin(), m_shortcuts_sliders.end());

    return true;
}

std::string GLGizmoTextureDisplacement::on_get_name() const
{
    return _u8L("Texture tool");
}

bool GLGizmoTextureDisplacement::on_is_activable() const
{
    return m_parent.get_canvas_type() != GLCanvas3D::CanvasAssembleView && GLGizmoPainterBase::on_is_activable();
}

void GLGizmoTextureDisplacement::on_shutdown()
{
    m_parent.toggle_model_objects_visibility(true);
    m_preview_glmodel.reset();
    m_part_previews.clear();
    m_shaded_preview_glmodel.reset();
    m_paint_overlay_glmodel.reset();
    m_paint_overlay_dirty = false;
    m_painted_colors = PaintedColors{};
    m_painted_colors_key.clear();
    m_painted_colors_glmodel.reset();
    m_painted_colors_drawn_key.clear();
    m_painted_colors_runs.clear();
    m_seed_fill_last_mesh_id = -1; // a hover from this session must not count in the next
    // Any preview still in flight is superseded: raising the shared counter makes it abort at its next
    // progress poll, and its completion handler then finds nothing to do.
    m_preview_generation->fetch_add(1);
    m_preview_job_pending = false;
    // The adjustment panel holds a full-size decode of its layer's image, which is the largest thing
    // here; a closed gizmo should not keep it alive. Discards the edit, as closing the gizmo on an open
    // panel implies.
    close_image_adjust();
    // The retouch brush is a mode, and a mode must not survive the gizmo it belongs to: reopening on
    // another object with the brush still armed would make the first drag paint a correction onto it.
    // A stroke in progress is dropped rather than committed, as closing the gizmo mid-drag implies.
    m_chroma_retouch_mode     = false;
    m_chroma_retouch_painting = false;
    m_chroma_mask.clear();
    m_chroma_mask_w = m_chroma_mask_h = 0;
    for (std::unique_ptr<GLTexture> &tex : m_chroma_error_tex)
        tex.reset(); // fill() cannot: a unique_ptr is not copyable
    // The palette caches hold the last volume's images; a closed gizmo should not keep them alive.
    m_palette_cache.clear();
    m_palette_quantizer      = nullptr;
    m_palette_pure_quantizer = nullptr;
    m_palette_filaments.clear();
    m_palette_images.clear();
    m_mix_ranking.reset();
    m_palette_slots.clear();
    m_palette_changed = false;
    m_uvcheck_glmodel.reset();
    m_wireframe_overlay_glmodel.reset();
    m_wireframe_overlay_vcount = 0;
    m_seam_glmodel.reset();
    m_seam_hover_glmodel.reset();
    m_seam_hover_edge     = { -1, -1 };
    m_seam_hover_vertex   = -1;
    m_seam_anchor_glmodel.reset();
    m_seam_path_mode      = false;
    m_seam_path_anchor    = -1;
    m_subdivide_editing       = false;
    m_subdivide_preview_tris = -1;
    m_subdivide_preview_glmodel.reset();
    m_shaded_active_chart  = -1;
    m_shaded_active_face.clear();
    m_shaded_island_delta  = Eigen::Matrix<float, 2, 3>::Identity();
    m_island_drag_active = false;
    m_island_move_set.clear();
    // The pane closes below, taking the highlight with it, and the view must not open the next session in a
    // Fast the user never picked. Set directly: nothing is to be queued against a gizmo that is closing.
    if (m_island_view.switched)
        m_use_shaded_preview = false;
    m_island_view = IslandViewFollow{};
    m_adjust_texture_mode = false;
    m_seam_edit_mode      = false;
    m_adjust_drag_handle  = AdjustHandle::None;
    m_adjust_anchor_valid = false;
    // The pane is request-only: closing the gizmo drops the request, so reopening it later doesn't
    // silently reopen the pane too.
    m_show_uv_editor = false;
    wxGetApp().plater()->show_uv_editor(false);

    // Destroyed, not just hidden: unlike the UV pane (owned by Plater), this frame is owned here, and
    // it holds a callback capturing `this`. Leaving it alive past the gizmo would leave that callback
    // pointing at a gizmo that is no longer driving anything.
    if (m_projector_frame != nullptr) {
        m_projector_frame->Destroy();
        m_projector_frame = nullptr;
    }
    m_projector_tex_source    = nullptr; // a rebuilt frame starts with no texture in it
    m_projector_tex_smoothing = -1.f;
}

PainterGizmoType GLGizmoTextureDisplacement::get_painter_type() const
{
    return PainterGizmoType::TEXTURE_DISPLACEMENT;
}

wxString GLGizmoTextureDisplacement::handle_snapshot_action_name(bool shift_down, GLGizmoPainterBase::Button button_down) const
{
    // Right button does the opposite of the toggle; Shift always erases.
    const bool erasing = shift_down || (m_erase_mode != (button_down == GLGizmoPainterBase::Button::Right));
    return erasing ? _L("Erase texture displacement paint") : _L("Paint texture displacement");
}

void GLGizmoTextureDisplacement::render_painter_gizmo()
{
    const Selection &selection = m_parent.get_selection();

    glsafe(::glEnable(GL_BLEND));
    glsafe(::glEnable(GL_DEPTH_TEST));

    // Once anything is painted, m_preview_glmodel holds the true displaced result (same algorithm
    // Bake uses). The untouched original topology (what render_triangles() draws) coincides exactly
    // with it everywhere except the displaced area, so both can be drawn: the real preview geometry
    // first, then the selectors' highlight with a small depth bias so it wins the depth test on the
    // coincident surface. Where the surface has actually been displaced, the raised preview geometry
    // legitimately occludes the flat highlight - that visible relief is itself the "this is painted"
    // indicator in that area.
    //
    // The shaded preview never actually moves geometry (it only shades), so its depth is identical to
    // the highlight's *everywhere*: the depth-biased opaque highlight would win the depth test across the
    // whole surface and hide the shading entirely. So render_triangles() leaves the textured volume out
    // there, and the translucent tint drawn further down is the paint feedback instead.
    // Coalesced shaded-preview rebuild from an in-progress UV island drag (see on_island_edited): done here, at
    // most once per drawn frame, rather than synchronously in the UV canvas's mouse-move handler.
    if (m_use_shaded_preview && m_shaded_preview_dirty) {
        rebuild_shaded_preview_mesh();
        m_shaded_preview_dirty = false;
    }
    // Same coalescing for the paint tint, but on its own flag: a stroke marks this every mouse move
    // (see on_mouse()) and it only costs the painted patch
    if (m_paint_overlay_dirty) {
        rebuild_paint_overlay();
        m_paint_overlay_dirty = false;
    }
    rebuild_other_paint_overlay(); // a no-op unless another layer's paint, the active layer or the preview changed
    // is_initialized() alone is not enough: render_shaded_preview_mesh() also needs an active layer
    // with a decoded texture and a compiled shader, and bails silently without them. Hiding the real
    // volume for a shaded pass that then draws nothing is what made the model vanish - most obviously
    // with zero layers, but equally with a layer that has no texture picked yet.
    const bool use_shaded = m_use_shaded_preview && m_shaded_preview_glmodel.is_initialized() && shaded_preview_ready();
    // Checker/Distortion are built from the *base* patch and drawn with a polygon offset, which biases
    // depth values - it does not move the geometry. It therefore cannot win against a surface that
    // genuinely stands in front, and the displaced preview does exactly that: it rises above the base
    // surface by the layer's depth. Drawn underneath a UV-check overlay it simply occludes it, which is
    // why those two modes looked like they did nothing. Leave it out and let the undisplaced volume show
    // through instead (toggle_model_objects_visibility below) - that one *is* coincident with the
    // overlay, which is what the offset assumes, and it is the surface whose mapping is being inspected.
    const bool use_true_preview = !use_shaded && m_uv_check_mode == UVCheckMode::None && has_true_preview();
    // In Checker/Distortion mode the UV-check overlay *is* the surface visualization the user is
    // looking at, so the opaque paint-selection highlight must not be drawn on top of it - same
    // reasoning as skipping it for the shaded preview (see bug #12). Without this the painted area
    // covers the checker/heatmap and it can't be seen.
    const bool show_paint_overlay = m_uv_check_mode == UVCheckMode::None;

    // Whether the ChromaStack overlay is drawn as a translucent plate this frame.
    //
    // Decided here, before anything is drawn, because a see-through pass cannot be an overlay: blended
    // over an opaque copy of its own surface it blends with the copy, which is exactly what the plate
    // looked like before any of this existed. The opaque surface is left out of the frame instead - and
    // that means geometry gets hidden on the strength of a draw that has not happened yet, where the one
    // way to end up with an invisible model is a guard this test does not share with
    // render_chroma_colors(). So it repeats every one of them, the mesh upload included: that is a hash
    // lookup after the first frame (see rebuild_chroma_colors) and it is the only guard that cannot be
    // answered without doing the work.
    const TextureDisplacementLayer *chroma_layer       = active_layer();
    // A picture with no painted face has nowhere to be yet. Drawn anyway, every view below would put
    // it over the whole object, coloured in bands counted from a base that is only the default -
    // which reads as the tool having painted something by itself.
    if (chroma_layer != nullptr && chroma_layer->chroma_enabled && !chroma_patch(*chroma_layer).valid)
        chroma_layer = nullptr;
    bool                            chroma_see_through = false;
    if (m_chroma_see_through && m_show_chroma_colors && !m_show_chroma_error && m_chroma_debug == 0 &&
        m_debug_stage < 0 && chroma_layer != nullptr && chroma_layer->chroma_enabled &&
        !chroma_layer->chroma.bands.empty() &&
        // Back lit, the plate stands in front of its own lamp and the shader draws what the lamp sends
        // through it; there is no scene behind it left to show. The shader keeps the plate opaque in that
        // mode, so hiding the surface under it would buy nothing and risk the model.
        chroma_layer->chroma.lighting == ChromaLighting::FrontLit && texture_volume() != nullptr &&
        m_c->selection_info()->model_object() != nullptr && wxGetApp().get_shader("chromastack") != nullptr) {
        rebuild_chroma_colors(*chroma_layer);
        chroma_see_through = m_chroma_color_model.is_initialized();
    }

    // The layered view, decided the same way and for the same reason: it replaces the plate's own
    // surface rather than covering it, so the geometry is hidden on the strength of a draw that has
    // not happened yet and every guard that draw applies has to be answered here first.
    //
    // It wins over the see-through pass when both are asked for. Both are the plate drawn translucent,
    // and the layered one is the finer answer - it is the stack with its layers apart, where the other
    // is the stack composited onto one surface - so running them together would blend the same light
    // into the frame twice.
    const bool chroma_layered = m_show_chroma_layers && !m_show_chroma_error && m_chroma_debug == 0 &&
                                m_debug_stage < 0 && chroma_layer != nullptr &&
                                chroma_layers_ready(*chroma_layer);
    if (chroma_layered)
        chroma_see_through = false;

    // Hide the real volume only when something is actually going to be drawn in its place; otherwise
    // put it back. Getting this wrong leaves an invisible model, so it is decided once, here, rather
    // than per branch below.
    ModelVolume *mv = texture_volume();
    // The parts a preview is standing in for, and whose real volumes therefore have to be hidden and
    // whose selectors must not be drawn over it. The shaded view only ever shades the layer stack's own
    // part; the true-displacement view covers every painted part of a multibody object.
    std::vector<const ModelVolume *> previewed;
    if (use_shaded && mv != nullptr) {
        previewed.push_back(mv);
    } else if (use_true_preview) {
        if (mv != nullptr && m_preview_glmodel.is_initialized())
            previewed.push_back(mv);
        for (const std::unique_ptr<PartPreview> &preview : m_part_previews)
            if (const ModelVolume *part = preview_part(*preview); part != nullptr)
                previewed.push_back(part);
    }
    m_parent.toggle_model_objects_visibility(
        true,
        m_c->selection_info()->model_object(),
        m_c->selection_info()->get_active_instance()
    );
    for (const ModelVolume *volume : previewed)
        m_parent.toggle_model_objects_visibility(false, m_c->selection_info()->model_object(),
                                                  m_c->selection_info()->get_active_instance(), volume);
    // A see-through or layered chroma view draws the plate itself, so the textured volume is hidden
    // with the previews whether or not a preview mesh is standing over it.
    if ((chroma_see_through || chroma_layered) && mv != nullptr)
        m_parent.toggle_model_objects_visibility(false, m_c->selection_info()->model_object(),
                                                  m_c->selection_info()->get_active_instance(), mv);

    // Over a preview, the paint feedback - the selectors' highlight in the Normal view, the tint in both -
    // is drawn only while a mouse button is down: during a stroke, a fill click included. Between strokes
    // it would cover the preview: the highlight buries it under a flat plane wherever the relief does not
    // rise, and the tint washes it green. The debug view shows a captured stage rather than a preview, so
    // it keeps the feedback.
    const bool paint_feedback = is_painting() || m_debug_stage >= 0;

    // Whether the textured volume's selector - and with it a fill tool's contour - is drawn this frame.
    bool textured_selector_drawn = true;

    // Every branch below leaves the textured volume's own surface out when chroma_see_through is on,
    // the paint feedback included: the selectors' highlight is an opaque plane over the whole volume,
    // and it would block the view through the plate just as surely as the preview mesh does. A stroke
    // over a see-through plate therefore shows its feedback as the translucent tint further down and
    // not as the highlight, which is the same trade the shaded preview already makes.
    const bool chroma_replaces_surface = chroma_see_through || chroma_layered;
    if (use_shaded) {
        if (!chroma_replaces_surface)
            render_shaded_preview_mesh();
        // The shaded mesh is the textured volume alone, so the other model parts are still the selectors' to draw.
        render_triangles(selection, previewed);
        textured_selector_drawn = false;
    } else if (use_true_preview) {
        if (!chroma_replaces_surface)
            render_preview_mesh();
        glsafe(::glEnable(GL_POLYGON_OFFSET_FILL));
        glsafe(::glPolygonOffset(-1.0f, -1.0f));
        // Without paint feedback, only the parts with no preview over them: render_preview_mesh() draws
        // the rest. A chroma view that replaces the plate's surface keeps the textured volume out even
        // while painting, since the selectors' highlight is an opaque plane over the whole of it and
        // would block the view through the plate.
        std::vector<const ModelVolume *> skip = paint_feedback ? std::vector<const ModelVolume *>{} : previewed;
        if (chroma_replaces_surface && mv != nullptr && std::find(skip.begin(), skip.end(), mv) == skip.end())
            skip.push_back(mv);
        render_triangles(selection, skip);
        glsafe(::glDisable(GL_POLYGON_OFFSET_FILL));
        textured_selector_drawn = paint_feedback && !chroma_replaces_surface;
    } else if (chroma_replaces_surface) {
        // No preview mesh of its own - a UV-check overlay, or nothing painted yet - so the other model
        // parts are all that is left for the selectors to draw.
        render_triangles(selection, mv);
        textured_selector_drawn = false;
    } else {
        // render_triangles() *is* the model in a painter gizmo (it draws every model-part volume with the
        // selector's colours), not an overlay on top of one - so it still has to run under a UV-check
        // overlay, or nothing draws the surface at all and the checker floats alone over an empty scene.
        // Deliberately without the depth bias the branch above applies: the checker/heatmap is drawn later
        // with its own -1 offset and has to win against this. Biasing both by the same amount is what made
        // the painted area cover the checker and is why this call used to be skipped outright.
        render_triangles(selection);
    }

    // The model's colour paint, over whichever surface was drawn, left out where that surface's preview shows
    // paint of its own: every layer's for the Normal mesh, the active layer's otherwise.
    const bool painted_colors_drawn = show_paint_overlay && m_debug_stage < 0 && render_painted_colors(use_true_preview);

    // The ChromaStack stack's colours, over the same surface. No ordering question with the paint above:
    // a ChromaStack layer colours by filament swap and never carries per-triangle colour, so the two
    // overlays are mutually exclusive by construction.
    // Either view of the stack draws the same overlay: the predicted colours, or how far from the picture
    // they landed. The error view is the one case where the overlay is wanted with the colours off, so it
    // has to open this gate on its own.
    const bool chroma_colors_drawn = !chroma_layered &&
                                       (m_show_chroma_colors || m_show_chroma_error || m_chroma_debug != 0) &&
                                       m_debug_stage < 0 &&
                                       chroma_layer != nullptr && chroma_layer->chroma_enabled &&
                                       render_chroma_colors(*chroma_layer, chroma_see_through);
    // The stack as its own sheets, in place of all of it: the picture is what the sheets add up to, so
    // there is no surface underneath for an overlay to sit on.
    if (chroma_layered)
        render_chroma_layers(*chroma_layer);

    // Every other layer's paint, in muted grey, so all layers stay visible while one of them is edited. Drawn
    // before the active layer's tint so that one reads on top where the two overlap.
    if (show_paint_overlay)
        render_paint_overlay(m_other_paint_glmodel);

    // The translucent paint tint. Needed in the shaded view because the opaque highlight above is
    // skipped there, and in the true-displacement view because the displaced surface rises *above*
    // the undisplaced overlay geometry and hides it exactly where the relief is strongest - in both
    // cases leaving an erase stroke with no visible effect until the next full preview rebuild. With no
    // preview, the highlight is the selectors' own, except during a stroke over the colour paint: the paint
    // there is not flushed yet, so the colours are still drawn over the stroke's highlight.
    const bool preview_drawn = use_shaded || use_true_preview;
    if (show_paint_overlay && (preview_drawn ? paint_feedback : (painted_colors_drawn || chroma_colors_drawn) && is_painting()))
        render_paint_overlay(m_paint_overlay_glmodel);

    // A fill tool's contour is drawn with the selectors, under the colour paint drawn since - which covers it
    // on a steep face, where the paint's slope-scaled offset outruns the contour's fixed one. Put it back on top,
    // at the depth its first draw stored, hence LEQUAL. Only where the selector was drawn this frame: drawing it
    // is what rebuilds the contour. The tool test matters because the base keeps the last hovered mesh when the
    // tool changes.
    const bool fill_tool = m_tool_type == ToolType::SMART_FILL || m_tool_type == ToolType::BUCKET_FILL ||
                           (m_tool_type == ToolType::BRUSH && m_cursor_type == TriangleSelector::CursorType::POINTER);
    const int  textured_mesh_id = texture_volume_raycaster_index();
    if (painted_colors_drawn && textured_selector_drawn && fill_tool && textured_mesh_id >= 0 &&
        textured_mesh_id == m_seed_fill_last_mesh_id && size_t(textured_mesh_id) < m_triangle_selectors.size()) {
        const ModelObject *mo = m_c->selection_info()->model_object();
        GLint depth_func = GL_LESS;
        glsafe(::glGetIntegerv(GL_DEPTH_FUNC, &depth_func));
        glsafe(::glDepthFunc(GL_LEQUAL));
        m_triangle_selectors[size_t(textured_mesh_id)]->render_paint_contour(
            mo->instances[selection.get_instance_idx()]->get_transformation().get_matrix() * mv->get_matrix());
        glsafe(::glDepthFunc(GLenum(depth_func)));
    }

    // The UV editor's island selection, shown on the model. Polled here rather than pushed: the pane
    // changes its selection in its own mouse handling, and a compare of a few ints per frame is free.
    if (const std::vector<int> *islands = island_highlight_selection()) {
        if (*islands != m_island_overlay_selection)
            rebuild_island_overlay(*islands);
        render_island_overlay();
    } else if (m_island_overlay_glmodel.is_initialized()) {
        m_island_overlay_glmodel.reset();
        m_island_overlay_selection.clear();
    }

    // Diagnostic overlays, drawn on top of whatever preview is active (both pull toward the camera
    // with a polygon offset so they win the depth test against the coincident surface).
    if (m_uv_check_mode != UVCheckMode::None)
        render_uvcheck_mesh();
    // While previewing a subdivision, its wireframe stands in for the mesh wireframe - it shows the
    // density the model *would* have. The normal wireframe toggle is left untouched underneath, so it
    // returns to whatever it was once the preview ends (which is what keeps an already-on wireframe on).
    if (m_subdivide_editing)
        render_subdivide_preview();
    else if (m_wireframe_overlay)
        render_wireframe_overlay();
    // Marked seams are always shown for an LSCM layer, so existing cuts are visible before entering
    // seam-edit mode - but they matter most while marking.
    render_seam_overlay();

    m_c->object_clipper()->render_cut();
    m_c->instances_hider()->render_cut();
    if (m_adjust_texture_mode)
        render_adjust_texture_gizmo();
    else if (m_chroma_retouch_mode) // its own brush, since this mode never reaches the painter base
        render_chroma_brush();
    else if (!m_seam_edit_mode) // the brush cursor is meaningless while marking seams
        render_cursor();

    glsafe(::glDisable(GL_BLEND));
}

bool GLGizmoTextureDisplacement::on_mouse(const wxMouseEvent &mouse_event)
{
    if (m_seam_edit_mode)
        return on_mouse_seam(mouse_event);
    if (m_adjust_texture_mode)
        return on_mouse_adjust_texture(mouse_event);
    if (m_chroma_retouch_mode)
        return on_mouse_chroma_retouch(mouse_event);
    // While the mode picker is up there is no layer to paint into, so the mouse is the camera's.
    if (const ModelVolume *mv = texture_volume(); mv != nullptr && mv->texture_displacement_layers.empty())
        return false;

    const bool handled = GLGizmoPainterBase::on_mouse(mouse_event);
    // A consumed drag/click is a paint (or erase) event: the base class has already updated the live
    // TriangleSelector, but nothing is flushed to the model - and so nothing rebuilds - until the
    // stroke ends. Mark the tint stale so it follows the brush from the first frame instead. Only the
    // flag is set here; the rebuild is coalesced to once per drawn frame in render_painter_gizmo().
    if (handled && (mouse_event.Dragging() || mouse_event.LeftDown() || mouse_event.RightDown() ||
                    mouse_event.LeftUp() || mouse_event.RightUp()))
        m_paint_overlay_dirty = true;
    return handled;
}

bool GLGizmoTextureDisplacement::update_adjust_anchor()
{
    const ModelVolume *mv = texture_volume();
    m_adjust_anchor_valid = mv != nullptr &&
        compute_layer_paint_anchor(mv->mesh().its, mv->texture_displacement_facet(m_active_layer_slot).get_data(),
                                    m_adjust_anchor_pos, m_adjust_anchor_normal);
    return m_adjust_anchor_valid;
}

TextureDisplacementLayer *GLGizmoTextureDisplacement::active_layer()
{
    ModelVolume *mv = texture_volume();
    if (mv == nullptr)
        return nullptr;
    for (TextureDisplacementLayer &l : mv->texture_displacement_layers)
        if (l.slot == m_active_layer_slot)
            return &l;
    return nullptr;
}

const TextureDisplacementLayer *GLGizmoTextureDisplacement::active_layer() const
{
    return const_cast<GLGizmoTextureDisplacement *>(this)->active_layer();
}

Vec3f GLGizmoTextureDisplacement::adjust_plane_point() const
{
    const ModelVolume *mv        = texture_volume();
    const float        bbox_size = (mv != nullptr) ? float(mv->mesh().bounding_box().size().norm()) : 1.f;
    const float        lift      = bbox_size * 0.01f + 0.2f; // clear of the surface, to avoid z-fighting
    return m_adjust_anchor_pos + m_adjust_anchor_normal * lift;
}

Vec3f GLGizmoTextureDisplacement::adjust_handle_center(const TextureDisplacementLayer &layer) const
{
    // apply_uv_transform() maps a planar mm coordinate p to uv = R(p / tiling_scale) + offset, and
    // on_mouse_adjust_texture() drives offset by  offset = offset_start - R(delta / tiling_scale).
    // Inverting that, the handle's displacement from the anchor is  - R^-1(offset) * tiling_scale -
    // which, substituted into the drag equation, moves the handle by exactly `delta`. So the handle
    // follows the cursor precisely, and is back on the anchor exactly when offset is zero.
    const float rad = layer.rotation_deg * float(M_PI) / 180.f;
    const float cs = std::cos(rad), sn = std::sin(rad);
    // Undo the non-square v scaling first - it is the last thing apply_uv_transform() does, so it is
    // the first thing to come off on the way back.
    const float aspect = layer_texture_aspect(layer);
    const Vec2f o(layer.offset.x(), (aspect > 0.f) ? layer.offset.y() / aspect : layer.offset.y());
    const Vec2f unrotated(o.x() * cs + o.y() * sn, -o.x() * sn + o.y() * cs);
    const Vec2f planar = -unrotated * layer.tiling_scale;

    Vec3f u_axis, v_axis;
    adjust_tangent_basis(u_axis, v_axis);
    return adjust_plane_point() + u_axis * planar.x() + v_axis * planar.y();
}

void GLGizmoTextureDisplacement::adjust_tangent_basis(Vec3f &u_axis, Vec3f &v_axis) const
{
    // libslic3r's own basis, asked for rather than restated here, so the ring's "0 degrees" and the
    // offset handle's plane always agree with what project_texture_displacement_uv() does - including
    // v pointing *down* the texture and u flipping on the far side of each plane.
    box_projection_axes(m_adjust_anchor_normal, u_axis, v_axis);
}

void GLGizmoTextureDisplacement::render_adjust_texture_gizmo()
{
    if (!m_adjust_anchor_valid)
        return;

    const ModelObject              *mo    = m_c->selection_info()->model_object();
    const ModelVolume              *mv    = texture_volume();
    const TextureDisplacementLayer *layer = active_layer();
    if (mo == nullptr || mv == nullptr || layer == nullptr)
        return;

    const Selection  &selection    = m_parent.get_selection();
    const Transform3d trafo_matrix = mo->instances[selection.get_instance_idx()]->get_transformation().get_matrix() * mv->get_matrix();

    // Handle sizes scale with the volume so they stay usable on both tiny and huge models.
    const float bbox_size    = float(mv->mesh().bounding_box().size().norm());
    const float panel_half   = bbox_size * 0.04f + 1.f;
    const float arrow_length = panel_half * 2.2f;

    // Tracks the layer's offset, so the handle actually travels with the texture as it is dragged.
    const Vec3f handle_center_local = adjust_handle_center(*layer);

    GLShaderProgram *shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    glsafe(::glDisable(GL_DEPTH_TEST));
    glsafe(::glEnable(GL_BLEND));
    shader->start_using();

    const Camera &camera = wxGetApp().plater()->get_camera();

    Vec3f u_axis, v_axis;
    adjust_tangent_basis(u_axis, v_axis);
    Transform3d plane_transform     = Transform3d::Identity();
    plane_transform.linear().col(0) = u_axis.cast<double>();
    plane_transform.linear().col(1) = v_axis.cast<double>();
    plane_transform.linear().col(2) = m_adjust_anchor_normal.cast<double>();
    plane_transform.translation()   = handle_center_local.cast<double>();

    // Pan panel: a flat square lying in the patch's own tangent plane. Dragging anywhere on it
    // moves the texture freely along both axes at once.
    if (!m_adjust_panel_glmodel.is_initialized()) {
        GLModel::Geometry init_data;
        init_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
        init_data.reserve_vertices(4);
        init_data.reserve_indices(6);
        init_data.add_vertex(Vec3f(-1.f, -1.f, 0.f));
        init_data.add_vertex(Vec3f(1.f, -1.f, 0.f));
        init_data.add_vertex(Vec3f(1.f, 1.f, 0.f));
        init_data.add_vertex(Vec3f(-1.f, 1.f, 0.f));
        init_data.add_triangle(0, 1, 2);
        init_data.add_triangle(0, 2, 3);
        m_adjust_panel_glmodel.init_from(std::move(init_data));
    }
    Transform3d view_model_matrix = camera.get_view_matrix() * trafo_matrix * plane_transform *
        Geometry::assemble_transform(Vec3d::Zero(), Vec3d::Zero(), Vec3d(panel_half, panel_half, panel_half));
    shader->set_uniform("view_model_matrix", view_model_matrix);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    ColorRGBA panel_color = m_adjust_drag_handle == AdjustHandle::Pan ? ColorRGBA::YELLOW() : ColorRGBA::ORANGE();
    panel_color.a(0.45f);
    m_adjust_panel_glmodel.set_color(panel_color);
    m_adjust_panel_glmodel.render();

    // Axis arrows: a shaft plus a small V-shaped arrowhead, both along local +X. Reused for both
    // the U and V axes below by swapping which world direction local +X is transformed to.
    if (!m_adjust_arrow_glmodel.is_initialized()) {
        GLModel::Geometry init_data;
        init_data.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
        init_data.reserve_vertices(4);
        init_data.reserve_indices(6);
        init_data.add_vertex(Vec3f(0.f, 0.f, 0.f));
        init_data.add_vertex(Vec3f(1.f, 0.f, 0.f));
        init_data.add_vertex(Vec3f(0.82f, 0.08f, 0.f));
        init_data.add_vertex(Vec3f(0.82f, -0.08f, 0.f));
        init_data.add_line(0, 1);
        init_data.add_line(1, 2);
        init_data.add_line(1, 3);
        m_adjust_arrow_glmodel.init_from(std::move(init_data));
    }
#if !SLIC3R_OPENGL_ES
    if (!OpenGLManager::get_gl_info().is_core_profile())
        glsafe(::glLineWidth(2.0f));
#endif // !SLIC3R_OPENGL_ES

    auto render_arrow = [&](const Vec3f &axis, const Vec3f &other_axis, bool is_active) {
        Transform3d arrow_transform     = Transform3d::Identity();
        arrow_transform.linear().col(0) = axis.cast<double>();
        arrow_transform.linear().col(1) = other_axis.cast<double>();
        arrow_transform.linear().col(2) = m_adjust_anchor_normal.cast<double>();
        arrow_transform.translation()   = handle_center_local.cast<double>();

        const Transform3d vmm = camera.get_view_matrix() * trafo_matrix * arrow_transform *
            Geometry::assemble_transform(Vec3d::Zero(), Vec3d::Zero(), Vec3d(arrow_length, arrow_length, arrow_length));
        shader->set_uniform("view_model_matrix", vmm);
        shader->set_uniform("projection_matrix", camera.get_projection_matrix());
        m_adjust_arrow_glmodel.set_color(is_active ? ColorRGBA::YELLOW() : ColorRGBA::ORANGE());
        m_adjust_arrow_glmodel.render();
    };
    render_arrow(u_axis, v_axis, m_adjust_drag_handle == AdjustHandle::AxisU);
    render_arrow(v_axis, u_axis, m_adjust_drag_handle == AdjustHandle::AxisV);

    shader->stop_using();
    glsafe(::glDisable(GL_BLEND));
    glsafe(::glEnable(GL_DEPTH_TEST));
}

bool GLGizmoTextureDisplacement::on_mouse_adjust_texture(const wxMouseEvent &mouse_event)
{
    if (!m_adjust_anchor_valid)
        return false;

    ModelVolume              *mv    = texture_volume();
    ModelObject              *mo    = m_c->selection_info()->model_object();
    TextureDisplacementLayer *layer = active_layer();
    if (mv == nullptr || mo == nullptr || layer == nullptr)
        return false;

    const Selection   &selection    = m_parent.get_selection();
    const Transform3d  trafo_matrix = mo->instances[selection.get_instance_idx()]->get_transformation().get_matrix() * mv->get_matrix();
    const Camera      &camera       = wxGetApp().plater()->get_camera();

    const float bbox_size    = float(mv->mesh().bounding_box().size().norm());
    const float panel_half   = bbox_size * 0.04f + 1.f;
    const float arrow_length = panel_half * 2.2f;
    // Where the handle is drawn (moves with the layer's offset) vs. the plane the drag is measured
    // against (fixed at the anchor). Keeping them apart is what stops the handle's own motion from
    // feeding back into the delta that produced it.
    const Vec3f handle_center_local = adjust_handle_center(*layer);
    const Vec3f drag_plane_local    = adjust_plane_point();
    const Vec3d handle_center_world = trafo_matrix * handle_center_local.cast<double>();
    const Vec2d mouse_pos(mouse_event.GetX(), mouse_event.GetY());

    Vec3f u_axis, v_axis;
    adjust_tangent_basis(u_axis, v_axis);

    const Point handle_screen = CameraUtils::project(camera, handle_center_world);
    const Vec2d handle_screen_d(double(handle_screen.x()), double(handle_screen.y()));

    // Point-to-segment distance in screen space, for the arrow shafts.
    auto dist_to_segment_px = [](const Vec2d &p, const Vec2d &a, const Vec2d &b) {
        const Vec2d ab = b - a;
        const double len2 = ab.squaredNorm();
        const double t = (len2 > 1e-9) ? std::clamp((p - a).dot(ab) / len2, 0.0, 1.0) : 0.0;
        return (p - (a + ab * t)).norm();
    };

    if (mouse_event.LeftDown()) {
        const Vec3d u_tip_world = trafo_matrix * (handle_center_local + u_axis * arrow_length).cast<double>();
        const Vec3d v_tip_world = trafo_matrix * (handle_center_local + v_axis * arrow_length).cast<double>();
        const Point u_tip_screen = CameraUtils::project(camera, u_tip_world);
        const Point v_tip_screen = CameraUtils::project(camera, v_tip_world);
        const Vec2d u_tip_screen_d(double(u_tip_screen.x()), double(u_tip_screen.y()));
        const Vec2d v_tip_screen_d(double(v_tip_screen.x()), double(v_tip_screen.y()));

        // Panel screen-space "radius", approximated from one corner (a loose circle around the
        // square is close enough for hit-testing purposes).
        const Vec3d panel_corner_world = trafo_matrix * (handle_center_local + (u_axis + v_axis) * panel_half).cast<double>();
        const Point panel_corner_screen = CameraUtils::project(camera, panel_corner_world);
        const double panel_screen_radius = (Vec2d(double(panel_corner_screen.x()), double(panel_corner_screen.y())) - handle_screen_d).norm();

        constexpr double pick_tolerance_px = 8.0;
        const double dist_to_u = dist_to_segment_px(mouse_pos, handle_screen_d, u_tip_screen_d);
        const double dist_to_v = dist_to_segment_px(mouse_pos, handle_screen_d, v_tip_screen_d);
        const double dist_to_panel = (handle_screen_d - mouse_pos).norm();

        // Arrows take priority over the panel (their tips extend past it), then the panel covers
        // the broader central area.
        if (dist_to_u <= pick_tolerance_px && dist_to_u <= dist_to_v)
            m_adjust_drag_handle = AdjustHandle::AxisU;
        else if (dist_to_v <= pick_tolerance_px)
            m_adjust_drag_handle = AdjustHandle::AxisV;
        else if (dist_to_panel <= panel_screen_radius)
            m_adjust_drag_handle = AdjustHandle::Pan;
        else {
            m_adjust_drag_handle = AdjustHandle::None;
            return false;
        }

        m_adjust_drag_start_offset = layer->offset;

        Vec3d world_hit;
        if (ray_plane_hit(camera, mouse_pos, trafo_matrix, drag_plane_local, m_adjust_anchor_normal, world_hit)) {
            const Vec3f local_hit = (trafo_matrix.inverse() * world_hit).cast<float>();
            m_adjust_drag_start_planar = project_planar(local_hit, m_adjust_anchor_normal);
        }
        return true;
    }

    if (mouse_event.Dragging() && m_adjust_drag_handle != AdjustHandle::None) {
        Vec3d world_hit;
        if (!ray_plane_hit(camera, mouse_pos, trafo_matrix, drag_plane_local, m_adjust_anchor_normal, world_hit))
            return true;
        const Vec3f local_hit      = (trafo_matrix.inverse() * world_hit).cast<float>();
        const Vec2f current_planar = project_planar(local_hit, m_adjust_anchor_normal);

        Vec2f delta_planar = current_planar - m_adjust_drag_start_planar;
        // project_planar()'s (x, y) axes are exactly u_axis/v_axis (see adjust_tangent_basis()),
        // so zeroing one component constrains the drag to only the other axis.
        if (m_adjust_drag_handle == AdjustHandle::AxisU)
            delta_planar.y() = 0.f;
        else if (m_adjust_drag_handle == AdjustHandle::AxisV)
            delta_planar.x() = 0.f;

        const float scale        = (layer->tiling_scale > 1e-6f) ? (1.f / layer->tiling_scale) : 1.f;
        const Vec2f delta_scaled = delta_planar * scale;
        const float rad          = layer->rotation_deg * float(M_PI) / 180.f;
        const float cs = std::cos(rad), sn = std::sin(rad);
        Vec2f delta_rotated(delta_scaled.x() * cs - delta_scaled.y() * sn, delta_scaled.x() * sn + delta_scaled.y() * cs);
        // ...and the same v scaling apply_uv_transform() applies for a non-square texture, so the
        // handle keeps tracking the cursor exactly instead of drifting on the v axis.
        delta_rotated.y() *= layer_texture_aspect(*layer);
        // Increasing `offset` shifts which texel is sampled at a fixed world position, which
        // visually slides the pattern the *opposite* way - subtracting is this session's
        // best-effort reasoning about the direction that feels like "dragging the texture",
        // unverified against an actual render (see header comment).
        layer->offset = m_adjust_drag_start_offset - delta_rotated;

        m_preview_params_dirty = true;
        m_parent.set_as_dirty();
        return true;
    }

    if (mouse_event.LeftUp() && m_adjust_drag_handle != AdjustHandle::None) {
        m_adjust_drag_handle = AdjustHandle::None;
        rebuild_preview();
        m_preview_params_dirty = false;
        return true;
    }

    return false;
}

ModelVolume* GLGizmoTextureDisplacement::texture_volume()
{
    ModelObject *mo = m_c->selection_info()->model_object();
    if (!mo)
        return nullptr;
    for (ModelVolume *mv : mo->volumes)
        if (mv->is_model_part())
            return mv;
    return nullptr;
}

const ModelVolume* GLGizmoTextureDisplacement::texture_volume() const
{
    return const_cast<GLGizmoTextureDisplacement *>(this)->texture_volume();
}

std::vector<ModelVolume *> GLGizmoTextureDisplacement::texture_parts()
{
    std::vector<ModelVolume *> out;
    ModelObject               *mo = m_c->selection_info()->model_object();
    if (mo == nullptr)
        return out;
    for (ModelVolume *mv : mo->volumes)
        if (mv->is_model_part())
            out.push_back(mv);
    return out;
}

std::vector<ModelVolume *> GLGizmoTextureDisplacement::painted_parts()
{
    std::vector<ModelVolume *> out;
    for (ModelVolume *mv : texture_parts())
        if (mv->is_texture_displacement_painted())
            out.push_back(mv);
    return out;
}

bool GLGizmoTextureDisplacement::any_part_painted() const
{
    for (const ModelVolume *mv : const_cast<GLGizmoTextureDisplacement *>(this)->texture_parts())
        if (mv->is_texture_displacement_painted())
            return true;
    return false;
}

std::vector<TextureDisplacementLayer> GLGizmoTextureDisplacement::layers_for_part(const ModelVolume &part) const
{
    const ModelVolume *owner = texture_volume();
    if (owner == nullptr)
        return {};
    std::vector<TextureDisplacementLayer> out = owner->texture_displacement_layers;
    if (&part == owner)
        return out;
    for (TextureDisplacementLayer &layer : out) {
        layer.lscm_seam_edges.clear();
        layer.islands.clear();
        layer.island_groups.clear();
        layer.lscm_uv_overrides.clear();
    }
    return out;
}

Vec3d GLGizmoTextureDisplacement::texture_anchor() const
{
    const ModelVolume *owner = texture_volume();
    return owner != nullptr ? Vec3d(texture_displacement_volume_to_world(*owner).translation()) : Vec3d::Zero();
}

std::vector<float> GLGizmoTextureDisplacement::resting_drops(const std::vector<ModelVolume *> &parts)
{
    const auto bottom = [](const ModelVolume &part) {
        return part.mesh().transformed_bounding_box(texture_displacement_volume_to_world(part)).min.z();
    };
    // Each part's bottom is measured the same way as the object's, so the lowest part's drop is exactly
    // 0 and a single-part object bakes against its own bottom as it always has.
    double object_bottom = std::numeric_limits<double>::max();
    for (const ModelVolume *part : texture_parts())
        object_bottom = std::min(object_bottom, bottom(*part));
    std::vector<float> out;
    out.reserve(parts.size());
    for (const ModelVolume *part : parts)
        out.push_back(float(bottom(*part) - object_bottom));
    return out;
}

Transform3d GLGizmoTextureDisplacement::prepare_texture_frame(const ModelVolume &part)
{
    if (texture_parts().size() < 2)
        return Transform3d::Identity();
    return texture_displacement_bake_frame(texture_displacement_volume_to_world(part), texture_anchor());
}

void GLGizmoTextureDisplacement::update_model_object()
{
    store_panel_state();
    bool         updated = false;
    ModelObject *mo      = m_c->selection_info()->model_object();
    int          idx     = -1;
    for (ModelVolume *mv : mo->volumes) {
        if (!mv->is_model_part())
            continue;
        ++idx;
        FacetsAnnotation &facet = mv->texture_displacement_facet(m_active_layer_slot);
        // See m_selectors_stale: the mask could not be loaded into this selector, so an empty selector
        // here is "failed to load", not "nothing painted", and writing it back would erase the paint.
        // A selector that does hold something is the user's own work and must be flushed as usual.
        if (m_selectors_stale && !facet.empty() && m_triangle_selectors[idx]->serialize().triangles_to_split.empty())
            continue;
        updated |= facet.set(*m_triangle_selectors[idx]);
    }

    // The fast (shaded) preview reads the live selector, so it has to be rebuilt after any stroke that
    // flushes here - not only when set() reports a change. Rebuilding it via rebuild_preview() below
    // is gated on `updated`, which misses e.g. the first paint into a slot; marking it dirty makes the
    // render loop (render_painter_gizmo) rebuild it next frame regardless. Without this, fast preview -
    // now the default view - stayed blank until a full reload (select-whole-model / reopen).
    m_shaded_preview_dirty = true;

    if (updated) {
        const ModelObjectPtrs &mos = wxGetApp().model().objects;
        wxGetApp().obj_list()->update_info_items(std::find(mos.begin(), mos.end(), mo) - mos.begin());
        m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
        rebuild_preview();
    }
}

void GLGizmoTextureDisplacement::update_from_model_object(bool first_update)
{
    wxBusyCursor wait;

    // Opening the gizmo puts the panel back where it was left: the layer and the mode come off the
    // volume, because they are the user's place in this model's stack, and the dock state off the
    // application config, because it is about their window. Only on open - this also runs on an
    // undo/redo and on a layer switch, where overriding what the user is doing now would be wrong.
    if (first_update) {
        if (const ModelVolume *mv = texture_volume(); mv != nullptr) {
            const TextureDisplacementOptions &opts = mv->texture_displacement_options;
            m_panel_mode = std::clamp(opts.panel_mode, 0, 1);
            // Only if that slot still holds a layer: the stack can have been edited by anything that
            // does not go through the panel, and a selection pointing at nothing shows an empty panel.
            const auto &stack = mv->texture_displacement_layers;
            if (std::any_of(stack.begin(), stack.end(),
                            [&opts](const TextureDisplacementLayer &l) { return l.slot == opts.active_slot; }))
                m_active_layer_slot = opts.active_slot;
        }
        m_undocked = wxGetApp().app_config->get_bool(UNDOCKED_CONFIG_KEY);
    }

    const ModelObject *mo = m_c->selection_info()->model_object();
    m_triangle_selectors.clear();
    // The base keeps the last mesh a fill tool hovered, and render_painter_gizmo() reads it as a hover that is
    // still on: a new set of selectors has none.
    m_seed_fill_last_mesh_id = -1;

    std::vector<ColorRGBA> ebt_colors;
    ebt_colors.push_back(GLVolume::NEUTRAL_COLOR);
    ebt_colors.push_back(TriangleSelectorGUI::enforcers_color);
    ebt_colors.push_back(TriangleSelectorGUI::blockers_color);
    m_selectors_stale = false;
    for (const ModelVolume *mv : mo->volumes) {
        if (!mv->is_model_part())
            continue;

        const TriangleMesh                            *mesh = &mv->mesh();
        const TriangleSelector::TriangleSplittingData &data =
            mv->texture_displacement_facet(m_active_layer_slot).get_data();
        // The same bound TriangleSelector::deserialize() checks before it gives up - silently, with a
        // void return and no way to report it. A mask recorded before the mesh was replaced indexes
        // triangles that no longer exist, and the selector then comes back empty even though the mask
        // is not. That has to be caught here, because the next update_model_object() would otherwise
        // write the empty selector back over the mask: the paint would vanish, and the bake would
        // report "nothing is painted" about the very data the flush had just deleted.
        const size_t facet_count = mesh->its.indices.size();
        for (const TriangleSelector::TriangleBitStreamMapping &m : data.triangles_to_split)
            if (m.triangle_idx < 0 || size_t(m.triangle_idx) >= facet_count) {
                m_selectors_stale = true;
                break;
            }
        m_triangle_selectors.emplace_back(std::make_unique<TriangleSelectorPatch>(*mesh, ebt_colors));
        // The mask uses a single state (ENFORCER): a higher state in the file is reset to NONE.
        m_triangle_selectors.back()->deserialize(data, false, EnforcerBlockerType::ENFORCER);
        m_triangle_selectors.back()->request_update_render_data();
    }

    // A never-textured volume opens on the mode picker (render_mode_picker()), which adds the first
    // layer of the kind chosen there.
    rebuild_preview();
}

void GLGizmoTextureDisplacement::store_panel_state()
{
    if (ModelVolume *mv = texture_volume(); mv != nullptr) {
        mv->texture_displacement_options.active_slot = m_active_layer_slot;
        mv->texture_displacement_options.panel_mode  = m_panel_mode;
    }
}

void GLGizmoTextureDisplacement::set_active_layer(int slot)
{
    if (slot == m_active_layer_slot)
        return;
    // Flush edits made while the previous layer was active before switching what the selectors
    // reflect - otherwise they would be silently lost.
    update_model_object();
    m_active_layer_slot = slot;
    store_panel_state();
    update_from_model_object(false);
    // The on-canvas gizmo (if on) is anchored to whichever layer is active - keep it in sync
    // instead of leaving it pointing at the previous layer's (now stale) paint patch.
    if (m_adjust_texture_mode)
        update_adjust_anchor();
    // Refresh every preview/overlay (shaded, UV editor, seams, ...) for the newly active layer.
    rebuild_preview();
}

void GLGizmoTextureDisplacement::ensure_panel_icons()
{
    if (m_panel_icons_tried)
        return;
    m_panel_icons_tried = true;

    // Order is irrelevant; the map keys by file name. Loaded with color_wite_gray so each icon has both
    // a monochrome ("normal", grey in the current theme) and an original-colour variant.
    static const std::vector<std::string> names = {
        "texture_displacement_brush.svg", "texture_displacement_face.svg", "texture_displacement_connected_area.svg",
        "texture_displacement_real_preview.svg", "texture_displacement_fast_preview.svg",
        "texture_displacement_checker.svg", "texture_displacement_distortion.svg",
        "texture_displacement_wireframe.svg", "texture_displacement_cross.svg",
        "texture_displacement_uv_select_island.svg", "texture_displacement_uv_select_edge.svg",
        "cog.svg", // the layer card's "adjust image" button
        "texture_displacement_uv_select_vertex.svg",
        // Paint / Erase, brush cursor shape, mapping, tiling, placement and the dock toggle.
        "texture_displacement_add.svg", "texture_displacement_add_negative.svg", "circle_paint.svg",
        "menu_obj_sphere.svg", "menu_obj_cube.svg", "menu_obj_cylinder.svg", "texture_displacement_map_unwrap.svg",
        "texture_displacement_map_view.svg", "texture_displacement_map_model_uv.svg",
        "texture_displacement_tile_repeat.svg", "menu_mirror_x.svg",
        "texture_displacement_adjust.svg", "canvas_drag.svg", "texture_displacement_move_up.svg",
        "texture_displacement_move_down.svg", "texture_displacement_drag.svg",
        "texture_displacement_select_all.svg", "texture_displacement_erase_all.svg",
        // The "type a value instead" icon at the right end of every slider.
        "edit.svg",
    };
    std::vector<std::string> paths;
    paths.reserve(names.size());
    for (const std::string &n : names)
        paths.push_back(resources_dir() + "/images/" + n);

    // Runs from the panel render, i.e. with a GL context current, so the upload is safe here.
    //
    // Rasterized at twice GLToolbar::Default_Icons_Size rather than at it: these are drawn at the
    // toolbar's icon size, which is that constant scaled by DPI (toolbar_icon_scale() folds in
    // em_unit), so on a 200% display the draw size reaches 80 px. Rasterizing at 40 would upscale a
    // 40 px bitmap there, which is what actually reads as blurry - downscaling does not. The icon set
    // is rasterized once for the gizmo's lifetime, so it cannot re-raster on a DPI change; sizing for
    // the larger case and letting ImGui shrink it is the version that looks right on both.
    const std::vector<IconManager::Icons> icons =
        m_panel_icons.init(paths, ImVec2(2 * GLToolbar::Default_Icons_Size, 2 * GLToolbar::Default_Icons_Size),
                           IconManager::RasterType::color_wite_gray);
    for (size_t i = 0; i < names.size() && i < icons.size(); ++i)
        m_panel_icon_map[names[i]] = icons[i];
}

// Seconds on a real clock, for the tooltips' own timing.
//
// Not ImGui::GetTime(): ImGuiWrapper::new_frame() feeds io.DeltaTime the real frame time but caps it
// at 1/60 s, so the idle gaps of the event-driven canvas don't count as time a +/- button is held.
// That cap also means ImGui's clock stalls while the canvas is idle - a pointer resting still on a
// control renders nothing, so the tooltip delay would never run out on ImGui's clock. A real clock
// keeps the tooltip timing and media playback independent of how often the canvas renders.
static double tip_now()
{
    using clock = std::chrono::steady_clock;
    static const clock::time_point base = clock::now();
    return std::chrono::duration<double>(clock::now() - base).count();
}

// How long the pointer has to rest on a control before its tooltip appears. The panel is dense and the
// tooltip lands on the model beside it, so with no wait at all a sweep from the top of the panel down to
// Bake flashes a dozen large panels over the scene. Once one has just been up the wait drops to almost
// nothing, so reading several settings in a row stays quick - but not to nothing, or that same sweep
// would flash again the moment one tooltip had been seen.
static constexpr double TIP_DELAY       = 0.45;
static constexpr double TIP_DELAY_AGAIN = 0.10;
static constexpr double TIP_STICKY      = 0.70;

void GLGizmoTextureDisplacement::panel_tooltip(const wxString &text, const std::vector<TipMedia> &media)
{
    panel_tooltip(into_u8(text), media);
}

void GLGizmoTextureDisplacement::panel_tooltip(const std::string &text, const std::vector<TipMedia> &media)
{
    // Nothing explains a control while the control is being used: dragging a slider, typing a value
    // into one, picking from a combo, dragging a layer card. The pointer sits on the very thing being
    // edited, so without this the tooltip covers the model exactly while the user is watching it change.
    // The full wait comes back afterwards rather than the short one - having just let go of a slider is
    // not a reason to be shown a picture of it.
    if (ImGui::IsAnyItemActive() || ImGui::IsAnyMouseDown()) {
        m_tip_item       = ImVec2(-1.f, -1.f);
        m_tip_last_shown = -1e9;
        return;
    }

    // The item's position stands in for its identity: moving to another control restarts the wait, and
    // staying on this one carries it on.
    const double now  = tip_now();
    const ImVec2 item = ImGui::GetItemRectMin();
    if (item.x != m_tip_item.x || item.y != m_tip_item.y) {
        m_tip_item        = item;
        m_tip_hover_since = now;
    }
    if (now - m_tip_hover_since < (now - m_tip_last_shown < TIP_STICKY ? TIP_DELAY_AGAIN : TIP_DELAY)) {
        // A pointer resting on a control produces no events at all, so unless the wait asks for frames
        // itself nothing redraws the canvas and the wait never finishes - the tooltip then appears only
        // on the next pixel of mouse movement.
        //
        // request_extra_frame() and not schedule_extra_frame(): the latter only arranges a timer, whose
        // event an idle event is assumed to follow, and GLCanvas3D::on_idle() renders nothing unless the
        // canvas was already dirty when it ran. request_extra_frame() ends on_idle with
        // evt.RequestMore(), which obliges wx to deliver the next one. It is what 3DBed's fade uses.
        // Costing a few hundred milliseconds of frames is the cheaper half of that trade.
        m_parent.request_extra_frame();
        return;
    }
    m_tip_last_shown = now;

    // ImGui hides a window on the first frame it is submitted, to measure it before showing it - and
    // for a tooltip it does so every time one reappears, because the windows are recycled (see the
    // Popup|Tooltip branch of Begin()). So the frame on which a tooltip is decided is never the frame
    // on which it is seen, and one that then asks for nothing more stays invisible until the next pixel
    // of mouse movement happens to produce another frame. Keep frames coming until it has settled.
    const int shown_frame = ImGui::GetFrameCount();
    m_tip_settled         = (shown_frame == m_tip_shown_frame + 1) ? m_tip_settled + 1 : 0;
    m_tip_shown_frame     = shown_frame;
    if (m_tip_settled < 2)
        m_parent.request_extra_frame();

    const ImGuiStyle &style = ImGui::GetStyle();
    const float       gap   = m_imgui->scaled(0.5f);
    const float       wrap  = m_imgui->scaled(20.f);
    // The width has to be known here, not left to ImGui, because the tooltip is placed by its left edge
    // (below) and an illustration has to be laid out against something. One carrying illustrations takes
    // the full wrap width; a plain one takes exactly what its text needs, as it did when ImGui sized it,
    // so a two-word tooltip is not a wide box with two words in it.
    const float text_w = ImGui::CalcTextSize(text.c_str(), nullptr, false, wrap).x;
    const float width  = (media.empty() ? std::min(wrap, std::ceil(text_w)) : wrap) + 2.f * style.WindowPadding.x;

    // Beside the anchor (the panel, or the texture picker when the tooltip comes from inside it), never
    // over it - the same reasoning, and the same placement, as the picker popup itself. ImGui normally
    // keeps a tooltip glued to the mouse; it honours a position set here instead (see the Orca branch in
    // imgui's Begin()), and still clamps the result inside the canvas.
    const bool  left = m_tooltip_anchor_min.x - gap - width >= 0.f;
    const float x    = left ? m_tooltip_anchor_min.x - gap - width : m_tooltip_anchor_max.x + gap;
    ImGui::SetNextWindowPos(ImVec2(x, ImGui::GetItemRectMin().y)); // level with the control it explains
    ImGui::SetNextWindowSize(ImVec2(width, 0.f));                  // height still follows the content

    ImGui::BeginTooltip();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 1.00f, 1.00f, 1.00f));
    ImGui::PushTextWrapPos(0.f); // the window's own edge, which is what the fixed width above sets
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    render_tip_media(media, width - 2.f * style.WindowPadding.x);
    ImGui::EndTooltip();
}

void GLGizmoTextureDisplacement::render_tip_media(const std::vector<TipMedia> &media, float width)
{
    // Collect what is actually there first: the layout divides the width between the illustrations, so
    // one that is missing has to drop out of the count rather than leave a gap.
    std::vector<std::pair<const AnimatedTexture *, const TipMedia *>> shown;
    std::string                                                       key;
    for (const TipMedia &m : media) {
        const AnimatedTexture *texture =
            m_tip_media.get((boost::filesystem::path(resources_dir()) / "images" / "texture_displacement" / m.file).string());
        if (texture != nullptr && texture->is_valid() && texture->width() > 0 && texture->height() > 0) {
            shown.emplace_back(texture, &m);
            key += m.file + "|";
        }
    }
    if (shown.empty())
        return;

    // Each animation plays from its first frame when its tooltip opens: these are short loops showing
    // one setting going from one value to another, and joining one halfway through shows the user the
    // end of a change they never saw the start of.
    // Restart on a different tooltip, and on the same one opening again - which is what a gap in the
    // frames it was drawn on means, since it is drawn on every frame for as long as it is on screen.
    const int this_frame = ImGui::GetFrameCount();
    if (key != m_tip_media_key || this_frame != m_tip_media_frame + 1) {
        m_tip_media_key   = key;
        m_tip_media_start = tip_now();
    }
    m_tip_media_frame = this_frame;
    const double time = tip_now() - m_tip_media_start;

    const ImGuiStyle &style   = ImGui::GetStyle();
    const float       spacing = style.ItemSpacing.x;
    const float       cell_w  = (width - spacing * float(shown.size() - 1)) / float(shown.size());
    const ImU32       frame   = ImGui::GetColorU32(ImGuiCol_Text, 0.25f);

    // A single illustration would otherwise be drawn at the tooltip's full width, and a square one at
    // full width is a picture as tall as the text beside it is long. Capped and centred in its column.
    const float max_h = m_imgui->scaled(10.f);

    ImGui::Spacing();
    for (size_t i = 0; i < shown.size(); ++i) {
        const AnimatedTexture *texture = shown[i].first;
        const TipMedia        &entry   = *shown[i].second;
        const float            cell_x  = ImGui::GetCursorPosX();

        float w = cell_w;
        float h = std::round(cell_w * float(texture->height()) / float(texture->width()));
        if (h > max_h) {
            w = std::round(max_h * float(texture->width()) / float(texture->height()));
            h = max_h;
        }

        ImGui::BeginGroup();
        const unsigned int id = texture->texture_id(time);
        ImGui::SetCursorPosX(cell_x + std::round(0.5f * (cell_w - w)));
        ImGui::Image((ImTextureID) (intptr_t) id, ImVec2(w, h));
        ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), frame, style.FrameRounding);
        // The canvas only redraws on demand, so an animation has to ask for its own frames. It asks for
        // them as fast as they come rather than pacing itself with a timer: which frame is drawn is
        // decided by the clock above, so a redraw that arrives early simply draws the same frame again,
        // and a guaranteed frame is worth more here than a frugal one that may never arrive.
        if (texture->is_animated())
            m_parent.request_extra_frame();

        if (!entry.caption.empty()) {
            // Centred under its own illustration, and never wider than its column.
            const std::string caption = into_u8(entry.caption);
            const float       text_w  = ImGui::CalcTextSize(caption.c_str()).x;
            ImGui::SetCursorPosX(cell_x + std::max(0.f, (cell_w - text_w) * 0.5f));
            ImGui::PushTextWrapPos(cell_x + cell_w);
            ImGui::TextDisabled("%s", caption.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::EndGroup();

        if (i + 1 < shown.size())
            ImGui::SameLine(0.f, spacing);
    }
}

void GLGizmoTextureDisplacement::remove_texture_layer(int slot)
{
    ModelVolume *mv = texture_volume();
    if (!mv)
        return;

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Remove texture displacement layer"), UndoRedo::SnapshotType::GizmoAction);

    auto &layers = mv->texture_displacement_layers;
    layers.erase(std::remove_if(layers.begin(), layers.end(),
                                 [slot](const TextureDisplacementLayer &l) { return l.slot == slot; }),
                 layers.end());
    if (slot >= 0 && slot < int(TEXTURE_DISPLACEMENT_MAX_LAYERS)) {
        // Every model part: paint left in the slot on another part would still bake, and the next layer
        // added reuses the slot.
        for (ModelVolume *part : texture_parts())
            part->texture_displacement_facet(slot).reset();
        m_thumbnails[size_t(slot)].reset();
        m_thumbnail_source[size_t(slot)].reset();
        m_layer_expanded[size_t(slot)] = false;
    }
    if (m_picker_slot == slot)
        m_picker_slot = -1;

    if (m_active_layer_slot == slot)
        update_from_model_object(false); // also rebuilds the preview
    else
        rebuild_preview(); // a non-active layer's contribution to the combined preview changed

    m_parent.set_as_dirty();
}

void GLGizmoTextureDisplacement::swap_layer_slots(int slot_a, int slot_b)
{
    ModelObject *mo = m_c->selection_info()->model_object();
    ModelVolume *mv = texture_volume();
    const int    n  = int(TEXTURE_DISPLACEMENT_MAX_LAYERS);
    if (mo == nullptr || mv == nullptr || slot_a == slot_b || slot_a < 0 || slot_b < 0 || slot_a >= n || slot_b >= n)
        return;

    // Every model part, not only the texture volume: the selectors paint the active slot on all of them.
    for (ModelVolume *v : mo->volumes) {
        if (!v->is_model_part())
            continue;
        TriangleSelector::TriangleSplittingData data_a = v->texture_displacement_facet(slot_a).get_data();
        TriangleSelector::TriangleSplittingData data_b = v->texture_displacement_facet(slot_b).get_data();
        v->texture_displacement_facet(slot_a).set_data(std::move(data_b));
        v->texture_displacement_facet(slot_b).set_data(std::move(data_a));
    }
    for (TextureDisplacementLayer &l : mv->texture_displacement_layers) {
        if (l.slot == slot_a)
            l.slot = slot_b;
        else if (l.slot == slot_b)
            l.slot = slot_a;
    }

    const size_t a = size_t(slot_a), b = size_t(slot_b);
    std::swap(m_thumbnails[a], m_thumbnails[b]);
    std::swap(m_thumbnail_source[a], m_thumbnail_source[b]);
    std::swap(m_thumbnail_smoothing[a], m_thumbnail_smoothing[b]);
    std::swap(m_layer_expanded[a], m_layer_expanded[b]);
    if (m_active_layer_slot == slot_a)
        m_active_layer_slot = slot_b;
    else if (m_active_layer_slot == slot_b)
        m_active_layer_slot = slot_a;
    if (m_picker_slot == slot_a)
        m_picker_slot = slot_b;
    else if (m_picker_slot == slot_b)
        m_picker_slot = slot_a;
}

void GLGizmoTextureDisplacement::move_texture_layer(int slot, int to_index)
{
    ModelVolume *mv = texture_volume();
    if (mv == nullptr)
        return;

    std::vector<int> slots;
    for (const TextureDisplacementLayer &l : mv->texture_displacement_layers)
        slots.push_back(l.slot);
    std::sort(slots.begin(), slots.end());
    const auto it = std::find(slots.begin(), slots.end(), slot);
    if (it == slots.end())
        return;
    const int from = int(it - slots.begin());
    // An insertion index counts the moved layer's own position, which is gone once it is lifted out.
    const int to = std::clamp(to_index > from ? to_index - 1 : to_index, 0, int(slots.size()) - 1);
    if (to == from)
        return;

    update_model_object(); // flush the active layer's strokes into its slot before slots are exchanged
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Reorder texture displacement layers"),
                                  UndoRedo::SnapshotType::GizmoAction);
    // Walked one neighbour at a time, so every layer in between shifts by one and keeps its order.
    const int step = to > from ? 1 : -1;
    for (int i = from; i != to; i += step)
        swap_layer_slots(slots[size_t(i)], slots[size_t(i + step)]);

    // The active layer's mask now sits in a different slot, so the selectors are reloaded from there.
    update_from_model_object(false); // also rebuilds the preview
    if (m_adjust_texture_mode)
        update_adjust_anchor();
    m_parent.set_as_dirty();
}

void GLGizmoTextureDisplacement::bake(bool own_snapshot)
{
    ModelVolume *owner = texture_volume();
    if (owner == nullptr || m_bake_in_progress)
        return;

    // Make sure the currently active layer's in-progress edits are flushed into the model before
    // baking, otherwise the most recent, not-yet-committed strokes would be silently skipped.
    update_model_object();

    // Every painted part, not only the one holding the layer stack: the brush paints across all of
    // them (see update_model_object()), so baking one would leave the user's paint on the others
    // untouched and their geometry flat.
    const std::vector<ModelVolume *> parts = painted_parts();
    if (parts.empty()) {
        show_error(nullptr, _u8L("Nothing is painted, there is nothing to bake."));
        return;
    }

    // A ChromaStack plate has two things it can be baked into, and the card says which (see
    // render_chroma_plate). A flat tile is not a displacement at all - it is one part per filament -
    // so it does not go through the bake below; it replaces the object instead.
    if (m_chroma_flat_mode != 3)
        for (const TextureDisplacementLayer &layer : owner->texture_displacement_layers)
            if (layer.chroma_enabled && layer.chroma.plate.generated && layer.chroma.relief_layers() > 0) {
                const int flatten_slot = layer.slot;
                wxGetApp().CallAfter([this, flatten_slot]() { chroma_make_flat_tile(flatten_slot); });
                return;
            }

    // The undo step for the whole operation, taken here because everything below this line changes the
    // model: the full-resolution re-solve replaces each picture's height map, the base is pinned just
    // after it, and the job replaces the meshes when it lands. The job used to take the snapshot when
    // it committed, which is after all of that, so the state it recorded already had the new height map
    // and a pinned base - an undo gave back the unbaked mesh with the base no longer following the
    // model, and from there every colour in the picture moved with the next scale or layer-height
    // change for no reason the user could see.
    //
    // Not before update_model_object() above: flushing the active layer's strokes into the model is the
    // user's paint arriving rather than part of what Bake does, and an undo of Bake should keep it.
    //
    // Scoped to this statement on purpose. TakeSnapshot suppresses further snapshots for its lifetime,
    // and the job below settles on the event loop long after this function has returned, so an object
    // held open would suppress every snapshot taken in between. `own_snapshot` is false when the caller
    // has already taken one meant to cover the displacement too - Standard mode's Bake, which remeshes
    // and subdivides first and has to undo as a single action.
    if (own_snapshot) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Bake texture displacement"),
                                      UndoRedo::SnapshotType::GizmoAction);
    }

    // The live path solves at CHROMA_SOLVE_PREVIEW_PX so the panel keeps up, which is far finer than
    // the mesh can carry but is not the picture itself. A bake is what gets printed, so it gets the
    // real thing - once, here, where a second or two is lost in an operation that already takes
    // longer than that.
    for (TextureDisplacementLayer &layer : owner->texture_displacement_layers)
        if (layer.chroma_enabled && layer.slot >= 0 && size_t(layer.slot) < TEXTURE_DISPLACEMENT_MAX_LAYERS &&
            m_chroma_solve_coarse[size_t(layer.slot)])
            chroma_solve_layer_blocking(layer);

    // The base stops following the model here, for every picture about to be baked.
    //
    // It follows the painted face, and a bake is the one moment that face stops being a face and
    // becomes the relief itself. Measured again afterwards, the patch is as tall as the whole stack,
    // so the base comes back as the base plus the relief - and the printed colours are read from the
    // height above the base, so every point of the picture then reads at or below it and the whole
    // plate turns the colour of the first filament. That is the grey plate with a few bright patches
    // on it, and it is this line that was missing rather than anything in the shader.
    //
    // Pinning the measurement rather than discarding it: the number was right when it was taken, and
    // it is the number this bake was solved against.
    for (TextureDisplacementLayer &layer : owner->texture_displacement_layers)
        if (layer.chroma_enabled && layer.chroma.base_auto) {
            layer.chroma.base_auto = false;
            if (const int measured = chroma_measured_base(layer); measured > 0)
                layer.chroma.base_layers = measured;
        }

    const Vec3d                                anchor = texture_anchor();
    const std::vector<float>                   drops  = resting_drops(parts);
    std::vector<TextureDisplacementBakeVolume> volumes;
    volumes.reserve(parts.size());
    for (size_t i = 0; i < parts.size(); ++i)
        volumes.push_back(texture_displacement_bake_volume(*parts[i], layers_for_part(*parts[i]),
                                                           owner->texture_displacement_options, anchor, drops[i]));

    m_bake_in_progress = true;
    queue_texture_displacement_bake(std::move(volumes), color_settings_for(*owner), [this]() {
        m_bake_in_progress = false;
        // Baking replaces each baked part's mesh (new id, new topology) without changing the object's
        // id or volume count, so GLGizmoPainterBase::data_changed()'s usual change-detection never
        // notices it needs to reload - do it explicitly here, otherwise the gizmo keeps painting
        // and rendering against the stale, pre-bake TriangleSelectorPatch until the object is
        // deselected and reselected.
        if (m_state == On && m_c->selection_info() && m_c->selection_info()->model_object())
            update_from_model_object(false);
        m_parent.set_as_dirty();
    });
}

// Standard mode's fixed recipe. These are both the values the hidden controls are pinned to and what
// bake_standard() drives its remesh and subdivision with - one set of numbers, so the preview cannot
// disagree with the bake. Chosen to be safe on an arbitrary imported part rather than optimal on any
// particular one: a 1 mm isotropic remesh gives the subdivider an even starting density whatever the
// input looked like, and the subdivision then spends up to 1.5 M triangles chasing texture curvature
// down to a 0.02 mm floor, which is finer than any FDM nozzle will resolve. The triangle budget is not
// here: it is the one control Standard mode still shows, so it belongs to the user (its default is
// m_subdivide_budget_k's initialiser).
static constexpr float STD_REMESH_EDGE_MM     = 1.0f;
static constexpr float STD_REMESH_SHARP_DEG   = 40.f;
static constexpr float STD_SUBDIV_MAX_EDGE_MM = 20.f;
static constexpr float STD_SUBDIV_DETAIL_MM   = 0.02f;
// 0.1 mm rather than the 0.02 it was: a hard step in the texture never satisfies the chord tolerance
// however finely it is split, so the floor is what stops it, and 0.02 mm carpeted every step edge with
// triangles five times finer than a nozzle can print - most of the budget, on almost every texture.
static constexpr float STD_SUBDIV_MIN_EDGE_MM = 0.1f;
// Edge length the band straddling the paint's edge is refined to. This is the one number that decides
// how clean the rim of an unpainted island looks: the bake steps the surface from full displacement to
// zero across that band, and nothing else in the criteria can see the step (see collect_paint_region()).
static constexpr float STD_SUBDIV_BORDER_MM   = 0.4f;
// Edge length a colour boundary is refined to. Finer than the border band because a colour edge is
// what the eye actually lands on - a stepped outline around a printed decal reads as a defect in a
// way a slightly coarse relief transition does not - and it costs triangles along an outline only.
static constexpr float STD_SUBDIV_COLOR_MM    = 0.25f;

bool GLGizmoTextureDisplacement::apply_standard_mode_presets(ModelVolume *mv)
{
    bool       changed = false;
    const auto pin     = [&changed](auto &field, auto value) {
        if (field != value) {
            field   = value;
            changed = true;
        }
    };

    if (mv != nullptr) {
        pin(mv->texture_displacement_options.displace_border, true);
        pin(mv->texture_displacement_options.smooth_enabled, false);
    }
    pin(m_subdivide_adaptive, true);
    pin(m_subdivide_feature, true);
    pin(m_subdivide_target_mm, STD_SUBDIV_MAX_EDGE_MM);
    pin(m_subdivide_detail_mm, STD_SUBDIV_DETAIL_MM);
    pin(m_subdivide_min_edge_mm, STD_SUBDIV_MIN_EDGE_MM);
    pin(m_subdivide_border_mm, STD_SUBDIV_BORDER_MM);
    pin(m_subdivide_color_mm, STD_SUBDIV_COLOR_MM);
    // Deliberately *not* pinned: the triangle budget stays visible and editable in Standard mode, so
    // pinning it would fight the user's own slider every frame.
    pin(m_remesh_target_edge_mm, STD_REMESH_EDGE_MM);
    pin(m_remesh_keep_sharp_edges, true);
    pin(m_remesh_sharp_angle_deg, STD_REMESH_SHARP_DEG);
    return changed;
}

void GLGizmoTextureDisplacement::bake_standard()
{
    ModelVolume *mv = texture_volume();
    if (mv == nullptr || m_bake_in_progress || m_prepare_in_progress)
        return;

    update_model_object(); // flush the active layer's in-progress strokes before anything reads them
    if (!any_part_painted()) {
        show_error(nullptr, _u8L("Nothing is painted, there is nothing to bake."));
        return;
    }
    apply_standard_mode_presets(mv); // belt and braces: never bake with values the panel is not showing

    // It refines as part of the bake, so preparing first would refine a second time at another
    // target.
    if (mv->texture_displacement_options.pipeline_v2) {
        bake();
        return;
    }

    // The whole recipe in one go. Either stage having nothing to do is normal, not a failure - a mesh
    // that is already even needs no remesh, one that is already fine enough for the texture needs no
    // subdivision - so no "nothing changed" message here: it goes straight on to the displacement.
    TextureDisplacementPrepareParams params;
    params.remesh_edge_mm         = STD_REMESH_EDGE_MM;
    params.remesh_sharp_deg       = STD_REMESH_SHARP_DEG;
    params.subdiv_target_mm       = STD_SUBDIV_MAX_EDGE_MM;
    params.subdiv_detail_mm       = STD_SUBDIV_DETAIL_MM;
    params.subdiv_min_edge_mm     = STD_SUBDIV_MIN_EDGE_MM;
    params.subdiv_border_mm       = STD_SUBDIV_BORDER_MM;
    params.subdiv_color_edge_mm   = STD_SUBDIV_COLOR_MM;
    params.subdiv_feature         = true;
    params.subdiv_added_triangles = m_subdivide_budget_k * 1000;
    params.cut_steps              = true;
    queue_prepare(params, _u8L("Bake texture displacement"), /* then_bake */ true, {});
}

// ---------------------------------------------------------------------------------------------
// Bake stage debug view
// ---------------------------------------------------------------------------------------------

void GLGizmoTextureDisplacement::run_stage_debug()
{
    ModelVolume *mv = texture_volume();
    if (mv == nullptr || m_debug_in_progress || m_bake_in_progress || m_prepare_in_progress)
        return;

    update_model_object(); // flush the active layer's in-progress strokes, as a real bake would
    if (!mv->is_texture_displacement_painted()) {
        show_error(nullptr, _u8L("Nothing is painted, so there are no bake stages to capture."));
        return;
    }

    // The default (one-run) pipeline refines as part of the bake, and Pro mode has the user prepare the
    // mesh themselves with the controls above - in both cases there is no preparation to replay, and
    // running one anyway would show stages the Bake button would not.
    const bool run_prepare = !pro_mode() && !mv->texture_displacement_options.pipeline_v2;

    TextureDisplacementPrepareParams params;
    if (run_prepare) {
        apply_standard_mode_presets(mv); // capture what the panel is showing, exactly as Bake does
        params.remesh_edge_mm         = STD_REMESH_EDGE_MM;
        params.remesh_sharp_deg       = STD_REMESH_SHARP_DEG;
        params.subdiv_target_mm       = STD_SUBDIV_MAX_EDGE_MM;
        params.subdiv_detail_mm       = STD_SUBDIV_DETAIL_MM;
        params.subdiv_min_edge_mm     = STD_SUBDIV_MIN_EDGE_MM;
        params.subdiv_border_mm       = STD_SUBDIV_BORDER_MM;
        params.subdiv_color_edge_mm   = STD_SUBDIV_COLOR_MM;
        params.subdiv_feature         = true;
        params.subdiv_added_triangles = m_subdivide_budget_k * 1000;
        params.cut_steps              = true;
    }

    // Free the previous run's meshes before the next one allocates its own: every stage is a full
    // copy of the mesh, so holding two runs at once doubles what is already the expensive part.
    exit_debug_view();

    m_debug_in_progress = true;
    queue_texture_displacement_debug(
        *mv, color_settings_for(*mv), params, prepare_texture_frame(*mv), resting_drops({mv}).front(), run_prepare,
        m_debug_check_topology,
        [this](std::vector<BakeStageSnapshot> stages) {
            m_debug_in_progress = false;
            if (!stages.empty()) {
                m_debug_stages = std::move(stages);
                // Land on the last stage: that is what a real bake would have committed, and the
                // question is usually what it looks like before stepping back to find where it went
                // wrong.
                show_debug_stage(int(m_debug_stages.size()) - 1);
            }
            m_parent.set_as_dirty();
        });
}

void GLGizmoTextureDisplacement::show_debug_stage(int index)
{
    if (index < 0 || size_t(index) >= m_debug_stages.size())
        return;
    const BakeStageSnapshot &stage = m_debug_stages[size_t(index)];
    if (stage.mesh.empty())
        return; // over the memory cap: its counts are still listed, there is just nothing to draw

    m_debug_stage = index;
    // Drawn through the ordinary true-displacement preview, so the Fast view - a shader trick over
    // the base mesh that never draws m_preview_glmodel - has to be left first.
    m_use_shaded_preview = false;
    m_preview_color_runs.clear();
    m_preview_glmodel.reset();
    // The stage is a capture of one part's run, so the other parts go back to being drawn as themselves
    // rather than keeping a live preview the captured stages say nothing about.
    m_part_previews.clear();

    indexed_triangle_set its;
    its.vertices = stage.mesh.vertices;
    its.indices  = stage.mesh.indices;
    m_preview_glmodel.init_from(its);
    m_preview_glmodel.set_color(GLVolume::NEUTRAL_COLOR);
    // Kept so the wireframe overlay draws this stage's edges rather than the base mesh's - which is
    // most of the point, since what a refinement stage did is a change in the edges, not the surface.
    m_preview_its = std::move(its);
    // A captured stage is whatever that stage was; nothing downstream reads this while one is up (the
    // chroma overlay is off entirely then), and leaving it false would claim a fault that was not tested.
    m_preview_displaced = true;
    refresh_wireframe();
    m_parent.set_as_dirty();
}

void GLGizmoTextureDisplacement::exit_debug_view()
{
    const bool was_showing = m_debug_stage >= 0;
    m_debug_stage          = -1;
    m_debug_stages.clear();
    m_debug_stages.shrink_to_fit();
    if (was_showing)
        rebuild_preview(); // hands m_preview_glmodel back to the live preview
}

void GLGizmoTextureDisplacement::render_debug_stage_panel(ModelVolume *mv)
{
    if (mv == nullptr)
        return;

    ImGui::Separator();
    if (!ImGui::CollapsingHeader(_u8L("Bake stages (debug)").c_str()))
        return;

    m_imgui->disabled_begin(m_debug_in_progress || m_bake_in_progress || m_prepare_in_progress ||
                            !mv->is_texture_displacement_painted());
    if (m_imgui->button(m_debug_in_progress ? _L("Capturing...") : _L("Capture stages")))
        run_stage_debug();
    m_imgui->disabled_end();
    if (ImGui::IsItemHovered())
        panel_tooltip(_u8L("Run the same bake the Bake button runs, keeping the mesh after every "
                           "stage, and step through them here. Nothing is committed to the model - "
                           "this is for finding which stage a bad result came from."));

    ImGui::SameLine();
    m_imgui->bbl_checkbox(_L("Check topology"), m_debug_check_topology);
    if (ImGui::IsItemHovered())
        panel_tooltip(_u8L("Count open and non-manifold edges after each stage, which is how a stage "
                           "that tore the mesh is spotted. It scans every edge, so it adds noticeably "
                           "to the capture on a fine bake."));

    if (m_debug_stages.empty())
        return;

    // Stage list. Selecting one draws it; the wireframe overlay is what makes a refinement stage
    // readable, so it is worth leaving on while stepping.
    ImGui::BeginChild("##bake_stages", ImVec2(0.f, m_imgui->scaled(9.f)), true,
                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
    for (size_t i = 0; i < m_debug_stages.size(); ++i) {
        const BakeStageSnapshot &st = m_debug_stages[i];
        char                     label[256];
        std::snprintf(label, sizeof(label), "%zu. %-18s %8zu tris %8.1f ms##stage%zu", i,
                      st.name.c_str(), st.triangles, st.ms, i);
        if (ImGui::Selectable(label, int(i) == m_debug_stage) && !st.mesh.empty())
            show_debug_stage(int(i));
    }
    ImGui::EndChild();

    // Prev / next, so stepping does not mean aiming at a list entry.
    m_imgui->disabled_begin(m_debug_stage <= 0);
    if (m_imgui->button(_L("< Previous")))
        show_debug_stage(m_debug_stage - 1);
    m_imgui->disabled_end();
    ImGui::SameLine();
    m_imgui->disabled_begin(m_debug_stage < 0 || size_t(m_debug_stage + 1) >= m_debug_stages.size());
    if (m_imgui->button(_L("Next >")))
        show_debug_stage(m_debug_stage + 1);
    m_imgui->disabled_end();
    ImGui::SameLine();
    if (m_imgui->button(_L("Close")))
        exit_debug_view();

    if (m_debug_stage >= 0 && size_t(m_debug_stage) < m_debug_stages.size()) {
        const BakeStageSnapshot &st = m_debug_stages[size_t(m_debug_stage)];
        m_imgui->text(from_u8(st.name) + ": " + std::to_string(st.triangles) + " " +
                      _u8L("triangles") + ", " + std::to_string(st.vertices) + " " + _u8L("vertices"));
        if (!st.detail.empty())
            m_imgui->text(from_u8(st.detail));
        if (st.topology_checked) {
            // Zero on both counts is the claim every stage's header makes; anything else is the bug.
            const std::string topo = _u8L("Open edges") + ": " + std::to_string(st.open_edges) + "   " +
                                     _u8L("Non-manifold") + ": " + std::to_string(st.non_manifold_edges) +
                                     "   " + _u8L("Degenerate") + ": " + std::to_string(st.degenerate);
            if (st.open_edges > 0 || st.non_manifold_edges > 0)
                m_imgui->warning_text(from_u8(topo));
            else
                m_imgui->text(topo);
        }
    }

    double total_ms = 0.0;
    for (const BakeStageSnapshot &st : m_debug_stages)
        total_ms += st.ms;
    char total[128];
    std::snprintf(total, sizeof(total), "%s: %.1f ms", _u8L("Total").c_str(), total_ms);
    m_imgui->text(total);

    if (m_imgui->button(_L("Write stage meshes"))) {
        const std::string dir =
            (boost::filesystem::temp_directory_path() / "orca-bake-stages").string();
        const size_t written = dump_bake_stages(m_debug_stages, dir);
        if (written > 0)
            show_info(nullptr, from_u8(_u8L("Wrote the stage meshes as OBJ files to:") + "\n" + dir));
        else
            show_error(nullptr, _u8L("Could not write the stage meshes."));
    }
    if (ImGui::IsItemHovered())
        panel_tooltip(_u8L("Write every captured stage out as an OBJ file, so they can be opened "
                           "side by side in a mesh viewer."));
}

void GLGizmoTextureDisplacement::queue_prepare(const TextureDisplacementPrepareParams &params,
                                               const std::string &snapshot_name, bool then_bake,
                                               const std::string &unchanged_msg, PrepareScope scope)
{
    ModelVolume *owner = texture_volume();
    if (owner == nullptr || m_prepare_in_progress || m_bake_in_progress)
        return;

    // Preparing only the layer stack's own part would hand the displacement a coarse mesh everywhere
    // else - a height map can only move vertices that already exist, so on a multibody object that is
    // the difference between relief and a flat face. See PrepareScope for which parts that means.
    const std::vector<ModelVolume *> parts = scope == PrepareScope::AllParts ? texture_parts() : painted_parts();
    if (parts.empty())
        return;

    TextureDisplacementPrepareInput input;
    input.params        = params;
    input.snapshot_name = snapshot_name;
    input.volumes.reserve(parts.size());
    for (const ModelVolume *part : parts) {
        TextureDisplacementPrepareVolume entry;
        entry.volume_id = part->id();
        entry.base_mesh = part->mesh().its;
        entry.masks     = facets_data_of(*part);
        entry.layers    = layers_for_part(*part);
        if (part->has_texture_uv())
            entry.model_uv = part->texture_uv;
        entry.texture_frame = prepare_texture_frame(*part);
        input.volumes.push_back(std::move(entry));
    }
    if (params.subdiv_color_edge_mm > 0.f)
        input.color = color_settings_for(*owner);

    m_prepare_in_progress = true;
    queue_texture_displacement_prepare(std::move(input), [this, then_bake, unchanged_msg](
                                                             TextureDisplacementPrepareOutcome outcome) {
        m_prepare_in_progress = false;
        // The commit replaced the volume's mesh (new id, new topology) without changing the object's id
        // or volume count, which is not something GLGizmoPainterBase::data_changed() can detect - so the
        // reload is explicit, exactly as it is after a bake. Done for every outcome: even a run that
        // committed nothing may have left the panel showing a stale triangle count.
        if (m_state == On && m_c->selection_info() && m_c->selection_info()->model_object())
            update_from_model_object(false);
        m_parent.set_as_dirty();

        switch (outcome) {
        case TextureDisplacementPrepareOutcome::Failed:
            return; // cancelled, or the volume went away while the job ran - say nothing, do nothing
        case TextureDisplacementPrepareOutcome::PaintLost:
            show_error(nullptr, _u8L("The painted area could not be carried onto the remeshed model, so "
                                     "nothing was changed. Switch to Pro mode and remesh before painting."));
            return;
        case TextureDisplacementPrepareOutcome::Unchanged:
            if (!unchanged_msg.empty())
                show_error(nullptr, unchanged_msg);
            break;
        case TextureDisplacementPrepareOutcome::Committed:
            break;
        }
        // ... and then the displacement itself, in the background exactly as the Pro-mode button does.
        // It commits into the snapshot the prepare opened - but only if the prepare opened one: a run
        // that found nothing to do took none, and a bake chained onto that has to push its own or it
        // would not be undoable at all.
        if (then_bake)
            bake(/* own_snapshot */ outcome == TextureDisplacementPrepareOutcome::Unchanged);
    });
}

void GLGizmoTextureDisplacement::render_paint_cursor_hint()
{
    // Only in the plain paint/select modes; seam and adjust modes have their own click semantics where
    // an add/remove sign would just be noise.
    if (m_seam_edit_mode || m_adjust_texture_mode)
        return;
    const ImGuiIO &io = ImGui::GetIO();
    // The pointer must be over the 3D view, not over this panel (or any other ImGui window).
    if (io.WantCaptureMouse || !ImGui::IsMousePosValid())
        return;

    // Shift erases (see handle_snapshot_action_name()), and so does a plain stroke in Erase mode.
    const bool  removing = io.KeyShift || m_erase_mode;
    const ImU32 color    = removing ? IM_COL32(235, 70, 60, 255) : IM_COL32(90, 210, 110, 255);
    const char *glyph    = removing ? "-" : "+";

    ImDrawList  *dl = ImGui::GetForegroundDrawList();
    const float  fs = ImGui::GetFontSize() * 1.5f;
    const ImVec2 at(io.MousePos.x + 15.f, io.MousePos.y - fs - 6.f);
    // A translucent dark disc behind the glyph so it reads on any material colour.
    dl->AddCircleFilled(ImVec2(at.x + fs * 0.28f, at.y + fs * 0.5f), fs * 0.62f, IM_COL32(0, 0, 0, 150));
    dl->AddText(ImGui::GetFont(), fs, at, color, glyph);
}

// ---- The rows the panel is built from ----
//
// The header says why these are members and not lambdas inside on_render_input_window.

// One end of a range, through the slider's own format, so the bounds in the tooltip read exactly like
// the value in the track. A parenthesised note in the format - "(auto)" - is dropped: it describes the
// current state, not the bound.
template<class T> static std::string fmt_bound(const char *format, T v)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), format, v);
    std::string  out(buf);
    const size_t paren = out.find('(');
    if (paren != std::string::npos)
        out.erase(paren);
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

static std::string range_text(const std::string &lo, const std::string &hi)
{
    return Slic3r::format(_u8L("Range: %1% to %2%"), lo, hi);
}

static std::string edit_icon_tip(const std::string &range)
{
    return _u8L("Type a value instead of dragging. Ctrl+click on the slider does the same.") + "\n\n" + range;
}

float GLGizmoTextureDisplacement::panel_gap() const { return std::round(m_imgui->scaled(0.4f)); }
float GLGizmoTextureDisplacement::panel_label_w() const { return m_imgui->scaled(4.8f); }
// Exactly as tall as the slider it sits beside, so it lines up with the track without the row having
// to be re-centred around it; icon_button() pads the glyph itself down to about 0.7 of this.
float GLGizmoTextureDisplacement::panel_edit_icon_w() const { return ImGui::GetFrameHeight(); }
float GLGizmoTextureDisplacement::panel_edit_icon_room() const { return panel_edit_icon_w() + panel_gap(); }

void GLGizmoTextureDisplacement::hover_tip(const std::string &text, const std::vector<TipMedia> &media)
{
    if (ImGui::IsItemHovered())
        panel_tooltip(text, media);
}

void GLGizmoTextureDisplacement::hover_tip(const wxString &text, const std::vector<TipMedia> &media)
{
    if (ImGui::IsItemHovered())
        panel_tooltip(text, media);
}

bool GLGizmoTextureDisplacement::icon_toggle(int uid, const std::string &iconfile, bool active, float sz,
                                             const wxString &label, const wxString &tip_text,
                                             const std::vector<TipMedia> &media, const wxString &unavailable)
{
    const ImGuiStyle &style     = ImGui::GetStyle();
    const ImVec4      orca      = ImGuiWrapper::COL_ORCA;
    const ImVec4      col_frame = ImGui::GetStyleColorVec4(ImGuiCol_Separator);
    const bool        na        = !unavailable.empty();
    const auto        it        = m_panel_icon_map.find(iconfile);
    const float       pad       = std::max(1.f, std::round(sz * 0.14f));
    const int         variant   = active ? 1 : 0;
    bool              clicked   = false;
    ImGui::PushID(uid);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, na ? style.Alpha * 0.32f : style.Alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
    ImGui::PushStyleColor(ImGuiCol_Button, active ? ImVec4(orca.x, orca.y, orca.z, 0.22f) : ImVec4(0.f, 0.f, 0.f, 0.f));
    if (na) {
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.f, 0.f, 0.f, 0.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.f, 0.f, 0.f, 0.f));
    }
    if (it != m_panel_icon_map.end() && int(it->second.size()) > variant && it->second[size_t(variant)]->is_valid()) {
        const IconManager::Icon &ic = *it->second[size_t(variant)];
        clicked = m_imgui->image_button((ImTextureID) (intptr_t) ic.tex_id, ImVec2(sz - 2.f * pad, sz - 2.f * pad), ic.tl, ic.br,
                                        int(pad));
    } else {
        clicked = ImGui::Button(label.ToUTF8().data(), ImVec2(0.f, sz)); // the control is never lost to a missing icon
    }
    ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                        ImGui::GetColorU32(active ? orca : col_frame), style.FrameRounding);
    ImGui::PopStyleColor(na ? 3 : 1);
    ImGui::PopStyleVar(2);
    ImGui::PopID();
    if (ImGui::IsItemHovered())
        panel_tooltip(na ? tip_text + "\n\n" + unavailable : tip_text, media);
    return clicked && !na;
}

bool GLGizmoTextureDisplacement::icon_button(int uid, const std::string &iconfile, float sz, const wxString &label,
                                             const wxString &tip_text, const std::vector<TipMedia> &media)
{
    const auto  it      = m_panel_icon_map.find(iconfile);
    const float pad     = std::max(1.f, std::round(sz * 0.14f));
    bool        clicked = false;
    ImGui::PushID(uid);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.f, 0.f, 0.f));
    if (it != m_panel_icon_map.end() && !it->second.empty() && it->second[0]->is_valid()) {
        const IconManager::Icon &ic = *it->second[0];
        clicked = m_imgui->image_button((ImTextureID) (intptr_t) ic.tex_id, ImVec2(sz - 2.f * pad, sz - 2.f * pad), ic.tl, ic.br,
                                        int(pad));
    } else {
        clicked = ImGui::Button(label.ToUTF8().data(), ImVec2(0.f, sz));
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImGui::PopID();
    if (!tip_text.empty() && ImGui::IsItemHovered())
        panel_tooltip(tip_text, media);
    return clicked;
}

void GLGizmoTextureDisplacement::vsep(float h)
{
    const float gap = panel_gap();
    ImGui::SameLine(0.f, gap);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + h * 0.2f), ImVec2(p.x, p.y + h * 0.8f),
                                        ImGui::GetColorU32(ImGuiCol_Separator));
    ImGui::Dummy(ImVec2(1.f, h));
    ImGui::SameLine(0.f, gap);
}

void GLGizmoTextureDisplacement::heading(const wxString &text)
{
    m_imgui->push_bold_font();
    m_imgui->text(text);
    m_imgui->pop_bold_font();
}

void GLGizmoTextureDisplacement::heading(const wxString &text, const std::string &summary, float card_pad)
{
    const float  frame_h = ImGui::GetFrameHeight();
    const float  x0      = ImGui::GetCursorPosX();
    const ImVec2 p       = ImGui::GetCursorScreenPos();
    const float  w = std::max(m_imgui->scaled(2.f), ImGui::GetContentRegionMax().x - x0 - std::max(card_pad, 1.f));
    ImGui::Dummy(ImVec2(w, frame_h));
    ImDrawList       *dl   = ImGui::GetWindowDrawList();
    const float       ty   = p.y + 0.5f * (frame_h - ImGui::GetFontSize());
    const std::string name = into_u8(text);
    m_imgui->push_bold_font();
    dl->AddText(ImVec2(p.x, ty), ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
    m_imgui->pop_bold_font();
    if (!summary.empty())
        dl->AddText(ImVec2(p.x + w - ImGui::CalcTextSize(summary.c_str()).x, ty), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                    summary.c_str());
}

void GLGizmoTextureDisplacement::render_mode_picker(float panel_w, bool busy)
{
    const ImGuiStyle &style  = ImGui::GetStyle();
    const ImVec4      orca   = ImGuiWrapper::COL_ORCA;
    const bool        dark   = wxGetApp().dark_mode();
    const float       pad    = std::round(m_imgui->scaled(0.6f));
    const float       img    = std::round(m_imgui->scaled(5.f));
    const float       text_x = pad + img + pad; // the text column, after the picture
    const float       wrap_w = panel_w - text_x - pad;

    // A card the width of the panel: a picture of what the mode makes beside its name and what it does,
    // the whole card one button. The pictures are the tooltip illustrations of the same features. An
    // animated one stands on its last frame, the finished result, and plays from the start on hover.
    const auto card = [&](int index, const char *id, const std::string &file, const std::string &title,
                          const std::string &text) {
        m_imgui->push_bold_font();
        ImFont      *title_font = ImGui::GetFont();
        const float  title_size = ImGui::GetFontSize();
        const ImVec2 title_sz   = ImGui::CalcTextSize(title.c_str());
        m_imgui->pop_bold_font();
        const ImVec2 text_sz = ImGui::CalcTextSize(text.c_str(), nullptr, false, wrap_w);
        const ImVec2 p       = ImGui::GetCursorScreenPos();
        const float  text_h  = title_sz.y + style.ItemSpacing.y + text_sz.y;
        const ImVec2 size(panel_w, std::round(std::max(img, text_h) + 2.f * pad));
        const bool   clicked = ImGui::InvisibleButton(id, size);
        const bool   hot     = ImGui::IsItemHovered() && !busy;
        const ImVec2 q(p.x + size.x, p.y + size.y);
        const float  r  = style.FrameRounding + 1.f;
        ImDrawList  *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, q, hot ? ImGui::GetColorU32(ImVec4(orca.x, orca.y, orca.z, 0.15f)) :
                                      ImGui::GetColorU32(ImGuiCol_Text, dark ? 0.045f : 0.047f), r);
        dl->AddRect(p, q, ImGui::GetColorU32(hot ? orca : style.Colors[ImGuiCol_Separator]), r);

        const ImVec2 img_min(p.x + pad, std::round(p.y + 0.5f * (size.y - img)));
        const ImVec2 img_max(img_min.x + img, img_min.y + img);
        const AnimatedTexture *texture =
            m_tip_media.get((boost::filesystem::path(resources_dir()) / "images" / "texture_displacement" / file).string());
        if (texture != nullptr && texture->is_valid()) {
            if (hot && m_mode_picker_hovered != index) {
                m_mode_picker_hovered     = index;
                m_mode_picker_hover_start = tip_now();
            } else if (!hot && m_mode_picker_hovered == index)
                m_mode_picker_hovered = -1;
            const double time = hot ? tip_now() - m_mode_picker_hover_start : std::max(0., texture->duration() - 1e-3);
            dl->AddImage((ImTextureID) (intptr_t) texture->texture_id(time), img_min, img_max);
            if (hot && texture->is_animated())
                m_parent.request_extra_frame(); // the canvas only redraws on demand
        }
        dl->AddRect(img_min, img_max, ImGui::GetColorU32(ImGuiCol_Text, 0.25f), style.FrameRounding);

        const float ty = std::round(p.y + 0.5f * (size.y - text_h));
        dl->AddText(title_font, title_size, ImVec2(p.x + text_x, ty), ImGui::GetColorU32(ImGuiCol_Text), title.c_str());
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(p.x + text_x, ty + title_sz.y + style.ItemSpacing.y),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), text.c_str(), nullptr, wrap_w);
        return clicked;
    };

    ImGui::AlignTextToFramePadding();
    heading(_L("Choose a mode"));
    // Only the first layer: past this point the panel is the same whichever card was picked.
    m_imgui->disabled_begin(busy);
    if (card(0, "##mode_chroma", "chroma_colors.gif", _u8L("ChromaStack"),
             _u8L("A color picture on a flat face, made of filament swaps: its height map is solved from the "
                  "picture and the filaments you give it.")))
        add_texture_layer(true);
    if (card(1, "##mode_relief", "tile_on.png", _u8L_CONTEXT("Displacement", "Texture tool mode"),
             _u8L("Height maps pushed into the surface where you paint them: tiles, depth and blending, in "
                  "layers.")))
        add_texture_layer(false);
    m_imgui->disabled_end();
}

void GLGizmoTextureDisplacement::slider_label(const wxString &label)
{
    const float x0 = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    m_imgui->text(label);
    ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), x0 + panel_label_w()));
}

// ---- Typed values on sliders ----
// Every slider in the panel goes through typed_slider(), so all of them take a number the same two
// ways: Ctrl+click on the track, or the edit icon at the right end for the same box without the
// modifier. Both end up in ImGui's own "temp input" mode - the icon only asks ImGui to focus the
// slider, which is what turns it into an input box (see SliderScalar) - so the parsing, the clamping
// and the Enter/click-away behaviour are ImGui's, not a second implementation.
bool GLGizmoTextureDisplacement::typed_row(const char *id, float item_w, const wxString &tip,
                                           const std::vector<TipMedia> &media, const std::string &range,
                                           const std::function<bool()> &slider)
{
    // Deferred from the click a frame ago: SetKeyboardFocusHere() is a request for the *next* frame,
    // so it has to be issued before the slider it names, not from the icon beside it.
    if (m_slider_type_request == id) {
        ImGui::SetKeyboardFocusHere();
        m_slider_type_request.clear();
        m_parent.set_as_dirty(); // the focus only lands on a frame that actually gets drawn
    }
    ImGui::SetNextItemWidth(item_w);
    const bool changed = slider();
    // AllowWhenDisabled: several of these rows are drawn inside disabled_begin(), and a greyed slider
    // is exactly when the user needs to be told what it is and why it is not moving.
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        wxString full = tip;
        if (!full.IsEmpty())
            full += "\n\n";
        panel_tooltip(full + from_u8(range), media);
    }
    // SameLine() puts the cursor back at the slider's own top (ItemSize records the Y the item was
    // drawn at), so an icon of the same height lands on the track wherever the row placed it.
    ImGui::SameLine(0.f, panel_gap());
    if (icon_button(int(ImGui::GetID(id)), "edit.svg", panel_edit_icon_w(), _L("Edit"), from_u8(edit_icon_tip(range)))) {
        m_slider_type_request       = id;
        m_slider_type_request_frame = ImGui::GetFrameCount();
        m_parent.set_as_dirty(); // the next frame is where the request above is turned into focus
    }
    return changed;
}

bool GLGizmoTextureDisplacement::typed_slider(const char *id, float item_w, float *v, float v_min, float v_max,
                                              const char *format, bool log, const wxString &tip,
                                              const std::vector<TipMedia> &media)
{
    return typed_row(id, item_w, tip, media, range_text(fmt_bound(format, v_min), fmt_bound(format, v_max)), [&] {
        return ImGui::SliderFloat(id, v, v_min, v_max, format,
                                  ImGuiSliderFlags_AlwaysClamp | (log ? ImGuiSliderFlags_Logarithmic : 0));
    });
}

bool GLGizmoTextureDisplacement::typed_slider_int(const char *id, float item_w, int *v, int v_min, int v_max,
                                                  const char *format, bool log, const wxString &tip,
                                                  const std::vector<TipMedia> &media)
{
    return typed_row(id, item_w, tip, media, range_text(fmt_bound(format, v_min), fmt_bound(format, v_max)), [&] {
        return ImGui::SliderInt(id, v, v_min, v_max, format,
                                ImGuiSliderFlags_AlwaysClamp | (log ? ImGuiSliderFlags_Logarithmic : 0));
    });
}

bool GLGizmoTextureDisplacement::float_row(const char *id, const wxString &label, float *v, float v_min, float v_max,
                                           const char *format, bool log, float right_inset, const wxString &tip,
                                           const std::vector<TipMedia> &media)
{
    slider_label(label);
    return typed_slider(id, -(std::max(right_inset, 1.f) + panel_edit_icon_room()), v, v_min, v_max, format, log, tip,
                        media);
}

bool GLGizmoTextureDisplacement::int_row(const char *id, const wxString &label, int *v, int v_min, int v_max,
                                         const char *format, float right_inset, const wxString &tip,
                                         const std::vector<TipMedia> &media)
{
    slider_label(label);
    return typed_slider_int(id, -(std::max(right_inset, 1.f) + panel_edit_icon_room()), v, v_min, v_max, format, false,
                            tip, media);
}

std::string GLGizmoTextureDisplacement::layer_name(const TextureDisplacementLayer &l)
{
    return l.name.empty() ? Slic3r::format(_u8L("Layer %1%"), l.slot + 1) : l.name;
}

bool GLGizmoTextureDisplacement::slot_painted(int slot) const
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr)
        return false;
    for (const ModelVolume *v : mo->volumes)
        if (v->is_model_part() && !v->texture_displacement_facet(slot).empty())
            return true;
    return false;
}

void GLGizmoTextureDisplacement::on_render_input_window(float x, float y, float bottom_limit)
{
    // Every frame, so only the slow ones are reported: see ChromaProfileScope's min_ms.
    ChromaProfileScope prof("panel_frame", {}, 12.0);
    ModelObject *mo = m_c->selection_info()->model_object();
    if (!mo)
        return;
    ModelVolume *mv = texture_volume();

    // The palette also follows project state nobody tells this gizmo about - the filaments and mixed
    // slots in the sidebar - so it is checked once a frame, and a change re-runs the previews.
    cached_palette();
    if (m_palette_changed)
        m_preview_params_dirty = true;

    float  scale = m_parent.get_scale();
    #ifdef WIN32
        int dpi = get_dpi_for_window(wxGetApp().GetTopWindow());
        scale *= (float) dpi / (float) DPI_DEFAULT;
    #endif // WIN32

    const float approx_height = m_imgui->scaled(24.f);
    y = std::min(y, bottom_limit - approx_height);

    // Docked (the default) the panel is pinned to the right edge of the 3D canvas and cannot be
    // moved. Deliberately *not* next to the gizmo toolbar, which is where `x` points and where every
    // other gizmo's window goes: this panel is far taller than those (layer stack plus the whole
    // per-layer control set), so at the toolbar it sits right on top of the part of the model being
    // painted. Pinning it to the canvas edge also parks it against the UV editor, since that pane is
    // docked on the right and the canvas therefore ends exactly at the pane's left edge - so the
    // panel follows the pane in and out instead of being clipped by it.
    //
    // Undocked it becomes an ordinary floating window: a title bar to drag it by, and no forced
    // position - the position is deliberately not seeded on undock, so the window stays exactly
    // where it already was and the user just gains the ability to move it from there.
    ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse;
    if (!m_undocked) {
        flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar;
        // Right-aligned (pivot 1), so the width the panel auto-resized to last frame does not need to
        // be known here. Width 0 skips GizmoImguiSetNextWIndowPos()'s own left-aligned fit-to-canvas
        // clamp, which would push the window back off the edge it is being pinned to.
        float right = float(m_parent.get_canvas_size().get_width()) - m_imgui->scaled(0.5f);
        GizmoImguiSetNextWIndowPos(right, y, 0.f, 0.f, ImGuiCond_Always, 1.0f, 0.0f);
    }

    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    GizmoImguiBegin(get_name(), flags);
    // Where this frame's tooltips place themselves (see panel_tooltip()). The size is last frame's -
    // this window auto-resizes, so its height is only known once its content has been laid out - which
    // is close enough for a tooltip that needs a frame of hovering to appear in the first place.
    m_tooltip_anchor_min = ImGui::GetWindowPos();
    m_tooltip_anchor_max = ImVec2(m_tooltip_anchor_min.x + ImGui::GetWindowSize().x,
                                  m_tooltip_anchor_min.y + ImGui::GetWindowSize().y);
    ensure_panel_icons();
    process_uv_commands(); // clicks from the UV editor pane, run here where the GL context is current
    // After the pane's commands, which can close it or change the view, and before the View row, so the row
    // lights the view this frame ends up in and a click on it has the last word.
    follow_island_highlight();
    const float previous_body_h   = m_panel_body_h;
    const float previous_footer_h = m_panel_footer_h;

    // Pinned every frame while Standard is active, so what Preview shows is always what Bake will do.
    if (!pro_mode() && apply_standard_mode_presets(mv))
        m_preview_params_dirty = true;

    // Layout follows the Option C design: a fixed-width panel, a header pinned at the top, a body that
    // scrolls as one, and a footer with Bake pinned at the bottom. Rows are laid out against panel_w and
    // against the body child's own fixed width - never against this window's width, which under
    // AlwaysAutoResize is derived from its content and would feed back into it.
    const bool        dark     = wxGetApp().dark_mode();
    const bool        busy     = m_bake_in_progress || m_prepare_in_progress;
    const ImGuiStyle &style    = ImGui::GetStyle();
    const float       panel_w  = m_imgui->scaled(21.5f);
    const float       frame_h  = ImGui::GetFrameHeight();
    const float       icon_md  = std::round(frame_h * 1.25f); // tool and view buttons
    const float       icon_sm  = std::round(frame_h * 1.05f); // buttons sitting in a row of text
    const float       gap_s    = panel_gap();
    const float       label_w  = panel_label_w(); // label column of a label + slider row
    const float       card_pad = std::round(m_imgui->scaled(0.55f));
    const ImVec4      orca     = ImGuiWrapper::COL_ORCA;
    const ImVec4      col_link = ImGuiWrapper::COL_ORCA;
    const ImVec4      col_frame = ImGui::GetStyleColorVec4(ImGuiCol_Separator);
    const ImU32       col_card = ImGui::GetColorU32(ImGuiCol_Text, dark ? 0.045f : 0.047f);
    const ImU32       col_line = ImGui::GetColorU32(ImGuiCol_Text, dark ? 0.08f : 0.09f);
    const ImU32       col_sep  = ImGui::GetColorU32(ImGuiCol_Separator);

    const auto   scoped_combo   = [&](const char *id, int *v, const char *const items[], int n) {
        push_panel_combo_style(m_parent.get_scale());
        const bool changed = ImGui::Combo(id, v, items, n);
        pop_panel_combo_style();
        return changed;
    };

    // ---- Typed values on sliders ----
    // Every slider in the panel goes through typed_slider(), so all of them take a number the same
    // two ways: Ctrl+click on the track, or the edit icon at the right end for the same box without
    // the modifier. Both end up in ImGui's own "temp input" mode - the icon only asks ImGui to focus
    // the slider, which is what turns it into an input box (see SliderScalar) - so the parsing,
    // the clamping and the Enter/click-away behaviour are ImGui's, not a second implementation.

    // Retires a click whose row is no longer being drawn - the panel changed mode in between - so that
    // it cannot fire later when that row comes back. Before any row is drawn, so a live one still wins.
    if (!m_slider_type_request.empty() && ImGui::GetFrameCount() > m_slider_type_request_frame + 1)
        m_slider_type_request.clear();

    // ---- Header: title, Standard / Pro, dock toggle. Pinned above the body. ----
    {
        const float x0 = ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding();
        heading(_L("Texture tool"));

        const std::string labels[2] = { _u8L_CONTEXT("Standard", "Mode"), _u8L("Pro") };
        const float       seg_pad   = m_imgui->scaled(0.5f);
        const float seg_w[2] = { ImGui::CalcTextSize(labels[0].c_str()).x + 2.f * seg_pad, ImGui::CalcTextSize(labels[1].c_str()).x + 2.f * seg_pad };
        ImGui::SameLine();
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), x0 + panel_w - (seg_w[0] + seg_w[1] + gap_s + icon_sm)));

        // Standard / Pro is a mode, not an option: Standard hides every mesh-preparation control and folds
        // the whole recipe into Bake, Pro shows all of it and hands the ordering to the user.
        const ImVec2 seg_min = ImGui::GetCursorScreenPos();
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.f, style.ItemSpacing.y));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
        m_imgui->disabled_begin(busy);
        for (int m = 0; m < 2; ++m) {
            if (m > 0)
                ImGui::SameLine();
            const bool on = m_panel_mode == m;
            ImGui::PushStyleColor(ImGuiCol_Button, on ? orca : ImVec4(0.f, 0.f, 0.f, 0.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? orca : ImVec4(orca.x, orca.y, orca.z, 0.25f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, orca);
            ImGui::PushStyleColor(ImGuiCol_Text, on ? ImVec4(1.f, 1.f, 1.f, 1.f) : style.Colors[ImGuiCol_TextDisabled]);
            const bool clicked = ImGui::Button((labels[m] + "##panel_mode").c_str(), ImVec2(seg_w[m], frame_h));
            ImGui::PopStyleColor(4);
            hover_tip(m == 0 ? _u8L("Standard - pick a texture, paint where it goes, press Bake. Everything the "
                                   "mesh needs is done for you in that one step.") :
                               _u8L("Pro - the mesh preparation is yours to run: Remesh, Subdivide and Bake as "
                                   "separate steps, with every setting on show. For when Standard's result is "
                                   "not what you wanted and you know why."));
            if (clicked && !on) {
                m_panel_mode = m;
                store_panel_state();
                if (!pro_mode()) {
                    // Leaving the subdivision preview open would strand a wireframe whose controls just
                    // disappeared, so close it as part of the switch.
                    m_subdivide_editing      = false;
                    m_subdivide_preview_tris = -1;
                    m_subdivide_preview_glmodel.reset();
                    if (apply_standard_mode_presets(mv))
                        m_preview_params_dirty = true;
                }
                m_parent.set_as_dirty();
            }
        }
        m_imgui->disabled_end();
        ImGui::PopStyleVar(2);
        ImGui::GetWindowDrawList()->AddRect(seg_min, ImVec2(ImGui::GetItemRectMax().x, seg_min.y + frame_h),
                                            ImGui::GetColorU32(col_frame), style.FrameRounding);

        ImGui::SameLine(0.f, gap_s);
        if (icon_button(806, "canvas_drag.svg", icon_sm, m_undocked ? _L("Dock panel") : _L("Undock panel"),
                        _L("Detach this panel so it can be dragged anywhere over the 3D view, or dock it back beside "
                           "the toolbar."))) {
            m_undocked = !m_undocked;
            wxGetApp().app_config->set_bool(UNDOCKED_CONFIG_KEY, m_undocked);
        }
    }
    ImGui::Separator();

    // ---- No layers yet: the mode picker stands in for the body and the footer ----
    // Every control below belongs to a layer, so with none there is nothing for them to act on.
    if (mv != nullptr && mv->texture_displacement_layers.empty()) {
        render_mode_picker(panel_w, busy);
        // The body and footer were not drawn, so whichever comes back next sizes itself from scratch.
        m_panel_body_h   = 0.f;
        m_panel_footer_h = 0.f;
        GizmoImguiEnd();
        ImGuiWrapper::pop_toolbar_style();
        return;
    }

    // ---- Body: everything between the header and the footer, scrolling as one. ----
    // Sized from last frame's content and footer heights (see m_panel_body_h): as tall as its content,
    // but never so tall that the footer drops below the bottom of the canvas.
    const float bottom     = m_undocked ? ImGui::GetIO().DisplaySize.y : bottom_limit;
    const float max_body_h = std::max(m_imgui->scaled(8.f), bottom - ImGui::GetCursorScreenPos().y - m_panel_footer_h -
                                                                 style.WindowPadding.y - style.ItemSpacing.y);
    const float body_h     = m_panel_body_h > 0.f ? std::min(m_panel_body_h + 1.f, max_body_h) : max_body_h;

    // The width a card's rows may use, from the body child's fixed width rather than from whatever is
    // left after a scrollbar: anything whose HEIGHT follows the width it is given has to be measured
    // against something that does not move, or the panel oscillates between having a scrollbar and
    // not. A scrollbar's worth is always held back, so the content never has to change when one
    // appears.
    const float card_content_w = panel_w - 2.f * card_pad - m_imgui->scaled(0.5f);

    push_slim_scrollbar_style(m_imgui->scaled(0.5f), dark);
    // NoScrollWithMouse: ease_wheel_scroll() eases the scroll instead of teleporting five text lines per
    // notch, which on blocks this tall lost the reader's place.
    ImGui::BeginChild("##td_body", ImVec2(panel_w, body_h), false, ImGuiWindowFlags_NoScrollWithMouse);

    // Everything that changes the layer list is deferred until the list is no longer being drawn:
    // removing or reordering shifts mv->texture_displacement_layers under the pointers the loop holds,
    // and switching the active layer mid-loop would draw two cards open for a frame.
    int slot_to_remove = -1;
    int move_slot      = -1;
    int move_to        = -1;
    int activate_slot  = -1;

    TextureDisplacementLayer *active = mv != nullptr ? active_layer() : nullptr;

    // ---- Paint: which layer strokes land in, and Paint / Erase ----
    {
        ImGui::AlignTextToFramePadding();
        heading(_L("Paint"));
        const std::string into      = active != nullptr ? Slic3r::format(_u8L("into %1%"), layer_name(*active)) :
                                                          _u8L("add a layer to paint");
        const float       toggles_x = ImGui::GetWindowContentRegionMax().x - (2.f * icon_sm + gap_s);
        const float       text_end  = toggles_x - (2.f * gap_s + 1.f);
        ImGui::SameLine();
        const std::string shown = ellipsize(into, std::max(0.f, text_end - ImGui::GetCursorPosX()));
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), text_end - ImGui::CalcTextSize(shown.c_str()).x));
        ImGui::TextDisabled("%s", shown.c_str());
        vsep(icon_sm);
        ImGui::SetCursorPosX(toggles_x);
        if (icon_toggle(820, "texture_displacement_add.svg", !m_erase_mode, icon_sm, _L("Paint"),
                        _L("Paint - add the active layer where you click or drag. The right button erases, and so does "
                           "holding Shift."),
                        { { "paint.gif" } }))
            m_erase_mode = false;
        ImGui::SameLine(0.f, gap_s);
        if (icon_toggle(821, "texture_displacement_add_negative.svg", m_erase_mode, icon_sm, _L("Erase"),
                        _L("Erase - remove the active layer where you click or drag. The right button paints."),
                        { { "erase.gif" } }))
            m_erase_mode = true;
    }

    // ---- Tools: brush / face / connected area, then the whole-model actions, then the active tool's own
    // control filling the rest of the row ----
    // "Face" and "Connected area" reuse the exact same selection machinery every other paint gizmo has
    // (single-facet click, and angle-limited flood fill respectively).
    {
        const bool is_brush_mode = m_tool_type == ToolType::BRUSH && m_cursor_type != TriangleSelector::CursorType::POINTER;
        const bool is_face_mode  = m_tool_type == ToolType::BRUSH && m_cursor_type == TriangleSelector::CursorType::POINTER;
        const bool is_area_mode  = m_tool_type == ToolType::SMART_FILL;
        const float row_y        = ImGui::GetCursorPosY();
        if (icon_toggle(801, "texture_displacement_brush.svg", is_brush_mode, icon_md, _L("Brush"),
                        _L("Brush - paint over the surface by dragging"), { { "paint.gif" } })) {
            m_tool_type = ToolType::BRUSH;
            if (m_cursor_type == TriangleSelector::CursorType::POINTER)
                m_cursor_type = TriangleSelector::CursorType::CIRCLE;
        }
        ImGui::SameLine(0.f, gap_s);
        if (icon_toggle(802, "texture_displacement_face.svg", is_face_mode, icon_md, _L("Face"), _L("Face - click individual triangles"),
                        { { "tool_face.gif" } })) {
            m_tool_type   = ToolType::BRUSH;
            m_cursor_type = TriangleSelector::CursorType::POINTER;
        }
        ImGui::SameLine(0.f, gap_s);
        if (icon_toggle(803, "texture_displacement_connected_area.svg", is_area_mode, icon_md, _L("Connected area"),
                        _L("Connected area - flood-fill the region reachable without crossing an edge sharper than the "
                           "angle threshold"),
                        { { "tool_smart_fill.gif" } })) {
            m_tool_type   = ToolType::SMART_FILL;
            m_cursor_type = TriangleSelector::CursorType::POINTER;
        }

        // Whole model: paint every face with the active layer, or clear its paint from all of them.
        const wxString whole_na = busy               ? _L("Wait for the bake to finish.") :
                                  active == nullptr  ? _L("Add a layer first.") :
                                                       wxString();
        const wxString erase_na = !whole_na.empty()                     ? whole_na :
                                  !slot_painted(m_active_layer_slot) ? _L("The active layer has no paint yet.") :
                                                                          wxString();
        ImGui::SameLine(0.f, gap_s);
        if (icon_toggle(806, "texture_displacement_select_all.svg", false, icon_md, _L("Select whole model"),
                        _L("Select whole model - paint every face of the model with the active layer"),
                        { { "select_all.png" } }, whole_na))
            select_whole_model();
        ImGui::SameLine(0.f, gap_s);
        if (icon_toggle(807, "texture_displacement_erase_all.svg", false, icon_md, _L("Erase whole model"),
                        _L("Erase whole model - clear the active layer's paint from every face"),
                        { { "erase_all.png" } }, erase_na)) {
            Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Reset texture displacement selection"),
                                          UndoRedo::SnapshotType::GizmoAction);
            int idx = -1;
            for (ModelVolume *v : mo->volumes)
                if (v->is_model_part()) {
                    ++idx;
                    m_triangle_selectors[idx]->reset();
                    m_triangle_selectors[idx]->request_update_render_data(true);
                }
            update_model_object();
            m_parent.set_as_dirty();
        }

        // The active tool's control fills the rest of the row, each part centred on the (taller) tool icons.
        const float row_end       = ImGui::GetWindowContentRegionMax().x;
        const auto  centre_on_row = [&](float h) { ImGui::SetCursorPosY(row_y + std::round((icon_md - h) * 0.5f)); };
        vsep(icon_md);
        if (is_brush_mode) {
            centre_on_row(frame_h);
            typed_slider("##cursor_radius",
                         std::max(1.f, row_end - ImGui::GetCursorPosX() - (3.f * gap_s + 1.f + 2.f * icon_sm) - panel_edit_icon_room()),
                         &m_cursor_radius, CursorRadiusMin, CursorRadiusMax, "%.2f mm", false, m_desc.at("cursor_size"),
                         { { "cursor_size.gif" } });
            vsep(icon_sm);
            const bool is_circle = m_cursor_type == TriangleSelector::CursorType::CIRCLE;
            centre_on_row(icon_sm);
            if (icon_toggle(804, "circle_paint.svg", is_circle, icon_sm, m_desc.at("circle"),
                            _L("Circle - paints everything under the brush as seen from the camera")))
                m_cursor_type = TriangleSelector::CursorType::CIRCLE;
            ImGui::SameLine(0.f, gap_s);
            centre_on_row(icon_sm);
            if (icon_toggle(805, "menu_obj_sphere.svg", !is_circle, icon_sm, m_desc.at("sphere"),
                            _L("Sphere - paints only within a ball around the point under the cursor")))
                m_cursor_type = TriangleSelector::CursorType::SPHERE;
        } else if (is_area_mode) {
            centre_on_row(frame_h);
            typed_slider("##smart_fill_angle", std::max(1.f, row_end - ImGui::GetCursorPosX() - panel_edit_icon_room()),
                         &m_smart_fill_angle, SmartFillAngleMin, SmartFillAngleMax, "%.0f°", false,
                         _L("Angle threshold - the fill stops at edges sharper than this"),
                         { { "smart_fill_angle_low.png", _L("Low") }, { "smart_fill_angle_high.png", _L("High") } });
        } else {
            centre_on_row(frame_h);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", ellipsize(_u8L("Click a triangle to paint it"),
                                                std::max(0.f, row_end - ImGui::GetCursorPosX())).c_str());
        }
    }

    // ---- View: Normal / Fast / Checker / Distortion as one group, Wireframe on its own ----
    // The underlying state stays m_use_shaded_preview + m_uv_check_mode.
    {
        const int cur_mode = view_mode();
        int  new_mode       = cur_mode;
        int  picked         = -1; // the button clicked, the active one included
        bool wf_toggle      = false;
        bool open_uv_editor = false;
        // Checker and Distortion both draw *the unwrap* - the first the texture grid laid over it, the second
        // its stretch - so they only mean anything for a layer mapped with Unwrap (LSCM). On the default
        // triplanar mapping (or cylindrical / spherical / from view) they are faded out with the reason in the
        // tooltip, rather than being offered and then showing nothing.
        const wxString uv_view_na = active == nullptr ? _L("Add a layer first.") :
                                    active->projection_method != TextureProjectionMethod::LSCM ?
                                                        _L("Only for a layer mapped with Unwrap (LSCM) - set the "
                                                           "active layer's Mapping to Unwrap to use this view.") :
                                                        wxString();
        // Either view over a layer that stopped being an unwrap shows nothing at all, so fall back to Normal.
        if ((cur_mode == 2 || cur_mode == 3) && !uv_view_na.empty())
            new_mode = 0;

        const float x0 = ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", _u8L("View").c_str());
        ImGui::SameLine();
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), x0 + m_imgui->scaled(2.6f)));
        if (icon_toggle(701, "texture_displacement_real_preview.svg", cur_mode == 0, icon_md, _L("Normal"),
                        _L("Normal - the real geometry, exactly what Bake will produce. Slower to update"),
                        { { "tile_on.png" } }))
            new_mode = picked = 0;
        ImGui::SameLine(0.f, gap_s);
        if (icon_toggle(702, "texture_displacement_fast_preview.svg", cur_mode == 1, icon_md, _L("Fast"),
                        _L("Fast - the relief is only shaded on, not built, and only for the layer you are "
                           "editing. Updates instantly while you paint"),
                        { { "view_fast.png" } }))
            new_mode = picked = 1;
        ImGui::SameLine(0.f, gap_s);
        if (icon_toggle(703, "texture_displacement_checker.svg", cur_mode == 2, icon_md, _L("Checker"),
                        _L("Checker - a test grid instead of the texture. Where the squares stay square the "
                           "texture is undistorted; where they stretch, it will too. Opens the UV editor if "
                           "it is closed"),
                        { { "view_checker.png" } }, uv_view_na)) {
            new_mode       = picked = 2;
            open_uv_editor = true;
        }
        ImGui::SameLine(0.f, gap_s);
        if (icon_toggle(704, "texture_displacement_distortion.svg", cur_mode == 3, icon_md, _L("Distortion"),
                        _L("Distortion - blue-to-red stretch heatmap over the unwrap. Opens the UV editor if "
                           "it is closed"),
                        { { "view_distortion.png" } }, uv_view_na)) {
            new_mode       = picked = 3;
            open_uv_editor = true;
        }
        vsep(icon_md);
        if (icon_toggle(705, "texture_displacement_wireframe.svg", m_wireframe_overlay, icon_md, _L("Wireframe"),
                        _L("Wireframe - overlay the mesh edges; independent of the view above"),
                        { { "view_wireframe.png" } }))
            wf_toggle = true;

        const std::string auto_label = _u8L("Auto");
        const float       auto_w     = frame_h * 0.78f /*ratio from BBLCheckbox*/ + style.ItemInnerSpacing.x + ImGui::CalcTextSize(auto_label.c_str()).x;
        ImGui::SameLine();
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - auto_w));
        if (m_imgui->bbl_checkbox(wxString::FromUTF8(auto_label) + "##auto_update", m_auto_update) && m_auto_update)
            rebuild_preview(); // catch up anything that changed while it was off
        hover_tip(_u8L("Rebuilds the preview as soon as anything changes. Turn it off on a heavy model if painting "
                        "or dragging a slider starts to stutter - the preview then waits until you let go."));

        // The views of a ChromaStack layer used to be three checkboxes here, on a line of their own
        // below this row. They are in the layer's own card now, on its Check step: this row is full before
        // they are added, and on their own line they still ran past the right edge of the panel as
        // soon as they were translated. See render_chroma_views().

        // Both are views of the unwrap, so picking one brings the UV editor up with it - including when that
        // view is already the active one and only the pane is missing.
        if (open_uv_editor && !m_show_uv_editor) {
            m_show_uv_editor = true;
            if (new_mode == cur_mode)
                update_uv_editor(); // otherwise apply_view_mode() below does it
        }
        if (picked >= 0)
            m_island_view.picked(picked);
        if (new_mode != cur_mode)
            apply_view_mode(new_mode);
        if (wf_toggle) {
            m_wireframe_overlay = !m_wireframe_overlay;
            refresh_wireframe();
            m_parent.set_as_dirty();
        }
    }

    ImGui::Separator();
    // ---- Texture layers: the active layer as an open card, every other one as a single row ----
    //
    // The heading and the count are for a stack of layers. A picture is not one: an object may hold
    // exactly one, it is always the top, and nothing can be added over it - so "Texture layers 1 / 8"
    // over a single card is a heading for a list that cannot have a second entry.
    const bool chroma_only = mv != nullptr && mv->texture_displacement_layers.size() == 1 &&
                             mv->texture_displacement_layers.front().chroma_enabled;
    if (!chroma_only) {
        ImGui::AlignTextToFramePadding();
        heading(_L("Texture layers"));
        if (mv != nullptr) {
            const std::string count = std::to_string(mv->texture_displacement_layers.size()) + " / " +
                                      std::to_string(TEXTURE_DISPLACEMENT_MAX_LAYERS);

            const std::string add_label = "+  " + _u8L("Add layer");

            ImGui::SameLine();
            ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(count.c_str()).x - ImGui::CalcTextSize(add_label.c_str()).x) - (style.FrameBorderSize + style.FramePadding.x) * 2.f - style.ItemSpacing.x);
            ImGui::TextDisabled("%s", count.c_str());

            ImGui::SameLine();
            const bool full = mv->texture_displacement_layers.size() >= TEXTURE_DISPLACEMENT_MAX_LAYERS;
            //Slic3r::format(into_u8(_L_PLURAL("All %1% layer used", "All %1% layers used", unsigned(TEXTURE_DISPLACEMENT_MAX_LAYERS))), TEXTURE_DISPLACEMENT_MAX_LAYERS)
            m_imgui->disabled_begin(busy || full);
            if (ImGui::Button((add_label + "##add_layer").c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0.f)))
                ImGui::OpenPopup("add_layer_kind");
            m_imgui->disabled_end();
            // The kind is chosen here because it is a property of the layer, not of the panel: the two
            // cards carry different controls and a card never shows one that does not apply to it.
            if (PanelPopup popup{"add_layer_kind"}) {
                // Width zero, so each row fills the popup rather than its own label: a selectable sized
                // to its text highlights behind the word only, which reads as nothing happening rather
                // than as the row being picked.
                const ImVec2 item(0.f, frame_h);
                if (ImGui::Selectable(_u8L("Relief layer").c_str(), false, 0, item))
                    add_texture_layer(false);
                if (ImGui::IsItemHovered())
                    panel_tooltip(_u8L("A height map pushed into the surface. Tiles, depth, blending."));
                if (ImGui::Selectable(_u8L("ChromaStack picture").c_str(), false, 0, item))
                    add_texture_layer(true);
                if (ImGui::IsItemHovered())
                    panel_tooltip(_u8L("A color picture made of filament swaps: one copy, on a flat face, its height "
                                       "map solved from the picture and the filaments you give it."));
            }
        }
    }

    std::vector<TextureDisplacementLayer *> ordered;
    if (mv != nullptr) {
        for (TextureDisplacementLayer &l : mv->texture_displacement_layers)
            ordered.push_back(&l);
        std::sort(ordered.begin(), ordered.end(), [](const auto *a, const auto *b) { return a->slot < b->slot; });
    }

    ImDrawList *dl         = ImGui::GetWindowDrawList();
    const float content_rx = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    const float rounding   = style.FrameRounding + 1.f;

    // Opens the texture library for a layer, making it the active one too (after the loop).
    const auto open_picker = [&](int slot) {
        m_picker_slot            = slot;
        m_picker_open_request    = true;
        m_picker_reveal_selected = true;
    };
    // The six-dot drag icon, drawn into `list` over [x, x + w] x [y, y + h].
    const auto draw_drag_icon = [&](ImDrawList *list, float x, float y, float w, float h, ImU32 tint) {
        const auto it = m_panel_icon_map.find("texture_displacement_drag.svg");
        if (it != m_panel_icon_map.end() && !it->second.empty() && it->second[0]->is_valid()) {
            // The icon's dots sit in the middle of its square, so it is drawn at the row's height centred on the
            // narrow hit area; only its transparent margins overhang the neighbouring widgets.
            const IconManager::Icon &ic = *it->second[0];
            const float              cx = x + 0.5f * w;
            list->AddImage((ImTextureID) (intptr_t) ic.tex_id, ImVec2(cx - 0.5f * h, y), ImVec2(cx + 0.5f * h, y + h), ic.tl, ic.br, tint);
        } else {
            const float r = std::max(1.f, std::round(m_imgui->scaled(0.08f)));
            for (int row = 0; row < 3; ++row)
                for (int col = 0; col < 2; ++col)
                    list->AddCircleFilled(ImVec2(x + w * (0.3f + 0.4f * col), y + h * 0.5f + float(row - 1) * 4.f * r), r, tint);
        }
    };
    const auto drag_tint = [dark](int alpha) { return dark ? IM_COL32(255, 255, 255, alpha) : IM_COL32(90, 90, 90, alpha); };

    // A card's type badge: a 1px frame around a word. Teal for a ChromaStack picture, the disabled grey
    // for a relief layer, so the two kinds are told apart by frame and word rather than by colour alone.
    const auto chip_width = [&](const wxString &text) {
        return ImGui::CalcTextSize(into_u8(text).c_str()).x + 2.f * std::round(m_imgui->scaled(0.18f));
    };
    // The badge is also the control: clicking it is how a layer changes kind, which is the only place
    // that can be done and the first place a user looks for it.
    const auto chip = [&](const wxString &text, bool accent) {
        const std::string t   = into_u8(text);
        const ImVec2      ts  = ImGui::CalcTextSize(t.c_str());
        const float       pad = std::round(m_imgui->scaled(0.18f));
        const ImVec2      p   = ImGui::GetCursorScreenPos();
        const float       h   = ts.y + 2.f * pad;
        const bool clicked = ImGui::InvisibleButton("##kind", ImVec2(ts.x + 2.f * pad, frame_h));
        const bool hot     = ImGui::IsItemHovered();
        const float y   = std::round(p.y + 0.5f * (frame_h - h));
        ImVec4      tint = accent ? orca : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
        if (!hot)
            tint.w *= 0.85f;
        const ImU32 col = ImGui::GetColorU32(tint);
        ImDrawList *d   = ImGui::GetWindowDrawList();
        if (hot)
            d->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + ts.x + 2.f * pad, y + h),
                             ImGui::GetColorU32(ImVec4(tint.x, tint.y, tint.z, 0.18f)), style.FrameRounding);
        d->AddRect(ImVec2(p.x, y), ImVec2(p.x + ts.x + 2.f * pad, y + h), col, style.FrameRounding);
        d->AddText(ImVec2(p.x + pad, y + pad), col, t.c_str());
        return clicked;
    };

    const float grip_w = std::round(m_imgui->scaled(0.7f));
    // Six-dot handle that drags a layer onto another one to reorder the stack.
    const auto grip = [&](const TextureDisplacementLayer &l, float h) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##grip", ImVec2(grip_w, h));
        const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
        draw_drag_icon(dl, p.x, p.y, grip_w, h, drag_tint(hot ? 255 : 150));
        hover_tip(_u8L("Drag onto another layer to reorder. The first layer sits on the bare surface; each one after "
                        "it adds its relief on top of the ones before."));
        // ImGui's own preview is a tooltip holding whatever is submitted here. The panel draws a copy of the whole
        // layer row under the cursor instead (after the layer list), so that one is switched off.
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
            const int slot = l.slot;
            ImGui::SetDragDropPayload("TD_LAYER", &slot, sizeof(slot));
            ImGui::EndDragDropSource();
        }
    };

    // A kind change asked for by a card's chip, carried out after the list has been drawn: it rewrites
    // the layer, and the preview with it, which is not something to do halfway through drawing them.
    int  convert_slot      = -1;
    bool convert_to_chroma = false;

    // The layer being dragged, while a reorder drag is in flight. Its card stays in place as a faded ghost.
    int dragged_slot = -1;
    if (const ImGuiPayload *payload = ImGui::GetDragDropPayload(); payload != nullptr && payload->IsDataType("TD_LAYER"))
        dragged_slot = *static_cast<const int *>(payload->Data);
    int dragged_index = -1;
    for (size_t i = 0; i < ordered.size(); ++i)
        if (ordered[i]->slot == dragged_slot)
            dragged_index = int(i);
    const ImU32 ghost_col = ImGui::GetColorU32(ImGuiCol_WindowBg, 0.65f);

    // A layer card or row [mn, mx] as a drop target. Dropped anywhere on another layer, the dragged one takes its
    // place and the layers in between shift over by one - so it lands above the target when moving up and below it
    // when moving down. A teal line on that side shows where; ImGui's default highlight rectangle says only
    // "something is here", so it is replaced by the line.
    //
    // Deciding by which half of the target the cursor is over instead made dropping onto a neighbour's near half a
    // silent no-op ("after the layer I am already after"), and that is exactly where a drag to the top ends.
    const auto drop_target = [&](size_t index, int slot, const ImVec2 &mn, const ImVec2 &mx) {
        if (!ImGui::BeginDragDropTarget())
            return;
        if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("TD_LAYER", ImGuiDragDropFlags_AcceptBeforeDelivery |
                                                                                 ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
            const int  from  = *static_cast<const int *>(payload->Data);
            const bool after = dragged_index >= 0 && dragged_index < int(index);
            if (from != slot) {
                const float y = after ? mx.y + 2.f : mn.y - 2.f;
                dl->AddLine(ImVec2(mn.x, y), ImVec2(mx.x, y), ImGui::GetColorU32(orca), 2.f);
            }
            if (payload->IsDelivery()) {
                move_slot = from;
                move_to   = int(index) + (after ? 1 : 0);
            }
        }
        ImGui::EndDragDropTarget();
    };

    for (size_t li = 0; li < ordered.size(); ++li) {
        TextureDisplacementLayer &layer     = *ordered[li];
        const bool                is_active = layer.slot == m_active_layer_slot;
        const bool                painted   = slot_painted(layer.slot);
        // Which of the two cards this is. The kind is the layer's own, fixed when it was added, so a
        // card never shows a control that does not apply to it.
        const bool                chroma_card = layer.chroma_enabled;
        ImGui::PushID(layer.slot);

        if (is_active) {
            // The card's height is not known until it is laid out, so its content goes into a foreground
            // channel and the backing rectangle into a background one, merged at the end - the standard
            // ImGui "rect behind a group" trick.
            const ImVec2 card_min = ImGui::GetCursorScreenPos();
            dl->ChannelsSplit(2);
            dl->ChannelsSetCurrent(1);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + card_pad);
            ImGui::Indent(card_pad);
            ImGui::BeginGroup();

            // Header: grip, thumbnail and name (both open the library), paint state, order, remove.
            {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap_s, style.ItemSpacing.y));
                if (chroma_card) {
                    // No grip and no room kept for one. A ChromaStack picture is always the top layer -
                    // its colours come from swapping filament at a height, so anything printed over it
                    // would bury them - and a blank column the width of a handle, on the one card that
                    // can never have one, is an indent with nothing in it.
                }
                else
                    grip(layer, frame_h), ImGui::SameLine();
                // The ChromaStack card's thumbnail is the *predicted print* rather than the source
                // picture: it is the most informative image in the panel, and this is where the eye
                // already lands - including on a card that has been collapsed.
                GLTexture *thumb = chroma_card ? (m_chroma_preview[size_t(layer.slot)] ?
                                                      m_chroma_preview[size_t(layer.slot)].get() :
                                                      nullptr) :
                                                 get_layer_thumbnail(layer);
                if (thumb != nullptr) {
                    if (ImGui::ImageButton((ImTextureID) (intptr_t) thumb->get_id(), ImVec2(frame_h, frame_h), ImVec2(0.f, 0.f),
                                           ImVec2(1.f, 1.f), 0)) {
                        if (chroma_card)
                            chroma_pick_picture(layer);
                        else
                            open_picker(layer.slot);
                    }
                } else if (ImGui::Button("?##thumb", ImVec2(frame_h, frame_h))) {
                    if (chroma_card)
                        chroma_pick_picture(layer);
                    else
                        open_picker(layer.slot);
                }
                hover_tip(chroma_card ?
                              _u8L("The print this stack is predicted to produce. Click to choose a different picture.") :
                              _u8L("Pick a different image for this layer. The paint, depth and tiling stay as they are."));
                {
                    // The teal outline on the thumbnail is what marks this card as the layer being painted.
                    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                    dl->AddRect(ImVec2(a.x - 2.f, a.y - 2.f), ImVec2(b.x + 2.f, b.y + 2.f), ImGui::GetColorU32(orca), style.FrameRounding);
                }
                ImGui::SameLine();

                const std::string not_painted = _u8L("not painted");
                // The feature's own name, and the same one everywhere else it is named: the object
                // menu, the Add layer popup, the docs and the code. It was "Color picture" here, which
                // reads better against its pair "Relief" and is the choice a reader would rather be
                // offered - but a feature with two names in one panel is a feature the user cannot ask
                // a question about, and the name that had to go was the one not written anywhere else.
                const wxString    chip_text   = chroma_card ? _L("ChromaStack") : _L("Relief");
                // Room for the icons after the chip: a relief card's cog, order arrows and cross, and a
                // ChromaStack card's cross alone - its cog's window opens from Fine-tune on the Picture step.
                const float right_w = (painted ? 0.f : ImGui::CalcTextSize(not_painted.c_str()).x + gap_s) +
                                      chip_width(chip_text) + gap_s +
                                      (chroma_card ? 1.f : 4.f) * frame_h + 3.f * gap_s;
                const float name_w  = std::max(frame_h, ImGui::GetWindowContentRegionMax().x - card_pad - ImGui::GetCursorPosX() - gap_s - right_w);
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(orca.x, orca.y, orca.z, 0.2f));
                ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(orca.x, orca.y, orca.z, 0.35f));
                ImGui::AlignTextToFramePadding();
                const std::string shown = ellipsize(layer_name(layer), name_w - 2.f * style.FramePadding.x);
                if (ImGui::Selectable((shown + "##name").c_str(), false, 0, ImVec2(name_w, frame_h))) {
                    if (chroma_card)
                        chroma_pick_picture(layer);
                    else
                        open_picker(layer.slot);
                }
                ImGui::PopStyleColor(2);
                hover_tip(chroma_card ? _u8L("Choose a different picture. The paint, the stack and the placement stay "
                                             "as they are.") :
                                        _u8L("Pick a different image for this layer. The paint, depth and tiling stay "
                                             "as they are."));
                ImGui::SameLine(0.f, gap_s);
                // There are two kinds and one of them is the current one, so the chip is the switch
                // rather than something that opens a switch: a menu of two items, one of them ticked,
                // is a click spent on being asked. Nothing is lost either way - a picture's stack
                // survives being a relief layer - so clicking again puts it back.
                if (chip(chip_text, chroma_card)) {
                    convert_slot      = layer.slot;
                    convert_to_chroma = !chroma_card;
                }
                if (ImGui::IsItemHovered())
                    panel_tooltip(chroma_card ?
                                      _u8L("A color picture made of filament swaps. Its height map is solved from the "
                                           "picture and the filaments below.\n\n"
                                           "Click to make it an ordinary relief layer. Its picture and filaments are "
                                           "kept, so clicking again brings them back.") :
                                      _u8L("A height map pushed into the surface. Tiles, depth, blending.\n\n"
                                           "Click to make it a ChromaStack picture: its texture becomes the picture a "
                                           "color stack is solved from."));

                if (!painted) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", not_painted.c_str());
                }
                // Between the name and the order arrows: it acts on the image the name refers to, so it
                // belongs with the name rather than with the controls that reorder the stack.
                //
                // Not on a ChromaStack card, whose picture step carries the filters and Fine-tune in its own
                // rows; and no order arrows there either, because a picture is always the top layer and has
                // nothing to be reordered past.
                if (!chroma_card) {
                    ImGui::SameLine();
                    m_imgui->disabled_begin(busy || !adjust_image_of(layer));
                    const std::string adjust_tip =
                        _u8L("Brightness, contrast, sharpness, blur, hue and saturation, shown on the image and on the "
                             "model as you set them. Save as copy writes the result to your texture library and points "
                             "this layer at it.");
                    if (icon_button(613 + layer.slot, "cog.svg", frame_h, _L("Adjust image"), adjust_tip))
                        open_image_adjust(layer);
                    m_imgui->disabled_end();

                    ImGui::SameLine();
                    m_imgui->disabled_begin(busy || li == 0);
                    if (icon_button(611, "texture_displacement_move_up.svg", frame_h, _L("Move up"), _L("Move up - applied earlier"))) {
                        move_slot = layer.slot;
                        move_to   = int(li) - 1;
                    }
                    m_imgui->disabled_end();
                    ImGui::SameLine();
                    m_imgui->disabled_begin(busy || li + 1 >= ordered.size());
                    if (icon_button(612, "texture_displacement_move_down.svg", frame_h, _L("Move down"), _L("Move down - applied later"))) {
                        move_slot = layer.slot;
                        move_to   = int(li) + 2;
                    }
                    m_imgui->disabled_end();
                }
                ImGui::SameLine();
                m_imgui->disabled_begin(busy);
                if (icon_button(600 + layer.slot, "texture_displacement_cross.svg", frame_h, m_desc.at("remove_layer"),
                                _L("Remove this layer")))
                    slot_to_remove = layer.slot;
                m_imgui->disabled_end();
                ImGui::PopStyleVar();
            }

            if (chroma_card) {
                // The four things the user decides, and nothing else: the picture and the print it
                // predicts, what is wrong, how many colours, and the filaments. The solve and the swaps
                // are not decisions - they follow the stack by themselves (see chroma_settle()).
                chroma_follow_face(layer);
                chroma_settle(layer);
                render_chroma_card(layer, card_pad, card_content_w);
            } else {
            // The three controls nearly every layer needs; everything else waits behind "More settings".
            // Depth and tile size are logarithmic, so both ends of their wide ranges stay usable.
            m_preview_params_dirty |= float_row("##depth", _L("Depth"), &layer.depth_mm, 0.01f, 10.f, "%.3f mm", true, card_pad,
                                                _L("Height of the relief: how far white in the texture lifts the surface, in "
                                                   "millimetres. Black does not move it at all, unless Midlevel says otherwise."),
                                                { { "depth.gif" } });
            // Mapped by the model's own UV layout there is no millimetre scale to set: the layout is the
            // placement (see TextureProjectionMethod::ModelUV), so the slider would move nothing. The row
            // states why rather than hiding: float_row()'s tooltip is shown for a disabled slider too.
            const bool tile_size_na = layer.projection_method == TextureProjectionMethod::ModelUV;
            m_imgui->disabled_begin(tile_size_na);
            m_preview_params_dirty |= float_row("##tile_size", _L("Tile size"), &layer.tiling_scale, 0.2f, 200.f, "%.2f mm", true, card_pad,
                                                tile_size_na ?
                                                    _L("Mapped by the model's own UV layout, which is already the placement - one "
                                                       "copy of the texture covers it exactly, so there is no tile size to set.") :
                                                    _L("How wide one copy of the texture is on the model. Smaller repeats the pattern "
                                                       "more often and makes its detail finer; with Tile off, this is the size of the "
                                                       "single copy."),
                                                tile_size_na ? std::vector<TipMedia>{} :
                                                               std::vector<TipMedia>{ { "tile_size_large.png", _L("Larger") },
                                                                                      { "tile_size_small.png", _L("Smaller") } });
            m_imgui->disabled_end();
            m_preview_params_dirty |= float_row("##rotation", _L("Rotation"), &layer.rotation_deg, 0.f, 360.f, "%.0f°", false, card_pad,
                                                _L("Turns the texture on the surface, in degrees - for lining a pattern up with an "
                                                   "edge of the model."),
                                                { { "rotation.gif" } });
            }

            // A ChromaStack card shows these rows - only its placement, for a picture painted onto somebody's
            // model - on its Plate step, rather than behind "More settings": the steps are how that card
            // opens and closes.
            const bool expanded_rows =
                chroma_card ? (layer.chroma_source && m_chroma_step[size_t(layer.slot)] == ChromaStepPlate) :
                              m_layer_expanded[size_t(layer.slot)];
            if (expanded_rows) {
                if (!chroma_card) {
                    // A dashed rule between the basic and the advanced settings.
                    const ImVec2 p    = ImGui::GetCursorScreenPos();
                    const float  x1   = content_rx - card_pad;
                    const float  dash = std::round(m_imgui->scaled(0.25f));
                    for (float xx = p.x; xx < x1; xx += 2.f * dash)
                        dl->AddLine(ImVec2(xx, p.y), ImVec2(std::min(xx + dash, x1), p.y), col_line);
                    ImGui::Dummy(ImVec2(1.f, 1.f));
                }

                // Everything from here to Mapping shapes a height map by hand. A ChromaStack picture's
                // height map is solved, and every one of these would move its steps off the print's layer
                // boundaries, so that card does not carry them at all.
                if (!chroma_card) {
                    // Midlevel: the height that means "don't move". At 0 the surface only ever bulges outwards;
                    // at 0.5 mid-grey is neutral and darker texels cut inwards.
                    m_preview_params_dirty |= float_row("##midlevel", _L("Midlevel"), &layer.midlevel, 0.f, 10.f, "%.2f", false, card_pad,
                                                        _L("Which gray stays where the surface already is. At 0 the texture only pushes "
                                                           "outwards; at 0.5 mid-gray stays put, so darker grays cut in and lighter ones "
                                                           "still push out - one image both embosses and engraves.\n\n"
                                                           "What cuts in has to fit: inside a sharp corner or through a thin wall, a deep "
                                                           "cut can pass through the other side."),
                                                        { { "midlevel.gif" } });
                    if (layer.midlevel > 0.f && layer.depth_mm > 1.f) {
                        ImGui::PushTextWrapPos(content_rx - card_pad);
                        m_imgui->warning_text(_L("Deep inward displacement may self-intersect."));
                        ImGui::PopTextWrapPos();
                    }

                    m_preview_params_dirty |= float_row("##smoothing", _L("Smoothing"), &layer.smoothing, 0.f, 1.f, "%.2f", false, card_pad,
                                                        _L("Blurs the image before it is used, which rounds off hard steps and removes "
                                                           "speckle from a noisy photo. Raise it if the relief looks harsh or grainy; it "
                                                           "costs fine detail."),
                                                        { { "smoothing_off.png", _L("Off") }, { "smoothing_on.png", _L("On") } });

                    // Edge fade: the relief flattens toward the boundary of the painted area.
                    m_preview_params_dirty |= m_imgui->bbl_checkbox(_L("Edge fade") + "##edge_smoothing", layer.edge_smoothing);
                    hover_tip(_u8L("Flattens the relief as it approaches the edge of the painted area, so it "
                                   "blends into the bare surface instead of stopping at a step."),
                              { { "edge_fade_off.png", _L("Off") }, { "edge_fade_on.png", _L("On") } });
                    ImGui::SameLine();
                    m_imgui->disabled_begin(!layer.edge_smoothing);
                    m_preview_params_dirty |= typed_slider("##edge_amount", -(card_pad + panel_edit_icon_room()),
                                                           &layer.edge_smoothing_amount, 0.02f, 1.f, "%.2f", false,
                                                           _L("How far in the fade reaches, as a share of the painted area. Small "
                                                              "values soften a narrow band at the edge; 1 flattens almost all of it."));
                    m_imgui->disabled_end();

                    // Invert and Colours share a row.
                    {
                        const float x0 = ImGui::GetCursorPosX();
                        m_preview_params_dirty |= m_imgui->bbl_checkbox(_L("Invert"), layer.invert);
                        hover_tip(_u8L("Turns the relief inside out: what stood out is cut in, and the other way "
                                       "round. The same as using a negative of the image."),
                                  { { "invert_off.png", _L("Off") }, { "invert_on.png", _L("On") } });
                        ImGui::SameLine();
                        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), x0 + 0.5f * (ImGui::GetWindowContentRegionMax().x - card_pad - x0)));

                        // Only offered for a texture that actually has colour - the shipped library is grayscale,
                        // and a checkbox that silently does nothing on nine textures out of ten is worse than no
                        // checkbox. Disabled rather than hidden so it is clear the feature exists and what it wants.
                        const bool has_color     = decode_height_texture(layer).has_color();
                        bool       color_enabled = layer.color_enabled && has_color;
                        m_imgui->disabled_begin(!has_color);
                        if (m_imgui->bbl_checkbox(_L("Colors"), color_enabled)) {
                            layer.color_enabled    = color_enabled;
                            m_preview_params_dirty = true;
                        }
                        m_imgui->disabled_end();
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                            panel_tooltip(has_color ?
                                              _u8L("Prints the painted area in the texture's colors as well as its "
                                                   "relief. Each color is matched to the nearest of your loaded "
                                                   "filaments; anything you did not paint keeps the object's own.") :
                                              _u8L("This texture is a grayscale height map, so it has no colors to "
                                                   "apply. Import a color image to use this."));

                        // The rest of colour belongs to the whole stack, not to this layer, so it only appears once -
                        // under whichever layer turned colour on.
                        if (color_enabled) {
                            TextureDisplacementOptions &opts = mv->texture_displacement_options;
                            if (m_imgui->bbl_checkbox(_L("Mix filaments"), opts.color_mix_enabled))
                                m_preview_params_dirty = true;
                            hover_tip(_u8L("Interleaves two filaments to fake the colors in between, so a handful "
                                           "of filaments can cover a photo or a gradient. An image of flat colors "
                                           "prints the same either way. Off uses one filament per area."),
                                      { { "mix_off.png", _L("Off") }, { "mix_on.png", _L("On") } });
                            if (opts.color_mix_enabled) {
                                cached_palette(); // brings m_palette_filaments up to date
                                // Every mix can become a filament slot, and there are only so many beside the
                                // physical filaments.
                                const int max_mixes = std::max(1, int(EnforcerBlockerType::ExtruderMax) - int(m_palette_filaments.size()));
                                if (int_row("##color_mix_count", _L("Mixed colors"), &opts.color_mix_count, 1, max_mixes, "%d", card_pad,
                                            _L("The most mixed filaments a bake may add. They are picked from the "
                                               "texture's colors, and only the ones the bake actually uses are created."),
                                            { { "mixed_colors_few.png", _L("Few") }, { "mixed_colors_many.png", _L("Many") } }))
                                    m_preview_params_dirty = true;
                                // After the slider, so a change shows in the same frame.
                                const int n_colors    = int(cached_palette().size());
                                const int n_filaments = int(m_palette_filaments.size());
                                // TRN The %2% part of "%1% printable colors %2%"; %1% is the number of filaments.
                                const std::string from_filaments = Slic3r::format(into_u8(_L_PLURAL("from %1% filament", "from %1% filaments", n_filaments)), n_filaments);
                                // TRN %1% is the number of printable colors, %2% is "from N filaments".
                                ImGui::TextDisabled("%s", Slic3r::format(into_u8(_L_PLURAL("%1% printable color %2%", "%1% printable colors %2%", n_colors)),
                                                                         n_colors, from_filaments).c_str());
                            }
                            if (int_row("##color_despeckle", _L("Denoise"), &opts.color_despeckle, 0, 6, "%d", card_pad,
                                        _L("Cleans up single stray triangles of the wrong color, which detail finer "
                                           "than the mesh leaves behind. Raise it if the result looks speckled, "
                                           "lower it if small features are being swallowed."),
                                        { { "denoise_off.png", _L("Off") }, { "denoise_on.png", _L("On") } }))
                                m_preview_params_dirty = true;
                        }
                }
            }

            // Where the picture sits on the model: its size there, its angle, how it is laid on, and the
            // handle that moves it. A ChromaStack plate pins all of that for itself - the picture is
            // the plate, and the plate was built around the picture - so for one of those the block is
            // absent rather than merely closed. A control that cannot act is worse than one that is
            // not there, because the reader has to work out which it is.
            const bool picture_is_the_plate = chroma_card && layer.chroma.plate.generated;
            if (!picture_is_the_plate) {
            if (chroma_card) {
                m_preview_params_dirty |= float_row("##tile_size", _L("Size on the model"), &layer.tiling_scale, 0.2f,
                                                    500.f, "%.2f mm", true, card_pad,
                                                    _L("How wide the picture is on the model. It is set to cover the "
                                                       "painted face when the picture is chosen; change it to crop in "
                                                       "or to leave a margin."),
                                                    { { "tile_size_large.png", _L("Larger") },
                                                      { "tile_size_small.png", _L("Smaller") } });
                m_preview_params_dirty |= float_row("##rotation", _L("Rotation"), &layer.rotation_deg, 0.f, 360.f,
                                                    "%.0f°", false, card_pad,
                                                    _L("Turns the picture on the surface, in degrees."),
                                                    { { "rotation.gif" } });
            }

                // Mapping, as one icon per TextureProjectionMethod, in the enum's own order. Not for a
                // picture: a ChromaStack solve reads the height of a column against one plane, which is
                // what every one of its own warnings about a patch that is not level is about, so the
                // choice between a sphere and a cylinder is a choice between two things it cannot do.
                if (!chroma_card) {
                {
                    const float x0 = ImGui::GetCursorPosX();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("%s", _u8L_CONTEXT("Mapping", "Texture Displacement").c_str());
                    ImGui::SameLine();
                    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), x0 + label_w));
                    struct MappingIcon
                    {
                        const char *file;
                        wxString    label;
                        wxString    tip;
                        const char *picture; // the same ball with the checker laid on it this way, or null for none
                    };
                    const MappingIcon mappings[] = {
                        { "menu_obj_cube.svg", _L("Triplanar (blended)"),
                          _L("Triplanar - projects the texture from all three axes at once and blends between them, so a "
                             "patch wrapping around a sharp edge has no seam."),
                          "map_cube.png" },
                        { "menu_obj_cylinder.svg", _L("Cylindrical"),
                          _L("Cylindrical - wraps the texture around the painted area's own center, for round shapes."),
                          "map_cylinder.png" },
                        { "menu_obj_sphere.svg", _L("Spherical"),
                          _L("Spherical - wraps the texture around the painted area's own center in both directions."),
                          "map_sphere.png" },
                        { "texture_displacement_map_unwrap.svg", _L("Unwrap (LSCM)"),
                          _L("Unwrap - flattens the painted area and maps the texture onto it with as little stretching as "
                             "possible. The area is cut into pieces at its sharp edges first (see Seam angle), so each "
                             "piece can lie flat on its own."),
                          "map_unwrap.png" },
                        { "texture_displacement_map_view.svg", _L("From view"),
                          _L("From view - projects straight onto the painted area from where you are looking, like a slide "
                             "projector."),
                          "map_view.png" },
                        { "texture_displacement_map_model_uv.svg", _L("Model UV"),
                          _L("Model UV - uses the texture coordinates the model itself came with, so a texture made for "
                             "that model lands exactly where it was painted. Nothing is unwrapped, and Tile size does "
                             "not apply: the model's own layout is the placement."),
                          nullptr },
                    };
                    // Only a volume that actually carries a UV channel can be mapped by it. Shown
                    // greyed out rather than hidden, so the reason is visible instead of the control
                    // simply being missing.
                    const wxString model_uv_na =
                        (mv != nullptr && mv->has_texture_uv()) ?
                            wxString() :
                            _L("This model carries no texture coordinates of its own. Only a model imported with a UV "
                               "channel - a textured OBJ, say - can be mapped by them.");
                    for (int mi = 0; mi < int(IM_ARRAYSIZE(mappings)); ++mi) {
                        if (mi > 0)
                            ImGui::SameLine(0.f, gap_s);
                        const auto method = static_cast<TextureProjectionMethod>(mi);
                        const wxString na = (method == TextureProjectionMethod::ModelUV) ? model_uv_na : wxString();
                        const std::vector<TipMedia> picture = mappings[mi].picture != nullptr ?
                                                                  std::vector<TipMedia>{ { mappings[mi].picture } } :
                                                                  std::vector<TipMedia>{};
                        if (icon_toggle(830 + mi, mappings[mi].file, layer.projection_method == method, icon_sm, mappings[mi].label,
                                        mappings[mi].tip, picture, na) &&
                            layer.projection_method != method) {
                            // Capture the current view the moment "From view" is chosen, so it does something
                            // sensible immediately rather than projecting from a stale/default direction.
                            if (method == TextureProjectionMethod::ViewProjected)
                                capture_view_projection(layer);
                            layer.projection_method = method;
                            m_preview_params_dirty  = true;
                            // Unwrap and its tools live in the UV editor pane, so picking it opens the pane
                            // (via the preview rebuild this triggers).
                            if (method == TextureProjectionMethod::LSCM)
                                m_show_uv_editor = true;
                        }
                    }
                }
                }

                if (layer.projection_method == TextureProjectionMethod::LSCM) {
                    // Unwrap and everything that edits it live in the UV editor pane, next to the islands they change.
                    // Picking this mapping opens the pane; this brings it back after it has been closed.
                    if (m_imgui->button(_u8L("Open UV editor"))) {
                        m_show_uv_editor = true;
                        update_uv_editor();
                    }
                    hover_tip(_u8L("Opens the UV editor, where you can see how the texture is laid out over the "
                                    "painted area, cut seams and move the pieces around."));
                    ImGui::SameLine();
                    ImGui::AlignTextToFramePadding();
                    if (m_uv_editor_unwrap.empty())
                        ImGui::TextDisabled("%s", _u8L("Not unwrapped yet").c_str());
                    else
                        ImGui::TextDisabled("%s", Slic3r::format(into_u8(_L_PLURAL("%1% island", "%1% islands", m_uv_editor_unwrap.chart_count)),
                                                                 m_uv_editor_unwrap.chart_count).c_str());
                }

                if (layer.projection_method == TextureProjectionMethod::ViewProjected) {
                    // Re-capture the projector from wherever the camera is now: orbit the model, press this, and
                    // the texture is re-laid from the new angle.
                    if (m_imgui->button(_u8L("Capture current view"))) {
                        capture_view_projection(layer);
                        // Selecting the visible faces *after* capturing means the projector axes are already the
                        // ones the selection was made against - the two describe the same viewpoint.
                        if (m_project_only_visible && select_visible_faces() == 0)
                            show_error(nullptr, _u8L("Nothing is visible from this angle - turn the model to face the "
                                                     "part you want to project onto."));
                        m_preview_params_dirty = true;
                        update_projector();
                    }
                    hover_tip(_u8L("Projects the texture onto the painted area from where you are looking now, like "
                                    "a slide projector. Faces turned away from you stretch, so line the view up with "
                                    "the surface you care about first."));

                    if (m_imgui->bbl_checkbox(_L("Project only on visible"), m_project_only_visible)) {
                        if (m_project_only_visible && select_visible_faces() == 0)
                            show_error(nullptr, _u8L("Nothing is visible from this angle - turn the model to face the "
                                                     "part you want to project onto."));
                        m_preview_params_dirty = true;
                        update_projector();
                    }
                    hover_tip(_u8L("Paints only the faces you can actually see right now - facing you and not hidden "
                                    "behind anything - and projects onto those. Replaces what the layer had painted."));

                    bool projector_open = m_projector_frame != nullptr && m_projector_frame->IsShown();
                    if (m_imgui->bbl_checkbox(_L("Projection frame"), projector_open))
                        show_projector(projector_open);
                    hover_tip(_u8L("Opens a window you drag over the model. Whatever you can see through it is what "
                                    "gets the texture, and its border becomes the edge of the projection."));

                    if (projector_open) {
                        if (int_row("##projector_opacity", _L("Opacity"), &m_projector_opacity, 20, 255, "%d", card_pad,
                                    _L("How solid the frame window looks while you place it. Lower to see the "
                                       "model through it; it does not affect the result.")))
                            m_projector_frame->set_opacity(m_projector_opacity);

                        if (m_imgui->button(_u8L("Apply projection frame"))) {
                            const int painted_count = apply_projection_frame();
                            if (painted_count == 0)
                                show_error(nullptr, _u8L("Nothing of the model is inside the frame - move it over the "
                                                         "part you want to project onto."));
                            else if (painted_count < 0)
                                show_error(nullptr, _u8L("The frame could not be applied. Make sure it overlaps the 3D view."));
                        }
                        hover_tip(_u8L("Projects the texture through the frame from where you are looking now and "
                                        "paints the faces inside it. Replaces what the layer had painted; the result "
                                        "sticks to the model, so you can orbit afterwards."));
                    }

                    if (layer.view_project_projective) {
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextDisabled("%s", _u8L("Placed by projection frame.").c_str());
                        ImGui::SameLine();
                        if (m_imgui->button(_u8L("Clear"))) {
                            // Back to the plain axis projection, where tiling/rotation/offset mean something again -
                            // the matrix path deliberately ignores them.
                            layer.view_project_projective = false;
                            layer.tile_enabled            = true;
                            m_preview_params_dirty        = true;
                        }
                    }
                }

                // Tile, and how it repeats. A picture is one copy, never a tile.
                if (!chroma_card) {
                    const float x0 = ImGui::GetCursorPosX();
                    m_preview_params_dirty |= m_imgui->bbl_checkbox(_L("Tile") + "##tile_enabled", layer.tile_enabled);
                    hover_tip(_u8L("Repeats the texture across the painted area. Off places one copy, like a decal, "
                                    "at the size set by Tile size."),
                              { { "tile_off.png", _L("Off") }, { "tile_on.png", _L("On") } });
                    ImGui::SameLine();
                    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), x0 + label_w));
                    const wxString tile_na = layer.tile_enabled ? wxString() : _L("Turn Tile on to choose how the texture repeats.");
                    if (icon_toggle(840, "texture_displacement_tile_repeat.svg", layer.tile_method == static_cast<TextureTileMethod>(0),
                                    icon_sm, _L("Repeat"), _L("Repeat"), { { "tile_repeat.png" } }, tile_na)) {
                        layer.tile_method      = static_cast<TextureTileMethod>(0);
                        m_preview_params_dirty = true;
                    }
                    ImGui::SameLine(0.f, gap_s);
                    if (icon_toggle(841, "menu_mirror_x.svg", layer.tile_method == static_cast<TextureTileMethod>(1), icon_sm,
                                    _L("Mirrored repeat"), _L("Mirrored repeat - every other tile is flipped, so edges meet seamlessly"),
                                    { { "tile_mirror.png" } }, tile_na)) {
                        layer.tile_method      = static_cast<TextureTileMethod>(1);
                        m_preview_params_dirty = true;
                    }
                }

                // Blend, and placement on the model. A picture replaces what is under it rather than
                // blending into it - it is the top layer by rule - so it keeps the placement and not the mode.
                {
                    const bool blend_row = !chroma_card;
                    const float x0 = ImGui::GetCursorPosX();
                    if (blend_row) {
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("%s", _u8L("Blend").c_str());
                    ImGui::SameLine();
                    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), x0 + label_w));
                    const std::string blend_add      = _u8L_CONTEXT("Add", "Texture Displacement");
                    const std::string blend_override  = _u8L_CONTEXT("Override", "Texture Displacement");
                    const std::string blend_subtract = _u8L_CONTEXT("Subtract", "Texture Displacement");
                    const std::string blend_multiply = _u8L_CONTEXT("Multiply", "Texture Displacement");
                    const std::string blend_divide   = _u8L("Divide");
                    const char *blend_items[] = { blend_add.c_str(), blend_override.c_str(), blend_subtract.c_str(),
                                                  blend_multiply.c_str(), blend_divide.c_str() };
                    // The combo's order is the panel's, not the enum's - Override reads as the sibling of Add and
                    // belongs next to it, while TextureBlendMode's numbering is fixed by what projects already
                    // store. Hence the explicit mapping rather than a cast of the index.
                    static const TextureBlendMode blend_modes[] = { TextureBlendMode::Add, TextureBlendMode::Override,
                                                                    TextureBlendMode::Subtract, TextureBlendMode::Multiply,
                                                                    TextureBlendMode::Divide };
                    static_assert(IM_ARRAYSIZE(blend_items) == IM_ARRAYSIZE(blend_modes),
                                  "every Blend entry needs its mode");
                    // The first layer has nothing before it to combine with - build_texture_displacement() makes it
                    // add regardless - so its combo shows Add and cannot be changed.
                    const bool base_layer = li == 0;
                    int        blend_mode = 0;
                    if (!base_layer)
                        for (int bi = 0; bi < int(IM_ARRAYSIZE(blend_modes)); ++bi)
                            if (blend_modes[bi] == layer.blend_mode)
                                blend_mode = bi;
                    ImGui::SetNextItemWidth(-(card_pad + gap_s + icon_sm));
                    m_imgui->disabled_begin(base_layer);
                    if (scoped_combo("##blend_mode", &blend_mode, blend_items, IM_ARRAYSIZE(blend_items)) && !base_layer &&
                        blend_mode >= 0 && blend_mode < int(IM_ARRAYSIZE(blend_modes))) {
                        layer.blend_mode       = blend_modes[blend_mode];
                        m_preview_params_dirty = true;
                    }
                    m_imgui->disabled_end();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        panel_tooltip(base_layer ?
                                          _u8L("The base layer has nothing beneath it to combine with, so it always adds.") :
                                          _u8L("What this layer does where it overlaps the ones below. Add and "
                                               "Subtract pile relief on or carve it away. Override replaces them, "
                                               "so only this layer's relief is left where it is painted. Multiply "
                                               "and Divide scale what is underneath, which turns this layer into a "
                                               "mask over it - there, Depth acts as a strength, and 1 mm leaves "
                                               "white areas untouched."),
                                      base_layer ? std::vector<TipMedia>{} :
                                                   std::vector<TipMedia>{ { "blend_add.png", from_u8(blend_add) },
                                                                          { "blend_multiply.png", from_u8(blend_multiply) } });

                    ImGui::SameLine(0.f, gap_s);
                    } else {
                        // No blend mode on a picture, but the placement handle still belongs to it, so the
                        // row keeps its label column and loses only the combo.
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextDisabled("%s", _u8L("Move the picture").c_str());
                        ImGui::SameLine();
                        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), x0 + label_w));
                    }
                    if (icon_toggle(842, "texture_displacement_adjust.svg", m_adjust_texture_mode, icon_sm, _L("Move the picture by hand"),
                                    _L("Move the picture by hand - drag the handle on the model to move it"))) {
                        if (!m_adjust_texture_mode) {
                            update_model_object(); // flush any pending strokes before anchoring
                            if (update_adjust_anchor())
                                m_adjust_texture_mode = true;
                            else
                                show_error(nullptr, _u8L("Paint something with this layer first."));
                        } else {
                            m_adjust_texture_mode = false;
                            m_adjust_drag_handle  = AdjustHandle::None;
                        }
                    }
                }
            }
            } // picture_is_the_plate

            // "More settings" / "Fewer settings", drawn as a link. Not on a ChromaStack card, whose tab row is
            // how it opens and closes.
            if (!chroma_card) {
                const bool        expanded = m_layer_expanded[size_t(layer.slot)];
                const std::string text     = expanded ? _u8L("Fewer settings") : _u8L("More settings");
                const float       arrow    = std::round(ImGui::GetFontSize() * 0.55f);
                const ImVec2      ts       = ImGui::CalcTextSize(text.c_str());
                const ImVec2      p        = ImGui::GetCursorScreenPos();
                if (ImGui::InvisibleButton("##more", ImVec2(arrow + gap_s + ts.x, ts.y))) {
                    m_layer_expanded[size_t(layer.slot)] = !expanded;
                    // Placement is one of the settings being hidden, so it stops with them.
                    if (expanded && m_adjust_texture_mode) {
                        m_adjust_texture_mode = false;
                        m_adjust_drag_handle  = AdjustHandle::None;
                    }
                }
                const ImU32 c  = ImGui::GetColorU32(col_link);
                const float cx = p.x + 0.5f * arrow, cy = p.y + 0.5f * ts.y, hs = 0.3f * arrow;
                if (expanded)
                    dl->AddTriangleFilled(ImVec2(cx - hs, cy + 0.5f * hs), ImVec2(cx + hs, cy + 0.5f * hs), ImVec2(cx, cy - 0.5f * hs), c);
                else
                    dl->AddTriangleFilled(ImVec2(cx - hs, cy - 0.5f * hs), ImVec2(cx + hs, cy - 0.5f * hs), ImVec2(cx, cy + 0.5f * hs), c);
                const float tx = p.x + arrow + gap_s;
                dl->AddText(ImVec2(tx, p.y), c, text.c_str());
                if (ImGui::IsItemHovered())
                    dl->AddLine(ImVec2(tx, p.y + ts.y), ImVec2(tx + ts.x, p.y + ts.y), c);
            }

            ImGui::EndGroup();
            ImGui::Unindent(card_pad);
            const float card_max_y = ImGui::GetItemRectMax().y + card_pad;
            drop_target(li, layer.slot, card_min, ImVec2(content_rx, card_max_y));
            if (layer.slot == dragged_slot)
                dl->AddRectFilled(card_min, ImVec2(content_rx, card_max_y), ghost_col, rounding);
            dl->ChannelsSetCurrent(0);
            dl->AddRectFilled(card_min, ImVec2(content_rx, card_max_y), col_card, rounding);
            dl->ChannelsMerge();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + card_pad);
        } else {
            // A closed layer: one outlined row - grip, chevron, thumbnail, name, depth, remove. A click
            // anywhere that is not one of its buttons opens it.
            const float  inner = std::round(m_imgui->scaled(0.15f));
            const float  row_h = frame_h + 2.f * inner;
            const ImVec2 p0    = ImGui::GetCursorScreenPos();
            const float  row_w = content_rx - p0.x;
            if (ImGui::InvisibleButton("##row", ImVec2(row_w, row_h)))
                activate_slot = layer.slot;
            const bool row_hovered = ImGui::IsItemHovered();
            ImGui::SetItemAllowOverlap(); // the row's own buttons, submitted after it, still take their clicks
            drop_target(li, layer.slot, p0, ImVec2(p0.x + row_w, p0.y + row_h));
            if (row_hovered)
                dl->AddRectFilled(p0, ImVec2(p0.x + row_w, p0.y + row_h), col_card, rounding);
            dl->AddRect(p0, ImVec2(p0.x + row_w, p0.y + row_h), col_line, rounding);

            ImGui::SetCursorScreenPos(ImVec2(p0.x + inner, p0.y + inner));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap_s, style.ItemSpacing.y));
            grip(layer, frame_h);
            ImGui::SameLine();
            {
                const ImVec2 c  = ImGui::GetCursorScreenPos();
                const float  aw = std::round(ImGui::GetFontSize() * 0.5f), hs = 0.3f * aw;
                const float  cx = c.x + 0.5f * aw, cy = c.y + 0.5f * frame_h;
                dl->AddTriangleFilled(ImVec2(cx - 0.5f * hs, cy - hs), ImVec2(cx - 0.5f * hs, cy + hs), ImVec2(cx + 0.5f * hs, cy),
                                      ImGui::GetColorU32(ImGuiCol_TextDisabled));
                ImGui::Dummy(ImVec2(aw, frame_h));
            }
            ImGui::SameLine();
            const float th = std::round(frame_h * 0.9f);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 0.5f * (frame_h - th));
            if (GLTexture *thumb = get_layer_thumbnail(layer)) {
                if (ImGui::ImageButton((ImTextureID) (intptr_t) thumb->get_id(), ImVec2(th, th), ImVec2(0.f, 0.f), ImVec2(1.f, 1.f), 0))
                    open_picker(layer.slot);
            } else if (ImGui::Button("?##thumb", ImVec2(th, th))) {
                open_picker(layer.slot);
            }
            hover_tip(_u8L("Pick a different image for this layer. The paint, depth and tiling stay as they are."));
            ImGui::SameLine();

            char depth_text[32];
            std::snprintf(depth_text, sizeof(depth_text), "%.3f mm", layer.depth_mm);
            const std::string summary    = painted ? std::string(depth_text) : _u8L("not painted");
            const float       sum_w      = ImGui::CalcTextSize(summary.c_str()).x;
            const float       right_edge = ImGui::GetWindowContentRegionMax().x - inner;
            const float       name_w     = std::max(0.f, right_edge - ImGui::GetCursorPosX() - 2.f * gap_s - sum_w - frame_h);
            ImGui::SetCursorPosY(p0.y - ImGui::GetWindowPos().y + ImGui::GetScrollY() + inner);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(ellipsize(layer_name(layer), name_w).c_str());
            ImGui::SameLine();
            ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), right_edge - frame_h - gap_s - sum_w));
            ImGui::TextDisabled("%s", summary.c_str());
            ImGui::SameLine();
            ImGui::SetCursorPosX(right_edge - frame_h);
            m_imgui->disabled_begin(busy);
            if (icon_button(600 + layer.slot, "texture_displacement_cross.svg", frame_h, m_desc.at("remove_layer"), _L("Remove this layer")))
                slot_to_remove = layer.slot;
            m_imgui->disabled_end();
            ImGui::PopStyleVar();
            if (layer.slot == dragged_slot)
                dl->AddRectFilled(p0, ImVec2(p0.x + row_w, p0.y + row_h), ghost_col, rounding);
            ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + row_h + 0.5f * style.ItemSpacing.y));
        }

        ImGui::PopID();
    }

    // The dragged layer follows the cursor as a copy of its row, held by its grip. On the foreground draw list, so
    // it stays visible over the rest of the panel and anywhere the drag wanders.
    if (dragged_slot >= 0) {
        const auto it = std::find_if(ordered.begin(), ordered.end(), [dragged_slot](const auto *l) { return l->slot == dragged_slot; });
        if (it != ordered.end()) {
            const TextureDisplacementLayer &l      = **it;
            ImDrawList                     *fg     = ImGui::GetForegroundDrawList();
            const float                     inner  = std::round(m_imgui->scaled(0.15f));
            const float                     row_h  = frame_h + 2.f * inner;
            const float                     list_x = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMin().x;
            const ImVec2                    mouse  = ImGui::GetIO().MousePos;
            const ImVec2                    mn(mouse.x - inner - 0.5f * grip_w, mouse.y - 0.5f * row_h);
            const ImVec2                    mx(mn.x + (content_rx - list_x), mn.y + row_h);
            fg->AddRectFilled(ImVec2(mn.x + 2.f, mn.y + 4.f), ImVec2(mx.x + 2.f, mx.y + 4.f), IM_COL32(0, 0, 0, 70), rounding);
            fg->AddRectFilled(mn, mx, ImGui::GetColorU32(ImGuiCol_WindowBg, 245.f / 255.f), rounding);
            fg->AddRect(mn, mx, ImGui::GetColorU32(orca), rounding, 0, 1.5f);

            float x = mn.x + inner;
            draw_drag_icon(fg, x, mn.y + inner, grip_w, frame_h, drag_tint(255));
            x += grip_w + gap_s;
            const float th = std::round(frame_h * 0.9f);
            if (GLTexture *thumb = get_layer_thumbnail(l)) {
                const float ty = mn.y + 0.5f * (row_h - th);
                fg->AddImage((ImTextureID) (intptr_t) thumb->get_id(), ImVec2(x, ty), ImVec2(x + th, ty + th));
            }
            x += th + gap_s;

            char depth_text[32];
            std::snprintf(depth_text, sizeof(depth_text), "%.3f mm", l.depth_mm);
            const std::string summary = slot_painted(l.slot) ? std::string(depth_text) : _u8L("not painted");
            const float       sum_w   = ImGui::CalcTextSize(summary.c_str()).x;
            const float       text_y  = mn.y + 0.5f * (row_h - ImGui::GetFontSize());
            fg->AddText(ImVec2(mx.x - inner - sum_w, text_y), ImGui::GetColorU32(ImGuiCol_TextDisabled), summary.c_str());
            const std::string name = ellipsize(layer_name(l), std::max(0.f, mx.x - inner - sum_w - gap_s - x));
            fg->AddText(ImVec2(x, text_y), ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
        }
    }

    // ---- Mesh preparation: Pro only. Standard pins all of it and runs it from Bake. ----
    if (pro_mode()) {
        ImGui::Separator();
        heading(_L("Subdivision"));

        if (m_imgui->bbl_checkbox(_L("Only painted area (adaptive)"), m_subdivide_adaptive)) {
            if (m_subdivide_editing)
                rebuild_subdivide_preview(); // switch the wireframe between the uniform and adaptive result
            m_parent.set_as_dirty();
        }
        hover_tip(_u8L("Adds triangles only where you painted, instead of everywhere. On a large part with a small "
                        "decal that is the difference between thousands of triangles and millions, and your paint "
                        "survives the refinement."),
                  { { "painted_only_off.png", _L("Off") }, { "painted_only_on.png", _L("On") } });

        if (m_subdivide_adaptive) {
            if (m_subdivide_target_mm <= 0.f && mv != nullptr) {
                // Seed the target at about half the mesh's mean edge length, so the default already adds a
                // useful amount of detail rather than landing on "no change".
                const indexed_triangle_set &its = mv->mesh().its;
                double                      sum = 0.0;
                size_t                      cnt = 0;
                for (const stl_triangle_vertex_indices &tri : its.indices)
                    for (int i = 0; i < 3; ++i) {
                        sum += (its.vertices[tri[i]] - its.vertices[tri[(i + 1) % 3]]).norm();
                        ++cnt;
                    }
                m_subdivide_target_mm = cnt > 0 ? std::clamp(float(sum / double(cnt)) * 0.5f, 0.001f, 20.f) : 1.f;
            }
            // Live preview: rebuild the wireframe as the slider moves, not only on release. The rebuild is
            // bounded by the painted region, so it stays responsive.
            const auto preview_live = [this]() {
                if (m_subdivide_editing)
                    rebuild_subdivide_preview();
                m_parent.set_as_dirty();
            };

            if (m_imgui->bbl_checkbox(_L("Follow texture detail"), m_subdivide_feature))
                preview_live();
            hover_tip(_u8L("Spends the triangles where the texture actually bends - packed along ridges and edges, "
                            "sparse over flat ground - instead of spreading them evenly. The same detail for fewer "
                            "triangles on most textures."),
                      { { "adaptive_off.png", _L("Off") }, { "adaptive_on.png", _L("On") } });

            // The edge-length target is a baseline in both modes. In feature mode it is what guarantees the
            // curvature test can actually see the texture: left too coarse, a big triangle over a fine
            // pattern can sample four points that all happen to land at similar heights, report no error,
            // and stall before refinement ever starts.
            if (float_row("##subdiv_target", m_subdivide_feature ? _L("Max edge") : _L("Target edge"), &m_subdivide_target_mm,
                          0.001f, 20.f, "%.3f mm", true, 0.f,
                          m_subdivide_feature ?
                              _L("No triangle in the painted area stays larger than this, even where the texture "
                                 "is flat. Keep it near the size of the smallest feature you want to come out.") :
                              _L("Triangles in the painted area are split until none is larger than this. Smaller "
                                 "means finer detail and, fast, a lot more triangles.")))
                preview_live();

            if (m_subdivide_feature) {
                if (float_row("##subdiv_detail", _L("Detail"), &m_subdivide_detail_mm, 0.001f, 1.f, "%.3f mm", true, 0.f,
                              _L("How far the mesh may sit from the shape the texture describes, in millimetres. "
                                 "Smaller follows fine detail and costs triangles; larger only chases the big "
                                 "features.")))
                    preview_live();
                if (float_row("##subdiv_min", _L("Min edge"), &m_subdivide_min_edge_mm, 0.001f, 20.f, "%.3f mm", true, 0.f,
                              _L("A floor on triangle size, so a sharp step in the texture cannot be chased "
                                 "forever. Lower it for finer relief, raise it if the count runs away at hard "
                                 "edges.")))
                    preview_live();
            }

            // Applies in both adaptive sub-modes: it is about the step the bake puts at the paint's edge, which
            // exists whether or not "Follow texture detail" is on. 0 turns the band off.
            if (float_row("##subdiv_border", _L("Edge detail"), &m_subdivide_border_mm, 0.f, 5.f, "%.3f mm", false, 0.f,
                          _L("Triangle size along the outline of the painted area, where the relief drops back to the "
                             "bare surface. Lower it if that rim looks jagged; it only costs triangles along the "
                             "outline. 0 turns it off.")))
                preview_live();

            // Only worth showing when a layer is actually colouring: with no colour there is no boundary for it
            // to refine and the control would do nothing whatever it is set to.
            if (mv != nullptr && any_layer_colors(*mv)) {
                if (float_row("##subdiv_color", _L("Color detail"), &m_subdivide_color_mm, 0.f, 5.f, "%.3f mm", false, 0.f,
                              _L("Triangle size where two colors meet. Each triangle prints in one filament, so a "
                                 "color edge can only be as sharp as the triangles along it - and nothing else "
                                 "refines there, since the surface is flat across a change of color. 0 turns it off.")))
                    preview_live();
            }

            ImGui::TextDisabled("%s", _u8L("Triangle budget: set with Triangles, below.").c_str());
            if (m_subdivide_editing && m_subdivide_preview_tris > 0)
                m_imgui->text(Slic3r::format(into_u8(_L_PLURAL("Preview: %1% triangle", "Preview: %1% triangles", m_subdivide_preview_tris)), m_subdivide_preview_tris));
        } else {
            if (int_row("##subdiv_steps", _L("Steps"), &m_subdivide_count, 0, 5, "%d", 0.f,
                        _L("Splits every triangle of the model into four, this many times over. Each step "
                           "quadruples the count - 3 turns 100 k triangles into 6.4 M - so prefer refining "
                           "only the painted area unless you need the whole model finer."),
                        { { "subdivide.gif" } })) {
                if (m_subdivide_editing)
                    rebuild_subdivide_preview(); // a count of 0 clears the preview, it doesn't compute one
                m_parent.set_as_dirty();
            }
        }

        {
            // Subdivide is a no-op when there is nothing to commit: 0 uniform passes, or an adaptive target that
            // is not set.
            const bool  subdivide_ready = m_subdivide_adaptive ? (m_subdivide_target_mm > 0.f) : (m_subdivide_count >= 1);
            const float half            = std::floor((ImGui::GetContentRegionAvail().x - style.ItemSpacing.x) * 0.5f);
            m_imgui->disabled_begin(busy);
            if (ImGui::Button((m_subdivide_editing ? _u8L("Hide preview") : _u8L("Preview subdivision")).c_str(), ImVec2(half, 0.f))) {
                if (m_subdivide_editing) {
                    m_subdivide_editing      = false;
                    m_subdivide_preview_tris = -1;
                    m_subdivide_preview_glmodel.reset();
                } else {
                    m_subdivide_editing = true;
                    rebuild_subdivide_preview();
                }
                m_parent.set_as_dirty();
            }
            m_imgui->disabled_end();
            hover_tip(_u8L("Shows what the refinement would produce as a wireframe, without touching the model."));
            ImGui::SameLine();
            m_imgui->disabled_begin(!subdivide_ready || busy);
            if (ImGui::Button(_u8L("Subdivide").c_str(), ImVec2(half, 0.f))) {
                if (m_subdivide_adaptive) {
                    subdivide_model_adaptive(); // refines only the painted area, carrying the paint forward
                } else {
                    subdivide_model(); // commits m_subdivide_count passes (takes its own snapshot)
                    // Back to 0 rather than staying at the count just applied: the mesh is now up to 4^N times
                    // denser, so re-previewing the same N passes on top of it is both pointless and by far the
                    // slowest thing this panel does.
                    m_subdivide_count = 0;
                }
                if (m_subdivide_editing)
                    rebuild_subdivide_preview();
                m_parent.set_as_dirty();
            }
            m_imgui->disabled_end();
            hover_tip(m_subdivide_adaptive ?
                          _u8L("Refines the painted area to the target edge length and carries your paint onto "
                               "the finer mesh. The rest of the model is left as it is.") :
                          _u8L("Replaces the model's geometry with the subdivided mesh and clears any not-yet-baked "
                               "paint on it (already-baked relief is unaffected)."));
        }

        // Remesh: even out uneven triangle sizes (CGAL isotropic remeshing).
        ImGui::Separator();
        heading(_L("Remeshing"));
        if (m_remesh_target_edge_mm <= 0.f && mv != nullptr) {
            // Seed the target with the model's current mean edge length, so the default is a sensible "make
            // everything about the size it already averages".
            const indexed_triangle_set &its = mv->mesh().its;
            double                      sum = 0.0;
            size_t                      cnt = 0;
            for (const stl_triangle_vertex_indices &tri : its.indices)
                for (int i = 0; i < 3; ++i) {
                    sum += (its.vertices[tri[i]] - its.vertices[tri[(i + 1) % 3]]).norm();
                    ++cnt;
                }
            m_remesh_target_edge_mm = cnt > 0 ? std::clamp(float(sum / double(cnt)), 0.1f, 20.f) : 1.f;
        }
        float_row("##remesh_edge", _L("Target edge"), &m_remesh_target_edge_mm, 0.1f, 20.f, "%.2f mm", true, 0.f,
                  _L("Triangle size the whole model is rebuilt with, in millimetres. Displacement works best on an "
                     "even mesh; this is what makes one out of an uneven import."),
                  { { "remesh_before.png", _L("Before") }, { "remesh_after.png", _L("After") } });

        m_imgui->bbl_checkbox(_L("Keep sharp edges") + "##remesh_sharp", m_remesh_keep_sharp_edges);
        hover_tip(_u8L("Holds hard edges and open borders in place while the rest is remeshed. Without it "
                       "the remesher slides vertices along the surface and rounds every crisp edge off - "
                       "a cube comes back with wobbly edges."),
                  { { "preserve_edges_off.png", _L("Off") }, { "preserve_edges_on.png", _L("On") } });
        ImGui::SameLine();
        m_imgui->disabled_begin(!m_remesh_keep_sharp_edges);
        typed_slider("##remesh_sharp_angle", -(1.f + panel_edit_icon_room()), &m_remesh_sharp_angle_deg, 5.f, 90.f, "%.0f°", false,
                     _L("How sharp a fold has to be, in degrees, before the remesher treats it as an edge worth "
                        "keeping. Lower protects more of the model's shape; higher rebuilds more of it evenly."));
        m_imgui->disabled_end();

        m_imgui->disabled_begin(mv == nullptr || busy);
        if (ImGui::Button(_u8L("Remesh").c_str()))
            remesh_model();
        m_imgui->disabled_end();
        hover_tip(_u8L("Rebuilds the whole model with triangles close to this edge length - splitting the big "
                       "ones and merging the small ones - so displacement has an even density to work with. "
                       "Replaces the geometry; your paint is carried onto the new triangles spatially, so it "
                       "survives (already-baked relief is kept too)."),
                  { { "remesh_before.png", _L("Before") }, { "remesh_after.png", _L("After") } });

        // Settings for the whole stack rather than one layer. Standard mode pins them instead of showing them.
        if (mv != nullptr) {
            TextureDisplacementOptions &opts = mv->texture_displacement_options;
            ImGui::Separator();
            heading(_L("Result"));

            m_preview_params_dirty |= m_imgui->bbl_checkbox(_L("Displace up to the border"), opts.displace_border);
            hover_tip(_u8L("Lets the relief run right to the edge of the painted area. Turn it off to hold that "
                            "outer ring flat, which keeps the displacement strictly inside your paint but flattens "
                            "the pattern at the border."));

            m_preview_params_dirty |= m_imgui->bbl_checkbox(_L("Smooth result"), opts.smooth_enabled);
            hover_tip(_u8L("Smooths the geometry after the texture has been applied, to take the hard steps out of a "
                            "low-resolution image. Only what the displacement moved is touched. The Smoothing slider "
                            "on a layer is a different thing: it blurs the image before it is used."),
                      { { "geometry_smoothing.gif" } });
            if (opts.smooth_enabled) {
                float percent = opts.smooth_strength * 100.f;
                if (float_row("##dispsmooth", _L_CONTEXT("Strength", "Texture Displacement"), &percent, 1.f, 100.f, "%.0f %%", false, 0.f,
                              _L("How far each pass pulls a vertex towards its neighbours. High values round the "
                                 "relief off quickly; low values need more passes but keep more of the detail."))) {
                    opts.smooth_strength   = std::clamp(percent / 100.f, 0.01f, 1.f);
                    m_preview_params_dirty = true;
                }
                if (int_row("##dispsmoothit", _L("Passes"), &opts.smooth_iterations, 1, 10, "%d", 0.f,
                            _L("How many smoothing passes to run. More passes spread the smoothing further across "
                               "the surface; Strength decides how much each one moves.")))
                    m_preview_params_dirty = true;

                m_preview_params_dirty |= m_imgui->bbl_checkbox(_L("Ignore outer ring"), opts.smooth_skip_border);
                hover_tip(_u8L("Keeps the outer ring of the painted area out of the smoothing. Its neighbours "
                                "outside the paint never move, so smoothing it drags the relief down and leaves the "
                                "pattern half-melted at the border."));

                // The settings above ride along with Preview/Bake. This button is for geometry that has *already*
                // been baked, where there is no displacement left to fold the smoothing into.
                if (m_imgui->button(_u8L("Smooth baked mesh now")))
                    smooth_model();
                hover_tip(_u8L("Applies the smoothing above to the model itself, right now, within the painted area. "
                                "For relief that is already baked in - anything not yet baked is smoothed by Bake."));
            }
        }
    }

    // Shown in both modes. The pipeline switch, the two v2 experiments and the stage debugger are
    // developer controls: kept, but off the panel unless this is flipped.
    static constexpr bool SHOW_PIPELINE_DEV_CONTROLS = false;
    if (mv != nullptr) {
        TextureDisplacementOptions &opts = mv->texture_displacement_options;
        ImGui::Separator();
        if (SHOW_PIPELINE_DEV_CONTROLS) {
            // The classic path is the opt-in: the one-run pipeline is the default, and its resolution
            // control lives in the footer next to Bake (see below).
            bool classic = !opts.pipeline_v2;
            if (m_imgui->bbl_checkbox(_L("Experimental: classic bake pipeline"), classic)) {
                opts.pipeline_v2       = !classic;
                m_preview_params_dirty = true;
            }
            hover_tip(_u8L("Bake by moving the vertices the mesh already has, after preparing it (remesh, "
                           "adaptive subdivision, and a cut along sharp steps in the texture). Keeps the "
                           "topology, which is what colors need. The default pipeline instead refines, "
                           "cleans up sliver triangles, displaces and simplifies in one run; nothing needs "
                           "preparing first, but it does not produce colors yet."));
        }
        if (SHOW_PIPELINE_DEV_CONTROLS && opts.pipeline_v2) {
            // Keeping the relief above the plate is not a checkbox: it is unconditional, in both pipelines
            // (see build_texture_displacement()).
            m_preview_params_dirty |= m_imgui->bbl_checkbox(_L("Align mesh to texture edges"), opts.v2_relocate);
            hover_tip(_u8L("Slide vertices sideways onto the edges in the texture before displacing them. "
                           "Displacement can only move vertices up and down, so without this a sharp step "
                           "in the image lands wherever the triangles happen to be and comes out as a "
                           "staircase. Moving the vertices onto the step first gives a straight wall at the "
                           "same triangle count."));
            m_preview_params_dirty |= m_imgui->bbl_checkbox(_L("Clean up slivers"), opts.v2_regularize);
            hover_tip(_u8L("Collapse the thin triangles refinement inherits from the model's own "
                           "tessellation, before displacement samples them. A sliver's three corners "
                           "land on three unrelated parts of the texture, which is what makes the "
                           "relief look jagged."),
                      { { "collapse_slivers_off.png", _L("Off") }, { "collapse_slivers_on.png", _L("On") } });
        }
        // Below both pipelines' own settings, because it replays whichever of them is selected.
        if (SHOW_PIPELINE_DEV_CONTROLS)
            render_debug_stage_panel(mv);
    }

    // Content height, for next frame's body size. The cursor position is scroll-compensated, so this is the
    // height of everything above, whether or not it is scrolled.
    m_panel_body_h = ImGui::GetCursorPosY() - style.ItemSpacing.y;
    // After the content rather than before it, so a row inside that scrolls sideways (the ChromaStack
    // filters) has already taken the wheel for itself when it was under the pointer. The scroll this sets
    // lands at the next Begin either way.
    ease_wheel_scroll(m_panel_scroll_target, m_panel_scroll_applied);
    ImGui::EndChild();
    pop_slim_scrollbar_style();

    // The layer-list changes deferred from the loop above.
    if (convert_slot >= 0 && mv != nullptr)
        for (TextureDisplacementLayer &l : mv->texture_displacement_layers)
            if (l.slot == convert_slot) {
                chroma_convert_layer(l, convert_to_chroma);
                break;
            }
    if (slot_to_remove >= 0)
        remove_texture_layer(slot_to_remove);
    else if (move_slot >= 0)
        move_texture_layer(move_slot, move_to);
    else if (activate_slot >= 0)
        set_active_layer(activate_slot);
    // Retexturing a layer makes it the one being painted, too.
    if (m_picker_open_request && m_picker_slot >= 0)
        set_active_layer(m_picker_slot);

    // In this window's scope, where the popup's open request and the popup itself share one ID stack.
    ImVec2 panel_rect_min, panel_rect_max;
    {
        const ImVec2 win_min = ImGui::GetWindowPos();
        const ImVec2 win_sz  = ImGui::GetWindowSize();
        render_texture_library_popup(win_min, ImVec2(win_min.x + win_sz.x, win_min.y + win_sz.y));
        // Only recorded here. The image-adjustment panel is a top-level window rather than a popup, so it
        // cannot be begun inside this one; it is drawn after GizmoImguiEnd() below.
        panel_rect_min = win_min;
        panel_rect_max = ImVec2(win_min.x + win_sz.x, win_min.y + win_sz.y);
    }

    // ImGui sliders report "changed" continuously on every frame while being dragged, not just once on
    // release - rebuilding the preview (a real CPU mesh recompute) on every one of those frames is what made
    // dragging these sliders feel slow. Only rebuild once the mouse button that's driving the drag is
    // released, i.e. once per edit instead of dozens of times per drag.
    // The feature-adaptive subdivision follows the *displaced* surface (it samples depth * texture), so depth /
    // tile-size / rotation / invert all change where it puts triangles. The heavy displacement preview below
    // only rebuilds on release, which made the subdivide wireframe look like it ignored those edits - so
    // rebuild it live here (during the drag), the same cadence its own sliders use. Cheap: it is bounded by
    // the painted region.
    if (m_preview_params_dirty && m_subdivide_editing && m_subdivide_adaptive && m_subdivide_feature)
        rebuild_subdivide_preview();
    if (m_preview_params_dirty && (m_auto_update || !ImGui::IsMouseDown(ImGuiMouseButton_Left))) {
        rebuild_preview();
        // Same edits (tile size, rotation, offset, a new texture) are what the projector window draws, so it
        // refreshes on the same one-per-edit cadence. No-op while it is closed.
        update_projector();
        m_preview_params_dirty = false;
    }

    // ---- Footer: pinned below the body, so Bake never scrolls away. ----
    // Close is carried out at the very end of this function, not where it is clicked: closing shuts
    // the tool down (on_shutdown() and the painter base drop the selectors, the previews and the
    // ChromaStack state), and everything drawn after the button in this frame reads that state.
    bool close_requested = false;
    const float footer_top = ImGui::GetCursorScreenPos().y;
    {
        TextureDisplacementOptions &opts = mv->texture_displacement_options;
        const float x0 = ImGui::GetCursorPosX();
        // Over every part the bake will act on, not just the one holding the layer stack, so the count
        // the footer quotes is the count Bake is actually working with on a multibody object. Before
        // anything is painted there is nothing to bake yet, and the whole object is the honest estimate.
        size_t base_facets = 0;
        {
            std::vector<ModelVolume *> counted = painted_parts();
            if (counted.empty())
                counted = texture_parts();
            for (const ModelVolume *part : counted)
                base_facets += part->mesh().facets_count();
        }
        const int  base_k = int((base_facets + 500) / 1000);
        const bool v2     = mv != nullptr && opts.pipeline_v2;

        ImGui::Separator();

        // A heading rather than a fold: what it holds decides whether the bake is worth running, and a
        // section that can be closed is one more place for the reason a bake came out coarse to hide.
        bool auto_res = opts.v2_refine_mm <= 0.f; // the Auto box below writes it
        m_imgui->push_bold_font();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted((auto_res ? _u8L("Quality") + " (" + _u8L("Auto") + ")" : _u8L("Quality")).c_str());
        m_imgui->pop_bold_font();
        // The triangle budget is the one subdivision control Standard mode keeps: its right value depends on
        // the part rather than on the recipe (a big model, or a fine texture, simply needs more of them), and
        // raising or lowering it is safe without understanding anything else. Pro's Subdivide uses it too.
        ImGui::AlignTextToFramePadding();
        if (v2) {
            const V2Resolution         &rec  = v2_recommendation(*mv);

            // -1 is "auto": the row shows the recommendation, greyed; editing it makes it a fixed value.
            const bool auto_budget = opts.v2_max_triangles_k < 0;
            int        shown_k     = auto_budget ? v2_recommendation(*mv).budget_k : opts.v2_max_triangles_k;
            m_imgui->disabled_begin(auto_budget);
            // TRN Slider value: %d is the triangle budget in thousands
            if (int_row("##v2budget", _L("Budget"), &shown_k, 0, 4000, auto_budget ? _u8L("%d k (auto)").c_str() : "%d k", 0.f,
                        _L("How many thousand triangles this bake may spend on the area you painted. "
                           "Relief already baked into the rest of the model is kept on top of it, so a "
                           "second bake somewhere else gets the same budget as the first. Raise it if the "
                           "warning below Resolution says the detail will not fit; 0 keeps every triangle "
                           "the refinement produced, however many that is."),
                        { { "subdivide.gif" } })) {
                opts.v2_max_triangles_k = shown_k;
                m_preview_params_dirty  = true;
            }
            m_imgui->disabled_end();

            // The one control the default pipeline needs: the edge length its refinement targets, which
            // decides how much of the texture the mesh can carry. About eight texels per edge is where
            // fine detail (mortar joints, knurl ridges) stops being lost between vertices.
            m_imgui->text(_L("Resolution"));
            ImGui::SameLine();
            ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), x0 + label_w));
            // Auto: the edge (and the budget) follow the texture and the model. Off: the slider holds
            // a fixed value, seeded with the recommendation so it starts from something sensible.
            m_imgui->disabled_begin(busy);
            if (m_imgui->bbl_checkbox(wxString::FromUTF8("##v2auto"), auto_res)) {
                if (auto_res) {
                    opts.v2_refine_mm       = 0.f;
                    opts.v2_max_triangles_k = -1;
                } else {
                    opts.v2_refine_mm       = rec.edge_mm > 0.f ? rec.edge_mm : 0.3f;
                    opts.v2_max_triangles_k = rec.budget_k > 0 ? rec.budget_k : 750;
                }
                m_preview_params_dirty = true;
                m_parent.set_as_dirty();
            }
            m_imgui->disabled_end();
            hover_tip(_u8L("On: the resolution follows the size of the model and the budget is left at its "
                           "default. Untick to set both yourself - worth doing for a texture much finer or "
                           "much coarser than the part it sits on."));
            ImGui::SameLine();
            float shown = auto_res ? rec.edge_mm : opts.v2_refine_mm;
            // Held in a local: the format outlives the call that reads it, and the slider and the
            // range in its tooltip are then formatted the same way.
            const std::string v2edge_format = auto_res ? _u8L("%.2f mm (auto)") : std::string("%.2f mm");
            const bool        v2edge_chosen = auto_res && rec.edge_mm > 0.f;
            const wxString    v2edge_tip =
                v2edge_chosen ?
                    from_u8(Slic3r::format(_u8L("Chosen from the size of the model: %1$.2f mm triangles, "
                                                "budget %2% k. Untick Auto to set them yourself."),
                                           rec.edge_mm, rec.budget_k)) :
                    _L("How fine the mesh is made under the paint, in millimetres. It has to be "
                       "smaller than the detail you want out of the texture - a 0.5 mm groove needs "
                       "triangles well under 0.5 mm. Smaller costs triangles fast: halving it needs "
                       "four times as many, and once they no longer fit the budget the bake simplifies "
                       "back down and the detail goes with it.");
            m_imgui->disabled_begin(busy || auto_res);
            if (typed_slider("##v2edge", x0 + panel_w - ImGui::GetCursorPosX() - panel_edit_icon_room(), &shown, 0.02f, 2.f,
                             v2edge_format.c_str(), true, v2edge_tip,
                             v2edge_chosen ? std::vector<TipMedia>{} : std::vector<TipMedia>{ { "subdivide.gif" } })) {
                opts.v2_refine_mm      = shown;
                m_preview_params_dirty = true;
                m_parent.set_as_dirty();
            }
            m_imgui->disabled_end();

            // What this resolution costs over what is painted, against what the budget allows. Refining
            // past the budget is not an error - the bake simplifies back down to it - but the result
            // then carries less of the texture than the resolution asks for, and the only sign of that
            // used to be a mesh that came out coarser than expected. Shown before the bake, so the
            // answer is to change a number rather than to wait out a bake and redo it.
            const float  edge_now   = auto_res ? rec.edge_mm : opts.v2_refine_mm;
            const int    budget_k   = opts.v2_max_triangles_k < 0 ? rec.budget_k : opts.v2_max_triangles_k;
            const size_t budget     = size_t(std::max(0, budget_k)) * 1000;
            const size_t needed     = estimated_refined_triangles(*mv, edge_now);
            // Only when it is clearly over: the estimate runs about 3% high where it matters and up to
            // a third high at coarse resolutions, where the mesh's own triangles are already near the
            // target, and a warning about a bake that would have fitted is worse than none.
            if (budget > 0 && needed > budget * 5 / 4) {
                // Thousands under a million: a 119 k budget shown as "0.1 M" says nothing.
                const auto count = [](size_t n) {
                    return n >= 1000000 ? Slic3r::format("%1$.1f M", double(n) / 1000000.) :
                                          Slic3r::format("%1% k", (n + 500) / 1000);
                };
                // Wrapped to the panel, like the note under the buttons: unwrapped text runs past the
                // panel's edge and takes the window's width with it.
                ImGui::PushTextWrapPos(x0 + panel_w);
                m_imgui->warning_text(Slic3r::format(_u8L("Needs about %1% triangles, budget %2% - the bake "
                                                          "will simplify back down and lose detail."),
                                                     count(needed), count(budget)));
                ImGui::PopTextWrapPos();
            }
        } else {
            m_imgui->text(_L("Triangles"));
            ImGui::SameLine();
            ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), x0 + label_w));
            m_imgui->disabled_begin(busy);
            if (typed_slider_int("##subdivbudget", x0 + panel_w - ImGui::GetCursorPosX() - panel_edit_icon_room(),
                                 &m_subdivide_budget_k, 10, 2000, "+%d k", true,
                                 _L("How many thousand triangles the refinement may add to the model. The triangle "
                                    "that fits worst is always split first, so even a run that spends the lot has "
                                    "spent it where it shows most. Raise it if the relief still looks coarse."),
                                 { { "subdivide.gif" } })) {
                if (m_subdivide_editing)
                    rebuild_subdivide_preview();
                m_parent.set_as_dirty();
            }
            m_imgui->disabled_end();
        }

        const float button_h = std::round(frame_h * 1.25f);
        const float button_w = std::max({
            ImGui::CalcTextSize(_u8L("Close").c_str()).x,
            ImGui::CalcTextSize(_u8L("Stop").c_str()).x,
            ImGui::CalcTextSize(_u8L("Preparing...").c_str()).x,
            ImGui::CalcTextSize(_u8L("Baking...").c_str()).x,
        }) + m_imgui->scaled(0.5f);
        const float row_y  = ImGui::GetCursorPosY();
        const float icon_h = 21.f * scale;
        const float row_h  = std::max(icon_h, button_h);

        const std::vector<std::pair<wxString, wxString>> shortcut = 
            m_tool_type == ToolType::BUCKET_FILL ? m_shortcuts_bucket_fill
            : m_tool_type == ToolType::SMART_FILL  ? m_shortcuts_bucket_fill 
            : m_tool_type == ToolType::BRUSH       ? m_shortcuts_brush 
            : m_tool_type == ToolType::GAP_FILL    ? m_shortcuts_gap_fill 
            : m_shortcuts_sliders; // the sliders are there whether or not a painting tool is

        ImGui::SetCursorPosY(row_y + (row_h - icon_h) * .5f); // center vertically
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(int(m_imgui->scaled(.5f)), style.ItemSpacing.y));
        GLGizmoUtils::render_tooltip_button(m_imgui, m_parent, shortcut, x, y);
        ImGui::SameLine();
        GLGizmoUtils::render_wiki_guide_button(m_parent, scale, "https://www.orcaslicer.com/wiki/print_prepare/prepare_texture_displacement");
        ImGui::SameLine();
        GLGizmoUtils::render_video_guide_button(m_parent, scale, "https://www.youtube.com/watch?v=D7w3tG1kdvE");
        ImGui::PopStyleVar(1);

        ImGui::SameLine(x0 + panel_w - button_w * 2 - style.ItemSpacing.x);

        // A ChromaStack picture that has no picture, no filaments or no solve yet has nothing to bake,
        // and the card already says so in one sentence. Bake borrows that sentence rather than growing
        // a second opinion: two readings of the same state are two things that can disagree, and the
        // one place a user looks for "can I print this yet" would then contradict the button.
        const TextureDisplacementLayer *bake_layer = mv != nullptr ? active_layer() : nullptr;
        std::string                     chroma_block;
        if (bake_layer != nullptr && bake_layer->chroma_enabled) {
            const ChromaReadiness state = chroma_readiness(*bake_layer);
            if (!state.ready)
                chroma_block = state.text;
        }
        // A filter being tried on a picture is checked on every layer, not only the active one: the
        // filter row does not make its card active, and a bake would print the filter all the same.
        if (chroma_block.empty() && mv != nullptr)
            for (const TextureDisplacementLayer &l : mv->texture_displacement_layers)
                if (l.chroma_enabled && !chroma_live_adjust(l).identity()) {
                    chroma_block = chroma_readiness(l).text;
                    break;
                }
        // Read before Close below, which releases the selection any_part_painted() finds the parts through.
        const bool        painted    = mv != nullptr && any_part_painted();
        const bool        can_bake   = !busy && painted && chroma_block.empty();
        const std::string bake_label = m_prepare_in_progress ? _u8L("Preparing...") :
                                       m_bake_in_progress    ? _u8L("Baking...") :
                                                               into_u8(m_desc.at("bake"));
        GLGizmoUtils::push_orca_button_style();
        m_imgui->push_bold_font();
        m_imgui->disabled_begin(!can_bake);
        ImGui::SetCursorPosY(row_y + (row_h - button_h) * .5f); // center vertically
        if (ImGui::Button((bake_label + "##bake").c_str(), ImVec2(button_w, button_h))) {
            // Standard mode's Bake is the whole pipeline (remesh -> refine -> displace); Pro's is only the
            // displacement, because there the user has already prepared the mesh with the controls above.
            if (pro_mode())
                bake();
            else
                bake_standard();
        }
        m_imgui->disabled_end();
        m_imgui->pop_bold_font();
        GLGizmoUtils::pop_orca_button_style();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            panel_tooltip(!chroma_block.empty() ? chroma_block :
                          mv != nullptr && !painted ?
                              (m_seam_edit_mode ? _u8L("Nothing is painted yet. The UV editor's seam tool is on, so "
                                                       "strokes on the model mark seams instead of painting - turn "
                                                       "it off in the pane to paint.") :
                                                  _u8L("Nothing is painted yet.")) :
                          pro_mode() ?
                              _u8L("Turn the painted height maps into real geometry, by moving the vertices that are "
                                   "already there. Use Subdivide first if the mesh is too coarse to show the detail.") :
                              _u8L("Turn the painted height maps into real geometry. The mesh is remeshed to an even "
                                   "density and refined where the texture bends first, so the detail has vertices to "
                                   "land on - all in one step."));

        ImGui::SameLine();
        if (busy) {
            if (ImGui::Button((_u8L("Stop") + "##stop").c_str(), ImVec2(button_w, button_h)))
                wxGetApp().plater()->get_ui_job_worker().cancel_all();
            hover_tip(_u8L("Stops the bake. Whatever it had already finished stays on the model, and can be undone."));
        } else {
            if (ImGui::Button((_u8L("Close") + "##close").c_str(), ImVec2(button_w, button_h)))
                close_requested = true;
            hover_tip(_u8L("Closes the tool without baking. Your paint, layers and settings stay with the model."));
        }

        // What Bake will produce, and which layers it will skip.
        if (mv != nullptr) {
            const std::string base_count = base_k > 0 ? Slic3r::format(_u8L("%1% k"), base_k) :
                                                        std::to_string(base_facets);
            const unsigned    base_n     = base_k > 0 ? unsigned(base_k) * 1000 : unsigned(base_facets);
            const unsigned    result_n   = unsigned(base_k + m_subdivide_budget_k) * 1000;
            // TRN %1% is a triangle count, either exact or rounded to thousands such as "120 k".
            std::string note = v2         ? Slic3r::format(into_u8(_L_PLURAL("%1% triangle before the bake", "%1% triangles before the bake", base_n)), base_count) :
                               // TRN %1% is a triangle count, either exact or rounded to thousands such as "120 k".
                               pro_mode() ? Slic3r::format(into_u8(_L_PLURAL("Bake moves existing vertices - the model stays at %1% triangle",
                                                                             "Bake moves existing vertices - the model stays at %1% triangles", base_n)), base_count) :
                                            Slic3r::format(into_u8(_L_PLURAL("Bake result up to ~%1% k triangle", "Bake result up to ~%1% k triangles", result_n)),
                                                           base_k + m_subdivide_budget_k);
            if (painted) {
                std::vector<std::string> skipped;
                for (const TextureDisplacementLayer &l : mv->texture_displacement_layers)
                    if (!slot_painted(l.slot))
                        skipped.push_back(layer_name(l));
                if (skipped.size() > 2)
                    note += ", " + Slic3r::format(into_u8(_L_PLURAL("%1% layer not painted, skipped", "%1% layers not painted, skipped",
                                                                    unsigned(skipped.size()))), skipped.size());
                else if (!skipped.empty())
                    // TRN %1% is one layer name, or two names separated by a comma.
                    note += ", " + Slic3r::format(into_u8(_L_PLURAL("%1% not painted, skipped", "%1% not painted, skipped",
                                                                    unsigned(skipped.size()))),
                                                   skipped.size() == 2 ? skipped[0] + ", " + skipped[1] : skipped[0]);
            }
            ImGui::PushTextWrapPos(x0 + panel_w);
            ImGui::TextDisabled("%s", note.c_str());
            ImGui::PopTextWrapPos();
            // The clause this line used to carry. It explains the number rather than reporting it, so
            // it belongs where an explanation is asked for.
            if (ImGui::IsItemHovered())
                panel_tooltip(v2 ? _u8L("What the model is before the bake. The bake refines it to the mesh detail "
                                        "set above, up to the triangle budget that goes with it.") :
                                   _u8L("What the model is before the bake, and roughly what it will be after."));
        }
    }
    m_panel_footer_h = ImGui::GetCursorScreenPos().y - footer_top;
    constexpr float resize_epsilon = 0.5f;
    const float content_bottom = ImGui::GetCursorPosY() - style.ItemSpacing.y;
    const float window_bottom  = ImGui::GetWindowContentRegionMax().y;
    const bool  outer_window_needs_fit = content_bottom < window_bottom - resize_epsilon ||
                                         (content_bottom > window_bottom + resize_epsilon &&
                                          ImGui::GetWindowHeight() < ImGui::GetMainViewport()->Size.y -
                                                                             2.f * style.DisplaySafeAreaPadding.y - resize_epsilon);
    if (std::abs(m_panel_body_h - previous_body_h) > resize_epsilon ||
        std::abs(m_panel_footer_h - previous_footer_h) > resize_epsilon || outer_window_needs_fit) {
        m_parent.request_extra_frame();
    }
    GizmoImguiEnd();

    // Outside the gizmo's window, because it is a window of its own: a floating, non-modal panel that
    // leaves the 3D view live so the relief can be watched while the sliders move. No-op when closed.
    render_image_adjust_window(panel_rect_min, panel_rect_max);
    render_chroma_zoom();
    render_chroma_diagnostics();

    ImGuiWrapper::pop_toolbar_style();

    // Drawn last, over everything, via the foreground draw list: the +/- add-remove sign next to the 3D
    // cursor. Still inside the gizmo's ImGui frame here, which is what render_paint_cursor_hint() needs.
    render_paint_cursor_hint();

    if (close_requested)
        m_parent.reset_all_gizmos();
}

} // namespace Slic3r::GUI
