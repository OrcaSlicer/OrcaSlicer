#include "GLGizmoEmboss.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/Gizmos/GizmoObjectManipulation.hpp"
#include "slic3r/GUI/MainFrame.hpp" // to update title when add text
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/CameraUtils.hpp"
#include "slic3r/GUI/Jobs/EmbossJob.hpp"
#include "slic3r/GUI/Jobs/CreateFontNameImageJob.hpp"
#include "slic3r/GUI/Jobs/NotificationProgressIndicator.hpp"
#include "slic3r/Utils/WxFontUtils.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

#include "libslic3r/Geometry.hpp" // to range pi pi
#include "libslic3r/Timer.hpp" 

#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/ClipperUtils.hpp" // union_ex
#include "libslic3r/AppConfig.hpp"    // store/load font list
#include "libslic3r/Format/OBJ.hpp" // load obj file for default object
#include "libslic3r/BuildVolume.hpp"

#include "imgui/imgui_stdlib.h" // using std::string for inputs
#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui/imgui_internal.h>

#include <wx/font.h>
#include <wx/fontutil.h>
#include <wx/fontdlg.h>
#include <wx/fontenum.h>
#include <wx/display.h> // detection of change DPI
#include <wx/hashmap.h>

#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp> // serialize deserialize facenames
#include <boost/functional/hash.hpp>
#include <boost/filesystem.hpp>

// cache font list by cereal
#include <cereal/cereal.hpp>
#include <cereal/types/vector.hpp>
#include <cereal/types/string.hpp>
#include <cereal/archives/binary.hpp>

#include <GL/glew.h>
#include <chrono> // measure enumeration of fonts

// uncomment for easier debug
//#define ALLOW_DEBUG_MODE
#ifdef ALLOW_DEBUG_MODE
#define ALLOW_ADD_FONT_BY_FILE
#define ALLOW_ADD_FONT_BY_OS_SELECTOR
#define SHOW_WX_FONT_DESCRIPTOR // OS specific descriptor | file path --> in edit style <tree header>
#define SHOW_FONT_FILE_PROPERTY // ascent, descent, line gap, cache --> in advanced <tree header>
#define SHOW_CONTAIN_3MF_FIX // when contain fix matrix --> show gray '3mf' next to close button
#define SHOW_OFFSET_DURING_DRAGGING // when drag with text over surface visualize used center
#define SHOW_IMGUI_ATLAS
#define SHOW_ICONS_TEXTURE
#define SHOW_FINE_POSITION // draw convex hull around volume
#define DRAW_PLACE_TO_ADD_TEXT // Interactive draw of window position 
#define ALLOW_OPEN_NEAR_VOLUME
#endif // ALLOW_DEBUG_MODE

//#define USE_PIXEL_SIZE_IN_WX_FONT

using namespace Slic3r;
using namespace Slic3r::Emboss;
using namespace Slic3r::GUI;
using namespace Slic3r::GUI::Emboss;

namespace {
// TRN - Title in Undo/Redo stack after rotate with text around emboss axe
const std::string rotation_snapshot_name = L("Text rotate");
// NOTE: Translation is made in "m_parent.do_rotate()"

// TRN - Title in Undo/Redo stack after move with text along emboss axe - From surface
const std::string move_snapshot_name = L("Text move");
// NOTE: Translation is made in "m_parent.do_translate()"

template<typename T> struct Limit {
    // Limitation for view slider range in GUI
    MinMax<T> gui;
    // Real limits for setting exacts values
    MinMax<T> values;
};
// Variable keep limits for variables
static const struct Limits
{
    MinMax<double> emboss{0.01, 1e4}; // in mm
    MinMax<float> size_in_mm{0.1f, 1000.f}; // in mm
    Limit<float> boldness{{-.5f, .5f}, {-5e5f, 5e5f}}; // in font points
    Limit<float> skew{{-1.f, 1.f}, {-100.f, 100.f}}; // ration without unit
    MinMax<int>  char_gap{-20000, 20000}; // in font points
    MinMax<int>  line_gap{-20000, 20000}; // in font points
    // distance text object from surface
    MinMax<float> angle{-180.f, 180.f}; // in degrees
} limits;

static const float SELECTABLE_INNER_OFFSET = 8.0f;

/// <summary>
/// Prepare data for emboss
/// </summary>
/// <param name="text">Text to emboss</param>
/// <param name="style_manager">Keep actual selected style</param>
/// <param name="text_lines">Needed when transform per glyph</param>
/// <param name="selection">Needed for transform per glyph</param>
/// <param name="type">Define type of volume - side of surface(in / out)</param>
/// <param name="cancel">Cancel for previous job</param>
/// <returns>Base data for emboss text</returns>
std::unique_ptr<DataBase> create_emboss_data_base(
    const std::string& text,
    StyleManager& style_manager,
    TextLinesModel& text_lines,
    const Selection& selection,
    ModelVolumeType type,
    std::shared_ptr<std::atomic<bool>>& cancel);
CreateVolumeParams create_input(GLCanvas3D &canvas, const StyleManager::Style &style, RaycastManager &raycaster, ModelVolumeType volume_type);

/// <summary>
/// Move window for edit emboss text near to embossed object
/// NOTE: embossed object must be selected
/// </summary>
ImVec2 calc_fine_position(const Selection &selection, const ImVec2 &windows_size, const Size &canvas_size);

struct TextDataBase : public DataBase
{
    TextDataBase(DataBase &&parent, const FontFileWithCache &font_file, 
        TextConfiguration &&text_configuration, const EmbossProjection& projection);
    // Create shape from text + font configuration
    EmbossShape &create_shape() override;
    void write(ModelVolume &volume) const override;

private:
    //  Keep pointer on Data of font (glyph shapes)
    FontFileWithCache m_font_file;
    // font item is not used for create object
    TextConfiguration m_text_configuration;
};

// Loaded icons enum
// Have to match order of files in function GLGizmoEmboss::init_icons()
enum class IconType : unsigned {
    rename = 0,
    erase,
    add,
    save,
    undo,
    italic,
    unitalic,
    bold,
    unbold,
    system_selector,
    open_file,
    exclamation,
    lock,
    lock_bold,
    unlock,
    unlock_bold,
    align_horizontal_left,
    align_horizontal_center,
    align_horizontal_right,
    align_vertical_top,
    align_vertical_center,
    align_vertical_bottom,
    // automatic calc of icon's count
    _count
};
// Define rendered version of icon
enum class IconState : unsigned { activable = 0, hovered /*1*/, disabled /*2*/ };
// selector for icon by enum
const IconManager::Icon &get_icon(const IconManager::VIcons& icons, IconType type, IconState state);
// short call of Slic3r::GUI::button
bool draw_button(const IconManager::VIcons& icons, IconType type, bool disable = false);

struct FaceName
{
    wxString    wx_name;
    std::string name_truncated = "";
    size_t      texture_index  = 0;
    // State for generation of texture
    // when start generate create share pointers
    std::shared_ptr<std::atomic<bool>> cancel = nullptr;
    // R/W only on main thread - finalize of job
    std::shared_ptr<bool> is_created = nullptr;
};

// Implementation of forwarded struct
// Keep sorted list of loadable face names
struct Facenames
{
    // flag to keep need of enumeration fonts from OS
    // false .. wants new enumeration check by Hash
    // true  .. already enumerated(During opened combo box)
    bool is_init = false;

    bool has_truncated_names = false;

    // data of can_load() faces
    std::vector<FaceName> faces = {};
    std::vector<std::string> faces_names = {};
    // Sorter set of Non valid face names in OS
    std::vector<wxString> bad = {};

    // Configuration of font encoding
    static const wxFontEncoding encoding = wxFontEncoding::wxFONTENCODING_SYSTEM;

    // Identify if preview texture exists
    GLuint texture_id = 0;

    // protection for open too much font files together
    // Gtk:ERROR:../../../../gtk/gtkiconhelper.c:494:ensure_surface_for_gicon: assertion failed (error == NULL): Failed to load
    // /usr/share/icons/Yaru/48x48/status/image-missing.png: Error opening file /usr/share/icons/Yaru/48x48/status/image-missing.png: Too
    // many open files (g-io-error-quark, 31) This variable must exist until no CreateFontImageJob is running
    unsigned int count_opened_font_files = 0;

    // Configuration for texture height
    const int count_cached_textures = 32;

    // index for new generated texture index(must be lower than count_cached_textures)
    size_t texture_index = 0;

    // hash created from enumerated font from OS
    // check when new font was installed
    size_t hash = 0;

    // filtration pattern
    //std::string       search = "";
    //std::vector<bool> hide; // result of filtration
};

bool store(const Facenames &facenames);
bool load(Facenames &facenames);
void init_face_names(Facenames &facenames);
void init_truncated_names(Facenames &face_names, float max_width);

// This configs holds GUI layout size given by translated texts.
// etc. When language changes, GUI is recreated and this class constructed again,
// so the change takes effect. (info by GLGizmoFdmSupports.hpp)
struct GuiCfg
{
    // Detect invalid config values when change monitor DPI
    double screen_scale;
    bool   dark_mode = false;

    // Zero means it is calculated in init function
    float        height_of_volume_type_selector       = 0.f;
    float        input_width                          = 0.f;
    float        delete_pos_x                         = 0.f;
    float        max_style_name_width                 = 0.f;
    unsigned int icon_width                           = 0;
    float        max_tooltip_width                    = 0.f;

    // maximal width and height of style image
    Vec2i32 max_style_image_size = Vec2i32(0, 0);

    float indent                = 0.f;
    float input_offset          = 0.f;
    float advanced_input_offset = 0.f;
    float lock_offset           = 0.f;

    ImVec2 text_size;

    // maximal size of face name image
    Vec2i32 face_name_size             = Vec2i32(0, 0);
    float face_name_texture_offset_x = 0.f;

    // maximal texture generate jobs running at once
    unsigned int max_count_opened_font_files = 10;

    // Only translations needed for calc GUI size
    struct Translations
    {
        std::string font;
        std::string height;
        std::string depth;

        // advanced
        std::string use_surface;
        std::string per_glyph;
        std::string alignment;
        std::string char_gap;
        std::string line_gap;
        std::string boldness;
        std::string skew_ration;
        std::string from_surface;
        std::string rotation;
        std::string keep_up;
        std::string collection;
    };
    Translations translations;
};
GuiCfg create_gui_configuration();

void draw_font_preview(FaceName &face, const std::string &text, Facenames &faces, const GuiCfg &cfg, bool is_visible);
// for existing volume which is selected(could init different(to volume text) lines count when edit text)
void init_text_lines(TextLinesModel &text_lines, const Selection& selection, /* const*/ StyleManager &style_manager, unsigned count_lines=0);
} // namespace priv

// use private definition
struct GLGizmoEmboss::Facenames: public ::Facenames{};
struct GLGizmoEmboss::GuiCfg: public ::GuiCfg{};

