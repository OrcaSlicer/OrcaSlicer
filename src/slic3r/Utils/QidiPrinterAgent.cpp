#include "QidiPrinterAgent.hpp"
#include "Http.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include "nlohmann/json.hpp"
#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>
#include <cctype>
#include <sstream>

namespace Slic3r {

namespace {

// Check whether any visible, compatible base preset in the collection has the given filament_id.
bool has_visible_base_preset(const PresetCollection& filaments, const std::string& filament_id)
{
    for (const auto& p : filaments.get_presets()) {
        if (p.is_visible && p.is_compatible
            && filaments.get_preset_base(p) == &p
            && p.filament_id == filament_id)
            return true;
    }
    return false;
}

} // anonymous namespace

const std::string QidiPrinterAgent_VERSION = "0.0.1";

QidiPrinterAgent::QidiPrinterAgent(std::string log_dir) : MoonrakerPrinterAgent(std::move(log_dir))
{
}

AgentInfo QidiPrinterAgent::get_agent_info_static()
{
    return AgentInfo{"qidi", "Qidi", QidiPrinterAgent_VERSION, "Qidi printer agent"};
}

bool QidiPrinterAgent::fetch_filament_info(std::string dev_id)
{
    std::string error;

    // 1. Fetch device info and infer series_id
    std::string series_id;
    {
        MoonrakerDeviceInfo info;
        if (fetch_device_info(device_info.base_url, device_info.api_key, info, error)) {
            series_id = infer_series_id(info.model_id, info.dev_name);
        }
    }
    if (series_id.empty()) {
        // Fall back to the configured Orca model if Moonraker doesn't expose a usable identifier.
        series_id = infer_series_id(device_info.model_id, device_info.model_name);
    }

    // 2. Fetch filament dictionary
    QidiFilamentDict dict;
    if (!fetch_filament_dict(device_info.base_url, device_info.api_key, dict, error)) {
        BOOST_LOG_TRIVIAL(warning) << "QidiPrinterAgent::fetch_filament_info: Failed to fetch filament dict: " << error;
    }

    // 3. Fetch slot info and build AmsTrayData directly
    std::vector<AmsTrayData> trays;
    int                      box_count = 0;
    if (!fetch_slot_info(device_info.base_url, device_info.api_key, dict, series_id, trays, box_count, error)) {
        BOOST_LOG_TRIVIAL(warning) << "QidiPrinterAgent::fetch_filament_info: Failed to fetch slot info: " << error;
        return false;
    }

    // 4. Build the AMS payload
    build_ams_payload(box_count, box_count * 4 - 1, trays);
    return true;
}

bool QidiPrinterAgent::apply_box_mapping(const PrintParams& params) const
{
    // enable_box mirrors task_use_ams: engage the multi-color box only when this
    // job actually routes filament through it. (See qidi-ams-findings.md §2/§8.3 —
    // if firmware treats enable_box as "a box exists" rather than "use it this job",
    // switch this gate to HasAms()/box_count instead.)
    const int enable = params.task_use_ams ? 1 : 0;
    if (!send_gcode(device_info.dev_id, "SAVE_VARIABLE VARIABLE=enable_box VALUE=" + std::to_string(enable))) {
        BOOST_LOG_TRIVIAL(error) << "QidiPrinterAgent::apply_box_mapping: failed to set enable_box";
        return false;
    }

    // When the box isn't used this job, leave the existing value_t<tool> slot
    // assignments untouched (enable_box=0 is enough to disengage it).
    if (!enable)
        return true;

    if (params.ams_mapping.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "QidiPrinterAgent::apply_box_mapping: enable_box set but ams_mapping is empty";
        return true;
    }

    // ams_mapping (v0) is a JSON array indexed by filament/tool; each value is the
    // physical box slot (-1 = unmapped). Mirror it onto the printer's value_t<tool>
    // variables: SAVE_VARIABLE VARIABLE=value_t<tool> VALUE='slot<n>'.
    auto mapping = nlohmann::json::parse(params.ams_mapping, nullptr, /*allow_exceptions*/ false);
    if (mapping.is_discarded() || !mapping.is_array()) {
        BOOST_LOG_TRIVIAL(error) << "QidiPrinterAgent::apply_box_mapping: invalid ams_mapping: " << params.ams_mapping;
        return false;
    }

    for (size_t tool = 0; tool < mapping.size(); ++tool) {
        if (!mapping[tool].is_number_integer())
            continue;
        const int slot = mapping[tool].get<int>();
        if (slot < 0)
            continue; // unmapped filament — skip
        const std::string gcode = "SAVE_VARIABLE VARIABLE=value_t" + std::to_string(tool) +
                                  " VALUE=\"'slot" + std::to_string(slot) + "'\"";
        if (!send_gcode(device_info.dev_id, gcode)) {
            BOOST_LOG_TRIVIAL(error) << "QidiPrinterAgent::apply_box_mapping: failed to set value_t" << tool;
            return false;
        }
    }
    return true;
}

int QidiPrinterAgent::start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    if (!apply_box_mapping(params))
        return BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED;
    return MoonrakerPrinterAgent::start_local_print(std::move(params), update_fn, cancel_fn);
}

