#include "Snapshot.hpp"

#include <time.h>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree_fwd.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/format.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/Time.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/FileParserError.hpp"
#include "libslic3r/Utils.hpp"
//BBS
#include "libslic3r/Preset.hpp"

#include "../GUI/GUI.hpp"
#include "../GUI/GUI_App.hpp"
#include "../GUI/I18N.hpp"
#include "../GUI/MainFrame.hpp"
#include "../GUI/MsgDialog.hpp"

#include <wx/richmsgdlg.h>

#define SLIC3R_SNAPSHOTS_DIR "snapshots"
#define SLIC3R_SNAPSHOT_FILE "snapshot.ini"

namespace Slic3r {
namespace GUI {
namespace Config {

void Snapshot::clear()
{
	this->id.clear();
	this->time_captured = 0;
	this->slic3r_version_captured = Semver::invalid();
	this->comment.clear();
	this->reason = SNAPSHOT_UNKNOWN;
	this->print.clear();
    this->sla_print.clear();
	this->filaments.clear();
    this->sla_material.clear();
	this->printer.clear();
    this->physical_printer.clear();
}

void Snapshot::load_ini(const std::string &path)
{
	this->clear();

	auto throw_on_parse_error = [&path](const std::string &msg) {
		throw file_parser_error(std::string("Failed loading the snapshot file. Reason: ") + msg, path);
	};

	// Load the snapshot.ini file.
    boost::property_tree::ptree tree;
    try {
        boost::nowide::ifstream ifs(path);
        boost::property_tree::read_ini(ifs, tree);
    } catch (const std::ifstream::failure &err) {
        throw file_parser_error(std::string("The snapshot file cannot be loaded. Reason: ") + err.what(), path);
    } catch (const std::runtime_error &err) {
        throw_on_parse_error(err.what());
    }

    // Parse snapshot.ini
    std::string group_name_vendor = "Vendor:";
	std::string key_filament = "filament";
    std::string key_prefix_model = "model_";
    for (auto &section : tree) {
    	if (section.first == "snapshot") {
    		// Parse the common section.
            for (auto &kvp : section.second) {
                if (kvp.first == "id")
                    this->id = kvp.second.data();
                else if (kvp.first == "time_captured") {
                	this->time_captured = Slic3r::Utils::parse_iso_utc_timestamp(kvp.second.data());
					if (this->time_captured == (time_t)-1)
				        throw_on_parse_error("invalid timestamp");
                } else if (kvp.first == "slic3r_version_captured") {
                	auto semver = Semver::parse(kvp.second.data());
                	if (! semver)
                		throw_on_parse_error("invalid slic3r_version_captured semver");
                	this->slic3r_version_captured = *semver;
                } else if (kvp.first == "comment") {
                	this->comment = kvp.second.data();
                } else if (kvp.first == "reason") {
                	std::string rsn = kvp.second.data();
                	if (rsn == "upgrade")
                		this->reason = SNAPSHOT_UPGRADE;
                	else if (rsn == "downgrade")
                		this->reason = SNAPSHOT_DOWNGRADE;
                    else if (rsn == "before_rollback")
                        this->reason = SNAPSHOT_BEFORE_ROLLBACK;
                	else if (rsn == "user")
                		this->reason = SNAPSHOT_USER;
                	else
                		this->reason = SNAPSHOT_UNKNOWN;
                }
            }
    	} else if (section.first == "presets") {
            // Load the names of the active presets.
            for (auto &kvp : section.second) {
                if (kvp.first == PRESET_PRINT_NAME) {
                    this->print = kvp.second.data();
                } else if (kvp.first == "sla_print") {
                    this->sla_print = kvp.second.data();
                } else if (boost::starts_with(kvp.first, PRESET_FILAMENT_NAME)) {
                    int idx = 0;
                    if (kvp.first == "filament" || sscanf(kvp.first.c_str(), "filament_%d", &idx) == 1) {
                        if (int(this->filaments.size()) <= idx)
                            this->filaments.resize(idx + 1, std::string());
                        this->filaments[idx] = kvp.second.data();
                    }
                } else if (kvp.first == "sla_material") {
                    this->sla_material = kvp.second.data();
                } else if (kvp.first == PRESET_PRINTER_NAME) {
                    this->printer = kvp.second.data();
                } else if (kvp.first == "physical_printer") {
                    this->physical_printer = kvp.second.data();
                }
            }
    	} else if (boost::starts_with(section.first, group_name_vendor) && section.first.size() > group_name_vendor.size()) {
    		// Vendor specific section.
            VendorConfig vc;
            vc.name = section.first.substr(group_name_vendor.size());
            for (auto &kvp : section.second) {
            	if (kvp.first == "version" || kvp.first == "min_slic3r_version" || kvp.first == "max_slic3r_version") {
            		// Version of the vendor specific config bundle bundled with this snapshot.
                	auto semver = Semver::parse(kvp.second.data());
                	if (! semver)
                		throw_on_parse_error("invalid " + kvp.first + " format for " + section.first);
					if (kvp.first == "version")
                		vc.version.config_version = *semver;
                	else if (kvp.first == "min_slic3r_version")
                		vc.version.min_slic3r_version = *semver;
                	else
                		vc.version.max_slic3r_version = *semver;
            	} else if (boost::starts_with(kvp.first, key_prefix_model) && kvp.first.size() > key_prefix_model.size()) {
                    // Parse the printer variants installed for the current model.
                    auto &set_variants = vc.models_variants_installed[kvp.first.substr(key_prefix_model.size())];
                    std::vector<std::string> variants;
                    if (unescape_strings_cstyle(kvp.second.data(), variants))
                        for (auto &variant : variants)
                            set_variants.insert(std::move(variant));
                }
			}
			this->vendor_configs.emplace_back(std::move(vc));
        }
    }
    // Sort the vendors lexicographically.
    std::sort(this->vendor_configs.begin(), this->vendor_configs.begin(),
        [](const VendorConfig &cfg1, const VendorConfig &cfg2) { return cfg1.name < cfg2.name; });
}

static std::string reason_string(const Snapshot::Reason reason)
{
    switch (reason) {
    case Snapshot::SNAPSHOT_UPGRADE:
        return "upgrade";
    case Snapshot::SNAPSHOT_DOWNGRADE:
        return "downgrade";
    case Snapshot::SNAPSHOT_BEFORE_ROLLBACK:
        return "before_rollback";
    case Snapshot::SNAPSHOT_USER:
        return "user";
    case Snapshot::SNAPSHOT_UNKNOWN:
    default:
        return "unknown";
    }
}

void Snapshot::save_ini(const std::string &path)
{
	boost::nowide::ofstream c;
    c.open(path, std::ios::out | std::ios::trunc);
    c << "# " << Slic3r::header_slic3r_generated() << std::endl;

    // Export the common "snapshot".
	c << std::endl << "[snapshot]" << std::endl;
	c << "id = " << this->id << std::endl;
	c << "time_captured = " << Slic3r::Utils::iso_utc_timestamp(this->time_captured) << std::endl;
	c << "slic3r_version_captured = " << this->slic3r_version_captured.to_string() << std::endl;
	c << "comment = " << this->comment << std::endl;
	c << "reason = " << reason_string(this->reason) << std::endl;

    // Export the active presets at the time of the snapshot.
	c << std::endl << "[presets]" << std::endl;
	c << "print = " << this->print << std::endl;
    c << "sla_print = " << this->sla_print << std::endl;
	c << "filament = " << this->filaments.front() << std::endl;
	for (size_t i = 1; i < this->filaments.size(); ++ i)
		c << "filament_" << std::to_string(i) << " = " << this->filaments[i] << std::endl;
    c << "sla_material = " << this->sla_material << std::endl;
	c << "printer = " << this->printer << std::endl;
    c << "physical_printer = " << this->physical_printer << std::endl;

    // Export the vendor configs.
    for (const VendorConfig &vc : this->vendor_configs) {
		c << std::endl << "[Vendor:" << vc.name << "]" << std::endl;
		c << "version = " << vc.version.config_version.to_string() << std::endl;
		c << "min_slic3r_version = " << vc.version.min_slic3r_version.to_string() << std::endl;
		c << "max_slic3r_version = " << vc.version.max_slic3r_version.to_string() << std::endl;
        // Export installed printer models and their variants.
        for (const auto &model : vc.models_variants_installed) {
            if (model.second.size() == 0)
                continue;
            const std::vector<std::string> variants(model.second.begin(), model.second.end());
            const auto escaped = escape_strings_cstyle(variants);
            c << "model_" << model.first << " = " << escaped << std::endl;
        }
    }
    c.close();
}

void Snapshot::export_selections(AppConfig &config) const
{
    assert(filaments.size() >= 1);
    config.clear_section("presets");
    config.set("presets", PRESET_PRINT_NAME,     print);
    config.set("presets", "sla_print", sla_print);
    config.set("presets", PRESET_FILAMENT_NAME,  filaments.front());
    for (unsigned i = 1; i < filaments.size(); ++i) {
        char name[64];
        sprintf(name, "filament_%u", i);
        config.set("presets", name, filaments[i]);
    }
    config.set("presets", "sla_material",     sla_material);
    config.set("presets", PRESET_PRINTER_NAME,          printer);
    config.set("presets", "physical_printer", physical_printer);
}

void Snapshot::export_vendor_configs(AppConfig &config) const
{
    std::map<std::string, std::map<std::string, std::set<std::string>>> vendors;
    for (const VendorConfig &vc : vendor_configs)
        vendors[vc.name] = vc.models_variants_installed;
    config.set_vendors(std::move(vendors));
}

//BBS: change directories by desigh
static constexpr auto snapshot_subdirs = { PRESET_PRINT_NAME, PRESET_SLA_PRINT_NAME, PRESET_FILAMENT_NAME, PRESET_SLA_MATERIALS_NAME, PRESET_PRINTER_NAME, "physical_printer", PRESET_SYSTEM_DIR };
//static constexpr auto snapshot_subdirs = { "print", "sla_print", "filament", "sla_material", "printer", "physical_printer", "vendor" };

// Perform a deep compare of the active print / sla_print / filament / sla_material / printer / physical_printer / vendor directories.
// Return true if the content of the current print / sla_print / filament / sla_material / printer / physical_printer / vendor directories
// matches the state stored in this snapshot.
bool Snapshot::equal_to_active(const AppConfig &app_config) const
{
    // 1) Check, whether this snapshot contains the same set of active vendors, printer models and variants
    // as app_config.
    {
        std::set<std::string> matched;
        for (const VendorConfig &vc : this->vendor_configs) {
            auto it_vendor_models_variants = app_config.vendors().find(vc.name);
            if (it_vendor_models_variants == app_config.vendors().end() ||
                it_vendor_models_variants->second != vc.models_variants_installed)
                // There are more vendors enabled in the snapshot than currently installed.
                return false;
            matched.insert(vc.name);
        }
        for (const auto &v : app_config.vendors())
            if (matched.find(v.first) == matched.end() && ! v.second.empty())
                // There are more vendors currently installed than enabled in the snapshot.
                return false;
    }

    // 2) Check, whether this snapshot references the same set of ini files as the current state.
    boost::filesystem::path data_dir     = boost::filesystem::path(Slic3r::data_dir());
    boost::filesystem::path snapshot_dir = boost::filesystem::path(Slic3r::data_dir()) / SLIC3R_SNAPSHOTS_DIR / this->id;
    for (const char *subdir : snapshot_subdirs) {
        boost::filesystem::path path1 = data_dir / subdir;
        boost::filesystem::path path2 = snapshot_dir / subdir;
        std::vector<std::string> files1, files2;
        if (boost::filesystem::is_directory(path1))
            for (auto &dir_entry : boost::filesystem::directory_iterator(path1))
                if (Slic3r::is_ini_file(dir_entry))
                    files1.emplace_back(dir_entry.path().filename().string());
        if (boost::filesystem::is_directory(path2))
            for (auto &dir_entry : boost::filesystem::directory_iterator(path2))
                if (Slic3r::is_ini_file(dir_entry))
                    files2.emplace_back(dir_entry.path().filename().string());
        std::sort(files1.begin(), files1.end());
        std::sort(files2.begin(), files2.end());
        if (files1 != files2)
            return false;
        for (const std::string &filename : files1) {
            FILE *f1 = boost::nowide::fopen((path1 / filename).string().c_str(), "rb");
            FILE *f2 = boost::nowide::fopen((path2 / filename).string().c_str(), "rb");
            bool same = true;
            if (f1 && f2) {
                char buf1[4096];
                char buf2[4096];
                do {
                    size_t r1 = fread(buf1, 1, 4096, f1);
                    size_t r2 = fread(buf2, 1, 4096, f2);
                    if (r1 != r2 || memcmp(buf1, buf2, r1)) {
                        same = false;
                        break;
                    }
                } while (! feof(f1) || ! feof(f2));
            } else
                same = false;
            if (f1)
                fclose(f1);
            if (f2)
                fclose(f2);
            if (! same)
                return false;
        }
    }
    return true;
}

size_t SnapshotDB::load_db()
{
	boost::filesystem::path snapshots_dir = SnapshotDB::create_db_dir();

	m_snapshots.clear();

    // Walk over the snapshot directories and load their index.
    std::string errors_cummulative;
	for (auto &dir_entry : boost::filesystem::directory_iterator(snapshots_dir))
        if (boost::filesystem::is_directory(dir_entry.status())) {
        	// Try to read "snapshot.ini".
            boost::filesystem::path path_ini = dir_entry.path() / SLIC3R_SNAPSHOT_FILE;
            Snapshot 			    snapshot;
            try {
            	snapshot.load_ini(path_ini.string());
            } catch (const std::runtime_error &err) {
                errors_cummulative += err.what();
                errors_cummulative += "\n";
                continue;
			}
            // Check that the name of the snapshot directory matches the snapshot id stored in the snapshot.ini file.
            if (dir_entry.path().filename().string() != snapshot.id) {
            	errors_cummulative += std::string("Snapshot ID ") + snapshot.id + " does not match the snapshot directory " + dir_entry.path().filename().string() + "\n";
            	continue;
            }
            m_snapshots.emplace_back(std::move(snapshot));
        }
    // Sort the snapshots by their date/time.
    std::sort(m_snapshots.begin(), m_snapshots.end(), [](const Snapshot &s1, const Snapshot &s2) { return s1.time_captured < s2.time_captured; });
    if (! errors_cummulative.empty())
        throw Slic3r::RuntimeError(errors_cummulative);
    return m_snapshots.size();
}

void SnapshotDB::update_slic3r_versions(std::vector<Index> &index_db)
{
	for (Snapshot &snapshot : m_snapshots) {
		for (Snapshot::VendorConfig &vendor_config : snapshot.vendor_configs) {
			auto it = std::find_if(index_db.begin(), index_db.end(), [&vendor_config](const Index &idx) { return idx.vendor() == vendor_config.name; });
			if (it != index_db.end()) {
				Index::const_iterator it_version = it->find(vendor_config.version.config_version);
				if (it_version != it->end()) {
					vendor_config.version.min_slic3r_version = it_version->min_slic3r_version;
					vendor_config.version.max_slic3r_version = it_version->max_slic3r_version;
				}
			}
		}
	}
}

static void copy_config_dir_single_level(const boost::filesystem::path &path_src, const boost::filesystem::path &path_dst)
{
//BBS: remove snapshots function currently
#if 0
    if (! boost::filesystem::is_directory(path_dst) &&
        ! boost::filesystem::create_directory(path_dst))
        throw Slic3r::RuntimeError(std::string("OrcaSlicer was unable to create a directory at ") + path_dst.string());

    for (auto &dir_entry : boost::filesystem::directory_iterator(path_src))
        if (Slic3r::is_ini_file(dir_entry))
            if (std::string error_message; copy_file(dir_entry.path().string(), (path_dst / dir_entry.path().filename()).string(), error_message, false) != SUCCESS)
                throw Slic3r::RuntimeError(format("Failed copying \"%1%\" to \"%2%\": %3%", path_src.string(), path_dst.string(), error_message));
#endif
}

static void delete_existing_ini_files(const boost::filesystem::path &path)
{
//BBS: remove snapshots function currently
#if 0
    if (! boost::filesystem::is_directory(path))
    	return;
    for (auto &dir_entry : boost::filesystem::directory_iterator(path))
        if (boost::filesystem::is_regular_file(dir_entry.status()) && boost::algorithm::iends_with(dir_entry.path().filename().string(), ".ini"))
		    boost::filesystem::remove(dir_entry.path());
#endif
}

const Snapshot&	SnapshotDB::take_snapshot(const AppConfig &app_config, Snapshot::Reason reason, const std::string &comment)
{
	boost::filesystem::path data_dir        = boost::filesystem::path(Slic3r::data_dir());
	boost::filesystem::path snapshot_db_dir = SnapshotDB::create_db_dir();

	// 1) Prepare the snapshot structure.
	Snapshot snapshot;
	// Snapshot header.
	snapshot.time_captured 			 = Slic3r::Utils::get_current_time_utc();
	snapshot.id 					 = Slic3r::Utils::iso_utc_timestamp(snapshot.time_captured);
	snapshot.slic3r_version_captured = Slic3r::SEMVER;
	snapshot.comment 				 = comment;
	snapshot.reason 				 = reason;
	// Active presets at the time of the snapshot.
    snapshot.print                   = app_config.get("presets", PRESET_PRINT_NAME);
    snapshot.sla_print               = app_config.get("presets", "sla_print");
    snapshot.filaments.emplace_back(app_config.get("presets", PRESET_FILAMENT_NAME));
    snapshot.sla_material            = app_config.get("presets", "sla_material");
    snapshot.printer                 = app_config.get("presets", PRESET_PRINTER_NAME);
    snapshot.physical_printer        = app_config.get("presets", "physical_printer");
    for (unsigned i = 1; i < 1000; ++ i) {
        char name[64];
        sprintf(name, "filament_%u", i);
        if (! app_config.has("presets", name))
            break;
        snapshot.filaments.emplace_back(app_config.get("presets", name));
    }
    // Vendor specific config bundles and installed printers.
    for (const auto &vendor : app_config.vendors()) {
        Snapshot::VendorConfig cfg;
        cfg.name = vendor.first;
        cfg.models_variants_installed = vendor.second;
        for (auto it = cfg.models_variants_installed.begin(); it != cfg.models_variants_installed.end();)
            if (it->second.empty())
                cfg.models_variants_installed.erase(it ++);
            else
                ++ it;
        // Read the active config bundle, parse the config version.
        PresetBundle bundle;
        //BBS: change directoties by design
        //bundle.load_configbundle((data_dir / PRESET_SYSTEM_DIR / (cfg.name + ".ini")).string(), PresetBundle::LoadConfigBundleAttribute::LoadVendorOnly, ForwardCompatibilitySubstitutionRule::EnableSilent);
        bundle.load_vendor_configs_from_json((data_dir/PRESET_SYSTEM_DIR).string(), cfg.name, PresetBundle::LoadConfigBundleAttribute::LoadVendorOnly, ForwardCompatibilitySubstitutionRule::EnableSilent);
        for (const auto &vp : bundle.vendors)
            if (vp.second.id == cfg.name)
                cfg.version.config_version = vp.second.config_version;
        snapshot.vendor_configs.emplace_back(std::move(cfg));
    }

	boost::filesystem::path snapshot_dir = snapshot_db_dir / snapshot.id;

    try {
	    boost::filesystem::create_directory(snapshot_dir);

        // Backup the presets.
        for (const char *subdir : snapshot_subdirs)
    	    copy_config_dir_single_level(data_dir / subdir, snapshot_dir / subdir);
        snapshot.save_ini((snapshot_dir / "snapshot.ini").string());
        assert(m_snapshots.empty() || m_snapshots.back().time_captured <= snapshot.time_captured);
        m_snapshots.emplace_back(std::move(snapshot));
    } catch (...) {
        if (boost::filesystem::is_directory(snapshot_dir)) {
            try {
                // Clean up partially copied snapshot.
                boost::filesystem::remove_all(snapshot_dir);
            } catch (...) {
                BOOST_LOG_TRIVIAL(error) << "Failed taking snapshot and failed removing the snapshot directory " << snapshot_dir;
            }
        }
        throw;
    }
    return m_snapshots.back();
}

const Snapshot& SnapshotDB::restore_snapshot(const std::string &id, AppConfig &app_config)
{
	for (const Snapshot &snapshot : m_snapshots)
		if (snapshot.id == id) {
			this->restore_snapshot(snapshot, app_config);
			return snapshot;
		}
	throw Slic3r::RuntimeError(std::string("Snapshot with id " + id + " was not found."));
}

void SnapshotDB::restore_snapshot(const Snapshot &snapshot, AppConfig &app_config)
{
	boost::filesystem::path data_dir        = boost::filesystem::path(Slic3r::data_dir());
	boost::filesystem::path snapshot_db_dir = SnapshotDB::create_db_dir();
    boost::filesystem::path snapshot_dir 	= snapshot_db_dir / snapshot.id;
    // Remove existing ini files and restore the ini files from the snapshot.
    for (const char *subdir : snapshot_subdirs) {
        boost::filesystem::path src = snapshot_dir / subdir;
        boost::filesystem::path dst = data_dir / subdir;
		delete_existing_ini_files(dst);
        if (boost::filesystem::is_directory(src))
    	    copy_config_dir_single_level(src, dst);
    }
    // Update AppConfig with the selections of the print / sla_print / filament / sla_material / printer profiles
    // and about the installed printer types and variants.
    snapshot.export_selections(app_config);
    snapshot.export_vendor_configs(app_config);
}

bool SnapshotDB::is_on_snapshot(AppConfig &app_config) const
{
    // Is the "on_snapshot" configuration value set?
    std::string on_snapshot = app_config.get("on_snapshot");
    if (on_snapshot.empty())
        // No, we are not on a snapshot.
        return false;
    // Is the "on_snapshot" equal to the current configuration state?
    auto it_snapshot = this->snapshot(on_snapshot);
    if (it_snapshot != this->end() && it_snapshot->equal_to_active(app_config))
        // Yes, we are on the snapshot.
        return true;
    // No, we are no more on a snapshot. Reset the state.
    app_config.set("on_snapshot", "");
    return false;
}

SnapshotDB::const_iterator SnapshotDB::snapshot_with_vendor_preset(const std::string &vendor_name, const Semver &config_version)
{
    auto it_found = m_snapshots.end();
    Snapshot::VendorConfig key;
    key.name = vendor_name;
    for (auto it = m_snapshots.begin(); it != m_snapshots.end(); ++ it) {
        const Snapshot &snapshot = *it;
        auto it_vendor_config = std::lower_bound(snapshot.vendor_configs.begin(), snapshot.vendor_configs.end(),
            key, [](const Snapshot::VendorConfig &cfg1, const Snapshot::VendorConfig &cfg2) { return cfg1.name < cfg2.name; });
        if (it_vendor_config != snapshot.vendor_configs.end() && it_vendor_config->name == vendor_name &&
            config_version == it_vendor_config->version.config_version) {
            // Vendor config found with the correct version.
            // Save it, but continue searching, as we want the newest snapshot.
            it_found = it;
        }
    }
    return it_found;
}

SnapshotDB::const_iterator SnapshotDB::snapshot(const std::string &id) const
{
    for (const_iterator it = m_snapshots.begin(); it != m_snapshots.end(); ++ it)
        if (it->id == id)
            return it;
    return m_snapshots.end();
}

boost::filesystem::path SnapshotDB::create_db_dir()
{
    boost::filesystem::path data_dir 	  = boost::filesystem::path(Slic3r::data_dir());
    boost::filesystem::path snapshots_dir = data_dir / SLIC3R_SNAPSHOTS_DIR;
    for (const boost::filesystem::path &path : { data_dir, snapshots_dir }) {
		boost::filesystem::path subdir = path;
        subdir.make_preferred();
        if (! boost::filesystem::is_directory(subdir) &&
            ! boost::filesystem::create_directory(subdir))
            throw Slic3r::RuntimeError(std::string("Slic3r was unable to create a directory at ") + subdir.string());
    }
    return snapshots_dir;
}

SnapshotDB& SnapshotDB::singleton()
{
	static SnapshotDB instance;
	static bool       loaded = false;
	if (! loaded) {
		try {
			loaded = true;
			// Load the snapshot database.
			instance.load_db();
			// Load the vendor specific configuration indices.
			std::vector<Index> index_db = Index::load_db();
			// Update the min / max slic3r versions compatible with the configurations stored inside the snapshots
			// based on the min / max slic3r versions defined by the vendor specific config indices.
			instance.update_slic3r_versions(index_db);
		} catch (std::exception & /* ex */) {
		}
	}
	return instance;
}

const Snapshot* take_config_snapshot_report_error(const AppConfig &app_config, Snapshot::Reason reason, const std::string &comment)
{
    try {
        return &SnapshotDB::singleton().take_snapshot(app_config, reason, comment);
    } catch (std::exception &err) {
        show_error(static_cast<wxWindow*>(wxGetApp().mainframe),
            _L("Taking a configuration snapshot failed.") + "\n\n" + from_u8(err.what()));
        return nullptr;
    }
}

bool take_config_snapshot_cancel_on_error(const AppConfig &app_config, Snapshot::Reason reason, const std::string &comment, const std::string &message)
{
    try {
        SnapshotDB::singleton().take_snapshot(app_config, reason, comment);
        return true;
    } catch (std::exception &err) {
        RichMessageDialog dlg(static_cast<wxWindow*>(wxGetApp().mainframe),
            _L("OrcaSlicer has encountered an error while taking a configuration snapshot.") + "\n\n" + from_u8(err.what()) + "\n\n" + from_u8(message),
            _L("OrcaSlicer error"),
            wxYES_NO);
        dlg.SetYesNoLabels(_L("Continue"), _L("Abort"));
        return dlg.ShowModal() == wxID_YES;
    }
}

} // namespace Config
} // namespace GUI
} // namespace Slic3r
