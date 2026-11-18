// One of the files GLGizmoTextureDisplacement is defined across. The class is declared once, in
// GLGizmoTextureDisplacement.hpp; its members are defined in GLGizmoTextureDisplacement.cpp and in the
// siblings named after the part of the gizmo they hold - Chroma, Library, Mesh, Preview, UV. The split
// is along those lines and nothing else: no member moved between parts, and the class did not change.
// Helpers more than one part needs live in GLGizmoTextureDisplacementInternal.hpp.

// Everything the gizmo draws over the model that is not the panel: the baked and the shaded previews,
// the paint, island and wireframe overlays, the UV check mesh, and the background preview job that
// keeps them current.

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
#include "slic3r/GUI/GLModel.hpp"
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

// The most grid cells a generated ChromaStack plate's preview spends on its picture face: about
// 0.18 mm cells on a 100 mm plate, finer than a nozzle draws, and 600 k triangles to upload.
static constexpr size_t CHROMA_FLAT_PREVIEW_CELLS = 300000;

using namespace gizmo_td;

namespace {
// The shaded preview shader's palette arrays. The palette itself stays within the paint mask's
// EnforcerBlockerType::ExtruderMax states, since every entry has to become a filament.
constexpr int PALETTE_MAX_ENTRIES = 64;
} // namespace

void GLGizmoTextureDisplacement::render_preview_mesh()
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr)
        return;

    auto *shader = wxGetApp().get_shader("gouraud_light");
    if (shader == nullptr)
        return;

    const Selection   &selection       = m_parent.get_selection();
    const Transform3d  instance_matrix = mo->instances[selection.get_instance_idx()]->get_transformation().get_matrix();
    const Camera      &camera          = wxGetApp().plater()->get_camera();
    const Transform3d &view_matrix     = camera.get_view_matrix();

    shader->start_using();
    // Each part is drawn through its own matrix: a multibody object's parts sit in different places
    // inside the object, so one matrix for all of them would pile every preview onto the first part.
    const auto draw = [&](const ModelVolume &volume, GLModel &model, const std::vector<PreviewColorRun> &runs) {
        if (!model.is_initialized())
            return;
        const Transform3d trafo_matrix = instance_matrix * volume.get_matrix();
        shader->set_uniform("view_model_matrix", view_matrix * trafo_matrix);
        shader->set_uniform("projection_matrix", camera.get_projection_matrix());
        const Matrix3d view_normal_matrix = view_matrix.matrix().block(0, 0, 3, 3) *
                                            trafo_matrix.matrix().block(0, 0, 3, 3).inverse().transpose();
        shader->set_uniform("view_normal_matrix", view_normal_matrix);
        if (runs.empty()) {
            model.render();
        } else {
            // set_color() writes the uniform the shader reads, so one call per group is all the
            // per-triangle colour this needs.
            for (const PreviewColorRun &run : runs) {
                model.set_color(run.color);
                model.render(run.range, shader);
            }
        }
    };

    if (const ModelVolume *owner = texture_volume(); owner != nullptr)
        draw(*owner, m_preview_glmodel, m_preview_color_runs);
    for (const std::unique_ptr<PartPreview> &preview : m_part_previews)
        if (const ModelVolume *part = preview_part(*preview); part != nullptr)
            draw(*part, preview->glmodel, preview->color_runs);
    shader->stop_using();
}

const ModelVolume *GLGizmoTextureDisplacement::preview_part(const PartPreview &preview) const
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr)
        return nullptr;
    for (const ModelVolume *mv : mo->volumes)
        if (mv->id() == preview.volume_id)
            return mv;
    return nullptr; // the part was removed, or its mesh replaced, since the preview was built
}

float GLGizmoTextureDisplacement::layer_texture_aspect(const TextureDisplacementLayer &layer)
{
    // decode_height_texture() is cached on the image_data allocation, so this is a hash lookup rather
    // than a PNG decode - cheap enough to call per rebuild.
    const DecodedHeightTexture tex = decode_height_texture(layer);
    return (tex.width > 0 && tex.height > 0) ? float(tex.width) / float(tex.height) : 1.f;
}

indexed_triangle_set GLGizmoTextureDisplacement::patch_in_world(const indexed_triangle_set &patch) const
{
    const ModelVolume *mv = texture_volume();
    if (mv == nullptr)
        return patch;
    // The bake's own frame, so the projection this feeds agrees with what bakes.
    const Transform3d to_world = texture_displacement_bake_frame(texture_displacement_volume_to_world(*mv));
    if (to_world.matrix().isApprox(Transform3d::Identity().matrix()))
        return patch;

    indexed_triangle_set world = patch;
    for (Vec3f &v : world.vertices)
        v = (to_world * v.cast<double>()).cast<float>();
    return world;
}

std::vector<Vec2f> GLGizmoTextureDisplacement::model_uv_corners(const indexed_triangle_set     &local_patch,
                                                                const TextureDisplacementLayer &layer,
                                                                const std::vector<int>         *patch_source) const
{
    const ModelVolume *mv = texture_volume();
    if (layer.projection_method != TextureProjectionMethod::ModelUV || patch_source == nullptr || mv == nullptr ||
        !mv->has_texture_uv())
        return {};
    // The channel is indexed against the volume's *local* mesh, and a uv is a coordinate in the
    // texture rather than on the model, so unlike every projected method this never goes through
    // patch_in_world() - there is no millimetre measurement here for a world scale to change.
    std::vector<Vec2f> uv = compute_model_corner_uvs(mv->mesh().its, mv->texture_uv, local_patch, *patch_source);
    for (Vec2f &p : uv)
        // Already a uv: no tiling scale, no aspect correction - see apply_uv_transform().
        p = apply_uv_transform(p, layer, 1.f, /* normalized_uv */ true);
    return uv;
}

std::vector<Vec2f> GLGizmoTextureDisplacement::compute_layer_vertex_uvs(const indexed_triangle_set     &local_patch,
                                                                       const TextureDisplacementLayer &layer,
                                                                       const std::vector<int>         *patch_source) const
{
    if (layer.projection_method == TextureProjectionMethod::ModelUV)
        // Collapsed to one per vertex the same way the unwrap is - a vertex has one position, so the
        // displacement can only read one height for it.
        return collapse_corner_uvs(local_patch, model_uv_corners(local_patch, layer, patch_source));

    // The bake maps the texture in world millimetres, so "Tile size (mm)" means the same thing on a
    // scaled instance as it does on an untouched one (see build_texture_displacement()). Everything
    // that has to agree with the bake - the fast preview's uvs, the checker/distortion overlays -
    // therefore has to project from the same world positions, not from the volume's own.
    const indexed_triangle_set patch  = patch_in_world(local_patch);
    const float                aspect = layer_texture_aspect(layer);
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
            // Negated up, as sample_layer_height()'s affine branch has it: v runs down the image.
            const Vec2f planar(patch.vertices[vi].dot(layer.view_project_right),
                               -patch.vertices[vi].dot(layer.view_project_up));
            uv[vi] = apply_uv_transform(planar, layer, aspect);
        }
        return uv;
    }
    return {}; // Triplanar / Cylindrical / Spherical: the shader projects on its own
}

