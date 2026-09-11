///|/ Copyright (c) Prusa Research 2023 Enrico Turri @enricoturri1966, Pavel Mikuš @Godrak
///|/
///|/ libvgcode is released under the terms of the AGPLv3 or higher
///|/
#ifndef VGCODE_VIEWERIMPL_HPP
#define VGCODE_VIEWERIMPL_HPP

#include "Settings.hpp"
#include "SegmentTemplate.hpp"
#include "OptionTemplate.hpp"
#if VGCODE_ENABLE_COG_AND_TOOL_MARKERS
#include "CogMarker.hpp"
#include "ToolMarker.hpp"
#endif // VGCODE_ENABLE_COG_AND_TOOL_MARKERS
#include "../include/PathVertex.hpp"
#include "../include/ColorRange.hpp"
#include "../include/ColorPrint.hpp"
#include "Bitset.hpp"
#include "ViewRange.hpp"
#include "Layers.hpp"
#include "ExtrusionRoles.hpp"

#include <string>
#include <optional>

namespace libvgcode {

struct GCodeInputData;

class ViewerImpl
{
public:
    ViewerImpl();
    ~ViewerImpl() { shutdown(); }
    ViewerImpl(const ViewerImpl& other) = delete;
    ViewerImpl(ViewerImpl&& other) = delete;
    ViewerImpl& operator = (const ViewerImpl& other) = delete;
    ViewerImpl& operator = (ViewerImpl&& other) = delete;

    //
    // Initialize shaders, uniform indices and segment geometry.
    //
    void init(const std::string& opengl_context_version);
    //
    // Release the resources used by the viewer.
    //
    void shutdown();
    //
    // Reset all caches and free gpu memory.
    //
    void reset();
    //
    // Setup all the variables used for visualization of the toolpaths
    // from the given gcode data.
    //
    void load(GCodeInputData&& gcode_data);

    //
    // Update the visibility property of toolpaths in dependence
    // of the current settings
    //
    void update_enabled_entities();
    //
    // Update the color of toolpaths in dependence of the current
    // view type and settings
    //
    void update_colors();
    void update_colors_texture();

    //
    // Render the toolpaths
    //
    void render(const Mat4x4& view_matrix, const Mat4x4& projection_matrix);

    EViewType get_view_type() const { return m_settings.view_type; }
    void set_view_type(EViewType type);

    ETimeMode get_time_mode() const { return m_settings.time_mode; }
    void set_time_mode(ETimeMode mode);

    const Interval& get_layers_view_range() const { return m_layers.get_view_range(); }
    void set_layers_view_range(const Interval& range) { set_layers_view_range(range[0], range[1]); }
    void set_layers_view_range(Interval::value_type min, Interval::value_type max);

    bool is_top_layer_only_view_range() const { return m_settings.top_layer_only_view_range; }
    void toggle_top_layer_only_view_range();

    // ORCA: darken every layer the layer slider is not scrubbed to, so that only the inspected
    // one(s) - the top thumb's layer, the bottom thumb's layer, or both - stay at full
    // brightness. dim_previous_layers_brightness sets how dark the rest go, 1.0 = unchanged,
    // 0.0 = black
    bool is_dim_previous_layers() const { return m_settings.dim_previous_layers; }
    void set_dim_previous_layers(bool value);
    //
    // Draw the preview from the reduced set of entities. Meant to be held only while the user is
    // dragging; the sets are already built, so this is just a buffer binding and never rebuilds.
    //
    void set_reduced_detail(bool value) { m_settings.reduced_detail = value; }
    bool is_reduced_detail() const { return m_settings.reduced_detail; }
    EReducedDetailMode get_reduced_detail_mode() const { return m_settings.reduced_detail_mode; }
    void set_reduced_detail_mode(EReducedDetailMode mode);
    uint32_t get_reduced_detail_layer_stride() const { return m_settings.reduced_detail_layer_stride; }
    void set_reduced_detail_layer_stride(uint32_t value);
    EReducedDetailMode get_rest_detail_mode() const { return m_settings.rest_detail_mode; }
    void set_rest_detail_mode(EReducedDetailMode mode);
    uint32_t get_rest_layer_stride() const { return m_settings.rest_layer_stride; }
    void set_rest_layer_stride(uint32_t value);
    bool is_rest_view_from_above() const { return m_settings.rest_view_from_above; }
    void set_rest_view_from_above(bool value);
    float get_dim_previous_layers_brightness() const { return m_settings.dim_previous_layers_brightness; }
    void set_dim_previous_layers_brightness(float value);

