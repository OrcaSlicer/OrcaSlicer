#include "GLGizmoCutPlanes.hpp"

#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Tab.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/Utils/UndoRedo.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Utils.hpp"
#include <wx/utils.h>
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Tesselate.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"

#include <glad/gl.h>

#include <algorithm>
#include <numeric>
#include <cmath>

#include "imgui/imgui_internal.h"
#include <wx/filesys.h>
#include <wx/filename.h>

namespace Slic3r { namespace GUI {

static const ColorRGBA STORED_PLANE_COLOR     = { 0.9f, 0.9f, 0.9f, 0.25f };
// Disabled planes: clearly distinct (muted red) and fainter, so their state is readable at a glance.
static const ColorRGBA DISABLED_PLANE_COLOR   = { 0.8f, 0.25f, 0.2f, 0.08f };
static const ColorRGBA CONTOUR_COLOR          = { 1.0f, 1.0f, 1.0f, 1.0f };
static const ColorRGBA CONTOUR_COLOR_ACTIVE   = { 1.0f, 0.7f, 0.1f, 1.0f };
// Active editing plane: a solid contour-shaped surface plus a much fainter square that keeps the
// infinite-plane affordance visible (and is all there is while dragging).
static const ColorRGBA ACTIVE_SURFACE_COLOR   = { 0.9f, 0.9f, 0.9f, 0.5f };
static const ColorRGBA ACTIVE_SQUARE_COLOR    = { 0.9f, 0.9f, 0.9f, 0.12f };

// Margin (mm) the fitted plane surfaces extend beyond the object's cross-section.
static constexpr double QUAD_MARGIN = 3.0;

// Tessellate the cross-section polygons (offset outward by QUAD_MARGIN, holes preserved) into a
// triangle surface at `height` along the plane whose slice-frame inverse rotation is `inv_rot`.
// Returns false when the cross-section is empty. Also reports the expanded surface's 2D bounds.
static bool build_plane_surface(const Polygons &polys, double height, const Transform3d &inv_rot,
                                GLModel &out_model, Vec2d *out_min2 = nullptr, Vec2d *out_max2 = nullptr)
{
    if (polys.empty())
        return false;
    const ExPolygons surface = union_ex(offset(polys, float(scale_(QUAD_MARGIN))));
    const std::vector<Vec2d> tris = triangulate_expolygons_2d(surface);
    if (tris.empty())
        return false;

    GLModel::Geometry gsurf;
    gsurf.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
    for (const Vec2d &p2 : tris) {
        const Vec3d p3 = inv_rot * Vec3d(p2.x(), p2.y(), height);
        gsurf.add_vertex(Vec3f(p3.x(), p3.y(), p3.z()));
    }
    for (unsigned int k = 0; k + 2 < (unsigned int) tris.size(); k += 3)
        gsurf.add_triangle(k, k + 1, k + 2);
    out_model.reset();
    out_model.init_from(std::move(gsurf));

    if (out_min2 != nullptr && out_max2 != nullptr) {
        const BoundingBox bb = get_extents(surface);
        *out_min2 = unscale(bb.min);
        *out_max2 = unscale(bb.max);
    }
    return true;
}

ModelObject *GLGizmoCutPlanes::selected_model_object() const
{
    return m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
}

Transform3d GLGizmoCutPlanes::instance_matrix() const
{
    const ModelObject *mo = selected_model_object();
    if (mo == nullptr || mo->instances.empty())
        return Transform3d::Identity();
    int inst = m_c->selection_info()->get_active_instance();
    if (inst < 0 || inst >= int(mo->instances.size()))
        inst = 0;
    return mo->instances[inst]->get_matrix();
}

void GLGizmoCutPlanes::sync_gizmo_from_active_plane()
{
    ModelObject *mo = selected_model_object();
    if (mo == nullptr || m_active_plane < 0 || m_active_plane >= int(mo->cut_planes.planes.size())) {
        // Free mode: the editing pose is not bound to a stored plane.
        m_active_plane = -1;
        return;
    }

    const Slic3r::CutPlane &cp = mo->cut_planes.planes[m_active_plane];
    const Transform3d m  = instance_matrix();
    // Points map by the full matrix, normals by the inverse-transpose of its linear part, so the
    // plane stays correct under instance rotation, non-uniform scale and mirror.
    const Vec3d point_world  = m * cp.point;
    Vec3d       normal_world = (m.linear().inverse().transpose() * cp.normal).normalized();

    m_plane_center = point_world;
    m_rotation_m   = Transform3d(Eigen::Quaterniond::FromTwoVectors(Vec3d::UnitZ(), normal_world));

    // Keep the base class's archived state consistent so its drag/undo bookkeeping starts from
    // this pose instead of a stale one.
    m_ar_plane_center          = m_plane_center;
    m_start_dragging_m         = m_rotation_m;
    m_transformed_bounding_box = transformed_bounding_box(m_plane_center, m_rotation_m);
}

void GLGizmoCutPlanes::commit_gizmo_to_active_plane()
{
    ModelObject *mo = selected_model_object();
    if (mo == nullptr || m_active_plane < 0 || m_active_plane >= int(mo->cut_planes.planes.size()))
        return;

    // UNLAYERED_MOD -- the authority on the tilt limit for stored planes: every authoring path ends
    // here. It clamps the POSE rather than just the value written, so the plane is stored at the
    // angle it is drawn at -- a stored-only clamp would leave the viewport showing a steeper plane
    // until something resynced it, which is the "print something the viewport never showed" failure
    // this design forbids. Usually a no-op, because a rotation drag has already been held at the
    // limit frame by frame.
    clamp_rotation_to_tilt_limit();

    const Transform3d m            = instance_matrix();
    const Vec3d       normal_world = (m_rotation_m * Vec3d::UnitZ()).normalized();

    Slic3r::CutPlane &cp = mo->cut_planes.planes[m_active_plane];
    cp.point  = m.inverse() * m_plane_center;
    cp.normal = (m.linear().transpose() * normal_world).normalized();
    m_contours_dirty = true;
    m_preview_valid = false;
    schedule_background_process();
}

// UNLAYERED_MOD -- the tilt limit for one stored plane. The reach comes from the last preview
// rebuild; 0 (not yet derived) yields the hotend term alone, which is the looser of the two,
// so a plane whose chunk has not been measured yet is never over-restricted.
double GLGizmoCutPlanes::plane_tilt_limit_deg(int plane_index, std::optional<double> azimuth) const
{
    const double reach = (plane_index >= 0 && plane_index < int(m_plane_reach.size()))
                       ? m_plane_reach[plane_index] : 0.;
    return virtual_max_tilt_deg(reach, azimuth);
}

std::optional<double> GLGizmoCutPlanes::tilt_azimuth(const Vec3d &n_world)
{
    if (std::hypot(n_world.x(), n_world.y()) < 1e-6)
        return std::nullopt;
    return std::atan2(n_world.y(), n_world.x());
}

std::optional<double> GLGizmoCutPlanes::tilt_azimuth_bc(double b_deg, double c_deg)
{
    return tilt_azimuth(Geometry::build_normal_from_bc(b_deg, c_deg));
}

// The whole-object (or whole-part) build direction prints the object as ONE tilted piece, so the
// run the gantry has to clear is the object's own footprint.
double GLGizmoCutPlanes::object_tilt_limit_deg(std::optional<double> azimuth) const
{
    const ModelObject *mo = selected_model_object();
    if (mo == nullptr)
        return virtual_max_tilt_deg(0., azimuth);
    const BoundingBoxf3 bb = mo->raw_mesh_bounding_box().transformed(instance_matrix());
    return virtual_max_tilt_deg(std::hypot(bb.size().x(), bb.size().y()), azimuth);
}

Vec3d GLGizmoCutPlanes::clamp_build_normal_to_tilt(const Vec3d &n_world, double limit_deg)
{
    if (limit_deg <= 0.)
        return n_world;
    // Go through B/C rather than trigonometry on the vector: B IS the tilt from vertical and C the
    // azimuth, so capping |B| leans the normal back up while leaving the direction it leans in
    // alone -- and it keeps this in the same convention as the typed field and Print::validate().
    double b = 0., c = 0.;
    Geometry::bc_from_build_normal(n_world.normalized(), b, c);
    if (std::abs(b) <= limit_deg)
        return n_world;
    return Geometry::build_normal_from_bc(b < 0. ? -limit_deg : limit_deg, c).normalized();
}

bool GLGizmoCutPlanes::clamp_rotation_to_tilt_limit()
{
    // The limit for the way this pose leans. Clamping keeps that azimuth, so the clamped normal is
    // held to the same limit it was measured against.
    const Vec3d  n     = (m_rotation_m * Vec3d::UnitZ()).normalized();
    const double limit = plane_tilt_limit_deg(m_active_plane, tilt_azimuth(n));
    if (limit <= 0.)
        return false;
    const Vec3d clamped = clamp_build_normal_to_tilt(n, limit);
    if (clamped.isApprox(n))
        return false;
    m_tilt_limit_hit_deg   = limit;
    m_tilt_limit_hit_plane = m_active_plane;
    // Same idiom as sync_gizmo_from_active_plane(): a cut plane has no meaningful spin about its
    // own normal, so rebuilding the pose from the normal alone loses nothing.
    m_rotation_m               = Transform3d(Eigen::Quaterniond::FromTwoVectors(Vec3d::UnitZ(), clamped));
    m_transformed_bounding_box = transformed_bounding_box(m_plane_center, m_rotation_m);
    return true;
}

void GLGizmoCutPlanes::on_dragging(const UpdateData &data)
{
    GLGizmoCut3D::on_dragging(data);
    // After the base has moved the pose, not instead of it: the plane then renders at the limit for
    // the rest of the drag, so the user sees it stop rather than watching it follow the cursor and
    // jump back on release.
    //
    // Only on the two TILT grabbers, because they are the only ones whose motion this can oppose.
    // A move, a spin about Z or a connector drag does not change how far the plane leans, so
    // clamping there would do nothing per frame and only cost the check.
    //
    // It does NOT follow that an over-steep plane survives being moved: commit_gizmo_to_active_plane
    // clamps whatever ends up being stored, so a plane inherited from a project made before this
    // printer was characterised straightens the first time it is edited at all. That is the "block
    // the plane outright" rule doing its job; the panel states the limit so the change is
    // attributable rather than mysterious.
    if ((m_hover_id == X || m_hover_id == Y) && ! clamp_rotation_to_tilt_limit()) {
        // Back inside the limit (by a hair more than rounding): the red line goes away.
        const Vec3d n = (m_rotation_m * Vec3d::UnitZ()).normalized();
        double      b = 0., c = 0.;
        Geometry::bc_from_build_normal(n, b, c);
        const double limit = plane_tilt_limit_deg(m_active_plane, tilt_azimuth(n));
        if (limit <= 0. || std::abs(b) < limit - 0.05)
            m_tilt_limit_hit_deg = 0.;
    }
}

const ModelVolume *GLGizmoCutPlanes::scoped_volume() const
{
    const Selection &selection = m_parent.get_selection();
    if (! selection.is_single_volume())
        return nullptr;
    const ModelObject *mo = selected_model_object();
    const GLVolume    *gv = selection.get_first_volume();
    if (mo == nullptr || gv == nullptr)
        return nullptr;
    const int vol_idx = gv->volume_idx();
    if (vol_idx < 0 || vol_idx >= int(mo->volumes.size()))
        return nullptr;
    const ModelVolume *mv = mo->volumes[vol_idx];
    return mv->is_model_part() ? mv : nullptr;
}

const indexed_triangle_set &GLGizmoCutPlanes::merged_mesh()
{
    const ModelVolume *scope = scoped_volume();
    if (m_merged_dirty || scope != m_merged_scope) {
        // Exactly the mesh the partition is derived from — shared with the segmentation module
        // rather than kept as a second copy of the same loop that could drift from it. When one
        // part is selected, that part alone (in the object-local frame, under its own matrix):
        // the user asked to see how the planes cut this body, not the whole object.
        const ModelObject *mo = selected_model_object();
        if (mo == nullptr)
            m_merged_its = indexed_triangle_set();
        else if (scope != nullptr) {
            m_merged_its = scope->mesh().its;
            its_transform(m_merged_its, scope->get_matrix());
        }
        else
            m_merged_its = merged_model_part_mesh(*mo, /*exclude_directed_parts*/ true);
        m_merged_scope = scope;
        m_merged_dirty = false;
        // Everything derived from the mesh follows it.
        m_contours_dirty = true;
        m_preview_valid  = false;
        m_active_surface.reset();
        // The face-pick raycaster and patches index this very mesh.
        m_face_raycaster.reset();
        m_face_hover_valid = false;
        m_face_hover_patch = -1;
        m_face_hover_model.reset();
        m_face_patches.clear();
        m_facet_patch.clear();
        m_face_hint_patches.clear();
        m_face_hint_model.reset();
        m_face_hint_outline.reset();
        m_face_hover_outline.reset();
    }
    return m_merged_its;
}

static const ColorRGBA FACE_HOVER_COLOR   { 1.0f, 0.55f, 0.0f, 0.90f };
static const ColorRGBA FACE_HINT_COLOR    { 1.0f, 0.55f, 0.0f, 0.45f };
static const ColorRGBA FACE_HINT_OUTLINE  { 0.80f, 0.35f, 0.0f, 1.0f };
static const ColorRGBA FACE_HOVER_OUTLINE { 1.0f, 1.0f, 1.0f, 1.0f };
// Outline vertices are lifted off the surface along the patch normal (by `lift` mm) so the
// depth-tested lines win against the face they trace (the fills use a polygon offset for the
// same reason; lines cannot).
template<class PatchRange>
static void patch_outlines_to_model(const indexed_triangle_set &its, const PatchRange &patches, float lift_mm, GLModel &model)
{
    model.reset();
    GLModel::Geometry geo;
    geo.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
    for (const auto *patch : patches) {
        const Vec3f lift = patch->normal.template cast<float>() * lift_mm;
        for (const auto &[a, b] : patch->boundary) {
            const unsigned int base = geo.vertices_count();
            geo.add_vertex(Vec3f(its.vertices[a] + lift));
            geo.add_vertex(Vec3f(its.vertices[b] + lift));
            geo.add_line(base, base + 1);
        }
    }
    if (geo.vertices_count() > 0)
        model.init_from(std::move(geo));
}

// A GLModel of the given facets of `its` (vertices copied, so the model is self-contained).
static void facets_to_model(const indexed_triangle_set &its, const std::vector<int> &facets, GLModel &model)
{
    model.reset();
    if (facets.empty())
        return;
    indexed_triangle_set sub;
    sub.indices.reserve(facets.size());
    std::vector<int> remap(its.vertices.size(), -1);
    for (int fi : facets) {
        const stl_triangle_vertex_indices &f = its.indices[fi];
        stl_triangle_vertex_indices        g;
        for (int j = 0; j < 3; ++ j) {
            int &r = remap[f(j)];
            if (r < 0) {
                r = int(sub.vertices.size());
                sub.vertices.push_back(its.vertices[f(j)]);
            }
            g(j) = r;
        }
        sub.indices.push_back(g);
    }
    model.init_from(sub);
}

void GLGizmoCutPlanes::compute_face_patches()
{
    m_face_patches.clear();
    m_facet_patch.clear();
    m_face_hint_patches.clear();
    m_face_hint_model.reset();
    m_face_hint_outline.reset();
    m_face_hover_patch = -1;

    const indexed_triangle_set &its = merged_mesh();
    const int num_facets = int(its.indices.size());
    if (num_facets == 0)
        return;

    // Region growing over facet adjacency. Membership is decided by coplanarity with the SEED's
    // plane -- every vertex of the candidate within `plane_eps` of it -- plus a loose same-facing
    // test, rather than by matching normals tightly: the tiny triangles that stitch a flat face
    // to neighbouring features have noisy normals and, under a normal-only test, split one flat
    // face into several patches. The distance to the seed plane still stops the walk on curved
    // surfaces, since it grows as the walk bends away. Seeds go largest-triangle-first so each
    // region's plane comes from an accurately-oriented facet.
    const std::vector<Vec3f>   normals   = its_face_normals(its);
    const std::vector<Vec3i32> neighbors = its_face_neighbors(its);
    std::vector<double>        areas(num_facets);
    for (int fi = 0; fi < num_facets; ++ fi) {
        const stl_triangle_vertex_indices &f = its.indices[fi];
        areas[fi] = 0.5 * double((its.vertices[f(1)] - its.vertices[f(0)]).cross(its.vertices[f(2)] - its.vertices[f(0)]).norm());
    }
    std::vector<int> seeds(num_facets);
    std::iota(seeds.begin(), seeds.end(), 0);
    std::sort(seeds.begin(), seeds.end(), [&areas](int a, int b) { return areas[a] > areas[b]; });

    constexpr float plane_eps      = 0.05f;   // mm, object-local
    constexpr float min_facing_dot = 0.9659f; // cos 15 deg
    m_facet_patch.assign(num_facets, -1);
    std::vector<int> queue;
    for (int seed : seeds) {
        if (m_facet_patch[seed] >= 0)
            continue;
        const int   patch_id = int(m_face_patches.size());
        const Vec3f n0       = normals[seed];
        const Vec3f p0       = its.vertices[its.indices[seed](0)];
        FacePatch   patch;
        Vec3d       normal_sum = Vec3d::Zero();
        queue.assign(1, seed);
        m_facet_patch[seed] = patch_id;
        while (! queue.empty()) {
            const int fi = queue.back();
            queue.pop_back();
            patch.facets.push_back(fi);
            patch.area += areas[fi];
            normal_sum += normals[fi].cast<double>() * areas[fi];
            for (int j = 0; j < 3; ++ j) {
                const int nb = neighbors[fi][j];
                if (nb < 0 || m_facet_patch[nb] >= 0 || normals[nb].dot(n0) < min_facing_dot)
                    continue;
                const stl_triangle_vertex_indices &g = its.indices[nb];
                bool coplanar = true;
                for (int k = 0; k < 3 && coplanar; ++ k)
                    coplanar = std::abs((its.vertices[g(k)] - p0).dot(n0)) <= plane_eps;
                if (coplanar) {
                    m_facet_patch[nb] = patch_id;
                    queue.push_back(nb);
                }
            }
        }
        patch.normal = normal_sum.norm() > 0. ? normal_sum.normalized() : n0.cast<double>().normalized();
        m_face_patches.push_back(std::move(patch));
    }

    // Outline of each patch: the edges whose facet across the edge lies in another patch (or in
    // none). Only knowable once every facet has its patch, hence a second pass.
    for (int fi = 0; fi < num_facets; ++ fi) {
        const int patch_id = m_facet_patch[fi];
        const stl_triangle_vertex_indices &f = its.indices[fi];
        for (int j = 0; j < 3; ++ j) {
            const int nb = neighbors[fi][j];
            if (nb < 0 || m_facet_patch[nb] != patch_id)
                m_face_patches[patch_id].boundary.emplace_back(f(j), f((j + 1) % 3));
        }
    }

    // Hints: the largest flat regions. Lay on Face's bounds (>= 5 mm^2, at most 254 candidates).
    std::vector<int> order(m_face_patches.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [this](int a, int b) { return m_face_patches[a].area > m_face_patches[b].area; });
    std::vector<int> hint_facets;
    for (int pi : order) {
        if (m_face_patches[pi].area < 5.0 || m_face_hint_patches.size() >= 254)
            break;
        m_face_hint_patches.push_back(pi);
        hint_facets.insert(hint_facets.end(), m_face_patches[pi].facets.begin(), m_face_patches[pi].facets.end());
    }
    facets_to_model(its, hint_facets, m_face_hint_model);
    rebuild_face_outlines();
}

void GLGizmoCutPlanes::rebuild_face_outlines()
{
    const indexed_triangle_set &its = merged_mesh();
    std::vector<const FacePatch*> hint_patches;
    hint_patches.reserve(m_face_hint_patches.size());
    for (int pi : m_face_hint_patches)
        hint_patches.push_back(&m_face_patches[pi]);
    patch_outlines_to_model(its, hint_patches, m_face_outline_lift, m_face_hint_outline);
    if (m_face_hover_patch >= 0)
        patch_outlines_to_model(its, std::vector<const FacePatch*>{ &m_face_patches[m_face_hover_patch] }, m_face_outline_lift, m_face_hover_outline);
    else
        m_face_hover_outline.reset();
}

bool GLGizmoCutPlanes::set_face_pick_mode(bool on)
{
    m_face_pick_mode   = on;
    m_face_hover_valid = false;
    m_face_hover_patch = -1;
    m_face_hover_model.reset();
    m_face_hover_outline.reset();
    if (on && m_face_patches.empty())
        compute_face_patches();
    m_parent.set_as_dirty();
    return on;
}

bool GLGizmoCutPlanes::update_face_hover(const Vec2d &mouse_pos)
{
    m_face_hover_valid = false;
    const indexed_triangle_set &merged = merged_mesh();
    if (merged.indices.empty())
        return false;
    if (! m_face_raycaster)
        m_face_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(merged));

