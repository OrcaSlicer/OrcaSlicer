#include "QidiPrinterAgent.hpp"
#include "Http.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include "nlohmann/json.hpp"
#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>
#include <algorithm>
#include <cctype>
#include <sstream>

namespace Slic3r {

namespace {

constexpr int QIDI_MAX_BOX_COUNT = 4;
constexpr int QIDI_SLOTS_PER_BOX = 4;

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

std::string normalize_filament_name_for_match(const std::string& input)
{
    std::string normalized = input;
    boost::trim(normalized);
    if (const auto suffix_pos = normalized.find(" @"); suffix_pos != std::string::npos) {
        normalized = normalized.substr(0, suffix_pos);
    }

    std::string cleaned;
    cleaned.reserve(normalized.size());
    for (unsigned char c : normalized) {
        if (std::isalnum(c) || c == '-' || c == '+' || c == '/' || std::isspace(c)) {
            cleaned.push_back(static_cast<char>(std::toupper(c)));
        } else {
            cleaned.push_back(' ');
        }
    }

    std::string collapsed;
    collapsed.reserve(cleaned.size());
    bool prev_space = true;
    for (unsigned char c : cleaned) {
        if (std::isspace(c)) {
            if (!prev_space) {
                collapsed.push_back(' ');
            }
            prev_space = true;
        } else {
            collapsed.push_back(static_cast<char>(c));
            prev_space = false;
        }
    }
    boost::trim(collapsed);
    return collapsed;
}

bool filament_name_match_relaxed(const std::string& wanted, const std::string& candidate)
{
    return wanted == candidate || (!candidate.empty() && boost::starts_with(wanted, candidate + " "));
}

std::vector<std::string> vendor_match_candidates(std::string vendor)
{
    std::vector<std::string> candidates;
    boost::trim(vendor);
    if (vendor.empty()) {
        return candidates;
    }

    candidates.push_back(vendor);
    const auto first_space = vendor.find_first_of(" \t");
    if (first_space != std::string::npos) {
        std::string first = vendor.substr(0, first_space);
        boost::trim(first);
        if (!first.empty() && !boost::iequals(first, vendor)) {
            candidates.push_back(first);
        }
    }
    return candidates;
}

std::string filament_id_by_name(const PresetCollection& filaments,
                                const std::string&     filament_name,
                                const std::vector<std::string>& vendor_filters = {})
{
    if (filament_name.empty()) {
        return "";
    }

    const std::string wanted = normalize_filament_name_for_match(filament_name);
    std::vector<std::string> normalized_vendor_filters;
    for (const auto& vendor_filter : vendor_filters) {
        const std::string normalized_vendor = normalize_filament_name_for_match(vendor_filter);
        if (!normalized_vendor.empty()) {
            normalized_vendor_filters.push_back(normalized_vendor);
        }
    }

    for (size_t i = 0; i < filaments.size(); ++i) {
        const auto& preset = filaments.preset(i);
        if (!preset.is_visible || !preset.is_compatible || preset.filament_id.empty()) {
            continue;
        }
        if (!normalized_vendor_filters.empty()) {
            const std::string preset_vendor = normalize_filament_name_for_match(preset.config.opt_string("filament_vendor", 0u));
            if (std::find(normalized_vendor_filters.begin(), normalized_vendor_filters.end(), preset_vendor)
                == normalized_vendor_filters.end()) {
                continue;
            }
        }
        if (filament_name_match_relaxed(wanted, normalize_filament_name_for_match(preset.name))) {
            BOOST_LOG_TRIVIAL(info) << "QidiPrinterAgent: filament matcher matched requested='" << filament_name
                                    << "' to preset='" << preset.name << "' filament_id='" << preset.filament_id << "'";
            return preset.filament_id;
        }
    }
    return "";
}

std::string qidi_setting_id(const std::string& series_id, int filament_type_idx, int vendor_type, const std::string& tray_type)
{
    const int vendor = (vendor_type == 1) ? 1 : 0;
    if (!series_id.empty() && std::all_of(series_id.begin(), series_id.end(), [](unsigned char c) { return std::isdigit(c); })
        && filament_type_idx > 0) {
        return "QD_" + series_id + "_" + std::to_string(vendor) + "_" + std::to_string(filament_type_idx);
    }

    const std::string upper = normalize_filament_name_for_match(tray_type);
    if (upper == "PLA") return "QD_1_0_1";
    if (upper == "ABS") return "QD_1_0_11";
    if (upper == "PETG") return "QD_1_0_41";
    if (upper == "TPU") return "QD_1_0_50";
    return "";
}

std::string match_qidi_filament_id(const std::string& full_name,
                                   const std::string& material,
                                   const std::string& vendor,
                                   const std::string& setting_id,
                                   const std::string& tray_type)
{
    auto* bundle = GUI::wxGetApp().preset_bundle;
    if (!bundle) {
        return setting_id;
    }

    const auto vendor_candidates = vendor_match_candidates(vendor);
    auto match_with_vendor_prefix = [&](const std::string& suffix) -> std::string {
        if (suffix.empty()) {
            return "";
        }
        for (const auto& vendor_candidate : vendor_candidates) {
            const std::string requested = vendor_candidate + " " + suffix;
            std::string match_id = filament_id_by_name(bundle->filaments, requested, vendor_candidates);
            if (!match_id.empty()) {
                return match_id;
            }
        }
        return "";
    };

    std::string match_id = match_with_vendor_prefix(full_name);
    if (match_id.empty()) {
        match_id = match_with_vendor_prefix(material);
    }
    if (match_id.empty()) {
        match_id = filament_id_by_name(bundle->filaments, full_name, vendor_candidates);
    }
    if (match_id.empty()) {
        match_id = filament_id_by_name(bundle->filaments, material, vendor_candidates);
    }
    if (match_id.empty() && !setting_id.empty() && has_visible_base_preset(bundle->filaments, setting_id)) {
        match_id = setting_id;
    }
    if (match_id.empty()) {
        match_id = bundle->filaments.filament_id_by_type(tray_type);
    }
    return match_id;
}

bool http_get_text(const std::string& url, const std::string& api_key, std::string& response_body, std::string& error)
{
    bool success = false;
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
    }
    return success;
}

const nlohmann::json* object_member(const nlohmann::json& obj, const std::string& key)
{
    if (!obj.is_object() || !obj.contains(key) || !obj[key].is_object()) {
        return nullptr;
    }
    return &obj[key];
}

std::string json_string(const nlohmann::json* obj, const char* key)
{
    if (!obj || !obj->is_object() || !obj->contains(key) || !(*obj)[key].is_string()) {
        return "";
    }
    return (*obj)[key].get<std::string>();
}

int json_int_member(const nlohmann::json* obj, const std::string& key, int default_value, bool* found = nullptr)
{
    if (found) {
        *found = false;
    }
    if (!obj || !obj->is_object() || !obj->contains(key) || !(*obj)[key].is_number_integer()) {
        return default_value;
    }

    try {
        const int value = (*obj)[key].get<int>();
        if (found) {
            *found = true;
        }
        return value;
    } catch (...) {}
    return default_value;
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

    std::string series_id;
    {
        MoonrakerDeviceInfo info;
        if (fetch_device_info(device_info.base_url, device_info.api_key, info, error)) {
            series_id = infer_series_id(info.model_id, info.dev_name);
        }
    }
    if (series_id.empty()) {
        series_id = infer_series_id(device_info.model_id, device_info.model_name);
    }

    std::vector<AmsTrayData> trays;
    int                      box_count = 0;
    if (fetch_multi_color_controller_slot_info(series_id, trays, box_count, error)) {
        build_ams_payload(box_count, box_count * QIDI_SLOTS_PER_BOX - 1, trays);
        return true;
    }
    BOOST_LOG_TRIVIAL(info) << "QidiPrinterAgent::fetch_filament_info: multi_color_controller path unavailable: " << error;

    QidiFilamentDict dict;
    if (!fetch_filament_dict(dict, error)) {
        BOOST_LOG_TRIVIAL(warning) << "QidiPrinterAgent::fetch_filament_info: Failed to fetch filament dict: " << error;
    }

    if (!fetch_slot_info(dict, series_id, trays, box_count, error)) {
        BOOST_LOG_TRIVIAL(warning) << "QidiPrinterAgent::fetch_filament_info: Failed to fetch slot info: " << error;
        return false;
    }

    build_ams_payload(box_count, box_count * QIDI_SLOTS_PER_BOX - 1, trays);
    return true;
}

bool QidiPrinterAgent::fetch_multi_color_controller_slot_info(const std::string&        series_id,
                                                             std::vector<AmsTrayData>& trays,
                                                             int&                      box_count,
                                                             std::string&              error)
{
    std::string url = join_url(device_info.base_url, "/printer/objects/query?multi_color_controller");

    std::string response_body;
    if (!http_get_text(url, device_info.api_key, response_body, error)) {
        return false;
    }

    auto json = nlohmann::json::parse(response_body, nullptr, false, true);
    if (json.is_discarded()) {
        error = "Invalid JSON response";
        return false;
    }

    const auto* result = object_member(json, "result");
    const auto* status = result ? object_member(*result, "status") : nullptr;
    const auto* mcc    = status ? object_member(*status, "multi_color_controller") : nullptr;
    if (!mcc) {
        error = "Unexpected JSON structure: missing multi_color_controller status";
        return false;
    }

    const auto* hardware = object_member(*mcc, "hardware");
    const auto* config   = object_member(*mcc, "config");
    bool has_box_count = false;
    box_count = json_int_member(hardware, "box_count", 0, &has_box_count);
    if (!has_box_count) {
        box_count = json_int_member(config, "box_count", 0, &has_box_count);
    }
    if (!has_box_count) {
        error = "multi_color_controller did not report box_count";
        return false;
    }
    if (box_count <= 0) {
        box_count = 0;
        trays.clear();
        return true;
    }
    box_count = std::min(box_count, QIDI_MAX_BOX_COUNT);

    const int max_slots = box_count * QIDI_SLOTS_PER_BOX;

    const auto* slots     = object_member(*mcc, "slots");
    const auto* states    = slots ? object_member(*slots, "states") : nullptr;
    const auto* materials = slots ? object_member(*slots, "materials") : nullptr;
    if (!states || states->empty()) {
        error = "multi_color_controller missing slot states";
        return false;
    }

    trays.clear();
    trays.reserve(max_slots);

    for (int i = 0; i < max_slots; ++i) {
        const std::string slot_key = "slot" + std::to_string(i);

        bool has_state = false;
        const int state = json_int_member(states, slot_key, 0, &has_state);
        bool has_filament = has_state && state > 0;

        AmsTrayData tray;
        tray.slot_index = i;
        tray.has_filament = has_filament;

        if (tray.has_filament) {
            const auto* slot_material = materials && materials->contains(slot_key) && (*materials)[slot_key].is_object()
                                            ? &(*materials)[slot_key] : nullptr;
            const auto* filament      = slot_material ? object_member(*slot_material, "filament") : nullptr;

            const std::string full_name = json_string(filament, "filament");
            const std::string material_type = json_string(filament, "type");
            const std::string material  = material_type.empty() ? full_name : material_type;
            const std::string vendor    = json_string(slot_material, "vendor");

            tray.tray_color = json_string(slot_material, "color");
            tray.tray_type = normalize_filament_type(material.empty() ? full_name : material);
            tray.tray_sub_brands = full_name;

            const int filament_type_idx = json_int_member(config, "filament_slot" + std::to_string(i), 0);
            const int vendor_type       = json_int_member(config, "vendor_slot" + std::to_string(i), 0);
            std::string setting_id;
            if (tray.tray_type.empty()) {
                error = "multi_color_controller loaded slot material metadata is incomplete";
                BOOST_LOG_TRIVIAL(warning) << "QidiPrinterAgent::fetch_multi_color_controller_slot_info: loaded " << slot_key
                                           << " has no material metadata; using legacy path";
                return false;
            }
            setting_id = qidi_setting_id(series_id, filament_type_idx, vendor_type, tray.tray_type);
            tray.tray_info_idx = match_qidi_filament_id(full_name, material, vendor, setting_id, tray.tray_type);

            BOOST_LOG_TRIVIAL(info) << "QidiPrinterAgent::fetch_multi_color_controller_slot_info: slot=" << i
                                    << " state=" << state << " material='" << material << "' name='" << full_name
                                    << "' vendor='" << vendor << "' setting_id='" << setting_id
                                    << "' tray_info_idx='" << tray.tray_info_idx << "'";
        }

        trays.push_back(tray);
    }

    return true;
}

bool QidiPrinterAgent::fetch_slot_info(const QidiFilamentDict&   dict,
                                       const std::string&        series_id,
                                       std::vector<AmsTrayData>& trays,
                                       int&                      box_count,
                                       std::string&              error)
{
    std::string url = join_url(device_info.base_url, "/printer/objects/query?save_variables=variables");
    for (int i = 0; i < QIDI_MAX_BOX_COUNT * QIDI_SLOTS_PER_BOX; ++i) {
        url += "&box_stepper%20slot" + std::to_string(i) + "=runout_button";
    }

    std::string response_body;
    if (!http_get_text(url, device_info.api_key, response_body, error)) {
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
    box_count = std::min(box_count, QIDI_MAX_BOX_COUNT);

    const int max_slots = box_count * QIDI_SLOTS_PER_BOX;
    trays.clear();
    trays.reserve(max_slots);

    for (int i = 0; i < max_slots; ++i) {
        AmsTrayData tray;
        tray.slot_index = i;

        const int color_index   = variables.value("color_slot" + std::to_string(i), 1);
        const int filament_type = variables.value("filament_slot" + std::to_string(i), 1);
        const int vendor_type   = variables.value("vendor_slot" + std::to_string(i), 0);

        std::string box_stepper_key = "box_stepper slot" + std::to_string(i);
        tray.has_filament = false;
        if (status.contains(box_stepper_key)) {
            auto& box_stepper = status[box_stepper_key];
            if (box_stepper.contains("runout_button") && !box_stepper["runout_button"].is_null()) {
                tray.has_filament = (box_stepper["runout_button"].get<int>() == 0);
            }
        }

        if (tray.has_filament) {
            std::string filament_name = "PLA";
            auto filament_it = dict.filaments.find(filament_type);
            if (filament_it != dict.filaments.end()) {
                filament_name = filament_it->second;
            }
            tray.tray_type = normalize_filament_type(filament_name);
            tray.tray_sub_brands = filament_name;

            const std::string setting_id = qidi_setting_id(series_id, filament_type, vendor_type, tray.tray_type);
            tray.tray_info_idx = match_qidi_filament_id("", "", "", setting_id, tray.tray_type);

            auto color_it = dict.colors.find(color_index);
            if (color_it != dict.colors.end()) {
                tray.tray_color = color_it->second;
            } else {
                tray.tray_color = "FFFFFFFF";
            }

            BOOST_LOG_TRIVIAL(info) << "QidiPrinterAgent::fetch_slot_info: slot=" << i
                                    << " material='" << filament_name
                                    << "' setting_id='" << setting_id
                                    << "' tray_info_idx='" << tray.tray_info_idx << "'";
        }

        trays.push_back(tray);
    }

    return true;
}

bool QidiPrinterAgent::fetch_filament_dict(QidiFilamentDict& dict, std::string& error) const
{
    std::string url = join_url(device_info.base_url, "/server/files/config/officiall_filas_list.cfg");

    std::string response_body;
    if (!http_get_text(url, device_info.api_key, response_body, error)) {
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
