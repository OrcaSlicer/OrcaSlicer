#include "ServerContext.hpp"

#include "PresetResolver.hpp"  // enumerate_preset_names (SliceCore — same include path as SliceTypes.hpp)

namespace Slic3r {
namespace Server {

bool ProfileCache::load_locked(const std::string &datadir, std::string &err)
{
    // Delegate to the shared SliceCore enumeration function so there is a
    // single source of truth for PresetBundle loading.  ProfileCache adds
    // the mutex guard and per-datadir caching on top.
    SliceCore::PresetNames pn;
    if (!SliceCore::enumerate_preset_names(datadir, pn, err))
        return false;

    // Map SliceCore::PresetNames fields into ProfileCache::Names.
    // Both structs have the same three members (printers/processes/filaments),
    // so this is a straightforward move.
    Names names;
    names.printers  = std::move(pn.printers);
    names.processes = std::move(pn.processes);
    names.filaments = std::move(pn.filaments);

    m_names          = std::move(names);
    m_loaded         = true;
    m_loaded_datadir = datadir;
    return true;
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
