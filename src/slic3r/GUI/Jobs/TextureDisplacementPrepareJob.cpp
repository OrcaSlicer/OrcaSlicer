#include "TextureDisplacementPrepareJob.hpp"

#include <algorithm>
#include <optional>

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoTextureDisplacement.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

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
    int last_reported = 1;
    m_result = GLGizmoTextureDisplacement::prepare_mesh(m_input.base_mesh, m_input.masks, m_input.layers,
                                                        m_input.params,
                                                        [&ctl, &status, &last_reported](int percent) {
                                                            if (ctl.was_canceled())
                                                                return false;
                                                            if (percent > last_reported) {
                                                                last_reported = percent;
                                                                ctl.update_status(percent, status);
                                                            }
                                                            return true;
                                                        });

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
    if (m_result.paint_lost) {
        outcome = TextureDisplacementPrepareOutcome::PaintLost;
        return;
    }
    if (m_result.mesh.indices.empty()) {
        outcome = TextureDisplacementPrepareOutcome::Unchanged;
        return;
    }

    Plater      *plater = wxGetApp().plater();
    ModelVolume *volume = get_model_volume(m_input.volume_id, plater->model().objects);
    // The lookup doubles as a staleness check: anything that replaces a volume's mesh also gives it a
    // new id (set_new_unique_id()), so a prepare computed from a mesh that has since been replaced -
    // by an undo, another bake, or a boolean - simply fails to find its volume and commits nothing.
    if (volume == nullptr)
        return;
    ModelObject *object = volume->get_object();
    if (object == nullptr)
        return;

    {
        Plater::TakeSnapshot snapshot(plater, m_input.snapshot_name, UndoRedo::SnapshotType::GizmoAction);

        // The four standard paint channels ride across on ModelVolume's own spatial remap. The eight
        // texture-displacement masks do not go through it - prepare_mesh() has already carried them,
        // exactly, from the source triangles the refinement records - so they are put back after
        // restore_painting(), which resets every extra facet before remapping the channels it knows.
        std::optional<TriangleSelector::SavedPainting> saved_painting = volume->save_painting();
        volume->set_mesh(TriangleMesh(std::move(m_result.mesh)));
        volume->set_new_unique_id();
        volume->calculate_convex_hull();
        volume->restore_painting(saved_painting);
        for (int i = 0; i < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++i)
            volume->texture_displacement_facet(i).set_data(std::move(m_result.masks[size_t(i)]));

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
