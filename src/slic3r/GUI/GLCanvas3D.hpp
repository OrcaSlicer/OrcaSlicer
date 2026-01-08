#ifndef slic3r_GLCanvas3D_hpp_
#define slic3r_GLCanvas3D_hpp_

#include <stddef.h>
#include <memory>
#include <chrono>
#include <cstdint>

#include "GLToolbar.hpp"
#include "Event.hpp"
#include "Selection.hpp"
#include "Gizmos/GLGizmosManager.hpp"
#include "GUI_ObjectLayers.hpp"
#include "GLSelectionRectangle.hpp"
#include "MeshUtils.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "GCodeViewer.hpp"
#include "Camera.hpp"
#include "SceneRaycaster.hpp"
#include "IMToolbar.hpp"
#include "slic3r/GUI/3DBed.hpp"
#include "libslic3r/Slicing.hpp"

#include <float.h>

#include <wx/timer.h>

class wxSizeEvent;
class wxIdleEvent;
class wxKeyEvent;
class wxMouseEvent;
class wxTimerEvent;
class wxPaintEvent;
class wxGLCanvas;
class wxGLContext;

// Support for Retina OpenGL on Mac OS.
// wxGTK3 seems to simulate OSX behavior in regard to HiDPI scaling support, enable it as well.
#define ENABLE_RETINA_GL (__APPLE__ || __WXGTK3__)

namespace Slic3r {

class BackgroundSlicingProcess;
class BuildVolume;
struct ThumbnailData;
struct ThumbnailsParams;
class ModelObject;
class ModelInstance;
struct TextInfo;
class PrintObject;
class Print;
class SLAPrint;
class PresetBundle;
namespace CustomGCode { struct Item; }

namespace GUI {

class Bed3D;
class PartPlateList;

#if ENABLE_RETINA_GL
class RetinaHelper;
#endif

class Size
{
    int m_width{ 0 };
    int m_height{ 0 };
    float m_scale_factor{ 1.0f };

public:
    Size() = default;
    Size(int width, int height, float scale_factor = 1.0f) : m_width(width), m_height(height), m_scale_factor(scale_factor) {}

    int get_width() const { return m_width; }
    void set_width(int width) { m_width = width; }

    int get_height() const { return m_height; }
    void set_height(int height) { m_height = height; }

    float get_scale_factor() const { return m_scale_factor; }
    void set_scale_factor(float factor) { m_scale_factor = factor; }
};

class RenderTimerEvent : public wxEvent
{
public:
    RenderTimerEvent(wxEventType type, wxTimer& timer)
        : wxEvent(timer.GetId(), type),
        m_timer(&timer)
    {
        SetEventObject(timer.GetOwner());
    }
    int GetInterval() const { return m_timer->GetInterval(); }
    wxTimer& GetTimer() const { return *m_timer; }

    virtual wxEvent* Clone() const { return new RenderTimerEvent(*this); }
    virtual wxEventCategory GetEventCategory() const  { return wxEVT_CATEGORY_TIMER; }
private:
    wxTimer* m_timer;
};

class  ToolbarHighlighterTimerEvent : public wxEvent
{
public:
    ToolbarHighlighterTimerEvent(wxEventType type, wxTimer& timer)
        : wxEvent(timer.GetId(), type),
        m_timer(&timer)
    {
        SetEventObject(timer.GetOwner());
    }
    int GetInterval() const { return m_timer->GetInterval(); }
    wxTimer& GetTimer() const { return *m_timer; }

    virtual wxEvent* Clone() const { return new ToolbarHighlighterTimerEvent(*this); }
    virtual wxEventCategory GetEventCategory() const { return wxEVT_CATEGORY_TIMER; }
private:
    wxTimer* m_timer;
};


class  GizmoHighlighterTimerEvent : public wxEvent
{
public:
    GizmoHighlighterTimerEvent(wxEventType type, wxTimer& timer)
        : wxEvent(timer.GetId(), type),
        m_timer(&timer)
    {
        SetEventObject(timer.GetOwner());
    }
    int GetInterval() const { return m_timer->GetInterval(); }
    wxTimer& GetTimer() const { return *m_timer; }

    virtual wxEvent* Clone() const { return new GizmoHighlighterTimerEvent(*this); }
    virtual wxEventCategory GetEventCategory() const { return wxEVT_CATEGORY_TIMER; }
private:
    wxTimer* m_timer;
};


wxDECLARE_EVENT(EVT_GLCANVAS_OBJECT_SELECT, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_PLATE_NAME_CHANGE, SimpleEvent);
//BBS: declare EVT_GLCANVAS_PLATE_SELECT
wxDECLARE_EVENT(EVT_GLCANVAS_PLATE_SELECT, SimpleEvent);

using Vec2dEvent = Event<Vec2d>;
// _bool_ value is used as a indicator of selection in the 3DScene
using RBtnEvent = Event<std::pair<Vec2d, bool>>;
using RBtnPlateEvent = Event<std::pair<Vec2d, int>>;
template <size_t N> using Vec2dsEvent = ArrayEvent<Vec2d, N>;

using Vec3dEvent = Event<Vec3d>;
template <size_t N> using Vec3dsEvent = ArrayEvent<Vec3d, N>;

using HeightProfileSmoothEvent = Event<HeightProfileSmoothingParams>;

wxDECLARE_EVENT(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_RIGHT_CLICK, RBtnEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_PLATE_RIGHT_CLICK, RBtnPlateEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_REMOVE_OBJECT, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_ARRANGE, SimpleEvent);
//BBS: add arrange and orient event
wxDECLARE_EVENT(EVT_GLCANVAS_ARRANGE_PARTPLATE, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_ORIENT, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_ORIENT_PARTPLATE, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_SELECT_CURR_PLATE_ALL, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_SELECT_ALL, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_QUESTION_MARK, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_INCREASE_INSTANCES, Event<int>); // data: +1 => increase, -1 => decrease
wxDECLARE_EVENT(EVT_GLCANVAS_INSTANCE_MOVED, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_FORCE_UPDATE, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_INSTANCE_ROTATED, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_INSTANCE_SCALED, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_ENABLE_ACTION_BUTTONS, Event<bool>);
wxDECLARE_EVENT(EVT_GLCANVAS_UPDATE_GEOMETRY, Vec3dsEvent<2>);
wxDECLARE_EVENT(EVT_GLCANVAS_MOUSE_DRAGGING_STARTED, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_MOUSE_DRAGGING_FINISHED, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_UPDATE_BED_SHAPE, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_TAB, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_RESETGIZMOS, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_MOVE_SLIDERS, wxKeyEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_EDIT_COLOR_CHANGE, wxKeyEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_JUMP_TO, wxKeyEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_UNDO, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_REDO, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_SWITCH_TO_OBJECT, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_SWITCH_TO_GLOBAL, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_COLLAPSE_SIDEBAR, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_RELOAD_FROM_DISK, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_RENDER_TIMER, wxTimerEvent/*RenderTimerEvent*/);
wxDECLARE_EVENT(EVT_GLCANVAS_TOOLBAR_HIGHLIGHTER_TIMER, wxTimerEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_GIZMO_HIGHLIGHTER_TIMER, wxTimerEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_UPDATE, SimpleEvent);
wxDECLARE_EVENT(EVT_CUSTOMEVT_TICKSCHANGED, wxCommandEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_RESET_LAYER_HEIGHT_PROFILE, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_ADAPTIVE_LAYER_HEIGHT_PROFILE, Event<float>);
wxDECLARE_EVENT(EVT_GLCANVAS_SMOOTH_LAYER_HEIGHT_PROFILE, HeightProfileSmoothEvent);

class GLCanvas3D
{
    static const double DefaultCameraZoomToBoxMarginFactor;
    static const double DefaultCameraZoomToBedMarginFactor;
    static const double DefaultCameraZoomToPlateMarginFactor;