    Vec3f  hit, normal;
    size_t facet = 0;
    if (! m_face_raycaster->unproject_on_mesh(mouse_pos, instance_matrix(), wxGetApp().plater()->get_camera(),
                                              hit, normal, nullptr, &facet))
        return false;

    m_face_hover_valid  = true;
    m_face_hover_point  = hit.cast<double>();
    m_face_hover_normal = normal.cast<double>().normalized();

    // Highlight the whole coplanar patch under the pointer (object-local, drawn under the
    // instance matrix) and snap to its normal; rebuilt only when the patch changes.
    const int patch = facet < m_facet_patch.size() ? m_facet_patch[facet] : -1;
    if (patch >= 0)
        m_face_hover_normal = m_face_patches[patch].normal;
    if (patch != m_face_hover_patch) {
        m_face_hover_patch = patch;
        if (patch >= 0) {
            facets_to_model(merged, m_face_patches[patch].facets, m_face_hover_model);
            patch_outlines_to_model(merged, std::vector<const FacePatch*>{ &m_face_patches[patch] }, m_face_outline_lift, m_face_hover_outline);
        } else {
            facets_to_model(merged, { int(facet) }, m_face_hover_model);
            m_face_hover_outline.reset();
        }
    }
    return true;
}

void GLGizmoCutPlanes::align_plane_to_hovered_face()
{
    if (! m_face_hover_valid)
        return;
    // Same conversions as sync_gizmo_from_active_plane(): points by the full instance matrix,
    // normals by the inverse-transpose of its linear part.
    const Transform3d m            = instance_matrix();
    const Vec3d       point_world  = m * m_face_hover_point;
    const Vec3d       normal_world = (m.linear().inverse().transpose() * m_face_hover_normal).normalized();

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Align cut plane to face"), UndoRedo::SnapshotType::GizmoAction);
    m_plane_center = point_world;
    m_rotation_m   = Transform3d(Eigen::Quaterniond::FromTwoVectors(Vec3d::UnitZ(), normal_world));
    // UNLAYERED_MOD -- picking a face is a third way to pose a plane, so it obeys the tilt limit as
    // the drag does. Clamped here, before the bookkeeping below captures the pose, so the plane is
    // stored and drawn at the same angle; on a steep face it lands short of the face it was aimed
    // at, which is the honest outcome when that face cannot be printed.
    clamp_rotation_to_tilt_limit();
    // Keep the base class's archived state in step so its drag/undo bookkeeping starts here, and
    // refresh the bounding box before anything consults set_center_pos()'s range check.
    m_ar_plane_center          = m_plane_center;
    m_start_dragging_m         = m_rotation_m;
    m_transformed_bounding_box = transformed_bounding_box(m_plane_center, m_rotation_m);
    update_clipper();
    commit_gizmo_to_active_plane(); // the bound stored plane follows; no-op in free mode
    m_preview_valid = false;
    set_face_pick_mode(false);
}

void GLGizmoCutPlanes::render_face_hover()
{
    if (! m_face_pick_mode)
        return;
    const bool hover = m_face_hover_valid && m_face_hover_model.is_initialized();
    if (! hover && ! m_face_hint_model.is_initialized())
        return;
    GLShaderProgram *shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;
    const Camera &camera = wxGetApp().plater()->get_camera();

    // Keep the outline lift at about a pixel and a half of world distance for the current zoom
    // (inv_zoom is world units per pixel at the camera target), within sane bounds, and rebuild
    // the outline models only when it has moved by more than a quarter.
    {
        const float wanted = std::clamp(1.5f * float(camera.get_inv_zoom()), 0.005f, 0.3f);
        if (std::abs(wanted - m_face_outline_lift) > 0.25f * m_face_outline_lift) {
            m_face_outline_lift = wanted;
            rebuild_face_outlines();
        }
    }

    // The overlays lie exactly on the surface: depth-test them (so patches behind other geometry
    // stay hidden) but pull them forward a hair so they win the z-fight with that surface.
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDepthMask(GL_FALSE));
    glsafe(::glEnable(GL_POLYGON_OFFSET_FILL));
    glsafe(::glPolygonOffset(-1.0f, -1.0f));
    glsafe(::glEnable(GL_CULL_FACE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    const bool mirrored = instance_matrix().matrix().determinant() < 0.0;
    glsafe(::glFrontFace(mirrored ? GL_CW : GL_CCW));

    shader->start_using();
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    shader->set_uniform("view_model_matrix", camera.get_view_matrix() * instance_matrix());
    if (m_face_hint_model.is_initialized()) {
        m_face_hint_model.set_color(FACE_HINT_COLOR);
        m_face_hint_model.render();
    }
    if (hover) {
        m_face_hover_model.set_color(FACE_HOVER_COLOR);
        m_face_hover_model.render();
    }

    // Outlines: depth-tested against the model only (the fills above wrote no depth), so they
    // trace the patch edges and vanish where the model is in front of them.
    glsafe(::glDisable(GL_POLYGON_OFFSET_FILL));
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glDepthFunc(GL_LEQUAL));
    if (m_face_hint_outline.is_initialized()) {
        glsafe(::glLineWidth(1.5f));
        m_face_hint_outline.set_color(FACE_HINT_OUTLINE);
        m_face_hint_outline.render();
    }
    if (hover && m_face_hover_outline.is_initialized()) {
        glsafe(::glLineWidth(2.5f));
        m_face_hover_outline.set_color(FACE_HOVER_OUTLINE);
        m_face_hover_outline.render();
    }
    glsafe(::glLineWidth(1.0f));
    glsafe(::glDepthFunc(GL_LESS));
    shader->stop_using();

    glsafe(::glFrontFace(GL_CCW));
    glsafe(::glDisable(GL_BLEND));
    glsafe(::glDepthMask(GL_TRUE));
}

std::string GLGizmoCutPlanes::get_tooltip() const
{
    if (m_face_pick_mode)
        return m_face_hover_valid ? _u8L("Click to align the cut plane to this face (right-click cancels)")
                                  : _u8L("Point at a face of the model (right-click cancels)");
    return GLGizmoCut3D::get_tooltip();
}

void GLGizmoCutPlanes::update_active_surface()
{
    if (m_dragging)
        return; // mid-drag only the faint square is shown; the surface snaps on release

    // Current editing pose in the object-local frame (same conversion the commit uses).
    const Transform3d m      = instance_matrix();
    const Vec3d       n_w    = (m_rotation_m * Vec3d::UnitZ()).normalized();
    const Vec3d       point  = m.inverse() * m_plane_center;
    const Vec3d       normal = (m.linear().transpose() * n_w).normalized();
    if (m_active_surface.is_initialized() && point.isApprox(m_active_surface_point, 1e-9) && normal.isApprox(m_active_surface_normal, 1e-9))
        return;
    m_active_surface_point  = point;
    m_active_surface_normal = normal;
    m_active_surface.reset();
    m_active_surface_touches = false;

    const indexed_triangle_set &merged = merged_mesh();
    if (merged.vertices.empty())
        return;

    MeshSlicingParams params;
    params.trafo.rotate(Eigen::Quaterniond::FromTwoVectors(normal, Vec3d::UnitZ()));
    const double height = normal.dot(point);
    m_active_surface_touches = build_plane_surface(slice_mesh(merged, float(height), params), height,
                        Transform3d(Eigen::Quaterniond::FromTwoVectors(Vec3d::UnitZ(), normal)), m_active_surface);
    if (! m_active_surface_touches) {
        // A plane lying exactly on a flat face (the "Select face" result) slices to nothing, yet
        // it is not outside the object: it touches it if a hair to either side has a section.
        constexpr float touch_eps = 0.02f;
        m_active_surface_touches = ! slice_mesh(merged, float(height) - touch_eps, params).empty()
                                || ! slice_mesh(merged, float(height) + touch_eps, params).empty();
    }
}

void GLGizmoCutPlanes::render_cut_plane()
{
    GLShaderProgram *shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    update_active_surface();

    const Camera &camera = wxGetApp().plater()->get_camera();

    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDepthMask(GL_FALSE));
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

