#version 110

const float INF = 1e9;

uniform sampler2D mask_tex;
uniform sampler2D depth_tex;
uniform sampler2D edges_tex;
uniform vec2 tex_size;
// false draws the ring around the selection, true the lines along its inner edges.
uniform bool inner;
// The stencil keeps the ring off the samples of the selection, without it the ring stays off covered pixels.
uniform bool use_stencil;
// Widths in pixels: x at least, y at most, z the width on the model in view space units.
uniform vec3 ring_width;
uniform int ring_reach;
uniform float ring_sigma;
// Half the width of an inner line, as ring_width.
uniform vec3 line_width;
uniform int line_reach;
uniform float line_opacity;
uniform vec3 outline_color;
// The projection on view z: window depth is (x * z + y) / (z * z + w), remapped from [-1, 1].
uniform vec4 depth_coef;
// View space size of a pixel at view depth z: x * -z + y.
uniform vec2 pixel_size_coef;

vec4 fetch_mask(ivec2 p)
{
    return texture2D(mask_tex, (vec2(p) + 0.5) / tex_size);
}

float fetch_depth(ivec2 p)
{
    return texture2D(depth_tex, (vec2(p) + 0.5) / tex_size).r;
}

vec4 fetch_edges(ivec2 p)
{
    return texture2D(edges_tex, (vec2(p) + 0.5) / tex_size);
}

bool covered(vec4 m)
{
    return m.g > 0.0;
}

// The texel and its eight neighbors are covered.
bool inside_mask(ivec2 p)
{
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            if (!covered(fetch_mask(p + ivec2(x, y))))
                return false;
    return true;
}

// Strength and crossing of an inner edge written by outline_edges.fs.
vec2 unpack_edge(float c)
{
    float v = floor(c * 255.0 + 0.5);
    float s = floor(v / 16.0);
    return vec2(s, v - s * 16.0) / 15.0;
}

float pixel_size(float z)
{
    return dot(pixel_size_coef, vec2(-z, 1.0));
}

// Thicker as the camera closes in, within a range of pixels.
float width_at(vec3 width, float z)
{
    return clamp(width.z / pixel_size(z), width.x, width.y);
}

// A smooth edge 1.2 pixels wide around width.
float edge_alpha(float width, float d)
{
    return 1.0 - smoothstep(width - 0.6, width + 0.6, d);
}

float view_z(float depth)
{
    float ndc = depth * 2.0 - 1.0;
    return (depth_coef.y - ndc * depth_coef.w) / (ndc * depth_coef.z - depth_coef.x);
}

float window_depth(float z)
{
    return 0.5 * (depth_coef.x * z + depth_coef.y) / (depth_coef.z * z + depth_coef.w) + 0.5;
}

// One sided, so that the slope does not span an edge. Uncovered texels keep the cleared depth of 1.
float depth_slope(float d, float before, float after)
{
    float s = INF;
    if (before < 1.0) s = abs(d - before);
    if (after < 1.0) s = min(s, abs(after - d));
    return s < INF ? s : 0.0;
}

// The depth of the selection at p, moved forward by its spread over the pixel, so that it passes on every sample.
float surface_depth(ivec2 p)
{
    float d = fetch_depth(p);
    float sx = depth_slope(d, fetch_depth(p - ivec2(1, 0)), fetch_depth(p + ivec2(1, 0)));
    float sy = depth_slope(d, fetch_depth(p - ivec2(0, 1)), fetch_depth(p + ivec2(0, 1)));
    return d - 0.5 * (sx + sy) - 1e-6;
}

// x beyond which a standard normal has p of its mass, for p up to 0.5 (Abramowitz and Stegun 26.2.23).
float normal_tail_inverse(float p)
{
    float t = sqrt(-2.0 * log(p));
    return t - (2.515517 + t * (0.802853 + t * 0.010328)) / (1.0 + t * (1.432788 + t * (0.189269 + t * 0.001308)));
}

