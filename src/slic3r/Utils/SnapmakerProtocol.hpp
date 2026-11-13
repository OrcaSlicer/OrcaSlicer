#pragma once

#include <string>

namespace Slic3r {
class DynamicPrintConfig;

namespace SnapmakerProtocol {
// Empty when no loaded-spool selection needs to travel with the job.
std::string filament_mapping(const DynamicPrintConfig& config);
// Empty for invalid input. The U1 applies this map to tools, heaters, and its startup macros.
std::string start_script(const std::string& filename, const std::string& mapping);
}
} // namespace Slic3r
