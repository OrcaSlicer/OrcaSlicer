#ifndef slic3r_GLGizmoFuzzySkin_hpp_
#define slic3r_GLGizmoFuzzySkin_hpp_

#include "GLGizmoPainterBase.hpp"

#include "slic3r/GUI/I18N.hpp"

namespace Slic3r::GUI {

class GLGizmoFuzzySkin : public GLGizmoPainterBase
{
public:
    GLGizmoFuzzySkin(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);

    void render_painter_gizmo() override;

protected:
    void        on_render_input_window(float x, float y, float bottom_limit) override;
    std::string on_get_name() const override;

    void show_tooltip_information(float caption_max, float x, float y);

    wxString handle_snapshot_action_name(bool shift_down, Button button_down) const override;

    std::string get_gizmo_entering_text() const override { return _u8L("Entering Paint-on fuzzy skin"); }
    std::string get_gizmo_leaving_text() const override { return _u8L("Leaving Paint-on fuzzy skin"); }
    std::string get_action_snapshot_name() const override { return _u8L("Paint-on fuzzy skin editing"); }

    EnforcerBlockerType get_left_button_state_type() const override { return EnforcerBlockerType::FUZZY_SKIN; }
    EnforcerBlockerType get_right_button_state_type() const override { return EnforcerBlockerType::NONE; }

    // BBS
    wchar_t                           m_current_tool = 0;

private:
    bool on_init() override;

    void update_model_object() override;
    void update_from_model_object(bool first_update) override;

    void             on_opening() override {}
    void             on_shutdown() override;
    PainterGizmoType get_painter_type() const override;

    // This map holds all translated description texts, so they can be easily referenced during layout calculations
    // etc. When language changes, GUI is recreated, and this class constructed again, so the change takes effect.
    std::map<std::string, wxString> m_desc;
};

} // namespace Slic3r::GUI

#endif // slic3r_GLGizmoFuzzySkin_hpp_
