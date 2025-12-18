#include "slic3r/GUI/ImGuiWrapper.hpp"
#include <imgui/imgui_internal.h>

#include "GizmoObjectManipulation.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
//#include "I18N.hpp"
#include "GLGizmosManager.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/Utils/UndoRedo.hpp"
#include "libslic3r/AppConfig.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/Geometry.hpp"
#include "slic3r/GUI/Selection.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/MainFrame.hpp"

#include <boost/algorithm/string.hpp>

#define MAX_NUM 9999.99
#define MAX_SIZE std::string_view{"9999.99"}

namespace Slic3r
{
namespace GUI
{

const double GizmoObjectManipulation::in_to_mm = 25.4;
const double GizmoObjectManipulation::mm_to_in = 0.0393700787;
const double GizmoObjectManipulation::oz_to_g = 28.34952;
const double GizmoObjectManipulation::g_to_oz = 0.035274;

// Helper function to be used by drop to bed button. Returns lowest point of this
// volume in world coordinate system.
static double get_volume_min_z(const GLVolume* volume)
{
    const Transform3f& world_matrix = volume->world_matrix().cast<float>();

    // need to get the ModelVolume pointer
    const ModelObject* mo = wxGetApp().model().objects[volume->composite_id.object_id];
    const ModelVolume* mv = mo->volumes[volume->composite_id.volume_id];
    const TriangleMesh& hull = mv->get_convex_hull();

    float min_z = std::numeric_limits<float>::max();
    for (const stl_vertex& vert : hull.its.vertices) {
        min_z = std::min(min_z, Vec3f::UnitZ().dot(world_matrix * vert));
    }
    return min_z;
}

GizmoObjectManipulation::GizmoObjectManipulation(GLCanvas3D& glcanvas)
    : m_glcanvas(glcanvas)
{
    m_imperial_units = wxGetApp().app_config->get("use_inches") == "1";
    m_new_unit_string = m_imperial_units ? L("in") : L("mm");

    const wxString shift                   = _L("Shift+");
    const wxString alt                     = GUI::shortkey_alt_prefix();
    const wxString ctrl                    = GUI::shortkey_ctrl_prefix();

    m_desc_move["part_selection_caption"] = alt + _L("Left mouse button");
    m_desc_move["part_selection"]         = _L("Part selection");
    m_desc_move["snap_step_caption"] = shift + _L("Left mouse button");
    m_desc_move["snap_step"]        = _L("Fixed step drag");

    m_desc_rotate["part_selection_caption"] = alt + _L("Left mouse button");
    m_desc_rotate["part_selection"]         = _L("Part selection");

    m_desc_scale["part_selection_caption"] = alt + _L("Left mouse button");
    m_desc_scale["part_selection"]         = _L("Part selection");
    m_desc_scale["snap_step_caption"]      = shift + _L("Left mouse button");
    m_desc_scale["snap_step"]              = _L("Fixed step drag");
    m_desc_scale["single_sided_caption"] = ctrl + _L("Left mouse button");
    m_desc_scale["single_sided"]         = _L("Single sided scaling");
}

void GizmoObjectManipulation::UpdateAndShow(const bool show)
{
	if (show) {
        this->set_dirty();
		this->update_if_dirty();
	}
}

void GizmoObjectManipulation::update_ui_from_settings()
{
    if (m_imperial_units != (wxGetApp().app_config->get("use_inches") == "1")) {
        m_imperial_units = wxGetApp().app_config->get("use_inches") == "1";

        m_new_unit_string = m_imperial_units ? L("in") : L("mm");

        update_buffered_value();
    }
}
void delete_negative_sign(Vec3d& value) {
    for (size_t i = 0; i < value.size(); i++) {
        if (abs(value[i]) < 0.001)
            value[i] = 0.f;
    }
}

void GizmoObjectManipulation::update_settings_value(const Selection &selection)
{
	m_new_move_label_string   = L("Position");
    m_new_rotate_label_string = L("Rotate (relative)");
    m_new_rotation            = Vec3d::Zero();
    m_new_absolute_rotation   = Vec3d::Zero();
    m_new_scale_label_string  = L("Scale ratios");

    ObjectList* obj_list = wxGetApp().obj_list();
    if (selection.is_single_full_instance()) {
        // all volumes in the selection belongs to the same instance, any of them contains the needed instance data, so we take the first one
        const GLVolume* volume = selection.get_first_volume();
        m_new_position = volume->get_instance_offset();
        auto rotation = volume->get_instance_transformation().get_rotation_by_quaternion();
        m_new_absolute_rotation = rotation * (180. / M_PI);
        delete_negative_sign(m_new_absolute_rotation);
        if (is_world_coordinates()) {//for move and rotate
            m_new_size     = selection.get_bounding_box_in_current_reference_system().first.size();
            m_unscale_size = selection.get_unscaled_instance_bounding_box().size();
            m_new_scale    = m_new_size.cwiseQuotient(m_unscale_size) * 100.0;
		}
        else {//if (is_local_coordinates()) {//for scale
            auto tran      = selection.get_first_volume()->get_instance_transformation();
            m_new_position = tran.get_matrix().inverse() * cs_center;
            if (is_instance_coordinates()) {
                m_new_position = Vec3d::Zero();
            }
            m_new_size  = selection.get_bounding_box_in_current_reference_system().first.size();
            m_unscale_size = selection.get_full_unscaled_instance_local_bounding_box().size();
            m_new_scale    = m_new_size.cwiseQuotient(m_unscale_size) * 100.0;
		}

        m_new_enabled  = true;
        // BBS: change "Instance Operations" to "Object Operations"
        m_new_title_string = L("Object Operations");
    }
    else if (selection.is_single_full_object() && obj_list->is_selected(itObject)) {
        const BoundingBoxf3& box = selection.get_bounding_box();
        m_new_position = box.center();
        m_new_scale    = Vec3d(100., 100., 100.);
        m_new_size     = selection.get_bounding_box_in_current_reference_system().first.size();
		m_new_scale_label_string  = L("Scale");
        m_new_enabled  = true;
        m_new_title_string = L("Object Operations");
    } else if (selection.is_single_volume_or_modifier()) {
        const GLVolume *volume = selection.get_first_volume();
        auto            rotation = volume->get_volume_transformation().get_rotation_by_quaternion();
        m_new_absolute_rotation  = rotation * (180. / M_PI);
        delete_negative_sign(m_new_absolute_rotation);
        if (is_world_coordinates()) {//for move and rotate
            const Geometry::Transformation trafo(volume->world_matrix());
            const Vec3d &offset = trafo.get_offset();
            m_new_position            = offset;
            m_new_scale               = Vec3d(100.0, 100.0, 100.0);
            m_unscale_size            = selection.get_bounding_box_in_current_reference_system().first.size();
            m_new_size                = selection.get_bounding_box_in_current_reference_system().first.size();
        } else if (is_local_coordinates()) {//for scale
            m_new_position            = Vec3d::Zero();
            m_new_scale               = volume->get_volume_scaling_factor() * 100.0;
            m_unscale_size            = selection.get_bounding_box_in_current_reference_system().first.size();
            m_new_size                = selection.get_bounding_box_in_current_reference_system().first.size();
        } else {
            m_new_position            = volume->get_volume_offset();
            m_new_scale_label_string  = L("Scale");
            m_new_scale               = Vec3d(100.0, 100.0, 100.0);
            m_unscale_size            = selection.get_bounding_box_in_current_reference_system().first.size();
            m_new_size                = selection.get_bounding_box_in_current_reference_system().first.size();
        }
        m_new_enabled = true;
        m_new_title_string = L("Volume Operations");
    } else if (obj_list->is_connectors_item_selected() || obj_list->multiple_selection() || obj_list->is_selected(itInstanceRoot)) {
        reset_settings_value();
		m_new_move_label_string   = L("Translate");
		m_new_scale_label_string  = L("Scale");
        m_unscale_size            = selection.get_bounding_box_in_current_reference_system().first.size();
        m_new_size                = selection.get_bounding_box_in_current_reference_system().first.size();
        m_new_enabled  = true;
        m_new_title_string = L("Group Operations");
    } else if (selection.is_wipe_tower()) {
        const BoundingBoxf3 &box = selection.get_bounding_box();
        m_new_position           = box.center();
    }
	else {
        // No selection, reset the cache.
//		assert(selection.is_empty());
		reset_settings_value();
	}
}

void GizmoObjectManipulation::update_buffered_value()
{
    if (this->m_imperial_units)
        m_buffered_position = this->m_new_position * this->mm_to_in;
    else
        m_buffered_position = this->m_new_position;

    m_buffered_rotation = this->m_new_rotation;
    m_buffered_absolute_rotation = this->m_new_absolute_rotation;
    m_buffered_scale = this->m_new_scale;

    if (this->m_imperial_units)
        m_buffered_size = this->m_new_size * this->mm_to_in;
    else
        m_buffered_size = this->m_new_size;
}

void GizmoObjectManipulation::update_if_dirty()
{
    if (! m_dirty)
        return;

    const Selection &selection = m_glcanvas.get_selection();
    this->update_settings_value(selection);
    this->update_buffered_value();

    auto update_label = [](wxString &label_cache, const std::string &new_label) {
        wxString new_label_localized = _(new_label) + ":";
        if (label_cache != new_label_localized) {
            label_cache = new_label_localized;
        }
    };
    update_label(m_cache.move_label_string,   m_new_move_label_string);
    update_label(m_cache.rotate_label_string, m_new_rotate_label_string);
    update_label(m_cache.rotate_label_string, m_new_rotate_label_string);
    update_label(m_cache.scale_label_string,  m_new_scale_label_string);

    enum ManipulationEditorKey
    {
        mePosition = 0,
        meRotation,
        meScale,
        meSize
    };

    for (int i = 0; i < 3; ++ i) {
        auto update = [this, i](Vec3d &cached, Vec3d &cached_rounded,  const Vec3d &new_value) {
			//wxString new_text = double_to_string(new_value(i), 2);
			double new_rounded = round(new_value(i)*100)/100.0;
			//new_text.ToDouble(&new_rounded);
			if (std::abs(cached_rounded(i) - new_rounded) > EPSILON) {
				cached_rounded(i) = new_rounded;
                //const int id = key_id*3+i;
                //if (m_imperial_units && (key_id == mePosition || key_id == meSize))
                //    new_text = double_to_string(new_value(i)*mm_to_in, 2);
                //if (id >= 0) m_editors[id]->set_value(new_text);
            }
			cached(i) = new_value(i);
		};
        update(m_cache.position, m_cache.position_rounded,  m_new_position);
        update(m_cache.scale,    m_cache.scale_rounded,     m_new_scale);
        update(m_cache.size,     m_cache.size_rounded,      m_new_size);
        update(m_cache.rotation, m_cache.rotation_rounded,  m_new_rotation);
        update(m_cache.absolute_rotation, m_cache.absolute_rotation_rounded, m_new_absolute_rotation);
    }

    update_reset_buttons_visibility();
    //update_mirror_buttons_visibility();

    m_dirty = false;
}

void GizmoObjectManipulation::update_reset_buttons_visibility()
{
    const Selection& selection = m_glcanvas.get_selection();

    if (selection.is_single_full_instance() || selection.is_single_volume_or_modifier()) {
        const GLVolume *               volume = selection.get_first_volume();

        Vec3d rotation;
        Vec3d scale;
        double min_z = 0.;

        if (selection.is_single_full_instance()) {
            rotation = volume->get_instance_rotation();
            scale = volume->get_instance_scaling_factor();
        }
        else {
            rotation = volume->get_volume_rotation();
            scale = volume->get_volume_scaling_factor();
            min_z = get_volume_min_z(volume);
        }
        m_show_clear_rotation = !rotation.isApprox(m_init_rotation);
        m_show_reset_0_rotation = !rotation.isApprox(Vec3d::Zero());
        m_show_clear_scale = (m_cache.scale / 100.0f - Vec3d::Ones()).norm() > 0.001;
        m_show_drop_to_bed = (std::abs(min_z) > EPSILON);
    }
}


void GizmoObjectManipulation::reset_settings_value()
{
    m_new_position = Vec3d::Zero();
    m_new_rotation = Vec3d::Zero();
    m_new_absolute_rotation = Vec3d::Zero();
    m_new_scale = Vec3d::Ones() * 100.;
    m_new_size = Vec3d::Zero();
    m_new_enabled = false;
    // no need to set the dirty flag here as this method is called from update_settings_value(),
    // which is called from update_if_dirty(), which resets the dirty flag anyways.
//    m_dirty = true;
}

void GizmoObjectManipulation::change_position_value(int axis, double value)
{
    if (std::abs(m_cache.position_rounded(axis) - value) < EPSILON)
        return;

    Vec3d position = m_cache.position;
    position(axis) = value;

    Selection& selection = m_glcanvas.get_selection();
    selection.setup_cache();
    TransformationType trafo_type;
    trafo_type.set_relative();
    switch (m_coordinates_type) {
    case ECoordinatesType::Instance: {
        trafo_type.set_instance();
        break;
    }
    case ECoordinatesType::Local: {
        trafo_type.set_local();
        break;
    }
    default: {
        break;
    }
    }
    selection.translate(position - m_cache.position, trafo_type);
    wxGetApp().plater()->take_snapshot("Set Position", UndoRedo::SnapshotType::GizmoAction);
    m_glcanvas.do_move("");

    m_cache.position = position;
	m_cache.position_rounded(axis) = DBL_MAX;
    this->UpdateAndShow(true);
}

void GizmoObjectManipulation::change_rotation_value(int axis, double value)
{
    if (std::abs(m_cache.rotation_rounded(axis) - value) < EPSILON)
        return;

    Vec3d rotation = m_cache.rotation;
    rotation(axis) = value;

    Selection& selection = m_glcanvas.get_selection();

    TransformationType transformation_type;
    transformation_type.set_relative();
    if (selection.is_single_full_instance())
        transformation_type.set_independent();
    if (is_local_coordinates())
        transformation_type.set_local();
    if (is_instance_coordinates())
        transformation_type.set_instance();

    selection.setup_cache();
    selection.rotate((M_PI / 180.0) * (transformation_type.absolute() ? rotation : rotation - m_cache.rotation), transformation_type);
    wxGetApp().plater()->take_snapshot(_u8L("Set Orientation"), UndoRedo::SnapshotType::GizmoAction);
    m_glcanvas.do_rotate("");

    m_cache.rotation = rotation;
	m_cache.rotation_rounded(axis) = DBL_MAX;
    this->UpdateAndShow(true);
}

void GizmoObjectManipulation::change_absolute_rotation_value(int axis, double value) {
    if (std::abs(m_cache.absolute_rotation_rounded(axis) - value) < EPSILON)
        return;

    Vec3d absolute_rotation = m_cache.absolute_rotation;
    absolute_rotation(axis) = value;

    Selection &selection = m_glcanvas.get_selection();
    TransformationType transformation_type;
    transformation_type.set_relative();
    if (selection.is_single_full_instance())
        transformation_type.set_independent();
    if (is_local_coordinates())
        transformation_type.set_local();
    if (is_instance_coordinates())
        transformation_type.set_instance();

    selection.setup_cache();
    auto diff_rotation = transformation_type.absolute() ? absolute_rotation : absolute_rotation - m_cache.absolute_rotation;
    selection.rotate((M_PI / 180.0) * diff_rotation, transformation_type);
    wxGetApp().plater()->take_snapshot("set absolute orientation", UndoRedo::SnapshotType::GizmoAction);
    m_glcanvas.do_rotate("");

    m_cache.absolute_rotation               = absolute_rotation;
    m_cache.absolute_rotation_rounded(axis) = DBL_MAX;
    this->UpdateAndShow(true);
}

void GizmoObjectManipulation::change_scale_value(int axis, double value)
{
    if (value <= 0.0)
        return;
    if (std::abs(m_cache.scale_rounded(axis) - value) < EPSILON) {
        m_show_clear_scale = (m_cache.scale / 100.0f - Vec3d::Ones()).norm() > 0.001;
        return;
    }
    Vec3d scale     = m_cache.scale;
    scale(axis)     = value;
    Vec3d ref_scale = m_cache.scale;
    const Selection &selection = m_glcanvas.get_selection();
    if (selection.is_single_volume_or_modifier()) {
        scale     = scale.cwiseQuotient(ref_scale); // scale / ref_scale
        ref_scale =  Vec3d::Ones();
    } else if (selection.is_single_full_instance())
        ref_scale = 100 * Vec3d::Ones();
    this->do_scale(axis, scale.cwiseQuotient(ref_scale));

    m_cache.scale = scale;
	m_cache.scale_rounded(axis) = DBL_MAX;
	this->UpdateAndShow(true);
}


void GizmoObjectManipulation::change_size_value(int axis, double value)
{
    if (value <= 0.0)
        return;
    if (std::abs(m_cache.size_rounded(axis) - value) < EPSILON)
        return;

    Vec3d size = m_cache.size;
    size(axis) = value;

    const Selection& selection = m_glcanvas.get_selection();

    Vec3d ref_size = m_cache.size;
    if (selection.is_single_volume_or_modifier()) {
        size     = size.cwiseQuotient(ref_size);
        ref_size = Vec3d::Ones();
    } else if (selection.is_single_full_instance()) {
        if (is_world_coordinates())
            ref_size = selection.get_full_unscaled_instance_bounding_box().size();
        else
            ref_size = selection.get_full_unscaled_instance_local_bounding_box().size();
    }

    this->do_scale(axis, size.cwiseQuotient(ref_size));

    m_cache.size = size;
	m_cache.size_rounded(axis) = DBL_MAX;
	this->UpdateAndShow(true);
}

void GizmoObjectManipulation::do_scale(int axis, const Vec3d &scale) const
{
    Selection& selection = m_glcanvas.get_selection();

    TransformationType transformation_type;
    if (is_local_coordinates())
        transformation_type.set_local();
    else if (is_instance_coordinates())
        transformation_type.set_instance();
    if (selection.is_single_volume_or_modifier() && !is_local_coordinates())
        transformation_type.set_relative();

    Vec3d scaling_factor = m_uniform_scale ? scale(axis) * Vec3d::Ones() : scale;
    limit_scaling_ratio(scaling_factor);

    selection.setup_cache();
    selection.scale(scaling_factor, transformation_type);
    m_glcanvas.do_scale(L("Set Scale"));
}


void GizmoObjectManipulation::limit_scaling_ratio(Vec3d &scaling_factor) const{
    for (size_t i = 0; i < scaling_factor.size(); i++) { // range protect //scaling_factor too big has problem
        if (scaling_factor[i] * m_unscale_size[i] > MAX_NUM) {
            scaling_factor[i] = MAX_NUM / m_unscale_size[i];
        }
    }
}

void GizmoObjectManipulation::on_change(const std::string &opt_key, int axis, double new_value)
{
    if (!m_cache.is_valid())
        return;

    if (m_imperial_units && (opt_key == "position" || opt_key == "size"))
        new_value *= in_to_mm;

    if (opt_key == "position")
        change_position_value(axis, new_value);
    else if (opt_key == "rotation")
        change_rotation_value(axis, new_value);
    else if (opt_key == "absolute_rotation")
        change_absolute_rotation_value(axis, new_value);
    else if (opt_key == "scale")
        change_scale_value(axis, new_value);
    else if (opt_key == "size")
        change_size_value(axis, new_value);
}

bool GizmoObjectManipulation::render_combo(
    ImGuiWrapper *imgui_wrapper, const std::string &label, const std::vector<std::string> &lines, size_t &selection_idx, float label_width, float item_width)
{
    ImGui::AlignTextToFramePadding();
    imgui_wrapper->text(label);
    ImGui::SameLine(label_width);
    ImGui::PushItemWidth(item_width);

    size_t selection_out = selection_idx;

    const char *selected_str = (selection_idx >= 0 && selection_idx < int(lines.size())) ? lines[selection_idx].c_str() : "";
    if (ImGui::BBLBeginCombo(("##" + label).c_str(), selected_str, 0)) {
        for (size_t line_idx = 0; line_idx < lines.size(); ++line_idx) {
            ImGui::PushID(int(line_idx));
            if (ImGui::Selectable("", line_idx == selection_idx)) selection_out = line_idx;

            ImGui::SameLine();
            ImGui::Text("%s", lines[line_idx].c_str());
            ImGui::PopID();
        }

        ImGui::EndCombo();
    }

    bool is_changed = selection_idx != selection_out;
    selection_idx   = selection_out;

    return is_changed;
}

void GizmoObjectManipulation::reset_position_value()
{
    Selection& selection = m_glcanvas.get_selection();

    if (selection.is_single_volume() || selection.is_single_modifier()) {
        GLVolume* volume = const_cast<GLVolume*>(selection.get_first_volume());
        volume->set_volume_offset(Vec3d::Zero());
    }
    else if (selection.is_single_full_instance()) {
        for (unsigned int idx : selection.get_volume_idxs()) {
            GLVolume* volume = const_cast<GLVolume*>(selection.get_volume(idx));
            volume->set_instance_offset(Vec3d::Zero());
        }
    }
    else
        return;

    // Copy position values from GLVolumes into Model (ModelInstance / ModelVolume), trigger background processing.
    wxGetApp().plater()->take_snapshot(_u8L("Reset Position"), UndoRedo::SnapshotType::GizmoAction);
    m_glcanvas.do_move("");

    UpdateAndShow(true);
}

void GizmoObjectManipulation::reset_rotation_value(bool reset_relative)
{
    Selection &selection = m_glcanvas.get_selection();
    selection.setup_cache();
    if (selection.is_single_volume_or_modifier()) {
        GLVolume *               vol    = const_cast<GLVolume *>(selection.get_first_volume());
        Geometry::Transformation trafo  = vol->get_volume_transformation();
        if (reset_relative) {
            auto offset = trafo.get_offset();
            trafo.set_matrix(m_init_rotation_scale_tran);
            trafo.set_offset(offset);
        }
        else {
            trafo.reset_rotation();
        }
        vol->set_volume_transformation(trafo);
    } else if (selection.is_single_full_instance()) {
        Geometry::Transformation trafo  = selection.get_first_volume()->get_instance_transformation();
        if (reset_relative) {
            auto offset = trafo.get_offset();
            trafo.set_matrix(m_init_rotation_scale_tran);
            trafo.set_offset(offset);
        } else {
            trafo.reset_rotation();
        }
        for (unsigned int idx : selection.get_volume_idxs()) {
            const_cast<GLVolume *>(selection.get_volume(idx))->set_instance_transformation(trafo);
        }
    } else
        return;
    // Synchronize instances/volumes.

    selection.synchronize_unselected_instances(Selection::SyncRotationType::RESET);
    selection.synchronize_unselected_volumes();
    // Copy rotation values from GLVolumes into Model (ModelInstance / ModelVolume), trigger background processing.
    m_glcanvas.do_rotate(L("Reset Rotation"));

    UpdateAndShow(true);
}

void GizmoObjectManipulation::reset_scale_value()
{
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Reset scale");

    change_scale_value(0, 100.);
    change_scale_value(1, 100.);
    change_scale_value(2, 100.);
}

void GizmoObjectManipulation::set_uniform_scaling(const bool use_uniform_scale)
{
    if (!use_uniform_scale)
        // Recalculate cached values at this panel, refresh the screen.
        this->UpdateAndShow(true);

    m_uniform_scale = use_uniform_scale;
    set_dirty();
}

void GizmoObjectManipulation::set_coordinates_type(ECoordinatesType type)
{
    /*if (wxGetApp().get_mode() == comSimple)
        type = ECoordinatesType::World;*/

    if (m_coordinates_type == type) return;

    m_coordinates_type = type;
    //m_word_local_combo->SetSelection((int) m_coordinates_type);
    this->UpdateAndShow(true);
    GLCanvas3D *canvas = wxGetApp().plater()->canvas3D();
    canvas->get_gizmos_manager().update_data();
    canvas->set_as_dirty();
    canvas->request_extra_frame();

}

static const char* label_values[3][3] = {
{ "##position_x", "##position_y", "##position_z"},
{ "##rotation_x", "##rotation_y", "##rotation_z"},
{ "##absolute_rotation_x", "##absolute_rotation_y", "##absolute_rotation_z"}
};

static const char* label_scale_values[2][3] = {
{ "##scale_x", "##scale_y", "##scale_z"},
{ "##size_x", "##size_y", "##size_z"}
};

bool GizmoObjectManipulation::reset_button(ImGuiWrapper *imgui_wrapper, float caption_max, float unit_size, float space_size, float end_text_size)
{
    bool        pressed   = false;
    ImTextureID normal_id = m_glcanvas.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_RESET);
    ImTextureID hover_id  = m_glcanvas.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_RESET_HOVER);

