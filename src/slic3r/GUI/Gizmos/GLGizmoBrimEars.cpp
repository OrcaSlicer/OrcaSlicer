#include "GLGizmoBrimEars.hpp"
#include <GL/glew.h>
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosCommon.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"

namespace Slic3r { namespace GUI {

static const ColorRGBA DEF_COLOR   = {0.7f, 0.7f, 0.7f, 1.f};
static const ColorRGBA SELECTED_COLOR = {0.0f, 0.5f, 0.5f, 1.0f};
static const ColorRGBA ERR_COLOR = {1.0f, 0.3f, 0.3f, 0.5f};
static const ColorRGBA HOVER_COLOR = {0.7f, 0.7f, 0.7f, 0.5f};

static ModelVolume *get_model_volume(const Selection &selection, Model &model)
{
    const Selection::IndicesList &idxs = selection.get_volume_idxs();
    // only one selected volume
    if (idxs.size() != 1) return nullptr;
    const GLVolume *selected_volume = selection.get_volume(*idxs.begin());
    if (selected_volume == nullptr) return nullptr;

    const GLVolume::CompositeID &cid  = selected_volume->composite_id;
    const ModelObjectPtrs       &objs = model.objects;
    if (cid.object_id < 0 || objs.size() <= static_cast<size_t>(cid.object_id)) return nullptr;
    const ModelObject *obj = objs[cid.object_id];
    if (cid.volume_id < 0 || obj->volumes.size() <= static_cast<size_t>(cid.volume_id)) return nullptr;
    return obj->volumes[cid.volume_id];
}

GLGizmoBrimEars::GLGizmoBrimEars(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id) : GLGizmoBase(parent, icon_filename, sprite_id)
{
    GLModel::Geometry cylinder_geometry = smooth_cylinder(16, 1.0f, 1.0f);
    m_cylinder.mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(cylinder_geometry.get_as_indexed_triangle_set()));
    m_cylinder.model.init_from(std::move(cylinder_geometry));
}

bool GLGizmoBrimEars::on_init()
{
    m_new_point_head_diameter = get_brim_default_radius();

    m_shortcut_key = WXK_CONTROL_E;

    // FIXME: maybe should be using GUI::shortkey_ctrl_prefix() or equivalent?
    const wxString ctrl  = _L("Ctrl+");
    // FIXME: maybe should be using GUI::shortkey_alt_prefix() or equivalent?
    const wxString alt   = _L("Alt+");

    m_desc["head_diameter"]    = _L("Head diameter");
    m_desc["max_angle"]        = _L("Max angle");
    m_desc["detection_radius"] = _L("Detection radius");
    m_desc["remove_selected"]  = _L("Remove selected points");
    m_desc["remove_all"]       = _L("Remove all");
    m_desc["auto_generate"]    = _L("Auto-generate points");
    m_desc["section_view"]     = _L("Section view");

    m_desc["left_click_caption"]       = _L("Left click");
    m_desc["left_click"]               = _L("Add a brim ear");
    m_desc["right_click_caption"]      = _L("Right click");
    m_desc["right_click"]              = _L("Delete a brim ear");
    m_desc["ctrl_mouse_wheel_caption"] = ctrl + _L("Mouse wheel");
    m_desc["ctrl_mouse_wheel"]         = _L("Adjust head diameter");
    m_desc["alt_mouse_wheel_caption"]  = alt + _L("Mouse wheel");
    m_desc["alt_mouse_wheel"]          = _L("Adjust section view");

    return true;
}

void GLGizmoBrimEars::set_brim_data()
{
    if (!m_c->selection_info()) return;

    ModelObject *mo = m_c->selection_info()->model_object();

    if (m_state == On && mo && mo->id() != m_old_mo_id) {
        reload_cache();
        m_old_mo_id = mo->id();
    }
}

void GLGizmoBrimEars::on_render()
{
    ModelObject     *mo        = m_c->selection_info()->model_object();
    const Selection &selection = m_parent.get_selection();

    // If current m_c->m_model_object does not match selection, ask GLCanvas3D to turn us off
    if (m_state == On && (mo != selection.get_model()->objects[selection.get_object_idx()] || m_c->selection_info()->get_active_instance() != selection.get_instance_idx())) {
        m_parent.post_event(SimpleEvent(EVT_GLCANVAS_RESETGIZMOS));
        return;
    }

    glsafe(::glEnable(GL_BLEND));
    glsafe(::glEnable(GL_DEPTH_TEST));

    if (selection.is_from_single_instance()) render_points(selection);

    m_selection_rectangle.render(m_parent);
    m_c->object_clipper()->render_cut();

    glsafe(::glDisable(GL_BLEND));
}

void GLGizmoBrimEars::render_points(const Selection &selection)
{
    auto editing_cache = m_editing_cache;
    if (render_hover_point) { editing_cache.push_back(*render_hover_point); }

    size_t cache_size = editing_cache.size();

    bool has_points = (cache_size != 0);

    if (!has_points) return;

    const auto shader = wxGetApp().get_shader("gouraud_light");
    if (shader == nullptr)
        return;
    shader->start_using();
    ScopeGuard guard([shader]() {
        shader->stop_using();
    });

    const Camera&      camera                          = wxGetApp().plater()->get_camera();
    const Transform3d& view_matrix                     = camera.get_view_matrix();
    const GLVolume    *vol                             = selection.get_volume(*selection.get_volume_idxs().begin());
    const Transform3d &instance_scaling_matrix_inverse = vol->get_instance_transformation().get_scaling_factor_matrix().inverse();
    const Transform3d &instance_matrix                 = vol->get_instance_transformation().get_matrix();

    shader->set_uniform("projection_matrix", camera.get_projection_matrix());

    ColorRGBA render_color;
    for (size_t i = 0; i < cache_size; ++i) {
        const BrimPoint &brim_point     = editing_cache[i].brim_point;
        const bool      &point_selected = editing_cache[i].selected;
        const bool      &hover          = editing_cache[i].is_hover;
        const bool      &error          = editing_cache[i].is_error;
        // keep show brim ear
        // if (is_mesh_point_clipped(brim_point.pos.cast<double>()))
        //     continue;

        // First decide about the color of the point.
        if (hover) {
            render_color = HOVER_COLOR;
        } else {
            if (size_t(m_hover_id) == i) // ignore hover state unless editing mode is active
                render_color = {0.f, 1.f, 1.f, 1.f};
            else { // neigher hover nor picking
                if (point_selected)
                    render_color = SELECTED_COLOR;
                else {
                    if (error)
                        render_color = ERR_COLOR;
                    else
                        render_color = DEF_COLOR;
                }
            }
        }

        m_cylinder.model.set_color(render_color);
        shader->set_uniform("emission_factor", 0.5f);

        if (vol->is_left_handed()) glFrontFace(GL_CW);

        // Matrices set, we can render the point mark now.
        // If in editing mode, we'll also render a cone pointing to the sphere.
        if (editing_cache[i].normal == Vec3f::Zero()) m_c->raycaster()->raycaster()->get_closest_point(editing_cache[i].brim_point.pos, &editing_cache[i].normal);

        Eigen::Quaterniond q;
        q.setFromTwoVectors(Vec3d{0., 0., 1.}, instance_scaling_matrix_inverse * editing_cache[i].normal.cast<double>());

        double radius = (double) brim_point.head_front_radius * RenderPointScale;
        const Transform3d center_matrix =
            instance_matrix
            * Geometry::translation_transform(brim_point.pos.cast<double>())
            // Inverse matrix of the instance scaling is applied so that the mark does not scale with the object.
            * instance_scaling_matrix_inverse
            * q
            * Geometry::scale_transform(Vec3d{radius, radius, .2});
        if (i < m_grabbers.size()) {
            m_grabbers[i].raycasters[0]->set_transform(center_matrix);
        }
        shader->set_uniform("view_model_matrix", view_matrix * center_matrix);
        m_cylinder.model.render();

        if (vol->is_left_handed()) glFrontFace(GL_CCW);
    }
}

bool GLGizmoBrimEars::is_mesh_point_clipped(const Vec3d &point) const
{
    if (m_c->object_clipper()->get_position() == 0.) return false;

    auto                 sel_info    = m_c->selection_info();
    int                  active_inst = m_c->selection_info()->get_active_instance();
    const ModelInstance *mi          = sel_info->model_object()->instances[active_inst];
    const Transform3d   &trafo       = mi->get_transformation().get_matrix();

    Vec3d transformed_point = trafo * point;
    transformed_point(2) += sel_info->get_sla_shift();
    return m_c->object_clipper()->get_clipping_plane()->is_point_clipped(transformed_point);
}

bool GLGizmoBrimEars::unproject_on_mesh2(const Vec2d &mouse_pos, std::pair<Vec3f, Vec3f> &pos_and_normal)
{
    const Camera        &camera          = wxGetApp().plater()->get_camera();
    double               clp_dist        = m_c->object_clipper()->get_position();
    const ClippingPlane *clp             = m_c->object_clipper()->get_clipping_plane();
    bool                 mouse_on_object = false;
    Vec3f                position_on_model {};
    Vec3f                normal_on_model {};
    double               closest_hit_distance = std::numeric_limits<double>::max();

    for (auto item : m_mesh_raycaster_map) {
        auto raycaster  = item.second->get_raycaster();
        auto& world_tran = item.second->get_transform();
        Vec3f normal     = Vec3f::Zero();
        Vec3f hit        = Vec3f::Zero();
        if (raycaster->unproject_on_mesh(mouse_pos, world_tran, camera, hit, normal, clp_dist != 0. ? clp : nullptr)) {
            double hit_squared_distance = (camera.get_position() - world_tran * hit.cast<double>()).norm();
            if (hit_squared_distance < closest_hit_distance) {
                closest_hit_distance = hit_squared_distance;
                mouse_on_object      = true;
                m_last_hit_volume    = item.first;
                auto volum_trsf      = m_last_hit_volume->get_volume_transformation().get_matrix();
                position_on_model    = (m_last_hit_volume->get_volume_transformation().get_matrix() * hit.cast<double>()).cast<float>();
                normal_on_model      = normal;
            }
        }
    }
    pos_and_normal = std::make_pair(position_on_model, normal_on_model);
    return mouse_on_object;
}
// Unprojects the mouse position on the mesh and saves hit point and normal of the facet into pos_and_normal
// Return false if no intersection was found, true otherwise.
bool GLGizmoBrimEars::unproject_on_mesh(const Vec2d &mouse_pos, std::pair<Vec3f, Vec3f> &pos_and_normal)
{
    if (m_c->raycaster()->raycasters().size() != 1) return false;
    if (!m_c->raycaster()->raycaster()) return false;

    const Camera            &camera    = wxGetApp().plater()->get_camera();
    const Selection         &selection = m_parent.get_selection();
    const GLVolume          *volume    = selection.get_volume(*selection.get_volume_idxs().begin());
    Geometry::Transformation trafo     = volume->get_instance_transformation();
    // trafo.set_offset(trafo.get_offset() + Vec3d(0., 0., m_c->selection_info()->get_sla_shift()));//sla shift看起来可以删掉

    double               clp_dist = m_c->object_clipper()->get_position();
    const ClippingPlane *clp      = m_c->object_clipper()->get_clipping_plane();

    // The raycaster query
    Vec3f hit;
    Vec3f normal;
    if (m_c->raycaster()->raycaster()->unproject_on_mesh(mouse_pos, trafo.get_matrix(), camera, hit, normal, clp_dist != 0. ? clp : nullptr)) {
        pos_and_normal = std::make_pair(hit, normal);
        return true;
    }

    return false;
}

void GLGizmoBrimEars::data_changed(bool is_serializing)
{
    if (!m_c->selection_info()) return;

    ModelObject *mo = m_c->selection_info()->model_object();
    if (mo) {
        reset_all_pick();
        register_single_mesh_pick();
    }

    set_brim_data();
}


bool GLGizmoBrimEars::on_mouse(const wxMouseEvent& mouse_event)
{
    // wxCoord == int --> wx/types.h
    Vec2i32 mouse_coord(mouse_event.GetX(), mouse_event.GetY());
    Vec2d   mouse_pos = mouse_coord.cast<double>();

    if (mouse_event.Moving()) {
        gizmo_event(SLAGizmoEventType::Moving, mouse_pos, mouse_event.ShiftDown(), mouse_event.AltDown(), false);
    }

    // when control is down we allow scene pan and rotation even when clicking
    // over some object
    bool control_down           = mouse_event.CmdDown();
    bool grabber_contains_mouse = (get_hover_id() != -1);

    const Selection &selection = m_parent.get_selection();
    int selected_object_idx = selection.get_object_idx();
    if (mouse_event.LeftDown()) {
        if ((!control_down || grabber_contains_mouse) &&
            gizmo_event(SLAGizmoEventType::LeftDown, mouse_pos, mouse_event.ShiftDown(), mouse_event.AltDown(), false))
            // the gizmo got the event and took some action, there is no need
            // to do anything more
            return true;
    } else if (mouse_event.RightDown()){
        if (!control_down && selected_object_idx != -1 &&
            gizmo_event(SLAGizmoEventType::RightDown, mouse_pos, false, false, false))
            // event was taken care of
            return true;
    } else if (mouse_event.Dragging()) {
        if (m_parent.get_move_volume_id() != -1)
            // don't allow dragging objects with the Sla gizmo on
            return true;
        if (!control_down && gizmo_event(SLAGizmoEventType::Dragging,
                                         mouse_pos, mouse_event.ShiftDown(),
                                         mouse_event.AltDown(), false)) {
            // the gizmo got the event and took some action, no need to do
            // anything more here
            m_parent.set_as_dirty();
            return true;
        }
        if(control_down && (mouse_event.LeftIsDown() || mouse_event.RightIsDown()))
        {
            // CTRL has been pressed while already dragging -> stop current action
            if (mouse_event.LeftIsDown())
                gizmo_event(SLAGizmoEventType::LeftUp, mouse_pos, mouse_event.ShiftDown(), mouse_event.AltDown(), true);
            else if (mouse_event.RightIsDown())
                gizmo_event(SLAGizmoEventType::RightUp, mouse_pos, mouse_event.ShiftDown(), mouse_event.AltDown(), true);
            return false;
        }
    } else if (mouse_event.LeftUp()) {
        if (gizmo_event(SLAGizmoEventType::LeftUp, mouse_pos, mouse_event.ShiftDown(), mouse_event.AltDown(), control_down)
            && !m_parent.is_mouse_dragging()) {
            // in case SLA/FDM gizmo is selected, we just pass the LeftUp
            // event and stop processing - neither object moving or selecting
            // is suppressed in that case
            return true;
        }
    }
    return use_grabbers(mouse_event);
}

// Following function is called from GLCanvas3D to inform the gizmo about a mouse/keyboard event.
// The gizmo has an opportunity to react - if it does, it should return true so that the Canvas3D is
// aware that the event was reacted to and stops trying to make different sense of it. If the gizmo
// concludes that the event was not intended for it, it should return false.
bool GLGizmoBrimEars::gizmo_event(SLAGizmoEventType action, const Vec2d &mouse_position, bool shift_down, bool alt_down, bool control_down)
{
    if (action != SLAGizmoEventType::MouseWheelDown || action != SLAGizmoEventType::MouseWheelUp || action != SLAGizmoEventType::Moving) {
        apply_radius_change();
    }

    ModelObject *mo          = m_c->selection_info()->model_object();
    int          active_inst = m_c->selection_info()->get_active_instance();

    if (action == SLAGizmoEventType::Moving) {
        // First check that the mouse pointer is on an object.
        const Selection     &selection = m_parent.get_selection();
        const ModelInstance *mi        = mo->instances[0];
        Plater              *plater    = wxGetApp().plater();
        if (!plater) return false;
        const Camera           &camera       = wxGetApp().plater()->get_camera();
        const GLVolume         *volume       = selection.get_volume(*selection.get_volume_idxs().begin());
        Transform3d             inverse_trsf = volume->get_instance_transformation().get_matrix_no_offset().inverse();
        std::pair<Vec3f, Vec3f> pos_and_normal;
        if (unproject_on_mesh2(mouse_position, pos_and_normal)) {
            render_hover_point = CacheEntry(BrimPoint(pos_and_normal.first, m_new_point_head_diameter / 2.f), false, (inverse_trsf * m_world_normal).cast<float>(), true);
        } else {
            render_hover_point.reset();
        }
    } else if (action == SLAGizmoEventType::LeftDown && (shift_down || alt_down || control_down)) {
        // left down with shift - show the selection rectangle:
        if (m_hover_id == -1) {
            if (shift_down || alt_down) { m_selection_rectangle.start_dragging(mouse_position, shift_down ? GLSelectionRectangle::Select : GLSelectionRectangle::Deselect); }
        } else {
            if (m_editing_cache[m_hover_id].selected)
                unselect_point(m_hover_id);
            else {
                if (!alt_down) select_point(m_hover_id);
            }
        }

        return true;
    }

    // left down without selection rectangle - place point on the mesh:
    if (action == SLAGizmoEventType::LeftDown && !m_selection_rectangle.is_dragging() && !shift_down) {
        // If any point is in hover state, this should initiate its move - return control back to GLCanvas:
        if (m_hover_id != -1) return false;

        // If there is some selection, don't add new point and deselect everything instead.
        if (m_selection_empty) {
            std::pair<Vec3f, Vec3f> pos_and_normal;
            if (unproject_on_mesh2(mouse_position, pos_and_normal)) {
                // we got an intersection
                const Selection &selection    = m_parent.get_selection();
                const GLVolume  *volume       = selection.get_volume(*selection.get_volume_idxs().begin());
                Transform3d      trsf         = volume->get_instance_transformation().get_matrix();
                Transform3d      inverse_trsf = volume->get_instance_transformation().get_matrix_no_offset().inverse();
                // BBS brim ear postion is placed on the bottom side
                Vec3d world_pos  = trsf * pos_and_normal.first.cast<double>();
                world_pos[2]     = -0.0001;
                Vec3d object_pos = trsf.inverse() * world_pos;
                // brim ear always face up
                Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Add brim ear");
                add_point_to_cache(object_pos.cast<float>(), m_new_point_head_diameter / 2.f, false, (inverse_trsf * m_world_normal).cast<float>());
                m_parent.set_as_dirty();
                m_wait_for_up_event = true;
                find_single();
            } else
                return false;
        } else
            select_point(NoPoints);

        return true;
    }

    // left up with selection rectangle - select points inside the rectangle:
    if ((action == SLAGizmoEventType::LeftUp || action == SLAGizmoEventType::ShiftUp || action == SLAGizmoEventType::AltUp) && m_selection_rectangle.is_dragging()) {
        // Is this a selection or deselection rectangle?
        GLSelectionRectangle::EState rectangle_status = m_selection_rectangle.get_state();

        // First collect positions of all the points in world coordinates.
        Geometry::Transformation trafo = mo->instances[active_inst]->get_transformation();
        // trafo.set_offset(trafo.get_offset() + Vec3d(0., 0., m_c->selection_info()->get_sla_shift()));
        std::vector<Vec3d> points;
        for (unsigned int i = 0; i < m_editing_cache.size(); ++i) points.push_back(trafo.get_matrix() * m_editing_cache[i].brim_point.pos.cast<double>());

        // Now ask the rectangle which of the points are inside.
        std::vector<Vec3f>        points_inside;
        std::vector<unsigned int> points_idxs = m_selection_rectangle.contains(points);
        m_selection_rectangle.stop_dragging();
        for (size_t idx : points_idxs) points_inside.push_back(points[idx].cast<float>());

        // Only select/deselect points that are actually visible. We want to check not only
        // the point itself, but also the center of base of its cone, so the points don't hide
        // under every miniature irregularity on the model. Remember the actual number and
        // append the cone bases.
        size_t orig_pts_num = points_inside.size();
        for (size_t idx : points_idxs)
            points_inside.emplace_back((trafo.get_matrix().cast<float>() * (m_editing_cache[idx].brim_point.pos + m_editing_cache[idx].normal)).cast<float>());

        for (size_t idx :
             m_c->raycaster()->raycaster()->get_unobscured_idxs(trafo, wxGetApp().plater()->get_camera(), points_inside, m_c->object_clipper()->get_clipping_plane())) {
            if (idx >= orig_pts_num) // this is a cone-base, get index of point it belongs to
                idx -= orig_pts_num;
            if (rectangle_status == GLSelectionRectangle::Deselect)
                unselect_point(points_idxs[idx]);
            else
                select_point(points_idxs[idx]);
        }
        return true;
    }

    // left up with no selection rectangle
    if (action == SLAGizmoEventType::LeftUp) {
        if (m_wait_for_up_event) { m_wait_for_up_event = false; }
        return true;
    }

    // dragging the selection rectangle:
    if (action == SLAGizmoEventType::Dragging) {
        if (m_wait_for_up_event)
            return true; // point has been placed and the button not released yet
                         // this prevents GLCanvas from starting scene rotation

        if (m_selection_rectangle.is_dragging()) {
            m_selection_rectangle.dragging(mouse_position);
            return true;
        }

        return false;
    }

    if (action == SLAGizmoEventType::Delete) {
        // delete key pressed
        delete_selected_points();
        return true;
    }

    if (action == SLAGizmoEventType::RightDown) {
        if (m_hover_id != -1) {
            select_point(NoPoints);
            select_point(m_hover_id);
            delete_selected_points();
            return true;
        }
        return false;
    }

    if (action == SLAGizmoEventType::SelectAll) {
        select_point(AllPoints);
        return true;
    }

    // mouse wheel up
    if (action == SLAGizmoEventType::MouseWheelUp) {
        if (control_down) {
            float initial_value = m_new_point_head_diameter;
            begin_radius_change(initial_value);
            m_new_point_head_diameter = std::min(20., initial_value + 0.1);
            update_cache_radius();
            return true;
        }

        apply_radius_change();
    }

    if (action == SLAGizmoEventType::MouseWheelDown) {
        if (control_down) {
            float initial_value = m_new_point_head_diameter;
            begin_radius_change(initial_value);
            m_new_point_head_diameter = std::max(5., initial_value - 0.1);
            update_cache_radius();
            return true;
        }

        apply_radius_change();
    }

    if (action == SLAGizmoEventType::MouseWheelUp && alt_down) {
        double pos = m_c->object_clipper()->get_position();
        pos        = std::min(1., pos + 0.01);
        m_c->object_clipper()->set_position_by_ratio(pos, false, true);
        return true;
    }

    if (action == SLAGizmoEventType::MouseWheelDown && alt_down) {
        double pos = m_c->object_clipper()->get_position();
        pos        = std::max(0., pos - 0.01);
        m_c->object_clipper()->set_position_by_ratio(pos, false, true);
        return true;
    }

    // reset clipper position
    if (action == SLAGizmoEventType::ResetClippingPlane) {
        m_c->object_clipper()->set_position_by_ratio(-1., false);
        return true;
    }

    return false;
}

void GLGizmoBrimEars::delete_selected_points()
{
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Delete brim ear");

    for (unsigned int idx = 0; idx < m_editing_cache.size(); ++idx) {
        if (m_editing_cache[idx].selected) { m_editing_cache.erase(m_editing_cache.begin() + (idx--)); }
    }

    select_point(NoPoints);
    find_single();
    update_model_object();
}

void GLGizmoBrimEars::on_dragging(const UpdateData& data)
{
    if (m_hover_id != -1) {
        std::pair<Vec3f, Vec3f> pos_and_normal;
        if (!unproject_on_mesh2(data.mouse_pos.cast<double>(), pos_and_normal)) return;
        m_editing_cache[m_hover_id].brim_point.pos[0] = pos_and_normal.first.x();
        m_editing_cache[m_hover_id].brim_point.pos[1] = pos_and_normal.first.y();
        //m_editing_cache[m_hover_id].normal            = pos_and_normal.second;
        m_editing_cache[m_hover_id].normal = Vec3f(0, 0, 1);
        find_single();
    }
}

std::vector<const ConfigOption *> GLGizmoBrimEars::get_config_options(const std::vector<std::string> &keys) const
{
    std::vector<const ConfigOption *> out;
    const ModelObject                *mo = m_c->selection_info()->model_object();

    if (!mo) return out;

    const DynamicPrintConfig           &object_cfg  = mo->config.get();
    const DynamicPrintConfig           &print_cfg   = wxGetApp().preset_bundle->sla_prints.get_edited_preset().config;
    std::unique_ptr<DynamicPrintConfig> default_cfg = nullptr;

    for (const std::string &key : keys) {
        if (object_cfg.has(key))
            out.push_back(object_cfg.option(key));
        else if (print_cfg.has(key))
            out.push_back(print_cfg.option(key));
        else { // we must get it from defaults
            if (default_cfg == nullptr) default_cfg.reset(DynamicPrintConfig::new_from_defaults_keys(keys));
            out.push_back(default_cfg->option(key));
        }
    }

    return out;
}

void GLGizmoBrimEars::begin_radius_change(float initial_value)
{
    if (m_old_point_head_diameter == 0.f)
        m_old_point_head_diameter = initial_value;
}

void GLGizmoBrimEars::update_cache_radius()
{
    if (render_hover_point)
        render_hover_point->brim_point.head_front_radius = m_new_point_head_diameter / 2.f;

    for (auto &cache_entry : m_editing_cache)
        if (cache_entry.selected) {
            cache_entry.brim_point.head_front_radius = m_new_point_head_diameter / 2.f;
            find_single();
            update_model_object();
        }
    m_parent.set_as_dirty();
}

void GLGizmoBrimEars::apply_radius_change()
{
    if (m_old_point_head_diameter == 0.f) return;

    // momentarily restore the old value to take snapshot
    for (auto& cache_entry : m_editing_cache)
        if (cache_entry.selected)
            cache_entry.brim_point.head_front_radius = m_old_point_head_diameter / 2.f;
    float backup              = m_new_point_head_diameter;
    m_new_point_head_diameter = m_old_point_head_diameter;
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Change point head diameter");
    m_new_point_head_diameter = backup;
    update_cache_radius();
    m_old_point_head_diameter = 0.f;
}

void GLGizmoBrimEars::on_render_input_window(float x, float y, float bottom_limit)
{
    static float last_y = 0.0f;
    static float last_h = 0.0f;

    ModelObject *mo = m_c->selection_info()->model_object();

    if (!mo) return;

    const DynamicPrintConfig& obj_cfg = mo->config.get();
    const DynamicPrintConfig& glb_cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    const float win_h = ImGui::GetWindowHeight();
    y                 = std::min(y, bottom_limit - win_h);
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);

