#pragma once

#include <cstdint>
#include <functional>
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
    int         plate_id        = 0;
    long long   sliced_ms       = 0;
    double      filament_used_mm = 0;
    int         layer_count     = 0;
    std::string gcode_path;
};

struct SliceRequest {
    std::string              input_path;
    std::vector<uint8_t>     input_bytes;
    std::string              input_filename;
    std::string              datadir;
    int                      plate = 0;
    PresetSelection          presets;
    Transforms               transforms;
    OutputTarget             output;
    ExportKind               export_kind = ExportKind::Gcode;
    std::function<void(int, const std::string &)> progress;
};

struct SliceResult {
    bool                     ok        = false;
    int                      exit_code = 0;
    std::string              error;
    std::vector<PlateStat>   plates;
    std::vector<uint8_t>     gcode_bytes;
};

} // namespace SliceCore
} // namespace Slic3r
