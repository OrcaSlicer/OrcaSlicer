#ifndef slic3r_GCodeViewer_hpp_
#define slic3r_GCodeViewer_hpp_

#include "3DScene.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "IMSlider.hpp"
#include "GLModel.hpp"
#include "I18N.hpp"

#include <boost/iostreams/device/mapped_file.hpp>

#include <cstdint>
#include <float.h>
#include <set>
#include <unordered_set>

namespace Slic3r {

class Print;
class TriangleMesh;
class PresetBundle;

namespace GUI {

class PartPlateList;
class OpenGLManager;

static const float GCODE_VIEWER_SLIDER_SCALE = 0.6f;
static const float SLIDER_DEFAULT_RIGHT_MARGIN  = 10.0f;
static const float SLIDER_DEFAULT_BOTTOM_MARGIN = 10.0f;
static const float SLIDER_RIGHT_MARGIN = 124.0f;
static const float SLIDER_BOTTOM_MARGIN = 64.0f;
class GCodeViewer
{
    using IBufferType = unsigned short;
    using VertexBuffer = std::vector<float>;
    using MultiVertexBuffer = std::vector<VertexBuffer>;
    using IndexBuffer = std::vector<IBufferType>;
    using MultiIndexBuffer = std::vector<IndexBuffer>;
    using InstanceBuffer = std::vector<float>;
    using InstanceIdBuffer = std::vector<size_t>;
    using InstancesOffsets = std::vector<Vec3f>;

    static const std::vector<ColorRGBA> Extrusion_Role_Colors;
    static const std::vector<ColorRGBA> Options_Colors;
    static const std::vector<ColorRGBA> Travel_Colors;
    static const std::vector<ColorRGBA> Range_Colors;
    static const ColorRGBA              Wipe_Color;
    static const ColorRGBA              Neutral_Color;

    enum class EOptionsColors : unsigned char
    {
        Retractions,
        Unretractions,
        Seams,
        ToolChanges,
        ColorChanges,
        PausePrints,
        CustomGCodes
    };

    // vbo buffer containing vertices data used to render a specific toolpath type
    struct VBuffer
    {
        enum class EFormat : unsigned char
        {
            // vertex format: 3 floats -> position.x|position.y|position.z
            Position,
            // vertex format: 4 floats -> position.x|position.y|position.z|normal.x
            PositionNormal1,
            // vertex format: 6 floats -> position.x|position.y|position.z|normal.x|normal.y|normal.z
            PositionNormal3
        };

        EFormat format{ EFormat::Position };
        // vbos id
        std::vector<unsigned int> vbos;
        // sizes of the buffers, in bytes, used in export to obj
        std::vector<size_t> sizes;
        // count of vertices, updated after data are sent to gpu
        size_t count{ 0 };

        size_t data_size_bytes() const { return count * vertex_size_bytes(); }
        // We set 65536 as max count of vertices inside a vertex buffer to allow
        // to use unsigned short in place of unsigned int for indices in the index buffer, to save memory
        size_t max_size_bytes() const { return 65536 * vertex_size_bytes(); }

        size_t vertex_size_floats() const { return position_size_floats() + normal_size_floats(); }
        size_t vertex_size_bytes() const { return vertex_size_floats() * sizeof(float); }

        size_t position_offset_floats() const { return 0; }
        size_t position_offset_bytes() const { return position_offset_floats() * sizeof(float); }

        size_t position_size_floats() const { return 3; }
        size_t position_size_bytes() const { return position_size_floats() * sizeof(float); }

        size_t normal_offset_floats() const {
            assert(format == EFormat::PositionNormal1 || format == EFormat::PositionNormal3);
            return position_size_floats();
        }
        size_t normal_offset_bytes() const { return normal_offset_floats() * sizeof(float); }

        size_t normal_size_floats() const {
            switch (format)
            {
            case EFormat::PositionNormal1: { return 1; }
            case EFormat::PositionNormal3: { return 3; }
            default:                       { return 0; }
            }
        }
        size_t normal_size_bytes() const { return normal_size_floats() * sizeof(float); }

        void reset();
    };

