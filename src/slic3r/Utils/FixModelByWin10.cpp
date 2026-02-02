#include "FixModelByWin10.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "libslic3r/MeshBoolean.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/format.hpp"
#include "../GUI/I18N.hpp"

namespace Slic3r {

class RepairCanceledException : public std::exception {
public:
    const char* what() const noexcept override { return "Model repair has been canceled"; }
};

// Return false if fixing was canceled.
// fix_result is empty if fixing finished successfully; otherwise contains a message.
bool fix_model_by_win10_sdk_gui(ModelObject &model_object, int volume_idx, GUI::ProgressDialog &progress_dialog, const wxString &msg_header, std::string &fix_result)
{
    std::mutex mtx;
    std::condition_variable condition;
    struct Progress {
        std::string message;
        int         percent  = 0;
        bool        updated  = false;
    } progress;

    std::atomic<bool> canceled = false;
    std::atomic<bool> finished = false;

    std::vector<ModelVolume*> volumes;
    if (volume_idx == -1)
        volumes = model_object.volumes;
    else
        volumes.emplace_back(model_object.volumes[volume_idx]);

    bool   success = false;
    size_t ivolume = 0;

    auto on_progress = [&mtx, &condition, &ivolume, &volumes, &progress](const char *msg, unsigned prcnt) {
        std::unique_lock<std::mutex> lock(mtx);
        progress.message = msg;
        progress.percent = int(std::floor((float(prcnt) + float(ivolume) * 100.f) / float(volumes.size())));
        progress.updated = true;
        condition.notify_all();
    };

    auto worker_thread = std::thread([&model_object, &volumes, &ivolume, on_progress, &success, &canceled, &finished, &fix_result]() {
        try {
            for (; ivolume < volumes.size(); ++ivolume) {
                if (canceled)
                    throw RepairCanceledException();

                on_progress(L("Repairing model object"), 10);

                TriangleMesh mesh = volumes[ivolume]->mesh();
                std::string  error;
                if (!MeshBoolean::cgal::repair(mesh, nullptr, &error))
                    throw Slic3r::RuntimeError(error.empty() ? L("Repair failed") : error.c_str());

                volumes[ivolume]->set_mesh(std::move(mesh));
                volumes[ivolume]->calculate_convex_hull();
                volumes[ivolume]->invalidate_convex_hull_2d();
                volumes[ivolume]->set_new_unique_id();

                on_progress(L("Repair finished"), 100);
            }

            model_object.invalidate_bounding_box();

            if (ivolume > 0)
                --ivolume;
            on_progress(L("Repair finished"), 100);
            success = true;
            finished = true;
        } catch (RepairCanceledException &) {
            canceled = true;
            finished = true;
            on_progress(L("Repair canceled"), 100);
        } catch (std::exception &ex) {
            success = false;
            finished = true;
            fix_result = ex.what();
            on_progress(ex.what(), 100);
        }
    });

    while (!finished) {
        std::unique_lock<std::mutex> lock(mtx);
        condition.wait_for(lock, std::chrono::milliseconds(250), [&progress]{ return progress.updated; });

        // Decrease progress percent slightly to avoid auto-closing.
        if (!progress_dialog.Update(progress.percent - 1, msg_header + _(progress.message)))
            canceled = true;
        else
            progress_dialog.Fit();

        progress.updated = false;
    }

    if (canceled) {
        // Nothing to show.
    } else if (success) {
        fix_result.clear();
    }

    if (worker_thread.joinable())
        worker_thread.join();

    return !canceled;
}

} // namespace Slic3r
