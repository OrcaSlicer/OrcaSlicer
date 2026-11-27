#version 140

// Copies a depth texture of the target's size into the bound depth buffer, where nearer than what
// it holds under the depth test; texels left at the far plane are skipped.

uniform sampler2D depth_texture;
uniform vec2 map_size;

out vec4 out_color;

void main()
{
    float depth = texture(depth_texture, gl_FragCoord.xy / map_size).r;
    if (depth >= 1.0)
        discard;
    gl_FragDepth = depth;
    out_color = vec4(0.0, 0.0, 0.0, 1.0);
}
