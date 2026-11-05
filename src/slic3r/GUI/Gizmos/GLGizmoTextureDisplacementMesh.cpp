// One of the files GLGizmoTextureDisplacement is defined across. The class is declared once, in
// GLGizmoTextureDisplacement.hpp; its members are defined in GLGizmoTextureDisplacement.cpp and in the
// siblings named after the part of the gizmo they hold - Chroma, Library, Mesh, Preview, UV. The split
// is along those lines and nothing else: no member moved between parts, and the class did not change.
// Helpers more than one part needs live in GLGizmoTextureDisplacementInternal.hpp.

// The mesh and colour pipeline the bake runs on: selecting faces, collecting the painted region and
// carrying a paint mask across a subdivision or a remesh, prepare_mesh() itself, and the filament
// palette - the mix ranking, the Lab lookup cube the quantizer is built from, and the texture uploads.

#include "GLGizmoTextureDisplacement.hpp"

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
#include "GLGizmoTextureDisplacementInternal.hpp"

namespace Slic3r::GUI {

using namespace gizmo_td;

namespace {
// Everything above is about drawing a ~48 px panel row, and none of it applies to the height texture
// the fast-preview *shader* samples: that one is magnified across the model, not minified into a
// row, and every texel it loses is relief the preview cannot show. It gets its own upload at (up to)
// this size, so the shaded preview reads the same height field the bake does instead of a 128 px box
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

// mix_targets(): the most pixels read per layer, and the histogram bins kept over all layers. Together
// they bound rank_mixes() to candidates x MIX_TARGET_BINS colour differences, the same order as filling
// the quantizer's lookup cube.
constexpr size_t MIX_TARGET_SAMPLES = size_t(1) << 20;
constexpr size_t MIX_TARGET_BINS    = 256;
// rank_mixes() stops once the best remaining mix would improve the match by less than this, in
// CIEDE2000 averaged over every pixel of the colouring layers (see mix_targets()): a mix that only
// touches a few stray pixels is not worth a filament slot.
constexpr float MIN_MIX_GAIN = 0.05f;

// The project's mixed filament slots, one string each, as the palette cache compares them: anything
// that changes which of them a mix can reuse changes this.
std::vector<std::string> mixed_slot_signature(const DynamicPrintConfig &project_config)
{
    std::vector<std::string> out;
    const auto *is_mixed = project_config.option<ConfigOptionBools>("filament_is_mixed");
    const auto *comps    = project_config.option<ConfigOptionStrings>("filament_mixed_components");
    const auto *ratios   = project_config.option<ConfigOptionStrings>("filament_mixed_sublayer_ratios");
    const auto *gradient = project_config.option<ConfigOptionBools>("filament_mixed_gradient");
    if (is_mixed == nullptr || comps == nullptr || ratios == nullptr)
        return out;
    for (size_t i = 0; i < is_mixed->values.size(); ++i)
        if (is_mixed->values[i])
            out.push_back(std::to_string(i) + ':' + (i < comps->values.size() ? comps->values[i] : std::string()) + '|' +
                          (i < ratios->values.size() ? ratios->values[i] : std::string()) + '|' +
                          (gradient != nullptr && i < gradient->values.size() && gradient->values[i] ? "g" : ""));
    return out;
}

// Whether two palettes would draw and print the same.
bool same_palette(const std::vector<PrintableColor> &l, const std::vector<PrintableColor> &r)
{
    return std::equal(l.begin(), l.end(), r.begin(), r.end(), [](const PrintableColor &x, const PrintableColor &y) {
        return x.a == y.a && x.b == y.b && x.num == y.num && x.den == y.den && x.rgb == y.rgb;
    });
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
} // namespace

int GLGizmoTextureDisplacement::select_visible_faces(const std::array<float, 12> *uv_clip)
{
    const ModelVolume *owner = texture_volume();
    ModelObject       *mo    = m_c->selection_info()->model_object();
    // Every model part, at its selector's and its raycaster's index (see texture_parts()): what is in
    // view gets painted whichever part it belongs to, and one part can hide another.
    const std::vector<ModelVolume *>         parts      = texture_parts();
    const std::vector<const MeshRaycaster *> raycasters = m_c->raycaster()->raycasters();
    if (owner == nullptr || mo == nullptr || parts.size() > m_triangle_selectors.size() || parts.size() > raycasters.size())
        return 0;

    const Selection   &selection      = m_parent.get_selection();
    const Transform3d  instance       = mo->instances[selection.get_instance_idx()]->get_transformation().get_matrix();
    const Transform3d  owner_to_world = instance * owner->get_matrix();
    const Camera      &camera         = wxGetApp().plater()->get_camera();

    // Pass 1 (cheap): drop back-facing triangles. Under perspective the view direction varies across
    // the model, so it is taken per triangle from the eye to the centroid; under an orthographic
    // camera get_position() is still a point on the view axis, so the same expression stays correct
    // in direction terms for everything actually on screen.
    const Vec3d eye = camera.get_position();
    const bool  ortho = camera.get_type() == Camera::EType::Ortho;
    const Vec3d fwd   = camera.get_dir_forward();

    std::vector<Geometry::Transformation> trafos;
    std::vector<std::vector<Vec3f>>       centroids(parts.size());    // world coords, what get_unobscured_idxs() expects
    std::vector<std::vector<unsigned>>    front_facing(parts.size()); // parallel: which facet each centroid came from
    trafos.reserve(parts.size());
    for (size_t p = 0; p < parts.size(); ++p) {
        const ModelVolume          &part = *parts[p];
        const indexed_triangle_set &its  = part.mesh().its;
        trafos.emplace_back(instance * part.get_matrix());
        const Transform3d &to_world = trafos.back().get_matrix();
        // The projection frame maps the owner's local coordinates (see apply_projection_frame()), so
        // another part's centroids are brought into them before the clip test.
        const Transform3d to_owner = (&part == owner) ? Transform3d::Identity() : Transform3d(owner_to_world.inverse() * to_world);

        // Normals transform by the inverse transpose, not by the matrix itself - with a non-uniform
        // scale the two differ, and using the wrong one flips the facing test on the scaled axes.
        const Matrix3d normal_matrix = to_world.matrix().block<3, 3>(0, 0).inverse().transpose();

        centroids[p].reserve(its.indices.size() / 2);
        front_facing[p].reserve(its.indices.size() / 2);
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
                if (!project_uv_projective(*uv_clip, (to_owner * centroid_local).cast<float>(), uv))
                    continue; // behind the projector
                if (uv.x() < 0.f || uv.x() > 1.f || uv.y() < 0.f || uv.y() > 1.f)
                    continue;
            }
            centroids[p].emplace_back(centroid_world.cast<float>());
            front_facing[p].push_back(unsigned(f));
        }
    }

