#ifndef slic3r_Format_UltimakerUFPExport_hpp_
#define slic3r_Format_UltimakerUFPExport_hpp_

// MakerBot / UltiMaker Fork – Orca Slicer 2.4
// =============================================================================
// Post-processes a finished G-code file into the native .ufp (UltiMaker
// Format Package) archive required by genuine UltiMaker S-Line printers
// (S3/S5/2+ Connect etc.) and by Cura Connect / the UltiMaker Digital
// Factory to correctly read back print time, material usage and a preview
// thumbnail. Called from GCode::do_export() after the .tmp → .gcode rename
// succeeds.
//
// This module is UltiMaker-only and has no dependency on, or knowledge of,
// MakerBot/.makerbot export - that lives entirely independently in
// Format/MakerBotExport.hpp/.cpp. Removing MakerBot device support from a
// build does not affect this module at all. The two modules only share the
// vendor-neutral helpers in Format/GCodeArchiveUtils.hpp (locale-safe
// number parsing, thumbnail extraction, ZIP writing) - neither includes the
// other.
//
// .ufp is an OPC (Open Packaging Conventions) container, the same family of
// format as .docx/.3mf. The embedded G-code must additionally start with a
// structured "Griffin" header (;START_OF_HEADER ... ;END_OF_HEADER) - real
// UltiMaker firmware/Cura Connect/Digital Factory tooling (via
// Ultimaker/libCharon's GCodeFile parser) rejects files without it.
// Reference used while implementing this: Ultimaker/libCharon
// (GCodeFile.py, OpenPackagingConvention.py) and Ultimaker/Cura
// (plugins/UFPWriter/UFPWriter.py), as published on GitHub (checked against
// the "Cura 5.12" / current master branch, June 2026).
//
// Output: gcfUltiGCode → .ufp (OPC ZIP: 3D/model.gcode incl. Griffin header,
//                                Metadata/thumbnail.png, Cura/slicemetadata.json)
//
// The original .gcode file is removed after successful archive creation
// (unless the archive path equals the input path).
// =============================================================================

#include <string>
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {
namespace UltimakerUFPExport {

// Returns the canonical output extension (with leading dot) for the given flavor.
std::string get_archive_extension(GCodeFlavor flavor);

// Main entry point – called from GCode::do_export() after the G-code is written.
//
// gcode_path : path of the freshly written .gcode file
// config     : full print config (used for printer model, temperatures, etc.)
//
// Returns the path of the created .ufp archive on success, or an empty
// string if packing was not needed (flavor isn't gcfUltiGCode) or failed.
std::string pack_to_archive(const std::string& gcode_path,
                             const PrintConfig& config);

} // namespace UltimakerUFPExport
} // namespace Slic3r

#endif // slic3r_Format_UltimakerUFPExport_hpp_
