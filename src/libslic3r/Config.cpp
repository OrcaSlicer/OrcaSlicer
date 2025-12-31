#include "Config.hpp"
#include "format.hpp"
#include "Utils.hpp"
#include "LocalesUtils.hpp"
#include "Preset.hpp"

#include <assert.h>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <regex>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/erase.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/config.hpp>
#include <boost/foreach.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/cstdlib.hpp>
#include <boost/nowide/iostream.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/format.hpp>
#include <string.h>
//BBS: add json support
#include "nlohmann/json.hpp"

using namespace nlohmann;

//FIXME for GCodeFlavor and gcfMarlin (for forward-compatibility conversion)
// This is not nice, likely it would be better to pass the ConfigSubstitutionContext to handle_legacy().
#include "PrintConfig.hpp"

namespace Slic3r {

//BBS: add json support
//static const std::string CONFIG_VERSION_KEY = "version";
//static const std::string CONFIG_NAME_KEY = "name";
//static const std::string CONFIG_URL_KEY = "url";
//static const std::string CONFIG_TYPE_KEY = "type";
//static const std::string CONFIG_FROM_KEY = "from";
//static const std::string CONFIG_INHERITS_KEY = "inherits";
//static const std::string CONFIG_INSTANT_KEY = "instantiation";

// Escape double quotes, \n, \r and backslash
std::string escape_string_cstyle(const std::string &str)
{
    // Allocate a buffer twice the input string length,
    // so the output will fit even if all input characters get escaped.
    std::vector<char> out(str.size() * 2, 0);
    char *outptr = out.data();
    for (size_t i = 0; i < str.size(); ++ i) {
        char c = str[i];
        if (c == '\r') {
            (*outptr ++) = '\\';
            (*outptr ++) = 'r';
        } else if (c == '\n') {
            (*outptr ++) = '\\';
            (*outptr ++) = 'n';
        } else if (c == '\\' || c == '"') {
            (*outptr++) = '\\';
            (*outptr++) = c;
        } else
            (*outptr ++) = c;
    }
    return std::string(out.data(), outptr - out.data());
}

std::string escape_strings_cstyle(const std::vector<std::string> &strs)
{
    // 1) Estimate the output buffer size to avoid buffer reallocation.
    size_t outbuflen = 0;
    for (size_t i = 0; i < strs.size(); ++ i)
        // Reserve space for every character escaped + quotes + semicolon.
        outbuflen += strs[i].size() * 2 + 3;
    // 2) Fill in the buffer.
    std::vector<char> out(outbuflen, 0);
    char *outptr = out.data();
    for (size_t j = 0; j < strs.size(); ++ j) {
        if (j > 0)
            // Separate the strings.
            (*outptr ++) = ';';
        const std::string &str = strs[j];
        // Is the string simple or complex? Complex string contains spaces, tabs, new lines and other
        // escapable characters. Empty string shall be quoted as well, if it is the only string in strs.
        bool should_quote = strs.size() == 1 && str.empty();
        for (size_t i = 0; i < str.size(); ++ i) {
            char c = str[i];
            if (c == ' ' || c == '\t' || c == '\\' || c == '"' || c == '\r' || c == '\n') {
                should_quote = true;
                break;
            }
        }
        if (should_quote) {
            (*outptr ++) = '"';
            for (size_t i = 0; i < str.size(); ++ i) {
                char c = str[i];
                if (c == '\\' || c == '"') {
                    (*outptr ++) = '\\';
                    (*outptr ++) = c;
                } else if (c == '\r') {
                    (*outptr ++) = '\\';
                    (*outptr ++) = 'r';
                } else if (c == '\n') {
                    (*outptr ++) = '\\';
                    (*outptr ++) = 'n';
                } else
                    (*outptr ++) = c;
            }
            (*outptr ++) = '"';
        } else {
            memcpy(outptr, str.data(), str.size());
            outptr += str.size();
        }
    }
    return std::string(out.data(), outptr - out.data());
}

// Unescape double quotes, \n, \r and backslash
bool unescape_string_cstyle(const std::string &str, std::string &str_out)
{
    std::vector<char> out(str.size(), 0);
    char *outptr = out.data();
    for (size_t i = 0; i < str.size(); ++ i) {
        char c = str[i];
        if (c == '\\') {
            if (++ i == str.size())
                return false;
            c = str[i];
            if (c == 'r')
                (*outptr ++) = '\r';
            else if (c == 'n')
                (*outptr ++) = '\n';
            else
                (*outptr ++) = c;
        } else
            (*outptr ++) = c;
    }
    str_out.assign(out.data(), outptr - out.data());
    return true;
}

bool unescape_strings_cstyle(const std::string &str, std::vector<std::string> &out)
{
    if (str.empty())
        return true;

    size_t i = 0;
    for (;;) {
        // Skip white spaces.
        char c = str[i];
        while (c == ' ' || c == '\t') {
            if (++ i == str.size())
                return true;
            c = str[i];
        }
        // Start of a word.
        std::vector<char> buf;
        buf.reserve(16);
        // Is it enclosed in quotes?
        c = str[i];
        if (c == '"') {
            // Complex case, string is enclosed in quotes.
            for (++ i; i < str.size(); ++ i) {
                c = str[i];
                if (c == '"') {
                    // End of string.
                    break;
                }
                if (c == '\\') {
                    if (++ i == str.size())
                        return false;
                    c = str[i];
                    if (c == 'r')
                        c = '\r';
                    else if (c == 'n')
                        c = '\n';
                }
                buf.push_back(c);
            }
            if (i == str.size())
                return false;
            ++ i;
        } else {
            for (; i < str.size(); ++ i) {
                c = str[i];
                if (c == ';')
                    break;
                buf.push_back(c);
            }
        }
        // Store the string into the output vector.
        out.push_back(std::string(buf.data(), buf.size()));
        if (i == str.size())
            return true;
        // Skip white spaces.
        c = str[i];
        while (c == ' ' || c == '\t') {
            if (++ i == str.size())
                // End of string. This is correct.
                return true;
            c = str[i];
        }
        if (c != ';')
            return false;
        if (++ i == str.size()) {
            // Emit one additional empty string.
            out.push_back(std::string());
            return true;
        }
    }
}

std::string escape_ampersand(const std::string& str)
{
    // Allocate a buffer 2 times the input string length,
    // so the output will fit even if all input characters get escaped.
    std::vector<char> out(str.size() * 6, 0);
    char* outptr = out.data();
    for (size_t i = 0; i < str.size(); ++i) {
        char c = str[i];
        if (c == '&') {
            (*outptr++) = '&';
            (*outptr++) = '&';
        } else
            (*outptr++) = c;
    }
    return std::string(out.data(), outptr - out.data());
}

void ConfigOptionDeleter::operator()(ConfigOption* p) {
    delete p;
}

std::vector<std::string> ConfigOptionDef::cli_args(const std::string &key) const
{
	std::vector<std::string> args;
	if (this->cli != ConfigOptionDef::nocli) {
        const std::string &cli = this->cli;
        //FIXME What was that for? Check the "readline" documentation.
        // Neither '=' nor '!' is used in any of the cli parameters currently defined by PrusaSlicer.
//        std::string cli = this->cli.substr(0, this->cli.find("="));
//        boost::trim_right_if(cli, boost::is_any_of("!"));
		if (cli.empty()) {
            // Convert an option key to CLI argument by replacing underscores with dashes.
            std::string opt = key;
            boost::replace_all(opt, "_", "-");
            args.emplace_back(std::move(opt));
        } else
			boost::split(args, cli, boost::is_any_of("|"));
    }
    return args;
}

ConfigOption* ConfigOptionDef::create_empty_option() const
{
	if (this->nullable) {
	    switch (this->type) {
	    case coFloats:          return new ConfigOptionFloatsNullable();
	    case coInts:            return new ConfigOptionIntsNullable();
	    case coPercents:        return new ConfigOptionPercentsNullable();
        case coFloatsOrPercents: return new ConfigOptionFloatsOrPercentsNullable();
	    case coBools:           return new ConfigOptionBoolsNullable();
	    default:                throw ConfigurationError(std::string("Unknown option type for nullable option ") + this->label);
	    }
	} else {
	    switch (this->type) {
	    case coFloat:           return new ConfigOptionFloat();
	    case coFloats:          return new ConfigOptionFloats();
	    case coInt:             return new ConfigOptionInt();
	    case coInts:            return new ConfigOptionInts();
	    case coString:          return new ConfigOptionString();
	    case coStrings:         return new ConfigOptionStrings();
	    case coPercent:         return new ConfigOptionPercent();
	    case coPercents:        return new ConfigOptionPercents();
	    case coFloatOrPercent:  return new ConfigOptionFloatOrPercent();
        case coFloatsOrPercents: return new ConfigOptionFloatsOrPercents();
	    case coPoint:           return new ConfigOptionPoint();
	    case coPoints:          return new ConfigOptionPoints();
	    case coPoint3:          return new ConfigOptionPoint3();
	//    case coPoint3s:         return new ConfigOptionPoint3s();
	    case coBool:            return new ConfigOptionBool();
	    case coBools:           return new ConfigOptionBools();
	    case coEnum:            return new ConfigOptionEnumGeneric(this->enum_keys_map);
        // BBS
        case coEnums:           return new ConfigOptionEnumsGeneric(this->enum_keys_map);
	    default:                throw ConfigurationError(std::string("Unknown option type for option ") + this->label);
	    }
	}
}

ConfigOption* ConfigOptionDef::create_default_option() const
{
    if (this->default_value) {
        ConfigOptionType type = this->default_value->type();
        if (type == coEnum)
            return new ConfigOptionEnumGeneric(this->enum_keys_map, this->default_value->getInt());

        if (type == coEnums) {
            auto dft = this->default_value->clone();
            if (dft->nullable()) {
                ConfigOptionEnumsGenericNullable *opt = dynamic_cast<ConfigOptionEnumsGenericNullable *>(this->default_value->clone());
                opt->keys_map = this->enum_keys_map;
                return opt;
            } else {
                ConfigOptionEnumsGeneric *opt = dynamic_cast<ConfigOptionEnumsGeneric *>(this->default_value->clone());
                opt->keys_map = this->enum_keys_map;
                return opt;
            }
            delete dft;
        }

        return this->default_value->clone();
    }
    return this->create_empty_option();
}

bool ConfigOptionDef::is_value_valid(const double value, const int max_precision /*= 4*/) const
{
    if (this->min == 0.f && value < 0) { // Special handling of 0
        return false;
    }

    const double ep = std::pow(0.1, max_precision);
    if (is_approx(value, (double) this->min, ep) || is_approx(value, (double) this->max, ep))
        return true;

    return this->min <= value && value <= this->max;
}

// Assignment of the serialization IDs is not thread safe. The Defs shall be initialized from the main thread!
ConfigOptionDef* ConfigDef::add(const t_config_option_key &opt_key, ConfigOptionType type)
{
	static size_t serialization_key_ordinal_last = 0;
    ConfigOptionDef *opt = &this->options[opt_key];
    opt->opt_key = opt_key;
    opt->type = type;
    opt->serialization_key_ordinal = ++ serialization_key_ordinal_last;
    this->by_serialization_key_ordinal[opt->serialization_key_ordinal] = opt;
    return opt;
}

ConfigOptionDef* ConfigDef::add_nullable(const t_config_option_key &opt_key, ConfigOptionType type)
{
	ConfigOptionDef *def = this->add(opt_key, type);
	def->nullable = true;
	return def;
}

std::ostream& ConfigDef::print_cli_help(std::ostream& out, bool show_defaults, std::function<bool(const ConfigOptionDef &)> filter) const
{
    // prepare a function for wrapping text
    auto wrap = [](std::string text, size_t line_length) -> std::string {
        std::istringstream words(text);
        std::ostringstream wrapped;
        std::string word;

        if (words >> word) {
            wrapped << word;
            size_t space_left = line_length - word.length();
            while (words >> word) {
                if (space_left < word.length() + 1) {
                    wrapped << '\n' << word;
                    space_left = line_length - word.length();
                } else {
                    wrapped << ' ' << word;
                    space_left -= word.length() + 1;
                }
            }
        }
        return wrapped.str();
    };

    // get the unique categories
    std::set<std::string> categories;
    for (const auto& opt : this->options) {
        const ConfigOptionDef& def = opt.second;
        if (filter(def))
            categories.insert(def.category);
    }

    for (auto category : categories) {
        if (category != "") {
            out << category << ":" << std::endl;
        } else if (categories.size() > 1) {
            out << "Misc options:" << std::endl;
        }

        for (const auto& opt : this->options) {
            const ConfigOptionDef& def = opt.second;
			if (def.category != category || def.cli == ConfigOptionDef::nocli || !filter(def))
                continue;

            // get all possible variations: --foo, --foobar, -f...
            std::vector<std::string> cli_args = def.cli_args(opt.first);
			if (cli_args.empty())
				continue;

            for (auto& arg : cli_args) {
                arg.insert(0, (arg.size() == 1) ? "-" : "--");
                //BBS: refine the print help format
                if (!def.cli_params.empty())
                    arg += " " + def.cli_params;
                /*if ( def.type == coInt || def.type == coInts) {
                    arg += " int_value";
                } else if (def.type == coFloat ||  def.type == coFloatOrPercent || def.type == coFloats) {
                    arg += " float_value";
                } else if (def.type == coPoint) {
                    arg += " X,Y";
                } else if (def.type == coPoint3) {
                    arg += " X,Y,Z";
                } else if (def.type == coString || def.type == coStrings) {
                    arg += " filename_lists";
                }*/
            }

            // left: command line options
            const std::string cli = boost::algorithm::join(cli_args, ", ");
            out << " " << std::left << std::setw(20) << cli;

            // right: option description
            std::string descr = def.tooltip;
            bool show_defaults_this = show_defaults || def.opt_key == "config_compatibility";
            if (show_defaults_this && def.default_value && def.type != coBool
                && (def.type != coString || !def.default_value->serialize().empty())) {
                descr += " (";
                if (!def.sidetext.empty()) {
                    descr += def.sidetext + ", ";
                } else if (!def.enum_values.empty()) {
                    descr += boost::algorithm::join(def.enum_values, ", ") + "; ";
                }
                descr += "default: " + def.default_value->serialize() + ")";
            }

            // wrap lines of description
            descr = wrap(descr, 80);
            std::vector<std::string> lines;
            boost::split(lines, descr, boost::is_any_of("\n"));

            // if command line options are too long, print description in new line
            for (size_t i = 0; i < lines.size(); ++i) {
                if (i == 0 && cli.size() > 19)
                    out << std::endl;
                if (i > 0 || cli.size() > 19)
                    out << std::string(21, ' ');
                out << lines[i] << std::endl;
            }
        }
    }
    return out;
}

void ConfigBase::apply_only(const ConfigBase &other, const t_config_option_keys &keys, bool ignore_nonexistent)
{
    // loop through options and apply them
    for (const t_config_option_key &opt_key : keys) {
        // Create a new option with default value for the key.
        // If the key is not in the parameter definition, or this ConfigBase is a static type and it does not support the parameter,
        // an exception is thrown if not ignore_nonexistent.
        ConfigOption *my_opt = this->option(opt_key, true);
        if (my_opt == nullptr) {
            // opt_key does not exist in this ConfigBase and it cannot be created, because it is not defined by this->def().
            // This is only possible if other is of DynamicConfig type.
            if (auto n = opt_key.find('#'); n != std::string::npos) {
                auto opt_key2 = opt_key.substr(0, n);
                auto my_opt2 = dynamic_cast<ConfigOptionVectorBase*>(this->option(opt_key2));
                auto other_opt = other.option(opt_key2);
                if (my_opt2 == nullptr && other_opt) {
                    my_opt2 = dynamic_cast<ConfigOptionVectorBase *>(this->option(opt_key2, true));
                    if (my_opt2->empty()) {
                        my_opt2->resize(1, other_opt);
                    }
                }   
                if (my_opt2) {
                    int index = std::atoi(opt_key.c_str() + n + 1);
                    if (other_opt)
                        my_opt2->set_at(other_opt, index, index);
                    continue;
                }
            }
            if (ignore_nonexistent)
                continue;
            throw UnknownOptionException(opt_key);
        }
		const ConfigOption *other_opt = other.option(opt_key);
		if (other_opt == nullptr) {
            // The key was not found in the source config, therefore it will not be initialized!
//			printf("Not found, therefore not initialized: %s\n", opt_key.c_str());
		} else
            my_opt->set(other_opt);
    }
}

// Are the two configs equal? Ignoring options not present in both configs.
//BBS: add skipped keys logic
bool ConfigBase::equals(const ConfigBase &other, const std::set<std::string>* skipped_keys) const
{
    for (const t_config_option_key &opt_key : this->keys()) {
        if (skipped_keys && (skipped_keys->count(opt_key) != 0))
            continue;
        const ConfigOption *this_opt  = this->option(opt_key);
        const ConfigOption *other_opt = other.option(opt_key);
        if (this_opt != nullptr && other_opt != nullptr && *this_opt != *other_opt)
            return false;
    }
    return true;
}

// Returns options differing in the two configs, ignoring options not present in both configs.
t_config_option_keys ConfigBase::diff(const ConfigBase &other) const
{
    t_config_option_keys diff;
    for (const t_config_option_key &opt_key : this->keys()) {
        const ConfigOption *this_opt  = this->option(opt_key);
        const ConfigOption *other_opt = other.option(opt_key);
        if (this_opt != nullptr && other_opt != nullptr && *this_opt != *other_opt)
            diff.emplace_back(opt_key);
    }
    return diff;
}

// Returns options being equal in the two configs, ignoring options not present in both configs.
t_config_option_keys ConfigBase::equal(const ConfigBase &other) const
{
    t_config_option_keys equal;
    for (const t_config_option_key &opt_key : this->keys()) {
        const ConfigOption *this_opt  = this->option(opt_key);
        const ConfigOption *other_opt = other.option(opt_key);
        if (this_opt != nullptr && other_opt != nullptr && *this_opt == *other_opt)
            equal.emplace_back(opt_key);
    }
    return equal;
}

std::string ConfigBase::opt_serialize(const t_config_option_key &opt_key) const
{
    const ConfigOption* opt = this->option(opt_key);
    assert(opt != nullptr);
    return opt->serialize();
}

void ConfigBase::set(const std::string &opt_key, int value, bool create)
{
    ConfigOption *opt = this->option_throw(opt_key, create);
    switch (opt->type()) {
    	case coInt:    static_cast<ConfigOptionInt*>(opt)->value = value; break;
    	case coFloat:  static_cast<ConfigOptionFloat*>(opt)->value = value; break;
		case coFloatOrPercent:  static_cast<ConfigOptionFloatOrPercent*>(opt)->value = value; static_cast<ConfigOptionFloatOrPercent*>(opt)->percent = false; break;
		case coString: static_cast<ConfigOptionString*>(opt)->value = std::to_string(value); break;
    	default: throw BadOptionTypeException("Configbase::set() - conversion from int not possible");
    }
}

void ConfigBase::set(const std::string &opt_key, double value, bool create)
{
    ConfigOption *opt = this->option_throw(opt_key, create);
    switch (opt->type()) {
    	case coFloat:  			static_cast<ConfigOptionFloat*>(opt)->value = value; break;
    	case coFloatOrPercent:  static_cast<ConfigOptionFloatOrPercent*>(opt)->value = value; static_cast<ConfigOptionFloatOrPercent*>(opt)->percent = false; break;
        case coString: 			static_cast<ConfigOptionString*>(opt)->value = float_to_string_decimal_point(value); break;
    	default: throw BadOptionTypeException("Configbase::set() - conversion from float not possible");
    }
}

bool ConfigBase::set_deserialize_nothrow(const t_config_option_key &opt_key_src, const std::string &value_src, ConfigSubstitutionContext& substitutions_ctxt, bool append)
{
    t_config_option_key opt_key = opt_key_src;
    std::string         value   = value_src;
    // Both opt_key and value may be modified by handle_legacy().
    // If the opt_key is no more valid in this version of Slic3r, opt_key is cleared by handle_legacy().
    this->handle_legacy(opt_key, value);
    if (opt_key.empty()) {
        // Ignore the option.
        //BBS: record these options, keep only one repeated opt_key
        auto iter = std::find(substitutions_ctxt.unrecogized_keys.begin(), substitutions_ctxt.unrecogized_keys.end(), opt_key_src);
        if (iter == substitutions_ctxt.unrecogized_keys.end())
            substitutions_ctxt.unrecogized_keys.push_back(opt_key_src);
        return true;
    }
    return this->set_deserialize_raw(opt_key, value, substitutions_ctxt, append);
}

void ConfigBase::set_deserialize(const t_config_option_key &opt_key_src, const std::string &value_src, ConfigSubstitutionContext& substitutions_ctxt, bool append)
{
	if (! this->set_deserialize_nothrow(opt_key_src, value_src, substitutions_ctxt, append))
		throw BadOptionValueException(format("Invalid value provided for parameter %1%: %2%", opt_key_src,  value_src));
}

void ConfigBase::set_deserialize(std::initializer_list<SetDeserializeItem> items, ConfigSubstitutionContext& substitutions_ctxt)
{
	for (const SetDeserializeItem &item : items)
		this->set_deserialize(item.opt_key, item.opt_value, substitutions_ctxt, item.append);
}

bool ConfigBase::set_deserialize_raw(const t_config_option_key &opt_key_src, const std::string &value, ConfigSubstitutionContext& substitutions_ctxt, bool append)
{
    t_config_option_key    opt_key = opt_key_src;
    // Try to deserialize the option by its name.
    const ConfigDef       *def     = this->def();
    if (def == nullptr)
        throw NoDefinitionException(opt_key);
    const ConfigOptionDef *optdef  = def->get(opt_key);
    if (optdef == nullptr) {
        // If we didn't find an option, look for any other option having this as an alias.
        for (const auto &opt : def->options) {
            for (const t_config_option_key &opt_key2 : opt.second.aliases) {
                if (opt_key2 == opt_key) {
                    opt_key = opt.first;
                    optdef = &opt.second;
                    break;
                }
            }
            if (optdef != nullptr)
                break;
        }
        if (optdef == nullptr)
            throw UnknownOptionException(opt_key);
    }

    if (! optdef->shortcut.empty()) {
        // Aliasing for example "solid_layers" to "top_shell_layers" and "bottom_shell_layers".
        for (const t_config_option_key &shortcut : optdef->shortcut)
            // Recursive call.
            if (! this->set_deserialize_raw(shortcut, value, substitutions_ctxt, append))
                return false;
        return true;
    }

    ConfigOption *opt = this->option(opt_key, true);
    assert(opt != nullptr);
    bool success     = false;
    bool substituted = false;
    if (optdef->type == coBools && substitutions_ctxt.rule != ForwardCompatibilitySubstitutionRule::Disable) {
        //FIXME Special handling of vectors of bools, quick and not so dirty solution before PrusaSlicer 2.3.2 release.
    	bool nullable = opt->nullable();
    	ConfigHelpers::DeserializationSubstitution default_value = ConfigHelpers::DeserializationSubstitution::DefaultsToFalse;
    	if (optdef->default_value) {
    		// Default value for vectors of booleans used in a "per extruder" context, thus the default contains just a single value.
    		assert(dynamic_cast<const ConfigOptionVector<unsigned char>*>(optdef->default_value.get()));
			auto &values = static_cast<const ConfigOptionVector<unsigned char>*>(optdef->default_value.get())->values;
			if (values.size() == 1 && values.front() == 1)
				default_value = ConfigHelpers::DeserializationSubstitution::DefaultsToTrue;
		}
    	auto result = nullable ?
    		static_cast<ConfigOptionBoolsNullable*>(opt)->deserialize_with_substitutions(value, append, default_value) :
    		static_cast<ConfigOptionBools*>(opt)->deserialize_with_substitutions(value, append, default_value);
    	success     = result != ConfigHelpers::DeserializationResult::Failed;
    	substituted = result == ConfigHelpers::DeserializationResult::Substituted;
    } else {
		//bool test = (opt_key == "filament_end_gcode");
		success = opt->deserialize(value, append);
	    if (! success && substitutions_ctxt.rule != ForwardCompatibilitySubstitutionRule::Disable &&
	        // Only allow substitutions of an enum value by another enum value or a boolean value with an enum value.
	        // That means, we expect enum values being added in the future and possibly booleans being converted to enums.
            (optdef->type == coEnum || optdef->type == coEnums || optdef->type == coBool) /*&& ConfigHelpers::looks_like_enum_value(value)*/) {
	        // Deserialize failed, try to substitute with a default value.
	        //assert(substitutions_ctxt.rule == ForwardCompatibilitySubstitutionRule::Enable || substitutions_ctxt.rule == ForwardCompatibilitySubstitutionRule::EnableSilent);
	        if (optdef->type == coBool)
	            static_cast<ConfigOptionBool*>(opt)->value = ConfigHelpers::enum_looks_like_true_value(value);
	        else
	        	// Just use the default of the option.
	            opt->set(optdef->default_value.get());
            success     = true;
            substituted = true;
	    }
	}

    if (substituted && (substitutions_ctxt.rule == ForwardCompatibilitySubstitutionRule::Enable ||
                        substitutions_ctxt.rule == ForwardCompatibilitySubstitutionRule::EnableSystemSilent)) {
        // Log the substitution.
        ConfigSubstitution config_substitution;
        config_substitution.opt_def   = optdef;
        config_substitution.old_value = value;
        config_substitution.new_value = ConfigOptionUniquePtr(opt->clone());
        substitutions_ctxt.substitutions.emplace_back(std::move(config_substitution));
    }
    return success;
}

// Return an absolute value of a possibly relative config variable.
// For example, return absolute infill extrusion width, either from an absolute value, or relative to the layer height.
double ConfigBase::get_abs_value(const t_config_option_key &opt_key) const
{
    // Get stored option value.
    const ConfigOption *raw_opt = this->option(opt_key);
    if (raw_opt == nullptr) {
      std::stringstream ss;
      ss << "You can't define an option that need " << opt_key << " without defining it!";
      throw std::runtime_error(ss.str());
    }
    assert(raw_opt != nullptr);

    if (raw_opt->type() == coFloat)
        return static_cast<const ConfigOptionFloat*>(raw_opt)->value;
    if (raw_opt->type() == coInt)
      return static_cast<const ConfigOptionInt *>(raw_opt)->value;
    if (raw_opt->type() == coBool)
      return static_cast<const ConfigOptionBool *>(raw_opt)->value ? 1 : 0;

    const ConfigOptionPercent *cast_opt = nullptr;
    if (raw_opt->type() == coFloatOrPercent) {
        auto cofop = static_cast<const ConfigOptionFloatOrPercent*>(raw_opt);
            if (cofop->value == 0 && boost::ends_with(opt_key, "_line_width")) {
                return this->get_abs_value("line_width");
            }
            if (!cofop->percent)
                return cofop->value;
            cast_opt = cofop;
    }

    if (raw_opt->type() == coPercent) {
      cast_opt = static_cast<const ConfigOptionPercent *>(raw_opt);
    }

    // Get option definition.
    const ConfigDef *def = this->def();
    if (def == nullptr)
        throw NoDefinitionException(opt_key);
    const ConfigOptionDef *opt_def = def->get(opt_key);


    assert(opt_def != nullptr);
    if (opt_def->ratio_over == "")
        return cast_opt->get_abs_value(1);
    // Compute absolute value over the absolute value of the base option.
    //FIXME there are some ratio_over chains, which end with empty ratio_with.
    // For example, XXX_extrusion_width parameters are not handled by get_abs_value correctly.
    return opt_def->ratio_over.empty() ? 0. :
        static_cast<const ConfigOptionFloatOrPercent*>(raw_opt)->get_abs_value(this->get_abs_value(opt_def->ratio_over));
    

    throw ConfigurationError("ConfigBase::get_abs_value(): Not a valid option type for get_abs_value()");
}

// Return an absolute value of a possibly relative config variable.
// For example, return absolute infill extrusion width, either from an absolute value, or relative to a provided value.
double ConfigBase::get_abs_value(const t_config_option_key &opt_key, double ratio_over) const
{
    // Get stored option value.
    const ConfigOption *raw_opt = this->option(opt_key);
    assert(raw_opt != nullptr);
    if (raw_opt->type() != coFloatOrPercent)
        throw ConfigurationError("ConfigBase::get_abs_value(): opt_key is not of coFloatOrPercent");
    // Compute absolute value.
    return static_cast<const ConfigOptionFloatOrPercent*>(raw_opt)->get_abs_value(ratio_over);
}

void ConfigBase::setenv_() const
{
    t_config_option_keys opt_keys = this->keys();
    for (t_config_option_keys::const_iterator it = opt_keys.begin(); it != opt_keys.end(); ++it) {
        // prepend the SLIC3R_ prefix
        std::ostringstream ss;
        ss << "SLIC3R_";
        ss << *it;
        std::string envname = ss.str();

        // capitalize environment variable name
        for (size_t i = 0; i < envname.size(); ++i)
            envname[i] = (envname[i] <= 'z' && envname[i] >= 'a') ? envname[i]-('a'-'A') : envname[i];

        boost::nowide::setenv(envname.c_str(), this->opt_serialize(*it).c_str(), 1);
    }
}

//BBS
ConfigSubstitutions ConfigBase::load_string_map(std::map<std::string, std::string>& key_values, ForwardCompatibilitySubstitutionRule compatibility_rule)
{
    CNumericLocalesSetter locales_setter;

    ConfigSubstitutionContext substitutions_ctxt(compatibility_rule);
    std::map<std::string, std::string>::iterator it;
    for (it = key_values.begin(); it != key_values.end(); it++) {
        try {
            t_config_option_key opt_key = it->first;
            this->set_deserialize(opt_key, it->second, substitutions_ctxt);
        }
        catch (UnknownOptionException& /* e */) {
            // ignore
        }
    }
    return std::move(substitutions_ctxt.substitutions);
}

//BBS: add json support
ConfigSubstitutions ConfigBase::load(const std::string &file, ForwardCompatibilitySubstitutionRule compatibility_rule)
{
    std::map<std::string, std::string> key_values;
    if (is_gcode_file(file))
        return this->load_from_gcode_file(file, compatibility_rule);
    else if (is_json_file(file)) {
        std::string reason;
        return this->load_from_json(file, compatibility_rule, key_values, reason);
    }
    else {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "unsupported format for config file" << file;
        return ConfigSubstitutions();
        //return this->load_from_ini(file, compatibility_rule);
    }
}

//BBS: add json support
ConfigSubstitutions ConfigBase::load_from_json(const std::string &file, ForwardCompatibilitySubstitutionRule compatibility_rule, std::map<std::string, std::string>& key_values, std::string& reason)
{
    int ret = 0;
    ConfigSubstitutionContext substitutions_ctxt(compatibility_rule);

    ret = load_from_json(file, substitutions_ctxt, true, key_values, reason);
    return std::move(substitutions_ctxt.substitutions);
}

int ConfigBase::load_from_json(const std::string &file, ConfigSubstitutionContext& substitution_context, bool load_inherits_to_config, std::map<std::string, std::string>& key_values, std::string& reason)
{
    json j;
    std::list<std::string> different_settings_append;
    std::string new_support_style;
    std::string is_infill_first;
    std::string get_wall_sequence;
    bool is_project_settings = false;

    CNumericLocalesSetter locales_setter;

    std::function<bool(const json::const_iterator&, const char,const char,const bool,std::string&)> parse_str_arr = [&parse_str_arr](const json::const_iterator& it, const char single_sep,const char array_sep,const bool escape_string_style,std::string& value_str)->bool {
        // must have consistent type name
        std::string consistent_type;
        for (auto iter = it.value().begin(); iter != it.value().end(); ++iter) {
            if (consistent_type.empty())
                consistent_type = iter.value().type_name();
            else {
                if (consistent_type != iter.value().type_name())
                    return false;
            }
        }

        bool first = true;
        for (auto iter = it.value().begin(); iter != it.value().end(); iter++) {
            if (iter.value().is_array()) {
                if (!first)
                    value_str += array_sep;
                else
                    first = false;
                bool success = parse_str_arr(iter, single_sep, array_sep,escape_string_style, value_str);
                if (!success)
                    return false;
            }
            else if (iter.value().is_string()) {
                if (!first)
                    value_str += single_sep;
                else
                    first = false;
                if (!escape_string_style)
                    value_str += iter.value();
                else {
                    value_str += "\"";
                    value_str += escape_string_cstyle(iter.value());
                    value_str += "\"";
                }
            }
            else {
                //should not happen
                return false;
            }
        }
        return true;
        };

    try {
        boost::nowide::ifstream ifs(file);
        ifs >> j;
        ifs.close();

        const ConfigDef* config_def = this->def();
        if (config_def == nullptr) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": no config defs!";
            return -1;
        }
        //parse the json elements
        for (auto it = j.begin(); it != j.end(); it++) {
            if (boost::iequals(it.key(),BBL_JSON_KEY_VERSION)) {
                key_values.emplace(BBL_JSON_KEY_VERSION, it.value());
            }
            else if (boost::iequals(it.key(), BBL_JSON_KEY_IS_CUSTOM)) {
                //skip it
            }
            else if (boost::iequals(it.key(), BBL_JSON_KEY_NAME)) {
                key_values.emplace(BBL_JSON_KEY_NAME, it.value());
                if (it.value() == "project_settings")
                    is_project_settings = true;
            }
            else if (boost::iequals(it.key(), BBL_JSON_KEY_URL)) {
                key_values.emplace(BBL_JSON_KEY_URL, it.value());
            }
            else if (boost::iequals(it.key(), BBL_JSON_KEY_TYPE)) {
                key_values.emplace(BBL_JSON_KEY_TYPE, it.value());
            }
            else if (boost::iequals(it.key(), BBL_JSON_KEY_SETTING_ID)) {
                key_values.emplace(BBL_JSON_KEY_SETTING_ID, it.value());
            }
            else if (boost::iequals(it.key(), BBL_JSON_KEY_FILAMENT_ID)) {
                key_values.emplace(BBL_JSON_KEY_FILAMENT_ID, it.value());
            }
            else if (boost::iequals(it.key(), BBL_JSON_KEY_FROM)) {
                key_values.emplace(BBL_JSON_KEY_FROM, it.value());
            }
            else if (boost::iequals(it.key(), BBL_JSON_KEY_DESCRIPTION)) {
                key_values.emplace(BBL_JSON_KEY_DESCRIPTION, it.value());
            }
            else if (boost::iequals(it.key(), BBL_JSON_KEY_INSTANTIATION)) {
                key_values.emplace(BBL_JSON_KEY_INSTANTIATION, it.value());
            }
            else if (!load_inherits_to_config && boost::iequals(it.key(), BBL_JSON_KEY_INHERITS)) {
                key_values.emplace(BBL_JSON_KEY_INHERITS, it.value());
            } else if (boost::iequals(it.key(), ORCA_JSON_KEY_RENAMED_FROM)) {
                key_values.emplace(ORCA_JSON_KEY_RENAMED_FROM, it.value());
            } else {
                t_config_option_key opt_key = it.key();
                std::string value_str;

                if (it.value().is_string()) {
                    //bool test1 = (it.key() == std::string("end_gcode"));
                    this->set_deserialize(opt_key, it.value(), substitution_context);
                    //some logic for special values
                    if (opt_key == "support_type") {
                        //std::string new_value = dynamic_cast<ConfigOptionString*>(this->option(opt_key))->value;
                        if (it.value() == "hybrid(auto)") {
                            different_settings_append.push_back(opt_key);
                            different_settings_append.push_back("support_style");
                            new_support_style = "tree_hybrid";
                        }
                    } else if (opt_key == "wall_infill_order") {
                        //BBS: check wall_infill order to decide if it be different and append to diff_setting_append
                        if (it.value() == "outer wall/inner wall/infill" || it.value() == "infill/outer wall/inner wall" || it.value() == "inner-outer-inner wall/infill") {
                            get_wall_sequence = "wall_seq_diff_to_system";
                        }

                        if (it.value() == "infill/outer wall/inner wall" || it.value() == "infill/inner wall/outer wall") {
                            different_settings_append.push_back("is_infill_first");
                            is_infill_first = "true";
                        }
                    }
                }
                else if (it.value().is_array()) {
                    t_config_option_key opt_key_src = opt_key;
                    this->handle_legacy(opt_key, value_str);
                    if (opt_key.empty()) {
                        //BBS: record these options
                        substitution_context.unrecogized_keys.push_back(opt_key_src);
                        continue;
                    }
                    bool valid = true, first = true;
                    const ConfigOptionDef* optdef = config_def->get(opt_key);
                    if (optdef == nullptr) {
                        // If we didn't find an option, look for any other option having this as an alias.
                        for (const auto& opt : config_def->options) {
                            for (const t_config_option_key& opt_key2 : opt.second.aliases) {
                                if (opt_key2 == opt_key) {
                                    opt_key = opt.first;
                                    optdef = &opt.second;
                                    break;
                                }
                            }
                            if (optdef != nullptr)
                                break;
                        }
                    }

                    char single_sep = ',';
                    char array_sep = '#';  // currenty not used
                    bool escape_string_type = false;
                    if (optdef) {
                        switch (optdef->type)
                        {
                        case coStrings:
                            escape_string_type = true;
                            single_sep = ';';
                            break;
                        case coPointsGroups:
                            single_sep = '#';
                            break;
                        default:
                            break;
                        }
                    }

                    // BBS: we only support 2 depth array
                    valid = parse_str_arr(it, single_sep, array_sep,escape_string_type, value_str);
                    if (!valid) {
                        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": parse " << file << " error, invalid json array for " << it.key();
                        break;
                    }
                    if (valid)
                        this->set_deserialize(opt_key, value_str, substitution_context);
                }
                else {
                    //should not happen
                    BOOST_LOG_TRIVIAL(error) << __FUNCTION__<< ": parse "<<file<<" error, invalid json type for " << it.key();
                }
            }
        }
        if (!different_settings_append.empty()) {
            if (!new_support_style.empty()) {
                ConfigOptionEnum<SupportMaterialStyle>* opt = this->option<ConfigOptionEnum<SupportMaterialStyle>>("support_style", true);
                opt->value = smsTreeHybrid;
            }

            if (!is_infill_first.empty()) {
                ConfigOptionBool *opt = this->option<ConfigOptionBool>("is_infill_first", true);
                opt->value = true;
            }

            if (is_project_settings) {
                std::vector<std::string>& different_settings = this->option<ConfigOptionStrings>("different_settings_to_system", true)->values;
                size_t size = different_settings.size();
                if (size == 0) {
                    size = this->option<ConfigOptionStrings>("filament_settings_id")->values.size() + 2;
                    different_settings.resize(size);
                }

                std::vector<bool> is_first(size, false);
                std::vector<std::vector<std::string>> original_diffs(size);
                for (int index = 0; index < size; index++)
                {
                    if (different_settings[index].empty()) {
                        is_first[index] = true;
                    }
                    else {
                        // remove unneeded key
                        if (get_wall_sequence.empty()) {
                            std::string wall_sqe_string = "wall_sequence";
                            int pos=different_settings[index].find(wall_sqe_string);

                            if (pos != different_settings[index].npos) {
                                int erase_len = wall_sqe_string.size();
                                if (pos + erase_len < different_settings[index].size() && different_settings[index][pos + erase_len] == ';')
                                    erase_len++;
                                different_settings[index].erase(pos, erase_len);
                            }

                        }

                        if (different_settings[index].empty()) {
                            is_first[index] = true;
                            continue;
                        }

                        Slic3r::unescape_strings_cstyle(different_settings[index], original_diffs[index]);
                    }
                }

                for (auto diff_key : different_settings_append)
                {
                    //get the index in the group
                    int index = 0;
                    bool need_insert = true;
                    if (diff_key == "support_type")
                        index = 0;
                    else if (diff_key == "support_style")
                        index = 0;
                    else if (diff_key == "is_infill_first")
                        index = 0;

                    //check whether exist firstly
                    if (!original_diffs[index].empty()) {
                        for (int j = 0; j < original_diffs[index].size(); j++) {
                            if (original_diffs[index][j] == diff_key) {
                                need_insert = false;
                                break;
                            }
                        }
                    }
                    if (!need_insert)
                        continue;

                    //insert this key
                    if (!is_first[index])
                        different_settings[index] += ";";
                    else
                        is_first[index] = false;
                    different_settings[index] += diff_key;
                }
            }
        }
        
        // Do legacy conversion on a completely loaded dictionary.
        // Perform composite conversions, for example merging multiple keys into one key.
        this->handle_legacy_composite();
        return 0;
    }
    catch (const std::ifstream::failure &err)  {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__<< ": parse "<<file<<" got a ifstream error, reason = " << err.what();
        reason = std::string("ifstreamError: ") + err.what();
        //throw ConfigurationError(format("Failed loading configuration file \"%1%\": %2%", file, e.what()));
    }
    catch(nlohmann::detail::parse_error &err) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__<< ": parse "<<file<<" got a nlohmann::detail::parse_error, reason = " << err.what();
        reason = std::string("JsonParseError: ") + err.what();
        //throw ConfigurationError(format("Failed loading configuration file \"%1%\": %2%", file, err.what()));
    }
    catch(std::exception &err) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__<< ": parse "<<file<<" got a generic exception, reason = " << err.what();
        reason = std::string("std::exception: ") + err.what();
    }
    return -1;
}

