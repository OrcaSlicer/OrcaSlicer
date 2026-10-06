#pragma once

#include "GLGizmoPainterBase.hpp"

#include "libslic3r/ObjectID.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "slic3r/GUI/I18N.hpp"

#include <map>
#include <string>
#include <utility>
#include <vector>

#include <wx/string.h>

namespace Slic3r {
class ModelObject;
class ModelVolume;
} // namespace Slic3r

namespace Slic3r::GUI {

class GLCanvas3D;

// Paints the shape of one painted modifier on its host part, see docs/HLSD/painted-modifiers.md.
class GLGizmoPaintedModifier : public GLGizmoPainterBase
{
public:
    GLGizmoPaintedModifier(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id);

    void render_painter_gizmo() override;

    // The painted modifier receiving the strokes. Without one, the next stroke creates it on the part painted.
    void set_target(const ModelVolume *painted_modifier);

protected:
    void        on_render_input_window(float x, float y, float bottom_limit) override;
    std::string on_get_name() const override;

    wxString handle_snapshot_action_name(bool shift_down, Button button_down) const override;

    std::string get_gizmo_entering_text() const override { return _u8L("Entering painted modifier"); }
    std::string get_gizmo_leaving_text() const override { return _u8L("Leaving painted modifier"); }
    std::string get_action_snapshot_name() const override { return _u8L("Painted modifier editing"); }

    EnforcerBlockerType get_left_button_state_type() const override { return EnforcerBlockerType::ENFORCER; }
    EnforcerBlockerType get_right_button_state_type() const override { return EnforcerBlockerType::NONE; }
    bool                is_mesh_paintable(int mesh_id) const override;

private:
    bool on_init() override;
    bool on_is_selectable() const override { return false; }

    void update_model_object() override;
    void update_from_model_object(bool first_update) override;

    void             on_opening() override {}
    void             on_shutdown() override;
    PainterGizmoType get_painter_type() const override { return PainterGizmoType::PAINTED_MODIFIER; }

    ModelVolume *target(const ModelObject &mo) const;
    void         render_tooltip_button(float x, float y);

    ObjectID m_target_id;
    // Index of the target's host among the model parts, -1 when the target has no host.
    int      m_target_mesh_id { -1 };
    float    m_depth { 0.f };
    wchar_t  m_current_tool { 0 };

    std::map<std::string, wxString>            m_desc;
    std::vector<std::pair<wxString, wxString>> m_shortcuts_brush;
    std::vector<std::pair<wxString, wxString>> m_shortcuts_triangle;
    std::vector<std::pair<wxString, wxString>> m_shortcuts_smart_fill;
};

} // namespace Slic3r::GUI
