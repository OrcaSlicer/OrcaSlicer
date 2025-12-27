#ifndef slic3r_GLGizmoSeam_hpp_
#define slic3r_GLGizmoSeam_hpp_

#include "GLGizmoPainterBase.hpp"

namespace Slic3r::GUI {

class GLGizmoSeam : public GLGizmoPainterBase
{
public:
    GLGizmoSeam(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);

    void render_painter_gizmo() override;

    //BBS
    bool on_key_down_select_tool_type(int keyCode);

protected:
    // BBS
    void on_set_state() override;

    wchar_t  m_current_tool = 0;
    void on_render_input_window(float x, float y, float bottom_limit) override;
    std::string on_get_name() const override;
    PainterGizmoType get_painter_type() const override;

    void show_tooltip_information(float caption_max, float x, float y);

    void tool_changed(wchar_t old_tool, wchar_t new_tool);

    wxString handle_snapshot_action_name(bool shift_down, Button button_down) const override;

    std::string get_gizmo_entering_text() const override { return _u8L("Entering Seam painting"); }
    std::string get_gizmo_leaving_text() const override { return _u8L("Leaving Seam painting"); }
    std::string get_action_snapshot_name() const override { return _u8L("Paint-on seam editing"); }
    static const constexpr float      CursorRadiusMin = 0.05f; // cannot be zero

    const float get_cursor_radius_min() const override { return CursorRadiusMin; }

private:
    bool on_init() override;

    //BBS:remove const
    void update_model_object() override;
    //BBS: add logic to distinguish the first_time_update and later_update
    void update_from_model_object(bool first_update = false) override;

    void on_opening() override {}
    void on_shutdown() override;

    // This map holds all translated description texts, so they can be easily referenced during layout calculations
    // etc. When language changes, GUI is recreated and this class constructed again, so the change takes effect.
    std::map<std::string, wxString> m_desc;
};



} // namespace Slic3r::GUI


#endif // slic3r_GLGizmoSeam_hpp_