ConfigSubstitutions ConfigBase::load_from_ini(const std::string &file, ForwardCompatibilitySubstitutionRule compatibility_rule)
{
    try {
        boost::property_tree::ptree tree;
        boost::nowide::ifstream ifs(file);
        boost::property_tree::read_ini(ifs, tree);
        return this->load(tree, compatibility_rule);
    } catch (const ConfigurationError &e) {
        throw ConfigurationError(format("Failed loading configuration file \"%1%\": %2%", file, e.what()));
    }
}

ConfigSubstitutions ConfigBase::load_from_ini_string(const std::string &data, ForwardCompatibilitySubstitutionRule compatibility_rule)
{
    boost::property_tree::ptree tree;
    std::istringstream iss(data);
    boost::property_tree::read_ini(iss, tree);
    return this->load(tree, compatibility_rule);
}

// Loading a "will be one day a legacy format" of configuration stored into 3MF or AMF.
// Accepts the same data as load_from_ini_string(), only with each configuration line possibly prefixed with a semicolon (G-code comment).
ConfigSubstitutions ConfigBase::load_from_ini_string_commented(std::string &&data, ForwardCompatibilitySubstitutionRule compatibility_rule)
{
    // Convert the "data" string into INI format by removing the semi-colons at the start of a line.
    // Also the "; generated by PrusaSlicer ..." comment line will be removed.
    size_t j = 0;
    for (size_t i = 0; i < data.size();)
        if (i == 0 || data[i] == '\n') {
            // Start of a line.
            if (data[i] == '\n') {
                // Consume LF, don't keep empty lines.
                if (j > 0 && data[j - 1] != '\n')
                    data[j ++] = data[i];
                ++ i;
            }
            // Skip all leading spaces;
            for (; i < data.size() && (data[i] == ' ' || data[i] == '\t'); ++ i) ;
            // Skip the semicolon (comment indicator).
            if (i < data.size() && data[i] == ';')
                ++ i;
            // Skip all leading spaces after semicolon.
            for (; i < data.size() && (data[i] == ' ' || data[i] == '\t'); ++ i) ;
            if (strncmp(data.data() + i, "generated by ", 13) == 0) {
                // Skip the "; generated by ..." line.
                for (; i < data.size() && data[i] != '\n'; ++ i);
            }
        } else if (data[i] == '\r' && i + 1 < data.size() && data[i + 1] == '\n') {
            // Skip CR.
            ++ i;
        } else {
            // Consume the rest of the data.
            data[j ++] = data[i ++];
        }
    data.erase(data.begin() + j, data.end());

    return this->load_from_ini_string(data, compatibility_rule);
}

