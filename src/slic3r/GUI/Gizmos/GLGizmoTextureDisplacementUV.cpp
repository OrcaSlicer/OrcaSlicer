// One of the files GLGizmoTextureDisplacement is defined across. The class is declared once, in
// GLGizmoTextureDisplacement.hpp; its members are defined in GLGizmoTextureDisplacement.cpp and in the
// siblings named after the part of the gizmo they hold - Chroma, Library, Mesh, Preview, UV. The split
// is along those lines and nothing else: no member moved between parts, and the class did not change.
// Helpers more than one part needs live in GLGizmoTextureDisplacementInternal.hpp.

// The UV side of the gizmo: the seams a chart is cut along and how they are picked on the model, the
// unwrap and the islands it produces, the UV editor pane and the commands it sends back, and the
// projection frame a layer is captured in.

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

void GLGizmoTextureDisplacement::update_uv_editor()
{
    Plater         *plater    = wxGetApp().plater();
    UVEditorCanvas *uv_canvas = plater->get_uv_editor_canvas();

    const ModelVolume        *mv    = texture_volume();
    TextureDisplacementLayer *layer = active_layer();
    // The pane is opened only on the user's explicit request (m_show_uv_editor), and only for an LSCM
    // layer - never automatically just because something is painted. Keep the cached state/unwrap so
    // that switching the toggle back on re-shows instantly (and re-solves if the paint changed while
    // it was hidden, via the state comparison below).
    if (!m_show_uv_editor || mv == nullptr || layer == nullptr ||
        layer->projection_method != TextureProjectionMethod::LSCM) {
        m_uv_editor_bg = UVBackground::None; // the next time it is shown, upload the background afresh
        push_uv_pane_state();
        plater->show_uv_editor(false);
        return;
    }
    if (uv_canvas == nullptr) {
        // The pane does not exist yet: show_uv_editor() builds it after this frame. Run again right
        // after that - both are queued on the plater, so they run in this order.
        plater->show_uv_editor(true);
        plater->CallAfter([this]() { update_uv_editor(); });
        return;
    }
    // Wired on every call rather than with the first unwrap: Unwrap itself is one of the pane's commands.
    uv_canvas->set_command_callback([this](UVEditorCanvas::Command cmd, float value) { on_uv_command(int(cmd), value); });

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
        // World millimetres, the space the bake unwraps in - otherwise the pane would lay the islands
        // out at the volume's own scale and show the texture at a different size than it bakes at.
        const indexed_triangle_set patch = patch_in_world(extract_painted_patch(mv->mesh().its, state.facets));
        if (patch.indices.empty()) {
            // Nothing painted to unwrap: clear what the pane showed but keep it open - its status line says what to do.
            m_uv_editor_state  = UVEditorState{};
            m_uv_editor_unwrap = PatchUnwrap{};
            m_uv_editor_distortion_colors.clear();
            uv_canvas->set_islands({});
            push_uv_pane_state();
            plater->show_uv_editor(true);
            return;
        }
        // Padding disabled (0): the user asked to pack islands with no gap between them.
        m_uv_editor_unwrap = compute_patch_unwrap(patch, layer->lscm_seam_angle_deg, 0.f, layer->lscm_seam_edges);
        m_island_overlay_glmodel.reset(); // the islands were renumbered: rebuilt from the pane's selection next frame
        m_island_overlay_selection.clear();
        // Re-apply any stored UV edits onto the fresh unwrap, so the pane shows exactly what
        // compute_lscm_uvs() will bake (which applies the same overrides).
        apply_lscm_uv_overrides(m_uv_editor_unwrap, layer->lscm_uv_overrides);
        m_uv_editor_state  = std::move(state);
        unwrap_changed     = true;
        // Precompute the distortion heatmap now, while the patch is in hand - relative stretch doesn't
        // change when islands are only moved, so this need not be redone on a drag. It is fed to the
        // canvas below only while the Distortion check mode is on.
        compute_uv_editor_distortion_colors(patch);
    }

    // The pane background either mirrors the height texture (default) or shows a UV checker (#7). It is uploaded
    // before, and independently of, the unwrap - the texture belongs on screen before anything is unwrapped - and
    // only when the choice, the layer's image or its smoothing changed, because the height image is large.
    const UVBackground desired_bg = (m_uv_check_mode == UVCheckMode::Checker) ? UVBackground::Checker : UVBackground::Height;
    const bool bg_smoothing_changed = (desired_bg == UVBackground::Height) &&
                                      (m_uv_editor_bg_smoothing != layer->smoothing ||
                                       !(m_uv_editor_bg_adjust == layer->preview_adjust));
    if (desired_bg != m_uv_editor_bg || layer->image_data.get() != m_uv_editor_bg_image || bg_smoothing_changed) {
        m_uv_editor_bg_smoothing = layer->smoothing;
        m_uv_editor_bg_adjust    = layer->preview_adjust;
        m_uv_editor_bg_image     = layer->image_data.get();
        m_uv_editor_bg           = desired_bg;
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
            if (!height.empty()) {
                uv_canvas->set_background_texture(height.pixels, height.width, height.height);
            } else {
                // Not recorded as uploaded, so the next update tries again rather than keeping a blank backdrop.
                uv_canvas->set_background_texture({}, 0, 0);
                m_uv_editor_bg = UVBackground::None;
            }
        }
    }
    // The backdrop's extent and repeat follow the tile settings, with or without an unwrap.
    uv_canvas->set_uv_transform(layer->tiling_scale, layer->rotation_deg, layer->tile_enabled,
                                layer->tile_method == TextureTileMethod::MirroredRepeat);

    if (m_uv_editor_unwrap.empty()) {
        // Nothing unwrapped yet: the pane opens anyway, because Unwrap itself lives in it. Its canvas shows the
        // texture and its status line says to paint the area and press Unwrap.
        push_uv_pane_state();
        plater->show_uv_editor(true);
        return;
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
        uv_canvas->set_islands(std::move(view));
    }

    uv_canvas->set_select_mode(static_cast<UVEditorCanvas::SelectMode>(m_uv_select_mode));
    uv_canvas->set_island_transforms(uv_editor_island_transforms(*layer));
    // The distortion heatmap tints the island fills only while its check mode is on; otherwise the
    // canvas falls back to its default light-green wash.
    if (m_uv_check_mode == UVCheckMode::Distortion)
        uv_canvas->set_island_fill_colors(m_uv_editor_distortion_colors);
    else
        uv_canvas->set_island_fill_colors({});

    push_uv_pane_state();
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