    // buffer containing instances data used to render a toolpaths using instanced or batched models
    // instance record format:
    // instanced models: 5 floats -> position.x|position.y|position.z|width|height (which are sent to the shader as -> vec3 (offset) + vec2 (scales) in GLModel::render_instanced())
    // batched models:   3 floats -> position.x|position.y|position.z
    struct InstanceVBuffer
    {
        // ranges used to render only subparts of the intances
        struct Ranges
        {
            struct Range
            {
                // offset in bytes of the 1st instance to render
                unsigned int offset;
                // count of instances to render
                unsigned int count;
                // vbo id
                unsigned int vbo{ 0 };
                // Color to apply to the instances
                ColorRGBA color;
            };

            std::vector<Range> ranges;

            void reset();
        };

        enum class EFormat : unsigned char
        {
            InstancedModel,
            BatchedModel
        };

        EFormat format;

        // cpu-side buffer containing all instances data
        InstanceBuffer buffer;
        // indices of the moves for all instances
        std::vector<size_t> s_ids;
        // position offsets, used to show the correct value of the tool position
        InstancesOffsets offsets;
        Ranges render_ranges;

        size_t data_size_bytes() const { return s_ids.size() * instance_size_bytes(); }

        size_t instance_size_floats() const {
            switch (format)
            {
            case EFormat::InstancedModel: { return 5; }
            case EFormat::BatchedModel: { return 3; }
            default: { return 0; }
            }
        }
        size_t instance_size_bytes() const { return instance_size_floats() * sizeof(float); }

        void reset();
    };

    // ibo buffer containing indices data (for lines/triangles) used to render a specific toolpath type
    struct IBuffer
    {
        // id of the associated vertex buffer
        unsigned int vbo{ 0 };
        // ibo id
        unsigned int ibo{ 0 };
        // count of indices, updated after data are sent to gpu
        size_t count{ 0 };

        void reset();
    };

    // Used to identify different toolpath sub-types inside a IBuffer
    struct Path
    {
        struct Endpoint
        {
            // index of the buffer in the multibuffer vector
            // the buffer type may change:
            // it is the vertex buffer while extracting vertices data,
            // the index buffer while extracting indices data
            unsigned int b_id{ 0 };
            // index into the buffer
            size_t i_id{ 0 };
            // move id
            size_t s_id{ 0 };
            Vec3f position{ Vec3f::Zero() };
        };

        struct Sub_Path
        {
            Endpoint first;
            Endpoint last;

            bool contains(size_t s_id) const {
                return first.s_id <= s_id && s_id <= last.s_id;
            }
        };

        EMoveType type{ EMoveType::Noop };
        ExtrusionRole role{ erNone };
        float delta_extruder{ 0.0f };
        float height{ 0.0f };
        float width{ 0.0f };
        float feedrate{ 0.0f };
        float fan_speed{ 0.0f };
        float temperature{ 0.0f };
        float volumetric_rate{ 0.0f };
        float layer_time{ 0.0f };
        unsigned char extruder_id{ 0 };
        unsigned char cp_color_id{ 0 };
        std::vector<Sub_Path> sub_paths;

        bool matches(const GCodeProcessorResult::MoveVertex& move) const;
        size_t vertices_count() const {
            return sub_paths.empty() ? 0 : sub_paths.back().last.s_id - sub_paths.front().first.s_id + 1;
        }
        bool contains(size_t s_id) const {
            return sub_paths.empty() ? false : sub_paths.front().first.s_id <= s_id && s_id <= sub_paths.back().last.s_id;
        }
        int get_id_of_sub_path_containing(size_t s_id) const {
            if (sub_paths.empty())
                return -1;
            else {
                for (int i = 0; i < static_cast<int>(sub_paths.size()); ++i) {
                    if (sub_paths[i].contains(s_id))
                        return i;
                }
                return -1;
            }
        }
        void add_sub_path(const GCodeProcessorResult::MoveVertex& move, unsigned int b_id, size_t i_id, size_t s_id) {
            Endpoint endpoint = { b_id, i_id, s_id, move.position };
            sub_paths.push_back({ endpoint , endpoint });
        }
    };

