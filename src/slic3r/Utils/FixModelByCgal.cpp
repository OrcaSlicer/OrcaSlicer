#include "FixModelByCgal.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <limits>
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
bool fix_model_with_cgal_gui(ModelObject &model_object, int volume_idx, GUI::ProgressDialog &progress_dialog, const wxString &msg_header, std::string &fix_result)
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

    bool   success = false;
    size_t ivolume = 0;

    auto on_progress = [&mtx, &condition, &ivolume, &model_object, &progress](const char *msg, unsigned prcnt) {
        std::unique_lock<std::mutex> lock(mtx);
        progress.message = msg;
        const size_t total = std::max<size_t>(1, model_object.volumes.size());
        progress.percent = int(std::floor((float(prcnt) + float(ivolume) * 100.f) / float(total)));
        progress.updated = true;
        condition.notify_all();
    };

    auto worker_thread = std::thread([&model_object, volume_idx, &ivolume, on_progress, &success, &canceled, &finished, &fix_result]() {
        try {
            size_t start_volume = volume_idx == -1 ? 0 : size_t(volume_idx);
            size_t end_volume   = volume_idx == -1 ? std::numeric_limits<size_t>::max() : size_t(volume_idx);

            for (ivolume = start_volume; ivolume < model_object.volumes.size(); ++ivolume) {
                if (volume_idx != -1 && ivolume > end_volume)
                    break;
                if (canceled)
                    throw RepairCanceledException();

                on_progress(L("Repairing model object"), 10);

                ModelVolume *volume = model_object.volumes[ivolume];

                size_t parts_count = 1;
                if (volume->is_splittable()) {
                    parts_count = volume->split(1);
                    if (parts_count > 1) {
                        const std::string msg = Slic3r::format(L("Split into %1% parts"), parts_count);
                        on_progress(msg.c_str(), 10);
                    }
                }

                size_t part_end = std::min(ivolume + parts_count - 1, model_object.volumes.size() - 1);
                if (volume_idx != -1)
                    end_volume = part_end;

                for (size_t part_idx = ivolume; part_idx <= part_end && part_idx < model_object.volumes.size(); ++part_idx) {
                    ModelVolume *part_volume = model_object.volumes[part_idx];
                    TriangleMesh mesh = part_volume->mesh();
                    if (its_num_open_edges(mesh.its) != 0) {
                        std::string error;
                        if (!MeshBoolean::cgal::repair(mesh, nullptr, &error))
                            throw Slic3r::RuntimeError(error.empty() ? L("Repair failed") : error.c_str());

                        part_volume->set_mesh(std::move(mesh));
                        part_volume->calculate_convex_hull();
                        part_volume->invalidate_convex_hull_2d();
                        part_volume->set_new_unique_id();
                    }
                }

                ivolume = part_end;

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