GLGizmoEmboss::GLGizmoEmboss(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
    , m_gui_cfg(nullptr)
    , m_style_manager(m_imgui->get_glyph_ranges(), create_default_styles)
    , m_face_names(std::make_unique<Facenames>())
    , m_rotate_gizmo(parent, GLGizmoRotate::Axis::Z) // grab id = 2 (Z axis)
{
    m_rotate_gizmo.set_group_id(0);
    m_rotate_gizmo.set_force_local_coordinate(true);
    // to use https://fontawesome.com/ (copy & paste) unicode symbols from web
    // paste HEX unicode into notepad move cursor after unicode press [alt] + [x]
}

bool GLGizmoEmboss::create_volume(ModelVolumeType volume_type, const Vec2d& mouse_pos)
{
    if (!init_create(volume_type))
        return false;

    // NOTE: change style manager - be carefull with order changes
    DataBasePtr base = create_emboss_data_base(m_text, m_style_manager, m_text_lines, m_parent.get_selection(), volume_type, m_job_cancel);
    CreateVolumeParams input = create_input(m_parent, m_style_manager.get_style(), m_raycast_manager, volume_type);
    return start_create_volume(input, std::move(base), mouse_pos);
}

// Designed for create volume without information of mouse in scene
bool GLGizmoEmboss::create_volume(ModelVolumeType volume_type)
{
    if (!init_create(volume_type))
        return false;

    // NOTE: change style manager - be carefull with order changes
    DataBasePtr base = create_emboss_data_base(m_text, m_style_manager, m_text_lines, m_parent.get_selection(), volume_type, m_job_cancel);
    CreateVolumeParams input = create_input(m_parent, m_style_manager.get_style(), m_raycast_manager, volume_type);
    return start_create_volume_without_position(input, std::move(base));
}


void GLGizmoEmboss::on_shortcut_key() {
    set_volume_by_selection();
    if (m_volume == nullptr) {
        // No volume to select from selection so create volume.
        // NOTE: After finish job for creation emboss Text volume,
        // GLGizmoEmboss will be opened
        create_volume(ModelVolumeType::MODEL_PART);
    }
    else {
        // shortcut is pressed when text is selected soo start edit it.
        auto& mng = m_parent.get_gizmos_manager();
        if (mng.get_current_type() != GLGizmosManager::Emboss)
            mng.open_gizmo(GLGizmosManager::Emboss);
    }
}

bool GLGizmoEmboss::re_emboss(const ModelVolume &text_volume, std::shared_ptr<std::atomic<bool>> job_cancel)
{
    assert(text_volume.text_configuration.has_value());
    assert(text_volume.emboss_shape.has_value());
    if (!text_volume.text_configuration.has_value() || 
        !text_volume.emboss_shape.has_value())
        return false; // not valid text volume to re emboss
    const TextConfiguration &tc = *text_volume.text_configuration;
    const EmbossShape       &es = *text_volume.emboss_shape;
    const ImWchar* ranges = ImGui::GetIO().Fonts->GetGlyphRangesDefault();

    StyleManager style_manager(ranges, create_default_styles);
    StyleManager::Style style{tc.style, es.projection};
    if (!style_manager.load_style(style))
        return false; // can't load font

    TextLinesModel text_lines;
    const Selection &selection = wxGetApp().plater()->canvas3D()->get_selection();
    DataBasePtr base = create_emboss_data_base(tc.text, style_manager, text_lines, selection, text_volume.type(), job_cancel);
    DataUpdate  data{std::move(base), text_volume.id(), false};

    RaycastManager raycast_manager; // Nothing is cached now, so It need to create raycasters
    return start_update_volume(std::move(data), text_volume, selection, raycast_manager);
}

namespace{
ModelVolumePtrs prepare_volumes_to_slice(const ModelVolume &mv)
{
    const ModelVolumePtrs &volumes = mv.get_object()->volumes;
    ModelVolumePtrs        result;
    result.reserve(volumes.size());
    for (ModelVolume *volume : volumes) {
        // only part could be surface for volumes
        if (!volume->is_model_part())
            continue;

        // is selected volume
        if (mv.id() == volume->id())
            continue;

        result.push_back(volume);
    }
    return result;
}
} // namespace

bool GLGizmoEmboss::do_mirror(size_t axis)
{ 
    // is valid input
    assert(axis < 3);
    if (axis >= 3)
        return false;

    // is gizmo opened and initialized?
    assert(m_parent.get_gizmos_manager().get_current_type() == GLGizmosManager::Emboss);
    if (m_parent.get_gizmos_manager().get_current_type() != GLGizmosManager::Emboss)
        return false;

    const std::optional<TextConfiguration> &tc = m_volume->text_configuration;
    assert(tc.has_value());
    bool is_per_glyph = tc.has_value()? tc->style.prop.per_glyph : false;
        
    const std::optional<EmbossShape> &es = m_volume->emboss_shape;
    assert(es.has_value());
    bool use_surface = es.has_value()? es->projection.use_surface : false;
    if (!use_surface && !is_per_glyph) {
        // do normal mirror with fix matrix
        Selection &selection = m_parent.get_selection();
        selection.setup_cache();

        auto selection_mirror_fnc = [&selection, &axis]() { selection.mirror((Axis) axis, get_drag_transformation_type(selection)); };
        selection_transform(selection, selection_mirror_fnc);

        m_parent.do_mirror(L("Set Mirror"));
        wxGetApp().obj_manipul()->UpdateAndShow(true);
        return true;
    }

    Vec3d scale = Vec3d::Ones();
    scale[axis] = -1.;

    Transform3d tr = m_volume->get_matrix();
    if (es.has_value()) {
        const std::optional<Transform3d> &fix_tr = es->fix_3mf_tr;
        if (fix_tr.has_value())
            tr = tr * (fix_tr->inverse());
    }

    // mirror
    tr = tr * Eigen::Scaling(scale);

    if (is_per_glyph) { 
        // init textlines before mirroring on mirrored text volume transformation
        ModelVolumePtrs volumes = prepare_volumes_to_slice(*m_volume);
        m_text_lines.init(tr, volumes, m_style_manager, m_text_lines.get_lines().size());
    }

    m_volume->set_transformation(tr); 
    // setting to volume is not visible for user(not GLVolume)    
    // NOTE: Staff around volume transformation change is done in job finish
    return process();
}

namespace{
// verify correct volume type for creation of text
bool check(ModelVolumeType volume_type) {
    return volume_type == ModelVolumeType::MODEL_PART ||
           volume_type == ModelVolumeType::NEGATIVE_VOLUME ||
           volume_type == ModelVolumeType::PARAMETER_MODIFIER;
}
}

bool GLGizmoEmboss::init_create(ModelVolumeType volume_type)
{
    // check valid volume type
    if (!check(volume_type)){    
        BOOST_LOG_TRIVIAL(error) << "Can't create embossed volume with this type: " << (int) volume_type;
        return false;
    }

    if (!is_activable()) {
        BOOST_LOG_TRIVIAL(error) << "Can't create text. Gizmo is not activabled.";
        return false;
    }

    // Check can't be inside is_activable() cause crash
    // steps to reproduce: start App -> key 't' -> key 'delete'
    if (wxGetApp().obj_list()->has_selected_cut_object()) {
        BOOST_LOG_TRIVIAL(error) << "Can't create text on cut object";
        return false;
    }

    m_style_manager.discard_style_changes();

    // set default text
    m_text = _u8L("Embossed text");
    return true;
}

bool GLGizmoEmboss::on_mouse_for_rotation(const wxMouseEvent &mouse_event)
{
    if (mouse_event.Moving()) return false;

    bool used = use_grabbers(mouse_event);
    if (!m_dragging) return used;

    if (mouse_event.Dragging()) {
        // check that style is activ
        assert(m_style_manager.is_active_font());
        if (!m_style_manager.is_active_font())
            return used;

        std::optional<float> &angle_opt = m_style_manager.get_style().angle;
        dragging_rotate_gizmo(m_rotate_gizmo.get_angle(), angle_opt, m_rotate_start_angle, m_parent.get_selection());
    }
    return used;
}

bool GLGizmoEmboss::on_mouse_for_translate(const wxMouseEvent &mouse_event)
{
    // exist selected volume?
    if (m_volume == nullptr)
        return false;
    
    const Camera &camera = wxGetApp().plater()->get_camera();
    bool was_dragging = m_surface_drag.has_value();
    bool res = on_mouse_surface_drag(mouse_event, camera, m_surface_drag, m_parent, m_raycast_manager, UP_LIMIT);
    bool is_dragging = m_surface_drag.has_value();

    // End with surface dragging?
    if (was_dragging && !is_dragging) 
        volume_transformation_changed();
    
    // Start with dragging
    else if (!was_dragging && is_dragging) {
        // Cancel job to prevent interuption of dragging (duplicit result)
        if (m_job_cancel != nullptr)
            m_job_cancel->store(true);
    }

    // during drag
    else if (was_dragging && is_dragging) {
        // update scale of selected volume --> should be approx the same
        calculate_scale();

        // Recalculate angle for GUI
        if (!m_keep_up) {
            const Selection &selection = m_parent.get_selection();
            const GLVolume *gl_volume = get_selected_gl_volume(selection);
            assert(gl_volume != nullptr);
            assert(m_style_manager.is_active_font());
            if (gl_volume == nullptr || !m_style_manager.is_active_font())
                return res;
            m_style_manager.get_style().angle = calc_angle(selection);
        }

        volume_transformation_changing();
    }
    return res;
}

void GLGizmoEmboss::on_mouse_change_selection(const wxMouseEvent &mouse_event)
{
    static bool was_dragging = true;  
    if ((mouse_event.LeftUp() || mouse_event.RightUp()) && !was_dragging) {
        // is hovered volume closest hovered?
        int hovered_idx = m_parent.get_first_hover_volume_idx();
        if (hovered_idx < 0) 
            // unselect object
            return close();

        const GLVolumePtrs &gl_volumes = m_parent.get_volumes().volumes;
        auto hovered_idx_ = static_cast<size_t>(hovered_idx);
        if (hovered_idx_ >= gl_volumes.size())
            return close();
        
        const GLVolume *gl_volume = gl_volumes[hovered_idx_];
        if (gl_volume == nullptr)
            return close();

        const ModelVolume *volume = get_model_volume(*gl_volume, m_parent.get_model()->objects);
        if (volume == nullptr || !volume->text_configuration.has_value())
            // select volume without text configuration
            return close();

        // Reselection of text to another text
    }
    was_dragging = mouse_event.Dragging();

    // Hook When click on object for reselection must be on event left down not up
    if (mouse_event.LeftDown()) {
        // is hovered volume closest hovered?
        int hovered_idx = m_parent.get_first_hover_volume_idx();
        if (hovered_idx < 0)
            // Potentionaly move with camera (drag)
            return;

        const GLVolumePtrs &gl_volumes = m_parent.get_volumes().volumes;
        auto hovered_idx_ = static_cast<size_t>(hovered_idx);
        if (hovered_idx_ >= gl_volumes.size())
            return;
        const GLVolume *gl_volume = gl_volumes[hovered_idx_];
        if (gl_volume == nullptr)
            return;
        const ModelVolume *volume = get_model_volume(*gl_volume, m_parent.get_model()->objects);
        if (volume == nullptr)
            return;

        if (volume->text_configuration.has_value())        
            return; // Reselection of text to another text

        // select volume without text configuration
        return close();
    }

    // Hook When drag with scene by right mouse button
    // object it is selected after drag scene !!
    if (mouse_event.RightDown()) {
        // is hovered volume closest hovered?
        int hovered_idx = m_parent.get_first_hover_volume_idx();
        if (hovered_idx < 0)
            // Potentionaly move with camera (drag)
            return;

        const GLVolumePtrs &gl_volumes = m_parent.get_volumes().volumes;
        auto hovered_idx_ = static_cast<size_t>(hovered_idx);
        if (hovered_idx_ >= gl_volumes.size())
            return;
        const GLVolume *gl_volume = gl_volumes[hovered_idx_];
        if (gl_volume == nullptr)
            return;
        const ModelVolume *volume = get_model_volume(*gl_volume, m_parent.get_model()->objects);
        if (volume == nullptr)
            return;

        // is actual selected?
        if (m_volume->id() == volume->id())
            return;

        // select volume without text configuration
        return close();
    }
}

bool GLGizmoEmboss::on_mouse(const wxMouseEvent &mouse_event)
{
    // not selected volume
    if (m_volume == nullptr ||
        get_model_volume(m_volume_id, m_parent.get_selection().get_model()->objects) == nullptr ||
        !m_volume->text_configuration.has_value()) return false;

    if (on_mouse_for_rotation(mouse_event)) return true;
    if (on_mouse_for_translate(mouse_event)) return true;
    on_mouse_change_selection(mouse_event);
    return false;
}

void GLGizmoEmboss::volume_transformation_changing()
{
    if (m_volume == nullptr || !m_volume->text_configuration.has_value()) {
        assert(false);
        return;
    }
    const FontProp &prop = m_volume->text_configuration->style.prop;
    if (prop.per_glyph)
        init_text_lines(m_text_lines, m_parent.get_selection(), m_style_manager, m_text_lines.get_lines().size());
}

void GLGizmoEmboss::volume_transformation_changed()
{
    if (m_volume == nullptr || 
        !m_volume->text_configuration.has_value() ||
        !m_volume->emboss_shape.has_value() ||
        !m_style_manager.is_active_font()) {
        assert(false);
        return;
    }

    if (!m_keep_up) {
        // Re-Calculate current angle of up vector
        m_style_manager.get_style().angle = calc_angle(m_parent.get_selection());
    } else {
        // angle should be the same
        assert(is_approx(m_style_manager.get_style().angle, calc_angle(m_parent.get_selection())));
    }

    const TextConfiguration &tc = *m_volume->text_configuration;
    const EmbossShape &es = *m_volume->emboss_shape;

    bool per_glyph = tc.style.prop.per_glyph;
    if (per_glyph)
        init_text_lines(m_text_lines, m_parent.get_selection(), m_style_manager, m_text_lines.get_lines().size());

    bool use_surface = es.projection.use_surface;

    // Update surface by new position
    if (use_surface || per_glyph)
        process();
    else {
        // inform slicing process that model changed
        // SLA supports, processing
        // ensure on bed
        wxGetApp().plater()->changed_object(*m_volume->get_object());
    }

    // Show correct value of height & depth inside of inputs
    calculate_scale();
}

bool        GLGizmoEmboss::wants_enter_leave_snapshots() const { return true; }
std::string GLGizmoEmboss::get_gizmo_entering_text() const { return _u8L("Enter emboss gizmo"); }
std::string GLGizmoEmboss::get_gizmo_leaving_text() const { return _u8L("Leave emboss gizmo"); }
std::string GLGizmoEmboss::get_action_snapshot_name() const { return _u8L("Embossing actions"); }

bool GLGizmoEmboss::on_init()
{
    m_rotate_gizmo.init();
    ColorRGBA gray_color(.6f, .6f, .6f, .3f);
    m_rotate_gizmo.set_highlight_color(gray_color);

    // NOTE: It has special handling in GLGizmosManager::handle_shortcut
    m_shortcut_key = WXK_CONTROL_T;

    // initialize text styles
    m_style_manager.init(wxGetApp().app_config);

    // Set rotation gizmo upwardrotate
    m_rotate_gizmo.set_angle(PI / 2);
    return true;
}

std::string GLGizmoEmboss::on_get_name() const { return _u8L("Emboss"); }

void GLGizmoEmboss::on_render() {
    // no volume selected
    const Selection &selection = m_parent.get_selection();
    if (m_volume == nullptr ||
        get_model_volume(m_volume_id, selection.get_model()->objects) == nullptr)
        return;
    if (selection.is_empty()) return;

    // prevent get local coordinate system on multi volumes
    if (!selection.is_single_volume_or_modifier() && 
        !selection.is_single_volume_instance()) return;
    
    const GLVolume *gl_volume_ptr = m_parent.get_selection().get_first_volume();
    if (gl_volume_ptr == nullptr) return;

    if (m_text_lines.is_init()) {
        const Transform3d& tr = gl_volume_ptr->world_matrix();
        const auto &fix = m_volume->emboss_shape->fix_3mf_tr;
        if (fix.has_value()) 
            m_text_lines.render(tr * fix->inverse());
        else 
            m_text_lines.render(tr);
    }

    bool is_surface_dragging = m_surface_drag.has_value();
    bool is_parent_dragging = m_parent.is_mouse_dragging();
    // Do NOT render rotation grabbers when dragging object
    bool is_rotate_by_grabbers = m_dragging;
    if (is_rotate_by_grabbers || 
        (!is_surface_dragging && !is_parent_dragging)) {
        glsafe(::glClear(GL_DEPTH_BUFFER_BIT));
        m_rotate_gizmo.render();
    }
}

void GLGizmoEmboss::on_register_raycasters_for_picking(){
    m_rotate_gizmo.register_raycasters_for_picking();
}
void GLGizmoEmboss::on_unregister_raycasters_for_picking(){
    m_rotate_gizmo.unregister_raycasters_for_picking();
}

#ifdef SHOW_FINE_POSITION
// draw suggested position of window
static void draw_fine_position(const Selection &selection,
                               const Size      &canvas,
                               const ImVec2    &windows_size)
{
    const Selection::IndicesList& indices = selection.get_volume_idxs();
    // no selected volume
    if (indices.empty()) return;
    const GLVolume *volume = selection.get_volume(*indices.begin());
    // bad volume selected (e.g. deleted one)
    if (volume == nullptr) return;

    const Camera   &camera = wxGetApp().plater()->get_camera();
    Slic3r::Polygon hull   = CameraUtils::create_hull2d(camera, *volume);
    ImVec2          canvas_size(canvas.get_width(), canvas.get_height());
    ImVec2 offset = ImGuiWrapper::suggest_location(windows_size, hull,
                                                   canvas_size);
    Slic3r::Polygon rect(
        {Point(offset.x, offset.y), Point(offset.x + windows_size.x, offset.y),
         Point(offset.x + windows_size.x, offset.y + windows_size.y),
         Point(offset.x, offset.y + windows_size.y)});
    ImGuiWrapper::draw(hull);
    ImGuiWrapper::draw(rect);
}
#endif // SHOW_FINE_POSITION

#ifdef DRAW_PLACE_TO_ADD_TEXT
static void draw_place_to_add_text()
{
    ImVec2             mp = ImGui::GetMousePos();
    Vec2d              mouse_pos(mp.x, mp.y);
    const Camera      &camera = wxGetApp().plater()->get_camera();
    Vec3d              p1 = CameraUtils::get_z0_position(camera, mouse_pos);
    std::vector<Vec3d> rect3d{p1 + Vec3d(5, 5, 0), p1 + Vec3d(-5, 5, 0),
                              p1 + Vec3d(-5, -5, 0), p1 + Vec3d(5, -5, 0)};
    Points             rect2d = CameraUtils::project(camera, rect3d);
    ImGuiWrapper::draw(Slic3r::Polygon(rect2d));
}
#endif // DRAW_PLACE_TO_ADD_TEXT

#ifdef SHOW_OFFSET_DURING_DRAGGING
static void draw_mouse_offset(const std::optional<Vec2d> &offset)
{
    if (!offset.has_value()) return;
    // debug draw
    auto   draw_list = ImGui::GetOverlayDrawList();
    ImVec2 p1        = ImGui::GetMousePos();
    ImVec2 p2(p1.x + offset->x(), p1.y + offset->y());
    ImU32  color     = ImGui::GetColorU32(ImGuiWrapper::COL_ORANGE_LIGHT);
    float  thickness = 3.f;
    draw_list->AddLine(p1, p2, color, thickness);
}
#endif // SHOW_OFFSET_DURING_DRAGGING

void GLGizmoEmboss::on_render_input_window(float x, float y, float bottom_limit)
{
    assert(m_volume != nullptr);
    // Do not render window for not selected text volume
    if (m_volume == nullptr ||
        get_model_volume(m_volume_id, m_parent.get_selection().get_model()->objects) == nullptr ||
        !m_volume->text_configuration.has_value()) {
        // This closing could lead to bad behavior of undo/redo stack when unselection create snapshot before close
        close();
        return;
    }

    // Not known situation when could happend this is only for sure
    if (!m_is_unknown_font && !m_style_manager.is_active_font())
        create_notification_not_valid_font("No active font in style. Select correct one.");
    else if (!m_is_unknown_font && !m_style_manager.get_wx_font().IsOk())
        create_notification_not_valid_font("WxFont is not loaded properly.");

    double screen_scale = wxDisplay(wxGetApp().plater()).GetScaleFactor();

    // Orca
    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0, 5.0) * screen_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 4.0f * screen_scale);

    // Configuration creation
    if (m_gui_cfg == nullptr ||                   // Exist configuration - first run
        m_gui_cfg->screen_scale != screen_scale  ||// change of DPI
        m_gui_cfg->dark_mode != m_is_dark_mode // change of dark mode
        ) {
        // Create cache for gui offsets
        ::GuiCfg cfg = create_gui_configuration();
        cfg.screen_scale = screen_scale;
        cfg.dark_mode    = m_is_dark_mode;

        GuiCfg gui_cfg{std::move(cfg)};
        m_gui_cfg = std::make_unique<const GuiCfg>(std::move(gui_cfg));

        // change resolution regenerate icons
        init_icons();
        m_style_manager.clear_imgui_font();
    }

    // Draw origin position of text during dragging
    if (m_surface_drag.has_value()) {
        ImVec2 mouse_pos = ImGui::GetMousePos();
        ImVec2 center(
            mouse_pos.x + m_surface_drag->mouse_offset.x(),
            mouse_pos.y + m_surface_drag->mouse_offset.y());
        ImU32 color = ImGui::GetColorU32(
            m_surface_drag->exist_hit ? 
                ImVec4(1.f, 1.f, 1.f, .75f) : // transparent white
                ImVec4(1.f, .3f, .3f, .75f)
        ); // Warning color
        const float radius = 16.f;
        ImGuiWrapper::draw_cross_hair(center, radius, color);
    }

#ifdef SHOW_FINE_POSITION
    draw_fine_position(m_parent.get_selection(), m_parent.get_canvas_size(), min_window_size);
#endif // SHOW_FINE_POSITION
#ifdef DRAW_PLACE_TO_ADD_TEXT
    draw_place_to_add_text();
#endif // DRAW_PLACE_TO_ADD_TEXT
#ifdef SHOW_OFFSET_DURING_DRAGGING
    draw_mouse_offset(m_dragging_mouse_offset);
#endif // SHOW_OFFSET_DURING_DRAGGING

    static float last_y = 0.0f;
    static float last_h = 0.0f;

    // adjust window position to avoid overlap the view toolbar
    const float win_h = ImGui::GetWindowHeight();
    y = std::min(y, bottom_limit - win_h);
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
    if (last_h != win_h || last_y != y) {
        // ask canvas for another frame to render the window in the correct position
        m_imgui->set_requires_extra_frame();
        if (last_h != win_h)
            last_h = win_h;
        if (last_y != y)
            last_y = y;
    }
    
    GizmoImguiBegin(get_name(), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    draw_window();

    GizmoImguiEnd();

    // Orca
    ImGui::PopStyleVar(2);
    ImGuiWrapper::pop_toolbar_style();
}

void GLGizmoEmboss::on_set_state()
{
    // enable / disable bed from picking
    // Rotation gizmo must work through bed
    m_parent.set_raycaster_gizmos_on_top(m_state == GLGizmoBase::On);

    m_rotate_gizmo.set_state(m_state);

    // Closing gizmo. e.g. selecting another one
    if (m_state == GLGizmoBase::Off) {
        // refuse outgoing during text preview
        reset_volume();
        // Store order and last activ index into app.ini
        // TODO: what to do when can't store into file?
        m_style_manager.store_styles_to_app_config(false);
        remove_notification_not_valid_font();
    } else if (m_state == GLGizmoBase::On) {
        // to reload fonts from system, when install new one
        wxFontEnumerator::InvalidateCache();

        // Immediately after set state On is called function data_changed(), 
        // where one could distiguish undo/redo serialization from opening by letter 'T'
        // set_volume_by_selection();
    }
}

void GLGizmoEmboss::data_changed(bool is_serializing) {
    set_volume_by_selection();
    if (!is_serializing && m_volume == nullptr)
        close();
}

void GLGizmoEmboss::on_start_dragging() { m_rotate_gizmo.start_dragging(); }
void GLGizmoEmboss::on_stop_dragging()
{
    m_rotate_gizmo.stop_dragging();

    // This is fast fix for second try to rotate
    // When fixing, move grabber above text (not on side)
    m_rotate_gizmo.set_angle(PI/2);

    // apply rotation
    m_parent.do_rotate(rotation_snapshot_name);
    m_rotate_start_angle.reset();
    volume_transformation_changed();
}
void GLGizmoEmboss::on_dragging(const UpdateData &data) { m_rotate_gizmo.dragging(data); }

