#version 110

uniform sampler2D color_texture;
uniform sampler2D depth_texture;
uniform vec2 inv_tex_size;
uniform float z_near;
uniform float z_far;

varying vec2 tex_coord;

float linearize_depth(float depth)
{
    float z = depth * 2.0 - 1.0;
    return (2.0 * z_near * z_far) / (z_far + z_near - z * (z_far - z_near));
}

void main()
{
    vec3 base = texture2D(color_texture, tex_coord).rgb;
    float depth_center = linearize_depth(texture2D(depth_texture, tex_coord).r);

    vec2 offsets[8];
    offsets[0] = vec2(-1.0,  0.0);
    offsets[1] = vec2( 1.0,  0.0);
    offsets[2] = vec2( 0.0, -1.0);
    offsets[3] = vec2( 0.0,  1.0);
    offsets[4] = vec2(-1.0, -1.0);
    offsets[5] = vec2( 1.0, -1.0);
    offsets[6] = vec2(-1.0,  1.0);
    offsets[7] = vec2( 1.0,  1.0);

    float occ = 0.0;
    for (int i = 0; i < 8; ++i) {
        vec2 uv = tex_coord + offsets[i] * inv_tex_size * 2.0;
        float sample_depth = linearize_depth(texture2D(depth_texture, uv).r);
        float delta = max(0.0, depth_center - sample_depth);
        occ += smoothstep(0.001, 0.03, delta);
    }

    occ /= 8.0;
    // Lighter AO: preserve non-cavity areas closer to original lighting.
    float ao = 1.0 - occ * 0.45;
    ao = clamp(ao, 0.62, 1.0);

    gl_FragColor = vec4(base * ao, 1.0);
}
