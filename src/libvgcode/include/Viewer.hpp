///|/ Copyright (c) Prusa Research 2023 Enrico Turri @enricoturri1966, Pavel Mikuš @Godrak
///|/
///|/ libvgcode is released under the terms of the AGPLv3 or higher
///|/
#ifndef VGCODE_VIEWER_HPP
#define VGCODE_VIEWER_HPP

#include "Types.hpp"

#include <string>

namespace libvgcode {

class ViewerImpl;
struct GCodeInputData;
struct PathVertex;
class ColorRange;
struct ColorPrint;

class Viewer
{
public:
    Viewer();
    ~Viewer();
    Viewer(const Viewer& other) = delete;
    Viewer(Viewer&& other) = delete;
    Viewer& operator = (const Viewer& other) = delete;
    Viewer& operator = (Viewer&& other) = delete;

    //
    // Initialize the viewer.
    // Param opengl_context_version must be the string returned by glGetString(GL_VERSION).
    // This method must be called after a valid OpenGL context has been already created
    // and before calling any other method of the viewer.
    // Throws an std::runtime_error exception if:
    // * the method is called before creating an OpenGL context
    // * the created OpenGL context does not support OpenGL 3.2 or greater
    // * when using OpenGL ES, the created OpenGL ES context does not support OpenGL ES 2.0 or greater
    // * any of the shaders fails to compile
    //
    void init(const std::string& opengl_context_version);
    //
    // Release the resources used by the viewer.
    // This method must be called before releasing the OpenGL context if the viewer
    // goes out of scope after releasing it.
    //
    void shutdown();
    //
    // Reset the contents of the viewer.
    // Automatically called by load() method.
    //
    void reset();
    //
    // Setup the viewer content from the given data.
    // See: GCodeInputData
    //
    void load(GCodeInputData&& gcode_data);
    //
    // Render the toolpaths according to the current settings and
    // using the given camera matrices.
    //
    void render(const Mat4x4& view_matrix, const Mat4x4& projection_matrix);

    //
    // ************************************************************************
    // Settings
    // The following methods can be used to query/customize the parameters
    // used to render the toolpaths.
    // ************************************************************************
    //

