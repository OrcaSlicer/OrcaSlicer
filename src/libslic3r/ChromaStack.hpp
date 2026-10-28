#ifndef slic3r_ChromaStack_hpp_
#define slic3r_ChromaStack_hpp_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <cereal/cereal.hpp>
#include <cereal/types/vector.hpp>
#include <nlohmann/json_fwd.hpp>

#include "Color.hpp"
#include "TransmissionDistance.hpp"
#include "TriangleMesh.hpp"

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

// How far from level the painted surface may tilt before the picture stops being printable there.
//
// A swap happens at one print Z across the whole plate, so what a point shows depends on how high it
// is, not on how far the relief stands off the surface it sits on. On a tilted face a relief of d mm
// along the normal only raises the print by d*cos(tilt), so the relief has to be deepened by
// 1/cos(tilt) to put its steps at the heights the colours were solved for - which runs away as the
// surface approaches vertical, and at 60 degrees has already doubled. Past here the correction is
// clamped and those texels print the wrong colour, which chroma_problems() reports.
static constexpr float CHROMA_MAX_TILT_DEG = 60.f;

// The most filament changes one stack may hold. Every band is a swap the printer has to make, so this is
// a limit on patience rather than on the model; the palette itself is bounded by the layer count.
static constexpr size_t CHROMA_MAX_BANDS = 16;

// One filament as the colour model sees it: what colour it is, and how far light gets into it.
struct ChromaFilament
{
    // Where the transmission distance came from, when it came from the shipped table rather than
    // from the preset: the table's own name for the spool it matched. Empty otherwise, which is what
    // the card shows as "no source but the preset".
    std::string td_source;

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
// How big a plate a picture gets when it makes its own, measured on its longer side. A hundred
// millimetres is a postcard: large enough that the layers are a picture rather than a swatch, small
// enough to fit any printer that exists and to print in an evening.
static constexpr float CHROMA_PLATE_DEFAULT_MM = 100.f;

// A plate made for a picture that has no model of its own.
//
// Most of what ChromaStack is good at - a photograph, a poster, a piece of line art - has no model
// behind it and does not want one. The slab it has to be printed on is a function of the picture and
// a few numbers, so the slab is made here rather than asked for: the picture's own proportions, a
// solid backing to print the colours against, and a frame around it.
//
// The frame is a sweep: its outline says what the plate's edge is, its profile says what the band
// between that edge and the picture does in cross-section, and every combination of the two is a
// frame. That is how it is built, and it is deliberately not how it is chosen. A frame is something
// looked at, so the user picks one from pictures of frames and the chosen definition resolves into
// `frame_id` and `profile_points` here; the enum shapes remain for projects saved before the gallery
// existed, and for the sizes - a width and a rise are dimensions, and dimensions belong in a field.
struct ChromaFrame
{
    // The plate's outline. The picture is always a rectangle inside it; this is what is around it.
    enum class Shape : int
    {
        Rectangle = 0, // the picture's own rectangle, grown by the frame's width
        Rounded   = 1, // the same, with the corners turned by `corner_mm`
        Ellipse   = 2, // an oval through the corners of that rectangle
        Outline   = 3, // `outline` below: whatever shape was handed in, usually from an SVG
    };

    // What the band between the outline and the picture does on its way in. Each is a polyline in
    // (how far in, how high), swept around the outline - see chroma_frame_profile().
    enum class Profile : int
    {
        Flat   = 0, // level with the picture's base: a border of one colour and nothing more
        Bevel  = 1, // a straight ramp from the outer edge down to the picture
        Step   = 2, // one square step
        Round  = 3, // a quarter round, the shape of a moulding
        Ogee   = 4, // round out then round in, the shape a picture frame usually is
        // Appended rather than inserted: the value is what a saved project stores.
        Mat    = 5, // a flat lip and a window cut at 45 degrees - what a mount board is
        Cove   = 6, // a hollow quarter, the inverse of Round
        Step2  = 7, // two square steps
    };

    Shape   shape     = Shape::Rectangle;
    Profile profile   = Profile::Flat;
    // How wide the band is, measured in from the plate's edge. 0 means no frame at all: the picture
    // runs to the edge of the plate.
    float   width_mm  = 6.f;
    // How far the frame stands above the picture's base. 0 with any profile is the same as Flat.
    float   rise_mm   = 2.f;
    // The corner radius for Rounded, and ignored by every other shape.
    float   corner_mm = 4.f;
    // The outline for Shape::Outline: a closed loop in millimetres, centred on the picture. Held as
    // the loop rather than as a path to the SVG it was read from, so a plate can be rebuilt from a
    // saved project whether or not that file is still on the machine.
    std::vector<Vec2f> outline;

    // Which frame of the gallery this is, or empty for one built from the enum above.
    //
    // Kept so a saved project reopens the gallery on the frame it is wearing rather than on whatever
    // is first. It is a name, not a path: a definition that has since been removed leaves the frame
    // itself intact, because everything the mesh needs is in `outline` and `profile_points`.
    std::string frame_id;

