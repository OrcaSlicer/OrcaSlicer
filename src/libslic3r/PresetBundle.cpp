#include <cassert>

#include "PresetBundle.hpp"
#include "PrintConfig.hpp"
#include "libslic3r.h"
#include "I18N.hpp"
#include "Utils.hpp"
#include "Model.hpp"
#include "format.hpp"
#include "libslic3r_version.h"

#include <algorithm>
#include <set>
#include <fstream>
#include <unordered_set>
#include <boost/filesystem.hpp>
#include <boost/algorithm/clamp.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/locale.hpp>
#include <boost/log/trivial.hpp>
#include <miniz/miniz.h>

// Mark string for localization and translate.
#define L(s) Slic3r::I18N::translate(s)

// Store the print/filament/printer presets into a "presets" subdirectory of the Slic3rPE config dir.
// This breaks compatibility with the upstream Slic3r if the --datadir is used to switch between the two versions.
//#define SLIC3R_PROFILE_USE_PRESETS_SUBDIR

namespace Slic3r {

static std::vector<std::string> s_project_options {
    "flush_volumes_vector",
    "flush_volumes_matrix",
    // BBS
    "filament_colour",
    "filament_colour_type",
    "filament_multi_colour",
    "wipe_tower_x",
    "wipe_tower_y",
    "wipe_tower_rotation_angle",
    "curr_bed_type",
    "flush_multiplier",
    "nozzle_volume_type",
    "filament_map_mode",
    "filament_map"
};

//Orca: add custom as default
const char *PresetBundle::ORCA_DEFAULT_BUNDLE = "Custom";
const char *PresetBundle::ORCA_DEFAULT_PRINTER_MODEL = "MyKlipper 0.4 nozzle";
const char *PresetBundle::ORCA_DEFAULT_PRINTER_VARIANT = "0.4";
const char *PresetBundle::ORCA_DEFAULT_FILAMENT = "Generic PLA @System";
const char *PresetBundle::ORCA_FILAMENT_LIBRARY = "OrcaFilamentLibrary";

DynamicPrintConfig PresetBundle::construct_full_config(
    Preset& in_printer_preset,
    Preset& in_print_preset,
    const DynamicPrintConfig& project_config,
    std::vector<Preset>& in_filament_presets,
    bool apply_extruder,
    std::optional<std::vector<int>> filament_maps_new)
{
    DynamicPrintConfig &printer_config = in_printer_preset.config;
    DynamicPrintConfig &print_config   = in_print_preset.config;

    DynamicPrintConfig out;
    out.apply(FullPrintConfig::defaults());
    out.apply(printer_config);
    out.apply(print_config);
    out.apply(project_config);
    out.apply(in_filament_presets[0].config);

    size_t num_filaments = in_filament_presets.size();

    std::vector<int> filament_maps = out.option<ConfigOptionInts>("filament_map")->values;
    if (filament_maps_new.has_value())
        filament_maps = *filament_maps_new;
    // in some middle state, they may be different
    if (filament_maps.size() != num_filaments) {
        filament_maps.resize(num_filaments, 1);
    }

    auto *extruder_diameter = dynamic_cast<const ConfigOptionFloats *>(out.option("nozzle_diameter"));
    // Collect the "compatible_printers_condition" and "inherits" values over all presets (print, filaments, printers) into a single vector.
    std::vector<std::string> compatible_printers_condition;
    std::vector<std::string> compatible_prints_condition;
    std::vector<std::string> inherits;
    std::vector<std::string> filament_ids;
    std::vector<std::string> print_compatible_printers;
    // BBS: add logic for settings check between different system presets
    std::vector<std::string> different_settings;
    std::string              different_print_settings, different_printer_settings;
    compatible_printers_condition.emplace_back(in_print_preset.compatible_printers_condition());

    const ConfigOptionStrings *compatible_printers = print_config.option<ConfigOptionStrings>("compatible_printers", false);
    if (compatible_printers) print_compatible_printers = compatible_printers->values;
    // BBS: add logic for settings check between different system presets
    std::string print_inherits = in_print_preset.inherits();
    inherits.emplace_back(print_inherits);

    // BBS: update printer config related with variants
    if (apply_extruder) {
        out.update_values_to_printer_extruders(out, printer_options_with_variant_1, "printer_extruder_id", "printer_extruder_variant");
        out.update_values_to_printer_extruders(out, printer_options_with_variant_2, "printer_extruder_id", "printer_extruder_variant", 2);
        // update print config related with variants
        out.update_values_to_printer_extruders(out, print_options_with_variant, "print_extruder_id", "print_extruder_variant");
    }

    if (num_filaments <= 1) {
        // BBS: update filament config related with variants
        DynamicPrintConfig filament_config = in_filament_presets[0].config;
        if (apply_extruder) filament_config.update_values_to_printer_extruders(out, filament_options_with_variant, "", "filament_extruder_variant", 1, filament_maps[0]);
        out.apply(filament_config);
        compatible_printers_condition.emplace_back(in_filament_presets[0].compatible_printers_condition());
        compatible_prints_condition.emplace_back(in_filament_presets[0].compatible_prints_condition());
        std::string filament_inherits = in_filament_presets[0].inherits();
        inherits.emplace_back(filament_inherits);
        filament_ids.emplace_back(in_filament_presets[0].filament_id);

        std::vector<int> &filament_self_indice = out.option<ConfigOptionInts>("filament_self_index", true)->values;
        int               index_size           = out.option<ConfigOptionStrings>("filament_extruder_variant")->size();
        filament_self_indice.resize(index_size, 1);
    } else {
        std::vector<const DynamicPrintConfig *> filament_configs;
        std::vector<const Preset *>             filament_presets;
        for (const Preset & preset : in_filament_presets) {
            filament_presets.emplace_back(&preset);
            filament_configs.emplace_back(&(preset.config));
        }

        std::vector<DynamicPrintConfig> filament_temp_configs;
        filament_temp_configs.resize(num_filaments);
        for (size_t i = 0; i < num_filaments; ++i) {
            filament_temp_configs[i] = *(filament_configs[i]);
            if (apply_extruder)
                filament_temp_configs[i].update_values_to_printer_extruders(out, filament_options_with_variant, "", "filament_extruder_variant", 1, filament_maps[i]);
        }

        // loop through options and apply them to the resulting config.
        std::vector<int> filament_variant_count(num_filaments, 1);
        for (const t_config_option_key &key : in_filament_presets[0].config.keys()) {
            if (key == "compatible_prints" || key == "compatible_printers") continue;
            // Get a destination option.
            ConfigOption *opt_dst = out.option(key, false);
            if (opt_dst->is_scalar()) {
                // Get an option, do not create if it does not exist.
                const ConfigOption *opt_src = filament_temp_configs.front().option(key);
                if (opt_src != nullptr) opt_dst->set(opt_src);
            } else {
                // BBS
                ConfigOptionVectorBase *opt_vec_dst = static_cast<ConfigOptionVectorBase *>(opt_dst);
                {
                    if (apply_extruder) {
                        std::vector<const ConfigOption *> filament_opts(num_filaments, nullptr);
                        // Setting a vector value from all filament_configs.
                        for (size_t i = 0; i < filament_opts.size(); ++i) filament_opts[i] = filament_temp_configs[i].option(key);
                        opt_vec_dst->set(filament_opts);
                    } else {
                        for (size_t i = 0; i < num_filaments; ++i) {
                            const ConfigOptionVectorBase *filament_option = static_cast<const ConfigOptionVectorBase *>(filament_temp_configs[i].option(key));
                            if (i == 0)
                                opt_vec_dst->set(filament_option);
                            else
                                opt_vec_dst->append(filament_option);

                            if (key == "filament_extruder_variant") filament_variant_count[i] = filament_option->size();
                        }
                    }
                }
            }
        }

        if (!apply_extruder) {
            // append filament_self_index
            std::vector<int> &filament_self_indice = out.option<ConfigOptionInts>("filament_self_index", true)->values;
            int               index_size           = out.option<ConfigOptionStrings>("filament_extruder_variant")->size();
            filament_self_indice.resize(index_size, 1);
            int k = 0;
            for (size_t i = 0; i < num_filaments; i++) {
                for (size_t j = 0; j < filament_variant_count[i]; j++) { filament_self_indice[k++] = i + 1; }
            }
        }
    }

    // These value types clash between the print and filament profiles. They should be renamed.
    out.erase("compatible_prints");
    out.erase("compatible_prints_condition");
    out.erase("compatible_printers");
    out.erase("compatible_printers_condition");
    out.erase("inherits");
    // BBS: add logic for settings check between different system presets
    out.erase("different_settings_to_system");

    static const char *keys[] = {"support_filament", "support_interface_filament"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        std::string key = std::string(keys[i]);
        auto       *opt = dynamic_cast<ConfigOptionInt *>(out.option(key, false));
        assert(opt != nullptr);
        opt->value = boost::algorithm::clamp<int>(opt->value, 0, int(num_filaments));
    }

    std::vector<std::string> filamnet_preset_names;
    for (auto preset : in_filament_presets) {
        filamnet_preset_names.emplace_back(preset.name);
    }
    out.option<ConfigOptionString>("print_settings_id", true)->value      = in_print_preset.name;
    out.option<ConfigOptionStrings>("filament_settings_id", true)->values = filamnet_preset_names;
    out.option<ConfigOptionString>("printer_settings_id", true)->value    = in_printer_preset.name;
    out.option<ConfigOptionStrings>("filament_ids", true)->values         = filament_ids;
    out.option<ConfigOptionInts>("filament_map", true)->values            = filament_maps;

    auto add_if_some_non_empty = [&out](std::vector<std::string> &&values, const std::string &key) {
        bool nonempty = false;
        for (const std::string &v : values)
            if (!v.empty()) {
                nonempty = true;
                break;
            }
        if (nonempty) out.set_key_value(key, new ConfigOptionStrings(std::move(values)));
    };
    add_if_some_non_empty(std::move(compatible_printers_condition), "compatible_machine_expression_group");
    add_if_some_non_empty(std::move(compatible_prints_condition), "compatible_process_expression_group");
    add_if_some_non_empty(std::move(inherits), "inherits_group");
    // BBS: add logic for settings check between different system presets
    //add_if_some_non_empty(std::move(different_settings), "different_settings_to_system");
    add_if_some_non_empty(std::move(print_compatible_printers), "print_compatible_printers");

    out.option<ConfigOptionEnumGeneric>("printer_technology", true)->value = ptFFF;
    return out;
}

PresetBundle::PresetBundle()
    : prints(Preset::TYPE_PRINT, Preset::print_options(), static_cast<const PrintRegionConfig &>(FullPrintConfig::defaults()))
    , filaments(Preset::TYPE_FILAMENT, Preset::filament_options(), static_cast<const PrintRegionConfig &>(FullPrintConfig::defaults()), "Default Filament")
    , sla_materials(Preset::TYPE_SLA_MATERIAL, Preset::sla_material_options(), static_cast<const SLAMaterialConfig &>(SLAFullPrintConfig::defaults()))
    , sla_prints(Preset::TYPE_SLA_PRINT, Preset::sla_print_options(), static_cast<const SLAPrintObjectConfig &>(SLAFullPrintConfig::defaults()))
    , printers(Preset::TYPE_PRINTER, Preset::printer_options(), static_cast<const PrintRegionConfig &>(FullPrintConfig::defaults()), "Default Printer")
    , physical_printers(PhysicalPrinter::printer_options())
{
    // The following keys are handled by the UI, they do not have a counterpart in any StaticPrintConfig derived classes,
    // therefore they need to be handled differently. As they have no counterpart in StaticPrintConfig, they are not being
    // initialized based on PrintConfigDef(), but to empty values (zeros, empty vectors, empty strings).
    //
    // "compatible_printers", "compatible_printers_condition", "inherits",
    // "print_settings_id", "filament_settings_id", "printer_settings_id", "printer_settings_id"
    // "printer_model", "printer_variant", "default_print_profile", "default_filament_profile"

    // Create the ID config keys, as they are not part of the Static print config classes.
    this->prints.default_preset().config.optptr("print_settings_id", true);
    this->prints.default_preset().compatible_printers_condition();
    this->prints.default_preset().inherits();

    this->filaments.default_preset().config.option<ConfigOptionStrings>("filament_settings_id", true)->values = {""};
    this->filaments.default_preset().compatible_printers_condition();
    this->filaments.default_preset().inherits();
    // Set all the nullable values to nils.
    {
        auto& default_config = this->filaments.default_preset().config;
        for(const std::string& opt_key : default_config.keys()){
            ConfigOption* opt = default_config.optptr(opt_key, false);
            bool is_override_key = std::find(filament_extruder_override_keys.begin(),filament_extruder_override_keys.end(), opt_key) != filament_extruder_override_keys.end();
            if(!is_override_key || !opt->nullable()) 
                continue;
            opt->deserialize("nil",ForwardCompatibilitySubstitutionRule::Disable);
        }
    }

    this->sla_materials.default_preset().config.optptr("sla_material_settings_id", true);
    this->sla_materials.default_preset().compatible_printers_condition();
    this->sla_materials.default_preset().inherits();

    this->sla_prints.default_preset().config.optptr("sla_print_settings_id", true);
    this->sla_prints.default_preset().config.opt_string("filename_format", true) = "[input_filename_base].sl1";
    this->sla_prints.default_preset().compatible_printers_condition();
    this->sla_prints.default_preset().inherits();

    //this->printers.add_default_preset(Preset::sla_printer_options(), static_cast<const SLAMaterialConfig &>(SLAFullPrintConfig::defaults()), "- default SLA -");
    //this->printers.preset(1).printer_technology_ref() = ptSLA;
    for (size_t i = 0; i < 1; ++i) {
        // The following ugly switch is to avoid printers.preset(0) to return the edited instance, as the 0th default is the current one.
        Preset &preset = this->printers.default_preset(i);
        for (const char *key : {"printer_settings_id", "printer_model", "printer_variant", "thumbnails"}) preset.config.optptr(key, true);
        //if (i == 0) {
            preset.config.optptr("default_print_profile", true);
            preset.config.option<ConfigOptionStrings>("default_filament_profile", true);
        //} else {
        //    preset.config.optptr("default_sla_print_profile", true);
        //    preset.config.optptr("default_sla_material_profile", true);
        //}
        // default_sla_material_profile
        preset.inherits();
    }

    // Re-activate the default presets, so their "edited" preset copies will be updated with the additional configuration values above.
    this->prints.select_preset(0);
    this->sla_prints.select_preset(0);
    this->filaments.select_preset(0);
    this->sla_materials.select_preset(0);
    this->printers.select_preset(0);

    this->project_config.apply_only(FullPrintConfig::defaults(), s_project_options);
}

PresetBundle::PresetBundle(const PresetBundle &rhs)
{
    *this = rhs;
}

PresetBundle& PresetBundle::operator=(const PresetBundle &rhs)
{
    prints              = rhs.prints;
    sla_prints          = rhs.sla_prints;
    filaments           = rhs.filaments;
    sla_materials       = rhs.sla_materials;
    printers            = rhs.printers;
    physical_printers   = rhs.physical_printers;

    filament_presets    = rhs.filament_presets;
    project_config      = rhs.project_config;
    vendors             = rhs.vendors;
    obsolete_presets    = rhs.obsolete_presets;
    m_errors    = rhs.m_errors;

    // Adjust Preset::vendor pointers to point to the copied vendors map.
    prints       .update_vendor_ptrs_after_copy(this->vendors);
    sla_prints   .update_vendor_ptrs_after_copy(this->vendors);
    filaments    .update_vendor_ptrs_after_copy(this->vendors);
    sla_materials.update_vendor_ptrs_after_copy(this->vendors);
    printers     .update_vendor_ptrs_after_copy(this->vendors);

    return *this;
}

void PresetBundle::reset(bool delete_files)
{
    // Clear the existing presets, delete their respective files.
    this->vendors.clear();
    this->prints       .reset(delete_files);
    this->sla_prints   .reset(delete_files);
    this->filaments    .reset(delete_files);
    this->sla_materials.reset(delete_files);
    this->printers     .reset(delete_files);
    // BBS: filament_presets is load from project config, not handled here
    //this->filament_presets.clear();
    if (this->filament_presets.empty())
        this->filament_presets.emplace_back(this->filaments.get_selected_preset_name());
    this->obsolete_presets.prints.clear();
    this->obsolete_presets.sla_prints.clear();
    this->obsolete_presets.filaments.clear();
    this->obsolete_presets.sla_materials.clear();
    this->obsolete_presets.printers.clear();
}

void PresetBundle::setup_directories()
{
    boost::filesystem::path data_dir = boost::filesystem::path(Slic3r::data_dir());
    //BBS: change directoties by design
    std::initializer_list<boost::filesystem::path> paths = {
        data_dir,
        data_dir / "ota",
		data_dir / PRESET_SYSTEM_DIR,
        data_dir / PRESET_USER_DIR,
        // Store the print/filament/printer presets at the same location as the upstream Slic3r.
        //data_dir / PRESET_SYSTEM_DIR / PRESET_PRINT_NAME,
        //data_dir / PRESET_SYSTEM_DIR / PRESET_FILAMENT_NAME,
        //data_dir / PRESET_SYSTEM_DIR / PRESET_PRINTER_NAME
    };
    for (const boost::filesystem::path &path : paths) {
		boost::filesystem::path subdir = path;
        subdir.make_preferred();
        if (! boost::filesystem::is_directory(subdir) &&
            ! boost::filesystem::create_directory(subdir)) {
            if (boost::filesystem::is_directory(subdir)) {
                BOOST_LOG_TRIVIAL(warning) << boost::format("creating directory %1% failed, maybe created by other instance, go on!")%subdir.string();
            }
            else
                throw Slic3r::RuntimeError(std::string("Unable to create directory ") + subdir.string());
        }
    }
}

// recursively copy all files and dirs in from_dir to to_dir
static void copy_dir(const boost::filesystem::path& from_dir, const boost::filesystem::path& to_dir)
{
    if(!boost::filesystem::is_directory(from_dir))
        return;
    // i assume to_dir.parent surely exists
    if (!boost::filesystem::is_directory(to_dir))
        boost::filesystem::create_directory(to_dir);
    for (auto& dir_entry : boost::filesystem::directory_iterator(from_dir)) {
        if (!boost::filesystem::is_directory(dir_entry.path())) {
            std::string em;
            CopyFileResult cfr = copy_file(dir_entry.path().string(), (to_dir / dir_entry.path().filename()).string(), em, false);
            if (cfr != SUCCESS) {
                BOOST_LOG_TRIVIAL(error) << "Error when copying files from " << from_dir << " to " << to_dir << ": " << em;
            }
        } else {
            copy_dir(dir_entry.path(), to_dir / dir_entry.path().filename());
        }
    }
}

void PresetBundle::copy_files(const std::string& from)
{
    boost::filesystem::path data_dir = boost::filesystem::path(Slic3r::data_dir());
    // list of searched paths based on current directory system in setup_directories()
    // do not copy cache and snapshots
    boost::filesystem::path from_data_dir = boost::filesystem::path(from);
    //BBS: change directoties by design
    std::initializer_list<boost::filesystem::path> from_dirs= {
        //from_data_dir / "vendor",
        // Store the print/filament/printer presets at the same location as the upstream Slic3r.
        from_data_dir / PRESET_PRINT_NAME,
        from_data_dir / PRESET_FILAMENT_NAME,
        from_data_dir / PRESET_PRINTER_NAME
    };
    // copy recursively all files
    //BBS: change directoties by design
    for (const boost::filesystem::path& from_dir : from_dirs) {
        copy_dir(from_dir, data_dir /"old"/from_dir.filename());
    }
}

PresetsConfigSubstitutions PresetBundle::load_presets(AppConfig &config, ForwardCompatibilitySubstitutionRule substitution_rule,
                                                      const PresetPreferences& preferred_selection/* = PresetPreferences()*/)
{
    // First load the vendor specific system presets.
    PresetsConfigSubstitutions substitutions;
    std::string errors_cummulative;

    //BBS: add config related logs
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" enter, substitution_rule %1%, preferred printer_model_id %2%")%substitution_rule%preferred_selection.printer_model_id;
    //BBS: change system config to json
    std::tie(substitutions, errors_cummulative) = this->load_system_presets_from_json(substitution_rule);

    // BBS load preset from user's folder, load system default if
    // BBS: change directories by design
    std::string dir_user_presets = config.get("preset_folder");
    if (dir_user_presets.empty()) {
        load_user_presets(DEFAULT_USER_FOLDER_NAME, substitution_rule);
    } else {
        load_user_presets(dir_user_presets, substitution_rule);
    }

    this->update_multi_material_filament_presets();
    this->update_compatible(PresetSelectCompatibleType::Never);

    this->load_selections(config, preferred_selection);

    set_calibrate_printer("");

    //BBS: add config related logs
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" finished, returned substitutions %1%")%substitutions.size();
    return substitutions;
}

//BBS: add function to generate differed preset for save
//the pointer should be freed by the caller
Preset* PresetBundle::get_preset_differed_for_save(Preset& preset)
{
    PresetCollection* preset_collection;

    switch(preset.type) {
        case Preset::TYPE_PRINT:
            preset_collection = &(this->prints);
            break;
        case Preset::TYPE_PRINTER:
            preset_collection = &(this->printers);
            break;
        case Preset::TYPE_FILAMENT:
            preset_collection = &(this->filaments);
            break;
        default:
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" invalid type %1%, return directly")%preset.type;
            return nullptr;
    }

    return preset_collection->get_preset_differed_for_save(preset);
}

int PresetBundle::get_differed_values_to_update(Preset& preset, std::map<std::string, std::string>& key_values)
{
    PresetCollection* preset_collection;

    switch(preset.type) {
        case Preset::TYPE_PRINT:
            preset_collection = &(this->prints);
            break;
        case Preset::TYPE_PRINTER:
            preset_collection = &(this->printers);
            break;
        case Preset::TYPE_FILAMENT:
            preset_collection = &(this->filaments);
            break;
        default:
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" invalid type %1%, return directly")%preset.type;
            return -1;
    }

    return preset_collection->get_differed_values_to_update(preset, key_values);
}

//BBS: get vendor's current version
Semver PresetBundle::get_vendor_profile_version(std::string vendor_name)
{
    Semver result_ver;

    auto vendor_profile = vendors.find(vendor_name);
    if (vendor_profile != vendors.end()) {
        result_ver = vendor_profile->second.config_version;
    }

    return result_ver;
}

VendorType PresetBundle::get_current_vendor_type()
{
    auto        t      = VendorType::Unknown;
    auto        config = &printers.get_edited_preset().config;
    std::string vendor_name;
    for (auto vendor_profile : vendors) {
        for (auto vendor_model : vendor_profile.second.models)
            if (vendor_model.name == config->opt_string("printer_model")) {
                vendor_name = vendor_profile.first;
                break;
            }
    }
    if (!vendor_name.empty())
    {
        if(vendor_name.compare("BBL") == 0)
            t = VendorType::Marlin_BBL;
        
        if(vendor_name.compare("Qidi") == 0)
            t = VendorType::Klipper_Qidi;
    }
    return t;
}

bool PresetBundle::use_bbl_network()
{
    const auto cfg             = printers.get_edited_preset().config;
    const bool use_bbl_network = is_bbl_vendor() && !cfg.opt_bool("bbl_use_printhost");
    return use_bbl_network;
}

bool PresetBundle::use_bbl_device_tab() {
    if (!is_bbl_vendor()) {
        return false;
    }

    if (use_bbl_network()) {
        return true;
    }

    const auto cfg = printers.get_edited_preset().config;
    // Use bbl device tab if printhost webui url is not set 
    return cfg.opt_string("print_host_webui").empty();
}

bool PresetBundle::backup_user_folder() const
{
    const std::string backup_folderpath = data_dir() + "/" + (boost::format("user_backup-v%1%") % SoftFever_VERSION).str();

    // Check if backup file already exists
    if (boost::filesystem::exists(boost::filesystem::path(backup_folderpath)))
        return false;

    BOOST_LOG_TRIVIAL(info) << "Backing up user folder to: " << backup_folderpath;
    try {
        // Copy the user folder to the backup folder
        boost::filesystem::copy(data_dir() + "/" + PRESET_USER_DIR, backup_folderpath, boost::filesystem::copy_options::recursive);
        BOOST_LOG_TRIVIAL(info) << "User folder backup completed successfully";
        return true;
    } catch (const std::exception& ex) {
        BOOST_LOG_TRIVIAL(error) << "Exception during user folder backup: " << ex.what();
        // Try to clean up partially copied backup folder
        if (boost::filesystem::exists(boost::filesystem::path(backup_folderpath)))
            boost::filesystem::remove_all(boost::filesystem::path(backup_folderpath));
        return false;
    }
}

