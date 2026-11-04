#version 140

// What a ChromaStack print looks like, with the stack composited per fragment.
//
// The colour is not a vertex attribute, not a texture on the surface and not a table: the fragment
// walks the filament stack itself, the same Beer-Lambert absorption the solver used, from its own
// height. A table indexed by height can only hold the answer for one geometry - a column seen
// straight down - and the two places that is not true are exactly where a print is looked at:
//
//  - at a grazing angle the light leaving the surface crosses each layer along the view ray rather
//    than straight up, so it passes through more filament and the colour deepens;
//  - a wall is not a column seen through at all. It is the cut side of the layers at that height,
//    backed by more of the same filament, so it shows that band's own colour - which is why the side
//    of a ChromaStack print is a set of stripes and its top is a picture.
//
// Per band rather than per layer, which is exact rather than a shortcut: a run of n layers of one
// filament passes exp(-n d) whether it is composited in one step or n, so a stack of sixteen bands
// costs sixteen iterations however many hundreds of layers it has.
//
// The same walk answers the two other things a user wants to see of a stack, and both are off unless
// a uniform says otherwise, so the view above is what an unset uniform gives:
//
//  - back lit, where the light is behind the print and what reaches the eye is only what got through
//    the whole column, so the bands' transmittances multiply instead of compositing over each other;
//  - the error overlay, which paints how far the solve landed from the picture instead of the colour
//    it predicts, because how good the match is cannot be read off the match itself.

#define INTENSITY_CORRECTION 0.6
const vec3 LIGHT_TOP_DIR = vec3(-0.4574957, 0.4574957, 0.7624929);
#define LIGHT_TOP_DIFFUSE    (0.8 * INTENSITY_CORRECTION)
#define LIGHT_TOP_SPECULAR   (0.125 * INTENSITY_CORRECTION)
#define LIGHT_TOP_SHININESS  20.0
const vec3 LIGHT_FRONT_DIR = vec3(0.6985074, 0.1397015, 0.6985074);
#define LIGHT_FRONT_DIFFUSE  (0.3 * INTENSITY_CORRECTION)
#define INTENSITY_AMBIENT    0.3

#define CHROMA_MAX_BANDS 16

// Which side the light comes from. Front is the reflected light of the stack composited downward, and
// it is 0 so that a uniform nobody sets leaves the print looking exactly as it did.
#define CHROMA_LIT_FRONT 0
#define CHROMA_LIT_BACK  1
// A lamp behind the plate and a room in front of it, which is where most back-lit prints actually
// hang. The two lights add rather than one replacing the other, so this is the weighted sum of the
// two walks below and not a third walk of its own.
#define CHROMA_LIT_BOTH  2

// Back lit, a band is a filter rather than a layer of paint, and the channel a filament reflects is
// the channel it passes - so its absorption runs against its own colour instead of being neutral.
// The floor is what keeps a pale filament darkening with depth at all: with no floor a white band
// would pass light as if it were not there, and depth is the only thing a lithophane draws with.
#define CHROMA_BACK_ABSORB_FLOOR 0.35

// The ramp the colour-difference image is drawn through. Its lightness falls the whole way up as well
// as its hue turning, so it reads in a greyscale screenshot and to a colour-blind eye, and it starts
// at the scene's own neutral grey rather than at any filament's colour: nothing on this ramp should
// be mistakeable for something the print could print.
const vec3 ERROR_RAMP_MATCH = vec3(0.80, 0.80, 0.80);
const vec3 ERROR_RAMP_CLOSE = vec3(0.95, 0.78, 0.25);
const vec3 ERROR_RAMP_OFF   = vec3(0.92, 0.33, 0.12);
const vec3 ERROR_RAMP_WRONG = vec3(0.72, 0.02, 0.30);
// The knees are the numbers the panel already explains a solve with, in the units the image carries:
// a byte of 0 is a perfect match and 255 is ERROR_DE_FULL, under ERROR_DE_GOOD the stack reproduces
// the picture, and over ERROR_DE_BAD its filaments cannot reach the picture's colours at all.
#define ERROR_DE_FULL  25.0
#define ERROR_DE_GOOD   5.0
#define ERROR_DE_BAD   15.0
// How much of the surface's shading the overlay keeps. All of its gradient, compressed toward full
// brightness: the shape still has to read, but a wrong pixel on the dark side of the model must not
// be hidden by the light it happens to sit in.
#define ERROR_SHADE_FLOOR 0.62

