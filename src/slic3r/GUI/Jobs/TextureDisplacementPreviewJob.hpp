#ifndef slic3r_TextureDisplacementPreviewJob_hpp_
#define slic3r_TextureDisplacementPreviewJob_hpp_

#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include "libslic3r/Point.hpp"
#include <memory>
#include <vector>

#include "libslic3r/TextureDisplacement.hpp"

#include "Job.hpp"

namespace Slic3r::GUI {

// One model part's share of a preview. Captured by value on the main thread when the job is queued -
// mirrors TextureDisplacementBakeVolume, but a preview never writes back to the Model.
struct TextureDisplacementPreviewVolume
{
    indexed_triangle_set                  base_mesh;
    std::vector<TextureDisplacementLayer> layers;
    TextureDisplacementFacetsData         facets_data;
    TextureDisplacementOptions            options;
    // Mesh coordinates -> world millimetres, so the preview is displaced in the same space the bake
    // is and the two cannot disagree. See build_texture_displacement().
    Transform3d                           volume_to_world = Transform3d::Identity();
    // The part's own UV channel (ModelVolume::texture_uv), so a ModelUV layer previews through the
    // same layout it will bake through. Empty when the part has none.
    std::vector<Vec2f>                    model_uv;
    // Where the part sits in its object, as TextureDisplacementBakeVolume has it.
    Vec3d                                 texture_anchor = Vec3d::Zero();
    float                                 resting_drop   = 0.f;
};

struct TextureDisplacementPreviewInput
{
    // The same parts the bake would displace, in the object's own volume order, so the preview shows
    // the whole of what Bake will do on a multibody object rather than just its first part.
    std::vector<TextureDisplacementPreviewVolume> volumes;
    // Empty unless a layer is colouring, in which case the preview reports the palette entry per
    // triangle alongside the mesh, so the Normal view shows the colours the bake will produce.
    TextureColorSettings                          color;
};

// A preview result for one part: the displaced mesh, and - when the input carried a palette - one
// palette entry per triangle (its index + 1; 0 means "no colour from the texture").
struct TextureDisplacementPreviewResult
{
    indexed_triangle_set  mesh;
    std::vector<uint8_t>  triangle_color;
};

// Computes the true (unbaked) displaced-mesh preview in the background. With several painted
// layers this is real, non-trivial CPU work (PNG sampling, per-layer vertex welding), which used
// to run synchronously on every paint stroke and parameter tweak and made editing feel slow with
// more than one or two layers. Unlike Bake, this never touches the live Model - a preview is
// purely informational, there is nothing to commit.
//
// Deliberately reports no status: the Job framework turns the first update_status() call into an
// on-screen progress notification, and a preview firing one on every stroke and slider release
// buried the user in notifications that only closed at 100%.
class TextureDisplacementPreviewJob : public Job
{
public:
    // `generation` is an opaque token the caller controls (typically an incrementing counter):
    // on_finished should only actually be applied by the caller if it still matches the caller's
    // current generation when the job completes, so that a burst of edits queuing several of
    // these jobs in a row can't have an earlier, now-stale result clobber a later one that
    // finishes first.
    //
    // `current_generation` is the caller's live counter, shared with the worker thread. The job
    // polls it *while computing* and aborts as soon as it no longer matches - so a preview that has
    // already been superseded stops burning CPU instead of running to completion for a result that
    // will only be thrown away. That matters because the UI job worker runs one job at a time in FIFO
    // order: without it, a Bake queued behind a handful of stale previews waits for every one of them.
    //
    // `on_finished` receives one result per input part, at the same index - or an empty vector when
    // the run was superseded, cancelled or threw.
    TextureDisplacementPreviewJob(TextureDisplacementPreviewInput &&input, uint64_t generation,
                                   std::shared_ptr<const std::atomic<uint64_t>> current_generation,
                                   std::function<void(std::vector<TextureDisplacementPreviewResult>, uint64_t)> on_finished);

    void process(Ctl &ctl) override;
    void finalize(bool canceled, std::exception_ptr &eptr) override;

private:
    TextureDisplacementPreviewInput                    m_input;
    uint64_t                                            m_generation;
    std::shared_ptr<const std::atomic<uint64_t>>        m_current_generation;
    std::vector<TextureDisplacementPreviewResult>       m_results;
    std::function<void(std::vector<TextureDisplacementPreviewResult>, uint64_t)> m_on_finished;
};

} // namespace Slic3r::GUI

#endif // slic3r_TextureDisplacementPreviewJob_hpp_
