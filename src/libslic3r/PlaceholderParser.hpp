#ifndef slic3r_PlaceholderParser_hpp_
#define slic3r_PlaceholderParser_hpp_

#include "libslic3r.h"
#include <map>
#include <random>
#include <string>
#include <string_view>
#include <vector>
#include "PrintConfig.hpp"

namespace Slic3r {

class PlaceholderParser
{
public:
    // Context to be shared during multiple executions of the PlaceholderParser.
    // The context is kept external to the PlaceholderParser, so that the same PlaceholderParser
    // may be called safely from multiple threads.
    // In the future, the context may hold variables created and modified by the PlaceholderParser
    // and shared between the PlaceholderParser::process() invocations.
    struct ContextData {
        std::mt19937                    rng;
        // If defined, then this dictionary is used by the scripts to define user variables and persist them
        // between PlaceholderParser evaluations.
        std::unique_ptr<DynamicConfig>  global_config;
    };

    PlaceholderParser(const DynamicConfig *external_config = nullptr);
    
    void clear_config() { m_config.clear(); }
    // Return a list of keys, which should be changed in m_config from rhs.
    // This contains keys, which are found in rhs, but not in m_config.
    std::vector<std::string> config_diff(const DynamicPrintConfig &rhs);
    // Return true if modified.
    bool apply_config(const DynamicPrintConfig &config);
    void apply_config(DynamicPrintConfig &&config);
    // To be called on the values returned by PlaceholderParser::config_diff().
    // The keys should already be valid.
    void apply_only(const DynamicPrintConfig &config, const std::vector<std::string> &keys);
    void apply_env_variables();

    // Add new ConfigOption values to m_config.
    void set(const std::string &key, const std::string &value)  { this->set(key, new ConfigOptionString(value)); }
    void set(const std::string &key, std::string_view value)    { this->set(key, new ConfigOptionString(std::string(value))); }
    void set(const std::string &key, const char *value)         { this->set(key, new ConfigOptionString(value)); }
    void set(const std::string &key, int value)                 { this->set(key, new ConfigOptionInt(value)); }
    void set(const std::string &key, unsigned int value)        { this->set(key, int(value)); }
    void set(const std::string &key, bool value)                { this->set(key, new ConfigOptionBool(value)); }
    void set(const std::string &key, double value)              { this->set(key, new ConfigOptionFloat(value)); }
    void set(const std::string &key, const std::vector<std::string> &values) { this->set(key, new ConfigOptionStrings(values)); }
    void set(const std::string &key, ConfigOption *opt)         { m_config.set_key_value(key, opt); }
	DynamicConfig&			config_writable()					{ return m_config; }
	const DynamicConfig&    config() const                      { return m_config; }
    const ConfigOption*     option(const std::string &key) const { return m_config.option(key); }
    // External config is not owned by PlaceholderParser. It has a lowest priority when looking up an option.
	const DynamicConfig*	external_config() const  			{ return m_external_config; }

    // Fill in the template using a macro processing language.
    // Throws Slic3r::PlaceholderParserError on syntax or runtime error.
    std::string process(const std::string &templ, unsigned int current_extruder_id, const DynamicConfig *config_override, DynamicConfig *config_outputs, ContextData *context) const;
    std::string process(const std::string &templ, unsigned int current_extruder_id = 0, const DynamicConfig *config_override = nullptr, ContextData *context = nullptr) const
        { return this->process(templ, current_extruder_id, config_override, nullptr /* config_outputs */, context); }

    // Evaluate a boolean expression using the full expressive power of the PlaceholderParser boolean expression syntax.
    // Throws Slic3r::PlaceholderParserError on syntax or runtime error.
    static bool evaluate_boolean_expression(const std::string &templ, const DynamicConfig &config, const DynamicConfig *config_override = nullptr);

    // Update timestamp, year, month, day, hour, minute, second variables at the provided config.
    static void update_timestamp(DynamicConfig &config);
    // Update timestamp, year, month, day, hour, minute, second variables at m_config.
    void update_timestamp() { update_timestamp(m_config); }

    static void update_user_name(DynamicConfig &config);
    void update_user_name() { update_user_name(m_config); }

private:
	// config has a higher priority than external_config when looking up a symbol.
    DynamicConfig 			 m_config;
    const DynamicConfig 	*m_external_config;
};

}

#endif