std::optional<FilamentBaseInfo> PresetBundle::get_filament_by_filament_id(const std::string& filament_id, const std::string& printer_name) const
{
    if (filament_id.empty())
        return std::nullopt;

    // basic filament info should be same in the parent preset and child preset
    // so just match the filament id is enough

    for (auto iter = filaments.begin(); iter != filaments.end(); ++iter) {
        const Preset& filament_preset = *iter;
        const auto& config = filament_preset.config;
        if (filament_preset.filament_id == filament_id) {
            FilamentBaseInfo info;
            info.filament_id = filament_id;
            info.is_system = filament_preset.is_system;
            info.filament_name = filament_preset.alias;
            if (config.has("filament_is_support"))
                info.is_support = config.option<ConfigOptionBools>("filament_is_support")->values[0];
            if (config.has("filament_type"))
                info.filament_type = config.option<ConfigOptionStrings>("filament_type")->values[0];
            if (config.has("filament_vendor"))
                info.vendor = config.option<ConfigOptionStrings>("filament_vendor")->values[0];
            if (config.has("nozzle_temperature_range_high"))
                info.nozzle_temp_range_high = config.option<ConfigOptionInts>("nozzle_temperature_range_high")->values[0];
            if (config.has("nozzle_temperature_range_low"))
                info.nozzle_temp_range_low = config.option<ConfigOptionInts>("nozzle_temperature_range_low")->values[0];
            if(config.has("temperature_vitrification"))
                info.temperature_vitrification = config.option<ConfigOptionInts>("temperature_vitrification")->values[0];

            if (!printer_name.empty()) {
                std::vector<std::string> compatible_printers = config.option<ConfigOptionStrings>("compatible_printers")->values;
                auto iter = std::find(compatible_printers.begin(), compatible_printers.end(), printer_name);
                if (iter != compatible_printers.end() && config.has("filament_printable")) {
                    info.filament_printable = config.option<ConfigOptionInts>("filament_printable")->values[0];
                    return info;
                }
            }
            else {
                return info;
            }
        }
    }
    return std::nullopt;
}

//BBS: load project embedded presets
PresetsConfigSubstitutions PresetBundle::load_project_embedded_presets(std::vector<Preset*> project_presets, ForwardCompatibilitySubstitutionRule substitution_rule)
{
    // First load the vendor specific system presets.
    PresetsConfigSubstitutions substitutions;
    std::string errors_cummulative;

    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" enter, substitution_rule %1%, preset toltal count %2%")%substitution_rule% project_presets.size();
    try {
        this->prints.load_project_embedded_presets(project_presets, PRESET_PRINT_NAME, substitutions, substitution_rule);
    } catch (const std::runtime_error &err) {
        errors_cummulative += err.what();
    }
    try {
        this->filaments.load_project_embedded_presets(project_presets, PRESET_FILAMENT_NAME, substitutions, substitution_rule);
    } catch (const std::runtime_error &err) {
        errors_cummulative += err.what();
    }
    try {
        this->printers.load_project_embedded_presets(project_presets, PRESET_PRINTER_NAME, substitutions, substitution_rule);
    } catch (const std::runtime_error &err) {
        errors_cummulative += err.what();
    }

    //this->update_multi_material_filament_presets();
    //this->update_compatible(PresetSelectCompatibleType::Never);
    if (! errors_cummulative.empty())
        throw Slic3r::RuntimeError(errors_cummulative);

    //this->load_selections(config, "");

    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" finished, returned substitutions %1%")%substitutions.size();
    return substitutions;
}

//BBS: get current project embedded presets
std::vector<Preset*> PresetBundle::get_current_project_embedded_presets()
{
    std::vector<Preset*> project_presets;

    project_presets = this->prints.get_project_embedded_presets();

    auto filament_presets = this->filaments.get_project_embedded_presets();
    if (!filament_presets.empty())
        std::copy(filament_presets.begin(), filament_presets.end(), std::back_inserter(project_presets));
    auto printer_presets = this->printers.get_project_embedded_presets();
    if (!printer_presets.empty())
        std::copy(printer_presets.begin(), printer_presets.end(), std::back_inserter(project_presets));

    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" finished, returned project_presets count %1%")%project_presets.size();
    return project_presets;
}

//BBS: reset project embedded presets
void PresetBundle::reset_project_embedded_presets()
{
    std::string prefer_printer;
    Preset& current_printer = this->printers.get_selected_preset();
    ConfigOption* inherits = current_printer.config.option("inherits");
    if (inherits) {
        prefer_printer = dynamic_cast<ConfigOptionString *>(inherits)->value;
    }
    //first printer, then filament, then print
    bool printer_reselect = this->printers.reset_project_embedded_presets();
    bool filament_reselect = this->filaments.reset_project_embedded_presets();
    bool print_reselect = this->prints.reset_project_embedded_presets();

    if (printer_reselect) {
        if (!prefer_printer.empty())
           this->printers.select_preset_by_name(prefer_printer, true);
        else
           this->printers.select_preset(this->printers.first_visible_idx());

        //this->update_multi_material_filament_presets();
        this->update_compatible(PresetSelectCompatibleType::Never);
    }
    else if (filament_reselect || print_reselect) {
        //Preset& current_printer = this->printers.get_selected_preset();
        /*if (filament_reselect) {
            const std::vector<std::string> &prefered_filament_profiles = current_printer.config.option<ConfigOptionStrings>("default_filament_profile")->values;
            const std::string prefered_filament_profile = prefered_filament_profiles.empty() ? std::string() : prefered_filament_profiles.front();
            if (!prefered_filament_profile.empty())
               this->filaments.select_preset_by_name(prefered_filament_profile, true);
            else
               this->filaments.select_preset(this->filaments.first_visible_idx());
        }

        if (print_reselect) {
        }*/
        this->update_compatible(PresetSelectCompatibleType::Never);
    }

    //this->update_multi_material_filament_presets();

    //update filament_presets
    for (size_t i = 0; i < filament_presets.size(); ++ i)
    {
        Preset* selected_filament = this->filaments.find_preset(filament_presets[i], false);
        if (!selected_filament) {
            //it should be the project embedded presets
            Preset& current_printer = this->printers.get_selected_preset();
            const std::vector<std::string> &prefered_filament_profiles = current_printer.config.option<ConfigOptionStrings>("default_filament_profile")->values;
            const std::string prefered_filament_profile = prefered_filament_profiles.empty() ? std::string() : prefered_filament_profiles.front();
            if (!prefered_filament_profile.empty())
                filament_presets[i] = prefered_filament_profile;
            else
            filament_presets[i] = this->filaments.first_visible().name;
        }
    }
}

//BBS: get bed texture for printer model
std::string PresetBundle::get_texture_for_printer_model(std::string model_name)
{
    std::string texture_name, vendor_name, out;

    for (auto vendor_profile: this->vendors)
    {
        for (auto vendor_model: vendor_profile.second.models)
        {
            if (vendor_model.name == model_name || vendor_model.id == model_name)
            {
                texture_name = vendor_model.bed_texture;
                vendor_name = vendor_profile.first;
                break;
            }
        }
    }

    if (!texture_name.empty())
    {
        out = Slic3r::data_dir() + "/vendor/" + vendor_name + "/" + texture_name;
        if (!boost::filesystem::exists(boost::filesystem::path(out)))
            out = Slic3r::resources_dir() + "/profiles/" + vendor_name + "/" + texture_name;
    }

    return out;
}

//BBS: get stl model for printer model
std::string PresetBundle::get_stl_model_for_printer_model(std::string model_name)
{
    std::string stl_name, vendor_name, out;

    for (auto vendor_profile: this->vendors)
    {
        for (auto vendor_model: vendor_profile.second.models)
        {
            if (vendor_model.name == model_name)
            {
                stl_name = vendor_model.bed_model;
                vendor_name = vendor_profile.first;
                break;
            }
        }
    }

    if (!stl_name.empty())
    {
        out = Slic3r::data_dir() + "/vendor/" + vendor_name + "/" + stl_name;
        if (!boost::filesystem::exists(boost::filesystem::path(out)))
            out = Slic3r::resources_dir() + "/profiles/" + vendor_name + "/" + stl_name;
    }

    return out;
}

std::string PresetBundle::get_hotend_model_for_printer_model(std::string model_name)
{
    std::string hotend_stl, vendor_name, out;

    for (auto vendor_profile: this->vendors)
    {
        for (auto vendor_model: vendor_profile.second.models)
        {
            if (vendor_model.name == model_name)
            {
                hotend_stl = vendor_model.hotend_model;
                vendor_name = vendor_profile.first;
                break;
            }
        }
    }

    if (!hotend_stl.empty())
    {
        out = Slic3r::data_dir() + "/vendor/" + vendor_name + "/" + hotend_stl;
        if (!boost::filesystem::exists(boost::filesystem::path(out)))
            out = Slic3r::resources_dir() + "/profiles/" + vendor_name + "/" + hotend_stl;
    }

    if (out.empty() ||!boost::filesystem::exists(boost::filesystem::path(out)))
        out = Slic3r::resources_dir() + "/profiles/hotend.stl";

    return out;
}

PresetsConfigSubstitutions PresetBundle::load_user_presets(std::string user, ForwardCompatibilitySubstitutionRule substitution_rule)
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << __LINE__ << " entry and user is: " << user;
    PresetsConfigSubstitutions substitutions;
    std::string errors_cummulative;

    fs::path user_folder(data_dir() + "/" + PRESET_USER_DIR);
    if (!fs::exists(user_folder)) fs::create_directory(user_folder);

    std::string dir_user_presets = data_dir() + "/" + PRESET_USER_DIR + "/" + user;
    fs::path    folder(user_folder / user);
    if (!fs::exists(folder)) fs::create_directory(folder);

    // BBS do not load sla_print
    // BBS: change directoties by design
    try {
        std::string print_selected_preset_name = prints.get_selected_preset().name;
        this->prints.load_presets(dir_user_presets, PRESET_PRINT_NAME, substitutions, substitution_rule);
        prints.select_preset_by_name(print_selected_preset_name, false);
    } catch (const std::runtime_error &err) {
        errors_cummulative += err.what();
    }
    try {
        std::string filament_selected_preset_name = filaments.get_selected_preset().name;
        this->filaments.load_presets(dir_user_presets, PRESET_FILAMENT_NAME, substitutions, substitution_rule);
        filaments.select_preset_by_name(filament_selected_preset_name, false);
    } catch (const std::runtime_error &err) {
        errors_cummulative += err.what();
    }
    try {
        std::string printer_selected_preset_name = printers.get_selected_preset().name;
        this->printers.load_presets(dir_user_presets, PRESET_PRINTER_NAME, substitutions, substitution_rule);
        printers.select_preset_by_name(printer_selected_preset_name, false);
    } catch (const std::runtime_error &err) {
        errors_cummulative += err.what();
    }
    if (!errors_cummulative.empty()) throw Slic3r::RuntimeError(errors_cummulative);
    this->update_multi_material_filament_presets();
    this->update_compatible(PresetSelectCompatibleType::Never);

    set_calibrate_printer("");

    return PresetsConfigSubstitutions();
}

PresetsConfigSubstitutions PresetBundle::load_user_presets(AppConfig &                                                config,
                                                           std::map<std::string, std::map<std::string, std::string>> &my_presets,
                                                           ForwardCompatibilitySubstitutionRule                       substitution_rule)
{
    // First load the vendor specific system presets.
    PresetsConfigSubstitutions substitutions;
    std::string errors_cummulative;
    bool process_added = false, filament_added = false, machine_added = false;

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" enter, substitution_rule %1%, preset toltal count %2%")%substitution_rule%my_presets.size();
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" print's selected_idx %1%, selected_name %2%") %prints.get_selected_idx() %prints.get_selected_preset_name();
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" filament's selected_idx %1%, selected_name %2%") %filaments.get_selected_idx() %filaments.get_selected_preset_name();
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" printers's selected_idx %1%, selected_name %2%") %printers.get_selected_idx() %printers.get_selected_preset_name();

    // Sync removing
    remove_users_preset(config, &my_presets);

    std::map<std::string, std::map<std::string, std::string>>::iterator it;
    for (int pass = 0; pass < 2; ++pass)
    for (it = my_presets.begin(); it != my_presets.end(); it++) {
        std::string name = it->first;
        std::map<std::string, std::string>& value_map = it->second;
        // Load user root presets at first pass
        std::map<std::string, std::string>::iterator inherits_iter = value_map.find(BBL_JSON_KEY_INHERITS);
        if ((pass == 1) == (inherits_iter == value_map.end() || inherits_iter->second.empty()))
            continue;
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " start load from cloud: " << name;
        //get the type first
        std::map<std::string, std::string>::iterator type_iter = value_map.find(BBL_JSON_KEY_TYPE);
        if (type_iter == value_map.end()) {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(" can not find type for setting %1%")%name;
            continue;
        }
        try {
            PresetCollection *preset_collection = nullptr;
            if (type_iter->second == PRESET_IOT_PRINT_TYPE) {
                preset_collection = &(this->prints);
                process_added |= preset_collection->load_user_preset(name, value_map, substitutions, substitution_rule);
            }
            else if (type_iter->second == PRESET_IOT_FILAMENT_TYPE) {
                preset_collection = &(this->filaments);
                filament_added |= preset_collection->load_user_preset(name, value_map, substitutions, substitution_rule);
            }
            else if (type_iter->second == PRESET_IOT_PRINTER_TYPE) {
                preset_collection = &(this->printers);
                machine_added |= preset_collection->load_user_preset(name, value_map, substitutions, substitution_rule);
            }
            else {
                BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format("invalid type %1% for setting %2%") %type_iter->second %name;
                continue;
            }
        }
        catch (const std::runtime_error& err) {
            errors_cummulative += err.what();
        }
    }
    /*if (process_added) {
        this->prints.update_after_user_presets_loaded();
    }
    if (filament_added) {
        this->filaments.update_after_user_presets_loaded();
    }
    if (machine_added) {
        this->printers.update_after_user_presets_loaded();
    }*/

    this->update_multi_material_filament_presets();
    this->update_compatible(PresetSelectCompatibleType::Never);
    //this->load_selections(config, PresetPreferences());

    set_calibrate_printer("");

    if (! errors_cummulative.empty())
        throw Slic3r::RuntimeError(errors_cummulative);

    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" finished, process_added %1%, filament_added %2%, machine_added %3%")%process_added %filament_added %machine_added;
    return substitutions;
}

PresetsConfigSubstitutions PresetBundle::import_presets(std::vector<std::string> &              files,
                                                        std::function<int(std::string const &)> override_confirm,
                                                        ForwardCompatibilitySubstitutionRule    rule)
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " entry";
    PresetsConfigSubstitutions substitutions;
    int overwrite = 0;
    std::vector<std::string>   result;
    for (auto &file : files) {
        if (Slic3r::is_json_file(file)) {
            import_json_presets(substitutions, file, override_confirm, rule, overwrite, result);
        }
        // Determine if it is a preset bundle
        if (boost::iends_with(file, ".orca_printer") || boost::iends_with(file, ".orca_filament") || boost::iends_with(file, ".zip")) {
            boost::system::error_code ec;
            // create user folder
            fs::path user_folder(data_dir() + "/" + PRESET_USER_DIR);
            if (!fs::exists(user_folder)) fs::create_directory(user_folder, ec);
            if (ec) BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " create directory failed: " << ec.message();
            // create default folder
            fs::path default_folder(user_folder / DEFAULT_USER_FOLDER_NAME);
            if (!fs::exists(default_folder)) fs::create_directory(default_folder, ec);
            if (ec) BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " create directory failed: " << ec.message();
            //create temp folder
            //std::string user_default_temp_dir = data_dir() + "/" + PRESET_USER_DIR + "/" + DEFAULT_USER_FOLDER_NAME + "/" + "temp";
            fs::path temp_folder(default_folder / "temp");
            std::string user_default_temp_dir = temp_folder.make_preferred().string();
            if (fs::exists(temp_folder)) fs::remove_all(temp_folder);
            fs::create_directory(temp_folder, ec);
            if (ec) BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " create directory failed: " << ec.message();

            file = boost::filesystem::path(file).make_preferred().string();
            mz_zip_archive zip_archive;
            mz_zip_zero_struct(&zip_archive);
            mz_bool status;

            /*if (!open_zip_reader(&zip_archive, file)) {
                BOOST_LOG_TRIVIAL(info) << "Failed to initialize reader ZIP archive";
                return substitutions;
            } else {
                BOOST_LOG_TRIVIAL(info) << "Success to initialize reader ZIP archive";
            }*/

            FILE *zipFile = boost::nowide::fopen(file.c_str(), "rb");
            status        = mz_zip_reader_init_cfile(&zip_archive, zipFile, 0, MZ_ZIP_FLAG_CASE_SENSITIVE | MZ_ZIP_FLAG_IGNORE_PATH);
            if (MZ_FALSE == status) {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " Failed to initialize reader ZIP archive";
                return substitutions;
            } else {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " Success to initialize reader ZIP archive";
            }

            // Extract Files
            int num_files = mz_zip_reader_get_num_files(&zip_archive);
            for (int i = 0; i < num_files; i++) {
                mz_zip_archive_file_stat file_stat;
                status = mz_zip_reader_file_stat(&zip_archive, i, &file_stat);
                if (status) {
                    std::string file_name = file_stat.m_filename;
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " Form zip file: " << file << ". Read file name: " << file_stat.m_filename;
                    size_t index = file_name.find_last_of('/');
                    if (std::string::npos != index) {
                        file_name = file_name.substr(index + 1);
                    }
                    if (BUNDLE_STRUCTURE_JSON_NAME == file_name) continue;
                    // create target file path
                    std::string target_file_path = boost::filesystem::path(temp_folder / file_name).make_preferred().string();

                    status = mz_zip_reader_extract_to_file(&zip_archive, i, encode_path(target_file_path.c_str()).c_str(), MZ_ZIP_FLAG_CASE_SENSITIVE);
                    // target file is opened
                    if (MZ_FALSE == status) {
                        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " Failed to open target file: " << target_file_path;
                    } else {
                        bool is_success = import_json_presets(substitutions, target_file_path, override_confirm, rule, overwrite, result);
                        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " import target file: " << target_file_path << " import result" << is_success;
                    }
                }
            }
            fclose(zipFile);
            if (fs::exists(temp_folder)) fs::remove_all(temp_folder, ec);
            if (ec) BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " remove directory failed: " << ec.message();
        }
    }
    files = result;
    return substitutions;
}

bool PresetBundle::import_json_presets(PresetsConfigSubstitutions &            substitutions,
                                       std::string &                           file,
                                       std::function<int(std::string const &)> override_confirm,
                                       ForwardCompatibilitySubstitutionRule    rule,
                                       int &                                   overwrite,
                                       std::vector<std::string> &              result)
{
    try {
        DynamicPrintConfig config;
        // BBS: change to json format
        // ConfigSubstitutions config_substitutions = config.load_from_ini(preset.file, substitution_rule);
        std::map<std::string, std::string> key_values;
        std::string                        reason;
        ConfigSubstitutions                config_substitutions = config.load_from_json(file, rule, key_values, reason);
        std::string                        name                 = key_values[BBL_JSON_KEY_NAME];
        std::string                        version_str          = key_values[BBL_JSON_KEY_VERSION];
        boost::optional<Semver>            version              = Semver::parse(version_str);
        if (!version) return false;

        PresetCollection *collection = nullptr;
        if (config.has("printer_settings_id"))
            collection = &printers;
        else if (config.has("print_settings_id"))
            collection = &prints;
        else if (config.has("filament_settings_id"))
            collection = &filaments;
        if (collection == nullptr) {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << " Preset type is unknown, not loading: " << name;
            return false;
        }
        if (overwrite == 0) overwrite = 1;
        if (auto p = collection->find_preset(name, false)) {
            if (p->is_default || p->is_system) {
                BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << " Preset already present and is system preset, not loading: " << name;
                return false;
            }
            if (overwrite != 2 && overwrite != 3) overwrite = override_confirm(name); //3: yes to all  2: no to all
        }
        if (overwrite == 0 || overwrite == 2) {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << " Preset already present, not loading: " << name;
            return false;
        }

        DynamicPrintConfig new_config;
        Preset *           inherit_preset  = nullptr;
        ConfigOption *     inherits_config = config.option(BBL_JSON_KEY_INHERITS);
        std::string        inherits_value;
        if (inherits_config) {
            ConfigOptionString *option_str = dynamic_cast<ConfigOptionString *>(inherits_config);
            inherits_value                 = option_str->value;
            inherit_preset                 = collection->find_preset2(inherits_value);
        }
        if (inherit_preset) {
            new_config = inherit_preset->config;
            new_config.apply(std::move(config));
        } else {
            // We support custom root preset now
            auto inherits_config2 = dynamic_cast<ConfigOptionString *>(inherits_config);
            if (inherits_config2 && !inherits_config2->value.empty()) {
                // we should skip this preset here
                BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", can not find inherit preset for user preset %1%, just skip") % name;
                return false;
            }
            // Find a default preset for the config. The PrintPresetCollection provides different default preset based on the "printer_technology" field.
            const Preset &default_preset = collection->default_preset_for(config);
            new_config                   = default_preset.config;
            new_config.apply(std::move(config));
            extend_default_config_length(new_config, true, default_preset.config);
        }

        Preset &preset     = collection->load_preset(collection->path_from_name(name, inherit_preset == nullptr), name, std::move(new_config), false);
        if (key_values.find(BBL_JSON_KEY_FILAMENT_ID) != key_values.end())
            preset.filament_id = key_values[BBL_JSON_KEY_FILAMENT_ID];
        preset.is_external = true;
        preset.version     = *version;
        inherit_preset     = collection->find_preset(inherits_value, false, true); // pointer maybe wrong after insert, redo find
        if (inherit_preset) preset.base_id = inherit_preset->setting_id;
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " " << __LINE__ << preset.name << " have filament_id: " << preset.filament_id << " and base_id: " << preset.base_id;
        Preset::normalize(preset.config);
        // Report configuration fields, which are misplaced into a wrong group.
        const Preset &default_preset = collection->default_preset_for(new_config);
        std::string   incorrect_keys = Preset::remove_invalid_keys(preset.config, default_preset.config);
        if (!incorrect_keys.empty()) {
            ++m_errors;
            BOOST_LOG_TRIVIAL(error) << "Error in a preset file: The preset \"" << preset.file
                                     << "\" contains the following incorrect keys: " << incorrect_keys << ", which were removed";
        }
        if (!config_substitutions.empty())
            substitutions.push_back({name, collection->type(), PresetConfigSubstitutions::Source::UserFile, file, std::move(config_substitutions)});
        collection->set_custom_preset_alias(preset);
        preset.save(inherit_preset ? &inherit_preset->config : nullptr);
        result.push_back(file);
    } catch (const std::ifstream::failure &err) {
        ++m_errors;
        BOOST_LOG_TRIVIAL(error) << boost::format("The config cannot be loaded: %1%. Reason: %2%") % file % err.what();
    } catch (const std::runtime_error &err) {
        ++m_errors;
        BOOST_LOG_TRIVIAL(error) << boost::format("Failed importing config file: %1%. Reason: %2%") % file % err.what();
    }
    return true;
}

//BBS save user preset to user_id preset folder
void PresetBundle::save_user_presets(AppConfig& config, std::vector<std::string>& need_to_delete_list)
{
    std::string user_sub_folder = DEFAULT_USER_FOLDER_NAME;
    if (!config.get("preset_folder").empty())
        user_sub_folder = config.get("preset_folder");
    //BBS: change directory by design
    const std::string dir_user_presets = data_dir() + "/" + PRESET_USER_DIR + "/"+ user_sub_folder;

    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" enter, save to %1%")%dir_user_presets;

    fs::path user_folder(data_dir() + "/" + PRESET_USER_DIR);
    if (!fs::exists(user_folder))
        fs::create_directory(user_folder);

    fs::path folder(dir_user_presets);
    if (!fs::exists(folder))
        fs::create_directory(folder);

    this->prints.save_user_presets(dir_user_presets, PRESET_PRINT_NAME, need_to_delete_list);
    this->filaments.save_user_presets(dir_user_presets, PRESET_FILAMENT_NAME, need_to_delete_list);
    this->printers.save_user_presets(dir_user_presets, PRESET_PRINTER_NAME, need_to_delete_list);
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" finished");
}

//BBS: save user preset to user_id preset folder
void PresetBundle::update_user_presets_directory(const std::string preset_folder)
{
    //BBS: change directory by design
    const std::string dir_user_presets = data_dir() + "/" + PRESET_USER_DIR + "/"+ preset_folder;

    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" enter, update directory to %1%")%dir_user_presets;

    fs::path user_folder(data_dir() + "/" + PRESET_USER_DIR);
    if (!fs::exists(user_folder))
        fs::create_directory(user_folder);

    fs::path folder(dir_user_presets);
    if (!fs::exists(folder))
        fs::create_directory(folder);

    this->prints.update_user_presets_directory(dir_user_presets, PRESET_PRINT_NAME);
    this->filaments.update_user_presets_directory(dir_user_presets, PRESET_FILAMENT_NAME);
    this->printers.update_user_presets_directory(dir_user_presets, PRESET_PRINTER_NAME);
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" finished");
}

void PresetBundle::remove_user_presets_directory(const std::string preset_folder)
{
    const std::string dir_user_presets = data_dir() + "/" + PRESET_USER_DIR + "/" + preset_folder;

    if (preset_folder.empty()) {
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": preset_folder is empty, no need to remove directory : %1%") % dir_user_presets;
        return;
    }
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" enter, delete directory : %1%") % dir_user_presets;
    fs::path folder(dir_user_presets);
    if (fs::exists(folder)) {
        fs::remove_all(folder);
    }
}

