#include "TextureDisplacementPreviewJob.hpp"

#include "slic3r/GUI/Gizmos/GLGizmoTextureDisplacement.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <atomic>
#include <functional>
#include <utility>
#include <vector>
#include "libslic3r/TextureDisplacement.hpp"
#include <exception>

namespace Slic3r::GUI {

TextureDisplacementPreviewJob::TextureDisplacementPreviewJob(
    TextureDisplacementPreviewInput &&input, uint64_t generation,
    std::shared_ptr<const std::atomic<uint64_t>> current_generation,
    std::function<void(std::vector<TextureDisplacementPreviewResult>, uint64_t)> on_finished)
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
    //
    // The quantizers are built once for the whole run: the palette is the object's, not the part's,
    // and each one fills a colour lookup cube that costs tens of milliseconds.
    TextureColorRequest  color_request;
    TextureColorRequest *color = nullptr;
    if (!m_input.color.empty()) {
        color_request.quantize = GLGizmoTextureDisplacement::make_palette_quantizer(m_input.color.palette);
        if (!m_input.color.palette_pure.empty())
            color_request.quantize_pure = GLGizmoTextureDisplacement::make_palette_quantizer(m_input.color.palette_pure);
        color_request.despeckle_passes = m_input.color.despeckle_passes;
        if (color_request.quantize)
            color = &color_request;
    }

    // Bail the moment this preview stops being the current one: build_texture_displacement() then
    // returns an empty mesh, and finalize() drops the whole run rather than showing half of it.
    const auto still_wanted = [this, &ctl](int) {
        return !ctl.was_canceled() &&
               (!m_current_generation || m_current_generation->load() == m_generation);
    };

    m_results.resize(m_input.volumes.size());
    for (size_t i = 0; i < m_input.volumes.size(); ++i) {
        const TextureDisplacementPreviewVolume &volume = m_input.volumes[i];
        if (color != nullptr)
            color_request.out_triangle = &m_results[i].triangle_color;
        m_results[i].mesh = build_texture_displacement(volume.base_mesh, volume.layers, volume.facets_data,
                                                       volume.options, still_wanted, color, volume.volume_to_world,
                                                       nullptr, nullptr,
                                                       volume.model_uv.empty() ? nullptr : &volume.model_uv,
                                                       &volume.texture_anchor, volume.resting_drop);
        if (!still_wanted(0)) {
            m_results.clear();
            return;
        }
    }
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
        m_on_finished({}, m_generation);
    else
        m_on_finished(std::move(m_results), m_generation);
}

} // namespace Slic3r::GUI
