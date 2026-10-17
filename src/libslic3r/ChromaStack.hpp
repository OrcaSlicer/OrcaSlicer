#ifndef slic3r_ChromaStack_hpp_
#define slic3r_ChromaStack_hpp_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <cereal/cereal.hpp>
#include <cereal/types/vector.hpp>

#include "Color.hpp"

namespace Slic3r {

// ChromaStack-style colour printing: a picture is reproduced on a single face by stacking thin layers
// of a few filaments, swapping filament at fixed layer boundaries, and varying the *height* of each
// pixel's column. Where the column is low, the layers above it are missing and the filament below
// shows; where it is high, the topmost filament covers everything beneath it. Because filament is
// translucent, a column that stops part-way through a run shows a blend of that run's filament and
// what is under it, which is what gives far more colours than there are filaments loaded.
//
// So there are only two things to compute, and they are both here:
//  - the *palette*: the colour the stack shows at every possible column height (chroma_palette());
//  - the *height map*: per pixel, the column height whose palette colour is closest to the picture
//    (chroma_solve()).
//
// Everything else is machinery that already exists. The height map is handed to a texture
// displacement layer as its height texture, so the existing projection/preview/bake pipeline turns
// it into real geometry (see TextureDisplacement.hpp), and the filament swaps are handed to the
// object's height ranges (ModelObject::layer_config_ranges), which is the slicer's own way of
// saying "print this Z band with that filament".

// The depth of filament that blocks essentially all the light behind it, as a fraction of the
// filament's transmission distance (TD).
//
// TD's scale is empirical, not physical: its values run from about 1
// for an opaque filament to 10 and above for a nearly clear one, and the community's calibration
// prints (the "seashell" step test) read a TD off the step where the colour beneath stops showing.
// That step is about a tenth of the TD number, which is the only thing this constant encodes - a
// filament of TD 2 stops hiding what is under it at roughly 0.2 mm, i.e. after two or three layers,
// while one of TD 8 needs most of a millimetre. The model below turns that into a per-layer
// transmittance; see chroma_palette().
static constexpr float CHROMA_TD_OPAQUE_FRACTION = 0.1f;

// What a fresh stack gets above the base. Twenty-four layers is about two millimetres at a fine layer
// height: enough steps for a continuous tonal ramp, and still a print that finishes in an evening.
static constexpr int CHROMA_DEFAULT_RELIEF_LAYERS = 24;

// The most picture layers the solve can express. A column's height above the base is stored as one
// byte per pixel in the 8-bit grayscale height map the displacement reads, so beyond this two
// different heights would have to share a grey level and the relief's steps would stop landing on
// layer boundaries - which is the one thing the quantization exists to guarantee.
//
// It bounds the bands, not the stack: the base is whatever the model already is under the painted
// face, often hundreds of layers on a tall part, and nothing about it is quantized into the height
// map. As a limit on the picture it is not one in practice - 255 layers is 20 mm of relief at
// 0.08 mm, far past where a picture gains anything.
static constexpr int CHROMA_MAX_RELIEF_LAYERS = 255;

// The most filament changes one stack may hold. Every band is a swap the printer has to make, so this is
// a limit on patience rather than on the model; the palette itself is bounded by the layer count.
static constexpr size_t CHROMA_MAX_BANDS = 16;

// One filament as the colour model sees it: what colour it is, and how far light gets into it.
struct ChromaFilament
{
    // 0-based index into the project's filaments, which is what a height range's `extruder` option
    // (1-based) is written from. -1 for a filament that is not loaded, which the GUI refuses to
    // solve with rather than silently printing in the wrong colour.
    int      filament = -1;
    ColorRGB color;
    // The filament preset's own name, so the panel can say which filament this is. Six similar greys
    // are six identical swatches and six identical numbers otherwise. Stored rather than looked up on
    // demand because a stack outlives the project's filament list - a preset can be swapped out from
    // under a saved stack, and then the name is the only record of what it was solved against.
    std::string name;
    // Transmission distance, mm, as the filament preset reports it
    // (`filament_transmission_distance`). Clamped to a sane range when the palette is built: zero
    // would make a layer infinitely opaque and a negative value is meaningless.
    float    td_mm = 4.f;

    template<class Archive> void serialize(Archive &ar)
    {
        float r = color.r(), g = color.g(), b = color.b();
        ar(filament, r, g, b, td_mm, name);
        color = ColorRGB(r, g, b);
    }
};

// A run of consecutive layers printed in one filament. The stack is a list of these, bottom-up, so
// a band boundary is exactly where the print pauses and the filament is changed.
struct ChromaBand
{
    ChromaFilament filament;
    int              layers = 1;