void GLGizmoTextureDisplacement::on_uv_command(int cmd, float value)
{
    m_uv_command_queue.emplace_back(cmd, value);
    m_parent.set_as_dirty(); // the panel render that runs it
}

void GLGizmoTextureDisplacement::process_uv_commands()
{
    std::vector<std::pair<int, float>> queue;
    queue.swap(m_uv_command_queue);
    for (const auto &[cmd, value] : queue)
        run_uv_command(cmd, value);
}

void GLGizmoTextureDisplacement::apply_view_mode(int mode)
{
    m_use_shaded_preview = (mode == 1);
    m_uv_check_mode    = (mode == 2) ? UVCheckMode::Checker :
                         (mode == 3) ? UVCheckMode::Distortion : UVCheckMode::None;
    rebuild_uvcheck_mesh();
    if (m_use_shaded_preview)
        rebuild_shaded_preview_mesh();
    else
        // The Fast view skips the CPU displacement entirely (see rebuild_preview()), so leaving it means
        // m_preview_glmodel may be stale or absent - ask for it now.
        queue_preview_job();
    // ...and the paint tint between the base and the displaced surface, for the same reason.
    m_paint_overlay_dirty = true;
    refresh_wireframe(); // Normal<->Fast swaps the wireframe between displaced and base mesh
    update_uv_editor();  // mirror the checker / distortion heatmap into the UV pane too
    m_parent.set_as_dirty();
}

