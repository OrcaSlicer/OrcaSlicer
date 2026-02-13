///|/ Copyright (c) Prusa Research 2020 - 2023 Enrico Turri @enricoturri1966
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <cassert>
#include <cinttypes>

#include "libslic3r/libslic3r.h"
#include "LibVGCodeWrapper.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/GCode/WipeTower.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Line.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "../../src/libvgcode/include/GCodeInputData.hpp"
#include "../../src/libvgcode/include/PathVertex.hpp"
#include "libvgcode/include/Types.hpp"

namespace libvgcode {
class Viewer;

Vec3 convert(const Slic3r::Vec3f& v)
{
    return { v.x(), v.y(), v.z() };
}

Slic3r::Vec3f convert(const Vec3& v)
{
    return { v[0], v[1], v[2] };
}

Mat4x4 convert(const Slic3r::Matrix4f& m)
{
    Mat4x4 ret;
    std::memcpy(ret.data(), m.data(), 16 * sizeof(float));
    return ret;
}

Slic3r::ColorRGBA convert(const Color& c)
{
    static const float inv_255 = 1.0f / 255.0f;
    return { c[0] * inv_255, c[1] * inv_255, c[2] * inv_255, 1.0f };
}

Color convert(const Slic3r::ColorRGBA& c)
{
    // ORCA: Fix dark color rendering. Ensure minimal brightness.
    auto safe_val = [](float v) -> uint8_t {
        return std::max((uint8_t)(v * 255.0f), (uint8_t)48);
    };
    return { safe_val(c.r()), safe_val(c.g()), safe_val(c.b()) };
}

Color convert(const std::string& color_str)
{
    Slic3r::ColorRGBA color_rgba;
    return decode_color(color_str, color_rgba) ? convert(color_rgba) : DUMMY_COLOR;
}

Slic3r::ExtrusionRole convert(EGCodeExtrusionRole role)
{
    switch (role)
    {
    case EGCodeExtrusionRole::None:                     { return Slic3r::ExtrusionRole::erNone; }
    case EGCodeExtrusionRole::Perimeter:                { return Slic3r::ExtrusionRole::erPerimeter; }
    case EGCodeExtrusionRole::ExternalPerimeter:        { return Slic3r::ExtrusionRole::erExternalPerimeter; }
    case EGCodeExtrusionRole::OverhangPerimeter:        { return Slic3r::ExtrusionRole::erOverhangPerimeter; }
    case EGCodeExtrusionRole::InternalInfill:           { return Slic3r::ExtrusionRole::erInternalInfill; }
    case EGCodeExtrusionRole::SolidInfill:              { return Slic3r::ExtrusionRole::erSolidInfill; }
    case EGCodeExtrusionRole::TopSolidInfill:           { return Slic3r::ExtrusionRole::erTopSolidInfill; }
    case EGCodeExtrusionRole::Ironing:                  { return Slic3r::ExtrusionRole::erIroning; }
    case EGCodeExtrusionRole::BridgeInfill:             { return Slic3r::ExtrusionRole::erBridgeInfill; }
    case EGCodeExtrusionRole::GapFill:                  { return Slic3r::ExtrusionRole::erGapFill; }
    case EGCodeExtrusionRole::Skirt:                    { return Slic3r::ExtrusionRole::erSkirt; }
    case EGCodeExtrusionRole::SupportMaterial:          { return Slic3r::ExtrusionRole::erSupportMaterial; }
    case EGCodeExtrusionRole::SupportMaterialInterface: { return Slic3r::ExtrusionRole::erSupportMaterialInterface; }
    case EGCodeExtrusionRole::WipeTower:                { return Slic3r::ExtrusionRole::erWipeTower; }
    case EGCodeExtrusionRole::Custom:                   { return Slic3r::ExtrusionRole::erCustom; }
    // ORCA
    case EGCodeExtrusionRole::BottomSurface:            { return Slic3r::ExtrusionRole::erBottomSurface; }
    case EGCodeExtrusionRole::InternalBridgeInfill:     { return Slic3r::ExtrusionRole::erInternalBridgeInfill; }
    case EGCodeExtrusionRole::Brim:                     { return Slic3r::ExtrusionRole::erBrim; }
    case EGCodeExtrusionRole::SupportTransition:        { return Slic3r::ExtrusionRole::erSupportTransition; }
    case EGCodeExtrusionRole::Mixed:                    { return Slic3r::ExtrusionRole::erMixed; }
    default:                                            { return Slic3r::ExtrusionRole::erNone; }
    }
}

EGCodeExtrusionRole convert(Slic3r::ExtrusionRole role)
{
    switch (role)
    {
    case Slic3r::ExtrusionRole::erNone:                        { return EGCodeExtrusionRole::None; }
    case Slic3r::ExtrusionRole::erPerimeter:                   { return EGCodeExtrusionRole::Perimeter; }
    case Slic3r::ExtrusionRole::erExternalPerimeter:           { return EGCodeExtrusionRole::ExternalPerimeter; }
    case Slic3r::ExtrusionRole::erOverhangPerimeter:           { return EGCodeExtrusionRole::OverhangPerimeter; }
    case Slic3r::ExtrusionRole::erInternalInfill:              { return EGCodeExtrusionRole::InternalInfill; }
    case Slic3r::ExtrusionRole::erSolidInfill:                 { return EGCodeExtrusionRole::SolidInfill; }
    case Slic3r::ExtrusionRole::erTopSolidInfill:              { return EGCodeExtrusionRole::TopSolidInfill; }
    case Slic3r::ExtrusionRole::erIroning:                     { return EGCodeExtrusionRole::Ironing; }
    case Slic3r::ExtrusionRole::erBridgeInfill:                { return EGCodeExtrusionRole::BridgeInfill; }
    case Slic3r::ExtrusionRole::erGapFill:                     { return EGCodeExtrusionRole::GapFill; }
    case Slic3r::ExtrusionRole::erSkirt:                       { return EGCodeExtrusionRole::Skirt; }
    case Slic3r::ExtrusionRole::erSupportMaterial:             { return EGCodeExtrusionRole::SupportMaterial; }
    case Slic3r::ExtrusionRole::erSupportMaterialInterface:    { return EGCodeExtrusionRole::SupportMaterialInterface; }
    case Slic3r::ExtrusionRole::erWipeTower:                   { return EGCodeExtrusionRole::WipeTower; }
    case Slic3r::ExtrusionRole::erCustom:                      { return EGCodeExtrusionRole::Custom; }
    // ORCA
    case Slic3r::ExtrusionRole::erBottomSurface:               { return EGCodeExtrusionRole::BottomSurface; }
    case Slic3r::ExtrusionRole::erInternalBridgeInfill:        { return EGCodeExtrusionRole::InternalBridgeInfill; }
    case Slic3r::ExtrusionRole::erBrim:                        { return EGCodeExtrusionRole::Brim; }
    case Slic3r::ExtrusionRole::erSupportTransition:           { return EGCodeExtrusionRole::SupportTransition; }
    case Slic3r::ExtrusionRole::erMixed:                       { return EGCodeExtrusionRole::Mixed; }
    default:                                                   { return EGCodeExtrusionRole::None; }
    }
}

EMoveType convert(Slic3r::EMoveType type)
{
    switch (type)
    {
    case Slic3r::EMoveType::Noop:         { return EMoveType::Noop; }
    case Slic3r::EMoveType::Retract:      { return EMoveType::Retract; }
    case Slic3r::EMoveType::Unretract:    { return EMoveType::Unretract; }
    case Slic3r::EMoveType::Seam:         { return EMoveType::Seam; }
    case Slic3r::EMoveType::Tool_change:  { return EMoveType::ToolChange; }
    case Slic3r::EMoveType::Color_change: { return EMoveType::ColorChange; }
    case Slic3r::EMoveType::Pause_Print:  { return EMoveType::PausePrint; }
    case Slic3r::EMoveType::Custom_GCode: { return EMoveType::CustomGCode; }
    case Slic3r::EMoveType::Travel:       { return EMoveType::Travel; }
    case Slic3r::EMoveType::Wipe:         { return EMoveType::Wipe; }
    case Slic3r::EMoveType::Extrude:      { return EMoveType::Extrude; }
    default:                              { return EMoveType::COUNT; }
    }
}

// EOptionType convert(const Slic3r::GUI::Preview::OptionType& type)
// {
//     switch (type)
//     {
//     case Slic3r::GUI::Preview::OptionType::Travel:          { return EOptionType::Travels; }
//     case Slic3r::GUI::Preview::OptionType::Wipe:            { return EOptionType::Wipes; }
//     case Slic3r::GUI::Preview::OptionType::Retractions:     { return EOptionType::Retractions; }
//     case Slic3r::GUI::Preview::OptionType::Unretractions:   { return EOptionType::Unretractions; }
//     case Slic3r::GUI::Preview::OptionType::Seams:           { return EOptionType::Seams; }
//     case Slic3r::GUI::Preview::OptionType::ToolChanges:     { return EOptionType::ToolChanges; }
//     case Slic3r::GUI::Preview::OptionType::ColorChanges:    { return EOptionType::ColorChanges; }
//     case Slic3r::GUI::Preview::OptionType::PausePrints:     { return EOptionType::PausePrints; }
//     case Slic3r::GUI::Preview::OptionType::CustomGCodes:    { return EOptionType::CustomGCodes; }
// #if VGCODE_ENABLE_COG_AND_TOOL_MARKERS
//     case Slic3r::GUI::Preview::OptionType::CenterOfGravity: { return EOptionType::CenterOfGravity; }
//     case Slic3r::GUI::Preview::OptionType::ToolMarker:      { return EOptionType::ToolMarker; }
// #else
//     // case Slic3r::GUI::Preview::OptionType::CenterOfGravity: { return EOptionType::COUNT; }
//     case Slic3r::GUI::Preview::OptionType::ToolMarker:      { return EOptionType::COUNT; }
// #endif // VGCODE_ENABLE_COG_AND_TOOL_MARKERS
//     default:                                                { return EOptionType::COUNT; }
//     }
// }

ETimeMode convert(const Slic3r::PrintEstimatedStatistics::ETimeMode& mode)
{
    switch (mode)
    {
    case Slic3r::PrintEstimatedStatistics::ETimeMode::Normal:  { return ETimeMode::Normal; }
    case Slic3r::PrintEstimatedStatistics::ETimeMode::Stealth: { return ETimeMode::Stealth; }
    default:                                                   { return ETimeMode::COUNT; }
    }
}

Slic3r::PrintEstimatedStatistics::ETimeMode convert(const ETimeMode& mode)
{
    switch (mode)
    {
    case ETimeMode::Normal:  { return Slic3r::PrintEstimatedStatistics::ETimeMode::Normal; }
    case ETimeMode::Stealth: { return Slic3r::PrintEstimatedStatistics::ETimeMode::Stealth; }
    default:                 { return Slic3r::PrintEstimatedStatistics::ETimeMode::Count; }
    }
}

GCodeInputData convert(const Slic3r::GCodeProcessorResult& result, const std::vector<std::string>& str_tool_colors,
    const std::vector<std::string>& str_color_print_colors, const Viewer& viewer)
{
    GCodeInputData ret;

    // collect tool colors
    ret.tools_colors.reserve(str_tool_colors.size());
    for (const std::string& color : str_tool_colors) {
        ret.tools_colors.emplace_back(convert(color));
    }

    // collect color print colors
    const std::vector<std::string>& str_colors = str_color_print_colors.empty() ? str_tool_colors : str_color_print_colors;
    ret.color_print_colors.reserve(str_colors.size());
    for (const std::string& color : str_colors) {
        ret.color_print_colors.emplace_back(convert(color));
    }

    const std::vector<Slic3r::GCodeProcessorResult::MoveVertex>& moves = result.moves;
    ret.vertices.reserve(2 * moves.size());
    for (size_t i = 1; i < moves.size(); ++i) {
        const Slic3r::GCodeProcessorResult::MoveVertex& curr = moves[i];
        const Slic3r::GCodeProcessorResult::MoveVertex& prev = moves[i - 1];
        const EMoveType curr_type = convert(curr.type);
        const EOptionType option_type = move_type_to_option(curr_type);
        if (option_type == EOptionType::COUNT || option_type == EOptionType::Travels || option_type == EOptionType::Wipes) {
            if (ret.vertices.empty() || prev.type != curr.type || prev.extrusion_role != curr.extrusion_role
                // ORCA: Fix issue with flow rate changes being visualized incorrectly
                || prev.mm3_per_mm != curr.mm3_per_mm) {
                // to allow libvgcode to properly detect the start/end of a path we need to add a 'phantom' vertex
                // equal to the current one with the exception of the position, which should match the previous move position,
                // and the times, which are set to zero
#if VGCODE_ENABLE_COG_AND_TOOL_MARKERS
                const libvgcode::PathVertex vertex = { convert(prev.position), curr.height, curr.width, curr.feedrate, prev.actual_feedrate,
                    curr.mm3_per_mm, curr.fan_speed, curr.temperature, 0.0f, convert(curr.extrusion_role), curr_type,
                    static_cast<uint32_t>(curr.gcode_id), static_cast<uint32_t>(curr.layer_id),
                    static_cast<uint8_t>(curr.extruder_id), static_cast<uint8_t>(curr.cp_color_id), { 0.0f, 0.0f } };
#else
              const libvgcode::PathVertex vertex = { convert(prev.position), curr.height, curr.width, curr.feedrate, prev.actual_feedrate,
                    curr.mm3_per_mm, curr.fan_speed, curr.temperature, convert(curr.extrusion_role), curr_type,
                    static_cast<uint32_t>(curr.gcode_id), static_cast<uint32_t>(curr.layer_id),
                    static_cast<uint8_t>(curr.extruder_id), static_cast<uint8_t>(curr.cp_color_id), { 0.0f, 0.0f } };
#endif // VGCODE_ENABLE_COG_AND_TOOL_MARKERS
                ret.vertices.emplace_back(vertex);
            }
        }

#if VGCODE_ENABLE_COG_AND_TOOL_MARKERS
        const libvgcode::PathVertex vertex = { convert(curr.position), curr.height, curr.width, curr.feedrate, curr.actual_feedrate,
            curr.mm3_per_mm, curr.fan_speed, curr.temperature,
            result.filament_densities[curr.extruder_id] * curr.mm3_per_mm * (curr.position - prev.position).norm(),
            convert(curr.extrusion_role), curr_type, static_cast<uint32_t>(curr.gcode_id), static_cast<uint32_t>(curr.layer_id),
            static_cast<uint8_t>(curr.extruder_id), static_cast<uint8_t>(curr.cp_color_id), curr.time };
#else
        const libvgcode::PathVertex vertex = { convert(curr.position), curr.height, curr.width, curr.feedrate, curr.actual_feedrate,
            curr.mm3_per_mm, curr.fan_speed, curr.temperature, convert(curr.extrusion_role), curr_type,
            static_cast<uint32_t>(curr.gcode_id), static_cast<uint32_t>(curr.layer_id),
            static_cast<uint8_t>(curr.extruder_id), static_cast<uint8_t>(curr.cp_color_id), curr.time };
#endif // VGCODE_ENABLE_COG_AND_TOOL_MARKERS
        ret.vertices.emplace_back(vertex);
    }
    ret.vertices.shrink_to_fit();

    ret.spiral_vase_mode = result.spiral_vase_mode;

    return ret;
}

static void convert_lines_to_vertices(const Slic3r::Lines& lines, const std::vector<float>& widths, const std::vector<float>& heights,
    float top_z, size_t layer_id, size_t extruder_id, size_t color_id, EGCodeExtrusionRole extrusion_role, bool closed, std::vector<PathVertex>& vertices)
{
    if (lines.empty())
        return;

    // loop once more in case of closed loops
    const size_t lines_end = closed ? (lines.size() + 1) : lines.size();
    for (size_t ii = 0; ii < lines_end; ++ii) {
        const size_t i = (ii == lines.size()) ? 0 : ii;
        const Slic3r::Line& line = lines[i];
        // first segment of the polyline
        if (ii == 0) {
            // add a dummy vertex at the start, to separate the current line from the others
            const Slic3r::Vec2f a = unscale(line.a).cast<float>();
#if VGCODE_ENABLE_COG_AND_TOOL_MARKERS
            libvgcode::PathVertex vertex = { convert(Slic3r::Vec3f(a.x(), a.y(), top_z)), heights[i], widths[i], 0.0f, 0.0f,
                0.0f, 0.0f, 0.0f, 0.0f, extrusion_role, EMoveType::Noop, 0, static_cast<uint32_t>(layer_id),
                static_cast<uint8_t>(extruder_id), static_cast<uint8_t>(color_id), { 0.0f, 0.0f } };
#else
            libvgcode::PathVertex vertex = { convert(Slic3r::Vec3f(a.x(), a.y(), top_z)), heights[i], widths[i], 0.0f, 0.0f,
                0.0f, 0.0f, 0.0f, extrusion_role, EMoveType::Noop, 0, static_cast<uint32_t>(layer_id),
                static_cast<uint8_t>(extruder_id), static_cast<uint8_t>(color_id), { 0.0f, 0.0f } };
#endif // VGCODE_ENABLE_COG_AND_TOOL_MARKERS
            vertices.emplace_back(vertex);
            // add the starting vertex of the segment
            vertex.type = EMoveType::Extrude;
            vertices.emplace_back(vertex);
        }
        // add the ending vertex of the segment
        const Slic3r::Vec2f b = unscale(line.b).cast<float>();
#if VGCODE_ENABLE_COG_AND_TOOL_MARKERS
        const libvgcode::PathVertex vertex = { convert(Slic3r::Vec3f(b.x(), b.y(), top_z)), heights[i], widths[i], 0.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 0.0f, extrusion_role, EMoveType::Extrude, 0, static_cast<uint32_t>(layer_id),
            static_cast<uint8_t>(extruder_id), static_cast<uint8_t>(color_id), { 0.0f, 0.0f } };
#else
        const libvgcode::PathVertex vertex = { convert(Slic3r::Vec3f(b.x(), b.y(), top_z)), heights[i], widths[i], 0.0f, 0.0f,
            0.0f, 0.0f, 0.0f, extrusion_role, EMoveType::Extrude, 0, static_cast<uint32_t>(layer_id),
            static_cast<uint8_t>(extruder_id), static_cast<uint8_t>(color_id), { 0.0f, 0.0f } };
#endif // VGCODE_ENABLE_COG_AND_TOOL_MARKERS
        vertices.emplace_back(vertex);
    }
}

static void convert_to_vertices(const Slic3r::ExtrusionPath& extrusion_path, float print_z, size_t layer_id, size_t extruder_id, size_t color_id,
    EGCodeExtrusionRole extrusion_role, const Slic3r::Point& shift, std::vector<PathVertex>& vertices)
{
    Slic3r::Polyline polyline = extrusion_path.polyline;
    polyline.remove_duplicate_points();
    polyline.translate(shift);
    const Slic3r::Lines lines = polyline.lines();
    std::vector<float> widths(lines.size(), extrusion_path.width);
    std::vector<float> heights(lines.size(), extrusion_path.height);
    convert_lines_to_vertices(lines, widths, heights, print_z, layer_id, extruder_id, color_id, extrusion_role, false, vertices);
}

static void convert_to_vertices(const Slic3r::ExtrusionMultiPath& extrusion_multi_path, float print_z, size_t layer_id, size_t extruder_id,
    size_t color_id, EGCodeExtrusionRole extrusion_role, const Slic3r::Point& shift, std::vector<PathVertex>& vertices)
{
    Slic3r::Lines lines;
    std::vector<float> widths;
    std::vector<float> heights;
    for (const Slic3r::ExtrusionPath& extrusion_path : extrusion_multi_path.paths) {
        Slic3r::Polyline polyline = extrusion_path.polyline;
        polyline.remove_duplicate_points();
        polyline.translate(shift);
        const Slic3r::Lines lines_this = polyline.lines();
        append(lines, lines_this);
        widths.insert(widths.end(), lines_this.size(), extrusion_path.width);
        heights.insert(heights.end(), lines_this.size(), extrusion_path.height);
    }
    convert_lines_to_vertices(lines, widths, heights, print_z, layer_id, extruder_id, color_id, extrusion_role, false, vertices);
}

static void convert_to_vertices(const Slic3r::ExtrusionLoop& extrusion_loop, float print_z, size_t layer_id, size_t extruder_id, size_t color_id,
    EGCodeExtrusionRole extrusion_role, const Slic3r::Point& shift, std::vector<PathVertex>& vertices)
{
    Slic3r::Lines lines;
    std::vector<float> widths;
    std::vector<float> heights;
    for (const Slic3r::ExtrusionPath& extrusion_path : extrusion_loop.paths) {
        Slic3r::Polyline polyline = extrusion_path.polyline;
        polyline.remove_duplicate_points();
        polyline.translate(shift);
        const Slic3r::Lines lines_this = polyline.lines();
        append(lines, lines_this);
        widths.insert(widths.end(), lines_this.size(), extrusion_path.width);
        heights.insert(heights.end(), lines_this.size(), extrusion_path.height);
    }
    convert_lines_to_vertices(lines, widths, heights, print_z, layer_id, extruder_id, color_id, extrusion_role, true, vertices);
}

// forward declaration
static void convert_to_vertices(const Slic3r::ExtrusionEntityCollection& extrusion_entity_collection, float print_z, size_t layer_id,
    size_t extruder_id, size_t color_id, EGCodeExtrusionRole extrusion_role, const Slic3r::Point& shift, std::vector<PathVertex>& vertices);

static void convert_to_vertices(const Slic3r::ExtrusionEntity& extrusion_entity, float print_z, size_t layer_id, size_t extruder_id, size_t color_id,
    EGCodeExtrusionRole extrusion_role, const Slic3r::Point& shift, std::vector<PathVertex>& vertices)
{
    auto* extrusion_path = dynamic_cast<const Slic3r::ExtrusionPath*>(&extrusion_entity);
    if (extrusion_path != nullptr)
        convert_to_vertices(*extrusion_path, print_z, layer_id, extruder_id, color_id, extrusion_role, shift, vertices);
    else {
        auto* extrusion_loop = dynamic_cast<const Slic3r::ExtrusionLoop*>(&extrusion_entity);
        if (extrusion_loop != nullptr)
            convert_to_vertices(*extrusion_loop, print_z, layer_id, extruder_id, color_id, extrusion_role, shift, vertices);
        else {
            auto* extrusion_multi_path = dynamic_cast<const Slic3r::ExtrusionMultiPath*>(&extrusion_entity);
            if (extrusion_multi_path != nullptr)
                convert_to_vertices(*extrusion_multi_path, print_z, layer_id, extruder_id, color_id, extrusion_role, shift, vertices);
            else {
                auto* extrusion_entity_collection = dynamic_cast<const Slic3r::ExtrusionEntityCollection*>(&extrusion_entity);
                if (extrusion_entity_collection != nullptr)
                    convert_to_vertices(*extrusion_entity_collection, print_z, layer_id, extruder_id, color_id, extrusion_role, shift, vertices);
                else
                    throw Slic3r::RuntimeError("Found unexpected extrusion_entity type");
            }
        }
    }
}

static void convert_to_vertices(const Slic3r::ExtrusionEntityCollection& extrusion_entity_collection, float print_z, size_t layer_id,
    size_t extruder_id, size_t color_id, EGCodeExtrusionRole extrusion_role, const Slic3r::Point& shift, std::vector<PathVertex>& vertices)
{
    for (const Slic3r::ExtrusionEntity* extrusion_entity : extrusion_entity_collection.entities) {
        if (extrusion_entity != nullptr)
            convert_to_vertices(*extrusion_entity, print_z, layer_id, extruder_id, color_id, extrusion_role, shift, vertices);
    }
}

struct VerticesData
{
    std::vector<PathVertex> vertices;
    std::vector<float> layers_zs;
};

static void convert_brim_skirt_to_vertices(const Slic3r::Print& print, std::vector<VerticesData>& vertices_data)
{
    vertices_data.emplace_back(VerticesData());
    VerticesData& data = vertices_data.back();

    // number of skirt layers
    size_t total_layer_count = 0;
    for (const Slic3r::PrintObject* print_object : print.objects()) {
        total_layer_count = std::max(total_layer_count, print_object->total_layer_count());
    }
    size_t skirt_height = print.has_infinite_skirt() ? total_layer_count : std::min<size_t>(print.config().skirt_height.value, total_layer_count);
    if (skirt_height == 0 && print.has_brim())
        skirt_height = 1;

    // Get first skirt_height layers.
    //FIXME This code is fishy. It may not work for multiple objects with different layering due to variable layer height feature.
    // This is not critical as this is just an initial preview.
    const Slic3r::PrintObject* highest_object = *std::max_element(print.objects().begin(), print.objects().end(),
        [](auto l, auto r) { return l->layers().size() < r->layers().size(); });
    data.layers_zs.reserve(skirt_height * 2);
    for (size_t i = 0; i < std::min(skirt_height, highest_object->layers().size()); ++i) {
        data.layers_zs.emplace_back(float(highest_object->layers()[i]->print_z));
    }
    // Only add skirt for the raft layers.
    for (size_t i = 0; i < std::min(skirt_height, std::min(highest_object->slicing_parameters().raft_layers(), highest_object->support_layers().size())); ++i) {
        data.layers_zs.emplace_back(float(highest_object->support_layers()[i]->print_z));
    }
    Slic3r::sort_remove_duplicates(data.layers_zs);
    skirt_height = std::min(skirt_height, data.layers_zs.size());
    data.layers_zs.erase(data.layers_zs.begin() + skirt_height, data.layers_zs.end());

    for (size_t i = 0; i < skirt_height; ++i) {
        // TODO - brim map?
        // if (i == 0)
        //     convert_to_vertices(print.brim(), data.layers_zs[i], i, 0, 0, EGCodeExtrusionRole::Skirt, Slic3r::Point(0, 0), data.vertices);
        convert_to_vertices(print.skirt(), data.layers_zs[i], i, 0, 0, EGCodeExtrusionRole::Skirt, Slic3r::Point(0, 0), data.vertices);
    }
}

class WipeTowerHelper
{
public:
    WipeTowerHelper(const Slic3r::Print& print) : m_print(print) {
        const Slic3r::PrintConfig& config = m_print.config();
        const Slic3r::WipeTowerData& wipe_tower_data = m_print.wipe_tower_data();
        if (wipe_tower_data.priming && config.single_extruder_multi_material_priming) {
            for (size_t i = 0; i < wipe_tower_data.priming.get()->size(); ++i) {
                m_priming.emplace_back(wipe_tower_data.priming.get()->at(i));
            }
        }
        if (wipe_tower_data.final_purge)
            m_final.emplace_back(*wipe_tower_data.final_purge.get());

        m_angle = print.model().wipe_tower.rotation / 180.0f * PI;
        // ORCA/BBS: plate index
        m_position = print.model().wipe_tower.positions[print.get_plate_index()].cast<float>();
        m_layers_count = wipe_tower_data.tool_changes.size() + (m_priming.empty() ? 0 : 1);
    }

