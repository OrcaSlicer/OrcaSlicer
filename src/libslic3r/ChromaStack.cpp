#include "ChromaStack.hpp"

#include <cctype>
#include <set>

#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include "Polygon.hpp"
#include "Utils.hpp"
#include "TextureToColor/ColorUtils.hpp"
#include "Triangulation.hpp"
#include "libslic3r.h"

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

static ColorRGB lab_to_srgb(const Lab &lab)
{
    const float fy = (lab.l + 16.f) / 116.f;
    const float fx = fy + lab.a / 500.f;
    const float fz = fy - lab.b / 200.f;
    auto        inv = [](float t) { return (t * t * t > 0.008856f) ? t * t * t : (t - 16.f / 116.f) / 7.787f; };
    const float x = inv(fx) * 0.95047f, y = inv(fy), z = inv(fz) * 1.08883f;
    const float r =  3.2406f * x - 1.5372f * y - 0.4986f * z;
    const float g = -0.9689f * x + 1.8758f * y + 0.0415f * z;
    const float b =  0.0557f * x - 0.2040f * y + 1.0570f * z;
    return ColorRGB(std::clamp(linear_to_srgb(std::max(r, 0.f)), 0.f, 1.f),
                    std::clamp(linear_to_srgb(std::max(g, 0.f)), 0.f, 1.f),
                    std::clamp(linear_to_srgb(std::max(b, 0.f)), 0.f, 1.f));
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

float ChromaStack::total_mm() const { return float(total_layers()) * layer_height_mm; }

float chroma_layer_density(float td_mm, float layer_height_mm)
{
    if (layer_height_mm <= 0.f)
        return 0.f;
    // The depth at which this filament hides what is behind it, turned into the e-folding length of
    // its absorption; see CHROMA_TD_OPAQUE_FRACTION.
    const float efold = CHROMA_TD_OPAQUE_FRACTION * std::clamp(td_mm, TD_MIN_MM, TD_MAX_MM) / OPAQUE_EFOLDS;
    return layer_height_mm / efold;
}

// The hue a filament lends to light passing through it, in linear light, with its own darkness taken
// out: the brightest channel comes out 1 and the others in proportion.
//
// How much light gets through at all is the transmission distance's business and
// chroma_layer_density()'s alone, so multiplying by the filament's colour as well would count the same
// absorption twice: a filament of TD 1 blocks the light from behind it whether it is white or black,
// which is exactly why a back-lit stack is chosen by TD and not by colour. What the colour decides is
// what the light that does get through looks like. A filament with no colour at all has no hue to
// lend and tints nothing.
static std::array<float, 3> backlit_tint(const ColorRGB &c)
{
    std::array<float, 3> lin{ srgb_to_linear(c.r()), srgb_to_linear(c.g()), srgb_to_linear(c.b()) };
    const float          peak = std::max({ lin[0], lin[1], lin[2] });
    if (peak <= 1e-6f)
        return { 1.f, 1.f, 1.f };
    for (float &v : lin)
        v /= peak;
    return lin;
}

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

    // Nothing illuminates a layer from the front, so no layer adds a colour of its own: the whole
    // column is a filter over the lamp, and entry k is what is left of the lamp's light after k layers
    // of it. The base is inside the column rather than an opaque backing behind it, which is the one
    // structural difference from the front-lit walk - and the reason entry 0 is the bare lamp rather
    // than a filament's colour.
    const auto walk_back = [&](std::vector<std::array<float, 3>> &out) {
        const float          intensity = std::max(0.f, stack.led_intensity);
        std::array<float, 3> light{ srgb_to_linear(stack.led_color.r()) * intensity,
                                    srgb_to_linear(stack.led_color.g()) * intensity,
                                    srgb_to_linear(stack.led_color.b()) * intensity };
        out[0] = light;
        for (int k = 1; k <= total; ++k) {
            const ChromaFilament      &f    = *per_layer[size_t(k - 1)];
            const float                t    = std::exp(-chroma_layer_density(f.td_mm, stack.layer_height_mm));
            const std::array<float, 3> tint = backlit_tint(f.color);
            for (int c = 0; c < 3; ++c)
                light[size_t(c)] *= t * tint[size_t(c)];
            out[size_t(k)] = light;
        }
    };

    const auto walk_front = [&](std::vector<std::array<float, 3>> &out) {
        std::array<float, 3> up = acc;
        out[0]                  = up;
        for (int k = 1; k <= total; ++k) {
            const ChromaFilament &f = *per_layer[size_t(k - 1)];
            const float           t = std::exp(-chroma_layer_density(f.td_mm, stack.layer_height_mm));
            const std::array<float, 3> own{ srgb_to_linear(f.color.r()), srgb_to_linear(f.color.g()),
                                            srgb_to_linear(f.color.b()) };
            for (int c = 0; c < 3; ++c)
                up[size_t(c)] = (1.f - t) * own[size_t(c)] + t * up[size_t(c)];
            out[size_t(k)] = up;
        }
    };

    std::vector<std::array<float, 3>> lin(size_t(total) + 1);
    if (stack.lighting == ChromaLighting::BackLit) {
        walk_back(lin);
    } else if (stack.lighting == ChromaLighting::BothLit) {
        // Added in linear light, which is the only place adding two lights means anything: one photon
        // off the face and one through the column arrive at the eye as their sum, and sRGB is a curve
        // laid over that sum rather than a space it can be taken in. Weighted rather than summed
        // outright so the balance is what the user set and not a function of how bright the lamp is.
        std::vector<std::array<float, 3>> back(size_t(total) + 1);
        walk_front(lin);
        walk_back(back);
        const float w = std::clamp(stack.lighting_balance, 0.f, 1.f);
        for (size_t k = 0; k < lin.size(); ++k)
            for (int c = 0; c < 3; ++c)
                lin[k][size_t(c)] = (1.f - w) * lin[k][size_t(c)] + w * back[k][size_t(c)];
    } else {
        walk_front(lin);
    }

    for (size_t k = 0; k < lin.size(); ++k)
        store(k, lin[k]);
    return palette;
}

// A mask as one byte per pixel of another grid, nearest-neighbour: 1 where the region covers that
// pixel, 0 elsewhere, and empty for a mask with nothing in it.
//
// Every mask here is painted on the picture's own grid while the thing it is applied to may be a
// reduced copy of the picture - the panel solves a preview at a few hundred texels a side - so each of
// the three places a mask is used needs the same resampling, and they share it rather than each
// getting the rounding slightly different.
static std::vector<uint8_t> mask_on_grid(const ChromaMask &mask, int width, int height)
{
    if (width <= 0 || height <= 0)
        return {};
    std::vector<uint8_t> src = chroma_mask_decode(mask);
    if (src.empty())
        return {};
    if (mask.width == width && mask.height == height)
        return src;

    std::vector<uint8_t> out(size_t(width) * size_t(height), 0);
    for (int y = 0; y < height; ++y) {
        const int my = std::min(mask.height - 1, int(int64_t(y) * int64_t(mask.height) / int64_t(height)));
        for (int x = 0; x < width; ++x) {
            const int mx = std::min(mask.width - 1, int(int64_t(x) * int64_t(mask.width) / int64_t(width)));
            out[size_t(y) * size_t(width) + size_t(x)] = src[size_t(my) * size_t(mask.width) + size_t(mx)];
        }
    }
    return out;
}

namespace {

// ---- Stage (a): what this stack can print ------------------------------------------------------
//
// Only the heights the solver may choose are in here: the base, where nothing of the relief is
// printed, up to the top of the stack. Built once per solve and read by every pixel.
struct ChromaChoices
{
    // The match's own form of each colour. Lab for CIE76; sRGB as well, because the one CIEDE2000 in
    // the tree takes sRGB (see pick_choice()).
    std::vector<Lab>                                        lab;
    std::vector<tex2color::color_utils::ColorDouble>        srgb;
    // And the bytes the preview is written from.
    std::vector<std::array<uint8_t, 3>>                     rgb8;