void PresetBundle::update_system_preset_setting_ids(std::map<std::string, std::map<std::string, std::string>>& system_presets)
{
    for (auto iterator: system_presets)
    {
        std::string name = iterator.first;
        std::map<std::string, std::string>& value_map = iterator.second;
        //get the type first
        std::map<std::string, std::string>::iterator type_iter = value_map.find(BBL_JSON_KEY_TYPE);
        if (type_iter == value_map.end()) {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(" can not find type for setting %1%")%name;
            continue;
        }
        PresetCollection *preset_collection = nullptr;
        if (type_iter->second == PRESET_IOT_PRINTER_TYPE) {
            preset_collection = &(this->printers);
        }
        else if (type_iter->second == PRESET_IOT_PRINTER_TYPE) {
            preset_collection = &(this->printers);
        }
        else if (type_iter->second == PRESET_IOT_PRINTER_TYPE) {
            preset_collection = &(this->printers);
        }
        else {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format("invalid type %1% for setting %2%") %type_iter->second %name;
            continue;
        }
        std::string setting_id;
        if (value_map.count(BBL_JSON_KEY_SETTING_ID) > 0)
            setting_id = value_map[BBL_JSON_KEY_SETTING_ID];
        else {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(" can not find setting_id for setting %1%")%name;
            continue;
        }
        Preset* preset = preset_collection->find_preset(name, false, true);
        if (preset) {
            if (!preset->setting_id.empty() && (preset->setting_id.compare(setting_id) != 0)) {
                ++m_errors;
                BOOST_LOG_TRIVIAL(error) << boost::format("name %1%, local setting_id %2% is different with remote id %3%")
                    %preset->name %preset->setting_id %setting_id;
            }
            else if (preset->setting_id.empty())
                preset->setting_id = setting_id;
        }
        else {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format("can not find setting %1% in system presets, type %2%") %name %type_iter->second;
            continue;
        }
    }
    return;
}

//BBS: validate printers from previous project
static std::set<std::string> gcodes_key_set =  {"filament_end_gcode", "filament_start_gcode", "change_filament_gcode", "layer_change_gcode", "machine_end_gcode", "machine_pause_gcode", "machine_start_gcode",
            "template_custom_gcode", "printing_by_object_gcode", "before_layer_change_gcode", "time_lapse_gcode", "wrapping_detection_gcode"};
int PresetBundle::validate_presets(const std::string &file_name, DynamicPrintConfig& config, std::set<std::string>& different_gcodes)
{
    bool    validated = false;
    std::vector<std::string> inherits_values                        = config.option<ConfigOptionStrings>("inherits_group", true)->values;
    std::vector<std::string> filament_preset_name                   = config.option<ConfigOptionStrings>("filament_settings_id", true)->values;
    std::string printer_preset                                      = config.option<ConfigOptionString>("printer_settings_id", true)->value;
    bool has_different_settings_to_system                           = config.option("different_settings_to_system")?true:false;
    std::vector<std::string> different_values;
    int     ret = VALIDATE_PRESETS_SUCCESS;

    if (has_different_settings_to_system)
        different_values = config.option<ConfigOptionStrings>("different_settings_to_system", true)->values;

    //PrinterTechnology printer_technology = Preset::printer_technology(config);
    size_t filament_count = config.option<ConfigOptionFloats>("filament_diameter")->values.size();
    inherits_values.resize(filament_count + 2, std::string());
    different_values.resize(filament_count + 2, std::string());
    filament_preset_name.resize(filament_count, std::string());

    std::string printer_inherits = inherits_values[filament_count + 1];

    validated = this->printers.validate_preset(printer_preset, printer_inherits);
    if (!validated) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(":file_name %1%, found the printer preset not inherit from system") % file_name;
        different_gcodes.emplace(printer_preset);
        ret = VALIDATE_PRESETS_PRINTER_NOT_FOUND;
    }
    for(unsigned int index = 0; index < filament_count; index ++)
    {
        std::string filament_preset = filament_preset_name[index];
        std::string filament_inherits = inherits_values[index+1];

        validated = this->filaments.validate_preset(filament_preset, filament_inherits);
        if (!validated) {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(":file_name %1%, found the filament %2% preset not inherit from system") % file_name %(index+1);
            different_gcodes.emplace(filament_preset);
            ret = VALIDATE_PRESETS_FILAMENTS_NOT_FOUND;
        }
    }

    //self defined presets, return directly
    if (ret)
    {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(":file_name %1%, found self defined presets, count %2%") %file_name %different_gcodes.size();
        return ret;
    }

    for(unsigned int index = 1; index < filament_count; index ++)
    {
        std::string different_settingss = different_values[index];

        std::vector<std::string> different_keys;

        Slic3r::unescape_strings_cstyle(different_settingss, different_keys);

        for (unsigned int j = 0; j < different_keys.size(); j++) {
            if (gcodes_key_set.find(different_keys[j]) != gcodes_key_set.end()) {
                different_gcodes.emplace(different_keys[j]);
                BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(":preset index %1%, different key %2%") %index %different_keys[j];
            }
        }
    }

    if (!different_gcodes.empty())
    {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(":file_name %1%, found different gcodes count %2%") %file_name %different_gcodes.size();
        return VALIDATE_PRESETS_MODIFIED_GCODES;
    }

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(":file_name %1%, validate presets success!") % file_name;

    return VALIDATE_PRESETS_SUCCESS;
}

void PresetBundle::remove_users_preset(AppConfig &config, std::map<std::string, std::map<std::string, std::string>> *my_presets)
{
    auto check_removed = [my_presets, this](Preset &preset) -> bool {
        if (my_presets == nullptr) return true;
        if (my_presets->find(preset.name) != my_presets->end()) return false;
        if (!preset.sync_info.empty()) return false; // syncing, not remove
        if (preset.setting_id.empty()) return false; // no id, not remove
        // Saved preset is removed by another session
        if (preset.is_dirty) {
            preset.setting_id.clear();
            return false;
        }
        preset.remove_files();
        return true;
    };
    std::string preset_folder_user_id = config.get("preset_folder");
    std::string printer_selected_preset_name = printers.get_selected_preset().name;
    bool need_reset_printer_preset = false;
    for (auto it = printers.begin(); it != printers.end();) {
        if (it->is_user() && it->user_id.compare(preset_folder_user_id) == 0 && check_removed(*it)) {
            BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(":printers erase %1%, type %2%， user_id %3%") % it->name % Preset::get_type_string(it->type) % it->user_id;
            if (it->name == printer_selected_preset_name)
                need_reset_printer_preset = true;
            it = printers.erase(it);
        }
        else {
            it++;
        }
    }

    if (need_reset_printer_preset) {
        std::string default_printer_model = ORCA_DEFAULT_PRINTER_MODEL;
        std::string default_printer_name;
        for (auto it = printers.begin(); it != printers.end(); it++) {
            if (it->config.has("printer_model")) {
                if (it->config.opt_string("printer_model") == default_printer_model) {
                    default_printer_name = it->name;
                    break;
                }
            }
        }
        printers.select_preset_by_name(default_printer_name, true);
    } else {
        printers.select_preset_by_name(printer_selected_preset_name, false);
    }

    std::string selected_print_name = prints.get_selected_preset().name;
    bool need_reset_print_preset = false;
    // remove preset if user_id is not current user
    for (auto it = prints.begin(); it != prints.end();) {
        if (it->is_user() && it->user_id.compare(preset_folder_user_id) == 0 && check_removed(*it)) {
            BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(":prints erase %1%, type %2%， user_id %3%")%it->name %Preset::get_type_string(it->type) %it->user_id;
            if (it->name == selected_print_name)
                need_reset_print_preset = true;
            it = prints.erase(it);
        }
        else {
            it++;
        }
    }
    if (need_reset_print_preset && printers.get_selected_preset().config.has("default_print_profile")) {
        std::string default_print_profile_name = printers.get_selected_preset().config.opt_string("default_print_profile");
        prints.select_preset_by_name(default_print_profile_name, true);
    } else {
        prints.select_preset_by_name(selected_print_name, false);
    }

    std::string selected_filament_name = filaments.get_selected_preset().name;
    bool need_reset_filament_preset = false;
    for (auto it = filaments.begin(); it != filaments.end();) {
        if (it->is_user() && it->user_id.compare(preset_folder_user_id) == 0 && check_removed(*it)) {
            BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(":filaments erase %1%, type %2%， user_id %3%")%it->name %Preset::get_type_string(it->type) %it->user_id;
            if (it->name == selected_filament_name)
                need_reset_filament_preset = true;
            it = filaments.erase(it);
        }
        else {
            it++;
        }
    }
    if (need_reset_filament_preset && printers.get_selected_preset().config.has("default_filament_profile")) {
        const std::vector<std::string>& prefered_filament_profiles = printers.get_selected_preset().config.option<ConfigOptionStrings>("default_filament_profile")->values;
        if (prefered_filament_profiles.size() > 0)
            filaments.select_preset_by_name(prefered_filament_profiles[0], true);
    } else {
        filaments.select_preset_by_name(selected_filament_name, false);
    }

    update_compatible(PresetSelectCompatibleType::Always);

    /* set selected preset */
    for (size_t i = 0; i < filament_presets.size(); ++i)
    {
        auto preset = this->filaments.find_preset(filament_presets[i]);
        if (preset == nullptr)
            filament_presets[i] = filaments.get_selected_preset_name();
    }
}


//BBS: add json related logic, load system presets from json
std::pair<PresetsConfigSubstitutions, std::string> PresetBundle::load_system_presets_from_json(ForwardCompatibilitySubstitutionRule compatibility_rule)
{
    //BBS: add config related logs
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" enter, compatibility_rule %1%")%compatibility_rule;
    if (compatibility_rule == ForwardCompatibilitySubstitutionRule::EnableSystemSilent)
        // Loading system presets, don't log substitutions.
        compatibility_rule = ForwardCompatibilitySubstitutionRule::EnableSilent;
    else if (compatibility_rule == ForwardCompatibilitySubstitutionRule::EnableSilentDisableSystem)
        // Loading system presets, throw on unknown option value.
        compatibility_rule = ForwardCompatibilitySubstitutionRule::Disable;

    // Here the vendor specific read only Config Bundles are stored.
    //BBS: change directory by design
    boost::filesystem::path     dir = (boost::filesystem::path(data_dir()) / PRESET_SYSTEM_DIR).make_preferred();
    if (validation_mode)
        dir = (boost::filesystem::path(data_dir())).make_preferred();

    PresetsConfigSubstitutions  substitutions;
    std::string                 errors_cummulative;
    bool                        first = true;
    std::vector<std::string> vendor_names;
    // store all vendor names in vendor_names
    for (auto& dir_entry : boost::filesystem::directory_iterator(dir)) {
        std::string vendor_file = dir_entry.path().string();
        if (!Slic3r::is_json_file(vendor_file))
            continue;

        std::string vendor_name = dir_entry.path().filename().string();

        // Remove the .json suffix.
        vendor_name.erase(vendor_name.size() - 5);
        vendor_names.push_back(vendor_name);
    }
    // Move ORCA_FILAMENT_LIBRARY to the beginning of the list
    for (size_t i = 0; i < vendor_names.size(); ++ i) {
        if (vendor_names[i] == ORCA_FILAMENT_LIBRARY) {
            std::swap(vendor_names[0], vendor_names[i]);
            break;
        }
    }

    for (auto &vendor_name : vendor_names)
    {
        if (validation_mode && !vendor_to_validate.empty() && vendor_name != vendor_to_validate && vendor_name != ORCA_FILAMENT_LIBRARY)
            continue;

        try {
            // Load the config bundle, flatten it.
            if (first) {
                // Reset this PresetBundle and load the first vendor config.
                append(substitutions, this->load_vendor_configs_from_json(dir.string(), vendor_name, PresetBundle::LoadSystem, compatibility_rule).first);
                first = false;
            } else {
                // Load the other vendor configs, merge them with this PresetBundle.
                // Report duplicate profiles.
                PresetBundle other;
                append(substitutions, other.load_vendor_configs_from_json(dir.string(), vendor_name, PresetBundle::LoadSystem, compatibility_rule, this).first);
                std::vector<std::string> duplicates = this->merge_presets(std::move(other));
                if (!duplicates.empty()) {
                    errors_cummulative += "Found duplicated settings in vendor " + vendor_name + "'s json file lists: ";
                    for (size_t i = 0; i < duplicates.size(); ++i) {
                        if (i > 0)
                            errors_cummulative += ", ";
                        errors_cummulative += duplicates[i];
                        ++m_errors;
                        BOOST_LOG_TRIVIAL(error) << "Found duplicated preset: " + duplicates[i] + " in vendor: " + vendor_name + ": ";
                    }
                }
            }
        } catch (const std::runtime_error &err) {
            if (validation_mode)
                throw err;
            else {
                errors_cummulative += err.what();
                errors_cummulative += "\n";
            }
        }
    }

    if (first) {
		// No config bundle loaded, reset.
		this->reset(false);
	}

	this->update_system_maps();
    //BBS: add config related logs
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" finished, errors_cummulative %1%")%errors_cummulative;
    return std::make_pair(std::move(substitutions), errors_cummulative);
}

std::pair<PresetsConfigSubstitutions, std::string> PresetBundle::load_system_models_from_json(ForwardCompatibilitySubstitutionRule compatibility_rule)
{
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" enter, compatibility_rule %1%") % compatibility_rule;
    if (compatibility_rule == ForwardCompatibilitySubstitutionRule::EnableSystemSilent)
        // Loading system presets, don't log substitutions.
        compatibility_rule = ForwardCompatibilitySubstitutionRule::EnableSilent;
    else if (compatibility_rule == ForwardCompatibilitySubstitutionRule::EnableSilentDisableSystem)
        // Loading system presets, throw on unknown option value.
        compatibility_rule = ForwardCompatibilitySubstitutionRule::Disable;

    // Here the vendor specific read only Config Bundles are stored.
    boost::filesystem::path    dir = (boost::filesystem::path(resources_dir()) / "profiles").make_preferred();
    PresetsConfigSubstitutions substitutions;
    std::string                errors_cummulative;
    for (auto &dir_entry : boost::filesystem::directory_iterator(dir)) {
        std::string vendor_file = dir_entry.path().string();
        if (Slic3r::is_json_file(vendor_file)) {
            std::string vendor_name = dir_entry.path().filename().string();
            // Remove the .json suffix.
            vendor_name.erase(vendor_name.size() - 5);
            try {
                // Load the config bundle, flatten it.
                append(substitutions, load_vendor_configs_from_json(dir.string(), vendor_name, PresetBundle::LoadVendorOnly, compatibility_rule).first);
            } catch (const std::runtime_error &err) {
                errors_cummulative += err.what();
                errors_cummulative += "\n";
            }
        }
    }

    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" finished, errors_cummulative %1%") % errors_cummulative;
    return std::make_pair(std::move(substitutions), errors_cummulative);
}

std::pair<PresetsConfigSubstitutions, std::string> PresetBundle::load_system_filaments_json(ForwardCompatibilitySubstitutionRule compatibility_rule)
{
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" enter, compatibility_rule %1%") % compatibility_rule;
    if (compatibility_rule == ForwardCompatibilitySubstitutionRule::EnableSystemSilent)
        // Loading system presets, don't log substitutions.
        compatibility_rule = ForwardCompatibilitySubstitutionRule::EnableSilent;
    else if (compatibility_rule == ForwardCompatibilitySubstitutionRule::EnableSilentDisableSystem)
        // Loading system presets, throw on unknown option value.
        compatibility_rule = ForwardCompatibilitySubstitutionRule::Disable;

    // Here the vendor specific read only Config Bundles are stored.
    boost::filesystem::path    dir = (boost::filesystem::path(resources_dir()) / "profiles").make_preferred();
    PresetsConfigSubstitutions substitutions;
    std::string                errors_cummulative;
    bool                       first = true;
    for (auto &dir_entry : boost::filesystem::directory_iterator(dir)) {
        std::string vendor_file = dir_entry.path().string();
        if (Slic3r::is_json_file(vendor_file)) {
            std::string vendor_name = dir_entry.path().filename().string();
            // Remove the .json suffix.
            vendor_name.erase(vendor_name.size() - 5);
            try {
                if (first) {
                    // Reset this PresetBundle and load the first vendor config.
                    append(substitutions, this->load_vendor_configs_from_json(dir.string(), vendor_name, PresetBundle::LoadSystem | PresetBundle::LoadFilamentOnly, compatibility_rule).first);
                    first = false;
                } else {
                    // Load the other vendor configs, merge them with this PresetBundle.
                    // Report duplicate profiles.
                    PresetBundle other;
                    append(substitutions, other.load_vendor_configs_from_json(dir.string(), vendor_name, PresetBundle::LoadSystem | PresetBundle::LoadFilamentOnly, compatibility_rule).first);
                    std::vector<std::string> duplicates = this->merge_presets(std::move(other));
                    if (!duplicates.empty()) {
                        errors_cummulative += "Found duplicated settings in vendor " + vendor_name + "'s json file lists: ";
                        for (size_t i = 0; i < duplicates.size(); ++i) {
                            if (i > 0) errors_cummulative += ", ";
                            errors_cummulative += duplicates[i];
                        }
                    }
                }
            } catch (const std::runtime_error &err) {
                errors_cummulative += err.what();
                errors_cummulative += "\n";
            }
        }
    }

    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" finished, errors_cummulative %1%") % errors_cummulative;
    return std::make_pair(std::move(substitutions), errors_cummulative);
}

VendorProfile PresetBundle::get_custom_vendor_models() const
{
    VendorProfile vendor;
    vendor.name = PRESET_CUSTOM_VENDOR;
    vendor.id   = PRESET_CUSTOM_VENDOR;
    for (auto &preset : printers.get_presets()) {
        if (preset.is_system) continue;
        if (printers.get_preset_base(preset) != &preset) continue;
        if (preset.is_default) continue;
        auto model = preset.config.opt_string("printer_model");
        auto variant = preset.config.opt_string("printer_variant");
        auto iter_model = std::find_if(vendor.models.begin(), vendor.models.end(), [model](VendorProfile::PrinterModel &m) {
            return m.name == model;
        });
        if (iter_model == vendor.models.end()) {
            iter_model = vendor.models.emplace(vendor.models.end(), VendorProfile::PrinterModel{});
            iter_model->id       = model;
            iter_model->name     = model;
            iter_model->variants = {VendorProfile::PrinterVariant(variant)};
        } else {
            iter_model->variants.push_back(VendorProfile::PrinterVariant(variant));
        }
    }
    return vendor;
}

// Merge one vendor's presets with the other vendor's presets, report duplicates.
std::vector<std::string> PresetBundle::merge_presets(PresetBundle &&other)
{
    this->vendors.insert(other.vendors.begin(), other.vendors.end());
    std::vector<std::string> duplicate_prints        = this->prints       .merge_presets(std::move(other.prints),        this->vendors);
    std::vector<std::string> duplicate_sla_prints    = this->sla_prints   .merge_presets(std::move(other.sla_prints),    this->vendors);
    std::vector<std::string> duplicate_filaments     = this->filaments    .merge_presets(std::move(other.filaments),     this->vendors);
    std::vector<std::string> duplicate_sla_materials = this->sla_materials.merge_presets(std::move(other.sla_materials), this->vendors);
    std::vector<std::string> duplicate_printers      = this->printers     .merge_presets(std::move(other.printers),      this->vendors);
	append(this->obsolete_presets.prints,        std::move(other.obsolete_presets.prints));
	append(this->obsolete_presets.sla_prints,    std::move(other.obsolete_presets.sla_prints));
	append(this->obsolete_presets.filaments,     std::move(other.obsolete_presets.filaments));
    append(this->obsolete_presets.sla_materials, std::move(other.obsolete_presets.sla_materials));
	append(this->obsolete_presets.printers,      std::move(other.obsolete_presets.printers));
	append(duplicate_prints, std::move(duplicate_sla_prints));
	append(duplicate_prints, std::move(duplicate_filaments));
    append(duplicate_prints, std::move(duplicate_sla_materials));
    append(duplicate_prints, std::move(duplicate_printers));
    m_errors += other.m_errors;
    return duplicate_prints;
}

void PresetBundle::update_system_maps()
{
    this->prints 	   .update_map_system_profile_renamed();
    this->sla_prints   .update_map_system_profile_renamed();
    this->filaments    .update_map_system_profile_renamed();
    this->sla_materials.update_map_system_profile_renamed();
    this->printers     .update_map_system_profile_renamed();

    this->prints       .update_map_alias_to_profile_name();
    this->sla_prints   .update_map_alias_to_profile_name();
    this->filaments    .update_map_alias_to_profile_name();
    this->sla_materials.update_map_alias_to_profile_name();

    this->filaments.update_library_profile_excluded_from();
}

static inline std::string remove_ini_suffix(const std::string &name)
{
    std::string out = name;
    if (boost::iends_with(out, ".ini"))
        out.erase(out.end() - 4, out.end());
    return out;
}

// Set the "enabled" flag for printer vendors, printer models and printer variants
// based on the user configuration.
// If the "vendor" section is missing, enable all models and variants of the particular vendor.
void PresetBundle::load_installed_printers(const AppConfig &config)
{
	this->update_system_maps();
    for (auto &preset : printers)
        preset.set_visible_from_appconfig(config);
}

const std::string& PresetBundle::get_preset_name_by_alias( const Preset::Type& preset_type, const std::string& alias) const
{
    // there are not aliases for Printers profiles
    if (preset_type == Preset::TYPE_PRINTER || preset_type == Preset::TYPE_INVALID)
        return alias;

    const PresetCollection& presets = preset_type == Preset::TYPE_PRINT     ? prints :
                                      preset_type == Preset::TYPE_SLA_PRINT ? sla_prints :
                                      preset_type == Preset::TYPE_FILAMENT  ? filaments :
                                      sla_materials;

    return presets.get_preset_name_by_alias(alias);
}

//BBS: get filament required hrc by filament type
const int PresetBundle::get_required_hrc_by_filament_type(const std::string& filament_type) const
{
    static std::unordered_map<std::string, int>filament_type_to_hrc;
    if (filament_type_to_hrc.empty()) {
        for (auto iter = filaments.m_presets.begin(); iter != filaments.m_presets.end(); iter++) {
            if (iter->vendor && iter->vendor->id == "BBL") {
                if (iter->config.has("filament_type") && iter->config.has("required_nozzle_HRC")) {
                    auto type = iter->config.opt_string("filament_type", 0);
                    auto hrc = iter->config.opt_int("required_nozzle_HRC", 0);
                    filament_type_to_hrc[type] = hrc;
                }
            }
        }
    }
    auto iter = filament_type_to_hrc.find(filament_type);
    if (iter != filament_type_to_hrc.end())
        return iter->second;
    else
        return 0;
}

//BBS: add project embedded preset logic
void PresetBundle::save_changes_for_preset(const std::string& new_name, Preset::Type type,
                                           const std::vector<std::string>& unselected_options, bool save_to_project)
{
    PresetCollection& presets = type == Preset::TYPE_PRINT          ? prints :
                                type == Preset::TYPE_SLA_PRINT      ? sla_prints :
                                type == Preset::TYPE_FILAMENT       ? filaments :
                                type == Preset::TYPE_SLA_MATERIAL   ? sla_materials : printers;

    // if we want to save just some from selected options
    if (!unselected_options.empty()) {
        // revert unselected options to the old values
        presets.get_edited_preset().config.apply_only(presets.get_selected_preset().config, unselected_options);
    }

    // Save the preset into Slic3r::data_dir / presets / section_name / preset_name.ini
    //BBS: add project embedded preset logic
    //presets.save_current_preset(new_name);
    presets.save_current_preset(new_name, false, save_to_project);
    // Mark the print & filament enabled if they are compatible with the currently selected preset.
    // If saving the preset changes compatibility with other presets, keep the now incompatible dependent presets selected, however with a "red flag" icon showing that they are no more compatible.
    update_compatible(PresetSelectCompatibleType::Never);

    if (type == Preset::TYPE_FILAMENT) {
        // synchronize the first filament presets.
        set_filament_preset(0, filaments.get_selected_preset_name());
    }
}

void PresetBundle::load_installed_filaments(AppConfig &config)
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": enter, printer size %1%")%printers.size();
    //if (! config.has_section(AppConfig::SECTION_FILAMENTS)
    //    || config.get_section(AppConfig::SECTION_FILAMENTS).empty()) {
        // Compatibility with the PrusaSlicer 2.1.1 and older, where the filament profiles were not installable yet.
        // Find all filament profiles, which are compatible with installed printers, and act as if these filament profiles
        // were installed.
        std::unordered_set<const Preset*> compatible_filaments;
        for (const Preset &printer : printers)
            if (printer.is_visible && printer.printer_technology() == ptFFF && printer.vendor && (!printer.vendor->models.empty())) {
                bool add_default_materials = true;
                if (config.has_section(AppConfig::SECTION_FILAMENTS))
                {
                    const std::map<std::string, std::string>& installed_filament = config.get_section(AppConfig::SECTION_FILAMENTS);
                    for (auto filament_iter : installed_filament)
                    {
                        Preset* filament = filaments.find_preset(filament_iter.first, false, true);
                        if (filament && is_compatible_with_printer(PresetWithVendorProfile(*filament, filament->vendor), PresetWithVendorProfile(printer, printer.vendor)))
                        {

                            //already has compatible filament
                            add_default_materials = false;
                            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": printer %1% vendor %2% already has default filament %3%")%printer.name %printer.vendor %filament_iter.first;
                            break;
                        }
                    }
                }

                if (!add_default_materials)
                    continue;

                const VendorProfile::PrinterModel *printer_model = PresetUtils::system_printer_model(printer);
                if (!printer_model) {
                    BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": can not find printer_model for printer %1%")%printer.name;
                    continue;
                }
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": printer %1% vendor %2% don't have filament visible, will add %3% default filaments")%printer.name %printer.vendor %printer_model->default_materials.size();
                for (auto default_filament: printer_model->default_materials)
                {
                    Preset* filament = filaments.find_preset(default_filament, false, true);
                    if (filament && filament->is_system)
                        compatible_filaments.insert(filament);
                }
                //const PresetWithVendorProfile printer_with_vendor_profile = printers.get_preset_with_vendor_profile(printer);
                //for (const Preset &filament : filaments)
                //  if (filament.is_system && is_compatible_with_printer(filaments.get_preset_with_vendor_profile(filament), printer_with_vendor_profile))
                //      compatible_filaments.insert(&filament);
            }
        // and mark these filaments as installed, therefore this code will not be executed at the next start of the application.
        for (const auto &filament: compatible_filaments) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": set filament %1% to visible by default")%filament->name;
            config.set(AppConfig::SECTION_FILAMENTS, filament->name, "true");
        }
    //}

    for (auto &preset : filaments)
        preset.set_visible_from_appconfig(config);
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": exit.");
}

