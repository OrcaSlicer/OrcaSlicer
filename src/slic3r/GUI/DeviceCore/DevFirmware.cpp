#include "DevFirmware.h"
#include "slic3r/GUI/DeviceManager.hpp"
#include "libslic3r/Utils.hpp" 
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <fstream>
#include <wx/utils.h>
#include <wx/process.h>

namespace Slic3r {

std::vector<FirmwareInfo> DevFirmware::get_local_firmwares() const {
    std::vector<FirmwareInfo> firmwares;

    if (!m_owner) return firmwares;

    // 1. Get the currently selected printer ID
    std::string current_printer_id = m_owner->printer_type; // e.g., "makerbot_replicator2"

    // 2. Build the path to the resources/firmware directory
    boost::filesystem::path fw_base_dir = boost::filesystem::path(Slic3r::resources_dir()) / "firmware";

    if (!boost::filesystem::exists(fw_base_dir)) {
        return firmwares;
    }

    // 3. Scan all subdirectories for the firmware_manifest.json
    for (auto& entry : boost::filesystem::recursive_directory_iterator(fw_base_dir)) {
        if (entry.path().filename() == "firmware_manifest.json") {
            try {
                std::ifstream ifs(entry.path().string());
                nlohmann::json j;
                ifs >> j;

                std::string manifest_internal_id = j.value("internal_id", "");

                // Check if the manifest matches the current printer
                if (current_printer_id == manifest_internal_id || current_printer_id.find(manifest_internal_id) != std::string::npos) {
                    
                    // Matching directory found! Load the available firmwares.
                    if (j.contains("firmwares") && j["firmwares"].is_array()) {
                        for (const auto& fw_json : j["firmwares"]) {
                            FirmwareInfo info;
                            info.module_type = fw_json.value("type", "stock"); 
                            info.version = fw_json.value("version", "");
                            info.name = fw_json.value("name", "");
                            info.description = fw_json.value("description", "");

                            // Build the absolute path to the .hex/.zip file and store it in the url field
                            boost::filesystem::path fw_file_path = entry.path().parent_path() / fw_json.value("file", "");
                            info.url = fw_file_path.string(); 

                            firmwares.push_back(info);
                        }
                    }
                    // Stop searching since we found the correct printer manifest
                    break; 
                }
            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(error) << "Error parsing firmware_manifest.json: " << e.what();
            }
        }
    }

    return firmwares;
}

bool DevFirmware::has_local_firmware() const {
    return !get_local_firmwares().empty();
}

// -----------------------------------------------------------------------------------------
// AVRDUDE Flash Wrapper for Legacy MakerBot and UltiMaker MightyBoards
// -----------------------------------------------------------------------------------------
bool DevFirmware::flash_via_usb(const std::string& hex_path, const std::string& serial_port, std::string& output_log) {
    if (serial_port.empty() || hex_path.empty()) {
        output_log = "Error: Serial port or firmware file path is missing.";
        return false;
    }

    if (!boost::filesystem::exists(hex_path)) {
        output_log = "Error: Firmware hex file not found at " + hex_path;
        return false;
    }

    // Resolve bundled avrdude binary from Orca's resource directory
    std::string avrdude_dir = Slic3r::resources_dir() + "/avrdude";
    std::string avrdude_exec = avrdude_dir + "/avrdude";
    
#ifdef _WIN32
    avrdude_exec += ".exe";
#endif

    std::string avrdude_conf = avrdude_dir + "/avrdude.conf";

    if (!boost::filesystem::exists(avrdude_exec) || !boost::filesystem::exists(avrdude_conf)) {
        output_log = "Error: avrdude binary or configuration file is missing in resources.";
        return false;
    }

    // Construct the avrdude execution command 
    // Defaults targeted for MakerBot MightyBoard RevE (ATmega2560 via stk500v2 protocol)
    wxString cmd = wxString::Format(
        "\"%s\" -C \"%s\" -v -p atmega2560 -c stk500v2 -P \"%s\" -b 115200 -D -U flash:w:\"%s\":i",
        avrdude_exec, avrdude_conf, serial_port, hex_path
    );

    BOOST_LOG_TRIVIAL(info) << "Executing firmware flash command: " << cmd.ToStdString();

    wxArrayString stdout_arr;
    wxArrayString stderr_arr;
    
    // Execute synchronously. UI should ideally run this in an async thread, 
    // but wxEXEC_SYNC prevents multiple concurrent flash attempts.
    long result = wxExecute(cmd, stdout_arr, stderr_arr, wxEXEC_SYNC | wxEXEC_NODISABLE);

    // Aggregate output for UI logging
    for (const auto& line : stdout_arr) {
        output_log += line.ToStdString() + "\n";
    }
    for (const auto& line : stderr_arr) {
        // avrdude writes most of its progress info to stderr
        output_log += line.ToStdString() + "\n"; 
    }

    if (result != 0) {
        BOOST_LOG_TRIVIAL(error) << "Firmware flash failed. Avrdude exit code: " << result;
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << "Firmware flash completed successfully.";
    return true;
}

} // namespace Slic3r