int GLGizmoTextureDisplacement::view_mode() const
{
    return m_use_shaded_preview                         ? 1 :
           m_uv_check_mode == UVCheckMode::Checker    ? 2 :
           m_uv_check_mode == UVCheckMode::Distortion ? 3 : 0;
}

int GLGizmoTextureDisplacement::IslandViewFollow::step(bool shown_now, int view, bool debug_open)
{
    shown = shown_now;
    if (!shown) {
        // The view is back to Normal already if a debug stage took it, and Normal applied over a stage would
        // queue a preview job that replaces the stage being inspected.
        const bool restore = switched && view == 1;
        switched = keep_normal = false;
        return restore ? 0 : -1;
    }
    if (view != 0 || keep_normal || debug_open)
        return -1;
    switched = true;
    return 1;
}

void GLGizmoTextureDisplacement::follow_island_highlight()
{
    const std::vector<int> *islands = island_highlight_selection();
    const int view = m_island_view.step(islands != nullptr && !islands->empty(), view_mode(), m_debug_stage >= 0);
    if (view >= 0)
        apply_view_mode(view);
}

void GLGizmoTextureDisplacement::push_uv_pane_state()
{
    UVEditorCanvas *canvas = wxGetApp().plater()->get_uv_editor_canvas();
    if (canvas == nullptr)
        return;

    UVEditorCanvas::PaneState       st;
    const ModelVolume              *mv    = texture_volume();
    const TextureDisplacementLayer *layer = active_layer();
    if (mv != nullptr && layer != nullptr && layer->projection_method == TextureProjectionMethod::LSCM) {
        st.has_layer       = true;
        st.layer_name      = from_u8(layer->name.empty() ? Slic3r::format(_u8L("Layer %1%"), layer->slot + 1) : layer->name);
        st.tile_mm         = layer->tiling_scale;
        st.seam_angle_deg  = layer->lscm_seam_angle_deg;
        st.connect_islands = layer->auto_connect_islands;
        st.has_seams       = !layer->lscm_seam_edges.empty();
        st.has_uv_edits    = !layer->lscm_uv_overrides.empty();

        // Retried while empty: the image may not decode yet, and a blank thumbnail must not stick.
        if (layer->image_data.get() != m_uv_thumb_source || m_uv_thumb_rgb.empty()) {
            m_uv_thumb_source = nullptr;
            m_uv_thumb_rgb.clear();
            const DecodedHeightTexture tex = decode_height_texture(*layer);
            if (!tex.empty()) {
                m_uv_thumb_source = layer->image_data.get();
                // Box-filtered, so a fine pattern such as bricks averages out instead of aliasing into noise.
                const int  n     = UV_THUMB_PX;
                const bool color = tex.has_color();
                m_uv_thumb_rgb.resize(size_t(n) * n * 3);
                for (int y = 0; y < n; ++y) {
                    const int y0 = y * tex.height / n, y1 = std::max(y0 + 1, (y + 1) * tex.height / n);
                    for (int x = 0; x < n; ++x) {
                        const int x0 = x * tex.width / n, x1 = std::max(x0 + 1, (x + 1) * tex.width / n);
                        uint64_t  sum[3]{};
                        for (int sy = y0; sy < y1; ++sy)
                            for (int sx = x0; sx < x1; ++sx) {
                                const size_t src = size_t(sy) * size_t(tex.width) + size_t(sx);
                                for (int ch = 0; ch < 3; ++ch)
                                    sum[ch] += color ? tex.rgb[src * 3 + ch] : tex.pixels[src];
                            }
                        const uint64_t count = uint64_t(y1 - y0) * uint64_t(x1 - x0);
                        for (int ch = 0; ch < 3; ++ch)
                            m_uv_thumb_rgb[size_t(y * n + x) * 3 + ch] = (unsigned char) (sum[ch] / count);
                    }
                }
            }
        }
        if (!m_uv_thumb_rgb.empty()) {
            st.thumbnail_px  = UV_THUMB_PX;
            st.thumbnail_rgb = m_uv_thumb_rgb;
        }

        if (!m_uv_editor_unwrap.empty()) {
            // Stale when the unwrap on screen was made from another layer, other paint, other seams or another
            // seam angle than this layer has now - the same things update_uv_editor() re-solves for.
            UVEditorState now;
            now.slot        = m_active_layer_slot;
            now.image_data  = layer->image_data.get();
            now.seam_angle  = layer->lscm_seam_angle_deg;
            now.padding     = layer->island_padding_mm;
            now.facets      = mv->texture_displacement_facet(m_active_layer_slot).get_data();
            now.seam_edges  = layer->lscm_seam_edges;
            st.unwrapped    = true;
            st.unwrap_stale = !(now == m_uv_editor_state);
            st.island_count = m_uv_editor_unwrap.chart_count;
            st.face_count   = m_uv_editor_unwrap.indices.size();
        }
    }
    st.mark_seams = m_seam_edit_mode;
    st.seam_path  = m_seam_path_mode;
    st.background = m_uv_check_mode == UVCheckMode::Checker    ? UVEditorCanvas::Background::Checker :
                    m_uv_check_mode == UVCheckMode::Distortion ? UVEditorCanvas::Background::Distortion :
                                                                 UVEditorCanvas::Background::Height;
    canvas->set_pane_state(std::move(st));
}

