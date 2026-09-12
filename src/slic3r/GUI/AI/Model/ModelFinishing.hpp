#pragma once

#include <boost/filesystem/path.hpp>
#include <array>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace Slic3r::AI {

struct ModelFinishingOptions {
    bool smooth_surface {true};
    bool repair_mesh {true};
    double strength {0.35};
    // Zero-based face ordinals in the source triangle OBJ (not preview draw
    // order). Empty means the whole surface. A local selection requires
    // repair_mesh=false; vertices incident to unselected faces stay fixed.
    std::vector<size_t> selected_faces;
    // Color-only cleanup requires an explicit selection and normalized source
    // palette centers. Centers classify colors; they never replace source RGB.
    bool clean_color_spots {false};
    std::vector<std::array<float, 3>> cleanup_palette;
    // Explicit face-only recoloring is exclusive with smoothing, repair and spot
    // cleanup. It assigns exact corner colors without moving source geometry.
    bool recolor_selected {false};
    std::array<float, 4> target_color {};
};

struct ModelFinishingResult {
    bool success {false};
    bool canceled {false};
    std::string error;
    std::string source_sha256;
    std::string output_sha256;
    std::array<double, 3> dimensions {};
    size_t vertices {0};
    size_t faces_before {0};
    size_t faces_after {0};
    size_t moved_vertices {0};
    size_t protected_vertices {0};
    size_t removed_degenerate_faces {0};
    size_t removed_duplicate_faces {0};
    size_t reversed_faces {0};
    size_t cleaned_color_regions {0};
    size_t recolored_vertices {0};
    // For local smoothing these counts describe the examined selection patch,
    // including its open border, rather than a whole-model quality report.
    size_t boundary_edges {0};
    size_t nonmanifold_edges {0};
    double max_displacement {0.0};
    double displacement_limit {0.0};
    size_t recolored_faces {0};
    bool changed() const {
        return moved_vertices || removed_degenerate_faces || removed_duplicate_faces || reversed_faces || recolored_vertices || recolored_faces;
    }
};

// Edits a new triangle OBJ beside its source so relative MTL/texture references
// remain valid. Vertex order, UVs, materials and components stay intact. Colors
// change only for explicit color cleanup/recoloring. Exact face recoloring may
// split shared vertex indices at color boundaries while preserving positions,
// triangle order and UV/normal references.
// No provider, printer, preset, or Orca project mutation takes place.
ModelFinishingResult finish_model_obj(
    const boost::filesystem::path& source,
    const boost::filesystem::path& destination,
    const ModelFinishingOptions& options,
    const std::function<bool()>& canceled = {});

// Supports OBJ and GLB sources and preserves the selected output format.
// GLB textures are sampled into the same editable colors used by the preview;
// the source GLB and its texture maps are retained unchanged.
ModelFinishingResult finish_model_artifact(
    const boost::filesystem::path& source,
    const boost::filesystem::path& destination,
    const ModelFinishingOptions& options,
    const std::function<bool()>& canceled = {});

} // namespace Slic3r::AI