    const std::vector<Slic3r::WipeTower::ToolChangeResult>& tool_change(size_t idx) {
        const auto& tool_changes = m_print.wipe_tower_data().tool_changes;
        return m_priming.empty() ?
            ((idx == tool_changes.size()) ? m_final : tool_changes[idx]) :
            ((idx == 0) ? m_priming : (idx == tool_changes.size() + 1) ? m_final : tool_changes[idx - 1]);
    }

    float get_angle() const { return m_angle; }
    const Slic3r::Vec2f& get_position() const { return m_position; }
    size_t get_layers_count() { return m_layers_count; }

private:
    const Slic3r::Print& m_print;
    std::vector<Slic3r::WipeTower::ToolChangeResult> m_priming;
    std::vector<Slic3r::WipeTower::ToolChangeResult> m_final;
    Slic3r::Vec2f m_position{ Slic3r::Vec2f::Zero() };
    float m_angle{ 0.0f };
    size_t m_layers_count{ 0 };
};

static void convert_wipe_tower_to_vertices(const Slic3r::Print& print, const std::vector<std::string>& str_tool_colors,
    std::vector<VerticesData>& vertices_data)
{
    vertices_data.emplace_back(VerticesData());
    VerticesData& data = vertices_data.back();

    WipeTowerHelper wipe_tower_helper(print);
    const float angle = wipe_tower_helper.get_angle();
    const Slic3r::Vec2f& position = wipe_tower_helper.get_position();

    for (size_t item = 0; item < wipe_tower_helper.get_layers_count(); ++item) {
        const std::vector<Slic3r::WipeTower::ToolChangeResult>& layer = wipe_tower_helper.tool_change(item);
        for (const Slic3r::WipeTower::ToolChangeResult& extrusions : layer) {
            data.layers_zs.emplace_back(extrusions.print_z);
            for (size_t i = 1; i < extrusions.extrusions.size(); /*no increment*/) {
                const Slic3r::WipeTower::Extrusion& e = extrusions.extrusions[i];
                if (e.width == 0.0f) {
                    ++i;
                    continue;
                }
                size_t j = i + 1;
                if (str_tool_colors.empty())
                    for (; j < extrusions.extrusions.size() && extrusions.extrusions[j].width > 0.0f; ++j);
                else
                    for (; j < extrusions.extrusions.size() && extrusions.extrusions[j].tool == e.tool && extrusions.extrusions[j].width > 0.0f; ++j);

                const size_t n_lines = j - i;
                Slic3r::Lines lines;
                std::vector<float> widths;
                std::vector<float> heights;
                lines.reserve(n_lines);
                widths.reserve(n_lines);
                heights.assign(n_lines, extrusions.layer_height);
                Slic3r::WipeTower::Extrusion e_prev = extrusions.extrusions[i - 1];

                if (!extrusions.priming) { // wipe tower extrusions describe the wipe tower at the origin with no rotation
                    e_prev.pos = Eigen::Rotation2Df(angle) * e_prev.pos;
                    e_prev.pos += position;
                }

                for (; i < j; ++i) {
                    Slic3r::WipeTower::Extrusion ee = extrusions.extrusions[i];
                    assert(ee.width > 0.0f);
                    if (!extrusions.priming) {
                        ee.pos = Eigen::Rotation2Df(angle) * ee.pos;
                        ee.pos += position;
                    }
                    lines.emplace_back(Slic3r::Point::new_scale(e_prev.pos.x(), e_prev.pos.y()), Slic3r::Point::new_scale(ee.pos.x(), ee.pos.y()));
                    widths.emplace_back(ee.width);
                    e_prev = ee;
                }

                convert_lines_to_vertices(lines, widths, heights, extrusions.print_z, item, static_cast<size_t>(e.tool), 0,
                                          EGCodeExtrusionRole::WipeTower, lines.front().a == lines.back().b, data.vertices);
            }
        }
    }

    Slic3r::sort_remove_duplicates(data.layers_zs);
}

class ObjectHelper
{
public:
    ObjectHelper(const std::vector<Slic3r::CustomGCode::Item>& color_print_values, size_t tool_colors_count, size_t color_print_colors_count, size_t extruders_count)
    : m_color_print_values(color_print_values)
    , m_tool_colors_count(tool_colors_count)
    , m_color_print_colors_count(color_print_colors_count)
    , m_extruders_count(extruders_count) {
    }

