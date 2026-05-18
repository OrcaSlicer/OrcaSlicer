#version 140

/**
 * SSAO Shader - GLSL 140 version with highlight protection
 * Preserves brightness on upward-facing surfaces for better visual quality
 */

uniform sampler2D color_texture;
uniform sampler2D depth_texture;
uniform vec2 inv_tex_size;
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
    float center_depth = linearize_depth(texture(depth_texture, tex_coord).r);

    // Derive a flatness/up proxy from depth gradient (no normal texture required).
    float depth_px = linearize_depth(texture(depth_texture, clamp(tex_coord + vec2(inv_tex_size.x, 0.0), vec2(0.001), vec2(0.999))).r);
    float depth_nx = linearize_depth(texture(depth_texture, clamp(tex_coord - vec2(inv_tex_size.x, 0.0), vec2(0.001), vec2(0.999))).r);
    float depth_py = linearize_depth(texture(depth_texture, clamp(tex_coord + vec2(0.0, inv_tex_size.y), vec2(0.001), vec2(0.999))).r);
    float depth_ny = linearize_depth(texture(depth_texture, clamp(tex_coord - vec2(0.0, inv_tex_size.y), vec2(0.001), vec2(0.999))).r);
    float depth_grad = length(vec2(depth_px - depth_nx, depth_py - depth_ny));
    float up_factor = 1.0 - smoothstep(0.002, 0.03, depth_grad);
    
    // Adaptive radius in pixel space
    float radius = mix(2.0, 5.0, center_depth / z_far);
    
    // Optimized sampling pattern
    const vec2 offsets[12] = vec2[](
        vec2(1.0, 0.0),  vec2(-1.0, 0.0),  vec2(0.0, 1.0),  vec2(0.0, -1.0),
        vec2(1.0, 1.0),  vec2(-1.0, 1.0),  vec2(1.0, -1.0), vec2(-1.0, -1.0),
        vec2(2.0, 0.0),  vec2(-2.0, 0.0),  vec2(0.0, 2.0),  vec2(0.0, -2.0)
    );
    
    float occlusion = 0.0;
    int valid_samples = 0;
    
    for (int i = 0; i < 12; i++) {
        vec2 uv = clamp(tex_coord + offsets[i] * inv_tex_size * radius, vec2(0.001), vec2(0.999));
        float sample_depth = linearize_depth(texture(depth_texture, uv).r);
        
        float depth_diff = max(0.0, center_depth - sample_depth);
        float threshold = 0.02 * (0.5 + center_depth / z_far);
        float contribution = smoothstep(0.001, threshold, depth_diff);
        
        // Reduce contribution on flatter/top-like areas
        float top_factor = 1.0 - up_factor * 0.6;  // 60% less occlusion on tops
        float planar_factor = smoothstep(0.0, 0.02, depth_grad);
        contribution *= (0.7 + planar_factor * 0.3) * top_factor;
        
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
    frag_color = vec4(color * occlusion, 1.0);
}