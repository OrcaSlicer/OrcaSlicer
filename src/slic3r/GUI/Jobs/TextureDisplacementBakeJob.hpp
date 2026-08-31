#ifndef slic3r_TextureDisplacementBakeJob_hpp_
#define slic3r_TextureDisplacementBakeJob_hpp_

#include <functional>
#include <vector>

#include "libslic3r/Color.hpp"
#include "libslic3r/ObjectID.hpp"
#include "libslic3r/TextureDisplacement.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "Job.hpp"

namespace Slic3r::GUI {

// Everything process() needs, captured by value on the main thread when the job is queued so the
// worker thread never touches the live Model concurrently with the UI (mirrors how EmbossJob's
// DataBase is captured before process() runs).
struct TextureDisplacementBakeInput
{
    ObjectID                              volume_id;
    indexed_triangle_set                  base_mesh;
    std::vector<TextureDisplacementLayer> layers;
    TextureDisplacementFacetsData         facets_data;
    TextureDisplacementOptions            options;
    // Captured on the main thread. Empty unless some layer is colouring, in which case the bake also
    // writes the volume's mmu_segmentation_facets - the same per-triangle filament assignment the MMU
    // paint gizmo writes - alongside the displaced geometry.
    TextureColorSettings                  color;
    // Whether this job pushes its own undo step when it commits. False when the caller has already
    // taken one that is meant to cover the displacement as well - Standard mode's Bake, which remeshes
    // and subdivides first and has to undo as a single action.
    bool                                  take_snapshot = true;
};

// Bakes a volume's painted texture-displacement layers into real mesh geometry in the background,
// then commits the result on the main thread - mirrors EmbossJob's UpdateJob/update_volume()
// bake-and-commit pattern (see EmbossJob.cpp).
class TextureDisplacementBakeJob : public Job
{
public:
    TextureDisplacementBakeJob(TextureDisplacementBakeInput &&input, std::function<void()> on_finished);

    void process(Ctl &ctl) override;
    void finalize(bool canceled, std::exception_ptr &eptr) override;

private:
    TextureDisplacementBakeInput m_input;
    TriangleMesh                 m_result;
    // Per triangle of m_result: the filament to print it in, as an EnforcerBlockerType value
    // (0 = leave alone). Empty unless a layer asked for colour. See TextureColorRequest.
    std::vector<uint8_t>         m_triangle_color;
    std::function<void()>        m_on_finished;
};

// Captures `volume`'s current mesh/layers/paint data and queues a TextureDisplacementBakeJob on
// the app's UI job worker. `on_finished` is always called once the job settles (success, failure,
// or cancellation), so the caller can clear its own "bake in progress" UI state. Must be called
// from the main thread.
void queue_texture_displacement_bake(const ModelVolume &volume, const TextureColorSettings &color,
                                     std::function<void()> on_finished, bool take_snapshot = true);

} // namespace Slic3r::GUI

#endif // slic3r_TextureDisplacementBakeJob_hpp_
