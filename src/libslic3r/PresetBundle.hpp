#ifndef slic3r_PresetBundle_hpp_
#define slic3r_PresetBundle_hpp_

#include "Preset.hpp"
#include "AppConfig.hpp"
#include "enum_bitmask.hpp"

#include <memory>
#include <unordered_map>
#include <optional>
#include <array>
#include <boost/filesystem/path.hpp>

#define DEFAULT_USER_FOLDER_NAME "default"
#define BUNDLE_STRUCTURE_JSON_NAME "bundle_structure.json"

#define VALIDATE_PRESETS_SUCCESS                0
#define VALIDATE_PRESETS_PRINTER_NOT_FOUND      1
#define VALIDATE_PRESETS_FILAMENTS_NOT_FOUND    2
#define VALIDATE_PRESETS_MODIFIED_GCODES        3


// define an enum class of vendor type
enum class VendorType {
    Unknown = 0,
    Klipper,
    Marlin,
    Marlin_BBL,
    Klipper_Qidi
};
namespace Slic3r {

struct AMSMapInfo
{
    /*for new ams mapping*/ // from struct FilamentInfo
    std::string ams_id{""};
    std::string slot_id{""};
};
struct AMSComboInfo
{
    std::vector<std::string>              ams_filament_colors;
    std::vector<std::vector<std::string>> ams_multi_color_filment;
    std::vector<std::string>              ams_filament_presets;
    std::vector<std::string>              ams_names;
    void  clear() {
        ams_filament_colors.clear();
        ams_multi_color_filment.clear();
        ams_filament_presets.clear();
        ams_names.clear();
    }
    bool empty() {
        return ams_names.empty();
    }
};
struct MergeFilamentInfo {
    std::vector<std::vector<int>> merges;
    bool  is_empty() { return merges.empty();}
};


struct FilamentBaseInfo
{
    std::string filament_name;
    std::string filament_id;
    std::string filament_type;
    std::string vendor;
    int nozzle_temp_range_low{ 220 };
    int nozzle_temp_range_high{ 220 };
    int temperature_vitrification = INT_MAX;
    bool is_support{ false };
    bool is_system{ true };
    int  filament_printable = 3;
};

// Bundle of Print + Filament + Printer presets.
class PresetBundle
{
public:
    static DynamicPrintConfig construct_full_config(Preset                         &in_printer_preset,
                                                    Preset                         &in_print_preset,
                                                    const DynamicPrintConfig       &project_config,
                                                    std::vector<Preset>            &in_filament_presets,
                                                    bool                            apply_extruder,
                                                    std::optional<std::vector<int>> filament_maps_new);
    PresetBundle();
    PresetBundle(const PresetBundle &rhs);
    PresetBundle& operator=(const PresetBundle &rhs);

    // Remove all the presets but the "-- default --".
    // Optionally remove all the files referenced by the presets from the user profile directory.
    void            reset(bool delete_files);

    void            setup_directories();
    void            copy_files(const std::string& from);

    struct PresetPreferences {
        std::string printer_model_id;// name of a preferred printer model
        std::string printer_variant; // name of a preferred printer variant
        std::string filament;        // name of a preferred filament preset
        std::string sla_material;    // name of a preferred sla_material preset
    };

    // Load ini files of all types (print, filament, printer) from Slic3r::data_dir() / presets.
    // Load selections (current print, current filaments, current printer) from config.ini
    // select preferred presets, if any exist
    PresetsConfigSubstitutions load_presets(AppConfig &config, ForwardCompatibilitySubstitutionRule rule,
                                            const PresetPreferences& preferred_selection = PresetPreferences());

    // Load selections (current print, current filaments, current printer) from config.ini
    // This is done just once on application start up.
    //BBS: change it to public
    void     load_selections(AppConfig &config, const PresetPreferences& preferred_selection = PresetPreferences());

    // BBS Load user presets
    PresetsConfigSubstitutions load_user_presets(std::string user, ForwardCompatibilitySubstitutionRule rule);
    PresetsConfigSubstitutions load_user_presets(AppConfig &config, std::map<std::string, std::map<std::string, std::string>>& my_presets, ForwardCompatibilitySubstitutionRule rule);
    PresetsConfigSubstitutions import_presets(std::vector<std::string> &files, std::function<int(std::string const &)> override_confirm, ForwardCompatibilitySubstitutionRule rule);
    bool                       import_json_presets(PresetsConfigSubstitutions &            substitutions,
                                                   std::string &                           file,
                                                   std::function<int(std::string const &)> override_confirm,
                                                   ForwardCompatibilitySubstitutionRule    rule,
                                                   int &                                   overwrite,
                                                   std::vector<std::string> &              result);
    void save_user_presets(AppConfig& config, std::vector<std::string>& need_to_delete_list);
    void remove_users_preset(AppConfig &config, std::map<std::string, std::map<std::string, std::string>> * my_presets = nullptr);
    void update_user_presets_directory(const std::string preset_folder);
    void remove_user_presets_directory(const std::string preset_folder);
    void update_system_preset_setting_ids(std::map<std::string, std::map<std::string, std::string>>& system_presets);

