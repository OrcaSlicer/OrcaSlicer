#include <cassert>

#include "Config.hpp"
#include "Exception.hpp"
#include "Preset.hpp"
#include "PresetBundle.hpp"
#include "AppConfig.hpp"

#ifdef _MSC_VER
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <Windows.h>
#endif /* _MSC_VER */

// instead of #include "slic3r/GUI/I18N.hpp" :
#ifndef L
// !!! If you needed to translate some string,
// !!! please use _L(string)
// !!! _() - is a standard wxWidgets macro to translate
// !!! L() is used only for marking localizable string
// !!! It will be used in "xgettext" to create a Locating Message Catalog.
#define L(s) s
#endif /* L */

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <unordered_map>
#include <boost/format.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/predicate.hpp>
//BBS: add regex
#include <boost/algorithm/string/regex.hpp>

#include <boost/nowide/convert.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/locale.hpp>
#include <boost/log/trivial.hpp>

#include "libslic3r.h"
#include "Utils.hpp"
#include "Time.hpp"
#include "PlaceholderParser.hpp"
#include "libslic3r/GCode/Thumbnails.hpp"

using boost::property_tree::ptree;

namespace Slic3r {

//BBS: add a function to load the version from xxx.json
Semver get_version_from_json(std::string file_path)
{
    try {
        boost::nowide::ifstream ifs(file_path);
        json j;
        ifs >> j;
        std::string version_str = j.at(BBL_JSON_KEY_VERSION);

        auto config_version = Semver::parse(version_str);
        if (! config_version) {
            return Semver();
        } else {
            return *config_version;
        }
    }
    catch(nlohmann::detail::parse_error &err) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__<< ": parse "<<file_path<<" got a nlohmann::detail::parse_error, reason = " << err.what();
        return Semver();
        //throw ConfigurationError(format("Failed loading configuration file \"%1%\": %2%", file_path, err.what()));
    }
}

//BBS: add a function to load the key-values from xxx.json
int get_values_from_json(std::string file_path, std::vector<std::string>& keys, std::map<std::string, std::string>& key_values)
{
    try {
        boost::nowide::ifstream ifs(file_path);
        json j;
        ifs >> j;

        for (int i=0; i < keys.size(); i++)
        {
            if (j.contains(keys[i])) {
                std::string value = j.at(keys[i]);
                key_values.emplace(keys[i], value);
            }
        }
    }
    catch(nlohmann::detail::parse_error &err) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__<< ": parse "<<file_path<<" got a nlohmann::detail::parse_error, reason = " << err.what();
        //throw ConfigurationError(format("Failed loading json file \"%1%\": %2%", file_path, err.what()));
        return 0;
    }
    return key_values.size();
}

ConfigFileType guess_config_file_type(const ptree &tree)
{
    size_t app_config   = 0;
    size_t bundle       = 0;
    size_t config       = 0;
    for (const ptree::value_type &v : tree) {
        if (v.second.empty()) {
            if (
#ifdef SUPPORT_BACKGROUND_PROCESSING
                v.first == "background_processing" ||
#endif
                v.first == "last_export_path")
                ++ app_config;
            else if (v.first == "nozzle_diameter" ||
                v.first == "filament_diameter")
                ++ config;
        } else if (boost::algorithm::starts_with(v.first, "print:") ||
            boost::algorithm::starts_with(v.first, "filament:") ||
            boost::algorithm::starts_with(v.first, "printer:") ||
            v.first == "settings")
            ++ bundle;
        else if (v.first == "presets") {
            ++ app_config;
            ++ bundle;
        } else if (v.first == "recent") {
            for (auto &kvp : v.second)
                if (kvp.first == "settings_folder" || kvp.first == "last_opened_folder")
                    ++ app_config;
        }
    }
    return (app_config > bundle && app_config > config) ? CONFIG_FILE_TYPE_APP_CONFIG :
           (bundle > config) ? CONFIG_FILE_TYPE_CONFIG_BUNDLE : CONFIG_FILE_TYPE_CONFIG;
}


VendorProfile VendorProfile::from_ini(const boost::filesystem::path &path, bool load_all)
{
    ptree tree;
    boost::filesystem::ifstream ifs(path);
    boost::property_tree::read_ini(ifs, tree);
    return VendorProfile::from_ini(tree, path, load_all);
}

static const std::unordered_map<std::string, std::string> pre_family_model_map {{
    { "MK3",        "MK3" },
    { "MK3MMU2",    "MK3" },
    { "MK2.5",      "MK2.5" },
    { "MK2.5MMU2",  "MK2.5" },
    { "MK2S",       "MK2" },
    { "MK2SMM",     "MK2" },
    { "SL1",        "SL1" },
}};


// 中间版本兼容性处理，如果是nil值，先改成default值，再进行扩展
void extend_default_config_length(DynamicPrintConfig& config, const bool set_nil_to_default, const DynamicPrintConfig& defaults)
{
    constexpr int default_param_length = 1;
    int filament_variant_length = default_param_length;
    int process_variant_length = default_param_length;
    int machine_variant_length = default_param_length;

    // Orca: use nozzle/extruder count as the default printer variant length
    // because non-BBL multi-extruder printers currently do not support extruder variant.
    if (config.has("nozzle_diameter")) {
        auto* nozzle_diameter = dynamic_cast<const ConfigOptionFloats*>(config.option("nozzle_diameter"));
        machine_variant_length = nozzle_diameter->values.size();
    }

    if(config.has("filament_extruder_variant"))
        filament_variant_length = config.option<ConfigOptionStrings>("filament_extruder_variant")->size();
    if(config.has("print_extruder_variant"))
        process_variant_length = config.option<ConfigOptionStrings>("print_extruder_variant")->size();
    if(config.has("printer_extruder_variant"))  // Use existing variant list if specified, so BBL's multi-variant profiles still works
        machine_variant_length = config.option<ConfigOptionStrings>("printer_extruder_variant")->size();

    auto replace_nil_and_resize = [&](const std::string & key, int length){
        ConfigOption* raw_ptr = config.option(key);
        ConfigOptionVectorBase* opt_vec = static_cast<ConfigOptionVectorBase *>(raw_ptr);
        if(set_nil_to_default && raw_ptr->is_nil() && defaults.has(key) && std::find(filament_extruder_override_keys.begin(), filament_extruder_override_keys.end(), key) == filament_extruder_override_keys.end()){
            opt_vec->clear();
            opt_vec->resize(length, defaults.option(key));
        }
        else{
            opt_vec->resize(length, raw_ptr);
        }
    };

    for(auto& key :config.keys()){
        if(auto iter = print_options_with_variant.find(key); iter != print_options_with_variant.end()){
            replace_nil_and_resize(key, process_variant_length);
        }
        else if(auto iter = filament_options_with_variant.find(key); iter != filament_options_with_variant.end()){
            replace_nil_and_resize(key, filament_variant_length);
        }
        else if(auto iter = printer_options_with_variant_1.find(key); iter != printer_options_with_variant_1.end()){
            replace_nil_and_resize(key, machine_variant_length);
        }
        else if(auto iter = printer_options_with_variant_2.find(key); iter != printer_options_with_variant_2.end()){
            replace_nil_and_resize(key, machine_variant_length * 2);
        }
    }
}


VendorProfile VendorProfile::from_ini(const ptree &tree, const boost::filesystem::path &path, bool load_all)
{
    static const std::string printer_model_key = "printer_model:";
    static const std::string filaments_section = "default_filaments";
    static const std::string materials_section = "default_sla_materials";

    const std::string id = path.stem().string();

    if (! boost::filesystem::exists(path)) {
        throw Slic3r::RuntimeError((boost::format("Cannot load Vendor Config Bundle `%1%`: File not found: `%2%`.") % id % path).str());
    }

    VendorProfile res(id);

    // Helper to get compulsory fields
    auto get_or_throw = [&](const ptree &tree, const std::string &key) -> ptree::const_assoc_iterator
    {
        auto res = tree.find(key);
        if (res == tree.not_found()) {
            throw Slic3r::RuntimeError((boost::format("Vendor Config Bundle `%1%` is not valid: Missing secion or key: `%2%`.") % id % key).str());
        }
        return res;
    };

    // Load the header
    const auto &vendor_section = get_or_throw(tree, "vendor")->second;
    res.name = get_or_throw(vendor_section, "name")->second.data();

    auto config_version_str = get_or_throw(vendor_section, "config_version")->second.data();
    auto config_version = Semver::parse(config_version_str);
    if (! config_version) {
        throw Slic3r::RuntimeError((boost::format("Vendor Config Bundle `%1%` is not valid: Cannot parse config_version: `%2%`.") % id % config_version_str).str());
    } else {
        res.config_version = std::move(*config_version);
    }

    // Load URLs
    const auto config_update_url = vendor_section.find("config_update_url");
    if (config_update_url != vendor_section.not_found()) {
        res.config_update_url = config_update_url->second.data();
    }

    const auto changelog_url = vendor_section.find("changelog_url");
    if (changelog_url != vendor_section.not_found()) {
        res.changelog_url = changelog_url->second.data();
    }

    if (! load_all) {
        return res;
    }

    // Load printer models
    for (auto &section : tree) {
        if (boost::starts_with(section.first, printer_model_key)) {
            VendorProfile::PrinterModel model;
            model.id = section.first.substr(printer_model_key.size());
            model.name = section.second.get<std::string>("name", model.id);

            const char *technology_fallback = boost::algorithm::starts_with(model.id, "SL") ? "SLA" : "FFF";

            auto technology_field = section.second.get<std::string>("technology", technology_fallback);
            if (! ConfigOptionEnum<PrinterTechnology>::from_string(technology_field, model.technology)) {
                BOOST_LOG_TRIVIAL(error) << boost::format("Vendor bundle: `%1%`: Invalid printer technology field: `%2%`") % id % technology_field;
                model.technology = ptFFF;
            }

            model.family = section.second.get<std::string>("family", std::string());
            if (model.family.empty() && res.name == "BBL") {
                // If no family is specified, it can be inferred for known printers
                const auto from_pre_map = pre_family_model_map.find(model.id);
                if (from_pre_map != pre_family_model_map.end()) { model.family = from_pre_map->second; }
            }
#if 0
            // Remove SLA printers from the initial alpha.
            if (model.technology == ptSLA)
                continue;
#endif
            section.second.get<std::string>("variants", "");
            const auto variants_field = section.second.get<std::string>("variants", "");
            std::vector<std::string> variants;
            if (Slic3r::unescape_strings_cstyle(variants_field, variants)) {
                for (const std::string &variant_name : variants) {
                    if (model.variant(variant_name) == nullptr)
                        model.variants.emplace_back(VendorProfile::PrinterVariant(variant_name));
                }
            } else {
                BOOST_LOG_TRIVIAL(error) << boost::format("Vendor bundle: `%1%`: Malformed variants field: `%2%`") % id % variants_field;
            }
            auto default_materials_field = section.second.get<std::string>("default_materials", "");
            if (default_materials_field.empty())
            	default_materials_field = section.second.get<std::string>("default_filaments", "");
            if (Slic3r::unescape_strings_cstyle(default_materials_field, model.default_materials)) {
            	Slic3r::sort_remove_duplicates(model.default_materials);
            	if (! model.default_materials.empty() && model.default_materials.front().empty())
            		// An empty material was inserted into the list of default materials. Remove it.
            		model.default_materials.erase(model.default_materials.begin());
            } else {
                BOOST_LOG_TRIVIAL(error) << boost::format("Vendor bundle: `%1%`: Malformed default_materials field: `%2%`") % id % default_materials_field;
            }
            model.bed_model   = section.second.get<std::string>("bed_model", "");
            model.bed_texture = section.second.get<std::string>("bed_texture", "");
            if (! model.id.empty() && ! model.variants.empty())
                res.models.push_back(std::move(model));
        }
    }

    // Load filaments and sla materials to be installed by default
    const auto filaments = tree.find(filaments_section);
    if (filaments != tree.not_found()) {
        for (auto &pair : filaments->second) {
            if (pair.second.data() == "1") {
                res.default_filaments.insert(pair.first);
            }
        }
    }
    const auto materials = tree.find(materials_section);
    if (materials != tree.not_found()) {
        for (auto &pair : materials->second) {
            if (pair.second.data() == "1") {
                res.default_sla_materials.insert(pair.first);
            }
        }
    }

    return res;
}

std::vector<std::string> VendorProfile::families() const
{
    std::vector<std::string> res;
    unsigned num_familiies = 0;

    for (auto &model : models) {
        if (std::find(res.begin(), res.end(), model.family) == res.end()) {
            res.push_back(model.family);
            num_familiies++;
        }
    }

    return res;
}

// Suffix to be added to a modified preset name in the combo box.
static std::string g_suffix_modified = " (modified)";
const std::string& Preset::suffix_modified()
{
    return g_suffix_modified;
}

void Preset::update_suffix_modified(const std::string& new_suffix_modified)
{
    g_suffix_modified = new_suffix_modified;
}
// Remove an optional "(modified)" suffix from a name.
// This converts a UI name to a unique preset identifier.
std::string Preset::remove_suffix_modified(const std::string &name)
{
    return boost::algorithm::starts_with(name, g_suffix_modified) ?
        name.substr(g_suffix_modified.size()) :
        name;
}

// Update new extruder fields at the printer profile.
void Preset::normalize(DynamicPrintConfig &config)
{
    size_t n = 1;
    if (config.option("single_extruder_multi_material") == nullptr || config.opt_bool("single_extruder_multi_material")) {
        // BBS
        auto* filament_diameter = dynamic_cast<const ConfigOptionFloats*>(config.option("filament_diameter"));
        if (filament_diameter != nullptr) {
            n = filament_diameter->values.size();
            // Loaded the FFF Printer settings. Verify, that all extruder dependent values have enough values.
            config.set_num_filaments((unsigned int) n);
        }
    } else {
        auto* nozzle_diameter = dynamic_cast<const ConfigOptionFloats*>(config.option("nozzle_diameter"));
        if (nozzle_diameter != nullptr) {
            n = nozzle_diameter->values.size();
            // Loaded the FFF Printer settings. Verify, that all extruder dependent values have enough values.
            config.set_num_extruders((unsigned int) n);
        }
    }

    if (config.option("filament_diameter") != nullptr) {
        // This config contains single or multiple filament presets.
        // Ensure that the filament preset vector options contain the correct number of values.
        const auto &defaults = FullPrintConfig::defaults();
        for (const std::string &key : Preset::filament_options()) {
            if (key == "compatible_prints" || key == "compatible_printers")
                continue;
            if (filament_options_with_variant.find(key) != filament_options_with_variant.end())
                continue;
            auto *opt = config.option(key, false);
            /*assert(opt != nullptr);
            assert(opt->is_vector());*/
            if (opt != nullptr && opt->is_vector())
                static_cast<ConfigOptionVectorBase*>(opt)->resize(n, defaults.option(key));
        }
        // The following keys are mandatory for the UI, but they are not part of FullPrintConfig, therefore they are handled separately.
        for (const std::string &key : { "filament_settings_id" }) {
            auto *opt = config.option(key, false);
            assert(opt == nullptr || opt->type() == coStrings);
            if (opt != nullptr && opt->type() == coStrings)
                static_cast<ConfigOptionStrings*>(opt)->values.resize(n, std::string());
        }
    }

    handle_legacy_sla(config);
}

std::string Preset::remove_invalid_keys(DynamicPrintConfig &config, const DynamicPrintConfig &default_config)
{
    std::string incorrect_keys;
    for (const std::string &key : config.keys())
        if (! default_config.has(key)) {
            if (incorrect_keys.empty())
                incorrect_keys = key;
            else {
                incorrect_keys += ", ";
                incorrect_keys += key;
            }
            config.erase(key);
        }
    return incorrect_keys;
}

std::string  Preset::get_type_string(Preset::Type type)
{
    switch (type) {
        case Preset::Type::TYPE_FILAMENT:
            return PRESET_FILAMENT_NAME;
        case Preset::Type::TYPE_PRINT:
            return PRESET_PRINT_NAME;
        case Preset::Type::TYPE_PRINTER:
            return PRESET_PRINTER_NAME;
        case Preset::Type::TYPE_PHYSICAL_PRINTER:
            return "physical_printer";
        case Preset::Type::TYPE_INVALID:
            return "invalid";
        default:
            return "invalid";
    }
}

std::string  Preset::get_iot_type_string(Preset::Type type)
{
    switch (type) {
    case Preset::Type::TYPE_FILAMENT:
        return PRESET_IOT_FILAMENT_TYPE;
    case Preset::Type::TYPE_PRINT:
        return PRESET_IOT_PRINT_TYPE;
    case Preset::Type::TYPE_PRINTER:
        return PRESET_IOT_PRINTER_TYPE;

    default:
        return "invalid";
    }
}

//make the type string compatibility with local and iot type string
Preset::Type Preset::get_type_from_string(std::string type_str)
{
    if (type_str.compare(PRESET_PRINT_NAME) == 0 || type_str.compare(PRESET_IOT_PRINT_TYPE) == 0)
        return Preset::Type::TYPE_PRINT;
    else if (type_str.compare(PRESET_FILAMENT_NAME) == 0 || type_str.compare(PRESET_IOT_FILAMENT_TYPE) == 0)
        return Preset::Type::TYPE_FILAMENT;
    else if (type_str.compare(PRESET_PRINTER_NAME) == 0 || type_str.compare(PRESET_IOT_PRINTER_TYPE) == 0)
        return Preset::Type::TYPE_PRINTER;
    else
        return Preset::Type::TYPE_INVALID;
}


void Preset::load_info(const std::string& file)
{
    try {
        boost::property_tree::ptree tree;
        boost::nowide::ifstream ifs(file);
        boost::property_tree::read_ini(ifs, tree);
        if (tree.empty()) return;
        for (const boost::property_tree::ptree::value_type &v : tree) {
            if (v.first.compare("sync_info") == 0)
                this->sync_info = v.second.get_value<std::string>();
            else if (v.first.compare("user_id") == 0)
                this->user_id = v.second.get_value<std::string>();
            else if (v.first.compare("setting_id") == 0) {
                this->setting_id = v.second.get_value<std::string>();
                if (this->setting_id.compare("null") == 0)
                    this->setting_id.clear();
            }
            else if (v.first.compare("base_id") == 0) {
                this->base_id = v.second.get_value<std::string>();
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " load info from: " << file << " and base_id: " << this->base_id;
                if (this->base_id.compare("null") == 0)
                    this->base_id.clear();
            }
            else if (v.first.compare("updated_time") == 0) {
                std::string time = v.second.get_value<std::string>();
                this->updated_time = std::atoll(time.c_str());
            }
        }
    }
    catch (...) {
        return;
    }

    //TODO: workaround for current info file convert, will remove it later
    if (this->updated_time == 0) {
        this->updated_time = (long long)Slic3r::Utils::get_current_time_utc();
        //this->sync_info = "update";
        BOOST_LOG_TRIVIAL(info) << boost::format("old info file, updated time to %1%") % this->updated_time;
        save_info();
    }
}

void Preset::save_info(std::string file)
{
    //BBS: add project embedded preset logic
    if (this->is_project_embedded)
        return;
    if (file.empty()) {
        fs::path idx_file(this->file);
        idx_file.replace_extension(".info");
        file = idx_file.string();
    }

    boost::nowide::ofstream c;
    c.open(file, std::ios::out | std::ios::trunc);
    std::string sync_info_to_save;
    //BBS: hold is used for stop requesting to server this time
    if (this->sync_info.compare("hold") != 0)
        sync_info_to_save = this->sync_info;
    c << "sync_info" << " = " << sync_info_to_save << std::endl;
    c << "user_id" << " = " << this->user_id << std::endl;
    c << "setting_id" << " = " << this->setting_id << std::endl;
    c << "base_id" << " = " << this->base_id << std::endl;
    c << "updated_time" << " = " << std::to_string(this->updated_time) << std::endl;
    c.close();
}

void Preset::remove_files()
{
    //BBS: add project embedded preset logic
    if (this->is_project_embedded)
        return;
    // Erase the preset file.
    boost::nowide::remove(this->file.c_str());
    fs::path idx_path(this->file);
    idx_path.replace_extension(".info");
    if (fs::exists(idx_path))
        boost::nowide::remove(idx_path.string().c_str());
}

