#ifndef slic3r_ProcessSettingsMerger_hpp_
#define slic3r_ProcessSettingsMerger_hpp_

#include <string>
#include <vector>

#include "PrintConfig.hpp"

namespace Slic3r {

class ProcessSettingsMerger
{
public:
    using OptionKeys = std::vector<std::string>;

    struct TransferableSettings
    {
        OptionKeys unsaved_options;
        OptionKeys system_default_options;

        bool empty() const { return unsaved_options.empty() && system_default_options.empty(); }
    };

    struct TransferSelection
    {
        OptionKeys unsaved_options;
        OptionKeys system_default_options;

        bool empty() const { return unsaved_options.empty() && system_default_options.empty(); }
        OptionKeys all_options() const;
    };

    struct TransferSourceSettings
    {
        DynamicPrintConfig saved_settings;
        DynamicPrintConfig edited_settings;
        DynamicPrintConfig parent_config;
        bool has_parent_config { false };

        const DynamicPrintConfig* parent_settings() const { return has_parent_config ? &parent_config : nullptr; }
    };

    static OptionKeys normalize_keys(OptionKeys keys);
    static OptionKeys diff_keys(const DynamicPrintConfig &source_settings, const DynamicPrintConfig &target_settings);
    static OptionKeys diff_keys(const DynamicPrintConfig &source_settings, const DynamicPrintConfig &target_settings, const OptionKeys &option_keys);
    static DynamicPrintConfig merge_settings(const DynamicPrintConfig &source_settings, const DynamicPrintConfig &target_settings, const OptionKeys &option_keys);
    static DynamicPrintConfig merge_settings(const DynamicPrintConfig &saved_settings, const DynamicPrintConfig &edited_settings, const DynamicPrintConfig &target_settings, const TransferSelection &selection);
    static TransferableSettings transferable_settings(const DynamicPrintConfig &saved_settings, const DynamicPrintConfig &edited_settings, const DynamicPrintConfig *parent_settings, const DynamicPrintConfig *target_settings = nullptr);
    static TransferSourceSettings transfer_source_settings(const DynamicPrintConfig &saved_settings, const DynamicPrintConfig &edited_settings, const DynamicPrintConfig *parent_settings);
    static TransferSelection default_selection(const TransferableSettings &settings);
};

} // namespace Slic3r

#endif
