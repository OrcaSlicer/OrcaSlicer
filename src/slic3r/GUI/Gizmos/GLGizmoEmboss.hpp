#ifndef slic3r_GLGizmoEmboss_hpp_
#define slic3r_GLGizmoEmboss_hpp_

#include "GLGizmoBase.hpp"
#include "GLGizmoRotate.hpp"
#include "slic3r/GUI/IconManager.hpp"
#include "slic3r/GUI/SurfaceDrag.hpp"
#include "slic3r/GUI/I18N.hpp" // TODO: not needed
#include "slic3r/GUI/TextLines.hpp"
#include "slic3r/Utils/RaycastManager.hpp"
#include "slic3r/Utils/EmbossStyleManager.hpp"

#include <optional>
#include <memory>
#include <atomic>

#include "libslic3r/Emboss.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TextConfiguration.hpp"

#include <imgui/imgui.h>
#include <GL/glew.h>

class wxFont;
namespace Slic3r{
    class AppConfig;
    class GLVolume;
    enum class ModelVolumeType : int;
}

namespace Slic3r::GUI {
class GLGizmoEmboss : public GLGizmoBase
{
public:
    explicit GLGizmoEmboss(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id);

    /// <summary>
    /// Create new embossed text volume by type on position of mouse
    /// </summary>
    /// <param name="volume_type">Object part / Negative volume / Modifier</param>
    /// <param name="mouse_pos">Define position of new volume</param>
    bool create_volume(ModelVolumeType volume_type, const Vec2d &mouse_pos);

    /// <summary>
    /// Create new text without given position
    /// </summary>
    /// <param name="volume_type">Object part / Negative volume / Modifier</param>
    bool create_volume(ModelVolumeType volume_type);

    /// <summary>
    /// Handle pressing of shortcut
    /// </summary>
    void on_shortcut_key();

    /// <summary>
    /// Mirroring from object manipulation panel
    /// !! Emboss gizmo must be active
    /// </summary>
    /// <param name="axis">Axis for mirroring must be one of {0,1,2}</param>
    /// <returns>True on success start job otherwise False</returns>
    bool do_mirror(size_t axis);


    /// <summary>
    /// Call on change inside of object conatining projected volume
    /// </summary>
    /// <param name="job_cancel">Way to stop re_emboss job</param>
    /// <returns>True on success otherwise False</returns>
    static bool re_emboss(const ModelVolume &text, std::shared_ptr<std::atomic<bool>> job_cancel = nullptr);

protected:
    bool on_init() override;
    std::string on_get_name() const override;
    void on_render() override;
    void on_register_raycasters_for_picking() override;
    void on_unregister_raycasters_for_picking() override;
    void on_render_input_window(float x, float y, float bottom_limit) override;
    void on_set_state() override;
    void data_changed(bool is_serializing) override; // selection changed
    void on_set_hover_id() override{ m_rotate_gizmo.set_hover_id(m_hover_id); }
    void on_enable_grabber(unsigned int id) override { m_rotate_gizmo.enable_grabber(); }
    void on_disable_grabber(unsigned int id) override { m_rotate_gizmo.disable_grabber(); }
    void on_start_dragging() override;
    void on_stop_dragging() override;
    void on_dragging(const UpdateData &data) override;    
    void push_button_style(bool pressed);
    void pop_button_style();

    /// <summary>
    /// Rotate by text on dragging rotate grabers
    /// </summary>
    /// <param name="mouse_event">Information about mouse</param>
    /// <returns>Propagete normaly return false.</returns>
    bool on_mouse(const wxMouseEvent &mouse_event) override;

    bool wants_enter_leave_snapshots() const override;
    std::string get_gizmo_entering_text() const override;
    std::string get_gizmo_leaving_text() const override;
    std::string get_action_snapshot_name() const override;

private:
    void volume_transformation_changing();
    void volume_transformation_changed();

    static EmbossStyles create_default_styles();
    // localized default text
    bool init_create(ModelVolumeType volume_type);

    void set_volume_by_selection();
    void reset_volume();

    // create volume from text - main functionality
    bool process(bool make_snapshot = true);
    void close();
    void draw_window();
    void draw_text_input();
    void draw_model_type();
    void draw_style_list();
    void draw_delete_style_button();
    void draw_style_rename_popup();
    void draw_style_rename_button();
    void draw_style_save_button(bool is_modified);
    void draw_style_save_as_popup();
    void draw_style_add_button();
    void init_font_name_texture();
    void draw_font_list_line();
    void draw_font_list();
    void draw_height(bool use_inch);
    void draw_depth(bool use_inch);