EmbossStyles GLGizmoEmboss::create_default_styles()
{
    wxFontEnumerator::InvalidateCache();
    wxArrayString facenames = wxFontEnumerator::GetFacenames(Facenames::encoding);

    wxFont wx_font_normal = *wxNORMAL_FONT;
#ifdef __APPLE__
    // Set normal font to helvetica when possible
    for (const wxString &facename : facenames) {
        if (facename.IsSameAs("Helvetica")) {
            wx_font_normal = wxFont(wxFontInfo().FaceName(facename).Encoding(Facenames::encoding));
            break;
        }
    }
#endif // __APPLE__

    // https://docs.wxwidgets.org/3.0/classwx_font.html
    // Predefined objects/pointers: wxNullFont, wxNORMAL_FONT, wxSMALL_FONT, wxITALIC_FONT, wxSWISS_FONT
    EmbossStyles styles = {
        WxFontUtils::create_emboss_style(wx_font_normal, _u8L("NORMAL")), // wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT)
        WxFontUtils::create_emboss_style(*wxSMALL_FONT, _u8L("SMALL")),  // A font using the wxFONTFAMILY_SWISS family and 2 points smaller than wxNORMAL_FONT.
        WxFontUtils::create_emboss_style(*wxITALIC_FONT, _u8L("ITALIC")), // A font using the wxFONTFAMILY_ROMAN family and wxFONTSTYLE_ITALIC style and of the same size of wxNORMAL_FONT.
        WxFontUtils::create_emboss_style(*wxSWISS_FONT, _u8L("SWISS")),  // A font identic to wxNORMAL_FONT except for the family used which is wxFONTFAMILY_SWISS.
        WxFontUtils::create_emboss_style(wxFont(10, wxFONTFAMILY_MODERN, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD), _u8L("MODERN")),        
    };

    // Not all predefined font for wx must be valid TTF, but at least one style must be loadable
    styles.erase(std::remove_if(styles.begin(), styles.end(), [](const EmbossStyle& style) {
        wxFont wx_font = WxFontUtils::create_wxFont(style);

        // check that face name is setabled
        if (style.prop.face_name.has_value()) {
            wxString face_name = wxString::FromUTF8(style.prop.face_name->c_str());
            wxFont wx_font_temp;
            if (!wx_font_temp.SetFaceName(face_name))
                return true;        
        }

        // Check that exsit valid TrueType Font for wx font
        return WxFontUtils::create_font_file(wx_font) == nullptr;
        }),styles.end()
    );

    // exist some valid style?
    if (!styles.empty())
        return styles;

    // No valid style in defult list
    // at least one style must contain loadable font
    wxFont wx_font;
    for (const wxString &face : facenames) {
        wx_font = wxFont(face);
        if (WxFontUtils::create_font_file(wx_font) != nullptr)
            break;
        wx_font = wxFont(); // NotOk
    }

    if (wx_font.IsOk()) {
        // use first alphabetic sorted installed font
        styles.push_back(WxFontUtils::create_emboss_style(wx_font, _u8L("First font")));
    } else {
        // On current OS is not installed any correct TTF font
        // use font packed with Slic3r
        std::string font_path = Slic3r::resources_dir() + "/fonts/NotoSans-Regular.ttf";
        styles.push_back(EmbossStyle{_u8L("Default font"), font_path, EmbossStyle::Type::file_path});
    }
    return styles;
}

namespace {

/// <summary>
/// Check installed fonts whether optional face name exist in installed fonts
/// </summary>
/// <param name="face_name_opt">Name from style - style.prop.face_name</param>
/// <param name="face_names">All installed good and bad fonts - not const must be possible to initialize it</param>
/// <returns>When it could be installed it contain value(potentionaly empty string)</returns>
std::optional<wxString> get_installed_face_name(const std::optional<std::string> &face_name_opt, ::Facenames& face_names)
{
    // Could exist OS without getter on face_name,
    // but it is able to restore font from descriptor
    // Soo default value must be TRUE
    if (!face_name_opt.has_value())
        return wxString();

    wxString face_name = wxString::FromUTF8(face_name_opt->c_str());

    // search in enumerated fonts
    // refresh list of installed font in the OS.
    init_face_names(face_names);
    face_names.is_init = false;

    auto cmp = [](const FaceName &fn, const wxString &wx_name) { return fn.wx_name < wx_name; };
    const std::vector<FaceName> &faces = face_names.faces;    
    // is font installed?
    if (auto it = std::lower_bound(faces.begin(), faces.end(), face_name, cmp);
        it != faces.end() && it->wx_name == face_name)
        return face_name;

    const std::vector<wxString> &bad = face_names.bad;
    auto it_bad = std::lower_bound(bad.begin(), bad.end(), face_name);
    if (it_bad == bad.end() || *it_bad != face_name) {
        // check if wx allowed to set it up - another encoding of name
        wxFontEnumerator::InvalidateCache();
        wxFont wx_font_;                                                                          // temporary structure
        if (wx_font_.SetFaceName(face_name) && WxFontUtils::create_font_file(wx_font_) != nullptr // can load TTF file?
        ) {
            return wxString();
            // QUESTION: add this name to allowed faces?
            // Could create twin of font face name
            // When not add it will be hard to select it again when change font
        }
    }
    return {}; // not installed    
}

void init_text_lines(TextLinesModel &text_lines, const Selection& selection, /* const*/ StyleManager &style_manager, unsigned count_lines)
{    
    const GLVolume *gl_volume_ptr = selection.get_first_volume();
    if (gl_volume_ptr == nullptr)
        return;
    const GLVolume        &gl_volume = *gl_volume_ptr;
    const ModelObjectPtrs &objects   = selection.get_model()->objects;
    const ModelVolume *mv_ptr = get_model_volume(gl_volume, objects);
    if (mv_ptr == nullptr)
        return;
    const ModelVolume &mv = *mv_ptr;
    if (mv.is_the_only_one_part())
        return;

    const std::optional<EmbossShape> &es_opt = mv.emboss_shape;
    if (!es_opt.has_value())
        return;
    const EmbossShape &es = *es_opt;

    const std::optional<TextConfiguration> &tc_opt = mv.text_configuration;
    if (!tc_opt.has_value())
        return;
    const TextConfiguration &tc = *tc_opt;

    // calculate count lines when not set
    if (count_lines == 0) {
        count_lines = get_count_lines(tc.text);
        if (count_lines == 0)
            return;
    }

    // prepare volumes to slice
    ModelVolumePtrs volumes = prepare_volumes_to_slice(mv);

    // For interactivity during drag over surface it must be from gl_volume not volume.
    Transform3d mv_trafo = gl_volume.get_volume_transformation().get_matrix();
    if (es.fix_3mf_tr.has_value())
        mv_trafo = mv_trafo * (es.fix_3mf_tr->inverse());
    text_lines.init(mv_trafo, volumes, style_manager, count_lines);
}
}

void GLGizmoEmboss::reinit_text_lines(unsigned count_lines) {    
    init_text_lines(m_text_lines, m_parent.get_selection(), m_style_manager, count_lines);
}

void GLGizmoEmboss::set_volume_by_selection()
{
    const Selection &selection = m_parent.get_selection();
    const GLVolume  *gl_volume = get_selected_gl_volume(selection);
    if (gl_volume == nullptr)
        return reset_volume();

    const ModelObjectPtrs &objects = m_parent.get_model()->objects;
    ModelVolume *volume = get_model_volume(*gl_volume, objects);
    if (volume == nullptr)
        return reset_volume();

    // is same volume as actual selected?
    if (volume->id() == m_volume_id && 
        m_volume != nullptr && 
        volume->text_configuration->style == m_volume->text_configuration->style)
        return;

    // for changed volume notification is NOT valid
    remove_notification_not_valid_font();

    // Do not use focused input value when switch volume(it must swith value)
    if (m_volume != nullptr && m_volume != volume) // when update volume it changed id BUT not pointer
        ImGuiWrapper::left_inputs();

    // Is selected volume text volume?
    const std::optional<TextConfiguration> &tc_opt = volume->text_configuration;
    if (!tc_opt.has_value())
        return reset_volume();

    // Emboss shape must be setted
    assert(volume->emboss_shape.has_value());
    if (!volume->emboss_shape.has_value())
        return;

    const TextConfiguration &tc = *tc_opt;
    const EmbossStyle    &style = tc.style;

    std::optional<wxString> installed_name = get_installed_face_name(style.prop.face_name, *m_face_names);

    wxFont wx_font;
    // load wxFont from same OS when font name is installed
    if (style.type == WxFontUtils::get_current_type() && installed_name.has_value())
        wx_font = WxFontUtils::load_wxFont(style.path);

    // Flag that is selected same font
    bool is_exact_font = true;
    // Different OS or try found on same OS
    if (!wx_font.IsOk()) {
        is_exact_font = false;
        // Try create similar wx font by FontFamily
        wx_font = WxFontUtils::create_wxFont(style);
        if (installed_name.has_value() && !installed_name->empty())
            is_exact_font = wx_font.SetFaceName(*installed_name);

        // Have to use some wxFont
        if (!wx_font.IsOk())
            wx_font = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
    }
    assert(wx_font.IsOk());

    // Load style to style manager
    const auto &styles = m_style_manager.get_styles();
    auto has_same_name = [&name = style.name](const StyleManager::Style &style_item) { return style_item.name == name; };

    StyleManager::Style style_{style};  // copy  
    style_.projection = volume->emboss_shape->projection;
    style_.angle = calc_angle(selection);
    style_.distance = calc_distance(*gl_volume, m_raycast_manager, m_parent);
        
    if (auto it = std::find_if(styles.begin(), styles.end(), has_same_name);
        it == styles.end()) {
        // style was not found
        m_style_manager.load_style(style_, wx_font);
    } else {
        // style name is in styles list
        size_t style_index = it - styles.begin();
        if (!m_style_manager.load_style(style_index)) {
            // can`t load stored style
            m_style_manager.erase(style_index);
            m_style_manager.load_style(style_, wx_font);
        } else {
            // stored style is loaded, now set modification of style
            m_style_manager.get_style() = style_;
            m_style_manager.set_wx_font(wx_font);
        }
    }
    
    if (!is_exact_font)
        create_notification_not_valid_font(tc);

    // cancel previous job
    if (m_job_cancel != nullptr) {
        m_job_cancel->store(true);
        m_job_cancel = nullptr;
    }

    m_text   = tc.text;
    m_volume = volume;
    m_volume_id = volume->id();
        
    if (tc.style.prop.per_glyph)
        reinit_text_lines();

    // Calculate current angle of up vector
    assert(m_style_manager.is_active_font());
    if (m_style_manager.is_active_font()) 
        m_style_manager.get_style().angle = calc_angle(selection);

    // calculate scale for height and depth inside of scaled object instance
    calculate_scale();    
}

void GLGizmoEmboss::reset_volume()
{
    if (m_volume == nullptr)
        return; // already reseted

    m_volume = nullptr;
    m_volume_id.id = 0;

    // No more need of current notification
    remove_notification_not_valid_font();
}

void GLGizmoEmboss::calculate_scale() {
    Transform3d to_world = m_parent.get_selection().get_first_volume()->world_matrix();
    auto to_world_linear = to_world.linear();
    auto calc = [&to_world_linear](const Vec3d &axe, std::optional<float>& scale)->bool {
        Vec3d  axe_world = to_world_linear * axe;
        double norm_sq   = axe_world.squaredNorm();
        if (is_approx(norm_sq, 1.)) {
            if (scale.has_value())
                scale.reset();
            else
                return false;
        } else {
            scale = sqrt(norm_sq);
        }
        return true;
    };

    bool exist_change = calc(Vec3d::UnitY(), m_scale_height);
    exist_change |= calc(Vec3d::UnitZ(), m_scale_depth);

    // Change of scale has to change font imgui font size
    if (exist_change)
        m_style_manager.clear_imgui_font();
}

namespace {
bool is_text_empty(std::string_view text) { return text.empty() || text.find_first_not_of(" \n\t\r") == std::string::npos; }
} // namespace

bool GLGizmoEmboss::process(bool make_snapshot)
{
    // no volume is selected -> selection from right panel
    assert(m_volume != nullptr);
    if (m_volume == nullptr) return false;

    // without text there is nothing to emboss
    if (is_text_empty(m_text)) return false;

    // exist loaded font file?
    if (!m_style_manager.is_active_font()) return false;

    const Selection& selection = m_parent.get_selection();
    DataBasePtr base = create_emboss_data_base(m_text, m_style_manager, m_text_lines, selection, m_volume->type(), m_job_cancel);
    DataUpdate  data{std::move(base), m_volume->id(), make_snapshot};

    // check valid count of text lines
    assert(data.base->text_lines.empty() || data.base->text_lines.size() == get_count_lines(m_text));

    if (!start_update_volume(std::move(data), *m_volume, selection, m_raycast_manager))
        return false;

    // notification is removed befor object is changed by job
    remove_notification_not_valid_font();
    return true;
}

void GLGizmoEmboss::close()
{
    // remove volume when text is empty
    if (m_volume != nullptr && 
        m_volume->text_configuration.has_value() &&
        is_text_empty(m_text)) {
        Plater &p = *wxGetApp().plater();
        // is the text object?
        if (m_volume->is_the_only_one_part()) {
            // delete whole object
            p.remove(m_parent.get_selection().get_object_idx());
        } else {
            // delete text volume
            p.remove_selected();
        }
    }

    // close gizmo == open it again
    auto& mng = m_parent.get_gizmos_manager();
    if (mng.get_current_type() == GLGizmosManager::Emboss)
        mng.open_gizmo(GLGizmosManager::Emboss);
}

void GLGizmoEmboss::draw_window()
{
#ifdef ALLOW_DEBUG_MODE
    if (ImGui::Button("re-process")) process();
#endif //  ALLOW_DEBUG_MODE

    // Setter of indent must be befor disable !!!
    ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, m_gui_cfg->indent);
    ScopeGuard indent_sc([](){ ImGui::PopStyleVar(/*ImGuiStyleVar_IndentSpacing*/); });

    // Disable all except selection of font, when open text from 3mf with unknown font
    m_imgui->disabled_begin(m_is_unknown_font);
    ScopeGuard unknown_font_sc([imgui = m_imgui]() { imgui->disabled_end(/*m_is_unknown_font*/); });

    draw_text_input();

    ImGui::Indent();
        // When unknown font is inside .3mf only font selection is allowed
        m_imgui->disabled_end(/*m_is_unknown_font*/);
        draw_font_list_line();
        m_imgui->disabled_begin(m_is_unknown_font);
        bool use_inch = wxGetApp().app_config->get_bool("use_inches");
        draw_height(use_inch);
        draw_depth(use_inch);
    ImGui::Unindent();

    // close advanced style property when unknown font is selected
    if (m_is_unknown_font && m_is_advanced_edit_style) 
        ImGui::SetNextTreeNodeOpen(false);

    if (ImGui::TreeNode(_u8L("Advanced").c_str())) {
        if (!m_is_advanced_edit_style) {
            m_is_advanced_edit_style = true;
            m_imgui->set_requires_extra_frame();
        } else {
            draw_advanced();
        }
        ImGui::TreePop();
    } else if (m_is_advanced_edit_style) {
        m_is_advanced_edit_style = false;
        m_imgui->set_requires_extra_frame();
    }

    ImGui::Separator();

    draw_style_list();

    // Do not select volume type, when it is text object
    if (!m_volume->is_the_only_one_part()) {
        ImGui::Separator();
        draw_model_type();
    }
       
#ifdef SHOW_WX_FONT_DESCRIPTOR
    if (is_selected_style)
        m_imgui->text_colored(ImGuiWrapper::COL_GREY_DARK, m_style_manager.get_style().path);
#endif // SHOW_WX_FONT_DESCRIPTOR

#ifdef SHOW_CONTAIN_3MF_FIX
    if (m_volume!=nullptr &&
        m_volume->text_configuration.has_value() &&
        m_volume->text_configuration->fix_3mf_tr.has_value()) {
        ImGui::SameLine();
        m_imgui->text_colored(ImGuiWrapper::COL_GREY_DARK, ".3mf");
        if (ImGui::IsItemHovered()) {
            Transform3d &fix = *m_volume->text_configuration->fix_3mf_tr;
            std::stringstream ss;
            ss << fix.matrix();            
            std::string filename = (m_volume->source.input_file.empty())? "unknown.3mf" :
                                   m_volume->source.input_file + ".3mf";
            ImGui::SetTooltip("Text configuation contain \n"
                              "Fix Transformation Matrix \n"
                              "%s\n"
                              "loaded from \"%s\" file.",
                              ss.str().c_str(), filename.c_str()
                );
        }
    }
#endif // SHOW_CONTAIN_3MF_FIX
#ifdef SHOW_ICONS_TEXTURE    
    auto &t = m_icons_texture;
    ImGui::Image((void *) t.get_id(), ImVec2(t.get_width(), t.get_height()));
#endif //SHOW_ICONS_TEXTURE
#ifdef SHOW_IMGUI_ATLAS
    const auto &atlas = m_style_manager.get_atlas();
    ImGui::Image(atlas.TexID, ImVec2(atlas.TexWidth, atlas.TexHeight));
#endif // SHOW_IMGUI_ATLAS

#ifdef ALLOW_OPEN_NEAR_VOLUME
    ImGui::SameLine();
    if (ImGui::Checkbox("##ALLOW_OPEN_NEAR_VOLUME", &m_allow_open_near_volume)) {
        if (m_allow_open_near_volume)
            m_set_window_offset = calc_fine_position(m_parent.get_selection(), get_minimal_window_size(), m_parent.get_canvas_size());
    } else if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", ((m_allow_open_near_volume) ? 
            "Fix settings position":
            "Allow floating window near text").c_str());
    }
#endif // ALLOW_FLOAT_WINDOW
 }

#include "imgui/imgui_internal.h" // scroll bar existence

