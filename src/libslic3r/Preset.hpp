#ifndef slic3r_Preset_hpp_
#define slic3r_Preset_hpp_

#include <deque>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <mutex>
#include <boost/filesystem/path.hpp>
#include <boost/property_tree/ptree_fwd.hpp>

#include "PrintConfig.hpp"
#include "Semver.hpp"
#include "ProjectTask.hpp"

//BBS: change system directories
#define PRESET_SYSTEM_DIR      "system"
#define PRESET_USER_DIR        "user"
#define PRESET_FILAMENT_NAME    "filament"
#define PRESET_PRINT_NAME     "process"
#define PRESET_PRINTER_NAME     "machine"
#define PRESET_SLA_PRINT_NAME  "sla_print"
#define PRESET_SLA_MATERIALS_NAME "sla_materials"
#define PRESET_PROFILES_DIR "profiles"
#define PRESET_PROFILES_TEMOLATE_DIR "profiles_template"
#define PRESET_TEMPLATE_DIR "Template"
#define PRESET_CUSTOM_VENDOR "Custom"

//BBS: iot preset type strings
#define PRESET_IOT_PRINTER_TYPE     "printer"
#define PRESET_IOT_FILAMENT_TYPE    "filament"
#define PRESET_IOT_PRINT_TYPE       "print"


//BBS: add json support
#define BBL_JSON_KEY_VERSION        "version"
#define BBL_JSON_KEY_IS_CUSTOM      "is_custom_defined"
#define BBL_JSON_KEY_URL            "url"
#define BBL_JSON_KEY_NAME           "name"
#define BBL_JSON_KEY_DESCRIPTION    "description"
#define BBL_JSON_KEY_FORCE_UPDATE   "force_update"
#define BBL_JSON_KEY_MACHINE_MODEL_LIST     "machine_model_list"
#define BBL_JSON_KEY_PROCESS_LIST   "process_list"
#define BBL_JSON_KEY_SUB_PATH       "sub_path"
#define BBL_JSON_KEY_FILAMENT_LIST  "filament_list"
#define BBL_JSON_KEY_MACHINE_LIST   "machine_list"
#define BBL_JSON_KEY_TYPE           "type"
#define BBL_JSON_KEY_FROM           "from"
#define BBL_JSON_KEY_SETTING_ID     "setting_id"
#define BBL_JSON_KEY_BASE_ID        "base_id"
#define BBL_JSON_KEY_USER_ID        "user_id"
#define BBL_JSON_KEY_FILAMENT_ID    "filament_id"
#define BBL_JSON_KEY_UPDATE_TIME    "updated_time"
#define BBL_JSON_KEY_INHERITS       "inherits"
#define BBL_JSON_KEY_INSTANTIATION  "instantiation"
#define BBL_JSON_KEY_NOZZLE_DIAMETER            "nozzle_diameter"
#define BBL_JSON_KEY_PRINTER_TECH                 "machine_tech"
#define BBL_JSON_KEY_FAMILY                     "family"
#define BBL_JSON_KEY_BED_MODEL                  "bed_model"
#define BBL_JSON_KEY_BED_TEXTURE                "bed_texture"
#define BBL_JSON_KEY_IMAGE_BED_TYPE             "image_bed_type"
#define BBL_JSON_KEY_BOTTOM_TEXTURE_END_NAME    "bottom_texture_end_name"
#define BBL_JSON_KEY_USE_DOUBLE_EXTRUDER_DEFAULT_TEXTURE  "use_double_extruder_default_texture"
#define BBL_JSON_KEY_BOTTOM_TEXTURE_RECT        "bottom_texture_rect"
#define BBL_JSON_KEY_MIDDLE_TEXTURE_RECT        "middle_texture_rect"

#define BBL_JSON_KEY_HOTEND_MODEL               "hotend_model"
#define BBL_JSON_KEY_DEFAULT_MATERIALS          "default_materials"
#define BBL_JSON_KEY_NOT_SUPPORT_BED_TYPE       "not_support_bed_type"
#define BBL_JSON_KEY_MODEL_ID                   "model_id"

// Orca extension
#define ORCA_JSON_KEY_RENAMED_FROM              "renamed_from"


namespace Slic3r {

class AppConfig;
class PresetBundle;

enum ConfigFileType
{
    CONFIG_FILE_TYPE_UNKNOWN,
    CONFIG_FILE_TYPE_APP_CONFIG,
    CONFIG_FILE_TYPE_CONFIG,
    CONFIG_FILE_TYPE_CONFIG_BUNDLE,
};

//BBS: add a function to load the version from xxx.json
extern Semver get_version_from_json(std::string file_path);
//BBS: add a function to load the key-values from xxx.json
extern int get_values_from_json(std::string file_path, std::vector<std::string>& keys, std::map<std::string, std::string>& key_values);

extern ConfigFileType guess_config_file_type(const boost::property_tree::ptree &tree);

extern void extend_default_config_length(DynamicPrintConfig& config, const bool set_nil_to_default, const DynamicPrintConfig& defaults);

class VendorProfile
{
public:
    std::string                     name;
    std::string                     id;
    Semver                          config_version;
    std::string                     config_update_url;
    std::string                     changelog_url;

    struct PrinterVariant {
        PrinterVariant() {}
        PrinterVariant(const std::string &name) : name(name) {}
        std::string                 name;
    };

    struct PrinterModel {
        PrinterModel() {}
        std::string                 id;
        std::string                 name;
        //BBS: this is internal id for the printer. Currently only used for searching in database
        std::string                 model_id;
        PrinterTechnology           technology;
        std::string                 family;
        std::vector<PrinterVariant> variants;
        std::vector<std::string>	default_materials;
        std::vector<std::string>    not_support_bed_types;
        // Vendor & Printer Model specific print bed model & texture.
        std::string 			 	bed_model;
        std::string 				bed_texture;
        std::string                 image_bed_type;
        std::string                 bottom_texture_end_name;
        std::string                 use_double_extruder_default_texture;
        std::string                 bottom_texture_rect;
        std::string                 middle_texture_rect;
        std::string                 hotend_model;
        PrinterVariant*       variant(const std::string &name) {
            for (auto &v : this->variants)
                if (v.name == name)
                    return &v;
            return nullptr;
        }

        const PrinterVariant* variant(const std::string &name) const { return const_cast<PrinterModel*>(this)->variant(name); }
    };
    std::vector<PrinterModel>          models;

    std::set<std::string>              default_filaments;
    std::set<std::string>              default_sla_materials;

