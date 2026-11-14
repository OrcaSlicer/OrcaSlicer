#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "libslic3r/ChromaDiagnostics.hpp"
#include "libslic3r/ChromaStack.hpp"
#include "libslic3r/Color.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

ChromaFilament filament(int index, const ColorRGB &color, float td_mm)
{
    ChromaFilament f;
    f.filament = index;
    f.color    = color;
    f.td_mm    = td_mm;
    f.name     = "filament " + std::to_string(index);
    return f;
}

ChromaBand band(const ChromaFilament &f, int layers)
{
    ChromaBand b;
    b.filament = f;
    b.layers   = layers;
    return b;
}

// TD 4 at 0.08 mm layers covers in five layers, which is a band with a real gradient in it. TD 1 covers
// in one and a quarter, which is a band with no intermediate colours at all - the two ends of what the
// diagnostics have to behave sensibly over.
const ChromaFilament WHITE = filament(0, ColorRGB(1.f, 1.f, 1.f), 4.f);
const ChromaFilament BLACK = filament(1, ColorRGB(0.f, 0.f, 0.f), 4.f);
const ChromaFilament RED   = filament(2, ColorRGB(1.f, 0.f, 0.f), 4.f);
// sRGB 0.5 grey lands at L* 53.4 and pure red at L* 53.2, so this pair is two filaments of the same
// lightness and different hue, which is the one thing the lightness finding exists to catch.
const ChromaFilament MID_GREY = filament(3, ColorRGB(0.5f, 0.5f, 0.5f), 4.f);

ChromaStack stack_of(std::vector<ChromaBand> bands, int base_layers = 4)
{
    ChromaStack s;
    s.bands           = std::move(bands);
    s.layer_height_mm = 0.08f;
    s.base_layers     = base_layers;
    return s;
}

// A picture made of the stack's own printable colours, one pixel per column height. Whatever the
// histogram's bucketing does to it, every bin it produces sits between two colours the stack can
// print, so nothing in it is out of reach.
std::vector<uint8_t> picture_of_palette(const ChromaStack &stack)
{
    const std::vector<ColorRGB> palette = chroma_palette(stack);
    const size_t                base    = size_t(std::max(0, stack.base_layers));
    std::vector<uint8_t>        out;
    for (size_t k = base; k < palette.size(); ++k) {
        out.push_back(uint8_t(palette[k].r() * 255.f + 0.5f));
        out.push_back(uint8_t(palette[k].g() * 255.f + 0.5f));
        out.push_back(uint8_t(palette[k].b() * 255.f + 0.5f));
    }
    return out;
}

std::vector<uint8_t> flat_picture(const ColorRGB &color, int pixels)
{
    std::vector<uint8_t> out;
    for (int i = 0; i < pixels; ++i) {
        out.push_back(uint8_t(color.r() * 255.f + 0.5f));
        out.push_back(uint8_t(color.g() * 255.f + 0.5f));
        out.push_back(uint8_t(color.b() * 255.f + 0.5f));
    }
    return out;
}

ChromaHistogram histogram_of(const std::vector<uint8_t> &rgb)
{
    return chroma_histogram(rgb.data(), int(rgb.size() / 3), 1);
}

bool has_kind(const ChromaDiagnostics &d, ChromaFindingKind kind)
{
    return std::any_of(d.findings.begin(), d.findings.end(),
                       [kind](const ChromaFinding &f) { return f.kind == kind; });
}

size_t count_kind(const ChromaDiagnostics &d, ChromaFindingKind kind)
{
    return size_t(std::count_if(d.findings.begin(), d.findings.end(),
                                [kind](const ChromaFinding &f) { return f.kind == kind; }));
}

const ChromaFinding *finding_of(const ChromaDiagnostics &d, ChromaFindingKind kind)
{
    for (const ChromaFinding &f : d.findings)
        if (f.kind == kind)
            return &f;
    return nullptr;
}

} // namespace