void GLGizmoEmboss::draw_text_input()
{
    auto create_range_text_prep = [&mng = m_style_manager, &text = m_text, &exist_unknown = m_text_contain_unknown_glyph]() {
        auto& ff = mng.get_font_file_with_cache();
        assert(ff.has_value());
        const auto &cn = mng.get_font_prop().collection_number;
        unsigned int font_index = (cn.has_value()) ? *cn : 0;
        return create_range_text(text, *ff.font_file, font_index, &exist_unknown);
    };
    
    double scale = m_scale_height.has_value() ? *m_scale_height : 1.;
    ImFont *imgui_font = m_style_manager.get_imgui_font();
    if (imgui_font == nullptr) {
        // try create new imgui font
        double screen_scale = wxDisplay(wxGetApp().plater()).GetScaleFactor();
        double imgui_scale = scale * screen_scale;
        m_style_manager.create_imgui_font(create_range_text_prep(), imgui_scale);
        imgui_font = m_style_manager.get_imgui_font();
    }
    bool exist_font = 
        imgui_font != nullptr &&
        imgui_font->IsLoaded() &&
        imgui_font->Scale > 0.f &&
        imgui_font->ContainerAtlas != nullptr;
    // NOTE: Symbol fonts doesn't have atlas 
    // when their glyph range is out of language character range
    if (exist_font) ImGui::PushFont(imgui_font);

    // show warning about incorrectness view of font
    std::string warning_tool_tip;
    if (!exist_font) {
        warning_tool_tip = _u8L("The text cannot be written using the selected font. Please try choosing a different font.");
    } else {
        auto append_warning = [&warning_tool_tip](std::string t) {
            if (!warning_tool_tip.empty()) 
                warning_tool_tip += "\n";
            warning_tool_tip += t;
        };
        if (is_text_empty(m_text)) 
            append_warning(_u8L("Embossed text cannot contain only white spaces."));
        if (m_text_contain_unknown_glyph)
            append_warning(_u8L("Text contains character glyph (represented by '?') unknown by font."));

        const FontProp &prop = m_style_manager.get_font_prop();
        if (prop.skew.has_value())     append_warning(_u8L("Text input doesn't show font skew."));
        if (prop.boldness.has_value()) append_warning(_u8L("Text input doesn't show font boldness."));
        if (prop.line_gap.has_value()) append_warning(_u8L("Text input doesn't show gap between lines."));
        auto &ff         = m_style_manager.get_font_file_with_cache();
        float imgui_size = StyleManager::get_imgui_font_size(prop, *ff.font_file, scale);
        if (imgui_size > StyleManager::max_imgui_font_size)
            append_warning(_u8L("Too tall, diminished font height inside text input."));
        if (imgui_size < StyleManager::min_imgui_font_size)
            append_warning(_u8L("Too small, enlarged font height inside text input."));
        bool is_multiline = m_text_lines.get_lines().size() > 1;
        if (is_multiline && (prop.align.first == FontProp::HorizontalAlign::center || prop.align.first == FontProp::HorizontalAlign::right))
            append_warning(_u8L("Text doesn't show current horizontal alignment."));
    }
    
    // flag for extend font ranges if neccessary
    // ranges can't be extend during font is activ(pushed)
    std::string range_text;
    ImVec2 input_size(m_gui_cfg->text_size.x, m_gui_cfg->text_size.y);
    const ImGuiInputTextFlags flags = ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_AutoSelectAll;
    if (ImGui::InputTextMultiline("##Text", &m_text, input_size, flags)) {
        if (m_style_manager.get_font_prop().per_glyph) {
            unsigned count_lines = get_count_lines(m_text);
            if (count_lines != m_text_lines.get_lines().size()) 
                // Necesarry to initialize count by given number (differ from stored in volume at the moment)
                reinit_text_lines(count_lines);         
        }
        process();
        range_text = create_range_text_prep();
    }

    if (exist_font) ImGui::PopFont();

    // warning tooltip has to be with default font
    if (!warning_tool_tip.empty()) {
        // Multiline input has hidden window for scrolling
        const ImGuiWindow *input = ImGui::GetCurrentWindow()->DC.ChildWindows.front();
        const ImGuiStyle &style = ImGui::GetStyle();
        float scrollbar_width = (input->ScrollbarY) ? style.ScrollbarSize : 0.f;
        float scrollbar_height = (input->ScrollbarX) ? style.ScrollbarSize : 0.f;

        if (ImGui::IsItemHovered())
            m_imgui->tooltip(warning_tool_tip, m_gui_cfg->max_tooltip_width);

        ImVec2 cursor = ImGui::GetCursorPos();
        float width = ImGui::GetContentRegionAvailWidth();
        const ImVec2& padding = style.FramePadding;
        ImVec2 icon_pos(width - m_gui_cfg->icon_width - scrollbar_width + padding.x, 
                        cursor.y - 2 * m_gui_cfg->icon_width - scrollbar_height - 2*padding.y);  // ORCA fix vertical position
        
        ImGui::SetCursorPos(icon_pos);
        draw(get_icon(m_icons, IconType::exclamation, IconState::hovered));
        ImGui::SetCursorPos(cursor);
    }

    // NOTE: must be after ImGui::font_pop() 
    //          -> imgui_font has to be unused
    // IMPROVE: only extend not clear
    // Extend font ranges
    if (!range_text.empty() &&
        !ImGuiWrapper::contain_all_glyphs(imgui_font, range_text) )
        m_style_manager.clear_imgui_font();    
}

