#include "libslic3r/ProjectTask.hpp"
#include <cstdlib>
#include <cassert>
#include <cstdio>
#include <boost/log/trivial.hpp>
#include <limits>

#include <map>
#include <vector>
#include <string>
#include <utility>
#include <wx/app.h>
#include <wx/colour.h>
#include <set>
#include "slic3r/GUI/DeviceCore/DevDefs.h"
#include "DevMapping.h"
#include "DevFilaSystem.h"
#include "DevUtil.h"

// TODO: remove this include
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GuiColor.hpp"

namespace Slic3r
{
    // Bambu keeps its fixed external pair (255 main/right, 254 deputy/left) and its two
    // left/right flags. Every other printer addresses one external virtual tray per live
    // extruder, so ids 255, 254, 253, ... are extruders 0, 1, 2, ... .
    static bool _is_bbl_vendor_printer()
    {
        return wxTheApp != nullptr && GUI::wxGetApp().preset_bundle != nullptr &&
               GUI::wxGetApp().preset_bundle->is_bbl_vendor();
    }

    // Extruder a virtual (external spool) tray id feeds: VIRTUAL_TRAY_MAIN_ID - tray_id.
    // Returns -1 for an empty, non-numeric or out-of-range id.
    static int _virtual_tray_extruder_id(const std::string& tray_id)
    {
        if (tray_id.empty())
            return -1;
        for (char c : tray_id)
        {
            if (c < '0' || c > '9')
                return -1;
        }
        try
        {
            const int vt_id = std::stoi(tray_id);
            if (vt_id < 0 || vt_id > VIRTUAL_TRAY_MAIN_ID)
                return -1;
            return VIRTUAL_TRAY_MAIN_ID - vt_id;
        }
        catch (...)
        {
            return -1;
        }
    }

    // True when obj->vt_slot actually carries this virtual tray id as a reported spool. A synthetic
    // topology filler (is_slot_placeholder) only makes the slot addressable and does not count.
    static bool _has_virtual_tray_slot(const MachineObject* obj, const std::string& tray_id)
    {
        if (obj == nullptr || tray_id.empty())
            return false;
        for (const auto& tray : obj->vt_slot)
        {
            if (!tray.is_slot_placeholder && tray.id == tray_id)
                return true;
        }
        return false;
    }

    // A generic printer may expose one AMS unit / external slot per extruder. When both legacy
    // flags of a category are enabled (the sidebar sync call uses both AMS and both external flags)
    // every extruder participates; when only one side is enabled (SelectMachine's left/right split)
    // keep to that side's legacy extruder (right = main = 0, left = deputy = 1), so the N slots do
    // not leak across sides.
    static bool _generic_extruder_in_selected_side(int extruder_id, bool use_right, bool use_left)
    {
        if (use_right && use_left)
            return true;
        if (use_right)
            return extruder_id == MAIN_EXTRUDER_ID;
        if (use_left)
            return extruder_id == DEPUTY_EXTRUDER_ID;
        return false;
    }

    bool DevMappingUtil::is_valid_mapping_result(const MachineObject* obj, std::vector<FilamentInfo>& result, bool check_empty_slot)
    {
        if (result.empty()) return false;

        const bool is_bbl_vendor = _is_bbl_vendor_printer();

        for (int i = 0; i < result.size(); i++)
        {
            // invalid mapping result
            if (result[i].tray_id < 0)
            {
                if (result[i].ams_id.empty() && result[i].slot_id.empty())
                {
                    return false;
                }
            }
            else
            {

                auto ams_item = obj->GetFilaSystem()->GetAmsById(result[i].ams_id);
                if (ams_item == nullptr)
                {
                    // A virtual (external spool) tray is not an AMS unit. Bambu keeps its fixed pair;
                    // a generic printer accepts whichever per-extruder slots it actually reported.
                    const bool is_virtual_tray = is_bbl_vendor
                        ? (result[i].ams_id == std::to_string(VIRTUAL_TRAY_MAIN_ID) ||
                           result[i].ams_id == std::to_string(VIRTUAL_TRAY_DEPUTY_ID))
                        : _has_virtual_tray_slot(obj, result[i].ams_id);
                    if (!is_virtual_tray)
                    {
                        result[i].tray_id = -1;
                        return false;
                    }
                }
                else
                {
                    if (check_empty_slot)
                    {
                        auto tray_item = ams_item->GetTrays().find(result[i].slot_id);
                        if (tray_item == ams_item->GetTrays().end())
                        {
                            result[i].tray_id = -1;
                            return false;
                        }
                        else
                        {
                            if (!tray_item->second->is_exists)
                            {
                                result[i].tray_id = -1;
                                return false;
                            }
                        }
                    }
                }
            }
        }
        return true;
    }

