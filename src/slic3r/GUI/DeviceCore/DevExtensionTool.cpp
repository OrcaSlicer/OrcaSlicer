#include "DevExtensionTool.h"
#include "DevUtil.h"

#include <nlohmann/json.hpp>

using namespace nlohmann;

namespace Slic3r
{

DevExtensionTool::DevExtensionTool(MachineObject* obj) : m_owner(obj)
{
    m_mount_3dp = MOUNT_NOT_MOUNTED;
    m_calib = CALIB_NONE;
    m_tool_type = TOOL_TYPE_EMPTY;
}

void DevExtensionToolParser::ParseV2_0(const nlohmann::json& extension_tool_json, std::weak_ptr<DevExtensionTool> extension_tool)
{
    if (auto ext_tool = extension_tool.lock())
    {
        DevJsonValParser::ParseVal(extension_tool_json, "mount_3d", ext_tool->m_mount_3dp, ext_tool->m_mount_3dp);
        DevJsonValParser::ParseVal(extension_tool_json, "calib", ext_tool->m_calib, ext_tool->m_calib);

        {
            const std::string& type_str = DevJsonValParser::GetVal<std::string>(extension_tool_json, "type", "");
            static std::map<std::string, DevExtensionTool::ToolType> s_type_map = {
                {"CP00", DevExtensionTool::TOOL_TYPE_CUT_CP00},
                {"LB00", DevExtensionTool::TOOL_TYPE_LASER_LB00},
                {"F000", DevExtensionTool::TOOL_TYPE_FAN_F000}
            };

            auto iter = s_type_map.find(type_str);
            iter != s_type_map.end() ? ext_tool->m_tool_type = iter->second : DevExtensionTool::TOOL_TYPE_EMPTY;
        }
    }
}

}