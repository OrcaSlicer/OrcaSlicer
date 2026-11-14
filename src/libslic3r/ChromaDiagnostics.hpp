#ifndef slic3r_ChromaDiagnostics_hpp_
#define slic3r_ChromaDiagnostics_hpp_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ChromaStack.hpp"
#include "Color.hpp"

namespace Slic3r {

// Why a ChromaStack solve came out the way it did.
//
// ChromaSolve::delta says *where* a solve failed, one byte a pixel, and the panel draws it as the
// pixels this stack cannot reach. What it cannot say is why, and the why is almost always one of a
// short list: two filaments competing for the same depths so that the swap between them buys nothing,
// a band whose top layers show the colour the layer below already showed, or a picture whose colours
// simply are not in the drawer at all. A user looking at a red overlay over one flower has no way to
// tell those apart, and the three have different fixes.
//
// So this is the stack and the picture put side by side in the one space where the question is
// legible: hue against lightness. What the stack can reach is chroma_palette(), what the picture wants
// is chroma_histogram(), and both go into the same chart - the reach as a curve through the palette's
// own levels, the wants as a cloud weighted by how much of the picture each colour is. Where the cloud
// sits away from the curve is a colour that cannot be printed; where the curve doubles back on itself
// is a swap being paid for twice.
//
// There is no GUI in here, deliberately. The same analysis has to be printable as text (which is what
// the findings are) and drawable as a chart (which is what the points are), and a panel that wants to
// do either should not have to redo the measuring.

// Where a colour sits in the chart: CIELAB's own cylindrical coordinates, which is what makes "hue
// against lightness" mean anything at all. Plotting a*b* instead would put the two things a filament
// stack is actually chosen for - how light a colour is and what colour it is - on axes that mix them.
struct ChromaMapCoord
{
    // The Lab hue angle in degrees, 0 at +a* and running anticlockwise through yellow, green and blue.
    // Wraps, so a chart drawing it on a linear axis has to decide where to cut it; drawing it as an
    // angle avoids the question.
    float hue_deg = 0.f;
    // L*, 0 for black and 100 for white. This is the axis the relief works along: a column's height is
    // read by the eye mostly as lightness, which is why two filaments of the same L* give a tonal ramp
    // that does not ramp.
    float lightness = 0.f;
    // C* = hypot(a*, b*): how far the colour sits off the neutral axis, and therefore how much its hue
    // is worth looking at. A near-grey has a hue angle that is numerically well defined and means
    // nothing, so a chart should fade or cluster the low-chroma points rather than scatter them around
    // the full circle on the strength of a rounding difference.
    float chroma_ab = 0.f;
};

// One colour the picture asks for: a bin of chroma_histogram(), placed in the chart and measured
// against what the stack can print.
struct ChromaWant
{
    ColorRGB       color;
    ChromaMapCoord at;
    // The bin's own weight, which is its share of the picture as chroma_histogram() weighted it:
    // coverage, boosted for a colour far from the picture's average, and again for a painted
    // importance region. The weights over all the wants sum to 1, so this is directly "how much of
    // the picture is this colour" in the sense the stack is chosen by.
    float weight = 0.f;
    // The colour difference to the closest colour the stack can reach. The same scale the solve
    // reports: under about 5 is a good match, over about 15 is a colour the filaments cannot reach and
    // no number of layers will fix.
    float nearest_de = 0.f;
    // The column height whose palette colour came closest, i.e. an index into chroma_palette(), or -1
    // when the stack prints nothing at all.
    int nearest_level = -1;
};

// One colour the stack can reach: a palette entry, which is to say a column of exactly `level` printed
// layers.
struct ChromaReach
{
    ColorRGB       color;
    ChromaMapCoord at;
    // The printed layer count, base included, so it indexes chroma_palette() directly.
    int level = 0;
    // Which band the top layer of that column belongs to, 0-based, or -1 for the bare base - a column
    // with none of the picture printed on it at all.
    int band = -1;
    // How far into that band the column stops, 1-based, or 0 for the bare base. A band's first layer
    // is where it still blends with what is under it and its last is where it has stopped saying
    // anything, and which of the two a reach point is matters to a reader of the chart.
    int layer_in_band = 0;
    // How far this column's colour is from the one a single layer shorter: what that layer bought.
    // 0 for the bare base, which has nothing below it. A run of these near zero is a band that has
    // gone opaque and is still being printed.
    float step_de = 0.f;
    // The total weight of the wants whose nearest reachable colour is this one: how much of the
    // picture this exact column height prints. Zero for a level that no part of the picture would ever
    // choose, which over a whole band is a band that could be dropped.
    float serves = 0.f;
};

// Which of the things that go wrong a finding is. The text says it in prose; this is for a caller that
// wants to draw a finding differently from the others, link it to a control, or suppress one kind.
enum class ChromaFindingKind : int
{
    // Nothing below applies: the stack reaches this picture.
    WellMatched = 0,
    // A band whose top layers show the colour the layer below already showed.
    WastedLayers = 1,
    // A band no part of the picture would ever choose: its swap could be dropped outright.
    BandUnused = 2,
    // Two bands reaching colours the stack cannot tell apart, so the swap between them buys nothing.
    BandsCollide = 3,
    // Two adjacent bands whose filaments are too close in lightness for the step between them to read.
    FilamentsTooClose = 4,
    // Colours the picture wants that nothing in the stack reaches.
    ColorsUnreachable = 5,
    // A translucent filament in the bottom band, where the base under it is its own colour and there
    // is nothing for the light to come back through.
    TranslucentWasted = 6,
    // The filaments run the wrong way up the stack for the lighting mode.
    OrderReversed = 7,
    // A band with fewer than two layers to opaque, which at this layer height has no intermediate
    // colours at all.
    LayerHeightTooCoarse = 8,
};

// One thing worth telling the user, as a sentence and as the data behind it.
struct ChromaFinding
{
    ChromaFindingKind kind = ChromaFindingKind::WellMatched;