    shader->start_using();
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());

    // Faint square at the gizmo pose: the infinite-plane affordance, and the only visual mid-drag.
    if (m_plane.model.is_initialized()) {
        m_plane.model.set_color(ACTIVE_SQUARE_COLOR);
        shader->set_uniform("view_model_matrix",
                            camera.get_view_matrix() * Geometry::translation_transform(m_plane_center) * m_rotation_m);
        m_plane.model.render();
    }

    // Contour-shaped surface of the current cut (object-local, like the stored planes).
    if (! m_dragging && m_active_surface.is_initialized()) {
        m_active_surface.set_color(ACTIVE_SURFACE_COLOR);
        shader->set_uniform("view_model_matrix", camera.get_view_matrix() * instance_matrix());
        m_active_surface.render();
    }

    shader->stop_using();

    glsafe(::glEnable(GL_CULL_FACE));
    glsafe(::glDisable(GL_BLEND));
    glsafe(::glDepthMask(GL_TRUE));
}

void GLGizmoCutPlanes::schedule_background_process()
{
    // Any cut-plane mutation reshapes the sliced geometry: tell the plater so the current slice
    // is invalidated and the Slice button re-arms (Print::apply picks the change up through the
    // cut_planes diff). Without this the preview keeps showing G-code from BEFORE the edit.
    m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
}

void GLGizmoCutPlanes::set_active_plane(int idx)
{
    m_active_plane = idx;
    sync_gizmo_from_active_plane();
    m_contours_dirty = true;
    m_preview_valid = false;
    m_parent.request_extra_frame();
}

void GLGizmoCutPlanes::offer_by_group_sequence(const wxString &message)
{
    if (wxGetApp().global_print_sequence() == PrintSequence::ByGroup)
        return;
    MessageDialog dialog(wxGetApp().plater(), message, _L("Change print sequence"), wxICON_QUESTION | wxYES_NO | wxYES_DEFAULT);
    if (dialog.ShowModal() == wxID_YES) {
        DynamicPrintConfig config = wxGetApp().preset_bundle->prints.get_edited_preset().config;
        config.set_key_value("print_sequence", new ConfigOptionEnum<PrintSequence>(PrintSequence::ByGroup));
        wxGetApp().get_tab(Preset::TYPE_PRINT)->load_config(config);
    }
}

