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
#include <utility>

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

enum DevAmsType : int
{
    EXT_SPOOL = 0,      // EXT
    AMS = 1,            // AMS1
    AMS_LITE = 2,       // AMS-Lite
    N3F = 3,            // N3F, AMS 2PRO
    N3S = 4,            // N3S, AMS HT
    AMS_LITE_MIXED = 5, // AMS-Lite for N9
};

// Device-numbered filament-change step codes. Newer firmware reports the exact change-step
// sequence through the AMS (ams.cfs), keyed by these codes, instead of the client hardcoding
// steps per model. Distinct from the legacy GUI-side Slic3r::GUI::FilamentStep enum.
// STEP_CHECK_POSITION and STEP_CONFIRM_EXTRUDED share code 0x08 by device design.
enum DevFilamentStep
{
    STEP_IDLE = 0x00,
    STEP_PAUSE = 0x01,
    STEP_HEAT_NOZZLE = 0x02,
    STEP_CUT_FILAMENT = 0x03,
    STEP_PULL_CURR_FILAMENT = 0x04,
    STEP_PUSH_NEW_FILAMENT = 0x05,
    STEP_GRAB_NEW_FILAMENT = 0x06,
    STEP_PURGE_OLD_FILAMENT = 0x07,
    STEP_CHECK_POSITION = 0x08,
    STEP_SWITCH_EXTRUDER = 0x09,
    STEP_SWITCH_HOTEND = 0x0A,
    STEP_AMS_FILA_COOLING = 0x0B,
    STEP_PUSH_SWITCHER_FILA = 0x0C,
    STEP_PULL_SWITCHER_FILA = 0x0D,
    STEP_SWITCHER_SWITCH = 0x0E,
    STEP_CONFIRM_EXTRUDED = 0x08,
    STEP_COUNT,
};

// Slots and Tray
#define VIRTUAL_TRAY_MAIN_ID    255
#define VIRTUAL_TRAY_DEPUTY_ID  254

#define VIRTUAL_AMS_MAIN_ID_STR   "255"
#define VIRTUAL_AMS_DEPUTY_ID_STR "254"

// Tray index offset for the AMS-Lite-mixed unit (A2L / N9). Its 4 trays occupy
// global tray indices 24..27.
#define AMS_LITE_MIXED_TRAY_INDEX_OFFSET 24

#define INVALID_AMS_TEMPERATURE std::numeric_limits<float>::min()

// (ams_id, slot_id) pair.
using DevAmsSlotId = std::pair<int, int>;

/* Extruder*/
#define MAIN_EXTRUDER_ID          0
#define DEPUTY_EXTRUDER_ID        1
#define UNIQUE_EXTRUDER_ID        MAIN_EXTRUDER_ID
#define INVALID_EXTRUDER_ID       -1

/* Logical extruder ids (multi-nozzle). */
#define LOGIC_UNIQUE_EXTRUDER_ID  0
#define LOGIC_L_EXTRUDER_ID       0
#define LOGIC_R_EXTRUDER_ID       1


/* Nozzle*/
enum NozzleFlowType
{
    NONE_FLOWTYPE,
    S_FLOW,
    H_FLOW,
    U_FLOW, // TPU 1.75 High Flow (device-reported; maps to nvtTPUHighFlow)
};

// Discrete nozzle diameters reported by the device.
enum NozzleDiameterType : int
{
    NONE_DIAMETER_TYPE,
    NOZZLE_DIAMETER_0_2,
    NOZZLE_DIAMETER_0_4,
    NOZZLE_DIAMETER_0_6,
    NOZZLE_DIAMETER_0_8
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

namespace GUI
{
// Print source. Defined here (rather than in SelectMachine.hpp) so the shared device-mapping
// GUI headers — AmsMappingPopup, wgtDeviceNozzleSelect — can name it without pulling in
// SelectMachine.hpp, which would create an include cycle.
enum PrintFromType
{
    FROM_NORMAL,
    FROM_SDCARD_VIEW,
};
}

};// namespace Slic3r

// A nozzle requirement of the sliced plate (or an installed nozzle's identity), compared in the
// pre-print slicing-vs-installed nozzle checks.
struct NozzleDef
{
    float                  nozzle_diameter;
    Slic3r::NozzleFlowType nozzle_flow_type;

    bool operator==(const NozzleDef& other) const
    {
        return nozzle_diameter == other.nozzle_diameter && nozzle_flow_type == other.nozzle_flow_type;
    }
};

template<> struct std::hash<NozzleDef>
{
    std::size_t operator()(const NozzleDef& v) const noexcept
    {
        size_t h1 = std::hash<int>{}(v.nozzle_diameter * 1000);
        size_t h2 = std::hash<int>{}(v.nozzle_flow_type);
        return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
    };
};