#include <nlohmann/json.hpp>
#include "DevFilaSystem.h"

// TODO: remove this include
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/I18N.hpp"

#include "DevUtil.h"

using namespace nlohmann;

namespace Slic3r {
static int _hex_digit_to_int(const char c) { return (c >= '0' && c <= '9') ? c - '0' : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1; }

wxColour DevAmsTray::decode_color(const std::string &color)
{
    std::array<int, 4> ret = {0, 0, 0, 0};
    const char *       c   = color.data();
    if (color.size() == 8) {
        for (size_t j = 0; j < 4; ++j) {
            int digit1 = _hex_digit_to_int(*c++);
            int digit2 = _hex_digit_to_int(*c++);
            if (digit1 == -1 || digit2 == -1) break;
            ret[j] = static_cast<float>(digit1 * 16 + digit2);
        }
    } else { return wxColour(255, 255, 255, 255); }

    return wxColour(ret[0], ret[1], ret[2], ret[3]);
}

void DevAmsTray::UpdateColorFromStr(const std::string& color)
{
    if (color.empty()) return;
    if (this->color != color)
    {
        wx_color = "#" + wxString::FromUTF8(color);
        this->color = color;
    }
}

void DevAmsTray::reset()
{
    tag_uid             = "";
    setting_id          = "";
    filament_setting_id = "";
    m_fila_type         = "";
    sub_brands          = "";
    color               = "";
    weight              = "";
    diameter            = "";
    temp                = "";
    time                = "";
    bed_temp_type       = "";
    bed_temp            = "";
    nozzle_temp_max     = "";
    nozzle_temp_min     = "";
    xcam_info           = "";
    uuid                = "";
    k                   = 0.0f;
    n                   = 0.0f;
    is_bbl              = false;
    hold_count          = 0;
    remain              = 0;
}


bool DevAmsTray::is_tray_info_ready() const
{
    if (color.empty()) return false;
    if (m_fila_type.empty()) return false;
    //if (setting_id.empty()) return false;
    return true;
}

bool DevAmsTray::is_unset_third_filament() const
{
    if (this->is_bbl) return false;
    return (color.empty() || m_fila_type.empty());
}

std::string DevAmsTray::get_display_filament_type() const
{
    if (m_fila_type == "PLA-S") return "Sup.PLA";
    if (m_fila_type == "PA-S") return "Sup.PA";
    if (m_fila_type == "ABS-S") return "Sup.ABS";
    return m_fila_type;
}

std::string DevAmsTray::get_filament_type()
{
    if (m_fila_type == "Sup.PLA") { return "PLA-S"; }
    if (m_fila_type == "Sup.PA") { return "PA-S"; }
    if (m_fila_type == "Sup.ABS") { return "ABS-S"; }
    if (m_fila_type == "Support W") { return "PLA-S"; }
    if (m_fila_type == "Support G") { return "PA-S"; }
    if (m_fila_type == "Support") { if (setting_id == "GFS00") { m_fila_type = "PLA-S"; } else if (setting_id == "GFS01") { m_fila_type = "PA-S"; } else { return "PLA-S"; } }

    return m_fila_type;
}


DevAms::DevAms(const std::string& ams_id, int extruder_id, AmsType type)
{
    m_ams_id = ams_id;
    m_ext_id = extruder_id;
    m_ams_type = type;
}

DevAms::DevAms(const std::string& ams_id, int nozzle_id, int type)
{
    m_ams_id = ams_id;
    m_ext_id = nozzle_id;
    m_ams_type = (AmsType)type;
    assert(DUMMY < type && m_ams_type <= N3S);
}

DevAms::~DevAms()
{
    for (auto it = m_trays.begin(); it != m_trays.end(); it++)
    {
        if (it->second)
        {
            delete it->second;
            it->second = nullptr;
        }
    }
    m_trays.clear();
}

static unordered_map<int, wxString> s_ams_display_formats = {
    {DevAms::AMS,      "AMS-%d"},
    {DevAms::AMS_LITE, "AMS Lite-%d"},
    {DevAms::N3F,      "AMS 2 PRO-%d"},
    {DevAms::N3S,      "AMS HT-%d"}
};

wxString DevAms::GetDisplayName() const
{
    wxString ams_display_format;
    auto iter = s_ams_display_formats.find(m_ams_type);
    if (iter != s_ams_display_formats.end()) 
    {
        ams_display_format = iter->second;
    }
    else
    {
        assert(0 && __FUNCTION__);
        ams_display_format = "AMS-%d";
    }

    int num_id;
    try
    {
        num_id = std::stoi(GetAmsId());
    }
    catch (const std::exception& e) 
    {
        assert(0 && __FUNCTION__);
        BOOST_LOG_TRIVIAL(error) << "Invalid AMS ID: " << GetAmsId() << ", error: " << e.what();
        num_id = 0;
    }

    int loc = (num_id > 127) ? (num_id - 127) : (num_id + 1);
    return wxString::Format(ams_display_format, loc);
}

int DevAms::GetSlotCount() const
{
    if (m_ams_type == AMS || m_ams_type == AMS_LITE || m_ams_type == N3F)
    {
        return 4;
    }
    else if (m_ams_type == N3S)
    {
        return 1;
    }

    return 1;
}

DevAmsTray* DevAms::GetTray(const std::string& tray_id) const
{
    auto it = m_trays.find(tray_id);
    if (it != m_trays.end())
    {
        return it->second;
    }

    return nullptr;
}

DevFilaSystem::~DevFilaSystem()
{
    for (auto it = amsList.begin(); it != amsList.end(); it++)
    {
        if (it->second)
        {
            delete it->second;
            it->second = nullptr;
        }
    }
    amsList.clear();
}

DevAms* DevFilaSystem::GetAmsById(const std::string& ams_id) const
{
    auto it = amsList.find(ams_id);
    if (it != amsList.end())
    {
        return it->second;
    }

    return nullptr;
}

DevAmsTray* DevFilaSystem::GetAmsTray(const std::string& ams_id, const std::string& tray_id) const
{
    auto it = amsList.find(ams_id);
    if (it == amsList.end()) return nullptr;
    if (!it->second) return nullptr;
    return it->second->GetTray(tray_id);;
}

void DevFilaSystem::CollectAmsColors(std::vector<wxColour>& ams_colors) const
{
    ams_colors.clear();
    ams_colors.reserve(amsList.size());
    for (auto ams = amsList.begin(); ams != amsList.end(); ams++)
    {
        for (auto tray = ams->second->GetTrays().begin(); tray != ams->second->GetTrays().end(); tray++)
        {
            if (tray->second->is_tray_info_ready())
            {
                auto ams_color = DevAmsTray::decode_color(tray->second->color);
                ams_colors.emplace_back(ams_color);
            }
        }
    }
}

int DevFilaSystem::GetExtruderIdByAmsId(const std::string& ams_id) const
{
    auto it = amsList.find(ams_id);
    if (it != amsList.end())
    {
        return it->second->GetExtruderId();
    }
    else if (stoi(ams_id) == VIRTUAL_TRAY_MAIN_ID)
    {
        return MAIN_EXTRUDER_ID;
    }
    else if (stoi(ams_id) == VIRTUAL_TRAY_DEPUTY_ID)
    {
        return DEPUTY_EXTRUDER_ID;
    }

    assert(false && __FUNCTION__);
    return 0; // not found
}

bool DevFilaSystem::IsAmsSettingUp() const
{
    int setting_up_stat = DevUtil::get_flag_bits(m_ams_cali_stat, 0, 8);
    if (setting_up_stat == 0x01 || setting_up_stat == 0x02 || setting_up_stat == 0x03 || setting_up_stat == 0x04)
    {
        return true;
    }

    return false;
}

bool DevFilaSystem::IsBBL_Filament(std::string tag_uid)
{
    if (tag_uid.empty())
    {
        return false;
    }

    for (int i = 0; i < tag_uid.length(); i++)
    {
        if (tag_uid[i] != '0') { return true; }
    }

    return false;
}

void DevFilaSystemParser::ParseV1_0(const json& jj, MachineObject* obj, DevFilaSystem* system, bool key_field_only)
{
    if (jj.contains("ams"))
    {
        if (jj["ams"].contains("ams"))
        {
            if (jj["ams"].contains("ams_exist_bits"))
            {
                obj->ams_exist_bits = stol(jj["ams"]["ams_exist_bits"].get<std::string>(), nullptr, 16);
            }

            if (jj["ams"].contains("tray_exist_bits"))
            {
                obj->tray_exist_bits = stol(jj["ams"]["tray_exist_bits"].get<std::string>(), nullptr, 16);
            }

            if (jj["ams"].contains("cali_stat")) { system->m_ams_cali_stat = jj["ams"]["cali_stat"].get<int>(); }

            if (!key_field_only)
            {
                if (jj["ams"].contains("tray_read_done_bits"))
                {
                    obj->tray_read_done_bits = stol(jj["ams"]["tray_read_done_bits"].get<std::string>(), nullptr, 16);
                }
                if (jj["ams"].contains("tray_reading_bits"))
                {
                    obj->tray_reading_bits = stol(jj["ams"]["tray_reading_bits"].get<std::string>(), nullptr, 16);
                }
                if (jj["ams"].contains("tray_is_bbl_bits"))
                {
                    obj->tray_is_bbl_bits = stol(jj["ams"]["tray_is_bbl_bits"].get<std::string>(), nullptr, 16);
                }
                if (jj["ams"].contains("version"))
                {
                    if (jj["ams"]["version"].is_number())
                    {
                        obj->ams_version = jj["ams"]["version"].get<int>();
                    }
                }

#if 0
                if (jj["ams"].contains("ams_rfid_status")) { }
#endif

                if (time(nullptr) - obj->ams_user_setting_start > HOLD_TIME_3SEC)
                {
                    if (jj["ams"].contains("insert_flag"))
                    {
                        system->m_ams_system_setting.SetDetectOnInsertEnabled(jj["ams"]["insert_flag"].get<bool>());
                    }
                    if (jj["ams"].contains("power_on_flag"))
                    {
                        system->m_ams_system_setting.SetDetectOnPowerupEnabled(jj["ams"]["power_on_flag"].get<bool>());
                    }
                    if (jj["ams"].contains("calibrate_remain_flag"))
                    {
                        system->m_ams_system_setting.SetDetectRemainEnabled(jj["ams"]["calibrate_remain_flag"].get<bool>());
                    }
                }

                json j_ams = jj["ams"]["ams"];
                std::set<std::string> ams_id_set;

                for (auto it = system->amsList.begin(); it != system->amsList.end(); it++)
                {
                    ams_id_set.insert(it->first);
                }

                for (auto it = j_ams.begin(); it != j_ams.end(); it++)
                {
                    if (!it->contains("id")) continue;
                    std::string ams_id = (*it)["id"].get<std::string>();

                    int extuder_id = MAIN_EXTRUDER_ID; // Default nozzle id
                    int type_id = 1;   // 0:dummy 1:ams 2:ams-lite 3:n3f 4:n3s

                    /*ams info*/
                    if (it->contains("info")) {
                        const std::string& info = (*it)["info"].get<std::string>();
                        type_id = DevUtil::get_flag_bits(info, 0, 4);
                        extuder_id = DevUtil::get_flag_bits(info, 8, 4);
                    } else {
                        if (!obj->is_enable_ams_np && obj->get_printer_ams_type() == "f1") {
                            type_id = DevAms::AMS_LITE;
                        }
                    }

                    /*AMS without initialization*/
                    if (extuder_id == 0xE)
                    {
                        ams_id_set.erase(ams_id);
                        system->amsList.erase(ams_id);
                        continue;
                    }

                    ams_id_set.erase(ams_id);
                    DevAms* curr_ams = nullptr;
                    auto ams_it = system->amsList.find(ams_id);
                    if (ams_it == system->amsList.end())
                    {
                        DevAms* new_ams = new DevAms(ams_id, extuder_id, type_id);
                        system->amsList.insert(std::make_pair(ams_id, new_ams));
                        // new ams added event
                        curr_ams = new_ams;
                    }
                    else
                    {
                        if (extuder_id != ams_it->second->GetExtruderId())
                        {
                            ams_it->second->m_ext_id = extuder_id;
                        }

                        curr_ams = ams_it->second;
                    }
                    if (!curr_ams) continue;

                    /*set ams type flag*/
                    curr_ams->SetAmsType(type_id);


                    /*set ams exist flag*/
                    try
                    {
                        if (!ams_id.empty())
                        {
                            int ams_id_int = atoi(ams_id.c_str());

                            if (type_id < 4)
                            {
                                curr_ams->m_exist = (obj->ams_exist_bits & (1 << ams_id_int)) != 0 ? true : false;
                            }
                            else
                            {
                                curr_ams->m_exist = DevUtil::get_flag_bits(obj->ams_exist_bits, 4 + (ams_id_int - 128));
                            }
                        }
                    }
                    catch (...)
                    {
                        ;
                    }

                    if (it->contains("dry_time") && (*it)["dry_time"].is_number())
                    {
                        curr_ams->m_left_dry_time = (*it)["dry_time"].get<int>();
                    }

                    if (it->contains("humidity"))
                    {
                        try
                        {
                            std::string humidity = (*it)["humidity"].get<std::string>();
                            curr_ams->m_humidity_level = atoi(humidity.c_str());
                        }
                        catch (...)
                        {
                            ;
                        }
                    }

                    if (it->contains("humidity_raw"))
                    {
                        try
                        {
                            std::string humidity_raw = (*it)["humidity_raw"].get<std::string>();
                            curr_ams->m_humidity_percent = atoi(humidity_raw.c_str());
                        }
                        catch (...)
                        {
                            ;
                        }
                    }


                    if (it->contains("temp"))
                    {
                        std::string temp = (*it)["temp"].get<std::string>();
                        try
                        {
                            curr_ams->m_current_temperature = DevUtil::string_to_float(temp);
                        }
                        catch (...)
                        {
                            curr_ams->m_current_temperature = INVALID_AMS_TEMPERATURE;
                        }
                    }

                    if (it->contains("tray"))
                    {
                        std::set<std::string> tray_id_set;
                        for (auto it = curr_ams->GetTrays().cbegin(); it != curr_ams->GetTrays().cend(); it++)
                        {
                            tray_id_set.insert(it->first);
                        }
                        for (auto tray_it = (*it)["tray"].begin(); tray_it != (*it)["tray"].end(); tray_it++)
                        {
                            if (!tray_it->contains("id")) continue;
                            std::string tray_id = (*tray_it)["id"].get<std::string>();
                            tray_id_set.erase(tray_id);
                            // compare tray_list
                            DevAmsTray* curr_tray = nullptr;
                            auto tray_iter = curr_ams->GetTrays().find(tray_id);
                            if (tray_iter == curr_ams->GetTrays().end())
                            {
                                DevAmsTray* new_tray = new DevAmsTray(tray_id);
                                curr_ams->m_trays.insert(std::make_pair(tray_id, new_tray));
                                curr_tray = new_tray;
                            }
                            else
                            {
                                curr_tray = tray_iter->second;
                            }
                            if (!curr_tray) continue;

                            if (curr_tray->hold_count > 0)
                            {
                                curr_tray->hold_count--;
                                continue;
                            }

                            curr_tray->id = (*tray_it)["id"].get<std::string>();
                            if (tray_it->contains("tag_uid"))
                                curr_tray->tag_uid = (*tray_it)["tag_uid"].get<std::string>();
                            else
                                curr_tray->tag_uid = "0";
                            if (tray_it->contains("tray_info_idx") && tray_it->contains("tray_type"))
                            {
                                curr_tray->setting_id = (*tray_it)["tray_info_idx"].get<std::string>();
                                //std::string type            = (*tray_it)["tray_type"].get<std::string>();
                                std::string type = MachineObject::setting_id_to_type(curr_tray->setting_id, (*tray_it)["tray_type"].get<std::string>());
                                if (curr_tray->setting_id == "GFS00")
                                {
                                    curr_tray->m_fila_type = "PLA-S";
                                }
                                else if (curr_tray->setting_id == "GFS01")
                                {
                                    curr_tray->m_fila_type = "PA-S";
                                }
                                else
                                {
                                    curr_tray->m_fila_type = type;
                                }
                            }
                            else
                            {
                                curr_tray->setting_id = "";
                                curr_tray->m_fila_type = "";
                            }
                            if (tray_it->contains("tray_sub_brands"))
                                curr_tray->sub_brands = (*tray_it)["tray_sub_brands"].get<std::string>();
                            else
                                curr_tray->sub_brands = "";
                            if (tray_it->contains("tray_weight"))
                                curr_tray->weight = (*tray_it)["tray_weight"].get<std::string>();
                            else
                                curr_tray->weight = "";
                            if (tray_it->contains("tray_diameter"))
                                curr_tray->diameter = (*tray_it)["tray_diameter"].get<std::string>();
                            else
                                curr_tray->diameter = "";
                            if (tray_it->contains("tray_temp"))
                                curr_tray->temp = (*tray_it)["tray_temp"].get<std::string>();
                            else
                                curr_tray->temp = "";
                            if (tray_it->contains("tray_time"))
                                curr_tray->time = (*tray_it)["tray_time"].get<std::string>();
                            else
                                curr_tray->time = "";
                            if (tray_it->contains("bed_temp_type"))
                                curr_tray->bed_temp_type = (*tray_it)["bed_temp_type"].get<std::string>();
                            else
                                curr_tray->bed_temp_type = "";
                            if (tray_it->contains("bed_temp"))
                                curr_tray->bed_temp = (*tray_it)["bed_temp"].get<std::string>();
                            else
                                curr_tray->bed_temp = "";
                            if (tray_it->contains("tray_color"))
                            {
                                auto color = (*tray_it)["tray_color"].get<std::string>();
                                curr_tray->UpdateColorFromStr(color);
                            }
                            else
                            {
                                curr_tray->color = "";
                            }
                            if (tray_it->contains("nozzle_temp_max"))
                            {
                                curr_tray->nozzle_temp_max = (*tray_it)["nozzle_temp_max"].get<std::string>();
                            }
                            else
                                curr_tray->nozzle_temp_max = "";
                            if (tray_it->contains("nozzle_temp_min"))
                                curr_tray->nozzle_temp_min = (*tray_it)["nozzle_temp_min"].get<std::string>();
                            else
                                curr_tray->nozzle_temp_min = "";
                            if (tray_it->contains("xcam_info"))
                                curr_tray->xcam_info = (*tray_it)["xcam_info"].get<std::string>();
                            else
                                curr_tray->xcam_info = "";
                            if (tray_it->contains("tray_uuid"))
                                curr_tray->uuid = (*tray_it)["tray_uuid"].get<std::string>();
                            else
                                curr_tray->uuid = "0";

                            if (tray_it->contains("ctype"))
                                curr_tray->ctype = (*tray_it)["ctype"].get<int>();
                            else
                                curr_tray->ctype = 0;
                            curr_tray->cols.clear();
                            if (tray_it->contains("cols"))
                            {
                                if ((*tray_it)["cols"].is_array())
                                {
                                    for (auto it = (*tray_it)["cols"].begin(); it != (*tray_it)["cols"].end(); it++)
                                    {
                                        curr_tray->cols.push_back(it.value().get<std::string>());
                                    }
                                }
                            }

                            if (tray_it->contains("remain"))
                            {
                                curr_tray->remain = (*tray_it)["remain"].get<int>();
                            }
                            else
                            {
                                curr_tray->remain = -1;
                            }
                            int ams_id_int = 0;
                            int tray_id_int = 0;
                            try
                            {
                                if (!ams_id.empty() && !curr_tray->id.empty())
                                {
                                    ams_id_int = atoi(ams_id.c_str());
                                    tray_id_int = atoi(curr_tray->id.c_str());

                                    if (type_id < 4)
                                    {
                                        curr_tray->is_exists = (obj->tray_exist_bits & (1 << (ams_id_int * 4 + tray_id_int))) != 0 ? true : false;
                                    }
                                    else
                                    {
                                        curr_tray->is_exists = DevUtil::get_flag_bits(obj->tray_exist_bits, 16 + (ams_id_int - 128));
                                    }

                                }
                            }
                            catch (...)
                            {
                            }
                            if (tray_it->contains("setting_id"))
                            {
                                curr_tray->filament_setting_id = (*tray_it)["setting_id"].get<std::string>();
                            }
                            auto curr_time = std::chrono::system_clock::now();
                            auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(curr_time - obj->extrusion_cali_set_hold_start);
                            if (diff.count() > HOLD_TIMEOUT || diff.count() < 0
                                || ams_id_int != (obj->extrusion_cali_set_tray_id / 4)
                                || tray_id_int != (obj->extrusion_cali_set_tray_id % 4))
                            {
                                if (tray_it->contains("k"))
                                {
                                    curr_tray->k = (*tray_it)["k"].get<float>();
                                }
                                if (tray_it->contains("n"))
                                {
                                    curr_tray->n = (*tray_it)["n"].get<float>();
                                }
                            }

                            std::string temp = tray_it->dump();

                            if (tray_it->contains("cali_idx"))
                            {
                                curr_tray->cali_idx = (*tray_it)["cali_idx"].get<int>();
                            }
                        }
                        // remove not in trayList
                        for (auto tray_it = tray_id_set.begin(); tray_it != tray_id_set.end(); tray_it++)
                        {
                            std::string tray_id = *tray_it;
                            auto tray = curr_ams->GetTrays().find(tray_id);
                            if (tray != curr_ams->GetTrays().end())
                            {
                                curr_ams->m_trays.erase(tray_id);
                                BOOST_LOG_TRIVIAL(trace) << "parse_json: remove ams_id=" << ams_id << ", tray_id=" << tray_id;
                            }
                        }
                    }
                }
                // remove not in amsList
                for (auto it = ams_id_set.begin(); it != ams_id_set.end(); it++)
                {
                    std::string ams_id = *it;
                    auto ams = system->amsList.find(ams_id);
                    if (ams != system->amsList.end())
                    {
                        BOOST_LOG_TRIVIAL(trace) << "parse_json: remove ams_id=" << ams_id;
                        system->amsList.erase(ams_id);
                    }
                }
            }
        }
    }
}

}