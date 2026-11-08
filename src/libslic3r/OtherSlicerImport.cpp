#include "OtherSlicerImport.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <memory>
#include <regex>
#include <sstream>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/cstdlib.hpp>
#include <boost/nowide/fstream.hpp>

#include "Config.hpp"
#include "LocalesUtils.hpp"
#include "PresetBundle.hpp"
#include "PrintConfig.hpp"
#include "Semver.hpp"

namespace Slic3r {
namespace OtherSlicers {

namespace fs = boost::filesystem;
using json   = nlohmann::json;

// Every slicer the importer knows, in the order they are looked for and listed. Adding one is a
// row here: its presets are read by the PrusaSlicer reader (.ini) or the OrcaSlicer reader (JSON
// under user/<account>/), whichever family it belongs to.
const std::vector<AppInfo> &apps()
{
    static const std::vector<AppInfo> table = {
        {App::PrusaSlicer, "PrusaSlicer", "prusaslicer", true, {"PrusaSlicer"}, "PrusaSlicer.ini", "com.prusa3d.PrusaSlicer",
         {"Prusa3D/PrusaSlicer/resources/profiles"},
         {"/Applications/PrusaSlicer.app/Contents/Resources/profiles",
          "/Applications/Original Prusa Drivers/PrusaSlicer.app/Contents/Resources/profiles"},
         {"/usr/share/PrusaSlicer/profiles"}},
        {App::OrcaSlicer, "OrcaSlicer", "orcaslicer", false, {"OrcaSlicer"}, "OrcaSlicer.conf", "io.github.softfever.OrcaSlicer",
         {"OrcaSlicer/resources/profiles"}, {"/Applications/OrcaSlicer.app/Contents/Resources/profiles"}, {}},
        {App::BambuStudio, "Bambu Studio", "bambustudio", false, {"BambuStudio"}, "BambuStudio.conf", "com.bambulab.BambuStudio",
         {"Bambu Studio/resources/profiles"}, {"/Applications/BambuStudio.app/Contents/Resources/profiles"}, {}},
        // Unlayered 2026-10-22: the big vendor forks. Folders, config names and install paths are
        // read from each project's source (SLIC3R_APP_KEY in version.inc -> wxStandardPaths user data
        // dir, "<key>.conf" / "<key>.ini"); see OtherSlicerImport tests for the layouts.
        //
        // Creality Print 6/7 nests its data under "Creality/Creality Print/<data version>" (6.0, 7.0,
        // 7.3 on master; " Alpha" for non-release builds). 7.3 copies 7.0 and 6.0 forward on first
        // launch and leaves them, so the newest folder is the live one. Creality Print 5.x is a
        // different, Qt-based program with its own preset format and is not read.
        {App::CrealityPrint, "Creality Print", "crealityprint", false,
         {"Creality/Creality Print/7.3", "Creality/Creality Print/7.0", "Creality/Creality Print/6.0",
          "Creality/Creality Print/7.3 Alpha", "Creality/Creality Print/7.0 Alpha", "Creality/Creality Print/6.0 Alpha"},
         "Creality.conf", "io.github.crealityofficial.CrealityPrint",
         {"Creality/Creality Print 7.3/resources/profiles", "Creality/Creality Print 7.2/resources/profiles",
          "Creality/Creality Print 7.1/resources/profiles", "Creality/Creality Print 7.0/resources/profiles",
          "Creality/Creality Print 6.0/resources/profiles", "Creality/Creality Print/resources/profiles"},
         {"/Applications/CrealityPrint.app/Contents/Resources/profiles"}, {}},
        // SuperSlicer: PrusaSlicer's layout. 2.7.62+ can keep each installation in its own subfolder
        // (installed.ini); only the classic flat folder is read.
        {App::SuperSlicer, "SuperSlicer", "superslicer", true, {"SuperSlicer", "SuperSlicer-alpha"}, "SuperSlicer.ini", "",
         {}, {"/Applications/SuperSlicer.app/Contents/Resources/profiles"}, {}},
        {App::ElegooSlicer, "ElegooSlicer", "elegooslicer", false, {"ElegooSlicer"}, "ElegooSlicer.conf", "",
         {"ElegooSlicer/resources/profiles"}, {"/Applications/ElegooSlicer.app/Contents/Resources/profiles"}, {}},
        {App::AnycubicSlicerNext, "Anycubic Slicer Next", "anycubicslicernext", false, {"AnycubicSlicerNext"},
         "AnycubicSlicerNext.conf", "", {"AnycubicSlicer/resources/profiles", "AnycubicSlicerNext/resources/profiles"},
         {"/Applications/AnycubicSlicerNext.app/Contents/Resources/profiles"}, {}},
        {App::SnapmakerOrca, "Snapmaker Orca", "snapmakerorca", false, {"Snapmaker_Orca"}, "Snapmaker_Orca.conf",
         "io.github.Snapmaker.Snapmaker_Orca", {"Snapmaker_Orca/resources/profiles"},
         {"/Applications/Snapmaker Orca.app/Contents/Resources/profiles"}, {}},
        {App::QIDIStudio, "QIDI Studio", "qidistudio", false, {"QIDIStudio"}, "QIDIStudio.conf", "",
         {"QIDIStudio/resources/profiles", "QIDI Studio/resources/profiles"},
         {"/Applications/QIDIStudio.app/Contents/Resources/profiles"}, {}},
        // Flashforge's Orca fork, renamed "Flash Studio"; its data folder kept the old key.
        {App::FlashStudio, "Flash Studio", "orcaflashforge", false, {"Orca-Flashforge"}, "Orca-Flashforge.conf", "",
         {"Orca-Flashforge/resources/profiles", "Flash Studio/resources/profiles"},
         {"/Applications/Flash Studio.app/Contents/Resources/profiles",
          "/Applications/Orca-Flashforge.app/Contents/Resources/profiles"}, {}},
        {App::PhrozenOrca, "PhrozenOrca", "phrozenorca", false, {"PhrozenOrca"}, "PhrozenOrca.conf", "",
         {"PhrozenOrca/resources/profiles"}, {"/Applications/PhrozenOrca.app/Contents/Resources/profiles"}, {}},
        {App::FlsunSlicer, "FlsunSlicer", "flsunslicer", true, {"FlsunSlicer"}, "FlsunSlicer.ini", "", {}, {}, {}},
    };
    return table;
}

const AppInfo &app_info(App app)
{
    for (const AppInfo &info : apps())
        if (info.app == app)
            return info;
    return apps().front();
}

std::string app_name(App app) { return app_info(app).name; }
std::string app_key(App app) { return app_info(app).key; }
bool        prusa_format(App app) { return app_info(app).prusa_format; }

std::string kind_dir(Kind kind)
{
    switch (kind) {
    case Kind::Printer: return "machine";
    case Kind::Filament: return "filament";
    case Kind::Process: return "process";
    }
    return {};
}

namespace {

constexpr Kind KINDS[] = {Kind::Printer, Kind::Filament, Kind::Process};

// PrusaSlicer's directory and .ini section name for a kind.
const char *prusa_dir(Kind kind) { return kind == Kind::Printer ? "printer" : kind == Kind::Filament ? "filament" : "print"; }

const char *settings_id_key(Kind kind)
{
    return kind == Kind::Printer ? "printer_settings_id" : kind == Kind::Filament ? "filament_settings_id" : "print_settings_id";
}

bool dir_exists(const fs::path &p)
{
    boost::system::error_code ec;
    return !p.empty() && fs::is_directory(p, ec);
}

std::vector<fs::path> list(const fs::path &dir, bool want_dirs, const char *ext = nullptr)
{
    std::vector<fs::path>     out;
    boost::system::error_code ec;
    if (!dir_exists(dir))
        return out;
    for (fs::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
        const fs::path &p = it->path();
        if (want_dirs ? fs::is_directory(p, ec) : (fs::is_regular_file(p, ec) && (ext == nullptr || boost::iequals(p.extension().string(), ext))))
            out.push_back(p);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string read_file(const fs::path &p)
{
    boost::nowide::ifstream ifs(p.string(), std::ios::binary);
    std::stringstream       ss;
    ss << ifs.rdbuf();
    return ss.str();
}

fs::path env_path(const char *name)
{
    const char *v = boost::nowide::getenv(name);
    return v != nullptr && *v != '\0' ? fs::path(v) : fs::path();
}

// Keys that say what a preset IS rather than how it prints; set afresh on import, never inherited.
bool is_meta_key(const std::string &key)
{
    static const std::set<std::string> meta = {"name", "inherits", "from", "instantiation", "setting_id", "type", "version",
                                               "is_custom_defined", "description", "url", "renamed_from", "alias", "base_id",
                                               "user_id", "updated_time", "sync_info", "include", "printer_settings_id",
                                               "print_settings_id", "filament_settings_id", "physical_printer_settings_id"};
    return meta.count(key) > 0;
}

// A login the person set up in the other app (API key, user, password): its presence is reported so
// they can enter it again, it is never copied. The address itself is copied (is_connection_address_key).
bool is_connection_secret_set(const std::string &key, const json &value)
{
    static const std::set<std::string> keys = {"printhost_apikey", "printhost_user", "printhost_password"};
    if (keys.count(key) == 0)
        return false;
    if (value.is_string())
        return !value.get<std::string>().empty();
    if (value.is_array())
        return std::any_of(value.begin(), value.end(), [](const json &v) { return v.is_string() && !v.get<std::string>().empty(); });
    return false;
}

// A preset's values as JSON (string, or an array of strings), for a key known to this fork:
// validated by deserializing them into the option the fork defines. `value` is in the
// serialization an .ini or a preset JSON string uses. Renames and value changes the fork already
// knows (PrintConfigDef::handle_legacy) apply first and may change `key`. Empty when the value
// does not parse or the fork has no such setting.
json to_json_value(std::string &key, std::string value)
{
    PrintConfigDef::handle_legacy(key, value);
    const ConfigOptionDef *def = key.empty() ? nullptr : print_config_def.get(key);
    if (def == nullptr)
        return {};
    std::unique_ptr<ConfigOption> opt(def->create_empty_option());
    if (opt == nullptr)
        return {};
    if (def->is_scalar()) {
        if (opt->deserialize(value))
            return value;
        // A per-extruder value from PrusaSlicer where this fork has one setting: the first one.
        std::vector<std::string> items;
        boost::split(items, value, boost::is_any_of(",;"));
        std::string first = boost::trim_copy(items.front());
        if (first.size() >= 2 && first.front() == '"' && first.back() == '"')
            first = first.substr(1, first.size() - 2);
        if (items.size() > 1 && opt->deserialize(first))
            return first;
        return {};
    }
    if (!opt->deserialize(value))
        return {};
    return static_cast<const ConfigOptionVectorBase *>(opt.get())->vserialize();
}

std::string format_number(double v)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.4g", v);
    return buf;
}

} // namespace

bool is_connection_address_key(const std::string &key)
{
    // Where the printer is and what kind of host it is (addresses are imported).
    static const std::set<std::string> keys = {"host_type", "print_host", "print_host_webui", "printhost_port",
                                               "printhost_authorization_type"};
    return keys.count(key) > 0;
}

bool is_private_key(const std::string &key)
{
    if (is_connection_address_key(key))
        return false;
    // Every other print host setting: API key, user, password, certificate file, and anything added
    // later. Never copied.
    if (boost::starts_with(key, "print_host") || boost::starts_with(key, "printhost_"))
        return true;
    static const std::set<std::string> keys = {"bbl_use_printhost", "bed_custom_texture", "bed_custom_model",
                                               "physical_printer_settings_id"};
    if (keys.count(key) > 0)
        return true;
    const std::string lower = boost::to_lower_copy(key);
    for (const char *word : {"password", "apikey", "api_key", "secret", "token"})
        if (lower.find(word) != std::string::npos)
            return true;
    return false;
}

std::map<std::string, std::map<std::string, std::string>> parse_ini(const std::string &text)
{
    std::map<std::string, std::map<std::string, std::string>> out;
    std::string                                              section;
    std::istringstream                                       in(text);
    std::string                                              line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const std::string t = boost::trim_copy(line);
        if (t.empty() || t.front() == '#' || t.front() == ';')
            continue;
        if (t.front() == '[' && t.back() == ']') {
            section = t.substr(1, t.size() - 2);
            continue;
        }
        const size_t eq = t.find('=');
        if (eq == std::string::npos)
            continue;
        out[section][boost::trim_copy(t.substr(0, eq))] = boost::trim_copy(t.substr(eq + 1));
    }
    return out;
}

// ------------------------------------------------------------------------------------------------
// PrusaSlicer -> this fork. The names below are the ones BambuStudio renamed and Orca kept; a key
// spelled the same in both is copied unchanged (and validated) without being listed here.
// ------------------------------------------------------------------------------------------------
namespace {

const std::map<std::string, std::string> &prusa_renames()
{
    static const std::map<std::string, std::string> m = {
        // print
        {"first_layer_height", "initial_layer_print_height"},
        {"perimeters", "wall_loops"},
        {"top_solid_layers", "top_shell_layers"},
        {"bottom_solid_layers", "bottom_shell_layers"},
        {"top_solid_min_thickness", "top_shell_thickness"},
        {"bottom_solid_min_thickness", "bottom_shell_thickness"},
        {"fill_density", "sparse_infill_density"},
        {"fill_pattern", "sparse_infill_pattern"},
        {"top_fill_pattern", "top_surface_pattern"},
        {"bottom_fill_pattern", "bottom_surface_pattern"},
        {"solid_fill_pattern", "internal_solid_infill_pattern"},
        {"fill_angle", "infill_direction"},
        {"solid_infill_below_area", "minimum_sparse_infill_area"},
        {"perimeter_speed", "inner_wall_speed"},
        {"external_perimeter_speed", "outer_wall_speed"},
        {"infill_speed", "sparse_infill_speed"},
        {"solid_infill_speed", "internal_solid_infill_speed"},
        {"top_solid_infill_speed", "top_surface_speed"},
        {"support_material_speed", "support_speed"},
        {"support_material_interface_speed", "support_interface_speed"},
        {"gap_fill_speed", "gap_infill_speed"},
        {"first_layer_speed", "initial_layer_speed"},
        {"first_layer_infill_speed", "initial_layer_infill_speed"},
        {"perimeter_acceleration", "inner_wall_acceleration"},
        {"external_perimeter_acceleration", "outer_wall_acceleration"},
        {"top_solid_infill_acceleration", "top_surface_acceleration"},
        {"solid_infill_acceleration", "internal_solid_infill_acceleration"},
        {"infill_acceleration", "sparse_infill_acceleration"},
        {"first_layer_acceleration", "initial_layer_acceleration"},
        {"brim_separation", "brim_object_gap"},
        {"skirts", "skirt_loops"},
        {"support_material", "enable_support"},
        {"support_material_threshold", "support_threshold_angle"},
        {"support_material_contact_distance", "support_top_z_distance"},
        {"support_material_bottom_contact_distance", "support_bottom_z_distance"},
        {"support_material_spacing", "support_base_pattern_spacing"},
        {"support_material_interface_layers", "support_interface_top_layers"},
        {"support_material_bottom_interface_layers", "support_interface_bottom_layers"},
        {"support_material_interface_spacing", "support_interface_spacing"},
        {"support_material_buildplate_only", "support_on_build_plate_only"},
        {"support_material_pattern", "support_base_pattern"},
        {"support_material_interface_pattern", "support_interface_pattern"},
        {"support_material_angle", "support_angle"},
        {"support_material_enforce_layers", "enforce_support_layers"},
        {"support_material_style", "support_style"},
        {"support_material_xy_spacing", "support_object_xy_distance"},
        {"dont_support_bridges", "bridge_no_support"},
        {"extrusion_width", "line_width"},
        {"external_perimeter_extrusion_width", "outer_wall_line_width"},
        {"perimeter_extrusion_width", "inner_wall_line_width"},
        {"infill_extrusion_width", "sparse_infill_line_width"},
        {"solid_infill_extrusion_width", "internal_solid_infill_line_width"},
        {"top_infill_extrusion_width", "top_surface_line_width"},
        {"first_layer_extrusion_width", "initial_layer_line_width"},
        {"support_material_extrusion_width", "support_line_width"},
        {"infill_overlap", "infill_wall_overlap"},
        {"bridge_flow_ratio", "bridge_flow"},
        {"overhangs", "detect_overhang_wall"},
        {"avoid_crossing_perimeters", "reduce_crossing_wall"},
        {"avoid_crossing_perimeters_max_detour", "max_travel_detour_distance"},
        {"only_retract_when_crossing_perimeters", "reduce_infill_retraction"},
        {"infill_first", "is_infill_first"},
        {"wipe_tower", "enable_prime_tower"},
        {"wipe_tower_width", "prime_tower_width"},
        {"wipe_tower_brim_width", "prime_tower_brim_width"},
        {"ironing_flowrate", "ironing_flow"},
        {"xy_size_compensation", "xy_contour_compensation"},
        {"spiral_vase", "spiral_mode"},
        {"fuzzy_skin_point_dist", "fuzzy_skin_point_distance"},
        {"output_filename_format", "filename_format"},
        {"perimeter_generator", "wall_generator"},
        {"gcode_resolution", "resolution"},
        {"thin_walls", "detect_thin_wall"},
        {"enable_dynamic_overhang_speeds", "enable_overhang_speed"},
        {"support_material_interface_contact_loops", "support_interface_loop_pattern"},
        {"support_tree_angle", "tree_support_branch_angle"},
        {"support_tree_angle_slow", "tree_support_angle_slow"},
        {"support_tree_tip_diameter", "tree_support_tip_diameter"},
        {"support_tree_branch_diameter", "tree_support_branch_diameter"},
        {"support_tree_branch_distance", "tree_support_branch_distance"},
        {"support_tree_branch_diameter_angle", "tree_support_branch_diameter_angle"},
        {"support_tree_top_rate", "tree_support_top_rate"},
        // filament
        {"extrusion_multiplier", "filament_flow_ratio"},
        {"temperature", "nozzle_temperature"},
        {"first_layer_temperature", "nozzle_temperature_initial_layer"},
        {"min_fan_speed", "fan_min_speed"},
        {"max_fan_speed", "fan_max_speed"},
        {"bridge_fan_speed", "overhang_fan_speed"},
        {"disable_fan_first_layers", "close_fan_the_first_x_layers"},
        {"fan_below_layer_time", "fan_cooling_layer_time"},
        {"slowdown_below_layer_time", "slow_down_layer_time"},
        {"min_print_speed", "slow_down_min_speed"},
        {"cooling", "slow_down_for_layer_cooling"},
        {"filament_retract_length", "filament_retraction_length"},
        {"filament_retract_lift", "filament_z_hop"},
        {"filament_retract_speed", "filament_retraction_speed"},
        {"filament_deretract_speed", "filament_deretraction_speed"},
        {"filament_retract_before_travel", "filament_retraction_minimum_travel"},
        {"filament_retract_layer_change", "filament_retract_when_changing_layer"},
        {"start_filament_gcode", "filament_start_gcode"},
        {"end_filament_gcode", "filament_end_gcode"},
        {"filament_shrinkage_compensation_xy", "filament_shrink"},
        // printer
        {"bed_shape", "printable_area"},
        {"max_print_height", "printable_height"},
        {"start_gcode", "machine_start_gcode"},
        {"end_gcode", "machine_end_gcode"},
        {"before_layer_gcode", "before_layer_change_gcode"},
        {"layer_gcode", "layer_change_gcode"},
        {"toolchange_gcode", "change_filament_gcode"},
        {"pause_print_gcode", "machine_pause_gcode"},
        {"between_objects_gcode", "printing_by_object_gcode"},
        {"retract_length", "retraction_length"},
        {"retract_lift", "z_hop"},
        {"retract_speed", "retraction_speed"},
        {"deretract_speed", "deretraction_speed"},
        {"retract_before_travel", "retraction_minimum_travel"},
        {"retract_layer_change", "retract_when_changing_layer"},
        {"machine_max_feedrate_x", "machine_max_speed_x"},
        {"machine_max_feedrate_y", "machine_max_speed_y"},
        {"machine_max_feedrate_z", "machine_max_speed_z"},
        {"machine_max_feedrate_e", "machine_max_speed_e"},
        {"extruder_clearance_height", "extruder_clearance_height_to_rod"},
    };
    return m;
}

// Same name, but not something to carry over: handled below, or meaningful only in PrusaSlicer.
bool prusa_key_handled_separately(const std::string &key)
{
    static const std::set<std::string> keys = {
        // resolved to absolute values or combined into other keys below
        "external_perimeter_speed", "small_perimeter_speed", "solid_infill_speed", "top_solid_infill_speed",
        "support_material_interface_speed", "first_layer_speed", "first_layer_infill_speed", "bed_temperature",
        "first_layer_bed_temperature", "ironing", "ironing_type", "support_material", "support_material_auto",
        "support_material_style", "external_perimeters_first", "complete_objects", "machine_limits_usage",
        "top_one_perimeter_type", "only_one_perimeter_top", "seam_position", "fill_pattern", "top_fill_pattern",
        "bottom_fill_pattern", "solid_fill_pattern", "filament_max_volumetric_speed", "support_material_xy_spacing",
        "overhang_speed_0", "overhang_speed_1", "overhang_speed_2", "overhang_speed_3", "gcode_label_objects", "arc_fitting",
        "infill_every_layers", "remaining_times",
        // names of PrusaSlicer's own presets; this fork does not have them
        "default_print_profile", "default_filament_profile", "default_sla_print_profile", "default_sla_material_profile",
    };
    return keys.count(key) > 0;
}

std::string prusa_infill_pattern(const std::string &v)
{
    if (v == "stars")
        return "tri-hexagon";
    if (v == "monotoniclines")
        return "monotonicline";
    return v;
}

} // namespace

json translate_prusa(const std::map<std::string, std::string> &ini, Kind kind, int *dropped)
{
    CNumericLocalesSetter locales_setter; // PrusaSlicer writes "0.2", whatever the UI language
    json out     = json::object();
    int  n_drops = 0;
    auto get     = [&ini](const std::string &k) -> const std::string * {
        auto it = ini.find(k);
        return it == ini.end() ? nullptr : &it->second;
    };
    auto put = [&out, &n_drops](const std::string &key, const std::string &value) {
        std::string k = key;
        json        v = to_json_value(k, value);
        if (v.is_null())
            ++n_drops;
        else
            out[k] = std::move(v);
    };

    // Speeds that PrusaSlicer may give as a percentage of another speed; this fork wants mm/s.
    // `base` is resolved first; a speed of 0 (PrusaSlicer: "automatic") is left to the default.
    std::map<std::string, double> resolved;
    std::function<bool(const std::string &, double &)> speed = [&](const std::string &key, double &mm_s) -> bool {
        if (auto it = resolved.find(key); it != resolved.end()) {
            mm_s = it->second;
            return true;
        }
        static const std::map<std::string, std::string> base_of = {
            {"external_perimeter_speed", "perimeter_speed"},       {"small_perimeter_speed", "perimeter_speed"},
            {"solid_infill_speed", "infill_speed"},                {"top_solid_infill_speed", "solid_infill_speed"},
            {"support_material_interface_speed", "support_material_speed"}};
        const std::string *v = get(key);
        if (v == nullptr || v->empty())
            return false;
        double value = 0;
        if (v->back() == '%') {
            auto   base_it = base_of.find(key);
            double base    = 0;
            if (base_it == base_of.end() || !speed(base_it->second, base))
                return false;
            try {
                value = std::stod(v->substr(0, v->size() - 1)) * base / 100.;
            } catch (...) {
                return false;
            }
        } else {
            try {
                value = std::stod(*v);
            } catch (...) {
                return false;
            }
        }
        if (!(value > 0))
            return false;
        resolved[key] = mm_s = value;
        return true;
    };
    for (const char *key : {"perimeter_speed", "external_perimeter_speed", "small_perimeter_speed", "infill_speed", "solid_infill_speed",
                            "top_solid_infill_speed", "support_material_speed", "support_material_interface_speed", "bridge_speed",
                            "gap_fill_speed", "travel_speed", "ironing_speed", "first_layer_speed", "first_layer_infill_speed"}) {
        if (get(key) == nullptr)
            continue;
        double mm_s = 0;
        auto   it   = prusa_renames().find(key);
        if (speed(key, mm_s))
            put(it == prusa_renames().end() ? key : it->second, format_number(mm_s));
        else
            ++n_drops;
    }

    for (const auto &[key, value] : ini) {
        if (is_meta_key(key) || is_private_key(key))
            continue;
        if (prusa_key_handled_separately(key))
            continue;
        if (resolved.count(key) || key == "perimeter_speed" || key == "infill_speed" || key == "support_material_speed" ||
            key == "bridge_speed" || key == "gap_fill_speed" || key == "travel_speed" || key == "ironing_speed")
            continue; // the speeds above
        auto it = prusa_renames().find(key);
        if (it != prusa_renames().end())
            put(it->second, value);
        else if (print_config_def.has(key))
            put(key, value);
        else
            ++n_drops; // a PrusaSlicer setting this fork has no equivalent for
    }

    // Values whose spelling differs.
    if (const std::string *v = get("seam_position"))
        put("seam_position", *v == "rear" ? std::string("back") : *v);
    for (const auto &[from, to] : {std::pair<const char *, const char *>{"fill_pattern", "sparse_infill_pattern"},
                                   {"top_fill_pattern", "top_surface_pattern"},
                                   {"bottom_fill_pattern", "bottom_surface_pattern"},
                                   {"solid_fill_pattern", "internal_solid_infill_pattern"}})
        if (const std::string *v = get(from))
            put(to, prusa_infill_pattern(*v));

    // PrusaSlicer: 0 = no limit. Here the limit is always applied, so leave the default.
    if (const std::string *v = get("filament_max_volumetric_speed")) {
        std::vector<std::string> items;
        boost::split(items, *v, boost::is_any_of(","));
        if (std::all_of(items.begin(), items.end(), [](const std::string &s) { return std::atof(s.c_str()) > 0; }))
            put("filament_max_volumetric_speed", *v);
    }

    // Distance to the object: absolute here; a percentage (of the line width) in PrusaSlicer is left
    // to the default.
    if (const std::string *v = get("support_material_xy_spacing"); v != nullptr && !v->empty() && v->back() != '%')
        put("support_object_xy_distance", *v);

    // One bed temperature in PrusaSlicer, one per plate type here: the same for every plate.
    if (kind == Kind::Filament) {
        for (const auto &[from, suffix] : {std::pair<const char *, const char *>{"bed_temperature", ""},
                                           {"first_layer_bed_temperature", "_initial_layer"}})
            if (const std::string *v = get(from))
                for (const char *plate : {"cool_plate_temp", "eng_plate_temp", "hot_plate_temp", "textured_plate_temp",
                                          "supertack_plate_temp", "textured_cool_plate_temp"})
                    if (print_config_def.has(std::string(plate) + suffix))
                        put(std::string(plate) + suffix, *v);
    }

    if (const std::string *v = get("ironing"))
        put("ironing_type", *v == "1" ? (get("ironing_type") ? *get("ironing_type") : std::string("top")) : std::string("no ironing"));

    if (const std::string *v = get("support_material")) {
        put("enable_support", *v);
        const std::string *style      = get("support_material_style");
        const bool         organic    = style != nullptr && *style == "organic";
        const std::string *automatic  = get("support_material_auto");
        put("support_type", std::string(organic ? "tree" : "normal") + (automatic == nullptr || *automatic == "1" ? "(auto)" : "(manual)"));
        if (style != nullptr)
            put("support_style", *style);
    }

    if (const std::string *v = get("external_perimeters_first"))
        put("wall_sequence", *v == "1" ? "outer wall/inner wall" : "inner wall/outer wall");
    if (const std::string *v = get("complete_objects"))
        put("print_sequence", *v == "1" ? "by object" : "by layer");
    if (const std::string *v = get("machine_limits_usage"))
        put("emit_machine_limits_to_gcode", *v == "emit_to_gcode" ? "1" : "0");
    if (const std::string *v = get("top_one_perimeter_type"))
        put("only_one_wall_top", *v == "none" ? "0" : "1");
    else if (const std::string *w = get("only_one_perimeter_top"))
        put("only_one_wall_top", *w);

    // Overhang speeds: PrusaSlicer numbers them by how much of the line is supported (0 = none), this
    // fork by how much overhangs (4/4 = all of it). A percentage has a different base: left out.
    for (const auto &[from, to] : {std::pair<const char *, const char *>{"overhang_speed_0", "overhang_4_4_speed"},
                                   {"overhang_speed_1", "overhang_3_4_speed"},
                                   {"overhang_speed_2", "overhang_2_4_speed"},
                                   {"overhang_speed_3", "overhang_1_4_speed"}})
        if (const std::string *v = get(from)) {
            if (!v->empty() && v->back() != '%')
                put(to, *v);
            else
                ++n_drops;
        }
    // Choices where this fork only has on / off.
    if (const std::string *v = get("gcode_label_objects"))
        put("gcode_label_objects", *v == "disabled" || *v == "0" ? "0" : "1");
    if (const std::string *v = get("arc_fitting"))
        put("enable_arc_fitting", *v == "disabled" ? "0" : "1");
    if (const std::string *v = get("infill_every_layers"))
        put("infill_combination", std::atoi(v->c_str()) > 1 ? "1" : "0");
    if (const std::string *v = get("remaining_times"))
        put("disable_m73", *v == "1" ? "0" : "1");

    if (dropped != nullptr)
        *dropped = n_drops;
    return out;
}

// ------------------------------------------------------------------------------------------------
// Where the apps keep their data.
// ------------------------------------------------------------------------------------------------

AppLocation location_at(App app, const fs::path &data_dir)
{
    AppLocation loc{app, data_dir, {}};
    if (prusa_format(app)) {
        loc.profile_roots.push_back(data_dir / "vendor");
        loc.profile_roots.push_back(data_dir / "cache");
    } else
        loc.profile_roots.push_back(data_dir / "system");
    return loc;
}

namespace {

std::vector<fs::path> data_dir_candidates(App app)
{
    std::vector<fs::path> out;
    const AppInfo        &info = app_info(app);
#if defined(_WIN32)
    if (fs::path appdata = env_path("APPDATA"); !appdata.empty())
        for (const std::string &folder : info.folders)
            out.push_back(appdata / fs::path(folder));
#elif defined(__APPLE__)
    if (fs::path home = env_path("HOME"); !home.empty())
        for (const std::string &folder : info.folders)
            out.push_back(home / "Library" / "Application Support" / fs::path(folder));
#else
    const fs::path home   = env_path("HOME");
    fs::path       config = env_path("XDG_CONFIG_HOME");
    if (config.empty() && !home.empty())
        config = home / ".config";
    for (const std::string &folder : info.folders) {
        if (!config.empty())
            out.push_back(config / fs::path(folder));
        if (!home.empty() && !info.flatpak.empty())
            out.push_back(home / ".var" / "app" / info.flatpak / "config" / fs::path(folder));
    }
#endif
    return out;
}

// The vendor profiles shipped with the program itself: the fallback for a parent the app has not
// copied into its data directory.
std::vector<fs::path> program_profile_roots(App app)
{
    std::vector<fs::path> out;
    const AppInfo        &info = app_info(app);
#if defined(_WIN32)
    for (const char *var : {"ProgramW6432", "ProgramFiles"}) {
        const fs::path pf = env_path(var);
        if (pf.empty())
            continue;
        for (const std::string &rel : info.windows_profiles)
            out.push_back(pf / fs::path(rel));
    }
#elif defined(__APPLE__)
    for (const std::string &dir : info.mac_profiles)
        out.emplace_back(dir);
#else
    for (const std::string &dir : info.linux_profiles)
        out.emplace_back(dir);
#endif
    out.erase(std::remove_if(out.begin(), out.end(), [](const fs::path &p) { return !dir_exists(p); }), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

bool same_dir(const fs::path &a, const fs::path &b)
{
    if (a.empty() || b.empty())
        return false;
    boost::system::error_code ec;
    return fs::equivalent(a, b, ec) && !ec;
}

} // namespace

std::vector<AppLocation> find(const fs::path &exclude)
{
    std::vector<AppLocation> out;
    for (const AppInfo &info : apps())
        for (const fs::path &dir : data_dir_candidates(info.app)) {
            if (!dir_exists(dir) || same_dir(dir, exclude))
                continue;
            AppLocation loc = location_at(info.app, dir);
            for (const fs::path &root : program_profile_roots(info.app))
                loc.profile_roots.push_back(root);
            out.push_back(std::move(loc));
            break;
        }
    return out;
}

// ------------------------------------------------------------------------------------------------
// Reading the user presets.
// ------------------------------------------------------------------------------------------------

Scan scan(const AppLocation &location)
{
    Scan                                           s;
    s.location = location;
    std::map<std::pair<Kind, std::string>, size_t> seen;
    std::vector<std::time_t>                       mtimes;
    auto add = [&](SourcePreset &&p) {
        boost::system::error_code ec;
        const std::time_t         mtime = fs::last_write_time(p.file, ec);
        auto                      it    = seen.find({p.kind, p.name});
        if (it == seen.end()) {
            seen.emplace(std::make_pair(p.kind, p.name), s.presets.size());
            s.presets.push_back(std::move(p));
            mtimes.push_back(mtime);
            return;
        }
        // The same name in two account folders: the newer one.
        ++s.not_importable["duplicates"];
        if (mtime > mtimes[it->second]) {
            s.presets[it->second] = std::move(p);
            mtimes[it->second]    = mtime;
        }
    };

    if (prusa_format(location.app)) {
        for (Kind kind : KINDS)
            for (const fs::path &file : list(location.data_dir / prusa_dir(kind), false, ".ini")) {
                try {
                    auto ini = parse_ini(read_file(file))[""];
                    if (kind == Kind::Printer && ini["printer_technology"] == "SLA") {
                        ++s.not_importable["SLA"];
                        continue;
                    }
                    SourcePreset p{location.app, kind, file.stem().string(), file, ini["inherits"]};
                    for (const auto &[k, v] : ini)
                        p.values[k] = v;
                    add(std::move(p));
                } catch (const std::exception &e) {
                    BOOST_LOG_TRIVIAL(warning) << "other slicers: cannot read " << file.string() << ": " << e.what();
                    ++s.not_importable["unreadable"];
                }
            }
        // Physical printers: a network connection linked to printer presets. The address is added to
        // the imported printer preset it names (plan()); the login never is.
        for (const fs::path &file : list(location.data_dir / "physical_printer", false, ".ini")) {
            try {
                auto ini = parse_ini(read_file(file))[""];
                if (ini["printer_technology"] == "SLA") {
                    ++s.not_importable["SLA"];
                    continue;
                }
                PhysicalPrinterSource pp;
                pp.name = file.stem().string();
                for (const auto &[k, v] : ini) {
                    if (is_connection_address_key(k) && !v.empty()) {
                        std::string key = k;
                        json        value = to_json_value(key, v); // validated: an unknown host type is left out
                        if (!value.is_null())
                            pp.address[key] = std::move(value);
                    } else if (is_connection_secret_set(k, json(v)))
                        pp.secret = true;
                }
                // "preset_names" (a list) since PrusaSlicer 2.3, "preset_name" before.
                std::vector<std::string> names;
                if (const std::string &list_value = ini["preset_names"]; !list_value.empty())
                    unescape_strings_cstyle(list_value, names);
                if (names.empty() && !ini["preset_name"].empty())
                    names.push_back(ini["preset_name"]);
                for (std::string &n : names) {
                    boost::trim(n);
                    if (!n.empty())
                        pp.presets.push_back(n);
                }
                if (pp.address.contains("print_host"))
                    s.physical.push_back(std::move(pp));
            } catch (const std::exception &e) {
                BOOST_LOG_TRIVIAL(warning) << "other slicers: cannot read " << file.string() << ": " << e.what();
                ++s.not_importable["unreadable"];
            }
        }
        for (const char *sla : {"sla_print", "sla_material"})
            s.not_importable["SLA"] += int(list(location.data_dir / sla, false, ".ini").size());
    } else {
        // user/<"default" or an account id>/{machine,filament,process}[/base]/*.json. "_local" and
        // other underscore folders hold bundles, not the person's own presets.
        for (const fs::path &user : list(location.data_dir / "user", true)) {
            if (boost::starts_with(user.filename().string(), "_"))
                continue;
            for (Kind kind : KINDS)
                for (const fs::path &dir : {user / kind_dir(kind), user / kind_dir(kind) / "base"})
                    for (const fs::path &file : list(dir, false, ".json")) {
                        try {
                            json j = json::parse(read_file(file));
                            if (!j.is_object())
                                throw std::runtime_error("not a preset");
                            std::string name = j.contains("name") && j["name"].is_string() ? j["name"].get<std::string>() : file.stem().string();
                            std::string inherits = j.contains("inherits") && j["inherits"].is_string() ? j["inherits"].get<std::string>() : std::string();
                            add(SourcePreset{location.app, kind, std::move(name), file, std::move(inherits), std::move(j)});
                        } catch (const std::exception &e) {
                            BOOST_LOG_TRIVIAL(warning) << "other slicers: cannot read " << file.string() << ": " << e.what();
                            ++s.not_importable["unreadable"];
                        }
                    }
        }
    }
    s.selection = read_system_selection(location);
    for (auto it = s.not_importable.begin(); it != s.not_importable.end();)
        it = it->second == 0 ? s.not_importable.erase(it) : std::next(it);
    return s;
}

// ------------------------------------------------------------------------------------------------
// What the person picked from the app's built-in lists.
// ------------------------------------------------------------------------------------------------

bool SystemSelection::has_vendor(const std::string &vendor) const
{
    auto it = models.find(vendor);
    if (it == models.end())
        return false;
    for (const auto &[model, nozzles] : it->second)
        if (!nozzles.empty())
            return true;
    return false;
}

std::string strip_checksum_line(const std::string &text)
{
    // AppConfig::save() writes the JSON, a newline, then "# MD5 checksum <32 hex digits>"; AppConfig::load()
    // cuts at the last '}' the same way.
    const size_t last = text.find_last_of('}');
    return last == std::string::npos ? text : text.substr(0, last + 1);
}

namespace {

void add_model(SystemSelection &sel, const std::string &vendor, const std::string &model, const std::string &escaped_nozzles)
{
    if (vendor.empty() || model.empty())
        return;
    std::vector<std::string> nozzles;
    if (!unescape_strings_cstyle(escaped_nozzles, nozzles))
        return;
    for (std::string &n : nozzles) {
        boost::trim(n);
        if (!n.empty())
            sel.models[vendor][model].insert(n);
    }
}

std::string json_string(const json &j, const char *key)
{
    return j.is_object() && j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string();
}

} // namespace

SystemSelection parse_app_config_json(const std::string &text)
{
    SystemSelection sel;
    const json      j = json::parse(strip_checksum_line(text));
    if (!j.is_object())
        return sel;
    // "models": [{"model": "Bambu Lab A1", "nozzle_diameter": "0.4;0.6", "vendor": "BBL"}, ...]
    if (j.contains("models") && j["models"].is_array())
        for (const json &m : j["models"])
            add_model(sel, json_string(m, "vendor"), json_string(m, "model"), json_string(m, "nozzle_diameter"));
    // "filaments": ["Bambu PLA Basic @BBL A1", ...] (an object of name -> "true" in older files)
    if (j.contains("filaments")) {
        const json &f = j["filaments"];
        if (f.is_array()) {
            for (const json &name : f)
                if (name.is_string() && !name.get<std::string>().empty())
                    sel.filaments.push_back(name.get<std::string>());
        } else if (f.is_object())
            for (auto it = f.begin(); it != f.end(); ++it)
                sel.filaments.push_back(it.key());
    }
    // "presets": {"machine": ..., "process": ..., "filaments": [...] or null}
    if (j.contains("presets") && j["presets"].is_object()) {
        const json &p = j["presets"];
        sel.machine   = json_string(p, "machine");
        sel.process   = json_string(p, "process");
        if (p.contains("filaments") && p["filaments"].is_array() && !p["filaments"].empty() && p["filaments"].front().is_string())
            sel.filament = p["filaments"].front().get<std::string>();
        else
            sel.filament = json_string(p, "filament");
    }
    return sel;
}

SystemSelection parse_app_config_ini(const std::string &text)
{
    SystemSelection sel;
    for (const auto &[section, values] : parse_ini(text)) {
        if (boost::starts_with(section, "vendor:")) {
            const std::string vendor = section.substr(7);
            for (const auto &[key, value] : values)
                if (boost::starts_with(key, "model:"))
                    add_model(sel, vendor, key.substr(6), value);
        } else if (section == "filaments") {
            for (const auto &[name, value] : values)
                if (value == "1" || value == "true")
                    sel.filaments.push_back(name);
        } else if (section == "presets") {
            auto get = [&values = values](const char *key) {
                auto it = values.find(key);
                return it == values.end() ? std::string() : it->second;
            };
            sel.machine  = get("printer");
            sel.process  = get("print");
            sel.filament = get("filament");
        }
    }
    return sel;
}

namespace {

// "001" < "002" as text, but a PrusaSlicer list counts 1, 2, ... 10: order by the number.
std::vector<std::string> in_key_order(const std::map<std::string, std::string> &numbered)
{
    std::vector<std::pair<long, std::string>> items;
    for (const auto &[key, path] : numbered) {
        if (path.empty())
            continue;
        char *end = nullptr;
        const long n = std::strtol(key.c_str(), &end, 10);
        if (end != key.c_str())
            items.emplace_back(n, path);
    }
    std::stable_sort(items.begin(), items.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    std::vector<std::string> out;
    for (auto &item : items)
        if (std::find(out.begin(), out.end(), item.second) == out.end())
            out.push_back(std::move(item.second));
    return out;
}

} // namespace

std::vector<std::string> parse_recent_projects_json(const std::string &text)
{
    const json j = json::parse(strip_checksum_line(text), nullptr, false);
    if (!j.is_object() || !j.contains("recent_projects") || !j["recent_projects"].is_object())
        return {};
    std::map<std::string, std::string> numbered;
    for (auto it = j["recent_projects"].begin(); it != j["recent_projects"].end(); ++it)
        if (it.value().is_string())
            numbered[it.key()] = it.value().get<std::string>();
    return in_key_order(numbered);
}

std::vector<std::string> parse_recent_projects_ini(const std::string &text)
{
    auto sections = parse_ini(text);
    auto it       = sections.find("recent_projects");
    return it == sections.end() ? std::vector<std::string>() : in_key_order(it->second);
}

std::vector<std::string> recent_projects(const AppLocation &location)
{
    const fs::path file = location.data_dir / app_info(location.app).conf;
    boost::system::error_code ec;
    if (!fs::is_regular_file(file, ec))
        return {};
    try {
        const std::string text = read_file(file);
        return prusa_format(location.app) ? parse_recent_projects_ini(text) : parse_recent_projects_json(text);
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(warning) << "other slicers: cannot read the recent projects in " << file.string() << ": " << e.what();
        return {};
    }
}

SystemSelection read_system_selection(const AppLocation &location)
{
    const fs::path file = location.data_dir / app_info(location.app).conf;
    boost::system::error_code ec;
    if (!fs::is_regular_file(file, ec))
        return {};
    try {
        const std::string text = read_file(file);
        return prusa_format(location.app) ? parse_app_config_ini(text) : parse_app_config_json(text);
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(warning) << "other slicers: cannot read the selections in " << file.string() << ": " << e.what();
        return {};
    }
}

VendorModels read_vendor_models(const fs::path &profiles_dir)
{
    VendorModels out;
    for (const fs::path &index : list(profiles_dir, false, ".json")) {
        const std::string vendor = index.stem().string();
        if (vendor == PresetBundle::ORCA_FILAMENT_LIBRARY)
            continue;
        try {
            const json j = json::parse(read_file(index));
            if (!j.contains("machine_model_list") || !j["machine_model_list"].is_array())
                continue;
            for (const json &e : j["machine_model_list"]) {
                const std::string name = json_string(e, "name"), sub_path = json_string(e, "sub_path");
                if (name.empty() || sub_path.empty())
                    continue;
                const fs::path model_file = profiles_dir / vendor / sub_path;
                boost::system::error_code ec;
                if (!fs::is_regular_file(model_file, ec))
                    continue;
                const json        m = json::parse(read_file(model_file));
                const std::string        nozzle_list = json_string(m, "nozzle_diameter");
                std::vector<std::string> nozzles;
                boost::split(nozzles, nozzle_list, boost::is_any_of(";"));
                for (std::string &n : nozzles) {
                    boost::trim(n);
                    if (!n.empty())
                        out[vendor][name].insert(n);
                }
            }
        } catch (const std::exception &e) {
            BOOST_LOG_TRIVIAL(warning) << "other slicers: cannot read the printer models in " << index.string() << ": " << e.what();
        }
    }
    return out;
}

std::map<std::string, std::string> read_vendor_filaments(const fs::path &profiles_dir)
{
    std::map<std::string, std::string> out;
    for (const fs::path &index : list(profiles_dir, false, ".json")) {
        const std::string vendor = index.stem().string();
        try {
            const json j = json::parse(read_file(index));
            if (!j.contains("filament_list") || !j["filament_list"].is_array())
                continue;
            for (const json &e : j["filament_list"])
                if (const std::string name = json_string(e, "name"); !name.empty())
                    out.emplace(name, vendor); // the first vendor (alphabetical) that has it
        } catch (const std::exception &e) {
            BOOST_LOG_TRIVIAL(warning) << "other slicers: cannot read the filaments in " << index.string() << ": " << e.what();
        }
    }
    return out;
}

// ------------------------------------------------------------------------------------------------
// History.
// ------------------------------------------------------------------------------------------------

namespace {
std::string history_key(App app, Kind kind, const std::string &name) { return app_key(app) + "|" + kind_dir(kind) + "|" + name; }
// The built-in selections: "system-machine" / "system-filament", so they never collide with a user
// preset of the same name.
std::string history_system_key(App app, Kind kind, const std::string &what)
{
    return app_key(app) + "|system-" + kind_dir(kind) + "|" + what;
}
} // namespace

void ImportHistory::load(const fs::path &file)
{
    m_entries.clear();
    boost::system::error_code ec;
    if (!fs::is_regular_file(file, ec))
        return;
    try {
        json j = json::parse(read_file(file));
        for (const json &e : j.value("imported", json::array()))
            if (e.is_object())
                m_entries[e.value("app", "") + "|" + e.value("kind", "") + "|" + e.value("source", "")] = e.value("target", "");
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(warning) << "other slicers: ignoring unreadable import history " << file.string() << ": " << e.what();
    }
}

bool ImportHistory::save(const fs::path &file) const
{
    json list = json::array();
    for (const auto &[key, target] : m_entries) {
        const size_t a = key.find('|'), b = key.find('|', a + 1);
        list.push_back({{"app", key.substr(0, a)}, {"kind", key.substr(a + 1, b - a - 1)}, {"source", key.substr(b + 1)}, {"target", target}});
    }
    try {
        const fs::path tmp = file.string() + ".tmp";
        {
            boost::nowide::ofstream out(tmp.string(), std::ios::binary | std::ios::trunc);
            out << json{{"version", 1}, {"imported", list}}.dump(1, '\t');
            if (!out)
                return false;
        }
        fs::rename(tmp, file);
        return true;
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "other slicers: cannot save import history " << file.string() << ": " << e.what();
        return false;
    }
}

std::string ImportHistory::imported_as(App app, Kind kind, const std::string &source_name) const
{
    auto it = m_entries.find(history_key(app, kind, source_name));
    return it == m_entries.end() ? std::string() : it->second;
}

void ImportHistory::add(App app, Kind kind, const std::string &source_name, const std::string &target_name)
{
    m_entries[history_key(app, kind, source_name)] = target_name;
}

bool ImportHistory::has_system(App app, Kind kind, const std::string &what) const
{
    return m_entries.count(history_system_key(app, kind, what)) > 0;
}

void ImportHistory::add_system(App app, Kind kind, const std::string &what)
{
    m_entries[history_system_key(app, kind, what)] = what;
}

// ------------------------------------------------------------------------------------------------
// Resolving a preset in the app it came from.
// ------------------------------------------------------------------------------------------------
namespace {

struct Node
{
    json                     values = json::object();
    std::vector<std::string> parents;
    bool                     system{false};
    bool                     instantiable{true};
};

struct Resolved
{
    json                     merged = json::object();
    bool                     complete{true};
    std::string              missing;
    std::vector<std::string> system_ancestors; // nearest first, instantiable only
};

class Resolver
{
public:
    explicit Resolver(const Scan &scan) : m_scan(scan)
    {
        for (const SourcePreset &p : scan.presets) {
            Node n;
            n.values = p.values;
            if (!p.inherits.empty())
                n.parents.push_back(p.inherits);
            m_user.emplace(std::make_pair(p.kind, p.name), std::move(n));
        }
    }

    Resolved resolve(const SourcePreset &p)
    {
        Resolved r;
        const Node *self = find(p.kind, p.name, /* prefer_system */ false);
        if (self != nullptr)
            resolve_into(p.kind, *self, r, 0);
        return r;
    }

private:
    void resolve_into(Kind kind, const Node &node, Resolved &r, int depth)
    {
        if (depth > 16) { // a loop
            r.complete = false;
            return;
        }
        for (const std::string &parent : node.parents) {
            const Node *p = find(kind, parent, true);
            if (p == nullptr) {
                if (r.complete) {
                    r.complete = false;
                    r.missing  = parent;
                }
                continue;
            }
            if (p->system && p->instantiable)
                r.system_ancestors.push_back(parent);
            resolve_into(kind, *p, r, depth + 1);
        }
        for (auto it = node.values.begin(); it != node.values.end(); ++it)
            if (!is_meta_key(it.key()))
                r.merged[it.key()] = it.value();
    }

    // A parent is looked up among the system presets first: a user preset never shadows one.
    const Node *find(Kind kind, const std::string &name, bool prefer_system)
    {
        if (prefer_system)
            if (const Node *n = find_system(kind, name))
                return n;
        auto it = m_user.find({kind, name});
        if (it != m_user.end())
            return &it->second;
        return prefer_system ? nullptr : find_system(kind, name);
    }

    const Node *find_system(Kind kind, const std::string &name)
    {
        auto key = std::make_pair(kind, name);
        auto it  = m_system.find(key);
        if (it != m_system.end())
            return it->second.get();
        std::unique_ptr<Node> node = prusa_format(m_scan.location.app) ? load_prusa(kind, name) : load_json(kind, name);
        return m_system.emplace(key, std::move(node)).first->second.get();
    }

    // PrusaSlicer: every vendor bundle (.ini) in the profile roots, first definition wins.
    std::unique_ptr<Node> load_prusa(Kind kind, const std::string &name)
    {
        if (!m_prusa_loaded) {
            m_prusa_loaded = true;
            for (const fs::path &root : m_scan.location.profile_roots)
                for (const fs::path &file : list(root, false, ".ini")) {
                    try {
                        for (auto &[section, values] : parse_ini(read_file(file))) {
                            const size_t colon = section.find(':');
                            if (colon == std::string::npos)
                                continue;
                            const std::string type = section.substr(0, colon);
                            Kind              k;
                            if (type == "printer")
                                k = Kind::Printer;
                            else if (type == "filament")
                                k = Kind::Filament;
                            else if (type == "print")
                                k = Kind::Process;
                            else
                                continue;
                            m_prusa.emplace(std::make_pair(k, section.substr(colon + 1)), std::move(values));
                        }
                    } catch (const std::exception &e) {
                        BOOST_LOG_TRIVIAL(warning) << "other slicers: cannot read " << file.string() << ": " << e.what();
                    }
                }
        }
        auto it = m_prusa.find({kind, name});
        if (it == m_prusa.end())
            return nullptr;
        auto n          = std::make_unique<Node>();
        n->system       = true;
        n->instantiable = !(boost::starts_with(name, "*") && boost::ends_with(name, "*"));
        for (const auto &[k, v] : it->second)
            if (k == "inherits") {
                std::vector<std::string> parents;
                boost::split(parents, v, boost::is_any_of(";"));
                for (std::string &parent : parents) {
                    boost::trim(parent);
                    if (!parent.empty())
                        n->parents.push_back(parent);
                }
            } else
                n->values[k] = v;
        return n;
    }

    // OrcaSlicer / Bambu Studio: <root>/<Vendor>.json lists every preset's file (sub_path); a vendor
    // without one is searched by file name.
    std::unique_ptr<Node> load_json(Kind kind, const std::string &name)
    {
        if (!m_json_indexed) {
            m_json_indexed = true;
            const char *list_key[] = {"machine_list", "filament_list", "process_list"};
            for (const fs::path &root : m_scan.location.profile_roots)
                for (const fs::path &index : list(root, false, ".json")) {
                    const fs::path vendor_dir = root / index.stem();
                    if (!dir_exists(vendor_dir))
                        continue;
                    try {
                        json j = json::parse(read_file(index));
                        for (Kind k : KINDS)
                            for (const json &e : j.value(list_key[int(k)], json::array()))
                                if (e.is_object() && e.contains("name") && e.contains("sub_path"))
                                    m_json_index.emplace(std::make_pair(k, e["name"].get<std::string>()),
                                                         vendor_dir / e["sub_path"].get<std::string>());
                    } catch (const std::exception &e) {
                        BOOST_LOG_TRIVIAL(warning) << "other slicers: cannot read " << index.string() << ": " << e.what();
                    }
                }
        }
        std::vector<fs::path> candidates;
        if (auto it = m_json_index.find({kind, name}); it != m_json_index.end())
            candidates.push_back(it->second);
        for (const fs::path &root : m_scan.location.profile_roots)
            for (const fs::path &vendor : list(root, true))
                candidates.push_back(vendor / kind_dir(kind) / (name + ".json"));
        for (const fs::path &file : candidates) {
            boost::system::error_code ec;
            if (!fs::is_regular_file(file, ec))
                continue;
            try {
                json j = json::parse(read_file(file));
                if (!j.is_object() || j.value("name", std::string()) != name)
                    continue;
                auto n          = std::make_unique<Node>();
                n->system       = true;
                n->instantiable = j.value("instantiation", std::string("true")) != "false";
                if (j.contains("inherits") && j["inherits"].is_string() && !j["inherits"].get<std::string>().empty())
                    n->parents.push_back(j["inherits"].get<std::string>());
                n->values = std::move(j);
                return n;
            } catch (const std::exception &e) {
                BOOST_LOG_TRIVIAL(warning) << "other slicers: cannot read " << file.string() << ": " << e.what();
            }
        }
        return nullptr;
    }

    const Scan                                                       &m_scan;
    std::map<std::pair<Kind, std::string>, Node>                      m_user;
    std::map<std::pair<Kind, std::string>, std::unique_ptr<Node>>     m_system;
    bool                                                              m_prusa_loaded{false};
    std::map<std::pair<Kind, std::string>, std::map<std::string, std::string>> m_prusa;
    bool                                                              m_json_indexed{false};
    std::map<std::pair<Kind, std::string>, fs::path>                  m_json_index;
};

// The preset's own settings (not the ones it inherits) are those of the existing user preset.
bool same_settings(const SourcePreset &p, const DynamicPrintConfig &existing)
{
    if (const ConfigOption *inherits = existing.option("inherits"); inherits != nullptr && inherits->serialize() != p.inherits)
        return false;
    int compared = 0;
    for (auto it = p.values.begin(); it != p.values.end(); ++it) {
        const std::string &key = it.key();
        if (is_meta_key(key) || is_private_key(key) || key == "compatible_printers" || key == "compatible_prints")
            continue;
        const ConfigOption *opt = existing.option(key);
        if (opt == nullptr)
            continue; // a setting this fork does not have
        std::vector<std::string> mine;
        if (it.value().is_string())
            mine.push_back(it.value().get<std::string>());
        else if (it.value().is_array())
            for (const json &v : it.value())
                mine.push_back(v.is_string() ? v.get<std::string>() : v.dump());
        else
            return false;
        const std::vector<std::string> theirs = opt->is_scalar() ? std::vector<std::string>{opt->serialize()} :
                                                                   static_cast<const ConfigOptionVectorBase *>(opt)->vserialize();
        if (opt->is_scalar() && mine.size() > 1)
            return false;
        if (mine != theirs)
            return false;
        ++compared;
    }
    return compared > 0;
}

std::string valid_version(const json &values)
{
    if (values.contains("version") && values["version"].is_string()) {
        const std::string v = values["version"].get<std::string>();
        if (Semver::parse(v))
            return v;
    }
    return "1.0.0.0";
}

} // namespace

namespace {

std::string squash(const std::string &s)
{
    std::string out;
    for (char c : s)
        if (std::isalnum(static_cast<unsigned char>(c)))
            out += char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// Unlayered's (vendor, model) for a model picked in the source app, or {"", ""} when it ships none.
// OrcaSlicer and Bambu Studio use the same vendor and model names as this fork. PrusaSlicer names
// its vendor "PrusaResearch" and its models by id ("MK4S", "COREONE"); this fork's Prusa vendor has
// "Prusa MK4S", "Prusa CORE One".
std::pair<std::string, std::string> match_model(App app, const std::string &vendor, const std::string &model, const VendorModels &shipped)
{
    if (auto v = shipped.find(vendor); v != shipped.end() && v->second.count(model) > 0)
        return {vendor, model};
    if (prusa_format(app)) {
        const std::string here = vendor == "PrusaResearch" ? std::string("Prusa") : vendor;
        if (auto v = shipped.find(here); v != shipped.end()) {
            const std::string id = squash(model);
            for (const auto &[name, nozzles] : v->second)
                if (squash(name) == id || squash(name) == squash(here) + id)
                    return {here, name};
        }
    }
    return {};
}

std::string model_key(const std::string &vendor, const std::string &model, const std::string &nozzle) { return vendor + ";" + model + ";" + nozzle; }

bool is_enabled(const VendorModels &enabled, const std::string &vendor, const std::string &model, const std::string &nozzle)
{
    auto v = enabled.find(vendor);
    if (v == enabled.end())
        return false;
    auto m = v->second.find(model);
    return m != v->second.end() && m->second.count(nozzle) > 0;
}

// The built-in selections of one app that Unlayered can bring over. `models_now` holds what is enabled
// here plus what earlier apps of the same plan already enable, and is updated; `filaments_now` the same
// for filaments. Skipped entries go to `skipped` when it is given.
void plan_system(const Scan &scan, const Target &target, const ImportHistory &history, VendorModels &models_now,
                 std::set<std::string> &filaments_now, std::vector<EnableModel> &models, std::vector<EnableFilament> &filaments,
                 std::vector<Skipped> *skipped)
{
    const App              app = scan.location.app;
    const SystemSelection &sel = scan.selection;
    // detail: the vendor, for a filament made for printers that are not selected.
    auto skip = [&](Kind kind, const std::string &name, const std::string &reason, const std::string &detail = std::string()) {
        if (skipped != nullptr)
            skipped->push_back({app, kind, name, reason, Skipped::Why::NotShipped, detail});
    };

    for (const auto &[vendor, by_model] : sel.models)
        for (const auto &[model, nozzles] : by_model) {
            if (nozzles.empty())
                continue;
            const auto [here_vendor, here_model] = match_model(app, vendor, model, target.shipped_models);
            if (here_model.empty()) {
                skip(Kind::Printer, model, "not available in Unlayered Slicer yet");
                continue;
            }
            const std::set<std::string> &shipped = target.shipped_models.at(here_vendor).at(here_model);
            for (const std::string &nozzle : nozzles) {
                if (shipped.count(nozzle) == 0) {
                    skip(Kind::Printer, here_model + " " + nozzle + " nozzle", "this nozzle is not available in Unlayered Slicer yet");
                    continue;
                }
                if (is_enabled(models_now, here_vendor, here_model, nozzle) ||
                    history.has_system(app, Kind::Printer, model_key(here_vendor, here_model, nozzle)))
                    continue; // already here, or brought over before (and maybe turned off since)
                models_now[here_vendor][here_model].insert(nozzle);
                models.push_back({app, here_vendor, here_model, nozzle});
            }
        }

    for (const std::string &name : sel.filaments) {
        if (filaments_now.count(name) > 0 || history.has_system(app, Kind::Filament, name) ||
            (target.filament_enabled && target.filament_enabled(name)))
            continue;
        const std::string vendor = target.filament_vendor ? target.filament_vendor(name) : std::string();
        if (vendor.empty()) {
            skip(Kind::Filament, name, "not available in Unlayered Slicer yet");
            continue;
        }
        // A vendor's own filaments only for that vendor's printers; the filament library for every printer.
        bool for_a_printer = vendor == PresetBundle::ORCA_FILAMENT_LIBRARY;
        if (auto v = models_now.find(vendor); v != models_now.end())
            for (const auto &[model, nozzles] : v->second)
                for_a_printer |= !nozzles.empty();
        if (!for_a_printer) {
            skip(Kind::Filament, name, "made for " + vendor + " printers, and none of them is selected", vendor);
            continue;
        }
        filaments_now.insert(name);
        filaments.push_back({app, name, vendor});
    }
}

} // namespace

Counts importable_counts(const Scan &scan, const Target &target, const ImportHistory &history)
{
    Counts c;
    for (const SourcePreset &p : scan.presets) {
        const std::string prev = history.imported_as(p.app, p.kind, p.name);
        if (!prev.empty() && target.name_taken && target.name_taken(p.kind, prev))
            continue;
        (p.kind == Kind::Printer ? c.printers : p.kind == Kind::Filament ? c.filaments : c.processes) += 1;
    }
    VendorModels                models_now = target.enabled_models;
    std::set<std::string>       filaments_now;
    std::vector<EnableModel>    models;
    std::vector<EnableFilament> filaments;
    plan_system(scan, target, history, models_now, filaments_now, models, filaments, nullptr);
    std::set<std::pair<std::string, std::string>> distinct;
    for (const EnableModel &m : models)
        distinct.insert({m.vendor, m.model});
    c.models           = int(distinct.size());
    c.system_filaments = int(filaments.size());
    return c;
}

Plan plan(const std::vector<Scan> &scans, const Target &target, const ImportHistory &history, bool with_system)
{
    Plan                                                            out;
    std::set<std::pair<Kind, std::string>>                          taken_now;
    std::map<std::tuple<App, Kind, std::string>, std::string>       renamed; // source -> name here
    std::vector<std::unique_ptr<Resolver>>                          resolvers(scans.size());
    auto taken = [&](Kind k, const std::string &n) { return taken_now.count({k, n}) > 0 || (target.name_taken && target.name_taken(k, n)); };
    auto has_system = [&](Kind k, const std::string &n) { return !n.empty() && target.has_system && target.has_system(k, n); };

    // A list of preset names (compatible_printers, compatible_prints) in the source app's names: the
    // names they have here, leaving out any this fork has not got. An empty list means "all".
    auto remap = [&](App app, Kind kind, json &value) {
        if (!value.is_array())
            return;
        json mapped = json::array();
        for (const json &v : value) {
            if (!v.is_string())
                continue;
            const std::string name = v.get<std::string>();
            if (auto it = renamed.find({app, kind, name}); it != renamed.end())
                mapped.push_back(it->second);
            else if (taken(kind, name))
                mapped.push_back(name);
        }
        value = std::move(mapped);
    };

    for (Kind kind : KINDS) // printers first, so filament and process compatibility can name them
        for (size_t i = 0; i < scans.size(); ++i) {
            const Scan &scan = scans[i];
            const App   app  = scan.location.app;
            for (const SourcePreset &p : scan.presets) {
                if (p.kind != kind)
                    continue;
                const std::string prev = history.imported_as(app, kind, p.name);
                if (!prev.empty() && taken(kind, prev)) {
                    renamed[{app, kind, p.name}] = prev;
                    out.skipped.push_back({app, kind, p.name, "imported before, as \"" + prev + "\"", Skipped::Why::AlreadyImported, prev});
                    continue;
                }

                if (!resolvers[i])
                    resolvers[i] = std::make_unique<Resolver>(scan);
                Resolved r = resolvers[i]->resolve(p);

                // The nearest ancestor this fork also has keeps the preset linked to that profile.
                std::string parent;
                for (const std::string &a : r.system_ancestors)
                    if (has_system(kind, a)) {
                        parent = a;
                        break;
                    }
                if (parent.empty() && !r.complete && has_system(kind, r.missing))
                    parent = r.missing;
                // A PrusaSlicer user preset holds every setting itself; its parent only adds the link.
                if (prusa_format(app))
                    r.complete = true;
                if (!r.complete && parent.empty()) {
                    out.skipped.push_back({app, kind, p.name,
                                           "based on \"" + r.missing + "\", which neither " + app_name(app) + " nor Unlayered Slicer has",
                                           Skipped::Why::MissingParent, r.missing});
                    continue;
                }

                std::string name = p.name;
                if (taken(kind, name)) {
                    const DynamicPrintConfig *existing = !prusa_format(app) && target.user_config ? target.user_config(kind, name) : nullptr;
                    if (existing != nullptr && same_settings(p, *existing)) {
                        renamed[{app, kind, p.name}] = name;
                        out.skipped.push_back({app, kind, p.name, "already in Unlayered Slicer", Skipped::Why::AlreadyPresent, name});
                        continue;
                    }
                    const std::string base = p.name + " (" + app_name(app) + ")";
                    name                   = base;
                    for (int n = 2; taken(kind, name) && n < 100; ++n)
                        name = p.name + " (" + app_name(app) + " " + std::to_string(n) + ")";
                    if (taken(kind, name)) {
                        out.skipped.push_back({app, kind, p.name, "no free name", Skipped::Why::Duplicate, {}});
                        continue;
                    }
                }
                taken_now.insert({kind, name});
                renamed[{app, kind, p.name}] = name;

                Planned item;
                item.source          = p;
                item.target_name     = name;
                item.target_inherits = parent;
                json values          = json::object();
                for (auto it = r.merged.begin(); it != r.merged.end(); ++it) {
                    if (is_private_key(it.key())) {
                        item.dropped_connection |= is_connection_secret_set(it.key(), it.value());
                        continue;
                    }
                    values[it.key()] = it.value();
                }
                if (prusa_format(app)) {
                    std::map<std::string, std::string> ini;
                    for (auto it = values.begin(); it != values.end(); ++it)
                        if (it.value().is_string())
                            ini[it.key()] = it.value().get<std::string>();
                    values = translate_prusa(ini, kind, &item.dropped_keys);
                }
                if (values.contains("compatible_printers"))
                    remap(app, Kind::Printer, values["compatible_printers"]);
                if (values.contains("compatible_prints"))
                    remap(app, Kind::Process, values["compatible_prints"]);
                // The default process and filament a printer selects: kept only where this fork has them.
                for (const auto &[key, k] : {std::pair<const char *, Kind>{"default_print_profile", Kind::Process},
                                             {"default_filament_profile", Kind::Filament}}) {
                    if (!values.contains(key))
                        continue;
                    json &v = values[key];
                    if (v.is_string())
                        v = json::array({v});
                    remap(app, k, v);
                    if (v.empty())
                        values.erase(key);
                    else if (print_config_def.get(key) != nullptr && print_config_def.get(key)->is_scalar())
                        v = v.front();
                }

                values["name"]     = name;
                values["inherits"] = parent;
                values["from"]     = "User";
                values["version"]  = valid_version(p.values);
                if (kind == Kind::Filament)
                    values[settings_id_key(kind)] = json::array({name});
                else
                    values[settings_id_key(kind)] = name;
                item.config = std::move(values);
                out.items.push_back(std::move(item));
            }

            // PrusaSlicer physical printers: the address goes into the imported printer preset it
            // names (the first one it names that is imported here). Its login never does.
            if (kind == Kind::Printer)
                for (const PhysicalPrinterSource &pp : scan.physical) {
                    Planned *target_item = nullptr;
                    for (const std::string &preset : pp.presets) {
                        for (Planned &item : out.items)
                            if (item.source.app == app && item.source.kind == Kind::Printer && item.source.name == preset) {
                                target_item = &item;
                                break;
                            }
                        if (target_item != nullptr)
                            break;
                    }
                    if (target_item == nullptr) {
                        // Its printer was imported before (or is here already): nothing new to say.
                        if (std::any_of(pp.presets.begin(), pp.presets.end(),
                                        [&](const std::string &preset) { return renamed.count({app, Kind::Printer, preset}) > 0; }))
                            continue;
                        out.skipped.push_back({app, Kind::Printer, pp.name,
                                               pp.presets.empty() ? std::string("a printer connection that names no printer profile") :
                                                                    "a printer connection for \"" + pp.presets.front() +
                                                                        "\", which is not one of your own printer profiles being imported",
                                               Skipped::Why::NoPrinter, pp.presets.empty() ? std::string() : pp.presets.front()});
                        continue;
                    }
                    if (!target_item->connection_from.empty())
                        continue; // one address per printer: the first physical printer naming it
                    for (auto it = pp.address.begin(); it != pp.address.end(); ++it)
                        target_item->config[it.key()] = it.value();
                    target_item->connection_from = pp.name;
                    target_item->dropped_connection |= pp.secret;
                }
        }

    if (!with_system)
        return out;

    VendorModels          models_now = target.enabled_models;
    std::set<std::string> filaments_now;
    for (const Scan &scan : scans)
        plan_system(scan, target, history, models_now, filaments_now, out.models, out.filaments, &out.skipped);

    // A fresh install: the first app (in the order given) whose current printer Unlayered has
    // (has_system: the lambda at the top of this function).
    if (target.fresh)
        for (const Scan &scan : scans) {
            const SystemSelection &sel = scan.selection;
            if (!has_system(Kind::Printer, sel.machine))
                continue;
            out.select = CurrentSelection{scan.location.app, sel.machine, has_system(Kind::Process, sel.process) ? sel.process : std::string(),
                                          has_system(Kind::Filament, sel.filament) ? sel.filament : std::string()};
            break;
        }
    return out;
}

std::vector<std::string> write(const Plan &plan, const fs::path &dir)
{
    std::vector<std::string> files;
    for (const Planned &item : plan.items) {
        std::string stem = item.target_name;
        for (char &c : stem)
            if (std::string("<>:\"/\\|?*").find(c) != std::string::npos || static_cast<unsigned char>(c) < 32)
                c = '_';
        const fs::path folder = dir / kind_dir(item.source.kind);
        fs::create_directories(folder);
        fs::path file = folder / (stem + ".json");
        for (int n = 2; fs::exists(file); ++n)
            file = folder / (stem + "_" + std::to_string(n) + ".json");
        boost::nowide::ofstream out(file.string(), std::ios::binary | std::ios::trunc);
        out << item.config.dump(4);
        if (out)
            files.push_back(file.string());
        else
            BOOST_LOG_TRIVIAL(error) << "other slicers: cannot write " << file.string();
    }
    return files;
}

} // namespace OtherSlicers
} // namespace Slic3r