    // The sentence, already translated. A complete sentence that names what is wrong and what to do
    // about it, in the register chroma_problems() uses, because the two end up in the same panel.
    std::string text;

    // How much this costs the picture, 0 to 1, which is what the findings are ranked by.
    //
    // The two kinds of cost here are not the same currency and there is no honest exchange rate
    // between them: a colour the stack cannot reach is a part of the picture printed wrong, while a
    // band that buys nothing is print time and a filament change spent on nothing. What they have in
    // common is being a *share* - of the picture's weight for the first, of the relief's layers for
    // the second - and a share is what this holds, so the ranking puts the largest problem of either
    // kind first and a reader who needs to know which currency it is has `kind`.
    float cost = 0.f;

    // The band or bands the finding is about, 0-based, or -1. `band_b` is only set for a finding about
    // a pair. These are what lets a panel put the finding next to the band's own row rather than in a
    // list at the bottom.
    int band_a = -1;
    int band_b = -1;

    // A colour the finding is about, when it is about one: the picture's own colour that the stack
    // misses by the most, for ColorsUnreachable. `has_color` rather than a sentinel because every
    // ColorRGB is a legitimate colour.
    //
    // It is a field rather than a word in the sentence on purpose. Naming a colour in prose means
    // splicing a translated word ("a dark red") into a translated sentence, which a translator cannot
    // reorder or inflect; a swatch beside the sentence says it better and in every language.
    ColorRGB color;
    bool     has_color = false;
};

// What chroma_diagnose() measured: the chart, the findings, and the handful of numbers a panel would
// otherwise compute again from the same two inputs.
struct ChromaDiagnostics
{
    // The picture's colours and the stack's, both in chart coordinates. `wants` is in the histogram's
    // own order, heaviest bin first; `reach` is in level order, so consecutive entries are consecutive
    // column heights and a chart can join them into the curve the stack walks.
    std::vector<ChromaWant>  wants;
    std::vector<ChromaReach> reach;

    // At most CHROMA_DIAG_MAX_FINDINGS of them, worst first. A stack with nothing wrong gets the one
    // WellMatched finding rather than an empty list, because "no findings" and "not analysed yet" look
    // identical to a panel and only one of them is worth saying out loud.
    std::vector<ChromaFinding> findings;

    // The weighted mean and the worst colour difference over the picture's bins. This is the same
    // number chroma_stack_error() returns and it is deliberately not ChromaSolve::mean_delta_e: that
    // one is a mean over pixels, and the mean over bins is what every judgement here and every
    // candidate the filament picker scored was made on. The two agree on whether a stack reaches a
    // picture and disagree in the last digit.
    double mean_de  = 0.;
    double worst_de = 0.;

    // The share of the picture, by the same weights the wants carry, whose nearest reachable colour is
    // further than CHROMA_DIAG_UNREACHABLE_DE away - the part of the picture that cannot be printed by
    // this stack in any arrangement - and the share within CHROMA_DIAG_GOOD_DE, which is the part that
    // comes out right.
    float unreachable_weight = 0.f;
    float matched_weight     = 0.f;

    // Relief layers that show a colour the layer below already showed, summed over the bands. What
    // chroma_trim_bands() would remove.
    int wasted_layers = 0;