    // The frame's own cross-section, in millimetres, when it has one: (how far in from the plate's
    // edge, how high above the picture's surface), outer edge first, the last sample where it meets
    // the picture. Empty for a frame that is still one of the Profile shapes, which is what every
    // project saved before the gallery existed is.
    //
    // Heights are measured from the picture's surface rather than from z = 0 so that a definition does
    // not have to know how thick the plate's backing is: chroma_frame_profile() adds that. What a
    // definition has to satisfy is listed there, and anything outside it is clamped rather than
    // refused - a frame that is slightly wrong should print slightly wrong, not take the plate with it.
    std::vector<Vec2f> profile_points;

    bool operator==(const ChromaFrame &o) const
    {
        return shape == o.shape && profile == o.profile && width_mm == o.width_mm && rise_mm == o.rise_mm &&
               corner_mm == o.corner_mm && outline == o.outline && frame_id == o.frame_id &&
               profile_points == o.profile_points;
    }
    bool operator!=(const ChromaFrame &o) const { return !(*this == o); }

    template<class Archive> void serialize(Archive &ar)
    {
        // Appended, never reordered; see ChromaStack::serialize().
        ar(shape, profile, width_mm, rise_mm, corner_mm, outline);
        ar(frame_id, profile_points);
    }
};

// Which way up the plate is printed.
//
// Face up is what a picture printed on a model does and what everything else here assumes: the
// picture's surface is the last thing printed, the backing is under it, and the relief stands proud
// of the plate.
//
// Face down turns it over. The picture's surface is the first layer, printed against the build plate,
// which comes out glass smooth with no layer lines on it at all - the one finish an FFF printer can
// give that no amount of fine layers reproduces. Three things change, and they are all in this file:
//
//  - the relief becomes a recess cut up into the bottom of the plate, so the height map means the
//    opposite of what it does face up (chroma_invert_relief());
//  - the bands print in the reverse order, because the light now enters through the face that printed
//    first: the filament the light meets first has to be the one in the first layer
//    (chroma_ranges());
//  - the base is no longer under the picture, it is above it. It is still the opaque backing the
//    colour model assumes - the light still ends on it - but nothing about the model measures it from
//    the painted face any more, so ChromaStack::base_auto has nothing to measure and the plate has to
//    be thick enough to hold the whole stack (ChromaStack::total_mm()).
//
// It is a plate's mode rather than a stack's because it is a fact about how this slab is laid on the
// bed. A picture painted onto somebody's model is printed whichever way up that model is.
enum class ChromaOrientation : int
{
    FaceUp   = 0,
    FaceDown = 1,
};

// The plate a picture made for itself: the picture's own area, what is under it, and what is around
// it. Everything else about the print - which filaments, how many layers, what colour each height
// shows - is the stack this hangs off.
struct ChromaPlate
{
    // Whether this picture made its own plate. False for a picture painted onto a model, which is
    // the other half of what ChromaStack does and is not touched by any of this.
    bool  generated = false;

    // The picture's area on the plate. The longer side is what the user sets; the other follows the
    // picture's proportions, because a photograph squeezed to fit a square is not the picture.
    float width_mm  = 100.f;
    float height_mm = 100.f;
    // The solid backing under the picture. Written in millimetres rather than layers because it is a
    // dimension of the plate; the stack's own base_layers is measured from it.
    //
    // Face down it is how thick the plate is over the picture instead, the picture being the bottom
    // face: the relief is cut up into it and the backing is what is left above, so it has to be at
    // least ChromaStack::total_mm() or the recess opens out through the back.
    float base_mm   = 1.f;
    // How finely the picture's face is divided, in millimetres. The relief is carried by vertices, so
    // a face of two triangles has no picture on it however good the height map is. The bake subdivides
    // further where the picture needs it; this is only enough to start from.
    float cell_mm   = 1.f;

    ChromaFrame frame;

    // Which way up it prints. Face up by default, which is the plate this has always made.
    ChromaOrientation orientation = ChromaOrientation::FaceUp;

    bool operator==(const ChromaPlate &o) const
    {
        return generated == o.generated && width_mm == o.width_mm && height_mm == o.height_mm &&
               base_mm == o.base_mm && cell_mm == o.cell_mm && frame == o.frame &&
               orientation == o.orientation;
    }
    bool operator!=(const ChromaPlate &o) const { return !(*this == o); }

