#include <nlohmann/json.hpp>

#include "DevConfig.h"
#include "DevUtil.h"

using namespace nlohmann;

namespace Slic3r
{

void DevConfig::ParseConfig(const json& print_json)
{
    ParseChamberConfig(print_json);
    ParsePrintOptionsConfig(print_json);
    ParseCalibrationConfig(print_json);

}

void DevConfig::ParseChamberConfig(const json& print_json)
{
    DevJsonValParser::ParseVal(print_json, "support_chamber", m_has_chamber);
    DevJsonValParser::ParseVal(print_json, "support_chamber_temp_edit", m_support_chamber_edit);
    if (m_support_chamber_edit)
    {
        if (print_json.contains("support_chamber_temp_edit_range"))
        {
            const auto &support_champer_range = print_json["support_chamber_temp_edit_range"];
            if (support_champer_range.is_array() && support_champer_range.size() > 1) {
                m_chamber_temp_edit_min = support_champer_range[0];
                m_chamber_temp_edit_max = support_champer_range[1];
            }
        }

        DevJsonValParser::ParseVal(print_json, "support_chamber_temp_switch_heating", m_chamber_temp_switch_heat);
    }
}

void DevConfig::ParsePrintOptionsConfig(const json& print_json)
{
    DevJsonValParser::ParseVal(print_json, "support_first_layer_inspect", m_support_first_layer_inspect);
    DevJsonValParser::ParseVal(print_json, "support_save_remote_print_file_to_storage", m_support_save_remote_print_file_to_storage);
    DevJsonValParser::ParseVal(print_json, "support_ai_monitoring", m_support_ai_monitor);
    DevJsonValParser::ParseVal(print_json, "support_print_without_sd", m_support_print_without_sd);
    DevJsonValParser::ParseVal(print_json, "support_print_all", m_support_print_all);
}

void DevConfig::ParseCalibrationConfig(const json& print_json)
{
    DevJsonValParser::ParseVal(print_json, "support_lidar_calibration", m_support_calibration_lidar);
    DevJsonValParser::ParseVal(print_json, "support_nozzle_offset_calibration", m_support_calibration_nozzle_offset);
    DevJsonValParser::ParseVal(print_json, "support_high_tempbed_calibration", m_support_calibration_high_temp_bed);
    DevJsonValParser::ParseVal(print_json, "support_auto_flow_calibration", m_support_calibration_pa_flow_auto);
    DevJsonValParser::ParseVal(print_json, "support_clump_position_calibration", m_support_calibration_clump_pos);
}

}