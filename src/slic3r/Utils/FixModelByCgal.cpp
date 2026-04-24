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

// Orca: This file provides utilities for repairing 3D model meshes using the CGAL library, handling mesh splitting, merging, and boolean operations.

namespace Slic3r {

namespace {

// Orca: Helper functions for analyzing mesh properties and transformations.

bool is_not_3dimensional_part(const TriangleMesh &mesh)
{
    // Orca: Determines if a mesh is degenerate or represents a non-3dimensional part by checking volume and bounding box dimensions.
    if (mesh.its.indices.empty())
        return true;

    indexed_triangle_set tmp = mesh.its;
    its_remove_degenerate_faces(tmp, true);
    if (tmp.indices.empty())
        return true;

    const BoundingBoxf3 bbox = mesh.bounding_box();
    const Vec3d size = bbox.size();
    const double min_dim = std::min(size.x(), std::min(size.y(), size.z()));
    const double max_dim = std::max(size.x(), std::max(size.y(), size.z()));
    if (min_dim <= EPSILON)
        return true;

    const double volume = std::abs(its_volume(mesh.its));
    const double bbox_volume = size.x() * size.y() * size.z();
    if (volume <= EPSILON)
        return true;

    const double min_relative_thickness = 1e-6;
    const double min_volume_ratio = 1e-6;
    if (min_dim / max_dim <= min_relative_thickness)
        return true;
    if (bbox_volume > 0.0 && volume / bbox_volume <= min_volume_ratio)
        return true;

    return false;
}

} // namespace

// Orca: Exception class for handling user-initiated cancellation of model repair operations.
class RepairCanceledException : public std::exception {
public:
    const char* what() const noexcept override { return "Model repair has been canceled"; }
};

// Orca: Main function to repair model objects using CGAL, with progress dialog and cancellation support.
// Returns false if fixing was canceled. fix_result contains error message if failed.
bool fix_model_with_cgal_gui(ModelObject &model_object, int volume_idx, GUI::ProgressDialog &progress_dialog, const wxString &msg_header, std::string &fix_result)
{
    // Orca: Synchronization primitives for progress updates between worker thread and GUI.
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

    // Orca: Lambda for updating progress from worker thread.
    auto on_progress = [&mtx, &condition, &ivolume, &model_object, &progress](const char *msg, unsigned prcnt) {
        std::unique_lock<std::mutex> lock(mtx);
        progress.message = msg;
        const size_t total = std::max<size_t>(1, model_object.volumes.size());
        progress.percent = int(std::floor((float(prcnt) + float(ivolume) * 100.f) / float(total)));
        progress.updated = true;
        condition.notify_all();
    };

    // Orca: Worker thread that performs the actual model repair operations.
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

                // Orca: Split splittable volumes into parts for individual processing.
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

                size_t removed_parts = 0;
                for (size_t idx = part_end + 1; idx > ivolume; --idx) {
                    const size_t part_idx = idx - 1;
                    const ModelVolume *part_volume = model_object.volumes[part_idx];
                    if (!is_not_3dimensional_part(part_volume->mesh()))
                        continue;

                    model_object.delete_volume(part_idx);
                    ++removed_parts;
                    if (part_end > 0)
                        --part_end;
                    else
                        part_end = 0;
                    if (volume_idx != -1)
                        end_volume = part_end;
                }

                if (removed_parts >= parts_count) {
                    ivolume = part_end;
                    on_progress(L("Repair finished"), 100);
                    continue;
                }

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

    // Orca: Main GUI loop to update progress dialog and handle cancellation.
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
