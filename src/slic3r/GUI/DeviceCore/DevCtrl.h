#pragma once
#include "libslic3r/CommonDefs.hpp"

#include "slic3r/Utils/json_diff.hpp"
#include <wx/string.h>

namespace Slic3r
{

//Previous definitions
class MachineObject;

class DevCtrlInfo
{
public:
    DevCtrlInfo() {};
    DevCtrlInfo(MachineObject* obj, int sequence_id, const json& req_json, int interval_max = 3, int interval_min = 0);

public:
    bool CheckCanUpdateData(const nlohmann::json& jj);

private:
    void OnTimeOut()  { m_time_out = true;}
    void OnReceived() { m_received = true;} ;

private:
    bool m_time_out = false;
    bool m_received = false;

    std::string m_request_dev_id = "";

    time_t m_request_time = 0;
    int    m_request_seq = 0;
    json   m_request_json = json();

    // check
    int m_request_interval_max = 3;
    int m_request_interval_min = 0;
};

class DevCtrl
{
    MachineObject* m_obj;
public:
    DevCtrl(MachineObject* obj) : m_obj(obj) {};
    ~DevCtrl() = default;

public:
    /*extruder system*/
    int command_select_extruder(int id);
};

};