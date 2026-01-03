#ifndef slic3r_AppConfig_hpp_
#define slic3r_AppConfig_hpp_

#include <set>
#include <map>
#include <string>
#include "nlohmann/json.hpp"
#include <boost/algorithm/string/trim_all.hpp>

#include "libslic3r/Config.hpp"
#include "libslic3r/Semver.hpp"
#include "calib.hpp"

using namespace nlohmann;

#define ENV_DEV_HOST		"0"
#define ENV_QAT_HOST		"1"
#define ENV_PRE_HOST		"2"
#define ENV_PRODUCT_HOST	"3"

#define SETTING_PROJECT_LOAD_BEHAVIOUR "project_load_behaviour"
#define OPTION_PROJECT_LOAD_BEHAVIOUR_LOAD_ALL "load_all"
#define OPTION_PROJECT_LOAD_BEHAVIOUR_ASK_WHEN_RELEVANT "ask_when_relevant"
#define OPTION_PROJECT_LOAD_BEHAVIOUR_ALWAYS_ASK "always_ask"
#define OPTION_PROJECT_LOAD_BEHAVIOUR_LOAD_GEOMETRY "load_geometry_only"

#define SETTING_NETWORK_PLUGIN_VERSION "network_plugin_version"
#define SETTING_NETWORK_PLUGIN_SKIPPED_VERSIONS "network_plugin_skipped_versions"
#define SETTING_NETWORK_PLUGIN_UPDATE_DISABLED "network_plugin_update_prompts_disabled"
#define SETTING_NETWORK_PLUGIN_REMIND_LATER "network_plugin_remind_later"
#define BAMBU_NETWORK_AGENT_VERSION_LEGACY  "01.10.01.01"

#define SUPPORT_DARK_MODE
//#define _MSW_DARK_MODE


namespace Slic3r {


// Connected LAN mode BambuLab printer
struct BBLocalMachine
{
    std::string dev_name;
    std::string dev_ip;
    std::string dev_id; /* serial number */
    std::string printer_type; /* model_id */

    bool operator==(const BBLocalMachine& other) const
    {
        return dev_name == other.dev_name && dev_ip == other.dev_ip && dev_id == other.dev_id && printer_type == other.printer_type;
    }
    bool operator!=(const BBLocalMachine& other) const { return !operator==(other); }
};

class AppConfig
{
public:
	enum class EAppMode : unsigned char
	{
		Editor,
		GCodeViewer
	};

    //BBS: remove GCodeViewer as seperate APP logic
	explicit AppConfig() :
		m_dirty(false),
		m_orig_version(Semver::invalid()),
		m_mode(EAppMode::Editor),
		m_legacy_datadir(false)
	{
		this->reset();
	}

	std::string get_language_code();
	std::string get_hms_host();
	bool get_stealth_mode();

	// Clear and reset to defaults.
	void 			   	reset();
	// Override missing or keys with their defaults.
	void 			   	set_defaults();

	// Load the slic3r.ini from a user profile directory (or a datadir, if configured).
	// return error string or empty strinf
	std::string         load();
	// Store the slic3r.ini into a user profile directory (or a datadir, if configured).
	void 			   	save();

	// Does this config need to be saved?
	bool 				dirty() const { return m_dirty; }


	void				set_dirty() { m_dirty = true; }

	// Const accessor, it will return false if a section or a key does not exist.
	bool get(const std::string &section, const std::string &key, std::string &value) const
	{
		value.clear();
		auto it = m_storage.find(section);
		if (it == m_storage.end())
			return false;
		auto it2 = it->second.find(key);
		if (it2 == it->second.end())
			return false;
		value = it2->second;
		return true;
	}
	std::string 		get(const std::string &section, const std::string &key) const
		{ std::string value; this->get(section, key, value); return value; }
	std::string 		get(const std::string &key) const
		{ std::string value; this->get("app", key, value); return value; }
	bool				get_bool(const std::string &section, const std::string &key) const
		{ return this->get(section, key) == "true" || this->get(key) == "1"; }
	bool				get_bool(const std::string &key) const
		{ return this->get_bool("app", key); }
	void			    set(const std::string &section, const std::string &key, const std::string &value)
	{
#ifndef NDEBUG
		{
			std::string key_trimmed = key;
			boost::trim_all(key_trimmed);
			assert(key_trimmed == key);
			assert(! key_trimmed.empty());
		}
#endif // NDEBUG
		std::string &old = m_storage[section][key];
		if (old != value) {
			old = value;
			m_dirty = true;
		}
	}

