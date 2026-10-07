#include "SnapmakerPrinterAgent.hpp"
#include "Http.hpp"
#include "MoonrakerPrinterAgent.hpp"
#include "IPrinterAgent.hpp"
#include "bambu_networking.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include "nlohmann/json.hpp"
#include <atomic>
#include <boost/log/trivial.hpp>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>
#include <string>
#include "libslic3r/Preset.hpp"
#include <cstddef>
#include <utility>

using json = nlohmann::json;

namespace Slic3r {

namespace {

constexpr const char* SNAPMAKER_AGENT_VERSION = "0.0.1";
constexpr int64_t     CAMERA_REFRESH_INTERVAL_MS = 300'000;

int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// RAII release of the in-flight fetch slot; movable so a failed thread start still releases it.
struct InFlightGuard
{
    MoonrakerPrinterAgent* owner;
    explicit InFlightGuard(MoonrakerPrinterAgent& o) noexcept : owner(&o) {}
    InFlightGuard(InFlightGuard&& other) noexcept : owner(other.owner) { other.owner = nullptr; }
    InFlightGuard(const InFlightGuard&) = delete;
    InFlightGuard& operator=(const InFlightGuard&) = delete;
    InFlightGuard& operator=(InFlightGuard&&) = delete;
    ~InFlightGuard() { if (owner) owner->release_fetch_slot(); }
};

// nlohmann::json::value() returns the default only when the key is absent; a present but
// null/wrong-typed value throws. Firmware JSON is untrusted, so read defensively.
int read_int_or(const nlohmann::json& obj, const char* key, int fallback)
{
    auto it = obj.find(key);
    return (it != obj.end() && it->is_number_integer()) ? it->get<int>() : fallback;
}

std::vector<std::string> read_string_array_or(const nlohmann::json& obj, const char* key)
{
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_array())
        return {};
    std::vector<std::string> out;
    out.reserve(it->size());
    for (const auto& v : *it)
        out.push_back(v.is_string() ? v.get<std::string>() : std::string{});
    return out;
}

std::vector<bool> read_bool_array_or(const nlohmann::json& obj, const char* key)
{
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_array())
        return {};
    std::vector<bool> out;
    out.reserve(it->size());
    for (const auto& v : *it)
        out.push_back(v.is_boolean() ? v.get<bool>() : false);
    return out;
}

// Parse a hex colour, stopping at the first non-hex character. std::stoul throws on
// empty/non-hex input, which must not escape the detached fetch thread.
unsigned int parse_hex_color(const std::string& hex)
{
    unsigned int value = 0;
    for (char c : hex) {
        int digit;
        if (c >= '0' && c <= '9')
            digit = c - '0';
        else if (c >= 'a' && c <= 'f')
            digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            digit = c - 'A' + 10;
        else
            break;
        value = (value << 4) | static_cast<unsigned int>(digit);
    }
    return value;
}

// Safely access a parallel array by index, returning a fallback if out of bounds.
template<typename T>
T safe_at(const std::vector<T>& vec, int index, const T& fallback)
{
    return (index >= 0 && index < static_cast<int>(vec.size())) ? vec[index] : fallback;
}

std::string find_closest_color_preset_by_vendor_and_type(const PresetCollection& filaments,
                                                         const std::string&      vendor_name,
                                                         const std::string&      filament_type,
                                                         const std::string&      color_rgba)
{
    std::string best_match_id       = "";
    int         best_color_distance = 0xffffffff;

    for (const auto& p : filaments.get_presets()) {
        if (p.is_visible && p.is_compatible &&
            // Filament profile must be detached from parent to be considered for matching
            filaments.get_preset_base(p) == &p && p.config.opt_string("filament_vendor", 0u) == vendor_name &&
            p.config.opt_string("filament_type", 0u) == filament_type) {
            // The printer returns RGBA in the format RRGGBBAA, but profiles store color as #RRGGBB,
            // so we must remove # and ignore alpha channel for distance calculation. Firmware
            // colours are untrusted; parse_hex_color tolerates empty/non-hex instead of throwing.
            unsigned int target_color_value =
                parse_hex_color(color_rgba.substr(0, color_rgba.size() >= 2 ? color_rgba.size() - 2 : 0));

            std::string  p_color = p.config.opt_string("default_filament_colour", 0u);
            unsigned int p_color_value = 0;
            if (!p_color.empty()) {
                size_t hash_pos = p_color.find("#");
                p_color_value   = parse_hex_color(p_color.substr(hash_pos != std::string::npos ? hash_pos + 1 : 0));
            } else {
                // Default to black if no color specified in profile. Assume other profiles might be a closer color match.
                // Could be a problem if the target color is also black and there exist a specific profile for that type, vendor and color
                // combination.
                p_color_value = 0;
            }

            // Calculate Euclidean color distance in RGB space
            int dr = ((target_color_value & 0xff) - (p_color_value & 0xff));
            int dg = (((target_color_value >> 8) & 0xff) - ((p_color_value >> 8) & 0xff));
            int db = (((target_color_value >> 16) & 0xff) - ((p_color_value >> 16) & 0xff));
            unsigned int distance = dr * dr + dg * dg + db * db;

            if (distance < best_color_distance) {
                best_color_distance = distance;
                best_match_id       = p.filament_id;
            }
        }
    }
    return best_match_id;
}

} // anonymous namespace

