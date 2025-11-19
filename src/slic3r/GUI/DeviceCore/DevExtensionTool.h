#pragma once
#include <optional>
#include "libslic3r/CommonDefs.hpp"

#include "slic3r/Utils/json_diff.hpp"
#include <wx/string.h>

#include "DevDefs.h"

namespace Slic3r
{

//Previous definitions
class MachineObject;

// some extension tools for toolheads
class DevExtensionTool
{
    friend class DevExtensionToolParser;
public:
    static std::shared_ptr<DevExtensionTool> Create(MachineObject* obj) { return std::shared_ptr<DevExtensionTool>(new DevExtensionTool(obj)); }

public:
    // tool type
    bool IsToolTypeFanF000() const { return m_tool_type == TOOL_TYPE_FAN_F000; }

    // mount state
    bool IsMounted() const { return m_mount_3dp == MOUNT_MOUNTED; }

protected:
    DevExtensionTool(MachineObject* obj);

private:
    MachineObject* m_owner = nullptr;

    enum MountState
    {
        MOUNT_NOT_MOUNTED = 0,
        MOUNT_MOUNTED = 1,
        MOUNT_NO_MODULE = 2,
        MOUNT_NO_CABLE = 3
    }  m_mount_3dp;

    enum CalibState
    {
        CALIB_NONE = 0,
        CALIB_FIRST = 1,
        CALIB_MOUNT = 2
    }  m_calib;

    enum ToolType
    {
        TOOL_TYPE_EMPTY = 0,
        TOOL_TYPE_CUT_CP00 = 1,
        TOOL_TYPE_LASER_LB00 = 2,
        TOOL_TYPE_FAN_F000 = 3,
    } m_tool_type;
};


class DevExtensionToolParser
{
public:
    static void ParseV2_0(const nlohmann::json& extension_tool_json, std::weak_ptr<DevExtensionTool> extension_tool);
};

};