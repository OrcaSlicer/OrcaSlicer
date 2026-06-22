#include "UltimakerUFPExport.hpp"
#include "GCodeArchiveUtils.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <locale>
#include <sstream>
#include <fstream>
#include <string>
#include <vector>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

namespace Slic3r {
namespace UltimakerUFPExport {

// Generische, vendor-neutrale Hilfsfunktionen (siehe GCodeArchiveUtils.hpp) -
// nur in dieser Übersetzungseinheit sichtbar, keine Abhängigkeit auf
// Format/MakerBotExport.* irgendeiner Art.
using namespace GCodeArchiveUtils;

// ── Public: archive extension helper ────────────────────────────────────────

std::string get_archive_extension(GCodeFlavor flavor)
{
    return flavor == gcfUltiGCode ? ".ufp" : ".gcode";
}

// ── Internal: minimal, UltiMaker-eigener G-code-Header-Parser ──────────────
//
// Bewusst NICHT identisch mit MakerBotExport.cpp's parse_header() - dieses
// Modul braucht nur eine kleine Teilmenge der Felder (kein tool_type, keine
// Birdwing-Retraction-/Speed-Profile usw.) und soll komplett unabhängig vom
// MakerBot-Modul bleiben, auch wenn beide denselben Orca-Kommentarstil lesen.

struct GriffinSourceData
{
    double first_layer_temp   = 215.0;
    double layer_height       = 0.20;
    double filament_diameter  = 1.75;
    double filament_density   = 1.24;
    int    duration_s         = 0;
    int    num_layers         = 0;
    double total_filament_mm  = 0.0;
};

static GriffinSourceData parse_griffin_source_data(const std::string& gcode_path)
{
    const ScopedCNumericLocale locale_guard; // siehe GCodeArchiveUtils.hpp
    GriffinSourceData d;

    std::ifstream gf(gcode_path);
    if (!gf.is_open()) return d;

    std::string line;
    while (std::getline(gf, line)) {
        // ── Filament-Akkumulation (G0/G1 mit positivem E) ───────────────
        if (line.size() >= 2 && line[0] == 'G' && (line[1] == '0' || line[1] == '1')) {
            const size_t e_pos = line.find('E');
            if (e_pos != std::string::npos) {
                const size_t semi = line.find(';');
                if (semi == std::string::npos || semi > e_pos) {
                    const double e = parse_double_safe(line.substr(e_pos + 1), 0.0);
                    if (e > 0.0) d.total_filament_mm += e;
                }
            }
            continue;
        }

        // ── Settings-Kommentare ──────────────────────────────────────────
        if (line.empty() || line[0] != ';') continue;

        if (line.find("thumbnail begin") != std::string::npos) {
            while (std::getline(gf, line) && line.find("thumbnail end") == std::string::npos) {}
            continue;
        }

        std::string comment = line.substr(1);
        boost::algorithm::trim(comment);

        size_t sep = comment.find('=');
        if (sep == std::string::npos) sep = comment.find(':');
        if (sep == std::string::npos) continue;

        std::string key = comment.substr(0, sep);
        std::string val = comment.substr(sep + 1);

        const size_t paren = key.find('(');
        if (paren != std::string::npos) key = key.substr(0, paren);

        boost::algorithm::trim(key);
        boost::algorithm::trim(val);
        boost::algorithm::to_lower(key);

        if      (key == "nozzle_temperature_initial_layer" || key == "first_layer_temperature")
                                                d.first_layer_temp  = parse_double_safe(val, d.first_layer_temp);
        else if (key == "layer_height")        d.layer_height       = parse_double_safe(val, d.layer_height);
        else if (key == "filament_diameter")   d.filament_diameter  = parse_double_safe(val, d.filament_diameter);
        else if (key == "filament_density")    d.filament_density   = parse_double_safe(val, d.filament_density);
        else if (key == "total layer number")  d.num_layers         = parse_int_safe(val, d.num_layers);
        else if (key.find("estimated printing time") != std::string::npos)
                                                d.duration_s         = std::max(d.duration_s, hms_to_seconds(val));
    }
    return d;
}

// ── Griffin-Header (von libCharon zwingend verlangt) ────────────────────────
//
// Ersetzt die frühere, hier nie aufgerufene Platzhalterimplementierung
// dieser Klasse, UND die separate, im MakerBot-Modul lebende
// pack_ufp()-Notlösung (die einen falschen OPC-Relationship-Typ benutzte und
// gar keinen Griffin-Header schrieb). Jedes hier geschriebene Feld ist 1:1
// an __validateGriffinHeader() in libCharon's GCodeFile.py ausgerichtet -
// empirisch gegen eine Python-Nachbildung dieser Validierung getestet
// (Einzel- UND Dual-Extruder-Fall, beide bestehen die Prüfung).
//
// Referenz: Ultimaker/libCharon (GCodeFile.py) und Ultimaker/Cura
// (plugins/UFPWriter/UFPWriter.py), Stand Juni 2026 ("Cura 5.12").

struct GriffinBBox
{
    double min_x =  std::numeric_limits<double>::infinity();
    double max_x = -std::numeric_limits<double>::infinity();
    double min_y =  std::numeric_limits<double>::infinity();
    double max_y = -std::numeric_limits<double>::infinity();
    double min_z =  std::numeric_limits<double>::infinity();
    double max_z = -std::numeric_limits<double>::infinity();