    float  scale       = m_glcanvas.get_scale();
    #ifdef WIN32
        int dpi = get_dpi_for_window(wxGetApp().GetTopWindow());
        scale *= (float) dpi / (float) DPI_DEFAULT;
    #endif // WIN32
    ImVec2 button_size = ImVec2(16 * scale, 16 * scale); // ORCA: Use exact resolution will prevent blur on icon

    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);

    pressed = ImGui::ImageButton3(normal_id, hover_id, button_size);

    ImGui::PopStyleVar(1);
    return pressed;
}

bool GizmoObjectManipulation::reset_zero_button(ImGuiWrapper *imgui_wrapper, float caption_max, float unit_size, float space_size, float end_text_size)
{
    bool        pressed   = false;
    ImTextureID normal_id = m_glcanvas.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_RESET_ZERO);
    ImTextureID hover_id  = m_glcanvas.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_RESET_ZERO_HOVER);

    float  scale       = m_glcanvas.get_scale();
    #ifdef WIN32
        int dpi = get_dpi_for_window(wxGetApp().GetTopWindow());
        scale *= (float) dpi / (float) DPI_DEFAULT;
    #endif // WIN32
    ImVec2 button_size = ImVec2(16 * scale, 16 * scale); // ORCA: Use exact resolution will prevent blur on icon

    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);

    pressed = ImGui::ImageButton3(normal_id, hover_id, button_size);

    ImGui::PopStyleVar(1);
    return pressed;
}

 float GizmoObjectManipulation::max_unit_size(int number, Vec3d &vec1, Vec3d &vec2,std::string str)
 {
     if (number <= 1) return -1;
     Vec3d vec[2] = {vec1, vec2};
     float nuit_max[4] = {0};
     float vec_max = 0, unit_size = 0;

     for (int i = 0; i < number; i++)
     {
         char buf[3][64] = {0};
         float buf_size[3] = {0};
         for (int j = 0; j < 3; j++) {
             ImGui::DataTypeFormatString(buf[j], IM_ARRAYSIZE(buf[j]), ImGuiDataType_Double, (void *) &vec[i][j], "%.2f");
             buf_size[j]  = ImGui::CalcTextSize(buf[j]).x;
             vec_max = std::max(buf_size[j], vec_max);
             nuit_max[i]  = vec_max;
         }
         unit_size = std::max(nuit_max[i], unit_size);
     }

     return unit_size + 8.0;
 }

 bool GizmoObjectManipulation::bbl_checkbox(const wxString &label, bool &value)
{
     bool result;
     bool b_value = value;
     if (b_value) {
         ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.00f, 0.68f, 0.26f, 1.00f));
         ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.00f, 0.68f, 0.26f, 1.00f));
         ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.00f, 0.68f, 0.26f, 1.00f));
     }
     auto label_utf8 = into_u8(label);
     result          = ImGui::BBLCheckbox(label_utf8.c_str(), &value);

     if (b_value) { ImGui::PopStyleColor(3); }
     return result;
}