int GLGizmoTextureDisplacement::layer_projection_frame(const indexed_triangle_set     &local_patch,
                                                       const TextureDisplacementLayer &layer,
                                                       Vec3f &center, Vec3f &axis) const
{
    center = Vec3f::Zero();
    axis   = Vec3f::UnitZ();
    const ModelVolume *mv = texture_volume();
    if (mv == nullptr || (layer.projection_method != TextureProjectionMethod::Cylindrical &&
                          layer.projection_method != TextureProjectionMethod::Spherical))
        return 0;
    // The bake averages the *whole mesh's* vertex normals over the patch's corners, so this has to as
    // well: a patch-only average would sometimes quantize to a different world axis and wrap the
    // texture the other way round. Both meshes go through patch_in_world() first, which is the frame
    // the shaders' tex_pos lives in.
    Vec3f average_normal;
    texture_displacement_patch_frame(patch_in_world(local_patch),
                                     texture_displacement_vertex_normals(patch_in_world(mv->mesh().its)),
                                     center, axis, average_normal);
    return layer.projection_method == TextureProjectionMethod::Cylindrical ? 1 : 2;
}

std::vector<Vec2f> GLGizmoTextureDisplacement::compute_layer_corner_uvs(const indexed_triangle_set     &local_patch,
                                                                        const TextureDisplacementLayer &layer,
                                                                        const std::vector<int>         *patch_source) const
{
    if (layer.projection_method == TextureProjectionMethod::ModelUV)
        return model_uv_corners(local_patch, layer, patch_source);
    if (layer.projection_method == TextureProjectionMethod::LSCM) {
        const indexed_triangle_set patch  = patch_in_world(local_patch);
        const float                aspect = layer_texture_aspect(layer);
        std::vector<Vec2f>         uv     = compute_lscm_corner_uvs(patch, layer);
        for (Vec2f &p : uv)
            p = apply_uv_transform(p, layer, aspect);
        return uv;
    }
    // Single-valued per point: fan the per-vertex result out over the corners.
    const std::vector<Vec2f> per_vertex = compute_layer_vertex_uvs(local_patch, layer, patch_source);
    if (per_vertex.size() != local_patch.vertices.size())
        return {};
    std::vector<Vec2f> corner(local_patch.indices.size() * 3);
    for (size_t f = 0; f < local_patch.indices.size(); ++f)
        for (int k = 0; k < 3; ++k)
            corner[f * 3 + size_t(k)] = per_vertex[size_t(local_patch.indices[f][k])];
    return corner;
}

void GLGizmoTextureDisplacement::rebuild_shaded_preview_mesh()
{
    m_shaded_preview_glmodel.reset();

    const ModelVolume *mv = texture_volume();
    if (mv == nullptr || m_triangle_selectors.empty())
        return;

    // Uses the *live* selector (not the flushed model facet data), so this reflects an in-progress
    // stroke immediately rather than only once it ends - the point of this preview mode is to be
    // the fast, no-CPU-meshing one.
    // The source map comes along because a ModelUV layer's uvs are read per base-mesh triangle; every
    // other mapping ignores it.
    std::vector<int>           patch_source;
    const indexed_triangle_set patch =
        m_triangle_selectors[0]->get_facets_strict(EnforcerBlockerType::ENFORCER, &patch_source);
    if (patch.indices.empty())
        return;

    // No "patch.vertices.size() == mesh vertex count" check here, and that is the point: a *brush*
    // stroke splits triangles, so the selector appends split vertices and the patch array is longer
    // than the mesh's. An earlier version bailed out on that as "shouldn't happen", which meant the
    // shaded model was never built while brushing and render_painter_gizmo() silently fell back to the
    // Normal (true-displacement) preview - Fast looked broken for brush and fine for Face/Connected
    // area, because only the brush splits. Everything below indexes the patch's own vertex array, so
    // the extra vertices are simply carried through.

    // Unpainted triangles, so the surrounding surface still renders (the render path hides the real
    // model in shaded mode). get_facets_strict() returns the same vertex array whatever state is asked.
    const indexed_triangle_set rest = m_triangle_selectors[0]->get_facets_strict(EnforcerBlockerType::NONE);

    // For LSCM we hand the shader the finished per-vertex texture uv (island placement + tiling/
    // rotation/offset already folded in, exactly what the bake samples), because it cannot be
    // reconstructed in the fragment shader the way a triplanar projection can. This is also what
    // makes the fast preview follow the UV editor: the uvs move when an island is dragged, so this
    // mesh rebuilds (on drag end) with them. The other projections keep projecting in-shader.
    // Per *corner*, not per vertex: the mesh below is flat (unshared) anyway, so each triangle can
    // carry its own chart's UVs - see compute_layer_corner_uvs().
    const TextureDisplacementLayer *active = active_layer();
    std::vector<Vec2f> corner_uv = active != nullptr ? compute_layer_corner_uvs(patch, *active, &patch_source) :
                                                       std::vector<Vec2f>{};
    m_shaded_preview_uses_vertex_uv = corner_uv.size() == patch.indices.size() * 3;
    if (!m_shaded_preview_uses_vertex_uv)
        corner_uv.clear();
    m_shaded_projection_mode = (active != nullptr && !m_shaded_preview_uses_vertex_uv) ?
                                 layer_projection_frame(patch, *active, m_shaded_patch_center, m_shaded_patch_axis) : 0;

    // Which triangles the in-flight UV drag moves. Computed here, against the very patch this mesh is
    // built from, so the flags can never be indexed by a different triangle count than they were sized
    // for (the drag starts from the flushed facet data, a brush stroke changes the live selector).
    compute_shaded_active_faces(m_shaded_active_chart >= 0 ? m_island_move_set : std::vector<int>{}, patch.indices.size());

    // Colour is quantized per *fragment* in the shader now (see the .fs), so this mesh carries no
    // colour of its own - the palette and the colour texture are uniforms, and every pixel matches the
    // image rather than the facet it landed on. What the *bake* will produce, at facet resolution, is
    // what the Normal view shows.
    m_shaded_preview_palette = (active != nullptr && active->color_enabled) ? cached_palette()
                                                                         : std::vector<PaletteEntry>{};

    GLModel::Geometry init_data;
    // P3N3T2: normal.x carries the paint weight, tex_coord the precomputed uv (see the vertex shader).
    init_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3T2 };

    // Per-triangle weighting via a *flat* (unshared-vertex) mesh: every corner of a painted triangle
    // gets weight 1, every corner of an unpainted one weight 0. This is what a coarse mesh needs - one
    // painted face of a raw cube has no strictly-interior vertex (all 8 are shared), so per-vertex
    // weighting would either bleed onto the neighbours (boundary weight 1) or vanish outright (boundary
    // weight 0, which is what made a single face show nothing). Duplicating vertices costs no shading
    // quality here because the preview shader takes its surface normal from screen-space derivatives of
    // position (dFdx/dFdy), not from a per-vertex normal. normal.y flags the UV-editor island being
    // dragged so the shader can move just that island via the island_delta uniform.
    const size_t   tri_total = patch.indices.size() + rest.indices.size();
    init_data.reserve_vertices(tri_total * 3);
    init_data.reserve_indices(tri_total * 3);
    unsigned vcount = 0;
    const auto emit_triangles = [&](const indexed_triangle_set &its, float weight, bool painted) {
        for (size_t f = 0; f < its.indices.size(); ++f) {
            const stl_triangle_vertex_indices &tri = its.indices[f];
            // One value for the whole triangle: island_active is an interpolated varying, so the three
            // corners have to agree or the shader moves part of a triangle and not the rest.
            const float act = (painted && f < m_shaded_active_face.size() && m_shaded_active_face[f]) ? 1.f : 0.f;
            for (int i = 0; i < 3; ++i) {
                const int   idx = tri[i];
                const Vec2f uv  = (painted && m_shaded_preview_uses_vertex_uv) ? corner_uv[f * 3 + size_t(i)]
                                                                             : Vec2f::Zero();
                init_data.add_vertex(its.vertices[size_t(idx)], Vec3f(weight, act, 0.f), uv);
            }
            init_data.add_triangle(vcount, vcount + 1, vcount + 2);
            vcount += 3;
        }
    };
    emit_triangles(patch, 1.f, true); // painted -> shaded as relief, and coloured by the shader
    // Untouched surface: flat, so it still shows but carries no relief - and uncoloured, which is what the
    // bake leaves it as (EnforcerBlockerType::NONE, i.e. the volume's own filament).
    emit_triangles(rest, 0.f, false);

    m_shaded_preview_glmodel.init_from(std::move(init_data));
    // GLModel::render() unconditionally re-sets the shader's "uniform_color" from this internal
    // color field right before drawing (see GLModel.cpp) - setting the uniform manually in
    // render_shaded_preview_mesh() would just get overwritten by it, so it must be set here instead.
    // GLModel::Geometry defaults to BLACK, which is exactly what showed up before this was added.
    m_shaded_preview_glmodel.set_color(GLVolume::NEUTRAL_COLOR);

    // The mesh now reflects the islands' current placement, so any live drag delta is measured from
    // here: reset it to identity and record the dragged island's baked transform.
    m_shaded_island_delta = Eigen::Matrix<float, 2, 3>::Identity();
    const TextureDisplacementLayer *al = active_layer();
    if (m_shaded_active_chart >= 0 && al != nullptr) {
        const std::vector<Eigen::Matrix<float, 2, 3>> xf = uv_editor_island_transforms(*al);
        m_shaded_baked_active_xf = (size_t(m_shaded_active_chart) < xf.size()) ? xf[size_t(m_shaded_active_chart)]
                                                                           : Eigen::Matrix<float, 2, 3>::Identity();
    } else {
        m_shaded_baked_active_xf = Eigen::Matrix<float, 2, 3>::Identity();
    }
}

