#pragma once
#include <nlohmann/json.hpp>
#include <wx/string.h>
#include "slic3r/Utils/json_diff.hpp"

namespace Slic3r {

//Previous definitions
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
    std::string module_type;    // ota or ams
    std::string version;
    std::string url;
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
public:
    DevFirmware(MachineObject* obj) : m_owner(obj) {}

private:
    MachineObject* m_owner = nullptr;
};

} // namespace Slic3r