#ifndef slic3r_GLGizmoBase_hpp_
#define slic3r_GLGizmoBase_hpp_

#include "libslic3r/Point.hpp"
#include "libslic3r/Color.hpp"

#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "slic3r/GUI/MeshUtils.hpp"
#include "slic3r/GUI/SceneRaycaster.hpp"
#include "slic3r/GUI/3DScene.hpp"

#include <cereal/archives/binary.hpp>

#include <wx/event.h>

#define ENABLE_FIXED_GRABBER 1

class wxWindow;

namespace Slic3r {

class BoundingBoxf3;
class Linef3;
class ModelObject;

namespace GUI {



class ImGuiWrapper;
class GLCanvas3D;
enum class CommonGizmosDataID;
class CommonGizmosDataPool;
class Selection;

class GLGizmoBase
{
public:
    // Starting value for ids to avoid clashing with ids used by GLVolumes
    // (254 is choosen to leave some space for forward compatibility)
    static const unsigned int BASE_ID = 255 * 255 * 254;
    static const unsigned int GRABBER_ELEMENTS_MAX_COUNT = 7;

    static float INV_ZOOM;

    //BBS colors
    static ColorRGBA DEFAULT_BASE_COLOR;
    static ColorRGBA DEFAULT_DRAG_COLOR;
    static ColorRGBA DEFAULT_HIGHLIGHT_COLOR;
    static std::array<ColorRGBA, 3> AXES_COLOR;
    static std::array<ColorRGBA, 3> AXES_HOVER_COLOR;
    static ColorRGBA CONSTRAINED_COLOR;
    static ColorRGBA FLATTEN_COLOR;
    static ColorRGBA FLATTEN_HOVER_COLOR;
    static ColorRGBA GRABBER_NORMAL_COL;
    static ColorRGBA GRABBER_HOVER_COL;
    static ColorRGBA GRABBER_UNIFORM_COL;
    static ColorRGBA GRABBER_UNIFORM_HOVER_COL;

    static void update_render_colors();
    static void load_render_colors();

    enum class EGrabberExtension
    {
        None = 0,
        PosX = 1 << 0,
        NegX = 1 << 1,
        PosY = 1 << 2,
        NegY = 1 << 3,
        PosZ = 1 << 4,
        NegZ = 1 << 5,
    };

    // Represents NO key(button on keyboard) value
    static const int NO_SHORTCUT_KEY_VALUE = 0;

protected:
    struct Grabber
    {
        static const float SizeFactor;
        static const float MinHalfSize;
        static const float DraggingScaleFactor;
        static const float FixedGrabberSize;
        static const float FixedRadiusSize;

        bool enabled{ true };
        bool dragging{ false };
        Vec3d center{ Vec3d::Zero() };
        Vec3d angles{ Vec3d::Zero() };
        Transform3d matrix{ Transform3d::Identity() };
        ColorRGBA color{GRABBER_NORMAL_COL};
        ColorRGBA hover_color{GRABBER_HOVER_COL};
        EGrabberExtension extensions{ EGrabberExtension::None };
        // the picking id shared by all the elements
        int picking_id{ -1 };
        std::array<std::shared_ptr<SceneRaycasterItem>, GRABBER_ELEMENTS_MAX_COUNT> raycasters = { nullptr };

        Grabber() = default;
        ~Grabber();

        void render(bool hover, float size) { render(size, hover ? hover_color : color); }

        float get_half_size(float size) const;
        float get_dragging_half_size(float size) const;
        PickingModel &get_cube();

        void register_raycasters_for_picking(int id);
        void unregister_raycasters_for_picking();

    private:
        void render(float size, const ColorRGBA& render_color);

        static PickingModel s_cube;
        static PickingModel s_cone;
    };

public:
    enum EState
    {
        Off,
        On,
        Num_States
    };

    struct UpdateData
    {
        const Linef3& mouse_ray;
        const Point& mouse_pos;

        UpdateData(const Linef3& mouse_ray, const Point& mouse_pos)
            : mouse_ray(mouse_ray), mouse_pos(mouse_pos)
        {}
    };

protected:
    GLCanvas3D& m_parent;

    int m_group_id; // TODO: remove only for rotate
    EState m_state;
    int m_shortcut_key;
    std::string m_icon_filename;
    unsigned int m_sprite_id;
    int m_hover_id{ -1 };
    bool m_dragging{ false };
    mutable std::vector<Grabber> m_grabbers;
    ImGuiWrapper* m_imgui;
    bool m_first_input_window_render{ true };
    CommonGizmosDataPool* m_c{ nullptr };

    bool m_is_dark_mode = false;

    bool render_combo(const std::string &label, const std::vector<std::string> &lines,
        int &selection_idx, float label_width, float item_width);
    void render_cross_mark(const Vec3f& target,bool is_single =false);
public:
    GLGizmoBase(GLCanvas3D& parent,
                const std::string& icon_filename,
                unsigned int sprite_id);
    virtual ~GLGizmoBase() = default;

    bool init() { return on_init(); }

    void load(cereal::BinaryInputArchive& ar) { m_state = On; on_load(ar); }
    void save(cereal::BinaryOutputArchive& ar) const { on_save(ar); }

