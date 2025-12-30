#include "GLGizmoCut.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"

#include <GL/glew.h>

#include <algorithm>

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Gizmos/GizmoObjectManipulation.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/Utils/UndoRedo.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"

#include "imgui/imgui_internal.h"
#include "slic3r/GUI/Field.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "FixModelByWin10.hpp"

namespace Slic3r {
namespace GUI {

static const ColorRGBA GRABBER_COLOR = ColorRGBA::YELLOW();
static const ColorRGBA UPPER_PART_COLOR = ColorRGBA::CYAN();
static const ColorRGBA LOWER_PART_COLOR = ColorRGBA::MAGENTA();
static const ColorRGBA MODIFIER_COLOR   = ColorRGBA(0.75f, 0.75f, 0.75f, 0.5f);

// connector colors
static const ColorRGBA PLAG_COLOR           = ColorRGBA::YELLOW();
static const ColorRGBA DOWEL_COLOR          = ColorRGBA::DARK_YELLOW();
static const ColorRGBA HOVERED_PLAG_COLOR   = ColorRGBA::CYAN();
static const ColorRGBA HOVERED_DOWEL_COLOR  = ColorRGBA(0.0f, 0.5f, 0.5f, 1.0f);
static const ColorRGBA SELECTED_PLAG_COLOR  = ColorRGBA::GRAY();
static const ColorRGBA SELECTED_DOWEL_COLOR = ColorRGBA::DARK_GRAY();
static const ColorRGBA CONNECTOR_DEF_COLOR  = ColorRGBA(1.0f, 1.0f, 1.0f, 0.5f);
static const ColorRGBA CONNECTOR_ERR_COLOR  = ColorRGBA(1.0f, 0.3f, 0.3f, 0.5f);
static const ColorRGBA HOVERED_ERR_COLOR    = ColorRGBA(1.0f, 0.3f, 0.3f, 1.0f);

static const ColorRGBA CUT_PLANE_DEF_COLOR  = ColorRGBA(0.9f, 0.9f, 0.9f, 0.5f);
static const ColorRGBA CUT_PLANE_ERR_COLOR  = ColorRGBA(1.0f, 0.8f, 0.8f, 0.5f);

const unsigned int AngleResolution = 64;
const unsigned int ScaleStepsCount = 72;
const float ScaleStepRad = 2.0f * float(PI) / ScaleStepsCount;
const unsigned int ScaleLongEvery = 2;
const float ScaleLongTooth = 0.1f; // in percent of radius
const unsigned int SnapRegionsCount = 8;

const float         UndefFloat = -999.f;
const std::string   UndefLabel = " ";

using namespace Geometry;

// Generates mesh for a line
static GLModel::Geometry its_make_line(Vec3f beg_pos, Vec3f end_pos)
{
    GLModel::Geometry init_data;
    init_data.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
    init_data.reserve_vertices(2);
    init_data.reserve_indices(2);

    // vertices
    init_data.add_vertex(beg_pos);
    init_data.add_vertex(end_pos);

    // indices
    init_data.add_line(0, 1);
    return init_data;
}

//! -- #ysFIXME those functions bodies are ported from GizmoRotation
// Generates mesh for a circle 
static void init_from_circle(GLModel& model, double radius)
{
    GLModel::Geometry init_data;
    init_data.format = { GLModel::Geometry::EPrimitiveType::LineLoop, GLModel::Geometry::EVertexLayout::P3 };
    init_data.reserve_vertices(ScaleStepsCount);
    init_data.reserve_indices(ScaleStepsCount);

    // vertices + indices
    for (unsigned int i = 0; i < ScaleStepsCount; ++i) {
        const float angle = float(i * ScaleStepRad);
        init_data.add_vertex(Vec3f(::cos(angle) * float(radius), ::sin(angle) * float(radius), 0.0f));
        init_data.add_index(i);
    }

    model.init_from(std::move(init_data));
    model.set_color(ColorRGBA::WHITE());
}

// Generates mesh for a scale
static void init_from_scale(GLModel& model, double radius)
{
    const float out_radius_long  = float(radius) * (1.0f + ScaleLongTooth);
    const float out_radius_short = float(radius) * (1.0f + 0.5f * ScaleLongTooth);

    GLModel::Geometry init_data;
    init_data.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
    init_data.reserve_vertices(2 * ScaleStepsCount);
    init_data.reserve_indices(2 * ScaleStepsCount);

    // vertices + indices
    for (unsigned int i = 0; i < ScaleStepsCount; ++i) {
        const float angle = float(i * ScaleStepRad);
        const float cosa = ::cos(angle);
        const float sina = ::sin(angle);
        const float in_x = cosa * float(radius);
        const float in_y = sina * float(radius);
        const float out_x = (i % ScaleLongEvery == 0) ? cosa * out_radius_long : cosa * out_radius_short;
        const float out_y = (i % ScaleLongEvery == 0) ? sina * out_radius_long : sina * out_radius_short;

        // vertices
        init_data.add_vertex(Vec3f(in_x, in_y, 0.0f));
        init_data.add_vertex(Vec3f(out_x, out_y, 0.0f));

        // indices
        init_data.add_line(i * 2, i * 2 + 1);
    }

    model.init_from(std::move(init_data));
    model.set_color(ColorRGBA::WHITE());
}

// Generates mesh for a snap_radii
static void init_from_snap_radii(GLModel& model, double radius)
{
    const float step = 2.0f * float(PI) / float(SnapRegionsCount);
    const float in_radius = float(radius) / 3.0f;
    const float out_radius = 2.0f * in_radius;

    GLModel::Geometry init_data;
    init_data.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
    init_data.reserve_vertices(2 * ScaleStepsCount);
    init_data.reserve_indices(2 * ScaleStepsCount);

    // vertices + indices
    for (unsigned int i = 0; i < ScaleStepsCount; ++i) {
        const float angle = float(i) * step;
        const float cosa = ::cos(angle);
        const float sina = ::sin(angle);
        const float in_x = cosa * in_radius;
        const float in_y = sina * in_radius;
        const float out_x = cosa * out_radius;
        const float out_y = sina * out_radius;

        // vertices
        init_data.add_vertex(Vec3f(in_x, in_y, 0.0f));
        init_data.add_vertex(Vec3f(out_x, out_y, 0.0f));

        // indices
        init_data.add_line(i * 2, i * 2 + 1);
    }

    model.init_from(std::move(init_data));
    model.set_color(ColorRGBA::WHITE());
}

// Generates mesh for a angle_arc
static void init_from_angle_arc(GLModel& model, double angle, double radius)
{
    model.reset();

    const float step_angle = float(angle) / float(AngleResolution);
    const float ex_radius = float(radius);

    GLModel::Geometry init_data;
    init_data.format = { GLModel::Geometry::EPrimitiveType::LineStrip, GLModel::Geometry::EVertexLayout::P3 };
    init_data.reserve_vertices(1 + AngleResolution);
    init_data.reserve_indices(1 + AngleResolution);

    // vertices + indices
    for (unsigned int i = 0; i <= AngleResolution; ++i) {
        const float angle = float(i) * step_angle;
        init_data.add_vertex(Vec3f(::cos(angle) * ex_radius, ::sin(angle) * ex_radius, 0.0f));
        init_data.add_index(i);
    }

    model.init_from(std::move(init_data));
}

//! --

GLGizmoCut3D::GLGizmoCut3D(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
    , m_connectors_group_id (GrabberID::Count)
    , m_connector_type (CutConnectorType::Plug)
    , m_connector_style (int(CutConnectorStyle::Prism))
    , m_connector_shape_id (int(CutConnectorShape::Circle))
{
    m_modes = { _u8L("Planar"), _u8L("Dovetail")//, _u8L("Grid")
//              , _u8L("Radial"), _u8L("Modular")
    };

    m_connector_modes = { _u8L("Auto"), _u8L("Manual") };

    m_connector_types = { _u8L("Plug"), _u8L("Dowel"), _u8L("Snap") };

    m_connector_styles = { _u8L("Prism"), _u8L("Frustum")
//              , _u8L("Claw")
    };

    m_connector_shapes = { _u8L("Triangle"), _u8L("Square"), _u8L("Hexagon"), _u8L("Circle")
//              , _u8L("D-shape")
    };

    m_axis_names = { "X", "Y", "Z" };

    m_part_orientation_names = {
        {"none",    _L("Keep orientation")},
        {"on_cut",  _L("Place on cut")},
        {"flip",    _L("Flip upside down")},
    };

    m_labels_map = {
        {"Connectors"   , _u8L("Connectors")},
        {"Type"         , _u8L("Type")},
        {"Style"        , _u8L("Style")},
        {"Shape"        , _u8L("Shape")},
        {"Depth"        , _u8L("Depth")},
        {"Size"         , _u8L("Size")},
        {"Rotation"     , _u8L("Rotation")},
        {"Groove"       , _u8L("Groove")},
        {"Width"        , _u8L("Width")},
        {"Flap Angle"   , _u8L("Flap Angle")},
        {"Groove Angle" , _u8L("Groove Angle")},
        {"Cut position" , _u8L("Cut position")}, // ORCA
        {"Build Volume" , _u8L("Build Volume")}, // ORCA
    };

//    update_connector_shape();
}

std::string GLGizmoCut3D::get_tooltip() const
{
    std::string tooltip;
    if (m_hover_id == Z || (m_dragging && m_hover_id == CutPlane)) {
        double koef = m_imperial_units ? GizmoObjectManipulation::mm_to_in : 1.0;
        std::string unit_str = " " + (m_imperial_units ? _u8L("in") : _u8L("mm"));
        const BoundingBoxf3& tbb = m_transformed_bounding_box;

        const std::string name = m_keep_as_parts ? _u8L("Part") : _u8L("Object");
        if (tbb.max.z() >= 0.0) {
            double top = (tbb.min.z() <= 0.0 ? tbb.max.z() : tbb.size().z()) * koef;
            tooltip += format(static_cast<float>(top), 2) + " " + unit_str + " (" + name + " A)";
            if (tbb.min.z() <= 0.0)
                tooltip += "\n";
        }
        if (tbb.min.z() <= 0.0) {
            double bottom = (tbb.max.z() <= 0.0 ? tbb.size().z() : (tbb.min.z() * (-1))) * koef;
            tooltip += format(static_cast<float>(bottom), 2) + " " + unit_str + " (" + name + " B)";
        }
        return tooltip;
    }

    if (!m_dragging && m_hover_id == CutPlane) {
        if (CutMode(m_mode) == CutMode::cutTongueAndGroove)
            return _u8L("Click to flip the cut plane\n"
                        "Drag to move the cut plane");
        return _u8L("Click to flip the cut plane\n"
                    "Drag to move the cut plane\n"
                    "Right-click a part to assign it to the other side");
    }

    if (tooltip.empty() && (m_hover_id == X || m_hover_id == Y || m_hover_id == CutPlaneZRotation)) {
        std::string axis = m_hover_id == X ? "X" : m_hover_id == Y ? "Y" : "Z";
        return axis + ": " + format(float(rad2deg(m_angle)), 1) + "°";
    }

    return tooltip;
}

bool GLGizmoCut3D::on_mouse(const wxMouseEvent &mouse_event)
{
    Vec2i32 mouse_coord(mouse_event.GetX(), mouse_event.GetY());
    Vec2d mouse_pos = mouse_coord.cast<double>();

    if (mouse_event.ShiftDown() && mouse_event.LeftDown())
        return gizmo_event(SLAGizmoEventType::LeftDown, mouse_pos, mouse_event.ShiftDown(), mouse_event.AltDown(), mouse_event.CmdDown());
    if (mouse_event.CmdDown() && mouse_event.LeftDown())
        return false;
    if (cut_line_processing()) {
        if (mouse_event.ShiftDown()) {
            if (mouse_event.Moving()|| mouse_event.Dragging())
                return gizmo_event(SLAGizmoEventType::Moving, mouse_pos, mouse_event.ShiftDown(), mouse_event.AltDown(), mouse_event.CmdDown());
            if (mouse_event.LeftUp())
                return gizmo_event(SLAGizmoEventType::LeftUp, mouse_pos, mouse_event.ShiftDown(), mouse_event.AltDown(), mouse_event.CmdDown());
        }
        discard_cut_line_processing();
    }
    else if (mouse_event.Moving())
        return false;

    if (m_hover_id >= CutPlane && mouse_event.LeftDown() && !m_connectors_editing) {
        // before processing of a use_grabbers(), detect start move position as a projection of mouse position to the cut plane
        Vec3d pos;
        Vec3d pos_world;
        if (unproject_on_cut_plane(mouse_pos, pos, pos_world, false))
            m_cut_plane_start_move_pos = pos_world;
    }

    if (use_grabbers(mouse_event)) {
        if (m_hover_id >= m_connectors_group_id) {
            if (mouse_event.LeftDown() && !mouse_event.CmdDown() && !mouse_event.AltDown())
                unselect_all_connectors();
            if (mouse_event.LeftUp() && !mouse_event.ShiftDown())
                gizmo_event(SLAGizmoEventType::LeftUp, mouse_pos, mouse_event.ShiftDown(), mouse_event.AltDown(), mouse_event.CmdDown());
        }
        else if (m_hover_id == CutPlane) {
            if (mouse_event.LeftDown()) {
                m_was_cut_plane_dragged = m_was_contour_selected = false;

                // disable / enable current contour
                Vec3d pos;
                Vec3d pos_world;
                m_was_contour_selected = unproject_on_cut_plane(mouse_pos.cast<double>(), pos, pos_world);
                if (m_was_contour_selected) {
                    // Following would inform the clipper about the mouse click, so it can
                    // toggle the respective contour as disabled.
                    //m_c->object_clipper()->pass_mouse_click(pos_world);
                    //process_contours();
                    return true;
                }

            }
            else if (mouse_event.LeftUp() && !m_was_cut_plane_dragged && !m_was_contour_selected)
                flip_cut_plane();
        }

        if (m_hover_id >= CutPlane && mouse_event.Dragging() && !m_connectors_editing) {
            // if we continue to dragging a cut plane, than update a start move position as a projection of mouse position to the cut plane after processing of a use_grabbers()
            Vec3d pos;
            Vec3d pos_world;
            if (unproject_on_cut_plane(mouse_pos, pos, pos_world, false))
                m_cut_plane_start_move_pos = pos_world;
        }

        toggle_model_objects_visibility();
        return true;
    }

    static bool pending_right_up = false;
    if (mouse_event.LeftDown()) {
        bool grabber_contains_mouse = (get_hover_id() != -1);
        const bool shift_down = mouse_event.ShiftDown();
        if ((!shift_down || grabber_contains_mouse) &&
            gizmo_event(SLAGizmoEventType::LeftDown, mouse_pos, mouse_event.ShiftDown(), mouse_event.AltDown(), false))
            return true;
    }
    else if (mouse_event.Dragging()) {
        bool control_down = mouse_event.CmdDown();
        if (m_parent.get_move_volume_id() != -1) {
            // don't allow dragging objects with the Sla gizmo on
            return true;
        }
        if (!control_down &&
            gizmo_event(SLAGizmoEventType::Dragging, mouse_pos, mouse_event.ShiftDown(), mouse_event.AltDown(), false)) {
            // the gizmo got the event and took some action, no need to do
            // anything more here
            m_parent.set_as_dirty();
            return true;
        }
        if (control_down && (mouse_event.LeftIsDown() || mouse_event.RightIsDown())) {
            // CTRL has been pressed while already dragging -> stop current action
            if (mouse_event.LeftIsDown())
                gizmo_event(SLAGizmoEventType::LeftUp, mouse_pos, mouse_event.ShiftDown(), mouse_event.AltDown(), true);
            else if (mouse_event.RightIsDown())
                pending_right_up = false;
        }
    }
    else if (mouse_event.LeftUp() && !m_parent.is_mouse_dragging()) {
        // in case SLA/FDM gizmo is selected, we just pass the LeftUp event
        // and stop processing - neither object moving or selecting is
        // suppressed in that case
        gizmo_event(SLAGizmoEventType::LeftUp, mouse_pos, mouse_event.ShiftDown(), mouse_event.AltDown(), mouse_event.CmdDown());
        return true;
    }
    else if (mouse_event.RightDown()) {
        if (! m_connectors_editing && mouse_event.GetModifiers() == wxMOD_NONE &&
            CutMode(m_mode) == CutMode::cutPlanar) {
            // Check the internal part raycasters.
            if (! m_part_selection.valid())
                process_contours();
            m_part_selection.toggle_selection(mouse_pos);
            check_and_update_connectors_state(); // after a contour is deactivated, its connectors are inside the object
            return true;
        }

        if (m_parent.get_selection().get_object_idx() != -1 &&
            gizmo_event(SLAGizmoEventType::RightDown, mouse_pos, false, false, false)) {
            // we need to set the following right up as processed to avoid showing
            // the context menu if the user release the mouse over the object
            pending_right_up = true;
            // event was taken care of by the SlaSupports gizmo
            return true;
        }
    }
    else if (pending_right_up && mouse_event.RightUp()) {
        pending_right_up = false;
        return true;
    }
    return false;
}

void GLGizmoCut3D::shift_cut(double delta)
{
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Move cut plane"), UndoRedo::SnapshotType::GizmoAction);
    set_center(m_plane_center + m_cut_normal * delta, true);
    m_ar_plane_center = m_plane_center;
}

void GLGizmoCut3D::rotate_vec3d_around_plane_center(Vec3d&vec)
{
    vec = Transformation(translation_transform(m_plane_center) * m_rotation_m * translation_transform(-m_plane_center)).get_matrix() * vec;
}

void GLGizmoCut3D::put_connectors_on_cut_plane(const Vec3d& cp_normal, double cp_offset)
{
    ModelObject* mo = m_c->selection_info()->model_object();
    if (CutConnectors& connectors = mo->cut_connectors; !connectors.empty()) {
        const float sla_shift        = m_c->selection_info()->get_sla_shift();
        const Vec3d& instance_offset = mo->instances[m_c->selection_info()->get_active_instance()]->get_offset();

        for (auto& connector : connectors) {
            // convert connetor pos to the world coordinates
            Vec3d pos = connector.pos + instance_offset;
            pos[Z] += sla_shift;
            // scalar distance from point to plane along the normal
            double distance = -cp_normal.dot(pos) + cp_offset;
            // move connector
            connector.pos += distance * cp_normal;
        }
    }
}

// returns true if the camera (forward) is pointing in the negative direction of the cut normal
bool GLGizmoCut3D::is_looking_forward() const
{
    const Camera& camera = wxGetApp().plater()->get_camera();
    const double dot = camera.get_dir_forward().dot(m_cut_normal);
    return dot < 0.05;
}

void GLGizmoCut3D::update_clipper()
{
    // update cut_normal
    Vec3d normal = m_rotation_m * Vec3d::UnitZ();
    normal.normalize();
    m_cut_normal = normal;

    // calculate normal and offset for clipping plane
    Vec3d beg = m_bb_center;
    beg[Z] -= m_radius;
    rotate_vec3d_around_plane_center(beg);

    m_clp_normal  = normal;
    double offset = normal.dot(m_plane_center);
    double dist   = normal.dot(beg);

    m_parent.set_color_clip_plane(normal, offset);

    if (!is_looking_forward()) {
        // recalculate normal and offset for clipping plane, if camera is looking downward to cut plane
        normal = m_rotation_m * (-1. * Vec3d::UnitZ());
        normal.normalize();

        beg = m_bb_center;
        beg[Z] += m_radius;
        rotate_vec3d_around_plane_center(beg);

        m_clp_normal = normal;
        offset       = normal.dot(m_plane_center);
        dist         = normal.dot(beg);
    }

    m_c->object_clipper()->set_range_and_pos(normal, offset, dist);

    put_connectors_on_cut_plane(normal, offset);

    if (m_raycasters.empty())
        on_register_raycasters_for_picking();
    else
        update_raycasters_for_picking_transform();
}

void GLGizmoCut3D::set_center(const Vec3d& center, bool update_tbb /*=false*/)
{
    set_center_pos(center, update_tbb);
    check_and_update_connectors_state();
    update_clipper();
}

void GLGizmoCut3D::switch_to_mode(size_t new_mode)
{
    m_mode = new_mode;
    update_raycasters_for_picking();

    apply_color_clip_plane_colors();
    if (auto oc = m_c->object_clipper()) {
        m_contour_width = CutMode(m_mode) == CutMode::cutTongueAndGroove ? 0.f : 0.4f;
        oc->set_behavior(m_connectors_editing, m_connectors_editing, double(m_contour_width));
    }

    update_plane_model();
    reset_cut_by_contours();
}

bool GLGizmoCut3D::render_cut_mode_combo()
{
    ImGui::AlignTextToFramePadding();
    ImGuiWrapper::push_combo_style(m_parent.get_scale());
    int selection_idx = int(m_mode);
    const bool is_changed = m_imgui->combo(_u8L("Mode"), m_modes, selection_idx, 0, m_label_width, m_control_width);
    ImGuiWrapper::pop_combo_style();

    if (is_changed) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Change cut mode"), UndoRedo::SnapshotType::GizmoAction);
        switch_to_mode(size_t(selection_idx));
        check_and_update_connectors_state();
    }

    return is_changed;
}

bool GLGizmoCut3D::render_double_input(const std::string& label, double& value_in)
{
    ImGui::AlignTextToFramePadding();
    m_imgui->text(label);
    ImGui::SameLine(m_label_width);
    ImGui::PushItemWidth(m_control_width);

    double value = value_in;
    if (m_imperial_units)
        value *= GizmoObjectManipulation::mm_to_in;
    double old_val = value;
    ImGui::InputDouble(("##" + label).c_str(), &value, 0.0f, 0.0f, "%.2f", ImGuiInputTextFlags_CharsDecimal);

    ImGui::SameLine();
    m_imgui->text(m_imperial_units ? _L("in") : _L("mm"));

    value_in = value * (m_imperial_units ? GizmoObjectManipulation::in_to_mm : 1.0);
    return !is_approx(old_val, value);
}

bool GLGizmoCut3D::render_slider_double_input(const std::string& label, float& value_in, float& tolerance_in, float min_val/* = -0.1f*/, float max_tolerance/* = -0.1f*/)
{
    // -------- [ ] -------- [ ]
    // slider_with + item_in_gap + first_input_width + item_out_gap + slider_with + item_in_gap + second_input_width
    double slider_with        = 0.24 * m_editing_window_width; // m_control_width * 0.35;
    double item_in_gap        = 0.01 * m_editing_window_width;
    double item_out_gap       = 0.01 * m_editing_window_width;
    double first_input_width  = 0.29 * m_editing_window_width;
    double second_input_width = 0.29 * m_editing_window_width;

    constexpr float UndefMinVal = -0.1f;
    const float f_mm_to_in = static_cast<float>(GizmoObjectManipulation::mm_to_in);

    ImGui::AlignTextToFramePadding();
    m_imgui->text(label);
    ImGui::SameLine(m_label_width);
    ImGui::PushItemWidth(slider_with);

    double left_width = m_label_width + slider_with + item_in_gap;

    bool m_imperial_units = false;

    float value = value_in;
    if (m_imperial_units)
        value *= f_mm_to_in;
    float old_val = value;

    const BoundingBoxf3 bbox = m_bounding_box;
    const float mean_size = float((bbox.size().x() + bbox.size().y() + bbox.size().z()) / 9.0) * (m_imperial_units ? f_mm_to_in : 1.f);
    const float min_v = min_val > 0.f ? /*std::min(max_val, mean_size)*/min_val : 1.f;

    float min_size = value_in < 0.f ? UndefMinVal : min_v;
    if (m_imperial_units) {
        min_size *= f_mm_to_in;
    }
    std::string format = value_in < 0.f ? " " : m_imperial_units ? "%.4f  " + _u8L("in") : "%.2f  " + _u8L("mm");

    m_imgui->bbl_slider_float_style(("##" + label).c_str(), &value, min_size, mean_size, format.c_str());

    ImGui::SameLine(left_width);
    ImGui::PushItemWidth(first_input_width);
    ImGui::BBLDragFloat(("##input_" + label).c_str(), &value, 0.05f, min_size, mean_size, format.c_str());

    value_in = value * float(m_imperial_units ? GizmoObjectManipulation::in_to_mm : 1.0);

    left_width += (first_input_width + item_out_gap);
    ImGui::SameLine(left_width);
    ImGui::PushItemWidth(slider_with);

    float tolerance = tolerance_in;
    if (m_imperial_units)
        tolerance *= f_mm_to_in;
    float old_tolerance = tolerance;
    // std::string format_t      = tolerance_in < 0.f ? " " : "%.f %%";
    float min_tolerance = tolerance_in < 0.f ? UndefMinVal : 0.f;
    const float max_tolerance_v = max_tolerance > 0.f ? std::min(max_tolerance, 0.5f * mean_size) : 0.5f * mean_size;

    m_imgui->bbl_slider_float_style("##tolerance_" + label, &tolerance, min_tolerance, max_tolerance_v, format.c_str(), 1.f, true,
                                    _L("Tolerance"));

    left_width += (slider_with + item_in_gap);
    ImGui::SameLine(left_width);
    ImGui::PushItemWidth(second_input_width);
    ImGui::BBLDragFloat(("##tolerance_input_" + label).c_str(), &tolerance, 0.05f, min_tolerance, max_tolerance_v, format.c_str());

    tolerance_in = tolerance * float(m_imperial_units ? GizmoObjectManipulation::in_to_mm : 1.0);

    return !is_approx(old_val, value) || !is_approx(old_tolerance, tolerance);
}

void GLGizmoCut3D::render_move_center_input(int axis)
{
    m_imgui->text(m_axis_names[axis]+":");
    ImGui::SameLine();
    ImGui::PushItemWidth(0.3f*m_control_width);

    Vec3d move = m_plane_center;
    double in_val, value = in_val = move[axis];
    if (m_imperial_units)
        value *= GizmoObjectManipulation::mm_to_in;
    ImGui::InputDouble(("##move_" + m_axis_names[axis]).c_str(), &value, 0.0, 0.0, "%.2f", ImGuiInputTextFlags_CharsDecimal);
    ImGui::SameLine();

    double val = value * (m_imperial_units ? GizmoObjectManipulation::in_to_mm : 1.0);

    if (in_val != val) {
        move[axis] = val;
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Move cut plane"), UndoRedo::SnapshotType::GizmoAction);
        set_center(move, true);
        m_ar_plane_center = m_plane_center;

        reset_cut_by_contours();
    }
}

bool GLGizmoCut3D::render_connect_type_radio_button(CutConnectorType type)
{
    ImGui::SameLine(type == CutConnectorType::Plug ? m_label_width : 0);
    ImGui::PushItemWidth(m_control_width);
    if (ImGui::RadioButton(m_connector_types[size_t(type)].c_str(), m_connector_type == type)) {
        m_connector_type = type;
//        update_connector_shape();
        return true;
    }
    return false;
}

void GLGizmoCut3D::render_connect_mode_radio_button(CutConnectorMode mode)
{
    ImGui::SameLine(mode == CutConnectorMode::Auto ? m_label_width : 2 * m_label_width);
    ImGui::PushItemWidth(m_control_width);
    if (ImGui::RadioButton(m_connector_modes[int(mode)].c_str(), m_connector_mode == mode))
        m_connector_mode = mode;
}

bool GLGizmoCut3D::render_reset_button(const std::string& label_id, const std::string& tooltip) const
{
    const ImGuiStyle &style = ImGui::GetStyle();

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {1, style.ItemSpacing.y});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);       // ORCA match button style

