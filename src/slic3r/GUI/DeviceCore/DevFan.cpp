#include <nlohmann/json.hpp>
#include "DevFan.h"
#include <wx/app.h>
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
using namespace nlohmann;

void Slic3r::DevFan::converse_to_duct(bool is_suppt_part_fun, bool is_suppt_aux_fun, bool is_suppt_cham_fun)
{
     m_air_duct_data.modes.clear();
     m_air_duct_data.parts.clear();
     m_air_duct_data.curren_mode = -1; // def mode

     if (is_suppt_part_fun) {
         AirParts part_fan;
         part_fan.type        = int(AirDuctType::AIR_FAN_TYPE);
         part_fan.id          = int(AIR_FUN::FAN_COOLING_0_AIRDOOR);
         part_fan.func        = int(AIR_FUN::FAN_COOLING_0_AIRDOOR);
         part_fan.state       = 0;
         part_fan.range_start = 0;
         part_fan.range_end   = 100;
         m_air_duct_data.parts.push_back(part_fan);
     }

     if (is_suppt_aux_fun) {
         AirParts aux_fan;
         aux_fan.type        = int(AirDuctType::AIR_FAN_TYPE);
         aux_fan.id          = int(AIR_FUN::FAN_REMOTE_COOLING_0_IDX);
         aux_fan.func        = int(AIR_FUN::FAN_REMOTE_COOLING_0_IDX);
         aux_fan.state       = 0;
         aux_fan.range_start = 0;
         aux_fan.range_end   = 100;
         m_air_duct_data.parts.push_back(aux_fan);
     }

     if (is_suppt_aux_fun) {
         AirParts chamber_fan;
         chamber_fan.type        = int(AirDuctType::AIR_FAN_TYPE);
         chamber_fan.id          = int(AIR_FUN::FAN_CHAMBER_0_IDX);
         chamber_fan.func        = int(AIR_FUN::FAN_CHAMBER_0_IDX);
         chamber_fan.state       = 0;
         chamber_fan.range_start = 0;
         chamber_fan.range_end   = 100;
         m_air_duct_data.parts.push_back(chamber_fan);
     }
}

static std::string _get_string_from_fantype(int type)
{
    switch (type) {
    case 1: return "cooling_fan";
    case 2: return "big_cooling_fan";
    case 3: return "chamber_fan";
    default: return "";
    }
    return "";
}

// Old protocol
int Slic3r::DevFan::command_control_fan(int fan_type, int val)
{
    std::string gcode = (boost::format("M106 P%1% S%2% \n") % (int) fan_type % (val)).str();
    return m_owner->publish_gcode(gcode);
}

// New protocol
int Slic3r::DevFan::command_control_fan_new(int fan_id, int val)
{
    BOOST_LOG_TRIVIAL(info) << "New protocol of fan setting(set speed), fan_id = " << fan_id;
    json j;
    j["print"]["command"]     = "set_fan";
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["fan_index"]   = fan_id;

    j["print"]["speed"] = val;
    BOOST_LOG_TRIVIAL(info) << "MachineObject::command_control_fan_val, set the speed of fan, fan_id = " << fan_id;
    return m_owner->publish_json(j);
}


int Slic3r::DevFan::command_handle_response(const json &response)
{
    if (!response.contains("sequence_id")) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ", error reponse.";
        return -1;
    }

    std::string reply = response["sequence_id"].get<std::string>();
    auto        it    = m_callback_list.find(reply);
    if (it != m_callback_list.end()) {
        if (it->second) it->second(response);
        m_callback_list.erase(it);
    }
    return 0;
}

int Slic3r::DevFan::command_control_air_duct(int mode_id, int submode, const CommandCallBack& cb)
{
    BOOST_LOG_TRIVIAL(info) << "MachineObject::command_control_air_duct, set air duct, d = " << mode_id;
    m_callback_list[std::to_string(m_owner->m_sequence_id)] = cb;
    json j;
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["command"]     = "set_airduct";
    j["print"]["modeId"]      = mode_id;
    j["print"]["submode"]     = submode;

    return m_owner->publish_json(j);
}

