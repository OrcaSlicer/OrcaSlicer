#include "GLGizmoPathCut.hpp"

#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/CameraUtils.hpp"
#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "slic3r/GUI/GLShader.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoBase.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosCommon.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoCut.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosManager.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include <optional>
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/MeshUtils.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Selection.hpp"

#include "libslic3r/AABBMesh.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PathCutConnectors.hpp"
#include "libslic3r/PathCutGeometry.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/libslic3r.h"

#include <boost/log/trivial.hpp>
#include <glad/gl.h>
#include <imgui/imgui.h>
#include <wx/app.h>
#include <wx/glcanvas.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <utility>
#include <string>
#include <thread>
#include <vector>

namespace Slic3r {
namespace GUI {

static const ColorRGBA PATH_COLOR  = {0.f, 0.588f, 0.533f, 1.f}; // Orca teal
static const ColorRGBA POINT_COLOR = {1.f, 0.55f, 0.f, 1.f};
// Preview: the piece that stays, the detached one (drawn apart) and the connectors.
static const ColorRGBA PREVIEW_STAY_COLOR  = {0.55f, 0.72f, 0.70f, 1.f};
static const ColorRGBA PREVIEW_MOVED_COLOR = {0.95f, 0.62f, 0.25f, 1.f};
static const ColorRGBA PREVIEW_PEG_COLOR   = {0.15f, 0.75f, 0.35f, 1.f};
static const ColorRGBA HOVER_FITS_COLOR    = {0.2f, 1.f, 0.4f, 0.5f};  // connector under the mouse: it fits
static const ColorRGBA HOVER_NO_FIT_COLOR  = {1.f, 0.15f, 0.15f, 0.6f}; // it does not fit there
static const ColorRGBA HOVER_FORCED_COLOR  = {1.f, 0.6f, 0.05f, 0.6f};  // it does not fit there, but goes there anyway
static const ColorRGBA ORB_RING_COLOR[2] = {{0.9f, 0.2f, 0.2f, 1.f}, {0.2f, 0.8f, 0.2f, 1.f}}; // like Orca's X / Y rings
static const ColorRGBA ORB_HOVER_COLOR     = {1.f, 0.9f, 0.2f, 1.f};
static const ColorRGBA ORB_AXIS_COLOR      = {0.f, 0.588f, 0.533f, 1.f};
static const ColorRGBA DONE_CUT_COLOR      = {1.f, 0.55f, 0.f, 0.45f};  // faces of the cuts already made on the object

// What of the connectors is shown on the cut face: the pegs, or the holes of the pins.
static void init_connector_model(GLModel &model, const PathCut::ConnectorResult &c)
{
    model.reset();
    const indexed_triangle_set &shown = c.pegs.indices.empty() ? c.holes_to : c.pegs;
    GLModel::Geometry           g;
    g.format = {GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3};
    for (const auto &v : shown.vertices) g.add_vertex(Vec3f(v));
    for (const auto &t : shown.indices) g.add_triangle(unsigned(t[0]), unsigned(t[1]), unsigned(t[2]));
    if (g.vertices_count() > 0)
        model.init_from(std::move(g));
}

// Messages produced by libslic3r/PathCutGeometry.hpp, listed here so xgettext picks them up.
[[maybe_unused]] static const char *const PATH_CUT_MESSAGES[] = {
    L("The path needs at least two points."),
    L("The path must not end where it starts. Use the loop mode for closed paths."),
    L("The loop needs at least three points."),
    L("The loop is degenerate (it encloses no area)."),
    L("The loop folds back on itself seen from its mean plane. Place the points more evenly around the part."),
    L(L("The path did not split the object. Make sure it crosses the object completely.")),
    L(L("The loop did not detach anything. Make sure it goes all the way around the part.")),
    L("The loop is too irregular. Place the points more evenly around the part."),
    L("The loop does not go all the way around the part. Place the points around it, or lower the cap margin."),
    L("The model has holes along the cut, so the cut faces cannot be closed. Repair the model first."),
    L("The cap spanning the loop runs into another part of the model. Place the loop where the part is free all around."),
    L("The object was changed while it was being cut. Cut it again."),
    L("The cut could not follow the loop along the surface. Place more points, closer together, along the line where you want to cut, or raise the margin."),
    L("The cut failed. Try a simpler path or a lower resolution model."),
    L("The cut ends inside the object. Increase or remove the limits, or start and end the path beyond the part."),
    L("The path crosses itself seen from the view. Place the points in order along the cut line."),
    L("No connectors fit: the cut face is too curved for a straight connector."),
    L("No connectors fit: the cut face is too small or too thin for them. Make them smaller or shorter."),
    L("The connector does not fit there: too close to the edge of the cut face."),
    L("The connector does not fit there: the cut face is too steep or uneven under it."),
    L("The connector does not fit there: the part is too thin for its depth. Make it shorter or try another spot."),
    L("The connector does not fit there: it would run into a hole that is already there."),
    L("The connector does not fit there: too close to another connector."),
    L("Click on the cut face to place a connector."),
};

GLGizmoPathCut::GLGizmoPathCut(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
{}

bool GLGizmoPathCut::on_init() { return true; }

std::string GLGizmoPathCut::on_get_name() const { return _u8L("Cut along path"); }

// Placing connectors: where the one under the mouse does not fit, say why.
std::string GLGizmoPathCut::get_tooltip() const
{
    if (m_placing && (m_orb_ring >= 0 || m_orb_hover >= 0))
        return orb_tooltip();
    if (m_placing && m_hover_on && m_hover_fit != PathCut::ConnectorFit::Ok)
        return force_connector(m_hover_fit) ? _u8L("It does not fit there, but it will be placed anyway: check the pieces.") :
                                              _u8L(PathCut::connector_fit_message(m_hover_fit));
    return "";
}

bool GLGizmoPathCut::on_is_activable() const
{
    const Selection &selection = m_parent.get_selection();
    return selection.is_single_full_instance() && !selection.is_wipe_tower();
}

CommonGizmosDataID GLGizmoPathCut::on_get_requirements() const
{
    if (m_c && m_c->raycaster_ptr())
        m_c->raycaster_ptr()->set_only_support_model_part_flag(false);
    return CommonGizmosDataID(int(CommonGizmosDataID::SelectionInfo) | int(CommonGizmosDataID::InstancesHider) |
                              int(CommonGizmosDataID::Raycaster) | int(CommonGizmosDataID::ObjectClipper));
}

static void abort_job(PathCutJob &job); // below, after PathCutJob
static bool job_object_unchanged(const PathCutJob &job);
static bool job_joined(const PathCutJob &job);

void GLGizmoPathCut::on_set_state()
{
    if (m_state == Off) {
        // A cut still running when the tool closes is cancelled, and its result dropped (it would
        // otherwise apply later, on whatever the user is doing by then).
        // (Its own flag, not the worker's cancel: that would hit whatever job is running at the time.)
        ++m_generation;
        if (m_running)
            abort_job(*m_running);
        m_running.reset();
        m_busy = false;
        m_creases.reset(); // rebuilt for the object the tool opens on next
        m_creases_object = nullptr;
    }
    if (m_state == On || m_state == Off)
        reset_path();
    if (m_state == On)
        sync_done();
    m_message.clear();
}

void GLGizmoPathCut::reset_path()
{
    clear_pending();
    m_points.clear();
    m_loop_pts.clear();
    m_object        = nullptr;
    m_preview_dirty = true;
}

bool GLGizmoPathCut::raycast(const Vec2d &mouse_pos, Vec3d &hit_world) const
{
    const ModelObject *mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    const int          inst = m_parent.get_selection().get_instance_idx();
    if (mo == nullptr || inst < 0 || inst >= int(mo->instances.size()) || m_c->raycaster() == nullptr)
        return false;
    const auto        &raycasters = m_c->raycaster()->raycasters();
    const Camera      &camera     = wxGetApp().plater()->get_camera();
    const Transform3d  inst_m     = mo->instances[inst]->get_transformation().get_matrix();
    double             best       = std::numeric_limits<double>::max();
    bool               found      = false;
    for (size_t i = 0; i < mo->volumes.size() && i < raycasters.size(); ++i) {
        if (!mo->volumes[i]->is_model_part())
            continue;
        const Transform3d trafo = inst_m * mo->volumes[i]->get_matrix();
        Vec3f             hit, normal;
        if (raycasters[i]->unproject_on_mesh(mouse_pos, trafo, camera, hit, normal, m_c->object_clipper()->get_clipping_plane())) {
            const Vec3d  w = trafo * hit.cast<double>();
            const double d = (camera.get_position() - w).squaredNorm();
            if (d < best) {
                best      = d;
                hit_world = w;
                found     = true;
            }
        }
    }
    return found;
}

void GLGizmoPathCut::add_point(const Vec3d &hit)
{
    const ModelObject *mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (mo == nullptr)
        return;
    if (m_object != mo) {
        m_points.clear();
        m_object = mo;
    }
    if (m_points.empty()) {
        m_view_dir = wxGetApp().plater()->get_camera().get_dir_forward();
        const int inst = m_parent.get_selection().get_instance_idx();
        if (inst >= 0 && inst < int(mo->instances.size()))
            m_inst_matrix = mo->instances[inst]->get_matrix();
    }
    clear_pending();
    m_points.push_back(hit);
    m_message.clear();
    m_preview_dirty = true;
    m_parent.set_as_dirty();
}

bool GLGizmoPathCut::on_mouse(const wxMouseEvent &mouse_event)
{
    if (m_busy) // the scene is frozen while a cut runs (zooming is fine)
        return mouse_event.GetWheelRotation() == 0;
    const Vec2d mouse_pos(mouse_event.GetX(), mouse_event.GetY());
    if (m_placing && on_orb_mouse(mouse_event, mouse_pos))
        return true;
    if (m_placing && mouse_event.Moving()) {
        if (m_orb_hover >= 0)
            clear_hover(); // on a ring of the orb: no connector shown under the mouse
        else
            update_hover(mouse_pos);
        return false; // the canvas still tracks the mouse
    }
    if (mouse_event.Leaving())
        clear_hover();
    if (mouse_event.LeftDown())
        m_press_on_object = m_orbiting = false; // a lost release must not leave a press behind
    if (mouse_event.LeftDown() && !mouse_event.CmdDown()) {
        Vec3d      hit;
        const bool on_part = m_placing ? raycast_face(mouse_pos, hit) : raycast(mouse_pos, hit);
        // Empty space: the canvas orbits the view. On a modifier or negative volume (not cut, so no point)
        // the press is still kept from the canvas, which would otherwise start moving the object.
        // While a computed cut is shown the object is hidden, so a click on the pieces drawn apart would
        // reach the canvas as a click on empty space and deselect the object, leaving the tool unable to
        // preview again: every press is kept then, and a drag orbits the view here.
        if (!on_part && m_parent.get_first_hover_volume_idx() < 0 && !m_pending)
            return false;
        // On the object: a click adds a point (on release), a drag orbits the view.
        m_press_on_object  = true;
        m_press_adds_point = on_part && !previewing(); // the preview is changed with "Edit", not by clicks
        m_orbiting         = false;
        m_press_pos = m_last_pos = mouse_pos;
        m_press_hit        = hit;
        return true;
    }
    if (m_press_on_object && mouse_event.Dragging() && mouse_event.LeftIsDown()) {
        if (!m_orbiting && (mouse_pos - m_press_pos).norm() > 4.) {
            m_orbiting = true;
            clear_hover();
            if (!m_parent.has_mouse_capture())
                m_parent.get_wxglcanvas()->CaptureMouse(); // released by the canvas on button up
        }
        // Orbit like the canvas does on empty space (GLCanvas3D, TRACKBALLSIZE = 0.8), with the user's
        // settings, around the selected object. Left drag mapped to something else: just no point.
        const AppConfig *cfg = wxGetApp().app_config;
        if (m_orbiting && std::atoi(cfg->get("left_mouse_drag_action").c_str()) == int(GLCanvas3D::MouseAction::Rotation)) {
            const std::string mult_pref = cfg->get("camera_orbit_mult");
            const double      mult      = mult_pref.empty() ? 1. : std::stod(mult_pref);
            const Vec2d       rot       = (mouse_pos - m_last_pos) * (PI * 0.8 / 180.) * mult;
            Camera           &camera    = wxGetApp().plater()->get_camera();
            if (cfg->get_bool("use_free_camera"))
                camera.rotate_local_around_target(Vec3d(rot.y(), rot.x(), 0.));
            else {
                camera.recover_from_free_camera();
                camera.rotate_on_sphere_with_target(rot.x(), rot.y(), true, m_parent.get_selection().get_bounding_box().center());
            }
            camera.auto_type(Camera::EType::Perspective);
            m_parent.set_as_dirty();
        }
        m_last_pos = mouse_pos;
        return true;
    }
    if (m_press_on_object && mouse_event.LeftUp()) {
        m_press_on_object = false;
        if (!m_orbiting && m_press_adds_point) {
            if (m_placing) {
                add_connector(m_press_hit);
                update_hover(mouse_pos); // the new connector is there now
            } else
                add_point(m_press_hit);
        }
        m_orbiting = false;
        return true;
    }
    // Right click removes the last point; a right drag pans the view as usual.
    if (mouse_event.RightDown()) {
        m_right_press = true;
        m_right_pos   = mouse_pos;
        return false;
    }
    if (mouse_event.RightUp() && m_right_press) {
        m_right_press = false;
        if ((mouse_pos - m_right_pos).norm() <= 4. && m_placing) {
            if (!m_spots.empty()) {
                m_spots.pop_back();
                rebuild_connector_preview();
                update_hover(mouse_pos);
                m_message.clear();
                m_parent.set_as_dirty();
            }
            return true;
        }
        if ((mouse_pos - m_right_pos).norm() <= 4. && !m_points.empty() && !previewing()) {
            clear_pending();
            m_points.pop_back();
            m_preview_dirty = true;
            m_parent.set_as_dirty();
            return true;
        }
    }
    return false;
}

std::vector<Vec3d> GLGizmoPathCut::preview_curve() const
{
    if (m_mode == Mode::Loop && !m_loop_pts.empty()) { // along the creases: the path itself
        std::vector<Vec3d> out(m_loop_pts);
        out.push_back(out.front());
        return out;
    }
    std::vector<PathCut::V3> pts(m_points.begin(), m_points.end());
    const bool closed = m_mode == Mode::Loop && pts.size() >= 3;
    std::vector<PathCut::V3> c = PathCut::catmull_rom(pts, closed, 12);
    std::vector<Vec3d>       out(c.begin(), c.end());
    if (closed && !out.empty())
        out.push_back(out.front());
    return out;
}

// The view direction locked at the first point, or forced level with the bed or vertical.
Vec3d GLGizmoPathCut::sweep_dir() const
{
    switch (m_sweep) {
    case Sweep::Level: {
        const Vec3d d(m_view_dir.x(), m_view_dir.y(), 0.);
        return d.norm() > 1e-6 ? Vec3d(d.normalized()) : Vec3d(Vec3d::UnitY());
    }
    case Sweep::Vertical: return m_view_dir.z() > 0. ? Vec3d(Vec3d::UnitZ()) : Vec3d(-Vec3d::UnitZ());
    case Sweep::Custom: {
        const double turn = m_turn * PI / 180., tilt = m_tilt * PI / 180.;
        return Vec3d(std::cos(tilt) * std::cos(turn), std::cos(tilt) * std::sin(turn), -std::sin(tilt));
    }
    default: return m_view_dir;
    }
}

// Loop mode, following the creases: the loop through the picked points along the model's creases.
void GLGizmoPathCut::update_loop_points()
{
    m_loop_pts.clear();
    if (m_mode != Mode::Loop || !m_follow_creases || m_points.size() < 3)
        return;
    const ModelObject *mo   = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    const int          inst = m_parent.get_selection().get_instance_idx();
    if (mo == nullptr || inst < 0 || inst >= int(mo->instances.size()))
        return;
    const Transform3d &matrix = mo->instances[inst]->get_matrix();
    size_t             tris   = 0;
    for (const ModelVolume *v : mo->volumes)
        if (v->is_model_part()) tris += v->mesh().its.indices.size();
    if (!m_creases || mo != m_creases_object || tris != m_creases_tris || !matrix.isApprox(m_creases_matrix)) {
        indexed_triangle_set world;
        for (const ModelVolume *v : mo->volumes)
            if (v->is_model_part()) {
                indexed_triangle_set its = v->mesh().its;
                its_transform(its, matrix * v->get_matrix(), true);
                its_merge(world, its);
            }
        m_creases        = std::make_shared<PathCut::CreaseGraph>(world);
        m_creases_object = mo;
        m_creases_tris   = tris;
        m_creases_matrix = matrix;
    }
    const std::vector<PathCut::V3> picked(m_points.begin(), m_points.end());
    for (const PathCut::V3 &p : m_creases->loop(picked)) m_loop_pts.push_back(p);
}

// The points the cut is built from: the loop along the creases, or the picked points.
std::vector<Vec3d> GLGizmoPathCut::cut_points() const
{
    return m_mode == Mode::Loop && !m_loop_pts.empty() ? m_loop_pts : m_points;
}

void GLGizmoPathCut::rebuild_preview()
{
    update_loop_points();
    m_curve.reset();
    const std::vector<Vec3d> curve = preview_curve();
    if (curve.size() >= 2) {
        GLModel::Geometry g;
        g.format = {GLModel::Geometry::EPrimitiveType::LineStrip, GLModel::Geometry::EVertexLayout::P3};
        for (size_t i = 0; i < curve.size(); ++i) {
            g.add_vertex(Vec3f(curve[i].cast<float>()));
            g.add_index(unsigned(i));
        }
        m_curve.init_from(std::move(g));
    }
    if (!m_sphere.is_initialized())
        m_sphere.init_from(smooth_sphere(16, 1.f));
    rebuild_cutter_preview();
    m_routes_model.reset();
    if (!m_routes.empty()) {
        GLModel::Geometry g;
        g.format = {GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3};
        for (const auto &route : m_routes)
            for (size_t i = 0; i + 1 < route.size(); ++i) {
                const unsigned base = unsigned(g.vertices_count());
                g.add_vertex(Vec3f(route[i].cast<float>()));
                g.add_vertex(Vec3f(route[i + 1].cast<float>()));
                g.add_line(base, base + 1);
            }
        if (g.vertices_count() > 0)
            m_routes_model.init_from(std::move(g));
    }
    m_preview_dirty = false;
}

PathCut::ViewLimits GLGizmoPathCut::view_limits() const
{
    PathCut::ViewLimits l;
    l.front = m_lim_front.on ? m_lim_front.mm : -1.;
    l.back  = m_lim_back.on ? m_lim_back.mm : -1.;
    l.start = m_lim_start.on ? m_lim_start.mm : -1.;
    l.end   = m_lim_end.on ? m_lim_end.mm : -1.;
    return l;
}

// Translucent preview of what will cut: the wall swept from the view path, or the cap spanning the loop
// (at the minimum margin; the cut may widen it).
void GLGizmoPathCut::rebuild_cutter_preview()
{
    m_cutter.reset();
    const std::vector<Vec3d>       cut = cut_points();
    const std::vector<PathCut::V3> pts(cut.begin(), cut.end());
    const BoundingBoxf3            bb = m_parent.get_selection().get_bounding_box();
    if (!bb.defined)
        return;
    GLModel::Geometry g;
    g.format = {GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3};
    if (m_mode == Mode::ViewPath && pts.size() >= 2) {
        const PathCut::ViewCutter vc = PathCut::make_view_cutter(pts, sweep_dir(), bb.min, bb.max, m_kerf, view_limits());
        if (!vc.valid())
            return;
        double lo = std::numeric_limits<double>::max(), hi = -lo;
        for (int c = 0; c < 8; ++c) {
            const double h = vc.frame.height(Vec3d(c & 1 ? bb.max.x() : bb.min.x(), c & 2 ? bb.max.y() : bb.min.y(), c & 4 ? bb.max.z() : bb.min.z()));
            lo = std::min(lo, h);
            hi = std::max(hi, h);
        }
        if (vc.limit_depth) {
            lo = std::max(lo, vc.h_min);
            hi = std::min(hi, vc.h_max);
        }
        // Clip the unlimited extensions to a little past the object.
        const double reach = (bb.max - bb.min).norm() * 0.6;
        for (size_t i = 0; i + 1 < vc.path.size(); ++i) {
            PathCut::V2 a = vc.path[i], b = vc.path[i + 1];
            const PathCut::V2 c = vc.frame.to2d(bb.center());
            if ((i == 0 && !vc.limit_start) || (i + 2 == vc.path.size() && !vc.limit_end)) {
                if ((a - c).norm() > reach) a = c + (a - c).normalized() * reach;
                if ((b - c).norm() > reach) b = c + (b - c).normalized() * reach;
            }
            const unsigned base = unsigned(g.vertices_count());
            g.add_vertex(Vec3f(vc.frame.to3d(a, lo).cast<float>()));
            g.add_vertex(Vec3f(vc.frame.to3d(b, lo).cast<float>()));
            g.add_vertex(Vec3f(vc.frame.to3d(b, hi).cast<float>()));
            g.add_vertex(Vec3f(vc.frame.to3d(a, hi).cast<float>()));
            g.add_triangle(base, base + 1, base + 2);
            g.add_triangle(base, base + 2, base + 3);
        }
    } else if (m_mode == Mode::Loop && pts.size() >= 3) {
        const PathCut::LoopCutter lc = PathCut::make_loop_cutter(pts, m_kerf, m_margin, [](const PathCut::V3 &p) { return p; }, m_flat_cap);
        if (!lc.valid())
            return;
        constexpr int rings = 8, sectors = 72;
        g.add_vertex(Vec3f(lc.frame.to3d(lc.center2d, lc.cap_height(lc.center2d)).cast<float>()));
        for (int k = 1; k <= rings + 1; ++k)
            for (int j = 0; j < sectors; ++j) {
                const double a = 2. * PI * j / sectors;
                double       r, h;
                lc.spoke_at(a, r, h);
                // The flat cap reaches past the loop; the exact cut stops at the loop.
                const double            rr = k <= rings ? r * k / rings : r + (m_flat_cap ? lc.margin : 0.);
                const PathCut::V2 q  = lc.center2d + PathCut::V2(std::cos(a), std::sin(a)) * rr;
                g.add_vertex(Vec3f(lc.frame.to3d(q, lc.cap_height(q)).cast<float>()));
            }
        for (int j = 0; j < sectors; ++j) {
            const unsigned j1 = unsigned((j + 1) % sectors);
            g.add_triangle(0, 1 + unsigned(j), 1 + j1);
            for (int k = 1; k <= rings; ++k) {
                const unsigned a0 = 1 + unsigned((k - 1) * sectors), a1 = 1 + unsigned(k * sectors);
                g.add_triangle(a0 + unsigned(j), a1 + unsigned(j), a1 + j1);
                g.add_triangle(a0 + unsigned(j), a1 + j1, a0 + j1);
            }
        }
    }
    if (g.vertices_count() > 0)
        m_cutter.init_from(std::move(g));
}

void GLGizmoPathCut::on_render()
{
    if (!previewing() && !m_placing)
        render_done();
    if (m_points.empty())
        return;
    if (m_preview_dirty)
        rebuild_preview();

    const Camera &camera = wxGetApp().plater()->get_camera();
    glsafe(::glDisable(GL_DEPTH_TEST));

    if (m_placing && m_pending) {
        if (GLShaderProgram *shader = wxGetApp().get_shader("flat")) {
            glsafe(::glDisable(GL_CULL_FACE));
            glsafe(::glEnable(GL_BLEND));
            glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
            shader->start_using();
            shader->set_uniform("view_model_matrix", camera.get_view_matrix() * Geometry::translation_transform(m_placing_offset));
            shader->set_uniform("projection_matrix", camera.get_projection_matrix());
            if (m_face_model.is_initialized()) {
                m_face_model.set_color(ColorRGBA(1.f, 0.55f, 0.f, 0.7f));
                m_face_model.render();
            }
            if (m_conn_model.is_initialized()) {
                m_conn_model.set_color(ColorRGBA(0.1f, 0.85f, 0.3f, 0.85f));
                m_conn_model.render();
            }
            if (m_hover_on && m_hover_model.is_initialized() && m_orb_hover < 0 && m_orb_ring < 0) {
                m_hover_model.set_color(m_hover_fit == PathCut::ConnectorFit::Ok ? HOVER_FITS_COLOR :
                                        force_connector(m_hover_fit)    ? HOVER_FORCED_COLOR :
                                                                          HOVER_NO_FIT_COLOR);
                m_hover_model.render();
            }
            shader->stop_using();
            glsafe(::glDisable(GL_BLEND));
            glsafe(::glEnable(GL_CULL_FACE));
        }
        render_orb();
    } else if (previewing()) {
        if (GLShaderProgram *shader = wxGetApp().get_shader("gouraud_light")) {
            shader->start_using();
            shader->set_uniform("projection_matrix", camera.get_projection_matrix());
            shader->set_uniform("emission_factor", 0.1f);
            const Transform3d &view = camera.get_view_matrix();
            auto draw = [&](GLModel &m, const Vec3d &shift, const ColorRGBA &color) {
                if (!m.is_initialized())
                    return;
                const Transform3d model = Geometry::translation_transform(m_preview_offset + shift);
                shader->set_uniform("view_model_matrix", view * model);
                shader->set_uniform("view_normal_matrix", Matrix3d(view.matrix().block(0, 0, 3, 3)));
                m.set_color(color);
                m.render();
            };
            glsafe(::glEnable(GL_DEPTH_TEST));
            draw(m_piece_stay, Vec3d::Zero(), PREVIEW_STAY_COLOR);
            draw(m_piece_moved, m_piece_shift, PREVIEW_MOVED_COLOR);
            draw(m_preview_pegs, m_pegs_on_moved ? m_piece_shift : Vec3d::Zero(), PREVIEW_PEG_COLOR);
            glsafe(::glDisable(GL_DEPTH_TEST));
            shader->stop_using();
        }
    } else if (m_cutter.is_initialized()) {
        if (GLShaderProgram *shader = wxGetApp().get_shader("flat")) {
            glsafe(::glDisable(GL_CULL_FACE));
            glsafe(::glEnable(GL_BLEND));
            glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
            shader->start_using();
            shader->set_uniform("view_model_matrix", camera.get_view_matrix());
            shader->set_uniform("projection_matrix", camera.get_projection_matrix());
            m_cutter.set_color(ColorRGBA(PATH_COLOR.r(), PATH_COLOR.g(), PATH_COLOR.b(), 0.25f));
            m_cutter.render();
            shader->stop_using();
            glsafe(::glDisable(GL_BLEND));
            glsafe(::glEnable(GL_CULL_FACE));
        }
    }

    if (m_routes_model.is_initialized()) {
        if (GLShaderProgram *shader = wxGetApp().get_shader("flat")) {
            shader->start_using();
            shader->set_uniform("view_model_matrix", camera.get_view_matrix());
            shader->set_uniform("projection_matrix", camera.get_projection_matrix());
            shader->set_uniform("width", 0.6f);
            m_routes_model.set_color(ColorRGBA(1.f, 0.15f, 0.15f, 1.f));
            m_routes_model.render();
            shader->stop_using();
        }
    }

    // The path and its points are drawn twice: faint through everything, so the whole path stays
    // readable, then solid where nothing is in front of them, so a point behind the object is told
    // from one in front at a glance. The path lies on the surface: the solid pass is drawn a hair towards the viewer, so the surface
    // it runs on does not hide it.
    const BoundingBoxf3 sel_bb   = m_parent.get_selection().get_bounding_box();
    const Vec3d         toward   = -camera.get_dir_forward() * std::clamp(sel_bb.size().norm() * 0.003, 0.05, 0.6);
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    for (const bool hidden_pass : {true, false}) {
        const Transform3d bias = hidden_pass ? Transform3d::Identity() : Transform3d(Geometry::translation_transform(toward));
        if (hidden_pass)
            glsafe(::glDisable(GL_DEPTH_TEST));
        else
            glsafe(::glEnable(GL_DEPTH_TEST));
        const float alpha = hidden_pass ? 0.25f : 1.f;
        if (m_curve.is_initialized()) {
            if (GLShaderProgram *shader = wxGetApp().get_shader("flat")) {
                shader->start_using();
                shader->set_uniform("view_model_matrix", camera.get_view_matrix() * bias);
                shader->set_uniform("projection_matrix", camera.get_projection_matrix());
                shader->set_uniform("width", 0.4f);
                m_curve.set_color(ColorRGBA(PATH_COLOR.r(), PATH_COLOR.g(), PATH_COLOR.b(), alpha));
                m_curve.render();
                shader->stop_using();
            }
        }
        if (GLShaderProgram *shader = wxGetApp().get_shader("gouraud_light")) {
            shader->start_using();
            shader->set_uniform("projection_matrix", camera.get_projection_matrix());
            shader->set_uniform("emission_factor", 0.5f);
            const Transform3d &view = camera.get_view_matrix();
            const Matrix4d    &proj = camera.get_projection_matrix().matrix();
            const double       vp_h = std::max(1, int(camera.get_viewport()[3]));
            // A few pixels across however far the view is zoomed in (never larger than before, on a small
            // part seen from far away).
            const double max_r = std::clamp(sel_bb.size().norm() * 0.004, 0.15, 1.2);
            m_sphere.set_color(ColorRGBA(POINT_COLOR.r(), POINT_COLOR.g(), POINT_COLOR.b(), alpha));
            for (const Vec3d &p : m_points) {
                // World size of one pixel at the point: 2 w / (P11 h), w its clip w (its depth in perspective).
                const Vec4d  pv     = proj * (view * Vec4d(p.x(), p.y(), p.z(), 1.));
                const double px     = 2. * std::abs(pv.w()) / (std::abs(proj(1, 1)) * vp_h);
                const double radius = std::min(max_r, 4. * px);
                const Transform3d model = bias * Geometry::translation_transform(p) * Geometry::scale_transform(radius);
                shader->set_uniform("view_model_matrix", view * model);
                const Matrix3d view_normal = view.matrix().block(0, 0, 3, 3) * model.matrix().block(0, 0, 3, 3).inverse().transpose();
                shader->set_uniform("view_normal_matrix", view_normal);
                m_sphere.render();
            }
            shader->stop_using();
        }
    }
    glsafe(::glDisable(GL_BLEND));
    glsafe(::glEnable(GL_DEPTH_TEST));
}

void GLGizmoPathCut::on_render_input_window(float x, float y, float bottom_limit)
{
    // A pending cut was computed with the old settings: drop it when they change.
    auto settings_changed = [this]() {
        clear_pending();
        m_preview_dirty = true;
    };
    const float scale = m_parent.get_scale();
    ImGuiWrapper::push_toolbar_style(scale);
    // Drop-down lists: dark text on white, in either theme. (Under the toolbar style the open list was
    // drawn black with dark green text, unreadable until hovered.)
    auto push_list_style = []() {
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(1.f, 1.f, 1.f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1.f, 1.f, 1.f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.92f, 0.92f, 0.92f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.88f, 0.88f, 0.88f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.12f, 0.12f, 0.12f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.f, 0.59f, 0.53f, 0.30f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.f, 0.59f, 0.53f, 0.18f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.f, 0.59f, 0.53f, 0.45f));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.92f, 0.92f, 0.92f, 1.f)); // the arrow box
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.65f, 0.65f, 0.65f, 1.f));
    };
    auto pop_list_style = []() { ImGui::PopStyleColor(10); };

    // Keep the window inside the canvas: move it up when it gets taller, and scroll beyond that.
    y = std::max(0.f, std::min(y, bottom_limit - m_window_height));
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.f, 0.f), ImVec2(FLT_MAX, std::max(100.f, bottom_limit - y)));
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
    GizmoImguiBegin(on_get_name(), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    const float input_w = ImGui::CalcTextSize("000.00").x * 2.5f;
    // Input fields start right of the longest label in the current language (translations are often much
    // longer than the English text), so no label runs under a field.
    const float gap = ImGui::GetStyle().ItemSpacing.x * 2.f;
    float       label_w = 0.f, check_w = 0.f;
    for (const std::string &l : {_u8L("Clearance (mm)"), _u8L("Min. cap margin (mm)"), _u8L("Max. deviation (mm)"), _u8L("Cut direction"), _u8L("Turn (deg)"),
                                 _u8L("Tilt (deg)"), _u8L("Connectors"), _u8L("Shape"), _u8L("Number of connectors"),
                                 _u8L("Diameter / side (mm)"), _u8L("Depth (mm)"), _u8L("Hole clearance (mm)"), _u8L("Placement")})
        label_w = std::max(label_w, ImGui::CalcTextSize(l.c_str()).x);
    for (const std::string &l : {_u8L("Limit in front (mm)"), _u8L("Limit behind (mm)"), _u8L("Limit past the first point (mm)"),
                                 _u8L("Limit past the last point (mm)")})
        check_w = std::max(check_w, ImGui::CalcTextSize(l.c_str()).x);
    const float field_x = ImGui::GetStyle().WindowPadding.x + label_w + gap;
    // checkbox square + its spacing, generously: the toolbar style draws larger boxes
    const float check_x = ImGui::GetStyle().WindowPadding.x + ImGui::GetFrameHeight() * 1.5f + check_w + gap;

    // Shown as the Curve mode of the Cut tool: the same row leads back to Planar and Dovetail.
    if (const std::optional<GLGizmoCut3D::ModeButton> clicked =
            GLGizmoCut3D::render_mode_buttons(*m_imgui, GLGizmoCut3D::ModeButton::Curve, field_x, scale, !m_busy, m_is_dark_mode))
        wxGetApp().CallAfter([this, mode = *clicked]() {
            GLGizmosManager &manager = m_parent.get_gizmos_manager();
            if (!manager.open_gizmo(GLGizmosManager::Cut))
                return;
            if (auto *cut = dynamic_cast<GLGizmoCut3D *>(manager.get_current()))
                cut->select_mode(mode);
        });
    ImGui::Separator();

    m_imgui->disabled_begin(m_busy);
    // Switching the mode starts a new path. A previewed cut that leaves the object in one piece is made
    // first, so the next cut (in the other mode) goes on from it: the tool opens again on the object.
    auto switch_mode = [this](Mode mode) {
        m_mode = mode;
        if (previewing() && job_joined(*m_pending)) {
            std::shared_ptr<PathCutJob> job = m_pending;
            wxGetApp().CallAfter([this, job]() { if (m_pending == job && previewing()) apply_cut(job); });
        } else
            reset_path();
    };
    if (m_imgui->bbl_radio_button(_u8L("Path from view").c_str(), m_mode == Mode::ViewPath) && m_mode != Mode::ViewPath)
        switch_mode(Mode::ViewPath);
    ImGui::SameLine();
    if (m_imgui->bbl_radio_button(_u8L("Closed loop on surface").c_str(), m_mode == Mode::Loop) && m_mode != Mode::Loop)
        switch_mode(Mode::Loop);

    ImGui::Separator();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.f);
    if (m_mode == Mode::ViewPath)
        m_imgui->text(_u8L("Click points on the object along the line where it should be split. The path is extended past the "
                           "object and cut straight through it in the direction you were looking when you placed the first point."));
    else
        m_imgui->text(_u8L("Click points on the surface all the way around the part to detach (e.g. an arm). The loop closes "
                           "itself; a curved cap spanning it separates only that part."));
    m_imgui->text(_u8L("Click to add a point, right click removes the last one. Drag to rotate the view."));
    ImGui::PopTextWrapPos();
    m_imgui->disabled_end();

    // The actions come first so they stay in view however long the settings below get.
    ImGui::Separator();
    m_imgui->text(GUI::format(_u8L("Points: %1%"), m_points.size()));

    // (button() disables itself, so these take !m_busy as their enable flag rather than nesting
    // inside the disabled block above)
    // Moving the object (or editing it elsewhere) while its preview is shown drops the preview: only
    // what is shown may be applied.
    if (m_pending && !m_busy && !job_object_unchanged(*m_pending)) {
        clear_pending();
        m_message          = _u8L("The object was changed during the preview. Make the preview again.");
        m_message_is_error = true;
        m_preview_dirty    = true;
    }
    const size_t min_pts = m_mode == Mode::Loop ? 3 : 2;
    if (!previewing() && !m_placing) {
        // Editing: place points, then compute the preview. The cut is only performed from the preview.
        if (m_imgui->button(_L("Undo point"), ImVec2(0, 0), !m_points.empty() && !m_busy)) {
            clear_pending();
            m_points.pop_back();
            settings_changed();
        }
        ImGui::SameLine();
        if (m_imgui->button(_L("Clear"), ImVec2(0, 0), !m_points.empty() && !m_busy))
            reset_path();
        ImGui::SameLine();
        if (m_imgui->button(_L("Preview cut"), ImVec2(0, 0), m_points.size() >= min_pts && !m_busy))
            wxGetApp().CallAfter([this]() { perform_cut(); });
        ImGui::SameLine();
        m_imgui->button(_L("Perform cut"), ImVec2(0, 0), false);
    } else if (previewing() && !m_busy) {
        if (m_imgui->button(_L("Edit cut")))
            back_to_edit();
        ImGui::SameLine();
        if (m_imgui->button(_L("Perform cut")))
            wxGetApp().CallAfter([this]() { if (previewing()) apply_cut(m_pending); });
    }
    if (m_busy) {
        ImGui::SameLine();
        // Frees the tool at once, whatever the cut is doing: its result is dropped when it ends.
        if (m_imgui->button(_L("Cancel"))) {
            if (m_running)
                abort_job(*m_running);
            m_running.reset();
            ++m_generation;
            m_busy             = false;
            m_message          = _u8L("Cut cancelled.");
            m_message_is_error = false;
        }
    }

    if (m_placing && m_pending && !m_busy) {
        // Manual connectors: the cut is computed, the user clicks on the cut face where each one goes.
        ImGui::Separator();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.f);
        m_imgui->text(_u8L("Click on the cut face (orange) where each connector should go. Right click removes the last one. "
                           "Then apply the cut."));
        ImGui::PopTextWrapPos();
        m_imgui->text(GUI::format(_L_PLURAL("%1% connector placed", "%1% connectors placed", m_spots.size()), m_spots.size()));
        if (m_imgui->button(_L("Undo connector"), ImVec2(0, 0), !m_spots.empty())) {
            m_spots.pop_back();
            rebuild_connector_preview();
            m_message.clear();
        }
        ImGui::SameLine();
        if (m_imgui->button(_L("Apply cut")))
            wxGetApp().CallAfter([this]() { if (m_pending && m_placing) apply_cut(m_pending); });
        ImGui::SameLine();
        if (m_imgui->button(_L("Cancel"))) {
            clear_pending();
            m_message.clear();
            m_preview_dirty = true;
        }
    }
    if (!m_message.empty()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.f);
        if (m_message_is_error)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.35f, 0.35f, 1.f));
        m_imgui->text(m_message);
        if (m_message_is_error)
            ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
    }

    ImGui::Separator();
    m_imgui->disabled_begin(m_busy);
    ImGui::AlignTextToFramePadding();
    m_imgui->text(_u8L("Clearance (mm)"));
    ImGui::SameLine(field_x);
    ImGui::PushItemWidth(input_w);
    if (ImGui::InputFloat("##pathcut_kerf", &m_kerf, 0.05f, 0.1f, "%.2f"))
        settings_changed();
    m_kerf = std::clamp(m_kerf, 0.02f, 2.f);
    if (m_mode == Mode::Loop) {
        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_flat_cap ? _u8L("Min. cap margin (mm)") : _u8L("Max. deviation (mm)"));
        ImGui::SameLine(field_x);
        if (ImGui::InputFloat("##pathcut_margin", &m_margin, 0.1f, 0.5f, "%.1f"))
            settings_changed();
        m_margin = std::clamp(m_margin, 0.2f, 10.f);
        if (m_imgui->bbl_checkbox(_L("Flat cut face"), m_flat_cap))
            settings_changed();
        if (m_imgui->bbl_checkbox(_L("Follow the model's edges"), m_follow_creases))
            settings_changed();
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_u8L("Between the points you place, the loop follows the sharpest edges of the model nearby "
                                  "(a seam, the line between belly and back), so a few points are enough."),
                             ImGui::GetFontSize() * 20.f);
    } else {
        // Direction the path is swept along: as seen when placing the first point, forced level with the
        // bed (a cut parallel to the bed for a path drawn from the front), vertical, or set by hand.
        ImGui::AlignTextToFramePadding();
        m_imgui->text(_u8L("Cut direction"));
        ImGui::SameLine(field_x);
        const std::vector<std::string> sweeps{_u8L("From the view"), _u8L("Level with the bed"), _u8L("Vertical"), _u8L("Custom")};
        const int sweep = int(m_sweep);
        ImGui::PushItemWidth(input_w * 2.2f);
        push_list_style();
        if (ImGui::BeginCombo("##pathcut_sweep", sweeps[size_t(sweep)].c_str())) {
            for (int i = 0; i < int(sweeps.size()); ++i)
                if (ImGui::Selectable(sweeps[size_t(i)].c_str(), i == sweep) && i != sweep) {
                    if (Sweep(i) == Sweep::Custom) { // start from the current direction
                        const Vec3d d = sweep_dir();
                        m_turn = float(std::atan2(d.y(), d.x()) * 180. / PI);
                        m_tilt = float(std::asin(std::clamp(-d.z(), -1., 1.)) * 180. / PI);
                    }
                    m_sweep         = Sweep(i);
                    settings_changed();
                }
            ImGui::EndCombo();
        }
        pop_list_style();
        ImGui::PopItemWidth();
        if (m_sweep == Sweep::Custom) {
            ImGui::AlignTextToFramePadding();
            m_imgui->text(_u8L("Turn (deg)"));
            ImGui::SameLine(field_x);
            if (ImGui::InputFloat("##pathcut_turn", &m_turn, 1.f, 15.f, "%.0f"))
                settings_changed();
            m_turn = std::fmod(m_turn + 540.f, 360.f) - 180.f;
            ImGui::AlignTextToFramePadding();
            m_imgui->text(_u8L("Tilt (deg)"));
            ImGui::SameLine(field_x);
            if (ImGui::InputFloat("##pathcut_tilt", &m_tilt, 1.f, 15.f, "%.0f"))
                settings_changed();
            m_tilt = std::clamp(m_tilt, -90.f, 90.f);
        }
        // How far the wall reaches: through everything unless limited, each side on its own.
        auto limit_row = [&](const wxString &label, Limit &lim, const char *id) {
            if (m_imgui->bbl_checkbox(label, lim.on))
                settings_changed();
            if (lim.on) {
                ImGui::SameLine(check_x);
                if (ImGui::InputFloat(id, &lim.mm, 0.5f, 5.f, "%.1f"))
                    settings_changed();
                lim.mm = std::clamp(lim.mm, 0.f, 1000.f);
            }
        };
        limit_row(_L("Limit in front (mm)"), m_lim_front, "##pathcut_front");
        limit_row(_L("Limit behind (mm)"), m_lim_back, "##pathcut_back");
        limit_row(_L("Limit past the first point (mm)"), m_lim_start, "##pathcut_start");
        limit_row(_L("Limit past the last point (mm)"), m_lim_end, "##pathcut_end");
    }

    // Connectors: straight, along the direction the pieces come apart in, placed where they fit. Edited while
    // placing them by hand, the cut is kept and only the connectors are checked again.
    auto conn_changed = [&]() {
        if (m_placing)
            connectors_changed();
        else
            settings_changed();
    };
    ImGui::Separator();
    ImGui::AlignTextToFramePadding();
    m_imgui->text(_u8L("Connectors"));
    ImGui::SameLine(field_x);
    {
        const std::vector<std::string> types{_u8L("None"), _u8L("Peg and hole"), _u8L("Separate pin")};
        const int                      type = std::clamp(int(m_conn.type), 0, int(types.size()) - 1);
        ImGui::PushItemWidth(input_w * 2.2f);
        push_list_style();
        if (ImGui::BeginCombo("##pathcut_conn_type", types[size_t(type)].c_str())) {
            for (int i = 0; i < int(types.size()); ++i)
                if (ImGui::Selectable(types[size_t(i)].c_str(), i == type) && i != type) {
                    m_conn.type = PathCut::ConnectorType(i);
                    conn_changed();
                }
            ImGui::EndCombo();
        }
        pop_list_style();
        ImGui::PopItemWidth();
    }
    if (m_conn.type != PathCut::ConnectorType::None) {
        ImGui::AlignTextToFramePadding();
        m_imgui->text(_u8L("Shape"));
        ImGui::SameLine(field_x);
        if (m_imgui->bbl_radio_button(_u8L("Cylinder").c_str(), m_conn.shape == PathCut::ConnectorShape::Cylinder) &&
            m_conn.shape != PathCut::ConnectorShape::Cylinder) {
            m_conn.shape = PathCut::ConnectorShape::Cylinder;
            conn_changed();
        }
        ImGui::SameLine();
        if (m_imgui->bbl_radio_button(_u8L("Square").c_str(), m_conn.shape == PathCut::ConnectorShape::Square) &&
            m_conn.shape != PathCut::ConnectorShape::Square) {
            m_conn.shape = PathCut::ConnectorShape::Square;
            conn_changed();
        }
        auto float_row = [&](const std::string &label, const char *id, double &value, float step, float lo, float hi, const char *fmt) {
            ImGui::AlignTextToFramePadding();
            m_imgui->text(label);
            ImGui::SameLine(field_x);
            float v = float(value);
            if (ImGui::InputFloat(id, &v, step, step * 5.f, fmt)) {
                value = std::clamp(v, lo, hi);
                conn_changed();
            }
        };
        ImGui::AlignTextToFramePadding();
        m_imgui->text(_u8L("Placement"));
        ImGui::SameLine(field_x);
        if (m_imgui->bbl_radio_button(_u8L("Automatic").c_str(), !m_conn_manual) && m_conn_manual) {
            m_conn_manual = false;
            settings_changed();
        }
        ImGui::SameLine();
        if (m_imgui->bbl_radio_button(_u8L("Manual").c_str(), m_conn_manual) && !m_conn_manual) {
            m_conn_manual = true;
            settings_changed();
        }
        if (!m_conn_manual) {
            ImGui::AlignTextToFramePadding();
            m_imgui->text(_u8L("Number of connectors"));
            ImGui::SameLine(field_x);
            if (ImGui::InputInt("##pathcut_conn_count", &m_conn.count, 1, 1)) {
                m_conn.count = std::clamp(m_conn.count, 1, 8);
                settings_changed();
            }
        }
        float_row(_u8L("Diameter / side (mm)"), "##pathcut_conn_size", m_conn.size, 0.5f, 1.f, 50.f, "%.1f");
        float_row(_u8L("Depth (mm)"), "##pathcut_conn_depth", m_conn.depth, 0.5f, 1.f, 50.f, "%.1f");
        float_row(_u8L("Hole clearance (mm)"), "##pathcut_conn_tol", m_conn.clearance, 0.05f, 0.f, 1.f, "%.2f");
        // All connectors share one direction (or the pieces could not come apart): tilted by hand if wanted.
        float_row(_u8L("Connector tilt (deg)"), "##pathcut_conn_tilt", m_conn.tilt, 1.f, 0.f, 60.f, "%.0f");
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_u8L("Tilts all the connectors away from the direction the pieces come apart in. They stay "
                                  "parallel, so the pieces still come apart, along the new direction."),
                             ImGui::GetFontSize() * 20.f);
        if (m_conn.tilt > 0.)
            float_row(_u8L("Tilt towards (deg)"), "##pathcut_conn_turn", m_conn.turn, 5.f, -180.f, 180.f, "%.0f");
        if (m_conn_manual) {
            if (m_imgui->bbl_checkbox(_L("Place even where it does not fit"), m_conn_force) && m_placing)
                update_hover(m_hover_pos);
            if (ImGui::IsItemHovered())
                m_imgui->tooltip(_u8L("A click places the connector where the checks say it does not fit (shown in orange). "
                                      "Its hole is still cut with the clearance set. Check the pieces before printing."),
                                 ImGui::GetFontSize() * 20.f);
        }
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.f);
        if (m_conn_manual)
            m_imgui->text(_u8L("Straight connectors along the mean direction of the cut face. After the cut is computed, click on "
                               "the cut face where each one should go: each spot is checked for room in both parts. Two or more "
                               "also keep the parts from turning."));
        else
            m_imgui->text(_u8L("Straight connectors along the mean direction of the cut face, placed automatically where both parts "
                               "have room for them. Two or more also keep the parts from turning."));
        ImGui::PopTextWrapPos();
    }
    ImGui::PopItemWidth();
    m_imgui->disabled_end();

    m_window_height = ImGui::GetWindowHeight();
    GizmoImguiEnd();
    ImGuiWrapper::pop_toolbar_style();
}