	void			    set_str(const std::string& section, const std::string& key, const std::string& value)
	{
#ifndef NDEBUG
		{
			std::string key_trimmed = key;
			boost::trim_all(key_trimmed);
			assert(key_trimmed == key);
			assert(!key_trimmed.empty());
		}
#endif // NDEBUG
		std::string& old = m_storage[section][key];
		if (old != value) {
			old = value;
			m_dirty = true;
		}
	}

	void				set(const std::string& section, const std::string &key, bool value)
	{
		if (value){
			set(section, key, std::string("true"));
		} else {
			set(section, key, std::string("false"));
		}
	}


	void			    set(const std::string &key, const std::string &value)
		{ this->set("app", key, value);  }

	void                set_bool(const std::string &key, const bool &value)
		{
			this->set("app", key, value);
		}

	bool				has(const std::string &section, const std::string &key) const
	{
		auto it = m_storage.find(section);
		if (it == m_storage.end())
			return false;
		auto it2 = it->second.find(key);
		return it2 != it->second.end() && ! it2->second.empty();
	}
	bool				has(const std::string &key) const
		{ return this->has("app", key); }

	void				erase(const std::string &section, const std::string &key)
	{
		auto it = m_storage.find(section);
		if (it != m_storage.end()) {
            it->second.erase(key);
            m_dirty = true;
		}
	}

	bool                has_section(const std::string &section) const
		{ return m_storage.find(section) != m_storage.end(); }
	const std::map<std::string, std::string>& get_section(const std::string &section) const
		{ return m_storage.find(section)->second; }
	void set_section(const std::string &section, const std::map<std::string, std::string>& data)
		{ m_storage[section] = data; }
	void 				clear_section(const std::string &section)
		{ m_storage[section].clear(); }

	typedef std::map<std::string, std::map<std::string, std::set<std::string>>> VendorMap;
	bool                get_variant(const std::string &vendor, const std::string &model, const std::string &variant) const;
	void                set_variant(const std::string &vendor, const std::string &model, const std::string &variant, bool enable);
	void                set_vendors(const AppConfig &from);
	void 				set_vendors(const VendorMap &vendors) { m_vendors = vendors; m_dirty = true; }
	void 				set_vendors(VendorMap &&vendors) { m_vendors = std::move(vendors); m_dirty = true; }
	const VendorMap&    vendors() const { return m_vendors; }

	// Orca printer settings
    typedef std::map<std::string, nlohmann::json> MachineSettingMap;
    bool has_printer_settings(std::string printer) const {
        return m_printer_settings.find(printer) != m_printer_settings.end();
    }
    void clear_printer_settings(std::string printer) {
        m_printer_settings.erase(printer);
        m_dirty = true;
    }
    bool has_printer_setting(std::string printer, std::string name) {
        if (!has_printer_settings(printer))
            return false;
        if (!m_printer_settings[printer].contains(name))
            return false;
        return true;
    }
    std::string get_printer_setting(std::string printer, std::string name) {
        if (!has_printer_setting(printer, name))
            return "";
        return m_printer_settings[printer][name];
    }
    void set_printer_setting(std::string printer, std::string name, std::string value) {
        m_printer_settings[printer][name] = value;
        m_dirty = true;
    }

