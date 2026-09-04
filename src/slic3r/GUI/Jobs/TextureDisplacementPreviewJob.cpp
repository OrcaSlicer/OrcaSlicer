#include "TextureDisplacementPreviewJob.hpp"

#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoTextureDisplacement.hpp"

namespace Slic3r::GUI {

TextureDisplacementPreviewJob::TextureDisplacementPreviewJob(TextureDisplacementPreviewInput &&input, uint64_t generation,
                                                              std::shared_ptr<const std::atomic<uint64_t>> current_generation,
                                                              std::function<void(TextureDisplacementPreviewResult, uint64_t)> on_finished)
    : m_input(std::move(input)), m_generation(generation), m_current_generation(std::move(current_generation)),
      m_on_finished(std::move(on_finished))
{
}

void TextureDisplacementPreviewJob::process(Ctl &ctl)
{
    // No ctl.update_status() anywhere in here on purpose - see the class comment. A preview is
    // invisible bookkeeping; the only thing on screen should be the preview itself.

    // Only ever touches m_input (captured by value before this job was queued) and local state -
    // never the live Model - so this is safe to run concurrently with the UI thread.
    TextureColorRequest  color_request;
    TextureColorRequest *color = nullptr;
    if (!m_input.color.empty()) {
        color_request.quantize = GLGizmoTextureDisplacement::make_palette_quantizer(m_input.color.palette);
        color_request.resolve  = GLGizmoTextureDisplacement::make_mix_resolver(
            m_input.color.palette, m_input.color.mix_mode, m_input.color.layer_height,
            m_input.color.dither_cell_mm);
        color_request.despeckle_passes = m_input.color.despeckle_passes;
        color_request.out_triangle     = &m_result.triangle_color;
        if (color_request.quantize)
            color = &color_request;
    }

    m_result.mesh = build_texture_displacement(m_input.base_mesh, m_input.layers, m_input.facets_data,
                                          m_input.options,
                                          [this, &ctl](int) {
                                              // Bail the moment this preview stops being the current
                                              // one; build_texture_displacement() then returns an
                                              // empty mesh and finalize() drops it.
                                              return !ctl.was_canceled() &&
                                                     (!m_current_generation ||
                                                      m_current_generation->load() == m_generation);
                                          },
                                          color);
}

void TextureDisplacementPreviewJob::finalize(bool canceled, std::exception_ptr &eptr)
{
    if (!m_on_finished)
        return;
    // The handler must run on *every* outcome, cancellation included, because the caller uses it to
    // clear its "a job is in flight" latch. Returning early on `canceled` - which is what a cancel_all()
    // from Plater (project load/close, app exit) delivers - left that latch stuck true and no preview
    // was ever queued again for the rest of the session. An empty result is the caller's signal that
    // nothing usable came back; it already handles that.
    if (canceled || eptr)
        m_on_finished(TextureDisplacementPreviewResult{}, m_generation);
    else
        m_on_finished(std::move(m_result), m_generation);
}

} // namespace Slic3r::GUI
