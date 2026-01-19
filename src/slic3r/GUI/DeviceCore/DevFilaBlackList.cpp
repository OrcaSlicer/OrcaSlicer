#include <nlohmann/json.hpp>

#include "DevFilaBlackList.h"
#include "DevFilaSystem.h"
#include "DevManager.h"

#include "libslic3r/Utils.hpp"

#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"

using namespace nlohmann;

namespace Slic3r {

json DevFilaBlacklist::filaments_blacklist = json::object();


bool DevFilaBlacklist::load_filaments_blacklist_config()
{
    if (!filaments_blacklist.empty())
    {
        return false;
    }

    filaments_blacklist = json::object();

    std::string config_file = Slic3r::resources_dir() + "/printers/filaments_blacklist.json";
    boost::nowide::ifstream json_file(config_file.c_str());

    try {
        if (json_file.is_open()) {
            json_file >> filaments_blacklist;
            return true;
        }
        else {
            BOOST_LOG_TRIVIAL(error) << "load filaments blacklist config failed";
        }
    }
    catch (...) {
        BOOST_LOG_TRIVIAL(error) << "load filaments blacklist config failed";
        return false;
    }
    return true;
}



static std::string _get_filament_name_from_ams(int ams_id, int slot_id)
{
    std::string name;
    DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) { return name; }

    MachineObject* obj = dev->get_selected_machine();
    if (obj == nullptr) { return name; }

    if (ams_id < 0 || slot_id < 0)
    {
        return name;
    }

    const auto tray = obj->GetFilaSystem()->GetAmsTray(std::to_string(ams_id), std::to_string(slot_id));
    if (!tray) { return name; }

    std::string filament_id = tray->setting_id;

    PresetBundle* preset_bundle = GUI::wxGetApp().preset_bundle;
    auto          option = preset_bundle->get_filament_by_filament_id(filament_id);
    name = option ? option->filament_name : "";
    return name;
}

// moved from tao.wang and zhimin.zeng
void check_filaments(std::string  model_id,
                     std::string  tag_vendor,
                     std::string  tag_type,
                     int          ams_id,
                     int          slot_id,
                     std::string  tag_name,
                     bool& in_blacklist,
                     std::string& ac,
                     wxString& info,
                     wxString& wiki_url)
{
    if (tag_name.empty())
    {
        tag_name = _get_filament_name_from_ams(ams_id, slot_id);
    }

    in_blacklist = false;
    std::transform(tag_vendor.begin(), tag_vendor.end(), tag_vendor.begin(), ::tolower);
    std::transform(tag_type.begin(), tag_type.end(), tag_type.begin(), ::tolower);
    std::transform(tag_name.begin(), tag_name.end(), tag_name.begin(), ::tolower);

    DevFilaBlacklist::load_filaments_blacklist_config();
    if (DevFilaBlacklist::filaments_blacklist.contains("blacklist"))
    {
        for (auto filament_item : DevFilaBlacklist::filaments_blacklist["blacklist"])
        {

            std::string vendor = filament_item.contains("vendor") ? filament_item["vendor"].get<std::string>() : "";
            std::string type = filament_item.contains("type") ? filament_item["type"].get<std::string>() : "";
            std::string type_suffix = filament_item.contains("type_suffix") ? filament_item["type_suffix"].get<std::string>() : "";
            std::string name = filament_item.contains("name") ? filament_item["name"].get<std::string>() : "";
            std::string slot = filament_item.contains("slot") ? filament_item["slot"].get<std::string>() : "";
            std::vector<std::string> model_ids = filament_item.contains("model_id") ? filament_item["model_id"].get<std::vector<std::string>>() : std::vector<std::string>();
            std::string action = filament_item.contains("action") ? filament_item["action"].get<std::string>() : "";
            std::string description = filament_item.contains("description") ? filament_item["description"].get<std::string>() : "";

            // check model id
            if (!model_ids.empty() && std::find(model_ids.begin(), model_ids.end(), model_id) == model_ids.end()) { continue; }

            // check vendor
            std::transform(vendor.begin(), vendor.end(), vendor.begin(), ::tolower);
            if (!vendor.empty())
            {
                if ((vendor == "bambu lab" && (tag_vendor == vendor)) ||
                    (vendor == "third party" && (tag_vendor != "bambu lab")))
                {
                    // Do nothing
                }
                else
                {
                    continue;
                }
            }

            // check type
            std::transform(type.begin(), type.end(), type.begin(), ::tolower);
            if (!type.empty() && (type != tag_type)) { continue; }

            // check type suffix
            std::transform(type_suffix.begin(), type_suffix.end(), type_suffix.begin(), ::tolower);
            if (!type_suffix.empty())
            {
                if (tag_type.length() < type_suffix.length()) { continue; }
                if ((tag_type.substr(tag_type.length() - type_suffix.length()) != type_suffix)) { continue; }
            }

            // check name
            std::transform(name.begin(), name.end(), name.begin(), ::tolower);
            if (!name.empty() && (name != tag_name)) { continue; }

            // check loc
            if (!slot.empty())
            {
                bool is_virtual_slot = devPrinterUtil::IsVirtualSlot(ams_id);
                bool check_virtual_slot = (slot == "ext");
                bool check_ams_slot = (slot == "ams");
                if (is_virtual_slot && !check_virtual_slot)
                {
                    continue;
                }
                else if (!is_virtual_slot && !check_ams_slot)
                {
                    continue;
                }
            }

            if (GUI::wxGetApp().app_config->get("skip_ams_blacklist_check") == "true") {
                action = "warning";
            }

            in_blacklist = true;
            ac = action;
            info = _L(description);
            wiki_url = filament_item.contains("wiki") ? filament_item["wiki"].get<std::string>() : "";
            return;

            // Error in description
            L("TPU is not supported by AMS.");
            L("AMS does not support 'Bambu Lab PET-CF'.");

            // Warning in description
            L("Please cold pull before printing TPU to avoid clogging. You may use cold pull maintenance on the printer.");
            L("Damp PVA will become flexible and get stuck inside AMS, please take care to dry it before use.");
            L("Damp PVA is flexible and may get stuck in extruder. Dry it before use.");
            L("The rough surface of PLA Glow can accelerate wear on the AMS system, particularly on the internal components of the AMS Lite.");
            L("CF/GF filaments are hard and brittle, it's easy to break or get stuck in AMS, please use with caution.");
            L("PPS-CF is brittle and could break in bended PTFE tube above Toolhead.");
            L("PPA-CF is brittle and could break in bended PTFE tube above Toolhead.");
        }
    }
}