void GLGizmoCutPlanes::add_plane()
{
    ModelObject *mo = selected_model_object();
    if (mo == nullptr)
        return;

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Add cut plane"), UndoRedo::SnapshotType::GizmoAction);

    // Record the CURRENT gizmo pose (world) as a new object-local plane. The pose is deliberately
    // left in place so the user can shift/rotate it and add the next plane, mirroring how the Cut
    // tool's plane behaves.
    const Transform3d m            = instance_matrix();
    const Vec3d       normal_world = (m_rotation_m * Vec3d::UnitZ()).normalized();

    Slic3r::CutPlane cp;
    cp.point  = m.inverse() * m_plane_center;
    cp.normal = (m.linear().transpose() * normal_world).normalized();
    const bool first_plane = mo->cut_planes.planes.empty();
    mo->cut_planes.planes.push_back(cp);
    if (first_plane)
        mo->cut_planes.enabled = true; // authoring the first plane implies the user wants the feature on

    if (first_plane)
        offer_by_group_sequence(_L("Cut-plane printing requires the \"By group\" print sequence. Change the current process setting to \"By group\" now?"));

    m_contours_dirty = true;
    m_preview_valid = false;
    schedule_background_process();
    m_parent.request_extra_frame();
}

void GLGizmoCutPlanes::delete_plane(size_t idx)
{
    ModelObject *mo = selected_model_object();
    if (mo == nullptr || idx >= mo->cut_planes.planes.size())
        return;

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Delete cut plane"), UndoRedo::SnapshotType::GizmoAction);
    mo->cut_planes.planes.erase(mo->cut_planes.planes.begin() + idx);
    if (m_active_plane == int(idx))
        m_active_plane = -1;              // the bound plane is gone: back to free mode, pose stays
    else if (m_active_plane > int(idx))
        -- m_active_plane;                // same plane, shifted index
    m_contours_dirty = true;
    m_preview_valid = false;
    schedule_background_process();
    m_parent.request_extra_frame();
}

void GLGizmoCutPlanes::move_plane(size_t idx, int dir)
{
    ModelObject *mo = selected_model_object();
    if (mo == nullptr)
        return;
    const int other = int(idx) + dir;
    if (idx >= mo->cut_planes.planes.size() || other < 0 || other >= int(mo->cut_planes.planes.size()))
        return;

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Reorder cut planes"), UndoRedo::SnapshotType::GizmoAction);
    std::swap(mo->cut_planes.planes[idx], mo->cut_planes.planes[other]);
    // A manual reorder is the user overriding the automatic (seat-rule) order: from here on the
    // list order alone decides, or the reorder would visibly do nothing whenever the seat rule
    // pins the order (crossing planes). "Automatic order" in the panel switches back.
    mo->cut_planes.user_ordered = true;
    // Keep the binding on the same physical plane.
    if (m_active_plane == int(idx))
        m_active_plane = other;
    else if (m_active_plane == other)
        m_active_plane = int(idx);
    m_contours_dirty = true;
    m_preview_valid = false;
    schedule_background_process();
    m_parent.request_extra_frame();
}

// UNLAYERED_MOD -- see the header.
bool GLGizmoCutPlanes::on_is_activable() const
{
    if (! nonplanar_printing_enabled())
        return false;
    const Selection &selection = m_parent.get_selection();
    if (selection.get_object_idx() < 0 || selection.is_wipe_tower() || m_parent.is_layers_editing_enabled())
        return false;
    // Unlike the destructive Cut tool, a single body (model part) of a multi-part object is enough
    // here: the plane stack, the merged mesh, the chunk preview, face picking and the object build
    // direction all work on the whole object regardless of which part is selected. The editing
    // plane then centres on the selected body, which is exactly what aligning to one of its faces
    // wants.
    return selection.is_single_full_instance() || selection.is_single_volume();
}

void GLGizmoCutPlanes::on_set_state()
{
    GLGizmoCut3D::on_set_state();
    if (m_state == On) {
        // Free mode with the base's default centered pose (set by update_bb() in the base
        // on_set_state) — exactly the Cut tool's opening state. The model is NOT touched here:
        // the selection-dependent common data may not be fully set up yet during a state switch.
        m_active_plane   = -1;
        // The coloured chunk preview follows the app-wide "Color cut chunks" setting (View menu /
        // Preferences): one switch, persistent, in and out of the gizmo. The checkbox below writes
        // the same setting.
        m_preview_chunks = wxGetApp().show_cut_chunk_colors();
        m_contours_dirty = m_merged_dirty = true;
        m_preview_valid = false;
        m_active_surface.reset();
        // Never clip/hide the object: this gizmo previews with plane quads and contours.
        if (auto oc = m_c->object_clipper())
            oc->set_behavior(false, false, 0.);
    } else {
        // Leaving the gizmo: drop the chunk preview and make sure the real object is visible again.
        // Re-show only what on_render() hid: the unscoped form un-hides every volume (wipe tower
        // included) and resets forced colours scene-wide, which is not this gizmo's to touch.
        m_preview_chunks = false;
        if (m_hidden_object != nullptr) {
            m_parent.toggle_model_objects_visibility(true, m_hidden_object, m_hidden_instance, m_hidden_volume);
            m_hidden_object   = nullptr;
            m_hidden_instance = -1;
            m_hidden_volume   = nullptr;
        }
        m_segment_models.clear();
        // A pending face pick does not outlive the gizmo, nor do its raycaster and patches.
        m_face_pick_mode   = false;
        m_face_hover_valid = false;
        m_face_hover_patch = -1;
        m_face_hover_model.reset();
        m_face_raycaster.reset();
        m_face_patches.clear();
        m_facet_patch.clear();
        m_face_hint_patches.clear();
        m_face_hint_model.reset();
        m_face_hint_outline.reset();
        m_face_hover_outline.reset();
    }
}

void GLGizmoCutPlanes::data_changed(bool is_serializing)
{
    GLGizmoCut3D::data_changed(is_serializing);
    // The model may have changed under us (undo/redo, object moved/scaled, plane list edited):
    // re-derive the pose of the bound plane, or drop to free mode if it disappeared.
    sync_gizmo_from_active_plane();
    m_contours_dirty = m_merged_dirty = true;
    m_preview_valid = false;
    m_active_surface.reset(); // pose-compare cache: force a rebuild against the fresh mesh
    if (auto oc = m_c->object_clipper())
        oc->set_behavior(false, false, 0.);
}

void GLGizmoCutPlanes::on_render()
{
    // The segment preview renders FIRST so the (translucent) plane quads drawn afterwards blend
    // over the segments instead of depth-masking them into invisibility.
    // Rebuild whenever the plane list the preview should show has changed -- which includes the
    // live pose of the plane under the grabbers, so the colors follow the plane as it is dragged
    // instead of snapping only when the drag ends. Runs even with the preview off, because the
    // per-plane annotations in the panel read the same derivation. On a heavy mesh the rebuild
    // waits for the drag to finish (see LIVE_PREVIEW_MAX_TRIANGLES).
    {
        const std::vector<Slic3r::CutPlane> wanted = effective_planes();
        if (! m_preview_valid || wanted != m_preview_planes) {
            const bool heavy = merged_mesh().indices.size() > LIVE_PREVIEW_MAX_TRIANGLES;
            if (! (heavy && m_dragging))
                rebuild_segment_preview();
        }
    }

    if (m_preview_chunks) {
        render_segment_preview();
        // Hide the real object while previewing. This runs before/after the base's
        // toggle_model_objects_visibility() re-show, but visibility flags are consumed by the
        // NEXT frame's object pass — the explicit hide below (after the base call) wins.
    }

    // The face-pick overlays go BEFORE the base render: the base clears the depth buffer to put
    // its grabbers on top, and the overlays must be depth-tested against the model (or the chunk
    // preview standing in for it) so the edges of faces hidden behind other geometry stay hidden.
    render_face_hover();

    // Base renders the editing plane with its grabbers (plus the color-clip tint via
    // update_clipper); we add the stored planes on top.
    GLGizmoCut3D::on_render();
    if (m_contours_dirty) {
        precompute_plane_contours();
        m_contours_dirty = false;
    }
    render_stored_planes();
    render_plane_contours();

    if (m_preview_chunks && ! m_segment_models.empty()) {
        // Only swap the real object out when there actually are segments to show — with an empty
        // or inert stack the preview must not leave the viewport empty.
        if (const ModelObject *mo = selected_model_object()) {
            const int inst = m_c->selection_info()->get_active_instance();
            // Scope or instance changed while something was hidden: re-show that first, or it
            // would stay hidden after the preview moves on.
            if (m_hidden_object != nullptr && (m_hidden_object != mo || m_hidden_instance != inst || m_hidden_volume != m_merged_scope))
                m_parent.toggle_model_objects_visibility(true, m_hidden_object, m_hidden_instance, m_hidden_volume);
            m_hidden_object   = mo;
            m_hidden_instance = inst;
            m_hidden_volume   = m_merged_scope; // scoped: the preview stands in for this part only
            m_parent.toggle_model_objects_visibility(false, m_hidden_object, m_hidden_instance, m_hidden_volume);
        }
        m_parent.set_use_color_clip_plane(false);
    }
}

void GLGizmoCutPlanes::render_stored_planes()
{
    const ModelObject *mo = selected_model_object();
    if (mo == nullptr || mo->cut_planes.planes.empty() || m_plane_quads.empty())
        return;
    GLShaderProgram *shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    const Camera &camera = wxGetApp().plater()->get_camera();

    glsafe(::glEnable(GL_DEPTH_TEST));
    // Translucent quads must not write depth, or anything behind them (notably the chunk
    // preview meshes) fails the depth test and turns invisible.
    glsafe(::glDepthMask(GL_FALSE));
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

    shader->start_using();
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    // The quads are built in OBJECT-LOCAL coordinates, fitted to each plane's cross-section:
    // render them under the instance matrix exactly like the contours.
    shader->set_uniform("view_model_matrix", camera.get_view_matrix() * instance_matrix());
    for (size_t i = 0; i < mo->cut_planes.planes.size() && i < m_plane_quads.size(); ++ i) {
        if (int(i) == m_active_plane)
            continue; // the bound plane is drawn by the base at the gizmo pose
        if (! m_plane_quads[i].is_initialized())
            continue;
        m_plane_quads[i].set_color(mo->cut_planes.planes[i].enabled ? STORED_PLANE_COLOR : DISABLED_PLANE_COLOR);
        m_plane_quads[i].render();
    }
    shader->stop_using();

    glsafe(::glEnable(GL_CULL_FACE));
    glsafe(::glDisable(GL_BLEND));
    glsafe(::glDepthMask(GL_TRUE));
}

