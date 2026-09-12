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
uniform vec3       eye_model_pos; // camera position in this volume's local space, for the view ray
uniform bool       use_vertex_uv; // true: sample at vertex_uv with a derived tangent frame (LSCM)
// A 2x3 affine (columns packed as lin = (m00, m01, m10, m11), tr = (m02, m12)) applied to the uv of
// the island currently being dragged in the UV editor (island_active > 0.5). Identity when nothing is
// dragged, so this whole path is a no-op then. Lets a UV island drag move the bump on the model with
// only a uniform update 
uniform vec4       island_delta_lin;
uniform vec2       island_delta_tr;

in vec3  clipping_planes_dots;
in vec4  model_pos;
in vec4  world_pos;
in float weight;
in float island_active;
in vec2  vertex_uv;

out vec4 out_color;

// The two model-space axes the triplanar planar coordinate is read off, per dominant normal
// component - same choice libslic3r's project_planar() makes, so planar.x runs along t, planar.y
// along b.
void projection_axes(vec3 n, out vec3 t, out vec3 b)
{
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
    vec3 an = abs(n);
    vec2 planar = (an.x >= an.y && an.x >= an.z) ? p.yz : ((an.y >= an.x && an.y >= an.z) ? p.xz : p.xy);
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
vec3 quantize_to_palette(vec3 rgb)
{
    vec3  lab  = srgb_to_lab(rgb);
    int   best = 0;
    float bd   = 1.0e20;
    for (int i = 0; i < 64; ++i) {
        if (i >= palette_count)
            break;
        vec3  d  = lab - palette_lab[i];
        float d2 = dot(d, d);
        if (d2 < bd) {
            bd   = d2;
            best = i;
        }
    }
    return palette_rgb[best];
}

void main()
{
    if (any(lessThan(clipping_planes_dots, ZERO)))
        discard;

    vec3 triangle_normal = normalize(cross(dFdx(model_pos.xyz), dFdy(model_pos.xyz)));
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
        vec3  sigmaS = dFdx(model_pos.xyz);
        vec3  sigmaT = dFdy(model_pos.xyz);
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
        projection_axes(triangle_normal, t, b);

        // Parallax occlusion mapping: march the view ray through the height shell and shade at the
        // first point where it drops below the displaced surface (see header).
        float amp      = (invert ? -1.0 : 1.0) * depth_mm * clamp(weight, 0.0, 1.0);
        vec3  view_dir = normalize(eye_model_pos - model_pos.xyz);
        float v_dot_n  = dot(view_dir, triangle_normal);
        vec2  uv       = project_uv(model_pos.xyz, triangle_normal);

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
            vec2  prev_uv  = project_uv(model_pos.xyz + view_dir * s, triangle_normal);
            float prev_gap = h_hi - amp * (H_AT(prev_uv) - midlevel); // >= 0 by construction
            for (int i = 0; i < PARALLAX_STEPS; ++i) {
                s -= ds;
                vec2  cur_uv = project_uv(model_pos.xyz + view_dir * s, triangle_normal);
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
        vec2  slope = amplitude * vec2(g.x * cs + g.y * sn, -g.x * sn + g.y * cs);

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

    // Diffuse albedo: the image's colour at this fragment, snapped to the nearest printable colour.
    // Only the albedo - the specular term (intensity.y) stays white - so a coloured fragment reads as
    // the same material under the same light, and the relief this preview exists to show is unaffected.
    vec3 albedo = uniform_color.rgb;
    if (palette_count > 0 && has_color_tex && have_uv && weight > 0.0)
        albedo = quantize_to_palette(texture(color_tex, color_uv).rgb);
    out_color = vec4(vec3(intensity.y) + albedo * intensity.x, uniform_color.a);
}
