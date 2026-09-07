#pragma once

#include "libslic3r/Model.hpp"
#include <set>

namespace Slic3r::GUI {

// Feedback from native texture matching, including virtual mixed filament IDs.
struct ModelColorImportResult {
    bool cancelled { false };
    bool colors_applied { false };
    size_t source_color_count { 0 };
    size_t mapped_color_count { 0 };
};

inline void collect_model_color_import_result(ModelColorImportResult* result, const Model& model, size_t source_colors)
{
    if (result == nullptr)
        return;
    std::set<size_t> used_filaments;
    for (const ModelObject* object : model.objects)
        for (const ModelVolume* volume : object->volumes) {
            const auto ids = volume->get_extruders_from_multi_material_painting();
            used_filaments.insert(ids.begin(), ids.end());
        }
    result->source_color_count = source_colors;
    result->mapped_color_count = used_filaments.size();
    result->colors_applied = !used_filaments.empty();
}

} // namespace Slic3r::GUI