    const float currt_scale = m_parent.get_scale();
    ImGuiWrapper::push_toolbar_style(currt_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0 * currt_scale, 5.0 * currt_scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 4.0f * currt_scale);
    GizmoImguiBegin(get_name(),
                    ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    float                 space_size      = m_imgui->get_style_scaling() * 8;
    std::vector<wxString> text_list       = {m_desc["head_diameter"], m_desc["max_angle"], m_desc["detection_radius"], m_desc["clipping_of_view"]};
    float                 widest_text     = m_imgui->find_widest_text(text_list);
    float                 caption_size    = widest_text + space_size + ImGui::GetStyle().WindowPadding.x;
    float                 input_text_size = m_imgui->scaled(10.0f);
    float                 button_size     = ImGui::GetFrameHeight();

    float list_width      = input_text_size + ImGui::GetStyle().ScrollbarSize + 2 * currt_scale;

    const float slider_icon_width = m_imgui->get_slider_icon_size().x;
    const float slider_width      = list_width - space_size;
    const float drag_left_width   = caption_size + slider_width + space_size;

    // adjust window position to avoid overlap the view toolbar
    if (last_h != win_h || last_y != y) {
        // ask canvas for another frame to render the window in the correct position
        m_imgui->set_requires_extra_frame();
        if (last_h != win_h) last_h = win_h;
        if (last_y != y) last_y = y;
    }

    ImGui::AlignTextToFramePadding();

    // Following is a nasty way to:
    //  - save the initial value of the slider before one starts messing with it
    //  - keep updating the head radius during sliding so it is continuosly refreshed in 3D scene
    //  - take correct undo/redo snapshot after the user is done with moving the slider
    float initial_value = m_new_point_head_diameter;
    m_imgui->text(m_desc["head_diameter"]);
    ImGui::SameLine(caption_size);
    ImGui::PushItemWidth(slider_width);
    m_imgui->bbl_slider_float_style("##head_diameter", &m_new_point_head_diameter, 5, 20, "%.1f", 1.0f, true);
    if (m_imgui->get_last_slider_status().clicked) {
        begin_radius_change(initial_value);
    }
    if (m_imgui->get_last_slider_status().edited)
        update_cache_radius();
    if (m_imgui->get_last_slider_status().deactivated_after_edit) {
        apply_radius_change();
    }
    ImGui::SameLine(drag_left_width);
    ImGui::PushItemWidth(1.5 * slider_icon_width);
    ImGui::BBLDragFloat("##head_diameter_input", &m_new_point_head_diameter, 0.05f, 0.0f, 0.0f, "%.1f");
    ImGui::AlignTextToFramePadding();

    m_imgui->text(m_desc["max_angle"]);
    ImGui::SameLine(caption_size);
    ImGui::PushItemWidth(slider_width);
    m_imgui->bbl_slider_float_style("##max_angle", &m_max_angle, 0, 180, "%.1f", 1.0f, true);
    ImGui::SameLine(drag_left_width);
    ImGui::PushItemWidth(1.5 * slider_icon_width);
    ImGui::BBLDragFloat("##max_angle_input", &m_max_angle, 0.05f, 0.0f, 180.0f, "%.1f");
    ImGui::AlignTextToFramePadding();

    m_imgui->text(m_desc["detection_radius"]);
    ImGui::SameLine(caption_size);
    ImGui::PushItemWidth(slider_width);
    m_imgui->bbl_slider_float_style("##detection_radius", &m_detection_radius, 0, static_cast<int>(m_detection_radius_max), "%.1f", 1.0f, true);
    ImGui::SameLine(drag_left_width);
    ImGui::PushItemWidth(1.5 * slider_icon_width);
    ImGui::BBLDragFloat("##detection_radius_input", &m_detection_radius, 0.05f, 0.0f, static_cast<float>(m_detection_radius_max), "%.1f");
    ImGui::Separator();

    float clp_dist = float(m_c->object_clipper()->get_position());
    m_imgui->text(m_desc["section_view"]);
    ImGui::SameLine(caption_size);
    ImGui::PushItemWidth(slider_width);
    bool slider_clp_dist = m_imgui->bbl_slider_float_style("##section_view", &clp_dist, 0.f, 1.f, "%.2f", 1.0f, true);
    ImGui::SameLine(drag_left_width);
    ImGui::PushItemWidth(1.5 * slider_icon_width);
    bool b_clp_dist_input = ImGui::BBLDragFloat("##section_view_input", &clp_dist, 0.05f, 0.0f, 0.0f, "%.2f");
    if (slider_clp_dist || b_clp_dist_input) { m_c->object_clipper()->set_position_by_ratio(clp_dist, false, true); }
    ImGui::Separator();

    // ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 10.0f));

    float f_scale = m_parent.get_gizmos_manager().get_layout_scale();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f * f_scale));
    if (m_imgui->button(m_desc["auto_generate"])) { auto_generate(); }

    if (m_imgui->button(m_desc["remove_selected"])) { delete_selected_points(); }
    float font_size = ImGui::GetFontSize();
    //ImGui::Dummy(ImVec2(font_size * 1, font_size * 1.3));
    ImGui::SameLine();
    if (m_imgui->button(m_desc["remove_all"])) {
        if (m_editing_cache.size() > 0) {
            select_point(AllPoints);
            delete_selected_points();
        }
    }
    ImGui::PopStyleVar(1);

    float get_cur_y = ImGui::GetContentRegionMax().y + ImGui::GetFrameHeight() + y;
    show_tooltip_information(x, get_cur_y);

    if (glb_cfg.opt_enum<BrimType>("brim_type") != btPainted) {
        ImGui::SameLine();
        auto link_text = [&]() {
            ImColor HyperColor = ImGuiWrapper::COL_ORCA;
            ImGui::PushStyleColor(ImGuiCol_Text, ImGuiWrapper::to_ImVec4(ColorRGB::WARNING()));
            float parent_width = ImGui::GetContentRegionAvail().x;
            m_imgui->text_wrapped(_L("Warning: The brim type is not set to \"painted\", the brim ears will not take effect!"), parent_width);
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_Text, HyperColor.Value);
            ImGui::Dummy(ImVec2(font_size * 1.8, font_size * 1.3));
            ImGui::SameLine();
            m_imgui->bold_text(_u8L("Set the brim type of this object to \"painted\""));
            ImGui::PopStyleColor();
            // underline
            ImVec2 lineEnd = ImGui::GetItemRectMax();
            lineEnd.y -= 2.0f;
            ImVec2 lineStart = lineEnd;
            lineStart.x = ImGui::GetItemRectMin().x;
            ImGui::GetWindowDrawList()->AddLine(lineStart, lineEnd, HyperColor);
            if (ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), true)) {
                m_link_text_hover = true;
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    DynamicPrintConfig new_conf = obj_cfg;
                    new_conf.set_key_value("brim_type", new ConfigOptionEnum<BrimType>(btPainted));
                    mo->config.assign_config(new_conf);
                }
            }else {
                m_link_text_hover = false;
            }
        };

        if (obj_cfg.option("brim_type")) {
            if (obj_cfg.opt_enum<BrimType>("brim_type") != btPainted) {
                link_text();
            }
        }else {
            link_text();
        }

    }

    if (!m_single_brim.empty()) {
        wxString out = _L("Warning") + ": " + std::to_string(m_single_brim.size()) + _L(" invalid brim ears");
        m_imgui->warning_text(out);
    }

    GizmoImguiEnd();
    ImGui::PopStyleVar(2);
    ImGuiWrapper::pop_toolbar_style();
}