void GLGizmoCutPlanes::precompute_plane_contours()
{
    m_contours_model.reset();
    m_contours_model_active.reset();
    m_plane_quads.clear();
    m_plane_quad_rects.clear();

    const ModelObject *mo = selected_model_object();
    if (mo == nullptr || mo->cut_planes.planes.empty())
        return;
    m_plane_quads.resize(mo->cut_planes.planes.size());
    m_plane_quad_rects.resize(mo->cut_planes.planes.size());

    const indexed_triangle_set &merged = merged_mesh();
    if (merged.vertices.empty())
        return;

    GLModel::Geometry geo, geo_active;
    geo.format        = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
    geo_active.format = geo.format;

    // Object-local bbox of the merged mesh: quad fallback for planes that miss the object.
    BoundingBoxf3 mesh_bb;
    for (const stl_vertex &v : merged.vertices)
        mesh_bb.merge(v.cast<double>());

    for (size_t i = 0; i < mo->cut_planes.planes.size(); ++ i) {
        const Slic3r::CutPlane &cp = mo->cut_planes.planes[i];
        const double len = cp.normal.norm();
        if (len < EPSILON)
            continue;
        const Vec3d  dir    = cp.normal / len;
        const float  height = float(dir.dot(cp.point));
        const Eigen::Quaterniond to_z = Eigen::Quaterniond::FromTwoVectors(dir, Vec3d::UnitZ());
        const Transform3d inv_rot = Transform3d(Eigen::Quaterniond::FromTwoVectors(Vec3d::UnitZ(), dir));

        MeshSlicingParams params;
        params.trafo.rotate(to_z);
        const Polygons polys = slice_mesh(merged, height, params);

        // Shape the plane surface like the cross-section itself, expanded by a few mm (holes
        // included); a plane that misses the object falls back to a rectangle over the object's
        // footprint in the plane frame.
        Vec2d surf_min2, surf_max2;
        if (build_plane_surface(polys, double(height), inv_rot, m_plane_quads[i], &surf_min2, &surf_max2)) {
            // Picking uses the expanded surface's bounding rectangle.
            m_plane_quad_rects[i] = { surf_min2, surf_max2, true };
        } else if (mesh_bb.defined) {
            Vec2d min2( std::numeric_limits<double>::max(),  std::numeric_limits<double>::max());
            Vec2d max2(-std::numeric_limits<double>::max(), -std::numeric_limits<double>::max());
            for (int corner = 0; corner < 8; ++ corner) {
                const Vec3d c(corner & 1 ? mesh_bb.max.x() : mesh_bb.min.x(),
                              corner & 2 ? mesh_bb.max.y() : mesh_bb.min.y(),
                              corner & 4 ? mesh_bb.max.z() : mesh_bb.min.z());
                const Vec3d p = to_z * c;
                min2 = min2.cwiseMin(Vec2d(p.x(), p.y()));
                max2 = max2.cwiseMax(Vec2d(p.x(), p.y()));
            }
            min2.array() -= QUAD_MARGIN;
            max2.array() += QUAD_MARGIN;
            m_plane_quad_rects[i] = { min2, max2, true };

            GLModel::Geometry quad;
            quad.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
            const Vec2d corners[4] = { min2, { max2.x(), min2.y() }, max2, { min2.x(), max2.y() } };
            for (const Vec2d &c2 : corners) {
                const Vec3d p3 = inv_rot * Vec3d(c2.x(), c2.y(), double(height));
                quad.add_vertex(Vec3f(p3.x(), p3.y(), p3.z()));
            }
            quad.add_triangle(0, 1, 2);
            quad.add_triangle(0, 2, 3);
            m_plane_quads[i].init_from(std::move(quad));
        }

        if (! cp.enabled || polys.empty())
            continue; // disabled planes take no part in the cut: no outline

        GLModel::Geometry &target = int(i) == m_active_plane ? geo_active : geo;
        for (const Polygon &poly : polys) {
            if (poly.points.size() < 2)
                continue;
            const unsigned int base_idx = (unsigned int) target.vertices_count();
            for (const Point &pt : poly.points) {
                const Vec2d p2 = unscale(pt);
                const Vec3d p3 = inv_rot * Vec3d(p2.x(), p2.y(), double(height));
                target.add_vertex(Vec3f(p3.x(), p3.y(), p3.z()));
            }
            const unsigned int n_pts = (unsigned int) poly.points.size();
            for (unsigned int k = 0; k < n_pts; ++ k)
                target.add_line(base_idx + k, base_idx + (k + 1) % n_pts);
        }
    }

    if (geo.vertices_count() > 0) {
        m_contours_model.init_from(std::move(geo));
        m_contours_model.set_color(CONTOUR_COLOR);
    }
    if (geo_active.vertices_count() > 0) {
        m_contours_model_active.init_from(std::move(geo_active));
        m_contours_model_active.set_color(CONTOUR_COLOR_ACTIVE);
    }
}

void GLGizmoCutPlanes::render_plane_contours()
{
    if (! m_contours_model.is_initialized() && ! m_contours_model_active.is_initialized())
        return;
    GLShaderProgram *shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    const Camera     &camera = wxGetApp().plater()->get_camera();
    const Transform3d world  = instance_matrix();

    glsafe(::glDisable(GL_DEPTH_TEST));
    shader->start_using();
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    shader->set_uniform("view_model_matrix", camera.get_view_matrix() * world);
    if (m_contours_model.is_initialized()) {
        glsafe(::glLineWidth(1.0f));
        m_contours_model.render();
    }
    if (m_contours_model_active.is_initialized()) {
        glsafe(::glLineWidth(2.0f));
        m_contours_model_active.render();
    }
    shader->stop_using();
    glsafe(::glEnable(GL_DEPTH_TEST));
}

int GLGizmoCutPlanes::pick_stored_plane(const Vec2d &mouse_pos) const
{
    const ModelObject *mo = selected_model_object();
    if (mo == nullptr || mo->cut_planes.planes.empty() || m_plane_quad_rects.empty())
        return -1;

    // Work in the OBJECT-LOCAL frame (where the fitted quad rects live): map the mouse ray by the
    // inverse instance matrix. The ray parameter t is preserved by the affine map, so nearest-hit
    // comparisons stay correct even under instance scaling.
    const Transform3d inv = instance_matrix().inverse();
    const Linef3 ray = const_cast<GLCanvas3D&>(m_parent).mouse_ray(Point(mouse_pos.x(), mouse_pos.y()));
    const Vec3d  a   = inv * ray.a;
    const Vec3d  dir = inv.linear() * ray.unit_vector();

    int    best   = -1;
    double best_t = std::numeric_limits<double>::max();
    for (size_t i = 0; i < mo->cut_planes.planes.size() && i < m_plane_quad_rects.size(); ++ i) {
        if (int(i) == m_active_plane || ! m_plane_quad_rects[i].valid)
            continue;
        const Slic3r::CutPlane &cp = mo->cut_planes.planes[i];
        const double len = cp.normal.norm();
        if (len < EPSILON)
            continue;
        const Vec3d  n     = cp.normal / len;
        const double denom = n.dot(dir);
        if (std::abs(denom) < 1e-9)
            continue;
        const double t = n.dot(cp.point - a) / denom;
        if (t < 0. || t >= best_t)
            continue;
        // Hit point in the plane's slice frame; inside the fitted quad rect?
        const Vec3d hit2 = Eigen::Quaterniond::FromTwoVectors(n, Vec3d::UnitZ()) * (a + t * dir);
        const PlaneQuadRect &rect = m_plane_quad_rects[i];
        if (hit2.x() >= rect.min2.x() && hit2.x() <= rect.max2.x() &&
            hit2.y() >= rect.min2.y() && hit2.y() <= rect.max2.y()) {
            best   = int(i);
            best_t = t;
        }
    }
    return best;
}

bool GLGizmoCutPlanes::on_mouse(const wxMouseEvent &mouse_event)
{
    if (m_face_pick_mode) {
        // While picking, the editing plane's own click handling (flip on click, drag on the quad)
        // must not fire: the click means "this face".
        const Vec2d mouse_pos(mouse_event.GetX(), mouse_event.GetY());
        if (mouse_event.Moving()) {
            update_face_hover(mouse_pos);
            m_parent.set_as_dirty();
        }
        else if (mouse_event.LeftDown()) {
            update_face_hover(mouse_pos);
            if (m_face_hover_valid)
                align_plane_to_hovered_face();
            return true;
        }
        else if (mouse_event.LeftUp())
            return true;
        else if (mouse_event.RightDown() || mouse_event.RightUp()) {
            set_face_pick_mode(false);
            return true;
        }
    }

    // The base handles the editing plane's grabbers and dragging first.
    if (GLGizmoCut3D::on_mouse(mouse_event))
        return true;
    if (mouse_event.LeftDown() && m_hover_id == -1) {
        // Not on a grabber: a click on a stored plane quad binds it for editing.
        const int hit = pick_stored_plane(Vec2d(mouse_event.GetX(), mouse_event.GetY()));
        if (hit >= 0) {
            set_active_plane(hit);
            return true;
        }
    }
    return false;
}

void GLGizmoCutPlanes::on_stop_dragging()
{
    // Decide (before the base resets its bookkeeping) whether this drag edited the plane.
    const bool rotated = m_hover_id == X || m_hover_id == Y || m_hover_id == CutPlaneZRotation;
    const bool moved   = (m_hover_id == Z || m_hover_id == CutPlane || m_hover_id == CutPlaneXMove || m_hover_id == CutPlaneYMove)
                      && m_was_cut_plane_dragged;

    // The base takes the "Move/Rotate cut plane" GizmoAction snapshot; the model is still
    // untouched at that point, so the snapshot correctly captures the pre-drag state.
    GLGizmoCut3D::on_stop_dragging();

    if (rotated || moved)
        commit_gizmo_to_active_plane(); // no-op in free mode
}

void GLGizmoCutPlanes::on_load(cereal::BinaryInputArchive &ar)
{
    GLGizmoCut3D::on_load(ar);
    ar(m_active_plane);
    // The model was deserialized before the gizmo state: clamp and re-derive the pose.
    sync_gizmo_from_active_plane();
    m_contours_dirty = true;
    m_preview_valid = false;
}

void GLGizmoCutPlanes::on_save(cereal::BinaryOutputArchive &ar) const
{
    GLGizmoCut3D::on_save(ar);
    ar(m_active_plane);
}