ConfigSubstitutions ConfigBase::load(const boost::property_tree::ptree &tree, ForwardCompatibilitySubstitutionRule compatibility_rule)
{
    ConfigSubstitutionContext substitutions_ctxt(compatibility_rule);
    for (const boost::property_tree::ptree::value_type &v : tree) {
        try {
            t_config_option_key opt_key = v.first;
            this->set_deserialize(opt_key, v.second.get_value<std::string>(), substitutions_ctxt);
        } catch (UnknownOptionException & /* e */) {
            // ignore
        }
    }
    // Do legacy conversion on a completely loaded dictionary.
    // Perform composite conversions, for example merging multiple keys into one key.
    this->handle_legacy_composite();
    return std::move(substitutions_ctxt.substitutions);
}

// BBS
static bool         is_whitespace(char c) { return c == ' ' || c == '\t'; }
static bool         is_end_of_line(char c) { return c == '\r' || c == '\n' || c == 0; }
static bool         is_end_of_gcode_line(char c) { return c == ';' || is_end_of_line(c); }
static bool         is_end_of_word(char c) { return is_whitespace(c) || is_end_of_gcode_line(c); }

static const char* skip_word(const char* c) {
    for (; !is_end_of_word(*c); ++c)
        ; // silence -Wempty-body
    return c;
}