// Moves the piece (single instance) to the free spot of the current plate nearest to `rest`,
// keeping a gap to the rest and to every other object except `replaced` (the object being cut).
// Falls back to just beside the rest when the plate has no room.
static void place_piece(ModelObject &piece, const BoundingBoxf3 &rest, const ModelObject *replaced,
                        const std::vector<BoundingBoxf3> &also_avoid = {})
{
    constexpr double gap = 5.;
    Plater          *plater = wxGetApp().plater();

    std::vector<BoundingBoxf3> obstacles{rest};
    obstacles.insert(obstacles.end(), also_avoid.begin(), also_avoid.end());
    for (const ModelObject *o : plater->model().objects)
        if (o != replaced)
            for (size_t i = 0; i < o->instances.size(); ++i)
                obstacles.emplace_back(o->instance_bounding_box(i));

    ModelInstance      *pi   = piece.instances[0];
    const BoundingBoxf3 bb   = piece.instance_bounding_box(0);
    const Vec2d         size = (bb.max - bb.min).head<2>();
    const BoundingBoxf3 bed  = plater->get_partplate_list().get_curr_plate()->get_build_volume();

    auto is_free = [&](const Vec2d &min) {
        const Vec2d max = min + size;
        if (min.x() < bed.min.x() || min.y() < bed.min.y() || max.x() > bed.max.x() || max.y() > bed.max.y())
            return false;
        for (const BoundingBoxf3 &o : obstacles)
            if (min.x() < o.max.x() + gap && max.x() > o.min.x() - gap && min.y() < o.max.y() + gap && max.y() > o.min.y() - gap)
                return false;
        return true;
    };

    const Vec2d target(rest.max.x() + gap, rest.min.y()); // default: right of the rest
    Vec2d       best     = target;
    double      best_d2  = std::numeric_limits<double>::max();
    constexpr double step = 2.;
    const Vec3d rc       = rest.center();
    const bool  on_plate = rc.x() >= bed.min.x() && rc.x() <= bed.max.x() && rc.y() >= bed.min.y() && rc.y() <= bed.max.y();
    // Objects on another plate keep the default placement.
    if (on_plate) {
        for (double x = bed.min.x(); x + size.x() <= bed.max.x(); x += step)
            for (double y = bed.min.y(); y + size.y() <= bed.max.y(); y += step) {
                const Vec2d  c(x, y);
                const double d2 = (c - target).squaredNorm();
                if (d2 < best_d2 && is_free(c)) {
                    best    = c;
                    best_d2 = d2;
                }
            }
    }
    const Vec2d delta = best - bb.min.head<2>();
    pi->set_offset(pi->get_offset() + Vec3d(delta.x(), delta.y(), 0.));
    piece.invalidate_bounding_box();
}

