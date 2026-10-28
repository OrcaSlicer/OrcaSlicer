#include "ChromaStackJob.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <utility>
#include <vector>

#include "libslic3r/ChromaStack.hpp"
#include "libslic3r/TextureDisplacement.hpp"

namespace Slic3r::GUI {

// ---- The plate ----------------------------------------------------------------------------------

ChromaPlateJob::ChromaPlateJob(ChromaPlateInput &&input, uint64_t generation,
                               std::shared_ptr<const std::atomic<uint64_t>>     current_generation,
                               std::function<void(ChromaPlateResult, uint64_t)> on_finished)
    : m_input(std::move(input)), m_generation(generation), m_current_generation(std::move(current_generation)),
      m_on_finished(std::move(on_finished))
{
}

void ChromaPlateJob::process(Ctl &ctl)
{
    // Nothing to poll inside: chroma_plate_mesh() is one call into a constrained Delaunay and takes
    // no abort callback. The check here still earns its place - a burst of keystrokes in the Size
    // field queues several of these, and all but the last can be dropped before they start.
    if (ctl.was_canceled() || (m_current_generation && m_current_generation->load() != m_generation))
        return;
    m_result.mesh = chroma_plate_mesh(m_input.plate);
}

void ChromaPlateJob::finalize(bool canceled, std::exception_ptr &eptr)
{
    if (!m_on_finished)
        return;
    // As with the preview job, the handler runs on every outcome so the caller's in-flight latch
    // always clears. An empty mesh is its signal that nothing usable came back.
    if (canceled || eptr)
        m_on_finished(ChromaPlateResult{}, m_generation);
    else
        m_on_finished(std::move(m_result), m_generation);
}

// ---- The solve ----------------------------------------------------------------------------------

namespace {
// Box-filtered down to `max_px` on the longer side. The solve's output drives a mesh refined to a
// fraction of a millimetre, so a photograph's own resolution is far past the point where another
// texel changes any vertex; averaging rather than dropping texels keeps a fine pattern from aliasing
// into noise on the way down. Returns false when the picture is already small enough.
//
// `palette_exact` switches that averaging off, for the one kind of picture it ruins. Flat artwork - a
// logo, line art, a flag - is a handful of fills, and the solve matches every texel to the nearest
// colour the stack can print at some height. Averaging across an edge invents a colour that is in
// neither fill, the match sends it to a height between the two, and a hard edge prints as a soft
// terraced ramp. Taking the texel at the centre of each block instead keeps the result inside the
// source's own set of colours, which is what the flag promises. Nearest-neighbour on a photograph
// would alias visibly, which is why this is keyed off the decode's judgement rather than offered as a
// choice.
bool downsample_rgb(const std::vector<uint8_t> &src, int src_w, int src_h, int max_px, bool palette_exact,
                    std::vector<uint8_t> &dst, int &dst_w, int &dst_h)
{
    const int longest = std::max(src_w, src_h);
    if (max_px <= 0 || longest <= max_px || src.size() < size_t(src_w) * size_t(src_h) * 3)
        return false;

    dst_w = std::max(1, int(std::lround(double(src_w) * double(max_px) / double(longest))));
    dst_h = std::max(1, int(std::lround(double(src_h) * double(max_px) / double(longest))));
    dst.resize(size_t(dst_w) * size_t(dst_h) * 3);

    if (palette_exact) {
        for (int y = 0; y < dst_h; ++y) {
            // The centre of the block this destination texel stands for, so the sampling grid does not
            // drift towards one edge of the picture the way taking the first texel of each block does.
            const int sy = std::min(src_h - 1, int((int64_t(y) * 2 + 1) * src_h / (int64_t(dst_h) * 2)));
            for (int x = 0; x < dst_w; ++x) {
                const int    sx = std::min(src_w - 1, int((int64_t(x) * 2 + 1) * src_w / (int64_t(dst_w) * 2)));
                const size_t s  = (size_t(sy) * size_t(src_w) + size_t(sx)) * 3;
                const size_t d  = (size_t(y) * size_t(dst_w) + size_t(x)) * 3;
                dst[d]          = src[s];
                dst[d + 1]      = src[s + 1];
                dst[d + 2]      = src[s + 2];
            }
        }
        return true;
    }

    for (int y = 0; y < dst_h; ++y) {
        const int y0 = int(int64_t(y) * src_h / dst_h);
        const int y1 = std::max(y0 + 1, int(int64_t(y + 1) * src_h / dst_h));
        for (int x = 0; x < dst_w; ++x) {
            const int x0 = int(int64_t(x) * src_w / dst_w);
            const int x1 = std::max(x0 + 1, int(int64_t(x + 1) * src_w / dst_w));
            uint32_t  sum[3]{};
            uint32_t  n = 0;
            for (int sy = y0; sy < src_h && sy < y1; ++sy)
                for (int sx = x0; sx < src_w && sx < x1; ++sx) {
                    const size_t s = (size_t(sy) * size_t(src_w) + size_t(sx)) * 3;
                    sum[0] += src[s];
                    sum[1] += src[s + 1];
                    sum[2] += src[s + 2];
                    ++n;
                }
            const size_t d = (size_t(y) * size_t(dst_w) + size_t(x)) * 3;
            for (int c = 0; c < 3; ++c)
                dst[d + c] = uint8_t(n == 0 ? 0 : sum[c] / n);
        }
    }
    return true;
}
} // namespace

ChromaSolveResult chroma_run_solve(ChromaSolveInput &input, const std::function<bool()> &superseded)
{
    ChromaSolveResult         result;
    TextureDisplacementLayer &layer = input.layer;
    if (!layer.chroma_source || layer.chroma.bands.empty() || layer.chroma.relief_layers() <= 0)
        return result;
    const auto stop = [&superseded]() { return superseded && superseded(); };
    if (stop())
        return result;

    // Decoding goes through the same cache as a layer's own texture: the probe carries nothing but
    // the image, and the cache is keyed by that buffer, so re-solving never decodes the PNG twice.
    // The cache guards itself with a mutex, which is what makes this safe from a worker.
    TextureDisplacementLayer probe;
    probe.image_data = layer.chroma_source;
    const DecodedHeightTexture source = decode_height_texture(probe);
    if (source.empty() || stop())
        return result;

    // A grayscale picture is a legitimate input - a tonal relief in two filaments - and the solve only
    // ever reads RGB, so grey is expanded rather than rejected.
    std::vector<uint8_t> grey_rgb;
    if (!source.has_color()) {
        grey_rgb.resize(source.pixels.size() * 3);
        for (size_t i = 0; i < source.pixels.size(); ++i)
            grey_rgb[i * 3] = grey_rgb[i * 3 + 1] = grey_rgb[i * 3 + 2] = source.pixels[i];
    }
    const std::vector<uint8_t> &full = source.has_color() ? source.rgb : grey_rgb;

    // The aspect is the picture's own, and it is what the placement was fitted against, so it comes
    // from the full-resolution decode rather than from whatever grid the solve ends up running on.
    const float aspect = (source.width > 0 && source.height > 0) ?
                             float(source.width) / float(source.height) : 1.f;

    // Flat artwork never wants dithering, whatever the stack says. Error diffusion trades the banding a
    // photograph shows across a sky for fine stipple, which at a tenth of a millimetre per pixel reads
    // as the intermediate colour; on a logo there is no banding to trade away and the stipple is simply
    // speckle along every edge. The decode already knows which kind of picture this is, so the card's
    // own Dither setting is left alone and overridden here for this solve only - the layer is a copy the
    // job owns, and the user's choice stays theirs for the day they point it at a photograph.
    if (source.palette_exact)
        layer.chroma.dither = false;

    const uint8_t       *rgb    = full.data();
    int                  width  = source.width;
    int                  height = source.height;
    std::vector<uint8_t> reduced;
    if (int rw = 0, rh = 0; downsample_rgb(full, source.width, source.height, input.max_solve_px,
                                           source.palette_exact, reduced, rw, rh)) {
        rgb    = reduced.data();
        width  = rw;
        height = rh;
    }
    if (stop())
        return result;

    // The patch the picture is printed on, and how far from level it is. Both are pure functions of
    // the mesh snapshot taken when this was queued, which is what keeps the Model out of here.
    const indexed_triangle_set patch = extract_painted_patch(input.base_mesh, input.facet_data);
    if (stop())
        return result;

    const ChromaPatchMetrics metrics = chroma_measure_patch(patch, input.trafo, input.bake,
                                                            input.instance_bottom_z);
    // The panel keeps the base up to date while it is open, but a solve is the point at which being
    // wrong stops being recoverable: it is what writes the height map every later step reads.
    if (input.base_auto && metrics.valid && layer.chroma.layer_height_mm > 0.f) {
        layer.chroma.base_layers = std::max(1, int(std::lround(metrics.top_mm / layer.chroma.layer_height_mm)));
        result.base_layers       = layer.chroma.base_layers;
    }

    // A level patch needs no tilt correction, and building its surface is a rasterisation of the whole
    // picture that can only conclude "nothing to do". A generated plate's picture face is flat by
    // construction, so this skips the common case outright. The threshold is well under the half
    // degree at which chroma_tilt_relief()'s own correction first exceeds the encoding's step.
    ChromaSurface surface;
    if (metrics.valid && metrics.tilt_deg > 0.5f)
        surface = chroma_build_surface(patch, input.trafo, input.bake, layer, aspect, width, height);
    if (stop())
        return result;

    // The dither cell, in texels of the grid this run actually works on. A dithered single-texel column
    // is a lie: anything narrower than an extrusion is dropped by the slicer, so the preview would
    // promise a colour the print cannot make. The grid matters because the interactive run is reduced
    // to max_solve_px while the final one is the picture's own size, so the same millimetre is a
    // different number of texels in each.
    ChromaSolveOptions options;
    options.high_quality = input.high_quality;
    if (input.picture_mm > 0.f && input.line_width_mm > 0.f && width > 0) {
        const float mm_per_texel = input.picture_mm / float(width);
        if (mm_per_texel > 0.f)
            options.dither_cell_px = std::max(1, int(std::ceil(input.line_width_mm / mm_per_texel)));
    }

    ChromaSolve solve = chroma_solve(rgb, width, height, layer.chroma, options);
    if (solve.empty() || stop())
        return result;

    // A relief of d mm along a tilted surface's normal only raises the print by d*cos(tilt), so on
    // anything but a level face the steps would land below the heights their colours were solved for.
    // Deepening the map texel by texel puts them back; the depth it asks for is what the layer then
    // carries. A level patch comes back untouched.
    const float depth_mm = chroma_tilt_relief(solve, layer.chroma, surface);

    // Printed picture-side down, the relief grows into the plate rather than out of it: the column that
    // is tallest in the print is the one that reaches furthest *up* from the face lying on the build
    // plate, so the height map is complemented. Done here rather than in the solve because it is a
    // property of how the plate is printed, not of how the picture was matched - and done over the full
    // byte range rather than the deepest column, so the steps stay on their layer boundaries.
    if (layer.chroma.plate.generated && layer.chroma.plate.orientation == ChromaOrientation::FaceDown)
        chroma_invert_relief(solve);

    std::vector<unsigned char> png = chroma_encode_height_png(solve);
    if (png.empty() || stop())
        return result;

    result.png      = std::make_shared<std::vector<unsigned char>>(std::move(png));
    result.depth_mm = depth_mm;

    result.preview.width  = solve.width;
    result.preview.height = solve.height;
    result.preview.pixels = std::move(solve.grey);
    result.preview.rgb    = std::move(solve.preview);
    result.delta          = std::move(solve.delta);
    result.mean_delta_e   = solve.mean_delta_e;
    result.max_delta_e    = solve.max_delta_e;
    result.ok             = true;
    return result;
}

ChromaSolveJob::ChromaSolveJob(ChromaSolveInput &&input, uint64_t generation,
                               std::shared_ptr<const std::atomic<uint64_t>>     current_generation,
                               std::function<void(ChromaSolveResult, uint64_t)> on_finished)
    : m_input(std::move(input)), m_generation(generation), m_current_generation(std::move(current_generation)),
      m_on_finished(std::move(on_finished))
{
}

void ChromaSolveJob::process(Ctl &ctl)
{
    // No ctl.update_status() anywhere in here, for the reason TextureDisplacementPreviewJob gives:
    // the first call turns into an on-screen progress notification, and one of those per slider
    // release is worse than the wait ever was.
    m_result = chroma_run_solve(m_input, [this, &ctl]() {
        return ctl.was_canceled() || (m_current_generation && m_current_generation->load() != m_generation);
    });
}

void ChromaSolveJob::finalize(bool canceled, std::exception_ptr &eptr)
{
    if (!m_on_finished)
        return;
    if (canceled || eptr)
        m_on_finished(ChromaSolveResult{}, m_generation);
    else
        m_on_finished(std::move(m_result), m_generation);
}

} // namespace Slic3r::GUI
