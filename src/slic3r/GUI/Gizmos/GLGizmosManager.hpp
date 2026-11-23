#ifndef slic3r_GUI_GLGizmosManager_hpp_
#define slic3r_GUI_GLGizmosManager_hpp_

#include "slic3r/GUI/GLTexture.hpp"
#include "slic3r/GUI/GLToolbar.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoBase.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosCommon.hpp"
//BBS: GUI refactor: add object manipulation
#include "slic3r/GUI/Gizmos/GizmoObjectManipulation.hpp"

#include "libslic3r/ObjectID.hpp"

#include <cstdlib>
#include "libslic3r/libslic3r.h"
#include <vector>
#include <memory>
#include <utility>
#include <cstddef>
#include "libslic3r/Point.hpp"
#include <string>
#include <wx/event.h>
#include <cereal/specialize.hpp>
#include <wx/timer.h>
#include <map>
#include <optional>

//BBS: GUI refactor: to support top layout
#define BBS_TOOLBAR_ON_TOP 1

namespace Slic3r {

namespace UndoRedo {
struct Snapshot;
}

namespace GUI {

class GLCanvas3D;
class ClippingPlane;
struct ToolbarGroupLayout;
struct ToolbarBarSlot;
struct ToolbarIconGroup;
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
        PaintedModifier,
        TextureDisplacement,
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
    // The gizmos the user shows on the bar, in their saved order.
    std::vector<EType> m_bar_types;
    // The user's gizmo groups, the open group's list, and the gizmo ids opened this session, newest first.
    std::vector<ToolbarIconGroup> m_groups;
    mutable GroupPopupState m_group_popup;
    // The group whose button the tooltip names, or -1.
    int m_tooltip_group{-1};
    std::vector<std::string> m_recent_gizmos;
    // Written while the list is drawn: the member clicked in it (opened after drawing).
    mutable EType m_group_pending_open{Undefined};

    //BBS: GUI refactor: add object manipulation
    GizmoObjectManipulation m_object_manipulation;

    std::vector<size_t> get_selectable_idxs() const;
    // The Prepare canvas's bar slots; each group with two or more icons on the bar collapses into one slot.
    std::vector<ToolbarBarSlot> get_bar_slots() const;
    // The group whose slot shows this gizmo, or -1 (always -1 on the Assemble canvas).
    int group_of_slot(EType type) const;
    int group_of_slot(EType type, const std::vector<ToolbarBarSlot>& slots) const;
    void render_group_list(int group, float left, float top) const;
    EType get_gizmo_from_mouse(const Vec2d &mouse_pos) const;

    bool activate_gizmo(EType type);

    std::string m_tooltip;
    bool m_serializing;
    std::unique_ptr<CommonGizmosDataPool> m_common_gizmos_data;

    //When there are more than 9 colors, shortcut key coloring
    wxTimer m_timer_set_color;
    // Leading digit of a two-digit color shortcut still waiting for its second digit.
    int m_pending_color_shortcut_tens = 0;
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

    enum MENU_ICON_NAME {
        IC_TOOLBAR_RESET            = 0,
        IC_TOOLBAR_RESET_HOVER,
        IC_TOOLBAR_RESET_ZERO,
        IC_TOOLBAR_RESET_ZERO_HOVER,
        IC_TOOLBAR_TOOLTIP,
        IC_TOOLBAR_WIKI_GUIDE,
        IC_TOOLBAR_VIDEO_GUIDE,
        IC_NAME_COUNT,
        IC_CANVAS_MENU,
        IC_CANVAS_MENU_HOVER,
        IC_CANVAS_MENU_DARK,
        IC_CANVAS_MENU_DARK_HOVER,
        IC_CANVAS_ZOOM,
        IC_CANVAS_ZOOM_HOVER,
        IC_CANVAS_ZOOM_DARK,
        IC_CANVAS_ZOOM_DARK_HOVER,
        IC_CANVAS_SECTION,
        IC_CANVAS_SECTION_HOVER,
        IC_CANVAS_SECTION_DARK,
        IC_CANVAS_SECTION_DARK_HOVER,
        IC_CANVAS_SECTION_ACTIVE,
        IC_CANVAS_SECTION_ACTIVE_HOVER,
        IC_CANVAS_SECTION_ACTIVE_DARK,
        IC_CANVAS_SECTION_ACTIVE_DARK_HOVER,
        IC_ALIGN_X_MIN,
        IC_ALIGN_X_CENTER,
        IC_ALIGN_X_MAX,
        IC_ALIGN_Y_MIN,
        IC_ALIGN_Y_CENTER,
        IC_ALIGN_Y_MAX,
        IC_ALIGN_Z_MIN,
        IC_ALIGN_Z_CENTER,
        IC_ALIGN_Z_MAX,
        IC_DISTRIBUTE_X,
        IC_DISTRIBUTE_Y,
        IC_DISTRIBUTE_Z,
        IC_ALIGN_X_MIN_DARK,
        IC_ALIGN_X_CENTER_DARK,
        IC_ALIGN_X_MAX_DARK,
        IC_ALIGN_Y_MIN_DARK,
        IC_ALIGN_Y_CENTER_DARK,
        IC_ALIGN_Y_MAX_DARK,
        IC_ALIGN_Z_MIN_DARK,
        IC_ALIGN_Z_CENTER_DARK,
        IC_ALIGN_Z_MAX_DARK,
        IC_DISTRIBUTE_X_DARK,
        IC_DISTRIBUTE_Y_DARK,
        IC_DISTRIBUTE_Z_DARK,
    };

