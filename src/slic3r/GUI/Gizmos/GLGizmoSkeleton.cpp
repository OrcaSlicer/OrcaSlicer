#include "GLGizmoSkeleton.hpp"
#include <boost/format.hpp>
#include "slic3r/GUI/Jobs/Worker.hpp"
#include "slic3r/GUI/Jobs/SkeletonJob.hpp" // SkeletonJobState is only forward-declared in the header
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/CameraUtils.hpp"

#include <glad/gl.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <queue>

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Gizmos/GizmoObjectManipulation.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/Utils/UndoRedo.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"
#include "libslic3r/SkeletonPipeline.hpp"
#include "libslic3r/SkeletonBuild.hpp"
#include "imgui/imgui_internal.h"
#include "slic3r/GUI/Field.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/Tab.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include <iostream>

namespace Slic3r { namespace GUI {

// Convenience: returns the per-session skeleton registry owned by Plater.
static SkeletonRegistry& skel_registry()
{
    return wxGetApp().plater()->skeleton_registry();
}

// UNLAYERED_MOD — see the header.
bool GLGizmoSkeleton::on_is_activable() const
{
    return nonplanar_printing_enabled() && GLGizmoCut3D::on_is_activable();
}

void GLGizmoSkeleton::on_set_state()
{
    GLGizmoCut3D::on_set_state();
    // Suppress the single-plane orange/teal split — we render per-edge discs instead.
    m_parent.set_use_color_clip_plane(false);
}

void GLGizmoSkeleton::on_render()
{
    GLGizmoCut3D::on_render();
    render_skeleton_cut_planes();
}


void GLGizmoSkeleton::precompute_cut_contours()
{
    m_cut_contours_model.reset();

    if (!m_c || !m_c->selection_info())
        return;
    const ModelObject* mo = m_c->selection_info()->model_object();
    if (!mo)
        return;

    const int n = m_skeleton_dag.num_nodes();
    if (n == 0)
        return;

    // Same mesh the skeleton was built from. raw_mesh() applies each volume's own matrix —
    // merging vol->mesh() directly, as this used to, misplaces every transformed volume and
    // the preview contours then disagree with the cuts.
    const indexed_triangle_set merged = mo->raw_mesh().its;
    if (merged.vertices.empty())
        return;

    GLModel::Geometry geo;
    geo.format = {GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3};

    for (int u = 0; u < n; ++u) {
        for (int v : m_skeleton.adj[u]) {
            if (v <= u) continue;

            const Vec3d pa  = m_skeleton_dag.nodes[u];
            const Vec3d ch  = m_skeleton_dag.nodes[v];
            const Vec3d mid = (pa + ch) * 0.5;
            const Eigen::Vector3d tang = m_skel_tangents.get(u, v);
            const Vec3d dir = tang.normalized();
            const float height_mesh = static_cast<float>(dir.dot(mid));

            MeshSlicingParams params;
            params.trafo.rotate(Eigen::Quaterniond::FromTwoVectors(dir, Vec3d::UnitZ()));

            const Polygons polys = slice_mesh(merged, height_mesh, params);
            if (polys.empty())
                continue;

            const Transform3d inv_rot = Transform3d(Eigen::Quaterniond::FromTwoVectors(Vec3d::UnitZ(), dir));

            // Compute centroids to filter stray polygons far from the edge midpoint.
            std::vector<Vec3d> centroids;
            centroids.reserve(polys.size());
            for (const Polygon& poly : polys) {
                Vec3d c = Vec3d::Zero();
                for (const Point& pt : poly.points) {
                    const Vec2d p2 = unscale(pt);
                    c += inv_rot * Vec3d(p2.x(), p2.y(), (double)height_mesh);
                }
                centroids.push_back(poly.points.empty() ? c : c / (double)poly.points.size());
            }
            double min_dist = std::numeric_limits<double>::max();
            for (const Vec3d& c : centroids)
                min_dist = std::min(min_dist, (c - mid).norm());
            const double threshold = min_dist * 2.0 + 1.0;

            for (size_t pi = 0; pi < polys.size(); ++pi) {
                const Polygon& poly = polys[pi];
                if (poly.points.size() < 2 || (centroids[pi] - mid).norm() > threshold)
                    continue;
                const unsigned int base_idx = (unsigned int)geo.vertices_count();
                for (const Point& pt : poly.points) {
                    const Vec2d p2 = unscale(pt);
                    const Vec3d p3 = inv_rot * Vec3d(p2.x(), p2.y(), (double)height_mesh);
                    geo.add_vertex(Vec3f(p3.x(), p3.y(), p3.z()));
                }
                const unsigned int n_pts = (unsigned int)poly.points.size();
                for (unsigned int i = 0; i < n_pts; ++i)
                    geo.add_line(base_idx + i, base_idx + (i + 1) % n_pts);
            }
        }
    }

    if (geo.vertices_count() > 0) {
        m_cut_contours_model.init_from(std::move(geo));
        m_cut_contours_model.set_color({1.f, 1.f, 1.f, 1.0f});
    }
}

void GLGizmoSkeleton::rebuild_step_contour()
{
    m_step_contour_model.reset();
    if (m_preview_step < 0 || m_preview_step >= (int)m_preview_edges.size())
        return;
    if (!m_c || !m_c->selection_info())
        return;
    const ModelObject* mo = m_c->selection_info()->model_object();
    if (!mo)
        return;

    indexed_triangle_set merged;
    for (const ModelVolume* vol : mo->volumes) {
        if (!vol->is_model_part())
            continue;
        const int base  = (int)merged.vertices.size();
        const auto& its = vol->mesh().its;
        merged.vertices.insert(merged.vertices.end(), its.vertices.begin(), its.vertices.end());
        for (auto f : its.indices)
            merged.indices.push_back({f.x() + base, f.y() + base, f.z() + base});
    }
    if (merged.vertices.empty())
        return;

    const auto& [par, child] = m_preview_edges[m_preview_step];
    const Vec3d pa  = m_skeleton_dag.nodes[par];
    const Vec3d ch  = m_skeleton_dag.nodes[child];
    const Vec3d mid = (pa + ch) * 0.5;
    const Vec3d dir = m_skel_tangents.get(par, child).normalized();
    const float height_mesh = static_cast<float>(dir.dot(mid));

    MeshSlicingParams params;
    params.trafo.rotate(Eigen::Quaterniond::FromTwoVectors(dir, Vec3d::UnitZ()));
    const Polygons polys = slice_mesh(merged, height_mesh, params);
    if (polys.empty())
        return;

    const Transform3d inv_rot = Transform3d(Eigen::Quaterniond::FromTwoVectors(Vec3d::UnitZ(), dir));

    std::vector<Vec3d> centroids;
    centroids.reserve(polys.size());
    for (const Polygon& poly : polys) {
        Vec3d c = Vec3d::Zero();
        for (const Point& pt : poly.points) {
            const Vec2d p2 = unscale(pt);
            c += inv_rot * Vec3d(p2.x(), p2.y(), (double)height_mesh);
        }
        centroids.push_back(poly.points.empty() ? c : c / (double)poly.points.size());
    }
    double min_dist = std::numeric_limits<double>::max();
    for (const Vec3d& c : centroids)
        min_dist = std::min(min_dist, (c - mid).norm());
    const double threshold = min_dist * 2.0 + 1.0;

    GLModel::Geometry geo;
    geo.format = {GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3};
    for (size_t pi = 0; pi < polys.size(); ++pi) {
        const Polygon& poly = polys[pi];
        if (poly.points.size() < 2 || (centroids[pi] - mid).norm() > threshold)
            continue;
        const unsigned int base_idx = (unsigned int)geo.vertices_count();
        for (const Point& pt : poly.points) {
            const Vec2d p2 = unscale(pt);
            const Vec3d p3 = inv_rot * Vec3d(p2.x(), p2.y(), (double)height_mesh);
            geo.add_vertex(Vec3f(p3.x(), p3.y(), p3.z()));
        }
        const unsigned int n_pts = (unsigned int)poly.points.size();
        for (unsigned int i = 0; i < n_pts; ++i)
            geo.add_line(base_idx + i, base_idx + (i + 1) % n_pts);
    }
    if (geo.vertices_count() > 0) {
        m_step_contour_model.init_from(std::move(geo));
        m_step_contour_model.set_color({1.f, 0.5f, 0.f, 1.f});
    }
}

void GLGizmoSkeleton::render_skeleton_cut_planes()
{
    if (!m_c || !m_c->selection_info())
        return;
    const ModelObject* mo = m_c->selection_info()->model_object();
    if (!mo)
        return;

    const int inst_id       = m_c->selection_info()->get_active_instance();
    if (inst_id < 0 || size_t(inst_id) >= mo->instances.size()) // UNLAYERED_MOD: no active instance
        return;
    const Transform3d world = mo->instances[inst_id]->get_matrix();
    const Camera& camera    = wxGetApp().plater()->get_camera();

    glsafe(::glDisable(GL_DEPTH_TEST));

    // Render cut contour lines (if computed).
    if (m_cut_contours_model.is_initialized()) {
        GLShaderProgram* shader = wxGetApp().get_shader("flat");
        if (shader) {
            shader->start_using();
            shader->set_uniform("projection_matrix", camera.get_projection_matrix());
            shader->set_uniform("view_model_matrix", camera.get_view_matrix() * world);
            glsafe(::glLineWidth(1.5f));
            m_cut_contours_model.render();
            shader->stop_using();
        }
    }

    // Render XYZ axis gizmo on the selected node (suppressed in connect mode).
    if (m_skel_tool != SkelTool::Connect && m_selected_node >= 0 && m_selected_node < m_skeleton.num_nodes()) {
        const Vec3d node_world = world * m_skeleton.nodes[m_selected_node];
        const float arrow_len  = 30.f * INV_ZOOM;
        render_node_axis_gizmo(node_world, arrow_len, camera);
    }

    // Highlight the connect-source node with a bright ring while in connect mode.
    if (m_skel_tool == SkelTool::Connect && m_connect_source >= 0 && m_connect_source < m_skeleton.num_nodes()) {
        GLShaderProgram* shader = wxGetApp().get_shader("flat");
        if (shader) {
            shader->start_using();
            shader->set_uniform("projection_matrix", camera.get_projection_matrix());
            shader->set_uniform("view_model_matrix", camera.get_view_matrix());

            const Vec3d src_world = world * m_skeleton.nodes[m_connect_source];
            GLModel::Geometry pt;
            pt.format = {GLModel::Geometry::EPrimitiveType::Points, GLModel::Geometry::EVertexLayout::P3};
            pt.color  = {1.f, 1.f, 0.f, 1.f};
            pt.add_vertex(Vec3f(src_world.x(), src_world.y(), src_world.z()));
            pt.add_index(0u);
            GLModel src_dot;
            src_dot.init_from(std::move(pt));
            glsafe(::glPointSize(14.f));
            src_dot.render();

            shader->stop_using();
        }
    }

    // Render highlighted contour for the current step-through preview edge.
    if (m_step_contour_model.is_initialized()) {
        GLShaderProgram* shader = wxGetApp().get_shader("flat");
        if (shader) {
            shader->start_using();
            shader->set_uniform("projection_matrix", camera.get_projection_matrix());
            shader->set_uniform("view_model_matrix", camera.get_view_matrix() * world);
            glsafe(::glLineWidth(3.0f));
            m_step_contour_model.render();
            glsafe(::glLineWidth(1.5f));
            shader->stop_using();
        }
    }

    // Render per-edge tangent direction arrows when "Show direction" is enabled.
    if (m_skel_show_direction && m_skeleton_dag.num_nodes() > 0 && !m_skel_tangents.edges.empty()) {
        GLShaderProgram* shader = wxGetApp().get_shader("flat");
        if (shader) {
            shader->start_using();
            shader->set_uniform("projection_matrix", camera.get_projection_matrix());
            shader->set_uniform("view_model_matrix", camera.get_view_matrix() * world);

            GLModel::Geometry geo;
            geo.format = {GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3};
            geo.color  = {1.f, 0.75f, 0.f, 1.f};

            const int n = m_skeleton_dag.num_nodes();
            for (int u = 0; u < n; ++u) {
                for (int v : m_skeleton.adj[u]) {
                    if (v <= u) continue;
                    const Vec3d pa   = m_skeleton_dag.nodes[u];
                    const Vec3d ch   = m_skeleton_dag.nodes[v];
                    const Vec3d mid  = (pa + ch) * 0.5;
                    const Vec3d tang = m_skel_tangents.get(u, v).normalized();

                    const double shaft = (ch - pa).norm() * 0.4;
                    const Vec3d  tail  = mid - tang * shaft * 0.5;
                    const Vec3d  tip   = mid + tang * shaft * 0.5;

                    unsigned int base = (unsigned int)geo.vertices_count();
                    geo.add_vertex(Vec3f(tail.x(), tail.y(), tail.z()));
                    geo.add_vertex(Vec3f(tip.x(),  tip.y(),  tip.z()));
                    geo.add_line(base, base + 1);

                    // Arrowhead — two short lines in the plane of tang and a perpendicular.
                    Vec3d perp = tang.cross(Vec3d::UnitZ());
                    if (perp.norm() < 1e-6) perp = tang.cross(Vec3d::UnitX());
                    perp.normalize();
                    const double hw   = shaft * 0.12;
                    const double hl   = shaft * 0.25;
                    const Vec3d  hb   = tip - tang * hl;
                    base = (unsigned int)geo.vertices_count();
                    geo.add_vertex(Vec3f(tip.x(), tip.y(), tip.z()));
                    geo.add_vertex(Vec3f((hb + perp * hw).x(), (hb + perp * hw).y(), (hb + perp * hw).z()));
                    geo.add_vertex(Vec3f((hb - perp * hw).x(), (hb - perp * hw).y(), (hb - perp * hw).z()));
                    geo.add_line(base, base + 1);
                    geo.add_line(base, base + 2);
                }
            }

            if (geo.vertices_count() > 0) {
                GLModel arrows;
                arrows.init_from(std::move(geo));
                glsafe(::glLineWidth(2.0f));
                arrows.render();
                glsafe(::glLineWidth(1.5f));
            }
            shader->stop_using();
        }
    }

    glsafe(::glEnable(GL_DEPTH_TEST));
}

void GLGizmoSkeleton::sync_skeleton_cache()
{
    if (!m_c || !m_c->selection_info() || !m_c->selection_info()->model_object())
        return;
    const ObjectID obj_id = m_c->selection_info()->model_object()->id();
    MeshSkeleton& ms = skel_registry()[obj_id];
    ms.source_id = obj_id;
    ms.graph     = m_skeleton;
    ms.dag       = m_skeleton_dag;
    ms.sync_from_graph();
}

void GLGizmoSkeleton::data_changed(bool is_serializing)
{
    GLGizmoCut3D::data_changed(is_serializing);
    if (!is_serializing && m_c && m_c->selection_info() && m_c->selection_info()->model_object()) {
        const ObjectID obj_id = m_c->selection_info()->model_object()->id();
        auto& reg = skel_registry();
        auto it = reg.find(obj_id);
        if (it != reg.end()) {
            m_skeleton     = it->second.graph;
            m_skeleton_dag = it->second.dag;
            rebuild_skeleton_dag();
            rebuild_skeleton_gl_models();
        } else {
            compute_skeleton();
            MeshSkeleton& ms = reg[obj_id];
            ms.source_id = obj_id;
            ms.graph     = m_skeleton;
            ms.dag       = m_skeleton_dag;
            ms.sync_from_graph();
        }
        precompute_cut_contours();
    }
}

// Returns the screen-space position of a world point.
static Vec2d project_to_screen(const Vec3d& world_pt, const Camera& camera)
{
    const auto& vp   = camera.get_viewport();
    const Vec4d ndc  = camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix()
                       * Vec4d(world_pt.x(), world_pt.y(), world_pt.z(), 1.0);
    if (ndc.w() == 0.0) return {-1e9, -1e9};
    return {
        (ndc.x() / ndc.w() * 0.5 + 0.5) * vp[2] + vp[0],
        (1.0 - (ndc.y() / ndc.w() * 0.5 + 0.5)) * vp[3] + vp[1]
    };
}

bool GLGizmoSkeleton::on_mouse(const wxMouseEvent& evt)
{
    if (m_skeleton.num_nodes() == 0)
        return false;
    if (!m_c || !m_c->selection_info())
        return false;
    const ModelObject* mo = m_c->selection_info()->model_object();
    if (!mo)
        return false;

    const int inst_id       = m_c->selection_info()->get_active_instance();
    if (inst_id < 0 || size_t(inst_id) >= mo->instances.size()) // UNLAYERED_MOD: no active instance
        return false;
    const Transform3d world = mo->instances[inst_id]->get_matrix();
    const Camera& camera    = wxGetApp().plater()->get_camera();
    const Vec2d mouse_pos(evt.GetX(), evt.GetY());

    const float arrow_len = 30.f * INV_ZOOM; // matches move gizmo scale

    if (evt.RightDown()) {
        // Right-click selects nearest node and queues context menu.
        int best = -1;
        double best_sq = 12.0 * 12.0;
        for (int i = 0; i < m_skeleton.num_nodes(); ++i) {
            const Vec2d sp = project_to_screen(world * m_skeleton.nodes[i], camera);
            const double d = (sp - mouse_pos).squaredNorm();
            if (d < best_sq) { best_sq = d; best = i; }
        }
        if (best >= 0) {
            m_selected_node    = best;
            m_node_ctx_pending = true;
            return true;
        }
        return false;
    }

    if (evt.LeftDown()) {
        // Connect tool: first click picks source, second click picks target and adds edge.
        if (m_skel_tool == SkelTool::Connect) {
            int best = -1;
            double best_sq = 12.0 * 12.0;
            for (int i = 0; i < m_skeleton.num_nodes(); ++i) {
                if (i == m_connect_source) continue;
                const Vec2d sp = project_to_screen(world * m_skeleton.nodes[i], camera);
                const double d = (sp - mouse_pos).squaredNorm();
                if (d < best_sq) { best_sq = d; best = i; }
            }
            if (best >= 0) {
                if (m_connect_source < 0) {
                    m_connect_source = best; // source selected — awaiting target
                } else {
                    // Add edge if it doesn't already exist.
                    auto& adj_src = m_skeleton.adj[m_connect_source];
                    if (std::find(adj_src.begin(), adj_src.end(), best) == adj_src.end()) {
                        m_skeleton.adj[m_connect_source].push_back(best);
                        m_skeleton.adj[best].push_back(m_connect_source);
                        rebuild_skeleton_dag();
                        rebuild_skeleton_gl_models();
                        precompute_cut_contours();
                        sync_skeleton_cache();
                    }
                    m_connect_source = -1; // reset for next pair; stay in Connect mode
                }
            }
            return true;
        }

        // Delete tool: click nearest node to remove it.
        if (m_skel_tool == SkelTool::Delete) {
            int best = -1;
            double best_sq = 12.0 * 12.0;
            for (int i = 0; i < m_skeleton.num_nodes(); ++i) {
                const Vec2d sp = project_to_screen(world * m_skeleton.nodes[i], camera);
                const double d = (sp - mouse_pos).squaredNorm();
                if (d < best_sq) { best_sq = d; best = i; }
            }
            if (best >= 0) {
                if (m_selected_node == best) m_selected_node = -1;
                delete_skeleton_node(best);
                return true;
            }
            return false;
        }

        // Move tool (default): check axis arrow tips of selected node first.
        if (m_selected_node >= 0 && m_selected_node < m_skeleton.num_nodes()) {
            const Vec3d node_world = world * m_skeleton.nodes[m_selected_node];
            const Vec3d axes[3] = {Vec3d::UnitX(), Vec3d::UnitY(), Vec3d::UnitZ()};

            for (int ax = 0; ax < 3; ++ax) {
                const Vec3d tip = node_world + axes[ax] * arrow_len;
                const Vec2d sp  = project_to_screen(tip, camera);
                if ((sp - mouse_pos).squaredNorm() < 12.0 * 12.0) {
                    m_drag_axis              = ax;
                    m_drag_axis_dir          = axes[ax];
                    m_drag_node_world_start  = node_world;
                    m_drag_grabber_world_start = tip;
                    return true;
                }
            }
        }

        // Select closest node within 12 px.
        int best = -1;
        double best_sq = 12.0 * 12.0;
        for (int i = 0; i < m_skeleton.num_nodes(); ++i) {
            const Vec2d sp = project_to_screen(world * m_skeleton.nodes[i], camera);
            const double d = (sp - mouse_pos).squaredNorm();
            if (d < best_sq) { best_sq = d; best = i; }
        }
        if (best >= 0) {
            m_selected_node = best;
            m_drag_axis     = -1;
            return true;
        }

        // Clicked empty space — deselect.
        m_selected_node = -1;
        return false;
    }

    if (evt.Dragging() && m_drag_axis >= 0) {
        Vec3d ray_pt, ray_dir;
        CameraUtils::ray_from_screen_pos(camera, mouse_pos, ray_pt, ray_dir);
        // Perspective camera returns unnormalized direction; normalize before use.
        ray_dir.normalize();

        {
            // Foot-of-perpendicular from grabber start to current mouse ray,
            // then project displacement onto the drag axis (same as GLGizmoMove3D::calc_projection).
            const Vec3d inters     = ray_pt + ((m_drag_grabber_world_start - ray_pt).dot(ray_dir)) * ray_dir;
            const double disp      = (inters - m_drag_grabber_world_start).dot(m_drag_axis_dir);
            const Vec3d new_world  = m_drag_node_world_start + disp * m_drag_axis_dir;
            const Vec3d new_local  = world.inverse() * new_world;
            m_skeleton.nodes[m_selected_node] = new_local;
            if (m_selected_node < (int) m_skeleton_dag.nodes.size())
                m_skeleton_dag.nodes[m_selected_node] = new_local;
            rebuild_skeleton_gl_models();
        }
        return true;
    }

    if (evt.LeftUp() && m_drag_axis >= 0) {
        m_drag_axis = -1;
        // The planes follow the moved node (they used to keep the pre-drag tangents).
        rebuild_skeleton_dag();
        precompute_cut_contours();
        sync_skeleton_cache();
        return true;
    }

    return false;
}

void GLGizmoSkeleton::render_node_axis_gizmo(const Vec3d& node_world, float arrow_len, const Camera& camera)
{
    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (!shader) return;

    shader->start_using();
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    shader->set_uniform("view_model_matrix", camera.get_view_matrix()); // vertices in world space

    glsafe(::glLineWidth(2.5f));

    const Vec3d axes[3]  = {Vec3d::UnitX(), Vec3d::UnitY(), Vec3d::UnitZ()};
    const ColorRGBA cols[3] = {{1.f,0.f,0.f,1.f}, {0.f,1.f,0.f,1.f}, {0.f,0.5f,1.f,1.f}};

    for (int ax = 0; ax < 3; ++ax) {
        const Vec3d tip = node_world + axes[ax] * arrow_len;

        const bool hovered = (m_drag_axis == ax);
        const ColorRGBA& col = hovered ? ColorRGBA{1.f, 1.f, 1.f, 1.f} : cols[ax];

        GLModel::Geometry geo;
        geo.format = {GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3};
        geo.color  = col;
        geo.add_vertex(Vec3f(node_world.x(), node_world.y(), node_world.z()));
        geo.add_vertex(Vec3f(tip.x(), tip.y(), tip.z()));
        geo.add_line(0, 1);
        GLModel arrow;
        arrow.init_from(std::move(geo));
        arrow.render();

    }

    // Draw a white point at each axis tip so the pick target is visible.
    GLModel::Geometry pts;
    pts.format = {GLModel::Geometry::EPrimitiveType::Points, GLModel::Geometry::EVertexLayout::P3};
    pts.color  = {1.f, 1.f, 1.f, 1.f};
    for (int ax = 0; ax < 3; ++ax) {
        const Vec3d tip = node_world + axes[ax] * arrow_len;
        pts.add_vertex(Vec3f(tip.x(), tip.y(), tip.z()));
        pts.add_index((unsigned int) ax);
    }
    GLModel tips_model;
    tips_model.init_from(std::move(pts));
    glsafe(::glPointSize(8.f));
    tips_model.render();

    shader->stop_using();
}

// Builds the ordered list of (parent, child) DAG edges to cut.
// Uses a true root-first BFS over the skeleton DAG: each DAG root seeds its own
// queue and they are round-robin interleaved, so parallel branches are zigzagged
// rather than Z-sorted (which is unreliable on complex meshes).
std::vector<std::pair<int,int>> GLGizmoSkeleton::build_cut_edges(const Transform3d& /*world*/) const
{
    // Derived by rebuild_skeleton_topology(), shared with the headless pipeline, so
    // `--export-skeleton`'s cut_edges list is the very order this gizmo will cut in.
    return m_skel_cut_edges;
}

// Object-local plane through the skeleton edge midpoint, facing the smoothed tangent (the
// parent->child print direction). The tangent IS the far side's build direction: the declarative
// pipeline derives the build normal from the plane normal, so no build_normal override is set --
// an override would freeze the direction against later edits of the plane in the Cut Planes
// gizmo. Skeleton nodes and tangents are object-local, which is exactly the frame CutPlane stores.
Slic3r::CutPlane GLGizmoSkeleton::make_edge_plane(int par, int child) const
{
    const SkeletonCutPlane sp = skeleton_edge_cut_plane(m_skeleton_dag, m_skel_tangents, par, child);
    Slic3r::CutPlane cp;
    cp.point  = sp.point;
    cp.normal = sp.normal;
    return cp;
}


// One-time prompt mirrored from GLGizmoCutPlanes::add_plane(): declarative cut planes only take
// effect under the "By group" print sequence.
void GLGizmoSkeleton::prompt_by_group_sequence()
{
    if (wxGetApp().global_print_sequence() == PrintSequence::ByGroup)
        return;
    MessageDialog dialog(
        wxGetApp().plater(),
        _L("Cut-plane printing requires the \"By group\" print sequence. Change the current process setting to \"By group\" now?"),
        _L("Change print sequence"), wxICON_QUESTION | wxYES_NO | wxYES_DEFAULT);
    if (dialog.ShowModal() == wxID_YES) {
        DynamicPrintConfig config = wxGetApp().preset_bundle->prints.get_edited_preset().config;
        config.set_key_value("print_sequence", new ConfigOptionEnum<PrintSequence>(PrintSequence::ByGroup));
        wxGetApp().get_tab(Preset::TYPE_PRINT)->load_config(config);
    }
}

// Author one declarative cut plane per skeleton edge on the selected object -- the data the Cut
// Planes gizmo edits -- instead of destructively splitting the object. The object stays whole;
// the partition, per-segment build directions and print order all derive at slice time, and
// every plane remains editable afterwards.
void GLGizmoSkeleton::perform_skeleton_cut(const Selection& selection)
{
    if (m_skeleton_dag.num_nodes() == 0)
        return;

    // Keep the cache holding the user's edited skeleton so data_changed() restores this version.
    sync_skeleton_cache();

    const int instance_idx = selection.get_instance_idx();
    const int object_idx   = selection.get_object_idx();
    if (instance_idx < 0 || object_idx < 0)
        return;

    Plater*      plater = wxGetApp().plater();
    ModelObject* mo     = plater->model().objects[object_idx];
    if (mo == nullptr)
        return;

    const std::vector<std::pair<int,int>> edges = build_cut_edges(Transform3d::Identity());
    if (edges.empty())
        return;

    // The segmentation's arrangement mask is a uint64_t: at most 64 enabled planes per object.
    if (mo->cut_planes.planes.size() + edges.size() > 64) {
        MessageDialog(plater,
                      format_wxstr(_L("This centreline would add %1% cut planes (the object already has %2%); "
                                      "at most 64 cut planes are supported per object."),
                                   edges.size(), mo->cut_planes.planes.size()),
                      _L("Too many cut planes"), wxICON_WARNING | wxOK).ShowModal();
        return;
    }

    Plater::TakeSnapshot snapshot(plater, _u8L("Cut along pipe"), UndoRedo::SnapshotType::GizmoAction);

    const bool was_empty = mo->cut_planes.planes.empty();
    for (const auto& [par, child] : edges)
        mo->cut_planes.planes.push_back(make_edge_plane(par, child));
    if (was_empty) {
        mo->cut_planes.enabled = true; // authoring the first planes implies the user wants the feature on
        prompt_by_group_sequence();
    }

    m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    m_parent.request_extra_frame();

    // Land the user in the Cut Planes gizmo to review the authored list. Deferred out of this
    // ImGui callback: open_gizmo() closes the current (Skeleton) gizmo, and closing it from
    // inside its own render would re-enter a dying gizmo (same reason GLGizmoEmboss defers).
    plater->CallAfter([]() {
        GLGizmosManager& mgr = wxGetApp().plater()->get_view3D_canvas3D()->get_gizmos_manager();
        if (mgr.get_current_type() != GLGizmosManager::EType::CutPlanes)
            mgr.open_gizmo(GLGizmosManager::EType::CutPlanes);
    });
}


void GLGizmoSkeleton::rebuild_skeleton_dag()
{
    if (m_skeleton.num_nodes() == 0) {
        m_skeleton_dag   = SkeletonDAG{};
        m_skel_tangents  = SkeletonTangents{};
        m_skel_cut_edges.clear();
        set_root_markers({});
        return;
    }
    // In world space, through the same function build_skeleton() ends with. This used to find the
    // roots and force the root planes along the OBJECT-LOCAL Z: on an instance rotated to stand the
    // part up (Intake_manifold, 90 deg about X) any node edit re-rooted the skeleton at the flange
    // tops and laid the "flat" first plane along a horizontal world axis.
    const ModelObject* mo = m_c && m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    Transform3d world = Transform3d::Identity();
    if (mo != nullptr) {
        const int inst = m_c->selection_info()->get_active_instance();
        if (inst >= 0 && inst < (int) mo->instances.size())
            world = mo->instances[inst]->get_matrix();
    }
    const indexed_triangle_set its = mo != nullptr ? mo->raw_mesh().its : indexed_triangle_set{};
    SkeletonTopology t = rebuild_skeleton_topology(m_skeleton, world, its, skeleton_params());
    m_skeleton_dag    = std::move(t.dag);
    m_skel_tangents   = std::move(t.tangents);
    m_skel_cut_edges  = std::move(t.cut_edges);
    m_skel_conv_count = (int) t.convergence_nodes.size(); // node edits change this; the panel reads the cache
    set_root_markers(t.roots);
}

void GLGizmoSkeleton::set_root_markers(const std::vector<int>& roots)
{
    GLModel::Geometry roots_geo;
    roots_geo.format = {GLModel::Geometry::EPrimitiveType::Points, GLModel::Geometry::EVertexLayout::P3};
    for (int idx : roots) {
        if (idx < 0 || idx >= m_skeleton.num_nodes())
            continue;
        const auto& rn = m_skeleton.nodes[idx];
        roots_geo.add_vertex(Vec3f(rn.x(), rn.y(), rn.z()));
        roots_geo.add_index((unsigned int) roots_geo.vertices_count() - 1);
    }
    m_skeleton_roots_model.reset();
    if (roots_geo.vertices_count() > 0) {
        m_skeleton_roots_model.init_from(std::move(roots_geo));
        m_skeleton_roots_model.set_color({0.f, 1.f, 0.f, 1.f}); // green root nodes
    }
}

void GLGizmoSkeleton::delete_skeleton_node(int idx)
{
    if (idx < 0 || idx >= m_skeleton.num_nodes()) return;

    m_skeleton.nodes.erase(m_skeleton.nodes.begin() + idx);
    m_skeleton.adj.erase(m_skeleton.adj.begin() + idx);

    for (auto& nbrs : m_skeleton.adj) {
        nbrs.erase(std::remove(nbrs.begin(), nbrs.end(), idx), nbrs.end());
        for (int& v : nbrs)
            if (v > idx) --v;
    }

    rebuild_skeleton_dag();
    rebuild_skeleton_gl_models();
    precompute_cut_contours();
    sync_skeleton_cache();
}

void GLGizmoSkeleton::render_cut_plane_input_window(CutConnectors& connectors, float x, float y, float bottom_limit)
{
    render_build_size();

    // What the tool is for, in the first line a new user reads. "Skeleton" said nothing about
    // that, and the tool was being tried on solid parts it cannot help.
    m_imgui->text_wrapped(_L("For bent pipes, ducts and tubes: finds the bore's centreline and cuts the part "
                             "into chunks along it, so each chunk prints square to the pipe. Not for solid or "
                             "boxy parts."),
                          ImGui::GetFontSize() * 24.f);
    ImGui::Separator();

    // ── Topbar: tool buttons (Move | Delete | Connect) ── separator ── Generator ──
    {
        const float scale = m_parent.get_gizmos_manager().get_layout_scale();
        const ImVec2 btn_sz(16.f * scale, 16.f * scale);
        const float max_tip_w = ImGui::GetFontSize() * 20.f;

        struct ToolBtn {
            SkelTool    tool;
            wchar_t     icon_light;
            wchar_t     icon_dark;
            const char* tooltip;
        };
        const ToolBtn tool_btns[] = {
            { SkelTool::Move,    ImGui::SphereButtonIcon,         ImGui::SphereButtonDarkIcon,   "Move node"      },
            { SkelTool::Delete,  ImGui::CloseBlockNotifButton,    ImGui::CloseBlockNotifButton,  "Delete node"    },
            { SkelTool::Connect, ImGui::FillButtonIcon,           ImGui::FillButtonDarkIcon,     "Connect nodes"  },
        };

        auto push_btn_style = [&](bool is_active) {
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,   3.f * scale);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,    ImVec2(4.f * scale, 4.f * scale));
            // UNLAYERED_MOD — these were hardcoded: pure-white text, and the pre-grayscale teal
            // #009687 for the active fill. Both are float literals, so the accent-colour pass that
            // recoloured the hex strings walked straight past them. White text over the light
            // theme's white window made the INACTIVE tool icons invisible, the same failure as the
            // white-on-white tick. Take the text colour from the window and tint the active fill
            // from the accent so this follows the theme instead of fighting it.
            const ImVec4 accent = ImGuiWrapper::COL_ORCA;
            ImGui::PushStyleColor(ImGuiCol_Text,          ImGui::GetStyleColorVec4(ImGuiCol_Text));
            ImGui::PushStyleColor(ImGuiCol_Button,        is_active ? ImVec4(accent.x, accent.y, accent.z, .25f) : ImVec4(0,0,0,0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, is_active ? ImVec4(accent.x, accent.y, accent.z, .25f) : ImVec4(.6f,.6f,.6f,.2f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  is_active ? ImVec4(accent.x, accent.y, accent.z, .30f) : ImVec4(0,0,0,0));
            ImGui::PushStyleColor(ImGuiCol_Border,        is_active ? ImGuiWrapper::COL_ORCA     : ImVec4(0,0,0,0));
            ImGui::PushStyleColor(ImGuiCol_BorderShadow,  is_active ? ImGuiWrapper::COL_ORCA     : ImVec4(0,0,0,0));
        };
        auto pop_btn_style = []() {
            ImGui::PopStyleColor(6);
            ImGui::PopStyleVar(3);
        };

        for (int i = 0; i < 3; ++i) {
            if (i > 0) ImGui::SameLine();
            const bool is_active = (m_skel_tool == tool_btns[i].tool);
            const wchar_t icon = m_is_dark_mode ? tool_btns[i].icon_dark : tool_btns[i].icon_light;
            push_btn_style(is_active);
            ImGui::PushID(i);
            if (m_imgui->glyph_button(icon, btn_sz)) {
                if (m_skel_tool != tool_btns[i].tool) {
                    m_skel_tool      = tool_btns[i].tool;
                    m_connect_source = -1;
                } else {
                    m_skel_tool      = SkelTool::Move;
                    m_connect_source = -1;
                }
            }
            ImGui::PopID();
            pop_btn_style();
            if (ImGui::IsItemHovered())
                m_imgui->tooltip(tool_btns[i].tooltip, max_tip_w);
        }

        // Vertical separator between tool group and generator button.
        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();

        // Generator button — toggles the skeleton generation settings panel.
        {
            const wchar_t gen_icon = m_is_dark_mode ? ImGui::PreferencesDarkButton : ImGui::PreferencesButton;
            push_btn_style(m_skel_show_generator);
            ImGui::PushID("gen");
            if (m_imgui->glyph_button(gen_icon, btn_sz))
                m_skel_show_generator = !m_skel_show_generator;
            ImGui::PopID();
            pop_btn_style();
            if (ImGui::IsItemHovered())
                m_imgui->tooltip("Skeleton generator settings", max_tip_w);
        }
    }

    ImGui::Separator();

    // Node context menu (right-click on a node in the viewport).
    if (m_node_ctx_pending) {
        ImGui::OpenPopup("##skel_node_ctx");
        m_node_ctx_pending = false;
    }
    const ImVec4 popup_bg   = m_is_dark_mode ? ImGuiWrapper::COL_WINDOW_BG_DARK : ImGuiWrapper::COL_WINDOW_BG;
    const ImVec4 popup_text = m_is_dark_mode ? ImVec4(1.f, 1.f, 1.f, 0.88f)
                                             : ImVec4(38/255.f, 46/255.f, 48/255.f, 1.f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg,       popup_bg);
    ImGui::PushStyleColor(ImGuiCol_Text,           popup_text);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,  m_is_dark_mode ? ImVec4(1.f,1.f,1.f,0.1f) : ImVec4(0.f,0.f,0.f,0.08f));
    if (ImGui::BeginPopup("##skel_node_ctx")) {
        if (ImGui::MenuItem(_u8L("Delete node").c_str())) {
            delete_skeleton_node(m_selected_node);
            m_selected_node = -1;
        }
        if (ImGui::MenuItem(_u8L("Connect to...").c_str())) {
            m_connect_source = m_selected_node;
            m_skel_tool      = SkelTool::Connect;
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor(3);

    // Connect-mode status hint.
    if (m_skel_tool == SkelTool::Connect) {
        if (m_connect_source < 0)
            m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, _L("Click a node to start connecting"));
        else
            m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, _L("Click another node to connect it"));
        ImGui::Separator();
    }