    // Pass 2 (the expensive one): a real ray query per surviving centroid, so geometry in front of a
    // front-facing triangle correctly hides it - the far inner wall of a cup is front-facing but not
    // visible. This is why the whole thing is click-driven rather than live. Each raycaster only sees
    // its own part's mesh, so a centroid has to be clear of every one of them; its own part goes first,
    // since that is what hides most of them.
    size_t visible = 0;
    {
        wxBusyCursor wait;
        const ClippingPlane *clipping_plane = m_c->object_clipper()->get_clipping_plane();
        for (size_t p = 0; p < parts.size(); ++p) {
            std::vector<Vec3f>    &points = centroids[p];
            std::vector<unsigned> &facets = front_facing[p];
            const auto keep_unobscured_by = [&](size_t q) {
                const std::vector<unsigned> clear = raycasters[q]->get_unobscured_idxs(trafos[q], camera, points, clipping_plane);
                std::vector<Vec3f>    kept_points;
                std::vector<unsigned> kept_facets;
                kept_points.reserve(clear.size());
                kept_facets.reserve(clear.size());
                for (unsigned i : clear) {
                    kept_points.push_back(points[i]);
                    kept_facets.push_back(facets[i]);
                }
                points = std::move(kept_points);
                facets = std::move(kept_facets);
            };
            if (!points.empty())
                keep_unobscured_by(p);
            for (size_t q = 0; q < parts.size() && !points.empty(); ++q)
                if (q != p)
                    keep_unobscured_by(q);
            visible += facets.size();
        }
    }
    if (visible == 0)
        return 0;

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Select visible faces for texture displacement"),
                                  UndoRedo::SnapshotType::GizmoAction);

    // Replaces the layer's paint rather than adding to it, on every part including those nothing was
    // visible on: the checkbox means "project onto what I can see", so a second capture from a new
    // angle should not leave the previous angle painted.
    // TriangleSelectorGUI, not the TriangleSelector base: request_update_render_data() is declared on
    // the GUI subclass, so binding to the base here would drop it.
    for (size_t p = 0; p < parts.size(); ++p) {
        TriangleSelectorGUI &selector = *m_triangle_selectors[p];
        selector.reset();
        for (unsigned f : front_facing[p])
            selector.set_facet(int(f), EnforcerBlockerType::ENFORCER);
        selector.request_update_render_data(true);
    }

    update_model_object();
    m_parent.set_as_dirty();
    return int(visible);
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
        m_triangle_selectors[idx]->request_update_render_data(true);
    }
    update_model_object();
    m_parent.set_as_dirty();
}

