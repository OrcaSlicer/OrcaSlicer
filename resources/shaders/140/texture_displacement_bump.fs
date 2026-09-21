#version 140

// Fast, geometry-free preview of texture displacement: perturbs the *shading* normal from the
// height texture's local gradient (a bump map), faded out by the per-vertex paint weight. The
// true, exact result is what "Bake" produces via libslic3r/TextureDisplacement.cpp on the CPU.
//
// The bake displaces each surface point along its normal by H = +/- depth_mm * (h(uv) - midlevel),
// with uv from the layer's projection. The perturbed normal is the analytic
//
//     N' = normalize(N - (dH/da) * T - (dH/db) * B)
//
// over any orthonormal surface tangent pair (T, B), where the two slopes are real mm-per-mm
// derivatives. Two things have to be right for the preview's apparent depth to match the bake's:
// the tangent frame the gradient is expressed in, and the uv->mm scale that turns a texel
// difference into a slope. Getting the scale wrong is a uniform flattening (a raw texel difference
// is dh over one texel step, not over one mm); getting the frame wrong tilts the bump along the
// wrong axes.
//
// Two projection paths:
//   * Triplanar (use_vertex_uv = 0): uv and the tangent axes are derived in-shader from the dominant
//     normal axis, mirroring libslic3r's project_planar()/apply_uv_transform(), and the slope is
//     formed analytically (there is a closed-form uv, so 1 uv unit is exactly tiling_scale mm). This
//     path also runs a parallax step before shading, see below.
//
// Parallax. A pure bump map perturbs shading only, so the pattern is welded to the base surface: it
// does not shift as the camera orbits and it does not get any deeper as depth_mm grows, which is
// exactly when the preview stops reading as real geometry. The triplanar path therefore shades at the
// point the *displaced* surface would show at this pixel rather than at the pixel's own base position.
//
// Two cheaper formulations were tried first and both are wrong here, which is worth recording:
//   * Solving Q = P + V * (H(Q) / dot(V, n)) by fixed-point iteration. Geometrically exact, but the
//     divisor goes to zero edge-on, and an unbounded step is not a small error - the sample lands a
//     large fraction of a tile away and the iteration oscillates instead of converging. It reads as a
//     *second, flat copy* of the pattern ghosted over the real one. Clamping the step to one tile does
//     not help either: a tile-sized shift lands on the neighbouring tile, which is the same pattern.
//   * Offset limiting (Welsh): step along the tangential part of V, whose length caps the shift at one
//     depth. Stable and cheap, but it understates parallax by exactly the factor that matters - the
//     relief still flattens as soon as the camera tilts, which is the complaint it was meant to fix.
//
// So this ray-marches instead (parallax occlusion mapping). A point at ray parameter s, i.e. P + V * s,
// sits at height s * dot(V, n) above the undisplaced surface. The displaced surface lives in a shell
// between the extreme values of amp * (h - midlevel); the march starts at the top of that shell, where
// the ray is outside the surface by construction, and steps inward until the ray height falls below the
// sampled height. That crossing *is* the visible point - no divergence, no ghosting, and parallax stays
// correct at any angle. The hit is interpolated between the last two samples, which is what keeps
// PARALLAX_STEPS low enough to afford. The march is skipped when sweeping the shell would move the
// sample point less than half a texel (the head-on case), so the common view pays almost nothing.
//
// The gradient/shading below is evaluated at the resulting uv, so the relief both slides correctly
// under camera motion and visibly deepens with depth_mm. What it still cannot do is change the
// model's silhouette or cast shadows; for that, switch the View row to Normal.
//   * Precomputed uv (use_vertex_uv = 1, used for LSCM): uv comes per-vertex from the CPU (the LSCM
//     unwrap with island placement + tiling/rotation/offset already folded in), and the perturbed
//     normal is built with Mikkelsen's method -- the surface gradient taken straight from the
//     screen-space derivatives of the sampled height and position. This makes no uv->mm scale
//     assumption, which matters because an LSCM map is conformal, not isometric: the local mm-per-uv
//     varies across the chart, so a single global 1/tiling factor (what an earlier version used) got
//     the apparent depth wrong. This path is also what makes the fast preview follow the UV editor:
//     move an island and its uv -- hence its bump -- moves with it.