    // Delete-mode status hint.
    if (m_skel_tool == SkelTool::Delete) {
        m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, _L("Click a node to delete it"));
        ImGui::Separator();
    }

    // Fallback: compute on first panel render if data_changed hasn't fired yet.
    //
    // The `!m_skel_job_state` term is essential now that the build is asynchronous. The GL model
    // stays uninitialised for the whole duration of the build, so without it this re-entered on
    // EVERY frame, and each compute_skeleton() cancels the in-flight job and starts a fresh one.
    // The build could then never finish and the worker churned indefinitely — measured at 4.3 CPU
    // hours in an app that looked idle, which starves everything else on the machine.
    if (!m_skeleton_model.is_initialized() && !m_skel_job_state &&
        m_c && m_c->selection_info() && m_c->selection_info()->model_object()) {
        const ObjectID obj_id = m_c->selection_info()->model_object()->id();
        auto& reg = skel_registry();
        auto it = reg.find(obj_id);
        if (it != reg.end()) {
            m_skeleton     = it->second.graph;
            m_skeleton_dag = it->second.dag;
            rebuild_skeleton_dag();
            rebuild_skeleton_gl_models();
        } else {
            // Async now: the registry is filled by apply_skeleton_result() when the worker
            // finishes. Reading m_skeleton here would cache the *previous* object's skeleton.
            compute_skeleton();
        }
    }

