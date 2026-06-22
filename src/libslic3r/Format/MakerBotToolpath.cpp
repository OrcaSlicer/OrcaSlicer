// MakerBot / UltiMaker Fork – Orca Slicer 2.4
// MakerBotToolpath.cpp  — VOLLSTÄNDIG ÜBERARBEITET 2026-06-14
//
// G-code → Birdwing JSON Toolpath converter.
// Basiert auf Reverse-Engineering des 1cm_x_1cm_block_Rep+.makerbot
// aus MakerBot Print 4.10.1 (Resources.zip/app.asar.unpacked/)
//
// KRITISCHE KORREKTUREN gegenüber der Vorversion:
//  1. JSON-Struktur:   Jeder Befehl in {"command":{...}} eingebettet
//  2. Metadata-Feld:   {"relative":{"a":true,"x":false,"y":false,"z":false}}
//  3. Tag-Namen:       "Travel Move","Retract","Restart","Inset","Infill",
//                      "Trailing Extrusion Move","Leaky Travel Move","Connection"
//  4. Retract-Moves:   Tag="Retract", a=negative mm (MK13: -0.5, MK12: -1.0)
//  5. Restart-Moves:   Tag="Restart", a=positive mm (~0.6)
//  6. Z-Tracking:      cur_z immer aktualisiert (auch vor in_print_area)
//  7. Kommentare:      "Layer Section N (M)" statt "chunk N (N)"
//  8. Feedrate:        F/60 (mm/min→mm/s) korrekt aus G-code übernommen
//  9. Koordinaten:     Vollständige Float-Präzision (keine Integer-Rundung)

#include "MakerBotToolpath.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cmath>
#include <cctype>
#include <clocale>
#include <locale>
#include <algorithm>

#include <nlohmann/json.hpp>
#include <boost/log/trivial.hpp>
#include <boost/algorithm/string.hpp>

