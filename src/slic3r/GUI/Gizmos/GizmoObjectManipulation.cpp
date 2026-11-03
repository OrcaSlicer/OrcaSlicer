#include "slic3r/GUI/ImGuiWrapper.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cmath>
#include <cfloat>
#include <imgui.h>
#include <imgui/imgui_internal.h>

#include "GizmoObjectManipulation.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
//#include "I18N.hpp"
#include "GLGizmosManager.hpp"
#include "GLGizmoAlignment.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/Utils/UndoRedo.hpp"
#include "libslic3r/AppConfig.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/Geometry.hpp"
#include "slic3r/GUI/Selection.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "GLGizmoUtils.hpp"

#include <boost/algorithm/string.hpp>
#include "libslic3r/Point.hpp"
#include <limits>
#include "slic3r/GUI/Gizmos/GLGizmoBase.hpp"
#include "slic3r/GUI/GUI.hpp"
#include <math.h>
#include "slic3r/GUI/ObjectDataViewModel.hpp"
#include "libslic3r/libslic3r.h"
#include "slic3r/GUI/GUI_Geometry.hpp"
#include <vector>
#include <string_view>
#include "libslic3r/Color.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"

#define MAX_NUM 9999.99
#define MAX_SIZE std::string_view{"9999.99"}

using namespace std::string_view_literals;