    template<class Archive> void serialize(Archive &ar)
    {
        // Appended, never reordered; see ChromaStack::serialize().
        ar(generated, width_mm, height_mm, base_mm, cell_mm, frame);
        ar(orientation);
    }
};

// Which way the light goes through the finished print.
//
// Front lit is what the rest of this file assumes and is exactly what it has always done: the light
// falls on the picture, the base behind it is opaque, and a column's colour is the light it sends back
// out. Back lit is the other print the same stack can make - the plate hangs in front of a lamp and
// what the eye sees is the light that got through the whole column, base included.
//
// Both lit is neither, and it is where most back-lit prints actually end up: a lamp behind the plate
// and a room in front of it, so some of what the eye receives came through the column and the rest
// bounced off its face. It is a mode rather than a view because the two add in linear light and the
// sum is a different set of colours from either - a stack chosen against the lamp alone washes out as
// soon as the room light lands on it, which is the complaint every lithophane in a lit room draws.
//
// It changes the palette and nothing else. The solve, the height map, the swaps and the preview all
// read chroma_palette() and none of them needs to know which mode built it.
enum class ChromaLighting : int
{
    FrontLit = 0,
    BackLit  = 1,
    BothLit  = 2,
};

// A region of the picture, as a run-length encoded 1-bit mask over the picture's own grid.
//
// A brush stroke is why this is not a list of rectangles. Rectangles are smaller still and a pixel
// test against them is a handful of comparisons, but a painted edit is whatever shape the cursor drew:
// decomposing a stroke into rectangles costs more than it saves, and a stroke of any width is dozens
// of them. A raw bitmap is the other end - one byte a texel, megabytes an edit in a project file. The
// runs sit in the middle, because a painted region is a few long runs per row: a stroke over a
// 2000 x 2000 picture is a few thousand numbers, and it decodes to a bitmap in one linear pass, which
// is what keeps the per-pixel cost of applying an edit to a single array lookup.
//
// The grid is the mask's own and is recorded rather than assumed, because the solve may run on a
// reduced grid (the GUI solves a preview at a few hundred texels a side) while the mask was painted at
// the picture's resolution. An edit is sampled nearest-neighbour, so one mask serves every resolution
// the picture is ever solved at.
struct ChromaMask
{
    int width  = 0;
    int height = 0;
    // Alternating run lengths over the mask's pixels, row-major and top-to-bottom like every other
    // image here, starting with a run of pixels *outside* the region. The lengths add up to
    // width * height, except that a trailing run of outside pixels may be left off.
    std::vector<uint32_t> runs;

    // A mask with nothing inside it: no grid, or a single run, which by the convention above is all
    // outside.
    bool empty() const { return width <= 0 || height <= 0 || runs.size() < 2; }

    template<class Archive> void serialize(Archive &ar) { ar(width, height, runs); }
};

// One edit painted over a solved height map: a region, and what the columns inside it do instead of
// what the colour match chose for them.
struct ChromaRetouchEdit
{
    enum class Target : int
    {
        // Move the column `value` layers from whatever the match chose, up or down. This is the
        // brush a user reaches for: "this cheek is a shade too dark" is a delta, not a height.
        Delta = 0,
        // Put the column at the top of band `value` (0-based), so it shows that band's own colour
        // whatever the picture said. -1 is the bare base, nothing of the picture printed at all.
        // Absolute rather than a colour, because a band top is the one height that is still a band
        // top after the bands above it are made longer or shorter.
        Band  = 1,
    };

    ChromaMask region;
    Target     target = Target::Delta;
    int        value  = 0;
    // Off keeps the edit and stops it applying, so the panel can show what an edit is doing by taking
    // it away and putting it back. Deleting and re-painting a stroke is not the same gesture.
    bool       enabled = true;

    template<class Archive> void serialize(Archive &ar) { ar(region, target, value, enabled); }
};

// The edits, in the order they were painted.
//
// The order is the whole of the data model: edits apply one after another and a later one over the
// same pixels wins, which is what makes painting behave like painting rather than like editing a set
// of rules that have to be made consistent with each other. They apply after the match and after the
// dither, because an edit is a correction of what the solve decided, and before the quantization,
// because a retouched column still has to land on a layer boundary.
struct ChromaRetouch
{
    std::vector<ChromaRetouchEdit> edits;

    bool empty() const { return edits.empty(); }

    template<class Archive> void serialize(Archive &ar) { ar(edits); }
};

// What the picture is about: the part of it whose colours are worth spending the stack on.
//
// Every measure here - the solve's mean error, the histogram the filament picker and the colour
// suggestions are scored against - treats a pixel of sky and a pixel of the face in it as equally
// important, because coverage is all it has to go on. A portrait is mostly background, so a stack
// chosen that way is a stack chosen for the background.
//
// Rather than a set of mapping modes to choose between, which is the usual answer and makes the user
// guess which one their picture is, this lets them say the one thing they actually know: which part
// matters. It is a region and a multiplier, and it changes nothing about how a pixel is matched -
// only how much that pixel counts when the question is "is this stack right for this picture".
//
// Absent by default, and a stack without one behaves exactly as it always has.
struct ChromaImportance
{
    // Painted on the picture's own grid, like a retouch edit's, and sampled the same way.
    ChromaMask region;

    // How much more a pixel inside the region counts than one outside it. 1 is no preference at all,
    // which is the same thing as having no region. Four is a reasonable brush: enough that a face
    // decides the stack and not so much that the background is thrown away, which would put the sky
    // at an error nobody asked for.
    float weight = 4.f;

    bool empty() const { return region.empty() || weight == 1.f; }

    template<class Archive> void serialize(Archive &ar) { ar(region, weight); }
};

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

    // A fingerprint of the object height ranges this stack last wrote, or 0 for none. The swaps are
    // written whenever the stack changes, which is only safe while they are still the ones we wrote:
    // the ranges are the object's, a user may have made their own, and silently replacing those would
    // be the panel taking something it was not given. Ranges that do not match this are left alone and
    // the card says so.
    uint64_t applied_ranges = 0;

    // Front lit or back lit; see ChromaLighting. Front lit is the default and is bit for bit what the
    // model has always done, so a stack saved before this existed reads back as the print it was.
    ChromaLighting lighting = ChromaLighting::FrontLit;

