#ifndef slic3r_GLGizmoCut_hpp_
#define slic3r_GLGizmoCut_hpp_

#include "GLGizmoBase.hpp"
#include "slic3r/GUI/GLSelectionRectangle.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/CutUtils.hpp"
#include "imgui/imgui.h"

namespace Slic3r {

enum class CutConnectorType : int;
class ModelVolume;
class GLShaderProgram;
struct CutConnectorAttributes;

namespace GUI {
class Selection;

enum class SLAGizmoEventType : unsigned char;

namespace CommonGizmosDataObjects { class ObjectClipper; }

class GLGizmoCut3D : public GLGizmoBase
{
    enum GrabberID {
        X = 0,
        Y,
        Z,
        CutPlane,
        CutPlaneZRotation,
        CutPlaneXMove,
        CutPlaneYMove,
        Count,
    };

    Transform3d                 m_rotation_m{ Transform3d::Identity() };
    double                      m_snap_step{ 1.0 };
    int                         m_connectors_group_id;

    // archived values 
    Vec3d m_ar_plane_center { Vec3d::Zero() };
    Transform3d m_start_dragging_m{ Transform3d::Identity() };

    Vec3d m_plane_center{ Vec3d::Zero() };
    // data to check position of the cut palne center on gizmo activation
    Vec3d m_min_pos{ Vec3d::Zero() };
    Vec3d m_max_pos{ Vec3d::Zero() };
    Vec3d m_bb_center{ Vec3d::Zero() };
    Vec3d m_center_offset{ Vec3d::Zero() };

    BoundingBoxf3 m_bounding_box;
    BoundingBoxf3 m_transformed_bounding_box;

    // values from RotationGizmo
    double m_radius{ 0.0 };
    double m_grabber_radius{ 0.0 };
    double m_grabber_connection_len{ 0.0 };
    Vec3d  m_cut_plane_start_move_pos {Vec3d::Zero()};

    double m_snap_coarse_in_radius{ 0.0 };
    double m_snap_coarse_out_radius{ 0.0 };
    double m_snap_fine_in_radius{ 0.0 };
    double m_snap_fine_out_radius{ 0.0 };

    // dragging angel in hovered axes
    double m_angle{ 0.0 };

    TriangleMesh    m_connector_mesh;
    // workaround for using of the clipping plane normal
    Vec3d           m_clp_normal{ Vec3d::Ones() };

    Vec3d           m_line_beg{ Vec3d::Zero() };
    Vec3d           m_line_end{ Vec3d::Zero() };

    Vec2d           m_ldown_mouse_position{ Vec2d::Zero() };

    GLModel m_grabber_connection;
    GLModel m_cut_line;

    PickingModel m_plane;
    PickingModel m_sphere;
    PickingModel m_cone;
    PickingModel m_cube;
    std::map<CutConnectorAttributes, PickingModel> m_shapes;
    std::vector<std::shared_ptr<SceneRaycasterItem>> m_raycasters;

    GLModel m_circle;
    GLModel m_scale;
    GLModel m_snap_radii;
    GLModel m_reference_radius;
    GLModel m_angle_arc;

    Vec3d   m_old_center;
    Vec3d   m_cut_normal;

    struct InvalidConnectorsStatistics
    {
        unsigned int    outside_cut_contour;
        unsigned int    outside_bb;
        bool            is_overlap;

        void invalidate() {
            outside_cut_contour = 0;
            outside_bb = 0;
            is_overlap = false;
        } 
    } m_info_stats;

    bool m_keep_upper{ true };
    bool m_keep_lower{ true };
    bool m_keep_as_parts{ false };
    bool m_place_on_cut_upper{ true };
    bool m_place_on_cut_lower{ false };
    bool m_rotate_upper{ false };
    bool m_rotate_lower{ false };

    // Input params for cut with tongue and groove
    Cut::Groove m_groove;
    bool m_groove_editing { false };

    bool m_is_slider_editing_done { false };

    // Input params for cut with snaps
    float m_snap_bulge_proportion{ 0.15f };
    float m_snap_space_proportion{ 0.3f };

    bool m_hide_cut_plane{ false };
    bool m_connectors_editing{ false };
    bool m_cut_plane_as_circle{ false };

    float m_connector_depth_ratio{ 3.f };
    float m_connector_size{ 2.5f };
    float m_connector_angle{ 0.f };

    float m_connector_depth_ratio_tolerance{ 0.1f };
    float m_connector_size_tolerance{ 0.f };

    float m_label_width{ 0.f };
    float m_control_width{ 200.f };
    double m_editing_window_width;
    bool  m_imperial_units{ false };

    float m_contour_width{ 0.4f };
    float m_cut_plane_radius_koef{ 1.5f };

    mutable std::vector<bool> m_selected; // which pins are currently selected
    int  m_selected_count{ 0 };

    GLSelectionRectangle m_selection_rectangle;

    std::vector<size_t> m_invalid_connectors_idxs;
    bool m_was_cut_plane_dragged { false };
    bool m_was_contour_selected { false };