void GizmoObjectManipulation::show_move_tooltip_information(ImGuiWrapper *imgui_wrapper, float caption_max, float x, float y)
{
    ImTextureID normal_id = m_glcanvas.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP);
    ImTextureID hover_id  = m_glcanvas.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP_HOVER);

    caption_max += imgui_wrapper->calc_text_size(": "sv).x + 35.f;

    float  scale       = m_glcanvas.get_scale();
    #ifdef WIN32
        int dpi = get_dpi_for_window(wxGetApp().GetTopWindow());
        scale *= (float) dpi / (float) DPI_DEFAULT;
    #endif // WIN32
    ImVec2 button_size = ImVec2(25 * scale, 25 * scale); // ORCA: Use exact resolution will prevent blur on icon
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0, ImGui::GetStyle().FramePadding.y});
    ImGui::ImageButton3(normal_id, hover_id, button_size);

    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip2(ImVec2(x, y));
        auto draw_text_with_caption = [this, &imgui_wrapper,& caption_max](const wxString &caption, const wxString &text) {
            imgui_wrapper->text_colored(ImGuiWrapper::COL_ACTIVE, caption);
            ImGui::SameLine(caption_max);
            imgui_wrapper->text_colored(ImGuiWrapper::COL_WINDOW_BG, text);
        };

        for (const auto &t : std::array<std::string, 2>{"part_selection", "snap_step"})
            draw_text_with_caption(m_desc_move.at(t + "_caption") + ": ", m_desc_move.at(t));
        ImGui::EndTooltip();
    }
    ImGui::PopStyleVar(2);
}