    // Used to batch the indices needed to render the paths
    struct RenderPath
    {
        // Index of the parent tbuffer
        unsigned char               tbuffer_id;
        // Render path property
        ColorRGBA                       color;
        // Index of the buffer in TBuffer::indices
        unsigned int                ibuffer_id;
        // Render path content
        // Index of the path in TBuffer::paths
        unsigned int                path_id;
        std::vector<unsigned int>   sizes;
        std::vector<size_t>         offsets; // use size_t because we need an unsigned integer whose size matches pointer's size (used in the call glMultiDrawElements())
        bool contains(size_t offset) const {
            for (size_t i = 0; i < offsets.size(); ++i) {
                if (offsets[i] <= offset && offset <= offsets[i] + static_cast<size_t>(sizes[i] * sizeof(IBufferType)))
                    return true;
            }
            return false;
        }
    };
    struct RenderPathPropertyLower {
        bool operator() (const RenderPath &l, const RenderPath &r) const {
            if (l.tbuffer_id < r.tbuffer_id)
                return true;
            if (l.color < r.color)
                return true;
            else if (l.color > r.color)
                return false;
            return l.ibuffer_id < r.ibuffer_id;
        }
    };
    struct RenderPathPropertyEqual {
        bool operator() (const RenderPath &l, const RenderPath &r) const {
            return l.tbuffer_id == r.tbuffer_id && l.ibuffer_id == r.ibuffer_id && l.color == r.color;
        }
    };

    // buffer containing data for rendering a specific toolpath type
    struct TBuffer
    {
        enum class ERenderPrimitiveType : unsigned char
        {
            Line,
            Triangle,
            InstancedModel,
            BatchedModel
        };

        ERenderPrimitiveType render_primitive_type;

        // buffers for point, line and triangle primitive types
        VBuffer vertices;
        std::vector<IBuffer> indices;

        struct Model
        {
            GLModel model;
            ColorRGBA color;
            InstanceVBuffer instances;
            GLModel::Geometry data;

            void reset();
        };

        // contain the buffer for model primitive types
        Model model;

        std::string shader;
        std::vector<Path> paths;
        std::vector<RenderPath> render_paths;
        bool visible{ false };

        void reset();

        // b_id index of buffer contained in this->indices
        // i_id index of first index contained in this->indices[b_id]
        // s_id index of first vertex contained in this->vertices
        void add_path(const GCodeProcessorResult::MoveVertex& move, unsigned int b_id, size_t i_id, size_t s_id);

        unsigned int max_vertices_per_segment() const {
            switch (render_primitive_type)
            {
            case ERenderPrimitiveType::Line:     { return 2; }
            case ERenderPrimitiveType::Triangle: { return 8; }
            default:                             { return 0; }
            }
        }

        size_t max_vertices_per_segment_size_floats() const { return vertices.vertex_size_floats() * static_cast<size_t>(max_vertices_per_segment()); }
        size_t max_vertices_per_segment_size_bytes() const { return max_vertices_per_segment_size_floats() * sizeof(float); }
        unsigned int indices_per_segment() const {
            switch (render_primitive_type)
            {
            case ERenderPrimitiveType::Line:     { return 2; }
            case ERenderPrimitiveType::Triangle: { return 30; } // 3 indices x 10 triangles
            default:                             { return 0; }
            }
        }
        size_t indices_per_segment_size_bytes() const { return static_cast<size_t>(indices_per_segment() * sizeof(IBufferType)); }
        unsigned int max_indices_per_segment() const {
            switch (render_primitive_type)
            {
            case ERenderPrimitiveType::Line:     { return 2; }
            case ERenderPrimitiveType::Triangle: { return 36; } // 3 indices x 12 triangles
            default:                             { return 0; }
            }
        }
        size_t max_indices_per_segment_size_bytes() const { return max_indices_per_segment() * sizeof(IBufferType); }