// A cut in flight: what it was computed from (to apply it, and to detect edits made meanwhile) and
// what came out.
struct PathCutJob
{
    // input
    indexed_triangle_set     src;
    std::vector<PathCut::V3> pts;
    Vec3d                    view_dir;
    GLGizmoPathCut::Mode     mode;
    float                    kerf, margin;
    bool                     flat_cap{true};
    PathCut::ViewLimits      limits;
    PathCut::ConnectorParams conn;
    std::atomic<bool>        abort{false}; // set when the tool closes or the cut is cancelled
    std::exception_ptr       eptr;         // what the cut threw, handed to the UI thread
    size_t                   merged_parts{0};  // model parts joined into one mesh for the cut
    bool                     conn_manual{false};
    // Volumes that are not cut (negative volumes, modifiers, support blockers / enforcers): each goes to
    // the pieces it overlaps, both if the cut runs through it. A negative volume only removes material of
    // its own part, so keeping a whole hole in both pieces is exactly the hole cut in two.
    struct Extra { size_t volume_idx; indexed_triangle_set mesh; };
    std::vector<Extra>       extras;           // object coordinates
    std::vector<unsigned char> extra_sides;    // per extra: 1 = in side_a, 2 = in side_b (bits), from the cut
    indexed_triangle_set     removed;          // the negative volumes joined: no new connector runs into them
    indexed_triangle_set     earlier_faces;    // faces of the earlier cuts that left the object in one piece
    // the object
    int          obj_idx{-1}, inst_idx{-1};
    ModelObject *mo{nullptr};
    Vec3d        offset{Vec3d::Zero()};
    Transform3d  inst_matrix;
    struct VolumeState { size_t id; Transform3d matrix; std::shared_ptr<const TriangleMesh> mesh; };
    std::vector<VolumeState> volumes_before; // meshes held so their addresses cannot be reused
    // output
    bool                                      follow_creases{false}; // the loop runs along creases: wider tolerance
    PathCut::Grouped                          grouped;
    bool                                      joined{false}; // the sides still touch elsewhere
    std::vector<std::vector<PathCut::V3>>     routes;        // ... along these, object coordinates
    indexed_triangle_set                      cut_faces;     // ... and the faces of this cut, object coordinates
    PathCut::ConnectorResult                  connectors;    // object coordinates
    // Manual connectors: the cut is kept to place them (and to check them again when their settings change).
    std::unique_ptr<PathCut::CutResult>       cut;
    std::unique_ptr<PathCut::ConnectorPlacer> placer;        // refers to grouped and removed
    bool                                      connectors_from_a{false}; // pegs stick out of side_a
    std::string                               connector_error;
    std::string                               error;