// The stack, bottom-up. Band 0 carries the solid base folded into it, since the base prints in its
// filament and the model treats it as opaque anyway.
uniform int   band_count;
uniform vec3  band_color[CHROMA_MAX_BANDS];
// The optical depth of ONE layer of that band: h / (CHROMA_TD_OPAQUE_FRACTION * TD / 3), so the layer
// passes exp(-density) of what is behind it. Precomputed because TD, the layer height and the
// constants that relate them belong to the model, not to a shader.
uniform float band_density[CHROMA_MAX_BANDS];
uniform float band_layers[CHROMA_MAX_BANDS];
uniform float total_layers;
// How much of that is the picture: the stack's own layers, without the opaque base under (or, face
// down, over) them. It is the sum of band_layers above and is uploaded rather than summed here only
// because the walk below would otherwise need a loop before it can start.
uniform float relief_layers;
// 1 when the plate prints picture down, 0 otherwise.
//
// Face down the picture's face is the plate's *bottom* and the opaque base is on top, so the filament
// a fragment is seen through is everything ABOVE it rather than everything below. The walk itself does
// not change - the base is still composited first and every band in front of it over that - only the
// depth each fragment is at, which is measured from the other end of the stack. Everything else in
// this shader, the walls and the layer lines included, follows from that one number.
uniform int   stack_flipped;

// The print as it was when it had got this far up, or 0 for the finished print.
//
// A stack is paid for one swap at a time and it is not obvious from the finished picture which swap
// bought what. Walking the limit up the stack answers that: everything already below it is finished
// and does not move, and everything still to come shows the colour the stack had reached at the
// time, which is what the print on the bed actually looked like at that layer. It costs one clamp,
// because the walk is already layer by layer from the bottom.
uniform float layer_limit;

// World Z of the plate's picture plane: the bottom of the object as its mesh stands before the preview
// displaces it, which is the face the relief grows out of in both orientations.
uniform float base_z;
uniform float layer_height;  // mm
// World +Z in eye space: the direction the stack is built along, which is what the view ray's path
// through it is measured against.
uniform vec3  stack_axis;
// How translucent the stack reads overall, 0 to 1, from the filament on top. Drives the wrap and the
// rim; an opaque stack should not glow.
uniform float translucency;
uniform float alpha;

// Whether the plate is drawn as the translucent object it is.
//
// The walk below already knows how much light the stack stops, because stopping light is the only
// thing it computes: the product of the bands' transmittances along the view ray is what the plate
// lets past. Handed to the blender as this fragment's alpha, that *is* the see-through part of how a
// print looks, and no ray has to be traced through the scene for it - whatever stands behind the plate
// is already in the colour buffer by the time this pass runs, so one blend per fragment buys what a
// ray would have gone and fetched.
//
// What it does and does not show is worth being plain about. A ChromaStack plate is built to be
// opaque: CHROMA_TD_OPAQUE_FRACTION says a filament hides what is behind it after about a tenth of its
// TD, so a finished two-millimetre stack stops essentially all of the light and comes out opaque here
// too, correctly. What shows through is the thin places - the first layers of a clear filament, a plate
// whose relief is most of its depth, the stack as the layer slider walks up it - which is also where a
// real print is see-through and nowhere else.
//
// 0, which is what an unset uniform gives, draws the plate opaque exactly as before.
uniform int see_through;

// CHROMA_LIT_FRONT or CHROMA_LIT_BACK, and the panel the print stands on when it is back lit. The
// colour and the intensity are read only in that mode, so leaving them unset cannot dim anything.
uniform int   lighting_mode;
uniform vec3  led_color;
uniform float led_intensity;
// CHROMA_LIT_BOTH only: how much of what the eye gets came through the plate. 0 is the front-lit view
// exactly and 1 the back-lit one, which is what keeps the mode continuous with the two it sits between.
uniform float lighting_balance;

