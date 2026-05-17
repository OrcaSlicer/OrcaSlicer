#version 110

/**
 * SSAO (Screen Space Ambient Occlusion) Shader - GLSL 110 version
 * Uses depth-based sampling with adaptive radius and weighting
 * Compatible with OpenGL 2.1 and older hardware
 */

uniform sampler2D color_texture;   // Original scene color
uniform sampler2D depth_texture;   // Depth buffer texture
uniform vec2 inv_tex_size;         // 1.0/width, 1.0/height - for UV offset calculation
uniform float z_near;              // Near clipping plane distance
uniform float z_far;               // Far clipping plane distance

varying vec2 tex_coord;            // Texture coordinates from vertex shader

/**
 * Convert linear depth buffer value to world/view space depth
 * Uses standard OpenGL perspective projection reverse mapping
 */
float linearize_depth(float depth)
{
    float z = depth * 2.0 - 1.0;  // Convert to NDC [-1, 1] range
    return (2.0 * z_near * z_far) / (z_far + z_near - z * (z_far - z_near));
}

void main()
{
    // Sample base color at current fragment
    vec3 base = texture2D(color_texture, tex_coord).rgb;
    
    // Linearize center pixel depth for accurate world-space comparisons
    float depth_center = linearize_depth(texture2D(depth_texture, tex_coord).r);
    
    // Adaptive sampling radius: larger radius for distant objects (perspective effect)
    // Closer objects need smaller radius to capture fine details
    float radius = mix(1.5, 4.0, depth_center / z_far);
    
    // Circular sampling pattern (unit circle) with diagonal weighting
    // Offsets normalized to unit circle for uniform directional sampling
    vec2 offsets[8];
    offsets[0] = vec2( 1.0,  0.0);      // Right
    offsets[1] = vec2( 0.707, 0.707);   // Top-right diagonal
    offsets[2] = vec2( 0.0,  1.0);      // Top
    offsets[3] = vec2(-0.707, 0.707);   // Top-left diagonal
    offsets[4] = vec2(-1.0,  0.0);      // Left
    offsets[5] = vec2(-0.707,-0.707);   // Bottom-left diagonal
    offsets[6] = vec2( 0.0, -1.0);      // Bottom
    offsets[7] = vec2( 0.707,-0.707);   // Bottom-right diagonal
    
    float occlusion = 0.0;
    int valid_samples = 0;
    
    for (int i = 0; i < 8; ++i) {
        // Apply radius and convert to UV space
        vec2 uv = tex_coord + offsets[i] * inv_tex_size * radius;
        
        // Clamp to texture edges to prevent border artifacts
        uv = clamp(uv, vec2(0.001), vec2(0.999));
        
        // Sample and linearize neighbor depth
        float sample_depth = linearize_depth(texture2D(depth_texture, uv).r);
        
        // Calculate depth difference (positive if neighbor is closer to camera)
        float depth_diff = max(0.0, depth_center - sample_depth);
        
        // Adaptive threshold: larger tolerance for distant objects
        // smoothstep creates soft occlusion falloff
        float threshold = 0.015 * (0.5 + depth_center / z_far);
        float contribution = smoothstep(0.001, threshold, depth_diff);
        
        // Weight diagonal samples less (they're further in screen space)
        // Reduces over-occlusion at 45-degree angles
        float diagonal_weight = 1.0 - abs(offsets[i].x * offsets[i].y) * 0.5;
        
        occlusion += contribution * diagonal_weight;
        valid_samples++;
    }
    
    // Average occlusion from all valid samples
    if (valid_samples > 0)
        occlusion /= float(valid_samples);
    
    // Apply AO intensity curve
    // 0.55 intensity factor - subtle effect that preserves original lighting
    float ambient_occlusion = 1.0 - occlusion * 0.55;
    ambient_occlusion = clamp(ambient_occlusion, 0.55, 1.0);
    
    // Gamma-style curve for more natural appearance
    ambient_occlusion = pow(ambient_occlusion, 1.1);
    
    // Final composite: modulate base color with AO factor
    gl_FragColor = vec4(base * ambient_occlusion, 1.0);
}