    //
    // View type
    // See: EViewType
    //
    EViewType get_view_type() const;
    void set_view_type(EViewType type);
    //
    // Time mode
    // See: ETimeMode
    //
    ETimeMode get_time_mode() const;
    void set_time_mode(ETimeMode mode);
    //
    // Top layer only
    // Whether or not the visible range is limited to the current top layer only.
    //
    bool is_top_layer_only_view_range() const;
    //
    // Toggle the top layer only state.
    //
    void toggle_top_layer_only_view_range();
    //
    // Returns true if the given option is visible.
    //
    bool is_option_visible(EOptionType type) const;
    //
    // Toggle the visibility state of the given option.
    //
    void toggle_option_visibility(EOptionType type);
    //
    // Returns true if the given extrusion role is visible.
    //
    bool is_extrusion_role_visible(EGCodeExtrusionRole role) const;
    //
    // Toggle the visibility state of the given extrusion role.
    //
    void toggle_extrusion_role_visibility(EGCodeExtrusionRole role);
    //
    // Return the color used to render the given extrusion rols.
    //
    const Color& get_extrusion_role_color(EGCodeExtrusionRole role) const;
    //
    // Set the color used to render the given extrusion role.
    //
    void set_extrusion_role_color(EGCodeExtrusionRole role, const Color& color);
    //
    // Reset the colors used to render the extrusion roles to the default value.
    //
    void reset_default_extrusion_roles_colors();
    //
    // Return the color used to render the given option.
    //
    const Color& get_option_color(EOptionType type) const;
    //
    // Set the color used to render the given option.
    //
    void set_option_color(EOptionType type, const Color& color);
    //
    // Reset the colors used to render the options to the default value.
    //
    void reset_default_options_colors();
    //
    // Return the count of colors in the palette used to render
    // the toolpaths when the view type is EViewType::Tool.
    //
    size_t get_tool_colors_count() const;
    //
    // Return the palette used to render the toolpaths when
    // the view type is EViewType::Tool.
    //
    const Palette& get_tool_colors() const;
    //
    // Set the palette used to render the toolpaths when
    // the view type is EViewType::Tool with the given one.
    //
    void set_tool_colors(const Palette& colors);
    //
    // Return the count of colors in the palette used to render
    // the toolpaths when the view type is EViewType::ColorPrint.
    //
    size_t get_color_print_colors_count() const;
    //
    // Return the palette used to render the toolpaths when
    // the view type is EViewType::ColorPrint.
    //
    const Palette& get_color_print_colors() const;
    //
    // Set the palette used to render the toolpaths when
    // the view type is EViewType::ColorPrint with the given one.
    //
    void set_color_print_colors(const Palette& colors);
    //
    // Get the color range for the given view type.
    // Valid view types are:
    // EViewType::Height
    // EViewType::Width
    // EViewType::Speed
    // EViewType::ActualSpeed
    // EViewType::FanSpeed
    // EViewType::Temperature
    // EViewType::VolumetricFlowRate
    // EViewType::ActualVolumetricFlowRate
    // EViewType::LayerTimeLinear
    // EViewType::LayerTimeLogarithmic
    //
    const ColorRange& get_color_range(EViewType type) const;
    //
    // Set the palette for the color range corresponding to the given view type
    // with the given value.
    // Valid view types are:
    // EViewType::Height
    // EViewType::Width
    // EViewType::Speed
    // EViewType::ActualSpeed
    // EViewType::FanSpeed
    // EViewType::Temperature
    // EViewType::VolumetricFlowRate
    // EViewType::ActualVolumetricFlowRate
    // EViewType::LayerTimeLinear
    // EViewType::LayerTimeLogarithmic
    //
    void set_color_range_palette(EViewType type, const Palette& palette);
    //
    // Get the radius, in mm, of the cylinders used to render the travel moves.
    //
    float get_travels_radius() const;
    //
    // Set the radius, in mm, of the cylinders used to render the travel moves.
    // Radius is clamped to [MIN_TRAVELS_RADIUS_MM..MAX_TRAVELS_RADIUS_MM]
    //
    void set_travels_radius(float radius);
    //
    // Get the radius, in mm, of the cylinders used to render the wipe moves.
    //
    float get_wipes_radius() const;
    //
    // Set the radius, in mm, of the cylinders used to render the wipe moves.
    // Radius is clamped to [MIN_WIPES_RADIUS_MM..MAX_WIPES_RADIUS_MM]
    //
    void set_wipes_radius(float radius);
    //
    // Return the count of detected layers.
    //
    size_t get_layers_count() const;
    //
    // Return the current visible layers range.
    //
    const Interval& get_layers_view_range() const;
    //
    // Set the current visible layers range with the given interval.
    // Values are clamped to [0..get_layers_count() - 1].
    //
    void set_layers_view_range(const Interval& range);
    //
    // Set the current visible layers range with the given min and max values.
    // Values are clamped to [0..get_layers_count() - 1].
    //
    void set_layers_view_range(Interval::value_type min, Interval::value_type max);
    //
    // Return the current visible range.
    // Three ranges are defined: full, enabled and visible.
    // For all of them the range endpoints represent:
    // [0] -> min vertex id
    // [1] -> max vertex id
    // Full is the range of vertices that could potentially be visualized accordingly to the current settings.
    // Enabled is the part of the full range that is selected for visualization accordingly to the current settings.
    // Visible is the part of the enabled range that is actually visualized accordingly to the current settings.
    // 
    const Interval& get_view_visible_range() const;
    //
    // Set the current visible range.
    // Values are clamped to the current view enabled range;
    // 
    void set_view_visible_range(Interval::value_type min, Interval::value_type max);
    //
    // Return the current full range.
    //
    const Interval& get_view_full_range() const;
    //
    // Return the current enabled range.
    //
    const Interval& get_view_enabled_range() const;

    //
    // ************************************************************************
    // Property getters
    // The following methods can be used to query detected properties.
    // ************************************************************************
    //