    bool object_unchanged() const
    {
        const Model &model = wxGetApp().plater()->model();
        if (obj_idx < 0 || obj_idx >= int(model.objects.size()) || model.objects[obj_idx] != mo || inst_idx >= int(mo->instances.size()) ||
            !mo->instances[inst_idx]->get_matrix().isApprox(inst_matrix) || mo->volumes.size() != volumes_before.size())
            return false;
        for (size_t i = 0; i < volumes_before.size(); ++i)
            if (mo->volumes[i]->id().id != volumes_before[i].id || !mo->volumes[i]->get_matrix().isApprox(volumes_before[i].matrix) ||
                mo->volumes[i]->get_mesh_shared_ptr() != volumes_before[i].mesh)
                return false;
        return true;
    }
};

static void abort_job(PathCutJob &job) { job.abort = true; }
static bool job_object_unchanged(const PathCutJob &job) { return job.object_unchanged(); }
static bool job_joined(const PathCutJob &job) { return job.joined; }

// Which pieces each volume that is not cut belongs to: those with material where it is. Its surface is
// sampled (vertices, and a grid inside each triangle) and each sample tested inside each piece by
// counting the crossings of a vertical line above it. A hole is partly in the material it removes, a
// modifier partly in the part it changes; one in neither piece (e.g. only in the gap) is dropped.
// (Bounding boxes are not enough: the box of a piece easily overlaps a hole of the other one, which then
// came along with the wrong piece, floating away from it once the pieces were placed apart.)
static std::vector<unsigned char> extras_in_sides(const std::vector<PathCutJob::Extra> &extras, const indexed_triangle_set &side_a,
                                                  const indexed_triangle_set &side_b)
{
    const PathCut::Frame up = PathCut::frame_from_normal(PathCut::V3::UnitZ(), PathCut::V3::Zero());
    std::vector<std::unique_ptr<PathCut::detail::LineCaster>> casters;
    for (const indexed_triangle_set *side : {&side_a, &side_b})
        casters.push_back(side->indices.empty() ? nullptr : std::make_unique<PathCut::detail::LineCaster>(*side, up));
    std::vector<double> hits;
    auto inside = [&](const PathCut::detail::LineCaster &lc, const PathCut::V3 &p) {
        if (!lc.crossings(up.to2d(p), hits))
            return false;
        const double h     = up.height(p);
        size_t       above = 0;
        for (double x : hits)
            if (x > h) ++above;
        return (above & 1) == 1;
    };
    std::vector<unsigned char> out(extras.size(), 0);
    for (size_t k = 0; k < extras.size(); ++k) {
        const indexed_triangle_set &m = extras[k].mesh;
        std::vector<PathCut::V3>    samples;
        for (const auto &v : m.vertices) samples.emplace_back(v.cast<double>());
        constexpr int n = 4;
        for (const auto &t : m.indices) {
            const PathCut::V3 a = m.vertices[size_t(t[0])].cast<double>(), b = m.vertices[size_t(t[1])].cast<double>(),
                              c = m.vertices[size_t(t[2])].cast<double>();
            for (int u = 0; u <= n; ++u)
                for (int w = 0; u + w <= n; ++w)
                    samples.push_back(a + (b - a) * (double(u) / n) + (c - a) * (double(w) / n));
        }
        for (size_t s = 0; s < 2; ++s) {
            if (!casters[s])
                continue;
            for (const PathCut::V3 &p : samples)
                if (inside(*casters[s], p)) {
                    out[k] |= (unsigned char)(1u << s);
                    break;
                }
        }
    }
    return out;
}

