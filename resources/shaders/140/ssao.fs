#version 140

/**
 * SSAO (Screen Space Ambient Occlusion) Shader - GLSL 140 version
 * Uses texelFetch for precise pixel access and better performance
 * Requires OpenGL 3.1+ / GLSL 1.40
 */

uniform sampler2D color_texture;   // Original scene color
uniform sampler2D depth_texture;   // Depth buffer texture
uniform float z_near;              // Near clipping plane distance
uniform float z_far;               // Far clipping plane distance

in vec2 tex_coord;                 // Texture coordinates from vertex shader
out vec4 frag_color;               // Final fragment color output

/**
 * Convert linear depth buffer value to world/view space depth
 * Same math as 110 version but with modern syntax
 */
float linearize_depth(float depth)
{
    float z = depth * 2.0 - 1.0;  // Convert to NDC [-1, 1] range
    return (2.0 * z_near * z_far) / (z_far + z_near - z * (z_far - z_near));
}

void main()
{
    // Get exact pixel coordinates using gl_FragCoord (pixel-perfect access)
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    
    // Use texelFetch for direct pixel access without texture filtering
    // Much faster and more precise than texture2D for depth buffers
    float center_depth = linearize_depth(texelFetch(depth_texture, pixel, 0).r);
    
    // Adaptive radius in pixel space (not UV space)
    // int cast ensures exact pixel offsets without floating point errors
    int radius = int(mix(2.0, 5.0, center_depth / z_far));
    
    // Optimized sampling pattern with more samples (12 vs 8)
    // Includes both cardinal directions and diagonals at different distances
    const ivec2 offsets[12] = ivec2[](
        ivec2(1, 0),  ivec2(-1, 0),  ivec2(0, 1),  ivec2(0, -1),  // Cardinal directions (distance 1)
        ivec2(1, 1),  ivec2(-1, 1),  ivec2(1, -1), ivec2(-1, -1), // Diagonals (distance 1.414)
        ivec2(2, 0),  ivec2(-2, 0),  ivec2(0, 2),  ivec2(0, -2)   // Far cardinal (distance 2)
    );
    
    float occlusion = 0.0;
    int valid_samples = 0;
    
    for (int i = 0; i < 12; i++) {
        // Calculate neighbor pixel position with adaptive radius
        ivec2 sample_pixel = pixel + offsets[i] * radius;
        
        // Boundary check to prevent reading outside framebuffer
        // Avoids artifacts at screen edges
        if (sample_pixel.x < 0 || sample_pixel.y < 0) 
            continue;
        
        // Direct pixel fetch - no filtering, exact depth value
        float sample_depth = linearize_depth(texelFetch(depth_texture, sample_pixel, 0).r);
        
        // Depth difference (positive = neighbor is in front)
        float depth_diff = max(0.0, center_depth - sample_depth);
        
        // Adaptive threshold based on distance
        // Distant objects need larger threshold due to depth compression
        float threshold = 0.02 * (0.5 + center_depth / z_far);
        
        // Smoothstep for soft occlusion falloff
        occlusion += smoothstep(0.001, threshold, depth_diff);
        valid_samples++;
    }
    
    // Calculate final AO factor
    if (valid_samples > 0) {
        // Average occlusion and apply intensity (0.5 = subtle effect)
        float ao_factor = 1.0 - (occlusion / float(valid_samples)) * 0.5;
        ao_factor = clamp(ao_factor, 0.58, 1.0);
        
        // Apply power curve for better visual response
        ao_factor = pow(ao_factor, 1.15);
        occlusion = ao_factor;
    } else {
        occlusion = 1.0;  // No samples available, no occlusion
    }
    
    // Sample color using standard filtering (better for colors)
    vec3 color = texture(color_texture, tex_coord).rgb;
    
    // Apply ambient occlusion to final color
    frag_color = vec4(color * occlusion, 1.0);
}