static const char* skip_whitespaces(const char* c) {
    for (; is_whitespace(*c); ++c)
        ; // silence -Wempty-body
    return c;
}

// Load the config keys from the given string.
size_t ConfigBase::load_from_gcode_string_legacy(ConfigBase& config, const char* str, ConfigSubstitutionContext& substitutions)
{
    if (str == nullptr)
        return 0;

    // BBS. Remove line numbers.
    std::regex match_pattern("\nN[0-9]* *");
    std::string replace_pattern = "\n";
    char* result = (char*)calloc(strlen(str) + 1, 1);
    std::regex_replace(result, str, str + strlen(str), match_pattern, replace_pattern);

    // Walk line by line in reverse until a non-configuration key appears.
    const char *data_start = result;
    data_start = skip_whitespaces(data_start);
    if (std::toupper(*data_start) == 'N')
        data_start = skip_word(data_start);

    // boost::nowide::ifstream seems to cook the text data somehow, so less then the 64k of characters may be retrieved.
    const char *end = data_start + strlen(data_start);
    size_t num_key_value_pairs = 0;
    for (;;) {
        // Extract next line.
        for (--end; end > data_start && (*end == '\r' || *end == '\n'); --end);
        if (end == data_start)
            break;
        const char *start = end ++;
        for (; start > data_start && *start != '\r' && *start != '\n'; --start);
        if (start == data_start)
            break;
        // Extracted a line from start to end. Extract the key = value pair.
        if (end - (++ start) < 10 || start[0] != ';' || start[1] != ' ')
            break;
        const char *key = start + 2;
        if (!((*key >= 'a' && *key <= 'z') || (*key >= 'A' && *key <= 'Z')))
            // A key must start with a letter.
            break;
        const char *sep = key;
        for (; sep != end && *sep != '='; ++ sep) ;
        if (sep == end || sep[-1] != ' ' || sep[1] != ' ')
            break;
        const char *value = sep + 2;
        if (value > end)
            break;
        const char *key_end = sep - 1;
        if (key_end - key < 3)
            break;
        // The key may contain letters, digits and underscores.
        for (const char *c = key; c != key_end; ++ c)
            if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_')) {
                key = nullptr;
                break;
            }
        if (key == nullptr)
            break;
        try {
            config.set_deserialize(std::string(key, key_end), std::string(value, end), substitutions);
            ++num_key_value_pairs;
        }
        catch (UnknownOptionException & /* e */) {
            // ignore
        }
        end = start;
    }

    // Do legacy conversion on a completely loaded dictionary.
    // Perform composite conversions, for example merging multiple keys into one key.
    config.handle_legacy_composite();

    free(result);
    return num_key_value_pairs;
}

