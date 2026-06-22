#include "QidiPrinterAgent.hpp"
#include "Http.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include "nlohmann/json.hpp"
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <cctype>
#include <regex>
#include <set>
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

void replace_all(std::string& text, const std::string& from, const std::string& to)
{
    if (from.empty())
        return;
    size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
}

bool plate_entry_index(const std::string& name, int& index)
{
    static const std::regex pattern(R"(^Metadata/(?:(?:plate_)([0-9]+)(?:\.gcode(?:\.md5)?|(?:_small)?\.png|\.json)|(?:plate_no_light_|top_|pick_)([0-9]+)\.png)$)");
    std::smatch match;
    if (!std::regex_match(name, match, pattern))
        return false;

    index = std::stoi((match[1].matched ? match[1] : match[2]).str());
    return true;
}

bool selected_plate_block(const std::string& block, int selected_plate)
{
    const std::string selected = std::to_string(selected_plate);
    return block.find("key=\"plater_id\" value=\"" + selected + "\"") != std::string::npos ||
           block.find("key=\"index\" value=\"" + selected + "\"") != std::string::npos ||
           block.find("Metadata/plate_" + selected + ".gcode") != std::string::npos;
}

std::string keep_selected_plate_block(const std::string& xml, int selected_plate)
{
    static const std::regex plate_pattern(R"(<plate>[\s\S]*?</plate>)");
    std::string result;
    size_t last = 0;
    bool found_plate = false;
    bool kept_plate = false;

    for (std::sregex_iterator it(xml.begin(), xml.end(), plate_pattern), end; it != end; ++it) {
        const std::smatch& match = *it;
        found_plate = true;
        result.append(xml, last, match.position() - last);
        const std::string block = match.str();
        if (selected_plate_block(block, selected_plate)) {
            result += block;
            kept_plate = true;
        }
        last = match.position() + match.length();
    }

    if (!found_plate || !kept_plate)
        return xml;

    result.append(xml, last, std::string::npos);
    return result;
}

std::string normalize_xml_for_plate(std::string xml, int selected_plate)
{
    xml = keep_selected_plate_block(xml, selected_plate);

    const std::string selected = std::to_string(selected_plate);
    replace_all(xml, "plate_no_light_" + selected, "plate_no_light_1");
    replace_all(xml, "plate_" + selected, "plate_1");
    replace_all(xml, "top_" + selected, "top_1");
    replace_all(xml, "pick_" + selected, "pick_1");
    replace_all(xml, "key=\"plater_id\" value=\"" + selected + "\"", "key=\"plater_id\" value=\"1\"");
    replace_all(xml, "key=\"index\" value=\"" + selected + "\"", "key=\"index\" value=\"1\"");
    return xml;
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

bool QidiPrinterAgent::normalize_3mf_for_upload(const std::string& source_path, std::string& normalized_path)
{
    namespace fs = boost::filesystem;

    mz_zip_archive source;
    mz_zip_zero_struct(&source);
    if (!open_zip_reader(&source, source_path)) {
        BOOST_LOG_TRIVIAL(error) << "QidiPrinterAgent: failed to open 3MF: " << source_path;
        return false;
    }

    std::set<int> gcode_plates;
    const mz_uint file_count = mz_zip_reader_get_num_files(&source);
    for (mz_uint i = 0; i < file_count; ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&source, i, &stat))
            continue;
        int plate_index = 0;
        if (boost::ends_with(stat.m_filename, ".gcode") && plate_entry_index(stat.m_filename, plate_index))
            gcode_plates.insert(plate_index);
    }

    if (gcode_plates.size() != 1) {
        BOOST_LOG_TRIVIAL(error) << "QidiPrinterAgent: expected one 3MF gcode plate, found " << gcode_plates.size();
        close_zip_reader(&source);
        return false;
    }

    const int selected_plate = *gcode_plates.begin();
    fs::path temp_path = fs::temp_directory_path() / fs::unique_path("orcaslicer-qidi-%%%%-%%%%-%%%%.gcode.3mf");

    mz_zip_archive target;
    mz_zip_zero_struct(&target);
    if (!open_zip_writer(&target, temp_path.string())) {
        BOOST_LOG_TRIVIAL(error) << "QidiPrinterAgent: failed to create normalized 3MF: " << temp_path.string();
        close_zip_reader(&source);
        return false;
    }

    bool ok = true;
    for (mz_uint i = 0; i < file_count && ok; ++i) {
        if (mz_zip_reader_is_file_a_directory(&source, i))
            continue;

        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&source, i, &stat)) {
            ok = false;
            break;
        }

        std::string name = stat.m_filename;
        int plate_index = 0;
        if (plate_entry_index(name, plate_index)) {
            if (plate_index != selected_plate)
                continue;
            const std::string selected = std::to_string(selected_plate);
            replace_all(name, "plate_no_light_" + selected, "plate_no_light_1");
            replace_all(name, "plate_" + selected, "plate_1");
            replace_all(name, "top_" + selected, "top_1");
            replace_all(name, "pick_" + selected, "pick_1");
        }

        size_t size = 0;
        void* mem = mz_zip_reader_extract_to_heap(&source, i, &size, 0);
        if (mem == nullptr) {
            ok = false;
            break;
        }
        std::string data(static_cast<const char*>(mem), size);
        mz_free(mem);

        if (name == "Metadata/filament_sequence.json") {
            nlohmann::json sequence = nlohmann::json::parse(data, nullptr, false);
            const std::string selected_key = "plate_" + std::to_string(selected_plate);
            if (sequence.is_object() && sequence.contains(selected_key)) {
                nlohmann::json normalized_sequence;
                normalized_sequence["plate_1"] = sequence[selected_key];
                data = normalized_sequence.dump();
            }
        } else if (name == "3D/3dmodel.model" || name == "Metadata/model_settings.config" ||
                   name == "Metadata/slice_info.config" || boost::iends_with(name, ".rels"))
            data = normalize_xml_for_plate(std::move(data), selected_plate);

        if (!mz_zip_writer_add_mem(&target, name.c_str(), data.data(), data.size(), MZ_DEFAULT_COMPRESSION))
            ok = false;
    }

    if (ok && !mz_zip_writer_finalize_archive(&target))
        ok = false;

    close_zip_writer(&target);
    close_zip_reader(&source);

    if (!ok) {
        boost::system::error_code ec;
        fs::remove(temp_path, ec);
        BOOST_LOG_TRIVIAL(error) << "QidiPrinterAgent: failed to normalize 3MF for upload";
        return false;
    }

    normalized_path = temp_path.string();
    BOOST_LOG_TRIVIAL(info) << "QidiPrinterAgent: normalized 3MF plate " << selected_plate << " as plate 1: " << normalized_path;
    return true;
}