    ImGui::PushStyleColor(ImGuiCol_Button, {0.25f, 0.25f, 0.25f, 0.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0, 0, 0, 0}); // ORCA match button style
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  {0, 0, 0, 0}); // ORCA match button style

    const bool revert = m_imgui->button(wxString(ImGui::RevertBtn) + "##" + wxString::FromUTF8(label_id));

    ImGui::PopStyleColor(3);

    if (ImGui::IsItemHovered())
        m_imgui->tooltip(tooltip.c_str(), ImGui::GetFontSize() * 20.0f);

    ImGui::PopStyleVar(2); // ORCA

    return revert;
}

static double get_grabber_mean_size(const BoundingBoxf3& bb)
{
#if ENABLE_FIXED_GRABBER
    // Orca: make grabber larger
    return 32. * GLGizmoBase::INV_ZOOM;
#else
    return (bb.size().x() + bb.size().y() + bb.size().z()) / 30.;
#endif
}

indexed_triangle_set GLGizmoCut3D::its_make_groove_plane()
{
    // values for calculation

    const float  side_width     = is_approx(m_groove.flaps_angle, 0.f) ? m_groove.depth : (m_groove.depth / sin(m_groove.flaps_angle));
    const float  flaps_width    = 2.f * side_width * cos(m_groove.flaps_angle);

    const float groove_half_width_upper = 0.5f * (m_groove.width);
    const float groove_half_width_lower = 0.5f * (m_groove.width + flaps_width);

    const float cut_plane_radius = 1.5f * float(m_radius);
    const float cut_plane_length = 1.5f * cut_plane_radius;

    const float groove_half_depth = 0.5f * m_groove.depth;

    const float x       = 0.5f * cut_plane_radius;
    const float y       = 0.5f * cut_plane_length;
    float       z_upper = groove_half_depth;
    float       z_lower = -groove_half_depth;

    const float proj = y * tan(m_groove.angle);

    float ext_upper_x = groove_half_width_upper + proj; // upper_x extension
    float ext_lower_x = groove_half_width_lower + proj; // lower_x extension

    float nar_upper_x = groove_half_width_upper - proj; // upper_x narrowing
    float nar_lower_x = groove_half_width_lower - proj; // lower_x narrowing

    const float cut_plane_thiknes = 0.02f;// 0.02f * (float)get_grabber_mean_size(m_bounding_box);   // cut_plane_thiknes

    // Vertices of the groove used to detection if groove is valid
    // They are written as:
    // {left_ext_lower, left_nar_lower, left_ext_upper, left_nar_upper,
    //  right_ext_lower, right_nar_lower, right_ext_upper, right_nar_upper }
    {
        m_groove_vertices.clear();
        m_groove_vertices.reserve(8);

        m_groove_vertices.emplace_back(Vec3f(-ext_lower_x, -y, z_lower).cast<double>());
        m_groove_vertices.emplace_back(Vec3f(-nar_lower_x,  y, z_lower).cast<double>());
        m_groove_vertices.emplace_back(Vec3f(-ext_upper_x, -y, z_upper).cast<double>());
        m_groove_vertices.emplace_back(Vec3f(-nar_upper_x,  y, z_upper).cast<double>());
        m_groove_vertices.emplace_back(Vec3f( ext_lower_x, -y, z_lower).cast<double>());
        m_groove_vertices.emplace_back(Vec3f( nar_lower_x,  y, z_lower).cast<double>());
        m_groove_vertices.emplace_back(Vec3f( ext_upper_x, -y, z_upper).cast<double>());
        m_groove_vertices.emplace_back(Vec3f( nar_upper_x,  y, z_upper).cast<double>());
    }

    // Different cases of groove plane:

    // groove is open

    if (groove_half_width_upper > proj && groove_half_width_lower > proj) {
        indexed_triangle_set mesh;

        auto get_vertices = [x, y](float z_upper, float z_lower, float nar_upper_x, float nar_lower_x, float ext_upper_x, float ext_lower_x) {
            return std::vector<stl_vertex>({
                // upper left part vertices
                {-x, -y, z_upper}, {-x, y, z_upper}, {-nar_upper_x, y, z_upper}, {-ext_upper_x, -y, z_upper},
                // lower part vertices
                {-ext_lower_x, -y, z_lower}, {-nar_lower_x, y, z_lower}, {nar_lower_x, y, z_lower}, {ext_lower_x, -y, z_lower},
                // upper right part vertices
                {ext_upper_x, -y, z_upper}, {nar_upper_x, y, z_upper}, {x, y, z_upper}, {x, -y, z_upper}
                });
        };

        mesh.vertices = get_vertices(z_upper, z_lower, nar_upper_x, nar_lower_x, ext_upper_x, ext_lower_x);
        mesh.vertices.reserve(2 * mesh.vertices.size());

        z_upper -= cut_plane_thiknes;
        z_lower -= cut_plane_thiknes;

        const float under_x_shift = cut_plane_thiknes / tan(0.5f * m_groove.flaps_angle);

        nar_upper_x += under_x_shift;
        nar_lower_x += under_x_shift;
        ext_upper_x += under_x_shift;
        ext_lower_x += under_x_shift;

        std::vector<stl_vertex> vertices = get_vertices(z_upper, z_lower, nar_upper_x, nar_lower_x, ext_upper_x, ext_lower_x);
        mesh.vertices.insert(mesh.vertices.end(), vertices.begin(), vertices.end());

        mesh.indices = {
            // above view
            {5,4,7}, {5,7,6},       // lower part
            {3,4,5}, {3,5,2},       // left side
            {9,6,8}, {8,6,7},       // right side
            {1,0,2}, {2,0,3},       // upper left part
            {9,8,10}, {10,8,11},    // upper right part
            // under view
            {20,21,22}, {20,22,23}, // upper right part
            {12,13,14}, {12,14,15}, // upper left part
            {18,21,20}, {18,20,19}, // right side
            {16,15,14}, {16,14,17}, // left side
            {16,17,18}, {16,18,19}, // lower part  
            // left edge
            {1,13,0}, {0,13,12},
            // front edge
            {0,12,3}, {3,12,15}, {3,15,4}, {4,15,16}, {4,16,7}, {7,16,19}, {7,19,20}, {7,20,8}, {8,20,11}, {11,20,23},
            // right edge
            {11,23,10}, {10,23,22},
            // back edge
            {1,13,2}, {2,13,14}, {2,14,17}, {2,17,5}, {5,17,6}, {6,17,18}, {6,18,9}, {9,18,21}, {9,21,10}, {10,21,22}
        };
        return mesh;
    }

    float cross_pt_upper_y = groove_half_width_upper / tan(m_groove.angle);

    // groove is closed

    if (groove_half_width_upper < proj && groove_half_width_lower < proj) {
        float cross_pt_lower_y = groove_half_width_lower / tan(m_groove.angle);

        indexed_triangle_set mesh;

        auto get_vertices = [x, y](float z_upper, float z_lower, float cross_pt_upper_y, float cross_pt_lower_y, float ext_upper_x, float ext_lower_x) {
            return std::vector<stl_vertex>({
                // upper part vertices
                {-x, -y, z_upper}, {-x, y, z_upper}, {x, y, z_upper}, {x, -y, z_upper},
                {ext_upper_x, -y, z_upper}, {0.f, cross_pt_upper_y, z_upper}, {-ext_upper_x, -y, z_upper},
                // lower part vertices
                {-ext_lower_x, -y, z_lower}, {0.f, cross_pt_lower_y, z_lower}, {ext_lower_x, -y, z_lower}
                });
        };

        mesh.vertices = get_vertices(z_upper, z_lower, cross_pt_upper_y, cross_pt_lower_y, ext_upper_x, ext_lower_x);
        mesh.vertices.reserve(2 * mesh.vertices.size());

        z_upper -= cut_plane_thiknes;
        z_lower -= cut_plane_thiknes;

        const float under_x_shift = cut_plane_thiknes / tan(0.5f * m_groove.flaps_angle);

        cross_pt_upper_y += cut_plane_thiknes;
        cross_pt_lower_y += cut_plane_thiknes;
        ext_upper_x += under_x_shift;
        ext_lower_x += under_x_shift;

        std::vector<stl_vertex> vertices = get_vertices(z_upper, z_lower, cross_pt_upper_y, cross_pt_lower_y, ext_upper_x, ext_lower_x);
        mesh.vertices.insert(mesh.vertices.end(), vertices.begin(), vertices.end());

        mesh.indices = {
            // above view
            {8,7,9},                    // lower part
            {5,8,6}, {6,8,7},       // left side
            {4,9,8}, {4,8,5},       // right side
            {1,0,6}, {1,6,5},{1,5,2}, {2,5,4}, {2,4,3},   // upper part
            // under view
            {10,11,16}, {16,11,15}, {15,11,12}, {15,12,14}, {14,12,13},   // upper part
            {18,15,14}, {14,18,19}, // right side
            {17,16,15}, {17,15,18}, // left side
            {17,18,19},                 // lower part  
            // left edge
            {1,11,0}, {0,11,10},
            // front edge
            {0,10,6}, {6,10,16}, {6,17,16}, {6,7,17}, {7,17,19}, {7,19,9}, {4,14,19}, {4,19,9}, {4,14,13}, {4,13,3},
            // right edge
            {3,13,12}, {3,12,2},
            // back edge
            {2,12,11}, {2,11,1}
        };

        return mesh;
    }

    // groove is closed from the roof

    indexed_triangle_set mesh;
    mesh.vertices = {
        // upper part vertices
        {-x, -y, z_upper}, {-x, y, z_upper}, {x, y, z_upper}, {x, -y, z_upper},
        {ext_upper_x, -y, z_upper}, {0.f, cross_pt_upper_y, z_upper}, {-ext_upper_x, -y, z_upper},
        // lower part vertices
        {-ext_lower_x, -y, z_lower}, {-nar_lower_x, y, z_lower}, {nar_lower_x, y, z_lower}, {ext_lower_x, -y, z_lower}
    };

    mesh.vertices.reserve(2 * mesh.vertices.size() + 1);

    z_upper -= cut_plane_thiknes;
    z_lower -= cut_plane_thiknes;

    const float under_x_shift = cut_plane_thiknes / tan(0.5f * m_groove.flaps_angle);

    nar_lower_x += under_x_shift;
    ext_upper_x += under_x_shift;
    ext_lower_x += under_x_shift;

    std::vector<stl_vertex> vertices = {
        // upper part vertices
        {-x, -y, z_upper}, {-x, y, z_upper}, {x, y, z_upper}, {x, -y, z_upper},
        {ext_upper_x, -y, z_upper}, {under_x_shift, cross_pt_upper_y, z_upper}, {-under_x_shift, cross_pt_upper_y, z_upper}, {-ext_upper_x, -y, z_upper},
        // lower part vertices
        {-ext_lower_x, -y, z_lower}, {-nar_lower_x, y, z_lower}, {nar_lower_x, y, z_lower}, {ext_lower_x, -y, z_lower}
    };
    mesh.vertices.insert(mesh.vertices.end(), vertices.begin(), vertices.end());

    mesh.indices = {
        // above view
        {8,7,10}, {8,10,9},     // lower part
        {5,8,7}, {5,7,6},       // left side
        {4,10,9}, {4,9,5},      // right side
        {1,0,6}, {1,6,5},{1,5,2}, {2,5,4}, {2,4,3},   // upper part
        // under view
        {11,12,18}, {18,12,17}, {17,12,16}, {16,12,13}, {16,13,15}, {15,13,14},   // upper part
        {21,16,15}, {21,15,22}, // right side
        {19,18,17}, {19,17,20}, // left side
        {19,20,21}, {19,21,22}, // lower part  
        // left edge
        {1,12,11}, {1,11,0},
        // front edge
        {0,11,18}, {0,18,6}, {7,19,18}, {7,18,6}, {7,19,22}, {7,22,10}, {10,22,15}, {10,15,4}, {4,15,14}, {4,14,3},
        // right edge
        {3,14,13}, {3,14,2},
        // back edge
        {2,13,12}, {2,12,1}, {5,16,21}, {5,21,9}, {9,21,20}, {9,20,8}, {5,17,20}, {5,20,8}
    };

    return mesh;
}