void GLGizmoBrimEars::show_tooltip_information(float x, float y)
{
    std::array<std::string, 4> info_array  = std::array<std::string, 4>{"left_click", "right_click", "ctrl_mouse_wheel", "alt_mouse_wheel"};
    float                      caption_max = 0.f;
    for (const auto &t : info_array) { caption_max = std::max(caption_max, m_imgui->calc_text_size(m_desc[t + "_caption"]).x); }

    ImTextureID normal_id = m_parent.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP);
    ImTextureID hover_id  = m_parent.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP_HOVER);

    caption_max += m_imgui->calc_text_size(": "sv).x + 35.f;

    float  scale       = m_parent.get_scale();
    #ifdef WIN32
        int dpi = get_dpi_for_window(wxGetApp().GetTopWindow());
        scale *= (float) dpi / (float) DPI_DEFAULT;
    #endif // WIN32
    ImVec2 button_size = ImVec2(25 * scale, 25 * scale); // ORCA: Use exact resolution will prevent blur on icon
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0, 0}); // ORCA: Dont add padding
    ImGui::ImageButton3(normal_id, hover_id, button_size);

    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip2(ImVec2(x, y));
        auto draw_text_with_caption = [this, &caption_max](const wxString &caption, const wxString &text) {
            m_imgui->text_colored(ImGuiWrapper::COL_ACTIVE, caption);
            ImGui::SameLine(caption_max);
            m_imgui->text_colored(ImGuiWrapper::COL_WINDOW_BG, text);
        };

        for (const auto &t : info_array) draw_text_with_caption(m_desc.at(t + "_caption") + ": ", m_desc.at(t));
        ImGui::EndTooltip();
    }
    ImGui::PopStyleVar(2);
}

