#ifndef slic3r_GLGizmoHollow_hpp_
#define slic3r_GLGizmoHollow_hpp_

#include "GLGizmoBase.hpp"
#include "slic3r/GUI/GLSelectionRectangle.hpp"

#include <libslic3r/SLA/Hollowing.hpp>
#include <libslic3r/ObjectID.hpp>
#include <wx/dialog.h>

#include <cereal/types/vector.hpp>


namespace Slic3r {

class ConfigOption;
class ConfigOptionDef;

namespace GUI {

enum class SLAGizmoEventType : unsigned char;

class GLGizmoHollow : public GLGizmoBase
{
private:
    bool unproject_on_mesh(const Vec2d& mouse_pos, std::pair<Vec3f, Vec3f>& pos_and_normal);


public:
    GLGizmoHollow(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);
    virtual ~GLGizmoHollow() = default;
    void set_sla_support_data(ModelObject* model_object, const Selection& selection);
    bool gizmo_event(SLAGizmoEventType action, const Vec2d& mouse_position, bool shift_down, bool alt_down, bool control_down);
    void delete_selected_points();    
    bool is_selection_rectangle_dragging() const {
        return m_selection_rectangle.is_dragging();
    }

private:
    bool on_init() override;
    void on_update(const UpdateData& data) override;
    void on_render() override;

    void render_points(const Selection& selection, bool picking = false);
    void hollow_mesh(bool postpone_error_messages = false);
    bool unsaved_changes() const;

    ObjectID m_old_mo_id = -1;

    GLModel m_cylinder;

    float m_new_hole_radius = 2.f;        // Size of a new hole.
    float m_new_hole_height = 6.f;
    mutable std::vector<bool> m_selected; // which holes are currently selected

    bool m_enable_hollowing = true;

    // Stashes to keep data for undo redo. Is taken after the editing
    // is done, the data are updated continuously.
    float m_offset_stash = 3.0f;
    float m_quality_stash = 0.5f;
    float m_closing_d_stash = 2.f;
    Vec3f m_hole_before_drag = Vec3f::Zero();
    sla::DrainHoles m_holes_in_drilled_mesh;

    sla::DrainHoles m_holes_stash;
    
    // This map holds all translated description texts, so they can be easily referenced during layout calculations
    // etc. When language changes, GUI is recreated and this class constructed again, so the change takes effect.
    std::map<std::string, wxString> m_desc;

    GLSelectionRectangle m_selection_rectangle;

    bool m_wait_for_up_event = false;
    bool m_selection_empty = true;
    EState m_old_state = Off; // to be able to see that the gizmo has just been closed (see on_set_state)

    std::vector<std::pair<const ConfigOption*, const ConfigOptionDef*>> get_config_options(const std::vector<std::string>& keys) const;
    bool is_mesh_point_clipped(const Vec3d& point) const;

    // Methods that do the model_object and editing cache synchronization,
    // editing mode selection, etc:
    enum {
        AllPoints = -2,
        NoPoints,
    };
    void select_point(int i);
    void unselect_point(int i);
    void reload_cache();

protected:
    void on_set_state() override;
    void on_set_hover_id() override;
    void on_start_dragging() override;
    void on_stop_dragging() override;
    void on_render_input_window(float x, float y, float bottom_limit) override;
    virtual CommonGizmosDataID on_get_requirements() const override;

    std::string on_get_name() const override;
    bool on_is_activable() const override;
    bool on_is_selectable() const override;
    void on_load(cereal::BinaryInputArchive& ar) override;
    void on_save(cereal::BinaryOutputArchive& ar) const override;
};



} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoHollow_hpp_