        bool has_data() const {
            switch (render_primitive_type)
            {
            case ERenderPrimitiveType::Line:
            case ERenderPrimitiveType::Triangle: {
                return !vertices.vbos.empty() && vertices.vbos.front() != 0 && !indices.empty() && indices.front().ibo != 0;
            }
            case ERenderPrimitiveType::InstancedModel: { return model.model.is_initialized() && !model.instances.buffer.empty(); }
            case ERenderPrimitiveType::BatchedModel: {
                return !model.data.vertices.empty() && !model.data.indices.empty() &&
                    !vertices.vbos.empty() && vertices.vbos.front() != 0 && !indices.empty() && indices.front().ibo != 0;
            }
            default: { return false; }
            }
        }
    };


    // helper to render extrusion paths
    struct Extrusions
    {
        struct Range
        {
            float min;
            float max;
            unsigned int count;
            bool log_scale;

            Range() { reset(); }
            void update_from(const float value) {
                if (value != max && value != min)
                    ++count;
                min = std::min(min, value);
                max = std::max(max, value);
            }
            void reset(bool log = false) { min = FLT_MAX; max = -FLT_MAX; count = 0; log_scale = log; }

            float step_size() const;
            ColorRGBA get_color_at(float value) const;
            float get_value_at_step(int step) const;

        };

        struct Ranges
        {
            // Color mapping by layer height.
            Range height;
            // Color mapping by extrusion width.
            Range width;
            // Color mapping by feedrate.
            Range feedrate;
            // Color mapping by fan speed.
            Range fan_speed;
            // Color mapping by volumetric extrusion rate.
            Range volumetric_rate;
            // Color mapping by extrusion temperature.
            Range temperature;
            // Color mapping by layer time.
            Range layer_duration;
Range layer_duration_log;
            void reset() {
                height.reset();
                width.reset();
                feedrate.reset();
                fan_speed.reset();
                volumetric_rate.reset();
                temperature.reset();
                layer_duration.reset();
                layer_duration_log.reset(true);
            }
        };

        unsigned int role_visibility_flags{ 0 };
        Ranges ranges;

        void reset_role_visibility_flags() {
            role_visibility_flags = 0;
            for (unsigned int i = 0; i < erCount; ++i) {
                role_visibility_flags |= 1 << i;
            }
        }

        void reset_ranges() { ranges.reset(); }
    };

    class Layers
    {
    public:
        struct Endpoints
        {
            size_t first{ 0 };
            size_t last{ 0 };

            bool operator == (const Endpoints& other) const { return first == other.first && last == other.last; }
            bool operator != (const Endpoints& other) const { return !operator==(other); }
        };

    private:
        std::vector<double> m_zs;
        std::vector<Endpoints> m_endpoints;

    public:
        void append(double z, Endpoints endpoints) {
            m_zs.emplace_back(z);
            m_endpoints.emplace_back(endpoints);
        }

        void reset() {
            m_zs = std::vector<double>();
            m_endpoints = std::vector<Endpoints>();
        }

        size_t size() const { return m_zs.size(); }
        bool empty() const { return m_zs.empty(); }
        const std::vector<double>& get_zs() const { return m_zs; }
        const std::vector<Endpoints>& get_endpoints() const { return m_endpoints; }
        std::vector<Endpoints>& get_endpoints() { return m_endpoints; }
        double get_z_at(unsigned int id) const { return (id < m_zs.size()) ? m_zs[id] : 0.0; }
        Endpoints get_endpoints_at(unsigned int id) const { return (id < m_endpoints.size()) ? m_endpoints[id] : Endpoints(); }
        int                           get_l_at(float z) const
        {
            auto iter = std::upper_bound(m_zs.begin(), m_zs.end(), z);
            return std::distance(m_zs.begin(), iter);
        }

        bool operator != (const Layers& other) const {
            if (m_zs != other.m_zs)
                return true;
            if (m_endpoints != other.m_endpoints)
                return true;
            return false;
        }
    };

    // used to render the toolpath caps of the current sequential range
    // (i.e. when sliding on the horizontal slider)
    struct SequentialRangeCap
    {
        TBuffer* buffer{ nullptr };
        unsigned int ibo{ 0 };
        unsigned int vbo{ 0 };
        ColorRGBA color;

