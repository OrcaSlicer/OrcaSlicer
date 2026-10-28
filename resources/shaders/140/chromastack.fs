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
// 1 when the plate prints picture down, 0 otherwise.
//
// Face down the picture's face is the plate's *bottom* and the opaque base is on top, so the filament
// a fragment is seen through is everything ABOVE it rather than everything below. The walk itself does
// not change - the base is still composited first and every band in front of it over that - only the
// depth each fragment is at, which is measured from the other end of the stack. Everything else in
// this shader, the walls and the layer lines included, follows from that one number.
uniform int   stack_flipped;

uniform float base_z;        // world Z of the bottom of the object
uniform float layer_height;  // mm
// World +Z in eye space: the direction the stack is built along, which is what the view ray's path
// through it is measured against.
uniform vec3  stack_axis;
// How translucent the stack reads overall, 0 to 1, from the filament on top. Drives the wrap and the
// rim; an opaque stack should not glow.
uniform float translucency;
uniform float alpha;

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

void main()
{
    if (any(lessThan(clipping_planes_dots, vec3(0.0))))
        discard;

    float z_rel  = world_z - base_z;
    float layers = max(z_rel / layer_height, 0.0);
    // Measured from the top down when the picture is on the underside. A uniform decides it, so every
    // fragment of a quad takes the same branch and fwidth() below still has its derivatives.
    if (stack_flipped != 0)
        layers = max(total_layers - layers, 0.0);
    // How much of a layer one pixel covers. Past half a layer the height is taken as continuous, so a
    // surface too far away to resolve its layers averages them instead of flickering between them.
    float px      = fwidth(layers);
    float stepped = clamp(ceil(layers - 1.0e-4), 1.0, total_layers);
    float here    = mix(stepped, clamp(layers, 0.0, total_layers), clamp(px - 0.5, 0.0, 1.0));

    vec3 N = normalize(eye_normal);
    vec3 V = normalize(-eye_position);

    // The view ray's path through one layer, in layers. Straight down it is 1; at 60 degrees off the
    // stacking axis it is 2. Clamped because a ray along the layers passes through no stack at all,
    // and that case is the wall below rather than an infinite path.
    float facing = clamp(abs(dot(normalize(stack_axis), V)), 0.0, 1.0);
    float path   = 1.0 / max(facing, 0.25);

    // Walk the bands from the bottom, stopping at the layer this fragment sits at. `own` is the
    // filament of the band that layer belongs to, which is what the side of the print shows.
    vec3  composite = band_color[0];
    vec3  own       = band_color[0];
    vec3  through   = vec3(1.0);
    float consumed  = 0.0;
    for (int i = 0; i < CHROMA_MAX_BANDS; ++i) {
        if (i >= band_count)
            break;
        float n = clamp(here - consumed, 0.0, band_layers[i]);
        if (n <= 0.0)
            break;
        float t   = exp(-band_density[i] * path * n);
        composite = (1.0 - t) * band_color[i] + t * composite;
        // Back lit, nothing is composited over anything: what leaves the band is what it passed, so
        // the transmittances multiply. The same density and the same lengthened path - a ray crossing
        // the stack at an angle goes through more filament whichever side the light is on - with the
        // absorption split per channel by the band's colour. Behind the one branch a uniform decides,
        // so the front view pays nothing for it.
        if (lighting_mode != CHROMA_LIT_FRONT)
            through *= exp(-band_density[i] * path * n *
                           mix(vec3(CHROMA_BACK_ABSORB_FLOOR), vec3(1.0), 1.0 - band_color[i]));
        own       = band_color[i];
        consumed += band_layers[i];
    }

    // A terrace is a column seen through; a wall is the layers seen edge on. Everything between is
    // between.
    float up     = clamp(abs(world_normal.z), 0.0, 1.0);
    vec3  albedo = mix(own, composite, smoothstep(0.35, 0.8, up));

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
    }

    out_color = vec4(color, alpha);
}
