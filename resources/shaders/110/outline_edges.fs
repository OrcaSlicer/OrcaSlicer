#version 110

// A crease fades in as the angle between its faces grows from 25 to 40 degrees.
const float CREASE_COS_START = 0.906;
const float CREASE_COS_FULL = 0.766;

uniform sampler2D mask_tex;
uniform sampler2D depth_tex;
uniform vec2 tex_size;
uniform mat4 inv_projection_matrix;
// View space size of a pixel at view depth z: x * -z + y.
uniform vec2 pixel_size_coef;
uniform int ring_reach;
uniform float ring_sigma;

vec2 unpack2(float c)
{
    float v = floor(c * 65535.0 + 0.5);
    float hi = floor(v / 256.0);
    return vec2(hi, v - hi * 256.0);
}

vec4 fetch_mask(ivec2 p)
{
    return texture2D(mask_tex, (vec2(p) + 0.5) / tex_size);
}

float fetch_depth(ivec2 p)
{
    return texture2D(depth_tex, (vec2(p) + 0.5) / tex_size).r;
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

vec3 view_position(ivec2 p, float depth)
{
    vec4 v = inv_projection_matrix * vec4((vec2(p) + 0.5) / tex_size * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    return v.xyz / v.w;
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
    vec4 mb = fetch_mask(q - dir);
    vec4 ma = fetch_mask(q + 2 * dir);
    vec3 nb = covered(mb) ? normal_of(mb) : n0;
    vec3 na = covered(ma) ? normal_of(ma) : n1;
    float d0 = fetch_depth(q);
    float d1 = fetch_depth(q + dir);
    vec3 p0 = view_position(q, d0);
    vec3 p1 = view_position(q + dir, d1);
    // One surface hides the other: they are pixels apart, and not along a surface seen edge on.
    vec3 d = p1 - p0;
    float gap = max(abs(dot(n0, d)), abs(dot(n1, d)));
    float px = dot(pixel_size_coef, vec2(-max(p0.z, p1.z), 1.0));
    float jump = smoothstep(2.0, 4.0, gap / px) * smoothstep(0.25, 0.5, gap / max(length(d), 1e-12));
    float crease = 1.0 - smoothstep(CREASE_COS_FULL, CREASE_COS_START, dot(n0, n1));
    float strength = unpack2(m0.g).x == unpack2(m1.g).x ? max(jump, crease) : 1.0;
    if (strength <= 0.0)
        return 0.0;

    // Where the depth planes of both sides meet, when each side is flat over two texels and the planes part
    // clearly along dir.
    float crossing = 0.5;
    if (covered(mb) && covered(ma) && dot(nb, n0) > 0.999 && dot(na, n1) > 0.999) {
        float a = d0 - fetch_depth(q - dir);
        float b = fetch_depth(q + 2 * dir) - d1;
        float u = (d1 - d0 - b) / (a - b);
        if (abs(a - b) > 0.1 * (abs(a) + abs(b)) && u >= 0.0 && u <= 1.0)
            crossing = u;
    }
    return pack_edge(strength, crossing);
}

void main()
{
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
            if (nearest == 0.0 || abs(float(k)) < abs(nearest - 16.0))
                nearest = float(16 + k);
        }
    }

    gl_FragColor = vec4(inner_edge(q, ivec2(1, 0)), inner_edge(q, ivec2(0, 1)),
                        nearest > 0.0 ? (nearest + 0.25) / 255.0 : 0.0, coverage / weights);
}