    int count() const { return int(lab.size()); }
};

ChromaChoices build_choices(const std::vector<ColorRGB> &palette, int base, int choices)
{
    ChromaChoices out;
    out.lab.resize(size_t(choices));
    out.srgb.resize(size_t(choices));
    out.rgb8.resize(size_t(choices));
    for (int i = 0; i < choices; ++i) {
        const ColorRGB &c = palette[size_t(base + i)];
        out.lab[size_t(i)]  = srgb_to_lab(c.r(), c.g(), c.b());
        out.srgb[size_t(i)] = { double(c.r()), double(c.g()), double(c.b()) };
        out.rgb8[size_t(i)] = { uint8_t(std::lround(c.r() * 255.f)), uint8_t(std::lround(c.g() * 255.f)),
                                uint8_t(std::lround(c.b() * 255.f)) };
    }
    return out;
}

// How far one colour is from one of the choices, by whichever metric this solve was asked for.
//
// The CIEDE2000 is the one in TextureToColor, which is the tree's only one and stays the only one: a
// second copy of a formula that long is a second thing to keep correct. Its entry point takes sRGB and
// converts both sides to Lab itself, so the Lab precomputed above buys nothing in high quality mode
// and a comparison there costs two colour-space conversions rather than none. That is the price of
// having one implementation, and it is why high quality is the worker's setting rather than the
// interactive one; a Lab-level entry point over there would remove the cost without changing a result.
float choice_error(const ChromaChoices &choices, bool high_quality, const Lab &target_lab,
                   const tex2color::color_utils::ColorDouble &target_srgb, int choice)
{
    if (high_quality)
        return float(tex2color::color_utils::calc_rgb_color_difference_by_ciede2000_srgb01(
            target_srgb, choices.srgb[size_t(choice)]));
    return delta_e(target_lab, choices.lab[size_t(choice)]);
}

// ---- Stage (b): the match, for one pixel -------------------------------------------------------
int pick_choice(const ChromaChoices &choices, bool high_quality, float r, float g, float b, float &out_error)
{
    const float cr = std::clamp(r, 0.f, 1.f), cg = std::clamp(g, 0.f, 1.f), cb = std::clamp(b, 0.f, 1.f);
    const Lab   target_lab  = high_quality ? Lab{} : srgb_to_lab(cr, cg, cb);
    const tex2color::color_utils::ColorDouble target_srgb{ double(cr), double(cg), double(cb) };

    int   best   = 0;
    float best_d = std::numeric_limits<float>::max();
    for (int i = 0; i < choices.count(); ++i) {
        const float d = choice_error(choices, high_quality, target_lab, target_srgb, i);
        if (d < best_d) {
            best_d = d;
            best   = i;
        }
    }
    out_error = best_d;
    return best;
}

// ---- Stage (b) over the picture: every pixel on its own ----------------------------------------
void match_pixels(const uint8_t *rgb, int width, int height, const ChromaChoices &choices, bool high_quality,
                  std::vector<uint16_t> &choice, std::vector<float> &errors)
{
    tbb::parallel_for(tbb::blocked_range<int>(0, height), [&](const tbb::blocked_range<int> &range) {
        for (int y = range.begin(); y < range.end(); ++y)
            for (int x = 0; x < width; ++x) {
                const size_t   px  = size_t(y) * size_t(width) + size_t(x);
                const uint8_t *src = rgb + px * 3;
                float          err = 0.f;
                choice[px] = uint16_t(pick_choice(choices, high_quality, src[0] / 255.f, src[1] / 255.f,
                                                  src[2] / 255.f, err));
                errors[px] = err;
            }
    });
}

// ---- Stage (c): the match with the error passed on ---------------------------------------------
//
// Floyd-Steinberg, serpentine so the error does not drift consistently to one side, over *cells*
// rather than over texels: a cell is `cell` texels square, every texel in it takes the one choice the
// cell made, and what the cell could not reach is diffused to the cells around it. The diffusion runs
// in sRGB, which is where the artifact being traded away lives and what every reference implementation
// diffuses in.
//
// Cells exist for the printer, not for the eye. Texel-by-texel diffusion scatters single-texel changes
// of height, and a region narrower than the nozzle's line width is not a thin wall but nothing at all:
// the slicer drops it, so the preview promises a tone the print cannot make. One choice per cell makes
// the smallest region whose height changes one cell across, which is the smallest region the printer
// can actually build. A cell's colour is the mean of its texels' - the print cannot show them
// separately, so their average is the honest thing to match.
//
// At cell 1 this is the texel-by-texel diffusion that was here before cells existed, arithmetic
// included: a mean over one texel is that texel, and the cell grid is the texel grid.
void match_pixels_dithered(const uint8_t *rgb, int width, int height, int cell, const ChromaChoices &choices,
                           bool high_quality, std::vector<uint16_t> &choice, std::vector<float> &errors)
{
    const int cols = (width + cell - 1) / cell;
    const int rows = (height + cell - 1) / cell;

    const size_t                      cell_cols = size_t(cols);
    std::vector<std::array<float, 3>> row(cell_cols), next(cell_cols);
    const auto read_row = [&](int cy, std::vector<std::array<float, 3>> &dst) {
        for (int cx = 0; cx < cols; ++cx) {
            std::array<float, 3> sum{ 0.f, 0.f, 0.f };
            float                n = 0.f;
            for (int y = cy * cell; y < std::min((cy + 1) * cell, height); ++y)
                for (int x = cx * cell; x < std::min((cx + 1) * cell, width); ++x) {
                    const uint8_t *src = rgb + (size_t(y) * size_t(width) + size_t(x)) * 3;
                    for (int c = 0; c < 3; ++c)
                        sum[size_t(c)] += src[c] / 255.f;
                    n += 1.f;
                }
            for (int c = 0; c < 3; ++c)
                dst[size_t(cx)][size_t(c)] = n > 0.f ? sum[size_t(c)] / n : 0.f;
        }
    };

    read_row(0, row);
    for (int cy = 0; cy < rows; ++cy) {
        if (cy + 1 < rows)
            read_row(cy + 1, next);
        const bool ltr = (cy % 2) == 0;
        for (int i = 0; i < cols; ++i) {
            const int cx     = ltr ? i : cols - 1 - i;
            float     err    = 0.f;
            const int picked = pick_choice(choices, high_quality, row[size_t(cx)][0], row[size_t(cx)][1],
                                           row[size_t(cx)][2], err);
            // Every texel of the cell takes the cell's choice and the cell's error: the cell is the
            // smallest thing the printer can give a height of its own to, so it is also the smallest
            // thing an error can honestly be reported for.
            for (int y = cy * cell; y < std::min((cy + 1) * cell, height); ++y)
                for (int x = cx * cell; x < std::min((cx + 1) * cell, width); ++x) {
                    const size_t px = size_t(y) * size_t(width) + size_t(x);
                    choice[px]      = uint16_t(picked);
                    errors[px]      = err;
                }

            std::array<float, 3> residual{};
            for (int c = 0; c < 3; ++c)
                residual[size_t(c)] = row[size_t(cx)][size_t(c)] - choices.rgb8[size_t(picked)][size_t(c)] / 255.f;

            const int  ahead  = ltr ? cx + 1 : cx - 1;
            const int  back   = ltr ? cx - 1 : cx + 1;
            const auto spread = [&](std::vector<std::array<float, 3>> &dst, int at, float w) {
                if (at < 0 || at >= cols)
                    return;
                for (int c = 0; c < 3; ++c)
                    dst[size_t(at)][size_t(c)] += residual[size_t(c)] * w;
            };
            spread(row, ahead, 7.f / 16.f);
            if (cy + 1 < rows) {
                spread(next, back, 3.f / 16.f);
                spread(next, cx, 5.f / 16.f);
                spread(next, ahead, 1.f / 16.f);
            }
        }
        row.swap(next);
    }
}

// ---- Stage (d): the stack's own edits ----------------------------------------------------------
//
// In the order they were painted, each one over whatever the ones before it left, which is what makes
// a later stroke over the same pixels win. Nothing here looks at the picture again except to say how
// far the edit has moved the column from it: an edit is the user overruling the match, and the error
// image has to report what will actually be printed rather than what the match would have chosen.
void apply_retouch(const ChromaStack &stack, const uint8_t *rgb, int width, int height, int relief,
                   const ChromaChoices &choices, bool high_quality, std::vector<uint16_t> &choice,
                   std::vector<float> &errors)
{
    for (const ChromaRetouchEdit &edit : stack.retouch.edits) {
        if (!edit.enabled || edit.region.empty())
            continue;
        // On this solve's own grid: one mask serves every resolution the picture is solved at, which
        // is what lets the coarse interactive solve and the full one it is replaced by show the same
        // edits.
        const std::vector<uint8_t> mask = mask_on_grid(edit.region, width, height);
        if (mask.empty())
            continue;
        const int band_top = chroma_band_top(stack, edit.value);

        tbb::parallel_for(tbb::blocked_range<int>(0, height), [&](const tbb::blocked_range<int> &range) {
            for (int y = range.begin(); y < range.end(); ++y) {
                for (int x = 0; x < width; ++x) {
                    const size_t px   = size_t(y) * size_t(width) + size_t(x);
                    if (mask[px] == 0)
                        continue;
                    const int    want = edit.target == ChromaRetouchEdit::Target::Band ? band_top :
                                                                                         int(choice[px]) + edit.value;
                    // Clamped to the stack: an edit cannot ask for a column the print does not have,
                    // and a delta that would run off either end stops at it rather than wrapping.
                    const int clamped = std::clamp(want, 0, relief);
                    if (clamped == int(choice[px]))
                        continue;
                    choice[px] = uint16_t(clamped);

                    const uint8_t *src = rgb + px * 3;
                    const float    r = src[0] / 255.f, g = src[1] / 255.f, b = src[2] / 255.f;
                    errors[px] = choice_error(choices, high_quality, srgb_to_lab(r, g, b),
                                              tex2color::color_utils::ColorDouble{ double(r), double(g), double(b) },
                                              clamped);
                }
            }
        });
    }
}

// ---- Stage (e): the choices as everything a solve hands back -----------------------------------
//
// `weights` is the importance region sampled onto this grid, or empty when there is none, and the
// only thing it touches is the mean: which colour a pixel got is its own business, how much that
// pixel's error counts is the user's.
void write_outputs(const ChromaChoices &choices, int base, int relief, const std::vector<uint16_t> &choice,
                   const std::vector<float> &errors, const std::vector<float> &weights, ChromaSolve &solve)
{
    const size_t count = choice.size();
    solve.levels.resize(count);
    solve.grey.resize(count);
    solve.preview.resize(count * 3);
    solve.delta.resize(count);
    for (size_t px = 0; px < count; ++px) {
        const size_t c   = size_t(choice[px]);
        solve.levels[px] = uint16_t(base + int(c));
        // The grey level a chosen height is written as. Rounded to the nearest of 256 levels, which
        // for any practical layer budget (a few dozen) maps every height to its own level exactly -
        // the relief's steps stay on layer boundaries, which is the whole point of quantizing here.
        solve.grey[px]            = uint8_t(std::lround(255.0 * double(c) / double(relief)));
        solve.preview[px * 3]     = choices.rgb8[c][0];
        solve.preview[px * 3 + 1] = choices.rgb8[c][1];
        solve.preview[px * 3 + 2] = choices.rgb8[c][2];
        solve.delta[px] = uint8_t(std::clamp(std::lround(double(errors[px]) * 255.0 /
                                                        double(CHROMA_ERROR_IMAGE_FULL_SCALE_DE)),
                                             0L, 255L));
    }

    if (weights.size() == count && count > 0) {
        double sum = 0., total = 0.;
        for (size_t px = 0; px < count; ++px) {
            sum += double(weights[px]) * double(errors[px]);
            total += double(weights[px]);
        }
        solve.mean_delta_e = total > 0. ? sum / total : 0.;
    } else {
        solve.mean_delta_e = count == 0 ? 0. : std::accumulate(errors.begin(), errors.end(), 0.) / double(count);
    }
    // Not weighted: the worst pixel is the worst pixel there is. An importance region says what to
    // optimise for, not what to stop looking at.
    solve.max_delta_e = errors.empty() ? 0. : double(*std::max_element(errors.begin(), errors.end()));
}

} // namespace