// create texture for visualization font face
void GLGizmoEmboss::init_font_name_texture() {
    Timer t("init_font_name_texture");
    // check if already exists
    GLuint &id = m_face_names->texture_id; 
    if (id != 0) return;
    // create texture for font
    GLenum target = GL_TEXTURE_2D;
    glsafe(::glGenTextures(1, &id));
    glsafe(::glBindTexture(target, id));
    glsafe(::glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
    glsafe(::glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
    const Vec2i32 &size = m_gui_cfg->face_name_size;
    GLint w = size.x(), h = m_face_names->count_cached_textures * size.y();
    std::vector<unsigned char> data(4*w * h, {0});
    const GLenum format = GL_RGBA, type = GL_UNSIGNED_BYTE;
    const GLint level = 0, internal_format = GL_RGBA, border = 0;
    glsafe(::glTexImage2D(target, level, internal_format, w, h, border, format, type, data.data()));

    // bind default texture
    GLuint no_texture_id = 0;
    glsafe(::glBindTexture(target, no_texture_id));

    // clear info about creation of texture - no one is initialized yet
    for (FaceName &face : m_face_names->faces) { 
        face.cancel = nullptr;
        face.is_created = nullptr;
    }

    // Prepare filtration cache
    //m_face_names->hide = std::vector<bool>(m_face_names->faces.size(), {false});
}

bool GLGizmoEmboss::select_facename(const wxString &facename)
{
    if (!wxFontEnumerator::IsValidFacename(facename)) return false;
    // Select font
    wxFont wx_font(wxFontInfo().FaceName(facename).Encoding(Facenames::encoding));
    if (!wx_font.IsOk()) return false;
#ifdef USE_PIXEL_SIZE_IN_WX_FONT
    // wx font could change source file by size of font
    int point_size = static_cast<int>(m_style_manager.get_font_prop().size_in_mm);
    wx_font.SetPointSize(point_size);
#endif // USE_PIXEL_SIZE_IN_WX_FONT
    if (!m_style_manager.set_wx_font(wx_font)) return false;
    process();
    return true;
}

void GLGizmoEmboss::push_button_style(bool pressed)
{
    if (m_is_dark_mode) {
        if (pressed) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(43 / 255.f, 64 / 255.f, 54 / 255.f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(43 / 255.f, 64 / 255.f, 54 / 255.f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(43 / 255.f, 64 / 255.f, 54 / 255.f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.f, 174 / 255.f, 66 / 255.f, 1.f));
        }
        else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(45.f / 255.f, 45.f / 255.f, 49.f / 255.f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(84 / 255.f, 84 / 255.f, 90 / 255.f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(84 / 255.f, 84 / 255.f, 90 / 255.f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(45.f / 255.f, 45.f / 255.f, 49.f / 255.f, 1.f));
        }
    }
    else {
        if (pressed) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(219 / 255.f, 253 / 255.f, 231 / 255.f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(219 / 255.f, 253 / 255.f, 231 / 255.f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(219 / 255.f, 253 / 255.f, 231 / 255.f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.f, 174 / 255.f, 66 / 255.f, 1.f));
        }
        else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.f, 1.f, 1.f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(238 / 255.f, 238 / 255.f, 238 / 255.f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(238 / 255.f, 238 / 255.f, 238 / 255.f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.f, 1.f, 1.f, 1.f));
        }
    
    }
}

void GLGizmoEmboss::pop_button_style()
{
    ImGui::PopStyleColor(4);
}

void GLGizmoEmboss::draw_font_list_line()
{
    ImGui::AlignTextToFramePadding();

    bool exist_stored_style   = m_style_manager.exist_stored_style();
    bool exist_change_in_font = m_style_manager.is_font_changed();
    const std::string& font_text = m_gui_cfg->translations.font;
    if (exist_change_in_font || !exist_stored_style)
        ImGuiWrapper::text_colored(ImGuiWrapper::COL_MODIFIED, font_text); // ORCA match color
    else
        ImGuiWrapper::text(font_text);

    ImGui::SameLine(m_gui_cfg->input_offset);

    draw_font_list();

    bool exist_change = false;
    if (!m_is_unknown_font) {
        ImGui::SameLine();
        if (draw_italic_button())
            exist_change = true;
        ImGui::SameLine();
        if (draw_bold_button())
            exist_change = true;
    } else {
        // when exist unknown font add confirmation button
        ImGui::SameLine();
        // Apply for actual selected font
        if (ImGui::Button(_u8L("Apply").c_str()))
            exist_change = true;
    }

    EmbossStyle &style = m_style_manager.get_style();
    if (exist_change_in_font) {
        ImGui::SameLine(ImGui::GetStyle().WindowPadding.x);
        auto r_icon = get_icon(m_icons, IconType::undo, IconState::hovered);
        if (Slic3r::GUI::button(r_icon, r_icon, r_icon)) { // ORCA draw bottom with same orange color
            const EmbossStyle *stored_style = m_style_manager.get_stored_style();

            style.path          = stored_style->path;
            style.prop.boldness = stored_style->prop.boldness;
            style.prop.skew     = stored_style->prop.skew;

            wxFont new_wx_font = WxFontUtils::load_wxFont(style.path);
            if (new_wx_font.IsOk() && m_style_manager.set_wx_font(new_wx_font))
                exist_change = true;
        } else if (ImGui::IsItemHovered())
            m_imgui->tooltip(_u8L("Revert font changes."), m_gui_cfg->max_tooltip_width);
    }

    if (exist_change) {
        m_style_manager.clear_glyphs_cache();
        if (m_style_manager.get_font_prop().per_glyph)
            reinit_text_lines(m_text_lines.get_lines().size());
        process();
    }
}

void GLGizmoEmboss::draw_font_list()
{
    ImGuiWrapper::push_combo_style(m_gui_cfg->screen_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 4.0f * m_gui_cfg->screen_scale);

    wxString tooltip_name = "";

    // Set partial
    wxString actual_face_name;
    if (m_style_manager.is_active_font()) {
        const wxFont &wx_font = m_style_manager.get_wx_font();
        if (wx_font.IsOk())
            actual_face_name = wx_font.GetFaceName();
    }
    // name of actual selected font
    const char * selected = (!actual_face_name.empty()) ?
        (const char *)actual_face_name.c_str() : " --- ";

    // Do not remove font face during enumeration
    // When deletation of font appear this variable is set
    std::optional<size_t> del_index;
    
    ImGui::SetNextItemWidth(2 * m_gui_cfg->input_width);
    std::vector<int> filtered_items_idx;
    bool             is_filtered = false;
    if (m_imgui->bbl_combo_with_filter("##Combo_Font", selected, m_face_names->faces_names,
        &filtered_items_idx, &is_filtered, m_imgui->scaled(32.f / 15.f))) {
        bool set_selection_focus = false;
        if (!m_face_names->is_init) {
            init_face_names(*m_face_names);
            set_selection_focus = true;
        }

        if (!m_face_names->has_truncated_names)
            init_truncated_names(*m_face_names, m_gui_cfg->input_width);

        if (m_face_names->texture_id == 0)
            init_font_name_texture();

        int show_items_count = is_filtered ? filtered_items_idx.size() : m_face_names->faces.size();

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0);

        for (int i = 0; i < show_items_count; i++) {
            int idx = is_filtered ? filtered_items_idx[i] : i;
            FaceName &face = m_face_names->faces[idx];
            const wxString &wx_face_name = face.wx_name;

            ImGui::PushID(idx);
            ScopeGuard sg([]() { ImGui::PopID(); });
            bool is_selected = (actual_face_name == wx_face_name);
            ImVec2 selectable_size(0, m_imgui->scaled(32.f / 15.f));
            ImGuiSelectableFlags flags = 0;
            if (ImGui::BBLSelectable(face.name_truncated.c_str(), is_selected, flags, selectable_size)) {
                if (!select_facename(wx_face_name)) {
                    del_index = idx;
                    MessageDialog(wxGetApp().plater(), GUI::format_wxstr(_L("Font \"%1%\" can't be selected."), wx_face_name));
                } else {
                    ImGui::CloseCurrentPopup();
                }
            }
            // tooltip as full name of font face
            if (ImGui::IsItemHovered())
                tooltip_name = wx_face_name;

            if (is_selected) {
                ImGui::SetItemDefaultFocus();
            }

            // on first draw set focus on selected font
            if (set_selection_focus && is_selected)
                ImGui::SetScrollHereY();
            ::draw_font_preview(face, m_text, *m_face_names, *m_gui_cfg, ImGui::IsItemVisible());
        }
        
        ImGui::PopStyleVar(3);
        ImGui::EndListBox();
        ImGui::EndPopup();
    } else if (m_face_names->is_init) {
        // Just one after close combo box
        // free texture and set id to zero
        m_face_names->is_init = false;
        // cancel all process for generation of texture
        for (FaceName &face : m_face_names->faces)
            if (face.cancel != nullptr)
                face.cancel->store(true);
        glsafe(::glDeleteTextures(1, &m_face_names->texture_id));
        m_face_names->texture_id = 0;
    }

    // delete unloadable face name when try to use
    if (del_index.has_value()) {
        auto face = m_face_names->faces.begin() + (*del_index);
        std::vector<wxString>& bad = m_face_names->bad;
        // sorted insert into bad fonts
        auto it = std::upper_bound(bad.begin(), bad.end(), face->wx_name);
        bad.insert(it, face->wx_name);
        m_face_names->faces.erase(face);
        m_face_names->faces_names.erase(m_face_names->faces_names.begin() + (*del_index));
        // update cached file
        store(*m_face_names);
    }

#ifdef ALLOW_ADD_FONT_BY_FILE
    ImGui::SameLine();
    // select font file by file browser
    if (draw_button(IconType::open_file)) {
        if (choose_true_type_file()) { 
            process();
        }
    } else if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Add file with font(.ttf, .ttc)");
#endif //  ALLOW_ADD_FONT_BY_FILE

#ifdef ALLOW_ADD_FONT_BY_OS_SELECTOR
    ImGui::SameLine();
    if (draw_button(IconType::system_selector)) {
        if (choose_font_by_wxdialog()) {
            process();
        }
    } else if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Open dialog for choose from fonts.");
#endif //  ALLOW_ADD_FONT_BY_OS_SELECTOR
    
    ImGui::PopStyleVar(2);
    ImGuiWrapper::pop_combo_style();

    if (!tooltip_name.IsEmpty())
        m_imgui->tooltip(tooltip_name, m_gui_cfg->max_tooltip_width);
}

void GLGizmoEmboss::draw_model_type()
{
    ImGui::AlignTextToFramePadding();
    bool is_last_solid_part = m_volume->is_the_only_one_part();
    std::string title = _u8L("Operation");
    if (is_last_solid_part) {
        ImVec4 color{.5f, .5f, .5f, 1.f};
        m_imgui->text_colored(color, title.c_str());
    } else {
        ImGui::Text("%s", title.c_str());
    }

    std::optional<ModelVolumeType> new_type;
    ModelVolumeType modifier = ModelVolumeType::PARAMETER_MODIFIER;
    ModelVolumeType negative = ModelVolumeType::NEGATIVE_VOLUME;
    ModelVolumeType part = ModelVolumeType::MODEL_PART;
    ModelVolumeType type = m_volume->type();

    //TRN EmbossOperation
    ImGuiWrapper::push_radio_style(m_parent.get_scale()); // ORCA
    if (ImGui::RadioButton(_u8L("Join").c_str(), type == part))
        new_type = part;
    else if (ImGui::IsItemHovered())
        m_imgui->tooltip(_u8L("Click to change text into object part."), m_gui_cfg->max_tooltip_width);
    ImGui::SameLine();

    std::string last_solid_part_hint = _u8L("You can't change a type of the last solid part of the object.");
    if (ImGui::RadioButton(_CTX_utf8(L_CONTEXT("Cut", "EmbossOperation"), "EmbossOperation").c_str(), type == negative))
        new_type = negative;
    else if (ImGui::IsItemHovered()) {
        if (is_last_solid_part)
            m_imgui->tooltip(last_solid_part_hint, m_gui_cfg->max_tooltip_width);
        else if (type != negative)
            m_imgui->tooltip(_u8L("Click to change part type into negative volume."), m_gui_cfg->max_tooltip_width);
    }

    // In simple mode are not modifiers
    if (wxGetApp().plater()->printer_technology() != ptSLA && wxGetApp().get_mode() != ConfigOptionMode::comSimple) {
        ImGui::SameLine();
        if (ImGui::RadioButton(_u8L("Modifier").c_str(), type == modifier))
            new_type = modifier;
        else if (ImGui::IsItemHovered()) {
            if (is_last_solid_part)
                m_imgui->tooltip(last_solid_part_hint, m_gui_cfg->max_tooltip_width);
            else if (type != modifier)
                m_imgui->tooltip(_u8L("Click to change part type into modifier."), m_gui_cfg->max_tooltip_width);
        }
    }
    ImGuiWrapper::pop_radio_style();

    if (m_volume != nullptr && new_type.has_value() && !is_last_solid_part) {
        GUI_App &app    = wxGetApp();
        Plater * plater = app.plater();
        Plater::TakeSnapshot snapshot(plater, _u8L("Change Text Type"), UndoRedo::SnapshotType::GizmoAction);
        m_volume->set_type(*new_type);

        bool is_volume_move_inside  = (type == part);
        bool is_volume_move_outside = (*new_type == part);
         // Update volume position when switch (from part) or (into part)
        if ((is_volume_move_inside || is_volume_move_outside))
            process();

        // inspiration in ObjectList::change_part_type()
        // how to view correct side panel with objects
        ObjectList *obj_list = app.obj_list();
        wxDataViewItemArray sel = obj_list->reorder_volumes_and_get_selection(
            obj_list->get_selected_obj_idx(),
            [volume = m_volume](const ModelVolume *vol) { return vol == volume; });
        if (!sel.IsEmpty()) obj_list->select_item(sel.front());       

        // NOTE: on linux, function reorder_volumes_and_get_selection call GLCanvas3D::reload_scene(refresh_immediately = false)
        // which discard m_volume pointer and set it to nullptr also selection is cleared so gizmo is automaticaly closed
        auto &mng = m_parent.get_gizmos_manager();
        if (mng.get_current_type() != GLGizmosManager::Emboss)
            mng.open_gizmo(GLGizmosManager::Emboss);
        // TODO: select volume back - Ask @Sasa
    }
}

void GLGizmoEmboss::draw_style_rename_popup() {
    std::string& new_name = m_style_manager.get_style().name;
    const std::string &old_name = m_style_manager.get_stored_style()->name;
    std::string text_in_popup = GUI::format(_L("Rename style (%1%) for embossing text"), old_name) + ": ";
    ImGui::Text("%s", text_in_popup.c_str());
        
    bool is_unique = (new_name == old_name) || // could be same as before rename
        m_style_manager.is_unique_style_name(new_name);

    bool allow_change = false;
    if (new_name.empty()) {
        ImGuiWrapper::text_colored(ImGuiWrapper::COL_ORANGE_DARK, _u8L("Name can't be empty."));
    }else if (!is_unique) { 
        ImGuiWrapper::text_colored(ImGuiWrapper::COL_ORANGE_DARK, _u8L("Name has to be unique."));
    } else {
        ImGui::NewLine();
        allow_change = true;
    }

    bool store = false;
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue;
    if (ImGui::InputText("##rename style", &new_name, flags) && allow_change) store = true;
    if (m_imgui->button(_L("OK"), ImVec2(0.f, 0.f), allow_change)) store = true;
    ImGui::SameLine();
    if (ImGui::Button(_u8L("Cancel").c_str())) {
        new_name = old_name;
        ImGui::CloseCurrentPopup();
    }

    if (store) {
        // rename style in all objects and volumes
        for (const ModelObject *mo :wxGetApp().plater()->model().objects) {
            for (ModelVolume *mv : mo->volumes) { 
                if (!mv->text_configuration.has_value()) continue;
                std::string& name = mv->text_configuration->style.name;
                if (name != old_name) continue;
                name = new_name;
            }
        }
        
        m_style_manager.rename(new_name);
        m_style_manager.store_styles_to_app_config();
        ImGui::CloseCurrentPopup();
    }
}

void GLGizmoEmboss::draw_style_rename_button()
{
    bool can_rename = m_style_manager.exist_stored_style();
    std::string title = _u8L("Rename style");
    const char * popup_id = title.c_str();
    if (draw_button(m_icons, IconType::rename, !can_rename)) {
        assert(m_style_manager.get_stored_style());
        ImGui::OpenPopup(popup_id);
    }
    else if (ImGui::IsItemHovered()) {
        if (can_rename) m_imgui->tooltip(_u8L("Rename current style."), m_gui_cfg->max_tooltip_width);
        else            m_imgui->tooltip(_u8L("Can't rename temporary style."), m_gui_cfg->max_tooltip_width);
    }
    if (ImGui::BeginPopupModal(popup_id, 0, ImGuiWindowFlags_AlwaysAutoResize)) {
        m_imgui->disable_background_fadeout_animation();
        draw_style_rename_popup();
        ImGui::EndPopup();
    }
}

void GLGizmoEmboss::draw_style_save_button(bool is_modified)
{
    if (draw_button(m_icons, IconType::save, !is_modified)) {
        // save styles to app config
        m_style_manager.store_styles_to_app_config();
    }else if (ImGui::IsItemHovered()) {
        std::string tooltip;
        if (!m_style_manager.exist_stored_style()) {
            tooltip = _u8L("First Add style to list.");
        } else if (is_modified) {
            tooltip = GUI::format(_L("Save %1% style"), m_style_manager.get_style().name);
        } else {
            tooltip = _u8L("No changes to save.");
        }
        m_imgui->tooltip(tooltip, m_gui_cfg->max_tooltip_width);
    }
}

void GLGizmoEmboss::draw_style_save_as_popup() {
    ImGui::Text("%s", (_u8L("New name of style") +": ").c_str());

    // use name inside of volume configuration as temporary new name
    std::string &new_name = m_volume->text_configuration->style.name;
    bool is_unique = m_style_manager.is_unique_style_name(new_name);        
    bool allow_change = false;
    if (new_name.empty()) {
        ImGuiWrapper::text_colored(ImGuiWrapper::COL_ORANGE_DARK, _u8L("Name can't be empty."));
    }else if (!is_unique) { 
        ImGuiWrapper::text_colored(ImGuiWrapper::COL_ORANGE_DARK, _u8L("Name has to be unique."));
    } else {
        ImGui::NewLine();
        allow_change = true;
    }

    bool save_style = false;
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue;
    if (ImGui::InputText("##save as style", &new_name, flags))
        save_style = true;
        
    if (m_imgui->button(_L("OK"), ImVec2(0.f, 0.f), allow_change))
        save_style = true;

    ImGui::SameLine();
    if (ImGui::Button(_u8L("Cancel").c_str())){
        // write original name to volume TextConfiguration
        new_name = m_style_manager.get_style().name;
        ImGui::CloseCurrentPopup();
    }

    if (save_style && allow_change) {
        m_style_manager.add_style(new_name);
        m_style_manager.store_styles_to_app_config();
        ImGui::CloseCurrentPopup();
    }
}

void GLGizmoEmboss::draw_style_add_button()
{
    bool only_add_style = !m_style_manager.exist_stored_style();
    bool can_add        = true;
    if (only_add_style &&
        m_volume->text_configuration->style.type != WxFontUtils::get_current_type())
        can_add = false;

    std::string title    = _u8L("Save as new style");
    const char *popup_id = title.c_str();
    // save as new style
    ImGui::SameLine();
    if (draw_button(m_icons, IconType::add, !can_add)) {
        if (!m_style_manager.exist_stored_style()) {
            m_style_manager.store_styles_to_app_config(wxGetApp().app_config);
        } else {
            ImGui::OpenPopup(popup_id);
        }
    } else if (ImGui::IsItemHovered()) {
        if (!can_add) {
            m_imgui->tooltip(_u8L("Only valid font can be added to style."), m_gui_cfg->max_tooltip_width);
        } else if (only_add_style) {
            m_imgui->tooltip(_u8L("Add style to my list."), m_gui_cfg->max_tooltip_width);
        } else {
            m_imgui->tooltip(_u8L("Save as new style."), m_gui_cfg->max_tooltip_width);
        }
    }

    if (ImGui::BeginPopupModal(popup_id, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        m_imgui->disable_background_fadeout_animation();
        draw_style_save_as_popup();
        ImGui::EndPopup();
    }
}

void GLGizmoEmboss::draw_delete_style_button() {
    bool is_stored  = m_style_manager.exist_stored_style();
    bool is_last    = m_style_manager.get_styles().size() == 1;
    bool can_delete = is_stored && !is_last;

    if (draw_button(m_icons, IconType::erase, !can_delete)) {
        std::string style_name = m_style_manager.get_style().name; // copy
        wxString dialog_title = _L("Remove style");
        size_t next_style_index = std::numeric_limits<size_t>::max();
        Plater *plater = wxGetApp().plater();
        bool exist_change = false;
        while (true) {
            // NOTE: can't use previous loaded activ index -> erase could change index
            size_t active_index = m_style_manager.get_style_index();
            next_style_index = (active_index > 0) ? active_index - 1 :
                                                   active_index + 1;
            
            if (next_style_index >= m_style_manager.get_styles().size()) {
                MessageDialog msg(plater, _L("Can't remove the last existing style."), dialog_title, wxICON_ERROR | wxOK);
                msg.ShowModal();
                break;
            }

            // IMPROVE: add function can_load?
            // clean unactivable styles
            if (!m_style_manager.load_style(next_style_index)) {
                m_style_manager.erase(next_style_index);
                exist_change = true;
                continue;
            }

            wxString message = GUI::format_wxstr(_L("Are you sure you want to permanently remove the \"%1%\" style?"), style_name);
            MessageDialog msg(plater, message, dialog_title, wxICON_WARNING | wxYES | wxNO);
            if (msg.ShowModal() == wxID_YES) {
                // delete style
                m_style_manager.erase(active_index);
                exist_change = true;
                process();
            } else {
                // load back style
                m_style_manager.load_style(active_index);
            }
            break;
        }
        if (exist_change)
            m_style_manager.store_styles_to_app_config(wxGetApp().app_config);
    }

    if (ImGui::IsItemHovered()) {
        const std::string &style_name = m_style_manager.get_style().name;
        std::string tooltip;
        if (can_delete)        tooltip = GUI::format(_L("Delete \"%1%\" style."), style_name);
        else if (is_last)      tooltip = GUI::format(_L("Can't delete \"%1%\". It is last style."), style_name);
        else/*if(!is_stored)*/ tooltip = GUI::format(_L("Can't delete temporary style \"%1%\"."), style_name);      
        m_imgui->tooltip(tooltip, m_gui_cfg->max_tooltip_width);  
    }
}

namespace {
// FIX IT: It should not change volume position before successfull change volume by process
void fix_transformation(const StyleManager::Style &from, const StyleManager::Style &to, GLCanvas3D &canvas) {
    // fix Z rotation when exists difference in styles
    const std::optional<float> &f_angle_opt = from.angle;
    const std::optional<float> &t_angle_opt = to.angle;
    if (!is_approx(f_angle_opt, t_angle_opt)) {
        // fix rotation
        float f_angle = f_angle_opt.value_or(.0f);
        float t_angle = t_angle_opt.value_or(.0f);
        do_local_z_rotate(canvas.get_selection(), t_angle - f_angle);
        std::string no_snapshot;
        canvas.do_rotate(no_snapshot);
    }

    // fix distance (Z move) when exists difference in styles
    const std::optional<float> &f_move_opt = from.distance;
    const std::optional<float> &t_move_opt = to.distance;
    if (!is_approx(f_move_opt, t_move_opt)) {
        float f_move = f_move_opt.value_or(.0f);
        float t_move = t_move_opt.value_or(.0f);
        do_local_z_move(canvas.get_selection(), t_move - f_move);
        std::string no_snapshot;
        canvas.do_move(no_snapshot);
    }
}
} // namesapce

void GLGizmoEmboss::draw_style_list() {
    if (!m_style_manager.is_active_font()) return;

    const StyleManager::Style *stored_style = nullptr;
    bool is_stored = m_style_manager.exist_stored_style();
    if (is_stored)
        stored_style = m_style_manager.get_stored_style();
    const StyleManager::Style &current_style = m_style_manager.get_style();
    bool is_changed = (stored_style)? !(*stored_style == current_style) : true;    
    bool is_modified = is_stored && is_changed;

    const float &max_style_name_width = m_gui_cfg->max_style_name_width;
    std::string &trunc_name = m_style_manager.get_truncated_name();
    if (trunc_name.empty()) {
        // generate trunc name
        std::string current_name = current_style.name;
        ImGuiWrapper::escape_double_hash(current_name);
        trunc_name = ImGuiWrapper::trunc(current_name, max_style_name_width);
    }

    std::string title = _u8L("Style");
    if (m_style_manager.exist_stored_style())
        ImGui::Text("%s", title.c_str());
    else
        ImGui::TextColored(ImGuiWrapper::COL_ORCA, "%s", title.c_str());
        
    ImGui::SetNextItemWidth(m_gui_cfg->input_width);
    auto add_text_modify = [&is_modified](const std::string& name) {
        if (!is_modified) return name;
        return name + Preset::suffix_modified();
    };
    std::optional<size_t> selected_style_index;
    std::string tooltip = "";
    ImGuiWrapper::push_combo_style(m_parent.get_scale());
    if (ImGui::BBLBeginCombo("##style_selector", add_text_modify(trunc_name).c_str())) {
        m_style_manager.init_style_images(m_gui_cfg->max_style_image_size, m_text);
        m_style_manager.init_trunc_names(max_style_name_width);
        std::optional<std::pair<size_t,size_t>> swap_indexes;
        const StyleManager::Styles &styles = m_style_manager.get_styles();
        for (const StyleManager::Style &style : styles) {
            size_t index = &style - &styles.front();
            const std::string &actual_style_name = style.name;
            ImGui::PushID(actual_style_name.c_str());
            bool is_selected = (index == m_style_manager.get_style_index());

            float select_height = static_cast<float>(m_gui_cfg->max_style_image_size.y());
            ImVec2 select_size(0.f, select_height); // 0,0 --> calculate in draw
            const std::optional<StyleManager::StyleImage> &img = style.image;            
            // allow click delete button
            ImGuiSelectableFlags_ flags = ImGuiSelectableFlags_AllowItemOverlap; 
            if (ImGui::BBLSelectable(style.truncated_name.c_str(), is_selected, flags, select_size)) {
                selected_style_index = index;
            } else if (ImGui::IsItemHovered())
                tooltip = actual_style_name;

            // reorder items
            if (ImGui::IsItemActive() && !ImGui::IsItemHovered()) {
                if (ImGui::GetMouseDragDelta(0).y < 0.f) {
                    if (index > 0) 
                        swap_indexes = {index, index - 1};
                } else if ((index + 1) < styles.size())
                    swap_indexes = {index, index + 1};
                if (swap_indexes.has_value()) 
                    ImGui::ResetMouseDragDelta();
            }

            // draw style name
            if (img.has_value()) {
                ImGui::SameLine(max_style_name_width);
                ImVec4 tint_color = ImGui::GetStyleColorVec4(ImGuiCol_Text);
                ImGui::Image(img->texture_id, img->tex_size, img->uv0, img->uv1, tint_color);
            }

            ImGui::PopID();
        }
        if (swap_indexes.has_value()) 
            m_style_manager.swap(swap_indexes->first,
                                swap_indexes->second);
        ImGui::EndCombo();
    } else {
        // do not keep in memory style images when no combo box open
        m_style_manager.free_style_images();
        if (ImGui::IsItemHovered()) {            
            std::string style_name = add_text_modify(current_style.name);
            tooltip = is_modified?
                GUI::format(_L("Modified style \"%1%\""), current_style.name):
                GUI::format(_L("Current style is \"%1%\""), current_style.name);
        }
    }
    ImGuiWrapper::pop_combo_style();
    if (!tooltip.empty())
        m_imgui->tooltip(tooltip, m_gui_cfg->max_tooltip_width);
        
    // Check whether user wants lose actual style modification
    if (selected_style_index.has_value() && is_modified) { 
        const std::string & style_name = m_style_manager.get_styles()[*selected_style_index].name;        
        wxString message = GUI::format_wxstr(_L("Changing style to \"%1%\" will discard current style modification.\n\nWould you like to continue anyway?"), style_name);
        MessageDialog not_loaded_style_message(nullptr, message, _L("Warning"), wxICON_WARNING | wxYES | wxNO);
        if (not_loaded_style_message.ShowModal() != wxID_YES) 
            selected_style_index.reset();
    }

    // selected style from combo box
    if (selected_style_index.has_value()) {
        const StyleManager::Style &style = m_style_manager.get_styles()[*selected_style_index];
        // create copy to be able do fix transformation only when successfully load style
        StyleManager::Style cur_s = current_style;  // copy
        StyleManager::Style new_s = style;    // copy
        if (m_style_manager.load_style(*selected_style_index)) {
            ::fix_transformation(cur_s, new_s, m_parent);
            process();
        } else {
            wxString title   = _L("Not valid style.");
            wxString message = GUI::format_wxstr(_L("Style \"%1%\" can't be used and will be removed from a list."), style.name);
            MessageDialog not_loaded_style_message(nullptr, message, title, wxOK);
            not_loaded_style_message.ShowModal();
            m_style_manager.erase(*selected_style_index);
        }
    }

    ImGui::SameLine();
    draw_style_rename_button();
        
    ImGui::SameLine();
    draw_style_save_button(is_modified);

    ImGui::SameLine();
    draw_style_add_button();

    // delete button
    ImGui::SameLine();
    draw_delete_style_button();
}

bool GLGizmoEmboss::draw_italic_button()
{
    const wxFont &wx_font = m_style_manager.get_wx_font(); 
    const auto& ff = m_style_manager.get_font_file_with_cache();
    if (!wx_font.IsOk() || !ff.has_value()) { 
        draw(get_icon(m_icons, IconType::italic, IconState::disabled));
        return false;
    }

    std::optional<float> &skew = m_style_manager.get_font_prop().skew;
    bool is_font_italic = skew.has_value() || WxFontUtils::is_italic(wx_font);
    if (is_font_italic) {
        // unset italic
        if (clickable(get_icon(m_icons, IconType::italic, IconState::hovered),
                      get_icon(m_icons, IconType::unitalic, IconState::hovered))) {
            skew.reset();
            if (wx_font.GetStyle() != wxFontStyle::wxFONTSTYLE_NORMAL) {
                wxFont new_wx_font = wx_font; // copy
                new_wx_font.SetStyle(wxFontStyle::wxFONTSTYLE_NORMAL);
                if(!m_style_manager.set_wx_font(new_wx_font))
                    return false;
            }
            return true;
        }
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_u8L("Unset italic"), m_gui_cfg->max_tooltip_width);
    } else {
        // set italic
        if (draw_button(m_icons, IconType::italic)) {
            wxFont new_wx_font = wx_font; // copy
            auto new_ff = WxFontUtils::set_italic(new_wx_font, *ff.font_file);
            if (new_ff != nullptr) {
                if(!m_style_manager.set_wx_font(new_wx_font, std::move(new_ff)))
                    return false;
            } else {
                // italic font doesn't exist 
                // add skew when wxFont can't set it
                skew = 0.2f;
            }            
            return true;
        }
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_u8L("Set italic"), m_gui_cfg->max_tooltip_width);
    }
    return false;
}

bool GLGizmoEmboss::draw_bold_button() {
    const wxFont &wx_font = m_style_manager.get_wx_font();
    const auto& ff = m_style_manager.get_font_file_with_cache();
    if (!wx_font.IsOk() || !ff.has_value()) {
        draw(get_icon(m_icons, IconType::bold, IconState::disabled));
        return false;
    }
    
    std::optional<float> &boldness = m_style_manager.get_font_prop().boldness;
    bool is_font_bold = boldness.has_value() || WxFontUtils::is_bold(wx_font);
    if (is_font_bold) {
        // unset bold
        if (clickable(get_icon(m_icons, IconType::bold, IconState::hovered),
                      get_icon(m_icons, IconType::unbold, IconState::hovered))) {
            boldness.reset();
            if (wx_font.GetWeight() != wxFontWeight::wxFONTWEIGHT_NORMAL) {
                wxFont new_wx_font = wx_font; // copy
                new_wx_font.SetWeight(wxFontWeight::wxFONTWEIGHT_NORMAL);
                if(!m_style_manager.set_wx_font(new_wx_font))
                    return false;
            }
            return true;
        }
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_u8L("Unset bold"), m_gui_cfg->max_tooltip_width);
    } else {
        // set bold
        if (draw_button(m_icons, IconType::bold)) {
            wxFont new_wx_font = wx_font; // copy
            auto new_ff = WxFontUtils::set_bold(new_wx_font, *ff.font_file);
            if (new_ff != nullptr) {
                if(!m_style_manager.set_wx_font(new_wx_font, std::move(new_ff)))
                    return false;
            } else {
                // bold font can't be loaded
                // set up boldness
                boldness = 20.f;
                //font_file->cache.empty();
            }
            return true;
        }
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_u8L("Set bold"), m_gui_cfg->max_tooltip_width);
    }
    return false;
}

template<typename T> bool exist_change(const T &value, const T *default_value){
    if (default_value == nullptr) return false;
    return (value != *default_value);
}

template<> bool exist_change(const std::optional<float> &value, const std::optional<float> *default_value){
    if (default_value == nullptr) return false;
    return !is_approx(value, *default_value);
}

template<> bool exist_change(const float &value, const float *default_value){
    if (default_value == nullptr) return false;
    return !is_approx(value, *default_value);
}

