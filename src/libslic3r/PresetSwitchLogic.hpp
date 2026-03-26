#ifndef slic3r_PresetSwitchLogic_hpp_
#define slic3r_PresetSwitchLogic_hpp_

#include "Preset.hpp"

namespace Slic3r {

inline bool should_run_generic_preset_selection(Preset::Type preset_type, bool select_preset, bool sidebar_is_multifilament)
{
    if (!select_preset)
        return false;

    if (preset_type == Preset::TYPE_FILAMENT && sidebar_is_multifilament)
        return false;

    return preset_type != Preset::TYPE_PRINTER;
}

} // namespace Slic3r

#endif