// The solve's colour difference per pixel of the picture, in its red channel: 0 where the stack hit
// the picture's colour and ERROR_DE_FULL or worse at 255. Sampled only when the overlay is on.
uniform int       error_enabled;
uniform sampler2D error_tex;

// The picture's own placement, so the overlay lines up with the pixels that are wrong. A ChromaStack
// picture is pinned to the XY plane rather than blended across the three planes - its colours are
// print heights, so it has to keep its place on the plate as the surface tilts - which is what makes
// this a closed form in the fragment's world position instead of a uv the mesh has to carry.
// libslic3r's box_plane_xy() and apply_uv_transform(), term for term.
uniform vec3  tex_anchor;    // the volume's origin in world space: the texture frame's origin
uniform float tiling_scale;  // mm per uv unit
uniform float tex_aspect;    // picture width / height
uniform float rotation_rad;
uniform vec2  uv_offset;

// Lateral scattering: light that goes into the plastic at one point and comes back out at another.
//
// The walk above is the straight-through part and it is exact, but filament is a dense scattering
// medium - nearer to wax than to glass - so a print is softer than the solve it came from. Detail
// finer than the depth at which the filament goes opaque does not survive the plastic, which is why
// a real plate never shows the hard terrace edges this shader drew without it.
//
// It is done in the picture's own space rather than on screen: the picture is flat and its uv is
// already reconstructed here in closed form (picture_uv()), so the radius can be stated in
// millimetres on the print and stay the same however far the view is zoomed. A screen-space blur
// would have to be re-derived per fragment, would smear across silhouettes, and would need a
// resolve pass this gizmo does not have.
//
// `scatter_tex` holds what the solve predicts the print looks like, straight down, at the solve's own
// resolution. What is added back is the DIFFERENCE the neighbourhood makes, not the neighbourhood
// itself, so everything the walk knows about grazing angles and walls survives untouched.
uniform int       scatter_enabled;
uniform sampler2D scatter_tex;
uniform vec2      scatter_uv;      // the scattering radius, in uv, already corrected for the picture's aspect
uniform float     scatter_strength;

// ---- The debug views ----
//
// What the shader believes, drawn instead of what it predicts. The preview answers "does this look
// right"; when it does not, each of these answers a different "why not". The first is the one that
// matters most often: a stack whose relief never reached the mesh reads as the top filament
// everywhere, and that looks exactly like a correct solve of a very dark picture.
#define CHROMA_DBG_OFF    0
#define CHROMA_DBG_HEIGHT 1
#define CHROMA_DBG_BAND   2
#define CHROMA_DBG_FACING 3
#define CHROMA_DBG_UV     4
#define CHROMA_DBG_SOLVE  5
#define CHROMA_DBG_ALBEDO 6
uniform int chroma_debug;

in vec3  clipping_planes_dots;
in vec3  eye_normal;
in vec3  eye_position;
in vec3  world_normal;
in vec2  world_xy;
in float world_z;

out vec4 out_color;

vec2 picture_uv()
{
    vec2 planar = vec2(world_xy.x - tex_anchor.x, -(world_xy.y - tex_anchor.y));
    planar     *= (tiling_scale > 1.0e-6) ? (1.0 / tiling_scale) : 1.0;
    float cs    = cos(rotation_rad);
    float sn    = sin(rotation_rad);
    vec2  uv    = vec2(planar.x * cs - planar.y * sn, planar.x * sn + planar.y * cs);
    // After the rotation, so the rotation stays a rotation rather than becoming a shear.
    uv.y *= tex_aspect;
    return uv + uv_offset;
}

// Piecewise, so the stops land on the differences that mean something rather than on even thirds. Each
// mix only begins where the one before it has finished, which is what the clamps are doing.
vec3 error_ramp(float e)
{
    vec3 c = mix(ERROR_RAMP_MATCH, ERROR_RAMP_CLOSE, clamp(e * ERROR_DE_FULL / ERROR_DE_GOOD, 0.0, 1.0));
    c      = mix(c, ERROR_RAMP_OFF, clamp((e * ERROR_DE_FULL - ERROR_DE_GOOD) / (ERROR_DE_BAD - ERROR_DE_GOOD), 0.0, 1.0));
    return   mix(c, ERROR_RAMP_WRONG, clamp((e * ERROR_DE_FULL - ERROR_DE_BAD) / (ERROR_DE_FULL - ERROR_DE_BAD), 0.0, 1.0));
}

