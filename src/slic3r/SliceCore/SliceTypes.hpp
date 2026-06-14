#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {
namespace SliceCore {

struct PresetSelection {
    std::optional<std::string> printer_name;
    std::optional<std::string> process_name;
    std::vector<std::string>   filament_names;
    std::vector<std::string>   load_settings;
    std::vector<std::string>   load_filaments;
    DynamicPrintConfig         overrides;
};

struct Transforms {
    int    arrange    = 0;
    int    orient     = 0;
    int    repetitions = 1;
    double rotate     = 0;
    double rotate_x   = 0;
    double rotate_y   = 0;
    double scale      = 1.0;
    bool   assemble       = false;
    bool   ensure_on_bed  = false;
    bool   convert_unit   = false;
    std::vector<int> skip_objects;
};

// Per-instance placement override within a single ObjectPlacement.
// When an ObjectPlacement carries a non-empty `instances` list the object's
// ModelInstance list is completely rebuilt from these entries; the global
// arrange / repetitions step must skip any object with explicit instances
// (enforced in SliceService::run).
struct InstancePlacement {
    std::optional<std::array<double, 3>> position;   // x,y,z mm absolute
    std::optional<double>                rotation_z; // degrees
    std::optional<double>                scale;      // uniform
};

// Per-object placement descriptor for a single ModelObject in the loaded model.
// Objects are matched by `index` (0-based into model.objects) or `name`; at
// least one of the two should be set.  All transform fields are optional; unset
// fields leave the object's existing transform unchanged for that component.
struct ObjectPlacement {
    std::optional<int>                   index;          // 0-based model.objects index
    std::optional<std::string>           name;           // ModelObject::name match
    std::optional<std::array<double, 3>> position;       // absolute offset mm (x,y,z)
    std::optional<std::array<double, 3>> rotation;       // degrees (rx,ry,rz)
    std::optional<std::array<double, 3>> scale;          // per-axis scale factors
    std::optional<double>                uniform_scale;  // overridden by `scale` if both set
    std::optional<std::array<bool, 3>>   mirror;         // mirror on (x,y,z) axes
    std::optional<int>                   orient;         // 1 = run orientation::orient()
    std::optional<bool>                  ensure_on_bed;  // lift lowest point to z=0
    std::optional<bool>                  printable;      // false = skip object entirely
    std::vector<InstancePlacement>       instances;      // explicit per-instance list
};

enum class OutputMode {
    File,
    Stdout,
    PrintHost
};

struct OutputTarget {
    OutputMode         mode       = OutputMode::File;
    std::string        outputdir;
    DynamicPrintConfig host_config;
    bool               start_print = false;
};

enum class ExportKind {
    Gcode,
    ThreeMF,
    Stl
};

struct PlateStat {
    int         plate_id         = 0;
    long long   sliced_ms        = 0;
    double      filament_used_mm = 0;
    int         layer_count      = 0;
    std::string gcode_path;

    // --- Tier-1 structured preview data ---
    // Total estimated print time in seconds (Normal mode, GCodeProcessorResult
    // print_statistics.modes[0].time).
    double estimated_print_time_s  = 0;
    // Time (seconds) spent on the initial layer (GCodeProcessorResult::initial_layer_time).
    double initial_layer_time_s    = 0;
    // Number of custom gcode events per z (used as color-change proxy:
    // GCodeProcessorResult::custom_gcode_per_print_z.size()).
    int    color_change_count      = 0;
    // Filament volume (mm³) consumed per extruder (0-based extruder index ->
    // volume).  Sourced from PrintEstimatedStatistics::model_volumes_per_extruder
    // (key=size_t extruder id, value=double mm³); keys cast to int here.
    std::map<int, double> filament_volume_per_extruder;
    // Thumbnail fields — populated by a separate task; initialised to false/empty.
    bool        thumbnail_generated = false;
    std::string thumbnail_path;
};

struct SliceRequest {
    std::string              input_path;
    std::vector<uint8_t>     input_bytes;
    std::string              input_filename;
    std::string              datadir;
    int                      plate = 0;
    PresetSelection          presets;
    Transforms               transforms;
    // Per-object placement overrides applied after apply_model_transforms().
    // Objects with explicit `instances` skip the global arrange/repetitions step.
    std::vector<ObjectPlacement> objects;
    OutputTarget             output;
    ExportKind               export_kind = ExportKind::Gcode;
    std::function<void(int, const std::string &)> progress;

    // Thumbnail generation flags (OpenGL/rendering work is a separate task;
    // these fields are parsed and stored but the render path is not wired here).
    bool generate_thumbnail  = false;
    int  thumbnail_width     = 512;
    int  thumbnail_height    = 512;
};

struct SliceResult {
    bool                     ok        = false;
    int                      exit_code = 0;
    std::string              error;
    std::vector<PlateStat>   plates;
    std::vector<uint8_t>     gcode_bytes;
    // Non-fatal diagnostic messages accumulated during placement and slicing.
    // Includes out-of-bounds object warnings, unmatched placement descriptors,
    // assemble-mode notices, etc.
    std::vector<std::string> warnings;
};

} // namespace SliceCore
} // namespace Slic3r
