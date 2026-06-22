#include "libslic3r/PrintConfig.hpp"
#include "MakerbotTranslator.hpp"
#include <cmath>

namespace Slic3r {

MakerbotTranslator::MakerbotTranslator() {
    toolpath_array = nlohmann::json::array();
}

void MakerbotTranslator::set_relative_extrusion(bool is_relative) {
    is_relative_e = is_relative;
}

void MakerbotTranslator::translate_move(double x, double y, double z, double e, double f_min, const std::string& feature_tag) {
    if (is_relative_e) {
        current_a += e;
    } else {
        current_a += (e - last_slicer_e);
        last_slicer_e = e;
    }

    // Convert scaling vectors from standard mm/min to machine-native mm/s
    double f_sec = f_min / 60.0;

    nlohmann::json move_cmd = {
        {"command", {
            {"function", "move"},
            {"metadata", {
                {"relative", {{"a", false}, {"x", false}, {"y", false}, {"z", false}}}
            }},
            {"parameters", {
                {"x", std::round(x * 10000.0) / 10000.0},
                {"y", std::round(y * 10000.0) / 10000.0},
                {"z", std::round(z * 10000.0) / 10000.0},
                {"a", std::round(current_a * 10000.0) / 10000.0},
                {"feedrate", std::round(f_sec * 1000.0) / 1000.0}
            }},
            {"tags", {feature_tag}}
        }}
    };
    toolpath_array.push_back(move_cmd);
}

void MakerbotTranslator::translate_custom_command(const std::string& function_name, const nlohmann::json& parameters) {
    nlohmann::json cmd = {
        {"command", {
            {"function", function_name},
            {"parameters", parameters},
            {"tags", nlohmann::json::array()}
        }}
    };
    toolpath_array.push_back(cmd);
}

const nlohmann::json& MakerbotTranslator::get_toolpath() const {
    return toolpath_array;
}

void MakerbotTranslator::clear() {
    toolpath_array.clear();
    current_a = 0.0;
    last_slicer_e = 0.0;
}

nlohmann::json MakerbotTranslator::generate_meta(const DynamicPrintConfig& config, const std::string& bot_type) const {
    nlohmann::json meta;
    meta["version"] = "1.0.0";
    meta["bot_type"] = bot_type;
    meta["extruder_materials"] = nlohmann::json::array({"pla"});

    if (config.has("nozzle_temperature")) {
        auto nozzle_temp = config.opt<ConfigOptionInts>("nozzle_temperature");
        if (nozzle_temp && !nozzle_temp->values.empty()) {
            int temp = nozzle_temp->values[0];
            meta["extruder_temperature"] = temp;
            meta["extruder_temperatures"] = nlohmann::json::array({temp});
            meta["toolhead_0_temperature"] = temp;
        }
    }

    if (config.has("chamber_temperature")) {
        auto chamber_temp = config.opt<ConfigOptionInts>("chamber_temperature");
        if (chamber_temp && !chamber_temp->values.empty()) {
            meta["chamber_temperature"] = chamber_temp->values[0];
        }
    }

    nlohmann::json miracle_config;
    miracle_config["_bot"] = bot_type;
    
    if (config.has("layer_height")) {
        auto layer_height = config.opt<ConfigOptionFloat>("layer_height");
        if (layer_height) miracle_config["layerHeight"] = layer_height->value;
    }
    if (config.has("wall_loops")) {
        auto wall_loops = config.opt<ConfigOptionInt>("wall_loops");
        if (wall_loops) miracle_config["numberOfShells"] = wall_loops->value;
    }

    meta["miracle_config"] = std::move(miracle_config);
    return meta;
}

} // namespace Slic3r