    //BBS: add API to get previous machine
    int validate_presets(const std::string &file_name, DynamicPrintConfig& config, std::set<std::string>& different_gcodes);

    //BBS: add function to generate differed preset for save
    //the pointer should be freed by the caller
    Preset* get_preset_differed_for_save(Preset& preset);
    int get_differed_values_to_update(Preset& preset, std::map<std::string, std::string>& key_values);


    //BBS: get vendor's current version
    Semver get_vendor_profile_version(std::string vendor_name);

    std::optional<FilamentBaseInfo> get_filament_by_filament_id(const std::string& filament_id, const std::string& printer_name = std::string()) const;

    // Orca: get vendor type
    VendorType get_current_vendor_type();
    // Vendor related handy functions
    bool is_bbl_vendor() { return get_current_vendor_type() == VendorType::Marlin_BBL; }
    bool is_qidi_vendor() { return get_current_vendor_type() == VendorType::Klipper_Qidi; }

    // Whether using bbl network for print upload
    bool use_bbl_network();
    // Whether using bbl's device tab
    bool use_bbl_device_tab();

    bool backup_user_folder() const;

    //BBS: project embedded preset logic
    PresetsConfigSubstitutions load_project_embedded_presets(std::vector<Preset*> project_presets, ForwardCompatibilitySubstitutionRule substitution_rule);
    std::vector<Preset*> get_current_project_embedded_presets();
    void reset_project_embedded_presets();

    //BBS: find printer model
    std::string get_texture_for_printer_model(std::string model_name);
    std::string get_stl_model_for_printer_model(std::string model_name);
    std::string get_hotend_model_for_printer_model(std::string model_name);

    // Export selections (current print, current filaments, current printer) into config.ini
    void            export_selections(AppConfig &config);

    // BBS
    void            set_num_filaments(unsigned int n, std::vector<std::string> new_colors);
    void            set_num_filaments(unsigned int n, std::string new_col = "");
    void         update_num_filaments(unsigned int to_del_flament_id);

    void get_ams_cobox_infos(AMSComboInfo &combox_info);
    unsigned int sync_ams_list(std::vector<std::pair<DynamicPrintConfig *,std::string>> &unknowns, bool use_map, std::map<int, AMSMapInfo> &maps,bool enable_append, MergeFilamentInfo& merge_info);
    //BBS: check whether this is the only edited filament
    bool is_the_only_edited_filament(unsigned int filament_index);

    void reset_default_nozzle_volume_type();

    std::vector<int> get_used_tpu_filaments(const std::vector<int> &used_filaments);
    // Orca: update selected filament and print
    void           update_selections(AppConfig &config);
    void set_calibrate_printer(std::string name);

    void set_is_validation_mode(bool mode) { validation_mode = mode; }
    void set_vendor_to_validate(std::string vendor) { vendor_to_validate = vendor; }

    std::vector<std::vector<DynamicPrintConfig>> get_extruder_filament_info() const;

    std::set<std::string> get_printer_names_by_printer_type_and_nozzle(const std::string &printer_type, std::string nozzle_diameter_str, bool system_only = true);
    bool                  check_filament_temp_equation_by_printer_type_and_nozzle_for_mas_tray(const std::string &printer_type,
                                                                                               std::string &      nozzle_diameter_str,
                                                                                               std::string &      setting_id,
                                                                                               std::string &      tag_uid,
                                                                                               std::string &      nozzle_temp_min,
                                                                                               std::string &      nozzle_temp_max,
                                                                                               std::string &      preset_setting_id);
    Preset *                    get_similar_printer_preset(std::string printer_model, std::string printer_variant);

    PresetCollection            prints;
    PresetCollection            sla_prints;
    PresetCollection            filaments;
    PresetCollection            sla_materials;
	PresetCollection& 			materials(PrinterTechnology pt)       { return pt == ptFFF ? this->filaments : this->sla_materials; }
	const PresetCollection& 	materials(PrinterTechnology pt) const { return pt == ptFFF ? this->filaments : this->sla_materials; }
    PrinterPresetCollection     printers;
    PhysicalPrinterCollection   physical_printers;
    // Filament preset names for a multi-extruder or multi-material print.
    // extruders.size() should be the same as printers.get_edited_preset().config.nozzle_diameter.size()
    std::vector<std::string>    filament_presets;
    // BBS: ams
    std::map<int, DynamicPrintConfig> filament_ams_list;
    std::vector<std::vector<std::string>> ams_multi_color_filment;

