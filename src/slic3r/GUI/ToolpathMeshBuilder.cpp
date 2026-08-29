#include "ToolpathMeshBuilder.hpp"

#include "LibVGCode/LibVGCodeWrapper.hpp"
#include <libvgcode/include/PathVertex.hpp>
#include <libvgcode/include/Viewer.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace Slic3r::GUI {
namespace {

constexpr unsigned char Flag_First = 0x01;
constexpr unsigned char Flag_Last = 0x02;
constexpr float Cap_Rounding_Factor = 0.25f;
constexpr float Direction_Epsilon = 1e-6f;

struct SegmentLocalAxes { Vec3f forward; Vec3f right; Vec3f up; };
struct Vertex { Vec3f position; Vec3f normal; };
struct CrossSection { Vertex right; Vertex top; Vertex left; Vertex bottom; };
enum class CornerType : unsigned char { RightTurn, LeftTurn, Straight };

SegmentLocalAxes segment_local_axes(const Vec3f& v1, const Vec3f& v2)
{
    SegmentLocalAxes axes;
    const Vec3f delta = v2 - v1;
    if (delta.squaredNorm() <= Direction_Epsilon * Direction_Epsilon)
        throw std::runtime_error("Degenerate libvgcode extrusion segment");
    axes.forward = delta.normalized();
    axes.right = axes.forward.cross(Vec3f::UnitZ()).normalized();
    axes.up = axes.right.cross(axes.forward);
    return axes;
}

CrossSection cross_section(const Vec3f& v, const Vec3f& right, const Vec3f& up, float width, float height)
{
    const Vec3f w_shift = 0.5f * width * right;
    const Vec3f h_shift = 0.5f * height * up;
    return {{v + w_shift, right}, {v + h_shift, up}, {v - w_shift, -right}, {v - h_shift, -up}};
}

CrossSection normal_cross_section(const Vec3f& v, const SegmentLocalAxes& axes, float width, float height)
{
    return cross_section(v, axes.right, axes.up, width, height);
}

CrossSection corner_cross_section(const Vec3f& v, const SegmentLocalAxes& axes1, const SegmentLocalAxes& axes2,
                                  float width, float height, CornerType& corner_type)
{
    if (std::abs(std::abs(axes1.forward.dot(axes2.forward)) - 1.0f) < Direction_Epsilon)
        corner_type = CornerType::Straight;
    else if (axes1.up.dot(axes1.forward.cross(axes2.forward)) < 0.0f)
        corner_type = CornerType::RightTurn;
    else
        corner_type = CornerType::LeftTurn;
    Vec3f right = axes1.right + axes2.right;
    if (right.squaredNorm() <= Direction_Epsilon * Direction_Epsilon)
        right = axes1.right;
    else
        right.normalize();
    return cross_section(v, right, axes1.up, width, height);
}

class Builder
{
public:
    Builder(const libvgcode::Viewer& viewer, PreviewMeshScope scope) : m_viewer(&viewer), m_scope(scope) {}
    explicit Builder(const libvgcode::GCodeInputData& data) : m_data(&data) {}

    PreviewTriangleMesh build()
    {
        const size_t count = vertices_count();
        if (count < 3)
            return {};
        libvgcode::Interval range{0, static_cast<uint32_t>(count - 1)};
        if (m_viewer && m_scope == PreviewMeshScope::Visible) {
            range = m_viewer->get_view_visible_range();
            if (range[0] >= count || range[1] <= range[0] || range[1] >= count)
                range = {0, static_cast<uint32_t>(count - 1)};
            if (m_viewer->is_top_layer_only_view_range())
                range[0] = m_viewer->get_view_full_range()[0];
        }

        const size_t last = std::min<size_t>(range[1] - 1, count - 2);
        if (range[0] > last)
            return {};
        for (size_t i = range[0]; i <= last; ++i) {
            const auto& curr = vertex_at(i);
            const auto& next = vertex_at(i + 1);
            if (!curr.is_extrusion() || !next.is_extrusion()) {
                m_need_start = true;
                continue;
            }
            const Vec3f first = libvgcode::convert(curr.position);
            const Vec3f second = libvgcode::convert(next.position);
            if ((second - first).squaredNorm() <= Direction_Epsilon * Direction_Epsilon) {
                m_need_start = true;
                continue;
            }
            const auto& nextnext = i + 2 < count ? vertex_at(i + 2) : next;
            unsigned char flags = 0;
            if (m_need_start || curr.gcode_id == next.gcode_id)
                flags |= Flag_First;
            if (i == last || !nextnext.is_extrusion())
                flags |= Flag_Last;
            emit_segment(flags, i, curr, next, nextnext);
            m_need_start = (flags & Flag_Last) != 0;
        }
        return std::move(m_mesh);
    }

private:
    const libvgcode::Viewer* m_viewer{};
    const libvgcode::GCodeInputData* m_data{};
    PreviewMeshScope m_scope{PreviewMeshScope::Complete};
    PreviewTriangleMesh m_mesh;
    bool m_need_start{true};

