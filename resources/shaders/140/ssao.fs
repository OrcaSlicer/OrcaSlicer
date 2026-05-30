#version 140

/**
 * SSAO Shader - GLSL 140 version with sharp depth threshold
 * Only darkens valleys/concave areas, ignores smooth variations
 */

uniform sampler2D color_texture;
uniform sampler2D depth_texture;
uniform sampler2D normal_texture;   
uniform float z_near;
uniform float z_far;

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
    
    // Calculate upward-facing factor (Z-up coordinate system)
    float up_factor = clamp(normal_center.z * 1.5, 0.0, 1.0);
    
    // Adaptive radius in pixel space
    int radius = int(mix(2.0, 4.0, center_depth / z_far));

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
        
        // Depth difference (positive if neighbor is closer to camera)
        float depth_diff = center_depth - sample_depth;
        
        // Sharp depth threshold ===
        // Minimum depth difference to consider occlusion (ignores small variations)
        float threshold_min = 0.008;  // Higher = only deep valleys get darkened
        float threshold_max = 0.04;   // Transition range for full occlusion
        
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

    vec3 color = texture(color_texture, tex_coord).rgb;
    frag_color = vec4(color * occlusion, 1.0);
}