    static void update_render_colors();
    static void load_render_colors();

    class LayersEditing
    {
    public:
        enum EState : unsigned char
        {
            Unknown,
            Editing,
            Completed,
            Num_States
        };

        static const float THICKNESS_BAR_WIDTH;

        // Orca: Shrinkage compensation
        void set_shrinkage_compensation(const Vec3d &shrinkage_compensation) { m_shrinkage_compensation = shrinkage_compensation; };

    private:
        bool                        m_enabled{ false };
        unsigned int                m_z_texture_id{ 0 };
        // Not owned by LayersEditing.
        const DynamicPrintConfig* m_config{ nullptr };
        // ModelObject for the currently selected object (Model::objects[last_object_id]).
        const ModelObject* m_model_object{ nullptr };
        // Maximum z of the currently selected object (Model::objects[last_object_id]).
        float                       m_object_max_z{ 0.0f };
        // Owned by LayersEditing.
        SlicingParameters* m_slicing_parameters{ nullptr };
        std::vector<double>         m_layer_height_profile;

        // Orca: Shrinkage compensation to apply when we need to use object_max_z with Z compensation.
        Vec3d                       m_shrinkage_compensation{ Vec3d::Ones() };

        mutable float               m_adaptive_quality{ 0.5f };
        mutable HeightProfileSmoothingParams m_smooth_params;

        static float                s_overlay_window_width;

        struct LayersTexture
        {
            // Texture data
            std::vector<char>   data;
            // Width of the texture, top level.
            size_t              width{ 0 };
            // Height of the texture, top level.
            size_t              height{ 0 };
            // For how many levels of detail is the data allocated?
            size_t              levels{ 0 };
            // Number of texture cells allocated for the height texture.
            size_t              cells{ 0 };
            // Does it need to be refreshed?
            bool                valid{ false };
        };
        LayersTexture   m_layers_texture;

    public:
        EState state{ Unknown };
        float band_width{ 2.0f };
        float strength{ 0.005f };
        int last_object_id{ -1 };
        float last_z{ 0.0f };
        LayerHeightEditActionType last_action{ LAYER_HEIGHT_EDIT_ACTION_INCREASE };
        struct Profile
        {
            GLModel baseline;
            GLModel profile;
            GLModel background;
            float old_canvas_width{ 0.0f };
            std::vector<double> old_layer_height_profile;
        };
        Profile m_profile;

        LayersEditing() = default;
        ~LayersEditing();

        void init();

        void set_config(const DynamicPrintConfig* config);
        void select_object(const Model& model, int object_id);

        bool is_allowed() const;

        bool is_enabled() const;
        void set_enabled(bool enabled);

        void show_tooltip_information(const GLCanvas3D& canvas, std::map<wxString, wxString> captions_texts, float x, float y);
        void render_variable_layer_height_dialog(const GLCanvas3D& canvas);
        void render_overlay(const GLCanvas3D& canvas);
        void render_volumes(const GLCanvas3D& canvas, const GLVolumeCollection& volumes);

        void adjust_layer_height_profile();
        void accept_changes(GLCanvas3D& canvas);
        void reset_layer_height_profile(GLCanvas3D& canvas);
        void adaptive_layer_height_profile(GLCanvas3D& canvas, float quality_factor);
        void smooth_layer_height_profile(GLCanvas3D& canvas, const HeightProfileSmoothingParams& smoothing_params);

        static float get_cursor_z_relative(const GLCanvas3D& canvas);
        static bool bar_rect_contains(const GLCanvas3D& canvas, float x, float y);
        static Rect get_bar_rect_screen(const GLCanvas3D& canvas);
        static float get_overlay_window_width() { return LayersEditing::s_overlay_window_width; }

        float object_max_z() const { return m_object_max_z; }

        std::string get_tooltip(const GLCanvas3D& canvas) const;

    private:
        bool is_initialized() const;
        void generate_layer_height_texture();
        void render_active_object_annotations(const GLCanvas3D& canvas);
        void render_profile(const GLCanvas3D& canvas);
        void update_slicing_parameters();

        static float thickness_bar_width(const GLCanvas3D& canvas);
    };

    struct Mouse
    {
        struct Drag
        {
            static const Point Invalid_2D_Point;
            static const Vec3d Invalid_3D_Point;
            static const int MoveThresholdPx;

            Point start_position_2D;
            Vec3d start_position_3D;
            int move_volume_idx;
            bool move_requires_threshold;
            Point move_start_threshold_position_2D;

        public:
            Drag();
        };

        bool dragging;
        Vec2d position;
        Vec3d scene_position;
        Drag drag;
        bool ignore_left_up;
        bool ignore_right_up;

        Mouse();

        void set_start_position_2D_as_invalid() { drag.start_position_2D = Drag::Invalid_2D_Point; }
        void set_start_position_3D_as_invalid() { drag.start_position_3D = Drag::Invalid_3D_Point; }
        void set_move_start_threshold_position_2D_as_invalid() { drag.move_start_threshold_position_2D = Drag::Invalid_2D_Point; }

        bool is_start_position_2D_defined() const { return (drag.start_position_2D != Drag::Invalid_2D_Point); }
        bool is_start_position_3D_defined() const { return (drag.start_position_3D != Drag::Invalid_3D_Point); }
        bool is_move_start_threshold_position_2D_defined() const { return (drag.move_start_threshold_position_2D != Drag::Invalid_2D_Point); }
        bool is_move_threshold_met(const Point& mouse_pos) const {
            return (std::abs(mouse_pos(0) - drag.move_start_threshold_position_2D(0)) > Drag::MoveThresholdPx)
                || (std::abs(mouse_pos(1) - drag.move_start_threshold_position_2D(1)) > Drag::MoveThresholdPx);
        }
    };

