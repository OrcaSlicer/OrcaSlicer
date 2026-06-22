#pragma once

#include "Export.hpp"
#include "Presets.hpp"
#include "Project.hpp"
#include "Slicer.hpp"

#include <cstddef>
#include <filesystem>
#include <string>

namespace orca {

struct PresetChanged {
    PresetType  type;
    std::string name;
};

struct SlicingProgress {
    SliceHandle handle;
    float       progress;
    std::string message;
};

struct SlicingFinished {
    SliceHandle handle;
    bool        success;
    std::string error;
};

struct ExportBegan {
    ExportHandle          handle;
    std::filesystem::path path;
};

struct ExportFinished {
    ExportHandle handle;
    bool         success;
    std::size_t  line_count;
    std::string  error;
};

struct ProjectLoaded {
    LoadHandle            handle;
    std::filesystem::path path;
};

struct ObjectAdded {
    ObjectId id;
};

struct ObjectRemoved {
    ObjectId id;
};

struct BeforeSlice {
    SliceHandle handle;
};

struct AfterPerimeters {
    SliceHandle handle;
    std::size_t object_count;
};

struct AfterInfill {
    SliceHandle handle;
    std::size_t object_count;
};

struct AfterIroning {
    SliceHandle handle;
};

struct AfterSupports {
    SliceHandle handle;
};

struct BeforeWipeTower {
    SliceHandle handle;
    bool        has_wipe_tower;
};

struct AfterSkirtBrim {
    SliceHandle handle;
};

struct BeforeGCodeExport {
    SliceHandle           handle;
    std::filesystem::path output_path;
};

struct AfterGCodeExport {
    SliceHandle           handle;
    std::filesystem::path output_path;
    std::size_t           line_count;
};

} // namespace orca