#define INTENSITY_CORRECTION 0.6

#define PARALLAX_STEPS 24
// Explicit LOD: the march samples inside non-uniform control flow, where implicit
// derivatives are undefined.
#define H_AT(uv) textureLod(height_tex, uv, 0.0).r

// normalized values for (-0.6/1.31, 0.6/1.31, 1./1.31)
const vec3 LIGHT_TOP_DIR = vec3(-0.4574957, 0.4574957, 0.7624929);
#define LIGHT_TOP_DIFFUSE    (0.8 * INTENSITY_CORRECTION)
#define LIGHT_TOP_SPECULAR   (0.125 * INTENSITY_CORRECTION)
#define LIGHT_TOP_SHININESS  20.0

// normalized values for (1./1.43, 0.2/1.43, 1./1.43)
const vec3 LIGHT_FRONT_DIR = vec3(0.6985074, 0.1397015, 0.6985074);
#define LIGHT_FRONT_DIFFUSE  (0.3 * INTENSITY_CORRECTION)

#define INTENSITY_AMBIENT    0.3

const vec3 ZERO = vec3(0.0, 0.0, 0.0);

uniform vec4 uniform_color;
// The printable palette, in **CIELAB** as well as RGB, and how many entries are real. Lab because the
// match has to be perceptual - the same reason the CPU side uses CIEDE2000 - and converting the
// palette once on the CPU is what lets the fragment shader match with a plain squared distance.
// Count 0 means nothing is colouring, and every fragment falls back to uniform_color as before.
uniform vec3      palette_lab[64];
uniform vec3      palette_rgb[64];
uniform int       palette_count;
uniform bool      pure_only;      // match against single filaments only (flat-colour image)
// How each entry prints. A pure entry is one filament (a == b); a mix interleaves filaments a and b,
// num parts of a in every den, and the print shows that interleave rather than the entry's average
// colour. The fragment resolves it exactly as GLGizmoTextureDisplacement::make_mix_resolver() does
// per triangle on the CPU, so the preview shows the pattern the bake will print.
uniform int       palette_a[64];
uniform int       palette_b[64];
uniform int       palette_num[64];
uniform int       palette_den[64];
uniform vec3      filament_rgb[16];
uniform int       filament_count;
uniform int       mix_mode;     // ColorMixMode: 0 Z bands, 1 XY dither, 2 auto
uniform float     layer_height; // mm; one Z band per print layer
uniform float     dither_cell;  // mm; one XY dither cell
uniform sampler2D color_tex;     // the layer's colour image, sampled at the same uv as the height
uniform bool      has_color_tex;
uniform bool volume_mirrored;

uniform mat4 view_model_matrix;
uniform mat3 view_normal_matrix;

