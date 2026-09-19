#ifndef slic3r_TextureDisplacementDebugJob_hpp_
#define slic3r_TextureDisplacementDebugJob_hpp_

#include <functional>
#include <vector>

#include "libslic3r/ObjectID.hpp"
#include "libslic3r/TextureBake/TextureBakeDebug.hpp"
#include "libslic3r/TextureDisplacement.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "Job.hpp"

namespace Slic3r {
class ModelVolume;
}

namespace Slic3r::GUI {

// Runs a bake with stage capture on and commits nothing.
//
// A bake is a chain of stages that each rewrite the whole mesh - remesh, refine, displace, smooth, or
// for the experimental pipeline subdivide, regularize, relocate, displace, decimate, repair - and when
// the result looks wrong the only useful question is which of them made it wrong. This runs the same
// recipe the Bake button runs, keeps every intermediate mesh, and hands them back for the gizmo to
// step through.
//
// Deliberately separate from TextureDisplacementBakeJob rather than a flag on it: this must never
// touch the Model, and keeping the committing path free of a "but not this time" branch is what
// guarantees that.
struct TextureDisplacementDebugInput
{
    ObjectID                              volume_id;
    indexed_triangle_set                  base_mesh;
    std::vector<TextureDisplacementLayer> layers;
    TextureDisplacementFacetsData         facets_data;
    TextureDisplacementOptions            options;
    // The preparation stages - remesh and adaptive refinement. Only the default pipeline uses them;
    // the experimental one refines as part of the bake and ignores this entirely.
    TextureDisplacementPrepareParams      prepare_params;
    bool                                  run_prepare = true;
    // Mesh coordinates -> world millimetres, as the bake gets it. The captured stages are brought
    // back into mesh coordinates before they are handed over, so the gizmo can draw them directly.
    Transform3d                           volume_to_world = Transform3d::Identity();
    // Only the quantizer is used, and only by the refinement's colour criterion - the debug run
    // produces no colour of its own.
    TextureColorSettings                  color;

    // The edge scan behind the open / non-manifold counts is a sort over every half-edge, which on a
    // multi-million triangle stage costs more than the stage did. On by default because a stage that
    // tore the mesh is exactly what this exists to find.
    bool   check_topology = true;
    // Stages above this keep their counts but not their geometry - see BakeStageRecorder.
    size_t mesh_cap = 4'000'000;
};

class TextureDisplacementDebugJob : public Job
{
public:
    TextureDisplacementDebugJob(TextureDisplacementDebugInput                       &&input,
                                std::function<void(std::vector<BakeStageSnapshot>)>   on_finished);

    void process(Ctl &ctl) override;
    void finalize(bool canceled, std::exception_ptr &eptr) override;

private:
    TextureDisplacementDebugInput                       m_input;
    std::vector<BakeStageSnapshot>                      m_stages;
    std::function<void(std::vector<BakeStageSnapshot>)> m_on_finished;
};

// Captures `volume`'s current mesh, layers and paint and queues one debug run. `on_finished` always
// runs exactly once on the UI thread, with an empty vector if the run was cancelled or threw, so the
// caller can clear its own in-progress state. Must be called from the main thread.
void queue_texture_displacement_debug(const ModelVolume &volume, const TextureColorSettings &color,
                                      const TextureDisplacementPrepareParams &prepare_params,
                                      bool run_prepare, bool check_topology,
                                      std::function<void(std::vector<BakeStageSnapshot>)> on_finished);

} // namespace Slic3r::GUI

#endif // slic3r_TextureDisplacementDebugJob_hpp_