TEST_CASE("A stack that prints nothing is not diagnosed", "[ChromaDiagnostics]")
{
    const ChromaStack       empty;
    const ChromaDiagnostics d = chroma_diagnose(ChromaHistogram{}, empty);

    REQUIRE(d.empty());
    REQUIRE(d.reach.empty());
    REQUIRE(d.wants.empty());
    // No stack is not a problem with a stack: a user who has not chosen filaments yet knows it.
    REQUIRE(d.findings.empty());
}

TEST_CASE("The reach holds one point for every column height the stack can print", "[ChromaDiagnostics]")
{
    const ChromaStack       stack = stack_of({ band(WHITE, 6), band(BLACK, 6) });
    const ChromaDiagnostics d     = chroma_diagnose(ChromaHistogram{}, stack);

    // Column heights from the bare base up to the top of the stack, inclusive at both ends.
    REQUIRE(d.reach.size() == size_t(stack.total_layers() - stack.base_layers + 1));
    REQUIRE(d.reach.front().level == stack.base_layers);
    REQUIRE(d.reach.back().level == stack.total_layers());
    REQUIRE(d.relief_layers == 12);
}

TEST_CASE("A reach point says which band its top layer is in", "[ChromaDiagnostics]")
{
    const ChromaStack       stack = stack_of({ band(WHITE, 6), band(BLACK, 6) });
    const ChromaDiagnostics d     = chroma_diagnose(ChromaHistogram{}, stack);

    // The bare base is nothing of the picture printed at all, so it belongs to no band.
    REQUIRE(d.reach.front().band == -1);
    REQUIRE(d.reach.front().layer_in_band == 0);
    // The first layer of the picture is the bottom band's first layer.
    REQUIRE(d.reach[1].band == 0);
    REQUIRE(d.reach[1].layer_in_band == 1);
    // Six layers in, the bottom band is finished and the next one starts over again at one.
    REQUIRE(d.reach[6].band == 0);
    REQUIRE(d.reach[6].layer_in_band == 6);
    REQUIRE(d.reach[7].band == 1);
    REQUIRE(d.reach[7].layer_in_band == 1);
    REQUIRE(d.reach.back().band == 1);
}

TEST_CASE("The bare base has nothing below it to have changed", "[ChromaDiagnostics]")
{
    const ChromaStack       stack = stack_of({ band(WHITE, 6), band(BLACK, 6) });
    const ChromaDiagnostics d     = chroma_diagnose(ChromaHistogram{}, stack);

    REQUIRE_THAT(d.reach.front().step_de, WithinAbs(0.f, 1e-6f));
    // A layer of black over white is the largest step this stack takes, so it is comfortably visible.
    REQUIRE(d.reach[7].step_de > 1.f);
}

TEST_CASE("A colour's chart position is its own place in CIELAB", "[ChromaDiagnostics]")
{
    // The base prints in the bottom band's filament, so the first reach point is that filament's own
    // colour exactly and its coordinates are the textbook ones for sRGB red.
    const ChromaStack       stack = stack_of({ band(RED, 4), band(BLACK, 4) });
    const ChromaDiagnostics d     = chroma_diagnose(ChromaHistogram{}, stack);

    REQUIRE_THAT(d.reach.front().at.lightness, WithinAbs(53.24f, 0.5f));
    REQUIRE_THAT(d.reach.front().at.hue_deg, WithinAbs(39.97f, 0.5f));
    REQUIRE_THAT(d.reach.front().at.chroma_ab, WithinAbs(104.55f, 0.5f));
}

TEST_CASE("A neutral colour sits on the chart's neutral axis", "[ChromaDiagnostics]")
{
    const ChromaStack       stack = stack_of({ band(MID_GREY, 4), band(BLACK, 4) });
    const ChromaDiagnostics d     = chroma_diagnose(ChromaHistogram{}, stack);

    REQUIRE_THAT(d.reach.front().at.chroma_ab, WithinAbs(0.f, 0.5f));
    REQUIRE_THAT(d.reach.front().at.lightness, WithinAbs(53.39f, 0.5f));
}

