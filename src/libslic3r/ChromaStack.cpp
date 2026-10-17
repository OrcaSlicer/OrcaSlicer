#include "ChromaStack.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <cstdlib>
#include <numeric>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <miniz.h>

namespace Slic3r {

// How many e-folding lengths of absorption count as "the colour beneath no longer shows". Three
// passes 5% of the light, which is about where the eye stops seeing what is under a layer.
static constexpr float OPAQUE_EFOLDS = 3.f;

// TD is a filament property, so a zero or negative value in a preset is a data error rather than a
// request for an infinitely opaque filament. Clamp instead: below this the filament is opaque at a
// single layer anyway.
static constexpr float TD_MIN_MM = 0.05f;
static constexpr float TD_MAX_MM = 100.f;

static float srgb_to_linear(float c)
{
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

static float linear_to_srgb(float c)
{
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f;
}

// CIELAB under D65, from non-linear sRGB in [0, 1]. Only used for nearest-colour search, so the
// absolute white point matters far less than both sides using the same one.
struct Lab
{
    float l = 0.f, a = 0.f, b = 0.f;
};

static Lab srgb_to_lab(float r, float g, float b)
{
    const float rl = srgb_to_linear(r), gl = srgb_to_linear(g), bl = srgb_to_linear(b);
    const float x = (0.4124f * rl + 0.3576f * gl + 0.1805f * bl) / 0.95047f;
    const float y = (0.2126f * rl + 0.7152f * gl + 0.0722f * bl);
    const float z = (0.0193f * rl + 0.1192f * gl + 0.9505f * bl) / 1.08883f;
    auto        f = [](float t) {
        return t > 0.008856f ? std::cbrt(t) : (7.787f * t + 16.f / 116.f);
    };
    const float fx = f(x), fy = f(y), fz = f(z);
    return Lab{ 116.f * fy - 16.f, 500.f * (fx - fy), 200.f * (fy - fz) };
}

static float delta_e(const Lab &p, const Lab &q)
{
    const float dl = p.l - q.l, da = p.a - q.a, db = p.b - q.b;
    return std::sqrt(dl * dl + da * da + db * db);
}

int ChromaStack::relief_layers() const
{
    int n = 0;
    for (const ChromaBand &band : bands)
        n += std::max(0, band.layers);
    return n;
}

int ChromaStack::total_layers() const { return std::max(0, base_layers) + relief_layers(); }

float ChromaStack::depth_mm() const { return float(relief_layers()) * layer_height_mm; }

std::vector<ColorRGB> chroma_palette(const ChromaStack &stack)
{
    const int total = stack.total_layers();
    // No bands means no picture and no filament to print the base in, whatever base_layers says.
    if (total <= 0 || stack.bands.empty() || stack.layer_height_mm <= 0.f)
        return {};

    // The filament each printed layer is in, bottom-up: the base first, in the bottom band's filament,
    // then the bands themselves.
    std::vector<const ChromaFilament *> per_layer;
    per_layer.reserve(size_t(total));
    for (int i = 0; i < std::max(0, stack.base_layers); ++i)
        per_layer.push_back(&stack.bands.front().filament);
    for (const ChromaBand &band : stack.bands)
        for (int i = 0; i < band.layers; ++i)
            per_layer.push_back(&band.filament);

    std::vector<ColorRGB> palette(size_t(total) + 1);

    // Linear light, walking up from an opaque base: C_0 is the bottom layer's own colour, so a
    // column one layer tall comes out exactly that colour however translucent the filament is.
    std::array<float, 3> acc{ srgb_to_linear(per_layer.front()->color.r()), srgb_to_linear(per_layer.front()->color.g()),
                              srgb_to_linear(per_layer.front()->color.b()) };
    auto                 store = [&palette](size_t k, const std::array<float, 3> &lin) {
        palette[k] = ColorRGB(std::clamp(linear_to_srgb(lin[0]), 0.f, 1.f), std::clamp(linear_to_srgb(lin[1]), 0.f, 1.f),
                              std::clamp(linear_to_srgb(lin[2]), 0.f, 1.f));
    };
    store(0, acc);

    for (int k = 1; k <= total; ++k) {
        const ChromaFilament &f  = *per_layer[size_t(k - 1)];
        const float             td = std::clamp(f.td_mm, TD_MIN_MM, TD_MAX_MM);
        // The depth at which this filament hides what is behind it, turned into the e-folding length
        // of its absorption; see CHROMA_TD_OPAQUE_FRACTION.
        const float efold = CHROMA_TD_OPAQUE_FRACTION * td / OPAQUE_EFOLDS;
        const float t     = std::exp(-stack.layer_height_mm / efold);
        const std::array<float, 3> own{ srgb_to_linear(f.color.r()), srgb_to_linear(f.color.g()),
                                        srgb_to_linear(f.color.b()) };
        for (int c = 0; c < 3; ++c)
            acc[size_t(c)] = (1.f - t) * own[size_t(c)] + t * acc[size_t(c)];
        store(size_t(k), acc);
    }
    return palette;
}

ChromaSolve chroma_solve(const uint8_t *rgb, int width, int height, const ChromaStack &stack)
{
    ChromaSolve solve;
    const int     total  = stack.total_layers();
    const int     base   = std::max(0, stack.base_layers);
    const int     relief = stack.relief_layers();
    if (rgb == nullptr || width <= 0 || height <= 0 || relief <= 0 || relief > CHROMA_MAX_RELIEF_LAYERS)
        return solve;

    const std::vector<ColorRGB> palette = chroma_palette(stack);
    if (int(palette.size()) != total + 1)
        return solve;

    // Only the heights the solver may choose: base (nothing of the relief printed) up to total.
    const int               choices = relief + 1;
    const size_t                        choice_count = size_t(choices);
    std::vector<Lab>                    choice_lab(choice_count);
    std::vector<std::array<uint8_t, 3>> choice_rgb(choice_count);
    for (int i = 0; i < choices; ++i) {
        const ColorRGB &c = palette[size_t(base + i)];
        choice_lab[size_t(i)] = srgb_to_lab(c.r(), c.g(), c.b());
        choice_rgb[size_t(i)] = { uint8_t(std::lround(c.r() * 255.f)), uint8_t(std::lround(c.g() * 255.f)),
                                  uint8_t(std::lround(c.b() * 255.f)) };
    }

    const size_t count = size_t(width) * size_t(height);
    solve.width        = width;
    solve.height       = height;
    solve.levels.resize(count);
    solve.grey.resize(count);
    solve.preview.resize(count * 3);

    std::vector<float> errors(count, 0.f);

    auto pick = [&](float r, float g, float b, float &out_error) {
        const Lab target = srgb_to_lab(std::clamp(r, 0.f, 1.f), std::clamp(g, 0.f, 1.f), std::clamp(b, 0.f, 1.f));
        int       best   = 0;
        float     best_d = std::numeric_limits<float>::max();
        for (int i = 0; i < choices; ++i) {
            const float d = delta_e(target, choice_lab[size_t(i)]);
            if (d < best_d) {
                best_d = d;
                best   = i;
            }
        }
        out_error = best_d;
        return best;
    };

    // The grey level a chosen height is written as. Rounded to the nearest of 256 levels, which for
    // any practical layer budget (a few dozen) maps every height to its own level exactly - the
    // relief's steps stay on layer boundaries, which is the whole point of quantizing here.
    auto grey_of = [relief](int choice) {
        return uint8_t(std::lround(255.0 * double(choice) / double(relief)));
    };

    auto write = [&](size_t px, int choice, float err) {
        solve.levels[px]        = uint16_t(base + choice);
        solve.grey[px]          = grey_of(choice);
        solve.preview[px * 3]     = choice_rgb[size_t(choice)][0];
        solve.preview[px * 3 + 1] = choice_rgb[size_t(choice)][1];
        solve.preview[px * 3 + 2] = choice_rgb[size_t(choice)][2];
        errors[px]              = err;
    };

    if (stack.dither) {
        // Floyd-Steinberg, serpentine so the error does not drift consistently to one side. The
        // diffusion buffer holds the two rows the kernel reaches, in sRGB - which is where the
        // artifact being traded away lives, and what every reference implementation diffuses in.
        const size_t                      row_len = size_t(width);
        std::vector<std::array<float, 3>> row(row_len), next(row_len);
        for (int x = 0; x < width; ++x) {
            const uint8_t *src = rgb + size_t(x) * 3;
            row[size_t(x)]     = { src[0] / 255.f, src[1] / 255.f, src[2] / 255.f };
        }
        for (int y = 0; y < height; ++y) {
            if (y + 1 < height)
                for (int x = 0; x < width; ++x) {
                    const uint8_t *src = rgb + (size_t(y + 1) * size_t(width) + size_t(x)) * 3;
                    next[size_t(x)]    = { src[0] / 255.f, src[1] / 255.f, src[2] / 255.f };
                }
            const bool ltr = (y % 2) == 0;
            for (int i = 0; i < width; ++i) {
                const int    x  = ltr ? i : width - 1 - i;
                const size_t px = size_t(y) * size_t(width) + size_t(x);
                float        err = 0.f;
                const int    choice = pick(row[size_t(x)][0], row[size_t(x)][1], row[size_t(x)][2], err);
                write(px, choice, err);

                std::array<float, 3> residual{};
                for (int c = 0; c < 3; ++c)
                    residual[size_t(c)] = row[size_t(x)][size_t(c)] - choice_rgb[size_t(choice)][size_t(c)] / 255.f;

                const int  ahead = ltr ? x + 1 : x - 1;
                const int  back  = ltr ? x - 1 : x + 1;
                auto spread = [&](std::vector<std::array<float, 3>> &dst, int at, float w) {
                    if (at < 0 || at >= width)
                        return;
                    for (int c = 0; c < 3; ++c)
                        dst[size_t(at)][size_t(c)] += residual[size_t(c)] * w;
                };
                spread(row, ahead, 7.f / 16.f);
                if (y + 1 < height) {
                    spread(next, back, 3.f / 16.f);
                    spread(next, x, 5.f / 16.f);
                    spread(next, ahead, 1.f / 16.f);
                }
            }
            row.swap(next);
        }
    } else {
        tbb::parallel_for(tbb::blocked_range<int>(0, height), [&](const tbb::blocked_range<int> &range) {
            for (int y = range.begin(); y < range.end(); ++y)
                for (int x = 0; x < width; ++x) {
                    const size_t   px  = size_t(y) * size_t(width) + size_t(x);
                    const uint8_t *src = rgb + px * 3;
                    float          err = 0.f;
                    write(px, pick(src[0] / 255.f, src[1] / 255.f, src[2] / 255.f, err), err);
                }
        });
    }

    solve.mean_delta_e = count == 0 ? 0. : std::accumulate(errors.begin(), errors.end(), 0.) / double(count);
    solve.max_delta_e  = errors.empty() ? 0. : double(*std::max_element(errors.begin(), errors.end()));
    return solve;
}

float chroma_layers_to_opaque(float td_mm, float layer_height_mm)
{
    if (layer_height_mm <= 0.f)
        return 0.f;
    return CHROMA_TD_OPAQUE_FRACTION * std::clamp(td_mm, TD_MIN_MM, TD_MAX_MM) / layer_height_mm;
}

// Below about one dE two colours are the same colour to the eye. Half of that is the threshold for
// "this layer changed something", so a step has to be comfortably invisible before a layer is called
// wasted.
static constexpr float VISIBLE_DE = 0.5f;

static float step_delta_e(const ColorRGB &a, const ColorRGB &b)
{
    return delta_e(srgb_to_lab(a.r(), a.g(), a.b()), srgb_to_lab(b.r(), b.g(), b.b()));
}

int chroma_distinct_levels(const ChromaStack &stack)
{
    const std::vector<ColorRGB> palette = chroma_palette(stack);
    const int                   base    = std::max(0, stack.base_layers);
    if (int(palette.size()) <= base + 1)
        return 0;
    int n = 0;
    for (size_t i = size_t(base) + 1; i < palette.size(); ++i)
        if (step_delta_e(palette[i - 1], palette[i]) > VISIBLE_DE)
            ++n;
    return n;
}

void chroma_trim_bands(ChromaStack &stack)
{
    // Walked over the stack's own palette rather than over the filaments' TD, because how fast a band
    // runs out of anything to say depends on what is under it as much as on the filament. A light band
    // over a dark one is finished in four or five layers; a dark one over a light one goes on paying
    // for many more, long past the depth at which it is nominally opaque, because the last few percent
    // of light coming through still moves a near-black colour perceptibly.
    const std::vector<ColorRGB> palette = chroma_palette(stack);
    if (palette.empty())
        return;
    int layer = std::max(0, stack.base_layers); // palette index of the layer under the band being looked at
    for (ChromaBand &band : stack.bands) {
        int keep = 1;
        for (int i = 1; i <= band.layers; ++i) {
            const size_t cur = size_t(layer) + size_t(i);
            if (cur >= palette.size())
                break;
            if (step_delta_e(palette[cur - 1], palette[cur]) > VISIBLE_DE)
                keep = i; // the last layer of this band that still changes the colour
        }
        layer += band.layers;
        band.layers = std::clamp(keep, 1, band.layers);
    }
}

int chroma_level_at(const ChromaStack &stack, double z_mm)
{
    const int total = stack.total_layers();
    if (total <= 0 || stack.layer_height_mm <= 0.f)
        return 0;
    // Layer k occupies ((k-1)h, k h], so a point inside it rounds up. The epsilon keeps a surface
    // sitting exactly on a layer boundary in the layer that printed it rather than in the one above,
    // which is the difference between a terrace showing its own colour and showing the next band's.
    const int k = int(std::ceil(z_mm / double(stack.layer_height_mm) - 1e-6));
    return std::clamp(k, 1, total);
}

std::vector<unsigned char> chroma_encode_height_png(const ChromaSolve &solve)
{
    if (solve.empty())
        return {};
    size_t      size = 0;
    void       *png  = tdefl_write_image_to_png_file_in_memory_ex(solve.grey.data(), solve.width, solve.height, 1, &size,
                                                                  MZ_DEFAULT_LEVEL, MZ_FALSE);
    if (png == nullptr || size == 0)
        return {};
    const unsigned char       *bytes = static_cast<const unsigned char *>(png);
    std::vector<unsigned char> out(bytes, bytes + size);
    mz_free(png);
    return out;
}

std::vector<ChromaRange> chroma_ranges(const ChromaStack &stack, double z_bottom)
{
    std::vector<ChromaRange> out;
    if (stack.layer_height_mm <= 0.f)
        return out;
    // The layer height is stored as a float, like every other dimension of a texture displacement
    // layer, so multiplying it up leaves noise in the last few digits - 5 layers of 0.1 mm come out as
    // 0.5000000074. The slicer derives its own layer boundaries from the print config in double
    // precision and lands on 0.5 exactly, and a boundary that misses it by any amount is a boundary a
    // layer can fall on the wrong side of. Rounding to the nanometre removes the noise without
    // disturbing any layer height a profile can express.
    const auto snap = [](double z) { return std::round(z * 1e6) / 1e6; };

    // The base is printed in the bottom band's filament - it is what is loaded when the print starts -
    // so it extends that band's range down to the first layer instead of being a swap of its own.
    int  layer = std::max(0, stack.base_layers);
    bool first = true;
    for (const ChromaBand &band : stack.bands) {
        if (band.layers <= 0)
            continue;
        const double z_min = snap(z_bottom + (first ? 0. : double(layer) * double(stack.layer_height_mm)));
        first = false;
        layer += band.layers;
        const double z_max = snap(z_bottom + double(layer) * double(stack.layer_height_mm));
        if (!out.empty() && out.back().filament == band.filament.filament)
            out.back().z_max = z_max; // no swap happens here, so it is one range
        else
            out.push_back(ChromaRange{ z_min, z_max, band.filament.filament });
    }
    return out;
}

ChromaStack chroma_default_stack(const std::vector<ChromaFilament> &filaments, float layer_height_mm,
                                     int base_layers, int relief_layers)
{
    ChromaStack stack;
    stack.layer_height_mm = layer_height_mm > 0.f ? layer_height_mm : 0.08f;
    stack.base_layers     = std::max(0, base_layers);
    if (filaments.empty() || relief_layers <= 0)
        return stack;

    std::vector<ChromaFilament> ordered = filaments;
    std::stable_sort(ordered.begin(), ordered.end(), [](const ChromaFilament &a, const ChromaFilament &b) {
        // Relative luminance of the linear colour: the perceptual order, so a saturated yellow
        // counts as light and a saturated blue as dark, which a plain r+g+b sum gets wrong.
        auto lum = [](const ColorRGB &c) {
            return 0.2126f * srgb_to_linear(c.r()) + 0.7152f * srgb_to_linear(c.g()) + 0.0722f * srgb_to_linear(c.b());
        };
        return lum(a.color) > lum(b.color);
    });

    const int count = int(ordered.size());
    int       left  = relief_layers;
    for (int i = 0; i < count; ++i) {
        // Spread the remainder over the first bands rather than piling it onto the last one.
        const int share = left / (count - i) + ((left % (count - i)) > 0 ? 1 : 0);
        ChromaBand band;
        band.filament = ordered[size_t(i)];
        band.layers   = std::max(1, share);
        left -= share;
        stack.bands.push_back(band);
    }
    return stack;
}

} // namespace Slic3r