void GizmoObjectManipulation::show_rotate_tooltip_information(ImGuiWrapper *imgui_wrapper, float caption_max, float x, float y)
{
    ImTextureID normal_id = m_glcanvas.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP);
    ImTextureID hover_id  = m_glcanvas.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP_HOVER);

    caption_max += imgui_wrapper->calc_text_size(": "sv).x + 35.f;

    float  scale       = m_glcanvas.get_scale();
    #ifdef WIN32
        int dpi = get_dpi_for_window(wxGetApp().GetTopWindow());
        scale *= (float) dpi / (float) DPI_DEFAULT;
    #endif // WIN32
    ImVec2 button_size = ImVec2(25 * scale, 25 * scale); // ORCA: Use exact resolution will prevent blur on icon
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0, ImGui::GetStyle().FramePadding.y});
    ImGui::ImageButton3(normal_id, hover_id, button_size);

    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip2(ImVec2(x, y));
        auto draw_text_with_caption = [this, &imgui_wrapper, &caption_max](const wxString &caption, const wxString &text) {
            imgui_wrapper->text_colored(ImGuiWrapper::COL_ACTIVE, caption);
            ImGui::SameLine(caption_max);
            imgui_wrapper->text_colored(ImGuiWrapper::COL_WINDOW_BG, text);
        };

        for (const auto &t : std::array<std::string, 1>{"part_selection"})
            draw_text_with_caption(m_desc_rotate.at(t + "_caption") + ": ", m_desc_rotate.at(t));
        ImGui::EndTooltip();
    }
    ImGui::PopStyleVar(2);
}