void GLGizmoCut3D::render_cut_plane()
{
    if (cut_line_processing())
        return;

    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

    shader->start_using();

    const Camera& camera = wxGetApp().plater()->get_camera();

    shader->set_uniform("projection_matrix", camera.get_projection_matrix());

    ColorRGBA cp_clr = can_perform_cut() && has_valid_groove() ? CUT_PLANE_DEF_COLOR : CUT_PLANE_ERR_COLOR;
    if (m_mode == size_t(CutMode::cutTongueAndGroove))
        cp_clr.a(cp_clr.a() - 0.1f);
    m_plane.model.set_color(cp_clr);

    const Transform3d view_model_matrix = camera.get_view_matrix() * translation_transform(m_plane_center) * m_rotation_m;
    shader->set_uniform("view_model_matrix", view_model_matrix);
    m_plane.model.render();

    glsafe(::glEnable(GL_CULL_FACE));
    glsafe(::glDisable(GL_BLEND));

    shader->stop_using();
}

static double get_half_size(double size)
{
    return std::max(size * 0.35, 0.05);
}

static double get_dragging_half_size(double size)
{
    return get_half_size(size) * 1.25;
}

void GLGizmoCut3D::render_model(GLModel& model, const ColorRGBA& color, Transform3d view_model_matrix)
{
    GLShaderProgram* shader = wxGetApp().get_shader("gouraud_light");
    if (shader) {
        shader->start_using();

        shader->set_uniform("view_model_matrix", view_model_matrix);
        shader->set_uniform("emission_factor", 0.2f);
        shader->set_uniform("projection_matrix", wxGetApp().plater()->get_camera().get_projection_matrix());

        model.set_color(color);
        model.render();

        shader->stop_using();
    }
}

void GLGizmoCut3D::render_line(GLModel& line_model, const ColorRGBA& color, Transform3d view_model_matrix, float width)
{
    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader) {
        shader->start_using();

        shader->set_uniform("view_model_matrix", view_model_matrix);
        shader->set_uniform("projection_matrix", wxGetApp().plater()->get_camera().get_projection_matrix());
        shader->set_uniform("width", width);

        line_model.set_color(color);
        line_model.render();

        shader->stop_using();
    }
}

void GLGizmoCut3D::render_rotation_snapping(GrabberID axis, const ColorRGBA& color)
{
    GLShaderProgram* line_shader = wxGetApp().get_shader("flat");
    if (!line_shader)
        return;

    const Camera& camera = wxGetApp().plater()->get_camera();
    Transform3d view_model_matrix = camera.get_view_matrix() * translation_transform(m_plane_center) * m_start_dragging_m;

    if (axis == X)
        view_model_matrix = view_model_matrix * rotation_transform(0.5 * PI * Vec3d::UnitY()) * rotation_transform(-PI * Vec3d::UnitZ());
    else if (axis == Y)
        view_model_matrix = view_model_matrix * rotation_transform(-0.5 * PI * Vec3d::UnitZ()) * rotation_transform(-0.5 * PI * Vec3d::UnitY());
    else
        view_model_matrix = view_model_matrix * rotation_transform(-0.5 * PI * Vec3d::UnitZ());

    line_shader->start_using();
    line_shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    line_shader->set_uniform("view_model_matrix", view_model_matrix);
    line_shader->set_uniform("width", 0.25f);

    m_circle.render();
    m_scale.render();
    m_snap_radii.render();
    m_reference_radius.render();
    if (m_dragging) {
        line_shader->set_uniform("width", 1.5f);
        m_angle_arc.set_color(color);
        m_angle_arc.render();
    }

    line_shader->stop_using();
}

void GLGizmoCut3D::render_grabber_connection(const ColorRGBA& color, Transform3d view_matrix, double line_len_koef/* = 1.0*/)
{
    const Transform3d line_view_matrix = view_matrix * scale_transform(Vec3d(1.0, 1.0, line_len_koef * m_grabber_connection_len));

    render_line(m_grabber_connection, color, line_view_matrix, 0.2f);
};

void GLGizmoCut3D::render_cut_plane_grabbers()
{
    glsafe(::glClear(GL_DEPTH_BUFFER_BIT));

    ColorRGBA color = ColorRGBA::GRAY();

    const Transform3d view_matrix = wxGetApp().plater()->get_camera().get_view_matrix() * translation_transform(m_plane_center) * m_rotation_m;

    const double mean_size = get_grabber_mean_size(m_bounding_box);
    double size;

    const bool no_xy_dragging = m_dragging && m_hover_id == CutPlane;

    if (!no_xy_dragging && m_hover_id != CutPlaneZRotation && m_hover_id != CutPlaneXMove && m_hover_id != CutPlaneYMove) {
        render_grabber_connection(GRABBER_COLOR, view_matrix);

        // render sphere grabber
        size = m_dragging ? get_dragging_half_size(mean_size) : get_half_size(mean_size);
        color = m_hover_id == Y ? ColorRGBA::Y() : // ORCA match axis colors
                m_hover_id == X ? ColorRGBA::X() : // ORCA match axis colors
                m_hover_id == Z ? GRABBER_COLOR                     :   ColorRGBA::GRAY();
        render_model(m_sphere.model, color, view_matrix * translation_transform(m_grabber_connection_len * Vec3d::UnitZ()) * scale_transform(size));
    }

    const bool no_xy_grabber_hovered = !m_dragging && (m_hover_id < 0 || m_hover_id == CutPlane);

    // render X grabber

    if (no_xy_grabber_hovered || m_hover_id == X)
    {
        size = m_dragging && m_hover_id == X ? get_dragging_half_size(mean_size) : get_half_size(mean_size);
        const Vec3d cone_scale = Vec3d(0.75 * size, 0.75 * size, 1.8 * size);
        //color = m_hover_id == X ? complementary(ColorRGBA::X()) : ColorRGBA::X();
        color = ColorRGBA::X(); // ORCA match axis colors

        if (m_hover_id == X) {
            render_grabber_connection(color, view_matrix);
            render_rotation_snapping(X, color);
        }

        Vec3d offset = Vec3d(0.0, 1.25 * size, m_grabber_connection_len);
        render_model(m_cone.model, color, view_matrix * translation_transform(offset) * rotation_transform(-0.5 * PI * Vec3d::UnitX()) * scale_transform(cone_scale));
        offset = Vec3d(0.0, -1.25 * size, m_grabber_connection_len);
        render_model(m_cone.model, color, view_matrix * translation_transform(offset) * rotation_transform(0.5 * PI * Vec3d::UnitX()) * scale_transform(cone_scale));
    }

    // render Y grabber

    if (no_xy_grabber_hovered || m_hover_id == Y)
    {
        size = m_dragging && m_hover_id == Y ? get_dragging_half_size(mean_size) : get_half_size(mean_size);
        const Vec3d cone_scale = Vec3d(0.75 * size, 0.75 * size, 1.8 * size);
        //color = m_hover_id == Y ? complementary(ColorRGBA::Y()) : ColorRGBA::Y();
        color = ColorRGBA::Y(); // ORCA match axis colors

        if (m_hover_id == Y) {
            render_grabber_connection(color, view_matrix);
            render_rotation_snapping(Y, color);
        }

        Vec3d offset = Vec3d(1.25 * size, 0.0, m_grabber_connection_len);
        render_model(m_cone.model, color, view_matrix * translation_transform(offset) * rotation_transform(0.5 * PI * Vec3d::UnitY()) * scale_transform(cone_scale));
        offset = Vec3d(-1.25 * size, 0.0, m_grabber_connection_len);
        render_model(m_cone.model, color, view_matrix * translation_transform(offset) * rotation_transform(-0.5 * PI * Vec3d::UnitY()) * scale_transform(cone_scale));
    }

    if (CutMode(m_mode) == CutMode::cutTongueAndGroove) {

        // render CutPlaneZRotation grabber

        if (no_xy_grabber_hovered || m_hover_id == CutPlaneZRotation)
        {
            size = 0.75 * (m_dragging ? get_dragging_half_size(mean_size) : get_half_size(mean_size));
            color = ColorRGBA::Z(); // ORCA match axis colors
            const ColorRGBA cp_color = m_hover_id == CutPlaneZRotation ? color : m_plane.model.get_color();

            const double grabber_shift = -1.75 * m_grabber_connection_len;

            render_model(m_sphere.model, cp_color, view_matrix * translation_transform(grabber_shift * Vec3d::UnitY()) * scale_transform(size));

            if (m_hover_id == CutPlaneZRotation) {
                const Vec3d cone_scale = Vec3d(0.75 * size, 0.75 * size, 1.8 * size);

                render_rotation_snapping(CutPlaneZRotation, color);
                render_grabber_connection(GRABBER_COLOR, view_matrix * rotation_transform(0.5 * PI * Vec3d::UnitX()), 1.75);

                Vec3d offset = Vec3d(1.25 * size, grabber_shift, 0.0);
                render_model(m_cone.model, color, view_matrix * translation_transform(offset) * rotation_transform(0.5 * PI * Vec3d::UnitY()) * scale_transform(cone_scale));
                offset = Vec3d(-1.25 * size, grabber_shift, 0.0);
                render_model(m_cone.model, color, view_matrix * translation_transform(offset) * rotation_transform(-0.5 * PI * Vec3d::UnitY()) * scale_transform(cone_scale));
            }
        }

        const double xy_connection_len = 0.75 * m_grabber_connection_len;

        // render CutPlaneXMove grabber

        if (no_xy_grabber_hovered || m_hover_id == CutPlaneXMove)
        {
            size = (m_dragging ? get_dragging_half_size(mean_size) : get_half_size(mean_size));
            color = m_hover_id == CutPlaneXMove ? ColorRGBA::X() : m_plane.model.get_color(); // ORCA match axis colors

            render_grabber_connection(GRABBER_COLOR, view_matrix * rotation_transform(0.5 * PI * Vec3d::UnitY()), 0.75);

            Vec3d offset = xy_connection_len * Vec3d::UnitX() - 0.5 * size * Vec3d::Ones();
            render_model(m_cube.model, color, view_matrix * translation_transform(offset) * scale_transform(size));

            const Vec3d cone_scale = Vec3d(0.5 * size, 0.5 * size, 1.8 * size);

            offset = (size + xy_connection_len) * Vec3d::UnitX();
            render_model(m_cone.model, color, view_matrix * translation_transform(offset) * rotation_transform(0.5 * PI * Vec3d::UnitY()) * scale_transform(cone_scale));
        }

        // render CutPlaneYMove grabber

        if (m_groove.angle > 0.0f && (no_xy_grabber_hovered || m_hover_id == CutPlaneYMove))
        {
            size = (m_dragging ? get_dragging_half_size(mean_size) : get_half_size(mean_size));
            color = m_hover_id == CutPlaneYMove ? ColorRGBA::Y() : m_plane.model.get_color(); // ORCA match axis colors

            render_grabber_connection(GRABBER_COLOR, view_matrix * rotation_transform(-0.5 * PI * Vec3d::UnitX()), 0.75);

            Vec3d offset = xy_connection_len * Vec3d::UnitY() - 0.5 * size * Vec3d::Ones();
            render_model(m_cube.model, color, view_matrix * translation_transform(offset) * scale_transform(size));

            const Vec3d cone_scale = Vec3d(0.5 * size, 0.5 * size, 1.8 * size);

            offset = (size + xy_connection_len) * Vec3d::UnitY();
            render_model(m_cone.model, color, view_matrix * translation_transform(offset) * rotation_transform(-0.5 * PI * Vec3d::UnitX()) * scale_transform(cone_scale));
        }
    }
}

void GLGizmoCut3D::render_cut_line()
{
    if (!cut_line_processing() || m_line_end.isApprox(Vec3d::Zero()))
        return;

    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glClear(GL_DEPTH_BUFFER_BIT));

    m_cut_line.reset();
    m_cut_line.init_from(its_make_line((Vec3f)m_line_beg.cast<float>(), (Vec3f)m_line_end.cast<float>()));

    render_line(m_cut_line, GRABBER_COLOR, wxGetApp().plater()->get_camera().get_view_matrix(), 0.25f);
}

bool GLGizmoCut3D::on_init()
{
    m_grabbers.emplace_back();
    m_shortcut_key = WXK_CONTROL_C;

    // initiate info shortcuts
    const wxString ctrl  = GUI::shortkey_ctrl_prefix();
    const wxString alt   = GUI::shortkey_alt_prefix();
    const wxString shift = _L("Shift+");

    m_shortcuts_cut.push_back(std::make_pair(shift + _L("Drag"), _L("Draw cut line")));

    m_shortcuts_connector.push_back(std::make_pair(_L("Left click"),         _L("Add connector")));
    m_shortcuts_connector.push_back(std::make_pair(_L("Right click"),        _L("Remove connector")));
    m_shortcuts_connector.push_back(std::make_pair(_L("Drag"),               _L("Move connector")));
    m_shortcuts_connector.push_back(std::make_pair(shift + _L("Left click"), _L("Add connector to selection")));
    m_shortcuts_connector.push_back(std::make_pair(alt   + _L("Left click"), _L("Remove connector from selection")));
    m_shortcuts_connector.push_back(std::make_pair(ctrl  + "A",              _L("Select all connectors")));

    return true;
}

void GLGizmoCut3D::on_load(cereal::BinaryInputArchive& ar)
{
    size_t mode;
    float groove_depth;
    float groove_width;
    float groove_flaps_angle;
    float groove_angle;
    float groove_depth_tolerance;
    float groove_width_tolerance;

    ar( m_keep_upper, m_keep_lower, m_rotate_lower, m_rotate_upper, m_hide_cut_plane, mode, m_connectors_editing,
        m_ar_plane_center, m_rotation_m,
        groove_depth, groove_width, groove_flaps_angle, groove_angle, groove_depth_tolerance, groove_width_tolerance);

    m_start_dragging_m = m_rotation_m;

    m_transformed_bounding_box = transformed_bounding_box(m_ar_plane_center, m_rotation_m);
    set_center_pos(m_ar_plane_center);

    if (m_mode != mode)
        switch_to_mode(mode);
    else if (CutMode(m_mode) == CutMode::cutTongueAndGroove) {
        if (!is_approx(m_groove.depth          , groove_depth) ||
            !is_approx(m_groove.width          , groove_width) ||
            !is_approx(m_groove.flaps_angle    , groove_flaps_angle) ||
            !is_approx(m_groove.angle          , groove_angle) ||
            !is_approx(m_groove.depth_tolerance, groove_depth_tolerance) ||
            !is_approx(m_groove.width_tolerance, groove_width_tolerance) ) 
        {
            m_groove.depth          = groove_depth;
            m_groove.width          = groove_width;
            m_groove.flaps_angle    = groove_flaps_angle;
            m_groove.angle          = groove_angle;
            m_groove.depth_tolerance= groove_depth_tolerance;
            m_groove.width_tolerance= groove_width_tolerance;
            update_plane_model();
        }
        reset_cut_by_contours();
    }

    m_parent.request_extra_frame();
}

void GLGizmoCut3D::on_save(cereal::BinaryOutputArchive& ar) const
{ 
    ar( m_keep_upper, m_keep_lower, m_rotate_lower, m_rotate_upper, m_hide_cut_plane, m_mode, m_connectors_editing,
        m_ar_plane_center, m_start_dragging_m,
        m_groove.depth, m_groove.width, m_groove.flaps_angle, m_groove.angle, m_groove.depth_tolerance, m_groove.width_tolerance);
}

std::string GLGizmoCut3D::on_get_name() const
{
    return _u8L("Cut");
}

void GLGizmoCut3D::apply_color_clip_plane_colors()
{
    if (CutMode(m_mode) == CutMode::cutTongueAndGroove)
        m_parent.set_color_clip_plane_colors({ CUT_PLANE_DEF_COLOR , CUT_PLANE_DEF_COLOR });
    else
        m_parent.set_color_clip_plane_colors({ UPPER_PART_COLOR , LOWER_PART_COLOR });
}

void GLGizmoCut3D::on_set_state()
{
    if (m_state == On) {
        m_parent.set_use_color_clip_plane(true);

        update_bb();
        m_connectors_editing = !m_selected.empty();
        m_transformed_bounding_box = transformed_bounding_box(m_plane_center, m_rotation_m);

        // initiate archived values
        m_ar_plane_center   = m_plane_center;
        m_start_dragging_m  = m_rotation_m;
        reset_cut_by_contours();

        m_parent.request_extra_frame();
    }
    else {
        if (auto oc = m_c->object_clipper()) {
            oc->set_behavior(true, true, 0.);
            oc->release();
        }
        m_selected.clear();
        m_parent.set_use_color_clip_plane(false);
        //m_c->selection_info()->set_use_shift(false);

        // Make sure that the part selection data are released when the gizmo is closed.
        // The CallAfter is needed because in perform_cut, the gizmo is closed BEFORE
        // the cut is performed (because of undo/redo snapshots), so the data would
        // be deleted prematurely.
        if (m_part_selection.valid())
            wxGetApp().CallAfter([this]() { m_part_selection = PartSelection(); });
    }
}

void GLGizmoCut3D::on_register_raycasters_for_picking()
{
 //   assert(m_raycasters.empty());
    if (!m_raycasters.empty())
        on_unregister_raycasters_for_picking();
    // the gizmo grabbers are rendered on top of the scene, so the raytraced picker should take it into account
    m_parent.set_raycaster_gizmos_on_top(true);

    init_picking_models();

    if (m_connectors_editing) {
        if (CommonGizmosDataObjects::SelectionInfo* si = m_c->selection_info()) {
            const CutConnectors& connectors = si->model_object()->cut_connectors;
            for (int i = 0; i < int(connectors.size()); ++i)
                m_raycasters.emplace_back(m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, i + m_connectors_group_id, *(m_shapes[connectors[i].attribs]).mesh_raycaster, Transform3d::Identity()));
        }
    }
    else if (!cut_line_processing()) {
        m_raycasters.emplace_back(m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, X, *m_cone.mesh_raycaster, Transform3d::Identity()));
        m_raycasters.emplace_back(m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, X, *m_cone.mesh_raycaster, Transform3d::Identity()));

        m_raycasters.emplace_back(m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, Y, *m_cone.mesh_raycaster, Transform3d::Identity()));
        m_raycasters.emplace_back(m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, Y, *m_cone.mesh_raycaster, Transform3d::Identity()));

        m_raycasters.emplace_back(m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, Z, *m_sphere.mesh_raycaster, Transform3d::Identity()));

        m_raycasters.emplace_back(m_parent.add_raycaster_for_picking(SceneRaycaster::EType::FallbackGizmo, CutPlane, *m_plane.mesh_raycaster, Transform3d::Identity()));

        if (CutMode(m_mode) == CutMode::cutTongueAndGroove) {
            m_raycasters.emplace_back(m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, CutPlaneZRotation, *m_sphere.mesh_raycaster, Transform3d::Identity()));
            m_raycasters.emplace_back(m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, CutPlaneZRotation, *m_cone.mesh_raycaster, Transform3d::Identity()));
            m_raycasters.emplace_back(m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, CutPlaneZRotation, *m_cone.mesh_raycaster, Transform3d::Identity()));

            m_raycasters.emplace_back(m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, CutPlaneXMove, *m_cube.mesh_raycaster, Transform3d::Identity()));
            m_raycasters.emplace_back(m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, CutPlaneXMove, *m_cone.mesh_raycaster, Transform3d::Identity()));

            m_raycasters.emplace_back(m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, CutPlaneYMove, *m_cube.mesh_raycaster, Transform3d::Identity()));
            m_raycasters.emplace_back(m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, CutPlaneYMove, *m_cone.mesh_raycaster, Transform3d::Identity()));
        }
    }

    update_raycasters_for_picking_transform();
}

void GLGizmoCut3D::on_unregister_raycasters_for_picking()
{
    m_parent.remove_raycasters_for_picking(SceneRaycaster::EType::Gizmo);
    m_parent.remove_raycasters_for_picking(SceneRaycaster::EType::FallbackGizmo);
    m_raycasters.clear();
    // the gizmo grabbers are rendered on top of the scene, so the raytraced picker should take it into account
    m_parent.set_raycaster_gizmos_on_top(false);
}

void GLGizmoCut3D::update_raycasters_for_picking()
{
    on_unregister_raycasters_for_picking();
    on_register_raycasters_for_picking();
}