	const std::map<std::string, BBLocalMachine>& get_local_machines() const { return m_local_machines; }
	void erase_local_machine(std::string dev_id)
    {
        auto it = m_local_machines.find(dev_id);
        if (it != m_local_machines.end()) {
            m_local_machines.erase(it);
            m_dirty = true;
        }
    }
    void update_local_machine(const BBLocalMachine& machine)
    {
        auto it = m_local_machines.find(machine.dev_id);
        if (it != m_local_machines.end()) {
            const auto& current = it->second;
            if (machine != current) {
                m_local_machines[machine.dev_id] = machine;
                m_dirty = true;
            }
        } else {
            m_local_machines[machine.dev_id] = machine;
            m_dirty = true;
        }
    }

    const std::vector<std::string> &get_filament_presets() const { return m_filament_presets; }
    void set_filament_presets(const std::vector<std::string> &filament_presets){
        m_filament_presets = filament_presets;
        m_dirty            = true;
    }
    const std::vector<std::string> &get_filament_colors() const { return m_filament_colors; }
    void set_filament_colors(const std::vector<std::string> &filament_colors){
        m_filament_colors = filament_colors;
        m_dirty                = true;
    }

	const std::vector<PrinterCaliInfo> &get_printer_cali_infos() const { return m_printer_cali_infos; }
    void save_printer_cali_infos(const PrinterCaliInfo& cali_info, bool need_change_status = true);

	// return recent/last_opened_folder or recent/settings_folder or empty string.
	std::string 		get_last_dir() const;
	void 				update_config_dir(const std::string &dir);
	void 				update_skein_dir(const std::string &dir);

	//std::string 		get_last_output_dir(const std::string &alt) const;
	//void                update_last_output_dir(const std::string &dir);
	std::string 		get_last_output_dir(const std::string& alt, const bool removable = false) const;
	void                update_last_output_dir(const std::string &dir, const bool removable = false);

	// BBS: backup & restore
	std::string 		get_last_backup_dir() const;
	void                update_last_backup_dir(const std::string &dir);

	std::string         get_region();
	std::string         get_country_code();
    bool				is_engineering_region();

    void                save_custom_color_to_config(const std::vector<std::string> &colors);
    std::vector<std::string> get_custom_color_from_config();

    void save_nozzle_volume_types_to_config(const std::string& printer_name, const std::string& nozzle_volume_types);
    std::string get_nozzle_volume_types_from_config(const std::string& printer_name);

	// reset the current print / filament / printer selections, so that
	// the  PresetBundle::load_selections(const AppConfig &config) call will select
	// the first non-default preset when called.
    void                reset_selections();

	// Get the default config path from Slic3r::data_dir().
	std::string			config_path();

	// Returns true if the user's data directory comes from before Slic3r 1.40.0 (no updating)
	bool 				legacy_datadir() const { return m_legacy_datadir; }
	void 				set_legacy_datadir(bool value) { m_legacy_datadir = value; }

	// Get the Slic3r version check url.
	// This returns a hardcoded string unless it is overriden by "version_check_url" in the ini file.
	std::string 		version_check_url() const;

	// Get the Orca profile update url.
	std::string 		profile_update_url() const;

	// Returns the original Slic3r version found in the ini file before it was overwritten
	// by the current version
	Semver 				orig_version() const { return m_orig_version; }

	// Does the config file exist?
	bool 				exists();

	void                set_loading_path(const std::string& path) { m_loading_path = path; }
	std::string         loading_path() { return (m_loading_path.empty() ? config_path() : m_loading_path); }

    std::vector<std::string> get_recent_projects() const;
    void set_recent_projects(const std::vector<std::string>& recent_projects);