    // The lamp behind a back-lit plate, and how bright it is. Ignored entirely when front lit.
    //
    // The colour is the light's own, not a filament's: a warm white LED prints a different picture
    // through the same stack than a daylight one does, and it is the one part of a back-lit print that
    // the slicer cannot see and the user has to say. The intensity scales it and may run above 1,
    // which is a lamp bright enough to blow out the thinnest columns - the palette clamps, so those
    // columns all show white and the picture loses its highlights, which is the honest prediction.
    ColorRGB led_color     = ColorRGB(1.f, 1.f, 1.f);
    float    led_intensity = 1.f;

    // How much of a both-lit picture is the lamp behind it: 0 is the front-lit print exactly, 1 the
    // back-lit one exactly, and a half is a plate on a window sill at dusk. Ignored in the other two
    // modes, where it would only be a second way to say what the mode already says.
    //
    // A balance rather than two intensities because the eye has no absolute scale here: what decides
    // the picture is the ratio of the light coming through to the light coming back, and a user can
    // see that ratio in the preview and match it to the room. Two lamps in lumens they would have to
    // guess at would be the same number wearing a disguise.
    float lighting_balance = 0.5f;

    // The edits painted over the solved height map, in order. Empty for every stack that has not been
    // retouched, which is what makes a saved project from before this existed solve to exactly the
    // same height map it did then.
    ChromaRetouch retouch;

    // Which part of the picture the stack is chosen for. Empty by default: every pixel counts the
    // same, which is what this has always done.
    ChromaImportance importance;

    // Total printed layers, base included: what the tallest column is.
    int  total_layers() const;
    // Layers the picture is built from, i.e. the bands' own total above the base.
    int  relief_layers() const;
    // The relief's height in mm, which is the `depth_mm` of the texture displacement layer the
    // height map is handed to.
    float depth_mm() const;
    // The whole stack's height in mm, base included. How thick a face-down plate has to be, since
    // there the stack is printed into the plate rather than onto it.
    float total_mm() const;
    bool  empty() const { return bands.empty(); }

    // The plate this picture made for itself, if it made one. A picture painted onto a model leaves
    // it at its defaults and nothing reads it.
    ChromaPlate plate;

    template<class Archive> void serialize(Archive &ar)
    {
        // Appended, never reordered: this archive is what an undo step and a clipboard entry are made
        // of, so the fields have to come off the stream in the order they went on however old the
        // stack is that is being read.
        ar(bands, layer_height_mm, base_layers, dither, base_auto, applied_ranges, plate);
        // Through locals, the way ChromaFilament's colour goes: cereal needs an lvalue per channel.
        float lr = led_color.r(), lg = led_color.g(), lb = led_color.b();
        ar(lighting, lr, lg, lb, led_intensity, retouch);
        led_color = ColorRGB(lr, lg, lb);
        ar(importance);
        ar(lighting_balance);
    }
};

// The optical depth of one layer of a filament: how much of what is behind it that layer takes out,
// as the exponent of Beer-Lambert absorption. A layer passes exp(-density) of the light from under it,
// and a run of n layers passes exp(-n * density), which is what makes the whole model per-band rather
// than per-layer arithmetic.
//
// The one place the relation between a TD, a layer height and an absorption lives, so the palette and
// the preview's shader cannot drift apart: both ask here.
float chroma_layer_density(float td_mm, float layer_height_mm);

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
//
// A back-lit stack (ChromaLighting::BackLit) walks the same layers with the same per-layer
// transmittance and composites the other thing: there is nothing illuminating a layer from the front,
// so no layer adds a colour of its own, and what entry k holds is the lamp's light after it has been
// through all k layers. Each layer multiplies it by exp(-density) - how much gets through at all,
// which is the TD's business and chroma_layer_density()'s alone - and tints what is left by that
// filament's hue, normalised so that a neutral filament does not darken the light twice. The base is
// part of the column here rather than an opaque backing, because it is: it is the thinnest part of a
// back-lit plate and the brightest part of the picture.
//
// So thickness means darkness in this mode, which inverts the mapping the whole rest of the model
// assumes. Nothing here compensates for that, because the solve has no business guessing: it matches
// colours, and entry k is still honestly the colour of a column k layers tall. What it means for a
// caller is that a back-lit stack wants its filaments darkest at the bottom rather than lightest, that
// the base should be one or two layers rather than the dozens a front-lit print needs, and that a
// photograph printed back lit comes out as a relief that looks like its own negative. A caller that
// wants a back-lit print to read like the picture inverts the picture's tone, not the height map: the
// height map's steps are layer boundaries and inverting it after the fact moves them off.
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

    // Per pixel, how far the colour this stack can print there landed from the picture: the same
    // colour difference mean_delta_e averages, one byte a pixel, scaled by
    // CHROMA_ERROR_IMAGE_FULL_SCALE_DE and clamped.
    //
    // The two numbers below say how good the match is; this says *where* it is bad, which is the
    // question a user actually has - a photograph with one unreachable red flower in it and a
    // photograph that is wrong all over have the same mean. The panel draws it as the pixels this
    // stack cannot reach.
    std::vector<uint8_t> delta;

    // How far the prediction landed from the picture, as a mean and worst-case dE over all
    // pixels. The panel reports it because it is the only honest answer to "will this stack
    // reproduce my image": under about 5 is a good match, over about 15 means the filaments cannot
    // reach the picture's colours and no amount of layers will fix it.
    //
    // CIE76 by default and CIEDE2000 when the solve was asked for a high quality match
    // (ChromaSolveOptions::high_quality). Both are dE on the same scale and the thresholds above hold
    // for either, which is what lets the panel report one number whichever metric produced it.
    //
    // The mean is weighted by ChromaStack::importance when the stack carries one, so the number
    // answers "is this stack right for the part of the picture that matters" rather than "for the
    // most of it". The worst case is not weighted: it is the worst pixel there is, and an importance
    // region is a statement about what to optimise, not about what to stop looking at.
    double mean_delta_e = 0.;
    double max_delta_e  = 0.;

    bool empty() const { return width <= 0 || height <= 0 || levels.empty(); }
};