ChromaSolve chroma_solve(const uint8_t *rgb, int width, int height, const ChromaStack &stack,
                         const ChromaSolveOptions &options)
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

    const ChromaChoices choices = build_choices(palette, base, relief + 1);

    const size_t count = size_t(width) * size_t(height);
    solve.width        = width;
    solve.height       = height;

    // What every stage after the match works on: one choice and one error per pixel, where a choice is
    // how many of the relief's layers that pixel prints. Keeping them apart from the outputs is what
    // lets a stage change its mind about a pixel without having to re-encode anything.
    std::vector<uint16_t> choice(count, 0);
    std::vector<float>    errors(count, 0.f);

    if (stack.dither) {
        // A cell larger than the picture would leave one cell, which is a solid colour rather than a
        // dither; the caller's number comes from a line width and a scale and has no reason to know
        // how small a preview grid it landed on.
        const int cell = std::clamp(options.dither_cell_px, 1, std::max(width, height));
        match_pixels_dithered(rgb, width, height, cell, choices, options.high_quality, choice, errors);
    } else {
        match_pixels(rgb, width, height, choices, options.high_quality, choice, errors);
    }

    apply_retouch(stack, rgb, width, height, relief, choices, options.high_quality, choice, errors);

    // What the picture is about, on this grid: a pixel inside it counts for that many pixels when the
    // errors are averaged. Nothing above this line reads it - the match is per pixel and a weight
    // cannot change which colour is nearest - so it is built here, where it is used.
    std::vector<float> weights;
    if (!stack.importance.empty()) {
        const std::vector<uint8_t> mask = mask_on_grid(stack.importance.region, width, height);
        if (!mask.empty()) {
            weights.assign(count, 1.f);
            const float inside = std::max(0.f, stack.importance.weight);
            for (size_t px = 0; px < count; ++px)
                if (mask[px] != 0)
                    weights[px] = inside;
        }
    }

    write_outputs(choices, base, relief, choice, errors, weights, solve);
    return solve;
}

ChromaSolve chroma_solve(const uint8_t *rgb, int width, int height, const ChromaStack &stack)
{
    return chroma_solve(rgb, width, height, stack, ChromaSolveOptions{});
}

ChromaMask chroma_mask_encode(const uint8_t *mask, int width, int height)
{
    ChromaMask out;
    if (mask == nullptr || width <= 0 || height <= 0)
        return out;
    out.width  = width;
    out.height = height;

    const size_t count  = size_t(width) * size_t(height);
    bool         inside = false; // the first run is the one outside the region, by convention
    uint32_t     run    = 0;
    for (size_t i = 0; i < count; ++i) {
        const bool here = mask[i] != 0;
        if (here != inside) {
            // A region that starts at the very first pixel opens with a run of length zero, which is
            // what keeps "the first run is outside" true for every mask rather than for most of them.
            out.runs.push_back(run);
            run    = 0;
            inside = here;
        }
        ++run;
    }
    // A trailing run of outside pixels says nothing the decoder cannot work out; one of inside pixels
    // has to be written.
    if (inside)
        out.runs.push_back(run);
    if (out.runs.size() < 2)
        out.runs.clear(); // nothing was painted at all
    return out;
}

std::vector<uint8_t> chroma_mask_decode(const ChromaMask &mask)
{
    if (mask.width <= 0 || mask.height <= 0)
        return {};
    const size_t         count = size_t(mask.width) * size_t(mask.height);
    std::vector<uint8_t> out(count, 0);
    size_t               at     = 0;
    bool                 inside = false;
    for (const uint32_t run : mask.runs) {
        const size_t end = std::min(count, at + size_t(run));
        if (inside)
            std::fill(out.begin() + std::ptrdiff_t(at), out.begin() + std::ptrdiff_t(end), uint8_t(1));
        at     = end;
        inside = !inside;
        if (at >= count)
            break;
    }
    return out;
}

