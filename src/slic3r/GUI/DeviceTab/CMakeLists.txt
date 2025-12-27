# GUI/DeviceTab
# usage -- GUI about device tab for BambuStudio
# date -- 2025.01.01
# status -- Building

list(APPEND SLIC3R_GUI_SOURCES
    GUI/DeviceTab/uiAmsHumidityPopup.h
    GUI/DeviceTab/uiAmsHumidityPopup.cpp
    GUI/DeviceTab/uiDeviceUpdateVersion.h
    GUI/DeviceTab/uiDeviceUpdateVersion.cpp
)
set(SLIC3R_GUI_SOURCES ${SLIC3R_GUI_SOURCES} PARENT_SCOPE)