// The colour difference that `ChromaSolve::delta` writes as 255, so a byte covers the whole range the
// number is read in: 0 is an exact match, about 5 (a byte of 51) is where a careful eye starts to see
// a difference at all, 15 (153) is the stack failing to reach the colour, and anything past 25 is
// failing so badly that how much worse it is stops carrying information. Clamped rather than scaled to
// the worst pixel, because a scale that moves with the picture cannot be compared between two stacks,
// which is the one thing the image is for.
static constexpr float CHROMA_ERROR_IMAGE_FULL_SCALE_DE = 25.f;

// How to run the solve, as against what it is solving. The stack says which colours can be printed;
// this says how hard to look for them and how coarse an answer the printer can actually make.
//
// Defaulted, it is exactly the solve as it was before any of this existed, byte for byte: a unit test
// holds that. Everything here is the caller's to decide per run and none of it is saved with the
// stack, because every one of them is a property of the machine or of the moment rather than of the
// picture - the line width the cell is derived from comes from the print profile, and whether this run
// is the interactive one or the final one is a fact about the run.
struct ChromaSolveOptions
{
    // Match with CIEDE2000 instead of CIE76.
    //
    // CIE76 is here because the match used to run on the UI thread, where several hundred million Lab
    // distances for a large picture was the whole budget; it now runs on a worker, so the better
    // metric is affordable for the run whose result gets printed. The coarse interactive solve stays
    // on CIE76, because it is redone on every keystroke and the two metrics disagree about one pixel
    // in a few hundred - visibly never, for the purpose the coarse solve serves. The output is the
    // same shape and the same format either way; only which palette entry wins can differ.
    bool high_quality = false;

    // The side of a dither cell, in texels of the grid being solved. 1 is Floyd-Steinberg as it has
    // always been, one texel at a time.
    //
    // Larger is for the printer rather than for the eye. Error diffusion scatters single-texel changes
    // of height, and a region narrower than the nozzle's line width is not a thin wall, it is nothing
    // at all: the slicer drops it, so the preview promises a tone the print cannot make and dither
    // makes a photograph worse rather than better. Diffusing at cell granularity instead - one choice
    // of height for the whole cell, the residual passed on to neighbouring cells - makes the smallest
    // region whose height changes one cell across, which is the smallest region the printer can
    // actually build.
    //
    // The caller computes it, because the library does not know the print: it is the line width in
    // millimetres divided by the millimetres a texel covers *on the grid this solve runs on*, rounded
    // up and at least 1. The grid matters - the GUI solves a preview at a few hundred texels a side
    // and the final pass at the picture's own resolution, so the same print is a different number of
    // texels in each.
    int dither_cell_px = 1;
};

// The painted surface itself, sampled on the solve's own grid: how far from level it is at each
// texel of the picture. Built by the GUI, which is the only place that knows where the picture lands
// on the mesh.
struct ChromaSurface
{
    int width  = 0;
    int height = 0;
    // cos(tilt) of the surface under this texel, i.e. the component of its normal along the print
    // axis. 0 where no painted triangle covers the texel, which is also how "nothing here" is told
    // from "level here".
    std::vector<float> facing;

    bool empty() const { return width <= 0 || height <= 0 || facing.empty(); }
};

// Deepens the solved relief where the surface is tilted, so its steps still land on the print heights
// the colours were solved for, and returns the depth in millimetres the layer must carry for the
// rewritten grey map to mean the same thing.
//
// The grey map is a linear encoding of "how far to displace along the normal", so the correction is a
// per-texel factor on it: 1/cos(tilt), clamped at CHROMA_MAX_TILT_DEG. The deepest texel sets the
// layer's depth and keeps the full 8 bits in use.
//
// A level patch leaves the map and the depth exactly as they were, so nothing about a flat picture
// changes by passing through here.
float chroma_tilt_relief(ChromaSolve &solve, const ChromaStack &stack, const ChromaSurface &surface);