//BBS: add logic for only difference save
void Preset::save(DynamicPrintConfig* parent_config)
{
    //BBS: add project embedded preset logic
    if (this->is_project_embedded)
        return;
    //BBS: change to json format
    //this->config.save(this->file);
    std::string from_str;
    if (this->is_user())
        from_str = std::string("User");
    else if (this->is_project_embedded)
        from_str = std::string("Project");
    else if (this->is_system)
        from_str = std::string("System");
    else
        from_str = std::string("Default");

    boost::filesystem::create_directories(fs::path(this->file).parent_path());

    //BBS: only save difference if it has parent
    if (parent_config) {
        DynamicPrintConfig temp_config;
        std::vector<std::string> dirty_options = config.diff(*parent_config);

        std::string extruder_id_name, extruder_variant_name;
        std::set<std::string> *key_set1 = nullptr, *key_set2 = nullptr;
        Preset::get_extruder_names_and_keysets(type, extruder_id_name, extruder_variant_name, &key_set1, &key_set2);

        if (!extruder_id_name.empty()) {
            dirty_options.emplace_back(extruder_id_name);
        }
        if (!extruder_variant_name.empty()) {
            dirty_options.emplace_back(extruder_variant_name);
        }

        for (auto option: dirty_options)
        {
            ConfigOption *opt_src = config.option(option);
            ConfigOption *opt_dst = temp_config.option(option, true);
            if (opt_dst->is_scalar() || !(opt_dst->nullable()))
                opt_dst->set(opt_src);
            else {
                ConfigOptionVectorBase* opt_vec_src = static_cast<ConfigOptionVectorBase*>(opt_src);
                ConfigOptionVectorBase* opt_vec_dst = static_cast<ConfigOptionVectorBase*>(opt_dst);
                ConfigOptionVectorBase* opt_vec_inherit = static_cast<ConfigOptionVectorBase*>(parent_config->option(option));
                if (opt_vec_src->size() == 1)
                    opt_dst->set(opt_src);
                else if (key_set1->find(option) != key_set1->end()) {
                    opt_vec_dst->set_with_nil(opt_vec_src, opt_vec_inherit, 1);
                }
                else if (key_set2->find(option) != key_set2->end()) {
                    opt_vec_dst->set_with_nil(opt_vec_src, opt_vec_inherit, 2);
                }
                else
                    opt_dst->set(opt_src);
            }
        }
        temp_config.save_to_json(this->file, this->name, from_str, this->version.to_string());
    } else if (!filament_id.empty() && inherits().empty()) {
        DynamicPrintConfig temp_config = config;
        temp_config.set_key_value(BBL_JSON_KEY_FILAMENT_ID, new ConfigOptionString(filament_id));
        temp_config.save_to_json(this->file, this->name, from_str, this->version.to_string());
    } else {
        this->config.save_to_json(this->file, this->name, from_str, this->version.to_string());
    }
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " save config for: " << this->name << " and filament_id: " << filament_id << " and base_id: " << this->base_id;

    fs::path idx_file(this->file);
    idx_file.replace_extension(".info");
    this->save_info(idx_file.string());
}

void Preset::reload(Preset const &parent)
{
    DynamicPrintConfig config;
    // BBS: change to json format
    // ConfigSubstitutions config_substitutions = config.load_from_ini(preset.file, substitution_rule);
    std::map<std::string, std::string> key_values;
    std::string                        reason;
    ForwardCompatibilitySubstitutionRule substitution_rule    = ForwardCompatibilitySubstitutionRule::Disable;
    try {
        ConfigSubstitutions                config_substitutions = config.load_from_json(file, substitution_rule, key_values, reason);
        this->config = parent.config;
        this->config.apply(std::move(config));
    } catch (const std::exception &err) {
        BOOST_LOG_TRIVIAL(error) << boost::format("Failed loading the user-config file: %1%. Reason: %2%") % file % err.what();
    }
}

// Return a label of this preset, consisting of a name and a "(modified)" suffix, if this preset is dirty.
std::string Preset::label(bool no_alias) const
{
    return (this->is_dirty ? g_suffix_modified : "")
        + ((no_alias || this->alias.empty()) ? this->name : this->alias);
}

bool is_compatible_with_print(const PresetWithVendorProfile &preset, const PresetWithVendorProfile &active_print, const PresetWithVendorProfile &active_printer)
{
    // Orca: we allow cross vendor compatibility
	// if (preset.vendor != nullptr && preset.vendor != active_printer.vendor)
	// 	// The current profile has a vendor assigned and it is different from the active print's vendor.
	// 	return false;
    auto &condition             = preset.preset.compatible_prints_condition();
    auto *compatible_prints     = dynamic_cast<const ConfigOptionStrings*>(preset.preset.config.option("compatible_prints"));
    bool  has_compatible_prints = compatible_prints != nullptr && ! compatible_prints->values.empty();
    if (! has_compatible_prints && ! condition.empty()) {
        try {
            return PlaceholderParser::evaluate_boolean_expression(condition, active_print.preset.config);
        } catch (const std::runtime_error &err) {
            //FIXME in case of an error, return "compatible with everything".
            printf("Preset::is_compatible_with_print - parsing error of compatible_prints_condition %s:\n%s\n", active_print.preset.name.c_str(), err.what());
            return true;
        }
    }
    return preset.preset.is_default || active_print.preset.name.empty() || ! has_compatible_prints ||
        std::find(compatible_prints->values.begin(), compatible_prints->values.end(), active_print.preset.name) !=
            compatible_prints->values.end();
}

//BBS: If one filament or process preset is compatible with one system printer preset,
// then we think this filament or process preset should be compatible with all
// user printer preset which is inherited from this system printer preset.
// Because printer_model and nozzle_diameter in BBL system machine preset
// can't be changed by user.
bool is_compatible_with_parent_printer(const PresetWithVendorProfile& preset, const PresetWithVendorProfile& active_printer)
{
    auto *compatible_printers     = dynamic_cast<const ConfigOptionStrings*>(preset.preset.config.option("compatible_printers"));
    bool  has_compatible_printers = compatible_printers != nullptr && ! compatible_printers->values.empty();
    //BBS: FIXME only check the parent now, but should check grand-parent as well.
    return has_compatible_printers &&
           std::find(compatible_printers->values.begin(), compatible_printers->values.end(), active_printer.preset.inherits()) !=
               compatible_printers->values.end();
}

bool is_compatible_with_printer(const PresetWithVendorProfile &preset, const PresetWithVendorProfile &active_printer, const DynamicPrintConfig *extra_config)
{
    // Orca: we allow cross vendor compatibility
	// if (preset.vendor != nullptr && preset.vendor != active_printer.vendor)
	// 	// The current profile has a vendor assigned and it is different from the active print's vendor.
	// 	return false;

    // Orca: check excluded printers
    if (preset.vendor != nullptr && preset.preset.type == Preset::TYPE_FILAMENT) {
        const auto& excluded_printers = preset.preset.m_excluded_from;
        const auto  excluded         = preset.vendor->name == PresetBundle::ORCA_FILAMENT_LIBRARY &&
                              excluded_printers.find(active_printer.preset.name) != excluded_printers.end();
        if (excluded)
            return false;
    }
    auto &condition               = preset.preset.compatible_printers_condition();
    auto *compatible_printers     = dynamic_cast<const ConfigOptionStrings*>(preset.preset.config.option("compatible_printers"));
    bool  has_compatible_printers = compatible_printers != nullptr && ! compatible_printers->values.empty();
    if (! has_compatible_printers && ! condition.empty()) {
        try {
            return PlaceholderParser::evaluate_boolean_expression(condition, active_printer.preset.config, extra_config);
        } catch (const std::runtime_error &err) {
            //FIXME in case of an error, return "compatible with everything".
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": parsing error of compatible_printers_condition %1%: %2%")%active_printer.preset.name %err.what();
            return true;
        }
    }
    return preset.preset.is_default || active_printer.preset.name.empty() || !has_compatible_printers ||
           std::find(compatible_printers->values.begin(), compatible_printers->values.end(), active_printer.preset.name) !=
               compatible_printers->values.end() ||
           (!active_printer.preset.is_system && is_compatible_with_parent_printer(preset, active_printer));
}

bool is_compatible_with_printer(const PresetWithVendorProfile &preset, const PresetWithVendorProfile &active_printer)
{
    DynamicPrintConfig config;
    config.set_key_value("printer_preset", new ConfigOptionString(active_printer.preset.name));
    const ConfigOption *opt = active_printer.preset.config.option("nozzle_diameter");
    if (opt)
        config.set_key_value("num_extruders", new ConfigOptionInt((int)static_cast<const ConfigOptionFloats*>(opt)->values.size()));
    return is_compatible_with_printer(preset, active_printer, &config);
}

void Preset::set_visible_from_appconfig(const AppConfig &app_config)
{
    //BBS: add config related log
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": name %1%, is_visible %2%")%name % is_visible;
    if (vendor == nullptr) { return; }

    if (type == TYPE_PRINTER) {
        const std::string &model = config.opt_string("printer_model");
        const std::string &variant = config.opt_string("printer_variant");
        if (model.empty() || variant.empty())
        	return;
        is_visible = app_config.get_variant(vendor->id, model, variant);
    } else if (type == TYPE_FILAMENT || type == TYPE_SLA_MATERIAL) {
    	const std::string &section_name = (type == TYPE_FILAMENT) ? AppConfig::SECTION_FILAMENTS : AppConfig::SECTION_MATERIALS;
    	if (app_config.has_section(section_name)) {
            // Check whether this profile is marked as "installed" in PrusaSlicer.ini,
    		// or whether a profile is marked as "installed", which this profile may have been renamed from.
	    	const std::map<std::string, std::string> &installed = app_config.get_section(section_name);
	    	auto has = [&installed](const std::string &name) {
	    		auto it = installed.find(name);
				return it != installed.end() && ! it->second.empty();
	    	};
	    	is_visible = has(this->name);
	    	for (auto it = this->renamed_from.begin(); ! is_visible && it != this->renamed_from.end(); ++ it)
	    		is_visible = has(*it);
	    }
    }
    //BBS: add config related log
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": name %1%, is_visible set to %2%")%name % is_visible;
}

std::string Preset::get_filament_type(std::string &display_filament_type)
{
    return config.get_filament_type(display_filament_type);
}

std::string Preset::get_printer_type(PresetBundle *preset_bundle)
{
    if (preset_bundle) {
        auto config = &preset_bundle->printers.get_edited_preset().config;
        std::string vendor_name;
        for (auto vendor_profile : preset_bundle->vendors) {
            for (auto vendor_model : vendor_profile.second.models)
                if (vendor_model.name == config->opt_string("printer_model"))
                {
                    vendor_name = vendor_profile.first;
                    return vendor_model.model_id;
                }
        }
    }
    return "";
}

std::string Preset::get_current_printer_type(PresetBundle *preset_bundle)
{
    if (preset_bundle) {
        auto config = &(this->config);
        std::string vendor_name;
        for (auto vendor_profile : preset_bundle->vendors) {
            for (auto vendor_model : vendor_profile.second.models)
                if (vendor_model.name == config->opt_string("printer_model")) {
                    vendor_name = vendor_profile.first;
                    return vendor_model.model_id;
                }
        }
    }
    return "";
}

void Preset::get_extruder_names_and_keysets(Type type, std::string& extruder_id_name, std::string& extruder_variant_name, std::set<std::string>** p_key_set1, std::set<std::string>** p_key_set2)
{
    if (type == Preset::TYPE_PRINT) {
        extruder_id_name = "print_extruder_id";
        extruder_variant_name = "print_extruder_variant";
        *p_key_set1 = &print_options_with_variant;
        *p_key_set2 = &empty_options;
    }
    else if (type == Preset::TYPE_PRINTER) {
        extruder_id_name = "printer_extruder_id";
        extruder_variant_name = "printer_extruder_variant";
        *p_key_set1 = &printer_options_with_variant_1;
        *p_key_set2 = &printer_options_with_variant_2;
    }
    else if (type == Preset::TYPE_FILAMENT) {
        extruder_variant_name = "filament_extruder_variant";
        *p_key_set1 = &filament_options_with_variant;
        *p_key_set2 = &empty_options;
    }
    else {
        *p_key_set1 = &empty_options;
        *p_key_set2 = &empty_options;
    }
}

bool Preset::has_lidar(PresetBundle *preset_bundle)
{
    bool has_lidar = false;
    if (preset_bundle) {
        auto config = &preset_bundle->printers.get_edited_preset().config;
        std::string vendor_name;
        for (auto vendor_profile : preset_bundle->vendors) {
            for (auto vendor_model : vendor_profile.second.models)
                if (vendor_model.name == config->opt_string("printer_model")) {
                    vendor_name = vendor_profile.first;
                    break;
                }
        }
        if (!vendor_name.empty())
            has_lidar = vendor_name.compare("BBL") == 0 ? true : false;
    }
    return has_lidar;
}

BedType Preset::get_default_bed_type(PresetBundle* preset_bundle)
{
    if (config.has("default_bed_type") && !config.opt_string("default_bed_type").empty()) {
        try {
            std::string str_bed_type = config.opt_string("default_bed_type");
            
            // Try parsing as integer first (legacy format)
            int bed_type_value = atoi(str_bed_type.c_str());
            if (bed_type_value > 0) {
                return BedType(bed_type_value);
            }
            else {
                BOOST_LOG_TRIVIAL(error) << "default_bed_type: invalid bed type: " << str_bed_type;
            }
            return BedType::btPEI;

        } catch(...) {
            ;
        }
    }

    std::string model_id = this->get_printer_type(preset_bundle);
    if (model_id == "BL-P001" || model_id == "BL-P002" || model_id == "C13") {
        return BedType::btPC;
    } else if (model_id == "C11") {
        return BedType::btPEI;
    }
    return BedType::btPEI;
}

bool Preset::has_cali_lines(PresetBundle* preset_bundle)
{
    std::string model_id = this->get_printer_type(preset_bundle);
    if (model_id == "BL-P001" || model_id == "BL-P002" || model_id == "C13") {
        return true;
    }
    return false;
}

static std::vector<std::string> s_Preset_print_options {
    "layer_height", "initial_layer_print_height", "wall_loops", "alternate_extra_wall", "slice_closing_radius", "spiral_mode", "spiral_mode_smooth", "spiral_mode_max_xy_smoothing", "spiral_starting_flow_ratio", "spiral_finishing_flow_ratio", "slicing_mode",
    "top_shell_layers", "top_shell_thickness", "top_surface_density", "bottom_surface_density", "bottom_shell_layers", "bottom_shell_thickness",
    "extra_perimeters_on_overhangs", "ensure_vertical_shell_thickness", "reduce_crossing_wall", "detect_thin_wall", "detect_overhang_wall", "overhang_reverse", "overhang_reverse_threshold","overhang_reverse_internal_only", "wall_direction",
    "seam_position", "staggered_inner_seams", "wall_sequence", "is_infill_first", "sparse_infill_density","fill_multiline", "sparse_infill_pattern", "lateral_lattice_angle_1", "lateral_lattice_angle_2", "infill_overhang_angle", "top_surface_pattern", "bottom_surface_pattern",
    "infill_direction", "solid_infill_direction", "counterbore_hole_bridging","infill_shift_step", "sparse_infill_rotate_template", "solid_infill_rotate_template", "symmetric_infill_y_axis","skeleton_infill_density", "infill_lock_depth", "skin_infill_depth", "skin_infill_density",
    "align_infill_direction_to_model", "extra_solid_infills",
    "minimum_sparse_infill_area", "reduce_infill_retraction","internal_solid_infill_pattern","gap_fill_target",
    "ironing_type", "ironing_pattern", "ironing_flow", "ironing_speed", "ironing_spacing", "ironing_angle", "ironing_angle_fixed", "ironing_inset",
    "support_ironing", "support_ironing_pattern", "support_ironing_flow", "support_ironing_spacing",
    "max_travel_detour_distance",
    "fuzzy_skin", "fuzzy_skin_thickness", "fuzzy_skin_point_distance", "fuzzy_skin_first_layer", "fuzzy_skin_noise_type", "fuzzy_skin_mode", "fuzzy_skin_scale", "fuzzy_skin_octaves", "fuzzy_skin_persistence",
    "max_volumetric_extrusion_rate_slope", "max_volumetric_extrusion_rate_slope_segment_length","extrusion_rate_smoothing_external_perimeter_only",
    "inner_wall_speed", "outer_wall_speed", "sparse_infill_speed", "internal_solid_infill_speed",
    "top_surface_speed", "support_speed", "support_object_xy_distance", "support_object_first_layer_gap", "support_interface_speed",
    "bridge_speed", "internal_bridge_speed", "gap_infill_speed", "travel_speed", "travel_speed_z", "initial_layer_speed",
    "outer_wall_acceleration", "initial_layer_acceleration", "top_surface_acceleration", "default_acceleration", "skirt_type", "skirt_loops", "skirt_speed","min_skirt_length", "skirt_distance", "skirt_start_angle", "skirt_height","single_loop_draft_shield", "draft_shield",
    "brim_width", "brim_object_gap", "brim_use_efc_outline", "brim_type", "brim_ears_max_angle", "brim_ears_detection_length", "enable_support", "support_type", "support_threshold_angle", "support_threshold_overlap","enforce_support_layers",
    "raft_layers", "raft_first_layer_density", "raft_first_layer_expansion", "raft_contact_distance", "raft_expansion",
    "support_base_pattern", "support_base_pattern_spacing", "support_expansion", "support_style",
    // BBS
    "print_extruder_id", "print_extruder_variant",
    "independent_support_layer_height",
    "support_angle", "support_interface_top_layers", "support_interface_bottom_layers",
    "support_interface_pattern", "support_interface_spacing", "support_interface_loop_pattern",
    "support_top_z_distance", "support_on_build_plate_only","support_critical_regions_only", "bridge_no_support", "thick_bridges", "thick_internal_bridges","dont_filter_internal_bridges","enable_extra_bridge_layer", "max_bridge_length", "print_sequence", "print_order", "support_remove_small_overhang",
    "filename_format", "wall_filament", "support_bottom_z_distance",
    "sparse_infill_filament", "solid_infill_filament", "support_filament", "support_interface_filament","support_interface_not_for_body",
    "ooze_prevention", "standby_temperature_delta", "preheat_time","preheat_steps", "interface_shells", "line_width", "initial_layer_line_width", "inner_wall_line_width",
    "outer_wall_line_width", "sparse_infill_line_width", "internal_solid_infill_line_width",
    "skin_infill_line_width","skeleton_infill_line_width",
    "top_surface_line_width", "support_line_width", "infill_wall_overlap","top_bottom_infill_wall_overlap", "bridge_flow", "internal_bridge_flow",
    "elefant_foot_compensation", "elefant_foot_compensation_layers", "xy_contour_compensation", "xy_hole_compensation", "resolution", "enable_prime_tower", "prime_tower_enable_framework",
    "prime_tower_width", "prime_tower_brim_width", "prime_tower_skip_points", "prime_volume",
    "prime_tower_infill_gap",
    "prime_tower_flat_ironing",
    "wipe_tower_no_sparse_layers", "compatible_printers", "compatible_printers_condition", "inherits",
    "flush_into_infill", "flush_into_objects", "flush_into_support",
     "tree_support_branch_angle", "tree_support_angle_slow", "tree_support_wall_count", "tree_support_top_rate", "tree_support_branch_distance", "tree_support_tip_diameter",
     "tree_support_branch_diameter", "tree_support_branch_diameter_angle",
     "detect_narrow_internal_solid_infill",
     "gcode_add_line_number", "enable_arc_fitting", "precise_z_height", "infill_combination","infill_combination_max_layer_height", /*"adaptive_layer_height",*/
     "support_bottom_interface_spacing", "enable_overhang_speed", "slowdown_for_curled_perimeters", "overhang_1_4_speed", "overhang_2_4_speed", "overhang_3_4_speed", "overhang_4_4_speed",
     "initial_layer_infill_speed", "only_one_wall_top", 
     "timelapse_type",
     "wall_generator", "wall_transition_length", "wall_transition_filter_deviation", "wall_transition_angle",
     "wall_distribution_count", "min_feature_size", "min_bead_width", "post_process", "min_length_factor",
     "small_perimeter_speed", "small_perimeter_threshold","bridge_angle","internal_bridge_angle", "filter_out_gap_fill", "travel_acceleration","inner_wall_acceleration", "min_width_top_surface",
     "default_jerk", "outer_wall_jerk", "inner_wall_jerk", "infill_jerk", "top_surface_jerk", "initial_layer_jerk","travel_jerk","default_junction_deviation",
     "top_solid_infill_flow_ratio","bottom_solid_infill_flow_ratio","only_one_wall_first_layer", "print_flow_ratio", "seam_gap",
     "set_other_flow_ratios", "first_layer_flow_ratio", "outer_wall_flow_ratio", "inner_wall_flow_ratio", "overhang_flow_ratio", "sparse_infill_flow_ratio", "internal_solid_infill_flow_ratio", "gap_fill_flow_ratio", "support_flow_ratio", "support_interface_flow_ratio", 
     "role_based_wipe_speed", "wipe_speed", "accel_to_decel_enable", "accel_to_decel_factor", "wipe_on_loops", "wipe_before_external_loop",
     "bridge_density","internal_bridge_density", "precise_outer_wall", "bridge_acceleration",
     "sparse_infill_acceleration", "internal_solid_infill_acceleration", "tree_support_auto_brim", 
     "tree_support_brim_width", "gcode_comments", "gcode_label_objects",
     "initial_layer_travel_speed", "exclude_object", "slow_down_layers", "infill_anchor", "infill_anchor_max","initial_layer_min_bead_width",
     "make_overhang_printable", "make_overhang_printable_angle", "make_overhang_printable_hole_size" ,"notes",
     "wipe_tower_cone_angle", "wipe_tower_extra_spacing","wipe_tower_max_purge_speed", 
     "wipe_tower_wall_type", "wipe_tower_extra_rib_length", "wipe_tower_rib_width", "wipe_tower_fillet_wall",
     "wipe_tower_filament", "wiping_volumes_extruders","wipe_tower_bridging", "wipe_tower_extra_flow","single_extruder_multi_material_priming",
     "wipe_tower_rotation_angle", "tree_support_branch_distance_organic", "tree_support_branch_diameter_organic", "tree_support_branch_angle_organic",
     "hole_to_polyhole", "hole_to_polyhole_threshold", "hole_to_polyhole_twisted", "mmu_segmented_region_max_width", "mmu_segmented_region_interlocking_depth",
     "small_area_infill_flow_compensation", "small_area_infill_flow_compensation_model",
     "enable_wrapping_detection",
     "seam_slope_type", "seam_slope_conditional", "scarf_angle_threshold", "scarf_joint_speed", "scarf_joint_flow_ratio", "seam_slope_start_height", "seam_slope_entire_loop", "seam_slope_min_length", "seam_slope_steps", "seam_slope_inner_walls", "scarf_overhang_threshold",
     "interlocking_beam", "interlocking_orientation", "interlocking_beam_layer_count", "interlocking_depth", "interlocking_boundary_avoidance", "interlocking_beam_width","calib_flowrate_topinfill_special_order",
};

