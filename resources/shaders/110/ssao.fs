#version 110

/**
 * SSAO Shader - GLSL 110 version with highlight protection
 * Preserves brightness on upward-facing surfaces (top areas)
 */

uniform sampler2D color_texture;
uniform sampler2D depth_texture;
uniform sampler2D normal_texture;
uniform vec2 inv_tex_size;
uniform float z_near;
uniform float z_far;
uniform bool is_outline;

varying vec2 tex_coord;

float linearize_depth(float depth)
{
    float z = depth * 2.0 - 1.0;
    return (2.0 * z_near * z_far) / (z_far + z_near - z * (z_far - z_near));
}

void main()
{
    if (is_outline) {
        gl_FragColor = vec4(texture2D(color_texture, tex_coord).rgb, 1.0);
        return;
    }
    vec3 base = texture2D(color_texture, tex_coord).rgb;
    float depth_center = linearize_depth(texture2D(depth_texture, tex_coord).r);

    // Sample normal at current fragment (range: -1 to 1)
    vec3 normal_center = texture2D(normal_texture, tex_coord).rgb * 2.0 - 1.0;
    
    // Calculate how much the surface faces upward
    // up_factor = 1.0 for surfaces pointing straight up (0,0,1)
    // up_factor = 0.0 for surfaces pointing down or sideways
    float up_factor = max(0.0, normal_center.z);  // Assuming Z is up axis
    // Alternative: if Y is up, use normal_center.y
    
    // Adaptive sampling radius
    float radius = mix(2.0, 4.0, depth_center / z_far);
    
    vec2 offsets[8];
    offsets[0] = vec2( 1.0,  0.0);
    offsets[1] = vec2( 0.707, 0.707);
    offsets[2] = vec2( 0.0,  1.0);
    offsets[3] = vec2(-0.707, 0.707);
    offsets[4] = vec2(-1.0,  0.0);
    offsets[5] = vec2(-0.707,-0.707);
    offsets[6] = vec2( 0.0, -1.0);
    offsets[7] = vec2( 0.707,-0.707);
    
    float occlusion = 0.0;
    int valid_samples = 0;
    
    for (int i = 0; i < 8; ++i) {
        vec2 uv = tex_coord + offsets[i] * inv_tex_size * radius;
        uv = clamp(uv, vec2(0.001), vec2(0.999));
        
        float sample_depth = linearize_depth(texture2D(depth_texture, uv).r);
        float depth_diff = max(0.0, depth_center - sample_depth);
        
        float threshold = 0.015 * (0.5 + depth_center / z_far);
        float contribution = smoothstep(0.001, threshold, depth_diff);
        
        float diagonal_weight = 1.0 - abs(offsets[i].x * offsets[i].y) * 0.5;
        occlusion += contribution * diagonal_weight;
        valid_samples++;
    }
    
    if (valid_samples > 0)
        occlusion /= float(valid_samples);
    
    // flatter/top-like surfaces get less darkening
    float ao_intensity = 0.55;
    float ambient_occlusion = 1.0 - occlusion * ao_intensity;
    
    // Different min values for top vs bottom surfaces
    float ao_min = mix(0.45, 0.70, up_factor);  // Bottom: 0.45, Top: 0.70
    ambient_occlusion = clamp(ambient_occlusion, ao_min, 1.0);
    
    // Boost brightness on top surfaces (optional)
    float brightness_boost = 1.0 + up_factor * 0.15;  // 15% extra brightness on top
    ambient_occlusion = pow(ambient_occlusion, 2.2) * brightness_boost;
    ambient_occlusion = clamp(ambient_occlusion, 0.45, 1.05);
    
    gl_FragColor = vec4(base * ambient_occlusion, 1.0);
}