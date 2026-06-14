#pragma once

#include <mutex>
#include <string>
#include <vector>

namespace Slic3r {
namespace Server {

// Process-wide server configuration shared (read-only) with the request
// handlers.  Threaded through Router so handlers stay decoupled from CLI
// argument parsing.
//
// - datadir       : OrcaSlicer data directory; used to build SliceRequest::datadir
//                   for jobs and to load the PresetBundle for /v1/profiles.
// - resources_dir : OrcaSlicer resources directory (already owned by JobQueue
//                   for SliceService; carried here for completeness/diagnostics).
struct ServerContext {
    std::string datadir;
    std::string resources_dir;
};

// Lazily-loaded, mutex-guarded cache of preset NAMES enumerated from a
// PresetBundle.  Profiles rarely change during a server run, so we load the
// bundle once (per datadir) and reuse the resulting name lists.
//
// Thread-safety: load() is invoked concurrently by connection threads; a
// single std::mutex serialises the (rare) load and the (frequent) reads.
class ProfileCache {
public:
    // The three preset kinds the /v1/profiles endpoint can enumerate.
    struct Names {
        std::vector<std::string> printers;
        std::vector<std::string> processes;   // "prints" collection
        std::vector<std::string> filaments;
    };

    // Returns the cached names for the given datadir, loading the PresetBundle
    // on first use.  On success returns true and fills `out`.  On failure
    // returns false and sets `err` (e.g. datadir missing/unreadable, or a
    // bundle-load exception).  Never throws.
    //
    // If `datadir` differs from the cached one, the cache is rebuilt for it.
    bool get(const std::string &datadir, Names &out, std::string &err);

private:
    mutable std::mutex m_mutex;
    bool               m_loaded = false;
    std::string        m_loaded_datadir;
    Names              m_names;

    // Performs the actual PresetBundle load + enumeration. Caller holds m_mutex.
    bool load_locked(const std::string &datadir, std::string &err);
};

} // namespace Server
} // namespace Slic3r
