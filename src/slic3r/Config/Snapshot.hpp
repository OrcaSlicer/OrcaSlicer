#ifndef slic3r_GUI_Snapshot_
#define slic3r_GUI_Snapshot_

#include <map>
#include <set>
#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>

#include "libslic3r/Semver.hpp"
#include "Version.hpp"

namespace Slic3r { 

class AppConfig;

namespace GUI {
namespace Config {


// A snapshot contains:
// 		Slic3r.ini
// 		vendor/
// 		print/
// 		sla_print/
// 		filament/
//		sla_material
//		printer/
// 		physical_printer/
class Snapshot
{
public:
	enum Reason {
		SNAPSHOT_UNKNOWN,
		SNAPSHOT_UPGRADE,
		SNAPSHOT_DOWNGRADE,
		SNAPSHOT_BEFORE_ROLLBACK,
		SNAPSHOT_USER,
	};

	Snapshot() { clear(); }

	void 		clear();
	void 		load_ini(const std::string &path);
	void 		save_ini(const std::string &path);

	// Export the print / sla_print / filament / sla_material / printer selections to be activated into the AppConfig.
	void 		export_selections(AppConfig &config) const;
	void 		export_vendor_configs(AppConfig &config) const;

	// Perform a deep compare of the active print / sla_print / filament / sla_material / printer / physical_printer / vendor directories.
	// Return true if the content of the current print / sla_print / filament / sla_material / printer / physical_printer / vendor directories
	// matches the state stored in this snapshot.
	bool 		equal_to_active(const AppConfig &app_config) const;

	// ID of a snapshot should equal to the name of the snapshot directory.
	// The ID contains the date/time, reason and comment to be human readable.
	std::string					id;
	std::time_t					time_captured;
	// Which Slic3r version captured this snapshot?
	Semver		 				slic3r_version_captured = Semver::invalid();
	// Comment entered by the user at the start of the snapshot capture.
	std::string 				comment;
	Reason						reason;

	std::string 				format_reason() const;

	// Active presets at the time of the snapshot.
	std::string 				print;
	std::string 				sla_print;
	std::vector<std::string>	filaments;
	std::string 				sla_material;
	std::string					printer;
	std::string 				physical_printer;

	// Annotation of the vendor configuration stored in the snapshot.
	// This information is displayed to the user and used to decide compatibility
	// of the configuration stored in the snapshot with the running Slic3r version.
	struct VendorConfig {
		// Name of the vendor contained in this snapshot.
		std::string name;
		// Version of the vendor config contained in this snapshot, along with compatibility data.
		Version version;
		// Which printer models of this vendor were installed, and which variants of the models?
		std::map<std::string, std::set<std::string>> models_variants_installed;
	};
	// List of vendor configs contained in this snapshot, sorted lexicographically.
	std::vector<VendorConfig> 	vendor_configs;
};

class SnapshotDB
{
public:
	// Initialize the SnapshotDB singleton instance. Load the database if it has not been loaded yet.
	static SnapshotDB&				singleton();

	typedef std::vector<Snapshot>::const_iterator const_iterator;

	// Load the snapshot database from the snapshots directory.
	// If the snapshot directory or its parent does not exist yet, it will be created.
	// Returns a number of snapshots loaded.
	size_t 							load_db();
	void 							update_slic3r_versions(std::vector<Index> &index_db);

	// Create a snapshot directory, copy the vendor config bundles, user print / sla_print / filament / sla_material / printer / physical_printer profiles,
	// create an index.
	const Snapshot&					take_snapshot(const AppConfig &app_config, Snapshot::Reason reason, const std::string &comment = "");
	const Snapshot&					restore_snapshot(const std::string &id, AppConfig &app_config);
	void 							restore_snapshot(const Snapshot &snapshot, AppConfig &app_config);
	// Test whether the AppConfig's on_snapshot variable points to an existing snapshot, and the existing snapshot
	// matches the current state. If it does not match the current state, the AppConfig's "on_snapshot" ID is reset.
	bool 							is_on_snapshot(AppConfig &app_config) const;
	// Finds the newest snapshot, which contains a config bundle for vendor_name with config_version.
	const_iterator					snapshot_with_vendor_preset(const std::string &vendor_name, const Semver &config_version);

	const_iterator					begin()     const { return m_snapshots.begin(); }
	const_iterator					end()       const { return m_snapshots.end(); }
	const_iterator 					snapshot(const std::string &id) const;
	const std::vector<Snapshot>& 	snapshots() const { return m_snapshots; }

private:
	// Create the snapshots directory if it does not exist yet.
	static boost::filesystem::path	create_db_dir();

	// Snapshots are sorted by their date/time, oldest first.
	std::vector<Snapshot>			m_snapshots;
};

// Take snapshot on SnapshotDB::singleton(). If taking snapshot fails, report an error and return nullptr.
const Snapshot* take_config_snapshot_report_error(const AppConfig &app_config, Snapshot::Reason reason, const std::string &comment);

// Take snapshot on SnapshotDB::singleton(). If taking snapshot fails, report "message", and present a "Continue" or "Abort" buttons to respond.
// Return true on success and on "Continue" to continue with the process (for example installation of presets).
bool take_config_snapshot_cancel_on_error(const AppConfig &app_config, Snapshot::Reason reason, const std::string &comment, const std::string &message);

} // namespace Config
} // namespace GUI
} // namespace Slic3r

#endif /* slic3r_GUI_Snapshot_ */
