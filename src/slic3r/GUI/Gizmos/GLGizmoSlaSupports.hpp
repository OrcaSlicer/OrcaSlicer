#ifndef slic3r_GLGizmoSlaSupports_hpp_
#define slic3r_GLGizmoSlaSupports_hpp_

#include "GLGizmoBase.hpp"
#include "slic3r/GUI/GLSelectionRectangle.hpp"

#include "libslic3r/SLA/SupportPoint.hpp"
#include "libslic3r/ObjectID.hpp"
#include <wx/dialog.h>

#include <cereal/types/vector.hpp>


namespace Slic3r {

class ConfigOption;

namespace GUI {

enum class SLAGizmoEventType : unsigned char;

class GLGizmoSlaSupports : public GLGizmoBase
{
private:

    bool unproject_on_mesh(const Vec2d& mouse_pos, std::pair<Vec3f, Vec3f>& pos_and_normal);

    const float RenderPointScale = 1.f;

    class CacheEntry {
    public:
        CacheEntry() :
            support_point(sla::SupportPoint()), selected(false), normal(Vec3f::Zero()) {}

        CacheEntry(const sla::SupportPoint& point, bool sel = false, const Vec3f& norm = Vec3f::Zero()) :
            support_point(point), selected(sel), normal(norm) {}

        bool operator==(const CacheEntry& rhs) const {
            return (support_point == rhs.support_point);
        }

        bool operator!=(const CacheEntry& rhs) const {
            return ! ((*this) == rhs);
        }

        sla::SupportPoint support_point;
        bool selected; // whether the point is selected
        Vec3f normal;

        template<class Archive>
        void serialize(Archive & ar)
        {
            ar(support_point, selected, normal);
        }
    };

public:
    GLGizmoSlaSupports(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);
    virtual ~GLGizmoSlaSupports() = default;
    void set_sla_support_data(ModelObject* model_object, const Selection& selection);
    bool gizmo_event(SLAGizmoEventType action, const Vec2d& mouse_position, bool shift_down, bool alt_down, bool control_down);
    void delete_selected_points(bool force = false);
    //ClippingPlane get_sla_clipping_plane() const;

    bool is_in_editing_mode() const { return m_editing_mode; }
    bool is_selection_rectangle_dragging() const { return m_selection_rectangle.is_dragging(); }
    bool has_backend_supports() const;
    void reslice_SLA_supports(bool postpone_error_messages = false) const;

    bool wants_enter_leave_snapshots() const override { return true; }
    std::string get_gizmo_entering_text() const override { return "Entering SLA support points"; }
    std::string get_gizmo_leaving_text() const override { return "Leaving SLA support points"; }

private:
    bool on_init() override;
    void on_update(const UpdateData& data) override;
    void on_render() override;

    void render_points(const Selection& selection, bool picking = false);
    bool unsaved_changes() const;

    bool m_lock_unique_islands = false;
    bool m_editing_mode = false;            // Is editing mode active?
    float m_new_point_head_diameter;        // Size of a new point.
    CacheEntry m_point_before_drag;         // undo/redo - so we know what state was edited
    float m_old_point_head_diameter = 0.;   // the same
    float m_minimal_point_distance_stash = 0.f; // and again
    float m_density_stash = 0.f;                // and again
    mutable std::vector<CacheEntry> m_editing_cache; // a support point and whether it is currently selected
    std::vector<sla::SupportPoint> m_normal_cache; // to restore after discarding changes or undo/redo
    ObjectID m_old_mo_id;

    GLModel m_cone;
    GLModel m_cylinder;
    GLModel m_sphere;

    // This map holds all translated description texts, so they can be easily referenced during layout calculations
    // etc. When language changes, GUI is recreated and this class constructed again, so the change takes effect.
    std::map<std::string, wxString> m_desc;

    GLSelectionRectangle m_selection_rectangle;

    bool m_wait_for_up_event = false;
    bool m_selection_empty = true;
    EState m_old_state = Off; // to be able to see that the gizmo has just been closed (see on_set_state)

    std::vector<const ConfigOption*> get_config_options(const std::vector<std::string>& keys) const;
    bool is_mesh_point_clipped(const Vec3d& point) const;
    bool is_point_in_hole(const Vec3f& pt) const;
    //void find_intersecting_facets(const igl::AABB<Eigen::MatrixXf, 3>* aabb, const Vec3f& normal, double offset, std::vector<unsigned int>& out) const;

    // Methods that do the model_object and editing cache synchronization,
    // editing mode selection, etc:
    enum {
        AllPoints = -2,
        NoPoints,
    };
    void select_point(int i);
    void unselect_point(int i);
    void editing_mode_apply_changes();
    void editing_mode_discard_changes();
    void reload_cache();
    void get_data_from_backend();
    void auto_generate();
    void switch_to_editing_mode();
    void disable_editing_mode();
    void ask_about_changes_call_after(std::function<void()> on_yes, std::function<void()> on_no);

protected:
    void on_set_state() override;
    void on_set_hover_id() override

    {
        if (! m_editing_mode || (int)m_editing_cache.size() <= m_hover_id)
            m_hover_id = -1;
    }
    void on_start_dragging() override;
    void on_stop_dragging() override;
    void on_render_input_window(float x, float y, float bottom_limit) override;

    std::string on_get_name() const override;
    bool on_is_activable() const override;
    bool on_is_selectable() const override;
    virtual CommonGizmosDataID on_get_requirements() const override;
    void on_load(cereal::BinaryInputArchive& ar) override;
    void on_save(cereal::BinaryOutputArchive& ar) const override;
};


class SlaGizmoHelpDialog : public wxDialog
{
public:
    SlaGizmoHelpDialog();
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoSlaSupports_hpp_
