#ifndef slic3r_GLGizmoAdvancedCut_hpp_
#define slic3r_GLGizmoAdvancedCut_hpp_

#include "GLGizmoBase.hpp"
#include "GLGizmoRotate.hpp"
#include "libslic3r/Model.hpp"

namespace Slic3r {
enum class CutConnectorType : int;
class ModelVolume;
struct CutConnectorAttributes;

namespace GUI {
enum class SLAGizmoEventType : unsigned char;

class GLGizmoAdvancedCut : public GLGizmoRotate3D
{
struct Rotate_data {
    double  angle;
    Axis    ax;

    Rotate_data(double an, Axis a)
        : angle(an), ax(a)
    {
    }
};
private:
    static const double Offset;
    static const double Margin;
    static const ColorRGBA GrabberColor;
    static const ColorRGBA GrabberHoverColor;

    mutable double m_movement;
    mutable double m_height;  // height of cut plane to heatbed
    mutable double m_height_delta;  // height of cut plane to heatbed
    double m_start_movement;
    double m_start_height;

    Vec3d m_rotation;
    //Vec3d m_current_base_rotation;
    std::vector<Rotate_data> m_rotate_cmds;

    Vec3d m_buffered_rotation;
    double m_buffered_movement;
    double m_buffered_height;

    Vec3d m_drag_pos;

    bool m_keep_upper;
    bool m_keep_lower;
    bool m_cut_to_parts;
    bool m_place_on_cut_upper{true};
    bool m_place_on_cut_lower{false};
    bool m_rotate_upper{false};
    bool m_rotate_lower{false};
    GLModel m_plane;
    GLModel m_grabber_connection;
    GLModel m_cut_line;

    bool m_do_segment;
    double m_segment_smoothing_alpha;
    int m_segment_number;

    std::array<Vec3d, 4> m_cut_plane_points;

    mutable Grabber m_move_grabber;

    unsigned int m_last_active_id;

    bool m_connectors_editing{false};
    bool m_show_shortcuts{false};

    std::vector<std::pair<wxString, wxString>> m_shortcuts;
    double m_label_width{150.0};
    double m_control_width{ 200.0 };
    double m_editing_window_width;

    CutConnectorType         m_connector_type;
    size_t                   m_connector_style;
    size_t                   m_connector_shape_id;

    float m_connector_depth_ratio{3.f};
    float m_connector_depth_ratio_tolerance{0.1f};

    float m_connector_size{2.5f};
    float m_connector_size_tolerance{0.f};

    TriangleMesh m_connector_mesh;
    bool         m_has_invalid_connector{false};

    // remember the connectors which is selected
    mutable std::vector<bool> m_selected;
    int                       m_selected_count{0};

    Vec3d m_cut_plane_center{Vec3d::Zero()};
    Vec3d m_cut_plane_normal{Vec3d::UnitZ()};

    Vec3d m_cut_line_begin{Vec3d::Zero()};
    Vec3d m_cut_line_end{Vec3d::Zero()};

    Transform3d m_rotate_matrix{Transform3d::Identity()};

    std::map<CutConnectorAttributes, GLModel> m_shapes;

    struct InvalidConnectorsStatistics
    {
        unsigned int outside_cut_contour;
        unsigned int outside_bb;
        bool         is_overlap;

        void invalidate()
        {
            outside_cut_contour = 0;
            outside_bb          = 0;
            is_overlap          = false;
        }
    } m_info_stats;

    //GLSelectionRectangle m_selection_rectangle;

public:
    GLGizmoAdvancedCut(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);

    bool gizmo_event(SLAGizmoEventType action, const Vec2d &mouse_position, bool shift_down, bool alt_down, bool control_down);
    bool on_key(wxKeyEvent &evt);

    double get_movement() const { return m_movement; }
    void set_movement(double movement) const;
    void finish_rotation();
    std::string get_tooltip() const override;

    BoundingBoxf3 bounding_box() const;
    //BoundingBoxf3 transformed_bounding_box(const Vec3d &plane_center, bool revert_move = false) const;

    bool is_looking_forward() const;

    bool unproject_on_cut_plane(const Vec2d &mouse_pos, Vec3d &pos, Vec3d &pos_world);
    
    virtual bool apply_clipping_plane() { return m_connectors_editing; }

    void data_changed(bool is_serializing) override;

protected:
    bool on_init() override;
    void on_load(cereal::BinaryInputArchive &ar) override;
    void on_save(cereal::BinaryOutputArchive &ar) const override;
    std::string on_get_name() const override;
    void on_set_state() override;
    bool on_is_activable() const override;
    CommonGizmosDataID on_get_requirements() const override;
    void on_start_dragging() override;
    void on_stop_dragging() override;
    void on_dragging(const UpdateData& data) override;
    void on_render() override;
    virtual void on_render_input_window(float x, float y, float bottom_limit);

    void show_tooltip_information(float x, float y);

private:
    void perform_cut(const Selection& selection);
    bool can_perform_cut() const;
    void apply_connectors_in_model(ModelObject *mo, bool &create_dowels_as_separate_object);

    bool is_selection_changed(bool alt_down, bool shift_down);
    void select_connector(int idx, bool select);

    double calc_projection(const Linef3& mouse_ray) const;
    Vec3d calc_plane_normal(const std::array<Vec3d, 4>& plane_points) const;
    Vec3d calc_plane_center(const std::array<Vec3d, 4>& plane_points) const;
    Vec3d get_plane_normal() const;
    Vec3d get_plane_center() const;
    void update_plane_points();
    std::array<Vec3d, 4> get_plane_points() const;
    std::array<Vec3d, 4> get_plane_points_world_coord() const;
    void reset_cut_plane();
    void reset_all();

    // update the connectors position so that the connectors are on the cut plane
    void put_connectors_on_cut_plane(const Vec3d &cp_normal, double cp_offset);
    void update_clipper();
    // on render
    void render_cut_plane_and_grabbers();
    void render_connectors();
    void render_clipper_cut();
    void render_cut_line();
    void render_connector_model(GLModel &model, const ColorRGBA& color, Transform3d model_matrix, bool for_picking = false);

    void clear_selection();
    void init_connector_shapes();
    void set_connectors_editing(bool connectors_editing);
    void reset_connectors();
    void update_connector_shape();
    void apply_selected_connectors(std::function<void(size_t idx)> apply_fn);
    void select_all_connectors();
    void unselect_all_connectors();
    void validate_connector_settings();
    bool add_connector(CutConnectors &connectors, const Vec2d &mouse_position);
    bool delete_selected_connectors();
    bool is_outside_of_cut_contour(size_t idx, const CutConnectors &connectors, const Vec3d cur_pos);
    bool is_conflict_for_connector(size_t idx, const CutConnectors &connectors, const Vec3d cur_pos);
    void check_conflict_for_all_connectors();

    // render input window
    void render_cut_plane_input_window(float x, float y, float bottom_limit);
    void init_connectors_input_window_data();
    void render_connectors_input_window(float x, float y, float bottom_limit);
    void render_input_window_warning() const;
    bool render_reset_button(const std::string &label_id, const std::string &tooltip) const;
    bool render_connect_type_radio_button(CutConnectorType type);

    bool render_combo(const std::string &label, const std::vector<std::string> &lines, size_t &selection_idx);
    bool render_slider_double_input(const std::string &label, float &value_in, float &tolerance_in);

    bool cut_line_processing() const;
    void discard_cut_line_processing();
    bool process_cut_line(SLAGizmoEventType action, const Vec2d &mouse_position);
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoAdvancedCut_hpp_