void GLGizmoTextureDisplacement::subdivide_model()
{
    ModelObject *mo = m_c->selection_info()->model_object();
    // Every model part, like the remesh: this is a plain mesh operation on the object, and a part left
    // coarse is a part no height map can raise relief on.
    const std::vector<ModelVolume *> parts = texture_parts();
    if (mo == nullptr || parts.empty() || m_subdivide_count < 1)
        return; // 0 passes means "no subdivision" - don't take a snapshot for a no-op

    Plater *plater = wxGetApp().plater();
    Plater::TakeSnapshot snapshot(plater, _u8L("Subdivide model for texture displacement"), UndoRedo::SnapshotType::GizmoAction);

    for (ModelVolume *mv : parts) {
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
    }

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

double GLGizmoTextureDisplacement::painted_area_mm2(const ModelVolume &mv)
{
    // Keyed on the paint generation, which every stroke and every layer edit raises, so a panel that
    // asks for this on every frame walks the mesh only when the answer can have changed.
    const std::string key = std::to_string(mv.id().id) + ":" + std::to_string(mv.mesh().facets_count()) + ":" +
                            std::to_string(m_preview_generation->load());
    if (key == m_painted_area_key)
        return m_painted_area_mm2;

    const TriangleMesh &mesh = mv.mesh();
    std::vector<uint8_t> region;
    double               area = 0.;
    if (collect_paint_region(mesh, facets_data_of(mv), region, nullptr)) {
        // In the frame the bake refines in, so a scaled instance is measured at the size it prints at.
        const Transform3d   frame = texture_displacement_bake_frame(texture_displacement_volume_to_world(mv));
        const indexed_triangle_set &its = mesh.its;
        for (size_t t = 0; t < its.indices.size() && t < region.size(); ++t) {
            if ((region[t] & REFINE_PAINTED) == 0)
                continue;
            const stl_triangle_vertex_indices &tri = its.indices[t];
            const Vec3d a = frame * its.vertices[size_t(tri[0])].cast<double>();
            const Vec3d b = frame * its.vertices[size_t(tri[1])].cast<double>();
            const Vec3d c = frame * its.vertices[size_t(tri[2])].cast<double>();
            area += 0.5 * (b - a).cross(c - a).norm();
        }
    }
    m_painted_area_mm2 = area;
    m_painted_area_key = key;
    return area;
}

size_t GLGizmoTextureDisplacement::estimated_refined_triangles(const ModelVolume &mv, float edge_mm)
{
    if (edge_mm <= 0.f)
        return 0;
    return size_t(4.0 * painted_area_mm2(mv) / (double(edge_mm) * double(edge_mm)));
}

const V2Resolution &GLGizmoTextureDisplacement::v2_recommendation(const ModelVolume &mv)
{
    // Rebuilt only when something it depends on changes: the surface area scan is O(triangles) and the
    // panel asks for this every frame.
    std::string key = std::to_string(mv.id().id) + ":" + std::to_string(mv.mesh().facets_count());
    for (const TextureDisplacementLayer &l : mv.texture_displacement_layers)
        key += Slic3r::format("|%1%:%2%:%3%:%4%", l.slot, reinterpret_cast<uintptr_t>(l.image_data.get()), l.tiling_scale, l.depth_mm);
    const Transform3d trafo = texture_displacement_volume_to_world(mv);
    for (int i = 0; i < 12; ++i)
        key += Slic3r::format(",%1%", trafo.matrix()(i / 4, i % 4));
    if (key != m_v2_rec_key) {
        m_v2_rec     = recommend_v2_resolution(mv.mesh().its, mv.texture_displacement_layers, trafo);
        m_v2_rec_key = std::move(key);
    }
    return m_v2_rec;
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
        if (layer.color_enabled && !layer.empty() && height_texture_has_color(layer))
            return true;
    return false;
}

TextureColorSettings GLGizmoTextureDisplacement::color_settings_for(const ModelVolume &mv)
{
    TextureColorSettings out;
    if (!any_layer_colors(mv))
        return out; // nothing is colouring: every colour path stays switched off
    out.palette          = cached_palette();
    out.palette_pure     = make_palette(m_palette_filaments, {});
    out.despeckle_passes = mv.texture_displacement_options.color_despeckle;
    return out;
}

const std::vector<GLGizmoTextureDisplacement::PaletteEntry> &GLGizmoTextureDisplacement::cached_palette()
{
    // Rebuilt only when something it depends on actually changes. The shaded preview rebuilds on every
    // paint stroke and the panel asks every frame, while ranking the mixes and filling the quantizer's
    // lookup cube each take tens of milliseconds - paying that per stroke is the difference between
    // painting that keeps up and painting that stutters.
    //
    // Two levels. The ranking depends only on the filaments and the images, so dragging the count, or a
    // bake creating slots, re-picks from it without ranking again; the palette and its quantizers depend
    // on that pick as well.
    const ModelVolume     *mv        = texture_volume();
    const bool             mixing    = mv != nullptr && mv->texture_displacement_options.color_mix_enabled;
    const int              mix_count = mv != nullptr ? std::max(0, mv->texture_displacement_options.color_mix_count) : 0;
    std::vector<ColorRGBA> filaments = filament_palette();
    // The images themselves rather than their addresses, so a freed and reallocated image can never
    // pass for the old one. Whether one has colour, and whether its colours are flat, follows from it.
    std::vector<std::shared_ptr<std::vector<unsigned char>>> images;
    if (mv != nullptr)
        for (const TextureDisplacementLayer &layer : mv->texture_displacement_layers)
            if (layer.color_enabled && !layer.empty())
                images.push_back(layer.image_data);

    const bool ranking_stale = filaments != m_palette_filaments || images != m_palette_images;
    if (ranking_stale) {
        m_palette_filaments = std::move(filaments);
        m_palette_images    = std::move(images);
        m_mix_ranking.reset();
    }
    if (mixing && !m_mix_ranking)
        m_mix_ranking = rank_mixes(m_palette_filaments, mix_targets(mv->texture_displacement_layers),
                                   int(EnforcerBlockerType::ExtruderMax) - int(m_palette_filaments.size()));

    // Which mixes the project can still print: those it has a fixed slot for, plus as many new ones as
    // there are free slots.
    const PresetBundle            &bundle     = *wxGetApp().preset_bundle;
    const int                      free_slots = std::max(0, int(EnforcerBlockerType::ExtruderMax) - int(bundle.filament_presets.size()));
    const std::vector<std::string> slots      = mixed_slot_signature(bundle.project_config);
    if (ranking_stale || m_palette_cache.empty() || mixing != m_palette_mixing || mix_count != m_palette_mix_count ||
        free_slots != m_palette_free_slots || slots != m_palette_slots) {
        m_palette_mixing     = mixing;
        m_palette_mix_count  = mix_count;
        m_palette_free_slots = free_slots;
        m_palette_slots      = slots;
        std::vector<PaletteEntry> mixes;
        if (mixing && m_mix_ranking)
            mixes = pick_mixes(*m_mix_ranking, mix_count, free_slots, [&bundle](const PaletteEntry &e) {
                return find_fixed_mixed_filament(bundle.project_config, {unsigned(e.a + 1), unsigned(e.b + 1)},
                                                 {e.a_percent(), 100 - e.a_percent()}) >= 0;
            });
        std::vector<PaletteEntry> palette = make_palette(m_palette_filaments, mixes);
        // The previews keep what they were drawn with, so a palette that really changed under them -
        // a filament or a mixed slot edited in the sidebar - has to send them round again.
        m_palette_changed        = m_palette_changed || (!m_palette_cache.empty() && !same_palette(palette, m_palette_cache));
        m_palette_cache          = std::move(palette);
        m_palette_quantizer      = nullptr;
        m_palette_pure_quantizer = nullptr;
    }
    return m_palette_cache;
}

std::pair<ColorQuantizeFn, ColorQuantizeFn> GLGizmoTextureDisplacement::palette_quantizers()
{
    cached_palette();
    if (!m_palette_quantizer) {
        m_palette_quantizer      = make_palette_quantizer(m_palette_cache);
        const bool has_mixes     = m_palette_cache.size() > m_palette_filaments.size();
        m_palette_pure_quantizer = has_mixes ? make_palette_quantizer(make_palette(m_palette_filaments, {})) : m_palette_quantizer;
    }
    return { m_palette_quantizer, m_palette_pure_quantizer };
}

std::vector<ColorRGBA> GLGizmoTextureDisplacement::filament_palette()
{
    std::vector<ColorRGBA> all = wxGetApp().plater()->get_extruders_colors();

    // Physical filaments only. A mix bakes into a mixed filament slot of its own, and those slots are
    // extruders too - mixing them again would hand a blend components naming a virtual slot, where it
    // can only name physical ones ("Mixed filament has invalid or mismatched components"). Mixed slots
    // are kept after the physical ones, so filament i here is extruder i.
    const auto *is_mixed = wxGetApp().preset_bundle->project_config.option<ConfigOptionBools>("filament_is_mixed");
    std::vector<ColorRGBA> palette;
    palette.reserve(all.size());
    for (size_t i = 0; i < all.size(); ++i)
        if (is_mixed == nullptr || i >= is_mixed->values.size() || !is_mixed->values[i])
            palette.push_back(all[i]);

    // A paint mask can only name so many states, and every mix spends one beside these.
    if (palette.size() > size_t(EnforcerBlockerType::ExtruderMax))
        palette.resize(size_t(EnforcerBlockerType::ExtruderMax));
    return palette;
}

std::vector<GLGizmoTextureDisplacement::MixTarget> GLGizmoTextureDisplacement::mix_targets(
    const std::vector<TextureDisplacementLayer> &layers)
{
    // Each layer's bins, weighted by their share of that layer's pixels.
    std::vector<std::vector<MixTarget>> per_layer;
    for (const TextureDisplacementLayer &layer : layers) {
        if (!layer.color_enabled || layer.empty() || analyze_texture_detail(layer).flat_colors)
            continue;
        TextureDisplacementLayer raw = layer;
        raw.smoothing                = 0.f;
        const DecodedHeightTexture tex = decode_height_texture(raw);
        if (!tex.has_color())
            continue;

        // 16 levels per channel, each bin keeping the mean of the colours that fell in it: a coarse grid
        // to gather on, without snapping every colour to a bin corner.
        struct Bin
        {
            double   r = 0., g = 0., b = 0.;
            uint32_t n = 0;
        };
        std::vector<Bin> bins(size_t(16 * 16 * 16));
        // Past the budget, one pixel from each run of `stride`, at an offset jittered by a fixed-seed
        // generator. A fixed step would sample a lattice that a striped texture can line up with, so
        // that only one of its colours is ever seen; jittered, stripes of any period or orientation are
        // sampled in proportion, and the same image still always gives the same targets. The offset
        // comes from the generator's high bits: a power-of-two LCG's low bits repeat every 2, 4, 8...
        // steps, and the stride is a power of two for exactly the images large enough to need this.
        const size_t npx     = size_t(tex.width) * size_t(tex.height);
        const size_t stride  = std::max<size_t>(1, npx / MIX_TARGET_SAMPLES);
        uint64_t     state   = 0x9E3779B97F4A7C15ull;
        size_t       sampled = 0;
        for (size_t start = 0; start < npx; start += stride) {
            state          = state * 6364136223846793005ull + 1442695040888963407ull;
            const size_t i = start + size_t((uint64_t(uint32_t(state >> 32)) * uint64_t(stride)) >> 32);
            if (i >= npx)
                break;
            const uint8_t *px  = &tex.rgb[i * 3];
            Bin           &bin = bins[size_t(px[0] >> 4) * 256 + size_t(px[1] >> 4) * 16 + size_t(px[2] >> 4)];
            bin.r += px[0];
            bin.g += px[1];
            bin.b += px[2];
            ++bin.n;
            ++sampled;
        }
        std::vector<MixTarget> targets;
        for (const Bin &bin : bins)
            if (bin.n > 0) {
                const double inv = 1. / (255. * double(bin.n));
                targets.push_back({ srgb_to_lab(Vec3f(float(bin.r * inv), float(bin.g * inv), float(bin.b * inv))),
                                    float(double(bin.n) / double(sampled)) });
            }
        per_layer.push_back(std::move(targets));
    }

    // The layers weigh the same and together 1, settled before the pruning below: what rank_mixes() sums
    // over the kept bins is then a mean over every pixel of the colouring layers, with the pixels of a
    // dropped bin counted as no better off.
    std::vector<MixTarget> out;
    for (std::vector<MixTarget> &targets : per_layer)
        for (MixTarget &t : targets) {
            t.weight /= float(per_layer.size());
            out.push_back(t);
        }
    if (out.size() > MIX_TARGET_BINS) {
        std::partial_sort(out.begin(), out.begin() + MIX_TARGET_BINS, out.end(),
                          [](const MixTarget &l, const MixTarget &r) { return l.weight > r.weight; });
        out.resize(MIX_TARGET_BINS);
    }
    return out;
}

std::vector<GLGizmoTextureDisplacement::PaletteEntry> GLGizmoTextureDisplacement::rank_mixes(
    const std::vector<ColorRGBA> &filaments, const std::vector<MixTarget> &targets, int limit)
{
    const int n = int(filaments.size());
    if (n < 2 || targets.empty() || limit <= 0)
        return {};

    // Every pair at every short-cycle ratio, coloured as its slot will be.
    std::vector<std::string> hex(filaments.size());
    for (int i = 0; i < n; ++i)
        hex[size_t(i)] = encode_color(filaments[size_t(i)]);
    std::vector<PaletteEntry> candidates;
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            for (int den = 2; den <= 6; ++den)
                for (int num = 1; num < den; ++num) {
                    if (std::gcd(num, den) != 1)
                        continue; // 2/4 is 1/2, already there
                    PaletteEntry e{ Vec3f::Zero(), i, j, num, den };
                    ColorRGB     blended;
                    if (!decode_color(blend_color_multi({ hex[size_t(i)], hex[size_t(j)] },
                                                        { e.a_percent(), 100 - e.a_percent() }),
                                      blended))
                        continue;
                    e.rgb = Vec3f(blended.r(), blended.g(), blended.b());
                    candidates.push_back(e);
                }

    // How far each target is from the nearest single filament, and from every candidate.
    const size_t       nt = targets.size(), nc = candidates.size();
    std::vector<Vec3f> filament_lab(size_t(n), Vec3f::Zero());
    for (int i = 0; i < n; ++i)
        filament_lab[size_t(i)] = srgb_to_lab(Vec3f(filaments[size_t(i)].r(), filaments[size_t(i)].g(), filaments[size_t(i)].b()));
    const auto de = [](const Vec3f &l, const Vec3f &r) { return DeltaE00(l.x(), l.y(), l.z(), r.x(), r.y(), r.z()); };
    std::vector<float> pure_d(nt, std::numeric_limits<float>::max());
    for (size_t t = 0; t < nt; ++t)
        for (const Vec3f &lab : filament_lab)
            pure_d[t] = std::min(pure_d[t], de(targets[t].lab, lab));
    std::vector<float> dist(nc * nt);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, nc), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t c = range.begin(); c < range.end(); ++c) {
            const Vec3f lab = srgb_to_lab(candidates[c].rgb);
            for (size_t t = 0; t < nt; ++t)
                dist[c * nt + t] = de(targets[t].lab, lab);
        }
    });

    // Greedy: each round takes the candidate that lowers the weighted error the most. A candidate only
    // counts where it beats the single filament by PREFER_PURE_DE, since everywhere else the quantizer
    // picks the filament anyway.
    std::vector<float>        current = pure_d;
    std::vector<char>         taken(nc, 0);
    std::vector<PaletteEntry> out;
    const auto                counts = [&](size_t c, size_t t) {
        const float d = dist[c * nt + t];
        return d < current[t] && d <= pure_d[t] - PREFER_PURE_DE;
    };
    while (int(out.size()) < limit) {
        size_t best      = nc;
        double best_gain = 0.;
        for (size_t c = 0; c < nc; ++c) {
            if (taken[c])
                continue;
            double gain = 0.;
            for (size_t t = 0; t < nt; ++t)
                if (counts(c, t))
                    gain += double(targets[t].weight) * double(current[t] - dist[c * nt + t]);
            if (gain > best_gain) {
                best_gain = gain;
                best      = c;
            }
        }
        if (best == nc || best_gain < double(MIN_MIX_GAIN))
            break;
        taken[best] = 1;
        out.push_back(candidates[best]);
        for (size_t t = 0; t < nt; ++t)
            if (counts(best, t))
                current[t] = dist[best * nt + t];
    }
    return out;
}

