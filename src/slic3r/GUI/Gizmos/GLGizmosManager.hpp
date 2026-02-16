#ifndef slic3r_GUI_GLGizmosManager_hpp_
#define slic3r_GUI_GLGizmosManager_hpp_

#include "slic3r/GUI/GLTexture.hpp"
#include "slic3r/GUI/GLToolbar.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoBase.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosCommon.hpp"
//BBS: GUI refactor: add object manipulation
#include "slic3r/GUI/Gizmos/GizmoObjectManipulation.hpp"

#include "libslic3r/ObjectID.hpp"

#include <wx/timer.h>
#include <map>

//BBS: GUI refactor: to support top layout
#define BBS_TOOLBAR_ON_TOP 1

namespace Slic3r {

namespace UndoRedo {
struct Snapshot;
}

namespace GUI {

class GLCanvas3D;
class ClippingPlane;
enum class SLAGizmoEventType : unsigned char;
class CommonGizmosDataPool;
//BBS: GUI refactor: add object manipulation
class GizmoObjectManipulation;
class Rect
{
    float m_left{ 0.0f };
    float m_top{ 0.0f };
    float m_right{ 0.0f };
    float m_bottom{ 0.0f };

public:
    Rect() = default;
    Rect(float left, float top, float right, float bottom) : m_left(left) , m_top(top) , m_right(right) , m_bottom(bottom) {}

    bool operator == (const Rect& other) const {
        if (std::abs(m_left - other.m_left) > EPSILON) return false;
        if (std::abs(m_top - other.m_top) > EPSILON) return false;
        if (std::abs(m_right - other.m_right) > EPSILON) return false;
        if (std::abs(m_bottom - other.m_bottom) > EPSILON) return false;
        return true;
    }
    bool operator != (const Rect& other) const { return !operator==(other); }

    float get_left() const { return m_left; }
    void set_left(float left) { m_left = left; }

    float get_top() const { return m_top; }
    void set_top(float top) { m_top = top; }

    float get_right() const { return m_right; }
    void set_right(float right) { m_right = right; }

    float get_bottom() const { return m_bottom; }
    void set_bottom(float bottom) { m_bottom = bottom; }

    float get_width() const { return m_right - m_left; }
    float get_height() const { return m_top - m_bottom; }
};

class GLGizmosManager : public Slic3r::ObjectBase
{
public:
    static const float Default_Icons_Size;

    enum EType : unsigned char
    {
        // Order must match index in m_gizmos!
        Move,
        Rotate,
        Scale,
        Flatten,
        Cut,
        MeshBoolean,
        FdmSupports,
        Seam,
        FuzzySkin,
        MmSegmentation,
        Emboss,
        Svg,
        Measure,
        Assembly,
        Simplify,
        BrimEars,
        //SlaSupports,
        // BBS
        //FaceRecognition,
        //Hollow,
        Undefined,
    };

private:
    struct Layout
    {
        float scale{ 1.0f };
        float icons_size{ Default_Icons_Size };
        float border{ 4.0f };
        float gap_y{ 4.0f };
        //BBS: GUI refactor: to support top layout
        float gap_x{ 4.0f };
        float stride_x() const { return icons_size + gap_x;}
        float scaled_gap_x() const { return scale * gap_x; }
        float scaled_stride_x() const { return scale * stride_x(); }

        float stride_y() const { return icons_size + gap_y;}

        float scaled_icons_size() const { return scale * icons_size; }
        float scaled_border() const { return scale * border; }
        float scaled_gap_y() const { return scale * gap_y; }
        float scaled_stride_y() const { return scale * stride_y(); }
    };

    GLCanvas3D& m_parent;
    bool m_enabled;
    std::vector<std::unique_ptr<GLGizmoBase>> m_gizmos;
    GLTexture m_icons_texture;
    bool m_icons_texture_dirty;
    BackgroundTexture m_background_texture;
    GLTexture m_arrow_texture;
    Layout m_layout;
    EType m_current;
    EType m_hover;
    std::pair<EType, bool> m_highlight; // bool true = higlightedShown, false = highlightedHidden

    //BBS: GUI refactor: add object manipulation
    GizmoObjectManipulation m_object_manipulation;

    std::vector<size_t> get_selectable_idxs() const;
    EType get_gizmo_from_mouse(const Vec2d &mouse_pos) const;

    bool activate_gizmo(EType type);

    std::string m_tooltip;
    bool m_serializing;
    std::unique_ptr<CommonGizmosDataPool> m_common_gizmos_data;

    //When there are more than 9 colors, shortcut key coloring
    wxTimer m_timer_set_color;
    void on_set_color_timer(wxTimerEvent& evt);

    // key MENU_ICON_NAME, value = ImtextureID
    static std::map<int, void*> icon_list;

    bool m_is_dark = false;

    /// <summary>
    /// Process mouse event on gizmo toolbar
    /// </summary>
    /// <param name="mouse_event">Event descriptor</param>
    /// <returns>TRUE when take responsibility for event otherwise FALSE.
    /// On true, event should not be process by others.
    /// On false, event should be process by others.</returns>
    bool gizmos_toolbar_on_mouse(const wxMouseEvent &mouse_event);
public:

    std::unique_ptr<AssembleViewDataPool> m_assemble_view_data;
    enum MENU_ICON_NAME {
        IC_TOOLBAR_RESET            = 0,
        IC_TOOLBAR_RESET_HOVER,
        IC_TOOLBAR_RESET_ZERO,
        IC_TOOLBAR_RESET_ZERO_HOVER,
        IC_TOOLBAR_TOOLTIP,
        IC_TOOLBAR_TOOLTIP_HOVER,
        IC_NAME_COUNT,
        IC_CANVAS_MENU,
        IC_CANVAS_MENU_HOVER,
        IC_CANVAS_MENU_DARK,
        IC_CANVAS_MENU_DARK_HOVER,
        IC_CANVAS_ZOOM,
        IC_CANVAS_ZOOM_HOVER,
        IC_CANVAS_ZOOM_DARK,
        IC_CANVAS_ZOOM_DARK_HOVER,
    };

    explicit GLGizmosManager(GLCanvas3D& parent);

    void switch_gizmos_icon_filename();

    bool init();

    bool init_icon_textures();

    float get_layout_scale();

    bool init_arrow(const std::string& filename);

    template<class Archive>
    void load(Archive& ar)
    {
        if (!m_enabled)
            return;

        m_serializing = true;

        // Following is needed to know which to be turn on, but not actually modify
        // m_current prematurely, so activate_gizmo is not confused.
        EType old_current = m_current;
        ar(m_current);
        EType new_current = m_current;
        m_current = old_current;

        // activate_gizmo call sets m_current and calls set_state for the gizmo
        // it does nothing in case the gizmo is already activated
        // it can safely be called for Undefined gizmo
        activate_gizmo(new_current);
        if (m_current != Undefined)
            m_gizmos[m_current]->load(ar);
    }

    template<class Archive>
    void save(Archive& ar) const
    {
        if (!m_enabled)
            return;

        ar(m_current);

        if (m_current != Undefined && !m_gizmos.empty())
            m_gizmos[m_current]->save(ar);
    }

    bool is_enabled() const { return m_enabled; }
    void set_enabled(bool enable) { m_enabled = enable; }

    void set_icon_dirty() { m_icons_texture_dirty = true; }
    void set_overlay_icon_size(float size);
    void set_overlay_scale(float scale);

    void refresh_on_off_state();
    void reset_all_states();
    bool open_gizmo(EType type);
    bool check_gizmos_closed_except(EType) const;

    void set_hover_id(int id);

    /// <summary>
    /// Distribute information about different data into active gizmo
    /// Should be called when selection changed
    /// </summary>
    void update_data();
    void update_assemble_view_data();

    EType get_current_type() const { return m_current; }
    GLGizmoBase* get_current() const;
    GLGizmoBase *get_gizmo(GLGizmosManager::EType type) const;
    EType get_gizmo_from_name(const std::string& gizmo_name) const;

    bool is_running() const;
    bool handle_shortcut(int key);

    bool is_dragging() const;

    //BBS
    void* get_icon_texture_id(MENU_ICON_NAME icon) {
        if (icon_list.find((int)icon) != icon_list.end())
            return icon_list[icon];
        else
            return nullptr;
    }
    void* get_icon_texture_id(MENU_ICON_NAME icon) const{
        if (icon_list.find((int)icon) != icon_list.end())
            return icon_list.at(icon);
        else
            return nullptr;
    }

    bool is_paint_gizmo();
    bool is_allow_select_all();
    ClippingPlane get_clipping_plane() const;
    ClippingPlane get_assemble_view_clipping_plane() const;
    bool wants_reslice_supports_on_undo() const;

    bool is_in_editing_mode(bool error_notification = false) const;
    bool is_hiding_instances() const;

    void on_change_color_mode(bool is_dark);
    void render_current_gizmo() const;
    void render_painter_gizmo();
    void render_painter_assemble_view() const;

    void render_overlay();

    void render_arrow(const GLCanvas3D& parent, EType highlighted_type) const;

    std::string get_tooltip() const;

    bool on_mouse(const wxMouseEvent &mouse_event);
    bool on_mouse_wheel(const wxMouseEvent &evt);
    bool on_char(wxKeyEvent& evt);
    bool on_key(wxKeyEvent& evt);

    void update_after_undo_redo(const UndoRedo::Snapshot& snapshot);

    int get_selectable_icons_cnt() const { return get_selectable_idxs().size(); }

    // To end highlight set gizmo = undefined
    void set_highlight(EType gizmo, bool highlight_shown) { m_highlight = std::pair<EType, bool>(gizmo, highlight_shown); }
    bool get_highlight_state() const { return m_highlight.second; }

    //BBS: GUI refactor: GLToolbar adjust
    float get_scaled_total_height() const;
    float get_scaled_total_width() const;
    GizmoObjectManipulation& get_object_manipulation() { return m_object_manipulation; }
    bool get_uniform_scaling() const { return m_object_manipulation.get_uniform_scaling();}

private:
    bool gizmo_event(SLAGizmoEventType action,
                     const Vec2d &     mouse_position = Vec2d::Zero(),
                     bool              shift_down     = false,
                     bool              alt_down       = false,
                     bool              control_down   = false);
    
    void render_background(float left, float top, float right, float bottom, float border_w, float border_h) const;
    
    void do_render_overlay() const;

    bool generate_icons_texture();

    void update_hover_state(const EType &type);
    bool grabber_contains_mouse() const;
};

std::string get_name_from_gizmo_etype(GLGizmosManager::EType type);

} // namespace GUI
} // namespace Slic3r

namespace cereal
{
    template <class Archive> struct specialize<Archive, Slic3r::GUI::GLGizmosManager, cereal::specialization::member_load_save> {};
}

#endif // slic3r_GUI_GLGizmosManager_hpp_