    // calc distance map
    struct DisValue {
        int  tray_id;
        float distance;
        bool  is_same_color = true;
        bool  is_type_match = true;
    };

    static void _parse_tray_info(int ams_id, int slot_id, DevAms::AmsType type, DevAmsTray tray, FilamentInfo& result)
    {
        result.color = tray.color;
        result.type = tray.get_filament_type();
        result.filament_id = tray.setting_id;
        result.ctype = tray.ctype;
        result.colors = tray.cols;

        /*for new ams mapping*/
        result.ams_id = std::to_string(ams_id);
        result.slot_id = std::to_string(slot_id);

        if (type == DevAms::EXT_SPOOL || ams_id == VIRTUAL_TRAY_MAIN_ID || ams_id == VIRTUAL_TRAY_DEPUTY_ID)
        {
            // An external spool is addressed by its virtual tray id (255 -> extruder 0, 254 -> 1,
            // ...); the id is the tray's identity, so it must not fall through to the AMS
            // ams_id * 4 + slot_id indexing.
            result.tray_id = atoi(tray.id.c_str());
            result.id = atoi(tray.id.c_str());
        }
        else
        {
            if (type == DevAms::N3S)
            {
                result.id = ams_id + slot_id;
            }
            else if (type == DevAms::AMS_LITE_MIXED)
            {
                result.id = AMS_LITE_MIXED_TRAY_INDEX_OFFSET + slot_id;
            }
            else
            {
                result.id = ams_id * 4 + slot_id;
            }
        }
    }

