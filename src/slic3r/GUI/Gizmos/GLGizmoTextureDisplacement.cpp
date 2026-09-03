#include "GLGizmoTextureDisplacement.hpp"

#include <boost/log/trivial.hpp>

#include "libslic3r/AABBTreeIndirect.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/MeshBoolean.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/format.hpp"

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
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/OpenGLManager.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/TextureLibrary.hpp"
#include "slic3r/GUI/TextureProjectorFrame.hpp"
#include "slic3r/GUI/UVEditorCanvas.hpp"
#include "slic3r/GUI/Jobs/TextureDisplacementBakeJob.hpp"
#include "slic3r/GUI/Jobs/TextureDisplacementPrepareJob.hpp"
#include "slic3r/GUI/Jobs/TextureDisplacementPreviewJob.hpp"
#include "slic3r/Utils/UndoRedo.hpp"
#include "GLGizmoUtils.hpp"

#include <glad/gl.h>
#include <tbb/parallel_for.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <queue>
#include <set>

namespace Slic3r::GUI {

namespace {
// ImGuiWrapper::slider_float()'s trailing `power` parameter is forwarded straight into
// ImGui::SliderFloat()'s ImGuiSliderFlags argument - this ImGui (1.83) replaced the old float
// "power" curve API with a flags word but kept the parameter in the same position, and
// ImGuiWrapper never caught up with the rename. Passing the Logarithmic flag through it is
// therefore how a log-scaled slider gets requested here.
//
// Depth and tile size both want it: they span two to three orders of magnitude, and the values
// that need the most precision (a few hundredths of a millimetre of relief, a very fine tile) are
// all bunched into the very bottom of a linear range, where a single pixel of slider travel is a
// bigger jump than the whole useful region.
constexpr float ImGuiLogSlider = float(ImGuiSliderFlags_Logarithmic);

// Uploads decoded 8-bit grayscale height data as an RGBA GPU texture for display in the panel.
// Shared by the per-layer thumbnail and the picker's library entries, which differ only in where
// their pixels came from.
//
// The source is downscaled to THUMBNAIL_MAX_PX first, with a box filter, and uploaded *without*
// mipmaps. Both halves of that matter, and together they are the fix for thumbnails rendering as
// garbage:
//   - GLTexture's mipmap levels are not real downscales (see load_from_raw_data()'s header) - every
//     level re-uploads the full-resolution buffer reinterpreted at a smaller size. A 512px texture
//     drawn into a ~48px row is minified ~10x, which is exactly the regime where OpenGL picks those
//     broken levels, so the thumbnail showed a scrambled crop of the image rather than the image.
//     Turning mipmaps off makes it sample level 0, which is the real picture.
//   - But level 0 at 512px sampled down to 48px with plain GL_LINEAR (a 2x2 tap) would alias badly
//     on exactly the high-frequency patterns these textures are (knurl, hexagons, weave). Box-
//     filtering down to roughly twice the displayed size first is what actually removes the
//     frequencies that would alias, and it cuts the VRAM these hold by ~16x as a bonus.
constexpr int THUMBNAIL_MAX_PX = 128;

// Everything above is about drawing a ~48 px panel row, and none of it applies to the height texture
// the fast-preview *shader* samples: that one is magnified across the model, not minified into a
// row, and every texel it loses is relief the preview cannot show. It gets its own upload at (up to)
// this size, so the bump preview reads the same height field the bake does instead of a 128 px box
// blur of it - which is what made Fast look flatter and softer than the result it was previewing.
constexpr int HEIGHT_TEX_MAX_PX = 2048;

// How many rings of vertex-adjacent triangles either side of the paint's edge join the border refine
// band (see collect_paint_region()). Two is enough to grade the size change without the band's own
// cost growing to matter: it is a ring around a perimeter, not an area.
constexpr int BORDER_BAND_RINGS = 2;

// Is `p` inside triangle `t`, given that it already lies in the triangle's plane? Barycentric via the
// three sub-triangle cross products, compared against the whole triangle's normal. Used to carry a
// partly painted source triangle's coverage onto the children of a subdivision, which are coplanar
// with it by construction (subdivision only adds edge midpoints).
bool point_in_triangle_coplanar(const Vec3f &p, const std::array<Vec3f, 3> &t)
{
    const Vec3f n = (t[1] - t[0]).cross(t[2] - t[0]);
    const float n2 = n.squaredNorm();
    if (n2 < 1e-20f)
        return false; // degenerate: it covers no area, so nothing is inside it
    // A small negative tolerance, scaled by the triangle, keeps a centroid sitting exactly on a shared
    // edge from falling through the gap between two neighbouring pieces.
    const float eps = -1e-4f * n2;
    return (t[1] - t[0]).cross(p - t[0]).dot(n) >= eps &&
           (t[2] - t[1]).cross(p - t[1]).dot(n) >= eps &&
           (t[0] - t[2]).cross(p - t[2]).dot(n) >= eps;
}

// Carries one texture-displacement paint mask from `src_mesh` onto `dst_mesh` - a different
// tessellation of the same surface (an isotropic remesh, in practice).
//
// TriangleSelector::remap_painting(), which ModelVolume::restore_painting() uses for the four
// standard paint channels, is not usable here. It runs one select_patch() flood fill per
// (painted sub-triangle x overlapping target facet) pair, and select_patch() splits the *target's*
// triangles down to a 0.02-0.05 mm edge limit along every cursor boundary, behind an
// O(target triangles) visited-set allocation per call. A coarse import painted with a fine brush
// carries tens of thousands of painted sub-triangles, so that is tens of millions of sub-triangles
// created on the target - which is what made Remesh, and Standard mode's Bake (which remeshes for
// you), appear to hang rather than finish.
//
// Each target triangle here asks one question instead: is the point on the source surface closest
// to my centroid inside a painted piece? One AABB-tree query per target triangle, whole facets
// only, no splitting - O(target log source). `src_tree` is built over `src_mesh.its` by the caller
// and shared across the layers; `dst_to_src` brings the target's vertices into the source's frame.
TriangleSelector::TriangleSplittingData remap_texture_paint_spatial(
    const TriangleMesh &src_mesh, const TriangleSelector::TriangleSplittingData &src_data,
    const AABBTreeIndirect::Tree3f &src_tree, const TriangleMesh &dst_mesh, const Vec3f &dst_to_src)
{
    const indexed_triangle_set &src  = src_mesh.its;
    const indexed_triangle_set &dst  = dst_mesh.its;
    const size_t                ntri = src.indices.size();
    if (src_data.bitstream.empty() || ntri == 0 || dst.indices.empty() || src_tree.empty())
        return {};

    // The painted patch, split into per-source-triangle pieces - the same shape
    // collect_paint_region() builds, and for the same reason: a source triangle the brush only
    // partly covered has to answer "is this point painted" geometrically rather than be rounded to
    // painted-or-not, which leaves a ragged fringe along any curved brush boundary.
    TriangleSelector src_sel(src_mesh);
    src_sel.deserialize(src_data, false);
    std::vector<int>           piece_src;
    const indexed_triangle_set patch = src_sel.get_facets_strict(EnforcerBlockerType::ENFORCER, &piece_src);
    if (patch.indices.empty())
        return {};

    const auto tri_area2 = [](const Vec3f &a, const Vec3f &b, const Vec3f &c) {
        return (b - a).cross(c - a).norm();
    };
    std::vector<float> src_area2(ntri, 0.f), covered2(ntri, 0.f);
    for (size_t i = 0; i < ntri; ++i)
        src_area2[i] = tri_area2(src.vertices[size_t(src.indices[i][0])], src.vertices[size_t(src.indices[i][1])],
                                 src.vertices[size_t(src.indices[i][2])]);
    for (size_t j = 0; j < patch.indices.size() && j < piece_src.size(); ++j) {
        if (size_t(piece_src[j]) >= ntri)
            continue;
        const stl_triangle_vertex_indices &t = patch.indices[j];
        covered2[size_t(piece_src[j])] += tri_area2(patch.vertices[size_t(t[0])], patch.vertices[size_t(t[1])],
                                                    patch.vertices[size_t(t[2])]);
    }
    std::vector<uint8_t> full(ntri, 0);
    for (size_t i = 0; i < ntri; ++i)
        full[i] = (src_area2[i] > 0.f && covered2[i] >= 0.999f * src_area2[i]) ? 1 : 0;

    // CSR pieces, kept for partly covered sources only - a full one answers every query "painted".
    std::vector<int> part_start(ntri + 1, 0);
    for (size_t j = 0; j < patch.indices.size() && j < piece_src.size(); ++j)
        if (size_t(piece_src[j]) < ntri && !full[size_t(piece_src[j])])
            ++part_start[size_t(piece_src[j]) + 1];
    for (size_t i = 0; i < ntri; ++i)
        part_start[i + 1] += part_start[i];
    std::vector<std::array<Vec3f, 3>> part;
    part.resize(size_t(part_start[ntri])); // not a constructor call: `vector<T> p(size_t(x[n]));` parses
                                           // as a function declaration, and every use of `part` below
                                           // then fails with something that does not mention the cause.
    {
        std::vector<int> fill(part_start.begin(), part_start.begin() + ntri);
        for (size_t j = 0; j < patch.indices.size() && j < piece_src.size(); ++j) {
            const size_t S = size_t(piece_src[j]);
            if (S >= ntri || full[S])
                continue;
            const stl_triangle_vertex_indices &t = patch.indices[j];
            part[size_t(fill[S]++)] = { patch.vertices[size_t(t[0])], patch.vertices[size_t(t[1])],
                                        patch.vertices[size_t(t[2])] };
        }
    }

    // Classify in parallel, then write the mask serially - TriangleSelector is not thread safe.
    std::vector<uint8_t> painted(dst.indices.size(), 0);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, dst.indices.size()),
                      [&](const tbb::blocked_range<size_t> &range) {
        for (size_t i = range.begin(); i < range.end(); ++i) {
            const stl_triangle_vertex_indices &t = dst.indices[i];
            const Vec3f centroid = (dst.vertices[size_t(t[0])] + dst.vertices[size_t(t[1])] +
                                    dst.vertices[size_t(t[2])]) / 3.f + dst_to_src;
            size_t hit     = 0;
            Vec3f  hit_pos = Vec3f::Zero();
            if (AABBTreeIndirect::squared_distance_to_indexed_triangle_set(src.vertices, src.indices, src_tree,
                                                                          centroid, hit, hit_pos) < 0.f ||
                hit >= ntri)
                continue;
            if (full[hit]) {
                painted[i] = 1;
                continue;
            }
            for (int k = part_start[hit]; k < part_start[hit + 1]; ++k)
                if (point_in_triangle_coplanar(hit_pos, part[size_t(k)])) {
                    painted[i] = 1;
                    break;
                }
        }
    });

    TriangleSelector dst_sel(dst_mesh);
    for (size_t i = 0; i < painted.size(); ++i)
        if (painted[i])
            dst_sel.set_facet(int(i), EnforcerBlockerType::ENFORCER);
    return dst_sel.serialize();
}

// Edge of the RGB lookup cube make_palette_quantizer() builds. 24 gives 13824 cells - far finer than
// the difference between any two printable colours - and costs one DeltaE00 per cell per palette
// entry to fill.
constexpr int PALETTE_LUT_EDGE = 24;

// Ceiling on the printable palette, which bounds that fill cost (and the shader's uniform array).
constexpr int PALETTE_MAX_ENTRIES = 64;

std::unique_ptr<GLTexture> upload_height_thumbnail(const DecodedHeightTexture &decoded, int max_px = THUMBNAIL_MAX_PX)
{
    if (decoded.empty())
        return nullptr;

    // Preserve aspect; never upscale a texture that is already small.
    const int scale = std::max(1, (std::max(decoded.width, decoded.height) + max_px - 1) / max_px);
    const int w     = std::max(1, decoded.width / scale);
    const int h     = std::max(1, decoded.height / scale);

    std::vector<unsigned char> rgba(size_t(w) * size_t(h) * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            // Average the source block this destination pixel covers.
            const int x0 = x * decoded.width / w, x1 = std::max(x0 + 1, (x + 1) * decoded.width / w);
            const int y0 = y * decoded.height / h, y1 = std::max(y0 + 1, (y + 1) * decoded.height / h);
            unsigned int sum = 0, n = 0;
            for (int sy = y0; sy < y1 && sy < decoded.height; ++sy)
                for (int sx = x0; sx < x1 && sx < decoded.width; ++sx, ++n)
                    sum += decoded.pixels[size_t(sy) * size_t(decoded.width) + size_t(sx)];

            const unsigned char gray = (n > 0) ? static_cast<unsigned char>(sum / n) : 0;
            const size_t        di   = (size_t(y) * size_t(w) + size_t(x)) * 4;
            rgba[di + 0] = gray;
            rgba[di + 1] = gray;
            rgba[di + 2] = gray;
            rgba[di + 3] = 255;
        }

    auto texture = std::make_unique<GLTexture>();
    if (!texture->load_from_raw_data(std::move(rgba), (unsigned int) w, (unsigned int) h, false, /* use_mipmaps */ false))
        return nullptr;
    return texture;
}

// The same upload, of the texture's *colour* rather than its height, for the fast preview to quantize
// per fragment. Null for a grayscale texture - there is nothing to show.
//
// Box-filtered down like the height is, and for a sharper reason: the fast preview quantizes every
// fragment independently, so any texel-scale noise left in the image becomes a scatter of single-pixel
// colour flips on screen. Filtering on the way to the GPU is where that is cheapest to remove.
std::unique_ptr<GLTexture> upload_color_texture(const DecodedHeightTexture &decoded, int max_px)
{
    if (!decoded.has_color())
        return nullptr;

    const int scale = std::max(1, (std::max(decoded.width, decoded.height) + max_px - 1) / max_px);
    const int w     = std::max(1, decoded.width / scale);
    const int h     = std::max(1, decoded.height / scale);

    std::vector<unsigned char> rgba(size_t(w) * size_t(h) * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int x0 = x * decoded.width / w, x1 = std::max(x0 + 1, (x + 1) * decoded.width / w);
            const int y0 = y * decoded.height / h, y1 = std::max(y0 + 1, (y + 1) * decoded.height / h);
            unsigned int sum[3] = { 0, 0, 0 };
            unsigned int n      = 0;
            for (int sy = y0; sy < y1 && sy < decoded.height; ++sy)
                for (int sx = x0; sx < x1 && sx < decoded.width; ++sx, ++n) {
                    const size_t si = (size_t(sy) * size_t(decoded.width) + size_t(sx)) * 3;
                    for (int c = 0; c < 3; ++c)
                        sum[c] += decoded.rgb[si + size_t(c)];
                }
            const size_t di = (size_t(y) * size_t(w) + size_t(x)) * 4;
            for (int c = 0; c < 3; ++c)
                rgba[di + size_t(c)] = (n > 0) ? static_cast<unsigned char>(sum[size_t(c)] / n) : 0;
            rgba[di + 3] = 255;
        }

    auto texture = std::make_unique<GLTexture>();
    if (!texture->load_from_raw_data(std::move(rgba), (unsigned int) w, (unsigned int) h, false, false))
        return nullptr;
    return texture;
}

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
}

bool GLGizmoTextureDisplacement::on_init()
{
    m_desc["cursor_size"]   = _L("Brush size");
    m_desc["circle"]        = _L("Circle");
    m_desc["sphere"]        = _L("Sphere");
    m_desc["add_texture"]   = _L("Add layer");
    m_desc["remove_layer"]  = _L("Remove");
    m_desc["bake"]          = _L("Bake");
    m_desc["remove_all"]    = _L("Erase all");
    return true;
}

std::string GLGizmoTextureDisplacement::on_get_name() const
{
    return _u8L("Texture displacement");
}

void GLGizmoTextureDisplacement::on_shutdown()
{
    m_parent.toggle_model_objects_visibility(true);
    m_preview_glmodel.reset();
    m_bump_preview_glmodel.reset();
    m_paint_overlay_glmodel.reset();
    m_paint_overlay_dirty = false;
    // Any preview still in flight is superseded: bumping the shared counter makes it abort at its next
    // progress poll, and its completion handler then finds nothing to do.
    m_preview_generation->fetch_add(1);
    m_preview_job_pending = false;
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
    m_bump_active_chart  = -1;
    m_bump_active_vertex.clear();
    m_bump_island_delta  = Eigen::Matrix<float, 2, 3>::Identity();
    m_island_drag_active = false;
    m_island_move_set.clear();
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
    return shift_down ? _L("Erase texture displacement paint") : _L("Paint texture displacement");
}

void GLGizmoTextureDisplacement::render_painter_gizmo()
{
    const Selection &selection = m_parent.get_selection();

    glsafe(::glEnable(GL_BLEND));
    glsafe(::glEnable(GL_DEPTH_TEST));

    // Once anything is painted, m_preview_glmodel holds the true displaced result (same algorithm
    // Bake uses). The untouched original topology (what render_triangles() draws) coincides
    // exactly with it everywhere except the painted/displaced area, so both are drawn: the real
    // preview geometry first, then the usual selection-highlight overlay with a small depth bias
    // so it wins the depth test on the coincident (unpainted) surface - keeping the familiar
    // enforcer/blocker highlight for precise brush editing there. Where the surface has actually
    // been displaced, the raised preview geometry legitimately occludes the flat overlay - that
    // visible bump is itself the "this is painted" indicator in that area.
    //
    // The bump preview is different: it never actually moves geometry (it's a shading trick), so
    // its depth is identical to the overlay's *everywhere*, not just in the unpainted area - the
    // depth-biased opaque overlay would win the depth test across the whole surface and hide the bump
    // shading entirely. So render_triangles() is skipped for it. What is *not* skipped is
    // render_paint_overlay(): leaving the bump shading as the only paint feedback meant a stroke that
    // erased paint, or added it with no texture picked, changed nothing on screen until the whole
    // preview rebuilt at stroke end - and in the true-displacement view the opaque overlay is hidden
    // by the raised surface for the same reason. The translucent tint covers both cases.
    // Coalesced bump rebuild from an in-progress UV island drag (see on_island_edited): done here, at
    // most once per drawn frame, rather than synchronously in the UV canvas's mouse-move handler.
    if (m_use_bump_preview && m_bump_preview_dirty) {
        rebuild_bump_preview_mesh();
        m_bump_preview_dirty = false;
    }
    // Same coalescing for the paint tint, but on its own flag: a stroke marks this every mouse move
    // (see on_mouse()) and it only costs the painted patch, whereas the bump mesh also carries every
    // unpainted triangle of the volume and stays on the stroke-end cadence.
    if (m_paint_overlay_dirty) {
        rebuild_paint_overlay();
        m_paint_overlay_dirty = false;
    }
    // is_initialized() alone is not enough: render_bump_preview_mesh() also needs an active layer
    // with a decoded texture and a compiled shader, and bails silently without them. Hiding the real
    // volume for a bump pass that then draws nothing is what made the model vanish - most obviously
    // with zero layers, but equally with a layer that has no texture picked yet.
    const bool use_bump = m_use_bump_preview && m_bump_preview_glmodel.is_initialized() && bump_preview_ready();
    const bool use_true_preview = !use_bump && m_preview_glmodel.is_initialized();
    // In Checker/Distortion mode the UV-check overlay *is* the surface visualization the user is
    // looking at, so the opaque paint-selection highlight must not be drawn on top of it - same
    // reasoning as skipping it for the bump preview (see bug #12). Without this the painted area
    // covers the checker/heatmap and it can't be seen.
    const bool show_paint_overlay = m_uv_check_mode == UVCheckMode::None;

    // Hide the real volume only when something is actually going to be drawn in its place; otherwise
    // put it back. Getting this wrong leaves an invisible model, so it is decided once, here, rather
    // than per branch below.
    m_parent.toggle_model_objects_visibility(true);
    if (use_bump || use_true_preview) {
        if (ModelVolume *mv = texture_volume())
            m_parent.toggle_model_objects_visibility(false, m_c->selection_info()->model_object(),
                                                      m_c->selection_info()->get_active_instance(), mv);
    }

    if (use_bump) {
        render_bump_preview_mesh();
    } else if (use_true_preview) {
        render_preview_mesh();

        if (show_paint_overlay) {
            glsafe(::glEnable(GL_POLYGON_OFFSET_FILL));
            glsafe(::glPolygonOffset(-1.0f, -1.0f));
            render_triangles(selection);
            glsafe(::glDisable(GL_POLYGON_OFFSET_FILL));
        }
    } else if (show_paint_overlay) {
        render_triangles(selection);
    }

    // The translucent paint tint. Needed in the bump view because the opaque highlight above is
    // skipped there, and in the true-displacement view because the displaced surface rises *above*
    // the undisplaced overlay geometry and hides it exactly where the relief is strongest - in both
    // cases leaving an erase stroke with no visible effect until the next full preview rebuild.
    if (show_paint_overlay && (use_bump || use_true_preview))
        render_paint_overlay();

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

bool GLGizmoTextureDisplacement::on_mouse_seam(const wxMouseEvent &mouse_event)
{
    // Hold Ctrl to orbit/pan the camera while in seam mode, exactly as the base painter lets you do
    // while painting: with Ctrl down we consume nothing, so the canvas gets the drag and moves the
    // view. Without this, seam mode swallowed every left-drag and the camera couldn't be rotated.
    if (mouse_event.CmdDown())
        return false;

    // Left-click marks/unmarks an edge; swallow the rest of the left-button stream so a drag doesn't
    // paint, but let everything else through so the camera still orbits/pans/zooms normally.
    if (mouse_event.LeftDown()) {
        const Vec2d pos(mouse_event.GetX(), mouse_event.GetY());
        if (m_seam_path_mode) {
            // Two-click shortest-path seam: first click sets the start vertex, the next seams the whole
            // path to it and becomes the new start (so a seam line chains click by click).
            const int v = seam_vertex_at(pos);
            if (v >= 0) {
                if (m_seam_path_anchor < 0)
                    m_seam_path_anchor = v;
                else {
                    mark_seam_path(m_seam_path_anchor, v);
                    m_seam_path_anchor = v;
                }
                rebuild_seam_anchor_overlay();
                m_parent.set_as_dirty();
            }
        } else {
            toggle_seam_at(pos);
        }
        return true;
    }
    // Live hover: highlight what a click would pick, so the clickable target is obvious (the "I don't
    // know how it works" the user hit). In normal mode that is an edge; in shortest-path mode it is a
    // vertex. Don't swallow the motion - the camera still needs it.
    if (mouse_event.Moving()) {
        const Vec2d pos(mouse_event.GetX(), mouse_event.GetY());
        if (m_seam_path_mode) {
            const int v = seam_vertex_at(pos);
            if (v != m_seam_hover_vertex) {
                m_seam_hover_vertex = v;
                m_seam_hover_edge   = { -1, -1 };
                rebuild_seam_hover_overlay();
                m_parent.set_as_dirty();
            }
        } else {
            const std::pair<int, int> edge = seam_edge_at(pos);
            if (edge != m_seam_hover_edge) {
                m_seam_hover_edge   = edge;
                m_seam_hover_vertex = -1;
                rebuild_seam_hover_overlay();
                m_parent.set_as_dirty();
            }
        }
    }
    if (mouse_event.LeftUp() || (mouse_event.Dragging() && mouse_event.LeftIsDown()))
        return true;
    return false;
}

int GLGizmoTextureDisplacement::texture_volume_raycaster_index() const
{
    const ModelVolume *mv = texture_volume();
    const ModelObject *mo = m_c->selection_info()->model_object();
    if (mv == nullptr || mo == nullptr)
        return -1;

    int idx = -1, count = 0;
    for (const ModelVolume *v : mo->volumes) {
        if (!v->is_model_part())
            continue;
        if (v == mv) { idx = count; break; }
        ++count;
    }
    return (idx >= 0 && idx < int(m_c->raycaster()->raycasters().size())) ? idx : -1;
}

std::pair<int, int> GLGizmoTextureDisplacement::seam_edge_at(const Vec2d &mouse_pos) const
{
    const ModelVolume *mv = texture_volume();
    const ModelObject *mo = m_c->selection_info()->model_object();
    if (mv == nullptr || mo == nullptr)
        return { -1, -1 };

    const int idx = texture_volume_raycaster_index();
    if (idx < 0)
        return { -1, -1 };
    const auto &raycasters = m_c->raycaster()->raycasters();

    const Selection  &selection = m_parent.get_selection();
    const Transform3d trafo     = mo->instances[selection.get_instance_idx()]->get_transformation().get_matrix() * mv->get_matrix();
    const Camera     &camera    = wxGetApp().plater()->get_camera();

    Vec3f  hit = Vec3f::Zero(), normal = Vec3f::Zero();
    size_t facet = 0;
    if (!raycasters[size_t(idx)]->unproject_on_mesh(mouse_pos, trafo, camera, hit, normal,
                                                     m_c->object_clipper()->get_clipping_plane(), &facet))
        return { -1, -1 };

    const indexed_triangle_set &its = mv->mesh().its;
    if (facet >= its.indices.size())
        return { -1, -1 };
    const stl_triangle_vertex_indices &tri = its.indices[facet];

    // The facet edge nearest the hit point (point-to-segment distance in mesh space).
    const auto seg_dist = [](const Vec3f &p, const Vec3f &a, const Vec3f &b) {
        const Vec3f  ab = b - a;
        const float  l2 = ab.squaredNorm();
        const float  t  = (l2 > 1e-12f) ? std::clamp((p - a).dot(ab) / l2, 0.f, 1.f) : 0.f;
        return (p - (a + ab * t)).norm();
    };
    int   best_i = 0;
    float best_d = std::numeric_limits<float>::max();
    for (int i = 0; i < 3; ++i) {
        const float d = seg_dist(hit, its.vertices[tri[i]], its.vertices[tri[(i + 1) % 3]]);
        if (d < best_d) { best_d = d; best_i = i; }
    }
    const int a = tri[best_i], b = tri[(best_i + 1) % 3];
    return { std::min(a, b), std::max(a, b) };
}

void GLGizmoTextureDisplacement::toggle_seam_at(const Vec2d &mouse_pos)
{
    TextureDisplacementLayer *layer = active_layer();
    if (layer == nullptr)
        return;
    const std::pair<int, int> edge = seam_edge_at(mouse_pos);
    if (edge.first < 0)
        return;

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Mark texture seam"), UndoRedo::SnapshotType::GizmoAction);
    auto &seams = layer->lscm_seam_edges;
    if (const auto it = std::find(seams.begin(), seams.end(), edge); it != seams.end())
        seams.erase(it);
    else
        seams.push_back(edge);
    rebuild_preview();
}

int GLGizmoTextureDisplacement::seam_vertex_at(const Vec2d &mouse_pos) const
{
    const ModelVolume *mv = texture_volume();
    const ModelObject *mo = m_c->selection_info()->model_object();
    if (mv == nullptr || mo == nullptr)
        return -1;
    const int idx = texture_volume_raycaster_index();
    if (idx < 0)
        return -1;
    const auto &raycasters = m_c->raycaster()->raycasters();

    const Selection  &selection = m_parent.get_selection();
    const Transform3d trafo     = mo->instances[selection.get_instance_idx()]->get_transformation().get_matrix() * mv->get_matrix();
    const Camera     &camera    = wxGetApp().plater()->get_camera();

    Vec3f  hit = Vec3f::Zero(), normal = Vec3f::Zero();
    size_t facet = 0;
    if (!raycasters[size_t(idx)]->unproject_on_mesh(mouse_pos, trafo, camera, hit, normal,
                                                     m_c->object_clipper()->get_clipping_plane(), &facet))
        return -1;
    const indexed_triangle_set &its = mv->mesh().its;
    if (facet >= its.indices.size())
        return -1;
    const stl_triangle_vertex_indices &tri = its.indices[facet];
    int   best = tri[0];
    float best_d = std::numeric_limits<float>::max();
    for (int i = 0; i < 3; ++i) {
        const float d = (hit - its.vertices[tri[i]]).squaredNorm();
        if (d < best_d) { best_d = d; best = tri[i]; }
    }
    return best;
}

void GLGizmoTextureDisplacement::mark_seam_path(int v_from, int v_to)
{
    TextureDisplacementLayer *layer = active_layer();
    const ModelVolume        *mv    = texture_volume();
    if (layer == nullptr || mv == nullptr || v_from < 0 || v_to < 0 || v_from == v_to)
        return;
    const indexed_triangle_set &its = mv->mesh().its;
    const size_t                n   = its.vertices.size();
    if (size_t(v_from) >= n || size_t(v_to) >= n)
        return;

    // Shortest path over the mesh's edge graph (Dijkstra, edge weight = length). Built on demand; one
    // pass per click is fine even on a dense mesh.
    std::vector<std::vector<std::pair<int, float>>> adj(n);
    for (const stl_triangle_vertex_indices &tri : its.indices)
        for (int i = 0; i < 3; ++i) {
            const int a = tri[i], b = tri[(i + 1) % 3];
            const float w = (its.vertices[size_t(a)] - its.vertices[size_t(b)]).norm();
            adj[size_t(a)].push_back({ b, w });
            adj[size_t(b)].push_back({ a, w });
        }

    std::vector<float> dist(n, std::numeric_limits<float>::infinity());
    std::vector<int>   prev(n, -1);
    using QN = std::pair<float, int>;
    std::priority_queue<QN, std::vector<QN>, std::greater<QN>> pq;
    dist[size_t(v_from)] = 0.f;
    pq.push({ 0.f, v_from });
    while (!pq.empty()) {
        const auto [d, u] = pq.top();
        pq.pop();
        if (d > dist[size_t(u)])
            continue;
        if (u == v_to)
            break;
        for (const auto &[w, ew] : adj[size_t(u)]) {
            const float nd = d + ew;
            if (nd < dist[size_t(w)]) {
                dist[size_t(w)] = nd;
                prev[size_t(w)] = u;
                pq.push({ nd, w });
            }
        }
    }
    if (prev[size_t(v_to)] < 0)
        return; // unreachable (disconnected components)

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Mark texture seam path"), UndoRedo::SnapshotType::GizmoAction);
    auto &seams = layer->lscm_seam_edges;
    for (int v = v_to; v != v_from && v >= 0; v = prev[size_t(v)]) {
        const int p = prev[size_t(v)];
        if (p < 0)
            break;
        const std::pair<int, int> e{ std::min(v, p), std::max(v, p) };
        if (std::find(seams.begin(), seams.end(), e) == seams.end())
            seams.push_back(e);
    }
    rebuild_preview();
}

void GLGizmoTextureDisplacement::rebuild_seam_anchor_overlay()
{
    m_seam_anchor_glmodel.reset();
    const ModelVolume *mv = texture_volume();
    if (!m_seam_edit_mode || !m_seam_path_mode || mv == nullptr || m_seam_path_anchor < 0)
        return;
    const indexed_triangle_set &its = mv->mesh().its;
    if (size_t(m_seam_path_anchor) >= its.vertices.size())
        return;

    // The anchor's incident edges, so the path's start vertex is visible on the model.
    GLModel::Geometry init_data;
    init_data.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
    unsigned nn = 0;
    for (const stl_triangle_vertex_indices &tri : its.indices)
        for (int i = 0; i < 3; ++i) {
            const int a = tri[i], b = tri[(i + 1) % 3];
            if (a == m_seam_path_anchor || b == m_seam_path_anchor) {
                init_data.add_vertex(its.vertices[size_t(a)]);
                init_data.add_vertex(its.vertices[size_t(b)]);
                init_data.add_line(nn, nn + 1);
                nn += 2;
            }
        }
    if (!init_data.is_empty())
        m_seam_anchor_glmodel.init_from(std::move(init_data));
}

void GLGizmoTextureDisplacement::rebuild_seam_hover_overlay()
{
    m_seam_hover_glmodel.reset();
    const ModelVolume *mv = texture_volume();
    if (!m_seam_edit_mode || mv == nullptr)
        return;
    const indexed_triangle_set &its = mv->mesh().its;

    GLModel::Geometry init_data;
    init_data.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };

    if (m_seam_path_mode) {
        // Highlight the hovered vertex as its ring of incident edges, so the click target is legible on
        // a dense mesh (matching the green anchor's style, in the hover yellow render_seam_overlay uses).
        if (m_seam_hover_vertex < 0 || size_t(m_seam_hover_vertex) >= its.vertices.size())
            return;
        unsigned nn = 0;
        for (const stl_triangle_vertex_indices &tri : its.indices)
            for (int i = 0; i < 3; ++i) {
                const int a = tri[i], b = tri[(i + 1) % 3];
                if (a == m_seam_hover_vertex || b == m_seam_hover_vertex) {
                    init_data.add_vertex(its.vertices[size_t(a)]);
                    init_data.add_vertex(its.vertices[size_t(b)]);
                    init_data.add_line(nn, nn + 1);
                    nn += 2;
                }
            }
        if (!init_data.is_empty())
            m_seam_hover_glmodel.init_from(std::move(init_data));
        return;
    }

    if (m_seam_hover_edge.first < 0 || size_t(m_seam_hover_edge.first) >= its.vertices.size() ||
        size_t(m_seam_hover_edge.second) >= its.vertices.size())
        return;
    init_data.reserve_vertices(2);
    init_data.reserve_indices(2);
    init_data.add_vertex(its.vertices[size_t(m_seam_hover_edge.first)]);
    init_data.add_vertex(its.vertices[size_t(m_seam_hover_edge.second)]);
    init_data.add_line(0, 1);
    m_seam_hover_glmodel.init_from(std::move(init_data));
}