    // Vertices of the groove used to detection if groove is valid
    std::vector<Vec3d> m_groove_vertices;

    class PartSelection {
    public:
        PartSelection() = default;
        PartSelection(const ModelObject* mo, const Transform3d& cut_matrix, int instance_idx, const Vec3d& center, const Vec3d& normal, const CommonGizmosDataObjects::ObjectClipper& oc);
        PartSelection(const ModelObject* mo, int instance_idx_in);
        ~PartSelection() { m_model.clear_objects(); }

        struct Part {
            GLModel glmodel;
            MeshRaycaster raycaster;
            bool selected;
            bool is_modifier;
        };

        void render(const Vec3d* normal, GLModel& sphere_model);
        void toggle_selection(const Vec2d& mouse_pos);
        void turn_over_selection();
        ModelObject* model_object() { return m_model.objects.front(); }
        bool valid() const { return m_valid; }
        bool is_one_object() const;
        const std::vector<Part>& parts() const { return m_parts; }
        const std::vector<size_t>* get_ignored_contours_ptr() const { return (valid() ? &m_ignored_contours : nullptr); }

        std::vector<Cut::Part> get_cut_parts();

    private:
        Model m_model;
        int m_instance_idx;
        std::vector<Part> m_parts;
        bool m_valid = false;
        std::vector<std::pair<std::vector<size_t>, std::vector<size_t>>> m_contour_to_parts; // for each contour, there is a vector of parts above and a vector of parts below
        std::vector<size_t> m_ignored_contours; // contour that should not be rendered (the parts on both sides will both be parts of the same object)

        std::vector<Vec3d> m_contour_points;         // Debugging
        std::vector<std::vector<Vec3d>> m_debug_pts; // Debugging

        void add_object(const ModelObject* object);
    };

    PartSelection m_part_selection;

    std::vector<std::pair<wxString, wxString>> m_shortcuts_cut;
    std::vector<std::pair<wxString, wxString>> m_shortcuts_connector;

    enum class CutMode {
        cutPlanar
        , cutTongueAndGroove
        //, cutGrig
        //,cutRadial
        //,cutModular
    };

    enum class CutConnectorMode {
        Auto
        , Manual
    };

    std::vector<std::string> m_modes;
    size_t m_mode{ size_t(CutMode::cutPlanar) };

    std::vector<std::string> m_connector_modes;
    CutConnectorMode m_connector_mode{ CutConnectorMode::Manual };

    std::vector<std::string> m_connector_types;
    CutConnectorType m_connector_type;

    std::vector<std::string> m_connector_styles;
    int m_connector_style;

    std::vector<std::string> m_connector_shapes;
    int m_connector_shape_id;

    std::vector<std::string> m_axis_names;

    std::map<std::string, wxString> m_part_orientation_names;

    std::map<std::string, std::string> m_labels_map;

public:
    GLGizmoCut3D(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);

    std::string get_tooltip() const override;
    bool unproject_on_cut_plane(const Vec2d& mouse_pos, Vec3d& pos, Vec3d& pos_world, bool respect_contours = true);
    bool gizmo_event(SLAGizmoEventType action, const Vec2d& mouse_position, bool shift_down, bool alt_down, bool control_down);

    bool is_in_editing_mode() const override { return m_connectors_editing; }
    bool is_selection_rectangle_dragging() const override { return m_selection_rectangle.is_dragging(); }
    bool is_looking_forward() const;

    /// <summary>
    /// Drag of plane
    /// </summary>
    /// <param name="mouse_event">Keep information about mouse click</param>
    /// <returns>Return True when use the information otherwise False.</returns>
    bool on_mouse(const wxMouseEvent &mouse_event) override;

    void shift_cut(double delta);
    void rotate_vec3d_around_plane_center(Vec3d&vec);
    void put_connectors_on_cut_plane(const Vec3d& cp_normal, double cp_offset);
    void update_clipper();
    void invalidate_cut_plane();

    BoundingBoxf3   bounding_box() const;
    BoundingBoxf3   transformed_bounding_box(const Vec3d& plane_center, const Transform3d& rotation_m = Transform3d::Identity()) const;

protected:
    bool               on_init() override;
    void               on_load(cereal::BinaryInputArchive&ar) override;
    void               on_save(cereal::BinaryOutputArchive&ar) const override;
    std::string        on_get_name() const override;
    void               on_set_state() override;
    CommonGizmosDataID on_get_requirements() const override;
    void               on_set_hover_id() override;
    bool               on_is_activable() const override;
    bool               on_is_selectable() const override;
    Vec3d              mouse_position_in_local_plane(GrabberID axis, const Linef3&mouse_ray) const;
    void               dragging_grabber_move(const GLGizmoBase::UpdateData &data);
    void               dragging_grabber_rotation(const GLGizmoBase::UpdateData &data);
    void               dragging_connector(const GLGizmoBase::UpdateData &data);
    void               on_dragging(const UpdateData&data) override;
    void               on_start_dragging() override;
    void               on_stop_dragging() override;
    void               on_render() override;