    uint8_t color_id(float print_z, size_t extruder_id) const {
        if (!m_color_print_values.empty())
            return color_print_color_id(double(print_z), extruder_id);
        else {
            if (m_tool_colors_count > 0)
                return std::min<uint8_t>(m_tool_colors_count - 1, static_cast<uint8_t>(extruder_id));
            else
                return 0;
        }
    }

private:
    const std::vector<Slic3r::CustomGCode::Item>& m_color_print_values;
    size_t m_tool_colors_count{ 0 };
    size_t m_color_print_colors_count{ 0 };
    size_t m_extruders_count{ 0 };

    uint8_t color_print_color_id(double print_z, size_t extruder_id) const {
        auto it = std::find_if(m_color_print_values.begin(), m_color_print_values.end(),
            [print_z](const Slic3r::CustomGCode::Item& code) {
            return std::fabs(code.print_z - print_z) < EPSILON;
        });
        if (it != m_color_print_values.end()) {
            Slic3r::CustomGCode::Type type = it->type;
            // pause print or custom Gcode
            if (type == Slic3r::CustomGCode::PausePrint || (type != Slic3r::CustomGCode::ColorChange && type != Slic3r::CustomGCode::Template))
                return static_cast<uint8_t>(m_color_print_colors_count - 1); // last color item is a gray color for pause print or custom G-code
            switch (it->type) {
            // change color for current extruder
            case Slic3r::CustomGCode::ColorChange: {
                const int c = color_change_color_id(it, extruder_id);
                if (c >= 0)
                    return static_cast<uint8_t>(c);
                break;
            }
            // change tool (extruder) 
            case Slic3r::CustomGCode::ToolChange:  { return tool_change_color_id(it, extruder_id); }
            default:                               { break; }
            }
        }

        const Slic3r::CustomGCode::Item value{ print_z + EPSILON, Slic3r::CustomGCode::Custom, 0, "" };
        it = std::lower_bound(m_color_print_values.begin(), m_color_print_values.end(), value);
        while (it != m_color_print_values.begin()) {
            --it;
            switch (it->type) {
            // change color for current extruder
            case Slic3r::CustomGCode::ColorChange: {
                const int c = color_change_color_id(it, extruder_id);
                if (c >= 0)
                    return static_cast<uint8_t>(c);
                break;
            }
            // change tool (extruder) 
            case Slic3r::CustomGCode::ToolChange:  { return tool_change_color_id(it, extruder_id); }
            default:                               { break; }
            }
        }

        return std::min<uint8_t>(m_extruders_count - 1, static_cast<uint8_t>(extruder_id));
    }

