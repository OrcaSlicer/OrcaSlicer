#version 110

/**
 * SSAO Shader - GLSL 110 version with highlight protection
 * Preserves brightness on upward-facing surfaces (top areas)
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

varying vec2 tex_coord;

// Position of the given pixel in view space. Valid under both an orthographic and a perspective
// camera, unlike the depth linearization it replaces.
vec3 view_pos(vec2 uv)
{
    vec2 c = clamp(uv, vec2(0.0), vec2(1.0));
    float d = texture2D(depth_texture, c).r;
    vec4 ndc = vec4(c * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    vec4 view = inv_projection_matrix * ndc;
    return view.xyz / view.w;
}

// Surface normal at the given pixel, from the forward differences of the reconstructed view
// position. It rings by a pixel across a depth discontinuity, which is acceptable here: the
// normal only weights the occlusion, nothing is shaded with it.
vec3 view_normal(vec2 uv)
{
    vec3 p  = view_pos(uv);
    vec3 px = view_pos(uv + vec2(inv_tex_size.x, 0.0));
    vec3 py = view_pos(uv + vec2(0.0, inv_tex_size.y));
    vec3 n = cross(px - p, py - p);
    float len = length(n);
    return (len > 1e-8) ? n / len : vec3(0.0, 0.0, 1.0);
}

void main()
{
    if (is_outline) {
        gl_FragColor = vec4(texture2D(color_texture, tex_coord).rgb, 1.0);
        return;
    }
    vec3 base = texture2D(color_texture, tex_coord).rgb;

    // Nothing was drawn here: occluding the background would only darken the gradient, and its
    // reconstructed normal is degenerate anyway.
    if (texture2D(depth_texture, tex_coord).r >= 0.9999) {
        gl_FragColor = vec4(base, 1.0);
        return;
    }

    float depth_center = -view_pos(tex_coord).z;
    vec3 normal_center = view_normal(tex_coord);
    
    // Calculate how much the surface faces upward
    // up_factor = 1.0 for surfaces pointing straight up, 0.0 for walls and downward faces
    float up_factor = clamp(dot(normal_center, up_view), 0.0, 1.0);
    
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
        
        float sample_depth = -view_pos(uv).z;
        float depth_diff = max(0.0, depth_center - sample_depth);
        
        // A view-space distance, so it has to scale with how far away the surface is. Held fixed
        // it means a fraction of a millimetre, which every extrusion ridge clears - the term then
        // saturates over the whole print and the AO reads as a flat dimming.
        float threshold = 0.006 * depth_center;
        float contribution = smoothstep(0.0015 * depth_center, threshold, depth_diff);
        
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
    ambient_occlusion = clamp(ambient_occlusion * brightness_boost, 0.45, 1.05);
    
    gl_FragColor = vec4(base * ambient_occlusion, 1.0);
}