void GLGizmoCutPlanes::render_cut_plane_input_window(CutConnectors &connectors, float x, float y, float bottom_limit)
{
    (void) connectors; (void) x; (void) y; (void) bottom_limit;

    ModelObject *mo = selected_model_object();
    if (mo == nullptr)
        return;
    CutPlaneStack &stack = mo->cut_planes;

    // Stock Cut-tool controls: build size, cut position, reset, flip, normal readout. The
    // position input and the flip button take their own snapshots and edit the gizmo pose; when a
    // stored plane is bound, the edit is committed back to it right after.
    render_build_size();

    ImGui::AlignTextToFramePadding();
    m_imgui->text(_L("Cut position") + ": ");
    ImGui::SameLine();
    const Vec3d center_before = m_plane_center;
    render_move_center_input(Z);
    if (! m_plane_center.isApprox(center_before))
        commit_gizmo_to_active_plane();
    ImGui::SameLine();

    const bool is_cut_plane_init = m_rotation_m.isApprox(Transform3d::Identity()) && m_bb_center.isApprox(m_plane_center);
    m_imgui->disabled_begin(is_cut_plane_init);
    if (render_reset_button("cut_plane", _u8L("Reset cutting plane"))) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Reset cutting plane"), UndoRedo::SnapshotType::GizmoAction);
        reset_cut_plane();
        m_active_plane = -1; // back to authoring a new plane
    }
    m_imgui->disabled_end();

    if (m_imgui->button(_L("Flip cut plane"))) {
        flip_cut_plane();                 // flips the gizmo pose under its own snapshot
        commit_gizmo_to_active_plane();   // keep the bound stored plane in sync (no-op when free)
    }
    ImGui::SameLine();
    if (m_imgui->button(m_face_pick_mode ? _L("Cancel face pick") : _L("Select face")))
        set_face_pick_mode(! m_face_pick_mode);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Click a face of the model to align the cut plane to it. Any face can be picked, not only the outer hull. Right-click cancels.").c_str());
    if (m_face_pick_mode)
        m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, _L("Click a face on the model"));

    ImGui::AlignTextToFramePadding();
    m_imgui->text(_L("Cut normal") + ": ");
    ImGui::SameLine();
    const Vec3d n = (m_rotation_m * Vec3d::UnitZ()).normalized();
    m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, GUI::format("X: %.3f  Y: %.3f  Z: %.3f", n.x(), n.y(), n.z()));

    render_object_build_direction();

    if (const ModelVolume *scope = scoped_volume()) {
        ImGui::Separator();
        m_imgui->text_colored(ImGuiWrapper::COL_GREY_DARK, GUI::format_wxstr(_L("Showing part \"%1%\" only; planes and build direction belong to the whole object"), scope->name));
    }

    ImGui::Separator();

    if (m_active_plane < 0) {
        if (m_imgui->button(_L("Add cut plane")))
            add_plane();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Record the current plane in this object's cut-plane list (the object is not split)").c_str());
    } else {
        ImGui::AlignTextToFramePadding();
        m_imgui->text(GUI::format(_L("Editing plane %1%"), m_active_plane + 1));
        ImGui::SameLine();
        if (m_imgui->button(_L("Done")))
            set_active_plane(-1); // pose stays where it is; edits were committed as they happened
    }

    if (! stack.planes.empty()) {
        ImGui::Separator();

        bool enabled = stack.enabled;
        if (m_imgui->checkbox(_L("Enable cut planes"), enabled)) {
            Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Toggle cut planes"), UndoRedo::SnapshotType::GizmoAction);
            stack.enabled = enabled;
            m_preview_valid = false;
            schedule_background_process();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Slice this object as multiplanar segments defined by the planes below").c_str());

        bool auto_order = ! stack.user_ordered;
        if (m_imgui->checkbox(_L("Automatic order"), auto_order)) {
            Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Toggle automatic plane order"), UndoRedo::SnapshotType::GizmoAction);
            stack.user_ordered = ! auto_order;
            m_preview_valid = false;
            schedule_background_process();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("On: sections are ordered automatically so each piece builds on the cut face it sits on.\n"
                                         "Off: the list order decides — the first plane in the list prints last and, where planes cross, "
                                         "its section claims the shared corner. Reordering a plane switches this off.").c_str());

        // Which planes actually produce a clip in the derived partition — read from the cached
        // derivation (refreshed in on_render when the plane list changes), never re-derived here:
        // this runs every frame the panel is drawn.
        const std::vector<bool> &splits = m_plane_splits;

        int to_delete = -1;
        int to_move_idx = -1, to_move_dir = 0;
        for (size_t i = 0; i < stack.planes.size(); ++ i) {
            ImGui::PushID(int(i));

            // Reorder arrows: with automatic order they are only the tie-break; a click flips the
            // stack to manual (user_ordered), where the list order alone decides -- the FIRST
            // plane in the list prints LAST and claims any contested wedge.
            m_imgui->disabled_begin(i == 0);
            if (ImGui::ArrowButton("##up", ImGuiDir_Up)) {
                to_move_idx = int(i);
                to_move_dir = -1;
            }
            m_imgui->disabled_end();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Reorder: the first plane in the list prints last. Switches Automatic order off.").c_str());
            ImGui::SameLine(0.f, 2.f);
            m_imgui->disabled_begin(i + 1 == stack.planes.size());
            if (ImGui::ArrowButton("##down", ImGuiDir_Down)) {
                to_move_idx = int(i);
                to_move_dir = 1;
            }
            m_imgui->disabled_end();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Reorder: the first plane in the list prints last. Switches Automatic order off.").c_str());
            ImGui::SameLine();

            // Same radio style as the Cut/Emboss/SVG gizmos: the bare ImGui radio inherits the
            // toolbar style's white check mark, which vanished on the light theme.
            ImGuiWrapper::push_radio_style(m_parent.get_scale());
            const bool toggle_active = ImGui::RadioButton("##active", int(i) == m_active_plane);
            ImGuiWrapper::pop_radio_style();
            if (toggle_active)
                set_active_plane(int(i) == m_active_plane ? -1 : int(i));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Edit this plane with the grabbers (click again to release)").c_str());

            ImGui::SameLine();
            bool plane_enabled = stack.planes[i].enabled;
            if (m_imgui->checkbox("##enabled", plane_enabled)) {
                Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Toggle cut plane"), UndoRedo::SnapshotType::GizmoAction);
                stack.planes[i].enabled = plane_enabled;
                m_contours_dirty = true;
                m_preview_valid = false;
                schedule_background_process();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Include this plane in the segmentation").c_str());

            ImGui::SameLine();
            m_imgui->text(GUI::format(_L("Plane %1%"), i + 1));

            // A plane can be perfectly valid and still not divide anything -- it is disabled, its
            // normal is degenerate, or it simply misses the mesh. Say so, instead of leaving the
            // user to wonder why the colors do not change across it.
            const char *state = nullptr;
            if (! stack.planes[i].enabled)
                state = "off";
            else if (stack.planes[i].normal.norm() < EPSILON)
                state = "invalid";
            else if (i < splits.size() && ! splits[i])
                state = "no cut";
            if (state != nullptr) {
                ImGui::SameLine();
                m_imgui->text_colored(ImGuiWrapper::COL_GREY_DARK, state);
            }

            ImGui::SameLine();
            if (m_imgui->button(_L("Delete")))
                to_delete = int(i);

            ImGui::PopID();
        }
        if (to_delete >= 0)
            delete_plane(size_t(to_delete));
        if (to_move_idx >= 0)
            move_plane(size_t(to_move_idx), to_move_dir);

        if (m_active_plane >= 0 && m_active_plane < int(stack.planes.size()))
            render_build_direction_editor(stack);

        bool preview = m_preview_chunks;
        if (m_imgui->checkbox(_L("Color cut chunks"), preview)) {
            m_preview_chunks = preview;
            // Same switch as View > Color Cut Chunks, so leaving the gizmo shows what was chosen here.
            wxGetApp().app_config->set_bool("show_cut_chunk_colors", preview);
            if (m_preview_chunks)
                m_preview_valid = false;
            else if (m_hidden_object != nullptr) {
                // Scoped, like on_set_state(): only re-show what the preview hid.
                m_parent.toggle_model_objects_visibility(true, m_hidden_object, m_hidden_instance, m_hidden_volume);
                m_hidden_object   = nullptr;
                m_hidden_instance = -1;
                m_hidden_volume   = nullptr;
            }
            m_parent.request_extra_frame();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Show the segments the enabled planes imply, each in its own color, including the plane you are editing. "
                                         "This is the same setting as View > Color Cut Chunks and stays in effect after leaving the tool.").c_str());

        if (m_preview_chunks) {
            // An empty preview has six different causes; showing the plain object without saying
            // which one applies is what makes the feature feel broken.
            wxString note;
            if (m_segment_models.empty()) {
                switch (m_preview_status) {
                case CutSegmentationStatus::NoSplit:        note = _L("No enabled plane divides this object."); break;
                case CutSegmentationStatus::NoRootCell:     note = _L("No part of the object rests on the bed side of every plane."); break;
                case CutSegmentationStatus::SegmentTooThin: note = _L("A segment would be thinner than the first layer."); break;
                case CutSegmentationStatus::TooManyPlanes:  note = _L("Too many enabled planes (64 maximum)."); break;
                case CutSegmentationStatus::NoGeometry:     note = _L("This object has no printable volume."); break;
                default:                                    note = _L("Cut planes are switched off for this object."); break;
                }
            }
            else if (m_preview_provisional)
                note = _L("Includes the plane you are editing — add it to keep it.");
            else if (! nonplanar_printing_enabled())
                note = _L("Non-planar printing is off: this printer will slice the object in one piece.");
            if (! note.IsEmpty()) {
                ImGui::Indent();
                m_imgui->text_colored(ImGuiWrapper::COL_GREY_DARK, note);
                ImGui::Unindent();
            }
        }
    }

    // Bundled offline walkthrough (resources/web/tutorial); also under Help > Multiplanar Tutorial.
    ImGui::Separator();
    if (m_imgui->button(_L("Tutorial")))
        wxLaunchDefaultBrowser(wxFileSystem::FileNameToURL(wxFileName(from_u8(Slic3r::resources_dir() + "/web/tutorial/multiplanar.html"))), wxBROWSER_NEW_WINDOW);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Open the multiplanar printing tutorial").c_str());
}

