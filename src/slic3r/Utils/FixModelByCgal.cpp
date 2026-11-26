#include "FixModelByCgal.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <wx/string.h>

#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/MeshRepair.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Thread.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Widgets/ProgressDialog.hpp"

namespace Slic3r {

namespace {

class RepairCanceledException : public std::exception
{
public:
    const char *what() const noexcept override { return "Model repair has been canceled"; }
};

} // namespace

bool fix_model_with_cgal(ModelObject &model_object, int volume_idx,
                         const std::function<bool(int percent, const std::string &what)> &report_progress, std::string &fix_result,
                         bool keep_painting, bool *modified)
{
    if (modified)
        *modified = false;
    // Asked once before the worker starts, so that a cancel given at once leaves every mesh as it is.
    if (!report_progress(0, std::string())) {
        fix_result.clear();
        return false;
    }

    // Keeps the backup manager from reading a mesh while the worker replaces it.
    SaveObjectGaurd backup_gaurd(model_object);

    // A painted modifier follows the mesh of its host.
    std::vector<ModelVolume *> volumes;
    if (volume_idx < 0)
        std::copy_if(model_object.volumes.begin(), model_object.volumes.end(), std::back_inserter(volumes),
                     [](const ModelVolume *volume) { return !volume->is_painted_modifier(); });
    else if (std::size_t(volume_idx) < model_object.volumes.size() && !model_object.volumes[volume_idx]->is_painted_modifier())
        volumes.push_back(model_object.volumes[volume_idx]);

    std::mutex              mtx;
    std::condition_variable condition;
    struct Progress
    {
        std::string message;
        int         percent = 0;
        bool        updated = false;
    } progress;
    std::atomic<bool> canceled = false;
    std::atomic<bool> finished = false;
    bool              any_modified = false;
    std::string       errors;

    auto on_progress = [&mtx, &condition, &progress](const std::string &msg, int percent) {
        std::lock_guard<std::mutex> lock(mtx);
        progress.message = msg;
        progress.percent = percent;
        progress.updated = true;
        condition.notify_all();
    };

    auto worker_thread = std::thread([&]() {
        set_current_thread_name("cgal_fix_model");
        try {
            for (std::size_t i = 0; i < volumes.size(); ++i) {
                ModelVolume      *volume = volumes[i];
                const std::string name   = volumes.size() > 1 ? volume->name : std::string();
                const auto        report = [&](int percent) {
                    if (canceled)
                        throw RepairCanceledException();
                    on_progress(name, int((100 * i + std::size_t(percent)) / volumes.size()));
                };
                report(0);

                MeshRepairStats stats;
                std::string     error;
                if (!volume->repair(keep_painting, &stats, &error, report))
                    errors += (errors.empty() ? "" : "\n") + (volumes.size() > 1 ? volume->name + ": " : std::string()) + error;
                any_modified = any_modified || stats.modified;
            }
        } catch (const RepairCanceledException &) {
            canceled = true;
        } catch (const std::exception &ex) {
            errors += (errors.empty() ? "" : "\n") + std::string(ex.what());
        }
        // Volumes repaired before a cancel or an error keep their new mesh.
        model_object.invalidate_bounding_box();
        finished = true;
        on_progress(_u8L("Repair finished"), 100);
    });

    while (!finished) {
        Progress shown;
        {
            std::unique_lock<std::mutex> lock(mtx);
            condition.wait_for(lock, std::chrono::milliseconds(250), [&progress] { return progress.updated; });
            shown            = progress;
            progress.updated = false;
        }
        if (!report_progress(shown.percent, shown.message))
            canceled = true;
    }

    if (worker_thread.joinable())
        worker_thread.join();

    fix_result = canceled ? std::string() : errors;
    if (modified)
        *modified = any_modified;
    return !canceled;
}

bool fix_model_with_cgal_gui(ModelObject &model_object, int volume_idx, GUI::ProgressDialog &progress_dialog,
                             const wxString &msg_header, std::string &fix_result, bool keep_painting, bool *modified)
{
    const auto shown = [&progress_dialog, &msg_header](int percent, const std::string &what) {
        // One percent short of the maximum, so that the dialog does not close itself.
        if (!progress_dialog.Update(std::max(0, percent - 1), msg_header + wxString::FromUTF8(what)))
            return false;
        progress_dialog.Fit();
        return true;
    };
    return fix_model_with_cgal(model_object, volume_idx, shown, fix_result, keep_painting, modified);
}

} // namespace Slic3r
