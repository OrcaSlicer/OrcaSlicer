#ifndef slic3r_Updater_InstallApply_hpp_
#define slic3r_Updater_InstallApply_hpp_

// The seam between the updater window/downloader and the per-platform install.
// Keeping it this small is what lets each platform's swap be written and
// reviewed on its own.

#include <functional>
#include <string>

#include "libslic3r/UpdateHandoff.hpp"

namespace Slic3r {
namespace Updater {

enum class Stage {
    Idle,
    Downloading,
    Verifying,
    Installing,
    Launching,
    Failed,
    Done,
};

const char* stage_key(Stage stage); // "downloading", "verifying", ... for logs

// Everything the install needs from the window: progress reports and a way to
// notice that the user cancelled. Cancelling is only honoured before the swap
// starts; once installing begins the call runs to completion or failure.
struct ProgressSink
{
    std::function<void(Stage, int percent, const std::string& message)> report;
    std::function<bool()> cancelled;

    void say(Stage stage, int percent, const std::string& message) const
    {
        if (report)
            report(stage, percent, message);
    }
};

// Verified artifact in hand: swap it into the install described by `handoff`,
// record it, and launch the new build. Returns false with `error` filled when
// nothing was changed, or when a change was rolled back. `launch_new_build` is
// false for the install that runs after the user quit, where starting the
// application again would contradict the quit.
bool apply_update(const UpdateHandoff& handoff,
                  const std::string&   artifact_path,
                  ProgressSink&        sink,
                  std::string&         error,
                  bool                 launch_new_build = true);

} // namespace Updater
} // namespace Slic3r

#endif // slic3r_Updater_InstallApply_hpp_