int QidiPrinterAgent::start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    if (!apply_box_mapping(params))
        return BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED;
    return MoonrakerPrinterAgent::start_print(std::move(params), update_fn, cancel_fn, wait_fn);
}

int QidiPrinterAgent::start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    if (!apply_box_mapping(params))
        return BAMBU_NETWORK_ERR_PRINT_WR_UPLOAD_FTP_FAILED;
    return MoonrakerPrinterAgent::start_local_print_with_record(std::move(params), update_fn, cancel_fn, wait_fn);
}

int QidiPrinterAgent::start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    if (!apply_box_mapping(params))
        return BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED;
    return MoonrakerPrinterAgent::start_sdcard_print(std::move(params), update_fn, cancel_fn);
}

bool QidiPrinterAgent::fetch_slot_info(const std::string&        base_url,
                                       const std::string&        api_key,
                                       const QidiFilamentDict&   dict,
                                       const std::string&        series_id,
                                       std::vector<AmsTrayData>& trays,
                                       int&                      box_count,
                                       std::string&              error)
{
    std::string url = join_url(base_url, "/printer/objects/query?save_variables=variables");
    for (int i = 0; i < 16; ++i) {
        url += "&box_stepper%20slot" + std::to_string(i) + "=runout_button";
    }

    std::string response_body;
    bool        success = false;
    std::string http_error;

    auto http = Http::get(url);
    if (!api_key.empty()) {
        http.header("X-Api-Key", api_key);
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
        error = http_error.empty() ? "Connection failed" : http_error;
        return false;
    }

    nlohmann::json status;
    nlohmann::json variables;
    if (!parse_slot_response(response_body, status, variables, error))
        return false;

    box_count = variables.value("box_count", 1);
    if (box_count < 0) {
        box_count = 0;
    }

    const int max_slots = box_count * 4;
    trays.clear();
    trays.reserve(max_slots);

    // Lambda to build setting_id from slot data
    auto build_setting_id = [&](int filament_type_idx, int vendor_type, const std::string& tray_type) {
        const int vendor = (vendor_type == 1) ? 1 : 0;
        if (is_numeric(series_id) && filament_type_idx > 0) {
            return "QD_" + series_id + "_" + std::to_string(vendor) + "_" + std::to_string(filament_type_idx);
        }
        return map_filament_type_to_setting_id(tray_type);
    };

    for (int i = 0; i < max_slots; ++i) {
        AmsTrayData tray;
        tray.slot_index = i;

        // Read slot variables
        const int color_index     = variables.value("color_slot" + std::to_string(i), 1);
        const int filament_type   = variables.value("filament_slot" + std::to_string(i), 1);
        const int vendor_type     = variables.value("vendor_slot" + std::to_string(i), 0);

        // Check filament presence via runout sensor
        std::string box_stepper_key = "box_stepper slot" + std::to_string(i);
        tray.has_filament = false;
        if (status.contains(box_stepper_key)) {
            auto& box_stepper = status[box_stepper_key];
            if (box_stepper.contains("runout_button") && !box_stepper["runout_button"].is_null()) {
                int runout_button = box_stepper["runout_button"].template get<int>();
                tray.has_filament = (runout_button == 0);
            }
        }

        if (tray.has_filament) {
            // Look up filament type name from dictionary
            std::string filament_name = "PLA";
            auto filament_it = dict.filaments.find(filament_type);
            if (filament_it != dict.filaments.end()) {
                filament_name = filament_it->second;
            }
            tray.tray_type = normalize_filament_type(filament_name);

            // Try Qidi-specific setting ID first; fall back to visible preset by type
            std::string setting_id = build_setting_id(filament_type, vendor_type, tray.tray_type);
            auto* bundle = GUI::wxGetApp().preset_bundle;
            if (!bundle) {
                tray.tray_info_idx = setting_id;
            } else if (!setting_id.empty() && has_visible_base_preset(bundle->filaments, setting_id)) {
                tray.tray_info_idx = setting_id;
            } else {
                tray.tray_info_idx = bundle->filaments.filament_id_by_type(tray.tray_type);
            }

            // Look up color from dictionary
            auto color_it = dict.colors.find(color_index);
            if (color_it != dict.colors.end()) {
                tray.tray_color = color_it->second;
            } else {
                tray.tray_color = "FFFFFFFF";
            }
        }

        trays.push_back(tray);
    }

    return true;
}