    void render_debug_input_window(float x);
    void unselect_all_connectors();
    void select_all_connectors();
    void apply_selected_connectors(std::function<void(size_t idx)> apply_fn);
    void render_connectors_input_window(CutConnectors &connectors, float x, float y, float bottom_limit);
    void render_build_size();
    void reset_cut_plane();
    void set_connectors_editing(bool connectors_editing);
    void flip_cut_plane();
    void process_contours();
    void reset_cut_by_contours();
    void render_flip_plane_button(bool disable_pred = false);
    void add_vertical_scaled_interval(float interval);
    void add_horizontal_scaled_interval(float interval);
    void add_horizontal_shift(float shift);
    void render_color_marker(float size, const ImU32& color);
    void render_groove_float_input(const std::string &label, float &in_val, const float &init_val, float &in_tolerance);
    void render_groove_angle_input(const std::string &label, float &in_val, const float &init_val, float min_val, float max_val);
    bool render_angle_input(const std::string& label, float& in_val, const float& init_val, float min_val, float max_val);
    void render_snap_specific_input(const std::string& label, const wxString& tooltip, float& in_val, const float& init_val, const float min_val, const float max_val);
    void render_cut_plane_input_window(CutConnectors &connectors, float x, float y, float bottom_limit);
    void init_input_window_data(CutConnectors &connectors);
    void render_input_window_warning() const;
    bool add_connector(CutConnectors&connectors, const Vec2d&mouse_position);
    bool delete_selected_connectors(CutConnectors&connectors);
    void select_connector(int idx, bool select);
    bool is_selection_changed(bool alt_down, bool shift_down);
    void process_selection_rectangle(CutConnectors &connectors);

    virtual void on_register_raycasters_for_picking() override;
    virtual void on_unregister_raycasters_for_picking() override;
    void update_raycasters_for_picking();
    void set_volumes_picking_state(bool state);
    void update_raycasters_for_picking_transform();

    void update_plane_model();

    void on_render_input_window(float x, float y, float bottom_limit) override;
    void show_tooltip_information(float x, float y);

    bool wants_enter_leave_snapshots() const override       { return true; }
    std::string get_gizmo_entering_text() const override    { return _u8L("Entering Cut gizmo"); }
    std::string get_gizmo_leaving_text() const override     { return _u8L("Leaving Cut gizmo"); }
    std::string get_action_snapshot_name() const override   { return _u8L("Cut gizmo editing"); }

    void data_changed(bool is_serializing) override; 
    Transform3d get_cut_matrix(const Selection& selection);

private:
    void set_center(const Vec3d&center, bool update_tbb = false);
    void switch_to_mode(size_t new_mode);
    bool render_cut_mode_combo();
    bool render_double_input(const std::string& label, double& value_in);
    bool render_slider_double_input(const std::string& label, float& value_in, float& tolerance_in, float min_val = -0.1f, float max_tolerance = -0.1f);
    void render_move_center_input(int axis);
    void render_connect_mode_radio_button(CutConnectorMode mode);
    bool render_reset_button(const std::string& label_id, const std::string& tooltip) const;
    bool render_connect_type_radio_button(CutConnectorType type);
    bool is_outside_of_cut_contour(size_t idx, const CutConnectors& connectors, const Vec3d cur_pos);
    bool is_conflict_for_connector(size_t idx, const CutConnectors& connectors, const Vec3d cur_pos);
    void render_connectors();

    bool can_perform_cut() const;
    bool has_valid_groove() const;
    bool has_valid_contour() const;
    void apply_connectors_in_model(ModelObject* mo, int &dowels_count);
    bool cut_line_processing() const;
    void discard_cut_line_processing();

    void apply_color_clip_plane_colors();
    void render_cut_plane();
    static void render_model(GLModel& model, const ColorRGBA& color, Transform3d view_model_matrix);
    void render_line(GLModel& line_model, const ColorRGBA& color, Transform3d view_model_matrix, float width);
    void render_rotation_snapping(GrabberID axis, const ColorRGBA& color);
    void render_grabber_connection(const ColorRGBA& color, Transform3d view_matrix, double line_len_koef = 1.0);
    void render_cut_plane_grabbers();
    void render_cut_line();
    void perform_cut(const Selection&selection);
    void set_center_pos(const Vec3d&center_pos, bool update_tbb = false);
    void update_bb();
    void init_picking_models();
    void init_rendering_items();
    void render_clipper_cut();
    void clear_selection();
    void reset_connectors();
    void init_connector_shapes();
    void update_connector_shape();
    void validate_connector_settings();
    bool process_cut_line(SLAGizmoEventType action, const Vec2d& mouse_position);
    void check_and_update_connectors_state();

    void toggle_model_objects_visibility();

    indexed_triangle_set its_make_groove_plane();

    indexed_triangle_set get_connector_mesh(CutConnectorAttributes connector_attributes);
    void apply_cut_connectors(ModelObject* mo, const std::string& connector_name);
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoCut_hpp_
