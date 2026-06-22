#ifndef slic3r_Format_GCodeArchiveUtils_hpp_
#define slic3r_Format_GCodeArchiveUtils_hpp_

// MakerBot / UltiMaker Fork – Orca Slicer 2.4
// =============================================================================
// Vendor-neutral helpers shared by the post-processing exporters that turn a
// finished, plain Orca .gcode file into a vendor-native archive format
// (Format/MakerBotExport.cpp → .makerbot, Format/UltimakerUFPExport.cpp → .ufp).
//
// This module knows NOTHING about MakerBot or UltiMaker specifically - it only
// deals with concerns that are generic to "reading Orca's standard G-code
// comment/thumbnail format" and "writing a ZIP archive". Neither vendor module
// includes the other; both depend only on this neutral leaf module (plus
// PrintConfig). This means MakerBot support can be entirely removed from a
// build (or vice versa) without breaking the other vendor's export.
// =============================================================================

#include <string>
#include <vector>

#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {
namespace GCodeArchiveUtils {

// ── Locale-safe numeric parsing ─────────────────────────────────────────────
//
// BACKGROUND: std::stod/strtod follow the GLOBAL C locale (set via
// setlocale()). wxWidgets calls setlocale(LC_ALL,"") at startup, which under
// e.g. a German system locale switches LC_NUMERIC to comma-decimal/point-
// thousands - PROCESS-WIDE, not just in the GUI. G-code settings comments are
// always point-format ("0.2"), so under that locale, std::stod silently
// mangles point-values to 0.0 or truncates them at the decimal point.
//
// FIX: std::istringstream with explicit imbue(std::locale::classic()) forces
// the point convention for THIS stream only, fully independent of the global
// process locale.

// RAII guard: forces LC_NUMERIC="C" for the scope, restores the previous
// state on exit. Defense-in-depth alongside parse_double_safe() below, to
// protect any other not-yet-discovered locale-sensitive call in the same
// call path (e.g. inside third-party code) without affecting the rest of the
// running application (such as comma-formatted numbers in the GUI).
class ScopedCNumericLocale
{
public:
    ScopedCNumericLocale();
    ~ScopedCNumericLocale();
    ScopedCNumericLocale(const ScopedCNumericLocale&) = delete;
    ScopedCNumericLocale& operator=(const ScopedCNumericLocale&) = delete;
private:
    std::string m_prev;
};

double parse_double_safe(const std::string& s, double fallback);
int    parse_int_safe(const std::string& s, int fallback);

// Parses "3h 52m 45s" / "3h52m45s" / "13833" (plain seconds) into seconds.
int hms_to_seconds(const std::string& s);

// ── Thumbnail extraction ─────────────────────────────────────────────────────
//
// Orca embeds preview thumbnails as base64-encoded PNGs inside
// "; thumbnail begin WxH ..." / "; thumbnail end" G-code comment blocks,
// regardless of the selected G-code flavor. Reading this format is therefore
// not a MakerBot- or UltiMaker-specific concern.

struct ExtractedThumbnail { int width = 0, height = 0; std::string png_bytes; };

std::vector<ExtractedThumbnail> extract_gcode_thumbnails(const std::string& gcode_path);

// Returns an exact width/height match if available, otherwise the largest
// thumbnail found, or nullptr if none exist.
const ExtractedThumbnail* choose_best_thumbnail(const std::vector<ExtractedThumbnail>& thumbs, int w, int h);

// ── Build-plate footprint ───────────────────────────────────────────────────
//
// Reads the bed/printable-area footprint (X/Y, in millimetres) from the
// "printable_area" config option. Returns false (out_x_mm/out_y_mm left
// untouched) if the option is missing, malformed, or implausibly small
// (<10mm); callers should fall back to a printer-specific default in that
// case.
//
// NOTE: printable_area points are already plain millimetres (a user-facing
// DynamicPrintConfig option, not an internal scaled/clipper coordinate) - no
// unit conversion is needed or should be applied here.
bool read_printable_area_size_mm(const PrintConfig& config, double& out_x_mm, double& out_y_mm);

// ── Misc ─────────────────────────────────────────────────────────────────────

// Today's date as "YYYY-MM-DD" (digits/'-' only, independent of locale).
std::string build_iso_date_today();

// Mass (grams) of `extrusion_mm` of filament with the given diameter/density.
double extrusion_mass_g(double extrusion_mm, double filament_diameter_mm, double density_g_cm3);

// Writes `entries` (archive path → raw bytes) as a ZIP file at `out_path`.
bool write_zip_archive(const std::string& out_path,
                        const std::vector<std::pair<std::string, std::string>>& entries);

} // namespace GCodeArchiveUtils
} // namespace Slic3r

#endif // slic3r_Format_GCodeArchiveUtils_hpp_