void Slic3r::DevFan::ParseV1_0(const json &print_json)
{
    if (print_json.contains("fan_gear")) {
        fan_gear          = print_json["fan_gear"].get<std::uint32_t>();
        big_fan2_speed    = (int) ((fan_gear & 0x00FF0000) >> 16);
        big_fan1_speed    = (int) ((fan_gear & 0x0000FF00) >> 8);
        cooling_fan_speed = (int) ((fan_gear & 0x000000FF) >> 0);
    } else {
        if (print_json.contains("cooling_fan_speed")) {
            cooling_fan_speed = stoi(print_json["cooling_fan_speed"].get<std::string>());
            cooling_fan_speed = round(floor(cooling_fan_speed / float(1.5)) * float(25.5));
        } else {
            cooling_fan_speed = 0;
        }
        if (print_json.contains("big_fan1_speed")) {
            big_fan1_speed = stoi(print_json["big_fan1_speed"].get<std::string>());
            big_fan1_speed = round(floor(big_fan1_speed / float(1.5)) * float(25.5));
        } else {
            big_fan1_speed = 0;
        }
        if (print_json.contains("big_fan2_speed")) {
            big_fan2_speed = stoi(print_json["big_fan2_speed"].get<std::string>());
            big_fan2_speed = round(floor(big_fan2_speed / float(1.5)) * float(25.5));
        } else {
            big_fan2_speed = 0;
        }
    }

    if (print_json.contains("heatbreak_fan_speed")) { heatbreak_fan_speed = stoi(print_json["heatbreak_fan_speed"].get<std::string>()); }



}

void Slic3r::DevFan::ParseV2_0(const json &print_json) {

     if (print_json.contains("support_aux_fan")) {
        if (print_json["support_aux_fan"].is_boolean())
            is_support_aux_fan = print_json["support_aux_fan"].get<bool>();
     }

    if (print_json.contains("support_chamber_fan")) {
        if (print_json["support_chamber_fan"].is_boolean())
            is_support_chamber_fan = print_json["support_chamber_fan"].get<bool>();
    }
}




void Slic3r::DevFan::ParseV3_0(const json &device)
{
    if (device.contains("airduct")) {
        is_support_airduct = true;
        m_air_duct_data.curren_mode = -1;
        m_air_duct_data.modes.clear();
        m_air_duct_data.parts.clear();

        m_air_duct_data.curren_mode = device["airduct"]["modeCur"].get<int>();

        const json &airduct = device["airduct"];
        if (airduct.contains("modeCur")) { m_air_duct_data.curren_mode = airduct["modeCur"].get<int>(); }
        if (airduct.contains("subMode")) { m_air_duct_data.m_sub_mode = airduct["subMode"].get<int>(); }
        if (airduct.contains("modeList") && airduct["modeList"].is_array()) {
            auto list = airduct["modeList"].get<std::vector<json>>();

            for (int i = 0; i < list.size(); ++i) {
                // only show 2 mode for o
                if (m_owner->is_series_o() && i >= 2) { break; }

                json    mode_json = list[i];
                AirMode mode;
                if (mode_json.contains("modeId")) mode.id = mode_json["modeId"].get<int>();
                if (mode_json.contains("ctrl")) {
                    for (auto it_mode_ctrl = mode_json["ctrl"].begin(); it_mode_ctrl != mode_json["ctrl"].end(); it_mode_ctrl++) {
                        mode.ctrl.push_back((*it_mode_ctrl).get<int>() >> 4);
                    }
                }

                if (mode_json.contains("off")) {
                    for (auto it_mode_off = mode_json["off"].begin(); it_mode_off != mode_json["off"].end(); *it_mode_off++) {
                        mode.off.push_back((*it_mode_off).get<int>() >> 4);
                    }
                }

                if (AIR_DUCT(mode.id) == AIR_DUCT::AIR_DUCT_EXHAUST) { continue; } /*STUDIO-12796*/
                m_air_duct_data.modes[mode.id] = mode;
            }
        }

        if (airduct.contains("parts") && airduct["parts"].is_array()) {
            for (auto it_part = airduct["parts"].begin(); it_part != airduct["parts"].end(); it_part++) {
                int state = (*it_part)["state"].get<int>();
                int range = (*it_part)["range"].get<int>();

                AirParts part;
                part.type        = m_owner->get_flag_bits((*it_part)["id"].get<int>(), 0, 4);
                part.id          = m_owner->get_flag_bits((*it_part)["id"].get<int>(), 4, 8);
                part.func        = (*it_part)["func"].get<int>();
                part.state       = m_owner->get_flag_bits(state, 0, 8);
                part.range_start = m_owner->get_flag_bits(range, 0, 16);
                part.range_end   = m_owner->get_flag_bits(range, 16, 16);

                m_air_duct_data.parts.push_back(part);
            }
        }
    }

}