    bool is_spiral_vase_mode() const { return m_settings.spiral_vase_mode; }

    std::vector<ETimeMode> get_time_modes() const;

    size_t get_layers_count() const { return m_layers.count(); }
    float get_layer_z(size_t layer_id) const { return m_layers.get_layer_z(layer_id); }
    std::vector<float> get_layers_zs() const { return m_layers.get_zs(); }

    size_t get_layer_id_at(float z) const { return m_layers.get_layer_id_at(z); }

    size_t get_used_extruders_count() const { return m_used_extruders.size(); }
    std::vector<uint8_t> get_used_extruders_ids() const;

    size_t get_color_prints_count(uint8_t extruder_id) const;
    std::vector<ColorPrint> get_color_prints(uint8_t extruder_id) const;

    AABox get_bounding_box(const std::vector<EMoveType>& types = {
        EMoveType::Retract, EMoveType::Unretract, EMoveType::Seam, EMoveType::ToolChange,
        EMoveType::ColorChange, EMoveType::PausePrint, EMoveType::CustomGCode, EMoveType::Travel,
        EMoveType::Wipe, EMoveType::Extrude }) const;
    AABox get_extrusion_bounding_box(const std::vector<EGCodeExtrusionRole>& roles = {
        EGCodeExtrusionRole::Perimeter, EGCodeExtrusionRole::ExternalPerimeter, EGCodeExtrusionRole::OverhangPerimeter,
        EGCodeExtrusionRole::InternalInfill, EGCodeExtrusionRole::SolidInfill, EGCodeExtrusionRole::TopSolidInfill,
        EGCodeExtrusionRole::Ironing, EGCodeExtrusionRole::BridgeInfill, EGCodeExtrusionRole::GapFill,
        EGCodeExtrusionRole::Skirt, EGCodeExtrusionRole::SupportMaterial, EGCodeExtrusionRole::SupportMaterialInterface,
        EGCodeExtrusionRole::WipeTower, EGCodeExtrusionRole::Custom,
        // ORCA
        EGCodeExtrusionRole::BottomSurface, EGCodeExtrusionRole::InternalBridgeInfill, EGCodeExtrusionRole::Brim,
        EGCodeExtrusionRole::SupportTransition, EGCodeExtrusionRole::Mixed
    }) const;

    bool is_option_visible(EOptionType type) const;
    void toggle_option_visibility(EOptionType type);

    bool is_extrusion_role_visible(EGCodeExtrusionRole role) const;
    void toggle_extrusion_role_visibility(EGCodeExtrusionRole role);

    const Interval& get_view_full_range() const { return m_view_range.get_full(); }
    const Interval& get_view_enabled_range() const { return m_view_range.get_enabled(); }
    const Interval& get_view_visible_range() const { return m_view_range.get_visible(); }
    void set_view_visible_range(Interval::value_type min, Interval::value_type max);

    size_t get_vertices_count() const { return m_vertices.size(); }
    const PathVertex& get_current_vertex() const { return get_vertex_at(get_current_vertex_id()); }
    size_t get_current_vertex_id() const { return static_cast<size_t>(m_view_range.get_visible()[1]); }
    const PathVertex& get_vertex_at(size_t id) const {
        return (id < m_vertices.size()) ? m_vertices[id] : PathVertex::DUMMY_PATH_VERTEX;
    }
    float get_estimated_time() const { return m_total_time[static_cast<size_t>(m_settings.time_mode)]; }
    float get_estimated_time_at(size_t id) const;
    Color get_vertex_color(const PathVertex& vertex) const;

    size_t get_extrusion_roles_count() const { return m_extrusion_roles.get_roles_count(); }
    std::vector<EGCodeExtrusionRole> get_extrusion_roles() const { return m_extrusion_roles.get_roles(); }
    float get_extrusion_role_estimated_time(EGCodeExtrusionRole role) const { return m_extrusion_roles.get_time(role, m_settings.time_mode); }

    size_t get_options_count() const { return m_options.size(); }
    const std::vector<EOptionType>& get_options() const { return m_options; }

    float get_travels_estimated_time() const { return m_travels_time[static_cast<size_t>(m_settings.time_mode)]; }
    std::vector<float> get_layers_estimated_times() const { return m_layers.get_times(m_settings.time_mode); }

    size_t get_tool_colors_count() const { return m_tool_colors.size(); }
    const Palette& get_tool_colors() const { return m_tool_colors; }
    void set_tool_colors(const Palette& colors);