void DevFilaBlacklist::check_filaments_in_blacklist(std::string model_id,
                                                    std::string tag_vendor,
                                                    std::string tag_type,
                                                    const std::string& filament_id,
                                                    int                ams_id,
                                                    int                slot_id,
                                                    std::string        tag_name,
                                                    bool& in_blacklist,
                                                    std::string& ac,
                                                    wxString& info)
{
    wxString wiki_url;
    check_filaments_in_blacklist_url(model_id, tag_vendor, tag_type, filament_id, ams_id, slot_id, tag_name, in_blacklist, ac, info, wiki_url);
}


bool check_filaments_printable(const std::string &tag_vendor, const std::string &tag_type, const std::string& filament_id, int ams_id, bool &in_blacklist, std::string &ac, wxString &info)
{
    DeviceManager *dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) {
        return true;
    }

    MachineObject *obj = dev->get_selected_machine();
    if (obj == nullptr || !obj->is_multi_extruders()) {
        return true;
    }

    Preset *printer_preset = GUI::get_printer_preset(obj);
    if (!printer_preset)
        return true;

    ConfigOptionInts *physical_extruder_map_op = dynamic_cast<ConfigOptionInts *>(printer_preset->config.option("physical_extruder_map"));
    if (!physical_extruder_map_op)
        return true;
    std::vector<int> physical_extruder_maps = physical_extruder_map_op->values;
    int obj_extruder_id = obj->get_extruder_id_by_ams_id(std::to_string(ams_id));
    int extruder_idx = obj_extruder_id;
    for (int index = 0; index < physical_extruder_maps.size(); ++index) {
        if (physical_extruder_maps[index] == obj_extruder_id) {
            extruder_idx = index;
            break;
        }
    }

    PresetBundle *preset_bundle = GUI::wxGetApp().preset_bundle;
    std::optional<FilamentBaseInfo> filament_info = preset_bundle->get_filament_by_filament_id(filament_id, printer_preset->name);
    if (filament_info.has_value() && !(filament_info->filament_printable >> extruder_idx & 1)) {
        wxString extruder_name = extruder_idx == 0 ? _L("left") : _L("right");
        ac                     = "prohibition";
        info                   = wxString::Format(_L("%s is not supported by %s extruder."), tag_type, extruder_name);
        in_blacklist           = true;
        return false;
    }

    return true;
}

void DevFilaBlacklist::check_filaments_in_blacklist_url(std::string model_id, std::string tag_vendor, std::string tag_type, const std::string& filament_id, int ams_id, int slot_id, std::string tag_name, bool& in_blacklist, std::string& ac, wxString& info, wxString& wiki_url)
{
    if (ams_id < 0 || slot_id < 0)
    {
        return;
    }

    if (!check_filaments_printable(tag_vendor, tag_type, filament_id, ams_id, in_blacklist, ac, info))
    {
        return;
    }

    check_filaments(model_id, tag_vendor, tag_type, ams_id, slot_id, tag_name, in_blacklist, ac, info, wiki_url);
}

}