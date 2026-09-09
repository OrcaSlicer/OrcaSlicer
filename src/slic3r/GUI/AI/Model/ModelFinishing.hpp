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
    // For local smoothing these counts describe the examined selection patch,
    // including its open border, rather than a whole-model quality report.
    size_t boundary_edges {0};
    size_t nonmanifold_edges {0};
    double max_displacement {0.0};
    double displacement_limit {0.0};
    bool changed() const {
        return moved_vertices || removed_degenerate_faces || removed_duplicate_faces || reversed_faces;
    }
};

// Edits a new triangle OBJ beside its source so relative MTL/texture references
// remain valid. Vertex order, color values, UVs, materials and components stay
// intact. No provider, printer, preset, or Orca project mutation takes place.
ModelFinishingResult finish_model_obj(
    const boost::filesystem::path& source,
    const boost::filesystem::path& destination,
    const ModelFinishingOptions& options,
    const std::function<bool()>& canceled = {});

} // namespace Slic3r::AI