    size_t vertices_count() const { return m_viewer ? m_viewer->get_vertices_count() : m_data->vertices.size(); }
    const libvgcode::PathVertex& vertex_at(size_t index) const {
        return m_viewer ? m_viewer->get_vertex_at(index) : m_data->vertices[index];
    }
    libvgcode::Color vertex_color(const libvgcode::PathVertex& vertex) const {
        if (m_viewer)
            return m_viewer->get_vertex_color(vertex);
        return vertex.extruder_id < m_data->tools_colors.size() ? m_data->tools_colors[vertex.extruder_id] : libvgcode::DUMMY_COLOR;
    }

    uint32_t add_vertex(const Vertex& vertex)
    {
        if (!vertex.position.allFinite() || !vertex.normal.allFinite())
            throw std::runtime_error("Non-finite libvgcode preview mesh vertex");
        const uint32_t index = static_cast<uint32_t>(m_mesh.vertices.size());
        m_mesh.vertices.push_back({vertex.position, vertex.normal, Vec2f::Zero()});
        m_mesh.bounds.merge(vertex.position.cast<double>());
        return index;
    }

    void add_triangle(uint32_t a, uint32_t b, uint32_t c)
    {
        m_mesh.indices.insert(m_mesh.indices.end(), {a, b, c});
        m_mesh.groups.back().index_count += 3;
    }

    void begin_group(size_t vertex_id, const libvgcode::PathVertex& vertex)
    {
        const libvgcode::Color color = vertex_color(vertex);
        const uint16_t material = vertex.extruder_id;
        const uint8_t role = static_cast<uint8_t>(vertex.role);
        const uint8_t extruder = vertex.extruder_id;
        const uint8_t colour = vertex.color_id;
        const uint32_t layer = vertex.layer_id;
        if (!m_mesh.groups.empty()) {
            auto& group = m_mesh.groups.back();
            if (group.material_slot == material && group.extrusion_role == role && group.extruder_id == extruder &&
                group.colour_id == colour && group.layer_id == layer && group.color == color)
                return;
        }
        (void) vertex_id;
        PreviewTriangleGroup group;
        group.first_index = static_cast<uint32_t>(m_mesh.indices.size());
        group.material_slot = material;
        group.extrusion_role = role;
        group.extruder_id = extruder;
        group.colour_id = colour;
        group.layer_id = layer;
        group.color = color;
        m_mesh.groups.push_back(group);
    }