        ~SequentialRangeCap();
        bool is_renderable() const { return buffer != nullptr; }
        void reset();
        size_t indices_count() const { return 6; }
    };

#if ENABLE_GCODE_VIEWER_STATISTICS
    struct Statistics
    {
        // time
        int64_t results_time{ 0 };
        int64_t load_time{ 0 };
        int64_t load_vertices{ 0 };
        int64_t smooth_vertices{ 0 };
        int64_t load_indices{ 0 };
        int64_t refresh_time{ 0 };
        int64_t refresh_paths_time{ 0 };
        // opengl calls
        int64_t gl_multi_lines_calls_count{ 0 };
        int64_t gl_multi_triangles_calls_count{ 0 };
        int64_t gl_triangles_calls_count{ 0 };
        int64_t gl_instanced_models_calls_count{ 0 };
        int64_t gl_batched_models_calls_count{ 0 };
        // memory
        int64_t results_size{ 0 };
        int64_t total_vertices_gpu_size{ 0 };
        int64_t total_indices_gpu_size{ 0 };
        int64_t total_instances_gpu_size{ 0 };
        int64_t max_vbuffer_gpu_size{ 0 };
        int64_t max_ibuffer_gpu_size{ 0 };
        int64_t paths_size{ 0 };
        int64_t render_paths_size{ 0 };
        int64_t models_instances_size{ 0 };
        // other
        int64_t travel_segments_count{ 0 };
        int64_t wipe_segments_count{ 0 };
        int64_t extrude_segments_count{ 0 };
        int64_t instances_count{ 0 };
        int64_t batched_count{ 0 };
        int64_t vbuffers_count{ 0 };
        int64_t ibuffers_count{ 0 };

        void reset_all() {
            reset_times();
            reset_opengl();
            reset_sizes();
            reset_others();
        }

        void reset_times() {
            results_time = 0;
            load_time = 0;
            load_vertices = 0;
            smooth_vertices = 0;
            load_indices = 0;
            refresh_time = 0;
            refresh_paths_time = 0;
        }

        void reset_opengl() {
            gl_multi_lines_calls_count = 0;
            gl_multi_triangles_calls_count = 0;
            gl_triangles_calls_count = 0;
            gl_instanced_models_calls_count = 0;
            gl_batched_models_calls_count = 0;
        }

        void reset_sizes() {
            results_size = 0;
            total_vertices_gpu_size = 0;
            total_indices_gpu_size = 0;
            total_instances_gpu_size = 0;
            max_vbuffer_gpu_size = 0;
            max_ibuffer_gpu_size = 0;
            paths_size = 0;
            render_paths_size = 0;
            models_instances_size = 0;
        }

        void reset_others() {
            travel_segments_count = 0;
            wipe_segments_count = 0;
            extrude_segments_count = 0;
            instances_count = 0;
            batched_count = 0;
            vbuffers_count = 0;
            ibuffers_count = 0;
        }
    };
#endif // ENABLE_GCODE_VIEWER_STATISTICS

public:
    enum class EViewType : unsigned char;
    struct SequentialView
    {
        class Marker
        {
            GLModel m_model;
            Vec3f m_world_position;
            Transform3f m_world_transform;
            // for seams, the position of the marker is on the last endpoint of the toolpath containing it
            // the offset is used to show the correct value of tool position in the "ToolPosition" window
            // see implementation of render() method
            Vec3f m_world_offset;
            float m_z_offset{ 0.5f };
            GCodeProcessorResult::MoveVertex m_curr_move;
            bool m_visible{ true };
            bool m_is_dark = false;

        public:
            float m_scale = 1.0f;

            void init(std::string filename);

            const BoundingBoxf3& get_bounding_box() const { return m_model.get_bounding_box(); }

            void set_world_position(const Vec3f& position);
            void set_world_offset(const Vec3f& offset) { m_world_offset = offset; }

            bool is_visible() const { return m_visible; }
            void set_visible(bool visible) { m_visible = visible; }

            void render(int canvas_width, int canvas_height, const EViewType& view_type);
            void on_change_color_mode(bool is_dark) { m_is_dark = is_dark; }

            void update_curr_move(const GCodeProcessorResult::MoveVertex move);
        };

