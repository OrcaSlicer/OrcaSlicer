# GUI/DeviceCore
# usage -- none GUI structure about device
# date -- 2025.07.14
# status -- Building

list(APPEND SLIC3R_GUI_SOURCES
    GUI/DeviceCore/DevBed.cpp
    GUI/DeviceCore/DevBed.h
    GUI/DeviceCore/DevConfig.h
    GUI/DeviceCore/DevConfig.cpp
    GUI/DeviceCore/DevConfigUtil.h
    GUI/DeviceCore/DevConfigUtil.cpp
    GUI/DeviceCore/DevCtrl.h
    GUI/DeviceCore/DevCtrl.cpp
    GUI/DeviceCore/DevDefs.h
    GUI/DeviceCore/DevExtensionTool.h
    GUI/DeviceCore/DevExtensionTool.cpp
    GUI/DeviceCore/DevExtruderSystem.h
    GUI/DeviceCore/DevExtruderSystem.cpp
    GUI/DeviceCore/DevExtruderSystemCtrl.cpp
    GUI/DeviceCore/DevFan.cpp
    GUI/DeviceCore/DevFan.h
    GUI/DeviceCore/DevFilaAmsSetting.h
    GUI/DeviceCore/DevFilaAmsSetting.cpp
    GUI/DeviceCore/DevFilaAmsSettingCtrl.cpp
    GUI/DeviceCore/DevFilaBlackList.h
    GUI/DeviceCore/DevFilaBlackList.cpp
    GUI/DeviceCore/DevFilaSystem.h
    GUI/DeviceCore/DevFilaSystem.cpp
    GUI/DeviceCore/DevFilaSystemCtrl.cpp
    GUI/DeviceCore/DevFirmware.h
    GUI/DeviceCore/DevFirmware.cpp
    GUI/DeviceCore/DevPrintOptions.h
    GUI/DeviceCore/DevPrintOptions.cpp
    GUI/DeviceCore/DevPrintTaskInfo.h
    GUI/DeviceCore/DevPrintTaskInfo.cpp
    GUI/DeviceCore/DevHMS.h
    GUI/DeviceCore/DevHMS.cpp
    GUI/DeviceCore/DevStorage.h
    GUI/DeviceCore/DevStorage.cpp
    GUI/DeviceCore/DevInfo.h
    GUI/DeviceCore/DevInfo.cpp
    GUI/DeviceCore/DevLamp.h
    GUI/DeviceCore/DevLamp.cpp
    GUI/DeviceCore/DevLampCtrl.cpp
    GUI/DeviceCore/DevManager.h
    GUI/DeviceCore/DevManager.cpp
    GUI/DeviceCore/DevMapping.h
    GUI/DeviceCore/DevMapping.cpp
    GUI/DeviceCore/DevNozzleSystem.h
    GUI/DeviceCore/DevNozzleSystem.cpp
    GUI/DeviceCore/DevUtil.h
    GUI/DeviceCore/DevUtil.cpp

)
set(SLIC3R_GUI_SOURCES ${SLIC3R_GUI_SOURCES} PARENT_SCOPE)