    int color_change_color_id(std::vector<Slic3r::CustomGCode::Item>::const_iterator it, size_t extruder_id) const {
        if (m_extruders_count == 1)
            return m600_color_id(it);

        auto it_n = it;
        bool is_tool_change = false;
        while (it_n != m_color_print_values.begin()) {
            --it_n;
            if (it_n->type == Slic3r::CustomGCode::ToolChange) {
                is_tool_change = true;
                if (it_n->extruder == it->extruder || (it_n->extruder == 0 && it->extruder == static_cast<int>(extruder_id + 1)))
                    return m600_color_id(it);
                break;
            }
        }
        if (!is_tool_change && it->extruder == static_cast<int>(extruder_id + 1))
            return m600_color_id(it);

        return -1;
    }

    uint8_t tool_change_color_id(std::vector<Slic3r::CustomGCode::Item>::const_iterator it, size_t extruder_id) const {
        const int current_extruder = it->extruder == 0 ? static_cast<int>(extruder_id + 1) : it->extruder;
        if (m_tool_colors_count == m_extruders_count + 1) // there is no one "M600"
            return std::min<uint8_t>(m_extruders_count - 1, std::max<uint8_t>(current_extruder - 1, 0));

        auto it_n = it;
        while (it_n != m_color_print_values.begin()) {
            --it_n;
            if (it_n->type == Slic3r::CustomGCode::ColorChange && it_n->extruder == current_extruder)
                return m600_color_id(it_n);
        }

        return std::min<uint8_t>(m_extruders_count - 1, std::max<uint8_t>(current_extruder - 1, 0));
    }

