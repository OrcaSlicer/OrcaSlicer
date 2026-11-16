#pragma once

#include <string>

namespace Slic3r {

struct GCodeProcessorResult;

// The statistics of a plate's G-code as JSON, in seconds, millimeters and grams: totals, per layer and per object instance.
std::string gcode_statistics_json(const GCodeProcessorResult &result);
// Where the statistics of the G-code at gcode_path are written.
std::string gcode_statistics_path(const std::string &gcode_path);
// Throws ExportError when the file cannot be written.
void export_gcode_statistics(const GCodeProcessorResult &result, const std::string &path);

} // namespace Slic3r