std::vector<GLGizmoTextureDisplacement::PaletteEntry> GLGizmoTextureDisplacement::pick_mixes(
    const std::vector<PaletteEntry> &ranking, int count, int free_slots, const std::function<bool(const PaletteEntry &)> &reusable)
{
    std::vector<PaletteEntry> out;
    for (const PaletteEntry &e : ranking) {
        if (int(out.size()) >= count)
            break;
        // Out of free slots, a later mix that already has one still fits.
        if (reusable && reusable(e)) {
            out.push_back(e);
        } else if (free_slots > 0) {
            out.push_back(e);
            --free_slots;
        }
    }
    return out;
}

std::vector<GLGizmoTextureDisplacement::PaletteEntry> GLGizmoTextureDisplacement::make_palette(
    const std::vector<ColorRGBA> &filaments, const std::vector<PaletteEntry> &mixes)
{
    std::vector<PaletteEntry> out;
    out.reserve(filaments.size() + mixes.size());
    for (int i = 0; i < int(filaments.size()); ++i)
        out.push_back({ Vec3f(filaments[size_t(i)].r(), filaments[size_t(i)].g(), filaments[size_t(i)].b()), i, i, 1, 1 });
    out.insert(out.end(), mixes.begin(), mixes.end());
    return out;
}