// Reading a config from G-code back to front for performance reasons: We don't want to scan
// hundreds of MB file for a short config block, which we expect to find at the end of the G-code.
class ReverseLineReader
{
public:
    using pos_type = boost::nowide::ifstream::pos_type;

    // Stop at file_start
    ReverseLineReader(boost::nowide::ifstream &ifs, pos_type file_start) : m_ifs(ifs), m_file_start(file_start)
    {
        m_ifs.seekg(0, m_ifs.end);
        m_file_pos = m_ifs.tellg();
        m_block.assign(m_block_size, 0);
    }

    bool getline(std::string &out) {
        out.clear();
        for (;;) {
            if (m_block_len == 0) {
                // Read the next block.
                m_block_len = size_t(std::min<std::fstream::pos_type>(m_block_size, m_file_pos - m_file_start));
                if (m_block_len == 0)
                    return false;
                m_file_pos -= m_block_len;
                m_ifs.seekg(m_file_pos, m_ifs.beg);
                if (! m_ifs.read(m_block.data(), m_block_len))
                    return false;
            }

            assert(m_block_len > 0);
            // Non-empty buffer. Find another LF.
            int i = int(m_block_len) - 1;
            for (; i >= 0; -- i)
                if (m_block[i] == '\n')
                    break;
            // i is position of LF or -1 if not found.
            if (i == -1) {
                // LF not found. Just make a backup of the buffer and continue.
                out.insert(out.begin(), m_block.begin(), m_block.begin() + m_block_len);
                m_block_len = 0;
            } else {
                assert(i >= 0);
                // Copy new line to the output. It may be empty.
                out.insert(out.begin(), m_block.begin() + i + 1, m_block.begin() + m_block_len);
                // Block length without the newline.
                m_block_len = i;
                // Remove CRLF from the end of the block.
                if (m_block_len > 0 && m_block[m_block_len - 1] == '\r')
                    -- m_block_len;
                return true;
            }
        }
        assert(false);
        return false;
    }

private:
    boost::nowide::ifstream &m_ifs;
    std::vector<char>        m_block;
    size_t                   m_block_size = 65536;
    size_t                   m_block_len  = 0;
    pos_type                 m_file_start;
    pos_type                 m_file_pos   = 0;
};

