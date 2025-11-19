#pragma once
#include <nlohmann/json.hpp>
#include "slic3r/Utils/json_diff.hpp"

#include <map>

namespace Slic3r {

class MachineObject;

enum AirDuctType { AIR_FAN_TYPE, AIR_DOOR_TYPE };
typedef std::function<void(const json &)> CommandCallBack;



enum AIR_FUN : int {
    FAN_HEAT_BREAK_0_IDX      = 0,
    FAN_COOLING_0_AIRDOOR     = 1,
    FAN_REMOTE_COOLING_0_IDX  = 2,
    FAN_CHAMBER_0_IDX         = 3,
    FAN_HEAT_BREAK_1_IDX      = 4,
    FAN_MC_BOARD_0_IDX        = 5,
    FAN_INNNER_LOOP_FAN_0_IDX = 6,
    FAN_TOTAL_COUNT           = 7,
    FAN_REMOTE_COOLING_1_IDX  = 10
};

enum AIR_DOOR { AIR_DOOR_FUNC_CHAMBER = 0, AIR_DOOR_FUNC_INNERLOOP, AIR_DOOR_FUNC_TOP };


enum AIR_DUCT : int {
    AIR_DUCT_NONE         = -1,
    AIR_DUCT_COOLING_FILT = 0,
    AIR_DUCT_HEATING_INTERNAL_FILT,
    AIR_DUCT_EXHAUST,
    AIR_DUCT_FULL_COOLING,
    AIR_DUCT_INIT = 0xFF // Initial mode, only used within mc
};

struct AirParts
{
    int type{0};
    int id{0};
    int func{0};
    int state{0};       // 100%
    int range_start{0}; // 100%
    int range_end{0};   // 100%

public:
    bool operator ==(const AirParts& other) const
    {
        return (type == other.type) && (id == other.id) && (func == other.func) && (state == other.state) && (range_start == other.range_start) && (range_end == other.range_end);
    };
};

struct AirMode
{
    int              id{-1};
    std::vector<int> ctrl;
    // If the fan is off, it cannot be controlled and is displayed as off
    std::vector<int> off;
    // If the fan is not off or ctrl, it will be displayed as auto

public:
    bool operator ==(const AirMode& other) const
    {
        return (id == other.id) && (ctrl == other.ctrl) && (off == other.off);
    };
};

struct AirDuctData
{
    int                              curren_mode{0};
    std::map<int, AirMode>           modes;
    std::vector<AirParts>            parts;

    int  m_sub_mode = -1;// the submode of airduct, for cooling: 0-filter, 1-cooling
    bool m_support_cooling_filter = false;// support switch filter on cooling mode or not

public:
    bool operator ==(const AirDuctData& other) const
    {
        return (curren_mode == other.curren_mode) && (modes == other.modes) && (parts == other.parts) &&
            (m_sub_mode == other.m_sub_mode) && (m_support_cooling_filter == other.m_support_cooling_filter);
    };

    bool operator !=(const AirDuctData& other) const
    {
        return !(operator==(other));
    };

    bool IsSupportCoolingFilter() const { return m_support_cooling_filter; }
    bool IsCoolingFilerOn() const { return m_sub_mode == 1; }
};

class DevFan
{
public:
     DevFan(MachineObject *obj) : m_owner(obj){};
public:

    enum FanType {//if have devPrinter , delete?
        COOLING_FAN     = 1,
        BIG_COOLING_FAN = 2,
        CHAMBER_FAN     = 3,
        EXHAUST_FAN,
        FILTER_FAN,
    };

     bool is_at_heating_mode() const { return m_air_duct_data.curren_mode == AIR_DUCT_HEATING_INTERNAL_FILT; };
     bool is_at_cooling_mode() const { return m_air_duct_data.curren_mode == AIR_DUCT_COOLING_FILT; };

     void SetSupportCoolingFilter(bool enable) { m_air_duct_data.m_support_cooling_filter = enable; }
     AirDuctData GetAirDuctData() { return m_air_duct_data; };

     void converse_to_duct(bool is_suppt_part_fun, bool is_suppt_aux_fun, bool is_suppt_cham_fun); // Convert the data to duct type to make the newand old protocols consistent
     int  command_handle_response(const json &response);
     int  command_control_fan(int fan_type, int val);                              // Old protocol
     int  command_control_fan_new(int fan_id, int val); // New protocol
     int  command_control_air_duct(int mode_id, int submode, const CommandCallBack& cb);

     void ParseV1_0(const json &print_json);
     void ParseV2_0(const json &print_json);
     void ParseV3_0(const json &print_json);

 public:
     bool     GetSupportAirduct() { return is_support_airduct; };
     bool     GetSupportAuxFanData() { return is_support_aux_fan; };
     bool     GetSupportChamberFan() { return is_support_aux_fan; };
     int      GetHeatBreakFanSpeed() { return heatbreak_fan_speed; }
     int      GetCoolingFanSpeed() { return cooling_fan_speed; }
     int      GetBigFan1Speed() { return big_fan1_speed; }
     int      GetBigFan2Speed() { return big_fan2_speed; }
     uint32_t GetFanGear() { return fan_gear; }


private:
      AirDuctData m_air_duct_data;

      bool is_support_aux_fan{false};
      bool is_support_chamber_fan{false};
      bool is_support_airduct{false};

      int      heatbreak_fan_speed = 0;
      int      cooling_fan_speed   = 0;
      int      big_fan1_speed      = 0;
      int      big_fan2_speed      = 0;
      uint32_t fan_gear            = 0;

      std::map<std::string, CommandCallBack> m_callback_list;

      MachineObject *m_owner = nullptr;
};


} // namespace Slic3r