void GLGizmoPathCut::clear_pending()
{
    m_pending.reset();
    m_routes.clear();
    m_routes_model.reset();
    m_placing = false;
    m_spots.clear();
    m_face_model.reset();
    m_conn_model.reset();
    clear_hover();
    m_orb_ring = m_orb_hover = -1;
    m_face_aabb.reset();
    m_piece_stay.reset();
    m_piece_moved.reset();
    m_preview_pegs.reset();
    show_object(true);
}

// While the preview is shown, the object itself is hidden so only the pieces are seen. (Shown again the
// way the tool shows it when it opens: only the selected instance of the selected object.)
void GLGizmoPathCut::show_object(bool show)
{
    if (show == !m_object_hidden)
        return;
    const Selection &selection = m_parent.get_selection();
    const int        obj_idx   = selection.get_object_idx();
    const int        inst_idx  = selection.get_instance_idx();
    const Model     &model     = wxGetApp().plater()->model();
    const ModelObject *mo = obj_idx >= 0 && obj_idx < int(model.objects.size()) ? model.objects[obj_idx] : nullptr;
    if (show) {
        // While the tool is open only the selected instance is shown (InstancesHider); once closed, all.
        if (m_state == On && mo != nullptr) {
            m_parent.toggle_model_objects_visibility(false);
            m_parent.toggle_model_objects_visibility(true, mo, inst_idx);
        } else
            m_parent.toggle_model_objects_visibility(true);
        m_object_hidden = false;
    } else if (mo != nullptr) {
        m_parent.toggle_model_objects_visibility(false, mo, inst_idx);
        m_object_hidden = true;
    }
    m_parent.set_as_dirty();
}

void GLGizmoPathCut::back_to_edit()
{
    clear_pending();
    m_message.clear();
    m_preview_dirty = true;
    m_parent.set_as_dirty();
}

bool GLGizmoPathCut::on_enter_key()
{
    const size_t min_pts = m_mode == Mode::Loop ? 3 : 2;
    if (m_busy || previewing() || m_placing || m_points.size() < min_pts)
        return false;
    wxGetApp().CallAfter([this]() { perform_cut(); });
    return true;
}

bool GLGizmoPathCut::on_escape_key()
{
    if (m_busy || !m_pending)
        return false; // Esc closes the tool, as before
    back_to_edit();   // the preview (or placing connectors) is left; the points and settings stay
    return true;
}

// The cut is computed: show the pieces apart, the smaller one (the piece that is detached when the cut
// is performed) moved away from the other along the line between their centres.
void GLGizmoPathCut::enter_preview(const std::shared_ptr<PathCutJob> &st)
{
    m_pending        = st;
    m_placing        = false;
    m_preview_offset = st->offset;
    rebuild_pieces_preview();
    show_object(false);
    m_parent.set_as_dirty();
}

void GLGizmoPathCut::rebuild_pieces_preview()
{
    m_piece_stay.reset();
    m_piece_moved.reset();
    m_preview_pegs.reset();
    m_piece_shift   = Vec3d::Zero();
    m_pegs_on_moved = false;
    if (!m_pending)
        return;
    const PathCutJob &st = *m_pending;
    // The detached piece is the smaller one, as when the cut is performed.
    const bool a_moves = std::abs(its_volume(st.grouped.side_a)) <= std::abs(its_volume(st.grouped.side_b));
    const indexed_triangle_set &moved = a_moves ? st.grouped.side_a : st.grouped.side_b;
    const indexed_triangle_set &stay  = a_moves ? st.grouped.side_b : st.grouped.side_a;
    if (!stay.indices.empty())
        m_piece_stay.init_from(stay);
    if (!moved.indices.empty())
        m_piece_moved.init_from(moved);
    if (!st.connectors.pegs.indices.empty()) {
        m_preview_pegs.init_from(st.connectors.pegs);
        m_pegs_on_moved = st.connectors_from_a == a_moves;
    }
    m_piece_shift = PathCut::preview_shift(moved, stay);
}

// The cut face shown and clicked while placing connectors. The picking tree refers to the face kept by
// the placer, so it is rebuilt whenever the placer is (a new placer frees the face the old tree read).
void GLGizmoPathCut::rebuild_face()
{
    m_face_model.reset();
    m_face_aabb.reset();
    if (!m_pending || !m_pending->placer || !m_pending->placer->error().empty())
        return;
    const indexed_triangle_set &face = m_pending->placer->face();
    if (face.indices.empty())
        return;
    GLModel::Geometry g;
    g.format = {GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3};
    for (const auto &v : face.vertices) g.add_vertex(Vec3f(v));
    for (const auto &t : face.indices) g.add_triangle(unsigned(t[0]), unsigned(t[1]), unsigned(t[2]));
    m_face_model.init_from(std::move(g));
    m_face_aabb = std::make_shared<AABBMesh>(face);
    // The direction orb: at the middle of the cut face, sized to it.
    BoundingBoxf3 bb;
    for (const auto &v : face.vertices) bb.merge(Vec3d(v.cast<double>()));
    m_orb_center = bb.center();
    m_orb_radius = std::clamp(0.3 * bb.size().norm(), 6., 30.);
    m_orb_axis   = Vec3d(m_pending->placer->axis());
    m_orb_ring   = m_orb_hover = -1;
    rebuild_orb();
}

// The cut separated the object and the connectors go where the user clicks: show the cut face.
void GLGizmoPathCut::start_placing(const std::shared_ptr<PathCutJob> &st)
{
    m_pending        = st;
    m_placing        = true;
    m_spots.clear();
    m_placing_offset = st->offset;
    rebuild_face();
    rebuild_connector_preview();
    m_message          = _u8L("The cut is ready. Click on the cut face (orange) where each connector should go, then apply the cut.");
    m_message_is_error = false;
    m_parent.set_as_dirty();
}

// The cut face is an open surface, seen from either side and through the object (it is drawn on top):
// the first hit along the mouse ray counts. (MeshRaycaster::unproject_on_mesh is for closed meshes: it
// takes an odd number of hits for a ray starting inside, so it never picks a single surface.)
bool GLGizmoPathCut::raycast_face(const Vec2d &mouse_pos, Vec3d &hit_world) const
{
    if (!m_face_aabb)
        return false;
    Vec3d point, dir;
    CameraUtils::ray_from_screen_pos(wxGetApp().plater()->get_camera(), mouse_pos, point, dir);
    const std::vector<AABBMesh::hit_result> hits = m_face_aabb->query_ray_hits(point - m_placing_offset, dir);
    for (const AABBMesh::hit_result &h : hits)
        if (h.is_hit()) {
            hit_world = h.position() + m_placing_offset;
            return true;
        }
    return false;
}

void GLGizmoPathCut::add_connector(const Vec3d &hit_world)
{
    if (!m_pending || !m_pending->placer || !m_pending->placer->error().empty())
        return;
    PathCut::ConnectorSpot   spot;
    const PathCut::V3        p   = PathCut::V3(hit_world - m_placing_offset);
    const PathCut::ConnectorFit why = m_pending->placer->try_at(p, spot, m_spots);
    if (force_connector(why))
        spot = m_pending->placer->forced_at(p);
    if (why == PathCut::ConnectorFit::Ok || force_connector(why)) {
        m_spots.push_back(spot);
        rebuild_connector_preview();
        m_message.clear();
    } else {
        m_message          = _u8L(PathCut::connector_fit_message(why));
        m_message_is_error = true;
    }
    m_parent.set_as_dirty();
}

// The connector settings changed while placing: check the placed ones again with the new settings and
// keep those that still fit. Switching connectors off drops the pending cut.
void GLGizmoPathCut::connectors_changed()
{
    if (!m_pending || !m_pending->cut || m_conn.type == PathCut::ConnectorType::None) {
        clear_pending();
        m_preview_dirty = true;
        return;
    }
    PathCutJob &st = *m_pending;
    st.conn        = m_conn;
    const indexed_triangle_set &from = st.connectors_from_a ? st.grouped.side_a : st.grouped.side_b;
    const indexed_triangle_set &to   = st.connectors_from_a ? st.grouped.side_b : st.grouped.side_a;
    m_face_aabb.reset(); // it reads the face of the placer replaced here
    st.placer = std::make_unique<PathCut::ConnectorPlacer>(*st.cut, st.connectors_from_a ? 1 : -1, from, to, st.conn,
                                                           st.removed.indices.empty() ? nullptr : &st.removed);
    rebuild_face();
    if (!st.placer->error().empty()) {
        m_spots.clear();
        m_message          = _u8L(st.placer->error().c_str());
        m_message_is_error = true;
    } else {
        std::vector<PathCut::ConnectorSpot> kept;
        for (const PathCut::ConnectorSpot &old : m_spots) {
            PathCut::ConnectorSpot spot;
            const PathCut::ConnectorFit why = st.placer->try_at(old.p, spot, kept);
            if (why == PathCut::ConnectorFit::Ok)
                kept.push_back(spot);
            else if (old.forced && force_connector(why))
                kept.push_back(st.placer->forced_at(old.p));
        }
        if (kept.size() < m_spots.size()) {
            const size_t removed = m_spots.size() - kept.size();
            m_message = GUI::format(_L_PLURAL("%1% placed connector no longer fits with these settings and was removed.",
                                              "%1% placed connectors no longer fit with these settings and were removed.", removed),
                                    removed);
            m_message_is_error = true;
        }
        m_spots = std::move(kept);
    }
    rebuild_connector_preview();
    if (m_hover_on)
        update_hover(m_hover_pos); // shown again with the new settings
    m_parent.set_as_dirty();
}