namespace Slic3r {

// ── Locale-unabhängiges Double-Parsing ──────────────────────────────────────
// Gleicher Fehler / gleicher Fix wie in MakerBotExport.cpp::parse_double_safe:
// std::stod richtet sich nach dem globalen C-Locale (setlocale), das
// wxWidgets beim Start typischerweise auf das System-Locale setzt. Unter
// einem Komma-Dezimal-Locale (z.B. de_DE) werden Punkt-Werte wie "123.45"
// aus dem G-code silent fehlinterpretiert. Diese Funktion parst IMMER nach
// der "C"-Konvention (Punkt als Dezimaltrennzeichen), unabhängig vom
// Prozess-Locale – betrifft hier JEDE X/Y/Z/E-Koordinate und jeden
// Linienbreiten-Wert im realen Toolpath, der an den Drucker geschickt wird.
static bool parse_double_locale_safe(const std::string& s, double& out)
{
    std::istringstream iss(s);
    iss.imbue(std::locale::classic());
    double v = 0.0;
    iss >> v;
    if (iss.fail() || !std::isfinite(v)) return false;
    out = v;
    return true;
}

// Defense-in-depth, identisch zu MakerBotExport.cpp (siehe dort für Details):
// erzwingt LC_NUMERIC="C" für die Dauer des Scopes, stellt danach den
// vorherigen Zustand wieder her, beeinflusst also nicht die restliche GUI.
class ScopedCNumericLocale
{
public:
    ScopedCNumericLocale()
    {
        const char* cur = std::setlocale(LC_NUMERIC, nullptr);
        m_prev = cur ? cur : "C";
        std::setlocale(LC_NUMERIC, "C");
    }
    ~ScopedCNumericLocale() { std::setlocale(LC_NUMERIC, m_prev.c_str()); }
    ScopedCNumericLocale(const ScopedCNumericLocale&) = delete;
    ScopedCNumericLocale& operator=(const ScopedCNumericLocale&) = delete;
private:
    std::string m_prev;
};

// ── Tag-Mapping: Orca ;TYPE: → Birdwing JSON-Tag ────────────────────────────
// Referenz: 1cm_x_1cm_block_Rep+.makerbot aus MakerBot Print 4.10.1
// Gültige Tags: "Trailing Extrusion Move", "Infill", "Inset",
//               "Leaky Travel Move", "Travel Move", "Connection",
//               "Retract", "Restart", "Support", "Outline"
static std::string orca_type_to_birdwing_tag(const std::string& orca_type)
{
    // "Outline" = Außenwand (MakerBot Desktop 3.x / Birdwing)
    // "Inset"   = Innenwand
    if (orca_type == "Outer wall")             return "Outline";
    if (orca_type == "Inner wall")             return "Inset";
    if (orca_type == "Sparse infill")          return "Infill";
    if (orca_type == "Internal solid infill")  return "Infill";
    if (orca_type == "Top surface")            return "Infill";
    if (orca_type == "Bottom surface")         return "Infill";
    if (orca_type == "Bridge infill")          return "Infill";
    if (orca_type == "Internal Bridge infill") return "Infill";
    if (orca_type == "Support")                return "Support";
    if (orca_type == "Support interface")      return "Support";
    if (orca_type == "Overhang wall")          return "Outline";
    if (orca_type == "Skirt")                  return "Outline";
    if (orca_type == "Brim")                   return "Outline";
    if (orca_type == "Raft")                   return "Infill";
    if (orca_type == "Custom")                 return ""; // start/end gcode → skip
    if (orca_type == "Wipe")                   return "Trailing Extrusion Move";
    return "Infill"; // sicherer Default
}

// ── G-code-Parameter parsen (z.B. "X123.45" → 123.45) ───────────────────────
static bool parse_gcode_param(const std::string& token, char axis, double& out)
{
    if (token.empty() || std::toupper(token[0]) != std::toupper(axis))
        return false;
    return parse_double_locale_safe(token.substr(1), out);
}

// ── Befehl als {"command":{...}} emittieren ───────────────────────────────────
// KRITISCH: MakerBot Print erwartet IMMER den äußeren "command"-Wrapper!
static nlohmann::json make_command(
    const std::string& function_name,
    const nlohmann::json& parameters,
    const std::vector<std::string>& tags,
    bool relative_a = true)
{
    nlohmann::json cmd;
    cmd["function"] = function_name;
    cmd["metadata"] = {
        {"relative", {
            {"a", relative_a},
            {"x", false},
            {"y", false},
            {"z", false}
        }}
    };
    cmd["parameters"] = parameters;
    cmd["tags"] = tags;
    return {{"command", cmd}};
}

static nlohmann::json make_comment(const std::string& text)
{
    return make_command("comment",
        {{"comment", text}},
        nlohmann::json::array(),
        false);
}

// ── Layer-Kommentarblock emittieren ──────────────────────────────────────────
// Referenz-Format aus 1cm_x_1cm_block_Rep+.makerbot:
//   "Layer Section 0 (1)"
//   "Material 0"
//   "Lower Position  0"
//   "Upper Position  0.3"
//   "Thickness       0.3"
//   "Width           2.5"
static void emit_layer_comments(
    nlohmann::json& commands,
    int    layer_idx,     // 0-basiert
    double z_upper,
    double z_lower,
    double thickness,
    double width)
{
    auto fmt = [](double v) -> std::string {
        // MakerBot nutzt kein Trailing-Zero-Format – direkte String-Konvertierung
        std::ostringstream o;
        o << v;
        return o.str();
    };
    // Layer-Index 0-basiert, Display-Nummer 1-basiert (in Klammern)
    commands.push_back(make_comment(
        "Layer Section " + std::to_string(layer_idx) + " (" +
        std::to_string(layer_idx + 1) + ")"));
    commands.push_back(make_comment("Material 0"));
    // Eingerückte Felder mit konsistenter Breite wie im Original
    commands.push_back(make_comment("Lower Position  " + fmt(z_lower)));
    commands.push_back(make_comment("Upper Position  " + fmt(z_upper)));
    commands.push_back(make_comment("Thickness       " + fmt(thickness)));
    commands.push_back(make_comment("Width           " + fmt(width)));
}

// ══════════════════════════════════════════════════════════════════════════════
// HAUPT-KONVERTER: G-code → Birdwing JSON Toolpath
// ══════════════════════════════════════════════════════════════════════════════
std::string gcode_to_birdwing_jsontoolpath(
    const std::string&        gcode_path,
    const BirdwingBuildVolume& bv,
    double                    layer_height,
    std::string&              error)
{
    const ScopedCNumericLocale locale_guard; // siehe Kommentar an der Klassendefinition
    std::ifstream f(gcode_path);
    if (!f.is_open()) {
        error = "Cannot open: " + gcode_path;
        return "";
    }

    // Koordinaten-Offset: Orca-Ursprung (links-vorne-unten) →
    // MakerBot-Ursprung (Mitte des Druckbetts)
    const double x_offset = bv.x / 2.0;   // Z18: 150.0
    const double y_offset = bv.y / 2.0;   // Z18: 152.5

    nlohmann::json commands = nlohmann::json::array();

    // ── Parser-Zustand ──────────────────────────────────────────────────────
    // Orca nutzt IMMER relative E (use_relative_e_distances=1 für gcfMakerBotBirdwing)
    // MakerBot G-code sendet kein M82/M83 – absolute_ext = false ist fest.
    const bool absolute_ext = false;
    bool       absolute_pos = true;

    double cur_x = 0, cur_y = 0, cur_z = 0, cur_e = 0;
    double cur_feedrate = 40.0; // mm/s Default
    double layer_z_lower = 0.0;
    int    layer_idx     = -1;
    bool   in_print_area = false;
    bool   retracted     = false;

    std::string current_tag = "Outline";
    double      layer_w     = bv.layer_width;

    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        // ── Kommentare / Direktiven ──────────────────────────────────────────
        const size_t semi = line.find(';');
        const std::string body = (semi != std::string::npos)
                                 ? line.substr(0, semi) : line;
        const std::string comment = (semi != std::string::npos)
                                    ? line.substr(semi + 1) : "";

        // ;TYPE:xxx – Klassifikation der folgenden Moves
        if (!comment.empty() && comment.find("TYPE:") == 0) {
            const std::string type = boost::trim_copy(comment.substr(5));
            const std::string new_tag = orca_type_to_birdwing_tag(type);
            if (new_tag.empty()) {
                // "Custom" → start/end G-code → Ausgabe pausieren
                in_print_area = false;
            } else {
                current_tag   = new_tag;
                in_print_area = true;
            }
            continue;
        }

        // ;WIDTH:xxx – Linienbreite
        if (!comment.empty() && comment.find("WIDTH:") == 0) {
            double w = 0.0;
            if (parse_double_locale_safe(boost::trim_copy(comment.substr(6)), w))
                layer_w = w;
            continue;
        }

        const std::string cmd_str = boost::trim_copy(body);
        if (cmd_str.empty()) continue;

        std::vector<std::string> tokens;
        {
            std::istringstream ss(cmd_str);
            std::string tok;
            while (ss >> tok) tokens.push_back(tok);
        }
        if (tokens.empty()) continue;
        const std::string& cmd = tokens[0];

        // ── Positioniermodus ─────────────────────────────────────────────────
        if (cmd == "G90") { absolute_pos = true;  continue; }
        if (cmd == "G91") { absolute_pos = false; continue; }
        if (cmd == "M82") { /* absolute_ext = true  – bei Birdwing ignoriert */ continue; }
        if (cmd == "M83") { /* absolute_ext = false – bei Birdwing Standard  */ continue; }

        // ── G28 – Alle Achsen homen ───────────────────────────────────────────
        if (cmd == "G28") {
            cur_x = 0; cur_y = 0; cur_z = 0;
            continue;
        }

        // ── G92 – Achsenposition setzen ──────────────────────────────────────
        if (cmd == "G92") {
            bool e_reset = false;
            for (size_t i = 1; i < tokens.size(); ++i) {
                double v = 0;
                if (parse_gcode_param(tokens[i], 'E', v)) { cur_e = v; e_reset = true; }
                if (parse_gcode_param(tokens[i], 'X', v)) cur_x = v;
                if (parse_gcode_param(tokens[i], 'Y', v)) cur_y = v;
                if (parse_gcode_param(tokens[i], 'Z', v)) cur_z = v;
            }
            if (e_reset) {
                // Nach G92 E0: Retract-Zustand zurücksetzen (kritisch für
                // korrekte E-Tracking-Akkumulation nach Purge-Linie)
                retracted = false;
            }
            continue;
        }

        // ── G1 / G0 – Bewegungsbefehl ─────────────────────────────────────────
        if (cmd == "G1" || cmd == "G0") {
            double nx = cur_x, ny = cur_y, nz = cur_z, ne = cur_e, nf = -1.0;
            bool has_x = false, has_y = false, has_z = false, has_e = false;

            for (size_t i = 1; i < tokens.size(); ++i) {
                double v = 0;
                if (parse_gcode_param(tokens[i], 'X', v)) {
                    nx = absolute_pos ? v : cur_x + v; has_x = true;
                }
                if (parse_gcode_param(tokens[i], 'Y', v)) {
                    ny = absolute_pos ? v : cur_y + v; has_y = true;
                }
                if (parse_gcode_param(tokens[i], 'Z', v)) {
                    nz = absolute_pos ? v : cur_z + v; has_z = true;
                }
                if (parse_gcode_param(tokens[i], 'E', v)) {
                    // Orca Birdwing: IMMER relativ (absolute_ext=false)
                    // → ne = cur_e + e_raw, e_delta = e_raw
                    ne = absolute_ext ? v : cur_e + v;
                    has_e = true;
                }
                if (parse_gcode_param(tokens[i], 'F', v)) {
                    nf = v / 60.0; // mm/min → mm/s
                }
            }

            // Feedrate aktualisieren wenn angegeben
            if (nf > 0) cur_feedrate = nf;

            // Vorherige Position merken – VOR dem Position-Update, damit
            // die Distanzberechnung für die a-Wert-Kalkulation unten stimmt.
            const double prev_x = cur_x;
            const double prev_y = cur_y;

            // Z IMMER aktualisieren (auch vor in_print_area!)
            if (has_z) {
                // Layer-Wechsel: neues Z-Maximum erreicht (monoton steigend).
                // Z-Hops (hoch dann runter) werden korrekt ignoriert, weil nach dem
                // Hop das Z wieder auf das vorherige Niveau zurückfällt.
                // FIX: Keine fragile Toleranz-Prüfung mehr – nur Z > letztes Z-Max.
                if (in_print_area && nz > layer_z_lower + 1e-5) {
                    ++layer_idx;
                    const double thickness = nz - layer_z_lower;
                    const double width = (layer_w > 1e-5) ? layer_w : 0.4; // Fallback
                    emit_layer_comments(commands, layer_idx,
                        nz,           // upper = neue Z
                        layer_z_lower,// lower = vorheriges Z-Maximum
                        thickness,
                        width);
                    layer_z_lower = nz;
                }
                cur_z = nz;
            }

            // Positions- und E-Stand aktualisieren
            cur_x = nx; cur_y = ny; cur_e = ne;

            // Vor dem Druckbereich: keine Moves ausgeben
            if (!in_print_area) continue;

            // Kein XY-Move → reiner Retract / Unretract / Z-Hop → separat behandeln
            const bool has_xy = has_x || has_y;

            // E-Delta (in relativem Modus = raw E-Wert direkt)
            // Denn: ne = cur_e_alt + e_raw → e_delta = ne - cur_e_alt = e_raw
            const double e_raw = has_e ? [&]() -> double {
                for (size_t i = 1; i < tokens.size(); ++i) {
                    double v = 0;
                    if (parse_gcode_param(tokens[i], 'E', v)) return v;
                }
                return 0.0;
            }() : 0.0;

            // ── Reiner Retract-Move (kein XY, negatives E) ───────────────────
            if (!has_xy && has_e && e_raw < -1e-4) {
                // KORREKTES FORMAT: Tag="Retract", a=negative mm
                commands.push_back(make_command("move",
                    {
                        {"x", nx - x_offset},
                        {"y", ny - y_offset},
                        {"z", nz},
                        {"a", e_raw},          // negativ! z.B. -0.5 oder -0.8
                        {"feedrate", cur_feedrate}
                    },
                    {"Retract"}));
                retracted = true;
                continue;
            }

            // ── Reiner Restart-Move (kein XY, positives E nach Retract) ─────
            if (!has_xy && has_e && e_raw > 1e-4 && retracted) {
                // KORREKTES FORMAT: Tag="Restart", a=positive mm
                commands.push_back(make_command("move",
                    {
                        {"x", nx - x_offset},
                        {"y", ny - y_offset},
                        {"z", nz},
                        {"a", e_raw},          // positiv! z.B. +0.5 oder +0.6
                        {"feedrate", cur_feedrate}
                    },
                    {"Restart"}));
                retracted = false;
                continue;
            }

            // ── Z-only Move (kein XY, kein E) → Travel Move ─────────────────
            if (!has_xy && !has_e) continue;
            if (!has_xy && has_e && std::fabs(e_raw) < 1e-4) continue;

            // ── Koordinaten validieren ────────────────────────────────────────
            const double json_x = nx - x_offset;
            const double json_y = ny - y_offset;
            if (json_x < -(bv.x / 2 + 10) || json_x > (bv.x / 2 + 10) ||
                json_y < -(bv.y / 2 + 10) || json_y > (bv.y / 2 + 10)) {
                BOOST_LOG_TRIVIAL(debug)
                    << "MakerBotToolpath: skip out-of-bounds x=" << json_x
                    << " y=" << json_y;
                continue;
            }

            // ── XY-Move klassifizieren ────────────────────────────────────────
            std::string tag;

            if (e_raw > 1e-6) {
                // Positives E → Extrusion
                if (retracted) {
                    tag = "Restart";
                    retracted = false;
                } else {
                    // FIX: prev_x/prev_y statt cur_x/cur_y (cur wurde bereits updated!)
                    const double dx   = nx - prev_x;
                    const double dy   = ny - prev_y;
                    const double dist = std::sqrt(dx*dx + dy*dy);
                    tag = (dist < 0.5 && current_tag != "Support")
                          ? "Trailing Extrusion Move"
                          : current_tag;
                }
            } else if (e_raw < -1e-4) {
                // Negative E bei XY-Move → Retract während Bewegung (rare)
                tag = "Retract";
                retracted = true;
            } else if (!has_e && has_xy && in_print_area &&
                       current_tag != "Travel Move" &&
                       current_tag != "Leaky Travel Move") {
                // ── Kein E im G-code, aber Print-TYPE aktiv ──────────────────────
                // Birdwing-Profil in Orca erzeugt G-code OHNE inline E-Werte
                // (z.B. 'G1 X120 Y117 F3000' ohne E). Die Extrusion muss aus
                // der Geometrie berechnet werden:
                //   a = L × layer_height × line_width / (π × (d_fil/2)²)
                // Mit d_fil=1.77mm, A_fil=2.4606mm², layer_height und line_width
                // aus dem Profil.
                //
                // WICHTIG: Nur wenn current_tag ein echter Print-Tag ist
                // (Outline, Inset, Infill, Support, Bridge) – nicht Travel.
                tag = current_tag;
                if (retracted) { tag = "Restart"; retracted = false; }
                // a wird unten berechnet
            } else {
                // Kein / minimales E + kein Print-TAG → Travel
                if (std::fabs(e_raw) < 1e-6) {
                    tag = "Travel Move";
                } else {
                    tag = "Leaky Travel Move";
                }
            }

            // ── a-Wert bestimmen ──────────────────────────────────────────────────
            double a_val;
            if (has_e) {
                // E direkt aus G-code (relativ = delta)
                a_val = e_raw;
            } else if (!tag.empty() &&
                       tag != "Travel Move" &&
                       tag != "Leaky Travel Move") {
                // Kein E im G-code → aus Geometrie berechnen
                // Formel: a = dist × (layer_h × line_w) / A_filament
                // A_filament = π × (1.77/2)² = 2.4606 mm²
                const double dx   = nx - prev_x;
                const double dy   = ny - prev_y;
                const double dist = std::sqrt(dx*dx + dy*dy);
                const double lh   = (layer_height > 1e-5) ? layer_height : 0.2;
                const double lw   = (layer_w     > 1e-5) ? layer_w     : 0.4;
                const double fil_area = 3.14159265358979 * 0.885 * 0.885; // (1.77/2)²
                a_val = dist * lh * lw / fil_area;
            } else {
                a_val = 0.0;
            }

            // ── Move emittieren ───────────────────────────────────────────────
            // Retract hat negatives a_val (e_raw) – darf NICHT geclipt werden!
            // Für alle anderen: a soll >= 0 sein.
            const double a_emit = (tag == "Retract") ? a_val : std::max(0.0, a_val);
            commands.push_back(make_command("move",
                {
                    {"x",        json_x},
                    {"y",        json_y},
                    {"z",        nz},
                    {"a",        a_emit},
                    {"feedrate", cur_feedrate}
                },
                {tag}));
        }
    }

