// MakerBot / UltiMaker Fork – Orca Slicer 2.4
// MakerBotExport.cpp – G-code → .makerbot / .ufp archive packer
//
// GOLD VERSION: Kombiniert Alpha (parse_header, build_birdwing_meta,
// BBox, extrusion_mass_g, extract_thumbnails, Lava-Support) mit
// Beta (pack_to_archive API, gcode_to_birdwing_jsontoolpath für
// korrektes Print-4.x-Format mit relative.a=true).
//
// Kernstrategie: Alle Geschwindigkeits- und Profil-Werte werden direkt
// aus dem G-code-Settings-Block gelesen (parse_header), nicht mehr über
// PrintConfig-Casts – das umgeht das DynamicPrintConfig/PrintConfig-Problem.

#include "MakerBotExport.hpp"
#include "MakerBotToolpath.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/miniz_extension.hpp"

#include <miniz.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

namespace Slic3r {
namespace MakerBotExport {

// ── Public: archive extension helper ────────────────────────────────────────

std::string get_archive_extension(GCodeFlavor flavor)
{
    switch (flavor) {
    case gcfMakerBotBirdwing:
    case gcfMakerBotLava:   return ".makerbot";
    case gcfMakerBotLegacy: return ".gcode";
    // gcfUltiGCode (.ufp) is handled entirely by UltimakerUFPExport - this
    // module is MakerBot-only. Never dispatched here in practice
    // (BackgroundSlicingProcess.cpp routes gcfUltiGCode straight to
    // UltimakerUFPExport::pack_to_archive).
    default:                return ".gcode";
    }
}

// ── Internal helpers ─────────────────────────────────────────────────────────

// All settings read from the G-code settings block in a single pass
struct HeaderData
{
    std::string filament_type                = "PLA";
    std::string tool_type                    = "mk13";
    int         first_layer_temp             = 215;
    int         temperature                  = 215;
    int         chamber_temp                 = 0;
    double      layer_height                 = 0.20;
    double      first_layer_height           = 0.20;
    int         wall_loops                   = 2;
    double      infill_density               = 0.15;
    double      filament_density             = 1.24;
    double      filament_diameter            = 1.75;
    double      nozzle_diameter              = 0.4;
    double      retraction_length            = 0.5;   // Z18 Orca: 0.5mm
    double      retraction_speed             = 50.0;  // mm/s (process)
    double      filament_retraction_speed    = 40.0;  // mm/s (filament limit)
    double      deretraction_speed           = 30.0;  // mm/s (restart)
    double      z_hop                        = 0.0;
    double      z_offset                     = 0.0;
    double      travel_speed                 = 110.0;
    double      outer_wall_speed             = 49.0;
    double      inner_wall_speed             = 82.0;
    double      sparse_infill_speed          = 82.0;
    double      internal_solid_infill_speed  = 82.0;
    double      top_surface_speed            = 50.0;
    double      bridge_speed                 = 50.0;
    double      line_width                   = 0.4;
    int         fan_max_speed                = 100;
    int         close_fan_first_layers       = 1;
    int         enable_support               = 0;
    int         raft_layers                  = 0;
    int         duration_s                   = 0;
    int         num_layers                   = 0;
    double      total_filament_mm            = 0.0;
    // Previously hardcoded directly in build_birdwing_meta() - now parsed from
    // the actual G-code settings block like everything else in this struct.
    // Defaults below are the SAME values that used to be hardcoded, so any
    // gcode that for some reason lacks one of these comments still gets a
    // sane fallback instead of 0.
    double      max_volumetric_speed         = 5.0;   // filament_max_volumetric_speed
    double      default_acceleration         = 500.0; // default_acceleration
    double      travel_acceleration          = 2000.0;// travel_acceleration
    double      retract_restart_extra        = 0.1;   // retract_restart_extra (-> ooze_feedstock_distance)
    int         bed_temperature              = 0;     // bed_temperature / platform_temperature
    double      travel_speed_z               = 3.0;   // travel_speed_z
};

struct BBox
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

