#pragma once

#include "miniz_extension.hpp"

#include <nlohmann/json_fwd.hpp>

namespace Slic3r {

class DynamicPrintConfig;

// Not on PresetBundle.hpp: miniz.h's zlib-compat macros rename GUI methods (GLTexture::Compressor::compress).
bool append_printer_bed_assets(mz_zip_archive &zip, const DynamicPrintConfig &config, nlohmann::json &bundle_structure);

} // namespace Slic3r
