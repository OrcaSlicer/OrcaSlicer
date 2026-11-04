#include "ChromaDiagnostics.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "ChromaStack.hpp"
#include "Color.hpp"
#include "I18N.hpp"
#include "TextureToColor/ColorUtils.hpp"
#include "format.hpp"

namespace Slic3r {

namespace {

// CIELAB under D65, from non-linear sRGB in [0, 1], and the plain Euclidean distance in it (CIE76).
//
// This is a copy of the pair ChromaStack.cpp keeps file-local, which is not a thing to do lightly. It
// is here because the two have to agree exactly: every number this file reports is compared against a
// threshold the solve and the filament picker also use, so a second conversion that rounded a shade
// differently would show a user a finding about a collision the picker had already decided was not one.
// The right fix is for the pair to be declared somewhere both files can reach, and until it is, this
// is twenty lines that must be changed in both places or in neither.
struct Lab
{
    float l = 0.f, a = 0.f, b = 0.f;
};

float srgb_to_linear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

Lab srgb_to_lab(float r, float g, float b)
{
    const float rl = srgb_to_linear(r), gl = srgb_to_linear(g), bl = srgb_to_linear(b);
    const float x = (0.4124f * rl + 0.3576f * gl + 0.1805f * bl) / 0.95047f;
    const float y = (0.2126f * rl + 0.7152f * gl + 0.0722f * bl);
    const float z = (0.0193f * rl + 0.1192f * gl + 0.9505f * bl) / 1.08883f;
    auto        f = [](float t) { return t > 0.008856f ? std::cbrt(t) : (7.787f * t + 16.f / 116.f); };
    const float fx = f(x), fy = f(y), fz = f(z);
    return Lab{ 116.f * fy - 16.f, 500.f * (fx - fy), 200.f * (fy - fz) };
}

Lab lab_of(const ColorRGB &c) { return srgb_to_lab(c.r(), c.g(), c.b()); }

float delta_e76(const Lab &p, const Lab &q)
{
    const float dl = p.l - q.l, da = p.a - q.a, db = p.b - q.b;
    return std::sqrt(dl * dl + da * da + db * db);
}

// CIEDE2000 between two of the stack's own colours.
//
// Which metric goes where is not a preference, it is what the two questions are. Everything measured
// against the picture - how far a bin is from the nearest printable colour, which level serves it, the
// mean the summary reports - uses CIE76, because that is what chroma_stack_error() minimised when the
// filaments were picked and what the interactive solve matched with: a finding computed on a different
// metric from the decision it is commenting on is a finding that argues with the panel next to it.
//
// The judgements about the stack against itself - "can it tell these two of its own colours apart" -
// use CIEDE2000 instead. They are threshold tests right down at a just-noticeable difference, which is
// exactly the range CIE76 gets wrong (it overstates differences in saturated blues by a factor of
// two), and there are only a few tens of thousands of them because the bands partition at most
// CHROMA_MAX_RELIEF_LAYERS levels between them however many bands there are.
float delta_e2000(const ColorRGB &a, const ColorRGB &b)
{
    using tex2color::color_utils::ColorDouble;
    const ColorDouble p{ double(a.r()), double(a.g()), double(a.b()) };
    const ColorDouble q{ double(b.r()), double(b.g()), double(b.b()) };
    return float(tex2color::color_utils::calc_rgb_color_difference_by_ciede2000_srgb01(p, q));
}

// Spelled out rather than taken from M_PI, which is not in the C++ standard and is absent from a
// strict MSVC build unless _USE_MATH_DEFINES happens to have been defined first.
constexpr float RAD_TO_DEG = 57.2957795130823f;

ChromaMapCoord coord_of(const Lab &lab)
{
    ChromaMapCoord out;
    out.lightness = lab.l;
    out.chroma_ab = std::sqrt(lab.a * lab.a + lab.b * lab.b);
    float h       = std::atan2(lab.b, lab.a) * RAD_TO_DEG;
    if (h < 0.f)
        h += 360.f;
    out.hue_deg = h;
    return out;
}

// What to call a band in a sentence.
//
// The filament preset's name, because that is what the user chose it by and what the band's row in the
// panel says. A stack saved before names were stored has none, and a hex colour is then a better label
// than an empty pair of brackets: it is still the one thing that tells six similar greys apart. Neither
// is a translated word, so putting it in a message costs the translator nothing.
std::string band_label(const ChromaBand &band)
{
    return band.filament.name.empty() ? encode_color(band.filament.color) : band.filament.name;
}

// A band's share of the relief, which is the currency the findings about wasted print time are priced
// in. Zero relief gives zero rather than a division by zero: a stack with no layers in it has no
// findings of that kind to make.
float relief_share(int layers, int relief_layers)
{
    return relief_layers > 0 ? float(std::max(0, layers)) / float(relief_layers) : 0.f;
}

} // namespace

ChromaDiagnostics chroma_diagnose(const ChromaHistogram &picture, const ChromaStack &stack)
{
    ChromaDiagnostics out;

    // What this stack can reach, from the one place that knows: the same palette the solve matched
    // against, the preview shader draws and the picker scored. Nothing here recomputes a colour.
    const std::vector<ColorRGB> palette = chroma_palette(stack);
    const int                   base    = std::clamp(std::max(0, stack.base_layers), 0, int(palette.size()));
    if (palette.empty() || int(palette.size()) <= base)
        return out;

    out.relief_layers   = stack.relief_layers();
    out.distinct_levels = chroma_distinct_levels(stack);

    // Which band each printed column height belongs to, built exactly the way chroma_palette() lays
    // the layers out: the base first in the bottom band's filament, then the bands bottom-up. A level
    // at or below the base is -1, the bare base, which is a column with none of the picture on it.
    std::vector<int> band_of(palette.size(), -1);
    std::vector<int> layer_in_band(palette.size(), 0);
    {
        int level = base;
        for (size_t b = 0; b < stack.bands.size(); ++b) {
            for (int i = 1; i <= stack.bands[b].layers; ++i) {
                const size_t k = size_t(level) + size_t(i);
                if (k >= palette.size())
                    break;
                band_of[k]       = int(b);
                layer_in_band[k] = i;
            }
            level += std::max(0, stack.bands[b].layers);
        }
    }

    // The reach: one point per column height the stack can actually print, in level order so a chart
    // can join consecutive entries into the curve the palette walks.
    std::vector<Lab> reach_lab(palette.size());
    for (size_t k = size_t(base); k < palette.size(); ++k)
        reach_lab[k] = lab_of(palette[k]);

    out.reach.reserve(palette.size() - size_t(base));
    for (size_t k = size_t(base); k < palette.size(); ++k) {
        ChromaReach r;
        r.color         = palette[k];
        r.at            = coord_of(reach_lab[k]);
        r.level         = int(k);
        r.band          = band_of[k];
        r.layer_in_band = layer_in_band[k];
        r.step_de       = k > size_t(base) ? delta_e76(reach_lab[k - 1], reach_lab[k]) : 0.f;
        out.reach.push_back(r);
    }

    // The wants, measured against the reach. The bins' weights already sum to 1, so the sums below are
    // shares of the picture without any further normalising.
    std::vector<float> served(stack.bands.size(), 0.f);
    double             mean = 0., worst = 0., unreachable = 0., matched = 0.;
    out.wants.reserve(picture.bins.size());
    for (const std::pair<ColorRGB, float> &bin : picture.bins) {
        const Lab target = lab_of(bin.first);
        float     best   = std::numeric_limits<float>::max();
        size_t    best_k = size_t(base);
        for (size_t k = size_t(base); k < palette.size(); ++k) {
            const float d = delta_e76(target, reach_lab[k]);
            if (d < best) {
                best   = d;
                best_k = k;
            }
        }

        ChromaWant w;
        w.color         = bin.first;
        w.at            = coord_of(target);
        w.weight        = bin.second;
        w.nearest_de    = best;
        w.nearest_level = int(best_k);
        out.wants.push_back(w);

        out.reach[best_k - size_t(base)].serves += bin.second;
        if (const int b = band_of[best_k]; b >= 0 && size_t(b) < served.size())
            served[size_t(b)] += bin.second;

        mean += double(bin.second) * double(best);
        worst = std::max(worst, double(best));
        if (best > CHROMA_DIAG_UNREACHABLE_DE)
            unreachable += double(bin.second);
        if (best <= CHROMA_DIAG_GOOD_DE)
            matched += double(bin.second);
    }
    out.mean_de            = mean;
    out.worst_de           = worst;
    out.unreachable_weight = float(unreachable);
    out.matched_weight     = float(matched);

    // Which levels belong to which band, so the pairwise tests below walk a band rather than the whole
    // palette. The bands partition the relief, so these add up to at most CHROMA_MAX_RELIEF_LAYERS
    // entries between them whatever the stack is.
    std::vector<std::vector<size_t>> levels_of(stack.bands.size());
    for (size_t k = size_t(base); k < palette.size(); ++k)
        if (const int b = band_of[k]; b >= 0 && size_t(b) < levels_of.size())
            levels_of[size_t(b)].push_back(k);

    // How many layers each band spends on a colour the layer below already showed, taken from the
    // library's own answer rather than from a threshold of this file's: chroma_trim_bands() is what the
    // panel's Trim button does, so a finding that disagrees with it would be a finding about a button.
    std::vector<int> wasted(stack.bands.size(), 0);
    {
        ChromaStack trimmed = stack;
        chroma_trim_bands(trimmed);
        if (trimmed.bands.size() == stack.bands.size())
            for (size_t b = 0; b < stack.bands.size(); ++b) {
                wasted[b] = std::max(0, stack.bands[b].layers - trimmed.bands[b].layers);
                out.wasted_layers += wasted[b];
            }
    }

    // A whole-stack finding has no share of its own to be priced in, so it is priced in how badly the
    // stack is doing: an order that wastes a filament matters when the picture is coming out wrong and
    // is a quibble when it is coming out right.
    const float overall_cost = std::clamp(float(out.mean_de) / CHROMA_DIAG_UNREACHABLE_DE, 0.f, 1.f);

    std::vector<ChromaFinding> found;
    const auto                 add = [&found](ChromaFindingKind kind, std::string text, float cost, int band_a,
                           int band_b) {
        ChromaFinding f;
        f.kind   = kind;
        f.text   = std::move(text);
        f.cost   = std::clamp(cost, 0.f, 1.f);
        f.band_a = band_a;
        f.band_b = band_b;
        found.push_back(std::move(f));
    };

    for (size_t b = 0; b < stack.bands.size(); ++b) {
        const ChromaBand &band = stack.bands[b];

        // Reported only from two layers up. One wasted layer is inside the half-dE the trim calls
        // "changed nothing" and is not worth a sentence, and it also keeps the message free of a count
        // that could come out as "its top 1 layers" - libslic3r has no plural lookup to fix that with.
        if (wasted[b] >= 2)
            add(ChromaFindingKind::WastedLayers,
                Slic3r::format(_u8L("Band %1% (%2%) spends its top %3% layers on the color the layer below already "
                                    "showed, because the filament has stopped letting anything through. Trim the "
                                    "bands, or move those layers to a band that is still changing the picture."),
                               b + 1, band_label(band), wasted[b]),
                relief_share(wasted[b], out.relief_layers), int(b), -1);

        // A band nothing lands on is a swap paid for and nothing printed in it. Only worth saying while
        // there is still a stack left after dropping it, and only when there was a picture to land on.
        if (!picture.empty() && stack.bands.size() > 2 && !levels_of[b].empty() &&
            served[b] < CHROMA_DIAG_UNUSED_SHARE)
            add(ChromaFindingKind::BandUnused,
                Slic3r::format(_u8L("Nothing in the picture would be printed at any height inside band %1% (%2%): "
                                    "every color that band reaches, the picture would rather have from somewhere "
                                    "else in the stack. Drop the band and give its layers to the others."),
                               b + 1, band_label(band)),
                relief_share(band.layers, out.relief_layers), int(b), -1);

        // A band with no intermediate colours at all. The filaments look right, the bands look long
        // enough, and the picture prints as flat steps, which reads as the colour model being wrong
        // when it is the layer height; nothing else makes it visible. A one-layer band has no
        // intermediate colours by construction and is not this problem.
        if (band.layers >= 2 && stack.layer_height_mm > 0.f) {
            const float to_opaque = chroma_layers_to_opaque(band.filament.td_mm, stack.layer_height_mm);
            if (to_opaque < 2.f)
                add(ChromaFindingKind::LayerHeightTooCoarse,
                    Slic3r::format(_u8L("Band %1% (%2%) covers what is under it in %3$.1f layers at a layer height "
                                        "of %4$.2f mm, so it has no intermediate colors and the picture can only "
                                        "use that filament or not use it. Print at a finer layer height, or give "
                                        "that band a more translucent filament."),
                                   b + 1, band_label(band), to_opaque, stack.layer_height_mm),
                    relief_share(band.layers, out.relief_layers), int(b), -1);
        }
    }

    // Two bands competing for the same depths. For each ordered pair, how much of one band's own range
    // of colours the other already reaches; if nearly all of it does, the filament change between them
    // is being paid for and buying nothing, and the band that is covered is the one to drop.
    std::vector<std::pair<size_t, size_t>> collided;
    for (size_t a = 0; a < stack.bands.size(); ++a)
        for (size_t b = a + 1; b < stack.bands.size(); ++b) {
            if (levels_of[a].empty() || levels_of[b].empty())
                continue;
            const auto covered = [&](const std::vector<size_t> &mine, const std::vector<size_t> &theirs) {
                size_t n = 0;
                for (const size_t k : mine)
                    for (const size_t j : theirs)
                        if (delta_e2000(palette[k], palette[j]) <= CHROMA_DIAG_INDISTINCT_DE) {
                            ++n;
                            break;
                        }
                return float(n) / float(mine.size());
            };
            const float b_by_a = covered(levels_of[b], levels_of[a]);
            const float a_by_b = covered(levels_of[a], levels_of[b]);
            // The more covered of the two is the one that buys nothing, and it is the one the sentence
            // names first, because it is the one to drop.
            const bool  drop_b = b_by_a >= a_by_b;
            const size_t loser = drop_b ? b : a, keeper = drop_b ? a : b;
            const float  share = std::max(b_by_a, a_by_b);
            if (share < CHROMA_DIAG_COLLISION_SHARE)
                continue;
            collided.emplace_back(a, b);
            add(ChromaFindingKind::BandsCollide,
                Slic3r::format(_u8L("Band %1% (%2%) reaches almost nothing that band %3% (%4%) does not already "
                                    "reach, so the filament change between them buys little. Drop one of the two, "
                                    "or put a filament further from the other one in it."),
                               loser + 1, band_label(stack.bands[loser]), keeper + 1,
                               band_label(stack.bands[keeper])),
                share * relief_share(stack.bands[loser].layers, out.relief_layers), int(loser), int(keeper));
        }

    // Two adjacent filaments the relief cannot step between.
    //
    // Adjacent only, because this is about the swap: that is where the ramp has to take a step, and two
    // bands of the same lightness with something different between them still read as three steps. And
    // about lightness rather than about colour, because lightness is the axis a column's height reaches
    // the eye on - two filaments of the same L* and different hue give a band boundary that changes the
    // tint and not the tone, which is not what the layers were spent on.
    for (size_t b = 0; b + 1 < stack.bands.size(); ++b) {
        const size_t c = b + 1;
        // Suppressed where the palette has already said the same thing more strongly: a pair whose
        // reachable colours collide does not also need telling that its filaments are similar.
        if (std::find(collided.begin(), collided.end(), std::make_pair(b, c)) != collided.end())
            continue;
        const float lb = lab_of(stack.bands[b].filament.color).l;
        const float lc = lab_of(stack.bands[c].filament.color).l;
        const float dl = std::abs(lb - lc);
        if (dl >= CHROMA_DIAG_LIGHTNESS_STEP)
            continue;
        add(ChromaFindingKind::FilamentsTooClose,
            Slic3r::format(_u8L("Band %1% (%2%) and band %3% (%4%) are within %5$.0f of each other in lightness, so "
                                "the step between them reads as one band rather than two and the swap is almost "
                                "invisible. Put a filament of clearly different lightness in one of them."),
                           b + 1, band_label(stack.bands[b]), c + 1, band_label(stack.bands[c]), dl),
            relief_share(stack.bands[c].layers, out.relief_layers), int(b), int(c));
    }

    // Colours the picture wants and no arrangement of these filaments can print. Reported once for the
    // whole picture rather than per colour: the fix is the same for all of them, and the swatch says
    // which colours better than any number of sentences would.
    if (out.unreachable_weight >= CHROMA_DIAG_UNREACHABLE_SHARE) {
        const ChromaWant *furthest = nullptr;
        for (const ChromaWant &w : out.wants)
            if (w.nearest_de > CHROMA_DIAG_UNREACHABLE_DE &&
                (furthest == nullptr || w.weight * w.nearest_de > furthest->weight * furthest->nearest_de))
                furthest = &w;
        if (furthest != nullptr) {
            add(ChromaFindingKind::ColorsUnreachable,
                Slic3r::format(_u8L("Part of the picture is in colors this stack cannot reach: the worst of them is "
                                    "%1$.0f dE from the nearest color it can print, and anything over %2$.0f means "
                                    "no number of layers will help. Add a filament nearer those colors, or pull the "
                                    "picture's saturation down towards what the stack reaches."),
                               out.worst_de, CHROMA_DIAG_UNREACHABLE_DE),
                out.unreachable_weight, -1, -1);
            ChromaFinding &finding = found.back();
            finding.color          = furthest->color;
            finding.has_color      = true;
        }
    }

    // A translucent filament with nothing under it to show.
    //
    // Front lit, the base prints in the bottom band's own filament, so the bottom band is that filament
    // over itself: however far light gets into it, what comes back out is the same colour, and every
    // layer of it is flat. A translucent filament there is the one filament in the drawer whose whole
    // point is wasted, and the fix is an order rather than a length. Back lit the base is inside the
    // column instead and the bottom band's transmission is what the picture is made of, so this is
    // front lit only.
    if (stack.lighting == ChromaLighting::FrontLit && stack.bands.size() >= 2 && stack.bands.front().layers >= 2) {
        const float bottom = stack.bands.front().filament.td_mm;
        float       least  = bottom;
        for (size_t b = 1; b < stack.bands.size(); ++b)
            least = std::min(least, stack.bands[b].filament.td_mm);
        if (bottom - least >= CHROMA_DIAG_TD_GAP_MM)
            add(ChromaFindingKind::TranslucentWasted,
                Slic3r::format(_u8L("Band 1 (%1%) is the most translucent filament in the stack and it is at the "
                                    "bottom, where what is under it is the base printed in that same filament, so "
                                    "its transmission distance of %2$.1f mm shows nothing. Put the most opaque "
                                    "filament in the bottom band and move this one further up."),
                               band_label(stack.bands.front()), bottom),
                overall_cost, 0, -1);
    }

    // The order the filaments run in.
    //
    // Front lit it should darken going up: a dark filament over a light one reads as a midtone after one
    // layer and as the dark colour after four, which is the long tonal ramp, while the reverse gives a
    // few washed-out steps and then nothing. Back lit it is the other way, because a dark band starves
    // everything above it. Both lit is neither and is left alone: the sum of the two walks has no
    // convention to be the wrong way round of.
    if (stack.bands.size() >= 2 && stack.lighting != ChromaLighting::BothLit) {
        int up = 0, down = 0;
        for (size_t b = 0; b + 1 < stack.bands.size(); ++b) {
            const float lb = lab_of(stack.bands[b].filament.color).l;
            const float lc = lab_of(stack.bands[b + 1].filament.color).l;
            if (lc > lb + CHROMA_DIAG_LIGHTNESS_STEP)
                ++up;
            else if (lb > lc + CHROMA_DIAG_LIGHTNESS_STEP)
                ++down;
        }
        if (stack.lighting == ChromaLighting::FrontLit && up > down)
            add(ChromaFindingKind::OrderReversed,
                _u8L("The filaments run dark at the bottom of the stack to light at the top. A dark filament over a "
                     "light one reads as a midtone after one layer and as the dark color after four, which is what "
                     "buys the long tonal ramp, so a front lit stack wants its light filaments at the bottom. "
                     "Reverse the order of the bands."),
                overall_cost, -1, -1);
        else if (stack.lighting == ChromaLighting::BackLit && down > up)
            add(ChromaFindingKind::OrderReversed,
                _u8L("The filaments run light at the bottom of the stack to dark at the top, and this plate is lit "
                     "from behind, where a dark band starves everything above it of light. A back lit stack wants "
                     "its dark filaments at the bottom. Reverse the order of the bands."),
                overall_cost, -1, -1);
    }

    // Worst first, and stably, so the same stack and the same picture always produce the same list in
    // the same order: a panel whose findings reshuffle between two identical solves is a panel nobody
    // trusts.
    std::stable_sort(found.begin(), found.end(),
                     [](const ChromaFinding &a, const ChromaFinding &b) { return a.cost > b.cost; });

    // At most two of a kind. Six findings that are all "this band wastes layers" is one finding said
    // six times, and it crowds out the one that says the picture cannot be printed at all.
    for (const ChromaFinding &f : found) {
        if (out.findings.size() >= CHROMA_DIAG_MAX_FINDINGS)
            break;
        const size_t same = size_t(std::count_if(out.findings.begin(), out.findings.end(),
                                                 [&f](const ChromaFinding &g) { return g.kind == f.kind; }));
        if (same < 2)
            out.findings.push_back(f);
    }

    // Said out loud rather than left as an empty list, because "nothing is wrong" and "this has not
    // been analysed yet" look identical to a panel and only one of them is worth the user's attention.
    if (out.findings.empty()) {
        ChromaFinding well;
        well.kind = ChromaFindingKind::WellMatched;
        well.text = _u8L("Nothing in this stack is working against the picture: it reaches the colors the picture is "
                         "mostly made of, and no band is spending a filament change or a run of layers on nothing.");
        out.findings.push_back(std::move(well));
    }

    return out;
}

ChromaDiagnostics chroma_diagnose(const uint8_t *rgb, int width, int height, const ChromaStack &stack)
{
    // The same weighting the filament picker and the colour suggestions use, because this is a comment
    // on what they chose: coverage boosted for a colour far from the picture's own average, and again
    // for whatever part of the picture the user painted as the part that matters. Judging a stack picked
    // that way against a histogram weighted by coverage alone would report the background's complaints
    // about a stack chosen for the subject.
    return chroma_diagnose(chroma_histogram(rgb, width, height, 192, CHROMA_VIVIDNESS_RECOMMENDED,
                                            stack.importance),
                           stack);
}

} // namespace Slic3r
