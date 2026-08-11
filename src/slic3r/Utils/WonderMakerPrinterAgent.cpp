#include "WonderMakerPrinterAgent.hpp"

#include "Http.hpp"
#include "nlohmann/json.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>

#include <optional>
#include <sstream>
#include <string>

namespace Slic3r {

constexpr const char* WONDERMAKER_AGENT_VERSION = "0.0.1";

namespace {
// Orca: the ZR touchscreen's colour picker, in the picker's own grid order. These are INDICES
// INTO SPRITE ASSETS, not colours -- writing a hex into tmt1.ini makes the slot render white --
// so every entry here is our approximation of what a swatch looks like, chosen to be recognisable
// next to a real spool. The upstream community table was derived the same way; matching a named
// swatch to the obvious filament colour is a better answer for the user than no colour at all.
//
// The exceptions are the last two. Rainbow and transparent are textures with no colour to
// approximate, and inventing one would misrepresent the spool rather than merely approximate it,
// so they stay empty. Gold, bronze and silver are textures too, but their conventional hexes are
// exactly how those spools are described everywhere else.
constexpr const char* TMT_PALETTE[] = {
    /*  0 white     */ "#FFFFFF", /*  1 cream    */ "#FFFDD0", /*  2 brown   */ "#8B4513",
    /*  3 grey      */ "#808080", /*  4 black    */ "#000000", /*  5 teal    */ "#008080",
    /*  6 lt. blue  */ "#87CEEB", /*  7 blue     */ "#0000FF", /*  8 green   */ "#008000",
    /*  9 dk green  */ "#006400", /* 10 yellow   */ "#FFFF00", /* 11 orange  */ "#FFA500",
    /* 12 pink      */ "#FFC0CB", /* 13 magenta  */ "#FF00FF", /* 14 red     */ "#FF0000",
    /* 15 violet    */ "#EE82EE", /* 16 gold     */ "#FFD700", /* 17 bronze  */ "#CD7F32",
    /* 18 silver    */ "#C0C0C0", /* 19 rainbow  */ nullptr,   /* 20 clear   */ nullptr,
};

// The screen's material list, in its own order. The screen knows only these coarse types.
constexpr const char* TMT_MATERIALS[] = {
    "PLA", "PLA Silk", "ABS", "PETG", "PLA Matte", "PLA Metal", "ABS Matte", "PETG Matte",
    "PVA", "TPE", "Marble", "HIPS", "TPU", "PET", "Wood", "ASA",
};

// One blocking GET, shaped like the other fetchers in this file. Returns false on any non-200.
bool tmt_http_get(const std::string& url, const std::string& api_key, std::string& body_out)
{
    bool        ok = false;
    std::string error;
    auto        http = Http::get(url);
    if (!api_key.empty())
        http.header("X-Api-Key", api_key);
    http.timeout_connect(5)
        .timeout_max(10)
        .on_complete([&](std::string body, unsigned status) {
            if (status == 200) {
                body_out = std::move(body);
                ok       = true;
            } else {
                error = "HTTP error: " + std::to_string(status);
            }
        })
        .on_error([&](std::string, std::string err, unsigned status) {
            error = err;
            if (status > 0)
                error += " (HTTP " + std::to_string(status) + ")";
        })
        .perform_sync();
    if (!ok)
        BOOST_LOG_TRIVIAL(debug) << "WonderMakerPrinterAgent fetch: " << url << " -> " << error;
    return ok;
}

// "key=value" out of an INI body, restricted to full-line entries. Full-line ';' comments survive
// the screen's own rewrites and are ignored here; INLINE comments are NOT stripped by the
// firmware's parser either, so a value carrying one is malformed to both of us and is skipped.
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
    // The file is the printer's own record: its touchscreen rewrites tmt1.ini whenever the user
    // assigns a filament, and reads it back at boot. Absent = this is not a stock-firmware ZR.
    std::string ini;
    if (!tmt_http_get(join_url(device_info.base_url, "/server/files/config/tmt1.ini"), device_info.api_key, ini))
        return false;

    // last_thr_number is the tool count. Without it we are not looking at a file we understand,
    // so refuse rather than guess a size from whichever colorN keys happen to be present.
    const std::optional<int> tool_count = tmt_ini_int(ini, "last_thr_number");
    if (!tool_count.has_value() || *tool_count <= 0 || *tool_count > 64) {
        BOOST_LOG_TRIVIAL(debug) << "WonderMakerPrinterAgent::fetch_tmt_filament_info: no usable last_thr_number";
        return false;
    }