namespace Slic3r
{
namespace GUI
{

const double GizmoObjectManipulation::in_to_mm = 25.4;
const double GizmoObjectManipulation::mm_to_in = 0.0393700787;
const double GizmoObjectManipulation::oz_to_g = 28.34952;
const double GizmoObjectManipulation::g_to_oz = 0.035274;

GizmoObjectManipulation::GizmoObjectManipulation(GLCanvas3D& glcanvas)
    : m_glcanvas(glcanvas), m_alignment_helper(glcanvas)
{
    m_imperial_units = wxGetApp().app_config->get("use_inches") == "1";
    m_new_unit_string = m_imperial_units ? L_CONTEXT("in", "inches") : L("mm");

    const wxString shift                   = GUI::shortkey_shift_prefix();
    const wxString alt                     = GUI::shortkey_alt_prefix();
    const wxString ctrl                    = GUI::shortkey_ctrl_prefix();

    m_shortcuts_move = {
        {alt + _L("Left mouse button"),     _L("Part selection")},
        {ctrl + _L("Left mouse button"),    _L("Select multiple objects")},
        {shift + _L("Left mouse button"),   _L("Fixed step drag")},
        {_L("Context Menu"),                _L("Toggle Auto-Drop")}
    };

    m_shortcuts_rotate = {
        {alt + _L("Left mouse button"),     _L("Part selection")},
        {_L("Context Menu"),                _L("Toggle Auto-Drop")}
    };

    m_shortcuts_scale = {
        {alt + _L("Left mouse button"),     _L("Part selection")},
        {shift + _L("Left mouse button"),   _L("Fixed step drag")},
        {ctrl + _L("Left mouse button"),    _L("Single sided scaling")},
        {_L("Context Menu"),                _L("Toggle Auto-Drop")}
    };
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

        m_new_unit_string = m_imperial_units ? L_CONTEXT("in", "inches") : L("mm");

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
        m_new_title_string = L("Object operations");
    }
    else if (selection.is_single_full_object() && obj_list->is_selected(itObject)) {
        const BoundingBoxf3& box = selection.get_bounding_box();
        m_new_position = box.center();
        m_new_scale    = Vec3d(100., 100., 100.);
        m_new_size     = selection.get_bounding_box_in_current_reference_system().first.size();
		m_new_scale_label_string  = L("Scale");
        m_new_enabled  = true;
        m_new_title_string = L("Object operations");
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
        m_new_title_string = L("Volume operations");
    } else if (obj_list->is_connectors_item_selected() || obj_list->multiple_selection() || obj_list->is_selected(itInstanceRoot)) {
        reset_settings_value();
		m_new_move_label_string   = L("Translate");
		m_new_scale_label_string  = L("Scale");
        m_unscale_size            = selection.get_bounding_box_in_current_reference_system().first.size();
        m_new_size                = selection.get_bounding_box_in_current_reference_system().first.size();
        m_new_enabled  = true;
        m_new_title_string = L("Group operations");
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
        auto update = [i](Vec3d &cached, Vec3d &cached_rounded,  const Vec3d &new_value) {
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

    // Assembly view has its own selection and transformations, which drop_selection() doesn't update
    m_enable_drop_to_bed = m_glcanvas.get_canvas_type() == GLCanvas3D::ECanvasType::CanvasView3D && selection.can_drop();

    if (selection.is_single_full_instance() || selection.is_single_volume_or_modifier()) {
        const GLVolume *               volume = selection.get_first_volume();

        Vec3d rotation;
        Vec3d scale;

        if (selection.is_single_full_instance()) {
            rotation = volume->get_instance_rotation();
            scale = volume->get_instance_scaling_factor();
        }
        else {
            rotation = volume->get_volume_rotation();
            scale = volume->get_volume_scaling_factor();
        }
        m_show_clear_rotation = !rotation.isApprox(m_init_rotation);
        m_show_reset_0_rotation = !rotation.isApprox(Vec3d::Zero());
        m_show_clear_scale = (m_cache.scale / 100.0f - Vec3d::Ones()).norm() > 0.001;
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
    wxGetApp().plater()->take_snapshot(_u8L("Set orientation"), UndoRedo::SnapshotType::GizmoAction);
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
    m_glcanvas.do_scale(L("Set scale"));
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
    if(!label.empty()){
        ImGui::AlignTextToFramePadding();
        imgui_wrapper->text(label);
        ImGui::SameLine(label_width);
    }

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
    wxGetApp().plater()->take_snapshot(_u8L("Reset position"), UndoRedo::SnapshotType::GizmoAction);
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
    m_glcanvas.do_rotate(L("Reset rotation"));

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

bool GizmoObjectManipulation::reset_button(ImGuiWrapper *imgui_wrapper, bool enabled)
{
    imgui_wrapper->disabled_begin(!enabled);
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

    pressed = ImGui::ImageButton3(normal_id, hover_id, button_size, {0,0}, {1,1}, -1, {0,0,0,0}, {1,1,1, enabled ? 1.f : 0.f});  // ORCA make icon invisible to prevent changes on layout

    ImGui::PopStyleVar(1);

    imgui_wrapper->disabled_end();
    return pressed;
}

bool GizmoObjectManipulation::reset_zero_button(ImGuiWrapper *imgui_wrapper,  bool enabled)
{
    imgui_wrapper->disabled_begin(!enabled);
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

    pressed = ImGui::ImageButton3(normal_id, hover_id, button_size, {0,0}, {1,1}, -1, {0,0,0,0}, {1,1,1, enabled ? 1.f : 0.f});  // ORCA make icon invisible to prevent changes on layout

    ImGui::PopStyleVar(1);

    imgui_wrapper->disabled_end();
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
         char buf[3][64] = {};
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

void GizmoObjectManipulation::set_init_rotation(const Geometry::Transformation &value) {
    m_init_rotation_scale_tran = value.get_matrix_no_offset();
    m_init_rotation      = value.get_rotation();
}

void GizmoObjectManipulation::reset_align_state()
{
    m_hovered_align_type = GLGizmoAlignment::AlignType::NONE;
    m_can_align_within   = m_alignment_helper.can_align(GLGizmoAlignment::AlignType::X_MIN);
    m_align_to_parent    = !m_can_align_within;
}

void GizmoObjectManipulation::do_render_move_window(ImGuiWrapper *imgui_wrapper, std::string window_name, float x, float y, float bottom_limit)
{
    float scale = m_glcanvas.get_scale();
#ifdef WIN32
    scale *= (float)get_dpi_for_window(wxGetApp().GetTopWindow()) / (float)DPI_DEFAULT;
#endif // WIN32

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

    const ImGuiStyle &style         = ImGui::GetStyle();

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
    // The align section shares the combo width, the caption column and the input columns of the position row.
    const bool show_align = m_glcanvas.get_canvas_type() == GLCanvas3D::ECanvasType::CanvasView3D &&
                            !m_glcanvas.get_selection().is_wipe_tower();
    //ORCA
    float coord_combo_width = std::max({
        imgui_wrapper->calc_text_size(_L("World")).x,
        imgui_wrapper->calc_text_size(_L("Object")).x,
        imgui_wrapper->calc_text_size(_L("Part")).x
    });
    float label_max = std::max({
        imgui_wrapper->calc_text_size(_L("Position")).x,
        imgui_wrapper->calc_text_size(_L("Relative")).x
    });
    if (show_align) {
        coord_combo_width = std::max({coord_combo_width, imgui_wrapper->calc_text_size(_L("Selection")).x,
                                      imgui_wrapper->calc_text_size(_L("Object")).x,
                                      imgui_wrapper->calc_text_size(_L("Plate")).x});
        label_max         = std::max(label_max, imgui_wrapper->calc_text_size(_L("Distribute")).x);
    }
    coord_combo_width += imgui_wrapper->calc_text_size("xxx"sv).x + imgui_wrapper->scaled(3.5f);
    float caption_max = std::max(label_max, coord_combo_width - 3 * space_size);
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
    // Four align buttons sit under each axis' input, a space before the next axis, so the inputs widen to hold them.
    const float align_icon_size   = 16.f * scale;
    const float align_button_size = align_icon_size + 2.f * style.FramePadding.x;  // ORCA
    if (show_align) {
        unit_size = std::max(unit_size, 3.f * align_button_size);
        unit_size = std::round(unit_size); // ORCA keep columns on whole pixels
    }
    int   index      = 1;
    int   index_unit = 1;

    ImGui::AlignTextToFramePadding();
    unsigned int current_active_id = ImGui::GetActiveID();

    Selection &              selection = m_glcanvas.get_selection();
    std::vector<std::string> modes     = {_u8L("World"), _u8L("Object")};//_u8L("Part") // ORCA use shorter terms to make UI more compact
    if (selection.is_multiple_full_object() || selection.is_wipe_tower()) {
        modes.pop_back();
    }
    size_t selection_idx = (int) m_coordinates_type;
    if (selection_idx >= modes.size()) {
        set_coordinates_type(ECoordinatesType::World);
        selection_idx = 0;
    }

    ImGuiWrapper::push_combo_style(m_glcanvas.get_scale());
    bool combox_changed = false;
    if (render_combo(imgui_wrapper, "", modes, selection_idx, 0, coord_combo_width)) {
        combox_changed = true;
    }
    ImGuiWrapper::pop_combo_style();
    // After pop_combo_style(): its light popup background would hide the tooltip's white text.
    if (ImGui::IsItemHovered()) {
        auto tooltip_str = _L("Coordinate system used for transform actions.");
        imgui_wrapper->tooltip(tooltip_str, imgui_wrapper->calc_text_size(tooltip_str).x + 3 * space_size);
    }

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
        imgui_wrapper->text(_L("Relative")); // ORCA
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

    // Align and distribute along the coordinate system of the panel, within the selection or to the object or plate holding it.
    using AlignType                = GLGizmoAlignment::AlignType;
    const bool old_align_to_parent = m_align_to_parent;
    AlignType  hovered_align_type  = AlignType::NONE;
    if (show_align) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        m_alignment_helper.set_object_cs(is_instance_coordinates());

        // Each row is row_height tall, separated by ItemSpacing.y. Every control is centered vertically in its row.
        const float       row_y         = ImGui::GetCursorPosY();
        const float       button_height = align_icon_size + 2.f * style.FramePadding.y;
        ImGuiWrapper::push_combo_style(m_glcanvas.get_scale());
        const float combo_height = ImGui::GetFrameHeight();
        ImGuiWrapper::pop_combo_style();
        const float row_height = std::max({ImGui::GetTextLineHeight(), button_height, combo_height});
        const float row_gap    = style.ItemSpacing.y;
        int         row        = 0;
        auto row_top       = [&](int r) { return row_y + r * (row_height + row_gap); };
        // Snap to whole pixels to avoid blurry icons
        auto center_in_row = [&](int r, float item_height) { return std::floor(row_top(r) + (row_height - item_height) * 0.5f); };

        // Row 0: align mode combo in the caption column.
        ImGui::SetCursorPosY(center_in_row(row, combo_height));

        // The last mode aligns to the parent. Aligning within the selection needs two items.
        std::vector<std::string> align_modes = {
            _u8L("Selection"),
            m_alignment_helper.is_parts_selection(true) ? _u8L("Object") : _u8L("Plate")
        };
        // Selecting enough items to align within them makes that the default, as when the tool opens.
        const bool can_align_within = m_alignment_helper.can_align(AlignType::X_MIN);
        if (can_align_within != m_can_align_within) {
            m_can_align_within = can_align_within;
            m_align_to_parent  = !can_align_within;
        }
        if (!can_align_within) {
            align_modes.erase(align_modes.begin());
            m_align_to_parent = true;
        }
        size_t align_mode_idx = m_align_to_parent ? align_modes.size() - 1 : 0;
        ImGuiWrapper::push_combo_style(m_glcanvas.get_scale());
        // render_combo() takes its ImGui id from the empty label, which the coordinate combo uses too.
        ImGui::PushID("align_mode");
        if (render_combo(imgui_wrapper, "", align_modes, align_mode_idx, 0, coord_combo_width))
            m_align_to_parent = align_mode_idx + 1 == align_modes.size();
        ImGui::PopID();
        ImGuiWrapper::pop_combo_style();
        // After pop_combo_style(): its light popup background would hide the tooltip's white text.
        if (ImGui::IsItemHovered()) {
            wxString tooltip_str = _L("Reference for alignment");
            imgui_wrapper->tooltip(tooltip_str, imgui_wrapper->calc_text_size(tooltip_str).x + 3 * space_size);
        }

        const bool dark_mode    = wxGetApp().dark_mode();
        int        current_axis = -1;
        auto align_icon = [&](int axis, AlignType type, GLGizmosManager::MENU_ICON_NAME icon,
                              GLGizmosManager::MENU_ICON_NAME dark_icon, const wxString &tip) {
            const bool distribute = type >= AlignType::DISTRIBUTE_X;
            const bool enabled    = m_align_to_parent ? m_alignment_helper.can_align(type, true) :
                                    distribute        ? m_alignment_helper.can_distribute(type) :
                                                        m_alignment_helper.can_align(type);
            if (axis != current_axis) {
                ImGui::SameLine(int(caption_max + (axis + 1) * space_size + axis * unit_size)); // ORCA ensure placement snaps to pixel to fix blur
                if (current_axis < 0)
                    ImGui::SetCursorPosY(center_in_row(row, button_height));
                current_axis = axis;
            } else
                ImGui::SameLine(0, 0);

            if (!enabled) {
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.3f);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyle().Colors[ImGuiCol_Button]);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyle().Colors[ImGuiCol_Button]);
            }
            ImTextureID texture = m_glcanvas.get_gizmos_manager().get_icon_texture_id(dark_mode ? dark_icon : icon);
            if (ImGui::ImageButton3(texture, texture, ImVec2(align_icon_size, align_icon_size)) && enabled) {
                if (distribute)
                    m_alignment_helper.distribute_objects(type);
                else
                    m_alignment_helper.align_objects(type, m_align_to_parent);
            }
            if (!enabled) {
                ImGui::PopStyleColor(2);
                ImGui::PopStyleVar();
            }

            if (ImGui::IsItemHovered()) {
                wxString text = tip;
                if (enabled)
                    hovered_align_type = type;
                else {
                    // Too few items is the reason when there are items to move (only distribute needs three);
                    // otherwise the helper names it.
                    std::string why_not;
                    if (m_alignment_helper.get_items_count(type, m_align_to_parent, &why_not) > 0)
                        why_not = _u8L("Select at least 3 parts or objects");
                    if (!why_not.empty())
                        text += "\n" + from_u8(why_not);
                }
                imgui_wrapper->tooltip(text, imgui_wrapper->calc_text_size(text).x + 3 * space_size);
            }
        };
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        align_icon(0, AlignType::X_MIN, GLGizmosManager::IC_ALIGN_X_MIN, GLGizmosManager::IC_ALIGN_X_MIN_DARK, _L("Align left") + " (-X)");
        align_icon(0, AlignType::CENTER_X, GLGizmosManager::IC_ALIGN_X_CENTER, GLGizmosManager::IC_ALIGN_X_CENTER_DARK, _L("Align left-right center") + " (X)");
        align_icon(0, AlignType::X_MAX, GLGizmosManager::IC_ALIGN_X_MAX, GLGizmosManager::IC_ALIGN_X_MAX_DARK, _L("Align right") + " (+X)");

        align_icon(1, AlignType::Y_MIN, GLGizmosManager::IC_ALIGN_Y_MIN, GLGizmosManager::IC_ALIGN_Y_MIN_DARK, _L("Align front") + " (-Y)");
        align_icon(1, AlignType::CENTER_Y, GLGizmosManager::IC_ALIGN_Y_CENTER, GLGizmosManager::IC_ALIGN_Y_CENTER_DARK, _L("Align front-back center") + " (Y)");
        align_icon(1, AlignType::Y_MAX, GLGizmosManager::IC_ALIGN_Y_MAX, GLGizmosManager::IC_ALIGN_Y_MAX_DARK, _L("Align back") + " (+Y)");

        align_icon(2, AlignType::Z_MIN, GLGizmosManager::IC_ALIGN_Z_MIN, GLGizmosManager::IC_ALIGN_Z_MIN_DARK, _L("Align bottom") + " (-Z)");
        align_icon(2, AlignType::CENTER_Z, GLGizmosManager::IC_ALIGN_Z_CENTER, GLGizmosManager::IC_ALIGN_Z_CENTER_DARK, _L("Align top-bottom center") + " (Z)");
        align_icon(2, AlignType::Z_MAX, GLGizmosManager::IC_ALIGN_Z_MAX, GLGizmosManager::IC_ALIGN_Z_MAX_DARK, _L("Align top") + " (+Z)");

        if (!m_align_to_parent) {
            row          = 1;
            current_axis = -1;
            ImGui::SetCursorPosY(center_in_row(row, ImGui::GetTextLineHeight()));
            imgui_wrapper->text(_L("Distribute"));
            align_icon(0, AlignType::DISTRIBUTE_X, GLGizmosManager::IC_DISTRIBUTE_X, GLGizmosManager::IC_DISTRIBUTE_X_DARK, _L("Distribute left-right") + " (X)");
            align_icon(1, AlignType::DISTRIBUTE_Y, GLGizmosManager::IC_DISTRIBUTE_Y, GLGizmosManager::IC_DISTRIBUTE_Y_DARK, _L("Distribute front-back") + " (Y)");
            align_icon(2, AlignType::DISTRIBUTE_Z, GLGizmosManager::IC_DISTRIBUTE_Z, GLGizmosManager::IC_DISTRIBUTE_Z_DARK, _L("Distribute top-bottom") + " (Z)");
        }
        ImGui::PopStyleVar();
        ImGui::SetCursorPosY(row_top(row + 1));
    }
    // The Move gizmo draws the preview with the scene, which an ImGui-only frame does not redraw.
    if (hovered_align_type != m_hovered_align_type || m_align_to_parent != old_align_to_parent) {
        m_hovered_align_type = hovered_align_type;
        m_glcanvas.set_as_dirty();
        m_glcanvas.request_extra_frame();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    float f_scale = m_glcanvas.get_gizmos_manager().get_layout_scale();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f * f_scale));