void PresetBundle::load_installed_sla_materials(AppConfig &config)
{
    if (! config.has_section(AppConfig::SECTION_MATERIALS)) {
        std::unordered_set<const Preset*> comp_sla_materials;
		// Compatibility with the PrusaSlicer 2.1.1 and older, where the SLA material profiles were not installable yet.
		// Find all SLA material profiles, which are compatible with installed printers, and act as if these SLA material profiles
		// were installed.
        for (const Preset &printer : printers)
            if (printer.is_visible && printer.printer_technology() == ptSLA) {
				const PresetWithVendorProfile printer_with_vendor_profile = printers.get_preset_with_vendor_profile(printer);
				for (const Preset &material : sla_materials)
					if (material.is_system && is_compatible_with_printer(sla_materials.get_preset_with_vendor_profile(material), printer_with_vendor_profile))
						comp_sla_materials.insert(&material);
			}
		// and mark these SLA materials as installed, therefore this code will not be executed at the next start of the application.
		for (const auto &material: comp_sla_materials)
            config.set(AppConfig::SECTION_MATERIALS, material->name, "true");
    }

    for (auto &preset : sla_materials)
        preset.set_visible_from_appconfig(config);
}

void PresetBundle::update_selections(AppConfig &config)
{
    std::string initial_printer_profile_name    = printers.get_selected_preset_name();
    // Orca: load from orca_presets
    std::string initial_print_profile_name        = config.get_printer_setting(initial_printer_profile_name, PRESET_PRINT_NAME);
    std::string initial_filament_profile_name     = config.get_printer_setting(initial_printer_profile_name, PRESET_FILAMENT_NAME);

    // Selects the profiles, which were selected at the last application close.
    prints.select_preset_by_name_strict(initial_print_profile_name);
    filaments.select_preset_by_name_strict(initial_filament_profile_name);

    // Load the names of the other filament profiles selected for a multi-material printer.
    // Load it even if the current printer technology is SLA.
    // The possibly excessive filament names will be later removed with this->update_multi_material_filament_presets()
    // once the FFF technology gets selected.
    this->filament_presets = { filaments.get_selected_preset_name() };
    for (unsigned int i = 1; i < 1000; ++ i) {
        char name[64];
        sprintf(name, "filament_%02u", i);
        auto f_name = config.get_printer_setting(initial_printer_profile_name, name);
        if (f_name.empty())
            break;
        this->filament_presets.emplace_back(remove_ini_suffix(f_name));
    }
    std::vector<std::string> filament_colors;
    auto f_colors = config.get_printer_setting(initial_printer_profile_name, "filament_colors");
    if (!f_colors.empty()) {
        boost::algorithm::split(filament_colors, f_colors, boost::algorithm::is_any_of(","));
    }
    filament_colors.resize(filament_presets.size(), "#26A69A");
    project_config.option<ConfigOptionStrings>("filament_colour")->values = filament_colors;

    std::vector<std::string> multi_filament_colors;
    if (config.has_printer_setting(initial_printer_profile_name, "filament_multi_colors")) {
        boost::algorithm::split(multi_filament_colors, config.get_printer_setting(initial_printer_profile_name, "filament_multi_colors"), boost::algorithm::is_any_of(","));
    }
    if (multi_filament_colors.size() == 0) project_config.option<ConfigOptionStrings>("filament_multi_colour")->values = filament_colors;
    else project_config.option<ConfigOptionStrings>("filament_multi_colour")->values = multi_filament_colors;

    std::vector<std::string> filament_color_types;
    if (config.has_printer_setting(initial_printer_profile_name, "filament_color_types")) {
        boost::algorithm::split(filament_color_types, config.get_printer_setting(initial_printer_profile_name, "filament_color_types"), boost::algorithm::is_any_of(","));
    }
    filament_color_types.resize(filament_presets.size(), "1");
    project_config.option<ConfigOptionStrings>("filament_colour_type")->values = filament_color_types;

    std::vector<int> filament_maps(filament_colors.size(), 1);
    project_config.option<ConfigOptionInts>("filament_map")->values = filament_maps;

    std::vector<std::string> extruder_ams_count_str;
    if (config.has_printer_setting(initial_printer_profile_name, "extruder_ams_count")) {
        boost::algorithm::split(extruder_ams_count_str, config.get_printer_setting(initial_printer_profile_name, "extruder_ams_count"), boost::algorithm::is_any_of(","));
    }
    this->extruder_ams_counts = get_extruder_ams_count(extruder_ams_count_str);

    std::vector<std::string> matrix;
    if (config.has_printer_setting(initial_printer_profile_name, "flush_volumes_matrix")) {
        boost::algorithm::split(matrix, config.get_printer_setting(initial_printer_profile_name, "flush_volumes_matrix"), boost::algorithm::is_any_of("|"));
        auto flush_volumes_matrix = matrix | boost::adaptors::transformed(boost::lexical_cast<double, std::string>);
        project_config.option<ConfigOptionFloats>("flush_volumes_matrix")->values = std::vector<double>(flush_volumes_matrix.begin(), flush_volumes_matrix.end());
    }
    if (config.has_printer_setting(initial_printer_profile_name, "flush_volumes_vector")) {
        boost::algorithm::split(matrix, config.get_printer_setting(initial_printer_profile_name, "flush_volumes_vector"), boost::algorithm::is_any_of("|"));
        auto flush_volumes_vector = matrix | boost::adaptors::transformed(boost::lexical_cast<double, std::string>);
        project_config.option<ConfigOptionFloats>("flush_volumes_vector")->values = std::vector<double>(flush_volumes_vector.begin(), flush_volumes_vector.end());
    }
    if (config.has_printer_setting(initial_printer_profile_name, "flush_multiplier")) {
        boost::algorithm::split(matrix, config.get_printer_setting(initial_printer_profile_name, "flush_multiplier"), boost::algorithm::is_any_of("|"));
        auto flush_multipliers = matrix | boost::adaptors::transformed(boost::lexical_cast<double, std::string>);
        project_config.option<ConfigOptionFloats>("flush_multiplier")->values = std::vector<double>(flush_multipliers.begin(), flush_multipliers.end());
    }

    // Update visibility of presets based on their compatibility with the active printer.
    // Always try to select a compatible print and filament preset to the current printer preset,
    // as the application may have been closed with an active "external" preset, which does not
    // exist.
    this->update_compatible(PresetSelectCompatibleType::Always);
    this->update_multi_material_filament_presets();

    std::string first_visible_filament_name;
    for (auto & fp : filament_presets) {
        if (auto it = filaments.find_preset_internal(fp); it == filaments.end() || !it->is_visible || !it->is_compatible) {
            if (first_visible_filament_name.empty())
                first_visible_filament_name = filaments.first_compatible().name;
            fp = first_visible_filament_name;
        }
    }

}

// Load selections (current print, current filaments, current printer) from config.ini
// This is done on application start up or after updates are applied.
void PresetBundle::load_selections(AppConfig &config, const PresetPreferences& preferred_selection/* = PresetPreferences()*/)
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": enter, preferred printer_model_id %1%")%preferred_selection.printer_model_id;
	// Update visibility of presets based on application vendor / model / variant configuration.
	this->load_installed_printers(config);

    // Update visibility of filament and sla material presets
    this->load_installed_filaments(config);
    this->load_installed_sla_materials(config);

    // Parse the initial print / filament / printer profile names.
    // std::string initial_sla_print_profile_name    = remove_ini_suffix(config.get("presets", PRESET_SLA_PRINT_NAME));
    // std::string initial_sla_material_profile_name = remove_ini_suffix(config.get("presets", PRESET_SLA_MATERIALS_NAME));
	std::string initial_printer_profile_name      = remove_ini_suffix(config.get("presets", PRESET_PRINTER_NAME));

    // Activate print / filament / printer profiles from either the config,
    // or from the preferred_model_id suggestion passed in by ConfigWizard.
    // If the printer profile enumerated by the config are not visible, select an alternate preset.
    // Do not select alternate profiles for the print / filament profiles as those presets
    // will be selected by the following call of this->update_compatible(PresetSelectCompatibleType::Always).

    const Preset *initial_printer = printers.find_preset(initial_printer_profile_name);
    // If executed due to a Config Wizard update, preferred_printer contains the first newly installed printer, otherwise nullptr.
    const Preset *preferred_printer = printers.find_system_preset_by_model_and_variant(preferred_selection.printer_model_id, preferred_selection.printer_variant);
    printers.select_preset_by_name(preferred_printer ? preferred_printer->name : initial_printer_profile_name, true);
    CNumericLocalesSetter locales_setter;

    // Orca: load from orca_presets
    // const auto os_presets = config.get_machine_settings(initial_printer_profile_name);
    std::string initial_print_profile_name        = config.get_printer_setting(initial_printer_profile_name, PRESET_PRINT_NAME);
    std::string initial_filament_profile_name     = config.get_printer_setting(initial_printer_profile_name, PRESET_FILAMENT_NAME);

    //BBS: set default print/filament profiles to BBL's default setting
    if (preferred_printer)
    {
        const std::string& prefered_print_profile = preferred_printer->config.opt_string("default_print_profile");
        if ((!initial_print_profile_name.compare("Default Setting")) && (prefered_print_profile.size() > 0))
            initial_print_profile_name = prefered_print_profile;

        const std::vector<std::string>& prefered_filament_profiles = preferred_printer->config.option<ConfigOptionStrings>("default_filament_profile")->values;
        if ((!initial_filament_profile_name.compare("Default Filament")) && (prefered_filament_profiles.size() > 0))
            initial_filament_profile_name = prefered_filament_profiles[0];
    }

    // Selects the profile, leaves it to -1 if the initial profile name is empty or if it was not found.
    prints.select_preset_by_name_strict(initial_print_profile_name);
    filaments.select_preset_by_name_strict(initial_filament_profile_name);
	// sla_prints.select_preset_by_name_strict(initial_sla_print_profile_name);
    // sla_materials.select_preset_by_name_strict(initial_sla_material_profile_name);

    // Load the names of the other filament profiles selected for a multi-material printer.
    // Load it even if the current printer technology is SLA.
    // The possibly excessive filament names will be later removed with this->update_multi_material_filament_presets()
    // once the FFF technology gets selected.
    this->filament_presets = { filaments.get_selected_preset_name() };
    for (unsigned int i = 1; i < 1000; ++ i) {
        char name[64];
        sprintf(name, "filament_%02u", i);
        auto f_name = config.get_printer_setting(initial_printer_profile_name, name);
        if (f_name.empty())
            break;
        this->filament_presets.emplace_back(remove_ini_suffix(f_name));
    }

    // Load data from AppConfig to ProjectConfig when Studio is initialized.
    std::vector<std::string> filament_colors;
    auto f_colors = config.get_printer_setting(initial_printer_profile_name, "filament_colors");
    if (!f_colors.empty()) {
        boost::algorithm::split(filament_colors, f_colors, boost::algorithm::is_any_of(","));
    }
    filament_colors.resize(filament_presets.size(), "#26A69A");
    project_config.option<ConfigOptionStrings>("filament_colour")->values = filament_colors;

    std::vector<std::string> multi_filament_colors;
    if (config.has_printer_setting(initial_printer_profile_name, "filament_multi_colors")) {
        boost::algorithm::split(multi_filament_colors, config.get_printer_setting(initial_printer_profile_name, "filament_multi_colors"), boost::algorithm::is_any_of(","));
    }
    if (multi_filament_colors.size() == 0) project_config.option<ConfigOptionStrings>("filament_multi_colour")->values = filament_colors;
    else project_config.option<ConfigOptionStrings>("filament_multi_colour")->values = multi_filament_colors;

    std::vector<std::string> filament_color_types;
    if (config.has_printer_setting(initial_printer_profile_name, "filament_color_types")) {
        boost::algorithm::split(filament_color_types, config.get_printer_setting(initial_printer_profile_name, "filament_color_types"), boost::algorithm::is_any_of(","));
    }
    filament_color_types.resize(filament_presets.size(), "1");
    project_config.option<ConfigOptionStrings>("filament_colour_type")->values = filament_color_types;

    std::vector<int> filament_maps(filament_colors.size(), 1);
    project_config.option<ConfigOptionInts>("filament_map")->values = filament_maps;

    std::vector<std::string> extruder_ams_count_str;
    if (config.has_printer_setting(initial_printer_profile_name, "extruder_ams_count")) {
        boost::algorithm::split(extruder_ams_count_str, config.get_printer_setting(initial_printer_profile_name, "extruder_ams_count"), boost::algorithm::is_any_of(","));
    }
    this->extruder_ams_counts = get_extruder_ams_count(extruder_ams_count_str);

    std::vector<std::string> matrix;
    if (config.has_printer_setting(initial_printer_profile_name, "flush_volumes_matrix")) {
        boost::algorithm::split(matrix, config.get_printer_setting(initial_printer_profile_name, "flush_volumes_matrix"), boost::algorithm::is_any_of("|"));
        auto flush_volumes_matrix = matrix | boost::adaptors::transformed(boost::lexical_cast<double, std::string>);
        project_config.option<ConfigOptionFloats>("flush_volumes_matrix")->values = std::vector<double>(flush_volumes_matrix.begin(), flush_volumes_matrix.end());
    }
    if (config.has_printer_setting(initial_printer_profile_name, "flush_volumes_vector")) {
        boost::algorithm::split(matrix, config.get_printer_setting(initial_printer_profile_name, "flush_volumes_vector"), boost::algorithm::is_any_of("|"));
        auto flush_volumes_vector = matrix | boost::adaptors::transformed(boost::lexical_cast<double, std::string>);
        project_config.option<ConfigOptionFloats>("flush_volumes_vector")->values = std::vector<double>(flush_volumes_vector.begin(), flush_volumes_vector.end());
    }
    if (config.has_printer_setting(initial_printer_profile_name, "flush_multiplier")) {
        boost::algorithm::split(matrix, config.get_printer_setting(initial_printer_profile_name, "flush_multiplier"), boost::algorithm::is_any_of("|"));
        auto flush_multipliers = matrix | boost::adaptors::transformed(boost::lexical_cast<double, std::string>);
        project_config.option<ConfigOptionFloats>("flush_multiplier")->values = std::vector<double>(flush_multipliers.begin(), flush_multipliers.end());
    }

    // Update visibility of presets based on their compatibility with the active printer.
    // Always try to select a compatible print and filament preset to the current printer preset,
    // as the application may have been closed with an active "external" preset, which does not
    // exist.
    this->update_compatible(PresetSelectCompatibleType::Always);
    this->update_multi_material_filament_presets();

    if (initial_printer != nullptr && (preferred_printer == nullptr || initial_printer == preferred_printer)) {
        // Don't run the following code, as we want to activate default filament / SLA material profiles when installing and selecting a new printer.
        // Only run this code if just a filament / SLA material was installed by Config Wizard for an active Printer.
        auto printer_technology = printers.get_selected_preset().printer_technology();
        if (printer_technology == ptFFF && ! preferred_selection.filament.empty()) {
            std::string preferred_preset_name = get_preset_name_by_alias(Preset::Type::TYPE_FILAMENT, preferred_selection.filament);
            if (auto it = filaments.find_preset_internal(preferred_preset_name);
                it != filaments.end() && (it->name == preferred_preset_name ) && it->is_visible && it->is_compatible) {
                filaments.select_preset_by_name_strict(preferred_preset_name);
                this->filament_presets.front() = filaments.get_selected_preset_name();
            }
        } else if (printer_technology == ptSLA && ! preferred_selection.sla_material.empty()) {
            std::string preferred_preset_name = get_preset_name_by_alias(Preset::Type::TYPE_SLA_MATERIAL, preferred_selection.sla_material);
            if (auto it = sla_materials.find_preset_internal(preferred_preset_name);
                it != sla_materials.end() && it->is_visible && it->is_compatible)
                sla_materials.select_preset_by_name_strict(preferred_preset_name);
        }
    }

    std::string first_visible_filament_name;
    for (auto & fp : filament_presets) {
        if (auto it = filaments.find_preset_internal(fp); it == filaments.end() || !it->is_visible || !it->is_compatible) {
            if (first_visible_filament_name.empty())
                first_visible_filament_name = filaments.first_compatible().name;
            fp = first_visible_filament_name;
        }
    }

    const Preset& current_printer = printers.get_selected_preset();
    const Preset* base_printer = printers.get_preset_base(current_printer);
    bool use_default_nozzle_volume_type = true;
    if (base_printer) {
        std::string prev_nozzle_volume_type = config.get_nozzle_volume_types_from_config(base_printer->name);
        if (!prev_nozzle_volume_type.empty()) {
            ConfigOptionEnumsGeneric* nozzle_volume_type_option = project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type");
            if (nozzle_volume_type_option->deserialize(prev_nozzle_volume_type)) {
                use_default_nozzle_volume_type = false;
            }
        }
    }

    if (use_default_nozzle_volume_type) {
        project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type")->values = current_printer.config.option<ConfigOptionEnumsGeneric>("default_nozzle_volume_type")->values;
    }

    // Parse the initial physical printer name.
    std::string initial_physical_printer_name = remove_ini_suffix(config.get("presets", "physical_printer"));

    // Activate physical printer from the config
    if (!initial_physical_printer_name.empty())
        physical_printers.select_printer(initial_physical_printer_name);

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": finished, preferred printer_model_id %1%")%preferred_selection.printer_model_id;
}

// Export selections (current print, current filaments, current printer) into config.ini
// BBS: change directories by design
void PresetBundle::export_selections(AppConfig &config)
{
	assert(this->printers.get_edited_preset().printer_technology() != ptFFF || filament_presets.size() >= 1);
	//assert(this->printers.get_edited_preset().printer_technology() != ptFFF || filament_presets.size() > 1 || filaments.get_selected_preset_name() == filament_presets.front());
    config.clear_section("presets");
    auto printer_name = printers.get_selected_preset_name();
    config.set("presets", PRESET_PRINTER_NAME, printer_name);

    config.clear_printer_settings(printer_name);
    config.set_printer_setting(printer_name, PRESET_PRINTER_NAME, printer_name);
    config.set_printer_setting(printer_name, PRESET_PRINT_NAME, prints.get_selected_preset_name());
    config.set_printer_setting(printer_name, PRESET_FILAMENT_NAME,     filament_presets.front());
    config.set_printer_setting(printer_name, "curr_bed_type", config.get("curr_bed_type"));
    for (unsigned i = 1; i < filament_presets.size(); ++i) {
        char name[64];
        assert(!filament_presets[i].empty());
        sprintf(name, "filament_%02u", i);
        config.set_printer_setting(printer_name, name, filament_presets[i]);
    }
    // Load project config data into app config
    CNumericLocalesSetter locales_setter;
    std::string           filament_colors = boost::algorithm::join(project_config.option<ConfigOptionStrings>("filament_colour")->values, ",");
    config.set_printer_setting(printer_name, "filament_colors", filament_colors);

    // Load filament multi color data into app config
    std::string           filament_multi_colors = boost::algorithm::join(project_config.option<ConfigOptionStrings>("filament_multi_colour")->values, ",");
    config.set_printer_setting(printer_name, "filament_multi_colors", filament_multi_colors);

    // Load filament color type data into app config
    std::string           filament_color_types = boost::algorithm::join(project_config.option<ConfigOptionStrings>("filament_colour_type")->values, ",");
    config.set_printer_setting(printer_name, "filament_color_types", filament_color_types);

    // Load ams counts data into app config
    std::string           extruder_ams_count_str = boost::algorithm::join(save_extruder_ams_count_to_string(this->extruder_ams_counts), ",");
    config.set_printer_setting(printer_name, "extruder_ams_count", extruder_ams_count_str);

    std::string flush_volumes_matrix = boost::algorithm::join(project_config.option<ConfigOptionFloats>("flush_volumes_matrix")->values |
                                                             boost::adaptors::transformed(static_cast<std::string (*)(double)>(std::to_string)),
                                                         "|");
    config.set_printer_setting(printer_name, "flush_volumes_matrix", flush_volumes_matrix);
    std::string flush_volumes_vector = boost::algorithm::join(project_config.option<ConfigOptionFloats>("flush_volumes_vector")->values |
                                                             boost::adaptors::transformed(static_cast<std::string (*)(double)>(std::to_string)),
                                                         "|");
    config.set_printer_setting(printer_name, "flush_volumes_vector", flush_volumes_vector);


    std::string flush_multiplier_str = boost::algorithm::join(project_config.option<ConfigOptionFloats>("flush_multiplier")->values |
                                                                  boost::adaptors::transformed(static_cast<std::string (*)(double)>(std::to_string)),
                                                              "|");
    config.set_printer_setting(printer_name, "flush_multiplier", flush_multiplier_str);

    // BBS
    //config.set("presets", "sla_print",    sla_prints.get_selected_preset_name());
    //config.set("presets", "sla_material", sla_materials.get_selected_preset_name());
    //config.set("presets", "physical_printer", physical_printers.get_selected_full_printer_name());
    //BBS: add config related log
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": printer %1%, print %2%, filaments[0] %3% ")%printers.get_selected_preset_name() % prints.get_selected_preset_name() %filament_presets[0];
}

// BBS
void PresetBundle::set_num_filaments(unsigned int n, std::vector<std::string> new_colors) {
    int old_filament_count = this->filament_presets.size();
    if (n > old_filament_count && old_filament_count != 0)
        filament_presets.resize(n, filament_presets.back());
    else {
        filament_presets.resize(n);
    }
    ConfigOptionStrings* filament_color = project_config.option<ConfigOptionStrings>("filament_colour");
    filament_color->resize(n);
    ams_multi_color_filment.resize(n);
    // BBS set new filament color to new_color
    if (old_filament_count < n) {
        if (!new_colors.empty()) {
            for (int i = old_filament_count; i < n; i++) {
                filament_color->values[i] = new_colors[i - old_filament_count];
            }
        }
    }
    update_multi_material_filament_presets();
}
void PresetBundle::set_num_filaments(unsigned int n, std::string new_color)
{
    unsigned old_filament_count = this->filament_presets.size();
    if (n > old_filament_count && old_filament_count != 0)
        filament_presets.resize(n, filament_presets.back());
    else {
        filament_presets.resize(n);
    }
    ConfigOptionStrings* filament_color = project_config.option<ConfigOptionStrings>("filament_colour");
    ConfigOptionStrings *filament_multi_color = project_config.option<ConfigOptionStrings>("filament_multi_colour");
    ConfigOptionStrings* filament_color_type = project_config.option<ConfigOptionStrings>("filament_colour_type");
    ConfigOptionInts* filament_map = project_config.option<ConfigOptionInts>("filament_map");
    filament_color->resize(n);
    filament_multi_color->resize(n);
    filament_color_type->resize(n);
    filament_map->values.resize(n, 1);
    ams_multi_color_filment.resize(n);

    //BBS set new filament color to new_color
    if (old_filament_count < n) {
        if (!new_color.empty()) {
            for (unsigned i = old_filament_count; i < n; i++) {
                filament_color->values[i] = new_color;
                filament_multi_color->values[i] = new_color;
                filament_color_type->values[i]  = "1";  // default color type
            }
        }
    }

    update_multi_material_filament_presets();
}

void PresetBundle::update_num_filaments(unsigned int to_del_flament_id)
{
    unsigned old_filament_count = this->filament_presets.size();
    assert(to_del_flament_id < old_filament_count);
    filament_presets.erase(filament_presets.begin() + to_del_flament_id);

    // update edited_preset
    {
        Preset& edited_preset = filaments.get_edited_preset();
        bool    edited_preset_deleted = true;
        for (std::string filament_preset_name : filament_presets) {
            if (filament_preset_name == edited_preset.name) {
                edited_preset_deleted = false;
            }
        }
        if (edited_preset_deleted) {
            filaments.select_preset_by_name(filament_presets.front(), false);
        }
    }

    ConfigOptionStrings *filament_color = project_config.option<ConfigOptionStrings>("filament_colour");
    ConfigOptionStrings *filament_multi_color = project_config.option<ConfigOptionStrings>("filament_multi_colour");
    ConfigOptionStrings *filament_color_type  = project_config.option<ConfigOptionStrings>("filament_colour_type");
    ConfigOptionInts* filament_map = project_config.option<ConfigOptionInts>("filament_map");
    if (filament_color->values.size() > to_del_flament_id) {
        filament_color->values.erase(filament_color->values.begin() + to_del_flament_id);
        if (filament_map->values.size() > to_del_flament_id) {
            filament_map->values.erase(filament_map->values.begin() + to_del_flament_id);
        }
    }
    else {
        filament_color->values.resize(to_del_flament_id);
        filament_map->values.resize(to_del_flament_id, 1);
    }

    // lambda function to erase or resize the container
    auto erase_or_resize = [to_del_flament_id](auto& container) {
        if (container.size() > to_del_flament_id) {
            container.erase(container.begin() + to_del_flament_id);
        } else {
            container.resize(to_del_flament_id);
        }
    };

    erase_or_resize(filament_multi_color->values);
    erase_or_resize(filament_color_type->values);
    erase_or_resize(ams_multi_color_filment);

    update_multi_material_filament_presets(to_del_flament_id);
}