// Turns a solved height map into the one a face-down plate needs, by inverting it.
//
// The solve always says the same thing: the grey level is how much of the relief this column prints,
// 0 at the base and 255 at the top of the stack. Face up that is how far the column stands above the
// plate. Face down the picture's surface is the first layer and the relief is a recess cut up into
// the plate behind it, so a column that prints more of the relief is one whose surface starts *lower*
// - the displacement is the empty space above it, which is the complement.
//
// Only the height map changes. The levels, the preview and the error are about colour and are the
// same picture whichever way up it is printed, and the depth the layer carries is still the relief's
// own depth.
//
// Apply it exactly once, to a face-down plate only, and do not pass the result through
// chroma_tilt_relief(): that corrects a painted patch that is not level, and a generated plate's
// picture face is flat by construction, which is the only surface this mode is for.
void chroma_invert_relief(ChromaSolve &solve);

// Solves `rgb` (width * height * 3 bytes, row-major, top-to-bottom) against the stack's palette.
// An empty stack, a zero-sized image or a stack with no relief layers gives an empty result.
//
// It runs as five stages, and they are stages rather than one pass because each of them is a different
// decision and all but the first are optional:
//
//  a. the palette and its precomputed Lab - what this stack can print, built once;
//  b. the match - per pixel, which of those colours is closest, by CIE76 or CIEDE2000;
//  c. the dither - the same match with the residual passed on to neighbouring cells instead of thrown
//     away, which replaces (b) rather than following it, since the choice a pixel makes depends on
//     what its neighbours did;
//  d. the retouch - the stack's own edits, applied in order over whatever (b) or (c) decided;
//  e. the outputs - the quantization to a grey level, the preview, the error image and the statistics.
//
// Weighting one pixel against another is ChromaStack::importance, and it lands in (e): which colour a
// pixel gets is decided by that pixel alone, so a weight cannot change the match, only how much that
// pixel's error counts in the mean. Anything that overrides a decision belongs beside (d) as another
// pass over the choices, and anything that changes what a pixel is matched against belongs between
// (a) and (b). None of them needs to touch the other stages.
//
// The overload without options is the one the GUI and ChromaSolveJob call, and it is this one with
// everything defaulted.
ChromaSolve chroma_solve(const uint8_t *rgb, int width, int height, const ChromaStack &stack,
                         const ChromaSolveOptions &options);
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

// The solve's error image as an 8-bit grayscale PNG, on the scale ChromaSolve::delta documents: black
// where the stack reaches the picture and white where it cannot. A PNG rather than the bytes because
// that is what everything downstream of here reads an image as, including the panel's own thumbnails.
std::vector<unsigned char> chroma_encode_error_png(const ChromaSolve &solve);

// A painted region turned into the run lengths ChromaMask holds. `mask` is one byte a pixel over
// `width` x `height`, row-major and top-to-bottom; anything non-zero is inside the region. This is the
// only way a mask should be built, so that the convention (first run outside, trailing outside run
// dropped) lives in one place.
ChromaMask chroma_mask_encode(const uint8_t *mask, int width, int height);

// The mask back as one byte a pixel, 0 outside and 1 inside, over its own grid. Applying an edit does
// this once and then samples the result; a caller that wants to show or edit a mask wants the same
// thing. A malformed mask - runs that overrun or fall short of the grid - gives what it does say and
// zeroes for the rest, rather than nothing: a bad run list in a project file should cost its edit, not
// the project.
std::vector<uint8_t> chroma_mask_decode(const ChromaMask &mask);

// How many printed layers above the base the top of band `band` (0-based) is, which is what
// ChromaRetouchEdit::Target::Band resolves to. -1 is the bare base and gives 0; a band past the end of
// the stack gives the whole relief.
int chroma_band_top(const ChromaStack &stack, int band);

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
//
// These come out in printed order, bottom-up, which face up is the order the stack is written in: the
// base first, in the bottom band's filament, then the bands. On a face-down plate
// (ChromaOrientation::FaceDown) it is the reverse. The light enters through the face that printed
// first, so the band it meets first has to be the one in the first layer, and the base - still the
// opaque thing the light ends on - is printed last, on top. The merging then does for the base what
// it does face up, since the base prints in the bottom band's filament and the bottom band is now the
// range just below it.
std::vector<ChromaRange> chroma_ranges(const ChromaStack &stack, double z_bottom = 0.);

// A picture's colours, coarsely bucketed and weighted by how much of it they cover. Scoring a
// candidate stack against this rather than against the picture is what makes trying hundreds of them
// affordable: a photograph is millions of pixels and a few hundred distinguishable colours.
struct ChromaHistogram
{
    std::vector<std::pair<ColorRGB, float>> bins; // colour and its weight, summing to 1
    bool empty() const { return bins.empty(); }
};

// The colour difference at which a bin is as far from the picture's own average as this weighting
// cares to count. Past it the boost stops growing, so one freakish pixel cannot outvote a photograph.
static constexpr float CHROMA_VIVID_REFERENCE_DE = 40.f;

// What `vividness` is worth passing. 0 is coverage alone, which is what this did before the
// parameter existed and what it still does by default; 1 doubles the weight of a bin a whole
// reference dE away from the average, which is enough for a small saturated area to earn a band
// without letting it take over a picture that is mostly one colour.
static constexpr float CHROMA_VIVIDNESS_DEFAULT     = 0.f;
static constexpr float CHROMA_VIVIDNESS_RECOMMENDED = 1.f;

