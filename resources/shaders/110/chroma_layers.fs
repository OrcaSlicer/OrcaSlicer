#version 110

// A ChromaStack drawn as the stack of translucent sheets it is printed as.
//
// The other chroma pass (chromastack.fs) composites the whole stack analytically, at one point per
// fragment, onto the surface of an opaque mesh. That is exact for the point it is asked at, and it
// carries the walls and the layer lines. Two things it cannot do, and both are what a print looks
// like rather than what colour it is:
//
//  - it has no geometry per layer, so the layers cannot be seen apart, cannot slide against each
//    other as the view turns, and a swap bought one layer too early reads as a colour rather than as
//    a layer;
//  - it reads its height off the *mesh*, which is a few hundred cells across where the picture is a
//    thousand texels, so the finest thing it can show is a mesh cell. That is why the surface view of
//    a plate looks coarser than the solve's own prediction of it.
//
// This pass is the print as it is built, at the picture's own resolution. One quad per printed layer,
// over the picture's rectangle, at that layer's own height in world space - which is where a printed
// layer really is, since the printer stacks them along world Z whatever the plate's frame does. Each
// quad is cut back to the texels whose column reaches it, tinted with its band's filament, and the
// blender puts the lot together in the order the light crosses them.
//
// So the compositing is not an approximation of the walk. "Over" applied back to front with one sheet
// per layer is the recursion chromastack.fs runs per band, carried out by the blender over real
// geometry rather than in one fragment, and the two agree on a column seen straight down.
//
// ---- Nothing here is lit, and that is the point ----
//
// A sheet is its filament's own colour at the alpha that one printed layer of it stops, and nothing
// else. What makes the picture is that there are two dozen of them and their transmittances have
// multiplied: a translucent filament stops around five per cent in a 0.08 mm layer, so one sheet is
// very nearly invisible and the colour only arrives once the stack has accumulated.
//
// An earlier version of this shader shaded every sheet - a diffuse term off the height map's
// gradient, a specular glint, a rim light - and it was wrong, not merely heavy. Shading decides the
// colour of a surface from where the light is; a HueForge decides it from how much plastic the light
// crossed. Put the two together and the shading wins, because it varies over the whole sheet while
// the accumulation varies only with the picture, and the result reads as one flat lit slab with a
// pattern on it. The only geometric term left is `path`, which is not light but length: a ray that
// enters at a grazing angle crosses more of the layer, so it stops more of it.
//
// Where the stack is thin it lets the scene behind through, for the same reason and with no flag
// asking for it: few sheets, little stopped. The backing is what hides the scene on a real print,
// and it is drawn here only when the viewer asks for it.

// How little of its own colour a filament still absorbs. Without a floor a pure red would pass red
// untouched however thick it was, and a lithophane of it would have no picture in its own hue.
// chromastack.fs uses the same number for the same reason.
#define CHROMA_BACK_ABSORB_FLOOR 0.35

// Which half of the light this draw is for.
//
// REFLECT is the light that fell on the print and came back out, composited over whatever is behind
// the plate with ordinary "over" blending. TRANSMIT is the light that went through it, and it
// multiplies the frame buffer instead: a sheet's own transmittance per channel, applied to the lamp
// quad already drawn behind the stack, which is exactly what a lithophane in front of a light box
// does. Both lit draws the second and then the first.
#define CHROMA_PASS_REFLECT  0
#define CHROMA_PASS_TRANSMIT 1
uniform int pass_mode;

// Where the picture lands on the plate, as the closed form the surface pass already uses: the picture
// is pinned to the XY plane, so its uv is a function of the fragment's world position and the mesh
// carries no texture coordinate. The sheet's own quad is built from the inverse of this, so the two
// views line up texel for texel.
uniform vec3  tex_anchor;
uniform float tiling_scale;  // mm per uv unit along u
uniform float tex_aspect;    // picture width / height; v is scaled by it, so v is mm / (scale/aspect)
uniform float rotation_rad;
uniform vec2  uv_offset;

// The solve's height map at its own resolution: how far up the stack each texel's column reaches, 0
// at the base and 1 at the top of the relief. The map the displacement is driven by, so a sheet is
// cut back to exactly the texels the print's relief covers at that height.
uniform sampler2D level_tex;

uniform float relief_layers;     // the relief's depth in layers, what level_tex's 0..1 measures
uniform float sheet_index;       // which layer this sheet is, counted from the build plate
// Face down the picture is the plate's underside, so the stored map is the complement of the column's
// height (chroma_invert_relief()), and a texel's column covers the layers *above* its face rather
// than the ones below it.
uniform int   stack_flipped;