void render_ring(ivec2 p)
{
    vec4 m = fetch_mask(p);
    float alpha = 1.0;
    float depth;
    if (covered(m)) {
        // Only the silhouette of the mask may hold samples off the selection. Inside, a face the mask has and the
        // scene culled, say, would get no stencil bit and fill with the ring.
        if (!use_stencil || inside_mask(p))
            discard;
        depth = surface_depth(p);
    }
    else {
        // The nearest covered texel from the nearest one of each row, and the gaussian coverage around p.
        float best = INF;
        ivec2 t = p;
        float coverage = 0.0;
        float weights = 0.0;
        for (int j = -ring_reach; j <= ring_reach; ++j) {
            vec4 e = fetch_edges(p + ivec2(0, j));
            float w = exp(-float(j * j) / (2.0 * ring_sigma * ring_sigma));
            weights += w;
            coverage += w * e.a;
            float v = floor(e.b * 255.0 + 0.5);
            if (v > 0.0) {
                float h = v - 16.0;
                float d2 = h * h + float(j * j);
                if (d2 < best) {
                    best = d2;
                    t = p + ivec2(int(h), j);
                }
            }
        }
        if (best >= INF)
            discard;
        // The coverage of a straight silhouette at dist is the normal tail beyond dist / sigma. Corners and thin
        // parts fall off faster, so it is kept within half a texel of the midpoint to the nearest covered texel.
        coverage /= weights;
        float centers = sqrt(best) - 0.5;
        float dist = coverage < 0.5 ? ring_sigma * normal_tail_inverse(max(coverage, 1e-6)) : 0.0;
        dist = clamp(dist, centers - 0.5, centers + 0.5);
        float z = view_z(fetch_depth(t));
        alpha = edge_alpha(width_at(ring_width, z), dist);
        if (alpha <= 0.0)
            discard;
        m = fetch_mask(t);
        // Brought forward with the distance, so that it shows over what the selection stands on.
        depth = window_depth(z + 3.0 * (dist + 2.0) * pixel_size(z));
    }
    gl_FragColor = vec4(outline_color, alpha);
    gl_FragDepth = depth;
}

void render_lines(ivec2 p)
{
    vec4 m = fetch_mask(p);
    if (!covered(m))
        discard;

    // The nearest crossing along the row and along the column. For a straight line at distance d they lie at
    // dx = d / |cos| and dy = d / |sin|, so 1 / d^2 = 1 / dx^2 + 1 / dy^2.
    float dx = INF;
    float sx = 0.0;
    for (int i = -line_reach - 1; i <= line_reach; ++i) {
        vec2 e = unpack_edge(fetch_edges(p + ivec2(i, 0)).r);
        float d = abs(float(i) + e.y);
        if (e.x > 0.0 && d < dx) {
            dx = d;
            sx = e.x;
        }
    }
    float dy = INF;
    float sy = 0.0;
    for (int j = -line_reach - 1; j <= line_reach; ++j) {
        vec2 e = unpack_edge(fetch_edges(p + ivec2(0, j)).g);
        float d = abs(float(j) + e.y);
        if (e.x > 0.0 && d < dy) {
            dy = d;
            sy = e.x;
        }
    }
    if (dx >= INF && dy >= INF)
        discard;
    float d = (dx < INF && dy < INF) ? dx * dy * inversesqrt(max(dx * dx + dy * dy, 1e-8)) : min(dx, dy);
    // A line thinner on screen than the narrowest drawn fades instead, so that a far selection is not all lines.
    float z = view_z(fetch_depth(p));
    float fade = clamp(line_width.z / (pixel_size(z) * line_width.x), 0.4, 1.0);
    float alpha = line_opacity * fade * max(sx, sy) * edge_alpha(width_at(line_width, z), d);
    if (alpha <= 0.0)
        discard;
    gl_FragColor = vec4(outline_color, alpha);
    gl_FragDepth = surface_depth(p);
}

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    if (inner)
        render_lines(p);
    else
        render_ring(p);
}