// Load the config keys from the tail of a G-code file.
ConfigSubstitutions ConfigBase::load_from_gcode_file(const std::string &file, ForwardCompatibilitySubstitutionRule compatibility_rule)
{
    // Read a 64k block from the end of the G-code.
	boost::nowide::ifstream ifs(file);
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(":  before parse_file %1%") % file.c_str();
    // Look for Slic3r or OrcaSlicer header.
    // Look for the header across the whole file as the G-code may have been extended at the start by a post-processing script or the user.
    //BBS
    bool has_delimiters = true;
    {
        //BBS
        std::string bambuslicer_gcode_header = "; OrcaSlicer";

        std::string orcaslicer_gcode_header = std::string("; generated by ");
        orcaslicer_gcode_header += SLIC3R_APP_NAME;

        std::string header;
        bool        header_found = false;
        while (std::getline(ifs, header)) {
            // BBS
            const char* line_c = skip_whitespaces(header.c_str());
            if (std::toupper(*line_c) == 'N')
                line_c = skip_word(line_c);
            line_c = skip_whitespaces(line_c);
            // BBS
            if (strncmp(bambuslicer_gcode_header.c_str(), line_c, strlen(bambuslicer_gcode_header.c_str())) == 0 ||
                strncmp(orcaslicer_gcode_header.c_str(), line_c, strlen(orcaslicer_gcode_header.c_str())) == 0) {
                header_found = true;
                break;
            }
        }
        if (!header_found) {
            std::string error_message = "Not a gcode file generated by ";
            error_message += SLIC3R_APP_FULL_NAME;
            error_message += ".";
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << error_message;
            throw Slic3r::RuntimeError(error_message.c_str());
        }
    }

    ConfigSubstitutionContext substitutions_ctxt(compatibility_rule);
    size_t                    key_value_pairs = 0;

    if (has_delimiters)
    {
        //BBS
        // PrusaSlicer starting with 2.4.0-alpha0 delimits the config section stored into G-code with
        // ; CONFIG_BLOCK_START
        // ...
        // ; CONFIG_BLOCK_END
        bool begin_found = false;
        bool end_found   = false;
        std::string line;
        while (std::getline(ifs, line))
            if (line.rfind("; CONFIG_BLOCK_START",0)==0) {
                begin_found = true;
                break;
            }
        if (!begin_found) {
            //BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << format("Configuration block closing tag \"; CONFIG_BLOCK_START\" not found when reading %1%", file);
            throw Slic3r::RuntimeError(format("Config tag \"; CONFIG_BLOCK_START\" not found"));
        }
        std::string key, value;
        while (std::getline(ifs, line)) {
            if (line.rfind("; CONFIG_BLOCK_END",0)==0) {
                end_found = true;
                break;
            }
            // line should be a valid key = value pair.
            auto pos = line.find('=');
            if (pos != std::string::npos && pos > 1 && line.front() == ';') {
                key   = line.substr(1, pos - 1);
                value = line.substr(pos + 1);
                boost::trim(key);
                boost::trim(value);
                try {
                    this->set_deserialize(key, value, substitutions_ctxt);
                    ++ key_value_pairs;
                } catch (UnknownOptionException & /* e */) {
                    // ignore
                }
            }
        }
        if (!end_found) {
            //BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << format("Configuration block opening tag \"; CONFIG_BLOCK_END\" not found when reading %1%", file);
            throw Slic3r::RuntimeError(format("Config tag \"; CONFIG_BLOCK_END\" not found"));
        }
    }
    else
    {
        auto                      header_end_pos = ifs.tellg();
        // Slicer older than 2.4.0-alpha0 do not emit any delimiter.
        // Try a heuristics reading the G-code from back.
        ifs.seekg(0, ifs.end);
        auto file_length = ifs.tellg();
    	auto data_length = std::min<std::fstream::pos_type>(65535, file_length - header_end_pos);
    	ifs.seekg(file_length - data_length, ifs.beg);
        std::vector<char> data(size_t(data_length) + 1, 0);
        ifs.read(data.data(), data_length);
        ifs.close();
        key_value_pairs = load_from_gcode_string_legacy(*this, data.data(), substitutions_ctxt);
    }

    if (key_value_pairs < 80) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << format("Suspiciously low number of configuration values extracted from %1%: %2%", file, key_value_pairs);
        throw Slic3r::RuntimeError(format("Suspiciously low number of configuration values extracted from %1%: %2%", file, key_value_pairs));
    }

    // Do legacy conversion on a completely loaded dictionary.
    // Perform composite conversions, for example merging multiple keys into one key.
    this->handle_legacy_composite();

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(":  finished to parse_file %1%") % file.c_str();
    return std::move(substitutions_ctxt.substitutions);
}

