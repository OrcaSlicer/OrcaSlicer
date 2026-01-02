#ifndef slic3r_GLGizmoMeasure_hpp_
#define slic3r_GLGizmoMeasure_hpp_

#include "GLGizmoBase.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/GUI/MeshUtils.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "libslic3r/Measure.hpp"
#include "libslic3r/Model.hpp"

namespace Slic3r {

enum class ModelVolumeType : int;
namespace Measure { class Measuring; }
namespace GUI {

enum class SLAGizmoEventType : unsigned char;
enum class EMeasureMode : unsigned char {
    ONLY_MEASURE,
    ONLY_ASSEMBLY
};
enum class AssemblyMode : unsigned char {
    FACE_FACE,
    POINT_POINT,
};
static const Slic3r::ColorRGBA SELECTED_1ST_COLOR = {0   / 255.f, 112 / 255.f, 210 / 255.f, 1.f};
static const Slic3r::ColorRGBA SELECTED_2ND_COLOR = {15  / 255.f, 173 / 255.f, 78  / 255.f, 1.f};
static const Slic3r::ColorRGBA NEUTRAL_COLOR      = {0.5f, 0.5f, 0.5f, 1.0f};
static const Slic3r::ColorRGBA HOVER_COLOR        = ColorRGBA::GREEN();

static const int POINT_ID        = 100;
static const int EDGE_ID         = 200;
static const int CIRCLE_ID       = 300;
static const int PLANE_ID        = 400;
static const int SEL_SPHERE_1_ID = 501;
static const int SEL_SPHERE_2_ID = 502;

static const float TRIANGLE_BASE   = 10.0f;
static const float TRIANGLE_HEIGHT = TRIANGLE_BASE * 1.618033f;

static const std::string CTRL_STR =
#ifdef __APPLE__
    "⌘"
#else
    "Ctrl"
#endif //__APPLE__
    ;

class TransformHelper
{
    struct Cache
    {
        std::array<int, 4> viewport;
        Matrix4d           ndc_to_ss_matrix;
        Transform3d        ndc_to_ss_matrix_inverse;
    };
    static Cache s_cache;

public:
    static Vec3d             model_to_world(const Vec3d &model, const Transform3d &world_matrix);
    static Vec4d             world_to_clip(const Vec3d &world, const Matrix4d &projection_view_matrix);
    static Vec3d             clip_to_ndc(const Vec4d &clip);
    static Vec2d             ndc_to_ss(const Vec3d &ndc, const std::array<int, 4> &viewport);
    static Vec4d             model_to_clip(const Vec3d &model, const Transform3d &world_matrix, const Matrix4d &projection_view_matrix);
    static Vec3d             model_to_ndc(const Vec3d &model, const Transform3d &world_matrix, const Matrix4d &projection_view_matrix);
    static Vec2d             model_to_ss(const Vec3d &model, const Transform3d &world_matrix, const Matrix4d &projection_view_matrix, const std::array<int, 4> &viewport);
    static Vec2d             world_to_ss(const Vec3d &world, const Matrix4d &projection_view_matrix, const std::array<int, 4> &viewport);
    static const Matrix4d &  ndc_to_ss_matrix(const std::array<int, 4> &viewport);
    static const Transform3d ndc_to_ss_matrix_inverse(const std::array<int, 4> &viewport);

private:
    static void update(const std::array<int, 4> &viewport);
};

class GLGizmoMeasure : public GLGizmoBase
{
protected:
    using PickRaycaster = SceneRaycasterItem;
	enum GripperType {
        UNDEFINE,
        POINT,
        EDGE,
        CIRCLE,
        CIRCLE_1,
        CIRCLE_2,
        PLANE,
        PLANE_1,
        PLANE_2,
        SPHERE_1,
        SPHERE_2,
    };

    enum class EMode : unsigned char
    {
        FeatureSelection,
        PointSelection
    };

    struct SelectedFeatures
    {
        struct Item
        {
            bool is_center{ false };
            std::optional<Measure::SurfaceFeature> source;
            std::optional<Measure::SurfaceFeature> feature;

            bool operator == (const Item& other) const {
                return this->is_center == other.is_center && this->source == other.source && this->feature == other.feature;
            }

            bool operator != (const Item& other) const {
                return !operator == (other);
            }

