#version 140

/**
 * SSAO Shader - GLSL 140 version with highlight protection
 * Preserves brightness on upward-facing surfaces for better visual quality
 */

uniform sampler2D color_texture;
uniform sampler2D depth_texture;
uniform sampler2D normal_texture;   
uniform float z_near;
uniform float z_far;

const float AO_BLEND_STRENGTH = 0.8;
const float GAMMA = 2.2;

in vec2 tex_coord;
out vec4 frag_color;

float linearize_depth(float depth)
{
    float z = depth * 2.0 - 1.0;
    return (2.0 * z_near * z_far) / (z_far + z_near - z * (z_far - z_near));
}

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    float center_depth = linearize_depth(texelFetch(depth_texture, pixel, 0).r);
    
    // Sample normal buffer (stored as RGB in 0-1 range, convert to -1 to 1)
    vec3 normal_center = texelFetch(normal_texture, pixel, 0).rgb * 2.0 - 1.0;
    normal_center = normalize(normal_center);
    
    // Calculate upward-facing factor
    // Assumes Z-up coordinate system (typical for 3D printing)
    float up_factor = clamp(normal_center.z * 1.5, 0.0, 1.0);  // Boosted for better response
    
    // Alternative if using Y-up: float up_factor = clamp(normal_center.y * 1.5, 0.0, 1.0);

    // Adaptive radius in pixel space
    int radius = int(mix(2.0, 5.0, center_depth / z_far));

    // Optimized sampling pattern
    const ivec2 offsets[12] = ivec2[](
        ivec2(1, 0),  ivec2(-1, 0),  ivec2(0, 1),  ivec2(0, -1),
        ivec2(1, 1),  ivec2(-1, 1),  ivec2(1, -1), ivec2(-1, -1),
        ivec2(2, 0),  ivec2(-2, 0),  ivec2(0, 2),  ivec2(0, -2)
    );

    float occlusion = 0.0;
    int valid_samples = 0;

    for (int i = 0; i < 12; i++) {
        ivec2 sample_pixel = pixel + offsets[i] * radius;
        
        if (sample_pixel.x < 0 || sample_pixel.y < 0) 
            continue;
        
        float sample_depth = linearize_depth(texelFetch(depth_texture, sample_pixel, 0).r);
        
        // Sample normal at neighbor
        vec3 normal_sample = texelFetch(normal_texture, sample_pixel, 0).rgb * 2.0 - 1.0;
        
        // Direction from center to sample in screen space
        vec2 dir_2d = normalize(vec2(sample_pixel - pixel));
        
        // Reduce occlusion when normals are similar (planar surfaces)
        float normal_similarity = dot(normal_center, normal_sample);
        float planar_factor = smoothstep(0.7, 0.95, normal_similarity);

        float depth_diff = max(0.0, center_depth - sample_depth);
        float threshold = 0.02 * (0.5 + center_depth / z_far);
        float contribution = smoothstep(0.001, threshold, depth_diff);

        // Reduce contribution on planar surfaces and top areas
        float top_factor = 1.0 - up_factor * 0.6;  // 60% less occlusion on tops
        contribution *= (1.0 - planar_factor * 0.5) * top_factor;

        occlusion += contribution;
        valid_samples++;
    }

    if (valid_samples > 0) {
        float ao_factor = 1.0 - (occlusion / float(valid_samples)) * 0.45;

        // Brighter minimum for top surfaces
        float ao_min = mix(0.50, 0.75, up_factor);
        ao_factor = clamp(ao_factor, ao_min, 1.0);

        // Additional brightness boost for upward-facing surfaces
        float brightness_boost = 1.0 + up_factor * 0.2;
        ao_factor = pow(ao_factor, 1.1) * brightness_boost;

        occlusion = clamp(ao_factor, 0.45, 1.05);
    } else {
        occlusion = 1.0;
    }

    vec3 color = texture(color_texture, tex_coord).rgb;
    float blended_occlusion = mix(1.0, occlusion, AO_BLEND_STRENGTH);

    // Apply AO in approximate linear space to avoid crushing mid tones.
    vec3 color_linear = pow(max(color, vec3(0.0)), vec3(GAMMA));
    color_linear *= blended_occlusion;
    vec3 color_out = pow(max(color_linear, vec3(0.0)), vec3(1.0 / GAMMA));
    frag_color = vec4(color_out, 1.0);
}