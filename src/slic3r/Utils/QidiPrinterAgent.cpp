#include "QidiPrinterAgent.hpp"
#include "Http.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/DeviceCore/DevFilaSystem.h"
#include "slic3r/GUI/DeviceCore/DevManager.h"

#include "nlohmann/json.hpp"
#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>
#include <algorithm>
#include <cctype>
#include <sstream>

namespace Slic3r {

const std::string QidiPrinterAgent_VERSION = "0.0.1";

QidiPrinterAgent::QidiPrinterAgent(std::string log_dir) : MoonrakerPrinterAgent(std::move(log_dir))
{
}

AgentInfo QidiPrinterAgent::get_agent_info_static()
{
    return AgentInfo{.id = "qidi", .name = "Qidi Printer Agent", .version = QidiPrinterAgent_VERSION, .description = "Qidi printer agent"};
}

void QidiPrinterAgent::fetch_filament_info(std::string dev_id)
{
    // Look up MachineObject via DeviceManager
    auto* dev_manager = GUI::wxGetApp().getDeviceManager();
    if (!dev_manager) {
        BOOST_LOG_TRIVIAL(error) << "QidiPrinterAgent::fetch_filament_info: DeviceManager is null";
        return;
    }
    MachineObject* obj = dev_manager->get_my_machine(dev_id);
    if (!obj) {
        BOOST_LOG_TRIVIAL(error) << "QidiPrinterAgent::fetch_filament_info: MachineObject not found for dev_id=" << dev_id;
        return;
    }

    std::vector<QidiSlotInfo> slots;
    int                       box_count = 0;
    std::string               error;
    if (!fetch_slot_info(device_info.base_url, device_info.api_key, slots, box_count, error)) {
        BOOST_LOG_TRIVIAL(error) << "QidiPrinterAgent::fetch_filament_info: Failed to fetch slot info: " << error;
        return;
    }

    QidiFilamentDict dict;
    if (!fetch_filament_dict(device_info.base_url, device_info.api_key, dict, error)) {
        BOOST_LOG_TRIVIAL(warning) << "QidiPrinterAgent::fetch_filament_info: Failed to fetch filament dict: " << error;
    }

    std::string series_id;
    {
        MoonrakerDeviceInfo info;
        std::string    device_error;
        if (fetch_device_info(device_info.base_url, device_info.api_key, info, device_error)) {
            series_id = infer_series_id(info.model_id, info.dev_name);
        }
    }

    auto build_setting_id = [&](const QidiSlotInfo& slot, const std::string& tray_type) {
        const int vendor = (slot.vendor_type == 1) ? 1 : 0;
        if (is_numeric(series_id) && slot.filament_type > 0) {
            return "QD_" + series_id + "_" + std::to_string(vendor) + "_" + std::to_string(slot.filament_type);
        }
        return map_filament_type_to_setting_id(tray_type);
    };

    // Build BBL-format JSON for DevFilaSystemParser::ParseV1_0
    // The expected format matches BBL's print.push_status AMS subset
    nlohmann::json ams_json = nlohmann::json::object();
    nlohmann::json ams_array = nlohmann::json::array();

    // Calculate ams_exist_bits and tray_exist_bits
    unsigned long ams_exist_bits = 0;
    unsigned long tray_exist_bits = 0;

    for (int ams_id = 0; ams_id < box_count; ++ams_id) {
        ams_exist_bits |= (1 << ams_id);

        nlohmann::json ams_unit = nlohmann::json::object();
        ams_unit["id"] = std::to_string(ams_id);
        ams_unit["info"] = "2100";  // AMS_LITE type (2), main extruder (0)

        nlohmann::json tray_array = nlohmann::json::array();
        for (int slot_id = 0; slot_id < 4; ++slot_id) {
            const int          slot_index = ams_id * 4 + slot_id;
            const QidiSlotInfo slot       = slot_index < static_cast<int>(slots.size()) ? slots[slot_index] : QidiSlotInfo{};

            nlohmann::json tray_json = nlohmann::json::object();
            tray_json["id"] = std::to_string(slot_id);
            tray_json["tag_uid"] = "0000000000000000";

            if (slot.filament_exists) {
                tray_exist_bits |= (1 << slot_index);

                std::string filament_type = "PLA";
                auto        filament_it   = dict.filaments.find(slot.filament_type);
                if (filament_it != dict.filaments.end()) {
                    filament_type = filament_it->second;
                }
                std::string tray_type = normalize_filament_type(filament_type);
                std::string setting_id = build_setting_id(slot, tray_type);

                std::string color    = "FFFFFFFF";
                auto        color_it = dict.colors.find(slot.color_index);
                if (color_it != dict.colors.end()) {
                    color = normalize_color(color_it->second);
                }

                tray_json["tray_info_idx"] = setting_id;
                tray_json["tray_type"] = tray_type;
                tray_json["tray_color"] = color;
            } else {
                tray_json["tray_info_idx"] = "";
                tray_json["tray_type"] = "";
                tray_json["tray_color"] = "00000000";
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

    ams_json["ams"] = ams_array;
    ams_json["ams_exist_bits"] = ams_exist_ss.str();
    ams_json["tray_exist_bits"] = tray_exist_ss.str();

    // Wrap in the expected structure for ParseV1_0
    nlohmann::json print_json = nlohmann::json::object();
    print_json["ams"] = ams_json;

    // Call the parser to populate DevFilaSystem
    DevFilaSystemParser::ParseV1_0(print_json, obj, obj->GetFilaSystem(), false);
}

bool QidiPrinterAgent::fetch_slot_info(const std::string& base_url,
                                       const std::string& api_key,
                                       std::vector<QidiSlotInfo>& slots,
                                       int&                       box_count,
                                       std::string&               error) const
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
    http.timeout_connect(10)
        .timeout_max(30)
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

    auto json = nlohmann::json::parse(response_body, nullptr, false, true);
    if (json.is_discarded()) {
        error = "Invalid JSON response";
        return false;
    }

    if (!json.contains("result") || !json["result"].contains("status") || !json["result"]["status"].contains("save_variables") ||
        !json["result"]["status"]["save_variables"].contains("variables")) {
        error = "Unexpected JSON structure";
        return false;
    }

    auto& variables = json["result"]["status"]["save_variables"]["variables"];
    auto& status    = json["result"]["status"];

    box_count = variables.value("box_count", 1);
    if (box_count < 0) {
        box_count = 0;
    }

    const int max_slots = box_count * 4;
    slots.clear();
    slots.reserve(max_slots);

    for (int i = 0; i < max_slots; ++i) {
        QidiSlotInfo slot;
        slot.slot_index    = i;
        slot.color_index   = variables.value("color_slot" + std::to_string(i), 1);
        slot.filament_type = variables.value("filament_slot" + std::to_string(i), 1);
        slot.vendor_type   = variables.value("vendor_slot" + std::to_string(i), 0);

        std::string box_stepper_key = "box_stepper slot" + std::to_string(i);
        slot.filament_exists        = false;
        if (status.contains(box_stepper_key)) {
            auto& box_stepper = status[box_stepper_key];
            if (box_stepper.contains("runout_button") && !box_stepper["runout_button"].is_null()) {
                int runout_button    = box_stepper["runout_button"].get<int>();
                slot.filament_exists = (runout_button == 0);
            }
        }
        slots.push_back(slot);
    }

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
    http.timeout_connect(10)
        .timeout_max(30)
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

std::string QidiPrinterAgent::normalize_color(const std::string& color)
{
    std::string value = color;
    boost::trim(value);
    if (value.rfind("0x", 0) == 0 || value.rfind("0X", 0) == 0) {
        value = value.substr(2);
    }
    if (!value.empty() && value[0] == '#') {
        value = value.substr(1);
    }
    std::string normalized;
    for (char c : value) {
        if (std::isxdigit(static_cast<unsigned char>(c))) {
            normalized.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }
    }
    if (normalized.size() == 6) {
        normalized += "FF";
    }
    if (normalized.size() != 8) {
        return "00000000";
    }
    return normalized;
}

std::string QidiPrinterAgent::map_filament_type_to_setting_id(const std::string& filament_type)
{
    std::string upper = filament_type;
    boost::trim(upper);
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

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
    std::string trimmed = filament_type;
    boost::trim(trimmed);
    std::string upper = trimmed;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

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

    return trimmed;
}

} // namespace Slic3r
