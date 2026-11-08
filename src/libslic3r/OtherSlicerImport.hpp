#ifndef slic3r_OtherSlicerImport_hpp_
#define slic3r_OtherSlicerImport_hpp_

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>

namespace Slic3r {

class DynamicPrintConfig;

// Unlayered 0.1.7: bring a person's own printer, filament and process presets over from PrusaSlicer,
// OrcaSlicer and Bambu Studio. No GUI here; the dialog (OtherSlicerImportDialog) runs the result
// through PresetBundle::import_presets like any imported preset file.
//
// The steps:
//  - find(): which of the supported apps have a data directory on this machine;
//  - scan(): read that app's user presets (never written to, never anything else of the app's);
//  - plan(): for each preset, resolve its "inherits" chain IN THE SOURCE APP (user presets, the
//    app's installed system presets, the vendor profiles shipped with it) and flatten it, so the
//    result stands on its own. If Unlayered Slicer has a system preset of the same name as one of the
//    ancestors, the preset inherits that instead and keeps its link to the vendor profile (the
//    importer then saves only what differs from it). PrusaSlicer keys and values are translated to
//    this fork's names. Names that already exist get " (<App>)" appended; nothing is overwritten.
//    A printer's network ADDRESS (host type, host, web UI, port) is copied; its
//    secrets (API keys, user names, passwords, tokens, certificate files) never are.
//  - write(): one preset JSON per planned preset, in the layout PresetBundle::import_presets reads.
//  - ImportHistory: which source presets were imported before, so a second run adds only new ones.
//
// Since 0.1.7 the import also brings over what the person picked from the app's BUILT-IN lists (most
// people never save a preset of their own): the printer models + nozzles and system filaments its app
// config has enabled, and, on a fresh install here, its current printer / process / filament. Those
// are matched against the vendor profiles Unlayered Slicer ships; anything it does not have is
// reported as skipped, never an error.
namespace OtherSlicers {

// New values go at the end: the import history keys on app_key(), never on the number.
enum class App {
    PrusaSlicer, OrcaSlicer, BambuStudio,
    CrealityPrint, SuperSlicer, ElegooSlicer, AnycubicSlicerNext, SnapmakerOrca, QIDIStudio, FlashStudio, PhrozenOrca,
    FlsunSlicer
};
enum class Kind { Printer, Filament, Process };

// "PrusaSlicer", "OrcaSlicer", "Bambu Studio": what the person sees, and the " (<App>)" suffix.
std::string app_name(App app);
// True for the PrusaSlicer family (.ini presets: PrusaSlicer, SuperSlicer); false for the
// OrcaSlicer / Bambu Studio family (JSON presets under user/<account>/).
bool prusa_format(App app);

// One slicer the importer looks for. Folder names are relative to the per-user application data
// directory (Windows %APPDATA%, macOS ~/Library/Application Support, Linux $XDG_CONFIG_HOME or
// ~/.config, and the Flatpak sandbox when the app has one) and may be nested ("Vendor/App/6.0");
// the first that exists is used.
struct AppInfo
{
    App                      app;
    std::string              name;         // shown to the person, and the " (<name>)" suffix
    std::string              key;          // stable id in the import history
    bool                     prusa_format; // see prusa_format()
    std::vector<std::string> folders;      // data directory candidates, newest version first
    std::string              conf;         // its app config file in that directory
    std::string              flatpak;      // Flatpak app id, "" for none
    std::vector<std::string> windows_profiles; // shipped profiles, relative to Program Files
    std::vector<std::string> mac_profiles;     // shipped profiles, absolute
    std::vector<std::string> linux_profiles;   // shipped profiles, absolute
};
const std::vector<AppInfo> &apps();
const AppInfo              &app_info(App app);
// "prusaslicer", "orcaslicer", "bambustudio": the history file and diagnostics tallies.
std::string app_key(App app);
// The PresetBundle type subdirectory: "machine", "filament", "process".
std::string kind_dir(Kind kind);

// One installation of another slicer.
struct AppLocation
{
    App                                  app;
    boost::filesystem::path              data_dir;      // e.g. %APPDATA%\OrcaSlicer
    // Where the app's system presets can be found, searched in order: its installed vendor profiles
    // (data_dir/system, data_dir/vendor) first, then the profiles shipped with the program.
    std::vector<boost::filesystem::path> profile_roots;
};

// The data directories of the supported apps that exist on this machine (Windows %APPDATA%, macOS
// ~/Library/Application Support, Linux $XDG_CONFIG_HOME or ~/.config and the Flatpak sandboxes).
// `exclude` is never returned (Unlayered Slicer's own data directory).
std::vector<AppLocation> find(const boost::filesystem::path &exclude = {});
// The same for one app, rooted at `data_dir`, with the standard profile roots under it. For tests
// and for an installation somewhere unusual.
AppLocation location_at(App app, const boost::filesystem::path &data_dir);

// vendor -> printer model -> nozzle diameters, as AppConfig::VendorMap stores the enabled ones.
using VendorModels = std::map<std::string, std::map<std::string, std::set<std::string>>>;

// What the person picked from the app's built-in lists, from its own app config:
// OrcaSlicer.conf / BambuStudio.conf (JSON + a "# MD5 checksum" line), PrusaSlicer.ini.
struct SystemSelection
{
    VendorModels             models;    // enabled printer models and nozzles
    std::vector<std::string> filaments; // enabled system filaments
    // The current selection: printer, process, first filament ("" when not recorded).
    std::string              machine, process, filament;