template<typename T, typename Draw>
bool GLGizmoEmboss::revertible(const std::string &name,
                               T                 &value,
                               const T           *default_value,
                               const std::string &undo_tooltip,
                               float              undo_offset,
                               Draw               draw) const
{
    ImGui::AlignTextToFramePadding();
    bool changed = exist_change(value, default_value);
    if (changed || default_value == nullptr)
        ImGuiWrapper::text_colored(ImGuiWrapper::COL_MODIFIED, name); // ORCA Match color
    else
        ImGuiWrapper::text(name);

    // render revert changes button
    if (changed) {
        ImGuiWindow *window = ImGui::GetCurrentWindow();
        float prev_x = window->DC.CursorPosPrevLine.x;
        ImGui::SameLine(undo_offset); // change cursor postion
        auto r_icon = get_icon(m_icons, IconType::undo, IconState::hovered);
        if (Slic3r::GUI::button(r_icon, r_icon, r_icon)) { // ORCA draw bottom with same orange color
            value = *default_value;

            // !! Fix to detect change of value after revert of float-slider
            m_imgui->get_last_slider_status().deactivated_after_edit = true;

            return true;
        } else if (ImGui::IsItemHovered())
            m_imgui->tooltip(undo_tooltip, m_gui_cfg->max_tooltip_width);
        window->DC.CursorPosPrevLine.x = prev_x; // set back previous position
    }
    return draw();
}
// May be move to ImGuiWrapper
template<typename T> bool imgui_input(const char *label, T *v, T step, T step_fast, const char *format, ImGuiInputTextFlags flags);
template<> bool imgui_input(const char *label, float *v, float step, float step_fast, const char *format, ImGuiInputTextFlags flags)
{ return ImGui::InputFloat(label, v, step, step_fast, format, flags); }
template<> bool imgui_input(const char *label, double *v, double step, double step_fast, const char *format, ImGuiInputTextFlags flags)
{ return ImGui::InputDouble(label, v, step, step_fast, format, flags); }

template<typename T> 
bool GLGizmoEmboss::rev_input(const std::string &name, T &value, const T *default_value,
    const std::string &undo_tooltip, T step, T step_fast, const char *format, ImGuiInputTextFlags flags) const
{
    // draw offseted input
    auto draw_offseted_input = [&offset = m_gui_cfg->input_offset, &width = m_gui_cfg->input_width,
                                &name, &value, &step, &step_fast, format, flags](){
        ImGui::SameLine(offset);
        ImGui::SetNextItemWidth(width);
        return imgui_input(("##" + name).c_str(),
            &value, step, step_fast, format, flags);
    };
    float undo_offset = ImGui::GetStyle().WindowPadding.x;
    return revertible(name, value, default_value, undo_tooltip, undo_offset, draw_offseted_input);
}

template<typename T>
bool GLGizmoEmboss::rev_input_mm(const std::string          &name,
                                 T                          &value,
                                 const T                    *default_value_ptr,
                                 const std::string          &undo_tooltip,
                                 T                           step,
                                 T                           step_fast,
                                 const char                 *format,
                                 bool                        use_inch,
                                 const std::optional<float> &scale) const
{
    // _variable which temporary keep value
    T value_ = value;
    T default_value_;
    if (use_inch) {
        // calc value in inch
        value_ *= GizmoObjectManipulation::mm_to_in;
        if (default_value_ptr) {
            default_value_    = GizmoObjectManipulation::mm_to_in * (*default_value_ptr);
            default_value_ptr = &default_value_;
        }
    }
    if (scale.has_value())        
        value_ *= *scale;
    bool use_correction = use_inch || scale.has_value();
    if (rev_input(name, use_correction ? value_ : value, default_value_ptr, undo_tooltip, step, step_fast, format)) {
        if (use_correction) {
            value = value_;
            if (use_inch)
                value *= GizmoObjectManipulation::in_to_mm;
            if (scale.has_value())
                value /= *scale;
        }
        return true;
    }
    return false;
}

bool GLGizmoEmboss::rev_checkbox(const std::string &name,
                                 bool              &value,
                                 const bool        *default_value,
                                 const std::string &undo_tooltip) const
{
    // draw offseted input
    auto draw_offseted_input = [this, &offset = m_gui_cfg->advanced_input_offset, &name, &value](){
        ImGui::SameLine(offset);
        return m_imgui->bbl_checkbox(wxString::FromUTF8("##" + name), value);
    };
    float undo_offset  = ImGui::GetStyle().WindowPadding.x;
    return revertible(name, value, default_value, undo_tooltip,
                      undo_offset, draw_offseted_input);
}

bool GLGizmoEmboss::set_height() {
    float &value = m_style_manager.get_font_prop().size_in_mm;

    // size can't be zero or negative
    apply(value, limits.size_in_mm);

    if (m_volume == nullptr || !m_volume->text_configuration.has_value()) {
        assert(false);
        return false;
    }
    
    // only different value need process
    if (is_approx(value, m_volume->text_configuration->style.prop.size_in_mm))
        return false;
    
    if (m_style_manager.get_font_prop().per_glyph)
        reinit_text_lines(m_text_lines.get_lines().size());

#ifdef USE_PIXEL_SIZE_IN_WX_FONT
    // store font size into path serialization
    const wxFont &wx_font = m_style_manager.get_wx_font();
    if (wx_font.IsOk()) {
        wxFont wx_font_new = wx_font; // copy
        wx_font_new.SetPointSize(static_cast<int>(value));
        m_style_manager.set_wx_font(wx_font_new);
    }
#endif
    return true;
}

void GLGizmoEmboss::draw_height(bool use_inch)
{
    float &value = m_style_manager.get_font_prop().size_in_mm;
    const EmbossStyle* stored_style = m_style_manager.get_stored_style();
    const float *stored = (stored_style != nullptr)? &stored_style->prop.size_in_mm : nullptr;
    const char *size_format = use_inch ? "%.2f in" : "%.1f mm";
    const std::string revert_text_size = _u8L("Revert text size.");
    const std::string& name = m_gui_cfg->translations.height;
    if (rev_input_mm(name, value, stored, revert_text_size, 0.1f, 1.f, size_format, use_inch, m_scale_height))
        if (set_height())
            process();
}

void GLGizmoEmboss::draw_depth(bool use_inch)
{
    double &value = m_style_manager.get_style().projection.depth;
    const StyleManager::Style * stored_style = m_style_manager.get_stored_style();
    const double *stored = ((stored_style!=nullptr)? &stored_style->projection.depth : nullptr);
    const std::string  revert_emboss_depth = _u8L("Revert embossed depth.");
    const char *size_format = ((use_inch) ? "%.3f in" : "%.2f mm");
    const std::string  name = m_gui_cfg->translations.depth;
    if (rev_input_mm(name, value, stored, revert_emboss_depth, 0.1, 1., size_format, use_inch, m_scale_depth)){
        // size can't be zero or negative
        apply(value, limits.emboss);

        // only different value need process
        if(!is_approx(value, m_volume->emboss_shape->projection.depth))
            process();
    }
}

bool GLGizmoEmboss::rev_slider(const std::string &name,
                               std::optional<int>& value,
                               const std::optional<int> *default_value,
                               const std::string &undo_tooltip,
                               int                v_min,
                               int                v_max,
                               const std::string& format,
                               const wxString    &tooltip) const
{    
    auto draw_slider_optional_int = [&]() -> bool {
        float slider_offset = m_gui_cfg->advanced_input_offset;
        float slider_width  = m_gui_cfg->input_width;
        ImGui::SameLine(slider_offset);
        ImGui::SetNextItemWidth(slider_width);
        return m_imgui->slider_optional_int( ("##" + name).c_str(), value, 
            v_min, v_max, format.c_str(), 1.f, false, tooltip);
    };
    float undo_offset = ImGui::GetStyle().WindowPadding.x;
    return revertible(name, value, default_value,
        undo_tooltip, undo_offset, draw_slider_optional_int);
}

bool GLGizmoEmboss::rev_slider(const std::string &name,
                               std::optional<float>& value,
                               const std::optional<float> *default_value,
                               const std::string &undo_tooltip,
                               float                v_min,
                               float                v_max,
                               const std::string& format,
                               const wxString    &tooltip) const
{    
    auto draw_slider_optional_float = [&]() -> bool {
        float slider_offset = m_gui_cfg->advanced_input_offset;
        float slider_width  = m_gui_cfg->input_width;
        ImGui::SameLine(slider_offset);
        ImGui::SetNextItemWidth(slider_width);
        return m_imgui->slider_optional_float(("##" + name).c_str(), value,
            v_min, v_max, format.c_str(), 1.f, false, tooltip);
    };
    float undo_offset = ImGui::GetStyle().WindowPadding.x;
    return revertible(name, value, default_value,
        undo_tooltip, undo_offset, draw_slider_optional_float);
}

bool GLGizmoEmboss::rev_slider(const std::string &name,
                               float             &value,
                               const float       *default_value,
                               const std::string &undo_tooltip,
                               float              v_min,
                               float              v_max,
                               const std::string &format,
                               const wxString    &tooltip) const
{    
    auto draw_slider_float = [&]() -> bool {
        float slider_offset = m_gui_cfg->advanced_input_offset;
        float slider_width  = m_gui_cfg->input_width;
        ImGui::SameLine(slider_offset);
        ImGui::SetNextItemWidth(slider_width);
        return m_imgui->slider_float("##" + name, &value, v_min, v_max,
            format.c_str(), 1.f, false, tooltip);
    };
    float undo_offset = ImGui::GetStyle().WindowPadding.x;
    return revertible(name, value, default_value,
        undo_tooltip, undo_offset, draw_slider_float);
}