// Two rings of six taps and a centre, weighted as a coarse Gaussian. Written as trigonometry rather
// than a table of offsets because GLSL 110 has no array initializers, and the same source has to
// compile on both profiles.
// The ramp the debug views measure with: dark blue at 0 through cyan and green to pale yellow at 1.
// Its lightness rises the whole way, so a value can be read off it in a greyscale screenshot and by
// an eye that does not separate the hues.
vec3 debug_ramp(float t)
{
    t      = clamp(t, 0.0, 1.0);
    vec3 c = mix(vec3(0.10, 0.05, 0.45), vec3(0.00, 0.55, 0.85), clamp(t * 3.0, 0.0, 1.0));
    c      = mix(c, vec3(0.30, 0.85, 0.25), clamp(t * 3.0 - 1.0, 0.0, 1.0));
    return   mix(c, vec3(1.00, 0.95, 0.35), clamp(t * 3.0 - 2.0, 0.0, 1.0));
}

// A colour per band index. The golden ratio puts adjacent indices on opposite sides of the hue
// wheel, so one band's worth of step reads as a step rather than as a gradient.
vec3 debug_index_color(float i)
{
    float h = fract(i * 0.6180339887);
    vec3  k = vec3(1.0, 2.0 / 3.0, 1.0 / 3.0);
    return clamp(abs(fract(vec3(h) + k) * 6.0 - 3.0) - 1.0, 0.0, 1.0) * 0.85 + 0.15;
}

vec3 scatter_at(vec2 uv, out vec3 centre)
{
    centre     = texture(scatter_tex, uv).rgb;
    vec3  sum  = centre * 0.42;
    float wsum = 0.42;
    for (int ring = 0; ring < 2; ++ring) {
        float r = (ring == 0) ? 0.45 : 1.0;
        float w = (ring == 0) ? 0.055 : 0.03;
        float t = (ring == 0) ? 0.0 : 0.5;
        for (int i = 0; i < 6; ++i) {
            float a = (float(i) + t) * 1.0471976;
            sum  += texture(scatter_tex, uv + vec2(cos(a), sin(a)) * r * scatter_uv).rgb * w;
            wsum += w;
        }
    }
    return sum / max(wsum, 1.0e-4);
}