    int DevMappingUtil::ams_filament_mapping(const MachineObject* obj, const std::vector<FilamentInfo>& filaments, std::vector<FilamentInfo>& result, std::vector<bool> map_opt, std::vector<int> exclude_id, bool nozzle_has_ams_then_ignore_ext)
    {
        if (filaments.empty())
            return -1;

        /////////////////////////
        // Step 1: collect filaments in machine
        std::map<int, FilamentInfo> tray_filaments; // tray_index : tray_color
        bool  left_nozzle_has_ams = false, right_nozzle_has_ams = false;

        const bool is_bbl_vendor = _is_bbl_vendor_printer();
        // Extruders whose AMS filament participates in this mapping. On a generic printer this
        // carries the per-extruder "nozzle already has AMS" state that the legacy right/left flags
        // express for Bambu, so the matching external spool can be suppressed.
        std::set<int> generic_extruders_with_ams;

        const auto& ams_list = obj->GetFilaSystem()->GetAmsList();
        for (auto ams = ams_list.begin(); ams != ams_list.end(); ams++)
        {
            std::string ams_id = ams->second->GetAmsId();
            auto        ams_type = ams->second->GetAmsType();
            const int   ams_id_int = atoi(ams->first.c_str());
            int         generic_tray_base = 0;
            if (!is_bbl_vendor) {
                for (const auto& other : ams_list) {
                    if (atoi(other.first.c_str()) < ams_id_int)
                        generic_tray_base += static_cast<int>(other.second->GetTrays().size());
                }
            }
            // GetAmsType() maps mixed -> AMS_LITE; recover the mixed type so N9 trays index at 24+slot.
            if (ams_type == DevAms::AMS_LITE && ams->second->IsAmsLiteMixed())
            {
                ams_type = DevAms::AMS_LITE_MIXED;
            }
            for (auto tray = ams->second->GetTrays().begin(); tray != ams->second->GetTrays().end(); tray++)
            {
                int ams_id = ams_id_int;
                int tray_id = atoi(tray->first.c_str());
                int tray_index = 0;
                if (!is_bbl_vendor)
                {
                    // Generic units can have different lane counts; index all lanes cumulatively,
                    // matching the shared tray-index map and sidebar list builder.
                    tray_index = generic_tray_base + tray_id;
                }
                else if (ams_type == DevAms::AMS || ams_type == DevAms::AMS_LITE || ams_type == DevAms::N3F)
                {
                    tray_index = ams_id * 4 + tray_id;
                }
                else if (ams_type == DevAms::AMS_LITE_MIXED)
                {
                    tray_index = AMS_LITE_MIXED_TRAY_INDEX_OFFSET + tray_id;
                }
                else if (ams_type == DevAms::N3S)
                {
                    tray_index = ams_id + tray_id;
                }
                else
                {
                    assert(0);
                }

                // skip exclude id
                for (int i = 0; i < exclude_id.size(); i++)
                {
                    if (tray_index == exclude_id[i])
                        continue;
                }
                // push
                FilamentInfo info;
                if (tray->second->is_tray_info_ready())
                {
                    _parse_tray_info(ams_id, tray_id, ams_type, *(tray->second), info);
                    // Generic unit lanes use one cumulative index across variable-width units;
                    // ams_id * 4 + slot_id is only valid for Bambu's fixed four-slot units.
                    if (!is_bbl_vendor)
                        info.id = tray_index;
                }

                bool ams_valid = false;
                if (is_bbl_vendor)
                {
                    //first: left,nozzle=1,map=1   second: right,nozzle=0,map=2
                    bool right_ams_valid = (ams->second->GetBindedExtruderSet().count(MAIN_EXTRUDER_ID) != 0) && map_opt[MappingOption::USE_RIGHT_AMS];
                    bool left_ams_valid = (ams->second->GetBindedExtruderSet().count(DEPUTY_EXTRUDER_ID) != 0) && map_opt[MappingOption::USE_LEFT_AMS];
                    if (right_ams_valid)
                    {
                        right_nozzle_has_ams = true;
                    }
                    if (left_ams_valid)
                    {
                        left_nozzle_has_ams = true;
                    }
                    ams_valid = right_ams_valid || left_ams_valid;
                }
                else
                {
                    // A generic unit is bound to its own extruder, so it takes part whenever the
                    // request covers that extruder.
                    for (int extruder_id : ams->second->GetBindedExtruderSet())
                    {
                        if (_generic_extruder_in_selected_side(extruder_id, map_opt[MappingOption::USE_RIGHT_AMS], map_opt[MappingOption::USE_LEFT_AMS]))
                        {
                            ams_valid = true;
                            generic_extruders_with_ams.insert(extruder_id);
                        }
                    }
                }

                if (ams_valid)
                {
                    tray_filaments.emplace(std::make_pair(tray_index, info));
                }
            }
        }

        if (map_opt[MappingOption::USE_RIGHT_EXT] || map_opt[MappingOption::USE_LEFT_EXT])
        {
            for (auto tray : obj->vt_slot)
            {
                // A synthetic topology filler is not a reported external spool, so it is never a
                // mapping candidate. Bambu keeps its fixed reported pair.
                if (!is_bbl_vendor && tray.is_slot_placeholder)
                {
                    continue;
                }
                if (is_bbl_vendor)
                {
                    // Bambu keeps its fixed pair and the two left/right external flags.
                    const bool right_ext_valid = (tray.id == std::to_string(VIRTUAL_TRAY_MAIN_ID) && map_opt[MappingOption::USE_RIGHT_EXT]);
                    const bool left_ext_valid = (tray.id == std::to_string(VIRTUAL_TRAY_DEPUTY_ID) && map_opt[MappingOption::USE_LEFT_EXT]);
                    if (!right_ext_valid && !left_ext_valid)
                    {
                        continue;
                    }
                    if (nozzle_has_ams_then_ignore_ext)
                    {
                        if (right_ext_valid && right_nozzle_has_ams)
                        {
                            continue;
                        }
                        if (left_ext_valid && left_nozzle_has_ams)
                        {
                            continue;
                        }
                    }
                }
                else
                {
                    // Generic: every present virtual tray is the external spool of the extruder it
                    // addresses (VIRTUAL_TRAY_MAIN_ID - tray.id): 255 -> 0, 254 -> 1, ... . Keep to
                    // the selected side when only one legacy flag is enabled, and drop the spool when
                    // its extruder already has AMS filament in this mapping and suppression is on.
                    const int extruder_id = _virtual_tray_extruder_id(tray.id);
                    if (extruder_id < 0)
                    {
                        continue;
                    }
                    if (!_generic_extruder_in_selected_side(extruder_id, map_opt[MappingOption::USE_RIGHT_EXT], map_opt[MappingOption::USE_LEFT_EXT]))
                    {
                        continue;
                    }
                    if (nozzle_has_ams_then_ignore_ext && generic_extruders_with_ams.count(extruder_id) != 0)
                    {
                        continue;
                    }
                }

                FilamentInfo info;
                _parse_tray_info(atoi(tray.id.c_str()), 0, DevAms::EXT_SPOOL, tray, info);
                tray_filaments.emplace(std::make_pair(info.tray_id, info));
            }
        }

        /////////////////////////
        // Step 2: collect the distances of filaments_in_slicing to filaments_in_machine
        char buffer[256];
        std::vector<std::vector<DisValue>> distance_map;

        // print title
        ::sprintf(buffer, "F(id)");
        std::string line = std::string(buffer);
        for (auto tray = tray_filaments.begin(); tray != tray_filaments.end(); tray++)
        {
            ::sprintf(buffer, "   AMS%02d", tray->second.id + 1);
            line += std::string(buffer);
        }
        BOOST_LOG_TRIVIAL(info) << "ams_mapping_distance:" << line;// Print the collected filaments

        for (int i = 0; i < filaments.size(); i++)
        {
            std::vector<DisValue> rol;
            ::sprintf(buffer, "F(%02d)", filaments[i].id + 1);
            line = std::string(buffer);
            for (auto tray = tray_filaments.begin(); tray != tray_filaments.end(); tray++)
            {
                DisValue val;
                val.tray_id = tray->second.id;
                wxColour c = wxColour(filaments[i].color);
                wxColour tray_c = DevAmsTray::decode_color(tray->second.color);
                val.distance = GUI::calc_color_distance(c, tray_c);
                if (filaments[i].type != tray->second.type)
                {
                    val.distance = 999999;
                    val.is_type_match = false;
                }
                else
                {
                    if (c.Alpha() != tray_c.Alpha())
                        val.distance = 999999;
                    val.is_type_match = true;
                }
                ::sprintf(buffer, "  %6.0f", val.distance);
                line += std::string(buffer);
                rol.push_back(val);
            }
            BOOST_LOG_TRIVIAL(info) << "ams_mapping_distance:" << line;
            distance_map.push_back(rol);
        }

        /////////////////////////
        // Step 3: do mapping algorithm

        // setup the mapping result
        for (int i = 0; i < filaments.size(); i++)
        {
            FilamentInfo info;
            info.id = filaments[i].id;
            info.tray_id = -1;
            info.type = filaments[i].type;
            info.filament_id = filaments[i].filament_id;
            result.push_back(info);
        }

        // traverse the mapping
        std::set<int> picked_src;
        std::set<int> picked_tar;
        for (int k = 0; k < distance_map.size(); k++)
        {
            float min_val = std::numeric_limits<float>::max();
            int picked_src_idx = -1;
            int picked_tar_idx = -1;
            for (int i = 0; i < distance_map.size(); i++)
            {
                if (picked_src.find(i) != picked_src.end())
                    continue;

                // try to mapping to different tray
                for (int j = 0; j < distance_map[i].size(); j++)
                {
                    if (picked_tar.find(j) != picked_tar.end())
                    {
                        if (distance_map[i][j].is_same_color
                            && distance_map[i][j].is_type_match
                            && distance_map[i][j].distance < (float)0.0001)
                        {
                            min_val = distance_map[i][j].distance;
                            picked_src_idx = i;
                            picked_tar_idx = j;
                            tray_filaments[picked_tar_idx].distance = min_val;
                        }
                        continue;
                    }

                    if (distance_map[i][j].is_same_color
                        && distance_map[i][j].is_type_match)
                    {
                        if (min_val > distance_map[i][j].distance)
                        {

                            min_val = distance_map[i][j].distance;
                            picked_src_idx = i;
                            picked_tar_idx = j;
                            tray_filaments[picked_tar_idx].distance = min_val;
                        }
                        else if (min_val == distance_map[i][j].distance && filaments[picked_src_idx].filament_id != tray_filaments[picked_tar_idx].filament_id && filaments[i].filament_id == tray_filaments[j].filament_id)
                        {

                            picked_src_idx = i;
                            picked_tar_idx = j;
                        }
                    }
                }

                // take a retry to mapping to used tray
                if (picked_src_idx < 0 || picked_tar_idx < 0)
                {
                    for (int j = 0; j < distance_map[i].size(); j++)
                    {
                        if (distance_map[i][j].is_same_color && distance_map[i][j].is_type_match)
                        {
                            if (min_val > distance_map[i][j].distance)
                            {
                                min_val = distance_map[i][j].distance;
                                picked_src_idx = i;
                                picked_tar_idx = j;
                                tray_filaments[picked_tar_idx].distance = min_val;
                            }
                            else if (min_val == distance_map[i][j].distance && filaments[picked_src_idx].filament_id != tray_filaments[picked_tar_idx].filament_id && filaments[i].filament_id == tray_filaments[j].filament_id)
                            {
                                picked_src_idx = i;
                                picked_tar_idx = j;
                            }
                        }
                    }
                }
            }

            if (picked_src_idx >= 0 && picked_tar_idx >= 0)
            {
                auto tray = tray_filaments.find(distance_map[k][picked_tar_idx].tray_id);

                if (tray != tray_filaments.end())
                {
                    result[picked_src_idx].tray_id = tray->first;

                    result[picked_src_idx].color = tray->second.color;
                    result[picked_src_idx].type = tray->second.type;
                    result[picked_src_idx].distance = tray->second.distance;
                    result[picked_src_idx].filament_id = tray->second.filament_id;
                    result[picked_src_idx].ctype = tray->second.ctype;
                    result[picked_src_idx].colors = tray->second.colors;

                    /*for new ams mapping*/
                    result[picked_src_idx].ams_id = tray->second.ams_id;
                    result[picked_src_idx].slot_id = tray->second.slot_id;
                }

                ::sprintf(buffer, "ams_mapping, picked F(%02d) AMS(%02d), distance=%6.0f", picked_src_idx + 1, picked_tar_idx + 1,
                    distance_map[picked_src_idx][picked_tar_idx].distance);
                BOOST_LOG_TRIVIAL(info) << std::string(buffer);
                picked_src.insert(picked_src_idx);
                picked_tar.insert(picked_tar_idx);
            }
        }

        // Orca: special cases that no AMS available, we select ext slot automatically because we don't have other choice anyway
        if (tray_filaments.size() == 1 && devPrinterUtil::IsVirtualSlot(tray_filaments.begin()->first)) {
            auto ext_tray = tray_filaments.begin();
            for (auto & r : result) {
                if (r.tray_id < 0) {
                    r.tray_id = ext_tray->first;

                    r.color = ext_tray->second.color;
                    r.type = ext_tray->second.type;
                    r.distance = ext_tray->second.distance;
                    r.filament_id = ext_tray->second.filament_id;
                    r.ctype = ext_tray->second.ctype;
                    r.colors = ext_tray->second.colors;

                    /*for new ams mapping*/
                    r.ams_id = ext_tray->second.ams_id;
                    r.slot_id = ext_tray->second.slot_id;
                }
            }
        }

        //check ams mapping result
        if (DevMappingUtil::is_valid_mapping_result(obj, result, true))
        {
            return 0;
        }

        /* for (auto it = result.begin(); it != result.end(); it++) {//This code has never been effective before 2025.03.18
             if (it->distance >= 6000) {
                 it->tray_id = -1;
             }
         }*/
        return 0;
    }

}
