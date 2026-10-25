#pragma once

#include "Config.hpp"
#include "PrintConfig.hpp"
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace Slic3r {

class Model;

struct ConditionalSettingCondition {
    std::string setting;
    std::string comparison = "eq";
    std::string value = "1";
};

struct ConditionalSettingEffect {
    std::string target;
    std::string value;
};

struct ConditionalSettingRule {
    std::string condition;
    std::string target;
    std::string value;

    bool enabled = true;
    bool match_all = true;
    std::vector<ConditionalSettingCondition> conditions;
    std::vector<ConditionalSettingEffect> effects;
    std::string name;

    std::vector<ConditionalSettingCondition> predicates() const;
    std::vector<ConditionalSettingEffect> actions() const;
    std::string serialize() const;
    static ConditionalSettingRule deserialize(const std::string &row);
};

struct ConditionalSettingsContext {
    std::vector<unsigned int> used_filaments;
    size_t object_count = 0;
    double model_height = 0.;
    bool has_support = false;
};

enum class ConditionalSettingsStage { All, Process, Printer };

std::vector<std::string> conditional_setting_conditions();
std::vector<std::string> conditional_setting_targets();
ConfigOptionType conditional_setting_type(const std::string &key);
std::vector<std::string> conditional_setting_comparisons(const std::string &key);
std::string conditional_setting_default(const std::string &key);
std::string conditional_setting_value(const DynamicPrintConfig &config, const ConditionalSettingsContext &context,
    const std::string &key, unsigned int filament);
ConditionalSettingsContext conditional_settings_context(const Model &model, const DynamicPrintConfig &config,
    const std::function<bool(size_t, size_t)> &include_instance = {});
std::string validate_conditional_setting(const ConditionalSettingRule &rule);
std::string validate_conditional_settings(const ConfigBase &config);
void apply_conditional_settings(DynamicPrintConfig &config, size_t used_filaments);
std::vector<std::string> apply_conditional_settings(DynamicPrintConfig &config, const DynamicPrintConfig &original,
    const ConditionalSettingsContext &context, ConditionalSettingsStage stage);

} // namespace Slic3r