    void update(double x, double y, double z)
    {
        min_x = std::min(min_x, x); max_x = std::max(max_x, x);
        min_y = std::min(min_y, y); max_y = std::max(max_y, y);
        min_z = std::min(min_z, z); max_z = std::max(max_z, z);
    }
};

static std::string build_griffin_header(
    const PrintConfig&       config,
    const GriffinSourceData& d,
    const GriffinBBox&       bbox)
{
    std::string machine = "ultimaker3"; // generischer, breit kompatibler Fallback
    if (const auto* opt = config.option("printer_model"))
        if (const auto* s = dynamic_cast<const ConfigOptionString*>(opt))
            if (!s->value.empty()) machine = s->value;

    int bed_temp = 0; // 0 ist laut isAPositiveNumber() gültig (>= 0)
    if (const auto* opt = config.option("first_layer_bed_temperature"))
        if (const auto* v = dynamic_cast<const ConfigOptionInts*>(opt))
            if (!v->values.empty() && v->values[0] > 0) bed_temp = v->values[0];
    if (bed_temp <= 0)
        if (const auto* opt = config.option("bed_temperature"))
            if (const auto* v = dynamic_cast<const ConfigOptionInts*>(opt))
                if (!v->values.empty()) bed_temp = v->values[0];

    // Düsendurchmesser pro Extruder für EXTRUDER_TRAIN.<n>.NOZZLE.DIAMETER.
    std::vector<double> nozzle_d = { 0.4 };
    if (const auto* opt = config.option("nozzle_diameter"))
        if (const auto* v = dynamic_cast<const ConfigOptionFloats*>(opt))
            if (!v->values.empty()) nozzle_d = v->values;

    const double volume_used_mm3 =
        3.14159265358979323846 * std::pow(d.filament_diameter / 2.0, 2) * d.total_filament_mm;

    std::ostringstream hdr;
    hdr.imbue(std::locale::classic()); // Zahlen IMMER im Punkt-Format - siehe GCodeArchiveUtils.hpp
    hdr << ";START_OF_HEADER\n";
    hdr << ";HEADER_VERSION:0.1\n";
    hdr << ";FLAVOR:Griffin\n";
    hdr << ";GENERATOR.NAME:OrcaSlicer\n";
    hdr << ";GENERATOR.VERSION:" << SLIC3R_VERSION << "\n";
    hdr << ";GENERATOR.BUILD_DATE:" << build_iso_date_today() << "\n";
    hdr << ";TARGET_MACHINE.NAME:" << machine << "\n";
    hdr << ";PRINT.TIME:" << d.duration_s << "\n";
    hdr << ";PRINT.SIZE.MIN.X:" << bbox.min_x << "\n";
    hdr << ";PRINT.SIZE.MIN.Y:" << bbox.min_y << "\n";
    hdr << ";PRINT.SIZE.MIN.Z:" << bbox.min_z << "\n";
    hdr << ";PRINT.SIZE.MAX.X:" << bbox.max_x << "\n";
    hdr << ";PRINT.SIZE.MAX.Y:" << bbox.max_y << "\n";
    hdr << ";PRINT.SIZE.MAX.Z:" << bbox.max_z << "\n";
    hdr << ";BUILD_PLATE.INITIAL_TEMPERATURE:" << bed_temp << "\n";
    for (size_t i = 0; i < nozzle_d.size(); ++i) {
        hdr << ";EXTRUDER_TRAIN." << i << ".INITIAL_TEMPERATURE:" << d.first_layer_temp << "\n";
        // Mangels Tool-Change-Tracking in parse_griffin_source_data() wird
        // der gesamte Filamentverbrauch Extruder 0 zugerechnet (bewusste,
        // dokumentierte Vereinfachung; 0.0 ist für Werkzeug 1+ trotzdem ein
        // laut Spec gültiger Wert, isAPositiveNumber() erlaubt >= 0).
        hdr << ";EXTRUDER_TRAIN." << i << ".MATERIAL.VOLUME_USED:" << (i == 0 ? volume_used_mm3 : 0.0) << "\n";
        hdr << ";EXTRUDER_TRAIN." << i << ".NOZZLE.DIAMETER:" << nozzle_d[i] << "\n";
    }
    hdr << ";END_OF_HEADER\n";
    return hdr.str();
}

// ── OPC-Containerstruktur ([Content_Types].xml + _rels) ────────────────────

static std::string build_ufp_content_types_xml(bool has_thumbnail)
{
    std::ostringstream xml;
    xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    xml << "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">\n";
    xml << "  <Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>\n";
    xml << "  <Default Extension=\"gcode\" ContentType=\"text/x-gcode\"/>\n";
    if (has_thumbnail)
        xml << "  <Default Extension=\"png\" ContentType=\"image/png\"/>\n";
    xml << "  <Default Extension=\"json\" ContentType=\"application/json\"/>\n";
    xml << "</Types>\n";
    return xml.str();
}

static std::string build_ufp_root_rels_xml()
{
    // Der reale, von libCharon (OpenPackagingConvention.py) erwartete
    // Relationship-Typ fürs G-code-Teil ist ".../relationships/gcode".
    std::ostringstream xml;
    xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    xml << "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n";
    xml << "  <Relationship Id=\"rel0\" Type=\"http://schemas.ultimaker.org/package/2018/relationships/gcode\" Target=\"/3D/model.gcode\"/>\n";
    xml << "</Relationships>\n";
    return xml.str();
}

static std::string build_ufp_model_rels_xml()
{
    std::ostringstream xml;
    xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    xml << "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n";
    xml << "  <Relationship Id=\"rel0\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/thumbnail\" Target=\"/Metadata/thumbnail.png\"/>\n";
    xml << "</Relationships>\n";
    return xml.str();
}

// ── Public entry point ───────────────────────────────────────────────────────

std::string pack_to_archive(const std::string& gcode_path, const PrintConfig& config)
{
    if (config.gcode_flavor != gcfUltiGCode)
        return {}; // nicht unsere Flavor - nichts zu tun

    namespace fs = boost::filesystem;

    const fs::path gcode_p(gcode_path);
    const std::string archive_path = (gcode_p.parent_path() / (gcode_p.stem().string() + ".ufp")).string();

    // BUG FIX (2026-06-19): identical collision bug already fixed in
    // GPXExport::pack_to_archive (x3g/Legacy) and MakerBotExport::pack_to_archive
    // (Birdwing/.makerbot, Lava). When the export dialog already names the
    // target with the final ".ufp" extension, gcode_path and archive_path
    // collapse onto the same file. The code below used to read the G-code
    // into memory, write the UFP zip OVER that same path, and then
    // unconditionally fs::remove(gcode_path) - deleting the just-written
    // archive with no trace in the log (the try/catch swallows it silently).
    //
    // Fix: stage the source G-code aside to a distinct temp path whenever
    // the paths collide, exactly mirroring the other two modules.
    std::string gcode_source = gcode_path;
    bool used_temp_source = false;
    if (archive_path == gcode_path) {
        gcode_source = gcode_path + ".tmp_gcode_for_ufp";
        try {
            fs::rename(gcode_path, gcode_source);
            used_temp_source = true;
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(warning) << "UltimakerUFPExport: could not stage temp G-code for "
                << gcode_path << ": " << e.what();
            return {};
        }
    }

    std::string gcode;
    {
        std::ifstream f(gcode_source, std::ios::binary);
        if (!f.is_open()) {
            BOOST_LOG_TRIVIAL(error) << "UltimakerUFPExport: input G-code not found: " << gcode_source;
            if (used_temp_source) {
                try { fs::rename(gcode_source, gcode_path); } catch (...) {}
            }
            return {};
        }
        gcode.assign(std::istreambuf_iterator<char>(f), {});
    }

    const GriffinSourceData data = parse_griffin_source_data(gcode_source);
    const std::vector<ExtractedThumbnail> thumbnails = extract_gcode_thumbnails(gcode_source);

    double bed_x = 0.0, bed_y = 0.0;
    if (!read_printable_area_size_mm(config, bed_x, bed_y)) { bed_x = 220.0; bed_y = 220.0; } // generischer UltiMaker-Fallback

    GriffinBBox bbox; // Bauraum-Bounding-Box als Näherung (kein echtes Modell-Bounding-Box-Tracking hier)
    bbox.update(-bed_x / 2.0, -bed_y / 2.0, 0.0);
    bbox.update( bed_x / 2.0,  bed_y / 2.0, data.layer_height * data.num_layers);

    const std::string griffin_header = build_griffin_header(config, data, bbox);
    const std::string full_gcode = griffin_header + gcode;

    const ExtractedThumbnail* thumb = choose_best_thumbnail(thumbnails, 320, 320);
    if (!thumb && !thumbnails.empty()) thumb = &thumbnails.front();

    nlohmann::json slicemeta;
    slicemeta["generator"]         = "OrcaSlicer";
    slicemeta["generator_version"] = SLIC3R_VERSION;
    slicemeta["material"]          = { {"length_mm", data.total_filament_mm},
                                        {"weight_g",  extrusion_mass_g(data.total_filament_mm, data.filament_diameter, data.filament_density)} };
    slicemeta["print_time_s"]      = data.duration_s;
    slicemeta["layer_height"]      = data.layer_height;

    std::vector<std::pair<std::string, std::string>> entries;
    entries.emplace_back("[Content_Types].xml",       build_ufp_content_types_xml(thumb != nullptr));
    entries.emplace_back("_rels/.rels",                build_ufp_root_rels_xml());
    entries.emplace_back("3D/model.gcode",             full_gcode);
    entries.emplace_back("Cura/slicemetadata.json",    slicemeta.dump(4));
    if (thumb) {
        entries.emplace_back("3D/_rels/model.gcode.rels", build_ufp_model_rels_xml());
        entries.emplace_back("Metadata/thumbnail.png",     thumb->png_bytes);
    }

    if (!write_zip_archive(archive_path, entries)) {
        BOOST_LOG_TRIVIAL(error) << "UltimakerUFPExport: failed to write UFP archive: " << archive_path;
        if (used_temp_source) {
            try { fs::rename(gcode_source, gcode_path); } catch (...) {}
        }
        return {};
    }

    // gcode_source is now guaranteed to differ from archive_path - either it
    // was already distinct, or it is our temp staging file - so this can
    // never remove the archive we just wrote.
    try { fs::remove(gcode_source); } catch (...) {}

    BOOST_LOG_TRIVIAL(info) << "UltimakerUFPExport: UFP archive created: " << archive_path
        << " [" << data.duration_s << "s, " << data.total_filament_mm << "mm filament, Griffin header included]";
    return archive_path;
}

} // namespace UltimakerUFPExport
} // namespace Slic3r