    GLGizmoUtils::render_tooltip_button(imgui_wrapper, m_glcanvas, m_shortcuts_move, x, y);

    // ORCA use existing function for drop and show button on bottom bar to keep gizmo compact
    imgui_wrapper->disabled_begin(!m_enable_drop_to_bed);
    ImGui::SameLine(0, space_size * 1.5f);
    if (imgui_wrapper->button(_L("Drop"))){
        wxGetApp().plater()->drop_selection();
        UpdateAndShow(true);
    }
    imgui_wrapper->disabled_end();
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        imgui_wrapper->tooltip(_L("Move the selection along Z so that its lowest point touches the bed."), ImGui::GetFontSize() * 20.0f);

    ImGui::SameLine();
    GLGizmoUtils::begin_right_aligned_buttons({ _L("Done") });
    if (imgui_wrapper->button(_L("Done"))) {
        m_glcanvas.reset_all_gizmos();
    }

    m_last_active_item = current_active_id;
    last_move_input_window_width = ImGui::GetWindowWidth();
    ImGui::PopStyleVar();
    imgui_wrapper->end();
    ImGui::PopStyleVar();
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
    // ORCA
    float caption_max = std::max({
        imgui_wrapper->calc_text_size(_L("Relative")).x,
        imgui_wrapper->calc_text_size(_L("Absolute")).x,
        imgui_wrapper->calc_text_size(_L("World")).x
        //imgui_wrapper->calc_text_size(_L("Object")).x,
        //imgui_wrapper->calc_text_size(_L("Part")).x
    }) + 3.f * space_size;
    float end_text_size = ImGui::CalcTextSize("°").x; // ORCA rotate gizmo not uses mm or inch

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
    imgui_wrapper->text(_L("World")); // ORCA
    if (ImGui::IsItemHovered()) {
        auto tooltip_str = _L("Coordinate system used for transform actions.");
        imgui_wrapper->tooltip(tooltip_str, imgui_wrapper->calc_text_size(tooltip_str).x + 3 * space_size);
    }
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
    imgui_wrapper->text(_L("Relative")); // ORCA
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

