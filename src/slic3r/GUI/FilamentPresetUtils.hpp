#ifndef slic3r_GUI_FilamentPresetUtils_hpp_
#define slic3r_GUI_FilamentPresetUtils_hpp_

#include <string>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"

namespace Slic3r { namespace GUI {

inline std::string filament_default_colour(const Preset* preset)
{
    return preset ? preset->config.opt_string("default_filament_colour", 0u) : std::string();
}

inline const Preset* find_preset_by_name_or_alias(
    const PresetBundle* preset_bundle,
    Preset::Type preset_type,
    const PresetCollection* collection,
    const std::string& name_or_alias)
{
    if (!collection || name_or_alias.empty())
        return nullptr;

    std::string preset_name = Preset::remove_suffix_modified(name_or_alias);
    if (preset_bundle)
        preset_name = preset_bundle->get_preset_name_by_alias(preset_type, preset_name);

    return collection->find_preset(preset_name);
}

inline const Preset* find_filament_preset_by_id(const PresetBundle* preset_bundle, const std::string& filament_id)
{
    if (!preset_bundle || filament_id.empty())
        return nullptr;

    for (auto it = preset_bundle->filaments.begin(); it != preset_bundle->filaments.end(); ++it)
        if (it->filament_id == filament_id)
            return &(*it);

    return nullptr;
}

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_FilamentPresetUtils_hpp_