    size_t get_color_print_colors_count() const { return m_color_print_colors.size(); }
    const Palette& get_color_print_colors() const { return m_color_print_colors; }
    void set_color_print_colors(const Palette& colors);

    const Color& get_extrusion_role_color(EGCodeExtrusionRole role) const;
    void set_extrusion_role_color(EGCodeExtrusionRole role, const Color& color);
    void reset_default_extrusion_roles_colors();

    const Color& get_option_color(EOptionType type) const;
    void set_option_color(EOptionType type, const Color& color);
    void reset_default_options_colors();

    const ColorRange& get_color_range(EViewType type) const;
    void set_color_range_palette(EViewType type, const Palette& palette);

    float get_travels_radius() const { return m_travels_radius; }
    void set_travels_radius(float radius);
    float get_wipes_radius() const { return m_wipes_radius; }
    void set_wipes_radius(float radius);

    size_t get_used_cpu_memory() const;
    size_t get_used_gpu_memory() const;

#if VGCODE_ENABLE_COG_AND_TOOL_MARKERS
    Vec3 get_cog_marker_position() const { return m_cog_marker.get_position(); }

    float get_cog_marker_scale_factor() const { return m_cog_marker_scale_factor; }
    void set_cog_marker_scale_factor(float factor) { m_cog_marker_scale_factor = std::max(factor, 0.001f); }

    const Vec3& get_tool_marker_position() const { return m_tool_marker.get_position(); }

    float get_tool_marker_offset_z() const { return m_tool_marker.get_offset_z(); }
    void set_tool_marker_offset_z(float offset_z) { m_tool_marker.set_offset_z(offset_z); }

    float get_tool_marker_scale_factor() const { return m_tool_marker_scale_factor; }
    void set_tool_marker_scale_factor(float factor) { m_tool_marker_scale_factor = std::max(factor, 0.001f); }

    const Color& get_tool_marker_color() const { return m_tool_marker.get_color(); }
    void set_tool_marker_color(const Color& color) { m_tool_marker.set_color(color); }

    float get_tool_marker_alpha() const { return m_tool_marker.get_alpha(); }
    void set_tool_marker_alpha(float alpha) { m_tool_marker.set_alpha(alpha); }
#endif // VGCODE_ENABLE_COG_AND_TOOL_MARKERS

private:
    //
    // Settings used to render the toolpaths
    //
    Settings m_settings;
    //
    // Detected layers
    //
    Layers m_layers;
    //
    // Detected extrusion roles
    //
    ExtrusionRoles m_extrusion_roles;
    //
    // Detected options
    //
    std::vector<EOptionType> m_options;
    //
    // Detected used extruders ids
    //
    std::map<uint8_t, std::vector<ColorPrint>> m_used_extruders;
    //
    // Vertices ranges for visualization
    //
    ViewRange m_view_range;
    //
    // Detected total moves times
    //
    std::array<float, TIME_MODES_COUNT> m_total_time{ 0.0f, 0.0f };
    //
    // Running sum of the vertex estimated times, one entry per vertex for each time mode.
    // get_estimated_time_at() answers from this instead of re-accumulating the whole vertex
    // array, which it was doing once per frame from the tool marker tooltip. The sums are
    // built by the same left-to-right addition the accumulate performed, so the value handed
    // back is bit-identical, float rounding included.
    //
    std::array<std::vector<float>, TIME_MODES_COUNT> m_cumulative_times;
    //
    // For each layer L, the index of the first vertex whose layer_id is >= L (m_vertices.size()
    // if there is none). Every vertex before it is guaranteed to belong to an earlier layer, so
    // the scan in update_view_full_range() can start there rather than at vertex 0. Derived from
    // the vertices themselves rather than from Layers, which buckets an out-of-order vertex into
    // the layer that happens to be open, so this stays exact whatever order the vertices arrive in.
    //
    std::vector<uint32_t> m_layer_first_vertex;
    //
    // Scratch buffer for update_colors_texture(), kept alive so that a slider drag does not
    // allocate and free one float per vertex of the print on every step.
    //
    std::vector<float> m_colors_scratch;
    //
    // Detected travel moves times
    //
    std::array<float, TIME_MODES_COUNT> m_travels_time{ 0.0f, 0.0f };
    //
    // Radius of cylinders used to render travel moves segments
    //
    float m_travels_radius{ DEFAULT_TRAVELS_RADIUS_MM };
    //
    // Radius of cylinders used to render wipe moves segments
    //
    float m_wipes_radius{ DEFAULT_WIPES_RADIUS_MM };
    //
    // Palette used to render extrusion roles
    //
    std::array<Color, size_t(EGCodeExtrusionRole::COUNT)> m_extrusion_roles_colors;
    //
    // Palette used to render options
    //
    std::array<Color, size_t(EOptionType::COUNT)> m_options_colors;

