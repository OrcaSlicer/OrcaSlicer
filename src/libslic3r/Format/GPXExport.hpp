#ifndef slic3r_Format_GPXExport_hpp_
#define slic3r_Format_GPXExport_hpp_

// MakerBot / UltiMaker Fork – Orca Slicer 2.4
// =============================================================================
// Post-processes a finished G-code file into the native .x3g archive required
// by the entire pre-Birdwing MakerBot/Sailfish lineage - Cupcake through
// Replicator 2X (gcfMakerBotLegacy). Called from GCode::do_export() after the
// .tmp → .gcode rename succeeds.
//
// This module is independent of MakerBotExport.* (Birdwing/Lava) and
// UltimakerUFPExport.* - removing either of those from a build does not
// affect .x3g export, and vice versa. The only shared dependency across all
// three vendor/format modules is Format/GCodeArchiveUtils.hpp.
//
// Unlike Birdwing/.makerbot or UltiMaker/.ufp, .x3g conversion is NOT done by
// Orca itself: it shells out to the external "gpx" tool (markwal/GPX on
// GitHub), which Orca does not bundle. gpx must be installed and reachable on
// PATH (or pointed to via the ORCA_GPX_BIN environment variable).
//
// Output: gcfMakerBotLegacy → .x3g
// The original .gcode file is removed after successful conversion.
// =============================================================================

#include <string>
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

// Native Orca-controlled GPX / X3G export wrapper.
// This is intentionally not a user post-processing script: Orca owns the export
// decision, chooses a GPX machine profile from the active printer preset, and
// writes the final .x3g path directly.
class GPXExport
{
public:
    static bool export_to_x3g(
        const std::string& gcode_filepath,
        const std::string& output_filepath,
        const PrintConfig& config,
        std::string* error_message = nullptr);

    // Maps the active printer preset (model name + extruder count) to one of
    // GPX's built-in machine ids (the strings GPX itself accepts via "-m").
    // See the .cpp for the verified alias table and its source.
    static std::string gpx_machine_for_config(const PrintConfig& config);
    static std::string find_gpx_binary();

    // ── Dispatch-friendly wrapper, API-compatible with MakerBotExport:: and
    // UltimakerUFPExport::'s pack_to_archive() ──────────────────────────────
    static std::string get_archive_extension(GCodeFlavor flavor);

    // gcode_path : path of the freshly written .gcode file
    // config     : full print config
    // Returns the path of the created .x3g file on success, or an empty
    // string if conversion was not needed (flavor isn't gcfMakerBotLegacy) or
    // failed (e.g. gpx not installed - check the log for the exact reason).
    static std::string pack_to_archive(const std::string& gcode_path,
                                        const PrintConfig& config);
};

} // namespace Slic3r

#endif // slic3r_Format_GPXExport_hpp_