    struct SlaCap
    {
        struct Triangles
        {
            GLModel object;
            GLModel supports;
        };
        typedef std::map<unsigned int, Triangles> ObjectIdToModelsMap;
        double z;
        ObjectIdToModelsMap triangles;

        SlaCap() { reset(); }
        void reset() { z = DBL_MAX; triangles.clear(); }
        bool matches(double z) const { return this->z == z; }
    };

    enum class EWarning {
        ObjectOutside,
        ToolpathOutside,
        SlaSupportsOutside,
        SomethingNotShown,
        ObjectClashed,
        ObjectLimited,
        GCodeConflict,
        ToolHeightOutside,
        TPUPrintableError,
        FilamentPrintableError,
        LeftExtruderPrintableError, // before slice
        RightExtruderPrintableError, // before slice
        MultiExtruderPrintableError,      // after slice
        MultiExtruderHeightOutside,       // after slice
        FilamentUnPrintableOnFirstLayer,
        MixUsePLAAndPETG,
        PrimeTowerOutside,
        NozzleFilamentIncompatible,
        MixtureFilamentIncompatible,
        FlushingVolumeZero
    };

    class RenderStats
    {
    private:
        std::chrono::time_point<std::chrono::high_resolution_clock> m_measuring_start;
        int m_fps_out = -1;
        int m_fps_running = 0;
    public:
        void increment_fps_counter() { ++m_fps_running; }
        int get_fps() { return m_fps_out; }
        int get_fps_and_reset_if_needed() {
            auto cur_time = std::chrono::high_resolution_clock::now();
            int elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(cur_time-m_measuring_start).count();
            if (elapsed_ms > 1000  || m_fps_out == -1) {
                m_measuring_start = cur_time;
                m_fps_out = int (1000. * m_fps_running / elapsed_ms);
                m_fps_running = 0;
            }
            return m_fps_out;
        }

    };

    class Labels
    {
        bool m_enabled{ false };
        bool m_shown{ false };
        GLCanvas3D& m_canvas;

    public:
        explicit Labels(GLCanvas3D& canvas) : m_canvas(canvas) {}
        void enable(bool enable) { m_enabled = enable; }
        void show(bool show) { m_shown = m_enabled ? show : false; }
        bool is_shown() const { return m_shown; }
        void render(const std::vector<const ModelInstance*>& sorted_instances) const;
    };

    class Tooltip
    {
        std::string m_text;
        std::chrono::steady_clock::time_point m_start_time;
        // Indicator that the mouse is inside an ImGUI dialog, therefore the tooltip should be suppressed.
        bool m_in_imgui = false;

    public:
        bool is_empty() const { return m_text.empty(); }
        void set_text(const std::string& text);
        void render(const Vec2d& mouse_position, GLCanvas3D& canvas);
        // Indicates that the mouse is inside an ImGUI dialog, therefore the tooltip should be suppressed.
        void set_in_imgui(bool b) { m_in_imgui = b; }
        bool is_in_imgui() const { return m_in_imgui; }
    };

    class Slope
    {
        bool m_enabled{ false };
        GLVolumeCollection& m_volumes;
    public:
        Slope(GLVolumeCollection& volumes) : m_volumes(volumes) {}

        void enable(bool enable) { m_enabled = enable; }
        bool is_enabled() const { return m_enabled; }
        void use(bool use) { m_volumes.set_slope_active(m_enabled ? use : false); }
        bool is_used() const { return m_volumes.is_slope_active(); }
        void globalUse(bool use) { m_volumes.set_slope_GlobalActive(m_enabled ? use : false); }
        bool is_GlobalUsed() const { return m_volumes.is_slope_GlobalActive(); }
        void set_normal_angle(float angle_in_deg) const {
            m_volumes.set_slope_normal_z(-::cos(Geometry::deg2rad(90.0f - angle_in_deg)));
        }
    };

    class RenderTimer : public wxTimer {
    private:
        virtual void Notify() override;
    };

    class ToolbarHighlighterTimer : public wxTimer {
    private:
        virtual void Notify() override;
    };

    class GizmoHighlighterTimer : public wxTimer {
    private:
        virtual void Notify() override;
    };

public:
    enum ECursorType : unsigned char
    {
        Standard,
        Cross
    };

    struct ArrangeSettings
    {
        float distance           = 0.f;
//        float distance_sla       = 6.;
        float accuracy           = 0.65f; // Unused currently
        bool  enable_rotation    = false;
        bool  allow_multi_materials_on_same_plate = true;
        bool  avoid_extrusion_cali_region = true;
        //BBS: add more arrangeSettings
        bool is_seq_print        = false;
        bool  align_to_y_axis    = false;
    };

    struct OrientSettings
    {
        float overhang_angle = 60.f;
        bool  enable_rotation = false;
        bool  min_area = true;
    };

    //BBS: add canvas type for assemble view usage
    enum ECanvasType
    {
        CanvasView3D = 0,
        CanvasPreview = 1,
        CanvasAssembleView = 2,
    };

    int GetHoverId();

private:
    bool m_is_dark = false;
    wxGLCanvas* m_canvas;
    wxGLContext* m_context;
    SceneRaycaster m_scene_raycaster;
    Bed3D &m_bed;
    std::map<std::string, wxString> m_assembly_view_desc;
#if ENABLE_RETINA_GL
    std::unique_ptr<RetinaHelper> m_retina_helper;
#endif
    unsigned int m_last_w, m_last_h;
    bool m_in_render;
    wxTimer m_timer;
    wxTimer m_timer_set_color;
    LayersEditing m_layers_editing;
    Mouse m_mouse;
    GLGizmosManager m_gizmos;
    //BBS: GUI refactor: GLToolbar
    mutable GLToolbar m_main_toolbar;
    mutable GLToolbar m_separator_toolbar;
    mutable IMToolbar m_sel_plate_toolbar;
    mutable GLToolbar m_assemble_view_toolbar;
    mutable IMReturnToolbar m_return_toolbar;
    mutable Vec2i32 m_canvas_toolbar_pos = {140, 5};
    mutable float m_sc{1};
    mutable float m_paint_toolbar_width;

    //BBS: add canvas type for assemble view usage
    ECanvasType m_canvas_type;
    std::array<ClippingPlane, 2> m_clipping_planes;
    ClippingPlane m_camera_clipping_plane;
    bool m_use_clipping_planes;
    std::array<SlaCap, 2> m_sla_caps;
    std::string m_sidebar_field;
    // when true renders an extra frame by not resetting m_dirty to false
    // see request_extra_frame()
    bool m_extra_frame_requested;
    bool m_event_handlers_bound{ false };

    GLVolumeCollection m_volumes;
    GCodeViewer m_gcode_viewer;

    RenderTimer m_render_timer;

    Selection m_selection;
    const DynamicPrintConfig* m_config;
    Model* m_model;
    BackgroundSlicingProcess *m_process;
    bool m_requires_check_outside_state{ false };

