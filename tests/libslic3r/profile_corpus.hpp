///|/ Shared enumeration for the profile-corpus harnesses.
///|/
///|/ Loads shipped vendor profiles and resolves each printer preset against a
///|/ range of filament configurations via the real full_config() assembly path,
///|/ invoking a callback with each resolved config. API-neutral (only full_config),
///|/ so callers can run it on either main or the ConfigCardinality branch.
///|/
///|/ Consumer: test_printconfigcardinality.cpp - its [ProfileCorpus] (baseline/branch diff),
///|/ [ProfileCardinalityInvariant] (after-state length invariant), and [ProfileCardinalityGate] cases.
///|/
#pragma once

#include <boost/filesystem.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/log/core.hpp>

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <functional>
#include <libslic3r/PrintConfig.hpp>
#include <libslic3r/Config.hpp>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"

namespace Slic3r { namespace CorpusTest {

namespace fs = boost::filesystem;

// TEST_DATA_DIR is <repo>/tests/data; the repo root is two levels up.
inline fs::path repo_root() { return fs::path(TEST_DATA_DIR).parent_path().parent_path(); }

// A representative, multi-vendor default set (includes the multi-extruder lanes).
inline std::vector<std::string> default_vendors()
{
    return { "BBL", "Snapmaker", "Prusa", "Creality", "Voron" };
}

inline std::vector<std::string> split_csv(const std::string &s)
{
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ','))
        if (! item.empty())
            out.push_back(item);
    return out;
}

// Every printer vendor shipped in resources/profiles: a <name>.json with a <name>/ directory beside
// it, which leaves out blacklist.json. The filament library has no printers to resolve; it loads as
// the base of every other vendor.
inline std::vector<std::string> all_vendors()
{
    const fs::path           profiles = repo_root() / "resources" / "profiles";
    std::vector<std::string> vendors;
    for (const std::string &name : vendor_names_in(profiles))
        if (name != PresetBundle::ORCA_FILAMENT_LIBRARY && fs::is_directory(profiles / name))
            vendors.push_back(name);
    return vendors;
}

// The vendor set to resolve: ORCA_CORPUS_VENDORS (comma-separated) if set, else the fallback.
inline std::vector<std::string> vendors_from_env(std::vector<std::string> fallback = default_vendors())
{
    if (const char *v = std::getenv("ORCA_CORPUS_VENDORS"))
        if (std::vector<std::string> vendors = split_csv(v); ! vendors.empty())
            return vendors;
    return fallback;
}

struct Combo {
    std::string  vendor, printer, filament, process;
    unsigned int nozzles = 0, filaments = 0;
};

// Resolve the corpus: for each printer x {N, N+1} filaments, call fn(combo, cfg).
// Returns { resolved, failed }. The presets load read-only, so nothing is written
// under data_dir(), which is the source tree's resources/profiles.
inline std::pair<int, int> for_each_resolved_config(
    const std::vector<std::string> &vendors,
    const std::function<void(const Combo &, DynamicPrintConfig &)> &fn)
{
    // Restore the process-global state we mutate below (logging, resources/data dirs) on the way out, so a
    // corpus test can't leak it into other tests that share the process.
    struct RestoreEnv {
        bool        logging   = boost::log::core::get()->get_logging_enabled();
        std::string resources = resources_dir();
        std::string data      = data_dir();
        ~RestoreEnv() {
            set_data_dir(data);
            set_resources_dir(resources);
            boost::log::core::get()->set_logging_enabled(logging);
        }
    } restore_env;

    boost::log::core::get()->set_logging_enabled(false);

    const fs::path resources = repo_root() / "resources";
    const fs::path profiles  = resources / "profiles";
    set_resources_dir(resources.string());
    // In validation mode load_presets() reads vendor JSONs straight from data_dir().
    set_data_dir(profiles.string());

    int resolved = 0, failed = 0;
    for (const std::string &vendor : vendors) {
        PresetBundle bundle;
        bundle.set_is_validation_mode(true);
        bundle.set_vendor_to_validate(vendor);   // ORCA_FILAMENT_LIBRARY still loads first
        bundle.set_default_suppressed(true);

        AppConfig app_config;
        app_config.set("preset_folder", "default");
        try {
            bundle.load_presets(app_config, ForwardCompatibilitySubstitutionRule::Disable,
                                PresetBundle::PresetPreferences(), nullptr, /*read_only=*/true);
        } catch (const std::exception &) {
            ++failed;
            continue;
        }

        std::vector<std::string> printer_names = bundle.printers.system_preset_names();
        std::sort(printer_names.begin(), printer_names.end());

        for (const std::string &pname : printer_names) {
            if (! bundle.printers.select_preset_by_name(pname, true))
                continue;
            // Read the nozzle count straight off the selected printer preset; no update_compatible needed here
            // (the inner {n, n+1} loop re-selects and recomputes compatibility for the actual resolution).
            const ConfigOption *opt = bundle.printers.get_edited_preset().config.option("nozzle_diameter");
            const auto *nd = dynamic_cast<const ConfigOptionFloats *>(opt);
            const unsigned int n = (nd && ! nd->values.empty()) ? (unsigned int) nd->values.size() : 1u;

            const unsigned int counts[] = { n, n + 1 };
            for (unsigned int c = 0; c < 2; ++c) {
                try {
                    bundle.printers.select_preset_by_name(pname, true);
                    bundle.update_compatible(PresetSelectCompatibleType::Always);
                    bundle.set_num_filaments(counts[c]);

                    DynamicPrintConfig cfg = bundle.full_config();
                    Combo combo{ vendor, pname,
                                 bundle.filaments.get_edited_preset().name,
                                 bundle.prints.get_edited_preset().name, n, counts[c] };
                    fn(combo, cfg);
                    ++resolved;
                } catch (const std::exception &) {
                    ++failed;
                }
            }
        }
    }
    return { resolved, failed };
}

}} // namespace Slic3r::CorpusTest