    template<class Archive> void serialize(Archive &ar) { ar(filament, layers); }
};

// The whole recipe: which filaments, in which order, how thick a layer is, and how much of the
// bottom is solid backing rather than picture.
struct ChromaStack
{
    std::vector<ChromaBand> bands; // bottom-up; bands.front() is the one the base is printed in

    // The print's layer height, mm. Taken from the print profile rather than chosen here - a stack
    // solved against 0.08 mm layers prints as the wrong picture at 0.2 mm, so the GUI re-solves when
    // the profile's layer height changes.
    float layer_height_mm = 0.08f;

    // Layers of solid backing under the picture: everything the model already is below the painted
    // face. Not part of any band - the bands are the picture - but printed in the bottom band's
    // filament, since that is what is loaded when the print starts.
    //
    // Keeping it out of the bands is what lets it be what it really is: a measurement of the model,
    // often a hundred layers or more, while a band is a handful. Folding the two together put a
    // three-digit number on a slider meant for single digits and made "how thick is the backing" and
    // "how many layers does the first filament get" the same control, which they are not.
    //
    // The colour model treats the base as opaque. That is what makes a column's colour depend only on
    // the layers above it, and why the panel warns about a base thin enough to let the plate show
    // through.
    int  base_layers = 4;

    // Whether `base_layers` follows the model or was typed in. It follows by default, and it should:
    // the painted face is wherever the geometry puts it, and a base that disagrees with it shifts
    // every colour in the picture - the stack ends up anchored at the bottom of the part instead of
    // under the face, so the picture's whole range is squeezed into the first few millimetres and
    // everything above it clamps to the top band.
    //
    // It cannot be measured once and left: a layer is created, and its picture chosen, before there
    // is any paint to measure, and the face moves again whenever the part is scaled or the layer
    // height changes. So the panel re-measures it every frame while this is set.
    bool base_auto = true;

    // Spread the quantization error into neighbouring pixels (Floyd-Steinberg, serpentine) instead
    // of rounding each pixel to its nearest palette colour on its own. A palette of twenty or thirty
    // colours has to cover a photograph's whole range, so plain rounding leaves visible bands across
    // a sky or a cheek; diffusing the error trades them for fine stipple, which at a tenth of a
    // millimetre per pixel reads as the intermediate colour. Off for flat-coloured artwork, where
    // stipple is the artifact rather than the cure.
    bool dither = false;

    // Total printed layers, base included: what the tallest column is.
    int  total_layers() const;
    // Layers the picture is built from, i.e. the bands' own total above the base.
    int  relief_layers() const;
    // The relief's height in mm, which is the `depth_mm` of the texture displacement layer the
    // height map is handed to.
    float depth_mm() const;
    bool  empty() const { return bands.empty(); }

    template<class Archive> void serialize(Archive &ar) { ar(bands, layer_height_mm, base_layers, dither, base_auto); }
};

// The colour the stack shows at every column height: entry k is a column of exactly k printed
// layers, so the vector has total_layers() + 1 entries.
//
// The model is Beer-Lambert absorption, layer by layer. A layer of thickness h in a filament of
// transmission distance TD passes
//
//     T = exp(-h / (CHROMA_TD_OPAQUE_FRACTION * TD / 3))
//
// of the light coming from under it and shows its own colour with the rest, so
//
//     C_k = (1 - T_k) * c_k + T_k * C_(k-1)
//
// walking up from an opaque base, C_0 = c_1. The divisor is what makes "opaque at
// CHROMA_TD_OPAQUE_FRACTION * TD" true: three e-folding lengths pass 5% of the light, which is
// where the colour beneath stops being visible to the eye.
//
// Beer-Lambert is not just the physically honest choice here, it is the only one that stays
// consistent when applied per layer: transmittance multiplies, so a run of n layers of one filament
// passes exp(-n h / d) whether it is composited in one step or n, and the palette does not depend on
// how the stack happens to be cut into bands.
//
// Compositing is done on linear light and the entries are converted back to sRGB, because the
// quantity being attenuated is light, not an sRGB code.
std::vector<ColorRGB> chroma_palette(const ChromaStack &stack);

// What chroma_solve() produced.
struct ChromaSolve
{
    int width  = 0;
    int height = 0;

    // Per pixel, row-major and top-to-bottom like DecodedHeightTexture: the column's printed layer
    // count, between base_layers and total_layers(). Sixteen bits because it counts from the build
    // plate, and the base alone can be several hundred layers on a tall part.
    std::vector<uint16_t> levels;
    // The same thing as the 8-bit grayscale height map the displacement samples: 0 at base_layers,
    // 255 at total_layers(). This is what gets encoded as a PNG into the layer's `image_data`, which
    // is why the solve has to be the thing that owns the quantization - the relief's steps have to
    // land on layer boundaries, and a grey level is the only channel the displacement reads.
    std::vector<uint8_t> grey;
    // Per pixel RGB: the palette colour the solve chose, i.e. what the print is predicted to look
    // like. Shown in the panel next to the source picture, since the 3D view can only show the
    // relief - the colours do not exist until the print swaps filament.
    std::vector<uint8_t> preview;