TEST_CASE("The mean colour difference is the one the filament picker scored stacks by", "[ChromaDiagnostics]")
{
    const ChromaStack stack = stack_of({ band(WHITE, 6), band(BLACK, 6) });
    // Half colours the stack prints and half a green it has no filament anywhere near, so the mean
    // being compared is a real number rather than two zeroes agreeing with each other.
    std::vector<uint8_t>       rgb   = picture_of_palette(stack);
    const std::vector<uint8_t> green = flat_picture(ColorRGB(0.1f, 0.7f, 0.3f), 13);
    rgb.insert(rgb.end(), green.begin(), green.end());

    const ChromaHistogram   hist = histogram_of(rgb);
    const ChromaDiagnostics d    = chroma_diagnose(hist, stack);

    REQUIRE(d.mean_de > 1.);
    // Same bins, same palette, same metric, summed in the same order, so this is the same number and
    // not merely a close one. Reporting a different mean from the one the picker minimised would have
    // the panel arguing with itself.
    REQUIRE_THAT(d.mean_de, WithinAbs(chroma_stack_error(hist, stack), 1e-5));
}

TEST_CASE("Every want is accounted to exactly one reach point", "[ChromaDiagnostics]")
{
    const ChromaStack          stack = stack_of({ band(WHITE, 6), band(BLACK, 6) });
    const std::vector<uint8_t> rgb   = picture_of_palette(stack);
    const ChromaDiagnostics    d     = chroma_diagnose(histogram_of(rgb), stack);

    REQUIRE_FALSE(d.wants.empty());

    float served = 0.f;
    for (const ChromaReach &r : d.reach)
        served += r.serves;
    // The histogram's weights sum to one and each bin goes to its nearest reachable colour and nowhere
    // else, so the reach accounts for the whole picture.
    REQUIRE_THAT(served, WithinAbs(1.f, 1e-3f));
}

TEST_CASE("A want names a level the reach actually holds", "[ChromaDiagnostics]")
{
    const ChromaStack          stack = stack_of({ band(WHITE, 6), band(BLACK, 6) });
    const std::vector<uint8_t> rgb   = picture_of_palette(stack);
    const ChromaDiagnostics    d     = chroma_diagnose(histogram_of(rgb), stack);

    REQUIRE_FALSE(d.wants.empty());
    for (const ChromaWant &w : d.wants) {
        REQUIRE(w.nearest_level >= stack.base_layers);
        REQUIRE(w.nearest_level <= stack.total_layers());
        // The reach is in level order starting at the base, so a level indexes it directly.
        const ChromaReach &r = d.reach[size_t(w.nearest_level - stack.base_layers)];
        REQUIRE(r.level == w.nearest_level);
        REQUIRE(r.serves > 0.f);
    }
}

TEST_CASE("A picture made of printable colours is reported as reached", "[ChromaDiagnostics]")
{
    const ChromaStack          stack = stack_of({ band(WHITE, 6), band(BLACK, 6) });
    const std::vector<uint8_t> rgb   = picture_of_palette(stack);
    const ChromaDiagnostics    d     = chroma_diagnose(histogram_of(rgb), stack);

    REQUIRE_THAT(d.unreachable_weight, WithinAbs(0.f, 1e-6f));
    REQUIRE(d.worst_de < CHROMA_DIAG_UNREACHABLE_DE);
    REQUIRE_FALSE(has_kind(d, ChromaFindingKind::ColorsUnreachable));
}

