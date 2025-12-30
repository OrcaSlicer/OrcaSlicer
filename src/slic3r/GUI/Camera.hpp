#ifndef slic3r_Camera_hpp_
#define slic3r_Camera_hpp_

#include "libslic3r/BoundingBox.hpp"
#include "3DScene.hpp"
#include <array>

namespace Slic3r {
namespace GUI {

#define REQUIRES_ZOOM_TO_CUR_PLATE      -1
#define REQUIRES_ZOOM_TO_ALL_PLATE      -2
#define REQUIRES_ZOOM_TO_PLATE_IDLE     -100


struct Camera
{
    static const double DefaultDistance;
    static const double DefaultZoomToBoxMarginFactor;
    static const double DefaultZoomToVolumesMarginFactor;
    static double FrustrumMinZRange;
    static double FrustrumMinNearZ;
    static double FrustrumZMargin;
    static double MaxFovDeg;
    static double ZoomUnit;

    enum class EType : unsigned char
    {
        Unknown,
        Ortho,
        Perspective,
        Num_types
    };
    enum class ViewAngleType : unsigned char {
        Iso = 0,
        Top_Front,
        Left,
        Right,
        Top,
        Bottom,
        Front,
        Rear,
        Count_ViewAngleType,
        Top_Plate,//for 3mf and Skip parts
    };
    bool requires_zoom_to_bed{ false };
    //BBS
    bool requires_zoom_to_volumes{ false };
    int  requires_zoom_to_plate{ REQUIRES_ZOOM_TO_PLATE_IDLE };

private:
    bool m_prevent_auto_type = true;
    EType m_type{ EType::Perspective };
    bool m_update_config_on_type_change_enabled{ false };
    Vec3d m_target{ Vec3d::Zero() };
    float m_zenit{ 45.0f };
    double m_zoom{ 1.0 };
    // Distance between camera position and camera target measured along the camera Z axis
    double m_distance{ DefaultDistance };
    double m_gui_scale{ 1.0 };

    std::array<int, 4> m_viewport;
    Transform3d m_view_matrix{ Transform3d::Identity() };
    // We are calculating the rotation part of the m_view_matrix from m_view_rotation.
    Eigen::Quaterniond m_view_rotation{ 1.0, 0.0, 0.0, 0.0 };
    Transform3d m_projection_matrix{ Transform3d::Identity() };
    std::pair<double, double> m_frustrum_zs;

    BoundingBoxf3 m_scene_box;

public:
    Camera() { set_default_orientation(); }

    EType get_type() const { return m_type; }
    std::string get_type_as_string() const;
    void set_type(EType type);
    // valid values for type: "false" -> ortho, "true" -> perspective
    void set_type(const std::string& type) { set_type((type == "true") ? EType::Perspective : EType::Ortho); }
    void select_next_type();
    void auto_type(EType preferred_type);

    void enable_update_config_on_type_change(bool enable) { m_update_config_on_type_change_enabled = enable; }

    void translate(const Vec3d& displacement);
    const Vec3d& get_target()  { 
        update_target();
        return m_target; }
    void set_target(const Vec3d& target);

    double get_distance()  { return (get_position() - get_target()).norm(); }
    double get_gui_scale() const { return m_gui_scale; }
    float  get_zenit() const { return m_zenit; }

    double get_zoom() const { return m_zoom; }
    double get_inv_zoom() const { assert(m_zoom != 0.0); return 1.0 / m_zoom; }
    void update_zoom(double delta_zoom) { set_zoom(m_zoom / (1.0 - std::max(std::min(delta_zoom, 4.0), -4.0) * ZoomUnit)); }
    void set_zoom(double zoom);

    const BoundingBoxf3& get_scene_box() const { return m_scene_box; }
    void set_scene_box(const BoundingBoxf3& box) { m_scene_box = box; }


    void select_view(const std::string& direction);
    void select_view(ViewAngleType type);
    const std::array<int, 4>& get_viewport() const { return m_viewport; }
    const Transform3d& get_view_matrix() const { return m_view_matrix; }
    const Transform3d& get_projection_matrix() const { return m_projection_matrix; }

    //BBS
    const Eigen::Quaterniond& get_view_rotation() const {return m_view_rotation; }