void GLGizmoTextureDisplacement::rebuild_seam_overlay()
{
    m_seam_glmodel.reset();
    const ModelVolume              *mv    = texture_volume();
    const TextureDisplacementLayer *layer = active_layer();
    if (mv == nullptr || layer == nullptr || layer->lscm_seam_edges.empty())
        return;
    const indexed_triangle_set &its = mv->mesh().its;

    GLModel::Geometry init_data;
    init_data.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
    init_data.reserve_vertices(layer->lscm_seam_edges.size() * 2);
    init_data.reserve_indices(layer->lscm_seam_edges.size() * 2);
    unsigned n = 0;
    for (const auto &[a, b] : layer->lscm_seam_edges) {
        if (a < 0 || b < 0 || size_t(a) >= its.vertices.size() || size_t(b) >= its.vertices.size())
            continue;
        init_data.add_vertex(its.vertices[size_t(a)]);
        init_data.add_vertex(its.vertices[size_t(b)]);
        init_data.add_line(n, n + 1);
        n += 2;
    }
    if (!init_data.is_empty())
        m_seam_glmodel.init_from(std::move(init_data));
}

void GLGizmoTextureDisplacement::render_seam_overlay()
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    const ModelVolume *mv = texture_volume();
    const bool         have_marked = m_seam_glmodel.is_initialized();
    const bool         have_hover  = m_seam_edit_mode && m_seam_hover_glmodel.is_initialized();
    const bool         have_anchor = m_seam_edit_mode && m_seam_anchor_glmodel.is_initialized();
    if (mo == nullptr || mv == nullptr || (!have_marked && !have_hover && !have_anchor))
        return;
    GLShaderProgram *shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    const Selection  &selection    = m_parent.get_selection();
    const Transform3d trafo_matrix = mo->instances[selection.get_instance_idx()]->get_transformation().get_matrix() * mv->get_matrix();
    const Camera     &camera       = wxGetApp().plater()->get_camera();

    shader->start_using();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix() * trafo_matrix);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    glsafe(::glEnable(GL_POLYGON_OFFSET_LINE));
    glsafe(::glPolygonOffset(-2.0f, -2.0f)); // pull further forward than the wireframe so seams read on top
    // A seam edge is geometrically the same line as a wireframe edge, so a mere polygon offset is a
    // fragile way to make the red seam beat the white wireframe - drivers apply GL_POLYGON_OFFSET_LINE
    // inconsistently, and the two lines then z-fight and the wireframe wins. When the wireframe is on,
    // or while actively marking, just draw the seams with depth testing off so they are unconditionally
    // on top - being visible is the one thing this overlay has to guarantee.
    const bool seams_on_top = m_wireframe_overlay || m_seam_edit_mode;
    if (seams_on_top)
        glsafe(::glDisable(GL_DEPTH_TEST));
#if !SLIC3R_OPENGL_ES
    const bool wide = !OpenGLManager::get_gl_info().is_core_profile();
    if (wide)
        glsafe(::glLineWidth(4.0f));
#endif // !SLIC3R_OPENGL_ES
    if (have_marked) {
        m_seam_glmodel.set_color(ColorRGBA(1.0f, 0.15f, 0.15f, 1.0f)); // Blender's seam red
        m_seam_glmodel.render();
    }
    // The edge a click would toggle, in yellow and pulled the furthest forward, so it is unmistakable
    // which edge is being targeted while marking seams.
    if (have_hover) {
        glsafe(::glPolygonOffset(-3.0f, -3.0f));
        m_seam_hover_glmodel.set_color(ColorRGBA(1.0f, 0.9f, 0.15f, 1.0f));
        m_seam_hover_glmodel.render();
    }
    // The shortest-path start vertex, shown as its ring of incident edges in green.
    if (have_anchor) {
        glsafe(::glPolygonOffset(-3.0f, -3.0f));
        m_seam_anchor_glmodel.set_color(ColorRGBA(0.2f, 1.0f, 0.4f, 1.0f));
        m_seam_anchor_glmodel.render();
    }
#if !SLIC3R_OPENGL_ES
    if (wide)
        glsafe(::glLineWidth(1.0f));
#endif // !SLIC3R_OPENGL_ES
    glsafe(::glDisable(GL_POLYGON_OFFSET_LINE));
    if (seams_on_top)
        glsafe(::glEnable(GL_DEPTH_TEST));
    shader->stop_using();
}

void GLGizmoTextureDisplacement::render_preview_mesh()
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    const ModelVolume  *mv = texture_volume();
    if (mo == nullptr || mv == nullptr)
        return;

    const Selection  &selection    = m_parent.get_selection();
    const Transform3d trafo_matrix = mo->instances[selection.get_instance_idx()]->get_transformation().get_matrix() * mv->get_matrix();

    auto *shader = wxGetApp().get_shader("gouraud_light");
    if (shader == nullptr)
        return;
    shader->start_using();
    const Camera      &camera      = wxGetApp().plater()->get_camera();
    const Transform3d &view_matrix = camera.get_view_matrix();
    shader->set_uniform("view_model_matrix", view_matrix * trafo_matrix);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    const Matrix3d view_normal_matrix = view_matrix.matrix().block(0, 0, 3, 3) * trafo_matrix.matrix().block(0, 0, 3, 3).inverse().transpose();
    shader->set_uniform("view_normal_matrix", view_normal_matrix);
    if (m_preview_color_runs.empty()) {
        m_preview_glmodel.render();
    } else {
        // set_color() writes the uniform the shader reads, so one call per group is all the
        // per-triangle colour this needs.
        for (const PreviewColorRun &run : m_preview_color_runs) {
            m_preview_glmodel.set_color(run.color);
            m_preview_glmodel.render(run.range, shader);
        }
    }
    shader->stop_using();
}

float GLGizmoTextureDisplacement::layer_texture_aspect(const TextureDisplacementLayer &layer)
{
    // decode_height_texture() is cached on the image_data allocation, so this is a hash lookup rather
    // than a PNG decode - cheap enough to call per rebuild.
    const DecodedHeightTexture tex = decode_height_texture(layer);
    return (tex.width > 0 && tex.height > 0) ? float(tex.width) / float(tex.height) : 1.f;
}

std::vector<Vec2f> GLGizmoTextureDisplacement::compute_layer_vertex_uvs(const indexed_triangle_set     &patch,
                                                                       const TextureDisplacementLayer &layer) const
{
    const float aspect = layer_texture_aspect(layer);
    if (layer.projection_method == TextureProjectionMethod::LSCM) {
        // compute_lscm_uvs() returns the unwrap's own (raw, mm) coordinates with the island placement
        // folded in - it does *not* apply the layer's tiling/rotation/offset. The bake applies those
        // on top (sample_layer_height()'s lscm branch runs the result through sample_at()), so the
        // shader's precomputed-uv path has to as well, or the fast preview samples millimetre-valued
        // coordinates as if they were uv and shows the texture at a wildly wrong scale.
        std::vector<Vec2f> uv = compute_lscm_uvs(patch, layer);
        for (Vec2f &p : uv)
            p = apply_uv_transform(p, layer, aspect);
        return uv;
    }
    if (layer.projection_method == TextureProjectionMethod::ViewProjected) {
        std::vector<Vec2f> uv(patch.vertices.size());
        for (size_t vi = 0; vi < patch.vertices.size(); ++vi) {
            if (layer.view_project_projective) {
                // Matches sample_layer_height()'s projective branch, including skipping
                // apply_uv_transform(). A vertex behind the projector gets a uv far outside [0,1] so
                // it samples as nothing, rather than the mirrored coordinate a blind divide gives.
                if (!project_uv_projective(layer.view_project_matrix, patch.vertices[vi], uv[vi]))
                    uv[vi] = Vec2f(-1e6f, -1e6f);
                continue;
            }
            const Vec2f planar(patch.vertices[vi].dot(layer.view_project_right),
                               patch.vertices[vi].dot(layer.view_project_up));
            uv[vi] = apply_uv_transform(planar, layer, aspect);
        }
        return uv;
    }
    return {}; // Triplanar / Cylindrical / Spherical: the shader projects on its own
}

void GLGizmoTextureDisplacement::rebuild_bump_preview_mesh()
{
    m_bump_preview_glmodel.reset();

    const ModelVolume *mv = texture_volume();
    if (mv == nullptr || m_triangle_selectors.empty())
        return;

    // Uses the *live* selector (not the flushed model facet data), so this reflects an in-progress
    // stroke immediately rather than only once it ends - the point of this preview mode is to be
    // the fast, no-CPU-meshing one.
    const indexed_triangle_set patch = m_triangle_selectors[0]->get_facets_strict(EnforcerBlockerType::ENFORCER);
    if (patch.indices.empty())
        return;

    // No "patch.vertices.size() == mesh vertex count" check here, and that is the point: a *brush*
    // stroke splits triangles, so the selector appends split vertices and the patch array is longer
    // than the mesh's. An earlier version bailed out on that as "shouldn't happen", which meant the
    // bump model was never built while brushing and render_painter_gizmo() silently fell back to the
    // Normal (true-displacement) preview - Fast looked broken for brush and fine for Face/Connected
    // area, because only the brush splits. Everything below indexes the patch's own vertex array, so
    // the extra vertices are simply carried through.

    // Unpainted triangles, so the surrounding surface still renders (the render path hides the real
    // model in bump mode). get_facets_strict() returns the same vertex array whatever state is asked.
    const indexed_triangle_set rest = m_triangle_selectors[0]->get_facets_strict(EnforcerBlockerType::NONE);

    // For LSCM we hand the shader the finished per-vertex texture uv (island placement + tiling/
    // rotation/offset already folded in, exactly what the bake samples), because it cannot be
    // reconstructed in the fragment shader the way a triplanar projection can. This is also what
    // makes the fast preview follow the UV editor: the uvs move when an island is dragged, so this
    // mesh rebuilds (on drag end) with them. The other projections keep projecting in-shader.
    const TextureDisplacementLayer *active = active_layer();
    std::vector<Vec2f> vertex_uv = active != nullptr ? compute_layer_vertex_uvs(patch, *active) : std::vector<Vec2f>{};
    m_bump_preview_uses_vertex_uv = vertex_uv.size() == patch.vertices.size();
    if (!m_bump_preview_uses_vertex_uv)
        vertex_uv.clear();

    // Colour is quantized per *fragment* in the shader now (see the .fs), so this mesh carries no
    // colour of its own - the palette and the colour texture are uniforms, and every pixel matches the
    // image rather than the facet it landed on. What the *bake* will produce, at facet resolution, is
    // what the Normal view shows.
    m_bump_preview_palette = (active != nullptr && active->color_enabled) ? cached_palette()
                                                                         : std::vector<PaletteEntry>{};

    GLModel::Geometry init_data;
    // P3N3T2: normal.x carries the paint weight, tex_coord the precomputed uv (see the vertex shader).
    init_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3T2 };

    // Per-triangle weighting via a *flat* (unshared-vertex) mesh: every corner of a painted triangle
    // gets weight 1, every corner of an unpainted one weight 0. This is what a coarse mesh needs - one
    // painted face of a raw cube has no strictly-interior vertex (all 8 are shared), so per-vertex
    // weighting would either bleed onto the neighbours (boundary weight 1) or vanish outright (boundary
    // weight 0, which is what made a single face show nothing). Duplicating vertices costs no shading
    // quality here because the bump shader takes its surface normal from screen-space derivatives of
    // position (dFdx/dFdy), not from a per-vertex normal. normal.y flags the UV-editor island being
    // dragged so the shader can move just that island via the island_delta uniform.
    const bool     have_active = m_bump_active_chart >= 0 && !m_bump_active_vertex.empty();
    const size_t   tri_total   = patch.indices.size() + rest.indices.size();
    init_data.reserve_vertices(tri_total * 3);
    init_data.reserve_indices(tri_total * 3);
    unsigned vcount = 0;
    const auto emit_triangles = [&](const indexed_triangle_set &its, float weight) {
        for (const stl_triangle_vertex_indices &tri : its.indices) {
            for (int i = 0; i < 3; ++i) {
                const int   idx = tri[i];
                const float act = (have_active && idx >= 0 && size_t(idx) < m_bump_active_vertex.size() &&
                                   m_bump_active_vertex[size_t(idx)]) ? 1.f : 0.f;
                const Vec2f uv  = (weight > 0.5f && m_bump_preview_uses_vertex_uv && size_t(idx) < vertex_uv.size()) ?
                                      vertex_uv[size_t(idx)] : Vec2f::Zero();
                init_data.add_vertex(its.vertices[size_t(idx)], Vec3f(weight, act, 0.f), uv);
            }
            init_data.add_triangle(vcount, vcount + 1, vcount + 2);
            vcount += 3;
        }
    };
    emit_triangles(patch, 1.f); // painted -> bumped, and coloured by the shader
    // Untouched surface: flat, so it still shows but isn't bumped - and uncoloured, which is what the
    // bake leaves it as (EnforcerBlockerType::NONE, i.e. the volume's own filament).
    emit_triangles(rest, 0.f);

    m_bump_preview_glmodel.init_from(std::move(init_data));
    // GLModel::render() unconditionally re-sets the shader's "uniform_color" from this internal
    // color field right before drawing (see GLModel.cpp) - setting the uniform manually in
    // render_bump_preview_mesh() would just get overwritten by it, so it must be set here instead.
    // GLModel::Geometry defaults to BLACK, which is exactly what showed up before this was added.
    m_bump_preview_glmodel.set_color(GLVolume::NEUTRAL_COLOR);

    // The mesh now reflects the islands' current placement, so any live drag delta is measured from
    // here: reset it to identity and record the dragged island's baked transform.
    m_bump_island_delta = Eigen::Matrix<float, 2, 3>::Identity();
    const TextureDisplacementLayer *al = active_layer();
    if (m_bump_active_chart >= 0 && al != nullptr) {
        const std::vector<Eigen::Matrix<float, 2, 3>> xf = uv_editor_island_transforms(*al);
        m_bump_baked_active_xf = (size_t(m_bump_active_chart) < xf.size()) ? xf[size_t(m_bump_active_chart)]
                                                                           : Eigen::Matrix<float, 2, 3>::Identity();
    } else {
        m_bump_baked_active_xf = Eigen::Matrix<float, 2, 3>::Identity();
    }
}

void GLGizmoTextureDisplacement::compute_bump_active_vertices(const std::vector<int> &charts)
{
    m_bump_active_vertex.clear();
    const ModelVolume *mv = texture_volume();
    if (mv == nullptr || charts.empty())
        return;
    const PatchUnwrap &u = m_uv_editor_unwrap;
    m_bump_active_vertex.assign(mv->mesh().its.vertices.size(), 0);
    // Flag the base vertices of every chart being moved. For a group/multi move that is more than one
    // chart, but since such a move is a pure translation the shader applies the same delta to them all
    // (see on_island_edited) - exactly the "joined islands move together" behaviour.
    for (size_t i = 0; i < u.uvs.size(); ++i) {
        if (i >= u.vertex_chart.size() ||
            std::find(charts.begin(), charts.end(), u.vertex_chart[i]) == charts.end())
            continue;
        const int sv = (i < u.source_vertex.size()) ? u.source_vertex[i] : -1;
        if (sv >= 0 && size_t(sv) < m_bump_active_vertex.size())
            m_bump_active_vertex[size_t(sv)] = 1;
    }
}

int GLGizmoTextureDisplacement::island_group_of(const std::vector<int> &groups, int c)
{
    return (c >= 0 && size_t(c) < groups.size() && groups[size_t(c)] >= 0) ? groups[size_t(c)] : c;
}

void GLGizmoTextureDisplacement::join_island_groups(std::vector<int> &groups, int a, int b, int chart_count)
{
    if (a < 0 || b < 0 || chart_count <= 0)
        return;
    // Materialise to a full explicit table first, so singletons (which were implicit) get a concrete id
    // that the relabel loop below can match on.
    if (int(groups.size()) < chart_count) {
        const size_t old = groups.size();
        groups.resize(size_t(chart_count));
        for (size_t i = old; i < groups.size(); ++i)
            groups[i] = int(i);
    }
    for (size_t i = 0; i < groups.size(); ++i)
        if (groups[i] < 0)
            groups[i] = int(i);
    const int ga = groups[size_t(a)], gb = groups[size_t(b)];
    if (ga == gb)
        return;
    const int g = std::min(ga, gb);
    for (int &x : groups)
        if (x == ga || x == gb)
            x = g;
}

std::vector<int> GLGizmoTextureDisplacement::build_island_move_set(const TextureDisplacementLayer &layer, int primary) const
{
    std::vector<int> set;
    const int        chart_count = std::max(m_uv_editor_unwrap.chart_count, 0);

    // Seed with the pane's multi-selection (falling back to just the primary if the canvas has none).
    std::vector<int> seeds;
    if (const UVEditorCanvas *canvas = wxGetApp().plater()->get_uv_editor_canvas())
        seeds = canvas->selected_islands();
    if (seeds.empty() && primary >= 0)
        seeds.push_back(primary);

    const auto add = [&set](int c) {
        if (c >= 0 && std::find(set.begin(), set.end(), c) == set.end())
            set.push_back(c);
    };
    for (int s : seeds) {
        add(s);
        // Pull in every chart sharing s's join group, so a joined pair moves as one.
        const int gs = island_group_of(layer.island_groups, s);
        for (int c = 0; c < chart_count; ++c)
            if (island_group_of(layer.island_groups, c) == gs)
                add(c);
    }
    add(primary); // never leave the primary out, whatever the selection state
    return set;
}

void GLGizmoTextureDisplacement::render_bump_preview_mesh()
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    const ModelVolume  *mv = texture_volume();
    if (mo == nullptr || mv == nullptr || !m_bump_preview_glmodel.is_initialized())
        return;

    const TextureDisplacementLayer *layer = active_layer();
    if (layer == nullptr || layer->empty())
        return;

    // Full-resolution height upload (smoothing-aware), whose grayscale value lives in the R channel
    // exactly as the shader samples it. Deliberately *not* the layer-list panel's thumbnail: that one
    // is box-filtered down to 128 px for a ~48 px row, and feeding it to the shader cost the preview
    // three quarters of the height map's detail - and, since height_tex_texel is derived from it, also
    // flattened the shading gradient and made the parallax march skip itself at angles where it should
    // run. Cached on the image_data pointer + smoothing, so no PNG is decoded per frame.
    GLTexture *tex = get_layer_height_texture(*layer);
    if (tex == nullptr || tex->get_width() <= 0 || tex->get_height() <= 0)
        return;

    GLShaderProgram *shader = wxGetApp().get_shader("texture_displacement_bump");
    if (shader == nullptr)
        return;

    const Selection  &selection    = m_parent.get_selection();
    const Transform3d trafo_matrix = mo->instances[selection.get_instance_idx()]->get_transformation().get_matrix() * mv->get_matrix();
    const Camera      &camera      = wxGetApp().plater()->get_camera();

    shader->start_using();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix() * trafo_matrix);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    shader->set_uniform("volume_world_matrix", trafo_matrix);
    const ClippingPlaneDataWrapper clp_data = this->get_clipping_plane_data();
    shader->set_uniform("clipping_plane", clp_data.clp_dataf);
    shader->set_uniform("z_range", clp_data.z_range);
    const Matrix3d view_normal_matrix =
        camera.get_view_matrix().matrix().block(0, 0, 3, 3) * trafo_matrix.matrix().block(0, 0, 3, 3).inverse().transpose();
    shader->set_uniform("view_normal_matrix", view_normal_matrix);
    shader->set_uniform("volume_mirrored", trafo_matrix.matrix().determinant() < 0.0);
    glsafe(::glActiveTexture(GL_TEXTURE0));
    glsafe(::glBindTexture(GL_TEXTURE_2D, tex->get_id()));
    // Match DecodedHeightTexture::sample()'s tiling. The sampler's wrap mode is the only place the
    // GPU path can express this, and nothing ever set it - so it sat at GL_REPEAT no matter what the
    // layer said: a MirroredRepeat layer previewed as a plain repeat, and a layer with tiling *off*
    // previewed as an endless tiling where the bake produces one placement and nothing around it.
    // CLAMP_TO_BORDER with a zero border is the exact analogue of sample()'s "outside [0,1) is 0".
    // GL_CLAMP_TO_BORDER is desktop-GL only; on ES the nearest thing is CLAMP_TO_EDGE, which smears
    // the border row instead of vanishing - still much closer to the bake than an endless repeat.
#if SLIC3R_OPENGL_ES
    const GLint no_tile_wrap = GL_CLAMP_TO_EDGE;
#else
    const GLint no_tile_wrap = GL_CLAMP_TO_BORDER;
#endif
    const GLint wrap = !layer->tile_enabled                                      ? no_tile_wrap :
                       (layer->tile_method == TextureTileMethod::MirroredRepeat) ? GL_MIRRORED_REPEAT :
                                                                                   GL_REPEAT;
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap));
#if !SLIC3R_OPENGL_ES
    if (wrap == GL_CLAMP_TO_BORDER) {
        static const GLfloat border[4] = { 0.f, 0.f, 0.f, 0.f };
        glsafe(::glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border));
    }
#endif // !SLIC3R_OPENGL_ES
    shader->set_uniform("height_tex", 0);
    shader->set_uniform("height_tex_texel", Vec2f(1.f / float(tex->get_width()), 1.f / float(tex->get_height())));
    shader->set_uniform("depth_mm", layer->depth_mm);
    shader->set_uniform("tiling_scale", layer->tiling_scale);
    // Read off the uploaded texture rather than the decoded one: they are the same image, and this is
    // the aspect the sampler will actually see.
    shader->set_uniform("tex_aspect", float(tex->get_width()) / float(tex->get_height()));
    shader->set_uniform("rotation_rad", layer->rotation_deg * float(M_PI) / 180.f);
    shader->set_uniform("uv_offset", layer->offset);
    shader->set_uniform("invert", layer->invert);
    // The parallax step in the triplanar path needs the real height, not just its gradient, so it
    // needs the midlevel the bake subtracts - and the camera position in the volume's own local space,
    // to build the view ray it walks along. Without the parallax the pattern is welded to the base
    // surface: it does not slide as the camera orbits and does not deepen with depth_mm, which is
    // exactly when the fast preview stops looking like geometry.
    shader->set_uniform("midlevel", layer->midlevel);
    shader->set_uniform("eye_model_pos", Vec3f((trafo_matrix.inverse() * camera.get_position()).cast<float>()));
    // When set, the shader samples at the per-vertex uv baked into the mesh (LSCM) rather than
    // projecting; see rebuild_bump_preview_mesh().
    shader->set_uniform("use_vertex_uv", m_bump_preview_uses_vertex_uv);

    // The filament palette the mesh's per-triangle indices refer to. Count 0 means "no layer is
    // colouring", and the shader keeps the model's own colour for every fragment.
    // The printable palette, in RGB for display and in Lab for the match. Uploaded rather than
    // matched on the CPU because the quantization is per fragment here.
    const GLTexture *color_tex = get_layer_color_texture(*layer);
    const int        palette_count =
        (color_tex != nullptr) ? int(std::min(m_bump_preview_palette.size(), size_t(PALETTE_MAX_ENTRIES))) : 0;
    shader->set_uniform("palette_count", palette_count);
    shader->set_uniform("has_color_tex", color_tex != nullptr);
    for (int i = 0; i < palette_count; ++i) {
        const Vec3f &rgb = m_bump_preview_palette[size_t(i)].rgb;
        float        l, a, b;
        RGB2Lab(rgb.x() * 255.f, rgb.y() * 255.f, rgb.z() * 255.f, &l, &a, &b);
        shader->set_uniform(("palette_rgb[" + std::to_string(i) + "]").c_str(), rgb);
        shader->set_uniform(("palette_lab[" + std::to_string(i) + "]").c_str(), Vec3f(l, a, b));
    }
    if (color_tex != nullptr) {
        shader->set_uniform("color_tex", 1);
        glsafe(::glActiveTexture(GL_TEXTURE1));
        glsafe(::glBindTexture(GL_TEXTURE_2D, (GLuint) color_tex->get_id()));
        glsafe(::glActiveTexture(GL_TEXTURE0));
    }
    // The live UV-editor island drag rides this 2x3 affine (identity except mid-drag); only the flagged
    // island's vertices apply it, so a drag is a uniform update rather than a mesh rebuild.
    const Eigen::Matrix<float, 2, 3> &d = m_bump_island_delta;
    shader->set_uniform("island_delta_lin", std::array<float, 4>{ d(0, 0), d(0, 1), d(1, 0), d(1, 1) });
    shader->set_uniform("island_delta_tr", Vec2f(d(0, 2), d(1, 2)));
    m_bump_preview_glmodel.render();
    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
    shader->stop_using();
}

bool GLGizmoTextureDisplacement::bump_preview_ready() const
{
    // Mirrors render_bump_preview_mesh()'s own preconditions. Kept as a separate query because the
    // caller has to know whether the bump pass will draw *before* it hides the real volume for it.
    if (m_c->selection_info() == nullptr || m_c->selection_info()->model_object() == nullptr)
        return false;
    if (texture_volume() == nullptr)
        return false;
    const TextureDisplacementLayer *layer = active_layer();
    if (layer == nullptr || layer->empty())
        return false;
    // The same texture render_bump_preview_mesh() will bind, not the panel thumbnail - the two are
    // separate caches and either can fail on its own.
    const GLTexture *tex = const_cast<GLGizmoTextureDisplacement *>(this)->get_layer_height_texture(*layer);
    if (tex == nullptr || tex->get_width() <= 0 || tex->get_height() <= 0)
        return false;
    return wxGetApp().get_shader("texture_displacement_bump") != nullptr;
}

void GLGizmoTextureDisplacement::rebuild_paint_overlay()
{
    m_paint_overlay_glmodel.reset();
    const ModelVolume *mv = texture_volume();
    if (mv == nullptr || m_triangle_selectors.empty())
        return;

    // The *live* selector, so an in-progress stroke shows immediately - which is the whole point:
    // this is the only feedback that a brush actually added or erased anything until the (much more
    // expensive) preview catches up at stroke end.
    const indexed_triangle_set patch = m_triangle_selectors[0]->get_facets_strict(EnforcerBlockerType::ENFORCER);
    if (patch.indices.empty())
        return;

    // In the true-displacement view the surface on screen is the *raised* one, and a tint built on
    // the flat base mesh would sink underneath it wherever the relief is deepest - which is precisely
    // where the user is looking. The bake is topology-preserving (patch vertex i is mesh vertex i, see
    // build_texture_displacement()), so the displaced positions can be read straight across. Vertices
    // the brush split live past the end of that array and keep their flat position; they sit on the
    // patch boundary, where the displacement is smallest anyway.
    const std::vector<Vec3f> *displaced = nullptr;
    if (!m_use_bump_preview && m_preview_its.vertices.size() == mv->mesh().its.vertices.size() &&
        !m_preview_its.vertices.empty())
        displaced = &m_preview_its.vertices;

    GLModel::Geometry init_data;
    init_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
    init_data.reserve_vertices(patch.indices.size() * 3);
    init_data.reserve_indices(patch.indices.size() * 3);
    unsigned n = 0;
    for (const stl_triangle_vertex_indices &tri : patch.indices) {
        for (int i = 0; i < 3; ++i) {
            const size_t idx = size_t(tri[i]);
            init_data.add_vertex((displaced != nullptr && idx < displaced->size()) ? (*displaced)[idx]
                                                                                   : patch.vertices[idx]);
        }
        init_data.add_triangle(n, n + 1, n + 2);
        n += 3;
    }
    m_paint_overlay_glmodel.init_from(std::move(init_data));
    // GLModel::render() re-sets "uniform_color" from this field just before drawing, so the colour
    // has to be set here rather than as a uniform at draw time.
    m_paint_overlay_glmodel.set_color(ColorRGBA(0.16f, 0.79f, 0.35f, 0.38f));
}