uniform sampler2D height_tex;
uniform vec2      height_tex_texel; // (1/width, 1/height) of height_tex
uniform float      depth_mm;
uniform float      tiling_scale;
// Height map width / height. Scales the v axis so a non-square image keeps its proportions
// instead of being squeezed into a square tile - mirrors libslic3r's apply_uv_transform().
uniform float      tex_aspect;
uniform float      rotation_rad;
uniform vec2       uv_offset;
uniform bool       invert;
uniform float      midlevel;      // the height that means "don't move"; needed by the parallax step
uniform vec3       eye_model_pos; // camera position in the texture frame (world minus tex_anchor)
uniform vec3       tex_anchor;    // the volume's origin in world space: the texture frame's origin
uniform bool       use_vertex_uv; // true: sample at vertex_uv with a derived tangent frame (LSCM)
// A 2x3 affine (columns packed as lin = (m00, m01, m10, m11), tr = (m02, m12)) applied to the uv of
// the island currently being dragged in the UV editor (island_active > 0.5). Identity when nothing is
// dragged, so this whole path is a no-op then. Lets a UV island drag move the bump on the model with
// only a uniform update 
uniform vec4       island_delta_lin;
uniform vec2       island_delta_tr;
// Which projection to reconstruct in-shader: 0 Triplanar, 1 Cylindrical, 2 Spherical (the low three
// TextureProjectionMethod values; LSCM and ViewProjected arrive through use_vertex_uv and leave this
// at 0). The two wrapping projections wrap around the *whole painted patch*, so its centroid - and,
// for Cylindrical, its axis - are properties no single fragment can derive. They come from the CPU,
// in this same texture frame, computed with the bake's own texture_displacement_patch_frame().
uniform int        projection_mode;
uniform vec3       patch_center;
uniform vec3       patch_axis;

in vec3  clipping_planes_dots;
in vec4  model_pos;
in vec4  world_pos;
in float weight;
in float island_active;
in vec2  vertex_uv;

out vec4 out_color;

// The cylinder's own frame, built exactly as libslic3r's project_cylindrical() builds it - including
// the handedness, which comes out left-handed for an axis of +Z. Copied rather than "corrected", so
// the preview wraps the texture the same way round as the bake.
void cylinder_frame(out vec3 up, out vec3 right, out vec3 fwd)
{
    up = (length(patch_axis) > 1e-8) ? normalize(patch_axis) : vec3(0.0, 0.0, 1.0);
    vec3 arbitrary = (abs(up.z) < 0.9) ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    right = normalize(cross(up, arbitrary));
    fwd   = normalize(cross(right, up));
}

// The raw, millimetre-valued projection of `p` (a position in the texture frame), before the layer's
// tiling/rotation/aspect/offset - a term-for-term transcription of libslic3r's project_planar(),
// project_cylindrical() and project_spherical(). `n` is read by the planar mode only.
vec2 projection_raw(vec3 p, vec3 n)
{
    if (projection_mode == 1) {                 // Cylindrical: (arc length around, distance along)
        vec3  up, right, fwd;
        cylinder_frame(up, right, fwd);
        vec3  rel = p - patch_center;
        float x   = dot(rel, right);
        float y   = dot(rel, fwd);
        return vec2(atan(y, x) * sqrt(x * x + y * y), dot(rel, up));
    }
    if (projection_mode == 2) {                 // Spherical: (longitude, latitude) * radius
        vec3  rel    = p - patch_center;
        float radius = length(rel);
        if (radius < 1e-8)
            return vec2(0.0);
        vec3 dir = rel / radius;
        return vec2(atan(dir.y, dir.x), asin(clamp(dir.z, -1.0, 1.0))) * radius;
    }
    vec3 an = abs(n);                           // Triplanar: drop the dominant normal axis
    return (an.x >= an.y && an.x >= an.z) ? p.yz : ((an.y >= an.x && an.y >= an.z) ? p.xz : p.xy);
}