std::vector<int> GLGizmoTextureDisplacement::palette_filaments(const std::vector<PaletteEntry>                &palette,
                                                               const std::vector<uint8_t>                     &triangle_color,
                                                               const std::function<int(const PaletteEntry &)> &slot_for_mix)
{
    std::vector<char> used(palette.size(), 0);
    for (const uint8_t v : triangle_color)
        if (v > 0 && size_t(v) <= palette.size())
            used[size_t(v) - 1] = 1;

    std::vector<int> out(palette.size(), -1);
    for (size_t i = 0; i < palette.size(); ++i) {
        const PaletteEntry &e = palette[i];
        if (!e.is_mix()) {
            out[i] = e.a;
        } else if (used[i]) {
            const int slot = slot_for_mix ? slot_for_mix(e) : -1;
            // No room for another slot: the component that dominates the blend is the nearest the print
            // can come.
            out[i] = slot >= 0 ? slot : (e.num * 2 >= e.den ? e.a : e.b);
        }
    }
    return out;
}

ColorQuantizeFn GLGizmoTextureDisplacement::make_palette_quantizer(const std::vector<PaletteEntry> &palette)
{
    if (palette.empty())
        return nullptr;

    // Lab once per entry, not once per lookup.
    struct Lab { float l, a, b; };
    std::vector<Lab> palette_lab(palette.size());
    for (size_t i = 0; i < palette.size(); ++i) {
        const Vec3f lab = srgb_to_lab(palette[i].rgb);
        palette_lab[i]  = { lab.x(), lab.y(), lab.z() };
    }

    constexpr int E   = PALETTE_LUT_EDGE;
    auto          lut = std::make_shared<std::vector<uint8_t>>(size_t(E) * E * E, 0);
    tbb::parallel_for(tbb::blocked_range<int>(0, E), [&](const tbb::blocked_range<int> &range) {
        for (int r = range.begin(); r < range.end(); ++r)
            for (int g = 0; g < E; ++g)
                for (int b = 0; b < E; ++b) {
                    // Cell centre, so the quantization error is symmetric across the cell.
                    float l0, a0, b0;
                    const Vec3f lab0 = srgb_to_lab(Vec3f((r + 0.5f) / E, (g + 0.5f) / E, (b + 0.5f) / E));
                    l0 = lab0.x(); a0 = lab0.y(); b0 = lab0.z();
                    int   best  = 0, best_pure = -1;
                    float best_d = std::numeric_limits<float>::max(), best_pure_d = best_d;
                    for (size_t i = 0; i < palette_lab.size(); ++i) {
                        const float d = DeltaE00(l0, a0, b0, palette_lab[i].l, palette_lab[i].a, palette_lab[i].b);
                        if (d < best_d) {
                            best_d = d;
                            best   = int(i);
                        }
                        if (!palette[i].is_mix() && d < best_pure_d) {
                            best_pure_d = d;
                            best_pure   = int(i);
                        }
                    }
                    // A mix only where it clearly beats the nearest single filament, see PREFER_PURE_DE.
                    if (best_pure >= 0 && palette[size_t(best)].is_mix() && best_pure_d - best_d < PREFER_PURE_DE)
                        best = best_pure;
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
    const std::vector<TextureDisplacementLayer> &layers, const std::vector<Vec2f> &model_uv,
    const Transform3d &texture_frame, const TextureDisplacementPrepareParams &params,
    const TextureColorSettings &color_settings, const DisplacementProgressFn &progress,
    BakeStageRecorder *debug)
{
    TextureDisplacementPrepareResult out;
    const auto                       report = [&progress](int pct) { return !progress || progress(pct); };

    // The samplers are built over a copy of the current mesh moved into `texture_frame`, and queried
    // with points and normals mapped across, so the stages below go on measuring the mesh itself.
    const bool           framed = !texture_frame.matrix().isApprox(Transform3d::Identity().matrix());
    const Matrix3d       frame_normal = texture_frame.linear().inverse().transpose();
    indexed_triangle_set framed_mesh;
    const auto           mesh_to_sample = [&](const indexed_triangle_set &its) -> const indexed_triangle_set & {
        if (!framed)
            return its;
        framed_mesh = its;
        for (Vec3f &v : framed_mesh.vertices)
            v = (texture_frame * v.cast<double>()).cast<float>();
        return framed_mesh;
    };
    const auto sample_in_frame = [&](auto sampler) -> decltype(sampler) {
        if (!framed || !sampler)
            return sampler;
        return [sampler, frame = texture_frame, frame_normal](const Vec3f &pos, const Vec3f &normal) {
            return sampler((frame * pos.cast<double>()).cast<float>(),
                           (frame_normal * normal.cast<double>()).normalized().cast<float>());
        };
    };

    // Stage capture for the debug view. Timed from the end of the previous stage, so the capture -
    // a copy plus an edge scan - never lands inside the measurement it reports.
    auto       stage_clock = std::chrono::steady_clock::now();
    const auto capture     = [&](const char *name, const indexed_triangle_set &m,
                             const std::string &detail = {}) {
        if (debug == nullptr)
            return;
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stage_clock).count();
        debug->capture(name, m.vertices, m.indices, ms, detail);
        stage_clock = std::chrono::steady_clock::now();
    };
    capture("input", base, "as handed to prepare");

    TriangleMesh                  mesh(base);
    TextureDisplacementFacetsData current = masks;
    bool                          changed = false;
    bool                          had_paint = false;
    for (const TriangleSelector::TriangleSplittingData &m : masks)
        had_paint = had_paint || TriangleSelector::has_facets(m, EnforcerBlockerType::ENFORCER);
    // The model's own UV channel for whatever `mesh` currently is. Every stage below keeps the input's
    // surface - it evens out, refines or cuts the triangles lying on it - so each corner reads its uv
    // straight off `base`'s channel at its own position instead of being carried stage by stage, which
    // would compound each stage's approximation. Untouched while nothing has replaced the mesh yet.
    const auto uv_now = [&]() {
        return (model_uv.empty() || !changed) ? model_uv : transfer_uv_channel(base, model_uv, mesh.its);
    };
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
            capture("remesh", mesh.its,
                    "target " + std::to_string(params.remesh_edge_mm) + " mm" +
                        (had_paint && !any ? ", paint lost" : ""));
            if (had_paint && !any) {
                out.paint_lost = true;
                return out;
            }
        } else if (debug != nullptr) {
            capture("remesh", mesh.its, "skipped: remeshing changed nothing");
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
            const std::vector<Vec2f> uv_stage = uv_now();
            const std::vector<Vec2f> *uv_ptr  = uv_stage.empty() ? nullptr : &uv_stage;
            const indexed_triangle_set &sampled = mesh_to_sample(mesh.its);
            HeightFieldSampler sampler;
            if (params.subdiv_feature)
                sampler = sample_in_frame(make_combined_displacement_sampler(sampled, layers, current, uv_ptr));
            // Colour boundaries need triangles of their own - the chord test cannot see them, since
            // the height field is perfectly smooth across a change of filament.
            ColorFieldSampler color;
            // A flat-colour layer is matched against single filaments only, as the bake does, so its
            // boundaries are refined where the bake will actually change filament.
            if (params.subdiv_color_edge_mm > 0.f && !color_settings.empty())
                color = sample_in_frame(make_combined_color_sampler(sampled, layers, current,
                                                                    make_palette_quantizer(color_settings.palette),
                                                                    make_palette_quantizer(color_settings.palette_pure),
                                                                    uv_ptr));
            // The refinement follows perceived colour: a mix is one colour here, however the slicer
            // interleaves its filaments layer by layer.
            // "Min edge" is a feature-mode control (it is the floor the curvature test refines down
            // to); in plain adaptive mode the target edge length is the only criterion, so the floor
            // must not be allowed to silently override a target the user set below it.
            const float tol   = params.subdiv_feature ? params.subdiv_detail_mm : 0.f;
            const float floor = params.subdiv_feature ? params.subdiv_min_edge_mm : 0.f;
            // Step textures (a grid, a knurl: two levels with sharp edges between them) are cut into
            // walls after refining rather than refined into ramps, see 3. below. The verdict is taken
            // here, on the coarse mesh, because the refinement has to know: in step mode it leaves
            // the stepped triangles to the cutter, which is only right if the cutter then cuts them.
            // The step width and the seam gap both span two texels of the finest layer: the bilinear
            // blend across a step is a texel wide, so a seam copy a texel from the step samples a
            // pure side.
            float texel_mm = std::numeric_limits<float>::max();
            if (params.cut_steps && sampler)
                for (const TextureDisplacementLayer &layer : layers) {
                    if (layer.empty())
                        continue;
                    const DecodedHeightTexture &tex = decode_height_texture(layer);
                    if (tex.width > 0 && layer.tiling_scale > 0.f)
                        texel_mm = std::min(texel_mm, layer.tiling_scale / float(tex.width));
                }
            const bool cut_steps = texel_mm < std::numeric_limits<float>::max() &&
                                   texture_has_steps_to_cut(mesh.its, region, sampler, 2.f * texel_mm, 2.f * texel_mm);
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
                                        color, params.subdiv_color_edge_mm, cut_steps);
            if (aborted)
                return {};
            if (refined.indices.size() != mesh.its.indices.size()) {
                mesh    = TriangleMesh(std::move(refined));
                current = masks_after_subdivision(mesh, source, paint);
                changed = true;
                capture("adaptive subdivide", mesh.its,
                        params.subdiv_feature ? "feature adaptive" : "length only");
            } else if (debug != nullptr) {
                capture("adaptive subdivide", mesh.its, "skipped: already meets the criteria");
            }

            // 3. Cut the painted area along the texture's sharp steps so they bake as walls. The
            //    refinement above left the stepped triangles alone, so this is where a binary
            //    texture's edges get their geometry. Paint is carried the same way as through the
            //    subdivision: every output triangle lies inside the pre-subdivision triangle it
            //    descends from, so the source maps compose.
            if (cut_steps) {
                std::vector<uint8_t> cut_region(mesh.its.indices.size(), 0);
                for (size_t i = 0; i < cut_region.size() && i < source.size(); ++i)
                    cut_region[i] = (region[size_t(source[i])] & REFINE_PAINTED) ? 1 : 0;
                const std::vector<Vec2f> uv_refined = uv_now();
                const HeightFieldSampler refined_sampler =
                    sample_in_frame(make_combined_displacement_sampler(mesh_to_sample(mesh.its), layers, current,
                                                                       uv_refined.empty() ? nullptr : &uv_refined));
                std::vector<int>     cut_source;
                size_t               cuts = 0;
                indexed_triangle_set cut = cut_mesh_at_steps(mesh.its, cut_region, refined_sampler,
                                                             2.f * texel_mm, 2.f * texel_mm, 0.f,
                                                             &cut_source, &cuts);
                if (cuts > 0) {
                    for (int &s : cut_source)
                        s = source[size_t(s)];
                    mesh    = TriangleMesh(std::move(cut));
                    current = masks_after_subdivision(mesh, cut_source, paint);
                    changed = true;
                    capture("step cut", mesh.its, std::to_string(cuts) + " triangles cut");
                } else if (debug != nullptr) {
                    capture("step cut", mesh.its, "skipped: the refined mesh has no steps left to cut");
                }
            } else if (debug != nullptr && params.cut_steps) {
                capture("step cut", mesh.its, "skipped: not a step texture");
            }
        }
    }
    if (!report(100) || !changed)
        return out; // an empty result: nothing to commit, which is not a failure

    out.uv    = uv_now();
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

    if (!any_part_painted()) {
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

    // Five iterations with three relaxation passes each, rather than CGAL's three-and-one. Splitting
    // and collapsing bring edge lengths near the target but leave the vertices where they fell, so it
    // is the relaxation count that decides how even the result looks - and three passes in total was
    // nowhere near enough. Fifteen costs proportionally more, but only on an explicit Remesh.
    indexed_triangle_set remeshed = MeshBoolean::cgal::remesh_isotropic(
        src, double(target_edge_mm), /* iterations */ 5, double(sharp_angle_deg),
        /* relaxation steps */ 3);
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
                       "shared by more than two triangles), or the target edge length may already be met."),
                  PrepareScope::AllParts);
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
            const auto [quantize, quantize_pure] = palette_quantizers();
            color = make_combined_color_sampler(mv->mesh().its, mv->texture_displacement_layers, facets, quantize,
                                                quantize_pure);
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
    if (m_thumbnails[slot] && m_thumbnail_source[slot].lock() == layer.image_data &&
        m_thumbnail_smoothing[slot] == layer.smoothing)
        return m_thumbnails[slot].get();

    // Reuses the already-decoded, already-cached grayscale pixels (see decode_height_texture()'s
    // own cache in TextureDisplacement.cpp) - only the gray-to-RGBA expansion and GPU upload below are
    // new work. Rebuilt when the texture *or the smoothing* changes, so the fast/shaded preview - which
    // samples this GPU texture directly - reflects the current smoothing rather than the raw image.
    std::unique_ptr<GLTexture> texture = upload_height_thumbnail(decode_height_texture(layer));
    if (!texture)
        return nullptr;

    m_thumbnails[slot]          = std::move(texture);
    m_thumbnail_source[slot]    = layer.image_data;
    m_thumbnail_smoothing[slot] = layer.smoothing;
    return m_thumbnails[slot].get();
}

