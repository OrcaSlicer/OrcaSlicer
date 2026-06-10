#include "ServerContext.hpp"

#include <exception>

#include <boost/filesystem/operations.hpp>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"   // set_data_dir

namespace Slic3r {
namespace Server {

namespace {

// Collect the names of visible, non-default presets from a PresetCollection.
//
// PresetCollection::begin()/end() (const) already skip the leading default
// presets (Preset.hpp:480 — begin() = cbegin() + m_num_default_presets), so we
// only need to filter out invisible / explicitly-default entries here.
//   - Preset::name        — Preset.hpp (public member)
//   - Preset::is_visible  — Preset.hpp:227
//   - Preset::is_default  — Preset.hpp:218
template <typename CollectionT>
std::vector<std::string> collect_names(const CollectionT &coll)
{
    std::vector<std::string> names;
    for (auto it = coll.begin(); it != coll.end(); ++it) {
        const Preset &preset = *it;
        if (!preset.is_visible || preset.is_default)
            continue;
        names.push_back(preset.name);
    }
    return names;
}

} // anonymous namespace

bool ProfileCache::load_locked(const std::string &datadir, std::string &err)
{
    // Validate the datadir up front so we fail loud rather than letting the
    // bundle silently load nothing.
    if (datadir.empty()) {
        err = "datadir is not configured (start the server with --datadir)";
        return false;
    }
    boost::system::error_code ec;
    if (!boost::filesystem::exists(datadir, ec) ||
        !boost::filesystem::is_directory(datadir, ec)) {
        err = "datadir does not exist or is not a directory: " + datadir;
        return false;
    }

    try {
        // Mirror PresetResolver.cpp: point libslic3r's global data_dir at the
        // caller-supplied directory, build a minimal AppConfig (no on-disk
        // selections), then load all presets.
        //
        // NOTE: set_data_dir() mutates libslic3r global state shared with the
        // slice workers (which also call it via resolve()). This is safe under
        // the default single-worker JobQueue. With workers > 1, a profile load
        // racing a concurrent slice could transiently flip the global data_dir;
        // since the server runs one datadir, the value written is identical, so
        // the practical risk is nil. The cache below also makes loads rare.
        set_data_dir(datadir);

        AppConfig app_config;
        app_config.reset_selections();

        PresetBundle bundle;
        // load_presets(AppConfig&, ForwardCompatibilitySubstitutionRule)
        //   — PresetBundle.hpp:185
        bundle.load_presets(app_config,
                            ForwardCompatibilitySubstitutionRule::EnableSilent);

        Names names;
        // bundle.printers / prints / filaments — PresetBundle.hpp:314-320.
        names.printers   = collect_names(bundle.printers);   // PrinterPresetCollection
        names.processes  = collect_names(bundle.prints);     // PresetCollection
        names.filaments  = collect_names(bundle.filaments);  // PresetCollection

        m_names          = std::move(names);
        m_loaded         = true;
        m_loaded_datadir = datadir;
        return true;
    } catch (const std::exception &ex) {
        err = std::string("failed to load preset bundle: ") + ex.what();
        return false;
    } catch (...) {
        err = "failed to load preset bundle: unknown exception";
        return false;
    }
}

bool ProfileCache::get(const std::string &datadir, Names &out, std::string &err)
{
    std::lock_guard<std::mutex> lk(m_mutex);

    // (Re)load if never loaded, or the requested datadir changed.
    if (!m_loaded || m_loaded_datadir != datadir) {
        if (!load_locked(datadir, err))
            return false;
    }

    out = m_names;
    return true;
}

} // namespace Server
} // namespace Slic3r
