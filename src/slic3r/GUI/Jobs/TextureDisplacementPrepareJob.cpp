#include "TextureDisplacementPrepareJob.hpp"

#include <algorithm>
#include <functional>
#include <exception>
#include "libslic3r/TextureDisplacement.hpp"
#include <cstddef>
#include <memory>
#include <optional>
#include <utility>
#include <string>
#include <vector>
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoTextureDisplacement.hpp"
#include "slic3r/Utils/UndoRedo.hpp"
#include "libslic3r/CutUtils.hpp"
#include "libslic3r/TriangleMesh.hpp"

namespace Slic3r::GUI {

TextureDisplacementPrepareJob::TextureDisplacementPrepareJob(
    TextureDisplacementPrepareInput &&input, std::function<void(TextureDisplacementPrepareOutcome)> on_finished)
    : m_input(std::move(input)), m_on_finished(std::move(on_finished))
{
}

void TextureDisplacementPrepareJob::process(Ctl &ctl)
{
    const std::string status = _u8L("Preparing mesh for texture displacement");
    ctl.update_status(1, status);

    // Only ever touches m_input (captured by value before this job was queued) and local state - never
    // the live Model - so this is safe to run concurrently with the UI thread.
    //
    // The progress hook is not just cosmetic: the framework's notification only grows a close button
    // once it reaches 100%, and below that it shows Cancel, which is wired through here. Reporting is
    // throttled to whole percentage points because every call repaints the notification and wakes the
    // idle loop.
    // Each part gets its own slice of the range, in order, so one bar covers them all.
    const size_t count = m_input.volumes.size();
    m_results.resize(count);
    int last_reported = 1;
    for (size_t i = 0; i < count; ++i) {
        const TextureDisplacementPrepareVolume &volume = m_input.volumes[i];
        m_results[i] = GLGizmoTextureDisplacement::prepare_mesh(volume.base_mesh, volume.masks, volume.layers,
                                                                volume.model_uv, volume.texture_frame,
                                                                m_input.params, m_input.color,
                                                                [&ctl, &status, &last_reported, i, count](int percent) {
                                                                    if (ctl.was_canceled())
                                                                        return false;
                                                                    const int overall =
                                                                        int((i * 100 + size_t(std::max(0, percent))) / count);
                                                                    if (overall > last_reported) {
                                                                        last_reported = overall;
                                                                        ctl.update_status(overall, status);
                                                                    }
                                                                    return true;
                                                                });
        if (ctl.was_canceled())
            break;
    }

    ctl.update_status(100, status); // always finish at 100: this is what closes the notification
}

void TextureDisplacementPrepareJob::finalize(bool canceled, std::exception_ptr &eptr)
{
    TextureDisplacementPrepareOutcome outcome = TextureDisplacementPrepareOutcome::Failed;
    struct OnExit
    {
        std::function<void(TextureDisplacementPrepareOutcome)> fn;
        const TextureDisplacementPrepareOutcome               *outcome;
        ~OnExit() { if (fn) fn(*outcome); }
    } on_exit{m_on_finished, &outcome};

    if (canceled || eptr)
        return;
    // One part losing its paint stops the whole prepare, exactly as it stops a single-part one: the
    // promise is that a prepare which cannot carry the paint changes nothing, and committing the other
    // parts would break it halfway. The message that follows tells the user to remesh by hand instead.
    for (const TextureDisplacementPrepareResult &result : m_results)
        if (result.paint_lost) {
            outcome = TextureDisplacementPrepareOutcome::PaintLost;
            return;
        }

    Plater *plater = wxGetApp().plater();

    // Resolved before the snapshot is opened, so a run whose parts have all gone away leaves no empty
    // undo step behind. The lookup doubles as the staleness check: anything that replaces a volume's
    // mesh also gives it a new id (set_new_unique_id()), so a prepare computed from a mesh that has
    // since been replaced - by an undo, another bake, or a boolean - simply fails to find its volume.
    std::vector<std::pair<size_t, ModelVolume *>> to_commit;
    for (size_t i = 0; i < m_results.size(); ++i) {
        if (m_results[i].mesh.indices.empty())
            continue;
        if (ModelVolume *volume = get_model_volume(m_input.volumes[i].volume_id, plater->model().objects);
            volume != nullptr && volume->get_object() != nullptr)
            to_commit.emplace_back(i, volume);
    }
    if (to_commit.empty()) {
        // Nothing needed doing anywhere, which is not an error - every mesh already met the criteria.
        outcome = TextureDisplacementPrepareOutcome::Unchanged;
        return;
    }

    ModelObject *object = to_commit.front().second->get_object();
    {
        Plater::TakeSnapshot snapshot(plater, m_input.snapshot_name, UndoRedo::SnapshotType::GizmoAction);

        for (const auto &[i, volume] : to_commit) {
            // The four standard paint channels ride across on ModelVolume's own spatial remap. The eight
            // texture-displacement masks do not go through it - prepare_mesh() has already carried them,
            // exactly, from the source triangles the refinement records - so they are put back after
            // restore_painting(), which resets every extra facet before remapping the channels it knows.
            std::optional<TriangleSelector::SavedPainting> saved_painting = volume->save_painting();
            volume->set_mesh(TriangleMesh(std::move(m_results[i].mesh)));
            volume->set_new_unique_id();
            volume->calculate_convex_hull();
            volume->restore_painting(saved_painting);
            for (int slot = 0; slot < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++slot)
                volume->texture_displacement_facet(slot).set_data(std::move(m_results[i].masks[size_t(slot)]));
            // The model's own UV channel, resampled onto the new mesh by the run. Also put back after
            // restore_painting(), which resets it along with the extra facets for the same reason: it is
            // indexed by triangle and the triangles have changed.
            volume->texture_uv = std::move(m_results[i].uv);
        }

        if (ObjectList *obj_list = wxGetApp().obj_list()) {
            const ModelObjectPtrs &objs = plater->model().objects;
            auto it = std::find(objs.begin(), objs.end(), object);
            if (it != objs.end())
                obj_list->update_info_items(size_t(it - objs.begin()));
        }
        plater->changed_object(*object);
    }
    outcome = TextureDisplacementPrepareOutcome::Committed;
}

void queue_texture_displacement_prepare(TextureDisplacementPrepareInput                      &&input,
                                        std::function<void(TextureDisplacementPrepareOutcome)> on_finished)
{
    auto &worker = wxGetApp().plater()->get_ui_job_worker();
    queue_job(worker, std::make_unique<TextureDisplacementPrepareJob>(std::move(input), std::move(on_finished)));
}

} // namespace Slic3r::GUI