        class GCodeWindow
        {
            struct Line
            {
                std::string command;
                std::string parameters;
                std::string comment;
            };
            bool m_is_dark = false;
            uint64_t m_selected_line_id{ 0 };
            size_t m_last_lines_size{ 0 };
            std::string m_filename;
            boost::iostreams::mapped_file_source m_file;
            // map for accessing data in file by line number
            std::vector<size_t> m_lines_ends;
            // current visible lines
            std::vector<Line> m_lines;

        public:
            float m_scale = 1.0f;
            GCodeWindow() = default;
            ~GCodeWindow() { stop_mapping_file(); }
            void load_gcode(const std::string& filename, const std::vector<size_t> &lines_ends);
            void reset() {
                stop_mapping_file();
                m_lines_ends.clear();
                m_lines_ends.shrink_to_fit();
                m_lines.clear();
                m_lines.shrink_to_fit();
                m_filename.clear();
                m_filename.shrink_to_fit();
            }

            //BBS: GUI refactor: add canvas size
            //void render(float top, float bottom, uint64_t curr_line_id) const;
            void render(float top, float bottom, float right, uint64_t curr_line_id) const;
            void on_change_color_mode(bool is_dark) { m_is_dark = is_dark; }

            void stop_mapping_file();
        };

        struct Endpoints
        {
            size_t first{ 0 };
            size_t last{ 0 };
        };

        bool skip_invisible_moves{ false };
        Endpoints endpoints;
        Endpoints current;
        Endpoints last_current;
        Endpoints global;
        Vec3f current_position{ Vec3f::Zero() };
        Vec3f current_offset{ Vec3f::Zero() };
        Marker marker;
        GCodeWindow gcode_window;
        std::vector<unsigned int> gcode_ids;
        float m_scale = 1.0;
        bool m_show_marker = false;
        void render(const bool has_render_path, float legend_height, int canvas_width, int canvas_height, int right_margin, const EViewType& view_type);
    };

    struct ETools
    {
        std::vector<ColorRGBA> m_tool_colors;
        std::vector<bool>  m_tool_visibles;
    };

    struct ExtruderFilament
    {
        std::string   type;
        std::string   hex_color;
        unsigned char filament_id;
        bool is_support_filament;
    };

    enum class EViewType : unsigned char
    {
        Summary = 0,
        FeatureType,
        Height,
        Width,
        Feedrate,
        FanSpeed,
        Temperature,
        VolumetricRate,
        Tool,
        ColorPrint,
        FilamentId,
        LayerTime,
        LayerTimeLog,
        Count
    };
    // helper to render shells
    struct Shells
    {
        GLVolumeCollection volumes;
        bool               visible{false};
        // BBS: always load shell when preview
        int  print_id{-1};
        int  print_modify_count{-1};
        bool previewing{false};
    };
    //BBS
    ConflictResultOpt m_conflict_result;
    GCodeCheckResult  m_gcode_check_result;
    FilamentPrintableResult filament_printable_reuslt;
    Shells            m_shells;

private:
    std::vector<int> m_plater_extruder;
    bool m_gl_data_initialized{ false };
    unsigned int m_last_result_id{ 0 };
    size_t m_moves_count{ 0 };
    //BBS: save m_gcode_result as well
    const GCodeProcessorResult* m_gcode_result;
    //BBS: add only gcode mode
    bool m_only_gcode_in_preview {false};
    std::vector<size_t> m_ssid_to_moveid_map;

    //BBS: extruder dispensing filament
    std::vector<ExtruderFilament> m_left_extruder_filament;
    std::vector<ExtruderFilament> m_right_extruder_filament;
    size_t m_nozzle_nums;

    std::vector<TBuffer> m_buffers{ static_cast<size_t>(EMoveType::Extrude) };
    // bounding box of toolpaths
    BoundingBoxf3 m_paths_bounding_box;
    // bounding box of toolpaths + marker tools
    BoundingBoxf3 m_max_bounding_box;
    //BBS: add shell bounding box
    BoundingBoxf3 m_shell_bounding_box;
    float m_max_print_height{ 0.0f };

    //BBS save m_tools_color and m_tools_visible
    ETools m_tools;
    ConfigOptionMode m_user_mode;
    bool m_fold = {false};