    std::string get_name(bool include_shortcut = true) const;

    EState get_state() const { return m_state; }
    void set_state(EState state) { m_state = state; on_set_state(); }

    int get_shortcut_key() const { return m_shortcut_key; }

    const std::string& get_icon_filename() const { return m_icon_filename; }

    void set_icon_filename(const std::string& filename);

    bool is_activable() const { return on_is_activable(); }
    bool is_selectable() const { return on_is_selectable(); }
    CommonGizmosDataID get_requirements() const { return on_get_requirements(); }
    virtual bool wants_enter_leave_snapshots() const { return false; }
    virtual std::string get_gizmo_entering_text() const { assert(false); return ""; }
    virtual std::string get_gizmo_leaving_text() const { assert(false); return ""; }
    virtual std::string get_action_snapshot_name() const;
    void set_common_data_pool(CommonGizmosDataPool* ptr) { m_c = ptr; }

    virtual bool apply_clipping_plane() { return true; }

    /// <summary>
    /// Implement when want to process mouse events in gizmo
    /// Click, Right click, move, drag, ...
    /// </summary>
    /// <param name="mouse_event">Keep information about mouse click</param>
    /// <returns>Return True when use the information and don't want to propagate it otherwise False.</returns>
    virtual bool on_mouse(const wxMouseEvent &mouse_event) { return false; }
    unsigned int get_sprite_id() const { return m_sprite_id; }

    int get_hover_id() const { return m_hover_id; }
    void set_hover_id(int id);
    
    bool is_dragging() const { return m_dragging; }

    // returns True when Gizmo changed its state
    bool update_items_state();

    void render() { on_render(); }
    void render_input_window(float x, float y, float bottom_limit);
    virtual void on_change_color_mode(bool is_dark) {  m_is_dark_mode = is_dark; }

    /// <summary>
    /// Mouse tooltip text
    /// </summary>
    /// <returns>Text to be visible in mouse tooltip</returns>
    virtual std::string get_tooltip() const { return ""; }

    int get_count() { return ++count; }
    std::string get_gizmo_name() { return on_get_name(); }

    /// <summary>
    /// Is called when data (Selection) is changed
    /// </summary>
    virtual void data_changed(bool is_serializing){};

    void register_raycasters_for_picking()   { register_grabbers_for_picking(); on_register_raycasters_for_picking(); }
    void unregister_raycasters_for_picking() { unregister_grabbers_for_picking(); on_unregister_raycasters_for_picking(); }

    virtual bool is_in_editing_mode() const { return false; }
    virtual bool is_selection_rectangle_dragging() const { return false; }

protected:
    float last_input_window_width = 0;
    virtual bool on_init() = 0;
    virtual void on_load(cereal::BinaryInputArchive& ar) {}
    virtual void on_save(cereal::BinaryOutputArchive& ar) const {}
    virtual std::string on_get_name() const = 0;
    virtual void on_set_state() {}
    virtual void on_set_hover_id() {}
    virtual bool on_is_activable() const { return true; }
    virtual bool on_is_selectable() const { return true; }
    virtual CommonGizmosDataID on_get_requirements() const { return CommonGizmosDataID(0); }
    virtual void on_enable_grabber(unsigned int id) {}
    virtual void on_disable_grabber(unsigned int id) {}
       
    // called inside use_grabbers
    virtual void on_start_dragging() {}
    virtual void on_stop_dragging() {}
    virtual void on_dragging(const UpdateData& data) {}

    virtual void on_render() = 0;
    virtual void on_render_input_window(float x, float y, float bottom_limit) {}

    bool GizmoImguiBegin(const std::string& name, int flags);
    void GizmoImguiEnd();
    void GizmoImguiSetNextWIndowPos(float &x, float y, int flag, float pivot_x = 0.0f, float pivot_y = 0.0f);
    void GizmoImguiSetNextWIndowPos(float &x, float y, float w, float h, int flag, float pivot_x = 0.0f, float pivot_y = 0.0f);

    void register_grabbers_for_picking();
    void unregister_grabbers_for_picking();
    virtual void on_register_raycasters_for_picking() {}
    virtual void on_unregister_raycasters_for_picking() {}

    void render_grabbers(const BoundingBoxf3& box) const;
    void render_grabbers(float size) const;
    void render_grabbers(size_t first, size_t last, float size, bool force_hover) const;

    std::string format(float value, unsigned int decimals) const;

    // Mark gizmo as dirty to Re-Render when idle()
    void set_dirty();

    /// <summary>
    /// function which 
    /// Set up m_dragging and call functions
    /// on_start_dragging / on_dragging / on_stop_dragging
    /// </summary>
    /// <param name="mouse_event">Keep information about mouse click</param>
    /// <returns>same as on_mouse</returns>
    bool use_grabbers(const wxMouseEvent &mouse_event);

    void do_stop_dragging(bool perform_mouse_cleanup);

private:
    // Flag for dirty visible state of Gizmo
    // When True then need new rendering
    bool m_dirty{ false };
    int count = 0;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoBase_hpp_