void GLGizmoTextureDisplacement::compute_shaded_active_faces(const std::vector<int> &charts, size_t patch_face_count)
{
    m_shaded_active_face.clear();
    if (charts.empty() || patch_face_count == 0)
        return;
    const PatchUnwrap &u = m_uv_editor_unwrap;
    if (u.source_face.size() != u.indices.size())
        return;
    m_shaded_active_face.assign(patch_face_count, 0);
    // Flag every triangle of every chart being moved. For a group/multi move that is more than one
    // chart, but since such a move is a pure translation the shader applies the same delta to them all
    // (see on_island_edited) - exactly the "joined islands move together" behaviour.
    for (size_t t = 0; t < u.indices.size(); ++t) {
        const int f  = u.source_face[t];
        const int v0 = u.indices[t][0]; // a triangle lies in one chart, so any corner names it
        if (f < 0 || size_t(f) >= m_shaded_active_face.size() || v0 < 0 || size_t(v0) >= u.vertex_chart.size())
            continue;
        if (std::find(charts.begin(), charts.end(), u.vertex_chart[size_t(v0)]) != charts.end())
            m_shaded_active_face[size_t(f)] = 1;
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

void GLGizmoTextureDisplacement::render_shaded_preview_mesh()
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    const ModelVolume  *mv = texture_volume();
    if (mo == nullptr || mv == nullptr || !m_shaded_preview_glmodel.is_initialized())
        return;

    const TextureDisplacementLayer *layer = active_layer();
    if (layer == nullptr || layer->empty())
        return;

    // Full-resolution height upload (smoothing-aware), whose grayscale value lives in the R channel
    // exactly as the shader samples it. Deliberately *not* the layer-list panel's thumbnail: that one
    // is box-filtered down to 128 px for a ~48 px row, and feeding it to the shader cost the preview
    // three quarters of the height map's detail - and, since height_tex_texel is derived from it, also
    // flattened the shading gradient and made the parallax march skip itself at angles where it should
    // run. Cached per image_data and smoothing, so no PNG is decoded per frame.
    GLTexture *tex = get_layer_height_texture(*layer);
    if (tex == nullptr || tex->get_width() <= 0 || tex->get_height() <= 0)
        return;

    GLShaderProgram *shader = wxGetApp().get_shader("texture_displacement_shaded");
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
    // The shader works on world-space normals (it projects the texture in world millimetres, as the
    // bake does), so the normal matrix is the view rotation alone - no model part.
    const Matrix3d view_normal_matrix = camera.get_view_matrix().matrix().block(0, 0, 3, 3);
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
    // The texture frame is world space anchored at the volume's origin (texture_displacement_bake_frame()):
    // the shader subtracts the anchor from the world position, and the eye is handed over the same way.
    const Vec3d tex_anchor = trafo_matrix.translation();
    shader->set_uniform("tex_anchor", Vec3f(tex_anchor.cast<float>()));
    shader->set_uniform("eye_model_pos", Vec3f((camera.get_position() - tex_anchor).cast<float>()));
    // When set, the shader samples at the per-vertex uv baked into the mesh (LSCM) rather than
    // projecting; see rebuild_shaded_preview_mesh().
    shader->set_uniform("use_vertex_uv", m_shaded_preview_uses_vertex_uv);
    // Cylindrical/Spherical wrap around the painted patch's own centre, which no fragment can derive:
    // captured with the mesh (see rebuild_shaded_preview_mesh()) and handed over here. 0 is the planar
    // projection every other in-shader path uses.
    shader->set_uniform("projection_mode", m_shaded_projection_mode);
    shader->set_uniform("patch_center", m_shaded_patch_center);
    shader->set_uniform("patch_axis", m_shaded_patch_axis);

    // The printable palette, in RGB for display and in Lab for the match. Uploaded rather than matched
    // on the CPU because the quantization is per fragment here. Count 0 means "no layer is colouring",
    // and the shader keeps the model's own colour for every fragment.
    const GLTexture *color_tex = get_layer_color_texture(*layer);
    const int        palette_count =
        (color_tex != nullptr) ? int(std::min(m_shaded_preview_palette.size(), size_t(PALETTE_MAX_ENTRIES))) : 0;
    shader->set_uniform("palette_count", palette_count);
    shader->set_uniform("has_color_tex", color_tex != nullptr);
    // A flat-colour image is matched against single filaments only, as the bake does.
    shader->set_uniform("pure_only", color_tex != nullptr && analyze_texture_detail(*layer).flat_colors);
    shader->set_uniform("prefer_pure_de", PREFER_PURE_DE);
    for (int i = 0; i < palette_count; ++i) {
        const PaletteEntry &e   = m_shaded_preview_palette[size_t(i)];
        const std::string   idx = "[" + std::to_string(i) + "]";
        // An entry's colour is what it prints as: its filament's, or for a mix its mixed filament slot's.
        shader->set_uniform(("palette_rgb" + idx).c_str(), e.rgb);
        shader->set_uniform(("palette_lab" + idx).c_str(), srgb_to_lab(e.rgb));
        // Only so the shader can tell a mix (a != b) from a single filament.
        shader->set_uniform(("palette_a" + idx).c_str(), e.a);
        shader->set_uniform(("palette_b" + idx).c_str(), e.b);
    }
    if (color_tex != nullptr) {
        shader->set_uniform("color_tex", 1);
        glsafe(::glActiveTexture(GL_TEXTURE1));
        glsafe(::glBindTexture(GL_TEXTURE_2D, (GLuint) color_tex->get_id()));
        glsafe(::glActiveTexture(GL_TEXTURE0));
    }
    // The live UV-editor island drag rides this 2x3 affine (identity except mid-drag); only the flagged
    // island's vertices apply it, so a drag is a uniform update rather than a mesh rebuild.
    const Eigen::Matrix<float, 2, 3> &d = m_shaded_island_delta;
    shader->set_uniform("island_delta_lin", std::array<float, 4>{ d(0, 0), d(0, 1), d(1, 0), d(1, 1) });
    shader->set_uniform("island_delta_tr", Vec2f(d(0, 2), d(1, 2)));
    m_shaded_preview_glmodel.render();
    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
    shader->stop_using();
}

bool GLGizmoTextureDisplacement::shaded_preview_ready() const
{
    // Mirrors render_shaded_preview_mesh()'s own preconditions. Kept as a separate query because the
    // caller has to know whether the shaded pass will draw *before* it hides the real volume for it.
    if (m_c->selection_info() == nullptr || m_c->selection_info()->model_object() == nullptr)
        return false;
    if (texture_volume() == nullptr)
        return false;
    const TextureDisplacementLayer *layer = active_layer();
    if (layer == nullptr || layer->empty())
        return false;
    // The same texture render_shaded_preview_mesh() will bind, not the panel thumbnail - the two are
    // separate caches and either can fail on its own.
    const GLTexture *tex = const_cast<GLGizmoTextureDisplacement *>(this)->get_layer_height_texture(*layer);
    if (tex == nullptr || tex->get_width() <= 0 || tex->get_height() <= 0)
        return false;
    return wxGetApp().get_shader("texture_displacement_shaded") != nullptr;
}

// Appends a painted patch to an overlay, lifted onto the displaced surface where that has the base mesh's
// topology (see rebuild_paint_overlay()).
static void append_paint_patch(GLModel::Geometry &out, const indexed_triangle_set &patch, const std::vector<Vec3f> *displaced)
{
    unsigned n = unsigned(out.vertices_count());
    for (const stl_triangle_vertex_indices &tri : patch.indices) {
        for (int i = 0; i < 3; ++i) {
            const size_t idx = size_t(tri[i]);
            out.add_vertex((displaced != nullptr && idx < displaced->size()) ? (*displaced)[idx] : patch.vertices[idx]);
        }
        out.add_triangle(n, n + 1, n + 2);
        n += 3;
    }
}

void GLGizmoTextureDisplacement::rebuild_other_paint_overlay()
{
    const ModelVolume *mv = texture_volume();
    // What it depends on: the volume, which layer is active, every other layer's paint (by its timestamp) and the
    // displaced positions it is lifted onto. Compared every frame, rebuilt only when it differs.
    std::string key;
    if (mv != nullptr) {
        key = std::to_string(mv->id().id) + ":" + std::to_string(m_active_layer_slot) + (m_use_shaded_preview ? ":b:" : ":t:") +
              std::to_string(reinterpret_cast<uintptr_t>(m_preview_its.vertices.data())) + ":" +
              std::to_string(m_preview_its.vertices.size());
        for (const TextureDisplacementLayer &l : mv->texture_displacement_layers)
            if (l.slot != m_active_layer_slot && l.slot >= 0 && l.slot < int(TEXTURE_DISPLACEMENT_MAX_LAYERS))
                key += "|" + std::to_string(l.slot) + "@" + std::to_string(mv->texture_displacement_facet(l.slot).timestamp());
    }
    if (key == m_other_paint_key)
        return;
    m_other_paint_key = std::move(key);
    m_other_paint_glmodel.reset();
    if (mv == nullptr)
        return;

    const std::vector<Vec3f> *displaced = nullptr;
    if (!m_use_shaded_preview && m_preview_its.vertices.size() == mv->mesh().its.vertices.size() &&
        !m_preview_its.vertices.empty())
        displaced = &m_preview_its.vertices;

    GLModel::Geometry init_data;
    init_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
    for (const TextureDisplacementLayer &l : mv->texture_displacement_layers) {
        if (l.slot == m_active_layer_slot || l.slot < 0 || l.slot >= int(TEXTURE_DISPLACEMENT_MAX_LAYERS) ||
            mv->texture_displacement_facet(l.slot).empty())
            continue;
        TriangleSelector selector(mv->mesh());
        selector.deserialize(mv->texture_displacement_facet(l.slot).get_data(), false);
        append_paint_patch(init_data, selector.get_facets_strict(EnforcerBlockerType::ENFORCER), displaced);
    }
    if (init_data.is_empty())
        return;
    m_other_paint_glmodel.init_from(std::move(init_data));
    // Neutral grey: painted, but not the layer the brush is working on.
    m_other_paint_glmodel.set_color(ColorRGBA(0.55f, 0.58f, 0.60f, 0.35f));
}

std::vector<size_t> GLGizmoTextureDisplacement::PaintedColors::outside(const std::vector<bool> &excluded) const
{
    std::vector<size_t> out;
    out.reserve(source.size());
    for (size_t i = 0; i < source.size(); ++i)
        if (size_t(source[i]) >= excluded.size() || !excluded[size_t(source[i])])
            out.push_back(i);
    return out;
}

GLGizmoTextureDisplacement::PaintedColors GLGizmoTextureDisplacement::painted_colors(const TriangleMesh &mesh,
                                                                                     const TriangleSelector::TriangleSplittingData &paint)
{
    PaintedColors out;
    TriangleSelector selector(mesh);
    selector.deserialize(paint, false);
    for (const EnforcerBlockerType state : TriangleSelector::extract_used_facet_states(paint)) {
        if (state == EnforcerBlockerType::NONE)
            continue;
        std::vector<int>           source;
        const indexed_triangle_set part = selector.get_facets_strict(state, &source);
        // Every state comes back over the same vertex array, only the triangles differ.
        if (out.facets.vertices.empty())
            out.facets.vertices = part.vertices;
        out.facets.indices.insert(out.facets.indices.end(), part.indices.begin(), part.indices.end());
        out.source.insert(out.source.end(), source.begin(), source.end());
        out.state.resize(out.facets.indices.size(), int(state));
    }
    return out;
}

void GLGizmoTextureDisplacement::rebuild_painted_colors(bool whole_stack)
{
    const ModelVolume *mv    = texture_volume();
    const bool         shown = mv != nullptr && any_layer_colors(*mv) && !mv->mmu_segmentation_facets.empty();

    // The sub-triangles, keyed on what they were read from: the volume, its mesh and the paint.
    std::string key;
    if (shown)
        key = std::to_string(mv->id().id) + ":" + std::to_string(reinterpret_cast<uintptr_t>(mv->mesh_ptr().get())) + ":" +
              std::to_string(mv->mmu_segmentation_facets.timestamp());
    if (key != m_painted_colors_key) {
        m_painted_colors_key = std::move(key);
        m_painted_colors     = shown ? painted_colors(mv->mesh(), mv->mmu_segmentation_facets.get_data()) : PaintedColors{};
        m_painted_colors_drawn_key.clear();
        m_painted_colors_glmodel.reset();
        m_painted_colors_runs.clear();
    }
    if (m_painted_colors.state.empty())
        return;

    // The part drawn, keyed on whose paint is left out and on that paint.
    std::string drawn_key = m_painted_colors_key + (whole_stack ? std::string(":all") : ":" + std::to_string(m_active_layer_slot));
    for (const TextureDisplacementLayer &l : mv->texture_displacement_layers)
        if (l.slot >= 0 && l.slot < int(TEXTURE_DISPLACEMENT_MAX_LAYERS))
            drawn_key += "|" + std::to_string(l.slot) + "@" + std::to_string(mv->texture_displacement_facet(l.slot).timestamp());
    if (drawn_key == m_painted_colors_drawn_key)
        return;
    m_painted_colors_drawn_key = std::move(drawn_key);
    m_painted_colors_glmodel.reset();
    m_painted_colors_runs.clear();

    // Whole model triangles, as the facets record what they touch: a triangle the paint only partly covers
    // is left to the preview.
    std::vector<bool> excluded(mv->mesh().its.indices.size(), false);
    for (const TextureDisplacementLayer &l : mv->texture_displacement_layers) {
        if (l.slot < 0 || l.slot >= int(TEXTURE_DISPLACEMENT_MAX_LAYERS) || (!whole_stack && l.slot != m_active_layer_slot))
            continue;
        for (const TriangleSelector::TriangleBitStreamMapping &m : mv->texture_displacement_facet(l.slot).get_data().triangles_to_split)
            if (m.triangle_idx >= 0 && size_t(m.triangle_idx) < excluded.size())
                excluded[size_t(m.triangle_idx)] = true;
    }

    // One model, its triangles in filament order (painted_colors() groups them), drawn a range per filament.
    const std::vector<size_t> kept = m_painted_colors.outside(excluded);
    GLModel::Geometry         init_data;
    init_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
    init_data.reserve_vertices(kept.size() * 3);
    init_data.reserve_indices(kept.size() * 3);
    unsigned n = 0;
    for (const size_t t : kept) {
        const int state = m_painted_colors.state[t];
        if (m_painted_colors_runs.empty() || m_painted_colors_runs.back().first != state)
            m_painted_colors_runs.push_back({ state, { size_t(n), size_t(n) } });
        for (int i = 0; i < 3; ++i)
            init_data.add_vertex(m_painted_colors.facets.vertices[size_t(m_painted_colors.facets.indices[t][i])]);
        init_data.add_triangle(n, n + 1, n + 2);
        n += 3;
        m_painted_colors_runs.back().second.second = size_t(n);
    }
    if (!init_data.is_empty())
        m_painted_colors_glmodel.init_from(std::move(init_data));
}

bool GLGizmoTextureDisplacement::render_painted_colors(bool whole_stack)
{
    rebuild_painted_colors(whole_stack);
    const ModelObject *mo     = m_c->selection_info()->model_object();
    const ModelVolume *mv     = texture_volume();
    GLShaderProgram   *shader = wxGetApp().get_shader("mm_gouraud");
    if (mo == nullptr || mv == nullptr || shader == nullptr || !m_painted_colors_glmodel.is_initialized())
        return false;

    const Selection             &selection    = m_parent.get_selection();
    const Transform3d            trafo_matrix = mo->instances[selection.get_instance_idx()]->get_transformation().get_matrix() * mv->get_matrix();
    const Camera                &camera       = wxGetApp().plater()->get_camera();
    const Transform3d           &view_matrix  = camera.get_view_matrix();
    const Matrix3d               normal_matrix = trafo_matrix.matrix().block(0, 0, 3, 3).inverse().transpose();
    const std::vector<ColorRGBA> colors        = wxGetApp().plater()->get_extruders_colors();

    shader->start_using();
    // Set up as render_triangles() sets it up, so the colours are lit, clipped and slope-marked exactly as the
    // neutral surface they cover.
    const ClippingPlaneDataWrapper clp_data = get_clipping_plane_data();
    shader->set_uniform("clipping_plane", clp_data.clp_dataf);
    shader->set_uniform("z_range", clp_data.z_range);
    shader->set_uniform("view_model_matrix", view_matrix * trafo_matrix);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    shader->set_uniform("view_normal_matrix", Matrix3d(view_matrix.matrix().block(0, 0, 3, 3) * normal_matrix));
    shader->set_uniform("volume_world_matrix", trafo_matrix);
    shader->set_uniform("volume_mirrored", trafo_matrix.matrix().determinant() < 0.);
    shader->set_uniform("slope.actived", m_parent.is_using_slope());
    shader->set_uniform("slope.volume_world_normal_matrix", Matrix3f(normal_matrix.cast<float>()));
    shader->set_uniform("slope.normal_z", float(-std::cos(Geometry::deg2rad(get_highlight_angle_limit_deg()))));
    shader->set_uniform("slope.up_direction", get_tilt_up_direction());
    shader->set_uniform("show_wireframe", false);
    // Pulled forward and without depth writes, as the tint is (see render_paint_overlay()). The tint is
    // drawn after this, so it shows on top.
    glsafe(::glEnable(GL_POLYGON_OFFSET_FILL));
    glsafe(::glPolygonOffset(-2.f, -2.f));
    glsafe(::glDepthMask(GL_FALSE));
    bool drawn = false;
    for (const auto &[state, range] : m_painted_colors_runs)
        if (state >= 1 && size_t(state) <= colors.size()) {
            m_painted_colors_glmodel.set_color(adjust_color_for_rendering(colors[size_t(state - 1)]));
            m_painted_colors_glmodel.render(range, shader);
            drawn = true;
        }
    glsafe(::glDepthMask(GL_TRUE));
    glsafe(::glDisable(GL_POLYGON_OFFSET_FILL));
    shader->stop_using();
    return drawn;
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
    if (!m_use_shaded_preview && m_preview_its.vertices.size() == mv->mesh().its.vertices.size() &&
        !m_preview_its.vertices.empty())
        displaced = &m_preview_its.vertices;

    GLModel::Geometry init_data;
    init_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
    init_data.reserve_vertices(patch.indices.size() * 3);
    init_data.reserve_indices(patch.indices.size() * 3);
    append_paint_patch(init_data, patch, displaced);
    m_paint_overlay_glmodel.init_from(std::move(init_data));
    // GLModel::render() re-sets "uniform_color" from this field just before drawing, so the colour
    // has to be set here rather than as a uniform at draw time.
    m_paint_overlay_glmodel.set_color(ColorRGBA(0.16f, 0.79f, 0.35f, 0.38f));
}

void GLGizmoTextureDisplacement::render_paint_overlay(GLModel &overlay)
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    const ModelVolume *mv = texture_volume();
    if (mo == nullptr || mv == nullptr || !overlay.is_initialized())
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
    // shaded surface - and against the selectors' highlight at -1 in the Normal view, by the full depth
    // unit OpenGL guarantees to tell apart. Depth writes are off: this is a tint, and letting it own the
    // depth buffer would make the wireframe and seam overlays drawn after it fight with geometry that is
    // not really there. Blending is already enabled by render_painter_gizmo().
    glsafe(::glEnable(GL_POLYGON_OFFSET_FILL));
    glsafe(::glPolygonOffset(-2.f, -2.f));
    glsafe(::glDepthMask(GL_FALSE));
    overlay.render();
    glsafe(::glDepthMask(GL_TRUE));
    glsafe(::glDisable(GL_POLYGON_OFFSET_FILL));
    shader->stop_using();
}

