#include "SpectrumAutoGraft.hpp"
#include "MixedFilament.hpp"

#include <algorithm>

#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>

namespace Slic3r {

bool spectrum_is_zr_ultra_s_dest(const DynamicPrintConfig &cfg)
{
    const ConfigOptionString *model = cfg.option<ConfigOptionString>("printer_model");
    const ConfigOptionString *settings_id = cfg.option<ConfigOptionString>("printer_settings_id");
    const bool name_match =
        (model != nullptr && model->value == "WonderMaker ZR Ultra S") ||
        (settings_id != nullptr && boost::contains(settings_id->value, "WonderMaker ZR Ultra"));
    if (!name_match)
        return false;

    if (const ConfigOptionFloats *nozzles = cfg.option<ConfigOptionFloats>("nozzle_diameter")) {
        if (nozzles->values.size() != 4)
            BOOST_LOG_TRIVIAL(warning) << "spectrum_is_zr_ultra_s_dest: expected 4 nozzles, got "
                                       << nozzles->values.size();
    }
    if (const ConfigOptionBool *semm = cfg.option<ConfigOptionBool>("single_extruder_multi_material")) {
        if (semm->value)
            BOOST_LOG_TRIVIAL(warning) << "spectrum_is_zr_ultra_s_dest: expected SEMM=0, got 1";
    }
    return true;
}

bool spectrum_should_auto_graft_leq4(const DynamicPrintConfig &cfg)
{
    const ConfigOptionStrings *colours = cfg.option<ConfigOptionStrings>("filament_colour");
    if (colours == nullptr)
        return false;
    const size_t n = colours->values.size();
    if (n < 1 || n > 4)
        return false;
    return !spectrum_is_zr_ultra_s_dest(cfg);
}

bool spectrum_auto_graft_load_is_project_open(bool load_model, bool load_config, bool is_restore)
{
    return load_model && load_config && !is_restore;
}

bool spectrum_restore_dest_filament_colours(std::vector<std::string>       &colour,
                                           const std::vector<std::string> &dest)
{
    if (dest.empty())
        return false;
    colour.resize(4);
    if (dest.size() >= 4) {
        for (size_t i = 0; i < 4; ++i)
            colour[i] = dest[i];
    } else {
        for (size_t i = 0; i < 4; ++i)
            colour[i] = dest[std::min(i, dest.size() - 1)];
    }
    return true;
}

std::string spectrum_filament_type_at(const std::vector<std::string> &types, size_t i)
{
    if (types.empty())
        return {};
    return types[std::min(i, types.size() - 1)];
}

bool spectrum_keep_imported_filament_colours(const std::string &mixed_filament_definitions)
{
    return spectrum_mix_looks_like_snapmaker_custom_entries(mixed_filament_definitions);
}

std::string spectrum_pick_filament_name_for_type(
    const std::string                                       &wanted,
    const std::string                                       &session_name,
    const std::vector<std::pair<std::string, std::string>> &name_and_type)
{
    if (wanted.empty())
        return session_name;
    for (const auto &pair : name_and_type) {
        if (pair.first == session_name && pair.second == wanted)
            return session_name;
    }
    for (const auto &pair : name_and_type) {
        if (pair.second == wanted)
            return pair.first;
    }
    return {};
}

} // namespace Slic3r