//BBS: add json support
void ConfigBase::save_to_json(const std::string &file, const std::string &name, const std::string &from, const std::string &version) const
{
    json j;
    //record the headers
    j[BBL_JSON_KEY_VERSION] = version;
    j[BBL_JSON_KEY_NAME] = name;
    j[BBL_JSON_KEY_FROM] = from;

    //record all the key-values
    for (const std::string &opt_key : this->keys())
    {
        const ConfigOption* opt = this->option(opt_key);
        if ( opt->is_scalar() ) {
            if (opt->type() == coString && (opt_key != "bed_custom_texture" && opt_key != "bed_custom_model"))
                //keep \n, \r, \t
                j[opt_key] = (dynamic_cast<const ConfigOptionString *>(opt))->value;
            else
                j[opt_key] = opt->serialize();
        }
        else {
            const ConfigOptionVectorBase* vec = static_cast<const ConfigOptionVectorBase*>(opt);
            //if (!vec->empty())
            std::vector<std::string> string_values = vec->vserialize();

            /*for (int i = 0; i < string_values.size(); i++)
            {
            std::string string_value = escape_string_cstyle(string_values[i]);
            j[opt_key][i] = string_value;
            }*/

            json j_array(string_values);
            j[opt_key] = j_array;
        }
    }

    boost::nowide::ofstream c;
    c.open(file, std::ios::out | std::ios::trunc);
    c << std::setw(4) << j << std::endl;
    c.close();

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" <<__LINE__ << boost::format(", saved config to %1%\n")%file;
}

void ConfigBase::save(const std::string &file) const
{
    boost::nowide::ofstream c;
    c.open(file, std::ios::out | std::ios::trunc);
    c << "# " << Slic3r::header_slic3r_generated() << std::endl;
    for (const std::string &opt_key : this->keys())
        c << opt_key << " = " << this->opt_serialize(opt_key) << std::endl;
    c.close();

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" <<__LINE__ << boost::format(", saved config to %1%\n")%file;
}

// Set all the nullable values to nils.
void ConfigBase::null_nullables()
{
    for (const std::string &opt_key : this->keys()) {
        ConfigOption *opt = this->optptr(opt_key, false);
        assert(opt != nullptr);
        if (opt->nullable())
        	opt->deserialize("nil", ForwardCompatibilitySubstitutionRule::Disable);
    }
}

DynamicConfig::DynamicConfig(const ConfigBase& rhs, const t_config_option_keys& keys)
{
	for (const t_config_option_key& opt_key : keys)
		this->options[opt_key] = std::unique_ptr<ConfigOption>(rhs.option(opt_key)->clone());
}

bool DynamicConfig::operator==(const DynamicConfig &rhs) const
{
    auto it1     = this->options.begin();
    auto it1_end = this->options.end();
    auto it2     = rhs.options.begin();
    auto it2_end = rhs.options.end();
    for (; it1 != it1_end && it2 != it2_end; ++ it1, ++ it2)
		if (it1->first != it2->first || *it1->second != *it2->second)
			// key or value differ
			return false;
    return it1 == it1_end && it2 == it2_end;
}

// Remove options with all nil values, those are optional and it does not help to hold them.
size_t DynamicConfig::remove_nil_options()
{
	size_t cnt_removed = 0;
	for (auto it = options.begin(); it != options.end();)
		if (it->second->is_nil()) {
			it = options.erase(it);
			++ cnt_removed;
		} else
			++ it;
	return cnt_removed;
}

ConfigOption* DynamicConfig::optptr(const t_config_option_key &opt_key, bool create)
{
    auto it = options.find(opt_key);
    if (it != options.end())
        // Option was found.
        return it->second.get();
    if (! create)
        // Option was not found and a new option shall not be created.
        return nullptr;
    // Try to create a new ConfigOption.
    const ConfigDef       *def    = this->def();
    if (def == nullptr)
        throw NoDefinitionException(opt_key);
    const ConfigOptionDef *optdef = def->get(opt_key);
    if (optdef == nullptr)
//        throw ConfigurationError(std::string("Invalid option name: ") + opt_key);
        // Let the parent decide what to do if the opt_key is not defined by this->def().
        return nullptr;
    ConfigOption *opt = optdef->create_default_option();
    this->options.emplace_hint(it, opt_key, std::unique_ptr<ConfigOption>(opt));
    return opt;
}

const ConfigOption* DynamicConfig::optptr(const t_config_option_key &opt_key) const
{
    auto it = options.find(opt_key);
    return (it == options.end()) ? nullptr : it->second.get();
}

bool DynamicConfig::read_cli(int argc, const char* const argv[], t_config_option_keys* extra, t_config_option_keys* keys)
{
    // cache the CLI option => opt_key mapping
    std::map<std::string,std::string> opts;
    for (const auto &oit : this->def()->options)
        for (const std::string &t : oit.second.cli_args(oit.first))
            opts[t] = oit.first;

    bool parse_options = true;
    for (int i = 1; i < argc; ++ i) {
        std::string token = argv[i];
        // Store non-option arguments in the provided vector.
        if (! parse_options || ! boost::starts_with(token, "-")) {
            extra->push_back(token);
            continue;
        }
#ifdef __APPLE__
        if (boost::starts_with(token, "-psn_"))
            // OSX launcher may add a "process serial number", for example "-psn_0_989382" to the command line.
            // While it is supposed to be dropped since OSX 10.9, we will rather ignore it.
            continue;
#endif /* __APPLE__ */
        // Stop parsing tokens as options when -- is supplied.
        if (token == "--") {
            parse_options = false;
            continue;
        }
        // Remove leading dashes (one or two).
        token.erase(token.begin(), token.begin() + (boost::starts_with(token, "--") ? 2 : 1));
        // Read value when supplied in the --key=value form.
        std::string value;
        {
            size_t equals_pos = token.find("=");
            if (equals_pos != std::string::npos) {
                value = token.substr(equals_pos+1);
                token.erase(equals_pos);
            }
        }
        // Look for the cli -> option mapping.
        auto it = opts.find(token);
        bool no = false;
        if (it == opts.end()) {
            //BBS: don't use 'no-' for boolean options
            boost::nowide::cerr << "Invalid option --" << token.c_str() << std::endl;
            return false;
            /* Remove the "no-" prefix used to negate boolean options.
            std::string yes_token;
            if (boost::starts_with(token, "no-")) {
                yes_token = token.substr(3);
                it = opts.find(yes_token);
                no = true;
            }
            if (it == opts.end()) {
                boost::nowide::cerr << "Invalid option --" << token.c_str() << std::endl;
                return false;
            }
            if (no)
                token = yes_token;*/
        }

        const t_config_option_key &opt_key = it->second;
        const ConfigOptionDef     &optdef  = this->def()->options.at(opt_key);

        // If the option type expects a value and it was not already provided,
        // look for it in the next token.
        if (value.empty() && optdef.type != coBool && optdef.type != coBools) {
            if (i == argc-1) {
                boost::nowide::cerr << "Need values for option --" << token.c_str() << std::endl;
                return false;
            }
            value = argv[++ i];
        }

        /*if (no) {
            assert(optdef.type == coBool || optdef.type == coBools);
            if (! value.empty()) {
                boost::nowide::cerr << "Boolean options negated by the --no- prefix cannot have a value." << std::endl;
                return false;
            }
        }*/

        // Store the option value.
        const bool               existing   = this->has(opt_key);
        if (keys != nullptr && ! existing) {
            // Save the order of detected keys.
            keys->push_back(opt_key);
        }
        ConfigOption            *opt_base   = this->option(opt_key, true);
        ConfigOptionVectorBase  *opt_vector = opt_base->is_vector() ? static_cast<ConfigOptionVectorBase*>(opt_base) : nullptr;
        if (opt_vector) {
			if (! existing)
				// remove the default values
				opt_vector->clear();
            // Vector values will be chained. Repeated use of a parameter will append the parameter or parameters
            // to the end of the value.
            if (opt_base->type() == coBools && value.empty())
                static_cast<ConfigOptionBools*>(opt_base)->values.push_back(!no);
            else
                // Deserialize any other vector value (ConfigOptionInts, Floats, Percents, Points) the same way
                // they get deserialized from an .ini file. For ConfigOptionStrings, that means that the C-style unescape
                // will be applied for values enclosed in quotes, while values non-enclosed in quotes are left to be
                // unescaped by the calling shell.
				opt_vector->deserialize(value, true);
        } else if (opt_base->type() == coBool) {
            if (value.empty())
                static_cast<ConfigOptionBool*>(opt_base)->value = !no;
            else
                opt_base->deserialize(value);
        } else if (opt_base->type() == coString) {
            // Do not unescape single string values, the unescaping is left to the calling shell.
            static_cast<ConfigOptionString*>(opt_base)->value = value;
        } else {
            // Just bail out if the configuration value is not understood.
            ConfigSubstitutionContext context(ForwardCompatibilitySubstitutionRule::Disable);
            // Any scalar value of a type different from Bool and String.
            if (! this->set_deserialize_nothrow(opt_key, value, context, false)) {
				boost::nowide::cerr << "Invalid value for option --" << token.c_str() << std::endl;
				return false;
			}
        }
    }
    return true;
}