    //
    // Spiral vase mode
    // Whether or not the gcode was generated with spiral vase mode enabled.
    // See: GCodeInputData
    //
    bool is_spiral_vase_mode() const;
    //
    // Return the z of the layer with the given id
    // or 0.0f if the id does not belong to [0..get_layers_count() - 1].
    //
    float get_layer_z(size_t layer_id) const;
    //
    // Return the list of zs of the detected layers.
    //
    std::vector<float> get_layers_zs() const;
    //
    // Return the id of the layer closest to the given z.
    //
    size_t get_layer_id_at(float z) const;
    //
    // Return the count of detected used extruders.
    //
    size_t get_used_extruders_count() const;
    //
    // Return the list of ids of the detected used extruders.
    //
    std::vector<uint8_t> get_used_extruders_ids() const;
    //
    // Return the list of detected time modes.
    //
    std::vector<ETimeMode> get_time_modes() const;
    //
    // Return the count of vertices used to render the toolpaths
    //
    size_t get_vertices_count() const;
    //
    // Return the vertex pointed by the max value of the view visible range
    //
    const PathVertex& get_current_vertex() const;
    //
    // Return the index of vertex pointed by the max value of the view visible range
    //
    size_t get_current_vertex_id() const;
    //
    // Return the vertex at the given index
    //
    const PathVertex& get_vertex_at(size_t id) const;
    //
    // Return the total estimated time, in seconds, using the current time mode.
    //
    float get_estimated_time() const;
    //
    // Return the estimated time, in seconds, at the vertex with the given index
    // using the current time mode.
    //
    float get_estimated_time_at(size_t id) const;
    //
    // Return the color used to render the given vertex with the current settings.
    //
    Color get_vertex_color(const PathVertex& vertex) const;
    //
    // Return the count of detected extrusion roles
    //
    size_t get_extrusion_roles_count() const;
    //
    // Return the list of detected extrusion roles
    //
    std::vector<EGCodeExtrusionRole> get_extrusion_roles() const;
    //
    // Return the count of detected options.
    //
    size_t get_options_count() const;
    //
    // Return the list of detected options.
    //
    const std::vector<EOptionType>& get_options() const;
    //
    // Return the count of detected color prints.
    //
    size_t get_color_prints_count(uint8_t extruder_id) const;
    //
    // Return the list of detected color prints.
    //
    std::vector<ColorPrint> get_color_prints(uint8_t extruder_id) const;
    //
    // Return the estimated time for the given role and the current time mode.
    //
    float get_extrusion_role_estimated_time(EGCodeExtrusionRole role) const;
    //
    // Return the estimated time for the travel moves and the current time mode.
    //
    float get_travels_estimated_time() const;
    //
    // Return the list of layers time for the current time mode.
    //
    std::vector<float> get_layers_estimated_times() const;
    //
    // Return the axes aligned bounding box containing all the given types.
    //
    AABox get_bounding_box(const std::vector<EMoveType>& types = {
        EMoveType::Retract, EMoveType::Unretract, EMoveType::Seam, EMoveType::ToolChange,
        EMoveType::ColorChange, EMoveType::PausePrint, EMoveType::CustomGCode, EMoveType::Travel,
        EMoveType::Wipe, EMoveType::Extrude }) const;
    //
    // Return the axes aligned bounding box containing all the extrusions with the given roles.
    //
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
    //
    // Return the size of the used cpu memory, in bytes
    //
    size_t get_used_cpu_memory() const;
    //
    // Return the size of the used gpu memory, in bytes
    //
    size_t get_used_gpu_memory() const;

#if VGCODE_ENABLE_COG_AND_TOOL_MARKERS
    //
    // Returns the position of the center of gravity of the toolpaths.
    // It does not take in account extrusions of type:
    // Skirt
    // Support Material
    // Support Material Interface
    // WipeTower
    // Custom
    //
    Vec3 get_cog_position() const;

    float get_cog_marker_scale_factor() const;
    void set_cog_marker_scale_factor(float factor);

    const Vec3& get_tool_marker_position() const;

    float get_tool_marker_offset_z() const;
    void set_tool_marker_offset_z(float offset_z);

    float get_tool_marker_scale_factor() const;
    void set_tool_marker_scale_factor(float factor);

    const Color& get_tool_marker_color() const;
    void set_tool_marker_color(const Color& color);

    float get_tool_marker_alpha() const;
    void set_tool_marker_alpha(float alpha);
#endif // VGCODE_ENABLE_COG_AND_TOOL_MARKERS

private:
    ViewerImpl* m_impl{ nullptr };
};

} // namespace libvgcode

#endif // VGCODE_VIEWER_HPP