void GLGizmoCut3D::set_volumes_picking_state(bool state)
{
    std::vector<std::shared_ptr<SceneRaycasterItem>>* raycasters = m_parent.get_raycasters_for_picking(SceneRaycaster::EType::Volume);
    if (raycasters != nullptr) {
        const Selection& selection = m_parent.get_selection();
        const Selection::IndicesList ids = selection.get_volume_idxs();
        for (unsigned int id : ids) {
            const GLVolume* v = selection.get_volume(id);
            auto it = std::find_if(raycasters->begin(), raycasters->end(), [v](std::shared_ptr<SceneRaycasterItem> item) { return item->get_raycaster() == v->mesh_raycaster.get(); });
            if (it != raycasters->end())
                (*it)->set_active(state);
        }
    }
}

void GLGizmoCut3D::update_raycasters_for_picking_transform()
{
    if (m_connectors_editing) {
        CommonGizmosDataObjects::SelectionInfo* si = m_c->selection_info();
        if (!si) 
            return;
        const ModelObject* mo = si->model_object();
        const CutConnectors& connectors = mo->cut_connectors;
        if (connectors.empty())
            return;
        auto inst_id = m_c->selection_info()->get_active_instance();
        if (inst_id < 0)
            return;

        const Vec3d& instance_offset = mo->instances[inst_id]->get_offset();
        const double sla_shift = double(m_c->selection_info()->get_sla_shift());

        const bool looking_forward = is_looking_forward();

        for (size_t i = 0; i < connectors.size(); ++i) {
            const CutConnector& connector = connectors[i];

            float height = connector.height;
            // recalculate connector position to world position
            Vec3d pos = connector.pos + instance_offset;
            if (connector.attribs.type == CutConnectorType::Dowel &&
                connector.attribs.style == CutConnectorStyle::Prism) {
                height = 0.05f;
                if (!looking_forward)
                    pos += 0.05 * m_clp_normal;
            }
            pos[Z] += sla_shift;

            const Transform3d scale_trafo = scale_transform(Vec3f(connector.radius, connector.radius, height).cast<double>());
            m_raycasters[i]->set_transform(translation_transform(pos) * m_rotation_m * scale_trafo);
        }
    }
    else if (!cut_line_processing()){
        const Transform3d trafo = translation_transform(m_plane_center) * m_rotation_m;

        const BoundingBoxf3 box = m_bounding_box;

        const double size = get_half_size(get_grabber_mean_size(box));
        Vec3d scale = Vec3d(0.75 * size, 0.75 * size, 1.8 * size);

        int id = 0;

        Vec3d offset = Vec3d(0.0, 1.25 * size, m_grabber_connection_len);
        m_raycasters[id++]->set_transform(trafo * translation_transform(offset) * rotation_transform(-0.5 * PI * Vec3d::UnitX()) * scale_transform(scale));
        offset = Vec3d(0.0, -1.25 * size, m_grabber_connection_len);
        m_raycasters[id++]->set_transform(trafo * translation_transform(offset) * rotation_transform(0.5 * PI * Vec3d::UnitX()) * scale_transform(scale));

        offset = Vec3d(1.25 * size, 0.0, m_grabber_connection_len);
        m_raycasters[id++]->set_transform(trafo * translation_transform(offset) * rotation_transform(0.5 * PI * Vec3d::UnitY()) * scale_transform(scale));
        offset = Vec3d(-1.25 * size, 0.0, m_grabber_connection_len);
        m_raycasters[id++]->set_transform(trafo * translation_transform(offset) * rotation_transform(-0.5 * PI * Vec3d::UnitY()) * scale_transform(scale));

        m_raycasters[id++]->set_transform(trafo * translation_transform(m_grabber_connection_len * Vec3d::UnitZ()) * scale_transform(size));

        m_raycasters[id++]->set_transform(trafo);

        if (CutMode(m_mode) == CutMode::cutTongueAndGroove) {

            double grabber_y_shift = -1.75 * m_grabber_connection_len;

            m_raycasters[id++]->set_transform(trafo * translation_transform(grabber_y_shift * Vec3d::UnitY()) * scale_transform(size));

            offset = Vec3d(1.25 * size, grabber_y_shift, 0.0);
            m_raycasters[id++]->set_transform(trafo * translation_transform(offset) * rotation_transform(0.5 * PI * Vec3d::UnitY()) * scale_transform(scale));
            offset = Vec3d(-1.25 * size, grabber_y_shift, 0.0);
            m_raycasters[id++]->set_transform(trafo * translation_transform(offset) * rotation_transform(-0.5 * PI * Vec3d::UnitY()) * scale_transform(scale));

            const double xy_connection_len = 0.75 * m_grabber_connection_len;
            const Vec3d cone_scale = Vec3d(0.5 * size, 0.5 * size, 1.8 * size);

            offset = xy_connection_len * Vec3d::UnitX() - 0.5 * size * Vec3d::Ones();
            m_raycasters[id++]->set_transform(trafo * translation_transform(offset) * scale_transform(size));
            offset = (size + xy_connection_len) * Vec3d::UnitX();
            m_raycasters[id++]->set_transform(trafo * translation_transform(offset) * rotation_transform(0.5 * PI * Vec3d::UnitY()) * scale_transform(cone_scale));

            if (m_groove.angle > 0.0f) {
                offset = xy_connection_len * Vec3d::UnitY() - 0.5 * size * Vec3d::Ones();
                m_raycasters[id++]->set_transform(trafo * translation_transform(offset) * scale_transform(size));
                offset = (size + xy_connection_len) * Vec3d::UnitY();
                m_raycasters[id++]->set_transform(trafo * translation_transform(offset) * rotation_transform(-0.5 * PI * Vec3d::UnitX()) * scale_transform(cone_scale));
            }
            else {
                // discard transformation for CutPlaneYMove grabbers
                m_raycasters[id++]->set_transform(Transform3d::Identity());
                m_raycasters[id++]->set_transform(Transform3d::Identity());
            }
        }
    }
}

void GLGizmoCut3D::update_plane_model()
{
    m_plane.reset();
    on_unregister_raycasters_for_picking();

    init_picking_models();
}

void GLGizmoCut3D::on_set_hover_id() 
{
}

bool GLGizmoCut3D::on_is_activable() const
{
    const Selection& selection = m_parent.get_selection();
    const int object_idx = selection.get_object_idx();
    if (object_idx < 0 || selection.is_wipe_tower())
        return false;

    if (const ModelObject* mo = wxGetApp().plater()->model().objects[object_idx];
        mo->is_cut() && mo->volumes.size() == 1) {
        const ModelVolume* volume = mo->volumes[0];
        if (volume->is_cut_connector() && volume->cut_info.connector_type == CutConnectorType::Dowel)
            return false;
    }

    // This is assumed in GLCanvas3D::do_rotate, do not change this
    // without updating that function too.
    return selection.is_single_full_instance() && !m_parent.is_layers_editing_enabled();
}

bool GLGizmoCut3D::on_is_selectable() const
{
    return wxGetApp().get_mode() != comSimple;
}

Vec3d GLGizmoCut3D::mouse_position_in_local_plane(GrabberID axis, const Linef3& mouse_ray) const
{
    double half_pi = 0.5 * PI;

    Transform3d m = Transform3d::Identity();

    switch (axis)
    {
    case X:
    {
        m.rotate(Eigen::AngleAxisd(half_pi, Vec3d::UnitZ()));
        m.rotate(Eigen::AngleAxisd(-half_pi, Vec3d::UnitY()));
        break;
    }
    case Y:
    {
        m.rotate(Eigen::AngleAxisd(half_pi, Vec3d::UnitY()));
        m.rotate(Eigen::AngleAxisd(half_pi, Vec3d::UnitZ()));
        break;
    }
    case Z:
    default:
    {
        // no rotation applied
        break;
    }
    }

    m = m * m_start_dragging_m.inverse();
    m.translate(-m_plane_center);

    return transform(mouse_ray, m).intersect_plane(0.0);
}

void GLGizmoCut3D::dragging_grabber_move(const GLGizmoBase::UpdateData &data)
{
    Vec3d starting_drag_position;
    if (m_hover_id == Z)
        starting_drag_position = translation_transform(m_plane_center) * m_rotation_m * (m_grabber_connection_len * Vec3d::UnitZ());
    else
        starting_drag_position = m_cut_plane_start_move_pos;

    double projection  = 0.0;

    Vec3d starting_vec = m_rotation_m * (m_hover_id == CutPlaneXMove ? Vec3d::UnitX() : m_hover_id == CutPlaneYMove ? Vec3d::UnitY() : Vec3d::UnitZ());
    if (starting_vec.norm() != 0.0) {
        const Vec3d mouse_dir = data.mouse_ray.unit_vector();
        // finds the intersection of the mouse ray with the plane parallel to the camera viewport and passing through the starting position
        // use ray-plane intersection see i.e. https://en.wikipedia.org/wiki/Line%E2%80%93plane_intersection algebraic form
        // in our case plane normal and ray direction are the same (orthogonal view)
        // when moving to perspective camera the negative z unit axis of the camera needs to be transformed in world space and used as plane normal
        const Vec3d inters = data.mouse_ray.a + (starting_drag_position - data.mouse_ray.a).dot(mouse_dir) * mouse_dir;
        // vector from the starting position to the found intersection
        const Vec3d inters_vec = inters - starting_drag_position;

        starting_vec.normalize();
        // finds projection of the vector along the staring direction
        projection = inters_vec.dot(starting_vec);
    }
    if (wxGetKeyState(WXK_SHIFT))
        projection = m_snap_step * std::round(projection / m_snap_step);

    const Vec3d shift = starting_vec * projection;
    if (shift != Vec3d::Zero())
        reset_cut_by_contours();

    // move  cut plane center
    set_center(m_plane_center + shift, true);

    m_was_cut_plane_dragged = true;
}

void GLGizmoCut3D::dragging_grabber_rotation(const GLGizmoBase::UpdateData &data)
{
    const Vec2d mouse_pos = to_2d(mouse_position_in_local_plane((GrabberID)m_hover_id, data.mouse_ray));

    const Vec2d orig_dir = Vec2d::UnitX();
    const Vec2d new_dir  = mouse_pos.normalized();

    const double two_pi = 2.0 * PI;

    double theta = ::acos(std::clamp(new_dir.dot(orig_dir), -1.0, 1.0));
    if (cross2(orig_dir, new_dir) < 0.0)
        theta = two_pi - theta;

    const double len = mouse_pos.norm();
    // snap to coarse snap region
    if (m_snap_coarse_in_radius <= len && len <= m_snap_coarse_out_radius) {
        const double step = two_pi / double(SnapRegionsCount);
        theta             = step * std::round(theta / step);
    }
    // snap to fine snap region (scale)
    else if (m_snap_fine_in_radius <= len && len <= m_snap_fine_out_radius) {
        const double step = two_pi / double(ScaleStepsCount);
        theta             = step * std::round(theta / step);
    }

    if (is_approx(theta, two_pi))
        theta = 0.0;
    if (m_hover_id != Y)
        theta += 0.5 * PI;

    if (!is_approx(theta, 0.0))
        reset_cut_by_contours();

    Vec3d rotation = Vec3d::Zero();
    rotation[m_hover_id == CutPlaneZRotation ? Z : m_hover_id] = theta;

    const Transform3d rotation_tmp = m_start_dragging_m * rotation_transform(rotation);
    const bool update_tbb = !m_rotation_m.rotation().isApprox(rotation_tmp.rotation());
    m_rotation_m = rotation_tmp;
    if (update_tbb)
        m_transformed_bounding_box = transformed_bounding_box(m_plane_center, m_rotation_m);

    m_angle = theta;
    while (m_angle > two_pi)
        m_angle -= two_pi;
    if (m_angle < 0.0)
        m_angle += two_pi;

    update_clipper();
}

void GLGizmoCut3D::dragging_connector(const GLGizmoBase::UpdateData &data)
{
    CutConnectors&          connectors = m_c->selection_info()->model_object()->cut_connectors;
    Vec3d                   pos;
    Vec3d                   pos_world;

    if (unproject_on_cut_plane(data.mouse_pos.cast<double>(), pos, pos_world)) {
        connectors[m_hover_id - m_connectors_group_id].pos = pos;
        update_raycasters_for_picking_transform();
    }
}

void GLGizmoCut3D::on_dragging(const UpdateData& data)
{
    if (m_hover_id < 0)
        return;
    if (m_hover_id == Z || m_hover_id == CutPlane || m_hover_id == CutPlaneXMove || m_hover_id == CutPlaneYMove)
        dragging_grabber_move(data);
    else if (m_hover_id == X || m_hover_id == Y || m_hover_id == CutPlaneZRotation)
        dragging_grabber_rotation(data);
    else if (m_hover_id >= m_connectors_group_id && m_connector_mode == CutConnectorMode::Manual)
        dragging_connector(data);
    check_and_update_connectors_state();

    if (CutMode(m_mode) == CutMode::cutTongueAndGroove)
        reset_cut_by_contours();
}

void GLGizmoCut3D::on_start_dragging()
{
    m_angle = 0.0;
    if (m_hover_id >= m_connectors_group_id && m_connector_mode == CutConnectorMode::Manual)
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Move connector"), UndoRedo::SnapshotType::GizmoAction);

    if (m_hover_id == X || m_hover_id == Y || m_hover_id == CutPlaneZRotation)
        m_start_dragging_m = m_rotation_m;
}

void GLGizmoCut3D::on_stop_dragging()
{
    if (m_hover_id == X || m_hover_id == Y || m_hover_id == CutPlaneZRotation) {
        m_angle_arc.reset();
        m_angle = 0.0;
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Rotate cut plane"), UndoRedo::SnapshotType::GizmoAction);
        m_start_dragging_m = m_rotation_m;
    }
    else if (m_hover_id == Z || m_hover_id == CutPlane || m_hover_id == CutPlaneXMove|| m_hover_id == CutPlaneYMove) {
        if (m_was_cut_plane_dragged)
            Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Move cut plane"), UndoRedo::SnapshotType::GizmoAction);
        m_ar_plane_center = m_plane_center;
    }

    if (CutMode(m_mode) == CutMode::cutTongueAndGroove)
        reset_cut_by_contours();
    //check_and_update_connectors_state();
}

void GLGizmoCut3D::set_center_pos(const Vec3d& center_pos, bool update_tbb /*=false*/)
{
    BoundingBoxf3 tbb = m_transformed_bounding_box;
    if (update_tbb) {
        Vec3d normal = m_rotation_m.inverse() * Vec3d(m_plane_center - center_pos);
        tbb.translate(normal.z() * Vec3d::UnitZ());
    }

    bool can_set_center_pos = false;
    {
        double limit_val = /*CutMode(m_mode) == CutMode::cutTongueAndGroove ? 0.5 * double(m_groove.depth) : */0.5;
        if (tbb.max.z() > -limit_val && tbb.min.z() < limit_val)
            can_set_center_pos = true;
        else {
            const double old_dist = (m_bb_center - m_plane_center).norm();
            const double new_dist = (m_bb_center - center_pos).norm();
            // check if forcing is reasonable
            if (new_dist < old_dist)
                can_set_center_pos = true;
        }
    }

    if (can_set_center_pos) {
        m_transformed_bounding_box = tbb;
        m_plane_center = center_pos;
        m_center_offset = m_plane_center - m_bb_center;
    }
}

BoundingBoxf3 GLGizmoCut3D::bounding_box() const
{
    BoundingBoxf3 ret;
    const Selection& selection = m_parent.get_selection();
    const Selection::IndicesList& idxs = selection.get_volume_idxs();
    for (unsigned int i : idxs) {
        const GLVolume* volume = selection.get_volume(i);
        // respect just to the solid parts for FFF and ignore pad and supports for SLA
        if (!volume->is_modifier && !volume->is_sla_pad() && !volume->is_sla_support())
            ret.merge(volume->transformed_convex_hull_bounding_box());
    }
    return ret;
}

BoundingBoxf3 GLGizmoCut3D::transformed_bounding_box(const Vec3d& plane_center, const Transform3d& rotation_m/* = Transform3d::Identity()*/) const
{
    const Selection& selection = m_parent.get_selection();

    const auto first_volume = selection.get_first_volume();
    Vec3d instance_offset   = first_volume->get_instance_offset();
    instance_offset[Z]     += first_volume->get_sla_shift_z();

    const auto cut_matrix = Transform3d::Identity() * rotation_m.inverse() * translation_transform(instance_offset - plane_center);

    const Selection::IndicesList& idxs = selection.get_volume_idxs();
    BoundingBoxf3 ret;
    for (unsigned int i : idxs) {
        const GLVolume* volume = selection.get_volume(i);
        // respect just to the solid parts for FFF and ignore pad and supports for SLA
        if (!volume->is_modifier && !volume->is_sla_pad() && !volume->is_sla_support()) {

            const auto instance_matrix = volume->get_instance_transformation().get_matrix_no_offset();
            auto volume_trafo = instance_matrix * volume->get_volume_transformation().get_matrix();
            ret.merge(volume->transformed_convex_hull_bounding_box(cut_matrix * volume_trafo));
        }
    }
    return ret;
}

void GLGizmoCut3D::update_bb()
{
    const BoundingBoxf3 box = bounding_box();
    if (!box.defined)
        return;
    if (!m_max_pos.isApprox(box.max) || !m_min_pos.isApprox(box.min)) {

        m_bounding_box = box;

        // check, if mode is set to Planar, when object has a connectors
        if (const int object_idx = m_parent.get_selection().get_object_idx();
            object_idx >= 0 && !wxGetApp().plater()->model().objects[object_idx]->cut_connectors.empty())
            m_mode = size_t(CutMode::cutPlanar);

        invalidate_cut_plane();
        reset_cut_by_contours();
        apply_color_clip_plane_colors();

        m_max_pos = box.max;
        m_min_pos = box.min;
        m_bb_center = box.center();
        m_transformed_bounding_box = transformed_bounding_box(m_bb_center);
        if (box.contains(m_center_offset))
            set_center_pos(m_bb_center + m_center_offset);
        else
            set_center_pos(m_bb_center);

        m_contour_width = CutMode(m_mode) == CutMode::cutTongueAndGroove ? 0.f : 0.4f;

        m_radius = box.radius();
        m_grabber_connection_len = 0.5 * m_radius;// std::min<double>(0.75 * m_radius, 35.0);
        m_grabber_radius = m_grabber_connection_len * 0.85;

        m_snap_coarse_in_radius   = m_grabber_radius / 3.0;
        m_snap_coarse_out_radius  = m_snap_coarse_in_radius * 2.;
        m_snap_fine_in_radius     = m_grabber_connection_len * 0.85;
        m_snap_fine_out_radius    = m_grabber_connection_len * 1.15;

        // input params for cut with tongue and groove
        m_groove.depth = m_groove.depth_init = std::max(1.f , 0.5f * float(get_grabber_mean_size(m_bounding_box)));
        m_groove.width = m_groove.width_init = 4.0f * m_groove.depth;
        m_groove.flaps_angle = m_groove.flaps_angle_init = float(PI) / 3.f;
        m_groove.angle = m_groove.angle_init = 0.f;
        m_plane.reset();
        m_cone.reset();
        m_sphere.reset();
        m_cube.reset();
        m_grabber_connection.reset();
        m_circle.reset();
        m_scale.reset();
        m_snap_radii.reset();
        m_reference_radius.reset();

        on_unregister_raycasters_for_picking();

        clear_selection();
        if (CommonGizmosDataObjects::SelectionInfo* selection = m_c->selection_info();
            selection && selection->model_object())
            m_selected.resize(selection->model_object()->cut_connectors.size(), false);
    }
}

void GLGizmoCut3D::init_picking_models()
{
    if (!m_cone.model.is_initialized()) {
        indexed_triangle_set its = its_make_cone(1.0, 1.0, PI / 12.0);
        m_cone.model.init_from(its);
        m_cone.mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(std::move(its)));
    }
    if (!m_sphere.model.is_initialized()) {
        indexed_triangle_set its = its_make_sphere(1.0, PI / 12.0);
        m_sphere.model.init_from(its);
        m_sphere.mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(std::move(its)));
    }
    if (!m_cube.model.is_initialized()) {
        indexed_triangle_set its = its_make_cube(1., 1., 1.);
        m_cube.model.init_from(its);
        m_cube.mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(std::move(its)));
    }

    if (!m_plane.model.is_initialized() && !m_hide_cut_plane && !m_connectors_editing) {
        const double cp_width = 0.02 * get_grabber_mean_size(m_bounding_box);
        indexed_triangle_set its = m_mode == size_t(CutMode::cutTongueAndGroove) ? its_make_groove_plane() :
                                   its_make_frustum_dowel((double)m_cut_plane_radius_koef * m_radius, cp_width, m_cut_plane_as_circle ? 180 : 4);

        m_plane.model.init_from(its);
        m_plane.mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(std::move(its)));
    }

    if (m_shapes.empty())
        init_connector_shapes();
}