    std::vector<std::map<int, int>> extruder_ams_counts;

    // Calibrate
    Preset const * calibrate_printer = nullptr;
    std::set<Preset const *> calibrate_filaments;

    // The project configuration values are kept separated from the print/filament/printer preset,
    // they are being serialized / deserialized from / to the .amf, .3mf, .config, .gcode,
    // and they are being used by slicing core.
    DynamicPrintConfig          project_config;

    // There will be an entry for each system profile loaded,
    // and the system profiles will point to the VendorProfile instances owned by PresetBundle::vendors.
    VendorMap                   vendors;

    // Orca: for OrcaFilamentLibrary
    std::map<std::string, DynamicPrintConfig> m_config_maps;
    std::map<std::string, std::string> m_filament_id_maps;

        struct ObsoletePresets
    {
        std::vector<std::string> prints;
        std::vector<std::string> sla_prints;
        std::vector<std::string> filaments;
        std::vector<std::string> sla_materials;
        std::vector<std::string> printers;
    };
    ObsoletePresets             obsolete_presets;

    bool                        has_defauls_only() const
        { return prints.has_defaults_only() && filaments.has_defaults_only() && printers.has_defaults_only(); }

    DynamicPrintConfig          full_config(bool apply_extruder = true, std::optional<std::vector<int>>filament_maps = std::nullopt) const;
    // full_config() with the some "useless" config removed.
    DynamicPrintConfig          full_config_secure(std::optional<std::vector<int>>filament_maps = std::nullopt) const;

    //BBS: add some functions for multiple extruders
    int get_printer_extruder_count() const;
    bool support_different_extruders();

    // Load user configuration and store it into the user profiles.
    // This method is called by the configuration wizard.
    void                        load_config_from_wizard(const std::string &name, DynamicPrintConfig config, Semver file_version)
        { this->load_config_file_config(name, false, std::move(config), file_version, true); }

    // Load configuration that comes from a model file containing configuration, such as 3MF et al.
    // This method is called by the Plater.
    void                        load_config_model(const std::string &name, DynamicPrintConfig config, Semver file_version = Semver())
        { this->load_config_file_config(name, true, std::move(config), file_version); }

    // Load an external config file containing the print, filament and printer presets.
    // Instead of a config file, a G-code may be loaded containing the full set of parameters.
    // In the future the configuration will likely be read from an AMF file as well.
    // If the file is loaded successfully, its print / filament / printer profiles will be activated.
    ConfigSubstitutions         load_config_file(const std::string &path, ForwardCompatibilitySubstitutionRule compatibility_rule);

    // Load a config bundle file, into presets and store the loaded presets into separate files
    // of the local configuration directory.
    // Load settings into the provided settings instance.
    // Activate the presets stored in the config bundle.
    // Returns the number of presets loaded successfully.
    enum LoadConfigBundleAttribute {
        // Save the profiles, which have been loaded.
        SaveImported,
        // Delete all old config profiles before loading.
        ResetUserProfile,
        // Load a system config bundle.
        LoadSystem,
        LoadVendorOnly,
        LoadFilamentOnly,
    };
    using LoadConfigBundleAttributes = enum_bitmask<LoadConfigBundleAttribute>;
    // Load the config bundle based on the flags.
    // Don't do any config substitutions when loading a system profile, perform and report substitutions otherwise.
    /*std::pair<PresetsConfigSubstitutions, size_t> load_configbundle(
        const std::string &path, LoadConfigBundleAttributes flags, ForwardCompatibilitySubstitutionRule compatibility_rule);*/
    //Orca: load config bundle from json, pass the base bundle to support cross vendor inheritance
    std::pair<PresetsConfigSubstitutions, size_t> load_vendor_configs_from_json(
        const std::string &path, const std::string &vendor_name, LoadConfigBundleAttributes flags, ForwardCompatibilitySubstitutionRule compatibility_rule, const PresetBundle* base_bundle = nullptr);

    // Export a config bundle file containing all the presets and the names of the active presets.
    //void                        export_configbundle(const std::string &path, bool export_system_settings = false, bool export_physical_printers = false);
    //BBS: add a function to export current configbundle as default
    //void export_current_configbundle(const std::string &path);
    //BBS: add a function to export system presets for cloud-slicer
    //void export_system_configs(const std::string &path);
    std::vector<std::string> export_current_configs(const std::string &path, std::function<int(std::string const &)> override_confirm,
        bool include_modify, bool export_system_settings = false);

    // Enable / disable the "- default -" preset.
    void                        set_default_suppressed(bool default_suppressed);

    // Set the filament preset name. As the name could come from the UI selection box,
    // an optional "(modified)" suffix will be removed from the filament name.
    void                        set_filament_preset(size_t idx, const std::string &name);