    // ── Async build status ──
    // Without this the window simply looks broken for the duration: on a 42908-face intake the
    // build takes ~2 minutes, and before it moved off the UI thread Windows greyed the whole
    // window out and logged AppHangTransient.
    if (m_skel_job_state && !m_skel_job_state->done.load()) {
        const int pct = m_skel_job_state->percent.load();
        const std::string stage = m_skel_job_state->stage_copy();
        m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT,
                              from_u8((boost::format("Finding centreline: %1%%% (%2%)") % pct % stage).str()));
        ImGui::SameLine();
        if (m_imgui->button(_L("Cancel")))
            wxGetApp().plater()->get_ui_job_worker().cancel();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Stops at the end of the current stage. Skeletonisation itself is a\n"
                              "single uninterruptible CGAL call, so on a dense mesh that can still\n"
                              "take a while to come back.");
        ImGui::Separator();
    }

    // ── Generator panel (collapsible via gear button) ──
    if (m_skel_show_generator) {
        ImGui::AlignTextToFramePadding();
        if (m_imgui->button(_L("Recompute centreline"))) {
            if (m_c && m_c->selection_info() && m_c->selection_info()->model_object())
                skel_registry().erase(m_c->selection_info()->model_object()->id());
            // Registry, contours and step preview are all refreshed by
            // apply_skeleton_result() once the worker's result lands.
            compute_skeleton();
            m_preview_step = -1;
            m_step_contour_model.reset();
        }

        ImGui::Separator();

        // Skeleton method selector.
        {
            const char* methods[] = {"MCF", "Voxel"};
            ImGui::SetNextItemWidth(120.f);
            ImGui::Combo(_L("Method").ToUTF8().data(), &m_skel_method, methods, 2);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "How the centreline is found.\n\n"
                    "MCF    shrinks the mesh onto its own centreline. Smooth curves,\n"
                    "       but slow and needs a watertight manifold mesh.\n"
                    "Voxel  fills a grid and erodes it to 1-voxel threads. Blocky and\n"
                    "       limited by Voxel resolution, but survives messy meshes.");
        }

        if (m_skel_method == 0) {
            ImGui::SetNextItemWidth(120.f);
            // UNLAYERED_MOD — the old label and tooltip had this backwards. Measured on a
            // 42908-face intake decimated to 8000: 0.1 -> 10.0 s, 0.4 -> 21.8 s. Raising it
            // costs time; it is CGAL's contraction weight, not a quality/speed tradeoff.
            ImGui::SliderFloat(_L("Contraction rate").ToUTF8().data(), &m_skel_quality_speed, 0.01f, 1.0f, "%.3f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("MCF only. How hard each contraction step pulls the mesh inward.\n"
                                  "Raising it makes the skeleton COARSER and SLOWER, not faster —\n"
                                  "measured 10s at 0.1 vs 22s at 0.4 on a 43k-face part.\n"
                                  "Leave at 0.1 unless the skeleton misses a thin feature.");
        }

        if (m_skel_method == 1) {
            ImGui::SetNextItemWidth(120.f);
            ImGui::SliderInt(_L("Voxel resolution").ToUTF8().data(), &m_skel_voxel_resolution, 32, 256);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Voxel only. The longest axis is split this many ways, so 64 on a\n"
                                  "100mm part gives ~1.6mm voxels and features thinner than that\n"
                                  "vanish. Cost is cubic in this number. Default 64.");
        }

        ImGui::SetNextItemWidth(120.f);
        ImGui::InputFloat(_L("Resample edge len").ToUTF8().data(), &m_skel_resample_edge_len, 0.0f, 0.0f, "%.2f mm");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Target edge length after resampling (mm). 0 = off.\n"
                              "Cuts land at edge midpoints, so this is also the segment height.");

        // UNLAYERED_MOD — hollow geometry. Skeletonisation contracts the material, so an open
        // tube collapses to a loop around the middle of its wall and the axis is lost; when a
        // bore is detected its centreline is traced from cross sections and used instead.
        {
            const char* modes[] = {"Off", "Threshold", "Always"};
            ImGui::SetNextItemWidth(120.f);
            ImGui::Combo(_L("Trace bores").ToUTF8().data(), &m_skel_tunnel_fill, modes, 3);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "How hollow geometry is handled:\n"
                    "Off        skeletonise the walls as-is\n"
                    "Threshold  follow the axis of deep bores (pipes, drilled holes),\n"
                    "           keep the material loop of shallow ones (rings, frames)\n"
                    "Always     follow the axis of every hole, however shallow");
        }

        // UNLAYERED_MOD — where the root stands. A part with no leg (a duct lying across the bed,
        // a U on its bend) used to be rooted at whichever end tip was lowest, so every cut grew
        // from one side; Middle hangs one foot under the middle of the contact patch instead.
        {
            const char* modes[] = {"Auto", "Middle", "Tips"};
            ImGui::SetNextItemWidth(120.f);
            ImGui::Combo(_L("Bed root").ToUTF8().data(), &m_skel_bed_root, modes, 3);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Where the first chunk stands on the bed (applies on Recompute centreline):\n"
                    "Auto    Middle when no branch runs down to the bed as a leg, else Tips\n"
                    "Middle  one root under the middle of the bed contact; the cuts fan out\n"
                    "        both ways, leaning toward the bed and squaring up to the pipe\n"
                    "Tips    every downward branch end that reaches the bed is a root");
        }

        m_imgui->checkbox(_L("Collapse to spine"), m_skel_collapse_to_spine);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Discards all branching and keeps one path from the lowest node to\n"
                              "the highest, so a branched part becomes a single chain of cuts.\n"
                              "Drastic — measured 17 nodes -> 12 on a solid T.");

        // UNLAYERED_MOD — "Junction collapse" removed from the UI: measured to do NOTHING.
        // collapse_nearby_junctions() only merges junction nodes that are DIRECTLY adjacent,
        // and skeletonization does not produce those on ordinary geometry. Swept over
        // pipe/tee/torus it moved no node and rotated no cut normal, and a direct test on the
        // solid T with resampling and smoothing disabled — so nothing downstream could mask it
        // — gave byte-identical node sets either way. m_skel_collapse_junctions stays true so
        // the stage still runs; if a mesh ever does produce adjacent junctions it will still be
        // cleaned up. Restore the checkbox only alongside a test case it actually changes.
        //
        // m_imgui->checkbox(_L("Junction collapse"), m_skel_collapse_junctions);

        // Convergence controls only bite when the DAG has a node with two or more parents,
        // which needs multiple roots. On a single-rooted part there are none and both of these
        // are no-ops, so say so rather than offering two dead controls.
        //
        // Read from the cached count, NOT by calling find_convergence_nodes() here: this runs once
        // per frame, and that function logs a line every time it is called. Recomputing it per
        // frame meant ~60 scans and 60 log lines a second for a value that only changes when the
        // skeleton does.
        const int n_conv = m_skel_conv_count;
        if (n_conv > 0) {
            if (m_imgui->checkbox(_L("Correct convergence"), m_skel_correct_convergence)) {
                rebuild_skeleton_dag();
                rebuild_step_contour();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Where branches merge, aims the cut plane along the bisector of the\n"
                                  "arriving branches instead of one branch's own direction, and rotates\n"
                                  "apart any pair that would end up facing each other.\n"
                                  "Measured 17 deg of cut-normal change on a torus.");
        } else {
            m_imgui->text_colored(ImGuiWrapper::COL_GREY_LIGHT,
                                  _L("No convergence nodes — bisector controls not applicable"));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("A convergence node is where two branches from different bed contacts\n"
                                  "merge. This part has none, so 'Correct convergence' and\n"
                                  "'Tangent blend W' would have no effect and are hidden.");
        }

        ImGui::Separator();

        // Advanced submenu toggle.
        {
            const char* adv_label = m_skel_show_advanced ? "Advanced \xe2\x96\xb2" : "Advanced \xe2\x96\xbc";
            if (m_imgui->button(wxString::FromUTF8(adv_label)))
                m_skel_show_advanced = !m_skel_show_advanced;
        }

        const int n_nodes = m_skeleton_dag.num_nodes();

        if (m_skel_show_advanced) {
            ImGui::Indent(10.f);

            ImGui::SetNextItemWidth(120.f);
            ImGui::SliderInt(_L("Smooth iters").ToUTF8().data(), &m_skel_smooth_iterations, 0, 50);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Passes of position averaging that remove skeletonisation jitter.\n"
                                  "On a closed loop this no longer shrinks the loop (Taubin pass).\n"
                                  "0 = off. Default 10");

            ImGui::SetNextItemWidth(120.f);
            ImGui::SliderFloat(_L("Smooth alpha").ToUTF8().data(), &m_skel_smooth_alpha, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("How far each smoothing pass moves a node toward its neighbours\n"
                                  "average. Above ~0.5 gains little; the loop-preserving path caps\n"
                                  "the effective value at 0.45 for stability. Default 0.5");

            if (m_skel_method == 0) {
                ImGui::SetNextItemWidth(120.f);
                ImGui::SliderFloat(_L("Min edge factor").ToUTF8().data(), &m_skel_min_edge_factor, 0.0001f, 0.02f, "%.4f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("MCF only. During contraction, edges shorter than this fraction of\n"
                                      "the bounding-box diagonal are collapsed, setting the RAW node\n"
                                      "density. Resample edge len re-spaces everything afterwards, so\n"
                                      "this mostly shows up as how much detail MCF keeps. Default 0.002");
            }

            if (m_skel_tunnel_fill == 1) {
                ImGui::SetNextItemWidth(120.f);
                ImGui::SliderFloat(_L("Bore aspect min").ToUTF8().data(), &m_skel_tunnel_min_aspect, 0.2f, 5.0f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Bore depth / bore width needed to follow the axis instead of\n"
                                      "the material loop. Measured: 60mm pipe with a 12mm bore 5.0,\n"
                                      "90deg elbow 2.8, torus 0.32, washer 0.27, windowed plate 0.18.\n"
                                      "Default 1.2");
            }

            ImGui::SetNextItemWidth(120.f);
            ImGui::SliderInt(_L("Face budget").ToUTF8().data(), &m_skel_decimate_faces, 0, 40000);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("0 = off. Above 0, the mesh is simplified to this many faces before\n"
                                  "skeletonising (bore tracing and bed snap still use full detail).\n"
                                  "Skeletonising is ~quadratic in face count, so this is the speed dial —\n"
                                  "but it CHANGES the skeleton on organic parts. Measured on a 43k-face\n"
                                  "intake: 126s at 0, 10s at 8000 but height 60mm -> 29mm, and 2000\n"
                                  "collapsed it to a single node. Raise it until it is fast enough, then\n"
                                  "check the skeleton still spans the part.");

            ImGui::SetNextItemWidth(120.f);
            ImGui::InputFloat(_L("Prune leaves").ToUTF8().data(), &m_skel_prune_leaves, 0.0f, 0.0f, "%.2f mm");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Iteratively remove branch tips whose edge is shorter than this.\n"
                                  "Clears stray skeletonisation twigs that would each become a cut.\n"
                                  "0 = off.");

            // Only shown when it can do something — see the n_conv note above.
            if (n_conv > 0) {
                ImGui::SetNextItemWidth(120.f);
                if (ImGui::SliderFloat(_L("Tangent blend W").ToUTF8().data(), &m_skel_tangent_W, 0.0f, 1.0f, "%.2f")) {
                    rebuild_skeleton_dag();
                    rebuild_step_contour();
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("On the branches leading into a merge point, blends each cut normal\n"
                                      "from the raw edge direction (0) toward the merge bisector (1).\n"
                                      "Measured up to 74 deg of change on a torus, 88 on a washer, and\n"
                                      "0.003 deg on a part with no merge point — hence only shown here.");
            }

            // How the cuts fan out of the root. Both apply live: they only re-aim normals.
            ImGui::SetNextItemWidth(120.f);
            if (ImGui::SliderFloat(_L("Root lean").ToUTF8().data(), &m_skel_root_lean, 0.0f, 90.0f, "%.0f deg")) {
                rebuild_skeleton_dag();
                precompute_cut_contours();
                rebuild_step_contour();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Middle root only. The first cuts either side of the middle are held\n"
                                  "at most 90 - this many degrees from flat, so the chunks beside the\n"
                                  "middle build up and outward, not sideways off the bed. Also sets the\n"
                                  "uncut seat over the bed contact (1.1 x centreline height x tan(lean)),\n"
                                  "which keeps the two first planes from meeting inside the part.\n"
                                  "Default 45.");

            ImGui::SetNextItemWidth(120.f);
            if (ImGui::InputFloat(_L("Root ramp").ToUTF8().data(), &m_skel_root_ramp, 0.0f, 0.0f, "%.1f mm")) {
                m_skel_root_ramp = std::max(0.0f, m_skel_root_ramp);
                rebuild_skeleton_dag();
                precompute_cut_contours();
                rebuild_step_contour();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Distance over which the lean out of the bed fades back to the pipe\n"
                                  "direction, so consecutive cuts turn in small steps instead of the\n"
                                  "first lying flat and the next snapping to the pipe.\n"
                                  "0 = auto: max(3 edge lengths, 30%% of the longest root-to-tip path).");

            m_imgui->checkbox(_L("Show direction"), m_skel_show_direction);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Draws an arrow per edge showing that cut plane normal.\n"
                                  "Display only — does not change the skeleton or the cuts.");

            ImGui::Separator();

            m_imgui->disabled_begin(n_nodes == 0);
            if (m_imgui->button(_L("Step"))) {
                if (const int inst_id = m_c && m_c->selection_info() ? m_c->selection_info()->get_active_instance() : -1;
                    inst_id >= 0 && m_c->selection_info()->model_object() &&
                    size_t(inst_id) < m_c->selection_info()->model_object()->instances.size()) { // UNLAYERED_MOD: no active instance
                    m_preview_world    = m_c->selection_info()->model_object()->instances[inst_id]->get_matrix();
                    m_preview_edges    = build_cut_edges(m_preview_world);
                    m_preview_step     = m_preview_edges.empty() ? -1 : 0;
                    rebuild_step_contour();
                }
            }
            if (ImGui::IsItemHovered() && n_nodes > 0)
                ImGui::SetTooltip("Step through each cut plane without committing");
            m_imgui->disabled_end();

            if (m_preview_step >= 0 && !m_preview_edges.empty()) {
                ImGui::Separator();
                m_imgui->disabled_begin(m_preview_step == 0);
                if (m_imgui->button("  <  ")) {
                    --m_preview_step;
                    rebuild_step_contour();
                }
                m_imgui->disabled_end();
                ImGui::SameLine();
                m_imgui->text(GUI::format(_L("Cut %1% / %2%"), m_preview_step + 1, (int)m_preview_edges.size()));
                ImGui::SameLine();
                m_imgui->disabled_begin(m_preview_step + 1 >= (int)m_preview_edges.size());
                if (m_imgui->button("  >  ")) {
                    ++m_preview_step;
                    rebuild_step_contour();
                }
                m_imgui->disabled_end();
                ImGui::SameLine();
                if (m_imgui->button(_L("Exit"))) {
                    m_preview_step = -1;
                    m_step_contour_model.reset();
                }
                ImGui::SameLine();
                if (m_imgui->button(_L("Cut here"))) {
                    // Author ONE declarative plane for the previewed edge -- the same data the
                    // Cut Planes gizmo edits -- instead of destructively splitting. Stay in this
                    // gizmo: the user is mid step-through.
                    if (ModelObject* mo = m_c->selection_info()->model_object(); mo != nullptr) {
                        if (mo->cut_planes.planes.size() >= 64) {
                            MessageDialog(wxGetApp().plater(),
                                          _L("At most 64 cut planes are supported per object."),
                                          _L("Too many cut planes"), wxICON_WARNING | wxOK).ShowModal();
                        } else {
                            Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Cut at pipe step"), UndoRedo::SnapshotType::GizmoAction);
                            const auto& [par, child] = m_preview_edges[m_preview_step];
                            const bool was_empty = mo->cut_planes.planes.empty();
                            mo->cut_planes.planes.push_back(make_edge_plane(par, child));
                            if (was_empty) {
                                mo->cut_planes.enabled = true;
                                prompt_by_group_sequence();
                            }
                            m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
                            m_parent.request_extra_frame();
                            // Advance the step-through past the edge just authored.
                            ++m_preview_step;
                            if (m_preview_step >= (int)m_preview_edges.size()) {
                                m_preview_step = -1;
                                m_step_contour_model.reset();
                            } else
                                rebuild_step_contour();
                        }
                    }
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Add a cut plane at this skeleton edge (the object is not split)");
            }

            ImGui::Unindent(10.f);
        }

        ImGui::Separator();
    } // end generator panel

    // Node count feedback.
    const int n_nodes = m_skeleton_dag.num_nodes();
    if (n_nodes > 0)
        m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT,
                              GUI::format(_L("Centreline: %1% nodes"), n_nodes));
    else
        m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_DARK, _L("No centreline yet"));

    // Footer: primary action button.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 10.0f));
    float f_scale = m_parent.get_gizmos_manager().get_layout_scale();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f * f_scale));

    const bool skeleton_ready = n_nodes > 0;
    m_imgui->disabled_begin(!skeleton_ready);
    if (m_imgui->button(_L("Cut along pipe")))
        perform_skeleton_cut(m_parent.get_selection());
    if (ImGui::IsItemHovered() && skeleton_ready)
        ImGui::SetTooltip("Adds a cut plane at every centreline step; the object is not split");
    m_imgui->disabled_end();

    ImGui::PopStyleVar(2);
}


// The base class applies the result and rebuilds the GL models; the cache of user edits and the
// preview contours have to follow, and can only be written once the worker's result has landed.
void GLGizmoSkeleton::apply_skeleton_result(const SkeletonResult& res)
{
    GLGizmoCut3D::apply_skeleton_result(res);
    m_skel_cut_edges = res.cut_edges;
    sync_skeleton_cache();
    precompute_cut_contours();
    rebuild_step_contour();
}

}} // namespace Slic3r::GUI
