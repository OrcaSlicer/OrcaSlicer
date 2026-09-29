#pragma once

#include <optional>
#include <wx/string.h>
#include "slic3r/Utils/json_diff.hpp"

namespace Slic3r
{
class MachineObject;
class DevFilaBlacklist
{
public:
    // Struct-in / struct-out check API that accumulates all matching rules,
    // replacing the earlier out-param, first-match overloads.
    // The per-nozzle dimensions (extruder_id/nozzle_flow/nozzle_diameter), calib_mode and
    // has_filament_switch are currently left unset, so the engine behaves exactly as the
    // old first-match parser for the shipping rule set.
    struct CheckFilamentInfo
    {
        std::string dev_id;
        std::string model_id;

        std::string fila_id;
        std::string fila_type;
        std::string fila_name;
        std::string fila_vendor;

        std::string calib_mode;
        bool has_filament_switch = false;

        std::optional<bool> used_for_print_support;// optional
        std::optional<bool> used_for_print_object;// optional

        int ams_id;
        int slot_id;

        std::optional<int> extruder_id;// optional
        std::optional<std::string> nozzle_flow;// optional
        std::optional<float> nozzle_diameter;// optional
    };

    struct CheckResultItem
    {
        std::string action;// warning/prohibition
        wxString    info_msg;
        wxString    wiki_url;
    };

    struct CheckResult
    {
        std::map<std::string, std::vector<CheckResultItem>> action_items;
        std::vector<CheckResultItem> get_items_by_action(const std::string& action) const
        {
            auto it = action_items.find(action);
            if (it != action_items.end()) {
                return it->second;
            }
            return std::vector<CheckResultItem>();
        }
    };

public:
    static bool load_filaments_blacklist_config();
    static CheckResult check_filaments_in_blacklist(const CheckFilamentInfo& info);

public:
    static json filaments_blacklist;
};// class DevFilaBlacklist

}// namespace Slic3r
