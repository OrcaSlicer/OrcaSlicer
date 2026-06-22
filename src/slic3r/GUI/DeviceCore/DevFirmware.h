#pragma once
#include <nlohmann/json.hpp>
#include <wx/string.h>
#include "slic3r/Utils/json_diff.hpp"
#include <vector>
#include <string>

namespace Slic3r {

class MachineObject;

enum PrinterFirmwareType
{
    FIRMWARE_TYPE_ENGINEER = 0,
    FIRMWARE_TYPE_PRODUCTION,
    FIRMEARE_TYPE_UKNOWN,
};

class FirmwareInfo
{
public:
    std::string module_type;    // e.g., ota or ams
    std::string version;
    std::string url;            // Holds the local file path for offline updates (.hex or .zip)
    std::string name;
    std::string description;
};

class DevFirmwareVersionInfo
{
public:
    std::string name;
    wxString    product_name;
    std::string sn;
    std::string hw_ver;
    std::string sw_ver;
    std::string sw_new_ver;
    int         firmware_flag = 0;

public:
    bool isValid() const { return !sn.empty(); }
    bool isAirPump() const { return product_name.Contains("Air Pump"); }
    bool isLaszer() const { return product_name.Contains("Laser"); }
    bool isCuttingModule() const { return product_name.Contains("Cutting Module"); }
    bool isExtinguishSystem() const { return product_name.Contains("Extinguishing System"); }
};

class DevFirmware
{
private:
    MachineObject* m_owner;

public:
    DevFirmware(MachineObject* obj) : m_owner(obj) {}

    // --- Local Firmware Registry ---
    std::vector<FirmwareInfo> get_local_firmwares() const;
    bool has_local_firmware() const;

    // --- AVRDUDE USB Flashing Routine for Legacy MakerBot/UltiMaker ---
    // Executes the avrdude binary bundled in the resources folder
    static bool flash_via_usb(const std::string& hex_path, const std::string& serial_port, std::string& output_log);
};

} // namespace Slic3r