void GizmoObjectManipulation::show_scale_tooltip_information(ImGuiWrapper *imgui_wrapper, float caption_max, float x, float y)
{
    ImTextureID normal_id = m_glcanvas.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP);
    ImTextureID hover_id  = m_glcanvas.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP_HOVER);

    caption_max += imgui_wrapper->calc_text_size(": "sv).x + 35.f;

    float  scale       = m_glcanvas.get_scale();
    #ifdef WIN32
        int dpi = get_dpi_for_window(wxGetApp().GetTopWindow());
        scale *= (float) dpi / (float) DPI_DEFAULT;
    #endif // WIN32
    ImVec2 button_size = ImVec2(25 * scale, 25 * scale); // ORCA: Use exact resolution will prevent blur on icon
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0, ImGui::GetStyle().FramePadding.y});
    ImGui::ImageButton3(normal_id, hover_id, button_size);

    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip2(ImVec2(x, y));
        auto draw_text_with_caption = [this, &imgui_wrapper, &caption_max](const wxString &caption, const wxString &text) {
            imgui_wrapper->text_colored(ImGuiWrapper::COL_ACTIVE, caption);
            ImGui::SameLine(caption_max);
            imgui_wrapper->text_colored(ImGuiWrapper::COL_WINDOW_BG, text);
        };

        for (const auto &t : std::array<std::string, 3>{"part_selection", "snap_step", "single_sided"})
            draw_text_with_caption(m_desc_scale.at(t + "_caption") + ": ", m_desc_scale.at(t));
        ImGui::EndTooltip();
    }
    ImGui::PopStyleVar(2);
}

void GizmoObjectManipulation::set_init_rotation(const Geometry::Transformation &value) {
    m_init_rotation_scale_tran = value.get_matrix_no_offset();
    m_init_rotation      = value.get_rotation();
}

void GizmoObjectManipulation::do_render_move_window(ImGuiWrapper *imgui_wrapper, std::string window_name, float x, float y, float bottom_limit)
{
    // BBS: GUI refactor: move gizmo to the right
    if (abs(last_move_input_window_width) > 0.01f) {
        if (x + last_move_input_window_width > m_glcanvas.get_canvas_size().get_width()) {
            if (last_move_input_window_width > m_glcanvas.get_canvas_size().get_width())
                x = 0;
            else
                x = m_glcanvas.get_canvas_size().get_width() - last_move_input_window_width;
        }
    }
#if BBS_TOOLBAR_ON_TOP
    imgui_wrapper->set_next_window_pos(x, y, ImGuiCond_Always, 0.f, 0.0f);
#else
    imgui_wrapper->set_next_window_pos(x, y, ImGuiCond_Always, 1.0f, 0.0f);
#endif

    // BBS
    ImGuiWrapper::push_toolbar_style(m_glcanvas.get_scale());
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0, 6.0));

    std::string name = this->m_new_title_string + "##" + window_name;
    imgui_wrapper->begin(_L(name), ImGuiWrapper::TOOLBAR_WINDOW_FLAGS);

    auto update = [this](unsigned int active_id, std::string opt_key, Vec3d original_value, Vec3d new_value) -> int {
        for (int i = 0; i < 3; i++) {
            if (original_value[i] != new_value[i]) {
                if (active_id != m_last_active_item) {
                    on_change(opt_key, i, new_value[i]);
                    return i;
                }
            }
        }
        return -1;
    };

    float space_size    = imgui_wrapper->get_style_scaling() * 8;
    float position_size = imgui_wrapper->calc_text_size(_L("Position")).x + space_size;
    float caption_max    = imgui_wrapper->calc_text_size(_L("Object coordinates")).x + 2 * space_size;
    float end_text_size = imgui_wrapper->calc_text_size(this->m_new_unit_string).x;

    // position
    Vec3d original_position;
    if (this->m_imperial_units)
        original_position = this->m_new_position * this->mm_to_in;
    else
        original_position = this->m_new_position;
    Vec3d display_position = m_buffered_position;

    // Rotation
    float unit_size = imgui_wrapper->calc_text_size(MAX_SIZE).x + space_size;
    int   index      = 1;
    int   index_unit = 1;

    ImGui::AlignTextToFramePadding();
    unsigned int current_active_id = ImGui::GetActiveID();

    Selection &              selection = m_glcanvas.get_selection();
    std::vector<std::string> modes     = {_u8L("World coordinates"), _u8L("Object coordinates")};//_u8L("Part coordinates")
    if (selection.is_multiple_full_object() || selection.is_wipe_tower()) {
        modes.pop_back();
    }
    size_t selection_idx = (int) m_coordinates_type;
    if (selection_idx >= modes.size()) {
        set_coordinates_type(ECoordinatesType::World);
        selection_idx = 0;
    }

    float caption_cs_size     = imgui_wrapper->calc_text_size(""sv).x;
    float caption_size        = caption_cs_size + 2 * space_size;
    float combox_content_size = imgui_wrapper->calc_text_size(_L("Object coordinates")).x * 1.2 + imgui_wrapper->calc_text_size("xxx"sv).x + imgui_wrapper->scaled(3);
    ImGuiWrapper::push_combo_style(m_glcanvas.get_scale());
    bool combox_changed = false;
    if (render_combo(imgui_wrapper, "", modes, selection_idx, caption_size, combox_content_size)) {
        combox_changed = true;
    }
    ImGuiWrapper::pop_combo_style();
    caption_max = combox_content_size - 4 * space_size;
    // ORCA use TextColored to match axes color
    float offset_to_center = (unit_size - ImGui::CalcTextSize("O").x) / 2;
    ImGui::SameLine(caption_max + index * space_size + offset_to_center);
    ImGui::TextColored(ImGuiWrapper::to_ImVec4(ColorRGBA::X()),"X");
    ImGui::SameLine(caption_max + unit_size + (++index) * space_size + offset_to_center);
    ImGui::TextColored(ImGuiWrapper::to_ImVec4(ColorRGBA::Y()),"Y");
    ImGui::SameLine(caption_max + (++index_unit) * unit_size + (++index) * space_size + offset_to_center);
    ImGui::TextColored(ImGuiWrapper::to_ImVec4(ColorRGBA::Z()),"Z");

    index      = 1;
    index_unit = 1;
    ImGui::AlignTextToFramePadding();
    if (selection.is_single_full_instance() && is_instance_coordinates()) {
        imgui_wrapper->text(_L("Translate(Relative)"));
    }
    else {
        imgui_wrapper->text(_L("Position"));
    }

    ImGui::SameLine(caption_max + index * space_size);
    ImGui::PushItemWidth(unit_size);
    ImGui::BBLInputDouble(label_values[0][0], &display_position[0], 0.0f, 0.0f, "%.2f");
    ImGui::SameLine(caption_max + unit_size + (++index) * space_size);
    ImGui::PushItemWidth(unit_size);
    ImGui::BBLInputDouble(label_values[0][1], &display_position[1], 0.0f, 0.0f, "%.2f");
    ImGui::SameLine(caption_max + (++index_unit) * unit_size + (++index) * space_size);
    ImGui::PushItemWidth(unit_size);
    ImGui::BBLInputDouble(label_values[0][2], &display_position[2], 0.0f, 0.0f, "%.2f");
    ImGui::SameLine(caption_max + (++index_unit) * unit_size + (++index) * space_size);
    imgui_wrapper->text(this->m_new_unit_string);
    bool is_avoid_one_update{false};
    if (combox_changed) {
        combox_changed = false;
        set_coordinates_type((ECoordinatesType) selection_idx);
        UpdateAndShow(true);
        is_avoid_one_update = true; // avoid update(current_active_id, "position", original_position
    }

    if (!is_avoid_one_update) {
        for (int i = 0; i < display_position.size(); i++) {
            if (display_position[i] > MAX_NUM) display_position[i] = MAX_NUM;
            if (display_position[i] < -MAX_NUM) display_position[i] = -MAX_NUM;
        }
        m_buffered_position = display_position;
        update(current_active_id, "position", original_position, m_buffered_position);
    }
    // the init position values are not zero, won't add reset button

    // send focus to m_glcanvas
    bool focued_on_text = false;
    for (int j = 0; j < 3; j++) {
        unsigned int id = ImGui::GetID(label_values[0][j]);
        if (current_active_id == id) {
            m_glcanvas.handle_sidebar_focus_event(label_values[0][j] + 2, true);
            focued_on_text = true;
            break;
        }
    }
    if (!focued_on_text) m_glcanvas.handle_sidebar_focus_event("", false);
    float get_cur_y      = ImGui::GetContentRegionMax().y + ImGui::GetFrameHeight() + y;
    float tip_caption_max    = 0.f;
    float total_text_max = 0.f;
    for (const auto &t : std::array<std::string, 2>{"part_selection", "snap_step"}) {
        tip_caption_max = std::max(tip_caption_max, imgui_wrapper->calc_text_size(m_desc_move[t + "_caption"]).x);
        total_text_max = std::max(total_text_max, imgui_wrapper->calc_text_size(m_desc_move[t]).x);
    }
    show_move_tooltip_information(imgui_wrapper, tip_caption_max, x, get_cur_y);
    m_last_active_item = current_active_id;
    last_move_input_window_width = ImGui::GetWindowWidth();
    imgui_wrapper->end();
    ImGui::PopStyleVar(1);
    ImGuiWrapper::pop_toolbar_style();
}