    VendorProfile() {}
    VendorProfile(std::string id) : id(std::move(id)) {}

    bool 		valid() const { return ! name.empty() && ! id.empty() && config_version.valid(); }

    // Load VendorProfile from an ini file.
    // If `load_all` is false, only the header with basic info (name, version, URLs) is loaded.
    static VendorProfile from_ini(const boost::filesystem::path &path, bool load_all=true);
    static VendorProfile from_ini(const boost::property_tree::ptree &tree, const boost::filesystem::path &path, bool load_all=true);

    size_t      num_variants() const { size_t n = 0; for (auto &model : models) n += model.variants.size(); return n; }
    std::vector<std::string> families() const;

    bool        operator< (const VendorProfile &rhs) const { return this->id <  rhs.id; }
    bool        operator==(const VendorProfile &rhs) const { return this->id == rhs.id; }
};

class Preset;

// Helper to hold a profile with its vendor definition, where the vendor definition may have been extracted from a parent system preset.
// The parent preset is only accessible through PresetCollection, therefore to allow definition of the various is_compatible_with methods
// outside of the PresetCollection, this composite is returned by PresetCollection::get_preset_with_vendor_profile() when needed.
struct PresetWithVendorProfile {
	PresetWithVendorProfile(const Preset &preset, const VendorProfile *vendor) : preset(preset), vendor(vendor) {}
	const Preset 		&preset;
	const VendorProfile *vendor;
};

// Note: it is imporant that map is used here rather than unordered_map,
// because we need iterators to not be invalidated,
// because Preset and the ConfigWizard hold pointers to VendorProfiles.
// XXX: maybe set is enough (cf. changes in Wizard)
typedef std::map<std::string, VendorProfile> VendorMap;

class Preset
{
public:
    enum Type
    {
        TYPE_INVALID,
        TYPE_PRINT,
        TYPE_SLA_PRINT,
        TYPE_FILAMENT,
        TYPE_SLA_MATERIAL,
        TYPE_PRINTER,
        TYPE_COUNT,
        // This type is here to support PresetConfigSubstitutions for physical printers, however it does not belong to the Preset class,
        // PhysicalPrinter class is used instead.
        TYPE_PHYSICAL_PRINTER,
        // BBS: plate config
        TYPE_PLATE,
        // BBS: model config
        TYPE_MODEL,
    };

    Type                type        = TYPE_INVALID;

    // The preset represents a "default" set of properties,
    // pulled from the default values of the PrintConfig (see PrintConfigDef for their definitions).
    bool                is_default = false;
    // External preset points to a configuration, which has been loaded but not imported
    // into the Slic3r default configuration location.
    bool                is_external = false;
    // System preset is read-only.
    bool                is_system   = false;
    // Preset is visible, if it is associated with a printer model / variant that is enabled in the AppConfig
    // or if it has no printer model / variant association.
    // Also the "default" preset is only visible, if it is the only preset in the list.
    bool                is_visible  = true;
    // Has this preset been modified?
    bool                is_dirty    = false;
    // Is this preset compatible with the currently active printer?
    bool                is_compatible = true;

    //BBS: add type for project-embedded
    bool                is_project_embedded = false;
    ConfigSubstitutions *loading_substitutions{nullptr};
    bool                is_user() const { return ! this->is_default && ! this->is_system && ! this->is_project_embedded; }
    //bool                is_user() const { return ! this->is_default && ! this->is_system; }

    // Name of the preset, usually derived form the file name.
    std::string         name;
    // File name of the preset. This could be a Print / Filament / Printer preset,
    // or a Configuration file bundling the Print + Filament + Printer presets (in that case is_external and possibly is_system will be true),
    // or it could be a G-code (again, is_external will be true).
    std::string         file;
    // If this is a system profile, then there should be a vendor data available to display at the UI.
    const VendorProfile *vendor      = nullptr;

    // Has this profile been loaded?
    bool                loaded      = false;

    // Configuration data, loaded from a file, or set from the defaults.
    DynamicPrintConfig  config;

    // Alias of the preset
    std::string         alias;
    // List of profile names, from which this profile was renamed at some point of time.
    // This list is then used to match profiles by their names when loaded from .gcode, .3mf, .amf,
    // and to match the "inherits" field of user profiles with updated system profiles.
    std::vector<std::string> renamed_from;

    // Orca: maintain a list of printer models that are excluded from this preset, designed for filaments without compatible_printer defined
    // (hence they are visible to all printer models by default) in Orca Filament Library. However, we might have speciliazed filament for
    // certain printer models defined in the vendor profile as well, in this case we want to hide this generic preset for these printer models.
    std::set<std::string> m_excluded_from;

    // Orca: flag to indicate if this preset is from Orca Filament Library
    bool m_from_orca_filament_lib = false;

    //BBS
    Semver              version;         // version of preset
    std::string         ini_str;         // ini string of preset
    std::string         setting_id;      // setting id in cloud database
    std::string         filament_id;      // setting id in cloud database
    std::string         user_id;         // preset user_id
    std::string         base_id;         // base id of preset
    std::string         sync_info;       // enum: "delete", "create", "update", ""
    std::string         description;     //
    long long           updated_time{0};    //last updated time
    std::map<std::string, std::string> key_values;

    static std::string  get_type_string(Preset::Type type);
    // get string type for iot
    static std::string  get_iot_type_string(Preset::Type type);
    static Preset::Type get_type_from_string(std::string type_str);
    void                load_info(const std::string& file);
    void                save_info(std::string file = "");
    void                remove_files();

    //BBS: add logic for only difference save
    //if parent_config is null, save all keys, otherwise, only save difference
    void                save(DynamicPrintConfig* parent_config);
    void                reload(Preset const & parent);

    // Return a label of this preset, consisting of a name and a "(modified)" suffix, if this preset is dirty.
    std::string         label(bool no_alias) const;

    // Set the is_dirty flag if the provided config is different from the active one.
    void                set_dirty(const DynamicPrintConfig &config) { this->is_dirty = ! this->config.diff(config).empty(); }
    void                set_dirty(bool dirty = true) { this->is_dirty = dirty; }
    void                reset_dirty() { this->is_dirty = false; }

    // Returns the name of the preset, from which this preset inherits.
    static std::string& inherits(DynamicPrintConfig &cfg) { return cfg.option<ConfigOptionString>("inherits", true)->value; }
    std::string&        inherits() { return Preset::inherits(this->config); }
    const std::string&  inherits() const { return Preset::inherits(const_cast<Preset*>(this)->config); }