SnapmakerPrinterAgent::SnapmakerPrinterAgent(std::string log_dir) : MoonrakerPrinterAgent(std::move(log_dir)) {}

void SnapmakerPrinterAgent::start_camera_monitor()
{
    enqueue_command([this] {
        send_ws_rpc("camera.start_monitor",
                    {{"domain", "lan"}, {"interval", 0}, {"expect_pw", false}});
    });
    m_camera_last_fire_ms.store(now_ms());
}

void SnapmakerPrinterAgent::on_status_loop_tick(const std::string& dev_id)
{
    (void) dev_id;
    const int64_t last = m_camera_last_fire_ms.load();
    if (last == 0 || now_ms() - last >= CAMERA_REFRESH_INTERVAL_MS) {
        start_camera_monitor();
    }
}

int SnapmakerPrinterAgent::command_start_camera(std::string dev_id)
{
    (void) dev_id;
    start_camera_monitor();
    return BAMBU_NETWORK_SUCCESS;
}

AgentInfo SnapmakerPrinterAgent::get_agent_info_static()
{
    return AgentInfo{"snapmaker", "Snapmaker", SNAPMAKER_AGENT_VERSION, "Snapmaker printer agent"};
}

std::string SnapmakerPrinterAgent::combine_filament_type(const std::string& type, const std::string& sub_type)
{
    const std::string base = trim_and_upper(type);
    const std::string sub  = trim_and_upper(sub_type);

    if (base.empty())
        return "PLA";

    if (sub.empty() || sub == "NONE")
        return base;

    if (sub == "CF")
        return base + "-CF";
    if (sub == "GF")
        return base + "-GF";
    if (sub == "SNAPSPEED" || sub == "HS")
        return base + " HIGH SPEED";
    if (sub == "SILK")
        return base + " SILK";
    if (sub == "WOOD")
        return base + " WOOD";
    if (sub == "MATTE")
        return base + " MATTE";
    if (sub == "MARBLE")
        return base + " MARBLE";

    // Unrecognized sub-type (brand names like Polylite, Basic, etc.) -- use base type only
    return base;
}

