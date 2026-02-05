///|/ Copyright (c) Prusa Research 2023 Enrico Turri @enricoturri1966, Pavel Mikuš @Godrak
///|/
///|/ libvgcode is released under the terms of the AGPLv3 or higher
///|/
#include "../include/Viewer.hpp"
#include "ViewerImpl.hpp"

namespace libvgcode {

Viewer::Viewer()
{
    m_impl = new ViewerImpl();
}

Viewer::~Viewer()
{
    delete m_impl;
}

void Viewer::init(const std::string& opengl_context_version)
{
    m_impl->init(opengl_context_version);
}

void Viewer::shutdown()
{
    m_impl->shutdown();
}

void Viewer::reset()
{
    m_impl->reset();
}

void Viewer::load(GCodeInputData&& gcode_data)
{
    m_impl->load(std::move(gcode_data));
}

void Viewer::render(const Mat4x4& view_matrix, const Mat4x4& projection_matrix)
{
    m_impl->render(view_matrix, projection_matrix);
}

EViewType Viewer::get_view_type() const
{
    return m_impl->get_view_type();
}

void Viewer::set_view_type(EViewType type)
{
    m_impl->set_view_type(type);
}

ETimeMode Viewer::get_time_mode() const
{
    return m_impl->get_time_mode();
}

void Viewer::set_time_mode(ETimeMode mode)
{
    m_impl->set_time_mode(mode);
}

bool Viewer::is_top_layer_only_view_range() const
{
    return m_impl->is_top_layer_only_view_range();
}

void Viewer::toggle_top_layer_only_view_range()
{
    m_impl->toggle_top_layer_only_view_range();
}

bool Viewer::is_option_visible(EOptionType type) const
{
    return m_impl->is_option_visible(type);
}

void Viewer::toggle_option_visibility(EOptionType type)
{
    m_impl->toggle_option_visibility(type);
}

bool Viewer::is_extrusion_role_visible(EGCodeExtrusionRole role) const
{
    return m_impl->is_extrusion_role_visible(role);
}

void Viewer::toggle_extrusion_role_visibility(EGCodeExtrusionRole role)
{
    m_impl->toggle_extrusion_role_visibility(role);
}

const Color& Viewer::get_extrusion_role_color(EGCodeExtrusionRole role) const
{
    return m_impl->get_extrusion_role_color(role);
}

void Viewer::set_extrusion_role_color(EGCodeExtrusionRole role, const Color& color)
{
    m_impl->set_extrusion_role_color(role, color);
}

void Viewer::reset_default_extrusion_roles_colors()
{
    m_impl->reset_default_extrusion_roles_colors();
}

const Color& Viewer::get_option_color(EOptionType type) const
{
    return m_impl->get_option_color(type);
}

void Viewer::set_option_color(EOptionType type, const Color& color)
{
    m_impl->set_option_color(type, color);
}

void Viewer::reset_default_options_colors()
{
    m_impl->reset_default_options_colors();
}

size_t Viewer::get_tool_colors_count() const
{
    return m_impl->get_tool_colors_count();
}

const Palette& Viewer::get_tool_colors() const
{
    return m_impl->get_tool_colors();
}

void Viewer::set_tool_colors(const Palette& colors)
{
    m_impl->set_tool_colors(colors);
}

size_t Viewer::get_color_print_colors_count() const
{
    return m_impl->get_color_print_colors_count();
}

const Palette& Viewer::get_color_print_colors() const
{
    return m_impl->get_color_print_colors();
}

void Viewer::set_color_print_colors(const Palette& colors)
{
    m_impl->set_color_print_colors(colors);
}

const ColorRange& Viewer::get_color_range(EViewType type) const
{
    return m_impl->get_color_range(type);
}

void Viewer::set_color_range_palette(EViewType type, const Palette& palette)
{
    m_impl->set_color_range_palette(type, palette);
}

float Viewer::get_travels_radius() const
{
    return m_impl->get_travels_radius();
}

void Viewer::set_travels_radius(float radius)
{
    m_impl->set_travels_radius(radius);
}

float Viewer::get_wipes_radius() const
{
    return m_impl->get_wipes_radius();
}

void Viewer::set_wipes_radius(float radius)
{
    m_impl->set_wipes_radius(radius);
}

size_t Viewer::get_layers_count() const
{
    return m_impl->get_layers_count();
}

const Interval& Viewer::get_layers_view_range() const
{
    return m_impl->get_layers_view_range();
}

void Viewer::set_layers_view_range(const Interval& range)
{
    m_impl->set_layers_view_range(range);
}

void Viewer::set_layers_view_range(Interval::value_type min, Interval::value_type max)
{
    m_impl->set_layers_view_range(min, max);
}

const Interval& Viewer::get_view_visible_range() const
{
    return m_impl->get_view_visible_range();
}

void Viewer::set_view_visible_range(Interval::value_type min, Interval::value_type max)
{
    m_impl->set_view_visible_range(min, max);
}

const Interval& Viewer::get_view_full_range() const
{
    return m_impl->get_view_full_range();
}

const Interval& Viewer::get_view_enabled_range() const
{
    return m_impl->get_view_enabled_range();
}

bool Viewer::is_spiral_vase_mode() const
{
    return m_impl->is_spiral_vase_mode();
}

float Viewer::get_layer_z(size_t layer_id) const
{
    return m_impl->get_layer_z(layer_id);
}

std::vector<float> Viewer::get_layers_zs() const
{
    return m_impl->get_layers_zs();
}

size_t Viewer::get_layer_id_at(float z) const
{
    return m_impl->get_layer_id_at(z);
}

size_t Viewer::get_used_extruders_count() const
{
    return m_impl->get_used_extruders_count();
}

std::vector<uint8_t> Viewer::get_used_extruders_ids() const
{
    return m_impl->get_used_extruders_ids();
}

std::vector<ETimeMode> Viewer::get_time_modes() const
{
    return m_impl->get_time_modes();
}

size_t Viewer::get_vertices_count() const
{
    return m_impl->get_vertices_count();
}

const PathVertex& Viewer::get_current_vertex() const
{
    return m_impl->get_current_vertex();
}

size_t Viewer::get_current_vertex_id() const
{
    return m_impl->get_current_vertex_id();
}

const PathVertex& Viewer::get_vertex_at(size_t id) const
{
    return m_impl->get_vertex_at(id);
}

float Viewer::get_estimated_time() const
{
    return m_impl->get_estimated_time();
}

float Viewer::get_estimated_time_at(size_t id) const
{
    return m_impl->get_estimated_time_at(id);
}

Color Viewer::get_vertex_color(const PathVertex& vertex) const
{
    return m_impl->get_vertex_color(vertex);
}

size_t Viewer::get_extrusion_roles_count() const
{
    return m_impl->get_extrusion_roles_count();
}

std::vector<EGCodeExtrusionRole> Viewer::get_extrusion_roles() const
{
    return m_impl->get_extrusion_roles();
}

size_t Viewer::get_options_count() const
{
    return m_impl->get_options_count();
}

const std::vector<EOptionType>& Viewer::get_options() const
{
    return m_impl->get_options();
}

size_t Viewer::get_color_prints_count(uint8_t extruder_id) const
{
    return m_impl->get_color_prints_count(extruder_id);
}

std::vector<ColorPrint> Viewer::get_color_prints(uint8_t extruder_id) const
{
    return m_impl->get_color_prints(extruder_id);
}

float Viewer::get_extrusion_role_estimated_time(EGCodeExtrusionRole role) const
{
    return m_impl->get_extrusion_role_estimated_time(role);
}

float Viewer::get_travels_estimated_time() const
{
    return m_impl->get_travels_estimated_time();
}

std::vector<float> Viewer::get_layers_estimated_times() const
{
    return m_impl->get_layers_estimated_times();
}

AABox Viewer::get_bounding_box(const std::vector<EMoveType>& types) const
{
    return m_impl->get_bounding_box(types);
}

AABox Viewer::get_extrusion_bounding_box(const std::vector<EGCodeExtrusionRole>& roles) const
{
    return m_impl->get_extrusion_bounding_box(roles);
}

size_t Viewer::get_used_cpu_memory() const
{
    return m_impl->get_used_cpu_memory();
}

size_t Viewer::get_used_gpu_memory() const
{
    return m_impl->get_used_gpu_memory();
}

#if VGCODE_ENABLE_COG_AND_TOOL_MARKERS
Vec3 Viewer::get_cog_position() const
{
    return m_impl->get_cog_marker_position();
}

float Viewer::get_cog_marker_scale_factor() const
{
    return m_impl->get_cog_marker_scale_factor();
}

void Viewer::set_cog_marker_scale_factor(float factor)
{
    m_impl->set_cog_marker_scale_factor(factor);
}

const Vec3& Viewer::get_tool_marker_position() const
{
    return m_impl->get_tool_marker_position();
}

float Viewer::get_tool_marker_offset_z() const
{
    return m_impl->get_tool_marker_offset_z();
}

void Viewer::set_tool_marker_offset_z(float offset_z)
{
    m_impl->set_tool_marker_offset_z(offset_z);
}

float Viewer::get_tool_marker_scale_factor() const
{
    return m_impl->get_tool_marker_scale_factor();
}

void Viewer::set_tool_marker_scale_factor(float factor)
{
    m_impl->set_tool_marker_scale_factor(factor);
}

const Color& Viewer::get_tool_marker_color() const
{
    return m_impl->get_tool_marker_color();
}

void Viewer::set_tool_marker_color(const Color& color)
{
    m_impl->set_tool_marker_color(color);
}

float Viewer::get_tool_marker_alpha() const
{
    return m_impl->get_tool_marker_alpha();
}

void Viewer::set_tool_marker_alpha(float alpha)
{
    m_impl->set_tool_marker_alpha(alpha);
}
#endif // VGCODE_ENABLE_COG_AND_TOOL_MARKERS

} // namespace libvgcode