void GLGizmoCutPlanes::render_object_build_direction()
{
    ModelObject *mo = selected_model_object();
    if (mo == nullptr)
        return;

    // With one part selected the direction goes on THAT part: at slice time a directed part is its
    // own segment, built along its direction, while the other parts stay in the root (see
    // append_part_segments). Otherwise it goes on the object. Either way the value is the pair of
    // world-frame machine angles layer_rotation_b/c in the model config, what the process settings
    // show under Others > Multiplanar; this section only authors them geometrically.
    ModelVolume *part = nullptr;
    if (const ModelVolume *scope = scoped_volume())
        for (ModelVolume *mv : mo->volumes)
            if (mv == scope)
                part = mv;
    ModelConfigObject &config = part != nullptr ? part->config : mo->config;

    const bool   has_b = config.has("layer_rotation_b");
    const bool   has_c = config.has("layer_rotation_c");
    const double b     = has_b ? config.opt_float("layer_rotation_b") : 0.;
    const double c     = has_c ? config.opt_float("layer_rotation_c") : 0.;

    ImGui::Separator();
    ImGui::AlignTextToFramePadding();
    m_imgui->text((part != nullptr ? _L("Part build direction") : _L("Object build direction")) + ": ");
    ImGui::SameLine();
    if (has_b || has_c)
        m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, GUI::format("B: %.1f°  C: %.1f°", b, c));
    else
        m_imgui->text_colored(ImGuiWrapper::COL_GREY_DARK, part != nullptr ? _L("with the object") : _L("default (+Z)"));

    if (m_imgui->button(_L("Use plane as build direction"))) {
        // The plane's normal (the arrow side) becomes "up": slicing rotates the object, or this
        // part alone, so that direction maps onto +Z. Frozen as angles, like the legacy chunk path.
        Plater::TakeSnapshot snapshot(wxGetApp().plater(),
                                      part != nullptr ? _u8L("Set part build direction") : _u8L("Set object build direction"),
                                      UndoRedo::SnapshotType::GizmoAction);
        const bool  first_directed_part = part != nullptr && ! object_has_directed_parts(*mo);
        // UNLAYERED_MOD -- the object (or part) prints as one tilted piece, so its own footprint is
        // the run the gantry has to clear, not the active plane's chunk. In practice the pose is
        // already inside the plane limit, but the two limits differ whenever the object is larger
        // than the chunk seated on that plane, which is the usual case.
        const Vec3d n_pose              = (m_rotation_m * Vec3d::UnitZ()).normalized();
        const Vec3d n_w                 = clamp_build_normal_to_tilt(n_pose, object_tilt_limit_deg(tilt_azimuth(n_pose)));
        double      new_b = 0., new_c = 0.;
        Geometry::bc_from_build_normal(n_w, new_b, new_c);
        config.set_key_value("layer_rotation_b", new ConfigOptionFloat(new_b));
        config.set_key_value("layer_rotation_c", new ConfigOptionFloat(new_c));
        wxGetApp().obj_list()->object_config_options_changed({ mo, part }); // show the settings node
        m_merged_dirty  = true;  // the undirected set changed; contours and preview follow the mesh
        m_preview_valid = false;
        if (first_directed_part)
            offer_by_group_sequence(_L("Per-part build directions require the \"By group\" print sequence. Change the current process setting to \"By group\" now?"));
        schedule_background_process();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", (part != nullptr
            ? _u8L("Slice this part along the editing plane\'s normal (the arrow side becomes \"up\"), as its own section printed after the rest of the object. "
                   "Pick a face or pose the plane first; flip the plane to reverse.")
            : _u8L("Slice this whole object along the editing plane\'s normal (the arrow side becomes \"up\"). "
                   "Pick a face or pose the plane first; flip the plane to reverse. "
                   "Select a single part to give that part its own direction instead.")).c_str());
    ImGui::SameLine();
    m_imgui->disabled_begin(! (has_b || has_c));
    if (m_imgui->button(_L("Reset direction"))) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(),
                                      part != nullptr ? _u8L("Reset part build direction") : _u8L("Reset object build direction"),
                                      UndoRedo::SnapshotType::GizmoAction);
        config.erase("layer_rotation_b");
        config.erase("layer_rotation_c");
        wxGetApp().obj_list()->object_config_options_changed({ mo, part });
        m_merged_dirty  = true;
        m_preview_valid = false;
        schedule_background_process();
    }
    m_imgui->disabled_end();

    if (part == nullptr && (has_b || has_c) && mo->cut_planes.active())
        m_imgui->text_colored(ImGuiWrapper::COL_GREY_DARK, _L("Not used while this object\'s cut planes are enabled: each chunk has its own direction."));
    if (part != nullptr && (has_b || has_c))
        m_imgui->text_colored(ImGuiWrapper::COL_GREY_DARK, _L("This part prints as its own section; the cut planes apply to the other parts."));
    if (virtual_5axis_enabled()) {
        const double cap = object_tilt_limit_deg(tilt_azimuth_bc(b, c));
        if (cap > 0. && std::abs(b) > cap + EPSILON)
            // Still possible despite the clamp above: the direction may predate the printer being
            // characterised, or have arrived with the project or a pasted config.
            m_imgui->warning_text(GUI::format_wxstr(
                _L("Warning: B is %1%°, past the %2%° this printer can build; slicing will refuse it."),
                wxString::Format("%.1f", std::abs(b)), wxString::Format("%.1f", cap)));
    }
}

void GLGizmoCutPlanes::render_build_direction_editor(CutPlaneStack &stack)
{
    ModelObject *mo = selected_model_object();
    if (mo == nullptr || m_active_plane < 0 || m_active_plane >= int(stack.planes.size()))
        return;
    Slic3r::CutPlane &cp = stack.planes[m_active_plane];

    // B/C are MACHINE angles, so they are a view of the build normal in the WORLD frame: the
    // stored object-local normal maps to world by the inverse-transpose of the instance linear
    // part. Rotating the instance therefore changes the displayed B/C while the stored plane
    // stays glued to the object — that is correct, machine angles must follow the world pose.
    const Transform3d m = instance_matrix();
    const Vec3d n_world = (m.linear().inverse().transpose() * cp.effective_build_normal()).normalized();
    double b_display = 0., c_display = 0.;
    Geometry::bc_from_build_normal(n_world, b_display, c_display);
    const bool pole = std::hypot(n_world.x(), n_world.y()) < 1e-6; // C degenerate at +-Z

    // Refresh the edit buffers whenever they don't belong to this plane or nothing is being typed.
    if (m_edit_plane != m_active_plane || (! ImGui::IsAnyItemActive())) {
        m_edit_b     = b_display;
        m_edit_c     = pole ? 0. : c_display;
        m_edit_plane = m_active_plane;
    }

    ImGui::AlignTextToFramePadding();
    m_imgui->text(_L("Build direction") + ": ");
    if (! cp.has_build_normal) {
        ImGui::SameLine();
        m_imgui->text_colored(ImGuiWrapper::COL_GREY_DARK, _L("(from cut plane)"));
    }

    auto commit_bc = [this, &cp, &m, pole](double b, double c) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Change build direction"), UndoRedo::SnapshotType::GizmoAction);
        const Vec3d n_w = Geometry::build_normal_from_bc(b, pole && std::abs(b) < 1e-6 ? 0. : c);
        cp.build_normal     = (m.linear().transpose() * n_w).normalized(); // world -> object-local
        cp.has_build_normal = true;
        schedule_background_process();
    };

    ImGui::SetNextItemWidth(90.f);
    ImGui::InputDouble("B##build_b", &m_edit_b, 0.0, 0.0, "%.2f");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        m_edit_b = std::clamp(m_edit_b, -180.0, 0.0);
        // UNLAYERED_MOD -- and then to the printer's virtual-mode tilt limit, so a typed angle
        // cannot reach past what the drag grabbers now refuse to go to. This used to be deliberately
        // unclamped, on the reasoning that the plane normal IS the cut geometry and quietly reducing
        // it would store a shallower partition than the viewport shows. That objection was about
        // clamping at SLICE time; at authoring time every path -- this field, the rotation grabbers,
        // face picking -- clamps the pose it draws as well as the value it stores, so what is shown
        // and what is stored still agree. Print::validate() remains the backstop for a direction
        // that arrives some other way (an old project, a pasted config, the skeleton gizmo).
        if (const double cap = plane_tilt_limit_deg(m_active_plane, tilt_azimuth_bc(m_edit_b, m_edit_c)); cap > 0.) {
            m_tilt_limit_hit_deg   = m_edit_b < -cap ? cap : 0.;
            m_tilt_limit_hit_plane = m_active_plane;
            m_edit_b = std::clamp(m_edit_b, -cap, 0.0);
        }
        commit_bc(m_edit_b, m_edit_c);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Machine B angle (rotation about Y), -180..0 degrees").c_str());

    ImGui::SameLine();
    m_imgui->disabled_begin(pole && ! ImGui::IsAnyItemActive() && std::abs(m_edit_b) < 1e-6);
    ImGui::SetNextItemWidth(90.f);
    ImGui::InputDouble("C##build_c", &m_edit_c, 0.0, 0.0, "%.2f");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        if (m_edit_c > 180.)  m_edit_c -= 360.;
        if (m_edit_c < -180.) m_edit_c += 360.;
        commit_bc(m_edit_b, m_edit_c);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Machine C angle (rotation about Z), -180..180 degrees").c_str());
    m_imgui->disabled_end();

    ImGui::SameLine();
    m_imgui->disabled_begin(! cp.has_build_normal);
    if (m_imgui->button(_L("Reset"))) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Reset build direction"), UndoRedo::SnapshotType::GizmoAction);
        cp.has_build_normal = false;
        cp.build_normal     = Vec3d::Zero();
        m_edit_plane        = -1; // refresh buffers from the plane normal next frame
        schedule_background_process();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Track the cut plane's normal again").c_str());
    m_imgui->disabled_end();

    // UNLAYERED_MOD -- say what the limit is and where it came from, so a drag that refuses to go
    // further is explained rather than merely felt. Shown whenever there is a limit, not only when
    // it bites: finding out why the plane stopped is exactly the moment the number is wanted.
    if (virtual_5axis_enabled()) {
        // One short line -- red while the plane is held at the limit (the drag, typed angle or
        // picked face asked for more tilt than the printer can build), grey otherwise -- and the
        // why in a tooltip. The panel does not wrap, so a sentence here runs off its edge.
        const bool   held  = m_tilt_limit_hit_deg > 0. && m_tilt_limit_hit_plane == m_active_plane;
        // For the way the plane leans now: the hotend is not round, so another azimuth can differ.
        const double cap   = plane_tilt_limit_deg(m_active_plane, tilt_azimuth_bc(m_edit_b, m_edit_c));
        const double limit = held ? m_tilt_limit_hit_deg : cap;
        if (limit > 0.) {
            const double reach = (m_active_plane >= 0 && m_active_plane < int(m_plane_reach.size()))
                               ? m_plane_reach[m_active_plane] : 0.;
            const wxString deg = wxString::Format("%.1f", limit);
            if (held)
                m_imgui->text_colored(ImGuiWrapper::COL_RED, GUI::format_wxstr(_L("Stopped at the tilt limit: %1%°"), deg));
            else
                m_imgui->text_colored(ImGuiWrapper::COL_GREY_DARK, GUI::format_wxstr(_L("Max tilt this way: %1%°"), deg));
            if (ImGui::IsItemHovered()) {
                wxString why = _L("Any steeper and the hotend or the gantry would hit the part, or the plane would pass "
                                  "the printer's max tilt angle. The limit depends on which way the plane leans");
                why += reach > 0. ? GUI::format_wxstr(_L(" and on the chunk's size (%1% mm across)."), wxString::Format("%.0f", reach))
                                  : wxString(".");
                why += " " + _L("On a virtual 5-axis printer the nozzle stays upright.");
                m_imgui->tooltip(why, ImGui::GetFontSize() * 20.0f);
            }
        }
    }
}