void main()
{
    if (any(lessThan(clipping_planes_dots, vec3(0.0))))
        discard;

    // How high this fragment stands above the plate's picture plane, in layers. That plane is the
    // undisplaced face the relief was displaced from, and it is the object's own bottom in both
    // orientations, because base_z is read off the volume's mesh before the preview displaces it.
    float z_rel  = (world_z - base_z) / layer_height;
    float layers = max(z_rel, 0.0);
    // Measured from the top down when the picture is on the underside: the relief is cut up into the
    // plate there, so a column whose face has risen k layers off the bed is k layers shorter than the
    // stack, and what the eye looking at that face sees through is the rest of it. A uniform decides
    // it, so every fragment of a quad takes the same branch and fwidth() below still has its
    // derivatives.
    //
    // Against the relief rather than the whole stack, because the base is on the far side of the
    // picture here: a column raised the full depth has the base directly against its face and is seen
    // through nothing at all. Measuring against total_layers instead adds the base's own thickness to
    // every column, which saturates the deepest fifth of the picture into one flat field and leaves
    // the rest a band too dark.
    if (stack_flipped != 0)
        layers = max(relief_layers - layers, 0.0);
    // How much of a layer one pixel covers. Past half a layer the height is taken as continuous, so a
    // surface too far away to resolve its layers averages them instead of flickering between them.
    float px      = fwidth(layers);
    float stepped = clamp(ceil(layers - 1.0e-4), 1.0, total_layers);
    float here    = mix(stepped, clamp(layers, 0.0, total_layers), clamp(px - 0.5, 0.0, 1.0));
    // Clamped after the sub-layer blend above rather than before it, so that a surface too far away
    // to resolve its layers still averages them instead of snapping to the limit.
    if (layer_limit > 0.0)
        here = min(here, layer_limit);

    vec3 N = normalize(eye_normal);
    vec3 V = normalize(-eye_position);

    // The view ray's path through one layer, in layers. Straight down it is 1; at 60 degrees off the
    // stacking axis it is 2. Clamped because a ray along the layers passes through no stack at all,
    // and that case is the wall below rather than an infinite path.
    float facing = clamp(abs(dot(normalize(stack_axis), V)), 0.0, 1.0);
    float path   = 1.0 / max(facing, 0.25);

    // Walk the bands from the bottom, stopping at the layer this fragment sits at. `own` is the
    // filament of the band that layer belongs to, which is what the side of the print shows.
    //
    // `stacked` is the light the filament itself sends back, already weighted by how much of the
    // fragment it covers, and `clear` is what the whole column passes. Kept apart rather than
    // composited over a backing colour because the two answers wanted below differ only in what is
    // assumed to be behind the stack, and one walk gives both.
    vec3  stacked   = vec3(0.0);
    float clear     = 1.0;
    vec3  own       = band_color[0];
    vec3  through   = vec3(1.0);
    // Which band the fragment's own layer fell in, for the debug views; the walk itself has no use
    // for it, since `own` already carries that band's colour.
    float band_hit  = 0.0;
    float consumed  = 0.0;
    for (int i = 0; i < CHROMA_MAX_BANDS; ++i) {
        if (i >= band_count)
            break;
        float n = clamp(here - consumed, 0.0, band_layers[i]);
        if (n <= 0.0)
            break;
        float t = exp(-band_density[i] * path * n);
        stacked = (1.0 - t) * band_color[i] + t * stacked;
        clear  *= t;
        // Back lit, nothing is composited over anything: what leaves the band is what it passed, so
        // the transmittances multiply. The same density and the same lengthened path - a ray crossing
        // the stack at an angle goes through more filament whichever side the light is on - with the
        // absorption split per channel by the band's colour. Behind the one branch a uniform decides,
        // so the front view pays nothing for it.
        if (lighting_mode != CHROMA_LIT_FRONT)
            through *= exp(-band_density[i] * path * n *
                           mix(vec3(CHROMA_BACK_ABSORB_FLOOR), vec3(1.0), 1.0 - band_color[i]));
        own       = band_color[i];
        band_hit  = float(i);
        consumed += band_layers[i];
    }

    // The two readings of that one walk.
    //
    // Backed by band 0 - the filament the base prints in, which the model treats as opaque - it is the
    // old composite: stacked + clear * band_color[0] expands term for term to the recursion this loop
    // used to carry, by induction on the bands with "nothing walked yet" giving band_color[0]. So the
    // opaque view is the same expression and not an approximation of it, down to the one extra
    // multiply-add's worth of float rounding.
    //
    // Divided by what the stack stopped instead, it is the colour of the filament on its own with
    // nothing assumed behind it. That is the one to shade when the blender is about to put the scene
    // behind the plate back in: the colour has to be what the plate adds, not what it adds plus a
    // backing it has not got.
    vec3 composite = (see_through != 0) ? stacked / max(1.0 - clear, 1.0e-4) :
                                         stacked + clear * band_color[0];

    // A terrace is a column seen through; a wall is the layers seen edge on. Everything between is
    // between.
    float up     = clamp(abs(world_normal.z), 0.0, 1.0);
    vec3  albedo = mix(own, composite, smoothstep(0.35, 0.8, up));

    // How much of this fragment the plate stops, which is the alpha the blender wants.
    //
    // A wall is opaque whatever its column says. This shader knows the depth of filament along the
    // stacking axis and nothing whatever about how far the print reaches sideways, and the side of a
    // plate has the body of the print behind it - so the one case it could guess at is the one case the
    // guess would be wrong in.
    float opacity = (see_through != 0) ? mix(1.0, 1.0 - clear, smoothstep(0.35, 0.8, up)) : 1.0;

    // Lateral scattering, on the picture's own surface only: a wall is the cut side of the layers and
    // shows its band's colour, with no picture on it to diffuse. Added as the difference the
    // neighbourhood makes, so the walk's own level and its grazing-angle darkening both survive.
    if (scatter_enabled != 0) {
        // Sampled before the picture's bounds are tested rather than inside the test, for the same
        // reason the error overlay is: a fetch under non-uniform control flow has no defined
        // derivatives, and off the picture there is nothing solved to diffuse anyway.
        vec2 suv = picture_uv();
        vec3 centre;
        vec3 soft = scatter_at(clamp(suv, vec2(0.0), vec2(1.0)), centre);
        float on  = (all(greaterThanEqual(suv, vec2(0.0))) && all(lessThanEqual(suv, vec2(1.0)))) ? 1.0 : 0.0;
        albedo   += (soft - centre) * scatter_strength * smoothstep(0.35, 0.8, up) * on;
        albedo    = max(albedo, vec3(0.0));
    }

    // Layer lines, on the walls only: a terrace is one layer's top surface and has none to show.
    float wall   = 1.0 - up;
    float phase  = fract(layers);
    float groove = smoothstep(0.0, 0.16, phase) * smoothstep(1.0, 0.84, phase);
    groove       = mix(groove, 0.65, clamp(px * 1.5, 0.0, 1.0));
    float lines  = mix(1.0, 0.84 + 0.16 * groove, wall * 0.9);
    albedo      *= lines;

    // Diffuse, with a wrapped terminator: light entering translucent filament comes back out a little
    // way round the curve, so the shaded side never goes as dark as an opaque surface's would.
    float wrap    = 0.25 + 0.45 * translucency;
    float diffuse = max((dot(N, LIGHT_TOP_DIR) + wrap) / (1.0 + wrap), 0.0) * LIGHT_TOP_DIFFUSE;
    diffuse      += max((dot(N, LIGHT_FRONT_DIR) + wrap) / (1.0 + wrap), 0.0) * LIGHT_FRONT_DIFFUSE;

    // Specular, strongest along a layer line, which is what makes a printed wall glint in stripes.
    float spec = LIGHT_TOP_SPECULAR * pow(max(dot(V, reflect(-LIGHT_TOP_DIR, N)), 0.0), LIGHT_TOP_SHININESS);
    spec      *= mix(1.0, 0.35 + 0.65 * groove, wall * 0.9);

    // Scattering out of the edges: a translucent part lights up where it is seen at a grazing angle.
    float rim = pow(1.0 - clamp(dot(N, V), 0.0, 1.0), 4.0);

    vec3 color = albedo * (INTENSITY_AMBIENT + diffuse) + vec3(spec) + albedo * rim * translucency * 0.55;

    // Even a clear sheet shows the light on its own face. One alpha has to carry both the filament's
    // transmission and that reflection, so it never falls below what the surface itself sends back: a
    // fully clear fragment keeps its glint and its lit edge instead of vanishing along with the plastic.
    if (see_through != 0)
        opacity = clamp(max(opacity, spec + rim * translucency * 0.55), 0.0, 1.0);

    // Back lit: none of that light is in the picture. A lithophane shows the LED through the column,
    // so the transmittance walked above *is* the image, and thickness is what makes it dark. A wall is
    // edge on to the panel with filament behind it whichever way it faces, so it keeps its own band's
    // colour at the brightness the column lets past, and the layer lines stay: a groove is thinner
    // filament, and thinner is brighter here rather than merely shaded.
    if (lighting_mode != CHROMA_LIT_FRONT) {
        vec3 lit    = mix(own * dot(through, vec3(0.2126, 0.7152, 0.0722)), through, smoothstep(0.35, 0.8, up));
        vec3 behind = lit * lines * led_color * led_intensity;
        // Both lit, the reflected light is still there and the transmitted light is added on top of it,
        // in the same proportion chroma_palette() blends the two palettes with - so the preview and the
        // colours the solve was chosen against stay the same picture.
        color = (lighting_mode == CHROMA_LIT_BACK) ? behind :
                                                     mix(color, behind, clamp(lighting_balance, 0.0, 1.0));
        // Behind a back-lit plate is the panel, and what the panel sends through the plate has just
        // been drawn. Letting the scene show through as well would be that same light counted twice,
        // with the thinnest and so brightest part of a lithophane fading into the background.
        opacity = 1.0;
    }

    // The error overlay replaces the colour outright, in either lighting mode: how far the stack
    // landed from the picture is a different question from what the picture looks like, and a user
    // reading one must not be reading the other at the same time. The shading is kept so the shape
    // still reads, and the specular with it, since a flat wash of ramp over a curved surface tells
    // the eye nothing about where on the model it is looking.
    //
    // Sampled before the test below rather than inside it: the test is per fragment, and a texture
    // fetch under non-uniform control flow has no defined derivatives to pick a mip level with.
    if (error_enabled != 0) {
        vec2  uv = picture_uv();
        float e  = texture(error_tex, uv).r;
        // Off the picture nothing was solved, so nothing there can be wrong: that surface keeps its
        // predicted colour, which is also what says where the picture ends.
        float on_picture = (all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)))) ? 1.0 : 0.0;
        float shade      = mix(ERROR_SHADE_FLOOR, 1.0, clamp(INTENSITY_AMBIENT + diffuse, 0.0, 1.0));
        color            = mix(color, error_ramp(e) * (shade * lines) + vec3(spec), on_picture);
        // The overlay is a reading rather than a picture of the print, so it has to be legible over
        // whatever the plate happens to be standing in front of.
        opacity          = mix(opacity, 1.0, on_picture);
    }

    // A debug view replaces the colour outright and keeps only a trace of the shading, so the shape
    // still reads. Last of everything, so that whichever overlay was on, this is what is seen.
    if (chroma_debug != CHROMA_DBG_OFF) {
        float shade = mix(0.72, 1.0, clamp(INTENSITY_AMBIENT + diffuse, 0.0, 1.0));
        vec3  dbg   = vec3(1.0, 0.0, 1.0);
        if (chroma_debug == CHROMA_DBG_HEIGHT) {
            // How far up the stack this fragment sits, as the shader measures it: 0 at the base and 1
            // at the top, through the ramp. A plate whose relief never made it into the mesh is one
            // flat colour here, which is the first thing to rule out.
            dbg = debug_ramp(here / max(total_layers, 1.0));
        } else if (chroma_debug == CHROMA_DBG_BAND) {
            // Which band the fragment's own layer belongs to - the filament the wall would show. The
            // terraces of the picture should be visible as flat fields with hard edges between them.
            dbg = debug_index_color(band_hit);
        } else if (chroma_debug == CHROMA_DBG_FACING) {
            // Green where the shader reads a terrace and composites the column through it, red where
            // it reads a wall and shows that band's own colour. The gradient between the two is the
            // mix it really uses, not a threshold drawn for the picture.
            dbg = mix(vec3(0.90, 0.15, 0.10), vec3(0.15, 0.85, 0.25), smoothstep(0.35, 0.8, up));
        } else if (chroma_debug == CHROMA_DBG_UV) {
            // Where the picture lands on the model, as its own uv. Black off the picture, so the edge
            // of the black is the edge of the picture.
            vec2  uv = picture_uv();
            float on = (all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)))) ? 1.0 : 0.0;
            dbg      = vec3(fract(uv.x), fract(uv.y), 0.35) * on;
        } else if (chroma_debug == CHROMA_DBG_SOLVE) {
            // What the solve predicted, straight out of its own image: the panel's "What prints"
            // thumbnail, laid on the model. Where this and the default view disagree, the walk and
            // the solve disagree, and the geometry between them is what to look at.
            vec2  uv = picture_uv();
            float on = (all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)))) ? 1.0 : 0.0;
            dbg      = texture(scatter_tex, clamp(uv, vec2(0.0), vec2(1.0))).rgb * on;
        } else {
            // The walk's own answer with no light on it at all.
            dbg = albedo;
        }
        color = dbg * shade;
        // Same reason as the error overlay: a debug view is read, not looked through.
        opacity = 1.0;
    }

    out_color = vec4(color, alpha * opacity);
}
