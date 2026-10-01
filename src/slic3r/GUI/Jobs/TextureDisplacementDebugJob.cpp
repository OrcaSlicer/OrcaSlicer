#include "TextureDisplacementDebugJob.hpp"

#include "libslic3r/Model.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoTextureDisplacement.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Plater.hpp"

namespace Slic3r::GUI {

TextureDisplacementDebugJob::TextureDisplacementDebugJob(
    TextureDisplacementDebugInput &&input, std::function<void(std::vector<BakeStageSnapshot>)> on_finished)
    : m_input(std::move(input)), m_on_finished(std::move(on_finished))
{
}

void TextureDisplacementDebugJob::process(Ctl &ctl)
{
    const std::string status = _u8L("Capturing bake stages");
    ctl.update_status(1, status);

    // Only ever touches m_input (captured by value before this job was queued) and local state - and,
    // unlike the bake job, never writes anything back either.
    int        last_reported = 1;
    const auto report        = [&ctl, &status, &last_reported](int percent) {
        if (ctl.was_canceled())
            return false;
        if (percent > last_reported) {
            last_reported = percent;
            ctl.update_status(percent, status);
        }
        return true;
    };

    BakeStageRecorder recorder;
    recorder.enable(true);
    recorder.set_check_topology(m_input.check_topology);
    recorder.set_mesh_cap(m_input.mesh_cap);

    indexed_triangle_set          mesh  = m_input.base_mesh;
    TextureDisplacementFacetsData masks = m_input.facets_data;

    // The default pipeline can only move vertices the mesh already has, so its recipe starts with the
    // preparation - which is where "first it remeshes, then it refines" actually happens. The
    // experimental pipeline refines as part of the bake and has nothing to prepare.
    if (m_input.run_prepare && !m_input.options.pipeline_v2) {
        const TextureDisplacementPrepareResult prepared =
            GLGizmoTextureDisplacement::prepare_mesh(mesh, masks, m_input.layers, m_input.prepare_params,
                                                     m_input.color.palette,
                                                     // Preparation is roughly half the run; the bake
                                                     // takes the progress bar from there.
                                                     [&report](int pct) { return report(1 + pct / 2); },
                                                     &recorder);
        if (ctl.was_canceled())
            return;
        // An empty result means nothing needed doing, which is not a failure - the bake below simply
        // runs on the mesh as it stands. paint_lost means the remesh dropped every layer's paint, so
        // there is nothing left to displace and the stages recorded so far are the whole story.
        if (prepared.paint_lost) {
            m_stages = recorder.take();
            ctl.update_status(100, status);
            return;
        }
        if (!prepared.mesh.indices.empty()) {
            mesh  = prepared.mesh;
            masks = prepared.masks;
        }
    }

    build_texture_displacement(mesh, m_input.layers, masks, m_input.options,
                               [&report](int pct) { return report(50 + pct / 2); },
                               /* color */ nullptr, m_input.volume_to_world, &recorder);

    m_stages = recorder.take();
    ctl.update_status(100, status); // always finish at 100: this is what closes the notification
}

void TextureDisplacementDebugJob::finalize(bool canceled, std::exception_ptr &eptr)
{
    if (!m_on_finished)
        return;
    // The handler must run on every outcome - the caller uses it to clear its in-progress latch, and
    // an empty vector is its signal that nothing usable came back.
    if (canceled || eptr)
        m_on_finished({});
    else
        m_on_finished(std::move(m_stages));
}

void queue_texture_displacement_debug(const ModelVolume &volume, const TextureColorSettings &color,
                                      const TextureDisplacementPrepareParams &prepare_params,
                                      bool run_prepare, bool check_topology,
                                      std::function<void(std::vector<BakeStageSnapshot>)> on_finished)
{
    TextureDisplacementDebugInput input;
    input.volume_id       = volume.id();
    input.base_mesh       = volume.mesh().its;
    input.layers          = volume.texture_displacement_layers;
    input.options         = volume.texture_displacement_options;
    input.prepare_params  = prepare_params;
    input.run_prepare     = run_prepare;
    input.check_topology  = check_topology;
    input.color           = color;
    input.volume_to_world = texture_displacement_volume_to_world(volume);
    for (int i = 0; i < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++i)
        input.facets_data[size_t(i)] = volume.texture_displacement_facet(i).get_data();

    auto &worker = wxGetApp().plater()->get_ui_job_worker();
    queue_job(worker, std::make_unique<TextureDisplacementDebugJob>(std::move(input), std::move(on_finished)));
}

} // namespace Slic3r::GUI
