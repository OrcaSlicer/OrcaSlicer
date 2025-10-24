//#include "D:/dev/bamboo_slicer/build_release/src/slic3r/CMakeFiles/libslic3r_gui.dir/Release/cmake_pch.hxx"
#include "DevHMS.h"

namespace Slic3r
{

bool DevHMSItem::parse_hms_info(unsigned attr, unsigned code)
{
    bool result = true;
    unsigned int model_id_int = (attr >> 24) & 0xFF;
    this->m_module_id = (ModuleID)model_id_int;
    this->m_module_num = (attr >> 16) & 0xFF;
    this->m_part_id = (attr >> 8) & 0xFF;
    this->m_reserved = (attr >> 0) & 0xFF;
    unsigned msg_level_int = code >> 16;
    if (msg_level_int < (unsigned)HMS_MSG_LEVEL_MAX)
    {
        this->m_msg_level = (HMSMessageLevel)msg_level_int;
    }
    else
    {
        this->m_msg_level = HMS_UNKNOWN;
    }

    this->m_msg_code = code & 0xFFFF;
    return result;
}

std::string DevHMSItem::get_long_error_code() const
{
    char buf[64];
    ::sprintf(buf, "%02X%02X%02X00000%1X%04X",
        this->m_module_id,
        this->m_module_num,
        this->m_part_id,
        (int)this->m_msg_level,
        this->m_msg_code);
    return std::string(buf);
}

void DevHMS::ParseHMSItems(const json& hms_json)
{
    m_hms_list.clear();

    try
    {
        if (hms_json.is_array())
        {
            for (auto it = hms_json.begin(); it != hms_json.end(); it++)
            {
                DevHMSItem item;
                if ((*it).contains("attr") && (*it).contains("code"))
                {
                    unsigned attr = (*it)["attr"].get<unsigned>();
                    unsigned code = (*it)["code"].get<unsigned>();
                    item.parse_hms_info(attr, code);
                }
                m_hms_list.push_back(item);
            }
        }
    }
    catch (const std::exception&)
    {
        assert(false && "Parse HMS items failed");
    }
}
}