#ifndef slic3r_GLGizmoBrimEars_hpp_
#define slic3r_GLGizmoBrimEars_hpp_

#include "GLGizmoBase.hpp"
#include "slic3r/GUI/GLSelectionRectangle.hpp"
#include "libslic3r/BrimEarsPoint.hpp"
#include "libslic3r/ObjectID.hpp"


namespace Slic3r {

class ConfigOption;

namespace GUI {

enum class SLAGizmoEventType : unsigned char;

class GLGizmoBrimEars : public GLGizmoBase
{
private:
    using PickRaycaster = SceneRaycasterItem;

    bool unproject_on_mesh(const Vec2d& mouse_pos, std::pair<Vec3f, Vec3f>& pos_and_normal);
    bool unproject_on_mesh2(const Vec2d& mouse_pos, std::pair<Vec3f, Vec3f>& pos_and_normal);

    const float RenderPointScale = 1.f;

    class CacheEntry {
    public:
        CacheEntry() :
            brim_point(BrimPoint()),
            selected(false),
            normal(Vec3f(0, 0, 1)),
            is_hover(false),
            is_error(false)
        {}

        CacheEntry(const BrimPoint &point, bool sel = false, const Vec3f &norm = Vec3f(0, 0, 1), bool hover = false, bool error = false)
            : brim_point(point), selected(sel), normal(norm), is_hover(hover), is_error(error)
        {}

        bool operator==(const CacheEntry& rhs) const {
            return (brim_point == rhs.brim_point);
        }

        bool operator!=(const CacheEntry& rhs) const {
            return ! ((*this) == rhs);
        }

        inline bool pos_is_zero() {
            return brim_point.pos.isZero();
        }

        void set_empty() {
            brim_point = BrimPoint();
            selected = false;
            normal.setZero();
            is_hover = false;
            is_error = false;
        }

        BrimPoint brim_point;
        bool selected; // whether the point is selected
        bool is_hover; // show mouse hover cylinder
        bool is_error;
        Vec3f normal;

        template<class Archive>
        void serialize(Archive & ar)
        {
            ar(brim_point, selected, normal);
        }
    };

public:
    GLGizmoBrimEars(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);
    virtual ~GLGizmoBrimEars() = default;
    void data_changed(bool is_serializing) override;
    void set_brim_data();
    bool on_mouse(const wxMouseEvent& mouse_event) override;
    bool gizmo_event(SLAGizmoEventType action, const Vec2d& mouse_position, bool shift_down, bool alt_down, bool control_down);
    void delete_selected_points();
    void update_model_object();
    //ClippingPlane get_sla_clipping_plane() const;

    bool is_selection_rectangle_dragging() const { return m_selection_rectangle.is_dragging(); }

    bool wants_enter_leave_snapshots() const override { return true; }
    std::string get_gizmo_entering_text() const override { return "Entering Brim Ears"; }
    std::string get_gizmo_leaving_text() const override { return "Leaving Brim Ears"; }

private:
    bool on_init() override;
    void on_dragging(const UpdateData& data) override;
    void on_render() override;

    void render_points(const Selection& selection);

    float m_new_point_head_diameter;        // Size of a new point.
    float m_max_angle = 125.f;
    float m_detection_radius = 1.f;
    double m_detection_radius_max = .0f;
    CacheEntry m_point_before_drag;         // undo/redo - so we know what state was edited
    float m_old_point_head_diameter = 0.;   // the same
    mutable std::vector<CacheEntry> m_editing_cache; // a support point and whether it is currently selectedchanges or undo/redo
    std::map<int, CacheEntry> m_single_brim;
    ObjectID m_old_mo_id;
    const Vec3d m_world_normal = {0, 0, 1};
    std::map<GLVolume*, std::shared_ptr<PickRaycaster>>   m_mesh_raycaster_map;
    GLVolume* m_last_hit_volume;
    std::optional<CacheEntry> render_hover_point;

    bool m_link_text_hover = false;
    
    PickingModel m_cylinder;

    // This map holds all translated description texts, so they can be easily referenced during layout calculations
    // etc. When language changes, GUI is recreated and this class constructed again, so the change takes effect.
    std::map<std::string, wxString> m_desc;

    GLSelectionRectangle m_selection_rectangle;

    ExPolygons m_first_layer;

    bool m_wait_for_up_event = false;
    bool m_selection_empty = true;
    EState m_old_state = Off; // to be able to see that the gizmo has just been closed (see on_set_state)

    std::vector<const ConfigOption*> get_config_options(const std::vector<std::string>& keys) const;
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
    Points generate_points(Polygon &obj_polygon, float ear_detection_length, float brim_ears_max_angle, bool is_outer);
    void auto_generate();
    void first_layer_slicer();
    void get_detection_radius_max();
    void update_raycasters();

    void begin_radius_change(float initial_value);
    void update_cache_radius();
    void apply_radius_change();

protected:
    void on_set_state() override;
    void on_set_hover_id() override

    {
        if ((int)m_editing_cache.size() <= m_hover_id)
            m_hover_id = -1;
    }
    void on_start_dragging() override;
    void on_stop_dragging() override;
    void on_render_input_window(float x, float y, float bottom_limit) override;
    void show_tooltip_information(float x, float y);

    std::string on_get_name() const override;
    bool on_is_activable() const override;
    //bool on_is_selectable() const override;
    virtual CommonGizmosDataID on_get_requirements() const override;
    void on_load(cereal::BinaryInputArchive& ar) override;
    void on_save(cereal::BinaryOutputArchive& ar) const override;
    virtual void on_register_raycasters_for_picking() override;
    virtual void on_unregister_raycasters_for_picking() override;
    void register_single_mesh_pick();
    //void update_single_mesh_pick(GLVolume* v);
    void reset_all_pick();
    bool add_point_to_cache(Vec3f pos, float head_radius, bool selected, Vec3f normal);
    float get_brim_default_radius() const;
    ExPolygon make_polygon(BrimPoint point, const Geometry::Transformation &trsf);
    void find_single();
};

wxDECLARE_EVENT(wxEVT_THREAD_DONE, wxCommandEvent);

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoBrimEars_hpp_