void GLGizmoCut3D::init_rendering_items()
{
    if (!m_grabber_connection.is_initialized())
        m_grabber_connection.init_from(its_make_line(Vec3f::Zero(), Vec3f::UnitZ()));
    if (!m_circle.is_initialized())
        init_from_circle(m_circle, m_grabber_radius);
    if (!m_scale.is_initialized())
        init_from_scale(m_scale, m_grabber_radius);
    if (!m_snap_radii.is_initialized())
        init_from_snap_radii(m_snap_radii, m_grabber_radius);
    if (!m_reference_radius.is_initialized()) {
        m_reference_radius.init_from(its_make_line(Vec3f::Zero(), m_grabber_connection_len * Vec3f::UnitX()));
        m_reference_radius.set_color(ColorRGBA::WHITE());
    }
    if (!m_angle_arc.is_initialized() || m_angle != 0.0)
        init_from_angle_arc(m_angle_arc, m_angle, m_grabber_connection_len);
}

void GLGizmoCut3D::render_clipper_cut()
{
    if (! m_connectors_editing)
        ::glDisable(GL_DEPTH_TEST);

    GLboolean cull_face = GL_FALSE;
    ::glGetBooleanv(GL_CULL_FACE, &cull_face);
    ::glDisable(GL_CULL_FACE);
    m_c->object_clipper()->render_cut(m_part_selection.get_ignored_contours_ptr());
    if (cull_face)
        ::glEnable(GL_CULL_FACE);

    if (! m_connectors_editing)
        ::glEnable(GL_DEPTH_TEST);
}

void GLGizmoCut3D::PartSelection::add_object(const ModelObject* object)
{
    m_model = Model();
    m_model.add_object(*object);

    const double sla_shift_z = wxGetApp().plater()->canvas3D()->get_selection().get_first_volume()->get_sla_shift_z();
    if (!is_approx(sla_shift_z, 0.)) {
        Vec3d inst_offset = model_object()->instances[m_instance_idx]->get_offset();
        inst_offset[Z] += sla_shift_z;
        model_object()->instances[m_instance_idx]->set_offset(inst_offset);
    }
}


GLGizmoCut3D::PartSelection::PartSelection(const ModelObject* mo, const Transform3d& cut_matrix, int instance_idx_in, const Vec3d& center, const Vec3d& normal, const CommonGizmosDataObjects::ObjectClipper& oc)
    : m_instance_idx(instance_idx_in)
{
    Cut cut(mo, instance_idx_in, cut_matrix);
    add_object(cut.perform_with_plane().front());

    const ModelVolumePtrs& volumes = model_object()->volumes;

    // split to parts
    for (int id = int(volumes.size())-1; id >= 0; id--)
        if (volumes[id]->is_splittable())
            volumes[id]->split(1);

    m_parts.clear();
    for (const ModelVolume* volume : volumes) {
        assert(volume != nullptr);
        m_parts.emplace_back(Part{GLModel(), MeshRaycaster(volume->mesh()), true, !volume->is_model_part()});
        m_parts.back().glmodel.set_color({ 0.f, 0.f, 1.f, 1.f });
        m_parts.back().glmodel.init_from(volume->mesh());

        // Now check whether this part is below or above the plane.
        Transform3d tr = (model_object()->instances[m_instance_idx]->get_matrix() * volume->get_matrix()).inverse();
        Vec3f pos = (tr * center).cast<float>();
        Vec3f norm = (tr.linear().inverse().transpose() * normal).cast<float>();
        for (const Vec3f& v : volume->mesh().its.vertices) {
            double p = (v - pos).dot(norm);
            if (std::abs(p) > EPSILON) {
                m_parts.back().selected = p > 0.;
                break;
            }
        }
    }

    // Now go through the contours and create a map from contours to parts.
    m_contour_points.clear();
    m_contour_to_parts.clear();
    m_debug_pts = std::vector<std::vector<Vec3d>>(m_parts.size(), std::vector<Vec3d>());
    if (std::vector<Vec3d> pts = oc.point_per_contour();! pts.empty()) {
        
        m_contour_to_parts.resize(pts.size());

        for (size_t pt_idx=0; pt_idx<pts.size(); ++pt_idx) {
            const Vec3d& pt = pts[pt_idx];
            const Vec3d dir = (center-pt).dot(normal) * normal;
            m_contour_points.emplace_back(dir + pt); // the result is in world coordinates.
            
            // Now, cast a ray from every contour point and see which volumes of the ones above
            // the plane are hit from the inside.
            for (size_t part_id=0; part_id<m_parts.size(); ++part_id) {
                const AABBMesh& aabb = m_parts[part_id].raycaster.get_aabb_mesh();
                const Transform3d& tr = (translation_transform(model_object()->instances[m_instance_idx]->get_offset()) * translation_transform(model_object()->volumes[part_id]->get_offset())).inverse();
                for (double d : {-1., 1.}) {
                    const Vec3d dir_mesh = d * tr.linear().inverse().transpose() * normal;
                    const Vec3d src = tr * (m_contour_points[pt_idx] + d*0.01 * normal);
                    AABBMesh::hit_result hit = aabb.query_ray_hit(src, dir_mesh);

                    m_debug_pts[part_id].emplace_back(src);

                    if (hit.is_inside()) {
                        // This part belongs to this point.
                        if (d == 1.)
                            m_contour_to_parts[pt_idx].first.emplace_back(part_id);
                        else
                            m_contour_to_parts[pt_idx].second.emplace_back(part_id);
                    }
                }
            }
        }

    }

    
    m_valid = true;
}

// In CutMode::cutTongueAndGroove we use PartSelection just for rendering
GLGizmoCut3D::PartSelection::PartSelection(const ModelObject* object, int instance_idx_in)
    : m_instance_idx (instance_idx_in)
{
    add_object(object);

    m_parts.clear();

    for (const ModelVolume* volume : object->volumes) {
        assert(volume != nullptr);
        m_parts.emplace_back(Part{ GLModel(), MeshRaycaster(volume->mesh()), true, !volume->is_model_part() });
        m_parts.back().glmodel.init_from(volume->mesh());

        // Now check whether this part is below or above the plane.
        m_parts.back().selected = volume->is_from_upper();
    }
    
    m_valid = true;
}

void GLGizmoCut3D::PartSelection::render(const Vec3d* normal, GLModel& sphere_model)
{
    if (! valid())
        return;

    const Camera&       camera          = wxGetApp().plater()->get_camera();

    if (GLShaderProgram* shader = wxGetApp().get_shader("gouraud_light")) {
        shader->start_using();
        shader->set_uniform("projection_matrix", camera.get_projection_matrix());
        shader->set_uniform("emission_factor", 0.f);

        // FIXME: Cache the transforms.

        const Vec3d         inst_offset     = model_object()->instances[m_instance_idx]->get_offset();
        const Transform3d   view_inst_matrix= camera.get_view_matrix() * translation_transform(inst_offset);

        const bool is_looking_forward = normal && camera.get_dir_forward().dot(*normal) < 0.05;

        for (size_t id=0; id<m_parts.size(); ++id) {
            if (!m_parts[id].is_modifier && normal && ((is_looking_forward && m_parts[id].selected) ||
                                                      (!is_looking_forward && !m_parts[id].selected)   ) )
                continue;
            shader->set_uniform("view_model_matrix", view_inst_matrix * model_object()->volumes[id]->get_matrix());
            if (m_parts[id].is_modifier) {
                glsafe(::glEnable(GL_BLEND));
                glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
            }
            m_parts[id].glmodel.set_color(m_parts[id].is_modifier ? MODIFIER_COLOR : (m_parts[id].selected ? UPPER_PART_COLOR : LOWER_PART_COLOR));
            m_parts[id].glmodel.render();
            if (m_parts[id].is_modifier)
                glsafe(::glDisable(GL_BLEND));
        }

        shader->stop_using();
    }



    // { // Debugging render:

    //     static int idx = -1;
    //     ImGui::Begin("DEBUG");
    //     for (int i=0; i<m_parts.size(); ++i)
    //         if (ImGui::Button(std::to_string(i).c_str()))
    //             idx = i;
    //     if (idx >= m_parts.size())
    //         idx = -1;
    //     ImGui::End();

    //     ::glDisable(GL_DEPTH_TEST);
    //     if (valid()) {
    //         for (size_t i=0; i<m_contour_points.size(); ++i) {
    //             const Vec3d& pt = m_contour_points[i];
    //             ColorRGBA col = ColorRGBA::GREEN();
            
    //             bool red = false;
    //             bool yellow = false;
    //             for (size_t j=0; j<m_contour_to_parts[i].first.size(); ++j) {
    //                 red |= m_parts[m_contour_to_parts[i].first[j]].selected;
    //                 yellow |= m_parts[m_contour_to_parts[i].second[j]].selected;
    //             }
    //             if (red)
    //                 col = ColorRGBA::RED();
    //             if (yellow)
    //                 col = ColorRGBA::YELLOW();
                    
    //             GLGizmoCut3D::render_model(sphere_model, col, camera.get_view_matrix() * translation_transform(pt));
    //         }
    //     }
        
    //     if (idx != -1) {
    //         render_model(m_parts[idx].glmodel, ColorRGBA::RED(), camera.get_view_matrix());
    //         for (const Vec3d& pt : m_debug_pts[idx]) {
    //             render_model(sphere_model, ColorRGBA::GREEN(), camera.get_view_matrix() * translation_transform(pt));
    //         }
    //     }
    //     ::glEnable(GL_DEPTH_TEST);
    // }
}


bool GLGizmoCut3D::PartSelection::is_one_object() const
{
    // In theory, the implementation could be just this:
    // return m_contour_to_parts.size() == m_ignored_contours.size();
    // However, this would require that the part-contour correspondence works
    // flawlessly. Because it is currently not always so for self-intersecting
    // objects, let's better check the parts itself:
    if (m_parts.size() < 2)
        return true;
    return std::all_of(m_parts.begin(), m_parts.end(), [this](const Part& part) {
        return part.is_modifier || part.selected == m_parts.front().selected;
    });
}

std::vector<Cut::Part> GLGizmoCut3D::PartSelection::get_cut_parts()
{
    std::vector<Cut::Part> parts;

    for (const auto& part : m_parts)
        parts.push_back({part.selected, part.is_modifier});

    return parts;
}


void GLGizmoCut3D::PartSelection::toggle_selection(const Vec2d& mouse_pos)
{
    // FIXME: Cache the transforms.
    const Camera& camera     = wxGetApp().plater()->get_camera();
    const Vec3d&  camera_pos = camera.get_position();

    Vec3f pos;
    Vec3f normal;

    std::vector<std::pair<size_t, double>> hits_id_and_sqdist;

    for (size_t id=0; id<m_parts.size(); ++id) {
//        const Vec3d volume_offset = model_object()->volumes[id]->get_offset();
        Transform3d tr = translation_transform(model_object()->instances[m_instance_idx]->get_offset()) * translation_transform(model_object()->volumes[id]->get_offset());
        if (m_parts[id].raycaster.unproject_on_mesh(mouse_pos, tr, camera, pos, normal)) {
            hits_id_and_sqdist.emplace_back(id, (camera_pos - tr*(pos.cast<double>())).squaredNorm());
        }
    }
    if (! hits_id_and_sqdist.empty()) {
        size_t id = std::min_element(hits_id_and_sqdist.begin(), hits_id_and_sqdist.end(),
            [](const std::pair<size_t, double>& a, const std::pair<size_t, double>& b) { return a.second < b.second; })->first;
        m_parts[id].selected = ! m_parts[id].selected;

        // And now recalculate the contours which should be ignored.
        m_ignored_contours.clear();
        size_t cont_id = 0;
        for (const auto& [parts_above, parts_below] : m_contour_to_parts) {
            for (size_t upper : parts_above) {
                bool upper_sel = m_parts[upper].selected;
                if (std::find_if(parts_below.begin(), parts_below.end(), [this, &upper_sel](const size_t& i) { return m_parts[i].selected == upper_sel; }) != parts_below.end()) {
                    m_ignored_contours.emplace_back(cont_id);
                    break;
                }
            }
            ++cont_id;
        }
    }
}

void GLGizmoCut3D::PartSelection::turn_over_selection()
{
    for (Part& part : m_parts)
        part.selected = !part.selected;
}

void GLGizmoCut3D::on_render()
{
    if (m_state == On) {
        // This gizmo is showing the object elevated. Tell the common
        // SelectionInfo object to lie about the actual shift.
        //m_c->selection_info()->set_use_shift(true);
    }

    // check objects visibility
    toggle_model_objects_visibility();

    update_clipper();

    init_picking_models();

    init_rendering_items();

    render_connectors();

    if (!m_connectors_editing)
        m_part_selection.render(nullptr, m_sphere.model);
    else
        m_part_selection.render(&m_cut_normal, m_sphere.model);

    render_clipper_cut();

    if (!m_hide_cut_plane && !m_connectors_editing) {
        render_cut_plane();
        render_cut_plane_grabbers();
    }

    render_cut_line();

    m_selection_rectangle.render(m_parent);
}

void GLGizmoCut3D::render_debug_input_window(float x)
{
    return;
    m_imgui->begin(wxString("DEBUG"));

    m_imgui->end();
/*
    static bool  hide_clipped  = false;
    static bool  fill_cut      = false;
    static float contour_width = 0.4f;

    m_imgui->checkbox(_L("Hide cut plane and grabbers"), m_hide_cut_plane);
    if (m_imgui->checkbox("hide_clipped", hide_clipped) && !hide_clipped)
        m_clp_normal = m_c->object_clipper()->get_clipping_plane()->get_normal();
    m_imgui->checkbox("fill_cut", fill_cut);
    m_imgui->slider_float("contour_width", &contour_width, 0.f, 3.f);
    if (auto oc = m_c->object_clipper())
        oc->set_behavior(hide_clipped || m_connectors_editing, fill_cut || m_connectors_editing, double(contour_width));
*/
    ImGui::PushItemWidth(0.5f * m_label_width);
    if (auto oc = m_c->object_clipper(); oc && m_imgui->slider_float("contour_width", &m_contour_width, 0.f, 3.f))
        oc->set_behavior(m_connectors_editing, m_connectors_editing, double(m_contour_width));

    ImGui::Separator();

    if (m_imgui->checkbox(("Render cut plane as disc"), m_cut_plane_as_circle))
        m_plane.reset();

    ImGui::PushItemWidth(0.5f * m_label_width);
    if (m_imgui->slider_float("cut_plane_radius_koef", &m_cut_plane_radius_koef, 1.f, 2.f))
        m_plane.reset();

    m_imgui->end();
}

void GLGizmoCut3D::unselect_all_connectors()
{
    std::fill(m_selected.begin(), m_selected.end(), false);
    m_selected_count = 0;
    validate_connector_settings();
}

void GLGizmoCut3D::select_all_connectors()
{
    std::fill(m_selected.begin(), m_selected.end(), true);
    m_selected_count = int(m_selected.size());
}

void GLGizmoCut3D::apply_selected_connectors(std::function<void(size_t idx)> apply_fn)
{
    for (size_t idx = 0; idx < m_selected.size(); idx++)
        if (m_selected[idx])
            apply_fn(idx);
    check_and_update_connectors_state();
    update_raycasters_for_picking_transform();
}

void GLGizmoCut3D::render_connectors_input_window(CutConnectors &connectors, float x, float y, float bottom_limit)
{
    // Connectors section

    ImGui::Separator();

    // WIP : Auto : Need to implement
    // m_imgui->text(_L("Mode"));
    // render_connect_mode_radio_button(CutConnectorMode::Auto);
    // render_connect_mode_radio_button(CutConnectorMode::Manual);

    ImGui::AlignTextToFramePadding();
    m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, m_labels_map["Connectors"]);

    m_imgui->disabled_begin(connectors.empty());
    ImGui::SameLine(m_label_width);
    const std::string act_name = _u8L("Remove connectors");
    if (render_reset_button("connectors", act_name)) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), act_name, UndoRedo::SnapshotType::GizmoAction);
        reset_connectors();
    }
    m_imgui->disabled_end();

    render_flip_plane_button(m_connectors_editing && connectors.empty());

    m_imgui->text(m_labels_map["Type"]);
    ImGuiWrapper::push_radio_style(m_parent.get_scale()); // ORCA
    bool type_changed = render_connect_type_radio_button(CutConnectorType::Plug);
    type_changed     |= render_connect_type_radio_button(CutConnectorType::Dowel);
    type_changed     |= render_connect_type_radio_button(CutConnectorType::Snap);
    if (type_changed)
        apply_selected_connectors([this, &connectors] (size_t idx) { connectors[idx].attribs.type = CutConnectorType(m_connector_type); });
    ImGuiWrapper::pop_radio_style();

    m_imgui->disabled_begin(m_connector_type != CutConnectorType::Plug);
        if (type_changed && m_connector_type == CutConnectorType::Dowel) {
            m_connector_style = int(CutConnectorStyle::Prism);
            apply_selected_connectors([this, &connectors](size_t idx) { connectors[idx].attribs.style = CutConnectorStyle(m_connector_style); });
        }
        if (render_combo(m_labels_map["Style"], m_connector_styles, m_connector_style, m_label_width, m_editing_window_width))
            apply_selected_connectors([this, &connectors](size_t idx) { connectors[idx].attribs.style = CutConnectorStyle(m_connector_style); });
    m_imgui->disabled_end();

    m_imgui->disabled_begin(m_connector_type == CutConnectorType::Snap);
        if (type_changed && m_connector_type == CutConnectorType::Snap) {
            m_connector_shape_id = int(CutConnectorShape::Circle);
            apply_selected_connectors([this, &connectors](size_t idx) { connectors[idx].attribs.shape = CutConnectorShape(m_connector_shape_id); });
        }
        if (render_combo(m_labels_map["Shape"], m_connector_shapes, m_connector_shape_id, m_label_width, m_editing_window_width))
            apply_selected_connectors([this, &connectors](size_t idx) { connectors[idx].attribs.shape = CutConnectorShape(m_connector_shape_id); });
    m_imgui->disabled_end();

    const float depth_min_value = m_connector_type == CutConnectorType::Snap ? m_connector_size : -0.1f;
    if (render_slider_double_input(m_labels_map["Depth"], m_connector_depth_ratio, m_connector_depth_ratio_tolerance, depth_min_value))
        apply_selected_connectors([this, &connectors](size_t idx) {
            if (m_connector_depth_ratio > 0)
                connectors[idx].height           = m_connector_depth_ratio;
            if (m_connector_depth_ratio_tolerance >= 0)
                connectors[idx].height_tolerance = m_connector_depth_ratio_tolerance;
        });

    if (render_slider_double_input(m_labels_map["Size"], m_connector_size, m_connector_size_tolerance))
        apply_selected_connectors([this, &connectors](size_t idx) {
            if (m_connector_size > 0)
                connectors[idx].radius           = 0.5f * m_connector_size;
            if (m_connector_size_tolerance >= 0)
                connectors[idx].radius_tolerance = 0.5f * m_connector_size_tolerance;
        });

    if (render_angle_input(m_labels_map["Rotation"], m_connector_angle, 0.f, 0.f, 180.f))
        apply_selected_connectors([this, &connectors](size_t idx) {
            connectors[idx].z_angle = m_connector_angle;
        });

    if (m_connector_type == CutConnectorType::Snap) {
        render_snap_specific_input(_u8L("Bulge"), _L("Bulge proportion related to radius"), m_snap_bulge_proportion, 0.15f, 5.f, 100.f * m_snap_space_proportion);
        render_snap_specific_input(_u8L("Space"), _L("Space proportion related to radius"), m_snap_space_proportion, 0.3f, 10.f, 50.f);
    }

    ImGui::Separator();

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 10.0f));
    float get_cur_y = ImGui::GetContentRegionMax().y + ImGui::GetFrameHeight() + y;
    show_tooltip_information(x, get_cur_y);

    float f_scale = m_parent.get_gizmos_manager().get_layout_scale();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f * f_scale));

    ImGui::SameLine();
    if (m_imgui->button(_L("Confirm connectors"))) {
        unselect_all_connectors();
        set_connectors_editing(false);
    }

    ImGui::SameLine(m_label_width + m_editing_window_width - m_imgui->calc_text_size(_L("Cancel")).x - m_imgui->get_style_scaling() * 8);

    if (m_imgui->button(_L("Cancel"))) {
        reset_connectors();
        set_connectors_editing(false);
    }

    ImGui::PopStyleVar(2);
}