bool GLGizmoBrimEars::on_is_activable() const
{
    const Selection &selection = m_parent.get_selection();

    if (!selection.is_single_full_instance()) return false;

    // Check that none of the selected volumes is outside. Only SLA auxiliaries (supports) are allowed outside.
    // const Selection::IndicesList &list = selection.get_volume_idxs();
    // for (const auto &idx : list)
    //     if (selection.get_volume(idx)->is_outside && selection.get_volume(idx)->composite_id.volume_id >= 0) return false;

    return true;
}

std::string GLGizmoBrimEars::on_get_name() const
{
    if (!on_is_activable() && m_state == EState::Off) {
        return _u8L("Brim Ears") + ":\n" + _u8L("Please select single object.");
    } else {
        return _u8L("Brim Ears");
    }
}

CommonGizmosDataID GLGizmoBrimEars::on_get_requirements() const
{
    return CommonGizmosDataID(int(CommonGizmosDataID::SelectionInfo) | int(CommonGizmosDataID::InstancesHider) | int(CommonGizmosDataID::Raycaster) |
                              int(CommonGizmosDataID::ObjectClipper));
}

void GLGizmoBrimEars::update_model_object()
{
    ModelObject* mo = m_c->selection_info()->model_object();
    if (mo) {
        mo->brim_points.clear();
        for (const CacheEntry& ce : m_editing_cache) mo->brim_points.emplace_back(ce.brim_point);
        wxGetApp().plater()->set_plater_dirty(true);
    }
    m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
}