void PresetBundle::get_ams_cobox_infos(AMSComboInfo& combox_info)
{
    combox_info.clear();
    for (auto &entry : filament_ams_list) {
        auto &ams                  = entry.second;
        auto  filament_id          = ams.opt_string("filament_id", 0u);
        auto  filament_color       = ams.opt_string("filament_colour", 0u);
        auto  ams_name             = ams.opt_string("tray_name", 0u);
        auto  filament_changed     = !ams.has("filament_changed") || ams.opt_bool("filament_changed");
        auto  filament_multi_color = ams.opt<ConfigOptionStrings>("filament_multi_colour")->values;
        if (filament_id.empty()) {
            continue;
        }
        if (!filament_changed && this->filament_presets.size() > combox_info.ams_filament_presets.size()) {
            combox_info.ams_filament_presets.push_back(this->filament_presets[combox_info.ams_filament_presets.size()]);
            combox_info.ams_filament_colors.push_back(filament_color);
            combox_info.ams_multi_color_filment.push_back(filament_multi_color);
            combox_info.ams_names.push_back(ams_name);
            continue;
        }
        auto iter = std::find_if(filaments.begin(), filaments.end(),
                                 [this, &filament_id](auto &f) { return f.is_compatible && filaments.get_preset_base(f) == &f && f.filament_id == filament_id; });
        if (iter == filaments.end()) {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": filament_id %1% not found or system or compatible") % filament_id;
            auto filament_type = ams.opt_string("filament_type", 0u);
            if (!filament_type.empty()) {
                filament_type = "Generic " + filament_type;
                iter          = std::find_if(filaments.begin(), filaments.end(),
                                    [&filament_type](auto &f) { return f.is_compatible && f.is_system && boost::algorithm::starts_with(f.name, filament_type); });
            }
            if (iter == filaments.end()) {
                // Prefer old selection
                if (combox_info.ams_filament_presets.size() < this->filament_presets.size()) {
                    combox_info.ams_filament_presets.push_back(this->filament_presets[combox_info.ams_filament_presets.size()]);
                    combox_info.ams_filament_colors.push_back(filament_color);
                    combox_info.ams_multi_color_filment.push_back(filament_multi_color);
                    combox_info.ams_names.push_back(ams_name);
                    continue;
                }
                iter = std::find_if(filaments.begin(), filaments.end(), [&filament_type](auto &f) { return f.is_compatible && f.is_system; });
                if (iter == filaments.end())
                    continue;
            }
            filament_id = iter->filament_id;
        }
        combox_info.ams_filament_presets.push_back(iter->name);
        combox_info.ams_filament_colors.push_back(filament_color);
        combox_info.ams_multi_color_filment.push_back(filament_multi_color);
        combox_info.ams_names.push_back(ams_name);
    }
}

unsigned int PresetBundle::sync_ams_list(std::vector<std::pair<DynamicPrintConfig *,std::string>> &unknowns, bool use_map, std::map<int, AMSMapInfo> &maps,bool enable_append, MergeFilamentInfo &merge_info)
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "use_map:" << use_map << " enable_append:" << enable_append;
    std::vector<std::string> ams_filament_presets;
    std::vector<std::string> ams_filament_colors;
    std::vector<std::string> ams_filament_color_types;
    std::vector<AMSMapInfo>  ams_array_maps;
    ams_multi_color_filment.clear();
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": filament_ams_list size: %1%") % filament_ams_list.size();
    struct AmsInfo
    {
        bool valid{false};
        bool is_map{false};
        std::string filament_color  = "";
        std::string filament_color_type = "";
        std::string filament_preset = "";
        std::vector<std::string> mutli_filament_color;
    };
    auto is_double_extruder = get_printer_extruder_count() == 2;
    std::vector<AmsInfo> ams_infos;
    int                  index = 0;
    for (auto &entry : filament_ams_list) {
        auto & ams = entry.second;
        auto filament_id = ams.opt_string("filament_id", 0u);
        auto filament_color = ams.opt_string("filament_colour", 0u);
        auto filament_color_type = ams.opt_string("filament_colour_type", 0u);
        auto filament_changed = !ams.has("filament_changed") || ams.opt_bool("filament_changed");
        auto filament_multi_color = ams.opt<ConfigOptionStrings>("filament_multi_colour")->values;
        auto ams_id     = ams.opt_string("ams_id", 0u);
        auto slot_id    = ams.opt_string("slot_id", 0u);
        ams_infos.push_back({filament_id.empty() ? false : true,false, filament_color});
        AMSMapInfo temp = {ams_id, slot_id};
        ams_array_maps.push_back(temp);
        index++;
        if (filament_id.empty()) {
            if (use_map) {
                for (int j = maps.size() - 1; j >= 0; j--) {
                    if (maps[j].slot_id == slot_id && maps[j].ams_id == ams_id) {
                        maps.erase(j);
                    }
                }
                ams_filament_presets.push_back("Generic PLA");//for unknow matieral
                auto default_unknown_color = "#CECECE";
                ams_filament_colors.push_back(default_unknown_color);
                ams_filament_color_types.push_back("1");
                if (filament_multi_color.size() == 0) {
                    filament_multi_color.push_back(default_unknown_color);
                }
                ams_multi_color_filment.push_back(filament_multi_color);
            }
            continue;
        }
        if (!filament_changed && this->filament_presets.size() > ams_filament_presets.size()) {
            ams_filament_presets.push_back(this->filament_presets[ams_filament_presets.size()]);
            ams_filament_colors.push_back(filament_color);
            ams_filament_color_types.push_back(filament_color_type);
            ams_multi_color_filment.push_back(filament_multi_color);
            continue;
        }
        bool has_type = false;
        auto filament_type = ams.opt_string("filament_type", 0u);
        auto iter = std::find_if(filaments.begin(), filaments.end(), [this, &filament_id, &has_type, filament_type](auto &f) {
            has_type |= f.config.opt_string("filament_type", 0u) == filament_type;
            return f.is_compatible && filaments.get_preset_base(f) == &f && f.filament_id == filament_id; });
        if (iter == filaments.end()) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": filament_id %1% not found or system or compatible") % filament_id;
            if (!filament_type.empty()) {
                filament_type = "Generic " + filament_type;
                iter = std::find_if(filaments.begin(), filaments.end(), [&filament_type](auto &f) {
                    return f.is_compatible && f.is_system
                        && boost::algorithm::starts_with(f.name, filament_type);
                });
            }
            if (iter == filaments.end()) {
                // Prefer old selection
                if (ams_filament_presets.size() < this->filament_presets.size()) {
                    ams_filament_presets.push_back(this->filament_presets[ams_filament_presets.size()]);
                    ams_filament_colors.push_back(filament_color);
                    ams_filament_color_types.push_back(filament_color_type);
                    ams_multi_color_filment.push_back(filament_multi_color);
                    unknowns.emplace_back(&ams, has_type ? L("The filament may not be compatible with the current machine settings. Generic filament presets will be used.") :
                                                           L("The filament model is unknown. Still using the previous filament preset."));
                    continue;
                }
                iter = std::find_if(filaments.begin(), filaments.end(), [](auto &f) {
                    return f.is_compatible && f.is_system;
                });
                if (iter == filaments.end())
                    continue;
            }
            unknowns.emplace_back(&ams, boost::algorithm::starts_with(iter->name, filament_type) ?
                                            (has_type ? L("The filament may not be compatible with the current machine settings. Generic filament presets will be used.") :
                                                        L("The filament model is unknown. Generic filament presets will be used.")) :
                                            (has_type ? L("The filament may not be compatible with the current machine settings. A random filament preset will be used.") :
                                                        L("The filament model is unknown. A random filament preset will be used.")));
            filament_id = iter->filament_id;
        }
        ams_filament_presets.push_back(iter->name);
        ams_filament_colors.push_back(filament_color);
        ams_filament_color_types.push_back(filament_color_type);
        ams_multi_color_filment.push_back(filament_multi_color);
    }
    if (ams_filament_presets.empty())
        return 0;
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "get filament_colour and from config";
    ConfigOptionStrings *filament_color = project_config.option<ConfigOptionStrings>("filament_colour");
    ConfigOptionStrings *filament_color_type = project_config.option<ConfigOptionStrings>("filament_colour_type");
    ConfigOptionInts *   filament_map = project_config.option<ConfigOptionInts>("filament_map");
    if (use_map) {
        auto check_has_merge_info = [](std::map<int, AMSMapInfo> &maps, MergeFilamentInfo &merge_info, int exist_colors_size) {
            std::set<int> done;
            for (auto it_i = maps.begin(); it_i != maps.end(); ++it_i) {
                std::vector<int> same_ams;
                same_ams.emplace_back(it_i->first);
                for (auto it_j = std::next(it_i); it_j != maps.end(); ++it_j) {
                    if (done.find(it_j->first) != done.end()) {
                        continue;
                    }
                    if (it_i->second.slot_id == "" || it_i->second.ams_id == ""){
                        continue;
                    }
                    if (it_i->second.slot_id == it_j->second.slot_id && it_i->second.ams_id == it_j->second.ams_id) {
                        same_ams.emplace_back(it_j->first);
                        done.insert(it_j->first);
                    }
                }
                if (same_ams.size() > 1) {
                    merge_info.merges.emplace_back(same_ams);
                }
            }
        };
        check_has_merge_info(maps, merge_info,filament_color->values.size());
        auto get_map_index = [&ams_infos](const std::vector<AMSMapInfo> &infos, const AMSMapInfo &temp) {
            for (int i = 0; i < infos.size(); i++) {
                if (infos[i].slot_id == temp.slot_id && infos[i].ams_id == temp.ams_id) {
                    ams_infos[i].is_map = true;
                    return i;
                }
            }
            return -1;
        };
        std::vector<AmsInfo> need_append_colors;
        auto exist_colors = filament_color->values;
        auto exist_color_types = filament_color_type->values;
        auto exist_filament_presets = this->filament_presets;
        std::vector<std::vector<std::string>> exist_multi_color_filment;
        exist_multi_color_filment.resize(exist_colors.size());
        for (int i = 0; i < exist_colors.size(); i++) {
            exist_multi_color_filment[i] = {exist_colors[i]};
        }
        for (size_t i = 0; i < exist_colors.size(); i++) {
            if (maps.find(i) != maps.end()) {//mapping exist
                auto valid_index = get_map_index(ams_array_maps, maps[i]);
                if (valid_index >= 0 && valid_index < ams_filament_presets.size()) {
                    exist_colors[i]           = ams_filament_colors[valid_index];
                    exist_color_types[i]      = ams_filament_color_types[valid_index];
                    exist_filament_presets[i] = ams_filament_presets[valid_index];
                    exist_multi_color_filment[i] = ams_multi_color_filment[valid_index];
                } else {
                    BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "check error: array bound (mapping exist)";
                }
            }
        }
        for (size_t i = 0; i < ams_infos.size(); i++) {// check append
            if (ams_infos[i].valid) {
                if (i >= ams_filament_presets.size()) {
                    BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "check error: array bound (check append)";
                    continue;
                }
                ams_infos[i].filament_preset = ams_filament_presets[i];
                ams_infos[i].mutli_filament_color = ams_multi_color_filment[i];
                if (!ams_infos[i].is_map) {
                    need_append_colors.emplace_back(ams_infos[i]);
                    ams_filament_colors[i]     = "";
                    ams_filament_color_types[i] = "";
                    ams_filament_presets[i]    = "";
                    ams_multi_color_filment[i] = std::vector<std::string>();
                }
            }
            else {
                ams_filament_colors[i]     = "";
                ams_filament_color_types[i] = "";
                ams_filament_presets[i]    = "";
                ams_multi_color_filment[i] = std::vector<std::string>();
            }
        }
        //delete redundant color
        ams_filament_colors.erase(std::remove_if(ams_filament_colors.begin(), ams_filament_colors.end(), [](std::string &value) { return value.empty(); }),
                                  ams_filament_colors.end());
        ams_filament_color_types.erase(std::remove_if(ams_filament_color_types.begin(), ams_filament_color_types.end(), [](std::string &value) { return value.empty(); }),
                                       ams_filament_color_types.end());
        ams_filament_presets.erase(std::remove_if(ams_filament_presets.begin(), ams_filament_presets.end(), [](std::string &value) { return value.empty(); }),
                                   ams_filament_presets.end());
        ams_multi_color_filment.erase(std::remove_if(ams_multi_color_filment.begin(), ams_multi_color_filment.end(),
                                                     [](std::vector<std::string> &value) { return value.empty(); }),
                                      ams_multi_color_filment.end());
        if (need_append_colors.size() > 0 && enable_append) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "need_append_colors.size() > 0 && enable_append";
            auto get_idx_in_array = [](std::vector<std::string> &presets, std::vector<std::string> &colors, const std::string &preset, const std::string &color) -> int {
                for (size_t i = 0; i < presets.size(); i++) {
                    if (presets[i] == preset && colors[i] == color) {
                        return i;
                    }
                }
                return -1;
            };
            for (size_t i = 0; i < need_append_colors.size(); i++){
                if (exist_filament_presets.size() >= size_t(EnforcerBlockerType::ExtruderMax)){
                    break;
                }
                auto idx = get_idx_in_array(exist_filament_presets, exist_colors, need_append_colors[i].filament_preset, need_append_colors[i].filament_color);
                if (idx >= 0) {
                    continue;
                }
                exist_filament_presets.push_back(need_append_colors[i].filament_preset);
                exist_colors.push_back(need_append_colors[i].filament_color);
                exist_color_types.push_back(need_append_colors[i].filament_color_type);
                exist_multi_color_filment.push_back(need_append_colors[i].mutli_filament_color);
            }
        }
        filament_color->values = exist_colors;
        filament_color_type->values = exist_color_types;
        ams_multi_color_filment = exist_multi_color_filment;
        this->filament_presets = exist_filament_presets;
        filament_map->values.resize(exist_filament_presets.size(), 1);
    }
    else {//overwrite;
        filament_color->values = ams_filament_colors;
        filament_color_type->values = ams_filament_color_types;
        this->filament_presets = ams_filament_presets;
        filament_map->values.resize(ams_filament_colors.size(), 1);

        auto& print_config = this->prints.get_edited_preset().config;
        auto  support_filament_opt = print_config.option<ConfigOptionInt>("support_filament");
        auto support_interface_filament_opt = print_config.option<ConfigOptionInt>("support_interface_filament");
        if (support_filament_opt->value > ams_filament_color_types.size())
            support_filament_opt->value = 0;

        if (support_interface_filament_opt->value > ams_filament_color_types.size())
            support_interface_filament_opt->value = 0;
    }
    // Update ams_multi_color_filment
    update_filament_multi_color();
    update_multi_material_filament_presets();
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "finish sync ams list";
    return this->filament_presets.size();
}

void PresetBundle::update_filament_multi_color()
{
    std::vector<std::string> exsit_multi_colors;
    for (auto &fil_item : ams_multi_color_filment){
        if (fil_item.empty()) break;
        if (fil_item.size() == 1)
            exsit_multi_colors.push_back(fil_item[0]);
        else {
            std::string colors = "";
            for (auto &color : fil_item){
                   colors += color + " ";
            }
            colors.erase(colors.size() - 1); // remove last space
            exsit_multi_colors.push_back(colors);
        }
    }
    ConfigOptionStrings *filament_multi_colour = project_config.option<ConfigOptionStrings>("filament_multi_colour");
    filament_multi_colour->resize(exsit_multi_colors.size());
    filament_multi_colour->values = exsit_multi_colors;
}

std::vector<int> PresetBundle::get_used_tpu_filaments(const std::vector<int> &used_filaments)
{
    std::vector<int> tpu_filaments;
    for (size_t i = 0; i < this->filament_presets.size(); ++i) {
        auto iter = std::find(used_filaments.begin(), used_filaments.end(), i + 1);
        if (iter == used_filaments.end()) continue;

        std::string filament_name = this->filament_presets[i];
        for (int f_index = 0; f_index < this->filaments.size(); f_index++) {
            PresetCollection *filament_presets = &this->filaments;
            Preset           *preset           = &filament_presets->preset(f_index);
            int               size             = this->filaments.size();
            if (preset && filament_name.compare(preset->name) == 0) {
                std::string display_filament_type;
                std::string filament_type = preset->config.get_filament_type(display_filament_type);
                if (display_filament_type == "TPU") {
                    tpu_filaments.push_back(i);
                }
            }
        }
    }
    return tpu_filaments;
}

void PresetBundle::set_calibrate_printer(std::string name)
{
    if (name.empty()) {
        calibrate_filaments.clear();
        return;
    }
    if (!name.empty())
        calibrate_printer = printers.find_preset(name);
    const Preset &                printer_preset = calibrate_printer ? *calibrate_printer : printers.get_edited_preset();
    const PresetWithVendorProfile active_printer = printers.get_preset_with_vendor_profile(printer_preset);
    DynamicPrintConfig            config;
    config.set_key_value("printer_preset", new ConfigOptionString(active_printer.preset.name));
    const ConfigOption *opt = active_printer.preset.config.option("nozzle_diameter");
    if (opt) config.set_key_value("num_extruders", new ConfigOptionInt((int) static_cast<const ConfigOptionFloats *>(opt)->values.size()));
    calibrate_filaments.clear();
    for (size_t i = filaments.num_default_presets(); i < filaments.size(); ++i) {
        const Preset &                preset                          = filaments.m_presets[i];
        const PresetWithVendorProfile this_preset_with_vendor_profile = filaments.get_preset_with_vendor_profile(preset);
        bool                          is_compatible                   = is_compatible_with_printer(this_preset_with_vendor_profile, active_printer, &config);
        if (is_compatible) calibrate_filaments.insert(&preset);
    }
}

std::vector<std::vector<DynamicPrintConfig>> PresetBundle::get_extruder_filament_info() const
{
    std::vector<std::vector<DynamicPrintConfig>> filament_infos;
    int extruder_nums = get_printer_extruder_count();
    if (extruder_nums > 1) {
        filament_infos.resize(extruder_nums, std::vector<DynamicPrintConfig>());
        for (auto ams_item : filament_ams_list) {
            if (ams_item.first & 0x10000) { // right
                filament_infos[1].push_back(ams_item.second);
            } else { // left
                filament_infos[0].push_back(ams_item.second);
            }
        }
    }
    return filament_infos;
}

std::set<std::string> PresetBundle::get_printer_names_by_printer_type_and_nozzle(const std::string &printer_type, std::string nozzle_diameter_str, bool system_only)
{
    std::set<std::string> printer_names;
    if ("0.0" == nozzle_diameter_str || nozzle_diameter_str.empty()) {
        nozzle_diameter_str = "0.4";
    }
    std::ostringstream    stream;

    for (auto printer_it = this->printers.begin(); printer_it != this->printers.end(); printer_it++) {
        if (system_only && !printer_it->is_system) continue;

        ConfigOption *      printer_model_opt = printer_it->config.option("printer_model");
        ConfigOptionString *printer_model_str = dynamic_cast<ConfigOptionString *>(printer_model_opt);
        if (!printer_model_str) continue;

        // use printer_model as printer type
        if (printer_model_str->value != printer_type) continue;

        if (printer_it->name.find(nozzle_diameter_str) != std::string::npos) printer_names.insert(printer_it->name);
    }

    assert(printer_names.size() == 1);

    for (auto& printer_name : printer_names) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " " << __LINE__ << " printer name: " << printer_name;
    }

    return printer_names;
}

bool PresetBundle::check_filament_temp_equation_by_printer_type_and_nozzle_for_mas_tray(
    const std::string &printer_type, std::string& nozzle_diameter_str, std::string &setting_id, std::string &tag_uid, std::string &nozzle_temp_min, std::string &nozzle_temp_max, std::string& preset_setting_id)
{
    bool is_equation = true;

    std::map<std::string, std::vector<Preset const *>> filament_list = filaments.get_filament_presets();
    std::set<std::string> printer_names       = get_printer_names_by_printer_type_and_nozzle(printer_type, nozzle_diameter_str);

    for (const Preset *preset : filament_list.find(setting_id)->second) {
        if (tag_uid == "0" || (tag_uid.size() == 16 && tag_uid.substr(12, 2) == "01")) continue;
        if (preset && !preset->is_user()) continue;
        ConfigOption *       printer_opt  = const_cast<Preset *>(preset)->config.option("compatible_printers");
        ConfigOptionStrings *printer_strs = dynamic_cast<ConfigOptionStrings *>(printer_opt);
        bool                 compared = false;
        for (const std::string &printer_str : printer_strs->values) {
            if (printer_names.find(printer_str) != printer_names.end()) {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " " << __LINE__ << "nozzle temp matching: preset name: " << preset->name << " printer name: " << printer_str;
                // Compare only once
                if (!compared) {
                    compared                        = true;
                    bool          min_temp_equation = false, max_temp_equation = false;
                    int           min_nozzle_temp = std::stoi(nozzle_temp_min);
                    int           max_nozzle_temp = std::stoi(nozzle_temp_max);
                    ConfigOption *opt_min         = const_cast<Preset *>(preset)->config.option("nozzle_temperature_range_low");
                    if (opt_min) {
                        ConfigOptionInts *opt_min_ints = dynamic_cast<ConfigOptionInts *>(opt_min);
                        min_nozzle_temp                = opt_min_ints->get_at(0);
                        if (std::to_string(min_nozzle_temp) == nozzle_temp_min)
                            min_temp_equation = true;
                        else {
                            BOOST_LOG_TRIVIAL(info) << "tray min temp: " << nozzle_temp_min << " preset min temp: " << min_nozzle_temp;
                            nozzle_temp_min = std::to_string(min_nozzle_temp);
                        }
                    }
                    ConfigOption *opt_max = const_cast<Preset *>(preset)->config.option("nozzle_temperature_range_high");
                    if (opt_max) {
                        ConfigOptionInts *opt_max_ints = dynamic_cast<ConfigOptionInts *>(opt_max);
                        max_nozzle_temp                = opt_max_ints->get_at(0);
                        if (std::to_string(max_nozzle_temp) == nozzle_temp_max)
                            max_temp_equation = true;
                        else {
                            BOOST_LOG_TRIVIAL(info) << "tray max temp: " << nozzle_temp_max << " preset min temp: " << max_nozzle_temp;
                            nozzle_temp_max = std::to_string(max_nozzle_temp);
                        }
                    }
                    if (min_temp_equation && max_temp_equation) {
                        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " " << __LINE__ << "Determine if the temperature has changed: no changed";
                    } else {
                        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " " << __LINE__ << "Determine if the temperature has changed: has changed";
                        preset_setting_id = preset->setting_id;
                        is_equation = false;
                    }
                } else {
                    assert(false);
                }
            }
        }
    }
    return is_equation;
}

Preset *PresetBundle::get_similar_printer_preset(std::string printer_model, std::string printer_variant)
{
    if (printer_model.empty())
        printer_model = printers.get_selected_preset().config.opt_string("printer_model");
    if (printer_model.empty()) // ORCA ensure a compatible model exist. fixes switches to blank preset if preset has no inherited value
        return nullptr;
    auto printer_variant_old = printers.get_selected_preset().config.opt_string("printer_variant");
    std::map<std::string, Preset*> printer_presets;
    for (auto &preset : printers.m_presets) {
        if (printer_variant.empty() && !preset.is_system)
            continue;
        if (preset.config.opt_string("printer_model") == printer_model)
            printer_presets.insert({preset.name, &preset});
    }
    if (printer_presets.empty())
        return nullptr;
    auto prefer_printer = printers.get_selected_preset().alias; //.name ORCA use alias instead "name" for calling system presets. otherwise nozzle combo will not change printer presets if they custom named

    if (!printer_variant.empty())
        boost::replace_all(prefer_printer, printer_variant_old, printer_variant);
    else if (auto n = prefer_printer.find(printer_variant_old); n != std::string::npos)
        prefer_printer = printer_model + " " + printer_variant_old + prefer_printer.substr(n + printer_variant_old.length());
    if (auto iter = printer_presets.find(prefer_printer); iter != printer_presets.end()) {
        return iter->second;
    }
    if (printer_variant.empty())
        printer_variant = printer_variant_old;
    for (auto& preset : printer_presets) {
        if (preset.second->config.opt_string("printer_variant") == printer_variant)
            return preset.second;
    }
    return printer_presets.begin()->second;
}

//BBS: check whether this is the only edited filament
bool PresetBundle::is_the_only_edited_filament(unsigned int filament_index)
{
    unsigned n = this->filament_presets.size();
    if (filament_index >= n)
        return false;

    std::string name = this->filament_presets[filament_index];
    Preset& edited_preset = this->filaments.get_edited_preset();
    if (edited_preset.name != name)
        return false;

    unsigned index = 0;
    while (index < n)
    {
        if (index == filament_index) {
            index ++;
            continue;
        }
        std::string filament_preset = this->filament_presets[index];
        if (edited_preset.name == filament_preset)
            return false;
        else
            index ++;
    }
    return true;
}

void PresetBundle::reset_default_nozzle_volume_type()
{
    Preset& current_printer = this->printers.get_edited_preset();
    this->project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type")->values = current_printer.config.option<ConfigOptionEnumsGeneric>("default_nozzle_volume_type")->values;
}

int PresetBundle::get_printer_extruder_count() const
{
    const Preset& printer_preset = this->printers.get_edited_preset();

    int count = printer_preset.config.option<ConfigOptionFloats>("nozzle_diameter")->values.size();

    return count;
}

bool PresetBundle::support_different_extruders()
{
    Preset& printer_preset = this->printers.get_edited_preset();
    int extruder_count;
    bool supported = printer_preset.config.support_different_extruders(extruder_count);

    return supported;
}