    std::array<unsigned int, 2> m_old_size{ 0, 0 };

    bool m_is_touchpad_navigation{ false };

    // Screen is only refreshed from the OnIdle handler if it is dirty.
    bool m_dirty;
    bool m_initialized;
    //BBS: add flag to controll rendering
    bool m_render_preview{ true };
    bool m_enable_render { true };
    bool m_apply_zoom_to_volumes_filter;
    bool m_picking_enabled;
    bool m_moving_enabled;
    bool m_dynamic_background_enabled;
    bool m_multisample_allowed;
    bool m_moving;
    bool m_tab_down;
    bool m_camera_movement;
    //BBS: add toolpath outside
    bool m_toolpath_outside{ false };
    ECursorType m_cursor_type;
    GLSelectionRectangle m_rectangle_selection;

    //BBS:add plate related logic
    mutable std::vector<int> m_hover_volume_idxs;
    std::vector<int> m_hover_plate_idxs;
    //BBS if explosion_ratio is changed, need to update volume bounding box
    mutable float m_explosion_ratio = 1.0;
    mutable Vec3d m_rotation_center{ 0.0, 0.0, 0.0};
    //BBS store camera view
    Camera camera;

    // Following variable is obsolete and it should be safe to remove it.
    // I just don't want to do it now before a release (Lukas Matena 24.3.2019)
    bool m_render_sla_auxiliaries;

    std::string m_color_by;

    bool m_reload_delayed;

    RenderStats m_render_stats;

    int m_imgui_undo_redo_hovered_pos{ -1 };
    int m_mouse_wheel{ 0 };
    int m_selected_extruder;

    Labels m_labels;
    Tooltip m_tooltip;
    bool m_tooltip_enabled{ true };
    Slope m_slope;

    OrientSettings m_orient_settings_fff, m_orient_settings_sla;

    ArrangeSettings m_arrange_settings_fff, m_arrange_settings_sla,
        m_arrange_settings_fff_seq_print;

    PrinterTechnology current_printer_technology() const;

    bool        m_show_world_axes{true};
    Bed3D::Axes m_axes;
    //BBS:record key botton frequency
    int auto_orient_count = 0;
    int auto_arrange_count = 0;
    int split_to_objects_count = 0;
    int split_to_part_count = 0;
    int custom_height_count = 0;
    int assembly_view_count = 0;

public:
    OrientSettings& get_orient_settings()
    {
        PrinterTechnology ptech = this->current_printer_technology();

        auto* ptr = &this->m_orient_settings_fff;

        if (ptech == ptSLA) {
            ptr = &this->m_orient_settings_sla;
        }

        return *ptr;
    }

    void load_arrange_settings();
    ArrangeSettings& get_arrange_settings();// { return get_arrange_settings(this); }
    ArrangeSettings& get_arrange_settings(PrintSequence print_seq) {
        return (print_seq == PrintSequence::ByObject) ? m_arrange_settings_fff_seq_print
            : m_arrange_settings_fff;
    }

    class SequentialPrintClearance
    {
        //BBS: add the height logic
        GLModel m_height_limit;
        GLModel m_fill;
        GLModel m_perimeter;
        bool m_render_fill{ true };
        bool m_visible{ false };

        std::vector<Pointf3s> m_hull_2d_cache;

    public:
        //BBS: add the height logic
        void set_polygons(const Polygons& polygons, const std::vector<std::pair<Polygon, float>>& height_polygons);
        void set_render_fill(bool render_fill) { m_render_fill = render_fill; }
        void set_visible(bool visible) { m_visible = visible; }
        void render();

        friend class GLCanvas3D;
    };

    SequentialPrintClearance m_sequential_print_clearance;
    bool m_sequential_print_clearance_first_displacement{ true };

    struct ToolbarHighlighter
    {
        void set_timer_owner(wxEvtHandler* owner, int timerid = wxID_ANY);
        void init(GLToolbarItem* toolbar_item, GLCanvas3D* canvas);
        void blink();
        void invalidate();
        bool                    m_render_arrow{ false };
        GLToolbarItem*          m_toolbar_item{ nullptr };
    private:
        GLCanvas3D*             m_canvas{ nullptr };
        int				        m_blink_counter{ 0 };
        ToolbarHighlighterTimer m_timer;
    }
    m_toolbar_highlighter;

    struct GizmoHighlighter
    {
        void set_timer_owner(wxEvtHandler* owner, int timerid = wxID_ANY);
        void init(GLGizmosManager* manager, GLGizmosManager::EType gizmo, GLCanvas3D* canvas);
        void blink();
        void invalidate();
        bool                    m_render_arrow{ false };
        GLGizmosManager::EType  m_gizmo_type;
    private:
        GLGizmosManager*        m_gizmo_manager{ nullptr };
        GLCanvas3D*             m_canvas{ nullptr };
        int				        m_blink_counter{ 0 };
        GizmoHighlighterTimer   m_timer;

    }
    m_gizmo_highlighter;

#if ENABLE_SHOW_CAMERA_TARGET
    struct CameraTarget
    {
        std::array<GLModel, 3> axis;
        Vec3d target{ Vec3d::Zero() };
    };

    CameraTarget m_camera_target;
#endif // ENABLE_SHOW_CAMERA_TARGET
    GLModel m_background;
public:
    explicit GLCanvas3D(wxGLCanvas* canvas, Bed3D &bed);
    ~GLCanvas3D();

    bool is_initialized() const { return m_initialized; }

    void set_context(wxGLContext* context) { m_context = context; }
    void set_type(ECanvasType type) { m_canvas_type = type; }
    ECanvasType get_canvas_type() { return m_canvas_type; }

    wxGLCanvas* get_wxglcanvas() { return m_canvas; }
	const wxGLCanvas* get_wxglcanvas() const { return m_canvas; }

    bool init();
    void post_event(wxEvent &&event);

    std::shared_ptr<SceneRaycasterItem> add_raycaster_for_picking(SceneRaycaster::EType type, int id, const MeshRaycaster& raycaster,
        const Transform3d& trafo = Transform3d::Identity(), bool use_back_faces = false) {
        return m_scene_raycaster.add_raycaster(type, id, raycaster, trafo, use_back_faces);
    }
    void remove_raycasters_for_picking(SceneRaycaster::EType type, int id) {
        m_scene_raycaster.remove_raycasters(type, id);
    }
    void remove_raycasters_for_picking(SceneRaycaster::EType type) {
        m_scene_raycaster.remove_raycasters(type);
    }

    std::vector<std::shared_ptr<SceneRaycasterItem>>* get_raycasters_for_picking(SceneRaycaster::EType type) {
        return m_scene_raycaster.get_raycasters(type);
    }

    void set_raycaster_gizmos_on_top(bool value) {
        m_scene_raycaster.set_gizmos_on_top(value);
    }

