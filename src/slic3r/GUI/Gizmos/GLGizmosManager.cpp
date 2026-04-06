#include "libslic3r/libslic3r.h"
#include "GLGizmosManager.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/Utils/UndoRedo.hpp"
#include "slic3r/GUI/NotificationManager.hpp"

#include "slic3r/GUI/Gizmos/GLGizmoMove.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoScale.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoRotate.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoFlatten.hpp"
//#include "slic3r/GUI/Gizmos/GLGizmoSlaSupports.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoFdmSupports.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoFuzzySkin.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoBrimEars.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoCut.hpp"
//#include "slic3r/GUI/Gizmos/GLGizmoFaceDetector.hpp"
//#include "slic3r/GUI/Gizmos/GLGizmoHollow.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoSeam.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoMmuSegmentation.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoSimplify.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoEmboss.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoSVG.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoMeshBoolean.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoAssembly.hpp"

#include "libslic3r/format.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <wx/glcanvas.h>

namespace Slic3r {
namespace GUI {
//BBS: GUI refactor: to support top layout
#if BBS_TOOLBAR_ON_TOP
const float GLGizmosManager::Default_Icons_Size = 40;
#else
const float GLGizmosManager::Default_Icons_Size = 64;
#endif

GLGizmosManager::GLGizmosManager(GLCanvas3D& parent)
    : m_parent(parent)
    , m_enabled(false)
    , m_icons_texture_dirty(true)
    , m_current(Undefined)
    , m_hover(Undefined)
    , m_tooltip("")
    , m_serializing(false)
    //BBS: GUI refactor: add object manipulation in gizmo
    , m_object_manipulation(parent)
{
    m_timer_set_color.Bind(wxEVT_TIMER, &GLGizmosManager::on_set_color_timer, this);
}

std::vector<size_t> GLGizmosManager::get_selectable_idxs() const
{
    std::vector<size_t> out;
    out.reserve(m_gizmos.size());
    if (m_parent.get_canvas_type() == GLCanvas3D::CanvasAssembleView) {
        for (size_t i = 0; i < m_gizmos.size(); ++i)
            if (m_gizmos[i]->get_sprite_id() == (unsigned int) Move ||
                m_gizmos[i]->get_sprite_id() == (unsigned int) Rotate ||
                m_gizmos[i]->get_sprite_id() == (unsigned int) Measure ||
                m_gizmos[i]->get_sprite_id() == (unsigned int) Assembly ||
                m_gizmos[i]->get_sprite_id() == (unsigned int) MmSegmentation)
                out.push_back(i);
    }
    else {
        for (size_t i = 0; i < m_gizmos.size(); ++i)
            if (m_gizmos[i]->is_selectable())
                out.push_back(i);
    }
    return out;
}

//BBS: GUI refactor: GLToolbar&&Gizmo adjust
GLGizmosManager::EType GLGizmosManager::get_gizmo_from_mouse(const Vec2d &mouse_pos) const
{
    if (! m_enabled)
        return Undefined;

    float icons_size = m_layout.scaled_icons_size();
    float border     = m_layout.scaled_border();

    //BBS: GUI refactor: GLToolbar&&Gizmo adjust
    //float space_width = GLGizmosManager::Default_Icons_Size * wxGetApp().toolbar_icon_scale();;
    float top_x;
    if (m_parent.get_canvas_type() == GLCanvas3D::CanvasAssembleView) {
        const float cnv_w = (float)m_parent.get_canvas_size().get_width();

        top_x = 0.5f * cnv_w + 0.5f * (m_parent.get_assembly_paint_toolbar_width());
    } else {
        const float separator_width = m_parent.get_separator_toolbar_width();

        top_x = m_parent.get_main_toolbar_offset();
        top_x += m_parent.get_main_toolbar_width() + separator_width / 2 + border;
    }
    float top_y = 0;
    float stride_x = m_layout.scaled_stride_x();

    // is mouse vertically in the area?
    //if ((border <= (float)mouse_pos(0) && ((float)mouse_pos(0) <= border + icons_size))) {
    if (((top_y + border) <= (float)mouse_pos(1)) && ((float)mouse_pos(1) <= (top_y + border + icons_size))) {
        // which icon is it on?
        int from_left = (float) mouse_pos(0) - top_x < 0 ? -1 : (int) ((float) mouse_pos(0) - top_x) / stride_x;
        if (from_left < 0)
            return Undefined;
        // is it really on the icon or already past the border?
        if ((float)mouse_pos(0) <= top_x + from_left * stride_x + icons_size) {
            std::vector<size_t> selectable = get_selectable_idxs();
            if (from_left < selectable.size())
                return static_cast<EType>(selectable[from_left]);
        }
    }
    return Undefined;
}

void GLGizmosManager::switch_gizmos_icon_filename()
{
    m_background_texture.metadata.filename = m_is_dark ? "toolbar_background_dark.png" : "toolbar_background.png";
    m_background_texture.metadata.left = 16;
    m_background_texture.metadata.top = 16;
    m_background_texture.metadata.right = 16;
    m_background_texture.metadata.bottom = 16;
    if (!m_background_texture.metadata.filename.empty())
        m_background_texture.texture.load_from_file(resources_dir() + "/images/" + m_background_texture.metadata.filename, false, GLTexture::SingleThreaded, false);

    for (auto& gizmo : m_gizmos) {
        gizmo->on_change_color_mode(m_is_dark);
        switch (gizmo->get_sprite_id())
        {
        case(EType::Move):
            gizmo->set_icon_filename(m_is_dark ? "toolbar_move_dark.svg" : "toolbar_move.svg");
            break;
        case(EType::Rotate):
            gizmo->set_icon_filename(m_is_dark ? "toolbar_rotate_dark.svg" : "toolbar_rotate.svg");
            break;
        case(EType::Scale):
            gizmo->set_icon_filename(m_is_dark ? "toolbar_scale_dark.svg" : "toolbar_scale.svg");
            break;
        case(EType::Flatten):
            gizmo->set_icon_filename(m_is_dark ? "toolbar_flatten_dark.svg" : "toolbar_flatten.svg");
            break;
        case(EType::Cut):
            gizmo->set_icon_filename(m_is_dark ? "toolbar_cut_dark.svg" : "toolbar_cut.svg");
            break;
        case(EType::FdmSupports):
            gizmo->set_icon_filename(m_is_dark ? "toolbar_support_dark.svg" : "toolbar_support.svg");
            break;
        case(EType::Seam):
            gizmo->set_icon_filename(m_is_dark ? "toolbar_seam_dark.svg" : "toolbar_seam.svg");
            break;
        case(EType::Emboss):
            gizmo->set_icon_filename(m_is_dark ? "toolbar_text_dark.svg" : "toolbar_text.svg");
            break;
        case(EType::MmSegmentation):
            gizmo->set_icon_filename(m_is_dark ? "mmu_segmentation_dark.svg" : "mmu_segmentation.svg");
            break;
        case(EType::FuzzySkin):
            gizmo->set_icon_filename(m_is_dark ? "toolbar_fuzzy_skin_paint_dark.svg" : "toolbar_fuzzy_skin_paint.svg");
            break;
        case(EType::MeshBoolean):
            gizmo->set_icon_filename(m_is_dark ? "toolbar_meshboolean_dark.svg" : "toolbar_meshboolean.svg");
            break;
        case (EType::Measure):
            gizmo->set_icon_filename(m_is_dark ? "toolbar_measure_dark.svg" : "toolbar_measure.svg");
            break;
        case (EType::Assembly):
            gizmo->set_icon_filename(m_is_dark ? "toolbar_assembly_dark.svg" : "toolbar_assembly.svg");
            break;
        case (EType::BrimEars):
            gizmo->set_icon_filename(m_is_dark ? "toolbar_brimears_dark.svg" : "toolbar_brimears.svg");
            break;
        }

    }
}

bool GLGizmosManager::init()
{
    if (!m_gizmos.empty())
        return true;
    init_icon_textures();

    m_background_texture.metadata.filename = m_is_dark ? "toolbar_background_dark.png" : "toolbar_background.png";
    m_background_texture.metadata.left = 16;
    m_background_texture.metadata.top = 16;
    m_background_texture.metadata.right = 16;
    m_background_texture.metadata.bottom = 16;

    if (!m_background_texture.metadata.filename.empty())
    {
        if (!m_background_texture.texture.load_from_file(resources_dir() + "/images/" + m_background_texture.metadata.filename, false, GLTexture::SingleThreaded, false))
            return false;
    }

    // Order of gizmos in the vector must match order in EType!
    //BBS: GUI refactor: add obj manipulation
    m_gizmos.clear();
    unsigned int sprite_id = 0;
    m_gizmos.emplace_back(new GLGizmoMove3D(m_parent, m_is_dark ? "toolbar_move_dark.svg" : "toolbar_move.svg", EType::Move, &m_object_manipulation));
    m_gizmos.emplace_back(new GLGizmoRotate3D(m_parent, m_is_dark ? "toolbar_rotate_dark.svg" : "toolbar_rotate.svg", EType::Rotate, &m_object_manipulation));
    m_gizmos.emplace_back(new GLGizmoScale3D(m_parent, m_is_dark ? "toolbar_scale_dark.svg" : "toolbar_scale.svg", EType::Scale, &m_object_manipulation));
    m_gizmos.emplace_back(new GLGizmoFlatten(m_parent, m_is_dark ? "toolbar_flatten_dark.svg" : "toolbar_flatten.svg", EType::Flatten));
    m_gizmos.emplace_back(new GLGizmoCut3D(m_parent, m_is_dark ? "toolbar_cut_dark.svg" : "toolbar_cut.svg", EType::Cut));
    m_gizmos.emplace_back(new GLGizmoMeshBoolean(m_parent, m_is_dark ? "toolbar_meshboolean_dark.svg" : "toolbar_meshboolean.svg", EType::MeshBoolean));
    m_gizmos.emplace_back(new GLGizmoFdmSupports(m_parent, m_is_dark ? "toolbar_support_dark.svg" : "toolbar_support.svg", EType::FdmSupports));
    m_gizmos.emplace_back(new GLGizmoSeam(m_parent, m_is_dark ? "toolbar_seam_dark.svg" : "toolbar_seam.svg", EType::Seam));
    m_gizmos.emplace_back(new GLGizmoFuzzySkin(m_parent, m_is_dark ? "toolbar_fuzzy_skin_paint_dark.svg" : "toolbar_fuzzy_skin_paint.svg", EType::FuzzySkin));
    m_gizmos.emplace_back(new GLGizmoMmuSegmentation(m_parent, m_is_dark ? "mmu_segmentation_dark.svg" : "mmu_segmentation.svg", EType::MmSegmentation));
    m_gizmos.emplace_back(new GLGizmoEmboss(m_parent, m_is_dark ? "toolbar_text_dark.svg" : "toolbar_text.svg", EType::Emboss));
    m_gizmos.emplace_back(new GLGizmoSVG(m_parent));
    m_gizmos.emplace_back(new GLGizmoMeasure(m_parent, m_is_dark ? "toolbar_measure_dark.svg" : "toolbar_measure.svg", EType::Measure));
    m_gizmos.emplace_back(new GLGizmoAssembly(m_parent, m_is_dark ? "toolbar_assembly_dark.svg" : "toolbar_assembly.svg", EType::Assembly));
    m_gizmos.emplace_back(new GLGizmoSimplify(m_parent, "reduce_triangles.svg", EType::Simplify));
    m_gizmos.emplace_back(new GLGizmoBrimEars(m_parent, m_is_dark ? "toolbar_brimears_dark.svg" : "toolbar_brimears.svg", EType::BrimEars));
    //m_gizmos.emplace_back(new GLGizmoSlaSupports(m_parent, "sla_supports.svg", sprite_id++));
    //m_gizmos.emplace_back(new GLGizmoFaceDetector(m_parent, "face recognition.svg", sprite_id++));
    //m_gizmos.emplace_back(new GLGizmoHollow(m_parent, "hollow.svg", sprite_id++));

    m_common_gizmos_data.reset(new CommonGizmosDataPool(&m_parent));
    if(!m_assemble_view_data)
        m_assemble_view_data.reset(new AssembleViewDataPool(&m_parent));

    for (auto& gizmo : m_gizmos) {
        if (! gizmo->init()) {
            m_gizmos.clear();
            return false;
        }
        gizmo->set_common_data_pool(m_common_gizmos_data.get());
        gizmo->on_change_color_mode(m_is_dark);
    }

    m_current = Undefined;
    m_hover = Undefined;
    m_highlight = std::pair<EType, bool>(Undefined, false);

    return true;
}
std::map<int, void *> GLGizmosManager::icon_list = {};
bool GLGizmosManager::init_icon_textures()
{
    if (icon_list.size() > 0) {
        return true;
    }
    ImTextureID texture_id;

    icon_list.clear();
    if (IMTexture::load_from_svg_file(Slic3r::resources_dir() + "/images/toolbar_reset.svg", 16, 16, texture_id))
        icon_list.insert(std::make_pair((int)IC_TOOLBAR_RESET, texture_id));
    else
        return false;

    if (IMTexture::load_from_svg_file(Slic3r::resources_dir() + "/images/toolbar_reset_hover.svg", 16, 16, texture_id))
        icon_list.insert(std::make_pair((int)IC_TOOLBAR_RESET_HOVER, texture_id));
    else
        return false;

    if (IMTexture::load_from_svg_file(Slic3r::resources_dir() + "/images/toolbar_reset_zero.svg", 16, 16, texture_id))
        icon_list.insert(std::make_pair((int) IC_TOOLBAR_RESET_ZERO, texture_id));
    else
        return false;

    if (IMTexture::load_from_svg_file(Slic3r::resources_dir() + "/images/toolbar_reset_zero_hover.svg", 16, 16, texture_id))
        icon_list.insert(std::make_pair((int) IC_TOOLBAR_RESET_ZERO_HOVER, texture_id));
    else
        return false;

    if (IMTexture::load_from_svg_file(Slic3r::resources_dir() + "/images/toolbar_tooltip.svg", 25, 25, texture_id)) // ORCA: Use same resolution with gizmos to prevent blur on icon
        icon_list.insert(std::make_pair((int)IC_TOOLBAR_TOOLTIP, texture_id));
    else
        return false;

    if (IMTexture::load_from_svg_file(Slic3r::resources_dir() + "/images/toolbar_tooltip_hover.svg", 25, 25, texture_id)) // ORCA: Use same resolution with gizmos to prevent blur on icon
        icon_list.insert(std::make_pair((int)IC_TOOLBAR_TOOLTIP_HOVER, texture_id));
    else
        return false;

    if (IMTexture::load_from_svg_file(Slic3r::resources_dir() + "/images/canvas_menu.svg", 72, 72, texture_id))
        icon_list.insert(std::make_pair((int) IC_CANVAS_MENU, texture_id));
    else
        return false;

    if (IMTexture::load_from_svg_file(Slic3r::resources_dir() + "/images/canvas_menu_hover.svg", 72, 72, texture_id))
        icon_list.insert(std::make_pair((int) IC_CANVAS_MENU_HOVER, texture_id));
    else
        return false;

    if (IMTexture::load_from_svg_file(Slic3r::resources_dir() + "/images/canvas_menu_dark.svg", 72, 72, texture_id))
        icon_list.insert(std::make_pair((int) IC_CANVAS_MENU_DARK, texture_id));
    else
        return false;

    if (IMTexture::load_from_svg_file(Slic3r::resources_dir() + "/images/canvas_menu_dark_hover.svg", 72, 72, texture_id))
        icon_list.insert(std::make_pair((int) IC_CANVAS_MENU_DARK_HOVER, texture_id));
    else
        return false;

    if (IMTexture::load_from_svg_file(Slic3r::resources_dir() + "/images/canvas_zoom.svg", 72, 72, texture_id))
        icon_list.insert(std::make_pair((int) IC_CANVAS_ZOOM, texture_id));
    else
        return false;

    if (IMTexture::load_from_svg_file(Slic3r::resources_dir() + "/images/canvas_zoom_hover.svg", 72, 72, texture_id))
        icon_list.insert(std::make_pair((int) IC_CANVAS_ZOOM_HOVER, texture_id));
    else
        return false;

    if (IMTexture::load_from_svg_file(Slic3r::resources_dir() + "/images/canvas_zoom_dark.svg", 72, 72, texture_id))
        icon_list.insert(std::make_pair((int) IC_CANVAS_ZOOM_DARK, texture_id));
    else
        return false;

    if (IMTexture::load_from_svg_file(Slic3r::resources_dir() + "/images/canvas_zoom_dark_hover.svg", 72, 72, texture_id))
        icon_list.insert(std::make_pair((int) IC_CANVAS_ZOOM_DARK_HOVER, texture_id));
    else
        return false;
    
    return true;
}

float GLGizmosManager::get_layout_scale()
{
    return m_layout.scale;
}

bool GLGizmosManager::init_arrow(const std::string& filename)
{
    if (m_arrow_texture.get_id() != 0)
        return true;

    const std::string path = resources_dir() + "/images/";
    return (!filename.empty()) ? m_arrow_texture.load_from_svg_file(path + filename, false, false, false, 1000) : false;
}

void GLGizmosManager::set_overlay_icon_size(float size)
{
    if (m_layout.icons_size != size)
    {
        m_layout.icons_size = size;
        m_icons_texture_dirty = true;
    }
}

void GLGizmosManager::set_overlay_scale(float scale)
{
    if (m_layout.scale != scale)
    {
        m_layout.scale = scale;
        m_icons_texture_dirty = true;
    }
}

void GLGizmosManager::refresh_on_off_state()
{
    if (m_serializing || m_current == Undefined || m_gizmos.empty())
        return;

    // FS: Why update data after Undefined gizmo activation?
    if (!m_gizmos[m_current]->is_activable() && activate_gizmo(Undefined))
        update_data(); 
}

void GLGizmosManager::reset_all_states()
{
    if (! m_enabled || m_serializing)
        return;

    const EType current = get_current_type();
    if (current != Undefined)
        // close any open gizmo
        open_gizmo(current);

    activate_gizmo(Undefined);
    // Orca: do not clear hover state, as Emboss gizmo can be used without selection
    //m_hover = Undefined;
}

bool GLGizmosManager::open_gizmo(EType type)
{
    int idx = static_cast<int>(type);

    // re-open same type cause closing
    if (m_current == type) type = Undefined;

    if (m_gizmos[idx]->is_activable() && activate_gizmo(type)) {
        // remove update data into gizmo itself
        update_data();
#ifdef __WXOSX__
        m_parent.post_event(SimpleEvent(wxEVT_PAINT));
#endif
        return true;
    }
    return false;
}


bool GLGizmosManager::check_gizmos_closed_except(EType type) const
{
    if (get_current_type() != type && get_current_type() != Undefined) {
        wxGetApp().plater()->get_notification_manager()->push_notification(
                    NotificationType::CustomSupportsAndSeamRemovedAfterRepair,
                    NotificationManager::NotificationLevel::PrintInfoNotificationLevel,
                    _u8L("Error: Please close all toolbar menus first"));
        return false;
    }
    return true;
}

void GLGizmosManager::set_hover_id(int id)
{
	// Measure and assembly handles hover by itself
    if (m_current == EType::Measure || m_current == EType::Assembly) { return; }
    if (!m_enabled || m_current == Undefined)
        return;

    m_gizmos[m_current]->set_hover_id(id);
}

void GLGizmosManager::update_assemble_view_data()
{
    if (m_assemble_view_data) {
        if (!wxGetApp().plater()->get_assmeble_canvas3D()->get_wxglcanvas()->IsShown())
            m_assemble_view_data->update(AssembleViewDataID(0));
        else
            m_assemble_view_data->update(AssembleViewDataID((int)AssembleViewDataID::ModelObjectsInfo | (int)AssembleViewDataID::ModelObjectsClipper));
    }
}

void GLGizmosManager::update_data()
{
    if (!m_enabled) return;

    const Selection& selection = m_parent.get_selection();

    if (m_common_gizmos_data)
        m_common_gizmos_data->update(get_current()
                                   ? get_current()->get_requirements()
                                   : CommonGizmosDataID(0));
    if (m_current != Undefined) m_gizmos[m_current]->data_changed(m_serializing);

    // Orca: hack: Fix issue that flatten gizmo faces not updated after reload from disk
    if (m_current != Flatten && !m_gizmos.empty()) m_gizmos[Flatten]->data_changed(m_serializing);

    //BBS: GUI refactor: add object manipulation in gizmo
    if (!selection.is_empty()) {
        m_object_manipulation.update_ui_from_settings();
        m_object_manipulation.UpdateAndShow(true);
    }
}

bool GLGizmosManager::is_running() const
{
    if (!m_enabled)
        return false;

    //GLGizmoBase* curr = get_current();
    //return (curr != nullptr) ? (curr->get_state() == GLGizmoBase::On) : false;
    return m_current != Undefined;
}

bool GLGizmosManager::handle_shortcut(int key)
{
    if (!m_enabled)
        return false;

    auto is_key = [pressed_key = key](int gizmo_key) { return (gizmo_key == pressed_key - 64) || (gizmo_key == pressed_key - 96); };
    // allowe open shortcut even when selection is empty    
    if (GLGizmoBase* gizmo_emboss = m_gizmos[Emboss].get();
        is_key(gizmo_emboss->get_shortcut_key())) {
        dynamic_cast<GLGizmoEmboss *>(gizmo_emboss)->on_shortcut_key();
        return true;
    }

    if (m_parent.get_selection().is_empty())
        return false;

    auto is_gizmo = [is_key](const std::unique_ptr<GLGizmoBase> &gizmo) {
        return gizmo->is_activable() && is_key(gizmo->get_shortcut_key());
    };
    auto it = std::find_if(m_gizmos.begin(), m_gizmos.end(), is_gizmo);

    if (it == m_gizmos.end())
        return false;

    EType gizmo_type = EType(it - m_gizmos.begin());
    return open_gizmo(gizmo_type);
}

bool GLGizmosManager::is_dragging() const
{
    if (! m_enabled || m_current == Undefined)
        return false;

    return m_gizmos[m_current]->is_dragging();
}

// Returns true if the gizmo used the event to do something, false otherwise.
bool GLGizmosManager::gizmo_event(SLAGizmoEventType action, const Vec2d& mouse_position, bool shift_down, bool alt_down, bool control_down)
{
    if (!m_enabled || m_gizmos.empty())
        return false;

    /*if (m_current == SlaSupports)
        return dynamic_cast<GLGizmoSlaSupports*>(m_gizmos[SlaSupports].get())->gizmo_event(action, mouse_position, shift_down, alt_down, control_down);
    else if (m_current == Hollow)
        return dynamic_cast<GLGizmoHollow*>(m_gizmos[Hollow].get())->gizmo_event(action, mouse_position, shift_down, alt_down, control_down);
    else*/ if (m_current == FdmSupports)
        return dynamic_cast<GLGizmoFdmSupports*>(m_gizmos[FdmSupports].get())->gizmo_event(action, mouse_position, shift_down, alt_down, control_down);
    else if (m_current == Seam)
        return dynamic_cast<GLGizmoSeam*>(m_gizmos[Seam].get())->gizmo_event(action, mouse_position, shift_down, alt_down, control_down);
    else if (m_current == MmSegmentation)
        return dynamic_cast<GLGizmoMmuSegmentation*>(m_gizmos[MmSegmentation].get())->gizmo_event(action, mouse_position, shift_down, alt_down, control_down);
    else if (m_current == Measure)
        return dynamic_cast<GLGizmoMeasure *>(m_gizmos[Measure].get())->gizmo_event(action, mouse_position, shift_down, alt_down, control_down);
    else if (m_current == Assembly)
        return dynamic_cast<GLGizmoAssembly *>(m_gizmos[Assembly].get())->gizmo_event(action, mouse_position, shift_down, alt_down, control_down);
    else if (m_current == Cut)
        return dynamic_cast<GLGizmoCut3D*>(m_gizmos[Cut].get())->gizmo_event(action, mouse_position, shift_down, alt_down, control_down);
    else if (m_current == FuzzySkin)
        return dynamic_cast<GLGizmoFuzzySkin*>(m_gizmos[FuzzySkin].get())->gizmo_event(action, mouse_position, shift_down, alt_down, control_down);
    else if (m_current == MeshBoolean)
        return dynamic_cast<GLGizmoMeshBoolean*>(m_gizmos[MeshBoolean].get())->gizmo_event(action, mouse_position, shift_down, alt_down, control_down);
    else if (m_current == BrimEars)
        return dynamic_cast<GLGizmoBrimEars*>(m_gizmos[BrimEars].get())->gizmo_event(action, mouse_position, shift_down, alt_down, control_down);
    else
        return false;
}

bool GLGizmosManager::is_paint_gizmo()
{
    return m_current == EType::FdmSupports ||
           m_current == EType::MmSegmentation ||
           m_current == EType::FuzzySkin ||
           m_current == EType::Seam;
}

bool GLGizmosManager::is_allow_select_all() {
    if (m_current == Undefined || m_current == EType::Move||
        m_current == EType::Rotate ||
        m_current == EType::Scale) {
        return true;
    }
    return false;
}

ClippingPlane GLGizmosManager::get_clipping_plane() const
{
    if (! m_common_gizmos_data
     || ! m_common_gizmos_data->object_clipper()
     || m_common_gizmos_data->object_clipper()->get_position() == 0.)
        return ClippingPlane::ClipsNothing();
    else {
        const ClippingPlane& clp = *m_common_gizmos_data->object_clipper()->get_clipping_plane();
        return ClippingPlane(-clp.get_normal(), clp.get_data()[3]);
    }
}

ClippingPlane GLGizmosManager::get_assemble_view_clipping_plane() const
{
    if (!m_assemble_view_data
        || !m_assemble_view_data->model_objects_clipper()
        || m_assemble_view_data->model_objects_clipper()->get_position() == 0.)
        return ClippingPlane::ClipsNothing();
    else {
        const ClippingPlane& clp = *m_assemble_view_data->model_objects_clipper()->get_clipping_plane();
        return ClippingPlane(-clp.get_normal(), clp.get_data()[3]);
    }
}

bool GLGizmosManager::wants_reslice_supports_on_undo() const
{
    return false;
    // return (m_current == SlaSupports
    //     && dynamic_cast<const GLGizmoSlaSupports*>(m_gizmos.at(SlaSupports).get())->has_backend_supports());
}

void GLGizmosManager::on_change_color_mode(bool is_dark) {
    m_is_dark = is_dark;
}

void GLGizmosManager::render_current_gizmo() const
{
    if (!m_enabled || m_current == Undefined)
        return;

    m_gizmos[m_current]->render();
}

void GLGizmosManager::render_painter_gizmo()
{
    // This function shall only be called when current gizmo is
    // derived from GLGizmoPainterBase.

    if (!m_enabled || m_current == Undefined)
        return;

    auto *gizmo = dynamic_cast<GLGizmoPainterBase*>(get_current());
    assert(gizmo); // check the precondition
    gizmo->render_painter_gizmo();
}

void GLGizmosManager::render_painter_assemble_view() const
{
    if (m_assemble_view_data && m_assemble_view_data->model_objects_clipper())
        m_assemble_view_data->model_objects_clipper()->render_cut();
}

void GLGizmosManager::render_overlay()
{
    if (!m_enabled)
        return;

    if (m_icons_texture_dirty)
        generate_icons_texture();

    do_render_overlay();
}

std::string GLGizmosManager::get_tooltip() const
{
    if (!m_tooltip.empty())
        return m_tooltip;

    const GLGizmoBase* curr = get_current();
    return (curr != nullptr) ? curr->get_tooltip() : "";
}

bool GLGizmosManager::on_mouse_wheel(const wxMouseEvent &evt)
{
    bool processed = false;

    if (/*m_current == SlaSupports || m_current == Hollow ||*/ m_current == FdmSupports || m_current == Seam || m_current == MmSegmentation || m_current == FuzzySkin || m_current == BrimEars) {
        float rot = (float)evt.GetWheelRotation() / (float)evt.GetWheelDelta();
        if (gizmo_event((rot > 0.f ? SLAGizmoEventType::MouseWheelUp : SLAGizmoEventType::MouseWheelDown), Vec2d::Zero(), evt.ShiftDown(), evt.AltDown()
            // BBS
#ifdef __WXOSX_MAC__
            , evt.RawControlDown()
#else
            , evt.ControlDown()
#endif
            ))
            processed = true;
    }

    return processed;
}

bool GLGizmosManager::gizmos_toolbar_on_mouse(const wxMouseEvent &mouse_event) {
    assert(m_enabled);
    // keep information about events to process
    struct MouseCapture
    {
        bool left = false;
        bool middle = false;
        bool right  = false;
        bool exist_tooltip = false;
        MouseCapture() = default;
        bool any() const { return left || middle || right; }
        void reset() {
            left   = false;
            middle = false;
            right  = false;
        }
    };
    static MouseCapture mc;

    // wxCoord == int --> wx/types.h
    Vec2i32 mouse_coord(mouse_event.GetX(), mouse_event.GetY());
    Vec2d mouse_pos = mouse_coord.cast<double>();

    EType gizmo = get_gizmo_from_mouse(mouse_pos);
    bool  selected_gizmo = gizmo != Undefined;

    // fast reaction on move mouse
    if (mouse_event.Moving()) {
        assert(!mc.any());
        if (selected_gizmo) {
            mc.exist_tooltip = true;
            update_hover_state(gizmo);
            // at this moment is enebled to process mouse move under gizmo
            // tools bar e.g. Do not interupt dragging. 
            return false;
        } else if (mc.exist_tooltip) {
            // first move out of gizmo tool bar - unselect tooltip
            mc.exist_tooltip = false;
            update_hover_state(Undefined);
            return false;
        }
        return false;
    }

    if (selected_gizmo) {
        // mouse is above toolbar
        if (mouse_event.LeftDown() || mouse_event.LeftDClick()) {
            mc.left = true;
            if (gizmo == Emboss) {
                GLGizmoBase *gizmo_emboss = m_gizmos[Emboss].get();
                dynamic_cast<GLGizmoEmboss *>(gizmo_emboss)->on_shortcut_key();
            } else {
                open_gizmo(gizmo);
            }
            return true;
        }
        else if (mouse_event.RightDown()) {
            mc.right  = true;
            return true;
        }
        else if (mouse_event.MiddleDown()) {
            mc.middle = true;
            return true;
        }
    }

    if (mc.any()) {
        // Check if exist release of event started above toolbar?
        if (mouse_event.Dragging()) {
            if (!selected_gizmo && mc.exist_tooltip) {
                // dragging out of gizmo let tooltip disapear
                mc.exist_tooltip = false;
                update_hover_state(Undefined);
            }
            // draging start on toolbar so no propagation into scene
            return true;
        }
        else if (mc.left && mouse_event.LeftUp()) {
            mc.left = false;
            return true;
        }
        else if (mc.right && mouse_event.RightUp()) {
            mc.right = false;
            return true;
        }
        else if (mc.middle && mouse_event.MiddleUp()) {
            mc.middle = false;
            return true;
        }
    
        // event out of window is not porocessed
        // left down on gizmo -> keep down -> move out of window -> release left
        if (mouse_event.Leaving()) mc.reset();
    }
    return false;
}

bool GLGizmosManager::on_mouse(const wxMouseEvent &mouse_event)
{
    if (!m_enabled) return false;

    // tool bar wants to use event?
    if (gizmos_toolbar_on_mouse(mouse_event)) return true;

    // current gizmo wants to use event?
    if (m_current != Undefined &&
        // check if gizmo override method could be slower than simple call virtual function
        // &m_gizmos[m_current]->on_mouse != &GLGizmoBase::on_mouse &&
        m_gizmos[m_current]->on_mouse(mouse_event))
        return true;

    if (mouse_event.RightUp() && m_current != EType::Undefined && !m_parent.is_mouse_dragging()) {
        // Prevent default right context menu in gizmos
        return true;
    }
        
    return false;
}

bool GLGizmosManager::on_char(wxKeyEvent& evt)
{
    // see include/wx/defs.h enum wxKeyCode
    int keyCode = evt.GetKeyCode();
    int ctrlMask = wxMOD_CONTROL;

    bool processed = false;

    if ((evt.GetModifiers() & ctrlMask) != 0) {
        switch (keyCode)
        {
#ifdef __APPLE__
        case 'a':
        case 'A':
#else /* __APPLE__ */
        case WXK_CONTROL_A:
#endif /* __APPLE__ */
        {
            //// Sla gizmo selects all support points
            //if ((m_current == SlaSupports || m_current == Hollow) && gizmo_event(SLAGizmoEventType::SelectAll))
            //    processed = true;

            break;
        }
        }
    }
    else if (!evt.HasModifiers()) {
        switch (keyCode)
        {
        // key ESC
        case WXK_ESCAPE:
        {
            if (m_current != Undefined) {
                if ((m_current == Measure || m_current == Assembly) && gizmo_event(SLAGizmoEventType::Escape)) {
                    // do nothing
                } else
                //if ((m_current != SlaSupports) || !gizmo_event(SLAGizmoEventType::DiscardChanges))
                    reset_all_states();

                processed = true;
            }
            break;
        }
        //skip some keys when gizmo
        case 'A':
        case 'a':
        {
            if (is_running()) {
                processed = true;
            }
            break;
        }
        //case WXK_RETURN:
        //{
        //    if ((m_current == SlaSupports) && gizmo_event(SLAGizmoEventType::ApplyChanges))
        //        processed = true;

        //    break;
        //}

        //case 'r' :
        //case 'R' :
        //{
            //if ((m_current == SlaSupports || m_current == Hollow || m_current == FdmSupports || m_current == Seam || m_current == MmSegmentation || m_current == FuzzySkin) && gizmo_event(SLAGizmoEventType::ResetClippingPlane))
            //    processed = true;

            //break;
        //}


        case WXK_BACK:
        case WXK_DELETE: {
            if ((m_current == Cut || m_current == Measure || m_current == Assembly) && gizmo_event(SLAGizmoEventType::Delete))
                processed = true;
            break;
        }
        //case 'A':
        //case 'a':
        //{
        //    if (m_current == SlaSupports)
        //    {
        //        gizmo_event(SLAGizmoEventType::AutomaticGeneration);
        //        // set as processed no matter what's returned by gizmo_event() to avoid the calling canvas to process 'A' as arrange
        //        processed = true;
        //    }
        //    break;
        //}
        //case 'M':
        //case 'm':
        //{
        //    if ((m_current == SlaSupports) && gizmo_event(SLAGizmoEventType::ManualEditing))
        //        processed = true;

        //    break;
        //}
        //case 'F':
        //case 'f':
        //{
           /* if (m_current == Scale)
            {
                if (!is_dragging())
                    wxGetApp().plater()->scale_selection_to_fit_print_volume();

                processed = true;
            }*/

            //break;
        //}
        // BBS: Skip all keys when in gizmo. This is necessary for 3D text tool.
        default:
        {
            //if (is_running() && m_current == EType::Text) {
            //    processed = true;
            //}
            break;
        }
        }
    }

    if (!processed && !evt.HasModifiers()) {
        if (handle_shortcut(keyCode))
            processed = true;
    }

    if (processed)
        m_parent.set_as_dirty();

    return processed;
}

bool GLGizmosManager::on_key(wxKeyEvent& evt)
{
    int keyCode = evt.GetKeyCode();
    bool processed = false;

    if (evt.GetEventType() == wxEVT_KEY_UP)
    {
        if (/*m_current == SlaSupports || m_current == Hollow ||*/ m_current == BrimEars)
        {
            bool is_editing = true;
            bool is_rectangle_dragging = false;

            /*if (m_current == SlaSupports) {
                GLGizmoSlaSupports* gizmo = dynamic_cast<GLGizmoSlaSupports*>(get_current());
                is_editing = gizmo->is_in_editing_mode();
                is_rectangle_dragging = gizmo->is_selection_rectangle_dragging();
            } else*/ if (m_current == BrimEars) {
                GLGizmoBrimEars* gizmo = dynamic_cast<GLGizmoBrimEars*>(get_current());
                is_rectangle_dragging = gizmo->is_selection_rectangle_dragging();
            }
            /*else {
                GLGizmoHollow* gizmo = dynamic_cast<GLGizmoHollow*>(get_current());
                is_rectangle_dragging = gizmo->is_selection_rectangle_dragging();
            }*/

            if (keyCode == WXK_SHIFT)
            {
                // shift has been just released - SLA gizmo might want to close rectangular selection.
                if (gizmo_event(SLAGizmoEventType::ShiftUp) || (is_editing && is_rectangle_dragging))
                    processed = true;
            }
            else if (keyCode == WXK_ALT)
            {
                // alt has been just released - SLA gizmo might want to close rectangular selection.
                if (gizmo_event(SLAGizmoEventType::AltUp) || (is_editing && is_rectangle_dragging))
                    processed = true;
            }
            else if (keyCode == WXK_CONTROL) {
                gizmo_event(SLAGizmoEventType::CtrlUp);
            }
        }
        // BBS
        if (m_current == MmSegmentation && keyCode > '0' && keyCode <= '9') {
            // capture number key
            processed = true;
        }
        if (m_current == Measure || m_current == Assembly) {
            if (keyCode == WXK_CONTROL)
                gizmo_event(SLAGizmoEventType::CtrlUp, Vec2d::Zero(), evt.ShiftDown(), evt.AltDown(), evt.CmdDown());
            else if (keyCode == WXK_SHIFT)
                gizmo_event(SLAGizmoEventType::ShiftUp, Vec2d::Zero(), evt.ShiftDown(), evt.AltDown(), evt.CmdDown());
        }
//        if (processed)
//            m_parent.set_cursor(GLCanvas3D::Standard);
    }
    else if (evt.GetEventType() == wxEVT_KEY_DOWN)
    {
        /*if ((m_current == SlaSupports) && ((keyCode == WXK_SHIFT) || (keyCode == WXK_ALT))
          && dynamic_cast<GLGizmoSlaSupports*>(get_current())->is_in_editing_mode())
        {
//            m_parent.set_cursor(GLCanvas3D::Cross);
            processed = true;
        }
        else*/ if  ((m_current == BrimEars) && ((keyCode == WXK_SHIFT) || (keyCode == WXK_ALT)))
        {
            processed = true;
        }
        else if (m_current == Cut)
        {
            auto do_move = [this, &processed](double delta_z) {
                GLGizmoCut3D* cut = dynamic_cast<GLGizmoCut3D*>(get_current());
                cut->shift_cut(delta_z);
                processed = true;
            };

            switch (keyCode)
            {
            case WXK_NUMPAD_UP:   case WXK_UP:   { do_move(1.0); break; }
            case WXK_NUMPAD_DOWN: case WXK_DOWN: { do_move(-1.0); break; }
            case WXK_SHIFT :      case WXK_ALT: {
                processed = get_current()->is_in_editing_mode();
            }
            default: { break; }
            }
        }
        else if (m_current == Simplify && keyCode == WXK_ESCAPE) {
            GLGizmoSimplify *simplify = dynamic_cast<GLGizmoSimplify *>(get_current());
            if (simplify != nullptr)
                processed = simplify->on_esc_key_down();
        }
        // BBS
        else if (m_current == MmSegmentation) {
            GLGizmoMmuSegmentation* mmu_seg = dynamic_cast<GLGizmoMmuSegmentation*>(get_current());
            if (mmu_seg != nullptr) {
                if (keyCode >= WXK_NUMPAD0 && keyCode <= WXK_NUMPAD9) {
                    keyCode = keyCode- WXK_NUMPAD0+'0';
                }
                if (keyCode >= '0' && keyCode <= '9') {
                    if (keyCode == '1' && !m_timer_set_color.IsRunning()) {
                        m_timer_set_color.StartOnce(500);
                        processed = true;
                    }
                    else if (keyCode < '7' && m_timer_set_color.IsRunning()) {
                        processed = mmu_seg->on_number_key_down(keyCode - '0'+10);
                        m_timer_set_color.Stop();
                    }
                    else {
                        processed = mmu_seg->on_number_key_down(keyCode - '0');
                    }
                }
                else if (keyCode == 'F' || keyCode == 'T' || keyCode == 'S' || keyCode == 'C' || keyCode == 'H' || keyCode == 'G') {
                    processed = mmu_seg->on_key_down_select_tool_type(keyCode);
                    if (processed) {
                        // force extra frame to automatically update window size
                        wxGetApp().imgui()->set_requires_extra_frame();
                    }
                }
            }
        }
        else if (m_current == FdmSupports) {
            GLGizmoFdmSupports* fdm_support = dynamic_cast<GLGizmoFdmSupports*>(get_current());
            if (fdm_support != nullptr && (keyCode == 'F' || keyCode == 'S' || keyCode == 'C' || keyCode == 'G')) {
                processed = fdm_support->on_key_down_select_tool_type(keyCode);
            }
            if (processed) {
                // force extra frame to automatically update window size
                wxGetApp().imgui()->set_requires_extra_frame();
            }
        }
        else if (m_current == Seam) {
            GLGizmoSeam* seam = dynamic_cast<GLGizmoSeam*>(get_current());
            if (seam != nullptr && (keyCode == 'S' || keyCode == 'C')) {
                processed = seam->on_key_down_select_tool_type(keyCode);
            }
            if (processed) {
                // force extra frame to automatically update window size
                wxGetApp().imgui()->set_requires_extra_frame();
            }
        } else if (m_current == Measure || m_current == Assembly) {
            if (keyCode == WXK_CONTROL)
                gizmo_event(SLAGizmoEventType::CtrlDown, Vec2d::Zero(), evt.ShiftDown(), evt.AltDown(), evt.CmdDown());
            else if (keyCode == WXK_SHIFT)
                gizmo_event(SLAGizmoEventType::ShiftDown, Vec2d::Zero(), evt.ShiftDown(), evt.AltDown(), evt.CmdDown());
        }
    }

    if (processed)
        m_parent.set_as_dirty();

    return processed;
}

void GLGizmosManager::on_set_color_timer(wxTimerEvent& evt)
{
    if (m_current == MmSegmentation) {
        GLGizmoMmuSegmentation* mmu_seg = dynamic_cast<GLGizmoMmuSegmentation*>(get_current());
        mmu_seg->on_number_key_down(1);
        m_parent.set_as_dirty();
    }
}

void GLGizmosManager::update_after_undo_redo(const UndoRedo::Snapshot& snapshot)
{
    update_data();
    m_serializing = false;
    // if (m_current == SlaSupports
    //  && snapshot.snapshot_data.flags & UndoRedo::SnapshotData::RECALCULATE_SLA_SUPPORTS)
    //     dynamic_cast<GLGizmoSlaSupports*>(m_gizmos[SlaSupports].get())->reslice_SLA_supports(true);
}

void GLGizmosManager::render_background(float left, float top, float right, float bottom, float border_w, float border_h) const
{
    const unsigned int tex_id = m_background_texture.texture.get_id();
    const float tex_width = float(m_background_texture.texture.get_width());
    const float tex_height = float(m_background_texture.texture.get_height());
    if (tex_id != 0 && tex_width > 0 && tex_height > 0) {
        //BBS: GUI refactor: remove the corners of gizmo
        const float inv_tex_width  = 1.0f / tex_width;
        const float inv_tex_height = 1.0f / tex_height;

        const float internal_left_uv   = float(m_background_texture.metadata.left) * inv_tex_width;
        const float internal_right_uv  = 1.0f - float(m_background_texture.metadata.right) * inv_tex_width;
        const float internal_top_uv    = 1.0f - float(m_background_texture.metadata.top) * inv_tex_height;
        const float internal_bottom_uv = float(m_background_texture.metadata.bottom) * inv_tex_height;

        GLTexture::render_sub_texture(tex_id, left, right, bottom, top, { { internal_left_uv, internal_bottom_uv }, { internal_right_uv, internal_bottom_uv }, { internal_right_uv, internal_top_uv }, { internal_left_uv, internal_top_uv } });

        /*
        const float internal_left   = left + border_w;
        const float internal_right  = right - border_w;
        const float internal_top    = top - border_h;
        const float internal_bottom = bottom + border_h;

        // float left_uv = 0.0f;
        const float right_uv = 1.0f;
        const float top_uv = 1.0f;
        const float bottom_uv = 0.0f;

        const float internal_left_uv   = float(m_background_texture.metadata.left) * inv_tex_width;
        const float internal_right_uv  = 1.0f - float(m_background_texture.metadata.right) * inv_tex_width;
        const float internal_top_uv    = 1.0f - float(m_background_texture.metadata.top) * inv_tex_height;
        const float internal_bottom_uv = float(m_background_texture.metadata.bottom) * inv_tex_height;

        // top-left corner
        GLTexture::render_sub_texture(tex_id, left, internal_left, internal_top, top, { { internal_left_uv, internal_bottom_uv }, { internal_right_uv, internal_bottom_uv }, { internal_right_uv, internal_top_uv }, { internal_left_uv, internal_top_uv } });

        // top edge
        GLTexture::render_sub_texture(tex_id, internal_left, internal_right, internal_top, top, { { internal_left_uv, internal_top_uv }, { internal_right_uv, internal_top_uv }, { internal_right_uv, top_uv }, { internal_left_uv, top_uv } });

        // top-right corner
        GLTexture::render_sub_texture(tex_id, internal_right, right, internal_top, top, { { internal_right_uv, internal_top_uv }, { right_uv, internal_top_uv }, { right_uv, top_uv }, { internal_right_uv, top_uv } });

        // center-left edge
        GLTexture::render_sub_texture(tex_id, left, internal_left, internal_bottom, internal_top, { { internal_left_uv, internal_bottom_uv }, { internal_right_uv, internal_bottom_uv }, { internal_right_uv, internal_top_uv }, { internal_left_uv, internal_top_uv } });

        // center
        GLTexture::render_sub_texture(tex_id, internal_left, internal_right, internal_bottom, internal_top, { { internal_left_uv, internal_bottom_uv }, { internal_right_uv, internal_bottom_uv }, { internal_right_uv, internal_top_uv }, { internal_left_uv, internal_top_uv } });

        // center-right edge
        GLTexture::render_sub_texture(tex_id, internal_right, right, internal_bottom, internal_top, { { internal_right_uv, internal_bottom_uv }, { right_uv, internal_bottom_uv }, { right_uv, internal_top_uv }, { internal_right_uv, internal_top_uv } });

        // bottom-left corner
        GLTexture::render_sub_texture(tex_id, left, internal_left, bottom, internal_bottom, { { internal_left_uv, internal_bottom_uv }, { internal_right_uv, internal_bottom_uv }, { internal_right_uv, internal_top_uv }, { internal_left_uv, internal_top_uv } });

        // bottom edge
        GLTexture::render_sub_texture(tex_id, internal_left, internal_right, bottom, internal_bottom, { { internal_left_uv, bottom_uv }, { internal_right_uv, bottom_uv }, { internal_right_uv, internal_bottom_uv }, { internal_left_uv, internal_bottom_uv } });

        // bottom-right corner
        GLTexture::render_sub_texture(tex_id, internal_right, right, bottom, internal_bottom, { { internal_right_uv, bottom_uv }, { right_uv, bottom_uv }, { right_uv, internal_bottom_uv }, { internal_right_uv, internal_bottom_uv } });
        */
    }
}

void GLGizmosManager::render_arrow(const GLCanvas3D& parent, EType highlighted_type) const
{
    const std::vector<size_t> selectable_idxs = get_selectable_idxs();
    if (selectable_idxs.empty())
        return;
    float cnv_w = (float)m_parent.get_canvas_size().get_width();
    float inv_zoom = (float)wxGetApp().plater()->get_camera().get_inv_zoom();
    float height = get_scaled_total_height();
    float zoomed_border = m_layout.scaled_border() * inv_zoom;
    float zoomed_top_x = (-0.5f * cnv_w) * inv_zoom;
    float zoomed_top_y = (0.5f * height) * inv_zoom;
    zoomed_top_x += zoomed_border;
    zoomed_top_y -= zoomed_border;
    float icons_size = m_layout.scaled_icons_size();
    float zoomed_icons_size = icons_size * inv_zoom;
    float zoomed_stride_y = m_layout.scaled_stride_y() * inv_zoom;
    for (size_t idx : selectable_idxs)
    {
        if (idx == highlighted_type) {
            int tex_width = m_icons_texture.get_width();
            int tex_height = m_icons_texture.get_height();
            unsigned int tex_id = m_arrow_texture.get_id();
            float inv_tex_width = (tex_width != 0.0f) ? 1.0f / tex_width : 0.0f;
            float inv_tex_height = (tex_height != 0.0f) ? 1.0f / tex_height : 0.0f;

            const float left_uv   = 0.0f;
            const float right_uv  = 1.0f;
            const float top_uv    = 1.0f;
            const float bottom_uv = 0.0f;

            float arrow_sides_ratio = (float)m_arrow_texture.get_height() / (float)m_arrow_texture.get_width();

            GLTexture::render_sub_texture(tex_id, zoomed_top_x + zoomed_icons_size * 1.2f, zoomed_top_x + zoomed_icons_size * 1.2f + zoomed_icons_size * 2.2f * arrow_sides_ratio, zoomed_top_y - zoomed_icons_size * 1.6f , zoomed_top_y + zoomed_icons_size * 0.6f, { { left_uv, bottom_uv }, { left_uv, top_uv }, { right_uv, top_uv }, { right_uv, bottom_uv } });
            break;
        }
        zoomed_top_y -= zoomed_stride_y;
    }
}

//BBS: GUI refactor: GLToolbar&&Gizmo adjust
//when rendering, {0, 0} is at the center, {-0.5, 0.5} at the left-top
void GLGizmosManager::do_render_overlay() const
{
    const std::vector<size_t> selectable_idxs = get_selectable_idxs();
    if (selectable_idxs.empty())
        return;

    const Size cnv_size = m_parent.get_canvas_size();
    const float cnv_w = (float)cnv_size.get_width();
    const float cnv_h = (float)cnv_size.get_height();

    if (cnv_w == 0 || cnv_h == 0)
        return;

    const float inv_cnv_w = 1.0f / cnv_w;
    const float inv_cnv_h = 1.0f / cnv_h;

    const float height = 2.0f * get_scaled_total_height() * inv_cnv_h;
    const float width  = 2.0f * get_scaled_total_width() * inv_cnv_w;
    const float border_h = 2.0f * m_layout.scaled_border() * inv_cnv_h;
    const float border_w = 2.0f * m_layout.scaled_border() * inv_cnv_w;

    float top_x;
    if (m_parent.get_canvas_type() == GLCanvas3D::CanvasAssembleView) {
        top_x = m_parent.get_assembly_paint_toolbar_width() * inv_cnv_w;
    }
    else {
        //BBS: GUI refactor: GLToolbar&&Gizmo adjust
        float main_toolbar_width = (float)m_parent.get_main_toolbar_width();
        float separator_width = (float)m_parent.get_separator_toolbar_width();
        //float space_width = GLGizmosManager::Default_Icons_Size * wxGetApp().toolbar_icon_scale();
        //float zoomed_top_x = 0.5f *(cnv_w + main_toolbar_width - 2 * space_width - width) * inv_zoom;

        int main_toolbar_left = -cnv_w + m_parent.get_main_toolbar_offset() * 2;
        //float zoomed_top_x = 0.5f *(main_toolbar_width + collapse_width - width - assemble_view_width) * inv_zoom;
        top_x = main_toolbar_left + main_toolbar_width * 2 + separator_width;
        top_x = top_x * inv_cnv_w;
    }
    float top_y = 1.0f;

    render_background(top_x, top_y, top_x + width, top_y - height, border_w, border_h);

    top_x += border_w;
    top_y -= border_h;

    const float icons_size_x = 2.0f * m_layout.scaled_icons_size() * inv_cnv_w;
    const float icons_size_y = 2.0f * m_layout.scaled_icons_size() * inv_cnv_h;
    //BBS: GUI refactor: GLToolbar&&Gizmo adjust
    const float stride_x = 2.0f * m_layout.scaled_stride_x() * inv_cnv_w;

    const unsigned int icons_texture_id = m_icons_texture.get_id();
    const int tex_width = m_icons_texture.get_width();
    const int tex_height = m_icons_texture.get_height();

    if (icons_texture_id == 0 || tex_width <= 1 || tex_height <= 1)
        return;

    const float du = (float)(tex_width - 1) / (6.0f * (float)tex_width); // 6 is the number of possible states if the icons
    const float dv = (float)(tex_height - 1) / (float)(m_gizmos.size() * tex_height);

    // tiles in the texture are spaced by 1 pixel
    const float u_offset = 1.0f / (float)tex_width;
    const float v_offset = 1.0f / (float)tex_height;

    bool is_render_current = false;

    for (size_t idx : selectable_idxs) {
        GLGizmoBase* gizmo = m_gizmos[idx].get();
        const unsigned int sprite_id = gizmo->get_sprite_id();
        // higlighted state needs to be decided first so its highlighting in every other state
        const int icon_idx = (m_highlight.first == idx ? (m_highlight.second ? 4 : 5) : (m_current == idx) ? 2 : ((m_hover == idx) ? 1 : (gizmo->is_activable()? 0 : 3)));

        const float u_left   = u_offset + icon_idx * du;
        const float u_right  = u_left + du - u_offset;
        const float v_top    = v_offset + sprite_id * dv;
        const float v_bottom = v_top + dv - v_offset;

        GLTexture::render_sub_texture(icons_texture_id, top_x, top_x + icons_size_x, top_y - icons_size_y, top_y, { { u_left, v_bottom }, { u_right, v_bottom }, { u_right, v_top }, { u_left, v_top } });
        if (idx == m_current
            // Orca: Show Svg dialog at the same place as emboss gizmo
            || (m_current == Svg && idx == Emboss)) {
            //BBS: GUI refactor: GLToolbar&&Gizmo adjust
            //render_input_window uses a different coordination(imgui)
            //1. no need to scale by camera zoom, set {0,0} at left-up corner for imgui
            //gizmo->render_input_window(width, 0.5f * cnv_h - zoomed_top_y * zoom, toolbar_top);
            m_gizmos[m_current]->render_input_window(0.5 * cnv_w + 0.5f * top_x * cnv_w, get_scaled_total_height(), cnv_h);

            is_render_current = true;
        }
        top_x += stride_x;
    }

    // BBS simplify gizmo is not a selected gizmo and need to render input window
    if (!is_render_current && m_current != Undefined) {
        m_gizmos[m_current]->render_input_window(0.5 * cnv_w + 0.5f * top_x * cnv_w, get_scaled_total_height(), cnv_h);
    }
}

float GLGizmosManager::get_scaled_total_height() const
{
//BBS: GUI refactor: to support top layout
#if BBS_TOOLBAR_ON_TOP
    return 2.0f * m_layout.scaled_border() + m_layout.scaled_icons_size();
#else
    return m_layout.scale * (2.0f * m_layout.border + (float)get_selectable_idxs().size() * m_layout.stride_y() - m_layout.gap_y);
#endif
}

float GLGizmosManager::get_scaled_total_width() const
{
//BBS: GUI refactor: to support top layout
#if BBS_TOOLBAR_ON_TOP
    return m_layout.scale * (2.0f * m_layout.border + (float)get_selectable_idxs().size() * m_layout.stride_x() - m_layout.gap_x);
#else
    return 2.0f * m_layout.scaled_border() + m_layout.scaled_icons_size();
#endif
}

GLGizmoBase* GLGizmosManager::get_current() const
{
    return ((m_current == Undefined) || m_gizmos.empty()) ? nullptr : m_gizmos[m_current].get();
}

GLGizmoBase* GLGizmosManager::get_gizmo(GLGizmosManager::EType type) const
{
    return ((type == Undefined) || m_gizmos.empty()) ? nullptr : m_gizmos[type].get();
}

GLGizmosManager::EType GLGizmosManager::get_gizmo_from_name(const std::string& gizmo_name) const
{
    std::vector<size_t> selectable_idxs = get_selectable_idxs();
    for (size_t idx = 0; idx < selectable_idxs.size(); ++idx)
    {
        std::string filename = m_gizmos[selectable_idxs[idx]]->get_icon_filename();
        filename = filename.substr(0, filename.find_first_of('.'));
        if (filename == gizmo_name)
            return (GLGizmosManager::EType)selectable_idxs[idx];
    }
    return GLGizmosManager::EType::Undefined;
}

bool GLGizmosManager::generate_icons_texture()
{
    std::string path = resources_dir() + "/images/";
    std::vector<std::string> filenames;
    for (size_t idx=0; idx<m_gizmos.size(); ++idx)
    {
        auto &gizmo = m_gizmos[idx];
        if (gizmo != nullptr)
        {
            const std::string& icon_filename = gizmo->get_icon_filename();
            if (!icon_filename.empty())
                filenames.push_back(path + icon_filename);
        }
    }

    std::vector<std::pair<int, bool>> states;
    states.push_back(std::make_pair(1, false)); // Activable
    states.push_back(std::make_pair(0, false)); // Hovered
    states.push_back(std::make_pair(0, true));  // Selected
    states.push_back(std::make_pair(2, false)); // Disabled
    states.push_back(std::make_pair(0, false)); // HighlightedShown
    states.push_back(std::make_pair(2, false)); // HighlightedHidden

    unsigned int sprite_size_px = (unsigned int)m_layout.scaled_icons_size();
//    // force even size
//    if (sprite_size_px % 2 != 0)
//        sprite_size_px += 1;

    bool res = m_icons_texture.load_from_svg_files_as_sprites_array(filenames, states, sprite_size_px, false);
    if (res)
        m_icons_texture_dirty = false;

    return res;
}

void GLGizmosManager::update_hover_state(const EType &type)
{
    assert(m_enabled);
    if (type == Undefined) { 
        m_hover = Undefined;
        m_tooltip.clear();
        return;
    }

    const GLGizmoBase &hovered_gizmo = *m_gizmos[type];
    m_hover = hovered_gizmo.is_activable() ? type : Undefined;    
    m_tooltip = hovered_gizmo.get_name();
}

bool GLGizmosManager::activate_gizmo(EType type)
{
    assert(!m_gizmos.empty());

    // already activated
    if (m_current == type) return true;

    if (m_current != Undefined) {
        // clean up previous gizmo
        GLGizmoBase &old_gizmo = *m_gizmos[m_current];
        old_gizmo.set_state(GLGizmoBase::Off);
        if (old_gizmo.get_state() != GLGizmoBase::Off)
            return false; // gizmo refused to be turned off, do nothing.

        old_gizmo.unregister_raycasters_for_picking();

        if (!m_serializing && old_gizmo.wants_enter_leave_snapshots())
            Plater::TakeSnapshot
                snapshot(wxGetApp().plater(),
                         old_gizmo.get_gizmo_leaving_text(),
                         UndoRedo::SnapshotType::LeavingGizmoWithAction);
    }

    if (type == Undefined) { 
        // it is deactivation of gizmo
        m_current = Undefined;
        return true;
    }

    // set up new gizmo
    GLGizmoBase& new_gizmo = *m_gizmos[type];
    if (!new_gizmo.is_activable()) return false;

    if (!m_serializing && new_gizmo.wants_enter_leave_snapshots())
        Plater::TakeSnapshot snapshot(wxGetApp().plater(),
                                      new_gizmo.get_gizmo_entering_text(),
                                      UndoRedo::SnapshotType::EnteringGizmo);

    m_current = type;
    new_gizmo.set_state(GLGizmoBase::On);
    if (new_gizmo.get_state() != GLGizmoBase::On) {
        m_current = Undefined;
        return false; // gizmo refused to be turned on.
    }

    new_gizmo.register_raycasters_for_picking();

    // sucessful activation of gizmo
    return true;
}


bool GLGizmosManager::grabber_contains_mouse() const
{
    if (!m_enabled)
        return false;

    GLGizmoBase* curr = get_current();
    return (curr != nullptr) ? (curr->get_hover_id() != -1) : false;
}


bool GLGizmosManager::is_in_editing_mode(bool error_notification) const
{
    /*if (m_current == SlaSupports && dynamic_cast<GLGizmoSlaSupports*>(get_current())->is_in_editing_mode()) {
        return true;
    } else*/ if (m_current == BrimEars) {
        dynamic_cast<GLGizmoBrimEars *>(get_current())->update_model_object();
        return false;
    } else {
        return false;
    }

}


bool GLGizmosManager::is_hiding_instances() const
{
    return (m_common_gizmos_data
         && m_common_gizmos_data->instances_hider()
         && m_common_gizmos_data->instances_hider()->is_valid());
}

std::string get_name_from_gizmo_etype(GLGizmosManager::EType type)
{
    switch (type) {
    case GLGizmosManager::EType::Move:
        return "Move";
    case GLGizmosManager::EType::Rotate:
        return "Rotate";
    case GLGizmosManager::EType::Scale:
        return "Scale";
    case GLGizmosManager::EType::Flatten:
        return "Flatten";
    case GLGizmosManager::EType::Cut:
        return "Cut";
    case GLGizmosManager::EType::MeshBoolean:
        return "MeshBoolean";
    case GLGizmosManager::EType::FdmSupports:
        return "FdmSupports";
    case GLGizmosManager::EType::Seam:
        return "Seam";
    case GLGizmosManager::EType::Emboss:
        return "Text";
    case GLGizmosManager::EType::MmSegmentation:
        return "Color Painting";
    case GLGizmosManager::EType::FuzzySkin:
        return "Fuzzy Skin Painting";
    default:
        return "";
    }
    return "";
}

} // namespace GUI
} // namespace Slic3r