    bool empty() const { return models.empty() && filaments.empty(); }
    // Any enabled model of this vendor ("BBL": Bambu Lab printers).
    bool has_vendor(const std::string &vendor) const;
};

// The app config's text without its trailing "# MD5 checksum <hex>" line (anything after the last '}').
std::string strip_checksum_line(const std::string &text);
// OrcaSlicer / Bambu Studio app config ("models", "filaments", "presets").
SystemSelection parse_app_config_json(const std::string &text);
// PrusaSlicer.ini ("[vendor:<Vendor>]" with "model:<id> = <nozzles>", "[filaments]", "[presets]").
SystemSelection parse_app_config_ini(const std::string &text);

// The app's recent project files, most recent first, as its app config lists them (Orca family:
// "recent_projects": {"001": path, ...} in the JSON config; PrusaSlicer family: [recent_projects]
// 1 = path). Paths as written; whether they still exist is the caller's business. Shown on the
// home page next to Unlayered's own (MainFrame::get_recent_projects).
std::vector<std::string> recent_projects(const AppLocation &location);
std::vector<std::string> parse_recent_projects_json(const std::string &text);
std::vector<std::string> parse_recent_projects_ini(const std::string &text);
// The app config file in `location.data_dir`, or an empty selection.
SystemSelection read_system_selection(const AppLocation &location);
// The printer models Unlayered ships: <profiles_dir>/<Vendor>.json "machine_model_list" and each
// model's "nozzle_diameter" ("0.4;0.2;0.6").
VendorModels read_vendor_models(const boost::filesystem::path &profiles_dir);
// The system filaments Unlayered ships, name -> vendor, from each <profiles_dir>/<Vendor>.json
// "filament_list" (the files may sit in sub folders, so the file name is not enough).
std::map<std::string, std::string> read_vendor_filaments(const boost::filesystem::path &profiles_dir);

// A PrusaSlicer physical printer (physical_printer/<name>.ini): a network connection that names the
// printer presets it is used with. Only the address is kept here; `secret` says one was set.
struct PhysicalPrinterSource
{
    std::string              name;
    nlohmann::json           address = nlohmann::json::object(); // host_type, print_host, ... in this fork's keys
    std::vector<std::string> presets;                            // the printer presets it is linked to
    bool                     secret{false};                      // an API key / user / password was set
};

// A user preset as the source app stores it.
struct SourcePreset
{
    App                     app;
    Kind                    kind;
    std::string             name;
    boost::filesystem::path file;     // for the log only
    std::string             inherits; // the source app's parent, "" for none
    // The preset's own values. Orca/Bambu: the JSON object as written. PrusaSlicer: the .ini lines
    // as JSON strings, still in PrusaSlicer's key names.
    nlohmann::json          values = nlohmann::json::object();
};

struct Scan
{
    AppLocation                        location;
    std::vector<SourcePreset>          presets;
    SystemSelection                    selection; // picked from the app's built-in lists
    std::vector<PhysicalPrinterSource> physical;  // PrusaSlicer physical printers
    // Found but never imported, by reason ("SLA", "unreadable", "duplicates").
    std::map<std::string, int> not_importable;
};

// Reads the app's user presets. Light: only the user presets are parsed, nothing is resolved.
Scan scan(const AppLocation &location);

// What Unlayered Slicer already has.
struct Target
{
    // Any preset of this kind (system, default, user, bundle) already has this name.
    std::function<bool(Kind, const std::string &)> name_taken;
    // A system preset of this kind with this name can be inherited from.
    std::function<bool(Kind, const std::string &)> has_system;
    // Optional: the existing USER preset of this name, to tell a preset imported by hand before
    // (identical settings, nothing to do) from a different one that happens to share its name.
    std::function<const DynamicPrintConfig *(Kind, const std::string &)> user_config;

