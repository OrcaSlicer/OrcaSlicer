#ifndef slic3r_Format_MakerBotExport_hpp_
#define slic3r_Format_MakerBotExport_hpp_

// MakerBot / UltiMaker Fork – Orca Slicer 2.4
// =============================================================================
// Post-processes a finished G-code file into the native .makerbot archive
// format required by MakerBot (Birdwing/Lava) printers. Called from
// GCode::do_export() after the .tmp → .gcode rename succeeds.
//
// This module is MakerBot-only and has no dependency on, or knowledge of,
// UltiMaker/.ufp export - that lives entirely independently in
// Format/UltimakerUFPExport.hpp/.cpp. Removing MakerBot device support from
// a build does not affect UltiMaker export, and vice versa. The two modules
// only share the vendor-neutral helpers in Format/GCodeArchiveUtils.hpp.
//
// Output formats by flavor:
//   gcfMakerBotBirdwing  → .makerbot  (ZIP: print.jsontoolpath + meta.json)
//   gcfMakerBotLava      → .makerbot  (ZIP: print.gcode + meta.json)
//   gcfMakerBotLegacy    → .gcode     (plain; user can convert to .x3g via GPX)
//
// The original .gcode file is removed after successful archive creation (unless
// the archive path equals the input path, i.e. the user saved as .makerbot).
// =============================================================================

#include <string>
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {
namespace MakerBotExport {

// Returns the canonical output extension (with leading dot) for the given flavor.
// Thumbnail data for .makerbot archive
// Firmware requires: thumbnail_55x40.png, thumbnail_110x80.png, thumbnail_320x200.png
struct BirdwingThumbnails {
    std::string png_55x40;    // raw PNG bytes
    std::string png_110x80;   // raw PNG bytes
    std::string png_320x200;  // raw PNG bytes
    bool has_thumbnails() const {
        return !png_55x40.empty() || !png_110x80.empty() || !png_320x200.empty();
    }
};

std::string get_archive_extension(GCodeFlavor flavor);

// Main entry point – called from GCode::do_export() after the G-code is written.
//
// gcode_path  : path of the freshly written .gcode file
// config      : full print config (used for nozzle temps, flavor, etc.)
//
// Returns the path of the created archive on success,
// or an empty string if packing was not needed or failed.
std::string pack_to_archive(const std::string &gcode_path,
                             const PrintConfig &config);

} // namespace MakerBotExport
} // namespace Slic3r

#endif // slic3r_Format_MakerBotExport_hpp_