    int m600_color_id(std::vector<Slic3r::CustomGCode::Item>::const_iterator it) const {
        int shift = 0;
        while (it != m_color_print_values.begin()) {
            --it;
            if (it->type == Slic3r::CustomGCode::ColorChange)
                ++shift;
        }
        return static_cast<int>(m_extruders_count) + shift;
    }
};

static void convert_object_to_vertices(const Slic3r::PrintObject& object, const std::vector<std::string>& str_tool_colors,
    const std::vector<std::string>& str_color_print_colors, const std::vector<Slic3r::CustomGCode::Item>& color_print_values,
    size_t extruders_count, VerticesData& data)
{
    const bool has_perimeters = object.is_step_done(Slic3r::posPerimeters);
    const bool has_infill     = object.is_step_done(Slic3r::posInfill);
    const bool has_support    = object.is_step_done(Slic3r::posSupportMaterial);

    // order layers by print_z
    std::vector<const Slic3r::Layer*> layers;
    if (has_perimeters || has_infill) {
        layers.reserve(layers.size() + object.layers().size());
        std::copy(object.layers().begin(), object.layers().end(), std::back_inserter(layers));
    }
    if (has_support) {
        layers.reserve(layers.size() + object.support_layers().size());
        std::copy(object.support_layers().begin(), object.support_layers().end(), std::back_inserter(layers));
    }
    std::sort(layers.begin(), layers.end(), [](const Slic3r::Layer* l1, const Slic3r::Layer* l2) { return l1->print_z < l2->print_z; });

    ObjectHelper object_helper(color_print_values, str_tool_colors.size(), str_color_print_colors.size(), extruders_count);

    data.layers_zs.reserve(layers.size());
    for (const Slic3r::Layer* layer : layers) {
        data.layers_zs.emplace_back(static_cast<float>(layer->print_z));
    }

    Slic3r::sort_remove_duplicates(data.layers_zs);

    for (const Slic3r::Layer* layer : layers) {
        const size_t old_vertices_count = data.vertices.size();
        const float layer_z = static_cast<float>(layer->print_z);
        const auto it = std::find(data.layers_zs.begin(), data.layers_zs.end(), layer_z);
        assert(it != data.layers_zs.end());
        const size_t layer_id = (it != data.layers_zs.end()) ? std::distance(data.layers_zs.begin(), it) : 0;
        for (const Slic3r::PrintInstance& instance : object.instances()) {
            const Slic3r::Point& copy = instance.shift;
            for (const Slic3r::LayerRegion* layerm : layer->regions()) {
                if (layerm->slices.empty())
                    continue;
                const Slic3r::PrintRegionConfig& cfg = layerm->region().config();
                if (has_perimeters) {
                    const size_t extruder_id = static_cast<size_t>(std::max(cfg.wall_filament.value - 1, 0));
                    convert_to_vertices(layerm->perimeters, layer_z, layer_id, extruder_id,
                        object_helper.color_id(layer_z, extruder_id), EGCodeExtrusionRole::ExternalPerimeter,
                        copy, data.vertices);
                }
                if (has_infill) {
                    for (const Slic3r::ExtrusionEntity* ee : layerm->fills) {
                        // fill represents infill extrusions of a single island.
                        const auto& fill = *dynamic_cast<const Slic3r::ExtrusionEntityCollection*>(ee);
                        if (!fill.entities.empty()) {
                            const bool is_solid_infill = Slic3r::is_solid_infill(fill.entities.front()->role());
                            const size_t extruder_id = is_solid_infill ?
                                static_cast<size_t>(std::max(cfg.solid_infill_filament.value - 1, 0)) :
                                static_cast<size_t>(std::max(cfg.sparse_infill_filament.value - 1, 0));
                            convert_to_vertices(fill, layer_z, layer_id, extruder_id,
                                                object_helper.color_id(layer_z, extruder_id),
                                                is_solid_infill ? EGCodeExtrusionRole::SolidInfill : EGCodeExtrusionRole::InternalInfill,
                                                copy, data.vertices);
                        }
                    }
                }
            }
            if (has_support) {
                const Slic3r::SupportLayer* support_layer = dynamic_cast<const Slic3r::SupportLayer*>(layer);
                if (support_layer == nullptr)
                    continue;
                const Slic3r::PrintObjectConfig& cfg = support_layer->object()->config();
                for (const Slic3r::ExtrusionEntity* extrusion_entity : support_layer->support_fills.entities) {
                    const bool is_support_material = extrusion_entity->role() == Slic3r::ExtrusionRole::erSupportMaterial;
                    const size_t extruder_id = is_support_material ?
                        static_cast<size_t>(std::max(cfg.support_filament.value - 1, 0)) :
                        static_cast<size_t>(std::max(cfg.support_interface_filament.value - 1, 0));
                    convert_to_vertices(*extrusion_entity, layer_z, layer_id,
                                        extruder_id, object_helper.color_id(layer_z, extruder_id),
                                        is_support_material ? EGCodeExtrusionRole::SupportMaterial : EGCodeExtrusionRole::SupportMaterialInterface,
                                        copy, data.vertices);
                }
            }
        }
        // filter out empty layers
        const size_t new_vertices_count = data.vertices.size();
        if (new_vertices_count == old_vertices_count)
            data.layers_zs.erase(data.layers_zs.begin() + layer_id);
    }
}

static void convert_objects_to_vertices(const Slic3r::ConstPrintObjectPtrsAdaptor& objects, const std::vector<std::string>& str_tool_colors,
    const std::vector<std::string>& str_color_print_colors, const std::vector<Slic3r::CustomGCode::Item>& color_print_values, size_t extruders_count,
    std::vector<VerticesData>& data)
{
    // extract vertices and layers zs object by object
    data.reserve(data.size() + objects.size());
    for (size_t i = 0; i < objects.size(); ++i) {
        data.emplace_back(VerticesData());
        convert_object_to_vertices(*objects[i], str_tool_colors, str_color_print_colors, color_print_values, extruders_count, data.back());
    }
}

// mapping from Slic3r::Print to libvgcode::GCodeInputData
GCodeInputData convert(const Slic3r::Print& print, const std::vector<std::string>& str_tool_colors,
    const std::vector<std::string>& str_color_print_colors, const std::vector<Slic3r::CustomGCode::Item>& color_print_values,
    size_t extruders_count)
{
    GCodeInputData ret;
    std::vector<VerticesData> data;
    if (print.is_step_done(Slic3r::psSkirtBrim) && (print.has_skirt() || print.has_brim()))
        // extract vertices and layers zs from skirt/brim
        convert_brim_skirt_to_vertices(print, data);
    if (!print.wipe_tower_data().tool_changes.empty() && print.is_step_done(Slic3r::psWipeTower))
        // extract vertices and layers zs from wipe tower
        convert_wipe_tower_to_vertices(print, str_tool_colors, data);
    // extract vertices and layers zs from objects
    convert_objects_to_vertices(print.objects(), str_tool_colors, str_color_print_colors, color_print_values, extruders_count, data);

    // collect layers zs
    std::vector<float> layers;
    for (const VerticesData& d : data) {
        layers.reserve(layers.size() + d.layers_zs.size());
        std::copy(d.layers_zs.begin(), d.layers_zs.end(), std::back_inserter(layers));
    }
    Slic3r::sort_remove_duplicates(layers);

    // Now we need to copy the vertices into ret.vertices to be consumed by the preliminary G-code preview.
    // We need to collect vertices in the first layer for all objects, push them into the output vector
    // and then do the same for all the layers. The algorithm relies on the fact that the vertices from
    // lower layers are always placed after vertices from the higher layer.
    std::vector<size_t> vert_indices(data.size(), 0);
    for (size_t layer_id = 0; layer_id < layers.size(); ++layer_id) {
        const float layer_z = layers[layer_id];
        for (size_t obj_idx = 0; obj_idx < data.size(); ++obj_idx) {
            // d contains PathVertices for one object. Let's stuff everything below this layer_z into ret.vertices.
            const size_t start_idx = vert_indices[obj_idx];
            size_t idx = start_idx;
            while (idx < data[obj_idx].vertices.size() && data[obj_idx].vertices[idx].position[2] <= layer_z)
                ++idx;
            // We have found a vertex above current layer_z. Let's copy the vertices into the output
            // and remember where to start when we process another layer.
            ret.vertices.insert(ret.vertices.end(),
                                data[obj_idx].vertices.begin() + start_idx,
                                data[obj_idx].vertices.begin() + idx);
            vert_indices[obj_idx] = idx;
        }
    }


    // collect tool colors
    ret.tools_colors.reserve(str_tool_colors.size());
    for (const std::string& color : str_tool_colors) {
        ret.tools_colors.emplace_back(convert(color));
    }

    // collect color print colors
    const std::vector<std::string>& str_colors = str_color_print_colors.empty() ? str_tool_colors : str_color_print_colors;
    ret.color_print_colors.reserve(str_colors.size());
    for (const std::string& color : str_colors) {
        ret.color_print_colors.emplace_back(convert(color));
    }

    return ret;
}

} // namespace libvgcode