void GLGizmoCut3D::render_build_size()
{
    double   koef     = m_imperial_units ? GizmoObjectManipulation::mm_to_in : 1.0;
    wxString unit_str = m_imperial_units ? _L("in") : _L("mm");
    Vec3d    tbb_sz   = m_transformed_bounding_box.size() * koef; // ORCA 

    ImGui::AlignTextToFramePadding();
    m_imgui->text(_L("Build Volume"));
    ImGui::SameLine(m_label_width);
    ImGui::Text("%.2f x %.2f x %.2f %s", tbb_sz.x(), tbb_sz.y(), tbb_sz.z(), unit_str.ToUTF8().data()); // ORCA use regular text color and simplify format
}

void GLGizmoCut3D::reset_cut_plane()
{
    m_angle_arc.reset();
    m_transformed_bounding_box = transformed_bounding_box(m_bb_center);
    set_center(m_bb_center);
    m_start_dragging_m = m_rotation_m = Transform3d::Identity();
    m_ar_plane_center  = m_plane_center;

    reset_cut_by_contours();
    m_parent.request_extra_frame();
}

void GLGizmoCut3D::invalidate_cut_plane()
{
    m_rotation_m    = Transform3d::Identity();
    m_plane_center  = Vec3d::Zero();
    m_min_pos       = Vec3d::Zero();
    m_max_pos       = Vec3d::Zero();
    m_bb_center     = Vec3d::Zero();
    m_center_offset = Vec3d::Zero();
}

void GLGizmoCut3D::set_connectors_editing(bool connectors_editing)
{
    if (m_connectors_editing == connectors_editing)
        return;

    m_connectors_editing = connectors_editing;
    update_raycasters_for_picking();

    m_c->object_clipper()->set_behavior(m_connectors_editing, m_connectors_editing, double(m_contour_width));

    m_parent.request_extra_frame();
}

void GLGizmoCut3D::flip_cut_plane()
{
    m_rotation_m = m_rotation_m * rotation_transform(PI * Vec3d::UnitX());
    m_transformed_bounding_box = transformed_bounding_box(m_plane_center, m_rotation_m);

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Flip cut plane"), UndoRedo::SnapshotType::GizmoAction);
    m_start_dragging_m = m_rotation_m;

    update_clipper();
    m_part_selection.turn_over_selection();

    if (CutMode(m_mode) == CutMode::cutTongueAndGroove)
        reset_cut_by_contours();
}

void GLGizmoCut3D::reset_cut_by_contours()
{
    m_part_selection = PartSelection();

    if (CutMode(m_mode) == CutMode::cutTongueAndGroove) {
        if (m_dragging || m_groove_editing || !has_valid_groove())
            return;
        process_contours();
    }
    else
        toggle_model_objects_visibility();
}

void GLGizmoCut3D::process_contours()
{
    const Selection& selection = m_parent.get_selection();
    const ModelObjectPtrs& model_objects = selection.get_model()->objects;

    const int instance_idx = selection.get_instance_idx();
    if (instance_idx < 0)
        return;
    const int object_idx = selection.get_object_idx();

    wxBusyCursor wait;

    if (CutMode(m_mode) == CutMode::cutTongueAndGroove) {
        if (has_valid_groove()) {
            Cut cut(model_objects[object_idx], instance_idx, get_cut_matrix(selection));
            const ModelObjectPtrs& new_objects = cut.perform_with_groove(m_groove, m_rotation_m, true);
            if (!new_objects.empty())
                m_part_selection = PartSelection(new_objects.front(), instance_idx);
        }
    }
    else {
        reset_cut_by_contours();
        m_part_selection = PartSelection(model_objects[object_idx], get_cut_matrix(selection), instance_idx, m_plane_center, m_cut_normal, *m_c->object_clipper());
    }

    toggle_model_objects_visibility();
}

void GLGizmoCut3D::render_flip_plane_button(bool disable_pred /*=false*/)
{
    ImGui::SameLine();

    if (m_hover_id == CutPlane)
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetColorU32(ImGuiCol_ButtonHovered));

    m_imgui->disabled_begin(disable_pred);
        if (m_imgui->button(_L("Flip cut plane")))
            flip_cut_plane();
    m_imgui->disabled_end();

    if (m_hover_id == CutPlane)
        ImGui::PopStyleColor();
}

void GLGizmoCut3D::add_vertical_scaled_interval(float interval)
{
    ImGui::GetCurrentWindow()->DC.CursorPos.y += m_imgui->scaled(interval);
}

void GLGizmoCut3D::add_horizontal_scaled_interval(float interval)
{
    ImGui::GetCurrentWindow()->DC.CursorPos.x += m_imgui->scaled(interval);
}

void GLGizmoCut3D::add_horizontal_shift(float shift)
{
    ImGui::GetCurrentWindow()->DC.CursorPos.x += shift;
}

void GLGizmoCut3D::render_color_marker(float size, const ImU32& color)
{
    const float radius = 0.5f * size;
    ImVec2 pos = ImGui::GetCurrentWindow()->DC.CursorPos;
    pos.x += radius;
    pos.y += 1.4f * radius;
    ImGui::GetCurrentWindow()->DrawList->AddNgonFilled(pos, radius, color, 6);
    m_imgui->text("  ");
    ImGui::SameLine();
}

void GLGizmoCut3D::render_groove_float_input(const std::string& label, float& in_val, const float& init_val, float& in_tolerance)
{
    bool is_changed{false};

    float val = in_val;
    float tolerance = in_tolerance;
    if (render_slider_double_input(label, val, tolerance, -0.1f, std::min(0.3f*in_val, 1.5f))) {
        if (m_imgui->get_last_slider_status().can_take_snapshot) {
            Plater::TakeSnapshot snapshot(wxGetApp().plater(), GUI::format("%1%: %2%", _u8L("Groove change"), label), UndoRedo::SnapshotType::GizmoAction);
            m_imgui->get_last_slider_status().invalidate_snapshot();
            m_groove_editing = true;
        }
        in_val = val;
        in_tolerance = tolerance;
        is_changed = true;
    }

    ImGui::SameLine();

    m_imgui->disabled_begin(is_approx(in_val, init_val) && is_approx(in_tolerance, 0.1f));
        const std::string act_name = _u8L("Reset");
        if (render_reset_button("##groove_" + label + act_name, act_name)) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), GUI::format("%1%: %2%", act_name, label), UndoRedo::SnapshotType::GizmoAction);
            in_val = init_val;
            in_tolerance = 0.1f;
            is_changed = true;
        }
    m_imgui->disabled_end();

    if (is_changed) {
        update_plane_model();
        reset_cut_by_contours();
    }

    if (m_is_slider_editing_done) {
        m_groove_editing = false;
        reset_cut_by_contours();
    }
}

bool GLGizmoCut3D::render_angle_input(const std::string& label, float& in_val, const float& init_val, float min_val, float max_val)
{
    // -------- [ ]
    // slider_with + item_in_gap + input_width
    double slider_with = 0.24 * m_editing_window_width; // m_control_width * 0.35;
    double item_in_gap = 0.01 * m_editing_window_width;
    double input_width = 0.29 * m_editing_window_width;

    ImGui::AlignTextToFramePadding();
    m_imgui->text(label);
    ImGui::SameLine(m_label_width);
    ImGui::PushItemWidth(slider_with);

    double left_width = m_label_width + slider_with + item_in_gap;

    bool is_changed{ false };

    float val = rad2deg(in_val);
    const float old_val = val;

    const std::string format = "%.0f°";
    m_imgui->bbl_slider_float_style("##angle_" + label, &val, min_val, max_val, format.c_str(), 1.f, true, from_u8(label));

    ImGui::SameLine(left_width);
    ImGui::PushItemWidth(input_width);
    ImGui::BBLDragFloat(("##angle_input_" + label).c_str(), &val, 0.05f, min_val, max_val, format.c_str());

    m_is_slider_editing_done |= m_imgui->get_last_slider_status().deactivated_after_edit;
    if (!is_approx(old_val, val)) {
        if (m_imgui->get_last_slider_status().can_take_snapshot) {
            // TRN: This is an entry in the Undo/Redo stack. The whole line will be 'Edited: (name of whatever was edited)'.
            Plater::TakeSnapshot snapshot(wxGetApp().plater(), GUI::format("%1%: %2%", _L("Edited"), label), UndoRedo::SnapshotType::GizmoAction);
            m_imgui->get_last_slider_status().invalidate_snapshot();
            if (m_mode == size_t(CutMode::cutTongueAndGroove))
                m_groove_editing = true;
        }
        in_val = deg2rad(val);
        is_changed = true;
    }

    ImGui::SameLine();

    m_imgui->disabled_begin(is_approx(in_val, init_val));
    const std::string act_name = _u8L("Reset");
    if (render_reset_button("##angle_" + label + act_name, act_name)) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), GUI::format("%1%: %2%", act_name, label), UndoRedo::SnapshotType::GizmoAction);
        in_val = init_val;
        is_changed = true;
    }
    m_imgui->disabled_end();

    return is_changed;
}

void GLGizmoCut3D::render_groove_angle_input(const std::string& label, float& in_val, const float& init_val, float min_val, float max_val)
{
    if (render_angle_input(label, in_val, init_val, min_val, max_val)) {
        update_plane_model();
        reset_cut_by_contours();
    }

    if (m_is_slider_editing_done) {
        m_groove_editing = false;
        reset_cut_by_contours();
    }
}

void GLGizmoCut3D::render_snap_specific_input(const std::string& label, const wxString& tooltip, float& in_val, const float& init_val, const float min_val, const float max_val)
{
    // -------- [ ]
    // slider_with + item_in_gap + input_width
    double slider_with = 0.24 * m_editing_window_width; // m_control_width * 0.35;
    double item_in_gap = 0.01 * m_editing_window_width;
    double input_width = 0.29 * m_editing_window_width;

    ImGui::AlignTextToFramePadding();
    m_imgui->text(label);
    ImGui::SameLine(m_label_width);
    ImGui::PushItemWidth(slider_with);

    double left_width = m_label_width + slider_with + item_in_gap;

    bool is_changed = false;
    const std::string format = "%.0f %%";

    float val = in_val * 100.f;
    const float old_val = val;
    m_imgui->bbl_slider_float_style("##snap_" + label, &val, min_val, max_val, format.c_str(), 1.f, true, tooltip);

    ImGui::SameLine(left_width);
    ImGui::PushItemWidth(input_width);
    ImGui::BBLDragFloat(("##snap_input_" + label).c_str(), &val, 0.05f, min_val, max_val, format.c_str());

    if (!is_approx(old_val, val)) {
        in_val = val * 0.01f;
        is_changed = true;
    }
    
    ImGui::SameLine();

    m_imgui->disabled_begin(is_approx(in_val, init_val));
    const std::string act_name = _u8L("Reset");
    if (render_reset_button("##snap_" + label + act_name, act_name)) {
        in_val = init_val;
        is_changed = true;
    }
    m_imgui->disabled_end();

    if (is_changed) {
        update_connector_shape();
        update_raycasters_for_picking();
    }
}

void GLGizmoCut3D::render_cut_plane_input_window(CutConnectors &connectors, float x, float y, float bottom_limit)
{
//    if (m_mode == size_t(CutMode::cutPlanar)) {
    CutMode mode = CutMode(m_mode);
    if (mode == CutMode::cutPlanar || mode == CutMode::cutTongueAndGroove) {
        const bool has_connectors = !connectors.empty();

        m_imgui->disabled_begin(has_connectors);
        if (render_cut_mode_combo())
            mode = CutMode(m_mode);
        m_imgui->disabled_end();

        render_build_size();

        ImGui::AlignTextToFramePadding();
        m_imgui->text(_L("Cut position"));
        ImGui::SameLine(m_label_width);
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

//        render_flip_plane_button();

        if (mode == CutMode::cutPlanar) {
            add_vertical_scaled_interval(0.75f);

            m_imgui->disabled_begin(!m_keep_upper || !m_keep_lower || m_keep_as_parts || (m_part_selection.valid() && m_part_selection.is_one_object()));
                if (m_imgui->button(has_connectors ? _L("Edit connectors") : _L("Add connectors")))
                    set_connectors_editing(true);
            m_imgui->disabled_end();

            ImGui::SameLine(1.5f * m_control_width);

            m_imgui->disabled_begin(is_cut_plane_init && !has_connectors);
                act_name = _u8L("Reset cut");
                if (m_imgui->button(wxString::FromUTF8(act_name), _L("Reset cutting plane and remove connectors"))) {
                    Plater::TakeSnapshot snapshot(wxGetApp().plater(), act_name, UndoRedo::SnapshotType::GizmoAction);
                    reset_cut_plane();
                    reset_connectors();
                }
            m_imgui->disabled_end();
        }
        else if (mode == CutMode::cutTongueAndGroove) {
            m_is_slider_editing_done = false;
            ImGui::Separator();
            m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, m_labels_map["Groove"] + ": ");
            render_groove_float_input(m_labels_map["Depth"], m_groove.depth, m_groove.depth_init, m_groove.depth_tolerance);
            render_groove_float_input(m_labels_map["Width"], m_groove.width, m_groove.width_init, m_groove.width_tolerance);
            render_groove_angle_input(m_labels_map["Flap Angle"], m_groove.flaps_angle, m_groove.flaps_angle_init, 30.f, 120.f);
            render_groove_angle_input(m_labels_map["Groove Angle"], m_groove.angle, m_groove.angle_init, 0.f, 15.f);
        }

        ImGui::Separator();

        // render "After Cut" section

        ImVec2 label_size;
        for (const wxString &label : {_L("Upper part"), _L("Lower part")}) {
            const ImVec2 text_size = ImGuiWrapper::calc_text_size(label);
            if (label_size.x < text_size.x)
                label_size.x = text_size.x;
            if (label_size.y < text_size.y)
                label_size.y = text_size.y;
        }

        const float marker_size = label_size.y;
        const float h_shift     = marker_size + label_size.x + m_imgui->scaled(2.f);

        auto render_part_action_line = [this, h_shift, marker_size, &connectors](const wxString &label, const wxString &suffix, bool &keep_part,
                                                                        bool &place_on_cut_part, bool &rotate_part) {
            bool keep = true;

            ImGui::AlignTextToFramePadding();
            render_color_marker(marker_size, ImGuiWrapper::to_ImU32(suffix == "##upper" ? UPPER_PART_COLOR : LOWER_PART_COLOR));
            m_imgui->text(label);

            ImGui::SameLine(h_shift);

            m_imgui->disabled_begin(!connectors.empty() || m_keep_as_parts);
            m_imgui->bbl_checkbox(_L("Keep") + suffix, connectors.empty() ? keep_part : keep);
            m_imgui->disabled_end();

            ImGui::SameLine();

            m_imgui->disabled_begin(!keep_part || m_keep_as_parts);
            if (m_imgui->bbl_checkbox(_L("Place on cut") + suffix, place_on_cut_part))
                rotate_part = false;
            ImGui::SameLine();
            if (m_imgui->bbl_checkbox(_L("Flip") + suffix, rotate_part))
                place_on_cut_part = false;
            m_imgui->disabled_end();
        };

        m_imgui->text(_L("After cut") + ": ");
        render_part_action_line(_L("Upper part"), "##upper", m_keep_upper, m_place_on_cut_upper, m_rotate_upper);
        render_part_action_line(_L("Lower part"), "##lower", m_keep_lower, m_place_on_cut_lower, m_rotate_lower);

        m_imgui->disabled_begin(has_connectors || m_part_selection.valid() || mode == CutMode::cutTongueAndGroove);

            if (m_part_selection.valid())
                m_keep_as_parts = false;

            m_imgui->bbl_checkbox(_L("Cut to parts"), m_keep_as_parts);
            if (m_keep_as_parts) {
                m_keep_upper = m_keep_lower = true;
                m_place_on_cut_upper = m_place_on_cut_lower = false;
                m_rotate_upper = m_rotate_lower = false;
            }
        m_imgui->disabled_end();
    }

    ImGui::Separator();

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 10.0f));
    float get_cur_y = ImGui::GetContentRegionMax().y + ImGui::GetFrameHeight() + y;
    show_tooltip_information(x, get_cur_y);

    float f_scale = m_parent.get_gizmos_manager().get_layout_scale();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f * f_scale));

    ImGui::SameLine();
    m_imgui->disabled_begin(!can_perform_cut());
        if(m_imgui->button(_L("Perform cut")))
            perform_cut(m_parent.get_selection());
    m_imgui->disabled_end();

    ImGui::PopStyleVar(2);
}

void GLGizmoCut3D::validate_connector_settings()
{
    if (m_connector_depth_ratio < 0.f)
        m_connector_depth_ratio = 3.f;
    if (m_connector_depth_ratio_tolerance < 0.f)
        m_connector_depth_ratio_tolerance = 0.1f;
    if (m_connector_size < 0.f)
        m_connector_size = 2.5f;
    if (m_connector_size_tolerance < 0.f)
        m_connector_size_tolerance = 0.f;
    if (m_connector_angle < 0.f || m_connector_angle > float(PI) )
        m_connector_angle = 0.f;

    if (m_connector_type == CutConnectorType::Undef)
        m_connector_type = CutConnectorType::Plug;
    if (m_connector_style == int(CutConnectorStyle::Undef))
        m_connector_style = int(CutConnectorStyle::Prism);
    if (m_connector_shape_id == int(CutConnectorShape::Undef))
        m_connector_shape_id = int(CutConnectorShape::Circle);
}

void GLGizmoCut3D::init_input_window_data(CutConnectors &connectors)
{
    m_imperial_units = wxGetApp().app_config->get_bool("use_inches");
    m_control_width  = m_imgui->get_font_size() * 9.f;

    m_editing_window_width = 1.45 * m_control_width + 11;

    if (m_connectors_editing && m_selected_count > 0) {
        float               depth_ratio             { UndefFloat };
        float               depth_ratio_tolerance   { UndefFloat };
        float               radius                  { UndefFloat };
        float               radius_tolerance        { UndefFloat };
        float               angle                   { UndefFloat };
        CutConnectorType    type                    { CutConnectorType::Undef };
        CutConnectorStyle   style                   { CutConnectorStyle::Undef };
        CutConnectorShape   shape                   { CutConnectorShape::Undef };

        bool is_init = false;
        for (size_t idx = 0; idx < m_selected.size(); idx++)
            if (m_selected[idx]) {
                const CutConnector& connector = connectors[idx];
                if (!is_init) {
                    depth_ratio             = connector.height;
                    depth_ratio_tolerance   = connector.height_tolerance;
                    radius                  = connector.radius;
                    radius_tolerance        = connector.radius_tolerance;
                    angle                   = connector.z_angle;
                    type                    = connector.attribs.type;
                    style                   = connector.attribs.style;
                    shape                   = connector.attribs.shape;

                    if (m_selected_count == 1)
                        break;
                    is_init = true;
                }
                else {
                    if (!is_approx(depth_ratio, connector.height))
                        depth_ratio         = UndefFloat;
                    if (!is_approx(depth_ratio_tolerance, connector.height_tolerance))
                        depth_ratio_tolerance = UndefFloat;
                    if (!is_approx(radius,connector.radius))
                        radius              = UndefFloat;
                    if (!is_approx(radius_tolerance, connector.radius_tolerance))
                        radius_tolerance    = UndefFloat;
                    if (!is_approx(angle, connector.z_angle))
                        angle               = UndefFloat;

                    if (type != connector.attribs.type)
                        type = CutConnectorType::Undef;
                    if (style != connector.attribs.style)
                        style = CutConnectorStyle::Undef;
                    if (shape != connector.attribs.shape)
                        shape = CutConnectorShape::Undef;
                }
            }

        m_connector_depth_ratio             = depth_ratio;
        m_connector_depth_ratio_tolerance   = depth_ratio_tolerance;
        m_connector_size                    = 2.f * radius;
        m_connector_size_tolerance          = 2.f * radius_tolerance;
        m_connector_type                    = type;
        m_connector_angle                   = angle;
        m_connector_style                   = int(style);
        m_connector_shape_id                = int(shape);
    }

    if (m_label_width == 0.f) {
        for (const auto& item : m_labels_map) {
            const float width = m_imgui->calc_text_size(item.second).x;
            if (m_label_width < width)
                m_label_width = width;
        }
        m_label_width += m_imgui->scaled(1.f);
        m_label_width += ImGui::GetStyle().WindowPadding.x;
    }
}

