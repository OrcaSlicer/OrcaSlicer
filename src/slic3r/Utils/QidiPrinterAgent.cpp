#include "QidiPrinterAgent.hpp"
#include "FilamentMatcher.hpp"
#include "Http.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include "nlohmann/json.hpp"
#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>
#include <cctype>
#include <sstream>

namespace Slic3r {

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
    trays.clear();
    trays.reserve(max_slots);

    for (int i = 0; i < max_slots; ++i) {
        AmsTrayData tray;
        tray.slot_index = i;

        const int color_index     = variables.value("color_slot" + std::to_string(i), 1);
        const int filament_type   = variables.value("filament_slot" + std::to_string(i), 1);
        const int vendor_type     = variables.value("vendor_slot" + std::to_string(i), 0);

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
            std::string filament_name = "PLA";
            auto filament_it = dict.filaments.find(filament_type);
            if (filament_it != dict.filaments.end()) {
                filament_name = filament_it->second;
            }
            // Prefer a filament_type the loaded profiles actually define, so a
            // composite survives ("PLA-CF" stays PLA-CF instead of collapsing
            // onto PLA and being excluded from its own profiles).  The static
            // ladder below is the fallback for an unrecognized material, and for
            // early init before any bundle exists.
            auto* preset_bundle = GUI::wxGetApp().preset_bundle;
            tray.tray_type      = preset_bundle ? FilamentMatcher::resolve_filament_type(
                                                      preset_bundle->filaments, filament_name)
                                                : std::string();
            if (tray.tray_type.empty())
                tray.tray_type = normalize_filament_type(filament_name);

            // Look up color hex from dictionary
            std::string color_hex;
            auto color_it = dict.colors.find(color_index);
            if (color_it != dict.colors.end()) {
                tray.tray_color = color_it->second;
                color_hex = color_it->second;
                if (!color_hex.empty() && color_hex[0] == '#')
                    color_hex = color_hex.substr(1);
            } else {
                tray.tray_color = "FFFFFFFF";
            }

            // Populate every piece of data QIDI reports for the shared matcher.
            // QIDI gives both forms of the vendor (name + numeric id) and the
            // filament (name + numeric index) plus the base type and color, so
            // all of V/F/C/P participate.
            //
            // Example: X-Max 4, filament vendor 7 = "Acme Inc",
            //          fila 52 = "PLA Plus", color 52 = "#FAFAFA"
            //   prefix        = "QD_3"
            //   vendor_name   = "Acme_Inc",  vendor_type  = 7
            //   filament_name = "PLA_Plus",  filament_idx = 52
            //   tray_type     = "PLA",       color        = "FAFAFA"
            FilamentMatchInput match_input;
            match_input.prefix        = series_id.empty() ? "QD" : ("QD_" + series_id);
            match_input.color         = color_hex;
            match_input.vendor_type   = vendor_type;
            match_input.filament_idx  = filament_type;
            match_input.tray_type     = tray.tray_type;

            auto vendor_it = dict.vendors.find(vendor_type);
            if (vendor_it != dict.vendors.end())
                match_input.vendor_name = FilamentMatcher::sanitize_for_id(vendor_it->second);

            match_input.filament_name = FilamentMatcher::sanitize_for_id(filament_name);

            auto match = FilamentMatcher::resolve(
                preset_bundle ? &preset_bundle->filaments : nullptr, match_input);
            tray.tray_info_idx       = match.filament_id;
            tray.matched_preset_name = match.preset_name;
        }

        trays.push_back(tray);
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
    dict.vendors.clear();
    parse_ini_section(response_body, "colordict", dict.colors);
    parse_filament_sections(response_body, dict.filaments);
    parse_ini_section(response_body, "vendor_list", dict.vendors);

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
