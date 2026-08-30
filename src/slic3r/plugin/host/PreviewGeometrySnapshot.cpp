#include "PreviewGeometrySnapshot.hpp"

#include "slic3r/GUI/ToolpathMeshBuilder.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/Format/GLB.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <stdexcept>

namespace Slic3r::PreviewGeometrySnapshot {
namespace {

uint8_t channel(float value)
{
    return uint8_t(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

GLB::Scene make_scene(const Snapshot& snapshot)
{
    GLB::Scene scene;
    scene.name = "OrcaSlicer preview";
    scene.bounds_min = snapshot.bounds_min;
    scene.bounds_max = snapshot.bounds_max;
    scene.root_rotation = {{-0.7071067811865476, 0, 0, 0.7071067811865476}};
    scene.extras = {{"orca", {{"sceneId", snapshot.scene_id},
                               {"plateIndex", snapshot.plate_index},
                               {"spiralVase", (snapshot.flags & FLAG_SPIRAL_VASE) != 0},
                               {"zOffset", snapshot.z_offset},
                               {"printableHeight", snapshot.printable_height}}}};

    scene.vertices.reserve(snapshot.vertices.size());
    for (const Vertex& vertex : snapshot.vertices)
        scene.vertices.push_back({vertex.position, vertex.normal, vertex.uv});
    scene.indices = snapshot.indices;
    scene.primitives.reserve(snapshot.groups.size());
    uint64_t covered_indices = 0;
    for (const Group& group : snapshot.groups) {
        if (group.first_index != covered_indices || group.index_count == 0 || group.index_count % 3 != 0 ||
            uint64_t(group.first_index) + group.index_count > snapshot.indices.size())
            throw std::runtime_error("Preview groups do not cover the index buffer");
        scene.primitives.push_back({group.first_index, group.index_count, group.material_slot,
                                    {{"orca", {{"layerId", group.layer_id},
                                                {"extrusionRole", group.extrusion_role},
                                                {"extruderId", group.extruder_id},
                                                {"colourId", group.colour_id}}}}});
        covered_indices += group.index_count;
    }
    if (covered_indices != snapshot.indices.size())
        throw std::runtime_error("Preview groups do not cover the index buffer");
    scene.materials.reserve(snapshot.materials.size());
    for (const MaterialSlot& material : snapshot.materials)
        scene.materials.push_back({material.display_name, material.rgba,
                                   {{"orca", {{"extruderId", material.extruder_id},
                                               {"presetId", material.preset_id}}}}});

    const auto add_area = [&](const std::vector<Point>& points, const char* kind) {
        if (points.size() < 2)
            return;
        GLB::Polyline polyline;
        polyline.closed = true;
        polyline.extras = {{"orca", {{"areaKind", kind}}}};
        polyline.points.reserve(points.size());
        for (const Point& point : points)
            polyline.points.push_back({point.x, point.y, snapshot.z_offset});
        scene.polylines.push_back(std::move(polyline));
    };
    add_area(snapshot.printable_area, "printable");
    add_area(snapshot.bed_excluded_area, "bed-excluded");
    add_area(snapshot.wrapping_excluded_area, "wrapping-excluded");
    return scene;
}

} // namespace

Snapshot capture(const GUI::PreviewTriangleMesh& mesh, const GCodeProcessorResult& result, int plate_index,
                 uint64_t expected_scene_id, const Pointfs& fallback_printable_area)
{
    if (mesh.vertices.empty() || mesh.indices.empty() || mesh.groups.empty())
        throw std::runtime_error("Preview mesh is empty");

    Snapshot out;
    out.scene_id = expected_scene_id;
    out.plate_index = plate_index;
    std::vector<std::string> colors;
    std::vector<std::string> filament_ids;
    size_t filament_count{};
    {
        std::lock_guard<std::mutex> lock(result.result_mutex);
        if (result.id != expected_scene_id)
            throw std::runtime_error("Slice result changed before preview capture");
        out.flags = result.spiral_vase_mode ? FLAG_SPIRAL_VASE : 0;
        out.z_offset = result.z_offset;
        out.printable_height = result.printable_height;
        colors = result.extruder_colors;
        filament_ids = result.settings_ids.filament;
        filament_count = result.filaments_count;
        const auto copy = [](const Pointfs& source, std::vector<Point>& target) {
            target.reserve(source.size());
            for (const Vec2d& point : source)
                target.push_back({float(point.x()), float(point.y())});
        };
        copy(result.printable_area.empty() ? fallback_printable_area : result.printable_area, out.printable_area);
        copy(result.bed_exclude_area, out.bed_excluded_area);
        copy(result.wrapping_exclude_area, out.wrapping_excluded_area);
    }

    out.vertices.reserve(mesh.vertices.size());
    for (const auto& vertex : mesh.vertices)
        out.vertices.push_back({{vertex.position.x(), vertex.position.y(), vertex.position.z()},
                                {vertex.normal.x(), vertex.normal.y(), vertex.normal.z()},
                                {vertex.uv.x(), vertex.uv.y()}});
    out.indices = mesh.indices;
    uint16_t max_slot = 0;
    for (const auto& group : mesh.groups) {
        out.groups.push_back({group.first_index, group.index_count, group.material_slot, group.extrusion_role,
                              group.extruder_id, group.colour_id, group.layer_id});
        max_slot = std::max(max_slot, group.material_slot);
    }

    const size_t material_count = std::max({colors.size(), filament_ids.size(), filament_count, size_t(max_slot) + 1});
    for (size_t i = 0; i < material_count; ++i) {
        MaterialSlot material;
        material.extruder_id = uint8_t(std::min<size_t>(i, 255));
        ColorRGBA color;
        if (i >= colors.size() || !decode_color(colors[i], color))
            color = ColorRGBA::GRAY();
        material.rgba = {{channel(color.r()), channel(color.g()), channel(color.b()), channel(color.a())}};
        if (i < filament_ids.size()) {
            material.preset_id = filament_ids[i];
            material.display_name = filament_ids[i];
        }
        out.materials.push_back(std::move(material));
    }
    if (mesh.bounds.defined)
        for (size_t i = 0; i < 3; ++i) {
            out.bounds_min[i] = mesh.bounds.min[int(i)];
            out.bounds_max[i] = mesh.bounds.max[int(i)];
        }
    return out;
}

std::vector<uint8_t> serialize(const Snapshot& snapshot)
{
    return GLB::serialize(make_scene(snapshot));
}

DocumentInfo validate(const uint8_t* data, size_t size, uint64_t max_file_size)
{
    const GLB::DocumentInfo glb = GLB::validate(data, size, max_file_size);
    DocumentInfo info;
    info.version = glb.version;
    info.file_size = glb.file_size;
    info.vertex_count = glb.vertex_count;
    info.index_count = glb.index_count;
    info.primitive_count = glb.primitive_count;
    info.material_count = glb.material_count;
    if (glb.scene_extras.contains("orca"))
        info.scene_id = glb.scene_extras["orca"].value("sceneId", uint64_t(0));
    return info;
}

DocumentInfo validate(const std::vector<uint8_t>& data, uint64_t max_file_size)
{
    return validate(data.data(), data.size(), max_file_size);
}

void write_atomic(const Snapshot& snapshot, const boost::filesystem::path& target)
{
    store_glb(target.string().c_str(), make_scene(snapshot));
}

} // namespace Slic3r::PreviewGeometrySnapshot
