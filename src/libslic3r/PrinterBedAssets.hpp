#pragma once

#include "miniz_extension.hpp"

#include <miniz.h>

#include <nlohmann/json_fwd.hpp>
#include <string>

namespace Slic3r {

class DynamicPrintConfig;

// Not on PresetBundle.hpp: miniz.h's zlib-compat macros rename GUI methods (GLTexture::Compressor::compress).
// printer_config_entry is the zip name also pushed onto bundle_structure["printer_config"].
// printer_index is that entry's 0-based index in the exported printer_config list.
bool append_printer_bed_assets(mz_zip_archive &zip, const DynamicPrintConfig &config, nlohmann::json &bundle_structure,
                               const std::string &printer_config_entry, int printer_index);

} // namespace Slic3r
