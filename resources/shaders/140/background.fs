#version 140

uniform vec4 top_color;
uniform vec4 bottom_color;

in vec2 tex_coord;

out vec4 out_color;

void main()
{
    out_color = mix(bottom_color, top_color, tex_coord.y);
}
