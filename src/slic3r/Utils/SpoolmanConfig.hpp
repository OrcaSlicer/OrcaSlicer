#ifndef ORCASLICER_SPOOLMANCONFIG_HPP
#define ORCASLICER_SPOOLMANCONFIG_HPP

#include "libslic3r/Config.hpp"

namespace Slic3r {
class AppConfig;

enum ConsumptionType
{
    ctWEIGHT,
    ctLENGTH
};

class SpoolmanConfigDef : public ConfigDef
{
public:
    SpoolmanConfigDef();
};

extern const SpoolmanConfigDef spoolman_config_def;

class SpoolmanDynamicConfig : public DynamicConfigWithDef
{
public:
    SpoolmanDynamicConfig() = default;
    explicit SpoolmanDynamicConfig(const AppConfig* app_config)
    {
        load_from_appconfig(app_config);
    }
    const ConfigDef* def() const override { return &spoolman_config_def; }
    // Clear config and load defaults
    void load_defaults();
    // Load config from appconfig on top of defaults
    void load_from_appconfig(const AppConfig* app_config);
    // Save config to appconfig with default values stripped
    void save_to_appconfig(AppConfig* app_config) const;
};

extern const SpoolmanDynamicConfig default_spoolman_config;
} // namespace Slic3r

#endif // ORCASLICER_SPOOLMANCONFIG_HPP