void GizmoObjectManipulation::do_render_rotate_window(ImGuiWrapper *imgui_wrapper, std::string window_name, float x, float y, float bottom_limit)
{
    // BBS: GUI refactor: move gizmo to the right
    if (abs(last_rotate_input_window_width) > 0.01f) {
        if (x + last_rotate_input_window_width > m_glcanvas.get_canvas_size().get_width()) {
            if (last_rotate_input_window_width > m_glcanvas.get_canvas_size().get_width())
                x = 0;
            else
                x = m_glcanvas.get_canvas_size().get_width() - last_rotate_input_window_width;
        }
    }
#if BBS_TOOLBAR_ON_TOP
    imgui_wrapper->set_next_window_pos(x, y, ImGuiCond_Always, 0.f, 0.0f);
#else
    imgui_wrapper->set_next_window_pos(x, y, ImGuiCond_Always, 1.0f, 0.0f);
#endif

    // BBS
    ImGuiWrapper::push_toolbar_style(m_glcanvas.get_scale());
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0, 6.0));

    std::string name = this->m_new_title_string + "##" + window_name;
    imgui_wrapper->begin(_L(name), ImGuiWrapper::TOOLBAR_WINDOW_FLAGS);

    auto update = [this](unsigned int active_id, std::string opt_key, Vec3d original_value, Vec3d new_value) -> int {
        for (int i = 0; i < 3; i++) {
            if (original_value[i] != new_value[i]) {
                if (active_id != m_last_active_item) {
                    on_change(opt_key, i, new_value[i]);
                    return i;
                }
            }
        }
        return -1;
    };

    float space_size    = imgui_wrapper->get_style_scaling() * 8;
    float position_size = imgui_wrapper->calc_text_size(_L("Rotate (relative)")).x + space_size;
    float World_size    = imgui_wrapper->calc_text_size(_L("World coordinates")).x + space_size;
    float caption_max   = std::max(position_size, World_size) + 2 * space_size;
    float end_text_size = imgui_wrapper->calc_text_size(this->m_new_unit_string).x;

    // position
    Vec3d original_position;
    if (this->m_imperial_units)
        original_position = this->m_new_position * this->mm_to_in;
    else
        original_position = this->m_new_position;
    Vec3d display_position = m_buffered_position;
    // Rotation
    Vec3d rotation   = this->m_buffered_rotation;
    Vec3d absolute_rotation = this->m_buffered_absolute_rotation;
    float unit_size = imgui_wrapper->calc_text_size(MAX_SIZE).x + space_size;
    int   index      = 1;
    int   index_unit = 1;

    ImGui::AlignTextToFramePadding();
    unsigned int current_active_id = ImGui::GetActiveID();
    ImGui::PushItemWidth(caption_max);
    imgui_wrapper->text(_L("World coordinates"));
    // ORCA use TextColored to match axes color
    float offset_to_center = (unit_size - ImGui::CalcTextSize("O").x) / 2;
    ImGui::SameLine(caption_max + index * space_size + offset_to_center);
    ImGui::TextColored(ImGuiWrapper::to_ImVec4(ColorRGBA::X()),"X");
    ImGui::SameLine(caption_max + unit_size + (++index) * space_size + offset_to_center);
    ImGui::TextColored(ImGuiWrapper::to_ImVec4(ColorRGBA::Y()),"Y");
    ImGui::SameLine(caption_max + (++index_unit) * unit_size + (++index) * space_size + offset_to_center);
    ImGui::TextColored(ImGuiWrapper::to_ImVec4(ColorRGBA::Z()),"Z");

    index      = 1;
    index_unit = 1;

    // ImGui::PushItemWidth(unit_size * 2);
    bool is_relative_input = false;
    ImGui::AlignTextToFramePadding();
    imgui_wrapper->text(_L("Rotate (relative)"));
    ImGui::SameLine(caption_max + index * space_size);
    ImGui::PushItemWidth(unit_size);
    if (ImGui::BBLInputDouble(label_values[1][0], &rotation[0], 0.0f, 0.0f, "%.2f")) {
        is_relative_input = true;
    }
    ImGui::SameLine(caption_max + unit_size + (++index) * space_size);
    ImGui::PushItemWidth(unit_size);
    if (ImGui::BBLInputDouble(label_values[1][1], &rotation[1], 0.0f, 0.0f, "%.2f")) {
        is_relative_input = true;
    }
    ImGui::SameLine(caption_max + (++index_unit) * unit_size + (++index) * space_size);
    ImGui::PushItemWidth(unit_size);
    if (ImGui::BBLInputDouble(label_values[1][2], &rotation[2], 0.0f, 0.0f, "%.2f")) {
        is_relative_input = true;
    }
    ImGui::SameLine(caption_max + (++index_unit) * unit_size + (++index) * space_size);
    imgui_wrapper->text("°");
    m_buffered_rotation = rotation;
    if (is_relative_input) {
        m_last_rotate_type = RotateType::Relative;
    }
    if (m_last_rotate_type == RotateType::Relative) {
        bool is_valid = update(current_active_id, "rotation", this->m_new_rotation, m_buffered_rotation) >= 0;
        if (is_valid) {
            m_last_rotate_type = RotateType::None;
        }
    }

    if (m_show_clear_rotation) {
        ImGui::SameLine(caption_max + 3 * unit_size + 4 * space_size + end_text_size);
        if (reset_button(imgui_wrapper, caption_max, unit_size, space_size, end_text_size)) {
            reset_rotation_value(true);
        }
        if (ImGui::IsItemHovered()) {
            float tooltip_size = imgui_wrapper->calc_text_size(_L("Reset current rotation to the value when open the rotation tool.")).x + 3 * space_size;
            imgui_wrapper->tooltip(_u8L("Reset current rotation to the value when open the rotation tool."), tooltip_size);
        }
    } else {
        ImGui::SameLine(caption_max + 3 * unit_size + 5 * space_size + end_text_size);
        ImGui::InvisibleButton("", ImVec2(ImGui::GetFontSize(), ImGui::GetFontSize()));
    }
    // send focus to m_glcanvas
    bool focued_on_text = false;
    for (int j = 0; j < 3; j++) {
        unsigned int id = ImGui::GetID(label_values[1][j]);
        if (current_active_id == id) {
            m_glcanvas.handle_sidebar_focus_event(label_values[1][j] + 2, true);
            focued_on_text = true;
            break;
        }
    }

    index      = 1;
    index_unit = 1;
    ImGui::AlignTextToFramePadding();
    imgui_wrapper->text(_L("Rotate (absolute)"));
    ImGui::SameLine(caption_max + index * space_size);
    ImGui::PushItemWidth(unit_size);
    bool is_absolute_input = false;
    if (ImGui::BBLInputDouble(label_values[2][0], &absolute_rotation[0], 0.0f, 0.0f, "%.2f")) {
        is_absolute_input = true;
    }
    ImGui::SameLine(caption_max + unit_size + (++index) * space_size);
    ImGui::PushItemWidth(unit_size);
    if (ImGui::BBLInputDouble(label_values[2][1], &absolute_rotation[1], 0.0f, 0.0f, "%.2f")) {
        is_absolute_input = true;
    }
    ImGui::SameLine(caption_max + (++index_unit) * unit_size + (++index) * space_size);
    ImGui::PushItemWidth(unit_size);
    if (ImGui::BBLInputDouble(label_values[2][2], &absolute_rotation[2], 0.0f, 0.0f, "%.2f")) {
        is_absolute_input = true;
    }
    ImGui::SameLine(caption_max + (++index_unit) * unit_size + (++index) * space_size);
    imgui_wrapper->text("°");
    m_buffered_absolute_rotation = absolute_rotation;
    if (is_absolute_input) {
        m_last_rotate_type = RotateType::Absolute;
    }
    if (m_last_rotate_type == RotateType::Absolute) {
        bool is_valid = update(current_active_id, "absolute_rotation", this->m_new_absolute_rotation, m_buffered_absolute_rotation) >= 0;
        if (is_valid) {
            m_last_rotate_type = RotateType::None;
        }
    }

    if (m_show_reset_0_rotation) {
        ImGui::SameLine(caption_max + 3 * unit_size + 4 * space_size + end_text_size);
        if (reset_zero_button(imgui_wrapper, caption_max, unit_size, space_size, end_text_size)) { reset_rotation_value(false); }
        if (ImGui::IsItemHovered()) {
            float tooltip_size = imgui_wrapper->calc_text_size(_L("Reset current rotation to real zeros.")).x + 3 * space_size;
            imgui_wrapper->tooltip(_L("Reset current rotation to real zeros."), tooltip_size);
        }
    }
    // send focus to m_glcanvas
    bool absolute_focued_on_text = false;
    for (int j = 0; j < 3; j++) {
        unsigned int id = ImGui::GetID(label_values[2][j]);
        if (current_active_id == id) {
            m_glcanvas.handle_sidebar_focus_event(label_values[2][j] + 2, true);
            absolute_focued_on_text = true;
            break;
        }
    }
    if (!focued_on_text  && !absolute_focued_on_text)
        m_glcanvas.handle_sidebar_focus_event("", false);

    float get_cur_y       = ImGui::GetContentRegionMax().y + ImGui::GetFrameHeight() + y;
    float tip_caption_max = 0.f;
    float total_text_max  = 0.f;
    for (const auto &t : std::array<std::string, 1>{"part_selection"}) {
        tip_caption_max = std::max(tip_caption_max, imgui_wrapper->calc_text_size(m_desc_move[t + "_caption"]).x);
        total_text_max  = std::max(total_text_max, imgui_wrapper->calc_text_size(m_desc_move[t]).x);
    }
    show_rotate_tooltip_information(imgui_wrapper, tip_caption_max, x, get_cur_y);
    m_last_active_item = current_active_id;
    last_rotate_input_window_width = ImGui::GetWindowWidth();
    imgui_wrapper->end();

    // BBS
    ImGui::PopStyleVar(1);
    ImGuiWrapper::pop_toolbar_style();
}

