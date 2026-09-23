#include "AmsPayload.hpp"

#include "Http.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/DeviceCore/DevFilaSystem.h"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/DeviceCore/DevStorage.h"
#include "slic3r/GUI/DeviceCore/DevFirmware.h"

#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>
#include <wx/thread.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <sstream>
#include <utility>

namespace Slic3r {

std::string map_filament_type_to_generic_id(const std::string& filament_type)
{
    std::string upper = filament_type;
    boost::trim(upper);
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

    // Normalize reported material names to an OrcaFilamentLibrary generic family. The
    // family's filament_id is resolved from the loaded system presets below rather than
    // hardcoded, so profile id re-mints never require touching this table.
    // scripts/test_moonraker_lane_data.py parses this initializer; keep the {"A", "B"} format.
    static const std::map<std::string, std::string> type_to_ofl_family = {
        // PLA variants
        {"PLA", "PLA"},
        {"PLA-CF", "PLA-CF"},
        {"PLA SILK", "PLA Silk"},
        {"PLA-SILK", "PLA Silk"},
        {"PLA HIGH SPEED", "PLA High Speed"},
        {"PLA-HS", "PLA High Speed"},
        {"PLA HS", "PLA High Speed"},

        // ABS/ASA variants
        {"ABS", "ABS"},
        {"ASA", "ASA"},

        // PETG/PET variants
        {"PETG", "PETG"},
        {"PET", "PETG"},
        {"PCTG", "PCTG"},

        // PA/Nylon variants
        {"PA", "PA"},
        {"NYLON", "PA"},
        {"PA-CF", "PA-CF"},
        {"PPA", "PPA-CF"},
        {"PPA-CF", "PPA-CF"},
        {"PPA-GF", "PPA-GF"},

        // PC variants
        {"PC", "PC"},

        // PP/PE variants
        {"PE", "PE"},
        {"PP", "PP"},

        // Support materials
        {"PVA", "PVA"},
        {"HIPS", "HIPS"},
        {"BVOH", "BVOH"},

        // TPU variants
        {"TPU", "TPU"},

        // Other materials
        {"EVA", "EVA"},
        {"PHA", "PHA"},
        {"COPE", "CoPE"},
        {"SBS", "SBS"},
    };

    auto it = type_to_ofl_family.find(upper);
    if (it == type_to_ofl_family.end())
        return UNKNOWN_FILAMENT_ID;

    if (auto* bundle = GUI::wxGetApp().preset_bundle) {
        const Preset* preset = bundle->filaments.find_preset("Generic " + it->second + " @System");
        if (preset != nullptr && preset->is_system && !preset->filament_id.empty())
            return preset->filament_id;
    }

    // Unknown material, or no loaded preset data to resolve against
    return UNKNOWN_FILAMENT_ID;
}

std::string normalize_ams_color(const std::string& color)
{
    std::string value = color;
    boost::trim(value);

    // Remove 0x or 0X prefix if present
    if (value.size() >= 2 && (value.rfind("0x", 0) == 0 || value.rfind("0X", 0) == 0)) {
        value = value.substr(2);
    }
    // Remove # prefix if present
    if (!value.empty() && value[0] == '#') {
        value = value.substr(1);
    }

    // Extract only hex digits
    std::string normalized;
    for (char c : value) {
        if (std::isxdigit(static_cast<unsigned char>(c))) {
            normalized.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }
    }

    // If 6 hex digits, add FF alpha
    if (normalized.size() == 6) {
        normalized += "FF";
    }

    // Validate length - return default if invalid
    if (normalized.size() != 8) {
        return "00000000";
    }

    return normalized;
}

bool parse_moonraker_lane_data(const nlohmann::json& body,
                               std::vector<AmsTrayData>& trays,
                               int& max_lane_index)
{
    // Expected structure: { "result": { "namespace": "lane_data", "value": { "lane1": {...}, ... } } }
    if (!body.is_object() || !body.contains("result") || !body["result"].is_object() ||
        !body["result"].contains("value") || !body["result"]["value"].is_object()) {
        BOOST_LOG_TRIVIAL(warning) << "AmsPayload: unexpected lane_data response structure";
        return false;
    }

    const auto& value = body["result"]["value"];
    trays.clear();
    max_lane_index = 0;

    for (const auto& lane_item : value.items()) {
        const auto& lane_obj = lane_item.value();
        if (!lane_obj.is_object()) {
            continue;
        }

        // Extract lane index from the "lane" field (tool number, 0-based)
        int lane_index = -1;
        const auto lane_it = lane_obj.find("lane");
        if (lane_it != lane_obj.end() && lane_it->is_string()) {
            try {
                lane_index = std::stoi(lane_it->get<std::string>());
            } catch (...) {
                lane_index = -1;
            }
        }

        if (lane_index < 0) {
            continue;
        }

        AmsTrayData tray;
        tray.slot_index = lane_index;
        if (const auto it = lane_obj.find("color"); it != lane_obj.end() && it->is_string())
            tray.tray_color = it->get<std::string>();
        if (const auto it = lane_obj.find("material"); it != lane_obj.end() && it->is_string())
            tray.tray_type = it->get<std::string>();
        if (const auto it = lane_obj.find("bed_temp"); it != lane_obj.end() && it->is_number())
            tray.bed_temp = it->get<int>();
        if (const auto it = lane_obj.find("nozzle_temp"); it != lane_obj.end() && it->is_number())
            tray.nozzle_temp = it->get<int>();

        // Presence is independent of declared material: a physically loaded lane
        // with no user-declared material must not read as empty. Prefer an
        // explicit signal, fall back to the legacy "has a material type" rule.
        const auto has_it = lane_obj.find("has_filament");
        const auto loaded_it = lane_obj.find("loaded");
        if (has_it != lane_obj.end() && has_it->is_boolean())
            tray.has_filament = has_it->get<bool>();
        else if (loaded_it != lane_obj.end() && loaded_it->is_boolean())
            tray.has_filament = loaded_it->get<bool>();
        else
            tray.has_filament = !tray.tray_type.empty();

        max_lane_index = std::max(max_lane_index, lane_index);
        trays.push_back(tray);
    }

    if (trays.empty()) {
        BOOST_LOG_TRIVIAL(info) << "AmsPayload: no lanes found";
        return false;
    }

    return true;
}

LaneDataFetch read_moonraker_lane_data(const std::string& origin,
                                       const std::string& api_key,
                                       std::vector<AmsTrayData>& trays,
                                       int& max_lane_index)
{
    trays.clear();
    max_lane_index = 0;
    std::string base = origin;
    while (!base.empty() && base.back() == '/')
        base.pop_back();
    if (base.empty())
        return LaneDataFetch::unknown;

    unsigned    http_status = 0;
    std::string response_body;
    std::string http_error;
    auto http = Http::get(base + "/server/database/item?namespace=lane_data");
    if (!api_key.empty())
        http.header("X-Api-Key", api_key);
    http.timeout_connect(5)
        .timeout_max(10)
        .on_complete([&](std::string body, unsigned status) {
            http_status = status;
            if (status == 200)
                response_body = std::move(body);
            else
                http_error = "HTTP error: " + std::to_string(status);
        })
        .on_error([&](std::string, std::string err, unsigned status) {
            // Http routes every >=400 response through here, so a 404 arrives as
            // a real HTTP status, not a transport failure (REQ-STS-007 §7.7).
            http_status = status;
            http_error  = err;
            if (status > 0)
                http_error += " (HTTP " + std::to_string(status) + ")";
        })
        .perform_sync();

    if (http_status == 404) {
        BOOST_LOG_TRIVIAL(info) << "AmsPayload: lane_data not served yet (unknown topology)";
        return LaneDataFetch::unknown;
    }
    if (http_status != 200) {
        BOOST_LOG_TRIVIAL(info) << "AmsPayload: lane_data fetch failed: " << http_error;
        return LaneDataFetch::error;
    }

    auto json = nlohmann::json::parse(response_body, nullptr, false, true);
    if (json.is_discarded()) {
        BOOST_LOG_TRIVIAL(warning) << "AmsPayload: invalid lane_data JSON";
        return LaneDataFetch::error;
    }
    const bool empty_value = json.is_object() && json.contains("result") && json["result"].is_object() &&
                             json["result"].contains("value") && json["result"]["value"].is_object() &&
                             json["result"]["value"].empty();
    if (empty_value)
        return LaneDataFetch::none;

    if (!parse_moonraker_lane_data(json, trays, max_lane_index))
        return LaneDataFetch::error;
    return LaneDataFetch::synced;
}

void resolve_tray_info_idx(std::vector<AmsTrayData>& trays)
{
    auto* bundle = GUI::wxGetApp().preset_bundle;
    for (auto& tray : trays) {
        // Absent lanes render as placeholders; resolving an empty type is busy
        // work at best and could bind a bogus id.
        if (!tray.has_filament)
            continue;
        // A vendor-aware resolver may already have matched this lane; only fill
        // what is still empty so the generic fallback cannot overwrite it.
        if (!tray.tray_info_idx.empty())
            continue;
        tray.tray_info_idx = bundle
            ? bundle->filaments.filament_id_by_type(tray.tray_type)
            : map_filament_type_to_generic_id(tray.tray_type);
    }
}

nlohmann::json build_bbl_ams_json(const std::vector<AmsTrayData>& trays,
                                  int ams_count,
                                  int max_lane_index)
{
    nlohmann::json ams_array = nlohmann::json::array();

    // ams_exist_bits marks the units; tray_exist_bits marks the occupied
    // slots. uint64_t: unsigned long is 32-bit on MSVC, so 1UL << 32+ is UB.
    // BBL's fields are 64-bit; a lane past 63 cannot be represented at all.
    uint64_t ams_exist_bits = 0;
    uint64_t tray_exist_bits = 0;

    auto set_exist_bit = [](uint64_t& bits, int index) {
        if (index < 0)
            return;
        if (index >= 64) {
            BOOST_LOG_TRIVIAL(warning) << "AmsPayload: lane index " << index << " exceeds the 64-bit exist-bits field; bit dropped";
            return;
        }
        bits |= (uint64_t{1} << index);
    };

    for (int ams_id = 0; ams_id < ams_count; ++ams_id) {
        set_exist_bit(ams_exist_bits, ams_id);

        nlohmann::json ams_unit = nlohmann::json::object();
        ams_unit["id"] = std::to_string(ams_id);
        ams_unit["info"] = "0002";  // treat as AMS_LITE

        nlohmann::json tray_array = nlohmann::json::array();
        int max_slot_in_this_ams = std::min(3, max_lane_index - ams_id * 4);
        for (int slot_id = 0; slot_id <= max_slot_in_this_ams; ++slot_id) {
            int slot_index = ams_id * 4 + slot_id;

            // Find tray with matching slot_index
            const AmsTrayData* tray = nullptr;
            for (const auto& t : trays) {
                if (t.slot_index == slot_index) {
                    tray = &t;
                    break;
                }
            }

            nlohmann::json tray_json = nlohmann::json::object();
            tray_json["id"] = std::to_string(slot_id);
            tray_json["tag_uid"] = "0000000000000000";

            if (tray && tray->has_filament) {
                set_exist_bit(tray_exist_bits, slot_index);

                // Present lanes never carry tray_slot_placeholder: a loaded but
                // undeclared lane stays occupied with an empty type, which is
                // what distinguishes it from an absent slot.
                tray_json["tray_info_idx"] = tray->tray_info_idx;
                tray_json["tray_type"] = tray->tray_type;
                tray_json["tray_color"] = normalize_ams_color(tray->tray_color);

                // Add temperature data if provided
                if (tray->bed_temp > 0) {
                    tray_json["bed_temp"] = std::to_string(tray->bed_temp);
                }
                if (tray->nozzle_temp > 0) {
                    tray_json["nozzle_temp_max"] = std::to_string(tray->nozzle_temp);
                }
            } else {
                tray_json["tray_info_idx"] = "";
                tray_json["tray_type"] = "";
                tray_json["tray_color"] = "00000000";
                tray_json["tray_slot_placeholder"] = "1";
            }

            tray_array.push_back(tray_json);
        }
        ams_unit["tray"] = tray_array;
        ams_array.push_back(ams_unit);
    }

    // Format as hex strings (matching BBL protocol)
    std::ostringstream ams_exist_ss;
    ams_exist_ss << std::hex << std::uppercase << ams_exist_bits;
    std::ostringstream tray_exist_ss;
    tray_exist_ss << std::hex << std::uppercase << tray_exist_bits;

    nlohmann::json ams_json = nlohmann::json::object();
    ams_json["ams"] = ams_array;
    ams_json["ams_exist_bits"] = ams_exist_ss.str();
    ams_json["tray_exist_bits"] = tray_exist_ss.str();
    return ams_json;
}

// --- Removal/absence state and op capability ---------------------------------

static std::mutex                         g_ams_state_mutex;

// One device's declaration from its get_capabilities reply. ops_known separates
// "no reply yet" (never gate) from "answered without ops" (gate every write).
struct AmsDeviceCaps
{
    std::vector<std::string> ops;
    bool                     ops_known      = false;
    bool                     has_ams        = false;
    bool                     filament_slots = false;
};
static std::map<std::string, AmsDeviceCaps> g_ams_caps;

void register_ams_ops(const std::string& dev_id, const std::vector<std::string>& ops)
{
    if (dev_id.empty())
        return;
    std::lock_guard<std::mutex> lock(g_ams_state_mutex);
    AmsDeviceCaps& caps = g_ams_caps[dev_id];
    caps.ops            = ops;
    caps.ops_known      = true;
}

bool ams_op_supported(const std::string& dev_id, const std::string& op)
{
    std::lock_guard<std::mutex> lock(g_ams_state_mutex);
    auto it = g_ams_caps.find(dev_id);
    if (it == g_ams_caps.end() || !it->second.ops_known)
        return true; // no capability reply yet: never gate
    return std::find(it->second.ops.begin(), it->second.ops.end(), op) != it->second.ops.end();
}

void register_ams_capability(const std::string& dev_id, bool has_ams)
{
    if (dev_id.empty())
        return;
    std::lock_guard<std::mutex> lock(g_ams_state_mutex);
    g_ams_caps[dev_id].has_ams = has_ams;
}

bool has_ams_capability(const std::string& dev_id)
{
    std::lock_guard<std::mutex> lock(g_ams_state_mutex);
    auto it = g_ams_caps.find(dev_id);
    return it != g_ams_caps.end() && it->second.has_ams;
}

void register_filament_slots(const std::string& dev_id, bool has_slots)
{
    if (dev_id.empty())
        return;
    std::lock_guard<std::mutex> lock(g_ams_state_mutex);
    g_ams_caps[dev_id].filament_slots = has_slots;
}

bool has_filament_slots(const std::string& dev_id)
{
    std::lock_guard<std::mutex> lock(g_ams_state_mutex);
    auto it = g_ams_caps.find(dev_id);
    return it != g_ams_caps.end() && it->second.filament_slots;
}

void clear_ams_caps(const std::string& dev_id)
{
    if (dev_id.empty())
        return;
    std::lock_guard<std::mutex> lock(g_ams_state_mutex);
    g_ams_caps.erase(dev_id);
}

void build_ams_payload_for_device(const std::string& dev_id,
                                  const std::optional<std::string>& printer_type,
                                  int ams_count,
                                  int max_lane_index,
                                  const std::vector<AmsTrayData>& trays,
                                  const QueueOnMainFn& queue_fn,
                                  const TrayInfoResolver& vendor_resolver)
{
    // A caller on the GUI thread must mutate DeviceManager inline: invoking
    // queue_fn (CallAfter) would defer the work until after the caller has
    // already read DevFilaSystem. Background callers route through queue_fn.
    const bool on_main = wxIsMainThread();
    auto apply = [dev_id, printer_type, ams_count, max_lane_index, trays, vendor_resolver]() {
    // Look up MachineObject via DeviceManager
    auto* dev_manager = GUI::wxGetApp().getDeviceManager();
    if (!dev_manager) {
        return;
    }
    MachineObject* obj = dev_manager->get_my_machine(dev_id);
    if (!obj) {
        return;
    }

    // Resolve preset ids here: reads the GUI preset bundle, which is only safe
    // on the main thread. trays is a copy, so mutating it is fine. The optional
    // vendor resolver runs first; the generic resolver fills the rest.
    std::vector<AmsTrayData> resolved = trays;
    if (vendor_resolver)
        vendor_resolver(resolved);
    resolve_tray_info_idx(resolved);

    // Wrap in the expected structure for ParseV1_0
    nlohmann::json print_json = nlohmann::json::object();
    print_json["ams"] = build_bbl_ams_json(resolved, ams_count, max_lane_index);

    // Call the parser to populate DevFilaSystem
    DevFilaSystemParser::ParseV1_0(print_json, obj, obj->GetFilaSystem().get(), false);
    BOOST_LOG_TRIVIAL(info) << "AmsPayload: parsed " << resolved.size() << " trays";

    // Set printer_type so update_sync_status() can match it against the preset's
    // printer type. nullopt = leave it (the caller's push_status already carries
    // it); a value = assign, including empty, preserving the old behavior.
    if (printer_type.has_value()) {
        obj->printer_type = *printer_type;
    }

    // Set push counters so is_info_ready() returns true for pull-mode agents.
    if (obj->m_push_count == 0) {
        obj->m_push_count = 1;
    }
    if (obj->m_full_msg_count == 0) {
        obj->m_full_msg_count = 1;
    }
    obj->last_push_time = std::chrono::system_clock::now();

    // Set storage state - Moonraker printers use virtual_sdcard, storage is always available.
    // This is required for SelectMachineDialog to allow printing (otherwise it blocks with "No SD card").
    obj->GetStorage()->set_sdcard_state(DevStorage::HAS_SDCARD_NORMAL);

    // Populate module_vers so is_info_ready() passes the version check.
    if (obj->module_vers.empty()) {
        DevFirmwareVersionInfo ota_info;
        ota_info.name = "ota";
        ota_info.sw_ver = "1.0.0";  // Placeholder version
        obj->module_vers.emplace("ota", ota_info);
    }
    };

    if (queue_fn && !on_main) {
        queue_fn(apply);
    } else {
        apply();
    }
}


} // namespace Slic3r
