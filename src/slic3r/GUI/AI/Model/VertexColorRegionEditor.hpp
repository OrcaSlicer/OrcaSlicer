#pragma once

#include "libslic3r/Color.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <boost/filesystem/path.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Slic3r::AI {

enum class RegionSelectionOperation {
    Replace,
    Add,
    Remove,
    AddSimilar
};

struct RegionSelectionSettings {
    float color_distance {0.12f};
    float normal_angle_degrees {70.0f};
    float local_radius_ratio {0.035f};
};

struct OverhangRegionSettings {
    float ground_band_mm {0.5f};
    float maximum_surface_angle_degrees {45.0f};
    float minimum_region_area_mm2 {4.0f};
    float minimum_region_area_ratio {0.0005f};
};

// Pure mesh/color editor used by the AI preview. It intentionally has no wxWidgets
// or Orca workspace dependencies so future semantic segmenters can feed the same
// face-selection contract without touching the slicer core.
class VertexColorRegionEditor
{
public:
    bool initialize(indexed_triangle_set mesh, std::vector<RGBA> vertex_colors, std::string& error);
    void clear();

    bool ready() const { return !m_mesh.indices.empty() && m_vertex_colors.size() == m_mesh.vertices.size(); }
    const indexed_triangle_set& mesh() const { return m_mesh; }
    const std::vector<RGBA>& vertex_colors() const { return m_vertex_colors; }
    // Source topology and vertex colors stay intact. Read corners when displaying
    // edits: a shared vertex can have a different color on either side of a boundary.
    RGBA corner_color(size_t face_index, size_t corner) const;
    bool has_color_overrides() const { return !m_face_color_overrides.empty(); }
    const std::vector<uint8_t>& selected_faces() const { return m_selected_faces; }
    size_t selected_face_count() const { return m_selected_face_count; }

    std::optional<size_t> pick_face(const Vec3d& ray_origin, const Vec3d& ray_direction) const;
    size_t update_selection(size_t seed_face, RegionSelectionOperation operation,
                            const RegionSelectionSettings& settings);
    size_t select_faces(const std::vector<size_t>& face_indices);
    size_t select_palette_material(const std::vector<RGBA>& palette, size_t palette_index);
    size_t select_elevated_overhang_regions(
        const OverhangRegionSettings& settings = OverhangRegionSettings {});
    void clear_selection();
    bool restore_selection(const std::vector<uint8_t>& selected_faces);
    bool apply_color(const RGBA& color);
    bool apply_color_to_obj_copy(const RGBA& color,
                                 const boost::filesystem::path& source,
                                 const boost::filesystem::path& destination,
                                 std::string& error);

    bool write_obj_copy(const boost::filesystem::path& source,
                        const boost::filesystem::path& destination,
                        std::string& error) const;

private:
    struct PickBvhNode
    {
        Vec3f minimum {Vec3f::Zero()};
        Vec3f maximum {Vec3f::Zero()};
        uint32_t first {0};
        uint32_t count {0};
        uint32_t left {0};
        uint32_t right {0};

        bool is_leaf() const { return count != 0; }
    };

    uint32_t build_pick_bvh(size_t begin, size_t end);
    std::vector<size_t> smart_region(size_t seed_face, const RegionSelectionSettings& settings) const;
    std::vector<size_t> local_patch(size_t seed_face, const RegionSelectionSettings& settings) const;
    RGBA face_color(size_t face_index) const;
    // Export preserves positions and face sequence, splitting vertices only where
    // incident faces need distinct colors. The editor's mesh is never split.
    void build_color_mesh(indexed_triangle_set& mesh, std::vector<RGBA>& colors) const;

    indexed_triangle_set m_mesh;
    std::vector<RGBA> m_vertex_colors;
    std::unordered_map<size_t, RGBA> m_face_color_overrides;
    std::vector<Vec3f> m_face_normals;
    std::vector<Vec3f> m_face_centers;
    std::vector<std::vector<uint32_t>> m_face_neighbors;
    std::vector<uint32_t> m_pick_face_order;
    std::vector<PickBvhNode> m_pick_nodes;
    std::vector<uint8_t> m_selected_faces;
    size_t m_selected_face_count {0};
    float m_mesh_diagonal {0.0f};
};

} // namespace Slic3r::AI