    bool m_initialized{ false };

    //
    // The OpenGL element used to represent all toolpath segments
    //
    SegmentTemplate m_segment_template;
    //
    // The OpenGL element used to represent all option markers
    //
    OptionTemplate m_option_template;
#if VGCODE_ENABLE_COG_AND_TOOL_MARKERS
    //
    // The OpenGL element used to represent the center of gravity
    //
    CogMarker m_cog_marker;
    float m_cog_marker_scale_factor{ 1.0f };
    //
    // The OpenGL element used to represent the tool nozzle
    //
    ToolMarker m_tool_marker;
    float m_tool_marker_scale_factor{ 1.0f };
#endif // VGCODE_ENABLE_COG_AND_TOOL_MARKERS
    //
    // cpu buffer to store vertices
    //
    std::vector<PathVertex> m_vertices;

    // Cache for the colors to reduce the need to recalculate colors of all the vertices.
    std::vector<float> m_vertices_colors;

    //
    // Variables used for toolpaths visibiliity
    //
    BitSet<> m_valid_lines_bitset;
#ifndef ENABLE_OPENGL_ES
    //
    // ORCA: bit set for the extrusion segments that lie on the visible surface of the print,
    // computed on demand by update_shell_bitset() for EReducedDetailMode::ShellOnly
    //
    BitSet<> m_shell_bitset;
    // the segments that are the topmost, or the bottommost, thing at their place in the whole
    // print: what a view from above, or below, sees of a layer, kept even while layers are skipped
    BitSet<> m_top_visible_bitset;
    BitSet<> m_bottom_visible_bitset;
#endif // ENABLE_OPENGL_ES
    //
    // Variables used for toolpaths coloring
    //
    std::optional<Settings> m_settings_used_for_ranges;
    ColorRange m_height_range;
    ColorRange m_width_range;
    ColorRange m_speed_range;
    ColorRange m_actual_speed_range;
    ColorRange m_fan_speed_range;
    ColorRange m_temperature_range;
    // ORCA: Add Pressure Advance visualization support
    ColorRange m_pressure_advance_range;
    // ORCA: Add Acceleration visualization support
    ColorRange m_acceleration_range;
    // ORCA: Add Jerk visualization support
    ColorRange m_jerk_range;
    ColorRange m_volumetric_rate_range;
    ColorRange m_actual_volumetric_rate_range;
    std::array<ColorRange, COLOR_RANGE_TYPES_COUNT> m_layer_time_range{
        ColorRange(EColorRangeType::Linear), ColorRange(EColorRangeType::Logarithmic)
    };
    Palette m_tool_colors;
    Palette m_color_print_colors;
    //
    // OpenGL shaders ids
    //
    unsigned int m_segments_shader_id{ 0 };
    unsigned int m_options_shader_id{ 0 };
#if VGCODE_ENABLE_COG_AND_TOOL_MARKERS
    unsigned int m_cog_marker_shader_id{ 0 };
    unsigned int m_tool_marker_shader_id{ 0 };
#endif // VGCODE_ENABLE_COG_AND_TOOL_MARKERS
    //
    // Caches for OpenGL uniforms id for segments shader 
    //
    int m_uni_segments_view_matrix_id{ -1 };
    int m_uni_segments_projection_matrix_id{ -1 };
    int m_uni_segments_camera_position_id{ -1 };
    int m_uni_segments_height_scale_id{ -1 };
    int m_uni_segments_positions_tex_id{ -1 };
    int m_uni_segments_height_width_angle_tex_id{ -1 };
    int m_uni_segments_colors_tex_id{ -1 };
    int m_uni_segments_segment_index_tex_id{ -1 };
    //
    // Caches for OpenGL uniforms id for options shader 
    //
    int m_uni_options_view_matrix_id{ -1 };
    int m_uni_options_projection_matrix_id{ -1 };
    int m_uni_options_positions_tex_id{ -1 };
    int m_uni_options_height_width_angle_tex_id{ -1 };
    int m_uni_options_colors_tex_id{ -1 };
    int m_uni_options_segment_index_tex_id{ -1 };
#if VGCODE_ENABLE_COG_AND_TOOL_MARKERS
    //
    // Caches for OpenGL uniforms id for cog marker shader 
    //
    int m_uni_cog_marker_world_center_position{ -1 };
    int m_uni_cog_marker_scale_factor{ -1 };
    int m_uni_cog_marker_view_matrix{ -1 };
    int m_uni_cog_marker_projection_matrix{ -1 };
    //
    // Caches for OpenGL uniforms id for tool marker shader 
    //
    int m_uni_tool_marker_world_origin{ -1 };
    int m_uni_tool_marker_scale_factor{ -1 };
    int m_uni_tool_marker_view_matrix{ -1 };
    int m_uni_tool_marker_projection_matrix{ -1 };
    int m_uni_tool_marker_color_base{ -1 };
#endif // VGCODE_ENABLE_COG_AND_TOOL_MARKERS

#ifdef ENABLE_OPENGL_ES
    class TextureData
    {
    public:
        void init(size_t vertices_count);
        void set_positions(const std::vector<Vec3>& positions);
        void set_heights_widths_angles(const std::vector<Vec3>& heights_widths_angles);
        void set_colors(const std::vector<float>& colors);
        void set_enabled_segments(const std::vector<uint32_t>& enabled_segments);
        void set_enabled_options(const std::vector<uint32_t>& enabled_options);
        void reset();
        size_t get_count() const { return m_count; }
        std::pair<unsigned int, size_t> get_positions_tex_id(size_t id) const;
        std::pair<unsigned int, size_t> get_heights_widths_angles_tex_id(size_t id) const;
        std::pair<unsigned int, size_t> get_colors_tex_id(size_t id) const;
        std::pair<unsigned int, size_t> get_enabled_segments_tex_id(size_t id) const;
        std::pair<unsigned int, size_t> get_enabled_options_tex_id(size_t id) const;