// switch gizmos
void GLGizmoBrimEars::on_set_state()
{
    if (m_state == m_old_state) return;

    if (m_state == On && m_old_state != On) {
        // the gizmo was just turned on
        wxGetApp().plater()->enter_gizmos_stack();
        first_layer_slicer();
    }
    if (m_state == Off && m_old_state != Off) {
        // the gizmo was just turned Off
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Brim ears edit");
        update_model_object();
        wxGetApp().plater()->leave_gizmos_stack();
        // wxGetApp().mainframe->update_slice_print_status(MainFrame::SlicePrintEventType::eEventSliceUpdate, true, true);
    }
    m_old_state = m_state;
}

void GLGizmoBrimEars::on_start_dragging()
{
    if (m_hover_id != -1) {
        select_point(NoPoints);
        select_point(m_hover_id);
        m_point_before_drag = m_editing_cache[m_hover_id];
    } else
        m_point_before_drag = CacheEntry();
}

void GLGizmoBrimEars::on_stop_dragging()
{
    if (m_hover_id != -1) {
        CacheEntry backup = m_editing_cache[m_hover_id];

        if (m_point_before_drag.brim_point.pos != Vec3f::Zero()             // some point was touched
            && backup.brim_point.pos != m_point_before_drag.brim_point.pos) // and it was moved, not just selected
        {
            m_editing_cache[m_hover_id] = m_point_before_drag;
            Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Move support point");
            m_editing_cache[m_hover_id] = backup;
        }
    }
    m_point_before_drag = CacheEntry();
}