void GLGizmoCut3D::render_input_window_warning() const
{
    if (! m_invalid_connectors_idxs.empty()) {
        wxString out = /*wxString(ImGui::WarningMarkerSmall)*/ _L("Warning") + ": " + _L("Invalid connectors detected") + ":";
        if (m_info_stats.outside_cut_contour > size_t(0))
            out += "\n - " + format_wxstr(_L_PLURAL("%1$d connector is out of cut contour", "%1$d connectors are out of cut contour", m_info_stats.outside_cut_contour),
                                          m_info_stats.outside_cut_contour);
        if (m_info_stats.outside_bb > size_t(0))
            out += "\n - " + format_wxstr(_L_PLURAL("%1$d connector is out of object", "%1$d connectors are out of object", m_info_stats.outside_bb),
                                           m_info_stats.outside_bb);
        if (m_info_stats.is_overlap)
            out += "\n - " + _L("Some connectors are overlapped");
        m_imgui->text(out);
    }
    if (!m_keep_upper && !m_keep_lower)
        m_imgui->text(/*wxString(ImGui::WarningMarkerSmall)*/ _L("Warning") + ": " + _L("Select at least one object to keep after cutting."));
    if (!has_valid_contour())
        m_imgui->text(/*wxString(ImGui::WarningMarkerSmall)*/ _L("Warning") + ": " + _L("Cut plane is placed out of object"));
    else if (!has_valid_groove())
        m_imgui->text(/*wxString(ImGui::WarningMarkerSmall)*/ _L("Warning") + ": " + _L("Cut plane with groove is invalid"));
}

void GLGizmoCut3D::on_render_input_window(float x, float y, float bottom_limit)
{
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    GizmoImguiBegin(get_name(), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    CutConnectors& connectors = m_c->selection_info()->model_object()->cut_connectors;

    init_input_window_data(connectors);

    if (m_connectors_editing) // connectors mode
        render_connectors_input_window(connectors, x, y, bottom_limit); 
    else
        render_cut_plane_input_window(connectors, x, y, bottom_limit);

    render_input_window_warning();

    GizmoImguiEnd();

    // Orca
    ImGuiWrapper::pop_toolbar_style();

    if (!m_connectors_editing) // connectors mode
        render_debug_input_window(x);
}

void GLGizmoCut3D::show_tooltip_information(float x, float y)
{
    auto &shortcuts = m_connectors_editing ? m_shortcuts_connector : m_shortcuts_cut;

    float                      caption_max = 0.f;
    for (const auto &short_cut : shortcuts) {
        caption_max = std::max(caption_max, m_imgui->calc_text_size(short_cut.first).x);
    }

    ImTextureID normal_id = m_parent.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP);
    ImTextureID hover_id  = m_parent.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP_HOVER);

    caption_max += m_imgui->calc_text_size(std::string_view{": "}).x + 35.f;

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

        for (const auto &short_cut : shortcuts)
            draw_text_with_caption(short_cut.first + ": ", short_cut.second);
        ImGui::EndTooltip();
    }
    ImGui::PopStyleVar(2);
}

bool GLGizmoCut3D::is_outside_of_cut_contour(size_t idx, const CutConnectors& connectors, const Vec3d cur_pos)
{
    // check if connector pos is out of clipping plane
    if (m_c->object_clipper() && m_c->object_clipper()->is_projection_inside_cut(cur_pos) == -1) {
        m_info_stats.outside_cut_contour++;
        return true;
    }

    // check if connector bottom contour is out of clipping plane
    const CutConnector& cur_connector = connectors[idx];
    const CutConnectorShape shape = CutConnectorShape(cur_connector.attribs.shape);
    const int   sectorCount = shape == CutConnectorShape::Triangle  ? 3 :
                              shape == CutConnectorShape::Square    ? 4 :
                              shape == CutConnectorShape::Circle    ? 60: // supposably, 60 points are enough for conflict detection
                              shape == CutConnectorShape::Hexagon   ? 6 : 1 ;

    indexed_triangle_set mesh;
    auto& vertices = mesh.vertices;
    vertices.reserve(sectorCount + 1);

    float fa = 2 * PI / sectorCount;
    auto vec = Eigen::Vector2f(0, cur_connector.radius);
    for (float angle = 0; angle < 2.f * PI; angle += fa) {
        Vec2f p = Eigen::Rotation2Df(angle) * vec;
        vertices.emplace_back(Vec3f(p(0), p(1), 0.f));
    }
    its_transform(mesh, translation_transform(cur_pos) * m_rotation_m);

    for (const Vec3f& vertex : vertices) {
        if (m_c->object_clipper()) {
            int contour_idx = m_c->object_clipper()->is_projection_inside_cut(vertex.cast<double>());
            bool is_invalid = (contour_idx == -1);
            if (m_part_selection.valid() && ! is_invalid) {
                assert(contour_idx >= 0);
                const std::vector<size_t>& ignored = *(m_part_selection.get_ignored_contours_ptr());
                is_invalid = (std::find(ignored.begin(), ignored.end(), size_t(contour_idx)) != ignored.end());
            }
            if (is_invalid) {
                m_info_stats.outside_cut_contour++;
                return true;
            }
        }
    }

    return false;
}

bool GLGizmoCut3D::is_conflict_for_connector(size_t idx, const CutConnectors& connectors, const Vec3d cur_pos)
{
    if (is_outside_of_cut_contour(idx, connectors, cur_pos))
        return true;

    const CutConnector& cur_connector = connectors[idx];    

    const Transform3d matrix = translation_transform(cur_pos) * m_rotation_m *
                               scale_transform(Vec3f(cur_connector.radius, cur_connector.radius, cur_connector.height).cast<double>());
    const BoundingBoxf3 cur_tbb = m_shapes[cur_connector.attribs].model.get_bounding_box().transformed(matrix);

    // check if connector's bounding box is inside the object's bounding box
    if (!m_bounding_box.contains(cur_tbb)) {
        m_info_stats.outside_bb++;
        return true;
    }

    // check if connectors are overlapping 
    for (size_t i = 0; i < connectors.size(); ++i) {
        if (i == idx)
            continue;
        const CutConnector& connector = connectors[i];

        if ((connector.pos - cur_connector.pos).norm() < double(connector.radius + cur_connector.radius)) {
            m_info_stats.is_overlap = true;
            return true;
        }
    }

    return false;
}

void GLGizmoCut3D::check_and_update_connectors_state()
{
    m_info_stats.invalidate();
    m_invalid_connectors_idxs.clear();
    if (CutMode(m_mode) != CutMode::cutPlanar)
        return;
    const ModelObject* mo = m_c->selection_info()->model_object();
    auto inst_id = m_c->selection_info()->get_active_instance();
    if (inst_id < 0)
        return;
    const CutConnectors& connectors = mo->cut_connectors;
    const ModelInstance* mi = mo->instances[inst_id];
    const Vec3d& instance_offset = mi->get_offset();
    const double sla_shift       = double(m_c->selection_info()->get_sla_shift());

     for (size_t i = 0; i < connectors.size(); ++i) {
        const CutConnector& connector = connectors[i];
        Vec3d pos = connector.pos + instance_offset + sla_shift * Vec3d::UnitZ(); // recalculate connector position to world position
        if (is_conflict_for_connector(i, connectors, pos))
            m_invalid_connectors_idxs.emplace_back(i);
     }
}

void GLGizmoCut3D::toggle_model_objects_visibility()
{
    bool has_active_volume = false;
    std::vector<std::shared_ptr<SceneRaycasterItem>>* raycasters = m_parent.get_raycasters_for_picking(SceneRaycaster::EType::Volume);
    for (const std::shared_ptr<SceneRaycasterItem> &raycaster : *raycasters)
        if (raycaster->is_active()) {
            has_active_volume = true;
            break;
        }

    if (m_part_selection.valid() && has_active_volume)
        m_parent.toggle_model_objects_visibility(false);
    else if (!m_part_selection.valid() && !has_active_volume) {
        const Selection& selection = m_parent.get_selection();
        const ModelObjectPtrs& model_objects = selection.get_model()->objects;
        m_parent.toggle_model_objects_visibility(true, model_objects[selection.get_object_idx()], selection.get_instance_idx());        
    }
}

void GLGizmoCut3D::render_connectors()
{
    ::glEnable(GL_DEPTH_TEST);

    if (cut_line_processing() ||
        CutMode(m_mode) != CutMode::cutPlanar ||
        m_connector_mode == CutConnectorMode::Auto || !m_c->selection_info())
        return;

    const ModelObject* mo = m_c->selection_info()->model_object();
    auto inst_id = m_c->selection_info()->get_active_instance();
    if (inst_id < 0)
        return;
    const CutConnectors& connectors = mo->cut_connectors;
    if (connectors.size() != m_selected.size()) {
        // #ysFIXME
        clear_selection();
        m_selected.resize(connectors.size(), false);
    }

    ColorRGBA render_color = CONNECTOR_DEF_COLOR;

    const ModelInstance* mi = mo->instances[inst_id];
    const Vec3d& instance_offset = mi->get_offset();
    const double sla_shift       = double(m_c->selection_info()->get_sla_shift());

    const bool looking_forward = is_looking_forward();

    for (size_t i = 0; i < connectors.size(); ++i) {
        const CutConnector& connector = connectors[i];

        float height = connector.height;
        // recalculate connector position to world position
        Vec3d pos = connector.pos + instance_offset + sla_shift * Vec3d::UnitZ();

        // First decide about the color of the point.
        assert(std::is_sorted(m_invalid_connectors_idxs.begin(), m_invalid_connectors_idxs.end()));
        const bool conflict_connector = std::binary_search(m_invalid_connectors_idxs.begin(), m_invalid_connectors_idxs.end(), i);
        if (conflict_connector)
            render_color = CONNECTOR_ERR_COLOR;
        else // default connector color
            render_color = connector.attribs.type == CutConnectorType::Dowel ? DOWEL_COLOR          : PLAG_COLOR;

        if (!m_connectors_editing)
            render_color = CONNECTOR_ERR_COLOR;
        else if (size_t(m_hover_id - m_connectors_group_id) == i)
            render_color = conflict_connector ? HOVERED_ERR_COLOR :
                           connector.attribs.type == CutConnectorType::Dowel ? HOVERED_DOWEL_COLOR  : HOVERED_PLAG_COLOR;
        else if (m_selected[i])
            render_color = connector.attribs.type == CutConnectorType::Dowel ? SELECTED_DOWEL_COLOR : SELECTED_PLAG_COLOR;

        const Camera& camera = wxGetApp().plater()->get_camera();
        if (connector.attribs.type  == CutConnectorType::Dowel &&
            connector.attribs.style == CutConnectorStyle::Prism) {
            if (m_connectors_editing) {
                height = 0.05f;
                if (!looking_forward)
                    pos += 0.05 * m_clp_normal;
            }
            else {
                if (looking_forward)
                    pos -= static_cast<double>(height) * m_clp_normal;
                else
                    pos += static_cast<double>(height) * m_clp_normal;
                height *= 2;
            }
        }
        else if (!looking_forward)
            pos += 0.05 * m_clp_normal;

        const Transform3d view_model_matrix = camera.get_view_matrix() * translation_transform(pos) * m_rotation_m *
                                              rotation_transform(-connector.z_angle * Vec3d::UnitZ()) *
                                              scale_transform(Vec3f(connector.radius, connector.radius, height).cast<double>());

        render_model(m_shapes[connector.attribs].model, render_color, view_model_matrix);
    }
}

bool GLGizmoCut3D::can_perform_cut() const
{
    if (! m_invalid_connectors_idxs.empty() || (!m_keep_upper && !m_keep_lower) || m_connectors_editing)
        return false;

    if (CutMode(m_mode) == CutMode::cutTongueAndGroove)
        return has_valid_groove();

    if (m_part_selection.valid())
        return ! m_part_selection.is_one_object();

    return true;
}

bool GLGizmoCut3D::has_valid_groove() const
{
    if (CutMode(m_mode) != CutMode::cutTongueAndGroove)
        return true;

    const float flaps_width = -2.f * m_groove.depth / tan(m_groove.flaps_angle);
    if (flaps_width > m_groove.width)
        return false;

    const Selection& selection  = m_parent.get_selection();
    const auto&list = selection.get_volume_idxs();
    // is more volumes selected?
    if (list.empty())
        return false;

    const Transform3d cp_matrix = translation_transform(m_plane_center) * m_rotation_m;

    for (size_t id = 0; id < m_groove_vertices.size(); id += 2) {
        const Vec3d beg = cp_matrix * m_groove_vertices[id];
        const Vec3d end = cp_matrix * m_groove_vertices[id + 1];

        bool intersection = false;
        for (const unsigned int volume_idx : list) {
            const GLVolume* glvol = selection.get_volume(volume_idx);
            if (!glvol->is_modifier && 
                glvol->mesh_raycaster->intersects_line(beg, end - beg, glvol->world_matrix())) {
                intersection = true;
                break;
            }
        }
        if (!intersection)
            return false;
    }

    return true;
}

bool GLGizmoCut3D::has_valid_contour() const
{
    const auto clipper = m_c->object_clipper();
    return clipper && clipper->has_valid_contour();
}

void GLGizmoCut3D::apply_connectors_in_model(ModelObject* mo, int &dowels_count)
{
    if (CutMode(m_mode) == CutMode::cutTongueAndGroove)
        return;
    if (m_connector_mode == CutConnectorMode::Manual) {
        clear_selection();

        for (CutConnector&connector : mo->cut_connectors) {
            connector.rotation_m = m_rotation_m;

            if (connector.attribs.type == CutConnectorType::Dowel) {
                if (connector.attribs.style == CutConnectorStyle::Prism)
                    connector.height *= 2;
                dowels_count ++;
            }
            else {
                // calculate shift of the connector center regarding to the position on the cut plane
                connector.pos += m_cut_normal * 0.5 * double(connector.height);
            }
        }
        apply_cut_connectors(mo, _u8L("Connector"));
    }
}

Transform3d GLGizmoCut3D::get_cut_matrix(const Selection& selection)
{
    const int instance_idx = selection.get_instance_idx();
    const int object_idx = selection.get_object_idx();
    ModelObject* mo = selection.get_model()->objects[object_idx];
    if (!mo)
        return Transform3d::Identity();

    // m_cut_z is the distance from the bed. Subtract possible SLA elevation.
    const double sla_shift_z = selection.get_first_volume()->get_sla_shift_z();

    const Vec3d instance_offset = mo->instances[instance_idx]->get_offset();
    Vec3d cut_center_offset = m_plane_center - instance_offset;
    cut_center_offset[Z] -= sla_shift_z;

    return translation_transform(cut_center_offset) * m_rotation_m;
}

void update_object_cut_id(CutObjectBase& cut_id, ModelObjectCutAttributes attributes, const int dowels_count)
{
    // we don't save cut information, if result will not contains all parts of initial object
    if (!attributes.has(ModelObjectCutAttribute::KeepUpper) ||
        !attributes.has(ModelObjectCutAttribute::KeepLower) ||
        attributes.has(ModelObjectCutAttribute::InvalidateCutInfo))
        return;

    if (cut_id.id().invalid())
        cut_id.init();
    // increase check sum, if it's needed
    {
        int cut_obj_cnt = -1;
        if (attributes.has(ModelObjectCutAttribute::KeepUpper))    cut_obj_cnt++;
        if (attributes.has(ModelObjectCutAttribute::KeepLower))    cut_obj_cnt++;
        if (attributes.has(ModelObjectCutAttribute::CreateDowels)) cut_obj_cnt+= dowels_count;
        if (cut_obj_cnt > 0)
            cut_id.increase_check_sum(size_t(cut_obj_cnt));
    }
}

static void check_objects_after_cut(const ModelObjectPtrs& objects)
{
    std::vector<std::string> err_objects_names;
    for (const ModelObject* object : objects) {
        std::vector<std::string> connectors_names;
        connectors_names.reserve(object->volumes.size());
        for (const ModelVolume* vol : object->volumes)
            if (vol->cut_info.is_connector)
                connectors_names.push_back(vol->name);
        const size_t connectors_count = connectors_names.size();
        sort_remove_duplicates(connectors_names);
        if (connectors_count != connectors_names.size())
            err_objects_names.push_back(object->name);
    }
    if (err_objects_names.empty())
        return;

    wxString names = from_u8(err_objects_names[0]);
    for (size_t i = 1; i < err_objects_names.size(); i++)
        names += ", " + from_u8(err_objects_names[i]);
    WarningDialog(wxGetApp().plater(), format_wxstr("Objects(%1%) have duplicated connectors. "
                                "Some connectors may be missing in slicing result.\n"
                                "Please report to PrusaSlicer team in which scenario this issue happened.\n"
                                "Thank you.", names)).ShowModal();
}

void synchronize_model_after_cut(Model& model, const CutObjectBase& cut_id)
{
    for (ModelObject* obj : model.objects)
        if (obj->is_cut() && obj->cut_id.has_same_id(cut_id) && !obj->cut_id.is_equal(cut_id))
            obj->cut_id.copy(cut_id);
}

