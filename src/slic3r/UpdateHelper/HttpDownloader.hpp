#ifndef slic3r_Updater_HttpDownloader_hpp_
#define slic3r_Updater_HttpDownloader_hpp_

// Minimal resumable HTTP download used by the updater helper. It knows nothing
// about handoffs or windows: give it a URL and the .part path, and it appends to
// what is already there.

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace Slic3r {
namespace Updater {

struct DownloadResult
{
    bool        ok = false;
    bool        cancelled = false;
    std::string error;
    int64_t     done = 0;      // bytes on disk when the call returned
    int64_t     total = 0;     // 0 when the server did not announce a length
    long        http_code = 0;
    int64_t     resumed_from = 0; // bytes that were already in the .part file
};

class HttpDownloader
{
public:
    // Called from the transfer thread, with absolute byte counts (resume included).
    using ProgressFn = std::function<void(int64_t done, int64_t total)>;

    // Creates the parent directory of `part_path` when it is missing and resumes
    // from the current .part size. `cancel` is polled from libcurl's progress
    // callback; setting it aborts the transfer and leaves the .part in place.
    DownloadResult download(const std::string&       url,
                            const std::string&       part_path,
                            const ProgressFn&        progress,
                            const std::atomic<bool>& cancel) const;

    static std::string user_agent();
};

} // namespace Updater
} // namespace Slic3r

#endif // slic3r_Updater_HttpDownloader_hpp_