        size_t get_enabled_segments_count() const;
        size_t get_enabled_options_count() const;

        size_t max_texture_capacity() const { return m_width * m_height; }
        size_t get_used_gpu_memory() const;

    private:
        //
        // Texture width
        //
        size_t m_width{ 0 };
        //
        // Texture height
        //
        size_t m_height{ 0 };
        //
        // Count of textures
        //
        size_t m_count{ 0 };
        //
        // Caches for size of data sent to gpu, in bytes
        //
        size_t m_positions_size{ 0 };
        size_t m_height_width_angle_size{ 0 };
        size_t m_colors_size{ 0 };
        size_t m_enabled_segments_size{ 0 };
        size_t m_enabled_options_size{ 0 };

        struct TexIds
        {
            //
            // OpenGL texture to store positions
            //
            std::pair<unsigned int, size_t> positions{ 0, 0 };
            //
            // OpenGL texture to store heights, widths and angles
            //
            std::pair<unsigned int, size_t> heights_widths_angles{ 0, 0 };
            //
            // OpenGL texture to store colors
            //
            std::pair<unsigned int, size_t> colors{ 0, 0 };
            //
            // OpenGL texture to store enabled segments
            //
            std::pair<unsigned int, size_t> enabled_segments{ 0, 0 };
            //
            // OpenGL texture to store enabled options
            //
            std::pair<unsigned int, size_t> enabled_options{ 0, 0 };
        };

        std::vector<TexIds> m_tex_ids;
    };

    TextureData m_texture_data;
#else
    //
    // OpenGL buffers to store positions
    //
    unsigned int m_positions_buf_id{ 0 };
    unsigned int m_positions_tex_id{ 0 };
    //
    // OpenGL buffers to store heights, widths and angles
    //
    unsigned int m_heights_widths_angles_buf_id{ 0 };
    unsigned int m_heights_widths_angles_tex_id{ 0 };
    //
    // OpenGL buffers to store colors
    //
    unsigned int m_colors_buf_id{ 0 };
    unsigned int m_colors_tex_id{ 0 };
    //
    // OpenGL buffers to store enabled segments
    //
    unsigned int m_enabled_segments_buf_id{ 0 };
    unsigned int m_enabled_segments_tex_id{ 0 };
    size_t m_enabled_segments_count{ 0 };
    //
    // OpenGL buffers to store enabled options
    //
    unsigned int m_enabled_options_buf_id{ 0 };
    unsigned int m_enabled_options_tex_id{ 0 };
    size_t m_enabled_options_count{ 0 };
    //
    // OpenGL buffers to store the reduced sets drawn while Settings::reduced_detail is set
    //
    unsigned int m_enabled_segments_reduced_buf_id{ 0 };
    unsigned int m_enabled_segments_reduced_tex_id{ 0 };
    size_t m_enabled_segments_reduced_count{ 0 };
    unsigned int m_enabled_options_reduced_buf_id{ 0 };
    unsigned int m_enabled_options_reduced_tex_id{ 0 };
    size_t m_enabled_options_reduced_count{ 0 };
    //
    // OpenGL buffer to store the segments drawn at rest while Settings::rest_detail_mode is not Off.
    // Every layer is drawn at rest, so the options are the full set and need no counterpart.
    //
    unsigned int m_enabled_segments_rest_buf_id{ 0 };
    unsigned int m_enabled_segments_rest_tex_id{ 0 };
    size_t m_enabled_segments_rest_count{ 0 };
    //
    // Caches for size of data sent to gpu, in bytes
    //
    size_t m_positions_tex_size{ 0 };
    size_t m_height_width_angle_tex_size{ 0 };
    size_t m_colors_tex_size{ 0 };
    size_t m_enabled_segments_tex_size{ 0 };
    size_t m_enabled_options_tex_size{ 0 };