bool QidiPrinterAgent::normalize_3mf_for_upload_in_place(const std::string& path)
{
    std::string normalized_path;
    if (!normalize_3mf_for_upload(path, normalized_path))
        return false;

    boost::system::error_code copy_ec;
    boost::filesystem::copy_file(normalized_path, path, boost::filesystem::copy_option::overwrite_if_exists, copy_ec);
    boost::system::error_code remove_ec;
    boost::filesystem::remove(normalized_path, remove_ec);
    if (copy_ec) {
        BOOST_LOG_TRIVIAL(error) << "QidiPrinterAgent: failed to replace 3MF with normalized copy: " << copy_ec.message();
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << "QidiPrinterAgent: replaced 3MF with normalized copy: " << path;
    return true;
}

int QidiPrinterAgent::start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    if (!boost::iends_with(params.filename, ".3mf"))
        return MoonrakerPrinterAgent::start_send_gcode_to_sdcard(std::move(params), update_fn, cancel_fn, wait_fn);

    if (update_fn)
        update_fn(PrintingStageUpload, 0, "Preparing 3MF...");

    std::string normalized_path;
    if (!normalize_3mf_for_upload(params.filename, normalized_path))
        return BAMBU_NETWORK_ERR_PRINT_SG_UPLOAD_FTP_FAILED;

    std::string filename = !params.ftp_file.empty() ? params.ftp_file : params.task_name;
    if (filename.empty())
        filename = boost::filesystem::path(params.filename).filename().string();

    if (!boost::iends_with(filename, ".3mf"))
        filename += ".3mf";

    bool uploaded = upload_gcode(normalized_path, filename, device_info.base_url, device_info.api_key,
                                 update_fn, cancel_fn, "1");
    boost::system::error_code ec;
    boost::filesystem::remove(normalized_path, ec);
    if (!uploaded)
        return BAMBU_NETWORK_ERR_PRINT_SG_UPLOAD_FTP_FAILED;

    if (update_fn)
        update_fn(PrintingStageFinished, 100, "File uploaded");
    return BAMBU_NETWORK_SUCCESS;
}

int QidiPrinterAgent::start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    const std::string source = params.dst_file.empty() ? params.filename : params.dst_file;
    if (!boost::iends_with(source, ".3mf"))
        return MoonrakerPrinterAgent::start_local_print(std::move(params), update_fn, cancel_fn);

    if (cancel_fn && cancel_fn())
        return BAMBU_NETWORK_ERR_CANCELED;

    if (update_fn)
        update_fn(PrintingStageUpload, 0, "Preparing 3MF...");

    std::string normalized_path;
    if (!normalize_3mf_for_upload(source, normalized_path))
        return BAMBU_NETWORK_ERR_PRINT_LP_UPLOAD_FTP_FAILED;

    const std::string filename = sanitize_filename(boost::filesystem::path(source).filename().string());
    bool uploaded = upload_gcode(normalized_path, filename, device_info.base_url, device_info.api_key,
                                 update_fn, cancel_fn, "1");
    boost::system::error_code ec;
    boost::filesystem::remove(normalized_path, ec);

    if (!uploaded)
        return BAMBU_NETWORK_ERR_PRINT_LP_UPLOAD_FTP_FAILED;

    if (cancel_fn && cancel_fn())
        return BAMBU_NETWORK_ERR_CANCELED;

    if (update_fn)
        update_fn(PrintingStageSending, 0, "Starting print...");
    if (!send_gcode(device_info.dev_id, "SDCARD_PRINT_FILE FILENAME=" + filename))
        return BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED;

    if (update_fn)
        update_fn(PrintingStageFinished, 100, "Print started");
    return BAMBU_NETWORK_SUCCESS;
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