void GLGizmoCut3D::perform_cut(const Selection& selection)
{
    if (!can_perform_cut())
        return;
    const int instance_idx = selection.get_instance_idx();
    const int object_idx = selection.get_object_idx();

    wxCHECK_RET(instance_idx >= 0 && object_idx >= 0, "GLGizmoCut: Invalid object selection");

    Plater* plater = wxGetApp().plater();
    ModelObject* mo = plater->model().objects[object_idx];
    if (!mo)
        return;

    // deactivate CutGizmo and than perform a cut
    m_parent.reset_all_gizmos();

    // perform cut
    {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Cut by Plane"));

        // This shall delete the part selection class and deallocate the memory.
        ScopeGuard part_selection_killer([this]() { m_part_selection = PartSelection(); });

        const bool cut_with_groove = CutMode(m_mode) == CutMode::cutTongueAndGroove;
        const bool cut_by_contour = !cut_with_groove && m_part_selection.valid();

        ModelObject* cut_mo = cut_by_contour ? m_part_selection.model_object() : nullptr;
        if (cut_mo)
            cut_mo->cut_connectors = mo->cut_connectors;
        else
            cut_mo = mo;

        int dowels_count = 0;
        const bool has_connectors = !mo->cut_connectors.empty();
        // update connectors pos as offset of its center before cut performing
        apply_connectors_in_model(cut_mo , dowels_count);

        wxBusyCursor wait;

        ModelObjectCutAttributes attributes = only_if(has_connectors ? true : m_keep_upper, ModelObjectCutAttribute::KeepUpper) |
                                              only_if(has_connectors ? true : m_keep_lower, ModelObjectCutAttribute::KeepLower) |
                                              only_if(has_connectors ? false : m_keep_as_parts, ModelObjectCutAttribute::KeepAsParts) |
                                              only_if(m_place_on_cut_upper, ModelObjectCutAttribute::PlaceOnCutUpper) |
                                              only_if(m_place_on_cut_lower, ModelObjectCutAttribute::PlaceOnCutLower) |
                                              only_if(m_rotate_upper, ModelObjectCutAttribute::FlipUpper) |
                                              only_if(m_rotate_lower, ModelObjectCutAttribute::FlipLower) |
                                              only_if(dowels_count > 0, ModelObjectCutAttribute::CreateDowels) |
                                              only_if(!has_connectors && !cut_with_groove && cut_mo->cut_id.id().invalid(), ModelObjectCutAttribute::InvalidateCutInfo);

        // update cut_id for the cut object in respect to the attributes
        update_object_cut_id(cut_mo->cut_id, attributes, dowels_count);

        Cut cut(cut_mo, instance_idx, get_cut_matrix(selection), attributes);
        const ModelObjectPtrs& new_objects = cut_by_contour    ? cut.perform_by_contour(m_part_selection.get_cut_parts(), dowels_count):
                                             cut_with_groove   ? cut.perform_with_groove(m_groove, m_rotation_m) :
                                                                 cut.perform_with_plane();

        // fix_non_manifold_edges
#ifdef HAS_WIN10SDK
        if (is_windows10()) {
            bool is_showed_dialog = false;
            bool user_fix_model   = false;
            for (size_t i = 0; i < new_objects.size(); i++) {
                for (size_t j = 0; j < new_objects[i]->volumes.size(); j++) {
                    if (its_num_open_edges(new_objects[i]->volumes[j]->mesh().its) > 0) {
                        if (!is_showed_dialog) {
                            is_showed_dialog = true;
                            MessageDialog dlg(nullptr, _L("non-manifold edges be caused by cut tool, do you want to fix it now?"), "", wxYES | wxCANCEL);
                            int           ret = dlg.ShowModal();
                            if (ret == wxID_YES) {
                                user_fix_model = true;
                            }
                        }
                        if (!user_fix_model) {
                            break;
                        }
                        // model_name
                        std::vector<std::string> succes_models;
                        // model_name     failing reason
                        std::vector<std::pair<std::string, std::string>> failed_models;
                        auto                                             plater = wxGetApp().plater();
                        auto fix_and_update_progress = [this, plater](ModelObject *model_object, const int vol_idx, const string &model_name, ProgressDialog &progress_dlg,
                                                                      std::vector<std::string> &succes_models, std::vector<std::pair<std::string, std::string>> &failed_models) {
                            wxString msg = _L("Repairing model object");
                            msg += ": " + from_u8(model_name) + "\n";
                            std::string res;
                            if (!fix_model_by_win10_sdk_gui(*model_object, vol_idx, progress_dlg, msg, res)) return false;
                            return true;
                        };
                        ProgressDialog progress_dlg(_L("Repairing model object"), "", 100, find_toplevel_parent(plater), wxPD_AUTO_HIDE | wxPD_APP_MODAL | wxPD_CAN_ABORT, true);

                        auto model_name = new_objects[i]->name;
                        if (!fix_and_update_progress(new_objects[i], j, model_name, progress_dlg, succes_models, failed_models)) {
                            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "run fix_and_update_progress error";
                        };
                    };
                }
            }
        }
 #endif
        check_objects_after_cut(new_objects);

        // save cut_id to post update synchronization
        const CutObjectBase cut_id = cut_mo->cut_id;

        // update cut results on plater and in the model 
        plater->apply_cut_object_to_model(object_idx, new_objects);

        synchronize_model_after_cut(plater->model(), cut_id);
    }
}

// Unprojects the mouse position on the mesh and saves hit point and normal of the facet into pos_and_normal
// Return false if no intersection was found, true otherwise.
bool GLGizmoCut3D::unproject_on_cut_plane(const Vec2d& mouse_position, Vec3d& pos, Vec3d& pos_world, bool respect_contours/* = true*/)
{
    const float sla_shift = m_c->selection_info()->get_sla_shift();

    const ModelObject* mo = m_c->selection_info()->model_object();
    const ModelInstance* mi = mo->instances[m_c->selection_info()->get_active_instance()];
    const Camera& camera = wxGetApp().plater()->get_camera();

    // Calculate intersection with the clipping plane.
    const ClippingPlane* cp = m_c->object_clipper()->get_clipping_plane(true);
    Vec3d point;
    Vec3d direction;
    Vec3d hit;
    MeshRaycaster::line_from_mouse_pos(mouse_position, Transform3d::Identity(), camera, point, direction);
    Vec3d normal = -cp->get_normal().cast<double>();
    double den = normal.dot(direction);
    if (den != 0.) {
        double t = (-cp->get_offset() - normal.dot(point))/den;
        hit = (point + t * direction);
    } else
        return false;

    // Now check if the hit is not obscured by a selected part on this side of the plane.
    // FIXME: This would be better solved by remembering which contours are active. We will
    // probably need that anyway because there is not other way to find out which contours
    // to render. If you want to uncomment it, fix it first. It does not work yet.
    /*for (size_t id = 0; id < m_part_selection.parts.size(); ++id) {
        if (! m_part_selection.parts[id].selected) {
            Vec3f pos, normal;
            const ModelObject* model_object = m_part_selection.model_object;
            const Vec3d volume_offset = m_part_selection.model_object->volumes[id]->get_offset();
            Transform3d tr = model_object->instances[m_part_selection.instance_idx]->get_matrix() * model_object->volumes[id]->get_matrix();
            if (m_part_selection.parts[id].raycaster.unproject_on_mesh(mouse_position, tr, camera, pos, normal))
                return false;
        }
    }*/

    if (respect_contours)
    {
        // Do not react to clicks outside a contour (or inside a contour that is ignored)
        int cont_id = m_c->object_clipper()->is_projection_inside_cut(hit);
        if (cont_id == -1)
            return false;
        if (m_part_selection.valid()) {
            const std::vector<size_t>& ign = *m_part_selection.get_ignored_contours_ptr();
            if (std::find(ign.begin(), ign.end(), cont_id) != ign.end())
                return false;
        }    
    }
    

    // recalculate hit to object's local position
    Vec3d hit_d = hit;
    hit_d -= mi->get_offset();
    hit_d[Z] -= sla_shift;

    // Return both the point and the facet normal.
    pos = hit_d;
    pos_world = hit;

    return true; 
}

void GLGizmoCut3D::clear_selection()
{
    m_selected.clear();
    m_selected_count = 0;
}

void GLGizmoCut3D::reset_connectors()
{
    m_c->selection_info()->model_object()->cut_connectors.clear();
    update_raycasters_for_picking();
    clear_selection();
    check_and_update_connectors_state();
}

void GLGizmoCut3D::init_connector_shapes()
{
    for (const CutConnectorType& type : {CutConnectorType::Dowel, CutConnectorType::Plug, CutConnectorType::Snap})
        for (const CutConnectorStyle& style : {CutConnectorStyle::Frustum, CutConnectorStyle::Prism}) {
            if (type == CutConnectorType::Dowel && style == CutConnectorStyle::Frustum)
                continue;
            for (const CutConnectorShape& shape : {CutConnectorShape::Circle, CutConnectorShape::Hexagon, CutConnectorShape::Square, CutConnectorShape::Triangle}) {
                if (type == CutConnectorType::Snap && shape != CutConnectorShape::Circle)
                    continue;
                const CutConnectorAttributes attribs = { type, style, shape };
                indexed_triangle_set its = get_connector_mesh(attribs);
                m_shapes[attribs].model.init_from(its);
                m_shapes[attribs].mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(std::move(its)));
            }
        }
}

void GLGizmoCut3D::update_connector_shape()
{
    CutConnectorAttributes attribs = { m_connector_type, CutConnectorStyle(m_connector_style), CutConnectorShape(m_connector_shape_id) };

    if (m_connector_type == CutConnectorType::Snap) {
        indexed_triangle_set its = get_connector_mesh(attribs);
        m_shapes[attribs].reset();
        m_shapes[attribs].model.init_from(its);
        m_shapes[attribs].mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(std::move(its)));

        //const indexed_triangle_set its = get_connector_mesh(attribs);
        //m_connector_mesh.clear();
        //m_connector_mesh = TriangleMesh(its);
    }


}

bool GLGizmoCut3D::cut_line_processing() const
{
    return !m_line_beg.isApprox(Vec3d::Zero());
}

void GLGizmoCut3D::discard_cut_line_processing()
{
    m_line_beg = m_line_end = Vec3d::Zero();
}

bool GLGizmoCut3D::process_cut_line(SLAGizmoEventType action, const Vec2d& mouse_position)
{
    const Camera& camera = wxGetApp().plater()->get_camera();

    Vec3d pt;
    Vec3d dir;
    MeshRaycaster::line_from_mouse_pos(mouse_position, Transform3d::Identity(), camera, pt, dir);
    dir.normalize();
    pt += dir; // Move the pt along dir so it is not clipped.

    if (action == SLAGizmoEventType::LeftDown && !cut_line_processing()) {
        m_line_beg = pt;
        m_line_end = pt;
        on_unregister_raycasters_for_picking();
        return true;
    }

    if (cut_line_processing()) {
        if (CutMode(m_mode) == CutMode::cutTongueAndGroove)
            m_groove_editing = true;
        reset_cut_by_contours();

        m_line_end = pt;
        if (action == SLAGizmoEventType::LeftDown || action == SLAGizmoEventType::LeftUp) {
            Vec3d line_dir = m_line_end - m_line_beg;
            if (line_dir.norm() < 3.0)
                return true;

            Vec3d cross_dir = line_dir.cross(dir).normalized();
            Eigen::Quaterniond q;
            Transform3d m = Transform3d::Identity();
            m.matrix().block(0, 0, 3, 3) = q.setFromTwoVectors(Vec3d::UnitZ(), cross_dir).toRotationMatrix();

            const Vec3d new_plane_center = m_bb_center + cross_dir * cross_dir.dot(pt - m_bb_center);
            // update transformed bb
            const auto new_tbb = transformed_bounding_box(new_plane_center, m);
            const GLVolume* first_volume = m_parent.get_selection().get_first_volume();
            Vec3d instance_offset = first_volume->get_instance_offset();
            instance_offset[Z] += first_volume->get_sla_shift_z();

            const Vec3d trans_center_pos = m.inverse() * (new_plane_center - instance_offset) + new_tbb.center();
            if (new_tbb.contains(trans_center_pos)) {
                Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Cut by line"), UndoRedo::SnapshotType::GizmoAction);
                m_transformed_bounding_box = new_tbb;
                set_center(new_plane_center);
                m_start_dragging_m = m_rotation_m = m;
                m_ar_plane_center = m_plane_center;
            }

            m_angle_arc.reset();
            discard_cut_line_processing();

            if (CutMode(m_mode) == CutMode::cutTongueAndGroove) {
                m_groove_editing = false;
                reset_cut_by_contours();
            }
        }
        else if (action == SLAGizmoEventType::Moving)
            this->set_dirty();
        return true;
    }
    return false;
}

bool GLGizmoCut3D::add_connector(CutConnectors& connectors, const Vec2d& mouse_position)
{
    if (!m_connectors_editing)
        return false;

    Vec3d pos;
    Vec3d pos_world;
    if (unproject_on_cut_plane(mouse_position.cast<double>(), pos, pos_world)) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Add connector"), UndoRedo::SnapshotType::GizmoAction);
        unselect_all_connectors();

        connectors.emplace_back(pos, m_rotation_m,
                                m_connector_size * 0.5f, m_connector_depth_ratio,
                                m_connector_size_tolerance * 0.5f, m_connector_depth_ratio_tolerance,
                                m_connector_angle,
                                CutConnectorAttributes( CutConnectorType(m_connector_type),
                                                        CutConnectorStyle(m_connector_style),
                                                        CutConnectorShape(m_connector_shape_id)));
        m_selected.push_back(true);
        m_selected_count = 1;
        assert(m_selected.size() == connectors.size());
        update_raycasters_for_picking();
        m_parent.set_as_dirty();
        check_and_update_connectors_state();

        return true;
    }
    return false;
}

bool GLGizmoCut3D::delete_selected_connectors(CutConnectors& connectors)
{
    if (connectors.empty())
        return false;

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Delete connector"), UndoRedo::SnapshotType::GizmoAction);

    // remove  connectors
    for (int i = int(connectors.size()) - 1; i >= 0; i--)
        if (m_selected[i])
            connectors.erase(connectors.begin() + i);
    // remove selections
    m_selected.erase(std::remove_if(m_selected.begin(), m_selected.end(), [](const auto& selected) {
        return selected; }), m_selected.end());
    m_selected_count = 0;

    assert(m_selected.size() == connectors.size());
    update_raycasters_for_picking();
    m_parent.set_as_dirty();
    check_and_update_connectors_state();
    return true;
}

void GLGizmoCut3D::select_connector(int idx, bool select)
{
    m_selected[idx] = select;
    if (select)
        ++m_selected_count;
    else
        --m_selected_count;
}

bool GLGizmoCut3D::is_selection_changed(bool alt_down, bool shift_down)
{
    if (m_hover_id >= m_connectors_group_id) {
        if (alt_down)
            select_connector(m_hover_id - m_connectors_group_id, false);
        else {
            if (!shift_down)
                unselect_all_connectors();
            select_connector(m_hover_id - m_connectors_group_id, true);
        }
        return true;
    }
    return false;
}

void GLGizmoCut3D::process_selection_rectangle(CutConnectors &connectors)
{
    GLSelectionRectangle::EState rectangle_status = m_selection_rectangle.get_state();

    ModelObject* mo          = m_c->selection_info()->model_object();
    int          active_inst = m_c->selection_info()->get_active_instance();

    // First collect positions of all the points in world coordinates.
    Transformation trafo = mo->instances[active_inst]->get_transformation();
    trafo.set_offset(trafo.get_offset() + double(m_c->selection_info()->get_sla_shift()) * Vec3d::UnitZ());

    std::vector<Vec3d> points;
    for (const CutConnector&connector : connectors)
        points.push_back(connector.pos + trafo.get_offset());

    // Now ask the rectangle which of the points are inside.
    std::vector<unsigned int> points_idxs = m_selection_rectangle.contains(points);
    m_selection_rectangle.stop_dragging();

    for (size_t idx : points_idxs)
        select_connector(int(idx), rectangle_status == GLSelectionRectangle::EState::Select);
}

bool GLGizmoCut3D::gizmo_event(SLAGizmoEventType action, const Vec2d& mouse_position, bool shift_down, bool alt_down, bool control_down)
{
    if (is_dragging() || m_connector_mode == CutConnectorMode::Auto)
        return false;

    if ( (m_hover_id < 0 || m_hover_id == CutPlane) && shift_down &&  ! m_connectors_editing &&
        (action == SLAGizmoEventType::LeftDown || action == SLAGizmoEventType::LeftUp || action == SLAGizmoEventType::Moving) )
        return process_cut_line(action, mouse_position);

    if (!m_keep_upper || !m_keep_lower)
        return false;

    if (!m_connectors_editing)
        return false;

    CutConnectors& connectors = m_c->selection_info()->model_object()->cut_connectors;

    if (action == SLAGizmoEventType::LeftDown) {
        if (shift_down || alt_down) {
            // left down with shift - show the selection rectangle:
            if (m_hover_id == -1)
                m_selection_rectangle.start_dragging(mouse_position, shift_down ? GLSelectionRectangle::EState::Select : GLSelectionRectangle::EState::Deselect);
        }
        else
            // If there is no selection and no hovering, add new point
            if (m_hover_id == -1 && !shift_down && !alt_down)
                if (!add_connector(connectors, mouse_position))
                    m_ldown_mouse_position = mouse_position;
        return true;
    }

    if (action == SLAGizmoEventType::LeftUp && !m_selection_rectangle.is_dragging()) {
        if ((m_ldown_mouse_position - mouse_position).norm() < 5.)
            unselect_all_connectors();
        return is_selection_changed(alt_down, shift_down);
    }

    // left up with selection rectangle - select points inside the rectangle:
    if ((action == SLAGizmoEventType::LeftUp || action == SLAGizmoEventType::ShiftUp || action == SLAGizmoEventType::AltUp) && m_selection_rectangle.is_dragging()) {
        // Is this a selection or deselection rectangle?
        process_selection_rectangle(connectors);
        return true;
    }

    // dragging the selection rectangle:
    if (action == SLAGizmoEventType::Dragging) {
        if (m_selection_rectangle.is_dragging()) {
            m_selection_rectangle.dragging(mouse_position);
            return true;
        }
        return false;
    }
    
    if (action == SLAGizmoEventType::RightDown && !shift_down) {
        // If any point is in hover state, this should initiate its move - return control back to GLCanvas:
        if (m_hover_id < m_connectors_group_id)
            return false;
        unselect_all_connectors();
        select_connector(m_hover_id - m_connectors_group_id, true);
        return delete_selected_connectors(connectors);
    }
    
    if (action == SLAGizmoEventType::Delete)
        return delete_selected_connectors(connectors);

    if (action == SLAGizmoEventType::SelectAll) {
        select_all_connectors();
        return true;
    }

    return false;
}

CommonGizmosDataID GLGizmoCut3D::on_get_requirements() const {
    return CommonGizmosDataID(
                int(CommonGizmosDataID::SelectionInfo)
              | int(CommonGizmosDataID::InstancesHider)
              | int(CommonGizmosDataID::ObjectClipper));
}

void GLGizmoCut3D::data_changed(bool is_serializing) 
{
    update_bb();
    if (auto oc = m_c->object_clipper())
        oc->set_behavior(m_connectors_editing, m_connectors_editing, double(m_contour_width));
}




indexed_triangle_set GLGizmoCut3D::get_connector_mesh(CutConnectorAttributes connector_attributes)
{
    indexed_triangle_set connector_mesh;

    int   sectorCount{ 1 };
    switch (CutConnectorShape(connector_attributes.shape)) {
    case CutConnectorShape::Triangle:
        sectorCount = 3;
        break;
    case CutConnectorShape::Square:
        sectorCount = 4;
        break;
    case CutConnectorShape::Circle:
        sectorCount = 360;
        break;
    case CutConnectorShape::Hexagon:
        sectorCount = 6;
        break;
    default:
        break;
    }

    if (connector_attributes.type == CutConnectorType::Snap)
        connector_mesh = its_make_snap(1.0, 1.0, m_snap_space_proportion, m_snap_bulge_proportion);
    else if (connector_attributes.style == CutConnectorStyle::Prism)
        connector_mesh = its_make_cylinder(1.0, 1.0, (2 * PI / sectorCount));
    else if (connector_attributes.type == CutConnectorType::Plug)
        connector_mesh = its_make_frustum(1.0, 1.0, (2 * PI / sectorCount));
    else
        connector_mesh = its_make_frustum_dowel(1.0, 1.0, sectorCount);

    return connector_mesh;
}

void GLGizmoCut3D::apply_cut_connectors(ModelObject* mo, const std::string& connector_name)
{
    if (mo->cut_connectors.empty())
        return;

    using namespace Geometry;

    size_t connector_id = mo->cut_id.connectors_cnt();
    for (const CutConnector& connector : mo->cut_connectors) {
        TriangleMesh mesh = TriangleMesh(get_connector_mesh(connector.attribs));
        // Mesh will be centered when loading.
        ModelVolume* new_volume = mo->add_volume(std::move(mesh), ModelVolumeType::NEGATIVE_VOLUME);

        // Transform the new modifier to be aligned inside the instance
        new_volume->set_transformation(translation_transform(connector.pos) * connector.rotation_m *
            rotation_transform(-connector.z_angle * Vec3d::UnitZ()) *
            scale_transform(Vec3f(connector.radius, connector.radius, connector.height).cast<double>()));

        new_volume->cut_info = { connector.attribs.type, connector.radius_tolerance, connector.height_tolerance };
        new_volume->name = connector_name + "-" + std::to_string(++connector_id);
    }
    mo->cut_id.increase_connectors_cnt(mo->cut_connectors.size());

    // delete all connectors
    mo->cut_connectors.clear();
}



} // namespace GUI
} // namespace Slic3r