TEST_CASE("A colour no filament in the stack reaches is reported with the colour", "[ChromaDiagnostics]")
{
    // Two neutral filaments reach nothing but greys, and a saturated red is a hundred dE from the
    // nearest of them - the case where no number of layers helps and only another filament will.
    const ChromaStack          stack = stack_of({ band(WHITE, 4), band(BLACK, 6) });
    const std::vector<uint8_t> rgb   = flat_picture(ColorRGB(1.f, 0.f, 0.f), 64);
    const ChromaDiagnostics    d     = chroma_diagnose(histogram_of(rgb), stack);

    REQUIRE(d.worst_de > CHROMA_DIAG_UNREACHABLE_DE);
    REQUIRE_THAT(d.unreachable_weight, WithinAbs(1.f, 1e-3f));

    const ChromaFinding *f = finding_of(d, ChromaFindingKind::ColorsUnreachable);
    REQUIRE(f != nullptr);
    REQUIRE(f->has_color);
    // The swatch is the picture's own colour rather than the nearest printable one: what to go and buy.
    REQUIRE(f->color.r() > 0.8f);
    REQUIRE(f->color.g() < 0.2f);
}

TEST_CASE("A band printed over its own base is reported as spending layers on nothing", "[ChromaDiagnostics]")
{
    // The base prints in the bottom band's filament, so the bottom band is that filament over itself
    // and every layer of it after the first shows the colour the one below already showed.
    const ChromaStack          stack = stack_of({ band(WHITE, 5), band(BLACK, 6) });
    const std::vector<uint8_t> rgb   = picture_of_palette(stack);
    const ChromaDiagnostics    d     = chroma_diagnose(histogram_of(rgb), stack);

    REQUIRE(d.wasted_layers >= 4);
    const ChromaFinding *f = finding_of(d, ChromaFindingKind::WastedLayers);
    REQUIRE(f != nullptr);
    REQUIRE(f->band_a == 0);
    // The filament's own name is what the sentence calls the band by, so the user can find the row.
    REQUIRE(f->text.find(WHITE.name) != std::string::npos);
}

TEST_CASE("What the trim would remove is what the diagnosis calls wasted", "[ChromaDiagnostics]")
{
    const ChromaStack       stack = stack_of({ band(WHITE, 5), band(BLACK, 6) });
    const ChromaDiagnostics d     = chroma_diagnose(ChromaHistogram{}, stack);

    ChromaStack trimmed = stack;
    chroma_trim_bands(trimmed);
    int removed = 0;
    for (size_t b = 0; b < stack.bands.size(); ++b)
        removed += stack.bands[b].layers - trimmed.bands[b].layers;

    REQUIRE(d.wasted_layers == removed);
}

TEST_CASE("A band with no intermediate colours blames the layer height", "[ChromaDiagnostics]")
{
    // TD 1 covers in a tenth of a millimetre, which at 0.08 mm layers is one and a quarter layers: the
    // band can only be used or not used, and the picture comes out as flat steps.
    const ChromaFilament       opaque = filament(4, ColorRGB(0.f, 0.3f, 0.9f), 1.f);
    const ChromaStack          stack  = stack_of({ band(WHITE, 3), band(opaque, 6) });
    const std::vector<uint8_t> rgb    = picture_of_palette(stack);
    const ChromaDiagnostics    d      = chroma_diagnose(histogram_of(rgb), stack);

    const ChromaFinding *f = finding_of(d, ChromaFindingKind::LayerHeightTooCoarse);
    REQUIRE(f != nullptr);
    REQUIRE(f->band_a == 1);
}

TEST_CASE("Two adjacent filaments of the same lightness are reported", "[ChromaDiagnostics]")
{
    // Pure red and a mid grey are a fifth of a dE apart in L*, so the swap between them moves the tint
    // and leaves the tone where it was. Two layers in the bottom band, so the wasted-layers finding is
    // below its own threshold and this test is about the one thing it is about.
    const ChromaStack          stack = stack_of({ band(RED, 2), band(MID_GREY, 6) });
    const std::vector<uint8_t> rgb   = picture_of_palette(stack);
    const ChromaDiagnostics    d     = chroma_diagnose(histogram_of(rgb), stack);

    const ChromaFinding *f = finding_of(d, ChromaFindingKind::FilamentsTooClose);
    REQUIRE(f != nullptr);
    REQUIRE(f->band_a == 0);
    REQUIRE(f->band_b == 1);
}

