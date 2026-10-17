#version 110

// What a ChromaStack print looks like, shaded per fragment.
//
// The colour is not a vertex attribute and not a texture on the surface: it is read from the
// fragment's own height. A filament swap happens at one print Z across the whole plate, so the layer
// a point sits in is what decides its colour - which means a band edge lands exactly on the layer
// boundary however coarse the mesh is, and the same shader is right before a bake (on the preview's
// displaced mesh) and after one (on the volume's own).
//
// On top of that it tries to look like printed plastic rather than like flat shaded colour: the
// terminator is wrapped and the grazing angles glow, because filament translucent enough to be worth
// stacking scatters light rather than reflecting it; and the walls carry layer lines, because that is
// the first thing the eye uses to read a print as a print.

#define INTENSITY_CORRECTION 0.6
const vec3 LIGHT_TOP_DIR = vec3(-0.4574957, 0.4574957, 0.7624929);
#define LIGHT_TOP_DIFFUSE    (0.8 * INTENSITY_CORRECTION)
#define LIGHT_TOP_SPECULAR   (0.125 * INTENSITY_CORRECTION)
#define LIGHT_TOP_SHININESS  20.0
const vec3 LIGHT_FRONT_DIR = vec3(0.6985074, 0.1397015, 0.6985074);
#define LIGHT_FRONT_DIFFUSE  (0.3 * INTENSITY_CORRECTION)
#define INTENSITY_AMBIENT    0.3

// The stack's colours, one texel per printed layer, bottom to top. A texture rather than a uniform
// array because a base can be several hundred layers and no uniform array survives that.
uniform sampler2D palette;
uniform float     palette_size;  // texels in `palette`, i.e. the stack's total printed layers
uniform float     base_z;        // world Z of the bottom of the object
uniform float     layer_height;  // mm
// How translucent the stack reads, 0 to 1, from the transmission distance of the filament on top.
// It drives the wrap and the rim: an opaque stack should not glow.
uniform float     translucency;
uniform float     alpha;

varying vec3  clipping_planes_dots;
varying vec3  eye_normal;
varying vec3  eye_position;
varying vec3  world_normal;
varying float world_z;

void main()
{
    if (any(lessThan(clipping_planes_dots, vec3(0.0))))
        discard;

    float z_rel  = world_z - base_z;
    float layers = max(z_rel / layer_height, 0.0);
    // How much of a layer one pixel covers. Everything below is faded out as this grows: layer lines
    // finer than a pixel are noise, and a colour read from one layer out of the dozen under a pixel
    // flickers as the camera moves.
    float px     = fwidth(layers);

    // The layer this fragment sits in, 1-based. Up close that is an exact texel of the palette; once a
    // pixel spans a layer or more it slides to a continuous coordinate so the linear filter averages
    // the bands it covers instead of picking one of them at random.
    float stepped = clamp(ceil(layers - 1.0e-4), 1.0, palette_size) - 0.5;
    float smooth_ = clamp(layers, 0.5, palette_size - 0.5);
    float u       = mix(stepped, smooth_, clamp(px - 0.5, 0.0, 1.0)) / palette_size;
    vec3  albedo  = texture2D(palette, vec2(u, 0.5)).rgb;

    vec3 N = normalize(eye_normal);
    vec3 V = normalize(-eye_position);

    // Layer lines, on the walls only: a terrace is one layer's top surface and has none to show. The
    // groove runs round the part at every layer boundary, so it is a function of height alone.
    float wall   = 1.0 - abs(world_normal.z);
    float phase  = fract(layers);
    float groove = smoothstep(0.0, 0.16, phase) * smoothstep(1.0, 0.84, phase);
    groove       = mix(groove, 0.65, clamp(px * 1.5, 0.0, 1.0)); // fade to flat when too fine to resolve
    float lines  = mix(1.0, 0.84 + 0.16 * groove, wall * 0.9);
    albedo      *= lines;

    // Diffuse, with a wrapped terminator: light entering translucent filament comes back out a little
    // way round the curve, so the shaded side never goes as dark as an opaque surface's would.
    float wrap    = 0.25 + 0.45 * translucency;
    float ndotl   = dot(N, LIGHT_TOP_DIR);
    float diffuse = max((ndotl + wrap) / (1.0 + wrap), 0.0) * LIGHT_TOP_DIFFUSE;
    diffuse      += max((dot(N, LIGHT_FRONT_DIR) + wrap) / (1.0 + wrap), 0.0) * LIGHT_FRONT_DIFFUSE;

    // Specular, strongest along a layer line, which is what makes a printed wall glint in stripes
    // rather than evenly.
    float spec = LIGHT_TOP_SPECULAR * pow(max(dot(V, reflect(-LIGHT_TOP_DIR, N)), 0.0), LIGHT_TOP_SHININESS);
    spec      *= mix(1.0, 0.35 + 0.65 * groove, wall * 0.9);

    // Scattering out of the edges: a translucent part lights up where it is seen at a grazing angle
    // and where it is thin. Tinted by the filament, because what comes back out has been through it.
    float rim = pow(1.0 - clamp(dot(N, V), 0.0, 1.0), 4.0);

    vec3 color = albedo * (INTENSITY_AMBIENT + diffuse) + vec3(spec) + albedo * rim * translucency * 0.55;
    gl_FragColor = vec4(color, alpha);
}
