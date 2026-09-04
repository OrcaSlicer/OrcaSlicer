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
uniform float rotation_rad;
uniform vec2  uv_offset;
uniform bool  use_vertex_uv;

varying vec3  clipping_planes_dots;
varying vec4  model_pos;
varying vec4  world_pos;
varying float distortion;
varying vec2  vertex_uv;

vec2 project_uv(vec3 p, vec3 n)
{
    vec3 an = abs(n);
    vec2 planar = (an.x >= an.y && an.x >= an.z) ? p.yz : ((an.y >= an.x && an.y >= an.z) ? p.xz : p.xy);
    planar *= (tiling_scale > 1e-6) ? (1.0 / tiling_scale) : 1.0;
    float cs = cos(rotation_rad);
    float sn = sin(rotation_rad);
    return vec2(planar.x * cs - planar.y * sn, planar.x * sn + planar.y * cs) + uv_offset;
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

    vec3 triangle_normal = normalize(cross(dFdx(model_pos.xyz), dFdy(model_pos.xyz)));
    if (volume_mirrored)
        triangle_normal = -triangle_normal;

    vec3 base;
    if (mode == 1) {
        base = heatmap(distortion);
    } else {
        vec2 uv = use_vertex_uv ? vertex_uv : project_uv(model_pos.xyz, triangle_normal);
        vec2 c  = floor(uv * checker_freq);
        float check = mod(c.x + c.y, 2.0);
        base = (check < 0.5) ? vec3(0.22, 0.23, 0.26) : vec3(0.82, 0.83, 0.86);
    }

    vec3  eye_normal = normalize(view_normal_matrix * triangle_normal);
    float intensity  = INTENSITY_AMBIENT + max(dot(eye_normal, LIGHT_TOP_DIR), 0.0) * LIGHT_TOP_DIFFUSE
                                         + max(dot(eye_normal, LIGHT_FRONT_DIR), 0.0) * LIGHT_FRONT_DIFFUSE;
    gl_FragColor = vec4(base * intensity, 1.0);
}