static std::vector<std::string> s_Preset_filament_options {/*"filament_colour", */ "default_filament_colour", "required_nozzle_HRC", "filament_diameter", "pellet_flow_coefficient", "volumetric_speed_coefficients", "filament_type",
                                                          "filament_soluble", "filament_is_support", "filament_printable",
    "filament_max_volumetric_speed", "filament_adaptive_volumetric_speed",
    "filament_flow_ratio", "filament_density", "filament_adhesiveness_category", "filament_cost", "filament_minimal_purge_on_wipe_tower",
    "nozzle_temperature", "nozzle_temperature_initial_layer",
    // BBS
    "cool_plate_temp", "textured_cool_plate_temp", "eng_plate_temp", "hot_plate_temp", "textured_plate_temp", "cool_plate_temp_initial_layer", "textured_cool_plate_temp_initial_layer", "eng_plate_temp_initial_layer", "hot_plate_temp_initial_layer", "textured_plate_temp_initial_layer", "supertack_plate_temp_initial_layer", "supertack_plate_temp",
    // "bed_type",
    //BBS:temperature_vitrification
    "temperature_vitrification", "reduce_fan_stop_start_freq","dont_slow_down_outer_wall", "slow_down_for_layer_cooling", "fan_min_speed",
    "fan_max_speed", "enable_overhang_bridge_fan", "overhang_fan_speed", "overhang_fan_threshold", "close_fan_the_first_x_layers", "full_fan_speed_layer", "fan_cooling_layer_time", "slow_down_layer_time", "slow_down_min_speed",
    "filament_start_gcode", "filament_end_gcode",
    //exhaust fan control
    "activate_air_filtration","during_print_exhaust_fan_speed","complete_print_exhaust_fan_speed",
    // Retract overrides
    "filament_retraction_length", "filament_z_hop", "filament_z_hop_types", "filament_retract_lift_above", "filament_retract_lift_below", "filament_retract_lift_enforce", "filament_retraction_speed", "filament_deretraction_speed", "filament_retract_restart_extra", "filament_retraction_minimum_travel",
    "filament_retract_when_changing_layer", "filament_wipe", "filament_retract_before_wipe",
    // Profile compatibility
    "filament_vendor", "compatible_prints", "compatible_prints_condition", "compatible_printers", "compatible_printers_condition", "inherits",
    //BBS
    "filament_wipe_distance", "additional_cooling_fan_speed",
    "nozzle_temperature_range_low", "nozzle_temperature_range_high",
    "filament_extruder_variant",
    //SoftFever
    "enable_pressure_advance", "pressure_advance","adaptive_pressure_advance","adaptive_pressure_advance_model","adaptive_pressure_advance_overhangs", "adaptive_pressure_advance_bridges","chamber_temperature", "filament_shrink","filament_shrinkage_compensation_z", "support_material_interface_fan_speed","internal_bridge_fan_speed", "filament_notes" /*,"filament_seam_gap"*/,
    "ironing_fan_speed",
    // Filament ironing overrides
    "filament_ironing_flow", "filament_ironing_spacing", "filament_ironing_inset", "filament_ironing_speed",
    "filament_loading_speed", "filament_loading_speed_start",
    "filament_unloading_speed", "filament_unloading_speed_start", "filament_toolchange_delay", "filament_cooling_moves", "filament_stamping_loading_speed", "filament_stamping_distance",
    "filament_cooling_initial_speed", "filament_cooling_final_speed", "filament_ramming_parameters",
    "filament_multitool_ramming", "filament_multitool_ramming_volume", "filament_multitool_ramming_flow", "activate_chamber_temp_control",
    "filament_long_retractions_when_cut","filament_retraction_distances_when_cut", "idle_temperature",
    //BBS filament change length while the extruder color
    "filament_change_length","filament_flush_volumetric_speed","filament_flush_temp",
    "long_retractions_when_ec", "retraction_distances_when_ec"
    };

static std::vector<std::string> s_Preset_machine_limits_options {
    "machine_max_acceleration_extruding", "machine_max_acceleration_retracting", "machine_max_acceleration_travel",
    "machine_max_acceleration_x", "machine_max_acceleration_y", "machine_max_acceleration_z", "machine_max_acceleration_e",
    "machine_max_speed_x", "machine_max_speed_y", "machine_max_speed_z", "machine_max_speed_e",
    "machine_min_extruding_rate", "machine_min_travel_rate",
    "machine_max_jerk_x", "machine_max_jerk_y", "machine_max_jerk_z", "machine_max_jerk_e",
    "machine_max_junction_deviation",
    //resonance avoidance ported from qidi slicer
    "resonance_avoidance", "min_resonance_avoidance_speed", "max_resonance_avoidance_speed",
};

static std::vector<std::string> s_Preset_printer_options {
    "printer_technology",
    "printable_area", "extruder_printable_area", "bed_exclude_area","bed_custom_texture", "bed_custom_model", "gcode_flavor",
    "fan_kickstart", "fan_speedup_time", "fan_speedup_overhangs",
    "single_extruder_multi_material", "manual_filament_change", "machine_start_gcode", "machine_end_gcode", "before_layer_change_gcode", "printing_by_object_gcode", "layer_change_gcode", "time_lapse_gcode", "wrapping_detection_gcode", "change_filament_gcode", "change_extrusion_role_gcode",
    "printer_model", "printer_variant", "printer_extruder_id", "printer_extruder_variant", "extruder_variant_list", "default_nozzle_volume_type",
    "printable_height", "extruder_printable_height", "extruder_clearance_radius", "extruder_clearance_height_to_lid", "extruder_clearance_height_to_rod",
    "nozzle_height", "master_extruder_id",
    "default_print_profile", "inherits",
    "silent_mode",
    "scan_first_layer", "enable_power_loss_recovery", "wrapping_detection_layers", "wrapping_exclude_area", "machine_load_filament_time", "machine_unload_filament_time", "machine_tool_change_time", "time_cost", "machine_pause_gcode", "template_custom_gcode",
    "nozzle_type", "nozzle_hrc","auxiliary_fan", "nozzle_volume","upward_compatible_machine", "z_hop_types", "travel_slope", "retract_lift_enforce","support_chamber_temp_control","support_air_filtration","printer_structure",
    "best_object_pos", "head_wrap_detect_zone",
    "host_type", "print_host", "printhost_apikey", "bbl_use_printhost",
    "print_host_webui",
    "printhost_cafile","printhost_port","printhost_authorization_type",
    "printhost_user", "printhost_password", "printhost_ssl_ignore_revoke", "thumbnails", "thumbnails_format",
    "use_relative_e_distances", "extruder_type", "use_firmware_retraction", "printer_notes",
    "grab_length", "support_object_skip_flush", "physical_extruder_map",
    "cooling_tube_retraction",
    "cooling_tube_length", "high_current_on_filament_swap", "parking_pos_retraction", "extra_loading_move", "purge_in_prime_tower", "enable_filament_ramming",
    "z_offset",
    "disable_m73", "preferred_orientation", "emit_machine_limits_to_gcode", "pellet_modded_printer", "support_multi_bed_types", "default_bed_type", "bed_mesh_min","bed_mesh_max","bed_mesh_probe_distance", "adaptive_bed_mesh_margin", "enable_long_retraction_when_cut","long_retractions_when_cut","retraction_distances_when_cut",
    "bed_temperature_formula", "nozzle_flush_dataset"
    };

static std::vector<std::string> s_Preset_sla_print_options {
    "layer_height",
    "faded_layers",
    "supports_enable",
    "support_head_front_diameter",
    "support_head_penetration",
    "support_head_width",
    "support_pillar_diameter",
    "support_small_pillar_diameter_percent",
    "support_max_bridges_on_pillar",
    "support_pillar_connection_mode",
    "support_buildplate_only",
    "support_pillar_widening_factor",
    "support_base_diameter",
    "support_base_height",
    "support_base_safety_distance",
    "support_critical_angle",
    "support_max_bridge_length",
    "support_max_pillar_link_distance",
    "support_object_elevation",
    "support_points_density_relative",
    "support_points_minimal_distance",
    "slice_closing_radius",
    "pad_enable",
    "pad_wall_thickness",
    "pad_wall_height",
    "pad_brim_size",
    "pad_max_merge_distance",
    // "pad_edge_radius",
    "pad_wall_slope",
    "pad_object_gap",
    "pad_around_object",
    "pad_around_object_everywhere",
    "pad_object_connector_stride",
    "pad_object_connector_width",
    "pad_object_connector_penetration",
    "hollowing_enable",
    "hollowing_min_thickness",
    "hollowing_quality",
    "hollowing_closing_distance",
    "filename_format",
    "default_sla_print_profile",
    "compatible_printers",
    "compatible_printers_condition",
    "inherits"
};

static std::vector<std::string> s_Preset_sla_material_options {
    "material_colour",
    "material_type",
    "initial_layer_height",
    "bottle_cost",
    "bottle_volume",
    "bottle_weight",
    "material_density",
    "exposure_time",
    "initial_exposure_time",
    "material_correction",
    "material_correction_x",
    "material_correction_y",
    "material_correction_z",
    "material_vendor",
    "material_print_speed",
    "default_sla_material_profile",
    "compatible_prints", "compatible_prints_condition",
    "compatible_printers", "compatible_printers_condition", "inherits"
};

static std::vector<std::string> s_Preset_sla_printer_options {
    "printer_technology",
    "printable_area","bed_custom_texture", "bed_custom_model", "printable_height",
    "display_width", "display_height", "display_pixels_x", "display_pixels_y",
    "display_mirror_x", "display_mirror_y",
    "display_orientation",
    "fast_tilt_time", "slow_tilt_time", "area_fill",
    "relative_correction",
    "relative_correction_x",
    "relative_correction_y",
    "relative_correction_z",
    "absolute_correction",
    "elefant_foot_compensation",
    "elefant_foot_min_width",
    "gamma_correction",
    "min_exposure_time", "max_exposure_time",
    "min_initial_exposure_time", "max_initial_exposure_time",
    "inherits"
};

const std::vector<std::string>& Preset::print_options()          { return s_Preset_print_options; }
const std::vector<std::string>& Preset::filament_options()       { return s_Preset_filament_options; }
const std::vector<std::string>& Preset::machine_limits_options() { return s_Preset_machine_limits_options; }
// The following nozzle options of a printer profile will be adjusted to match the size
// of the nozzle_diameter vector.
const std::vector<std::string>& Preset::nozzle_options()         { return print_config_def.extruder_option_keys(); }
const std::vector<std::string>& Preset::sla_print_options()      { return s_Preset_sla_print_options; }
const std::vector<std::string>& Preset::sla_material_options()   { return s_Preset_sla_material_options; }
const std::vector<std::string>& Preset::sla_printer_options()    { return s_Preset_sla_printer_options; }

const std::vector<std::string>& Preset::printer_options()
{
    static std::vector<std::string> s_opts = [](){
        std::vector<std::string> opts = s_Preset_printer_options;
        append(opts, s_Preset_machine_limits_options);
        append(opts, Preset::nozzle_options());
        return opts;
    }();
    return s_opts;
}

PresetCollection::PresetCollection(Preset::Type type, const std::vector<std::string> &keys, const Slic3r::StaticPrintConfig &defaults, const std::string &default_name) :
    m_type(type),
    m_edited_preset(type, "", false),
    m_saved_preset(type, "", false),
    m_idx_selected(0)
{
    // Insert just the default preset.
    this->add_default_preset(keys, defaults, default_name);
    m_edited_preset.config.apply(m_presets.front().config);
    update_saved_preset_from_current_preset();
}

 //BBS: add operator= implemention
PresetCollection& PresetCollection::operator=(const PresetCollection &rhs)
{
    m_type = rhs.m_type;
    m_presets = rhs.m_presets;
    m_map_alias_to_profile_name = rhs.m_map_alias_to_profile_name;
    m_map_system_profile_renamed = rhs.m_map_system_profile_renamed;
    m_edited_preset = rhs.m_edited_preset;
    m_saved_preset = rhs.m_saved_preset;
    m_idx_selected = rhs.m_idx_selected;
    m_default_suppressed = rhs.m_default_suppressed;
    m_num_default_presets = rhs.m_num_default_presets;
    m_dir_path = rhs.m_dir_path;

    return *this;
}

void PresetCollection::reset(bool delete_files)
{
    //BBS: add lock logic for sync preset in background
    lock();
    if (m_presets.size() > m_num_default_presets) {
        if (delete_files) {
            // Erase the preset files.
            for (Preset &preset : m_presets)
                if (! preset.is_default && ! preset.is_external && ! preset.is_system) {
                    //BBS remove idx and ini files
                    preset.remove_files();
                }
        }
        // Don't use m_presets.resize() here as it requires a default constructor for Preset.
        m_presets.erase(m_presets.begin() + m_num_default_presets, m_presets.end());
        this->select_preset(0);
    }
    //BBS: add lock logic for sync preset in background
    unlock();
    m_map_alias_to_profile_name.clear();
    m_map_system_profile_renamed.clear();
}

void PresetCollection::add_default_preset(const std::vector<std::string> &keys, const Slic3r::StaticPrintConfig &defaults, const std::string &preset_name)
{
    // Insert just the default preset.
    m_presets.emplace_back(Preset(this->type(), preset_name, true));
    m_presets.back().config.apply_only(defaults, keys.empty() ? defaults.keys() : keys);
    m_presets.back().loaded = true;
    ++ m_num_default_presets;
}

// Load all presets found in dir_path.
// Throws an exception on error.
void PresetCollection::load_presets(
    const std::string &dir_path, const std::string &subdir,
    PresetsConfigSubstitutions& substitutions, ForwardCompatibilitySubstitutionRule substitution_rule)
{
    // Don't use boost::filesystem::canonical() on Windows, it is broken in regard to reparse points,
    // see https://github.com/prusa3d/PrusaSlicer/issues/732
    boost::filesystem::path dir = boost::filesystem::absolute(boost::filesystem::path(dir_path) / subdir).make_preferred();

    // Load custom roots first
    if (fs::exists(dir / "base")) {
        load_presets(dir.string(), "base", substitutions, substitution_rule);
    }

    //BBS: add config related logs
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" enter, load presets from %1%, current type %2%")%dir %Preset::get_type_string(m_type);
    //BBS do not parse folder if not exists
    m_dir_path = dir.string();
    if (!fs::exists(dir)) {
        fs::create_directory(dir);
        return;
    }

    std::string errors_cummulative;
    // Store the loaded presets into a new vector, otherwise the binary search for already existing presets would be broken.
    // (see the "Preset already present, not loading" message).
    std::deque<Preset> presets_loaded;

    //BBS: get the extruder related info for this preset collection
    std::string extruder_id_name, extruder_variant_name;
    std::set<std::string> *key_set1 = nullptr, *key_set2 = nullptr;
    Preset::get_extruder_names_and_keysets(m_type, extruder_id_name, extruder_variant_name, &key_set1, &key_set2);

    //BBS: change to json format
    for (auto &dir_entry : boost::filesystem::directory_iterator(dir))
    {
        std::string file_name = dir_entry.path().filename().string();
        //if (Slic3r::is_ini_file(dir_entry)) {
        if (Slic3r::is_json_file(file_name)) {
            // Remove the .ini suffix.
            std::string name = file_name.erase(file_name.size() - 5);
            if (this->find_preset(name, false)) {
                // This happens when there's is a preset (most likely legacy one) with the same name as a system preset
                // that's already been loaded from a bundle.
                BOOST_LOG_TRIVIAL(warning) << "Preset already present, not loading: " << name;
                continue;
            }
            try {
                Preset preset(m_type, name, false);
                preset.file = dir_entry.path().string();
                // Load the preset file, apply preset values on top of defaults.
                try {
                    fs::path idx_path(preset.file);
                    idx_path.replace_extension(".info");
                    if (fs::exists(idx_path)) {
                        preset.load_info(idx_path.string());
                    }
                    DynamicPrintConfig config;
                    //BBS: change to json format
                    //ConfigSubstitutions config_substitutions = config.load_from_ini(preset.file, substitution_rule);
                    std::map<std::string, std::string> key_values;
                    std::string reason;
                    ConfigSubstitutions config_substitutions = config.load_from_json(preset.file, substitution_rule, key_values, reason);
                    if (! config_substitutions.empty())
                        substitutions.push_back({ preset.name, m_type, PresetConfigSubstitutions::Source::UserFile, preset.file, std::move(config_substitutions) });
                    if (!reason.empty()) {
                        fs::path file_path(preset.file);
                        if (fs::exists(file_path))
                            fs::remove(file_path);
                        file_path.replace_extension(".info");
                        if (fs::exists(file_path))
                            fs::remove(file_path);
                        BOOST_LOG_TRIVIAL(error) << boost::format("parse config %1% failed")%preset.file;
                        ++m_errors;
                        continue;
                    }

                    std::string version_str = key_values[BBL_JSON_KEY_VERSION];
                    boost::optional<Semver> version = Semver::parse(version_str);
                    if (!version) continue;
                    preset.version = *version;

                    if (key_values.find(BBL_JSON_KEY_FILAMENT_ID) != key_values.end())
                        preset.filament_id = key_values[BBL_JSON_KEY_FILAMENT_ID];
                    if (key_values.find(BBL_JSON_KEY_DESCRIPTION) != key_values.end())
                        preset.description = key_values[BBL_JSON_KEY_DESCRIPTION];
                    if (key_values.find(BBL_JSON_KEY_INSTANTIATION) != key_values.end())
                        preset.is_visible = key_values[BBL_JSON_KEY_INSTANTIATION] != "false";

                    //Orca: find and use the inherit config as the base
                    Preset* inherit_preset = nullptr;
                    ConfigOption* inherits_config = config.option(BBL_JSON_KEY_INHERITS);

                    // check inherits_config
                    if (inherits_config) {
                        ConfigOptionString * option_str = dynamic_cast<ConfigOptionString *> (inherits_config);
                        std::string inherits_value = option_str->value;
                        // Orca: try to find if the parent preset has been renamed
                        inherit_preset = this->find_preset2(inherits_value);

                    } else {
                        ;
                    }
                    const Preset& default_preset = this->default_preset_for(config);
                    if (inherit_preset) {
                        preset.config = inherit_preset->config;
                        preset.filament_id = inherit_preset->filament_id;
                        extend_default_config_length(config, false, {});
                        preset.config.update_diff_values_to_child_config(config, extruder_id_name, extruder_variant_name, *key_set1, *key_set2);
                    }
                    else {
                        auto inherits_config2 = dynamic_cast<ConfigOptionString *>(inherits_config);
                        if ((inherits_config2 && !inherits_config2->value.empty())) {
                            BOOST_LOG_TRIVIAL(error) << boost::format("can not find parent %1% for config %2%!")%inherits_config2->value %preset.file;
                            ++m_errors;
                            continue;
                        }
                        // We support custom root preset now
                        // Find a default preset for the config. The PrintPresetCollection provides different default preset based on the "printer_technology" field.
                        preset.config = default_preset.config;
                        preset.config.apply(std::move(config));
                        extend_default_config_length(preset.config, true, default_preset.config);
                    }
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " load preset: " << name << " and filament_id: " << preset.filament_id << " and base_id: " << preset.base_id;

                    Preset::normalize(preset.config);
                    // Report configuration fields, which are misplaced into a wrong group.
                    std::string incorrect_keys = Preset::remove_invalid_keys(preset.config, default_preset.config);
                    if (!incorrect_keys.empty()) {
                        ++m_errors;
                        BOOST_LOG_TRIVIAL(error)
                            << "Error in a preset file: The preset \"" << preset.file
                            << "\" contains the following incorrect keys: " << incorrect_keys << ", which were removed";
                    }

                    if (preset.type == Preset::TYPE_FILAMENT && preset.is_user() && preset.inherits().empty()) {
                        auto compatible_printers = dynamic_cast<ConfigOptionStrings *>(preset.config.option("compatible_printers", true));
                        if (compatible_printers && compatible_printers->values.empty()) {
                            size_t at_pos = name.find('@');
                            if (at_pos != std::string::npos && at_pos + 1 < name.length()) {
                                compatible_printers->values.push_back(name.substr(at_pos + 1));
                                preset.save(nullptr);
                                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " added compatible_printers for preset: " << name;
                            }
                        }
                    }

                    preset.loaded = true;
                    //BBS: add some workaround for previous incorrect settings
                    if ((!preset.setting_id.empty())&&(preset.setting_id == preset.base_id))
                        preset.setting_id.clear();
                    //BBS: add config related logs
                    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(", preset type %1%, name %2%, path %3%, is_system %4%, is_default %5% is_visible %6%")%Preset::get_type_string(m_type) %preset.name %preset.file %preset.is_system %preset.is_default %preset.is_visible;
                    // add alias for custom filament preset
                    set_custom_preset_alias(preset);
                } catch (const std::ifstream::failure &err) {
                    ++m_errors;
                    BOOST_LOG_TRIVIAL(error) << boost::format("The user-config cannot be loaded: %1%. Reason: %2%")%preset.file %err.what();
                    fs::path file_path(preset.file);
                    if (fs::exists(file_path))
                        fs::remove(file_path);
                    file_path.replace_extension(".info");
                    if (fs::exists(file_path))
                        fs::remove(file_path);
                    //throw Slic3r::RuntimeError(std::string("The selected preset cannot be loaded: ") + preset.file + "\n\tReason: " + err.what());
                } catch (const std::runtime_error &err) {
                    ++m_errors;
                    BOOST_LOG_TRIVIAL(error) << boost::format("Failed loading the user-config file: %1%. Reason: %2%")%preset.file %err.what();
                    //throw Slic3r::RuntimeError(std::string("Failed loading the preset file: ") + preset.file + "\n\tReason: " + err.what());
                    fs::path file_path(preset.file);
                    if (fs::exists(file_path))
                        fs::remove(file_path);
                    file_path.replace_extension(".info");
                    if (fs::exists(file_path))
                        fs::remove(file_path);
                }

                presets_loaded.emplace_back(preset);
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << __LINE__ << " load config successful and preset name is:" << preset.name;
            } catch (const std::runtime_error &err) {
                errors_cummulative += err.what();
                errors_cummulative += "\n";
            }
        }
    }
    if (presets_loaded.size() > 0)
        m_presets.insert(m_presets.end(), std::make_move_iterator(presets_loaded.begin()), std::make_move_iterator(presets_loaded.end()));
    std::sort(m_presets.begin() + m_num_default_presets, m_presets.end());
    //BBS: add config related logs
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": loaded %1% presets from %2%, type %3%")%presets_loaded.size() %dir %Preset::get_type_string(m_type);
    //this->select_preset(first_visible_idx());
    if (! errors_cummulative.empty())
        throw Slic3r::RuntimeError(errors_cummulative);
}

