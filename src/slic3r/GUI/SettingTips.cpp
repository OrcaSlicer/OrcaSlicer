#include "SettingTips.hpp"

#include <string>
#include <utility>
#include <vector>

#include <wx/string.h>

#include "libslic3r/PrintConfig.hpp"

namespace Slic3r::GUI {

SettingTip setting_tip(const std::string &opt_key)
{
    SettingTips tips;
    add_process_setting_tips(tips);
    add_filament_setting_tips(tips);
    add_printer_setting_tips(tips);
    const std::string key = opt_key.substr(0, opt_key.find('#'));
    auto              it  = tips.find(key);
    // A filament's override of a printer setting (filament_z_hop_types overrides z_hop_types) shows the
    // pictures of the setting it overrides, unless it has its own.
    static const std::string prefix = "filament_";
    if (it == tips.end() && key.compare(0, prefix.size(), prefix) == 0 && is_filament_extruder_override_key(key))
        it = tips.find(key.substr(prefix.size()));
    return it == tips.end() ? SettingTip{} : it->second;
}

static void append_value_media(const SettingTip &tip, const std::string &value, const wxString &value_label,
                               std::vector<TipMedia> &out)
{
    if (value.empty())
        return;
    const auto it = tip.values.find(value);
    if (it == tip.values.end())
        return;
    TipMedia media = it->second;
    if (media.caption.empty())
        media.caption = value_label;
    out.push_back(std::move(media));
}

std::vector<TipMedia> setting_tip_media(const std::string &opt_key, const std::string &value, const wxString &value_label)
{
    const SettingTip      tip = setting_tip(opt_key);
    std::vector<TipMedia> out = tip.media;
    append_value_media(tip, value, value_label, out);
    return out;
}

std::vector<TipMedia> setting_value_media(const std::string &opt_key, const std::string &value, const wxString &value_label)
{
    std::vector<TipMedia> out;
    append_value_media(setting_tip(opt_key), value, value_label, out);
    return out;
}

} // namespace Slic3r::GUI
