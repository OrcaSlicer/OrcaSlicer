/**
* @file  DevDefs.h
* @brief Common definitions, macros, and constants for printer modules.
*        Enhance building and including.
*
* This file provides shared macros, constants, and enumerations for all printer-related
* modules, including printer types, status, binding states, connection types, and error codes.
* It is intended to be included wherever printer-specific definitions are required.
*/

#pragma once
#include <string>

enum PrinterArch
{
    ARCH_CORE_XY,
    ARCH_I3,
};

enum PrinterSeries
{
    SERIES_X1 = 0,
    SERIES_P1P,
    SERIES_UNKNOWN,
};

namespace Slic3r
{

/*AMS*/
enum AmsStatusMain
{
    AMS_STATUS_MAIN_IDLE = 0x00,
    AMS_STATUS_MAIN_FILAMENT_CHANGE = 0x01,
    AMS_STATUS_MAIN_RFID_IDENTIFYING = 0x02,
    AMS_STATUS_MAIN_ASSIST = 0x03,
    AMS_STATUS_MAIN_CALIBRATION = 0x04,
    AMS_STATUS_MAIN_SELF_CHECK = 0x10,
    AMS_STATUS_MAIN_DEBUG = 0x20,
    AMS_STATUS_MAIN_UNKNOWN = 0xFF,
};

// Slots and Tray
#define VIRTUAL_TRAY_MAIN_ID    255
#define VIRTUAL_TRAY_DEPUTY_ID  254

#define VIRTUAL_AMS_MAIN_ID_STR   "255"
#define VIRTUAL_AMS_DEPUTY_ID_STR "254"

#define INVALID_AMS_TEMPERATURE std::numeric_limits<float>::min()

/* Extruder*/
#define MAIN_EXTRUDER_ID          0
#define DEPUTY_EXTRUDER_ID        1
#define UNIQUE_EXTRUDER_ID        MAIN_EXTRUDER_ID
#define INVALID_EXTRUDER_ID       -1


/* Nozzle*/
enum NozzleFlowType
{
    NONE_FLOWTYPE,
    S_FLOW,
    H_FLOW
};

/*Print speed*/
enum DevPrintingSpeedLevel
{
    SPEED_LEVEL_INVALID = 0,
    SPEED_LEVEL_SILENCE = 1,
    SPEED_LEVEL_NORMAL = 2,
    SPEED_LEVEL_RAPID = 3,
    SPEED_LEVEL_RAMPAGE = 4,
    SPEED_LEVEL_COUNT
};

/*Upgrade*/
enum class DevFirmwareUpgradingState : int
{
    DC = -1,
    UpgradingUnavaliable = 0,
    UpgradingAvaliable = 1,
    UpgradingInProgress = 2,
    UpgradingFinished = 3
};

class devPrinterUtil
{
public:
    devPrinterUtil() = delete;
    ~devPrinterUtil() = delete;

public:
    static bool IsVirtualSlot(int ams_id) { return (ams_id == VIRTUAL_TRAY_MAIN_ID || ams_id == VIRTUAL_TRAY_DEPUTY_ID);}
    static bool IsVirtualSlot(const std::string& ams_id) { return (ams_id == VIRTUAL_AMS_MAIN_ID_STR || ams_id == VIRTUAL_AMS_DEPUTY_ID_STR); }
};

};// namespace Slic3r