void GLGizmoPathCut::rebuild_connector_preview()
{
    m_conn_model.reset();
    if (!m_pending || !m_pending->placer || !m_pending->placer->error().empty() || m_spots.empty())
        return;
    init_connector_model(m_conn_model, m_pending->placer->build(m_spots));
}

void GLGizmoPathCut::update_hover(const Vec2d &mouse_pos)
{
    m_hover_pos = mouse_pos;
    Vec3d hit;
    if (!m_placing || m_busy || !m_pending || !m_pending->placer || !m_pending->placer->error().empty() ||
        !raycast_face(mouse_pos, hit)) {
        clear_hover();
        return;
    }
    const PathCut::ConnectorPlacer &placer = *m_pending->placer;
    const PathCut::V3               p      = PathCut::V3(hit - m_placing_offset);
    PathCut::ConnectorSpot          spot;
    m_hover_fit = placer.try_at(p, spot, m_spots);
    if (m_hover_fit != PathCut::ConnectorFit::Ok)
        spot = placer.forced_at(p); // shown (and placed, if forced) as it would go there
    init_connector_model(m_hover_model, placer.build({spot}));
    m_hover_on = m_hover_model.is_initialized();
    m_parent.set_as_dirty();
}

void GLGizmoPathCut::clear_hover()
{
    if (!m_hover_on && !m_hover_model.is_initialized())
        return;
    m_hover_on = false;
    m_hover_model.reset();
    m_parent.set_as_dirty();
}

// Manual connectors placed even where they do not fit, when the user asks for it (not off the cut face).
bool GLGizmoPathCut::force_connector(PathCut::ConnectorFit why) const
{
    return m_conn_force && why != PathCut::ConnectorFit::Ok && why != PathCut::ConnectorFit::OffFace;
}

// ---------------------------------------------------------------------------------------------------------
// Direction orb, while placing connectors: an arrow along the direction all the connectors share, and two
// rings around it to turn it, as the rotation rings of the plane cut tool. Dragging a ring turns the arrow
// about the ring's axis (1 degree steps); on release the connectors take the new direction (as a tilt and
// a turn, the same settings as in the panel).

// The rings' axes for an arrow along `axis`: perpendicular to it, the first one level with the bed if it can.
static void orb_frame(const Vec3d &axis, Vec3d &e1, Vec3d &e2)
{
    e1 = Vec3d::UnitZ().cross(axis);
    if (e1.norm() < 1e-6) e1 = Vec3d::UnitX().cross(axis);
    e1.normalize();
    e2 = axis.cross(e1).normalized();
}

std::vector<Vec3d> GLGizmoPathCut::orb_ring(int ring) const
{
    const Vec3d axis = m_orb_ring >= 0 ? m_orb_start_axis : m_orb_axis; // the rings stay put while dragging
    Vec3d       e1, e2;
    orb_frame(axis, e1, e2);
    const Vec3d n = ring == 0 ? e1 : e2, a = axis, b = n.cross(a);
    const Vec3d c = m_orb_center + m_placing_offset;
    std::vector<Vec3d> pts;
    for (int k = 0; k <= 64; ++k) {
        const double t = 2. * PI * k / 64.;
        pts.push_back(c + (a * std::cos(t) + b * std::sin(t)) * m_orb_radius);
    }
    return pts;
}

// The ring under the mouse (within 7 px of it on screen), or -1.
int GLGizmoPathCut::orb_pick(const Vec2d &mouse_pos) const
{
    const Camera &camera = wxGetApp().plater()->get_camera();
    int           best   = -1;
    double        bd     = 7. * m_parent.get_scale();
    for (int ring = 0; ring < 2; ++ring) {
        const Points pts = CameraUtils::project(camera, orb_ring(ring));
        for (size_t k = 0; k + 1 < pts.size(); ++k) {
            const Vec2d  a = pts[k].cast<double>(), b = pts[k + 1].cast<double>(), ab = b - a;
            const double t = ab.squaredNorm() > 0. ? std::clamp((mouse_pos - a).dot(ab) / ab.squaredNorm(), 0., 1.) : 0.;
            const double d = (a + ab * t - mouse_pos).norm();
            if (d < bd) { bd = d; best = ring; }
        }
    }
    return best;
}

// Where the mouse ray meets the plane of a ring, as a direction from its centre; false when it misses.
bool GLGizmoPathCut::orb_plane_dir(const Vec2d &mouse_pos, int ring, Vec3d &dir) const
{
    Vec3d e1, e2;
    orb_frame(m_orb_start_axis, e1, e2);
    const Vec3d n = ring == 0 ? e1 : e2, c = m_orb_center + m_placing_offset;
    Vec3d       point, ray;
    CameraUtils::ray_from_screen_pos(wxGetApp().plater()->get_camera(), mouse_pos, point, ray);
    const double den = n.dot(ray);
    if (std::abs(den) < 1e-9)
        return false;
    const Vec3d p = point + ray * (n.dot(c - point) / den);
    dir           = p - c - n * n.dot(p - c);
    if (dir.norm() < 1e-9)
        return false;
    dir.normalize();
    return true;
}

bool GLGizmoPathCut::on_orb_mouse(const wxMouseEvent &mouse_event, const Vec2d &mouse_pos)
{
    if (!m_pending || !m_pending->placer || !m_pending->placer->error().empty() || m_conn.type == PathCut::ConnectorType::None)
        return false;
    if (m_orb_ring >= 0) {
        if (mouse_event.Dragging() && mouse_event.LeftIsDown()) {
            Vec3d d;
            if (orb_plane_dir(mouse_pos, m_orb_ring, d)) {
                Vec3d e1, e2;
                orb_frame(m_orb_start_axis, e1, e2);
                const Vec3d  n     = m_orb_ring == 0 ? e1 : e2;
                double       angle = std::atan2(n.dot(m_orb_start_dir.cross(d)), m_orb_start_dir.dot(d));
                const double step  = PI / 180.;
                angle              = step * std::round(angle / step);
                m_orb_axis         = (Eigen::AngleAxisd(angle, n) * m_orb_start_axis).normalized();
                // Not past 80 degrees from the direction the pieces come apart in (they would not come apart).
                const Vec3d pull = Vec3d(m_pending->placer->pull_axis());
                double      tilt = 0., turn = 0.;
                PathCut::tilt_of(pull, m_orb_axis, tilt, turn);
                if (tilt > 80.)
                    m_orb_axis = Vec3d(PathCut::tilted_axis(pull, 80., turn));
                rebuild_orb();
                m_parent.set_as_dirty();
            }
            return true;
        }
        if (mouse_event.LeftUp()) {
            m_orb_ring = -1;
            double tilt = 0., turn = 0.;
            PathCut::tilt_of(m_pending->placer->pull_axis(), m_orb_axis, tilt, turn);
            m_conn.tilt = std::round(tilt);
            m_conn.turn = std::round(turn);
            connectors_changed(); // the connectors take the new direction (and the orb follows the placer)
            m_parent.set_as_dirty();
            return true;
        }
        return true;
    }
    if (mouse_event.Moving()) {
        const int hover = orb_pick(mouse_pos);
        if (hover != m_orb_hover) {
            m_orb_hover = hover;
            m_parent.set_as_dirty();
        }
        return false;
    }
    if (mouse_event.LeftDown() && !mouse_event.CmdDown() && m_orb_hover >= 0 && orb_pick(mouse_pos) == m_orb_hover) {
        m_orb_start_axis = m_orb_axis;
        if (!orb_plane_dir(mouse_pos, m_orb_hover, m_orb_start_dir))
            return false;
        m_orb_ring = m_orb_hover;
        clear_hover();
        if (!m_parent.has_mouse_capture())
            m_parent.get_wxglcanvas()->CaptureMouse(); // released by the canvas on button up
        return true;
    }
    return false;
}

void GLGizmoPathCut::rebuild_orb()
{
    for (GLModel &m : m_orb_models) m.reset();
    for (int ring = 0; ring < 2; ++ring) {
        const std::vector<Vec3d> pts = orb_ring(ring);
        GLModel::Geometry        g;
        g.format = {GLModel::Geometry::EPrimitiveType::LineStrip, GLModel::Geometry::EVertexLayout::P3};
        for (size_t k = 0; k < pts.size(); ++k) {
            g.add_vertex(Vec3f(pts[k].cast<float>()));
            g.add_index(unsigned(k));
        }
        m_orb_models[ring].init_from(std::move(g));
    }
    // The arrow: a line from the centre along the direction, through the cut face both ways.
    const Vec3d       c = m_orb_center + m_placing_offset;
    GLModel::Geometry g;
    g.format = {GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3};
    g.add_vertex(Vec3f((c - m_orb_axis * (0.5 * m_orb_radius)).cast<float>()));
    g.add_vertex(Vec3f((c + m_orb_axis * (1.3 * m_orb_radius)).cast<float>()));
    g.add_line(0, 1);
    // arrow head
    Vec3d e1, e2;
    orb_frame(m_orb_axis, e1, e2);
    const Vec3d tip = c + m_orb_axis * (1.3 * m_orb_radius), back = tip - m_orb_axis * (0.15 * m_orb_radius);
    const Vec3d sides[4] = {e1, -e1, e2, -e2};
    for (const Vec3d &s : sides) {
        const unsigned base = unsigned(g.vertices_count());
        g.add_vertex(Vec3f(tip.cast<float>()));
        g.add_vertex(Vec3f((back + s * (0.08 * m_orb_radius)).cast<float>()));
        g.add_line(base, base + 1);
    }
    m_orb_models[2].init_from(std::move(g));
}

void GLGizmoPathCut::render_orb()
{
    if (!m_pending || !m_pending->placer || !m_pending->placer->error().empty() || m_conn.type == PathCut::ConnectorType::None)
        return;
    GLShaderProgram *shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;
    const Camera &camera = wxGetApp().plater()->get_camera();
    shader->start_using();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix());
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    for (int ring = 0; ring < 2; ++ring)
        if (m_orb_models[ring].is_initialized()) {
            const bool active = m_orb_ring == ring || (m_orb_ring < 0 && m_orb_hover == ring);
            shader->set_uniform("width", active ? 3.f : 1.5f);
            m_orb_models[ring].set_color(active ? ORB_HOVER_COLOR : ORB_RING_COLOR[ring]);
            m_orb_models[ring].render();
        }
    if (m_orb_models[2].is_initialized()) {
        shader->set_uniform("width", 2.f);
        m_orb_models[2].set_color(ORB_AXIS_COLOR);
        m_orb_models[2].render();
    }
    shader->stop_using();
}

std::string GLGizmoPathCut::orb_tooltip() const
{
    if (!m_pending || !m_pending->placer)
        return "";
    if (m_orb_ring >= 0) {
        double tilt = 0., turn = 0.;
        PathCut::tilt_of(m_pending->placer->pull_axis(), m_orb_axis, tilt, turn);
        return GUI::format(_u8L("Connector tilt %1% deg, towards %2% deg"), int(std::round(tilt)), int(std::round(turn)));
    }
    return m_orb_hover >= 0 ? _u8L("Drag to turn the direction of the connectors") : "";
}

// The faces of earlier cuts belong to one instance: shown again when the tool opens on it, moved along
// with it; dropped for any other instance, or when it was rotated or scaled meanwhile.
void GLGizmoPathCut::sync_done()
{
    if (!m_done_faces.indices.empty() || !m_done_routes.empty()) {
        const Selection     &selection = m_parent.get_selection();
        const int            obj_idx   = selection.get_object_idx();
        const int            inst_idx  = selection.get_instance_idx();
        const Model         &model     = wxGetApp().plater()->model();
        const ModelObject   *mo = obj_idx >= 0 && obj_idx < int(model.objects.size()) ? model.objects[obj_idx] : nullptr;
        const ModelInstance *mi = mo != nullptr && inst_idx >= 0 && inst_idx < int(mo->instances.size()) ? mo->instances[inst_idx] : nullptr;
        const Transform3d    now  = mi != nullptr ? mi->get_matrix() : Transform3d::Identity();
        if (mi != nullptr && mi->id() == m_done_object && now.linear().isApprox(m_done_matrix.linear(), 1e-9)) {
            const Vec3d delta = now.translation() - m_done_matrix.translation();
            for (auto &v : m_done_faces.vertices) v += delta.cast<float>();
            for (auto &route : m_done_routes)
                for (Vec3d &p : route) p += delta;
            m_done_matrix = now;
        } else {
            m_done_faces.clear();
            m_done_routes.clear();
            m_done_object = ObjectID();
        }
    }
    rebuild_done();
}

void GLGizmoPathCut::rebuild_done()
{
    m_done_faces_model.reset();
    m_done_routes_model.reset();
    if (!m_done_faces.indices.empty()) {
        GLModel::Geometry g;
        g.format = {GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3};
        for (const auto &v : m_done_faces.vertices) g.add_vertex(Vec3f(v));
        for (const auto &f : m_done_faces.indices) g.add_triangle(unsigned(f[0]), unsigned(f[1]), unsigned(f[2]));
        m_done_faces_model.init_from(std::move(g));
    }
    GLModel::Geometry g;
    g.format = {GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3};
    for (const auto &route : m_done_routes)
        for (size_t i = 0; i + 1 < route.size(); ++i) {
            const unsigned base = unsigned(g.vertices_count());
            g.add_vertex(Vec3f(route[i].cast<float>()));
            g.add_vertex(Vec3f(route[i + 1].cast<float>()));
            g.add_line(base, base + 1);
        }
    if (g.vertices_count() > 0)
        m_done_routes_model.init_from(std::move(g));
}