            void reset() {
                is_center = false;
                source.reset();
                feature.reset();
            }
        };

        Item first;
        Item second;

        void reset() {
            first.reset();
            second.reset();
        }

        bool operator == (const SelectedFeatures & other) const {
            if (this->first != other.first) return false;
            return this->second == other.second;
        }

        bool operator != (const SelectedFeatures & other) const {
            return !operator == (other);
        }
    };

    //struct VolumeCacheItem
    //{
    //    const ModelObject* object{ nullptr };
    //    const ModelInstance* instance{ nullptr };
    //    const ModelVolume* volume{ nullptr };
    //    Transform3d world_trafo;

    //    bool operator == (const VolumeCacheItem& other) const {
    //        return this->object == other.object && this->instance == other.instance && this->volume == other.volume &&
    //            this->world_trafo.isApprox(other.world_trafo);
    //    }
    //};

    //std::vector<VolumeCacheItem> m_volumes_cache;

    EMode m_mode{ EMode::FeatureSelection };
    Measure::MeasurementResult m_measurement_result;
    Measure::AssemblyAction    m_assembly_action;
    std::map<GLVolume*, std::shared_ptr<Measure::Measuring>> m_mesh_measure_map;
    std::shared_ptr<Measure::Measuring>                      m_curr_measuring{nullptr};

    //first feature
    PickingModel m_sphere;
    PickingModel m_cylinder;
    struct CircleGLModel
    {
        PickingModel circle;
        Measure::SurfaceFeature *last_circle_feature{nullptr};
        float                    inv_zoom{0};
    };
    CircleGLModel  m_curr_circle;
    CircleGLModel  m_feature_circle_first;
    CircleGLModel  m_feature_circle_second;
    void           init_circle_glmodel(GripperType gripper_type, const Measure::SurfaceFeature &feature, CircleGLModel &circle_gl_model, float inv_zoom);

    struct PlaneGLModel {
        int                      plane_idx{0};
        PickingModel plane;
    };
    PlaneGLModel m_curr_plane;
    PlaneGLModel m_feature_plane_first;
    PlaneGLModel m_feature_plane_second;
    void  init_plane_glmodel(GripperType gripper_type, const Measure::SurfaceFeature &feature, PlaneGLModel &plane_gl_model);

    struct Dimensioning
    {
        GLModel line;
        GLModel triangle;
        GLModel arc;
    };
    Dimensioning m_dimensioning;

    std::map<GLVolume*, std::shared_ptr<PickRaycaster>>   m_mesh_raycaster_map;
    std::map<GripperType, std::shared_ptr<PickRaycaster>> m_gripper_id_raycast_map;
    std::vector<GLVolume*>                                m_hit_different_volumes;
    std::vector<GLVolume*>                                m_hit_order_volumes;
    GLVolume*                                             m_last_hit_volume;
    //std::vector<std::shared_ptr<GLModel>>                 m_plane_models_cache;
    unsigned int                                          m_last_active_item_imgui{0};
    Vec3d                                                 m_buffered_distance;
    Vec3d                                                 m_distance;
    double                                                m_buffered_parallel_distance{0};
    double                                                m_buffered_around_center{0};
    // used to keep the raycasters for point/center spheres
    //std::vector<std::shared_ptr<PickRaycaster>> m_selected_sphere_raycasters;
    std::optional<Measure::SurfaceFeature> m_curr_feature;
    std::optional<Vec3d> m_curr_point_on_feature_position;

    // These hold information to decide whether recalculation is necessary:
    float m_last_inv_zoom{ 0.0f };
    std::optional<Measure::SurfaceFeature> m_last_circle_feature;
    int m_last_plane_idx{ -1 };

    bool m_mouse_left_down{ false }; // for detection left_up of this gizmo
    bool m_mouse_left_down_mesh_deal{false};//for pick mesh

    KeyAutoRepeatFilter m_shift_kar_filter;

    SelectedFeatures m_selected_features;
    int m_pending_scale{ 0 };
    bool m_set_center_coincidence{false};
    bool m_editing_distance{ false };
    bool m_is_editing_distance_first_frame{ true };
    bool m_can_set_xyz_distance{false};
    void update_if_needed();

    void disable_scene_raycasters();
    void restore_scene_raycasters_state();

