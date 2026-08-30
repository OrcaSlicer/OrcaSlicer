#ifndef slic3r_GLGizmoRotate2_hpp_
#define slic3r_GLGizmoRotate2_hpp_

#include "GLGizmoBase.hpp"
#include "GLGizmoRotate.hpp"

namespace Slic3r {
namespace GUI {

enum class SLAGizmoEventType : unsigned char;

class GLGizmoRotate2 : public GLGizmoBase
{
private:
    bool m_precise{false};
    bool m_snap{false};

    double m_angle{ 0.0 };
    Vec3d m_center{ Vec3d::Zero() };
    Transform3d m_orient_matrix{ Transform3d::Identity() };

    Vec2d m_center_ss; // Gizmo center in screen space
    Vec2d m_mouse_curr_pos{INT_MAX, INT_MAX};

    // Where the mouse is clicked on the ring when dragging is started
    float m_drag_angle_start{ 0.f };

    float m_old_axis_line_length = 0;
    float m_old_angle{ 0.0f };

    ColorRGBA m_axis_color;
    ColorRGBA m_highlight_color;

    GLModel m_ring;
    GLModel m_circle;
    GLModel m_axis_line;
    GLModel m_reference_radius;
    GLModel m_angle_arc;
    static GLModel s_snap_radii;
    static GLModel s_snap_radii_fine;

    // 0 - half ring, 1 - full ring
    std::array<std::shared_ptr<SceneRaycasterItem>, 2> m_raycasters = {nullptr};

    static PickingModel s_torus;
    static PickingModel s_torus_half;

    void init_data_from_selection(const Selection& selection);
    Transform3d local_transform() const;
    // Update m_center_ss — object center in screen space
    void update_center_ss();

    void render_ring(const ColorRGBA& color, bool full);
    void render_circle(const ColorRGBA& color);
    void render_radius(const ColorRGBA& color);
    void render_angle_arc(const ColorRGBA& color);
    void render_snap_radii(const ColorRGBA& color);
    void render_angle_number();

protected:
    bool on_init() override;
    std::string on_get_name() const override { return ""; }
    void on_start_dragging() override;
    void on_dragging(const UpdateData &data) override;
    void on_render() override;
    virtual void on_register_raycasters_for_picking() override;
    virtual void on_unregister_raycasters_for_picking() override;

public:
    GLGizmoRotate::Axis m_axis;
    bool m_enabled { true };
    bool m_show_center { true };

    GLGizmoRotate2(GLCanvas3D& parent, GLGizmoRotate::Axis axis);
    virtual ~GLGizmoRotate2() = default;

    std::string get_tooltip() const override;
    
    bool on_mouse(const wxMouseEvent &mouse_event) override;
    void start_dragging();
    void stop_dragging();
    void dragging(const UpdateData &data);
    
    double get_angle() const;
    void set_angle(double angle);

    void set_snap(bool enabled);
    void set_precise(bool enabled);
};

class GLGizmoRotate3D2 : public GLGizmoBase
{
private:
    std::array<GLGizmoRotate2, 3> m_gizmos;
 
    //BBS: add size adjust related
    GizmoObjectManipulation* m_object_manipulation;

    const GLVolume *m_last_volume;
    
protected:
    bool on_init() override;
    std::string on_get_name() const override;
    void on_set_state() override;
    void on_set_hover_id() override
    {
        for (int i = 0; i < 3; ++i)
            m_gizmos[i].set_hover_id((m_hover_id == i) ? 0 : -1);
    }
    bool on_is_activable() const override;
    void on_start_dragging() override;
    void on_stop_dragging() override;
    void on_dragging(const UpdateData &data) override;

    void on_render() override;
    virtual void on_register_raycasters_for_picking() override;
    virtual void on_unregister_raycasters_for_picking() override;

    void on_render_input_window(float x, float y, float bottom_limit) override;

public:
    GLGizmoRotate3D2(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id, GizmoObjectManipulation* obj_manipulation);

    bool on_mouse(const wxMouseEvent &mouse_event) override;
    bool gizmo_event(SLAGizmoEventType action, const Vec2d& mouse_position, bool shift_down, bool alt_down, bool control_down);
    
    void data_changed(bool is_serializing) override;

    std::string get_tooltip() const override;

    Vec3d get_rotation() const { return Vec3d(m_gizmos[X].get_angle(), m_gizmos[Y].get_angle(), m_gizmos[Z].get_angle()); }
    void set_rotation(const Vec3d& rotation) { m_gizmos[X].set_angle(rotation.x()); m_gizmos[Y].set_angle(rotation.y()); m_gizmos[Z].set_angle(rotation.z()); }
};


} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoRotate2_hpp_