    // The relief's own layer count and how many of those levels are a colour of their own
    // (chroma_distinct_levels()), carried so a panel can show the ratio without rebuilding the
    // palette for it.
    int relief_layers   = 0;
    int distinct_levels = 0;

    bool empty() const { return reach.empty(); }
};

// The colour difference below which the stack cannot tell two of its own colours apart.
//
// A just-noticeable difference is about 2.3 dE for a pair of flat patches side by side, and these are
// not side by side: they are two column heights somewhere in a picture, with the eye's attention on
// the picture rather than on them. Rounded to 2.5, which is where "the swap between these two bands
// buys nothing" starts being true rather than pedantic.
//
// Deliberately far above the half-dE that chroma_trim_bands() calls "this layer changed something".
// That one is asking whether a single layer did anything measurable at all, and errs towards keeping
// it; this one is asking whether a whole filament change is worth making, and errs towards saying no.
static constexpr float CHROMA_DIAG_INDISTINCT_DE = 2.5f;

// Where a match stops being good and where it stops being a match. Both are the thresholds
// ChromaSolve::mean_delta_e documents, held here as names so a finding and the number the panel prints
// beside it cannot drift apart.
static constexpr float CHROMA_DIAG_GOOD_DE        = 5.f;
static constexpr float CHROMA_DIAG_UNREACHABLE_DE = 15.f;

// How close in L* two adjacent bands' filaments have to be before the step between them stops reading.
//
// A relief is read as a tonal ramp: a column's height reaches the eye mostly as how light that column
// is, which is the whole reason the light-to-dark convention exists. Two filaments within this much of
// each other in lightness give a swap that costs a filament change and moves the tone by less than the
// print's own surface texture does. Ten L* is about a tenth of the full black-to-white range, which is
// roughly where a step in a gradient becomes findable on a printed surface.
static constexpr float CHROMA_DIAG_LIGHTNESS_STEP = 10.f;

// How much of a band has to be reachable elsewhere before the band is called a collision, and how
// little of the picture a band has to serve before it is called unused.
//
// Not 100% and not 0, because neither question is about the extremes: a band that covers four fifths
// of its range with colours another band already reaches is a wasted swap whatever the last fifth
// does, and a band that half a percent of the picture's weight lands on is serving noise.
static constexpr float CHROMA_DIAG_COLLISION_SHARE = 0.8f;
static constexpr float CHROMA_DIAG_UNUSED_SHARE    = 0.005f;

// The least of the picture that has to be out of reach before saying so. Below this the finding would
// fire on a photograph's handful of specular highlights, which no stack reaches and no user wants a
// filament bought for.
static constexpr float CHROMA_DIAG_UNREACHABLE_SHARE = 0.02f;

// How far apart two transmission distances have to be before one of them is "the translucent one". TD
// is an empirical scale read off a step print, so a difference under a couple of millimetres is inside
// the measurement rather than a fact about the filaments.
static constexpr float CHROMA_DIAG_TD_GAP_MM = 2.f;

// How many findings come back. Six is the most a panel can show without the list becoming the thing
// being read instead of the picture, and a stack with more than six things wrong with it has one thing
// wrong with it.
static constexpr size_t CHROMA_DIAG_MAX_FINDINGS = 6;

// Analyses `stack` against `picture` and reports both the chart and the findings.
//
// `picture` is a chroma_histogram() of the picture being solved, and it should be the *same* histogram
// the filament picker and the colour suggestions were scored against - weights and all - or the
// findings will contradict them: a stack chosen for a painted importance region, judged against a
// histogram weighted by coverage alone, reads as a stack chosen for the wrong thing. The overload
// below builds it the way the panel does.
//
// Cheap enough to run on every solve, which is the whole reason it works on the histogram: a few
// hundred bins against at most CHROMA_MAX_RELIEF_LAYERS palette entries is tens of thousands of
// colour differences, not the hundreds of millions a pass over the pixels would be.
//
// An empty stack, or one that prints nothing, gives an empty result rather than a finding: a caller in
// that state has not finished choosing filaments and does not need to be told so twice.
ChromaDiagnostics chroma_diagnose(const ChromaHistogram &picture, const ChromaStack &stack);

// The same thing starting from the picture's pixels (width * height * 3 bytes, row-major and
// top-to-bottom, like chroma_solve()), bucketing them the way the filament picker does:
// CHROMA_VIVIDNESS_RECOMMENDED, and the stack's own importance region. For a caller that has the
// pixels and no histogram and wants the analysis to agree with the picker.
ChromaDiagnostics chroma_diagnose(const uint8_t *rgb, int width, int height, const ChromaStack &stack);

} // namespace Slic3r

#endif // slic3r_ChromaDiagnostics_hpp_