	void set_mouse_device(const std::string& name, double translation_speed, double translation_deadzone, float rotation_speed, float rotation_deadzone, double zoom_speed, bool swap_yz, bool invert_x, bool invert_y, bool invert_z, bool invert_yaw, bool invert_pitch, bool invert_roll);
	std::vector<std::string> get_mouse_device_names() const;
	bool get_mouse_device_translation_speed(const std::string& name, double& speed) const
		{ return get_3dmouse_device_numeric_value(name, "translation_speed", speed); }
    bool get_mouse_device_translation_deadzone(const std::string& name, double& deadzone) const
		{ return get_3dmouse_device_numeric_value(name, "translation_deadzone", deadzone); }
    bool get_mouse_device_rotation_speed(const std::string& name, float& speed) const
		{ return get_3dmouse_device_numeric_value(name, "rotation_speed", speed); }
    bool get_mouse_device_rotation_deadzone(const std::string& name, float& deadzone) const
		{ return get_3dmouse_device_numeric_value(name, "rotation_deadzone", deadzone); }
	bool get_mouse_device_zoom_speed(const std::string& name, double& speed) const
		{ return get_3dmouse_device_numeric_value(name, "zoom_speed", speed); }
	bool get_mouse_device_swap_yz(const std::string& name, bool& swap) const
		{ return get_3dmouse_device_numeric_value(name, "swap_yz", swap); }
	bool get_mouse_device_invert_x(const std::string& name, bool& invert) const
		{ return get_3dmouse_device_numeric_value(name, "invert_x", invert); }
	bool get_mouse_device_invert_y(const std::string& name, bool& invert) const
		{ return get_3dmouse_device_numeric_value(name, "invert_y", invert); }
	bool get_mouse_device_invert_z(const std::string& name, bool& invert) const
		{ return get_3dmouse_device_numeric_value(name, "invert_z", invert); }
	bool get_mouse_device_invert_yaw(const std::string& name, bool& invert) const
		{ return get_3dmouse_device_numeric_value(name, "invert_yaw", invert); }
	bool get_mouse_device_invert_pitch(const std::string& name, bool& invert) const
		{ return get_3dmouse_device_numeric_value(name, "invert_pitch", invert); }
	bool get_mouse_device_invert_roll(const std::string& name, bool& invert) const
		{ return get_3dmouse_device_numeric_value(name, "invert_roll", invert); }

	static const std::string SECTION_FILAMENTS;
    static const std::string SECTION_MATERIALS;
    static const std::string SECTION_EMBOSS_STYLE;

    std::string get_network_plugin_version() const;
    void set_network_plugin_version(const std::string& version);

    std::vector<std::string> get_skipped_network_versions() const;
    void add_skipped_network_version(const std::string& version);
    bool is_network_version_skipped(const std::string& version) const;
    void clear_skipped_network_versions();

    bool is_network_update_prompt_disabled() const;
    void set_network_update_prompt_disabled(bool disabled);

    bool should_remind_network_update_later() const;
    void set_remind_network_update_later(bool remind);
    void clear_remind_network_update_later();

private:
	template<typename T>
	bool get_3dmouse_device_numeric_value(const std::string &device_name, const char *parameter_name, T &out) const
	{
	    std::string key = std::string("mouse_device:") + device_name;
	    auto it = m_storage.find(key);
	    if (it == m_storage.end())
	        return false;
	    auto it_val = it->second.find(parameter_name);
	    if (it_val == it->second.end())
	        return false;
        out = T(string_to_double_decimal_point(it_val->second));
	    return true;
	}

	// Type of application: Editor or GCodeViewer
	EAppMode													m_mode { EAppMode::Editor };
	// Map of section, name -> value
	std::map<std::string, std::map<std::string, std::string>> 	m_storage;

	// Map of enabled vendors / models / variants
	VendorMap                                                   m_vendors;

	// Preset for each machine
	MachineSettingMap											m_printer_settings;
	// Has any value been modified since the config.ini has been last saved or loaded?
	bool														m_dirty;
	// Original version found in the ini file before it was overwritten
	Semver                                                      m_orig_version;
	// Whether the existing version is before system profiles & configuration updating
	bool                                                        m_legacy_datadir;

	std::string                                                 m_loading_path;

	std::vector<std::string>									m_filament_presets;
    std::vector<std::string>									m_filament_colors;
	std::vector<std::string>									m_filament_multi_colors;
	std::vector<std::string>									m_filament_color_types;

	std::vector<PrinterCaliInfo>								m_printer_cali_infos;

	std::map<std::string, BBLocalMachine>						m_local_machines;
};

} // namespace Slic3r

#endif /* slic3r_AppConfig_hpp_ */