void GLGizmoTextureDisplacement::run_uv_command(int cmd, float value)
{
    using Command = UVEditorCanvas::Command;
    if (cmd == int(Command::PaneClosed)) {
        // Closed with the pane's own X: keep it closed until asked again, and upload the background afresh then.
        m_show_uv_editor = false;
        m_uv_editor_bg   = UVBackground::None;
        // The seam tool belongs to the pane: left on with the pane gone, every stroke on the model would
        // be swallowed as a seam click and nothing would paint.
        if (m_seam_edit_mode) {
            m_seam_edit_mode    = false;
            m_seam_hover_edge   = { -1, -1 };
            m_seam_hover_vertex = -1;
            m_seam_hover_glmodel.reset();
            m_seam_path_anchor  = -1;
            m_seam_anchor_glmodel.reset();
            push_uv_pane_state();
        }
        return;
    }
    if (cmd == int(Command::SetBackground)) {
        // Height goes back to whichever of Normal / Fast was showing; Checker and Distortion are views of their own.
        const int background = std::clamp(int(std::lround(value)), 0, 2);
        apply_view_mode(background == 1 ? 2 : background == 2 ? 3 : (m_use_shaded_preview ? 1 : 0));
        return;
    }

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
    else if (cmd == int(Command::PickTexture)) {
        // The same texture library the layer card opens; the panel renders it this frame.
        m_picker_slot            = m_active_layer_slot;
        m_picker_open_request    = true;
        m_picker_reveal_selected = true;
    } else if (cmd == int(Command::Unwrap)) {
        // The solve runs only on this - painting, the seam angle and seam marking never trigger it.
        m_uv_unwrap_pending      = true;
        m_uv_apply_connected_net = true; // a genuine re-unwrap may relayout the islands
        m_show_uv_editor         = true;
        update_uv_editor();
    } else if (cmd == int(Command::SetSeamAngle)) {
        const float angle = std::clamp(value, 5.f, 90.f);
        if (angle != layer->lscm_seam_angle_deg) {
            layer->lscm_seam_angle_deg = angle;
            rebuild_preview(); // the bake unwraps with it; this also marks the pane's unwrap stale
        }
    } else if (cmd == int(Command::SetConnectIslands)) {
        const bool connect = value != 0.f;
        if (connect != layer->auto_connect_islands) {
            layer->auto_connect_islands = connect;
            // Apply (or, when turned off, just stop re-applying) right away rather than waiting for the next re-unwrap.
            if (connect && !m_uv_editor_unwrap.empty()) {
                std::vector<TextureIsland> net = compute_connected_net(m_uv_editor_unwrap);
                if (net.size() == size_t(m_uv_editor_unwrap.chart_count)) {
                    if (layer->islands.size() < net.size())
                        layer->islands.resize(net.size());
                    for (size_t i = 0; i < net.size(); ++i)
                        layer->islands[i] = net[i];
                }
            }
            rebuild_preview();
        }
    } else if (cmd == int(Command::SetSelectMode)) {
        m_uv_select_mode = std::clamp(int(std::lround(value)), 0, 2); // the canvas has already switched
    } else if (cmd == int(Command::SetMarkSeams)) {
        m_seam_edit_mode = value != 0.f;
        if (m_seam_edit_mode)
            m_adjust_texture_mode = false; // the two click modes are mutually exclusive
        else {
            m_seam_hover_edge   = { -1, -1 }; // drop the hover highlight when leaving the mode
            m_seam_hover_vertex = -1;
            m_seam_hover_glmodel.reset();
            m_seam_path_anchor = -1;
            m_seam_anchor_glmodel.reset();
        }
        push_uv_pane_state();
    } else if (cmd == int(Command::SetSeamPath)) {
        m_seam_path_mode    = value != 0.f;
        m_seam_path_anchor  = -1;
        m_seam_hover_edge   = { -1, -1 }; // hover target type changes with the mode
        m_seam_hover_vertex = -1;
        m_seam_hover_glmodel.reset();
        m_seam_anchor_glmodel.reset();
        push_uv_pane_state();
    } else if (cmd == int(Command::ClearSeams)) {
        if (!layer->lscm_seam_edges.empty()) {
            Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Clear texture seams"), UndoRedo::SnapshotType::GizmoAction);
            layer->lscm_seam_edges.clear();
            rebuild_preview();
        }
    } else if (cmd == int(Command::ClearUVEdits)) {
        if (!layer->lscm_uv_overrides.empty()) {
            Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Clear texture UV edits"), UndoRedo::SnapshotType::GizmoAction);
            layer->lscm_uv_overrides.clear();
            m_uv_unwrap_pending = true; // re-solve so the pane drops the edited coords
            update_uv_editor();
            rebuild_preview();
        }
    }
    // FrameAll/ToggleSnap are handled inside the canvas; ProjectFromView is not wired yet. SelectionChanged
    // only asks for this frame: follow_island_highlight() and the overlay read the selection back themselves.
    m_parent.set_as_dirty();
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
        // Set up the GPU drag: bake the mesh once (via the dirty flag), which is also what flags the
        // moved islands' triangles. From then on the drag is a uniform update, no rebuild - see
        // render_shaded_preview_mesh().
        m_shaded_active_chart = island;
        m_shaded_island_delta  = Eigen::Matrix<float, 2, 3>::Identity();
        m_shaded_preview_dirty = true;
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
        m_shaded_active_chart  = -1;
        m_shaded_active_face.clear();
        m_island_move_set.clear();
        m_shaded_island_delta  = Eigen::Matrix<float, 2, 3>::Identity();
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
        // rebuild. delta = F_current * F_baked^-1 in final-uv space. 
        // The one rebuild that bakes the flags is
        // scheduled at drag start above and consumed once per frame by render_painter_gizmo().
        if (m_use_shaded_preview && m_shaded_active_chart == island && size_t(island) < xf.size()) {
            Eigen::Matrix3f cur = Eigen::Matrix3f::Identity();
            cur.topRows<2>()    = xf[size_t(island)];
            Eigen::Matrix3f bak = Eigen::Matrix3f::Identity();
            bak.topRows<2>()    = m_shaded_baked_active_xf;
            m_shaded_island_delta = (cur * bak.inverse()).topRows<2>();
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

        // Keyed by this one unwrapped copy, not by its mesh vertex: a seam vertex has a copy in each island it
        // borders, and only the one that was dragged may move. Small list, linear scan is fine.
        const int key = lscm_uv_override_key(unwrapped);
        auto it = std::find_if(layer->lscm_uv_overrides.begin(), layer->lscm_uv_overrides.end(),
                               [key](const std::pair<int, Vec2f> &p) { return p.first == key; });
        if (it != layer->lscm_uv_overrides.end())
            it->second = raw_uv;
        else
            layer->lscm_uv_overrides.emplace_back(key, raw_uv);
    }

    rebuild_preview(); // the baked displacement samples the moved uv now
}

} // namespace Slic3r::GUI