t_config_option_keys DynamicConfig::keys() const
{
    t_config_option_keys keys;
    keys.reserve(this->options.size());
    for (const auto &opt : this->options)
        keys.emplace_back(opt.first);
    return keys;
}

DynamicConfig::DynamicConfigDifference DynamicConfig::diff_report(const DynamicConfig& rhs) const {
    DynamicConfig::DynamicConfigDifference result;

    std::set<t_config_option_key> all_keys;

    for (const auto& kvp : this->options) {
	all_keys.insert(kvp.first);
    }
    for (const auto& kvp : rhs.options) {
	all_keys.insert(kvp.first);
    }

    for (const auto& key : all_keys) {
	auto left_it = this->options.find(key);
	auto right_it = rhs.options.find(key);

	bool left_has = (left_it != this->options.end());
	bool right_has = (right_it != rhs.options.end());

	if (left_has && right_has) {
	    if (*left_it->second != *right_it->second) {
		result.differences[key] = {
		    left_it->second->serialize(),
		    right_it->second->serialize()
		};
	    }
	} else if (left_has) {
	    result.differences[key] = {
		left_it->second->serialize(),
		std::nullopt
	    };
	} else if (right_has) {
	    result.differences[key] = {
		std::nullopt,
		right_it->second->serialize()
	    };
	}
    }
    return result;
}

std::ostream& operator<<(std::ostream& os, const DynamicConfig::DynamicConfigDifference& diff) {
    if (!diff.is_different()) {
        os << "Configurations are identical.\n";
        return os;
    }

    int missing_right=0, missing_left=0, differ=0;
    os << "DynamicConfig Differences Found (" << diff.differences.size() << " keys):\n";
    for (const auto& kvp : diff.differences) {
        const auto& key = kvp.first;
        const auto& detail = kvp.second;

        os << "  Key: **" << key << "**\n";

        if (detail.is_missing_key()) {
            // Determine which side is missing the key
            if (detail.left_value.has_value()) {
                os << "    - **Missing in Right**: Key exists in left config. Value: " << detail.left_value.value() << "\n";
		missing_right++;
            } else {
                os << "    - **Missing in Left**: Key exists in right config. Value: " << detail.right_value.value() << "\n";
		missing_left++;
            }
        } else if (detail.is_different_value()) {
	    differ++;
            os << "    - **Value Differs**:\n";
            os << "      -> Left Value:  " << detail.left_value.value() << "\n";
            os << "      -> Right Value: " << detail.right_value.value() << "\n";
        }
    }
    os << "Summary: " << missing_right << " missing on right, " << missing_left << " missing on left, and " << differ << " have differing values\n";
    return os;
}

void StaticConfig::set_defaults()
{
    // use defaults from definition
    auto *defs = this->def();
    if (defs != nullptr) {
        for (const std::string &key : this->keys()) {
            const ConfigOptionDef   *def = defs->get(key);
            ConfigOption            *opt = this->option(key);
            if (def != nullptr && opt != nullptr && def->default_value)
                opt->set(def->default_value.get());
        }
    }
}

t_config_option_keys StaticConfig::keys() const
{
    t_config_option_keys keys;
    assert(this->def() != nullptr);
    for (const auto &opt_def : this->def()->options)
        if (this->option(opt_def.first) != nullptr)
            keys.push_back(opt_def.first);
    return keys;
}

// Iterate over the pairs of options with equal keys, call the fn.
// Returns true on early exit by fn().
//BBS: add skipped key logic
template<typename Fn>
static inline bool dynamic_config_iterate(const DynamicConfig &lhs, const DynamicConfig &rhs, Fn fn, const std::set<std::string>* skipped_keys = nullptr)
{
    std::map<t_config_option_key, std::unique_ptr<ConfigOption>>::const_iterator i = lhs.cbegin();
    std::map<t_config_option_key, std::unique_ptr<ConfigOption>>::const_iterator j = rhs.cbegin();
    while (i != lhs.cend() && j != rhs.cend())
        if (i->first < j->first)
            ++ i;
        else if (i->first > j->first)
            ++ j;
        else {
            assert(i->first == j->first);
            if (skipped_keys && (skipped_keys->count(i->first) != 0))
            {
                //do nothing
            }
            else if (fn(i->first, i->second.get(), j->second.get()))
                // Early exit by fn.
                return true;
            ++ i;
            ++ j;
        }
    // Finished to the end.
    return false;
}

// Are the two configs equal? Ignoring options not present in both configs.
//BBS: add skipped keys logic
bool DynamicConfig::equals(const DynamicConfig &other, const std::set<std::string>* skipped_keys) const
{
    return ! dynamic_config_iterate(*this, other,
        [](const t_config_option_key & /* key */, const ConfigOption *l, const ConfigOption *r) { return *l != *r; },
        skipped_keys);
}

// Returns options differing in the two configs, ignoring options not present in both configs.
t_config_option_keys DynamicConfig::diff(const DynamicConfig &other) const
{
    t_config_option_keys diff;
    dynamic_config_iterate(*this, other,
        [&diff](const t_config_option_key &key, const ConfigOption *l, const ConfigOption *r) {
            if (*l != *r)
                diff.emplace_back(key);
            // Continue iterating.
            return false;
        });
    return diff;
}

// Returns options being equal in the two configs, ignoring options not present in both configs.
t_config_option_keys DynamicConfig::equal(const DynamicConfig &other) const
{
    t_config_option_keys equal;
    dynamic_config_iterate(*this, other,
        [&equal](const t_config_option_key &key, const ConfigOption *l, const ConfigOption *r) {
            if (*l == *r)
                equal.emplace_back(key);
            // Continue iterating.
            return false;
        });
    return equal;
}

}

#include <cereal/types/polymorphic.hpp>
CEREAL_REGISTER_TYPE(Slic3r::ConfigOption)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionSingle<double>)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionSingle<int>)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionSingle<std::string>)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionSingle<Slic3r::Vec2d>)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionSingle<Slic3r::Vec3d>)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionSingle<bool>)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionVectorBase)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionVector<double>)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionVector<int>)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionVector<std::string>)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionVector<Slic3r::Vec2d>)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionVector<unsigned char>)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionFloat)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionFloats)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionFloatsNullable)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionInt)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionInts)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionIntsNullable)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionString)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionStrings)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionPercent)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionPercents)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionPercentsNullable)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionFloatOrPercent)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionFloatsOrPercents)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionFloatsOrPercentsNullable)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionPoint)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionPoints)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionPoint3)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionBool)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionBools)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionBoolsNullable)
CEREAL_REGISTER_TYPE(Slic3r::ConfigOptionEnumGeneric)
CEREAL_REGISTER_TYPE(Slic3r::ConfigBase)
CEREAL_REGISTER_TYPE(Slic3r::DynamicConfig)

CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOption, Slic3r::ConfigOptionSingle<double>)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOption, Slic3r::ConfigOptionSingle<int>)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOption, Slic3r::ConfigOptionSingle<std::string>)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOption, Slic3r::ConfigOptionSingle<Slic3r::Vec2d>)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOption, Slic3r::ConfigOptionSingle<Slic3r::Vec3d>)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOption, Slic3r::ConfigOptionSingle<bool>)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOption, Slic3r::ConfigOptionVectorBase)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVectorBase, Slic3r::ConfigOptionVector<double>)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVectorBase, Slic3r::ConfigOptionVector<int>)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVectorBase, Slic3r::ConfigOptionVector<std::string>)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVectorBase, Slic3r::ConfigOptionVector<Slic3r::Vec2d>)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVectorBase, Slic3r::ConfigOptionVector<unsigned char>)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionSingle<double>, Slic3r::ConfigOptionFloat)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVector<double>, Slic3r::ConfigOptionFloats)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVector<double>, Slic3r::ConfigOptionFloatsNullable)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionSingle<int>, Slic3r::ConfigOptionInt)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVector<int>, Slic3r::ConfigOptionInts)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVector<int>, Slic3r::ConfigOptionIntsNullable)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionSingle<std::string>, Slic3r::ConfigOptionString)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVector<std::string>, Slic3r::ConfigOptionStrings)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionFloat, Slic3r::ConfigOptionPercent)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionFloats, Slic3r::ConfigOptionPercents)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionFloats, Slic3r::ConfigOptionPercentsNullable)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionPercent, Slic3r::ConfigOptionFloatOrPercent)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVector<Slic3r::FloatOrPercent>, Slic3r::ConfigOptionFloatsOrPercents)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVector<Slic3r::FloatOrPercent>, Slic3r::ConfigOptionFloatsOrPercentsNullable)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionSingle<Slic3r::Vec2d>, Slic3r::ConfigOptionPoint)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVector<Slic3r::Vec2d>, Slic3r::ConfigOptionPoints)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionSingle<Slic3r::Vec3d>, Slic3r::ConfigOptionPoint3)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionSingle<bool>, Slic3r::ConfigOptionBool)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVector<unsigned char>, Slic3r::ConfigOptionBools)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionVector<unsigned char>, Slic3r::ConfigOptionBoolsNullable)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigOptionInt, Slic3r::ConfigOptionEnumGeneric)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::ConfigBase, Slic3r::DynamicConfig)