    // Returns the "compatible_prints_condition".
    static std::string& compatible_prints_condition(DynamicPrintConfig &cfg) { return cfg.option<ConfigOptionString>("compatible_prints_condition", true)->value; }
    std::string&        compatible_prints_condition() {
		assert(this->type == TYPE_FILAMENT || this->type == TYPE_SLA_MATERIAL);
        return Preset::compatible_prints_condition(this->config);
    }
    const std::string&  compatible_prints_condition() const { return const_cast<Preset*>(this)->compatible_prints_condition(); }

    // Returns the "compatible_printers_condition".
    static std::string& compatible_printers_condition(DynamicPrintConfig &cfg) { return cfg.option<ConfigOptionString>("compatible_printers_condition", true)->value; }
    std::string&        compatible_printers_condition() {
		assert(this->type == TYPE_PRINT || this->type == TYPE_SLA_PRINT || this->type == TYPE_FILAMENT || this->type == TYPE_SLA_MATERIAL);
        return Preset::compatible_printers_condition(this->config);
    }
    const std::string&  compatible_printers_condition() const { return const_cast<Preset*>(this)->compatible_printers_condition(); }

    // Return a printer technology, return ptFFF if the printer technology is not set.
    static PrinterTechnology printer_technology(const DynamicPrintConfig &cfg) {
        auto *opt = cfg.option<ConfigOptionEnum<PrinterTechnology>>("printer_technology");
        // The following assert may trigger when importing some legacy profile,
        // but it is safer to keep it here to capture the cases where the "printer_technology" key is queried, where it should not.
//        assert(opt != nullptr);
        return (opt == nullptr) ? ptFFF : opt->value;
    }
    PrinterTechnology   printer_technology() const { return Preset::printer_technology(this->config); }
    // This call returns a reference, it may add a new entry into the DynamicPrintConfig.
    PrinterTechnology&  printer_technology_ref() { return this->config.option<ConfigOptionEnum<PrinterTechnology>>("printer_technology", true)->value; }

    // Set is_visible according to application config
    void                set_visible_from_appconfig(const AppConfig &app_config);

    // Resize the extruder specific fields, initialize them with the content of the 1st extruder.
    void                set_num_extruders(unsigned int n) { this->config.set_num_extruders(n); }

    // Sort lexicographically by a preset name. The preset name shall be unique across a single PresetCollection.
    bool                operator<(const Preset &other) const { return this->name < other.name; }

    // special for upport G and Support W
    std::string get_filament_type(std::string &display_filament_type);
    std::string get_printer_type(PresetBundle *preset_bundle); // get edited preset type
    std::string get_current_printer_type(PresetBundle *preset_bundle); // get current preset type

    static void get_extruder_names_and_keysets(Type type, std::string& extruder_id_name, std::string& extruder_variant_name, std::set<std::string>** p_key_set1, std::set<std::string>** p_key_set2);
    std::string get_printer_id() const { return vendor ? vendor->id : ""; }

    bool has_lidar(PresetBundle *preset_bundle);
    bool is_custom_defined();

    BedType get_default_bed_type(PresetBundle *preset_bundle);
    bool has_cali_lines(PresetBundle* preset_bundle);


    static double convert_pellet_flow_to_filament_diameter(double pellet_flow_coefficient)
    {
        return sqrt(4 / (PI * pellet_flow_coefficient)); 
    }

    static double convert_filament_diameter_to_pellet_flow(double filament_diameter)
    {
        return 4 / (pow(filament_diameter, 2) * PI); 
    }

    static const std::vector<std::string>&  print_options();
    static const std::vector<std::string>&  filament_options();
    // Printer options contain the nozzle options.
    static const std::vector<std::string>&  printer_options();
    // Nozzle options of the printer options.
    static const std::vector<std::string>&  nozzle_options();
    // Printer machine limits, those are contained in printer_options().
    static const std::vector<std::string>&  machine_limits_options();

    static const std::vector<std::string>&  sla_printer_options();
    static const std::vector<std::string>&  sla_material_options();
    static const std::vector<std::string>&  sla_print_options();

	static void                             update_suffix_modified(const std::string& new_suffix_modified);
    static const std::string&               suffix_modified();
    static std::string                      remove_suffix_modified(const std::string& name);
    static void                             normalize(DynamicPrintConfig &config);
    // Report configuration fields, which are misplaced into a wrong group, remove them from the config.
    static std::string                      remove_invalid_keys(DynamicPrintConfig &config, const DynamicPrintConfig &default_config);

    // BBS: move constructor to public
    Preset(Type type, const std::string &name, bool is_default = false) : type(type), is_default(is_default), name(name) {}

protected:
    Preset() = default;

    friend class        PresetCollection;
    friend class        PresetBundle;
};

bool is_compatible_with_print  (const PresetWithVendorProfile &preset, const PresetWithVendorProfile &active_print, const PresetWithVendorProfile &active_printer);
bool is_compatible_with_printer(const PresetWithVendorProfile &preset, const PresetWithVendorProfile &active_printer, const DynamicPrintConfig *extra_config);
bool is_compatible_with_printer(const PresetWithVendorProfile &preset, const PresetWithVendorProfile &active_printer);

enum class PresetSelectCompatibleType {
	// Never select a compatible preset if the newly selected profile is not compatible.
	Never,
	// Only select a compatible preset if the active profile used to be compatible, but it is no more.
	OnlyIfWasCompatible,
	// Always select a compatible preset if the active profile is no more compatible.
	Always
};

// Substitutions having been performed during parsing a single configuration file.
struct PresetConfigSubstitutions {
    // User readable preset name.
    std::string                             preset_name;
    // Type of the preset (Print / Filament / Printer ...)
    Preset::Type                            preset_type;
    enum class Source {
        UserFile,
        ConfigBundle,
        //BBS: add cloud and project type
        UserCloud,
        ProjectFile,
    };
    Source                                  preset_source;
    // Source of the preset. It may be empty in case of a ConfigBundle being loaded.
    std::string                             preset_file;
    // What config value has been substituted with what.
    ConfigSubstitutions                     substitutions;
};

// Substitutions having been performed during parsing a set of configuration files, for example when starting up
// PrusaSlicer and reading the user Print / Filament / Printer profiles.
using PresetsConfigSubstitutions = std::vector<PresetConfigSubstitutions>;

// Collections of presets of the same type (one of the Print, Filament or Printer type).
class PresetCollection
{
public:
    // Initialize the PresetCollection with the "- default -" preset.
    PresetCollection(Preset::Type type, const std::vector<std::string> &keys, const Slic3r::StaticPrintConfig &defaults, const std::string &default_name = "Default Setting");