DynamicPrintConfig PresetBundle::full_config(bool apply_extruder, std::optional<std::vector<int>>filament_maps) const
{
    return (this->printers.get_edited_preset().printer_technology() == ptFFF) ?
        this->full_fff_config(apply_extruder, filament_maps) :
        this->full_sla_config();
}

DynamicPrintConfig PresetBundle::full_config_secure(std::optional<std::vector<int>>filament_maps) const
{
    DynamicPrintConfig config = this->full_fff_config(false, filament_maps);
    //FIXME legacy, the keys should not be there after conversion to a Physical Printer profile.
    config.erase("print_host");
    config.erase("print_host_webui");
    config.erase("printhost_apikey");
    config.erase("printhost_cafile");    
    config.erase("printhost_user");    
    config.erase("printhost_password");    
    config.erase("printhost_port");    
    return config;
}

const std::set<std::string> ignore_settings_list ={
    "inherits",
    "print_settings_id", "filament_settings_id", "printer_settings_id"
};

DynamicPrintConfig PresetBundle::full_fff_config(bool apply_extruder, std::optional<std::vector<int>> filament_maps_new) const
{
    DynamicPrintConfig out;
    out.apply(FullPrintConfig::defaults());
    out.apply(this->prints.get_edited_preset().config);
    // Add the default filament preset to have the "filament_preset_id" defined.
	out.apply(this->filaments.default_preset().config);
	out.apply(this->printers.get_edited_preset().config);
    out.apply(this->project_config);

    // BBS
    size_t  num_filaments = this->filament_presets.size();

    std::vector<int> filament_maps = out.option<ConfigOptionInts>("filament_map")->values;
    if (filament_maps_new.has_value())
        filament_maps = *filament_maps_new;
    //in some middle state, they may be different
    if (filament_maps.size() != num_filaments) {
        filament_maps.resize(num_filaments, 1);
    }
    else {
        assert(filament_maps.size() == num_filaments);
    }

    auto* extruder_diameter = dynamic_cast<const ConfigOptionFloats*>(out.option("nozzle_diameter"));
    // Collect the "compatible_printers_condition" and "inherits" values over all presets (print, filaments, printers) into a single vector.
    std::vector<std::string> compatible_printers_condition;
    std::vector<std::string> compatible_prints_condition;
    std::vector<std::string> inherits;
    std::vector<std::string> filament_ids;
    std::vector<std::string> print_compatible_printers;
    //BBS: add logic for settings check between different system presets
    std::vector<std::string> different_settings;
    std::string different_print_settings, different_printer_settings;
    compatible_printers_condition.emplace_back(this->prints.get_edited_preset().compatible_printers_condition());

    const ConfigOptionStrings* compatible_printers =  (const_cast<PresetBundle*>(this))->prints.get_edited_preset().config.option<ConfigOptionStrings>("compatible_printers", false);
    if (compatible_printers)
        print_compatible_printers = compatible_printers->values;
    //BBS: add logic for settings check between different system presets
    std::string print_inherits = this->prints.get_edited_preset().inherits();
    inherits                     .emplace_back(print_inherits);
    const Preset* print_parent_preset =  this->prints.get_selected_preset_parent();
    if (print_parent_preset) {
        std::vector<std::string> dirty_options = this->prints.dirty_options_without_option_list(&(this->prints.get_edited_preset()), print_parent_preset, ignore_settings_list, false);
        if (!dirty_options.empty()) {
            different_print_settings = Slic3r::escape_strings_cstyle(dirty_options);
        }
    }
    different_settings.emplace_back(different_print_settings);

    //BBS: update printer config related with variants
    if (apply_extruder) {
        out.update_values_to_printer_extruders(out, printer_options_with_variant_1, "printer_extruder_id", "printer_extruder_variant");
        out.update_values_to_printer_extruders(out, printer_options_with_variant_2, "printer_extruder_id", "printer_extruder_variant", 2);
        //update print config related with variants
        out.update_values_to_printer_extruders(out, print_options_with_variant, "print_extruder_id", "print_extruder_variant");
    }

    if (num_filaments <= 1) {
        //BBS: update filament config related with variants
        DynamicPrintConfig filament_config = this->filaments.get_edited_preset().config;
        if (apply_extruder)
            filament_config.update_values_to_printer_extruders(out, filament_options_with_variant, "", "filament_extruder_variant", 1, filament_maps[0]);
        out.apply(filament_config);
        compatible_printers_condition.emplace_back(this->filaments.get_edited_preset().compatible_printers_condition());
        compatible_prints_condition  .emplace_back(this->filaments.get_edited_preset().compatible_prints_condition());
        //BBS: add logic for settings check between different system presets
        //std::string filament_inherits = this->filaments.get_edited_preset().inherits();
        std::string current_preset_name = this->filament_presets[0];
        const Preset* preset = this->filaments.find_preset(current_preset_name, true);
        std::string filament_inherits = preset->inherits();
        inherits                     .emplace_back(filament_inherits);
        filament_ids.emplace_back(this->filaments.get_edited_preset().filament_id);

        std::string different_filament_settings;
        const Preset* filament_parent_preset =  this->filaments.get_selected_preset_parent();
        if (filament_parent_preset) {
            std::vector<std::string> dirty_options = this->filaments.dirty_options_without_option_list(&(this->filaments.get_edited_preset()), filament_parent_preset, ignore_settings_list, false);
            if (!dirty_options.empty()) {
                different_filament_settings = Slic3r::escape_strings_cstyle(dirty_options);
            }
        }

        different_settings.emplace_back(different_filament_settings);

        std::vector<int>& filament_self_indice = out.option<ConfigOptionInts>("filament_self_index", true)->values;
        int index_size = out.option<ConfigOptionStrings>("filament_extruder_variant")->size();
        filament_self_indice.resize(index_size, 1);
    } else {
        // Retrieve filament presets and build a single config object for them.
        // First collect the filament configurations based on the user selection of this->filament_presets.
        // Here this->filaments.find_preset() and this->filaments.first_visible() return the edited copy of the preset if active.
        std::vector<const DynamicPrintConfig*> filament_configs;
        std::vector<const Preset*> filament_presets;
        for (const std::string& filament_preset_name : this->filament_presets) {
            const Preset* preset = this->filaments.find_preset(filament_preset_name, true);
            filament_presets.emplace_back(preset);
            filament_configs.emplace_back(&(preset->config));
        }
        while (filament_configs.size() < num_filaments) {
            const Preset* preset = &this->filaments.first_visible();
            filament_presets.emplace_back(preset);
            filament_configs.emplace_back(&(preset->config));
        }
        for (int index = 0; index < num_filaments; index++) {
            const DynamicPrintConfig *cfg = filament_configs[index];
            const Preset *preset = filament_presets[index];
            // The compatible_prints/printers_condition() returns a reference to configuration key, which may not yet exist.
            DynamicPrintConfig &cfg_rw = *const_cast<DynamicPrintConfig*>(cfg);
            compatible_printers_condition.emplace_back(Preset::compatible_printers_condition(cfg_rw));
            compatible_prints_condition  .emplace_back(Preset::compatible_prints_condition(cfg_rw));

            //BBS: add logic for settings check between different system presets
            std::string filament_inherits = Preset::inherits(cfg_rw);
            inherits                     .emplace_back(filament_inherits);
            filament_ids.emplace_back(preset->filament_id);
            std::string different_filament_settings;

            const Preset* filament_parent_preset = nullptr;
            if (preset->is_system || preset->is_default) {
                bool is_selected = this->filaments.get_selected_preset_name() == preset->name;
                if (is_selected) {
                    //use the real preset
                    filament_parent_preset = const_cast<PresetBundle*>(this)->filaments.find_preset(preset->name, false, true);
                }
                else {
                    filament_parent_preset = preset;
                }
            }
            else if (!filament_inherits.empty())
                filament_parent_preset =  const_cast<PresetBundle*>(this)->filaments.find_preset(filament_inherits, false, true);

            if (filament_parent_preset) {
                std::vector<std::string> dirty_options = cfg_rw.diff(filament_parent_preset->config);
                if (!dirty_options.empty()) {
                    auto iter = dirty_options.begin();
                    while (iter != dirty_options.end()) {
                        if (ignore_settings_list.find(*iter) != ignore_settings_list.end()) {
                            iter = dirty_options.erase(iter);
                        }
                        else {
                            ++iter;
                        }
                    }
                    different_filament_settings = Slic3r::escape_strings_cstyle(dirty_options);
                }
            }

            different_settings.emplace_back(different_filament_settings);
        }

        std::vector<DynamicPrintConfig> filament_temp_configs;
        filament_temp_configs.resize(num_filaments);
        for (size_t i = 0; i < num_filaments; ++i) {
            filament_temp_configs[i] = *(filament_configs[i]);
            if (apply_extruder)
                filament_temp_configs[i].update_values_to_printer_extruders(out, filament_options_with_variant, "", "filament_extruder_variant", 1, filament_maps[i]);
        }

        // loop through options and apply them to the resulting config.
        std::vector<int> filament_variant_count(num_filaments, 1);
        for (const t_config_option_key &key : this->filaments.default_preset().config.keys()) {
			if (key == "compatible_prints" || key == "compatible_printers")
				continue;
            // Get a destination option.
            ConfigOption *opt_dst = out.option(key, false);
            if (opt_dst->is_scalar()) {
                // Get an option, do not create if it does not exist.
                const ConfigOption *opt_src = filament_temp_configs.front().option(key);
                if (opt_src != nullptr)
                    opt_dst->set(opt_src);
            } else {
                // BBS
                ConfigOptionVectorBase* opt_vec_dst = static_cast<ConfigOptionVectorBase*>(opt_dst);
                {
                    if (apply_extruder) {
                        std::vector<const ConfigOption*> filament_opts(num_filaments, nullptr);
                        // Setting a vector value from all filament_configs.
                        for (size_t i = 0; i < filament_opts.size(); ++i)
                            filament_opts[i] = filament_temp_configs[i].option(key);
                        opt_vec_dst->set(filament_opts);
                    }
                    else {
                        for (size_t i = 0; i < num_filaments; ++i) {
                            const ConfigOptionVectorBase* filament_option = static_cast<const ConfigOptionVectorBase*>(filament_temp_configs[i].option(key));
                            if (i == 0)
                                opt_vec_dst->set(filament_option);
                            else
                                opt_vec_dst->append(filament_option);

                            if (key == "filament_extruder_variant")
                                filament_variant_count[i] = filament_option->size();
                        }
                    }
                }
            }
        }

        if (!apply_extruder) {
            //append filament_self_index
            std::vector<int>& filament_self_indice = out.option<ConfigOptionInts>("filament_self_index", true)->values;
            int index_size = out.option<ConfigOptionStrings>("filament_extruder_variant")->size();
            filament_self_indice.resize(index_size, 1);
            int k = 0;
            for (size_t i = 0; i < num_filaments; i++) {
                for (size_t j = 0; j < filament_variant_count[i]; j++) {
                    filament_self_indice[k++] = i + 1;
                }
            }
        }
    }

    //BBS: add logic for settings check between different system presets
    std::string printer_inherits = this->printers.get_edited_preset().inherits();
    // Don't store the "compatible_printers_condition" for the printer profile, there is none.
    inherits                     .emplace_back(printer_inherits);
    const Preset* printer_parent_preset =  this->printers.get_selected_preset_parent();
    if (printer_parent_preset) {
        std::vector<std::string> dirty_options = this->printers.dirty_options_without_option_list(&(this->printers.get_edited_preset()), printer_parent_preset, ignore_settings_list, false);
        if (!dirty_options.empty()) {
            different_printer_settings = Slic3r::escape_strings_cstyle(dirty_options);
        }
    }
    different_settings.emplace_back(different_printer_settings);

    // These value types clash between the print and filament profiles. They should be renamed.
    out.erase("compatible_prints");
    out.erase("compatible_prints_condition");
    out.erase("compatible_printers");
    out.erase("compatible_printers_condition");
    out.erase("inherits");
    //BBS: add logic for settings check between different system presets
    out.erase("different_settings_to_system");

    static const char* keys[] = {"support_filament", "support_interface_filament", "wipe_tower_filament"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++ i) {
        std::string key = std::string(keys[i]);
        auto *opt = dynamic_cast<ConfigOptionInt*>(out.option(key, false));
        assert(opt != nullptr);
        opt->value = boost::algorithm::clamp<int>(opt->value, 0, int(num_filaments));
    }

    static const char* keys_1based[] = {"wall_filament", "sparse_infill_filament", "solid_infill_filament"};
    for (size_t i = 0; i < sizeof(keys_1based) / sizeof(keys_1based[0]); ++ i) {
        std::string key = std::string(keys_1based[i]);
        auto *opt = dynamic_cast<ConfigOptionInt*>(out.option(key, false));
        assert(opt != nullptr);
        if(opt->value < 1 || opt->value > int(num_filaments))
            opt->value = 1;
    }
    out.option<ConfigOptionString >("print_settings_id",    true)->value  = this->prints.get_selected_preset_name();
    out.option<ConfigOptionStrings>("filament_settings_id", true)->values = this->filament_presets;
    out.option<ConfigOptionString >("printer_settings_id",  true)->value  = this->printers.get_selected_preset_name();
    out.option<ConfigOptionStrings>("filament_ids", true)->values = filament_ids;
    out.option<ConfigOptionInts>("filament_map", true)->values = filament_maps;
    // Serialize the collected "compatible_printers_condition" and "inherits" fields.
    // There will be 1 + num_exturders fields for "inherits" and 2 + num_extruders for "compatible_printers_condition" stored.
    // The vector will not be stored if all fields are empty strings.
    auto add_if_some_non_empty = [&out](std::vector<std::string> &&values, const std::string &key) {
        bool nonempty = false;
        for (const std::string &v : values)
            if (! v.empty()) {
                nonempty = true;
                break;
            }
        if (nonempty)
            out.set_key_value(key, new ConfigOptionStrings(std::move(values)));
    };
    add_if_some_non_empty(std::move(compatible_printers_condition), "compatible_machine_expression_group");
    add_if_some_non_empty(std::move(compatible_prints_condition),   "compatible_process_expression_group");
    add_if_some_non_empty(std::move(inherits),                      "inherits_group");
    //BBS: add logic for settings check between different system presets
    add_if_some_non_empty(std::move(different_settings),            "different_settings_to_system");
    add_if_some_non_empty(std::move(print_compatible_printers),     "print_compatible_printers");
    out.option<ConfigOptionStrings>("extruder_ams_count", true)->values   = save_extruder_ams_count_to_string(this->extruder_ams_counts);

	out.option<ConfigOptionEnumGeneric>("printer_technology", true)->value = ptFFF;
    return out;
}

DynamicPrintConfig PresetBundle::full_sla_config() const
{
    DynamicPrintConfig out;
    out.apply(SLAFullPrintConfig::defaults());
    out.apply(this->sla_prints.get_edited_preset().config);
    out.apply(this->sla_materials.get_edited_preset().config);
    out.apply(this->printers.get_edited_preset().config);
    // There are no project configuration values as of now, the project_config is reserved for FFF printers.
//    out.apply(this->project_config);

    // Collect the "compatible_printers_condition" and "inherits" values over all presets (sla_prints, sla_materials, printers) into a single vector.
    std::vector<std::string> compatible_printers_condition;
	std::vector<std::string> compatible_prints_condition;
    std::vector<std::string> inherits;
    compatible_printers_condition.emplace_back(this->sla_prints.get_edited_preset().compatible_printers_condition());
	inherits					 .emplace_back(this->sla_prints.get_edited_preset().inherits());
    compatible_printers_condition.emplace_back(this->sla_materials.get_edited_preset().compatible_printers_condition());
	compatible_prints_condition  .emplace_back(this->sla_materials.get_edited_preset().compatible_prints_condition());
    inherits                     .emplace_back(this->sla_materials.get_edited_preset().inherits());
    inherits                     .emplace_back(this->printers.get_edited_preset().inherits());

    // These two value types clash between the print and filament profiles. They should be renamed.
    out.erase("compatible_printers");
    out.erase("compatible_printers_condition");
    out.erase("inherits");

    out.option<ConfigOptionString >("sla_print_settings_id",    true)->value  = this->sla_prints.get_selected_preset_name();
    out.option<ConfigOptionString >("sla_material_settings_id", true)->value  = this->sla_materials.get_selected_preset_name();
    out.option<ConfigOptionString >("printer_settings_id",      true)->value  = this->printers.get_selected_preset_name();

    // Serialize the collected "compatible_printers_condition" and "inherits" fields.
    // There will be 1 + num_exturders fields for "inherits" and 2 + num_extruders for "compatible_printers_condition" stored.
    // The vector will not be stored if all fields are empty strings.
    auto add_if_some_non_empty = [&out](std::vector<std::string> &&values, const std::string &key) {
        bool nonempty = false;
        for (const std::string &v : values)
            if (! v.empty()) {
                nonempty = true;
                break;
            }
        if (nonempty)
            out.set_key_value(key, new ConfigOptionStrings(std::move(values)));
    };
    add_if_some_non_empty(std::move(compatible_printers_condition), "compatible_machine_expression_group");
    add_if_some_non_empty(std::move(compatible_prints_condition),   "compatible_process_expression_group");
    add_if_some_non_empty(std::move(inherits),                      "inherits_group");

	out.option<ConfigOptionEnumGeneric>("printer_technology", true)->value = ptSLA;
	return out;
}

// Load an external config file containing the print, filament and printer presets.
// Instead of a config file, a G-code may be loaded containing the full set of parameters.
// In the future the configuration will likely be read from an AMF file as well.
// If the file is loaded successfully, its print / filament / printer profiles will be activated.
ConfigSubstitutions PresetBundle::load_config_file(const std::string &path, ForwardCompatibilitySubstitutionRule compatibility_rule)
{
	if (is_gcode_file(path)) {
		DynamicPrintConfig config;
        //BBS: add config related logs
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" enter, gcodefile %1%, compatibility_rule %2%")%path %compatibility_rule;
		config.apply(FullPrintConfig::defaults());
        ConfigSubstitutions config_substitutions = config.load_from_gcode_file(path, compatibility_rule);
        Preset::normalize(config);
		load_config_file_config(path, true, std::move(config));
		return config_substitutions;
	}

    //BBS: add config related logs
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" can not load config file %1% not from gcode")%path ;
    throw Slic3r::RuntimeError(std::string("Unknown configuration file: ") + path);
    
    return ConfigSubstitutions{};
}


//some filament presets split from one to sperate ones
//following map recording these filament presets
//for example: previously ''Bambu PLA Basic @BBL H2D 0.6 nozzle' was saved in ''Bambu PLA Basic @BBL H2D' with 0.4
static std::map<std::string, std::map<std::string, std::string>> filament_preset_convert = {
{"Bambu Lab H2D 0.6 nozzle", {{"Bambu PLA Basic @BBL H2D", "Bambu PLA Basic @BBL H2D 0.6 nozzle"},
                              {"Bambu PLA Matte @BBL H2D", "Bambu PLA Matte @BBL H2D 0.6 nozzle"},
                              {"Bambu ABS @BBL H2D", "Bambu ABS @BBL H2D 0.6 nozzle"}}},
{"Bambu Lab H2D 0.8 nozzle", {{"Bambu PETG HF @BBL H2D 0.6 nozzle", "Bambu PETG HF @BBL H2D 0.8 nozzle"},
                              {"Bambu ASA @BBL H2D 0.6 nozzle", "Bambu ASA @BBL H2D 0.8 nozzle"}}}
};