// The two surface directions projection_raw()'s u and v run along at `p`, plus how many raw units one
// millimetre of travel along each of them covers - the factor that turns the uv-space height gradient
// into a real mm-per-mm slope. For the planar projection both axes are world axes and the factor is 1.
// Cylindrical and Spherical are arc-length parametrized, so it is 1 there too, except for the
// spherical longitude, whose circle shrinks by cos(latitude) toward the poles. Exact where the surface
// really is the cylinder/sphere the projection assumes - the same assumption libslic3r makes.
void projection_axes(vec3 p, vec3 n, out vec3 t, out vec3 b, out vec2 units_per_mm)
{
    units_per_mm = vec2(1.0, 1.0);
    if (projection_mode == 1) {
        vec3 up, right, fwd;
        cylinder_frame(up, right, fwd);
        vec3  rel = p - patch_center;
        vec2  xy  = vec2(dot(rel, right), dot(rel, fwd));
        float r   = length(xy);
        t = (r > 1e-6) ? (fwd * xy.x - right * xy.y) / r : right; // circumferential: u runs along it
        b = up;                                                   // v is the distance along the axis
        return;
    }
    if (projection_mode == 2) {
        vec3  rel = p - patch_center;
        float r   = length(rel);
        vec3  dir = (r > 1e-8) ? rel / r : vec3(0.0, 0.0, 1.0);
        float c   = length(dir.xy);                               // cos(latitude)
        t = (c > 1e-6) ? vec3(-dir.y, dir.x, 0.0) / c : vec3(1.0, 0.0, 0.0);
        b = cross(dir, t);                                        // increasing latitude, unit length
        units_per_mm = vec2(1.0 / max(c, 1e-3), 1.0);
        return;
    }
    vec3 an = abs(n);
    if (an.x >= an.y && an.x >= an.z) {        // planar = p.yz
        t = vec3(0.0, 1.0, 0.0);
        b = vec3(0.0, 0.0, 1.0);
    } else if (an.y >= an.x && an.y >= an.z) { // planar = p.xz
        t = vec3(1.0, 0.0, 0.0);
        b = vec3(0.0, 0.0, 1.0);
    } else {                                   // planar = p.xy
        t = vec3(1.0, 0.0, 0.0);
        b = vec3(0.0, 1.0, 0.0);
    }
}

vec2 project_uv(vec3 p, vec3 n)
{
    vec2 planar = projection_raw(p, n);
    planar *= (tiling_scale > 1e-6) ? (1.0 / tiling_scale) : 1.0;
    float cs = cos(rotation_rad);
    float sn = sin(rotation_rad);
    vec2 r = vec2(planar.x * cs - planar.y * sn, planar.x * sn + planar.y * cs);
    // After the rotation, so the rotation stays a rotation rather than becoming a shear.
    r.y *= tex_aspect;
    return r + uv_offset;
}

// sRGB -> CIELAB, matching slic3r/Utils/ColorSpaceConvert's RGB2Lab so this picks the same entry the
// bake does.
vec3 srgb_to_lab(vec3 c)
{
    vec3 v = vec3(c.r > 0.04045 ? pow((c.r + 0.055) / 1.055, 2.4) : c.r / 12.92,
                  c.g > 0.04045 ? pow((c.g + 0.055) / 1.055, 2.4) : c.g / 12.92,
                  c.b > 0.04045 ? pow((c.b + 0.055) / 1.055, 2.4) : c.b / 12.92);
    vec3 xyz = vec3(dot(v, vec3(0.4124, 0.3576, 0.1805)) / 0.95047,
                    dot(v, vec3(0.2126, 0.7152, 0.0722)),
                    dot(v, vec3(0.0193, 0.1192, 0.9505)) / 1.08883);
    vec3 f   = vec3(xyz.x > 0.008856 ? pow(xyz.x, 1.0 / 3.0) : (7.787 * xyz.x) + 16.0 / 116.0,
                    xyz.y > 0.008856 ? pow(xyz.y, 1.0 / 3.0) : (7.787 * xyz.y) + 16.0 / 116.0,
                    xyz.z > 0.008856 ? pow(xyz.z, 1.0 / 3.0) : (7.787 * xyz.z) + 16.0 / 116.0);
    return vec3(116.0 * f.y - 16.0, 500.0 * (f.x - f.y), 200.0 * (f.y - f.z));
}