    // How far the prediction landed from the picture, as a mean and worst-case CIE76 dE over all
    // pixels. The panel reports it because it is the only honest answer to "will this stack
    // reproduce my image": under about 5 is a good match, over about 15 means the filaments cannot
    // reach the picture's colours and no amount of layers will fix it.
    double mean_delta_e = 0.;
    double max_delta_e  = 0.;

    bool empty() const { return width <= 0 || height <= 0 || levels.empty(); }
};

// Solves `rgb` (width * height * 3 bytes, row-major, top-to-bottom) against the stack's palette.
// An empty stack, a zero-sized image or a stack with no relief layers gives an empty result.
ChromaSolve chroma_solve(const uint8_t *rgb, int width, int height, const ChromaStack &stack);

// How many layers of this filament it takes to hide what is under it, at this layer height. The
// single most useful number about a stack and the one nothing else makes visible: it is what decides
// whether a band is a gradient or a step.
//
// A filament hides what is behind it at about CHROMA_TD_OPAQUE_FRACTION of its TD (see
// chroma_palette()), so this is that depth in layers. Below two the band has no intermediate colours
// at all - the picture can only use the filament or not use it - which is what a coarse layer height
// does to an otherwise good stack: TD 4 at 0.2 mm covers in two layers, the same filament at 0.08 mm
// in five.
float chroma_layers_to_opaque(float td_mm, float layer_height_mm);

// How many of the picture's layers actually change the colour.
//
// A band eventually stops saying anything: once it hides what is under it, another layer of it shows
// the colour the one below already showed. Those layers still cost print time and still take up
// relief height, and nothing about the stack makes them visible - the band looks like ten layers of
// gradient and is, say, five of gradient and five of flat.
//
// Measured against the palette rather than against TD, because how long a band keeps paying depends
// on what is under it as much as on the filament: a light band over a dark one is finished in four or
// five layers, while a dark one over a light one goes on moving the colour perceptibly well past the
// depth at which it is nominally opaque.
int chroma_distinct_levels(const ChromaStack &stack);

// Shortens every band to its last layer that still changes the colour. Bands already shorter than
// that are left alone: they are still inside their fade, where every layer counts.
void chroma_trim_bands(ChromaStack &stack);

// The printed layer a point `z_mm` above the bottom of the object falls in: 1-based, clamped to the
// stack, 0 for a stack that prints nothing. Pair it with chroma_palette() to get the colour shown
// there.
//
// This, rather than the height map, is what decides the colour a surface shows. A filament swap
// happens at one print Z across the whole plate, so every surface crossing that height changes colour
// whatever the picture says - the terraces of the relief, the walls between them, and the sides of the
// model alike. Colouring a preview this way is therefore not an approximation of the print; it is what
// the print does, and it keeps working after the relief has been baked into the mesh and the height
// map is no longer attached to anything on screen.
int chroma_level_at(const ChromaStack &stack, double z_mm);

// The solve's height map as an 8-bit grayscale PNG, ready to be stored in a
// TextureDisplacementLayer's `image_data`.
std::vector<unsigned char> chroma_encode_height_png(const ChromaSolve &solve);

// One filament swap as an object height range: the Z band, in the object's own coordinates (0 at its
// bottom), that prints in `filament` (0-based).
struct ChromaRange
{
    double z_min = 0.;
    double z_max = 0.;
    int    filament = -1;
};

// The stack's bands as height ranges, in object coordinates, starting at `z_bottom` (which is 0 for
// a stack whose base starts at the object's first layer). Bands printed in the same filament as the
// band below are merged, so a stack that repeats a filament does not produce a pointless swap.
std::vector<ChromaRange> chroma_ranges(const ChromaStack &stack, double z_bottom = 0.);

// A starting point for a stack: the given filaments, lightest at the bottom, each with an equal
// share of `relief_layers` on top of `base_layers`.
//
// Light-to-dark is the convention filament-painting guides teach, and it is not arbitrary: a dark
// filament over a light one reads as a midtone at one or two layers and as the dark colour at four,
// so putting the dark filaments last is what buys the long tonal ramp. The reverse order gives a few
// washed-out steps and then nothing.
ChromaStack chroma_default_stack(const std::vector<ChromaFilament> &filaments, float layer_height_mm,
                                     int base_layers, int relief_layers);

} // namespace Slic3r

#endif // slic3r_ChromaStack_hpp_