    explicit GLGizmosManager(GLCanvas3D& parent);
    ~GLGizmosManager();

    void switch_gizmos_icon_filename();

    bool init();

    // Applies the user's gizmo bar order, hidden set and groups; ids that name no gizmo are ignored.
    void set_bar_layout(const ToolbarGroupLayout& layout, const std::vector<ToolbarIconGroup>& groups);
    void close_group_list();
    // Closes the open group list unless the mouse is on it or on its group's button.
    void close_group_list_unless_at(const Vec2d& mouse_pos);
    // The stock rule only (not the user's choice): true when the gizmos are not created yet.
    bool is_bar_item_available(EType type) const;
    static std::string gizmo_id(EType type);
    // The gizmo with that toolbar layout id, or Undefined.
    static EType gizmo_type_of(const std::string& id);

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
    // ORCA
    bool is_mouse_over_toolbar_icon(const Vec2d& mouse_pos) const { return get_gizmo_from_mouse(mouse_pos) != Undefined; }
    // A group's list is open; no gizmo is then, as opening either closes the other.
    bool is_group_list_open() const { return m_group_popup.is_open(); }
    void set_enabled(bool enable) { m_enabled = enable; }

    void set_icon_dirty() { m_icons_texture_dirty = true; }
    void set_overlay_icon_size(float size);
    void set_overlay_scale(float scale);

    void refresh_on_off_state();
    void reset_all_states();
    bool open_gizmo(EType type);
    // What a left click on a gizmo's toolbar icon did.
    enum class ToolbarClick { Nothing, Opened, Closed, CreationStarted };
    // Does what that click does: the Text gizmo starts creating a text part when no text is
    // selected (the creation job opens the gizmo), the others open or close.
    ToolbarClick click_toolbar_icon(EType type);
    bool check_gizmos_closed_except(EType) const;

    void set_hover_id(int id);

    /// <summary>
    /// Distribute information about different data into active gizmo
    /// Should be called when selection changed
    /// </summary>
    void update_data();
    // Passes the canvas' section view to the object clipper of the painting and brim ears gizmos.
    void update_section_view();

    EType get_current_type() const { return m_current; }
    GLGizmoBase* get_current() const;
    GLGizmoBase *get_gizmo(GLGizmosManager::EType type) const;
    EType get_gizmo_from_name(const std::string& gizmo_name) const;

    bool is_running() const;
    // Whether this canvas's gizmo toolbar shows an icon for `type`.
    bool is_on_toolbar(EType type) const;
    // Whether any gizmo still works in the background, see GLGizmoBase::is_busy().
    bool is_busy() const;
    // Opens the gizmo bound to a Plater-context shortcut; false when no gizmo has it or it cannot open now.
    bool open_gizmo_by_shortcut(Shortcut shortcut);
    // Lets the current gizmo consume the delete key; false when it did not.
    bool on_delete_key();

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
    bool is_allow_multi_select_parts_or_objects() const;
    // Empty when the open gizmo has no object clipper.
    std::optional<ClippingPlane> get_clipping_plane() const;
    bool wants_reslice_supports_on_undo() const;

    bool is_in_editing_mode(bool error_notification = false) const;
    bool is_hiding_instances() const;

    void on_change_color_mode(bool is_dark);
    void render_current_gizmo() const;
    void render_painter_gizmo();

    void render_overlay();
    void render_overlay_input_window();
    // Hash of the state render_overlay() draws from: enabled, hover, current and highlight.
    size_t get_overlay_state_hash() const;

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
    
    void do_render_overlay(bool draw_icons) const;

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