TEST_CASE("Filaments far apart in lightness are left alone", "[ChromaDiagnostics]")
{
    const ChromaStack          stack = stack_of({ band(WHITE, 2), band(BLACK, 6) });
    const std::vector<uint8_t> rgb   = picture_of_palette(stack);
    const ChromaDiagnostics    d     = chroma_diagnose(histogram_of(rgb), stack);

    REQUIRE_FALSE(has_kind(d, ChromaFindingKind::FilamentsTooClose));
}

TEST_CASE("A front lit stack built dark to light is told it is upside down", "[ChromaDiagnostics]")
{
    const ChromaStack          stack = stack_of({ band(BLACK, 6), band(WHITE, 6) });
    const std::vector<uint8_t> rgb   = picture_of_palette(stack);
    const ChromaDiagnostics    d     = chroma_diagnose(histogram_of(rgb), stack);

    REQUIRE(has_kind(d, ChromaFindingKind::OrderReversed));
}

TEST_CASE("A front lit stack built light to dark is left alone", "[ChromaDiagnostics]")
{
    const ChromaStack          stack = stack_of({ band(WHITE, 6), band(BLACK, 6) });
    const std::vector<uint8_t> rgb   = picture_of_palette(stack);
    const ChromaDiagnostics    d     = chroma_diagnose(histogram_of(rgb), stack);

    REQUIRE_FALSE(has_kind(d, ChromaFindingKind::OrderReversed));
}

TEST_CASE("A back lit stack wants the other order", "[ChromaDiagnostics]")
{
    // Lit from behind, a dark band starves everything above it, so the convention inverts and the
    // light-to-dark stack is the one with the problem.
    // A thin base, because lit from behind the base is part of the column rather than a backing
    // behind it, and a picture on a thick one is black.
    ChromaStack stack = stack_of({ band(WHITE, 6), band(BLACK, 6) }, 2);
    stack.lighting    = ChromaLighting::BackLit;

    const std::vector<uint8_t> rgb = picture_of_palette(stack);
    const ChromaDiagnostics    d   = chroma_diagnose(histogram_of(rgb), stack);

    REQUIRE(has_kind(d, ChromaFindingKind::OrderReversed));
}

TEST_CASE("A translucent filament at the bottom of a front lit stack is reported", "[ChromaDiagnostics]")
{
    // The base prints in the bottom band's own filament, so there is nothing under the bottom band for
    // the light to come back through and the most translucent filament in the drawer is wasted there.
    const ChromaFilament       clear_white = filament(5, ColorRGB(1.f, 1.f, 1.f), 8.f);
    const ChromaFilament       flat_black  = filament(6, ColorRGB(0.f, 0.f, 0.f), 1.f);
    const ChromaStack          stack       = stack_of({ band(clear_white, 6), band(flat_black, 6) });
    const std::vector<uint8_t> rgb         = picture_of_palette(stack);
    const ChromaDiagnostics    d           = chroma_diagnose(histogram_of(rgb), stack);

    const ChromaFinding *f = finding_of(d, ChromaFindingKind::TranslucentWasted);
    REQUIRE(f != nullptr);
    REQUIRE(f->band_a == 0);
}

TEST_CASE("A stack with nothing wrong with it says so", "[ChromaDiagnostics]")
{
    // One layer of white over a white base wastes nothing, eight layers of black over it is a gradient
    // the whole way, the filaments are a hundred apart in lightness and in the order the convention
    // asks for, and the picture is made of colours the stack prints.
    const ChromaStack          stack = stack_of({ band(WHITE, 1), band(BLACK, 8) });
    const std::vector<uint8_t> rgb   = picture_of_palette(stack);
    const ChromaDiagnostics    d     = chroma_diagnose(histogram_of(rgb), stack);

    REQUIRE(d.findings.size() == 1);
    REQUIRE(d.findings.front().kind == ChromaFindingKind::WellMatched);
    REQUIRE_FALSE(d.findings.front().text.empty());
}