//BBS: add function to generate differed preset for save
//the pointer should be freed by the caller
Preset* PresetCollection::get_preset_differed_for_save(Preset& preset)
{
    if (preset.is_system || preset.is_default)
        return nullptr;

    Preset* new_preset = nullptr;
    //BBS: only save difference for user preset
    std::string& inherits = preset.inherits();
    Preset* parent_preset = nullptr;
    if (!inherits.empty()) {
        parent_preset = this->find_preset(inherits, false, true);
    }
    if (parent_preset) {
        new_preset = new Preset();
        *new_preset = preset;

        DynamicPrintConfig temp_config;
        std::vector<std::string> dirty_options = preset.config.diff(parent_preset->config);

        std::string extruder_id_name, extruder_variant_name;
        std::set<std::string> *key_set1 = nullptr, *key_set2 = nullptr;
        Preset::get_extruder_names_and_keysets(m_type, extruder_id_name, extruder_variant_name, &key_set1, &key_set2);

        if (!extruder_id_name.empty()) {
            dirty_options.emplace_back(extruder_id_name);
        }
        if (!extruder_variant_name.empty()) {
            dirty_options.emplace_back(extruder_variant_name);
        }

        for (auto option: dirty_options)
        {
            ConfigOption *opt_src = preset.config.option(option);
            ConfigOption *opt_dst = temp_config.option(option, true);
            if (opt_dst->is_scalar() || !(opt_dst->nullable()))
                opt_dst->set(opt_src);
            else {
                ConfigOptionVectorBase* opt_vec_src = static_cast<ConfigOptionVectorBase*>(opt_src);
                ConfigOptionVectorBase* opt_vec_dst = static_cast<ConfigOptionVectorBase*>(opt_dst);
                ConfigOptionVectorBase* opt_vec_inherit = static_cast<ConfigOptionVectorBase*>(parent_preset->config.option(option));
                if (opt_vec_src->size() == 1)
                    opt_dst->set(opt_src);
                else if (key_set1->find(option) != key_set1->end()) {
                    opt_vec_dst->set_with_nil(opt_vec_src, opt_vec_inherit, 1);
                }
                else if (key_set2->find(option) != key_set2->end()) {
                    opt_vec_dst->set_with_nil(opt_vec_src, opt_vec_inherit, 2);
                }
                else
                    opt_dst->set(opt_src);
            }
        }

        new_preset->config = temp_config;
    }

    return new_preset;
}

//BBS:get the differencen values to update
int PresetCollection::get_differed_values_to_update(Preset& preset, std::map<std::string, std::string>& key_values)
{
    if (preset.is_system || preset.is_default || preset.is_project_embedded) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" Error: not a user preset! Should not happen, name %1%") %preset.name;
        ++m_errors;
        return -1;
    }

    //BBS: only save difference for user preset
    std::string& inherit_preset = preset.inherits();
    Preset* parent_preset = nullptr;
    if (!inherit_preset.empty()) {
        parent_preset = this->find_preset(inherit_preset, false, true);
    }
    if (parent_preset) {
        DynamicPrintConfig temp_config;
        std::vector<std::string> dirty_options = preset.config.diff(parent_preset->config);

        for (auto option: dirty_options)
        {
            ConfigOption *opt_src = preset.config.option(option);
            if (opt_src)
                key_values[option] = opt_src->serialize();
        }
    }
    else {
        for (auto iter = preset.config.cbegin(); iter != preset.config.cend(); ++iter)
        {
            key_values[iter->first] = iter->second->serialize();
        }
    }

    //add other values
    key_values[BBL_JSON_KEY_VERSION] = preset.version.to_string();
    if (!preset.base_id.empty()) {
        key_values[BBL_JSON_KEY_BASE_ID] = preset.base_id;
    } else {
        key_values.erase(BBL_JSON_KEY_BASE_ID);
        if (get_preset_base(preset) == &preset && !preset.filament_id.empty()) {
            key_values[BBL_JSON_KEY_FILAMENT_ID] = preset.filament_id;
        }
    }
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " uploading user preset name is: " << preset.name << "and create filament_id is: " << preset.filament_id
                            << " and base_id is: " << preset.base_id;
    key_values[BBL_JSON_KEY_UPDATE_TIME] = std::to_string(preset.updated_time);
    key_values[BBL_JSON_KEY_TYPE] = Preset::get_iot_type_string(preset.type);
    return 0;
}

//BBS: save user presets to local
void PresetCollection::load_project_embedded_presets(std::vector<Preset*>& project_presets, const std::string& type, PresetsConfigSubstitutions& substitutions, ForwardCompatibilitySubstitutionRule rule)
{
    std::string errors_cummulative;
    // Store the loaded presets into a new vector, otherwise the binary search for already existing presets would be broken.
    // (see the "Preset already present, not loading" message).
    std::deque<Preset> presets_loaded;
    std::vector<Preset*>::iterator it;

    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" enter, type %1% , total preset counts %2%")%Preset::get_type_string(m_type) %project_presets.size();
    std::string extruder_id_name, extruder_variant_name;
    std::set<std::string> *key_set1 = nullptr, *key_set2 = nullptr;
    Preset::get_extruder_names_and_keysets(m_type, extruder_id_name, extruder_variant_name, &key_set1, &key_set2);

    lock();
    for (it = project_presets.begin(); it != project_presets.end(); it++) {
        Preset* preset = *it;
        if (preset->type != Preset::get_type_from_string(type)) continue;
        if (!preset->is_project_embedded) continue;
        std::string name = preset->name;
        if (this->find_preset(name, false)) {
            BOOST_LOG_TRIVIAL(warning) << "Preset already present, not loading: " << name;
            continue;
        }
        try {
            DynamicPrintConfig config = preset->config;
            if (preset->loading_substitutions && ! preset->loading_substitutions->empty()) {
                substitutions.push_back({ preset->name, m_type, PresetConfigSubstitutions::Source::ProjectFile, preset->name, std::move(*(preset->loading_substitutions))});
                free(preset->loading_substitutions);
                preset->loading_substitutions = NULL;
            }
            //BBS: use inherit config as the base
            Preset* inherit_preset = nullptr;
            ConfigOption* inherits_config = config.option(BBL_JSON_KEY_INHERITS);
            if (inherits_config) {
                ConfigOptionString * option_str = dynamic_cast<ConfigOptionString *> (inherits_config);
                std::string inherits_value = option_str->value;
                /*size_t pos = inherits_value.find_first_of('*');
                if (pos != std::string::npos) {
                    inherits_value.replace(pos, 1, 1, '~');
                    option_str->value = inherits_value;
                }*/
                inherit_preset = this->find_preset(inherits_value, false, true);
            }
            const Preset& default_preset = this->default_preset_for(config);
            if (inherit_preset) {
                preset->config = inherit_preset->config;
                preset->filament_id = inherit_preset->filament_id;
            }
            else {
                // Find a default preset for the config. The PrintPresetCollection provides different default preset based on the "printer_technology" field.
                //BBS 202407: don't load project embedded preset when can not find inherit
                //preset->config = default_preset.config;
                BOOST_LOG_TRIVIAL(error) << boost::format("can not find parent for config %1%!")%preset->file;
                continue;
            }
            preset->config.update_diff_values_to_child_config(config, extruder_id_name, extruder_variant_name, *key_set1, *key_set2);
            //preset->config.apply(std::move(config));
            Preset::normalize(preset->config);
            // Report configuration fields, which are misplaced into a wrong group.
            std::string incorrect_keys = Preset::remove_invalid_keys(preset->config, default_preset.config);
            if (!incorrect_keys.empty()) {
                ++m_errors;
                BOOST_LOG_TRIVIAL(error) << "Error in a preset file: The preset \"" << preset->name
                                         << "\" contains the following incorrect keys: " << incorrect_keys << ", which were removed";
            }
            preset->loaded = true;
            presets_loaded.emplace_back(*preset);
            BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(", %1% got preset, name %2%, path %3%, is_system %4%, is_default %5% is_visible %6%")%Preset::get_type_string(m_type) %preset->name %preset->file %preset->is_system %preset->is_default %preset->is_visible;
        } catch (const std::runtime_error &err) {
            errors_cummulative += err.what();
            errors_cummulative += "\n";
        }
    }

    m_presets.insert(m_presets.end(), std::make_move_iterator(presets_loaded.begin()), std::make_move_iterator(presets_loaded.end()));
    std::sort(m_presets.begin() + m_num_default_presets, m_presets.end());
    //don't select it here
    //this->select_preset(first_visible_idx());
    unlock();

    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" finished, %1% got %2% presets, errors_cummulative %3%")%Preset::get_type_string(m_type) %presets_loaded.size() %errors_cummulative;
    if (! errors_cummulative.empty())
        throw Slic3r::RuntimeError(errors_cummulative);
}

//BBS: get project embedded presets from
std::vector<Preset*> PresetCollection::get_project_embedded_presets()
{
    std::vector<Preset*> project_presets;

    lock();
    for (Preset &preset : m_presets) {
        //if (preset.type != Preset::get_type_from_string(type)) continue;
        if (!preset.is_project_embedded) continue;

        Preset* new_preset = get_preset_differed_for_save(preset);

        if (new_preset)
            project_presets.push_back(new_preset);
    }
    unlock();
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" enter, type %1% , total preset counts %2%")%Preset::get_type_string(m_type) %project_presets.size();
    return project_presets;
}

//BBS: reset project embedded presets
bool PresetCollection::reset_project_embedded_presets()
{
    std::deque<Preset>::iterator it = m_presets.begin();
    bool re_select = false;
    int count = -1;

    lock();
    while ( it!=m_presets.end() )
    {
        count++;
        //if (preset.type != Preset::get_type_from_string(type)) continue;
        if (it->is_project_embedded) {
            BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" type %1% , delete preset %2%")%Preset::get_type_string(m_type) % it->name;
            if ((!re_select) && (m_idx_selected == count))
                re_select = true;
            if (m_idx_selected > count) {
                m_idx_selected--;
                count--;
            }
            it = m_presets.erase(it);
        }
        else
            it++;
    }

    if (re_select)
        m_idx_selected = -1;

    unlock();

    return re_select;
}

void PresetCollection::set_sync_info_and_save(std::string name, std::string setting_id, std::string syncinfo, long long update_time)
{
    lock();
    for (auto it = m_presets.begin(); it != m_presets.end(); it++) {
        Preset* preset = &m_presets[it - m_presets.begin()];
        if (preset->name == name) {
            if (syncinfo.empty())
                preset->sync_info.clear();
            else
                preset->sync_info = syncinfo;
            if (get_preset_base(*preset) == preset) {
                for (auto & preset2 : m_presets)
                    if (preset2.inherits() == preset->name) {
                        preset2.base_id = setting_id;
                        preset2.save_info();
                    }
            }
            preset->setting_id = setting_id;
            if (update_time > 0)
                preset->updated_time = update_time;
            preset->sync_info == "update" ? preset->save(nullptr) : preset->save_info();
            break;
        }
    }
    unlock();
}

bool PresetCollection::need_sync(std::string name, std::string setting_id, long long update_time)
{
    lock();
    auto preset = find_preset(name, false, true);
    bool need   = preset == nullptr || preset->setting_id != setting_id || preset->updated_time < update_time;
    unlock();
    return need;
}

//BBS: get user presets
int PresetCollection::get_user_presets(PresetBundle *preset_bundle, std::vector<Preset> &result_presets)
{
    int count = 0;
    result_presets.clear();

    lock();
    for (Preset &preset : m_presets) {
        if (!preset.is_user()) continue;
        if (preset.base_id.empty() && preset.inherits() != "") continue;
        if (!preset.setting_id.empty() && preset.sync_info.empty()) continue;
        //if (!preset.is_bbl_vendor_preset(preset_bundle)) continue;
        if (preset.sync_info == "hold") continue;

        result_presets.push_back(preset);
        count++;
    }
    unlock();

    return count;
}

//BBS: update user presets directory
void PresetCollection::update_user_presets_directory(const std::string& dir_path, const std::string& type)
{
    boost::filesystem::path dir = boost::filesystem::absolute(boost::filesystem::path(dir_path) / type).make_preferred();

    if (!fs::exists(dir))
        fs::create_directory(dir);

    m_dir_path = dir.string();
}

//BBS: save user presets to local
void PresetCollection::save_user_presets(const std::string& dir_path, const std::string& type, std::vector<std::string>& need_to_delete_list)
{
    boost::filesystem::path dir = boost::filesystem::absolute(boost::filesystem::path(dir_path) / type).make_preferred();

    if (!fs::exists(dir))
        fs::create_directory(dir);

    m_dir_path = dir.string();

    std::vector<std::string> delete_name_list;
    //std::map<std::string, Preset*>::iterator it;
    //for (it = my_presets.begin(); it != my_presets.end(); it++) {
    for (auto it = m_presets.begin(); it != m_presets.end(); it++) {
        Preset* preset = &m_presets[it - m_presets.begin()];
        if (!preset->is_user()) continue;
        if (preset->sync_info != "save") continue;
        preset->sync_info.clear();
        preset->file = path_for_preset(*preset);

        //BBS: only save difference for user preset
        std::string inherits = Preset::inherits(preset->config);
        if (inherits.empty()) {
            // We support custom root preset now
            //BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" can not find inherits for %1% , should not happen")%preset->name;
            //// BBS add sync info
            //preset->sync_info = "delete";
            //need_to_delete_list.push_back(preset->setting_id);
            //delete_name_list.push_back(preset->name);
            preset->save(nullptr);
            continue;
        }
        Preset* parent_preset = this->find_preset(inherits, false, true);
        if (!parent_preset) {
            ++m_errors;
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" can not find parent preset for %1% , inherits %2%")%preset->name %inherits;
            continue;
        }

        if (preset->base_id.empty())
            preset->base_id = parent_preset->setting_id;
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " " << preset->name << " filament_id: " << preset->filament_id << " base_id: " << preset->base_id;
        preset->save(&(parent_preset->config));
    }

    for (auto delete_name: delete_name_list)
    {
        this->delete_preset(delete_name);
    }
    delete_name_list.clear();

    return;
}