    typedef std::deque<Preset>::iterator Iterator;
    typedef std::deque<Preset>::const_iterator ConstIterator;
    typedef std::function<void(Preset* preset, std::string sync_info)> SyncFunc;
    //BBS get m_presets begin
    Iterator        lbegin() { return m_presets.begin(); }
    //BBS: validate_preset
    bool            validate_preset(const std::string &name, std::string &inherit);

    Iterator        begin() { return m_presets.begin() + m_num_default_presets; }
    ConstIterator   begin() const { return m_presets.cbegin() + m_num_default_presets; }
    ConstIterator   cbegin() const { return m_presets.cbegin() + m_num_default_presets; }
    Iterator        end() { return m_presets.end(); }
    ConstIterator   end() const { return m_presets.cend(); }
    ConstIterator   cend() const { return m_presets.cend(); }

    //BBS
    Iterator        erase(Iterator it) { return m_presets.erase(it); }
    SyncFunc        sync_func{ nullptr };
    void            set_sync_func(SyncFunc func) { sync_func = func; }
    //BBS: mutex
    void            lock() { m_mutex.lock(); }
    void            unlock() { m_mutex.unlock(); }

    void            reset(bool delete_files);

    Preset::Type    type() const { return m_type; }
    // Name, to be used on the screen and in error messages. Not localized.
    std::string     name() const;
    // Name, to be used as a section name in config bundle, and as a folder name for presets.
    std::string     section_name() const;
    const std::deque<Preset>& operator()() const { return m_presets; }

    // Add default preset at the start of the collection, increment the m_default_preset counter.
    void            add_default_preset(const std::vector<std::string> &keys, const Slic3r::StaticPrintConfig &defaults, const std::string &preset_name);

    // Load ini files of the particular type from the provided directory path.
    void            load_presets(const std::string &dir_path, const std::string &subdir, PresetsConfigSubstitutions& substitutions, ForwardCompatibilitySubstitutionRule rule);

    //BBS: update user presets directory
    void            update_user_presets_directory(const std::string& dir_path, const std::string& type);
    void            save_user_presets(const std::string& dir_path, const std::string& type, std::vector<std::string>& need_to_delete_list);
    bool            load_user_preset(std::string name, std::map<std::string, std::string> preset_values, PresetsConfigSubstitutions& substitutions, ForwardCompatibilitySubstitutionRule rule);
    void            update_after_user_presets_loaded();
    //BBS: get user presets
    int  get_user_presets(PresetBundle *preset_bundle, std::vector<Preset> &result_presets);
    void set_sync_info_and_save(std::string name, std::string setting_id, std::string syncinfo, long long update_time);
    bool need_sync(std::string name, std::string setting_id, long long update_time);

    //BBS: add function to generate differed preset for save
    //the pointer should be freed by the caller
    Preset* get_preset_differed_for_save(Preset& preset);
    //BBS:get the differencen values to update
    int get_differed_values_to_update(Preset& preset, std::map<std::string, std::string>& key_values);

    //BBS: add project embedded presets logic
    void load_project_embedded_presets(std::vector<Preset*>& project_presets, const std::string& type, PresetsConfigSubstitutions& substitutions, ForwardCompatibilitySubstitutionRule rule);
    std::vector<Preset*> get_project_embedded_presets();
    bool reset_project_embedded_presets();

    // Load a preset from an already parsed config file, insert it into the sorted sequence of presets
    // and select it, losing previous modifications.
    Preset&         load_preset(const std::string &path, const std::string &name, const DynamicPrintConfig &config, bool select = true, Semver file_version = Semver());
    Preset&         load_preset(const std::string &path, const std::string &name, DynamicPrintConfig &&config, bool select = true, Semver file_version = Semver());

    bool clone_presets(std::vector<Preset const *> const &presets, std::vector<std::string> &failures, std::function<void(Preset &, Preset::Type &)> modifier, bool force_rewritten = false);
    bool clone_presets_for_printer(
        std::vector<Preset const *> const &templates, std::vector<std::string> &failures, std::string const &printer, std::function <std::string(std::string)> create_filament_id, bool force_rewritten = false);
    bool clone_presets_for_filament(Preset const *const &     preset,
                                    std::vector<std::string> &failures,
                                    std::string const &       filament_name,
                                    std::string const &       filament_id,
                                    const DynamicConfig &     dynamic_config,
                                    const std::string &       compatible_printers,
                                    bool                      force_rewritten = false);

    std::map<std::string, std::vector<Preset const *>> get_filament_presets() const;

    // Returns a loaded preset, returns true if an existing preset was selected AND modified from config.
    // In that case the successive filament loaded for a multi material printer should not be modified, but
    // an external preset should be created instead.
    enum class LoadAndSelect {
        // Never select
        Never,
        // Always select
        Always,
        // Select a profile only if it was modified.
        OnlyIfModified,
    };
    std::pair<Preset*, bool> load_external_preset(
        // Path to the profile source file (a G-code, an AMF or 3MF file, a config file)
        const std::string           &path,
        // Name of the profile, derived from the source file name.
        const std::string           &name,
        // Original name of the profile, extracted from the loaded config. Empty, if the name has not been stored.
        const std::string           &original_name,
        // Config to initialize the preset from.
        const DynamicPrintConfig    &config,
        //different settings list
        const std::set<std::string> &different_settings_list,
        // Select the preset after loading?
        LoadAndSelect                select = LoadAndSelect::Always,
        const Semver                file_version = Semver(),
        const std::string           filament_id = std::string());

    // Save the preset under a new name. If the name is different from the old one,
    // a new preset is stored into the list of presets.
    // All presets are marked as not modified and the new preset is activated.
    //BBS: add project embedded preset logic
    void            save_current_preset(const std::string &new_name, bool detach = false, bool save_to_project = false, Preset* _curr_preset = nullptr, const Preset* _current_printer = nullptr);

    // Delete the current preset, activate the first visible preset.
    // returns true if the preset was deleted successfully.
    bool            delete_current_preset();
    // Delete the current preset, activate the first visible preset.
    // returns true if the preset was deleted successfully.
    bool            delete_preset(const std::string& name);

    // Enable / disable the "- default -" preset.
    void            set_default_suppressed(bool default_suppressed);
    bool            is_default_suppressed() const { return m_default_suppressed; }