    float get_explosion_ratio() { return m_explosion_ratio; }
    void reset_explosion_ratio() { m_explosion_ratio = 1.0; }
    void on_change_color_mode(bool is_dark, bool reinit = true);
    const bool get_dark_mode_status() { return m_is_dark; }
    void set_as_dirty();
    void requires_check_outside_state() { m_requires_check_outside_state = true; }

    unsigned int get_volumes_count() const;
    const GLVolumeCollection& get_volumes() const { return m_volumes; }
    void reset_volumes();
    ModelInstanceEPrintVolumeState check_volumes_outside_state(ObjectFilamentResults* object_results = nullptr) const;
    bool is_all_plates_selected() { return m_sel_plate_toolbar.m_all_plates_stats_item && m_sel_plate_toolbar.m_all_plates_stats_item->selected; }
    const float get_scale() const;

    //BBS
    GCodeViewer& get_gcode_viewer() { return m_gcode_viewer; }
    void init_gcode_viewer(ConfigOptionMode mode, Slic3r::PresetBundle* preset_bundle) { m_gcode_viewer.init(mode, preset_bundle); }
    void reset_gcode_toolpaths() { m_gcode_viewer.reset(); }
    const GCodeViewer::SequentialView& get_gcode_sequential_view() const { return m_gcode_viewer.get_sequential_view(); }
    void update_gcode_sequential_view_current(unsigned int first, unsigned int last) { m_gcode_viewer.update_sequential_view_current(first, last); }

    void toggle_selected_volume_visibility(bool selected_visible);
    void toggle_sla_auxiliaries_visibility(bool visible, const ModelObject* mo = nullptr, int instance_idx = -1);
    void toggle_model_objects_visibility(bool visible, const ModelObject* mo = nullptr, int instance_idx = -1, const ModelVolume* mv = nullptr);
    void update_instance_printable_state_for_object(size_t obj_idx);
    void update_instance_printable_state_for_objects(const std::vector<size_t>& object_idxs);

    void set_config(const DynamicPrintConfig* config);
    void set_process(BackgroundSlicingProcess* process);
    void set_model(Model* model);
    const Model* get_model() const { return m_model; }

    const Selection& get_selection() const { return m_selection; }
    Selection& get_selection() { return m_selection; }

    const GLGizmosManager& get_gizmos_manager() const { return m_gizmos; }
    GLGizmosManager& get_gizmos_manager() { return m_gizmos; }

    void bed_shape_changed();

    //BBS: add part plate related logic
    void plates_count_changed();

    //BBS get camera
    Camera& get_camera();

    void set_clipping_plane(unsigned int id, const ClippingPlane& plane)
    {
        if (id < 2)
        {
            m_clipping_planes[id] = plane;
            m_sla_caps[id].reset();
        }
    }
    void reset_clipping_planes_cache() { m_sla_caps[0].triangles.clear(); m_sla_caps[1].triangles.clear(); }
    void set_use_clipping_planes(bool use) { m_use_clipping_planes = use; }

    bool                                get_use_clipping_planes() const { return m_use_clipping_planes; }
    const std::array<ClippingPlane, 2> &get_clipping_planes() const { return m_clipping_planes; };

    void set_use_color_clip_plane(bool use) { m_volumes.set_use_color_clip_plane(use); }
    void set_color_clip_plane(const Vec3d& cp_normal, double offset) { m_volumes.set_color_clip_plane(cp_normal, offset); }
    void set_color_clip_plane_colors(const std::array<ColorRGBA, 2>& colors) { m_volumes.set_color_clip_plane_colors(colors); }

    void toggle_world_axes_visibility(bool force_show = false);
    void refresh_camera_scene_box();
    void set_color_by(const std::string& value);

    BoundingBoxf3 volumes_bounding_box(bool current_plate_only = false) const;
    BoundingBoxf3 scene_bounding_box() const;
    BoundingBoxf3 plate_scene_bounding_box(int plate_idx) const;

    bool is_layers_editing_enabled() const;
    bool is_layers_editing_allowed() const;

    void reset_layer_height_profile();
    void adaptive_layer_height_profile(float quality_factor);
    void smooth_layer_height_profile(const HeightProfileSmoothingParams& smoothing_params);

    bool is_reload_delayed() const;

    void enable_layers_editing(bool enable);
    void enable_legend_texture(bool enable);
    void enable_picking(bool enable);
    void enable_moving(bool enable);
    void enable_gizmos(bool enable);
    void enable_selection(bool enable);
    void enable_main_toolbar(bool enable);
    //BBS: GUI refactor: GLToolbar
    void _update_select_plate_toolbar_stats_item(bool force_selected = false);
    void reset_select_plate_toolbar_selection();
    void enable_select_plate_toolbar(bool enable);
    void enable_assemble_view_toolbar(bool enable);
    void enable_return_toolbar(bool enable);
    void enable_separator_toolbar(bool enable);
    void enable_dynamic_background(bool enable);
    void enable_labels(bool enable) { m_labels.enable(enable); }
    void enable_slope(bool enable) { m_slope.enable(enable); }
    void allow_multisample(bool allow);

    void zoom_to_bed();
    void zoom_to_volumes();
    void zoom_to_selection();
    void zoom_to_gcode();
    //BBS -1 for current plate
    void zoom_to_plate(int plate_idx = -1);
    void select_view(const std::string& direction);
    //BBS: add part plate related logic
    void select_plate();
    //BBS: GUI refactor: GLToolbar&&gizmo
    int get_main_toolbar_offset() const;
    int get_main_toolbar_height() const { return m_main_toolbar.get_height(); }
    int get_main_toolbar_width() const { return m_main_toolbar.get_width(); }
    float get_assemble_view_toolbar_width() const { return m_assemble_view_toolbar.get_width(); }
    float get_assemble_view_toolbar_height() const { return m_assemble_view_toolbar.get_height(); }
    float get_assembly_paint_toolbar_width() const { return m_paint_toolbar_width; }
    float get_separator_toolbar_width() const { return m_separator_toolbar.get_width(); }
    float get_separator_toolbar_height() const { return m_separator_toolbar.get_height(); }
    bool  is_collapse_toolbar_on_left() const;
    float get_collapse_toolbar_width() const;
    float get_collapse_toolbar_height() const;

    void update_volumes_colors_by_extruder();

    bool is_dragging() const { return m_gizmos.is_dragging() || m_moving; }

    void render(bool only_init = false);
    bool is_rendering_enabled()
    {
        return m_enable_render;
    }
    void enable_render(bool enabled)
    {
        m_enable_render = enabled;
    }