const std::vector<int> *GLGizmoTextureDisplacement::island_highlight_selection()
{
    const TextureDisplacementLayer *al        = active_layer();
    const UVEditorCanvas           *uv_canvas = wxGetApp().plater()->get_uv_editor_canvas();
    if (m_show_uv_editor && al != nullptr && al->projection_method == TextureProjectionMethod::LSCM &&
        uv_canvas != nullptr && !m_uv_editor_unwrap.empty())
        return &uv_canvas->selected_islands();
    return nullptr;
}

void GLGizmoTextureDisplacement::rebuild_island_overlay(const std::vector<int> &selection)
{
    m_island_overlay_glmodel.reset();
    m_island_overlay_selection = selection;
    const ModelVolume *mv = texture_volume();
    if (mv == nullptr || selection.empty() || m_uv_editor_unwrap.empty())
        return;
    // The unwrap was made from the painted patch in the bake frame; the same extraction on the
    // volume's own mesh gives the same triangles and vertex order in local coordinates, which is the
    // frame the overlay is drawn in (with the volume's transform, like the paint tint).
    const indexed_triangle_set patch = extract_painted_patch(mv->mesh().its, m_uv_editor_state.facets);
    const PatchUnwrap         &uw    = m_uv_editor_unwrap;
    std::vector<uint8_t>       chosen(size_t(std::max(uw.chart_count, 0)), 0);
    for (const int c : selection)
        if (c >= 0 && size_t(c) < chosen.size())
            chosen[size_t(c)] = 1;

    GLModel::Geometry init_data;
    init_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
    unsigned n = 0;
    for (const stl_triangle_vertex_indices &tri : uw.indices) {
        const int v0 = tri[0];
        if (v0 < 0 || size_t(v0) >= uw.vertex_chart.size())
            continue;
        const int c = uw.vertex_chart[size_t(v0)];
        if (c < 0 || size_t(c) >= chosen.size() || !chosen[size_t(c)])
            continue;
        bool ok = true;
        for (int k = 0; k < 3 && ok; ++k) {
            const int u = tri[k];
            ok = u >= 0 && size_t(u) < uw.source_vertex.size() && uw.source_vertex[size_t(u)] >= 0 &&
                 size_t(uw.source_vertex[size_t(u)]) < patch.vertices.size();
        }
        if (!ok)
            continue;
        for (int k = 0; k < 3; ++k)
            init_data.add_vertex(patch.vertices[size_t(uw.source_vertex[size_t(tri[k])])]);
        init_data.add_triangle(n, n + 1, n + 2);
        n += 3;
    }
    if (n == 0)
        return;
    m_island_overlay_glmodel.init_from(std::move(init_data));
    m_island_overlay_glmodel.set_color(ColorRGBA(0.10f, 0.55f, 0.95f, 0.45f)); // the pane's selection blue
}