    // Select a preset. If an invalid index is provided, the first visible preset is selected.
    Preset&         select_preset(size_t idx);
    // Return the selected preset, without the user modifications applied.
    Preset&         get_selected_preset() {
        //BBS fix crash when m_idx_selected == -1, give a default value
        if ((m_idx_selected < 0) || (m_idx_selected >= m_presets.size())) {
            select_preset(first_visible_idx());
        }
        return m_presets[m_idx_selected];
    }
    const Preset&   get_selected_preset() const { return m_presets[m_idx_selected]; }
    size_t          get_selected_idx()    const { return m_idx_selected; }
    // Returns the name of the selected preset, or an empty string if no preset is selected.
    std::string     get_selected_preset_name() const {
        if (m_idx_selected == size_t(-1) || m_idx_selected >= m_presets.size())
            return std::string();
        return this->get_selected_preset().name;
    }
    // For the current edited preset, return the parent preset if there is one.
    // If there is no parent preset, nullptr is returned.
    // The parent preset may be a system preset or a user preset, which will be
    // reflected by the UI.
    const Preset*   get_selected_preset_parent() const;
	// Get parent preset for a child preset, based on the "inherits" field of a child,
	// where the "inherits" profile name is searched for in both m_presets and m_map_system_profile_renamed.
	const Preset*	get_preset_parent(const Preset& child) const;
	const Preset*	get_preset_base(const Preset& child) const;
	// Return the selected preset including the user modifications.
    Preset&         get_edited_preset()         { return m_edited_preset; }
    const Preset&   get_edited_preset() const   { return m_edited_preset; }

    const Preset& get_selected_preset_base() const { return *get_preset_base(m_presets[m_idx_selected]); }

    // Return the last saved preset.
//  const Preset&   get_saved_preset() const { return m_saved_preset; }

    // Return vendor of the first parent profile, for which the vendor is defined, or null if such profile does not exist.
    PresetWithVendorProfile get_preset_with_vendor_profile(const Preset &preset) const;
    PresetWithVendorProfile get_edited_preset_with_vendor_profile() const { return this->get_preset_with_vendor_profile(this->get_edited_preset()); }

    const std::string& 		get_preset_name_by_alias(const std::string& alias) const;
	const std::string*		get_preset_name_renamed(const std::string &old_name) const;
    bool                    is_alias_exist(const std::string &alias, Preset* preset = nullptr);
    void                    set_printer_hold_alias(const std::string &alias, Preset &preset, bool remove = false);

	// used to update preset_choice from Tab
	const std::deque<Preset>&	get_presets() const	{ return m_presets; }
    size_t                      get_idx_selected()	{ return m_idx_selected; }
	static const std::string&	get_suffix_modified();

    // Return a preset possibly with modifications.
	Preset&			default_preset(size_t idx = 0)		 { assert(idx < m_num_default_presets); return m_presets[idx]; }
	const Preset&   default_preset(size_t idx = 0) const { assert(idx < m_num_default_presets); return m_presets[idx]; }
	virtual const Preset& default_preset_for(const DynamicPrintConfig & /* config */) const { return this->default_preset(); }
    // Return a preset by an index. If the preset is active, a temporary copy is returned.
    Preset&         preset(size_t idx, bool real = false) {
        if (real) return m_presets[idx];
        return (idx == m_idx_selected) ? m_edited_preset : m_presets[idx];
    }
    const Preset&   preset(size_t idx) const    { return const_cast<PresetCollection*>(this)->preset(idx); }
    void            discard_current_changes() {
        m_presets[m_idx_selected].reset_dirty();
        m_edited_preset = m_presets[m_idx_selected];
//        update_saved_preset_from_current_preset();
    }

    // Return a preset by its name. If the preset is active, a temporary copy is returned.
    // If a preset is not found by its name, null is returned.
    // return real pointer if set real = true
    Preset* find_preset(const std::string& name, bool first_visible_if_not_found = false, bool real = false, bool only_from_library = false);
    const Preset* find_preset(const std::string& name, bool first_visible_if_not_found = false) const
    {
        return const_cast<PresetCollection*>(this)->find_preset(name, first_visible_if_not_found);
    }
    // Orca: find preset, if not found, keep searching in the renamed history. This is function should only be used when find
    // system(parent) presets for custom preset.
    Preset* find_preset2(const std::string& name, bool auto_match = true);
    const Preset* find_preset2(const std::string& name, bool auto_match = true) const
    {
        return const_cast<PresetCollection*>(this)->find_preset2(name, auto_match);
    }
    size_t first_visible_idx() const;
    // Return index of the first compatible preset. Certainly at least the '- default -' preset shall be compatible.
    // If one of the prefered_alternates is compatible, select it.
    template<typename PreferedCondition> size_t first_compatible_idx(PreferedCondition prefered_condition) const
    {
        size_t i             = m_default_suppressed ? m_num_default_presets : 0;
        size_t n             = this->m_presets.size();
        size_t i_compatible  = n;
        int    match_quality = -1;
        for (; i < n; ++i)
            // Since we use the filament selection from Wizard, it's needed to control the preset visibility too
            if (m_presets[i].is_compatible) {
                int this_match_quality = prefered_condition(m_presets[i]);
                if (this_match_quality > match_quality) {
                    if (match_quality == std::numeric_limits<int>::max())
                        // Better match will not be found.
                        return i;
                    // Store the first compatible profile with highest match quality into i_compatible.
                    i_compatible  = i;
                    match_quality = this_match_quality;
                }
            }
        return (i_compatible == n) ?
                    // No compatible preset found, return the default preset.
                    0 :
                    // Compatible preset found.
                    i_compatible;
}
    // Return index of the first compatible preset. Certainly at least the '- default -' preset shall be compatible.
    size_t          first_compatible_idx() const { return this->first_compatible_idx([](const Preset&) -> int { return 0; }); }

    std::vector<std::string> diameters_of_selected_printer();

    // Return index of the first visible preset. Certainly at least the '- default -' preset shall be visible.
    // Return the first visible preset. Certainly at least the '- default -' preset shall be visible.
    Preset&         first_visible()             { return this->preset(this->first_visible_idx()); }
    const Preset&   first_visible() const       { return this->preset(this->first_visible_idx()); }
    Preset&         first_compatible()          { return this->preset(this->first_compatible_idx()); }
    template<typename PreferedCondition>
    Preset&         first_compatible(PreferedCondition prefered_condition) { return this->preset(this->first_compatible_idx(prefered_condition)); }
    const Preset&   first_compatible() const    { return this->preset(this->first_compatible_idx()); }