    // printable_only == false -> render also non printable volumes as grayed
    // parts_only == false -> render also sla support and pad
    void render_thumbnail(ThumbnailData& thumbnail_data, unsigned int w, unsigned int h, const ThumbnailsParams& thumbnail_params,
                                 Camera::EType           camera_type,
                                 Camera::ViewAngleType   camera_view_angle_type = Camera::ViewAngleType::Iso,
                                 bool                    for_picking  = false,
                                 bool                    ban_light    = false);
    void render_thumbnail(ThumbnailData &           thumbnail_data,
                                 unsigned int              w,
                                 unsigned int              h,
                                 const ThumbnailsParams &  thumbnail_params,
                                 ModelObjectPtrs &         model_objects,
                                 const GLVolumeCollection &volumes,
                                 Camera::EType             camera_type,
                                 Camera::ViewAngleType     camera_view_angle_type = Camera::ViewAngleType::Iso,
                                 bool                      for_picking  = false,
                                 bool                      ban_light    = false);
    void render_thumbnail(ThumbnailData &           thumbnail_data,
                                 std::vector<ColorRGBA> &  extruder_colors,
                                 unsigned int              w,
                                 unsigned int              h,
                                 const ThumbnailsParams &  thumbnail_params,
                                 ModelObjectPtrs &         model_objects,
                                 const GLVolumeCollection &volumes,
                                 Camera::EType             camera_type,
                                 Camera::ViewAngleType     camera_view_angle_type = Camera::ViewAngleType::Iso,
                                 bool                      for_picking  = false,
                                 bool                      ban_light    = false);
    static void render_thumbnail_internal(ThumbnailData& thumbnail_data, const ThumbnailsParams& thumbnail_params, PartPlateList& partplate_list, ModelObjectPtrs& model_objects,
        const GLVolumeCollection& volumes, std::vector<ColorRGBA>& extruder_colors,
                                          GLShaderProgram *                  shader,
                                          Camera::EType                      camera_type,
                                          Camera::ViewAngleType              camera_view_angle_type = Camera::ViewAngleType::Iso,
                                          bool                               for_picking  = false,
                                          bool                               ban_light    = false);
    // render thumbnail using an off-screen framebuffer
    static void render_thumbnail_framebuffer(ThumbnailData& thumbnail_data, unsigned int w, unsigned int h, const ThumbnailsParams& thumbnail_params,
        PartPlateList& partplate_list, ModelObjectPtrs& model_objects, const GLVolumeCollection& volumes, std::vector<ColorRGBA>& extruder_colors,
                                             GLShaderProgram *                  shader,
                                             Camera::EType                      camera_type,
                                             Camera::ViewAngleType              camera_view_angle_type = Camera::ViewAngleType::Iso,
                                             bool                               for_picking  = false,
                                             bool                               ban_light    = false);
    // render thumbnail using an off-screen framebuffer when GLEW_EXT_framebuffer_object is supported
    static void render_thumbnail_framebuffer_ext(ThumbnailData& thumbnail_data, unsigned int w, unsigned int h, const ThumbnailsParams& thumbnail_params,
        PartPlateList& partplate_list, ModelObjectPtrs& model_objects, const GLVolumeCollection& volumes, std::vector<ColorRGBA>& extruder_colors,
                                                 GLShaderProgram *                  shader,
                                                 Camera::EType                      camera_type,
                                                 Camera::ViewAngleType              camera_view_angle_type = Camera::ViewAngleType::Iso,
                                                 bool                               for_picking  = false,
                                                 bool                               ban_light    = false);

    // render thumbnail using the default framebuffer
    static void render_thumbnail_legacy(ThumbnailData &                    thumbnail_data,
                                 unsigned int                       w,
                                 unsigned int                       h,
                                 const ThumbnailsParams &           thumbnail_params,
                                 PartPlateList &                    partplate_list,
                                 ModelObjectPtrs &                  model_objects,
                                 const GLVolumeCollection &         volumes,
                                 std::vector<ColorRGBA> &           extruder_colors,
                                 GLShaderProgram *                  shader,
                                 Camera::EType                      camera_type,
                                 Camera::ViewAngleType              camera_view_angle_type = Camera::ViewAngleType::Iso,
                                 bool                               for_picking  = false,
                                 bool                               ban_light = false);

    //BBS use gcoder viewer render calibration thumbnails
    void render_calibration_thumbnail(ThumbnailData& thumbnail_data, unsigned int w, unsigned int h, const ThumbnailsParams& thumbnail_params);

    //BBS
    void select_curr_plate_all();
    void select_object_from_idx(std::vector<int>& object_idxs);
    void remove_curr_plate_all();
    void update_plate_thumbnails();

    void select_all();
    void deselect_all();
    void exit_gizmo();
    void set_selected_visible(bool visible);
    void delete_selected();
    void ensure_on_bed(unsigned int object_idx, bool allow_negative_z);

    bool is_gcode_legend_enabled() const { return m_gcode_viewer.is_legend_enabled(); }
    GCodeViewer::EViewType get_gcode_view_type() const { return m_gcode_viewer.get_view_type(); }
    const std::vector<double>& get_gcode_layers_zs() const;
    std::vector<double> get_volumes_print_zs(bool active_only) const;
    unsigned int get_gcode_options_visibility_flags() const { return m_gcode_viewer.get_options_visibility_flags(); }
    void set_gcode_options_visibility_from_flags(unsigned int flags);
    unsigned int get_toolpath_role_visibility_flags() const { return m_gcode_viewer.get_toolpath_role_visibility_flags(); }
    void set_volumes_z_range(const std::array<double, 2>& range);
    std::vector<CustomGCode::Item>& get_custom_gcode_per_print_z() { return m_gcode_viewer.get_custom_gcode_per_print_z(); }
    size_t get_gcode_extruders_count() { return m_gcode_viewer.get_extruders_count(); }

    std::vector<int> load_object(const ModelObject& model_object, int obj_idx, std::vector<int> instance_idxs);
    std::vector<int> load_object(const Model& model, int obj_idx);

    void mirror_selection(Axis axis);

    void reload_scene(bool refresh_immediately, bool force_full_scene_refresh = false);
    //Orca: shell preview improvement
    void set_shell_transparence(float alpha = 0.2f);
    void load_shells(const Print& print, bool force_previewing = false);
    void reset_shells() { m_gcode_viewer.reset_shell(); }
    void set_shells_on_previewing(bool is_preview) { m_gcode_viewer.set_shells_on_preview(is_preview); }

    //BBS: add only gcode mode
    void load_gcode_preview(const GCodeProcessorResult& gcode_result, const std::vector<std::string>& str_tool_colors, bool only_gcode);
    void refresh_gcode_preview_render_paths();
    void set_gcode_view_preview_type(GCodeViewer::EViewType type) { return m_gcode_viewer.set_view_type(type); }
    GCodeViewer::EViewType get_gcode_view_preview_type() const { return m_gcode_viewer.get_view_type(); }
    void load_sla_preview();
    //void load_preview(const std::vector<std::string>& str_tool_colors, const std::vector<CustomGCode::Item>& color_print_values);
    void bind_event_handlers();
    void unbind_event_handlers();