    Layers m_layers;
    std::array<unsigned int, 2> m_layers_z_range;
    std::vector<ExtrusionRole> m_roles;
    size_t m_extruders_count;
    std::vector<unsigned char> m_extruder_ids;
    std::vector<float> m_filament_diameters;
    std::vector<float> m_filament_densities;
    Extrusions m_extrusions;
    SequentialView m_sequential_view;
    IMSlider* m_moves_slider;
    IMSlider* m_layers_slider;

    /*BBS GUI refactor, store displayed items in color scheme combobox */
    std::vector<EViewType> view_type_items;
    std::vector<std::string> view_type_items_str;
    int       m_view_type_sel = 0;
    EViewType m_view_type{ EViewType::FeatureType };
    std::vector<EMoveType> options_items;

    bool m_legend_enabled{ true };
    float m_legend_height;
    PrintEstimatedStatistics m_print_statistics;
    PrintEstimatedStatistics::ETimeMode m_time_estimate_mode{ PrintEstimatedStatistics::ETimeMode::Normal };
#if ENABLE_GCODE_VIEWER_STATISTICS
    Statistics m_statistics;
#endif // ENABLE_GCODE_VIEWER_STATISTICS
    std::array<float, 2> m_detected_point_sizes = { 0.0f, 0.0f };
    GCodeProcessorResult::SettingsIds m_settings_ids;
    std::array<SequentialRangeCap, 2> m_sequential_range_caps;

    std::vector<CustomGCode::Item> m_custom_gcode_per_print_z;

    bool m_contained_in_bed{ true };
mutable bool m_no_render_path { false };
    bool m_is_dark = false;

public:
    GCodeViewer();
    ~GCodeViewer();

    void on_change_color_mode(bool is_dark);
    float m_scale = 1.0;
    void set_scale(float scale = 1.0);
    void init(ConfigOptionMode mode, Slic3r::PresetBundle* preset_bundle);
    void update_by_mode(ConfigOptionMode mode);

    // extract rendering data from the given parameters
    //BBS: add only gcode mode
    void load(const GCodeProcessorResult& gcode_result, const Print& print, const BuildVolume& build_volume,
            const std::vector<BoundingBoxf3>& exclude_bounding_box, ConfigOptionMode mode, bool only_gcode = false);
    // recalculate ranges in dependence of what is visible and sets tool/print colors
    void refresh(const GCodeProcessorResult& gcode_result, const std::vector<std::string>& str_tool_colors);
    void refresh_render_paths();
    void update_shells_color_by_extruder(const DynamicPrintConfig* config);
    void set_shell_transparency(float alpha = 0.15f);

    void reset();
    //BBS: always load shell at preview
    void reset_shell();
    void load_shells(const Print& print, bool initialized, bool force_previewing = false);
    void set_shells_on_preview(bool is_previewing) { m_shells.previewing = is_previewing; }
    //BBS: add all plates filament statistics
    void render_all_plates_stats(const std::vector<const GCodeProcessorResult*>& gcode_result_list, bool show = true) const;
    //BBS: GUI refactor: add canvas width and height
    void render(int canvas_width, int canvas_height, int right_margin);
    //BBS
    void _render_calibration_thumbnail_internal(ThumbnailData& thumbnail_data, const ThumbnailsParams& thumbnail_params, PartPlateList& partplate_list, OpenGLManager& opengl_manager);
    void _render_calibration_thumbnail_framebuffer(ThumbnailData& thumbnail_data, unsigned int w, unsigned int h, const ThumbnailsParams& thumbnail_params, PartPlateList& partplate_list, OpenGLManager& opengl_manager);
    void render_calibration_thumbnail(ThumbnailData& thumbnail_data, unsigned int w, unsigned int h, const ThumbnailsParams& thumbnail_params, PartPlateList& partplate_list, OpenGLManager& opengl_manager);

    bool has_data() const { return !m_roles.empty(); }
    bool can_export_toolpaths() const;
    std::vector<int> get_plater_extruder();

