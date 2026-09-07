#pragma once

#include "slic3r/AI/Contracts/GeneratedModelArtifact.hpp"

#include <cstddef>
#include <string>

namespace Slic3r::AI {

enum class ImportColorMode
{
    ManualMatch,
    AutoMap,
    SingleColor,
    NativeMatch
};

struct ModelImportRequest
{
    GeneratedModelArtifact artifact;
    ImportColorMode         color_mode { ImportColorMode::NativeMatch };
};

enum class ModelImportOutcome
{
    Imported,
    InvalidArtifact,
    ImportFailed,
    RepairFailed,
    Cancelled
};

struct ModelImportResult
{
    ModelImportOutcome outcome { ModelImportOutcome::ImportFailed };
    ImportColorMode    color_mode { ImportColorMode::NativeMatch };
    bool               colors_applied { false };
    bool               color_mapping_collapsed { false };
    bool               manual_coloring_required { false };
    bool               manual_repair_required { false };
    size_t             source_color_count { 0 };
    size_t             mapped_color_count { 0 };
    std::string        error;

    bool imported() const { return outcome == ModelImportOutcome::Imported; }
};

class IModelArtifactConsumer
{
public:
    virtual ~IModelArtifactConsumer() = default;

    virtual ModelImportResult import_artifact(const ModelImportRequest& request) = 0;
};

} // namespace Slic3r::AI