//BBS: load one user preset from key-values
bool PresetCollection::load_user_preset(std::string name, std::map<std::string, std::string> preset_values, PresetsConfigSubstitutions& substitutions, ForwardCompatibilitySubstitutionRule rule)
{
    std::string errors_cummulative;
    // Store the loaded presets into a new vector, otherwise the binary search for already existing presets would be broken.
    // (see the "Preset already present, not loading" message).
    //std::deque<Preset> presets_loaded;
    int count = 0;

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" enter, name %1% , total value counts %2%")%name %preset_values.size();

    //if the version is not matching, skip it
    if (preset_values.find(BBL_JSON_KEY_VERSION) == preset_values.end()) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format("can not find version, not loading for user preset %1%")%name;
        return false;
    }
    std::string version_str = preset_values[BBL_JSON_KEY_VERSION];
    boost::optional<Semver> cloud_version = Semver::parse(version_str);
    if (!cloud_version) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format("invalid version %1%, not loading for user preset %2%")%version_str %name;
        return false;
    }

    //setting_id
    if (preset_values.find(BBL_JSON_KEY_SETTING_ID) == preset_values.end()) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format("can not find setting_id, not loading for user preset %1%")%name;
        return false;
    }
    std::string cloud_setting_id = preset_values[BBL_JSON_KEY_SETTING_ID];

    //update_time
    long long cloud_update_time = 0;
    if (preset_values.find(BBL_JSON_KEY_UPDATE_TIME) != preset_values.end()) {
        cloud_update_time = std::atoll(preset_values[BBL_JSON_KEY_UPDATE_TIME].c_str());
    }

    //user_id
    if (preset_values.find(BBL_JSON_KEY_USER_ID) == preset_values.end()) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format("can not find user_id, not loading for user preset %1%")%name;
        return false;
    }
    std::string cloud_user_id = preset_values[BBL_JSON_KEY_USER_ID];

    lock();
    //std::string name = preset->name;
    auto iter = this->find_preset_internal(name);
    bool need_update = false;
    if ((iter != m_presets.end()) && (iter->name == name)) {
        BOOST_LOG_TRIVIAL(info) << "Found the Preset locally: " << name;
        //BBS: we should compare the time between cloud and local
        if ((cloud_update_time == 0) || (cloud_update_time <= iter->updated_time)) {
            if (cloud_update_time < iter->updated_time)
                iter->sync_info = "update";
            else
                iter->sync_info.clear();
            // Fixup possible data lost
            iter->setting_id = cloud_setting_id;
            fs::path idx_file(iter->file);
            idx_file.replace_extension(".info");
            iter->save_info(idx_file.string());
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format("preset %1%'s update_time is eqaul or newer, cloud  update_time %2%, local update_time %3%")%name %cloud_update_time %iter->updated_time;
            unlock();
            return false;
        }
        else {
            //update the one from cloud which is newer
            need_update = true;
        }
    }

    // base_id
    if (preset_values.find(BBL_JSON_KEY_BASE_ID) == preset_values.end()) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format("can not find base_id, not loading for user preset %1%") % name;
        unlock();
        return false;
    }
    std::string cloud_base_id = preset_values[BBL_JSON_KEY_BASE_ID];

    //filament_id
    std::string cloud_filament_id;
    if ((m_type == Preset::TYPE_FILAMENT) && preset_values.find(BBL_JSON_KEY_FILAMENT_ID) != preset_values.end()) {
        cloud_filament_id = preset_values[BBL_JSON_KEY_FILAMENT_ID];
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " " << name << " filament_id: " << cloud_filament_id << " base_id: " << cloud_base_id;
    }

    DynamicPrintConfig new_config, cloud_config;
    try {
        ConfigSubstitutions config_substitutions = cloud_config.load_string_map(preset_values, rule);
        if (! config_substitutions.empty())
            substitutions.push_back({ name, m_type, PresetConfigSubstitutions::Source::UserCloud, name, std::move(config_substitutions) });

        //BBS: use inherit config as the base
        Preset* inherit_preset = nullptr;
        ConfigOption* inherits_config = cloud_config.option(BBL_JSON_KEY_INHERITS);
        if (inherits_config) {
            ConfigOptionString * option_str = dynamic_cast<ConfigOptionString *> (inherits_config);
            std::string inherits_value = option_str->value;
            /*size_t pos = inherits_value.find_first_of('*');
            if (pos != std::string::npos) {
                inherits_value.replace(pos, 1, 1, '~');
                option_str->value = inherits_value;
            }*/
            inherit_preset = this->find_preset(inherits_value, false, true);
        }
        const Preset& default_preset = this->default_preset_for(cloud_config);
        if (inherit_preset) {
            new_config = inherit_preset->config;
            if (cloud_filament_id == "null") {
                cloud_filament_id = inherit_preset->filament_id;
            }
        }
        else {
            // We support custom root preset now
            auto inherits_config2 = dynamic_cast<ConfigOptionString *>(inherits_config);
            if (inherits_config2 && !inherits_config2->value.empty()) {
                //we should skip this preset here
                BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", can not find inherit preset for user preset %1%, just skip")%name;
                unlock();
                return false;
            }
            // Find a default preset for the config. The PrintPresetCollection provides different default preset based on the "printer_technology" field.
            new_config = default_preset.config;
        }

        extend_default_config_length(cloud_config, false, {});

        if (inherit_preset) {
            std::string extruder_id_name, extruder_variant_name;
            std::set<std::string> *key_set1 = nullptr, *key_set2 = nullptr;
            Preset::get_extruder_names_and_keysets(m_type, extruder_id_name, extruder_variant_name, &key_set1, &key_set2);

            new_config.update_diff_values_to_child_config(cloud_config, extruder_id_name, extruder_variant_name, *key_set1, *key_set2);
        }
        else{
            new_config.apply(std::move(cloud_config));
            extend_default_config_length(new_config, true, default_preset.config);
        }
        Preset::normalize(new_config);
        // Report configuration fields, which are misplaced into a wrong group.
        std::string incorrect_keys = Preset::remove_invalid_keys(new_config, default_preset.config);
        if (!incorrect_keys.empty()) {
            ++m_errors;
            BOOST_LOG_TRIVIAL(error) << "Error in a preset file: The preset \"" << name
                                     << "\" contains the following incorrect keys: " << incorrect_keys << ", which were removed";
        }
        if (need_update) {
            if (iter->name == m_edited_preset.name && iter->is_dirty) {
                // Keep modifies when update from remote
                new_config.apply_only(m_edited_preset.config, m_edited_preset.config.diff(iter->config));
            }
            iter->config = new_config;
            iter->updated_time = cloud_update_time;
            iter->sync_info    = "save";
            iter->version      = cloud_version.value();
            iter->user_id = cloud_user_id;
            iter->setting_id = cloud_setting_id;
            iter->base_id = cloud_base_id;
            iter->filament_id = cloud_filament_id;
            //presets_loaded.emplace_back(*it->second);
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", update the user preset %1% from cloud, type %2%, setting_id %3%, base_id %4%, sync_info %5% inherits %6%, filament_id %7%")
               % iter->name %Preset::get_type_string(m_type) %iter->setting_id %iter->base_id %iter->sync_info %iter->inherits() % iter->filament_id;
        }
        else {
            //create a new one
            Preset preset(m_type, name, false);
            preset.is_system = false;
            preset.loaded = true;
            preset.config = new_config;
            preset.updated_time = cloud_update_time;
            preset.sync_info   = "save";
            preset.version      = cloud_version.value();
            preset.user_id = cloud_user_id;
            preset.setting_id = cloud_setting_id;
            preset.base_id = cloud_base_id;
            preset.filament_id = cloud_filament_id;

            size_t cur_index = iter - m_presets.begin();
            m_presets.insert(iter, preset);
            //m_presets.emplace_back (preset);
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", insert a new user preset %1%, type %2%, setting_id %3%, base_id %4%, sync_info %5% inherits %6%, filament_id %7%")
               %preset.name %Preset::get_type_string(m_type) %preset.setting_id %preset.base_id %preset.sync_info %preset.inherits() %preset.filament_id;
            if (cur_index <= m_idx_selected) {
                m_idx_selected ++;
                BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(", increase m_idx_selected to %1%, due to user preset inserted")%m_idx_selected;
            }
        }
    } catch (const std::runtime_error &err) {
        errors_cummulative += err.what();
        errors_cummulative += "\n";
    }

    unlock();

    if (! errors_cummulative.empty())
        throw Slic3r::RuntimeError(errors_cummulative);

    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" finished, load user preset %1% , type %2%, errors_cummulative %3%")%name %Preset::get_type_string(m_type) %errors_cummulative;
    return (need_update)?false:true;
}

//re-sort and re-select
void PresetCollection::update_after_user_presets_loaded()
{
    lock();
    std::string     selected_name = get_selected_preset_name();
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", before sort, type %1%, selected_idx %2%, selected_name %3%") %m_type %m_idx_selected %selected_name;
    std::sort(m_presets.begin() + m_num_default_presets, m_presets.end());
    this->select_preset_by_name(selected_name, false);
    unlock();
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", after sort, type %1%, selected_idx %2%") %m_type %m_idx_selected;

    return;
}

//BBS: validate_preset
bool PresetCollection::validate_preset(const std::string &preset_name, std::string &inherit_name)
{
    std::deque<Preset>::iterator it       = this->find_preset_internal(preset_name);
    bool                         found    = (it != m_presets.end()) && (it->name == preset_name) && (it->is_system || it->is_default);
    if (!found) {
        it = this->find_preset_renamed(preset_name);
        found = it != m_presets.end() && (it->is_system || it->is_default);
    }
    if (!found) {
        if (!inherit_name.empty()) {
            it    = this->find_preset_internal(inherit_name);
            found = it != m_presets.end() && it->name == inherit_name && (it->is_system || it->is_default);
            if (found)
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": preset_name %1%, inherit_name %2%, found inherit in list")%preset_name %inherit_name;
            else
                BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": preset_name %1%, inherit_name %2%, can not found preset and inherit in list")%preset_name %inherit_name;
        }
        else {
            //inherit is null , should not happen , just consider it as valid
            found = false;
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": preset_name %1%, no inherit, set to not found")%preset_name;
        }
    }
    else {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": preset_name %1%, found in list")%preset_name;
    }

    return found;
}


// Load a preset from an already parsed config file, insert it into the sorted sequence of presets
// and select it, losing previous modifications.
Preset& PresetCollection::load_preset(const std::string &path, const std::string &name, const DynamicPrintConfig &config, bool select, Semver file_version)
{
    DynamicPrintConfig cfg(this->default_preset().config);
    cfg.apply_only(config, cfg.keys(), true);
    return this->load_preset(path, name, std::move(cfg), select, file_version);
}

static bool profile_print_params_same(const DynamicPrintConfig &cfg_old, const DynamicPrintConfig &cfg_new)
{
    t_config_option_keys diff = cfg_old.diff(cfg_new);
    // Following keys are used by the UI, not by the slicing core, therefore they are not important
    // when comparing profiles for equality. Ignore them.
    for (const char *key : { "compatible_prints", "compatible_prints_condition",
                             "compatible_printers", "compatible_printers_condition", "inherits",
                             "print_settings_id", "filament_settings_id", "sla_print_settings_id", "sla_material_settings_id", "printer_settings_id",
                             "printer_model", "printer_variant", "default_print_profile", "default_filament_profile", "default_sla_print_profile", "default_sla_material_profile"
                             })
        diff.erase(std::remove(diff.begin(), diff.end(), key), diff.end());
    // Preset with the same name as stored inside the config exists.
    return diff.empty();
}

// Load a preset from an already parsed config file, insert it into the sorted sequence of presets
// and select it, losing previous modifications.
// Only a single profile could be edited at at the same time, which introduces complexity when loading
// filament profiles for multi-extruder printers.
std::pair<Preset*, bool> PresetCollection::load_external_preset(
    // Path to the profile source file (a G-code, an AMF or 3MF file, a config file)
    const std::string           &path,
    // Name of the profile, derived from the source file name.
    const std::string           &name,
    // Original name of the profile, extracted from the loaded config. Empty, if the name has not been stored.
    const std::string           &original_name,
    // Config to initialize the preset from. It may contain configs of all presets merged in a single dictionary!
    const DynamicPrintConfig    &combined_config,
    //different settings list
    const std::set<std::string> &different_settings_list,
    // Select the preset after loading?
    LoadAndSelect                select,
    const Semver                file_version,
    const std::string           filament_id)
{
    // Load the preset over a default preset, so that the missing fields are filled in from the default preset.
    DynamicPrintConfig cfg(this->default_preset_for(combined_config).config);
    // SoftFever: ignore print connection info from project
    auto        keys = cfg.keys();
    keys.erase(std::remove_if(keys.begin(), keys.end(),
                              [](std::string &val) {
                                return val == "print_host" || val == "print_host_webui" || val == "printhost_apikey" ||
                                       val == "printhost_cafile" || val == "printhost_user" || val == "printhost_password" || val == "printhost_port";
                              }),
               keys.end());
    cfg.apply_only(combined_config, keys, true);
    std::string                 &inherits = Preset::inherits(cfg);

    //BBS: add different settings check logic, replace the old system preset's default value with new system preset's default values
    std::deque<Preset>::iterator it       = this->find_preset_internal(original_name);
    bool                         found    = it != m_presets.end() && it->name == original_name;
    if (! found) {
        // Try to match the original_name against the "renamed_from" profile names of loaded system profiles.
        it = this->find_preset_renamed(original_name);
        found = it != m_presets.end();
    }

    std::string extruder_id_name, extruder_variant_name;
    std::set<std::string> *key_set1 = nullptr, *key_set2 = nullptr;
    Preset::get_extruder_names_and_keysets(m_type, extruder_id_name, extruder_variant_name, &key_set1, &key_set2);

    if (!inherits.empty() && (different_settings_list.size() > 0)) {
        auto iter = this->find_preset_internal(inherits);
        if (iter != m_presets.end() && iter->name == inherits) {
            //std::vector<std::string> dirty_options = cfg.diff(iter->config);
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": change preset %1% inherit %2% 's value to %3% 's values")%original_name %inherits %path;
            cfg.update_non_diff_values_to_base_config(iter->config, keys, different_settings_list, extruder_id_name, extruder_variant_name, *key_set1, *key_set2);
        }
    }
    else if (found && it->is_system && (different_settings_list.size() > 0)) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": change preset %1% 's value to %2% 's values")%original_name %path;
        cfg.update_non_diff_values_to_base_config(it->config, keys, different_settings_list, extruder_id_name, extruder_variant_name, *key_set1, *key_set2);
    }

    //BBS: add config related logs
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" enter, type %1% , path %2%, name %3%, original_name %4%, inherits %5%")%Preset::get_type_string(m_type) %path %name %original_name %inherits;
    if (select == LoadAndSelect::Never) {
        // Some filament profile has been selected and modified already.
        // Check whether this profile is equal to the modified edited profile.
        const Preset &edited = this->get_edited_preset();
        if ((edited.name == original_name || edited.name == inherits) && profile_print_params_same(edited.config, cfg)) {
            // Just point to that already selected and edited profile.
            //BBS: add config related logs
            BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" Just point to that already selected and edited profile %1%")%edited.name;
            return std::make_pair(&(*this->find_preset_internal(edited.name)), false);
        }
    }
    // Is there a preset already loaded with the name stored inside the config?
    /*std::deque<Preset>::iterator it       = this->find_preset_internal(original_name);
    bool                         found    = it != m_presets.end() && it->name == original_name;
    if (! found) {
        // Try to match the original_name against the "renamed_from" profile names of loaded system profiles.
        it = this->find_preset_renamed(original_name);
        found = it != m_presets.end();
    }*/
    if (found && profile_print_params_same(it->config, cfg)) {
        // The preset exists and it matches the values stored inside config.
        if (select == LoadAndSelect::Always)
            this->select_preset(it - m_presets.begin());
        //BBS: set the preset to visible
        if ( !it->is_visible ) {
            it->is_visible = true;
            //AppConfig* app_config = get_app_config();
            //if (app_config)
            //    app_config->set(AppConfig::SECTION_FILAMENTS, it->name, "1");
        }
        //BBS: add config related logs
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" The preset exists and it matches the values stored inside config. using original_name %1%")%original_name;
        return std::make_pair(&(*it), false);
    }
    if (! found && select != LoadAndSelect::Never && ! inherits.empty()) {
        // Try to use a system profile as a base to select the system profile
        // and override its settings with the loaded ones.
        assert(it == m_presets.end());
        it    = this->find_preset_internal(inherits);
        found = it != m_presets.end() && it->name == inherits;
        if (found && profile_print_params_same(it->config, cfg)) {
            // The system preset exists and it matches the values stored inside config.
            if (select == LoadAndSelect::Always)
                this->select_preset(it - m_presets.begin());
            //BBS: set the preset to visible
            if ( !it->is_visible ) {
                it->is_visible = true;
                //AppConfig* app_config = get_app_config();
                //if (app_config)
                //    app_config->set(AppConfig::SECTION_FILAMENTS, it->name, "1");
            }
            //BBS: add config related logs
            BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" The preset exists and it matches the values stored inside config. using inherits %1%")%inherits;
            return std::make_pair(&(*it), false);
        }
    }
    if (found) {
        //BBS: only select preset for always
        //if (select != LoadAndSelect::Never) {
        if (select == LoadAndSelect::Always) {
            // Select the existing preset and override it with new values, so that
            // the differences will be shown in the preset editor against the referenced profile.
            this->select_preset(it - m_presets.begin());
            // The source config may contain keys from many possible preset types. Just copy those that relate to this preset.
            //this->get_edited_preset().config.apply_only(combined_config, keys, true);
            this->get_edited_preset().config.apply_only(cfg, keys, true);
            this->update_dirty();
            update_saved_preset_from_current_preset();
            assert(this->get_edited_preset().is_dirty);
            //BBS: set the preset to visible
            if ( !it->is_visible ) {
                it->is_visible = true;
                //AppConfig* app_config = get_app_config();
                //if (app_config)
                //    app_config->set(AppConfig::SECTION_FILAMENTS, it->name, "1");
            }
            //BBS: add config related logs
            BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" Select the existing preset %1% and override it with new values")%it->name;
            return std::make_pair(&(*it), this->get_edited_preset().is_dirty);
        }

        //BBS: for other filaments under AMS
        if (it->is_project_embedded) {
            //update the properties back to the preset
            it->config.apply_only(cfg, keys, true);
            it->is_dirty = false;

            return std::make_pair(&(*it), false);
        }
        if (inherits.empty()) {
            // Update the "inherits" field.
            // There is a profile with the same name already loaded. Should we update the "inherits" field?
            inherits = it->vendor ? it->name : it->inherits();
        }
    }

    // The external preset does not match an internal preset, load the external preset.
    std::string new_name;
    //BBS: add project embedded preset logic
    //BBS: refine the name logic
    for (size_t idx = 0;; ++ idx) {
        std::string prefix;
        if (original_name.empty()) {
            if (!inherits.empty()) {
                if (idx == 0)
                    prefix = inherits;
                else
                    prefix = inherits + "-" + std::to_string(idx);
            }
            else {
                if (idx > 0)
                    prefix =  std::to_string(idx);
            }
        } else {
            std::string reduced_name = original_name;
            //TODO
            //boost::regex rx("3mf\(*\)");
            //boost::iterator_range<std::string::iterator> result = boost::algorithm::find_regex(reduced_name, rx);
            //if (!result.empty()) {
            //    reduced_name = std::string(result.begin(), result.end());
            //}

            if (idx == 0)
                prefix = reduced_name;
            else
                prefix = reduced_name + "-" + std::to_string(idx) ;
        }
        //new_name = name + suffix;
        new_name = prefix + "(" + name + ")";
        it = this->find_preset_internal(new_name);
        if (it == m_presets.end() || it->name != new_name)
            // Unique profile name. Insert a new profile.
            break;
        if (profile_print_params_same(it->config, cfg)) {
            // The preset exists and it matches the values stored inside config.
            if (select == LoadAndSelect::Always)
                this->select_preset(it - m_presets.begin());
            //BBS: add config related logs
            BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" The preset %1% exists and it matches the values stored inside config.")%new_name;
            return std::make_pair(&(*it), false);
        }
        // Form another profile name.
    }
    // Insert a new profile.
    //BBS: add project embedded preset logic
    bool from_project = boost::algorithm::iends_with(name, ".3mf");
    if (m_type == Preset::TYPE_PRINT)
        cfg.option<ConfigOptionString >("print_settings_id", true)->value  = new_name;
    else if (m_type == Preset::TYPE_FILAMENT)
        cfg.option<ConfigOptionStrings>("filament_settings_id", true)->values[0] = new_name;
    else if (m_type == Preset::TYPE_PRINTER)
        cfg.option<ConfigOptionString>("printer_settings_id", true)->value = new_name;
    Preset &preset = this->load_preset(path, new_name, std::move(cfg), select == LoadAndSelect::Always);
    preset.is_external = true;
    preset.version = file_version;
    if (!filament_id.empty())
        preset.filament_id = filament_id;
    else {
        if (!inherits.empty()) {
            Preset *parent = this->find_preset(inherits, false, true);
            if (parent)
                preset.filament_id = parent->filament_id;
        }
    }
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " " << preset.name << " filament_id: " << preset.filament_id << " base_id: " << preset.base_id;
    if (from_project) {
        preset.is_project_embedded = true;
    }
    else {
        //external config
        preset.file = path_for_preset(preset);
        //BBS: save full config here for external
        //we can not reach here
        preset.save(nullptr);
    }
    if (&this->get_selected_preset() == &preset)
        this->get_edited_preset().is_external = true;

    //BBS: add config related logs
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(", type %1% added a preset, name %2%, path %3%, is_system %4%, is_default %5% is_external %6%")%Preset::get_type_string(m_type) %preset.name %preset.file %preset.is_system %preset.is_default %preset.is_external;
    return std::make_pair(&preset, false);
}

Preset& PresetCollection::load_preset(const std::string &path, const std::string &name, DynamicPrintConfig &&config, bool select, Semver file_version)
{
    lock();
    auto it = this->find_preset_internal(name);
    if (it == m_presets.end() || it->name != name) {
        // The preset was not found. Create a new preset.
        if (m_presets.begin() + m_idx_selected >= it)
            ++m_idx_selected;
        it = m_presets.emplace(it, Preset(m_type, name, false));
    }
    Preset &preset = *it;
    preset.file = path;
    preset.config = std::move(config);
    preset.loaded = true;
    preset.is_dirty = false;

    //BBS
    if (file_version.valid())
        preset.version = file_version;
    if (select)
        this->select_preset_by_name(name, true);
    unlock();
    //BBS: add config related logs
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(", preset type %1%, name %2%, path %3%, is_system %4%, is_default %5% is_visible %6%")%Preset::get_type_string(m_type) %preset.name %preset.file %preset.is_system %preset.is_default %preset.is_visible;
    return preset;
}

bool PresetCollection::clone_presets(std::vector<Preset const *> const &presets, std::vector<std::string> &failures, std::function<void(Preset &, Preset::Type &)> modifier, bool force_rewritten)
{
    std::vector<Preset> new_presets;
    for (auto curr_preset : presets) {
        new_presets.push_back(*curr_preset);
        auto &preset = new_presets.back();
        preset.vendor         = nullptr;
        preset.renamed_from.clear();
        preset.m_excluded_from.clear();
        preset.setting_id.clear();
        preset.inherits().clear();
        preset.is_default  = false;
        preset.is_system   = false;
        preset.is_external = false;
        preset.is_visible = true;
        preset.is_project_embedded = false;
        modifier(preset, m_type);
        if (find_preset(preset.name) && !force_rewritten) {
            failures.push_back(preset.name);
        }
        preset.file                = this->path_for_preset(preset);
        if (m_type == Preset::TYPE_PRINT)
            preset.config.option<ConfigOptionString>("print_settings_id", true)->value = preset.name;
        else if (m_type == Preset::TYPE_FILAMENT)
            preset.config.option<ConfigOptionStrings>("filament_settings_id", true)->values[0] = preset.name;
        else if (m_type == Preset::TYPE_PRINTER)
            preset.config.option<ConfigOptionString>("printer_settings_id", true)->value = preset.name;
    }
    if (!failures.empty() && !force_rewritten)
        return false;
    lock();
    auto old_name = this->get_edited_preset().name;
    for (auto preset : new_presets) {
        preset.alias.clear();
        set_custom_preset_alias(preset);
        preset.base_id.clear();
        auto it = this->find_preset_internal(preset.name);
        assert((it == m_presets.end() || it->name != preset.name) || force_rewritten);
        if (it == m_presets.end() || it->name != preset.name) {
            Preset &new_preset = *m_presets.insert(it, preset);
            new_preset.save(nullptr);
        } else if (force_rewritten) {
            *it = preset;
            (*it).save(nullptr);
        }
    }
    this->select_preset_by_name(old_name, true);
    unlock();
    return true;
}

