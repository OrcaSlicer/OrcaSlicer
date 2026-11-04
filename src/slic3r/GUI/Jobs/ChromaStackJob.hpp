#ifndef slic3r_ChromaStackJob_hpp_
#define slic3r_ChromaStackJob_hpp_

#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <vector>

#include "libslic3r/ChromaDiagnostics.hpp"
#include "libslic3r/ChromaStack.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TextureDisplacement.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include "Job.hpp"

namespace Slic3r::GUI {

// The two pieces of a ChromaStack rebuild that are worth their own thread. Both used to run inside
// the gizmo's ImGui frame, which is why moving a slider froze the window for as long as the picture
// was large: a solve is a nearest-colour search over every texel of the source, and a plate is a
// constrained Delaunay over up to 120 k points.
//
// Both follow TextureDisplacementPreviewJob exactly: the input is captured by value on the main
// thread, process() touches nothing but that copy, and the generation token lets a superseded run
// abort mid-computation rather than finish a result nobody will use. That matters more here than it
// does for the preview, because the UI worker is a single FIFO thread - without it, a burst of
// keystrokes in the Size field would queue up and each one would run to completion in turn.

// ---- The plate ----------------------------------------------------------------------------------

// Rebuilding the plate is two very different costs: the triangulation, which is pure arithmetic and
// belongs here, and swapping the mesh into the live volume, which touches the Model and the scene and
// can only happen on the main thread. Only the first is in process(); the caller does the second in
// its completion handler.
struct ChromaPlateInput
{
    ChromaPlate plate;
};

struct ChromaPlateResult
{
    ChromaPlateMesh mesh;
};

class ChromaPlateJob : public Job
{
public:
    ChromaPlateJob(ChromaPlateInput &&input, uint64_t generation,
                   std::shared_ptr<const std::atomic<uint64_t>>           current_generation,
                   std::function<void(ChromaPlateResult, uint64_t)>       on_finished);

    void process(Ctl &ctl) override;
    void finalize(bool canceled, std::exception_ptr &eptr) override;

private:
    ChromaPlateInput                                m_input;
    uint64_t                                        m_generation;
    std::shared_ptr<const std::atomic<uint64_t>>    m_current_generation;
    ChromaPlateResult                               m_result;
    std::function<void(ChromaPlateResult, uint64_t)> m_on_finished;
};

// ---- The solve ----------------------------------------------------------------------------------

// Everything chroma_solve_layer() used to do on the UI thread: decode the picture, measure how the
// painted patch is tilted, choose a column height per texel, deepen the map where the surface is not
// level, and encode the result as the PNG the displacement reads.
//
// The patch is extracted here rather than handed in because extracting it is itself a pass over the
// mesh. `base_mesh` and `facet_data` are the volume's, copied at queue time - the same thing the
// preview job captures, and for the same reason.
struct ChromaSolveInput
{
    // A full copy of the layer: the source bytes, the placement the picture is sampled through, and
    // the stack to solve against. Copied rather than referenced because the panel keeps editing the
    // live one while this runs.
    TextureDisplacementLayer                 layer;

    indexed_triangle_set                     base_mesh;
    TriangleSelector::TriangleSplittingData  facet_data;

    // The two frames chroma_patch() measures in: the instance's, which is where print Z is, and the
    // bake's, which is where the picture lands. See GLGizmoTextureDisplacement::chroma_patch().
    Transform3d                              trafo             = Transform3d::Identity();
    Transform3d                              bake              = Transform3d::Identity();
    double                                   instance_bottom_z = 0.;

    // Recompute the base from the patch before solving, when the panel's padlock is closed.
    bool                                     base_auto         = false;

    // The longest side the solve may work at, in texels; 0 means the picture's own resolution. The
    // height map drives a mesh refined to a fraction of a millimetre, so solving a 12 MP photograph
    // texel for texel buys nothing a bake cannot do once, at the end.
    int                                      max_solve_px      = 0;