// Nearest printable colour to a sampled one. Quantizing per *fragment* rather than per facet is the
// whole point of this path: it shows the image at the texture's resolution instead of the mesh's,
// which is what you need while choosing a texture and placing it. The Normal view is where the
// facet-resolution truth - what actually bakes - is shown.
//
// Squared distance in Lab (CIE76) rather than the CPU's CIEDE2000: the two agree except on near-ties,
// and CIEDE2000 per fragment across 64 entries is not worth its cost in a preview.
int nearest_palette_entry(vec3 rgb)
{
    vec3  lab  = srgb_to_lab(rgb);
    int   best = 0;
    int   best_pure = -1;
    float bd   = 1.0e20;
    float bd_pure = 1.0e20;
    for (int i = 0; i < 64; ++i) {
        if (i >= palette_count)
            break;
        if (pure_only && palette_a[i] != palette_b[i])
            continue; // a flat-colour image never takes a mix (see the bake)
        vec3  d  = lab - palette_lab[i];
        float d2 = dot(d, d);
        if (palette_a[i] == palette_b[i] && d2 < bd_pure) {
            bd_pure   = d2;
            best_pure = i;
        }
        if (d2 < bd) {
            bd   = d2;
            best = i;
        }
    }
    // The same bias make_palette_quantizer() applies (PREFER_PURE_DE = 10): a mix is an interleave, so
    // it is only worth taking when it beats the nearest single filament by a visible step. Without it
    // this picked a mix for almost every fragment - with four filaments the palette is 4 pure entries
    // against 30 mixes - while the bake picked a single filament for most of them, so the preview
    // interleaved the whole wall where the bake interleaves only patches. Compared on the distances
    // rather than their squares, so the threshold means the same thing as it does on the CPU (up to
    // CIE76 against CIEDE2000, the approximation already noted above).
    if (best_pure >= 0 && palette_a[best] != palette_b[best] && sqrt(bd_pure) - sqrt(bd) < 10.0)
        best = best_pure;
    return best;
}

// One 2x2 Bayer cell, {0, 2; 3, 1}, for x and y in {0, 1}.
float bayer2(float x, float y) { return 2.0 * x + 3.0 * y - 4.0 * x * y; }

// The colour the printer lays down at world point `pos` for palette entry `index`: its filament, or
// for a mix whichever of its two filaments this point falls on. Mirrors make_mix_resolver() on the
// CPU, floors on the band/cell size included. All the modular arithmetic is done in floats with
// mod(), which wraps negative coordinates the way the CPU's ((v % n) + n) % n does and needs no
// integer % (not available on every GLSL 1.10 target).
vec3 printed_color(int index, vec3 pos, vec3 normal, vec3 footprint)
{
    int a = palette_a[index];
    int b = palette_b[index];
    if (a < 0 || a >= filament_count || b < 0 || b >= filament_count)
        return palette_rgb[index]; // no filament to resolve to: the entry's own colour
    if (a == b)
        return filament_rgb[a];
    float num = float(palette_num[index]);
    float den = float(palette_den[index]);
    // Auto: bands where the surface is steeper than ~45 degrees, the dominant filament elsewhere.
    if (mix_mode == 2 && abs(normal.z) >= 0.7)
        return filament_rgb[(num * 2.0 >= den) ? a : b];

    // Pre-filter. The interleave is an ordered dither the eye is meant to blend away, and no dither
    // blends when it is drawn at less than a few pixels per period - it aliases, which is what turned
    // every upright wall into horizontal streaks: the Z band cycle is den * layer_height (around a
    // millimetre), and every pixel of a row on a vertical wall shares one z, so each row came out as a
    // 1-bit threshold of the image at that row's phase. `footprint` is mm of world position per pixel,
    // so this is zoom- and resolution-correct rather than a tuned constant: where the print's own
    // pattern is finer than this view can resolve, show what the print looks like from here, which is
    // the entry's perceptual average. The Normal view remains where the per-facet truth lives.
    float period = (mix_mode == 1) ? 2.0 * max(dither_cell, 0.01) : den * max(layer_height, 0.01);
    float px     = (mix_mode == 1) ? max(footprint.x, footprint.y) : footprint.z;
    float sharp  = clamp(period / max(4.0 * px, 1e-6) - 0.5, 0.0, 1.0);
    if (sharp <= 0.0)
        return palette_rgb[index];

    vec3 picked;
    if (mix_mode == 1) {
        // Ordered 4x4 Bayer over floor(x / cell), floor(y / cell). The CPU's table
        //     0  8  2 10
        //    12  4 14  6
        //     3 11  1  9
        //    15  7 13  5
        // is 4 * bayer2(x % 2, y % 2) + bayer2(x / 2, y / 2), which needs no array (GLSL 1.10 has
        // no constant arrays).
        float cell  = max(dither_cell, 0.01);
        float gx    = mod(floor(pos.x / cell), 4.0);
        float gy    = mod(floor(pos.y / cell), 4.0);
        float bayer = 4.0 * bayer2(mod(gx, 2.0), mod(gy, 2.0)) + bayer2(floor(gx / 2.0), floor(gy / 2.0));
        picked = filament_rgb[(num / den > (bayer + 0.5) / 16.0) ? a : b];
    } else {
        // Z bands: one per band height, the band's phase in the a/b cycle picks the filament. Both
        // operands are integer-valued, so the half keeps "phase < num" exact under float rounding.
        float slot  = floor(pos.z / max(layer_height, 0.01));
        float phase = mod(slot, den);
        picked = filament_rgb[(phase < num - 0.5) ? a : b];
    }
    return mix(palette_rgb[index], picked, sharp);
}