    void render_dimensioning();

#if ENABLE_MEASURE_GIZMO_DEBUG
    void render_debug_dialog();
#endif // ENABLE_MEASURE_GIZMO_DEBUG

public:
    GLGizmoMeasure(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);
    /// <summary>
    /// Apply rotation on select plane
    /// </summary>
    /// <param name="mouse_event">Keep information about mouse click</param>
    /// <returns>Return True when use the information otherwise False.</returns>
    bool on_mouse(const wxMouseEvent &mouse_event) override;

    void data_changed(bool is_serializing) override;

    virtual bool gizmo_event(SLAGizmoEventType action, const Vec2d& mouse_position, bool shift_down, bool alt_down, bool control_down);

    bool wants_enter_leave_snapshots() const override { return true; }
    std::string get_gizmo_entering_text() const override { return _u8L("Entering Measure gizmo"); }
    std::string get_gizmo_leaving_text() const override { return _u8L("Leaving Measure gizmo"); }
    //std::string get_action_snapshot_name() const override { return _u8L("Measure gizmo editing"); }

protected:
    bool on_init() override;
    std::string on_get_name() const override;
    bool on_is_activable() const override;
    void on_render() override;
    void on_set_state() override;

    //virtual void on_render_for_picking() override;
    void         show_selection_ui();
    void         show_distance_xyz_ui();
    //void         show_point_point_assembly();
    void         show_face_face_assembly_common();
    void         show_face_face_assembly_senior();
    void         init_render_input_window();
    virtual void on_render_input_window(float x, float y, float bottom_limit) override;

    virtual void render_input_window_warning(bool same_model_object);
    void remove_selected_sphere_raycaster(int id);
    void update_measurement_result();

    void show_tooltip_information(float caption_max, float x, float y);
    void reset_all_pick();
    void reset_gripper_pick(GripperType id,bool is_all = false);
    void register_single_mesh_pick();
    //void update_single_mesh_pick(GLVolume* v);

    std::string format_double(double value);
    std::string format_vec3(const Vec3d &v);
    std::string surface_feature_type_as_string(Measure::SurfaceFeatureType type);
    std::string point_on_feature_type_as_string(Measure::SurfaceFeatureType type, int hover_id);
    std::string center_on_feature_type_as_string(Measure::SurfaceFeatureType type);
    bool is_feature_with_center(const Measure::SurfaceFeature &feature);
    Vec3d get_feature_offset(const Measure::SurfaceFeature &feature);

    void reset_all_feature();
    void reset_feature1_render();
    void reset_feature2_render();
    void reset_feature1();
    void reset_feature2();
    bool is_two_volume_in_same_model_object();
    Measure::Measuring* get_measuring_of_mesh(GLVolume *v, Transform3d &tran);
    void update_world_plane_features(Measure::Measuring *cur_measuring, Measure::SurfaceFeature &feautre);
    void update_feature_by_tran(Measure::SurfaceFeature & feature);
    void set_distance(bool same_model_object, const Vec3d &displacement, bool take_shot = true);
    void set_to_parallel(bool same_model_object, bool take_shot = true, bool is_anti_parallel = false);
    void set_to_reverse_rotation(bool same_model_object,int feature_index);
    void set_to_around_center_of_faces(bool same_model_object,float rotate_degree);
    void set_to_center_coincidence(bool same_model_object);
    void set_parallel_distance(bool same_model_object,float dist);

    bool is_pick_meet_assembly_mode(const SelectedFeatures::Item& item);
 protected:
    // This map holds all translated description texts, so they can be easily referenced during layout calculations
    // etc. When language changes, GUI is recreated and this class constructed again, so the change takes effect.
    std::map<std::string, wxString> m_desc;
    bool                     m_show_reset_first_tip{false};
    bool                     m_selected_wrong_feature_waring_tip{false};
    EMeasureMode             m_measure_mode{EMeasureMode::ONLY_MEASURE};
    AssemblyMode             m_assembly_mode{AssemblyMode::FACE_FACE};
    bool                     m_flip_volume_2{false};
    float                    m_space_size;
    float                    m_input_size_max;
    bool                     m_use_inches;
    bool                     m_only_select_plane{false};
    std::string              m_units;
    mutable bool             m_same_model_object;
    mutable unsigned int     m_current_active_imgui_id;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoMeasure_hpp_
