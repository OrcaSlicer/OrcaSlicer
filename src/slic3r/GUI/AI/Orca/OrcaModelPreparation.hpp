#pragma once

#include <memory>
#include "libslic3r/Model.hpp"

namespace Slic3r::GUI {

struct ModelPreparationOptions
{
    double total_height_mm {120.0};
    bool add_round_base {false};
    double base_height_mm {3.0};
};

struct ModelPreparation
{
    Geometry::Transformation instance;
    std::unique_ptr<Model> base;
};

// Compute a detached proposal without touching the source or its autosave state.
ModelPreparation prepare_model(const ModelObject& source, const ModelPreparationOptions& options);
// Call inside a native snapshot. Existing volume IDs, meshes and painting stay intact.
void apply_model_preparation(ModelObject& source, const ModelPreparation& proposal);

} // namespace Slic3r::GUI