    // End-of-print Kommentar
    commands.push_back(make_comment("End of print"));

    BOOST_LOG_TRIVIAL(info)
        << "MakerBotToolpath: " << commands.size()
        << " Befehle aus " << gcode_path;

    return commands.dump();
}


// ── make_birdwing_meta_json: unverändert (funktioniert korrekt) ──────────────
static std::string make_birdwing_meta_json_full(
    const std::string& bot_type,
    double             layer_height,
    double             layer_width,
    double             total_filament_mm,
    int                duration_s,
    const std::string& extruder_type,
    double             nozzle_diameter,
    double             feed_diameter,
    double             retract_distance,
    double             extruder_temp,
    double             travel_speed_xy,
    double             travel_speed_z,
    double             fill_speed,
    double             inner_speed,
    double             outer_speed,
    bool               do_raft,
    bool               do_fan,
    bool               do_exp_decel,
    double             retract_rate = 30.0,
    double             restart_rate = 18.0)
{
    // Extruder-Hardware-ID (bwcoreutils/tool_mappings.hh aus z18_6.json)
    // Bestätigt durch z18_6.json: mk12=7, mk13=8
    int extruder_id = 7; // mk12 default
    if      (extruder_type == "mk13")              extruder_id = 8;
    else if (extruder_type == "mk13_impla")        extruder_id = 14;
    else if (extruder_type == "mk13_experimental") extruder_id = 99;

    // Material-String: mk13_impla → "im-pla", alle anderen → "pla"
    const std::string material = (extruder_type == "mk13_impla") ? "im-pla" : "pla";

    // Filament-Masse: ρ_PLA=1.24 g/cm³, d=1.77mm (aus z18_6.json: feed_diameter=1.77)
    const double filament_mass_g =
        total_filament_mm * 3.14159265358979
        * (feed_diameter / 2.0) * (feed_diameter / 2.0)
        * 1.24e-3;

    const int ext_temp_int = static_cast<int>(extruder_temp);

    // commanded_duration ≈ 75% von total (Overhead durch Beschleunigung etc.)
    const double commanded_duration = duration_s * 0.75;

    // ── Extrusions-Profil (aus z18_6.json und legacy profile) ─────────────────
    // Werte direkt aus MakerBot-Originalquellen:
    // retract_distance: mk12=1.0mm, mk13=0.5mm (aus z18_6.json)
    // retract_rate:     50 mm/s
    // restart_rate:     30 mm/s
    nlohmann::json extrusion_profile = {
        {"feedDiameter",          feed_diameter > 0 ? feed_diameter : 1.77},
        {"nozzleDiameter",        nozzle_diameter > 0 ? nozzle_diameter : 0.4},
        {"defaultTemperature",    ext_temp_int},
        {"idleTemperature",       190},
        {"retractDistance",       retract_distance > 0 ? retract_distance : 0.8},
        {"retractRate",           retract_rate > 0 ? retract_rate : 30.0},
        {"restartRate",           restart_rate > 0 ? restart_rate : 30.0},
        {"restartExtraDistance",  0.1},
        {"oozeFeedstockDistance", 0.1},   // aus z18_6.json: ooze_feedstock_distance
        {"preOozeFeedstockDistance", 0.1},
        {"extrusionVolumeMultiplier", 1.0},
        {"toolchangeRestartDistance",  18.5},
        {"toolchangeRestartRate",       6.0},
        {"toolchangeRetractDistance",  19.0},
        {"toolchangeRetractRate",       6.0},
        // Geschwindigkeiten aus gantry_configuration in z18_6.json:
        // max_outer_shell_speed=40, max_inner_shell_speed=90, max_fill_speed=110
        {"extrusionProfiles", {
            {"outlines",               {{"feedrate", outer_speed > 0 ? outer_speed : 40.0}, {"fanSpeed", 0.95}}},
            {"insets",                 {{"feedrate", inner_speed > 0 ? inner_speed : 90.0}, {"fanSpeed", 0.95}}},
            {"infill",                 {{"feedrate", fill_speed  > 0 ? fill_speed  : 110.0},{"fanSpeed", 0.5}}},
            {"roofSurfaceFills",       {{"feedrate", fill_speed  > 0 ? fill_speed  : 110.0},{"fanSpeed", 0.5}}},
            {"floorSurfaceFills",      {{"feedrate", fill_speed  > 0 ? fill_speed  : 110.0},{"fanSpeed", 0.5}}},
            {"sparseRoofSurfaceFills", {{"feedrate", fill_speed  > 0 ? fill_speed  : 110.0},{"fanSpeed", 0.5}}},
            {"firstModelLayer",        {{"feedrate", 30.0},  {"fanSpeed", 1.0}}},
            {"raftBase",               {{"feedrate", 10.0},  {"fanSpeed", 0.5}}},
            {"raft",                   {{"feedrate", 90.0},  {"fanSpeed", 0.95}}},
            {"bridges",                {{"feedrate", 40.0},  {"fanSpeed", 0.95}}}
        }}
    };

    // ── miracle_config.gaggles.default ────────────────────────────────────────
    nlohmann::json mg = {
        {"layerHeight",        layer_height},
        {"numberOfShells",     2},
        {"infillDensity",      0.1},
        {"sparseInfillPattern","diamond (fast)"},
        {"floorThickness",     0.8},
        {"roofThickness",      0.8},
        {"floorSolidThickness",0.8},
        {"roofSolidThickness", 0.8},
        {"doRaft",    do_raft},
        {"doSupport", false},
        {"doBreakawaySupport", false},
        {"doFanCommand",        do_fan},
        {"doFanModulation",     do_fan},
        {"fanDefaultSpeed",     do_fan ? 0.95 : 0.0},
        {"fanLayer",            do_fan ? 1 : 0},
        {"fanModulationThreshold", 0.5},
        {"fanModulationWindow",    0.1},
        // Geschwindigkeiten aus z18_6.json/gantry_configuration
        {"travelSpeedXY", travel_speed_xy > 0 ? travel_speed_xy : 150.0},
        {"travelSpeedZ",  travel_speed_z  > 0 ? travel_speed_z  : 3.0},
        {"minSpeedMultiplier", 0.3},
        // Exponential Deceleration (Birdwing 5th Gen Feature)
        {"doExponentialDeceleration",       do_exp_decel},
        {"exponentialDecelerationRatio",    do_exp_decel ? 0.375 : 0.0},
        {"exponentialDecelerationSegmentCount", do_exp_decel ? 10 : 0},
        {"exponentialDecelerationMinSpeed", 0.0},
        // Rate Limiting (aus z18_6_mk13_pla_balanced profile)
        {"doRateLimit",               true},
        {"rateLimitBufferSize",       100},
        {"rateLimitMinSpeed",         10},
        {"rateLimitSpeedRatio",       0.3},
        {"rateLimitTransmissionRate", travel_speed_xy > 0 ? travel_speed_xy : 150.0},
        // Geometrie
        {"defaultExtruder",      0},
        {"defaultSupportMaterial", 0},
        {"adjacentFillLeakyConnections", true},
        {"adjacentFillLeakyDistanceRatio", 1.4},
        {"anchorExtrusionAmount", 5.0},
        {"anchorExtrusionSpeed",  2.0},
        {"anchorWidth",           2.0},
        {"doAnchor",              true},
        {"doBridging",            true},
        {"doExternalSpurs",       true},
        {"doFixedShellStart",     true},
        {"doNewPathPlanning",     true},
        {"doSplitLongMoves",      false},
        {"fixedShellStartDirection", 215},
        {"infillShellSpacingMultiplier", 0.55},
        {"insetDistanceMultiplier", 1.0},
        {"leakyConnectionsAdjacentDistance", 0.8},
        {"maxConnectionLength",  10.0},
        {"maxSparseFillThickness", layer_height},
        {"minLayerDuration",     5.0},
        {"minLayerHeight",       0.01},
        {"minSpurLength",        0.34},
        {"minSpurWidth",         0.12},
        {"shellsLeakyConnections", true},
        {"splitMinimumDistance", 0.4},
        // Raft (aus z18_6_mk13_pla_balanced_none_raft.json legacy profile)
        {"raftBaseLayers",    1},
        {"raftBaseThickness", 0.3},
        {"raftBaseWidth",     2.5},
        {"raftExtraOffset",   0.0},
        {"raftInterfaceLayers", 2},
        {"raftInterfaceThickness", 0.27},
        {"raftInterfaceWidth", 0.4},
        {"raftInterfaceZOffset", -0.14},
        {"raftModelSpacing",  0.26},
        {"raftSurfaceLayers", 2},
        {"raftSurfaceShells", 2},
        {"raftSurfaceThickness", 0.27},
        {"raftSurfaceZOffset", -0.03},
        // Support
        {"supportAngle",        68.0},
        {"supportExtraDistance", 0.5},
        {"supportLayerHeight",  layer_height},
        {"supportLeakyConnections", true},
        {"supportModelSpacing", 0.4},
        // Startposition (Z18: center-origin)
        {"startPosition", {{"x", 145.5}, {"y", 130.0}, {"z", layer_height}}},
        {"extruderProfiles", {extrusion_profile}}
    };

    // ── machine_config Extruder-Profil ────────────────────────────────────────
    // Exakt übernommen aus z18_6.json für mk13 (das übliche Modell)
    nlohmann::json ext_hw_profile = {
        {"nozzle_diameter", nozzle_diameter > 0 ? nozzle_diameter : 0.4},
        {"max_speed_mm_per_second", {{"a", 5.3}}},  // aus z18_6.json!
        {"steps_per_mm", {{"a", 108.55}}},           // aus z18_6.json!
        {"materials", {{material, {
            {"feed_diameter",          feed_diameter > 0 ? feed_diameter : 1.77},
            {"max_flow_rate",          5.0},          // aus z18_6.json!
            {"ooze_feedstock_distance",0.1},
            {"restart_rate",           restart_rate  > 0 ? restart_rate  : 30.0},
            {"retract_distance",       retract_distance > 0 ? retract_distance : 0.8},
            {"retract_rate",           retract_rate  > 0 ? retract_rate  : 30.0},
            {"temperature",            ext_temp_int},
            // slip_compensation_table ENTFERNT:
            // Orca's Kalibrierung (flow_ratio, Max Volumetric Speed) ersetzt sie.
            // Doppelkompensation durch Firmware + Slicer → Überextrusion vermeiden!
            {"acceleration", {
                {"impulse_speed_limit_mm_per_s", {{"a", 3.0}}},
                {"max_speed_change_mm_per_s",    {{"a", 0.5}}},
                {"min_speed_change_mm_per_s",    {{"a", 0.01}}},
                {"rate_mm_per_s_sq",             {{"a", 10.0}}}
            }}
        }}}}
    };

    // ── Vollständiges meta.json ───────────────────────────────────────────────
    // Pflichtfelder bestätigt durch Firmware-Analyse Z18 v2.6.3
    nlohmann::json meta = {
        {"version",       "1.1.0"},
        {"bot_type",      bot_type},
        {"toolpath_type", "jsontoolpath"},
        {"grue_version",  "5.4.0"},

        // Extruder-Identifikation (Firmware validiert!)
        {"tool_type",           extruder_type},
        {"tool_types",          {extruder_type}},
        {"_attached_extruders", {extruder_type}},
        {"_bot",                bot_type},
        {"_extruders",          {extruder_type}},

        // Material (auf Display angezeigt)
        {"material",   material},
        {"materials",  {material}},
        {"_materials", {material}},

        // Raft-Flag (auf Display angezeigt)
        {"uses_raft", do_raft},

        // Temperaturen (auf Display angezeigt)
        {"extruder_temperature",  ext_temp_int},
        {"extruder_temperatures", {ext_temp_int}},
        {"platform_temperature",  0},
        {"chamber_temperature",   nullptr},

        // Filament-Verbrauch (auf Display angezeigt)
        {"extrusion_distance_mm",  std::max(0.0, total_filament_mm)},
        {"extrusion_distances_mm", {std::max(0.0, total_filament_mm)}},
        {"extrusion_mass_g",       filament_mass_g},
        {"extrusion_masses_g",     {filament_mass_g}},

        // Druckzeit (auf Display angezeigt)
        {"duration_s",           duration_s},
        {"commanded_duration_s", commanded_duration},

        // Druckqualität
        {"preferences", {
            {"default", {
                {"print_mode", "balanced"},
                {"overrides", nlohmann::json::object()}
            }}
        }},

        // miracle_config (informativ, Firmware slicet NICHT neu)
        {"miracle_config", {
            {"_bot",       bot_type},
            {"_extruders", {extruder_type}},
            {"_materials", {material}},
            {"doRaft",     do_raft},
            {"version",    "5.4.0"},
            {"gaggles",    {{"default", mg}}}
        }},

        // machine_config (Extruder-IDs werden von Firmware validiert!)
        {"machine_config", {
            {"bot_type",  bot_type},
            {"version",   "1.1.0"},
            {"build_volume", {{"x", 300}, {"y", 305}, {"z", 457}}},
            {"makerbot_generation", 5},
            {"chamber_temperature_default", 0},
            {"extra_slicer_settings", {{"plate_variability", 0.6}}},
            // start_position aus z18_6.json
            {"start_position", {{"x", 145.5}, {"y", 130.0}, {"z", layer_height}}},
            {"max_speed_mm_per_second", {{"x", 175}, {"y", 175}, {"z", 3.0}}},
            // steps_per_mm aus z18_6.json (exakte Werte!)
            {"steps_per_mm", {
                {"x",  88.573186},
                {"y",  88.573186},
                {"z", -2666.666666}
            }},
            // acceleration aus z18_6.json
            {"acceleration", {
                {"buffer_size", 128},
                {"rate_mm_per_s_sq",          {{"x", 850}, {"y", 850}, {"z", 150}}},
                {"max_speed_change_mm_per_s", {{"x", 25},  {"y", 25},  {"z", 0}}},
                {"min_speed_change_mm_per_s", {{"x", 1},   {"y", 1},   {"z", 0}}},
                {"impulse_speed_limit_mm_per_s", {{"x", 70}, {"y", 70}, {"z", 0}}},
                {"split_move_distance_mm",    2.5},
                {"split_move_recursion_count", 36}
            }},
            // gantry_configuration aus z18_6.json
            {"gantry_configuration", {
                {"max_fill_speed",        fill_speed  > 0 ? fill_speed  : 110.0},
                {"max_inner_shell_speed", inner_speed > 0 ? inner_speed : 90.0},
                {"max_outer_shell_speed", outer_speed > 0 ? outer_speed : 40.0},
                {"travel_speed_xy",       travel_speed_xy > 0 ? travel_speed_xy : 150.0},
                {"travel_speed_z",        travel_speed_z  > 0 ? travel_speed_z  : 3.0}
            }},
            {"extruder_profiles", {
                // attached_extruders: calibrated=true, id=8 (mk13) aus z18_6.json
                {"attached_extruders", {
                    {{"calibrated", true}, {"id", extruder_id}}
                }},
                // supported_extruders exakt aus z18_6.json
                {"supported_extruders", {
                    {"0",  nullptr},
                    {"1",  "mk12"}, {"2",  "mk12"}, {"3",  "mk12"},
                    {"4",  "mk12"}, {"5",  "mk12"}, {"6",  "mk12"},
                    {"7",  "mk12"}, {"8",  "mk13"}, {"9",  "mk12"},
                    {"10", "mk12"}, {"11", "mk12"}, {"12", "mk12"},
                    {"13", "mk12"},
                    {"14", "mk13_impla"},
                    {"99", "mk13_experimental"}
                }},
                // Profil für den gewählten Extruder
                {extruder_type, ext_hw_profile}
            }}
        }}
    };

    return meta.dump(2);
}

