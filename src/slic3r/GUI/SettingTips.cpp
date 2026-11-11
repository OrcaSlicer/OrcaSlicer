#include "SettingTips.hpp"

namespace Slic3r::GUI {

SettingTip setting_tip(const std::string &opt_key)
{
    SettingTips tips;
    add_process_setting_tips(tips);
    add_filament_setting_tips(tips);
    add_printer_setting_tips(tips);
    const auto it = tips.find(opt_key);
    return it == tips.end() ? SettingTip{} : it->second;
}

} // namespace Slic3r::GUI