void GizmoObjectManipulation::do_render_scale_input_window(ImGuiWrapper* imgui_wrapper, std::string window_name, float x, float y, float bottom_limit)
{
    //BBS: GUI refactor: move gizmo to the right
    if (abs(last_scale_input_window_width) > 0.01f) {
        if (x + last_scale_input_window_width > m_glcanvas.get_canvas_size().get_width()) {
            if (last_scale_input_window_width > m_glcanvas.get_canvas_size().get_width())
                x = 0;
            else
                x = m_glcanvas.get_canvas_size().get_width() - last_scale_input_window_width;
        }
    }
#if BBS_TOOLBAR_ON_TOP
    imgui_wrapper->set_next_window_pos(x, y, ImGuiCond_Always, 0.f, 0.0f);
#else
    imgui_wrapper->set_next_window_pos(x, y, ImGuiCond_Always, 1.0f, 0.0f);
#endif

    //BBS
    ImGuiWrapper::push_toolbar_style(m_glcanvas.get_scale());
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0, 6.0));

    std::string name = this->m_new_title_string + "##" + window_name;
    imgui_wrapper->begin(_L(name), ImGuiWrapper::TOOLBAR_WINDOW_FLAGS);

    auto update = [this](unsigned int active_id, std::string opt_key, Vec3d original_value, Vec3d new_value)->int {
        for (int i = 0; i < 3; i++)
        {
            if (original_value[i] != new_value[i])
            {
                if (active_id != m_last_active_item)
                {
                    on_change(opt_key, i, new_value[i]);
                    return i;
                }
            }
        }
        return -1;
    };

    float space_size = imgui_wrapper->get_style_scaling() * 8;
    float scale_size = imgui_wrapper->calc_text_size(_L("Scale")).x + space_size;
    float caption_max   = imgui_wrapper->calc_text_size(_L("Object coordinates")).x + 2 * space_size;
    float end_text_size = imgui_wrapper->calc_text_size(this->m_new_unit_string).x;
    ImGui::AlignTextToFramePadding();
    unsigned int current_active_id = ImGui::GetActiveID();

    Vec3d scale = m_buffered_scale;
    Vec3d display_size = m_buffered_size;

    Vec3d display_position = m_buffered_position;

    float unit_size = imgui_wrapper->calc_text_size(MAX_SIZE).x + space_size;
    bool imperial_units = this->m_imperial_units;

    int index      = 2;
    int index_unit = 1;

    Selection &              selection = m_glcanvas.get_selection();
    std::vector<std::string> modes     = {_u8L("World coordinates"), _u8L("Object coordinates"), _u8L("Part coordinates")};
    if (selection.is_single_full_object()) { modes.pop_back(); }
    if (selection.is_multiple_full_object()) {
        modes.pop_back();
        modes.pop_back();
    }
    size_t selection_idx = (int) m_coordinates_type;
    if (selection_idx >= modes.size()) {
        set_coordinates_type(ECoordinatesType::World);
        selection_idx = 0;
    }

    float caption_cs_size     = imgui_wrapper->calc_text_size(""sv).x;
    float caption_size        = caption_cs_size + 2 * space_size;
    float combox_content_size = imgui_wrapper->calc_text_size(_L("Object coordinates")).x * 1.2 + imgui_wrapper->calc_text_size("xxx"sv).x + imgui_wrapper->scaled(3);
    ImGuiWrapper::push_combo_style(m_glcanvas.get_scale());
    bool combox_changed = false;
    if (render_combo(imgui_wrapper, "", modes, selection_idx, caption_size, combox_content_size)) {
        combox_changed = true;
    }
    ImGuiWrapper::pop_combo_style();
    caption_max = combox_content_size - 4 * space_size;
    //ImGui::Dummy(ImVec2(caption_max, -1));
    // ORCA use TextColored to match axes color
    float offset_to_center = (unit_size - ImGui::CalcTextSize("O").x) / 2;
    ImGui::SameLine(caption_max + space_size + offset_to_center);
    ImGui::TextColored(ImGuiWrapper::to_ImVec4(ColorRGBA::X()),"X");
    ImGui::SameLine(caption_max + unit_size + index * space_size + offset_to_center);
    ImGui::TextColored(ImGuiWrapper::to_ImVec4(ColorRGBA::Y()),"Y");
    ImGui::SameLine(caption_max + (++index_unit) * unit_size + (++index) * space_size + offset_to_center);
    ImGui::TextColored(ImGuiWrapper::to_ImVec4(ColorRGBA::Z()),"Z");

    index      = 2;
    index_unit = 1;

    //ImGui::PushItemWidth(unit_size * 2);
    ImGui::AlignTextToFramePadding();
    imgui_wrapper->text(_L("Scale"));
    ImGui::SameLine(caption_max + space_size);
    ImGui::PushItemWidth(unit_size);
    ImGui::BBLInputDouble(label_scale_values[0][0], &scale[0], 0.0f, 0.0f, "%.2f");
    ImGui::SameLine(caption_max + unit_size + index * space_size);
    ImGui::PushItemWidth(unit_size);
    ImGui::BBLInputDouble(label_scale_values[0][1], &scale[1], 0.0f, 0.0f, "%.2f");
    ImGui::SameLine(caption_max + (++index_unit) *unit_size + (++index) * space_size);
    ImGui::PushItemWidth(unit_size);
    ImGui::BBLInputDouble(label_scale_values[0][2], &scale[2], 0.0f, 0.0f, "%.2f");
    ImGui::SameLine(caption_max + (++index_unit) *unit_size + (++index) * space_size);
    imgui_wrapper->text("%");
    if (scale.x() > 0 && scale.y() > 0 && scale.z() > 0) {
        m_buffered_scale = scale;
    }

    if (m_show_clear_scale) {
        ImGui::SameLine(caption_max + 3 * unit_size + 4 * space_size + end_text_size);
        if (reset_button(imgui_wrapper, caption_max, unit_size, space_size, end_text_size))
            reset_scale_value();
    } else {
        ImGui::SameLine(caption_max + 3 * unit_size + 5 * space_size + end_text_size);
        ImGui::InvisibleButton("", ImVec2(ImGui::GetFontSize(), ImGui::GetFontSize()));
    }

    //Size
    Vec3d original_size;
    if (this->m_imperial_units)
        original_size = this->m_new_size * this->mm_to_in;
    else
        original_size = this->m_new_size;

    index              = 2;
    index_unit         = 1;
    //ImGui::PushItemWidth(unit_size * 2);
    ImGui::AlignTextToFramePadding();
    imgui_wrapper->text(_L("Size"));
    ImGui::SameLine(caption_max + space_size);
    ImGui::PushItemWidth(unit_size);
    ImGui::BBLInputDouble(label_scale_values[1][0], &display_size[0], 0.0f, 0.0f, "%.2f");
    ImGui::SameLine(caption_max + unit_size + index * space_size);
    ImGui::PushItemWidth(unit_size);
    ImGui::BBLInputDouble(label_scale_values[1][1], &display_size[1], 0.0f, 0.0f, "%.2f");
    ImGui::SameLine(caption_max + (++index_unit) *unit_size + (++index) * space_size);
    ImGui::PushItemWidth(unit_size);
    ImGui::BBLInputDouble(label_scale_values[1][2], &display_size[2], 0.0f, 0.0f, "%.2f");
    ImGui::SameLine(caption_max + (++index_unit) *unit_size + (++index) * space_size);
    imgui_wrapper->text(this->m_new_unit_string);
    for (int i = 0; i < display_size.size(); i++) {
        if (std::abs(display_size[i]) > MAX_NUM) {
            display_size[i] = MAX_NUM;
        }
    }
    if (display_size.x() > 0 && display_size.y() > 0 && display_size.z() > 0) {
        m_buffered_size = display_size;
    }
    ImGui::Separator();
    ImGui::AlignTextToFramePadding();
    bool is_avoid_one_update{false};
    if (combox_changed) {
        combox_changed = false;
        set_coordinates_type((ECoordinatesType) selection_idx);
        UpdateAndShow(true);
        is_avoid_one_update = true;
    }

    auto uniform_scale_size =imgui_wrapper->calc_text_size(_L("uniform scale")).x;
    ImGui::PushItemWidth(uniform_scale_size);
    int size_sel{-1};
    if (!is_avoid_one_update) {
        size_sel    = update(current_active_id, "size", original_size, m_buffered_size);
    }
    ImGui::PopStyleVar(1);
    bool uniform_scale = this->m_uniform_scale;

    // BBS: when select multiple objects, uniform scale can be deselected
    //const Selection &selection = m_glcanvas.get_selection();
    //bool uniform_scale_only    = selection.is_multiple_full_object() || selection.is_multiple_full_instance() || selection.is_mixed() || selection.is_multiple_volume() ||
    //                          selection.is_multiple_modifier();

    //if (uniform_scale_only) {
    //    imgui_wrapper->disabled_begin(true);
    //    imgui_wrapper->bbl_checkbox(_L("uniform scale"), uniform_scale_only);
    //    imgui_wrapper->disabled_end();
    //} else {
        imgui_wrapper->bbl_checkbox(_L("uniform scale"), uniform_scale);
    //}
    if (uniform_scale != this->m_uniform_scale) { this->set_uniform_scaling(uniform_scale); }

     // for (int index = 0; index < 3; index++)
    //    BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ",before_index="<<index <<boost::format(",scale %1%, buffered %2%, original_id %3%, new_id %4%\n") %
    //    this->m_new_scale[index] % m_buffered_scale[index] % m_last_active_item % current_active_id;
    int scale_sel = update(current_active_id, "scale", this->m_new_scale, m_buffered_scale);
    if ((scale_sel >= 0)) {
        // for (int index = 0; index < 3; index++)
        //    BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ",after_index="<<index <<boost::format(",scale %1%, buffered %2%, original_id %3%, new_id %4%\n") %
        //    this->m_new_scale[index] % m_buffered_scale[index] % m_last_active_item % current_active_id;
        for (int i = 0; i < 3; ++i) {
            if (i != scale_sel) ImGui::ClearInputTextInitialData(label_scale_values[0][i], m_buffered_scale[i]);
            ImGui::ClearInputTextInitialData(label_scale_values[1][i], m_buffered_size[i]);
        }
    }

    if ((size_sel >= 0)) {
        // for (int index = 0; index < 3; index++)
        //    BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ",after_index="<<index <<boost::format(",scale %1%, buffered %2%, original_id %3%, new_id %4%\n") %
        //    this->m_new_scale[index] % m_buffered_scale[index] % m_last_active_item % current_active_id;
        for (int i = 0; i < 3; ++i) {
            ImGui::ClearInputTextInitialData(label_scale_values[0][i], m_buffered_scale[i]);
            if (i != size_sel) ImGui::ClearInputTextInitialData(label_scale_values[1][i], m_buffered_size[i]);
        }
    }

    //send focus to m_glcanvas
    bool focued_on_text = false;
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 3; j++)
        {
            unsigned int id = ImGui::GetID(label_scale_values[i][j]);
            if (current_active_id == id)
            {
                m_glcanvas.handle_sidebar_focus_event(label_scale_values[i][j] + 2, true);
                focued_on_text = true;
                break;
            }
        }
    if (!focued_on_text)
        m_glcanvas.handle_sidebar_focus_event("", false);
    float get_cur_y       = ImGui::GetContentRegionMax().y + ImGui::GetFrameHeight() + y;
    float tip_caption_max = 0.f;
    float total_text_max  = 0.f;
    for (const auto &t : std::array<std::string, 3>{"part_selection", "snap_step", "single_sided"}) {
        tip_caption_max = std::max(tip_caption_max, imgui_wrapper->calc_text_size(m_desc_scale[t + "_caption"]).x);
        total_text_max  = std::max(total_text_max, imgui_wrapper->calc_text_size(m_desc_scale[t]).x);
    }
    show_scale_tooltip_information(imgui_wrapper, tip_caption_max, x, get_cur_y);
    m_last_active_item = current_active_id;

    last_scale_input_window_width = ImGui::GetWindowWidth();
    imgui_wrapper->end();

    //BBS
    ImGuiWrapper::pop_toolbar_style();
}


} //namespace GUI
} //namespace Slic3r
