#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct indexed_triangle_set;

namespace Slic3r {

class TriangleMesh;
class ModelVolume;

struct TextureImage {
    int width  = 0;
    int height = 0;
    int channels = 4;
    std::vector<unsigned char> data;
};

struct TexturedMesh {
    std::vector<std::array<float,3>>  vertices;
    std::vector<std::array<int,3>>    indices;
    std::vector<std::array<float,2>>  uvs;
    std::vector<TextureImage>         textures;
    std::vector<int>                  material_ids;
    // material index -> index in textures[] (-1 if no texture, use material_colors)
    std::vector<int>                  material_texture_map;
    // per-material baseColorFactor (RGBA 0-1), indexed by material index
    std::vector<std::array<float,4>>  material_colors;

    // Per-face independent UV support (for OBJ where the same vertex can have
    // different texture coordinates on different faces).
    std::vector<std::array<float,2>>  uv_coords;   // UV coordinate pool
    std::vector<std::array<int,3>>    uv_indices;   // per-face UV indices into uv_coords

    bool has_face_uvs() const { return !uv_indices.empty() && !uv_coords.empty(); }
};

struct PaintedMesh {
    std::vector<std::array<float,3>>            vertices;
    std::vector<std::array<int,3>>              indices;
    std::vector<std::array<std::size_t,3>>      face_colors;   // per-face RGB [0..255]
    std::vector<std::array<std::size_t,3>>      cluster_colors;
};

using PaintProgressCallback = std::function<void(int percent, const char* message)>;
using PaintCancelCallback   = std::function<bool()>;
using PaintMeshRepairCallback = std::function<bool(const indexed_triangle_set& mesh,
                                                   indexed_triangle_set&       repaired_mesh,
                                                   std::function<void(const char* message, unsigned progress)> progress_callback,
                                                   std::function<bool()> cancel_callback,
                                                   std::string* error_message)>;

struct TexturePaintingSettings {
    std::size_t target_colors_num  = 4;
    double      smooth_weight      = 0.5;
    std::size_t oversampling_iters = 0;
    enum class MeshRepairDecision {
        Ask,
        ImportWithoutRepair,
        RepairAndImport
    };
    MeshRepairDecision mesh_repair_decision = MeshRepairDecision::ImportWithoutRepair;
    bool* mesh_repair_decision_required = nullptr;
    PaintMeshRepairCallback mesh_repair_callback;
};

struct FilamentMatch {
    int    cluster_index   = -1;
    int    filament_index  = -1;
    double delta_e         = 0.0;
    std::array<std::size_t,3> cluster_color   = {0,0,0};
    std::array<float,4>       filament_color  = {0,0,0,1};
};

bool texture_to_painting(
    const TexturedMesh& textured,
    PaintedMesh& painted,
    const TexturePaintingSettings& settings = {},
    PaintProgressCallback progress = nullptr,
    PaintCancelCallback cancel = nullptr);

std::vector<FilamentMatch> match_clusters_to_filaments(
    const std::vector<std::array<std::size_t,3>>& cluster_colors,
    const std::vector<std::array<float,4>>& filament_colors,
    const std::vector<std::string>& filament_names);

double compute_delta_e(
    const std::array<std::size_t,3>& rgb1,
    const std::array<float,4>& rgba2);

bool apply_painted_mesh_to_volume(
    const PaintedMesh& painted,
    const std::vector<FilamentMatch>& matches,
    ModelVolume& volume);

// Decode a TextureImage (which may contain raw PNG/JPEG bytes) into BGR pixel data.
// On success, populates out_pixels (BGR, 3 bytes/pixel) and sets out_w/out_h.
bool decode_texture_to_pixels(
    const TextureImage& img,
    std::vector<unsigned char>& out_pixels,
    int& out_w, int& out_h);

// Sample per-face colors from the correct texture per material_ids.
// Uses material_texture_map / material_colors for multi-material GLBs.
// Falls back to textures[0] when the mapping is absent.
bool sample_original_face_colors(
    const TexturedMesh& textured,
    std::vector<std::array<std::size_t,3>>& out_face_colors);

} // namespace Slic3r