    nlohmann::json to_json() const
    {
        auto fz = [](double v) { return std::isfinite(v) ? v : 0.0; };
        return {{"x_min", fz(min_x)}, {"x_max", fz(max_x)},
                {"y_min", fz(min_y)}, {"y_max", fz(max_y)},
                {"z_min", fz(min_z)}, {"z_max", fz(max_z)}};
    }
};

struct ThumbnailBlob { int width = 0, height = 0; std::string bytes; };

static double parse_double_safe(const std::string& s, double fb)
{
    try { size_t p; double v = std::stod(s, &p);
          return std::isfinite(v) ? v : fb; }
    catch (...) { return fb; }
}

static int parse_int_safe(const std::string& s, int fb)
{
    try { size_t p; return std::stoi(s, &p); }
    catch (...) { return fb; }
}

static int hms_to_seconds(const std::string& s)
{
    // Parse "3h 52m 45s" or "3h52m45s" or "13833" (pure seconds)
    const std::regex part_re(R"((\d+)\s*([dhms]))", std::regex::icase);
    int total = 0;
    bool found_unit = false;
    for (std::sregex_iterator it(s.begin(), s.end(), part_re), end; it != end; ++it) {
        found_unit = true;
        const int value = parse_int_safe((*it)[1].str(), 0);
        const char unit = static_cast<char>(std::tolower((*it)[2].str()[0]));
        if      (unit == 'd') total += value * 86400;
        else if (unit == 'h') total += value * 3600;
        else if (unit == 'm') total += value * 60;
        else if (unit == 's') total += value;
    }
    if (!found_unit) total = parse_int_safe(s, 0);
    return total;
}

// Single-pass G-code parser:
//   - Collects all settings from "; key = value" comments
//   - Accumulates positive E values for filament total (relative E mode)
//   - Detects tool_type from "; makerbot_tool_type = mk13" comments
static HeaderData parse_header(const std::string& gcode_path, const PrintConfig& config)
{
    HeaderData h;

    // Smart extruder type from PrintConfig if available
    {
        const auto* opt = config.option("smart_extruder_type");
        if (opt) {
            try {
                const auto* ss = dynamic_cast<const ConfigOptionStrings*>(opt);
                if (ss && !ss->values.empty() && !ss->values[0].empty()
                    && ss->values[0] != "none")
                    h.tool_type = ss->values[0];
            } catch (...) {}
        }
    }

    std::ifstream gf(gcode_path);
    if (!gf.is_open()) return h;

    std::string line;
    while (std::getline(gf, line)) {
        // ── Filament accumulation (G0/G1 with positive E) ───────────────
        if (line.size() >= 2 && line[0] == 'G' && (line[1] == '0' || line[1] == '1')) {
            const size_t e_pos = line.find('E');
            if (e_pos != std::string::npos) {
                const size_t semi = line.find(';');
                if (semi == std::string::npos || semi > e_pos) {
                    try {
                        const double e = std::stod(line.substr(e_pos + 1));
                        if (e > 0.0) h.total_filament_mm += e;
                    } catch (...) {}
                }
            }
            continue;
        }

        // ── Settings comments ────────────────────────────────────────────
        if (line.empty() || line[0] != ';') continue;

        // Skip thumbnail blocks
        if (line.find("thumbnail begin") != std::string::npos) {
            while (std::getline(gf, line) && line.find("thumbnail end") == std::string::npos) {}
            continue;
        }

        std::string comment = line.substr(1);
        boost::algorithm::trim(comment);

        // makerbot_tool_type = mk13
        {
            std::string cl = boost::algorithm::to_lower_copy(comment);
            if (cl.rfind("makerbot_tool_type", 0) == 0) {
                const size_t eq = comment.find('=');
                if (eq != std::string::npos) {
                    std::string tool = comment.substr(eq + 1);
                    boost::algorithm::trim(tool);
                    if (!tool.empty()) h.tool_type = tool;
                }
                continue;
            }
        }

        // key = value
        size_t sep = comment.find('=');
        if (sep == std::string::npos) sep = comment.find(':');
        if (sep == std::string::npos) continue;

        std::string key = comment.substr(0, sep);
        std::string val = comment.substr(sep + 1);

        // Drop unit suffix like "(mm)"
        const size_t paren = key.find('(');
        if (paren != std::string::npos) key = key.substr(0, paren);

        boost::algorithm::trim(key);
        boost::algorithm::trim(val);
        boost::algorithm::to_lower(key);

        // Map keys to HeaderData fields
        if      (key == "nozzle_temperature_initial_layer" || key == "first_layer_temperature")
                                                h.first_layer_temp             = parse_int_safe(val, h.first_layer_temp);
        else if (key == "nozzle_temperature" || key == "temperature")
                                                h.temperature                  = parse_int_safe(val, h.temperature);
        else if (key == "chamber_temperature") h.chamber_temp                  = parse_int_safe(val, h.chamber_temp);
        else if (key == "total layer number")  h.num_layers                    = parse_int_safe(val, h.num_layers);
        else if (key == "layer_height")        h.layer_height                  = parse_double_safe(val, h.layer_height);
        else if (key == "first_layer_height")  h.first_layer_height            = parse_double_safe(val, h.first_layer_height);
        else if (key == "line_width")          h.line_width                    = parse_double_safe(val, h.line_width);
        else if (key == "wall_loops")          h.wall_loops                    = parse_int_safe(val, h.wall_loops);
        else if (key == "sparse_infill_density") {
            boost::algorithm::erase_all(val, "%");
            h.infill_density = parse_double_safe(val, h.infill_density * 100.0) / 100.0;
        }
        else if (key == "filament_density")    h.filament_density              = parse_double_safe(val, h.filament_density);
        else if (key == "filament_diameter")   h.filament_diameter             = parse_double_safe(val, h.filament_diameter);
        else if (key == "nozzle_diameter")     h.nozzle_diameter               = parse_double_safe(val, h.nozzle_diameter);
        else if (key == "filament_type")       h.filament_type                 = boost::algorithm::to_upper_copy(val);
        else if (key == "z_offset")            h.z_offset                      = parse_double_safe(val, h.z_offset);
        else if (key == "retraction_length")   h.retraction_length             = parse_double_safe(val, h.retraction_length);
        else if (key == "retraction_speed")    h.retraction_speed              = parse_double_safe(val, h.retraction_speed);
        else if (key == "filament_retraction_speed") h.filament_retraction_speed = parse_double_safe(val, h.filament_retraction_speed);
        else if (key == "deretraction_speed")  h.deretraction_speed            = parse_double_safe(val, h.deretraction_speed);
        else if (key == "z_hop")               h.z_hop                         = parse_double_safe(val, h.z_hop);
        else if (key == "travel_speed")        h.travel_speed                  = parse_double_safe(val, h.travel_speed);
        else if (key == "outer_wall_speed")    h.outer_wall_speed              = parse_double_safe(val, h.outer_wall_speed);
        else if (key == "inner_wall_speed")    h.inner_wall_speed              = parse_double_safe(val, h.inner_wall_speed);
        else if (key == "sparse_infill_speed") h.sparse_infill_speed           = parse_double_safe(val, h.sparse_infill_speed);
        else if (key == "internal_solid_infill_speed") h.internal_solid_infill_speed = parse_double_safe(val, h.internal_solid_infill_speed);
        else if (key == "top_surface_speed")   h.top_surface_speed             = parse_double_safe(val, h.top_surface_speed);
        else if (key == "bridge_speed")        h.bridge_speed                  = parse_double_safe(val, h.bridge_speed);
        else if (key == "fan_max_speed")       h.fan_max_speed                 = parse_int_safe(val, h.fan_max_speed);
        else if (key == "close_fan_the_first_x_layers") h.close_fan_first_layers = parse_int_safe(val, h.close_fan_first_layers);
        else if (key == "enable_support")      h.enable_support                = parse_int_safe(val, h.enable_support);
        else if (key == "raft_layers")         h.raft_layers                   = parse_int_safe(val, h.raft_layers);
        else if (key == "filament_max_volumetric_speed") h.max_volumetric_speed = parse_double_safe(val, h.max_volumetric_speed);
        else if (key == "default_acceleration") h.default_acceleration         = parse_double_safe(val, h.default_acceleration);
        else if (key == "travel_acceleration")  h.travel_acceleration          = parse_double_safe(val, h.travel_acceleration);
        else if (key == "retract_restart_extra") h.retract_restart_extra       = parse_double_safe(val, h.retract_restart_extra);
        else if (key == "bed_temperature" || key == "hot_plate_temp" || key == "bed_temperature_initial_layer")
                                                h.bed_temperature               = parse_int_safe(val, h.bed_temperature);
        else if (key == "travel_speed_z")      h.travel_speed_z                = parse_double_safe(val, h.travel_speed_z);
        else if (key.find("estimated printing time") != std::string::npos)
                                               h.duration_s                    = std::max(h.duration_s, hms_to_seconds(val));
    }

    // Effective retract rate = min(process, filament_limit)
    // (filament_retraction_speed is the actual G-code F value / 60)
    h.filament_retraction_speed = std::min(h.retraction_speed, h.filament_retraction_speed);

    return h;
}

static double extrusion_mass_g(double extrusion_mm, double filament_diameter_mm, double density_g_cm3)
{
    const double r = (filament_diameter_mm / 2.0) / 10.0; // cm
    const double l = extrusion_mm / 10.0;                  // cm
    return l * 3.14159265358979323846 * r * r * density_g_cm3;
}

static nlohmann::json build_birdwing_meta(
    const PrintConfig&  config,
    const std::string&  bot_type,
    const HeaderData&   h,
    const BBox&         bbox,
    double              total_extrusion,
    int                 command_count,
    const std::string&  project_name)
{
    const std::string mat_up  = h.filament_type.empty() ? "PLA" : boost::algorithm::to_upper_copy(h.filament_type);
    const std::string mat_lo  = boost::algorithm::to_lower_copy(mat_up);
    const std::string tool    = h.tool_type.empty() || h.tool_type == "none" ? "mk13" : h.tool_type;
    const double      mass_g  = extrusion_mass_g(total_extrusion, h.filament_diameter, h.filament_density);
    // Effective retract rate: min(process speed, filament limit)
    const double ret_rate  = std::max(1.0, h.filament_retraction_speed);
    const double rest_rate = std::max(1.0, h.deretraction_speed);

    nlohmann::json meta;
    meta["bot_type"]                 = bot_type.empty() ? "z18_6" : bot_type;
    meta["bounding_box"]             = bbox.to_json();
    meta["chamber_temperature"]      = h.chamber_temp;
    meta["commanded_duration_s"]     = h.duration_s;
    meta["duration_s"]               = h.duration_s;
    meta["extruder_temperature"]     = h.first_layer_temp;
    meta["extruder_temperatures"]    = nlohmann::json::array({h.first_layer_temp, h.temperature});
    meta["extrusion_distance_mm"]    = total_extrusion;
    meta["extrusion_distances_mm"]   = nlohmann::json::array({total_extrusion});
    meta["extrusion_mass_g"]         = mass_g;
    meta["extrusion_masses_g"]       = nlohmann::json::array({mass_g});
    meta["extrusion_mass_a_grams"]   = mass_g;
    meta["extrusion_distance_a_mm"]  = total_extrusion;
    meta["material"]                 = mat_up;
    meta["materials"]                = nlohmann::json::array({mat_up});
    meta["model_counts"]             = nlohmann::json::array({nlohmann::json{{"count",1},{"name","instance0"}}});
    meta["name"]                     = project_name;
    meta["num_tool_changes"]         = 0; // TODO: not yet tracked - needs actual T0/T1 tool-change counting from the gcode (single-extruder prints are correctly 0; multi-material prints will under-report)
    meta["num_z_layers"]             = h.num_layers;
    meta["num_z_transitions"]        = h.num_layers > 0 ? h.num_layers + 1 : 0;
    meta["platform_temperature"]     = h.bed_temperature; // was: hardcoded 0
    meta["tool_type"]                = tool;
    meta["tool_types"]               = nlohmann::json::array({tool});
    meta["total_commands"]           = command_count;
    meta["version"]                  = "1.2.0";
    meta["uses_raft"]                = h.raft_layers > 0;

    // printer_settings – human-readable summary
    {
        nlohmann::json ps;
        ps["layer_height"]         = h.layer_height;
        ps["infill"]               = h.infill_density;
        ps["shells"]               = h.wall_loops;
        ps["support"]              = h.enable_support > 0;
        ps["raft"]                 = h.raft_layers > 0;
        ps["materials"]            = nlohmann::json::array({mat_up});
        ps["extruder_temperatures"]= nlohmann::json::array({h.first_layer_temp, h.temperature});
        ps["first_layer_height"]   = h.first_layer_height;
        ps["chamber_temperature"]  = h.chamber_temp;
        ps["slicer"]               = "OrcaSlicer MakerBot native export";
        meta["printer_settings"]   = ps;
    }

    // extruder_profiles (for firmware: per-material speeds and hw params)
    {
        // hw limits based on actual profile speeds (no artificial caps)
        nlohmann::json ext_hw;
        // BUG FIX (2026-06-19): these four were hardcoded literals, completely
        // ignoring the user's actual filament/process settings - exactly the
        // "static defaults instead of live Orca values" problem reported.
        // All four now come from HeaderData, parsed from the G-code's own
        // settings block (the same mechanism every other field in this
        // function already uses) instead of being baked in here.
        ext_hw["feed_diameter"]     = h.filament_diameter;       // was: hardcoded 1.77
        ext_hw["nozzle_diameter"]   = h.nozzle_diameter;
        ext_hw["max_flow_rate"]     = h.max_volumetric_speed;    // was: hardcoded 5.0
        ext_hw["ooze_feedstock_distance"] = h.retract_restart_extra; // was: hardcoded 0.1
        ext_hw["retract_distance"]  = h.retraction_length;
        ext_hw["retract_rate"]      = ret_rate;
        ext_hw["restart_rate"]      = rest_rate;
        ext_hw["temperature"]       = h.temperature;
        // No slip_compensation_table – Orca calibration handles this
        ext_hw["acceleration"]      = nlohmann::json{
            {"default", nlohmann::json{
                {"normal_move",    h.default_acceleration},  // was: hardcoded 500.0
                {"during_retract", h.travel_acceleration},   // was: hardcoded 2000.0
                {"after_retract",  h.travel_acceleration}    // was: hardcoded 2000.0
            }}
        };

        nlohmann::json ext_materials;
        ext_materials[mat_lo] = ext_hw;

        nlohmann::json ep;
        ep["materials"] = ext_materials;

        nlohmann::json machine_config;
        machine_config["extruder_profiles"] = nlohmann::json{{tool, ep}};
        machine_config["gantry_configuration"] = nlohmann::json{
            {"max_outer_shell_speed", h.outer_wall_speed},
            {"max_inner_shell_speed", h.inner_wall_speed},
            {"max_fill_speed",        h.sparse_infill_speed},
            {"travel_speed_xy",       h.travel_speed},
            {"travel_speed_z",        h.travel_speed_z}, // was: hardcoded 3.0
            {"max_speed_mm_per_second", nlohmann::json{{"x",175.0},{"y",175.0},{"z",h.travel_speed_z}}}
        };
        meta["machine_config"] = machine_config;
    }

    // miracle_config – Birdwing slicer profile
    {
        nlohmann::json extrusion_profiles;
        extrusion_profiles["outlines"]      = {{"feedrate", h.outer_wall_speed}};
        extrusion_profiles["insets"]        = {{"feedrate", h.inner_wall_speed}};
        extrusion_profiles["solid"]         = {{"feedrate", h.internal_solid_infill_speed}};
        extrusion_profiles["sparse"]        = {{"feedrate", h.sparse_infill_speed}};
        extrusion_profiles["floor_surface"] = {{"feedrate", h.top_surface_speed}};
        extrusion_profiles["roof_surface"]  = {{"feedrate", h.top_surface_speed}};
        extrusion_profiles["bridges"]       = {{"feedrate", h.bridge_speed}};

        nlohmann::json ext_profile;
        ext_profile["feedDiameter"]      = h.filament_diameter; // was: hardcoded 1.77
        ext_profile["nozzleDiameter"]    = h.nozzle_diameter;
        ext_profile["temperature"]       = h.temperature;
        ext_profile["retractDistance"]   = h.retraction_length;
        ext_profile["retractRate"]       = ret_rate;
        ext_profile["restartRate"]       = rest_rate;
        ext_profile["zHopDistance"]      = h.z_hop;
        ext_profile["extrusionProfiles"] = extrusion_profiles;

        nlohmann::json gaggle;
        gaggle["_baseLayer"]             = h.raft_layers > 0 ? "raft" : "model";
        gaggle["_printMode"]             = "balanced";
        gaggle["_supportType"]           = "modelBreakaway";
        gaggle["baseLayerHeight"]        = h.first_layer_height;
        gaggle["bedZOffset"]             = 0.0; // NOTE: distinct from h.z_offset (nozzle/filament Z calibration) - bed-leveling Z offset isn't currently exposed in Orca's config model, left as firmware default rather than guessing a wrong mapping
        gaggle["chamberTemp"]            = h.chamber_temp;
        gaggle["doFanCommand"]           = true;
        gaggle["doExponentialDeceleration"] = true;
        gaggle["doRaft"]                 = h.raft_layers > 0;
        gaggle["doSupport"]              = h.enable_support > 0;
        gaggle["fanLayer"]               = h.close_fan_first_layers;
        gaggle["fanSpeed"]               = std::max(0.0, std::min(1.0, h.fan_max_speed / 100.0));
        gaggle["layerHeight"]            = h.layer_height;
        gaggle["machineBounds"]          = nlohmann::json::array({150.0, 152.5, -150.0, -152.5});
        gaggle["platformTemp"]           = h.bed_temperature; // was: hardcoded 0
        gaggle["travelSpeedXY"]          = h.travel_speed;
        gaggle["travelSpeedZ"]           = h.travel_speed_z; // was: hardcoded 3
        gaggle["numberOfShells"]         = h.wall_loops;
        gaggle["infillDensity"]          = h.infill_density;
        gaggle["extruderProfiles"]       = nlohmann::json::array({ext_profile});

        nlohmann::json mc;
        mc["_bot"]       = meta["bot_type"];
        mc["_extruders"] = nlohmann::json::array({tool});
        mc["_materials"] = nlohmann::json::array({mat_lo});
        mc["configPath"] = "/tmp/profile.json";
        mc["doRaft"]     = h.raft_layers > 0;
        mc["doSupport"]  = h.enable_support > 0;
        mc["layerHeight"]= h.layer_height;
        mc["numberOfShells"]  = h.wall_loops;
        mc["infillDensity"]   = h.infill_density;
        mc["gaggles"]["default"] = gaggle;
        meta["miracle_config"] = mc;
    }

    return meta;
}

static nlohmann::json build_lava_meta(
    const PrintConfig&  config,
    const std::string&  bot_type,
    const HeaderData&   h,
    double              total_extrusion,
    const std::string&  project_name)
{
    const std::string mat_lo  = boost::algorithm::to_lower_copy(h.filament_type.empty() ? std::string("pla") : h.filament_type);
    const double      mass_g  = extrusion_mass_g(total_extrusion, h.filament_diameter, h.filament_density);

    // Determine tool types for Method/Sketch (mk14 / mk14_s)
    std::vector<std::string> tools;
    {
        const auto* opt = config.option("smart_extruder_type");
        if (opt) {
            try {
                const auto* ss = dynamic_cast<const ConfigOptionStrings*>(opt);
                if (ss) for (const auto& v : ss->values)
                    if (!v.empty() && v != "none") tools.push_back(v);
            } catch (...) {}
        }
    }
    if (tools.empty()) tools = {"mk14"};

    std::string lava_bot = bot_type;
    if (lava_bot.empty()) {
        const auto* opt = config.option("printer_model");
        if (opt) try { lava_bot = dynamic_cast<const ConfigOptionString*>(opt)->value; } catch (...) {}
        if (lava_bot.empty()) lava_bot = "method";
    }

    nlohmann::json meta;
    meta["bot_type"]                = lava_bot;
    meta["commanded_duration_s"]    = h.duration_s;
    meta["duration_s"]              = h.duration_s;
    meta["extruder_temperature"]    = h.first_layer_temp;
    meta["extruder_temperatures"]   = nlohmann::json::array({h.first_layer_temp, h.temperature});
    meta["extrusion_distance_mm"]   = total_extrusion;
    meta["extrusion_distances_mm"]  = nlohmann::json::array({total_extrusion});
    meta["extrusion_mass_g"]        = mass_g;
    meta["extrusion_masses_g"]      = nlohmann::json::array({mass_g});
    meta["material"]                = mat_lo;
    meta["materials"]               = nlohmann::json::array({mat_lo});
    meta["model_counts"]            = nlohmann::json::array({nlohmann::json{{"count",1},{"name","instance0"}}});
    meta["name"]                    = project_name;
    meta["platform_temperature"]    = h.bed_temperature; // was: hardcoded 0
    meta["build_plane_temperature"] = h.chamber_temp;
    meta["tool_type"]               = tools.front();
    meta["tool_types"]              = tools;
    meta["version"]                 = "3.0.0";
    meta["preferences"]["instance0"]["printMode"] = "balanced";
    meta["preferences"]["instance0"]["machineBounds"] = nullptr;
    meta["miracle_config"]["_bot"] = lava_bot;
    meta["miracle_config"]["_extruders"] = tools;
    meta["miracle_config"]["_materials"] = nlohmann::json::array({mat_lo});
    meta["miracle_config"]["gaggles"]["instance0"] = nlohmann::json::object();
    return meta;
}

// Base64 decoder for thumbnail extraction
static std::string base64_decode(const std::string& input)
{
    static constexpr unsigned char kDec[256] = {
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,62,64,64,64,63,52,53,54,55,56,57,58,59,60,61,64,64,64,65,64,64,
        64,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,64,64,64,64,64,
        64,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64
    };
    std::string out;
    int val = 0, valb = -8;
    for (unsigned char c : input) {
        if (std::isspace(c)) continue;
        if (c == '=') break;
        const unsigned char d = kDec[c];
        if (d >= 64) continue;
        val = (val << 6) + d;
        valb += 6;
        if (valb >= 0) {
            out.push_back(char((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

static std::vector<ThumbnailBlob> extract_thumbnails(const std::string& gcode_path)
{
    std::vector<ThumbnailBlob> out;
    std::ifstream gf(gcode_path);
    if (!gf.is_open()) return out;

    const std::regex begin_re(R"(^;\s*thumbnail\s+begin\s+(\d+)x(\d+)\s+\d+)", std::regex::icase);
    const std::regex end_re  (R"(^;\s*thumbnail\s+end)", std::regex::icase);

    bool collecting = false;
    int w = 0, h = 0;
    std::string b64;
    std::string line;

    while (std::getline(gf, line)) {
        std::smatch m;
        if (!collecting && std::regex_search(line, m, begin_re)) {
            collecting = true;
            w = parse_int_safe(m[1].str(), 0);
            h = parse_int_safe(m[2].str(), 0);
            b64.clear();
            continue;
        }
        if (collecting && std::regex_search(line, end_re)) {
            const std::string bytes = base64_decode(b64);
            if (!bytes.empty()) out.push_back({w, h, bytes});
            collecting = false;
            continue;
        }
        if (collecting) {
            std::string s = line;
            boost::algorithm::trim(s);
            if (!s.empty() && s[0] == ';') s.erase(s.begin());
            boost::algorithm::trim(s);
            b64 += s;
        }
    }
    return out;
}

static const ThumbnailBlob* choose_thumbnail(const std::vector<ThumbnailBlob>& thumbs, int w, int h)
{
    const ThumbnailBlob* best = nullptr;
    for (const auto& t : thumbs) {
        if (t.width == w && t.height == h) return &t;
        if (!best || (t.width * t.height) > (best->width * best->height)) best = &t;
    }
    return best;
}

static void add_thumbnail_entries(
    std::vector<std::pair<std::string, std::string>>& entries,
    const std::vector<ThumbnailBlob>& thumbs,
    const std::vector<std::pair<std::string, std::pair<int,int>>>& targets)
{
    for (const auto& tgt : targets)
        if (const ThumbnailBlob* t = choose_thumbnail(thumbs, tgt.second.first, tgt.second.second))
            entries.emplace_back(tgt.first, t->bytes);
}

static bool write_zip(const std::string& out_path,
                      const std::vector<std::pair<std::string, std::string>>& entries)
{
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_writer_init_file(&zip, out_path.c_str(), 0)) return false;
    for (const auto& e : entries)
        if (!mz_zip_writer_add_mem(&zip, e.first.c_str(), e.second.data(), e.second.size(), MZ_DEFAULT_COMPRESSION))
        { mz_zip_writer_end(&zip); return false; }
    const bool ok = mz_zip_writer_finalize_archive(&zip) != 0;
    mz_zip_writer_end(&zip);
    return ok;
}

// ── pack_makerbot_birdwing (Birdwing / 5th Gen) ─────────────────────────────

static bool pack_makerbot_birdwing(const std::string& gcode_path,
                                    const std::string& archive_path,
                                    const PrintConfig& config,
                                    const std::string& project_name)
{
    // 1. Parse ALL settings from G-code header + count filament in one pass
    const HeaderData header = parse_header(gcode_path, config);
    const std::vector<ThumbnailBlob> thumbnails = extract_thumbnails(gcode_path);

    // 2. Convert G-code to Birdwing JSON toolpath (Print 4.x format, relative.a=true)
    BirdwingBuildVolume bv;
    {
        const auto* pa_opt = config.option("printable_area");
        if (pa_opt) {
            try {
                const auto* pts = dynamic_cast<const ConfigOptionPoints*>(pa_opt);
                if (pts && pts->values.size() >= 2) {
                    const auto& vv = pts->values;
                    double xmin=1e9, xmax=-1e9, ymin=1e9, ymax=-1e9;
                    for (const auto& p : vv) {
                        xmin = std::min(xmin, p.x()); xmax = std::max(xmax, p.x());
                        ymin = std::min(ymin, p.y()); ymax = std::max(ymax, p.y());
                    }
                    const double W = (xmax - xmin) / 1000.0;
                    const double H = (ymax - ymin) / 1000.0;
                    if (W > 10.0 && H > 10.0) { bv.x = W; bv.y = H; }
                }
            } catch (...) {}
        }
    }
    bv.layer_width = header.line_width > 0.01 ? header.line_width : 0.4;

    std::string tp_error;
    const std::string toolpath_json = gcode_to_birdwing_jsontoolpath(
        gcode_path, bv, header.layer_height, tp_error);

    if (toolpath_json.empty()) {
        BOOST_LOG_TRIVIAL(error) << "MakerBotExport: toolpath conversion failed: " << tp_error;
        return false;
    }

    // 3. Count commands for meta.json
    int command_count = 0;
    {
        // Quick count: every occurrence of "\"command\"" = one command
        const std::string needle = "\"command\"";
        size_t pos = 0;
        while ((pos = toolpath_json.find(needle, pos)) != std::string::npos) {
            ++command_count; pos += needle.size();
        }
    }

    // 4. Determine bot_type from config
    std::string bot_type = "z18_6";
    {
        const auto* bt = config.option("makerbot_bot_type");
        if (bt) try { bot_type = dynamic_cast<const ConfigOptionString*>(bt)->value; } catch (...) {}
    }

    // 5. Build meta.json using header (all values from G-code settings block)
    BBox bbox; // Birdwing's meta needs bounding box – use build volume as proxy
    bbox.update(-bv.x/2, -bv.y/2, 0.0);
    bbox.update( bv.x/2,  bv.y/2, header.layer_height * header.num_layers);

    const nlohmann::json meta = build_birdwing_meta(
        config, bot_type, header, bbox,
        header.total_filament_mm, command_count, project_name);

    // 6. Pack ZIP archive
    std::vector<std::pair<std::string, std::string>> entries;
    entries.emplace_back("meta.json",          meta.dump(4));
    entries.emplace_back("print.jsontoolpath", toolpath_json);
    add_thumbnail_entries(entries, thumbnails, {
        {"thumbnail_320x200.png",          {320,  200}},
        {"thumbnail_110x80.png",           {110,   80}},
        {"thumbnail_55x40.png",            { 55,   40}},
        {"isometric_thumbnail_640x640.png",{640,  640}},
        {"isometric_thumbnail_320x320.png",{320,  320}},
        {"isometric_thumbnail_120x120.png",{120,  120}}
    });

    if (!write_zip(archive_path, entries)) {
        BOOST_LOG_TRIVIAL(error) << "MakerBotExport: failed to write archive: " << archive_path;
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << "MakerBotExport: Birdwing archive created: " << archive_path
        << " [" << header.duration_s << "s, "
        << header.total_filament_mm << "mm, "
        << header.outer_wall_speed << "mm/s outer, "
        << header.retraction_length << "mm retract]";
    return true;
}

// ── pack_makerbot_lava (Method / Sketch = Lava format) ───────────────────────

static bool pack_makerbot_lava(const std::string& gcode_path,
                                const std::string& archive_path,
                                const PrintConfig& config,
                                const std::string& project_name)
{
    std::string gcode;
    {
        std::ifstream f(gcode_path, std::ios::binary);
        if (!f.is_open()) return false;
        gcode.assign(std::istreambuf_iterator<char>(f), {});
    }

    const HeaderData header = parse_header(gcode_path, config);
    const std::vector<ThumbnailBlob> thumbnails = extract_thumbnails(gcode_path);

    std::string bot_type;
    {
        const auto* bt = config.option("makerbot_bot_type");
        if (bt) try { bot_type = dynamic_cast<const ConfigOptionString*>(bt)->value; } catch (...) {}
    }

    const nlohmann::json meta = build_lava_meta(
        config, bot_type, header, header.total_filament_mm, project_name);

    nlohmann::json slicemeta;
    slicemeta["generator"] = "OrcaSlicer MakerBot Lava native export";

    std::vector<std::pair<std::string, std::string>> entries;
    entries.emplace_back("meta.json",          meta.dump(4));
    entries.emplace_back("print.gcode",        gcode);
    entries.emplace_back("slicemetadata.json", slicemeta.dump(4));
    add_thumbnail_entries(entries, thumbnails, {
        {"thumbnail_140x106.png",          {140,  106}},
        {"thumbnail_212x300.png",          {212,  300}},
        {"thumbnail_960x1460.png",         {960, 1460}},
        {"thumbnail_90x90.png",            { 90,   90}},
        {"isometric_thumbnail_120x120.png",{120,  120}},
        {"isometric_thumbnail_320x320.png",{320,  320}},
        {"isometric_thumbnail_640x640.png",{640,  640}}
    });

    if (!write_zip(archive_path, entries)) {
        BOOST_LOG_TRIVIAL(error) << "MakerBotExport: failed to write Lava archive: " << archive_path;
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << "MakerBotExport: Lava archive created: " << archive_path;
    return true;
}

// ── Public entry point ───────────────────────────────────────────────────────

std::string pack_to_archive(const std::string& gcode_path, const PrintConfig& config)
{
    namespace fs = boost::filesystem;

    const GCodeFlavor flavor = config.gcode_flavor;

    // Determine archive output path
    const std::string ext = get_archive_extension(flavor);
    if (ext == ".gcode")
        return {}; // MakerBotLegacy: leave as plain G-code

    const fs::path gcode_p(gcode_path);
    const std::string archive_path = (gcode_p.parent_path() / (gcode_p.stem().string() + ext)).string();

    // BUG FIX (2026-06-19): when the export dialog (Plater.cpp) already names
    // the target with the final archive extension (.makerbot), gcode_path and
    // the freshly computed archive_path collapse onto the SAME file (stem()
    // strips exactly one extension, so "Cube.makerbot" -> stem "Cube" ->
    // archive_path "Cube.makerbot" again). The packer then overwrites that
    // path with the real ZIP archive, and the unconditional cleanup further
    // down used to call fs::remove(gcode_path) on that identical path right
    // afterwards - silently destroying the just-written archive (the
    // try/catch swallows everything, so nothing shows up in the log;
    // confirmed by reproducing with debug logging: "Birdwing archive
    // created" is logged, then the file is gone, with zero trace of the
    // removal itself). Mirrors the fix already applied in
    // GPXExport::pack_to_archive for the x3g/Legacy pipeline.
    //
    // project_name is captured from archive_path (the real, final, visible
    // filename) BEFORE any staging happens, rather than re-derived from
    // gcode_path inside each packer. That decouples the on-printer/on-screen
    // project title from the staging implementation entirely.
    const std::string project_name = fs::path(archive_path).stem().string();

    std::string gcode_source = gcode_path;
    bool used_temp_source = false;
    if (archive_path == gcode_path) {
        gcode_source = gcode_path + ".tmp_gcode_for_pack";
        try {
            fs::rename(gcode_path, gcode_source);
            used_temp_source = true;
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(warning) << "MakerBotExport: could not stage temp G-code for "
                << gcode_path << ": " << e.what();
            return {};
        }
    }

    bool ok = false;
    if (flavor == gcfMakerBotLava) {
        ok = pack_makerbot_lava(gcode_source, archive_path, config, project_name);
    } else {
        // gcfMakerBotBirdwing (and anything else → try Birdwing)
        ok = pack_makerbot_birdwing(gcode_source, archive_path, config, project_name);
    }

    if (!ok) {
        BOOST_LOG_TRIVIAL(error) << "MakerBotExport: packing failed for " << gcode_source;
        if (used_temp_source) {
            try { fs::rename(gcode_source, gcode_path); } catch (...) {}
        }
        return {};
    }

    // Remove the source .gcode file (user got the archive). gcode_source is
    // now GUARANTEED to differ from archive_path - either it was already a
    // distinct path, or it is our temp staging file - so this can never
    // remove the archive we just wrote.
    try { fs::remove(gcode_source); } catch (...) {}

    BOOST_LOG_TRIVIAL(info) << "MakerBotExport: archive at " << archive_path;
    return archive_path;
}

} // namespace MakerBotExport
} // namespace Slic3r