bool SnapmakerPrinterAgent::fetch_filament_info(std::string dev_id, FilamentSyncMode sync_mode)
{
    (void) dev_id;
    if (sync_mode != get_filament_sync_mode())
        return false;

    // Snapshot everything the fetch needs (URL, api key, TLS/CA): a reconnect can rewrite
    // device_info meanwhile.
    const ConnectionSettings connection = get_connection_settings();

    // Reserve under the same mutex shutdown() uses, so the flag and the count can't race.
    {
        std::lock_guard<std::mutex> lock(fetch_lifecycle_mutex);
        if (shutting_down.load())
            return false;
        filament_fetch_in_flight.fetch_add(1, std::memory_order_relaxed);
    }

    InFlightGuard guard{*this};
    std::thread([this, guard = std::move(guard), connection]() {
        try {
            const std::string url = join_url(connection.base_url, "/printer/objects/query?print_task_config&filament_detect");

            std::string response_body;
            bool success = false;
            std::string http_error;

            auto http = Http::get(url);
            configure_http(http, connection);
            if (!connection.api_key.empty()) {
                http.header("X-Api-Key", connection.api_key);
            }
            http.timeout_connect(5)
                .timeout_max(10)
                .on_complete([&](std::string body, unsigned status) {
                    if (status == 200) {
                        response_body = body;
                        success       = true;
                    } else {
                        http_error = "HTTP error: " + std::to_string(status);
                    }
                })
                .on_error([&](std::string body, std::string err, unsigned status) {
                    http_error = err;
                    if (status > 0) {
                        http_error += " (HTTP " + std::to_string(status) + ")";
                    }
                })
                .perform_sync();

            if (!success) {
                BOOST_LOG_TRIVIAL(warning) << "SnapmakerPrinterAgent::fetch_filament_info: HTTP request failed: " << http_error;
                return;
            }

            auto json = nlohmann::json::parse(response_body, nullptr, false, true);
            if (json.is_discarded()) {
                BOOST_LOG_TRIVIAL(warning) << "SnapmakerPrinterAgent::fetch_filament_info: Invalid JSON response";
                return;
            }

            // Navigate to result.status.print_task_config
            if (!json.contains("result") || !json["result"].contains("status") || !json["result"]["status"].contains("print_task_config")) {
                BOOST_LOG_TRIVIAL(warning) << "SnapmakerPrinterAgent::fetch_filament_info: Missing print_task_config in response";
                return;
            }

            auto& ptc = json["result"]["status"]["print_task_config"];

            // Read parallel arrays from print_task_config
            auto filament_exist    = read_bool_array_or(ptc, "filament_exist");
            auto filament_type     = read_string_array_or(ptc, "filament_type");
            auto filament_sub_type = read_string_array_or(ptc, "filament_sub_type");
            auto filament_color    = read_string_array_or(ptc, "filament_color_rgba");
            auto filament_vendor   = read_string_array_or(ptc, "filament_vendor");

            const int slot_count = static_cast<int>(filament_exist.size());
            if (slot_count == 0) {
                BOOST_LOG_TRIVIAL(info) << "SnapmakerPrinterAgent::fetch_filament_info: No filament slots reported";
                return;
            }

            // Read NFC filament_detect data for temperature info (optional)
            nlohmann::json nfc_info;
            if (json["result"]["status"].contains("filament_detect") && json["result"]["status"]["filament_detect"].contains("info")) {
                nfc_info = json["result"]["status"]["filament_detect"]["info"];
            }

            static const std::string empty_str;
            static const std::string default_color = "FFFFFFFF";

            std::vector<AmsTrayData> trays;
            trays.reserve(slot_count);

            for (int i = 0; i < slot_count; ++i) {
                AmsTrayData tray;
                tray.slot_index   = i;
                tray.has_filament = filament_exist[i];

                if (tray.has_filament) {
                    tray.tray_type  = combine_filament_type(safe_at(filament_type, i, empty_str), safe_at(filament_sub_type, i, empty_str));
                    tray.tray_color = safe_at(filament_color, i, default_color);

                    auto* bundle = GUI::wxGetApp().preset_bundle;
                    // Try to find a matching preset for this filament based on vendor, type and color.
                    // If not found, default to traditional search by type only or generic type mapping.
                    if (bundle) {
                        std::string vendor      = safe_at(filament_vendor, i, empty_str);
                        std::string filament_id = find_closest_color_preset_by_vendor_and_type(bundle->filaments, vendor, tray.tray_type,
                                                                                               tray.tray_color);

                        if (!filament_id.empty()) {
                            tray.tray_info_idx = filament_id;
                            BOOST_LOG_TRIVIAL(warning)
                                << "Filament sync: Found manufacturer-specific profile for slot " << i << ": " << filament_id;
                        } else {
                            tray.tray_info_idx = bundle->filaments.filament_id_by_type(tray.tray_type);
                        }
                    } else {
                        tray.tray_info_idx = map_filament_type_to_generic_id(tray.tray_type);
                    }

                    // Extract NFC temperature data if available
                    if (nfc_info.is_array() && i < static_cast<int>(nfc_info.size()) && nfc_info[i].is_object()) {
                        auto&       nfc_slot = nfc_info[i];
                        std::string vendor   = "NONE";
                        if (auto vendor_it = nfc_slot.find("VENDOR"); vendor_it != nfc_slot.end() && vendor_it->is_string()) {
                            vendor = vendor_it->get<std::string>();
                        }
                        if (vendor != "NONE" && !vendor.empty()) {
                            tray.bed_temp    = read_int_or(nfc_slot, "BED_TEMP", 0);
                            tray.nozzle_temp = read_int_or(nfc_slot, "FIRST_LAYER_TEMP", 0);
                        }
                    }
                }

                trays.emplace_back(std::move(tray));
            }

            build_ams_payload(1, slot_count - 1, trays);
        } catch (const std::exception& e) {
            // why: an exception escaping a detached thread is std::terminate, and firmware
            // JSON is untrusted; mirror run_command_worker and swallow it here.
            BOOST_LOG_TRIVIAL(error) << "SnapmakerPrinterAgent::fetch_filament_info: unhandled exception: " << e.what();
        } catch (...) {
            BOOST_LOG_TRIVIAL(error) << "SnapmakerPrinterAgent::fetch_filament_info: unhandled exception";
        }
    }).detach();

    return true;
}

std::string SnapmakerPrinterAgent::get_camera_url() const
{
    return get_connection_settings().base_url + "/server/files/camera/monitor.jpg";
}

FilamentSyncMode SnapmakerPrinterAgent::get_filament_sync_mode() const
{
    if (GUI::wxGetApp().app_config->get_bool("use_printer_agents"))
        return FilamentSyncMode::subscription;
    return FilamentSyncMode::pull;
}

} // namespace Slic3r