    ImGui::SameLine(caption_max + index_unit * unit_size + (++index) * space_size + end_text_size);
    if (reset_button(imgui_wrapper, m_show_clear_rotation)) // ORCA reserve icon space to prevent changes on layout
        reset_rotation_value(true);
    if (m_show_clear_rotation && ImGui::IsItemHovered()) {
        float tooltip_size = imgui_wrapper->calc_text_size(_L("Reset current rotation to the value when open the rotation tool.")).x + 3 * space_size;
        imgui_wrapper->tooltip(_u8L("Reset current rotation to the value when open the rotation tool."), tooltip_size);
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
    imgui_wrapper->text(_L("Absolute"));
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

    ImGui::SameLine(caption_max + index_unit * unit_size + (++index) * space_size + end_text_size);
    if (reset_zero_button(imgui_wrapper, m_show_reset_0_rotation)) // ORCA reserve icon space to prevent changes on layout
        reset_rotation_value(false);
    if (m_show_reset_0_rotation && ImGui::IsItemHovered()) {
        float tooltip_size = imgui_wrapper->calc_text_size(_L("Reset current rotation to real zeros.")).x + 3 * space_size;
        imgui_wrapper->tooltip(_L("Reset current rotation to real zeros."), tooltip_size);
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

    ImGui::Spacing(); // needed after Text
    ImGui::Separator();
    ImGui::Spacing();
    float f_scale = m_glcanvas.get_gizmos_manager().get_layout_scale();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f * f_scale));

