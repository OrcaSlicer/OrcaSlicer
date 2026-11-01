#version 140

// Farthest triangle edge distance stored by outline_mask.fs, in pixels.
const float EXIT_REACH = 4.0;
// A crease fades in as the angle between its faces grows from 25 to 40 degrees.
const float CREASE_COS_START = 0.906;
const float CREASE_COS_FULL = 0.766;

uniform sampler2D mask_tex;
uniform sampler2D depth_tex;
uniform mat4 inv_projection_matrix;
// View space size of a pixel at view depth z: x * -z + y.
uniform vec2 pixel_size_coef;
uniform int ring_reach;
uniform float ring_sigma;

out vec4 out_color;

ivec2 tex_size;

vec2 unpack2(float c)
{
    float v = floor(c * 65535.0 + 0.5);
    float hi = floor(v / 256.0);
    return vec2(hi, v - hi * 256.0);
}

vec4 fetch_mask(ivec2 p)
{
    return texelFetch(mask_tex, clamp(p, ivec2(0), tex_size - 1), 0);
}

float fetch_depth(ivec2 p)
{
    return texelFetch(depth_tex, clamp(p, ivec2(0), tex_size - 1), 0).r;
}

bool covered(vec4 m)
{
    return m.g > 0.0;
}

vec3 normal_of(vec4 m)
{
    vec2 e = unpack2(m.r) / 255.0 * 2.0 - 1.0;
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

// Pixels to the edge of the triangle under the texel along an axis direction.
float exit_along(vec4 m, ivec2 dir)
{
    vec2 e = unpack2(dir.x != 0 ? m.b : m.a) / 255.0 * EXIT_REACH;
    return dir.x + dir.y > 0 ? e.x : e.y;
}

vec3 view_position(ivec2 p, float depth)
{
    vec4 v = inv_projection_matrix * vec4((vec2(p) + 0.5) / vec2(tex_size) * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    return v.xyz / v.w;
}

// Under a pixel along both axes: the normal of such a sliver may be far off its surface.
bool tiny_triangle(vec4 m)
{
    vec4 e = vec4(unpack2(m.b), unpack2(m.a)) / 255.0 * EXIT_REACH;
    return e.x + e.y < 1.0 && e.z + e.w < 1.0;
}

// The normal of the texel, or of one beside it when the texel lies on a tiny triangle.
vec3 surface_normal(vec4 m, vec4 l, vec4 r)
{
    if (tiny_triangle(m)) {
        if (covered(l) && !tiny_triangle(l))
            return normal_of(l);
        if (covered(r) && !tiny_triangle(r))
            return normal_of(r);
    }
    return normal_of(m);
}

// One surface hides the other: d, between them, is pixels long off their planes.
float jump_strength(vec3 n0, vec3 n1, vec3 d, float px)
{
    float gap = max(abs(dot(n0, d)), abs(dot(n1, d)));
    return smoothstep(2.0, 4.0, gap / px);
}

// 1 for a turn between neighboring texels of about 7 to 23 degrees: neither flat nor a crease.
float gradual_turn(float cos_angle)
{
    return (1.0 - smoothstep(0.985, 0.997, cos_angle)) * smoothstep(0.88, 0.92, cos_angle);
}

float pack_edge(float strength, float crossing)
{
    return (floor(strength * 15.0 + 0.5) * 16.0 + floor(clamp(crossing, 0.0, 1.0) * 15.0 + 0.5) + 0.25) / 255.0;
}

// The inner edge between q and q + dir: how strong it is, and where it crosses from q (0) to q + dir (1).
float inner_edge(ivec2 q, ivec2 dir)
{
    vec4 m0 = fetch_mask(q);
    vec4 m1 = fetch_mask(q + dir);
    // The silhouette is drawn by the ring.
    if (!covered(m0) || !covered(m1))
        return 0.0;

    vec3 n0 = normal_of(m0);
    vec3 n1 = normal_of(m1);
    float d0 = fetch_depth(q);
    float d1 = fetch_depth(q + dir);
    vec3 p0 = view_position(q, d0);
    vec3 p1 = view_position(q + dir, d1);
    vec3 d = p1 - p0;
    float px = dot(pixel_size_coef, vec2(-max(p0.z, p1.z), 1.0));
    bool same_part = m0.g == m1.g;
    if (same_part && jump_strength(n0, n1, d, px) <= 0.0 && dot(n0, n1) >= CREASE_COS_START)
        return 0.0;

    // Beside the pair, and before and after it along dir.
    ivec2 perp = ivec2(dir.y, dir.x);
    vec4 l0 = fetch_mask(q - perp);
    vec4 r0 = fetch_mask(q + perp);
    vec4 l1 = fetch_mask(q + dir - perp);
    vec4 r1 = fetch_mask(q + dir + perp);
    vec4 mb = fetch_mask(q - dir);
    vec4 ma = fetch_mask(q + 2 * dir);
    n0 = surface_normal(m0, l0, r0);
    n1 = surface_normal(m1, l1, r1);
    vec3 nb = covered(mb) ? normal_of(mb) : n0;
    vec3 na = covered(ma) ? normal_of(ma) : n1;
    float jump = jump_strength(n0, n1, d, px);
    // A step: the texel after the pair lies pixels off the surface before it carried on, both ways, which a surface
    // seen edge on does not.
    float off = 1e9;
    if (covered(mb))
        off = length(p1 - (2.0 * p0 - view_position(q - dir, fetch_depth(q - dir))));
    if (covered(ma))
        off = min(off, length(p0 - (2.0 * p1 - view_position(q + 2 * dir, fetch_depth(q + 2 * dir)))));
    if (off < 1e9)
        jump *= smoothstep(1.0, 2.0, off / px);
    // Nor a fillet, which turns a little at every texel on both sides.
    jump *= 1.0 - gradual_turn(dot(nb, n0)) * gradual_turn(dot(n1, na));
    float crease = 1.0 - smoothstep(CREASE_COS_FULL, CREASE_COS_START, dot(n0, n1));
    float strength = same_part ? max(jump, crease) : 1.0;
    if (strength <= 0.0)
        return 0.0;

    // Where the triangle under q ends and the one under q + dir starts: together when they share the edge.
    float lo = exit_along(m0, dir);
    float hi = 1.0 - exit_along(m1, -dir);
    if (same_part && abs(hi - lo) < 0.15)
        return pack_edge(strength, 0.5 * (lo + hi));
    // Where the depth planes of both sides meet, when each side is flat over two texels and the planes part
    // clearly along dir.
    if (covered(mb) && covered(ma) && dot(nb, n0) > 0.999 && dot(na, n1) > 0.999) {
        float a = d0 - fetch_depth(q - dir);
        float b = fetch_depth(q + 2 * dir) - d1;
        float u = (d1 - d0 - b) / (a - b);
        if (abs(a - b) > 0.1 * (abs(a) + abs(b)) && u >= 0.0 && u <= 1.0)
            return pack_edge(strength, u);
    }
    // Where the nearer surface ends, when its triangle ends on a straight edge: the texel behind it, or the
    // texels beside it, end on the same line.
    float crossing = 0.5;
    if (jump > 0.0) {
        if (p0.z > p1.z) {
            if ((covered(mb) && abs(exit_along(mb, dir) - min(lo + 1.0, EXIT_REACH)) < 0.1) ||
                (covered(l0) && covered(r0) && abs(exit_along(l0, dir) + exit_along(r0, dir) - 2.0 * lo) < 0.1))
                crossing = lo;
        }
        else {
            float e = 1.0 - hi;
            if ((covered(ma) && abs(exit_along(ma, -dir) - min(e + 1.0, EXIT_REACH)) < 0.1) ||
                (covered(l1) && covered(r1) && abs(exit_along(l1, -dir) + exit_along(r1, -dir) - 2.0 * e) < 0.1))
                crossing = hi;
        }
    }
    return pack_edge(strength, crossing);
}

void main()
{
    tex_size = textureSize(mask_tex, 0);
    ivec2 q = ivec2(gl_FragCoord.xy);

    // For the ring around the selection: the nearest covered texel along the row, offset by 16, and the
    // coverage of the row under a gaussian, whose sum over the column sets the silhouette between texels.
    float nearest = 0.0;
    float coverage = 0.0;
    float weights = 0.0;
    for (int k = -ring_reach; k <= ring_reach; ++k) {
        float w = exp(-float(k * k) / (2.0 * ring_sigma * ring_sigma));
        weights += w;
        if (covered(fetch_mask(q + ivec2(k, 0)))) {
            coverage += w;
            if (nearest == 0.0 || abs(k) < abs(int(nearest) - 16))
                nearest = float(16 + k);
        }
    }

    out_color = vec4(inner_edge(q, ivec2(1, 0)), inner_edge(q, ivec2(0, 1)),
                     nearest > 0.0 ? (nearest + 0.25) / 255.0 : 0.0, coverage / weights);
}
