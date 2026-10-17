#pragma once

#include <functional>
#include <map>
#include <string>

namespace Slic3r {

// Renumbers the tools a G-code file uses, for a tool changer that is told which physical tool
// prints each filament only when the print is sent, and whose firmware cannot remap tool numbers
// itself. The slicer names tools by filament index, so a project with more filaments than the
// printer has tools emits tool numbers the printer does not have.
//
// `tool_map` maps a tool number in the G-code to the physical tool that prints it; tool numbers
// missing from the map are left alone. Rewritten are tool changes (`T6`) and the tool argument
// of M104 / M109 (`M104 S220 T6`). Comment lines are passed to `rewrite_comment`, when given, and
// otherwise left as they are, except for a leading `; MD5:<hex>` line: Flashforge-family firmware
// checks that checksum against the rest of the file, so it is recomputed over the result.
std::string remap_gcode_tools(const std::string&                       gcode,
                              const std::map<int, int>&                tool_map,
                              const std::function<void(std::string&)>& rewrite_comment = {});

} // namespace Slic3r
