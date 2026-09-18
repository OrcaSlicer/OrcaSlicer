#pragma once

#include "slic3r/AI/ModelGeneration/SemanticColoring/SemanticColoring.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include <filesystem>
#include <memory>

namespace Slic3r::GUI {

GLModel::Geometry build_semantic_colored_geometry(
    const AI::SemanticColoring::MeshSnapshot&, const AI::SemanticColoring::FaceColors&,
    const AI::SemanticColoring::SubfaceColors&, const AI::SemanticColoring::Cancel& = {});

// No UI callbacks from the worker. The owning preview polls and adopts only
// the current request, so cancellation/model switches cannot paint a new model.
class ModelSemanticColoring {
public:
    using Snapshot = AI::SemanticColoring::MeshSnapshot;
    using Color = AI::SemanticColoring::Color;
    using FaceColors = AI::SemanticColoring::FaceColors;
    struct Result {
        GLModel::Geometry geometry;
        FaceColors automatic;
        AI::SemanticColoring::SubfaceColors automatic_subfaces;
        std::shared_ptr<const AI::SemanticColoring::Analysis> analysis;
        std::string error;
        bool person_detected {false};
        bool cache_hit {false};
        size_t subface_added_triangles {0};
        size_t subface_rejected_candidates {0};
        double elapsed_ms {0};
    };
    ModelSemanticColoring(std::filesystem::path runtime, std::filesystem::path cache);
    ~ModelSemanticColoring();
    ModelSemanticColoring(const ModelSemanticColoring&) = delete;
    ModelSemanticColoring& operator=(const ModelSemanticColoring&) = delete;
    bool request(std::shared_ptr<const Snapshot>, std::vector<Color> mapping_palette, std::vector<Color> target_palette,
                 std::vector<Color> portrait_card, FaceColors manual);
    void cancel();
    std::unique_ptr<Result> poll();
    bool busy() const;
    int progress() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace Slic3r::GUI