    // For the built-in selections (all optional; without them nothing of the kind is offered):
    // the printer models Unlayered ships (read_vendor_models) and those already enabled here
    // (AppConfig::vendors()).
    VendorModels shipped_models;
    VendorModels enabled_models;
    // The shipped vendor defining the system filament of this name ("" when none), and whether that
    // filament is already enabled here.
    std::function<std::string(const std::string &)> filament_vendor;
    std::function<bool(const std::string &)>        filament_enabled;
    // No printer of the person's own is selected here yet (a fresh install): the source app's current
    // printer / process / filament may be selected. An existing selection is never changed.
    bool fresh{false};
};

// What was imported by earlier runs: (app, kind, source name) -> the name it got here.
class ImportHistory
{
public:
    void        load(const boost::filesystem::path &file);
    bool        save(const boost::filesystem::path &file) const;
    // The name an earlier import gave this source preset, or "".
    std::string imported_as(App app, Kind kind, const std::string &source_name) const;
    void        add(App app, Kind kind, const std::string &source_name, const std::string &target_name);
    bool        empty() const { return m_entries.empty(); }
    // The built-in selections brought over before: printer "<vendor>;<model>;<nozzle>" or a filament
    // name. Not offered again, even when the person has since turned it off here.
    bool        has_system(App app, Kind kind, const std::string &what) const;
    void        add_system(App app, Kind kind, const std::string &what);

private:
    std::map<std::string, std::string> m_entries; // "<app_key>|<kind_dir>|<source name>" -> target
};

struct Planned
{
    SourcePreset   source;
    std::string    target_name;     // == source.name unless that was taken
    std::string    target_inherits; // an Unlayered system preset, or "" (flattened, stands alone)
    nlohmann::json config = nlohmann::json::object(); // the preset JSON to import
    int            dropped_keys{0}; // settings this fork has no equivalent for (PrusaSlicer only)
    // An API key / user name / password was set in the other app and left out: the person has to
    // enter it again (the address itself is kept).
    bool           dropped_connection{false};
    // The PrusaSlicer physical printer whose address was added to this printer ("" for none).
    std::string    connection_from;
};

struct Skipped
{
    App         app;
    Kind        kind;
    std::string name;
    std::string reason; // English, for the log
    // NotShipped: a printer model / nozzle or filament picked from the app's built-in list that
    // Unlayered Slicer has no profile for. NoPrinter: a PrusaSlicer physical printer none of whose
    // printer presets is imported, so its address has nowhere to go.
    enum class Why { AlreadyImported, AlreadyPresent, Duplicate, MissingParent, NotShipped, NoPrinter } why;
    // AlreadyImported: the name it got here; MissingParent: the parent's name; NotShipped: the vendor
    // when a filament was left out because none of that vendor's printers is selected, else "";
    // NoPrinter: the printer preset the connection names.
    std::string detail;
};

// A printer model + nozzle from the app's built-in list, to enable here (as the setup wizard does).
struct EnableModel
{
    App         app;
    std::string vendor, model, nozzle; // Unlayered's names
};

// A system filament from the app's built-in list, to enable here.
struct EnableFilament
{
    App         app;
    std::string name, vendor;
};

// The source app's current printer / process / filament, to select on a fresh install ("" = keep).
struct CurrentSelection
{
    App         app;
    std::string machine, process, filament;
};

struct Plan
{
    std::vector<Planned>            items;
    std::vector<Skipped>            skipped;
    std::vector<EnableModel>        models;
    std::vector<EnableFilament>     filaments;
    std::optional<CurrentSelection> select;
};

// Counts of what a fresh plan() would import per app, without resolving anything: user presets
// not imported before (by the history, while the preset it was imported as still exists), and the
// built-in printer models / filaments Unlayered has that are neither enabled here nor brought over
// before.
struct Counts
{
    int printers{0}, filaments{0}, processes{0}; // the person's own (user) presets
    int models{0}, system_filaments{0};          // picked from the app's built-in lists
    int user_presets() const { return printers + filaments + processes; }
    int total() const { return user_presets() + models + system_filaments; }
};
Counts importable_counts(const Scan &scan, const Target &target, const ImportHistory &history);

// `with_system`: also plan the built-in selections (models, filaments, select). False plans the
// user presets only (the GUI re-plans them once the vendor profiles they need are installed).
Plan plan(const std::vector<Scan> &scans, const Target &target, const ImportHistory &history, bool with_system = true);

// One <target name>.json per planned item under dir/<kind_dir>/, in the format
// PresetBundle::import_presets reads. Returns the files, in plan order (printers first).
std::vector<std::string> write(const Plan &plan, const boost::filesystem::path &dir);

// Exposed for the tests.
// A PrusaSlicer preset's lines (key -> value as written in its .ini) in this fork's keys and
// values, as preset JSON values (strings, or arrays of strings for vector settings).
nlohmann::json translate_prusa(const std::map<std::string, std::string> &ini, Kind kind, int *dropped = nullptr);
// Never imported: credentials (API keys, user names, passwords, tokens), certificate and other files
// elsewhere on the disk. A printer's address (host_type, print_host, print_host_webui, port) is not
// private and is copied.
bool is_private_key(const std::string &key);
// The printer connection keys that ARE copied: where the printer is, not how to log in.
bool is_connection_address_key(const std::string &key);
// A PrusaSlicer .ini (a user preset, or a vendor bundle with [kind:name] sections) as
// section -> key -> value. A file without sections lands in section "".
std::map<std::string, std::map<std::string, std::string>> parse_ini(const std::string &text);

} // namespace OtherSlicers
} // namespace Slic3r

#endif // slic3r_OtherSlicerImport_hpp_