std::vector<Slic3r::CutPlane> GLGizmoCutPlanes::effective_planes(bool *provisional) const
{
    if (provisional != nullptr)
        *provisional = false;
    const ModelObject *mo = selected_model_object();
    if (mo == nullptr)
        return {};

    std::vector<Slic3r::CutPlane> planes = mo->cut_planes.planes;
    if (! mo->cut_planes.enabled)
        // The stack is switched off: the slicer ignores it, and so does the preview. Note this
        // also covers the object's very first plane -- the stack is only enabled by add_plane() --
        // so nothing is previewed until that plane is committed. Deliberate: with no stored plane
        // there are no section colours to be wrong about, and colouring chunks for a stack that is
        // still off would claim a partition slicing would not produce.
        return {};

    // The pose currently under the grabbers, in the object-local frame — the same conversion
    // commit_gizmo_to_active_plane() performs.
    const Transform3d m      = instance_matrix();
    const Vec3d       n_w    = (m_rotation_m * Vec3d::UnitZ()).normalized();
    const Vec3d       point  = m.inverse() * m_plane_center;
    const Vec3d       normal = (m.linear().transpose() * n_w).normalized();

    if (m_active_plane >= 0 && m_active_plane < int(planes.size())) {
        // Bound plane: show it where it is being dragged, but keep everything else about it —
        // notably `enabled`, so a disabled plane still does not split the preview.
        planes[m_active_plane].point  = point;
        planes[m_active_plane].normal = normal;
    }
    else if (! planes.empty()) {
        // Free mode: the plane being authored is not in the stack yet, so without this the user
        // sees a plane that does not split the colors. Appended, because add_plane() appends —
        // committing it therefore reproduces exactly the partition shown here.
        Slic3r::CutPlane provisional_plane;
        provisional_plane.point   = point;
        provisional_plane.normal  = normal;
        provisional_plane.enabled = true;
        planes.push_back(provisional_plane);
        if (provisional != nullptr)
            *provisional = true;
    }
    return planes;
}

double GLGizmoCutPlanes::preview_min_segment_thickness() const
{
    // Print::apply refuses to segment when any segment would be thinner than the initial layer;
    // use the same bound so the preview goes inert exactly where slicing does.
    const DynamicPrintConfig &cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    if (const ConfigOptionFloat *opt = cfg.option<ConfigOptionFloat>("initial_layer_print_height"))
        return opt->value;
    return 0.;
}

void GLGizmoCutPlanes::rebuild_segment_preview()
{
    m_segment_models.clear();
    m_preview_planes = effective_planes(&m_preview_provisional);
    m_preview_status = CutSegmentationStatus::StackInactive;
    m_preview_valid  = true;
    m_plane_splits.assign(m_preview_planes.size(), false);
    m_plane_reach.assign(m_preview_planes.size(), 0.);

    const ModelObject *mo = selected_model_object();
    if (mo == nullptr || m_preview_planes.empty())
        return;

    // The filament the chunks fade from: the scoped part's, or the object's first part's -- the
    // same part whose colour GLVolume::render_cut_chunks() fades from in the Prepare view.
    const ColorRGBA filament = [this, mo]() {
        const ModelVolume *mv = m_merged_scope;
        for (const ModelVolume *v : mo->volumes)
            if (mv == nullptr && v->is_model_part())
                mv = v;
        const std::vector<ColorRGBA> colors = wxGetApp().plater()->get_extruders_colors();
        int extruder = (mv != nullptr ? mv->extruder_id() : 1) - 1;
        if (extruder < 0 || extruder >= int(colors.size()))
            extruder = 0;
        return colors.empty() ? ColorRGBA::WHITE() : colors[extruder];
    }();

    const indexed_triangle_set &merged = merged_mesh();
    if (merged.vertices.empty()) {
        m_preview_status = CutSegmentationStatus::NoGeometry;
        return;
    }

    // Take the meshes the derivation itself cut. Reconstructing them separately (segment_meshes)
    // cuts every physical plane twice — once per side, in two different rotated frames — so the two
    // sides of a cut face are only approximately the same surface.
    std::vector<indexed_triangle_set> pieces;
    std::vector<CutSegmentDescriptor> segs;
    if (m_merged_scope != nullptr && model_volume_has_build_direction(*m_merged_scope)) {
        // A directed part is its own segment and the planes never touch it: show it as the one
        // piece slicing makes of it, in that segment's colour.
        std::vector<indexed_triangle_set>       all_pieces;
        const std::vector<CutSegmentDescriptor> all = derive_cut_segments(*mo, instance_matrix(), preview_min_segment_thickness(), nullptr, &all_pieces);
        for (const CutSegmentDescriptor &seg : all)
            if (seg.part_volume == m_merged_scope->id()) {
                m_preview_status = CutSegmentationStatus::Ok;
                if (m_preview_chunks && seg.index < int(all_pieces.size()) && ! all_pieces[seg.index].indices.empty()) {
                    m_segment_models.resize(1);
                    m_segment_models[0].init_from(all_pieces[seg.index]);
                    m_segment_models[0].set_color(cut_segment_color(seg.index, all.size(), filament));
                }
                return;
            }
        return;
    }
    segs = derive_cut_segments(merged, m_preview_planes, instance_matrix(), preview_min_segment_thickness(),
                               mo->cut_planes.user_ordered, &m_preview_status, &pieces, mo->name);
    if (m_merged_scope == nullptr) {
        // Directed parts print as their own segments after the partition (or after a root made
        // of the undirected parts when there is no partition) -- show them the same way.
        append_part_segments(*mo, merged, instance_matrix(), segs, &pieces);
        if (! segs.empty() && m_preview_status == CutSegmentationStatus::StackInactive)
            m_preview_status = CutSegmentationStatus::Ok;
    }
    if (segs.empty())
        return;

    m_plane_splits.assign(m_preview_planes.size(), false);
    m_plane_reach.assign(m_preview_planes.size(), 0.);
    for (const CutSegmentDescriptor &seg : segs) {
        for (const CutSegmentClip &clip : seg.clips)
            if (clip.plane_index >= 0 && clip.plane_index < int(m_plane_splits.size()))
                m_plane_splits[clip.plane_index] = true;
        // A chunk is built along the plane it is seated on, so that plane's tilt limit is set by
        // the largest chunk sitting on it. Several can, when one plane cuts a part into disjoint
        // pieces, and then the biggest governs -- the limit has to hold for all of them.
        if (seg.base_plane >= 0 && seg.base_plane < int(m_plane_reach.size()))
            m_plane_reach[seg.base_plane] = std::max(m_plane_reach[seg.base_plane], seg.build_reach_max);
    }

    if (! m_preview_chunks)
        return; // descriptors and plane annotations only; no GPU upload while the preview is off

    m_segment_models.resize(pieces.size());
    for (size_t i = 0; i < pieces.size(); ++ i) {
        if (pieces[i].indices.empty())
            continue;
        m_segment_models[i].init_from(pieces[i]);
        m_segment_models[i].set_color(cut_segment_color(i, pieces.size(), filament));
    }
}

void GLGizmoCutPlanes::render_segment_preview()
{
    if (m_segment_models.empty())
        return;
    GLShaderProgram *shader = wxGetApp().get_shader("gouraud_light");
    if (shader == nullptr)
        return;

    const Camera     &camera = wxGetApp().plater()->get_camera();
    const Transform3d world  = instance_matrix();
    const Transform3d view_model = camera.get_view_matrix() * world;

    // Set every state this pass depends on and put it back afterwards. Inheriting it from whatever
    // ran before is how a solid opaque preview ends up blended, depth-masked away, or lit by
    // another pass's normal matrix.
    GLboolean prev_blend = GL_FALSE, prev_cull = GL_FALSE, prev_depth_test = GL_FALSE, prev_depth_mask = GL_TRUE;
    GLint     prev_front_face = GL_CCW;
    glsafe(::glGetBooleanv(GL_BLEND, &prev_blend));
    glsafe(::glGetBooleanv(GL_CULL_FACE, &prev_cull));
    glsafe(::glGetBooleanv(GL_DEPTH_TEST, &prev_depth_test));
    glsafe(::glGetBooleanv(GL_DEPTH_WRITEMASK, &prev_depth_mask));
    glsafe(::glGetIntegerv(GL_FRONT_FACE, &prev_front_face));

    glsafe(::glDisable(GL_BLEND));
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDepthMask(GL_TRUE));
    glsafe(::glEnable(GL_CULL_FACE));
    // A mirrored or negatively-scaled instance reverses triangle winding; culling with the default
    // front face would then cull exactly the faces that should be drawn (the same correction
    // GLVolume and the painter gizmos apply).
    const bool mirrored = world.matrix().determinant() < 0.0;
    glsafe(::glFrontFace(mirrored ? GL_CW : GL_CCW));

    shader->start_using();
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    shader->set_uniform("emission_factor", 0.f);
    shader->set_uniform("view_model_matrix", view_model);
    // gouraud_light lights every fragment through view_normal_matrix; leaving it at whatever the
    // previous user of this shader set makes the preview's shading (and so its apparent colour)
    // wrong in a way that changes with what else is on screen.
    shader->set_uniform("view_normal_matrix", (Matrix3d) view_model.matrix().block(0, 0, 3, 3).inverse().transpose());
    for (GLModel &model : m_segment_models)
        if (model.is_initialized())
            model.render();
    shader->stop_using();

    glsafe(::glFrontFace(prev_front_face));
    if (! prev_cull)
        glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glDepthMask(prev_depth_mask));
    if (! prev_depth_test)
        glsafe(::glDisable(GL_DEPTH_TEST));
    if (prev_blend)
        glsafe(::glEnable(GL_BLEND));
}

} // namespace GUI
} // namespace Slic3r
