#ifndef slic3r_TextureDisplacementPreviewJob_hpp_
#define slic3r_TextureDisplacementPreviewJob_hpp_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "libslic3r/Color.hpp"
#include "libslic3r/TextureDisplacement.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "Job.hpp"

namespace Slic3r::GUI {

// Everything process() needs, captured by value on the main thread when the job is queued -
// mirrors TextureDisplacementBakeInput, but a preview never writes back to the Model.
struct TextureDisplacementPreviewInput
{
    indexed_triangle_set                  base_mesh;
    std::vector<TextureDisplacementLayer> layers;
    TextureDisplacementFacetsData         facets_data;
    TextureDisplacementOptions            options;
    // Empty unless a layer is colouring, in which case the preview reports the filament per triangle
    // alongside the mesh, so the Normal view shows what the bake will produce - interleaving included.
    TextureColorSettings                  color;
};

// A preview result: the displaced mesh, and - when the input carried a palette - one filament index
// per triangle (an EnforcerBlockerType value; 0 means "no colour from the texture").
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
    TextureDisplacementPreviewJob(TextureDisplacementPreviewInput &&input, uint64_t generation,
                                   std::shared_ptr<const std::atomic<uint64_t>> current_generation,
                                   std::function<void(TextureDisplacementPreviewResult, uint64_t)> on_finished);

    void process(Ctl &ctl) override;
    void finalize(bool canceled, std::exception_ptr &eptr) override;

private:
    TextureDisplacementPreviewInput                    m_input;
    uint64_t                                            m_generation;
    std::shared_ptr<const std::atomic<uint64_t>>        m_current_generation;
    TextureDisplacementPreviewResult                    m_result;
    std::function<void(TextureDisplacementPreviewResult, uint64_t)> m_on_finished;
};

} // namespace Slic3r::GUI

#endif // slic3r_TextureDisplacementPreviewJob_hpp_
