#include "VertexColorRegionEditor.hpp"
#include "ModelArtifact.hpp"

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <numeric>
#include <queue>
#include <sstream>
#include <unordered_map>

namespace Slic3r::AI {
namespace {

constexpr float PI = 3.14159265358979323846f;
constexpr size_t PICK_BVH_LEAF_SIZE = 8;

struct PositionCell
{
    int64_t x {0};
    int64_t y {0};
    int64_t z {0};

    bool operator==(const PositionCell& other) const
    {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct PositionCellHash
{
    size_t operator()(const PositionCell& cell) const
    {
        size_t seed = std::hash<int64_t> {}(cell.x);
        seed ^= std::hash<int64_t> {}(cell.y) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        seed ^= std::hash<int64_t> {}(cell.z) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        return seed;
    }
};

struct IndexedEdgeOwner
{
    uint32_t first_vertex {0};
    uint32_t second_vertex {0};
    uint32_t first_face {0};
    uint32_t use_count {1};
};

uint64_t edge_key(uint32_t first, uint32_t second)
{
    if (first > second)
        std::swap(first, second);
    return (uint64_t(first) << 32) | uint64_t(second);
}

PositionCell position_cell(const Vec3f& position, double cell_size)
{
    return {
        int64_t(std::floor(double(position.x()) / cell_size)),
        int64_t(std::floor(double(position.y()) / cell_size)),
        int64_t(std::floor(double(position.z()) / cell_size))
    };
}

float color_distance_squared(const RGBA& left, const RGBA& right)
{
    const float red = left[0] - right[0];
    const float green = left[1] - right[1];
    const float blue = left[2] - right[2];
    return red * red + green * green + blue * blue;
}

bool ray_triangle_intersection(const Vec3d& origin, const Vec3d& direction,
                               const Vec3f& a_float, const Vec3f& b_float, const Vec3f& c_float,
                               double& distance)
{
    constexpr double epsilon = 1e-9;
    const Vec3d a = a_float.cast<double>();
    const Vec3d b = b_float.cast<double>();
    const Vec3d c = c_float.cast<double>();
    const Vec3d edge_a = b - a;
    const Vec3d edge_b = c - a;
    const Vec3d p = direction.cross(edge_b);
    const double determinant = edge_a.dot(p);
    if (std::abs(determinant) < epsilon)
        return false;
    const double inverse = 1.0 / determinant;
    const Vec3d offset = origin - a;
    const double u = offset.dot(p) * inverse;
    if (u < 0.0 || u > 1.0)
        return false;
    const Vec3d q = offset.cross(edge_a);
    const double v = direction.dot(q) * inverse;
    if (v < 0.0 || u + v > 1.0)
        return false;
    distance = edge_b.dot(q) * inverse;
    return distance > epsilon;
}

bool ray_box_intersection(const Vec3d& origin, const Vec3d& direction,
                          const Vec3f& minimum_float, const Vec3f& maximum_float,
                          double maximum_distance, double& entry_distance)
{
    constexpr double direction_epsilon = 1e-12;
    constexpr double bounds_epsilon = 1e-7;
    const Vec3d minimum = minimum_float.cast<double>().array() - bounds_epsilon;
    const Vec3d maximum = maximum_float.cast<double>().array() + bounds_epsilon;
    double near_distance = 0.0;
    double far_distance = maximum_distance;
    for (size_t axis = 0; axis < 3; ++axis) {
        if (std::abs(direction[axis]) <= direction_epsilon) {
            if (origin[axis] < minimum[axis] || origin[axis] > maximum[axis])
                return false;
            continue;
        }
        double first = (minimum[axis] - origin[axis]) / direction[axis];
        double second = (maximum[axis] - origin[axis]) / direction[axis];
        if (first > second)
            std::swap(first, second);
        near_distance = std::max(near_distance, first);
        far_distance = std::min(far_distance, second);
        if (near_distance > far_distance)
            return false;
    }
    entry_distance = near_distance;
    return far_distance > 0.0;
}

} // namespace

bool VertexColorRegionEditor::initialize(indexed_triangle_set mesh, std::vector<RGBA> vertex_colors,
                                         std::string& error)
{
    clear();
    if (mesh.indices.empty() || mesh.vertices.empty()) {
        error = "The OBJ contains no selectable triangles.";
        return false;
    }
    if (vertex_colors.size() != mesh.vertices.size()) {
        error = "Local recoloring requires OBJ vertex colors.";
        return false;
    }

    m_mesh = std::move(mesh);
    m_vertex_colors = std::move(vertex_colors);
    m_face_normals.resize(m_mesh.indices.size());
    m_face_centers.resize(m_mesh.indices.size());
    m_face_neighbors.resize(m_mesh.indices.size());
    m_selected_faces.assign(m_mesh.indices.size(), 0);

    Vec3f minimum = m_mesh.vertices.front();
    Vec3f maximum = minimum;
    for (const Vec3f& vertex : m_mesh.vertices) {
        minimum = minimum.cwiseMin(vertex);
        maximum = maximum.cwiseMax(vertex);
    }
    m_mesh_diagonal = (maximum - minimum).norm();

    std::unordered_map<uint64_t, IndexedEdgeOwner> indexed_edges;
    indexed_edges.reserve(m_mesh.indices.size() * 3);
    for (size_t face_index = 0; face_index < m_mesh.indices.size(); ++face_index) {
        const stl_triangle_vertex_indices& face = m_mesh.indices[face_index];
        const Vec3f& a = m_mesh.vertices[face[0]];
        const Vec3f& b = m_mesh.vertices[face[1]];
        const Vec3f& c = m_mesh.vertices[face[2]];
        Vec3f normal = (b - a).cross(c - a);
        if (normal.squaredNorm() > 1e-12f)
            normal.normalize();
        else
            normal = Vec3f::UnitZ();
        m_face_normals[face_index] = normal;
        m_face_centers[face_index] = (a + b + c) / 3.0f;

        const std::array<std::pair<uint32_t, uint32_t>, 3> edges {{
            {uint32_t(face[0]), uint32_t(face[1])},
            {uint32_t(face[1]), uint32_t(face[2])},
            {uint32_t(face[2]), uint32_t(face[0])}
        }};
        for (const auto& edge : edges) {
            const uint64_t key = edge_key(edge.first, edge.second);
            const auto [owner, inserted] = indexed_edges.emplace(
                key, IndexedEdgeOwner {edge.first, edge.second, uint32_t(face_index), 1});
            if (!inserted) {
                ++owner->second.use_count;
                if (owner->second.first_face != face_index) {
                    m_face_neighbors[face_index].push_back(owner->second.first_face);
                    m_face_neighbors[owner->second.first_face].push_back(uint32_t(face_index));
                }
            }
        }
    }

    // OBJ exporters commonly duplicate vertices along UV or material seams. Weld only
    // boundary-edge endpoints for selection adjacency; the mesh and its indices remain unchanged.
    const double position_tolerance = std::clamp(double(m_mesh_diagonal) * 1e-7, 1e-7, 1e-4);
    const double tolerance_squared = position_tolerance * position_tolerance;
    std::unordered_map<PositionCell, std::vector<uint32_t>, PositionCellHash> vertices_by_cell;
    vertices_by_cell.reserve(m_mesh.vertices.size());
    std::vector<uint32_t> canonical_vertices(m_mesh.vertices.size());
    for (size_t vertex_index = 0; vertex_index < m_mesh.vertices.size(); ++vertex_index) {
        const Vec3f& vertex = m_mesh.vertices[vertex_index];
        const PositionCell cell = position_cell(vertex, position_tolerance);
        uint32_t canonical = std::numeric_limits<uint32_t>::max();
        for (int64_t dx = -1; dx <= 1; ++dx) {
            for (int64_t dy = -1; dy <= 1; ++dy) {
                for (int64_t dz = -1; dz <= 1; ++dz) {
                    const auto candidates = vertices_by_cell.find({cell.x + dx, cell.y + dy, cell.z + dz});
                    if (candidates == vertices_by_cell.end())
                        continue;
                    for (uint32_t candidate : candidates->second) {
                        if ((m_mesh.vertices[candidate].cast<double>() - vertex.cast<double>()).squaredNorm() <=
                            tolerance_squared)
                            canonical = std::min(canonical, candidate);
                    }
                }
            }
        }
        if (canonical == std::numeric_limits<uint32_t>::max()) {
            canonical = uint32_t(vertex_index);
            vertices_by_cell[cell].push_back(canonical);
        }
        canonical_vertices[vertex_index] = canonical;
    }

    std::unordered_map<uint64_t, std::vector<uint32_t>> faces_by_geometric_edge;
    faces_by_geometric_edge.reserve(indexed_edges.size());
    for (const auto& item : indexed_edges) {
        const IndexedEdgeOwner& edge = item.second;
        if (edge.use_count != 1)
            continue;
        const uint32_t first = canonical_vertices[edge.first_vertex];
        const uint32_t second = canonical_vertices[edge.second_vertex];
        if (first != second)
            faces_by_geometric_edge[edge_key(first, second)].push_back(edge.first_face);
    }
    for (const auto& item : faces_by_geometric_edge) {
        const std::vector<uint32_t>& faces = item.second;
        if (faces.size() != 2 || faces[0] == faces[1])
            continue;
        m_face_neighbors[faces[0]].push_back(faces[1]);
        m_face_neighbors[faces[1]].push_back(faces[0]);
    }
    for (std::vector<uint32_t>& neighbors : m_face_neighbors) {
        std::sort(neighbors.begin(), neighbors.end());
        neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());
    }