    // The set the next draw reads from: the reduced one only while the user is dragging, and only
    // if a reduced set is being built at all; otherwise the rest set, if one is being built. A rest
    // set already smaller than the reduced one, as it is with layers merged looking from above,
    // stays bound through the drag: it was right for the camera the drag started from.
    bool build_rest_set() const {
        return m_settings.rest_detail_mode != EReducedDetailMode::Off && m_settings.rest_detail_mode != EReducedDetailMode::LayersOnly;
    }
    bool use_reduced_set() const {
        return m_settings.reduced_detail && m_settings.reduced_detail_mode != EReducedDetailMode::Off &&
               !(build_rest_set() && m_enabled_segments_rest_count < m_enabled_segments_reduced_count);
    }
    bool use_rest_set() const { return !use_reduced_set() && build_rest_set(); }
    // how many layers each drawn segment of the bound set stands in for
    float active_height_scale() const {
        if (use_reduced_set())
            return static_cast<float>(std::max<uint32_t>(1, m_settings.reduced_detail_layer_stride));
        if (use_rest_set() && m_settings.rest_detail_mode == EReducedDetailMode::ShellOnly)
            return static_cast<float>(std::max<uint32_t>(1, m_settings.rest_layer_stride));
        return 1.0f;
    }
    size_t active_segments_count() const {
        return use_reduced_set() ? m_enabled_segments_reduced_count : use_rest_set() ? m_enabled_segments_rest_count : m_enabled_segments_count;
    }
    unsigned int active_segments_buf_id() const {
        return use_reduced_set() ? m_enabled_segments_reduced_buf_id : use_rest_set() ? m_enabled_segments_rest_buf_id : m_enabled_segments_buf_id;
    }
    unsigned int active_segments_tex_id() const {
        return use_reduced_set() ? m_enabled_segments_reduced_tex_id : use_rest_set() ? m_enabled_segments_rest_tex_id : m_enabled_segments_tex_id;
    }
    size_t active_options_count() const {
        return use_reduced_set() ? m_enabled_options_reduced_count : m_enabled_options_count;
    }
    unsigned int active_options_buf_id() const {
        return use_reduced_set() ? m_enabled_options_reduced_buf_id : m_enabled_options_buf_id;
    }
    unsigned int active_options_tex_id() const {
        return use_reduced_set() ? m_enabled_options_reduced_tex_id : m_enabled_options_tex_id;
    }

    // Whether the segment starting at vertex i belongs to the set built under the given mode
    bool reduced_set_keeps(EReducedDetailMode mode, size_t i, const PathVertex& v) const;
    void update_shell_bitset();
#endif // ENABLE_OPENGL_ES

    void update_view_full_range();
    void update_color_ranges();
    void update_heights_widths();
    void render_segments(const Mat4x4& view_matrix, const Mat4x4& projection_matrix, const Vec3& camera_position);
    void render_options(const Mat4x4& view_matrix, const Mat4x4& projection_matrix);
#if VGCODE_ENABLE_COG_AND_TOOL_MARKERS
    void render_cog_marker(const Mat4x4& view_matrix, const Mat4x4& projection_matrix);
    void render_tool_marker(const Mat4x4& view_matrix, const Mat4x4& projection_matrix);
#endif // VGCODE_ENABLE_COG_AND_TOOL_MARKERS
};

} // namespace libvgcode

#endif // VGCODE_VIEWERIMPL_HPP