    const float                get_max_print_height() const { return m_max_print_height; }
    const BoundingBoxf3& get_paths_bounding_box() const { return m_paths_bounding_box; }
    const BoundingBoxf3& get_max_bounding_box() const { return m_max_bounding_box; }
    const BoundingBoxf3& get_shell_bounding_box() const { return m_shell_bounding_box; }
    const std::vector<double>& get_layers_zs() const { return m_layers.get_zs(); }
    const std::array<unsigned int,2> &get_layers_z_range() const { return m_layers_z_range; }

    const SequentialView& get_sequential_view() const { return m_sequential_view; }
    void update_sequential_view_current(unsigned int first, unsigned int last);

    /* BBS IMSlider */
    IMSlider *get_moves_slider() { return m_moves_slider; }
    IMSlider *get_layers_slider() { return m_layers_slider; }
    void enable_moves_slider(bool enable) const;
    void update_moves_slider(bool set_to_max = false);
    void update_layers_slider_mode();
    void update_marker_curr_move();

    bool is_contained_in_bed() const { return m_contained_in_bed; }
    //BBS: add only gcode mode
    bool is_only_gcode_in_preview() const { return m_only_gcode_in_preview; }

    EViewType get_view_type() const { return m_view_type; }
    void set_view_type(EViewType type, bool reset_feature_type_visible = true) {
        if (type == EViewType::Count)
            type = EViewType::FeatureType;
        m_view_type = (EViewType)type;
        if (reset_feature_type_visible && type == EViewType::ColorPrint) {
            reset_visible(EViewType::FeatureType);
        }
    }
    void reset_visible(EViewType type) {
        if (type == EViewType::FeatureType) {
            for (size_t i = 0; i < m_roles.size(); ++i) {
                ExtrusionRole role = m_roles[i];
                m_extrusions.role_visibility_flags = m_extrusions.role_visibility_flags | (1 << role);
            }
        } else if (type == EViewType::ColorPrint){
            for(auto item: m_tools.m_tool_visibles) item = true;
        }
    }

    bool is_toolpath_move_type_visible(EMoveType type) const;
    void set_toolpath_move_type_visible(EMoveType type, bool visible);
    unsigned int get_toolpath_role_visibility_flags() const { return m_extrusions.role_visibility_flags; }
    void set_toolpath_role_visibility_flags(unsigned int flags) { m_extrusions.role_visibility_flags = flags; }
    unsigned int get_options_visibility_flags() const;
    void set_options_visibility_from_flags(unsigned int flags);
    void set_layers_z_range(const std::array<unsigned int, 2>& layers_z_range);

    bool is_legend_enabled() const { return m_legend_enabled; }
    void enable_legend(bool enable) { m_legend_enabled = enable; }
    float get_legend_height() { return m_legend_height; }

    void export_toolpaths_to_obj(const char* filename) const;

    std::vector<CustomGCode::Item>& get_custom_gcode_per_print_z() { return m_custom_gcode_per_print_z; }
    size_t get_extruders_count() { return m_extruders_count; }
    void push_combo_style();
    void pop_combo_style();

private:
    void load_toolpaths(const GCodeProcessorResult& gcode_result, const BuildVolume& build_volume, const std::vector<BoundingBoxf3>& exclude_bounding_box);
    //BBS: always load shell at preview
    //void load_shells(const Print& print);
    void refresh_render_paths(bool keep_sequential_current_first, bool keep_sequential_current_last) const;
    void render_toolpaths();
    void render_shells(int canvas_width, int canvas_height);

    //BBS: GUI refactor: add canvas size
    void render_legend(float &legend_height, int canvas_width, int canvas_height, int right_margin);
    void render_legend_color_arr_recommen(float window_padding);
    void render_slider(int canvas_width, int canvas_height);

#if ENABLE_GCODE_VIEWER_STATISTICS
    void render_statistics();
#endif // ENABLE_GCODE_VIEWER_STATISTICS
    bool is_visible(ExtrusionRole role) const {
        return role < erCount && (m_extrusions.role_visibility_flags & (1 << role)) != 0;
    }
    bool is_visible(const Path& path) const { return is_visible(path.role); }
    void log_memory_used(const std::string& label, int64_t additional = 0) const;
    ColorRGBA option_color(EMoveType move_type) const;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GCodeViewer_hpp_