//convert the old filament preset to new one after split
static void convert_filament_preset_name(std::string& machine_name, std::string& filament_name)
{
    auto machine_iter = filament_preset_convert.find(machine_name);
    if (machine_iter != filament_preset_convert.end())
    {
        std::map<std::string, std::string>& filament_maps = machine_iter->second;
        auto filament_iter = filament_maps.find(filament_name);
        if (filament_iter != filament_maps.end())
        {
            filament_name = filament_iter->second;
        }
    }
}
// Load a config file from a boost property_tree. This is a private method called from load_config_file.
// is_external == false on if called from ConfigWizard
void PresetBundle::load_config_file_config(const std::string &name_or_path, bool is_external, DynamicPrintConfig &&config, Semver file_version, bool selected)
{
    PrinterTechnology printer_technology = Preset::printer_technology(config);

    auto clear_compatible_printers = [](DynamicPrintConfig& config){
        ConfigOption *opt_compatible = config.optptr("compatible_printers");
        if (opt_compatible != nullptr) {
            assert(opt_compatible->type() == coStrings);
            if (opt_compatible->type() == coStrings)
                static_cast<ConfigOptionStrings*>(opt_compatible)->values.clear();
        }
    };
    clear_compatible_printers(config);

#if 0
    size_t num_extruders = (printer_technology == ptFFF) ?
        std::min(config.option<ConfigOptionFloats>("nozzle_diameter"  )->values.size(),
                 config.option<ConfigOptionFloats>("filament_diameter")->values.size()) :
		// 1 SLA material
        1;
#else
    // BBS: use filament_colour insteadof filament_settings_id, filament_settings_id sometimes is not generated
    ConfigOptionStrings* filament_colour_option = config.option<ConfigOptionStrings>("filament_colour");
    size_t num_filaments = filament_colour_option?filament_colour_option->size():0;
    if (num_filaments == 0)
        throw Slic3r::RuntimeError(std::string("Invalid configuration file: ") + name_or_path);
#endif

    //BBS: add config related logs
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": , name_or_path %1%, is_external %2%, num_filaments %3%") % name_or_path % is_external % num_filaments;
    // Make a copy of the "compatible_machine_expression_group" and "inherits_group" vectors, which
    // accumulate values over all presets (print, filaments, printers).
    // These values will be distributed into their particular presets when loading.
    std::vector<std::string> compatible_printers_condition_values   = std::move(config.option<ConfigOptionStrings>("compatible_machine_expression_group", true)->values);
    std::vector<std::string> compatible_prints_condition_values     = std::move(config.option<ConfigOptionStrings>("compatible_process_expression_group",   true)->values);
    std::vector<std::string> inherits_values                        = std::move(config.option<ConfigOptionStrings>("inherits_group", true)->values);
    std::vector<std::string> filament_ids                           = std::move(config.option<ConfigOptionStrings>("filament_ids", true)->values);
    std::vector<std::string> print_compatible_printers              = std::move(config.option<ConfigOptionStrings>("print_compatible_printers", true)->values);
    //BBS: add different settings check logic
    bool has_different_settings_to_system                           = config.option("different_settings_to_system")?true:false;
    std::vector<std::string> different_values                       = std::move(config.option<ConfigOptionStrings>("different_settings_to_system", true)->values);
    std::string &compatible_printers_condition  = Preset::compatible_printers_condition(config);
    std::string &compatible_prints_condition    = Preset::compatible_prints_condition(config);
    std::string &inherits                       = Preset::inherits(config);
    compatible_printers_condition_values.resize(num_filaments + 2, std::string());
    compatible_prints_condition_values.resize(num_filaments, std::string());
    inherits_values.resize(num_filaments + 2, std::string());
    different_values.resize(num_filaments + 2, std::string());
    filament_ids.resize(num_filaments, std::string());
    // The "default_filament_profile" will be later extracted into the printer profile.
	switch (printer_technology) {
	case ptFFF:
		config.option<ConfigOptionString>("default_print_profile", true);
        config.option<ConfigOptionStrings>("default_filament_profile", true);
		break;
	case ptSLA:
		config.option<ConfigOptionString>("default_sla_print_profile", true);
		config.option<ConfigOptionString>("default_sla_material_profile", true);
		break;
    default: break;
	}

    bool process_multi_extruder = false;
    std::vector<int> filament_variant_index;
    size_t extruder_variant_count;
    if (!config.option<ConfigOptionInts>("filament_self_index")) {
        std::vector<int>& filament_self_indice = config.option<ConfigOptionInts>("filament_self_index", true)->values;
        filament_self_indice.resize(num_filaments);
        for (int index = 0; index < num_filaments; index++)
            filament_self_indice[index] = index + 1;
    }
    std::vector<int> filament_self_indice = std::move(config.option<ConfigOptionInts>("filament_self_index")->values);
    if (config.option("extruder_variant_list")) {
        //3mf support multiple extruder logic
        size_t extruder_count = config.option<ConfigOptionFloats>("nozzle_diameter")->values.size();
        extruder_variant_count = config.option<ConfigOptionStrings>("filament_extruder_variant", true)->size();
        if ((extruder_variant_count != filament_self_indice.size())
            || (extruder_variant_count < num_filaments)) {
            assert(false);
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": invalid config file %1%, can not find suitable filament_extruder_variant or filament_self_index") % name_or_path;
            throw Slic3r::RuntimeError(std::string("Invalid configuration file: ") + name_or_path);
        }
        if (num_filaments != extruder_variant_count) {
            process_multi_extruder = true;
            filament_variant_index.resize(num_filaments, 0);

            size_t cur_filament_id = 1;
            for (size_t index = 0; index < filament_self_indice.size(); index++) {
                if (filament_self_indice[index] == cur_filament_id) {
                    filament_variant_index[cur_filament_id - 1] = index;
                    cur_filament_id++;
                    if (cur_filament_id > num_filaments)
                        break;
                }
            }
        }
    }
    //no need to parse extruder_ams_count
    std::vector<std::string> extruder_ams_count = std::move(config.option<ConfigOptionStrings>("extruder_ams_count", true)->values);
    config.erase("extruder_ams_count");
    if (this->extruder_ams_counts.empty())
        this->extruder_ams_counts = get_extruder_ams_count(extruder_ams_count);


    // 1) Create a name from the file name.
    // Keep the suffix (.ini, .gcode, .amf, .3mf etc) to differentiate it from the normal profiles.
    std::string name = is_external ? boost::filesystem::path(name_or_path).filename().string() : name_or_path;

    // 2) If the loading succeeded, split and load the config into print / filament / printer settings.
    // First load the print and printer presets.

	auto load_preset =
		[&config, &inherits, &inherits_values,
         &compatible_printers_condition, &compatible_printers_condition_values,
         &compatible_prints_condition, &compatible_prints_condition_values,
         is_external, &name, &name_or_path, file_version, selected]
		(PresetCollection &presets, size_t idx, const std::string &key, const std::set<std::string> &different_keys, std::string filament_id) {
		// Split the "compatible_printers_condition" and "inherits" values one by one from a single vector to the print & printer profiles.
		inherits = inherits_values[idx];
		compatible_printers_condition = compatible_printers_condition_values[idx];
        if (idx > 0 && idx - 1 < compatible_prints_condition_values.size())
            compatible_prints_condition = compatible_prints_condition_values[idx - 1];
        //BBS: add config related logs
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": , name %1%, is_external %2%, inherits %3%")%name %is_external %inherits;
		if (is_external)
			presets.load_external_preset(name_or_path, name, config.opt_string(key, true), config, different_keys, PresetCollection::LoadAndSelect::Always, file_version, filament_id);
		else
            presets.load_preset(presets.path_from_name(name, inherits.empty()), name, config, selected, file_version).save(nullptr);
	};

    switch (Preset::printer_technology(config)) {
    case ptFFF:
    {
        //BBS: add different settings logic
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": load print preset from print_settings_id");
        std::vector<std::string> print_different_keys_vector;
        std::string print_different_settings = different_values[0];
        Slic3r::unescape_strings_cstyle(print_different_settings, print_different_keys_vector);
        std::set<std::string> print_different_keys_set(print_different_keys_vector.begin(), print_different_keys_vector.end());
        //if (!has_different_settings_to_system) {
        //    print_different_keys_set.clear();
        //}
        //else
            print_different_keys_set.insert(ignore_settings_list.begin(), ignore_settings_list.end());
        if (!print_compatible_printers.empty()) {
            ConfigOptionStrings* compatible_printers = config.option<ConfigOptionStrings>("compatible_printers", true);
            compatible_printers->values = print_compatible_printers;
        }

        load_preset(this->prints, 0, "print_settings_id", print_different_keys_set, std::string());

        //clear compatible printers
        clear_compatible_printers(config);

        std::vector<std::string> printer_different_keys_vector;
        std::string printer_different_settings = different_values[num_filaments + 1];
        Slic3r::unescape_strings_cstyle(printer_different_settings, printer_different_keys_vector);
        std::set<std::string> printer_different_keys_set(printer_different_keys_vector.begin(), printer_different_keys_vector.end());
        //if (!has_different_settings_to_system) {
        //    printer_different_keys_set.clear();
        //}
        //else
            printer_different_keys_set.insert(ignore_settings_list.begin(), ignore_settings_list.end());
        //BBS: add config related logs
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": load printer preset from printer_settings_id");
        load_preset(this->printers, num_filaments + 1, "printer_settings_id", printer_different_keys_set, std::string());

        // 3) Now load the filaments. If there are multiple filament presets, split them and load them.
        auto old_filament_profile_names = config.option<ConfigOptionStrings>("filament_settings_id", true);
        old_filament_profile_names->values.resize(num_filaments, std::string());

        auto old_machine_profile_name = config.option<ConfigOptionString>("printer_settings_id", true);

        if (num_filaments <= 1) {
            // Split the "compatible_printers_condition" and "inherits" values from the cummulative vectors to separate filament presets.
            inherits                      = inherits_values[1];
            compatible_printers_condition = compatible_printers_condition_values[1];
			compatible_prints_condition   = compatible_prints_condition_values.front();
			Preset                *loaded = nullptr;

            //BBS: add different settings logic
            std::vector<std::string> filament_different_keys_vector;
            std::string filament_different_settings = different_values[1];
            Slic3r::unescape_strings_cstyle(filament_different_settings, filament_different_keys_vector);
            std::set<std::string> filament_different_keys_set(filament_different_keys_vector.begin(), filament_different_keys_vector.end());
            //if (!has_different_settings_to_system) {
            //    filament_different_keys_set.clear();
            //}
            //else
                filament_different_keys_set.insert(ignore_settings_list.begin(), ignore_settings_list.end());

            std::string filament_id = filament_ids[0];
            //BBS: add config related logs
            BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": load single filament preset from filament_settings_id");
            if (is_external) {
                if (inherits.empty())
                    convert_filament_preset_name(old_machine_profile_name->value, old_filament_profile_names->values.front());
                else
                    convert_filament_preset_name(old_machine_profile_name->value, inherits);
                loaded = this->filaments.load_external_preset(name_or_path, name, old_filament_profile_names->values.front(), config, filament_different_keys_set, PresetCollection::LoadAndSelect::Always, file_version, filament_id).first;
            }
            else {
                // called from Config Wizard.
				loaded= &this->filaments.load_preset(this->filaments.path_from_name(name, inherits.empty()), name, config, true, file_version);
				loaded->save(nullptr);
			}
            this->filament_presets.clear();
			this->filament_presets.emplace_back(loaded->name);
        } else {
            assert(is_external);
            // Split the filament presets, load each of them separately.
            std::vector<DynamicPrintConfig> configs(num_filaments, this->filaments.default_preset().config);
            // loop through options and scatter them into configs.
            for (const t_config_option_key &key : this->filaments.default_preset().config.keys()) {
                ConfigOption *other_opt = config.option(key);
                if (other_opt == nullptr)
                    continue;
                if (other_opt->is_scalar()) {
                    for (size_t i = 0; i < configs.size(); ++ i)
                        configs[i].option(key, false)->set(other_opt);
                }
                else if (key != "compatible_printers" && key != "compatible_prints") {
                    for (size_t i = 0; i < configs.size(); ++i) {
                        if (process_multi_extruder && (filament_options_with_variant.find(key) != filament_options_with_variant.end())) {
                            ConfigOptionVectorBase* other_opt_vec = static_cast<ConfigOptionVectorBase*>(other_opt);
                            if (other_opt_vec->size() != extruder_variant_count) {
                                other_opt_vec->resize(extruder_variant_count);
                            }
                            size_t next_index = (i < (configs.size() - 1)) ? filament_variant_index[i + 1] : extruder_variant_count;
                            static_cast<ConfigOptionVectorBase*>(configs[i].option(key, false))->set(other_opt, filament_variant_index[i], next_index - filament_variant_index[i]);
                        }
                        else
                            static_cast<ConfigOptionVectorBase*>(configs[i].option(key, false))->set_at(other_opt, 0, i);
                    }
                }
            }
            // Load the configs into this->filaments and make them active.
            this->filament_presets = std::vector<std::string>(configs.size());
            // To avoid incorrect selection of the first filament preset (means a value of Preset->m_idx_selected)
            // in a case when next added preset take a place of previosly selected preset,
            // we should add presets from last to first
            bool any_modified = false;
            //BBS: add config related logs
            BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": load multiple filament preset from filament_settings_id");
            for (int i = (int)configs.size()-1; i >= 0; i--) {
                DynamicPrintConfig &cfg = configs[i];
                // Split the "compatible_printers_condition" and "inherits" from the cummulative vectors to separate filament presets.
                cfg.opt_string("compatible_printers_condition", true) = compatible_printers_condition_values[i + 1];
                cfg.opt_string("compatible_prints_condition",   true) = compatible_prints_condition_values[i];
                cfg.opt_string("inherits", true)                      = inherits_values[i + 1];

                //BBS: add different settings logic
                std::vector<std::string> filament_different_keys_vector;
                std::string filament_different_settings = different_values[i+1];
                Slic3r::unescape_strings_cstyle(filament_different_settings, filament_different_keys_vector);
                std::set<std::string> filament_different_keys_set(filament_different_keys_vector.begin(), filament_different_keys_vector.end());
                //if (!has_different_settings_to_system) {
                //    filament_different_keys_set.clear();
                //}
                //else
                    filament_different_keys_set.insert(ignore_settings_list.begin(), ignore_settings_list.end());

                std::string filament_id = filament_ids[i];

                // Load all filament presets, but only select the first one in the preset dialog.
                std::string& filament_inherit = cfg.opt_string("inherits", true);
                if (filament_inherit.empty() && (i < int(old_filament_profile_names->values.size())))
                    convert_filament_preset_name(old_machine_profile_name->value, old_filament_profile_names->values[i]);
                else
                    convert_filament_preset_name(old_machine_profile_name->value, filament_inherit);
                auto [loaded, modified] = this->filaments.load_external_preset(name_or_path, name,
                    (i < int(old_filament_profile_names->values.size())) ? old_filament_profile_names->values[i] : "",
                    std::move(cfg),
                    filament_different_keys_set,
                    i == 0 ?
                        PresetCollection::LoadAndSelect::Always :
                    any_modified ?
                        PresetCollection::LoadAndSelect::Never :
                        PresetCollection::LoadAndSelect::OnlyIfModified,
                    file_version,
                    filament_id);
                any_modified |= modified;
                this->filament_presets[i] = loaded->name;
            }
        }

        // 4) Load the project config values (the per extruder wipe matrix etc).
        this->project_config.apply_only(config, s_project_options);

        break;
    }
    case ptSLA:
    {
        /*std::set<std::string> different_keys_set;
        load_preset(this->sla_prints, 0, "sla_print_settings_id", different_keys_set);
        load_preset(this->sla_materials, 1, "sla_material_settings_id", different_keys_set);
        load_preset(this->printers, 2, "printer_settings_id", different_keys_set);*/
        break;
    }
    default:
        break;
    }

	this->update_compatible(PresetSelectCompatibleType::Never);
    this->update_multi_material_filament_presets();

    //BBS
    //const std::string &physical_printer = config.option<ConfigOptionString>("physical_printer_settings_id", true)->value;
    const std::string physical_printer;
    if (this->printers.get_edited_preset().is_external || physical_printer.empty()) {
        this->physical_printers.unselect_printer();
    } else {
        // Activate the physical printer profile if possible.
        PhysicalPrinter *pp = this->physical_printers.find_printer(physical_printer, true);
        if (pp != nullptr && std::find(pp->preset_names.begin(), pp->preset_names.end(), this->printers.get_edited_preset().name) != pp->preset_names.end())
            this->physical_printers.select_printer(pp->name, this->printers.get_edited_preset().name);
        else
            this->physical_printers.unselect_printer();
    }
    //BBS: add config related logs
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": finished");
}

//BBS: Load a config bundle file from json
std::pair<PresetsConfigSubstitutions, size_t> PresetBundle::load_vendor_configs_from_json(
    const std::string &path, const std::string &vendor_name, LoadConfigBundleAttributes flags, ForwardCompatibilitySubstitutionRule compatibility_rule, const PresetBundle* base_bundle)
{
    // Enable substitutions for user config bundle, throw an exception when loading a system profile.
    ConfigSubstitutionContext  substitution_context { compatibility_rule };
    PresetsConfigSubstitutions substitutions;

    //BBS: add config related logs
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" enter, path %1%, compatibility_rule %2%")%path.c_str()%compatibility_rule;
    if (flags.has(LoadConfigBundleAttribute::ResetUserProfile) || flags.has(LoadConfigBundleAttribute::LoadSystem))
        // Reset this bundle, delete user profile files if SaveImported.
        this->reset(flags.has(LoadConfigBundleAttribute::SaveImported));

    // 1) load the vroot json and construct the vendor profile
    VendorProfile vendor_profile(vendor_name);
    std::string root_file = path + "/" + vendor_name + ".json";
    std::vector<std::pair<std::string, std::string>> machine_model_subfiles;
    std::vector<std::pair<std::string, std::string>> process_subfiles;
    std::vector<std::pair<std::string, std::string>> filament_subfiles;
    std::vector<std::pair<std::string, std::string>> machine_subfiles;
    auto get_name_and_subpath = [this](json::iterator& it, std::vector<std::pair<std::string, std::string>>& subfile_map) {
        if (it.value().is_array()) {
            for (auto iter1 = it.value().begin(); iter1 != it.value().end(); iter1++) {
                if (iter1.value().is_object()) {
                    std::string name, subpath;
                    for (auto iter2 = iter1.value().begin(); iter2 != iter1.value().end(); iter2++) {
                        if (iter2.value().is_string()) {
                            if (boost::iequals(iter2.key(), BBL_JSON_KEY_NAME)) {
                                name = iter2.value();
                            } else if (boost::iequals(iter2.key(), BBL_JSON_KEY_SUB_PATH)) {
                                subpath = iter2.value();
                            }
                        }
                        else {
                            ++m_errors;
                            BOOST_LOG_TRIVIAL(error) << __FUNCTION__<< ": invalid value type for " << iter2.key();
                        }
                    }
                    if (!name.empty() && !subpath.empty())
                        subfile_map.push_back(std::make_pair(name, subpath));
                } else {
                    ++m_errors;
                    BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": invalid type for " << iter1.key();
                }
            }
        } else {
            ++m_errors;
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": invalid type for " << it.key();
        }
    };
    try {
        boost::nowide::ifstream ifs(root_file);
        json j;
        ifs >> j;
        //parse the json elements
        for (auto it = j.begin(); it != j.end(); it++) {
            if (boost::iequals(it.key(), BBL_JSON_KEY_VERSION)) {
                //get version
                std::string version_str = it.value();
                auto config_version = Semver::parse(version_str);
                if (! config_version) {
                    throw ConfigurationError((boost::format("vendor %1%'s config version: %2% invalid\nSuggest cleaning the directory %3% firstly")
                        % vendor_name % version_str % path).str());
                } else {
                    vendor_profile.config_version = std::move(*config_version);
                }
            }
            else if (boost::iequals(it.key(), BBL_JSON_KEY_URL)) {
                //get url
                vendor_profile.config_update_url = it.value();
            }
            else if (boost::iequals(it.key(), BBL_JSON_KEY_DESCRIPTION)) {
                //get description
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< ": parse "<<root_file<<", got description:  " << it.value();
            }
            else if (boost::iequals(it.key(), BBL_JSON_KEY_NAME)) {
                //get name
                vendor_profile.name = it.value();
            }
            else if (boost::iequals(it.key(), BBL_JSON_KEY_MACHINE_MODEL_LIST)) {
                //get machine model list
                get_name_and_subpath(it, machine_model_subfiles);
            }
            else if (boost::iequals(it.key(), BBL_JSON_KEY_PROCESS_LIST)) {
                //get process list
                get_name_and_subpath(it, process_subfiles);
            }
            else if (boost::iequals(it.key(), BBL_JSON_KEY_FILAMENT_LIST)) {
                //get filament list
                get_name_and_subpath(it, filament_subfiles);
            }
            else if (boost::iequals(it.key(), BBL_JSON_KEY_MACHINE_LIST)) {
                //get machine list
                get_name_and_subpath(it, machine_subfiles);
            }
        }
    }
    catch(nlohmann::detail::parse_error &err) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__<< ": parse "<<root_file<<" got a nlohmann::detail::parse_error, reason = " << err.what();
        throw ConfigurationError((boost::format("Failed loading configuration file %1%: %2%\nSuggest cleaning the directory %3% firstly")
                %root_file %err.what() % path).str());
        //goto __error_process;
    }

    if (flags.has(LoadConfigBundleAttribute::LoadFilamentOnly)) {
        machine_model_subfiles.clear();
        machine_subfiles.clear();
        process_subfiles.clear();
    }

    //2) paste the machine model
    for (auto& machine_model : machine_model_subfiles)
    {
        std::string subfile = path + "/" + vendor_name + "/" + machine_model.second;
        VendorProfile::PrinterModel model;
        model.id = machine_model.first;
        try {
            boost::nowide::ifstream ifs(subfile);
            json j;
            ifs >> j;
            //parse the json elements
            for (auto it = j.begin(); it != j.end(); it++) {
                if (boost::iequals(it.key(), BBL_JSON_KEY_VERSION)) {
                    //get version
                }
                else if (boost::iequals(it.key(), BBL_JSON_KEY_URL)) {
                    //get url
                }
                else if (boost::iequals(it.key(), BBL_JSON_KEY_NAME)) {
                    //get name
                    model.name = it.value();
                }
                else if (boost::iequals(it.key(), BBL_JSON_KEY_MODEL_ID)) {
                    //get model_id
                    model.model_id = it.value();
                }
                else if (boost::iequals(it.key(), BBL_JSON_KEY_NOZZLE_DIAMETER)) {
                    //get nozzle diameter
                    std::string nozzle_diameters = it.value();
                    std::vector<std::string> variants;
                    if (Slic3r::unescape_strings_cstyle(nozzle_diameters, variants)) {
                        for (const std::string &variant_name : variants) {
                            if (model.variant(variant_name) == nullptr)
                                model.variants.emplace_back(VendorProfile::PrinterVariant(variant_name));
                        }
                    } else {
                        ++m_errors;
                        BOOST_LOG_TRIVIAL(error)<< __FUNCTION__ << boost::format(": invalid nozzle_diameters %1% for Vendor %1%") % nozzle_diameters % vendor_name;
                    }
                }
                else if (boost::iequals(it.key(), BBL_JSON_KEY_PRINTER_TECH)) {
                    //get printer tech
                    if (boost::algorithm::starts_with(it.value(), "SL"))
                        model.technology = ptSLA;
                    else
                        model.technology = ptFFF;
                }
                else if (boost::iequals(it.key(), BBL_JSON_KEY_FAMILY)) {
                    //get family
                    model.family = it.value();
                }
                else if (boost::iequals(it.key(), BBL_JSON_KEY_BED_MODEL)) {
                    //get bed model
                    model.bed_model = it.value();
                } else if (boost::iequals(it.key(), BBL_JSON_KEY_BOTTOM_TEXTURE_END_NAME)) {
                    model.bottom_texture_end_name = it.value();
                } else if (boost::iequals(it.key(), BBL_JSON_KEY_USE_DOUBLE_EXTRUDER_DEFAULT_TEXTURE)) {
                    model.use_double_extruder_default_texture = it.value();
                } else if (boost::iequals(it.key(), BBL_JSON_KEY_BOTTOM_TEXTURE_RECT)) {
                    model.bottom_texture_rect = it.value();
                } else if (boost::iequals(it.key(), BBL_JSON_KEY_MIDDLE_TEXTURE_RECT)) {
                    model.middle_texture_rect = it.value();
                }
                else if (boost::iequals(it.key(), BBL_JSON_KEY_IMAGE_BED_TYPE)) {
                    model.image_bed_type = it.value();
                }
                else if (boost::iequals(it.key(), BBL_JSON_KEY_BED_TEXTURE)) {
                    //get bed texture
                    model.bed_texture = it.value();
                }
                else if (boost::iequals(it.key(), BBL_JSON_KEY_HOTEND_MODEL)) {
                    model.hotend_model = it.value();
                }
                else if (boost::iequals(it.key(), BBL_JSON_KEY_DEFAULT_MATERIALS)) {
                    //get machine list
                    std::string default_materials_field = it.value();
                    if (Slic3r::unescape_strings_cstyle(default_materials_field, model.default_materials)) {
                    	Slic3r::sort_remove_duplicates(model.default_materials);
                        if (! model.default_materials.empty() && model.default_materials.front().empty())
                            // An empty material was inserted into the list of default materials. Remove it.
                            model.default_materials.erase(model.default_materials.begin());
                    } else {
                        ++m_errors;
                        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": invalid default_materials %1% for Vendor %1%") % default_materials_field % vendor_name;
                    }
                } else if (boost::iequals(it.key(), BBL_JSON_KEY_NOT_SUPPORT_BED_TYPE)) {
                    // get machine list
                    std::string not_support_bed_type_field = it.value();
                    if (Slic3r::unescape_strings_cstyle(not_support_bed_type_field, model.not_support_bed_types)) {
                        Slic3r::sort_remove_duplicates(model.not_support_bed_types);
                        if (!model.not_support_bed_types.empty() && model.not_support_bed_types.front().empty())
                            // An empty material was inserted into the list of default materials. Remove it.
                            model.not_support_bed_types.erase(model.not_support_bed_types.begin());
                    } else {
                        BOOST_LOG_TRIVIAL(error) << __FUNCTION__
                                                 << boost::format(": invalid not_support_bed_types %1% for Vendor %1%") % not_support_bed_type_field % vendor_name;
                    }
                }
            }
        }
        catch(nlohmann::detail::parse_error &err) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__<< ": parse "<< subfile <<" got a nlohmann::detail::parse_error, reason = " << err.what();
            throw ConfigurationError((boost::format("Failed loading configuration file %1%: %2%\nSuggest cleaning the directory %3% firstly")
                %subfile %err.what() % path).str());
        }

        if (! model.id.empty() && ! model.variants.empty())
            vendor_profile.models.push_back(std::move(model));
    }

    //insert the vendor profile
    this->vendors.emplace(vendor_name, vendor_profile);
    const VendorProfile* current_vendor_profile = &this->vendors[vendor_name];

    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(", loaded vendor profile, name %1%, id %2%, version %3%")%vendor_profile.name%vendor_profile.id%vendor_profile.config_version.to_string();

    if (flags.has(LoadConfigBundleAttribute::LoadVendorOnly))
        return std::make_pair(PresetsConfigSubstitutions{}, 0);

    // 3) paste the process/filament/print configs
    PresetCollection         *presets = nullptr;
    size_t                   presets_loaded = 0;

    auto parse_subfile = [this, path, vendor_name, presets_loaded, current_vendor_profile, base_bundle](
        ConfigSubstitutionContext& substitution_context,
        PresetsConfigSubstitutions& substitutions,
        LoadConfigBundleAttributes& flags,
        std::pair<std::string, std::string>& subfile_iter,
        std::map<std::string, DynamicPrintConfig>& config_maps,
        std::map<std::string, std::string>& filament_id_maps,
        PresetCollection* presets_collection,
        size_t& count, bool is_from_lib = false) -> std::string {

        std::string subfile = path + "/" + vendor_name + "/" + subfile_iter.second;
        // Load the print, filament or printer preset.
        std::string               preset_name;
        DynamicPrintConfig        config;
        std::string 			  alias_name, inherits, description, instantiation, setting_id, filament_id;
        std::vector<std::string>  renamed_from;
        const DynamicPrintConfig* default_config = nullptr;
        std::string               reason;
        try {
            std::map<std::string, std::string> key_values;
            substitution_context.substitutions.clear();

            //parse the json elements
            DynamicPrintConfig config_src;
            std::string _renamed_from_str;
            config_src.load_from_json(subfile, substitution_context, false, key_values, reason);
            if (!reason.empty()) {
                ++m_errors;
                BOOST_LOG_TRIVIAL(error) << __FUNCTION__<< ": load config file "<<subfile<<" Failed!";
                return reason;
            }
            preset_name = key_values[BBL_JSON_KEY_NAME];
            description     = key_values[BBL_JSON_KEY_DESCRIPTION];
            if(key_values.find(BBL_JSON_KEY_INSTANTIATION) == key_values.end())
            {
                BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": Missing instantiation attribute for " << preset_name;
                ++m_errors;
            }
            instantiation   = key_values[BBL_JSON_KEY_INSTANTIATION];
            if(instantiation != "false" && instantiation != "true"){
                BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": Missing instantiation attribute for " << preset_name;
                ++m_errors;
            }
            auto setting_it = key_values.find(BBL_JSON_KEY_SETTING_ID);
            if (setting_it != key_values.end())
                setting_id = setting_it->second;
            auto filament_it = key_values.find(BBL_JSON_KEY_FILAMENT_ID);
            if (filament_it != key_values.end())
                filament_id = filament_it->second;
            //check whether it inherits other preset or not
            auto it1 = key_values.find(BBL_JSON_KEY_INHERITS);
            if (it1 != key_values.end()) {
                inherits = it1->second;
                auto it2 = config_maps.find(inherits);
                default_config = nullptr;
                if (it2 != config_maps.end())
                    default_config = &(it2->second);
                if(default_config == nullptr && base_bundle != nullptr) {
                    auto base_it2 = base_bundle->m_config_maps.find(inherits);
                    if (base_it2 != base_bundle->m_config_maps.end())
                        default_config = &(base_it2->second);
                }
                if (default_config != nullptr) {
                    if (filament_id.empty() && (presets_collection->type() == Preset::TYPE_FILAMENT)) {
                        auto filament_id_map_iter = filament_id_maps.find(inherits);
                        if (filament_id_map_iter != filament_id_maps.end()) {
                            filament_id = filament_id_map_iter->second;
                        }
                        if (filament_id.empty() && base_bundle != nullptr) {
                            auto filament_id_map_iter = base_bundle->m_filament_id_maps.find(inherits);
                            if (filament_id_map_iter != base_bundle->m_filament_id_maps.end()) {
                                filament_id = filament_id_map_iter->second;
                            }
                        }
                    }
                }
                else {
                    ++m_errors;
                    BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": can not find inherits " << inherits << " for " << preset_name;
                    // throw ConfigurationError(format("can not find inherits %1% for %2%", inherits, preset_name));
                    reason = "Can not find inherits: " + inherits;
                    return reason;
                }
            }
            else {
                if (presets_collection->type() == Preset::TYPE_PRINTER)
                    default_config = &presets_collection->default_preset_for(config_src).config;
                else
                    default_config = &presets_collection->default_preset().config;
            }
            config = *default_config;
            config.apply(config_src);
            extend_default_config_length(config, true, *default_config);
            if (instantiation == "false" && "Template" != vendor_name) {
                // Report configuration fields, which are misplaced into a wrong group.
                std::string incorrect_keys = Preset::remove_invalid_keys(config, *default_config);
                if (!incorrect_keys.empty()) {
                    ++m_errors;
                    BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": The config " << subfile << " contains incorrect keys: " << incorrect_keys
                                             << ", which were removed";
                }

                config_maps.emplace(preset_name, std::move(config));
                if ((presets_collection->type() == Preset::TYPE_FILAMENT) && (!filament_id.empty()))
                    filament_id_maps.emplace(preset_name, filament_id);
                return reason;
            }
            if (config.has("alias"))
                alias_name = (dynamic_cast<const ConfigOptionString *>(config.option("alias")))->value;

            if (key_values.find(ORCA_JSON_KEY_RENAMED_FROM) != key_values.end()) {
                if (!unescape_strings_cstyle(key_values[ORCA_JSON_KEY_RENAMED_FROM], renamed_from)) {
                    BOOST_LOG_TRIVIAL(error) << "Error in a Config \"" << path << "\": The preset \"" << preset_name
                                             << "\" contains invalid \"renamed_from\" key, which is being ignored.";
                }
            }
            Preset::normalize(config);
        }
        catch(nlohmann::detail::parse_error &err) {
            ++m_errors;
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__<< ": parse "<< subfile <<" got a nlohmann::detail::parse_error, reason = " << err.what();
            reason = std::string("json parse error") + err.what();
            return reason;
        }

        // Report configuration fields, which are misplaced into a wrong group.
        std::string incorrect_keys = Preset::remove_invalid_keys(config, *default_config);
        if (!incorrect_keys.empty()) {
            ++m_errors;
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": The config " << subfile << " contains incorrect keys: " << incorrect_keys
                                     << ", which were removed";
        }

        if (presets_collection->type() == Preset::TYPE_PRINTER) {
            // Filter out printer presets, which are not mentioned in the vendor profile.
            // These presets are considered not installed.
            auto printer_model   = config.opt_string("printer_model");
            if (printer_model.empty()) {
                ++m_errors;
                BOOST_LOG_TRIVIAL(error) << "Error in a Vendor Config Bundle \"" << path << "\": The printer preset \"" <<
                    preset_name << "\" defines no printer model, it will be ignored.";
                reason = std::string("can not find printer_model");
                return reason;
            }
            auto printer_variant = config.opt_string("printer_variant");
            if (printer_variant.empty()) {
                ++m_errors;
                BOOST_LOG_TRIVIAL(error) << "Error in a Vendor Config Bundle \"" << path << "\": The printer preset \"" <<
                    preset_name << "\" defines no printer variant, it will be ignored.";
                reason = std::string("can not find printer_variant");
                return reason;
            }
            auto it_model = std::find_if(current_vendor_profile->models.cbegin(), current_vendor_profile->models.cend(),
                [&](const VendorProfile::PrinterModel &m) { return m.id == printer_model; }
            );
            if (it_model == current_vendor_profile->models.end()) {
                ++m_errors;
                BOOST_LOG_TRIVIAL(error) << "Error in a Vendor Config Bundle \"" << path << "\": The printer preset \"" <<
                    preset_name << "\" defines invalid printer model \"" << printer_model << "\", it will be ignored.";
                reason = std::string("can not find printer model in vendor profile");
                return reason;
            }
            auto it_variant = it_model->variant(printer_variant);
            if (it_variant == nullptr) {
                ++m_errors;
                BOOST_LOG_TRIVIAL(error) << "Error in a Vendor Config Bundle \"" << path << "\": The printer preset \"" <<
                    preset_name << "\" defines invalid printer variant \"" << printer_variant << "\", it will be ignored.";
                reason = std::string("can not find printer_variant in vendor profile");
                return reason;
            }
        }
        const Preset *preset_existing = presets_collection->find_preset(preset_name, false);
        if (preset_existing != nullptr) {
            ++m_errors;
            BOOST_LOG_TRIVIAL(error) << "Error in a Vendor Config Bundle \"" << path << "\": The printer preset \"" <<
                preset_name << "\" has already been loaded from another Config Bundle.";
            reason = std::string("duplicated defines");
            return reason;
        }

        auto file_path = (boost::filesystem::path(data_dir())  /PRESET_SYSTEM_DIR/ vendor_name / subfile_iter.second).make_preferred();
        if(validation_mode)
            file_path = (boost::filesystem::path(data_dir()) / vendor_name / subfile_iter.second).make_preferred();

        // Load the preset into the list of presets, save it to disk.
        Preset &loaded = presets_collection->load_preset(file_path.string(), preset_name, std::move(config), false);
        if (flags.has(LoadConfigBundleAttribute::LoadSystem)) {
            loaded.is_system = true;
            loaded.vendor = current_vendor_profile;
            loaded.version = current_vendor_profile->config_version;
            loaded.description = description;
            loaded.setting_id = setting_id;
            loaded.filament_id = filament_id;
            loaded.m_from_orca_filament_lib = is_from_lib;
            BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << " " << __LINE__ << ", " << loaded.name << " load filament_id: " << filament_id;
            if (presets_collection->type() == Preset::TYPE_FILAMENT) {
                if (filament_id.empty() && "Template" != vendor_name) {
                    ++m_errors;
                    BOOST_LOG_TRIVIAL(error) << __FUNCTION__<< ": can not find filament_id for " << preset_name;
                    //throw ConfigurationError(format("can not find inherits %1% for %2%", inherits, preset_name));
                    reason = "Can not find filament_id for " + preset_name;
                    return reason;
                }
                else {
                    filament_id_maps.emplace(preset_name, filament_id);
                }
            }
        }

        // Derive the profile logical name aka alias from the preset name if the alias was not stated explicitely.
        if (alias_name.empty()) {
            size_t end_pos = preset_name.find_first_of("@");
            if (end_pos != std::string::npos) {
                alias_name = preset_name.substr(0, end_pos);
                if (renamed_from.empty())
                    // Add the preset name with the '@' character removed into the "renamed_from" list.
                    renamed_from.emplace_back(alias_name + preset_name.substr(end_pos + 1));
                boost::trim_right(alias_name);
            }
        }
        if (alias_name.empty())
            loaded.alias = preset_name;
        else {
            loaded.alias = std::move(alias_name);
            filaments.set_printer_hold_alias(loaded.alias, loaded);
        }
        loaded.renamed_from = std::move(renamed_from);
        if (! substitution_context.empty())
            substitutions.push_back({
                preset_name, presets_collection->type(), PresetConfigSubstitutions::Source::ConfigBundle,
                std::string(), std::move(substitution_context.substitutions) });
        config_maps.emplace(preset_name, loaded.config);
        ++count;
        //BBS: add config related logs
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(", got preset %1%, from %2%")%loaded.name %subfile;
        return reason;
    };

    std::map<std::string, DynamicPrintConfig> configs;
    std::map<std::string, std::string> filament_id_maps;
    //3.1) paste the process
    presets = &this->prints;
    configs.clear();
    filament_id_maps.clear();
    for (auto& subfile : process_subfiles)
    {
        std::string reason = parse_subfile(substitution_context, substitutions, flags, subfile, configs, filament_id_maps, presets, presets_loaded);
        if (!reason.empty()) {
            ++m_errors;
            //parse error
            std::string subfile_path = path + "/" + vendor_name + "/" + subfile.second;
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(", got error when parse process setting from %1%") % subfile_path;
            throw ConfigurationError((boost::format("Failed loading configuration file %1%\nSuggest cleaning the directory %2% firstly") % subfile_path % path).str());
        }
    }

    //3.2) paste the filaments
    presets = &this->filaments;
    configs.clear();
    filament_id_maps.clear();
    const auto is_orca_lib = vendor_name == ORCA_FILAMENT_LIBRARY;
    for (auto& subfile : filament_subfiles)
    {
        std::string reason = parse_subfile(substitution_context, substitutions, flags, subfile, configs, filament_id_maps, presets,
                                           presets_loaded, is_orca_lib);
        if (!reason.empty()) {
            ++m_errors;
            //parse error
            std::string subfile_path = path + "/" + vendor_name + "/" + subfile.second;
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(", got error when parse filament setting from %1%") % subfile_path;
            throw ConfigurationError((boost::format("Failed loading configuration file %1%\nSuggest cleaning the directory %2% firstly") % subfile_path % path).str());
        }
    }
    if (is_orca_lib) {
        m_config_maps      = configs;
        m_filament_id_maps = filament_id_maps;
    }

    //3.3) paste the printers
    presets = &this->printers;
    configs.clear();
    filament_id_maps.clear();
    for (auto& subfile : machine_subfiles)
    {
        std::string reason = parse_subfile(substitution_context, substitutions, flags, subfile, configs, filament_id_maps, presets, presets_loaded);
        if (!reason.empty()) {
            ++m_errors;
            //parse error
            std::string subfile_path = path + "/" + vendor_name + "/" + subfile.second;
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(", got error when parse printer setting from %1%") % subfile_path;
            throw ConfigurationError((boost::format("Failed loading configuration file %1%\nSuggest cleaning the directory %2% firstly") % subfile_path % path).str());
        }
    }

    //BBS: add config related logs
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(", finished, presets_loaded %1%")%presets_loaded;
    return std::make_pair(std::move(substitutions), presets_loaded);
}