    Vec3d get_dir_right() const { return m_view_matrix.matrix().block(0, 0, 3, 3).row(0); }
    Vec3d get_dir_up() const { return m_view_matrix.matrix().block(0, 0, 3, 3).row(1); }
    Vec3d get_dir_forward() const { return -m_view_matrix.matrix().block(0, 0, 3, 3).row(2); }


    Vec3d get_position() const { return m_view_matrix.matrix().inverse().block(0, 3, 3, 1); }

    double get_near_z() const { return m_frustrum_zs.first; }
    double get_far_z() const { return m_frustrum_zs.second; }
    const std::pair<double, double>& get_z_range() const { return m_frustrum_zs; }

    double get_near_left() const;
    double get_near_right() const;
    double get_near_top() const;
    double get_near_bottom() const;
    double get_near_width() const;
    double get_near_height() const;

    double get_fov() const;

    void set_viewport(int x, int y, unsigned int w, unsigned int h);
    void apply_viewport() const;
    // Calculates and applies the projection matrix tighting the frustrum z range around the given box.
    // If larger z span is needed, pass the desired values of near and far z (negative values are ignored)
    void apply_projection(const BoundingBoxf3& box, double near_z = -1.0, double far_z = -1.0);

    void apply_projection(double left, double right, double bottom, double top, double near_z, double far_z);

    void zoom_to_box(const BoundingBoxf3& box, double margin_factor = DefaultZoomToBoxMarginFactor);
    void zoom_to_volumes(const GLVolumePtrs& volumes, double margin_factor = DefaultZoomToVolumesMarginFactor);

#if ENABLE_CAMERA_STATISTICS
    void debug_render() const;
#endif // ENABLE_CAMERA_STATISTICS

    // translate the camera in world space
    void translate_world(const Vec3d& displacement) { set_target(m_target + displacement); }

    // BBS rotate the camera on a sphere having center == target
    void rotate_on_sphere_with_target(double delta_azimut_rad, double delta_zenit_rad, bool apply_limits, Vec3d target);
    void rotate_local_with_target(const Vec3d& rotation_rad, Vec3d target);

    // rotate the camera on a sphere having center == m_target and radius == m_distance
    // using the given variations of spherical coordinates
    // if apply_limits == true the camera stops rotating when its forward vector is parallel to the world Z axis
    void rotate_on_sphere(double delta_azimut_rad, double delta_zenit_rad, bool apply_limits);

    // rotate the camera around three axes parallel to the camera local axes and passing through m_target
    void rotate_local_around_target(const Vec3d& rotation_rad);

    void set_rotation(const Transform3d& rotation);

    // returns true if the camera z axis (forward) is pointing in the negative direction of the world z axis
    bool is_looking_downward() const { return get_dir_forward().dot(Vec3d::UnitZ()) < 0.0; }
    bool is_looking_front() const { return abs(get_dir_up().dot(Vec3d::UnitZ())-1) < 0.001; }
    // forces camera right vector to be parallel to XY plane
    void recover_from_free_camera() {
        if (std::abs(get_dir_right()(2)) > EPSILON)
            look_at(get_position(), m_target, Vec3d::UnitZ());
    }

    //BBS store and load camera view
    void load_camera_view(Camera& cam);

    void look_at(const Vec3d& position, const Vec3d& target, const Vec3d& up);

    double max_zoom() const { return 250.0; }
    double min_zoom() const { return 0.2 * calc_zoom_to_bounding_box_factor(m_scene_box); }

private:
    // returns tight values for nearZ and farZ plane around the given bounding box
    // the camera MUST be outside of the bounding box in eye coordinate of the given box
    std::pair<double, double> calc_tight_frustrum_zs_around(const BoundingBoxf3& box);
    double calc_zoom_to_bounding_box_factor(const BoundingBoxf3& box, double margin_factor = DefaultZoomToBoxMarginFactor) const;
    double calc_zoom_to_volumes_factor(const GLVolumePtrs& volumes, Vec3d& center, double margin_factor = DefaultZoomToVolumesMarginFactor) const;
    void set_distance(double distance);

    void set_default_orientation();
    Vec3d validate_target(const Vec3d& target) const;
    void update_zenit();
    void update_target();
};

} // GUI
} // Slic3r

#endif // slic3r_Camera_hpp_