    // Return number of presets including the "- default -" preset.
    size_t          size() const                { return m_presets.size(); }
    bool            has_defaults_only() const   { return m_presets.size() <= m_num_default_presets; }

    // For Print / Filament presets, disable those, which are not compatible with the printer.
    template<typename PreferedCondition>
    void            update_compatible(const PresetWithVendorProfile &active_printer, const PresetWithVendorProfile *active_print, PresetSelectCompatibleType select_other_if_incompatible, PreferedCondition prefered_condition)
    {
        if (this->update_compatible_internal(active_printer, active_print, select_other_if_incompatible) == (size_t)-1) {
            // Find some other compatible preset, or the "-- default --" preset.
            size_t index = this->first_compatible_idx(prefered_condition);
            this->select_preset(index);
        }
    }
    void            update_compatible(const PresetWithVendorProfile &active_printer, const PresetWithVendorProfile *active_print, PresetSelectCompatibleType select_other_if_incompatible)
        { this->update_compatible(active_printer, active_print, select_other_if_incompatible, [](const Preset&) -> int { return 0; }); }

    size_t          num_visible() const { return std::count_if(m_presets.begin(), m_presets.end(), [](const Preset &preset){return preset.is_visible;}); }

    // Compare the content of get_selected_preset() with get_edited_preset() configs, return true if they differ.
    bool                        current_is_dirty() const
        { return is_dirty(&this->get_edited_preset(), &this->get_selected_preset()); }
    // Compare the content of get_selected_preset() with get_edited_preset() configs, return the list of keys where they differ.
    std::vector<std::string>    current_dirty_options(const bool deep_compare = false) const
        { return dirty_options(&this->get_edited_preset(), &this->get_selected_preset(), deep_compare); }
    // Compare the content of get_selected_preset() with get_edited_preset() configs, return the list of keys where they differ.
    std::vector<std::string>    current_different_from_parent_options(const bool deep_compare = false) const
        { return dirty_options(&this->get_edited_preset(), this->get_selected_preset_parent(), deep_compare); }

    // Compare the content of get_saved_preset() with get_edited_preset() configs, return true if they differ.
    bool                        saved_is_dirty() const
        { return is_dirty(&this->get_edited_preset(), &m_saved_preset); }
    // Compare the content of get_saved_preset() with get_edited_preset() configs, return the list of keys where they differ.
//    std::vector<std::string>    saved_dirty_options() const
//        { return dirty_options(&this->get_edited_preset(), &this->get_saved_preset(), /* deep_compare */ false); }
    // Copy edited preset into saved preset.
    void                        update_saved_preset_from_current_preset() { m_saved_preset = m_edited_preset; }

    // Return a sorted list of system preset names.
    // Used for validating the "inherits" flag when importing user's config bundles.
    // Returns names of all system presets including the former names of these presets.
    std::vector<std::string>    system_preset_names() const;

    // Update a dirty flag of the current preset
    // Return true if the dirty flag changed.
    bool            update_dirty();

    // Select a profile by its name. Return true if the selection changed.
    // Without force, the selection is only updated if the index changes.
    // With force, the changes are reverted if the new index is the same as the old index.
    bool            select_preset_by_name(const std::string &name, bool force);
    bool is_base_preset(const Preset &preset) const { return preset.is_system || (preset.is_user() && preset.inherits().empty()); }

    // Generate a file path from a profile name. Add the ".ini" suffix if it is missing.
    std::string     path_from_name(const std::string &new_name, bool detach = false) const;
    std::string     path_for_preset(const Preset & preset) const;

    size_t num_default_presets() { return m_num_default_presets; }

protected:
    PresetCollection() = default;
    // Copy constructor and copy operators are not to be used from outside PresetBundle,
    // as the Profile::vendor points to an instance of VendorProfile stored at parent PresetBundle!
    PresetCollection(const PresetCollection &other) = default;
    //BBS: add operator= logic insteadof default
    PresetCollection& operator=(const PresetCollection &other);
    // After copying a collection with the default operators above, call this function
    // to adjust Profile::vendor pointers.
    void            update_vendor_ptrs_after_copy(const VendorMap &vendors);

    // Select a preset, if it exists. If it does not exist, select an invalid (-1) index.
    // This is a temporary state, which shall be fixed immediately by the following step.
    bool            select_preset_by_name_strict(const std::string &name);

    // Merge one vendor's presets with the other vendor's presets, report duplicates.
    std::vector<std::string> merge_presets(PresetCollection &&other, const VendorMap &new_vendors);

    // Update m_map_alias_to_profile_name from loaded system profiles.
	void 			update_map_alias_to_profile_name();

    // Update m_map_system_profile_renamed from loaded system profiles.
    void 			update_map_system_profile_renamed();

    // Orca: update m_excluded_from loaded system profiles.
    void 			update_library_profile_excluded_from();


    void            set_custom_preset_alias(Preset &preset);

private:
    // Find a preset position in the sorted list of presets.
    // The "-- default -- " preset is always the first, so it needs
    // to be handled differently.
    // If a preset does not exist, an iterator is returned indicating where to insert a preset with the same name.
    std::deque<Preset>::iterator find_preset_internal(const std::string &name, bool from_orca_lib_only = false)
    {
        auto it = Slic3r::lower_bound_by_predicate(m_presets.begin() + m_num_default_presets, m_presets.end(), [&name](const auto& l) { return l.name < name;  });
        if (it == m_presets.end() || it->name != name) {
            // Preset has not been not found in the sorted list of non-default presets. Try the defaults.
            for (size_t i = 0; i < m_num_default_presets; ++ i)
                if (m_presets[i].name == name && (!from_orca_lib_only || m_presets[i].m_from_orca_filament_lib)) {
                    it = m_presets.begin() + i;
                    break;
                }
        }
        return it;
    }
    std::deque<Preset>::const_iterator find_preset_internal(const std::string &name) const
        { return const_cast<PresetCollection*>(this)->find_preset_internal(name); }
    std::deque<Preset>::iterator 	   find_preset_renamed(const std::string &name) {
    	auto it_renamed = m_map_system_profile_renamed.find(name);
    	auto it = (it_renamed == m_map_system_profile_renamed.end()) ? m_presets.end() : this->find_preset_internal(it_renamed->second);
    	assert((it_renamed == m_map_system_profile_renamed.end()) || (it != m_presets.end() && it->name == it_renamed->second));
    	return it;
    }
    std::deque<Preset>::const_iterator find_preset_renamed(const std::string &name) const
        { return const_cast<PresetCollection*>(this)->find_preset_renamed(name); }