    // How wide the picture is on the print, in millimetres, or 0 when nothing has measured it. The
    // solve needs it to size a dither cell: a dithered column narrower than an extrusion is dropped by
    // the slicer, so the cell is set in texels of whatever grid this run works on, and only the caller
    // knows how many millimetres a texel of that grid is. See chroma_run_solve().
    float                                    picture_mm        = 0.f;
    // The extrusion width the relief's steps are drawn with, in millimetres, or 0 for "do not bother":
    // the dither then works per texel, as it did before any of this.
    float                                    line_width_mm     = 0.f;
    // CIEDE2000 instead of CIE76 for the per-pixel match. For the final pass, whose height map is what
    // gets printed; the interactive one stays on the cheaper metric, where the difference against a
    // one-dimensional palette is hard to see and the cost is easy to feel.
    bool                                     high_quality      = false;
    // How many layers a lone texel has to stand above its neighbours before the solve pulls it down,
    // or 0 to leave them. A per-run option rather than part of the stack, like the dither cell: it is
    // a property of the printer the relief is going to, not of the picture.
    int                                      despike_layers    = 0;
};

struct ChromaSolveResult
{
    bool                                        ok           = false;
    std::shared_ptr<std::vector<unsigned char>> png;
    float                                       depth_mm     = 0.f;
    // Whether the picture is flat artwork rather than a photograph, and the evidence for saying so.
    // Decided here because this is where the picture exists at its own resolution; the card only
    // reports it, and the dither the solve already switched off is the thing it explains.
    ChromaPaletteReport                         palette;
    // Why the solve came out the way it did: the picture's colours and the stack's reach in one space,
    // plus the ranked findings. Computed here because this is where the picture exists at the
    // resolution the solve saw, and carried back rather than recomputed, since asking the question a
    // second time would mean walking every pixel a second time.
    ChromaDiagnostics                           diagnostics;
    // -1 leaves the caller's own value alone; anything else is what the patch measured.
    int                                         base_layers  = -1;
    // The predicted print, for the card's second thumbnail. Uploaded by the caller, since a GL
    // texture can only be created on the thread that owns the context.
    DecodedHeightTexture                        preview;
    // How far the stack landed from the picture at every pixel, one byte each, on the same grid as
    // `preview` (see CHROMA_ERROR_IMAGE_FULL_SCALE_DE for the scale). The card shows it on demand and
    // the overlay shader draws it over the model, which is the only honest answer to "which parts of
    // my picture is this stack getting wrong".
    std::vector<uint8_t>                        delta;
    double                                      mean_delta_e = 0.;
    double                                      max_delta_e  = 0.;
    // The column's printed layer count per pixel, on the same grid as `preview`, exactly as the solve
    // produced it. The height map above is the same thing quantised to a byte and then reworked for
    // the surface it is displaced onto - deepened for tilt, complemented face down - so it is the
    // wrong thing to rebuild levels from. Carried because splitting a stack into parts
    // (chroma_split_bands()) is arithmetic on layer counts and nothing else will do.
    std::vector<uint16_t>                       levels;
};

// The solve itself, with "has this been superseded?" left to the caller - which is what lets one
// description of it serve both the worker and the bake's synchronous full-resolution pass. Touches no
// Model and no GL state; `superseded` may be empty, and is polled between stages. Note that it
// updates `input.layer.chroma.base_layers` in place when the input asked for the base to be measured.
ChromaSolveResult chroma_run_solve(ChromaSolveInput &input, const std::function<bool()> &superseded);

class ChromaSolveJob : public Job
{
public:
    ChromaSolveJob(ChromaSolveInput &&input, uint64_t generation,
                   std::shared_ptr<const std::atomic<uint64_t>>     current_generation,
                   std::function<void(ChromaSolveResult, uint64_t)> on_finished);

    void process(Ctl &ctl) override;
    void finalize(bool canceled, std::exception_ptr &eptr) override;

private:
    ChromaSolveInput                                m_input;
    uint64_t                                        m_generation;
    std::shared_ptr<const std::atomic<uint64_t>>    m_current_generation;
    ChromaSolveResult                               m_result;
    std::function<void(ChromaSolveResult, uint64_t)> m_on_finished;
};

} // namespace Slic3r::GUI

#endif // slic3r_ChromaStackJob_hpp_
