#pragma once
#include "libslic3r/CommonDefs.hpp"

#include "slic3r/Utils/json_diff.hpp"
#include <wx/string.h>
#include <map>

namespace Slic3r
{
// Previous definitions
class MachineObject;
class DevHMSItem;

class DevHMS
{
public:
    DevHMS(MachineObject* obj) : m_object(obj) {}

public:
    void                           ParseHMSItems(const json& hms_json);
    const std::vector<DevHMSItem>& GetHMSItems() const { return m_hms_list; };

private:
    MachineObject* m_object = nullptr;

    // all hms for this machine
    std::vector<DevHMSItem>  m_hms_list;
};

enum HMSMessageLevel
{
    HMS_UNKNOWN = 0,
    HMS_FATAL = 1,
    HMS_SERIOUS = 2,
    HMS_COMMON = 3,
    HMS_INFO = 4,
    HMS_MSG_LEVEL_MAX,
};

enum ModuleID
{
    MODULE_UKNOWN = 0x00,
    MODULE_01 = 0x01,
    MODULE_02 = 0x02,
    MODULE_MC = 0x03,
    MODULE_04 = 0x04,
    MODULE_MAINBOARD = 0x05,
    MODULE_06 = 0x06,
    MODULE_AMS = 0x07,
    MODULE_TH = 0x08,
    MODULE_09 = 0x09,
    MODULE_10 = 0x0A,
    MODULE_11 = 0x0B,
    MODULE_XCAM = 0x0C,
    MODULE_13 = 0x0D,
    MODULE_14 = 0x0E,
    MODULE_15 = 0x0F,
    MODULE_MAX = 0x10
};

class DevHMSItem
{
public:
    std::string get_long_error_code() const;
    HMSMessageLevel get_level() const { return m_msg_level; }

    void set_read() { m_already_read = true; };
    bool has_read() const { return m_already_read; };

protected:
    friend void DevHMS::ParseHMSItems(const json& hms_json);
    bool parse_hms_info(unsigned attr, unsigned code);

private:
    ModuleID        m_module_id;
    unsigned        m_module_num;
    unsigned        m_part_id;
    unsigned        m_reserved;
    HMSMessageLevel m_msg_level = HMS_UNKNOWN;
    int             m_msg_code = 0;
    bool            m_already_read = false;
};

};// End of namespace Slic3r