    // Read out the number of extruders from an active printer preset,
    // update size and content of filament_presets.
    void                        update_multi_material_filament_presets(size_t to_delete_filament_id = size_t(-1));

    void                        on_extruders_count_changed(int extruder_count);

    // Update the is_compatible flag of all print and filament presets depending on whether they are marked
    // as compatible with the currently selected printer (and print in case of filament presets).
    // Also updates the is_visible flag of each preset.
    // If select_other_if_incompatible is true, then the print or filament preset is switched to some compatible
    // preset if the current print or filament preset is not compatible.
    void                        update_compatible(PresetSelectCompatibleType select_other_print_if_incompatible, PresetSelectCompatibleType select_other_filament_if_incompatible);
    void                        update_compatible(PresetSelectCompatibleType select_other_if_incompatible) { this->update_compatible(select_other_if_incompatible, select_other_if_incompatible); }

    // Set the is_visible flag for printer vendors, printer models and printer variants
    // based on the user configuration.
    // If the "vendor" section is missing, enable all models and variants of the particular vendor.
    void                        load_installed_printers(const AppConfig &config);

    const std::string&          get_preset_name_by_alias(const Preset::Type& preset_type, const std::string& alias) const;

    const int                   get_required_hrc_by_filament_type(const std::string& filament_type) const;
    // Save current preset of a provided type under a new name. If the name is different from the old one,
    // Unselected option would be reverted to the beginning values
    //BBS: add project embedded preset logic
    void                        save_changes_for_preset(const std::string& new_name, Preset::Type type, const std::vector<std::string>& unselected_options, bool save_to_project = false);

    std::pair<PresetsConfigSubstitutions, std::string> load_system_models_from_json(ForwardCompatibilitySubstitutionRule compatibility_rule);
    std::pair<PresetsConfigSubstitutions, std::string> load_system_filaments_json(ForwardCompatibilitySubstitutionRule compatibility_rule);
    VendorProfile                                      get_custom_vendor_models() const;

    //orca: add 'custom' as default
    static const char *ORCA_DEFAULT_BUNDLE;
	static const char *ORCA_DEFAULT_PRINTER_MODEL;
	static const char *ORCA_DEFAULT_PRINTER_VARIANT;
	static const char *ORCA_DEFAULT_FILAMENT;
    static const char *ORCA_FILAMENT_LIBRARY;


    static std::array<Preset::Type, 3>  types_list(PrinterTechnology pt) {
        if (pt == ptFFF)
            return  { Preset::TYPE_PRINTER, Preset::TYPE_PRINT, Preset::TYPE_FILAMENT };
        return      { Preset::TYPE_PRINTER, Preset::TYPE_SLA_PRINT, Preset::TYPE_SLA_MATERIAL };
    }

    // Orca: for validation only
    bool has_errors() const;

private:
    //std::pair<PresetsConfigSubstitutions, std::string> load_system_presets(ForwardCompatibilitySubstitutionRule compatibility_rule);
    //BBS: add json related logic
    std::pair<PresetsConfigSubstitutions, std::string> load_system_presets_from_json(ForwardCompatibilitySubstitutionRule compatibility_rule);
    // Merge one vendor's presets with the other vendor's presets, report duplicates.
    std::vector<std::string>    merge_presets(PresetBundle &&other);
    // Update the multicolor information for filaments.
    void update_filament_multi_color();
    // Update renamed_from and alias maps of system profiles.
    void 						update_system_maps();

    // Set the is_visible flag for filaments and sla materials,
    // apply defaults based on enabled printers when no filaments/materials are installed.
    void                        load_installed_filaments(AppConfig &config);
    void                        load_installed_sla_materials(AppConfig &config);

    // Load print, filament & printer presets from a config. If it is an external config, then the name is extracted from the external path.
    // and the external config is just referenced, not stored into user profile directory.
    // If it is not an external config, then the config will be stored into the user profile directory.
    void                        load_config_file_config(const std::string &name_or_path, bool is_external, DynamicPrintConfig &&config, Semver file_version = Semver(), bool selected = false);
    /*ConfigSubstitutions         load_config_file_config_bundle(
        const std::string &path, const boost::property_tree::ptree &tree, ForwardCompatibilitySubstitutionRule compatibility_rule);*/

    DynamicPrintConfig          full_fff_config(bool apply_extruder, std::optional<std::vector<int>> filament_maps=std::nullopt) const;
    DynamicPrintConfig          full_sla_config() const;

    // Orca: used for validation only
    bool validation_mode = false;
    std::string vendor_to_validate = ""; 
    int m_errors = 0;

};

ENABLE_ENUM_BITMASK_OPERATORS(PresetBundle::LoadConfigBundleAttribute)

} // namespace Slic3r

#endif /* slic3r_PresetBundle_hpp_ */