GLTexture *GLGizmoTextureDisplacement::get_layer_color_texture(const TextureDisplacementLayer &layer)
{
    if (layer.empty() || !layer.color_enabled)
        return nullptr;
    if (m_color_tex && m_color_tex_source.lock() == layer.image_data && m_color_tex_smoothing == layer.smoothing)
        return m_color_tex.get();

    std::unique_ptr<GLTexture> texture = upload_color_texture(decode_height_texture(layer), HEIGHT_TEX_MAX_PX);
    if (!texture) {
        m_color_tex.reset();
        m_color_tex_source.reset();
        return nullptr;
    }
    m_color_tex           = std::move(texture);
    m_color_tex_source    = layer.image_data;
    m_color_tex_smoothing = layer.smoothing;
    return m_color_tex.get();
}

GLTexture *GLGizmoTextureDisplacement::get_layer_height_texture(const TextureDisplacementLayer &layer)
{
    if (layer.empty())
        return nullptr;

    // One slot, not one per layer: the preview shader only ever shades the *active* layer, so a single
    // full-resolution upload is enough and the VRAM cost stays at one texture rather than eight.
    if (m_height_tex && m_height_tex_source.lock() == layer.image_data && m_height_tex_smoothing == layer.smoothing)
        return m_height_tex.get();

    std::unique_ptr<GLTexture> texture = upload_height_thumbnail(decode_height_texture(layer), HEIGHT_TEX_MAX_PX);
    if (!texture)
        return nullptr;

    m_height_tex           = std::move(texture);
    m_height_tex_source    = layer.image_data;
    m_height_tex_smoothing = layer.smoothing;
    return m_height_tex.get();
}

} // namespace Slic3r::GUI
