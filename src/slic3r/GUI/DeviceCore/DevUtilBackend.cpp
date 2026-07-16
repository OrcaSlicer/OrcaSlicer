#include "DevUtilBackend.h"

#include "slic3r/GUI/BackgroundSlicingProcess.hpp"

#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include <boost/log/trivial.hpp>

namespace Slic3r
{

std::shared_ptr<MultiNozzleUtils::NozzleGroupResultBase> DevUtilBackend::GetNozzleGroupResult(Slic3r::GUI::Plater* plater)
{
    if (plater && plater->background_process().get_current_gcode_result()) {
        return plater->background_process().get_current_gcode_result()->nozzle_group_result;
    }

    return nullptr;
}

static std::unordered_map<std::string, DevAmsType> s_ams_type_map = {
    {"0", DevAmsType::N3F},
    {"1", DevAmsType::N3S},
};

std::optional<Slic3r::DevFilamentDryingPreset> DevUtilBackend::GetFilamentDryingPreset(const std::string& fila_id)
{
    if (fila_id.empty() || !GUI::wxGetApp().preset_bundle) {
        return std::nullopt;
    }

    for (auto iter = GUI::wxGetApp().preset_bundle->filaments.begin(); iter != GUI::wxGetApp().preset_bundle->filaments.end(); ++iter) {
        const Preset& filament_preset = *iter;
        const auto& config = filament_preset.config;
        if (filament_preset.filament_id == fila_id) {
            DevFilamentDryingPreset info;
            info.filament_id = fila_id;
            try {
                if (config.has("filament_dev_ams_drying_ams_limitations")) {
                    std::vector<std::string> types = config.option<ConfigOptionStrings>("filament_dev_ams_drying_ams_limitations")->values;
                    for (auto type : types) {
                        if (s_ams_type_map.count(type) == 0) {
                            continue;
                        }
                        info.ams_limitations.insert(s_ams_type_map[type]);
                    }
                }

                if (config.has("filament_dev_ams_drying_temperature")) {
                    info.filament_dev_ams_drying_temperature_on_idle[DevAmsType::N3F] = config.option<ConfigOptionFloats>("filament_dev_ams_drying_temperature")->get_at(0);
                    info.filament_dev_ams_drying_temperature_on_idle[DevAmsType::N3S] = config.option<ConfigOptionFloats>("filament_dev_ams_drying_temperature")->get_at(1);
                    info.filament_dev_ams_drying_temperature_on_print[DevAmsType::N3F] = config.option<ConfigOptionFloats>("filament_dev_ams_drying_temperature")->get_at(2);
                    info.filament_dev_ams_drying_temperature_on_print[DevAmsType::N3S] = config.option<ConfigOptionFloats>("filament_dev_ams_drying_temperature")->get_at(3);
                }

                if (config.has("filament_dev_ams_drying_time")) {
                    info.filament_dev_ams_drying_time_on_idle[DevAmsType::N3F] = config.option<ConfigOptionFloats>("filament_dev_ams_drying_time")->get_at(0);
                    info.filament_dev_ams_drying_time_on_idle[DevAmsType::N3S] = config.option<ConfigOptionFloats>("filament_dev_ams_drying_time")->get_at(1);
                    info.filament_dev_ams_drying_time_on_print[DevAmsType::N3F] = config.option<ConfigOptionFloats>("filament_dev_ams_drying_time")->get_at(2);
                    info.filament_dev_ams_drying_time_on_print[DevAmsType::N3S] = config.option<ConfigOptionFloats>("filament_dev_ams_drying_time")->get_at(3);
                }

                if (config.has("filament_dev_drying_softening_temperature")) {
                    info.filament_dev_drying_softening_temperature = config.option<ConfigOptionFloats>("filament_dev_drying_softening_temperature")->get_at(0);
                }

                if (config.has("filament_dev_ams_drying_heat_distortion_temperature")) {
                    info.filament_dev_ams_drying_heat_distortion_temperature = config.option<ConfigOptionFloats>("filament_dev_ams_drying_heat_distortion_temperature")->get_at(0);
                }

                if (config.has("filament_dev_drying_cooling_temperature")) {
                    info.filament_dev_drying_cooling_temperature = config.option<ConfigOptionFloats>("filament_dev_drying_cooling_temperature")->get_at(0);
                }

                return info;
            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " exception: " << e.what();
            }
        }
    }

    return std::nullopt;
}

}; // namespace Slic3r