bool PresetCollection::clone_presets_for_printer(std::vector<Preset const *> const &     templates,
                                                 std::vector<std::string> &              failures,
                                                 std::string const &                     printer,
                                                 std::function<std::string(std::string)> create_filament_id,
                                                 bool                                    force_rewritten)
{
    return clone_presets(templates, failures, [printer, create_filament_id](Preset &preset, Preset::Type &type) {
            std::string prefix          = preset.name.substr(0, preset.name.find(" @"));
            std::replace(prefix.begin(), prefix.end(), '/', '-');
            preset.name                 = prefix + " @" + printer;
            auto *compatible_printers   = dynamic_cast<ConfigOptionStrings *>(preset.config.option("compatible_printers"));
            compatible_printers->values = std::vector<std::string>{printer};
            preset.is_visible           = true;
            if (type == Preset::TYPE_FILAMENT) {
                preset.filament_id = create_filament_id(prefix);
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " " << __LINE__ << preset.name << " create filament_id: " << preset.filament_id;
            }
    }, force_rewritten);
}

bool PresetCollection::clone_presets_for_filament(Preset const *const &     preset,
                                                  std::vector<std::string> &failures,
                                                  std::string const &       filament_name,
                                                  std::string const &       filament_id,
                                                  const DynamicConfig &     dynamic_config,
                                                  const std::string &       compatible_printers,
                                                  bool                      force_rewritten)
{
    std::vector<Preset const *> const presets = {preset};
    return clone_presets(presets, failures, [&filament_name, &filament_id, &dynamic_config, &compatible_printers](Preset &preset, Preset::Type &type) {
        preset.name        = filament_name + " @" + compatible_printers;
        if (type == Preset::TYPE_FILAMENT) {
            preset.config.apply_only(dynamic_config, {"filament_vendor", "compatible_printers", "filament_type"},true);

            preset.filament_id = filament_id;
            auto compatible = dynamic_cast<ConfigOptionStrings *>(preset.config.option("compatible_printers"));
            if (compatible->values.empty()) {
                BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " " << __LINE__ << preset.name << " apply compatible_printer failed";
                compatible->values.push_back(compatible_printers);
            }
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " " << __LINE__ << preset.name << " is cloned and filament_id: " << filament_id;
         }
        },
        force_rewritten);
}

std::map<std::string, std::vector<Preset const *>> PresetCollection::get_filament_presets() const
{
    std::map<std::string, std::vector<Preset const *>> filament_presets;
    for (auto &preset : m_presets) {
        if (preset.is_user()) {
            if (preset.inherits() == "") { filament_presets[preset.filament_id].push_back(&preset); }
            continue;
        }
        if (get_preset_base(preset) == &preset) { filament_presets[preset.filament_id].push_back(&preset); }
    }
    return filament_presets;
}

//BBS: add project embedded preset logic
void PresetCollection::save_current_preset(const std::string &new_name, bool detach, bool save_to_project, Preset* _curr_preset, const Preset* _current_printer)
{
    Preset curr_preset = _curr_preset ? *_curr_preset : m_edited_preset;
    //BBS: add lock logic for sync preset in background
    std::string final_inherits;
    lock();
    // 1) Find the preset with a new_name or create a new one,
    // initialize it with the edited config.
    auto it = this->find_preset_internal(new_name);
    if (it != m_presets.end() && it->name == new_name) {
        // Preset with the same name found.
        Preset &preset = *it;
        //BBS: add project embedded preset logic
        if (preset.is_default || preset.is_system) {
        //if (preset.is_default || preset.is_external || preset.is_system)
            // Cannot overwrite the default preset.
            //BBS: add lock logic for sync preset in background
            unlock();
            return;
        }
        // Overwriting an existing preset.
        preset.config = std::move(curr_preset.config);
        // The newly saved preset will be activated -> make it visible.
        preset.is_visible = true;
        //TODO: remove the detach logic
        if (detach) {
            // Clear the link to the parent profile.
            preset.vendor = nullptr;
			preset.inherits().clear();
			preset.alias.clear();
			preset.renamed_from.clear();
            preset.m_excluded_from.clear();
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": save preset %1% , with detach")%new_name;
        }
        //BBS: add lock logic for sync preset in background

        if (m_type == Preset::TYPE_PRINT)
            preset.config.option<ConfigOptionString>("print_settings_id", true)->value = new_name;
        else if (m_type == Preset::TYPE_FILAMENT)
            preset.config.option<ConfigOptionStrings>("filament_settings_id", true)->values[0] = new_name;
        else if (m_type == Preset::TYPE_PRINTER)
            preset.config.option<ConfigOptionString>("printer_settings_id", true)->value = new_name;
        final_inherits = preset.inherits();
        unlock();
        // TODO: apply change from custom root to devided presets.
        if (preset.inherits().empty()) {
            for (auto &preset2 : m_presets)
                if (preset2.inherits() == preset.name)
                    preset2.reload(preset);
        }
    } else {
        // Creating a new preset.
        Preset       &preset   = *m_presets.insert(it, curr_preset);
        std::string  &inherits = preset.inherits();
        std::string   old_name = preset.name;
        preset.name = new_name;
        preset.vendor = nullptr;
		preset.alias.clear();
        preset.renamed_from.clear();
        preset.m_excluded_from.clear();
        preset.setting_id.clear();
        if (detach) {
        	// Clear the link to the parent profile.
        	inherits.clear();
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": save preset %1% , with detach")%new_name;
        } else if (is_base_preset(preset)) {
            inherits = old_name;
        }
        // Orca: check if compatible_printers exists and is not empty, set it to the current printer if it is empty
        if (nullptr != _current_printer && preset.is_system && m_type == Preset::TYPE_FILAMENT) {
            ConfigOptionStrings* compatible_printers = preset.config.option<ConfigOptionStrings>("compatible_printers");
            if (compatible_printers && compatible_printers->values.empty()) {
                compatible_printers->values.push_back(_current_printer->name);
            }
        }

        preset.is_default  = false;
        preset.is_system   = false;
        preset.is_external = false;
        preset.file        = this->path_for_preset(preset);
        // The newly saved preset will be activated -> make it visible.
        preset.is_visible  = true;
        // Just system presets have aliases
        preset.alias.clear();
        //BBS: add project embedded preset logic
        if (save_to_project) {
            preset.is_project_embedded = true;
        }
        else
            preset.is_project_embedded = false;
        if (m_type == Preset::TYPE_PRINT)
            preset.config.option<ConfigOptionString>("print_settings_id", true)->value = new_name;
        else if (m_type == Preset::TYPE_FILAMENT)
            preset.config.option<ConfigOptionStrings>("filament_settings_id", true)->values[0] = new_name;
        else if (m_type == Preset::TYPE_PRINTER)
            preset.config.option<ConfigOptionString>("printer_settings_id", true)->value = new_name;
        //BBS: add lock logic for sync preset in background
        final_inherits = inherits;
        unlock();
    }
    // 2) Activate the saved preset.
    this->select_preset_by_name(new_name, true);
    // 2) Store the active preset to disk.
    //BBS: only save difference for user preset
    Preset* parent_preset = nullptr;
    if (!final_inherits.empty()) {
        parent_preset = this->find_preset(final_inherits, false, true);
        if (parent_preset && this->get_selected_preset().base_id.empty()) {
            this->get_selected_preset().base_id = parent_preset->setting_id;
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " base_id: " << parent_preset->setting_id;
        }
    }
    if (parent_preset)
        this->get_selected_preset().save(&(parent_preset->config));
    else
        this->get_selected_preset().save(nullptr);
}

bool PresetCollection::delete_current_preset()
{
    Preset &selected = this->get_selected_preset();
    if (selected.is_default)
        return false;

    if (get_preset_base(selected) == &selected) {
        for (auto &preset2 : m_presets)
            if (preset2.inherits() == selected.name)
                return false;
    }

    //BBS: add project embedded preset logic and refine is_external
    //if (! selected.is_external && ! selected.is_system) {
    if (! selected.is_system) {
        //BBS Erase the preset file.
        selected.remove_files();
    }
    //BBS: add lock logic for sync preset in background
    lock();
    // Remove the preset from the list.
    m_presets.erase(m_presets.begin() + m_idx_selected);
    unlock();

    // Find the next visible preset.
    size_t new_selected_idx = m_idx_selected;
    if (new_selected_idx < m_presets.size())
        for (; new_selected_idx < m_presets.size() && ! m_presets[new_selected_idx].is_visible; ++ new_selected_idx) ;
    if (new_selected_idx == m_presets.size())
        for (--new_selected_idx; new_selected_idx > 0 && !m_presets[new_selected_idx].is_visible; --new_selected_idx);
    this->select_preset(new_selected_idx);
    return true;
}

bool PresetCollection::delete_preset(const std::string& name)
{
    auto it = this->find_preset_internal(name);

    Preset& preset = *it;
    if (preset.is_default)
        return false;
    //BBS: add project embedded preset logic and refine is_external
    //if (!preset.is_external && !preset.is_system) {
    if (! preset.is_system) {
        preset.remove_files();
    }
    //BBS: add lock logic for sync preset in background
    lock();
    set_printer_hold_alias(it->alias, *it, true);
    m_presets.erase(it);
    unlock();

    return true;
}

const Preset* PresetCollection::get_selected_preset_parent() const
{
    if (this->get_selected_idx() == size_t(-1))
        // This preset collection has no preset activated yet. Only the get_edited_preset() is valid.
        return nullptr;

    const Preset 	  &selected_preset = this->get_selected_preset();
    if (get_preset_base(selected_preset) == &selected_preset)
        return &selected_preset;

    const Preset 	  &edited_preset   = this->get_edited_preset();
    const std::string &inherits        = edited_preset.inherits();
    const Preset      *preset          = nullptr;
    if (inherits.empty()) {
        if (selected_preset.is_external)
            return nullptr;
        preset = &this->default_preset(m_type == Preset::Type::TYPE_PRINTER && edited_preset.printer_technology() == ptSLA ? 1 : 0);
    } else
        preset = this->find_preset(inherits, false);
    if (preset == nullptr) {
	    // Resolve the "renamed_from" field.
    	assert(! inherits.empty());
    	auto it = this->find_preset_renamed(inherits);
		if (it != m_presets.end())
			preset = &(*it);
    }
    //BBS: add project embedded preset logic and refine is_external
    return (preset == nullptr/* || preset->is_default || preset->is_external*/) ? nullptr : preset;
    //return (preset == nullptr/* || preset->is_default*/ || preset->is_external) ? nullptr : preset;
}

const Preset* PresetCollection::get_preset_parent(const Preset& child) const
{
    const std::string &inherits = child.inherits();
    if (inherits.empty())
// 		return this->get_selected_preset().is_system ? &this->get_selected_preset() : nullptr;
        return nullptr;
    const Preset* preset = this->find_preset(inherits, false);
    if (preset == nullptr) {
    	auto it = this->find_preset_renamed(inherits);
		if (it != m_presets.end())
			preset = &(*it);
    }
    return
         // not found
        (preset == nullptr/* || preset->is_default */||
         // this should not happen, user profile should not derive from an external profile
         //BBS: add project embedded preset logic and refine is_external
         /*preset->is_external ||*/
         // this should not happen, however people are creative, see GH #4996
         preset == &child) ?
            nullptr :
            preset;
}

const Preset *PresetCollection::get_preset_base(const Preset &child) const
{
    if (child.is_system || child.is_default)
        return &child;
    // Handle user preset
    if (child.inherits().empty())
        return &child; // this is user root
    auto inherits = find_preset2(child.inherits(),true);
    return inherits ? get_preset_base(*inherits) : nullptr;
}

// Return vendor of the first parent profile, for which the vendor is defined, or null if such profile does not exist.
PresetWithVendorProfile PresetCollection::get_preset_with_vendor_profile(const Preset &preset) const
{
	const Preset		*p = &preset;
	const VendorProfile *v = nullptr;
	do {
		if (p->vendor != nullptr) {
			v = p->vendor;
			break;
		}
		p = this->get_preset_parent(*p);
	} while (p != nullptr);
	return PresetWithVendorProfile(preset, v);
}

const std::string& PresetCollection::get_preset_name_by_alias(const std::string& alias) const
{
	for (
		// Find the 1st profile name with the alias.
		auto it = Slic3r::lower_bound_by_predicate(m_map_alias_to_profile_name.begin(), m_map_alias_to_profile_name.end(), [&alias](auto &l){ return l.first < alias; });
		// Continue over all profile names with the same alias.
		it != m_map_alias_to_profile_name.end() && it->first == alias; ++ it)
        for (const std::string &preset_name : it->second) {
            if (auto it_preset = this->find_preset_internal(preset_name);
			it_preset != m_presets.end() && it_preset->name == preset_name &&
            it_preset->is_visible && (it_preset->is_compatible || size_t(it_preset - m_presets.begin()) == m_idx_selected))
	        return it_preset->name;
        }
		
    return alias;
}

const std::string* PresetCollection::get_preset_name_renamed(const std::string &old_name) const
{
	auto it_renamed = m_map_system_profile_renamed.find(old_name);
	if (it_renamed != m_map_system_profile_renamed.end())
		return &it_renamed->second;
	return nullptr;
}

bool PresetCollection::is_alias_exist(const std::string &alias, Preset* preset)
{
    auto it = m_map_alias_to_profile_name.find(alias);
    if (m_map_alias_to_profile_name.end() == it) return false;
    if (!preset) return true;

    auto compatible_printers = dynamic_cast<ConfigOptionStrings *>(preset->config.option("compatible_printers"));
    if (compatible_printers == nullptr) return true;

    for (const std::string &printer_name : compatible_printers->values) {
        auto printer_iter = m_printer_hold_alias.find(printer_name);
        if (m_printer_hold_alias.end() != printer_iter) {
            auto alias_iter = m_printer_hold_alias[printer_name].find(alias);
            if (m_printer_hold_alias[printer_name].end() != alias_iter) {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " " << " The alias already exists: " << alias << " and the preset name: " << preset->name;
                return true;
            }
        }
    }
    return false;
}

const std::string& PresetCollection::get_suffix_modified() {
    return g_suffix_modified;
}

// Return a preset by its name. If the preset is active, a temporary copy is returned.
// If a preset was not found by its name, null is returned.
Preset* PresetCollection::find_preset(const std::string &name, bool first_visible_if_not_found, bool real, bool only_from_library)
{
    Preset key(m_type, name, false);
    auto it = this->find_preset_internal(name, only_from_library);
    // Ensure that a temporary copy is returned if the preset found is currently selected.
    return (it != m_presets.end() && it->name == key.name) ? &this->preset(it - m_presets.begin(), real) :
        first_visible_if_not_found ? &this->first_visible() : nullptr;
}

Preset* PresetCollection::find_preset2(const std::string& name, bool auto_match/* = true */)
{
    auto preset = find_preset(name,false,true);
    if (preset == nullptr) {
        auto _name = get_preset_name_renamed(name);
        if (_name != nullptr)
            preset = find_preset(*_name,false,true);
        if (auto_match && preset == nullptr) {
            //Orca: one more try, find the most likely preset in OrcaFilamentLibrary
            if (name.find("Generic") != std::string::npos) {
                // The regex pattern matches an optional prefix ending in '_' then "Generic" followed by the material name.
                std::regex re(R"(^(?:.*?\b(?:\w+_)?)(Generic)\b\s+([^@]+?)\s*(?:@.*)?$)");
                auto       alter_name = std::regex_replace(name, re, "Generic $2 @System");
                preset                = find_preset2(alter_name, false);
                // print preset file name
                if (preset != nullptr) {
                    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << " " << "Failed to find: " << name
                                               << ". fallback to library preset: " << preset->file;
                }
            }
        }
    }

    return preset;
}

// Return index of the first visible preset. Certainly at least the '- default -' preset shall be visible.
size_t PresetCollection::first_visible_idx() const
{
    //BBS: set first visible filament to fla
    size_t first_visible = -1;
    size_t idx = m_default_suppressed ? m_num_default_presets : 0;
    for (; idx < m_presets.size(); ++ idx)
        if (m_presets[idx].is_visible && m_presets[idx].get_printer_id() == "BBL") {
            if (first_visible == -1)
                first_visible = idx;
            if (m_type != Preset::TYPE_FILAMENT)
                break;
            else {
                if (m_presets[idx].name.find("PLA") != std::string::npos) {
                    first_visible = idx;
                    break;
                }
            }
        }
    if (first_visible == -1) {
        if (m_presets.size() > 1 && m_default_suppressed)
            first_visible = m_presets.size() == m_num_default_presets ? 0 : m_num_default_presets;
        else
            first_visible = 0;
    }
    return first_visible;
}

std::vector<std::string> PresetCollection::diameters_of_selected_printer()
{
    std::set<std::string> diameters;
    auto printer_model = m_edited_preset.config.opt_string("printer_model");
    for (auto &preset : m_presets) {
        if (preset.config.opt_string("printer_model") == printer_model)
            diameters.insert(preset.config.opt_string("printer_variant"));
    }
    return std::vector<std::string>{diameters.begin(), diameters.end()};
}

void PresetCollection::set_default_suppressed(bool default_suppressed)
{
    if (m_default_suppressed != default_suppressed) {
        m_default_suppressed = default_suppressed;
        bool default_visible = ! default_suppressed || m_idx_selected < m_num_default_presets;
        for (size_t i = 0; i < m_num_default_presets; ++ i)
            m_presets[i].is_visible = default_visible;
    }
}

size_t PresetCollection::update_compatible_internal(const PresetWithVendorProfile &active_printer, const PresetWithVendorProfile *active_print, PresetSelectCompatibleType unselect_if_incompatible)
{
    DynamicPrintConfig config;
    config.set_key_value("printer_preset", new ConfigOptionString(active_printer.preset.name));
    const ConfigOption *opt = active_printer.preset.config.option("nozzle_diameter");
    if (opt)
        config.set_key_value("num_extruders", new ConfigOptionInt((int)static_cast<const ConfigOptionFloats*>(opt)->values.size()));
    int some_compatible = 0;

    if (active_print)
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": active printer %1%, print %2%, unselect_if_incompatible %3%")%active_printer.preset.name %active_print->preset.name % (int)unselect_if_incompatible;
    else
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": active printer %1%, unselect_if_incompatible %2%")%active_printer.preset.name % (int)unselect_if_incompatible;
    for (size_t idx_preset = m_num_default_presets; idx_preset < m_presets.size(); ++ idx_preset) {
        bool    selected        = idx_preset == m_idx_selected;
        Preset &preset_selected = m_presets[idx_preset];
        Preset &preset_edited   = selected ? m_edited_preset : preset_selected;

        const PresetWithVendorProfile this_preset_with_vendor_profile = this->get_preset_with_vendor_profile(preset_edited);
        bool    was_compatible  = preset_edited.is_compatible;
        preset_edited.is_compatible = is_compatible_with_printer(this_preset_with_vendor_profile, active_printer, &config);
        if (preset_edited.is_compatible)
            some_compatible++;
	    if (active_print != nullptr)
	        preset_edited.is_compatible &= is_compatible_with_print(this_preset_with_vendor_profile, *active_print, active_printer);
        if (! preset_edited.is_compatible && selected &&
            (unselect_if_incompatible == PresetSelectCompatibleType::Always || (unselect_if_incompatible == PresetSelectCompatibleType::OnlyIfWasCompatible && was_compatible)))
        {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(":  type %1% , previous selected %2% becomes uncompatible, will select later")%Preset::get_type_string(m_type) %m_idx_selected;
            m_idx_selected = size_t(-1);
        }
        if (selected)
            preset_selected.is_compatible = preset_edited.is_compatible;
    }
    // Update visibility of the default profiles here if the defaults are suppressed, the current profile is not compatible and we don't want to select another compatible profile.
    if (m_idx_selected >= m_num_default_presets && m_default_suppressed)
    {
        for (size_t i = 0; i < m_num_default_presets; ++ i)
        {
            m_presets[i].is_visible = (some_compatible == 0);
        }
    }

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": type %1% returned m_idx_selected %2%, some_compatible %3%")%Preset::get_type_string(m_type) %m_idx_selected %some_compatible;
    return m_idx_selected;
}