    // call after set m_style_manager.get_style().prop.size_in_mm
    bool set_height();

    bool draw_italic_button();
    bool draw_bold_button();
    void draw_advanced();

    bool select_facename(const wxString& facename);

    template<typename T> bool rev_input_mm(const std::string &name, T &value, const T *default_value,
        const std::string &undo_tooltip, T step, T step_fast, const char *format, bool use_inch, const std::optional<float>& scale) const;

    /// <summary>
    /// Reversible input float with option to restor default value
    /// TODO: make more general, static and move to ImGuiWrapper 
    /// </summary>
    /// <returns>True when value changed otherwise FALSE.</returns>
    template<typename T> bool rev_input(const std::string &name, T &value, const T *default_value, 
        const std::string &undo_tooltip, T step, T step_fast, const char *format, ImGuiInputTextFlags flags = 0) const;
    bool rev_checkbox(const std::string &name, bool &value, const bool* default_value, const std::string  &undo_tooltip) const;
    bool rev_slider(const std::string &name, std::optional<int>& value, const std::optional<int> *default_value,
        const std::string &undo_tooltip, int v_min, int v_max, const std::string &format, const wxString &tooltip) const;
    bool rev_slider(const std::string &name, std::optional<float>& value, const std::optional<float> *default_value,
        const std::string &undo_tooltip, float v_min, float v_max, const std::string &format, const wxString &tooltip) const;
    bool rev_slider(const std::string &name, float &value, const float *default_value, 
        const std::string &undo_tooltip, float v_min, float v_max, const std::string &format, const wxString &tooltip) const;
    template<typename T, typename Draw> bool revertible(const std::string &name, T &value, const T *default_value,
        const std::string &undo_tooltip, float undo_offset, Draw draw) const;

    // process mouse event
    bool on_mouse_for_rotation(const wxMouseEvent &mouse_event);
    bool on_mouse_for_translate(const wxMouseEvent &mouse_event);
    void on_mouse_change_selection(const wxMouseEvent &mouse_event);

    // When open text loaded from .3mf it could be written with unknown font
    bool m_is_unknown_font = false;
    void create_notification_not_valid_font(const TextConfiguration& tc);
    void create_notification_not_valid_font(const std::string& text);
    void remove_notification_not_valid_font();

    struct GuiCfg;
    std::unique_ptr<const GuiCfg> m_gui_cfg;

    // Is open tree with advanced options
    bool m_is_advanced_edit_style = false;

    // Keep information about stored styles and loaded actual style to compare with
    Emboss::StyleManager m_style_manager;

    // pImpl to hide implementation of FaceNames to .cpp file
    struct Facenames; // forward declaration
    std::unique_ptr<Facenames> m_face_names;

    // Text to emboss
    std::string m_text; // Sequence of Unicode UTF8 symbols

    // When true keep up vector otherwise relative rotation
    bool m_keep_up = true;

    // current selected volume 
    // NOTE: Be carefull could be uninitialized (removed from Model)
    ModelVolume *m_volume = nullptr;

    // When work with undo redo stack there could be situation that 
    // m_volume point to unexisting volume so One need also objectID
    ObjectID m_volume_id;

    // True when m_text contain character unknown by selected font
    bool m_text_contain_unknown_glyph = false;

    // cancel for previous update of volume to cancel finalize part
    std::shared_ptr<std::atomic<bool>> m_job_cancel = nullptr;

    // Keep information about curvature of text line around surface
    TextLinesModel m_text_lines;
    void reinit_text_lines(unsigned count_lines=0);

    // Rotation gizmo
    GLGizmoRotate m_rotate_gizmo;
    // Value is set only when dragging rotation to calculate actual angle
    std::optional<float> m_rotate_start_angle;

    // Keep data about dragging only during drag&drop
    std::optional<SurfaceDrag> m_surface_drag;

    // Keep old scene triangle data in AABB trees, 
    // all the time it need actualize before use.
    RaycastManager m_raycast_manager;

    // For text on scaled objects
    std::optional<float> m_scale_height;
    std::optional<float> m_scale_depth;
    void calculate_scale();

    // drawing icons
    IconManager m_icon_manager;
    IconManager::VIcons m_icons;
    void init_icons();

    // only temporary solution
    static const std::string M_ICON_FILENAME;
};

} // namespace Slic3r::GUI

#endif // slic3r_GLGizmoEmboss_hpp_
