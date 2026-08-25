#include "ProcessSettingsMerger.hpp"

#include <algorithm>
#include <set>

namespace Slic3r {

namespace {

ProcessSettingsMerger::OptionKeys filter_keys_different_from_target(
    ProcessSettingsMerger::OptionKeys keys,
    const DynamicPrintConfig &source_settings,
    const DynamicPrintConfig &target_settings)
{
    ProcessSettingsMerger::OptionKeys filtered;
    for (const std::string &key : ProcessSettingsMerger::normalize_keys(std::move(keys))) {
        if (!ProcessSettingsMerger::diff_keys(source_settings, target_settings, { key }).empty())
            filtered.emplace_back(key);
    }
    return filtered;
}

void remove_keys(ProcessSettingsMerger::OptionKeys &keys, const ProcessSettingsMerger::OptionKeys &keys_to_remove)
{
    const std::set<std::string> remove_set(keys_to_remove.begin(), keys_to_remove.end());
    keys.erase(std::remove_if(keys.begin(), keys.end(), [&remove_set](const std::string &key) {
        return remove_set.find(key) != remove_set.end();
    }), keys.end());
}

} // namespace

ProcessSettingsMerger::OptionKeys ProcessSettingsMerger::TransferSelection::all_options() const
{
    OptionKeys keys = system_default_options;
    keys.insert(keys.end(), unsaved_options.begin(), unsaved_options.end());
    return normalize_keys(std::move(keys));
}

ProcessSettingsMerger::OptionKeys ProcessSettingsMerger::normalize_keys(OptionKeys keys)
{
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
}

ProcessSettingsMerger::OptionKeys ProcessSettingsMerger::diff_keys(const DynamicPrintConfig &source_settings, const DynamicPrintConfig &target_settings)
{
    return normalize_keys(target_settings.diff(source_settings));
}

ProcessSettingsMerger::OptionKeys ProcessSettingsMerger::diff_keys(
    const DynamicPrintConfig &source_settings,
    const DynamicPrintConfig &target_settings,
    const OptionKeys &option_keys)
{
    DynamicPrintConfig merged = merge_settings(source_settings, target_settings, option_keys);
    return normalize_keys(merged.diff(target_settings));
}

DynamicPrintConfig ProcessSettingsMerger::merge_settings(
    const DynamicPrintConfig &source_settings,
    const DynamicPrintConfig &target_settings,
    const OptionKeys &option_keys)
{
    DynamicPrintConfig merged = target_settings;
    const OptionKeys keys = normalize_keys(option_keys);
    if (!keys.empty())
        merged.apply_only(source_settings, keys, true);
    return merged;
}

DynamicPrintConfig ProcessSettingsMerger::merge_settings(
    const DynamicPrintConfig &saved_settings,
    const DynamicPrintConfig &edited_settings,
    const DynamicPrintConfig &target_settings,
    const TransferSelection &selection)
{
    DynamicPrintConfig merged = target_settings;

    const OptionKeys system_default_keys = normalize_keys(selection.system_default_options);
    if (!system_default_keys.empty())
        merged.apply_only(saved_settings, system_default_keys, true);

    const OptionKeys unsaved_keys = normalize_keys(selection.unsaved_options);
    if (!unsaved_keys.empty())
        merged.apply_only(edited_settings, unsaved_keys, true);

    return merged;
}

ProcessSettingsMerger::TransferableSettings ProcessSettingsMerger::transferable_settings(
    const DynamicPrintConfig &saved_settings,
    const DynamicPrintConfig &edited_settings,
    const DynamicPrintConfig *parent_settings,
    const DynamicPrintConfig *target_settings)
{
    TransferableSettings settings;
    settings.unsaved_options = diff_keys(edited_settings, saved_settings);

    if (target_settings != nullptr)
        settings.system_default_options = diff_keys(saved_settings, *target_settings);
    else if (parent_settings != nullptr)
        settings.system_default_options = diff_keys(saved_settings, *parent_settings);

    remove_keys(settings.system_default_options, settings.unsaved_options);

    if (target_settings != nullptr) {
        settings.unsaved_options = filter_keys_different_from_target(
            std::move(settings.unsaved_options),
            edited_settings,
            *target_settings);
        settings.system_default_options = filter_keys_different_from_target(
            std::move(settings.system_default_options),
            saved_settings,
            *target_settings);
    }

    settings.unsaved_options = normalize_keys(std::move(settings.unsaved_options));
    settings.system_default_options = normalize_keys(std::move(settings.system_default_options));
    return settings;
}

ProcessSettingsMerger::TransferSourceSettings ProcessSettingsMerger::transfer_source_settings(
    const DynamicPrintConfig &saved_settings,
    const DynamicPrintConfig &edited_settings,
    const DynamicPrintConfig *parent_settings)
{
    TransferSourceSettings source;
    source.saved_settings = saved_settings;
    source.edited_settings = edited_settings;
    source.has_parent_config = parent_settings != nullptr;
    if (parent_settings != nullptr)
        source.parent_config = *parent_settings;
    return source;
}

ProcessSettingsMerger::TransferSelection ProcessSettingsMerger::default_selection(const TransferableSettings &settings)
{
    TransferSelection selection;
    selection.unsaved_options = settings.unsaved_options;
    return selection;
}

} // namespace Slic3r
