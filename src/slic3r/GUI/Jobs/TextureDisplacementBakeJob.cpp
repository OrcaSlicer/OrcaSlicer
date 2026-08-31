#include "TextureDisplacementBakeJob.hpp"

#include <algorithm>

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoTextureDisplacement.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

namespace Slic3r::GUI {

TextureDisplacementBakeJob::TextureDisplacementBakeJob(TextureDisplacementBakeInput &&input, std::function<void()> on_finished)
    : m_input(std::move(input)), m_on_finished(std::move(on_finished))
{
}

void TextureDisplacementBakeJob::process(Ctl &ctl)
{
    const std::string status = _u8L("Baking texture displacement");
    ctl.update_status(1, status);

    // Only ever touches m_input (captured by value before this job was queued) and local state -
    // never the live Model - so this is safe to run concurrently with the UI thread.
    //
    // The progress hook matters for more than cosmetics: the framework's progress notification only
    // grows a close button once it reaches 100%, so a job that reports 0 and nothing else leaves an
    // uncloseable notification pinned on screen. It also carries the Cancel button's effect into the
    // bake, which on a subdivided mesh can run for several seconds.
    // Colour, when any layer asks for it, is computed in the same pass as the displacement: both need
    // the same per-layer projection and UV work, and doing it twice would double the expensive part.
    TextureColorRequest  color_request;
    TextureColorRequest *color = nullptr;
    if (!m_input.color.empty()) {
        color_request.quantize = GLGizmoTextureDisplacement::make_palette_quantizer(m_input.color.palette);
        color_request.resolve  = GLGizmoTextureDisplacement::make_mix_resolver(
            m_input.color.palette, m_input.color.mix_mode, m_input.color.layer_height,
            m_input.color.dither_cell_mm);
        color_request.despeckle_passes = m_input.color.despeckle_passes;
        color_request.out_triangle     = &m_triangle_color;
        if (color_request.quantize)
            color = &color_request;
    }

    int last_reported = 1;
    m_result = TriangleMesh(build_texture_displacement(
        m_input.base_mesh, m_input.layers, m_input.facets_data, m_input.options,
        [&ctl, &status, &last_reported](int percent) {
            if (ctl.was_canceled())
                return false;
            // The notification repaints (and wakes the idle loop) on every call, so only push a
            // message when the displayed integer percentage actually moves.
            if (percent > last_reported) {
                last_reported = percent;
                ctl.update_status(percent, status);
            }
            return true;
        },
        color));

    // Always finish at 100: this is what closes the notification. Reported even on cancel, where
    // build_texture_displacement() returns an empty mesh and finalize() commits nothing.
    ctl.update_status(100, status);
}

void TextureDisplacementBakeJob::finalize(bool canceled, std::exception_ptr &eptr)
{
    struct OnExit
    {
        std::function<void()> fn;
        ~OnExit() { if (fn) fn(); }
    } on_exit{m_on_finished};

    if (canceled || eptr || m_result.empty())
        return;

    Plater *plater = wxGetApp().plater();

    const auto commit = [this, plater]() {
        ModelVolume *volume = get_model_volume(m_input.volume_id, plater->model().objects);
        if (volume == nullptr)
            return;

        volume->set_mesh(std::move(m_result));
        volume->set_new_unique_id();
        volume->calculate_convex_hull();

        // Colour lands in mmu_segmentation_facets, merged *over* whatever is already painted there
        // rather than replacing it: a triangle the texture does not colour keeps its existing filament,
        // and one the user never painted at all stays at NONE, which already means "the volume's own
        // filament". That is what confines the effect to the painted area without having to invent a
        // colour for everything outside it. Safe to index straight onto the new mesh - the bake is
        // topology-preserving, so triangle i is still triangle i.
        if (!m_triangle_color.empty() && m_triangle_color.size() == volume->mesh().its.indices.size()) {
            TriangleSelector selector(volume->mesh());
            const TriangleSelector::TriangleSplittingData &existing = volume->mmu_segmentation_facets.get_data();
            if (!existing.bitstream.empty())
                selector.deserialize(existing, false);
            for (size_t i = 0; i < m_triangle_color.size(); ++i)
                if (m_triangle_color[i] > 0)
                    selector.set_facet(int(i), EnforcerBlockerType(m_triangle_color[i]));
            volume->mmu_segmentation_facets.set(selector);
        }

        // Clear the paint mask of every layer that was actually baked so a repeat bake (or the paint
        // overlay) doesn't act on triangles that no longer represent the same unbaked surface. The
        // texture layer definitions themselves (and paint outside the baked area, if any) are left
        // untouched so the user can keep sculpting with the same textures.
        for (const TextureDisplacementLayer &layer : m_input.layers)
            if (!layer.empty() && layer.slot >= 0 && layer.slot < int(TEXTURE_DISPLACEMENT_MAX_LAYERS))
                volume->texture_displacement_facet(layer.slot).reset();

        ModelObject *object = volume->get_object();
        if (object == nullptr)
            return;

        if (ObjectList *obj_list = wxGetApp().obj_list()) {
            const ModelObjectPtrs &objs = plater->model().objects;
            auto it = std::find(objs.begin(), objs.end(), object);
            if (it != objs.end())
                obj_list->update_info_items(size_t(it - objs.begin()));
        }

        plater->changed_object(*object);
    };

    // Standard mode's Bake is remesh -> subdivide -> displace under a single snapshot, and this job runs
    // long after that snapshot's scope has closed. Adding one here would put an undo step *between* the
    // subdivision and the displacement: the first Undo would land on a mesh carrying every added
    // triangle and no relief at all, and pressing Bake again from there would subdivide that mesh a
    // second time. So the caller says who owns the undo step.
    if (m_input.take_snapshot) {
        Plater::TakeSnapshot snapshot(plater, _u8L("Bake texture displacement"), UndoRedo::SnapshotType::GizmoAction);
        commit();
    } else {
        commit();
    }
}

void queue_texture_displacement_bake(const ModelVolume &volume, const TextureColorSettings &color,
                                     std::function<void()> on_finished, bool take_snapshot)
{
    TextureDisplacementBakeInput input;
    input.color         = color;
    input.take_snapshot = take_snapshot;
    input.volume_id  = volume.id();
    input.base_mesh  = volume.mesh().its;
    input.layers     = volume.texture_displacement_layers;
    input.options    = volume.texture_displacement_options;
    for (int i = 0; i < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++i)
        input.facets_data[size_t(i)] = volume.texture_displacement_facet(i).get_data();


    auto &worker = wxGetApp().plater()->get_ui_job_worker();
    queue_job(worker, std::make_unique<TextureDisplacementBakeJob>(std::move(input), std::move(on_finished)));
}

} // namespace Slic3r::GUI