void GLGizmoPathCut::render_done()
{
    if (!m_done_faces_model.is_initialized() && !m_done_routes_model.is_initialized())
        return;
    GLShaderProgram *shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;
    const Camera &camera = wxGetApp().plater()->get_camera();
    glsafe(::glDisable(GL_DEPTH_TEST));
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    shader->start_using();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix());
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    if (m_done_faces_model.is_initialized()) {
        m_done_faces_model.set_color(DONE_CUT_COLOR);
        m_done_faces_model.render();
    }
    if (m_done_routes_model.is_initialized()) {
        shader->set_uniform("width", 0.6f);
        m_done_routes_model.set_color(ColorRGBA(1.f, 0.15f, 0.15f, 1.f));
        m_done_routes_model.render();
    }
    shader->stop_using();
    glsafe(::glDisable(GL_BLEND));
    glsafe(::glEnable(GL_CULL_FACE));
    glsafe(::glEnable(GL_DEPTH_TEST));
}

void GLGizmoPathCut::perform_cut()
{
    if (m_busy)
        return;
    clear_pending();
    const Selection &selection = m_parent.get_selection();
    const int        obj_idx   = selection.get_object_idx();
    const int        inst_idx  = selection.get_instance_idx();
    Plater          *plater    = wxGetApp().plater();
    auto refuse = [this](const std::string &why) {
        m_message          = why;
        m_message_is_error = true;
        m_parent.set_as_dirty();
    };
    // Never silently: a preview button that does nothing looks broken.
    if (obj_idx < 0 || inst_idx < 0 || obj_idx >= int(plater->model().objects.size())) {
        refuse(_u8L("No object is selected. Select the object to cut and make the preview again."));
        return;
    }
    ModelObject *mo = plater->model().objects[obj_idx];
    if (mo != m_object) {
        reset_path();
        refuse(_u8L("Another object is selected. Place the points again on the object to cut."));
        return;
    }
    // The cut replaces the object by its pieces. Its model parts are joined into one mesh and cut together
    // (their own settings are lost, the result takes those of the first part). The other volumes (negative
    // volumes such as the holes of an earlier cut, modifiers, support blockers/enforcers) are not cut: each
    // goes to the pieces it overlaps. Refused when other copies of the object would be removed.
    if (mo->instances.size() > 1) {
        refuse(_u8L("This object has several copies. Cutting it would remove the other copies: make them separate "
                    "objects first, then cut one of them."));
        return;
    }
    size_t parts = 0;
    for (const ModelVolume *v : mo->volumes)
        if (v->is_model_part())
            ++parts;
    const ModelInstance *mi = mo->instances[inst_idx];
    if (!mi->get_matrix().isApprox(m_inst_matrix)) {
        reset_path();
        refuse(_u8L("The object was moved since the points were placed. Place them again."));
        return;
    }

    // Everything the background job needs is copied here; the job never touches the Model.
    auto st      = std::make_shared<PathCutJob>();
    st->obj_idx  = obj_idx;
    st->inst_idx = inst_idx;
    st->mo       = mo;
    st->offset   = mi->get_offset();
    st->inst_matrix = mi->get_matrix();
    for (const ModelVolume *v : mo->volumes)
        st->volumes_before.push_back({v->id().id, v->get_matrix(), v->get_mesh_shared_ptr()});

    // Work in millimetres as printed: bake instance rotation/scale into the mesh, keep the offset.
    const Transform3d inst_no_offset = mi->get_transformation().get_matrix_no_offset();
    for (size_t i = 0; i < mo->volumes.size(); ++i) {
        const ModelVolume   *v   = mo->volumes[i];
        indexed_triangle_set its = v->mesh().its;
        its_transform(its, inst_no_offset * v->get_matrix(), true);
        if (v->is_model_part())
            its_merge(st->src, its);
        else {
            if (v->is_negative_volume())
                its_merge(st->removed, its);
            st->extras.push_back({i, std::move(its)});
        }
    }
    if (st->src.indices.empty())
        return;
    if (m_preview_dirty)
        rebuild_preview(); // the loop along the creases for the current points
    for (const Vec3d &p : cut_points())
        st->pts.emplace_back(p - st->offset);
    st->follow_creases = m_mode == Mode::Loop && !m_loop_pts.empty();
    st->view_dir = sweep_dir();
    st->mode     = m_mode;
    st->kerf     = m_kerf;
    st->margin   = m_margin;
    st->flat_cap = m_flat_cap;
    st->limits   = view_limits();
    st->conn     = m_conn;
    st->merged_parts   = parts;
    st->conn_manual    = m_conn_manual;
    st->earlier_faces  = m_done_faces; // shown in world coordinates, the job works relative to the offset
    for (auto &v : st->earlier_faces.vertices) v -= st->offset.cast<float>();

    m_busy             = true;
    m_message          = _u8L("Cutting, please wait...");
    m_message_is_error = false;
    m_parent.set_as_dirty();

    // Runs on the UI job worker thread, so the UI stays responsive and the job can be cancelled.
    auto process = [st]() {
        using clk = std::chrono::steady_clock;
        auto t0   = clk::now();
        auto lap  = [&t0](const char *what) {
            const auto now = clk::now();
            BOOST_LOG_TRIVIAL(info) << "PathCut: " << what << " "
                                    << std::chrono::duration_cast<std::chrono::milliseconds>(now - t0).count() << " ms";
            t0 = now;
        };
        auto canceled = [&st]() { return st->abort.load(); };
        BOOST_LOG_TRIVIAL(info) << "PathCut: " << st->src.indices.size() << " triangles";
        {
            // The settings and the points of the cut, so that a cut reported with its log can be repeated
            // on the object exported as STL (points relative to the instance offset).
            std::ostringstream os;
            os << std::setprecision(9) << "PathCut: " << (st->mode == GLGizmoPathCut::Mode::ViewPath ? "view" : "loop") << " kerf "
               << st->kerf << " margin " << st->margin << " flat " << st->flat_cap << " edges " << st->follow_creases << " view "
               << st->view_dir.x() << ',' << st->view_dir.y() << ',' << st->view_dir.z() << " limits " << st->limits.start << ','
               << st->limits.end << ',' << st->limits.front << ',' << st->limits.back << " offset " << st->offset.x() << ','
               << st->offset.y() << ',' << st->offset.z() << " points";
            for (const PathCut::V3 &p : st->pts) os << ' ' << p.x() << ',' << p.y() << ',' << p.z();
            BOOST_LOG_TRIVIAL(info) << os.str();
        }
        PathCut::make_edge_manifold(st->src);
        lap("make_edge_manifold");
        PathCut::V3 mn = PathCut::V3::Constant(std::numeric_limits<double>::max()), mx = -mn;
        for (const auto &v : st->src.vertices) {
            mn = mn.cwiseMin(PathCut::V3(v.cast<double>()));
            mx = mx.cwiseMax(PathCut::V3(v.cast<double>()));
        }
        PathCut::CutResult                      res;
        std::function<int(const PathCut::V3 &)> side;
        PathCut::ViewCutter                     vc;
        PathCut::LoopCutter                     lc;
        if (st->mode == GLGizmoPathCut::Mode::ViewPath) {
            vc = PathCut::make_view_cutter(st->pts, st->view_dir, mn, mx, st->kerf, st->limits);
            if (!vc.valid()) {
                st->error = vc.error;
                return;
            }
            res  = PathCut::cut_with_slab(std::move(st->src), vc, vc.limited(), 0.5, canceled);
            side = [&vc](const PathCut::V3 &p) { return vc.side(p); };
        } else {
            bool cut_done = false;
            if (st->follow_creases && !st->flat_cap) {
                // Along the model's edges: cut exactly along them, nothing smoothed (the membrane cut face
                // takes any outline). Where that cannot be done, the loop cut below follows them closely.
                const PathCut::CreaseGraph graph(st->src);
                const std::vector<int>     chain = graph.loop_vertices(st->pts);
                lap("edges followed");
                if (!chain.empty()) {
                    res      = PathCut::cut_along_edge_loop(st->src, chain, st->kerf, canceled);
                    cut_done = res.error.empty() || canceled();
                    if (cut_done)
                        side = [](const PathCut::V3 &) { return 0; };
                    else
                        BOOST_LOG_TRIVIAL(info) << "PathCut: cut along the edges failed (" << res.error << "), cutting along the loop";
                }
            }
            if (!cut_done) {
                AABBMesh aabb(st->src);
                auto     snap = [&aabb](const PathCut::V3 &p) -> PathCut::V3 {
                    int   idx;
                    Vec3d c;
                    aabb.squared_distance(Vec3d(p), idx, c);
                    return c;
                };
                lc = PathCut::make_loop_cutter(st->pts, st->kerf, st->margin, snap, st->flat_cap);
                if (!lc.valid()) {
                    st->error = lc.error;
                    return;
                }
                if (!st->flat_cap) {
                    // Exactly along the loop: only the surface the loop runs on is cut (folds, creases).
                    // Along a crease the surface folds sharply under the loop: let the cut line find it a little further.
                    res = PathCut::cut_along_loop(std::move(st->src), lc, st->follow_creases ? std::max<double>(st->margin, 3.) : st->margin,
                                                  canceled);
                } else {
                    // Flat cut face: a slab through the mean plane of the loop, widened past bulges near it.
                    if (!PathCut::fit_loop_margin(st->src, lc)) {
                        st->error = L("The cap spanning the loop runs into another part of the model. Place the loop where the part is free all around.");
                        return;
                    }
                    BOOST_LOG_TRIVIAL(info) << "PathCut: cap margin " << lc.margin << " mm";
                    res = PathCut::cut_with_slab(std::move(st->src), lc, true, 0.5, canceled);
                }
                side = [&lc](const PathCut::V3 &p) { return lc.side(p); };
            }
        }
        lap("cut");
        if (canceled())
            return;
        if (!res.error.empty()) {
            st->error = res.error;
            return;
        }
        std::vector<signed char> vertex_side;
        st->grouped = PathCut::group_cut(res, side, st->mode == GLGizmoPathCut::Mode::Loop, &st->joined,
                                         st->earlier_faces.indices.empty() ? nullptr : &vertex_side);
        lap("grouping");
        if (!st->joined && !st->earlier_faces.indices.empty()) {
            // The pieces come apart along the faces of all the cuts: connectors can go on any of them.
            PathCut::add_earlier_faces(res, st->earlier_faces, vertex_side);
            BOOST_LOG_TRIVIAL(info) << "PathCut: " << res.earlier_faces.size() << " triangles of earlier cut faces";
        }
        if (!st->extras.empty()) {
            st->extra_sides = extras_in_sides(st->extras, st->grouped.side_a, st->grouped.side_b);
            lap("volumes kept");
        }
        if (st->joined) {
            st->routes = PathCut::joined_routes(res, 3, -1., canceled);
            for (size_t ti = res.first_cap; ti < res.mesh.indices.size(); ++ti) {
                const int base = int(st->cut_faces.vertices.size());
                for (int k = 0; k < 3; ++k) st->cut_faces.vertices.push_back(res.mesh.vertices[res.mesh.indices[ti][k]]);
                st->cut_faces.indices.emplace_back(base, base + 1, base + 2);
            }
            lap("connection routes");
        } else if (st->conn.type != PathCut::ConnectorType::None && !st->grouped.side_a.indices.empty() &&
                   !st->grouped.side_b.indices.empty()) {
            st->connectors_from_a = PathCut::peg_side(res, st->grouped.side_a, st->grouped.side_b) == 1;
            const indexed_triangle_set &from = st->connectors_from_a ? st->grouped.side_a : st->grouped.side_b;
            const indexed_triangle_set &to   = st->connectors_from_a ? st->grouped.side_b : st->grouped.side_a;
            st->placer = std::make_unique<PathCut::ConnectorPlacer>(res, st->connectors_from_a ? 1 : -1, from, to, st->conn,
                                                                    st->removed.indices.empty() ? nullptr : &st->removed);
            if (!st->placer->error().empty()) {
                st->connector_error = st->placer->error();
                st->placer.reset();
            } else if (!st->conn_manual) {
                const std::vector<PathCut::ConnectorSpot> spots = st->placer->auto_place(st->conn.count, st->connector_error, canceled);
                st->connectors = st->placer->build(spots);
                st->placer.reset();
            } else
                st->cut = std::make_unique<PathCut::CutResult>(std::move(res)); // to check the connectors again on edits
            lap("connectors");
        }
        };

    // Back on the UI thread.
    const unsigned generation = m_generation;
    auto finalize = [this, st, generation](bool canceled, std::exception_ptr &eptr) {
        if (generation != m_generation || m_state != On) {
            eptr = nullptr; // the tool was closed meanwhile: drop the result, whatever it is
            return;
        }
        m_busy = false;
        m_running.reset();
        std::string error = st->error;
        if (eptr) {
            try {
                std::rethrow_exception(eptr);
            } catch (const std::exception &e) {
                BOOST_LOG_TRIVIAL(error) << "PathCut: " << e.what();
                error = L("The cut failed. Try a simpler path or a lower resolution model.");
            } catch (...) {
                BOOST_LOG_TRIVIAL(error) << "PathCut: unknown exception";
                error = L("The cut failed. Try a simpler path or a lower resolution model.");
            }
            eptr = nullptr; // handled here, don't let the worker show a generic error
        }
        if (canceled) {
            m_message.clear();
            m_parent.set_as_dirty();
            return;
        }
        if (error.empty() && !st->joined && (st->grouped.side_a.indices.empty() || st->grouped.side_b.indices.empty()))
            error = st->mode == Mode::ViewPath ?
                        L("The path did not split the object. Make sure it crosses the object completely.") :
                        L("The loop did not detach anything. Make sure it goes all the way around the part.");
        if (!error.empty()) {
            m_message          = _u8L(error.c_str());
            m_message_is_error = true;
            m_parent.set_as_dirty();
            return;
        }
        if (st->joined) {
            if (!st->object_unchanged()) {
                m_message          = _u8L("The object was changed while it was being cut. Cut it again.");
                m_message_is_error = true;
                reset_path();
                m_parent.set_as_dirty();
                return;
            }
            // Show where it is still connected and let the user decide: a preview with a warning.
            enter_preview(st);
            for (const auto &route : st->routes) {
                m_routes.emplace_back();
                for (const PathCut::V3 &p : route) m_routes.back().push_back(p + st->offset);
            }
            m_message          = _u8L("The cut does not separate the object: it is still connected along the red lines. Edit the cut, "
                                      "or perform it now and cut the connection next.");
            m_message_is_error = true;
            m_preview_dirty    = true;
            m_parent.set_as_dirty();
            return;
        }
        if (st->placer) {
            if (!st->object_unchanged()) {
                m_message          = _u8L("The object was changed while it was being cut. Cut it again.");
                m_message_is_error = true;
                reset_path();
                m_parent.set_as_dirty();
                return;
            }
            start_placing(st);
            return;
        }
        if (!st->object_unchanged()) {
            m_message          = _u8L("The object was changed while it was being cut. Cut it again.");
            m_message_is_error = true;
            reset_path();
            m_parent.set_as_dirty();
            return;
        }
        // Nothing changes yet: the pieces are shown and the user performs the cut or edits it.
        enter_preview(st);
        m_message          = _u8L("Preview: the pieces are shown apart. Perform the cut if it is right, or edit it.");
        m_message_is_error = false;
    };

    // The cut runs on a thread of its own, not on the shared UI job worker: a cut stuck in a phase that
    // does not check for cancelling would otherwise keep the worker (and its progress bar) busy until the
    // application is closed. Cancelling frees the tool at once; a cut still running is just left to end
    // and its result dropped (the generation no longer matches).
    m_running = st;
    std::thread([st, process, finalize]() {
        try {
            process();
        } catch (...) {
            st->eptr = std::current_exception();
        }
        if (wxTheApp)
            wxTheApp->CallAfter([st, finalize]() {
                std::exception_ptr e = st->eptr;
                finalize(st->abort.load(), e);
            });
    }).detach();
}