void GLGizmoEmboss::draw_advanced()
{
    const auto &ff = m_style_manager.get_font_file_with_cache();
    if (!ff.has_value()) { 
        ImGui::Text("%s", _u8L("Advanced options cannot be changed for the selected font.\n"
                                   "Select another font.").c_str());
        return;
    }

    FontProp &font_prop = m_style_manager.get_font_prop();
    const FontFile::Info &font_info = get_font_info(*ff.font_file, font_prop);
#ifdef SHOW_FONT_FILE_PROPERTY
    ImGui::SameLine();
    int cache_size = ff.has_value()? (int)ff.cache->size() : 0;
    std::string ff_property = 
        "ascent=" + std::to_string(font_info.ascent) +
        ", descent=" + std::to_string(font_info.descent) +
        ", lineGap=" + std::to_string(font_info.linegap) +
        ", unitPerEm=" + std::to_string(font_info.unit_per_em) + 
        ", cache(" + std::to_string(cache_size) + " glyphs)";
    if (font_file->infos.size() > 1) { 
        unsigned int collection = current_prop.collection_number.value_or(0);
        ff_property += ", collect=" + std::to_string(collection+1) + "/" + std::to_string(font_file->infos.size());
    }
    m_imgui->text_colored(ImGuiWrapper::COL_GREY_DARK, ff_property);
#endif // SHOW_FONT_FILE_PROPERTY

    auto &tr = m_gui_cfg->translations;

    const StyleManager::Style *stored_style = nullptr;
    if (m_style_manager.exist_stored_style())
        stored_style = m_style_manager.get_stored_style();
    
    bool is_the_only_one_part = m_volume->is_the_only_one_part();
    bool can_use_surface = (m_volume->emboss_shape->projection.use_surface)? true : // already used surface must have option to uncheck
                            !is_the_only_one_part;
    m_imgui->disabled_begin(!can_use_surface);
    const bool *def_use_surface = stored_style ?
        &stored_style->projection.use_surface : nullptr;
    StyleManager::Style &current_style = m_style_manager.get_style();
    bool &use_surface = current_style.projection.use_surface;
    if (rev_checkbox(tr.use_surface, use_surface, def_use_surface,
                     _u8L("Revert using of model surface."))) {
        if (use_surface)
            // when using surface distance is not used
            current_style.distance.reset();        
        process();
    }
    m_imgui->disabled_end(); // !can_use_surface

    bool &per_glyph = font_prop.per_glyph;
    bool can_use_per_glyph = (per_glyph) ? true : // already used surface must have option to uncheck
                            !is_the_only_one_part;
    m_imgui->disabled_begin(!can_use_per_glyph);
    const bool *def_per_glyph = stored_style ? &stored_style->prop.per_glyph : nullptr;
    if (rev_checkbox(tr.per_glyph, per_glyph, def_per_glyph,
        _u8L("Revert Transformation per glyph."))) {
        if (per_glyph && !m_text_lines.is_init())
            reinit_text_lines();
        process();
    } else if (ImGui::IsItemHovered()) {
        if (per_glyph) {
            m_imgui->tooltip(_u8L("Set global orientation for whole text."), m_gui_cfg->max_tooltip_width);
        } else {
            m_imgui->tooltip(_u8L("Set position and orientation per glyph."), m_gui_cfg->max_tooltip_width);
            if (!m_text_lines.is_init())
                reinit_text_lines();
        }
    } else if (!per_glyph && m_text_lines.is_init())
        m_text_lines.reset();
    m_imgui->disabled_end(); // !can_use_per_glyph
        
    auto draw_align = [&align = font_prop.align, input_offset = m_gui_cfg->advanced_input_offset, &icons = m_icons, &m_imgui = m_imgui, &m_gui_cfg = m_gui_cfg]() {
        bool is_change = false;
        ImGui::SameLine(input_offset);
        if (align.first==FontProp::HorizontalAlign::left) draw(get_icon(icons, IconType::align_horizontal_left, IconState::hovered));
        else if (draw_button(icons, IconType::align_horizontal_left)) { align.first=FontProp::HorizontalAlign::left; is_change = true; }
        else if (ImGui::IsItemHovered()) m_imgui->tooltip(_CTX_utf8(L_CONTEXT("Left", "Alignment"), "Alignment"), m_gui_cfg->max_tooltip_width);
        ImGui::SameLine();
        if (align.first==FontProp::HorizontalAlign::center) draw(get_icon(icons, IconType::align_horizontal_center, IconState::hovered));
        else if (draw_button(icons, IconType::align_horizontal_center)) { align.first=FontProp::HorizontalAlign::center; is_change = true; }
        else if (ImGui::IsItemHovered()) m_imgui->tooltip(_CTX_utf8(L_CONTEXT("Center", "Alignment"), "Alignment"), m_gui_cfg->max_tooltip_width);
        ImGui::SameLine();
        if (align.first==FontProp::HorizontalAlign::right) draw(get_icon(icons, IconType::align_horizontal_right, IconState::hovered));
        else if (draw_button(icons, IconType::align_horizontal_right)) { align.first=FontProp::HorizontalAlign::right; is_change = true; }
        else if (ImGui::IsItemHovered()) m_imgui->tooltip(_CTX_utf8(L_CONTEXT("Right", "Alignment"), "Alignment"), m_gui_cfg->max_tooltip_width);

        ImGui::SameLine(0, ImGui::GetStyle().ItemSpacing.x * 2.f); // ORCA use wider spacing for separation between horizontal / vertical alignment

        if (align.second==FontProp::VerticalAlign::top) draw(get_icon(icons, IconType::align_vertical_top, IconState::hovered));
        else if (draw_button(icons, IconType::align_vertical_top)) { align.second=FontProp::VerticalAlign::top; is_change = true; }
        else if (ImGui::IsItemHovered()) m_imgui->tooltip(_CTX_utf8(L_CONTEXT("Top", "Alignment"), "Alignment"), m_gui_cfg->max_tooltip_width);
        ImGui::SameLine();
        if (align.second==FontProp::VerticalAlign::center) draw(get_icon(icons, IconType::align_vertical_center, IconState::hovered));
        else if (draw_button(icons, IconType::align_vertical_center)) { align.second=FontProp::VerticalAlign::center; is_change = true; }
        else if (ImGui::IsItemHovered()) m_imgui->tooltip(_CTX_utf8(L_CONTEXT("Middle", "Alignment"), "Alignment"), m_gui_cfg->max_tooltip_width);
        ImGui::SameLine();
        if (align.second==FontProp::VerticalAlign::bottom) draw(get_icon(icons, IconType::align_vertical_bottom, IconState::hovered));
        else if (draw_button(icons, IconType::align_vertical_bottom)) { align.second=FontProp::VerticalAlign::bottom; is_change = true; }
        else if (ImGui::IsItemHovered()) m_imgui->tooltip(_CTX_utf8(L_CONTEXT("Bottom", "Alignment"), "Alignment"), m_gui_cfg->max_tooltip_width);
        return is_change;
    };
    const FontProp::Align * def_align = stored_style ? &stored_style->prop.align : nullptr;
    float undo_offset = ImGui::GetStyle().WindowPadding.x;
    if (revertible(tr.alignment, font_prop.align, def_align, _u8L("Revert alignment."), undo_offset, draw_align)) {
        if (font_prop.per_glyph)
            reinit_text_lines(m_text_lines.get_lines().size());
        // TODO: move with text in finalize to not change position
        process();
    }
    
    // TRN EmbossGizmo: font units
    std::string units = _u8L("points");
    std::string units_fmt = "%.0f " + units;
    
    // input gap between characters
    auto def_char_gap = stored_style ?
        &stored_style->prop.char_gap : nullptr;

    bool exist_change = false;
    int half_ascent = font_info.ascent / 2;
    int min_char_gap = -half_ascent;
    int max_char_gap = half_ascent;
    FontProp &current_prop = current_style.prop;
    if (rev_slider(tr.char_gap, current_prop.char_gap, def_char_gap, _u8L("Revert gap between characters"), 
        min_char_gap, max_char_gap, units_fmt, _L("Distance between characters"))){
        // Condition prevent recalculation when insertint out of limits value by imgui input
        const std::optional<int> &volume_char_gap = m_volume->text_configuration->style.prop.char_gap;
        if (!apply(current_prop.char_gap, limits.char_gap) ||
            !volume_char_gap.has_value() || volume_char_gap != current_prop.char_gap) {
            // char gap is stored inside of imgui font atlas
            m_style_manager.clear_imgui_font();
            exist_change = true;
        }
    }
    bool last_change = false;
    if (m_imgui->get_last_slider_status().deactivated_after_edit)
        last_change = true;

    // input gap between lines
    bool is_multiline = get_count_lines(m_volume->text_configuration->text) > 1; // TODO: cache count lines
    m_imgui->disabled_begin(!is_multiline);
    auto def_line_gap = stored_style ?
        &stored_style->prop.line_gap : nullptr;
    int min_line_gap = -half_ascent;
    int max_line_gap = half_ascent;
    if (rev_slider(tr.line_gap, current_prop.line_gap, def_line_gap, _u8L("Revert gap between lines"), 
        min_line_gap, max_line_gap, units_fmt, _L("Distance between lines"))){
        // Condition prevent recalculation when insertint out of limits value by imgui input
        const std::optional<int> &volume_line_gap = m_volume->text_configuration->style.prop.line_gap;
        if (!apply(current_prop.line_gap, limits.line_gap) ||
            !volume_line_gap.has_value() || volume_line_gap != current_prop.line_gap) {        
            // line gap is planed to be stored inside of imgui font atlas
            m_style_manager.clear_imgui_font();
            if (font_prop.per_glyph)
                reinit_text_lines(m_text_lines.get_lines().size());
            exist_change = true;
        }
    }
    if (m_imgui->get_last_slider_status().deactivated_after_edit)
        last_change = true;
    m_imgui->disabled_end(); // !is_multiline

    // input boldness
    auto def_boldness = stored_style ?
        &stored_style->prop.boldness : nullptr;
    int min_boldness = static_cast<int>(font_info.ascent * limits.boldness.gui.min);
    int max_boldness = static_cast<int>(font_info.ascent * limits.boldness.gui.max);
    if (rev_slider(tr.boldness, current_prop.boldness, def_boldness, _u8L("Undo boldness"), 
        min_boldness, max_boldness, units_fmt, _L("Tiny / Wide glyphs"))){
        const std::optional<float> &volume_boldness = m_volume->text_configuration->style.prop.boldness;
        if (!apply(current_prop.boldness, limits.boldness.values) ||
            !volume_boldness.has_value() || volume_boldness != current_prop.boldness)
            exist_change = true;
    }
    if (m_imgui->get_last_slider_status().deactivated_after_edit)
        last_change = true;

    // input italic
    auto def_skew = stored_style ?
        &stored_style->prop.skew : nullptr;
    if (rev_slider(tr.skew_ration, current_prop.skew, def_skew, _u8L("Undo letter's skew"),
        limits.skew.gui.min, limits.skew.gui.max, "%.2f", _L("Italic strength ratio"))){
        const std::optional<float> &volume_skew = m_volume->text_configuration->style.prop.skew;
        if (!apply(current_prop.skew, limits.skew.values) ||
            !volume_skew.has_value() ||volume_skew != current_prop.skew)
            exist_change = true;
    }
    if (m_imgui->get_last_slider_status().deactivated_after_edit)
        last_change = true;
    
    // input surface distance
    bool allowe_surface_distance = !use_surface && !m_volume->is_the_only_one_part();
    std::optional<float> &distance = current_style.distance;
    float prev_distance = distance.value_or(.0f);
    float min_distance = static_cast<float>(-2 * current_style.projection.depth);
    float max_distance = static_cast<float>(2 * current_style.projection.depth);
    auto def_distance = stored_style ?
        &stored_style->distance : nullptr;    
    m_imgui->disabled_begin(!allowe_surface_distance);    
    bool use_inch = wxGetApp().app_config->get_bool("use_inches");
    const std::string undo_move_tooltip = _u8L("Undo translation");
    const wxString move_tooltip = _L("Distance of the center of the text to the model surface.");
    bool is_moved = false;
    if (use_inch) {
        std::optional<float> distance_inch;
        if (distance.has_value()) distance_inch = (*distance * GizmoObjectManipulation::mm_to_in);
        std::optional<float> def_distance_inch;
        if (def_distance != nullptr) {
            if (def_distance->has_value()) def_distance_inch = GizmoObjectManipulation::mm_to_in * (*(*def_distance));
            def_distance = &def_distance_inch;
        }
        min_distance *= GizmoObjectManipulation::mm_to_in;
        max_distance *= GizmoObjectManipulation::mm_to_in;
        if (rev_slider(tr.from_surface, distance_inch, def_distance, undo_move_tooltip, min_distance, max_distance, "%.3f in", move_tooltip)) {
            if (distance_inch.has_value()) {
                distance = *distance_inch * GizmoObjectManipulation::in_to_mm;
            } else {
                distance.reset();
            }
            is_moved = true;
        }
    } else {
        if (rev_slider(tr.from_surface, distance, def_distance, undo_move_tooltip, 
        min_distance, max_distance, "%.2f mm", move_tooltip)) is_moved = true;
    }

    if (is_moved){
        if (font_prop.per_glyph){
            process(false);
        } else {
            do_local_z_move(m_parent.get_selection(), distance.value_or(.0f) - prev_distance);
        }
    }

    // Apply move to model(backend)
    if (m_imgui->get_last_slider_status().deactivated_after_edit) {
        m_parent.do_move(move_snapshot_name);
        if (font_prop.per_glyph)
            process();
    }

    m_imgui->disabled_end();  // allowe_surface_distance

    // slider for Clockwise angle in degress
    // stored angle is optional CCW and in radians
    // Convert stored value to degress
    // minus create clockwise roation from CCW
    float angle = current_style.angle.value_or(0.f);
    float angle_deg = static_cast<float>(-angle * 180 / M_PI);
    float def_angle_deg_val = 
        (!stored_style || !stored_style->angle.has_value()) ?
        0.f : (*stored_style->angle * -180 / M_PI);
    float* def_angle_deg = stored_style ?
        &def_angle_deg_val : nullptr;
    if (rev_slider(tr.rotation, angle_deg, def_angle_deg, _u8L("Undo rotation"), 
        limits.angle.min, limits.angle.max, u8"%.2f °",
                   _L("Rotate text Clockwise."))) {
        // convert back to radians and CCW
        double angle_rad = -angle_deg * M_PI / 180.0;
        Geometry::to_range_pi_pi(angle_rad);                

        double diff_angle = angle_rad - angle;
        do_local_z_rotate(m_parent.get_selection(), diff_angle);
        
        // calc angle after rotation
        const Selection &selection = m_parent.get_selection();
        const GLVolume *gl_volume = get_selected_gl_volume(selection);
        assert(gl_volume != nullptr);
        assert(m_style_manager.is_active_font());
        if (m_style_manager.is_active_font() && gl_volume != nullptr) 
            m_style_manager.get_style().angle = calc_angle(selection);
        
        if (font_prop.per_glyph)
            reinit_text_lines(m_text_lines.get_lines().size());

        // recalculate for surface cut
        if (use_surface || font_prop.per_glyph) 
            process(false);
    }

    // Apply rotation on model (backend)
    if (m_imgui->get_last_slider_status().deactivated_after_edit) {
        m_parent.do_rotate(rotation_snapshot_name);

        // recalculate for surface cut
        if (use_surface || font_prop.per_glyph)
            process();
    }

    // Keep up - lock button icon
    if (!m_volume->is_the_only_one_part()) {
        ImGui::SameLine(m_gui_cfg->lock_offset);
        const IconManager::Icon &icon = get_icon(m_icons, m_keep_up ? IconType::lock : IconType::unlock, IconState::activable);
        const IconManager::Icon &icon_hover = get_icon(m_icons, m_keep_up ? IconType::lock_bold : IconType::unlock_bold, IconState::activable);
        const IconManager::Icon &icon_disable = get_icon(m_icons, m_keep_up ? IconType::lock : IconType::unlock, IconState::disabled);
        if (button(icon, icon_hover, icon_disable))
            m_keep_up = !m_keep_up;
    
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(m_keep_up?
                _u8L("Unlock the text's rotation when moving text along the object's surface."):
                _u8L("Lock the text's rotation when moving text along the object's surface.")
            , m_gui_cfg->max_tooltip_width);
    }

    // when more collection add selector
    if (ff.font_file->infos.size() > 1) {
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s", tr.collection.c_str());
        ImGui::SameLine(m_gui_cfg->advanced_input_offset);
        ImGui::SetNextItemWidth(m_gui_cfg->input_width);
        unsigned int selected = current_prop.collection_number.value_or(0);
        std::string tooltip = "";
        ImGuiWrapper::push_combo_style(m_parent.get_scale());
        if (ImGui::BBLBeginCombo("## Font collection", std::to_string(selected).c_str())) {
            for (unsigned int i = 0; i < ff.font_file->infos.size(); ++i) {
                ImGui::PushID(1 << (10 + i));
                bool is_selected = (i == selected);
                if (ImGui::BBLSelectable(std::to_string(i).c_str(), is_selected)) {
                    if (i == 0) current_prop.collection_number.reset();
                    else current_prop.collection_number = i;
                    exist_change = true;
                    last_change = true;
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        } else if (ImGui::IsItemHovered()) {
            tooltip = _u8L("Select from True Type Collection.");
        }
        ImGuiWrapper::pop_combo_style();
        if (!tooltip.empty())
            m_imgui->tooltip(tooltip, m_gui_cfg->max_tooltip_width);
    }

    if (exist_change || last_change) {
        m_style_manager.clear_glyphs_cache();
        if (font_prop.per_glyph)
            reinit_text_lines();
        else
            m_text_lines.reset();
        process(last_change);
    }

    if (ImGui::Button(_u8L("Set text to face camera").c_str())) {
        assert(get_selected_volume(m_parent.get_selection()) == m_volume);
        const Camera &cam = wxGetApp().plater()->get_camera();
        auto wanted_up_limit = m_keep_up ? std::optional<double>(UP_LIMIT) : std::optional<double>{};
        if (face_selected_volume_to_camera(cam, m_parent, wanted_up_limit))
            volume_transformation_changed();
    } else if (ImGui::IsItemHovered()) {
        m_imgui->tooltip(_u8L("Orient the text towards the camera."), m_gui_cfg->max_tooltip_width);
    }

    //ImGui::SameLine(); if (ImGui::Button("Re-emboss")) GLGizmoEmboss::re_emboss(*m_volume);    

#ifdef ALLOW_DEBUG_MODE
    ImGui::Text("family = %s", (current_prop.family.has_value() ?
                                    current_prop.family->c_str() :
                                    " --- "));
    ImGui::Text("face name = %s", (current_prop.face_name.has_value() ?
                                       current_prop.face_name->c_str() :
                                       " --- "));
    ImGui::Text("style = %s",
                (current_prop.style.has_value() ? current_prop.style->c_str() :
                                                 " --- "));
    ImGui::Text("weight = %s", (current_prop.weight.has_value() ?
                                    current_prop.weight->c_str() :
                                    " --- "));

    std::string descriptor = style.path;
    ImGui::Text("descriptor = %s", descriptor.c_str());
#endif // ALLOW_DEBUG_MODE
}

#ifdef ALLOW_ADD_FONT_BY_OS_SELECTOR
bool GLGizmoEmboss::choose_font_by_wxdialog()
{
    wxFontData data;
    data.EnableEffects(false);
    data.RestrictSelection(wxFONTRESTRICT_SCALABLE);
    // set previous selected font
    EmbossStyle &selected_style = m_style_manager.get_style();
    if (selected_style.type == WxFontUtils::get_current_type()) {
        std::optional<wxFont> selected_font = WxFontUtils::load_wxFont(
            selected_style.path);
        if (selected_font.has_value()) data.SetInitialFont(*selected_font);
    }

    wxFontDialog font_dialog(wxGetApp().mainframe, data);
    if (font_dialog.ShowModal() != wxID_OK) return false;

    data                = font_dialog.GetFontData();
    wxFont   wx_font       = data.GetChosenFont();
    size_t   font_index = m_style_manager.get_fonts().size();
    EmbossStyle emboss_style  = WxFontUtils::create_emboss_style(wx_font);

    // Check that deserialization NOT influence font
    // false - use direct selected wxFont in dialog
    // true - use font item (serialize and deserialize wxFont)
    bool use_deserialized_font = false;

    // Try load and use new added font
    if ((use_deserialized_font && !m_style_manager.load_style(font_index)) ||
        (!use_deserialized_font && !m_style_manager.load_style(emboss_style, wx_font))) {
        m_style_manager.erase(font_index);
        wxString message = GUI::format_wxstr(
            _L("Font \"%1%\" can't be used. Please select another."),
            emboss_style.name);
        wxString      title = "Selected font is NOT True-type.";
        MessageDialog not_loaded_font_message(nullptr, message, title, wxOK);
        not_loaded_font_message.ShowModal();
        return choose_font_by_wxdialog();
    }

    // fix dynamic creation of italic font
    const auto& cn = m_style_manager.get_font_prop().collection_number;
    unsigned int font_collection = cn.has_value() ? *cn : 0;
    const auto&ff = m_style_manager.get_font_file_with_cache();
    if (WxFontUtils::is_italic(wx_font) &&
        !Emboss::is_italic(*ff.font_file, font_collection)) {
        m_style_manager.get_font_prop().skew = 0.2;
    }
    return true;
}
#endif // ALLOW_ADD_FONT_BY_OS_SELECTOR

#if defined ALLOW_ADD_FONT_BY_FILE || defined ALLOW_DEBUG_MODE
namespace priv {
static std::string get_file_name(const std::string &file_path)
{
    size_t pos_last_delimiter = file_path.find_last_of("/\\");
    size_t pos_point          = file_path.find_last_of('.');
    size_t offset             = pos_last_delimiter + 1;
    size_t count              = pos_point - pos_last_delimiter - 1;
    return file_path.substr(offset, count);
}
} // namespace priv
#endif // ALLOW_ADD_FONT_BY_FILE || ALLOW_DEBUG_MODE

#ifdef ALLOW_ADD_FONT_BY_FILE
bool GLGizmoEmboss::choose_true_type_file()
{
    wxArrayString input_files;
    wxString      fontDir      = wxEmptyString;
    wxString      selectedFile = wxEmptyString;
    wxFileDialog  dialog(nullptr, "Choose one or more files (TTF, TTC):",
                        fontDir, selectedFile, file_wildcards(FT_FONTS),
                        wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() == wxID_OK) dialog.GetPaths(input_files);
    if (input_files.IsEmpty()) return false;
    size_t index = m_style_manager.get_fonts().size();
    // use first valid font
    for (auto &input_file : input_files) {
        std::string path = std::string(input_file.c_str());
        std::string name = get_file_name(path);
        //make_unique_name(name, m_font_list);
        const FontProp& prop = m_style_manager.get_font_prop();
        EmbossStyle style{ name, path, EmbossStyle::Type::file_path, prop };
        m_style_manager.add_font(style);
        // set first valid added font as active
        if (m_style_manager.load_style(index)) return true;
        m_style_manager.erase(index);       
    }
    return false;
}
#endif // ALLOW_ADD_FONT_BY_FILE

void GLGizmoEmboss::create_notification_not_valid_font(
    const TextConfiguration &tc)
{
    const EmbossStyle &es = m_style_manager.get_style();
    const auto &face_name_opt = es.prop.face_name;
    const std::string &face_name_3mf = tc.style.prop.face_name.value_or(tc.style.path);

    std::optional<std::string> face_name_by_wx;
    if (!face_name_opt.has_value()) {
        const wxFont& wx_font = m_style_manager.get_wx_font();
        if (wx_font.IsOk()) {
            wxString wx_face_name = wx_font.GetFaceName();
            if (!wx_face_name.empty())
                face_name_by_wx = std::string(wx_face_name.ToUTF8().data());
        }
    }
    const std::string &face_name = face_name_opt.value_or(face_name_by_wx.value_or(es.path));
    std::string text =
        GUI::format(_L("Can't load exactly same font (\"%1%\"). "
                       "Application selected a similar one (\"%2%\"). "
                       "You have to specify font for enable edit text."),
                    face_name_3mf, face_name);
    create_notification_not_valid_font(text);
}

void GLGizmoEmboss::create_notification_not_valid_font(const std::string &text) {
    // not neccessary, but for sure that old notification doesnt exist
    if (m_is_unknown_font)
        remove_notification_not_valid_font();
    m_is_unknown_font = true;

    auto type  = NotificationType::UnknownFont;
    auto level = NotificationManager::NotificationLevel::WarningNotificationLevel;
    auto notification_manager = wxGetApp().plater()->get_notification_manager();
    notification_manager->push_notification(type, level, text);
}

void GLGizmoEmboss::remove_notification_not_valid_font()
{
    if (!m_is_unknown_font) return;
    m_is_unknown_font      = false;
    auto type                 = NotificationType::UnknownFont;
    auto notification_manager = wxGetApp().plater()->get_notification_manager();
    notification_manager->close_notification_of_type(type);
}

void GLGizmoEmboss::init_icons()
{
    // icon order has to match the enum IconType
    std::vector<std::string> filenames{
        "edit_button.svg",
        "delete.svg",
        "add_copies.svg", 
        "save.svg", 
        "undo.svg",    
        "make_italic.svg",
        "make_unitalic.svg",
        "make_bold.svg",
        "make_unbold.svg",   
        "search.svg",
        "open.svg", 
        "obj_warning.svg",  // ORCA: use obj_warning instead exclamation. exclamation is not compatible with low res
        "lock_closed.svg",  // lock,
        "lock_closed_f.svg",// lock_bold,
        "lock_open.svg",    // unlock,
        "lock_open_f.svg",  // unlock_bold,
        "align_horizontal_left.svg", 
        "align_horizontal_center.svg",
        "align_horizontal_right.svg",
        "align_vertical_top.svg",
        "align_vertical_center.svg",
        "align_vertical_bottom.svg"
    };
    assert(filenames.size() == static_cast<size_t>(IconType::_count));
    std::string path = resources_dir() + "/images/";
    for (std::string &filename : filenames) filename = path + filename;

    ImVec2 size(m_gui_cfg->icon_width, m_gui_cfg->icon_width);
    auto type = IconManager::RasterType::color_wite_gray;
    m_icons = m_icon_manager.init(filenames, size, type);
}

static std::size_t hash_value(wxString const &s){
    boost::hash<std::string> hasher;
    return hasher(s.ToStdString());
}

// increase number when change struct FacenamesSerializer
constexpr std::uint32_t FACENAMES_VERSION = 1;
struct FacenamesSerializer
{
    // hash number for unsorted vector of installed font into system
    size_t hash = 0;
    // assumption that is loadable
    std::vector<wxString> good;
    // Can't load for some reason
    std::vector<wxString> bad;
};

template<class Archive> void save(Archive &archive, wxString const &d)
{ std::string s(d.ToUTF8().data()); archive(s);}
template<class Archive> void load(Archive &archive, wxString &d)
{ std::string s; archive(s); d = s;}
template<class Archive> void serialize(Archive &ar, FacenamesSerializer &t, const std::uint32_t version)
{
    // When performing a load, the version associated with the class
    // is whatever it was when that data was originally serialized
    // When we save, we'll use the version that is defined in the macro
    if (version != FACENAMES_VERSION) return;
    ar(t.hash, t.good, t.bad);
}
CEREAL_CLASS_VERSION(::FacenamesSerializer, FACENAMES_VERSION); // register class version

/////////////
// private namespace implementation
///////////////
namespace {

const IconManager::Icon &get_icon(const IconManager::VIcons& icons, IconType type, IconState state) { 
    return *icons[(unsigned) type][(unsigned) state]; }

bool draw_button(const IconManager::VIcons &icons, IconType type, bool disable){
    return Slic3r::GUI::button(
        get_icon(icons, type, IconState::activable),
        get_icon(icons, type, IconState::hovered),
        get_icon(icons, type, IconState::disabled),
        disable);}

TextDataBase::TextDataBase(DataBase               &&parent,
                           const FontFileWithCache &font_file,
                           TextConfiguration      &&text_configuration,
                           const EmbossProjection  &projection)
    : DataBase(std::move(parent)), m_font_file(font_file) /* copy */, m_text_configuration(std::move(text_configuration))
{
    assert(m_font_file.has_value());
    shape.projection = projection; // copy

    const FontProp &fp = m_text_configuration.style.prop;
    const FontFile &ff = *m_font_file.font_file;
    shape.scale = get_text_shape_scale(fp, ff);
}

EmbossShape &TextDataBase::create_shape()
{
    if (!shape.shapes_with_ids.empty())
        return shape;

    // create shape by configuration
    const char *text = m_text_configuration.text.c_str();
    std::wstring text_w = boost::nowide::widen(text);
    const FontProp &fp = m_text_configuration.style.prop;
    auto was_canceled = [&c = cancel](){ return c->load(); };

    shape.shapes_with_ids = text2vshapes(m_font_file, text_w, fp, was_canceled);
    return shape;
}

void TextDataBase::write(ModelVolume &volume) const
{
    DataBase::write(volume);
    volume.text_configuration = m_text_configuration; // copy
    assert(volume.emboss_shape.has_value());
}

std::unique_ptr<DataBase> create_emboss_data_base(const std::string                  &text,
                                       StyleManager                       &style_manager,
                                       TextLinesModel                     &text_lines,
                                       const Selection                    &selection,
                                       ModelVolumeType                     type,
                                       std::shared_ptr<std::atomic<bool>> &cancel)
{
    // create volume_name
    std::string volume_name = text; // copy
    // contain_enter?
    if (volume_name.find('\n') != std::string::npos)
        // change enters to space
        std::replace(volume_name.begin(), volume_name.end(), '\n', ' ');

    if (!style_manager.is_active_font()) {
        style_manager.load_valid_style();
        assert(style_manager.is_active_font());
        if (!style_manager.is_active_font())
            return {}; // no active font in style, should never happend !!!
    }

    const StyleManager::Style &style = style_manager.get_style();
    // actualize font path - during changes in gui it could be corrupted
    // volume must store valid path
    assert(style_manager.get_wx_font().IsOk());
    assert(style.path.compare(WxFontUtils::store_wxFont(style_manager.get_wx_font())) == 0);

    if (style.prop.per_glyph) {
        if (!text_lines.is_init())
            init_text_lines(text_lines, selection, style_manager);
    } else
        text_lines.reset();
    
    bool is_outside = (type == ModelVolumeType::MODEL_PART);

    // Cancel previous Job, when it is in process
    // worker.cancel(); --> Use less in this case I want cancel only previous EmbossJob no other jobs
    // Cancel only EmbossUpdateJob no others
    if (cancel != nullptr)
        cancel->store(true);
    // create new shared ptr to cancel new job
    cancel = std::make_shared<std::atomic<bool>>(false);

    DataBase base(volume_name, cancel);
    base.is_outside = is_outside;
    base.text_lines = text_lines.get_lines();
    base.from_surface = style.distance;

    FontFileWithCache &font = style_manager.get_font_file_with_cache();
    TextConfiguration tc{static_cast<EmbossStyle>(style), text};
    return std::make_unique<TextDataBase>(std::move(base), font, std::move(tc), style.projection);
}

CreateVolumeParams create_input(GLCanvas3D &canvas, const StyleManager::Style &style, RaycastManager& raycaster, ModelVolumeType volume_type)
{
    auto gizmo = static_cast<unsigned char>(GLGizmosManager::Emboss);
    const GLVolume *gl_volume = get_first_hovered_gl_volume(canvas);
    Plater *plater = wxGetApp().plater();
    return CreateVolumeParams{canvas, plater->get_camera(), plater->build_volume(),
        plater->get_ui_job_worker(), volume_type, raycaster, gizmo, gl_volume, style.distance, style.angle};
}

ImVec2 calc_fine_position(const Selection &selection, const ImVec2 &windows_size, const Size &canvas_size)
{
    const Selection::IndicesList indices = selection.get_volume_idxs();
    // no selected volume
    if (indices.empty())
        return {};
    const GLVolume *volume = selection.get_volume(*indices.begin());
    // bad volume selected (e.g. deleted one)
    if (volume == nullptr)
        return {};

    const Camera   &camera = wxGetApp().plater()->get_camera();
    Slic3r::Polygon hull   = CameraUtils::create_hull2d(camera, *volume);

    ImVec2 c_size(canvas_size.get_width(), canvas_size.get_height());
    ImVec2 offset = ImGuiWrapper::suggest_location(windows_size, hull, c_size);
    return offset;
}

std::string concat(std::vector<wxString> data) {
    std::stringstream ss;
    for (const auto &d : data) 
        ss << d.c_str() << ", ";
    return ss.str();
}

boost::filesystem::path get_fontlist_cache_path(){
    return boost::filesystem::path(data_dir()) / "cache" / "fonts.cereal";
}

bool store(const Facenames &facenames) {
    std::string cache_path = get_fontlist_cache_path().string();
    boost::nowide::ofstream file(cache_path, std::ios::binary);
    ::cereal::BinaryOutputArchive archive(file);
    std::vector<wxString> good;
    good.reserve(facenames.faces.size());
    for (const FaceName &face : facenames.faces) good.push_back(face.wx_name);
    FacenamesSerializer data = {facenames.hash, good, facenames.bad};

    assert(std::is_sorted(data.bad.begin(), data.bad.end()));
    assert(std::is_sorted(data.good.begin(), data.good.end()));

    try {
        archive(data);
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(error) << "Failed to write fontlist cache - " << cache_path << ex.what();
        return false;
    }
    return true;
}

bool load(Facenames &facenames) {
    boost::filesystem::path path = get_fontlist_cache_path();
    std::string             path_str = path.string();
    if (!boost::filesystem::exists(path)) {
        BOOST_LOG_TRIVIAL(warning) << "Fontlist cache - '" << path_str << "' does not exists.";
        return false;
    }
    boost::nowide::ifstream file(path_str, std::ios::binary);
    cereal::BinaryInputArchive archive(file);
    
    FacenamesSerializer data;
    try {
        archive(data);
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(error) << "Failed to load fontlist cache - '" << path_str << "'. Exception: " << ex.what();
        return false;
    }

    assert(std::is_sorted(data.bad.begin(), data.bad.end()));
    assert(std::is_sorted(data.good.begin(), data.good.end()));

    facenames.hash = data.hash;
    facenames.faces.reserve(data.good.size());
    facenames.faces_names.reserve(data.good.size());
    for (const wxString &face : data.good) {
        facenames.faces.push_back({face});
        facenames.faces_names.push_back(face.utf8_string());
    }
    facenames.bad = data.bad;
    return true;
}

void init_truncated_names(Facenames &face_names, float max_width)
{
    for (FaceName &face : face_names.faces) {
        std::string name_str(face.wx_name.ToUTF8().data());
        face.name_truncated = ImGuiWrapper::trunc(name_str, max_width);
    }
    face_names.has_truncated_names = true;
}

void init_face_names(Facenames &face_names)
{
    Timer t("enumerate_fonts");
    if (face_names.is_init) return;
    face_names.is_init = true;

    // to reload fonts from system, when install new one
    wxFontEnumerator::InvalidateCache();

    // try load cache
    // Only not OS enumerated face has hash value 0
    if (face_names.hash == 0) {
        load(face_names);
        face_names.has_truncated_names = false;
    }

    using namespace std::chrono;
    steady_clock::time_point enumerate_start = steady_clock::now();
    ScopeGuard sg([&enumerate_start, &face_names = face_names]() {
        steady_clock::time_point enumerate_end = steady_clock::now();
        long long enumerate_duration = duration_cast<milliseconds>(enumerate_end - enumerate_start).count();
        BOOST_LOG_TRIVIAL(info) << "OS enumerate " << face_names.faces.size() << " fonts "
                                << "(+ " << face_names.bad.size() << " can't load "
                                << "= " << face_names.faces.size() + face_names.bad.size() << " fonts) "
                                << "in " << enumerate_duration << " ms\n" << concat(face_names.bad);
    });
    wxArrayString facenames = wxFontEnumerator::GetFacenames(face_names.encoding);
    size_t hash = boost::hash_range(facenames.begin(), facenames.end());
    // Zero value is used as uninitialized hash
    if (hash == 0) hash = 1;
    // check if it is same as last time
    if (face_names.hash == hash) { 
        // no new installed font
        BOOST_LOG_TRIVIAL(info) << "Same FontNames hash, cache is used. " 
            << "For clear cache delete file: " << get_fontlist_cache_path().string();
        return;
    }

    BOOST_LOG_TRIVIAL(info) << ((face_names.hash == 0) ?
        "FontName list is generate from scratch." :
        "Hash are different. Only previous bad fonts are used and set again as bad");
    face_names.hash = hash;
    
    // validation lambda
    auto is_valid_font = [encoding = face_names.encoding, bad = face_names.bad /*copy*/](const wxString &name) {
        if (name.empty()) return false;

        // vertical font start with @, we will filter it out
        // Not sure if it is only in Windows so filtering is on all platforms
        if (name[0] == '@') return false;        

        // previously detected bad font
        auto it = std::lower_bound(bad.begin(), bad.end(), name);
        if (it != bad.end() && *it == name) return false;

        wxFont wx_font(wxFontInfo().FaceName(name).Encoding(encoding));
        //*
        // Faster chech if wx_font is loadable but not 100%
        // names could contain not loadable font
        if (!WxFontUtils::can_load(wx_font)) return false;

        /*/
        // Slow copy of font files to try load font
        // After this all files are loadable
        auto font_file = WxFontUtils::create_font_file(wx_font);
        if (font_file == nullptr) 
            return false; // can't create font file
        // */
        return true;
    };

    face_names.faces.clear();
    face_names.faces_names.clear();
    face_names.bad.clear();
    face_names.faces.reserve(facenames.size());
    face_names.faces_names.reserve(facenames.size());
    std::sort(facenames.begin(), facenames.end());
    for (const wxString &name : facenames) {
        if (is_valid_font(name)) {
            face_names.faces.push_back({name});
            face_names.faces_names.push_back(name.utf8_string());
        }else{
            face_names.bad.push_back(name);
        }
    }
    assert(std::is_sorted(face_names.bad.begin(), face_names.bad.end()));
    face_names.has_truncated_names = false;
    store(face_names);
}

void draw_font_preview(FaceName &face, const std::string& text, Facenames &faces, const GuiCfg &cfg, bool is_visible){
    // Limit for opened font files at one moment
    unsigned int &count_opened_fonts = faces.count_opened_font_files; 
    // Size of texture
    ImVec2 size(cfg.face_name_size.x(), cfg.face_name_size.y());
    float  count_cached_textures_f = static_cast<float>(faces.count_cached_textures);
    std::string state_text;
    // uv0 and uv1 set to pixel 0,0 in texture
    ImVec2 uv0(0.f, 0.f), uv1(1.f / size.x, 1.f / size.y / count_cached_textures_f);
    if (face.is_created != nullptr) {
        // not created preview 
        if (*face.is_created) {
            // Already created preview
            size_t texture_index = face.texture_index;
            uv0 = ImVec2(0.f, texture_index / count_cached_textures_f);
            uv1 = ImVec2(1.f, (texture_index + 1) / count_cached_textures_f);
        } else {
            // Not finished preview
            if (is_visible) {
                // when not canceled still loading
                state_text = (face.cancel->load()) ?
                    " " + _u8L("No symbol"):
                    " ... " + _u8L("Loading");
            } else {
                // not finished and not visible cancel job
                face.is_created = nullptr;
                face.cancel->store(true);
            }
        }
    } else if (is_visible && count_opened_fonts < cfg.max_count_opened_font_files) {
        ++count_opened_fonts;
        face.cancel     = std::make_shared<std::atomic_bool>(false);
        face.is_created = std::make_shared<bool>(false);

        const unsigned char gray_level = 5;
        // format type and level must match to texture data
        const GLenum format = GL_RGBA, type = GL_UNSIGNED_BYTE;
        const GLint  level = 0;
        // select next texture index
        size_t texture_index = (faces.texture_index + 1) % faces.count_cached_textures;

        // set previous cach as deleted
        for (FaceName &f : faces.faces)
            if (f.texture_index == texture_index) {
                if (f.cancel != nullptr) f.cancel->store(true);
                f.is_created = nullptr;
            }

        faces.texture_index = texture_index;
        face.texture_index         = texture_index;

        // render text to texture
        FontImageData data{
            text,
            face.wx_name,
            faces.encoding,
            faces.texture_id,
            faces.texture_index,
            cfg.face_name_size,
            gray_level,
            format,
            type,
            level,
            &count_opened_fonts,
            face.cancel,    // copy
            face.is_created // copy
        };
        auto  job    = std::make_unique<CreateFontImageJob>(std::move(data));
        auto &worker = wxGetApp().plater()->get_ui_job_worker();
        queue_job(worker, std::move(job));
    } else {
        // cant start new thread at this moment so wait in queue
        state_text = " ... " + _u8L("In queue");
    }

    if (!state_text.empty()) {
        ImGui::SameLine(cfg.face_name_texture_offset_x);
        ImGui::Text("%s", state_text.c_str());
    }

    ImGui::SameLine(cfg.face_name_texture_offset_x);
    ImTextureID tex_id     = (void *) (intptr_t) faces.texture_id;
    ImVec4 tint_color = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    ImGui::Image(tex_id, size, uv0, uv1, tint_color);
}

GuiCfg create_gui_configuration()
{
    GuiCfg cfg; // initialize by default values;

    float line_height = ImGui::GetTextLineHeight();
    float line_height_with_spacing = ImGui::GetTextLineHeightWithSpacing();
    float space = line_height_with_spacing - line_height;
    const ImGuiStyle &style  = ImGui::GetStyle();

    cfg.max_style_name_width = ImGui::CalcTextSize("Maximal style name..").x;

    cfg.icon_width = static_cast<unsigned int>(std::ceil(line_height));
    // make size pair number
    if (cfg.icon_width % 2 != 0) ++cfg.icon_width;

    cfg.delete_pos_x = cfg.max_style_name_width + space;
    const float count_line_of_text = 3.f;
    cfg.text_size = ImVec2(-FLT_MIN, line_height_with_spacing * count_line_of_text);
    ImVec2 letter_m_size = ImGui::CalcTextSize("M");
    const float count_letter_M_in_input = 12.f;
    cfg.input_width = letter_m_size.x * count_letter_M_in_input;
    GuiCfg::Translations &tr = cfg.translations;

    // TRN - Input label. Be short as possible
    // Select look of letter shape
    tr.font   = _u8L("Font");
    // TRN - Input label. Be short as possible
    // Height of one text line - Font Ascent
    tr.height = _u8L("Height");
    // TRN - Input label. Be short as possible
    // Size in emboss direction
    tr.depth  = _u8L("Depth");

    float max_text_width = std::max({
        ImGui::CalcTextSize(tr.font.c_str()).x,
        ImGui::CalcTextSize(tr.height.c_str()).x,
        ImGui::CalcTextSize(tr.depth.c_str()).x});
    cfg.indent       = static_cast<float>(cfg.icon_width);
    cfg.input_offset = style.WindowPadding.x + cfg.indent + max_text_width + space;

    // TRN - Input label. Be short as possible
    // Copy surface of model on surface of the embossed text
    tr.use_surface = _u8L("Use surface");
    // TRN - Input label. Be short as possible
    // Option to change projection on curved surface 
    // for each character(glyph) in text separately
    tr.per_glyph = _u8L("Per glyph");
    // TRN - Input label. Be short as possible
    // Align Top|Middle|Bottom and Left|Center|Right
    tr.alignment = _u8L("Alignment");
    // TRN - Input label. Be short as possible
    tr.char_gap = _u8L("Char gap");
    // TRN - Input label. Be short as possible
    tr.line_gap = _u8L("Line gap");
    // TRN - Input label. Be short as possible
    tr.boldness = _u8L("Boldness");

    // TRN - Input label. Be short as possible
    // Like Font italic 
    tr.skew_ration = _u8L("Skew ratio");

    // TRN - Input label. Be short as possible
    // Distance from model surface to be able 
    // move text as part fully into not flat surface
    // move text as modifier fully out of not flat surface
    tr.from_surface = _u8L("From surface");

    // TRN - Input label. Be short as possible
    // Angle between Y axis and text line direction.
    tr.rotation = _u8L("Rotation");

    // TRN - Input label. Be short as possible
    // Keep vector from bottom to top of text aligned with printer Y axis
    tr.keep_up = _u8L("Keep up");

    // TRN - Input label. Be short as possible. 
    // Some Font file contain multiple fonts inside and
    // this is numerical selector of font inside font collections
    tr.collection = _u8L("Collection");

    float max_advanced_text_width = std::max({
        ImGui::CalcTextSize(tr.use_surface.c_str()).x,
        ImGui::CalcTextSize(tr.per_glyph.c_str()).x,
        ImGui::CalcTextSize(tr.alignment.c_str()).x,
        ImGui::CalcTextSize(tr.char_gap.c_str()).x,
        ImGui::CalcTextSize(tr.line_gap.c_str()).x,
        ImGui::CalcTextSize(tr.boldness.c_str()).x,
        ImGui::CalcTextSize(tr.skew_ration.c_str()).x,
        ImGui::CalcTextSize(tr.from_surface.c_str()).x,
        ImGui::CalcTextSize(tr.rotation.c_str()).x + cfg.icon_width + 2*space,
        ImGui::CalcTextSize(tr.keep_up.c_str()).x,
        ImGui::CalcTextSize(tr.collection.c_str()).x });
    cfg.advanced_input_offset = max_advanced_text_width
        + 3 * space + cfg.indent;

    cfg.lock_offset = cfg.advanced_input_offset - (cfg.icon_width + space);
    // calculate window size
    float input_height = line_height_with_spacing + 2*style.FramePadding.y;
    float separator_height = 2 + style.FramePadding.y;

    // "Text is to object" + radio buttons
    cfg.height_of_volume_type_selector = separator_height + line_height_with_spacing + input_height;

    int max_style_image_width = static_cast<int>(std::round(cfg.max_style_name_width - 2 * style.FramePadding.x));
    int max_style_image_height = static_cast<int>(std::round(input_height));
    cfg.max_style_image_size = Vec2i32(max_style_image_width, line_height);
    cfg.face_name_size = Vec2i32(cfg.input_width, line_height_with_spacing);
    cfg.face_name_texture_offset_x = cfg.face_name_size.x() + style.WindowPadding.x + space;

    cfg.max_tooltip_width = ImGui::GetFontSize() * 20.0f;

    return cfg;
}
} // namespace
