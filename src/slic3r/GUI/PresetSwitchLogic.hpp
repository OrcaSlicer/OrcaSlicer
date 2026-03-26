#ifndef slic3r_GUI_PresetSwitchLogic_hpp_
#define slic3r_GUI_PresetSwitchLogic_hpp_

#include "libslic3r/Preset.hpp"

namespace Slic3r {
namespace GUI {

enum class PresetSelectionFollowup {
    None,
    UpdateComboOnly,
    RunGenericTabSelection,
    RunPrinterSelectionFlow
};

inline PresetSelectionFollowup preset_selection_followup(Preset::Type preset_type, bool select_preset, bool sidebar_is_multifilament)
{
    if (preset_type == Preset::TYPE_FILAMENT && sidebar_is_multifilament)
        return PresetSelectionFollowup::UpdateComboOnly;

    if (!select_preset)
        return PresetSelectionFollowup::None;

    return preset_type == Preset::TYPE_PRINTER ?
        PresetSelectionFollowup::RunPrinterSelectionFlow :
        PresetSelectionFollowup::RunGenericTabSelection;
}

} // namespace GUI
} // namespace Slic3r

#endif