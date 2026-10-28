#include "CustomGCode.hpp"
#include <vector>
#include <utility>
#include <cstddef>
#include <cassert>
#include <map>
#include <string>
#include <nlohmann/json.hpp>

namespace Slic3r {

namespace CustomGCode {

void Item::from_json(const nlohmann::json& j) {
    std::string type_str;
    j.at("type").get_to(type_str);
    std::map<std::string,Type> str2type = { {"ColorChange", ColorChange },
        {"PausePrint",PausePrint},
        {"ToolChange",ToolChange},
        {"Template",Template},
        {"Custom",Custom},
        {"Unknown",Unknown} };
    type = Unknown;
    if (str2type.find(type_str) != str2type.end())
        type = str2type[type_str];
    j.at("print_z").get_to(print_z);
    j.at("color").get_to(color);
    j.at("extruder").get_to(extruder);
    if(j.contains("extra"))
        j.at("extra").get_to(extra);
}

void Info::from_json(const nlohmann::json& j) {
    std::string mode_str;
    if (j.contains("mode"))
        j.at("mode").get_to(mode_str);
    if (mode_str == "SingleExtruder") mode = SingleExtruder;
    else if (mode_str == "MultiAsSingle") mode = MultiAsSingle;
    else if (mode_str == "MultiExtruder") mode = MultiExtruder;
    else mode = Undef;

    auto j_gcodes = j.at("gcodes");
    gcodes.reserve(j_gcodes.size());
    for (auto& jj : j_gcodes) {
        Item item;
        item.from_json(jj);
        gcodes.push_back(item);
    }
}

//BBS: useless config and function
#if 0
// If loaded configuration has a "colorprint_heights" option (if it was imported from older Slicer), 
// and if CustomGCode::Info.gcodes is empty (there is no color print data available in a new format
// then CustomGCode::Info.gcodes should be updated considering this option.
extern void update_custom_gcode_per_print_z_from_config(Info& info, DynamicPrintConfig* config)
{
	auto *colorprint_heights = config->option<ConfigOptionFloats>("colorprint_heights");
    if (colorprint_heights == nullptr)
        return;
    if (info.gcodes.empty() && ! colorprint_heights->values.empty()) {
		// Convert the old colorprint_heighs only if there is no equivalent data in a new format.
        const std::vector<std::string>& colors = ColorPrintColors::get();
        const auto& colorprint_values = colorprint_heights->values;
        info.gcodes.clear();
        info.gcodes.reserve(colorprint_values.size());
        int i = 0;
        for (auto val : colorprint_values)
            info.gcodes.emplace_back(Item{ val, ColorChange, 1, colors[(++i)%7] });

        info.mode = SingleExtruder;
	}

	// The "colorprint_heights" config value has been deprecated. At this point of time it has been converted
	// to a new format and therefore it shall be erased.
    config->erase("colorprint_heights");
}
#endif

// If information for custom Gcode per print Z was imported from older Slicer, mode will be undefined.
// So, we should set CustomGCode::Info.mode should be updated considering code values from items.
extern void check_mode_for_custom_gcode_per_print_z(Info& info)
{
    if (info.mode != Undef)
        return;

    bool is_single_extruder = true;
    for (auto item : info.gcodes) 
    {
        if (item.type == ToolChange) {
            info.mode = MultiAsSingle;
            return;
        }
        if (item.type == ColorChange && item.extruder > 1)
            is_single_extruder = false;
    }

    info.mode = is_single_extruder ? SingleExtruder : MultiExtruder;
}

// Return pairs of <print_z, 1-based extruder ID> sorted by increasing print_z from custom_gcode_per_print_z.
// print_z corresponds to the first layer printed with the new extruder.
std::vector<std::pair<double, unsigned int>> custom_tool_changes(const Info& custom_gcode_per_print_z, size_t num_extruders)
{
    std::vector<std::pair<double, unsigned int>> custom_tool_changes;
    for (const Item& custom_gcode : custom_gcode_per_print_z.gcodes)
        if (custom_gcode.type == ToolChange) {
            // If extruder count in PrinterSettings was changed, use default (0) extruder for extruders, more than num_extruders
            assert(custom_gcode.extruder >= 0);
            custom_tool_changes.emplace_back(custom_gcode.print_z, static_cast<unsigned int>(size_t(custom_gcode.extruder) > num_extruders ? 1 : custom_gcode.extruder));
        }
    return custom_tool_changes;
}

} // namespace CustomGCode

} // namespace Slic3r