bool QidiPrinterAgent::parse_slot_response(const std::string& response_body,
                                           nlohmann::json&    status,
                                           nlohmann::json&    variables,
                                           std::string&       error)
{
    auto json = nlohmann::json::parse(response_body, nullptr, false, true);
    if (json.is_discarded()) {
        error = "Invalid JSON response";
        return false;
    }

    if (!json.is_object() || !json.contains("result") || !json["result"].is_object() || !json["result"].contains("status") ||
        !json["result"]["status"].is_object() || !json["result"]["status"].contains("save_variables") ||
        !json["result"]["status"]["save_variables"].is_object() || !json["result"]["status"]["save_variables"].contains("variables") ||
        !json["result"]["status"]["save_variables"]["variables"].is_object()) {
        // why: Qidi firmware may send null here, but json::value() throws for it.
        error = "Unexpected JSON structure: save_variables.variables must be an object";
        return false;
    }

    status    = json["result"]["status"];
    variables = status["save_variables"]["variables"];
    return true;
}

bool QidiPrinterAgent::fetch_filament_dict(const std::string& base_url,
                                           const std::string& api_key,
                                           QidiFilamentDict& dict,
                                           std::string& error) const
{
    std::string url = join_url(base_url, "/server/files/config/officiall_filas_list.cfg");

    std::string response_body;
    bool        success = false;
    std::string http_error;

    auto http = Http::get(url);
    if (!api_key.empty()) {
        http.header("X-Api-Key", api_key);
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
        error = http_error.empty() ? "Connection failed" : http_error;
        return false;
    }

    dict.colors.clear();
    dict.filaments.clear();
    parse_ini_section(response_body, "colordict", dict.colors);
    parse_filament_sections(response_body, dict.filaments);

    return !dict.colors.empty();
}

