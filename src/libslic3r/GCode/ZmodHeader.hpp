#pragma once

#include <map>
#include <string>

namespace Slic3r {

class ConfigBase;

// The printer agent id of Flashforge printers running Zmod.
inline constexpr char ZMOD_PRINTER_AGENT_ID[] = "zmod";

// Whether files for this printer get the Zmod header: its printer agent is Zmod's. Stock
// Flashforge firmware and every other printer get the G-code unchanged.
bool printer_wants_zmod_header(const ConfigBase& printer_config);

// The file header Zmod (https://github.com/ghzserg/zmod) reads on Flashforge printers, written the
// way its preprocessor (https://github.com/ghzserg/zmod_preprocess, Apache-2.0) writes it:
//   ; MD5:<hex>          first line, the MD5 of every byte after it; Zmod checks it and deletes a
//                        file that does not match. Stock Flashforge firmware reads it as a comment.
//   ; zmod_color_data = <tools>|<colour per tool>|<type per tool>
//                        placed before "; HEADER_BLOCK_END": the tools the file changes to, in order
//                        of first use, with the filament colours and types listed by tool number, so
//                        Zmod's colour menu need not scan the file.
// Running it on a file that already has the header replaces it.
std::string add_zmod_header(const std::string& gcode);

// add_zmod_header() over a file in place. Returns false when it cannot be read or written.
bool add_zmod_header_to_file(const std::string& path);

// Renumbers the tools of a "; zmod_color_data =" line after the file's tools were renumbered
// through `tool_map` (G-code tool -> physical tool). Other lines are left alone. With several
// filaments on one tool, the first one listed names its colour and type.
void remap_zmod_color_data(std::string& line, const std::map<int, int>& tool_map);

// Lower-case hex MD5 of `data`.
std::string md5_hex(const std::string& data);

} // namespace Slic3r
