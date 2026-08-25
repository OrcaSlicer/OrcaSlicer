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

enum class TransferPresetOrigin {
    Unknown,
    Default,
    System,
    User,
    Bundle,
    ProjectFile
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

inline TransferPresetOrigin transfer_preset_origin(const Preset *preset, bool project_snapshot, bool snapshot_differs_from_preset)
{
    if (project_snapshot && (preset == nullptr || preset->is_project_embedded || snapshot_differs_from_preset))
        return TransferPresetOrigin::ProjectFile;

    if (preset == nullptr)
        return TransferPresetOrigin::Unknown;
    if (preset->is_project_embedded)
        return TransferPresetOrigin::ProjectFile;
    if (preset->is_default)
        return TransferPresetOrigin::Default;
    if (preset->is_system)
        return TransferPresetOrigin::System;
    if (preset->is_from_bundle())
        return TransferPresetOrigin::Bundle;
    if (preset->is_user())
        return TransferPresetOrigin::User;

    return TransferPresetOrigin::Unknown;
}

inline bool transfer_preset_identities_equal(const std::string &left, const std::string &right)
{
    return !left.empty() && Preset::remove_suffix_modified(left) == Preset::remove_suffix_modified(right);
}

} // namespace GUI
} // namespace Slic3r

#endif