// `vividness` is why a small vivid area can be worth a filament.
//
// Weighted by coverage alone, a mostly-grey picture with one saturated flower in it has the flower
// outvoted by the background: the picker then chooses filaments that serve the grey, the suggestions
// suggest greys, and the one thing the picture is about prints as a smudge. Coverage is the wrong
// measure on its own, because what a stack is for is the colours that are *distinctive*, and a bin's
// distance from the picture's own average colour is exactly that:
//
//     weight = coverage * (1 + vividness * min(dE(bin, average) / CHROMA_VIVID_REFERENCE_DE, 1))
//
// The average is the coverage-weighted mean of the bins in CIELAB, so it is the picture's own centre
// rather than a fixed grey, and a picture that is all one colour has every bin sitting on it: every
// factor is 1 and the weighting is coverage, which is why a flat picture behaves exactly as it did.
// The capping also happens on the weight rather than on coverage, so a vivid sliver survives
// `max_bins` instead of being the first thing dropped.
//
// `importance` is the other half of the same question and the user's own answer to it: a pixel inside
// the region counts for ChromaImportance::weight pixels of coverage. Both act on the same number, so
// a region that is painted and a colour that is distinctive compound rather than fight. An empty one
// (the default) leaves every pixel counting once.
ChromaHistogram chroma_histogram(const uint8_t *rgb, int width, int height, int max_bins = 192,
                                 float vividness = CHROMA_VIVIDNESS_DEFAULT,
                                 const ChromaImportance &importance = ChromaImportance{});

// Picks `count` of `available`, in printing order, for a picture.
//
// The order matters as much as the choice - a stack is filaments seen through each other, so the same
// four in a different order reach different colours - which is why this searches orderings rather than
// just picking the four nearest the picture's own colours. It grows the stack one filament at a time,
// trying every remaining filament at every position and keeping whichever placement loses the least to
// the picture, which is a few dozen candidate stacks instead of the tens of thousands an exhaustive
// search over subsets and permutations would be.
//
// Bands come out equal, since what each one needs depends on the stack it ends up in; chroma_trim_bands()
// is what shortens them afterwards. An empty result means there was nothing to choose from.
ChromaStack chroma_auto_stack(const ChromaHistogram &picture, const std::vector<ChromaFilament> &available,
                              int count, float layer_height_mm, int base_layers, int relief_layers);

// How far a stack lands from a picture, as the mean colour difference over `picture`'s bins - the same
// measure the solve reports, computed without solving. This is what chroma_auto_stack() minimises.
double chroma_stack_error(const ChromaHistogram &picture, const ChromaStack &stack);

// One colour worth owning for a picture, and what the user already has that is like it.
struct ChromaSuggestion
{
    ColorRGB color;            // the colour to go looking for
    float    td_mm = 4.f;      // how translucent it wants to be, for the band it would print
    int      layers = 1;       // how many layers that band would get
    int      nearest = -1;     // the loaded filament closest to it, or -1 when none is loaded
    float    nearest_de = 0.f; // how far that one is, as a colour difference
};

// The `count` colours a picture would most like to be printed in, in printing order, whether or not
// anyone owns them.
//
// chroma_auto_stack() answers "which of my filaments", which is the question with a cheap answer and
// a ceiling: a picture whose colours are not in the drawer cannot be printed well out of it however
// the drawer is arranged. This answers the other one - what to go and buy - by letting the colours
// themselves move: a weighted k-means over the picture's colours for a starting set, then a hill
// climb that walks each band's colour wherever the stack's own error falls fastest.
//
// Each suggestion carries the transmission distance its band wants, since a colour is only half of
// what to look for: the same red at TD 1 and at TD 6 are different filaments for this purpose.
std::vector<ChromaSuggestion> chroma_suggest_colors(const ChromaHistogram &picture, int count,
                                                    const std::vector<ChromaFilament> &loaded,
                                                    float layer_height_mm, int relief_layers);

// A step wedge for measuring a filament's transmission distance by printing it.
//
// TD cannot be read off a spool and no filament preset ships one, so every stack starts from a guess
// and every prediction rests on it. The measurement is a staircase of the filament under test printed
// over a contrasting base: the step at which the base stops showing through is the depth at which that
// filament covers, and CHROMA_TD_OPAQUE_FRACTION turns that depth into a TD.
struct ChromaWedge
{
    int   steps           = 16;    // how many thicknesses to offer
    float layer_height_mm = 0.08f; // the height they step by, and the height it must be printed at
    int   base_layers     = 6;     // the contrasting base under them, thick enough to be opaque itself
    float step_depth_mm   = 7.f;   // how deep each step is, i.e. how much of it there is to look at
    float width_mm        = 36.f;  // across the staircase
    // Every fifth step is widened by this much, so the step a user is looking at can be counted
    // without counting every step up to it.
    float tick_mm         = 5.f;

    float base_height_mm() const { return float(base_layers) * layer_height_mm; }
};

