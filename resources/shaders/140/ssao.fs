#version 140

/**
 * SSAO Shader - GLSL 140 version with a slope-based occlusion test
 * Only darkens valleys/concave areas, ignores smooth variations
 */

uniform sampler2D color_texture;
uniform sampler2D depth_texture;
uniform vec2 inv_tex_size;
uniform float z_far;
uniform bool is_outline;
// The pass has no normal target to read, so the surface normal is reconstructed from the depth
// buffer. inv_projection_matrix unprojects a pixel back into view space and up_view is world +Z
// expressed in view space, which is what tells a top surface from a wall.
uniform mat4 inv_projection_matrix;
uniform vec3 up_view;

in vec2 tex_coord;
out vec4 frag_color;

// Position of the given pixel in view space. Valid under both an orthographic and a perspective
// camera, unlike the depth linearization it replaces.
vec3 view_pos(ivec2 pixel)
{
    ivec2 p = clamp(pixel, ivec2(0), textureSize(depth_texture, 0) - 1);
    float d = texelFetch(depth_texture, p, 0).r;
    vec4 ndc = vec4((vec2(p) + 0.5) * inv_tex_size * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    vec4 view = inv_projection_matrix * ndc;
    return view.xyz / view.w;
}

// Surface normal at the given pixel, from the forward differences of the reconstructed view
// position. It rings by a pixel across a depth discontinuity, which is acceptable here: the
// normal only weights the occlusion, nothing is shaded with it.
vec3 view_normal(ivec2 pixel, vec3 p)
{
    vec3 px = view_pos(pixel + ivec2(1, 0));
    vec3 py = view_pos(pixel + ivec2(0, 1));
    vec3 n = cross(px - p, py - p);
    float len = length(n);
    return (len > 1e-8) ? n / len : vec3(0.0, 0.0, 1.0);
}

void main()
{
    if (is_outline) {
        frag_color = vec4(texture(color_texture, tex_coord).rgb, 1.0);
        return;
    }
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec3 color = texture(color_texture, tex_coord).rgb;

    // Nothing was drawn here: occluding the background would only darken the gradient, and its
    // reconstructed normal is degenerate anyway.
    if (texelFetch(depth_texture, pixel, 0).r >= 0.9999) {
        frag_color = vec4(color, 1.0);
        return;
    }

    vec3 center_pos = view_pos(pixel);
    float center_depth = -center_pos.z;
    vec3 normal_center = view_normal(pixel, center_pos);

    // Calculate upward-facing factor (Z-up coordinate system)
    float up_factor = clamp(dot(normal_center, up_view), 0.0, 1.0);

    // Adaptive radius in pixel space
    int radius = int(mix(2.0, 4.0, center_depth / z_far));

    // Optimized sampling pattern
    const int SAMPLE_COUNT = 12;
    const ivec2 offsets[SAMPLE_COUNT] = ivec2[](
        ivec2(1, 0),  ivec2(-1, 0),  ivec2(0, 1),  ivec2(0, -1),
        ivec2(1, 1),  ivec2(-1, 1),  ivec2(1, -1), ivec2(-1, -1),
        ivec2(2, 0),  ivec2(-2, 0),  ivec2(0, 2),  ivec2(0, -2)
    );

    // Occlusion is a slope, not a depth difference: the sine of the angle a neighbour subtends
    // above the centre's tangent plane. A raw difference depends on camera distance and zoom,
    // so no fixed thresholds suit both a 0.2 mm layer step and a 5 mm overhang.
    const float SLOPE_MIN = 0.08;   // ~5 degrees, above the depth-buffer noise of a flat surface
    const float SLOPE_MAX = 0.60;   // ~37 degrees, a full crease

    float occlusion = 0.0;

    for (int i = 0; i < SAMPLE_COUNT; i++) {
        // No edge rejection: view_pos clamps, giving a near-zero delta and no occlusion.
        // Rejecting one side only would bias the denominator against the other.
        ivec2 sample_pixel = pixel + offsets[i] * radius;
        
        vec3 delta = view_pos(sample_pixel) - center_pos;
        float dist = length(delta);
        // How far the neighbour rises towards the viewer out of the centre's tangent plane. A
        // flat surface gives ~0 whatever its orientation, so this also subsumes the separate
        // planar test the normals were compared for.
        float rise = (dist > 1e-6) ? dot(delta, normal_center) / dist : 0.0;
        
        float contribution = 0.0;
        if (rise > SLOPE_MIN) {
            // Abrupt mapping with power curve
            contribution = (rise - SLOPE_MIN) / (SLOPE_MAX - SLOPE_MIN);
            contribution = clamp(contribution, 0.0, 1.0);
            contribution = pow(contribution, 2.0);  // Steeper curve for sharper transition
        }
        
        occlusion += contribution;
    }

    // Calculate ambient occlusion factor with higher base intensity
    float ao_factor = 1.0 - (occlusion / float(SAMPLE_COUNT)) * 0.6;

    // Keep bright areas clean (higher minimum for upward-facing surfaces). The old 0.85 floor
    // and 1.15 boost were set when up_factor came from the colour buffer and read ~0; with a
    // real normal they capped a top surface at 2% darkening, which hid the AO entirely.
    float ao_min = mix(0.45, 0.70, up_factor);
    occlusion = clamp(ao_factor, ao_min, 1.0);

    frag_color = vec4(color * occlusion, 1.0);
}