    GLGizmoUtils::render_tooltip_button(imgui_wrapper, m_glcanvas, m_shortcuts_rotate, x, y);
    
    ImGui::SameLine();
    GLGizmoUtils::begin_right_aligned_buttons({ _L("Done") });
    if (imgui_wrapper->button(_L("Done"))) {
        m_glcanvas.reset_all_gizmos();
    }

    m_last_active_item = current_active_id;
    last_rotate_input_window_width = ImGui::GetWindowWidth();
    ImGui::PopStyleVar();
    imgui_wrapper->end();

    // BBS
    ImGui::PopStyleVar();
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
    // ORCA
    float coord_combo_width = std::max({
        imgui_wrapper->calc_text_size(_L("World")).x,
        imgui_wrapper->calc_text_size(_L("Object")).x,
        imgui_wrapper->calc_text_size(_L("Part")).x
    }) + imgui_wrapper->calc_text_size("xxx"sv).x + imgui_wrapper->scaled(3.5f);
    float label_max = std::max({
        imgui_wrapper->calc_text_size(_L_CONTEXT("Scale", "Noun")).x,
        imgui_wrapper->calc_text_size(_L("Size")).x
    });
    float caption_max = std::max(label_max, coord_combo_width - 3 * space_size);
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
    std::vector<std::string> modes     = {_u8L("World"), _u8L("Object"), _u8L("Part")}; // ORCA use shorter terms to make UI more compact
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