// The frame's cross-section, swept around the outline to make the band between the plate's edge and
// the picture.
//
// A polyline in (how far in from the edge, how high above z = 0), outer edge first, ending where the
// picture starts: the last sample is always (width_mm, base_mm), because that is where the frame
// meets the picture's own surface. Two samples at the same inset are a vertical face, which is what
// makes a step a step.
//
// Everything a frame can be is this polyline, which is why the profiles started as an enum of shapes
// rather than a library of models: a thicker frame is a longer sweep of the same curve, and an SVG
// outline is the same curve swept around a different loop.
//
// A frame that carries its own cross-section (ChromaFrame::profile_points, which is what a frame out
// of the gallery is) is returned as it stands, with base_mm added to its heights and nothing of the
// Profile enum consulted: the whole point of a gallery is that a real frame's section is not one of
// eight shapes, and rounding it to the nearest of them would make the gallery a lie. What such a
// definition has to satisfy, and what is quietly enforced here rather than refused, is:
//
//  - at least two samples, or it is not a section and the enum is used instead;
//  - insets inside [0, width_mm] and never going backwards, because each sample becomes one ring of
//    the sweep and the rings have to march steadily in from the plate's edge. A sample that goes back
//    on the one before it is pulled forward to it, which turns an overhang into a vertical face;
//  - heights at or above the picture's surface, since the frame surrounds the picture rather than
//    cutting into it;
//  - the first sample at the plate's edge and the last where the picture starts, which are forced, so
//    the frame cannot meet the picture in a step of rounding error.
//
// Two samples at the same inset are still a vertical face, and that is the only way a definition can
// say one.
std::vector<Vec2f> chroma_frame_profile(const ChromaFrame &frame, float base_mm);

// The plate's outline, in millimetres, centred on the picture: the loop the frame's profile is swept
// around. `grow_mm` is how far outside the picture's own rectangle it runs, which is the frame's
// width for the plate's edge and less than that for a ring further into the sweep.
//
// Shape::Outline returns the frame's own loop, scaled about its centre so that it sits `grow_mm`
// outside the picture - an SVG is a shape, not a size, and the picture is what has the size.
std::vector<Vec2f> chroma_frame_outline(const ChromaPlate &plate, float grow_mm);

// The plate as a mesh, sitting on z = 0 and centred on the origin.
//
// The picture's own face comes first in `indices`, which is what lets the caller paint it without
// knowing anything about how the rest of the plate is built: the first `picture_triangles` triangles
// are the surface the picture is printed on, and everything after them is the frame and the backing.
//
// Face down (ChromaOrientation::FaceDown) the same thing is built the other way up: the picture's
// face is the plate's bottom, at z = 0, so it prints against the build plate and comes out as smooth
// as the plate is, and the whole bottom of the plate is that one flat plane - a frame cannot stand
// proud of the picture there, because there is nothing below the first layer to stand on, so the
// frame's section shapes the *back* instead. The picture's triangles still come first and are still
// the surface to paint; they simply face downwards, and the relief is displaced up into the plate
// rather than out of it.
struct ChromaPlateMesh
{
    indexed_triangle_set its;
    size_t               picture_triangles = 0;

    bool empty() const { return its.indices.empty(); }
};

ChromaPlateMesh chroma_plate_mesh(const ChromaPlate &plate);

// The wedge as a mesh, sitting on z = 0. The base is one slab; step i (1-based) stands i layers proud
// of it, so the filament under test is i layers thick there.
indexed_triangle_set chroma_wedge_mesh(const ChromaWedge &wedge);

// The transmission distance a wedge has measured: `step` is the first step at which the base below no
// longer shows through. 0 or less means it was not readable, and gives 0.
float chroma_td_from_step(int step, float layer_height_mm);

// A starting point for a stack: the given filaments, lightest at the bottom, each with an equal
// share of `relief_layers` on top of `base_layers`.
//
// Light-to-dark is the convention filament-painting guides teach, and it is not arbitrary: a dark
// filament over a light one reads as a midtone at one or two layers and as the dark colour at four,
// so putting the dark filaments last is what buys the long tonal ramp. The reverse order gives a few
// washed-out steps and then nothing.
ChromaStack chroma_default_stack(const std::vector<ChromaFilament> &filaments, float layer_height_mm,
                                     int base_layers, int relief_layers);

// The stack as the keys of the .3mf's `chroma` object, and back.
//
// It lives here, beside the fields, rather than being written out by hand where the texture
// displacement layers are turned into JSON, because that is the only way a field added to the stack
// cannot be forgotten by the project file. The caller owns the object and the keys that are the
// *layer's* rather than the stack's (`source_in_3mf`), and the decision that a stack with no bands in
// it is not a ChromaStack layer at all.
//
// The plate goes in only when the picture made one, and it is not what rebuilds the slab - the mesh
// is already in the project. It is there for everything about a plate that triangles cannot say:
// which way up it prints, which frame out of the gallery it is wearing, and that frame's own
// cross-section. A project written before the key existed comes back without them, which is exactly
// the plate it was.
//
// Keys are only ever added. Every one of them is optional on the way back in and leaves the field at
// whatever the stack handed in already has, so a project saved before a field existed reads as the
// print it was: `lighting` absent is front lit, `retouch` absent is no edits, and nothing about either
// changes a height map solved without them.
void chroma_stack_to_json(nlohmann::json &chroma, const ChromaStack &stack);
void chroma_stack_from_json(const nlohmann::json &chroma, ChromaStack &stack);

} // namespace Slic3r

#endif // slic3r_ChromaStack_hpp_
