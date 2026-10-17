#version 110

// See resources/shaders/140/texture_displacement_uvcheck.fs; GLSL 1.10 compatibility variant.

#define INTENSITY_CORRECTION 0.6
const vec3 LIGHT_TOP_DIR = vec3(-0.4574957, 0.4574957, 0.7624929);
#define LIGHT_TOP_DIFFUSE    (0.8 * INTENSITY_CORRECTION)
const vec3 LIGHT_FRONT_DIR = vec3(0.6985074, 0.1397015, 0.6985074);
#define LIGHT_FRONT_DIFFUSE  (0.3 * INTENSITY_CORRECTION)
#define INTENSITY_AMBIENT    0.3
const vec3 ZERO = vec3(0.0, 0.0, 0.0);

uniform mat3 view_normal_matrix;
uniform bool volume_mirrored;

uniform int   mode;
uniform float checker_freq;
uniform float tiling_scale;
// Tile extent along v, in mm. Zero or negative means "the same as tiling_scale" - see
// layer_tile_mm() on the CPU side, so an unset uniform behaves exactly as this shader always did.
uniform float tiling_scale_v;
// Mirror along one tile axis; false by default, so an unset uniform is the old behaviour.
uniform bool  mirror_u;
uniform bool  mirror_v;
uniform vec3  tex_anchor; // the volume's origin in world space
// The placement's rotation, so this overlay projects the same frame the bake does. Identity, which
// is what an unset uniform gives, leaves the old world-aligned behaviour.
uniform mat3  tex_rot;    // world -> texture frame
uniform float rotation_rad;
uniform vec2  uv_offset;
uniform bool  use_vertex_uv;
// The in-shader projection (0 Triplanar, 1 Cylindrical, 2 Spherical) and the painted patch's frame the
// wrapping ones wrap around, in the texture frame - the same uniforms, and the same formulas, as
// texture_displacement_shaded.fs, so the checker reports the projection the bake will actually use.
uniform int   projection_mode;
uniform vec3  patch_center;
uniform vec3  patch_axis;
// Height map width / height, as apply_uv_transform() applies it. Without it the checker diverged from
// the bake for any non-square texture, in every in-shader projection.
uniform float tex_aspect;

varying vec3  clipping_planes_dots;
varying vec4  model_pos;
varying vec4  world_pos;
varying float distortion;
varying vec2  vertex_uv;

// Right handed (up x right), like libslic3r's project_cylindrical(), so the angle taken from it grows
// anticlockwise about the axis - to the viewer's right when seen from outside the cylinder.
void cylinder_frame(out vec3 up, out vec3 right, out vec3 fwd)
{
    up = (length(patch_axis) > 1e-8) ? normalize(patch_axis) : vec3(0.0, 0.0, 1.0);
    vec3 arbitrary = (abs(up.z) < 0.9) ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    right = normalize(cross(up, arbitrary));
    fwd   = normalize(cross(up, right));
}

// Which side of a world plane a face is on, with zero counting as the positive side. Not GLSL's own
// sign(), which returns 0 there and would collapse the coordinate onto a line.
float box_face_sign(float n) { return n >= 0.0 ? 1.0 : -1.0; }

// Term for term libslic3r's project_planar() / project_cylindrical() / project_spherical(), in mm.
vec2 projection_raw(vec3 p, vec3 n)
{
    if (projection_mode == 1) {
        vec3  up, right, fwd;
        cylinder_frame(up, right, fwd);
        vec3  rel = p - patch_center;
        float x   = dot(rel, right);
        float y   = dot(rel, fwd);
        return vec2(atan(y, x) * sqrt(x * x + y * y), -dot(rel, up));
    }
    if (projection_mode == 2) {
        vec3  rel    = p - patch_center;
        float radius = length(rel);
        if (radius < 1e-8)
            return vec2(0.0);
        vec3 dir = rel / radius;
        return vec2(atan(dir.y, dir.x), -asin(clamp(dir.z, -1.0, 1.0))) * radius;
    }
    // v negated so the image's first pixel row sits at the top of the model, u flipped on the far side
    // of each plane so the two faces sharing it are not mirror images - see libslic3r's box_plane_yz().
    vec3 an = abs(n);
    if (an.x >= an.y && an.x >= an.z)
        return vec2(box_face_sign(n.x) * p.y, -p.z);
    if (an.y >= an.x && an.y >= an.z)
        return vec2(-box_face_sign(n.y) * p.x, -p.z);
    return vec2(box_face_sign(n.z) * p.x, -p.y);
}


// Tile extents in mm, u then v, with the v fallback layer_tile_mm() applies.
vec2 tile_mm()
{
    float su = (tiling_scale > 1e-6) ? tiling_scale : 1.0;
    return vec2(su, (tiling_scale_v > 1e-6) ? tiling_scale_v : su);
}

// +1 or -1 per axis, matching layer_mirror_sign() on the CPU side.
vec2 mirror_sign()
{
    return vec2(mirror_u ? -1.0 : 1.0, mirror_v ? -1.0 : 1.0);
}

vec2 project_uv(vec3 p, vec3 n)
{
    vec2 planar = projection_raw(p, n);
    float cs = cos(rotation_rad);
    float sn = sin(rotation_rad);
    vec2 r = vec2(planar.x * cs - planar.y * sn, planar.x * sn + planar.y * cs);
    // Extents divide after the rotation, so a tile whose two sides differ stays a rotated rectangle
    // instead of shearing. With a square tile the two orders agree exactly. The mirror is a sign on
    // the same division, in the tile's own frame.
    r *= mirror_sign() / tile_mm();
    r.y *= tex_aspect; // after the rotation, so the rotation stays a rotation rather than a shear
    return r + uv_offset;
}

vec3 heatmap(float t)
{
    t = clamp(t, 0.0, 1.0);
    return clamp(vec3(1.5 - abs(4.0 * t - 3.0),
                      1.5 - abs(4.0 * t - 2.0),
                      1.5 - abs(4.0 * t - 1.0)), 0.0, 1.0);
}

void main()
{
    if (any(lessThan(clipping_planes_dots, ZERO)))
        discard;

    // World space anchored at the volume's origin, like the bake and the shaded preview.
    vec3 triangle_normal = normalize(cross(dFdx(world_pos.xyz), dFdy(world_pos.xyz)));
    vec3 tex_pos = tex_rot * (world_pos.xyz - tex_anchor);
    if (volume_mirrored)
        triangle_normal = -triangle_normal;

    vec3 base;
    if (mode == 1) {
        base = heatmap(distortion);
    } else {
        vec2 uv = use_vertex_uv ? vertex_uv : project_uv(tex_pos, tex_rot * triangle_normal);
        vec2 c  = floor(uv * checker_freq);
        float check = mod(c.x + c.y, 2.0);
        base = (check < 0.5) ? vec3(0.22, 0.23, 0.26) : vec3(0.82, 0.83, 0.86);
    }

    vec3  eye_normal = normalize(view_normal_matrix * triangle_normal);
    float intensity  = INTENSITY_AMBIENT + max(dot(eye_normal, LIGHT_TOP_DIR), 0.0) * LIGHT_TOP_DIFFUSE
                                         + max(dot(eye_normal, LIGHT_FRONT_DIR), 0.0) * LIGHT_FRONT_DIFFUSE;
    gl_FragColor = vec4(base * intensity, 1.0);
}