void GLGizmoBrimEars::on_load(cereal::BinaryInputArchive &ar) { ar(m_new_point_head_diameter, m_editing_cache, m_selection_empty); }

void GLGizmoBrimEars::on_save(cereal::BinaryOutputArchive &ar) const { ar(m_new_point_head_diameter, m_editing_cache, m_selection_empty); }

void GLGizmoBrimEars::select_point(int i)
{
    if (i == AllPoints || i == NoPoints) {
        for (auto &point_and_selection : m_editing_cache) point_and_selection.selected = (i == AllPoints);
        m_selection_empty = (i == NoPoints);

        if (i == AllPoints) m_new_point_head_diameter = m_editing_cache[0].brim_point.head_front_radius * 2.f;
    } else {
        m_editing_cache[i].selected = true;
        m_selection_empty           = false;
        m_new_point_head_diameter   = m_editing_cache[i].brim_point.head_front_radius * 2.f;
    }
}

void GLGizmoBrimEars::unselect_point(int i)
{
    m_editing_cache[i].selected = false;
    m_selection_empty           = true;
    for (const CacheEntry &ce : m_editing_cache) {
        if (ce.selected) {
            m_selection_empty = false;
            break;
        }
    }
}

void GLGizmoBrimEars::reload_cache()
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    m_editing_cache.clear();
    for (const BrimPoint &point : mo->brim_points) m_editing_cache.emplace_back(point);
    find_single();
}

