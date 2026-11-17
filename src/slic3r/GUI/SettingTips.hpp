#pragma once

#include <map>
#include <string>
#include <vector>

#include <wx/string.h>

#include "slic3r/GUI/IllustratedTipPopup.hpp"

namespace Slic3r::GUI {

// The pictures in the tooltip of a print, filament or printer setting. Its text stays where it always
// was, in the option's definition in PrintConfig.cpp; only the pictures are kept here.
struct SettingTip
{
    // Shown under the text, whatever the setting's value.
    std::vector<TipMedia> media;
    // For a setting with a list of values (an enum): one picture per value, keyed by the value's name as
    // it is stored in presets and projects (the option's `enum_values`). The setting's tooltip shows the
    // picture of the value chosen now, after `media`, and the open drop-down list shows each value's own
    // picture while the pointer rests on it. A caption left empty is filled with the value's label.
    std::map<std::string, TipMedia> values;
};

using SettingTips = std::map<std::string, SettingTip>;

// The pictures of the setting `opt_key` (without a "#n" index); empty when it has none.
//
// The table is rebuilt on each call, since the captions are translated and the language can change while
// the app runs. It is called when a tooltip opens, not on every frame.
SettingTip setting_tip(const std::string &opt_key);

// The pictures of the tooltip of the setting `opt_key` (a "#n" index is ignored): its `media`, then the
// picture of `value` when that value has one, captioned `value_label` unless it has a caption of its own.
// `value` empty: the setting's `media` alone.
std::vector<TipMedia> setting_tip_media(const std::string &opt_key, const std::string &value, const wxString &value_label);

// The picture of one value of the setting alone, as above, for the item of an open drop-down list.
std::vector<TipMedia> setting_value_media(const std::string &opt_key, const std::string &value, const wxString &value_label);

// Each group of settings keeps its pictures in a file of its own, so the three are edited independently:
// SettingTipsProcess.cpp, SettingTipsFilament.cpp and SettingTipsPrinter.cpp. Pictures are named relative
// to resources/images, under settings/process, settings/filament and settings/printer.
void add_process_setting_tips(SettingTips &tips);
void add_process_quality_setting_tips(SettingTips &tips); // SettingTipsProcessQuality.cpp
void add_process_strength_setting_tips(SettingTips &tips); // SettingTipsProcessStrength.cpp
void add_process_speed_setting_tips(SettingTips &tips); // SettingTipsProcessSpeed.cpp
void add_process_support_setting_tips(SettingTips &tips); // SettingTipsProcessSupport.cpp
void add_process_others_setting_tips(SettingTips &tips); // SettingTipsProcessOthers.cpp
void add_filament_setting_tips(SettingTips &tips);
void add_printer_setting_tips(SettingTips &tips);

} // namespace Slic3r::GUI