    m_pick_face_order.resize(m_mesh.indices.size());
    std::iota(m_pick_face_order.begin(), m_pick_face_order.end(), uint32_t(0));
    m_pick_nodes.reserve(std::max<size_t>(1, m_mesh.indices.size() / 2));
    build_pick_bvh(0, m_pick_face_order.size());
    return true;
}

uint32_t VertexColorRegionEditor::build_pick_bvh(size_t begin, size_t end)
{
    PickBvhNode node;
    node.minimum = Vec3f::Constant(std::numeric_limits<float>::infinity());
    node.maximum = Vec3f::Constant(-std::numeric_limits<float>::infinity());
    Vec3f center_minimum = node.minimum;
    Vec3f center_maximum = node.maximum;
    for (size_t item = begin; item < end; ++item) {
        const uint32_t face_index = m_pick_face_order[item];
        const stl_triangle_vertex_indices& face = m_mesh.indices[face_index];
        for (size_t corner = 0; corner < 3; ++corner) {
            const Vec3f& vertex = m_mesh.vertices[face[corner]];
            node.minimum = node.minimum.cwiseMin(vertex);
            node.maximum = node.maximum.cwiseMax(vertex);
        }
        center_minimum = center_minimum.cwiseMin(m_face_centers[face_index]);
        center_maximum = center_maximum.cwiseMax(m_face_centers[face_index]);
    }

    const uint32_t node_index = uint32_t(m_pick_nodes.size());
    m_pick_nodes.emplace_back(node);
    const size_t count = end - begin;
    if (count <= PICK_BVH_LEAF_SIZE) {
        m_pick_nodes[node_index].first = uint32_t(begin);
        m_pick_nodes[node_index].count = uint32_t(count);
        return node_index;
    }

    Eigen::Index split_axis = 0;
    (center_maximum - center_minimum).maxCoeff(&split_axis);
    const size_t middle = begin + count / 2;
    std::nth_element(
        m_pick_face_order.begin() + begin,
        m_pick_face_order.begin() + middle,
        m_pick_face_order.begin() + end,
        [this, split_axis](uint32_t left, uint32_t right) {
            const float left_value = m_face_centers[left][split_axis];
            const float right_value = m_face_centers[right][split_axis];
            return left_value == right_value ? left < right : left_value < right_value;
        });
    const uint32_t left = build_pick_bvh(begin, middle);
    const uint32_t right = build_pick_bvh(middle, end);
    m_pick_nodes[node_index].left = left;
    m_pick_nodes[node_index].right = right;
    return node_index;
}

void VertexColorRegionEditor::clear()
{
    m_mesh = {};
    m_vertex_colors.clear();
    m_face_color_overrides.clear();
    m_face_normals.clear();
    m_face_centers.clear();
    m_face_neighbors.clear();
    m_pick_face_order.clear();
    m_pick_nodes.clear();
    m_selected_faces.clear();
    m_selected_face_count = 0;
    m_mesh_diagonal = 0.0f;
}

std::optional<size_t> VertexColorRegionEditor::pick_face(const Vec3d& ray_origin,
                                                         const Vec3d& ray_direction) const
{
    if (!ready() || ray_direction.squaredNorm() < 1e-12)
        return std::nullopt;
    const Vec3d direction = ray_direction.normalized();
    double nearest = std::numeric_limits<double>::infinity();
    std::optional<size_t> result;
    if (m_pick_nodes.empty())
        return result;

    struct PendingNode
    {
        uint32_t index;
        double entry_distance;
    };
    double root_distance = 0.0;
    if (!ray_box_intersection(ray_origin, direction, m_pick_nodes.front().minimum,
                              m_pick_nodes.front().maximum, nearest, root_distance))
        return result;
    std::vector<PendingNode> pending {{0, root_distance}};
    pending.reserve(64);
    while (!pending.empty()) {
        const PendingNode current = pending.back();
        pending.pop_back();
        if (current.entry_distance > nearest)
            continue;
        const PickBvhNode& node = m_pick_nodes[current.index];
        if (node.is_leaf()) {
            for (uint32_t item = node.first; item < node.first + node.count; ++item) {
                const size_t face_index = m_pick_face_order[item];
                const stl_triangle_vertex_indices& face = m_mesh.indices[face_index];
                double distance = 0.0;
                if (!ray_triangle_intersection(ray_origin, direction,
                                               m_mesh.vertices[face[0]], m_mesh.vertices[face[1]],
                                               m_mesh.vertices[face[2]], distance))
                    continue;
                if (distance < nearest - 1e-9 ||
                    (std::abs(distance - nearest) <= 1e-9 && (!result || face_index < *result))) {
                    nearest = distance;
                    result = face_index;
                }
            }
            continue;
        }

        const PickBvhNode& left = m_pick_nodes[node.left];
        const PickBvhNode& right = m_pick_nodes[node.right];
        double left_distance = 0.0;
        double right_distance = 0.0;
        const bool hit_left = ray_box_intersection(
            ray_origin, direction, left.minimum, left.maximum, nearest, left_distance);
        const bool hit_right = ray_box_intersection(
            ray_origin, direction, right.minimum, right.maximum, nearest, right_distance);
        if (hit_left && hit_right) {
            if (left_distance <= right_distance) {
                pending.push_back({node.right, right_distance});
                pending.push_back({node.left, left_distance});
            } else {
                pending.push_back({node.left, left_distance});
                pending.push_back({node.right, right_distance});
            }
        } else if (hit_left) {
            pending.push_back({node.left, left_distance});
        } else if (hit_right) {
            pending.push_back({node.right, right_distance});
        }
    }
    return result;
}

RGBA VertexColorRegionEditor::corner_color(size_t face_index, size_t corner) const
{
    const auto edited = m_face_color_overrides.find(face_index);
    return edited == m_face_color_overrides.end()
        ? m_vertex_colors[m_mesh.indices[face_index][corner]] : edited->second;
}

RGBA VertexColorRegionEditor::face_color(size_t face_index) const
{
    const auto edited = m_face_color_overrides.find(face_index);
    if (edited != m_face_color_overrides.end())
        return edited->second;
    RGBA result {0.0f, 0.0f, 0.0f, 1.0f};
    const stl_triangle_vertex_indices& face = m_mesh.indices[face_index];
    for (size_t channel = 0; channel < 4; ++channel)
        result[channel] = (m_vertex_colors[face[0]][channel] + m_vertex_colors[face[1]][channel] +
                           m_vertex_colors[face[2]][channel]) / 3.0f;
    return result;
}

std::vector<size_t> VertexColorRegionEditor::smart_region(
    size_t seed_face, const RegionSelectionSettings& settings) const
{
    std::vector<size_t> result;
    if (seed_face >= m_mesh.indices.size())
        return result;
    const RGBA seed_color = face_color(seed_face);
    const float maximum_color_distance = settings.color_distance * settings.color_distance;
    const float minimum_normal_dot = std::cos(settings.normal_angle_degrees * PI / 180.0f);
    std::vector<uint8_t> visited(m_mesh.indices.size(), 0);
    std::queue<size_t> pending;
    visited[seed_face] = 1;
    pending.push(seed_face);
    while (!pending.empty()) {
        const size_t current = pending.front();
        pending.pop();
        result.push_back(current);
        for (uint32_t neighbor : m_face_neighbors[current]) {
            if (visited[neighbor])
                continue;
            if (color_distance_squared(face_color(neighbor), seed_color) > maximum_color_distance)
                continue;
            if (m_face_normals[current].dot(m_face_normals[neighbor]) < minimum_normal_dot)
                continue;
            visited[neighbor] = 1;
            pending.push(neighbor);
        }
    }
    return result;
}

std::vector<size_t> VertexColorRegionEditor::local_patch(
    size_t seed_face, const RegionSelectionSettings& settings) const
{
    std::vector<size_t> result;
    if (seed_face >= m_mesh.indices.size())
        return result;
    const Vec3f seed_center = m_face_centers[seed_face];
    const float radius = std::max(1e-5f, m_mesh_diagonal * settings.local_radius_ratio);
    const float radius_squared = radius * radius;
    const float minimum_normal_dot = std::cos(std::min(88.0f, settings.normal_angle_degrees + 15.0f) *
                                               PI / 180.0f);
    std::vector<uint8_t> visited(m_mesh.indices.size(), 0);
    std::queue<size_t> pending;
    visited[seed_face] = 1;
    pending.push(seed_face);
    while (!pending.empty()) {
        const size_t current = pending.front();
        pending.pop();
        result.push_back(current);
        for (uint32_t neighbor : m_face_neighbors[current]) {
            if (visited[neighbor])
                continue;
            if ((m_face_centers[neighbor] - seed_center).squaredNorm() > radius_squared)
                continue;
            if (m_face_normals[current].dot(m_face_normals[neighbor]) < minimum_normal_dot)
                continue;
            visited[neighbor] = 1;
            pending.push(neighbor);
        }
    }
    return result;
}

size_t VertexColorRegionEditor::update_selection(size_t seed_face, RegionSelectionOperation operation,
                                                 const RegionSelectionSettings& settings)
{
    if (!ready() || seed_face >= m_mesh.indices.size())
        return m_selected_face_count;
    const std::vector<size_t> region = (operation == RegionSelectionOperation::Replace || operation == RegionSelectionOperation::AddSimilar)
        ? smart_region(seed_face, settings) : local_patch(seed_face, settings);
    if (operation == RegionSelectionOperation::Replace) {
        std::fill(m_selected_faces.begin(), m_selected_faces.end(), uint8_t(0));
        m_selected_face_count = 0;
    }
    for (size_t face : region) {
        const bool selected = operation != RegionSelectionOperation::Remove;
        if (bool(m_selected_faces[face]) == selected)
            continue;
        m_selected_faces[face] = selected ? 1 : 0;
        if (selected)
            ++m_selected_face_count;
        else
            --m_selected_face_count;
    }
    return m_selected_face_count;
}

size_t VertexColorRegionEditor::select_faces(const std::vector<size_t>& face_indices)
{
    if (!ready())
        return 0;

    std::vector<uint8_t> selected(m_mesh.indices.size(), 0);
    size_t selected_count = 0;
    for (size_t face_index : face_indices) {
        if (face_index >= selected.size() || selected[face_index])
            continue;
        selected[face_index] = 1;
        ++selected_count;
    }
    // Invalid or empty evidence is non-destructive, matching the other automatic
    // localization helpers and preserving an in-progress manual selection.
    if (selected_count == 0)
        return 0;
    m_selected_faces = std::move(selected);
    m_selected_face_count = selected_count;
    return selected_count;
}

size_t VertexColorRegionEditor::select_palette_material(const std::vector<RGBA>& palette,
                                                        size_t palette_index)
{
    if (!ready() || palette.empty() || palette_index >= palette.size())
        return m_selected_face_count;

    std::fill(m_selected_faces.begin(), m_selected_faces.end(), uint8_t(0));
    m_selected_face_count = 0;
    for (size_t face_index = 0; face_index < m_mesh.indices.size(); ++face_index) {
        const RGBA color = face_color(face_index);
        size_t nearest_index = 0;
        float nearest_distance = color_distance_squared(color, palette.front());
        for (size_t candidate = 1; candidate < palette.size(); ++candidate) {
            const float distance = color_distance_squared(color, palette[candidate]);
            if (distance < nearest_distance) {
                nearest_distance = distance;
                nearest_index = candidate;
            }
        }
        if (nearest_index != palette_index)
            continue;
        m_selected_faces[face_index] = 1;
        ++m_selected_face_count;
    }
    return m_selected_face_count;
}

size_t VertexColorRegionEditor::select_elevated_overhang_regions(
    const OverhangRegionSettings& settings)
{
    if (!ready())
        return 0;

    const float ground_band = std::max(0.0f, settings.ground_band_mm);
    const float surface_angle = std::clamp(settings.maximum_surface_angle_degrees, 0.0f, 89.9f);
    const float maximum_normal_z = -std::cos(surface_angle * PI / 180.0f);
    const double minimum_region_area = std::max(0.0f, settings.minimum_region_area_mm2);
    const double minimum_region_ratio = std::max(0.0f, settings.minimum_region_area_ratio);
    const float ground_limit = std::min_element(
        m_mesh.vertices.begin(), m_mesh.vertices.end(),
        [](const Vec3f& left, const Vec3f& right) { return left.z() < right.z(); })->z() + ground_band;

    std::vector<double> face_areas(m_mesh.indices.size(), 0.0);
    std::vector<uint8_t> candidates(m_mesh.indices.size(), 0);
    double surface_area = 0.0;
    for (size_t face_index = 0; face_index < m_mesh.indices.size(); ++face_index) {
        const stl_triangle_vertex_indices& face = m_mesh.indices[face_index];
        const Vec3f& a = m_mesh.vertices[face[0]];
        const Vec3f& b = m_mesh.vertices[face[1]];
        const Vec3f& c = m_mesh.vertices[face[2]];
        const double area = 0.5 * double((b - a).cross(c - a).norm());
        face_areas[face_index] = area;
        surface_area += area;
        if (area > 1e-12 && m_face_normals[face_index].z() < maximum_normal_z &&
            std::min({a.z(), b.z(), c.z()}) > ground_limit)
            candidates[face_index] = 1;
    }

    std::vector<uint8_t> visited(m_mesh.indices.size(), 0);
    std::vector<uint8_t> localized(m_mesh.indices.size(), 0);
    size_t localized_count = 0;
    for (size_t seed = 0; seed < candidates.size(); ++seed) {
        if (!candidates[seed] || visited[seed])
            continue;
        visited[seed] = 1;
        std::vector<size_t> pending {seed};
        std::vector<size_t> region;
        double region_area = 0.0;
        while (!pending.empty()) {
            const size_t face_index = pending.back();
            pending.pop_back();
            region.push_back(face_index);
            region_area += face_areas[face_index];
            for (uint32_t neighbor : m_face_neighbors[face_index]) {
                if (!candidates[neighbor] || visited[neighbor])
                    continue;
                visited[neighbor] = 1;
                pending.push_back(neighbor);
            }
        }
        const double region_ratio = surface_area > 0.0 ? region_area / surface_area : 0.0;
        if (region_area < minimum_region_area || region_ratio < minimum_region_ratio)
            continue;
        for (size_t face_index : region) {
            localized[face_index] = 1;
            ++localized_count;
        }
    }

    // A failed localization is non-destructive: an existing manual/material selection
    // remains available for the user to inspect or edit.
    if (localized_count == 0)
        return 0;
    m_selected_faces = std::move(localized);
    m_selected_face_count = localized_count;
    return localized_count;
}

void VertexColorRegionEditor::clear_selection()
{
    std::fill(m_selected_faces.begin(), m_selected_faces.end(), uint8_t(0));
    m_selected_face_count = 0;
}

bool VertexColorRegionEditor::restore_selection(const std::vector<uint8_t>& selected_faces)
{
    if (!ready() || selected_faces.size() != m_selected_faces.size())
        return false;
    m_selected_face_count = 0;
    for (size_t face_index = 0; face_index < selected_faces.size(); ++face_index) {
        m_selected_faces[face_index] = selected_faces[face_index] == 0 ? 0 : 1;
        m_selected_face_count += m_selected_faces[face_index];
    }
    return true;
}

bool VertexColorRegionEditor::apply_color(const RGBA& color)
{
    if (!ready() || m_selected_face_count == 0)
        return false;
    for (size_t face_index = 0; face_index < m_mesh.indices.size(); ++face_index) {
        if (!m_selected_faces[face_index])
            continue;
        const auto& face = m_mesh.indices[face_index];
        if (m_vertex_colors[face[0]] == color && m_vertex_colors[face[1]] == color &&
            m_vertex_colors[face[2]] == color)
            m_face_color_overrides.erase(face_index);
        else
            m_face_color_overrides[face_index] = color;
    }
    return true;
}

bool VertexColorRegionEditor::apply_color_to_obj_copy(const RGBA& color,
                                                      const boost::filesystem::path& source,
                                                      const boost::filesystem::path& destination,
                                                      std::string& error)
{
    const auto original_overrides = m_face_color_overrides;
    if (!apply_color(color)) {
        error = "No model region is selected.";
        return false;
    }
    const bool written = write_obj_copy(source, destination, error);
    // Export is a candidate copy. Keep the source editor intact so cached
    // before/after views and undo followed by another edit use original colors.
    m_face_color_overrides = original_overrides;
    return written;
}

void VertexColorRegionEditor::build_color_mesh(indexed_triangle_set& mesh,
                                               std::vector<RGBA>& colors) const
{
    mesh = m_mesh;
    colors = m_vertex_colors;
    std::vector<uint8_t> assigned(m_mesh.vertices.size(), 0);
    std::unordered_map<int, std::vector<int>> variants;
    for (size_t face_index = 0; face_index < m_mesh.indices.size(); ++face_index) {
        for (size_t corner = 0; corner < 3; ++corner) {
            const int source_vertex = m_mesh.indices[face_index][corner];
            const RGBA color = corner_color(face_index, corner);
            if (!assigned[source_vertex]) {
                colors[source_vertex] = color;
                assigned[source_vertex] = 1;
            }
            if (colors[source_vertex] == color)
                continue;
            auto& alternatives = variants[source_vertex];
            auto existing = std::find_if(alternatives.begin(), alternatives.end(),
                [&colors, &color](int index) { return colors[index] == color; });
            int output_vertex;
            if (existing != alternatives.end()) {
                output_vertex = *existing;
            } else {
                output_vertex = int(mesh.vertices.size());
                mesh.vertices.push_back(m_mesh.vertices[source_vertex]);
                colors.push_back(color);
                alternatives.push_back(output_vertex);
            }
            mesh.indices[face_index][corner] = output_vertex;
        }
    }
}

bool VertexColorRegionEditor::write_obj_copy(const boost::filesystem::path& source,
                                             const boost::filesystem::path& destination,
                                             std::string& error) const
{
    if (!ready()) {
        error = "No vertex-color model is loaded.";
        return false;
    }
    indexed_triangle_set color_mesh;
    std::vector<RGBA> colors;
    build_color_mesh(color_mesh, colors);
    if (model_artifact_format(source) == "glb" || model_artifact_format(destination) == "glb") {
        TriangleMesh source_mesh; ObjInfo source_colors;
        if (!load_model_artifact(source, source_mesh, source_colors, error)) return false;
        if (source_mesh.its.vertices.size() != m_mesh.vertices.size() || source_mesh.its.indices != m_mesh.indices) {
            error = "The source model changed while recoloring. Please reload it.";
            return false;
        }
        for (size_t i = 0; i < m_mesh.vertices.size(); ++i)
            if ((source_mesh.its.vertices[i] - m_mesh.vertices[i]).squaredNorm() > 1e-10f) {
                error = "The source geometry changed while recoloring. Please reload it.";
                return false;
            }
        return write_model_artifact(destination, color_mesh, colors, error);
    }
    boost::filesystem::ifstream input(source);
    if (!input) {
        error = "Unable to read the source OBJ.";
        return false;
    }
    boost::system::error_code filesystem_error;
    boost::filesystem::create_directories(destination.parent_path(), filesystem_error);
    if (filesystem_error) {
        error = "Unable to create the edited model directory.";
        return false;
    }
    boost::filesystem::path temporary = destination;
    temporary += ".tmp";
    boost::filesystem::ofstream output(temporary, std::ios::trunc);
    if (!output) {
        error = "Unable to create the edited OBJ.";
        return false;
    }

    // Emit the derived position/color array once. Texture and normal records keep
    // their original ordering; only face position indices are remapped below.
    output << std::setprecision(std::numeric_limits<float>::max_digits10);
    for (size_t vertex = 0; vertex < color_mesh.vertices.size(); ++vertex) {
        const auto& position = color_mesh.vertices[vertex];
        const auto& color = colors[vertex];
        output << "v " << position.x() << ' ' << position.y() << ' ' << position.z() << ' '
               << color[0] << ' ' << color[1] << ' ' << color[2] << ' ' << color[3] << '\n';
    }
    auto invalid_source = [&]() {
        output.close();
        boost::filesystem::remove(temporary, filesystem_error);
        error = "The source OBJ geometry or face layout changed while recoloring. Please reload it.";
        return false;
    };
    std::string line;
    size_t vertex_index = 0;
    size_t face_index = 0;
    while (std::getline(input, line)) {
        std::istringstream parser(line);
        std::string tag;
        parser >> tag;
        if (tag != "v" && tag != "f") {
            output << line << '\n';
            continue;
        }
        if (tag == "v") {
            Vec3f position;
            if (!(parser >> position.x() >> position.y() >> position.z()) ||
                vertex_index >= m_mesh.vertices.size())
                return invalid_source();
            if (!position.allFinite() ||
                (position - m_mesh.vertices[vertex_index]).squaredNorm() > 1e-10f)
                return invalid_source();
            ++vertex_index;
            continue;
        }

        std::vector<int> source_indices;
        std::vector<std::string> suffixes;
        std::string token;
        while (parser >> token) {
            if (token.front() == '#')
                break;
            const size_t slash = token.find('/');
            const std::string index_text = token.substr(0, slash);
            std::istringstream index_parser(index_text);
            int64_t index;
            char trailing;
            if (!(index_parser >> index) || (index_parser >> trailing) || index == 0)
                return invalid_source();
            index = index < 0 ? int64_t(vertex_index) + index : index - 1;
            if (index < 0 || index >= int64_t(m_mesh.vertices.size()))
                return invalid_source();
            source_indices.push_back(int(index));
            suffixes.push_back(slash == std::string::npos ? "" : token.substr(slash));
        }
        // Match the OBJ reader's triangle/quad fan, preserving face identity even
        // when the reader corrected the source winding on a closed mesh.
        if (source_indices.size() < 3 || source_indices.size() > 4)
            return invalid_source();
        for (size_t triangle = 1; triangle + 1 < source_indices.size(); ++triangle) {
            if (face_index >= m_mesh.indices.size())
                return invalid_source();
            const std::array<size_t, 3> fan {{0, triangle, triangle + 1}};
            const auto& source_face = m_mesh.indices[face_index];
            std::array<size_t, 3> token_indices;
            for (size_t corner = 0; corner < 3; ++corner) {
                const auto found = std::find_if(fan.begin(), fan.end(),
                    [&](size_t token_index) { return source_indices[token_index] == source_face[corner]; });
                if (found == fan.end())
                    return invalid_source();
                token_indices[corner] = *found;
            }
            output << "f";
            for (size_t corner = 0; corner < 3; ++corner)
                output << ' ' << color_mesh.indices[face_index][corner] + 1 << suffixes[token_indices[corner]];
            output << '\n';
            ++face_index;
        }
    }
    output.close();
    if (!output || input.bad() || vertex_index != m_vertex_colors.size() || face_index != m_mesh.indices.size()) {
        boost::filesystem::remove(temporary, filesystem_error);
        error = "The edited OBJ could not be written completely.";
        return false;
    }
    boost::filesystem::remove(destination, filesystem_error);
    filesystem_error.clear();
    boost::filesystem::rename(temporary, destination, filesystem_error);
    if (filesystem_error) {
        boost::filesystem::remove(temporary, filesystem_error);
        error = "Unable to finalize the edited OBJ.";
        return false;
    }
    return true;
}

} // namespace Slic3r::AI