void QidiPrinterAgent::parse_ini_section(const std::string& content, const std::string& section_name, std::map<int, std::string>& result)
{
    std::istringstream stream(content);
    std::string        line;
    bool               in_section     = false;
    std::string        section_header = "[" + section_name + "]";

    while (std::getline(stream, line)) {
        boost::trim(line);
        if (!line.empty() && line[0] == '[') {
            in_section = (line == section_header);
            continue;
        }
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }
        if (in_section) {
            auto pos = line.find('=');
            if (pos != std::string::npos) {
                std::string key   = line.substr(0, pos);
                std::string value = line.substr(pos + 1);
                boost::trim(key);
                boost::trim(value);
                try {
                    int index     = std::stoi(key);
                    result[index] = value;
                } catch (...) {}
            }
        }
    }
}

void QidiPrinterAgent::parse_filament_sections(const std::string& content, std::map<int, std::string>& result)
{
    std::istringstream stream(content);
    std::string        line;
    int                current_fila_index = -1;

    while (std::getline(stream, line)) {
        boost::trim(line);
        if (!line.empty() && line[0] == '[') {
            current_fila_index = -1;
            if (line.size() > 5 && line.substr(0, 5) == "[fila" && line.back() == ']') {
                std::string num_str = line.substr(5, line.size() - 6);
                try {
                    current_fila_index = std::stoi(num_str);
                } catch (...) {
                    current_fila_index = -1;
                }
            }
            continue;
        }
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }
        if (current_fila_index > 0) {
            auto pos = line.find('=');
            if (pos != std::string::npos) {
                std::string key   = line.substr(0, pos);
                std::string value = line.substr(pos + 1);
                boost::trim(key);
                boost::trim(value);
                if (key == "filament") {
                    result[current_fila_index] = value;
                }
            }
        }
    }
}

std::string QidiPrinterAgent::map_filament_type_to_setting_id(const std::string& filament_type)
{
    const std::string upper = trim_and_upper(filament_type);

    if (upper == "PLA") {
        return "QD_1_0_1";
    }
    if (upper == "ABS") {
        return "QD_1_0_11";
    }
    if (upper == "PETG") {
        return "QD_1_0_41";
    }
    if (upper == "TPU") {
        return "QD_1_0_50";
    }
    return "";
}

std::string QidiPrinterAgent::normalize_model_key(std::string value)
{
    boost::algorithm::to_lower(value);
    std::string normalized;
    normalized.reserve(value.size());
    for (unsigned char c : value) {
        if (std::isalnum(c)) {
            normalized.push_back(static_cast<char>(c));
        }
    }
    return normalized;
}

std::string QidiPrinterAgent::infer_series_id(const std::string& model_id, const std::string& dev_name)
{
    std::string source = model_id.empty() ? dev_name : model_id;
    boost::trim(source);
    if (source.empty()) {
        return "";
    }
    if (is_numeric(source)) {
        return source;
    }

    const std::string key = normalize_model_key(source);
    if (key.find("q2") != std::string::npos) {
        return "1";
    }
    if (key.find("xmax") != std::string::npos && key.find("4") != std::string::npos) {
        return "3";
    }
    if ((key.find("xplus") != std::string::npos || key.find("plus") != std::string::npos) && key.find("4") != std::string::npos) {
        return "0";
    }
    return "";
}

std::string QidiPrinterAgent::normalize_filament_type(const std::string& filament_type)
{
    const std::string upper = trim_and_upper(filament_type);

    if (upper.find("PLA") != std::string::npos)
        return "PLA";
    if (upper.find("ABS") != std::string::npos)
        return "ABS";
    if (upper.find("PETG") != std::string::npos)
        return "PETG";
    if (upper.find("TPU") != std::string::npos)
        return "TPU";
    if (upper.find("ASA") != std::string::npos)
        return "ASA";
    if (upper.find("PA") != std::string::npos || upper.find("NYLON") != std::string::npos)
        return "PA";
    if (upper.find("PC") != std::string::npos)
        return "PC";
    if (upper.find("PVA") != std::string::npos)
        return "PVA";

    return upper;
}

} // namespace Slic3r