    void on_size(wxSizeEvent& evt);
    void on_idle(wxIdleEvent& evt);
    void on_char(wxKeyEvent& evt);
    void on_key(wxKeyEvent& evt);
    void on_mouse_wheel(wxMouseEvent& evt);
    void on_timer(wxTimerEvent& evt);
    void on_render_timer(wxTimerEvent& evt);
    void on_set_color_timer(wxTimerEvent& evt);
    void on_mouse(wxMouseEvent& evt);
    void on_gesture(wxGestureEvent& evt);
    void on_paint(wxPaintEvent& evt);
    void on_set_focus(wxFocusEvent& evt);
    void force_set_focus();

    bool is_camera_rotate(const wxMouseEvent& evt, const bool buttonsSwapped) const;
    bool is_camera_pan(const wxMouseEvent& evt, const bool buttonsSwapped) const;

    Size get_canvas_size() const;
    Vec2d get_local_mouse_position() const;

    // store opening position of menu
    std::optional<Vec2d> m_popup_menu_positon; // position of mouse right click
    void  set_popup_menu_position(const Vec2d &position) { m_popup_menu_positon = position; }
    const std::optional<Vec2d>& get_popup_menu_position() const { return m_popup_menu_positon; }
    void clear_popup_menu_position() { m_popup_menu_positon.reset(); }

    void set_tooltip(const std::string& tooltip);

    // the following methods add a snapshot to the undo/redo stack, unless the given string is empty
    void do_move(const std::string& snapshot_type);
    void do_rotate(const std::string& snapshot_type);
    void do_scale(const std::string& snapshot_type);
    void do_center();
    void do_drop();
    void do_center_plate(const int plate_idx);
    void do_mirror(const std::string& snapshot_type);

    void update_gizmos_on_off_state();
    void reset_all_gizmos() { m_gizmos.reset_all_states(); }

    void handle_sidebar_focus_event(const std::string& opt_key, bool focus_on);
    void handle_layers_data_focus_event(const t_layer_height_range range, const EditorType type);

    void update_ui_from_settings();

    int get_move_volume_id() const { return m_mouse.drag.move_volume_idx; }
    int get_first_hover_volume_idx() const { return m_hover_volume_idxs.empty() ? -1 : m_hover_volume_idxs.front(); }
    void set_selected_extruder(int extruder) { m_selected_extruder = extruder;}

    class WipeTowerInfo {
    protected:
        Vec2d m_pos = {std::nan(""), std::nan("")};
        double m_rotation = 0.;
        BoundingBoxf m_bb;
        // BBS: add partplate logic
        int m_plate_idx = -1;
        friend class GLCanvas3D;

    public:
        inline operator bool() const {
            return !std::isnan(m_pos.x()) && !std::isnan(m_pos.y());
        }

        inline const Vec2d& pos() const { return m_pos; }
        inline double rotation() const { return m_rotation; }
        inline const Vec2d bb_size() const { return m_bb.size(); }

        void apply_wipe_tower() const;
    };

    // BBS: add partplate logic
    WipeTowerInfo get_wipe_tower_info(int plate_idx) const;

    // Returns the view ray line, in world coordinate, at the given mouse position.
    Linef3 mouse_ray(const Point& mouse_pos);

    void set_mouse_as_dragging() { m_mouse.dragging = true; }
    bool is_mouse_dragging() const { return m_mouse.dragging; }

    double get_size_proportional_to_max_bed_size(double factor) const;

    // BBS: get empty cells to put new object
    // start_point={-1,-1} means sort from bed center, step is the unscaled x,y stride
    std::vector<Vec2f> get_empty_cells(const Vec2f start_point, const Vec2f step = {10, 10});
    // BBS: get the nearest empty cell
    // start_point={-1,-1} means sort from bed center
    Vec2f get_nearest_empty_cell(const Vec2f start_point, const Vec2f step = {10, 10});

    void set_cursor(ECursorType type);
    void msw_rescale();

    void request_extra_frame() { m_extra_frame_requested = true; }

    void schedule_extra_frame(int miliseconds);

    int get_main_toolbar_item_id(const std::string& name) const { return m_main_toolbar.get_item_id(name); }
    void force_main_toolbar_left_action(int item_id) { m_main_toolbar.force_left_action(item_id, *this); }
    void force_main_toolbar_right_action(int item_id) { m_main_toolbar.force_right_action(item_id, *this); }

    bool has_toolpaths_to_export() const;
    void export_toolpaths_to_obj(const char* filename) const;

    void mouse_up_cleanup();

    bool are_labels_shown() const { return m_labels.is_shown(); }
    void show_labels(bool show) { m_labels.show(show); }

    bool is_overhang_shown() const { return m_slope.is_GlobalUsed(); }
    void show_overhang(bool show) { m_slope.globalUse(show); }

    bool is_using_slope() const { return m_slope.is_used(); }
    void use_slope(bool use) { m_slope.use(use); }
    void set_slope_normal_angle(float angle_in_deg) { m_slope.set_normal_angle(angle_in_deg); }

    void highlight_toolbar_item(const std::string& item_name);
    void highlight_gizmo(const std::string& gizmo_name);

    ArrangeSettings get_arrange_settings() const {
        const ArrangeSettings &settings = get_arrange_settings();
        ArrangeSettings ret = settings;
        if (&settings == &m_arrange_settings_fff_seq_print) {
            ret.distance = std::max(ret.distance,
                                    float(min_object_distance(*m_config)));
        }

        return ret;
    }

    // Timestamp for FPS calculation and notification fade-outs.
    static int64_t timestamp_now() {
#ifdef _WIN32
        // Cheaper on Windows, calls GetSystemTimeAsFileTime()
        return wxGetUTCTimeMillis().GetValue();
#else
        // calls clock()
        return wxGetLocalTimeMillis().GetValue();
#endif
    }

    void reset_sequential_print_clearance() {
        m_sequential_print_clearance.set_visible(false);
        m_sequential_print_clearance.set_render_fill(false);
        //BBS: add the height logic
        m_sequential_print_clearance.set_polygons(Polygons(), std::vector<std::pair<Polygon, float>>());
    }

    void set_sequential_print_clearance_visible(bool visible) {
        m_sequential_print_clearance.set_visible(visible);
    }

    void set_sequential_print_clearance_render_fill(bool render_fill) {
        m_sequential_print_clearance.set_render_fill(render_fill);
    }

    //BBS: add the height logic
    void set_sequential_print_clearance_polygons(const Polygons& polygons, const std::vector<std::pair<Polygon, float>>& height_polygons) {
        m_sequential_print_clearance.set_polygons(polygons, height_polygons);
    }

    bool can_sequential_clearance_show_in_gizmo();
    void update_sequential_clearance();

    const Print* fff_print() const;
    const SLAPrint* sla_print() const;