    ImGuiWrapper::push_combo_style(m_glcanvas.get_scale());
    bool combox_changed = false;
    if (render_combo(imgui_wrapper, "", modes, selection_idx, 0, coord_combo_width)) {
        combox_changed = true;
    }
    ImGuiWrapper::pop_combo_style();
    // After pop_combo_style(): its light popup background would hide the tooltip's white text.
    if (ImGui::IsItemHovered()) {
        auto tooltip_str = _L("Coordinate system used for transform actions.");
        imgui_wrapper->tooltip(tooltip_str, imgui_wrapper->calc_text_size(tooltip_str).x + 3 * space_size);
    }

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
    imgui_wrapper->text(_L_CONTEXT("Scale", "Noun"));
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

    ImGui::SameLine(caption_max + 3 * unit_size + 4 * space_size + end_text_size);
    if (reset_button(imgui_wrapper, m_show_clear_scale))
        reset_scale_value();

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

    ImGui::AlignTextToFramePadding();
    bool is_avoid_one_update{false};
    if (combox_changed) {
        combox_changed = false;
        set_coordinates_type((ECoordinatesType) selection_idx);
        UpdateAndShow(true);
        is_avoid_one_update = true;
    }

    auto uniform_scale_size =imgui_wrapper->calc_text_size(_L("Uniform scale")).x;
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
    //    imgui_wrapper->bbl_checkbox(_L("Uniform scale"), uniform_scale_only);
    //    imgui_wrapper->disabled_end();
    //} else {
        imgui_wrapper->bbl_checkbox(_L("Uniform scale"), uniform_scale);
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
    
    ImGui::Separator();
    float f_scale = m_glcanvas.get_gizmos_manager().get_layout_scale();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f * f_scale));

    GLGizmoUtils::render_tooltip_button(imgui_wrapper, m_glcanvas, m_shortcuts_scale, x, y);

    ImGui::SameLine();
    GLGizmoUtils::begin_right_aligned_buttons({ _L("Done") });
    if (imgui_wrapper->button(_L("Done"))) {
        m_glcanvas.reset_all_gizmos();
    }

    m_last_active_item = current_active_id;

    last_scale_input_window_width = ImGui::GetWindowWidth();
    imgui_wrapper->end();

    //BBS
    ImGui::PopStyleVar(1);
    ImGuiWrapper::pop_toolbar_style();
}


} //namespace GUI
} //namespace Slic3r