void main()
{
    if (any(lessThan(clipping_planes_dots, ZERO)))
        discard;

    // Everything below runs in world millimetres, like the bake: a tile is tiling_scale mm on the
    // printed part whatever the instance's scale or rotation, so the preview has to project from the
    // world position and perturb the world normal.
    vec3 triangle_normal = normalize(cross(dFdx(world_pos.xyz), dFdy(world_pos.xyz)));
    vec3 tex_pos = world_pos.xyz - tex_anchor; // the frame the texture is projected in, as the bake does
    // World mm per pixel, for pre-filtering the interleave in printed_color(). Taken here because the
    // albedo branch at the end of main() is non-uniform control flow, where derivatives are undefined.
    vec3 pos_fwidth = fwidth(world_pos.xyz);
    if (volume_mirrored)
        triangle_normal = -triangle_normal;

    // Where the colour is read from. Both branches below already compute the uv this fragment's
    // *height* came from - including the parallax-marched one on the triplanar path - and the colour
    // has to follow it exactly, or the colour would slide off the relief as the camera orbits.
    vec2 color_uv    = vec2(0.0);
    bool have_uv     = false;

    if (use_vertex_uv) {
        // Precomputed-uv (LSCM) path - Mikkelsen's surface-gradient bump ("Bump Mapping
        // Unparametrized Surfaces on the GPU"). The perturbed normal is derived straight from the
        // screen-space derivatives of the *sampled height* and the position, so it is scale-exact
        // with no uv->mm assumption at all - which is the whole point here: an LSCM map is conformal,
        // not isometric, so the local mm-per-uv varies across the chart and the earlier "one global
        // 1/tiling factor" got the depth visibly wrong. dFdx(h) captures the true on-screen rate of
        // change however the chart is stretched or however fine the tiling is.
        //
        // use_vertex_uv is a uniform, so this whole branch is uniform control flow and the texture
        // derivatives are well defined; the paint weight gates the result by a plain multiply (k)
        // rather than a per-fragment branch, keeping it that way.
        // The dragged island's uv rides a uniform affine so its bump moves without a rebuild; every
        // other vertex (island_active == 0) samples its baked uv unchanged.
        vec2 uv = (island_active > 0.5)
                    ? vec2(dot(island_delta_lin.xy, vertex_uv), dot(island_delta_lin.zw, vertex_uv)) + island_delta_tr
                    : vertex_uv;
        color_uv = uv;
        have_uv  = true;
        float h = texture(height_tex, uv).r;
        float k = (invert ? -1.0 : 1.0) * depth_mm * clamp(weight, 0.0, 1.0);
        vec3  sigmaS = dFdx(world_pos.xyz);
        vec3  sigmaT = dFdy(world_pos.xyz);
        vec3  R1 = cross(sigmaT, triangle_normal);
        vec3  R2 = cross(triangle_normal, sigmaS);
        float det = dot(sigmaS, R1);
        float dHdx = k * dFdx(h);
        float dHdy = k * dFdy(h);
        if (abs(det) > 1e-12)
            triangle_normal = normalize(triangle_normal - (dHdx * R1 + dHdy * R2) / det);
    } else if (weight > 0.0) {
        // Triplanar path: uv and the tangent axes are reconstructed in-shader from the dominant
        // normal component (see header). The gradient is expressed analytically because there is a
        // closed-form uv here, unlike the LSCM case.
        vec3 t, b;
        vec2 units_per_mm;
        projection_axes(tex_pos, triangle_normal, t, b, units_per_mm);

        // Parallax occlusion mapping: march the view ray through the height shell and shade at the
        // first point where it drops below the displaced surface (see header).
        float amp      = (invert ? -1.0 : 1.0) * depth_mm * clamp(weight, 0.0, 1.0);
        vec3  view_dir = normalize(eye_model_pos - tex_pos);
        float v_dot_n  = dot(view_dir, triangle_normal);
        vec2  uv       = project_uv(tex_pos, triangle_normal);

        // The shell the displaced surface lives inside, as signed heights along the normal. Taken from
        // both ends of h in [0, 1] so it stays correct for an inverted layer or a raised midlevel,
        // where the surface sits *below* the undisplaced one.
        float h_end_a = amp * (0.0 - midlevel);
        float h_end_b = amp * (1.0 - midlevel);
        float h_hi    = max(h_end_a, h_end_b);
        float h_lo    = min(h_end_a, h_end_b);
        // How far, in mm, sweeping the ray across the shell slides the sample point sideways. Below half
        // a texel there is no parallax to find and the march would be pure cost - which is the common
        // case of looking straight down at a surface.
        float sweep = length(view_dir - triangle_normal * v_dot_n) * (h_hi - h_lo) / max(v_dot_n, 1e-4);
        if (v_dot_n > 0.05 && sweep > 0.5 * tiling_scale * height_tex_texel.x) {
            // A point at ray parameter s (model_pos + view_dir * s) sits at height s * v_dot_n above the
            // undisplaced surface. Start at the top of the shell, where the ray is outside the surface
            // by construction, and step inward; the crossing is what this pixel actually sees.
            float s        = h_hi / v_dot_n;
            float ds       = (h_hi - h_lo) / (v_dot_n * float(PARALLAX_STEPS));
            vec2  prev_uv  = project_uv(tex_pos + view_dir * s, triangle_normal);
            float prev_gap = h_hi - amp * (H_AT(prev_uv) - midlevel); // >= 0 by construction
            for (int i = 0; i < PARALLAX_STEPS; ++i) {
                s -= ds;
                vec2  cur_uv = project_uv(tex_pos + view_dir * s, triangle_normal);
                float gap    = s * v_dot_n - amp * (H_AT(cur_uv) - midlevel);
                if (gap <= 0.0) {
                    // Crossed between the last two samples - interpolating the hit is what stops it
                    // quantising to the step size, and so what keeps the step count affordable.
                    uv = mix(prev_uv, cur_uv, clamp(prev_gap / max(prev_gap - gap, 1e-6), 0.0, 1.0));
                    break;
                }
                prev_uv  = cur_uv;
                prev_gap = gap;
            }
        }

        color_uv = uv; // after the parallax march, so colour and relief stay registered
        have_uv  = true;

        float hL = texture(height_tex, uv - vec2(height_tex_texel.x, 0.0)).r;
        float hR = texture(height_tex, uv + vec2(height_tex_texel.x, 0.0)).r;
        float hD = texture(height_tex, uv - vec2(0.0, height_tex_texel.y)).r;
        float hU = texture(height_tex, uv + vec2(0.0, height_tex_texel.y)).r;

        // Central difference, per uv unit (not per texel).
        vec2 dh_duv = vec2((hR - hL) / (2.0 * height_tex_texel.x), (hU - hD) / (2.0 * height_tex_texel.y));

        // uv -> mm is 1/tiling_scale for the triplanar projection, so this turns the uv-space
        // gradient into a real surface slope.
        float inv_tiling = (tiling_scale > 1e-6) ? (1.0 / tiling_scale) : 1.0;
        float amplitude  = (invert ? -1.0 : 1.0) * depth_mm * inv_tiling * clamp(weight, 0.0, 1.0);
        // uv was rotated by project_uv() while t/b are the unrotated model axes, so rotate the
        // gradient back into the axes' frame.
        float cs = cos(rotation_rad);
        float sn = sin(rotation_rad);
        // One uv unit is tiling_scale mm along u but tiling_scale / tex_aspect mm along v, so the v
        // component of the gradient carries the extra factor before being rotated back into t/b.
        vec2  g     = vec2(dh_duv.x, dh_duv.y * tex_aspect);
        // ...and back out of raw-projection units into millimetres of travel along t / b, which is a
        // no-op except for the spherical longitude (see projection_axes()). After the inverse rotation,
        // because units_per_mm is expressed in the t/b frame rather than in uv.
        vec2  slope = amplitude * vec2(g.x * cs + g.y * sn, -g.x * sn + g.y * cs) * units_per_mm;

        vec3 gradient = slope.x * t + slope.y * b;
        gradient -= triangle_normal * dot(triangle_normal, gradient);
        triangle_normal = normalize(triangle_normal - gradient);
    }

    vec3 eye_normal = normalize(view_normal_matrix * triangle_normal);
    float NdotL = max(dot(eye_normal, LIGHT_TOP_DIR), 0.0);

    vec2 intensity = vec2(0.0);
    intensity.x = INTENSITY_AMBIENT + NdotL * LIGHT_TOP_DIFFUSE;
    vec3 position = (view_model_matrix * model_pos).xyz;
    intensity.y = LIGHT_TOP_SPECULAR * pow(max(dot(-normalize(position), reflect(-LIGHT_TOP_DIR, eye_normal)), 0.0), LIGHT_TOP_SHININESS);

    NdotL = max(dot(eye_normal, LIGHT_FRONT_DIR), 0.0);
    intensity.x += NdotL * LIGHT_FRONT_DIFFUSE;

    // Diffuse albedo: the image's colour at this fragment, snapped to the nearest printable colour -
    // and, where that is a mix, the filament the interleave puts here, so the pattern that prints shows.
    // Only the albedo - the specular term (intensity.y) stays white - so a coloured fragment reads as
    // the same material under the same light, and the relief this preview exists to show is unaffected.
    vec3 albedo = uniform_color.rgb;
    if (palette_count > 0 && has_color_tex && have_uv && weight > 0.0)
        // tex_pos, not world_pos: the bake resolves the interleave in the bake frame (world
        // orientation and scale about the volume's origin, see texture_displacement_bake_frame()), so
        // measuring z from the bed instead shifted the band phase by the volume origin's height - a
        // different filament in the same place than the bake produces.
        albedo = printed_color(nearest_palette_entry(texture(color_tex, color_uv).rgb), tex_pos, triangle_normal, pos_fwidth);
    out_color = vec4(vec3(intensity.y) + albedo * intensity.x, uniform_color.a);
}