void GLGizmoTextureDisplacement::render_island_overlay()
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    const ModelVolume *mv = texture_volume();
    if (mo == nullptr || mv == nullptr || !m_island_overlay_glmodel.is_initialized())
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
    // Above the paint tint (a larger offset), translucent, no depth writes - a marker, not geometry.
    glsafe(::glEnable(GL_POLYGON_OFFSET_FILL));
    glsafe(::glPolygonOffset(-2.0f, -2.0f));
    glsafe(::glDepthMask(GL_FALSE));
    m_island_overlay_glmodel.render();
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
    std::vector<int>           patch_source; // only ModelUV reads it; see compute_layer_vertex_uvs()
    const indexed_triangle_set patch =
        m_triangle_selectors[0]->get_facets_strict(EnforcerBlockerType::ENFORCER, &patch_source);
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
    std::vector<Vec2f> uv       = compute_layer_vertex_uvs(patch, *layer, &patch_source);
    const bool         have_uvs = uv.size() == patch.vertices.size();
    m_uvcheck_uses_vertex_uv    = have_uvs;
    m_uvcheck_projection_mode   = have_uvs ? 0 :
                                      layer_projection_frame(patch, *layer, m_uvcheck_patch_center, m_uvcheck_patch_axis);
    // Per corner as well: under LSCM a seam vertex has a
    // different uv in each island it borders, so the shared-vertex form drew one triangle per face from
    // a neighbouring island's placement. Only the *drawing* needs this; the distortion metric below is
    // a per-vertex average by construction and keeps using `uv`.
    const std::vector<Vec2f> corner_uv = compute_layer_corner_uvs(patch, *layer, &patch_source);
    const bool               have_corner_uvs = have_uvs && corner_uv.size() == patch.indices.size() * 3;

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
    // Flat (one vertex per triangle corner), so each triangle can carry its own island's uv - see
    // have_corner_uvs above. Costs nothing in shading quality: the overlay shades from uv and the
    // interpolated distortion value alone, never from a per-vertex normal.
    init_data.reserve_vertices(patch.indices.size() * 3);
    init_data.reserve_indices(patch.indices.size() * 3);
    unsigned vcount = 0;
    for (size_t f = 0; f < patch.indices.size(); ++f) {
        const stl_triangle_vertex_indices &tri = patch.indices[f];
        for (int k = 0; k < 3; ++k) {
            const size_t vi = size_t(tri[k]);
            init_data.add_vertex(patch.vertices[vi], Vec3f(distortion[vi], 0.f, 0.f),
                                 have_corner_uvs ? corner_uv[f * 3 + size_t(k)] :
                                                   (have_uvs ? uv[vi] : Vec2f::Zero()));
        }
        init_data.add_triangle(vcount, vcount + 1, vcount + 2);
        vcount += 3;
    }

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
    // The shader works on world-space normals (it projects the texture in world millimetres, as the
    // bake does), so the normal matrix is the view rotation alone - no model part.
    const Matrix3d view_normal_matrix = camera.get_view_matrix().matrix().block(0, 0, 3, 3);
    shader->set_uniform("view_normal_matrix", view_normal_matrix);
    shader->set_uniform("volume_mirrored", trafo_matrix.matrix().determinant() < 0.0);
    shader->set_uniform("tex_anchor", Vec3f(trafo_matrix.translation().cast<float>())); // see the preview shader
    shader->set_uniform("mode", m_uv_check_mode == UVCheckMode::Distortion ? 1 : 0);
    shader->set_uniform("checker_freq", 4.f); // squares per texture tile
    shader->set_uniform("tiling_scale", layer->tiling_scale);
    shader->set_uniform("rotation_rad", layer->rotation_deg * float(M_PI) / 180.f);
    shader->set_uniform("uv_offset", layer->offset);
    shader->set_uniform("use_vertex_uv", m_uvcheck_uses_vertex_uv);
    shader->set_uniform("projection_mode", m_uvcheck_projection_mode);
    shader->set_uniform("patch_center", m_uvcheck_patch_center);
    shader->set_uniform("patch_axis", m_uvcheck_patch_axis);
    // Was never uploaded, so the checker disagreed with the bake for any non-square height map.
    shader->set_uniform("tex_aspect", layer_texture_aspect(*layer));

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

    // Building from the base mesh (shaded/paint mode); its topology only changes on bake/subdivide, and
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
    // leaves them buried inside the relief, which is why the wireframe "didn't show in real mode". In
    // Fast (shaded) mode or with nothing painted, the surface is the undisplaced base mesh.
    if (!m_use_shaded_preview && !m_preview_its.indices.empty())
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
    // Raised first: any in-flight job's result (captured generation from before this call) will
    // now compare unequal to m_preview_generation and be discarded when it completes, even if it
    // finishes after the job queued below - and, since the counter is shared with the worker, that
    // job also notices mid-run and aborts rather than computing a result nobody will use.
    m_preview_generation->fetch_add(1);
    // Everything rebuilt from here on reads the current palette. Cleared before any of the early returns
    // below, which would otherwise leave it set and re-run this every frame.
    m_palette_changed = false;
    ChromaProfileSteps prof("rebuild_preview");
    update_uv_editor();
    prof.step("update_uv_editor");
    rebuild_shaded_preview_mesh();
    prof.step("rebuild_shaded_preview_mesh");
    rebuild_paint_overlay();
    prof.step("rebuild_paint_overlay");
    rebuild_uvcheck_mesh();
    prof.step("rebuild_uvcheck_mesh");
    rebuild_seam_overlay();
    prof.step("rebuild_seam_overlay");
    // The adaptive subdivision preview is driven by the painted area, so it has to follow the paint
    // while it is open - a plain stroke changes which triangles would be refined. The uniform preview
    // depends only on the mesh, so it is left to its own controls.
    if (m_subdivide_editing && m_subdivide_adaptive)
        rebuild_subdivide_preview();
    prof.step("rebuild_subdivide_preview");

    // The debug view owns m_preview_glmodel while it is up. A stage is a snapshot of a run that has
    // already finished, so letting the live preview overwrite it would replace the thing being
    // inspected with something else; leaving the view is explicit (its Close button).
    if (m_debug_stage >= 0)
        return;

    const ModelVolume *mv = texture_volume();
    if (mv == nullptr || !any_part_painted()) {
        m_preview_glmodel.reset();
        m_part_previews.clear();
        m_preview_its = indexed_triangle_set{}; // no displaced mesh; wireframe falls back to the base
        m_preview_displaced = false;
        m_preview_job_pending = false;
        refresh_wireframe();
        return;
    }
    // In Fast/paint modes the wireframe follows the base mesh and can be built now; the true-displacement
    // view's wireframe needs the displaced mesh, which only exists once the job below completes.
    if (m_use_shaded_preview) {
        refresh_wireframe();
        // Fast view: the shader *is* the preview, and m_preview_glmodel is never drawn. Running the
        // full CPU displacement anyway - which is what happened on every stroke and slider release -
        // was the single largest cost in the gizmo, and it bought nothing. The switch back to the
        // true-displacement view queues it (see the View row in on_render_input_window()).
        m_preview_job_pending = false;
        prof.total("fast view, no displacement job");
        return;
    }

    queue_preview_job();
    prof.total("displacement job queued");
}