uniform vec3  sheet_color;
// What this sheet absorbs along the stacking axis: chroma_layer_density() for one layer, times as
// many layers as the sheet stands for.
uniform float sheet_density;
// Whether the sheet covers every texel of the picture rather than being cut by the height map. The
// backing is drawn this way, as one sheet carrying its whole thickness rather than its own hundreds:
// it is uncut by construction, and at any real base thickness it stops the light anyway, so cutting
// it up would draw hundreds of hidden quads to reach the same answer.
uniform int   sheet_full;
// The light box behind a back-lit plate. Drawn before the stack and never cut by the height map: it
// is the lamp, not a layer.
uniform int   sheet_lamp;
uniform vec3  led_color;
uniform float led_intensity;

// Scales what the reflected pass contributes, for a plate lit from both sides: the transmitted pass
// has already been laid down underneath it.
uniform float reflect_strength;
uniform float alpha;

varying vec3  clipping_planes_dots;
varying vec3  eye_normal;
varying vec3  eye_position;
varying vec2  world_xy;

vec2 picture_uv()
{
    vec2 planar = vec2(world_xy.x - tex_anchor.x, -(world_xy.y - tex_anchor.y));
    planar     *= (tiling_scale > 1.0e-6) ? (1.0 / tiling_scale) : 1.0;
    float cs    = cos(rotation_rad);
    float sn    = sin(rotation_rad);
    vec2  uv    = vec2(planar.x * cs - planar.y * sn, planar.x * sn + planar.y * cs);
    uv.y       *= tex_aspect;
    return uv + uv_offset;
}

void main()
{
    if (any(lessThan(clipping_planes_dots, vec3(0.0))))
        discard;

    vec2 uv = picture_uv();
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
        discard;

    // The lamp behind the plate, before any of the stack is drawn. It carries no picture and takes no
    // light: everything after it multiplies what it put down.
    if (sheet_lamp != 0) {
        gl_FragColor = vec4(led_color * max(led_intensity, 0.0), 1.0);
        return;
    }

    // How much of this sheet the column covers, in [0, 1].
    //
    // The fractional part is not a smoothing hack. The map is sampled bilinearly, so between two
    // texels that print different numbers of layers it reads a height genuinely part way between them,
    // and a sheet half covered is the honest drawing of a terrace edge falling inside a texel. It is
    // also what keeps the steps from crawling as the view moves.
    float reach = texture2D(level_tex, uv).r * relief_layers;
    float cover = 1.0;
    if (sheet_full == 0)
        cover = (stack_flipped == 0) ? clamp(reach - sheet_index, 0.0, 1.0) :
                                       clamp(sheet_index + 1.0 - reach, 0.0, 1.0);
    if (cover <= 0.002)
        discard;

    // The view ray's path through one layer, in layers: straight on it is 1, and at a grazing angle it
    // is longer, which is why a print deepens as it is tilted. Clamped because a ray running along the
    // sheet crosses an unbounded amount of it and would otherwise turn the silhouette into a hard line.
    // The sheet is flat, so its own normal is the quad's and needs nothing from the height map.
    vec3  V      = normalize(-eye_position);
    vec3  flat_n = normalize(eye_normal);
    float path   = 1.0 / max(abs(dot(flat_n, V)), 0.2);

    // What this sheet stops along that ray. The backing is no special case: a base of any real
    // thickness comes out of this at 0.999 and over, which is the opacity the model gives it rather
    // than one asserted here, and a stack with no base at all correctly lets the scene through.
    float stop = 1.0 - exp(-sheet_density * path * cover);

    if (pass_mode == CHROMA_PASS_TRANSMIT) {
        // Multiplied into the lamp's light rather than composited over it. A filament absorbs least in
        // its own colour, which is what makes a lithophane of a red filament red where it is thick -
        // the same per-channel absorption the walk uses, with the same floor so a saturated filament
        // still carries a picture.
        vec3 absorb = mix(vec3(CHROMA_BACK_ABSORB_FLOOR), vec3(1.0), 1.0 - sheet_color);
        gl_FragColor    = vec4(exp(-sheet_density * path * cover * absorb), 1.0);
        return;
    }

    // The filament's own colour, at the alpha this one layer of it stops. The blender does the rest.
    gl_FragColor = vec4(sheet_color, alpha * reflect_strength * stop);
}
