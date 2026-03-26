#ifndef slic3r_ProcessSettingsMerger_hpp_
#define slic3r_ProcessSettingsMerger_hpp_

#include <vector>

#include "PrintConfig.hpp"

namespace Slic3r {

enum class ProcessSettingsCategory {
    Safe,
    Conditional,
    HardwareSpecific,
};

class ProcessSettingsMerger
{
public:
    using Categories = std::vector<ProcessSettingsCategory>;
    using OptionKeys = std::vector<std::string>;

    static Categories all_categories();
    static OptionKeys option_keys_for_categories(const Categories &categories);
    static DynamicPrintConfig merge_settings(const DynamicPrintConfig &old_settings, const DynamicPrintConfig &new_defaults, const Categories &categories);
    static DynamicPrintConfig merge_settings(const DynamicPrintConfig &old_settings, const DynamicPrintConfig &new_defaults, const OptionKeys &option_keys);
    static OptionKeys diff_keys(const DynamicPrintConfig &old_settings, const DynamicPrintConfig &new_defaults, const Categories &categories);
    static OptionKeys diff_keys(const DynamicPrintConfig &old_settings, const DynamicPrintConfig &new_defaults, const OptionKeys &option_keys);
    static bool has_transferable_settings(const DynamicPrintConfig &old_settings, const DynamicPrintConfig &new_defaults, const Categories &categories = all_categories());
};

} // namespace Slic3r

#endif