int chroma_band_top(const ChromaStack &stack, int band)
{
    const int relief = stack.relief_layers();
    if (band < 0)
        return 0; // the bare base: nothing of the picture printed here
    int top = 0;
    for (size_t i = 0; i < stack.bands.size(); ++i) {
        top += std::max(0, stack.bands[i].layers);
        if (int(i) == band)
            return std::min(top, relief);
    }
    return relief;
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

float chroma_tilt_relief(ChromaSolve &solve, const ChromaStack &stack, const ChromaSurface &surface)
{
    const float depth = stack.depth_mm();
    if (solve.empty() || surface.empty() || depth <= 0.f)
        return depth;
    if (surface.width != solve.width || surface.height != solve.height)
        return depth; // not this picture's surface; better to print it level than to print it wrong
    const size_t count = solve.grey.size();
    if (surface.facing.size() != count)
        return depth;

    const float floor_cos = std::cos(CHROMA_MAX_TILT_DEG * float(M_PI) / 180.f);
    // What the deepest texel asks for. Measured rather than assumed, so a picture that only clips a
    // tilted corner keeps nearly all of the 8 bits for its own relief.
    float boost = 1.f;
    for (size_t i = 0; i < count; ++i) {
        const float facing = surface.facing[i];
        if (facing <= 0.f || solve.grey[i] == 0)
            continue; // unpainted, or a texel that displaces by nothing and cannot be deepened
        boost = std::max(boost, 1.f / std::max(facing, floor_cos));
    }
    if (boost <= 1.f + 1e-4f)
        return depth; // level enough that the correction is below the encoding's own step

    // The grey map is "displace this fraction of depth_mm along the normal", so deepening the layer
    // and scaling every texel by its own factor leaves a level texel displacing exactly as far as it
    // did and a tilted one reaching the print height its colour was solved for.
    for (size_t i = 0; i < count; ++i) {
        const float facing = surface.facing[i];
        if (facing <= 0.f) {
            // Nothing painted here. The texel is outside the patch and is never sampled, but it is
            // rescaled with the rest so the map stays a map rather than a cliff at the patch edge.
            solve.grey[i] = uint8_t(std::lround(float(solve.grey[i]) / boost));
            continue;
        }
        const float scaled = float(solve.grey[i]) / (std::max(facing, floor_cos) * boost);
        solve.grey[i]      = uint8_t(std::clamp(std::lround(scaled), 0L, 255L));
    }
    return depth * boost;
}

void chroma_invert_relief(ChromaSolve &solve)
{
    // The complement of the whole byte range rather than of the deepest column, so a grey level still
    // means the same number of layers it did before: the map is read against the layer's own depth,
    // and rescaling it to whatever this picture's deepest column happened to be would move every step
    // off its layer boundary.
    for (uint8_t &g : solve.grey)
        g = uint8_t(255 - int(g));
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

std::vector<unsigned char> chroma_encode_error_png(const ChromaSolve &solve)
{
    if (solve.empty() || solve.delta.size() != size_t(solve.width) * size_t(solve.height))
        return {};
    size_t size = 0;
    void  *png  = tdefl_write_image_to_png_file_in_memory_ex(solve.delta.data(), solve.width, solve.height, 1, &size,
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

    // The same ranges, in the order they are printed. Face down the picture's surface is the first
    // layer, and the light enters through it, so the band the light meets first has to be the one at
    // the bottom: the bands come out top-down, and the base - still the opaque thing the light ends on
    // - is printed last, on top of them. It is written as its own run of layers here rather than being
    // folded into a band, and the merge below then does for it exactly what the face-up path does by
    // hand, since it prints in the bottom band's filament and the bottom band is the range just under
    // it.
    if (stack.plate.orientation == ChromaOrientation::FaceDown) {
        int layer = 0;
        for (size_t i = stack.bands.size(); i-- > 0;) {
            const ChromaBand &band = stack.bands[i];
            if (band.layers <= 0)
                continue;
            const double z_min = snap(z_bottom + double(layer) * double(stack.layer_height_mm));
            layer += band.layers;
            const double z_max = snap(z_bottom + double(layer) * double(stack.layer_height_mm));
            if (!out.empty() && out.back().filament == band.filament.filament)
                out.back().z_max = z_max;
            else
                out.push_back(ChromaRange{ z_min, z_max, band.filament.filament });
        }
        const int base = std::max(0, stack.base_layers);
        if (base > 0 && !stack.bands.empty()) {
            const double z_min = snap(z_bottom + double(layer) * double(stack.layer_height_mm));
            layer += base;
            const double z_max = snap(z_bottom + double(layer) * double(stack.layer_height_mm));
            const int    filament = stack.bands.front().filament.filament;
            if (!out.empty() && out.back().filament == filament)
                out.back().z_max = z_max;
            else
                out.push_back(ChromaRange{ z_min, z_max, filament });
        }
        return out;
    }

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

ChromaHistogram chroma_histogram(const uint8_t *rgb, int width, int height, int max_bins, float vividness,
                                 const ChromaImportance &importance)
{
    ChromaHistogram out;
    if (rgb == nullptr || width <= 0 || height <= 0 || max_bins <= 0)
        return out;

    // Four bits a channel: fine enough that two colours in one bucket are the same colour to a stack of
    // a few filaments, coarse enough that a photograph collapses to a few hundred buckets.
    struct Bucket
    {
        double r = 0., g = 0., b = 0.;
        double n = 0.; // pixels in the bucket, which is what the colour sums are a mean over
        double w = 0.; // what those pixels are worth, which is what the bin's weight is built from
    };
    std::vector<Bucket> buckets(16 * 16 * 16);

    // The painted region, if there is one, on this picture's own grid: the same resampling a retouch
    // edit gets, and for the same reason - the mask is painted at the picture's full resolution and
    // this may be counting a reduced copy of it.
    const std::vector<uint8_t> mask   = importance.empty() ? std::vector<uint8_t>() :
                                                             mask_on_grid(importance.region, width, height);
    const double               inside = double(std::max(0.f, importance.weight));

    const size_t count = size_t(width) * size_t(height);
    for (size_t i = 0; i < count; ++i) {
        const uint8_t *p   = rgb + i * 3;
        const size_t   key = size_t(p[0] >> 4) * 256 + size_t(p[1] >> 4) * 16 + size_t(p[2] >> 4);
        const double   w   = (!mask.empty() && mask[i] != 0) ? inside : 1.;
        Bucket        &b   = buckets[key];
        // The colour is the plain mean of the pixels in the bucket: a pixel inside the region is the
        // same colour wherever it is, and only how much the bucket counts is weighted.
        b.r += p[0];
        b.g += p[1];
        b.b += p[2];
        b.n += 1.;
        b.w += w;
    }

    std::vector<std::pair<ColorRGB, float>> all;
    for (const Bucket &b : buckets)
        if (b.n > 0. && b.w > 0.)
            all.emplace_back(ColorRGB(float(b.r / b.n / 255.), float(b.g / b.n / 255.), float(b.b / b.n / 255.)),
                             float(b.w));

    // The picture's own average colour, in Lab and weighted the same way the bins are, so that "far
    // from the average" is far from this picture rather than far from a fixed grey. Taken over every
    // bucket, before the cap, or the average would be of whatever survived the cap.
    if (vividness > 0.f && all.size() > 1) {
        double total = 0.;
        double ml = 0., ma = 0., mb = 0.;
        for (const auto &[color, weight] : all) {
            const Lab p = srgb_to_lab(color.r(), color.g(), color.b());
            ml += double(p.l) * double(weight);
            ma += double(p.a) * double(weight);
            mb += double(p.b) * double(weight);
            total += double(weight);
        }
        if (total > 0.) {
            const Lab mean{ float(ml / total), float(ma / total), float(mb / total) };
            for (auto &e : all) {
                const Lab   p = srgb_to_lab(e.first.r(), e.first.g(), e.first.b());
                const float d = std::min(delta_e(p, mean) / CHROMA_VIVID_REFERENCE_DE, 1.f);
                e.second *= 1.f + vividness * d;
            }
        }
    }

    // The heaviest buckets first, so capping the count drops the colours the picture can most afford
    // to lose - which with a vividness is not the same as the ones it shows least of.
    std::sort(all.begin(), all.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
    if (int(all.size()) > max_bins)
        all.resize(size_t(max_bins));

    double total = 0.;
    for (const auto &e : all)
        total += double(e.second);
    if (total <= 0.)
        return out;
    out.bins.reserve(all.size());
    for (auto &e : all)
        out.bins.emplace_back(e.first, float(double(e.second) / total));
    return out;
}

namespace {
// How far a stack lands from a picture, with the bins measured either one after another or all at
// once. Both arrive at the same double, bit for bit, and that is the point of the arrangement:
//
//  - which colour is nearest a bin depends on nothing but that bin, so the bins can be measured in
//    any order at all;
//  - the sum is then taken over them in index order, serially, so the rounding of a double addition
//    happens in the same sequence however many threads measured the bins.
//
// A search that returned a different stack from one run to the next would be worse than a slow one:
// a user would move a slider back to where it was and get a different answer.
//
// `parallel` is false for the inner call of a search that is already spread over its candidates,
// where another layer of tasks buys nothing.
double stack_error_over(const ChromaHistogram &picture, const ChromaStack &stack, bool parallel)
{
    if (picture.empty())
        return 0.;
    const std::vector<ColorRGB> palette = chroma_palette(stack);
    const int                   base    = std::clamp(stack.base_layers, 0, int(palette.size()));
    if (int(palette.size()) <= base)
        return std::numeric_limits<double>::max();

    std::vector<Lab> reachable;
    reachable.reserve(palette.size() - size_t(base));
    for (size_t i = size_t(base); i < palette.size(); ++i)
        reachable.push_back(srgb_to_lab(palette[i].r(), palette[i].g(), palette[i].b()));

    const size_t       count = picture.bins.size();
    std::vector<float> nearest(count, std::numeric_limits<float>::max());
    const auto         measure = [&](size_t from, size_t to) {
        for (size_t i = from; i < to; ++i) {
            const ColorRGB &color  = picture.bins[i].first;
            const Lab       target = srgb_to_lab(color.r(), color.g(), color.b());
            float           best   = std::numeric_limits<float>::max();
            for (const Lab &p : reachable)
                best = std::min(best, delta_e(target, p));
            nearest[i] = best;
        }
    };
    if (parallel)
        tbb::parallel_for(tbb::blocked_range<size_t>(0, count),
                          [&](const tbb::blocked_range<size_t> &r) { measure(r.begin(), r.end()); });
    else
        measure(0, count);

    double error = 0.;
    for (size_t i = 0; i < count; ++i)
        error += double(picture.bins[i].second) * double(nearest[i]);
    return error;
}
} // namespace

double chroma_stack_error(const ChromaHistogram &picture, const ChromaStack &stack)
{
    return stack_error_over(picture, stack, true);
}

ChromaStack chroma_auto_stack(const ChromaHistogram &picture, const std::vector<ChromaFilament> &available,
                              int count, float layer_height_mm, int base_layers, int relief_layers)
{
    ChromaStack stack;
    stack.layer_height_mm = layer_height_mm > 0.f ? layer_height_mm : 0.08f;
    stack.base_layers     = std::max(0, base_layers);
    if (available.empty() || count <= 0 || relief_layers <= 0)
        return stack;

    const int    want = std::min({ count, int(available.size()), int(CHROMA_MAX_BANDS) });
    std::vector<bool> used(available.size(), false);

    // One candidate placement: which of the filaments, and where in the stack it would go.
    struct Candidate
    {
        size_t filament;
        size_t at;
    };

    // Grown one filament at a time: every unused filament tried at every position, the best placement
    // kept. Bands are re-divided at each step so a candidate is scored as the stack it would really be.
    for (int step = 0; step < want; ++step) {
        // Listed in a fixed order - filament, then position - and scored in parallel, because each
        // candidate is a pure function of the stack this step started from. Nothing about a candidate
        // depends on another, which is what makes the search parallel at all.
        std::vector<Candidate> candidates;
        for (size_t i = 0; i < available.size(); ++i) {
            if (used[i])
                continue;
            for (size_t at = 0; at <= stack.bands.size(); ++at)
                candidates.push_back(Candidate{ i, at });
        }
        if (candidates.empty())
            break;

        const auto trial_for = [&](const Candidate &c) {
            ChromaStack trial = stack;
            ChromaBand  band;
            band.filament = available[c.filament];
            trial.bands.insert(trial.bands.begin() + long(c.at), band);
            const int each = std::max(1, relief_layers / int(trial.bands.size()));
            for (ChromaBand &b : trial.bands)
                b.layers = each;
            return trial;
        };

        std::vector<double> errors(candidates.size(), std::numeric_limits<double>::max());
        tbb::parallel_for(tbb::blocked_range<size_t>(0, candidates.size()),
                          [&](const tbb::blocked_range<size_t> &r) {
                              for (size_t i = r.begin(); i < r.end(); ++i)
                                  // Serially inside: this loop is already spread over the candidates.
                                  errors[i] = stack_error_over(picture, trial_for(candidates[i]), false);
                          });

        // Chosen by walking the candidates in their own order and keeping the first that is strictly
        // better, which is exactly what the serial search did. The winner is therefore the same stack
        // on every run and on every machine, however the work was divided up.
        size_t best       = 0;
        double best_error = std::numeric_limits<double>::max();
        bool   found      = false;
        for (size_t i = 0; i < errors.size(); ++i)
            if (errors[i] < best_error) {
                best_error = errors[i];
                best       = i;
                found      = true;
            }
        if (!found)
            break;
        used[candidates[best].filament] = true;
        stack                           = trial_for(candidates[best]);
    }
    return stack;
}

namespace {
// The stack a set of candidate colours would make, so a candidate can be scored as the thing it would
// really print rather than as a list of colours.
ChromaStack stack_from_colors(const std::vector<ColorRGB> &colors, const std::vector<float> &tds, int layers,
                              float layer_height_mm)
{
    ChromaStack stack;
    stack.layer_height_mm = layer_height_mm;
    stack.base_layers     = 0; // the suggestion is about the picture, not about what it sits on
    for (size_t i = 0; i < colors.size(); ++i) {
        ChromaBand band;
        band.filament.color = colors[i];
        band.filament.td_mm = tds[i];
        band.layers         = layers;
        stack.bands.push_back(band);
    }
    return stack;
}
} // namespace

std::vector<ChromaSuggestion> chroma_suggest_colors(const ChromaHistogram &picture, int count,
                                                    const std::vector<ChromaFilament> &loaded,
                                                    float layer_height_mm, int relief_layers)
{
    std::vector<ChromaSuggestion> out;
    const int want = std::clamp(count, 1, int(CHROMA_MAX_BANDS));
    if (picture.empty() || relief_layers <= 0 || layer_height_mm <= 0.f)
        return out;

    // Start from the picture's own colours: a weighted k-means, seeded k-means++ so the first centres
    // are spread over the picture rather than crowded into whatever it has most of.
    std::vector<Lab> bins;
    bins.reserve(picture.bins.size());
    for (const auto &[color, weight] : picture.bins)
        bins.push_back(srgb_to_lab(color.r(), color.g(), color.b()));

    std::vector<size_t> centres;
    centres.push_back(0);
    for (size_t i = 1; i < picture.bins.size(); ++i)
        if (picture.bins[i].second > picture.bins[centres[0]].second)
            centres[0] = i;
    std::vector<double> scores(bins.size(), 0.);
    while (int(centres.size()) < want && centres.size() < picture.bins.size()) {
        // Every bin's score is a pure function of the centres chosen so far, so they are measured
        // together and the winner is then picked by walking them in order - the same bin the serial
        // scan would have stopped on, whatever order the threads finished in.
        tbb::parallel_for(tbb::blocked_range<size_t>(0, bins.size()), [&](const tbb::blocked_range<size_t> &r) {
            for (size_t i = r.begin(); i < r.end(); ++i) {
                float nearest = std::numeric_limits<float>::max();
                for (size_t c : centres)
                    nearest = std::min(nearest, delta_e(bins[i], bins[c]));
                // Furthest from what is already covered, weighted by how much of the picture it is.
                scores[i] = double(nearest) * double(picture.bins[i].second);
            }
        });

        size_t best       = 0;
        double best_score = -1.;
        for (size_t i = 0; i < bins.size(); ++i)
            if (scores[i] > best_score) {
                best_score = scores[i];
                best       = i;
            }
        if (best_score <= 0.)
            break;
        centres.push_back(best);
    }

    std::vector<ColorRGB> colors;
    for (size_t c : centres)
        colors.push_back(picture.bins[c].first);
    if (colors.empty())
        return out;

    // Light at the bottom: a dark filament over a light one reads as a midtone at one layer and as
    // itself at four, which is where the long tonal ramp comes from.
    std::stable_sort(colors.begin(), colors.end(), [](const ColorRGB &a, const ColorRGB &b) {
        auto lum = [](const ColorRGB &c) {
            return 0.2126f * srgb_to_linear(c.r()) + 0.7152f * srgb_to_linear(c.g()) + 0.0722f * srgb_to_linear(c.b());
        };
        return lum(a) > lum(b);
    });

    const int layers = std::max(1, relief_layers / int(colors.size()));
    // Each band wants to be translucent enough to fade over its own length and no more: a filament
    // that covers in two layers inside a band of eight wastes six, and one that never covers at all
    // never reaches its own colour.
    const std::vector<float> tds(colors.size(),
                                 std::clamp(float(layers) * layer_height_mm / CHROMA_TD_OPAQUE_FRACTION, 1.f, 12.f));

    // Then let the colours move. Each pass tries one step along each axis of CIELAB for each band and
    // keeps whatever lowers the error; the step halves between passes, so the first passes travel and
    // the last ones settle.
    double best = chroma_stack_error(picture, stack_from_colors(colors, tds, layers, layer_height_mm));
    for (float step = 16.f; step > 1.f; step *= 0.5f) {
        bool moved = false;
        for (size_t b = 0; b < colors.size(); ++b) {
            const Lab here = srgb_to_lab(colors[b].r(), colors[b].g(), colors[b].b());

            // The six moves of one band are all measured against the colours this band started from -
            // each one replaces the same entry, so an accepted move cannot change what the next one
            // would score. Only "the best so far" carries between them, so they are scored together
            // and then *accepted* one by one in the order below, which walks the climb down exactly
            // the path the serial loop walked. Bands stay in sequence: a later band does see what an
            // earlier one settled on.
            std::array<Lab, 6>    tried;
            std::array<double, 6> errors{};
            for (int m = 0; m < 6; ++m) {
                Lab         candidate = here;
                const int   axis      = m / 2;
                const float dir       = (m % 2 == 0) ? -1.f : 1.f;
                (axis == 0 ? candidate.l : axis == 1 ? candidate.a : candidate.b) += dir * step;
                candidate.l   = std::clamp(candidate.l, 0.f, 100.f);
                tried[size_t(m)] = candidate;
            }
            tbb::parallel_for(0, 6, [&](int m) {
                std::vector<ColorRGB> candidate = colors;
                candidate[b]                    = lab_to_srgb(tried[size_t(m)]);
                errors[size_t(m)] = stack_error_over(picture,
                                                     stack_from_colors(candidate, tds, layers, layer_height_mm), false);
            });

            for (int m = 0; m < 6; ++m)
                if (errors[size_t(m)] < best) {
                    best      = errors[size_t(m)];
                    colors[b] = lab_to_srgb(tried[size_t(m)]);
                    moved     = true;
                }
        }
        if (!moved && step < 4.f)
            break;
    }

    for (size_t i = 0; i < colors.size(); ++i) {
        ChromaSuggestion s;
        s.color  = colors[i];
        s.td_mm  = tds[i];
        s.layers = layers;
        const Lab target = srgb_to_lab(s.color.r(), s.color.g(), s.color.b());
        float     best_de = std::numeric_limits<float>::max();
        for (const ChromaFilament &f : loaded) {
            const float de = delta_e(target, srgb_to_lab(f.color.r(), f.color.g(), f.color.b()));
            if (de < best_de) {
                best_de    = de;
                s.nearest  = f.filament;
            }
        }
        s.nearest_de = (s.nearest >= 0) ? best_de : 0.f;
        out.push_back(s);
    }
    return out;
}

namespace {
// One axis-aligned box, appended. The wedge is nothing but boxes, and a box is the only solid whose
// facets can be written down without a mesher.
void append_box(indexed_triangle_set &its, const Vec3f &min, const Vec3f &max)
{
    const int base = int(its.vertices.size());
    for (int i = 0; i < 8; ++i)
        its.vertices.emplace_back((i & 1) ? max.x() : min.x(), (i & 2) ? max.y() : min.y(),
                                  (i & 4) ? max.z() : min.z());
    // Wound so every facet faces outward; the pairs are the two triangles of each of the six faces.
    static const int faces[12][3] = { { 0, 2, 1 }, { 1, 2, 3 }, { 4, 5, 6 }, { 5, 7, 6 },
                                      { 0, 1, 4 }, { 1, 5, 4 }, { 2, 6, 3 }, { 3, 6, 7 },
                                      { 0, 4, 2 }, { 2, 4, 6 }, { 1, 3, 5 }, { 3, 7, 5 } };
    for (const auto &f : faces)
        its.indices.emplace_back(base + f[0], base + f[1], base + f[2]);
}
} // namespace

std::vector<Vec2f> chroma_frame_profile(const ChromaFrame &frame, float base_mm)
{
    const float width = frame.width_mm;
    if (width <= 0.f)
        return {}; // no frame: the picture runs to the edge of the plate

    // A frame out of the gallery brings its own cross-section, and it is used as it stands: a real
    // moulding is not one of eight shapes, and picking the nearest of them would make the gallery a
    // picture of frames the mesh cannot build.
    //
    // What is enforced here is only what the sweep cannot survive. Every sample becomes one ring
    // between the picture and the plate's edge, so the insets have to march steadily inwards and stay
    // inside the band; a sample that goes backwards is pulled forward onto the one before it, which
    // turns an overhang the sweep cannot express into the vertical face it can. Heights are measured
    // from the picture's own surface, so the plate's backing is added here rather than being baked
    // into a definition that cannot know it.
    if (frame.profile_points.size() >= 2) {
        std::vector<Vec2f> out;
        out.reserve(frame.profile_points.size());
        float reached = 0.f;
        for (const Vec2f &sample : frame.profile_points) {
            reached = std::clamp(std::max(sample.x(), reached), 0.f, width);
            out.emplace_back(reached, base_mm + std::max(0.f, sample.y()));
        }
        // The two ends are where the plate's edge and the picture are, not where rounding left them.
        out.front().x() = 0.f;
        out.back()      = Vec2f(width, base_mm);
        return out;
    }

    const float rise = std::max(0.f, frame.rise_mm);
    // A profile with nothing to rise by is a flat border whatever its shape was set to, and saying so
    // here keeps every caller below from having to.
    if (rise <= 0.f || frame.profile == ChromaFrame::Profile::Flat)
        return { Vec2f(0.f, base_mm), Vec2f(width, base_mm) };

    const float top = base_mm + rise;
    std::vector<Vec2f> out;
    switch (frame.profile) {
    case ChromaFrame::Profile::Bevel:
        // A straight ramp down into the picture, so the frame is read as something to look through.
        out = { Vec2f(0.f, top), Vec2f(width, base_mm) };
        break;
    case ChromaFrame::Profile::Step:
        // A flat lip, then a square drop to the picture.
        out = { Vec2f(0.f, top), Vec2f(0.5f * width, top), Vec2f(0.5f * width, base_mm), Vec2f(width, base_mm) };
        break;
    case ChromaFrame::Profile::Mat: {
        // A mount board, which is the shape a framed picture is actually read by: a flat lip, and a
        // window cut at 45 degrees so the thickness of the board shows as a bright line around the
        // picture. The run of the cut equals its drop - that is what makes it 45 - and it is given
        // whatever room is left when the lip has had its share.
        const float cut = std::min(rise, 0.8f * width);
        out = { Vec2f(0.f, top), Vec2f(width - cut, top), Vec2f(width, base_mm) };
        break;
    }
    case ChromaFrame::Profile::Cove: {
        // A hollow: the same quarter as Round, turned inside out, so the band scoops away from the
        // picture instead of swelling over it.
        constexpr int steps = 12;
        out.reserve(steps + 1);
        for (int i = 0; i <= steps; ++i) {
            const float t = float(i) / float(steps);
            const float u = 1.f - t;
            out.emplace_back(width * t, base_mm + rise * (1.f - std::sqrt(std::max(0.f, 1.f - u * u))));
        }
        break;
    }
    case ChromaFrame::Profile::Step2:
        // Two square steps, which reads as a deeper frame than its rise: each tread catches the light
        // differently and the eye counts the edges rather than the height.
        out = { Vec2f(0.f, top),
                Vec2f(width / 3.f, top),
                Vec2f(width / 3.f, base_mm + 0.5f * rise),
                Vec2f(2.f * width / 3.f, base_mm + 0.5f * rise),
                Vec2f(2.f * width / 3.f, base_mm),
                Vec2f(width, base_mm) };
        break;
    case ChromaFrame::Profile::Round: {
        // A quarter ellipse, high at the edge and flat where it meets the picture: a moulding.
        constexpr int steps = 12;
        out.reserve(steps + 1);
        for (int i = 0; i <= steps; ++i) {
            const float t = float(i) / float(steps);
            out.emplace_back(width * t, base_mm + rise * std::sqrt(std::max(0.f, 1.f - t * t)));
        }
        break;
    }
    case ChromaFrame::Profile::Ogee: {
        // Cyma recta: convex out of the edge, concave into the picture. Two quarter ellipses of half
        // the rise each, meeting halfway - the shape a picture frame actually is.
        constexpr int steps = 16;
        const float   r     = 0.5f * rise;
        out.reserve(steps + 1);
        for (int i = 0; i <= steps; ++i) {
            const float t = float(i) / float(steps);
            float       z = base_mm;
            if (t <= 0.5f) {
                const float u = t / 0.5f;
                z = top - r * (1.f - std::sqrt(std::max(0.f, 1.f - u * u)));
            } else {
                const float u = (t - 0.5f) / 0.5f;
                z = base_mm + r * std::sqrt(std::max(0.f, 1.f - u * u));
            }
            out.emplace_back(width * t, z);
        }
        break;
    }
    case ChromaFrame::Profile::Flat:
    default: out = { Vec2f(0.f, base_mm), Vec2f(width, base_mm) }; break;
    }
    // Whatever the curve did, it has to arrive exactly where the picture's surface is, or the frame
    // and the picture meet in a step of rounding error.
    out.front().x() = 0.f;
    out.back()      = Vec2f(width, base_mm);
    return out;
}

std::vector<Vec2f> chroma_frame_outline(const ChromaPlate &plate, float grow_mm)
{
    const float hw = 0.5f * std::max(0.1f, plate.width_mm);
    const float hh = 0.5f * std::max(0.1f, plate.height_mm);
    const float g  = std::max(0.f, grow_mm);

    // Every loop here is the picture's own boundary when grow is 0 and the plate's edge when grow is
    // the frame's width; the sweep walks between the two. That is what makes the shape and the frame
    // one setting rather than two that have to agree.
    const auto rectangle = [&]() {
        return std::vector<Vec2f>{ Vec2f(-hw - g, -hh - g), Vec2f(hw + g, -hh - g), Vec2f(hw + g, hh + g),
                                   Vec2f(-hw - g, hh + g) };
    };

    switch (plate.frame.shape) {
    case ChromaFrame::Shape::Rectangle: return rectangle();
    case ChromaFrame::Shape::Rounded: {
        // The corner is turned on the plate's edge and every ring inside it keeps the same centre, so
        // the radius shrinks with the ring: a constant radius would make the inner rings bulge.
        //
        // It is floored rather than allowed to run out. A corner smaller than the frame is wider than
        // it - a 2 mm radius on a 6 mm frame - leaves the inner rings with no corner left to turn, and
        // answering those with a plain rectangle gave the sweep two rings of different point counts,
        // which it reads as a change of topology and refuses: the frame simply vanished. The floor is
        // small enough to be a sharp corner to look at and to print, and large enough that no two
        // points of the ring land on top of each other, which is the thing the triangulation cannot
        // take.
        constexpr float   min_corner = 0.2f;
        const float r = std::clamp(std::max(0.f, plate.frame.corner_mm) - (plate.frame.width_mm - g), min_corner,
                                   std::max(min_corner, std::min(hw + g, hh + g)));
        constexpr int     per_corner = 8;
        std::vector<Vec2f> out;
        out.reserve(4 * (per_corner + 1));
        const Vec2f centre[4] = { Vec2f(hw + g - r, hh + g - r), Vec2f(-hw - g + r, hh + g - r),
                                  Vec2f(-hw - g + r, -hh - g + r), Vec2f(hw + g - r, -hh - g + r) };
        for (int c = 0; c < 4; ++c) {
            const float a0 = float(c) * 0.5f * float(M_PI);
            for (int i = 0; i <= per_corner; ++i) {
                const float a = a0 + 0.5f * float(M_PI) * float(i) / float(per_corner);
                out.emplace_back(centre[c].x() + r * std::cos(a), centre[c].y() + r * std::sin(a));
            }
        }
        return out;
    }
    case ChromaFrame::Shape::Ellipse: {
        // Inscribed in the picture rather than around it, so at grow 0 this loop *is* the picture's
        // boundary: an oval frame holds an oval picture, which is what one is for.
        constexpr int steps = 72;
        std::vector<Vec2f> out;
        out.reserve(steps);
        for (int i = 0; i < steps; ++i) {
            const float a = 2.f * float(M_PI) * float(i) / float(steps);
            out.emplace_back((hw + g) * std::cos(a), (hh + g) * std::sin(a));
        }
        return out;
    }
    case ChromaFrame::Shape::Outline: {
        if (plate.frame.outline.size() < 3)
            return rectangle();
        // An SVG is a shape, not a size: scale it about its own centre until it sits grow_mm outside
        // the picture, rather than trusting whatever millimetres the file was drawn in.
        Vec2f lo = plate.frame.outline.front(), hi = plate.frame.outline.front();
        for (const Vec2f &p : plate.frame.outline) {
            lo = lo.cwiseMin(p);
            hi = hi.cwiseMax(p);
        }
        const Vec2f centre = 0.5f * (lo + hi);
        const Vec2f half   = 0.5f * (hi - lo);
        if (half.x() <= 1e-4f || half.y() <= 1e-4f)
            return rectangle();
        // Fitted inside the picture at grow 0, so the SVG's shape is the picture's boundary, and
        // grown from there with the rest of the sweep.
        const float k = std::min(hw / half.x(), hh / half.y()) * (1.f + g / std::max(hw, hh));
        std::vector<Vec2f> out;
        out.reserve(plate.frame.outline.size());
        for (const Vec2f &p : plate.frame.outline)
            out.emplace_back((p - centre) * k);
        return out;
    }
    default: return rectangle();
    }
}

namespace {
// Counter-clockwise seen from above, whatever the loop arrived as. An SVG is drawn by somebody else
// and its winding is their business; every face below is wound from these, so they have to agree.
void make_ccw(std::vector<Vec2f> &loop)
{
    double twice_area = 0.;
    for (size_t i = 0; i < loop.size(); ++i) {
        const Vec2f &a = loop[i];
        const Vec2f &b = loop[(i + 1) % loop.size()];
        twice_area += double(a.x()) * double(b.y()) - double(b.x()) * double(a.y());
    }
    if (twice_area < 0.)
        std::reverse(loop.begin(), loop.end());
}

// A ring of the sweep, appended to the mesh at one height. Every ring has the same points in the same
// order - they are the same loop at different distances out - so a band between two of them is a quad
// strip and needs no stitching logic beyond this.
uint32_t append_ring(indexed_triangle_set &its, const std::vector<Vec2f> &loop, float z)
{
    const uint32_t first = uint32_t(its.vertices.size());
    for (const Vec2f &p : loop)
        its.vertices.emplace_back(p.x(), p.y(), z);
    return first;
}

// Outward-facing quads between two rings of the same loop, lower first.
void append_band(indexed_triangle_set &its, uint32_t lower, uint32_t upper, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        const uint32_t a = lower + uint32_t(i), b = lower + uint32_t((i + 1) % count);
        const uint32_t c = upper + uint32_t(i), d = upper + uint32_t((i + 1) % count);
        its.indices.emplace_back(a, b, d);
        its.indices.emplace_back(a, d, c);
    }
}

// The inside of a closed loop, triangulated, with `fill` scattered through it. The fill is what makes
// a picture possible: the relief is carried by vertices, so a face of two triangles shows nothing
// however good the height map is.
std::vector<Vec3i32> triangulate_face(const std::vector<Vec2f> &loop, const std::vector<Vec2f> &fill,
                                      std::vector<Vec2f> &points_out)
{
    points_out = loop;
    points_out.insert(points_out.end(), fill.begin(), fill.end());

    Points pts;
    pts.reserve(points_out.size());
    for (const Vec2f &p : points_out)
        pts.emplace_back(scaled<coord_t>(p.x()), scaled<coord_t>(p.y()));

    Triangulation::HalfEdges edges;
    edges.reserve(loop.size());
    for (uint32_t i = 0; i < uint32_t(loop.size()); ++i)
        edges.emplace_back(i, (i + 1) % uint32_t(loop.size()));
    std::sort(edges.begin(), edges.end());
    return Triangulation::triangulate(pts, edges);
}
} // namespace

ChromaPlateMesh chroma_plate_mesh(const ChromaPlate &plate)
{
    ChromaPlateMesh out;
    const float     base = std::max(0.2f, plate.base_mm);

    // The picture's own boundary, and the sweep from it out to the plate's edge. An empty profile is
    // a plate with no frame at all, where the picture runs to the edge and the only ring is its own.
    std::vector<Vec2f>              picture = chroma_frame_outline(plate, 0.f);
    const std::vector<Vec2f>        profile = chroma_frame_profile(plate.frame, base);
    if (picture.size() < 3)
        return out;
    make_ccw(picture);

    // ---- The picture's face, first, because that is what gets painted ----
    //
    // A grid at the plate's own cell size, kept clear of the boundary so the triangulation is not
    // handed two points in nearly the same place. The cap is on the triangulation, which is a CGAL
    // constrained Delaunay and does not want a million points for a face nobody will look at that
    // closely; the bake subdivides where the picture actually needs it.
    Vec2f lo = picture.front(), hi = picture.front();
    for (const Vec2f &p : picture) {
        lo = lo.cwiseMin(p);
        hi = hi.cwiseMax(p);
    }
    const Vec2f span = hi - lo;
    float       cell = std::max(0.2f, plate.cell_mm);
    constexpr size_t MAX_FILL = 120000;
    while (size_t(span.x() / cell + 1.f) * size_t(span.y() / cell + 1.f) > MAX_FILL)
        cell *= 1.5f;

    Polygon boundary;
    boundary.points.reserve(picture.size());
    for (const Vec2f &p : picture)
        boundary.points.emplace_back(scaled<coord_t>(p.x()), scaled<coord_t>(p.y()));
    const double margin = double(scaled<coord_t>(0.35f * cell));

    std::vector<Vec2f> fill;
    for (float y = lo.y() + cell; y < hi.y() - 0.5f * cell; y += cell)
        for (float x = lo.x() + cell; x < hi.x() - 0.5f * cell; x += cell) {
            const Point q(scaled<coord_t>(x), scaled<coord_t>(y));
            if (!boundary.contains(q))
                continue;
            // Clear of the boundary, so the triangulation never has to resolve two points a micron
            // apart - which it answers with slivers the displacement then tears open.
            bool too_close = false;
            for (size_t i = 0; i < boundary.points.size() && !too_close; ++i)
                too_close = (boundary.points[i] - q).cast<double>().norm() < margin;
            if (!too_close)
                fill.emplace_back(x, y);
        }

    if (plate.orientation == ChromaOrientation::FaceDown) {
        // ---- The picture's face, first as always, but now the plate's bottom ----
        //
        // It prints against the build plate, which is what the mode is for: the one surface finish an
        // FFF printer gives that no layer height reproduces. The relief is displaced up into the plate
        // from here rather than out of it, so this face needs the same grid of vertices the face-up
        // one does.
        std::vector<Vec2f>   down_points;
        std::vector<Vec3i32> down = triangulate_face(picture, fill, down_points);
        const uint32_t       down_first = append_ring(out.its, down_points, 0.f);
        for (const Vec3i32 &t : down)
            // Wound the other way round from the face-up plate's: this face is seen from below.
            out.its.indices.emplace_back(down_first + uint32_t(t.x()), down_first + uint32_t(t.z()),
                                         down_first + uint32_t(t.y()));
        out.picture_triangles = out.its.indices.size();

        // ---- The border around it, in the same plane ----
        //
        // A frame cannot stand proud of the picture on this side: there is nothing below the first
        // layer for it to stand on. So the whole bottom of the plate is one flat plane, which is also
        // what lets the smooth finish run right out to the plate's edge, and the frame's section
        // shapes the back instead.
        uint32_t bed_first = down_first;
        if (profile.size() >= 2 && plate.frame.width_mm > 0.f) {
            std::vector<Vec2f> ring = chroma_frame_outline(plate, plate.frame.width_mm);
            make_ccw(ring);
            if (ring.size() == picture.size()) {
                bed_first = append_ring(out.its, ring, 0.f);
                append_band(out.its, down_first, bed_first, picture.size()); // flat, and facing down
            }
        }

        // ---- The back: the picture's own lid, and the frame's section swept in from the edge ----
        std::vector<Vec2f>   lid_points;
        std::vector<Vec3i32> lid       = triangulate_face(picture, {}, lid_points);
        const uint32_t       lid_first = append_ring(out.its, lid_points, base);
        for (const Vec3i32 &t : lid)
            out.its.indices.emplace_back(lid_first + uint32_t(t.x()), lid_first + uint32_t(t.y()),
                                         lid_first + uint32_t(t.z()));

        // Walked from the plate's edge inwards, which is the order that leaves the outer wall and every
        // step of the section wound the same way. The last sample of a profile is the picture's own
        // loop at the picture's height, which is the lid's ring, so it is not built a second time.
        uint32_t previous = bed_first;
        for (size_t i = 0; i + 1 < profile.size(); ++i) {
            std::vector<Vec2f> ring = chroma_frame_outline(plate, plate.frame.width_mm - profile[i].x());
            make_ccw(ring);
            if (ring.size() != picture.size())
                break; // the shape changed topology on the way in; better a plain plate than a torn one
            const uint32_t here = append_ring(out.its, ring, profile[i].y());
            append_band(out.its, previous, here, picture.size());
            previous = here;
        }
        append_band(out.its, previous, lid_first, picture.size());
        return out;
    }

    std::vector<Vec2f>   face_points;
    std::vector<Vec3i32> face = triangulate_face(picture, fill, face_points);
    const uint32_t       face_first = append_ring(out.its, face_points, base);
    for (const Vec3i32 &t : face)
        out.its.indices.emplace_back(face_first + uint32_t(t.x()), face_first + uint32_t(t.y()),
                                     face_first + uint32_t(t.z()));
    out.picture_triangles = out.its.indices.size();

    // ---- The frame: one ring per profile sample, swept out to the plate's edge ----
    std::vector<Vec2f> edge       = picture; // the plate's outer loop, the picture's own when there is no frame
    uint32_t           edge_first = face_first; // and its vertices, which the outer wall hangs from
    if (profile.size() >= 2) {
        uint32_t     previous = 0;
        const size_t count    = picture.size();
        for (size_t i = profile.size(); i-- > 0;) {
            // Walked from the picture outwards, and handed to append_band() outer ring first.
            //
            // That order is the whole correctness of the sweep's winding. append_band() orients a quad
            // by tangent x (upper - lower), which for the vertical outer wall below - where the offset
            // between the rings is straight up - comes out facing outward, correctly. Here the offset
            // between two rings is mostly radial, and tangent x (radially outward) points *down*: a
            // flat frame band handed over picture-ring-first faces into the plate, and a rising one
            // faces outward and down instead of inward and up. Passing the outer ring as `lower`
            // negates that cross product, which is right for a band that rises, one that falls and one
            // that is level alike. is_closed() normalises its edge keys, so no test here can see this;
            // what shows it is an Ogee or a Mat section rendering inside out.
            std::vector<Vec2f> ring = chroma_frame_outline(plate, plate.frame.width_mm - profile[i].x());
            make_ccw(ring);
            if (ring.size() != count)
                break; // the shape changed topology on the way out; better a plain plate than a torn one
            const uint32_t here = append_ring(out.its, ring, profile[i].y());
            if (i + 1 < profile.size())
                append_band(out.its, here, previous, count);
            else
                append_band(out.its, here, face_first, count); // the picture's own edge to the first ring
            previous   = here;
            edge       = ring;
            edge_first = here;
        }
    }

    // ---- The outer wall and the bottom ----
    //
    // Both hang off the loop that is already there - the outermost ring of the sweep, or the picture's
    // own boundary when there is no frame - rather than a second copy of it at the same coordinates.
    // Two rings in the same place look identical and slice identically, but every edge between them is
    // an edge belonging to one triangle, which is the definition of a mesh with a hole in it.
    const uint32_t bottom = append_ring(out.its, edge, 0.f);
    append_band(out.its, bottom, edge_first, edge.size());

    std::vector<Vec2f> unused;
    for (const Vec3i32 &t : triangulate_face(edge, {}, unused))
        // Wound the other way round: this face is seen from below.
        out.its.indices.emplace_back(bottom + uint32_t(t.x()), bottom + uint32_t(t.z()),
                                     bottom + uint32_t(t.y()));
    return out;
}

indexed_triangle_set chroma_wedge_mesh(const ChromaWedge &wedge)
{
    indexed_triangle_set its;
    const int   steps = std::clamp(wedge.steps, 1, 100);
    const float h     = std::max(0.01f, wedge.layer_height_mm);
    const float depth = std::max(1.f, wedge.step_depth_mm);
    const float width = std::max(5.f, wedge.width_mm);
    const float base  = float(std::max(1, wedge.base_layers)) * h;
    const float len   = depth * float(steps);

    // The base: one slab under the whole staircase, in the contrasting filament.
    append_box(its, Vec3f(0.f, 0.f, 0.f), Vec3f(width, len, base));

    for (int i = 1; i <= steps; ++i) {
        const float y0 = depth * float(i - 1);
        // Every fifth step reaches further out, so a step can be counted in fives rather than one by
        // one - the thing a user does while holding the print up to the light.
        const float w = (i % 5 == 0) ? width + std::max(0.f, wedge.tick_mm) : width;
        append_box(its, Vec3f(0.f, y0, base), Vec3f(w, y0 + depth, base + h * float(i)));
    }
    return its;
}

float chroma_td_from_step(int step, float layer_height_mm)
{
    if (step <= 0 || layer_height_mm <= 0.f)
        return 0.f;
    // The step is the depth at which the filament covers, and covering happens at a tenth of the TD.
    return float(step) * layer_height_mm / CHROMA_TD_OPAQUE_FRACTION;
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

namespace {
// A colour as three bytes, which is what it is on screen and in a filament preset, and the same
// encoding the rest of the texture displacement JSON uses: a float triple would persist the last
// rounding and read back as a different colour than it printed.
nlohmann::json chroma_color_to_json(const ColorRGB &c)
{
    return nlohmann::json::array({ int(c.r() * 255.f + 0.5f), int(c.g() * 255.f + 0.5f), int(c.b() * 255.f + 0.5f) });
}

ColorRGB chroma_color_from_json(const nlohmann::json &j, const ColorRGB &fallback)
{
    if (!j.is_array() || j.size() != 3)
        return fallback;
    for (const nlohmann::json &v : j)
        if (!v.is_number())
            return fallback;
    return ColorRGB(uint8_t(std::clamp(int(j[0]), 0, 255)), uint8_t(std::clamp(int(j[1]), 0, 255)),
                    uint8_t(std::clamp(int(j[2]), 0, 255)));
}

// A key that is absent, or there with the wrong type, leaves the field alone. That is what makes
// every key optional both ways round: an old project is missing the new ones, and a project written by
// a newer build carries ones this one has never heard of.
template<class T> void chroma_read(const nlohmann::json &j, const char *key, T &out)
{
    const auto it = j.find(key);
    if (it == j.end())
        return;
    try {
        out = it->get<T>();
    } catch (...) {
    }
}

// A polyline as flat pairs rather than nested arrays: shorter in the file, and a reader can ignore a
// trailing odd element instead of having to validate every sub-array. The same shape the texture
// displacement layers store their seam edges in.
nlohmann::json points_to_json(const std::vector<Vec2f> &points)
{
    nlohmann::json out = nlohmann::json::array();
    for (const Vec2f &p : points) {
        out.push_back(p.x());
        out.push_back(p.y());
    }
    return out;
}

void points_from_json(const nlohmann::json &j, std::vector<Vec2f> &out)
{
    if (!j.is_array())
        return;
    std::vector<Vec2f> read;
    read.reserve(j.size() / 2);
    for (size_t i = 0; i + 1 < j.size(); i += 2) {
        if (!j[i].is_number() || !j[i + 1].is_number())
            return;
        read.emplace_back(float(j[i]), float(j[i + 1]));
    }
    out = std::move(read);
}

void mask_to_json(nlohmann::json &out, const ChromaMask &mask)
{
    out["width"]  = mask.width;
    out["height"] = mask.height;
    out["runs"]   = mask.runs;
}

void mask_from_json(const nlohmann::json &j, ChromaMask &mask)
{
    chroma_read(j, "width", mask.width);
    chroma_read(j, "height", mask.height);
    chroma_read(j, "runs", mask.runs);
}
} // namespace

void chroma_stack_to_json(nlohmann::json &chroma, const ChromaStack &stack)
{
    chroma["layer_height_mm"] = stack.layer_height_mm;
    chroma["base_layers"]     = stack.base_layers;
    chroma["base_auto"]       = stack.base_auto;
    chroma["dither"]          = stack.dither;
    chroma["applied_ranges"]  = stack.applied_ranges;

    nlohmann::json &bands = chroma["bands"];
    bands                 = nlohmann::json::array();
    for (const ChromaBand &band : stack.bands)
        bands.push_back(nlohmann::json{ { "filament", band.filament.filament },
                                        { "name", band.filament.name },
                                        { "td_mm", band.filament.td_mm },
                                        { "color", chroma_color_to_json(band.filament.color) },
                                        { "layers", band.layers } });

    chroma["lighting"]         = int(stack.lighting);
    chroma["led_color"]        = chroma_color_to_json(stack.led_color);
    chroma["led_intensity"]    = stack.led_intensity;
    chroma["lighting_balance"] = stack.lighting_balance;

    // Only when there is something to say. An edit's mask is the largest thing in a stack by a wide
    // margin, and a project that was never retouched should not carry the key at all - which is also
    // what makes "absent means no edits" the same thing on the way back in as it is for a project
    // written before any of this existed.
    if (!stack.retouch.empty()) {
        nlohmann::json &edits = chroma["retouch"];
        edits                 = nlohmann::json::array();
        for (const ChromaRetouchEdit &edit : stack.retouch.edits) {
            nlohmann::json e;
            mask_to_json(e, edit.region);
            e["target"]  = int(edit.target);
            e["value"]   = edit.value;
            e["enabled"] = edit.enabled;
            edits.push_back(std::move(e));
        }
    }

    // The same rule as the retouch: written only when there is one, so a picture nobody has marked up
    // carries neither key and reads back as the picture it was.
    if (!stack.importance.empty()) {
        nlohmann::json &importance = chroma["importance"];
        importance                 = nlohmann::json::object();
        mask_to_json(importance, stack.importance.region);
        importance["weight"] = stack.importance.weight;
    }

    // The plate, for a picture that made one.
    //
    // The slab's own dimensions are in the mesh already, so this is not what rebuilds it. What it is
    // for is everything about the plate that the mesh cannot say: which way up it prints, which frame
    // out of the gallery it is wearing, and that cross-section, none of which can be recovered from
    // triangles. A project written before this existed has no key and comes back as what it was, a
    // plate nobody can edit any further.
    if (stack.plate.generated) {
        nlohmann::json &p = chroma["plate"];
        p                 = nlohmann::json::object();
        p["generated"]    = stack.plate.generated;
        p["width_mm"]     = stack.plate.width_mm;
        p["height_mm"]    = stack.plate.height_mm;
        p["base_mm"]      = stack.plate.base_mm;
        p["cell_mm"]      = stack.plate.cell_mm;
        p["orientation"]  = int(stack.plate.orientation);

        nlohmann::json &f = p["frame"];
        f                 = nlohmann::json::object();
        f["shape"]        = int(stack.plate.frame.shape);
        f["profile"]      = int(stack.plate.frame.profile);
        f["width_mm"]     = stack.plate.frame.width_mm;
        f["rise_mm"]      = stack.plate.frame.rise_mm;
        f["corner_mm"]    = stack.plate.frame.corner_mm;
        if (!stack.plate.frame.outline.empty())
            f["outline"] = points_to_json(stack.plate.frame.outline);
        if (!stack.plate.frame.frame_id.empty())
            f["frame_id"] = stack.plate.frame.frame_id;
        if (!stack.plate.frame.profile_points.empty())
            f["profile_points"] = points_to_json(stack.plate.frame.profile_points);
    }
}

void chroma_stack_from_json(const nlohmann::json &chroma, ChromaStack &stack)
{
    if (!chroma.is_object())
        return;

    chroma_read(chroma, "layer_height_mm", stack.layer_height_mm);
    chroma_read(chroma, "base_layers", stack.base_layers);
    chroma_read(chroma, "base_auto", stack.base_auto);
    chroma_read(chroma, "dither", stack.dither);
    chroma_read(chroma, "applied_ranges", stack.applied_ranges);

    if (const auto bands = chroma.find("bands"); bands != chroma.end() && bands->is_array()) {
        stack.bands.clear();
        for (const nlohmann::json &b : *bands) {
            if (!b.is_object())
                continue;
            ChromaBand band;
            chroma_read(b, "filament", band.filament.filament);
            chroma_read(b, "name", band.filament.name);
            chroma_read(b, "td_mm", band.filament.td_mm);
            if (const auto col = b.find("color"); col != b.end())
                band.filament.color = chroma_color_from_json(*col, band.filament.color);
            chroma_read(b, "layers", band.layers);
            band.layers = std::max(1, band.layers);
            stack.bands.push_back(band);
        }
    }

    int lighting = int(stack.lighting);
    chroma_read(chroma, "lighting", lighting);
    // Anything this build does not know is front lit, which is the mode every other part of the model
    // assumes and the only one a stack can be edited back out of.
    stack.lighting = (lighting == int(ChromaLighting::BackLit))   ? ChromaLighting::BackLit :
                     (lighting == int(ChromaLighting::BothLit)) ? ChromaLighting::BothLit :
                                                                  ChromaLighting::FrontLit;
    chroma_read(chroma, "lighting_balance", stack.lighting_balance);
    stack.lighting_balance = std::clamp(stack.lighting_balance, 0.f, 1.f);
    if (const auto led = chroma.find("led_color"); led != chroma.end())
        stack.led_color = chroma_color_from_json(*led, stack.led_color);
    chroma_read(chroma, "led_intensity", stack.led_intensity);
    // A negative lamp is not a lamp. The upper end is left alone: an intensity above 1 is a bright
    // light, which the palette clamps rather than refuses.
    stack.led_intensity = std::max(0.f, stack.led_intensity);

    if (const auto imp = chroma.find("importance"); imp != chroma.end() && imp->is_object()) {
        mask_from_json(*imp, stack.importance.region);
        chroma_read(*imp, "weight", stack.importance.weight);
        // A negative weight is not a preference, it is a sign error; 1 is "no preference", which is
        // also what an empty region means.
        stack.importance.weight = std::max(0.f, stack.importance.weight);
    }

    if (const auto p = chroma.find("plate"); p != chroma.end() && p->is_object()) {
        chroma_read(*p, "generated", stack.plate.generated);
        chroma_read(*p, "width_mm", stack.plate.width_mm);
        chroma_read(*p, "height_mm", stack.plate.height_mm);
        chroma_read(*p, "base_mm", stack.plate.base_mm);
        chroma_read(*p, "cell_mm", stack.plate.cell_mm);
        int orientation = int(stack.plate.orientation);
        chroma_read(*p, "orientation", orientation);
        // Anything this build does not know is face up, which is the plate every other part of the
        // model assumes and the only one a picture painted on a model can be.
        stack.plate.orientation = (orientation == int(ChromaOrientation::FaceDown)) ? ChromaOrientation::FaceDown :
                                                                                      ChromaOrientation::FaceUp;

        if (const auto f = p->find("frame"); f != p->end() && f->is_object()) {
            ChromaFrame &frame = stack.plate.frame;
            int          shape = int(frame.shape), profile = int(frame.profile);
            chroma_read(*f, "shape", shape);
            chroma_read(*f, "profile", profile);
            frame.shape   = ChromaFrame::Shape(std::clamp(shape, 0, 3));
            frame.profile = ChromaFrame::Profile(std::clamp(profile, 0, 7));
            chroma_read(*f, "width_mm", frame.width_mm);
            chroma_read(*f, "rise_mm", frame.rise_mm);
            chroma_read(*f, "corner_mm", frame.corner_mm);
            chroma_read(*f, "frame_id", frame.frame_id);
            if (const auto o = f->find("outline"); o != f->end())
                points_from_json(*o, frame.outline);
            if (const auto s = f->find("profile_points"); s != f->end())
                points_from_json(*s, frame.profile_points);
            // One sample is not a cross-section, and chroma_frame_profile() would fall back to the
            // enum anyway; dropping it here keeps the gallery from showing a frame that is not one.
            if (frame.profile_points.size() < 2)
                frame.profile_points.clear();
        }
    }

    if (const auto edits = chroma.find("retouch"); edits != chroma.end() && edits->is_array()) {
        stack.retouch.edits.clear();
        for (const nlohmann::json &e : *edits) {
            if (!e.is_object())
                continue;
            ChromaRetouchEdit edit;
            mask_from_json(e, edit.region);
            int target = int(edit.target);
            chroma_read(e, "target", target);
            edit.target = (target == int(ChromaRetouchEdit::Target::Band)) ? ChromaRetouchEdit::Target::Band :
                                                                            ChromaRetouchEdit::Target::Delta;
            chroma_read(e, "value", edit.value);
            chroma_read(e, "enabled", edit.enabled);
            // An edit with no region covers nothing and can neither apply nor be shown; dropping it
            // here keeps a malformed run list out of the panel's list of strokes.
            if (!edit.region.empty())
                stack.retouch.edits.push_back(std::move(edit));
        }
    }
}

} // namespace Slic3r