Points GLGizmoBrimEars::generate_points(Polygon &obj_polygon, float ear_detection_length, float brim_ears_max_angle, bool is_outer)
{
    const coordf_t angle_threshold = (180 - brim_ears_max_angle) * PI / 180.0;
    Points         pt_ears;
    if (ear_detection_length > 0) {
        double detect_length = ear_detection_length / SCALING_FACTOR;
        Points points = obj_polygon.points;
        points.push_back(points.front());
        points = MultiPoint::_douglas_peucker(points, detect_length);
        if (points.size() > 4) {
            points.erase(points.end() - 1);
        }
        obj_polygon.points = points;
    }
    append(pt_ears, is_outer ? obj_polygon.convex_points(angle_threshold) : obj_polygon.concave_points(angle_threshold));
    return pt_ears;
}

void GLGizmoBrimEars::first_layer_slicer()
{
    const Selection              &selection = m_parent.get_selection();
    const Selection::IndicesList &idxs      = selection.get_volume_idxs();
    if (idxs.size() <= 0) return;
    std::vector<float>  slice_height(1, 0.1);
    MeshSlicingParamsEx params;
    params.mode           = MeshSlicingParams::SlicingMode::Regular;
    params.closing_radius = 0.1f;
    params.extra_offset   = 0.05f;
    params.resolution     = 0.01;
    ExPolygons part_ex;
    ExPolygons negative_ex;
    for (auto idx : idxs) {
        const GLVolume    *volume       = selection.get_volume(idx);
        const ModelVolume *model_volume = get_model_volume(*volume, wxGetApp().model());
        if (model_volume == nullptr) continue;
        if (model_volume->type() == ModelVolumeType::MODEL_PART || model_volume->type() == ModelVolumeType::NEGATIVE_VOLUME) {
            indexed_triangle_set volume_its = model_volume->mesh().its;
            if (volume_its.indices.size() <= 0) continue;
            Transform3d         trsf = volume->get_instance_transformation().get_matrix() * volume->get_volume_transformation().get_matrix();
            MeshSlicingParamsEx params_ex(params);
            params_ex.trafo = params_ex.trafo * trsf;
            if (params_ex.trafo.rotation().determinant() < 0.) its_flip_triangles(volume_its);
            ExPolygons sliced_layer = slice_mesh_ex(volume_its, slice_height, params_ex).front();
            if (model_volume->type() == ModelVolumeType::MODEL_PART) {
                part_ex = union_ex(part_ex, sliced_layer);
            } else {
                negative_ex = union_ex(negative_ex, sliced_layer);
            }
        }
    }
    m_first_layer = diff_ex(part_ex, negative_ex);
    get_detection_radius_max();
}

void GLGizmoBrimEars::auto_generate()
{
    const Selection &selection = m_parent.get_selection();
    const GLVolume  *volume    = selection.get_volume(*selection.get_volume_idxs().begin());
    Transform3d      trsf      = volume->get_instance_transformation().get_matrix();
    Vec3f            normal    = (volume->get_instance_transformation().get_matrix_no_offset().inverse() * m_world_normal).cast<float>();
    auto             add_point = [this, &trsf, &normal](const Point &p) {
        Vec3d world_pos  = {float(p.x() * SCALING_FACTOR), float(p.y() * SCALING_FACTOR), -0.0001};
        Vec3d object_pos = trsf.inverse() * world_pos;
        // m_editing_cache.emplace_back(BrimPoint(object_pos.cast<float>(), m_new_point_head_diameter / 2), false, normal);
        add_point_to_cache(object_pos.cast<float>(), m_new_point_head_diameter / 2, false, normal);
    };
    for (const ExPolygon &ex_poly : m_first_layer) {
        Polygon  out_poly   = ex_poly.contour;
        Polygons inner_poly = ex_poly.holes;
        polygons_reverse(inner_poly);
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Auto generate brim ear");
        Points               out_points = generate_points(out_poly, m_detection_radius, m_max_angle, true);
        for (Point &p : out_points) { add_point(p); }
        for (Polygon &pl : inner_poly) {
            Points inner_points = generate_points(pl, m_detection_radius, m_max_angle, false);
            for (Point &p : inner_points) { add_point(p); }
        }
    }
    find_single();
}

void GLGizmoBrimEars::get_detection_radius_max()
{
    double max_dist = 0.0;
    int    min_points_num = 0;
    for (const ExPolygon &ex_poly : m_first_layer) {
        Polygon  out_poly   = ex_poly.contour;
        Polygons inner_poly = ex_poly.holes;
        polygons_reverse(inner_poly);

        Points out_points = out_poly.points;
        out_points.push_back(out_points.front());
        double tolerance = 0.0;
        min_points_num   = MultiPoint::_douglas_peucker(out_points, 0).size();
        int repeat       = 0;
        int loop_protect = 0;
        for (;;) {
            loop_protect++;
            tolerance += 10;
            int num = MultiPoint::_douglas_peucker(out_points, tolerance / SCALING_FACTOR).size();
            if (num == min_points_num) {
                repeat++;
                if (repeat > 1)
                    break;
            }
            min_points_num = num;
            if (loop_protect > 100) break;
        }
        loop_protect = 0;
        for (;;) {
            loop_protect++;
            tolerance -= 1;
            int num = MultiPoint::_douglas_peucker(out_points, tolerance / SCALING_FACTOR).size();
            if (num <= min_points_num) {
                min_points_num = num;
            }else{
                break;
            }
            if (loop_protect > 100) break;
        }
        tolerance += 1;
        if (tolerance > max_dist)
            max_dist = tolerance;
    }
    if (max_dist > 100 || max_dist <= 0) {
        m_detection_radius_max = 100;
    } else {
        m_detection_radius_max = max_dist;
    }
}