    // Presence per tool. Read filament_detected, NOT enabled: only the ACTIVE tool's sensor is
    // enabled, so `enabled` reports one loaded tool no matter how many are really loaded.
    std::string sensors_url = join_url(device_info.base_url, "/printer/objects/query?");
    for (int i = 0; i < *tool_count; ++i)
        sensors_url += (i ? "&" : "") + std::string("filament_switch_sensor%20filament") + std::to_string(i);
    std::string sensors_body;
    nlohmann::json sensors = nlohmann::json::object();
    if (tmt_http_get(sensors_url, device_info.api_key, sensors_body)) {
        auto parsed = nlohmann::json::parse(sensors_body, nullptr, false, true);
        if (!parsed.is_discarded() && parsed.contains("result") && parsed["result"].contains("status"))
            sensors = parsed["result"]["status"];
    }

    trays.clear();
    max_lane_index = 0;
    for (int slot = 0; slot < *tool_count; ++slot) {
        AmsTrayData tray;
        tray.slot_index = slot;

        const std::string sensor_key = "filament_switch_sensor filament" + std::to_string(slot);
        if (sensors.contains(sensor_key) && sensors[sensor_key].contains("filament_detected") &&
            sensors[sensor_key]["filament_detected"].is_boolean())
            tray.has_filament = sensors[sensor_key]["filament_detected"].get<bool>();

        if (const auto color = tmt_ini_int(ini, "color" + std::to_string(slot))) {
            if (*color >= 0 && *color < (int) (sizeof(TMT_PALETTE) / sizeof(TMT_PALETTE[0])) && TMT_PALETTE[*color] != nullptr)
                tray.tray_color = TMT_PALETTE[*color];
            else
                // Rainbow, transparent, or an index this build has never heard of. Leave the
                // colour empty rather than invent one; the slot still reports its material and
                // presence, and the index is logged so a new swatch is reportable, not invisible.
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


std::string WonderMakerProtocol::build_start_script(const std::string&      filename,
                                                    const std::vector<int>& box_of_tool_1based,
                                                    bool                    bed_leveling)
{
    std::string script;
    // The ZR's touchscreen probes before each print; G30 is what its start sequence issues. An
    // absent command simply means "don't probe", so the off case emits nothing rather than a
    // disable command the firmware has no notion of.
    if (bed_leveling)
        script += "G30\n";

    for (size_t tool = 0; tool < box_of_tool_1based.size(); ++tool) {
        const int box_1based = box_of_tool_1based[tool];
        // A tool this plate doesn't print carries no pick; leaving its variable alone preserves
        // whatever the printer's own screen last set for it.
        if (box_1based <= 0)
            continue;
        const std::string variable = "box_modify_t" + std::to_string(tool);
        const std::string value    = std::to_string(box_1based - 1);
        // Both forms, every time: the live variable is what the tool reads on first selection,
        // and `_backup` is what survives the print -- writing only one leaves the printer
        // describing a mapping it did not run.
        script += "SAVE_VARIABLE VARIABLE=" + variable + " VALUE=" + value + "\n";
        script += "SAVE_VARIABLE VARIABLE=" + variable + "_backup VALUE=" + value + "\n";
    }

    script += "SDCARD_PRINT_FILE FILENAME=\"" + filename + "\"";
    return script;
}

std::string WonderMakerProtocol::build_start_script(const std::string& filename, const DevicePrintJobInfo& job)
{
    // filament_map_1based is indexed by the tool numbers the g-code actually emits, which for this
    // protocol are dense (see protocol_requires_dense_tool_numbering), so it is already the
    // per-tool box list this dialect wants.
    return build_start_script(filename, job.filament_map_1based, job.option_on("bed_leveling"));
}

WonderMakerPrinterAgent::WonderMakerPrinterAgent(std::string log_dir) : MoonrakerPrinterAgent(std::move(log_dir)) {}

AgentInfo WonderMakerPrinterAgent::get_agent_info_static()
{
    return AgentInfo{"wondermaker", "WonderMaker", WONDERMAKER_AGENT_VERSION, "WonderMaker printer agent"};
}

bool WonderMakerPrinterAgent::fetch_filament_info(std::string dev_id)
{
    // The inherited readers first: a ZR running WonderSync reports a real MMU object, and that
    // data is richer than anything the config file carries.
    if (MoonrakerPrinterAgent::fetch_filament_info(dev_id))
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
