#include "GLGizmoCutChunks.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"

#include <glad/gl.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Gizmos/GizmoObjectManipulation.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/Utils/UndoRedo.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"
#include "imgui/imgui_internal.h"
#include "slic3r/GUI/Field.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
// #include "FixModelByWin10.hpp"
#include "libslic3r/PrintConfig.hpp"
#include <iostream>

namespace Slic3r { namespace GUI {

// Build a readable, collision-free name for a freshly separated cut chunk:
// "<base> - N" using the lowest N not already taken. Keeps repeated cuts from
// accumulating suffixes like _A_A_A_B.
static std::string make_chunk_name(const Model& model, const std::string& base)
{
    auto taken = [&model](const std::string& nm) {
        for (const ModelObject* o : model.objects)
            if (o->name == nm)
                return true;
        return false;
    };
    for (int n = 1;; ++n) {
        std::string candidate = base + " - " + std::to_string(n);
        if (!taken(candidate))
            return candidate;
    }
}

// UNLAYERED_MOD — see the header. Mirrors the gate used by GLCanvas3D::_render_chunk_build_volumes().
bool GLGizmoCutChunks3D::on_is_activable() const
{
    if (! nonplanar_printing_enabled())
        return false;
    return GLGizmoCut3D::on_is_activable();
}

void GLGizmoCutChunks3D::perform_cut(const Selection& selection, bool surpress_diag = false)
{

    m_keep_upper         = true;
    m_keep_lower         = true;
    m_connectors_editing = false;

    //disables auto drop (an object's, shared by its instances; the chunks inherit it)
    ModelObject* src_obj = m_c->selection_info()->model_object();
    src_obj->auto_drop = false;

    // Remember where the source object sat and what it was called, so afterwards
    // we can keep the trunk in that slot / under that name and send only the new
    // chunk to the bottom of the list.
    const int         src_index = selection.get_object_idx();
    const std::string src_name  = src_obj->name;

    m_c->selection_info()->model_object()->cut_connectors.clear();

    const Vec3d normal       = m_cut_normal;
    const Vec3d plane_center = m_plane_center;

    BOOST_LOG_TRIVIAL(info) << "Cut plane normal: [" << normal.x() << ", " << normal.y() << ", " << normal.z() << "]";
    printf("Cut plane normal: [%.4f, %.4f, %.4f]\n", normal.x(), normal.y(), normal.z());

    double b_angle_deg = 0., c_angle_deg = 0.;
    Geometry::bc_from_build_normal(normal, b_angle_deg, c_angle_deg);

    printf("B angle: %.4f deg, C angle: %.4f deg\n", b_angle_deg, c_angle_deg);
    printf("X: %.4f, Y: %.4f, Z: %.4f\n", m_plane_center[0], m_plane_center[1], m_plane_center[2]);

    // Only rebuild the part selection if one isn't already in place.
    // This preserves stray-contour deselections made interactively (on_mouse)
    // or set up programmatically (e.g. select_contour_near in skeleton cuts).
    if (!m_part_selection.valid())
        process_contours();
    else
        m_part_selection.turn_over_selection();

    Plater* plater = wxGetApp().plater();
    std::vector<ObjectID> ids_before;

    for (size_t i = 0; i < plater->model().objects.size(); ++i)
        ids_before.push_back(plater->model().objects[i]->id());

    GLGizmoCut3D::perform_cut(selection, surpress_diag);

    // Classify the two resulting pieces. The tilted piece (on the +normal side,
    // the one that receives the layer rotation) is the freshly separated chunk;
    // the other piece is the trunk it was cut from.
    ModelObject* trunk = nullptr;
    ModelObject* chunk = nullptr;
    for (ModelObject* obj : plater->model().objects) {
        if (std::find(ids_before.begin(), ids_before.end(), obj->id()) != ids_before.end())
            continue;

        const Vec3d instance_offset = obj->instances[0]->get_offset();
        const double side           = normal.dot(instance_offset - plane_center);
        const bool is_upper         = side > 0.0;

        if (is_upper) {
            obj->config.set_key_value("layer_rotation_b", new ConfigOptionFloat(b_angle_deg));
            obj->config.set_key_value("layer_rotation_c", new ConfigOptionFloat(c_angle_deg));
            BOOST_LOG_TRIVIAL(info) << "Upper object '" << obj->name << "' B=" << b_angle_deg << " C=" << c_angle_deg;
            chunk = obj;
        } else {
            trunk = obj;
        }
    }

    // Naming: keep the original name on the trunk; give the new chunk a readable,
    // non-accumulating "<name> - N" (instead of appending _A/_B on every cut).
    if (trunk)
        trunk->name = src_name;
    if (chunk)
        chunk->name = make_chunk_name(plater->model(), src_name);

    // Ordering: apply_cut_object_to_model appended both new pieces at the end of
    // the list. Move the trunk back into the source object's original slot; the
    // new chunk stays at the bottom.
    if (trunk && src_index >= 0) {
        int trunk_idx = -1;
        for (size_t i = 0; i < plater->model().objects.size(); ++i)
            if (plater->model().objects[i] == trunk) {
                trunk_idx = static_cast<int>(i);
                break;
            }
        if (trunk_idx >= 0)
            wxGetApp().obj_list()->reorder_object(trunk_idx, src_index);
    }
}

// BFS skeleton cut: iterates nodes leaf-first, cuts at each node's edge midpoint,
// assigns layer-rotation config to each arm chunk. After each cut the trunk
// (root-side piece) is re-selected and m_c is refreshed so process_contours()
// inside the next perform_cut() sees the correct mesh and clipper.
void GLGizmoCutChunks3D::perform_skeleton_cut(const Selection& selection)
{
    printf("Starting Skeleton Cut\n\n");
    if (m_skeleton_dag.num_nodes() == 0)
        return;

    const int instance_idx = selection.get_instance_idx();
    const int object_idx   = selection.get_object_idx();
    if (instance_idx < 0 || object_idx < 0)
        return;

    Plater* plater           = wxGetApp().plater();
    ModelObject* original_mo = plater->model().objects[object_idx];
    if (!original_mo)
        return;

    // Snapshot of all object IDs before any cuts — used to identify new segments.
    const int insert_idx = object_idx;
    std::vector<ObjectID> pre_cut_ids;
    pre_cut_ids.reserve(plater->model().objects.size());
    for (ModelObject* obj : plater->model().objects)
        pre_cut_ids.push_back(obj->id());

    // Skeleton nodes are in the object's local space; capture world transform once.
    const Transform3d world = original_mo->instances[instance_idx]->get_matrix();

    // --- Build ground-up BFS edge list -----------------------------------
    // Seed BFS from all ground nodes: nodes where no neighbour has a lower
    // world Z.  A branching skeleton can have multiple branches that each
    // contact the bed, so there may be several such roots.  Multi-source BFS
    // still produces a spanning forest with correctly directed (bottom→top)
    // edges; the subsequent sort reinforces that ordering.
    const int n  = m_skeleton_dag.num_nodes();
    auto world_z = [&](int i) { return (world * m_skeleton_dag.nodes[i]).z(); };

    std::vector<bool> visited(n, false);
    std::queue<int> bfs_q;
    for (int i = 0; i < n; ++i) {
        bool is_ground = true;
        for (int nb : m_skeleton.adj[i])
            if (world_z(nb) < world_z(i)) {
                is_ground = false;
                break;
            }
        if (is_ground) {
            visited[i] = true;
            bfs_q.push(i);
        }
    }

    std::vector<std::pair<int, int>> edges; // (parent, child)
    edges.reserve(n - 1);
    while (!bfs_q.empty()) {
        int u = bfs_q.front();
        bfs_q.pop();
        for (int v : m_skeleton.adj[u]) {
            if (!visited[v]) {
                visited[v] = true;
                edges.emplace_back(u, v);
                bfs_q.push(v);
            }
        }
    }

    // Sort edges bottom-to-top in world space so cuts proceed from the bed upward.
    std::sort(edges.begin(), edges.end(),
              [&](const std::pair<int, int>& a, const std::pair<int, int>& b) { return world_z(a.first) < world_z(b.first); });
    // -----------------------------------------------------------------------

    const Transform3d saved_rotation_m = m_rotation_m;
    ScopeGuard restore_rotation([this, &saved_rotation_m]() {
        m_rotation_m     = saved_rotation_m;
        m_part_selection = PartSelection();
    });

    Plater::TakeSnapshot snapshot(plater, _u8L("Cut along pipe"));
    const CommonGizmosDataID data_requirements = on_get_requirements();

    // Map each skeleton node to the ObjectID of the model object that currently
    // contains it.
    std::vector<ObjectID> node_to_obj(n, plater->model().objects[object_idx]->id());

    for (const auto& [par, child] : edges) {
        const Vec3d parent_local = m_skeleton_dag.nodes[par];
        const Vec3d child_local  = m_skeleton_dag.nodes[child];
        const Vec3d world_mid    = (world * parent_local + world * child_local) * 0.5;
        // Normal points from child (lower) toward parent (higher) — upward through the object.
        const Vec3d world_norm = (world.linear() * (child_local - parent_local)).normalized();

        // Find the model object currently owning the parent node by ObjectID.
        const ObjectID active_id = node_to_obj[par];
        int active_idx           = -1;
        for (size_t i = 0; i < plater->model().objects.size(); ++i)
            if (plater->model().objects[i]->id() == active_id) {
                active_idx = static_cast<int>(i);
                break;
            }
        if (active_idx < 0)
            break;

        m_parent.get_selection().add_object(static_cast<unsigned int>(active_idx), true);
        m_c->update(data_requirements);

        m_plane_center = world_mid;
        m_rotation_m   = Transform3d(Eigen::Quaterniond::FromTwoVectors(Vec3d::UnitZ(), world_norm));
        update_clipper();

        // Reset so process_contours() runs fresh for this cut's geometry.
        m_part_selection = PartSelection();
        process_contours();

        // For branching skeletons, steer the contour selection toward the arm
        // (child node). Spine mode cuts are sequential top-to-bottom so the
        // default plane-side assignment is already correct — skip this there.
        if (!m_skel_collapse_to_spine && m_part_selection.valid())
            m_part_selection.select_contour_near(world * child_local);

        std::vector<ObjectID> ids_before;
        for (ModelObject* obj : plater->model().objects)
            ids_before.push_back(obj->id());

        perform_cut(m_parent.get_selection(), true);
        m_parent.reload_scene(false);

        const Vec3d axis = (world * child_local - world * parent_local).normalized();
        ObjectID trunk_id{}, arm_id{};
        int n_new = 0;
        for (ModelObject* obj : plater->model().objects) {
            if (std::find(ids_before.begin(), ids_before.end(), obj->id()) != ids_before.end())
                continue;
            ++n_new;
            const Vec3d center = obj->instance_bounding_box(0).center();
            if ((center - world * child_local).squaredNorm() < (center - world * parent_local).squaredNorm())
                arm_id = obj->id();
            else
                trunk_id = obj->id();
        }

        if (n_new < 2) {
            printf("No cut made at edge (%d→%d) — n_new=%d, skipping\n", par, child, n_new);
            continue;
        }

        // BFS from child through the undirected skeleton to find child's subtree.
        // All nodes in the subtree move to the arm object; the rest stay with trunk.
        std::vector<bool> in_subtree(n, false);
        {
            std::queue<int> q;
            q.push(child);
            in_subtree[child] = true;
            while (!q.empty()) {
                int u = q.front();
                q.pop();
                for (int v : m_skeleton.adj[u]) {
                    if (!in_subtree[v] && node_to_obj[v] == active_id && v != par) {
                        in_subtree[v] = true;
                        q.push(v);
                    }
                }
            }
        }
        for (int i = 0; i < n; ++i) {
            if (node_to_obj[i] != active_id)
                continue;
            node_to_obj[i] = in_subtree[i] ? arm_id : trunk_id;
        }

        // Re-select the trunk for the next iteration.
        int trunk_idx = -1;
        for (size_t i = 0; i < plater->model().objects.size(); ++i) {
            if (plater->model().objects[i]->id() == trunk_id) {
                trunk_idx = static_cast<int>(i);
                // break;
                continue;
            }
        }
        if (trunk_idx < 0)
            // break;
            continue;

        m_parent.get_selection().add_object(static_cast<unsigned int>(trunk_idx), true);
        m_c->update(data_requirements);
    }

    // Sort the resulting segments by world Z (lowest first) so the object list
    // order matches the physical print stack from bed to tip.
    {
        auto& objs = plater->model().objects;

        std::vector<ModelObject*> new_segs;
        for (ModelObject* obj : objs)
            if (std::find(pre_cut_ids.begin(), pre_cut_ids.end(), obj->id()) == pre_cut_ids.end())
                new_segs.push_back(obj);

        for (ModelObject* obj : new_segs) {
            auto it = std::find(objs.begin(), objs.end(), obj);
            if (it != objs.end())
                objs.erase(it);
        }

        std::stable_sort(new_segs.begin(), new_segs.end(), [](const ModelObject* a, const ModelObject* b) {
            return a->instance_bounding_box(0).center().z() < b->instance_bounding_box(0).center().z();
        });

        const int safe_idx = std::min(insert_idx, (int) objs.size());
        objs.insert(objs.begin() + safe_idx, new_segs.begin(), new_segs.end());

        m_parent.reload_scene(false);
    }
}

void GLGizmoCutChunks3D::render_cut_plane_input_window(CutConnectors& connectors, float x, float y, float bottom_limit)
{
    render_build_size();

    ImGui::AlignTextToFramePadding();
    m_imgui->text(_L("Cut position") + ": ");
    ImGui::SameLine();
    render_move_center_input(Z);
    ImGui::SameLine();

    const bool is_cut_plane_init = m_rotation_m.isApprox(Transform3d::Identity()) && m_bb_center.isApprox(m_plane_center);
    m_imgui->disabled_begin(is_cut_plane_init);
    std::string act_name = _u8L("Reset cutting plane");
    if (render_reset_button("cut_plane", act_name)) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), act_name, UndoRedo::SnapshotType::GizmoAction);
        reset_cut_plane();
    }
    m_imgui->disabled_end();

    // Display the cut plane normal vector
    ImGui::AlignTextToFramePadding();
    m_imgui->text(_L("Cut normal") + ": ");
    ImGui::SameLine();
    const Vec3d& normal = m_cut_normal;
    m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, GUI::format("X: %.3f  Y: %.3f  Z: %.3f", normal.x(), normal.y(), normal.z()));

    ImGui::Separator();

    if (m_imgui->checkbox(_L("Show pipe centreline"), m_show_skeleton) && m_show_skeleton && !m_skeleton_model.is_initialized())
        compute_skeleton();
    if (m_show_skeleton) {
        ImGui::SameLine();
        if (m_imgui->button(_L("Recompute")))
            compute_skeleton();

        ImGui::Indent();
        ImGui::SetNextItemWidth(120.f);
        ImGui::SliderFloat(_L("Quality/speed").ToUTF8().data(), &m_skel_quality_speed, 0.01f, 1.0f, "%.3f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Lower = higher quality, slower. Default 0.1");

        ImGui::SetNextItemWidth(120.f);
        ImGui::SliderFloat(_L("Min edge factor").ToUTF8().data(), &m_skel_min_edge_factor, 0.0001f, 0.02f, "%.4f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Fraction of bounding box diagonal. Default 0.002");

        ImGui::SetNextItemWidth(120.f);
        ImGui::SliderInt(_L("Smooth iters").ToUTF8().data(), &m_skel_smooth_iterations, 0, 50);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Laplacian smoothing passes. 0 = off. Default 10");

        ImGui::SetNextItemWidth(120.f);
        ImGui::SliderFloat(_L("Smooth alpha").ToUTF8().data(), &m_skel_smooth_alpha, 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Smoothing strength per iteration. Default 0.5");

        ImGui::SetNextItemWidth(120.f);
        ImGui::SliderFloat(_L("Collapse angle").ToUTF8().data(), &m_skel_collapse_angle, 0.0f, 90.0f, "%.1f°");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Merge nearly-collinear edges below this angle (degrees). 0 = off. Default 10");

        ImGui::SetNextItemWidth(120.f);
        ImGui::SliderFloat(_L("Collapse edge len").ToUTF8().data(), &m_skel_collapse_edge_len, 0.0f, 5.0f, "%.2f mm");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Merge edges shorter than this length (mm). 0 = off. Default 0.2 (≈ layer height)");

        ImGui::SetNextItemWidth(120.f);
        ImGui::SliderFloat(_L("Resample edge len").ToUTF8().data(), &m_skel_resample_edge_len, 0.0f, 5.0f, "%.2f mm");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Subdivide edges longer than this after collapse to restore node density. 0 = off.");

        m_imgui->checkbox(_L("Collapse to spine"), m_skel_collapse_to_spine);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("DFS leaf-to-leaf: collapses all branches into the single shortest path through the skeleton");

        ImGui::Separator();

        const bool skeleton_ready = m_skeleton_dag.num_nodes() > 0;
        m_imgui->disabled_begin(!skeleton_ready);
        if (m_imgui->button(_L("Cut along pipe")))
            perform_skeleton_cut(m_parent.get_selection());
        if (ImGui::IsItemHovered() && skeleton_ready)
            ImGui::SetTooltip("BFS leaf-first cut: splits the object at every skeleton edge midpoint");
        m_imgui->disabled_end();

        ImGui::Unindent();
    }

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 10.0f));
    float get_cur_y = ImGui::GetContentRegionMax().y + ImGui::GetFrameHeight() + y;

    float f_scale = m_parent.get_gizmos_manager().get_layout_scale();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f * f_scale));

    ImGui::SameLine();
    m_imgui->disabled_begin(!can_perform_cut());
    if (m_imgui->button(_L("Perform cut")))
        perform_cut(m_parent.get_selection());
    m_imgui->disabled_end();

    ImGui::PopStyleVar(2);
}

}} // namespace Slic3r::GUI