void PresetBundle::on_extruders_count_changed(int extruders_count)
{
    printers.get_edited_preset().set_num_extruders(extruders_count);
    update_multi_material_filament_presets();
    reset_default_nozzle_volume_type();
    extruder_ams_counts.resize(extruders_count);
}

void PresetBundle::update_multi_material_filament_presets(size_t to_delete_filament_id)
{
    if (printers.get_edited_preset().printer_technology() != ptFFF)
        return;

    // BBS
#if 0
    // Verify and select the filament presets.
    auto   *nozzle_diameter = static_cast<const ConfigOptionFloats*>(printers.get_edited_preset().config.option("nozzle_diameter"));
    size_t  num_extruders   = nozzle_diameter->values.size();
    // Verify validity of the current filament presets.
    for (size_t i = 0; i < std::min(this->filament_presets.size(), num_extruders); ++ i)
        this->filament_presets[i] = this->filaments.find_preset(this->filament_presets[i], true)->name;
    // Append the rest of filament presets.
    this->filament_presets.resize(num_extruders, this->filament_presets.empty() ? this->filaments.first_visible().name : this->filament_presets.back());
#else
    size_t num_filaments = this->filament_presets.size();
#endif
    if (to_delete_filament_id == -1)
        to_delete_filament_id = num_filaments;

    // Now verify if flush_volumes_matrix has proper size (it is used to deduce number of extruders in wipe tower generator):
    std::vector<double> old_matrix = this->project_config.option<ConfigOptionFloats>("flush_volumes_matrix")->values;
    size_t old_nozzle_nums = this->project_config.option<ConfigOptionFloats>("flush_multiplier")->values.size();
    size_t old_number_of_filaments = size_t(sqrt(old_matrix.size() / old_nozzle_nums) + EPSILON);
    size_t nozzle_nums = get_printer_extruder_count();
    if (old_nozzle_nums != nozzle_nums) {
        std::vector<double>& f_multiplier = this->project_config.option<ConfigOptionFloats>("flush_multiplier")->values;
        f_multiplier.resize(nozzle_nums, 1.f);
    }

    if ( (num_filaments * num_filaments) != size_t(old_matrix.size() / old_nozzle_nums) ) {
        // First verify if purging volumes presets for each extruder matches number of extruders
        std::vector<double>& filaments = this->project_config.option<ConfigOptionFloats>("flush_volumes_vector")->values;
        while (filaments.size() < 2* num_filaments) {
            filaments.push_back(filaments.size()>1 ? filaments[0] : 140.);  // copy the values from the first extruder
            filaments.push_back(filaments.size()>1 ? filaments[1] : 140.);
        }
        while (filaments.size() > 2* num_filaments) {
            filaments.pop_back();
            filaments.pop_back();
        }

        size_t old_matrix_size = old_number_of_filaments * old_number_of_filaments;
        size_t new_matrix_size = num_filaments * num_filaments;
        std::vector<double> new_matrix(new_matrix_size * nozzle_nums, 0);
        for (unsigned int i = 0; i < num_filaments; ++i)
            for (unsigned int j = 0; j < num_filaments; ++j) {
                if (i < old_number_of_filaments && j < old_number_of_filaments) {
                    unsigned int old_i = i >= to_delete_filament_id ? i + 1 : i;
                    unsigned int old_j = j >= to_delete_filament_id ? j + 1 : j;
                    for (size_t nozzle_id = 0; nozzle_id < nozzle_nums; ++nozzle_id) {
                        new_matrix[i * num_filaments + j + new_matrix_size * nozzle_id] = old_matrix[old_i * old_number_of_filaments + old_j + old_matrix_size * nozzle_id];
                    }
                } else {
                    for (size_t nozzle_id = 0; nozzle_id < nozzle_nums; ++nozzle_id) {
                        new_matrix[i * num_filaments + j + new_matrix_size * nozzle_id] = (i == j ? 0. : filaments[2 * i] + filaments[2 * j + 1]);
                    }
                }
            }
        this->project_config.option<ConfigOptionFloats>("flush_volumes_matrix")->values = new_matrix;
    }
}

void PresetBundle::update_compatible(PresetSelectCompatibleType select_other_print_if_incompatible, PresetSelectCompatibleType select_other_filament_if_incompatible)
{
    const Preset					&printer_preset					    = this->printers.get_edited_preset();
	const PresetWithVendorProfile    printer_preset_with_vendor_profile = this->printers.get_preset_with_vendor_profile(printer_preset);

    class PreferedProfileMatch
    {
    public:
        PreferedProfileMatch(const std::string &prefered_alias, const std::string &prefered_name) :
            m_prefered_alias(prefered_alias), m_prefered_name(prefered_name) {}

        int operator()(const Preset &preset) const
        {
            return
                preset.is_default || preset.is_external ?
                    // Don't match any properties of the "-- default --" profile or the external profiles when switching printer profile.
                    0 :
                    ! m_prefered_alias.empty() && m_prefered_alias == preset.alias ?
                        // Matching an alias, always take this preset with priority.
                        std::numeric_limits<int>::max() :
                        // Otherwise take the prefered profile, or the first compatible.
                        preset.name == m_prefered_name;
        }

    private:
        const std::string  m_prefered_alias;
        const std::string &m_prefered_name;
    };

    // Matching by the layer height in addition.
    class PreferedPrintProfileMatch : public PreferedProfileMatch
    {
    public:
        PreferedPrintProfileMatch(const Preset *preset, const std::string &prefered_name) :
            PreferedProfileMatch(preset ? preset->alias : std::string(), prefered_name), m_prefered_layer_height(preset ? preset->config.opt_float("layer_height") : 0) {}

        int operator()(const Preset &preset) const
        {
            // Don't match any properties of the "-- default --" profile or the external profiles when switching printer profile.
            if (preset.is_default || preset.is_external)
                return 0;
            int match_quality = PreferedProfileMatch::operator()(preset);
            if (match_quality < std::numeric_limits<int>::max()) {
                match_quality += 1;
                if (preset.is_visible)
                    match_quality += 1;
                if (m_prefered_layer_height > 0. && std::abs(preset.config.opt_float("layer_height") - m_prefered_layer_height) < 0.0005)
                    match_quality *= 10;
            }
            return match_quality;
        }

    private:
        const double m_prefered_layer_height;
    };

    // Matching by the layer height in addition.
    class PreferedFilamentProfileMatch : public PreferedProfileMatch
    {
    public:
        PreferedFilamentProfileMatch(const Preset *preset, const std::string &prefered_name) :
            PreferedProfileMatch(preset ? preset->alias : std::string(), prefered_name),
            m_prefered_filament_type(preset ? preset->config.opt_string("filament_type", 0) : std::string()) {}

        int operator()(const Preset &preset) const
        {
            // Don't match any properties of the "-- default --" profile or the external profiles when switching printer profile.
            if (preset.is_default || preset.is_external)
                return 0;
            int match_quality = PreferedProfileMatch::operator()(preset);
            if (match_quality < std::numeric_limits<int>::max()) {
                match_quality += 1;
                if(preset.is_visible)
                    match_quality += 1;
                if (! m_prefered_filament_type.empty() && m_prefered_filament_type == preset.config.opt_string("filament_type", 0))
                    match_quality *= 10;
            }
            return match_quality;
        }

    private:
        const std::string m_prefered_filament_type;
    };

    // Matching by the layer height in addition.
    class PreferedFilamentsProfileMatch
    {
    public:
        PreferedFilamentsProfileMatch(const Preset *preset, const std::vector<std::string> &prefered_names) :
            m_prefered_alias(preset ? preset->alias : std::string()),
            m_prefered_filament_type(preset ? preset->config.opt_string("filament_type", 0) : std::string("PLA")), // BBS: default choose PLA
            m_prefered_names(prefered_names)
            {}

        int operator()(const Preset &preset) const
        {
            // Don't match any properties of the "-- default --" profile or the external profiles when switching printer profile.
            if (preset.is_default || preset.is_external)
                return 0;
            if (! m_prefered_alias.empty() && m_prefered_alias == preset.alias)
                // Matching an alias, always take this preset with priority.
                return std::numeric_limits<int>::max();
            int match_quality = (std::find(m_prefered_names.begin(), m_prefered_names.end(), preset.name) != m_prefered_names.end()) + 1;
            if (! m_prefered_filament_type.empty() && m_prefered_filament_type == preset.config.opt_string("filament_type", 0))
                match_quality *= 10;
            return match_quality;
        }

    private:
        const std::string               m_prefered_alias;
        const std::string               m_prefered_filament_type;
        const std::vector<std::string> &m_prefered_names;
    };

    BOOST_LOG_TRIVIAL(info) << boost::format("update_compatibility for all presets enter, select_other_print_if_incompatible %1%, select_other_filament_if_incompatible %2%")%(int)select_other_print_if_incompatible %(int)select_other_filament_if_incompatible;
	switch (printer_preset.printer_technology()) {
    case ptFFF:
    {
		assert(printer_preset.config.has("default_print_profile"));
		assert(printer_preset.config.has("default_filament_profile"));
        const std::vector<std::string> &prefered_filament_profiles = printer_preset.config.option<ConfigOptionStrings>("default_filament_profile")->values;
        this->prints.update_compatible(printer_preset_with_vendor_profile, nullptr, select_other_print_if_incompatible,
            PreferedPrintProfileMatch(this->prints.get_selected_idx() == size_t(-1) ? nullptr : &this->prints.get_edited_preset(), printer_preset.config.opt_string("default_print_profile")));
        const PresetWithVendorProfile   print_preset_with_vendor_profile = this->prints.get_edited_preset_with_vendor_profile();
        // Remember whether the filament profiles were compatible before updating the filament compatibility.
        std::vector<char> 				filament_preset_was_compatible(this->filament_presets.size(), false);
        for (size_t idx = 0; idx < this->filament_presets.size(); ++ idx) {
            Preset *preset = this->filaments.find_preset(this->filament_presets[idx], false);
            filament_preset_was_compatible[idx] = preset != nullptr && preset->is_compatible;
        }
        // First select a first compatible profile for the preset editor.
        BOOST_LOG_TRIVIAL(info) << boost::format("prefered filaments: size %1%, previous selected %2%") %prefered_filament_profiles.size() % this->filaments.get_selected_idx();
        if (this->filaments.get_selected_idx() != size_t(-1))
        {
            BOOST_LOG_TRIVIAL(info) << boost::format("previous selected filament： %1%") % this->filaments.get_edited_preset().name;
        }
        for (size_t idx = 0; idx < prefered_filament_profiles.size(); ++idx) {
            BOOST_LOG_TRIVIAL(info) << boost::format("prefered filament： %1%") % prefered_filament_profiles[idx];
        }
        this->filaments.update_compatible(printer_preset_with_vendor_profile, &print_preset_with_vendor_profile, select_other_filament_if_incompatible,
            PreferedFilamentsProfileMatch(this->filaments.get_selected_idx() == size_t(-1) ? nullptr : &this->filaments.get_edited_preset(), prefered_filament_profiles));
        if (select_other_filament_if_incompatible != PresetSelectCompatibleType::Never) {
            // Verify validity of the current filament presets.
            const std::string prefered_filament_profile = prefered_filament_profiles.empty() ? std::string() : prefered_filament_profiles.front();
            if (this->filament_presets.size() == 1) {
                // The compatible profile should have been already selected for the preset editor. Just use it.
            	if (select_other_filament_if_incompatible == PresetSelectCompatibleType::Always || filament_preset_was_compatible.front())
                	this->filament_presets.front() = this->filaments.get_edited_preset().name;
            } else {
                for (size_t idx = 0; idx < this->filament_presets.size(); ++ idx) {
                    std::string &filament_name = this->filament_presets[idx];
                    Preset      *preset = this->filaments.find_preset(filament_name, false);
                    if (preset == nullptr || (! preset->is_compatible && (select_other_filament_if_incompatible == PresetSelectCompatibleType::Always || filament_preset_was_compatible[idx])))
                        // Pick a compatible profile. If there are prefered_filament_profiles, use them.
                        filament_name = this->filaments.first_compatible(
                            PreferedFilamentProfileMatch(preset,
                                (idx < prefered_filament_profiles.size()) ? prefered_filament_profiles[idx] : prefered_filament_profile)).name;
                }
            }
        }
		break;
    }
    case ptSLA:
    {
		assert(printer_preset.config.has("default_sla_print_profile"));
		assert(printer_preset.config.has("default_sla_material_profile"));
		this->sla_prints.update_compatible(printer_preset_with_vendor_profile, nullptr, select_other_print_if_incompatible,
            PreferedPrintProfileMatch(this->sla_prints.get_selected_idx() == size_t(-1) ? nullptr : &this->sla_prints.get_edited_preset(), printer_preset.config.opt_string("default_sla_print_profile")));
        const PresetWithVendorProfile sla_print_preset_with_vendor_profile = this->sla_prints.get_edited_preset_with_vendor_profile();
		this->sla_materials.update_compatible(printer_preset_with_vendor_profile, &sla_print_preset_with_vendor_profile, select_other_filament_if_incompatible,
            PreferedProfileMatch(this->sla_materials.get_selected_idx() == size_t(-1) ? std::string() : this->sla_materials.get_edited_preset().alias, printer_preset.config.opt_string("default_sla_material_profile")));
		break;
	}
    default: break;
    }

    BOOST_LOG_TRIVIAL(info) << boost::format("update_compatibility for all presets exit");
}


std::vector<std::string> PresetBundle::export_current_configs(const std::string &                     path,
                                                              std::function<int(std::string const &)> override_confirm,
                                                              bool                                    include_modify,
                                                              bool                                    export_system_settings)
{
    const Preset &print_preset    = include_modify ? prints.get_edited_preset() : prints.get_selected_preset();
    const Preset &printer_preset  = include_modify ? printers.get_edited_preset() : printers.get_selected_preset();
    std::set<Preset const *> presets { &print_preset, &printer_preset };
    for (auto &f : filament_presets) {
        auto filament_preset = filaments.find_preset(f, include_modify);
        if (filament_preset) presets.insert(filament_preset);
    }

    int overwrite = 0;
    std::vector<std::string> result;
    for (auto preset : presets) {
        if ((preset->is_system  && !export_system_settings) || preset->is_default)
            continue;
        std::string file = path + "/" + preset->name + ".json";
        if (overwrite == 0) overwrite = 1;
        if (boost::filesystem::exists(file) && overwrite < 2) {
            overwrite = override_confirm(preset->name);
            if (overwrite == 0 || overwrite == 2)
                continue;
        }
        preset->config.save_to_json(file, preset->name, "", preset->version.to_string());
        result.push_back(file);
    }
    return result;
}

// Set the filament preset name. As the name could come from the UI selection box,
// an optional "(modified)" suffix will be removed from the filament name.
void PresetBundle::set_filament_preset(size_t idx, const std::string &name)
{
    if (idx >= filament_presets.size()) {
        BOOST_LOG_TRIVIAL(warning) << boost::format("Warning: set_filament_preset out of range %1% - %2%") % idx % filament_presets.size();
        return;
    }
    filament_presets[idx] = Preset::remove_suffix_modified(name);
}

void PresetBundle::set_default_suppressed(bool default_suppressed)
{
    prints.set_default_suppressed(default_suppressed);
    filaments.set_default_suppressed(default_suppressed);
    sla_prints.set_default_suppressed(default_suppressed);
    sla_materials.set_default_suppressed(default_suppressed);
    printers.set_default_suppressed(default_suppressed);
}

bool PresetBundle::has_errors() const
{
    if (m_errors != 0 || printers.m_errors != 0 || filaments.m_errors != 0 || prints.m_errors != 0)
        return true;

    bool has_errors = false;
    // Orca: check if all filament presets have compatible_printers setting
    for (auto& preset : filaments) {
        if (!preset.is_system)
            continue;
        // It's per design that the Orca Filament Library can have the empty compatible_printers.
        if(preset.vendor->name == PresetBundle::ORCA_FILAMENT_LIBRARY)
            continue;
        auto* compatible_printers = dynamic_cast<const ConfigOptionStrings*>(preset.config.option("compatible_printers"));
        if (compatible_printers == nullptr || compatible_printers->values.empty()) {
            has_errors = true;
            BOOST_LOG_TRIVIAL(error) << "Filament preset \"" << preset.file << "\" is missing compatible_printers setting";
        }
    }

    return has_errors;
}

} // namespace Slic3r