    size_t update_compatible_internal(const PresetWithVendorProfile &active_printer, const PresetWithVendorProfile *active_print, PresetSelectCompatibleType unselect_if_incompatible);
public:
    static bool                     is_dirty(const Preset *edited, const Preset *reference);
    static std::vector<std::string> dirty_options(const Preset *edited, const Preset *reference, const bool deep_compare = false);
    //BBS: add function for dirty_options_without_option_list
    static std::vector<std::string> dirty_options_without_option_list(const Preset *edited, const Preset *reference, const std::set<std::string>& option_ignore_list, const bool deep_compare = false);
private:
    // Type of this PresetCollection: TYPE_PRINT, TYPE_FILAMENT or TYPE_PRINTER.
    Preset::Type            m_type;
    // List of presets, starting with the "- default -" preset.
    // Use deque to force the container to allocate an object per each entry,
    // so that the addresses of the presets don't change during resizing of the container.
    std::deque<Preset>      m_presets;
    // System profiles may have aliases. Map to the full profile name.
    std::map<std::string, std::vector<std::string>> m_map_alias_to_profile_name;
    std::unordered_map<std::string, std::unordered_set<std::string>> m_printer_hold_alias;
    // Map from old system profile name to a current system profile name.
    std::map<std::string, std::string> m_map_system_profile_renamed;
    // Initially this preset contains a copy of the selected preset. Later on, this copy may be modified by the user.
    Preset                  m_edited_preset;
    // Contains a copy of the last saved selected preset.
    Preset                  m_saved_preset;

    // Selected preset.
    size_t                  m_idx_selected;
    // Is the "- default -" preset suppressed?
    bool                    m_default_suppressed  = true;
    size_t                  m_num_default_presets = 0;

    // Path to the directory to store the config files into.
    std::string             m_dir_path;

    // to access select_preset_by_name_strict() and the default & copy constructors.
    friend class PresetBundle;

    //BBS: mutex
    std::mutex          m_mutex;

    // Orca: used for validation only
    int m_errors = 0;
};

// Printer supports the FFF and SLA technologies, with different set of configuration values,
// therefore this PresetCollection needs to handle two defaults.
class PrinterPresetCollection : public PresetCollection
{
public:
    PrinterPresetCollection(Preset::Type type, const std::vector<std::string> &keys, const Slic3r::StaticPrintConfig &defaults, const std::string &default_name = "Default Printer") :
		PresetCollection(type, keys, defaults, default_name) {}

    const Preset&   default_preset_for(const DynamicPrintConfig &config) const override;

    const Preset*   find_system_preset_by_model_and_variant(const std::string &model_id, const std::string &variant) const;
    const Preset*   find_custom_preset_by_model_and_variant(const std::string &model_id, const std::string &variant) const;

    bool            only_default_printers() const;
private:
    PrinterPresetCollection() = default;
    PrinterPresetCollection(const PrinterPresetCollection &other) = default;
    PrinterPresetCollection& operator=(const PrinterPresetCollection &other) = default;

    friend class PresetBundle;
};

namespace PresetUtils {
	// PrinterModel of a system profile, from which this preset is derived, or null if it is not derived from a system profile.
	const VendorProfile::PrinterModel* system_printer_model(const Preset &preset);
    std::string system_printer_bed_model(const Preset& preset);
    std::string system_printer_bed_texture(const Preset& preset);
    std::string system_printer_hotend_model(const Preset& preset);
} // namespace PresetUtils


//////////////////////////////////////////////////////////////////////

class PhysicalPrinter
{
public:
    PhysicalPrinter(const std::string& name, const DynamicPrintConfig &default_config);
    PhysicalPrinter(const std::string& name, const DynamicPrintConfig &default_config, const Preset& preset);
    void set_name(const std::string &name);

    // Name of the Physical Printer, usually derived form the file name.
    std::string         name;
    // File name of the Physical Printer.
    std::string         file;
    // Configuration data, loaded from a file, or set from the defaults.
    DynamicPrintConfig  config;
    // set of presets used with this physical printer
    std::set<std::string> preset_names;

    // Has this profile been loaded?
    bool                loaded = false;

    static std::string  separator();
    static const std::vector<std::string>&  printer_options();
    static const std::vector<std::string>&  print_host_options();
    static std::vector<std::string>         presets_with_print_host_information(const PrinterPresetCollection& printer_presets);
    static bool has_print_host_information(const DynamicPrintConfig& config);

    const std::set<std::string>&            get_preset_names() const;

    void                update_preset_names_in_config();

    //BBS: change to json format
    //void                save() { this->config.save(this->file); }
    void                save(DynamicPrintConfig* parent_config) { this->config.save_to_json(this->file, std::string("Physical_Printer"), std::string("User"), std::string(SLIC3R_VERSION)); }
    void                save(const std::string& file_name_from, const std::string& file_name_to);

    void                update_from_preset(const Preset& preset);
    void                update_from_config(const DynamicPrintConfig &new_config);

    // add preset to the preset_names
    // return false, if preset with this name is already exist in the set
    bool                add_preset(const std::string& preset_name);
    bool                delete_preset(const std::string& preset_name);
    void                reset_presets();

    // Return a printer technology, return ptFFF if the printer technology is not set.
    static PrinterTechnology printer_technology(const DynamicPrintConfig& cfg) {
        auto* opt = cfg.option<ConfigOptionEnum<PrinterTechnology>>("printer_technology");
        // The following assert may trigger when importing some legacy profile,
        // but it is safer to keep it here to capture the cases where the "printer_technology" key is queried, where it should not.
        return (opt == nullptr) ? ptFFF : opt->value;
    }
    PrinterTechnology   printer_technology() const { return printer_technology(this->config); }

    // Sort lexicographically by a preset name. The preset name shall be unique across a single PresetCollection.
    bool                operator<(const PhysicalPrinter& other) const { return this->name < other.name; }

    // get full printer name included a name of the preset
    std::string         get_full_name(std::string preset_name) const;

    // get printer name from the full name uncluded preset name
    static std::string  get_short_name(std::string full_name);

    // get preset name from the full name uncluded printer name
    static std::string  get_preset_name(std::string full_name);

protected:
    friend class        PhysicalPrinterCollection;
};


// ---------------------------------
// ***  PhysicalPrinterCollection  ***
// ---------------------------------

// Collections of physical printers
class PhysicalPrinterCollection
{
public:
    PhysicalPrinterCollection(const std::vector<std::string>& keys);