void GLGizmoTextureDisplacement::render_paint_overlay()
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    const ModelVolume *mv = texture_volume();
    if (mo == nullptr || mv == nullptr || !m_paint_overlay_glmodel.is_initialized())
        return;
    GLShaderProgram *shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    const Selection  &selection    = m_parent.get_selection();
    const Transform3d trafo_matrix = mo->instances[selection.get_instance_idx()]->get_transformation().get_matrix() * mv->get_matrix();
    const Camera     &camera       = wxGetApp().plater()->get_camera();

    shader->start_using();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix() * trafo_matrix);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    // Translucent, and pulled toward the camera so it wins the depth test against the coincident
    // bump surface. Depth writes are off: this is a tint, and letting it own the depth buffer would
    // make the wireframe and seam overlays drawn after it fight with geometry that is not really
    // there. Blending is already enabled by render_painter_gizmo().
    glsafe(::glEnable(GL_POLYGON_OFFSET_FILL));
    glsafe(::glPolygonOffset(-1.5f, -1.5f));
    glsafe(::glDepthMask(GL_FALSE));
    m_paint_overlay_glmodel.render();
    glsafe(::glDepthMask(GL_TRUE));
    glsafe(::glDisable(GL_POLYGON_OFFSET_FILL));
    shader->stop_using();
}

void GLGizmoTextureDisplacement::rebuild_uvcheck_mesh()
{
    m_uvcheck_glmodel.reset();
    if (m_uv_check_mode == UVCheckMode::None)
        return;

    const ModelVolume *mv = texture_volume();
    if (mv == nullptr || m_triangle_selectors.empty())
        return;
    const indexed_triangle_set patch = m_triangle_selectors[0]->get_facets_strict(EnforcerBlockerType::ENFORCER);
    if (patch.indices.empty())
        return;
    // Everything below works in the *patch's* vertex space, not the mesh's. Those agree only until a
    // brush stroke splits a triangle, after which the patch array is longer - and since patch triangle
    // indices are used to index it, reading the mesh's array instead would run off the end. An earlier
    // version guarded that by bailing out, which quietly disabled the Checker and Distortion overlays
    // for anything painted with the brush.
    const TextureDisplacementLayer *layer = active_layer();
    if (layer == nullptr)
        return;

    // The checker samples wherever the projection puts it; the projections the shader can't
    // reconstruct (LSCM, ViewProjected) get a precomputed per-vertex uv, the rest project in-shader.
    std::vector<Vec2f> uv       = compute_layer_vertex_uvs(patch, *layer);
    const bool         have_uvs = uv.size() == patch.vertices.size();
    m_uvcheck_uses_vertex_uv    = have_uvs;

    // Per-vertex area distortion in [0,1] (0.5 == ideal), only when both requested and possible.
    std::vector<float> distortion(patch.vertices.size(), 0.5f);
    if (m_uv_check_mode == UVCheckMode::Distortion && have_uvs) {
        std::vector<float> tri_log(patch.indices.size(), 0.f);
        for (size_t f = 0; f < patch.indices.size(); ++f) {
            const stl_triangle_vertex_indices &t = patch.indices[f];
            const float a3 = 0.5f * (patch.vertices[t[1]] - patch.vertices[t[0]]).cross(patch.vertices[t[2]] - patch.vertices[t[0]]).norm();
            const Vec2f e0 = uv[t[1]] - uv[t[0]];
            const Vec2f e1 = uv[t[2]] - uv[t[0]];
            const float a2 = 0.5f * std::abs(e0.x() * e1.y() - e0.y() * e1.x());
            tri_log[f] = (a3 > 1e-12f && a2 > 1e-12f) ? std::log2(a2 / a3) : 0.f;
        }
        // Centre the heatmap on the patch's own median stretch, so a globally-scaled unwrap reads as
        // uniformly "ideal" and only *relative* stretching (the thing that matters) shows up as colour.
        std::vector<float> sorted = tri_log;
        float              median = 0.f;
        if (!sorted.empty()) {
            std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
            median = sorted[sorted.size() / 2];
        }
        std::vector<float> sum(patch.vertices.size(), 0.f);
        std::vector<int>   cnt(patch.vertices.size(), 0);
        for (size_t f = 0; f < patch.indices.size(); ++f) {
            // +/- 2 stops (4x stretch either way) spans the full blue->red range.
            const float d = std::clamp(0.5f + (tri_log[f] - median) / 4.f, 0.f, 1.f);
            for (int k = 0; k < 3; ++k) {
                sum[patch.indices[f][k]] += d;
                ++cnt[patch.indices[f][k]];
            }
        }
        for (size_t v = 0; v < distortion.size(); ++v)
            if (cnt[v] > 0)
                distortion[v] = sum[v] / float(cnt[v]);
    }

    GLModel::Geometry init_data;
    init_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3T2 };
    init_data.reserve_vertices(patch.vertices.size());
    init_data.reserve_indices(patch.indices.size() * 3);
    for (size_t vi = 0; vi < patch.vertices.size(); ++vi)
        init_data.add_vertex(patch.vertices[vi], Vec3f(distortion[vi], 0.f, 0.f),
                             have_uvs ? uv[vi] : Vec2f::Zero());
    for (const stl_triangle_vertex_indices &tri : patch.indices)
        init_data.add_triangle(unsigned(tri[0]), unsigned(tri[1]), unsigned(tri[2]));

    m_uvcheck_glmodel.init_from(std::move(init_data));
}

void GLGizmoTextureDisplacement::render_uvcheck_mesh()
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    const ModelVolume *mv = texture_volume();
    if (mo == nullptr || mv == nullptr || !m_uvcheck_glmodel.is_initialized())
        return;
    const TextureDisplacementLayer *layer = active_layer();
    if (layer == nullptr)
        return;
    GLShaderProgram *shader = wxGetApp().get_shader("texture_displacement_uvcheck");
    if (shader == nullptr)
        return;

    const Selection  &selection    = m_parent.get_selection();
    const Transform3d trafo_matrix = mo->instances[selection.get_instance_idx()]->get_transformation().get_matrix() * mv->get_matrix();
    const Camera     &camera       = wxGetApp().plater()->get_camera();

    shader->start_using();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix() * trafo_matrix);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    shader->set_uniform("volume_world_matrix", trafo_matrix);
    const ClippingPlaneDataWrapper clp_data = this->get_clipping_plane_data();
    shader->set_uniform("clipping_plane", clp_data.clp_dataf);
    shader->set_uniform("z_range", clp_data.z_range);
    const Matrix3d view_normal_matrix =
        camera.get_view_matrix().matrix().block(0, 0, 3, 3) * trafo_matrix.matrix().block(0, 0, 3, 3).inverse().transpose();
    shader->set_uniform("view_normal_matrix", view_normal_matrix);
    shader->set_uniform("volume_mirrored", trafo_matrix.matrix().determinant() < 0.0);
    shader->set_uniform("mode", m_uv_check_mode == UVCheckMode::Distortion ? 1 : 0);
    shader->set_uniform("checker_freq", 4.f); // squares per texture tile
    shader->set_uniform("tiling_scale", layer->tiling_scale);
    shader->set_uniform("rotation_rad", layer->rotation_deg * float(M_PI) / 180.f);
    shader->set_uniform("uv_offset", layer->offset);
    shader->set_uniform("use_vertex_uv", m_uvcheck_uses_vertex_uv);

    // Coincident with the base surface, so pull it toward the camera to win the depth test.
    glsafe(::glEnable(GL_POLYGON_OFFSET_FILL));
    glsafe(::glPolygonOffset(-1.0f, -1.0f));
    m_uvcheck_glmodel.render();
    glsafe(::glDisable(GL_POLYGON_OFFSET_FILL));
    shader->stop_using();
}

void GLGizmoTextureDisplacement::build_wireframe_from_its(const indexed_triangle_set &its)
{
    m_wireframe_overlay_glmodel.reset();
    m_wireframe_overlay_vcount = its.vertices.size();
    if (its.indices.empty())
        return;

    GLModel::Geometry init_data;
    init_data.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
    init_data.reserve_vertices(its.vertices.size());
    init_data.reserve_indices(its.indices.size() * 6);
    for (const Vec3f &v : its.vertices)
        init_data.add_vertex(v);
    // One segment per triangle edge; shared edges drawn twice, harmless for a wireframe and far
    // cheaper than deduplicating a million of them.
    for (const stl_triangle_vertex_indices &tri : its.indices)
        for (int i = 0; i < 3; ++i)
            init_data.add_line(unsigned(tri[i]), unsigned(tri[(i + 1) % 3]));

    if (!init_data.is_empty())
        m_wireframe_overlay_glmodel.init_from(std::move(init_data));
}

void GLGizmoTextureDisplacement::rebuild_wireframe_overlay()
{
    if (!m_wireframe_overlay) {
        m_wireframe_overlay_glmodel.reset();
        m_wireframe_overlay_vcount = 0;
        return;
    }
    const ModelVolume *mv = texture_volume();
    if (mv == nullptr)
        return;
    const indexed_triangle_set &its = mv->mesh().its;
    if (its.indices.empty())
        return;

    // Building from the base mesh (bump/paint mode); its topology only changes on bake/subdivide, and
    // this runs on every rebuild_preview(), so rebuild only when the vertex count actually changes.
    if (m_wireframe_overlay_glmodel.is_initialized() && m_wireframe_overlay_vcount == its.vertices.size())
        return;
    build_wireframe_from_its(its);
}

void GLGizmoTextureDisplacement::refresh_wireframe()
{
    if (!m_wireframe_overlay) {
        m_wireframe_overlay_glmodel.reset();
        m_wireframe_overlay_vcount = 0;
        return;
    }
    // The wireframe has to sit on whatever mesh is actually on screen. In the true-displacement view
    // that is the raised preview geometry (m_preview_its) - drawing the flat base mesh's edges there
    // leaves them buried inside the bumps, which is why the wireframe "didn't show in real mode". In
    // Fast (bump) mode or with nothing painted, the surface is the undisplaced base mesh.
    if (!m_use_bump_preview && !m_preview_its.indices.empty())
        build_wireframe_from_its(m_preview_its);
    else
        rebuild_wireframe_overlay();
}

void GLGizmoTextureDisplacement::render_wireframe_overlay()
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    const ModelVolume *mv = texture_volume();
    if (mo == nullptr || mv == nullptr || !m_wireframe_overlay_glmodel.is_initialized())
        return;
    GLShaderProgram *shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    const Selection  &selection    = m_parent.get_selection();
    const Transform3d trafo_matrix = mo->instances[selection.get_instance_idx()]->get_transformation().get_matrix() * mv->get_matrix();
    const Camera     &camera       = wxGetApp().plater()->get_camera();

    shader->start_using();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix() * trafo_matrix);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    // Pull the lines toward the camera so they sit on the surface rather than z-fighting into it.
    glsafe(::glEnable(GL_POLYGON_OFFSET_LINE));
    glsafe(::glPolygonOffset(-1.0f, -1.0f));
    m_wireframe_overlay_glmodel.set_color(ColorRGBA(1.0f, 1.0f, 1.0f, 0.6f)); // white, so it reads on any material
    m_wireframe_overlay_glmodel.render();
    glsafe(::glDisable(GL_POLYGON_OFFSET_LINE));
    shader->stop_using();
}

void GLGizmoTextureDisplacement::rebuild_preview()
{
    // Bumped first: any in-flight job's result (captured generation from before this call) will
    // now compare unequal to m_preview_generation and be discarded when it completes, even if it
    // finishes after the job queued below - and, since the counter is shared with the worker, that
    // job also notices mid-run and aborts rather than computing a result nobody will use.
    m_preview_generation->fetch_add(1);
    update_uv_editor();
    rebuild_bump_preview_mesh();
    rebuild_paint_overlay();
    rebuild_uvcheck_mesh();
    rebuild_seam_overlay();
    // The adaptive subdivision preview is driven by the painted area, so it has to follow the paint
    // while it is open - a plain stroke changes which triangles would be refined. The uniform preview
    // depends only on the mesh, so it is left to its own controls.
    if (m_subdivide_editing && m_subdivide_adaptive)
        rebuild_subdivide_preview();

    const ModelVolume *mv = texture_volume();
    if (mv == nullptr || !mv->is_texture_displacement_painted()) {
        m_preview_glmodel.reset();
        m_preview_its = indexed_triangle_set{}; // no displaced mesh; wireframe falls back to the base
        m_preview_job_pending = false;
        refresh_wireframe();
        return;
    }
    // In Fast/paint modes the wireframe follows the base mesh and can be built now; the true-displacement
    // view's wireframe needs the displaced mesh, which only exists once the job below completes.
    if (m_use_bump_preview) {
        refresh_wireframe();
        // Fast view: the shader *is* the preview, and m_preview_glmodel is never drawn. Running the
        // full CPU displacement anyway - which is what happened on every stroke and slider release -
        // was the single largest cost in the gizmo, and it bought nothing. The switch back to the
        // true-displacement view queues it (see the View row in on_render_input_window()).
        m_preview_job_pending = false;
        return;
    }

    queue_preview_job();
}

void GLGizmoTextureDisplacement::queue_preview_job()
{
    // A job in flight when the gizmo closes still runs its completion handler, which would otherwise
    // happily queue the follow-up run it was holding - against a gizmo nobody is looking at any more.
    if (m_state != On)
        return;
    const ModelVolume *mv = texture_volume();
    if (mv == nullptr || !mv->is_texture_displacement_painted())
        return;

    // One in flight at a time; everything requested meanwhile collapses into a single follow-up run
    // issued from the completion handler. See m_preview_job_running.
    if (m_preview_job_running) {
        m_preview_job_pending = true;
        return;
    }

    const uint64_t generation = m_preview_generation->load();

    TextureDisplacementPreviewInput input;
    input.base_mesh = mv->mesh().its;
    input.layers    = mv->texture_displacement_layers;
    input.options   = mv->texture_displacement_options;
    for (int i = 0; i < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++i)
        input.facets_data[size_t(i)] = mv->texture_displacement_facet(i).get_data();
    // Captured here rather than read in the handler: get_extruders_colors() is main-thread state and
    // the preview has to be grouped against the same palette it was computed with, not whatever is
    // loaded by the time it lands.
    input.color = color_settings_for(*mv);
    // The filament list the result's indices refer to, captured with the job rather than read back
    // when it lands - loading a filament meanwhile must not recolour a preview computed against a
    // different list.
    const std::vector<ColorRGBA> filaments = m_palette_filaments;

    m_preview_job_running = true;
    auto &worker = wxGetApp().plater()->get_ui_job_worker();
    queue_job(worker, std::make_unique<TextureDisplacementPreviewJob>(std::move(input), generation, m_preview_generation,
        [this, filaments](TextureDisplacementPreviewResult result, uint64_t result_generation) {
            indexed_triangle_set its = std::move(result.mesh);
            m_preview_job_running = false;
            if (result_generation != m_preview_generation->load()) {
                // Superseded while this was computing (it will have aborted early and come back
                // empty). Whatever the newest state is, it still needs a run.
                m_preview_job_pending = true;
            } else if (its.indices.empty()) {
                // Aborted or cancelled rather than finished - the handler runs on every outcome so
                // the in-flight latch above always clears. Keep whatever preview is already on screen
                // rather than blanking it; there is no new result to show, not a new empty one.
            } else {
                m_preview_glmodel.reset();
                m_preview_color_runs.clear();
                if (result.triangle_color.size() == its.indices.size() && !filaments.empty()) {
                    // Group by *filament*, not by palette entry: what the bake wrote is the resolved
                    // filament, interleaving already applied, so this shows the real banding rather
                    // than the flat average the eye will turn it into.
                    indexed_triangle_set sorted;
                    sorted.vertices = its.vertices;
                    sorted.indices.reserve(its.indices.size());
                    for (int want = 0; want <= int(filaments.size()); ++want) {
                        const size_t first = sorted.indices.size();
                        for (size_t i = 0; i < its.indices.size(); ++i)
                            if (int(result.triangle_color[i]) == want)
                                sorted.indices.push_back(its.indices[i]);
                        if (sorted.indices.size() == first)
                            continue;
                        m_preview_color_runs.push_back(
                            { { first * 3, sorted.indices.size() * 3 },
                              want == 0 ? GLVolume::NEUTRAL_COLOR : filaments[size_t(want - 1)] });
                    }
                    m_preview_glmodel.init_from(sorted);
                } else {
                    m_preview_glmodel.init_from(its);
                }
                m_preview_glmodel.set_color(GLVolume::NEUTRAL_COLOR);
                // Keep the displaced mesh so the wireframe overlay can be drawn on it (the
                // true-displacement view), then refresh the wireframe from it.
                m_preview_its = std::move(its);
                refresh_wireframe();
                // The paint tint rides the displaced surface in this view, so it follows the new mesh.
                m_paint_overlay_dirty = true;
            }
            if (m_preview_job_pending) {
                m_preview_job_pending = false;
                if (!m_use_bump_preview)
                    queue_preview_job(); // no-ops if the gizmo has closed in the meantime
            }
            m_parent.set_as_dirty();
        }));
}

void GLGizmoTextureDisplacement::update_uv_editor()
{
    Plater         *plater    = wxGetApp().plater();
    UVEditorCanvas *uv_canvas = plater->get_uv_editor_canvas();
    if (uv_canvas == nullptr)
        return;

    const ModelVolume        *mv    = texture_volume();
    TextureDisplacementLayer *layer = active_layer();
    // The pane is opened only on the user's explicit request (m_show_uv_editor), and only for an LSCM
    // layer - never automatically just because something is painted. Keep the cached state/unwrap so
    // that switching the toggle back on re-shows instantly (and re-solves if the paint changed while
    // it was hidden, via the state comparison below).
    if (!m_show_uv_editor || mv == nullptr || layer == nullptr ||
        layer->projection_method != TextureProjectionMethod::LSCM) {
        plater->show_uv_editor(false);
        return;
    }

    // A vertex/edge edit committing (or an undo reverting one) changes the per-vertex UV overrides
    // without going through the Unwrap button. Detect that and force a re-solve, so the pane's geometry
    // stays in step with what will bake - the one exception to "only re-solve on Unwrap".
    {
        size_t sig = 1469598103934665603ull; // FNV-1a seed
        const auto mix = [&sig](uint64_t x) { sig = (sig ^ x) * 1099511628211ull; };
        mix(layer->lscm_uv_overrides.size());
        for (const auto &[v, uv] : layer->lscm_uv_overrides) {
            mix(uint64_t(uint32_t(v)));
            mix(uint64_t(uint32_t(int32_t(std::llround(uv.x() * 1024.f)))));
            mix(uint64_t(uint32_t(int32_t(std::llround(uv.y() * 1024.f)))));
        }
        if (sig != m_uv_overrides_sig) {
            m_uv_overrides_sig  = sig;
            m_uv_unwrap_pending = true;
        }
    }

    UVEditorState state;
    state.slot       = m_active_layer_slot;
    state.image_data = layer->image_data.get();
    state.seam_angle = layer->lscm_seam_angle_deg;
    state.padding    = layer->island_padding_mm;
    state.facets     = mv->texture_displacement_facet(m_active_layer_slot).get_data();
    state.seam_edges = layer->lscm_seam_edges;

    // The re-solve happens only when the user pressed "Unwrap" (m_uv_unwrap_pending). Every other call
    // into here - a paint stroke ending, a slider release, the check mode changing - must not pay for
    // a fresh LSCM solve; it just re-applies the cheap affine transforms over whatever unwrap already
    // exists. If the paint changed underneath but the user hasn't asked to re-unwrap, the pane keeps
    // showing the last unwrap on purpose (that is the whole point of making it an explicit action).
    bool unwrap_changed = false;
    if (m_uv_unwrap_pending) {
        m_uv_unwrap_pending = false;
        const indexed_triangle_set patch = extract_painted_patch(mv->mesh().its, state.facets);
        if (patch.indices.empty()) {
            m_uv_editor_state  = UVEditorState{};
            m_uv_editor_unwrap = PatchUnwrap{};
            m_uv_editor_distortion_colors.clear();
            plater->show_uv_editor(false);
            return;
        }
        // Padding disabled (0): the user asked to pack islands with no gap between them.
        m_uv_editor_unwrap = compute_patch_unwrap(patch, layer->lscm_seam_angle_deg, 0.f, layer->lscm_seam_edges);
        // Re-apply any stored per-vertex UV edits onto the fresh unwrap, so the pane shows exactly what
        // compute_lscm_uvs() will bake (which applies the same overrides). Keyed by mesh vertex, so every
        // unwrapped copy of that vertex gets it - matching the bake's single-UV-per-vertex settle.
        if (!layer->lscm_uv_overrides.empty()) {
            std::map<int, Vec2f> ov;
            for (const auto &[mv2, uv] : layer->lscm_uv_overrides)
                ov[mv2] = uv;
            for (size_t i = 0; i < m_uv_editor_unwrap.uvs.size(); ++i) {
                const int sv = (i < m_uv_editor_unwrap.source_vertex.size()) ? m_uv_editor_unwrap.source_vertex[i] : -1;
                const auto it = ov.find(sv);
                if (it != ov.end())
                    m_uv_editor_unwrap.uvs[i] = it->second;
            }
        }
        m_uv_editor_state  = std::move(state);
        unwrap_changed     = true;
        // Precompute the distortion heatmap now, while the patch is in hand - relative stretch doesn't
        // change when islands are only moved, so this need not be redone on a drag. It is fed to the
        // canvas below only while the Distortion check mode is on.
        compute_uv_editor_distortion_colors(patch);
    }

    if (m_uv_editor_unwrap.empty()) {
        // Nothing has been unwrapped yet (or the paint was cleared): keep the pane hidden until the user
        // presses Unwrap. The panel shows a "Press Unwrap" hint in this state.
        plater->show_uv_editor(false);
        return;
    }

    // The pane background either mirrors the height texture (default) or shows a UV checker (#7), and is
    // only re-uploaded when that choice, or the unwrap, actually changes - the height image is large.
    const UVBackground desired_bg = (m_uv_check_mode == UVCheckMode::Checker) ? UVBackground::Checker : UVBackground::Height;
    const bool bg_smoothing_changed = (desired_bg == UVBackground::Height) && (m_uv_editor_bg_smoothing != layer->smoothing);
    if (unwrap_changed || desired_bg != m_uv_editor_bg || bg_smoothing_changed) {
        m_uv_editor_bg_smoothing = layer->smoothing;
        if (desired_bg == UVBackground::Checker) {
            // An even squares-per-axis count so the pattern tiles seamlessly across the UV unit
            // boundary (texcoord == position repeats it once per tile). Softened grays, not pure
            // black/white, so it doesn't fight the island wires drawn over it.
            constexpr int              tex = 512, squares = 8, cell = tex / squares;
            std::vector<unsigned char> checker(size_t(tex) * size_t(tex));
            for (int y = 0; y < tex; ++y)
                for (int x = 0; x < tex; ++x)
                    checker[size_t(y) * tex + x] = ((x / cell + y / cell) & 1) ? 205 : 70;
            uv_canvas->set_background_texture(checker, tex, tex);
        } else {
            const DecodedHeightTexture height = decode_height_texture(*layer);
            if (!height.empty())
                uv_canvas->set_background_texture(height.pixels, height.width, height.height);
            else
                uv_canvas->set_background_texture({}, 0, 0);
        }
        m_uv_editor_bg = desired_bg;
    }

    // Grow (never shrink) the layer's island list to cover every chart. Shrinking would throw away a
    // hand placement the moment a stroke temporarily merged two islands, and a stale extra entry is
    // harmless - island_transform_matrix() only looks up the charts that actually exist.
    if (layer->islands.size() < size_t(m_uv_editor_unwrap.chart_count))
        layer->islands.resize(size_t(m_uv_editor_unwrap.chart_count));

    // Connected-net layout (on by default): a *fresh* unwrap is unfolded so adjacent charts sit
    // edge-to-edge (cube -> a net), rather than as separately packed squares. Only when the user pressed
    // Unwrap (m_uv_apply_connected_net) - a re-segmentation renumbers charts anyway, so any hand
    // placement from before is already meaningless. A *refresh* re-solve (a committed vertex edit, or an
    // undo) must NOT relayout, or it would throw away every island placement on every vertex edit.
    if (unwrap_changed && m_uv_apply_connected_net && layer->auto_connect_islands) {
        std::vector<TextureIsland> net = compute_connected_net(m_uv_editor_unwrap);
        if (net.size() == size_t(m_uv_editor_unwrap.chart_count)) {
            if (layer->islands.size() < net.size())
                layer->islands.resize(net.size());
            for (size_t i = 0; i < net.size(); ++i)
                layer->islands[i] = net[i];
        }
    }
    if (unwrap_changed)
        m_uv_apply_connected_net = false; // consumed; a refresh re-solve leaves placements alone

    // The geometry goes over in the unwrap's *raw* mm coordinates and is only re-uploaded when the
    // unwrap itself changed. Everything a slider or a drag can touch - island placement, tiling,
    // rotation, offset - is an affine map on top of that, so it goes over as one 2x3 matrix per
    // island instead. That is the whole reason dragging an island is now free: a patch of a million
    // triangles has a million UVs to re-transform and re-upload otherwise, and it was doing exactly
    // that on every single mouse-move event.
    if (unwrap_changed) {
        UVEditorCanvas::Islands view;
        view.uvs            = m_uv_editor_unwrap.uvs;
        view.indices        = m_uv_editor_unwrap.indices;
        view.vertex_island  = m_uv_editor_unwrap.vertex_chart;
        view.boundary_edges = m_uv_editor_unwrap.boundary_edges;
        view.island_count   = m_uv_editor_unwrap.chart_count;

        uv_canvas->set_island_edit_callback(
            [this](int island, const Vec2f &offset_delta, float rotation_delta, float scale_factor, bool finished) {
                on_island_edited(island, offset_delta, rotation_delta, scale_factor, finished);
            });
        uv_canvas->set_vertex_edit_callback(
            [this](const std::vector<std::pair<int, Vec2f>> &edits) { on_uv_vertex_edited(edits); });
        uv_canvas->set_command_callback([this](UVEditorCanvas::Command cmd) { on_uv_command(int(cmd)); });
        uv_canvas->set_islands(std::move(view));
    }

    uv_canvas->set_select_mode(static_cast<UVEditorCanvas::SelectMode>(m_uv_select_mode));

    uv_canvas->set_uv_transform(layer->tiling_scale, layer->rotation_deg, layer->tile_enabled,
                                layer->tile_method == TextureTileMethod::MirroredRepeat);
    uv_canvas->set_island_transforms(uv_editor_island_transforms(*layer));
    // The distortion heatmap tints the island fills only while its check mode is on; otherwise the
    // canvas falls back to its default light-green wash.
    if (m_uv_check_mode == UVCheckMode::Distortion)
        uv_canvas->set_island_fill_colors(m_uv_editor_distortion_colors);
    else
        uv_canvas->set_island_fill_colors({});

    plater->show_uv_editor(true);
}

void GLGizmoTextureDisplacement::compute_uv_editor_distortion_colors(const indexed_triangle_set &patch)
{
    m_uv_editor_distortion_colors.clear();
    const PatchUnwrap &u = m_uv_editor_unwrap;
    if (u.empty() || u.chart_count <= 0)
        return;

    // log2(uv area / 3D area) per unwrap triangle - the same measure the 3D distortion overlay uses.
    std::vector<float> tri_log(u.indices.size(), 0.f);
    std::vector<int>   tri_chart(u.indices.size(), -1);
    for (size_t f = 0; f < u.indices.size(); ++f) {
        const stl_triangle_vertex_indices &t = u.indices[f];
        if (t[0] < 0 || size_t(t[0]) >= u.vertex_chart.size())
            continue;
        tri_chart[f] = u.vertex_chart[size_t(t[0])];
        const auto p3 = [&](int uv_idx) -> Vec3f {
            const int sv = (size_t(uv_idx) < u.source_vertex.size()) ? u.source_vertex[size_t(uv_idx)] : -1;
            return (sv >= 0 && size_t(sv) < patch.vertices.size()) ? patch.vertices[size_t(sv)] : Vec3f::Zero();
        };
        const float a3 = 0.5f * (p3(t[1]) - p3(t[0])).cross(p3(t[2]) - p3(t[0])).norm();
        const Vec2f e0 = u.uvs[size_t(t[1])] - u.uvs[size_t(t[0])];
        const Vec2f e1 = u.uvs[size_t(t[2])] - u.uvs[size_t(t[0])];
        const float a2 = 0.5f * std::abs(e0.x() * e1.y() - e0.y() * e1.x());
        tri_log[f] = (a3 > 1e-12f && a2 > 1e-12f) ? std::log2(a2 / a3) : 0.f;
    }

    // Centre on the median stretch, so a globally scaled unwrap reads as uniform and only *relative*
    // stretching shows up - matching the 3D overlay's convention.
    std::vector<float> sorted = tri_log;
    float              median = 0.f;
    if (!sorted.empty()) {
        std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
        median = sorted[sorted.size() / 2];
    }

    std::vector<double> chart_sum(size_t(u.chart_count), 0.0);
    std::vector<int>    chart_cnt(size_t(u.chart_count), 0);
    for (size_t f = 0; f < tri_log.size(); ++f) {
        const int c = tri_chart[f];
        if (c >= 0 && c < u.chart_count) {
            chart_sum[size_t(c)] += tri_log[f];
            ++chart_cnt[size_t(c)];
        }
    }

    // Blue (compressed) -> green (ideal) -> red (stretched), +/- 2 stops spanning the full range.
    const auto heat = [](float t) -> ColorRGBA {
        t = std::clamp(t, 0.f, 1.f);
        const ColorRGBA blue{ 0.15f, 0.35f, 1.0f, 0.5f }, green{ 0.2f, 0.9f, 0.3f, 0.5f }, red{ 1.0f, 0.2f, 0.15f, 0.5f };
        if (t < 0.5f) { const float s = t * 2.f;         return blue * (1.f - s) + green * s; }
        const float s = (t - 0.5f) * 2.f;                return green * (1.f - s) + red * s;
    };

    m_uv_editor_distortion_colors.resize(size_t(u.chart_count), heat(0.5f));
    for (int c = 0; c < u.chart_count; ++c)
        if (chart_cnt[size_t(c)] > 0) {
            const float avg = float(chart_sum[size_t(c)] / chart_cnt[size_t(c)]);
            m_uv_editor_distortion_colors[size_t(c)] = heat(std::clamp(0.5f + (avg - median) / 4.f, 0.f, 1.f));
        }
}

