#ifndef slic3r_TextureDisplacementBakeJob_hpp_
#define slic3r_TextureDisplacementBakeJob_hpp_

#include <exception>
#include <cstdint>
#include <functional>
#include "libslic3r/Point.hpp"
#include <vector>

#include "libslic3r/ObjectID.hpp"
#include "libslic3r/TextureDisplacement.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "Job.hpp"

namespace Slic3r { class ModelVolume; }

namespace Slic3r::GUI {

// One model part's share of a bake: its own mesh, its own paint masks and UV channel, and the layer
// stack to displace it by. Captured by value on the main thread when the job is queued so the worker
// thread never touches the live Model concurrently with the UI (mirrors how EmbossJob's DataBase is
// captured before process() runs).
struct TextureDisplacementBakeVolume
{
    ObjectID                              volume_id;
    indexed_triangle_set                  base_mesh;
    std::vector<TextureDisplacementLayer> layers;
    TextureDisplacementFacetsData         facets_data;
    TextureDisplacementOptions            options;
    // Mesh coordinates -> world millimetres. Captured here with everything else so the worker never
    // reaches back into the live Model for it. See build_texture_displacement().
    Transform3d                           volume_to_world = Transform3d::Identity();
    // The part's own UV channel (ModelVolume::texture_uv), empty when it has none: what a ModelUV
    // layer maps through, and what the commit puts back on the baked mesh.
    std::vector<Vec2f>                    model_uv;
    // Where the part sits in its object: the world point the object's texture is anchored at, and how
    // far the object's lowest point lies below the part's own. See build_texture_displacement().
    Vec3d                                 texture_anchor = Vec3d::Zero();
    float                                 resting_drop   = 0.f;
};

struct TextureDisplacementBakeInput
{
    // Every model part the bake acts on, in the object's own volume order. An object made of several
    // parts carries one entry per part the paint actually reaches: the layer stack is the object's,
    // but each part is displaced through its own mesh, its own masks and its own UV channel. Baking
    // only the first part is what made texture displacement silently ignore every other body of a
    // multibody object.
    std::vector<TextureDisplacementBakeVolume> volumes;
    // Captured on the main thread, and shared by every part: the palette comes from the project's
    // filaments and from the layer stack, neither of which is per part. Empty unless some layer is
    // colouring, in which case the bake also writes each part's mmu_segmentation_facets - the same
    // per-triangle filament assignment the MMU paint gizmo writes - alongside the displaced geometry.
    TextureColorSettings                       color;
    // Whether this job pushes its own undo step when it commits. False when the caller has already
    // taken one that is meant to cover the displacement as well - Standard mode's Bake, which remeshes
    // and subdivides first and has to undo as a single action.
    bool                                       take_snapshot = true;
};

// Bakes an object's painted texture-displacement layers into real mesh geometry in the background,
// then commits the result on the main thread - mirrors EmbossJob's UpdateJob/update_volume()
// bake-and-commit pattern (see EmbossJob.cpp). Every part is displaced in the one job and committed
// under the one undo step, so a multibody object undoes as a single action.
class TextureDisplacementBakeJob : public Job
{
public:
    TextureDisplacementBakeJob(TextureDisplacementBakeInput &&input, std::function<void()> on_finished);

    void process(Ctl &ctl) override;
    void finalize(bool canceled, std::exception_ptr &eptr) override;

private:
    // What the bake produced for one part of m_input.volumes, at the same index.
    struct VolumeResult
    {
        TriangleMesh         mesh;
        // Per triangle of `mesh`: the palette entry to print it in, as its index + 1 (0 = leave
        // alone). Empty unless a layer asked for colour. See TextureColorRequest; finalize() turns
        // these into filaments.
        std::vector<uint8_t> triangle_color;
        // What the bake spent, for the message it leaves behind when the budget capped the detail.
        TextureBakeStats     stats;
    };

    TextureDisplacementBakeInput m_input;
    std::vector<VolumeResult>    m_results;
    std::function<void()>        m_on_finished;
};

// Captures each of `volumes`' current mesh/layers/paint data and queues a single
// TextureDisplacementBakeJob on the app's UI job worker. `on_finished` is always called once the job
// settles (success, failure, or cancellation), so the caller can clear its own "bake in progress" UI
// state. Must be called from the main thread.
void queue_texture_displacement_bake(std::vector<TextureDisplacementBakeVolume> volumes,
                                     const TextureColorSettings &color,
                                     std::function<void()> on_finished, bool take_snapshot = true);

// `volume`'s share of a bake, read off the live Model. `layers`, `options`, `texture_anchor` and
// `resting_drop` are passed in rather than taken from `volume` because they belong to the object, not
// to the part: every part of a multibody object is displaced by the same stack, in the same texture
// frame and against the same plate (see GLGizmoTextureDisplacement::layers_for_part()).
TextureDisplacementBakeVolume texture_displacement_bake_volume(const ModelVolume                           &volume,
                                                               const std::vector<TextureDisplacementLayer> &layers,
                                                               const TextureDisplacementOptions            &options,
                                                               const Vec3d                                 &texture_anchor,
                                                               float                                        resting_drop);

} // namespace Slic3r::GUI

#endif // slic3r_TextureDisplacementBakeJob_hpp_
