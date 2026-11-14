#include "WonderMakerPrinterAgent.hpp"

#include "MoonrakerPrinterAgent.hpp"
#include "IPrinterAgent.hpp"
#include "nlohmann/json.hpp"

#include <boost/algorithm/string/trim.hpp>
#include <boost/log/trivial.hpp>

#include <cstddef>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

constexpr const char* WONDERMAKER_AGENT_VERSION = "0.0.1";

namespace {
// Orca: the ZR touchscreen's colour picker in grid order; tmt1.ini stores indices into it, not
// colours. 0-15 are sampled from the screen, 16-18 are metallic mid-tones, and rainbow/clear have
// no colour.
constexpr const char* TMT_PALETTE[] = {
    /*  0 white     */ "#FFFFFF", /*  1 tan      */ "#E7CEB5", /*  2 brown   */ "#714623",
    /*  3 grey      */ "#97999B", /*  4 black    */ "#0C0809", /*  5 mint    */ "#49C5B1",
    /*  6 blue      */ "#009CDE", /*  7 navy     */ "#03257E", /*  8 green   */ "#00B140",
    /*  9 dk green  */ "#265B31", /* 10 yellow   */ "#FCE300", /* 11 orange  */ "#FE5000",
    /* 12 pink      */ "#F5B6CD", /* 13 magenta  */ "#E10098", /* 14 red     */ "#CD001A",
    /* 15 purple    */ "#5F249F", /* 16 gold     */ "#E1CD88", /* 17 bronze  */ "#D3B7A7",
    /* 18 silver    */ "#949393", /* 19 rainbow  */ nullptr,   /* 20 clear   */ nullptr,
};

// The screen's material list, in its own order.
constexpr const char* TMT_MATERIALS[] = {
    "PLA", "PLA Silk", "ABS", "PETG", "PLA Matte", "PLA Metal", "ABS Matte", "PETG Matte",
    "PVA", "TPE", "Marble", "HIPS", "TPU", "PET", "Wood", "ASA",
};

// Full-line "key=value" entries only; the firmware doesn't strip inline comments either, so a
// value carrying one is skipped as malformed.
std::optional<int> tmt_ini_int(const std::string& ini, const std::string& key)
{
    std::istringstream stream(ini);
    for (std::string line; std::getline(stream, line);) {
        boost::trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[')
            continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string name = line.substr(0, eq);
        boost::trim(name);
        if (name != key)
            continue;
        std::string value = line.substr(eq + 1);
        boost::trim(value);
        try {
            size_t    consumed = 0;
            const int parsed   = std::stoi(value, &consumed);
            if (consumed == value.size())
                return parsed;
        } catch (...) {}
        return std::nullopt;
    }
    return std::nullopt;
}
} // namespace

bool WonderMakerPrinterAgent::fetch_tmt_filament_info(std::vector<AmsTrayData>& trays, int& max_lane_index)
{
    // Absent on anything but a stock-firmware ZR.
    const std::string ini_url = join_url(device_info.base_url, "/server/files/config/tmt1.ini");
    std::string       ini;
    std::string       error;
    if (!fetch_body(ini_url, device_info.api_key, ini, error)) {
        BOOST_LOG_TRIVIAL(debug) << "WonderMakerPrinterAgent fetch: " << ini_url << " -> " << error;
        return false;
    }

    // last_thr_number is the tool count; without it the file is not understood.
    const std::optional<int> tool_count = tmt_ini_int(ini, "last_thr_number");
    if (!tool_count.has_value() || *tool_count <= 0 || *tool_count > 64) {
        BOOST_LOG_TRIVIAL(debug) << "WonderMakerPrinterAgent::fetch_tmt_filament_info: no usable last_thr_number";
        return false;
    }

    // filament_detected, not `enabled`: only the active tool's sensor is enabled.
    std::string sensors_url = join_url(device_info.base_url, "/printer/objects/query?");
    for (int i = 0; i < *tool_count; ++i)
        sensors_url += (i ? "&" : "") + std::string("filament_switch_sensor%20filament") + std::to_string(i);
    nlohmann::json sensors = nlohmann::json::object();
    nlohmann::json json;
    if (!get_json(sensors_url, device_info.api_key, {}, json, error))
        BOOST_LOG_TRIVIAL(debug) << "WonderMakerPrinterAgent fetch: " << sensors_url << " -> " << error;
    else if (const nlohmann::json& result = json.contains("result") ? json["result"] : json; result.contains("status"))
        sensors = result["status"];

    trays.clear();
    max_lane_index = 0;
    for (int slot = 0; slot < *tool_count; ++slot) {
        AmsTrayData tray;
        tray.slot_index = slot;
        tray.slot         = slot;
        tray.extruder     = slot;
        tray.virtual_tool = slot;

        const std::string sensor_key = "filament_switch_sensor filament" + std::to_string(slot);
        if (sensors.contains(sensor_key) && sensors[sensor_key].contains("filament_detected") &&
            sensors[sensor_key]["filament_detected"].is_boolean())
            tray.has_filament = sensors[sensor_key]["filament_detected"].get<bool>();

        if (const auto color = tmt_ini_int(ini, "color" + std::to_string(slot))) {
            if (*color >= 0 && *color < (int) (sizeof(TMT_PALETTE) / sizeof(TMT_PALETTE[0])) && TMT_PALETTE[*color] != nullptr)
                tray.tray_color = TMT_PALETTE[*color];
            else
                // Rainbow, clear, or unknown: no colour rather than an invented one.
                BOOST_LOG_TRIVIAL(info) << "WonderMakerPrinterAgent::fetch_tmt_filament_info: slot " << slot
                                        << " has palette index " << *color << ", which has no RGB equivalent";
        }
        if (const auto material = tmt_ini_int(ini, "material" + std::to_string(slot))) {
            if (*material >= 0 && *material < (int) (sizeof(TMT_MATERIALS) / sizeof(TMT_MATERIALS[0]))) {
                tray.tray_type     = TMT_MATERIALS[*material];
                tray.tray_info_idx = map_filament_type_to_generic_id(tray.tray_type);
            } else {
                BOOST_LOG_TRIVIAL(info) << "WonderMakerPrinterAgent::fetch_tmt_filament_info: slot " << slot
                                        << " has unknown material index " << *material;
            }
        }

        max_lane_index = slot;
        trays.push_back(std::move(tray));
    }

    return !trays.empty();
}


std::string WonderMakerProtocol::build_prelude(bool bed_leveling, bool time_lapse)
{
    std::string script;
    // Same order as the touchscreen's start sequence. ENABLE is always stated because it is
    // persistent printer state; moonraker-timelapse ships in stock firmware.
    script += std::string("_SET_TIMELAPSE_SETUP ENABLE=") + (time_lapse ? "True" : "False") + "\n";
    if (time_lapse)
        script += "HYPERLAPSE ACTION=STOP\n";
    // G30 = probe a fresh adaptive mesh, G31 = load the saved one. Always stated: the variable is sticky.
    script += bed_leveling ? "G30\n" : "G31\n";
    return script;
}

std::string WonderMakerProtocol::build_map_lines(const std::vector<int>& box_of_tool_1based)
{
    std::string script;
    for (size_t tool = 0; tool < box_of_tool_1based.size(); ++tool) {
        const int box_1based = box_of_tool_1based[tool];
        // Unused tools keep whatever the screen last set.
        if (box_1based <= 0)
            continue;
        const std::string variable = "box_modify_t" + std::to_string(tool);
        const std::string value    = std::to_string(box_1based - 1);
        // The live variable is read on first selection; `_backup` survives the print.
        script += "SAVE_VARIABLE VARIABLE=" + variable + " VALUE=" + value + "\n";
        script += "SAVE_VARIABLE VARIABLE=" + variable + "_backup VALUE=" + value + "\n";
    }
    return script;
}

std::string WonderMakerProtocol::build_start_script(const std::string&      filename,
                                                    const std::vector<int>& box_of_tool_1based,
                                                    bool                    bed_leveling,
                                                    bool                    time_lapse)
{
    return build_prelude(bed_leveling, time_lapse) + build_map_lines(box_of_tool_1based) +
           "SDCARD_PRINT_FILE FILENAME=\"" + filename + "\"";
}

WonderMakerPrinterAgent::WonderMakerPrinterAgent(std::string log_dir) : MoonrakerPrinterAgent(std::move(log_dir)) {}

AgentInfo WonderMakerPrinterAgent::get_agent_info_static()
{
    return AgentInfo{"wondermaker", "WonderMaker", WONDERMAKER_AGENT_VERSION, "WonderMaker printer agent"};
}

bool WonderMakerPrinterAgent::fetch_filament_info(std::string dev_id, FilamentSyncMode sync_mode)
{
    // A ZR running WonderSync reports a real MMU object, which is richer than tmt1.ini.
    if (MoonrakerPrinterAgent::fetch_filament_info(dev_id, sync_mode))
        return true;

    if (!ensure_device_info(dev_id))
        return false;

    std::vector<AmsTrayData> trays;
    int                      max_lane_index = 0;
    if (!fetch_tmt_filament_info(trays, max_lane_index))
        return false;

    BOOST_LOG_TRIVIAL(info) << "WonderMakerPrinterAgent::fetch_filament_info: read " << (max_lane_index + 1)
                            << " tools from tmt1.ini";
    build_ams_payload(max_lane_index + 1, max_lane_index, trays, AmsUnitShape::Toolchanger);
    return true;
}

} // namespace Slic3r
