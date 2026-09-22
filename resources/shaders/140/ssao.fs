#version 140

/**
 * SSAO Shader - GLSL 140 version with sharp depth threshold
 * Only darkens valleys/concave areas, ignores smooth variations
 */

uniform sampler2D color_texture;
uniform sampler2D depth_texture;
uniform vec2 inv_tex_size;
uniform float z_near;
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
    ivec2 sz = textureSize(depth_texture, 0);
    ivec2 p = clamp(pixel, ivec2(0), sz - 1);
    float d = texelFetch(depth_texture, p, 0).r;
    vec4 ndc = vec4((vec2(p) + 0.5) * inv_tex_size * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    vec4 view = inv_projection_matrix * ndc;
    return view.xyz / view.w;
}

// Surface normal at the given pixel, from the forward differences of the reconstructed view
// position. It rings by a pixel across a depth discontinuity, which is acceptable here: the
// normal only weights the occlusion, nothing is shaded with it.
vec3 view_normal(ivec2 pixel)
{
    vec3 p  = view_pos(pixel);
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

    float center_depth = -view_pos(pixel).z;
    vec3 normal_center = view_normal(pixel);

    // Calculate upward-facing factor (Z-up coordinate system)
    float up_factor = clamp(dot(normal_center, up_view) * 1.5, 0.0, 1.0);

    // Adaptive radius in pixel space
    int radius = int(mix(2.0, 4.0, center_depth / z_far));

    // Optimized sampling pattern
    const ivec2 offsets[12] = ivec2[](
        ivec2(1, 0),  ivec2(-1, 0),  ivec2(0, 1),  ivec2(0, -1),
        ivec2(1, 1),  ivec2(-1, 1),  ivec2(1, -1), ivec2(-1, -1),
        ivec2(2, 0),  ivec2(-2, 0),  ivec2(0, 2),  ivec2(0, -2)
    );

    // The thresholds below are view-space distances, so they have to scale with how far away the
    // surface is. Held fixed they mean a fraction of a millimetre, which every extrusion ridge
    // clears - the term then saturates over the whole print and the AO reads as a flat dimming.
    float threshold_min = 0.0015 * center_depth;
    float threshold_max = 0.0075 * center_depth;

    float occlusion = 0.0;
    int valid_samples = 0;

    for (int i = 0; i < 12; i++) {
        ivec2 sample_pixel = pixel + offsets[i] * radius;
        
        if (sample_pixel.x < 0 || sample_pixel.y < 0) 
            continue;
        
        float sample_depth = -view_pos(sample_pixel).z;
        vec3 normal_sample = view_normal(sample_pixel);
        
        // Depth difference (positive if neighbor is closer to camera)
        float depth_diff = center_depth - sample_depth;
        
        float contribution = 0.0;
        if (depth_diff > threshold_min) {
            // Abrupt mapping with power curve
            contribution = (depth_diff - threshold_min) / (threshold_max - threshold_min);
            contribution = clamp(contribution, 0.0, 1.0);
            contribution = pow(contribution, 2.0);  // Steeper curve for sharper transition
        }
        
        // Reduce occlusion on planar surfaces (similar normals)
        float normal_similarity = dot(normal_center, normal_sample);
        float planar_factor = smoothstep(0.75, 0.95, normal_similarity);
        contribution *= (1.0 - planar_factor * 0.6);
        
        occlusion += contribution;
        valid_samples++;
    }

    if (valid_samples > 0) {
        // Calculate ambient occlusion factor with higher base intensity
        float ao_factor = 1.0 - (occlusion / float(valid_samples)) * 0.6;
        
        // Keep bright areas clean (higher minimum for upward-facing surfaces)
        float ao_min = mix(0.55, 0.85, up_factor);
        ao_factor = clamp(ao_factor, ao_min, 1.0);
        
        // Slight brightness boost for upward-facing surfaces
        float brightness_boost = 1.0 + up_factor * 0.15;
        ao_factor = ao_factor * brightness_boost;
        
        occlusion = ao_factor;
    } else {
        occlusion = 1.0;
    }

    frag_color = vec4(color * occlusion, 1.0);
}