    void reset_old_size() { m_old_size = { 0, 0 }; }

    bool is_object_sinking(int object_idx) const;

    void apply_retina_scale(Vec2d &screen_coordinate) const;

    void _perform_layer_editing_action(wxMouseEvent* evt = nullptr);

    // Convert the screen space coordinate to an object space coordinate.
    // If the Z screen space coordinate is not provided, a depth buffer value is substituted.
    Vec3d _mouse_to_3d(const Point& mouse_pos, float* z = nullptr);

    bool make_current_for_postinit();

private:
    bool _is_shown_on_screen() const;

    void _update_slice_error_status();

    void _switch_toolbars_icon_filename();
    bool _init_toolbars();
    bool _init_main_toolbar();
    bool _init_select_plate_toolbar();
    bool _update_imgui_select_plate_toolbar();
    bool _init_assemble_view_toolbar();
    bool _init_return_toolbar();
    bool _init_separator_toolbar();
    // BBS
    //bool _init_view_toolbar();
    bool _init_collapse_toolbar();

    bool _set_current();
    void _resize(unsigned int w, unsigned int h);

    //BBS: add part plate related logic
    BoundingBoxf3 _max_bounding_box(bool include_gizmos, bool include_bed_model, bool include_plates) const;

    void _zoom_to_box(const BoundingBoxf3& box, double margin_factor = DefaultCameraZoomToBoxMarginFactor);
    void _update_camera_zoom(double zoom);

    void _refresh_if_shown_on_screen();

    void _picking_pass();
    void _rectangular_selection_picking_pass();
    void _render_background();
    void _render_bed(const Transform3d& view_matrix, const Transform3d& projection_matrix, bool bottom, bool show_axes);
    //BBS: add part plate related logic
    void _render_platelist(const Transform3d& view_matrix, const Transform3d& projection_matrix, bool bottom, bool only_current, bool only_body = false, int hover_id = -1, bool render_cali = false, bool show_grid = true);
    //BBS: add outline drawing logic
    void _render_objects(GLVolumeCollection::ERenderType type, bool with_outline = true);
    //BBS: GUI refactor: add canvas size as parameters
    void _render_gcode(int canvas_width, int canvas_height);
    //BBS: render a plane for assemble
    void _render_plane() const;
    void _render_selection();
    void _render_sequential_clearance();
#if ENABLE_RENDER_SELECTION_CENTER
    void _render_selection_center();
#endif // ENABLE_RENDER_SELECTION_CENTER
    void _check_and_update_toolbar_icon_scale();
    void _render_overlays();
    void _render_style_editor();
    void _render_volumes_for_picking(const Camera& camera) const;
    void _render_current_gizmo() const;
    void _render_gizmos_overlay();
    void _render_main_toolbar();
    void _render_imgui_select_plate_toolbar();
    void _render_assemble_view_toolbar() const;
    void _render_return_toolbar() const;
    void _render_canvas_toolbar();
    void _render_separator_toolbar_right() const;
    void _render_separator_toolbar_left() const;
    void _render_collapse_toolbar() const;
    // BBS
    //void _render_view_toolbar() const;
    void _render_paint_toolbar() const;
    float _show_assembly_tooltip_information(float caption_max, float x, float y) const;
    void _render_assemble_control();
    void _render_assemble_info() const;
#if ENABLE_SHOW_CAMERA_TARGET
    void _render_camera_target();
#endif // ENABLE_SHOW_CAMERA_TARGET
    void _render_sla_slices();
    void _render_selection_sidebar_hints();
    //BBS: GUI refactor: adjust main toolbar position
    bool _render_orient_menu(float left, float right, float bottom, float top);
    bool _render_arrange_menu(float left, float right, float bottom, float top);
    void _render_3d_navigator();

    void _update_volumes_hover_state();

    // Convert the screen space coordinate to world coordinate on the bed.
    Vec3d _mouse_to_bed_3d(const Point& mouse_pos);

    void _start_timer();
    void _stop_timer();

    // Create 3D thick extrusion lines for a skirt and brim.
    // Adds a new Slic3r::GUI::3DScene::Volume to volumes, updates collision with the build_volume.
    void _load_print_toolpaths(const BuildVolume &build_volume);
    // Create 3D thick extrusion lines for object forming extrusions.
    // Adds a new Slic3r::GUI::3DScene::Volume to $self->volumes,
    // one for perimeters, one for infill and one for supports, updates collision with the build_volume.
    void _load_print_object_toolpaths(const PrintObject& print_object, const BuildVolume &build_volume,
        const std::vector<std::string>& str_tool_colors, const std::vector<CustomGCode::Item>& color_print_values);
    // Create 3D thick extrusion lines for wipe tower extrusions, updates collision with the build_volume.
    void _load_wipe_tower_toolpaths(const BuildVolume &build_volume, const std::vector<std::string>& str_tool_colors);

    // Load SLA objects and support structures for objects, for which the slaposSliceSupports step has been finished.
	void _load_sla_shells();
    void _update_sla_shells_outside_state();
    void _set_warning_notification_if_needed(EWarning warning);

    //BBS: add partplate print volume get function
    BoundingBoxf3 _get_current_partplate_print_volume();

    // generates a warning notification containing the given message
    void _set_warning_notification(EWarning warning, bool state);

    bool is_flushing_matrix_error();
    bool _is_any_volume_outside() const;

    // updates the selection from the content of m_hover_volume_idxs
    void _update_selection_from_hover();

    bool _deactivate_collapse_toolbar_items();
    bool _deactivate_arrange_menu();
    //BBS: add deactivate_orient_menu
    bool _deactivate_orient_menu();
    //BBS: add _deactivate_layersediting_menu
    bool _deactivate_layersediting_menu();

    // BBS FIXME
    float get_overlay_window_width() { return 0; /*LayersEditing::get_overlay_window_width();*/ }
};

const ModelVolume *get_model_volume(const GLVolume &v, const Model &model);
ModelVolume *get_model_volume(const ObjectID &volume_id, const ModelObjectPtrs &objects);
ModelVolume *get_model_volume(const GLVolume &v, const ModelObjectPtrs &objects);
ModelVolume *get_model_volume(const GLVolume &v, const ModelObject &object);

GLVolume *get_first_hovered_gl_volume(const GLCanvas3D &canvas);
GLVolume *get_selected_gl_volume(const GLCanvas3D &canvas);

ModelObject *get_model_object(const GLVolume &gl_volume, const Model &model);
ModelObject *get_model_object(const GLVolume &gl_volume, const ModelObjectPtrs &objects);

ModelInstance *get_model_instance(const GLVolume &gl_volume, const Model &model);
ModelInstance *get_model_instance(const GLVolume &gl_volume, const ModelObjectPtrs &objects);
ModelInstance *get_model_instance(const GLVolume &gl_volume, const ModelObject &object);

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLCanvas3D_hpp_