void GLGizmoTextureDisplacement::on_uv_command(int cmd)
{
    TextureDisplacementLayer *layer = active_layer();
    if (layer == nullptr)
        return;

    if (cmd == int(UVEditorCanvas::Command::AverageScale)) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Average island scale"), UndoRedo::SnapshotType::GizmoAction);
        average_island_scales(layer->islands);
        rebuild_preview();
    } else if (cmd == int(UVEditorCanvas::Command::CutSelectedIsland)) {
        UVEditorCanvas *canvas = wxGetApp().plater()->get_uv_editor_canvas();
        const int       chart  = canvas != nullptr ? canvas->selected_island() : -1;
        if (chart < 0) {
            show_error(nullptr, _u8L("Select an island in the UV editor first."));
            return;
        }
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Cut texture island"), UndoRedo::SnapshotType::GizmoAction);
        cut_island(*layer, chart);
        rebuild_preview();
    } else if (cmd == int(UVEditorCanvas::Command::JoinSelected)) {
        UVEditorCanvas *canvas = wxGetApp().plater()->get_uv_editor_canvas();
        const int       chart  = canvas != nullptr ? canvas->selected_island() : -1;
        if (chart < 0) {
            show_error(nullptr, _u8L("Select an island in the UV editor first."));
            return;
        }
        // Join to whichever neighbouring island (one it shares an edge with) is currently placed
        // nearest - i.e. the one it was dragged up against.
        const Eigen::Matrix<float, 2, 3> sel_m = island_transform_matrix(chart, m_uv_editor_unwrap, layer->islands);
        const Vec2f sel_c = sel_m.block<2, 2>(0, 0) * m_uv_editor_unwrap.chart_centroid[size_t(chart)] + sel_m.col(2);
        int   best_parent = -1;
        float best_d2     = std::numeric_limits<float>::max();
        TextureIsland best_place, cand;
        for (int p = 0; p < m_uv_editor_unwrap.chart_count; ++p) {
            if (p == chart)
                continue;
            if (!join_chart_placement(m_uv_editor_unwrap, layer->islands, chart, p, cand))
                continue; // not a neighbour
            const Eigen::Matrix<float, 2, 3> pm = island_transform_matrix(p, m_uv_editor_unwrap, layer->islands);
            const Vec2f pc = pm.block<2, 2>(0, 0) * m_uv_editor_unwrap.chart_centroid[size_t(p)] + pm.col(2);
            const float d2 = (pc - sel_c).squaredNorm();
            if (d2 < best_d2) { best_d2 = d2; best_parent = p; best_place = cand; }
        }
        if (best_parent < 0) {
            show_error(nullptr, _u8L("This island has no neighbour it shares an edge with."));
            return;
        }
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Join texture island"), UndoRedo::SnapshotType::GizmoAction);
        if (layer->islands.size() <= size_t(chart))
            layer->islands.resize(size_t(chart) + 1);
        layer->islands[size_t(chart)] = best_place;
        // Record the join so the two (and anything already grouped with either) move together from now
        // on, not just visually snap once.
        join_island_groups(layer->island_groups, chart, best_parent, m_uv_editor_unwrap.chart_count);
        rebuild_preview();
    } else if (cmd == int(UVEditorCanvas::Command::UnjoinSelected)) {
        UVEditorCanvas *canvas = wxGetApp().plater()->get_uv_editor_canvas();
        const int       chart  = canvas != nullptr ? canvas->selected_island() : -1;
        if (chart < 0 || size_t(chart) >= layer->islands.size()) {
            show_error(nullptr, _u8L("Select an island in the UV editor first."));
            return;
        }
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Unjoin texture island"), UndoRedo::SnapshotType::GizmoAction);
        layer->islands[size_t(chart)] = TextureIsland{}; // back to its own packed position
        // Break its join link too, so it stops moving with the others (the rest stay grouped).
        if (size_t(chart) < layer->island_groups.size())
            layer->island_groups[size_t(chart)] = chart;
        rebuild_preview();
    }
    // FrameAll/ToggleSnap are handled inside the canvas; ProjectFromView is not wired yet.
}

void GLGizmoTextureDisplacement::capture_view_projection(TextureDisplacementLayer &layer)
{
    const ModelVolume *mv = texture_volume();
    const ModelObject *mo = m_c->selection_info()->model_object();
    if (mv == nullptr || mo == nullptr)
        return;

    const Camera     &camera    = wxGetApp().plater()->get_camera();
    const Selection  &selection = m_parent.get_selection();
    const Transform3d trafo     = mo->instances[selection.get_instance_idx()]->get_transformation().get_matrix() * mv->get_matrix();

    // The view matrix's rotation rows are the camera axes in world space; bring them into the
    // volume's local frame (where the mesh vertices live) so the projector rides along with the part.
    const Matrix3d view_rot   = camera.get_view_matrix().matrix().block<3, 3>(0, 0);
    const Matrix3d trafo_rot  = trafo.matrix().block<3, 3>(0, 0);
    const Matrix3d world_to_local = trafo_rot.inverse();
    const Vec3d    local_right = world_to_local * Vec3d(view_rot.row(0).transpose());
    const Vec3d    local_up    = world_to_local * Vec3d(view_rot.row(1).transpose());

    // Unit axes: the projected planar coordinate must stay in mm so tiling_scale keeps meaning mm.
    layer.view_project_right = local_right.norm() > 1e-9 ? Vec3f(local_right.normalized().cast<float>()) : Vec3f::UnitX();
    layer.view_project_up    = local_up.norm() > 1e-9 ? Vec3f(local_up.normalized().cast<float>()) : Vec3f::UnitY();
}

void GLGizmoTextureDisplacement::show_projector(bool show)
{
    if (!show) {
        if (m_projector_frame != nullptr)
            m_projector_frame->Hide();
        return;
    }

    if (m_projector_frame == nullptr) {
        // Parented to the main frame, not to Plater: Plater is a wxPanel, and wxFRAME_FLOAT_ON_PARENT
        // wants a real top-level window to float above.
        m_projector_frame         = new TextureProjectorFrame(wxGetApp().mainframe);
        m_projector_tex_source    = nullptr; // fresh window, nothing uploaded into it yet
        m_projector_tex_smoothing = -1.f;
        m_projector_frame->set_opacity(m_projector_opacity);

        // Opened centred over the 3D canvas, at about half its size: the frame is meant to be
        // dragged onto part of the model, so starting somewhere on top of it beats the OS's default
        // cascade position, which is often off over the sidebar.
        if (const wxGLCanvas *cnv = m_parent.get_wxglcanvas(); cnv != nullptr) {
            const wxRect  area = cnv->GetScreenRect();
            const wxSize  size(std::max(160, area.width / 2), std::max(160, area.height / 2));
            m_projector_frame->SetSize(wxRect(area.GetTopLeft() + wxPoint((area.width - size.x) / 2,
                                                                         (area.height - size.y) / 2),
                                              size));
        }
    }

    m_projector_frame->Show();
    m_projector_frame->Raise();
    update_projector();
}

void GLGizmoTextureDisplacement::update_projector()
{
    if (m_projector_frame == nullptr || !m_projector_frame->IsShown())
        return;

    const TextureDisplacementLayer *layer = active_layer();
    if (layer == nullptr || layer->projection_method != TextureProjectionMethod::ViewProjected) {
        m_projector_frame->set_texture({}, 0, 0);
        m_projector_tex_source    = nullptr;
        m_projector_tex_smoothing = -1.f;
        return;
    }

    // Only re-upload when the pixels actually changed - this is reached from the panel's per-edit
    // flush, and rebuilding the bitmap from identical bytes every time would be pure waste.
    if (m_projector_tex_source != layer->image_data.get() || m_projector_tex_smoothing != layer->smoothing) {
        const DecodedHeightTexture height = decode_height_texture(*layer);
        if (height.empty())
            m_projector_frame->set_texture({}, 0, 0);
        else
            m_projector_frame->set_texture(height.pixels, height.width, height.height);
        m_projector_tex_source    = layer->image_data.get();
        m_projector_tex_smoothing = layer->smoothing;
    }
}

int GLGizmoTextureDisplacement::apply_projection_frame()
{
    TextureDisplacementLayer *layer = active_layer();
    const ModelVolume        *mv    = texture_volume();
    const ModelObject        *mo    = m_c->selection_info()->model_object();
    wxGLCanvas               *cnv   = m_parent.get_wxglcanvas();
    if (layer == nullptr || mv == nullptr || mo == nullptr || cnv == nullptr || m_projector_frame == nullptr ||
        !m_projector_frame->IsShown())
        return -1;

    // The frame's gate, brought from screen coordinates into the GL viewport's pixel space. Two
    // conversions, both necessary: ScreenToClient() because the viewport's origin is the canvas's
    // top-left, not the desktop's, and the retina scale because the viewport is sized in physical
    // pixels (see GLCanvas3D::get_canvas_size()) while wx hands out logical ones.
    const wxRect  gate   = m_projector_frame->client_rect_on_screen();
    const wxPoint tl     = cnv->ScreenToClient(gate.GetTopLeft());
    const double  scale  = double(m_parent.get_scale());
    const double  rx = double(tl.x) * scale, ry = double(tl.y) * scale;
    const double  rw = double(gate.width) * scale, rh = double(gate.height) * scale;
    if (rw < 1.0 || rh < 1.0)
        return -1;

    const Camera             &camera = wxGetApp().plater()->get_camera();
    const std::array<int, 4> &vp     = camera.get_viewport();
    const Selection          &sel    = m_parent.get_selection();
    const Transform3d trafo = mo->instances[sel.get_instance_idx()]->get_transformation().get_matrix() * mv->get_matrix();

    // Local space -> clip space, the same product the renderer uses, so the mapping agrees with what
    // is actually on screen rather than with an idealisation of it.
    const Eigen::Matrix4d K = camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix() * trafo.matrix();

    // Window coordinates follow igl::project's convention (as CameraUtils::project does):
    //     win_x = vp.x + vp.w * (ndc.x + 1) / 2,  and y measured downward as vp.h - win_y_gl.
    // Turning those into uv = (win - rect_origin) / rect_size gives u and v as affine functions of
    // ndc.x and ndc.y, and since ndc = clip.xyz / clip.w, multiplying through by clip.w leaves a
    // plain linear combination of K's rows - i.e. one 3x4 matrix carrying the perspective divide.
    const double A = double(vp[2]) / (2.0 * rw);
    const double B = (double(vp[0]) + double(vp[2]) * 0.5 - rx) / rw;
    const double C = -double(vp[3]) / (2.0 * rh);
    const double D = (double(vp[3]) * 0.5 - double(vp[1]) - ry) / rh;

    const Eigen::Vector4d row_u = A * K.row(0).transpose() + B * K.row(3).transpose();
    const Eigen::Vector4d row_v = C * K.row(1).transpose() + D * K.row(3).transpose();
    const Eigen::Vector4d row_w = K.row(3).transpose();

    std::array<float, 12> m{};
    for (int i = 0; i < 4; ++i) {
        m[size_t(i)]     = float(row_u[i]);
        m[size_t(4 + i)] = float(row_v[i]);
        m[size_t(8 + i)] = float(row_w[i]);
    }

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Apply texture projection frame"),
                                  UndoRedo::SnapshotType::GizmoAction);

    layer->projection_method       = TextureProjectionMethod::ViewProjected;
    layer->view_project_projective = true;
    layer->view_project_matrix     = m;
    // Off, so the height sampler returns 0 outside [0,1) and the frame's border becomes a hard edge
    // of the displacement rather than the first seam of an endless repeat.
    layer->tile_enabled = false;
    // The affine axes are kept up to date too, so turning the projective mapping off later leaves a
    // sane flat projection from this same viewpoint instead of whatever was captured long ago.
    capture_view_projection(*layer);

    const int painted = select_visible_faces(&m);
    m_preview_params_dirty = true;
    rebuild_preview();
    m_parent.set_as_dirty();
    return painted;
}