// An exception escaping here (UI thread, job finalize or CallAfter) would end the application.
void GLGizmoPathCut::apply_cut(std::shared_ptr<PathCutJob> st)
{
    try {
        do_apply_cut(st);
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "PathCut: applying the cut failed: " << e.what();
        cut_failed();
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) << "PathCut: applying the cut failed";
        cut_failed();
    }
}

// The tool may already be closed at that point (applying closes it), so also as a notification.
void GLGizmoPathCut::cut_failed()
{
    m_message          = _u8L("The cut could not be applied.");
    m_message_is_error = true;
    wxGetApp().plater()->get_notification_manager()->push_notification(
        NotificationType::CustomNotification, NotificationManager::NotificationLevel::ErrorNotificationLevel, m_message);
    m_parent.set_as_dirty();
}

void GLGizmoPathCut::do_apply_cut(const std::shared_ptr<PathCutJob> &st)
{
    const std::vector<PathCut::ConnectorSpot> spots = m_placing ? m_spots : std::vector<PathCut::ConnectorSpot>{};
    clear_pending();
    if (!st->object_unchanged()) {
        m_message          = _u8L("The object was changed while it was being cut. Cut it again.");
        m_message_is_error = true;
        reset_path();
        m_parent.set_as_dirty();
        return;
    }
    Plater            *plater     = wxGetApp().plater();
    ModelObject       *mo         = st->mo;
    const ModelVolume *first_part = nullptr;
    for (const ModelVolume *v : mo->volumes)
        if (v->is_model_part()) {
            first_part = v;
            break;
        }

    // Detached piece (smaller) first. If the sides still touch elsewhere, everything is one object
    // with the gap, so the remaining connection can be cut next.
    PathCut::Grouped                   &grouped = st->grouped;
    std::vector<indexed_triangle_set *> sides{&grouped.side_a, &grouped.side_b};
    if (std::abs(its_volume(grouped.side_a)) > std::abs(its_volume(grouped.side_b)))
        std::swap(sides[0], sides[1]);
    if (sides[0]->indices.empty())
        sides.erase(sides.begin());

    if (st->placer) { // manual connectors: the ones the user placed (possibly none)
        st->connectors = st->placer->build(spots);
        st->placer.reset();
    }
    const PathCut::ConnectorResult &conn       = st->connectors;
    const bool                      connectors = sides.size() == 2 && !conn.positions.empty();
    const indexed_triangle_set     *from_side  = st->connectors_from_a ? &grouped.side_a : &grouped.side_b;

    Model                          tmp;
    // TRN %1%: the name of the object that was cut
    const std::vector<std::string> names = sides.size() == 2 ?
        std::vector<std::string>{GUI::format(_u8L("%1% - piece"), mo->name), GUI::format(_u8L("%1% - rest"), mo->name)} :
        std::vector<std::string>{GUI::format(_u8L("%1% - cut"), mo->name)};
    auto add_part = [&](ModelObject *no, const std::string &name, indexed_triangle_set its, ModelVolumeType type) {
        if (its.indices.empty())
            return;
        ModelVolume *nv = no->add_volume(TriangleMesh(std::move(its)), type);
        nv->name        = name;
        if (first_part && type == ModelVolumeType::MODEL_PART) {
            nv->config.apply(first_part->config);
            nv->set_material_id(first_part->material_id());
        }
    };
    auto new_object = [&](const std::string &name, indexed_triangle_set its) {
        ModelObject *no = tmp.add_object(*mo);
        no->clear_volumes();
        no->clear_instances();
        no->cut_connectors.clear();
        no->cut_id.invalidate();
        no->name = name;
        add_part(no, name, std::move(its), ModelVolumeType::MODEL_PART);
        return no;
    };
    auto put_on_bed = [&](ModelObject *no) {
        ModelInstance *ni = no->add_instance();
        ni->set_offset(st->offset);
        no->invalidate_bounding_box();
        no->ensure_on_bed();
    };
    for (size_t i = 0; i < sides.size(); ++i) {
        const bool   is_from = sides[i] == from_side;
        // The volumes that are not cut go to the pieces they are in (decided by the cut job).
        const unsigned char bit = sides[i] == &grouped.side_a ? 1 : 2;
        ModelObject *no      = new_object(names[i], std::move(*sides[i]));
        for (size_t k = 0; k < st->extras.size(); ++k) {
            if (k >= st->extra_sides.size() || !(st->extra_sides[k] & bit))
                continue;
            const ModelVolume *src = mo->volumes[st->extras[k].volume_idx]; // unchanged: checked above
            ModelVolume       *nv  = no->add_volume(TriangleMesh(st->extras[k].mesh), src->type());
            nv->name               = src->name;
            nv->config.apply(src->config);
        }
        if (connectors) {
            const std::string name = _u8L("Connector");
            if (is_from) {
                add_part(no, name, conn.pegs, ModelVolumeType::MODEL_PART);
                add_part(no, name, conn.holes_from, ModelVolumeType::NEGATIVE_VOLUME);
            } else
                add_part(no, name, conn.holes_to, ModelVolumeType::NEGATIVE_VOLUME);
        }
        put_on_bed(no);
    }
    // Separate pins: one object each, standing on the bed.
    const size_t n_cut = tmp.objects.size();
    if (connectors && !conn.pin.indices.empty())
        for (size_t k = 0; k < conn.positions.size(); ++k)
            // TRN %1%: the name of the object that was cut, %2%: the number of the pin
            put_on_bed(new_object(GUI::format(_u8L("%1% - pin %2%"), mo->name, k + 1), conn.pin));
    // The pieces now rest on the bed; put the detached one (and the pins) in the free spots of the current
    // plate closest to the rest, so nothing overlaps the rest, each other or any other object.
    if (n_cut == 2) {
        const BoundingBoxf3        rest_bb = tmp.objects[1]->instance_bounding_box(0);
        std::vector<BoundingBoxf3> placed;
        for (size_t i = 0; i < tmp.objects.size(); ++i) {
            if (i == 1)
                continue;
            place_piece(*tmp.objects[i], rest_bb, mo, placed);
            placed.push_back(tmp.objects[i]->instance_bounding_box(0));
        }
    }
    if (grouped.n_small > 0)
        BOOST_LOG_TRIVIAL(info) << "PathCut: " << grouped.n_small << " tiny fragments kept with their side";

    // Still in one piece: the tool opens again on it, showing the cuts made so far (their faces) and
    // where it is still connected, so the next cut can be marked right away.
    indexed_triangle_set            done_faces;
    std::vector<std::vector<Vec3d>> done_routes;
    if (st->joined && tmp.objects.size() == 1) {
        const Vec3d shift = tmp.objects[0]->instances[0]->get_offset() - st->offset; // put on the bed
        done_faces        = m_done_faces;
        for (auto &v : done_faces.vertices) v += shift.cast<float>();
        const int base = int(done_faces.vertices.size());
        for (const auto &v : st->cut_faces.vertices) done_faces.vertices.push_back(v + (st->offset + shift).cast<float>());
        for (const auto &f : st->cut_faces.indices) done_faces.indices.emplace_back(f[0] + base, f[1] + base, f[2] + base);
        for (const auto &route : st->routes) {
            done_routes.emplace_back();
            for (const PathCut::V3 &p : route) done_routes.back().push_back(p + st->offset + shift);
        }
    }

    // The object is replaced: the faces of its earlier cuts are carried over above only if it stays in one piece.
    m_done_faces.clear();
    m_done_routes.clear();
    m_done_object = ObjectID();
    m_message.clear();
    m_parent.reset_all_gizmos();
    const auto t0 = std::chrono::steady_clock::now();
    Plater::TakeSnapshot snapshot(plater, _u8L("Cut along path"));
    plater->apply_cut_object_to_model(size_t(st->obj_idx), tmp.objects);
    auto warn = [plater](const std::string &text) {
        plater->get_notification_manager()->push_notification(NotificationType::CustomNotification,
                                                              NotificationManager::NotificationLevel::WarningNotificationLevel, text);
    };
    if (st->joined) {
        warn(st->conn.type == PathCut::ConnectorType::None ?
                 _u8L("Cut along path: the cut is made, but the object is still in one piece. Cut the remaining connection to separate it.") :
                 _u8L("Cut along path: the cut is made, but the object is still in one piece. Cut the remaining connection to "
                      "separate it. Connectors are only added once the cut separates the object."));
        if (!done_faces.indices.empty())
            wxGetApp().CallAfter([this, done_faces = std::move(done_faces), done_routes = std::move(done_routes)]() mutable {
                if (!m_parent.get_gizmos_manager().open_gizmo(GLGizmosManager::PathCut))
                    return;
                m_done_faces  = std::move(done_faces);
                m_done_routes = std::move(done_routes);
                // The instance the tool opened on: the one left in one piece.
                const Selection &selection = m_parent.get_selection();
                const Model     &model     = wxGetApp().plater()->model();
                const int        obj_idx   = selection.get_object_idx();
                const int        inst_idx  = selection.get_instance_idx();
                if (obj_idx >= 0 && obj_idx < int(model.objects.size()) && inst_idx >= 0 &&
                    inst_idx < int(model.objects[obj_idx]->instances.size())) {
                    const ModelInstance *mi = model.objects[obj_idx]->instances[inst_idx];
                    m_done_object = mi->id();
                    m_done_matrix = mi->get_matrix();
                }
                rebuild_done();
                m_message          = _u8L("The cut is made, shown in orange. The object is still connected along the red lines: mark the next cut there.");
                m_message_is_error = false;
                m_parent.set_as_dirty();
            });
    } else if (st->conn.type != PathCut::ConnectorType::None) {
        if (!st->connector_error.empty())
            // TRN %1%: why no connector could be placed
            warn(GUI::format(_u8L("Cut along path: %1%"), _u8L(st->connector_error.c_str())));
        else if (!st->conn_manual && int(conn.positions.size()) < st->conn.count)
            // TRN %1%: connectors that fit, %2%: connectors asked for
            warn(GUI::format(_L_PLURAL("Cut along path: only %1% of %2% connectors fits on the cut face.",
                                       "Cut along path: only %1% of %2% connectors fit on the cut face.", conn.positions.size()),
                             conn.positions.size(), st->conn.count));
    }
    if (st->merged_parts > 1)
        warn(_u8L("Cut along path: the parts of the object were joined into one before cutting."));
    BOOST_LOG_TRIVIAL(info) << "PathCut: apply to model "
                            << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()
                            << " ms";
}

} // namespace GUI
} // namespace Slic3r