    void emit_segment(unsigned char flags, size_t v1_id, const libvgcode::PathVertex& v1,
                      const libvgcode::PathVertex& v2, const libvgcode::PathVertex& v3)
    {
        const Vec3f p1 = libvgcode::convert(v1.position);
        const Vec3f p2 = libvgcode::convert(v2.position);
        const Vec3f p3 = libvgcode::convert(v3.position);
        const SegmentLocalAxes axes12 = segment_local_axes(p1, p2);
        const SegmentLocalAxes axes23 = (p3 - p2).squaredNorm() > Direction_Epsilon * Direction_Epsilon ?
            segment_local_axes(p2, p3) : axes12;

        if ((flags & Flag_First) != 0) {
            begin_group(v1_id, v1);
            const uint32_t base = static_cast<uint32_t>(m_mesh.vertices.size());
            const Vertex cap = {p1 - Cap_Rounding_Factor * v1.width * axes12.forward, -axes12.forward};
            const CrossSection section = normal_cross_section(p1, axes12, v1.width, v1.height);
            add_vertex(cap); add_vertex(section.right); add_vertex(section.top); add_vertex(section.left); add_vertex(section.bottom);
            add_triangle(base, base + 1, base + 2); add_triangle(base, base + 2, base + 3);
            add_triangle(base, base + 3, base + 4); add_triangle(base, base + 4, base + 1);
        }

        begin_group(v1_id + 1, v2);
        if ((flags & Flag_Last) != 0) {
            const uint32_t previous = static_cast<uint32_t>(m_mesh.vertices.size()) - 4;
            const uint32_t base = static_cast<uint32_t>(m_mesh.vertices.size());
            const Vertex cap = {p2 + Cap_Rounding_Factor * v2.width * axes12.forward, axes12.forward};
            const CrossSection section = normal_cross_section(p2, axes12, v2.width, v2.height);
            add_vertex(cap); add_vertex(section.right); add_vertex(section.top); add_vertex(section.left); add_vertex(section.bottom);
            add_triangle(previous, base + 1, base + 2); add_triangle(previous, base + 2, previous + 1);
            add_triangle(previous + 1, base + 2, base + 3); add_triangle(previous + 1, base + 3, previous + 2);
            add_triangle(previous + 2, base + 3, base + 4); add_triangle(previous + 2, base + 4, previous + 3);
            add_triangle(previous + 3, base + 4, base + 1); add_triangle(previous + 3, base + 1, previous);
            add_triangle(base, base + 3, base + 2); add_triangle(base, base + 2, base + 1);
            add_triangle(base, base + 1, base + 4); add_triangle(base, base + 4, base + 3);
            return;
        }

        CornerType corner_type;
        const CrossSection corner = corner_cross_section(p2, axes12, axes23, v2.width, v2.height, corner_type);
        const CrossSection before = normal_cross_section(p2, axes12, v2.width, v2.height);
        const CrossSection after = normal_cross_section(p2, axes23, v2.width, v2.height);
        const uint32_t previous = static_cast<uint32_t>(m_mesh.vertices.size()) - 4;
        const uint32_t base = static_cast<uint32_t>(m_mesh.vertices.size());
        if (corner_type == CornerType::Straight) {
            add_vertex(before.right); add_vertex(before.top); add_vertex(before.left); add_vertex(before.bottom);
            add_triangle(previous, base, base + 1); add_triangle(previous, base + 1, previous + 1);
            add_triangle(previous + 1, base + 1, base + 2); add_triangle(previous + 1, base + 2, previous + 2);
            add_triangle(previous + 2, base + 2, base + 3); add_triangle(previous + 2, base + 3, previous + 3);
            add_triangle(previous + 3, base + 3, base); add_triangle(previous + 3, base, previous);
        } else if (corner_type == CornerType::RightTurn) {
            add_vertex(before.left); add_vertex(corner.left); add_vertex(corner.right); add_vertex(before.top);
            add_vertex(after.left); add_vertex(before.bottom);
            add_triangle(previous, base + 2, base + 3); add_triangle(previous, base + 3, previous + 1);
            add_triangle(previous + 1, base + 3, base); add_triangle(previous + 1, base, previous + 2);
            add_triangle(previous + 2, base, base + 5); add_triangle(previous + 2, base + 5, previous + 3);
            add_triangle(previous + 3, base + 5, base + 2); add_triangle(previous + 3, base + 2, previous);
            add_triangle(base + 1, base, base + 3); add_triangle(base + 1, base + 3, base + 4);
            add_triangle(base + 1, base + 4, base + 5); add_triangle(base + 1, base + 5, base);
        } else {
            add_vertex(before.right); add_vertex(corner.right); add_vertex(after.right); add_vertex(before.top);
            add_vertex(corner.left); add_vertex(before.bottom);
            add_triangle(previous, base, base + 3); add_triangle(previous, base + 3, previous + 1);
            add_triangle(previous + 1, base + 3, base + 4); add_triangle(previous + 1, base + 4, previous + 2);
            add_triangle(previous + 2, base + 4, base + 5); add_triangle(previous + 2, base + 5, previous + 3);
            add_triangle(previous + 3, base + 5, base); add_triangle(previous + 3, base, previous);
            add_triangle(base + 1, base + 2, base + 3); add_triangle(base + 1, base + 3, base);
            add_triangle(base + 1, base, base + 5); add_triangle(base + 1, base + 5, base + 2);
        }
    }
};

} // namespace

PreviewTriangleMesh build_preview_triangle_mesh(const libvgcode::Viewer& viewer, PreviewMeshScope scope)
{
    return Builder(viewer, scope).build();
}

PreviewTriangleMesh build_preview_triangle_mesh(const libvgcode::GCodeInputData& data)
{
    return Builder(data).build();
}

} // namespace Slic3r::GUI