    typedef std::deque<PhysicalPrinter>::iterator Iterator;
    typedef std::deque<PhysicalPrinter>::const_iterator ConstIterator;
    Iterator        begin() { return m_printers.begin(); }
    ConstIterator   begin() const { return m_printers.cbegin(); }
    ConstIterator   cbegin() const { return m_printers.cbegin(); }
    Iterator        end() { return m_printers.end(); }
    ConstIterator   end() const { return m_printers.cend(); }
    ConstIterator   cend() const { return m_printers.cend(); }

    bool            empty() const {return m_printers.empty(); }

    void            reset(bool delete_files) {};

    const std::deque<PhysicalPrinter>& operator()() const { return m_printers; }

    // Load ini files of the particular type from the provided directory path.
    void            load_printers(const std::string& dir_path, const std::string& subdir, PresetsConfigSubstitutions& substitutions, ForwardCompatibilitySubstitutionRule rule);
    void            load_printers_from_presets(PrinterPresetCollection &printer_presets);
    // Load printer from the loaded configuration
    void            load_printer(const std::string& path, const std::string& name, DynamicPrintConfig&& config, bool select, bool save=false);

    // Save the printer under a new name. If the name is different from the old one,
    // a new printer is stored into the list of printers.
    // New printer is activated.
    void            save_printer(PhysicalPrinter& printer, const std::string& renamed_from = "");

    // Delete the current preset, activate the first visible preset.
    // returns true if the preset was deleted successfully.
    bool            delete_printer(const std::string& name);
    // Delete the selected preset
    // returns true if the preset was deleted successfully.
    bool            delete_selected_printer();
    // Delete preset_name preset from all printers:
    // If there is last preset for the printer and first_check== false, then delete this printer
    // returns true if all presets were deleted successfully.
    bool            delete_preset_from_printers(const std::string& preset_name);

    // Get list of printers which have more than one preset and "preset_names" preset is one of them
    std::vector<std::string> get_printers_with_preset( const std::string &preset_name);
    // Get list of printers which has only "preset_names" preset
    std::vector<std::string> get_printers_with_only_preset( const std::string &preset_name);

    // Return the selected preset, without the user modifications applied.
    PhysicalPrinter&        get_selected_printer() { return m_printers[m_idx_selected]; }
    const PhysicalPrinter&  get_selected_printer() const { return m_printers[m_idx_selected]; }

    size_t                  get_selected_idx()    const { return m_idx_selected; }
    // Returns the name of the selected preset, or an empty string if no preset is selected.
    std::string             get_selected_printer_name() const { return (m_idx_selected == size_t(-1)) ? std::string() : this->get_selected_printer().name; }
    // Returns the config of the selected printer, or nullptr if no printer is selected.
    DynamicPrintConfig*     get_selected_printer_config() { return (m_idx_selected == size_t(-1)) ? nullptr : &(this->get_selected_printer().config); }
    // Returns the config of the selected printer, or nullptr if no printer is selected.
    PrinterTechnology       get_selected_printer_technology() { return (m_idx_selected == size_t(-1)) ? PrinterTechnology::ptAny : this->get_selected_printer().printer_technology(); }

    // Each physical printer can have a several related preset,
    // so, use the next functions to get an exact names of selections in the list:
    // Returns the full name of the selected printer, or an empty string if no preset is selected.
    std::string     get_selected_full_printer_name() const;
    // Returns the printer model of the selected preset, or an empty string if no preset is selected.
    std::string     get_selected_printer_preset_name() const { return (m_idx_selected == size_t(-1)) ? std::string() : m_selected_preset; }

    // Select printer by the full printer name, which contains name of printer, separator and name of selected preset
    // If full_name doesn't contain name of selected preset, then select first preset in the list for this printer
    void select_printer(const std::string& full_name);
    void select_printer(const PhysicalPrinter& printer);
    void select_printer(const std::string& printer_name, const std::string& preset_name);
    bool has_selection() const;
    void unselect_printer() ;
    bool is_selected(ConstIterator it, const std::string &preset_name) const;

    // Return a printer by an index. If the printer is active, a temporary copy is returned.
    PhysicalPrinter& printer(size_t idx) { return m_printers[idx]; }
    const PhysicalPrinter& printer(size_t idx) const { return const_cast<PhysicalPrinterCollection*>(this)->printer(idx); }

    // Return a preset by its name. If the preset is active, a temporary copy is returned.
    // If a preset is not found by its name, null is returned.
    // It is possible case (in)sensitive search
    PhysicalPrinter* find_printer(const std::string& name, bool case_sensitive_search = true);
    const PhysicalPrinter* find_printer(const std::string& name, bool case_sensitive_search = true) const
    {
        return const_cast<PhysicalPrinterCollection*>(this)->find_printer(name, case_sensitive_search);
    }

    // Generate a file path from a profile name. Add the ".ini" suffix if it is missing.
    std::string     path_from_name(const std::string& new_name) const;

    const DynamicPrintConfig& default_config() const { return m_default_config; }

private:
    friend class PresetBundle;
    PhysicalPrinterCollection() = default;
    PhysicalPrinterCollection& operator=(const PhysicalPrinterCollection& other) = default;

    // Find a physical printer position in the sorted list of printers.
    // The name of a printer should be unique and case insensitive
    // Use this functions with case_sensitive_search = false, when you need case insensitive search
    std::deque<PhysicalPrinter>::iterator find_printer_internal(const std::string& name, bool case_sensitive_search = true);
    std::deque<PhysicalPrinter>::const_iterator find_printer_internal(const std::string& name, bool case_sensitive_search = true) const
    {
        return const_cast<PhysicalPrinterCollection*>(this)->find_printer_internal(name);
    }

    // List of printers
    // Use deque to force the container to allocate an object per each entry,
    // so that the addresses of the presets don't change during resizing of the container.
    std::deque<PhysicalPrinter> m_printers;

    // Default config for a physical printer containing all key/value pairs of PhysicalPrinter::printer_options().
    DynamicPrintConfig          m_default_config;

    // Selected printer.
    size_t                      m_idx_selected = size_t(-1);
    // The name of the preset which is currently select for this printer
    std::string                 m_selected_preset;

    // Path to the directory to store the config files into.
    std::string                 m_dir_path;
};


} // namespace Slic3r

#endif /* slic3r_Preset_hpp_ */