indexed_triangle_set GLGizmoTextureDisplacement::group_preview_by_color(const indexed_triangle_set   &its,
                                                                        const std::vector<uint8_t>   &triangle_color,
                                                                        const std::vector<ColorRGBA> &entry_colors,
                                                                        std::vector<PreviewColorRun> &runs)
{
    runs.clear();
    if (triangle_color.size() != its.indices.size() || entry_colors.empty())
        return its;

    indexed_triangle_set sorted;
    sorted.vertices = its.vertices;
    sorted.indices.reserve(its.indices.size());
    for (int want = 0; want <= int(entry_colors.size()); ++want) {
        const size_t first = sorted.indices.size();
        for (size_t i = 0; i < its.indices.size(); ++i)
            if (int(triangle_color[i]) == want)
                sorted.indices.push_back(its.indices[i]);
        if (sorted.indices.size() == first)
            continue;
        runs.push_back({ { first * 3, sorted.indices.size() * 3 },
                         want == 0 ? GLVolume::NEUTRAL_COLOR : entry_colors[size_t(want - 1)] });
    }
    return sorted;
}

void GLGizmoTextureDisplacement::queue_preview_job()
{
    // A job in flight when the gizmo closes still runs its completion handler, which would otherwise
    // happily queue the follow-up run it was holding - against a gizmo nobody is looking at any more.
    if (m_state != On)
        return;
    const ModelVolume *owner = texture_volume();
    if (owner == nullptr)
        return;
    // Every painted part, so the preview shows the whole of what Bake will do rather than its first
    // part - the same set bake() displaces.
    const std::vector<ModelVolume *> parts = painted_parts();
    if (parts.empty())
        return;

    // One in flight at a time; everything requested meanwhile collapses into a single follow-up run
    // issued from the completion handler. See m_preview_job_running.
    if (m_preview_job_running) {
        m_preview_job_pending = true;
        return;
    }

    const uint64_t generation = m_preview_generation->load();

    TextureDisplacementPreviewInput input;
    // Captured here rather than read in the handler: the palette is main-thread state, and the preview
    // has to be grouped against the same palette it was computed with, not whatever it is by the time
    // the result lands. Read off the layer stack's own part, since that is where the stack lives.
    input.color = color_settings_for(*owner);
    input.volumes.reserve(parts.size());
    // Which part each result belongs to, so the handler can tell the layer stack's own preview - the one
    // the overlays and the wireframe are keyed to - from the rest.
    std::vector<ObjectID> part_ids;
    part_ids.reserve(parts.size());
    const Vec3d              anchor = texture_anchor();
    const std::vector<float> drops  = resting_drops(parts);
    for (size_t i = 0; i < parts.size(); ++i) {
        const ModelVolume               *part = parts[i];
        TextureDisplacementPreviewVolume entry;
        entry.base_mesh = part->mesh().its;
        entry.layers    = layers_for_part(*part);
        entry.options   = owner->texture_displacement_options;
        // The preview is looked at, not printed or sliced: skip the simplification and the repair that
        // follows it, which are most of the one-run pipeline's time. The relief is the same.
        if (entry.options.pipeline_v2)
            entry.options.v2_max_triangles_k = 0;
        entry.volume_to_world = texture_displacement_volume_to_world(*part);
        entry.texture_anchor  = anchor;
        entry.resting_drop    = drops[i];
        // A generated ChromaStack plate's picture is one flat face, so its relief can be previewed
        // straight from the height map rather than through the bake's refinement, which took seconds
        // and grew with the Quality setting. That matters because every colour edit re-solves the
        // height map and lands here. See build_texture_displacement_flat_preview().
        if (std::any_of(entry.layers.begin(), entry.layers.end(), [](const TextureDisplacementLayer &l) {
                return l.chroma_enabled && l.chroma.plate.generated;
            }))
            entry.flat_preview_cells = CHROMA_FLAT_PREVIEW_CELLS;
        if (part->has_texture_uv())
            entry.model_uv = part->texture_uv;
        for (int slot = 0; slot < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++slot)
            entry.facets_data[size_t(slot)] = part->texture_displacement_facet(slot).get_data();
        input.volumes.push_back(std::move(entry));
        part_ids.push_back(part->id());
    }
    const ObjectID owner_id = owner->id();

    // The result names a palette entry per triangle (index + 1), so these are the colours to draw it in:
    // a single filament's own, or for a mix the colour its slot will show once a bake creates it.
    std::vector<ColorRGBA> entry_colors;
    entry_colors.reserve(input.color.palette.size());
    for (const PrintableColor &e : input.color.palette)
        entry_colors.emplace_back(e.rgb.x(), e.rgb.y(), e.rgb.z(), 1.f);

    m_preview_job_running = true;
    auto &worker = wxGetApp().plater()->get_ui_job_worker();
    queue_job(worker, std::make_unique<TextureDisplacementPreviewJob>(std::move(input), generation, m_preview_generation,
        [this, entry_colors, part_ids, owner_id](std::vector<TextureDisplacementPreviewResult> results,
                                                 uint64_t result_generation) {
            ChromaProfileSteps prof("preview_landed");
            m_preview_job_running = false;
            if (result_generation != m_preview_generation->load()) {
                // Superseded while this was computing (it will have aborted early and come back
                // empty). Whatever the newest state is, it still needs a run.
                m_preview_job_pending = true;
            } else if (results.empty()) {
                // Aborted or cancelled rather than finished - the handler runs on every outcome so
                // the in-flight latch above always clears. Keep whatever preview is already on screen
                // rather than blanking it; there is no new result to show, not a new empty one.
            } else {
                // A finished run carries the whole object, so it replaces the previous one outright: a
                // part that is no longer painted must lose its preview, or its flat volume would stay
                // hidden behind geometry from before.
                m_preview_glmodel.reset();
                m_preview_color_runs.clear();
                m_preview_its = indexed_triangle_set{};
                m_part_previews.clear();

                for (size_t i = 0; i < results.size() && i < part_ids.size(); ++i) {
                    indexed_triangle_set its = std::move(results[i].mesh);
                    if (its.indices.empty())
                        continue; // this part's layers moved nothing; its own volume is drawn instead

                    std::vector<PreviewColorRun> runs;
                    indexed_triangle_set         grouped =
                        group_preview_by_color(its, results[i].triangle_color, entry_colors, runs);

                    if (part_ids[i] == owner_id) {
                        m_preview_color_runs = std::move(runs);
                        m_preview_glmodel.init_from(grouped);
                        m_preview_glmodel.set_color(GLVolume::NEUTRAL_COLOR);
                        // Keep the displaced mesh so the wireframe overlay can be drawn on the raised
                        // surface actually shown in the true-displacement view.
                        m_preview_its = std::move(its);
                    } else {
                        auto preview        = std::make_unique<PartPreview>();
                        preview->volume_id  = part_ids[i];
                        preview->color_runs = std::move(runs);
                        preview->glmodel.init_from(grouped);
                        preview->glmodel.set_color(GLVolume::NEUTRAL_COLOR);
                        m_part_previews.push_back(std::move(preview));
                    }
                    prof.step("group_by_color", std::to_string(grouped.indices.size()) + " tri x " +
                                                   std::to_string(entry_colors.size() + 1) + " entries");
                }
                prof.step("glmodel_init_from");

                // Did the run move anything? The pipeline refines and hands back a surface whether or
                // not the layers have a height map to displace with, so the mesh arriving says nothing
                // about the relief being in it. Compared by bounding box, because refinement means the
                // two meshes do not share a topology and there are no vertices to pair up. The object's
                // own part is the one asked, since it is the one whose base mesh is at hand.
                m_preview_displaced = false;
                if (const ModelVolume *base = texture_volume();
                    base != nullptr && !m_preview_its.vertices.empty() && !base->mesh().its.vertices.empty()) {
                    const auto span = [](const indexed_triangle_set &m) {
                        Vec3f lo = m.vertices.front(), hi = m.vertices.front();
                        for (const Vec3f &v : m.vertices) {
                            lo = lo.cwiseMin(v);
                            hi = hi.cwiseMax(v);
                        }
                        return std::make_pair(lo, hi);
                    };
                    const auto [blo, bhi] = span(base->mesh().its);
                    const auto [plo, phi] = span(m_preview_its);
                    m_preview_displaced   = (blo - plo).cwiseAbs().maxCoeff() > 1e-4f ||
                                          (bhi - phi).cwiseAbs().maxCoeff() > 1e-4f;
                }
                prof.step("displaced_bbox_check");

                refresh_wireframe();
                prof.step("refresh_wireframe");
                // The paint tint rides the displaced surface in this view, so it follows the new mesh.
                m_paint_overlay_dirty = true;
            }
            if (m_preview_job_pending) {
                m_preview_job_pending = false;
                if (!m_use_shaded_preview)
                    queue_preview_job(); // no-ops if the gizmo has closed in the meantime
            }
            m_parent.set_as_dirty();
            prof.total();
        }));
}

} // namespace Slic3r::GUI