void GLGizmoTextureDisplacement::cut_island(TextureDisplacementLayer &layer, int chart)
{
    const ModelVolume *mv = texture_volume();
    if (mv == nullptr)
        return;
    const std::vector<Vec3f> &verts = mv->mesh().its.vertices;
    const PatchUnwrap        &u     = m_uv_editor_unwrap;

    // Collect the chart's triangles back in mesh-vertex space, and its 3D bounding box.
    std::vector<std::array<int, 3>> tris;
    Vec3f lo(std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
    Vec3f hi(std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest());
    for (const stl_triangle_vertex_indices &t : u.indices) {
        if (t[0] < 0 || size_t(t[0]) >= u.vertex_chart.size() || u.vertex_chart[size_t(t[0])] != chart)
            continue;
        std::array<int, 3> bt{};
        bool               ok = true;
        for (int k = 0; k < 3; ++k) {
            const int uvv = t[k];
            if (uvv < 0 || size_t(uvv) >= u.source_vertex.size()) { ok = false; break; }
            const int base = u.source_vertex[size_t(uvv)];
            if (base < 0 || size_t(base) >= verts.size()) { ok = false; break; }
            bt[k] = base;
            lo    = lo.cwiseMin(verts[size_t(base)]);
            hi    = hi.cwiseMax(verts[size_t(base)]);
        }
        if (ok)
            tris.push_back(bt);
    }
    if (tris.empty())
        return;

    // Cut perpendicular to the longest axis, through the centroid - a long thin island is split
    // across its narrow middle, which is exactly the "islands might be very long" case.
    const Vec3f  ext  = hi - lo;
    const int    axis = (ext.x() >= ext.y() && ext.x() >= ext.z()) ? 0 : (ext.y() >= ext.z() ? 1 : 2);
    const float  mid  = 0.5f * (lo[axis] + hi[axis]);

    std::set<std::pair<int, int>> seams(layer.lscm_seam_edges.begin(), layer.lscm_seam_edges.end());
    for (const std::array<int, 3> &bt : tris)
        for (int i = 0; i < 3; ++i) {
            const int a = bt[i], b = bt[(i + 1) % 3];
            // Endpoints on opposite sides of the plane -> this edge crosses it -> make it a seam.
            if ((verts[size_t(a)][axis] < mid) != (verts[size_t(b)][axis] < mid))
                seams.insert({ std::min(a, b), std::max(a, b) });
        }
    layer.lscm_seam_edges.assign(seams.begin(), seams.end());
}

// unwrap mm -> texture uv, per island: the island's own hand placement, then the layer's
// tiling/rotation/offset. Both are affine, so they compose into one matrix the canvas can hand
// straight to a shader.
std::vector<Eigen::Matrix<float, 2, 3>>
GLGizmoTextureDisplacement::uv_editor_island_transforms(const TextureDisplacementLayer &layer)
{
    const float uv_scale = (layer.tiling_scale > 1e-6f) ? (1.f / layer.tiling_scale) : 1.f;
    const float rad      = layer.rotation_deg * float(M_PI) / 180.f;
    const float cs       = std::cos(rad) * uv_scale;
    const float sn       = std::sin(rad) * uv_scale;

    Eigen::Matrix2f uv_linear;
    uv_linear << cs, -sn,
                 sn,  cs;

    m_uv_editor_bbox_min = Vec2f(std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
    m_uv_editor_bbox_max = Vec2f(std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest());

    std::vector<Eigen::Matrix<float, 2, 3>> transforms(size_t(std::max(m_uv_editor_unwrap.chart_count, 0)));
    for (int c = 0; c < m_uv_editor_unwrap.chart_count; ++c) {
        const Eigen::Matrix<float, 2, 3> island = island_transform_matrix(c, m_uv_editor_unwrap, layer.islands);
        Eigen::Matrix<float, 2, 3>      &m      = transforms[size_t(c)];
        m.block<2, 2>(0, 0) = uv_linear * island.block<2, 2>(0, 0);
        m.col(2)            = uv_linear * island.col(2) + layer.offset;
    }

    // Only for the panel's readout; the canvas computes its own bounds.
    for (size_t i = 0; i < m_uv_editor_unwrap.uvs.size(); ++i) {
        const int c = m_uv_editor_unwrap.vertex_chart[i];
        if (c < 0 || size_t(c) >= transforms.size())
            continue;
        const Vec2f uv = transforms[size_t(c)].block<2, 2>(0, 0) * m_uv_editor_unwrap.uvs[i] + transforms[size_t(c)].col(2);
        m_uv_editor_bbox_min = m_uv_editor_bbox_min.cwiseMin(uv);
        m_uv_editor_bbox_max = m_uv_editor_bbox_max.cwiseMax(uv);
    }
    return transforms;
}

void GLGizmoTextureDisplacement::on_island_edited(int island, const Vec2f &offset_delta, float rotation_delta,
                                                  float scale_factor, bool finished)
{
    TextureDisplacementLayer *layer = active_layer();
    if (layer == nullptr || island < 0)
        return;
    if (size_t(island) >= layer->islands.size())
        layer->islands.resize(size_t(island) + 1);

    // A pure translation (no rotation, no scale) is the only gesture that moves a whole group/selection
    // together; rotate and scale stay on the primary island alone. This split is what makes flagging a
    // group's worth of vertices on the GPU safe: the shared shader delta is a pure translation, so the
    // same offset is correct for every flagged island.
    const bool is_move = rotation_delta == 0.f && scale_factor == 1.f;

    if (!m_island_drag_active) {
        // Taken before the first delta lands, so one undo reverts the whole drag rather than just
        // its final mouse-move.
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Move texture island"), UndoRedo::SnapshotType::GizmoAction);
        m_island_drag_active = true;
        // Decide the moved set once, at drag start: the whole selection + join groups for a move, or
        // just the primary for a rotate/scale.
        m_island_move_set = is_move ? build_island_move_set(*layer, island) : std::vector<int>{ island };
        // Set up the GPU drag: flag the moved islands' vertices and bake the mesh once (via the dirty
        // flag). From then on the drag is a uniform update, no rebuild - see render_bump_preview_mesh().
        m_bump_active_chart = island;
        compute_bump_active_vertices(m_island_move_set);
        m_bump_island_delta  = Eigen::Matrix<float, 2, 3>::Identity();
        m_bump_preview_dirty = true;
    }

    // Apply the edit. A move goes to every island in the moved set (same offset -> they translate as
    // one); a rotate/scale goes only to the primary, about its own centroid.
    const std::vector<int>  single{ island };
    const std::vector<int> &targets = (is_move && !m_island_move_set.empty()) ? m_island_move_set : single;
    for (int c : targets) {
        if (c < 0)
            continue;
        if (size_t(c) >= layer->islands.size())
            layer->islands.resize(size_t(c) + 1);
        TextureIsland &target = layer->islands[size_t(c)];
        target.offset += offset_delta;
        if (c == island) {
            target.rotation_deg += rotation_delta;
            // Guarded: a scale that reaches zero is unrecoverable (every subsequent factor multiplies it)
            // and would collapse the island to a point - exactly the failure this feature hit once already.
            target.scale = std::clamp(target.scale * scale_factor, 0.001f, 1000.f);
        }
    }

    if (finished) {
        m_island_drag_active = false;
        m_bump_active_chart  = -1;
        m_bump_active_vertex.clear();
        m_island_move_set.clear();
        m_bump_island_delta  = Eigen::Matrix<float, 2, 3>::Identity();
        rebuild_preview(); // the real displaced geometry moved: recompute it once, at the end
    } else {
        const std::vector<Eigen::Matrix<float, 2, 3>> xf = uv_editor_island_transforms(*layer);
        if (UVEditorCanvas *uv_canvas = wxGetApp().plater()->get_uv_editor_canvas()) {
            // Live feedback in the pane, and deliberately *just* the transforms: nothing about the
            // unwrap changed, so none of the vertex buffers need touching. This is what makes a drag
            // interactive on a patch with a million triangles.
            uv_canvas->set_island_transforms(xf);
        }
        // Move the island on the model live through the shader's island_delta uniform - no mesh
        // rebuild. delta = F_current * F_baked^-1 in final-uv space (the bump mesh bakes F_baked; the
        // shader applies delta to the flagged island's uv). The one rebuild that bakes the flags is
        // scheduled at drag start above and consumed once per frame by render_painter_gizmo().
        if (m_use_bump_preview && m_bump_active_chart == island && size_t(island) < xf.size()) {
            Eigen::Matrix3f cur = Eigen::Matrix3f::Identity();
            cur.topRows<2>()    = xf[size_t(island)];
            Eigen::Matrix3f bak = Eigen::Matrix3f::Identity();
            bak.topRows<2>()    = m_bump_baked_active_xf;
            m_bump_island_delta = (cur * bak.inverse()).topRows<2>();
            m_parent.set_as_dirty();
        }
    }
}

void GLGizmoTextureDisplacement::on_uv_vertex_edited(const std::vector<std::pair<int, Vec2f>> &edits)
{
    TextureDisplacementLayer *layer = active_layer();
    if (layer == nullptr || edits.empty() || m_uv_editor_unwrap.empty())
        return;

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Edit texture UV"), UndoRedo::SnapshotType::GizmoAction);

    for (const auto &[unwrapped, raw_uv] : edits) {
        if (unwrapped < 0 || size_t(unwrapped) >= m_uv_editor_unwrap.source_vertex.size())
            continue;
        // Keep the gizmo's own unwrap copy in step, so the pane and the bake agree without a re-solve.
        if (size_t(unwrapped) < m_uv_editor_unwrap.uvs.size())
            m_uv_editor_unwrap.uvs[size_t(unwrapped)] = raw_uv;

        const int mesh_v = m_uv_editor_unwrap.source_vertex[size_t(unwrapped)];
        if (mesh_v < 0)
            continue;
        // Store (or update) the override for this mesh vertex. Small list, linear scan is fine.
        auto it = std::find_if(layer->lscm_uv_overrides.begin(), layer->lscm_uv_overrides.end(),
                               [mesh_v](const std::pair<int, Vec2f> &p) { return p.first == mesh_v; });
        if (it != layer->lscm_uv_overrides.end())
            it->second = raw_uv;
        else
            layer->lscm_uv_overrides.emplace_back(mesh_v, raw_uv);
    }

    rebuild_preview(); // the baked displacement samples the moved uv now
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
    // Mirrors project_planar()'s own dominant-axis choice exactly, so the ring's "0 degrees" and
    // the offset handle's plane always agree with what project_texture_displacement_uv() does.
    const Vec3f n = m_adjust_anchor_normal.cwiseAbs();
    if (n.x() >= n.y() && n.x() >= n.z()) {
        u_axis = Vec3f::UnitY();
        v_axis = Vec3f::UnitZ();
    } else if (n.y() >= n.x() && n.y() >= n.z()) {
        u_axis = Vec3f::UnitX();
        v_axis = Vec3f::UnitZ();
    } else {
        u_axis = Vec3f::UnitX();
        v_axis = Vec3f::UnitY();
    }
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

void GLGizmoTextureDisplacement::update_model_object()
{
    bool         updated = false;
    ModelObject *mo      = m_c->selection_info()->model_object();
    int          idx     = -1;
    for (ModelVolume *mv : mo->volumes) {
        if (!mv->is_model_part())
            continue;
        ++idx;
        updated |= mv->texture_displacement_facet(m_active_layer_slot).set(*m_triangle_selectors[idx]);
    }

    // The fast (bump) preview reads the live selector, so it has to be rebuilt after any stroke that
    // flushes here - not only when set() reports a change. Rebuilding it via rebuild_preview() below
    // is gated on `updated`, which misses e.g. the first paint into a slot; marking it dirty makes the
    // render loop (render_painter_gizmo) rebuild it next frame regardless. Without this, fast preview -
    // now the default view - stayed blank until a full reload (select-whole-model / reopen).
    m_bump_preview_dirty = true;

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

    const ModelObject *mo = m_c->selection_info()->model_object();
    m_triangle_selectors.clear();

    std::vector<ColorRGBA> ebt_colors;
    ebt_colors.push_back(GLVolume::NEUTRAL_COLOR);
    ebt_colors.push_back(TriangleSelectorGUI::enforcers_color);
    ebt_colors.push_back(TriangleSelectorGUI::blockers_color);
    for (const ModelVolume *mv : mo->volumes) {
        if (!mv->is_model_part())
            continue;

        const TriangleMesh *mesh = &mv->mesh();
        m_triangle_selectors.emplace_back(std::make_unique<TriangleSelectorPatch>(*mesh, ebt_colors));
        m_triangle_selectors.back()->deserialize(mv->texture_displacement_facet(m_active_layer_slot).get_data(), false);
        m_triangle_selectors.back()->request_update_render_data();
    }

    // Start a freshly opened, never-textured volume with one layer already in place, so the panel is
    // ready to paint straight away rather than showing an empty layer list. Only on first open, and
    // only when there are none - never during an undo/redo or layer-switch reload (which also come
    // through here), where silently adding a layer would be wrong.
    if (first_update)
        if (ModelVolume *tv = texture_volume(); tv != nullptr && tv->texture_displacement_layers.empty()) {
            add_texture_layer(); // takes its own snapshot and rebuilds the preview
            return;
        }

    rebuild_preview();
}

void GLGizmoTextureDisplacement::set_active_layer(int slot)
{
    if (slot == m_active_layer_slot)
        return;
    // Flush edits made while the previous layer was active before switching what the selectors
    // reflect - otherwise they would be silently lost.
    update_model_object();
    m_active_layer_slot = slot;
    update_from_model_object(false);
    // The on-canvas gizmo (if on) is anchored to whichever layer is active - keep it in sync
    // instead of leaving it pointing at the previous layer's (now stale) paint patch.
    if (m_adjust_texture_mode)
        update_adjust_anchor();
    // Refresh every preview/overlay (bump, UV editor, seams, ...) for the newly active layer.
    rebuild_preview();
}

unsigned int GLGizmoTextureDisplacement::tool_icon_id()
{
    if (!m_tool_icon_tried) {
        m_tool_icon_tried = true;
        // Runs from the panel render, i.e. with a GL context current, so the upload is safe here.
        m_tool_icon.load_from_svg_file(resources_dir() + "/images/texture_displacement_add.svg", false, false, false, 32);
    }
    return m_tool_icon.get_id();
}

void GLGizmoTextureDisplacement::ensure_panel_icons()
{
    if (m_panel_icons_tried)
        return;
    m_panel_icons_tried = true;

    // Order is irrelevant; the map keys by file name. Loaded with color_wite_gray so each icon has both
    // a monochrome ("normal", grey in the current theme) and an original-colour variant.
    static const std::vector<std::string> names = {
        "toolbar_big_brush.svg", "toolbar_face.svg", "texture_displacement_connected_area.svg",
        "texture_displacement_real_preview.svg", "texture_displacement_fast_preview.svg",
        "texture_displacement_checker.svg", "texture_displacement_distortion.svg",
        "texture_displacement_wireframe.svg", "texture_displacement_cross.svg",
        "texture_displacement_uv_select_island.svg", "texture_displacement_uv_select_edge.svg",
        "texture_displacement_uv_select_vertex.svg",
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

void GLGizmoTextureDisplacement::add_texture_layer()
{
    ModelVolume *mv = texture_volume();
    if (!mv)
        return;

    std::array<bool, TEXTURE_DISPLACEMENT_MAX_LAYERS> used{};
    for (const TextureDisplacementLayer &l : mv->texture_displacement_layers)
        if (l.slot >= 0 && size_t(l.slot) < used.size())
            used[size_t(l.slot)] = true;
    int free_slot = -1;
    for (size_t i = 0; i < used.size(); ++i)
        if (!used[i]) { free_slot = int(i); break; }
    if (free_slot < 0) {
        show_error(nullptr, _u8L("Maximum number of texture displacement layers reached."));
        return;
    }

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Add texture displacement layer"), UndoRedo::SnapshotType::GizmoAction);

    TextureDisplacementLayer layer;
    layer.slot = free_slot;
    // Start the layer off on the first library texture rather than on nothing at all: a textureless
    // layer looks broken (painting on it appears to do nothing, because there is no height map to
    // displace by). The user swaps it for another from the layer's own picker.
    const std::vector<TextureLibraryEntry> &library = texture_library();
    if (!library.empty())
        if (const LibraryTexture *tex = get_library_texture(library.front().path)) {
            layer.name       = library.front().name;
            layer.path       = library.front().path;
            layer.image_data = tex->image_data;
        }
    mv->texture_displacement_layers.push_back(std::move(layer));

    set_active_layer(free_slot);
    // set_active_layer() is a no-op when the new slot happens to be the one already active (slot 0,
    // for the very first layer added), so the preview would not pick the new texture up on its own.
    rebuild_preview();
    m_parent.set_as_dirty();
}

const GLGizmoTextureDisplacement::LibraryTexture *GLGizmoTextureDisplacement::get_library_texture(const std::string &path)
{
    if (auto it = m_library_textures.find(path); it != m_library_textures.end())
        return it->second.image_data ? &it->second : nullptr;

    LibraryTexture entry;
    std::string    error;
    entry.image_data = load_texture_image_data(path, error);
    if (entry.image_data) {
        TextureDisplacementLayer probe;
        probe.image_data = entry.image_data;
        entry.thumbnail  = upload_height_thumbnail(decode_height_texture(probe));
        if (!entry.thumbnail)
            entry.image_data.reset(); // decoded to nothing usable - treat it as a failed load
    } else {
        BOOST_LOG_TRIVIAL(error) << "Texture displacement: could not load texture " << path << ": " << error;
    }

    // Cached whether it loaded or not: a file that failed is remembered as unusable, so the picker
    // does not retry (and re-log) it on every frame it is on screen.
    const auto [pos, inserted] = m_library_textures.emplace(path, std::move(entry));
    return pos->second.image_data ? &pos->second : nullptr;
}

void GLGizmoTextureDisplacement::set_layer_texture(TextureDisplacementLayer &layer, const TextureLibraryEntry &entry)
{
    const LibraryTexture *tex = get_library_texture(entry.path);
    if (tex == nullptr) {
        show_error(nullptr, _u8L("Could not load the selected texture."));
        return;
    }

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Change texture displacement texture"), UndoRedo::SnapshotType::GizmoAction);
    layer.name = entry.name;
    layer.path = entry.path;
    // Handing over the library's own buffer (rather than a copy) is what lets decode_height_texture()
    // - whose cache is keyed by exactly this pointer - hit straight away instead of re-decoding
    // the PNG the first time the layer is previewed or baked.
    layer.image_data = tex->image_data;

    rebuild_preview();
    m_parent.set_as_dirty();
}

void GLGizmoTextureDisplacement::import_custom_texture(TextureDisplacementLayer &layer)
{
    const wxString wildcard = "Images (*.png;*.jpg;*.jpeg;*.bmp)|*.png;*.jpg;*.jpeg;*.bmp";
    wxFileDialog   dialog(nullptr, _L("Choose a texture image (height map)"), wxEmptyString, wxEmptyString, wildcard,
                         wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK)
        return;

    // Converts to the 8-bit grayscale PNG the bake code understands and copies it into the user's
    // own texture folder, so it stays available for later models (and survives an app update, which
    // rewrites the shipped folder wholesale).
    std::string                              error;
    const std::optional<TextureLibraryEntry> entry = import_texture_to_library(into_u8(dialog.GetPath()), error);
    if (!entry) {
        show_error(nullptr, error);
        return;
    }
    set_layer_texture(layer, *entry);
}

float GLGizmoTextureDisplacement::texture_row_height() const
{
    return m_imgui->scaled(3.f);
}

bool GLGizmoTextureDisplacement::texture_row(const char *id, const std::string &name, GLTexture *thumbnail, bool selected, float width)
{
    const float  row_h   = texture_row_height();
    const ImVec2 start   = ImGui::GetCursorPos();
    const bool   clicked = ImGui::Selectable(id, selected, 0, ImVec2(width, row_h));
    const ImVec2 after   = ImGui::GetCursorPos();

    // Lay the image and the name back over the Selectable that was just emitted, so the whole row
    // - preview included - is the clickable target rather than just a strip of text next to it.
    ImGui::SetCursorPos(ImVec2(start.x + ImGui::GetStyle().FramePadding.x, start.y));
    if (thumbnail != nullptr) {
        // Fit inside a row_h square without distorting a non-square source image.
        const float  aspect = (thumbnail->get_height() > 0) ? float(thumbnail->get_width()) / float(thumbnail->get_height()) : 1.f;
        const ImVec2 dim    = (aspect >= 1.f) ? ImVec2(row_h, row_h / aspect) : ImVec2(row_h * aspect, row_h);
        ImGui::Image((ImTextureID) (intptr_t) thumbnail->get_id(), dim);
        ImGui::SameLine();
    }
    ImGui::SetCursorPosY(start.y + (row_h - ImGui::GetTextLineHeight()) * 0.5f); // centre the name on the image
    ImGui::TextUnformatted(name.c_str());

    ImGui::SetCursorPos(after);
    return clicked;
}

void GLGizmoTextureDisplacement::render_texture_picker(TextureDisplacementLayer &layer)
{
    const float row_h        = texture_row_height();
    const float import_btn_w = m_imgui->scaled(1.6f);
    const float spacing      = ImGui::GetStyle().ItemSpacing.x;
    const float picker_w     = std::max(m_imgui->scaled(10.f), ImGui::GetContentRegionAvail().x - import_btn_w - spacing);

    const ImVec2 start = ImGui::GetCursorPos();
    if (texture_row("##texture_picker", layer.name.empty() ? _u8L("Choose a texture...") : layer.name,
                    get_layer_thumbnail(layer), false, picker_w))
        ImGui::OpenPopup("##texture_library");
    const ImVec2 after = ImGui::GetCursorPos();

    // Positioned explicitly rather than with SameLine(): texture_row() draws several widgets and
    // then rewinds the cursor, so ImGui's notion of "the previous line" is not the row's own.
    ImGui::SetCursorPos(ImVec2(start.x + picker_w + spacing, start.y));
    if (ImGui::Button("+", ImVec2(import_btn_w, row_h)))
        import_custom_texture(layer);
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_u8L("Import your own image as a height map. It is converted to the format the slicer "
                              "bakes from and saved to your personal texture folder, kept separate from the "
                              "textures shipped with OrcaSlicer."),
                          m_imgui->scaled(20.f));
    ImGui::SetCursorPos(after);

    if (ImGui::BeginPopup("##texture_library")) {
        const std::vector<TextureLibraryEntry> &library = texture_library();
        if (library.empty())
            m_imgui->text(_L("No textures found."));

        bool shipped_heading = false;
        bool user_heading    = false;
        for (const TextureLibraryEntry &entry : library) {
            if (!entry.is_user && !shipped_heading) {
                ImGui::TextDisabled("%s", _u8L("Built-in").c_str());
                shipped_heading = true;
            } else if (entry.is_user && !user_heading) {
                if (shipped_heading)
                    ImGui::Separator();
                ImGui::TextDisabled("%s", _u8L("My textures").c_str());
                user_heading = true;
            }

            ImGui::PushID(entry.path.c_str());
            const LibraryTexture *tex = get_library_texture(entry.path);
            if (texture_row("##entry", entry.name, tex != nullptr ? tex->thumbnail.get() : nullptr,
                            entry.path == layer.path, m_imgui->scaled(14.f))) {
                set_layer_texture(layer, entry);
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndPopup();
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
        mv->texture_displacement_facet(slot).reset();
        m_thumbnails[size_t(slot)].reset();
        m_thumbnail_source[size_t(slot)] = nullptr;
    }

    if (m_active_layer_slot == slot)
        update_from_model_object(false); // also rebuilds the preview
    else
        rebuild_preview(); // a non-active layer's contribution to the combined preview changed

    m_parent.set_as_dirty();
}

int GLGizmoTextureDisplacement::select_visible_faces(const std::array<float, 12> *uv_clip)
{
    ModelVolume *mv = texture_volume();
    ModelObject *mo = m_c->selection_info()->model_object();
    const int    idx = texture_volume_raycaster_index();
    if (mv == nullptr || mo == nullptr || idx < 0 || idx >= int(m_triangle_selectors.size()))
        return 0;

    const indexed_triangle_set &its = mv->mesh().its;
    if (its.indices.empty())
        return 0;

    const Selection             &selection = m_parent.get_selection();
    const Geometry::Transformation trafo(mo->instances[selection.get_instance_idx()]->get_transformation().get_matrix() *
                                         mv->get_matrix());
    const Transform3d            &to_world = trafo.get_matrix();
    const Camera                 &camera   = wxGetApp().plater()->get_camera();

    // Normals transform by the inverse transpose, not by the matrix itself - with a non-uniform
    // scale the two differ, and using the wrong one flips the facing test on the scaled axes.
    const Matrix3d normal_matrix = to_world.matrix().block<3, 3>(0, 0).inverse().transpose();

    // Pass 1 (cheap): drop back-facing triangles. Under perspective the view direction varies across
    // the model, so it is taken per triangle from the eye to the centroid; under an orthographic
    // camera get_position() is still a point on the view axis, so the same expression stays correct
    // in direction terms for everything actually on screen.
    const Vec3d eye = camera.get_position();
    const bool  ortho = camera.get_type() == Camera::EType::Ortho;
    const Vec3d fwd   = camera.get_dir_forward();

    std::vector<Vec3f>    centroids;   // world coords, what get_unobscured_idxs() expects
    std::vector<unsigned> front_facing; // parallel: which facet each centroid came from
    centroids.reserve(its.indices.size() / 2);
    front_facing.reserve(its.indices.size() / 2);

    for (size_t f = 0; f < its.indices.size(); ++f) {
        const stl_triangle_vertex_indices &tri = its.indices[f];
        const Vec3d a = its.vertices[tri[0]].cast<double>();
        const Vec3d b = its.vertices[tri[1]].cast<double>();
        const Vec3d c = its.vertices[tri[2]].cast<double>();
        const Vec3d n_world = normal_matrix * (b - a).cross(c - a);
        if (n_world.squaredNorm() < 1e-20)
            continue; // degenerate triangle: no meaningful normal, so no meaningful facing test
        const Vec3d centroid_local = (a + b + c) / 3.0;
        const Vec3d centroid_world = to_world * centroid_local;
        const Vec3d view_dir       = ortho ? fwd : Vec3d(centroid_world - eye);
        if (n_world.dot(view_dir) >= 0.0)
            continue; // facing away from the camera

        // Clip to the projection frame before the raycast, not after: outside the frame the texture
        // samples to nothing anyway, so those facets would only be painted to no effect - and this
        // is also what keeps the ray queries proportional to the framed area instead of the model.
        if (uv_clip != nullptr) {
            Vec2f uv;
            if (!project_uv_projective(*uv_clip, centroid_local.cast<float>(), uv))
                continue; // behind the projector
            if (uv.x() < 0.f || uv.x() > 1.f || uv.y() < 0.f || uv.y() > 1.f)
                continue;
        }
        centroids.emplace_back(centroid_world.cast<float>());
        front_facing.push_back(unsigned(f));
    }
    if (centroids.empty())
        return 0;

    // Pass 2 (the expensive one): a real ray query per surviving centroid, so geometry in front of a
    // front-facing triangle correctly hides it - the far inner wall of a cup is front-facing but not
    // visible. This is why the whole thing is click-driven rather than live.
    std::vector<unsigned> unobscured;
    {
        wxBusyCursor wait;
        unobscured = m_c->raycaster()->raycasters()[size_t(idx)]->get_unobscured_idxs(
            trafo, camera, centroids, m_c->object_clipper()->get_clipping_plane());
    }
    if (unobscured.empty())
        return 0;

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Select visible faces for texture displacement"),
                                  UndoRedo::SnapshotType::GizmoAction);

    // Replaces the layer's paint rather than adding to it: the checkbox means "project onto what I
    // can see", so a second capture from a new angle should not leave the previous angle painted.
    // TriangleSelectorGUI, not the TriangleSelector base: request_update_render_data() is declared on
    // the GUI subclass, so binding to the base here would drop it.
    TriangleSelectorGUI &selector = *m_triangle_selectors[size_t(idx)];
    selector.reset();
    for (unsigned i : unobscured)
        selector.set_facet(int(front_facing[i]), EnforcerBlockerType::ENFORCER);
    selector.request_update_render_data();

    update_model_object();
    m_parent.set_as_dirty();
    return int(unobscured.size());
}

void GLGizmoTextureDisplacement::select_whole_model()
{
    ModelObject *mo = m_c->selection_info()->model_object();
    if (!mo)
        return;

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Select whole model for texture displacement"), UndoRedo::SnapshotType::GizmoAction);

    int idx = -1;
    for (const ModelVolume *v : mo->volumes) {
        if (!v->is_model_part())
            continue;
        ++idx;
        const size_t facet_count = v->mesh().its.indices.size();
        for (size_t i = 0; i < facet_count; ++i)
            m_triangle_selectors[idx]->set_facet(int(i), EnforcerBlockerType::ENFORCER);
        m_triangle_selectors[idx]->request_update_render_data();
    }
    update_model_object();
    m_parent.set_as_dirty();
}

void GLGizmoTextureDisplacement::subdivide_model()
{
    ModelVolume *mv = texture_volume();
    ModelObject *mo = m_c->selection_info()->model_object();
    if (mv == nullptr || mo == nullptr || m_subdivide_count < 1)
        return; // 0 passes means "no subdivision" - don't take a snapshot for a no-op

    Plater *plater = wxGetApp().plater();
    Plater::TakeSnapshot snapshot(plater, _u8L("Subdivide model for texture displacement"), UndoRedo::SnapshotType::GizmoAction);

    // Same save/replace/restore-painting dance GLGizmoSimplify uses when it re-tessellates a
    // volume's mesh: supported/seam/mmu/fuzzy-skin masks get remapped onto the new triangles,
    // texture-displacement doesn't (no remap support for it yet) and is dropped instead of being
    // left referring to triangle indices that no longer mean the same thing.
    std::optional<TriangleSelector::SavedPainting> saved_painting = mv->save_painting();

    // max_edge_length 0 means "no triangle is ever small enough", so every non-degenerate edge is
    // split on each of the m_subdivide_count passes - i.e. a plain "subdivide the whole mesh N times".
    TriangleMesh new_mesh(subdivide_mesh_uniform(mv->mesh().its, 0.f, m_subdivide_count));
    mv->set_mesh(std::move(new_mesh));
    mv->set_new_unique_id();
    mv->calculate_convex_hull();
    mv->restore_painting(saved_painting);

    if (ObjectList *obj_list = wxGetApp().obj_list()) {
        const ModelObjectPtrs &objs = plater->model().objects;
        auto it = std::find(objs.begin(), objs.end(), mo);
        if (it != objs.end())
            obj_list->update_info_items(size_t(it - objs.begin()));
    }

    plater->changed_object(*mo);
    update_from_model_object(false); // reload selectors/preview against the new mesh + cleared paint
    m_parent.set_as_dirty();
}

bool GLGizmoTextureDisplacement::collect_paint_region(
    const TriangleMesh &mesh, const TextureDisplacementFacetsData &facets,
    std::vector<uint8_t> &region,
    std::array<LayerPaintMap, TEXTURE_DISPLACEMENT_MAX_LAYERS> *paint)
{
    const indexed_triangle_set &its  = mesh.its;
    const size_t                ntri = its.indices.size();
    const size_t                nvert = its.vertices.size();

    region.assign(ntri, 0);
    if (paint)
        for (LayerPaintMap &pm : *paint)
            pm = LayerPaintMap{};

    // Twice the area of each source triangle, for the "is this one covered edge to edge" test below.
    // Only the paint carry-forward needs it, and the live subdivide preview calls this on every
    // slider frame, so it is not built for the region-only path.
    const auto tri_area2 = [](const Vec3f &a, const Vec3f &b, const Vec3f &c) {
        return (b - a).cross(c - a).norm();
    };
    std::vector<float> source_area2;
    if (paint) {
        source_area2.resize(ntri);
        for (size_t i = 0; i < ntri; ++i)
            source_area2[i] = tri_area2(its.vertices[size_t(its.indices[i][0])],
                                        its.vertices[size_t(its.indices[i][1])],
                                        its.vertices[size_t(its.indices[i][2])]);
    }

    bool any_paint = false;
    for (int slot = 0; slot < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++slot) {
        const TriangleSelector::TriangleSplittingData &data = facets[size_t(slot)];
        if (!TriangleSelector::has_facets(data, EnforcerBlockerType::ENFORCER))
            continue;

        // The refine region is exactly the original triangles the brush touched. `triangles_to_split`
        // lists precisely those: serialize() records an entry for every original triangle that is
        // either split (i.e. partially painted, which is the patch boundary) or carries a non-default
        // state (fully painted). No dilation - an earlier version marked every triangle sharing a
        // *vertex* with the patch, which on a coarse model pulls in a whole fan of huge unpainted
        // neighbours and then refines them to the resolution floor, since the height field the detail
        // test samples is not restricted to the painted area. The conformal closure inside
        // subdivide_mesh_adaptive() already grades the size change outward on its own.
        for (const TriangleSelector::TriangleBitStreamMapping &m : data.triangles_to_split)
            if (size_t(m.triangle_idx) < ntri)
                region[m.triangle_idx] |= REFINE_PAINTED;

        if (paint) {
            TriangleSelector sel(mesh);
            sel.deserialize(data, false);
            // get_facets_strict() now reports which source triangle each sub-triangle came from, which
            // is what lets partial coverage be carried forward geometrically instead of being rounded
            // away. Note a *fully* painted source can still come back as several sub-triangles (T-joint
            // splits forced by a refined neighbour), so "wholly painted" is an area test, not a
            // one-piece test.
            std::vector<int>           src;
            const indexed_triangle_set patch = sel.get_facets_strict(EnforcerBlockerType::ENFORCER, &src);

            LayerPaintMap &pm = (*paint)[slot];
            pm.full.assign(ntri, 0);
            pm.part_start.assign(ntri + 1, 0);

            std::vector<float> covered2(ntri, 0.f);
            for (size_t j = 0; j < patch.indices.size() && j < src.size(); ++j) {
                if (size_t(src[j]) >= ntri)
                    continue;
                const stl_triangle_vertex_indices &t = patch.indices[j];
                covered2[size_t(src[j])] += tri_area2(patch.vertices[size_t(t[0])], patch.vertices[size_t(t[1])],
                                                      patch.vertices[size_t(t[2])]);
            }
            for (size_t i = 0; i < ntri; ++i)
                pm.full[i] = (source_area2[i] > 0.f && covered2[i] >= 0.999f * source_area2[i]) ? 1 : 0;

            // Only partly covered sources need their pieces kept - a full one answers every query with
            // "painted", and an untouched one with "not painted".
            for (size_t j = 0; j < patch.indices.size() && j < src.size(); ++j)
                if (size_t(src[j]) < ntri && !pm.full[size_t(src[j])])
                    ++pm.part_start[size_t(src[j]) + 1];
            for (size_t i = 0; i < ntri; ++i)
                pm.part_start[i + 1] += pm.part_start[i];
            pm.part.resize(size_t(pm.part_start[ntri]));
            {
                std::vector<int> fill(pm.part_start.begin(), pm.part_start.begin() + ntri);
                for (size_t j = 0; j < patch.indices.size() && j < src.size(); ++j) {
                    const size_t S = size_t(src[j]);
                    if (S >= ntri || pm.full[S])
                        continue;
                    const stl_triangle_vertex_indices &t = patch.indices[j];
                    pm.part[size_t(fill[S]++)] = { patch.vertices[size_t(t[0])], patch.vertices[size_t(t[1])],
                                                   patch.vertices[size_t(t[2])] };
                }
            }
        }
        any_paint = true;
    }
    if (!any_paint)
        return false;

    // The band straddling the paint's edge. The bake steps the surface from full displacement to zero
    // across it, and nothing else in the refinement criteria can see that step: the chord-error
    // sampler has no per-point paint test, so just outside the paint it keeps reporting the same
    // smooth height field and reports no error at all. Left alone, the transition therefore stays at
    // whatever density the input had - which is what makes the rim of an unpainted island a ring of
    // big, steeply tilted triangles.
    //
    // Seeded from the vertices shared by a painted and an unpainted triangle (the actual edge of the
    // paint) and grown outward over vertex adjacency, so the band covers both sides of the step.
    if (BORDER_BAND_RINGS > 0) {
        // Vertex -> incident triangles, CSR-style (counted, prefix-summed, filled). This runs on every
        // frame of the subdivide preview's sliders, so it must not allocate a small vector per vertex.
        std::vector<int> vstart(nvert + 1, 0);
        for (size_t i = 0; i < ntri; ++i)
            for (int k = 0; k < 3; ++k)
                if (size_t(its.indices[i][k]) < nvert)
                    ++vstart[size_t(its.indices[i][k]) + 1];
        for (size_t v = 0; v < nvert; ++v)
            vstart[v + 1] += vstart[v];
        // static_cast, not size_t(...): the latter parses as a parameter declaration (see the note
        // above the identical prefix sum on `part`).
        std::vector<int> vtri(static_cast<size_t>(vstart[nvert]), 0);
        {
            std::vector<int> fill(vstart.begin(), vstart.begin() + nvert);
            for (size_t i = 0; i < ntri; ++i)
                for (int k = 0; k < 3; ++k)
                    if (size_t(its.indices[i][k]) < nvert)
                        vtri[size_t(fill[size_t(its.indices[i][k])]++)] = int(i);
        }

        // A vertex used by both a painted and an unpainted triangle sits exactly on the paint's edge.
        std::vector<uint8_t> ring_vertex(nvert, 0);
        for (size_t v = 0; v < nvert; ++v) {
            bool painted = false, unpainted = false;
            for (int k = vstart[v]; k < vstart[v + 1]; ++k)
                ((region[size_t(vtri[size_t(k)])] & REFINE_PAINTED) ? painted : unpainted) = true;
            ring_vertex[v] = (painted && unpainted) ? 1 : 0;
        }

        for (int ring = 0; ring < BORDER_BAND_RINGS; ++ring) {
            std::vector<uint8_t> next = ring_vertex;
            for (size_t v = 0; v < nvert; ++v) {
                if (!ring_vertex[v])
                    continue;
                for (int k = vstart[v]; k < vstart[v + 1]; ++k) {
                    const size_t t = size_t(vtri[size_t(k)]);
                    region[t] |= REFINE_BORDER;
                    // Grow through this triangle's other corners, for the following ring.
                    for (int c = 0; c < 3; ++c)
                        if (size_t(its.indices[t][c]) < nvert)
                            next[size_t(its.indices[t][c])] = 1;
                }
            }
            ring_vertex.swap(next);
        }
    }
    return true;
}

TextureDisplacementFacetsData GLGizmoTextureDisplacement::masks_after_subdivision(
    const TriangleMesh &new_mesh, const std::vector<int> &source,
    const std::array<LayerPaintMap, TEXTURE_DISPLACEMENT_MAX_LAYERS> &paint)
{
    // Children inherit their parent's source triangle, and subdivision only ever adds edge midpoints,
    // so every new triangle lies inside its source and on the same surface - which means the source's
    // painted *pieces* can be queried directly by point containment. That is what keeps a brush outline
    // smooth: rounding each source to wholly painted or not instead leaves a ragged fringe of isolated
    // triangles along any curved boundary, and the refined mesh then reproduces that fringe exactly
    // rather than hiding it.
    TextureDisplacementFacetsData       out{};
    const indexed_triangle_set         &its = new_mesh.its;
    for (int slot = 0; slot < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++slot) {
        const LayerPaintMap &pm = paint[size_t(slot)];
        if (pm.empty())
            continue;
        TriangleSelector sel(new_mesh);
        for (size_t i = 0; i < source.size() && i < its.indices.size(); ++i) {
            const size_t S = size_t(source[i]);
            if (S >= pm.full.size())
                continue;
            bool painted = pm.full[S] != 0;
            if (!painted && pm.part_start[S] != pm.part_start[S + 1]) {
                const stl_triangle_vertex_indices &t = its.indices[i];
                const Vec3f centroid = (its.vertices[size_t(t[0])] + its.vertices[size_t(t[1])] +
                                        its.vertices[size_t(t[2])]) / 3.f;
                for (int k = pm.part_start[S]; k < pm.part_start[S + 1] && !painted; ++k)
                    painted = point_in_triangle_coplanar(centroid, pm.part[size_t(k)]);
            }
            if (painted)
                sel.set_facet(int(i), EnforcerBlockerType::ENFORCER);
        }
        out[size_t(slot)] = sel.serialize();
    }
    return out;
}

TextureDisplacementFacetsData GLGizmoTextureDisplacement::facets_data_of(const ModelVolume &mv)
{
    TextureDisplacementFacetsData out{};
    for (int i = 0; i < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++i)
        out[size_t(i)] = mv.texture_displacement_facet(i).get_data();
    return out;
}

bool GLGizmoTextureDisplacement::any_layer_colors(const ModelVolume &mv)
{
    for (const TextureDisplacementLayer &layer : mv.texture_displacement_layers)
        if (layer.color_enabled && !layer.empty() && decode_height_texture(layer).has_color())
            return true;
    return false;
}

TextureColorSettings GLGizmoTextureDisplacement::color_settings_for(const ModelVolume &mv)
{
    TextureColorSettings out;
    if (!any_layer_colors(mv))
        return out; // nothing is colouring: every colour path stays switched off
    out.palette          = cached_palette();
    out.mix_mode         = mv.texture_displacement_options.color_mix_mode;
    out.despeckle_passes = mv.texture_displacement_options.color_despeckle;
    out.layer_height     = print_layer_height();
    // The dither cell is tied to the colour-detail target: a cell much smaller than a facet cannot be
    // drawn at all, and one much larger stops reading as a blend and starts reading as a check.
    out.dither_cell_mm   = std::max(m_subdivide_color_mm, 0.05f) * 2.f;
    return out;
}

const std::vector<GLGizmoTextureDisplacement::PaletteEntry> &GLGizmoTextureDisplacement::cached_palette()
{
    // Rebuilt only when the loaded filaments or the mixing setting actually change. The bump preview
    // rebuilds on every paint stroke and the subdivide preview on every slider frame, and filling the
    // quantizer's lookup cube for a 64-entry palette is tens of milliseconds - paying that per stroke
    // is the difference between painting that keeps up and painting that stutters.
    const ModelVolume *mv     = texture_volume();
    const bool         mixing = mv != nullptr && mv->texture_displacement_options.color_mix_enabled;
    std::vector<ColorRGBA> filaments = filament_palette();
    if (m_palette_cache.empty() || filaments != m_palette_filaments || mixing != m_palette_mixing) {
        m_palette_filaments = std::move(filaments);
        m_palette_mixing    = mixing;
        m_palette_cache     = make_palette(m_palette_filaments, mixing);
        m_palette_quantizer = make_palette_quantizer(m_palette_cache);
    }
    return m_palette_cache;
}

std::vector<ColorRGBA> GLGizmoTextureDisplacement::filament_palette()
{
    std::vector<ColorRGBA> palette = wxGetApp().plater()->get_extruders_colors();
    // mmu_segmentation_facets encodes the filament in a 6-bit prefix code and stops at Extruder16.
    if (palette.size() > size_t(EnforcerBlockerType::ExtruderMax))
        palette.resize(size_t(EnforcerBlockerType::ExtruderMax));
    return palette;
}

float GLGizmoTextureDisplacement::print_layer_height()
{
    try {
        const DynamicPrintConfig &cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
        if (const ConfigOptionFloat *opt = cfg.option<ConfigOptionFloat>("layer_height"); opt != nullptr)
            if (opt->value > 1e-3)
                return float(opt->value);
    } catch (...) {
    }
    return 0.2f;
}

std::vector<GLGizmoTextureDisplacement::PaletteEntry> GLGizmoTextureDisplacement::make_palette(
    const std::vector<ColorRGBA> &filaments, bool mixing)
{
    std::vector<PaletteEntry> out;
    const int                 n = int(filaments.size());
    for (int i = 0; i < n; ++i)
        out.push_back({ Vec3f(filaments[size_t(i)].r(), filaments[size_t(i)].g(), filaments[size_t(i)].b()),
                        i, i, 1, 1 });
    if (!mixing || n < 2)
        return out;

    // How many intermediate steps each pair gets, chosen so the whole palette stays under
    // PALETTE_MAX_ENTRIES. Fewer filaments means more room for mixes, which is also what you want:
    // with two filaments the mixes are the only way to get anywhere, and with sixteen there is little
    // point mixing at all. `den` is also the band/dither repeat, so a small one is a short pattern.
    const int pairs = n * (n - 1) / 2;
    int       steps = 0;
    for (int s = 5; s >= 1; --s)
        if (n + pairs * s <= PALETTE_MAX_ENTRIES) {
            steps = s;
            break;
        }
    if (steps == 0)
        return out;
    const int den = steps + 1;

    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j) {
            float la, aa, ba, lb, ab, bb;
            RGB2Lab(filaments[size_t(i)].r() * 255.f, filaments[size_t(i)].g() * 255.f,
                    filaments[size_t(i)].b() * 255.f, &la, &aa, &ba);
            RGB2Lab(filaments[size_t(j)].r() * 255.f, filaments[size_t(j)].g() * 255.f,
                    filaments[size_t(j)].b() * 255.f, &lb, &ab, &bb);
            for (int k = 1; k <= steps; ++k) {
                // k/den of filament i, the rest of j - averaged in Lab, which is what the eye does
                // when the two are interleaved too finely to resolve.
                const float t = float(k) / float(den);
                float       r, g, b;
                Lab2RGB(la * t + lb * (1.f - t), aa * t + ab * (1.f - t), ba * t + bb * (1.f - t), &r, &g, &b);
                out.push_back({ Vec3f(std::clamp(r / 255.f, 0.f, 1.f), std::clamp(g / 255.f, 0.f, 1.f),
                                      std::clamp(b / 255.f, 0.f, 1.f)),
                                i, j, k, den });
            }
        }
    return out;
}

ColorResolveFn GLGizmoTextureDisplacement::make_mix_resolver(const std::vector<PaletteEntry> &palette,
                                                             ColorMixMode mode, float layer_height,
                                                             float cell_mm)
{
    if (palette.empty())
        return nullptr;
    auto        entries = std::make_shared<std::vector<PaletteEntry>>(palette);
    const float band    = std::max(layer_height, 0.01f);
    const float cell    = std::max(cell_mm, 0.01f);

    return [entries, mode, band, cell](int index, const Vec3f &pos) -> int {
        if (index < 0 || size_t(index) >= entries->size())
            return -1;
        const PaletteEntry &e = (*entries)[size_t(index)];
        if (!e.is_mix())
            return e.a;

        // Which of the two filaments this point falls on. Both patterns are *ordered*, never random:
        // the eye blends a regular pattern into a flat colour, and turns a random one into noise.
        if (mode == ColorMixMode::ZBands) {
            // One band per print layer. floorf, not a cast, so this stays correct below z = 0.
            const int slot = int(std::floor(pos.z() / band));
            const int phase = ((slot % e.den) + e.den) % e.den;
            return phase < e.num ? e.a : e.b;
        }
        // Ordered 4x4 Bayer over the surface, indexed by position so the pattern is stable in space
        // rather than in triangle order (which would move under any remesh, and read as noise).
        static const int BAYER[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
        const int gx = ((int(std::floor(pos.x() / cell)) % 4) + 4) % 4;
        const int gy = ((int(std::floor(pos.y() / cell)) % 4) + 4) % 4;
        // A third axis would be ideal, but the two dominant ones are enough for a surface pattern and
        // keep the cell square on the faces that matter.
        const float threshold = (float(BAYER[gy * 4 + gx]) + 0.5f) / 16.f;
        return (float(e.num) / float(e.den)) > threshold ? e.a : e.b;
    };
}

ColorQuantizeFn GLGizmoTextureDisplacement::make_palette_quantizer(const std::vector<PaletteEntry> &palette)
{
    if (palette.empty())
        return nullptr;

    // Lab once per entry, not once per lookup.
    struct Lab { float l, a, b; };
    std::vector<Lab> palette_lab(palette.size());
    for (size_t i = 0; i < palette.size(); ++i)
        RGB2Lab(palette[i].rgb.x() * 255.f, palette[i].rgb.y() * 255.f, palette[i].rgb.z() * 255.f,
                &palette_lab[i].l, &palette_lab[i].a, &palette_lab[i].b);

    constexpr int E   = PALETTE_LUT_EDGE;
    auto          lut = std::make_shared<std::vector<uint8_t>>(size_t(E) * E * E, 0);
    tbb::parallel_for(tbb::blocked_range<int>(0, E), [&](const tbb::blocked_range<int> &range) {
        for (int r = range.begin(); r < range.end(); ++r)
            for (int g = 0; g < E; ++g)
                for (int b = 0; b < E; ++b) {
                    // Cell centre, so the quantization error is symmetric across the cell.
                    float l0, a0, b0;
                    RGB2Lab((r + 0.5f) / E * 255.f, (g + 0.5f) / E * 255.f, (b + 0.5f) / E * 255.f, &l0, &a0, &b0);
                    int   best  = 0;
                    float best_d = std::numeric_limits<float>::max();
                    for (size_t i = 0; i < palette_lab.size(); ++i) {
                        const float d = DeltaE00(l0, a0, b0, palette_lab[i].l, palette_lab[i].a, palette_lab[i].b);
                        if (d < best_d) {
                            best_d = d;
                            best   = int(i);
                        }
                    }
                    (*lut)[(size_t(r) * E + size_t(g)) * E + size_t(b)] = uint8_t(best);
                }
    });

    return [lut](const Vec3f &rgb) -> int {
        constexpr int E = PALETTE_LUT_EDGE;
        const int r = std::clamp(int(rgb.x() * E), 0, E - 1);
        const int g = std::clamp(int(rgb.y() * E), 0, E - 1);
        const int b = std::clamp(int(rgb.z() * E), 0, E - 1);
        return int((*lut)[(size_t(r) * E + size_t(g)) * E + size_t(b)]);
    };
}

TextureDisplacementPrepareResult GLGizmoTextureDisplacement::prepare_mesh(
    const indexed_triangle_set &base, const TextureDisplacementFacetsData &masks,
    const std::vector<TextureDisplacementLayer> &layers, const TextureDisplacementPrepareParams &params,
    const std::vector<PrintableColor> &palette, const DisplacementProgressFn &progress)
{
    TextureDisplacementPrepareResult out;
    const auto                       report = [&progress](int pct) { return !progress || progress(pct); };

    TriangleMesh                  mesh(base);
    TextureDisplacementFacetsData current = masks;
    bool                          changed = false;
    bool                          had_paint = false;
    for (const TriangleSelector::TriangleSplittingData &m : masks)
        had_paint = had_paint || TriangleSelector::has_facets(m, EnforcerBlockerType::ENFORCER);
    if (!report(1))
        return out;

    // 1. Even out the triangle density, and carry the paint onto the result. Everything downstream is
    //    driven by that paint, so losing it is a hard stop rather than something to bake around - and
    //    stopping here, before anything is committed, leaves the user's model exactly as it was.
    if (params.remesh_edge_mm > 0.f) {
        indexed_triangle_set remeshed;
        if (plan_remesh(mesh.its, params.remesh_edge_mm, params.remesh_sharp_deg, remeshed)) {
            TriangleMesh                   new_mesh(std::move(remeshed));
            const AABBTreeIndirect::Tree3f old_tree =
                AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(mesh.its.vertices, mesh.its.indices);
            TextureDisplacementFacetsData carried{};
            bool                          any = false;
            for (int i = 0; i < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++i) {
                // Both meshes are the volume's own local frame, so there is no shift between them.
                carried[size_t(i)] =
                    remap_texture_paint_spatial(mesh, current[size_t(i)], old_tree, new_mesh, Vec3f::Zero());
                any = any || !carried[size_t(i)].bitstream.empty();
            }
            mesh    = std::move(new_mesh);
            current = std::move(carried);
            changed = true;
            if (had_paint && !any) {
                out.paint_lost = true;
                return out;
            }
        }
    }
    if (!report(50))
        return {};

    // 2. Refine where the texture bends - the painted area only, plus the graded band the conformal
    //    closure pulls in around it.
    if (params.subdiv_target_mm > 0.f) {
        std::vector<uint8_t>                                       region;
        std::array<LayerPaintMap, TEXTURE_DISPLACEMENT_MAX_LAYERS> paint;
        if (collect_paint_region(mesh, current, region, &paint)) {
            // Feature-adaptive: sample the combined displacement so refinement follows texture
            // curvature. A null sampler (only LSCM layers, or nothing decodable) falls back to the
            // length baseline alone.
            HeightFieldSampler sampler;
            if (params.subdiv_feature)
                sampler = make_combined_displacement_sampler(mesh.its, layers, current);
            // Colour boundaries need triangles of their own - the chord test cannot see them, since
            // the height field is perfectly smooth across a change of filament.
            ColorFieldSampler color;
            if (params.subdiv_color_edge_mm > 0.f && !palette.empty())
                color = make_combined_color_sampler(mesh.its, layers, current, make_palette_quantizer(palette));
            // Note the sampler is built on the *quantizer* alone - the refinement follows perceived
            // colour, never the interleaving that realises a mix (see ColorResolveFn).
            // "Min edge" is a feature-mode control (it is the floor the curvature test refines down
            // to); in plain adaptive mode the target edge length is the only criterion, so the floor
            // must not be allowed to silently override a target the user set below it.
            const float tol   = params.subdiv_feature ? params.subdiv_detail_mm : 0.f;
            const float floor = params.subdiv_feature ? params.subdiv_min_edge_mm : 0.f;
            bool        aborted = false;
            std::vector<int> source;
            // The budget is "triangles the refinement may *add*", so the mesh's own count is the
            // baseline - otherwise the control would be meaningless (or a dead end) on a dense model.
            indexed_triangle_set refined =
                subdivide_mesh_adaptive(mesh.its, region, params.subdiv_target_mm,
                                        int(mesh.its.indices.size()) + params.subdiv_added_triangles, &source,
                                        sampler, tol, floor, params.subdiv_border_mm, [&](int pct) {
                                            aborted = !report(50 + pct / 2);
                                            return !aborted;
                                        },
                                        color, params.subdiv_color_edge_mm);
            if (aborted)
                return {};
            if (refined.indices.size() != mesh.its.indices.size()) {
                mesh    = TriangleMesh(std::move(refined));
                current = masks_after_subdivision(mesh, source, paint);
                changed = true;
            }
        }
    }
    if (!report(100) || !changed)
        return out; // an empty result: nothing to commit, which is not a failure

    out.mesh  = std::move(mesh.its);
    out.masks = std::move(current);
    return out;
}

void GLGizmoTextureDisplacement::subdivide_model_adaptive()
{
    ModelVolume *mv = texture_volume();
    if (mv == nullptr || m_subdivide_target_mm <= 0.f)
        return;

    update_model_object(); // flush any in-progress stroke into the committed masks first

    if (!mv->is_texture_displacement_painted()) {
        show_error(nullptr, _u8L("Paint the area you want to subdivide first - adaptive subdivision only "
                                 "refines where you have painted."));
        return;
    }

    TextureDisplacementPrepareParams params;
    params.subdiv_target_mm       = m_subdivide_target_mm;
    params.subdiv_detail_mm       = m_subdivide_detail_mm;
    params.subdiv_min_edge_mm     = m_subdivide_min_edge_mm;
    params.subdiv_border_mm       = m_subdivide_border_mm;
    params.subdiv_color_edge_mm   = m_subdivide_color_mm;
    params.subdiv_feature         = m_subdivide_feature;
    params.subdiv_added_triangles = m_subdivide_budget_k * 1000;
    queue_prepare(params, _u8L("Adaptive subdivide for texture displacement"), /* then_bake */ false,
                  _u8L("Nothing to subdivide - the painted area already meets the target edge length and "
                       "detail tolerance, or the triangle budget is already used up."));
}

void GLGizmoTextureDisplacement::smooth_model()
{
    ModelVolume *mv = texture_volume();
    ModelObject *mo = m_c->selection_info()->model_object();
    if (mv == nullptr || mo == nullptr)
        return;
    const TextureDisplacementOptions &opts = mv->texture_displacement_options;
    if (opts.smooth_strength <= 0.f || opts.smooth_iterations <= 0)
        return;

    update_model_object(); // flush any in-progress stroke, so the painted region below is current

    // Movable = the vertices of the painted triangles, so the first ring of purely unpainted vertices
    // outside them stays put and anchors the result. Restricted rather than whole-model on purpose:
    // this runs on geometry that has already been baked, and relaxing the whole thing would quietly
    // round off every unrelated feature on the part. With "Ignore outer ring" the patch's own rim is
    // held as well - see TextureDisplacementOptions::smooth_skip_border.
    const indexed_triangle_set &its = mv->mesh().its;
    std::vector<uint8_t>        painted_face(its.indices.size(), 0);
    bool                        any = false;
    for (int slot = 0; slot < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++slot)
        for (const TriangleSelector::TriangleBitStreamMapping &m : mv->texture_displacement_facet(slot).get_data().triangles_to_split)
            if (size_t(m.triangle_idx) < its.indices.size()) {
                painted_face[size_t(m.triangle_idx)] = 1;
                any                                  = true;
            }
    if (!any) {
        show_error(nullptr, _u8L("Paint the area you want to smooth first - smoothing only touches the "
                                 "painted part of the model."));
        return;
    }
    std::vector<uint8_t> movable(its.vertices.size(), 0);
    for (size_t i = 0; i < its.indices.size(); ++i)
        if (painted_face[i])
            for (int k = 0; k < 3; ++k)
                movable[size_t(its.indices[i][k])] = 1;
    if (opts.smooth_skip_border)
        for (size_t i = 0; i < its.indices.size(); ++i)
            if (!painted_face[i])
                for (int k = 0; k < 3; ++k)
                    movable[size_t(its.indices[i][k])] = 0; // also used by unpainted geometry -> the rim
    if (std::none_of(movable.begin(), movable.end(), [](uint8_t m) { return m != 0; })) {
        // Every painted vertex is on the patch's rim, so "Ignore outer ring" leaves nothing to move.
        show_error(nullptr, _u8L("Nothing to smooth - the painted area is only one triangle deep, so with "
                                 "\"Ignore outer ring\" on there are no interior vertices to relax."));
        return;
    }

    indexed_triangle_set smoothed = its;
    {
        wxBusyCursor wait;
        smooth_mesh_vertices(smoothed, movable, opts.smooth_strength, opts.smooth_iterations);
    }

    Plater *plater = wxGetApp().plater();
    Plater::TakeSnapshot snapshot(plater, _u8L("Smooth texture displacement"), UndoRedo::SnapshotType::GizmoAction);

    // No save/restore-painting dance here, unlike subdivide and remesh: smoothing only moves vertices,
    // it does not touch the triangle list, so every paint channel still refers to exactly the triangles
    // it did before. set_mesh() clears the extra facets, so this saves and puts back the
    // texture-displacement masks verbatim - no remap needed, and none of them is lost.
    std::optional<TriangleSelector::SavedPainting>                    saved_painting = mv->save_painting();
    std::array<TriangleSelector::TriangleSplittingData, TEXTURE_DISPLACEMENT_MAX_LAYERS> saved_texture;
    for (int i = 0; i < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++i)
        saved_texture[size_t(i)] = mv->texture_displacement_facet(i).get_data();

    mv->set_mesh(TriangleMesh(std::move(smoothed)));
    mv->set_new_unique_id();
    mv->calculate_convex_hull();
    mv->restore_painting(saved_painting);
    for (int i = 0; i < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++i)
        mv->texture_displacement_facet(i).set_data(std::move(saved_texture[size_t(i)]));

    if (ObjectList *obj_list = wxGetApp().obj_list()) {
        const ModelObjectPtrs &objs = plater->model().objects;
        auto it = std::find(objs.begin(), objs.end(), mo);
        if (it != objs.end())
            obj_list->update_info_items(size_t(it - objs.begin()));
    }
    plater->changed_object(*mo);
    update_from_model_object(false);
    m_parent.set_as_dirty();
}

bool GLGizmoTextureDisplacement::plan_remesh(const indexed_triangle_set &src, float target_edge_mm,
                                             float sharp_angle_deg, indexed_triangle_set &out)
{
    if (target_edge_mm <= 0.f)
        return false;

    // Its cost and memory grow with the *square* of 1/target_edge, and it runs single-threaded on the
    // UI thread, so the target has to be bounded against the part's actual surface area rather than
    // taken at face value. Standard mode asks for 1 mm whatever it is handed, and the slider goes down
    // to 0.1 mm: on a 150 mm part those are ~500 k and ~50 M triangles respectively - minutes to hours
    // of frozen UI, which is indistinguishable from a hang. Raising the target instead still gives the
    // subdivider the even density it needs, and the subdivision that follows is where the detail was
    // always going to come from anyway. An equilateral triangle of edge L covers sqrt(3)/4 * L^2.
    static constexpr double REMESH_MAX_TRIANGLES = 150000.0;
    double area = 0.0;
    for (const stl_triangle_vertex_indices &tri : src.indices)
        area += 0.5 * double((src.vertices[size_t(tri[1])] - src.vertices[size_t(tri[0])])
                                 .cross(src.vertices[size_t(tri[2])] - src.vertices[size_t(tri[0])])
                                 .norm());
    if (const double budget_edge = std::sqrt(area / (0.4330127 * REMESH_MAX_TRIANGLES));
        budget_edge > double(target_edge_mm)) {
        BOOST_LOG_TRIVIAL(info) << "Texture displacement: remesh target raised from " << target_edge_mm
                                << " mm to " << budget_edge << " mm to stay within "
                                << int(REMESH_MAX_TRIANGLES) << " triangles.";
        target_edge_mm = float(budget_edge);
    }

    indexed_triangle_set remeshed =
        MeshBoolean::cgal::remesh_isotropic(src, double(target_edge_mm), 3, double(sharp_angle_deg));
    // remesh_isotropic() signals failure by handing the input straight back, so compare against it
    // structurally. Vertex count alone is not enough: a remesh that only redistributes triangles at
    // roughly the current density legitimately lands on the same count, and treating that as failure
    // meant a perfectly good result got thrown away with an error message.
    if (remeshed.indices.empty() ||
        (remeshed.vertices.size() == src.vertices.size() && remeshed.indices.size() == src.indices.size() &&
         remeshed.indices == src.indices))
        return false;

    out = std::move(remeshed);
    return true;
}

void GLGizmoTextureDisplacement::remesh_model()
{
    if (texture_volume() == nullptr || m_remesh_target_edge_mm <= 0.f)
        return;

    update_model_object(); // the paint rides across the remesh, so flush any in-progress stroke first

    TextureDisplacementPrepareParams params;
    params.remesh_edge_mm   = m_remesh_target_edge_mm;
    params.remesh_sharp_deg = m_remesh_keep_sharp_edges ? m_remesh_sharp_angle_deg : 0.f;
    queue_prepare(params, _u8L("Remesh model for texture displacement"), /* then_bake */ false,
                  _u8L("Remeshing did not change the model. It may be non-manifold (open edges or edges "
                       "shared by more than two triangles), or the target edge length may already be met."));
}

void GLGizmoTextureDisplacement::rebuild_subdivide_preview()
{
    m_subdivide_preview_glmodel.reset();
    m_subdivide_preview_tris = -1;
    const ModelVolume *mv = texture_volume();
    if (mv == nullptr)
        return;

    // The same subdivision Apply would commit, kept in a throwaway mesh and shown only as a
    // wireframe - the model itself is not touched until Apply.
    indexed_triangle_set its;
    if (m_subdivide_adaptive) {
        if (m_subdivide_target_mm <= 0.f)
            return;
        const TextureDisplacementFacetsData facets = facets_data_of(*mv);
        std::vector<uint8_t>                region;
        if (!collect_paint_region(mv->mesh(), facets, region, nullptr))
            return; // nothing painted yet: nothing to preview
        HeightFieldSampler sampler;
        if (m_subdivide_feature)
            sampler = make_combined_displacement_sampler(mv->mesh().its, mv->texture_displacement_layers, facets);
        // Feature mode: curvature (Detail tolerance) on top of the Max-edge baseline, down to the
        // Min-edge floor. Plain adaptive: tol 0, so only the target-edge-length criterion applies.
        const float tol   = m_subdivide_feature ? m_subdivide_detail_mm : 0.f;
        const float floor = m_subdivide_feature ? m_subdivide_min_edge_mm : 0.f;
        // Same colour criterion Apply will use, so the previewed wireframe is the mesh that commits.
        ColorFieldSampler color;
        if (m_subdivide_color_mm > 0.f && any_layer_colors(*mv)) {
            cached_palette(); // refreshes m_palette_quantizer if the filaments changed
            color = make_combined_color_sampler(mv->mesh().its, mv->texture_displacement_layers, facets,
                                                m_palette_quantizer);
        }
        its = subdivide_mesh_adaptive(mv->mesh().its, region, m_subdivide_target_mm,
                                      int(mv->mesh().its.indices.size()) + m_subdivide_budget_k * 1000,
                                      nullptr, sampler, tol, floor, m_subdivide_border_mm, nullptr, color,
                                      m_subdivide_color_mm);
    } else {
        if (m_subdivide_count < 1)
            return;
        its = subdivide_mesh_uniform(mv->mesh().its, 0.f, m_subdivide_count);
    }
    if (its.indices.empty())
        return;
    m_subdivide_preview_tris = int(its.indices.size());

    GLModel::Geometry init_data;
    init_data.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
    init_data.reserve_vertices(its.vertices.size());
    init_data.reserve_indices(its.indices.size() * 6);
    for (const Vec3f &v : its.vertices)
        init_data.add_vertex(v);
    for (const stl_triangle_vertex_indices &tri : its.indices)
        for (int i = 0; i < 3; ++i)
            init_data.add_line(unsigned(tri[i]), unsigned(tri[(i + 1) % 3]));
    if (!init_data.is_empty())
        m_subdivide_preview_glmodel.init_from(std::move(init_data));
}

void GLGizmoTextureDisplacement::render_subdivide_preview()
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    const ModelVolume *mv = texture_volume();
    if (mo == nullptr || mv == nullptr || !m_subdivide_preview_glmodel.is_initialized())
        return;
    GLShaderProgram *shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    const Selection  &selection    = m_parent.get_selection();
    const Transform3d trafo_matrix = mo->instances[selection.get_instance_idx()]->get_transformation().get_matrix() * mv->get_matrix();
    const Camera     &camera       = wxGetApp().plater()->get_camera();

    shader->start_using();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix() * trafo_matrix);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    glsafe(::glEnable(GL_POLYGON_OFFSET_LINE));
    glsafe(::glPolygonOffset(-1.0f, -1.0f));
    m_subdivide_preview_glmodel.set_color(ColorRGBA(0.2f, 0.9f, 1.0f, 0.7f)); // cyan, reads as "preview"
    m_subdivide_preview_glmodel.render();
    glsafe(::glDisable(GL_POLYGON_OFFSET_LINE));
    shader->stop_using();
}

GLTexture *GLGizmoTextureDisplacement::get_layer_thumbnail(const TextureDisplacementLayer &layer)
{
    if (layer.empty() || layer.slot < 0 || size_t(layer.slot) >= TEXTURE_DISPLACEMENT_MAX_LAYERS)
        return nullptr;

    const size_t slot = size_t(layer.slot);
    if (m_thumbnails[slot] && m_thumbnail_source[slot] == layer.image_data.get() &&
        m_thumbnail_smoothing[slot] == layer.smoothing)
        return m_thumbnails[slot].get();

    // Reuses the already-decoded, already-cached grayscale pixels (see decode_height_texture()'s
    // own cache in TextureDisplacement.cpp) - only the gray-to-RGBA expansion and GPU upload below are
    // new work. Rebuilt when the texture *or the smoothing* changes, so the fast/bump preview - which
    // samples this GPU texture directly - reflects the current smoothing rather than the raw image.
    std::unique_ptr<GLTexture> texture = upload_height_thumbnail(decode_height_texture(layer));
    if (!texture)
        return nullptr;

    m_thumbnails[slot]          = std::move(texture);
    m_thumbnail_source[slot]    = layer.image_data.get();
    m_thumbnail_smoothing[slot] = layer.smoothing;
    return m_thumbnails[slot].get();
}

GLTexture *GLGizmoTextureDisplacement::get_layer_color_texture(const TextureDisplacementLayer &layer)
{
    if (layer.empty() || !layer.color_enabled)
        return nullptr;
    if (m_color_tex && m_color_tex_source == layer.image_data.get() && m_color_tex_smoothing == layer.smoothing)
        return m_color_tex.get();

    std::unique_ptr<GLTexture> texture = upload_color_texture(decode_height_texture(layer), HEIGHT_TEX_MAX_PX);
    if (!texture) {
        m_color_tex.reset();
        m_color_tex_source = nullptr;
        return nullptr;
    }
    m_color_tex           = std::move(texture);
    m_color_tex_source    = layer.image_data.get();
    m_color_tex_smoothing = layer.smoothing;
    return m_color_tex.get();
}

GLTexture *GLGizmoTextureDisplacement::get_layer_height_texture(const TextureDisplacementLayer &layer)
{
    if (layer.empty())
        return nullptr;

    // One slot, not one per layer: the bump shader only ever shades the *active* layer, so a single
    // full-resolution upload is enough and the VRAM cost stays at one texture rather than eight.
    if (m_height_tex && m_height_tex_source == layer.image_data.get() && m_height_tex_smoothing == layer.smoothing)
        return m_height_tex.get();

    std::unique_ptr<GLTexture> texture = upload_height_thumbnail(decode_height_texture(layer), HEIGHT_TEX_MAX_PX);
    if (!texture)
        return nullptr;

    m_height_tex           = std::move(texture);
    m_height_tex_source    = layer.image_data.get();
    m_height_tex_smoothing = layer.smoothing;
    return m_height_tex.get();
}

void GLGizmoTextureDisplacement::bake(bool own_snapshot)
{
    ModelVolume *mv = texture_volume();
    if (!mv || m_bake_in_progress)
        return;

    // Make sure the currently active layer's in-progress edits are flushed into the model before
    // baking, otherwise the most recent, not-yet-committed strokes would be silently skipped.
    update_model_object();

    if (!mv->is_texture_displacement_painted()) {
        show_error(nullptr, _u8L("Nothing is painted, there is nothing to bake."));
        return;
    }

    m_bake_in_progress = true;
    queue_texture_displacement_bake(*mv, color_settings_for(*mv), [this]() {
        m_bake_in_progress = false;
        // Baking replaces the volume's mesh (new id, new topology) without changing the object's
        // id or volume count, so GLGizmoPainterBase::data_changed()'s usual change-detection never
        // notices it needs to reload - do it explicitly here, otherwise the gizmo keeps painting
        // and rendering against the stale, pre-bake TriangleSelectorPatch until the object is
        // deselected and reselected.
        if (m_state == On && m_c->selection_info() && m_c->selection_info()->model_object())
            update_from_model_object(false);
        m_parent.set_as_dirty();
    }, own_snapshot);
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
static constexpr float STD_SUBDIV_MIN_EDGE_MM = 0.02f;
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
    if (!mv->is_texture_displacement_painted()) {
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
    queue_prepare(params, _u8L("Bake texture displacement"), /* then_bake */ true, {});
}

void GLGizmoTextureDisplacement::queue_prepare(const TextureDisplacementPrepareParams &params,
                                               const std::string &snapshot_name, bool then_bake,
                                               const std::string &unchanged_msg)
{
    ModelVolume *mv = texture_volume();
    if (mv == nullptr || m_prepare_in_progress || m_bake_in_progress)
        return;

    TextureDisplacementPrepareInput input;
    input.volume_id     = mv->id();
    input.base_mesh     = mv->mesh().its;
    input.masks         = facets_data_of(*mv);
    input.layers        = mv->texture_displacement_layers;
    input.params        = params;
    input.snapshot_name = snapshot_name;
    if (params.subdiv_color_edge_mm > 0.f)
        input.color = color_settings_for(*mv);

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

    // Shift erases (see handle_snapshot_action_name()); a plain stroke adds.
    const bool  removing = io.KeyShift;
    const ImU32 color    = removing ? IM_COL32(235, 70, 60, 255) : IM_COL32(90, 210, 110, 255);
    const char *glyph    = removing ? "-" : "+";

    ImDrawList  *dl = ImGui::GetForegroundDrawList();
    const float  fs = ImGui::GetFontSize() * 1.5f;
    const ImVec2 at(io.MousePos.x + 15.f, io.MousePos.y - fs - 6.f);
    // A translucent dark disc behind the glyph so it reads on any material colour.
    dl->AddCircleFilled(ImVec2(at.x + fs * 0.28f, at.y + fs * 0.5f), fs * 0.62f, IM_COL32(0, 0, 0, 150));
    dl->AddText(ImGui::GetFont(), fs, at, color, glyph);
}

void GLGizmoTextureDisplacement::on_render_input_window(float x, float y, float bottom_limit)
{
    ModelObject *mo = m_c->selection_info()->model_object();
    if (!mo)
        return;
    ModelVolume *mv = texture_volume();

    const float approx_height = m_imgui->scaled(24.f);
    y = std::min(y, bottom_limit - approx_height);

    // Docked (the default) the panel is pinned next to the gizmo toolbar and cannot be moved, like
    // every other gizmo's. Undocked it becomes an ordinary floating window: a title bar to drag it
    // by, and no forced position - this panel is tall enough (layer stack, per-layer controls) that
    // it can cover the very part of the model being painted, and being able to shove it aside is the
    // point. The position is deliberately *not* seeded on undock, so the window stays exactly where
    // it already was and the user just gains the ability to move it from there.
    ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse;
    if (!m_undocked) {
        flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar;
        GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 1.0f, 0.0f);
    }

    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    GizmoImguiBegin(get_name(), flags);

    // Combo drop-downs otherwise inherit ImGui's near-black default popup background; under the light
    // theme that leaves the dark item text unreadable ("the dropbox is black"). Pushed only around each
    // Combo below (never around a tooltip, whose own near-black default is what makes it readable).
    const ImVec4 combo_popup_bg = wxGetApp().dark_mode() ? ImVec4(0.18f, 0.18f, 0.19f, 1.f)
                                                         : ImVec4(0.93f, 0.93f, 0.93f, 1.f);
    const auto scoped_combo = [&](const char *id, int *v, const char *const items[], int n) {
        ImGui::PushStyleColor(ImGuiCol_PopupBg, combo_popup_bg);
        const bool changed = ImGui::Combo(id, v, items, n);
        ImGui::PopStyleColor();
        return changed;
    };

    // Icon toggle button shared by the selection-mode and view-mode rows, styled like the main toolbar:
    // an inactive button shows the icon in the theme's normal (grey) monochrome, an active one shows it
    // in its original colours. All icons are the same square size. Falls back to a text checkbox if the
    // icon set could not be loaded, so the control is never lost.
    ensure_panel_icons();
    // Sized to match the 3D toolbar's icons exactly, rather than to the panel's font. It is the same
    // expression GLCanvas3D::_update_toolbar_icons_scale() uses, and it is valid here because ImGui's
    // DisplaySize is set from the canvas's pixel size (GLCanvas3D::_resize()) - so one ImGui unit is
    // one canvas pixel, the very units the toolbar is drawn in. Deriving it rather than hard-coding a
    // font multiple also keeps the two in step when the toolbar auto-fit shrinks its icons to make
    // them fit a narrow window, which it does by lowering the same toolbar_icon_scale() read here.
    const float icon_btn_sz = GLToolbar::Default_Icons_Size * wxGetApp().toolbar_icon_scale() * m_parent.get_scale();
    // The icon SVGs carry their own border, so the ImGui button's own frame border and idle fill are
    // suppressed here (FrameBorderSize 0 + transparent ImGuiCol_Button) to avoid a doubled border - the
    // hover/active fill is left in place for feedback. Applied only around these gizmo icon buttons.
    const auto push_borderless_icon_style = []() {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.f, 0.f, 0.f));
    };
    const auto pop_borderless_icon_style = []() {
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    };
    const auto  icon_toggle = [&](int uid, const std::string &iconfile, bool active, const wxString &label,
                                 const wxString &tip) -> bool {
        bool       clicked = false;
        const auto it      = m_panel_icon_map.find(iconfile);
        ImGui::PushID(uid);
        // color_wite_gray variants: [0] normal/grey, [1] original colour, [2] disabled.
        if (it != m_panel_icon_map.end() && it->second.size() >= 2 && it->second[active ? 1 : 0]->is_valid()) {
            const IconManager::Icon &ic = *it->second[active ? 1 : 0];
            push_borderless_icon_style();
            clicked = m_imgui->image_button((ImTextureID) (intptr_t) ic.tex_id, ImVec2(icon_btn_sz, icon_btn_sz),
                                            ic.tl, ic.br, -1, ImVec4(0, 0, 0, 0), ImVec4(1, 1, 1, 1));
            pop_borderless_icon_style();
        } else {
            bool v  = active;
            clicked = ImGui::Checkbox(label.ToUTF8().data(), &v);
        }
        ImGui::PopID();
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(tip, m_imgui->scaled(18.f));
        return clicked;
    };
    // Borderless icon button (non-toggle): always the grey monochrome variant. Falls back to a plain
    // text button when the icon file is absent, so the control is never lost before the art lands.
    const auto icon_button = [&](int uid, const std::string &iconfile, float sz, const wxString &label,
                                 const wxString &tip) -> bool {
        bool       clicked = false;
        const auto it      = m_panel_icon_map.find(iconfile);
        ImGui::PushID(uid);
        if (it != m_panel_icon_map.end() && !it->second.empty() && it->second[0]->is_valid()) {
            const IconManager::Icon &ic = *it->second[0];
            push_borderless_icon_style();
            clicked = m_imgui->image_button((ImTextureID) (intptr_t) ic.tex_id, ImVec2(sz, sz), ic.tl, ic.br, -1,
                                            ImVec4(0, 0, 0, 0), ImVec4(1, 1, 1, 1));
            pop_borderless_icon_style();
        } else {
            clicked = m_imgui->button(label);
        }
        ImGui::PopID();
        if (!tip.empty() && ImGui::IsItemHovered())
            m_imgui->tooltip(tip, m_imgui->scaled(18.f));
        return clicked;
    };

    if (m_imgui->button(m_undocked ? _L("Dock panel") : _L("Undock panel")))
        m_undocked = !m_undocked;
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_u8L("Detach this panel so it can be dragged anywhere over the 3D view, or dock it "
                              "back beside the toolbar."),
                          m_imgui->scaled(20.f));

    // Standard / Pro, right-aligned on the header row. A two-position slider rather than a checkbox
    // because it is a mode, not an option: Standard hides every mesh-preparation control and folds the
    // whole recipe into Bake, Pro shows all of it and hands the ordering to the user.
    {
        const float mode_w = m_imgui->scaled(6.2f);
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - mode_w));
        ImGui::PushItemWidth(mode_w);
        // The format string carries no conversion, so ImGui prints it verbatim as the slider's label -
        // which is how a two-position slider gets word labels instead of "0" and "1".
        const std::string mode_label = pro_mode() ? _u8L("Pro") : _u8L("Standard");
        if (ImGui::SliderInt("##panel_mode", &m_panel_mode, 0, 1, mode_label.c_str())) {
            m_panel_mode = std::clamp(m_panel_mode, 0, 1);
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
        ImGui::PopItemWidth();
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_u8L("Standard - paint, then press Bake: the mesh is remeshed, refined where the "
                                  "texture bends, and displaced in one step.\n\nPro - every mesh-preparation "
                                  "control is shown and you run Remesh, Subdivide and Bake yourself, in the "
                                  "order you want."),
                              m_imgui->scaled(20.f));
    }
    // Pinned every frame while Standard is active, so what Preview shows is always what Bake will do.
    if (!pro_mode() && apply_standard_mode_presets(mv))
        m_preview_params_dirty = true;

    ImGui::Separator();

    // Selection mode: which of TriangleSelector's existing click/brush mechanisms drives painting.
    // "Face" and "Connected area" reuse the exact same underlying selection machinery every other
    // paint gizmo already has (single-facet click, and angle-limited flood fill respectively) -
    // just exposed here as an alternative to brushing, one triangle/region at a time.
    m_imgui->text(_L("Selection mode"));
    const bool is_brush_mode = m_tool_type == ToolType::BRUSH && m_cursor_type != TriangleSelector::CursorType::POINTER;
    const bool is_face_mode  = m_tool_type == ToolType::BRUSH && m_cursor_type == TriangleSelector::CursorType::POINTER;
    const bool is_area_mode  = m_tool_type == ToolType::SMART_FILL;
    ImGui::SameLine();
    if (icon_toggle(801, "toolbar_big_brush.svg", is_brush_mode, _L("Brush"),
                    _L("Brush - paint over the surface by dragging"))) {
        m_tool_type   = ToolType::BRUSH;
        m_cursor_type = TriangleSelector::CursorType::CIRCLE;
    }
    ImGui::SameLine();
    if (icon_toggle(802, "toolbar_face.svg", is_face_mode, _L("Face"),
                    _L("Face - click individual triangles"))) {
        m_tool_type   = ToolType::BRUSH;
        m_cursor_type = TriangleSelector::CursorType::POINTER;
    }
    ImGui::SameLine();
    if (icon_toggle(803, "texture_displacement_connected_area.svg", is_area_mode, _L("Connected area"),
                    _L("Connected area - flood-fill the region reachable without crossing an edge sharper than the "
                       "angle threshold"))) {
        m_tool_type   = ToolType::SMART_FILL;
        m_cursor_type = TriangleSelector::CursorType::POINTER;
    }

    if (is_brush_mode) {
        m_imgui->text(m_desc.at("cursor_size"));
        ImGui::SameLine();
        ImGui::PushItemWidth(m_imgui->scaled(8.4f));
        m_imgui->slider_float("##cursor_radius", &m_cursor_radius, CursorRadiusMin, CursorRadiusMax, "%.2f");
        ImGui::PopItemWidth();

        bool is_circle = m_cursor_type == TriangleSelector::CursorType::CIRCLE;
        if (ImGui::RadioButton(m_desc.at("circle").ToUTF8().data(), is_circle))
            m_cursor_type = TriangleSelector::CursorType::CIRCLE;
        ImGui::SameLine();
        if (ImGui::RadioButton(m_desc.at("sphere").ToUTF8().data(), !is_circle))
            m_cursor_type = TriangleSelector::CursorType::SPHERE;
    } else if (is_area_mode) {
        ImGui::PushItemWidth(m_imgui->scaled(8.4f));
        m_imgui->slider_float(_u8L("Angle threshold"), &m_smart_fill_angle, SmartFillAngleMin, SmartFillAngleMax, "%.0f");
        ImGui::PopItemWidth();
    }

    if (m_imgui->button(_u8L("Select whole model")))
        select_whole_model();

    // View mode (#: "make View Mode with just icons"): Normal / Fast / Checker / Distortion behave as
    // one radio group, Wireframe as an independent toggle. Each has its own icon; the tooltip carries
    // the meaning. The underlying state stays m_use_bump_preview + m_uv_check_mode.
    {
        const int  cur_mode = m_use_bump_preview ? 1 :
                              (m_uv_check_mode == UVCheckMode::Checker    ? 2 :
                               m_uv_check_mode == UVCheckMode::Distortion ? 3 : 0);
        int  new_mode  = cur_mode;
        bool wf_toggle = false;

        m_imgui->text(_L("View"));
        ImGui::SameLine();
        if (icon_toggle(701, "texture_displacement_real_preview.svg", cur_mode == 0, _L("Normal"),
                        _L("Normal - the true displaced geometry (what Bake produces)"))) new_mode = 0;
        ImGui::SameLine();
        if (icon_toggle(702, "texture_displacement_fast_preview.svg", cur_mode == 1, _L("Fast"),
                        _L("Fast - a bump-shaded approximation of the active layer only; quick to update, not exact"))) new_mode = 1;
        ImGui::SameLine();
        if (icon_toggle(703, "texture_displacement_checker.svg", cur_mode == 2, _L("Checker"),
                        _L("Checker - a test grid over the unwrap; squares stay square where it does not stretch"))) new_mode = 2;
        ImGui::SameLine();
        if (icon_toggle(704, "texture_displacement_distortion.svg", cur_mode == 3, _L("Distortion"),
                        _L("Distortion - blue-to-red stretch heatmap over the unwrap (needs the Unwrap/LSCM projection)"))) new_mode = 3;
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(m_imgui->scaled(0.6f), 0.f));
        ImGui::SameLine();
        if (icon_toggle(705, "texture_displacement_wireframe.svg", m_wireframe_overlay, _L("Wireframe"),
                        _L("Wireframe - overlay the mesh edges; independent of the view above"))) wf_toggle = true;

        if (new_mode != cur_mode) {
            m_use_bump_preview = (new_mode == 1);
            m_uv_check_mode    = (new_mode == 2) ? UVCheckMode::Checker :
                                 (new_mode == 3) ? UVCheckMode::Distortion : UVCheckMode::None;
            rebuild_uvcheck_mesh();
            if (m_use_bump_preview)
                rebuild_bump_preview_mesh();
            else
                // The Fast view skips the CPU displacement entirely (see rebuild_preview()), so
                // leaving it means m_preview_glmodel may be stale or absent - ask for it now.
                queue_preview_job();
            // ...and the paint tint between the base and the displaced surface, for the same reason.
            m_paint_overlay_dirty = true;
            refresh_wireframe(); // Normal<->Fast swaps the wireframe between displaced and base mesh
            update_uv_editor(); // mirror the checker / distortion heatmap into the UV pane too (#7)
            m_parent.set_as_dirty();
        }
        if (wf_toggle) {
            m_wireframe_overlay = !m_wireframe_overlay;
            refresh_wireframe();
            m_parent.set_as_dirty();
        }
    }

    if (ImGui::Checkbox(_u8L("Auto update").c_str(), &m_auto_update))
        if (m_auto_update)
            rebuild_preview(); // catch up anything that changed while it was off
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_u8L("Rebuild the displaced geometry as soon as anything changes (painting, textures, "
                              "sliders). Turn off to only rebuild when you release a slider, on very heavy models."),
                          m_imgui->scaled(20.f));

    ImGui::Separator();
    m_imgui->text(_L("Texture layers"));
    if (mv != nullptr) {
        // Add-layer affordance as an icon beside the heading. It is deliberately *not* right-aligned
        // against the window edge: this panel uses ImGuiWindowFlags_AlwaysAutoResize, and positioning
        // an item at GetWindowContentRegionMax().x - w feeds the window's own width back into its
        // auto-fit, growing it by one item-spacing every frame - which, with the panel docked and
        // anchored by its right edge, walked it left off-screen on hover. A plain SameLine can't do that.
        const unsigned int add_icon = tool_icon_id();
        const float        sz       = m_imgui->scaled(1.3f);
        ImGui::SameLine();
        bool add_clicked;
        if (add_icon != 0) {
            // The add icon SVG carries its own border; suppress the ImGui frame border/idle fill.
            push_borderless_icon_style();
            add_clicked = m_imgui->image_button((ImTextureID) (intptr_t) add_icon, ImVec2(sz, sz));
            pop_borderless_icon_style();
        } else {
            add_clicked = m_imgui->button(m_desc.at("add_texture"));
        }
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_u8L("Add a texture layer"), m_imgui->scaled(20.f));
        if (add_clicked)
            add_texture_layer();
    }

    if (mv != nullptr) {
        std::vector<TextureDisplacementLayer *> ordered;
        for (TextureDisplacementLayer &l : mv->texture_displacement_layers)
            ordered.push_back(&l);
        std::sort(ordered.begin(), ordered.end(), [](const auto *a, const auto *b) { return a->slot < b->slot; });

        // Removing a layer erases it from mv->texture_displacement_layers, which shifts every later
        // element down and leaves `ordered` - and `layer` itself - pointing at the wrong element
        // (or past the end). Removing mid-loop would therefore keep rendering this row's remaining
        // widgets against freed/shifted memory. Defer it to after the loop instead.
        int slot_to_remove = -1;

        // The layer stack lives in its own scrolling, tinted region (#10, #12): with eight layers'
        // worth of controls the panel otherwise runs off the bottom of the screen, and there was
        // nothing to tell "settings that belong to this layer" apart from "settings that belong to
        // the tool".
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(1.f, 1.f, 1.f, 0.04f));
        ImGui::BeginChild("##texture_layers", ImVec2(0.f, m_imgui->scaled(20.f)), true);

        for (size_t li = 0; li < ordered.size(); ++li) {
            TextureDisplacementLayer *layer = ordered[li];
            ImGui::PushID(layer->slot);
            const bool is_active = layer->slot == m_active_layer_slot;

            // Tint the whole active layer's block, not just its header (#: "background color for the
            // selected layer ... whole plate"). The block's height isn't known until it is laid out, so
            // draw the block into a foreground channel and the backing rectangle into a background one,
            // then merge - the standard ImGui "rect behind a group" trick.
            ImDrawList  *dl        = ImGui::GetWindowDrawList();
            const ImVec2 block_min = ImGui::GetCursorScreenPos();
            const float  block_rx  = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
            if (is_active) {
                dl->ChannelsSplit(2);
                dl->ChannelsSetCurrent(1);
            }

            // Clicking the header - or anywhere in the layer's block, see the group below - makes
            // it the active layer (#11, #12). A radio button was doing this before, which worked but
            // gave no sense of which block of controls belonged to which layer.
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.f, 0.68f, 0.58f, 0.55f));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.f, 0.68f, 0.58f, 0.35f));
            const std::string header = layer->name.empty() ? Slic3r::format(_u8L("Layer %1%"), li + 1) : layer->name;
            if (ImGui::Selectable(header.c_str(), is_active, 0, ImVec2(ImGui::GetContentRegionAvail().x - m_imgui->scaled(3.f), 0.f)))
                set_active_layer(layer->slot);
            ImGui::PopStyleColor(2);
            ImGui::SameLine();
            if (icon_button(600 + layer->slot, "texture_displacement_cross.svg", m_imgui->scaled(1.3f),
                            m_desc.at("remove_layer"), _u8L("Remove this layer")))
                slot_to_remove = layer->slot;

            // Everything below is this layer's own; the group lets a click anywhere inside it select
            // the layer, which is what makes the block feel like one object rather than loose widgets.
            ImGui::BeginGroup();
            render_texture_picker(*layer);

            ImGui::PushItemWidth(m_imgui->scaled(8.4f));
            // Depth and tile size are logarithmic: see ImGuiLogSlider.
            m_preview_params_dirty |= m_imgui->slider_float(_u8L("Depth (mm)"), &layer->depth_mm, 0.01f, 10.f, "%.3f", ImGuiLogSlider);
            m_preview_params_dirty |= m_imgui->slider_float(_u8L("Tile size (mm)"), &layer->tiling_scale, 0.2f, 200.f, "%.2f", ImGuiLogSlider);
            m_preview_params_dirty |= m_imgui->slider_float(_u8L("Rotation"), &layer->rotation_deg, 0.f, 360.f, "%.0f");
            // Midlevel (#19): the height that means "don't move". At 0 the surface only ever bulges
            // outwards; at 0.5 mid-grey is neutral and darker texels cut inwards.
            m_preview_params_dirty |= m_imgui->slider_float(_u8L("Midlevel"), &layer->midlevel, 0.f, 10.f, "%.2f");
            ImGui::PopItemWidth();
            if (ImGui::IsItemHovered())
                m_imgui->tooltip(_u8L("The grey level that stays put. At 0 the texture can only push the surface "
                                      "outwards. Raise it and anything darker cuts inwards instead, so one height "
                                      "map both embosses and engraves -0.5 makes mid-grey neutral.\n\n"
                                      "Cutting inwards can fold the surface through itself where normals converge: "
                                      "inside a sharp concave corner, or through a thin wall. Keep Depth small "
                                      "relative to the feature you are cutting into."),
                                  m_imgui->scaled(20.f));
            if (layer->midlevel > 0.f && layer->depth_mm > 1.f)
                m_imgui->warning_text(_L("Deep inward displacement may self-intersect."));

            ImGui::PushItemWidth(m_imgui->scaled(8.4f));
            m_preview_params_dirty |= m_imgui->slider_float(_u8L("Smoothing"), &layer->smoothing, 0.f, 1.f, "%.2f");
            ImGui::PopItemWidth();
            if (ImGui::IsItemHovered())
                m_imgui->tooltip(_u8L("Blurs the height texture before it displaces the surface, rounding hard edges "
                                      "and removing speckle without needing a softer source image. Affects the preview "
                                      "and the bake alike."),
                                  m_imgui->scaled(20.f));

            // Edge smoothing: fade the displacement to flat toward the boundary of the painted area.
            m_preview_params_dirty |= ImGui::Checkbox(_u8L("Edge smoothing").c_str(), &layer->edge_smoothing);
            if (ImGui::IsItemHovered())
                m_imgui->tooltip(_u8L("Fade the relief to flat toward the edge of the painted area, so it blends into "
                                      "the surrounding surface. A small amount only softens a thin band at the very "
                                      "edge; the maximum flattens the whole painted face."),
                                  m_imgui->scaled(20.f));
            if (layer->edge_smoothing) {
                ImGui::PushItemWidth(m_imgui->scaled(8.4f));
                m_preview_params_dirty |= m_imgui->slider_float(_u8L("Edge amount"), &layer->edge_smoothing_amount,
                                                                 0.02f, 1.f, "%.2f");
                ImGui::PopItemWidth();
            }

            m_preview_params_dirty |= ImGui::Checkbox(_u8L("Invert").c_str(), &layer->invert);

            // Colour. Only offered for a texture that actually has some - the shipped library is
            // grayscale, and a checkbox that silently does nothing on nine textures out of ten is
            // worse than no checkbox. Disabled rather than hidden so it is clear the feature exists
            // and what it wants.
            {
                const bool has_color = decode_height_texture(*layer).has_color();
                m_imgui->disabled_begin(!has_color);
                bool color_enabled = layer->color_enabled && has_color;
                if (ImGui::Checkbox(_u8L("Use image colours").c_str(), &color_enabled)) {
                    layer->color_enabled   = color_enabled;
                    m_preview_params_dirty = true;
                }
                m_imgui->disabled_end();
                if (ImGui::IsItemHovered())
                    m_imgui->tooltip(has_color ?
                                         _u8L("Colours the painted area from the texture's own colours, as well as "
                                              "displacing it. Each colour is matched to the closest printable "
                                              "colour, and the result is written as multi-material paint - so the "
                                              "area prints in those filaments. Everything you did not paint keeps "
                                              "the object's own filament.") :
                                         _u8L("This texture is a grayscale height map, so it has no colours to "
                                              "apply. Import a colour image to use this."),
                                      m_imgui->scaled(20.f));

                // The rest of colour belongs to the whole stack, not to this layer, so it only appears
                // once - under whichever layer turned colour on.
                if (color_enabled && mv != nullptr) {
                    TextureDisplacementOptions &opts = mv->texture_displacement_options;

                    if (ImGui::Checkbox(_u8L("Mix filaments").c_str(), &opts.color_mix_enabled))
                        m_preview_params_dirty = true;
                    if (ImGui::IsItemHovered())
                        m_imgui->tooltip(_u8L("Interleave pairs of filaments to reach colours between them, so a few "
                                              "filaments cover far more than a few colours. Off means every triangle "
                                              "prints in one of the filaments exactly."),
                                          m_imgui->scaled(20.f));

                    if (opts.color_mix_enabled) {
                        m_imgui->text(_u8L("Mix by"));
                        ImGui::SameLine();
                        const std::string  mix_z     = _u8L("Layers");
                        const std::string  mix_xy    = _u8L("Surface");
                        const char        *mix_items[] = { mix_z.c_str(), mix_xy.c_str() };
                        int                mix_mode  = int(opts.color_mix_mode);
                        ImGui::PushItemWidth(m_imgui->scaled(8.4f));
                        if (scoped_combo("##color_mix_mode", &mix_mode, mix_items, IM_ARRAYSIZE(mix_items))) {
                            opts.color_mix_mode    = ColorMixMode(mix_mode);
                            m_preview_params_dirty = true;
                        }
                        ImGui::PopItemWidth();
                        if (ImGui::IsItemHovered())
                            m_imgui->tooltip(_u8L("Layers: the two filaments alternate between print layers, which "
                                                  "blends smoothly on upright surfaces but disappears on flat-facing "
                                                  "ones, where a whole layer is a single band.\n"
                                                  "Surface: a fine checkerboard across the surface, which works at "
                                                  "any angle but can read as texture rather than as a blend."),
                                              m_imgui->scaled(20.f));

                        const int count = int(cached_palette().size());
                        ImGui::TextDisabled("%s", from_u8(Slic3r::format(
                            _u8L("%1% printable colours from %2% filaments"), count,
                            int(m_palette_filaments.size()))).ToUTF8().data());
                    }

                    ImGui::PushItemWidth(m_imgui->scaled(8.4f));
                    if (ImGui::SliderInt(_u8L("Denoise").c_str(), &opts.color_despeckle, 0, 6))
                        m_preview_params_dirty = true;
                    ImGui::PopItemWidth();
                    if (ImGui::IsItemHovered())
                        m_imgui->tooltip(_u8L("Removes stray single triangles of the wrong colour, which is what "
                                              "detail in the image finer than the mesh leaves behind. Each step "
                                              "replaces a triangle's colour with the one most of its neighbours "
                                              "have, so features wider than a triangle are kept. Raise it if the "
                                              "result looks speckled; lower it if fine detail is being eaten."),
                                          m_imgui->scaled(20.f));
                }
            }

            // The lowest painted layer has nothing underneath it to combine with - it *is* the
            // base - so a blend mode would be meaningless (and Multiply/Divide against an implicit
            // zero would annihilate it). build_texture_displacement() forces the first layer to
            // reach a given vertex to behave additively regardless; say so rather than offering a
            // control that silently does nothing.
            if (li == 0) {
                ImGui::TextDisabled("%s", _u8L("Base layer").c_str());
            } else {
                m_imgui->text(_u8L("Blend"));
                ImGui::SameLine();
                const std::string  blend_add      = _u8L("Add");
                const std::string  blend_subtract = _u8L("Subtract");
                const std::string  blend_multiply = _u8L("Multiply");
                const std::string  blend_divide   = _u8L("Divide");
                const char        *blend_items[]  = { blend_add.c_str(), blend_subtract.c_str(), blend_multiply.c_str(),
                                                       blend_divide.c_str() };
                int                blend_mode     = static_cast<int>(layer->blend_mode);
                ImGui::PushItemWidth(m_imgui->scaled(8.4f));
                if (scoped_combo("##blend_mode", &blend_mode, blend_items, IM_ARRAYSIZE(blend_items))) {
                    layer->blend_mode      = static_cast<TextureBlendMode>(blend_mode);
                    m_preview_params_dirty = true;
                }
                ImGui::PopItemWidth();
                if (ImGui::IsItemHovered())
                    m_imgui->tooltip(_u8L("How this layer combines with the layers below it, wherever they overlap. "
                                          "Add and Subtract pile relief on or carve it away. Multiply and Divide "
                                          "scale the relief underneath, which makes this layer act as a mask over "
                                          "it - for those two, Depth is a gain, and a depth of 1 mm on a white "
                                          "part of the texture leaves the layers below unchanged."),
                                      m_imgui->scaled(20.f));
            }

            m_preview_params_dirty |= ImGui::Checkbox(_u8L("Tile").c_str(), &layer->tile_enabled);
            if (layer->tile_enabled) {
                ImGui::SameLine();
                const std::string  tile_method_repeat   = _u8L("Repeat");
                const std::string  tile_method_mirrored  = _u8L("Mirrored repeat");
                const char        *tile_method_items[]   = { tile_method_repeat.c_str(), tile_method_mirrored.c_str() };
                int                tile_method           = static_cast<int>(layer->tile_method);
                ImGui::PushItemWidth(m_imgui->scaled(8.4f));
                if (scoped_combo("##tile_method", &tile_method, tile_method_items, IM_ARRAYSIZE(tile_method_items))) {
                    layer->tile_method      = static_cast<TextureTileMethod>(tile_method);
                    m_preview_params_dirty = true;
                }
                ImGui::PopItemWidth();
            }

            {
                m_imgui->text(_u8L("Projection"));
                ImGui::SameLine();
                const std::string  projection_triplanar   = _u8L("Triplanar (blended)");
                const std::string  projection_cylindrical = _u8L("Cylindrical");
                const std::string  projection_spherical   = _u8L("Spherical");
                const std::string  projection_lscm        = _u8L("Unwrap (LSCM)");
                const std::string  projection_view        = _u8L("From view");
                const char        *projection_items[]     = { projection_triplanar.c_str(), projection_cylindrical.c_str(),
                                                                projection_spherical.c_str(), projection_lscm.c_str(),
                                                                projection_view.c_str() };
                int                projection_method      = static_cast<int>(layer->projection_method);
                ImGui::PushItemWidth(m_imgui->scaled(8.4f));
                if (scoped_combo("##projection_method", &projection_method, projection_items, IM_ARRAYSIZE(projection_items))) {
                    const auto new_method = static_cast<TextureProjectionMethod>(projection_method);
                    // Capture the current view the moment "From view" is chosen, so it does something
                    // sensible immediately rather than projecting from a stale/default direction.
                    if (new_method == TextureProjectionMethod::ViewProjected &&
                        layer->projection_method != TextureProjectionMethod::ViewProjected)
                        capture_view_projection(*layer);
                    layer->projection_method = new_method;
                    m_preview_params_dirty   = true;
                }
                ImGui::PopItemWidth();
                if (ImGui::IsItemHovered())
                    m_imgui->tooltip(layer->projection_method == TextureProjectionMethod::LSCM ?
                                          _u8L("Flattens the painted area and maps the texture onto it with as little "
                                               "stretching as possible. The area is cut into pieces at its sharp edges "
                                               "first (see Seam angle), so each piece can lie flat on its own.") :
                                          _u8L("Triplanar projects the texture from all three axes at once and blends "
                                               "between them, so a patch wrapping around a sharp edge has no seam. "
                                               "Cylindrical and Spherical wrap the texture around the painted area's "
                                               "own centre, for round shapes."),
                                      m_imgui->scaled(20.f));

                if (layer->projection_method == TextureProjectionMethod::LSCM) {
                    ImGui::PushItemWidth(m_imgui->scaled(8.4f));
                    m_preview_params_dirty |= m_imgui->slider_float(_u8L("Seam angle"), &layer->lscm_seam_angle_deg,
                                                                     5.f, 90.f, "%.0f");
                    ImGui::PopItemWidth();
                    if (ImGui::IsItemHovered())
                        m_imgui->tooltip(_u8L("Edges sharper than this are cut, and the pieces either side of them are "
                                              "flattened separately. Lower it to cut more: each piece then lies flat "
                                              "with less stretching, at the cost of the texture not running continuously "
                                              "across the cut. Raise it to keep more of the area in one piece. Corners "
                                              "of a box are 90 degrees, so the default cuts them apart; a smoothly "
                                              "rounded surface stays whole."),
                                          m_imgui->scaled(20.f));

                    if (ImGui::Checkbox(_u8L("Connect islands").c_str(), &layer->auto_connect_islands)) {
                        // Apply (or, when turned off, just stop re-applying) right away rather than
                        // waiting for the next re-unwrap.
                        if (layer->auto_connect_islands && !m_uv_editor_unwrap.empty()) {
                            std::vector<TextureIsland> net = compute_connected_net(m_uv_editor_unwrap);
                            if (net.size() == size_t(m_uv_editor_unwrap.chart_count)) {
                                if (layer->islands.size() < net.size())
                                    layer->islands.resize(net.size());
                                for (size_t i = 0; i < net.size(); ++i)
                                    layer->islands[i] = net[i];
                            }
                        }
                        m_preview_params_dirty = true;
                    }
                    if (ImGui::IsItemHovered())
                        m_imgui->tooltip(_u8L("Lay the unwrap out as a connected net: pieces that share an edge are "
                                              "unfolded next to each other (a cube becomes a joined net rather than six "
                                              "loose squares). They stay separate islands, so you can still move any of "
                                              "them by hand afterwards."),
                                          m_imgui->scaled(20.f));

                    if (is_active) {
                        // Explicit unwrap (#: "Add unwrap button so it does not recompute on every
                        // change"). The LSCM solve runs only when this is pressed - painting, the seam
                        // angle slider and seam marking no longer trigger it - and the pane opens right
                        // afterwards. Re-press it to fold in any edits made since.
                        if (m_imgui->button(_u8L("Unwrap"))) {
                            m_uv_unwrap_pending      = true;
                            m_uv_apply_connected_net = true; // a genuine re-unwrap may relayout the islands
                            m_show_uv_editor         = true;
                            update_uv_editor();
                        }
                        if (ImGui::IsItemHovered())
                            m_imgui->tooltip(_u8L("Flatten the painted area into UV islands and open the UV editor. The "
                                                  "unwrap is computed only when you press this, not on every edit - so "
                                                  "paint, change the seam angle or mark seams first, then press Unwrap to "
                                                  "see the result. Islands can then be moved, rotated and scaled."),
                                              m_imgui->scaled(20.f));

                        // Select mode for the UV pane: whole islands, single vertices, or single edges.
                        // Vertex/Edge are free-form UV editing and feed the per-vertex overrides that the
                        // bake honours; Island is the move/rotate/scale-with-grouping behaviour.
                        if (!m_uv_editor_unwrap.empty()) {
                            m_imgui->text(_u8L("Select:"));
                            const auto set_uv_mode = [&](int mode) {
                                if (mode != m_uv_select_mode) {
                                    m_uv_select_mode = mode;
                                    if (UVEditorCanvas *c = wxGetApp().plater()->get_uv_editor_canvas())
                                        c->set_select_mode(static_cast<UVEditorCanvas::SelectMode>(mode));
                                }
                            };
                            ImGui::SameLine();
                            if (icon_toggle(810, "texture_displacement_uv_select_island.svg", m_uv_select_mode == 0,
                                            _L("Island"), _L("Island - move, rotate and scale whole islands")))
                                set_uv_mode(0);
                            ImGui::SameLine();
                            if (icon_toggle(811, "texture_displacement_uv_select_vertex.svg", m_uv_select_mode == 1,
                                            _L("Vertex"), _L("Vertex - drag vertices to reshape; Shift/Ctrl to multi-select")))
                                set_uv_mode(1);
                            ImGui::SameLine();
                            if (icon_toggle(812, "texture_displacement_uv_select_edge.svg", m_uv_select_mode == 2,
                                            _L("Edge"), _L("Edge - drag edges to reshape; Shift/Ctrl to multi-select")))
                                set_uv_mode(2);
                            if (!layer->lscm_uv_overrides.empty()) {
                                ImGui::SameLine();
                                if (m_imgui->button(_u8L("Clear UV edits"))) {
                                    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Clear texture UV edits"),
                                                                   UndoRedo::SnapshotType::GizmoAction);
                                    layer->lscm_uv_overrides.clear();
                                    m_uv_unwrap_pending = true; // re-solve so the pane drops the edited coords
                                    update_uv_editor();
                                    rebuild_preview();
                                }
                                if (ImGui::IsItemHovered())
                                    m_imgui->tooltip(_u8L("Discard all manual vertex/edge moves and return the unwrap to its "
                                                          "automatic shape."),
                                                      m_imgui->scaled(20.f));
                            }
                        }

                        // Manual seam marking (#9): a click mode that toggles mesh edges as seams.
                        bool seam_mode = m_seam_edit_mode;
                        if (ImGui::Checkbox(_u8L("Mark seams").c_str(), &seam_mode)) {
                            m_seam_edit_mode = seam_mode;
                            if (seam_mode)
                                m_adjust_texture_mode = false; // the two click modes are mutually exclusive
                            else {
                                m_seam_hover_edge = { -1, -1 }; // drop the hover highlight when leaving the mode
                                m_seam_hover_vertex = -1;
                                m_seam_hover_glmodel.reset();
                                m_seam_path_anchor = -1;
                                m_seam_anchor_glmodel.reset();
                            }
                            m_parent.set_as_dirty();
                        }
                        if (ImGui::IsItemHovered())
                            m_imgui->tooltip(_u8L("Click edges on the model to cut the unwrap along them, like marking a "
                                                  "seam in Blender. The edge under the cursor is highlighted yellow; click "
                                                  "to mark it red. Click a marked (red) edge again to unmark it. Hold Ctrl "
                                                  "and drag to rotate the view. Painting is paused while this is on."),
                                              m_imgui->scaled(20.f));
                        if (m_seam_edit_mode) {
                            // Shortest-path mode, for dense meshes: click two points, seam the whole path.
                            ImGui::SameLine();
                            bool path_mode = m_seam_path_mode;
                            if (ImGui::Checkbox(_u8L("Path").c_str(), &path_mode)) {
                                m_seam_path_mode    = path_mode;
                                m_seam_path_anchor  = -1;
                                m_seam_hover_edge   = { -1, -1 }; // hover target type changes with the mode
                                m_seam_hover_vertex = -1;
                                m_seam_hover_glmodel.reset();
                                m_seam_anchor_glmodel.reset();
                                m_parent.set_as_dirty();
                            }
                            if (ImGui::IsItemHovered())
                                m_imgui->tooltip(_u8L("Instead of clicking every edge, click a start point and then an end "
                                                      "point: the whole shortest path between them is seamed at once (green "
                                                      "marks the start). Each click extends the seam from the last point. "
                                                      "Best for dense meshes."),
                                                  m_imgui->scaled(20.f));
                        }
                        if (!layer->lscm_seam_edges.empty()) {
                            ImGui::SameLine();
                            if (m_imgui->button(_u8L("Clear seams"))) {
                                Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Clear texture seams"),
                                                               UndoRedo::SnapshotType::GizmoAction);
                                layer->lscm_seam_edges.clear();
                                rebuild_preview();
                            }
                        }

                        // What the UV editor is actually showing. Cheap, and the only way to tell an
                        // unwrap that produced nothing apart from one merely framed off-screen. The
                        // island tools (snap, average scale, cut) and the gesture hints now live in the
                        // UV pane itself - its toolbar and status line - rather than here (#18).
                        if (m_uv_editor_unwrap.empty())
                            m_imgui->text(_u8L("Press Unwrap to flatten the painted area."));
                        else
                            m_imgui->text(Slic3r::format(_u8L("Unwrap: %1% islands, %2% faces, %3% verts."),
                                                         m_uv_editor_unwrap.chart_count, m_uv_editor_unwrap.indices.size(),
                                                         m_uv_editor_unwrap.uvs.size()));
                    }
                }

                if (layer->projection_method == TextureProjectionMethod::ViewProjected) {
                    // Re-capture the projector from wherever the camera is now (#6): orbit the model,
                    // press this, and the texture is re-laid from the new angle.
                    if (m_imgui->button(_u8L("Capture current view"))) {
                        capture_view_projection(*layer);
                        // Selecting the visible faces *after* capturing means the projector axes are
                        // already the ones the selection was made against - the two describe the
                        // same viewpoint, which is the whole point of the option.
                        if (m_project_only_visible && select_visible_faces() == 0)
                            show_error(nullptr, _u8L("Nothing is visible from this angle - turn the model to face the "
                                                     "part you want to project onto."));
                        m_preview_params_dirty = true;
                        update_projector();
                    }
                    if (ImGui::IsItemHovered())
                        m_imgui->tooltip(_u8L("Projects the texture straight onto the painted area from the direction "
                                              "you are currently looking, like a slide projector. Faces angled away from "
                                              "that direction will stretch - turn the model to where you want the "
                                              "texture crisp, then capture."),
                                          m_imgui->scaled(20.f));

                    if (ImGui::Checkbox(_u8L("Project only on visible").c_str(), &m_project_only_visible)) {
                        if (m_project_only_visible && select_visible_faces() == 0)
                            show_error(nullptr, _u8L("Nothing is visible from this angle - turn the model to face the "
                                                     "part you want to project onto."));
                        m_preview_params_dirty = true;
                        update_projector();
                    }
                    if (ImGui::IsItemHovered())
                        m_imgui->tooltip(_u8L("Paints exactly the faces you can currently see - facing the camera and "
                                              "not hidden behind anything else - and projects onto those. This replaces "
                                              "the layer's painted area, and is re-applied each time you capture the "
                                              "view."),
                                          m_imgui->scaled(20.f));

                    ImGui::Separator();
                    bool projector_open = m_projector_frame != nullptr && m_projector_frame->IsShown();
                    if (ImGui::Checkbox(_u8L("Projection frame").c_str(), &projector_open))
                        show_projector(projector_open);
                    if (ImGui::IsItemHovered())
                        m_imgui->tooltip(_u8L("Opens a see-through window you drag over the model. Whatever shows "
                                              "through it is what the texture is projected onto, and the window's "
                                              "border becomes the edge of the projection. Move and resize it to frame "
                                              "the area you want, then press Apply."),
                                          m_imgui->scaled(20.f));

                    if (projector_open) {
                        ImGui::PushItemWidth(m_imgui->scaled(6.f));
                        if (ImGui::SliderInt(_u8L("Opacity").c_str(), &m_projector_opacity, 20, 255))
                            m_projector_frame->set_opacity(m_projector_opacity);
                        ImGui::PopItemWidth();

                        if (m_imgui->button(_u8L("Apply projection frame"))) {
                            const int painted = apply_projection_frame();
                            if (painted == 0)
                                show_error(nullptr, _u8L("Nothing of the model is inside the frame - move it over the "
                                                         "part you want to project onto."));
                            else if (painted < 0)
                                show_error(nullptr, _u8L("The frame could not be applied. Make sure it overlaps the "
                                                         "3D view."));
                        }
                        if (ImGui::IsItemHovered())
                            m_imgui->tooltip(_u8L("Projects the texture through the frame from the direction you are "
                                                  "looking now, and paints the visible faces inside it. This replaces "
                                                  "the layer's painted area. The result is fixed to the model, so you "
                                                  "can orbit freely afterwards."),
                                              m_imgui->scaled(20.f));
                    }

                    if (layer->view_project_projective) {
                        m_imgui->text(_u8L("Placed by projection frame."));
                        ImGui::SameLine();
                        if (m_imgui->button(_u8L("Clear"))) {
                            // Back to the plain axis projection, where tiling/rotation/offset mean
                            // something again - the matrix path deliberately ignores them.
                            layer->view_project_projective = false;
                            layer->tile_enabled            = true;
                            m_preview_params_dirty         = true;
                        }
                    }
                }
            }

            if (is_active) {
                bool adjust_on = m_adjust_texture_mode;
                if (ImGui::Checkbox(_u8L("Adjust placement").c_str(), &adjust_on)) {
                    if (adjust_on) {
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
            ImGui::EndGroup();
            // A click that lands on the layer's body (not on a widget, which consumes its own click)
            // selects it too, so the whole block reads as one clickable object (#12).
            if (!is_active && ImGui::IsItemClicked())
                set_active_layer(layer->slot);

            if (is_active) {
                const float  pad = m_imgui->scaled(0.25f);
                const ImVec2 rmin(block_min.x - pad, block_min.y - pad);
                const ImVec2 rmax(block_rx, ImGui::GetCursorScreenPos().y);
                dl->ChannelsSetCurrent(0);
                dl->AddRectFilled(rmin, rmax, ImGui::GetColorU32(ImVec4(0.0f, 0.59f, 0.53f, 0.22f)), m_imgui->scaled(0.2f));
                dl->ChannelsMerge();
            }

            ImGui::PopID();
            ImGui::Separator();
        }

        ImGui::EndChild();
        ImGui::PopStyleColor();

        if (slot_to_remove >= 0)
            remove_texture_layer(slot_to_remove); // deferred: see slot_to_remove's declaration
    }

    // ImGui sliders report "changed" continuously on every frame while being dragged, not just
    // once on release - rebuilding the preview (a real CPU mesh recompute) on every one of those
    // frames is what made dragging these sliders feel slow. Only rebuild once the mouse button
    // that's driving the drag is released, i.e. once per edit instead of dozens of times per drag.
    // The feature-adaptive subdivision follows the *displaced* surface (it samples depth * texture),
    // so depth / tile-size / rotation / invert all change where it puts triangles. The heavy
    // displacement preview below only rebuilds on release, which made the subdivide wireframe look
    // like it ignored those edits - so rebuild it live here (during the drag), the same cadence its
    // own sliders use. Cheap: it is bounded by the painted region.
    if (m_preview_params_dirty && m_subdivide_editing && m_subdivide_adaptive && m_subdivide_feature)
        rebuild_subdivide_preview();
    if (m_preview_params_dirty && (m_auto_update || !ImGui::IsMouseDown(ImGuiMouseButton_Left))) {
        rebuild_preview();
        // Same edits (tile size, rotation, offset, a new texture) are what the projector window
        // draws, so it refreshes on the same one-per-edit cadence. No-op while it is closed.
        update_projector();
        m_preview_params_dirty = false;
    }
    // (The "Add layer" button now lives next to the "Texture layers" heading, as an icon.)

    // Settings for the whole stack rather than one layer, so they sit outside the layer list. Standard
    // mode pins them (see apply_standard_mode_presets()) instead of showing them.
    if (pro_mode() && mv != nullptr) {
        TextureDisplacementOptions &opts = mv->texture_displacement_options;
        ImGui::Separator();

        m_preview_params_dirty |= ImGui::Checkbox(_u8L("Displace up to the border").c_str(), &opts.displace_border);
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_u8L("Move the outermost ring of painted vertices as well, so the relief runs all the "
                                  "way to the edge of the painted area. Turn this off to hold that ring flat, which "
                                  "keeps the displacement strictly inside the paint but flattens the pattern right "
                                  "at the border."),
                              m_imgui->scaled(20.f));

        m_preview_params_dirty |= ImGui::Checkbox(_u8L("Smooth result").c_str(), &opts.smooth_enabled);
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_u8L("Relax the displaced surface after the textures have been applied, to round off "
                                  "the hard steps a bitmap height map leaves behind. Only the vertices the "
                                  "displacement moved are touched. This is geometry smoothing - the per-layer "
                                  "\"Blur\" slider instead softens the height map before it is sampled."),
                              m_imgui->scaled(20.f));
        if (opts.smooth_enabled) {
            ImGui::PushItemWidth(m_imgui->scaled(8.4f));
            float percent = opts.smooth_strength * 100.f;
            if (m_imgui->slider_float(std::string(_u8L("Smoothing (%)")) + "##dispsmooth", &percent, 1.f, 100.f, "%.0f", 1.f)) {
                opts.smooth_strength   = std::clamp(percent / 100.f, 0.01f, 1.f);
                m_preview_params_dirty = true;
            }
            if (ImGui::SliderInt((_u8L("Passes") + "##dispsmoothit").c_str(), &opts.smooth_iterations, 1, 10)) {
                opts.smooth_iterations = std::clamp(opts.smooth_iterations, 1, 10);
                m_preview_params_dirty = true;
            }
            if (ImGui::IsItemHovered())
                m_imgui->tooltip(_u8L("How many relaxation passes to run. More passes reach further across the "
                                      "surface; strength controls how much each one moves a vertex."),
                                  m_imgui->scaled(20.f));
            ImGui::PopItemWidth();

            m_preview_params_dirty |= ImGui::Checkbox(_u8L("Ignore outer ring").c_str(), &opts.smooth_skip_border);
            if (ImGui::IsItemHovered())
                m_imgui->tooltip(_u8L("Leave the outermost ring of painted vertices out of the smoothing. Their "
                                      "neighbours outside the paint never move, so relaxing them drags the relief "
                                      "back down and the pattern comes out half-melted right at the edge. Turn this "
                                      "off only if you want that edge softened on purpose."),
                                  m_imgui->scaled(20.f));

            // The settings above ride along with Preview/Bake. This button is for geometry that has
            // *already* been baked, where there is no displacement left to fold the smoothing into.
            if (m_imgui->button(_u8L("Smooth baked mesh now")))
                smooth_model();
            if (ImGui::IsItemHovered())
                m_imgui->tooltip(_u8L("Apply the smoothing above to the model's real geometry right now, using the "
                                      "painted area to decide what to touch. For relief that is already baked in - "
                                      "unbaked displacement is smoothed by Bake itself."),
                                  m_imgui->scaled(20.f));
        }
    }

    ImGui::Separator();
    m_imgui->text(_u8L("Subdivision"));

    // The triangle budget is the one subdivision control Standard mode keeps: its right value depends
    // on the part rather than on the recipe (a big model, or a fine texture, simply needs more of them),
    // and raising or lowering it is safe without understanding anything else in this section. Shared
    // with the Pro layout below so there is one definition of the widget.
    const auto budget_slider = [this]() {
        ImGui::PushItemWidth(m_imgui->scaled(8.4f));
        if (ImGui::SliderInt((_u8L("Added triangles (k)") + "##subdivbudget").c_str(), &m_subdivide_budget_k, 10, 2000)) {
            m_subdivide_budget_k = std::clamp(m_subdivide_budget_k, 10, 2000);
            if (m_subdivide_editing)
                rebuild_subdivide_preview();
            m_parent.set_as_dirty();
        }
        ImGui::PopItemWidth();
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_u8L("How many thousand triangles the refinement may add. It always splits the "
                                  "worst-fitting triangle first, so a run that uses the whole budget has still spent "
                                  "it where it shows most - raise this if the result still looks too coarse."),
                              m_imgui->scaled(20.f));
    };

    // Everything else from here to the Bake row is mesh preparation, which is exactly what Standard
    // mode takes over: it pins those to one recipe and runs them from the Bake button (see
    // bake_standard()), so showing them would only invite changing numbers that get overwritten.
    if (!pro_mode())
        budget_slider();
    if (pro_mode()) {

    if (ImGui::Checkbox(_u8L("Only painted area (adaptive)").c_str(), &m_subdivide_adaptive)) {
        if (m_subdivide_editing)
            rebuild_subdivide_preview(); // switch the wireframe between the uniform and adaptive result
        m_parent.set_as_dirty();
    }
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_u8L("Refine only where you have painted, down to a target edge length, instead of splitting "
                              "the whole model. Keeps the triangle count down on a big part with a small decal, and - "
                              "unlike whole-model subdivision - your paint is carried onto the finer mesh instead of "
                              "being cleared."),
                          m_imgui->scaled(20.f));

    if (m_subdivide_adaptive) {
        if (m_subdivide_target_mm <= 0.f && mv != nullptr) {
            // Seed the target at about half the mesh's mean edge length, so the default already adds
            // a useful amount of detail rather than landing on "no change".
            const indexed_triangle_set &its = mv->mesh().its;
            double sum = 0.0; size_t cnt = 0;
            for (const stl_triangle_vertex_indices &tri : its.indices)
                for (int i = 0; i < 3; ++i) {
                    sum += (its.vertices[tri[i]] - its.vertices[tri[(i + 1) % 3]]).norm();
                    ++cnt;
                }
            m_subdivide_target_mm = cnt > 0 ? std::clamp(float(sum / double(cnt)) * 0.5f, 0.001f, 20.f) : 1.f;
        }
        // Live preview: rebuild the wireframe as the slider moves, not only on release, so it tracks
        // the value. The rebuild is bounded by the painted region, so it stays responsive.
        const auto preview_live = [this]() {
            if (m_subdivide_editing)
                rebuild_subdivide_preview();
            m_parent.set_as_dirty();
        };

        if (ImGui::Checkbox(_u8L("Follow texture detail").c_str(), &m_subdivide_feature)) {
            if (m_subdivide_editing)
                rebuild_subdivide_preview();
            m_parent.set_as_dirty();
        }
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_u8L("Put triangles only where the texture actually bends - dense over hills, ridges and "
                                  "noise, sparse over flat areas and smooth slopes - instead of an even density "
                                  "everywhere. Uses the combined displacement of all painted layers."),
                              m_imgui->scaled(20.f));

        ImGui::PushItemWidth(m_imgui->scaled(8.4f));
        // The edge-length target is a baseline in both modes. In feature mode it is what guarantees
        // the curvature test can actually see the texture: left too coarse, a big triangle over a
        // fine pattern can sample four points that all happen to land at similar heights, report no
        // error, and stall before refinement ever starts. "##subdiv" avoids an ID clash with the
        // remesh "Target edge (mm)" slider below (same label == same widget to ImGui).
        if (m_imgui->slider_float(std::string(m_subdivide_feature ? _u8L("Max edge (mm)") : _u8L("Target edge (mm)")) +
                                      "##subdiv",
                                  &m_subdivide_target_mm, 0.001f, 20.f, "%.3f", ImGuiLogSlider))
            preview_live();
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(m_subdivide_feature ?
                                 _u8L("Nothing in the painted area stays coarser than this, even where the texture is "
                                      "flat. Keep it near the size of the features you want picked up - too coarse and "
                                      "fine detail can be missed entirely.") :
                                 _u8L("Triangles in the painted area are split until every edge is at or below this "
                                      "length. Smaller means finer detail and more triangles."),
                             m_imgui->scaled(20.f));

        if (m_subdivide_feature) {
            // On top of the baseline: the chord-error tolerance, and the resolution floor.
            if (m_imgui->slider_float(std::string(_u8L("Detail (mm)")) + "##subdivdetail", &m_subdivide_detail_mm,
                                      0.001f, 1.f, "%.3f", ImGuiLogSlider))
                preview_live();
            if (ImGui::IsItemHovered())
                m_imgui->tooltip(_u8L("How closely the mesh follows the texture's relief. Smaller captures finer bumps; "
                                      "larger only chases the big features."),
                                  m_imgui->scaled(20.f));
            if (m_imgui->slider_float(std::string(_u8L("Min edge (mm)")) + "##subdivmin", &m_subdivide_min_edge_mm,
                                      0.001f, 20.f, "%.3f", ImGuiLogSlider))
                preview_live();
            if (ImGui::IsItemHovered())
                m_imgui->tooltip(_u8L("The finest triangle size refinement will ever produce. Smaller captures finer "
                                      "relief; also stops runaway subdivision at a sharp texture step, where the "
                                      "surface never becomes flat."),
                                  m_imgui->scaled(20.f));
        }

        // Applies in both adaptive sub-modes: it is not a texture-detail criterion, it is about the
        // step the bake puts at the paint's edge, which exists whether or not "Follow texture detail"
        // is on. 0 turns the band off and restores the old behaviour.
        if (m_imgui->slider_float(std::string(_u8L("Edge detail (mm)")) + "##subdivborder", &m_subdivide_border_mm,
                                  0.f, 5.f, "%.3f"))
            preview_live();
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_u8L("Triangle size along the boundary of the painted area. The relief drops back to the "
                                  "flat surface across that boundary, and the triangles spanning the drop are what you "
                                  "see as a jagged rim around an unpainted region - smaller values make the outline "
                                  "cleaner. Costs triangles along the outline only, not over the whole area. "
                                  "0 turns it off."),
                              m_imgui->scaled(20.f));

        // Only worth showing when a layer is actually colouring: with no colour there is no boundary
        // for it to refine and the control would do nothing whatever it is set to.
        if (mv != nullptr && any_layer_colors(*mv)) {
            if (m_imgui->slider_float(std::string(_u8L("Colour detail (mm)")) + "##subdivcolor",
                                      &m_subdivide_color_mm, 0.f, 5.f, "%.3f"))
                preview_live();
            if (ImGui::IsItemHovered())
                m_imgui->tooltip(_u8L("Triangle size along a boundary between two colours. Colour is assigned per "
                                      "triangle, so an edge between two filaments can only be as clean as the "
                                      "triangles along it - and the relief criteria cannot see it at all, because "
                                      "the surface is perfectly smooth across a change of colour. Costs triangles "
                                      "along the boundaries only. 0 turns it off."),
                                  m_imgui->scaled(20.f));
        }

        ImGui::PopItemWidth();
        budget_slider();

        if (m_subdivide_editing && m_subdivide_preview_tris > 0)
            m_imgui->text(Slic3r::format(_u8L("Preview: %1% triangles"), m_subdivide_preview_tris));
    } else {
        ImGui::PushItemWidth(m_imgui->scaled(8.4f));
        if (ImGui::SliderInt(_u8L("Subdivide steps").c_str(), &m_subdivide_count, 0, 5)) {
            m_subdivide_count = std::clamp(m_subdivide_count, 0, 5);
            if (m_subdivide_editing)
                rebuild_subdivide_preview(); // a count of 0 clears the preview, it doesn't compute one
            m_parent.set_as_dirty();
        }
        ImGui::PopItemWidth();
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_u8L("How many times to split every triangle into four. Each step roughly quadruples the "
                                  "triangle count, so there are enough vertices for the height texture to displace. "
                                  "0 means no subdivision."),
                              m_imgui->scaled(20.f));
    }

    // Apply is a no-op when there is nothing to commit: 0 uniform passes, or an adaptive target that
    // is not set (the preview covers the painted-area check itself).
    const bool subdivide_ready = m_subdivide_adaptive ? (m_subdivide_target_mm > 0.f) : (m_subdivide_count >= 1);
    if (!m_subdivide_editing) {
        if (m_imgui->button(_u8L("Preview subdivision"))) {
            m_subdivide_editing = true;
            rebuild_subdivide_preview();
            m_parent.set_as_dirty();
        }
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_u8L("Shows the subdivided mesh as a wireframe without changing the model. Press Apply to "
                                  "commit it, or Done to leave the model as it is."),
                              m_imgui->scaled(20.f));
    } else {
        m_imgui->disabled_begin(!subdivide_ready || m_prepare_in_progress || m_bake_in_progress);
        if (m_imgui->button(_u8L("Apply"))) {
            if (m_subdivide_adaptive) {
                subdivide_model_adaptive(); // refines only the painted area, carrying the paint forward
            } else {
                subdivide_model(); // commits m_subdivide_count passes (takes its own snapshot)
                // Back to 0 rather than staying at the count just applied: the mesh is now up to 4^N
                // times denser, so re-previewing the same N passes on top of it is both pointless (the
                // density asked for is already committed) and by far the slowest thing this panel does.
                m_subdivide_count = 0;
            }
            rebuild_subdivide_preview();
            m_parent.set_as_dirty();
        }
        m_imgui->disabled_end();
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(m_subdivide_adaptive ?
                                 _u8L("Refines the painted area to the target edge length and carries your paint onto "
                                      "the finer mesh. The rest of the model is left as it is.") :
                                 _u8L("Replaces the model's geometry with the subdivided mesh and clears any not-yet-baked "
                                      "paint on it (already-baked bumps are unaffected)."),
                             m_imgui->scaled(20.f));
        ImGui::SameLine();
        if (m_imgui->button(_u8L("Done"))) {
            m_subdivide_editing       = false;
            m_subdivide_preview_tris = -1;
            m_subdivide_preview_glmodel.reset();
            m_parent.set_as_dirty();
        }
    }

    // Remesh: even out uneven triangle sizes (CGAL isotropic remeshing). GPU Delaunay isn't practical
    // here, but this delivers the same goal - a consistent triangle size across the whole model.
    m_imgui->text(_u8L("Remeshing"));
    if (m_remesh_target_edge_mm <= 0.f && mv != nullptr) {
        // Seed the target with the model's current mean edge length, so the default is a sensible
        // "make everything about the size it already averages".
        const indexed_triangle_set &its = mv->mesh().its;
        double sum = 0.0; size_t cnt = 0;
        for (const stl_triangle_vertex_indices &tri : its.indices)
            for (int i = 0; i < 3; ++i) {
                sum += (its.vertices[tri[i]] - its.vertices[tri[(i + 1) % 3]]).norm();
                ++cnt;
            }
        m_remesh_target_edge_mm = cnt > 0 ? std::clamp(float(sum / double(cnt)), 0.1f, 20.f) : 1.f;
    }
    ImGui::PushItemWidth(m_imgui->scaled(8.4f));
    m_imgui->slider_float(_u8L("Target edge (mm)"), &m_remesh_target_edge_mm, 0.1f, 20.f, "%.2f", ImGuiLogSlider);
    ImGui::PopItemWidth();

    ImGui::Checkbox(_u8L("Keep sharp edges").c_str(), &m_remesh_keep_sharp_edges);
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_u8L("Holds hard edges and open borders in place while the rest is remeshed. Without it "
                              "the remesher slides vertices along the surface and rounds every crisp edge off - "
                              "a cube comes back with wobbly edges."),
                          m_imgui->scaled(20.f));
    if (m_remesh_keep_sharp_edges) {
        ImGui::PushItemWidth(m_imgui->scaled(8.4f));
        m_imgui->slider_float(_u8L("Sharp edge angle"), &m_remesh_sharp_angle_deg, 5.f, 90.f, "%.0f deg");
        ImGui::PopItemWidth();
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_u8L("Edges bent by more than this count as hard features and are kept. Lower keeps "
                                  "more detail but leaves more of the mesh untouched; higher remeshes more freely."),
                              m_imgui->scaled(20.f));
    }

    m_imgui->disabled_begin(mv == nullptr || m_prepare_in_progress || m_bake_in_progress);
    if (m_imgui->button(_u8L("Remesh")))
        remesh_model();
    m_imgui->disabled_end();
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_u8L("Rebuilds the whole model with triangles close to this edge length - splitting the big "
                              "ones and merging the small ones - so displacement has an even density to work with. "
                              "Replaces the geometry; your paint is carried onto the new triangles spatially, so it "
                              "survives (already-baked bumps are kept too)."),
                          m_imgui->scaled(20.f));

    } // pro_mode()

    ImGui::Separator();

    m_imgui->disabled_begin(mv == nullptr || !mv->is_texture_displacement_painted());
    if (m_imgui->button(m_desc.at("remove_all"))) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Reset texture displacement selection"), UndoRedo::SnapshotType::GizmoAction);
        int idx = -1;
        for (ModelVolume *v : mo->volumes)
            if (v->is_model_part()) {
                ++idx;
                m_triangle_selectors[idx]->reset();
                m_triangle_selectors[idx]->request_update_render_data();
            }
        update_model_object();
        m_parent.set_as_dirty();
    }
    m_imgui->disabled_end();

    // Next to Bake and shown in both modes: the two pipelines have to be switchable on their own.
    if (mv != nullptr) {
        TextureDisplacementOptions &opts = mv->texture_displacement_options;
        ImGui::Separator();
    m_preview_params_dirty |= ImGui::Checkbox(_u8L("Experimental bake pipeline").c_str(), &opts.pipeline_v2);
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_u8L("Bake with the alternative pipeline: it refines, cleans up sliver triangles, "
                              "displaces and simplifies in one run, instead of moving the vertices the mesh "
                              "already has. Nothing needs preparing first - Subdivide and Remesh are ignored. "
                              "It does not produce colours yet, because it rebuilds the topology."),
                          m_imgui->scaled(20.f));
    if (opts.pipeline_v2) {
        ImGui::PushItemWidth(m_imgui->scaled(8.4f));
        if (m_imgui->slider_float(std::string(_u8L("Edge length (mm)")) + "##v2edge", &opts.v2_refine_mm,
                                  0.02f, 2.f, "%.1f", ImGuiLogSlider)) {
            opts.v2_refine_mm      = std::clamp(opts.v2_refine_mm, 0.02f, 2.f);
            m_preview_params_dirty = true;
        }
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_u8L("Triangle size the painted area is refined to before displacement. This is "
                                  "what decides how much of the texture the mesh can carry."),
                              m_imgui->scaled(20.f));
        if (ImGui::SliderInt((_u8L("Triangle budget (k)") + "##v2budget").c_str(), &opts.v2_max_triangles_k,
                             0, 4000)) {
            opts.v2_max_triangles_k = std::clamp(opts.v2_max_triangles_k, 0, 4000);
            m_preview_params_dirty  = true;
        }
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_u8L("Triangles to simplify down to after displacement, in thousands. 0 turns "
                                  "simplification off, which is worth comparing on its own."),
                              m_imgui->scaled(20.f));
        ImGui::PopItemWidth();
        m_preview_params_dirty |= ImGui::Checkbox(_u8L("Clean up slivers").c_str(), &opts.v2_regularize);
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_u8L("Collapse the thin triangles refinement inherits from the model's own "
                                  "tessellation, before displacement samples them. A sliver's three corners "
                                  "land on three unrelated parts of the texture, which is what makes the "
                                  "relief look jagged."),
                              m_imgui->scaled(20.f));
    }
    }

    ImGui::SameLine();
    m_imgui->disabled_begin(m_bake_in_progress || m_prepare_in_progress || mv == nullptr ||
                            !mv->is_texture_displacement_painted());
    if (m_imgui->button(m_prepare_in_progress ? _L("Preparing...") :
                        m_bake_in_progress    ? _L("Baking...") :
                                                m_desc.at("bake"))) {
        // Standard mode's Bake is the whole pipeline (remesh -> refine -> displace); Pro's is only the
        // displacement, because there the user has already prepared the mesh with the controls above.
        if (pro_mode())
            bake();
        else
            bake_standard();
    }
    m_imgui->disabled_end();
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(pro_mode() ?
                             _u8L("Turn the painted height maps into real geometry, by moving the vertices that are "
                                  "already there. Use Subdivide first if the mesh is too coarse to show the detail.") :
                             _u8L("Turn the painted height maps into real geometry. The mesh is remeshed to an even "
                                  "density and refined where the texture bends first, so the detail has vertices to "
                                  "land on - all in one step."),
                         m_imgui->scaled(20.f));

    ImGui::Separator();
    if (m_imgui->button(_L("Close")))
        m_parent.reset_all_gizmos();

    GizmoImguiEnd();
    ImGuiWrapper::pop_toolbar_style();

    // Drawn last, over everything, via the foreground draw list: the +/- add-remove sign next to the
    // 3D cursor. Still inside the gizmo's ImGui frame here, which is what render_paint_cursor_hint() needs.
    render_paint_cursor_hint();
}

} // namespace Slic3r::GUI
