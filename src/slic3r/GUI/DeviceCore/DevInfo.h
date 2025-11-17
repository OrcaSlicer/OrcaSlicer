#pragma once
#include <nlohmann/json.hpp>
#include "slic3r/Utils/json_diff.hpp"
#include <wx/string.h>

namespace Slic3r {

class MachineObject;

/* some static info of machine*/ /*TODO*/
class DevInfo
{
public:
    DevInfo(MachineObject* obj) : m_owner(obj) {};

public:
    //std::string GetDevName() const { return m_dev_name; }
    //std::string GetDevId() const { return m_dev_id; }
    //std::string GetDevIP() const { return m_dev_ip; }
    //std::string GetPrinterTypeStr() const { return m_printer_type_str; }
    //std::string GetPrinterSignal() const { return m_printer_signal; }
    //std::string GetConnectType() const { return m_connect_type; }
    //std::string GetBindState() const { return m_bind_state; }

private:
    //std::string m_dev_name;
    //std::string m_dev_id;
    //std::string m_dev_ip;
    //std::string m_printer_type_str;
    //std::string m_printer_signal;
    //std::string m_connect_type;
    //std::string m_bind_state;

    MachineObject* m_owner = nullptr;
};

} // namespace Slic3r