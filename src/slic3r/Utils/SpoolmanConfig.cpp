#include "SpoolmanConfig.hpp"

#include "Spoolman.hpp"
#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/I18N.hpp"
#include <regex>

namespace Slic3r {
const static std::regex spoolman_regex("^spoolman_");

static const t_config_enum_values s_keys_map_ConsumptionType = {
    {"weight", ctWEIGHT},
    {"length", ctLENGTH}
};

SpoolmanConfigDef::SpoolmanConfigDef()
{
    ConfigOptionDef* def;

    def          = this->add("spoolman_enabled", coBool);
    def->label   = _u8L("Spoolman Enabled");
    def->tooltip = _u8L("Enables spool management features powered by a Spoolman server instance");
    def->set_default_value(new ConfigOptionBool());

    def          = this->add("spoolman_host", coString);
    def->label   = _u8L("Spoolman Host");
    def->tooltip = _u8L("Points to where you Spoolman instance is hosted. Use the format of <host>:<port>. You may also just specify the "
                        "host and it will use the default Spoolman port of ") +
                   Spoolman::DEFAULT_PORT;
    def->set_default_value(new ConfigOptionString());

    def = this->add("spoolman_consumption_type", coEnum);
    def->label = _u8L("Consumption Type");
    def->tooltip = _u8L("The unit of measurement that sent to Spoolman for consumption");
    def->set_default_value(new ConfigOptionEnumGeneric(&s_keys_map_ConsumptionType, ctWEIGHT));
    def->enum_keys_map = &s_keys_map_ConsumptionType;
    def->enum_labels = {
        _u8L("Weight"),
        _u8L("Length")
    };
    def->enum_values = {
        "weight",
        "length"
    };
}

const SpoolmanConfigDef spoolman_config_def;

void SpoolmanDynamicConfig::load_defaults()
{
    this->clear();
    this->apply(default_spoolman_config);
}

void SpoolmanDynamicConfig::load_from_appconfig(const AppConfig* app_config)
{
    this->load_defaults();
    for (const auto& [opt_key, opt_def] : this->def()->options) {
        auto app_config_key = regex_replace(opt_key, spoolman_regex, "");
        auto val = app_config->get("spoolman", app_config_key);
        if (val.empty())
            continue;
        auto opt = this->option(opt_key, true);
        opt->deserialize(val);
    }
}

void SpoolmanDynamicConfig::save_to_appconfig(AppConfig* app_config) const
{
    for (const auto& key : this->keys()) {
        const auto opt = this->option(key);
        if (opt == this->def()->get(key)->default_value.get())
            continue;
        auto val = opt->serialize();
        auto app_config_key = regex_replace(key, spoolman_regex, "");
        app_config->set("spoolman", app_config_key, val);
    }
}

SpoolmanDynamicConfig create_spoolman_default_config()
{
    SpoolmanDynamicConfig config;
    for (const auto& key : config.def()->keys())
        config.option(key, true);
    return config;
}

const SpoolmanDynamicConfig default_spoolman_config = create_spoolman_default_config();
} // namespace Slic3r