TEST_CASE("Findings come back worst first", "[ChromaDiagnostics]")
{
    const ChromaStack          stack = stack_of({ band(BLACK, 8), band(WHITE, 8), band(RED, 8) });
    const std::vector<uint8_t> rgb   = flat_picture(ColorRGB(0.f, 0.6f, 0.2f), 64);
    const ChromaDiagnostics    d     = chroma_diagnose(histogram_of(rgb), stack);

    REQUIRE_FALSE(d.findings.empty());
    for (size_t i = 1; i < d.findings.size(); ++i)
        REQUIRE(d.findings[i - 1].cost >= d.findings[i].cost);
}

TEST_CASE("A badly built stack does not bury its findings under one of them", "[ChromaDiagnostics]")
{
    // Four opaque filaments given eight layers each: every band goes flat after a layer or two, so
    // every band has the same two complaints and the list would be nothing else without the cap.
    const ChromaStack stack = stack_of({ band(filament(0, ColorRGB(1.f, 1.f, 1.f), 1.f), 8),
                                         band(filament(1, ColorRGB(0.f, 0.f, 0.f), 1.f), 8),
                                         band(filament(2, ColorRGB(1.f, 0.f, 0.f), 1.f), 8),
                                         band(filament(3, ColorRGB(0.f, 0.f, 1.f), 1.f), 8) });
    const std::vector<uint8_t> rgb = picture_of_palette(stack);
    const ChromaDiagnostics    d   = chroma_diagnose(histogram_of(rgb), stack);

    REQUIRE(d.findings.size() <= CHROMA_DIAG_MAX_FINDINGS);
    REQUIRE(count_kind(d, ChromaFindingKind::WastedLayers) <= 2);
    REQUIRE(count_kind(d, ChromaFindingKind::LayerHeightTooCoarse) <= 2);
}

TEST_CASE("The same stack and picture are diagnosed the same way twice", "[ChromaDiagnostics]")
{
    const ChromaStack          stack = stack_of({ band(WHITE, 6), band(RED, 6), band(BLACK, 6) });
    const std::vector<uint8_t> rgb   = picture_of_palette(stack);
    const ChromaHistogram      hist  = histogram_of(rgb);

    const ChromaDiagnostics a = chroma_diagnose(hist, stack);
    const ChromaDiagnostics b = chroma_diagnose(hist, stack);

    REQUIRE(a.findings.size() == b.findings.size());
    for (size_t i = 0; i < a.findings.size(); ++i) {
        REQUIRE(a.findings[i].kind == b.findings[i].kind);
        REQUIRE(a.findings[i].text == b.findings[i].text);
    }
    REQUIRE_THAT(a.mean_de, WithinAbs(b.mean_de, 1e-12));
}

TEST_CASE("Diagnosing from pixels is diagnosing the histogram the picker would build", "[ChromaDiagnostics]")
{
    const ChromaStack          stack = stack_of({ band(WHITE, 6), band(BLACK, 6) });
    const std::vector<uint8_t> rgb   = picture_of_palette(stack);
    const int                  width = int(rgb.size() / 3);

    const ChromaDiagnostics from_pixels = chroma_diagnose(rgb.data(), width, 1, stack);
    const ChromaDiagnostics from_bins   = chroma_diagnose(chroma_histogram(rgb.data(), width, 1, 192,
                                                                          CHROMA_VIVIDNESS_RECOMMENDED,
                                                                          stack.importance),
                                                          stack);

    REQUIRE(from_pixels.wants.size() == from_bins.wants.size());
    REQUIRE_THAT(from_pixels.mean_de, WithinAbs(from_bins.mean_de, 1e-12));
}