// ── Öffentlicher Wrapper: Exakte alte 18-Parameter-Signatur ───────────────────
// MakerBotExport.cpp ruft diese Version auf (ohne retract_rate/restart_rate).
// Sensible Defaults: retract_rate=30mm/s, restart_rate=18mm/s.
// Sobald MakerBotExport.cpp aktualisiert wird, kann der Wrapper entfernt werden.
std::string make_birdwing_meta_json(
    const std::string& bot_type,
    double             layer_height,
    double             layer_width,
    double             total_filament_mm,
    int                duration_s,
    const std::string& extruder_type,
    double             nozzle_diameter,
    double             feed_diameter,
    double             retract_distance,
    double             extruder_temp,
    double             travel_speed_xy,
    double             travel_speed_z,
    double             fill_speed,
    double             inner_speed,
    double             outer_speed,
    bool               do_raft,
    bool               do_fan,
    bool               do_exp_decel,
    double             retract_rate,    // aus Orca retraction_speed  (default im .hpp: 30.0)
    double             restart_rate)    // aus Orca deretraction_speed (default im .hpp: 18.0)
{
    return make_birdwing_meta_json_full(
        bot_type, layer_height, layer_width, total_filament_mm, duration_s,
        extruder_type, nozzle_diameter, feed_diameter, retract_distance,
        extruder_temp, travel_speed_xy, travel_speed_z,
        fill_speed, inner_speed, outer_speed,
        do_raft, do_fan, do_exp_decel,
        retract_rate, restart_rate);
}



} // namespace Slic3r