// Update a dirty flag of the current preset
// Return true if the dirty flag changed.
bool PresetCollection::update_dirty()
{
    bool was_dirty = this->get_selected_preset().is_dirty;
    bool is_dirty  = current_is_dirty();
    this->get_selected_preset().is_dirty = is_dirty;
    this->get_edited_preset().is_dirty = is_dirty;

    return was_dirty != is_dirty;
}

template<class T>
void add_correct_opts_to_diff(const std::string &opt_key, t_config_option_keys& vec, const ConfigBase &other, const ConfigBase &this_c, bool strict)
{
    const T* opt_init = static_cast<const T*>(other.option(opt_key));
    const T* opt_cur = static_cast<const T*>(this_c.option(opt_key));
    int opt_init_max_id = opt_init->values.size() - 1;
    if (opt_init_max_id < 0) {
        for (int i = 0; i < int(opt_cur->values.size()); i++)
            vec.emplace_back(opt_key + "#" + std::to_string(i));
        return;
    }

    for (int i = 0; i < int(opt_cur->values.size()); i++)
    {
        int init_id = i <= opt_init_max_id ? i : 0;
        if (opt_cur->values[i] != opt_init->values[init_id]) {
            if (opt_cur->nullable()) {
                if (opt_cur->is_nil(i)) {
                    if (strict && !opt_init->is_nil(init_id))
                        vec.emplace_back(opt_key + "#" + std::to_string(i));
                } else {
                    if (strict || !opt_init->is_nil(init_id))
                        vec.emplace_back(opt_key + "#" + std::to_string(i));
                }
            } else {
                vec.emplace_back(opt_key + "#" + std::to_string(i));
            }
        }
    }
}

// Use deep_diff to correct return of changed options, considering individual options for each extruder.
inline t_config_option_keys deep_diff(const ConfigBase &config_this, const ConfigBase &config_other, bool strict = true)
{
    t_config_option_keys diff;
    t_config_option_keys keys;
    if (strict) {
        t_config_option_keys keys_this = config_this.keys();
        t_config_option_keys keys_other = config_other.keys();
        std::set_union(keys_this.begin(), keys_this.end(), keys_other.begin(), keys_other.end(), std::back_inserter(keys));
    } else {
        keys = config_this.keys();
    }
    for (const t_config_option_key &opt_key : keys) {
        const ConfigOption *this_opt  = config_this.option(opt_key);
        const ConfigOption *other_opt = config_other.option(opt_key);
        if (this_opt != nullptr && other_opt != nullptr && *this_opt != *other_opt)
        {
            //BBS: add bed_exclude_area
            if (opt_key == "printable_area" || opt_key == "bed_exclude_area" || opt_key == "compatible_prints" || opt_key == "compatible_printers" || opt_key == "thumbnails" ||  opt_key == "wrapping_exclude_area") {
                // Scalar variable, or a vector variable, which is independent from number of extruders,
                // thus the vector is presented to the user as a single input.
                diff.emplace_back(opt_key);
            } else if (opt_key == "default_filament_profile") {
                // Ignore this field, it is not presented to the user, therefore showing a "modified" flag for this parameter does not help.
                // Also the length of this field may differ, which may lead to a crash if the block below is used.
            }
            else if (opt_key == "thumbnails") {
                // "thumbnails" can not contain extensions in old config but they are valid and use PNG extension by default
                // So, check if "thumbnails" is really changed
                // We will compare full thumbnails instead of exactly config values
                auto [thumbnails, er]         = GCodeThumbnails::make_and_check_thumbnail_list(config_this);
                auto [thumbnails_new, er_new] = GCodeThumbnails::make_and_check_thumbnail_list(config_other);
                if (thumbnails != thumbnails_new || er != er_new)
                    // if those strings are actually the same, erase them from the list of dirty oprions
                    diff.emplace_back(opt_key);
            } else {
                switch (other_opt->type()) {
                case coInts:    add_correct_opts_to_diff<ConfigOptionInts       >(opt_key, diff, config_other, config_this, strict);  break;
                case coBools:   add_correct_opts_to_diff<ConfigOptionBools      >(opt_key, diff, config_other, config_this, strict);  break;
                case coFloats:  add_correct_opts_to_diff<ConfigOptionFloats     >(opt_key, diff, config_other, config_this, strict);  break;
                case coStrings: add_correct_opts_to_diff<ConfigOptionStrings    >(opt_key, diff, config_other, config_this, strict);  break;
                case coPercents:add_correct_opts_to_diff<ConfigOptionPercents   >(opt_key, diff, config_other, config_this, strict);  break;
                case coFloatsOrPercents: add_correct_opts_to_diff<ConfigOptionFloatsOrPercents>(opt_key, diff, config_other, config_this, strict); break;
                case coPoints:  add_correct_opts_to_diff<ConfigOptionPoints     >(opt_key, diff, config_other, config_this, strict);  break;
                // BBS
                case coEnums: add_correct_opts_to_diff<ConfigOptionInts>(opt_key, diff, config_other, config_this, strict); break;
                default:        diff.emplace_back(opt_key);     break;
                }
            }
        }
        else if (strict) {
            const ConfigOption *opt = nullptr;
            if (this_opt != nullptr && other_opt == nullptr)
                opt = this_opt;
            else if (this_opt == nullptr && other_opt != nullptr)
                opt = other_opt;
            if (opt) {
                if (opt->type() & coVectorType) {
                    auto vec = dynamic_cast<ConfigOptionVectorBase const *>(opt);
                    for (size_t i = 0; i < vec->size(); i++)
                        diff.push_back(opt_key + "#" + std::to_string(i));
                } else {
                        diff.push_back(opt_key);
                }
            }
        }
    }
    return diff;
}

static constexpr const std::initializer_list<const char*> optional_keys { "compatible_prints", "compatible_printers" };
//BBS: skip these keys for dirty check
static std::set<std::string> skipped_in_dirty = {"printer_settings_id", "print_settings_id", "filament_settings_id"};

bool PresetCollection::is_dirty(const Preset *edited, const Preset *reference)
{
    if (edited != nullptr && reference != nullptr) {
        // Only compares options existing in both configs.
        if (! reference->config.equals(edited->config, &skipped_in_dirty))
            return true;
        // The "compatible_printers" option key is handled differently from the others:
        // It is not mandatory. If the key is missing, it means it is compatible with any printer.
        // If the key exists and it is empty, it means it is compatible with no printer.
        for (auto &opt_key : optional_keys)
            if (reference->config.has(opt_key) != edited->config.has(opt_key))
                return true;
    }
    return false;
}

std::vector<std::string> PresetCollection::dirty_options(const Preset *edited, const Preset *reference, const bool deep_compare /*= false*/)
{
    std::vector<std::string> changed;
    if (edited != nullptr && reference != nullptr) {
        // Only compares options existing in both configs.
        changed = deep_compare ?
                deep_diff(edited->config, reference->config) :
                reference->config.diff(edited->config);
        // The "compatible_printers" option key is handled differently from the others:
        // It is not mandatory. If the key is missing, it means it is compatible with any printer.
        // If the key exists and it is empty, it means it is compatible with no printer.
        for (auto &opt_key : optional_keys)
            if (reference->config.has(opt_key) != edited->config.has(opt_key))
                changed.emplace_back(opt_key);
    }
    return changed;
}

//BBS: add function for dirty_options_without_option_list
std::vector<std::string> PresetCollection::dirty_options_without_option_list(const Preset *edited, const Preset *reference, const std::set<std::string>& option_ignore_list, const bool deep_compare)
{
    std::vector<std::string> changed;
    if (edited != nullptr && reference != nullptr) {
        // Only compares options existing in both configs.
        changed = deep_compare ?
                deep_diff(edited->config, reference->config) :
                reference->config.diff(edited->config);
        // The "compatible_printers" option key is handled differently from the others:
        // It is not mandatory. If the key is missing, it means it is compatible with any printer.
        // If the key exists and it is empty, it means it is compatible with no printer.
        for (auto &opt_key : optional_keys) {
            if (reference->config.has(opt_key) != edited->config.has(opt_key))
                changed.emplace_back(opt_key);
        }
        auto iter = changed.begin();
        while (iter != changed.end()) {
            if (option_ignore_list.find(*iter) != option_ignore_list.end()) {
                iter = changed.erase(iter);
            }
            else {
                ++iter;
            }
        }
    }
    return changed;
}

// Select a new preset. This resets all the edits done to the currently selected preset.
// If the preset with index idx does not exist, a first visible preset is selected.
Preset& PresetCollection::select_preset(size_t idx)
{
    //BBS: add config related logs
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": %1% try to select preset %2%")%Preset::get_type_string(m_type) %idx;
    for (Preset &preset : m_presets)
        preset.is_dirty = false;
    if (idx >= m_presets.size())
        idx = first_visible_idx();
    m_idx_selected = idx;
    m_edited_preset = m_presets[idx];
    update_saved_preset_from_current_preset();
    bool default_visible = ! m_default_suppressed || m_idx_selected < m_num_default_presets;
    for (size_t i = 0; i < m_num_default_presets; ++i)
        m_presets[i].is_visible = default_visible;

    //set this preset to true
    if (!m_presets[idx].is_visible) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": %1% set %2%, idx %3% to visible") % Preset::get_type_string(m_type) % m_presets[idx].name % idx;
        m_presets[idx].is_visible = true;
    }
    //BBS: add config related logs
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": %1% select success, m_idx_selected %2%, name %3%, is_system %4%, is_default %5%")%Preset::get_type_string(m_type) % m_idx_selected % m_edited_preset.name % m_edited_preset.is_system % m_edited_preset.is_default;
    return m_presets[idx];
}

bool PresetCollection::select_preset_by_name(const std::string &name_w_suffix, bool force)
{
    //BBS: add config related logs
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": %1%, try to select by name %2%, force %3%")%Preset::get_type_string(m_type) %name_w_suffix %force;
    std::string name = Preset::remove_suffix_modified(name_w_suffix);
    // 1) Try to find the preset by its name.
    auto it = this->find_preset_internal(name);
    size_t idx = 0;
    if (it != m_presets.end() && it->name == name && it->is_visible)
        // Preset found by its name and it is visible.
        idx = it - m_presets.begin();
    else {
        // Find the first visible preset.
        for (size_t i = m_default_suppressed ? m_num_default_presets : 0; i < m_presets.size(); ++ i)
            if (m_presets[i].is_visible) {
                idx = i;
                break;
            }
        // If the first visible preset was not found, return the 0th element, which is the default preset.
    }

    // 2) Select the new preset.
    if (m_idx_selected != idx || force) {
        this->select_preset(idx);
        //BBS: add config related logs
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": %1%, select %2%, success")%Preset::get_type_string(m_type) %name_w_suffix;
        return true;
    }

    //BBS: add config related logs
    if (m_idx_selected == idx)
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": %1%, already selected before") % Preset::get_type_string(m_type);
    else
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": %1%, select %2%, failed")%Preset::get_type_string(m_type) %name_w_suffix;
    return false;
}

bool PresetCollection::select_preset_by_name_strict(const std::string &name)
{
    //BBS: add config related logs
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": %1%, try to select by name %2%")%Preset::get_type_string(m_type) %name;
    // 1) Try to find the preset by its name.
    auto it = this->find_preset_internal(name);

    size_t idx = (size_t)-1;
    if (it != m_presets.end() && it->name == name && it->is_visible)
        // Preset found by its name.
        idx = it - m_presets.begin();
    // 2) Select the new preset.
    if (idx != (size_t)-1) {
        this->select_preset(idx);
        //BBS: add config related logs
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": %1%, select %2%, success")%Preset::get_type_string(m_type) %name;
        return true;
    }
    m_idx_selected = idx;
    //BBS: add config related logs
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": %1%, select %2%, failed")%Preset::get_type_string(m_type) %name;
    return false;
}

// Merge one vendor's presets with the other vendor's presets, report duplicates.
std::vector<std::string> PresetCollection::merge_presets(PresetCollection &&other, const VendorMap &new_vendors)
{
    std::vector<std::string> duplicates;
    for (Preset &preset : other.m_presets) {
        if (preset.is_default || preset.is_external)
            continue;
        Preset key(m_type, preset.name);
        auto it = std::lower_bound(m_presets.begin() + m_num_default_presets, m_presets.end(), key);
        if (it == m_presets.end() || it->name != preset.name) {
            if (preset.vendor != nullptr) {
                // Re-assign a pointer to the vendor structure in the new PresetBundle.
                auto it = new_vendors.find(preset.vendor->id);
                assert(it != new_vendors.end());
                preset.vendor = &it->second;
            }
            m_presets.emplace(it, std::move(preset));
        } else
            duplicates.emplace_back(std::move(preset.name));
    }
    return duplicates;
}

void PresetCollection::update_vendor_ptrs_after_copy(const VendorMap &new_vendors)
{
    for (Preset &preset : m_presets)
        if (preset.vendor != nullptr) {
            assert(! preset.is_default && ! preset.is_external);
            // Re-assign a pointer to the vendor structure in the new PresetBundle.
            auto it = new_vendors.find(preset.vendor->id);
            assert(it != new_vendors.end());
            preset.vendor = &it->second;
        }
}

void PresetCollection::update_map_alias_to_profile_name()
{
	m_map_alias_to_profile_name.clear();
    for (const Preset &preset : m_presets) {
        m_map_alias_to_profile_name[preset.alias].push_back(preset.name);
    }
	// now m_map_alias_to_profile_name is map, not need sort
	//std::sort(m_map_alias_to_profile_name.begin(), m_map_alias_to_profile_name.end(), [](auto &l, auto &r) { return l.first < r.first; });
}

void PresetCollection::update_library_profile_excluded_from()
{
    // Orca: Collect all filament presets that has empty compatible_printers and belongs to the Orca Filament Library.
    std::map<std::string, std::set<std::string>*> excluded_froms;
    for (Preset& preset : m_presets) {
        if (preset.vendor != nullptr && preset.vendor->name == PresetBundle::ORCA_FILAMENT_LIBRARY) {
            // check if the preset has empty compatible_printers
            const auto* compatible_printers = dynamic_cast<const ConfigOptionStrings*>(preset.config.option("compatible_printers"));
            if (compatible_printers == nullptr || compatible_printers->values.empty())
                excluded_froms[preset.alias] = &preset.m_excluded_from;
        }
    }

    // Check all presets that has the same alias as the filament presets with empty compatible_printers in Orca Filament Library.
    for (const Preset& preset : m_presets) {
        if (preset.vendor == nullptr || preset.vendor->name == PresetBundle::ORCA_FILAMENT_LIBRARY)
            continue;

        const auto* compatible_printers = dynamic_cast<const ConfigOptionStrings*>(preset.config.option("compatible_printers"));
        // All profiles in concrete vendor profile shouldn't have empty compatible_printers, but here we check it for safety.
        if (compatible_printers == nullptr || compatible_printers->values.empty())
            continue;
        auto itr = excluded_froms.find(preset.alias);
        if (itr != excluded_froms.end()) {
            // Add the printer models to the excluded_from list.
            for (const std::string& printer_name : compatible_printers->values) {
                itr->second->insert(printer_name);
            }
        }
    }
}

void PresetCollection::update_map_system_profile_renamed()
{
	m_map_system_profile_renamed.clear();
	for (Preset &preset : m_presets)
		for (const std::string &renamed_from : preset.renamed_from) {
            const auto [it, success] = m_map_system_profile_renamed.insert(std::pair<std::string, std::string>(renamed_from, preset.name));
            if (!success) {
                ++m_errors;
                BOOST_LOG_TRIVIAL(error) << boost::format("Preset name \"%1%\" was marked as renamed from \"%2%\", though preset name "
                                                          "\"%3%\" was marked as renamed from \"%2%\" as well.") %
                                                preset.name % renamed_from % it->second;
            }
        }
}

void PresetCollection::set_custom_preset_alias(Preset &preset)
{
    if (m_type == Preset::Type::TYPE_FILAMENT && preset.config.has(BBL_JSON_KEY_INHERITS) && preset.config.option<ConfigOptionString>(BBL_JSON_KEY_INHERITS)->value.empty()) {
        std::string alias_name;
        std::string preset_name = preset.name;
        if (alias_name.empty()) {
            size_t end_pos = preset_name.find_first_of("@");
            if (end_pos != std::string::npos) {
                alias_name = preset_name.substr(0, end_pos);
                boost::trim_right(alias_name);
            }
        }
        if (alias_name.empty() || is_alias_exist(alias_name, &preset))
            preset.alias = "";
        else {
            preset.alias = std::move(alias_name);
            m_map_alias_to_profile_name[preset.alias].push_back(preset.name);
            set_printer_hold_alias(preset.alias, preset);
        }
    }
}

void PresetCollection::set_printer_hold_alias(const std::string &alias, Preset &preset, bool remove)
{
    auto compatible_printers = dynamic_cast<ConfigOptionStrings *>(preset.config.option("compatible_printers"));
    if (compatible_printers == nullptr) return;
    for (const std::string &printer_name : compatible_printers->values) {
        auto printer_iter = m_printer_hold_alias.find(printer_name);
        bool insert_success = false, remove_success = false;
        if (m_printer_hold_alias.end() == printer_iter) {
            if (!remove) {
                insert_success = true;
                m_printer_hold_alias[printer_name].insert(alias);
            }
        } else {
            auto &printer_filament_alias = m_printer_hold_alias[printer_name];
            auto  alias_iter             = printer_filament_alias.find(alias);
            if (printer_filament_alias.end() == alias_iter) {
                if (!remove) {
                    insert_success = true;
                    printer_filament_alias.insert(alias);
                }
            } else {
                if (remove) {
                    if (preset.inherits() == "") {
                        remove_success = true;
                        printer_filament_alias.erase(alias);
                    }
                    if (auto alias_iter = m_map_alias_to_profile_name.find(alias); alias_iter != m_map_alias_to_profile_name.end()) {
                        auto& presets = alias_iter->second;
                        auto  new_end = std::remove(presets.begin(), presets.end(), preset.name);
                        presets.erase(new_end, presets.end());
                        if (presets.empty()) { m_map_alias_to_profile_name.erase(alias); }
                    }
                }
            }
        }
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " " << " preset name : " << preset.name << " remove action: " << remove << " insert success: "
                                << insert_success << " remove success: " << remove_success << " alias: " << alias;
    }
}

std::string PresetCollection::name() const
{
    switch (this->type()) {
    case Preset::TYPE_PRINT:        return L(PRESET_PRINT_NAME);
    case Preset::TYPE_FILAMENT:     return L(PRESET_FILAMENT_NAME);
    //case Preset::TYPE_SLA_PRINT:    return L("SLA print");
    //case Preset::TYPE_SLA_MATERIAL: return L("SLA material");
    case Preset::TYPE_PRINTER:      return L(PRESET_PRINTER_NAME);
    default:                        return "invalid";
    }
}

//BBS: change directoties by design
std::string PresetCollection::section_name() const
{
    switch (this->type()) {
    case Preset::TYPE_PRINT:        return PRESET_PRINT_NAME;
    case Preset::TYPE_FILAMENT:     return PRESET_FILAMENT_NAME;
    //case Preset::TYPE_SLA_PRINT:    return PRESET_SLA_PRINT_NAME;
    //case Preset::TYPE_SLA_MATERIAL: return PRESET_SLA_MATERIALS_NAME;
    case Preset::TYPE_PRINTER:      return PRESET_PRINTER_NAME;
    default:                        return "invalid";
    }
}

// Used for validating the "inherits" flag when importing user's config bundles.
// Returns names of all system presets including the former names of these presets.
std::vector<std::string> PresetCollection::system_preset_names() const
{
    size_t num = 0;
    for (const Preset &preset : m_presets)
        if (preset.is_system)
            ++ num;
    std::vector<std::string> out;
    out.reserve(num);
    for (const Preset &preset : m_presets)
        if (preset.is_system) {
            out.emplace_back(preset.name);
            out.insert(out.end(), preset.renamed_from.begin(), preset.renamed_from.end());
        }
    std::sort(out.begin(), out.end());
    return out;
}

// Generate a file path from a profile name. Add the ".ini" suffix if it is missing.
std::string PresetCollection::path_from_name(const std::string &new_name, bool detach) const
{
     //BBS: change to json format
    //std::string file_name = boost::iends_with(new_name, ".ini") ? new_name : (new_name + ".ini");
    std::string file_name = boost::iends_with(new_name, ".json") ? new_name : (new_name + ".json");
    if (detach)
        return (boost::filesystem::path(m_dir_path) / "base" / file_name).make_preferred().string();
    else
        return (boost::filesystem::path(m_dir_path) / file_name).make_preferred().string();
}

std::string PresetCollection::path_for_preset(const Preset &preset) const
{
    return path_from_name(preset.name, is_base_preset(preset));
}

const Preset& PrinterPresetCollection::default_preset_for(const DynamicPrintConfig &config) const
{
    const ConfigOptionEnumGeneric *opt_printer_technology = config.opt<ConfigOptionEnumGeneric>("printer_technology");
    return this->default_preset((opt_printer_technology == nullptr || opt_printer_technology->value == ptFFF) ? 0 : 1);
}

