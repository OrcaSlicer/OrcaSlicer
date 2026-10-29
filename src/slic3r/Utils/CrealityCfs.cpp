#include "CrealityCfs.hpp"
#include "MoonrakerPrinterAgent.hpp"

#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r {

static std::string map_creality_material_id(const std::string& material_id)
{
    static const std::map<std::string, std::string> types = {
        {"000001", "PLA"}, {"000002", "ABS"}, {"000003", "PETG"},
        {"000004", "TPU"}, {"000005", "ASA"}, {"000006", "PA"},
        {"000007", "PC"}, {"000008", "PLA-CF"}, {"000009", "PETG-CF"},
        {"000010", "PA-CF"},
    };
    auto it = types.find(material_id);
    return it == types.end() ? std::string() : it->second;
}

// K2-family firmware reports branded/RFID spools with ids outside the generic
// 0000xx range (a Creality CR-Silk PLA reports "005001"). Those printers also
// publish the resolved name in box.same_material, whose entries are
// [material_id, color, [slots], type_name] - use it as a fallback so a slot is
// not dropped just because its id is missing from the static table above.
static std::map<std::string, std::string> creality_same_material_types(const nlohmann::json& box)
{
    std::map<std::string, std::string> types;
    auto it = box.find("same_material");
    if (it == box.end() || !it->is_array())
        return types;

    for (const auto& entry : *it) {
        if (!entry.is_array() || entry.size() < 4 || !entry[0].is_string() || !entry[3].is_string())
            continue;
        const std::string id   = entry[0].get<std::string>();
        const std::string type = entry[3].get<std::string>();
        if (!id.empty() && !type.empty())
            types.emplace(id, type);
    }
    return types;
}

bool parse_creality_cfs_response(const std::string& response, std::vector<CrealityCfsSlot>& slots)
{
    slots.clear();
    const auto json = nlohmann::json::parse(response, nullptr, false, true);
    if (json.is_discarded() || !json.is_object() || !json.contains("result") ||
        !json["result"].is_object() || !json["result"].contains("status") ||
        !json["result"]["status"].is_object() || !json["result"]["status"].contains("box") ||
        !json["result"]["status"]["box"].is_object())
        return false;

    const auto& box = json["result"]["status"]["box"];
    const auto same_material_types = creality_same_material_types(box);
    for (int tray_idx = 1; tray_idx <= 4; ++tray_idx) {
        auto it = box.find("T" + std::to_string(tray_idx));
        if (it == box.end() || !it->is_object() || MoonrakerPrinterAgent::safe_json_string(*it, "state") != "connect")
            continue;

        const auto& tray = *it;
        const int base_slot = (tray_idx - 1) * 4;
        for (int slot_i = 0; slot_i < 4; ++slot_i) {
            const std::string material_id =
                tray.contains("material_type") ? MoonrakerPrinterAgent::safe_array_string(tray["material_type"], slot_i) : "";
            std::string type = map_creality_material_id(material_id);
            if (type.empty()) {
                auto same_it = same_material_types.find(material_id);
                if (same_it != same_material_types.end())
                    type = same_it->second;
            }
            if (type.empty())
                continue;

            std::string color =
                tray.contains("color_value") ? MoonrakerPrinterAgent::safe_array_string(tray["color_value"], slot_i) : "";
            // Creality prefixes six-digit RGB colors with a leading zero.
            if (color.size() == 7 && color.front() == '0')
                color.erase(0, 1);
            slots.push_back({base_slot + slot_i, type, color});
        }
    }
    return true;
}

} // namespace Slic3r
