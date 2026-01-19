#ifndef slic3r_GLGizmoAssembly_hpp_
#define slic3r_GLGizmoAssembly_hpp_

#include "GLGizmoMeasure.hpp"

namespace Slic3r {

namespace GUI {
class GLGizmoAssembly : public GLGizmoMeasure
{

public:
    GLGizmoAssembly(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);
    /// <summary>
    /// Apply rotation on select plane
    /// </summary>
    /// <param name="mouse_event">Keep information about mouse click</param>
    /// <returns>Return True when use the information otherwise False.</returns>
    //bool on_mouse(const wxMouseEvent &mouse_event) override;
    //void data_changed(bool is_serializing) override;
    //bool gizmo_event(SLAGizmoEventType action, const Vec2d &mouse_position, bool shift_down, bool alt_down, bool control_down) override;

    bool wants_enter_leave_snapshots() const override { return true; }
    std::string get_gizmo_entering_text() const override { return _u8L("Entering Assembly gizmo"); }
    std::string get_gizmo_leaving_text() const override { return _u8L("Leaving Assembly gizmo"); }
protected:
    bool on_init() override;
    std::string on_get_name() const override;
    bool on_is_activable() const override;
    //void on_render() override;
    //void on_set_state() override;
    virtual void on_render_input_window(float x, float y, float bottom_limit) override;

    void render_input_window_warning(bool same_model_object) override;
    bool render_assembly_mode_combo(double label_width, float item_width);

    void switch_to_mode(AssemblyMode new_mode);
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoAssembly_hpp_