const Preset* PrinterPresetCollection::find_system_preset_by_model_and_variant(const std::string &model_id, const std::string& variant) const
{
    if (model_id.empty()) { return nullptr; }

    const auto it = std::find_if(cbegin(), cend(), [&](const Preset &preset) {
        if (!preset.is_system || preset.config.opt_string("printer_model") != model_id)
            return false;
        if (variant.empty())
            return true;
        return preset.config.opt_string("printer_variant") == variant;
    });

    return it != cend() ? &*it : nullptr;
}

const Preset *PrinterPresetCollection::find_custom_preset_by_model_and_variant(const std::string &model_id, const std::string &variant) const
{
    if (model_id.empty()) { return nullptr; }

    const auto it = std::find_if(cbegin(), cend(), [&](const Preset &preset) {
        if (preset.config.opt_string("printer_model") != model_id)
            return false;
        if (variant.empty())
            return true;
        return preset.config.opt_string("printer_variant") == variant;
    });

    return it != cend() ? &*it : nullptr;
}

bool  PrinterPresetCollection::only_default_printers() const
{
    for (const auto& printer : get_presets()) {
        if (!boost::starts_with(printer.name,"Default") && printer.is_visible)
            return false;
    }
    return true;
}
// -------------------------
// ***  PhysicalPrinter  ***
// -------------------------

std::string PhysicalPrinter::separator()
{
    return " * ";
}

static std::vector<std::string> s_PhysicalPrinter_opts {
    "preset_name", // temporary option to compatibility with older Slicer
    "preset_names",
    "printer_technology",
    "bbl_use_printhost",
    "host_type",
    "print_host",
    "print_host_webui",
    "printhost_apikey",
    "printhost_cafile",
    "printhost_port",
    "printhost_authorization_type",
    // HTTP digest authentization (RFC 2617)
    "printhost_user",
    "printhost_password",
    "printhost_ssl_ignore_revoke"
};

const std::vector<std::string>& PhysicalPrinter::printer_options()
{
    return s_PhysicalPrinter_opts;
}

std::vector<std::string> PhysicalPrinter::presets_with_print_host_information(const PrinterPresetCollection& printer_presets)
{
    std::vector<std::string> presets;
    for (const Preset& preset : printer_presets)
        if (has_print_host_information(preset.config))
            presets.emplace_back(preset.name);

    return presets;
}

bool PhysicalPrinter::has_print_host_information(const DynamicPrintConfig& config)
{
    return false;
}

const std::set<std::string>& PhysicalPrinter::get_preset_names() const
{
    return preset_names;
}

// temporary workaround for compatibility with older Slicer
static void update_preset_name_option(const std::set<std::string>& preset_names, DynamicPrintConfig& config)
{
    std::string name;
    for (auto el : preset_names)
        name += el + ";";
    name.pop_back();
    config.set_key_value("preset_name", new ConfigOptionString(name));
}

void PhysicalPrinter::update_preset_names_in_config()
{
    if (!preset_names.empty()) {
        std::vector<std::string>& values = config.option<ConfigOptionStrings>("preset_names")->values;
        values.clear();
        for (auto preset : preset_names)
            values.push_back(preset);

        // temporary workaround for compatibility with older Slicer
        update_preset_name_option(preset_names, config);
    }
}

void PhysicalPrinter::save(const std::string& file_name_from, const std::string& file_name_to)
{
    // rename the file
    boost::nowide::rename(file_name_from.data(), file_name_to.data());
    this->file = file_name_to;
    // save configuration
    //BBS: change to save
    //this->config.save(this->file);
    this->config.save_to_json(this->file, std::string("Physical_Printer"), std::string("User"), std::string(SLIC3R_VERSION));
}

void PhysicalPrinter::update_from_preset(const Preset& preset)
{
    config.apply_only(preset.config, printer_options(), true);
    // add preset names to the options list
    preset_names.emplace(preset.name);
    update_preset_names_in_config();
}

void PhysicalPrinter::update_from_config(const DynamicPrintConfig& new_config)
{
    config.apply_only(new_config, printer_options(), false);

    const std::vector<std::string>& values = config.option<ConfigOptionStrings>("preset_names")->values;

    if (values.empty())
        preset_names.clear();
    else {
        for (const std::string& val : values)
            preset_names.emplace(val);
        // temporary workaround for compatibility with older Slicer
        update_preset_name_option(preset_names, config);
    }
}

void PhysicalPrinter::reset_presets()
{
    return preset_names.clear();
}

bool PhysicalPrinter::add_preset(const std::string& preset_name)
{
    return preset_names.emplace(preset_name).second;
}

bool PhysicalPrinter::delete_preset(const std::string& preset_name)
{
    return preset_names.erase(preset_name) > 0;
}

PhysicalPrinter::PhysicalPrinter(const std::string& name, const DynamicPrintConfig& default_config) :
    name(name), config(default_config)
{
    update_from_config(config);
}

PhysicalPrinter::PhysicalPrinter(const std::string& name, const DynamicPrintConfig &default_config, const Preset& preset) :
    name(name), config(default_config)
{
    update_from_preset(preset);
}

void PhysicalPrinter::set_name(const std::string& name)
{
    this->name = name;
}

std::string PhysicalPrinter::get_full_name(std::string preset_name) const
{
    return name + separator() + preset_name;
}

std::string PhysicalPrinter::get_short_name(std::string full_name)
{
    int pos = full_name.find(separator());
    if (pos > 0)
        boost::erase_tail(full_name, full_name.length() - pos);
    return full_name;
}

std::string PhysicalPrinter::get_preset_name(std::string name)
{
    int pos = name.find(separator());
    boost::erase_head(name, pos + 3);
    return Preset::remove_suffix_modified(name);
}


// -----------------------------------
// ***  PhysicalPrinterCollection  ***
// -----------------------------------

PhysicalPrinterCollection::PhysicalPrinterCollection( const std::vector<std::string>& keys)
{
    // Default config for a physical printer containing all key/value pairs of PhysicalPrinter::printer_options().
    for (const std::string &key : keys) {
        const ConfigOptionDef *opt = print_config_def.get(key);
        assert(opt);
        assert(opt->default_value);
        m_default_config.set_key_value(key, opt->default_value->clone());
    }
}

// Load all printers found in dir_path.
// Throws an exception on error.
void PhysicalPrinterCollection::load_printers(
    const std::string& dir_path, const std::string& subdir,
    PresetsConfigSubstitutions& substitutions, ForwardCompatibilitySubstitutionRule substitution_rule)
{
    // Don't use boost::filesystem::canonical() on Windows, it is broken in regard to reparse points,
    // see https://github.com/prusa3d/PrusaSlicer/issues/732
    boost::filesystem::path dir = boost::filesystem::absolute(boost::filesystem::path(dir_path) / subdir).make_preferred();
    m_dir_path = dir.string();
    if(!boost::filesystem::exists(dir))
        return;
    std::string errors_cummulative;
    // Store the loaded printers into a new vector, otherwise the binary search for already existing presets would be broken.
    std::deque<PhysicalPrinter> printers_loaded;
    //BBS: change to json format
    for (auto& dir_entry : boost::filesystem::directory_iterator(dir))
    {
        std::string file_name = dir_entry.path().filename().string();
        //if (Slic3r::is_ini_file(dir_entry)) {
        if (Slic3r::is_json_file(file_name)) {
            // Remove the .json suffix.
            std::string name = file_name.erase(file_name.size() - 5);
            if (this->find_printer(name, false)) {
                // This happens when there's is a preset (most likely legacy one) with the same name as a system preset
                // that's already been loaded from a bundle.
                BOOST_LOG_TRIVIAL(warning) << "Printer already present, not loading: " << name;
                continue;
            }
            try {
                PhysicalPrinter printer(name, this->default_config());
                printer.file = dir_entry.path().string();
                // Load the preset file, apply preset values on top of defaults.
                try {
                    DynamicPrintConfig config;
                    //ConfigSubstitutions config_substitutions = config.load_from_ini(printer.file, substitution_rule);
                    std::map<std::string, std::string> key_values;
                    std::string reason;
                    ConfigSubstitutions config_substitutions = config.load_from_json(printer.file, substitution_rule, key_values, reason);
                    if (! config_substitutions.empty())
                        substitutions.push_back({ name, Preset::TYPE_PHYSICAL_PRINTER, PresetConfigSubstitutions::Source::UserFile, printer.file, std::move(config_substitutions) });
                    printer.update_from_config(config);
                    printer.loaded = true;
                }
                catch (const std::ifstream::failure& err) {
                    throw Slic3r::RuntimeError(std::string("The selected preset cannot be loaded: ") + printer.file + "\n\tReason: " + err.what());
                }
                catch (const std::runtime_error& err) {
                    throw Slic3r::RuntimeError(std::string("Failed loading the preset file: ") + printer.file + "\n\tReason: " + err.what());
                }
                printers_loaded.emplace_back(printer);
            }
            catch (const std::runtime_error& err) {
                errors_cummulative += err.what();
                errors_cummulative += "\n";
            }
        }
    }
    m_printers.insert(m_printers.end(), std::make_move_iterator(printers_loaded.begin()), std::make_move_iterator(printers_loaded.end()));
    std::sort(m_printers.begin(), m_printers.end());
    if (!errors_cummulative.empty())
        throw Slic3r::RuntimeError(errors_cummulative);
}

void PhysicalPrinterCollection::load_printer(const std::string& path, const std::string& name, DynamicPrintConfig&& config, bool select, bool save/* = false*/)
{
    auto it = this->find_printer_internal(name);
    if (it == m_printers.end() || it->name != name) {
        // The preset was not found. Create a new preset.
        it = m_printers.emplace(it, PhysicalPrinter(name, config));
    }

    it->file = path;
    it->config = std::move(config);
    it->loaded = true;
    if (select)
        this->select_printer(*it);

    if (save)
        it->save(nullptr);
}

// if there is saved user presets, contains information about "Print Host upload",
// Create default printers with this presets
// Note! "Print Host upload" options will be cleared after physical printer creations
void PhysicalPrinterCollection::load_printers_from_presets(PrinterPresetCollection& printer_presets)
{
//BBS
#if 0
    int cnt=0;
    for (Preset& preset: printer_presets) {
        DynamicPrintConfig& config = preset.config;
        for(const char* option : legacy_print_host_options) {
            if (!config.opt_string(option).empty()) {
                // check if printer with those "Print Host upload" options already exist
                PhysicalPrinter* existed_printer = find_printer_with_same_config(config);
                if (existed_printer)
                    // just add preset for this printer
                    existed_printer->add_preset(preset.name);
                else {
                    std::string new_printer_name = (boost::format("Printer %1%") % ++cnt ).str();
                    while (find_printer(new_printer_name))
                        new_printer_name = (boost::format("Printer %1%") % ++cnt).str();

                    // create new printer from this preset
                    PhysicalPrinter printer(new_printer_name, this->default_config(), preset);
                    printer.loaded = true;
                    save_printer(printer);
                }

                // erase "Print Host upload" information from the preset
                for (const char *opt : legacy_print_host_options)
                    config.opt_string(opt).clear();
                // save changes for preset
                preset.save(nullptr);

                // update those changes for edited preset if it's equal to the preset
                Preset& edited = printer_presets.get_edited_preset();
                if (preset.name == edited.name) {
                    for (const char *opt : legacy_print_host_options)
                        edited.config.opt_string(opt).clear();
                }

                break;
            }
        }
    }
#endif
}

PhysicalPrinter* PhysicalPrinterCollection::find_printer( const std::string& name, bool case_sensitive_search)
{
    auto it = this->find_printer_internal(name, case_sensitive_search);

    // Ensure that a temporary copy is returned if the preset found is currently selected.
    auto is_equal_name = [name, case_sensitive_search](const std::string& in_name) {
        if (case_sensitive_search)
            return in_name == name;
        return boost::to_lower_copy<std::string>(in_name) == boost::to_lower_copy<std::string>(name);
    };

    if (it == m_printers.end() || !is_equal_name(it->name))
        return nullptr;
    return &this->printer(it - m_printers.begin());
}

std::deque<PhysicalPrinter>::iterator PhysicalPrinterCollection::find_printer_internal(const std::string& name, bool case_sensitive_search/* = true*/)
{
    if (case_sensitive_search)
        return Slic3r::lower_bound_by_predicate(m_printers.begin(), m_printers.end(), [&name](const auto& l) { return l.name < name;  });

    std::string low_name = boost::to_lower_copy<std::string>(name);

    size_t i = 0;
    for (const PhysicalPrinter& printer : m_printers) {
        if (boost::to_lower_copy<std::string>(printer.name) == low_name)
            break;
        i++;
    }
    if (i == m_printers.size())
        return m_printers.end();

    return m_printers.begin() + i;
}

// Generate a file path from a profile name. Add the ".ini" suffix if it is missing.
std::string PhysicalPrinterCollection::path_from_name(const std::string& new_name) const
{
    //BBS: change to json format
    //std::string file_name = boost::iends_with(new_name, ".ini") ? new_name : (new_name + ".ini");
    std::string file_name = boost::iends_with(new_name, ".json") ? new_name : (new_name + ".json");
    return (boost::filesystem::path(m_dir_path) / file_name).make_preferred().string();
}

void PhysicalPrinterCollection::save_printer(PhysicalPrinter& edited_printer, const std::string& renamed_from/* = ""*/)
{
    // controll and update preset_names in edited_printer config
    edited_printer.update_preset_names_in_config();

    std::string name = renamed_from.empty() ? edited_printer.name : renamed_from;
    // 1) Find the printer with a new_name or create a new one,
    // initialize it with the edited config.
    auto it = this->find_printer_internal(name);
    if (it != m_printers.end() && it->name == name) {
        // Printer with the same name found.
        // Overwriting an existing preset.
        it->config = std::move(edited_printer.config);
        it->name = edited_printer.name;
        it->preset_names = edited_printer.preset_names;
        // sort printers and get new it
        std::sort(m_printers.begin(), m_printers.end());
        it = this->find_printer_internal(edited_printer.name);
    }
    else {
        // Creating a new printer.
        it = m_printers.emplace(it, edited_printer);
    }
    assert(it != m_printers.end());

    // 2) Save printer
    PhysicalPrinter& printer = *it;
    if (printer.file.empty())
        printer.file = this->path_from_name(printer.name);

    if (printer.file == this->path_from_name(printer.name))
        printer.save(nullptr);
    else
        // if printer was renamed, we should rename a file and than save the config
        printer.save(printer.file, this->path_from_name(printer.name));

    // update idx_selected
    m_idx_selected = it - m_printers.begin();
}

bool PhysicalPrinterCollection::delete_printer(const std::string& name)
{
    auto it = this->find_printer_internal(name);
    if (it == m_printers.end())
        return false;

    const PhysicalPrinter& printer = *it;
    // Erase the preset file.
    boost::nowide::remove(printer.file.c_str());
    m_printers.erase(it);
    return true;
}

bool PhysicalPrinterCollection::delete_selected_printer()
{
    if (!has_selection())
        return false;
    const PhysicalPrinter& printer = this->get_selected_printer();

    // Erase the preset file.
    boost::nowide::remove(printer.file.c_str());
    // Remove the preset from the list.
    m_printers.erase(m_printers.begin() + m_idx_selected);
    // unselect all printers
    unselect_printer();

    return true;
}

bool PhysicalPrinterCollection::delete_preset_from_printers( const std::string& preset_name)
{
    std::vector<std::string> printers_for_delete;
    for (PhysicalPrinter& printer : m_printers) {
        if (printer.preset_names.size() == 1 && *printer.preset_names.begin() == preset_name)
            printers_for_delete.emplace_back(printer.name);
        else if (printer.delete_preset(preset_name))
            save_printer(printer);
    }

    if (!printers_for_delete.empty())
        for (const std::string& printer_name : printers_for_delete)
            delete_printer(printer_name);

    unselect_printer();
    return true;
}

// Get list of printers which have more than one preset and "preset_names" preset is one of them
std::vector<std::string> PhysicalPrinterCollection::get_printers_with_preset(const std::string& preset_name)
{
    std::vector<std::string> printers;

    for (auto printer : m_printers) {
        if (printer.preset_names.size() == 1)
            continue;
        if (printer.preset_names.find(preset_name) != printer.preset_names.end())
            printers.emplace_back(printer.name);
    }

    return printers;
}

// Get list of printers which has only "preset_names" preset
std::vector<std::string> PhysicalPrinterCollection::get_printers_with_only_preset(const std::string& preset_name)
{
    std::vector<std::string> printers;

    for (auto printer : m_printers)
        if (printer.preset_names.size() == 1 && *printer.preset_names.begin() == preset_name)
            printers.emplace_back(printer.name);

    return printers;
}

std::string PhysicalPrinterCollection::get_selected_full_printer_name() const
{
    return (m_idx_selected == size_t(-1)) ? std::string() : this->get_selected_printer().get_full_name(m_selected_preset);
}

void PhysicalPrinterCollection::select_printer(const std::string& full_name)
{
    std::string printer_name = PhysicalPrinter::get_short_name(full_name);
    auto it = this->find_printer_internal(printer_name);
    if (it == m_printers.end()) {
        unselect_printer();
        return;
    }

    // update idx_selected
    m_idx_selected = it - m_printers.begin();

    // update name of the currently selected preset
    if (printer_name == full_name)
        // use first preset in the list
        m_selected_preset = *it->preset_names.begin();
    else
        m_selected_preset = it->get_preset_name(full_name);
}

void PhysicalPrinterCollection::select_printer(const std::string& printer_name, const std::string& preset_name)
{
    if (preset_name.empty())
        return select_printer(printer_name);
    return select_printer(printer_name + PhysicalPrinter::separator() + preset_name);
}

void PhysicalPrinterCollection::select_printer(const PhysicalPrinter& printer)
{
    return select_printer(printer.name);
}

bool PhysicalPrinterCollection::has_selection() const
{
    return m_idx_selected != size_t(-1);
}

void PhysicalPrinterCollection::unselect_printer()
{
    m_idx_selected = size_t(-1);
    m_selected_preset.clear();
}

bool PhysicalPrinterCollection::is_selected(PhysicalPrinterCollection::ConstIterator it, const std::string& preset_name) const
{
    return  m_idx_selected      == size_t(it - m_printers.begin()) &&
            m_selected_preset   == preset_name;
}


namespace PresetUtils {
	const VendorProfile::PrinterModel* system_printer_model(const Preset &preset)
	{
		const VendorProfile::PrinterModel *out = nullptr;
		if (preset.vendor != nullptr) {
			auto *printer_model = preset.config.opt<ConfigOptionString>("printer_model");
			if (printer_model != nullptr && ! printer_model->value.empty()) {
				auto it = std::find_if(preset.vendor->models.begin(), preset.vendor->models.end(), [printer_model](const VendorProfile::PrinterModel &pm) { return pm.id == printer_model->value; });
				if (it != preset.vendor->models.end())
					out = &(*it);
			}
		}
		return out;
	}

    std::string system_printer_bed_model(const Preset& preset)
    {
        std::string out;
        const VendorProfile::PrinterModel* pm = PresetUtils::system_printer_model(preset);
        if (pm != nullptr && !pm->bed_model.empty()) {
            out = Slic3r::data_dir() + "/vendor/" + preset.vendor->id + "/" + pm->bed_model;
            if (!boost::filesystem::exists(boost::filesystem::path(out)))
                out = Slic3r::resources_dir() + "/profiles/" + preset.vendor->id + "/" + pm->bed_model;
        }
        return out;
    }

    std::string system_printer_bed_texture(const Preset& preset)
    {
        std::string out;
        const VendorProfile::PrinterModel* pm = PresetUtils::system_printer_model(preset);
        if (pm != nullptr && !pm->bed_texture.empty()) {
            out = Slic3r::data_dir() + "/vendor/" + preset.vendor->id + "/" + pm->bed_texture;
            if (!boost::filesystem::exists(boost::filesystem::path(out)))
                out = Slic3r::resources_dir() + "/profiles/" + preset.vendor->id + "/" + pm->bed_texture;
        }
        return out;
    }

    std::string system_printer_hotend_model(const Preset& preset)
    {
        std::string out;
        const VendorProfile::PrinterModel* pm = PresetUtils::system_printer_model(preset);
        if (pm != nullptr && !pm->hotend_model.empty()) {
            out = Slic3r::data_dir() + "/vendor/" + preset.vendor->id + "/" + pm->hotend_model;
            if (!boost::filesystem::exists(boost::filesystem::path(out)))
                out = Slic3r::resources_dir() + "/profiles/" + preset.vendor->id + "/" + pm->hotend_model;
        }
        
        if (out.empty() ||!boost::filesystem::exists(boost::filesystem::path(out)))
            out = Slic3r::resources_dir() + "/profiles/hotend.stl";
        return out;
    }
} // namespace PresetUtils

} // namespace Slic3r
