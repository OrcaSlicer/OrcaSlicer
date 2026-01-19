#version 110

uniform vec4 top_color;
uniform vec4 bottom_color;

varying vec2 tex_coord;

void main()
{
    gl_FragColor = mix(bottom_color, top_color, tex_coord.y);
}