bool GLGizmoBrimEars::add_point_to_cache(Vec3f pos, float head_radius, bool selected, Vec3f normal)
{
    BrimPoint point(pos, head_radius);
    for (int i = 0; i < m_editing_cache.size(); i++) {
        if (m_editing_cache[i].brim_point == point) { return false; }
    }
    m_editing_cache.emplace_back(point, selected, normal);
    update_model_object();
    return true;
}


void GLGizmoBrimEars::on_register_raycasters_for_picking() {
    update_raycasters();
}

void GLGizmoBrimEars::on_unregister_raycasters_for_picking()
{
    m_parent.remove_raycasters_for_picking(SceneRaycaster::EType::Gizmo);
    m_grabbers.clear();
}

void GLGizmoBrimEars::update_raycasters()
{
    // Remove extra raycasters
    if (m_editing_cache.size() < m_grabbers.size()) {
        for (auto it = m_grabbers.begin() + m_editing_cache.size(); it != m_grabbers.end(); ++it) {
            if (it->picking_id >= 0) {
                it->unregister_raycasters_for_picking();
            }
        }
        m_grabbers.erase(m_grabbers.begin() + m_editing_cache.size(), m_grabbers.end());
    } else if (m_editing_cache.size() > m_grabbers.size()) {
        auto remaining = m_editing_cache.size() - m_grabbers.size();
        while (remaining > 0) {
            const auto id   = m_grabbers.size();
            auto& g = m_grabbers.emplace_back();
            g.register_raycasters_for_picking(id);
            g.raycasters[0] = m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, id,
                                                                 *m_cylinder.mesh_raycaster, Transform3d::Identity());
            remaining--;
        }
    }
}

void GLGizmoBrimEars::register_single_mesh_pick()
{
    Selection                    &selection = m_parent.get_selection();
    const Selection::IndicesList &idxs      = selection.get_volume_idxs();
    if (idxs.size() > 0) {
        for (unsigned int idx : idxs) {
            GLVolume *v          = const_cast<GLVolume *>(selection.get_volume(idx));
            const ModelVolume* mv = get_model_volume(*v, wxGetApp().model());
            if (!mv->is_model_part()) continue;
            auto      world_tran = v->get_instance_transformation() * v->get_volume_transformation();
            if (m_mesh_raycaster_map.find(v) != m_mesh_raycaster_map.end()) {
                m_mesh_raycaster_map[v]->set_transform(world_tran.get_matrix());
            } else {
                auto mesh               = mv->mesh_ptr();
                m_mesh_raycaster_map[v] = std::make_shared<PickRaycaster>(-1, *v->mesh_raycaster, world_tran.get_matrix());
                m_mesh_raycaster_map[v]->set_transform(world_tran.get_matrix());
            }
        }
    }
}

//void GLGizmoBrimEars::update_single_mesh_pick(GLVolume *v)
//{
//    if (m_mesh_raycaster_map.find(v) != m_mesh_raycaster_map.end()) {
//        auto world_tran = v->get_instance_transformation() * v->get_volume_transformation();
//        m_mesh_raycaster_map[v]->world_tran.set_from_transform(world_tran.get_matrix());
//    }
//}

void GLGizmoBrimEars::reset_all_pick() { std::map<GLVolume *, std::shared_ptr<PickRaycaster>>().swap(m_mesh_raycaster_map); }

float GLGizmoBrimEars::get_brim_default_radius() const
{
    const double              nozzle_diameter = wxGetApp().preset_bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter")->get_at(0);
    const DynamicPrintConfig &pring_cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    return pring_cfg.get_abs_value("initial_layer_line_width", nozzle_diameter) * 16.0f;
}

ExPolygon GLGizmoBrimEars::make_polygon(BrimPoint point, const Geometry::Transformation &trsf)
{
    ExPolygon   point_round;
    Transform3d model_trsf = trsf.get_matrix();
    Vec3f       world_pos  = point.transform(trsf.get_matrix());
    coord_t     size_ear   = scale_(point.head_front_radius);
    for (size_t i = 0; i < POLY_SIDE_COUNT; i++) {
        double angle = (2.0 * PI * i) / POLY_SIDE_COUNT;
        point_round.contour.points.emplace_back(size_ear * cos(angle), size_ear * sin(angle));
    }
    Vec3f   pos  = point.transform(model_trsf);
    int32_t pt_x = scale_(pos.x());
    int32_t pt_y = scale_(pos.y());
    point_round.translate(Point(pt_x, pt_y));
    return point_round;
}

void GLGizmoBrimEars::find_single()
{
    update_raycasters();

    if (m_editing_cache.size() == 0) {
        m_single_brim.clear();
        return;
    }
    const Selection         &selection = m_parent.get_selection();
    const GLVolume          *volume    = selection.get_volume(*selection.get_volume_idxs().begin());
    Geometry::Transformation trsf      = volume->get_instance_transformation();
    ExPolygons               model_pl  = m_first_layer;

    m_single_brim.clear();
    for (int i = 0; i < m_editing_cache.size(); i++)
        m_single_brim[i] = m_editing_cache[i];
    unsigned int index = 0;
    bool cyc = true;
    while (cyc) {
        index++;
        if (index > 99999999) break; // cycle protection
        if (m_single_brim.empty()) {
            break;
        }
        auto end = --m_single_brim.end();
        for (auto it = m_single_brim.begin(); it != m_single_brim.end(); ++it) {
            ExPolygon point_pl = make_polygon(it->second.brim_point, trsf);
            if (overlaps(model_pl, point_pl)) {
                model_pl.emplace_back(point_pl);
                model_pl = union_ex(model_pl);
                it = m_single_brim.erase(it);
                break;
            } else {
                if (it == end) cyc = false;
            }
        }
    }
    for (auto& it : m_editing_cache) {
        it.is_error = false;
    }
    for (auto &it : m_single_brim) {
        m_editing_cache[it.first].is_error = true;
    }
}
}} // namespace Slic3r::GUI
