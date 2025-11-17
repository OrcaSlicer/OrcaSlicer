#pragma once
#include "libslic3r/CommonDefs.hpp"

#include "slic3r/Utils/json_diff.hpp"
#include <wx/string.h>

namespace Slic3r
{

//Previous definitions
class MachineObject;


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